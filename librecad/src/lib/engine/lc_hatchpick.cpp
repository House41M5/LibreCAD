/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Hatch by picking a point inside a closed area (AutoCAD HATCH "Pick internal point").
** kai fork (branch kai/thai-fonts-2216), 2026-11-20.
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
**********************************************************************/

// How (a planar graph of what is on screen):
//  1. pieces: every visible line / arc on screen (circles = two half arcs, ellipses and splines = short lines,
//     polylines and blocks = what they hold)
//  2. each piece is cut where another crosses it or where another's end touches it
//  3. the cut parts are the edges, their ends the vertices (ends closer than kTol = one vertex; the same edge drawn
//     twice = one edge); edges with a free end are dropped, again and again (they bound nothing)
//  4. walking every edge both ways with the face on the left (at each vertex take the first edge clockwise from the
//     way back) gives the boundary cycles of all faces: a bounded face's outer cycle runs counter-clockwise
//     (area > 0), the outside of each separate group of edges runs clockwise (area < 0)
//  5. the face of p = the smallest counter-clockwise cycle that holds p; islands = the clockwise outside cycle of
//     every other group of edges that lies inside it
//  6. an edge walked both ways in one cycle is a bridge (a line joining the outer boundary to a shape inside):
//     dropped, which splits the cycle into separate loops

#include "lc_hatchpick.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include <QObject>
#include <QString>

#include "lc_splinepoints.h"
#include "rs_arc.h"
#include "rs_circle.h"
#include "rs_ellipse.h"
#include "rs_entitycontainer.h"
#include "rs_hatch.h"
#include "rs_information.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_pen.h"
#include "rs_vector.h"

namespace {

// Points closer than this are one vertex. Below the contourTolerance (5e-7) of RS_EntityContainer::optimizeContours,
// so the loops given to RS_Hatch always chain (two arcs meeting at a vertex are each <= kTol off it); well above the
// error of computed intersections (~1e-9 at coordinates of 1e6 - DWG site plans).
constexpr double kTol = 2e-7;
constexpr double kTwoPi = 2. * M_PI;
constexpr size_t kMaxPieces = 200000;  // more on screen than this: ask to zoom in

struct Piece {
    bool arc = false;
    RS_Vector a, b;                     // ends
    RS_Vector c;                        // arc: centre
    double r = 0., a1 = 0., sw = 0.;    // arc: radius, start angle, signed sweep (+ = counter-clockwise)
    RS_Vector lo, hi;                   // bounding box
    std::vector<std::pair<double, RS_Vector>> cuts;  // (t along the piece 0..1, point)

    RS_Vector at(double t) const
    {
        if (!arc)
            return a + (b - a) * t;
        return c + RS_Vector::polar(r, a1 + sw * t);
    }

    // t of a point that is on the piece (0..1)
    double param(const RS_Vector& p) const
    {
        double t = 0.;
        if (!arc) {
            const RS_Vector d = b - a;
            const double l2 = d.squared();
            t = l2 > 0. ? RS_Vector::dotP(p - a, d) / l2 : 0.;
        } else {
            const double ang = (p - c).angle();
            const double d = sw >= 0. ? RS_Math::correctAngle(ang - a1) : RS_Math::correctAngle(a1 - ang);
            t = d / std::abs(sw);
            if (t > 1.)  // a hair before the start (d close to 2 pi) or past the end
                t = p.distanceTo(a) < p.distanceTo(b) ? 0. : 1.;
        }
        return std::min(1., std::max(0., t));
    }
};

void setBox(Piece& p)
{
    p.lo = RS_Vector(std::min(p.a.x, p.b.x), std::min(p.a.y, p.b.y));
    p.hi = RS_Vector(std::max(p.a.x, p.b.x), std::max(p.a.y, p.b.y));
    if (!p.arc)
        return;
    for (int k = 0; k < 4; ++k) {  // axis points the sweep passes
        const double ang = k * M_PI_2;
        const double d = p.sw >= 0. ? RS_Math::correctAngle(ang - p.a1) : RS_Math::correctAngle(p.a1 - ang);
        if (d <= std::abs(p.sw)) {
            const RS_Vector q = p.c + RS_Vector::polar(p.r, ang);
            p.lo = RS_Vector(std::min(p.lo.x, q.x), std::min(p.lo.y, q.y));
            p.hi = RS_Vector(std::max(p.hi.x, q.x), std::max(p.hi.y, q.y));
        }
    }
}

double sweepOf(double a1, double a2, bool reversed)
{
    double sw = reversed ? -RS_Math::correctAngle(a1 - a2) : RS_Math::correctAngle(a2 - a1);
    if (std::abs(sw) < 1e-12)  // start = end: the whole turn
        sw = reversed ? -kTwoPi : kTwoPi;
    return sw;
}

struct Collector {
    RS_Vector vMin, vMax;
    std::vector<Piece> pieces;
    std::vector<std::pair<RS_Vector, RS_Vector>> texts;  // boxes of TEXT / MTEXT (islands when inside the area)
    bool tooMany = false;

    bool inView(const RS_Vector& lo, const RS_Vector& hi) const
    {
        return hi.x >= vMin.x - kTol && lo.x <= vMax.x + kTol && hi.y >= vMin.y - kTol && lo.y <= vMax.y + kTol;
    }

    void add(Piece&& p)
    {
        setBox(p);
        if (!inView(p.lo, p.hi))
            return;
        if (pieces.size() >= kMaxPieces) {
            tooMany = true;
            return;
        }
        pieces.push_back(std::move(p));
    }

    void line(const RS_Vector& a, const RS_Vector& b)
    {
        if (!a.valid || !b.valid || a.distanceTo(b) <= kTol)
            return;
        Piece p;
        p.a = a;
        p.b = b;
        add(std::move(p));
    }

    void arc(const RS_Vector& c, double r, double a1, double sw)
    {
        if (r <= kTol || r * std::abs(sw) <= kTol)
            return;
        if (std::abs(sw) > kTwoPi - 1e-9) {  // a whole turn: two halves (an edge from a vertex to itself has no use)
            arc(c, r, a1, sw / 2.);
            arc(c, r, a1 + sw / 2., sw / 2.);
            return;
        }
        Piece p;
        p.arc = true;
        p.c = c;
        p.r = r;
        p.a1 = a1;
        p.sw = sw;
        p.a = p.at(0.);
        p.b = p.at(1.);
        add(std::move(p));
    }

    void polyline(const std::vector<RS_Vector>& pts)
    {
        for (size_t i = 1; i < pts.size(); ++i)
            line(pts[i - 1], pts[i]);
    }

    void walk(RS_EntityContainer* box)
    {
        for (RS_Entity* e : *box) {
            if (tooMany)
                return;
            if (e == nullptr || !e->isVisible() || !inView(e->getMin(), e->getMax()))
                continue;
            switch (e->rtti()) {
            case RS2::EntityLine: {
                const auto* l = static_cast<RS_Line*>(e);
                line(l->getStartpoint(), l->getEndpoint());
                break;
            }
            case RS2::EntityArc: {
                const auto* a = static_cast<RS_Arc*>(e);
                arc(a->getCenter(), a->getRadius(), a->getAngle1(),
                    sweepOf(a->getAngle1(), a->getAngle2(), a->isReversed()));
                break;
            }
            case RS2::EntityCircle: {
                const auto* c = static_cast<RS_Circle*>(e);
                arc(c->getCenter(), c->getRadius(), 0., kTwoPi);
                break;
            }
            case RS2::EntityEllipse: {
                const auto* el = static_cast<RS_Ellipse*>(e);
                double a1 = 0., sw = kTwoPi;
                if (el->isEllipticArc()) {
                    a1 = el->getAngle1();
                    sw = sweepOf(a1, el->getAngle2(), el->isReversed());
                }
                const int n = std::max(8, int(std::ceil(std::abs(sw) / kTwoPi * 128.)));
                std::vector<RS_Vector> pts;
                pts.reserve(n + 1);
                for (int i = 0; i <= n; ++i)
                    pts.push_back(el->getEllipsePoint(a1 + sw * i / n));
                polyline(pts);
                break;
            }
            case RS2::EntitySplinePoints:
                polyline(static_cast<LC_SplinePoints*>(e)->getStrokePoints());
                break;
            case RS2::EntityPolyline:  // lines and arcs
            case RS2::EntitySpline:    // short lines
            case RS2::EntityInsert:    // the block's entities, already placed
                walk(static_cast<RS_EntityContainer*>(e));
                break;
            case RS2::EntityText:   // not a boundary; its box is kept clear (AutoCAD treats text as an island)
            case RS2::EntityMText:
                texts.emplace_back(e->getMin(), e->getMax());
                break;
            default:  // dimensions, hatches, points, solids, images: not boundaries
                break;
            }
        }
    }
};

std::unique_ptr<RS_Entity> tempEntity(const Piece& p)
{
    if (!p.arc)
        return std::make_unique<RS_Line>(nullptr, RS_LineData(p.a, p.b));
    return std::make_unique<RS_Arc>(nullptr, RS_ArcData(p.c, p.r, p.a1, p.a1 + p.sw, p.sw < 0.));
}

// step 2: where each piece is cut
void findCuts(std::vector<Piece>& ps)
{
    std::vector<std::unique_ptr<RS_Entity>> ents;
    ents.reserve(ps.size());
    for (const Piece& p : ps)
        ents.push_back(tempEntity(p));
    std::vector<size_t> order(ps.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&ps](size_t i, size_t j) { return ps[i].lo.x < ps[j].lo.x; });
    for (size_t oi = 0; oi < order.size(); ++oi) {
        Piece& p = ps[order[oi]];
        const RS_Entity* ep = ents[order[oi]].get();
        for (size_t oj = oi + 1; oj < order.size(); ++oj) {
            Piece& q = ps[order[oj]];
            if (q.lo.x > p.hi.x + kTol)
                break;
            if (q.lo.y > p.hi.y + kTol || q.hi.y < p.lo.y - kTol)
                continue;
            const RS_Entity* eq = ents[order[oj]].get();
            // two arcs of one circle (the halves of a donut junction dot): getIntersection gives points that are
            // not on them (the cut landed on the centre - a loop with a gap of one radius, 16-MAYr1 2026-11-20);
            // where they meet is found by the end checks below
            const bool sameCircle = p.arc && q.arc && p.c.distanceTo(q.c) <= kTol && std::abs(p.r - q.r) <= kTol;
            const RS_VectorSolutions sol =
                sameCircle ? RS_VectorSolutions() : RS_Information::getIntersection(ep, eq, true);
            for (size_t k = 0; k < sol.size(); ++k) {
                const RS_Vector v = sol.get(k);
                // only a point on both (a loop is handed on only when its ends meet within 5e-7)
                if (!v.valid || ep->getDistanceToPoint(v, nullptr, RS2::ResolveNone) > kTol
                    || eq->getDistanceToPoint(v, nullptr, RS2::ResolveNone) > kTol)
                    continue;
                p.cuts.emplace_back(p.param(v), v);
                q.cuts.emplace_back(q.param(v), v);
            }
            // an end of one on the other: T joints, and lines that lie along each other (no crossing point)
            for (const RS_Vector& v : {q.a, q.b})
                if (ep->getDistanceToPoint(v, nullptr, RS2::ResolveNone) <= kTol)
                    p.cuts.emplace_back(p.param(v), v);
            for (const RS_Vector& v : {p.a, p.b})
                if (eq->getDistanceToPoint(v, nullptr, RS2::ResolveNone) <= kTol)
                    q.cuts.emplace_back(q.param(v), v);
        }
    }
}

struct Vertices {
    std::vector<RS_Vector> pts;
    std::map<std::pair<long long, long long>, std::vector<int>> grid;

    static long long cell(double v) { return (long long) std::floor(v / (2. * kTol)); }

    int id(const RS_Vector& v)
    {
        const long long cx = cell(v.x), cy = cell(v.y);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto it = grid.find({cx + dx, cy + dy});
                if (it == grid.end())
                    continue;
                for (int k : it->second)
                    if (pts[k].distanceTo(v) <= kTol)
                        return k;
            }
        pts.push_back(v);
        grid[{cx, cy}].push_back(int(pts.size()) - 1);
        return int(pts.size()) - 1;
    }
};

struct Edge {
    int va = -1, vb = -1;
    bool arc = false;
    RS_Vector c;
    double r = 0., sw = 0.;  // arc: centre, radius, signed sweep from va to vb
    bool alive = true;
};

// half-edge h: edge h / 2, from va to vb when h is even
class Graph {
public:
    Vertices V;
    std::vector<Edge> E;

    int from(int h) const { return h & 1 ? E[h >> 1].vb : E[h >> 1].va; }
    int to(int h) const { return h & 1 ? E[h >> 1].va : E[h >> 1].vb; }
    double sweep(int h) const { return h & 1 ? -E[h >> 1].sw : E[h >> 1].sw; }

    RS_Vector mid(const Edge& e) const
    {
        if (!e.arc)
            return (V.pts[e.va] + V.pts[e.vb]) * 0.5;
        return e.c + RS_Vector::polar(e.r, (V.pts[e.va] - e.c).angle() + e.sw / 2.);
    }

    // step 3
    void build(std::vector<Piece>& ps)
    {
        std::map<std::pair<int, int>, std::vector<int>> byEnds;
        for (Piece& p : ps) {
            p.cuts.emplace_back(0., p.a);
            p.cuts.emplace_back(1., p.b);
            std::sort(p.cuts.begin(), p.cuts.end(),
                      [](const std::pair<double, RS_Vector>& x, const std::pair<double, RS_Vector>& y) {
                          return x.first < y.first;
                      });
            int prev = -1;
            double tPrev = 0.;
            for (const auto& cut : p.cuts) {
                const int v = V.id(cut.second);
                if (prev >= 0 && v != prev) {
                    Edge e;
                    e.va = prev;
                    e.vb = v;
                    e.arc = p.arc;
                    e.c = p.c;
                    e.r = p.r;
                    e.sw = p.sw * (cut.first - tPrev);
                    if (!e.arc || std::abs(e.sw) * e.r > kTol)
                        addEdge(e, byEnds);
                }
                if (v != prev) {
                    prev = v;
                    tPrev = cut.first;
                }
            }
        }
    }

    void addEdge(const Edge& e, std::map<std::pair<int, int>, std::vector<int>>& byEnds)
    {
        auto& same = byEnds[{std::min(e.va, e.vb), std::max(e.va, e.vb)}];
        const RS_Vector m = mid(e);
        for (int k : same)  // drawn twice (two lines on top of each other, a line along a polyline side)
            if (E[k].arc == e.arc && mid(E[k]).distanceTo(m) <= 10. * kTol)
                return;
        same.push_back(int(E.size()));
        E.push_back(e);
    }

    // step 3: drop edges with a free end, again and again
    void prune()
    {
        std::vector<int> deg(V.pts.size(), 0);
        std::vector<std::vector<int>> at(V.pts.size());
        for (size_t i = 0; i < E.size(); ++i) {
            ++deg[E[i].va];
            ++deg[E[i].vb];
            at[E[i].va].push_back(int(i));
            at[E[i].vb].push_back(int(i));
        }
        std::vector<int> todo;
        for (size_t v = 0; v < deg.size(); ++v)
            if (deg[v] == 1)
                todo.push_back(int(v));
        while (!todo.empty()) {
            const int v = todo.back();
            todo.pop_back();
            for (int i : at[v]) {
                if (!E[i].alive)
                    continue;
                E[i].alive = false;
                const int w = E[i].va == v ? E[i].vb : E[i].va;
                --deg[v];
                if (--deg[w] == 1)
                    todo.push_back(w);
            }
        }
    }

    // step 4: angle of a half-edge leaving its start; ties (tangent edges) by curvature, left turns last
    void leaving(int h, double* angle, double* curve) const
    {
        const Edge& e = E[h >> 1];
        const RS_Vector& s = V.pts[from(h)];
        if (!e.arc) {
            *angle = RS_Math::correctAngle((V.pts[to(h)] - s).angle());
            *curve = 0.;
            return;
        }
        const double sw = sweep(h);
        *angle = RS_Math::correctAngle((s - e.c).angle() + (sw >= 0. ? M_PI_2 : -M_PI_2));
        *curve = (sw >= 0. ? 1. : -1.) / e.r;
    }

    void order()
    {
        out.assign(V.pts.size(), {});
        pos.assign(2 * E.size(), -1);
        std::vector<double> ang(2 * E.size()), cur(2 * E.size());
        for (size_t i = 0; i < E.size(); ++i) {
            if (!E[i].alive)
                continue;
            for (int h : {int(2 * i), int(2 * i + 1)}) {
                leaving(h, &ang[h], &cur[h]);
                out[from(h)].push_back(h);
            }
        }
        for (auto& list : out) {
            std::sort(list.begin(), list.end(), [&ang, &cur](int x, int y) {
                if (std::abs(ang[x] - ang[y]) > 1e-9)
                    return ang[x] < ang[y];
                return cur[x] < cur[y];
            });
            for (size_t k = 0; k < list.size(); ++k)
                pos[list[k]] = int(k);
        }
    }

    // the half-edge after h on the face to its left: at h's end, the first edge clockwise from the way back
    int next(int h) const
    {
        const auto& list = out[to(h)];
        const int m = int(list.size());
        return list[(pos[h ^ 1] - 1 + m) % m];
    }

    // step 4: all cycles
    void cycles()
    {
        cyc.clear();
        cycleOf.assign(2 * E.size(), -1);
        const int H = int(2 * E.size());
        for (int h = 0; h < H; ++h) {
            if (!E[h >> 1].alive || cycleOf[h] >= 0)
                continue;
            std::vector<int> c;
            int x = h;
            bool ok = true;
            do {
                if (cycleOf[x] >= 0 || int(c.size()) > H) {
                    ok = false;
                    break;
                }
                cycleOf[x] = int(cyc.size());
                c.push_back(x);
                x = next(x);
            } while (x != h);
            cyc.push_back(ok ? c : std::vector<int>());
        }
    }

    // a cycle as points relative to o (arcs in steps of at most 2.5 degrees)
    std::vector<RS_Vector> polygon(const std::vector<int>& c, const RS_Vector& o) const
    {
        std::vector<RS_Vector> pts;
        for (int h : c) {
            const RS_Vector& s = V.pts[from(h)];
            const Edge& e = E[h >> 1];
            if (!e.arc) {
                pts.push_back(s - o);
                continue;
            }
            const double sw = sweep(h);
            const double a0 = (s - e.c).angle();
            const int n = std::max(1, int(std::ceil(std::abs(sw) / (kTwoPi / 144.))));
            for (int i = 0; i < n; ++i)
                pts.push_back(e.c + RS_Vector::polar(e.r, a0 + sw * i / n) - o);
        }
        return pts;
    }

    std::vector<std::vector<int>> out;  // half-edges leaving each vertex, by angle
    std::vector<int> pos;               // place of each half-edge in out[from]
    std::vector<std::vector<int>> cyc;
    std::vector<int> cycleOf;
};

double area(const std::vector<RS_Vector>& pts)
{
    double a = 0.;
    for (size_t i = 0, n = pts.size(); i < n; ++i) {
        const RS_Vector& p = pts[i];
        const RS_Vector& q = pts[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2.;
}

bool inside(const std::vector<RS_Vector>& pts, const RS_Vector& p)
{
    bool in = false;
    for (size_t i = 0, n = pts.size(), j = n - 1; i < n; j = i++) {
        const RS_Vector& a = pts[i];
        const RS_Vector& b = pts[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

using Box = std::pair<RS_Vector, RS_Vector>;  // (min, max)

bool overlap(const Box& a, const Box& b)
{
    return a.first.x <= b.second.x && b.first.x <= a.second.x && a.first.y <= b.second.y && b.first.y <= a.second.y;
}

bool inBox(const RS_Vector& p, const Box& b)
{
    return p.x >= b.first.x && p.x <= b.second.x && p.y >= b.first.y && p.y <= b.second.y;
}

// does the closed polygon pts touch box b (an edge crossing it or a corner of pts in it)?
bool polygonHitsBox(const std::vector<RS_Vector>& pts, const Box& b)
{
    const RS_Vector c[4] = {b.first, RS_Vector(b.second.x, b.first.y), b.second, RS_Vector(b.first.x, b.second.y)};
    auto cross = [](const RS_Vector& o, const RS_Vector& a, const RS_Vector& p) {
        return (a.x - o.x) * (p.y - o.y) - (a.y - o.y) * (p.x - o.x);
    };
    for (size_t i = 0, n = pts.size(); i < n; ++i) {
        const RS_Vector& p = pts[i];
        const RS_Vector& q = pts[(i + 1) % n];
        if (inBox(p, b))
            return true;
        for (int k = 0; k < 4; ++k) {  // proper crossing of segment pq with box side k
            const RS_Vector& u = c[k];
            const RS_Vector& v = c[(k + 1) % 4];
            if (cross(p, q, u) * cross(p, q, v) < 0. && cross(u, v, p) * cross(u, v, q) < 0.)
                return true;
        }
    }
    return false;
}

void addBox(const Box& b, RS_Hatch* hatch)
{
    auto* box = new RS_EntityContainer(hatch);
    box->setPen(RS_Pen(RS2::FlagInvalid));
    const RS_Vector c[4] = {b.first, RS_Vector(b.second.x, b.first.y), b.second, RS_Vector(b.first.x, b.second.y)};
    for (int k = 0; k < 4; ++k) {
        auto* l = new RS_Line(box, RS_LineData(c[k], c[(k + 1) % 4]));
        l->setPen(RS_Pen(RS2::FlagInvalid));
        box->addEntity(l);
    }
    hatch->addEntity(box);
}

int findRoot(std::vector<int>& up, int x)
{
    while (up[x] != x)
        x = up[x] = up[up[x]];
    return x;
}

// step 6: a cycle without its bridges, as closed loops
std::vector<std::vector<int>> loopsOf(const Graph& g, const std::vector<int>& c)
{
    std::map<int, int> uses;
    for (int h : c)
        ++uses[h >> 1];
    std::vector<int> kept;
    for (int h : c)
        if (uses[h >> 1] == 1)
            kept.push_back(h);
    std::multimap<int, size_t> byStart;
    for (size_t i = 0; i < kept.size(); ++i)
        byStart.emplace(g.from(kept[i]), i);
    std::vector<bool> used(kept.size(), false);
    std::vector<std::vector<int>> loops;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (used[i])
            continue;
        std::vector<int> loop;
        size_t j = i;
        for (;;) {
            used[j] = true;
            loop.push_back(kept[j]);
            const int v = g.to(kept[j]);
            if (v == g.from(kept[i]))
                break;
            bool found = false;
            const auto range = byStart.equal_range(v);
            for (auto it = range.first; it != range.second; ++it)
                if (!used[it->second]) {
                    j = it->second;
                    found = true;
                    break;
                }
            if (!found)
                break;
        }
        if (g.to(loop.back()) == g.from(loop.front()))
            loops.push_back(loop);
    }
    return loops;
}

// a loop as hatch boundary entities, or nullptr when its pieces do not meet end to end within 4e-7 (RS_Hatch's
// optimizeContours would reject it: contourTolerance 5e-7)
// (not "emit": a Qt macro once a Qt header is in - the build broke on that name)
RS_EntityContainer* makeLoop(const Graph& g, const std::vector<int>& loop, RS_Hatch* hatch)
{
    auto* box = new RS_EntityContainer(hatch);
    box->setPen(RS_Pen(RS2::FlagInvalid));
    RS_Vector first(0., 0.), last(0., 0.);
    bool started = false;
    bool ok = !loop.empty();
    for (int h : loop) {
        const Edge& e = g.E[h >> 1];
        const RS_Vector& s = g.V.pts[g.from(h)];
        const RS_Vector& t = g.V.pts[g.to(h)];
        RS_Entity* x = nullptr;
        if (!e.arc)
            x = new RS_Line(box, RS_LineData(s, t));
        else
            x = new RS_Arc(box, RS_ArcData(e.c, e.r, (s - e.c).angle(), (t - e.c).angle(), g.sweep(h) < 0.));
        x->setPen(RS_Pen(RS2::FlagInvalid));
        box->addEntity(x);
        // start point = s, end point = t for arcs either way (RS_Arc: start at angle1, end at angle2)
        const RS_Vector xs = x->getStartpoint();
        const RS_Vector xe = x->getEndpoint();
        if (!started) {
            first = xs;
            started = true;
        } else if (xs.distanceTo(last) > 4e-7) {
            ok = false;
        }
        last = xe;
    }
    if (ok && last.distanceTo(first) > 4e-7)
        ok = false;
    if (!ok) {
        delete box;
        return nullptr;
    }
    return box;
}

}  // namespace

bool LC_HatchPick::addLoops(RS_EntityContainer* container, const RS_Vector& p, const RS_Vector& vMin,
                            const RS_Vector& vMax, RS_Hatch* hatch, QString* error, int* islands)
{
    if (islands)
        *islands = 0;
    if (container == nullptr || hatch == nullptr)
        return false;

    Collector col;
    col.vMin = RS_Vector(std::min(vMin.x, vMax.x), std::min(vMin.y, vMax.y));
    col.vMax = RS_Vector(std::max(vMin.x, vMax.x), std::max(vMin.y, vMax.y));
    col.walk(container);
    if (col.tooMany) {
        if (error)
            *error = QObject::tr("Hatch: too many objects on screen (more than %1 pieces) - zoom in to the area")
                         .arg(kMaxPieces);
        return false;
    }

    findCuts(col.pieces);
    Graph g;
    g.build(col.pieces);
    g.prune();
    g.order();
    g.cycles();

    // step 5: the face of p
    const int nc = int(g.cyc.size());
    std::vector<double> areas(nc, 0.);
    std::vector<std::vector<RS_Vector>> polys(nc);
    int face = -1;
    for (int i = 0; i < nc; ++i) {
        if (g.cyc[i].empty())
            continue;
        polys[i] = g.polygon(g.cyc[i], p);
        areas[i] = area(polys[i]);
        if (areas[i] > 0. && inside(polys[i], RS_Vector(0., 0.)) && (face < 0 || areas[i] < areas[face]))
            face = i;
    }
    if (face < 0) {
        if (error)
            *error = QObject::tr("Hatch: no closed boundary around the point. Only what is on screen counts - "
                                 "zoom out if the area is bigger than the view; ends must meet (no gaps).");
        return false;
    }

    // groups of connected edges
    std::vector<int> up(g.V.pts.size());
    std::iota(up.begin(), up.end(), 0);
    for (const Edge& e : g.E)
        if (e.alive)
            up[findRoot(up, e.va)] = findRoot(up, e.vb);
    std::map<int, int> outside;  // group -> its clockwise outside cycle (the most negative area)
    for (int i = 0; i < nc; ++i) {
        if (g.cyc[i].empty() || areas[i] >= 0.)
            continue;
        const int grp = findRoot(up, g.from(g.cyc[i].front()));
        const auto it = outside.find(grp);
        if (it == outside.end() || areas[i] < areas[it->second])
            outside[grp] = i;
    }

    const std::vector<std::vector<int>> faceLoops = loopsOf(g, g.cyc[face]);
    const int faceGroup = findRoot(up, g.from(g.cyc[face].front()));
    std::vector<std::vector<int>> islandLoops;
    std::vector<int> islandCycles;
    for (const auto& go : outside) {
        if (go.first == faceGroup)
            continue;
        const RS_Vector q = g.V.pts[g.from(g.cyc[go.second].front())] - p;
        if (!inside(polys[face], q))
            continue;
        islandCycles.push_back(go.second);
        for (auto& l : loopsOf(g, g.cyc[go.second]))
            islandLoops.push_back(l);
    }

    // text in the area: its box is an island, as AutoCAD keeps the hatch off text. Overlapping boxes as one; only
    // boxes wholly in the area that touch neither its boundary nor an island. First with a margin of 0.4 x the box
    // height left and right, 0.2 x up and down (LibreCAD's text box can be narrower than the letters drawn: the
    // last "1" of "50/51" in 16-MAYr1 was half covered with 5%); a text whose wide box does not fit (text filling
    // a small frame) gets a 5% margin.
    auto boxesOf = [&](double fx, double fy) {
        std::vector<Box> boxes;
        for (Box b : col.texts) {
            if (!b.first.valid || !b.second.valid || b.second.x <= b.first.x || b.second.y <= b.first.y)
                continue;
            const double h = std::min(b.second.x - b.first.x, b.second.y - b.first.y);
            const RS_Vector m(fx * h, fy * h);
            b = Box(b.first - p - m, b.second - p + m);  // relative to p, as polys
            for (bool merged = true; merged;) {
                merged = false;
                for (size_t k = 0; k < boxes.size(); ++k)
                    if (overlap(boxes[k], b)) {
                        b = Box(RS_Vector(std::min(b.first.x, boxes[k].first.x),
                                          std::min(b.first.y, boxes[k].first.y)),
                                RS_Vector(std::max(b.second.x, boxes[k].second.x),
                                          std::max(b.second.y, boxes[k].second.y)));
                        boxes.erase(boxes.begin() + long(k));
                        merged = true;
                        break;
                    }
            }
            boxes.push_back(b);
        }
        return boxes;
    };
    auto fits = [&](const Box& b) {
        if (!inside(polys[face], b.first) || polygonHitsBox(polys[face], b))
            return false;
        for (int ic : islandCycles)
            if (polygonHitsBox(polys[ic], b) || inside(polys[ic], b.first))
                return false;
        return true;
    };
    std::vector<Box> textIslands;  // relative to p until added
    for (const Box& b : boxesOf(0.4, 0.2))
        if (fits(b))
            textIslands.push_back(b);
    for (const Box& b : boxesOf(0.05, 0.05)) {
        bool covered = false;
        for (const Box& t : textIslands)
            if (overlap(t, b)) {
                covered = true;
                break;
            }
        if (!covered && fits(b))
            textIslands.push_back(b);
    }
    for (Box& b : textIslands)
        b = Box(b.first + p, b.second + p);
    // the boundary must close; an island loop that does not is left out (hatched over) rather than failing it all
    std::vector<RS_EntityContainer*> made;
    bool ok = !faceLoops.empty();
    for (const auto& l : faceLoops) {
        RS_EntityContainer* b = ok ? makeLoop(g, l, hatch) : nullptr;
        if (b == nullptr) {
            ok = false;
            break;
        }
        made.push_back(b);
    }
    if (!ok) {
        for (RS_EntityContainer* b : made)
            delete b;
        if (error)
            *error = QObject::tr("Hatch: the boundary around the point could not be closed");
        return false;
    }
    int nIslands = 0;
    for (const auto& l : islandLoops)
        if (RS_EntityContainer* b = makeLoop(g, l, hatch)) {
            made.push_back(b);
            ++nIslands;
        }
    for (RS_EntityContainer* b : made)
        hatch->addEntity(b);
    for (const Box& b : textIslands)
        addBox(b, hatch);
    if (islands)
        *islands = nIslands + int(textIslands.size());
    return true;
}

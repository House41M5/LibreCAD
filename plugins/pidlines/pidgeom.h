/*****************************************************************************/
/*  pidgeom.h - path geometry for the pidlines plugin                        */
/*                                                                           */
/*  No LibreCAD dependency (Qt Core only) so pidgeom_test.cpp can exercise   */
/*  cutting and healing without the GUI.                                     */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*****************************************************************************/

/*
 * Cut: the line is split into pieces with a gap centered on each symbol.
 *   A gap never covers a segment joint (stations are nudged off joints), so it
 *   always lies inside one straight or one arc segment. The gap length may
 *   differ per station (gapAt), e.g. a longer gap on vertical runs.
 * Heal: two piece ends belong together when the straight or same-circle arc
 *   between them has a pid_ symbol exactly at its middle. The pieces carry no
 *   other link, so a gap is never longer than `reach`. Healing ignores piece
 *   direction because LibreCAD arcs from addArc are always counter-clockwise.
 */

#ifndef PIDGEOM_H
#define PIDGEOM_H

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <QList>
#include <QPointF>
#include <QVector>

namespace pidgeom {

const double kPi = 3.14159265358979323846;
const double kTol = 1e-3;     // mm; endpoint/midpoint matching, survives DXF round trips

inline double norm2pi(double a) {
    a = std::fmod(a, 2 * kPi);
    return a < 0 ? a + 2 * kPi : a;
}
inline double dist(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }
inline double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
inline double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
inline QPointF unit(QPointF v) {
    double l = std::hypot(v.x(), v.y());
    return l > 0 ? v / l : QPointF();
}

struct Seg {
    bool arc = false;
    QPointF a, b;                       // line
    QPointF c;                          // arc center
    double r = 0, a0 = 0, sweep = 0;    // arc: start angle, signed sweep (CCW > 0)
    double len = 0;
};

inline Seg lineSeg(QPointF a, QPointF b) {
    Seg s;
    s.a = a; s.b = b;
    s.len = dist(a, b);
    return s;
}

inline Seg arcSeg(QPointF c, double r, double a0, double sweep) {
    Seg s;
    s.arc = true;
    s.c = c; s.r = r; s.a0 = a0; s.sweep = sweep;
    s.len = std::fabs(sweep) * r;
    return s;
}

// polyline vertex p1 -> p2 with bulge b (b = tan(sweep/4), > 0 = CCW)
inline Seg bulgeSeg(QPointF p1, QPointF p2, double b) {
    double dx = p2.x() - p1.x(), dy = p2.y() - p1.y();
    double c = std::hypot(dx, dy);
    if (std::fabs(b) < 1e-9 || c < 1e-12)
        return lineSeg(p1, p2);
    double sweep = 4 * std::atan(b);
    // center sits on the chord's left normal at (c/2)(1-b^2)/(2b), signed by b
    double d = (c / 2) * (1 - b * b) / (2 * b);
    QPointF mid((p1.x() + p2.x()) / 2, (p1.y() + p2.y()) / 2);
    QPointF ctr(mid.x() - dy / c * d, mid.y() + dx / c * d);
    double r = dist(p1, ctr);
    return arcSeg(ctr, r, std::atan2(p1.y() - ctr.y(), p1.x() - ctr.x()), sweep);
}

inline QPointF segPoint(const Seg &s, double t) {
    if (!s.arc) {
        double f = s.len > 0 ? t / s.len : 0;
        return QPointF(s.a.x() + (s.b.x() - s.a.x()) * f, s.a.y() + (s.b.y() - s.a.y()) * f);
    }
    double a = s.a0 + (s.len > 0 ? s.sweep * t / s.len : 0);
    return QPointF(s.c.x() + s.r * std::cos(a), s.c.y() + s.r * std::sin(a));
}

inline double segAngle(const Seg &s, double t) {
    if (!s.arc)
        return std::atan2(s.b.y() - s.a.y(), s.b.x() - s.a.x());
    double a = s.a0 + (s.len > 0 ? s.sweep * t / s.len : 0);
    return a + (s.sweep >= 0 ? kPi / 2 : -kPi / 2);
}

inline QPointF segStart(const Seg &s) { return segPoint(s, 0); }
inline QPointF segEnd(const Seg &s) { return segPoint(s, s.len); }
inline double bulgeOf(const Seg &s) { return s.arc ? std::tan(s.sweep / 4) : 0.0; }

inline Seg reversed(const Seg &s) {
    if (!s.arc) return lineSeg(s.b, s.a);
    return arcSeg(s.c, s.r, s.a0 + s.sweep, -s.sweep);
}

// part of s between distances t0 < t1 along it
inline Seg subSeg(const Seg &s, double t0, double t1) {
    if (!s.arc) return lineSeg(segPoint(s, t0), segPoint(s, t1));
    double f0 = t0 / s.len, f1 = t1 / s.len;
    return arcSeg(s.c, s.r, s.a0 + s.sweep * f0, s.sweep * (f1 - f0));
}

inline double segDist(const Seg &s, QPointF p) {
    if (!s.arc) {
        QPointF d = s.b - s.a;
        double l2 = dot(d, d);
        double f = l2 > 0 ? dot(p - s.a, d) / l2 : 0;
        f = std::min(1.0, std::max(0.0, f));
        return dist(p, s.a + d * f);
    }
    double ang = std::atan2(p.y() - s.c.y(), p.x() - s.c.x());
    double off = norm2pi(s.sweep >= 0 ? ang - s.a0 : s.a0 - ang);
    if (off <= std::fabs(s.sweep) + 1e-12)
        return std::fabs(dist(p, s.c) - s.r);
    return std::min(dist(p, segStart(s)), dist(p, segEnd(s)));
}

struct Path {
    QList<Seg> segs;
    bool closed = false;
    double length() const {
        double l = 0;
        for (const Seg &s: segs) l += s.len;
        return l;
    }
    QPointF start() const { return segStart(segs.first()); }
    QPointF end() const { return segEnd(segs.last()); }
};

inline Path reversed(const Path &p) {
    Path r;
    r.closed = p.closed;
    for (int i = p.segs.size() - 1; i >= 0; --i) r.segs << reversed(p.segs.at(i));
    return r;
}

inline void pathAt(const Path &p, double t, QPointF *pt, double *ang) {
    for (int i = 0; i < p.segs.size(); ++i) {
        const Seg &s = p.segs.at(i);
        if (t <= s.len || i == p.segs.size() - 1) {
            double u = std::min(std::max(t, 0.0), s.len);
            *pt = segPoint(s, u);
            *ang = segAngle(s, u);
            return;
        }
        t -= s.len;
    }
}

inline double pathDist(const Path &p, QPointF pt) {
    double d = 1e300;
    for (const Seg &s: p.segs) d = std::min(d, segDist(s, pt));
    return d;
}

// part of the path between distances t0 < t1
inline QList<Seg> subPath(const Path &p, double t0, double t1) {
    QList<Seg> out;
    double acc = 0;
    for (const Seg &s: p.segs) {
        double s0 = acc, s1 = acc + s.len;
        acc = s1;
        double a = std::max(t0, s0), b = std::min(t1, s1);
        if (b - a > 1e-9) out << subSeg(s, a - s0, b - s0);
    }
    return out;
}

// merge neighbours that are one straight line or one arc, drop zero-length pieces
inline QList<Seg> simplify(const QList<Seg> &in) {
    QList<Seg> out;
    for (const Seg &s: in) {
        if (s.len < 1e-9) continue;
        if (!out.isEmpty()) {
            Seg &l = out.last();
            if (!l.arc && !s.arc) {
                QPointF u1 = unit(l.b - l.a), u2 = unit(s.b - s.a);
                if (std::fabs(cross(u1, u2)) < 1e-6 && dot(u1, u2) > 0 && dist(l.b, s.a) < kTol) {
                    l = lineSeg(l.a, s.b);
                    continue;
                }
            } else if (l.arc && s.arc && dist(l.c, s.c) < kTol && std::fabs(l.r - s.r) < kTol
                       && (l.sweep > 0) == (s.sweep > 0)
                       && std::fabs(l.sweep + s.sweep) <= 2 * kPi + 1e-9
                       && dist(segEnd(l), segStart(s)) < kTol) {
                l = arcSeg(l.c, l.r, l.a0, l.sweep + s.sweep);
                continue;
            }
        }
        out << s;
    }
    return out;
}

// distances along the path where one segment hands over to the next
inline QList<double> joints(const Path &p) {
    QList<double> j;
    double acc = 0;
    for (int i = 0; i + 1 < p.segs.size(); ++i) {
        acc += p.segs.at(i).len;
        j << acc;
    }
    if (p.closed) j << 0.0 << p.length();
    return j;
}

struct Station {
    double t = 0;
    bool cut = false;
    double gap = 0;     // length cut out around t when cut
};

// Symbols centered on the path, `spacing` apart. gapAt(t) is the gap wanted at
// t (0 = no cut there). A station gets cut = true when its gap fits: away from
// joints and path ends and with a real piece left between neighbouring gaps;
// otherwise the symbol sits on the line.
inline QList<Station> planStations(const Path &p, double spacing,
                                   const std::function<double(double)> &gapAt) {
    const double L = p.length();
    int n = std::max(1, int(std::floor(L / spacing + 1e-9)));
    double first = (L - (n - 1) * spacing) / 2;
    QList<Station> st;
    for (int k = 0; k < n; ++k) {
        Station s;
        s.t = first + k * spacing;
        st << s;
    }

    const double margin = 0.01;
    auto minPiece = [](double gap) { return std::max(0.05, 0.25 * gap); };
    const QList<double> J = joints(p);
    auto fits = [&](double t) {
        const double g = gapAt(t), h = g / 2;
        if (g <= 0) return false;
        if (!p.closed && (t - h < minPiece(g) || t + h > L - minPiece(g))) return false;
        if (p.closed && (t - h < margin || t + h > L - margin)) return false;
        for (double j: J)
            if (j > t - h - margin && j < t + h + margin) return false;
        return true;
    };
    for (Station &s: st) {
        const double h = gapAt(s.t) / 2;
        if (h <= 0) continue;
        if (fits(s.t)) { s.cut = true; s.gap = 2 * h; continue; }
        // nudge off the joint that is in the way, the shorter way first; each
        // side of the joint uses the gap wanted on that side
        for (double j: J) {
            if (!(j > s.t - h - margin && j < s.t + h + margin)) continue;
            double c1 = j - gapAt(std::max(0.0, j - 1e-6)) / 2 - 2 * margin;
            double c2 = j + gapAt(std::min(L, j + 1e-6)) / 2 + 2 * margin;
            if (std::fabs(c2 - s.t) < std::fabs(c1 - s.t)) std::swap(c1, c2);
            if (fits(c1)) { s.t = c1; s.cut = true; }
            else if (fits(c2)) { s.t = c2; s.cut = true; }
            if (s.cut) s.gap = gapAt(s.t);
            break;
        }
    }
    // a gap must leave a piece before the next symbol: no symbol may end up
    // sitting in a gap, or healing would lose it
    std::sort(st.begin(), st.end(), [](const Station &x, const Station &y) { return x.t < y.t; });
    auto tooClose = [&](const Station &x, const Station &y, double d) {
        return d < (x.cut ? x.gap / 2 : 0) + (y.cut ? y.gap / 2 : 0)
                + minPiece(std::max(x.cut ? x.gap : 0, y.cut ? y.gap : 0));
    };
    for (int k = 1; k < st.size(); ++k) {
        if ((st[k].cut || st[k - 1].cut) && tooClose(st[k - 1], st[k], st[k].t - st[k - 1].t)) {
            if (st[k].cut) st[k].cut = false;
            else st[k - 1].cut = false;
        }
    }
    if (p.closed && st.size() > 1 && (st.first().cut || st.last().cut)
            && tooClose(st.last(), st.first(), st.first().t + L - st.last().t)) {
        if (st.last().cut) st.last().cut = false;
        else st.first().cut = false;
    }
    return st;
}

// the same gap everywhere
inline QList<Station> planStations(const Path &p, double spacing, double gap) {
    return planStations(p, spacing, std::function<double(double)>([gap](double) { return gap; }));
}

// visible pieces once the cut stations are taken out (each with its own gap)
inline QList<QList<Seg>> pieces(const Path &p, const QList<Station> &st) {
    const double L = p.length();
    QList<Station> cuts;
    for (const Station &s: st)
        if (s.cut) cuts << s;
    std::sort(cuts.begin(), cuts.end(), [](const Station &x, const Station &y) { return x.t < y.t; });
    auto lo = [&](int i) { return cuts[i].t - cuts[i].gap / 2; };
    auto hi = [&](int i) { return cuts[i].t + cuts[i].gap / 2; };
    QList<QList<Seg>> out;
    if (cuts.isEmpty()) {
        out << simplify(p.segs);
        return out;
    }
    const int last = cuts.size() - 1;
    if (p.closed) {
        for (int i = 0; i < last; ++i)
            out << simplify(subPath(p, hi(i), lo(i + 1)));
        // the piece across the start vertex
        out << simplify(subPath(p, hi(last), L) + subPath(p, 0, lo(0)));
    } else {
        out << simplify(subPath(p, 0, lo(0)));
        for (int i = 0; i < last; ++i)
            out << simplify(subPath(p, hi(i), lo(i + 1)));
        out << simplify(subPath(p, hi(last), L));
    }
    for (int i = out.size() - 1; i >= 0; --i)
        if (out.at(i).isEmpty()) out.removeAt(i);
    return out;
}

// terminal segment at end k (0 = start, 1 = end), oriented away from the path
inline Seg outward(const Path &p, int k) {
    return k == 1 ? p.segs.last() : reversed(p.segs.first());
}

inline QPointF endPoint(const Path &p, int k) { return k == 1 ? p.end() : p.start(); }

// The gap between end ea of A and end eb of B, if it is the straight line or
// same-circle arc that continues both pieces and has `ins` at its middle.
inline bool bridge(const Path &A, int ea, const Path &B, int eb, QPointF ins, Seg *br) {
    const Seg ta = outward(A, ea), tb = outward(B, eb);
    const QPointF Ea = endPoint(A, ea), Eb = endPoint(B, eb);
    if (!ta.arc && !tb.arc) {
        QPointF d = Eb - Ea;
        if (std::hypot(d.x(), d.y()) < kTol) return false;
        QPointF da = unit(ta.b - ta.a), db = unit(tb.b - tb.a);
        if (std::fabs(cross(da, d)) > kTol || dot(da, d) <= 0) return false;
        if (std::fabs(cross(db, d)) > kTol || dot(db, d) >= 0) return false;
        if (dist((Ea + Eb) / 2, ins) > kTol) return false;
        *br = lineSeg(Ea, Eb);
        return true;
    }
    if (ta.arc && tb.arc) {
        if (dist(ta.c, tb.c) > kTol || std::fabs(ta.r - tb.r) > kTol) return false;
        if ((ta.sweep > 0) == (tb.sweep > 0)) return false;     // B must continue A's turn
        double angA = std::atan2(Ea.y() - ta.c.y(), Ea.x() - ta.c.x());
        double angB = std::atan2(Eb.y() - ta.c.y(), Eb.x() - ta.c.x());
        double sweep = ta.sweep > 0 ? norm2pi(angB - angA) : -norm2pi(angA - angB);
        Seg s = arcSeg(ta.c, ta.r, angA, sweep);
        if (dist(segPoint(s, s.len / 2), ins) > kTol) return false;
        *br = s;
        return true;
    }
    return false;
}

struct Link {
    int other = -1;     // path index on the other side of the gap
    int otherEnd = 0;
    int ins = -1;       // symbol in the gap
    Seg br;             // from this end to the other end
};
typedef std::array<Link, 2> Links;   // [start, end]

// Link piece ends through the symbols sitting in their gaps. `reach` bounds the
// half gap; closed paths have no ends. Returns, per symbol, whether it linked.
inline QVector<bool> linkPaths(const QList<Path> &paths, const QList<QPointF> &ins,
                               const QList<double> &reach, QVector<Links> *links) {
    links->fill(Links(), paths.size());
    QVector<bool> used(ins.size(), false);
    for (int i = 0; i < ins.size(); ++i) {
        QList<QPair<int, int>> near;    // (path, end)
        for (int p = 0; p < paths.size(); ++p) {
            if (paths.at(p).closed) continue;
            for (int k = 0; k < 2; ++k)
                if ((*links)[p][k].other < 0 && dist(endPoint(paths.at(p), k), ins.at(i)) <= reach.at(i))
                    near << qMakePair(p, k);
        }
        for (int x = 0; x < near.size() && !used[i]; ++x) {
            for (int y = 0; y < near.size() && !used[i]; ++y) {
                if (x == y) continue;
                const int pa = near[x].first, ea = near[x].second, pb = near[y].first, eb = near[y].second;
                Seg br;
                if (!bridge(paths.at(pa), ea, paths.at(pb), eb, ins.at(i), &br)) continue;
                Link &la = (*links)[pa][ea];
                la.other = pb; la.otherEnd = eb; la.ins = i; la.br = br;
                Link &lb = (*links)[pb][eb];
                lb.other = pa; lb.otherEnd = ea; lb.ins = i; lb.br = reversed(br);
                used[i] = true;
            }
        }
    }
    return used;
}

struct Chain {
    QList<int> members;
    QList<int> gapSymbols;
    Path merged;
};

// Follow the links from `start` in both directions and join the pieces and gaps
// into one path in piece order.
inline Chain walkChain(const QList<Path> &paths, const QVector<Links> &links, int start) {
    Chain ch;
    // back up to the head of an open chain; coming back to start means a loop
    int head = start, freeEnd = 0;
    bool loop = false;
    {
        int e = start, out = 0;
        for (int guard = 0; guard <= paths.size(); ++guard) {
            const Link &l = links[e][out];
            if (l.other < 0) { head = e; freeEnd = out; break; }
            if (l.other == start) { loop = true; head = start; freeEnd = 0; break; }
            e = l.other;
            out = 1 - l.otherEnd;
        }
    }
    ch.merged.closed = loop;
    int e = head, in = freeEnd;
    for (int guard = 0; guard <= paths.size(); ++guard) {
        ch.members << e;
        Path pe = in == 0 ? paths.at(e) : reversed(paths.at(e));
        ch.merged.segs += pe.segs;
        const Link &l = links[e][1 - in];
        if (l.other < 0) break;
        ch.merged.segs << l.br;
        ch.gapSymbols << l.ins;
        if (loop && l.other == head) break;
        e = l.other;
        in = l.otherEnd;
    }
    // a closed polyline that was never cut has no links but is still a loop
    if (!loop && ch.members.size() == 1 && paths.at(head).closed)
        ch.merged.closed = true;
    ch.merged.segs = simplify(ch.merged.segs);
    return ch;
}

} // namespace pidgeom

#endif // PIDGEOM_H

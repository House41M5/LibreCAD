/*
 * Context: standalone test of lib/engine/lc_splinefit.h (vp Tolerance mode, 2026-11-02 plans-win
 * librecad-factory-drawings step 22). Build: cl /EHsc /std:c++17 /O2 /I <src>/lib/engine lc_splinefit_test.cpp
 * How: every result is drawn the way RS_Spline draws it (a copy of rbasis/knot/knotu, closed = first
 * `degree` points wrapped) and measured both ways against the input polyline with its own distance code,
 * not the fit's. Corners must lie on the curve, open ends must be the polyline's ends exactly.
 * Every case runs twice: fit() and fitOnPolyline() (step 23), the second also checking that every control
 * point lies on the polyline.
 * Gotchas: the "dense sine 40" cases have corners of their own (chords of a coarse curve), so a small
 * tolerance there needs more points than the polyline - that is expected and printed, not a failure.
 */
#define _USE_MATH_DEFINES
#include "lc_splinefit.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

using lc::splinefit::Vec;

namespace {

int g_fail = 0;
bool g_onPolyline = false; // cases() runs once per mode

lc::splinefit::Result fitAny(const std::vector<Vec>& pts, const std::vector<char>& vertex, bool closed, int degree,
                             double tol) {
    return g_onPolyline ? lc::splinefit::fitOnPolyline(pts, vertex, closed, degree, tol)
                        : lc::splinefit::fit(pts, vertex, closed, degree, tol);
}

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_fail;
        std::printf("    FAIL: %s\n", what.c_str());
    }
}

// --- RS_Spline's drawing, copied (rs_spline.cpp: knot, knotu, rbasis, rbspline, rbsplinu) ---
std::vector<double> knotOpen(size_t num, size_t order) {
    std::vector<double> k(num + order, 0.);
    for (size_t i = order; i <= num; ++i) {
        k[i] = double(i - order + 1);
    }
    for (size_t i = num + 1; i < k.size(); ++i) {
        k[i] = k[num];
    }
    return k;
}

std::vector<double> knotU(size_t num, size_t order) {
    std::vector<double> k(num + order, 0.);
    for (size_t i = 0; i < k.size(); ++i) {
        k[i] = double(i);
    }
    return k;
}

std::vector<double> rbasis(int c, double t, int npts, const std::vector<double>& x) {
    const int nplusc = npts + c;
    std::vector<double> temp(nplusc, 0.);
    for (int i = 0; i < nplusc - 1; i++) {
        if ((t >= x[i]) && (t < x[i + 1])) {
            temp[i] = 1;
        }
    }
    for (int k = 2; k <= c; k++) {
        for (int i = 0; i < nplusc - k; i++) {
            if (temp[i] != 0) {
                temp[i] = ((t - x[i]) * temp[i]) / (x[i + k - 1] - x[i]);
            }
            if (temp[i + 1] != 0) {
                temp[i] += ((x[i + k] - t) * temp[i + 1]) / (x[i + k] - x[i + 1]);
            }
        }
    }
    if (t >= x[nplusc - 1]) {
        temp[npts - 1] = 1;
    }
    double sum = 0.;
    for (int i = 0; i < npts; i++) {
        sum += temp[i];
    }
    std::vector<double> r(npts, 0.);
    if (sum != 0) {
        for (int i = 0; i < npts; i++) {
            r[i] = temp[i] / sum;
        }
    }
    return r;
}

std::vector<Vec> drawSpline(const lc::splinefit::Result& r, size_t perPoint) {
    std::vector<Vec> b = r.control;
    if (r.closed) {
        b.insert(b.end(), r.control.begin(), r.control.begin() + r.degree);
    }
    const size_t npts = b.size();
    const size_t k = size_t(r.degree) + 1;
    const size_t p1 = perPoint * npts;
    std::vector<Vec> out(p1);
    if (r.closed) {
        const auto x = knotU(npts, k);
        double t = double(k - 1);
        const double step = double(npts - k + 1) / double(p1 - 1);
        for (auto& v : out) {
            if (x[npts + k - 1] - t < 5e-6) {
                t = x[npts + k - 1];
            }
            const auto nb = rbasis(int(k), t, int(npts), x);
            for (size_t i = 0; i < npts; ++i) {
                v = v + b[i] * nb[i];
            }
            t += step;
        }
    } else {
        const auto x = knotOpen(npts, k);
        double t = x[0];
        const double step = (x[npts + k - 1] - t) / double(p1 - 1);
        for (auto& v : out) {
            if (x[npts + k - 1] - t < 5e-6) {
                t = x[npts + k - 1];
            }
            const auto nb = rbasis(int(k), t, int(npts), x);
            for (size_t i = 0; i < npts; ++i) {
                v = v + b[i] * nb[i];
            }
            t += step;
        }
    }
    return out;
}

/**
 * The same curve sampled span by span, every knot included (perSpan points per unit span). RS_Spline's own
 * drawing steps evenly over the whole range and so misses the knots, where the corners are: measured on it a
 * corner looks cut by up to half a step.
 */
std::vector<Vec> sampleSpans(const lc::splinefit::Result& r, size_t perSpan) {
    std::vector<Vec> b = r.control;
    if (r.closed) {
        b.insert(b.end(), r.control.begin(), r.control.begin() + r.degree);
    }
    const size_t npts = b.size();
    const size_t k = size_t(r.degree) + 1;
    const auto x = r.closed ? knotU(npts, k) : knotOpen(npts, k);
    const double t0 = r.closed ? double(k - 1) : x[0];
    const double t1 = r.closed ? double(npts) : x[npts + k - 1];
    std::vector<Vec> out;
    const size_t steps = size_t(std::lround(t1 - t0)) * perSpan;
    for (size_t i = 0; i <= steps; ++i) {
        const double t = i == steps ? t1 : t0 + double(i) / double(perSpan);
        const auto nb = rbasis(int(k), t, int(npts), x);
        Vec v;
        for (size_t j = 0; j < npts; ++j) {
            v = v + b[j] * nb[j];
        }
        out.push_back(v);
    }
    return out;
}

// --- distance to a chain of segments, through a grid ---
double segDist(const Vec& p, const Vec& a, const Vec& b) {
    const Vec d = b - a;
    const double l2 = d.dot(d);
    double t = l2 > 0. ? (p - a).dot(d) / l2 : 0.;
    t = t < 0. ? 0. : (t > 1. ? 1. : t);
    return (p - (a + d * t)).norm();
}

class Chain {
public:
    Chain(const std::vector<Vec>& pts, bool closed) : m_p(pts) {
        if (closed) {
            m_p.push_back(pts.front());
        }
        double minX = m_p[0].x, maxX = minX, minY = m_p[0].y, maxY = minY;
        for (const Vec& v : m_p) {
            minX = std::min(minX, v.x);
            maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y);
            maxY = std::max(maxY, v.y);
        }
        m_ox = minX;
        m_oy = minY;
        m_cell = std::max(std::max(maxX - minX, maxY - minY) / 256., 1e-12);
        for (size_t i = 0; i + 1 < m_p.size(); ++i) {
            const long x0 = cx(std::min(m_p[i].x, m_p[i + 1].x)), x1 = cx(std::max(m_p[i].x, m_p[i + 1].x));
            const long y0 = cy(std::min(m_p[i].y, m_p[i + 1].y)), y1 = cy(std::max(m_p[i].y, m_p[i + 1].y));
            for (long gx = x0; gx <= x1; ++gx) {
                for (long gy = y0; gy <= y1; ++gy) {
                    m_grid[key(gx, gy)].push_back(int(i));
                }
            }
        }
    }
    double dist(const Vec& q) const {
        const long gx = cx(q.x), gy = cy(q.y);
        double best = 1e300;
        for (long r = 0; r < 100000; ++r) {
            if (best < 1e299 && (r - 1) * m_cell > best) {
                break;
            }
            for (long x = gx - r; x <= gx + r; ++x) {
                for (long y = gy - r; y <= gy + r; ++y) {
                    if (std::max(std::labs(x - gx), std::labs(y - gy)) != r) {
                        continue;
                    }
                    const auto it = m_grid.find(key(x, y));
                    if (it == m_grid.end()) {
                        continue;
                    }
                    for (int i : it->second) {
                        best = std::min(best, segDist(q, m_p[i], m_p[i + 1]));
                    }
                }
            }
            if (r > 2048 && best < 1e299) {
                break;
            }
        }
        return best;
    }

private:
    long cx(double x) const { return long(std::floor((x - m_ox) / m_cell)); }
    long cy(double y) const { return long(std::floor((y - m_oy) / m_cell)); }
    static long long key(long x, long y) { return (long long)(x) * 1000003LL + y; }
    std::vector<Vec> m_p;
    double m_ox = 0., m_oy = 0., m_cell = 1.;
    std::unordered_map<long long, std::vector<int>> m_grid;
};

struct Case {
    std::string name;
    std::vector<Vec> pts;
    std::vector<char> vertex;
    bool closed = false;
};

/** Fits, draws like RS_Spline, measures both ways; returns the control count (0 = none). */
size_t run(const Case& c, int degree, double tol, long expectCount = -1, bool mustReach = true) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = fitAny(c.pts, c.vertex, c.closed, degree, tol);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %s %-34s p=%d tol=%-6g -> %4zu control (polyline %zu points)", g_onPolyline ? "on " : "lsq",
                c.name.c_str(), degree, tol, r.control.size(), c.pts.size());
    if (r.control.empty()) {
        std::printf("\n");
        check(false, "no control points");
        return 0;
    }
    // measure: curve -> polyline and polyline -> curve
    const auto curve = sampleSpans(r, 64);
    const Chain poly(c.pts, c.closed);
    const Chain cur(curve, false);
    double toPoly = 0.;
    for (const Vec& v : curve) {
        toPoly = std::max(toPoly, poly.dist(v));
    }
    double toCurve = 0.;
    const size_t segs = c.closed ? c.pts.size() : c.pts.size() - 1;
    for (size_t i = 0; i < segs; ++i) {
        const Vec a = c.pts[i];
        const Vec b = c.pts[(i + 1) % c.pts.size()];
        for (int k = 0; k <= 32; ++k) {
            toCurve = std::max(toCurve, cur.dist(a + (b - a) * (k / 32.)));
        }
    }
    const double dev = std::max(toPoly, toCurve);
    // what LibreCAD draws ($SPLINESEGS = 8 lines per control point)
    const auto drawn = drawSpline(r, 8);
    double drawnDev = 0.;
    for (const Vec& v : drawn) {
        drawnDev = std::max(drawnDev, poly.dist(v));
    }
    std::printf(" dev %.3g (fit says %.3g, drawn %.3g) %s %.1f ms\n", dev, r.maxDeviation, drawnDev,
                r.reached ? "reached" : "NOT reached", ms);
    if (mustReach) {
        check(r.reached, "tolerance not reached");
    }
    if (r.reached) {
        check(dev <= tol * 1.001, "measured deviation above the tolerance");
    }
    check(std::abs(dev - r.maxDeviation) <= std::max(0.05 * tol, 0.3 * std::max(dev, r.maxDeviation)) ||
              dev <= r.maxDeviation,
          "fit underestimates its deviation");
    if (expectCount >= 0) {
        check(long(r.control.size()) == expectCount, "control count " + std::to_string(r.control.size()) +
                                                          " != " + std::to_string(expectCount));
    }
    if (!c.closed) {
        check((r.control.front() - c.pts.front()).norm() == 0. && (r.control.back() - c.pts.back()).norm() == 0.,
              "open ends not exactly the polyline's ends");
    } else {
        bool wrapped = int(r.control.size()) >= 2 * degree;
        for (int a = 0; wrapped && a < degree; ++a) {
            wrapped = (r.control[a] - r.control[r.control.size() - degree + a]).norm() == 0.;
        }
        check(!wrapped, "closed control points look wrapped to RS_Spline");
    }
    if (g_onPolyline) {
        double off = 0.;
        for (const Vec& v : r.control) {
            off = std::max(off, poly.dist(v) / (1. + v.norm()));
        }
        check(off <= 1e-12, "control point off the polyline (relative " + std::to_string(off) + ")");
    }
    // corners lie on the curve
    const size_t n = c.pts.size();
    for (size_t i = 0; i < n; ++i) {
        if ((!c.vertex.empty() && !c.vertex[i]) || (!c.closed && (i == 0 || i + 1 == n))) {
            continue;
        }
        const Vec din = c.pts[i] - c.pts[(i + n - 1) % n];
        const Vec dout = c.pts[(i + 1) % n] - c.pts[i];
        // a seam a rounding error long is no corner: the fit merges its ends (prepare's eps), and the "far, seam
        // gap" case turns only 2 pi / 256 there - lsq passed by luck, the closeness limit scaling with 3.4e6
        const double eps = 1e-9 * (1. + c.pts[i].norm());
        if (din.norm() <= eps || dout.norm() <= eps) {
            continue;
        }
        if (std::atan2(std::abs(din.cross(dout)), din.dot(dout)) > lc::splinefit::kCornerAngle + 1e-9) {
            const double dc = cur.dist(c.pts[i]);
            check(dc <= 1e-9 * (1. + c.pts[i].norm()), "corner " + std::to_string(i) + " off the curve by " +
                                                           std::to_string(dc));
        }
    }
    return r.control.size();
}

Case sine(int segs, double len = 100.) {
    Case c;
    c.name = "sine " + std::to_string(segs) + " segs";
    for (int i = 0; i <= segs; ++i) {
        const double x = len * i / segs;
        c.pts.emplace_back(x, 10. * std::sin(x / 10.));
    }
    return c;
}

Case polygon(int sides, double r, Vec center = {0., 0.}, double gap = 0.) {
    Case c;
    c.name = "closed " + std::to_string(sides) + "-gon";
    c.closed = true;
    for (int i = 0; i < sides; ++i) {
        const double a = 2. * M_PI * i / sides;
        c.pts.emplace_back(center.x + r * std::cos(a), center.y + r * std::sin(a));
    }
    if (gap > 0.) {
        c.pts.push_back(c.pts.front() + Vec(gap, 0.)); // a closing point a rounding error away
    }
    return c;
}

void cases() {
    {
        Case c;
        c.name = "straight 2 points";
        c.pts = {{0., 0.}, {100., 50.}};
        run(c, 3, 0.01, 4);
        run(c, 2, 0.01, 3);
        run(c, 1, 0.01, 3);
    }
    {
        Case c;
        c.name = "L 3 segments (2 corners)";
        c.pts = {{0., 0.}, {100., 0.}, {100., 50.}, {0., 50.}};
        run(c, 3, 0.01, 1 + 3 + 3 + 1);
        run(c, 2, 0.01, 1 + 2 + 2 + 1);
        run(c, 1, 0.01, 4);
    }
    {
        Case c;
        c.name = "closed rectangle (4 corners)";
        c.closed = true;
        c.pts = {{0., 0.}, {100., 0.}, {100., 50.}, {0., 50.}};
        run(c, 3, 0.01, 12);
        run(c, 2, 0.01, 8);
    }
    {
        const Case c = sine(400);
        run(c, 3, 0.01);
        run(c, 3, 0.001);
        run(c, 2, 0.01);
        run(c, 1, 0.01);
    }
    {
        const Case c = sine(40);
        run(c, 3, 0.1);
        run(c, 3, 0.05);
        run(c, 3, 0.01);
    }
    {
        const Case c = polygon(256, 50.);
        run(c, 3, 0.01);
        run(c, 2, 0.01);
        run(c, 3, 0.001);
    }
    {
        Case c = polygon(256, 50., {3477900., 7400.}, 1e-9);
        c.name = "closed 256-gon far, seam gap";
        run(c, 3, 0.01);
    }
    {
        // teardrop: one sharp corner, two tangent lines, an arc given as unflagged points
        Case c;
        c.name = "closed teardrop (1 corner, arc)";
        c.closed = true;
        c.pts.emplace_back(-20., 0.);
        c.vertex.push_back(1);
        for (int deg = 120; deg >= -120; deg -= 2) {
            const double a = deg * M_PI / 180.;
            c.pts.emplace_back(10. * std::cos(a), 10. * std::sin(a));
            c.vertex.push_back(char(deg == 120 || deg == -120));
        }
        run(c, 3, 0.01);
        run(c, 2, 0.01);
    }
    {
        // slot: two half circles and two lines, tangent everywhere
        Case c;
        c.name = "closed slot (no corner, arcs)";
        c.closed = true;
        for (int deg = -90; deg <= 90; deg += 3) {
            const double a = deg * M_PI / 180.;
            c.pts.emplace_back(40. + 10. * std::cos(a), 10. * std::sin(a));
            c.vertex.push_back(char(deg == -90 || deg == 90));
        }
        for (int deg = 90; deg <= 270; deg += 3) {
            const double a = deg * M_PI / 180.;
            c.pts.emplace_back(10. * std::cos(a), 10. * std::sin(a));
            c.vertex.push_back(char(deg == 90 || deg == 270));
        }
        run(c, 3, 0.01);
    }
    {
        Case c;
        c.name = "open square (start = end)";
        c.pts = {{0., 0.}, {50., 0.}, {50., 50.}, {0., 50.}, {0., 0.}};
        run(c, 3, 0.01, 1 + 3 + 3 + 3 + 1);
    }
    {
        Case c;
        c.name = "sawtooth 50 teeth (all corners)";
        for (int i = 0; i <= 100; ++i) {
            c.pts.emplace_back(i * 2., (i % 2) * 2.);
        }
        run(c, 3, 0.01, 299); // spans 1 + 98 * 3 + 1 = 296, control = spans + 3
    }
    {
        Case c;
        c.name = "spiral 5000 points";
        for (int i = 0; i < 5000; ++i) {
            const double a = i * 0.01;
            c.pts.emplace_back((10. + 2. * a) * std::cos(a), (10. + 2. * a) * std::sin(a));
        }
        run(c, 3, 0.01);
    }
    {
        // degenerate inputs: no control points, no crash
        std::printf("  degenerate inputs\n");
        const std::vector<char> none;
        check(fitAny({}, none, false, 3, 0.01).control.empty(), "empty");
        check(fitAny({{1., 1.}}, none, false, 3, 0.01).control.empty(), "one point");
        check(fitAny({{1., 1.}, {1., 1.}}, none, false, 3, 0.01).control.empty(), "two equal points");
        check(fitAny({{0., 0.}, {1., 1.}}, none, true, 3, 0.01).control.empty(), "closed 2 points");
        check(fitAny({{0., 0.}, {1., 1.}, {0., 0.}}, none, true, 3, 0.01).control.empty(), "closed back and forth");
        const auto r = fitAny({{0., 0.}, {1., 0.}, {1., 1.}}, none, false, 3, -1.);
        check(!r.control.empty(), "bad tolerance falls back to 0.01");
    }
}

} // namespace

int main() {
    std::printf("lc_splinefit test\n");
    for (bool on : {false, true}) {
        g_onPolyline = on;
        std::printf(on ? "fitOnPolyline\n" : "fit\n");
        cases();
    }
    std::printf(g_fail == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

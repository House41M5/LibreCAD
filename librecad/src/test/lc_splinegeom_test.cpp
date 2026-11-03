// Context: 2026-11-03 plans-win librecad-factory-drawings step 25 - standalone test of lc_splinegeom.h
// (exact RS_Spline geometry: evaluation as RS_Spline::update draws it, chord-bounded lines, nearest point,
// length, knot insertion, pieces, reversal, corners, offset). Build: see
// dxf-tools/rtsuite/splinepts/step25/build_splinegeom_test.bat. Prints PASS/FAIL per check, ALL PASS at the end.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "lc_splinegeom.h"

using lc::splinegeom::Spline;
using lc::splinegeom::Vec;
namespace sg = lc::splinegeom;

static int g_fail = 0;

static void check(bool ok, const char* what, double value = 0.) {
    std::printf("%s %s (%.3g)\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) {
        ++g_fail;
    }
}

// ---- LibreCAD's own drawing code (rs_spline.cpp knot/knotu/rbasis/rbspline/rbsplinu), copied as is ----
static std::vector<double> lcKnot(size_t num, size_t order, const std::vector<double>& knots) {
    if (knots.size() == num + order) {
        return knots;
    }
    std::vector<double> kv(num + order, 0.);
    for (size_t i = order; i < num + 1; ++i) {
        kv[i] = double(i - order + 1);
    }
    for (size_t i = num + 1; i < kv.size(); ++i) {
        kv[i] = kv[num];
    }
    return kv;
}

static std::vector<double> lcKnotu(size_t num, size_t order, const std::vector<double>& knots) {
    if (knots.size() == num + order) {
        return knots;
    }
    std::vector<double> kv(num + order, 0.);
    for (size_t i = 0; i < kv.size(); ++i) {
        kv[i] = double(i);
    }
    return kv;
}

static std::vector<double> rbasis(int c, double t, int npts, const std::vector<double>& x) {
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
    std::vector<double> r(npts, 0);
    if (sum != 0) {
        for (int i = 0; i < npts; i++) {
            r[i] = temp[i] / sum;
        }
    }
    return r;
}

// points as update() makes them, and the parameter of each
static void lcDraw(int degree, bool closed, std::vector<Vec> cp, const std::vector<double>& knots, size_t segs,
                   std::vector<Vec>& out, std::vector<double>& params) {
    if (closed) {
        const std::vector<Vec> head(cp.begin(), cp.begin() + degree);
        cp.insert(cp.end(), head.begin(), head.end());
    }
    const size_t npts = cp.size();
    const size_t k = size_t(degree) + 1;
    const size_t p1 = segs * npts;
    out.assign(p1, Vec());
    params.assign(p1, 0.);
    if (!closed) {
        const auto x = lcKnot(npts, k, knots);
        double t = x[0];
        const double step = (x[npts + k - 1] - t) / double(p1 - 1);
        for (size_t j = 0; j < p1; ++j) {
            if (x[npts + k - 1] - t < 5e-6) {
                t = x[npts + k - 1];
            }
            const auto nb = rbasis(int(k), t, int(npts), x);
            for (size_t i = 0; i < npts; i++) {
                out[j] = out[j] + cp[i] * nb[i];
            }
            params[j] = t;
            t += step;
        }
    } else {
        const auto x = lcKnotu(npts, k, knots);
        double t = double(k - 1);
        const double step = double(npts - k + 1) / double(p1 - 1);
        for (size_t j = 0; j < p1; ++j) {
            if (x[npts + k - 1] - t < 5e-6) {
                t = x[npts + k - 1];
            }
            const auto nb = rbasis(int(k), t, int(npts), x);
            for (size_t i = 0; i < npts; i++) {
                out[j] = out[j] + cp[i] * nb[i];
            }
            params[j] = t;
            t += step;
        }
    }
}
// ---------------------------------------------------------------------------------------------------------

static std::vector<Vec> wave(int n, double ox, double oy) {
    std::vector<Vec> p;
    for (int i = 0; i < n; ++i) {
        p.emplace_back(ox + 10. * i, oy + 20. * std::sin(0.9 * i) + 3. * (i % 3));
    }
    return p;
}

static std::vector<Vec> blob(int n, double ox, double oy, double r) {
    std::vector<Vec> p;
    for (int i = 0; i < n; ++i) {
        const double a = 2. * M_PI * i / n;
        const double rr = r * (1. + 0.25 * std::cos(3. * a));
        p.emplace_back(ox + rr * std::cos(a), oy + rr * std::sin(a));
    }
    return p;
}

static double maxGap(const Spline& a, const Spline& b, double lo, double hi, int steps, double shiftB = 0.) {
    double worst = 0.;
    for (int i = 0; i <= steps; ++i) {
        const double t = lo + (hi - lo) * i / steps;
        worst = std::max(worst, (sg::eval(a, t) - sg::eval(b, t + shiftB)).norm());
    }
    return worst;
}

static void suite(double ox, double oy) {
    std::printf("== origin %.0f, %.0f\n", ox, oy);
    const double rel = 1e-12 * (1. + std::abs(ox) + std::abs(oy));
    struct Case {
        const char* name;
        int degree;
        bool closed;
        std::vector<Vec> cp;
        std::vector<double> knots;
    };
    std::vector<Case> cases;
    for (int deg = 1; deg <= 3; ++deg) {
        cases.push_back({"open uniform", deg, false, wave(9, ox, oy), {}});
        cases.push_back({"closed", deg, true, blob(7, ox, oy, 40.), {}});
    }
    {
        // open, custom non-uniform clamped knots (as from a DWG through dwg2lc)
        std::vector<double> U{0, 0, 0, 0, 0.7, 1.1, 2.9, 3.0, 4.6, 4.6, 4.6, 4.6};
        cases.push_back({"open custom knots", 3, false, wave(8, ox, oy), U});
    }
    for (const Case& c : cases) {
        char buf[160];
        Spline s;
        const bool built = sg::build(c.degree, c.closed, c.cp, c.knots, s);
        std::snprintf(buf, sizeof buf, "build %s deg %d", c.name, c.degree);
        check(built, buf);
        if (!built) {
            continue;
        }
        // 1. same points as LibreCAD's drawing code at the same parameters
        std::vector<Vec> lp;
        std::vector<double> lt;
        lcDraw(c.degree, c.closed, c.cp, c.knots, 8, lp, lt);
        double worst = 0.;
        for (size_t i = 0; i < lp.size(); ++i) {
            worst = std::max(worst, (sg::eval(s, lt[i]) - lp[i]).norm());
        }
        std::snprintf(buf, sizeof buf, "  eval = RS_Spline::update points (%zu)", lp.size());
        check(worst <= 1e-9 * (1. + std::abs(ox) + std::abs(oy)) * 1e-3 + 1e-9, buf, worst);
        // 2. closed: C(t0) = C(t1), tangent continuous at the seam
        if (c.closed) {
            Vec a1, b1;
            const Vec a = sg::eval(s, s.t0(), &a1);
            const Vec b = sg::eval(s, s.t1(), &b1);
            check((a - b).norm() <= rel && (c.degree < 2 || (a1 - b1).norm() <= 1e-9 * (1. + a1.norm())),
                  "  closed: seam continuous", (a - b).norm());
        }
        // 3. chord-bounded lines
        const double eps = sg::defaultChordError(s);
        std::vector<Vec> tp;
        std::vector<double> tt;
        sg::tessellate(s, eps, tp, tt);
        double tw = 0.;
        for (size_t i = 0; i + 1 < tt.size(); ++i) {
            for (int q = 1; q < 8; ++q) {
                const double t = tt[i] + (tt[i + 1] - tt[i]) * q / 8.;
                tw = std::max(tw, lc::splinefit::detail::distToSegment(sg::eval(s, t), tp[i], tp[i + 1]));
            }
        }
        std::snprintf(buf, sizeof buf, "  lines within chord error %.3g (%zu lines)", eps, tp.size() - 1);
        check(tw <= eps * 1.5, buf, tw);
        // 4. nearest point = brute force
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> ux(-30., 120.), uy(-60., 60.);
        std::vector<Vec> dense;
        for (int i = 0; i <= 200000; ++i) {
            dense.push_back(sg::eval(s, s.t0() + (s.t1() - s.t0()) * i / 200000.));
        }
        double nw = 0.;
        for (int i = 0; i < 60; ++i) {
            const Vec q(ox + ux(rng), oy + uy(rng));
            double d = 0.;
            sg::nearestParam(s, tp, tt, q, &d);
            double bd = 1e300;
            for (const Vec& v : dense) {
                bd = std::min(bd, (v - q).norm());
            }
            nw = std::max(nw, d - bd);
        }
        check(nw <= 1e-6, "  nearest point not worse than brute force", nw);
        // 5. length = fine polyline length; paramAtLength inverts it
        // degree 1: the control polygon itself (the dense samples cut its corners)
        const std::vector<Vec>& ref = c.degree == 1 ? s.P : dense;
        double pl = 0.;
        for (size_t i = 1; i < ref.size(); ++i) {
            pl += (ref[i] - ref[i - 1]).norm();
        }
        const double L = sg::length(s);
        std::snprintf(buf, sizeof buf, "  length %.6f vs dense polyline %.6f", L, pl);
        check(std::abs(L - pl) <= 1e-6 * L + (c.degree == 1 ? 1e-9 : 2e-6 * L), buf, L - pl);
        double iw = 0.;
        for (double f : {0.1, 0.37, 0.5, 0.81, 0.999}) {
            const double t = sg::paramAtLength(s, s.t0(), f * L);
            iw = std::max(iw, std::abs(sg::length(s, s.t0(), t) - f * L));
        }
        check(iw <= 1e-9 * L, "  paramAtLength inverts length", iw);
        // 6. knot insertion keeps the shape
        Spline ins = s;
        const double tk = s.t0() + 0.4321 * (s.t1() - s.t0());
        sg::insertKnot(ins, tk, c.degree);
        check(maxGap(s, ins, s.t0(), s.t1(), 4000) <= rel, "  knot insertion keeps the shape",
              maxGap(s, ins, s.t0(), s.t1(), 4000));
        // 7. pieces
        const double a = s.t0() + 0.21 * (s.t1() - s.t0());
        const double b = s.t0() + 0.77 * (s.t1() - s.t0());
        Spline sb;
        const bool okSub = sg::sub(s, a, b, sb);
        check(okSub && maxGap(s, sb, a, b, 4000) <= rel && (sg::eval(sb, sb.t0()) - sg::eval(s, a)).norm() <= rel,
              "  sub(a, b) = the curve on [a, b], clamped", okSub ? maxGap(s, sb, a, b, 4000) : -1.);
        Spline f1, f2;
        bool two = false;
        const double tc = s.t0() + 0.613 * (s.t1() - s.t0());
        const bool okSplit = sg::split(s, tc, f1, f2, two);
        if (!c.closed) {
            check(okSplit && two && maxGap(s, f1, s.t0(), tc, 3000) <= rel && maxGap(s, f2, tc, s.t1(), 3000) <= rel,
                  "  split open = two pieces");
        } else {
            const double T = s.t1() - s.t0();
            double gw = 0.;
            for (int i = 0; i <= 4000; ++i) {
                const double u = tc + T * i / 4000.;
                gw = std::max(gw, (sg::eval(f1, u) - sg::eval(s, sg::wrapParam(s, u))).norm());
            }
            check(okSplit && !two && !f1.periodic && gw <= rel, "  split closed = one open loop from the cut", gw);
            Vec e0 = sg::eval(f1, f1.t0());
            Vec e1 = sg::eval(f1, f1.t1());
            check((e0 - e1).norm() <= rel, "  split closed: ends meet at the cut", (e0 - e1).norm());
        }
        // 8. reversal
        const Spline r = sg::reversed(s);
        double rw = 0.;
        for (int i = 0; i <= 3000; ++i) {
            const double t = s.t0() + (s.t1() - s.t0()) * i / 3000.;
            rw = std::max(rw, (sg::eval(s, t) - sg::eval(r, s.U.front() + s.U.back() - t)).norm());
        }
        check(rw <= rel, "  reversed = same curve backwards", rw);
        // 9. corners: none on a smooth curve (degree >= 2), every inner knot on degree 1
        const auto cs = sg::corners(s);
        if (c.degree >= 2) {
            check(cs.empty(), "  no corners on a smooth curve", double(cs.size()));
        }
    }
}

// offset: distance from the offset spline to the base curve is |d| within tol both ways
static void offsetCheck(const char* name, const Spline& s, double d, bool expectOk) {
    char buf[200];
    const double eps = sg::defaultChordError(s);
    sg::OffsetError why = sg::OffsetError::None;
    Spline o;
    const auto clk = std::chrono::steady_clock::now();
    const bool ok = sg::offsetSpline(s, d, eps, o, &why);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clk).count();
    if (!expectOk) {
        std::snprintf(buf, sizeof buf, "offset %s d=%g refused (radius)", name, d);
        check(!ok && why == sg::OffsetError::Radius, buf);
        return;
    }
    std::snprintf(buf, sizeof buf, "offset %s d=%g: %zu control points from %zu, %.1f ms", name, d, ok ? o.P.size() : 0,
                  s.P.size(), ms);
    check(ok, buf);
    if (!ok) {
        return;
    }
    if (s.periodic) {
        check((sg::eval(o, o.t0()) - sg::eval(o, o.t1())).norm() <= 1e-9, "  closed: the offset ends where it starts");
    }
    std::vector<Vec> pts;
    std::vector<char> vtx;
    std::vector<Vec> tp, op;
    std::vector<double> tt, ot;
    sg::tessellate(s, eps * 0.1, tp, tt);
    sg::tessellate(o, eps * 0.1, op, ot);
    // offset -> base: every offset point at |d| from the base (corners: at most |d| ... miter points further)
    double w1 = 0.;
    for (int i = 0; i <= 3000; ++i) {
        const Vec q = sg::eval(o, o.t0() + (o.t1() - o.t0()) * i / 3000.);
        double dd = 0.;
        sg::nearestParam(s, tp, tt, q, &dd);
        // a mitre point is further than |d| from the corner it stands for; allow it when the nearest base point is a corner
        w1 = std::max(w1, std::abs(dd - std::abs(d)) > 2. * eps ? (dd < std::abs(d) ? std::abs(d) - dd : 0.) : 0.);
    }
    // base -> offset: C(t) + d n(t) lies on the offset within tolerance (away from corners)
    double w2 = 0.;
    const auto cs = sg::corners(s);
    for (int i = 1; i < 3000; ++i) {
        const double t = s.t0() + (s.t1() - s.t0()) * i / 3000.;
        // within 3|d| of a corner the offset is cut (inside) or mitred (outside): not d along the normal
        bool nearCorner = false;
        for (double c : cs) {
            nearCorner = nearCorner || (sg::eval(s, t) - sg::eval(s, c)).norm() < 3. * std::abs(d);
        }
        if (nearCorner) {
            continue;
        }
        const Vec tg = sg::tangentAt(s, t, 1);
        const Vec q = sg::eval(s, t) + Vec(-tg.y, tg.x) * d;
        double dd = 0.;
        sg::nearestParam(o, op, ot, q, &dd);
        if (dd > w2 && dd > 2. * eps) {
            const Vec c = sg::eval(s, t);
            std::snprintf(buf, sizeof buf, "    worst t=%.6f base (%.4f, %.4f) tangent (%.4f, %.4f) want (%.4f, %.4f) gap %.4g", t, c.x,
                          c.y, tg.x, tg.y, q.x, q.y, dd);
        }
        w2 = std::max(w2, dd);
    }
    if (w2 > 2. * eps) {
        std::printf("%s\n", buf);
    }
    if (w2 > 2. * eps) {
        std::printf("    offset polyline (%zu):", pts.size());
        for (size_t i = 0; i < pts.size() && i < 40; ++i) {
            std::printf(" (%.3f,%.3f)%s", pts[i].x, pts[i].y, vtx[i] ? "*" : "");
        }
        std::printf("\n    corners:");
        for (double c : cs) {
            std::printf(" %.4f", c);
        }
        std::printf("  domain [%.4f, %.4f]\n", s.t0(), s.t1());
    }
    std::snprintf(buf, sizeof buf, "  offset never closer than |d| - tol to the base");
    check(w1 <= 2. * eps, buf, w1);
    if (w1 > 2. * eps) {
        // where: the offset points closest to the base
        for (int i = 0; i <= 3000; i += 1) {
            const double t = o.t0() + (o.t1() - o.t0()) * i / 3000.;
            const Vec q = sg::eval(o, t);
            double dd = 0.;
            sg::nearestParam(s, tp, tt, q, &dd);
            if (dd < std::abs(d) - 2. * eps && (i % 20 == 0 || dd < 0.1)) {
                std::printf("    close: t=%.4f (%.4f, %.4f) dist %.4f\n", t, q.x, q.y, dd);
            }
        }
        std::printf("    offset knots:");
        for (double u : o.U) {
            std::printf(" %.3f", u);
        }
        std::printf("\n    offset points:");
        for (const Vec& v : o.P) {
            std::printf(" (%.3f,%.3f)", v.x, v.y);
        }
        std::printf("\n");
    }
    std::snprintf(buf, sizeof buf, "  base + d*normal on the offset within tol %.3g", eps);
    check(w2 <= 2. * eps, buf, w2);
}

int main() {
    suite(0., 0.);
    suite(3477760., 7400.);

    std::printf("== offset\n");
    {
        Spline s;
        sg::build(3, false, wave(9, 0., 0.), {}, s);
        offsetCheck("open wave", s, 2.0, true);
        offsetCheck("open wave", s, -2.0, true);
        offsetCheck("open wave", s, 25.0, false);
    }
    {
        Spline s;
        sg::build(3, true, blob(9, 0., 0., 40.), {}, s);
        offsetCheck("closed blob", s, 3.0, true);
        offsetCheck("closed blob", s, -3.0, true);
    }
    {
        // vp-like spline with corners: fit a rectangle with rounded-free corners and a bump
        std::vector<Vec> poly{{0, 0}, {60, 0}, {60, 30}, {30, 45}, {0, 30}};
        const auto fr = lc::splinefit::fit(poly, {}, true, 3, 0.01);
        Spline s;
        const bool b = sg::build(3, true, fr.control, {}, s);
        check(b, "build vp pentagon (corner blocks)");
        if (b) {
            check(sg::corners(s).size() == 5, "  5 corners found", double(sg::corners(s).size()));
            offsetCheck("pentagon", s, 2.0, true);
            offsetCheck("pentagon", s, -2.0, true);
        }
        // the same far from the origin (GUI test t25 at 3.48e6: corner directions from chords were noise)
        std::vector<Vec> farPoly;
        for (const Vec& v : poly) {
            farPoly.emplace_back(v.x + 3477760., v.y + 7400.);
        }
        const auto ff = lc::splinefit::fit(farPoly, {}, true, 3, 0.01);
        Spline sf;
        if (sg::build(3, true, ff.control, {}, sf)) {
            check(sg::corners(sf).size() == 5, "far pentagon: 5 corners", double(sg::corners(sf).size()));
            offsetCheck("far pentagon", sf, 2.0, true);
            offsetCheck("far pentagon", sf, -2.0, true);
        }
        // corner blocks written by hand, as make_t25.py does (triple points, closed, uniform)
        std::vector<Vec> cb;
        for (const Vec& v : farPoly) {
            cb.insert(cb.end(), {v, v, v});
        }
        Spline sb;
        if (sg::build(3, true, cb, {}, sb)) {
            check(sg::corners(sb).size() == 5, "far triple-point pentagon: 5 corners", double(sg::corners(sb).size()));
            offsetCheck("far triple-point pentagon", sb, 2.0, true);
            offsetCheck("far triple-point pentagon", sb, -2.0, true);
        }
        // a loop with one corner (teardrop: a triple point, the rest smooth) meets itself at the join
        {
            std::vector<Vec> tear{{0, 0}, {0, 0}, {0, 0}, {30, 10}, {45, 30}, {30, 50}, {10, 40}};
            Spline st;
            if (sg::build(3, true, tear, {}, st)) {
                check(sg::corners(st).size() == 1, "teardrop: 1 corner", double(sg::corners(st).size()));
                offsetCheck("teardrop", st, 1.5, true);
                offsetCheck("teardrop", st, -1.5, true);
            }
        }
        std::vector<Vec> L{{0, 0}, {50, 0}, {50, 20}, {80, 40}};
        const auto fl = lc::splinefit::fit(L, {}, false, 3, 0.01);
        Spline sl;
        if (sg::build(3, false, fl.control, {}, sl)) {
            check(sg::corners(sl).size() == 2, "open L: 2 corners", double(sg::corners(sl).size()));
            offsetCheck("open L", sl, 1.5, true);
            offsetCheck("open L", sl, -1.5, true);
        }
    }
    {
        // degree 1: the offset is the mitred polyline
        std::vector<Vec> cp{{0, 0}, {10, 0}, {10, 10}};
        Spline s;
        sg::build(1, false, cp, {}, s);
        std::vector<Vec> pts;
        std::vector<char> vtx;
        const bool ok = sg::offsetPolyline(s, 1., sg::defaultChordError(s), pts, vtx);
        // left of travel: (0,1) (9,1) (9,10)
        bool exact = ok && pts.size() == 3 && (pts[0] - Vec(0, 1)).norm() < 1e-12 && (pts[1] - Vec(9, 1)).norm() < 1e-12 &&
                     (pts[2] - Vec(9, 10)).norm() < 1e-12;
        check(exact, "degree 1 offset = mitred polyline (inside corner cut)", double(pts.size()));
        const bool ok2 = sg::offsetPolyline(s, -1., sg::defaultChordError(s), pts, vtx);
        exact = ok2 && pts.size() == 3 && (pts[0] - Vec(0, -1)).norm() < 1e-12 && (pts[1] - Vec(11, -1)).norm() < 1e-12 &&
                (pts[2] - Vec(11, 10)).norm() < 1e-12;
        check(exact, "degree 1 offset = mitred polyline (outside mitre)", double(pts.size()));
    }
    {
        // timing: the offset action recomputes its preview on every mouse move
        for (int n : {6, 40, 200}) {
            std::vector<Vec> cp;
            for (int i = 0; i < n; ++i) {
                cp.emplace_back(3477760. + 100. * i / n, 7400. + 20. * std::sin(0.31 * i) + 5. * std::cos(1.7 * i));
            }
            Spline s;
            sg::build(3, false, cp, {}, s);
            const auto t0 = std::chrono::steady_clock::now();
            const double eps = sg::defaultChordError(s);
            Spline o;
            sg::OffsetError why = sg::OffsetError::None;
            const bool ok = sg::offsetSpline(s, 0.5, eps, o, &why);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("TIME offsetSpline %d control points: %.1f ms -> %zu points, ok %d%s\n", n, ms, ok ? o.P.size() : 0,
                        int(ok), why == sg::OffsetError::Radius ? " (refused: radius)" : "");
        }
    }
    std::printf(g_fail == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

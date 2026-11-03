/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
**
**********************************************************************/
/*
 * Context: 2026-11-03 (plans-win librecad-factory-drawings step 25, user: make splines fully usable in the
 * factory). RS_Spline (what vp, the DXF/DWG reader and Draw > Spline make) is an RS_EntityContainer of short
 * lines, $SPLINESEGS per control point spread evenly over the parameter. Drawing, snapping, length, trim
 * limits and intersections all used those lines, so a spline with few control points (vp Tolerance) was
 * off its own curve by more than the fit tolerance (0.016 on a 5-point spline at 0.01), and offset, trim,
 * cut, lengthen and stretch did not take an RS_Spline at all.
 * How: the exact B-spline as RS_Spline::update() defines it (build()), evaluated with de Boor's basis
 * (lc_splinefit.h dersBasis); lines bounded by a chord error (tessellate); nearest point by Newton from the
 * nearest line; length by Gauss-Legendre; pieces by knot insertion (sub, concat); offset as a polyline of
 * offset samples joined at corners, which lc::splinefit::fit turns back into a spline within a tolerance.
 * Gotchas:
 *  - build() must follow RS_Spline exactly: a closed spline wraps its first `degree` points unless already
 *    wrapped, the file's knots are used only when the count fits, else uniform - clamped (open) or 0,1,2,..
 *    (closed). The curve lives on [U[p], U[n]] in both cases.
 *  - Knot insertion at the right end of a closed (unclamped) curve needs the span past U[n]; insertKnot
 *    takes the last knot index <= t over the whole vector, which is right for both ends.
 *  - A corner block (vp keeps `degree` equal points at a corner) has C' = 0 at the knot: directions come
 *    from point differences there, not from the derivative.
 *  - No Qt/LibreCAD types, so test/lc_splinegeom_test.cpp builds it alone (see lc_splinefit.h).
 */
#ifndef LC_SPLINEGEOM_H
#define LC_SPLINEGEOM_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "lc_splinefit.h"

namespace lc {
namespace splinegeom {

using Vec = splinefit::Vec;

/** A B-spline curve: n control points, n + p + 1 knots, defined on [U[p], U[n]]. */
struct Spline {
    int p = 3;
    std::vector<double> U;
    std::vector<Vec> P;
    bool periodic = false; ///< closed as RS_Spline draws it (first p points repeated at the end)

    int n() const { return int(P.size()); }
    double t0() const { return U[std::size_t(p)]; }
    double t1() const { return U[P.size()]; }
};

inline bool samePoint(const Vec& a, const Vec& b) { return a.x == b.x && a.y == b.y; }

/** The curve RS_Spline::update() draws for these data (see the Gotchas). */
inline bool build(int degree, bool closed, std::vector<Vec> cp, const std::vector<double>& knots, Spline& s) {
    if (degree < 1 || degree > 3) {
        return false;
    }
    const std::size_t p = std::size_t(degree);
    if ((!closed && cp.size() < p + 1) || cp.size() < 3) {
        return false;
    }
    if (closed) {
        const bool wrapped = cp.size() >= 2 * p && std::equal(cp.begin(), cp.begin() + long(p), cp.end() - long(p), samePoint);
        if (!wrapped) {
            const std::vector<Vec> head(cp.begin(), cp.begin() + long(p));
            cp.insert(cp.end(), head.begin(), head.end());
        }
    }
    const std::size_t n = cp.size();
    s.p = degree;
    s.periodic = closed;
    s.P = std::move(cp);
    if (knots.size() == n + p + 1) {
        s.U = knots;
    } else {
        s.U.assign(n + p + 1, 0.);
        for (std::size_t i = 0; i < s.U.size(); ++i) {
            s.U[i] = closed ? double(i) : double(std::clamp<long>(long(i) - long(p), 0, long(n - p)));
        }
    }
    for (std::size_t i = 1; i < s.U.size(); ++i) {
        if (!(s.U[i] >= s.U[i - 1])) {
            return false;
        }
    }
    return s.t1() > s.t0();
}

/** The span k in [p, n-1] (U[k] < U[k+1]) that holds t; the ends go to the first/last non-empty span. */
inline int findSpan(const Spline& s, double t) {
    const int n = s.n();
    const int p = s.p;
    if (t >= s.U[std::size_t(n)]) {
        int k = n - 1;
        while (k > p && !(s.U[std::size_t(k)] < s.U[std::size_t(k) + 1])) {
            --k;
        }
        return k;
    }
    if (t <= s.U[std::size_t(p)]) {
        int k = p;
        while (k < n - 1 && !(s.U[std::size_t(k)] < s.U[std::size_t(k) + 1])) {
            ++k;
        }
        return k;
    }
    const auto it = std::upper_bound(s.U.begin() + p, s.U.begin() + n + 1, t);
    return int(it - s.U.begin()) - 1;
}

/** The polynomial of span k at t (t may sit on either end of the span: left/right limits at knots). */
inline Vec evalSpan(const Spline& s, int k, double t, Vec* d1 = nullptr, Vec* d2 = nullptr) {
    double ders[3][4];
    const int nd = d2 != nullptr ? 2 : (d1 != nullptr ? 1 : 0);
    splinefit::detail::dersBasis(s.U, k, t, s.p, nd, ders);
    Vec c, a, b;
    for (int j = 0; j <= s.p; ++j) {
        const Vec& q = s.P[std::size_t(k - s.p + j)];
        c = c + q * ders[0][j];
        a = a + q * ders[1][j];
        b = b + q * ders[2][j];
    }
    if (d1 != nullptr) {
        *d1 = a;
    }
    if (d2 != nullptr) {
        *d2 = b;
    }
    return c;
}

inline Vec eval(const Spline& s, double t, Vec* d1 = nullptr, Vec* d2 = nullptr) {
    t = std::clamp(t, s.t0(), s.t1());
    return evalSpan(s, findSpan(s, t), t, d1, d2);
}

/** Closed: t brought into [t0, t1); open: clamped. */
inline double wrapParam(const Spline& s, double t) {
    if (!s.periodic) {
        return std::clamp(t, s.t0(), s.t1());
    }
    const double T = s.t1() - s.t0();
    double r = std::fmod(t - s.t0(), T);
    if (r < 0.) {
        r += T;
    }
    if (r >= T) {
        r = 0.;
    }
    return s.t0() + r;
}

/** Diagonal of the control points' box and their largest coordinate. */
inline void extent(const Spline& s, double& diag, double& maxAbs) {
    double x0 = std::numeric_limits<double>::max();
    double y0 = x0;
    double x1 = -x0;
    double y1 = -x0;
    maxAbs = 0.;
    for (const Vec& q : s.P) {
        x0 = std::min(x0, q.x);
        x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y);
        y1 = std::max(y1, q.y);
        maxAbs = std::max({maxAbs, std::abs(q.x), std::abs(q.y)});
    }
    diag = s.P.empty() ? 0. : std::hypot(x1 - x0, y1 - y0);
}

/** Chord error for the lines LibreCAD draws and measures on: 1e-5 of the size (100 mm -> 0.001 mm). */
inline double defaultChordError(const Spline& s) {
    double diag = 0.;
    double maxAbs = 0.;
    extent(s, diag, maxAbs);
    return std::max(1e-5 * diag, 1e-12 * (1. + maxAbs));
}

namespace detail {

inline void subdivide(const Spline& s, int k, double ta, double tb, const Vec& pa, const Vec& pb, double eps, int depth,
                      std::size_t maxPoints, std::vector<Vec>& pts, std::vector<double>& ts) {
    const double tm = 0.5 * (ta + tb);
    const Vec pm = evalSpan(s, k, tm);
    if (depth < 10 && pts.size() < maxPoints && splinefit::detail::distToSegment(pm, pa, pb) > eps) {
        subdivide(s, k, ta, tm, pa, pm, eps, depth + 1, maxPoints, pts, ts);
        subdivide(s, k, tm, tb, pm, pb, eps, depth + 1, maxPoints, pts, ts);
        return;
    }
    pts.push_back(pb);
    ts.push_back(tb);
}

} // namespace detail

/**
 * Points along the curve from t0 to t1 with every chord within eps of the curve (checked at the chord's
 * middle parameter, at least 4 chords per span), and their parameters. A closed curve ends on its start.
 */
inline void tessellate(const Spline& s, double eps, std::vector<Vec>& pts, std::vector<double>& ts,
                       std::size_t maxPoints = 200000) {
    pts.clear();
    ts.clear();
    const int n = s.n();
    pts.push_back(evalSpan(s, findSpan(s, s.t0()), s.t0()));
    ts.push_back(s.t0());
    for (int k = s.p; k < n; ++k) {
        const double a = s.U[std::size_t(k)];
        const double b = s.U[std::size_t(k) + 1];
        if (!(b > a)) {
            continue;
        }
        double ta = a;
        Vec pa = evalSpan(s, k, a);
        for (int q = 1; q <= 4; ++q) {
            const double tb = q == 4 ? b : a + (b - a) * q / 4.;
            const Vec pb = evalSpan(s, k, tb);
            detail::subdivide(s, k, ta, tb, pa, pb, eps, 0, maxPoints, pts, ts);
            ta = tb;
            pa = pb;
        }
    }
}

namespace detail {

/** Newton on (C(t) - q).C'(t) = 0 inside [lo, hi]. */
inline double project(const Spline& s, const Vec& q, double t, double lo, double hi) {
    for (int it = 0; it < 40; ++it) {
        Vec d1, d2;
        const Vec c = eval(s, t, &d1, &d2);
        const Vec r = c - q;
        const double f = r.dot(d1);
        const double g = d1.dot(d1) + r.dot(d2);
        const double gg = d1.dot(d1);
        double dt = 0.;
        if (g > 0.) {
            dt = -f / g;
        } else if (gg > 0.) {
            dt = -f / gg;
        } else {
            break;
        }
        const double tn = std::clamp(t + dt, lo, hi);
        if (std::abs(tn - t) <= 1e-15 * (1. + std::abs(t))) {
            t = tn;
            break;
        }
        t = tn;
    }
    return t;
}

} // namespace detail

/**
 * Parameter of the point of the curve nearest q. `pts`/`ts` are a tessellation of s (seeds); Newton
 * refines from the nearest chord and its neighbours. *dist gets the distance.
 */
inline double nearestParam(const Spline& s, const std::vector<Vec>& pts, const std::vector<double>& ts, const Vec& q,
                           double* dist = nullptr) {
    double bestT = s.t0();
    double bestD = std::numeric_limits<double>::max();
    if (pts.size() < 2) {
        const double d = (eval(s, bestT) - q).norm();
        if (dist != nullptr) {
            *dist = d;
        }
        return bestT;
    }
    std::size_t bi = 0;
    double bw = 0.;
    double bd = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const Vec ab = pts[i + 1] - pts[i];
        const double l2 = ab.dot(ab);
        const double w = l2 > 0. ? std::clamp((q - pts[i]).dot(ab) / l2, 0., 1.) : 0.;
        const double d = (pts[i] + ab * w - q).norm();
        if (d < bd) {
            bd = d;
            bi = i;
            bw = w;
        }
    }
    const std::size_t m = pts.size() - 1;
    const double lo = ts[bi > 0 ? bi - 1 : 0];
    const double hi = ts[std::min(bi + 2, m)];
    const double seed = ts[bi] + bw * (ts[bi + 1] - ts[bi]);
    for (double t : {seed, ts[bi], ts[bi + 1]}) {
        // the seed itself counts too: Newton can leave a kink (degree 1, corner) for a worse point
        for (double tr : {t, detail::project(s, q, t, lo, hi)}) {
            const double d = (eval(s, tr) - q).norm();
            if (d < bestD) {
                bestD = d;
                bestT = tr;
            }
        }
    }
    if (!s.periodic) {
        for (double t : {s.t0(), s.t1()}) {
            const double d = (eval(s, t) - q).norm();
            if (d < bestD) {
                bestD = d;
                bestT = t;
            }
        }
    }
    if (dist != nullptr) {
        *dist = bestD;
    }
    return bestT;
}

namespace detail {

/** Gauss-Legendre, 10 points, of |C'| over [a, b] inside span k. */
inline double speedIntegral(const Spline& s, int k, double a, double b) {
    static const double x[5] = {0.1488743389816312, 0.4333953941292472, 0.6794095682990244, 0.8650633666889845,
                                0.9739065285171717};
    static const double w[5] = {0.2955242247147529, 0.2692667193099963, 0.2190863625159820, 0.1494513491505806,
                                0.0666713443086881};
    const double h = 0.5 * (b - a);
    const double c = 0.5 * (a + b);
    double sum = 0.;
    for (int i = 0; i < 5; ++i) {
        for (double sg : {-1., 1.}) {
            Vec d1;
            evalSpan(s, k, c + sg * h * x[i], &d1);
            sum += w[i] * d1.norm();
        }
    }
    return sum * h;
}

/** Length over [a, b] within span k, in 4 pieces. */
inline double spanLength(const Spline& s, int k, double a, double b) {
    double sum = 0.;
    for (int q = 0; q < 4; ++q) {
        sum += speedIntegral(s, k, a + (b - a) * q / 4., a + (b - a) * (q + 1) / 4.);
    }
    return sum;
}

} // namespace detail

/** Arc length from a to b (a <= b, both in [t0, t1]). */
inline double length(const Spline& s, double a, double b) {
    if (!(b > a)) {
        return 0.;
    }
    double sum = 0.;
    for (int k = s.p; k < s.n(); ++k) {
        const double ua = std::max(a, s.U[std::size_t(k)]);
        const double ub = std::min(b, s.U[std::size_t(k) + 1]);
        if (ub > ua) {
            sum += detail::spanLength(s, k, ua, ub);
        }
    }
    return sum;
}

inline double length(const Spline& s) { return length(s, s.t0(), s.t1()); }

/** The parameter t >= a with length(a, t) = L (clamped to t1). */
inline double paramAtLength(const Spline& s, double a, double L) {
    if (L <= 0.) {
        return a;
    }
    double acc = 0.;
    for (int k = findSpan(s, a); k < s.n(); ++k) {
        const double ua = std::max(a, s.U[std::size_t(k)]);
        const double ub = s.U[std::size_t(k) + 1];
        if (!(ub > ua)) {
            continue;
        }
        const double l = detail::spanLength(s, k, ua, ub);
        if (acc + l < L) {
            acc += l;
            continue;
        }
        // inside this span: bisection guarded Newton on spanLength(ua, t) = L - acc
        const double want = L - acc;
        double lo = ua;
        double hi = ub;
        double t = ua + (ub - ua) * (want / std::max(l, 1e-300));
        for (int it = 0; it < 60; ++it) {
            const double g = detail::spanLength(s, k, ua, t) - want;
            if (std::abs(g) <= 1e-13 * (1. + l)) {
                return t;
            }
            if (g > 0.) {
                hi = t;
            } else {
                lo = t;
            }
            Vec d1;
            evalSpan(s, k, t, &d1);
            const double sp = d1.norm();
            double tn = sp > 0. ? t - g / sp : 0.5 * (lo + hi);
            if (!(tn > lo && tn < hi)) {
                tn = 0.5 * (lo + hi);
            }
            if (std::abs(tn - t) <= 1e-15 * (1. + std::abs(t))) {
                return tn;
            }
            t = tn;
        }
        return t;
    }
    return s.t1();
}

/** Number of knots equal to t. */
inline int multiplicity(const Spline& s, double t) {
    return int(std::count(s.U.begin(), s.U.end(), t));
}

/** Boehm knot insertion (The NURBS Book A5.1): t inserted r times, never past multiplicity p. */
inline void insertKnot(Spline& s, double t, int r) {
    const int p = s.p;
    const int mult = multiplicity(s, t);
    r = std::min(r, p - mult);
    if (r <= 0) {
        return;
    }
    const int k = int(std::upper_bound(s.U.begin(), s.U.end(), t) - s.U.begin()) - 1;
    const int np = s.n() - 1;
    const int mp = np + p + 1;
    if (k < p || k > mp - 1) {
        return;
    }
    std::vector<double> UQ(s.U.size() + std::size_t(r));
    std::vector<Vec> Q(s.P.size() + std::size_t(r));
    for (int i = 0; i <= k; ++i) {
        UQ[std::size_t(i)] = s.U[std::size_t(i)];
    }
    for (int i = 1; i <= r; ++i) {
        UQ[std::size_t(k + i)] = t;
    }
    for (int i = k + 1; i <= mp; ++i) {
        UQ[std::size_t(i + r)] = s.U[std::size_t(i)];
    }
    for (int i = 0; i <= k - p; ++i) {
        Q[std::size_t(i)] = s.P[std::size_t(i)];
    }
    for (int i = k - mult; i <= np; ++i) {
        Q[std::size_t(i + r)] = s.P[std::size_t(i)];
    }
    std::vector<Vec> R(std::size_t(p + 1));
    for (int i = 0; i <= p - mult; ++i) {
        R[std::size_t(i)] = s.P[std::size_t(k - p + i)];
    }
    int L = 0;
    for (int j = 1; j <= r; ++j) {
        L = k - p + j;
        for (int i = 0; i <= p - j - mult; ++i) {
            const double den = s.U[std::size_t(i + k + 1)] - s.U[std::size_t(L + i)];
            const double alpha = den > 0. ? (t - s.U[std::size_t(L + i)]) / den : 0.;
            R[std::size_t(i)] = R[std::size_t(i + 1)] * alpha + R[std::size_t(i)] * (1. - alpha);
        }
        Q[std::size_t(L)] = R[0];
        Q[std::size_t(k + r - j - mult)] = R[std::size_t(p - j - mult)];
    }
    for (int i = L + 1; i < k - mult; ++i) {
        Q[std::size_t(i)] = R[std::size_t(i - L)];
    }
    s.U.swap(UQ);
    s.P.swap(Q);
}

/** The piece [a, b] (t0 <= a < b <= t1) as an open clamped spline with the same shape. */
inline bool sub(const Spline& in, double a, double b, Spline& out) {
    if (!(b > a) || a < in.t0() || b > in.t1()) {
        return false;
    }
    Spline s = in;
    insertKnot(s, a, s.p);
    insertKnot(s, b, s.p);
    const int ka = int(std::upper_bound(s.U.begin(), s.U.end(), a) - s.U.begin()) - 1;
    const int kb = int(std::lower_bound(s.U.begin(), s.U.end(), b) - s.U.begin());
    if (ka - s.p < 0 || kb > s.n() || kb - 1 < ka - s.p) {
        return false;
    }
    out.p = s.p;
    out.periodic = false;
    out.P.assign(s.P.begin() + (ka - s.p), s.P.begin() + kb);
    out.U.assign(std::size_t(s.p + 1), a);
    out.U.insert(out.U.end(), s.U.begin() + ka + 1, s.U.begin() + kb);
    out.U.insert(out.U.end(), std::size_t(s.p + 1), b);
    return out.U.size() == out.P.size() + std::size_t(out.p) + 1 && out.n() >= out.p + 1;
}

/** A then B (B starts where A ends), one open spline; the join keeps knot multiplicity p. */
inline bool concat(const Spline& A, const Spline& B, Spline& out) {
    if (A.p != B.p || A.n() < A.p + 1 || B.n() < B.p + 1) {
        return false;
    }
    const int p = A.p;
    const double shift = A.t1() - B.t0();
    out.p = p;
    out.periodic = false;
    out.P = A.P;
    out.P.insert(out.P.end(), B.P.begin() + 1, B.P.end());
    out.U.assign(A.U.begin(), A.U.end() - 1);
    for (std::size_t i = std::size_t(p) + 1; i < B.U.size(); ++i) {
        out.U.push_back(B.U[i] + shift);
    }
    return out.U.size() == out.P.size() + std::size_t(p) + 1;
}

/**
 * Open: the pieces [t0, t] and [t, t1]. Closed: one open spline running from t round to t (second empty).
 * Returns false when t is at an open end.
 */
inline bool split(const Spline& s, double t, Spline& first, Spline& second, bool& hasSecond) {
    hasSecond = false;
    if (!s.periodic) {
        if (!(t > s.t0() && t < s.t1())) {
            return false;
        }
        hasSecond = true;
        return sub(s, s.t0(), t, first) && sub(s, t, s.t1(), second);
    }
    t = wrapParam(s, t);
    if (t <= s.t0()) {
        return sub(s, s.t0(), s.t1(), first);
    }
    Spline a, b;
    return sub(s, t, s.t1(), a) && sub(s, s.t0(), t, b) && concat(a, b, first);
}

/** Same curve, opposite direction (knots mirrored). */
inline Spline reversed(const Spline& s) {
    Spline r = s;
    std::reverse(r.P.begin(), r.P.end());
    const double lo = s.U.front();
    const double hi = s.U.back();
    for (std::size_t i = 0; i < s.U.size(); ++i) {
        r.U[i] = lo + hi - s.U[s.U.size() - 1 - i];
    }
    return r;
}

/** Unit tangent leaving t (dir > 0) or arriving at t (dir < 0), from points when C' vanishes. */
inline Vec tangentAt(const Spline& s, double t, int dir) {
    const int k = dir > 0 ? findSpan(s, t) : [&]() {
        // left limit: span with U[k] < t <= U[k+1]
        int kk = findSpan(s, t);
        while (kk > s.p && !(s.U[std::size_t(kk)] < t)) {
            --kk;
        }
        return kk;
    }();
    Vec d1;
    evalSpan(s, k, t, &d1);
    const double sp = d1.norm();
    double diag = 0.;
    double maxAbs = 0.;
    extent(s, diag, maxAbs);
    const double h = s.U[std::size_t(k) + 1] - s.U[std::size_t(k)];
    if (sp * h > 1e-6 * (diag + 1e-300)) {
        return d1 * (1. / sp);
    }
    // Vanishing derivative (a corner block: C' = C'' = 0 there). A chord to a nearby point is rounding noise
    // far from the origin (step 25: 3.48e6, the pentagon's mitres went wrong), so use the control polygon:
    // with t a knot of multiplicity p the curve passes through a control point there and leaves (arrives)
    // along the leg to the next (from the previous) distinct one - exactly.
    Spline c = s;
    insertKnot(c, t, c.p);
    const int last = int(std::upper_bound(c.U.begin(), c.U.end(), t) - c.U.begin()) - 1;
    const int idx = std::clamp(last - c.p, 0, c.n() - 1);
    const Vec& P = c.P[std::size_t(idx)];
    const double tol = std::max(16e-12 * (1. + maxAbs), 1e-9 * diag);
    if (dir > 0) {
        for (int j = idx + 1; j < c.n(); ++j) {
            const Vec v = c.P[std::size_t(j)] - P;
            if (v.norm() > tol) {
                return v * (1. / v.norm());
            }
        }
    } else {
        for (int j = idx - 1; j >= 0; --j) {
            const Vec v = P - c.P[std::size_t(j)];
            if (v.norm() > tol) {
                return v * (1. / v.norm());
            }
        }
    }
    return sp > 0. ? d1 * (1. / sp) : Vec(1., 0.);
}

/** Interior parameters where the curve turns by more than angleTol (C0 knots, corner blocks); closed: the seam too. */
inline std::vector<double> corners(const Spline& s, double angleTol = 1e-4) {
    std::vector<double> out;
    for (int k = s.p + 1; k < s.n(); ++k) {
        const double t = s.U[std::size_t(k)];
        if (!(t > s.U[std::size_t(k) - 1])) {
            continue; // handled at the first of equal knots
        }
        if (!(t > s.t0() && t < s.t1())) {
            continue;
        }
        const Vec a = tangentAt(s, t, -1);
        const Vec b = tangentAt(s, t, 1);
        if (std::atan2(std::abs(a.cross(b)), a.dot(b)) > angleTol) {
            out.push_back(t);
        }
    }
    if (s.periodic) {
        const Vec a = tangentAt(s, s.t1(), -1);
        const Vec b = tangentAt(s, s.t0(), 1);
        if (std::atan2(std::abs(a.cross(b)), a.dot(b)) > angleTol) {
            out.insert(out.begin(), s.t0());
        }
    }
    return out;
}

namespace detail {

inline bool segIntersect(const Vec& a, const Vec& b, const Vec& c, const Vec& d, Vec& x) {
    const Vec r = b - a;
    const Vec q = d - c;
    const double den = r.cross(q);
    if (std::abs(den) <= 1e-300) {
        return false;
    }
    const double u = (c - a).cross(q) / den;
    const double v = (c - a).cross(r) / den;
    if (u < 0. || u > 1. || v < 0. || v > 1.) {
        return false;
    }
    x = a + r * u;
    return true;
}

inline bool lineIntersect(const Vec& a, const Vec& ta, const Vec& b, const Vec& tb, Vec& x) {
    const double den = ta.cross(tb);
    if (std::abs(den) <= 1e-12) {
        return false;
    }
    const double u = (b - a).cross(tb) / den;
    x = a + ta * u;
    return true;
}

} // namespace detail

/** Why offsetPolyline() refused. */
enum class OffsetError { None, Degenerate, Radius };

/**
 * The offset of s by d (d > 0: to the left of the direction of travel) as a polyline: samples of every
 * smooth piece (chords within eps of the true offset) joined at corners - mitred on the outside, cut where
 * the two offsets cross on the inside. vertex[] marks the joins and open ends (corner candidates for
 * lc::splinefit::fit). Refuses with Radius where |d| reaches the radius of curvature on the inner side
 * (the offset would turn back on itself).
 */
inline bool offsetPolyline(const Spline& s, double d, double eps, std::vector<Vec>& pts, std::vector<char>& vertex,
                           OffsetError* why = nullptr) {
    pts.clear();
    vertex.clear();
    if (why != nullptr) {
        *why = OffsetError::Degenerate;
    }
    if (!(std::abs(d) > 0.) || s.n() < s.p + 1) {
        return false;
    }
    std::vector<double> cut = corners(s);
    // pieces between corners
    std::vector<std::pair<double, double>> pieces;
    if (s.periodic) {
        if (cut.empty()) {
            pieces.emplace_back(s.t0(), s.t1());
        } else {
            for (std::size_t i = 0; i < cut.size(); ++i) {
                const double a = cut[i];
                const double b = i + 1 < cut.size() ? cut[i + 1] : cut[0] + (s.t1() - s.t0());
                pieces.emplace_back(a, b);
            }
        }
    } else {
        double a = s.t0();
        for (double c : cut) {
            pieces.emplace_back(a, c);
            a = c;
        }
        pieces.emplace_back(a, s.t1());
    }
    std::vector<Vec> tp;
    std::vector<double> tt;
    tessellate(s, eps * 0.25, tp, tt);
    const double T = s.t1() - s.t0();
    auto at = [&](double t, int dir) {
        // point and left normal at t (t may run past t1 on a closed curve)
        const double tw = s.periodic ? wrapParam(s, t) : t;
        const double use = (s.periodic && dir < 0 && tw == s.t0() && t > s.t0()) ? s.t1() : tw;
        const Vec c = eval(s, use);
        const Vec tg = tangentAt(s, use, dir);
        return std::make_pair(c + Vec(-tg.y, tg.x) * d, tg);
    };
    std::vector<std::vector<Vec>> off(pieces.size());
    std::vector<Vec> tanStart(pieces.size());
    std::vector<Vec> tanEnd(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const double a = pieces[i].first;
        const double b = pieces[i].second;
        std::vector<double> params{a};
        // degree 1: every piece is straight (corners() cut at each bend), its two ends are the offset
        for (int lap = 0; lap < (s.p == 1 ? 0 : (s.periodic ? 2 : 1)); ++lap) {
            for (double t : tt) {
                const double u = t + lap * T;
                if (u > a && u < b) {
                    params.push_back(u);
                }
            }
        }
        params.push_back(b);
        std::sort(params.begin(), params.end());
        for (std::size_t j = 0; j < params.size(); ++j) {
            const double u = params[j];
            const int dir = j == 0 ? 1 : (j + 1 == params.size() ? -1 : 1);
            const auto pr = at(u, dir);
            if (j > 0 && j + 1 < params.size()) {
                Vec d1, d2;
                eval(s, s.periodic ? wrapParam(s, u) : u, &d1, &d2);
                const double sp = d1.norm();
                if (sp > 0.) {
                    const double kappa = d1.cross(d2) / (sp * sp * sp);
                    if (d * kappa >= 1. - 1e-9) {
                        if (why != nullptr) {
                            *why = OffsetError::Radius;
                        }
                        return false;
                    }
                }
            }
            off[i].push_back(pr.first);
            if (j == 0) {
                tanStart[i] = pr.second;
            }
            if (j + 1 == params.size()) {
                tanEnd[i] = pr.second;
            }
        }
    }
    // joins: piece i end with piece i+1 start
    const std::size_t m = pieces.size();
    const std::size_t joins = s.periodic ? (cut.empty() ? 0 : m) : m - 1;
    for (std::size_t i = 0; i < joins; ++i) {
        const std::size_t j = (i + 1) % m;
        std::vector<Vec>& A = off[i];
        std::vector<Vec>& B = off[j];
        const Vec ta = tanEnd[i];
        const Vec tb = tanStart[j];
        const bool inside = ta.cross(tb) * d > 0.;
        Vec x;
        bool found = false;
        if (inside) {
            // the two offsets cross near the corner: search back along A and forward along B
            const std::size_t stopA = A.size() > 4000 ? A.size() - 4000 : 0;
            const std::size_t endB = std::min<std::size_t>(B.size(), 4000);
            for (std::size_t ia = A.size() - 1; ia > stopA && !found; --ia) {
                for (std::size_t ib = 0; ib + 1 < endB; ++ib) {
                    if (detail::segIntersect(A[ia - 1], A[ia], B[ib], B[ib + 1], x)) {
                        A.resize(ia);
                        A.push_back(x);
                        B.erase(B.begin(), B.begin() + long(ib) + 1);
                        B.insert(B.begin(), x);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            if (detail::lineIntersect(A.back(), ta, B.front(), tb, x) && (x - A.back()).norm() <= 10. * std::abs(d)) {
                A.push_back(x);
                B.insert(B.begin(), x);
            }
        }
    }
    // assemble (a piece starts where the previous one ends after the joins: the repeat is merged below)
    for (std::size_t i = 0; i < m; ++i) {
        const std::vector<Vec>& A = off[i];
        for (std::size_t j = 0; j < A.size(); ++j) {
            const bool end = j == 0 || j + 1 == A.size();
            pts.push_back(A[j]);
            vertex.push_back(char(end && (!s.periodic || !cut.empty()) ? 1 : 0));
        }
    }
    auto same = [](const Vec& a, const Vec& b) {
        return (a - b).norm() <= 1e-12 * (1. + std::abs(a.x) + std::abs(a.y));
    };
    std::vector<Vec> q;
    std::vector<char> qv;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (!q.empty() && same(pts[i], q.back())) {
            qv.back() = char(qv.back() | vertex[i]);
            continue;
        }
        q.push_back(pts[i]);
        qv.push_back(vertex[i]);
    }
    if (s.periodic && q.size() > 1 && same(q.front(), q.back())) {
        qv.front() = char(qv.front() | qv.back());
        q.pop_back();
        qv.pop_back();
    }
    if (s.p == 1 && q.size() > 2) {
        // a mitre keeps the old piece ends on the straight run to it: drop points with no turn
        std::vector<Vec> r;
        std::vector<char> rv;
        const std::size_t cnt = q.size();
        for (std::size_t i = 0; i < cnt; ++i) {
            const bool end = !s.periodic && (i == 0 || i + 1 == cnt);
            if (!end) {
                const Vec& a = q[(i + cnt - 1) % cnt];
                const Vec& b = q[(i + 1) % cnt];
                const Vec u = q[i] - a;
                const Vec w = b - q[i];
                if (std::abs(u.cross(w)) <= 1e-12 * u.norm() * w.norm() && u.dot(w) > 0.) {
                    continue;
                }
            }
            r.push_back(q[i]);
            rv.push_back(qv[i]);
        }
        q.swap(r);
        qv.swap(rv);
    }
    pts.swap(q);
    vertex.swap(qv);
    if (why != nullptr) {
        *why = OffsetError::None;
    }
    return pts.size() >= 2;
}

namespace detail {

inline Vec leftOf(const Vec& u) { return Vec(-u.y, u.x); }

/**
 * Tiller-Hanson: every leg of the control polygon moved by d to its left, each control point at the meeting
 * of the moved legs around it. Zero-length legs (repeated points: corner blocks) are skipped; their points
 * take the meeting of the nearest real legs.
 */
inline bool offsetPolygon(const Spline& s, double d, std::vector<Vec>& Q) {
    const int n = s.n();
    double diag = 0.;
    double maxAbs = 0.;
    extent(s, diag, maxAbs);
    const double tiny = 1e-12 * (1. + diag + maxAbs);
    std::vector<Vec> leg(std::size_t(std::max(0, n - 1)));
    std::vector<int> prevLeg(std::size_t(n), -1);
    std::vector<int> nextLeg(std::size_t(n), -1);
    bool any = false;
    for (int i = 0; i + 1 < n; ++i) {
        const Vec v = s.P[std::size_t(i) + 1] - s.P[std::size_t(i)];
        const double l = v.norm();
        leg[std::size_t(i)] = l > tiny ? v * (1. / l) : Vec();
        any = any || l > tiny;
    }
    if (!any) {
        return false;
    }
    int last = -1;
    for (int i = 0; i < n; ++i) {
        prevLeg[std::size_t(i)] = last;
        if (i + 1 < n && leg[std::size_t(i)].norm() > 0.) {
            last = i;
        }
    }
    int next = -1;
    for (int i = n - 1; i >= 0; --i) {
        if (i + 1 < n && leg[std::size_t(i)].norm() > 0.) {
            next = i;
        }
        nextLeg[std::size_t(i)] = next;
    }
    Q.assign(std::size_t(n), Vec());
    for (int i = 0; i < n; ++i) {
        int a = prevLeg[std::size_t(i)];
        int b = nextLeg[std::size_t(i)];
        if (a < 0) {
            a = b;
        }
        if (b < 0) {
            b = a;
        }
        const Vec& P = s.P[std::size_t(i)];
        const Vec ua = leg[std::size_t(a)];
        const Vec ub = leg[std::size_t(b)];
        const Vec pa = P + leftOf(ua) * d;
        const Vec pb = P + leftOf(ub) * d;
        Vec x;
        if (a == b || std::abs(ua.cross(ub)) < 1e-9 || !lineIntersect(pa, ua, pb, ub, x) ||
            (x - P).norm() > 10. * std::abs(d)) {
            const Vec m = leftOf(ua) + leftOf(ub);
            const double ml = m.norm();
            x = ml > 1e-12 ? P + m * (std::abs(d) / ml) * (d < 0. ? -1. : 1.) : pa;
        }
        Q[std::size_t(i)] = x;
    }
    return true;
}

/** Largest d * curvature over the curve (>= 1: the offset turns back on itself). */
inline double maxCurvatureRatio(const Spline& s, double d) {
    double worst = -std::numeric_limits<double>::max();
    for (int k = s.p; k < s.n(); ++k) {
        const double a = s.U[std::size_t(k)];
        const double b = s.U[std::size_t(k) + 1];
        if (!(b > a)) {
            continue;
        }
        // speeds far below the span's average (a corner block's end: C' = 0) give no curvature
        const double mean = (evalSpan(s, k, b) - evalSpan(s, k, a)).norm() / (b - a);
        for (int q = 0; q <= 32; ++q) {
            Vec d1, d2;
            evalSpan(s, k, a + (b - a) * q / 32., &d1, &d2);
            const double sp = d1.norm();
            if (sp > 1e-6 * mean && sp > 0.) {
                worst = std::max(worst, d * d1.cross(d2) / (sp * sp * sp));
            }
        }
    }
    return worst;
}

/**
 * Offset of one smooth open piece: Tiller-Hanson, then a knot in the middle of every span whose offset is
 * still off by more than tol (checked at 7 points per span), until none is. "Off" is geometric: the distance
 * from the offset point to the nearest base point minus |d|, on the right side. (Comparing O(t) with
 * C(t) + d n(t) at the same t counted the parameter drift as error, never settled where C' vanishes at a
 * corner block, and doubled the points.)
 */
inline bool offsetSmooth(const Spline& piece, double d, double tol, Spline& out, std::size_t maxPoints) {
    Spline b = piece;
    for (int it = 0; it < 40; ++it) {
        std::vector<Vec> Q;
        if (!offsetPolygon(b, d, Q)) {
            return false;
        }
        Spline o = b;
        o.P = Q;
        std::vector<double> refine;
        for (int k = b.p; k < b.n(); ++k) {
            const double a = b.U[std::size_t(k)];
            const double e = b.U[std::size_t(k) + 1];
            if (!(e > a)) {
                continue;
            }
            for (int q = 1; q < 8; ++q) {
                const double t = a + (e - a) * q / 8.;
                const Vec op = evalSpan(o, k, t);
                const double sn = project(b, op, t, b.t0(), b.t1());
                Vec d1;
                const Vec c = eval(b, sn, &d1);
                const Vec r = op - c;
                const double side = r.cross(d1) * (d > 0. ? -1. : 1.); // left of travel for d > 0
                if (std::abs(r.norm() - std::abs(d)) > tol || side < 0.) {
                    refine.push_back(0.5 * (a + e));
                    break;
                }
            }
        }
        if (refine.empty()) {
            out = o;
            return true;
        }
        if (b.P.size() + refine.size() > maxPoints) {
            return false;
        }
        for (double t : refine) {
            insertKnot(b, t, 1);
        }
    }
    return false;
}

/** A straight piece from a to b as a spline of degree p. */
inline Spline straight(const Vec& a, const Vec& b, int p) {
    Spline s;
    s.p = p;
    const double L = std::max((b - a).norm(), 1e-300);
    for (int i = 0; i <= p; ++i) {
        s.P.push_back(a + (b - a) * (double(i) / p));
    }
    s.U.assign(std::size_t(p + 1), 0.);
    s.U.insert(s.U.end(), std::size_t(p + 1), L);
    return s;
}

/**
 * Where the end of A and the start of B cross (A's parameter ta, B's tb): searched on their lines, Newton after.
 * same: A and B are one curve (a loop with one corner) - neighbouring lines share a point, which is no crossing.
 */
inline bool crossNear(const Spline& A, const Spline& B, double eps, double& ta, double& tb, bool same = false) {
    std::vector<Vec> pa, pb;
    std::vector<double> sa, sb;
    tessellate(A, eps, pa, sa);
    tessellate(B, eps, pb, sb);
    const std::size_t stopA = pa.size() > 4000 ? pa.size() - 4000 : 0;
    const std::size_t endB = std::min<std::size_t>(pb.size(), 4000);
    for (std::size_t ia = pa.size() - 1; ia > stopA; --ia) {
        for (std::size_t ib = 0; ib + 1 < endB; ++ib) {
            if (same && ib + 2 >= ia) {
                break; // the same line or a neighbour of it (and everything after belongs to A's end)
            }
            Vec x;
            if (!segIntersect(pa[ia - 1], pa[ia], pb[ib], pb[ib + 1], x)) {
                continue;
            }
            const double la = (pa[ia] - pa[ia - 1]).norm();
            const double lb = (pb[ib + 1] - pb[ib]).norm();
            ta = sa[ia - 1] + (sa[ia] - sa[ia - 1]) * (la > 0. ? (x - pa[ia - 1]).norm() / la : 0.);
            tb = sb[ib] + (sb[ib + 1] - sb[ib]) * (lb > 0. ? (x - pb[ib]).norm() / lb : 0.);
            for (int it = 0; it < 30; ++it) {
                Vec da, db;
                const Vec F = eval(A, ta, &da) - eval(B, tb, &db);
                const double det = -da.x * db.y + db.x * da.y;
                if (!(std::abs(det) > 0.)) {
                    break;
                }
                const double dta = (F.x * db.y - db.x * F.y) / det;
                const double dtb = (-da.x * F.y + da.y * F.x) / det;
                ta = std::clamp(ta + dta, A.t0(), A.t1());
                tb = std::clamp(tb + dtb, B.t0(), B.t1());
                if (std::abs(dta) <= 1e-15 * (1. + std::abs(ta)) && std::abs(dtb) <= 1e-15 * (1. + std::abs(tb))) {
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

} // namespace detail

/**
 * The offset of s by d (d > 0: left of the direction of travel) within tol, as one open spline: the smooth
 * pieces between corners each by Tiller-Hanson with knot refinement, joined at the corners - mitred on the
 * outside (bevelled past 10|d|), cut where they cross on the inside. A closed spline gives an open one that
 * ends where it starts. Refuses with Radius where |d| reaches the radius of curvature on the inner side.
 */
inline bool offsetSpline(const Spline& s, double d, double tol, Spline& out, OffsetError* why = nullptr,
                         std::size_t maxPoints = 50000) {
    if (why != nullptr) {
        *why = OffsetError::Degenerate;
    }
    if (!(std::abs(d) > 0.) || s.n() < s.p + 1) {
        return false;
    }
    // smooth pieces, as open clamped splines
    std::vector<Spline> pieces;
    const std::vector<double> cs = corners(s);
    if (!s.periodic) {
        double a = s.t0();
        std::vector<double> ends = cs;
        ends.push_back(s.t1());
        for (double c : ends) {
            Spline pc;
            if (c > a && sub(s, a, c, pc)) {
                pieces.push_back(pc);
            }
            a = c;
        }
    } else {
        Spline loop, unused;
        bool two = false;
        const double start = cs.empty() ? s.t0() : cs.front();
        if (!split(s, start, loop, unused, two)) {
            return false;
        }
        double a = loop.t0();
        const double T = s.t1() - s.t0();
        std::vector<double> ends;
        for (std::size_t i = 1; i < cs.size(); ++i) {
            ends.push_back(loop.t0() + (cs[i] - start));
        }
        ends.push_back(loop.t0() + T);
        for (double c : ends) {
            Spline pc;
            if (c > a && sub(loop, a, std::min(c, loop.t1()), pc)) {
                pieces.push_back(pc);
            }
            a = c;
        }
    }
    if (pieces.empty()) {
        return false;
    }
    for (const Spline& pc : pieces) {
        if (detail::maxCurvatureRatio(pc, d) >= 1. - 1e-9) {
            if (why != nullptr) {
                *why = OffsetError::Radius;
            }
            return false;
        }
    }
    std::vector<Spline> off(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (!detail::offsetSmooth(pieces[i], d, tol, off[i], maxPoints)) {
            return false;
        }
    }
    // joins: piece i to piece i+1 (closed with corners: the last to the first as well)
    const std::size_t m = pieces.size();
    const std::size_t joins = s.periodic ? (cs.empty() ? 0 : m) : m - 1;
    std::vector<std::vector<Spline>> connectors(m);
    for (std::size_t i = 0; i < joins; ++i) {
        const std::size_t j = (i + 1) % m; // j == i: a loop with one corner meets itself
        const Spline A = off[i];
        const Spline B = off[j];
        const Vec ta = tangentAt(pieces[i], pieces[i].t1(), -1);
        const Vec tb = tangentAt(pieces[j], pieces[j].t0(), 1);
        const Vec EA = eval(A, A.t1());
        const Vec SB = eval(B, B.t0());
        const bool inside = ta.cross(tb) * d > 0.;
        double ua = 0.;
        double ub = 0.;
        if (inside && detail::crossNear(A, B, tol, ua, ub, i == j) && ua > A.t0() && ub < B.t1()) {
            if (i == j) {
                Spline r;
                if (ub < ua && sub(A, ub, ua, r)) {
                    r.P.back() = r.P.front();
                    off[i] = r;
                    continue;
                }
            } else {
                Spline a2, b2;
                if (sub(A, A.t0(), ua, a2) && sub(B, ub, B.t1(), b2)) {
                    b2.P.front() = eval(a2, a2.t1()); // the same point on both
                    off[i] = a2;
                    off[j] = b2;
                    continue;
                }
            }
        }
        Vec M;
        if (!inside && detail::lineIntersect(EA, ta, SB, tb, M) && (M - EA).norm() <= 10. * std::abs(d)) {
            if ((M - EA).norm() > 1e-12 * (1. + std::abs(M.x) + std::abs(M.y))) {
                connectors[i].push_back(detail::straight(EA, M, s.p));
            }
            if ((SB - M).norm() > 1e-12 * (1. + std::abs(M.x) + std::abs(M.y))) {
                connectors[i].push_back(detail::straight(M, SB, s.p));
            }
        } else if ((SB - EA).norm() > 1e-12 * (1. + std::abs(EA.x) + std::abs(EA.y))) {
            connectors[i].push_back(detail::straight(EA, SB, s.p));
        }
    }
    Spline acc = off[0];
    for (std::size_t i = 0; i < m; ++i) {
        for (Spline& c : connectors[i]) {
            c.P.front() = eval(acc, acc.t1());
            Spline next;
            if (!concat(acc, c, next)) {
                return false;
            }
            acc = next;
        }
        if (i + 1 < m) {
            Spline B = off[i + 1];
            B.P.front() = eval(acc, acc.t1());
            Spline next;
            if (!concat(acc, B, next)) {
                return false;
            }
            acc = next;
        }
    }
    if (s.periodic) {
        acc.P.back() = acc.P.front(); // ends where it starts
    }
    out = acc;
    if (why != nullptr) {
        *why = OffsetError::None;
    }
    return true;
}

} // namespace splinegeom
} // namespace lc

#endif

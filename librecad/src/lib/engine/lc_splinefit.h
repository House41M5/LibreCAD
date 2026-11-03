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
 * Context: "Spline from polyline" (vp, lc_actionsplineexplode.h) puts every vertex plus Mid Points points
 * per segment into the spline, so N segments became 3N+1 control points (2026-11-02, plans-win
 * librecad-factory-drawings step 22: user asked why there are so many points). Its Tolerance mode asks this
 * file for few control points within a tolerance of the polyline instead.
 * How: least squares on RS_Spline's own knots - uniform; open = clamped (RS_Spline::knot), closed = periodic
 * with the first `degree` points wrapped (RS_Spline::knotu) - so the result is a plain RS_Spline with no
 * custom knot vector for DXF/DWG or the spline point actions to carry. Each data point maps linearly (by
 * length along the polyline) into its span. The spans start few; every span still off by more than the
 * tolerance is halved, until all are within it both ways: data -> curve after projecting each data point
 * onto the curve, and curve -> polyline from samples of every span. A span halved many times is short on
 * the drawing but still one unit of parameter, and LibreCAD draws $SPLINESEGS lines per unit, so the
 * detailed parts get the drawing lines as well as the control points. Neighbouring spans are kept within 2:1 in
 * length (see the balance in fit()). Halving whole spans overshoots, so one
 * sweep of knot removal follows: a breakpoint goes when the fit without it is still within the tolerance.
 * Corners (turn > kCornerAngle, the angle of the vx Arcs biarc fit) stay sharp: the `degree` control points
 * that are non-zero at that knot are all the vertex, so the curve passes through it and is straight on both
 * sides. A piece between two corners keeps at least `degree` spans so the blocks never overlap.
 * Gotchas:
 *  - A tolerance below the polyline's own corners (a dense polyline from vx) cannot be met with fewer
 *    points than the polyline has: the curve has to hug every chord. maxDeviation and `reached` say so and
 *    the caller reports it.
 *  - The closed system is cyclic. With a corner the unknowns start right after its fixed block, which cuts
 *    the cycle, so the matrix is banded; without one, the last `degree` unknowns go through a Schur
 *    complement.
 *  - No Qt/LibreCAD types, so test/lc_splinefit_test.cpp builds it alone. No function may be called emit,
 *    signals or slots (Qt macros; see lc_biarcfit.h).
 */
#ifndef LC_SPLINEFIT_H
#define LC_SPLINEFIT_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#ifndef LC_SPLINEFIT_TRACE
#define LC_SPLINEFIT_TRACE(...) ((void) 0)
#endif

namespace lc {
namespace splinefit {

struct Vec {
    double x = 0.;
    double y = 0.;
    Vec() = default;
    Vec(double x_, double y_) : x(x_), y(y_) {}
    Vec operator+(const Vec& o) const { return {x + o.x, y + o.y}; }
    Vec operator-(const Vec& o) const { return {x - o.x, y - o.y}; }
    Vec operator*(double s) const { return {x * s, y * s}; }
    double dot(const Vec& o) const { return x * o.x + y * o.y; }
    double cross(const Vec& o) const { return x * o.y - y * o.x; }
    double norm() const { return std::hypot(x, y); }
};

/** Turning angle above which a vertex stays a sharp corner (the same as lc::biarc::kCornerAngle). */
constexpr double kCornerAngle = 0.5;

struct Result {
    std::vector<Vec> control; ///< RS_Spline control points; closed: not wrapped (RS_Spline::update wraps them)
    int degree = 3;
    bool closed = false;
    double maxDeviation = 0.; ///< both ways against the polyline, as measured by the fit
    bool reached = false;     ///< maxDeviation <= tolerance
};

namespace detail {

inline double distToSegment(const Vec& p, const Vec& a, const Vec& b) {
    const Vec d = b - a;
    const double l2 = d.dot(d);
    double t = l2 > 0. ? (p - a).dot(d) / l2 : 0.;
    t = std::clamp(t, 0., 1.);
    return (p - (a + d * t)).norm();
}

/** Basis functions and derivatives, The NURBS Book A2.3: ders[k][j] = k-th derivative of N(span-p+j). */
inline void dersBasis(const std::vector<double>& U, int span, double u, int p, int nd, double ders[3][4]) {
    double ndu[4][4] = {};
    double left[4] = {};
    double right[4] = {};
    ndu[0][0] = 1.;
    for (int j = 1; j <= p; ++j) {
        left[j] = u - U[span + 1 - j];
        right[j] = U[span + j] - u;
        double saved = 0.;
        for (int q = 0; q < j; ++q) {
            ndu[j][q] = right[q + 1] + left[j - q];
            const double temp = ndu[q][j - 1] / ndu[j][q];
            ndu[q][j] = saved + right[q + 1] * temp;
            saved = left[j - q] * temp;
        }
        ndu[j][j] = saved;
    }
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 4; ++j) {
            ders[k][j] = 0.;
        }
    }
    for (int j = 0; j <= p; ++j) {
        ders[0][j] = ndu[j][p];
    }
    nd = std::min(nd, p);
    double a[2][4] = {};
    for (int q = 0; q <= p; ++q) {
        int s1 = 0;
        int s2 = 1;
        a[0][0] = 1.;
        for (int k = 1; k <= nd; ++k) {
            double d = 0.;
            const int rk = q - k;
            const int pk = p - k;
            if (q >= k) {
                a[s2][0] = a[s1][0] / ndu[pk + 1][rk];
                d = a[s2][0] * ndu[rk][pk];
            }
            const int j1 = rk >= -1 ? 1 : -rk;
            const int j2 = q - 1 <= pk ? k - 1 : p - q;
            for (int j = j1; j <= j2; ++j) {
                a[s2][j] = (a[s1][j] - a[s1][j - 1]) / ndu[pk + 1][rk + j];
                d += a[s2][j] * ndu[rk + j][pk];
            }
            if (q <= pk) {
                a[s2][k] = -a[s1][k - 1] / ndu[pk + 1][q];
                d += a[s2][k] * ndu[q][pk];
            }
            ders[k][q] = d;
            std::swap(s1, s2);
        }
    }
    double fac = p;
    for (int k = 1; k <= nd; ++k) {
        for (int j = 0; j <= p; ++j) {
            ders[k][j] *= fac;
        }
        fac *= p - k;
    }
}

/**
 * A B-spline on RS_Spline's knots with `spans` unit spans, parameter u in [0, spans]. Open: clamped,
 * spans + p control points. Closed: periodic, `spans` control points (RS_Spline's t = u + p).
 */
class Curve {
public:
    Curve(int p, bool closed, int spans) : m_p(p), m_closed(closed), m_m(spans), m_n(closed ? spans : spans + p) {
        // open: n + p + 1 knots; closed: (n + p wrapped points) + p + 1 - both spans + 2p + 1
        const int size = m_m + 2 * p + 1;
        m_U.resize(size);
        for (int j = 0; j < size; ++j) {
            m_U[j] = closed ? double(j - p) : double(std::clamp(j - p, 0, m_m));
        }
    }
    int count() const { return m_n; }
    int degree() const { return m_p; }
    int spans() const { return m_m; }
    bool closed() const { return m_closed; }

    double wrap(double u) const {
        if (!m_closed) {
            return std::clamp(u, 0., double(m_m));
        }
        u -= m_m * std::floor(u / m_m);
        return u >= m_m ? 0. : u;
    }

    int spanOf(double u) const { return std::clamp(int(std::floor(wrap(u))), 0, m_m - 1); }

    /** Control indices and basis values (plus nd derivatives) at u. */
    void basis(double u, int idx[4], double ders[3][4], int nd) const {
        u = wrap(u);
        const int s = spanOf(u);
        const int span = m_p + s;
        dersBasis(m_U, span, u, m_p, nd, ders);
        for (int a = 0; a <= m_p; ++a) {
            idx[a] = m_closed ? (s + a) % m_n : s + a;
        }
    }

    void eval(const std::vector<Vec>& P, double u, Vec* c, Vec* d1 = nullptr, Vec* d2 = nullptr) const {
        int idx[4];
        double ders[3][4];
        basis(u, idx, ders, d2 != nullptr ? 2 : (d1 != nullptr ? 1 : 0));
        Vec r0, r1, r2;
        for (int a = 0; a <= m_p; ++a) {
            const Vec& q = P[idx[a]];
            r0 = r0 + q * ders[0][a];
            r1 = r1 + q * ders[1][a];
            r2 = r2 + q * ders[2][a];
        }
        *c = r0;
        if (d1 != nullptr) {
            *d1 = r1;
        }
        if (d2 != nullptr) {
            *d2 = r2;
        }
    }

private:
    int m_p;
    bool m_closed;
    int m_m;
    int m_n;
    std::vector<double> m_U;
};

/** Lower band Cholesky in place: a[i*(w+1)+k] = A(i, i-k). */
inline bool bandCholesky(std::vector<double>& a, int f, int w) {
    const int W = w + 1;
    for (int i = 0; i < f; ++i) {
        const int j0 = std::max(0, i - w);
        for (int j = j0; j <= i; ++j) {
            double sum = a[i * W + (i - j)];
            for (int l = std::max(j0, j - w); l < j; ++l) {
                sum -= a[i * W + (i - l)] * a[j * W + (j - l)];
            }
            if (j == i) {
                if (!(sum > 0.) || !std::isfinite(sum)) {
                    return false;
                }
                a[i * W] = std::sqrt(sum);
            } else {
                a[i * W + (i - j)] = sum / a[j * W];
            }
        }
    }
    return true;
}

inline void bandSolve(const std::vector<double>& a, int f, int w, double* x) {
    const int W = w + 1;
    for (int i = 0; i < f; ++i) {
        double s = x[i];
        for (int l = std::max(0, i - w); l < i; ++l) {
            s -= a[i * W + (i - l)] * x[l];
        }
        x[i] = s / a[i * W];
    }
    for (int i = f - 1; i >= 0; --i) {
        double s = x[i];
        for (int l = i + 1; l <= std::min(f - 1, i + w); ++l) {
            s -= a[l * W + (l - i)] * x[l];
        }
        x[i] = s / a[i * W];
    }
}

/** Small dense solve with partial pivoting; A is n x n row-major, b holds `cols` right-hand sides. */
inline bool denseSolve(std::vector<double> A, int n, std::vector<double>& b, int cols) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) {
            if (std::abs(A[r * n + c]) > std::abs(A[piv * n + c])) {
                piv = r;
            }
        }
        if (!(std::abs(A[piv * n + c]) > 0.)) {
            return false;
        }
        if (piv != c) {
            for (int k = 0; k < n; ++k) {
                std::swap(A[c * n + k], A[piv * n + k]);
            }
            for (int k = 0; k < cols; ++k) {
                std::swap(b[c * cols + k], b[piv * cols + k]);
            }
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            for (int k = c; k < n; ++k) {
                A[r * n + k] -= f * A[c * n + k];
            }
            for (int k = 0; k < cols; ++k) {
                b[r * cols + k] -= f * b[c * cols + k];
            }
        }
    }
    for (int c = n - 1; c >= 0; --c) {
        for (int k = 0; k < cols; ++k) {
            double s = b[c * cols + k];
            for (int j = c + 1; j < n; ++j) {
                s -= A[c * n + j] * b[j * cols + k];
            }
            b[c * cols + k] = s / A[c * n + c];
        }
    }
    return true;
}

/** The normal equations of one fit: symmetric, non-zero only within cyclic distance w. */
class Normal {
public:
    Normal(int n, int w, bool closed) : m_n(n), m_w(w), m_closed(closed), m_a(size_t(n) * (2 * w + 1), 0.),
        m_rx(n, 0.), m_ry(n, 0.) {}

    /** Canonical offset of j from i, or a value > w when they never couple. */
    int offset(int i, int j) const {
        int d = j - i;
        if (m_closed) {
            if (d > m_n / 2) {
                d -= m_n;
            } else if (d < -((m_n - 1) / 2)) {
                d += m_n;
            }
        }
        return d;
    }
    bool couples(int i, int j) const { return std::abs(offset(i, j)) <= m_w; }
    double& at(int i, int j) { return m_a[size_t(i) * (2 * m_w + 1) + (offset(i, j) + m_w)]; }
    double get(int i, int j) const {
        const int d = offset(i, j);
        return std::abs(d) <= m_w ? m_a[size_t(i) * (2 * m_w + 1) + (d + m_w)] : 0.;
    }
    int n() const { return m_n; }
    int w() const { return m_w; }
    std::vector<double>& rx() { return m_rx; }
    std::vector<double>& ry() { return m_ry; }

    /** Indices j coupled with i (each once). */
    template <class F>
    void forRow(int i, F f) const {
        for (int d = -m_w; d <= m_w; ++d) {
            int j = i + d;
            if (m_closed) {
                j = ((j % m_n) + m_n) % m_n;
            } else if (j < 0 || j >= m_n) {
                continue;
            }
            if (offset(i, j) == d) {
                f(j);
            }
        }
    }

private:
    int m_n;
    int m_w;
    bool m_closed;
    std::vector<double> m_a;
    std::vector<double> m_rx;
    std::vector<double> m_ry;
};

/**
 * Solves the normal equations with the fixed control points given (fixed[i] != 0 => P[i] stays).
 * Closed curves have their fixed points in whole corner blocks of p, or none.
 */
inline bool solve(Normal& N, const std::vector<char>& fixed, std::vector<Vec>& P, bool closed) {
    const int n = N.n();
    const int w = N.w();
    std::vector<double> rx = N.rx();
    std::vector<double> ry = N.ry();
    for (int i = 0; i < n; ++i) {
        if (fixed[i]) {
            continue;
        }
        N.forRow(i, [&](int j) {
            if (fixed[j]) {
                rx[i] -= N.get(i, j) * P[j].x;
                ry[i] -= N.get(i, j) * P[j].y;
            }
        });
    }
    int firstFixed = -1;
    for (int i = 0; i < n; ++i) {
        if (fixed[i]) {
            firstFixed = i;
            break;
        }
    }
    if (closed && firstFixed < 0) {
        // Schur complement on the last p unknowns (Z); the rest (Y) is banded without wrap.
        const int nz = w;
        const int ny = n - nz;
        if (ny < nz + 1) {
            return false;
        }
        const int W = w + 1;
        std::vector<double> a(size_t(ny) * W, 0.);
        std::vector<double> B(size_t(ny) * nz, 0.);
        std::vector<double> C(size_t(nz) * nz, 0.);
        for (int i = 0; i < n; ++i) {
            N.forRow(i, [&](int j) {
                const double v = N.get(i, j);
                if (i < ny && j < ny) {
                    if (j <= i && i - j <= w) {
                        a[size_t(i) * W + (i - j)] = v;
                    }
                } else if (i < ny) {
                    B[size_t(i) * nz + (j - ny)] = v;
                } else if (j >= ny) {
                    C[size_t(i - ny) * nz + (j - ny)] = v;
                }
            });
        }
        if (!bandCholesky(a, ny, w)) {
            return false;
        }
        std::vector<double> ax(rx.begin(), rx.begin() + ny);
        std::vector<double> ay(ry.begin(), ry.begin() + ny);
        bandSolve(a, ny, w, ax.data());
        bandSolve(a, ny, w, ay.data());
        std::vector<double> G(size_t(ny) * nz);
        std::vector<double> col(ny);
        for (int k = 0; k < nz; ++k) {
            for (int i = 0; i < ny; ++i) {
                col[i] = B[size_t(i) * nz + k];
            }
            bandSolve(a, ny, w, col.data());
            for (int i = 0; i < ny; ++i) {
                G[size_t(i) * nz + k] = col[i];
            }
        }
        std::vector<double> S(C);
        std::vector<double> rz(size_t(nz) * 2);
        for (int k = 0; k < nz; ++k) {
            for (int l = 0; l < nz; ++l) {
                double s = 0.;
                for (int i = 0; i < ny; ++i) {
                    s += B[size_t(i) * nz + k] * G[size_t(i) * nz + l];
                }
                S[size_t(k) * nz + l] -= s;
            }
            double sx = rx[ny + k];
            double sy = ry[ny + k];
            for (int i = 0; i < ny; ++i) {
                sx -= B[size_t(i) * nz + k] * ax[i];
                sy -= B[size_t(i) * nz + k] * ay[i];
            }
            rz[size_t(k) * 2] = sx;
            rz[size_t(k) * 2 + 1] = sy;
        }
        if (!denseSolve(S, nz, rz, 2)) {
            return false;
        }
        for (int k = 0; k < nz; ++k) {
            P[ny + k] = {rz[size_t(k) * 2], rz[size_t(k) * 2 + 1]};
        }
        for (int i = 0; i < ny; ++i) {
            double x = ax[i];
            double y = ay[i];
            for (int k = 0; k < nz; ++k) {
                x -= G[size_t(i) * nz + k] * rz[size_t(k) * 2];
                y -= G[size_t(i) * nz + k] * rz[size_t(k) * 2 + 1];
            }
            P[i] = {x, y};
        }
        return true;
    }
    // Free unknowns in working order: closed starts right after a fixed point, so a fixed block cuts the cycle.
    std::vector<int> order;
    std::vector<int> pos(n, -1);
    const int start = closed ? (firstFixed + 1) % n : 0;
    for (int k = 0; k < n; ++k) {
        const int i = (start + k) % n;
        if (!fixed[i]) {
            pos[i] = int(order.size());
            order.push_back(i);
        }
    }
    const int f = int(order.size());
    if (f == 0) {
        return true;
    }
    int bw = 0;
    for (int i : order) {
        N.forRow(i, [&](int j) {
            if (pos[j] >= 0) {
                bw = std::max(bw, std::abs(pos[i] - pos[j]));
            }
        });
    }
    const int W = bw + 1;
    std::vector<double> a(size_t(f) * W, 0.);
    for (int i : order) {
        N.forRow(i, [&](int j) {
            if (pos[j] >= 0 && pos[j] <= pos[i]) {
                a[size_t(pos[i]) * W + (pos[i] - pos[j])] = N.get(i, j);
            }
        });
    }
    if (!bandCholesky(a, f, bw)) {
        return false;
    }
    std::vector<double> x(f);
    std::vector<double> y(f);
    for (int k = 0; k < f; ++k) {
        x[k] = rx[order[k]];
        y[k] = ry[order[k]];
    }
    bandSolve(a, f, bw, x.data());
    bandSolve(a, f, bw, y.data());
    for (int k = 0; k < f; ++k) {
        P[order[k]] = {x[k], y[k]};
    }
    return true;
}

/** The polyline as data for the fit: dense points with their length along it, corners flagged. */
struct Data {
    std::vector<Vec> q;
    std::vector<double> s;
    std::vector<char> corner;
    double length = 0.;
    double eps = 0.;
};

/** Dedupes, finds corners, rotates a closed polyline to its first corner, and subdivides every chord. */
inline bool prepare(const std::vector<Vec>& input, const std::vector<char>& vertexFlags, bool closed, double tol,
                    Data& d) {
    if (input.empty()) {
        return false;
    }
    double minX = input[0].x, maxX = minX, minY = input[0].y, maxY = minY, maxAbs = 0.;
    for (const Vec& v : input) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
            return false;
        }
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y);
        maxY = std::max(maxY, v.y);
        maxAbs = std::max({maxAbs, std::abs(v.x), std::abs(v.y)});
    }
    const double diag = std::hypot(maxX - minX, maxY - minY);
    if (!(diag > 0.)) {
        return false;
    }
    // points closer than a rounding error are one (a closed polyline far from the origin, see lc_biarcfit.h)
    d.eps = std::max(1e-9 * diag, 1e-13 * maxAbs);
    std::vector<Vec> pts;
    std::vector<char> vtx;
    for (size_t i = 0; i < input.size(); ++i) {
        const char f = vertexFlags.size() == input.size() ? vertexFlags[i] : 1;
        if (!pts.empty() && (input[i] - pts.back()).norm() <= d.eps) {
            vtx.back() = char(vtx.back() | f);
            continue;
        }
        pts.push_back(input[i]);
        vtx.push_back(f);
    }
    if (closed) {
        while (pts.size() > 1 && (pts.back() - pts.front()).norm() <= d.eps) {
            vtx.front() = char(vtx.front() | vtx.back());
            pts.pop_back();
            vtx.pop_back();
        }
    }
    const size_t n = pts.size();
    if (n < 2 || (closed && n < 3)) {
        return false;
    }
    std::vector<char> corner(n, 0);
    std::vector<double> bend(n, 0.); // where the refinement towards a bent vertex starts; 0 = none
    for (size_t i = 0; i < n; ++i) {
        if (!vtx[i] || (!closed && (i == 0 || i + 1 == n))) {
            continue;
        }
        const Vec a = pts[(i + n - 1) % n];
        const Vec c = pts[(i + 1) % n];
        const Vec din = pts[i] - a;
        const Vec dout = c - pts[i];
        const double turn = std::atan2(std::abs(din.cross(dout)), din.dot(dout));
        corner[i] = turn > kCornerAngle;
        bend[i] = turn > 1e-6 ? std::max(tol / 4., 2. * tol / turn) : 0.;
    }
    if (closed) {
        const auto it = std::find(corner.begin(), corner.end(), 1);
        if (it != corner.end()) {
            const long r = long(it - corner.begin());
            std::rotate(pts.begin(), pts.begin() + r, pts.end());
            std::rotate(corner.begin(), corner.begin() + r, corner.end());
            std::rotate(bend.begin(), bend.begin() + r, bend.end());
        }
    }
    double total = 0.;
    const size_t chords = closed ? n : n - 1;
    for (size_t i = 0; i < chords; ++i) {
        total += (pts[(i + 1) % n] - pts[i]).norm();
    }
    const double h = total / 256.;
    d = Data{{}, {}, {}, 0., d.eps};
    double s = 0.;
    std::vector<double> fr;
    for (size_t i = 0; i < chords; ++i) {
        const Vec& a = pts[i];
        const Vec& b = pts[(i + 1) % n];
        const double len = (b - a).norm();
        d.q.push_back(a);
        d.s.push_back(s);
        d.corner.push_back(corner[i]);
        fr.clear();
        // at least 4 parts: the data -> curve check only sees these points, and on a dense polyline the curve
        // is farthest from a chord near its middle (with 1 part the fit said 0.00465 where it was 0.00605)
        const int c = std::clamp(int(std::ceil(len / h)), 4, 64);
        for (int k = 1; k < c; ++k) {
            fr.push_back(double(k) / c);
        }
        // denser towards bent vertices: the curve has to turn within a tolerance of them
        if (bend[i] > 0.) {
            for (double t = bend[i]; t < len / 2.; t *= 2.) {
                fr.push_back(t / len);
            }
        }
        if (bend[(i + 1) % n] > 0.) {
            for (double t = bend[(i + 1) % n]; t < len / 2.; t *= 2.) {
                fr.push_back(1. - t / len);
            }
        }
        std::sort(fr.begin(), fr.end());
        double last = 0.;
        for (double f : fr) {
            if (f - last > 1e-9 && f < 1. - 1e-9) {
                d.q.push_back(a + (b - a) * f);
                d.s.push_back(s + len * f);
                d.corner.push_back(0);
                last = f;
            }
        }
        s += len;
    }
    if (!closed) {
        d.q.push_back(pts[n - 1]);
        d.s.push_back(s);
        d.corner.push_back(0);
    }
    d.length = s;
    return s > 0.;
}

/** Least squares for the free control points at the data parameters u, plus a whisper of smoothing. */
inline bool leastSquares(const Curve& cv, const Data& d, const std::vector<double>& u, const std::vector<char>& fixed,
                         std::vector<Vec>& P) {
    const int n = cv.count();
    const int p = cv.degree();
    const bool closed = cv.closed();
    Normal nm(n, p, closed);
    int idx[4];
    double bs[3][4];
    double diag = 0.;
    for (size_t j = 0; j < d.q.size(); ++j) {
        cv.basis(u[j], idx, bs, 0);
        for (int a = 0; a <= p; ++a) {
            for (int c = 0; c <= p; ++c) {
                nm.at(idx[a], idx[c]) += bs[0][a] * bs[0][c];
            }
            nm.rx()[idx[a]] += bs[0][a] * d.q[j].x;
            nm.ry()[idx[a]] += bs[0][a] * d.q[j].y;
            diag += bs[0][a] * bs[0][a];
        }
    }
    // differences of order min(p, 2) keep spans without data solvable; too weak to bend anything else
    const double lambda = 1e-6 * diag / n;
    const int order = std::min(p, 2);
    const int rows = closed ? n : n - order;
    for (int r = 0; r < rows; ++r) {
        const int id[3] = {r, (r + 1) % n, (r + 2) % n};
        const double c1[2] = {-1., 1.};
        const double c2[3] = {1., -2., 1.};
        const double* c = order == 1 ? c1 : c2;
        for (int a = 0; a <= order; ++a) {
            for (int e = 0; e <= order; ++e) {
                nm.at(id[a], id[e]) += lambda * c[a] * c[e];
            }
        }
    }
    return solve(nm, fixed, P, closed);
}

/**
 * Parameters by length: a data point at fraction f of its piece along the polyline gets the parameter at
 * fraction f of the curve's length over that piece. Monotone, so the data can never fold back - the
 * Newton projection fed back into the fit did, and the spline blew up as spans were added.
 */
inline void reparametrize(const Curve& cv, const std::vector<Vec>& P, const Data& d, const std::vector<int>& piece,
                          const std::vector<double>& bounds, const std::vector<double>& ub, int samples,
                          std::vector<double>& u) {
    const int N = int(d.q.size());
    const int pieces = int(bounds.size()) - 1;
    std::vector<double> tu;
    std::vector<double> ta;
    int j = 0;
    for (int k = 0; k < pieces; ++k) {
        const double ua = ub[k];
        const double ue = ub[k + 1];
        const int cnt = std::max(1, int(std::lround((ue - ua) * samples)));
        tu.assign(size_t(cnt) + 1, ua);
        ta.assign(size_t(cnt) + 1, 0.);
        Vec prev;
        cv.eval(P, ua, &prev);
        for (int i = 1; i <= cnt; ++i) {
            tu[i] = i == cnt ? ue : ua + (ue - ua) * i / cnt;
            Vec c;
            cv.eval(P, tu[i], &c); // closed: u = spans wraps to the start, closing the loop
            ta[i] = ta[i - 1] + (c - prev).norm();
            prev = c;
        }
        const double total = ta[cnt];
        const double sa = bounds[k];
        const double sb = bounds[k + 1];
        int at = 0;
        for (; j < N && piece[j] == k; ++j) {
            const double f = sb > sa ? (d.s[j] - sa) / (sb - sa) : 0.;
            if (f <= 0. || !(total > 0.)) {
                u[j] = ua;
                continue;
            }
            if (f >= 1.) {
                u[j] = ue;
                continue;
            }
            const double target = f * total;
            while (at + 1 < cnt && ta[at + 1] < target) {
                ++at;
            }
            const double seg = ta[at + 1] - ta[at];
            const double g = seg > 0. ? std::clamp((target - ta[at]) / seg, 0., 1.) : 0.;
            u[j] = tu[at] + (tu[at + 1] - tu[at]) * g;
        }
    }
}

} // namespace detail

/**
 * Fits a spline of `degree` (1..3) to the polyline through `pts` (closed: no repeated first point needed).
 * `vertex[i]` marks real polyline vertices (corner candidates); points between them, such as an arc
 * sampled by the caller, have 0. An empty `vertex` makes every point a vertex. The result has no control
 * points when the polyline has too few distinct points.
 */
inline Result fit(const std::vector<Vec>& pts, const std::vector<char>& vertex, bool closed, int degree,
                  double tolerance, std::size_t maxControl = 4096) {
    using namespace detail;
    Result res;
    const int p = std::clamp(degree, 1, 3);
    res.degree = p;
    res.closed = closed;
    if (!(tolerance > 0.) || !std::isfinite(tolerance)) {
        tolerance = 0.01;
    }
    Data d;
    if (!prepare(pts, vertex, closed, tolerance, d)) {
        return res;
    }
    const int N = int(d.q.size());
    const double L = d.length;

    // pieces between the corners and the open ends, by length along the polyline; a closed polyline has
    // its first corner at data point 0 (prepare rotates to it), and without corners it is one cyclic piece
    std::vector<double> bounds{0.};
    for (int j = 1; j < N; ++j) {
        if (d.corner[j]) {
            bounds.push_back(d.s[j]);
        }
    }
    bounds.push_back(L);
    const int pieces = int(bounds.size()) - 1;
    const bool cyclic = closed && !d.corner[0];
    // breakpoints: knot k sits at length b[k]; bfix marks the piece bounds
    std::vector<double> b{0.};
    std::vector<char> bfix{1};
    std::vector<int> minSpans; // per piece: the least it may keep (knot removal)
    for (int k = 0; k < pieces; ++k) {
        int spans = p; // between two corners: the fixed blocks of p must not overlap
        if (cyclic) {
            spans = std::max(2 * p + 2, 4);
        } else if (!closed && pieces == 1) {
            spans = std::max(1, 3 - p);
        } else if (!closed && (k == 0 || k == pieces - 1)) {
            spans = 1;
        } else if (closed && pieces == 1) {
            spans = std::max(p + 2, 3);
        } else if (closed && pieces == 2) {
            spans = std::max(p, 2);
        }
        minSpans.push_back(spans);
        for (int i = 1; i <= spans; ++i) {
            // the bound itself exactly, so a corner's data point lands on its knot
            b.push_back(i == spans ? bounds[k + 1] : bounds[k] + (bounds[k + 1] - bounds[k]) * i / spans);
            bfix.push_back(char(i == spans));
        }
    }
    std::vector<int> piece(N);
    for (int j = 0, k = 0; j < N; ++j) {
        while (k + 1 < pieces && d.s[j] >= bounds[k + 1]) {
            ++k;
        }
        piece[j] = k;
    }
    auto fixedParam = [&](int j) { return d.corner[j] || (!closed && (j == 0 || j == N - 1)); };
    std::vector<double> u(N);
    for (int j = 0, k = 0; j < N; ++j) {
        const int m = int(b.size()) - 1;
        while (k + 1 < m && d.s[j] >= b[k + 1]) {
            ++k;
        }
        const double len = b[k + 1] - b[k];
        u[j] = k + (len > 0. ? std::clamp((d.s[j] - b[k]) / len, 0., 1.) : 0.);
    }

    constexpr int kSamples = 16; // curve samples per span: length table, curve -> polyline check
    struct Round {
        bool ok = false;
        std::vector<Vec> P;
        std::vector<double> err; // per span
        double maxErr = std::numeric_limits<double>::infinity();
    };
    // One fit at the breakpoints b: least squares and length parameters twice, then the error per span both
    // ways. u comes in as the parameters to start from and goes out as the length parameters of the fit.
    auto evaluate = [&](const std::vector<char>& bfix, std::vector<double>& u) {
        Round r;
        const int m = int(bfix.size()) - 1;
        const Curve cv(p, closed, m);
        const int n = cv.count();
        std::vector<double> ub;
        for (int k = 0; k <= m; ++k) {
            if (bfix[k]) {
                ub.push_back(k);
            }
        }
        // fixed control points: the open ends and the corner blocks
        std::vector<Vec> P(n);
        std::vector<char> fixed(n, 0);
        if (!closed) {
            P[0] = d.q.front();
            P[n - 1] = d.q.back();
            fixed[0] = fixed[n - 1] = 1;
        }
        for (int j = 0; j < N; ++j) {
            if (d.corner[j]) {
                const int k = int(std::lround(u[j]));
                for (int a = 0; a < p; ++a) {
                    const int i = closed ? (k + a) % n : k + a;
                    P[i] = d.q[j];
                    fixed[i] = 1;
                }
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            if (!leastSquares(cv, d, u, fixed, P)) {
                return r;
            }
            reparametrize(cv, P, d, piece, bounds, ub, kSamples, u);
        }
        // data -> curve (Newton from the length parameter), curve -> polyline
        std::vector<double> err(m, 0.);
        std::vector<std::vector<int>> bucket(m);
        for (int j = 0; j < N; ++j) {
            const int k = cv.spanOf(u[j]);
            bucket[k].push_back(j);
            Vec c;
            cv.eval(P, u[j], &c);
            double e = (c - d.q[j]).norm();
            if (!fixedParam(j) && e > 0.) {
                const double lo = cyclic ? u[j] - 1. : std::max(u[j] - 1., ub[piece[j]]);
                const double hi = cyclic ? u[j] + 1. : std::min(u[j] + 1., ub[piece[j] + 1]);
                double v = u[j];
                for (int it = 0; it < 4; ++it) {
                    Vec d1, d2;
                    cv.eval(P, v, &c, &d1, &d2);
                    const Vec rr = c - d.q[j];
                    double den = d1.dot(d1) + rr.dot(d2);
                    if (!(den > 0.)) {
                        den = d1.dot(d1);
                    }
                    if (!(den > 0.)) {
                        break;
                    }
                    const double step = std::clamp(-rr.dot(d1) / den, -0.25, 0.25);
                    v = std::clamp(v + step, lo, hi);
                    if (std::abs(step) < 1e-12) {
                        break;
                    }
                }
                cv.eval(P, v, &c);
                e = std::min(e, (c - d.q[j]).norm());
            }
            err[k] = std::max(err[k], e);
        }
        auto spanAt = [&](int k) { return closed ? ((k % m) + m) % m : k; };
        std::vector<int> win;
        for (int k = 0; k < m; ++k) {
            win.clear();
            for (int w = 1; win.empty() && w <= m; ++w) {
                for (int o = -w; o <= w; ++o) {
                    const int kk = spanAt(k + o);
                    if (kk >= 0 && kk < m) {
                        win.insert(win.end(), bucket[kk].begin(), bucket[kk].end());
                    }
                }
            }
            for (int t = 0; t <= kSamples; ++t) {
                Vec c;
                cv.eval(P, k + double(t) / kSamples, &c);
                double near = std::numeric_limits<double>::infinity();
                for (int j : win) {
                    for (int o = -1; o <= 0; ++o) {
                        int j0 = j + o;
                        int j1 = j0 + 1;
                        if (closed) {
                            j0 = (j0 + N) % N;
                            j1 = j1 % N;
                        } else if (j0 < 0 || j1 >= N) {
                            continue;
                        }
                        near = std::min(near, distToSegment(c, d.q[j0], d.q[j1]));
                    }
                }
                if (std::isfinite(near)) {
                    err[k] = std::max(err[k], near);
                }
            }
        }
        r.ok = true;
        r.P = std::move(P);
        r.maxErr = *std::max_element(err.begin(), err.end());
        r.err = std::move(err);
        return r;
    };

    Round best;
    std::vector<double> bestB;
    std::vector<char> bestF;
    std::vector<double> bestU;
    const double minSpan = std::max(10. * d.eps, tolerance * 1e-3);
    for (int round = 0; round < 64; ++round) {
        const int m = int(b.size()) - 1;
        Round r = evaluate(bfix, u);
        if (!r.ok) {
            LC_SPLINEFIT_TRACE("  round %d: solve failed\n", round);
            break;
        }
        const Curve cv(p, closed, m);
        const int n = cv.count();
        LC_SPLINEFIT_TRACE("  round %d spans %d control %d maxErr %g at span %d\n", round, m, n, r.maxErr,
                           int(std::max_element(r.err.begin(), r.err.end()) - r.err.begin()));
        const bool done = r.maxErr <= tolerance;
        if (r.maxErr < best.maxErr) {
            best = r;
            bestB = b;
            bestF = bfix;
            bestU = u;
        }
        if (done) {
            break;
        }
        // halve the spans still off, worst first, within maxControl
        std::vector<int> bad;
        for (int k = 0; k < m; ++k) {
            if (r.err[k] > tolerance && b[k + 1] - b[k] > minSpan) {
                bad.push_back(k);
            }
        }
        std::sort(bad.begin(), bad.end(), [&](int x, int y) { return r.err[x] > r.err[y]; });
        const long room = long(maxControl) - long(n);
        if (bad.empty() || room <= 0) {
            break;
        }
        if (long(bad.size()) > room) {
            bad.resize(size_t(room));
        }
        std::vector<char> split(m, 0);
        for (int k : bad) {
            split[k] = 1;
        }
        // 2:1 balance: after the split no span is more than twice as long (along the polyline) as a neighbour.
        // The knots are uniform, so neighbours of very different lengths force the curve's parameter speed to
        // jump at the knot; without the balance more spans made the fit worse (a 2000-point spiral reached
        // 2236 spans and 9.3 off, a 400-segment sine 247 control points where 50 do - Python trials, step 22).
        std::vector<double> len(m);
        for (int k = 0; k < m; ++k) {
            len[k] = b[k + 1] - b[k];
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (int k = 0; k < m; ++k) {
                for (int o = -1; o <= 1 && !split[k]; o += 2) {
                    int k2 = k + o;
                    if (closed) {
                        k2 = (k2 + m) % m;
                    } else if (k2 < 0 || k2 >= m) {
                        continue;
                    }
                    if (len[k] > 2. * (len[k2] / (split[k2] ? 2. : 1.)) * (1. + 1e-9)) {
                        split[k] = 1;
                        changed = true;
                    }
                }
            }
        }
        if (n + std::count(split.begin(), split.end(), char(1)) > long(maxControl)) {
            break;
        }
        std::vector<double> nb;
        std::vector<char> nf;
        std::vector<double> newStart(m);
        for (int k = 0; k < m; ++k) {
            newStart[k] = double(nb.size());
            nb.push_back(b[k]);
            nf.push_back(bfix[k]);
            if (split[k]) {
                nb.push_back(0.5 * (b[k] + b[k + 1]));
                nf.push_back(0);
            }
        }
        nb.push_back(b[m]);
        nf.push_back(bfix[m]);
        // carry the parameters over (monotone): a halved span maps its [0, 1] onto two
        for (int j = 0; j < N; ++j) {
            int k = cv.spanOf(u[j]);
            double f = cv.wrap(u[j]) - k;
            if (!closed && u[j] >= m) {
                k = m - 1;
                f = 1.;
            }
            u[j] = newStart[k] + (split[k] ? 2. * f : f);
        }
        b.swap(nb);
        bfix.swap(nf);
    }
    if (best.P.empty()) {
        return res;
    }
    if (best.maxErr <= tolerance) {
        // Knot removal: halving overshoots (whole spans at a time), so one sweep over the breakpoints, easiest
        // first, drops each one whose removal still fits within the tolerance. Python trials on the step-22
        // test drawing: a slot 84 -> 60 control points, a coarse sine 145 -> 111; a second sweep gained 1.
        // The work is bounded: every try is a whole fit over the data.
        b = bestB;
        bfix = bestF;
        u = bestU;
        const int m0 = int(b.size()) - 1;
        std::vector<int> order;
        for (int k = 1; k < m0; ++k) {
            if (!bfix[k]) {
                order.push_back(k);
            }
        }
        std::sort(order.begin(), order.end(), [&](int x, int y) {
            return std::max(best.err[x - 1], best.err[x]) < std::max(best.err[y - 1], best.err[y]);
        });
        std::vector<double> keys;
        for (int k : order) {
            keys.push_back(b[k]);
        }
        // ~1 s on this machine at most: a 2000-point spiral took 2 s with twice this budget, most of it here
        const long budget = std::max(16L, long(2e6 / std::max(N, 1)));
        long tries = 0;
        for (double key : keys) {
            if (tries >= budget) {
                break;
            }
            const int k = int(std::lower_bound(b.begin(), b.end(), key) - b.begin());
            if (k <= 0 || k >= int(b.size()) - 1 || b[k] != key || bfix[k]) {
                continue;
            }
            // the piece around breakpoint k keeps its least number of spans
            int a = k - 1;
            while (!bfix[a]) {
                --a;
            }
            int c = k + 1;
            while (!bfix[c]) {
                ++c;
            }
            int pid = 0;
            for (int i = 1; i <= a; ++i) {
                pid += bfix[i] ? 1 : 0;
            }
            if (c - a <= minSpans[pid]) {
                continue;
            }
            std::vector<char> nf(bfix);
            nf.erase(nf.begin() + k);
            std::vector<double> uu(u);
            for (double& v : uu) {
                v = v >= k + 1 ? v - 1. : (v >= k - 1 ? (k - 1) + (v - (k - 1)) / 2. : v);
            }
            ++tries;
            Round r = evaluate(nf, uu);
            if (r.ok && r.maxErr <= tolerance) {
                b.erase(b.begin() + k);
                bfix.swap(nf);
                u.swap(uu);
                best = std::move(r);
            }
        }
        LC_SPLINEFIT_TRACE("  removal: %ld tries, spans %d -> %d, maxErr %g\n", tries, m0, int(b.size()) - 1,
                           best.maxErr);
    }
    std::vector<Vec> control = best.P;
    const double bestErr = best.maxErr;
    if (closed) {
        // RS_Spline::update takes a closed spline whose first p points equal its last p as already wrapped
        auto looksWrapped = [&](const std::vector<Vec>& c) {
            if (int(c.size()) < 2 * p) {
                return false;
            }
            for (int a = 0; a < p; ++a) {
                if ((c[a] - c[c.size() - p + a]).norm() > 0.) {
                    return false;
                }
            }
            return true;
        };
        for (size_t t = 0; t < control.size() && looksWrapped(control); ++t) {
            std::rotate(control.begin(), control.begin() + 1, control.end());
        }
    }
    res.control = control;
    res.maxDeviation = bestErr;
    res.reached = bestErr <= tolerance;
    return res;
}

} // namespace splinefit
} // namespace lc

#endif

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
 * Context: arcs for "explode spline" in Arcs mode (2026-11-02, plans-win librecad-factory-drawings step 21:
 * user asked whether a spline can explode into arc pieces). AutoCAD does the same with PLINECONVERTMODE=1;
 * CNC follows arcs (G02/G03) directly, with fewer pieces and no corners between them.
 * How: biarcs - two arcs that share a tangent where they meet - between dense points of the curve. A span
 * is halved until the deviation in both directions (curve points -> arcs, arc points -> the dense polyline)
 * is within the tolerance. Both pieces at a joint take the tangent of the same dense point, so the result
 * has no corners (G1) except where the curve itself has one.
 * Prototype with numbers on the real control_motor splines: dxf-tools/rtsuite/splinepts/biarc_proto.py.
 * Gotchas:
 *  - the second arc of a biarc is built backwards from the end point (tangent reversed) and then turned
 *    round; reading its tangents without turning it round shows a 180 degree "corner" (prototype bug).
 *  - the arc -> curve direction is measured against the dense polyline, which is itself off the curve by
 *    its sagitta; the caller picks a density whose sagitta is a small part of the tolerance.
 *  - no LibreCAD types here, so test/lc_biarcfit_test.cpp builds on its own; but it is included after Qt
 *    headers, so no names that Qt defines as macros (emit, signals, slots) - "emit(" broke the first build.
 */
#ifndef LC_BIARCFIT_H
#define LC_BIARCFIT_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace lc {
namespace biarc {

struct Vec {
    double x = 0.;
    double y = 0.;
};

inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
inline Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
inline Vec operator-(Vec a) { return {-a.x, -a.y}; }
inline Vec operator*(Vec a, double s) { return {a.x * s, a.y * s}; }
inline double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }
inline double norm(Vec a) { return std::hypot(a.x, a.y); }
inline Vec unit(Vec a) {
    const double n = norm(a);
    return n > 0. ? a * (1. / n) : a;
}

constexpr double kPi = 3.14159265358979323846;

/** Angle between neighbouring dense chords (radians) above which the curve has a corner there. */
constexpr double kCornerAngle = 0.5;

/** A straight piece or an arc, running from start to end. */
struct Piece {
    Vec start;
    Vec end;
    bool arc = false;
    Vec center;
    double radius = 0.;
    double angle1 = 0.; ///< direction of start seen from the center
    double sweep = 0.;  ///< signed, > 0 counter-clockwise

    /** DXF bulge: tan(sweep / 4), 0 for a straight piece. */
    double bulge() const { return arc ? std::tan(sweep / 4.) : 0.; }

    /** Point at fraction f (0 = start, 1 = end). */
    Vec pointAt(double f) const {
        if (!arc) {
            return start + (end - start) * f;
        }
        const double a = angle1 + sweep * f;
        return {center.x + radius * std::cos(a), center.y + radius * std::sin(a)};
    }

    /** Unit tangent in the running direction at fraction f. */
    Vec tangentAt(double f) const {
        if (!arc) {
            return unit(end - start);
        }
        const double a = angle1 + sweep * f;
        const Vec radial{std::cos(a), std::sin(a)};
        return sweep > 0. ? Vec{-radial.y, radial.x} : Vec{radial.y, -radial.x};
    }
};

struct Result {
    std::vector<Piece> pieces;
    /** Largest deviation measured: curve points to the pieces and piece points to the dense polyline. */
    double maxDeviation = 0.;
};

namespace detail {

inline double wrap2pi(double a) {
    a = std::fmod(a, 2. * kPi);
    return a < 0. ? a + 2. * kPi : a;
}

inline double distToSegment(Vec p, Vec a, Vec b) {
    const Vec ab = b - a;
    const double len2 = dot(ab, ab);
    double t = len2 > 0. ? dot(p - a, ab) / len2 : 0.;
    t = std::clamp(t, 0., 1.);
    return norm(p - (a + ab * t));
}

inline double distToPiece(Vec p, const Piece& pc) {
    if (!pc.arc) {
        return distToSegment(p, pc.start, pc.end);
    }
    const double ang = std::atan2(p.y - pc.center.y, p.x - pc.center.x);
    const double rel = pc.sweep > 0. ? wrap2pi(ang - pc.angle1) : wrap2pi(pc.angle1 - ang);
    if (rel <= std::abs(pc.sweep)) {
        return std::abs(norm(p - pc.center) - pc.radius);
    }
    return std::min(norm(p - pc.start), norm(p - pc.end));
}

/**
 * Arc from p, leaving with unit tangent t, to q (a straight piece when q lies on the tangent line ahead).
 * Returns false when q lies on the tangent line behind p.
 */
inline bool arcFrom(Vec p, Vec t, Vec q, Piece& pc) {
    pc = Piece{};
    pc.start = p;
    pc.end = q;
    const Vec chord = q - p;
    const double len = norm(chord);
    if (!(len > 0.)) {
        return false;
    }
    const double sinTheta = cross(t, chord) / len; // the chord's angle from the tangent
    if (std::abs(sinTheta) < 1e-9) {
        return dot(t, chord) > 0.;
    }
    const double s = len / (2. * sinTheta); // signed radius, > 0: center on the left, counter-clockwise
    const Vec n{-t.y, t.x};
    pc.arc = true;
    pc.center = p + n * s;
    pc.radius = std::abs(s);
    pc.angle1 = std::atan2(p.y - pc.center.y, p.x - pc.center.x);
    const double a2 = std::atan2(q.y - pc.center.y, q.x - pc.center.x);
    pc.sweep = s > 0. ? wrap2pi(a2 - pc.angle1) : -wrap2pi(pc.angle1 - a2);
    return true;
}

inline Piece reversed(const Piece& pc) {
    Piece r = pc;
    r.start = pc.end;
    r.end = pc.start;
    if (pc.arc) {
        r.angle1 = pc.angle1 + pc.sweep;
        r.sweep = -pc.sweep;
    }
    return r;
}

/**
 * Equal-length biarc from p0 (unit tangent t0) to p1 (unit tangent t1): 1 piece (a straight one) or 2,
 * 0 when there is none (tangents pointing back, or an arc over half a circle - the span is split instead).
 */
inline int biarc(Vec p0, Vec t0, Vec p1, Vec t1, Piece out[2]) {
    const Vec v = p1 - p0;
    const double vv = dot(v, v);
    if (!(vv > 0.)) {
        return 0;
    }
    // |p0 + a t0 - (p1 - a t1)| = 2a  =>  d a^2 + 2 (v.(t0+t1)) a - v.v = 0 with d = 2 (1 - t0.t1);
    // the positive root written without the cancellation of (-b + sqrt(...)) / d, and valid for d = 0
    const double d = 2. * (1. - dot(t0, t1));
    const double vt = dot(v, t0 + t1);
    const double denom = vt + std::sqrt(std::max(vt * vt + d * vv, 0.));
    if (!(denom > 1e-12 * std::sqrt(vv))) {
        return 0;
    }
    const double alpha = vv / denom;
    const Vec pm = (p0 + t0 * alpha + p1 - t1 * alpha) * 0.5;
    Piece a;
    Piece back;
    if (!arcFrom(p0, t0, pm, a) || !arcFrom(p1, -t1, pm, back)) {
        return 0;
    }
    const Piece b = reversed(back);
    if ((a.arc && std::abs(a.sweep) > kPi) || (b.arc && std::abs(b.sweep) > kPi)) {
        return 0;
    }
    if (!a.arc && !b.arc) { // all in line: one straight piece
        out[0] = Piece{};
        out[0].start = p0;
        out[0].end = p1;
        return 1;
    }
    out[0] = a;
    out[1] = b;
    return 2;
}

/** Unit tangent at b of the parabola through a, b, c (chord length parameter). */
inline Vec tangentMid(Vec a, Vec b, Vec c) {
    const double d1 = norm(b - a);
    const double d2 = norm(c - b);
    if (!(d1 > 0.) || !(d2 > 0.)) {
        return unit(c - a);
    }
    return unit((b - a) * (d2 / (d1 * (d1 + d2))) + (c - b) * (d1 / (d2 * (d1 + d2))));
}

/** Unit tangent at a (the first point) of the parabola through a, b, c. */
inline Vec tangentStart(Vec a, Vec b, Vec c) {
    const double d1 = norm(b - a);
    const double d2 = norm(c - b);
    if (!(d1 > 0.) || !(d2 > 0.)) {
        return unit(b - a);
    }
    const double s = d1 + d2;
    return unit(a * (-(d1 + s) / (d1 * s)) + b * (s / (d1 * d2)) + c * (-d1 / (s * d2)));
}

class Fitter {
public:
    Fitter(const std::vector<Vec>& pts, bool cyclic, double tolerance)
        : m_pts(pts), m_cyclic(cyclic), m_tol(tolerance) {}

    Result run() {
        Result r;
        const size_t n = m_pts.size();
        if (n < 2) {
            return r;
        }
        computeTangents();
        std::vector<size_t> breaks{0};
        for (size_t i = 1; i + 1 < n; ++i) {
            if (m_corner[i]) {
                breaks.push_back(i);
            }
        }
        breaks.push_back(n - 1);
        for (size_t k = 1; k < breaks.size(); ++k) {
            fitSpan(breaks[k - 1], breaks[k], r);
        }
        return r;
    }

private:
    Vec at(long k) const {
        const long n = long(m_pts.size());
        if (m_cyclic) {
            const long m = n - 1; // the last point repeats the first
            return m_pts[size_t(((k % m) + m) % m)];
        }
        return m_pts[size_t(std::clamp(k, 0L, n - 1))];
    }

    void computeTangents() {
        const long n = long(m_pts.size());
        m_in.assign(size_t(n), Vec{});
        m_out.assign(size_t(n), Vec{});
        m_corner.assign(size_t(n), false);
        for (long i = 0; i < n; ++i) {
            const bool hasPrev = m_cyclic || i > 0;
            const bool hasNext = m_cyclic || i < n - 1;
            const Vec p = at(i);
            Vec tin;
            Vec tout;
            if (hasPrev && hasNext) {
                const Vec prev = at(i - 1);
                const Vec next = at(i + 1);
                const Vec a = p - prev;
                const Vec b = next - p;
                const double c = dot(a, b) / (norm(a) * norm(b));
                if (std::acos(std::clamp(c, -1., 1.)) > kCornerAngle) {
                    m_corner[size_t(i)] = true;
                    const bool twoBack = m_cyclic || i >= 2;
                    const bool twoAhead = m_cyclic || i + 2 <= n - 1;
                    tin = twoBack ? -tangentStart(p, prev, at(i - 2)) : unit(a);
                    tout = twoAhead ? tangentStart(p, next, at(i + 2)) : unit(b);
                } else {
                    tin = tout = tangentMid(prev, p, next);
                }
            } else if (!hasPrev) {
                tin = tout = n >= 3 ? tangentStart(p, at(1), at(2)) : unit(at(1) - p);
            } else {
                tin = tout = n >= 3 ? -tangentStart(p, at(n - 2), at(n - 3)) : unit(p - at(n - 2));
            }
            m_in[size_t(i)] = tin;
            m_out[size_t(i)] = tout;
        }
        if (m_cyclic) { // the closing point is the first point: same tangents, corner or not
            m_corner[size_t(n - 1)] = m_corner[0];
            m_in[size_t(n - 1)] = m_in[0];
            m_out[size_t(n - 1)] = m_out[0];
            if (m_corner[0]) { // the seam is a break anyway (first and last index)
                m_corner[0] = false;
                m_corner[size_t(n - 1)] = false;
            }
        }
    }

    /** Deviation of the pieces from the dense points a..b, stopping early once it is over the tolerance. */
    double deviation(size_t a, size_t b, const Piece* pieces, int count) const {
        double worst = 0.;
        for (size_t i = a + 1; i < b; ++i) {
            double d = distToPiece(m_pts[i], pieces[0]);
            for (int k = 1; k < count; ++k) {
                d = std::min(d, distToPiece(m_pts[i], pieces[k]));
            }
            worst = std::max(worst, d);
            if (worst > m_tol) {
                return worst;
            }
        }
        constexpr int kSamples = 8;
        for (int k = 0; k < count; ++k) {
            for (int j = 1; j < kSamples; ++j) {
                const Vec q = pieces[k].pointAt(double(j) / kSamples);
                double d = distToSegment(q, m_pts[a], m_pts[a + 1]);
                for (size_t s = a + 1; s < b; ++s) {
                    d = std::min(d, distToSegment(q, m_pts[s], m_pts[s + 1]));
                }
                worst = std::max(worst, d);
                if (worst > m_tol) {
                    return worst;
                }
            }
        }
        return worst;
    }

    void keep(const Piece* pieces, int count, double dev, Result& r) const {
        for (int k = 0; k < count; ++k) {
            r.pieces.push_back(pieces[k]);
        }
        r.maxDeviation = std::max(r.maxDeviation, dev);
    }

    void fitSpan(size_t a, size_t b, Result& r) const {
        Piece pieces[2];
        const int count = biarc(m_pts[a], m_out[a], m_pts[b], m_in[b], pieces);
        if (count > 0) {
            const double dev = deviation(a, b, pieces, count);
            if (dev <= m_tol) {
                keep(pieces, count, dev, r);
                return;
            }
        }
        if (b - a < 2) { // one dense chord left and no biarc within the tolerance: keep the chord
            Piece line;
            line.start = m_pts[a];
            line.end = m_pts[b];
            keep(&line, 1, 0., r);
            return;
        }
        const size_t m = a + (b - a) / 2;
        fitSpan(a, m, r);
        fitSpan(m, b, r);
    }

    const std::vector<Vec>& m_pts;
    const bool m_cyclic;
    const double m_tol;
    std::vector<Vec> m_in;
    std::vector<Vec> m_out;
    std::vector<bool> m_corner;
};

} // namespace detail

/**
 * Tangent-continuous arcs (and straight pieces where the curve is straight) through dense points of a
 * smooth curve, each within tolerance of the points. closed: the curve ends where it starts (the first
 * point is appended when it is missing) and the seam gets no corner either. Points at the same place are
 * dropped. The pieces run in the order of the points; the result is empty for fewer than 2 points.
 * "The same place": closer than 1e-9 of the curve's extent or 1e-13 of its largest coordinate. A closed
 * RS_Spline far from the origin ends a rounding error away from its start (seen: control_motor copy at
 * x = 3.48e6); taken as two points, that gap became a chord of ~1e-9 whose noisy direction counted as a
 * corner, and the seam got a 5e-5 rad kink.
 */
inline Result fit(std::vector<Vec> pts, bool closed, double tolerance) {
    double lo[2] = {0., 0.};
    double hi[2] = {0., 0.};
    double maxAbs = 0.;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec& p = pts[i];
        lo[0] = i == 0 ? p.x : std::min(lo[0], p.x);
        lo[1] = i == 0 ? p.y : std::min(lo[1], p.y);
        hi[0] = i == 0 ? p.x : std::max(hi[0], p.x);
        hi[1] = i == 0 ? p.y : std::max(hi[1], p.y);
        maxAbs = std::max({maxAbs, std::abs(p.x), std::abs(p.y)});
    }
    const double eps = std::max(1e-9 * std::hypot(hi[0] - lo[0], hi[1] - lo[1]), 1e-13 * maxAbs);
    std::vector<Vec> clean;
    clean.reserve(pts.size() + 1);
    for (const Vec& p : pts) {
        if (clean.empty() || norm(p - clean.back()) > eps) {
            clean.push_back(p);
        }
    }
    if (closed && clean.size() >= 3) {
        if (norm(clean.back() - clean.front()) > eps) {
            clean.push_back(clean.front());
        } else {
            clean.back() = clean.front(); // the loop ends exactly on its start
        }
    }
    if (clean.size() < 2 || (closed && clean.size() < 4)) {
        closed = false; // too few points for a loop: fit them as an open curve
    }
    detail::Fitter f(clean, closed, std::max(tolerance, 1e-12));
    return f.run();
}

} // namespace biarc
} // namespace lc

#endif

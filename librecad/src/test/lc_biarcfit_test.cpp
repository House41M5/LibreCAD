/*
 * Context: standalone test of lib/engine/lc_biarcfit.h (arcs for "explode spline" in Arcs mode, plans-win
 * librecad-factory-drawings step 21). Not part of src.pro; build and run by hand, e.g. from a VS x64 prompt:
 *   cl /nologo /EHsc /std:c++17 /O2 /I..\lib\engine lc_biarcfit_test.cpp /Fe:%TEMP%\biarc_test.exe && %TEMP%\biarc_test.exe
 * Prints PASS/FAIL per check and exits non-zero on any failure.
 * Deviation is checked against the exact curve (not the dense points the fit saw), both ways: exact curve
 * points -> pieces, and points along every piece -> a much denser exact polyline. The bulge check rebuilds
 * each arc from (start, end, bulge) with the formula of RS_Polyline::createVertex, i.e. what a polyline
 * made from the pieces will draw.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "lc_biarcfit.h"

using lc::biarc::Piece;
using lc::biarc::Vec;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;

using Curve = std::function<Vec(double)>; // parameter 0..1

// n chords; closed: the repeated first point is left to fit()
std::vector<Vec> sample(const Curve& c, int n, bool closed) {
    std::vector<Vec> pts;
    const int count = closed ? n : n + 1;
    for (int i = 0; i < count; ++i) {
        pts.push_back(c(double(i) / n));
    }
    return pts;
}

// atan2, not acos: acos of a dot product near 1 cannot show angles below ~1.5e-8 rad
double angleBetween(Vec a, Vec b) {
    return std::atan2(std::abs(lc::biarc::cross(a, b)), lc::biarc::dot(a, b));
}

// center from (start, end, bulge) as RS_Polyline::createVertex computes it
Vec centerFromBulge(Vec start, Vec end, double bulge) {
    const bool reversed = std::signbit(bulge);
    const double alpha = std::atan(std::abs(bulge)) * 4.0;
    const Vec middle = (start + end) * 0.5;
    const double dist = lc::biarc::norm(end - start) / 2.0;
    const double angle = std::atan2(end.y - start.y, end.x - start.x);
    const double radius = std::abs(dist / std::sin(alpha / 2.0));
    const double wu = std::abs(radius * radius - dist * dist);
    const double angleNew = reversed ? angle - kPi / 2 : angle + kPi / 2;
    const double h = (std::abs(alpha) > kPi) ? -std::sqrt(wu) : std::sqrt(wu);
    return middle + Vec{h * std::cos(angleNew), h * std::sin(angleNew)};
}

struct Stats {
    size_t pieces = 0;
    size_t arcs = 0;
    double devCurveToPieces = 0.;
    double devPiecesToCurve = 0.;
    double maxJoint = 0.;      // largest tangent jump at a joint (radians)
    double maxChainGap = 0.;   // largest gap between the end of a piece and the start of the next
    double maxBulgeCenter = 0.; // largest center difference rebuilt from the bulge
    double reported = 0.;
};

Stats measure(const lc::biarc::Result& r, const Curve& c, bool closed) {
    Stats s;
    s.pieces = r.pieces.size();
    s.reported = r.maxDeviation;
    const int dense = 6000;
    std::vector<Vec> exact;
    for (int i = 0; i <= dense; ++i) {
        exact.push_back(c(double(i) / dense));
    }
    for (const Vec& p : exact) {
        double d = 1e300;
        for (const Piece& pc : r.pieces) {
            d = std::fmin(d, lc::biarc::detail::distToPiece(p, pc));
        }
        s.devCurveToPieces = std::fmax(s.devCurveToPieces, d);
    }
    for (const Piece& pc : r.pieces) {
        if (pc.arc) {
            ++s.arcs;
            const Vec cb = centerFromBulge(pc.start, pc.end, pc.bulge());
            s.maxBulgeCenter = std::fmax(s.maxBulgeCenter, lc::biarc::norm(cb - pc.center));
        }
        for (int j = 0; j <= 40; ++j) {
            const Vec q = pc.pointAt(j / 40.);
            double d = 1e300;
            for (size_t i = 1; i < exact.size(); ++i) {
                d = std::fmin(d, lc::biarc::detail::distToSegment(q, exact[i - 1], exact[i]));
            }
            s.devPiecesToCurve = std::fmax(s.devPiecesToCurve, d);
        }
    }
    const size_t n = r.pieces.size();
    for (size_t k = 0; k + 1 < n || (closed && k < n && n > 1); ++k) {
        const Piece& a = r.pieces[k];
        const Piece& b = r.pieces[(k + 1) % n];
        s.maxChainGap = std::fmax(s.maxChainGap, lc::biarc::norm(a.end - b.start));
        s.maxJoint = std::fmax(s.maxJoint, angleBetween(a.tangentAt(1.), b.tangentAt(0.)));
    }
    return s;
}

void report(const char* name, const Stats& s, double tol) {
    std::printf("  %-22s tol %-7g pieces %4zu (arcs %4zu)  dev curve->pieces %.3g  pieces->curve %.3g  reported %.3g"
                "  joint %.2g rad  gap %.2g  bulge-center %.2g\n",
                name, tol, s.pieces, s.arcs, s.devCurveToPieces, s.devPiecesToCurve, s.reported, s.maxJoint,
                s.maxChainGap, s.maxBulgeCenter);
}

// deviation from the exact curve may exceed the tolerance a little: the fit sees dense points, not the curve
bool within(const Stats& s, double tol) {
    return s.devCurveToPieces <= tol * 1.05 && s.devPiecesToCurve <= tol * 1.05;
}

void smoothCase(const char* name, const Curve& c, int n, bool closed, double tol, size_t maxPieces) {
    const auto t0 = std::chrono::steady_clock::now();
    const lc::biarc::Result r = lc::biarc::fit(sample(c, n, closed), closed, tol);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const Stats s = measure(r, c, closed);
    report(name, s, tol);
    std::printf("  %-22s fit time %.2f ms for %d points\n", name, ms, n);
    char what[160];
    std::snprintf(what, sizeof what, "%s: within tolerance of the exact curve both ways", name);
    check(within(s, tol), what);
    std::snprintf(what, sizeof what, "%s: pieces chain end to start%s", name, closed ? " (and close)" : "");
    check(s.maxChainGap == 0., what);
    std::snprintf(what, sizeof what, "%s: no corner at any joint (< 1e-7 rad)", name);
    check(s.maxJoint < 1e-7, what);
    std::snprintf(what, sizeof what, "%s: bulge rebuilds the same arc (center < 1e-6)", name);
    check(s.maxBulgeCenter < 1e-6, what);
    std::snprintf(what, sizeof what, "%s: at most %zu pieces", name, maxPieces);
    check(s.pieces > 0 && s.pieces <= maxPieces, what);
}

} // namespace

int main() {
    // 1. straight line: one straight piece
    {
        const Curve c = [](double t) { return Vec{10. * t, 5. * t}; };
        const auto r = lc::biarc::fit(sample(c, 100, false), false, 0.01);
        check(r.pieces.size() == 1 && !r.pieces[0].arc, "straight: one straight piece");
        check(r.maxDeviation < 1e-9, "straight: deviation 0");
    }
    // 2. quarter circle r = 10: arcs on that circle
    {
        const Curve c = [](double t) { return Vec{10. * std::cos(t * kPi / 2), 10. * std::sin(t * kPi / 2)}; };
        smoothCase("quarter circle", c, 200, false, 1e-3, 4);
        const auto r = lc::biarc::fit(sample(c, 200, false), false, 1e-3);
        bool onCircle = true;
        for (const Piece& pc : r.pieces) {
            onCircle = onCircle && pc.arc && std::abs(pc.radius - 10.) < 1e-3 && pc.sweep > 0.;
        }
        check(onCircle, "quarter circle: every piece is a counter-clockwise arc of radius 10");
    }
    // 3./4. sine wave, two tolerances: tighter gives more pieces
    const Curve sine = [](double t) { return Vec{60. * t, 5. * std::sin(12. * t)}; };
    smoothCase("sine 0.01", sine, 3000, false, 0.01, 60);
    smoothCase("sine 0.001", sine, 3000, false, 0.001, 150);
    {
        const auto a = lc::biarc::fit(sample(sine, 3000, false), false, 0.01);
        const auto b = lc::biarc::fit(sample(sine, 3000, false), false, 0.001);
        check(b.pieces.size() > a.pieces.size(), "sine: tolerance 0.001 gives more pieces than 0.01");
    }
    // 5. closed ellipse: closed chain, no corner at the seam either
    {
        const Curve c = [](double t) { return Vec{20. * std::cos(2 * kPi * t), 8. * std::sin(2 * kPi * t)}; };
        smoothCase("closed ellipse", c, 2000, true, 0.01, 40);
    }
    // 6. full circle (closed), clockwise
    {
        const Curve c = [](double t) { return Vec{5. * std::cos(-2 * kPi * t), 5. * std::sin(-2 * kPi * t)}; };
        smoothCase("closed circle cw", c, 720, true, 1e-4, 16);
        const auto r = lc::biarc::fit(sample(c, 720, true), true, 1e-4);
        bool cw = !r.pieces.empty();
        for (const Piece& pc : r.pieces) {
            cw = cw && pc.arc && pc.sweep < 0. && pc.bulge() < 0.;
        }
        check(cw, "closed circle cw: every piece clockwise with a negative bulge");
    }
    // 7. a wiggly curve with many turns (like the 30-control-point spline of the prototype)
    {
        const Curve c = [](double t) { return Vec{73. * t, 3. * std::sin(40. * t) + 1.5 * std::sin(97. * t)}; };
        smoothCase("wiggly", c, 8000, false, 0.01, 400);
    }
    // 7b. closed curve far from the origin whose last point misses the first by a rounding error
    //     (LibreCAD's closed RS_Spline at x = 3.48e6 did this: the seam got a 5e-5 rad kink)
    {
        const Curve c = [](double t) {
            return Vec{3477900. + 20. * std::cos(2 * kPi * t), 7400. + 8. * std::sin(2 * kPi * t)};
        };
        std::vector<Vec> pts = sample(c, 2000, true);
        pts.push_back(c(0.) + Vec{1e-9, -1e-9});
        const auto r = lc::biarc::fit(pts, true, 0.01);
        const Stats s = measure(r, c, true);
        report("far closed, seam gap", s, 0.01);
        check(within(s, 0.01), "far closed, seam gap: within tolerance of the exact curve both ways");
        check(s.maxChainGap == 0., "far closed, seam gap: pieces chain and close exactly");
        check(s.maxJoint < 1e-7, "far closed, seam gap: no corner at the seam (< 1e-7 rad)");
    }
    // 8. a real corner: two straight pieces, the corner kept
    {
        std::vector<Vec> pts;
        for (int i = 0; i <= 50; ++i) pts.push_back({i * 0.2, 0.});
        for (int i = 1; i <= 50; ++i) pts.push_back({10., i * 0.2});
        const auto r = lc::biarc::fit(pts, false, 0.01);
        const bool two = r.pieces.size() == 2 && !r.pieces[0].arc && !r.pieces[1].arc;
        check(two, "corner: two straight pieces");
        check(two && std::abs(angleBetween(r.pieces[0].tangentAt(1.), r.pieces[1].tangentAt(0.)) - kPi / 2) < 1e-9,
              "corner: the 90 degree corner is kept");
        check(r.maxDeviation < 1e-9, "corner: deviation 0");
    }
    // 9. degenerate input
    {
        check(lc::biarc::fit({}, false, 0.01).pieces.empty(), "no points: no pieces");
        check(lc::biarc::fit({{1., 1.}, {1., 1.}}, false, 0.01).pieces.empty(), "two equal points: no pieces");
        const auto two = lc::biarc::fit({{0., 0.}, {3., 4.}}, false, 0.01);
        check(two.pieces.size() == 1 && !two.pieces[0].arc, "two points: one straight piece");
        const auto tri = lc::biarc::fit({{0., 0.}, {3., 0.}, {3., 4.}}, true, 0.01);
        check(!tri.pieces.empty(), "three points closed: fitted as an open curve, no crash");
    }
    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

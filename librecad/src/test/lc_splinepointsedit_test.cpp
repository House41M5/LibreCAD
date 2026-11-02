/*
 * Context: standalone test of actions/lc_splinepointsedit.h (the list and knot edits behind the spline
 * point actions). Not part of src.pro; build and run by hand, e.g. from a VS x64 prompt:
 *   cl /nologo /EHsc /std:c++17 /I..\actions lc_splinepointsedit_test.cpp /Fe:%TEMP%\spe_test.exe && %TEMP%\spe_test.exe
 * Prints PASS/FAIL per check and exits non-zero on any failure.
 * The locality checks evaluate the B-spline with de Boor's algorithm before and after an edit and require
 * the spans away from the edit to be unchanged at the same parameter values.
 */

#include <cmath>
#include <cstdio>
#include <vector>

#include "lc_splinepointsedit.h"

using namespace LC_SplinePointsEdit;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct V { double x, y; };

V deBoor(const std::vector<V>& P, const std::vector<double>& U, int p, double t) {
    const int n = int(P.size());
    int k = p;
    while (k < n - 1 && t >= U[k + 1]) ++k;
    std::vector<V> d(p + 1);
    for (int j = 0; j <= p; ++j) d[j] = P[j + k - p];
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const double den = U[j + 1 + k - r] - U[j + k - p];
            const double a = den == 0.0 ? 0.0 : (t - U[j + k - p]) / den;
            d[j] = {(1 - a) * d[j - 1].x + a * d[j].x, (1 - a) * d[j - 1].y + a * d[j].y};
        }
    }
    return d[p];
}

double dist(V a, V b) { return std::hypot(a.x - b.x, a.y - b.y); }

// A clamped, non-uniform knot vector for n control points of degree p.
std::vector<double> makeKnots(int n, int p) {
    std::vector<double> U(p + 1, 0.0);
    double u = 0.0;
    for (int k = 0; k < n - p - 1; ++k) {
        u += 1.0 + 0.37 * (k % 5);
        U.push_back(u);
    }
    u += 1.0;
    for (int k = 0; k <= p; ++k) U.push_back(u);
    return U;
}

// Index of the first knot where a and b differ (the inserted/removed knot).
std::size_t firstDiff(const std::vector<double>& a, const std::vector<double>& b) {
    std::size_t k = 0;
    while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
    return k;
}

// Max distance between the two curves over the given spans [U[m], U[m+1]) of `U` (sampled).
double maxDeviation(const std::vector<V>& P0, const std::vector<double>& U0,
                    const std::vector<V>& P1, const std::vector<double>& U1, int p,
                    const std::vector<double>& U, int mFrom, int mTo) {
    double worst = 0.0;
    for (int m = mFrom; m <= mTo; ++m) {
        for (int s = 0; s < 8; ++s) {
            const double t = U[m] + (U[m + 1] - U[m]) * s / 8.0;
            worst = std::max(worst, dist(deBoor(P0, U0, p, t), deBoor(P1, U1, p, t)));
        }
    }
    return worst;
}

void testLists() {
    std::vector<int> v{10, 11, 12};
    check(insertAt(v, 0, 9) && v == std::vector<int>({9, 10, 11, 12}), "insertAt 0 = before the first point");
    check(insertAt(v, 4, 13) && v == std::vector<int>({9, 10, 11, 12, 13}), "insertAt size() = after the last point");
    check(insertAt(v, 2, 99) && v == std::vector<int>({9, 10, 99, 11, 12, 13}), "insertAt middle");
    check(!insertAt(v, 7, 0) && v.size() == 6, "insertAt past the end is refused");

    check(indicesBetween(6, 1, 4, false, false) == std::vector<std::size_t>({2, 3}), "between 1 and 4 (open)");
    check(indicesBetween(6, 4, 1, false, false) == std::vector<std::size_t>({2, 3}), "between is order independent");
    check(indicesBetween(6, 1, 2, false, false).empty(), "neighbours: nothing between");
    check(indicesBetween(6, 3, 3, false, false).empty(), "same point: nothing");
    check(indicesBetween(6, 1, 9, false, false).empty(), "index out of range: nothing");
    check(indicesBetween(6, 1, 4, true, true) == std::vector<std::size_t>({0, 5}), "closed + outside = the other arc");
    check(indicesBetween(6, 1, 4, false, true) == std::vector<std::size_t>({2, 3}), "outside ignored when open");
    check(indicesBetween(6, 0, 5, true, true).empty(), "closed outside of 0..5: nothing");

    std::vector<int> w{0, 1, 2, 3, 4, 5};
    check(removeIndices(w, {2, 3}, 3) && w == std::vector<int>({0, 1, 4, 5}), "remove 2,3");
    std::vector<int> w2{0, 1, 2, 3, 4};
    check(!removeIndices(w2, {1, 2, 3}, 3) && w2.size() == 5, "refused when fewer than minCount would remain");
    check(!removeIndices(w2, {}, 1) && w2.size() == 5, "nothing to remove is refused");
    check(removeIndices(w2, {4}, 4) && w2 == std::vector<int>({0, 1, 2, 3}), "remove the last point down to minCount");
    check(!removeIndices(w2, {0}, 4), "remove below minCount refused");
}

void testKnots() {
    const int p = 3;
    const std::vector<double> U = makeKnots(8, p);
    check(isClampedKnots(U, 8, p), "makeKnots is clamped");
    check(!isClampedKnots(U, 7, p), "size mismatch is not clamped");
    std::vector<double> bad = U;
    bad[1] = 0.5;
    check(!isClampedKnots(bad, 8, p), "unclamped start is rejected");
    check(knotsAfterInsert(bad, 8, p, 3).empty() && knotsAfterRemove(bad, 8, p, 3).empty(),
          "edits on unclamped knots fall back (empty)");
    check(knotsAfterInsert({}, 8, p, 3).empty(), "empty (uniform) knots stay empty");

    for (std::size_t at = 0; at <= 8; ++at) {
        const auto U1 = knotsAfterInsert(U, 8, p, at);
        char msg[96];
        std::snprintf(msg, sizeof msg, "insert at %zu gives a clamped vector for 9 points", at);
        check(isClampedKnots(U1, 9, p), msg);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        const auto U1 = knotsAfterRemove(U, 8, p, i);
        char msg[96];
        std::snprintf(msg, sizeof msg, "remove %zu gives a clamped vector for 7 points", i);
        check(isClampedKnots(U1, 7, p), msg);
    }
    const std::vector<double> bezier{0, 0, 0, 0, 1, 1, 1, 1};
    check(knotsAfterRemove(bezier, 4, p, 1).empty(), "no interior knot to remove (Bezier)");
    check(isClampedKnots(knotsAfterInsert(bezier, 4, p, 2), 5, p), "insert into a Bezier");
}

void testLocality(int p) {
    const int n = 30;
    std::vector<V> P;
    for (int k = 0; k < n; ++k) P.push_back({double(k), std::sin(k * 0.7) * 3.0});
    const std::vector<double> U = makeKnots(n, p);

    // insert at index 15
    std::vector<V> P1 = P;
    insertAt(P1, 15, V{15.5, 6.0});
    const std::vector<double> U1 = knotsAfterInsert(U, n, p, 15);
    const int s = int(firstDiff(U, U1));
    const int leftEnd = std::min(15, s - p - 1) - 1;  // last unchanged span on the left (new indexing)
    const int rightBegin = std::max(15, s) + p + 1;   // first unchanged span on the right (new indexing)
    char msg[160];
    std::snprintf(msg, sizeof msg, "degree %d insert: spans %d..%d and %d..%d unchanged (new knot at %d)",
                  p, p, leftEnd, rightBegin, n, s);
    const double devL = maxDeviation(P, U, P1, U1, p, U1, p, leftEnd);
    const double devR = maxDeviation(P, U, P1, U1, p, U1, rightBegin, n);
    check(leftEnd >= p + 3 && rightBegin <= n - 3 && devL < 1e-12 && devR < 1e-12, msg);
    std::snprintf(msg, sizeof msg, "degree %d insert: the curve near the edit does change", p);
    check(maxDeviation(P, U, P1, U1, p, U1, leftEnd + 1, rightBegin - 1) > 1e-3, msg);

    // remove index 15
    std::vector<V> P2 = P;
    removeIndices(P2, {15}, 4);
    const std::vector<double> U2 = knotsAfterRemove(U, n, p, 15);
    const int r = int(firstDiff(U, U2));  // removed knot index in the old vector
    const int leftEnd2 = std::min(15, r - p - 1) - 1;
    const int rightBegin2 = std::max(15, r) + p;      // new indexing (one less after the removal)
    std::snprintf(msg, sizeof msg, "degree %d remove: spans %d..%d and %d..%d unchanged (old knot %d)",
                  p, p, leftEnd2, rightBegin2, n - 2, r);
    const double devL2 = maxDeviation(P, U, P2, U2, p, U2, p, leftEnd2);
    const double devR2 = maxDeviation(P, U, P2, U2, p, U2, rightBegin2, n - 2);
    check(leftEnd2 >= p + 3 && rightBegin2 <= n - 5 && devL2 < 1e-12 && devR2 < 1e-12, msg);

    // master's behaviour for comparison: uniform knots after the insert. Compare shapes, not parameters:
    // distance from points of the old far-left spans to a dense sampling of the new curve.
    std::vector<double> uniform(p + 1, 0.0);
    for (int k = 1; k < n + 1 - p; ++k) uniform.push_back(k);
    for (int k = 0; k <= p; ++k) uniform.push_back(n + 1 - p);
    std::vector<V> dense;
    for (int s2 = 0; s2 < 20000; ++s2) dense.push_back(deBoor(P1, uniform, p, uniform.back() * s2 / 20000.0));
    double farMove = 0.0;
    for (int m = p; m <= leftEnd; ++m) {
        for (int s2 = 0; s2 < 8; ++s2) {
            const V q = deBoor(P, U, p, U[m] + (U[m + 1] - U[m]) * s2 / 8.0);
            double best = 1e300;
            for (const V& d : dense) best = std::min(best, dist(q, d));
            farMove = std::max(farMove, best);
        }
    }
    std::snprintf(msg, sizeof msg, "degree %d: clearing the knots (master) moves the far spans by %.3f", p, farMove);
    check(isClampedKnots(uniform, n + 1, p) && farMove > 1e-2, msg);
}

} // namespace

int main() {
    testLists();
    testKnots();
    testLocality(2);
    testLocality(3);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

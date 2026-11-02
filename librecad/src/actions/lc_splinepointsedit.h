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
 * Context: the list and knot edits behind the spline point actions (lc_actionsplinemodify.cpp:
 * insert, append, remove, remove between two). Header-only and free of LibreCAD types so the
 * edits can be tested on plain values: librecad/src/test/lc_splinepointsedit_test.cpp.
 * Why the knot functions: master clears the knot vector of an RS_Spline on every point edit,
 * which reshapes the whole curve when the knots were not uniform (splines that came from DWG,
 * e.g. fitted with ezdxf). Adding/removing one interior knot next to the edited point keeps the
 * knots of the far spans, so by B-spline locality the curve away from the edit keeps its shape.
 */

#ifndef LC_SPLINEPOINTSEDIT_H
#define LC_SPLINEPOINTSEDIT_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace LC_SplinePointsEdit {

/** Inserts p so that it gets index `at` (0 = before the first point, size() = after the last). */
template <class P>
bool insertAt(std::vector<P>& pts, std::size_t at, const P& p) {
    if (at > pts.size()) {
        return false;
    }
    pts.insert(pts.begin() + at, p);
    return true;
}

/**
 * Indices (ascending) of the points strictly between i and j. For a closed spline with
 * `outside` set, the indices of the other arc instead: before the lower and after the higher
 * index, so that i..j remain. Empty when i == j, an index is out of range or nothing lies between.
 */
inline std::vector<std::size_t> indicesBetween(std::size_t n, std::size_t i, std::size_t j,
                                               bool closed, bool outside) {
    std::vector<std::size_t> out;
    if (i >= n || j >= n || i == j) {
        return out;
    }
    const std::size_t lo = std::min(i, j);
    const std::size_t hi = std::max(i, j);
    if (closed && outside) {
        for (std::size_t k = 0; k < lo; ++k) out.push_back(k);
        for (std::size_t k = hi + 1; k < n; ++k) out.push_back(k);
    } else {
        for (std::size_t k = lo + 1; k < hi; ++k) out.push_back(k);
    }
    return out;
}

/**
 * Removes the points at the given indices (ascending, unique) if at least minCount points
 * remain. Returns false and leaves pts unchanged otherwise, or when there is nothing to remove.
 */
template <class P>
bool removeIndices(std::vector<P>& pts, const std::vector<std::size_t>& indices, std::size_t minCount) {
    if (indices.empty() || indices.back() >= pts.size() || pts.size() - indices.size() < minCount) {
        return false;
    }
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        pts.erase(pts.begin() + *it);
    }
    return true;
}

/** Greville abscissa of control point k: the mean of knots k+1 .. k+degree. */
inline double greville(const std::vector<double>& knots, std::size_t k, int degree) {
    double s = 0.0;
    for (int m = 1; m <= degree; ++m) {
        s += knots[k + m];
    }
    return s / degree;
}

/** Whether knots is a non-decreasing clamped knot vector for n control points of `degree`. */
inline bool isClampedKnots(const std::vector<double>& knots, std::size_t n, int degree) {
    if (degree < 1 || n < std::size_t(degree) + 1 || knots.size() != n + degree + 1) {
        return false;
    }
    for (std::size_t m = 1; m < knots.size(); ++m) {
        if (!(knots[m] >= knots[m - 1])) {
            return false;
        }
    }
    for (int m = 1; m <= degree; ++m) {
        if (knots[m] != knots.front() || knots[knots.size() - 1 - m] != knots.back()) {
            return false;
        }
    }
    return knots.back() > knots.front();
}

/**
 * Knot vector after a control point is inserted at index `at` (0..n) of an open B-spline with
 * n control points: one interior knot halfway between the Greville abscissae of the new point's
 * neighbours. Returns {} when the knots are not clamped or the new knot would exceed multiplicity
 * `degree`; the caller then falls back to uniform knots.
 */
inline std::vector<double> knotsAfterInsert(const std::vector<double>& knots, std::size_t n,
                                            int degree, std::size_t at) {
    if (!isClampedKnots(knots, n, degree) || at > n) {
        return {};
    }
    std::size_t a = 0;
    std::size_t b = 1;
    if (at == n) {
        a = n - 2;
        b = n - 1;
    } else if (at > 0) {
        a = at - 1;
        b = at;
    }
    const double v = 0.5 * (greville(knots, a, degree) + greville(knots, b, degree));
    std::vector<double> out = knots;
    const auto first = out.begin() + degree + 1;
    const auto last = out.end() - (degree + 1);
    out.insert(std::upper_bound(first, last, v), v);
    if (std::count(out.begin(), out.end(), v) > degree) {
        return {};
    }
    return out;
}

/**
 * Knot vector after control point i is removed from an open B-spline with n control points:
 * drops the interior knot nearest to the point's Greville abscissa. Returns {} when the knots
 * are not clamped or there is no interior knot.
 */
inline std::vector<double> knotsAfterRemove(const std::vector<double>& knots, std::size_t n,
                                            int degree, std::size_t i) {
    if (!isClampedKnots(knots, n, degree) || i >= n || n <= std::size_t(degree) + 1) {
        return {};
    }
    const double g = greville(knots, i, degree);
    std::size_t best = degree + 1;
    for (std::size_t m = degree + 1; m < n; ++m) {  // interior knots
        if (std::abs(knots[m] - g) < std::abs(knots[best] - g)) {
            best = m;
        }
    }
    std::vector<double> out = knots;
    out.erase(out.begin() + best);
    return out;
}

} // namespace LC_SplinePointsEdit

#endif

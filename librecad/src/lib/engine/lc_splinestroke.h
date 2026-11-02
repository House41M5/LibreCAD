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
 * Context: one place that turns a spline into the points of the lines that replace it, shared by the
 * "explode spline" action (lc_actionsplineexplode) and the generic Explode (RS_Modification::explode)
 * - 2026-11-02, plans-win librecad-factory-drawings step 20.
 * Why: before this, Explode cloned RS_Spline's internal display lines (pen FlagInvalid -> written as
 * colour 7 / CONTINUOUS / lineweight 0) and skipped LC_SplinePoints (atomic) without a word.
 * The points are the ones LibreCAD draws on screen at the same resolution, so exploding at $SPLINESEGS
 * gives exactly the picture the user saw. Resolution: per control point for RS_Spline (as update()),
 * per quadratic piece for LC_SplinePoints (as getStrokePoints()) - master's action passes one count
 * for the whole RS_Spline, which gives 8 segments for a whole curve.
 */
#ifndef LC_SPLINESTROKE_H
#define LC_SPLINESTROKE_H

#include <vector>

#include "lc_splinepoints.h"
#include "rs_spline.h"
#include "rs_vector.h"

namespace lc {

/**
 * Points along a spline or spline through points; a closed curve ends on its first point again.
 * segments <= 0: the drawing's $SPLINESEGS (8 when unset).
 * Returns false (pts empty) for other entities or splines without enough points.
 */
inline bool splineStrokePoints(const RS_Entity* e, int segments, std::vector<RS_Vector>& pts, bool& closed) {
    pts.clear();
    closed = false;
    if (e == nullptr) {
        return false;
    }
    if (segments <= 0) {
        segments = e->getGraphicVariableInt("$SPLINESEGS", 8);
        if (segments <= 0) {
            segments = 8;
        }
    }
    switch (e->rtti()) {
    case RS2::EntitySpline: {
        const auto* s = static_cast<const RS_Spline*>(e);
        closed = s->isClosed();
        pts = s->getStrokePoints(segments);
        break;
    }
    case RS2::EntitySplinePoints: {
        const auto* s = static_cast<const LC_SplinePoints*>(e);
        closed = s->isClosed();
        if (s->getControlPoints().size() < 3) {
            return false;
        }
        pts = s->getStrokePoints(segments);
        if (closed && !pts.empty()) {
            pts.push_back(pts.front());
        }
        break;
    }
    default:
        return false;
    }
    // drop repeated points (a zero-length line is useless and a polyline vertex twice is a trap)
    std::vector<RS_Vector> out;
    out.reserve(pts.size());
    for (const RS_Vector& p : pts) {
        if (p.valid && (out.empty() || (p - out.back()).squared() > RS_TOLERANCE2)) {
            out.push_back(p);
        }
    }
    pts.swap(out);
    if (pts.size() < 2) {
        pts.clear();
        return false;
    }
    return true;
}

} // namespace lc

#endif

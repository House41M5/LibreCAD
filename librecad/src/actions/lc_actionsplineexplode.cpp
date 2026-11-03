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
 * Context and differences from master: see lc_actionsplineexplode.h.
 * Gotchas: setPenToActive()/setLayerToActive() look the document up through the parent, so they run
 * after reparent(). RS_Polyline::endPolyline() adds the closing segment from the last vertex to the
 * first, so a closed result gets its vertices without the repeated first point; in Arcs mode the bulge
 * given with a vertex belongs to the segment that starts there, so the closing segment takes the bulge
 * of the last vertex added.
 */

#include "lc_actionsplineexplode.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>

#include "lc_biarcfit.h"
#include "lc_splineexplodeoptions.h"
#include "lc_splinefit.h"
#include "lc_splinefrompolylineoptions.h"
#include "lc_splinepoints.h"
#include "lc_splinestroke.h"
#include "lc_undosection.h"
#include "rs_arc.h"
#include "rs_dialogfactory.h"
#include "rs_document.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_polyline.h"
#include "rs_preview.h"
#include "rs_spline.h"

namespace {

const EntityTypeList g_splineTypes = {RS2::EntitySplinePoints, RS2::EntitySpline};

void applyPenAndLayer(const RS_Entity* source, RS_Entity* target, bool currentLayer, bool currentPen) {
    if (currentLayer) {
        target->setLayerToActive();
    } else {
        target->setLayer(source->getLayer());
    }
    if (currentPen) {
        target->setPenToActive();
    } else {
        target->setPen(source->getPen(false));
    }
}

/** Adds the new entities and (unless keep) removes the old one, all in one undo cycle. */
void replaceInDocument(RS_Document* document, RS_EntityContainer* parent, RS_Entity* old,
                       const std::vector<RS_Entity*>& added, bool keep) {
    for (RS_Entity* n : added) {
        parent->addEntity(n);
    }
    old->setSelected(false);
    if (document != nullptr) {
        LC_UndoSection undo(document);
        for (RS_Entity* n : added) {
            undo.addUndoable(n);
        }
        if (!keep) {
            old->setUndoState(true);
            undo.addUndoable(old);
        }
    } else if (!keep) {
        parent->removeEntity(old);
    }
}

/**
 * Points of e dense enough for the arc fit: the chords between them stay within about tolerance / 20 of
 * the curve, so measuring the arcs against the chords costs little of the tolerance. Starts at 16 points
 * per control point (spline through points: per curve piece), estimates the largest sagitta from the
 * turn between neighbouring chords (sagitta ~ chord * turn / 8, quadratic in the spacing) and samples
 * again as densely as needed, at most 4096 per control point and about 200000 points in all.
 */
bool densePoints(const RS_Entity* e, double tolerance, std::vector<RS_Vector>& pts, bool& closed) {
    constexpr int base = 16;
    if (!lc::splineStrokePoints(e, base, pts, closed)) {
        return false;
    }
    double sagitta = 0.;
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const RS_Vector a = pts[i] - pts[i - 1];
        const RS_Vector b = pts[i + 1] - pts[i];
        const double la = a.magnitude();
        const double lb = b.magnitude();
        if (la <= 0. || lb <= 0.) {
            continue;
        }
        const double turn = std::acos(std::clamp(RS_Vector::dotP(a, b) / (la * lb), -1., 1.));
        if (turn > lc::biarc::kCornerAngle) {
            continue; // a corner of the curve, not a bend that denser points would follow better
        }
        sagitta = std::max(sagitta, 0.5 * (la + lb) * turn / 8.);
    }
    const double target = tolerance / 20.;
    if (sagitta <= target) {
        return true;
    }
    const size_t units = std::max<size_t>(1, pts.size() / base); // about the control points / curve pieces
    const int cap = int(std::clamp<size_t>(200000 / units, base, 4096));
    const int segments = std::min(cap, int(std::ceil(base * std::sqrt(sagitta / target))));
    return segments <= base || lc::splineStrokePoints(e, segments, pts, closed);
}

} // namespace

// ---------------------------------------------------------------------------
// Explode spline

LC_ActionSplineExplode::LC_ActionSplineExplode(RS_EntityContainer& container, RS_GraphicView& graphicView)
    : RS_PreviewActionInterface("Explode Spline", container, graphicView, RS2::ActionDrawSplineExplode) {
    actionType = RS2::ActionDrawSplineExplode;
}

void LC_ActionSplineExplode::init(int status) {
    RS_PreviewActionInterface::init(status);
    deletePreview();
}

int LC_ActionSplineExplode::getDrawingSegments() const {
    const int n = graphic != nullptr ? graphic->getVariableInt("$SPLINESEGS", 8) : 8;
    return n > 0 ? n : 8;
}

void LC_ActionSplineExplode::setArcTolerance(double t) {
    m_arcTolerance = std::isfinite(t) ? std::max(std::abs(t), kMinArcTolerance) : 0.01;
}

std::vector<RS_Entity*> LC_ActionSplineExplode::explodedToArcs(const RS_Entity* e, RS_EntityContainer* parent,
                                                               double* deviation) const {
    std::vector<RS_Entity*> result;
    std::vector<RS_Vector> pts;
    bool closed = false;
    if (!densePoints(e, m_arcTolerance, pts, closed)) {
        return result;
    }
    std::vector<lc::biarc::Vec> v;
    v.reserve(pts.size());
    for (const RS_Vector& p : pts) {
        v.push_back({p.x, p.y});
    }
    const lc::biarc::Result fit = lc::biarc::fit(std::move(v), closed, m_arcTolerance);
    if (fit.pieces.empty()) {
        return result;
    }
    if (deviation != nullptr) {
        *deviation = fit.maxDeviation;
    }
    const auto rs = [](const lc::biarc::Vec& p) { return RS_Vector(p.x, p.y); };
    if (m_toPolyline) {
        auto* pl = new RS_Polyline(parent);
        for (const lc::biarc::Piece& pc : fit.pieces) {
            pl->addVertex(rs(pc.start), pc.bulge()); // the bulge of the segment starting here
        }
        if (closed) {
            pl->setClosed(true);
            pl->endPolyline(); // closing segment: the last piece, with the bulge of the last vertex
        } else {
            pl->addVertex(rs(fit.pieces.back().end));
        }
        result.push_back(pl);
    } else {
        for (const lc::biarc::Piece& pc : fit.pieces) {
            if (pc.arc) {
                result.push_back(new RS_Arc(parent, RS_ArcData(rs(pc.center), pc.radius,
                                                               RS_Math::correctAngle(pc.angle1),
                                                               RS_Math::correctAngle(pc.angle1 + pc.sweep),
                                                               pc.sweep < 0.)));
            } else {
                result.push_back(new RS_Line(parent, rs(pc.start), rs(pc.end)));
            }
        }
    }
    return result;
}

std::vector<RS_Entity*> LC_ActionSplineExplode::exploded(const RS_Entity* e, RS_EntityContainer* parent,
                                                         double* deviation) const {
    if (deviation != nullptr) {
        *deviation = 0.;
    }
    if (m_toArcs) {
        return explodedToArcs(e, parent, deviation);
    }
    std::vector<RS_Entity*> result;
    std::vector<RS_Vector> pts;
    bool closed = false;
    const int segments = m_useCustomSegments ? std::max(1, m_customSegments) : getDrawingSegments();
    if (!lc::splineStrokePoints(e, segments, pts, closed)) {
        return result;
    }
    if (m_toPolyline) {
        if (closed && pts.size() > 2 && (pts.back() - pts.front()).squared() <= RS_TOLERANCE2) {
            pts.pop_back(); // endPolyline() closes it
        }
        auto* pl = new RS_Polyline(parent);
        for (const RS_Vector& p : pts) {
            pl->addVertex(p);
        }
        if (closed) {
            pl->setClosed(true);
            pl->endPolyline();
        }
        result.push_back(pl);
    } else {
        for (size_t i = 1; i < pts.size(); ++i) {
            result.push_back(new RS_Line(parent, pts[i - 1], pts[i]));
        }
    }
    return result;
}

void LC_ActionSplineExplode::explodeEntity(RS_Entity* e) {
    RS_EntityContainer* parent = e->getParent() != nullptr ? e->getParent() : container;
    double deviation = 0.;
    std::vector<RS_Entity*> added = exploded(e, parent, &deviation);
    if (added.empty()) {
        RS_DIALOGFACTORY->commandMessage(tr("This spline has too few points to explode."));
        return;
    }
    for (RS_Entity* n : added) {
        n->reparent(parent);
        applyPenAndLayer(e, n, m_useCurrentLayer, m_useCurrentAttributes);
    }
    deletePreview();
    replaceInDocument(document, parent, e, added, m_keepOriginals);
    graphicView->redraw(RS2::RedrawDrawing);
    RS_DIALOGFACTORY->updateSelectionWidget(container->countSelected(), container->totalSelectedLength());
    if (m_toArcs) {
        size_t arcs = 0;
        size_t lines = 0;
        const auto count = [&arcs, &lines](const RS_Entity* x) { ++(x->rtti() == RS2::EntityArc ? arcs : lines); };
        for (RS_Entity* n : added) {
            if (n->rtti() == RS2::EntityPolyline) {
                for (RS_Entity* s : *static_cast<RS_Polyline*>(n)) {
                    count(s);
                }
            } else {
                count(n);
            }
        }
        const QString dev = QString::number(deviation, 'g', 3);
        RS_DIALOGFACTORY->commandMessage(
            m_toPolyline ? tr("Spline exploded to a polyline of %1 arcs and %2 lines, deviation up to %3.").arg(arcs).arg(lines).arg(dev)
                         : tr("Spline exploded to %1 arcs and %2 lines, deviation up to %3.").arg(arcs).arg(lines).arg(dev));
        return;
    }
    const size_t n = m_toPolyline ? static_cast<RS_Polyline*>(added.front())->count() : added.size();
    RS_DIALOGFACTORY->commandMessage(m_toPolyline ? tr("Spline exploded to a polyline of %1 segments.").arg(n)
                                                  : tr("Spline exploded to %1 lines.").arg(n));
}

void LC_ActionSplineExplode::mouseMoveEvent(QMouseEvent* e) {
    deletePreview();
    RS_Entity* en = catchEntity(e, g_splineTypes);
    if (en != nullptr) {
        for (RS_Entity* n : exploded(en, preview.get())) {
            preview->addEntity(n);
        }
    }
    drawPreview();
}

void LC_ActionSplineExplode::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        RS_Entity* en = catchEntity(e, g_splineTypes);
        if (en == nullptr) {
            RS_DIALOGFACTORY->commandMessage(tr("No spline found. Pick a spline or a spline through points."));
            return;
        }
        if (en->isLocked()) {
            RS_DIALOGFACTORY->commandMessage(tr("The spline is on a locked layer."));
            return;
        }
        explodeEntity(en);
    } else if (e->button() == Qt::RightButton) {
        deletePreview();
        init(getStatus() - 1);
    }
}

void LC_ActionSplineExplode::updateMouseButtonHints() {
    if (getStatus() == 0) {
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the spline to explode"), tr("Cancel"));
    } else {
        RS_DIALOGFACTORY->updateMouseWidget();
    }
}

void LC_ActionSplineExplode::updateMouseCursor() {
    graphicView->setMouseCursor(RS2::SelectCursor);
}

void LC_ActionSplineExplode::createOptionsWidget() {
    m_optionWidget = std::make_unique<LC_SplineExplodeOptions>(nullptr);
}

// ---------------------------------------------------------------------------
// Spline from polyline

LC_ActionSplineFromPolyline::LC_ActionSplineFromPolyline(RS_EntityContainer& container, RS_GraphicView& graphicView)
    : RS_PreviewActionInterface("Spline from Polyline", container, graphicView, RS2::ActionDrawSplineFromPolyline) {
    actionType = RS2::ActionDrawSplineFromPolyline;
}

void LC_ActionSplineFromPolyline::init(int status) {
    RS_PreviewActionInterface::init(status);
    deletePreview();
    m_fitted = Fitted{};
}

void LC_ActionSplineFromPolyline::setTolerance(double t) {
    m_tolerance = std::isfinite(t) ? std::max(std::abs(t), kMinTolerance) : 0.01;
}

namespace {

/** The polyline for lc::splinefit: its vertices (flag 1) and its arcs as points at most tol/20 off (flag 0). */
void fitInput(const RS_Polyline* polyline, double tol, std::vector<lc::splinefit::Vec>& pts, std::vector<char>& vertex) {
    auto add = [&](const RS_Vector& v, char f) {
        pts.emplace_back(v.x, v.y);
        vertex.push_back(f);
    };
    add(polyline->getStartpoint(), 1);
    for (RS_Entity* e : *const_cast<RS_Polyline*>(polyline)) { // the container has no const iteration
        if (e == nullptr || !e->isAtomic()) {
            continue;
        }
        if (e->rtti() == RS2::EntityArc) {
            const auto* arc = static_cast<const RS_Arc*>(e);
            const double a1 = arc->getAngle1();
            const bool reversed = arc->isReversed();
            const double r = arc->getRadius();
            double da = reversed ? std::fmod(a1 - arc->getAngle2() + 2. * M_PI, 2. * M_PI)
                                 : std::fmod(arc->getAngle2() - a1 + 2. * M_PI, 2. * M_PI);
            if (da < RS_TOLERANCE) {
                da = 2. * M_PI;
            }
            // sagitta r (1 - cos(step / 2)) <= tol / 20
            double step = da;
            const double c = 1. - tol / (20. * r);
            if (c > -1. && c < 1.) {
                step = 2. * std::acos(c);
            }
            const int parts = std::clamp(int(std::ceil(da / step)), 1, 4096);
            for (int i = 1; i < parts; ++i) {
                const double t = da * i / parts;
                add(arc->getCenter().relative(r, reversed ? a1 - t : a1 + t), 0);
            }
        }
        add(e->getEndpoint(), 1);
    }
}

} // namespace

const LC_ActionSplineFromPolyline::Fitted& LC_ActionSplineFromPolyline::fitted(const RS_Polyline* polyline) const {
    Fitted& f = m_fitted;
    if (f.polyline == polyline && f.count == polyline->count() && f.start == polyline->getStartpoint() &&
        f.end == polyline->getEndpoint() && f.length == polyline->getLength() && f.degree == m_degree &&
        f.tolerance == m_tolerance && f.onPolyline == m_onPolyline && f.closed == polyline->isClosed()) {
        return f;
    }
    std::vector<lc::splinefit::Vec> pts;
    std::vector<char> vertex;
    fitInput(polyline, m_tolerance, pts, vertex);
    const bool closed = polyline->isClosed();
    const lc::splinefit::Result r = m_onPolyline
                                        ? lc::splinefit::fitOnPolyline(pts, vertex, closed, m_degree, m_tolerance)
                                        : lc::splinefit::fit(pts, vertex, closed, m_degree, m_tolerance);
    f = Fitted{};
    f.polyline = polyline;
    f.count = polyline->count();
    f.start = polyline->getStartpoint();
    f.end = polyline->getEndpoint();
    f.length = polyline->getLength();
    f.degree = m_degree;
    f.tolerance = m_tolerance;
    f.onPolyline = m_onPolyline;
    f.closed = closed;
    for (const lc::splinefit::Vec& v : r.control) {
        f.control.emplace_back(v.x, v.y);
    }
    f.deviation = r.maxDeviation;
    f.reached = r.reached;
    return f;
}

std::vector<RS_Vector> LC_ActionSplineFromPolyline::pointsFromPolyline(const RS_Polyline* polyline, int midPoints) {
    std::vector<RS_Vector> pts;
    if (polyline == nullptr) {
        return pts;
    }
    midPoints = std::max(0, midPoints);
    const int parts = midPoints + 1;
    pts.push_back(polyline->getStartpoint());
    for (RS_Entity* e : *const_cast<RS_Polyline*>(polyline)) { // the container has no const iteration
        if (e == nullptr || !e->isAtomic()) {
            continue;
        }
        if (e->rtti() == RS2::EntityLine) {
            const auto* line = static_cast<const RS_Line*>(e);
            const RS_Vector a = line->getStartpoint();
            const RS_Vector d = line->getEndpoint() - a;
            for (int i = 1; i <= midPoints; ++i) {
                pts.push_back(a + d * (double(i) / parts)); // master: i / midPoints
            }
            pts.push_back(line->getEndpoint());
        } else if (e->rtti() == RS2::EntityArc) {
            const auto* arc = static_cast<const RS_Arc*>(e);
            const double a1 = arc->getAngle1();
            const double a2 = arc->getAngle2();
            const bool reversed = arc->isReversed();
            double da = reversed ? std::fmod(a1 - a2 + 2. * M_PI, 2. * M_PI) : std::fmod(a2 - a1 + 2. * M_PI, 2. * M_PI);
            if (da < RS_TOLERANCE) {
                da = 2. * M_PI;
            }
            for (int i = 1; i <= midPoints; ++i) {
                const double t = da * (double(i) / parts);
                pts.push_back(arc->getCenter().relative(arc->getRadius(), reversed ? a1 - t : a1 + t));
            }
            pts.push_back(arc->getEndpoint());
        }
    }
    // a closed polyline's closing segment ends on the start point again
    if (polyline->isClosed() && pts.size() > 1 && (pts.back() - pts.front()).squared() <= RS_TOLERANCE2) {
        pts.pop_back();
    }
    return pts;
}

RS_Entity* LC_ActionSplineFromPolyline::splineFor(const RS_Polyline* polyline, RS_EntityContainer* parent) const {
    const bool closed = polyline->isClosed();
    if (m_useTolerance) {
        const Fitted& f = fitted(polyline);
        if (f.control.size() < 3) {
            return nullptr;
        }
        auto* s = new RS_Spline(parent, RS_SplineData(std::clamp(m_degree, 1, 3), closed));
        for (const RS_Vector& p : f.control) {
            s->addControlPoint(p);
        }
        s->update();
        return s;
    }
    const std::vector<RS_Vector> pts = pointsFromPolyline(polyline, m_midPoints);
    if (m_useFitPoints && m_degree == 2) {
        if (pts.size() < 3) {
            return nullptr;
        }
        auto* sp = new LC_SplinePoints(parent, LC_SplinePointsData(closed, false));
        for (const RS_Vector& p : pts) {
            sp->addPoint(p);
        }
        sp->update();
        return sp;
    }
    const int degree = std::clamp(m_degree, 1, 3);
    // RS_Spline::update() draws nothing below these counts
    if (pts.size() < std::max<size_t>(3, size_t(degree) + 1)) {
        return nullptr;
    }
    auto* s = new RS_Spline(parent, RS_SplineData(degree, closed));
    for (const RS_Vector& p : pts) {
        s->addControlPoint(p);
    }
    s->update();
    return s;
}

void LC_ActionSplineFromPolyline::convert(RS_Polyline* polyline) {
    RS_EntityContainer* parent = polyline->getParent() != nullptr ? polyline->getParent() : container;
    RS_Entity* s = splineFor(polyline, parent);
    if (s == nullptr) {
        RS_DIALOGFACTORY->commandMessage(tr("This polyline has too few points for a spline of degree %1.").arg(m_degree));
        return;
    }
    QString done;
    if (m_useTolerance) { // before the polyline leaves the document
        const Fitted& f = fitted(polyline);
        const unsigned vertices = polyline->count() + (polyline->isClosed() ? 0 : 1);
        done = f.reached ? tr("Spline of %1 control points for %2 polyline vertices, deviation up to %3.")
                               .arg(f.control.size()).arg(vertices).arg(f.deviation, 0, 'g', 3)
                         : tr("Tolerance %1 not reached: spline of %2 control points, deviation up to %3. "
                              "A larger tolerance gives fewer points.")
                               .arg(f.tolerance, 0, 'g', 3).arg(f.control.size()).arg(f.deviation, 0, 'g', 3);
    }
    s->reparent(parent);
    applyPenAndLayer(polyline, s, m_useCurrentLayer, m_useCurrentAttributes);
    deletePreview();
    replaceInDocument(document, parent, polyline, {s}, m_keepOriginals);
    if (!done.isEmpty()) {
        RS_DIALOGFACTORY->commandMessage(done);
    }
    graphicView->redraw(RS2::RedrawDrawing);
    RS_DIALOGFACTORY->updateSelectionWidget(container->countSelected(), container->totalSelectedLength());
}

void LC_ActionSplineFromPolyline::mouseMoveEvent(QMouseEvent* e) {
    deletePreview();
    auto* pl = static_cast<RS_Polyline*>(catchEntity(e, RS2::EntityPolyline));
    // Tolerance mode fits on hover only up to kPreviewSegments (test spirals: 1000 points 0.4 s, 3000 points
    // 4.6 s before the knot removal budget was halved, 5000 points 2.6 s after): a bigger polyline is fitted on
    // the click, so moving the mouse does not freeze the window
    const bool fitNow = !m_useTolerance || pl == nullptr || pl->count() <= kPreviewSegments ||
                        m_fitted.polyline == pl;
    if (pl != nullptr && fitNow) {
        if (RS_Entity* s = splineFor(pl, preview.get())) {
            preview->addEntity(s);
        }
    }
    drawPreview();
}

void LC_ActionSplineFromPolyline::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        auto* pl = static_cast<RS_Polyline*>(catchEntity(e, RS2::EntityPolyline));
        if (pl == nullptr) {
            RS_DIALOGFACTORY->commandMessage(tr("No polyline found."));
            return;
        }
        if (pl->isLocked()) {
            RS_DIALOGFACTORY->commandMessage(tr("The polyline is on a locked layer."));
            return;
        }
        convert(pl);
    } else if (e->button() == Qt::RightButton) {
        deletePreview();
        init(getStatus() - 1);
    }
}

void LC_ActionSplineFromPolyline::updateMouseButtonHints() {
    if (getStatus() == 0) {
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the polyline to turn into a spline"), tr("Cancel"));
    } else {
        RS_DIALOGFACTORY->updateMouseWidget();
    }
}

void LC_ActionSplineFromPolyline::updateMouseCursor() {
    graphicView->setMouseCursor(RS2::SelectCursor);
}

void LC_ActionSplineFromPolyline::createOptionsWidget() {
    m_optionWidget = std::make_unique<LC_SplineFromPolylineOptions>(nullptr);
}

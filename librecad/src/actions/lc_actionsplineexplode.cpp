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
 * first, so a closed result gets its vertices without the repeated first point.
 */

#include "lc_actionsplineexplode.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>

#include "lc_splineexplodeoptions.h"
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

std::vector<RS_Entity*> LC_ActionSplineExplode::exploded(const RS_Entity* e, RS_EntityContainer* parent) const {
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
    std::vector<RS_Entity*> added = exploded(e, parent);
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
    s->reparent(parent);
    applyPenAndLayer(polyline, s, m_useCurrentLayer, m_useCurrentAttributes);
    deletePreview();
    replaceInDocument(document, parent, polyline, {s}, m_keepOriginals);
    graphicView->redraw(RS2::RedrawDrawing);
    RS_DIALOGFACTORY->updateSelectionWidget(container->countSelected(), container->totalSelectedLength());
}

void LC_ActionSplineFromPolyline::mouseMoveEvent(QMouseEvent* e) {
    deletePreview();
    auto* pl = static_cast<RS_Polyline*>(catchEntity(e, RS2::EntityPolyline));
    if (pl != nullptr) {
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

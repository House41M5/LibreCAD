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
 * Context: see lc_actionsplinemodify.h. Added 2026-11-02 on kai/thai-fonts-2216 (plans-win
 * librecad-factory-drawings step 19, user asked to add/remove points of a spline through points).
 * Deliberate differences from master:
 *  - points are picked by index (nearest grip) instead of comparing RS_Vector values with ==, which is
 *    exact here and ambiguous for duplicates (the wrapped control points of a closed RS_Spline);
 *  - remove-between on a control-point LC_SplinePoints writes the result back to the control points;
 *    master assigns it to splinePoints, turning control points into points the curve passes through;
 *  - an open RS_Spline keeps a custom knot vector, adjusted next to the edit (lc_splinepointsedit.h),
 *    where master clears it and every non-uniform spline changes shape along its whole length;
 *  - the minimum point count is one rule per entity (minimumPointsOf) for every action.
 * Gotchas: RS_Spline keeps the first `degree` control points repeated at the end when closed; they are
 * removed before editing and update() wraps them again. LC_SplinePoints regenerates its control points
 * from splinePoints in update() unless cut/useControlPoints is set.
 * m_entity can go away while another action runs (undo, delete), so resume() checks it is still in the
 * container before touching it.
 */

#include "lc_actionsplinemodify.h"

#include <algorithm>

#include <QMouseEvent>

#include "lc_splinepoints.h"
#include "lc_splinepointsedit.h"
#include "lc_undosection.h"
#include "rs_circle.h"
#include "rs_coordinateevent.h"
#include "rs_debug.h"
#include "rs_dialogfactory.h"
#include "rs_document.h"
#include "rs_graphicview.h"
#include "rs_preview.h"
#include "rs_spline.h"

namespace {

const EntityTypeList g_splineTypes = {RS2::EntitySplinePoints, RS2::EntitySpline};

/** Same rule as LC_SplinePoints::UpdateControlPoints(): the control points are the data. */
bool usesControlPoints(const LC_SplinePointsData& d) {
    return d.cut || d.useControlPoints;
}

/** Same test as RS_Spline::hasWrappedControlPoints(), which is private. */
bool hasWrappedControlPoints(const RS_SplineData& d) {
    const auto& cp = d.controlPoints;
    if (!d.closed || d.degree < 1 || cp.size() < size_t(2 * d.degree)) {
        return false;
    }
    return std::equal(cp.cbegin(), cp.cbegin() + d.degree, cp.cbegin() + (cp.size() - d.degree));
}

std::vector<RS_Vector> editablePoints(const RS_Entity* e) {
    if (e == nullptr) {
        return {};
    }
    switch (e->rtti()) {
    case RS2::EntitySplinePoints: {
        const LC_SplinePointsData& d = static_cast<const LC_SplinePoints*>(e)->getData();
        return usesControlPoints(d) ? d.controlPoints : d.splinePoints;
    }
    case RS2::EntitySpline: {
        const RS_SplineData& d = static_cast<const RS_Spline*>(e)->getData();
        std::vector<RS_Vector> pts = d.controlPoints;
        if (hasWrappedControlPoints(d)) {
            pts.resize(pts.size() - d.degree);
        }
        return pts;
    }
    default:
        return {};
    }
}

bool closedSpline(const RS_Entity* e) {
    if (e == nullptr) {
        return false;
    }
    if (e->rtti() == RS2::EntitySplinePoints) {
        return static_cast<const LC_SplinePoints*>(e)->isClosed();
    }
    if (e->rtti() == RS2::EntitySpline) {
        return static_cast<const RS_Spline*>(e)->isClosed();
    }
    return false;
}

/**
 * Fewest points a spline keeps: 3 for LC_SplinePoints (a quadratic; with 2 control points
 * RS_FilterDXFRW::writeSplinePoints writes a LINE), degree + 1 for an open RS_Spline and 3 for a
 * closed one (RS_Spline::update, issue #1689).
 */
size_t minimumPointsOf(const RS_Entity* e) {
    if (e != nullptr && e->rtti() == RS2::EntitySpline) {
        const auto* s = static_cast<const RS_Spline*>(e);
        return s->isClosed() ? 3 : size_t(s->getDegree()) + 1;
    }
    return 3;
}

} // namespace

// ---------------------------------------------------------------------------
// LC_ActionSplineModifyBase

LC_ActionSplineModifyBase::LC_ActionSplineModifyBase(const char* name, RS_EntityContainer& container,
                                                     RS_GraphicView& graphicView, RS2::ActionType type)
    : RS_PreviewActionInterface(name, container, graphicView, type) {
    actionType = type;
}

LC_ActionSplineModifyBase::~LC_ActionSplineModifyBase() = default;

void LC_ActionSplineModifyBase::init(int status) {
    RS_PreviewActionInterface::init(status);
    if (status <= ChooseEntity) {
        releaseEntity();
    }
}

void LC_ActionSplineModifyBase::finish(bool updateTB) {
    deletePreview();
    if (m_entity != nullptr) {
        releaseEntity();
        graphicView->redraw(RS2::RedrawDrawing);
    }
    RS_PreviewActionInterface::finish(updateTB);
}

void LC_ActionSplineModifyBase::resume() {
    RS_PreviewActionInterface::resume();
    // findEntity first: after the redo history was cut, an undone entity is deleted.
    if (m_entity != nullptr && (container->findEntity(m_entity) < 0 || m_entity->isUndone())) {
        m_entity = nullptr;
        deletePreview();
        setStatus(ChooseEntity);
    }
}

RS_Vector LC_ActionSplineModifyBase::eventPosition(QMouseEvent* e) {
    return takesNewPoint(getStatus()) ? snapPoint(e) : snapFree(e);
}

void LC_ActionSplineModifyBase::mouseMoveEvent(QMouseEvent* e) {
    if (getStatus() == ChooseEntity || m_entity == nullptr) {
        return;
    }
    const RS_Vector pos = eventPosition(e);
    if (pos.valid) {
        previewAt(pos, (e->modifiers() & Qt::ShiftModifier) != 0);
    }
}

void LC_ActionSplineModifyBase::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        if (getStatus() == ChooseEntity) {
            pickEntity(e);
            return;
        }
        if (m_entity == nullptr) {
            setStatus(ChooseEntity);
            return;
        }
        const RS_Vector pos = eventPosition(e);
        if (pos.valid) {
            onPoint(pos, (e->modifiers() & Qt::ShiftModifier) != 0);
        }
    } else if (e->button() == Qt::RightButton) {
        deletePreview();
        deleteSnapper();
        switch (getStatus()) {
        case SetPoint2:
            setStatus(SetPoint1);
            break;
        case SetPoint1:
            releaseEntity();
            graphicView->redraw(RS2::RedrawDrawing);
            setStatus(ChooseEntity);
            break;
        default:
            init(-1);
            break;
        }
    }
}

void LC_ActionSplineModifyBase::coordinateEvent(RS_CoordinateEvent* e) {
    if (e == nullptr || m_entity == nullptr || getStatus() == ChooseEntity || !takesNewPoint(getStatus())) {
        return;
    }
    const RS_Vector pos = e->getCoordinate();
    if (pos.valid) {
        onPoint(pos, false);
    }
}

void LC_ActionSplineModifyBase::updateMouseCursor() {
    graphicView->setMouseCursor(getStatus() == ChooseEntity ? RS2::SelectCursor : RS2::CadCursor);
}

bool LC_ActionSplineModifyBase::canEdit(const RS_Entity* /*e*/, QString& /*message*/) const {
    return true;
}

void LC_ActionSplineModifyBase::pickEntity(QMouseEvent* e) {
    RS_Entity* en = catchEntity(e, g_splineTypes);
    if (en == nullptr) {
        RS_DIALOGFACTORY->commandMessage(tr("No spline found. Pick a spline or a spline through points."));
        return;
    }
    if (en->isLocked()) {
        RS_DIALOGFACTORY->commandMessage(tr("The spline is on a locked layer."));
        return;
    }
    QString message;
    if (!canEdit(en, message)) {
        RS_DIALOGFACTORY->commandMessage(message);
        return;
    }
    m_entity = en;
    m_entity->setSelected(true);
    graphicView->redraw(RS2::RedrawDrawing);
    RS_DIALOGFACTORY->updateSelectionWidget(container->countSelected(), container->totalSelectedLength());
    setStatus(SetPoint1);
}

void LC_ActionSplineModifyBase::releaseEntity() {
    if (m_entity != nullptr) {
        m_entity->setSelected(false);
        m_entity = nullptr;
        RS_DIALOGFACTORY->updateSelectionWidget(container->countSelected(), container->totalSelectedLength());
    }
}

std::vector<RS_Vector> LC_ActionSplineModifyBase::points() const {
    return editablePoints(m_entity);
}

bool LC_ActionSplineModifyBase::isClosed() const {
    return closedSpline(m_entity);
}

size_t LC_ActionSplineModifyBase::minimumPoints() const {
    return minimumPointsOf(m_entity);
}

int LC_ActionSplineModifyBase::nearestPoint(const RS_Vector& pos) const {
    const std::vector<RS_Vector> pts = points();
    int best = -1;
    double bestDist = 0.;
    for (size_t k = 0; k < pts.size(); ++k) {
        const double d = pos.distanceTo(pts[k]);
        if (best < 0 || d < bestDist) {
            best = int(k);
            bestDist = d;
        }
    }
    return best;
}

RS_Entity* LC_ActionSplineModifyBase::edited(const Edit& ed) const {
    if (m_entity == nullptr) {
        return nullptr;
    }
    std::vector<RS_Vector> pts = points();
    const size_t n = pts.size();
    std::vector<size_t> removed;
    if (ed.kind == Edit::Insert) {
        if (!ed.point.valid || !LC_SplinePointsEdit::insertAt(pts, ed.i, ed.point)) {
            return nullptr;
        }
    } else {
        removed = (ed.kind == Edit::Remove)
                      ? std::vector<size_t>{ed.i}
                      : LC_SplinePointsEdit::indicesBetween(n, ed.i, ed.j, isClosed(), ed.outside);
        if (!LC_SplinePointsEdit::removeIndices(pts, removed, minimumPoints())) {
            return nullptr;
        }
    }

    if (m_entity->rtti() == RS2::EntitySplinePoints) {
        auto* sp = static_cast<LC_SplinePoints*>(m_entity->clone());
        LC_SplinePointsData& d = sp->getData();
        (usesControlPoints(d) ? d.controlPoints : d.splinePoints) = pts;
        sp->update();
        return sp;
    }
    if (m_entity->rtti() == RS2::EntitySpline) {
        RS_SplineData d = static_cast<const RS_Spline*>(m_entity)->getData();
        std::vector<double> knots;  // empty = uniform knots (RS_Spline::knot)
        if (!d.closed) {
            if (ed.kind == Edit::Insert) {
                knots = LC_SplinePointsEdit::knotsAfterInsert(d.knotslist, n, d.degree, ed.i);
            } else {
                knots = d.knotslist;
                size_t count = n;
                for (auto it = removed.rbegin(); it != removed.rend() && !knots.empty(); ++it, --count) {
                    knots = LC_SplinePointsEdit::knotsAfterRemove(knots, count, d.degree, *it);
                }
            }
        }
        d.controlPoints = pts;  // unwrapped; update() wraps a closed spline again
        d.knotslist = knots;
        auto* s = new RS_Spline(m_entity->getParent(), d);
        s->setLayer(m_entity->getLayer(false));
        s->setPen(m_entity->getPen(false));
        s->update();
        return s;
    }
    return nullptr;
}

void LC_ActionSplineModifyBase::previewEdit(const Edit* ed, const std::vector<RS_Vector>& marks) {
    deletePreview();
    if (ed != nullptr) {
        if (RS_Entity* e = edited(*ed)) {
            preview->addEntity(e);
        }
    }
    const double r = graphicView->toGraphDX(6);
    for (const RS_Vector& m : marks) {
        if (m.valid) {
            preview->addEntity(new RS_Circle(preview.get(), RS_CircleData(m, r)));
        }
    }
    drawPreview();
}

bool LC_ActionSplineModifyBase::applyEdit(const Edit& ed) {
    if (m_entity == nullptr || document == nullptr) {
        return false;
    }
    RS_Entity* ne = edited(ed);
    if (ne == nullptr) {
        return false;
    }
    deletePreview();
    RS_EntityContainer* parent = m_entity->getParent() != nullptr ? m_entity->getParent() : container;
    ne->reparent(parent);
    parent->addEntity(ne);
    m_entity->setSelected(false);
    {
        LC_UndoSection undo(document);
        m_entity->setUndoState(true);
        undo.addUndoable(m_entity);
        undo.addUndoable(ne);
    }
    m_entity = ne;
    m_entity->setSelected(true);
    graphicView->redraw(RS2::RedrawDrawing);
    RS_DIALOGFACTORY->updateSelectionWidget(container->countSelected(), container->totalSelectedLength());
    return true;
}

// ---------------------------------------------------------------------------
// Insert point

LC_ActionSplineInsertPoint::LC_ActionSplineInsertPoint(RS_EntityContainer& container,
                                                       RS_GraphicView& graphicView)
    : LC_ActionSplineModifyBase("Insert spline point", container, graphicView,
                                RS2::ActionDrawSplinePointAdd) {
}

bool LC_ActionSplineInsertPoint::takesNewPoint(int status) const {
    return status == SetPoint2;
}

void LC_ActionSplineInsertPoint::previewAt(const RS_Vector& pos, bool shift) {
    const std::vector<RS_Vector> pts = points();
    if (getStatus() == SetPoint1) {
        const int k = nearestPoint(pos);
        if (k >= 0) {
            previewEdit(nullptr, {pts[k]});
        }
        return;
    }
    if (m_ref >= pts.size()) {
        return;
    }
    Edit ed;
    ed.kind = Edit::Insert;
    ed.i = m_ref + (shift ? 1 : 0);
    ed.point = pos;
    previewEdit(&ed, {pts[m_ref], pos});
}

void LC_ActionSplineInsertPoint::onPoint(const RS_Vector& pos, bool shift) {
    const std::vector<RS_Vector> pts = points();
    if (getStatus() == SetPoint1) {
        const int k = nearestPoint(pos);
        if (k < 0) {
            return;
        }
        m_ref = size_t(k);
        graphicView->moveRelativeZero(pts[m_ref]);
        setStatus(SetPoint2);
        previewEdit(nullptr, {pts[m_ref]});
        return;
    }
    if (m_ref >= pts.size()) {
        setStatus(SetPoint1);
        return;
    }
    Edit ed;
    ed.kind = Edit::Insert;
    ed.i = m_ref + (shift ? 1 : 0);
    ed.point = pos;
    if (applyEdit(ed)) {
        graphicView->moveRelativeZero(pos);
        setStatus(SetPoint1);
    }
}

void LC_ActionSplineInsertPoint::updateMouseButtonHints() {
    switch (getStatus()) {
    case ChooseEntity:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the spline to insert a point into"), tr("Cancel"));
        break;
    case SetPoint1:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the point to insert before"), tr("Back"));
        break;
    case SetPoint2:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Specify the new point (Shift: insert after the selected point)"),
                                            tr("Back"));
        break;
    default:
        RS_DIALOGFACTORY->updateMouseWidget();
        break;
    }
}

// ---------------------------------------------------------------------------
// Append point

LC_ActionSplineAppendPoint::LC_ActionSplineAppendPoint(RS_EntityContainer& container,
                                                       RS_GraphicView& graphicView)
    : LC_ActionSplineModifyBase("Append spline point", container, graphicView,
                                RS2::ActionDrawSplinePointAppend) {
}

bool LC_ActionSplineAppendPoint::takesNewPoint(int status) const {
    return status == SetPoint1 || status == SetPoint2;
}

bool LC_ActionSplineAppendPoint::nearerToStart(const RS_Vector& pos) const {
    const std::vector<RS_Vector> pts = points();
    return !pts.empty() && pos.distanceTo(pts.front()) < pos.distanceTo(pts.back());
}

void LC_ActionSplineAppendPoint::previewAt(const RS_Vector& pos, bool shift) {
    const std::vector<RS_Vector> pts = points();
    if (pts.empty()) {
        return;
    }
    const bool atStart = (getStatus() == SetPoint1 || shift) ? nearerToStart(pos) : m_atStart;
    Edit ed;
    ed.kind = Edit::Insert;
    ed.i = atStart ? 0 : pts.size();
    ed.point = pos;
    previewEdit(&ed, {atStart ? pts.front() : pts.back(), pos});
}

void LC_ActionSplineAppendPoint::onPoint(const RS_Vector& pos, bool shift) {
    const std::vector<RS_Vector> pts = points();
    if (pts.empty()) {
        return;
    }
    const bool atStart = (getStatus() == SetPoint1 || shift) ? nearerToStart(pos) : m_atStart;
    Edit ed;
    ed.kind = Edit::Insert;
    ed.i = atStart ? 0 : pts.size();
    ed.point = pos;
    if (applyEdit(ed)) {
        m_atStart = atStart;
        graphicView->moveRelativeZero(pos);
        setStatus(SetPoint2);
    }
}

void LC_ActionSplineAppendPoint::updateMouseButtonHints() {
    switch (getStatus()) {
    case ChooseEntity:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the spline to add points to"), tr("Cancel"));
        break;
    case SetPoint1:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Specify the new point (added at the nearer end)"), tr("Back"));
        break;
    case SetPoint2:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Specify the next point (Shift: at the nearer end)"), tr("Back"));
        break;
    default:
        RS_DIALOGFACTORY->updateMouseWidget();
        break;
    }
}

// ---------------------------------------------------------------------------
// Remove point

LC_ActionSplineRemovePoint::LC_ActionSplineRemovePoint(RS_EntityContainer& container,
                                                       RS_GraphicView& graphicView)
    : LC_ActionSplineModifyBase("Remove spline point", container, graphicView,
                                RS2::ActionDrawSplinePointRemove) {
}

bool LC_ActionSplineRemovePoint::takesNewPoint(int /*status*/) const {
    return false;
}

bool LC_ActionSplineRemovePoint::canEdit(const RS_Entity* e, QString& message) const {
    const size_t n = editablePoints(e).size();
    const size_t minimum = minimumPointsOf(e);
    if (n <= minimum) {
        message = tr("This spline has %1 points and needs at least %2; no point can be removed.")
                      .arg(n).arg(minimum);
        return false;
    }
    return true;
}

void LC_ActionSplineRemovePoint::previewAt(const RS_Vector& pos, bool /*shift*/) {
    const int k = nearestPoint(pos);
    if (k < 0) {
        return;
    }
    Edit ed;
    ed.kind = Edit::Remove;
    ed.i = size_t(k);
    previewEdit(&ed, {points()[k]});
}

void LC_ActionSplineRemovePoint::onPoint(const RS_Vector& pos, bool /*shift*/) {
    const int k = nearestPoint(pos);
    if (k < 0) {
        return;
    }
    Edit ed;
    ed.kind = Edit::Remove;
    ed.i = size_t(k);
    if (!applyEdit(ed)) {
        RS_DIALOGFACTORY->commandMessage(tr("At least %1 points must remain.").arg(minimumPoints()));
        return;
    }
    if (points().size() <= minimumPoints()) {
        RS_DIALOGFACTORY->commandMessage(tr("No more points can be removed from this spline."));
        releaseEntity();
        graphicView->redraw(RS2::RedrawDrawing);
        setStatus(ChooseEntity);
    }
}

void LC_ActionSplineRemovePoint::updateMouseButtonHints() {
    switch (getStatus()) {
    case ChooseEntity:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the spline to remove points from"), tr("Cancel"));
        break;
    case SetPoint1:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the point to remove"), tr("Back"));
        break;
    default:
        RS_DIALOGFACTORY->updateMouseWidget();
        break;
    }
}

// ---------------------------------------------------------------------------
// Remove between two points

LC_ActionSplineRemoveBetween::LC_ActionSplineRemoveBetween(RS_EntityContainer& container,
                                                           RS_GraphicView& graphicView)
    : LC_ActionSplineModifyBase("Remove spline points between two", container, graphicView,
                                RS2::ActionDrawSplinePointDelTwo) {
}

bool LC_ActionSplineRemoveBetween::takesNewPoint(int /*status*/) const {
    return false;
}

void LC_ActionSplineRemoveBetween::previewAt(const RS_Vector& pos, bool shift) {
    const int k = nearestPoint(pos);
    if (k < 0) {
        return;
    }
    const std::vector<RS_Vector> pts = points();
    if (getStatus() == SetPoint1 || m_first >= pts.size()) {
        previewEdit(nullptr, {pts[k]});
        return;
    }
    Edit ed;
    ed.kind = Edit::RemoveBetween;
    ed.i = m_first;
    ed.j = size_t(k);
    ed.outside = shift && isClosed();
    previewEdit(&ed, {pts[m_first], pts[k]});
}

void LC_ActionSplineRemoveBetween::onPoint(const RS_Vector& pos, bool shift) {
    const int k = nearestPoint(pos);
    if (k < 0) {
        return;
    }
    if (getStatus() == SetPoint1) {
        m_first = size_t(k);
        setStatus(SetPoint2);
        previewEdit(nullptr, {points()[m_first]});
        return;
    }
    if (size_t(k) == m_first) {
        return;
    }
    Edit ed;
    ed.kind = Edit::RemoveBetween;
    ed.i = m_first;
    ed.j = size_t(k);
    ed.outside = shift && isClosed();
    if (applyEdit(ed)) {
        setStatus(SetPoint1);
        return;
    }
    if (LC_SplinePointsEdit::indicesBetween(points().size(), ed.i, ed.j, isClosed(), ed.outside).empty()) {
        RS_DIALOGFACTORY->commandMessage(tr("There are no points between the two selected points."));
    } else {
        RS_DIALOGFACTORY->commandMessage(tr("At least %1 points must remain.").arg(minimumPoints()));
    }
}

void LC_ActionSplineRemoveBetween::updateMouseButtonHints() {
    switch (getStatus()) {
    case ChooseEntity:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the spline to remove points from"), tr("Cancel"));
        break;
    case SetPoint1:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Select the first point"), tr("Back"));
        break;
    case SetPoint2:
        RS_DIALOGFACTORY->updateMouseWidget(isClosed() ? tr("Select the second point (Shift: remove the other side)")
                                                       : tr("Select the second point"),
                                            tr("Back"));
        break;
    default:
        RS_DIALOGFACTORY->updateMouseWidget();
        break;
    }
}

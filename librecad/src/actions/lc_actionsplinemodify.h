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
 * Context: editing the points of a spline that is already drawn - insert a point, append points at an
 * end, remove a point, remove the points between two. Backport of master's four actions
 * (actions/drawing/draw/spline/lc_action_spline_{add_point,append_point,remove_points,remove_between}),
 * rewritten for this branch's action API (RS_PreviewActionInterface + QMouseEvent) the way
 * lc_actiondrawhyperbolafp was; master sits on LC_UndoableDocumentModificationAction/LC_MouseEvent.
 * Works on LC_SplinePoints and RS_Spline. The points the user picks are the grips shown on the selected
 * entity: the points the curve passes through for a freshly drawn spline through points, its control
 * points once it went through DXF (the filter reads a degree-2 SPLINE back as LC_SplinePoints with
 * cut = true), the control points of an RS_Spline.
 * Differences from master (why): see lc_actionsplinemodify.cpp and lc_splinepointsedit.h.
 */

#ifndef LC_ACTIONSPLINEMODIFY_H
#define LC_ACTIONSPLINEMODIFY_H

#include <vector>

#include "rs_previewactioninterface.h"
#include "rs_vector.h"

class RS_Entity;

class LC_ActionSplineModifyBase : public RS_PreviewActionInterface {
    Q_OBJECT
public:
    enum Status {
        ChooseEntity, /**< Choosing the spline. */
        SetPoint1,    /**< First point of the edit. */
        SetPoint2     /**< Second point of the edit. */
    };

    void init(int status = 0) override;
    void finish(bool updateTB = true) override;
    void resume() override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void coordinateEvent(RS_CoordinateEvent* e) override;
    void updateMouseCursor() override;

protected:
    LC_ActionSplineModifyBase(const char* name, RS_EntityContainer& container,
                              RS_GraphicView& graphicView, RS2::ActionType type);
    ~LC_ActionSplineModifyBase() override;

    /** One edit of the point list; Append is an Insert at 0 or at size(). */
    struct Edit {
        enum Kind { Insert, Remove, RemoveBetween };
        Kind kind = Insert;
        size_t i = 0;          /**< Insert: index the new point gets. Remove/RemoveBetween: (first) point. */
        size_t j = 0;          /**< RemoveBetween: second point. */
        bool outside = false;  /**< RemoveBetween on a closed spline: remove the other arc. */
        RS_Vector point{false};/**< Insert: the new point. */
    };

    /** Whether this status takes a new position (snapped, typed coordinates allowed)
     *  rather than picking one of the existing points. */
    virtual bool takesNewPoint(int status) const = 0;
    /** Preview for the mouse at pos (snapped for new points, raw otherwise). */
    virtual void previewAt(const RS_Vector& pos, bool shift) = 0;
    /** Left click at pos, or a typed coordinate when takesNewPoint(). */
    virtual void onPoint(const RS_Vector& pos, bool shift) = 0;
    /** Extra condition on the picked spline; the message is shown when it fails. */
    virtual bool canEdit(const RS_Entity* e, QString& message) const;

    std::vector<RS_Vector> points() const;
    bool isClosed() const;
    size_t minimumPoints() const;
    /** Index of the existing point nearest to pos, -1 without a spline. */
    int nearestPoint(const RS_Vector& pos) const;
    /** The spline after the edit (not in the document), nullptr when the edit is not possible. */
    RS_Entity* edited(const Edit& ed) const;
    /** Preview of the edited spline (if possible) plus markers at the given points. */
    void previewEdit(const Edit* ed, const std::vector<RS_Vector>& marks);
    /** Replaces the spline by the edited one in one undo cycle. False when not possible. */
    bool applyEdit(const Edit& ed);
    /** Deselects the spline and forgets it. */
    void releaseEntity();

    RS_Entity* m_entity = nullptr;

private:
    void pickEntity(QMouseEvent* e);
    RS_Vector eventPosition(QMouseEvent* e);
};

/** Insert a point before (Shift: after) a picked point. master: LC_ActionSplineAddPoint. */
class LC_ActionSplineInsertPoint : public LC_ActionSplineModifyBase {
    Q_OBJECT
public:
    LC_ActionSplineInsertPoint(RS_EntityContainer& container, RS_GraphicView& graphicView);
    void updateMouseButtonHints() override;
protected:
    bool takesNewPoint(int status) const override;
    void previewAt(const RS_Vector& pos, bool shift) override;
    void onPoint(const RS_Vector& pos, bool shift) override;
private:
    size_t m_ref = 0;
};

/** Add points at the end nearer to the first new point, then keep adding there
 *  (Shift: at the end nearer to the click). master: LC_ActionSplineAppendPoint. */
class LC_ActionSplineAppendPoint : public LC_ActionSplineModifyBase {
    Q_OBJECT
public:
    LC_ActionSplineAppendPoint(RS_EntityContainer& container, RS_GraphicView& graphicView);
    void updateMouseButtonHints() override;
protected:
    bool takesNewPoint(int status) const override;
    void previewAt(const RS_Vector& pos, bool shift) override;
    void onPoint(const RS_Vector& pos, bool shift) override;
private:
    bool nearerToStart(const RS_Vector& pos) const;
    bool m_atStart = false;
};

/** Remove picked points one by one. master: LC_ActionRemoveSplinePoints. */
class LC_ActionSplineRemovePoint : public LC_ActionSplineModifyBase {
    Q_OBJECT
public:
    LC_ActionSplineRemovePoint(RS_EntityContainer& container, RS_GraphicView& graphicView);
    void updateMouseButtonHints() override;
protected:
    bool takesNewPoint(int status) const override;
    void previewAt(const RS_Vector& pos, bool shift) override;
    void onPoint(const RS_Vector& pos, bool shift) override;
    bool canEdit(const RS_Entity* e, QString& message) const override;
};

/** Remove the points between two picked points (closed spline + Shift: the other arc).
 *  master: LC_ActionSplineRemoveBetween. */
class LC_ActionSplineRemoveBetween : public LC_ActionSplineModifyBase {
    Q_OBJECT
public:
    LC_ActionSplineRemoveBetween(RS_EntityContainer& container, RS_GraphicView& graphicView);
    void updateMouseButtonHints() override;
protected:
    bool takesNewPoint(int status) const override;
    void previewAt(const RS_Vector& pos, bool shift) override;
    void onPoint(const RS_Vector& pos, bool shift) override;
private:
    size_t m_first = 0;
};

#endif

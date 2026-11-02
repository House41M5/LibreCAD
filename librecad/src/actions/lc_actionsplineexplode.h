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
 * Context: turn a spline into a polyline / lines and a polyline into a spline (2026-11-02, plans-win
 * librecad-factory-drawings step 20: user asked whether people explode splines in practice - yes, for
 * CNC/laser cutting and for programs or old DXF without SPLINE - then chose to backport master's
 * actions/drawing/draw/spline/lc_action_spline_modify_explode and lc_action_spline_from_polyline).
 * Rewritten for this branch's action API (RS_PreviewActionInterface + QMouseEvent) like
 * lc_actionsplinemodify; master sits on LC_UndoableDocumentModificationAction/LC_MouseEvent.
 * Differences from master (why):
 *  - explode keeps the spline's own layer and pen by default (master: current layer and pen). Testing
 *    the generic Explode showed what losing them means: colours gone, HIDDENX2 hidden lines solid.
 *  - explode removes the spline by default (master keeps it): a cutter would follow both.
 *  - explode resolution is per control point / per quadratic piece through lc::splineStrokePoints(),
 *    the same points LibreCAD draws; master passes one count for a whole RS_Spline (8 segments).
 *  - spline from polyline: the extra points on a straight segment divide it into (n+1) parts; master
 *    divides by n, so its last "middle" point is the segment end and the end point comes twice.
 *  - spline from polyline replaces the polyline by default (master keeps it), as PEDIT Spline does.
 * Arcs mode (step 21, not in master): the spline becomes tangent-continuous arcs within a tolerance
 * (lc_biarcfit.h), like AutoCAD with PLINECONVERTMODE=1 - fewer pieces than lines and no corners for a
 * cutter to stop at. Off by default, as PLINECONVERTMODE is 0 by default.
 */
#ifndef LC_ACTIONSPLINEEXPLODE_H
#define LC_ACTIONSPLINEEXPLODE_H

#include <vector>

#include "rs_previewactioninterface.h"
#include "rs_vector.h"

class RS_Entity;
class RS_Polyline;

/** Pick a spline (or spline through points): it becomes a polyline or separate lines. */
class LC_ActionSplineExplode : public RS_PreviewActionInterface {
    Q_OBJECT
public:
    LC_ActionSplineExplode(RS_EntityContainer& container, RS_GraphicView& graphicView);
    void init(int status = 0) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void updateMouseButtonHints() override;
    void updateMouseCursor() override;

    bool isToPolyline() const { return m_toPolyline; }
    void setToPolyline(bool b) { m_toPolyline = b; }
    bool isKeepOriginals() const { return m_keepOriginals; }
    void setKeepOriginals(bool b) { m_keepOriginals = b; }
    bool isUseCurrentLayer() const { return m_useCurrentLayer; }
    void setUseCurrentLayer(bool b) { m_useCurrentLayer = b; }
    bool isUseCurrentAttributes() const { return m_useCurrentAttributes; }
    void setUseCurrentAttributes(bool b) { m_useCurrentAttributes = b; }
    bool isUseCustomSegments() const { return m_useCustomSegments; }
    void setUseCustomSegments(bool b) { m_useCustomSegments = b; }
    int getCustomSegments() const { return m_customSegments; }
    void setCustomSegments(int n) { m_customSegments = n; }
    /** $SPLINESEGS of the drawing (8 when unset): what is used without custom segments. */
    int getDrawingSegments() const;
    /** Arcs mode (step 21): tangent-continuous arcs within the tolerance instead of lines. */
    bool isToArcs() const { return m_toArcs; }
    void setToArcs(bool b) { m_toArcs = b; }
    double getArcTolerance() const { return m_arcTolerance; }
    void setArcTolerance(double t);

    /** Smallest arc tolerance accepted (drawing units); smaller values are raised to it. */
    static constexpr double kMinArcTolerance = 1e-6;

protected:
    void createOptionsWidget() override;

private:
    /**
     * The entities that replace e (not in the document), empty when e cannot be exploded.
     * deviation (if given): Arcs mode - the largest deviation measured; otherwise 0.
     */
    std::vector<RS_Entity*> exploded(const RS_Entity* e, RS_EntityContainer* parent, double* deviation = nullptr) const;
    std::vector<RS_Entity*> explodedToArcs(const RS_Entity* e, RS_EntityContainer* parent, double* deviation) const;
    void explodeEntity(RS_Entity* e);

    bool m_toPolyline = true;
    bool m_keepOriginals = false;
    bool m_useCurrentLayer = false;
    bool m_useCurrentAttributes = false;
    bool m_useCustomSegments = false;
    int m_customSegments = 8;
    bool m_toArcs = false;
    double m_arcTolerance = 0.01;
};

/** Pick a polyline: it becomes a spline (control points) or a spline through its vertices. */
class LC_ActionSplineFromPolyline : public RS_PreviewActionInterface {
    Q_OBJECT
public:
    LC_ActionSplineFromPolyline(RS_EntityContainer& container, RS_GraphicView& graphicView);
    void init(int status = 0) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void updateMouseButtonHints() override;
    void updateMouseCursor() override;

    int getDegree() const { return m_degree; }
    void setDegree(int d) { m_degree = d; }
    int getMidPoints() const { return m_midPoints; }
    void setMidPoints(int n) { m_midPoints = n; }
    bool isUseFitPoints() const { return m_useFitPoints; }
    void setUseFitPoints(bool b) { m_useFitPoints = b; }
    bool isKeepOriginals() const { return m_keepOriginals; }
    void setKeepOriginals(bool b) { m_keepOriginals = b; }
    bool isUseCurrentLayer() const { return m_useCurrentLayer; }
    void setUseCurrentLayer(bool b) { m_useCurrentLayer = b; }
    bool isUseCurrentAttributes() const { return m_useCurrentAttributes; }
    void setUseCurrentAttributes(bool b) { m_useCurrentAttributes = b; }

    /** Polyline vertices plus m_midPoints points inside each segment (closed: last == first dropped). */
    static std::vector<RS_Vector> pointsFromPolyline(const RS_Polyline* polyline, int midPoints);

protected:
    void createOptionsWidget() override;

private:
    /** The spline for the polyline (not in the document), nullptr when there are too few points. */
    RS_Entity* splineFor(const RS_Polyline* polyline, RS_EntityContainer* parent) const;
    void convert(RS_Polyline* polyline);

    int m_degree = 3;
    int m_midPoints = 2;
    bool m_useFitPoints = true;
    bool m_keepOriginals = false;
    bool m_useCurrentLayer = false;
    bool m_useCurrentAttributes = false;
};

#endif

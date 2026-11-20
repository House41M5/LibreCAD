/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Hatch by picking a point inside a closed area (AutoCAD HATCH "Pick internal point").
** kai fork (branch kai/thai-fonts-2216), 2026-11-20.
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
**********************************************************************/

#ifndef LC_HATCHPICK_H
#define LC_HATCHPICK_H

class QString;
class RS_EntityContainer;
class RS_Hatch;
class RS_Vector;

namespace LC_HatchPick {

/**
 * Finds the smallest closed area around p made by the visible lines, arcs, circles, ellipses, polylines, splines
 * and block contents of container that lie in the window vMin..vMax (the screen - what is off screen is not looked
 * at, as in AutoCAD), and adds its boundary to hatch as loops: the outer boundary, plus every closed shape inside it
 * that does not touch it (islands; RS_Hatch fills even-odd, so an island inside an island is filled again =
 * AutoCAD island style "Normal"). Objects that cross are cut where they cross; ends closer than 2e-7 are one
 * point; open ends (lines that lead nowhere) are ignored. Text, dimensions, hatches, points and solids are not
 * boundaries, but the box around TEXT / MTEXT that lies wholly in the area is kept clear as an island (as AutoCAD
 * does; overlapping boxes as one; a box touching the boundary or an island is left out).
 *
 * @param islands  number of island loops added (may be nullptr)
 * @return false with *error set when no closed area surrounds p; hatch is then left as it was
 */
bool addLoops(RS_EntityContainer* container, const RS_Vector& p, const RS_Vector& vMin, const RS_Vector& vMax,
              RS_Hatch* hatch, QString* error, int* islands = nullptr);

}

#endif

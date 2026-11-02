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
// Context: tool options of the spline-from-polyline action (lc_actionsplineexplode.h), 2026-11-02 step 20.
// Settings: /Draw/SplineFromPolyline*. "Through Vertices" only means something with degree 2
// (LC_SplinePoints); this branch's RS_Spline has no fit points, so the box is disabled otherwise.
#ifndef LC_SPLINEFROMPOLYLINEOPTIONS_H
#define LC_SPLINEFROMPOLYLINEOPTIONS_H

#include "lc_actionoptionswidget.h"

class LC_ActionSplineFromPolyline;
namespace Ui {
class LC_SplineFromPolylineOptions;
}

class LC_SplineFromPolylineOptions : public LC_ActionOptionsWidget {
    Q_OBJECT
public:
    explicit LC_SplineFromPolylineOptions(QWidget* parent = nullptr);
    ~LC_SplineFromPolylineOptions() override;

protected:
    void doSetAction(RS_ActionInterface* a, bool update) override;
    void doSaveSettings() override;
    bool checkActionRttiValid(RS2::ActionType actionType) override;
    QString getSettingsOptionNamePrefix() override { return "/SplineFromPolyline"; }

protected slots:
    void languageChange() override;
    void onDegreeChanged(int value);
    void onMidPointsChanged(int value);
    void onFitPointsClicked(bool value);
    void onKeepOriginalsClicked(bool value);
    void onCurrentLayerClicked(bool value);
    void onCurrentAttrClicked(bool value);

private:
    void apply(int degree, int midPoints, bool fit, bool keep, bool layer, bool attr);
    Ui::LC_SplineFromPolylineOptions* ui;
    LC_ActionSplineFromPolyline* action = nullptr;
};

#endif

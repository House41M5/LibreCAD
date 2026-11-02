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
// Context: tool options of the explode spline action (lc_actionsplineexplode.h), 2026-11-02 step 20.
// Settings: /Draw/SplineExplode*. Defaults differ from master on purpose - see the action's header.
// Step 21 added Arcs + Tolerance (ToArcs, ArcTolerance); Segments only applies without Arcs and the other
// way round, so each pair is disabled while the other mode is on.
#ifndef LC_SPLINEEXPLODEOPTIONS_H
#define LC_SPLINEEXPLODEOPTIONS_H

#include "lc_actionoptionswidget.h"

class LC_ActionSplineExplode;
namespace Ui {
class LC_SplineExplodeOptions;
}

class LC_SplineExplodeOptions : public LC_ActionOptionsWidget {
    Q_OBJECT
public:
    explicit LC_SplineExplodeOptions(QWidget* parent = nullptr);
    ~LC_SplineExplodeOptions() override;

protected:
    void doSetAction(RS_ActionInterface* a, bool update) override;
    void doSaveSettings() override;
    bool checkActionRttiValid(RS2::ActionType actionType) override;
    QString getSettingsOptionNamePrefix() override { return "/SplineExplode"; }

protected slots:
    void languageChange() override;
    void onPolylineClicked(bool value);
    void onArcsClicked(bool value);
    void onToleranceEditingFinished();
    void onCustomSegmentsClicked(bool value);
    void onSegmentsChanged(int value);
    void onKeepOriginalsClicked(bool value);
    void onCurrentLayerClicked(bool value);
    void onCurrentAttrClicked(bool value);

private:
    struct Values {
        bool polyline = true;
        bool arcs = false;
        double tolerance = 0.01;
        bool custom = false;
        int segments = 8;
        bool keep = false;
        bool layer = false;
        bool attr = false;
    };
    Values fromAction() const;
    void apply(const Values& v);
    Ui::LC_SplineExplodeOptions* ui;
    LC_ActionSplineExplode* action = nullptr;
};

#endif

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
// Context: see lc_splinefrompolylineoptions.h.
#include "lc_splinefrompolylineoptions.h"

#include "lc_actionsplineexplode.h"
#include "ui_lc_splinefrompolylineoptions.h"

LC_SplineFromPolylineOptions::LC_SplineFromPolylineOptions(QWidget* parent)
    : LC_ActionOptionsWidget(parent), ui(new Ui::LC_SplineFromPolylineOptions) {
    ui->setupUi(this);
    connect(ui->sbDegree, QOverload<int>::of(&QSpinBox::valueChanged), this, &LC_SplineFromPolylineOptions::onDegreeChanged);
    connect(ui->sbMidPoints, QOverload<int>::of(&QSpinBox::valueChanged), this, &LC_SplineFromPolylineOptions::onMidPointsChanged);
    connect(ui->cbFitPoints, &QCheckBox::clicked, this, &LC_SplineFromPolylineOptions::onFitPointsClicked);
    connect(ui->cbKeepOriginals, &QCheckBox::clicked, this, &LC_SplineFromPolylineOptions::onKeepOriginalsClicked);
    connect(ui->cbCurrentLayer, &QCheckBox::clicked, this, &LC_SplineFromPolylineOptions::onCurrentLayerClicked);
    connect(ui->cbCurrentAttr, &QCheckBox::clicked, this, &LC_SplineFromPolylineOptions::onCurrentAttrClicked);
}

LC_SplineFromPolylineOptions::~LC_SplineFromPolylineOptions() {
    delete ui;
}

void LC_SplineFromPolylineOptions::languageChange() {
    ui->retranslateUi(this);
}

bool LC_SplineFromPolylineOptions::checkActionRttiValid(RS2::ActionType actionType) {
    return actionType == RS2::ActionDrawSplineFromPolyline;
}

void LC_SplineFromPolylineOptions::doSetAction(RS_ActionInterface* a, bool update) {
    action = dynamic_cast<LC_ActionSplineFromPolyline*>(a);
    if (action == nullptr) {
        return;
    }
    if (update) {
        apply(action->getDegree(), action->getMidPoints(), action->isUseFitPoints(), action->isKeepOriginals(),
              action->isUseCurrentLayer(), action->isUseCurrentAttributes());
    } else {
        apply(loadInt("Degree", 3), loadInt("MidPoints", 2), loadBool("UseFitPoints", true),
              loadBool("KeepOriginals", false), loadBool("UseCurrentLayer", false), loadBool("UseCurrentAttributes", false));
    }
}

void LC_SplineFromPolylineOptions::apply(int degree, int midPoints, bool fit, bool keep, bool layer, bool attr) {
    degree = qBound(1, degree, 3);
    midPoints = qBound(0, midPoints, 50);
    action->setDegree(degree);
    action->setMidPoints(midPoints);
    action->setUseFitPoints(fit);
    action->setKeepOriginals(keep);
    action->setUseCurrentLayer(layer);
    action->setUseCurrentAttributes(attr);
    {
        const QSignalBlocker b1(ui->sbDegree);
        const QSignalBlocker b2(ui->sbMidPoints);
        ui->sbDegree->setValue(degree);
        ui->sbMidPoints->setValue(midPoints);
    }
    ui->cbFitPoints->setChecked(fit);
    ui->cbFitPoints->setEnabled(degree == 2);
    ui->cbKeepOriginals->setChecked(keep);
    ui->cbCurrentLayer->setChecked(layer);
    ui->cbCurrentAttr->setChecked(attr);
}

void LC_SplineFromPolylineOptions::doSaveSettings() {
    if (action == nullptr) {
        return;
    }
    save("Degree", action->getDegree());
    save("MidPoints", action->getMidPoints());
    save("UseFitPoints", action->isUseFitPoints());
    save("KeepOriginals", action->isKeepOriginals());
    save("UseCurrentLayer", action->isUseCurrentLayer());
    save("UseCurrentAttributes", action->isUseCurrentAttributes());
}

void LC_SplineFromPolylineOptions::onDegreeChanged(int value) {
    if (action != nullptr) {
        action->setDegree(value);
        ui->cbFitPoints->setEnabled(value == 2);
    }
}

void LC_SplineFromPolylineOptions::onMidPointsChanged(int value) {
    if (action != nullptr) {
        action->setMidPoints(value);
    }
}

void LC_SplineFromPolylineOptions::onFitPointsClicked(bool value) {
    if (action != nullptr) {
        action->setUseFitPoints(value);
    }
}

void LC_SplineFromPolylineOptions::onKeepOriginalsClicked(bool value) {
    if (action != nullptr) {
        action->setKeepOriginals(value);
    }
}

void LC_SplineFromPolylineOptions::onCurrentLayerClicked(bool value) {
    if (action != nullptr) {
        action->setUseCurrentLayer(value);
    }
}

void LC_SplineFromPolylineOptions::onCurrentAttrClicked(bool value) {
    if (action != nullptr) {
        action->setUseCurrentAttributes(value);
    }
}

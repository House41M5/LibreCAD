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
    connect(ui->cbTolerance, &QCheckBox::clicked, this, &LC_SplineFromPolylineOptions::onToleranceClicked);
    connect(ui->leTolerance, &QLineEdit::editingFinished, this, &LC_SplineFromPolylineOptions::onToleranceEditingFinished);
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
        apply(fromAction());
        return;
    }
    Values v;
    v.degree = loadInt("Degree", 3);
    v.midPoints = loadInt("MidPoints", 2);
    v.fit = loadBool("UseFitPoints", true);
    v.toleranceMode = loadBool("UseTolerance", false);
    double tolerance = 0.01;
    if (toDouble(load("Tolerance", "0.01"), tolerance, 0.01, true)) {
        v.tolerance = tolerance;
    }
    v.keep = loadBool("KeepOriginals", false);
    v.layer = loadBool("UseCurrentLayer", false);
    v.attr = loadBool("UseCurrentAttributes", false);
    apply(v);
}

LC_SplineFromPolylineOptions::Values LC_SplineFromPolylineOptions::fromAction() const {
    Values v;
    v.degree = action->getDegree();
    v.midPoints = action->getMidPoints();
    v.fit = action->isUseFitPoints();
    v.toleranceMode = action->isUseTolerance();
    v.tolerance = action->getTolerance();
    v.keep = action->isKeepOriginals();
    v.layer = action->isUseCurrentLayer();
    v.attr = action->isUseCurrentAttributes();
    return v;
}

void LC_SplineFromPolylineOptions::apply(const Values& v) {
    const int degree = qBound(1, v.degree, 3);
    const int midPoints = qBound(0, v.midPoints, 50);
    action->setDegree(degree);
    action->setMidPoints(midPoints);
    action->setUseFitPoints(v.fit);
    action->setUseTolerance(v.toleranceMode);
    action->setTolerance(v.tolerance);
    action->setKeepOriginals(v.keep);
    action->setUseCurrentLayer(v.layer);
    action->setUseCurrentAttributes(v.attr);
    {
        const QSignalBlocker b1(ui->sbDegree);
        const QSignalBlocker b2(ui->sbMidPoints);
        ui->sbDegree->setValue(degree);
        ui->sbMidPoints->setValue(midPoints);
    }
    // Tolerance chooses the points itself: Mid Points and Through Vertices do not apply then
    ui->lMidPoints->setEnabled(!v.toleranceMode);
    ui->sbMidPoints->setEnabled(!v.toleranceMode);
    ui->cbFitPoints->setChecked(v.fit);
    ui->cbFitPoints->setEnabled(degree == 2 && !v.toleranceMode);
    ui->cbTolerance->setChecked(v.toleranceMode);
    ui->leTolerance->setText(fromDouble(action->getTolerance())); // as raised to the minimum
    ui->leTolerance->setEnabled(v.toleranceMode);
    ui->cbKeepOriginals->setChecked(v.keep);
    ui->cbCurrentLayer->setChecked(v.layer);
    ui->cbCurrentAttr->setChecked(v.attr);
}

void LC_SplineFromPolylineOptions::doSaveSettings() {
    if (action == nullptr) {
        return;
    }
    save("Degree", action->getDegree());
    save("MidPoints", action->getMidPoints());
    save("UseFitPoints", action->isUseFitPoints());
    save("UseTolerance", action->isUseTolerance());
    save("Tolerance", fromDouble(action->getTolerance()));
    save("KeepOriginals", action->isKeepOriginals());
    save("UseCurrentLayer", action->isUseCurrentLayer());
    save("UseCurrentAttributes", action->isUseCurrentAttributes());
}

void LC_SplineFromPolylineOptions::onDegreeChanged(int value) {
    if (action != nullptr) {
        action->setDegree(value);
        ui->cbFitPoints->setEnabled(value == 2 && !action->isUseTolerance());
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

void LC_SplineFromPolylineOptions::onToleranceClicked(bool value) {
    if (action != nullptr) {
        Values v = fromAction();
        v.toleranceMode = value;
        apply(v);
    }
}

void LC_SplineFromPolylineOptions::onToleranceEditingFinished() {
    if (action == nullptr) {
        return;
    }
    double tolerance = action->getTolerance();
    if (toDouble(ui->leTolerance->text(), tolerance, 0.01, true)) {
        action->setTolerance(tolerance);
    }
    ui->leTolerance->setText(fromDouble(action->getTolerance()));
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

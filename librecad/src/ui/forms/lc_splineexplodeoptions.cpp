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
// Context: see lc_splineexplodeoptions.h.
#include "lc_splineexplodeoptions.h"

#include "lc_actionsplineexplode.h"
#include "ui_lc_splineexplodeoptions.h"

LC_SplineExplodeOptions::LC_SplineExplodeOptions(QWidget* parent)
    : LC_ActionOptionsWidget(parent), ui(new Ui::LC_SplineExplodeOptions) {
    ui->setupUi(this);
    connect(ui->cbPolyline, &QCheckBox::clicked, this, &LC_SplineExplodeOptions::onPolylineClicked);
    connect(ui->cbArcs, &QCheckBox::clicked, this, &LC_SplineExplodeOptions::onArcsClicked);
    connect(ui->leTolerance, &QLineEdit::editingFinished, this, &LC_SplineExplodeOptions::onToleranceEditingFinished);
    connect(ui->cbCustomSegments, &QCheckBox::clicked, this, &LC_SplineExplodeOptions::onCustomSegmentsClicked);
    connect(ui->sbSegments, QOverload<int>::of(&QSpinBox::valueChanged), this, &LC_SplineExplodeOptions::onSegmentsChanged);
    connect(ui->cbKeepOriginals, &QCheckBox::clicked, this, &LC_SplineExplodeOptions::onKeepOriginalsClicked);
    connect(ui->cbCurrentLayer, &QCheckBox::clicked, this, &LC_SplineExplodeOptions::onCurrentLayerClicked);
    connect(ui->cbCurrentAttr, &QCheckBox::clicked, this, &LC_SplineExplodeOptions::onCurrentAttrClicked);
}

LC_SplineExplodeOptions::~LC_SplineExplodeOptions() {
    delete ui;
}

void LC_SplineExplodeOptions::languageChange() {
    ui->retranslateUi(this);
}

bool LC_SplineExplodeOptions::checkActionRttiValid(RS2::ActionType actionType) {
    return actionType == RS2::ActionDrawSplineExplode;
}

LC_SplineExplodeOptions::Values LC_SplineExplodeOptions::fromAction() const {
    Values v;
    v.polyline = action->isToPolyline();
    v.arcs = action->isToArcs();
    v.tolerance = action->getArcTolerance();
    v.custom = action->isUseCustomSegments();
    v.segments = action->getCustomSegments();
    v.keep = action->isKeepOriginals();
    v.layer = action->isUseCurrentLayer();
    v.attr = action->isUseCurrentAttributes();
    return v;
}

void LC_SplineExplodeOptions::doSetAction(RS_ActionInterface* a, bool update) {
    action = dynamic_cast<LC_ActionSplineExplode*>(a);
    if (action == nullptr) {
        return;
    }
    if (update) {
        apply(fromAction());
        return;
    }
    Values v;
    v.polyline = loadBool("ToPolyline", true);
    v.arcs = loadBool("ToArcs", false);
    double tolerance = 0.01;
    if (toDouble(load("ArcTolerance", "0.01"), tolerance, 0.01, true)) {
        v.tolerance = tolerance;
    }
    v.custom = loadBool("UseCustomSegments", false);
    v.segments = loadInt("CustomSegments", 8);
    v.keep = loadBool("KeepOriginals", false);
    v.layer = loadBool("UseCurrentLayer", false);
    v.attr = loadBool("UseCurrentAttributes", false);
    apply(v);
}

void LC_SplineExplodeOptions::apply(const Values& v) {
    action->setToPolyline(v.polyline);
    action->setToArcs(v.arcs);
    action->setArcTolerance(v.tolerance);
    action->setUseCustomSegments(v.custom);
    action->setCustomSegments(v.segments);
    action->setKeepOriginals(v.keep);
    action->setUseCurrentLayer(v.layer);
    action->setUseCurrentAttributes(v.attr);
    ui->cbPolyline->setChecked(v.polyline);
    ui->cbArcs->setChecked(v.arcs);
    ui->leTolerance->setText(fromDouble(action->getArcTolerance())); // as raised to the minimum
    ui->lTolerance->setEnabled(v.arcs);
    ui->leTolerance->setEnabled(v.arcs);
    ui->cbCustomSegments->setChecked(v.custom);
    ui->cbCustomSegments->setEnabled(!v.arcs);
    {
        const QSignalBlocker block(ui->sbSegments);
        // without custom segments the box shows what is used: the drawing's $SPLINESEGS
        ui->sbSegments->setValue(v.custom ? v.segments : action->getDrawingSegments());
    }
    ui->sbSegments->setEnabled(v.custom && !v.arcs);
    ui->cbKeepOriginals->setChecked(v.keep);
    ui->cbCurrentLayer->setChecked(v.layer);
    ui->cbCurrentAttr->setChecked(v.attr);
}

void LC_SplineExplodeOptions::doSaveSettings() {
    if (action == nullptr) {
        return;
    }
    save("ToPolyline", action->isToPolyline());
    save("ToArcs", action->isToArcs());
    save("ArcTolerance", fromDouble(action->getArcTolerance()));
    save("UseCustomSegments", action->isUseCustomSegments());
    save("CustomSegments", action->getCustomSegments());
    save("KeepOriginals", action->isKeepOriginals());
    save("UseCurrentLayer", action->isUseCurrentLayer());
    save("UseCurrentAttributes", action->isUseCurrentAttributes());
}

void LC_SplineExplodeOptions::onPolylineClicked(bool value) {
    if (action != nullptr) {
        action->setToPolyline(value);
    }
}

void LC_SplineExplodeOptions::onArcsClicked(bool value) {
    if (action != nullptr) {
        Values v = fromAction();
        v.arcs = value;
        apply(v);
    }
}

void LC_SplineExplodeOptions::onToleranceEditingFinished() {
    if (action == nullptr) {
        return;
    }
    double tolerance = action->getArcTolerance();
    if (toDouble(ui->leTolerance->text(), tolerance, 0.01, true)) {
        action->setArcTolerance(tolerance);
    }
    ui->leTolerance->setText(fromDouble(action->getArcTolerance()));
}

void LC_SplineExplodeOptions::onCustomSegmentsClicked(bool value) {
    if (action != nullptr) {
        Values v = fromAction();
        v.custom = value;
        apply(v);
    }
}

void LC_SplineExplodeOptions::onSegmentsChanged(int value) {
    if (action != nullptr && action->isUseCustomSegments()) {
        action->setCustomSegments(value);
    }
}

void LC_SplineExplodeOptions::onKeepOriginalsClicked(bool value) {
    if (action != nullptr) {
        action->setKeepOriginals(value);
    }
}

void LC_SplineExplodeOptions::onCurrentLayerClicked(bool value) {
    if (action != nullptr) {
        action->setUseCurrentLayer(value);
    }
}

void LC_SplineExplodeOptions::onCurrentAttrClicked(bool value) {
    if (action != nullptr) {
        action->setUseCurrentAttributes(value);
    }
}

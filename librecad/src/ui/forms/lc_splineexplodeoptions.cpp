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

void LC_SplineExplodeOptions::doSetAction(RS_ActionInterface* a, bool update) {
    action = dynamic_cast<LC_ActionSplineExplode*>(a);
    if (action == nullptr) {
        return;
    }
    if (update) {
        apply(action->isToPolyline(), action->isUseCustomSegments(), action->getCustomSegments(),
              action->isKeepOriginals(), action->isUseCurrentLayer(), action->isUseCurrentAttributes());
    } else {
        apply(loadBool("ToPolyline", true), loadBool("UseCustomSegments", false), loadInt("CustomSegments", 8),
              loadBool("KeepOriginals", false), loadBool("UseCurrentLayer", false), loadBool("UseCurrentAttributes", false));
    }
}

void LC_SplineExplodeOptions::apply(bool polyline, bool custom, int segments, bool keep, bool layer, bool attr) {
    action->setToPolyline(polyline);
    action->setUseCustomSegments(custom);
    action->setCustomSegments(segments);
    action->setKeepOriginals(keep);
    action->setUseCurrentLayer(layer);
    action->setUseCurrentAttributes(attr);
    ui->cbPolyline->setChecked(polyline);
    ui->cbCustomSegments->setChecked(custom);
    {
        const QSignalBlocker block(ui->sbSegments);
        // without custom segments the box shows what is used: the drawing's $SPLINESEGS
        ui->sbSegments->setValue(custom ? segments : action->getDrawingSegments());
    }
    ui->sbSegments->setEnabled(custom);
    ui->cbKeepOriginals->setChecked(keep);
    ui->cbCurrentLayer->setChecked(layer);
    ui->cbCurrentAttr->setChecked(attr);
}

void LC_SplineExplodeOptions::doSaveSettings() {
    if (action == nullptr) {
        return;
    }
    save("ToPolyline", action->isToPolyline());
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

void LC_SplineExplodeOptions::onCustomSegmentsClicked(bool value) {
    if (action != nullptr) {
        apply(action->isToPolyline(), value, action->getCustomSegments(), action->isKeepOriginals(),
              action->isUseCurrentLayer(), action->isUseCurrentAttributes());
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

/*****************************************************************************/
/*  pidlines.cpp - P&ID signal line decorator for LibreCAD (Kai build)       */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*****************************************************************************/

/*
 * Context: P&ID signal lines (ISA-5.1: pneumatic //, capillary X, hydraulic L,
 *   data link o) cannot be LibreCAD line types -- the line type list is fixed
 *   in the core and has no embedded symbols. This plugin inserts one block per
 *   symbol along the selected line/arc/polyline instead, so the result is plain
 *   DXF that every CAD program shows the same way.
 * Patterns: every pid_*.dxf in <LibreCAD>/resources/pidlines and in
 *   Documents/LibreCAD/pidlines (the latter wins on the same name). The symbol
 *   is drawn around (0,0) with the line running along +X; the block name is the
 *   file name, so "re-place" finds old symbols by the pid_ prefix.
 * Gotchas:
 *   - Needs the patched Doc_plugin_interface::getSelect (Kai build). Stock
 *     2.2.1.x returns false after every selection, so nothing happens.
 *   - A block already in the drawing is reused as is: editing the .dxf later
 *     does not update drawings that already contain that block.
 *   - Old symbols are found by lying on the selected path. If the line was
 *     moved or rotated, the old symbols are off the path and stay; delete them
 *     by hand.
 */

#include <cmath>
#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "document_interface.h"
#include "pidlines.h"

namespace {

const double kPi = 3.14159265358979323846;
const QString kPrefix = QStringLiteral("pid_");

struct Seg {
    bool arc = false;
    QPointF a, b;          // line
    QPointF c;             // arc center
    double r = 0, a0 = 0, sweep = 0;   // arc: start angle, signed sweep (CCW > 0)
    double len = 0;
};

struct Path {
    QList<Seg> segs;
    QString layer;
    double length() const {
        double l = 0;
        for (const Seg &s: segs) l += s.len;
        return l;
    }
};

double norm2pi(double a) {
    a = std::fmod(a, 2 * kPi);
    return a < 0 ? a + 2 * kPi : a;
}

Seg lineSeg(QPointF a, QPointF b) {
    Seg s;
    s.a = a; s.b = b;
    s.len = std::hypot(b.x() - a.x(), b.y() - a.y());
    return s;
}

Seg arcSeg(QPointF c, double r, double a0, double sweep) {
    Seg s;
    s.arc = true;
    s.c = c; s.r = r; s.a0 = a0; s.sweep = sweep;
    s.len = std::fabs(sweep) * r;
    return s;
}

// polyline vertex p1 -> p2 with bulge b (b = tan(sweep/4), > 0 = CCW)
Seg bulgeSeg(QPointF p1, QPointF p2, double b) {
    double dx = p2.x() - p1.x(), dy = p2.y() - p1.y();
    double c = std::hypot(dx, dy);
    if (std::fabs(b) < 1e-9 || c < 1e-12)
        return lineSeg(p1, p2);
    double sweep = 4 * std::atan(b);
    // center sits on the chord's left normal at (c/2)(1-b^2)/(2b), signed by b
    double d = (c / 2) * (1 - b * b) / (2 * b);
    QPointF mid((p1.x() + p2.x()) / 2, (p1.y() + p2.y()) / 2);
    QPointF ctr(mid.x() - dy / c * d, mid.y() + dx / c * d);
    double r = std::hypot(p1.x() - ctr.x(), p1.y() - ctr.y());
    return arcSeg(ctr, r, std::atan2(p1.y() - ctr.y(), p1.x() - ctr.x()), sweep);
}

// point and tangent angle at distance t along one segment
void segAt(const Seg &s, double t, QPointF *p, double *ang) {
    if (!s.arc) {
        double f = s.len > 0 ? t / s.len : 0;
        *p = QPointF(s.a.x() + (s.b.x() - s.a.x()) * f, s.a.y() + (s.b.y() - s.a.y()) * f);
        *ang = std::atan2(s.b.y() - s.a.y(), s.b.x() - s.a.x());
        return;
    }
    double a = s.a0 + (s.len > 0 ? s.sweep * t / s.len : 0);
    *p = QPointF(s.c.x() + s.r * std::cos(a), s.c.y() + s.r * std::sin(a));
    *ang = a + (s.sweep >= 0 ? kPi / 2 : -kPi / 2);
}

void pathAt(const Path &path, double t, QPointF *p, double *ang) {
    for (int i = 0; i < path.segs.size(); ++i) {
        const Seg &s = path.segs.at(i);
        if (t <= s.len || i == path.segs.size() - 1) {
            segAt(s, qMin(t, s.len), p, ang);
            return;
        }
        t -= s.len;
    }
}

double segDist(const Seg &s, QPointF p) {
    if (!s.arc) {
        double dx = s.b.x() - s.a.x(), dy = s.b.y() - s.a.y();
        double l2 = dx * dx + dy * dy;
        double f = l2 > 0 ? ((p.x() - s.a.x()) * dx + (p.y() - s.a.y()) * dy) / l2 : 0;
        f = qBound(0.0, f, 1.0);
        return std::hypot(p.x() - s.a.x() - f * dx, p.y() - s.a.y() - f * dy);
    }
    double ang = std::atan2(p.y() - s.c.y(), p.x() - s.c.x());
    double off = norm2pi(s.sweep >= 0 ? ang - s.a0 : s.a0 - ang);
    if (off <= std::fabs(s.sweep) + 1e-12)
        return std::fabs(std::hypot(p.x() - s.c.x(), p.y() - s.c.y()) - s.r);
    QPointF e1, e2; double dummy;
    segAt(s, 0, &e1, &dummy);
    segAt(s, s.len, &e2, &dummy);
    return qMin(std::hypot(p.x() - e1.x(), p.y() - e1.y()),
                std::hypot(p.x() - e2.x(), p.y() - e2.y()));
}

double pathDist(const Path &path, QPointF p) {
    double d = 1e300;
    for (const Seg &s: path.segs) d = qMin(d, segDist(s, p));
    return d;
}

bool toPath(Plug_Entity *e, Path *path) {
    QHash<int, QVariant> d;
    e->getData(&d);
    path->layer = d.value(DPI::LAYER).toString();
    switch (d.value(DPI::ETYPE).toInt()) {
    case DPI::LINE:
        path->segs << lineSeg(QPointF(d.value(DPI::STARTX).toDouble(), d.value(DPI::STARTY).toDouble()),
                              QPointF(d.value(DPI::ENDX).toDouble(), d.value(DPI::ENDY).toDouble()));
        break;
    case DPI::ARC: {
        double a1 = d.value(DPI::STARTANGLE).toDouble();
        double a2 = d.value(DPI::ENDANGLE).toDouble();
        bool rev = d.value(DPI::REVERSED).toBool();
        double sweep = rev ? -norm2pi(a1 - a2) : norm2pi(a2 - a1);
        if (std::fabs(sweep) < 1e-12) sweep = rev ? -2 * kPi : 2 * kPi;
        path->segs << arcSeg(QPointF(d.value(DPI::STARTX).toDouble(), d.value(DPI::STARTY).toDouble()),
                             d.value(DPI::RADIUS).toDouble(), a1, sweep);
        break;
    }
    case DPI::POLYLINE: {
        QList<Plug_VertexData> v;
        e->getPolylineData(&v);
        for (int i = 0; i + 1 < v.size(); ++i)
            path->segs << bulgeSeg(v.at(i).point, v.at(i + 1).point, v.at(i).bulge);
        if (d.value(DPI::CLOSEPOLY).toInt() && v.size() > 2) {
            Seg s = bulgeSeg(v.last().point, v.first().point, v.last().bulge);
            if (s.len > 1e-9) path->segs << s;
        }
        break;
    }
    default:
        return false;
    }
    // drop zero-length pieces so pathAt never divides by them
    for (int i = path->segs.size() - 1; i >= 0; --i)
        if (path->segs.at(i).len < 1e-9) path->segs.removeAt(i);
    return !path->segs.isEmpty();
}

QList<QFileInfo> findPatterns() {
    QStringList dirs;
    dirs << QCoreApplication::applicationDirPath() + "/resources/pidlines"
         << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/LibreCAD/pidlines";
    QMap<QString, QFileInfo> byName;   // later dir overrides, sorted by name
    for (const QString &dir: dirs) {
        const QFileInfoList files = QDir(dir).entryInfoList(QStringList() << "pid_*.dxf", QDir::Files);
        for (const QFileInfo &f: files)
            byName.insert(f.completeBaseName().toLower(), f);
    }
    return byName.values();
}

QString patternLabel(const QFileInfo &f) {
    return f.completeBaseName().mid(kPrefix.size());
}

} // namespace

QString LC_PidLines::name() const
{
    return tr("P&ID signal line");
}

PluginCapabilities LC_PidLines::getCapabilities() const
{
    PluginCapabilities pluginCapabilities;
    pluginCapabilities.menuEntryPoints
            << PluginMenuLocation("plugins_menu", tr("P&ID signal line"));
    return pluginCapabilities;
}

void LC_PidLines::execComm(Document_Interface *doc, QWidget *parent, QString cmd)
{
    Q_UNUSED(cmd);
    const QList<QFileInfo> patterns = findPatterns();
    PidLinesDlg dlg(patterns, parent);
    if (dlg.exec() != QDialog::Accepted)
        return;
    dlg.saveSettings();
    const int pat = dlg.pattern();
    const double spacing = dlg.spacing();
    const double scale = dlg.scale();

    QList<Plug_Entity *> sel;
    if (!doc->getSelect(&sel, QString::fromUtf8("เลือกเส้น/โค้ง/polyline แล้วกด Enter"))) {
        return;   // Esc or nothing selected
    }
    QList<Path> paths;
    int skipped = 0;
    for (Plug_Entity *e: sel) {
        Path p;
        if (toPath(e, &p)) paths << p; else ++skipped;
    }
    qDeleteAll(sel);
    doc->unselectEntities();
    if (paths.isEmpty()) {
        QMessageBox::information(parent, name(),
            QString::fromUtf8("ในที่เลือกไม่มี line / arc / polyline"));
        return;
    }

    int removed = 0;
    if (dlg.replaceExisting() || pat < 0) {
        QList<Plug_Entity *> all;
        doc->getAllEntities(&all, false);
        for (Plug_Entity *e: all) {
            QHash<int, QVariant> d;
            e->getData(&d);
            if (d.value(DPI::ETYPE).toInt() != DPI::INSERT
                    || !d.value(DPI::BLKNAME).toString().startsWith(kPrefix, Qt::CaseInsensitive))
                continue;
            QPointF ip(d.value(DPI::STARTX).toDouble(), d.value(DPI::STARTY).toDouble());
            for (const Path &p: paths) {
                if (pathDist(p, ip) < 1e-4) {
                    doc->removeEntity(e);
                    ++removed;
                    break;
                }
            }
        }
        qDeleteAll(all);
    }

    int placed = 0;
    if (pat >= 0) {
        const QFileInfo &file = patterns.at(pat);
        QString block = file.completeBaseName();
        if (!doc->getAllBlocks().contains(block)) {
            block = doc->addBlockfromFromdisk(file.absoluteFilePath());
            if (block.isEmpty()) {
                QMessageBox::warning(parent, name(),
                    QString::fromUtf8("เปิดไฟล์สัญลักษณ์ไม่ได้:\n%1").arg(file.absoluteFilePath()));
                return;
            }
        }
        const QString prevLayer = doc->getCurrentLayer();
        for (const Path &p: paths) {
            const double len = p.length();
            // symbols centered on the path: n gaps of `spacing`, equal margins at both ends
            int n = qMax(1, int(std::floor(len / spacing + 1e-9)));
            double first = (len - (n - 1) * spacing) / 2;
            doc->setLayer(p.layer);
            for (int k = 0; k < n; ++k) {
                QPointF pt; double ang;
                pathAt(p, first + k * spacing, &pt, &ang);
                // keep letter-like symbols (L) upright: fold the angle into (-90°, 90°]
                ang = norm2pi(ang);
                if (ang > kPi / 2 + 1e-9 && ang <= 3 * kPi / 2 + 1e-9) ang -= kPi;
                else if (ang > 3 * kPi / 2) ang -= 2 * kPi;
                doc->addInsert(block, pt, QPointF(scale, scale), ang);
                ++placed;
            }
        }
        doc->setLayer(prevLayer);
    }
    doc->updateView();
    if (skipped)
        QMessageBox::information(parent, name(),
            QString::fromUtf8("วางสัญลักษณ์ %1 จุด บน %2 เส้น · ลบของเดิม %3\nข้ามวัตถุที่ไม่ใช่เส้น %4 ชิ้น")
                .arg(placed).arg(paths.size()).arg(removed).arg(skipped));
}

/*****************************/

PidLinesDlg::PidLinesDlg(const QList<QFileInfo> &pats, QWidget *parent)
    : QDialog(parent), patterns(pats)
{
    setWindowTitle(QString::fromUtf8("เส้นสัญญาณ P&ID"));
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "LibreCAD", "pidlines");

    patternBox = new QComboBox;
    for (const QFileInfo &f: patterns)
        patternBox->addItem(patternLabel(f), f.absoluteFilePath());
    patternBox->addItem(QString::fromUtf8("(ลบสัญลักษณ์อย่างเดียว)"));
    int idx = patternBox->findText(s.value("pattern").toString());
    patternBox->setCurrentIndex(idx >= 0 ? idx : 0);

    spacingBox = new QDoubleSpinBox;
    spacingBox->setRange(0.1, 100000);
    spacingBox->setDecimals(2);
    spacingBox->setValue(s.value("spacing", 15.0).toDouble());

    scaleBox = new QDoubleSpinBox;
    scaleBox->setRange(0.01, 1000);
    scaleBox->setDecimals(2);
    scaleBox->setValue(s.value("scale", 1.0).toDouble());

    replaceBox = new QCheckBox(QString::fromUtf8("ลบสัญลักษณ์เดิมบนเส้นที่เลือกก่อน (วางใหม่)"));
    replaceBox->setChecked(s.value("replace", true).toBool());

    QFormLayout *form = new QFormLayout;
    form->addRow(QString::fromUtf8("ลาย:"), patternBox);
    form->addRow(QString::fromUtf8("ระยะห่าง:"), spacingBox);
    form->addRow(QString::fromUtf8("สเกลสัญลักษณ์:"), scaleBox);
    form->addRow(replaceBox);

    QLabel *hint = new QLabel(QString::fromUtf8(
        "ตกลงแล้ว: เลือกเส้นบนแบบ แล้วกด Enter\n"
        "เพิ่มลายเอง: วาดสัญลักษณ์รอบจุด (0,0) ให้เส้นวิ่งตามแกน X\n"
        "แล้วเซฟเป็น pid_<ชื่อ>.dxf ใน Documents\\LibreCAD\\pidlines"));
    hint->setWordWrap(true);

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(patternBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PidLinesDlg::patternChanged);

    QVBoxLayout *main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addWidget(hint);
    main->addWidget(bb);
    patternChanged();
}

void PidLinesDlg::patternChanged()
{
    bool removeOnly = pattern() < 0;
    spacingBox->setEnabled(!removeOnly);
    scaleBox->setEnabled(!removeOnly);
    replaceBox->setEnabled(!removeOnly);
}

int PidLinesDlg::pattern() const
{
    int i = patternBox->currentIndex();
    return i < patterns.size() ? i : -1;
}

double PidLinesDlg::spacing() const { return spacingBox->value(); }
double PidLinesDlg::scale() const { return scaleBox->value(); }
bool PidLinesDlg::replaceExisting() const { return replaceBox->isChecked(); }

void PidLinesDlg::saveSettings() const
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "LibreCAD", "pidlines");
    s.setValue("pattern", patternBox->currentText());
    s.setValue("spacing", spacingBox->value());
    s.setValue("scale", scaleBox->value());
    s.setValue("replace", replaceBox->isChecked());
}

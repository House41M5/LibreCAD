/*****************************************************************************/
/*  pidlines.cpp - P&ID signal line decorator for LibreCAD (Kai build)       */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*****************************************************************************/

/*
 * Context: P&ID signal lines (ISA-5.1: pneumatic //, capillary X, hydraulic L,
 *   data link o, ...) cannot be LibreCAD line types -- the line type list is
 *   fixed in the core and has no embedded symbols. This plugin inserts one block
 *   per symbol along the selected line/arc/polyline and, for patterns with a gap
 *   (o, ◉, text), cuts the line under each symbol, so the result is plain DXF
 *   that every CAD program shows the same way.
 * Patterns: every pid_*.dxf in <LibreCAD>/resources/pidlines and in
 *   Documents/LibreCAD/pidlines (the latter wins on the same name). The symbol
 *   is drawn around (0,0) with the line running along +X. pidlines.ini in the
 *   same folders gives each pattern a label, the gap (mm at scale 1) cut out
 *   under each symbol, a default spacing, line=0 for patterns drawn without
 *   the line, and how a symbol sits on steep runs (mirror_steep) and vertical
 *   runs (upright_vertical, gap_vertical). Text patterns (--CWS--) come from
 *   make_text_patterns.py.
 * Re-place / remove: clicking one piece of a cut line is enough. Pieces are
 *   joined back through the symbols sitting in their gaps (pidgeom.h), so the
 *   original line comes back before the new pattern is applied.
 * Gotchas:
 *   - Needs the patched Doc_plugin_interface::getSelect (Kai build). Stock
 *     2.2.1.x returns false after every selection, so nothing happens.
 *   - A block already in the drawing is reused as is: editing the .dxf later
 *     does not update drawings that already contain that block. (pid_pneumatic
 *     changed from 68° to 45° strokes on 2026-10-20; an older drawing keeps the
 *     68° block and, mirrored on steep runs, leans / but steeper on horizontals.)
 *   - mirror_steep inserts with y scale -scale. RS_Insert::update turns circles
 *     and arcs into ellipses when x scale > y scale, so keep it to line symbols.
 *   - A line edited by hand after cutting (a piece moved or trimmed) no longer
 *     joins up; its leftover symbols have to be deleted by hand.
 *   - Plugin_Entity::updateData on a polyline resets "closed" unless CLOSEPOLY
 *     is passed, so applyPen always passes it.
 *   - Pieces keep the source's entity type (a polyline stays polylines, even a
 *     one-segment piece): LibreCAD draws a LINE lighter than a polyline segment
 *     with the same pen (measured 2026-10-19: peak 130 vs 192 on screen), so a
 *     LINE/polyline mix showed darker corners on one line.
 */

#include <cmath>
#include <vector>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "document_interface.h"
#include "pidlines.h"
#include "pidgeom.h"

using namespace pidgeom;

namespace {

const QString kPrefix = QStringLiteral("pid_");

struct Pattern {
    QString name;           // file name without pid_ and .dxf, lower case
    QString label;
    QString file;           // absolute path of the .dxf
    double gap = 0;         // line cut out under each symbol, mm at scale 1
    double spacing = 0;     // default spacing in the dialog, 0 = none
    bool keepLine = true;   // false: the pattern is drawn without the line
    bool mirrorSteep = false;   // mirror on lines steeper than 45° (// never turns into \\)
    bool uprightVertical = false;   // on vertical runs: not turned, looks as on a horizontal run
    double gapVertical = -1;    // gap on vertical runs, mm at scale 1; -1 = same as gap
};

struct Pen {
    int color = -1;
    QString ltype = QStringLiteral("BYLAYER");
    QString lwidth = QStringLiteral("BYLAYER");
    bool byLayer() const {
        return color == -1 && ltype == QLatin1String("BYLAYER")
                && (lwidth == QLatin1String("BYLAYER") || lwidth == QLatin1String("BYDEFAULT"));
    }
};

struct Ent {                // a line, arc or polyline of the drawing
    Plug_Entity *e = nullptr;
    qulonglong id = 0;
    int type = 0;           // DPI::LINE / ARC / POLYLINE
    Path path;
    QString layer;
    Pen pen;
};

struct Sym {                // a pid_ symbol insert
    Plug_Entity *e = nullptr;
    QPointF p;
    double scale = 1;
};

QStringList patternDirs() {
    return QStringList()
            << QCoreApplication::applicationDirPath() + "/resources/pidlines"
            << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/LibreCAD/pidlines";
}

QList<Pattern> loadPatterns() {
    QMap<QString, Pattern> byName;      // sorted by name; a later folder overrides
    const QStringList dirs = patternDirs();
    for (const QString &dir: dirs) {
        const QFileInfoList files = QDir(dir).entryInfoList(QStringList() << "pid_*.dxf", QDir::Files);
        for (const QFileInfo &f: files) {
            Pattern &p = byName[f.completeBaseName().mid(kPrefix.size()).toLower()];
            p.name = f.completeBaseName().mid(kPrefix.size()).toLower();
            p.file = f.absoluteFilePath();
            if (p.label.isEmpty()) p.label = p.name;
        }
    }
    for (const QString &dir: dirs) {
        const QString ini = dir + "/pidlines.ini";
        if (!QFileInfo::exists(ini)) continue;
        QSettings s(ini, QSettings::IniFormat);
        s.setIniCodec("UTF-8");
        for (Pattern &p: byName) {
            s.beginGroup(p.name);
            if (s.contains("label")) p.label = s.value("label").toString();
            if (s.contains("gap")) p.gap = std::max(0.0, s.value("gap").toDouble());
            if (s.contains("spacing")) p.spacing = std::max(0.0, s.value("spacing").toDouble());
            if (s.contains("line")) p.keepLine = s.value("line").toInt() != 0;
            if (s.contains("mirror_steep")) p.mirrorSteep = s.value("mirror_steep").toInt() != 0;
            if (s.contains("upright_vertical")) p.uprightVertical = s.value("upright_vertical").toInt() != 0;
            if (s.contains("gap_vertical")) p.gapVertical = std::max(0.0, s.value("gap_vertical").toDouble());
            s.endGroup();
        }
    }
    return byName.values();
}

bool toPath(Plug_Entity *e, const QHash<int, QVariant> &d, Path *path) {
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
        path->closed = d.value(DPI::CLOSEPOLY).toBool();
        if (path->closed && v.size() > 2) {
            Seg s = bulgeSeg(v.last().point, v.first().point, v.last().bulge);
            if (s.len > 1e-9) path->segs << s;
        }
        break;
    }
    default:
        return false;
    }
    path->segs = simplify(path->segs);
    return !path->segs.isEmpty();
}

double rad2deg(double a) { return a * 180.0 / kPi; }

// Adds `segs` as one entity on the current layer: a POLYLINE when asked for
// (keeps the source type) or when it takes more than one segment, else LINE/ARC.
// Returns true for a polyline (applyPen needs to know).
bool addSegs(Document_Interface *doc, QList<Seg> segs, bool closed, bool polyline) {
    if (closed && segs.size() == 1) {
        // a loop merged into one 360° arc: a polyline needs two vertices (bulge of 360° is infinite)
        const Seg s = segs.takeFirst();
        segs << subSeg(s, 0, s.len / 2) << subSeg(s, s.len / 2, s.len);
    }
    if (segs.size() == 1 && !closed && !polyline) {
        const Seg &s = segs.first();
        if (!s.arc) {
            QPointF a = s.a, b = s.b;
            doc->addLine(&a, &b);
        } else {
            // addArc is counter-clockwise only
            double a1 = s.sweep > 0 ? s.a0 : s.a0 + s.sweep;
            QPointF c = s.c;
            doc->addArc(&c, s.r, rad2deg(a1), rad2deg(a1 + std::fabs(s.sweep)));
        }
        return false;
    }
    std::vector<Plug_VertexData> v;
    for (const Seg &s: segs) v.emplace_back(segStart(s), bulgeOf(s));
    if (!closed) v.emplace_back(segEnd(segs.last()), 0.0);
    doc->addPolyline(v, closed);
    return true;
}

// The last entities added get the source line's own colour/line type/width.
// polyClosed holds, in adding order, -1 for non-polylines and 0/1 for polylines.
void applyPen(Document_Interface *doc, const Pen &pen, const QList<int> &polyClosed) {
    if (pen.byLayer() || polyClosed.isEmpty()) return;
    QList<Plug_Entity *> all;
    doc->getAllEntities(&all, false);
    const int first = all.size() - polyClosed.size();
    for (int i = std::max(0, first); i < all.size(); ++i) {
        QHash<int, QVariant> d;
        d.insert(DPI::COLOR, pen.color);
        d.insert(DPI::LTYPE, pen.ltype);
        d.insert(DPI::LWIDTH, pen.lwidth);
        int pc = polyClosed.at(i - first);
        if (pc >= 0) d.insert(DPI::CLOSEPOLY, pc == 1);
        all.at(i)->updateData(&d);
    }
    qDeleteAll(all);
}

// within 1° of vertical, either direction
bool isVertical(double ang) {
    return std::fabs(std::cos(ang)) < std::sin(kPi / 180);
}

// keep letter-like symbols (L) upright: fold the angle into (-90°, 90°]
double upright(double ang) {
    ang = norm2pi(ang);
    if (ang > kPi / 2 + 1e-9 && ang <= 3 * kPi / 2 + 1e-9) return ang - kPi;
    if (ang > 3 * kPi / 2) return ang - 2 * kPi;
    return ang;
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
    const QList<Pattern> patterns = loadPatterns();
    QStringList names, labels;
    QList<double> spacings;
    for (const Pattern &p: patterns) { names << p.name; labels << p.label; spacings << p.spacing; }
    PidLinesDlg dlg(names, labels, spacings, parent);
    if (dlg.exec() != QDialog::Accepted)
        return;
    dlg.saveSettings();
    const int pat = dlg.pattern();
    const double spacing = dlg.spacing();
    const double scale = dlg.scale();

    QList<Plug_Entity *> sel;
    if (!doc->getSelect(&sel, QString::fromUtf8("เลือกเส้น/โค้ง/polyline แล้วกด Enter")))
        return;     // Esc or nothing selected
    QSet<qulonglong> selected;
    int skipped = 0;
    for (Plug_Entity *e: sel) {
        QHash<int, QVariant> d;
        e->getData(&d);
        int t = d.value(DPI::ETYPE).toInt();
        if (t == DPI::LINE || t == DPI::ARC || t == DPI::POLYLINE)
            selected << d.value(DPI::EID).toULongLong();
        else if (!(t == DPI::INSERT && d.value(DPI::BLKNAME).toString().startsWith(kPrefix, Qt::CaseInsensitive)))
            ++skipped;      // symbols caught in a window selection are expected, not reported
    }
    qDeleteAll(sel);
    doc->unselectEntities();
    if (selected.isEmpty()) {
        QMessageBox::information(parent, name(), QString::fromUtf8("ในที่เลือกไม่มี line / arc / polyline"));
        return;
    }

    // the whole drawing: pieces of a cut line may lie outside the selection
    QList<Plug_Entity *> all;
    doc->getAllEntities(&all, false);
    QList<Ent> ents;
    QList<Sym> syms;
    for (Plug_Entity *e: all) {
        QHash<int, QVariant> d;
        e->getData(&d);
        int t = d.value(DPI::ETYPE).toInt();
        if (t == DPI::INSERT) {
            if (!d.value(DPI::BLKNAME).toString().startsWith(kPrefix, Qt::CaseInsensitive)) continue;
            Sym s;
            s.e = e;
            s.p = QPointF(d.value(DPI::STARTX).toDouble(), d.value(DPI::STARTY).toDouble());
            s.scale = std::fabs(d.value(DPI::XSCALE, 1.0).toDouble());
            syms << s;
            continue;
        }
        Ent en;
        if (!toPath(e, d, &en.path)) continue;
        en.e = e;
        en.id = d.value(DPI::EID).toULongLong();
        en.type = d.value(DPI::ETYPE).toInt();
        en.layer = d.value(DPI::LAYER).toString();
        en.pen.color = d.value(DPI::COLOR).toInt();
        en.pen.ltype = d.value(DPI::LTYPE).toString();
        en.pen.lwidth = d.value(DPI::LWIDTH).toString();
        ents << en;
    }

    QList<Path> paths;
    for (const Ent &en: ents) paths << en.path;
    QList<QPointF> symPts;
    QList<double> reach;
    for (const Sym &s: syms) { symPts << s.p; reach << 10.0 * std::max(1.0, s.scale); }
    QVector<Links> links;
    const QVector<bool> linked = linkPaths(paths, symPts, reach, &links);

    QString block;
    if (pat >= 0) {
        const QFileInfo file(patterns.at(pat).file);
        block = file.completeBaseName();
        if (!doc->getAllBlocks().contains(block)) {
            block = doc->addBlockfromFromdisk(file.absoluteFilePath());
            if (block.isEmpty()) {
                qDeleteAll(all);
                QMessageBox::warning(parent, name(),
                    QString::fromUtf8("เปิดไฟล์สัญลักษณ์ไม่ได้:\n%1").arg(file.absoluteFilePath()));
                return;
            }
        }
    }

    const QString prevLayer = doc->getCurrentLayer();
    QSet<int> done, removedSyms;
    int lines = 0, placed = 0, removed = 0, uncut = 0;
    for (int i = 0; i < ents.size(); ++i) {
        if (!selected.contains(ents.at(i).id) || done.contains(i)) continue;
        const Chain ch = walkChain(paths, links, i);
        for (int m: ch.members) done << m;
        const Ent &src = ents.at(ch.members.first());
        ++lines;

        // old symbols: the ones in the gaps and the ones sitting on the line
        QSet<int> old;
        for (int s: ch.gapSymbols) old << s;
        for (int s = 0; s < syms.size(); ++s) {
            if (linked[s] || old.contains(s)) continue;
            for (int m: ch.members)
                if (pathDist(paths.at(m), syms.at(s).p) < kTol) { old << s; break; }
        }
        for (int s: old) {
            if (removedSyms.contains(s)) continue;
            doc->removeEntity(syms.at(s).e);
            removedSyms << s;
            ++removed;
        }

        const bool hadGaps = !ch.gapSymbols.isEmpty();
        // a polyline anywhere in the chain means the line started as a polyline
        bool asPoly = false;
        for (int m: ch.members) asPoly = asPoly || ents.at(m).type == DPI::POLYLINE;
        auto removeLine = [&]() { for (int m: ch.members) doc->removeEntity(ents.at(m).e); };
        QList<int> added;   // applyPen bookkeeping, in adding order
        doc->setLayer(src.layer);

        if (pat < 0) {      // remove symbols only: a cut line is joined back
            if (hadGaps) {
                removeLine();
                added << (addSegs(doc, ch.merged.segs, ch.merged.closed, asPoly) ? int(ch.merged.closed) : -1);
            }
            applyPen(doc, src.pen, added);
            continue;
        }

        const Pattern &pt = patterns.at(pat);
        const bool cut = pt.keepLine;   // whether and how much: the pattern's gap
        const Path &path = ch.merged;
        auto vertical = [&](double t) {
            QPointF p; double ang;
            pathAt(path, t, &p, &ang);
            return isVertical(ang);
        };
        auto gapAt = [&](double t) {
            if (!cut) return 0.0;
            return (pt.gapVertical >= 0 && vertical(t) ? pt.gapVertical : pt.gap) * scale;
        };
        const QList<Station> st = planStations(path, spacing, gapAt);
        bool anyCut = false;
        for (const Station &s: st) {
            anyCut = anyCut || s.cut;
            if (gapAt(s.t) > 0 && !s.cut) ++uncut;
        }
        if (!pt.keepLine) {
            removeLine();
        } else if (anyCut) {
            removeLine();
            for (const QList<Seg> &piece: pieces(ch.merged, st))
                added << (addSegs(doc, piece, false, asPoly) ? 0 : -1);
        } else if (hadGaps) {
            removeLine();
            added << (addSegs(doc, ch.merged.segs, ch.merged.closed, asPoly) ? int(ch.merged.closed) : -1);
        }   // else the line stays as it is
        for (const Station &s: st) {
            QPointF p; double ang;
            pathAt(ch.merged, s.t, &p, &ang);
            if (pt.uprightVertical && isVertical(ang)) {
                // drawn as on a horizontal line, not turned: the L's foot crosses the line
                doc->addInsert(block, p, QPointF(scale, scale), 0);
                added << -1;
                ++placed;
                continue;
            }
            ang = upright(ang);
            // a 45° stroke on a vertical line leans \ once rotated; mirrored it leans / again
            const bool mirror = pt.mirrorSteep && std::fabs(ang) > kPi / 4 + 1e-9;
            doc->addInsert(block, p, QPointF(scale, mirror ? -scale : scale), ang);
            added << -1;
            ++placed;
        }
        applyPen(doc, src.pen, added);
    }
    doc->setLayer(prevLayer);
    qDeleteAll(all);
    doc->updateView();

    QStringList notes;
    if (skipped) notes << QString::fromUtf8("ข้ามวัตถุที่ไม่ใช่เส้น %1 ชิ้น").arg(skipped);
    if (uncut) notes << QString::fromUtf8("สัญลักษณ์ %1 จุดวางบนเส้นโดยไม่ตัด (ใกล้มุม/ปลายเส้น หรือระยะห่างแคบกว่าช่อง)").arg(uncut);
    if (!notes.isEmpty())
        QMessageBox::information(parent, name(),
            QString::fromUtf8("%1 เส้น · วางสัญลักษณ์ %2 · ลบของเดิม %3\n").arg(lines).arg(placed).arg(removed)
            + notes.join("\n"));
}

/*****************************/

PidLinesDlg::PidLinesDlg(const QStringList &names, const QStringList &labels,
                         const QList<double> &spacings, QWidget *parent)
    : QDialog(parent), count(names.size()), names(names), spacings(spacings)
{
    setWindowTitle(QString::fromUtf8("เส้นสัญญาณ P&ID"));
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "LibreCAD", "pidlines");

    patternBox = new QComboBox;
    for (int i = 0; i < names.size(); ++i)
        patternBox->addItem(labels.at(i), names.at(i));
    patternBox->addItem(QString::fromUtf8("(ลบสัญลักษณ์อย่างเดียว · ต่อเส้นกลับ)"));
    int idx = patternBox->findData(s.value("pattern").toString());
    patternBox->setCurrentIndex(idx >= 0 ? idx : 0);

    spacingBox = new QDoubleSpinBox;
    spacingBox->setRange(0.1, 100000);
    spacingBox->setDecimals(2);
    spacingBox->setValue(s.value("spacing", 15.0).toDouble());

    scaleBox = new QDoubleSpinBox;
    scaleBox->setRange(0.01, 1000);
    scaleBox->setDecimals(2);
    scaleBox->setValue(s.value("scale", 1.0).toDouble());

    // no "cut the line" checkbox: since 2026-10-20 each pattern's gap in
    // pidlines.ini decides (0 = drawn over the line); a global switch only
    // broke the patterns that need the cut (o, text)
    QFormLayout *form = new QFormLayout;
    form->addRow(QString::fromUtf8("ลาย:"), patternBox);
    form->addRow(QString::fromUtf8("ระยะห่าง:"), spacingBox);
    form->addRow(QString::fromUtf8("สเกลสัญลักษณ์:"), scaleBox);

    QLabel *hint = new QLabel(QString::fromUtf8(
        "ตกลงแล้ว: เลือกเส้นบนแบบ แล้วกด Enter (เส้นที่ตัดแล้ว คลิกท่อนเดียวพอ)\n"
        "สัญลักษณ์เดิมบนเส้นที่เลือกจะถูกแทนที่\n"
        "เพิ่มลายเอง: วาดสัญลักษณ์รอบจุด (0,0) ให้เส้นวิ่งตามแกน X\n"
        "เซฟเป็น pid_<ชื่อ>.dxf ใน Documents\\LibreCAD\\pidlines\n"
        "ตัดเส้นหรือไม่ (gap) และชื่อที่แสดง: pidlines.ini ในโฟลเดอร์เดียวกัน"));
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

// spacing follows the pattern: last one used for it, else its pidlines.ini
// default, else the last spacing used at all
void PidLinesDlg::patternChanged()
{
    const int i = pattern();
    const bool removeOnly = i < 0;
    spacingBox->setEnabled(!removeOnly);
    scaleBox->setEnabled(!removeOnly);
    if (removeOnly) return;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "LibreCAD", "pidlines");
    double sp = s.value("patternSpacing/" + names.at(i), 0.0).toDouble();
    if (sp <= 0) sp = spacings.at(i);
    if (sp <= 0) sp = s.value("spacing", 15.0).toDouble();
    spacingBox->setValue(sp);
}

int PidLinesDlg::pattern() const
{
    int i = patternBox->currentIndex();
    return i < count ? i : -1;
}

double PidLinesDlg::spacing() const { return spacingBox->value(); }
double PidLinesDlg::scale() const { return scaleBox->value(); }

void PidLinesDlg::saveSettings() const
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "LibreCAD", "pidlines");
    s.setValue("pattern", patternBox->currentData().toString());
    s.setValue("spacing", spacingBox->value());
    if (pattern() >= 0)
        s.setValue("patternSpacing/" + names.at(pattern()), spacingBox->value());
    s.setValue("scale", scaleBox->value());
    s.remove("cut");    // left by builds that had the checkbox
}

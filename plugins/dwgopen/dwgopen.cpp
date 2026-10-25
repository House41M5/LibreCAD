/*****************************************************************************/
/*  dwgopen.cpp - "Open DWG..." / "Save as DWG..." for LibreCAD 2.2.1.6       */
/*                (converts through LibreDWG)                                */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/
// Context: user 2026-10-25 "ทำปุ่มเปิด DWG ใน LibreCAD ให้เลย" then "ทำปุ่มบันทึกเป็น DWG ด้วย" [Claude-Win@acer].
//          LibreCAD 2.2.1.6 marks AutoCAD 2018 DWG (AC1032) unsupported (libdwgr.cpp:206-208), 35 of the user's
//          36 DWG are AC1032, and it cannot write DWG at all.
// What:    File menu + File toolbar:
//          "เปิด DWG…" (after Open) runs `ruby -S rake -f ~/dxf-tools/Rakefile dwg` with DWG_FILE=<file>
//          (LibreDWG dwg2dxf + dwg2lc.py) and opens the DXF from the task's "OUT: <path>" line in a new tab.
//          The DWG itself is never written; the DXF goes to Documents/LibreCAD/from-dwg/.
//          "บันทึกเป็น DWG…" (after Save as) converts the active drawing's DXF file with `rake todwg`
//          (DXF_FILE=, DWG_TARGET=; lc2dwg.py + LibreDWG dxf2dwg) into an AutoCAD 2000 DWG.
// Why a plugin: a dll in Documents/LibreCAD/plugins needs no admin and no new LibreCAD.exe.
//          LibreCAD only offers plugin entries that run on the open drawing (execPlug dereferences the current
//          window, and the actions are disabled with no drawing open) - so the actions are added to the File menu
//          directly and call QC_ApplicationWindow's public slot slotFileOpen(QString) / the "FileSave" action.
//          A plugin cannot ask LibreCAD for the drawing's file name (Document_Interface has no such call): the
//          active tab shows only the file name, so the full path comes from the Recent Files menu entries
//          (LibreCAD adds a file there on open and on save) - several or none matching -> the user is asked.
// Recover: delete Documents/LibreCAD/plugins/dwgopen0.dll and restart LibreCAD.
// Gotchas: LibreCAD marks a drawing modified right after opening it, so "unsaved changes" is asked, not assumed.
//          Nothing is overwritten silently: rake dwg / rake todwg keep the old DXF / DWG as .bak-<time>.
//          Test hooks (environment, read once at start-up): LC_DWGOPEN_FILE=<dwg> opens that DWG;
//          LC_DWGSAVE_TARGET=<dwg> saves the drawing opened on the command line as that DWG without any dialog.
//          QProcess from a GUI process starts children with CREATE_NO_WINDOW, so no console flashes.
#include "dwgopen.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>

namespace {

const int kTimeoutMs = 600 * 1000;  // rake's guard.py stops LibreDWG at 300 s; the python steps take seconds

QString tr8(const char *s) { return QString::fromUtf8(s); }

// LibreCAD's own icon with a red "DWG" band.
QIcon dwgIcon(const QIcon &base)
{
    QPixmap pm = base.pixmap(32, 32);
    if (pm.isNull()) {
        pm = QPixmap(32, 32);
        pm.fill(Qt::transparent);
    }
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QRect band(0, 19, 32, 13);
    p.fillRect(band, QColor(190, 25, 25));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(band, Qt::AlignCenter, QStringLiteral("DWG"));
    return QIcon(pm);
}

// Put act right after `after` in w (menu or toolbar); at the end when `after` is not there.
void insertAfter(QWidget *w, QAction *after, QAction *act)
{
    const QList<QAction *> list = w->actions();
    int i = after ? list.indexOf(after) : -1;
    if (i >= 0 && i + 1 < list.size())
        w->insertAction(list.at(i + 1), act);
    else
        w->addAction(act);
}

QString findRuby()
{
    QString r = QStandardPaths::findExecutable(QStringLiteral("ruby"));
    if (r.isEmpty()) {
        const QString scoop = QDir::homePath() + QStringLiteral("/scoop/apps/ruby/current/bin/ruby.exe");
        if (QFileInfo::exists(scoop))
            r = scoop;
    }
    return r;
}

QString lastLines(const QString &text, int n)
{
    QStringList l = text.split('\n', Qt::SkipEmptyParts);
    while (l.size() > n)
        l.removeFirst();
    return l.join('\n');
}

// File name shown on the active tab ("a.dxf[*]", maybe " [Draft Mode]" after it).
QString tabFileName(QMdiSubWindow *sub)
{
    return sub->windowTitle().section(QStringLiteral("[*]"), 0, 0).trimmed();
}

} // namespace

QString LC_DwgOpen::name() const
{
    return tr8("เปิด DWG");
}

PluginCapabilities LC_DwgOpen::getCapabilities() const
{
    // No entry in the Plugins menu (see the header). Called once from QC_ApplicationWindow::loadPlugins after the
    // menus and toolbars exist; add our actions once the event loop runs.
    QTimer::singleShot(0, const_cast<LC_DwgOpen *>(this), &LC_DwgOpen::install);
    return PluginCapabilities();
}

void LC_DwgOpen::execComm(Document_Interface *, QWidget *, QString)
{
    chooseAndOpen();
}

QMainWindow *LC_DwgOpen::mainWindow() const
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->inherits("QC_ApplicationWindow"))
            return qobject_cast<QMainWindow *>(w);
    return nullptr;
}

void LC_DwgOpen::install()
{
    if (installed)
        return;
    QMainWindow *win = mainWindow();
    if (!win)
        return;
    installed = true;
    QMenu *file = win->menuBar()->findChild<QMenu *>(QStringLiteral("File"));
    QToolBar *tb = win->findChild<QToolBar *>(QStringLiteral("file_toolbar"));
    const QIcon fallback = win->style()->standardIcon(QStyle::SP_DialogOpenButton);

    QAction *open = win->findChild<QAction *>(QStringLiteral("FileOpen"));
    auto *actOpen = new QAction(dwgIcon(open ? open->icon() : fallback), tr8("เปิด DWG…"), this);
    actOpen->setObjectName(QStringLiteral("FileOpenDwg"));
    QString tip = tr8("เปิด DWG: แปลงเป็น DXF ด้วย LibreDWG ก่อน (ไฟล์ DWG ไม่ถูกแก้ · DXF อยู่ใน Documents\\LibreCAD\\from-dwg)");
    actOpen->setToolTip(tip);
    actOpen->setStatusTip(tip);
    connect(actOpen, &QAction::triggered, this, &LC_DwgOpen::chooseAndOpen);

    QAction *save = win->findChild<QAction *>(QStringLiteral("FileSave"));
    QAction *saveAs = win->findChild<QAction *>(QStringLiteral("FileSaveAs"));
    auto *actSave = new QAction(dwgIcon(save ? save->icon() : fallback), tr8("บันทึกเป็น DWG…"), this);
    actSave->setObjectName(QStringLiteral("FileSaveDwg"));
    tip = tr8("บันทึกแบบที่เปิดอยู่เป็น DWG (AutoCAD 2000) จากไฟล์ DXF ของแบบนี้ (ไฟล์ DXF ไม่ถูกแก้ · DWG เดิมเก็บเป็น .bak)");
    actSave->setToolTip(tip);
    actSave->setStatusTip(tip);
    connect(actSave, &QAction::triggered, this, [this] { saveAsDwg(QString()); });

    if (file) {
        insertAfter(file, open, actOpen);
        insertAfter(file, saveAs ? saveAs : save, actSave);
    }
    if (tb) {
        insertAfter(tb, open, actOpen);
        insertAfter(tb, tb->actions().contains(saveAs) ? saveAs : save, actSave);
    }

    const QString autoOpen = qEnvironmentVariable("LC_DWGOPEN_FILE");
    if (!autoOpen.isEmpty())
        QTimer::singleShot(500, this, [this, autoOpen] { openDwg(autoOpen); });
    const QString autoSave = qEnvironmentVariable("LC_DWGSAVE_TARGET");
    if (!autoSave.isEmpty())
        QTimer::singleShot(3000, this, [this, autoSave] { saveAsDwg(autoSave); });
}

bool LC_DwgOpen::runRake(const QString &task, const QStringList &env, const QString &busy, QString *out)
{
    QMainWindow *win = mainWindow();
    const QString ruby = findRuby();
    const QString tools = QDir::homePath() + QStringLiteral("/dxf-tools");
    if (ruby.isEmpty() || !QFileInfo::exists(tools + QStringLiteral("/Rakefile"))) {
        QMessageBox::warning(win, name(),
                             tr8("ต้องมี Ruby (scoop install ruby) และ %1\\Rakefile").arg(QDir::toNativeSeparators(tools)));
        return false;
    }

    QProcess proc;
    QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : env)
        pe.insert(kv.section('=', 0, 0), kv.section('=', 1));
    pe.remove(QStringLiteral("LC_DWGOPEN_FILE"));
    pe.remove(QStringLiteral("LC_DWGSAVE_TARGET"));
    proc.setProcessEnvironment(pe);
    proc.setWorkingDirectory(tools);
    proc.setProcessChannelMode(QProcess::MergedChannels);

    QProgressDialog dlg(busy, QString(), 0, 0, win);
    dlg.setWindowTitle(name());
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setCancelButton(nullptr);
    dlg.show();

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);
    connect(&proc, &QProcess::errorOccurred, &loop, [&loop](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            loop.quit();
    });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    proc.start(ruby, {QStringLiteral("-S"), QStringLiteral("rake"), QStringLiteral("-f"),
                      tools + QStringLiteral("/Rakefile"), task});
    timeout.start(kTimeoutMs);
    if (proc.state() != QProcess::NotRunning || proc.error() != QProcess::FailedToStart)
        loop.exec();
    const bool timedOut = proc.state() != QProcess::NotRunning;
    if (timedOut) {
        proc.kill();
        proc.waitForFinished(5000);
    }
    QApplication::restoreOverrideCursor();
    dlg.close();

    const QString log = QString::fromUtf8(proc.readAll()).replace(QStringLiteral("\r"), QString());
    out->clear();
    for (const QString &line : log.split('\n'))
        if (line.startsWith(QStringLiteral("OUT: ")))
            *out = line.mid(5).trimmed();

    if (timedOut || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || out->isEmpty()
        || !QFileInfo::exists(*out)) {
        const QString why = timedOut ? tr8("ทำงานนานเกิน %1 วินาที — หยุดแล้ว").arg(kTimeoutMs / 1000)
                          : proc.error() == QProcess::FailedToStart ? tr8("เรียก %1 ไม่ได้").arg(ruby)
                                                                     : tr8("แปลงไม่สำเร็จ (rc %1)").arg(proc.exitCode());
        QMessageBox::warning(win, name(), why + QStringLiteral("\n\n") + lastLines(log, 15));
        return false;
    }
    return true;
}

void LC_DwgOpen::chooseAndOpen()
{
    QSettings settings(QStringLiteral("LibreCAD"), QStringLiteral("dwgopen"));
    QString dir = settings.value(QStringLiteral("lastDir")).toString();
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString dwg = QFileDialog::getOpenFileName(mainWindow(), tr8("เปิด DWG"), dir, tr8("DWG (*.dwg)"));
    if (dwg.isEmpty())
        return;
    settings.setValue(QStringLiteral("lastDir"), QFileInfo(dwg).absolutePath());
    openDwg(dwg);
}

void LC_DwgOpen::openDwg(const QString &dwg)
{
    QMainWindow *win = mainWindow();
    const QFileInfo src(dwg);
    if (!src.isFile()) {
        QMessageBox::warning(win, name(), tr8("ไม่พบไฟล์\n%1").arg(QDir::toNativeSeparators(dwg)));
        return;
    }

    // Same place as DWG_OUT in ~/dxf-tools/Rakefile; only used to ask first - the DXF opened is the task's OUT line.
    const QString expected = QDir::homePath() + QStringLiteral("/Documents/LibreCAD/from-dwg/")
                             + src.completeBaseName() + QStringLiteral(".dxf");
    const QFileInfo old(expected);
    if (old.exists()) {
        QMessageBox box(QMessageBox::Question, name(),
                        tr8("%1 เคยแปลงไว้แล้ว (%2)\n\nแปลงใหม่ ⇒ DXF เดิมถูกเก็บเป็น .bak-<เวลา> ข้างไฟล์ ไม่หาย")
                            .arg(old.fileName(), old.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                        QMessageBox::NoButton, win);
        QPushButton *useOld = box.addButton(tr8("เปิดไฟล์ที่แปลงไว้"), QMessageBox::AcceptRole);
        QPushButton *redo = box.addButton(tr8("แปลงใหม่"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(useOld);
        box.exec();
        if (box.clickedButton() == useOld) {
            QMetaObject::invokeMethod(win, "slotFileOpen", Qt::QueuedConnection, Q_ARG(QString, expected));
            return;
        }
        if (box.clickedButton() != redo)
            return;
    }

    QString out;
    if (!runRake(QStringLiteral("dwg"), {QStringLiteral("DWG_FILE=") + src.absoluteFilePath()},
                 tr8("กำลังแปลง %1 …\n(ไฟล์ใหญ่ใช้ราว 15 วินาที)").arg(src.fileName()), &out))
        return;
    if (win && win->statusBar())
        win->statusBar()->showMessage(tr8("แปลงจาก DWG แล้ว: %1").arg(QDir::toNativeSeparators(out)), 10000);
    QMetaObject::invokeMethod(win, "slotFileOpen", Qt::QueuedConnection, Q_ARG(QString, out));
}

QString LC_DwgOpen::activeDxfPath(QMdiSubWindow *sub)
{
    QMainWindow *win = mainWindow();
    const QString shown = tabFileName(sub);
    QStringList found;
    auto consider = [&](const QString &path) {
        const QFileInfo fi(path);
        if (fi.fileName().compare(shown, Qt::CaseInsensitive) == 0 && fi.isFile()
            && !found.contains(fi.absoluteFilePath(), Qt::CaseInsensitive))
            found << fi.absoluteFilePath();
    };
    // the tab's tooltip holds the full path of a file opened in this tab
    if (QMdiArea *mdi = sub->mdiArea())
        for (QTabBar *bar : mdi->findChildren<QTabBar *>())
            if (bar->currentIndex() >= 0)
                consider(bar->tabToolTip(bar->currentIndex()));
    // Recent Files: LibreCAD's file menu has a sub menu whose actions carry the full path as data, newest first
    if (QMenu *file = win->menuBar()->findChild<QMenu *>(QStringLiteral("File")))
        for (QMenu *m : file->findChildren<QMenu *>())
            for (QAction *a : m->actions())
                if (a->isVisible() && a->data().type() == QVariant::String)
                    consider(a->data().toString());

    if (found.size() == 1)
        return found.first();
    if (found.size() > 1) {
        bool ok = false;
        const QString pick = QInputDialog::getItem(win, name(), tr8("มีไฟล์ชื่อ %1 หลายที่ — แบบที่เปิดอยู่คือไฟล์ไหน").arg(shown),
                                                   found, 0, false, &ok);
        return ok ? pick : QString();
    }
    const QString pick = QFileDialog::getOpenFileName(win, tr8("แบบที่เปิดอยู่ (%1) คือไฟล์ไหน").arg(shown),
                                                      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                      tr8("DXF (*.dxf)"));
    return pick;
}

void LC_DwgOpen::saveAsDwg(const QString &presetTarget)
{
    QMainWindow *win = mainWindow();
    const bool test = !presetTarget.isEmpty();
    QMdiArea *mdi = win ? win->findChild<QMdiArea *>() : nullptr;
    QMdiSubWindow *sub = mdi ? mdi->activeSubWindow() : nullptr;
    if (!sub) {
        QMessageBox::information(win, name(), tr8("ยังไม่มีแบบเปิดอยู่"));
        return;
    }

    // 1. the DWG is made from the DXF on disk: unsaved changes or a drawing never saved -> save it first
    const bool unnamed = !tabFileName(sub).endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive);
    if (!test && (unnamed || sub->isWindowModified())) {
        QAction *saveAct = win->findChild<QAction *>(unnamed ? QStringLiteral("FileSaveAs") : QStringLiteral("FileSave"));
        if (!unnamed) {
            QMessageBox box(QMessageBox::Question, name(),
                            tr8("แบบนี้มีการแก้ที่ยังไม่ได้บันทึก (LibreCAD ขึ้น * ทันทีที่เปิดไฟล์ แม้ยังไม่ได้แก้)\n\n"
                                "DWG จะทำจากไฟล์ DXF ที่บันทึกไว้"),
                            QMessageBox::NoButton, win);
            QPushButton *saveFirst = box.addButton(tr8("บันทึก DXF ก่อนแล้วแปลง"), QMessageBox::AcceptRole);
            QPushButton *asSaved = box.addButton(tr8("แปลงจากไฟล์ที่บันทึกไว้"), QMessageBox::ActionRole);
            box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(saveFirst);
            box.exec();
            if (box.clickedButton() == saveFirst) {
                if (saveAct)
                    saveAct->trigger();
            } else if (box.clickedButton() != asSaved) {
                return;
            }
        } else if (saveAct) {
            QMessageBox::information(win, name(), tr8("แบบนี้ยังไม่เคยบันทึก — บันทึกเป็นไฟล์ .dxf ก่อน แล้วจะแปลงเป็น DWG ต่อ"));
            saveAct->trigger();
        }
        if (!tabFileName(sub).endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive)) {
            QMessageBox::information(win, name(), tr8("ยังไม่ได้บันทึกเป็นไฟล์ .dxf — ยกเลิกการบันทึกเป็น DWG"));
            return;
        }
    }

    // 2. which file on disk
    const QString dxf = activeDxfPath(sub);
    if (dxf.isEmpty())
        return;

    // 3. where to write
    QString target = presetTarget;
    if (!test) {
        QSettings settings(QStringLiteral("LibreCAD"), QStringLiteral("dwgopen"));
        QString dir = settings.value(QStringLiteral("lastSaveDir")).toString();
        if (dir.isEmpty() || !QFileInfo(dir).isDir())
            dir = QFileInfo(dxf).absolutePath();
        target = QFileDialog::getSaveFileName(win, tr8("บันทึกเป็น DWG (AutoCAD 2000)"),
                                              dir + QStringLiteral("/") + QFileInfo(dxf).completeBaseName()
                                                  + QStringLiteral(".dwg"),
                                              tr8("DWG (*.dwg)"));
        if (target.isEmpty())
            return;
        if (!target.endsWith(QStringLiteral(".dwg"), Qt::CaseInsensitive))
            target += QStringLiteral(".dwg");
        settings.setValue(QStringLiteral("lastSaveDir"), QFileInfo(target).absolutePath());
    }

    QString out;
    if (!runRake(QStringLiteral("todwg"),
                 {QStringLiteral("DXF_FILE=") + dxf, QStringLiteral("DWG_TARGET=") + QFileInfo(target).absoluteFilePath()},
                 tr8("กำลังบันทึก %1 เป็น DWG …").arg(QFileInfo(dxf).fileName()), &out))
        return;
    if (win->statusBar())
        win->statusBar()->showMessage(tr8("บันทึก DWG แล้ว: %1").arg(QDir::toNativeSeparators(out)), 15000);
    if (!test)
        QMessageBox::information(win, name(),
                                 tr8("บันทึกแล้ว: %1\n\nแบบ AutoCAD 2000 · ทำจาก %2\n"
                                     "ถ้ามี DWG ชื่อนี้อยู่ก่อน ไฟล์เดิมถูกเก็บเป็น .bak-<เวลา> ข้างไฟล์")
                                     .arg(QDir::toNativeSeparators(out), QDir::toNativeSeparators(dxf)));
}

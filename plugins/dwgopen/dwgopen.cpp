/*****************************************************************************/
/*  dwgopen.cpp - "Open DWG..." for LibreCAD 2.2.1.6 (converts through LibreDWG) */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/
// Context: user 2026-10-25 "ทำปุ่มเปิด DWG ใน LibreCAD ให้เลย" [Claude-Win@acer]. LibreCAD 2.2.1.6 marks
//          AutoCAD 2018 DWG (AC1032) unsupported (libdwgr.cpp:206-208) and 35 of the user's 36 DWG are AC1032.
// What:    adds "เปิด DWG…" to the File menu (after Open) and the File toolbar. It runs
//          `ruby -S rake -f ~/dxf-tools/Rakefile dwg` with DWG_FILE=<file> (LibreDWG dwg2dxf + dwg2lc.py, the same
//          path as `rake dwg`), then opens the DXF from the task's "OUT: <path>" line in a new tab.
//          The DWG itself is never written; the DXF goes to Documents/LibreCAD/from-dwg/.
// Why a plugin: a dll in Documents/LibreCAD/plugins needs no admin and no new LibreCAD.exe.
//          LibreCAD only offers plugin entries that run on the open drawing (execPlug dereferences the current
//          window, and the actions are disabled with no drawing open) - so the action is added to the File menu
//          directly and calls QC_ApplicationWindow's public slot slotFileOpen(QString) by name.
// Recover: delete Documents/LibreCAD/plugins/dwgopen0.dll and restart LibreCAD.
// Gotchas: LC_DWGOPEN_FILE=<dwg> opens that DWG right after start-up (used by the test; could back an
//          Explorer "open with" later). A DXF already converted is never overwritten silently: the plugin asks,
//          and rake dwg itself keeps the old one as .bak-<time>.
//          QProcess from a GUI process starts children with CREATE_NO_WINDOW, so no console flashes.
#include "dwgopen.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
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
#include <QTimer>
#include <QToolBar>

namespace {

const int kTimeoutMs = 600 * 1000;  // rake's guard.py stops dwg2dxf at 300 s; dwg2lc.py takes seconds

QString tr8(const char *s) { return QString::fromUtf8(s); }

// The open-folder icon of LibreCAD's own Open, with a red "DWG" band.
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

} // namespace

QString LC_DwgOpen::name() const
{
    return tr8("เปิด DWG");
}

PluginCapabilities LC_DwgOpen::getCapabilities() const
{
    // No entry in the Plugins menu (see the header). Called once from QC_ApplicationWindow::loadPlugins after the
    // menus and toolbars exist; add our action once the event loop runs.
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

    QAction *open = win->findChild<QAction *>(QStringLiteral("FileOpen"));
    auto *act = new QAction(dwgIcon(open ? open->icon() : win->style()->standardIcon(QStyle::SP_DialogOpenButton)),
                            tr8("เปิด DWG…"), this);
    act->setObjectName(QStringLiteral("FileOpenDwg"));
    const QString tip = tr8("เปิด DWG: แปลงเป็น DXF ด้วย LibreDWG ก่อน (ไฟล์ DWG ไม่ถูกแก้ · DXF อยู่ใน Documents\\LibreCAD\\from-dwg)");
    act->setToolTip(tip);
    act->setStatusTip(tip);
    connect(act, &QAction::triggered, this, &LC_DwgOpen::chooseAndOpen);

    if (QMenu *file = win->menuBar()->findChild<QMenu *>(QStringLiteral("File")))
        insertAfter(file, open, act);
    if (QToolBar *tb = win->findChild<QToolBar *>(QStringLiteral("file_toolbar")))
        insertAfter(tb, open, act);

    const QString autoFile = qEnvironmentVariable("LC_DWGOPEN_FILE");
    if (!autoFile.isEmpty())
        QTimer::singleShot(500, this, [this, autoFile] { openDwg(autoFile); });
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

    const QString ruby = findRuby();
    const QString tools = QDir::homePath() + QStringLiteral("/dxf-tools");
    if (ruby.isEmpty() || !QFileInfo::exists(tools + QStringLiteral("/Rakefile"))) {
        QMessageBox::warning(win, name(),
                             tr8("ต้องมี Ruby (scoop install ruby) และ %1\\Rakefile").arg(QDir::toNativeSeparators(tools)));
        return;
    }

    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("DWG_FILE"), src.absoluteFilePath());
    env.remove(QStringLiteral("LC_DWGOPEN_FILE"));
    proc.setProcessEnvironment(env);
    proc.setWorkingDirectory(tools);
    proc.setProcessChannelMode(QProcess::MergedChannels);

    QProgressDialog dlg(tr8("กำลังแปลง %1 …\n(ไฟล์ใหญ่ใช้ราว 15 วินาที)").arg(src.fileName()), QString(), 0, 0, win);
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
                      tools + QStringLiteral("/Rakefile"), QStringLiteral("dwg")});
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
    QString out;
    for (const QString &line : log.split('\n'))
        if (line.startsWith(QStringLiteral("OUT: ")))
            out = line.mid(5).trimmed();

    if (timedOut || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || out.isEmpty()
        || !QFileInfo::exists(out)) {
        const QString why = timedOut ? tr8("แปลงนานเกิน %1 วินาที — หยุดแล้ว").arg(kTimeoutMs / 1000)
                          : proc.error() == QProcess::FailedToStart ? tr8("เรียก %1 ไม่ได้").arg(ruby)
                                                                     : tr8("แปลงไม่สำเร็จ (rc %1)").arg(proc.exitCode());
        QMessageBox::warning(win, name(), why + QStringLiteral("\n\n") + lastLines(log, 15));
        return;
    }
    if (win && win->statusBar())
        win->statusBar()->showMessage(tr8("แปลงจาก DWG แล้ว: %1").arg(QDir::toNativeSeparators(out)), 10000);
    QMetaObject::invokeMethod(win, "slotFileOpen", Qt::QueuedConnection, Q_ARG(QString, out));
}

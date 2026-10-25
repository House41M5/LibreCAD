// Context: round-trip test suite 2026-10-25 [Claude-Win@acer] - put LibreCAD's own DXF writer in the loop.
// What:    only when LC_TEST_RESAVE=<log file> is set: once the drawing is open (polled every second) the plugin runs its own menu entry
//          ("lctest touch") on the drawing given on the command line - execComm adds one POINT on layer LCTEST, because
//          RS_Graphic::save writes nothing for an unmodified drawing - then every drawing window saves itself through
//          the public slot QC_MDIWindow::slotFileSave(bool&, bool), and LibreCAD quits 3 s later. The comparison
//          ignores layer LCTEST. Without the variable the plugin does nothing (its menu entry only adds the point).
//          Not part of the user's set-up: the test copies the dll into Documents/LibreCAD/plugins for the run and
//          removes it afterwards.
#include "lctest.h"
#include "document_interface.h"
#include <QAction>
#include <QApplication>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPointF>
#include <QTimer>
#include <cstdio>

static void logLine(const QString &s)
{
    if (FILE *f = std::fopen(qgetenv("LC_TEST_RESAVE").constData(), "a")) {
        std::fprintf(f, "%s\n", s.toUtf8().constData());
        std::fclose(f);
    }
}

PluginCapabilities LC_Test::getCapabilities() const
{
    PluginCapabilities pc;
    pc.menuEntryPoints << PluginMenuLocation(QStringLiteral("plugins_menu"), QStringLiteral("lctest touch"));
    if (!qEnvironmentVariableIsEmpty("LC_TEST_RESAVE")) {
        QObject *self = const_cast<LC_Test *>(this);
        // poll until the drawing from the command line is open (big files take longer than any fixed delay)
        auto *poll = new QTimer(self);
        QObject::connect(poll, &QTimer::timeout, [self, poll] {
            bool ready = false;
            for (QWidget *w : QApplication::topLevelWidgets())
                if (w->inherits("QC_ApplicationWindow"))
                    if (QMdiArea *mdi = w->findChild<QMdiArea *>())
                        for (QMdiSubWindow *sub : mdi->subWindowList())
                            ready = ready || sub->windowTitle().contains(QStringLiteral(".dxf"), Qt::CaseInsensitive);
            if (!ready)
                return;
            poll->stop();
            for (QAction *a : self->findChildren<QAction *>())
                if (a->text() == QStringLiteral("lctest touch"))
                    a->trigger();
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (!w->inherits("QC_ApplicationWindow"))
                    continue;
                if (QMdiArea *mdi = w->findChild<QMdiArea *>())
                    for (QMdiSubWindow *sub : mdi->subWindowList()) {
                        bool cancelled = false, ok = false;
                        const bool called = QMetaObject::invokeMethod(sub, "slotFileSave", Qt::DirectConnection,
                                                                      Q_RETURN_ARG(bool, ok), Q_ARG(bool &, cancelled),
                                                                      Q_ARG(bool, false));
                        logLine(QStringLiteral("window '%1' invoked=%2 saved=%3 cancelled=%4")
                                    .arg(sub->windowTitle()).arg(called).arg(ok).arg(cancelled));
                    }
            }
            QTimer::singleShot(3000, [] { QApplication::exit(0); });
        });
        poll->start(1000);
    }
    return pc;
}

void LC_Test::execComm(Document_Interface *doc, QWidget *, QString)
{
    const QString keep = doc->getCurrentLayer();
    doc->setLayer(QStringLiteral("LCTEST"));
    QPointF p(0, 0);
    doc->addPoint(&p);
    doc->setLayer(keep);
    logLine(QStringLiteral("touched: point on LCTEST"));
}

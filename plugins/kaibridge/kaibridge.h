/*****************************************************************************/
/*  kaibridge.h - Kai Bridge: JSON-over-named-pipe access to the open drawing */
/*                                                                           */
/*  Context: 2026-10-16 (Claude-Win). v0.2 - toolbar toggle, no modal window. */
/*  Document_Interface is only valid inside execComm() (it lives on the stack */
/*  of QC_ApplicationWindow::execPlug, qc_applicationwindow.cpp:780). So the  */
/*  pipe server lives in the plugin object, and when a request arrives it     */
/*  triggers its own hidden menu QAction -> LibreCAD calls execComm() with a  */
/*  fresh Document_Interface -> pending requests run -> UI is free again.     */
/*  Menu QActions are children of the plugin object (loadPlugins:730), which  */
/*  is how we find/hide/trigger them.                                         */
/*  No QtNetwork: LibreCAD does not ship Qt5Network.dll - use Win32 pipes.   */
/*  v0.1 (modal session dialog) dropped: user feedback 2026-10-16 - floating  */
/*  window blocked all input; asked for a toolbar icon instead.               */
/*****************************************************************************/

#ifndef KAIBRIDGE_H
#define KAIBRIDGE_H

#include "qc_plugininterface.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>

class QAction;
class QTimer;
class Document_Interface;

class KaiBridgeServer : public QObject
{
    Q_OBJECT

public:
    explicit KaiBridgeServer(QObject *parent);
    ~KaiBridgeServer() override;
    void setup(QObject *plugin);              // find actions, build toolbar
    void runPending(Document_Interface *doc); // called from execComm
    void toggle();

private slots:
    void poll();
    void turnOff();

private:
    bool turnOn(QString *err);
    void setState(const char *state);
    void reply(const QJsonObject &resp);
    bool writeAll(const QByteArray &data);
    void resetPipe(const char *why);
    void log(const QString &s);
    QJsonObject dispatch(Document_Interface *doc, const QString &cmd, const QJsonObject &a);

    QPointer<QAction> execAction;    // hidden menu entry -> execComm("Kai Bridge exec")
    QPointer<QAction> toggleAction;  // toolbar button
    QTimer *timer;
    void *pipe = nullptr;            // HANDLE
    bool on = false;
    bool connected = false;
    bool busy = false;
    bool stopRequested = false;
    QByteArray inbuf;
    QList<QJsonObject> pending;
    quint64 gen = 0;                 // connection generation; replies from an old connection are dropped
    QString token;
    QString sessionFile;
    int handled = 0;
};

class LC_KaiBridge : public QObject, QC_PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(QC_PluginInterface)
    Q_PLUGIN_METADATA(IID LC_DocumentInterface_iid FILE "kaibridge.json")

public:
    LC_KaiBridge();
    PluginCapabilities getCapabilities() const Q_DECL_OVERRIDE;
    QString name() const Q_DECL_OVERRIDE;
    void execComm(Document_Interface *doc, QWidget *parent, QString cmd) Q_DECL_OVERRIDE;

private:
    KaiBridgeServer *server;
};

#endif // KAIBRIDGE_H

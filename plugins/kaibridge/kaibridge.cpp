/*****************************************************************************/
/*  kaibridge.cpp - Kai Bridge plugin for LibreCAD 2.2.1.5  (v0.2)            */
/*                                                                           */
/*  Protocol: newline-delimited JSON over \\.\pipe\kaibridge (local only).    */
/*    request : {"token":"..","id":1,"cmd":"addLine","args":{...}}            */
/*    response: {"id":1,"ok":true,"result":{...}} | {"id":1,"ok":false,"error":".."} */
/*  Session info (pipe, token, pid) -> %LOCALAPPDATA%\KaiBridge\session.json   */
/*  written when the toolbar "K" button is switched ON, deleted when OFF.     */
/*  Angles in JSON are DEGREES; LibreCAD internals are radians except addArc. */
/*                                                                           */
/*  Gotchas (verified in LibreCAD source / live test 2026-10-16):             */
/*   - deleting Plug_Entity from getAllEntities() frees only the wrapper.     */
/*   - removed/undone entities stay in the container with VISIBLE=0 (same as  */
/*     frozen-layer entities) -> filtered out unless includeHidden.           */
/*   - undo is per command (Doc_plugin_interface opens its own undo sections) */
/*     - live test: Ctrl+Z reverted test commands one by one.                 */
/*   - execPlug() dereferences the active MDI window without a null check:    */
/*     only trigger the exec action while it is enabled (= a drawing is open).*/
/*   - getPoint/select spin processEvents -> poll() re-enters; guarded by busy.*/
/*****************************************************************************/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QToolBar>
#include <QtMath>

#include "document_interface.h"
#include "kaibridge.h"

static const char *PIPE_NAME = "\\\\.\\pipe\\kaibridge";
static const char *BRIDGE_VERSION = "0.2.4";
static const char *MENU_TOGGLE = "Kai Bridge on/off";
static const char *MENU_EXEC = "Kai Bridge exec";   // hidden; triggered by the server

/* ---------------- plugin entry ---------------- */

LC_KaiBridge::LC_KaiBridge() : server(new KaiBridgeServer(this)) {}

QString LC_KaiBridge::name() const { return QStringLiteral("Kai Bridge"); }

PluginCapabilities LC_KaiBridge::getCapabilities() const
{
    // menu QActions are created by LibreCAD right after this returns
    QTimer::singleShot(0, server, [s = server, p = const_cast<LC_KaiBridge *>(this)] { s->setup(p); });
    PluginCapabilities caps;
    caps.menuEntryPoints << PluginMenuLocation("plugins_menu", MENU_TOGGLE)
                         << PluginMenuLocation("plugins_menu", MENU_EXEC);
    return caps;
}

void LC_KaiBridge::execComm(Document_Interface *doc, QWidget *parent, QString cmd)
{
    Q_UNUSED(parent);
    if (cmd == MENU_EXEC) server->runPending(doc);
    else server->toggle();
}

/* ---------------- helpers ---------------- */

static QIcon makeIcon(const QColor &bg)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(2, 2, 28, 28, 6, 6);
    QFont f = p.font(); f.setBold(true); f.setPixelSize(20); p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(pm.rect(), Qt::AlignCenter, "K");
    return QIcon(pm);
}

static QString typeName(int t)
{
    static const char *n[] = {"point","line","constructionline","circle","arc","ellipse","image",
        "overlaybox","solid","mtext","text","insert","polyline","spline","splinepoints","hatch",
        "dimleader","dimaligned","dimlinear","dimradial","dimdiametric","dimangular","unknown"};
    return (t >= 0 && t <= DPI::UNKNOWN) ? QString(n[t]) : QString("unknown");
}

static QString keyName(int k)
{
    switch (k) {
    case DPI::TEXTCONTENT: return "text";     case DPI::BLKNAME: return "block";
    case DPI::LTYPE: return "linetype";       case DPI::TXTSTYLE: return "style";
    case DPI::LAYER: return "layer";          case DPI::STARTX: return "x1";
    case DPI::STARTY: return "y1";            case DPI::ENDX: return "x2";
    case DPI::ENDY: return "y2";              case DPI::VVECTORX: return "vx";
    case DPI::VVECTORY: return "vy";          case DPI::SIZEU: return "sizeu";
    case DPI::SIZEV: return "sizev";          case 30: return "z";
    case DPI::LWIDTH: return "lineweight";    case DPI::RADIUS: return "radius";
    case DPI::HEIGHT: return "height";        case DPI::XSCALE: return "xscale";
    case DPI::YSCALE: return "yscale";        case DPI::ZSCALE: return "zscale";
    case DPI::COLSPACE: return "colspace";    case DPI::ROWSPACE: return "rowspace";
    case DPI::LTSCALE: return "ltscale";      case DPI::STARTANGLE: return "angle1_rad";
    case DPI::ENDANGLE: return "angle2_rad";  case DPI::VISIBLE: return "visible";
    case DPI::COLOR: return "color";          case 70: return "flag70";
    case DPI::ROWCOUNT: return "rowcount";    case DPI::TXTALIGNH: return "halign";
    case DPI::TXTALIGNV: return "valign";     case DPI::REVERSED: return "reversed";
    default: return QString("k%1").arg(k);
    }
}

static QJsonObject entToJson(Plug_Entity *e)
{
    QHash<int, QVariant> d;
    e->getData(&d);
    QJsonObject o;
    for (auto it = d.constBegin(); it != d.constEnd(); ++it) {
        if (it.key() == DPI::ETYPE) o["type"] = typeName(it.value().toInt());
        else if (it.key() == DPI::EID) o["id"] = QString::number(it.value().toULongLong());
        else o[keyName(it.key())] = QJsonValue::fromVariant(it.value());
    }
    if (o.contains("angle1_rad")) o["angle1"] = qRadiansToDegrees(o["angle1_rad"].toDouble());
    if (o.contains("angle2_rad")) o["angle2"] = qRadiansToDegrees(o["angle2_rad"].toDouble());
    if (o["type"].toString() == "polyline") {
        QList<Plug_VertexData> vs;
        e->getPolylineData(&vs);
        QJsonArray va;
        for (const auto &v : vs) va.append(QJsonArray{v.point.x(), v.point.y(), v.bulge});
        o["vertices"] = va;
    }
    return o;
}

struct EntList {   // wrappers only; entities stay in the drawing
    QList<Plug_Entity *> l;
    ~EntList() { qDeleteAll(l); }
};

static void getEntities(Document_Interface *doc, EntList &out, bool includeHidden)
{
    QList<Plug_Entity *> all;
    doc->getAllEntities(&all, false);
    for (Plug_Entity *e : all) {
        QHash<int, QVariant> d; e->getData(&d);
        if (includeHidden || d.value(DPI::VISIBLE).toInt() == 1) out.l.append(e);
        else delete e;
    }
}

static QString entId(Plug_Entity *e)
{
    QHash<int, QVariant> d; e->getData(&d);
    return QString::number(d.value(DPI::EID).toULongLong());
}

static QString newestId(Document_Interface *doc)
{
    EntList el; getEntities(doc, el, true);
    qulonglong mx = 0;
    for (Plug_Entity *e : el.l) { QHash<int, QVariant> d; e->getData(&d); mx = qMax(mx, d.value(DPI::EID).toULongLong()); }
    return QString::number(mx);
}

static QPointF pt(const QJsonObject &a, const char *x, const char *y)
{
    return QPointF(a[x].toDouble(), a[y].toDouble());
}

static QJsonObject err(const QString &m) { return QJsonObject{{"__error", m}}; }

struct LayerScope {   // optional "layer" arg: switch for one add, then restore
    Document_Interface *d; QString prev; bool on = false;
    LayerScope(Document_Interface *doc, const QJsonObject &a) : d(doc) {
        if (a.contains("layer")) { prev = d->getCurrentLayer(); d->setLayer(a["layer"].toString()); on = true; }
    }
    ~LayerScope() { if (on) d->setLayer(prev); }
};

/* ---------------- server ---------------- */

KaiBridgeServer::KaiBridgeServer(QObject *parent) : QObject(parent)
{
    timer = new QTimer(this);
    timer->setInterval(15);
    connect(timer, &QTimer::timeout, this, &KaiBridgeServer::poll);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &KaiBridgeServer::turnOff);
}

// v0.2.4: the plugin object is destroyed at library unload, AFTER QApplication is gone.
// v0.2.3 called turnOff() -> setState() -> QPixmap here -> Qt fatal 0xc0000409 on every
// LibreCAD exit (Event Log 2026-10-16 23:02). Raw Win32 cleanup only - no Qt GUI, no log().
KaiBridgeServer::~KaiBridgeServer()
{
    if (pipe) { DisconnectNamedPipe(pipe); CloseHandle(pipe); pipe = nullptr; }
    if (!sessionFile.isEmpty()) DeleteFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(sessionFile).utf16()));
}

void KaiBridgeServer::setup(QObject *plugin)
{
    if (toggleAction) return;
    // hide the exec entry by removing it from the menu - NOT setVisible(false):
    // in Qt5 an invisible action is also disabled and trigger() becomes a no-op
    // (v0.2.0 bug: every request answered "no drawing open")
    for (QAction *a : plugin->findChildren<QAction *>()) {
        if (a->data().toString() == MENU_EXEC) {
            execAction = a;
            for (QWidget *w : a->associatedWidgets()) w->removeAction(a);
        }
    }
    QMainWindow *mw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->inherits("QC_ApplicationWindow")) mw = qobject_cast<QMainWindow *>(w);
    toggleAction = new QAction(makeIcon(QColor(120, 120, 120)), "Kai Bridge", this);
    toggleAction->setCheckable(true);
    toggleAction->setToolTip("Kai Bridge: OFF (click to let Kai connect)");
    connect(toggleAction, &QAction::triggered, this, [this] { toggle(); });
    if (mw) {
        QToolBar *tb = mw->addToolBar("Kai");
        tb->setObjectName("KaiBridgeToolBar");
        tb->addAction(toggleAction);
    }
}

void KaiBridgeServer::setState(const char *state)
{
    if (!toggleAction || !qApp || QCoreApplication::closingDown()) return;
    QString s(state);
    QColor c = s == "off" ? QColor(120, 120, 120) : s == "busy" ? QColor(230, 140, 0)
             : s == "connected" ? QColor(0, 150, 70) : QColor(0, 110, 200);
    toggleAction->setIcon(makeIcon(c));
    toggleAction->setChecked(on);
    QString tip = s == "off" ? "Kai Bridge: OFF (click to let Kai connect)"
                : s == "busy" ? "Kai Bridge: working - if Kai asked you to click/select, do it or press Esc"
                : QString("Kai Bridge: %1 - %2 commands (click to turn off)").arg(s).arg(handled);
    toggleAction->setToolTip(tip);
}

void KaiBridgeServer::toggle()
{
    if (busy) {   log("toggle refused: busy");  // v0.2.2: turning off mid-getPoint orphaned the pick and broke the client
        setState("busy");
        QMessageBox::information(nullptr, "Kai Bridge",
            "Kai is waiting for you in the drawing (click a point / select).\n"
            "Press Esc in the drawing to cancel, then turn Kai Bridge off.");
        return;
    }
    if (on) { log("toggle: off by user"); turnOff(); return; }
    QString e;
    if (!turnOn(&e)) {
        setState("off");
        QMessageBox::warning(nullptr, "Kai Bridge", e);
    }
}

bool KaiBridgeServer::turnOn(QString *e)
{
    HANDLE h = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 1 << 16, 1 << 16, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *e = QString("Cannot create pipe %1 (error %2) - Kai Bridge already on in another LibreCAD?")
                 .arg(PIPE_NAME).arg(GetLastError());
        return false;
    }
    pipe = h;
    token = QString::number(QRandomGenerator::system()->generate64(), 16)
          + QString::number(QRandomGenerator::system()->generate64(), 16);
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/KaiBridge";
    QDir().mkpath(dir);
    sessionFile = dir + "/session.json";
    QFile f(sessionFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        CloseHandle(pipe); pipe = nullptr;
        *e = QString("Cannot write %1").arg(sessionFile);
        return false;
    }
    f.write(QJsonDocument(QJsonObject{
        {"pipe", PIPE_NAME}, {"token", token}, {"pid", qint64(GetCurrentProcessId())},
        {"version", BRIDGE_VERSION},
        {"started", QDateTime::currentDateTime().toString(Qt::ISODate)}}).toJson());
    f.close();
    on = true; handled = 0; gen++;
    log(QString("ON pid %1").arg(GetCurrentProcessId()));
    timer->start();
    setState("waiting");
    return true;
}

void KaiBridgeServer::turnOff()
{
    if (!on && !pipe) return;   // already off (aboutToQuit already ran)
    log("OFF");
    timer->stop();
    if (pipe) { DisconnectNamedPipe(pipe); CloseHandle(pipe); pipe = nullptr; }
    if (!sessionFile.isEmpty()) { QFile::remove(sessionFile); sessionFile.clear(); }
    on = connected = stopRequested = false;
    inbuf.clear();
    pending.clear();
    token.clear();
    setState("off");
}

void KaiBridgeServer::log(const QString &s)
{
    // %LOCALAPPDATA%\KaiBridge\log.txt - kept small: truncated when > 256 KB
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/KaiBridge";
    QDir().mkpath(dir);
    QFile f(dir + "/log.txt");
    bool big = f.exists() && f.size() > (256 << 10);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | (big ? QIODevice::Truncate : QIODevice::Append)))
        // QLocale::c(): with Windows region th-TH, QDateTime::toString printed Thai digits
        f.write((QLocale::c().toString(QDateTime::currentDateTime(), "yyyy-MM-dd HH:mm:ss.zzz ") + s + "\n").toUtf8());
}

void KaiBridgeServer::resetPipe(const char *why)
{
    log(QString("disconnect: %1 (gen %2, busy %3)").arg(why).arg(gen).arg(busy));
    DisconnectNamedPipe(pipe);
    connected = false;
    gen++;
    inbuf.clear();
    pending.clear();
    setState("waiting");
}

bool KaiBridgeServer::writeAll(const QByteArray &data)
{
    qint64 off = 0;
    QElapsedTimer t; t.start();
    while (off < data.size()) {
        DWORD n = 0;
        DWORD chunk = DWORD(qMin<qint64>(data.size() - off, 1 << 15));
        if (!WriteFile(pipe, data.constData() + off, chunk, &n, nullptr)) return false;
        off += n;
        if (n == 0) {
            if (t.elapsed() > 10000) return false;   // client not reading
            Sleep(2);
        }
    }
    return true;
}

void KaiBridgeServer::reply(const QJsonObject &resp)
{
    if (pipe && !writeAll(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n")) resetPipe("write failed");
}

void KaiBridgeServer::poll()
{
    if (!on || !pipe) return;
    if (!connected) {
        if (ConnectNamedPipe(pipe, nullptr)) { connected = true; }
        else {
            DWORD g = GetLastError();
            if (g == ERROR_PIPE_CONNECTED) connected = true;
            else if (g == ERROR_NO_DATA) { DisconnectNamedPipe(pipe); return; }
            else return;  // ERROR_PIPE_LISTENING
        }
        log(QString("client connected (gen %1)").arg(gen));
        setState(busy ? "busy" : "connected");
    }
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) { if (!busy) resetPipe("peek failed (client gone)"); return; }
    if (avail > 0) {
        QByteArray chunk(int(avail), Qt::Uninitialized);
        DWORD got = 0;
        if (!ReadFile(pipe, chunk.data(), avail, &got, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE && !busy) resetPipe("read: broken pipe");
            return;
        }
        inbuf.append(chunk.left(int(got)));
        int nl;
        while ((nl = inbuf.indexOf('\n')) >= 0) {
            QByteArray line = inbuf.left(nl).trimmed();
            inbuf.remove(0, nl + 1);
            if (line.isEmpty()) continue;
            QJsonParseError pe;
            QJsonDocument jd = QJsonDocument::fromJson(line, &pe);
            if (pe.error != QJsonParseError::NoError || !jd.isObject()) {
                reply({{"ok", false}, {"error", "bad json: " + pe.errorString()}});
                continue;
            }
            QJsonObject req = jd.object();
            if (req["token"].toString() != token) {
                reply({{"id", req["id"]}, {"ok", false}, {"error", "bad token"}});
                continue;
            }
            if (req["cmd"].toString() == "ping") {   // no drawing needed - also our diagnostics
                reply({{"id", req["id"]}, {"ok", true}, {"result", QJsonObject{
                    {"plugin", "kaibridge"}, {"version", BRIDGE_VERSION},
                    {"pid", qint64(GetCurrentProcessId())},
                    {"execActionFound", !execAction.isNull()},
                    {"execActionEnabled", execAction ? execAction->isEnabled() : false},
                    {"handled", handled}}}});
                continue;
            }
            req["__gen"] = QString::number(gen);
            pending.append(req);
        }
    }
    if (busy || pending.isEmpty()) return;
    if (!execAction || !execAction->isEnabled()) {
        QString why = !execAction ? "internal: hidden exec action not found"
                                  : "no drawing open in LibreCAD (exec action disabled)";
        while (!pending.isEmpty())
            reply({{"id", pending.takeFirst()["id"]}, {"ok", false}, {"error", why}});
        return;
    }
    busy = true;
    setState("busy");
    log(QString("exec start: %1 pending").arg(pending.size()));
    execAction->trigger();     // -> LibreCAD execPlug -> execComm(MENU_EXEC) -> runPending()
    busy = false;
    log("exec end");
    if (stopRequested) { log("stop requested by client"); turnOff(); return; }
    // v0.2.2: the client may have gone away while we were busy (peek failures are
    // ignored while busy) - re-check so the button does not stay green forever
    DWORD dummy = 0;
    if (on && connected && !PeekNamedPipe(pipe, nullptr, 0, nullptr, &dummy, nullptr)) resetPipe("client gone while busy");
    if (on) setState(connected ? "connected" : "waiting");
}

void KaiBridgeServer::runPending(Document_Interface *doc)
{
    while (!pending.isEmpty()) {
        QJsonObject req = pending.takeFirst();
        QString cmd = req["cmd"].toString();
        QJsonObject r;
        try { r = dispatch(doc, cmd, req["args"].toObject()); }
        catch (...) { r = err("exception in " + cmd); }
        handled++;
        QJsonObject resp{{"id", req["id"]}};
        if (r.contains("__error")) { resp["ok"] = false; resp["error"] = r["__error"]; }
        else { resp["ok"] = true; resp["result"] = r; }
        if (req["__gen"].toString() != QString::number(gen)) continue;  // client from an old connection
        reply(resp);
    }
}

QJsonObject KaiBridgeServer::dispatch(Document_Interface *doc, const QString &cmd, const QJsonObject &a)
{
    if (cmd == "ping") {
        return {{"plugin", "kaibridge"}, {"version", BRIDGE_VERSION}, {"pid", qint64(GetCurrentProcessId())}};
    }
    if (cmd == "info") {
        int c; DPI::LineWidth w; DPI::LineType t;
        doc->getCurrentLayerProperties(&c, &w, &t);
        return {{"layers", QJsonArray::fromStringList(doc->getAllLayer())},
                {"currentLayer", doc->getCurrentLayer()},
                {"currentLayerColor", c}, {"currentLayerWidth", int(w)}, {"currentLayerType", int(t)},
                {"blocks", QJsonArray::fromStringList(doc->getAllBlocks())}};
    }
    if (cmd == "entities") {
        EntList el; getEntities(doc, el, a["includeHidden"].toBool());
        QString onlyLayer = a["layer"].toString(), onlyType = a["type"].toString();
        QJsonArray arr;
        for (Plug_Entity *e : el.l) {
            QJsonObject o = entToJson(e);
            if (!onlyLayer.isEmpty() && o["layer"].toString() != onlyLayer) continue;
            if (!onlyType.isEmpty() && o["type"].toString() != onlyType) continue;
            arr.append(o);
        }
        return {{"count", arr.size()}, {"entities", arr}};
    }
    if (cmd == "addPoint" || cmd == "addLine" || cmd == "addCircle" || cmd == "addArc" ||
        cmd == "addPolyline" || cmd == "addText") {
        LayerScope ls(doc, a);
        if (cmd == "addPoint") { QPointF p = pt(a, "x", "y"); doc->addPoint(&p); }
        else if (cmd == "addLine") { QPointF p1 = pt(a, "x1", "y1"), p2 = pt(a, "x2", "y2"); doc->addLine(&p1, &p2); }
        else if (cmd == "addCircle") { QPointF c = pt(a, "cx", "cy"); doc->addCircle(&c, a["r"].toDouble()); }
        else if (cmd == "addArc") {   // degrees (Doc_plugin_interface::addArc converts deg2rad)
            QPointF c = pt(a, "cx", "cy");
            doc->addArc(&c, a["r"].toDouble(), a["a1"].toDouble(), a["a2"].toDouble());
        } else if (cmd == "addPolyline") {
            std::vector<Plug_VertexData> vs;
            for (const QJsonValue &v : a["points"].toArray()) {
                QJsonArray p = v.toArray();
                if (p.size() < 2) return err("points must be [[x,y(,bulge)],...]");
                vs.emplace_back(QPointF(p[0].toDouble(), p[1].toDouble()), p.size() > 2 ? p[2].toDouble() : 0.0);
            }
            if (vs.size() < 2) return err("need >= 2 points");
            doc->addPolyline(vs, a["closed"].toBool());
        } else {  // addText: angle in degrees -> radians (RS_TextData stores radians)
            QPointF p = pt(a, "x", "y");
            doc->addText(a["text"].toString(), a["style"].toString("standard"), &p,
                         a["height"].toDouble(2.5), qDegreesToRadians(a["angle"].toDouble(0)),
                         DPI::HAlign(a["halign"].toInt(0)), DPI::VAlign(a["valign"].toInt(2)));
        }
        doc->updateView();
        return {{"id", newestId(doc)}};
    }
    if (cmd == "setLayer") {
        doc->setLayer(a["name"].toString());
        if (a.contains("color") || a.contains("lineweight") || a.contains("linetype")) {
            int c; DPI::LineWidth w; DPI::LineType t;
            doc->getCurrentLayerProperties(&c, &w, &t);
            doc->setCurrentLayerProperties(a["color"].toInt(c), DPI::LineWidth(a["lineweight"].toInt(int(w))),
                                           DPI::LineType(a["linetype"].toInt(int(t))));
        }
        return {{"currentLayer", doc->getCurrentLayer()}};
    }
    if (cmd == "deleteLayer") {
        return {{"deleted", doc->deleteLayer(a["name"].toString())}};
    }
    if (cmd == "remove" || cmd == "move" || cmd == "rotate" || cmd == "scale") {
        QSet<QString> want;
        for (const QJsonValue &v : a["ids"].toArray()) want.insert(v.isString() ? v.toString() : QString::number(qulonglong(v.toDouble())));
        if (want.isEmpty()) return err("ids required");
        EntList el; getEntities(doc, el, false);
        DPI::Disposition disp = a["copy"].toBool() ? DPI::KEEP_ORIGINAL : DPI::DELETE_ORIGINAL;
        QJsonArray done;
        for (Plug_Entity *e : el.l) {
            QString id = entId(e);
            if (!want.contains(id)) continue;
            if (cmd == "remove") doc->removeEntity(e);
            else if (cmd == "move") e->move(pt(a, "dx", "dy"), disp);
            else if (cmd == "rotate") e->rotate(pt(a, "cx", "cy"), qDegreesToRadians(a["angle"].toDouble()), disp);
            else e->scale(pt(a, "cx", "cy"), QPointF(a["fx"].toDouble(1), a["fy"].toDouble(a["fx"].toDouble(1))), disp);
            done.append(id);
        }
        doc->updateView();
        return {{"done", done}, {"notFound", int(want.size() - done.size())}};
    }
    if (cmd == "getPoint") {   // user clicks in the drawing (UI is not blocked)
        QPointF p, base; bool hasBase = a.contains("bx");
        if (hasBase) base = pt(a, "bx", "by");
        // cancel with Esc, not right-click (right-click -> finish() -> same freed-action risk as getEnt)
        if (!doc->getPoint(&p, a["message"].toString("Kai: click a point (Esc = cancel)"), hasBase ? &base : nullptr))
            return err("cancelled");
        return {{"x", p.x()}, {"y", p.y()}};
    }
    if (cmd == "select") {
        // DISABLED in v0.2.4 - no safe way found in LibreCAD 2.2.1.5's plugin API:
        //  - doc->getSelect(): always returns false (finish() drops the action before isValid();
        //    the bundled "List entities" plugin fails the same way - user test 2026-10-16)
        //  - doc->getEnt() loop: LibreCAD crashed (0xc0000005 inside LibreCAD.exe, called from
        //    execComm) when the user ended the picking with Esc - the action is likely freed
        //    while getEnt still reads it. Only a left-click on an entity is a safe completion.
        return err("select is disabled: unsafe in LibreCAD 2.2.1.5 (crashed LibreCAD) - "
                   "see kaibridge.cpp comment");
    }
    if (cmd == "view") { doc->updateView(); return {}; }
    if (cmd == "stop") { stopRequested = true; return {{"stopping", true}}; }
    return err("unknown cmd: " + cmd);
}

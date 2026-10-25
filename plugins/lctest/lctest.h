/*  lctest.h - test helper for LibreCAD 2.2.1.6: LC_TEST_RESAVE=1 makes LibreCAD save the drawing it opened  */
/*  This library is free software, licensed under the terms of the GNU General Public License v2 or later.  */
#ifndef LCTEST_H
#define LCTEST_H
#include "qc_plugininterface.h"
#include <QObject>

class LC_Test : public QObject, QC_PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(QC_PluginInterface)
    Q_PLUGIN_METADATA(IID LC_DocumentInterface_iid FILE "lctest.json")
public:
    PluginCapabilities getCapabilities() const Q_DECL_OVERRIDE;
    QString name() const Q_DECL_OVERRIDE { return QStringLiteral("lctest"); }
    void execComm(Document_Interface *doc, QWidget *parent, QString cmd) Q_DECL_OVERRIDE;
};
#endif

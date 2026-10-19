/*****************************************************************************/
/*  pidlines.h - P&ID signal line decorator for LibreCAD (Kai build)         */
/*                                                                           */
/*  Places a symbol block (//, X, L, o ...) at even spacing along selected   */
/*  lines, arcs and polylines and cuts the line under each symbol. Each      */
/*  symbol is a DXF file in a pattern folder so new patterns need no rebuild.*/
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*****************************************************************************/

#ifndef PIDLINES_H
#define PIDLINES_H

#include "qc_plugininterface.h"
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QCheckBox;

class LC_PidLines : public QObject, QC_PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(QC_PluginInterface)
    Q_PLUGIN_METADATA(IID LC_DocumentInterface_iid FILE  "pidlines.json")

 public:
    virtual PluginCapabilities getCapabilities() const Q_DECL_OVERRIDE;
    virtual QString name() const Q_DECL_OVERRIDE;
    virtual void execComm(Document_Interface *doc,
                          QWidget *parent, QString cmd) Q_DECL_OVERRIDE;
};

class PidLinesDlg : public QDialog
{
    Q_OBJECT

public:
    //! names = pattern names (file name without pid_), labels = what the list shows
    PidLinesDlg(const QStringList &names, const QStringList &labels, QWidget *parent = nullptr);

    //! index into the pattern list, -1 = remove symbols only
    int pattern() const;
    double spacing() const;
    double scale() const;
    bool cutLine() const;
    void saveSettings() const;

private slots:
    void patternChanged();

private:
    int count;
    QComboBox *patternBox;
    QDoubleSpinBox *spacingBox;
    QDoubleSpinBox *scaleBox;
    QCheckBox *cutBox;
};

#endif // PIDLINES_H

/*****************************************************************************/
/*  dwgopen.h - "Open DWG..." / "Save as DWG..." for LibreCAD 2.2.1.6        */
/*              (converts through LibreDWG)                                  */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 2 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

#ifndef DWGOPEN_H
#define DWGOPEN_H

#include "qc_plugininterface.h"
#include <QObject>
#include <QStringList>

class QMainWindow;
class QMdiSubWindow;

class LC_DwgOpen : public QObject, QC_PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(QC_PluginInterface)
    Q_PLUGIN_METADATA(IID LC_DocumentInterface_iid FILE "dwgopen.json")

public:
    PluginCapabilities getCapabilities() const Q_DECL_OVERRIDE;
    QString name() const Q_DECL_OVERRIDE;
    void execComm(Document_Interface *doc, QWidget *parent, QString cmd) Q_DECL_OVERRIDE;

private slots:
    void install();
    void chooseAndOpen();

private:
    void openDwg(const QString &dwg);
    void saveAsDwg(const QString &presetTarget);
    QString activeDxfPath(QMdiSubWindow *sub);
    bool runRake(const QString &task, const QStringList &env, const QString &busy, QString *out);
    QMainWindow *mainWindow() const;
    bool installed = false;
};

#endif // DWGOPEN_H

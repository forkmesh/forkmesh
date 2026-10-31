#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

namespace forkmesh::launchericon {

QString dataHome();
bool launcherInstalled(const QString &dataHome);
QByteArray launcherEntry(const QString &clientBin);
bool installLauncher(const QString &dataHome, const QString &clientBin,
                     const QString &runningBin);
QString launcherTarget(const QByteArray &entry);
QString currentClient(const QString &installedBin, const QString &sourceBin,
                      const QString &runningBin);
bool repairLauncher(const QString &dataHome, const QString &retiredRoot,
                    const QString &clientBin);
QStringList refresh(const QString &dataHome, const QImage &icon, const QByteArray &svg);
void refreshInstalled();

}

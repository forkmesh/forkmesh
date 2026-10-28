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
QStringList refresh(const QString &dataHome, const QImage &icon, const QByteArray &svg);
void refreshInstalled();

}

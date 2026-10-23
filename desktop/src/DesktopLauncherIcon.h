#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

namespace forkmesh::launchericon {

QString dataHome();
bool launcherInstalled(const QString &dataHome);
QStringList refresh(const QString &dataHome, const QImage &icon, const QByteArray &svg);
void refreshInstalled();

}

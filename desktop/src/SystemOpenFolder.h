#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh::systemopen {

QString folderFromArgument(const QString &argument);
QString folderFromLaunchArguments(const QStringList &arguments);
bool isGitCheckout(const QString &path);
QStringList gitCheckoutsAt(const QString &folder);
QString enclosingGitCheckout(const QString &folder);
int repositoryContaining(const QString &folder,
                         const QStringList &repositoryPaths);

}

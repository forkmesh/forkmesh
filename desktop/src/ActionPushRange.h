#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::actions {

using PushRangeGit = std::function<bool(const QStringList &args, QString *out)>;

bool isKnownPushBase(const QString &before);
bool isMetadataPath(const QString &path);
bool isMetadataOnlyChange(const QStringList &paths);
QStringList pushRangeChangedPaths(const PushRangeGit &git,
                                  const QString &before,
                                  const QString &after);
QStringList pushRangeChangedPaths(const QString &repoPath,
                                  const QString &before,
                                  const QString &after);

}

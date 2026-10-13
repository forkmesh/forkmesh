#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

namespace forkmesh::paths {

// Which of these checkouts a path lies in: the index of the deepest checkout
// that is the path itself or one of its parents, or -1 for none.
//
// The build bakes in FORKMESH_SOURCE_DIR, the desktop/ CMake directory, which
// sits one level inside the repository a user registers. Matching it with ==
// never found that repository, so every "this app's own repo" lookup fell back
// to whichever repository the Issues tab happened to show.
inline int checkoutContaining(const QStringList &checkouts, const QString &path)
{
    if (path.trimmed().isEmpty())
        return -1;
    const QString target = QDir::cleanPath(QDir(path).absolutePath());
    int best = -1;
    qsizetype bestLength = -1;
    for (int i = 0; i < checkouts.size(); ++i) {
        if (checkouts.at(i).trimmed().isEmpty())
            continue;
        const QString root = QDir::cleanPath(QDir(checkouts.at(i)).absolutePath());
        const QString prefix =
            root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        if ((target == root || target.startsWith(prefix)) &&
            root.size() > bestLength) {
            best = i;
            bestLength = root.size();
        }
    }
    return best;
}

} // namespace forkmesh::paths

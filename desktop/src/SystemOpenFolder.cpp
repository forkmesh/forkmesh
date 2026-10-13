#include "SystemOpenFolder.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace forkmesh::systemopen {

namespace {

constexpr Qt::CaseSensitivity kPathCase =
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif

QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath())
                               : canonical;
}

}

QString folderFromArgument(const QString &argument)
{
    if (argument.isEmpty() || argument.size() > 4096)
        return {};
    QString path = argument;
    if (argument.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(argument);
        if (!url.isValid() || !url.isLocalFile())
            return {};
        path = url.toLocalFile();
    }
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
        return {};
    const QFileInfo info(path);
    if (!info.isDir())
        return {};
    return QDir::cleanPath(info.absoluteFilePath());
}

QString folderFromLaunchArguments(const QStringList &arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i - 1) == QLatin1String("--project"))
            continue;
        const QString folder = folderFromArgument(arguments.at(i));
        if (!folder.isEmpty())
            return folder;
    }
    return {};
}

bool isGitCheckout(const QString &path)
{
    const QDir dir(path);
    return dir.exists(QStringLiteral(".git")) ||
           (dir.exists(QStringLiteral("HEAD")) &&
            dir.exists(QStringLiteral("objects")));
}

QStringList gitCheckoutsAt(const QString &folder)
{
    if (isGitCheckout(folder))
        return {QDir::cleanPath(folder)};
    QStringList paths;
    const QFileInfoList children = QDir(folder).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &child : children) {
        if (!child.isSymLink() && isGitCheckout(child.absoluteFilePath()))
            paths.append(QDir::cleanPath(child.absoluteFilePath()));
    }
    return paths;
}

QString enclosingGitCheckout(const QString &folder)
{
    QDir dir(QDir::cleanPath(folder));
    while (dir.cdUp()) {
        if (isGitCheckout(dir.absolutePath()))
            return QDir::cleanPath(dir.absolutePath());
    }
    return {};
}

int repositoryContaining(const QString &folder,
                         const QStringList &repositoryPaths)
{
    const QString target = comparablePath(folder);
    int best = -1;
    qsizetype bestLength = -1;
    for (int i = 0; i < repositoryPaths.size(); ++i) {
        if (repositoryPaths.at(i).isEmpty())
            continue;
        const QString root = comparablePath(repositoryPaths.at(i));
        const bool inside =
            target.compare(root, kPathCase) == 0 ||
            target.startsWith(root.endsWith(QLatin1Char('/'))
                                  ? root
                                  : root + QLatin1Char('/'),
                              kPathCase);
        if (inside && root.size() > bestLength) {
            best = i;
            bestLength = root.size();
        }
    }
    return best;
}

}

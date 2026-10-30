#pragma once

#include "FastScratchDisk.h"
#include "SafeDelete.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace forkmesh::agentscratch {

inline bool isAgentRoot(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return false;
    QFile file(QDir(dir).filePath(QStringLiteral(".gitignore")));
    return file.open(QIODevice::ReadOnly) &&
           file.readLine().startsWith("# ForkMesh agent worktrees");
}

inline QString markerText()
{
    return QStringLiteral("# ForkMesh agent worktrees. Never part of the project.\n*\n");
}

inline QString legacyRootFolder(const QString &worktreeRoot)
{
    return QDir(worktreeRoot).filePath(QStringLiteral(".scratch"));
}

inline QString fastRootFolder(const QString &worktreeRoot)
{
    const QString parent = fastdisk::scratchParent(fastdisk::activeRoot(), worktreeRoot);
    return parent.isEmpty() ? QString() : parent + QStringLiteral("/.scratch");
}

inline QString rootFolder(const QString &worktreeRoot)
{
    const QString fast = fastRootFolder(worktreeRoot);
    return fast.isEmpty() ? legacyRootFolder(worktreeRoot) : fast;
}

inline QStringList rootFolders(const QString &worktreeRoot)
{
    QStringList roots;
    const QString fast = fastRootFolder(worktreeRoot);
    if (!fast.isEmpty())
        roots << fast;
    roots << legacyRootFolder(worktreeRoot);
    return roots;
}

inline bool prepareFastParent(const QString &folder, const QString &worktreeRoot)
{
    const QString active = fastdisk::activeRoot();
    if (active.isEmpty() || worktreeRoot.isEmpty() ||
        !folder.startsWith(fastdisk::scratchBase(active) + QLatin1Char('/')))
        return true;
    const QString parent = QFileInfo(QFileInfo(folder).absolutePath()).absolutePath();
    if (!QDir().mkpath(parent))
        return false;
    const auto writeIfMissing = [](const QString &path, const QString &text) {
        if (QFileInfo::exists(path))
            return true;
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(text.toUtf8()) >= 0;
    };
    return writeIfMissing(QDir(parent).filePath(QStringLiteral(".gitignore")), markerText()) &&
           writeIfMissing(QDir(parent).filePath(fastdisk::ownerFileName()),
                          QDir::cleanPath(QDir(worktreeRoot).absolutePath()) +
                              QLatin1Char('\n'));
}

inline QStringList foldersFor(const QString &worktreePath)
{
    const QString trimmed = worktreePath.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QFileInfo info(QDir::cleanPath(QDir(trimmed).absolutePath()));
    const QString name = info.fileName();
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) ||
        QFileInfo(info.filePath() + QStringLiteral("/.git")).isDir() ||
        !isAgentRoot(info.absolutePath()))
        return {};
    QStringList folders;
    for (const QString &root : rootFolders(info.absolutePath()))
        folders << QDir(root).filePath(name);
    return folders;
}

inline QString folderFor(const QString &worktreePath)
{
    const QStringList folders = foldersFor(worktreePath);
    return folders.isEmpty() ? QString() : folders.first();
}

inline QStringList environment(const QString &folder, const QString &worktreePath = QString())
{
    if (folder.isEmpty())
        return {};
    const QString worktreeRoot =
        worktreePath.trimmed().isEmpty()
            ? QString()
            : QFileInfo(QDir::cleanPath(QDir(worktreePath).absolutePath())).absolutePath();
    if (!prepareFastParent(folder, worktreeRoot))
        return {};
    const QString tmp = folder + QStringLiteral("/tmp");
    if (!QDir().mkpath(tmp))
        return {};
    QStringList env{QStringLiteral("FORKMESH_AGENT_SCRATCH=") + folder,
                    QStringLiteral("TMPDIR=") + tmp, QStringLiteral("TMP=") + tmp,
                    QStringLiteral("TEMP=") + tmp};
    const QString active = fastdisk::activeRoot();
    if (!active.isEmpty() && folder.startsWith(active + QLatin1Char('/'))) {
        const QString ccache = fastdisk::ccacheDir(active);
        if (QDir().mkpath(ccache))
            env << QStringLiteral("CCACHE_DIR=") + ccache
                << QStringLiteral("CCACHE_MAXSIZE=40G");
    }
    return env;
}

inline bool remove(const QString &folder)
{
    if (folder.isEmpty())
        return true;
    const QFileInfo info(folder);
    if (!info.exists() && !info.isSymLink())
        return true;
    return safedelete::removeTree(folder, info.absolutePath());
}

inline bool removeAll(const QString &worktreePath)
{
    bool ok = true;
    for (const QString &folder : foldersFor(worktreePath))
        ok = remove(folder) && ok;
    return ok;
}

inline QStringList orphans(const QString &worktreeRoot)
{
    QStringList found;
    if (!isAgentRoot(worktreeRoot))
        return found;
    for (const QString &rootPath : rootFolders(worktreeRoot)) {
        const QDir root(rootPath);
        for (const QString &name :
             root.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
            const QFileInfo worktree(QDir(worktreeRoot).filePath(name));
            if (!worktree.exists() && !worktree.isSymLink())
                found << root.filePath(name);
        }
    }
    return found;
}

inline int sweepOrphans(const QString &worktreeRoot)
{
    int removed = 0;
    for (const QString &folder : orphans(worktreeRoot))
        removed += remove(folder) ? 1 : 0;
    return removed;
}

inline bool ownedByUs(const QFileInfo &info)
{
#if defined(Q_OS_UNIX)
    return info.ownerId() == ::getuid();
#else
    Q_UNUSED(info);
    return true;
#endif
}

inline QVector<QPair<QString, QString>> sessionLeftovers(int sessionId,
                                                         const QString &buildsDir,
                                                         const QString &tempDir)
{
    QVector<QPair<QString, QString>> found;
    if (sessionId <= 0)
        return found;
    const QString id = QString::number(sessionId);
    const auto collect = [&found](const QString &parent, const QString &pattern) {
        if (parent.trimmed().isEmpty() || !QFileInfo(parent).isDir())
            return;
        const QRegularExpression match(pattern);
        const QDir dir(parent);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : entries) {
            if (match.match(entry.fileName()).hasMatch() &&
                (entry.isSymLink() || ownedByUs(entry)))
                found.append({entry.absoluteFilePath(), dir.absolutePath()});
        }
    };
    collect(buildsDir, QStringLiteral("^agent-%1([-.].*)?$").arg(id));
    collect(tempDir, QStringLiteral("^(_tmp_)?fm%1([-_.].*)?$").arg(id));
    collect(QDir(tempDir).filePath(QStringLiteral("forkmesh-agent-jails")),
            QStringLiteral("^s%1(-triage)?$").arg(id));
    return found;
}

inline int removeSessionLeftovers(int sessionId, const QString &buildsDir,
                                  const QString &tempDir)
{
    int removed = 0;
    for (const auto &leftover : sessionLeftovers(sessionId, buildsDir, tempDir))
        removed += safedelete::removeTree(leftover.first, leftover.second) ? 1 : 0;
    return removed;
}

} // namespace forkmesh::agentscratch

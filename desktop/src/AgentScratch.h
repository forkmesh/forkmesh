#pragma once

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

inline QString rootFolder(const QString &worktreeRoot)
{
    return QDir(worktreeRoot).filePath(QStringLiteral(".scratch"));
}

inline QString folderFor(const QString &worktreePath)
{
    const QString trimmed = worktreePath.trimmed();
    if (trimmed.isEmpty())
        return QString();
    const QFileInfo info(QDir::cleanPath(QDir(trimmed).absolutePath()));
    const QString name = info.fileName();
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) ||
        QFileInfo(info.filePath() + QStringLiteral("/.git")).isDir() ||
        !isAgentRoot(info.absolutePath()))
        return QString();
    return QDir(rootFolder(info.absolutePath())).filePath(name);
}

inline QStringList environment(const QString &folder)
{
    if (folder.isEmpty())
        return {};
    const QString tmp = folder + QStringLiteral("/tmp");
    if (!QDir().mkpath(tmp))
        return {};
    return {QStringLiteral("FORKMESH_AGENT_SCRATCH=") + folder,
            QStringLiteral("TMPDIR=") + tmp, QStringLiteral("TMP=") + tmp,
            QStringLiteral("TEMP=") + tmp};
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

inline QStringList orphans(const QString &worktreeRoot)
{
    QStringList found;
    if (!isAgentRoot(worktreeRoot))
        return found;
    const QDir root(rootFolder(worktreeRoot));
    for (const QString &name :
         root.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const QFileInfo worktree(QDir(worktreeRoot).filePath(name));
        if (!worktree.exists() && !worktree.isSymLink())
            found << root.filePath(name);
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

#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>

namespace forkmesh::worktreeinclude {

inline constexpr char kFileName[] = ".worktreeinclude";

struct Result {
    bool configured = false;
    int copied = 0;
    int existing = 0;
    QStringList errors;
};

inline QStringList splitNul(const QByteArray &bytes)
{
    QStringList out;
    for (const QByteArray &part : bytes.split('\0'))
        if (!part.isEmpty())
            out << QString::fromUtf8(part);
    return out;
}

inline bool safeRelative(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path))
        return false;
    for (const QString &part : path.split(QLatin1Char('/')))
        if (part == QLatin1String(".."))
            return false;
    return true;
}

inline QStringList copyList(const QStringList &matches, const QStringList &ignored)
{
    QSet<QString> ignoredFiles;
    QStringList ignoredDirs;
    for (const QString &entry : ignored) {
        if (entry.endsWith(QLatin1Char('/')))
            ignoredDirs << entry;
        else
            ignoredFiles.insert(entry);
    }
    QStringList out;
    QSet<QString> seen;
    for (const QString &path : matches) {
        if (!safeRelative(path) || path.endsWith(QLatin1Char('/')) ||
            path == QLatin1String(kFileName) || seen.contains(path))
            continue;
        bool hit = ignoredFiles.contains(path);
        for (int i = 0; !hit && i < ignoredDirs.size(); ++i)
            hit = path.startsWith(ignoredDirs.at(i));
        if (!hit)
            continue;
        seen.insert(path);
        out << path;
    }
    return out;
}

inline QByteArray runGit(const QString &repoRoot, const QStringList &args, bool *ok)
{
    QProcess git;
    git.setWorkingDirectory(repoRoot);
    git.start(QStringLiteral("git"), args);
    const bool finished = git.waitForFinished(60000);
    if (!finished)
        git.kill();
    *ok = finished && git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0;
    return git.readAllStandardOutput();
}

inline Result copyFiles(const QString &repoRoot, const QString &worktree,
                        const QStringList &files)
{
    Result result;
    result.configured = true;
    const QDir source(repoRoot);
    const QDir target(worktree);
    for (const QString &relative : files) {
        const QFileInfo from(source.filePath(relative));
        if (from.isSymLink() || !from.isFile())
            continue;
        const QString to = target.filePath(relative);
        if (QFileInfo::exists(to)) {
            ++result.existing;
            continue;
        }
        if (!QDir().mkpath(QFileInfo(to).absolutePath()) ||
            !QFile::copy(from.absoluteFilePath(), to)) {
            result.errors << relative;
            continue;
        }
        ++result.copied;
    }
    return result;
}

inline Result apply(const QString &repoRoot, const QString &worktree)
{
    const QString include = QDir(repoRoot).filePath(QLatin1String(kFileName));
    if (repoRoot.isEmpty() || worktree.isEmpty() || !QFileInfo(include).isFile())
        return {};
    bool matchesOk = false;
    const QStringList matches = splitNul(runGit(
        repoRoot,
        {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--others"),
         QStringLiteral("--ignored"), QStringLiteral("--exclude-from=") + include},
        &matchesOk));
    bool ignoredOk = false;
    const QStringList ignored = splitNul(runGit(
        repoRoot,
        {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--others"),
         QStringLiteral("--ignored"), QStringLiteral("--exclude-standard"),
         QStringLiteral("--directory")},
        &ignoredOk));
    if (!matchesOk || !ignoredOk) {
        Result failed;
        failed.configured = true;
        failed.errors << QStringLiteral("git ls-files failed");
        return failed;
    }
    return copyFiles(repoRoot, worktree, copyList(matches, ignored));
}

}

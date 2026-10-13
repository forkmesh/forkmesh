#include "ActionPushRange.h"

#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace forkmesh::actions {

namespace {

const QRegularExpression &objectIdPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    return pattern;
}

QStringList splitPaths(const QString &text)
{
    return text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

void appendUnique(QStringList &into, QSet<QString> &seen,
                  const QStringList &paths)
{
    for (const QString &path : paths) {
        if (!seen.contains(path)) {
            seen.insert(path);
            into.append(path);
        }
    }
}

QStringList tipParentsUnion(const PushRangeGit &git, const QString &after)
{
    QString parentsLine;
    if (!git({QStringLiteral("rev-list"), QStringLiteral("--parents"),
              QStringLiteral("-n"), QStringLiteral("1"), after,
              QStringLiteral("--")},
             &parentsLine))
        return {};
    QStringList parents =
        parentsLine.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!parents.isEmpty())
        parents.removeFirst();
    QStringList changed;
    QSet<QString> seen;
    if (parents.isEmpty()) {
        QString files;
        if (git({QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"),
                 QStringLiteral("--name-only"), QStringLiteral("-r"),
                 QStringLiteral("--root"), after},
                &files))
            appendUnique(changed, seen, splitPaths(files));
        return changed;
    }
    for (const QString &parent : std::as_const(parents)) {
        QString files;
        if (git({QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"),
                 QStringLiteral("--name-only"), QStringLiteral("-r"), parent,
                 after},
                &files))
            appendUnique(changed, seen, splitPaths(files));
    }
    return changed;
}

}

bool isKnownPushBase(const QString &before)
{
    const QString value = before.trimmed().toLower();
    return objectIdPattern().match(value).hasMatch() &&
           value.count(QLatin1Char('0')) != value.size();
}

bool isMetadataPath(const QString &path)
{
    return path.startsWith(QLatin1String(".forkmesh/issues/")) ||
           path.startsWith(QLatin1String(".forkmesh/pulls/")) ||
           path.startsWith(QLatin1String(".forkmesh/commits/"));
}

bool isMetadataOnlyChange(const QStringList &paths)
{
    return !paths.isEmpty() &&
           std::all_of(paths.cbegin(), paths.cend(), isMetadataPath);
}

QStringList pushRangeChangedPaths(const PushRangeGit &git,
                                  const QString &before,
                                  const QString &after)
{
    const QString tip = after.trimmed();
    if (tip.isEmpty() || tip.startsWith(QLatin1Char('-')))
        return {};
    const QString base = before.trimmed().toLower();
    if (isKnownPushBase(base) && base != tip.toLower() &&
        git({QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
             base, tip},
            nullptr)) {
        QString files;
        if (git({QStringLiteral("diff"), QStringLiteral("--no-ext-diff"),
                 QStringLiteral("--no-renames"), QStringLiteral("--name-only"),
                 base, tip, QStringLiteral("--")},
                &files))
            return splitPaths(files);
    }
    return tipParentsUnion(git, tip);
}

// code-health: allow(blocking-wait) called only from the runOffThread workflow scan
QStringList pushRangeChangedPaths(const QString &repoPath,
                                  const QString &before,
                                  const QString &after)
{
    const PushRangeGit git = [&repoPath](const QStringList &args,
                                         QString *out) {
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QStringLiteral("git"),
                      QStringList{QStringLiteral("-C"), repoPath} + args);
        if (!process.waitForStarted(3000) || !process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished(1000);
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0)
            return false;
        if (out)
            *out = QString::fromUtf8(process.readAllStandardOutput());
        return true;
    };
    return pushRangeChangedPaths(git, before, after);
}

}

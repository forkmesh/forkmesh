#include "GitRefusal.h"

#include <QSet>

namespace forkmesh::gitrefusal {

bool overLocalChanges(const QString &gitOutput)
{
    return gitOutput.contains(
               QLatin1String("Your local changes to the following files would be "
                             "overwritten"),
               Qt::CaseInsensitive)
           || gitOutput.contains(
               QLatin1String("untracked working tree files would be overwritten"),
               Qt::CaseInsensitive);
}

QStringList mergeSourceRefPatterns(const QString &branch)
{
    const QString name = branch.trimmed();
    if (name.isEmpty())
        return {};
    return {QStringLiteral("refs/heads/") + name,
            QStringLiteral("refs/remotes/*/") + name};
}

MergeSource mergeSourceFromRefs(const QString &branch,
                                const QString &forEachRefOutput)
{
    MergeSource source;
    const QString name = branch.trimmed();
    if (name.isEmpty() || name == QLatin1String("HEAD"))
        return source;
    const QString local = QStringLiteral("refs/heads/") + name;
    const QString remotePrefix = QStringLiteral("refs/remotes/");
    const QString remoteSuffix = QLatin1Char('/') + name;
    QSet<QString> remoteCommits;
    for (const QString &line :
         forEachRefOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts =
            line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 2)
            continue;
        const QString &ref = parts.at(0);
        const QString &commit = parts.at(1);
        if (ref == local) {
            source.kind = MergeSourceKind::Local;
            source.ref = ref;
            source.commit = commit;
            source.candidates.clear();
            return source;
        }
        if (!ref.startsWith(remotePrefix) || !ref.endsWith(remoteSuffix))
            continue;
        const QString remote =
            ref.mid(remotePrefix.size(),
                    ref.size() - remotePrefix.size() - remoteSuffix.size());
        if (remote.isEmpty() || remote.contains(QLatin1Char('/')))
            continue;
        source.candidates.append(ref.mid(remotePrefix.size()));
        remoteCommits.insert(commit);
        if (source.ref.isEmpty()) {
            source.ref = ref;
            source.commit = commit;
        }
    }
    if (remoteCommits.isEmpty()) {
        source.ref.clear();
        source.commit.clear();
        return source;
    }
    if (remoteCommits.size() > 1) {
        source.kind = MergeSourceKind::Ambiguous;
        source.ref.clear();
        source.commit.clear();
        return source;
    }
    source.kind = MergeSourceKind::RemoteTracking;
    return source;
}

QString firstFailureLine(const QString &gitOutput)
{
    QString fallback;
    for (const QString &raw : gitOutput.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("fatal:"))
            || line.startsWith(QLatin1String("error:")))
            return line;
        if (fallback.isEmpty() && !line.startsWith(QLatin1String("hint:"))
            && !line.startsWith(QLatin1String("From "))
            && !line.startsWith(QLatin1String("Fetching ")))
            fallback = line;
    }
    return fallback;
}

bool remoteUnavailable(const QString &gitOutput)
{
    static const QStringList needles{
        QStringLiteral("The requested URL returned error: 5"),
        QStringLiteral("Could not resolve host"),
        QStringLiteral("Failed to connect to"),
        QStringLiteral("Connection timed out"),
        QStringLiteral("Connection refused"),
        QStringLiteral("Operation timed out"),
        QStringLiteral("Could not read from remote repository"),
        QStringLiteral("the remote end hung up unexpectedly"),
        QStringLiteral("early EOF"),
        QStringLiteral("HTTP2 framing layer"),
        QStringLiteral("HTTP/2 stream"),
        QStringLiteral("transfer closed with outstanding read data"),
        QStringLiteral("unexpected disconnect while reading sideband packet"),
    };
    for (const QString &needle : needles)
        if (gitOutput.contains(needle, Qt::CaseInsensitive))
            return true;
    return false;
}

QString forkmeshRemoteMessage(const QString &gitOutput)
{
    static const QString help =
        QStringLiteral("remote: Help: https://forkmesh.com/docs/git-errors/");
    const QStringList lines = gitOutput.split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i) {
        if (!lines.at(i).trimmed().startsWith(help))
            continue;
        const QString message = lines.at(i - 1).trimmed();
        if (message.startsWith(QLatin1String("remote: ")))
            return message.mid(8).trimmed().left(240);
    }
    return {};
}

QString connectionFailure(const QString &gitOutput)
{
    struct Rule {
        QStringList needles;
        QString explanation;
    };
    static const QList<Rule> rules{
        {{QStringLiteral("HTTP2 framing layer"), QStringLiteral("HTTP/2 stream"),
          QStringLiteral("curl 16 "), QStringLiteral("curl 92 ")},
         QStringLiteral("The HTTPS connection broke partway through, often "
                        "because of a VPN, proxy or unstable network. Trying "
                        "again usually works.")},
        {{QStringLiteral("curl 18 "), QStringLiteral("curl 56 "),
          QStringLiteral("transfer closed with outstanding read data"),
          QStringLiteral("unexpected disconnect while reading sideband packet"),
          QStringLiteral("early EOF")},
         QStringLiteral("The connection dropped during the download. Trying "
                        "again usually works.")},
        {{QStringLiteral("Could not resolve host")},
         QStringLiteral("The server's name could not be looked up. Check the "
                        "network connection.")},
        {{QStringLiteral("Failed to connect to"),
          QStringLiteral("Connection refused"),
          QStringLiteral("Connection timed out"),
          QStringLiteral("Operation timed out")},
         QStringLiteral("The server could not be reached. Check the network "
                        "connection.")},
    };
    for (const Rule &rule : rules)
        for (const QString &needle : rule.needles)
            if (gitOutput.contains(needle, Qt::CaseInsensitive))
                return rule.explanation;
    return {};
}

QString explainFailure(const QString &gitOutput)
{
    const QString remote = forkmeshRemoteMessage(gitOutput);
    return remote.isEmpty() ? connectionFailure(gitOutput) : remote;
}

bool notFastForward(const QString &gitOutput)
{
    return gitOutput.contains(QLatin1String("Not possible to fast-forward"),
                              Qt::CaseInsensitive)
           || gitOutput.contains(QLatin1String("have diverged"),
                                 Qt::CaseInsensitive)
           || gitOutput.contains(QLatin1String("diverging branches"),
                                 Qt::CaseInsensitive);
}

QSet<QString> branchesCheckedOutInWorktrees(const QString &worktreeListPorcelain)
{
    QSet<QString> branches;
    const QString prefix = QStringLiteral("branch refs/heads/");
    for (const QString &raw : worktreeListPorcelain.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(prefix) && line.size() > prefix.size())
            branches.insert(line.mid(prefix.size()));
    }
    return branches;
}

}

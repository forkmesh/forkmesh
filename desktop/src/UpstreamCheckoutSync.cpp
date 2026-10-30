#include "UpstreamCheckoutSync.h"

#include "LocalControlHandoff.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

namespace forkmesh::upstream {

namespace {

constexpr int kPlumbingTimeoutMs = 30 * 1000;
constexpr int kFetchTimeoutMs = 10 * 60 * 1000;
const QString kGeneratedStatsPath =
    QStringLiteral(".forkmesh/stats/repository.json");

bool runGit(const QString &dir, const QStringList &args, QString *output,
            int timeoutMs = kPlumbingTimeoutMs)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                       QStringLiteral("0"));
    if (!environment.contains(QStringLiteral("GIT_SSH_COMMAND")))
        environment.insert(
            QStringLiteral("GIT_SSH_COMMAND"),
            QStringLiteral("ssh -oBatchMode=yes -oConnectTimeout=15 "
                           "-oServerAliveInterval=15 -oServerAliveCountMax=2"));
    process.setProcessEnvironment(environment);
    QStringList boundedArgs{
        QStringLiteral("-c"), QStringLiteral("pack.threads=1"),
        QStringLiteral("-c"), QStringLiteral("core.deltaBaseCacheLimit=16m"),
        QStringLiteral("-c"), QStringLiteral("pack.deltaCacheSize=16m"),
        QStringLiteral("-c"), QStringLiteral("pack.windowMemory=16m")};
    boundedArgs.append(args);
    process.start(QStringLiteral("git"), boundedArgs);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput());
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

// code-health: allow(blocking-wait) only reached via refreshCore, whose callers run on QThread::create workers in MainWindowRepos
bool runGitWithEnvironment(const QString &dir, const QStringList &args,
                           const QProcessEnvironment &environment,
                           QString *output,
                           int timeoutMs = kPlumbingTimeoutMs)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProcessEnvironment(environment);
    QStringList boundedArgs{
        QStringLiteral("-c"), QStringLiteral("pack.threads=1"),
        QStringLiteral("-c"), QStringLiteral("core.deltaBaseCacheLimit=16m"),
        QStringLiteral("-c"), QStringLiteral("pack.deltaCacheSize=16m"),
        QStringLiteral("-c"), QStringLiteral("pack.windowMemory=16m")};
    boundedArgs.append(args);
    process.start(QStringLiteral("git"), boundedArgs);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput());
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

bool containedIn(const QString &repo, const QString &tip,
                 const QString &target)
{
    return runGit(repo,
                  {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"),
                   tip, target},
                  nullptr);
}

bool worktreeClean(const QString &worktreePath)
{
    QString status;
    if (!runGit(worktreePath,
                {QStringLiteral("status"), QStringLiteral("--porcelain"),
                 QStringLiteral("--untracked-files=no")},
                &status))
        return false;
    return status.trimmed().isEmpty();
}

bool onlyGeneratedStatsDirty(const QString &worktreePath)
{
    QString status;
    if (!runGit(worktreePath,
                {QStringLiteral("status"), QStringLiteral("--porcelain"),
                 QStringLiteral("--untracked-files=no")},
                &status))
        return false;
    const QStringList lines =
        status.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return false;
    for (const QString &line : lines) {
        if (line.size() < 4 || line.mid(3) != kGeneratedStatsPath)
            return false;
    }
    return true;
}

QHash<QString, QString> checkedOutBranches(const QString &repo)
{
    QHash<QString, QString> result;
    QString listing;
    if (!runGit(repo,
                {QStringLiteral("worktree"), QStringLiteral("list"),
                 QStringLiteral("--porcelain")},
                &listing))
        return result;
    QString currentPath;
    const QStringList lines = listing.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("worktree ")))
            currentPath = line.mid(9).trimmed();
        else if (line.startsWith(QLatin1String("branch refs/heads/")) &&
                 !currentPath.isEmpty())
            result.insert(line.mid(18).trimmed(), currentPath);
    }
    return result;
}

QHash<QString, QString> refTips(const QString &repo, const QString &prefix,
                                int strip)
{
    QHash<QString, QString> result;
    QString listing;
    if (!runGit(repo,
                {QStringLiteral("for-each-ref"), prefix,
                 QStringLiteral("--format=%(refname:lstrip=") +
                     QString::number(strip) + QStringLiteral(") %(objectname)")},
                &listing))
        return result;
    const QStringList lines = listing.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int space = line.lastIndexOf(QLatin1Char(' '));
        if (space <= 0)
            continue;
        const QString name = line.left(space).trimmed();
        if (name.isEmpty() || name == QLatin1String("HEAD"))
            continue;
        result.insert(name, line.mid(space + 1).trimmed());
    }
    return result;
}

bool sideChangesOnlyCollaboration(const QString &repo, const QString &base,
                                  const QString &side)
{
    QString changes;
    if (!runGit(repo,
                {QStringLiteral("diff"), QStringLiteral("--name-status"),
                 QStringLiteral("--no-renames"), QStringLiteral("-z"), base,
                 side, QStringLiteral("--")},
                &changes))
        return false;
    const QStringList fields = changes.split(QChar(u'\0'),
                                              Qt::SkipEmptyParts);
    if (fields.isEmpty() || fields.size() % 2 != 0 || fields.size() > 1024)
        return false;
    static const QRegularExpression collaborationPath(
        QStringLiteral(
            "^\\.forkmesh/(?:issues/(?:open|closed)/[1-9][0-9]{0,8}/|"
            "discussions/[1-9][0-9]{0,8}/|pulls/[1-9][0-9]{0,8}/)"));
    for (int i = 0; i < fields.size(); i += 2) {
        const QString status = fields.at(i);
        const QString path = fields.at(i + 1);
        if ((status != QLatin1String("A") && status != QLatin1String("M") &&
             status != QLatin1String("D")) ||
            !collaborationPath.match(path).hasMatch())
            return false;
    }
    return true;
}

qint64 commitTime(const QString &repo, const QString &commit)
{
    QString raw;
    if (!runGit(repo,
                {QStringLiteral("show"), QStringLiteral("-s"),
                 QStringLiteral("--format=%ct"), commit},
                &raw))
        return 0;
    return raw.trimmed().toLongLong();
}

struct TreeEntry {
    QString mode;
    QString oid;

    bool operator==(const TreeEntry &other) const
    {
        return mode == other.mode && oid == other.oid;
    }
};

TreeEntry treeEntry(const QString &repo, const QString &tree,
                    const QString &path)
{
    QString raw;
    if (!runGit(repo,
                {QStringLiteral("ls-tree"), QStringLiteral("-z"), tree,
                 QStringLiteral("--"), path},
                &raw))
        return {};
    const int tab = raw.indexOf(QLatin1Char('\t'));
    if (tab <= 0)
        return {};
    const QStringList header =
        raw.left(tab).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (header.size() != 3 || header.at(1) != QLatin1String("blob"))
        return {};
    return {header.at(0), header.at(2)};
}

QString issueRecordStatus(const QString &path)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^\\.forkmesh/issues/(open|closed)/([1-9][0-9]{0,8})/issue-\\2\\.json$"));
    return pattern.match(path).captured(1);
}

bool placeholderOpenEvent(const QJsonObject &event)
{
    const QString sig = event.value(QStringLiteral("sig")).toString();
    return sig.isEmpty() || sig == QLatin1String("relay-placeholder");
}

qint64 issueEventTs(const QJsonObject &event)
{
    return qint64(event.value(QStringLiteral("ts")).toDouble());
}

QString issueEventKey(const QJsonObject &event)
{
    const QString id = event.value(QStringLiteral("id")).toString();
    return id.isEmpty()
               ? QString::fromUtf8(
                     QJsonDocument(event).toJson(QJsonDocument::Compact))
               : id;
}

QString issueProjectionKey(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("claim"))
        return QStringLiteral("claim:") +
               event.value(QStringLiteral("assignee"))
                   .toString()
                   .trimmed()
                   .toLower();
    static const QSet<QString> projected{
        QStringLiteral("title"),     QStringLiteral("status"),
        QStringLiteral("labels"),    QStringLiteral("milestone"),
        QStringLiteral("dates"),     QStringLiteral("priority"),
        QStringLiteral("progress"),  QStringLiteral("assignees")};
    return projected.contains(type) ? type : QString();
}

void applyIssueProjection(QJsonObject &issue, const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("dates")) {
        issue.insert(QStringLiteral("startDate"),
                     event.value(QStringLiteral("startDate")));
        issue.insert(QStringLiteral("endDate"),
                     event.value(QStringLiteral("endDate")));
    } else if (type == QLatin1String("claim")) {
        const QString name =
            event.value(QStringLiteral("assignee")).toString().trimmed();
        if (name.isEmpty())
            return;
        QJsonArray assignees;
        bool present = false;
        for (const QJsonValue &value :
             issue.value(QStringLiteral("assignees")).toArray()) {
            const bool same = value.toString().trimmed().compare(
                                  name, Qt::CaseInsensitive) == 0;
            present = present || same;
            if (!same || event.value(QStringLiteral("claimState")).toString() !=
                             QLatin1String("released"))
                assignees.append(value);
        }
        if (!present && event.value(QStringLiteral("claimState")).toString() ==
                            QLatin1String("assigned"))
            assignees.append(name);
        issue.insert(QStringLiteral("assignees"), assignees);
    } else if (!issueProjectionKey(event).isEmpty()) {
        issue.insert(type, event.value(type));
    }
}

QByteArray mergeIssueRecords(const QByteArray &sourceBytes,
                             const QByteArray &peerBytes,
                             const QString &directoryStatus)
{
    QJsonParseError sourceError;
    QJsonParseError peerError;
    const QJsonDocument sourceDocument =
        QJsonDocument::fromJson(sourceBytes, &sourceError);
    const QJsonDocument peerDocument =
        QJsonDocument::fromJson(peerBytes, &peerError);
    if (sourceError.error != QJsonParseError::NoError ||
        peerError.error != QJsonParseError::NoError ||
        !sourceDocument.isObject() || !peerDocument.isObject())
        return {};
    const QJsonObject source = sourceDocument.object();
    const QJsonObject peer = peerDocument.object();
    const int number = source.value(QStringLiteral("number")).toInt();
    if (number <= 0 || peer.value(QStringLiteral("number")).toInt() != number)
        return {};

    QJsonObject merged = source;
    QJsonArray events;
    QSet<QString> seen;
    QHash<QString, qint64> latestProjection;
    int sourceOpen = -1;
    for (const QJsonValue &value : source.value(QStringLiteral("events")).toArray()) {
        if (!value.isObject())
            return {};
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("type")).toString() == QLatin1String("open") &&
            sourceOpen < 0)
            sourceOpen = int(events.size());
        seen.insert(issueEventKey(event));
        const QString projection = issueProjectionKey(event);
        if (!projection.isEmpty())
            latestProjection[projection] =
                qMax(latestProjection.value(projection), issueEventTs(event));
        events.append(event);
    }

    QList<QJsonObject> additions;
    QJsonObject peerOpen;
    for (const QJsonValue &value : peer.value(QStringLiteral("events")).toArray()) {
        if (!value.isObject())
            return {};
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("type")).toString() == QLatin1String("open")) {
            if (peerOpen.isEmpty())
                peerOpen = event;
            continue;
        }
        if (!seen.contains(issueEventKey(event)))
            additions.append(event);
    }
    std::stable_sort(additions.begin(), additions.end(),
                     [](const QJsonObject &a, const QJsonObject &b) {
                         const qint64 left = issueEventTs(a);
                         const qint64 right = issueEventTs(b);
                         if (left != right)
                             return left < right;
                         return issueEventKey(a) < issueEventKey(b);
                     });

    if (!peerOpen.isEmpty() && !placeholderOpenEvent(peerOpen) &&
        (sourceOpen < 0 ||
         placeholderOpenEvent(events.at(sourceOpen).toObject()))) {
        if (sourceOpen < 0)
            events.insert(0, peerOpen);
        else
            events.replace(sourceOpen, peerOpen);
        merged.insert(QStringLiteral("createdAt"),
                      double(issueEventTs(peerOpen)));
        merged.insert(QStringLiteral("author"),
                      peerOpen.value(QStringLiteral("author")));
        merged.insert(QStringLiteral("authorName"),
                      peerOpen.value(QStringLiteral("authorName")));
        if (!latestProjection.contains(QStringLiteral("title")))
            merged.insert(QStringLiteral("title"),
                          peerOpen.value(QStringLiteral("title")));
    } else if (sourceOpen < 0) {
        return {};
    }

    const qint64 latestAssignees =
        latestProjection.value(QStringLiteral("assignees"));
    for (const QJsonObject &event : additions) {
        events.append(event);
        const QString projection = issueProjectionKey(event);
        const qint64 ts = issueEventTs(event);
        if (projection.isEmpty() || ts < latestProjection.value(projection))
            continue;
        if (projection.startsWith(QLatin1String("claim:")) &&
            ts < latestAssignees)
            continue;
        latestProjection[projection] = ts;
        applyIssueProjection(merged, event);
    }

    if (merged.value(QStringLiteral("title")).toString().isEmpty())
        merged.insert(QStringLiteral("title"),
                      peer.value(QStringLiteral("title")));
    if (merged.value(QStringLiteral("status")).toString(QStringLiteral("open")) !=
        directoryStatus)
        return {};
    qint64 updatedAt =
        qMax(qint64(source.value(QStringLiteral("updatedAt")).toDouble()),
             qint64(peer.value(QStringLiteral("updatedAt")).toDouble()));
    int votes = 0;
    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        updatedAt = qMax(updatedAt, issueEventTs(event));
        if (event.value(QStringLiteral("type")).toString() == QLatin1String("vote"))
            ++votes;
    }
    merged.insert(QStringLiteral("updatedAt"), double(updatedAt));
    merged.insert(QStringLiteral("votes"), votes);
    merged.insert(QStringLiteral("events"), events);
    return QJsonDocument(merged).toJson(QJsonDocument::Indented);
}

QString mergedIssueRecordBlob(const QString &repo, const QString &path,
                              const QString &sourceOid,
                              const QString &peerOid,
                              const QString &scratchDirectory)
{
    const QString status = issueRecordStatus(path);
    if (status.isEmpty())
        return {};
    QString sourceBytes;
    QString peerBytes;
    if (!runGit(repo,
                {QStringLiteral("cat-file"), QStringLiteral("blob"), sourceOid},
                &sourceBytes) ||
        !runGit(repo,
                {QStringLiteral("cat-file"), QStringLiteral("blob"), peerOid},
                &peerBytes))
        return {};
    const QByteArray merged =
        mergeIssueRecords(sourceBytes.toUtf8(), peerBytes.toUtf8(), status);
    if (merged.isEmpty())
        return {};
    const QString scratch =
        QDir(scratchDirectory).filePath(QStringLiteral("issue-record.json"));
    QFile file(scratch);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(merged) != merged.size())
        return {};
    file.close();
    QString oid;
    if (!runGit(repo,
                {QStringLiteral("hash-object"), QStringLiteral("-w"), scratch},
                &oid))
        return {};
    return oid.trimmed();
}

QString mergeTreeWithIssueRecords(const QString &repo, const QString &left,
                                  const QString &right)
{
    QString raw;
    const bool clean = runGit(repo,
                              {QStringLiteral("merge-tree"),
                               QStringLiteral("--write-tree"),
                               QStringLiteral("--name-only"),
                               QStringLiteral("--no-messages"), left, right},
                              &raw);
    QStringList lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return {};
    const QString conflictedTree = lines.takeFirst().trimmed();
    if (clean)
        return conflictedTree;
    static const QRegularExpression objectId(
        QStringLiteral("^[0-9a-f]{40}(?:[0-9a-f]{24})?$"));
    lines.removeDuplicates();
    if (!objectId.match(conflictedTree).hasMatch() || lines.isEmpty() ||
        lines.size() > 512)
        return {};

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return {};
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_INDEX_FILE"),
                       QDir(temporary.path()).filePath(QStringLiteral("index")));
    if (!runGitWithEnvironment(repo,
                               {QStringLiteral("read-tree"), conflictedTree},
                               environment, nullptr))
        return {};
    for (const QString &path : std::as_const(lines)) {
        const TreeEntry leftEntry = treeEntry(repo, left, path);
        const TreeEntry rightEntry = treeEntry(repo, right, path);
        if (leftEntry.mode != QLatin1String("100644") ||
            rightEntry.mode != QLatin1String("100644"))
            return {};
        const QString oid = mergedIssueRecordBlob(
            repo, path, leftEntry.oid, rightEntry.oid, temporary.path());
        if (!objectId.match(oid).hasMatch() ||
            !runGitWithEnvironment(
                repo,
                {QStringLiteral("update-index"), QStringLiteral("--add"),
                 QStringLiteral("--cacheinfo"), leftEntry.mode, oid, path},
                environment, nullptr))
            return {};
    }
    QString tree;
    if (!runGitWithEnvironment(repo, {QStringLiteral("write-tree")},
                               environment, &tree))
        return {};
    return tree.trimmed();
}

// Build a tree that keeps the source side byte-for-byte except for clean
// three-way changes under the Git-authoritative collaboration stores. This is
// the recovery path for a stale mirror lineage that contains both ordinary
// code and later issue intake: the code remains source-owned, while unique
// issues/discussions/pull records are not marooned or discarded.
QString selectiveCollaborationTree(const QString &repo, const QString &base,
                                   const QString &source,
                                   const QString &peer)
{
    QString changes;
    if (!runGit(repo,
                {QStringLiteral("diff"), QStringLiteral("--name-status"),
                 QStringLiteral("--no-renames"), QStringLiteral("-z"), base,
                 peer, QStringLiteral("--"),
                 QStringLiteral(".forkmesh/issues"),
                 QStringLiteral(".forkmesh/discussions"),
                 QStringLiteral(".forkmesh/pulls")},
                &changes))
        return {};
    const QStringList fields =
        changes.split(QChar(u'\0'), Qt::SkipEmptyParts);
    if (fields.isEmpty() || fields.size() % 2 != 0 || fields.size() > 1024)
        return {};

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return {};
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_INDEX_FILE"),
                       QDir(temporary.path()).filePath(QStringLiteral("index")));
    if (!runGitWithEnvironment(repo,
                               {QStringLiteral("read-tree"), source},
                               environment, nullptr))
        return {};

    for (int i = 0; i < fields.size(); i += 2) {
        const QString status = fields.at(i);
        const QString path = fields.at(i + 1);
        if (status != QLatin1String("A") && status != QLatin1String("M") &&
            status != QLatin1String("D"))
            return {};
        const TreeEntry oldEntry = treeEntry(repo, base, path);
        const TreeEntry peerEntry = treeEntry(repo, peer, path);
        const TreeEntry sourceEntry = treeEntry(repo, source, path);
        if (sourceEntry == peerEntry)
            continue;
        if (!(sourceEntry == oldEntry)) {
            if (sourceEntry.mode != QLatin1String("100644") ||
                peerEntry.mode != QLatin1String("100644"))
                return {}; // both sides changed this record; preserve the source
            const QString merged = mergedIssueRecordBlob(
                repo, path, sourceEntry.oid, peerEntry.oid, temporary.path());
            if (merged.isEmpty() ||
                !runGitWithEnvironment(
                    repo,
                    {QStringLiteral("update-index"), QStringLiteral("--add"),
                     QStringLiteral("--cacheinfo"), sourceEntry.mode, merged,
                     path},
                    environment, nullptr))
                return {};
            continue;
        }
        if (peerEntry.oid.isEmpty()) {
            if (!runGitWithEnvironment(
                    repo,
                    {QStringLiteral("update-index"),
                     QStringLiteral("--force-remove"), QStringLiteral("--"),
                     path},
                    environment, nullptr))
                return {};
        } else if (!runGitWithEnvironment(
                       repo,
                       {QStringLiteral("update-index"), QStringLiteral("--add"),
                        QStringLiteral("--cacheinfo"), peerEntry.mode,
                        peerEntry.oid, path},
                       environment, nullptr)) {
            return {};
        }
    }

    QString tree;
    if (!runGitWithEnvironment(repo, {QStringLiteral("write-tree")},
                               environment, &tree))
        return {};
    return tree.trimmed();
}

// Return a synthetic merge commit only for a clean collaboration-only
// divergence. Issues, discussions, and the pull ledger are Git-authoritative
// stores and can be written by different intake winners while the owner is
// offline. Ordinary source-code divergence remains deliberately untouched.
// merge-tree performs the content merge without touching the index or working
// copy; the fixed identity/time makes independently healed owners converge on
// the same object when they see the same two parents.
QString reconcileCollaborationDivergence(const QString &repo,
                                         const QString &left,
                                         const QString &right)
{
    QString basesRaw;
    if (!runGit(repo,
                {QStringLiteral("merge-base"), QStringLiteral("--all"), left,
                 right},
                &basesRaw))
        return {};
    const QStringList bases =
        basesRaw.split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
    if (bases.isEmpty())
        return {};

    // Several merge bases are the NORMAL shape once a mirror has healed its
    // own divergence: its reconciler merges two intake lineages the source
    // already holds separately, so the two sides share both of them. Bailing
    // out on that (the old `bases.size() != 1`) deadlocked convergence
    // outright — the source could not fast-forward to the mirror, the
    // fast-forward-only fan-out could not move the mirror to the source, and
    // the whole fleet froze at the mirror's commit until someone merged it by
    // hand. A side is safe to absorb when it changes only collaboration paths
    // relative to EVERY base, which is base-count independent.
    bool collaborationOnly = true;
    for (const QString &base : bases) {
        if (!sideChangesOnlyCollaboration(repo, base, right)) {
            collaborationOnly = false;
            break;
        }
    }

    static const QRegularExpression objectId(
        QStringLiteral("^[0-9a-f]{40}(?:[0-9a-f]{24})?$"));
    QString leftTree;
    QString rightTree;
    if (!runGit(repo,
                {QStringLiteral("rev-parse"), left + QStringLiteral("^{tree}")},
                &leftTree) ||
        !runGit(repo,
                {QStringLiteral("rev-parse"), right + QStringLiteral("^{tree}")},
                &rightTree))
        return {};
    leftTree = leftTree.trimmed();
    rightTree = rightTree.trimmed();

    QString tree;
    if (leftTree == rightTree) {
        // Rewritten histories can carry identical content under different
        // commits. Bridge their ancestry so future source pushes become an
        // ordinary fast-forward instead of staying amber forever.
        tree = leftTree;
    } else if (collaborationOnly) {
        // merge-tree --write-tree builds its own virtual base when the sides
        // have more than one merge base, so this arm stays correct for the
        // multi-base shape above.
        tree = mergeTreeWithIssueRecords(repo, left, right);
    } else if (bases.size() == 1) {
        tree = selectiveCollaborationTree(repo, bases.first(), left, right);
    } else {
        // A multi-base side that also carries source-code changes is not
        // something a single base can describe; leave it for the operator.
        return {};
    }
    if (!objectId.match(tree).hasMatch())
        return {};

    QStringList parents{left, right};
    std::sort(parents.begin(), parents.end());
    const qint64 timestamp =
        qMax(commitTime(repo, parents.at(0)), commitTime(repo, parents.at(1))) +
        1;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"),
                       QStringLiteral("ForkMesh Collaboration Reconciler"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral(
                           "collaboration-reconciler@users.noreply.forkmesh.com"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                       QStringLiteral("ForkMesh Collaboration Reconciler"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral(
                           "collaboration-reconciler@users.noreply.forkmesh.com"));
    const QString date = QStringLiteral("@%1 +0000").arg(timestamp);
    environment.insert(QStringLiteral("GIT_AUTHOR_DATE"), date);
    environment.insert(QStringLiteral("GIT_COMMITTER_DATE"), date);
    QString commitRaw;
    if (!runGitWithEnvironment(
            repo,
            {QStringLiteral("commit-tree"), tree, QStringLiteral("-p"),
             parents.at(0), QStringLiteral("-p"), parents.at(1),
             QStringLiteral("-m"),
             QStringLiteral("Merge divergent mirror collaboration intake")},
            environment, &commitRaw))
        return {};
    const QString commit = commitRaw.trimmed();
    return objectId.match(commit).hasMatch() ? commit : QString();
}

} // namespace

bool configureForkCheckoutRemote(const QString &checkoutPath,
                                 const QString &sourceUrl,
                                 const QString &forkMirrorPath,
                                 QString *error)
{
    if (error)
        error->clear();
    const QString path = checkoutPath.trimmed();
    const QString source = sourceUrl.trimmed();
    const QString mirror = forkMirrorPath.trimmed();
    if (path.isEmpty() || source.isEmpty() || mirror.isEmpty() ||
        !QFileInfo(QDir(path).filePath(QStringLiteral(".git"))).exists()) {
        if (error)
            *error = QStringLiteral(
                "the fork checkout or one of its remotes is missing");
        return false;
    }
    if (!runGit(path,
                {QStringLiteral("remote"), QStringLiteral("set-url"),
                 QStringLiteral("origin"), source},
                nullptr)) {
        if (error)
            *error = QStringLiteral("could not set origin's fetch URL");
        return false;
    }
    if (!runGit(path,
                {QStringLiteral("remote"), QStringLiteral("set-url"),
                 QStringLiteral("--push"), QStringLiteral("origin"), mirror},
                nullptr)) {
        if (error)
            *error = QStringLiteral("could not set origin's push URL");
        return false;
    }
    return true;
}

QString RefreshOutcome::summary() const
{
    if (!error.isEmpty())
        return QStringLiteral("upstream refresh failed: %1").arg(error);
    if (!headFastForwarded && branchesUpdated == 0 && branchesPruned == 0)
        return QString();
    QStringList parts;
    if (headFastForwarded)
        parts << QStringLiteral("fast-forwarded the checkout");
    if (branchesUpdated > 0)
        parts << QStringLiteral("%1 branch(es) updated").arg(branchesUpdated);
    if (branchesPruned > 0)
        parts << QStringLiteral("%1 pruned").arg(branchesPruned);
    if (branchesKept > 0)
        parts << QStringLiteral("%1 diverged kept").arg(branchesKept);
    return QStringLiteral("upstream refresh: ") +
           parts.join(QStringLiteral(", "));
}

QString deletedBranchTombstoneRef(const QString &branch)
{
    return QStringLiteral("refs/forkmesh/trash/deleted/") + branch;
}

QString restoreDeletedBranch(const QString &checkoutPath,
                             const QString &branch)
{
    const QString name = branch.trimmed();
    if (checkoutPath.trimmed().isEmpty() || name.isEmpty())
        return QString();
    if (runGit(checkoutPath,
               {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                QStringLiteral("--quiet"), QStringLiteral("refs/heads/") + name},
               nullptr))
        return QString();
    const QString tombstone = deletedBranchTombstoneRef(name);
    QString tip;
    if (!runGit(checkoutPath,
                {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                 QStringLiteral("--quiet"), tombstone + QStringLiteral("^{commit}")},
                &tip))
        return QString();
    tip = tip.trimmed();
    if (tip.isEmpty() ||
        !runGit(checkoutPath, {QStringLiteral("branch"), name, tip}, nullptr))
        return QString();
    runGit(checkoutPath,
           {QStringLiteral("update-ref"), QStringLiteral("-d"), tombstone, tip},
           nullptr);
    return tip;
}

QByteArray branchTrashRequestCanonical(const QString &owner,
                                       const QString &repo,
                                       const QString &timestampMs)
{
    return QStringList{QStringLiteral("forkmesh-branch-trash-v1"),
                       owner.trimmed().toLower(), repo.trimmed(), timestampMs}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

bool parseServedBranchTrash(const QByteArray &body, const QString &owner,
                            const QString &repo,
                            QHash<QString, QStringList> *deleted)
{
    static const QRegularExpression branchName(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$"));
    static const QRegularExpression objectId(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    constexpr int kMaxEntries = 5000;
    if (deleted)
        deleted->clear();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject object = document.object();
    const QJsonValue entries = object.value(QStringLiteral("entries"));
    if (object.value(QStringLiteral("ok")).toBool() != true ||
        object.value(QStringLiteral("owner")).toString() !=
            owner.trimmed().toLower() ||
        object.value(QStringLiteral("repo")).toString() != repo.trimmed() ||
        !entries.isArray() || entries.toArray().size() > kMaxEntries ||
        object.value(QStringLiteral("cores")).toArray().isEmpty())
        return false;
    QHash<QString, QStringList> parsed;
    for (const QJsonValue &value : entries.toArray()) {
        const QJsonObject entry = value.toObject();
        const QString branch = entry.value(QStringLiteral("branch")).toString();
        const QString oid = entry.value(QStringLiteral("oid")).toString();
        if (!branchName.match(branch).hasMatch() ||
            branch.contains(QStringLiteral("..")) ||
            branch.contains(QStringLiteral("//")) ||
            branch.endsWith(QLatin1Char('.')) ||
            branch.endsWith(QLatin1Char('/')) ||
            branch.endsWith(QStringLiteral(".lock")) ||
            !objectId.match(oid).hasMatch())
            return false;
        if (!parsed[branch].contains(oid))
            parsed[branch].append(oid);
    }
    if (deleted)
        *deleted = parsed;
    return true;
}

static bool coveredByDeletion(const QString &path, const QString &branch,
                              const QString &tip)
{
    QString deletedTip;
    if (!runGit(path,
                {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                 QStringLiteral("--quiet"), deletedBranchTombstoneRef(branch)},
                &deletedTip))
        return false;
    return containedIn(path, tip, deletedTip.trimmed());
}

static void keepReplacedTip(const QString &path, const QString &branch,
                            const QString &tip)
{
    if (tip.isEmpty())
        return;
    runGit(path,
           {QStringLiteral("update-ref"),
            QStringLiteral("refs/forkmesh/trash/sync/") + branch, tip},
           nullptr);
}

static QStringList upstreamTrashRefspecs(const QString &trashNs)
{
    return {QStringLiteral("+refs/forkmesh/trash/branch/*:") + trashNs +
                QStringLiteral("/branch/*"),
            QStringLiteral("+refs/forkmesh/trash/agent-branch/*:") + trashNs +
                QStringLiteral("/agent-branch/*")};
}

static QHash<QString, QStringList> upstreamTrashTips(const QString &path,
                                                     const QString &trashNs)
{
    static const QRegularExpression token(QStringLiteral("^[0-9a-f]{16}$"));
    QHash<QString, QStringList> result;
    for (const QString &kind :
         {QStringLiteral("/branch"), QStringLiteral("/agent-branch")}) {
        const QString prefix = trashNs + kind;
        const QHash<QString, QString> tips =
            refTips(path, prefix, int(prefix.count(QLatin1Char('/'))) + 1);
        for (auto it = tips.constBegin(); it != tips.constEnd(); ++it) {
            const int slash = it.key().lastIndexOf(QLatin1Char('/'));
            if (slash <= 0 || !token.match(it.key().mid(slash + 1)).hasMatch())
                continue;
            result[it.key().left(slash)].append(it.value());
        }
    }
    return result;
}

static bool recordUpstreamDeletion(const QString &path, const QString &branch,
                                   const QString &tip)
{
    const QString tombstone = deletedBranchTombstoneRef(branch);
    QString existing;
    if (runGit(path,
               {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                QStringLiteral("--quiet"), tombstone},
               &existing)) {
        existing = existing.trimmed();
        if (existing == tip || containedIn(path, tip, existing))
            return true;
        if (!containedIn(path, existing, tip))
            keepReplacedTip(path, branch, existing);
    }
    return runGit(path, {QStringLiteral("update-ref"), tombstone, tip},
                  nullptr);
}

static RefreshOutcome refreshCore(const QString &checkoutPath,
                                  const QString &upstreamUrl,
                                  const QStringList &forcedBranches,
                                  bool repointOrigin, bool pruneGone,
                                  bool recoverGeneratedStats,
                                  bool reconcileCollaboration = false,
                                  const QProcessEnvironment &fetchEnvironment = {},
                                  const QHash<QString, QStringList> &servedTrash = {})
{
    RefreshOutcome outcome;
    const QString path = checkoutPath.trimmed();
    const QString url = upstreamUrl.trimmed();
    if (path.isEmpty() || url.isEmpty() ||
        !QFileInfo(QDir(path).filePath(QStringLiteral(".git"))).exists()) {
        outcome.error = QStringLiteral("no managed checkout to refresh");
        return outcome;
    }

    const QString remoteNs = repointOrigin
                                 ? QStringLiteral("refs/remotes/origin")
                                 : QStringLiteral("refs/remotes/forkmesh-mesh");
    const QString trashNs =
        QStringLiteral("refs/forkmesh/upstream-trash/") +
        (repointOrigin ? QStringLiteral("origin")
                       : QStringLiteral("forkmesh-mesh"));
    const QHash<QString, QString> seenUpstream = refTips(path, remoteNs, 3);
    if (repointOrigin) {
        if (!runGit(path,
                    {QStringLiteral("remote"), QStringLiteral("set-url"),
                     QStringLiteral("origin"), url},
                    nullptr))
            runGit(path,
                   {QStringLiteral("remote"), QStringLiteral("add"),
                    QStringLiteral("origin"), url},
                   nullptr);
        runGit(path,
               {QStringLiteral("config"), QStringLiteral("remote.origin.fetch"),
                QStringLiteral("+refs/heads/*:refs/remotes/origin/*")},
               nullptr);
        if (!runGit(path,
                    QStringList{QStringLiteral("fetch"),
                                QStringLiteral("--prune"),
                                QStringLiteral("--prune-tags"),
                                QStringLiteral("--tags"),
                                QStringLiteral("--force"),
                                QStringLiteral("--quiet"),
                                QStringLiteral("origin"),
                                QStringLiteral(
                                    "+refs/heads/*:refs/remotes/origin/*")} +
                        upstreamTrashRefspecs(trashNs),
                    nullptr, kFetchTimeoutMs)) {
            outcome.error = QStringLiteral("fetch from the relay failed");
            return outcome;
        }
    } else {
        QStringList fetchArgs{QStringLiteral("fetch"),
                              QStringLiteral("--prune"),
                              QStringLiteral("--quiet")};
        if (QUrl(url).scheme().compare(QStringLiteral("ssh"),
                                      Qt::CaseInsensitive) == 0) {
            // Managed remotes log in as the provisioner account; repository
            // bytes are owned by the unprivileged mirror service account.
            fetchArgs << QStringLiteral(
                "--upload-pack=runuser -u forkmesh-node -- git-upload-pack");
        }
        fetchArgs << url
                  << QStringLiteral("+refs/heads/*:") + remoteNs +
                         QStringLiteral("/*")
                  << upstreamTrashRefspecs(trashNs);
        const bool fetched = fetchEnvironment.isEmpty()
                                 ? runGit(path, fetchArgs, nullptr,
                                          kFetchTimeoutMs)
                                 : runGitWithEnvironment(
                                       path, fetchArgs, fetchEnvironment,
                                       nullptr, kFetchTimeoutMs);
        if (!fetched) {
            outcome.error = QStringLiteral("fetch from the mesh failed");
            return outcome;
        }
    }
    outcome.fetchOk = true;

    const QHash<QString, QString> upstream = refTips(path, remoteNs, 3);
    if (upstream.isEmpty()) {
        outcome.error = QStringLiteral("upstream advertised no branches");
        return outcome;
    }
    const QHash<QString, QString> local =
        refTips(path, QStringLiteral("refs/heads"), 2);
    const QString localMain = !local.value(QStringLiteral("main")).isEmpty()
                                  ? local.value(QStringLiteral("main"))
                                  : local.value(QStringLiteral("master"));
    const QHash<QString, QString> checkedOut = checkedOutBranches(path);
    const QSet<QString> forced(forcedBranches.begin(), forcedBranches.end());
    QString headBranch;
    runGit(path,
           {QStringLiteral("symbolic-ref"), QStringLiteral("--short"),
            QStringLiteral("HEAD")},
           &headBranch);
    headBranch = headBranch.trimmed();

    for (auto it = upstream.constBegin(); it != upstream.constEnd(); ++it) {
        const QString &branch = it.key();
        QString target = it.value();
        const QString have = local.value(branch);
        if (have == target)
            continue;
        const bool forceTracked =
            forced.contains(branch) ||
            (repointOrigin &&
             branch.startsWith(QStringLiteral("forkmesh/pull/")));
        const QString worktree = checkedOut.value(branch);
        bool fastForward = !have.isEmpty() && containedIn(path, have, target);
        if (reconcileCollaboration && !have.isEmpty() && !fastForward &&
            !containedIn(path, target, have)) {
            const QString merged =
                reconcileCollaborationDivergence(path, have, target);
            if (!merged.isEmpty()) {
                target = merged;
                fastForward = true; // the synthetic merge has `have` as parent
            }
        }
        if (!worktree.isEmpty()) {
            bool moved = false;
            if (fastForward) {
                moved = runGit(worktree,
                               {QStringLiteral("merge"),
                                QStringLiteral("--ff-only"), target},
                               nullptr);
                if (!moved && recoverGeneratedStats &&
                    onlyGeneratedStatsDirty(worktree) &&
                    runGit(worktree,
                           {QStringLiteral("restore"),
                            QStringLiteral("--source=HEAD"),
                            QStringLiteral("--staged"),
                            QStringLiteral("--worktree"),
                            QStringLiteral("--"), kGeneratedStatsPath},
                           nullptr)) {
                    moved = runGit(worktree,
                                   {QStringLiteral("merge"),
                                    QStringLiteral("--ff-only"), target},
                                   nullptr);
                }
            } else if (forceTracked && worktreeClean(worktree)) {
                keepReplacedTip(path, branch, have);
                moved = runGit(worktree,
                               {QStringLiteral("reset"), QStringLiteral("--hard"),
                                target},
                               nullptr);
            }
            if (moved) {
                ++outcome.branchesUpdated;
                if (branch == headBranch)
                    outcome.headFastForwarded = true;
            } else if (!fastForward && !forceTracked) {
                ++outcome.branchesKept;
            }
            continue;
        }
        // Reverse convergence must not resurrect a branch the source already
        // deleted after merging it. A mirror-only tip wholly contained in the
        // source's primary branch carries no unique work to recover; leave it
        // absent so the following source fan-out prunes it from that mirror.
        // A mirror-only branch with unique commits is still materialized and
        // therefore appears as pending source work until it is propagated.
        const bool staleMirrorOnlyBranch =
            reconcileCollaboration && have.isEmpty() && !localMain.isEmpty() &&
            containedIn(path, target, localMain);
        if (staleMirrorOnlyBranch)
            continue;
        const bool pullRecord =
            branch.startsWith(QStringLiteral("forkmesh/pull/"));
        if (have.isEmpty() && (!forceTracked || pullRecord) &&
            coveredByDeletion(path, branch, target))
            continue;
        if (have.isEmpty() || fastForward || forceTracked) {
            if (!fastForward)
                keepReplacedTip(path, branch, have);
            if (runGit(path,
                       {QStringLiteral("branch"), QStringLiteral("--force"),
                        branch, target},
                       nullptr))
                ++outcome.branchesUpdated;
        } else {
            ++outcome.branchesKept; // local-only commits: never clobbered
        }
    }

    QHash<QString, QStringList> trashed = upstreamTrashTips(path, trashNs);
    for (auto it = servedTrash.constBegin(); it != servedTrash.constEnd(); ++it)
        for (const QString &tip : it.value())
            if (!trashed[it.key()].contains(tip))
                trashed[it.key()].append(tip);
    const QString upstreamPrimary =
        !upstream.value(QStringLiteral("main")).isEmpty()
            ? upstream.value(QStringLiteral("main"))
            : upstream.value(QStringLiteral("master"));
    QSet<QString> deletedUpstream;
    for (auto it = local.constBegin(); it != local.constEnd(); ++it) {
        const QString &branch = it.key();
        const QString &tip = it.value();
        if (upstream.contains(branch) || checkedOut.contains(branch) ||
            branch == headBranch ||
            branch.startsWith(QStringLiteral("forkmesh/")) ||
            !trashed.contains(branch) || coveredByDeletion(path, branch, tip))
            continue;
        const bool pushedWork =
            seenUpstream.contains(branch) || upstreamPrimary.isEmpty() ||
            !containedIn(path, tip, upstreamPrimary);
        bool covered = false;
        for (const QString &trashTip : trashed.value(branch)) {
            if (trashTip == tip ||
                (pushedWork && containedIn(path, tip, trashTip))) {
                covered = true;
                break;
            }
        }
        if (!covered || !recordUpstreamDeletion(path, branch, tip))
            continue;
        if (runGit(path,
                   {QStringLiteral("update-ref"), QStringLiteral("-d"),
                    QStringLiteral("refs/heads/") + branch, tip},
                   nullptr)) {
            deletedUpstream.insert(branch);
            ++outcome.branchesPruned;
        }
    }

    if (pruneGone) {
        const QString upstreamMain = upstream.value(QStringLiteral("main"));
        for (auto it = local.constBegin(); it != local.constEnd(); ++it) {
            const QString &branch = it.key();
            if (upstream.contains(branch) || checkedOut.contains(branch) ||
                deletedUpstream.contains(branch))
                continue;
            if (repointOrigin &&
                branch.startsWith(QStringLiteral("forkmesh/pull/"))) {
                keepReplacedTip(path, branch, it.value());
                if (runGit(path,
                           {QStringLiteral("branch"), QStringLiteral("-D"),
                            branch},
                           nullptr))
                    ++outcome.branchesPruned;
                continue;
            }
            if (!upstreamMain.isEmpty() &&
                containedIn(path, it.value(), upstreamMain)) {
                if (runGit(path,
                           {QStringLiteral("branch"), QStringLiteral("-D"),
                            branch},
                           nullptr))
                    ++outcome.branchesPruned;
            } else {
                ++outcome.branchesKept;
            }
        }
    }

    return outcome;
}

RefreshOutcome refreshManagedCheckoutFromUpstream(
    const QString &checkoutPath, const QString &upstreamUrl,
    const QStringList &forcedBranches)
{
    return refreshCore(checkoutPath, upstreamUrl, forcedBranches,
                       /*repointOrigin=*/true, /*pruneGone=*/true,
                       /*recoverGeneratedStats=*/true,
                       /*reconcileCollaboration=*/false);
}

RefreshOutcome convergeSourceCheckoutFromMesh(const QString &checkoutPath,
                                              const QString &meshUrl)
{
    return convergeSourceCheckoutFromMirrors(checkoutPath,
                                             QStringList{meshUrl});
}

RefreshOutcome convergeSourceCheckoutFromMirrors(
    const QString &checkoutPath, const QStringList &mirrorUrls)
{
    QList<MirrorRoute> routes;
    routes.reserve(mirrorUrls.size());
    for (const QString &url : mirrorUrls)
        routes.append({url, {}});
    return convergeSourceCheckoutFromRoutes(checkoutPath, routes);
}

ServedBranchSync syncServedAgentBranches(const QString &checkoutPath,
                                         const QStringList &sources,
                                         const QStringList &branches)
{
    constexpr int kServedBranchFetchTimeoutMs = 90 * 1000;
    ServedBranchSync outcome;
    const QString path = checkoutPath.trimmed();
    QStringList remaining;
    for (const QString &raw : branches) {
        const QString branch = raw.trimmed();
        if (forkmesh::localcontrol::handoffBranchValid(branch) &&
            !remaining.contains(branch))
            remaining.append(branch);
    }
    if (path.isEmpty() || remaining.isEmpty() || !QDir(path).exists())
        return outcome;
    const QHash<QString, QString> checkedOut = checkedOutBranches(path);
    QStringList tried;
    for (const QString &rawSource : sources) {
        const QString source = rawSource.trimmed();
        if (source.isEmpty() || tried.contains(source) || remaining.isEmpty())
            continue;
        tried.append(source);
        QStringList lsArgs{QStringLiteral("ls-remote"), source};
        for (const QString &branch : std::as_const(remaining))
            lsArgs.append(QStringLiteral("refs/heads/") + branch);
        QString listing;
        if (!runGit(path, lsArgs, &listing))
            continue;
        outcome.reachedSource = true;
        QHash<QString, QString> advertised;
        for (const QString &line : listing.split(QLatin1Char('\n'))) {
            const QStringList fields =
                line.trimmed().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
            if (fields.size() != 2 ||
                !fields.at(1).startsWith(QLatin1String("refs/heads/")))
                continue;
            const QString branch = fields.at(1).mid(11);
            if (remaining.contains(branch))
                advertised.insert(branch, fields.at(0).trimmed());
        }
        if (advertised.isEmpty())
            continue;
        const QHash<QString, QString> local =
            refTips(path, QStringLiteral("refs/heads/agent"), 2);
        const auto hasCommit = [&path](const QString &sha) {
            return runGit(path,
                          {QStringLiteral("cat-file"), QStringLiteral("-e"),
                           sha + QStringLiteral("^{commit}")},
                          nullptr);
        };
        QStringList fetchArgs{QStringLiteral("fetch"), QStringLiteral("--quiet"),
                              QStringLiteral("--no-tags"),
                              QStringLiteral("--no-write-fetch-head"), source};
        for (auto it = advertised.constBegin(); it != advertised.constEnd(); ++it)
            if (local.value(it.key()) != it.value() && !hasCommit(it.value()))
                fetchArgs.append(QStringLiteral("refs/heads/") + it.key());
        if (fetchArgs.size() > 5)
            runGit(path, fetchArgs, nullptr, kServedBranchFetchTimeoutMs);
        for (auto it = advertised.constBegin(); it != advertised.constEnd(); ++it) {
            const QString &branch = it.key();
            const QString &target = it.value();
            if (!hasCommit(target))
                continue;
            remaining.removeAll(branch);
            const QString have = local.value(branch);
            if (have == target) {
                outcome.current.append(branch);
                continue;
            }
            if (checkedOut.contains(branch) ||
                (!have.isEmpty() && !containedIn(path, have, target))) {
                outcome.kept.append(branch);
                continue;
            }
            if (runGit(path,
                       {QStringLiteral("update-ref"), QStringLiteral("-m"),
                        QStringLiteral("forkmesh: sync cloud agent branch"),
                        QStringLiteral("refs/heads/") + branch, target, have},
                       nullptr))
                outcome.updated.append(branch);
            else
                outcome.kept.append(branch);
        }
    }
    outcome.missing = remaining;
    const QHash<QString, QString> after =
        refTips(path, QStringLiteral("refs/heads/agent"), 2);
    for (const QString &raw : branches) {
        const QString branch = raw.trimmed();
        if (after.contains(branch) && !outcome.present.contains(branch))
            outcome.present.append(branch);
    }
    return outcome;
}

QStringList refsNamedAsBadObjects(const QString &gitErrors)
{
    static const QRegularExpression badObject(
        QStringLiteral("\\bbad object (refs/[^\\s'\"]+)"));
    QStringList refs;
    QRegularExpressionMatchIterator it = badObject.globalMatch(gitErrors);
    while (it.hasNext()) {
        const QString ref = it.next().captured(1);
        if (!ref.contains(QStringLiteral("..")) && !refs.contains(ref))
            refs.append(ref);
    }
    return refs;
}

QStringList dropUnreadableRefs(const QString &gitDir, const QString &gitErrors)
{
    QStringList dropped;
    for (const QString &ref : refsNamedAsBadObjects(gitErrors)) {
        QString target;
        if (!runGit(gitDir,
                    {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                     QStringLiteral("--quiet"), QStringLiteral("--end-of-options"),
                     ref},
                    &target))
            continue;
        target = target.trimmed();
        if (target.isEmpty() ||
            runGit(gitDir, {QStringLiteral("cat-file"), QStringLiteral("-t"),
                            target},
                   nullptr))
            continue;
        if (runGit(gitDir, {QStringLiteral("update-ref"), QStringLiteral("-d"),
                            ref, target},
                   nullptr))
            dropped.append(ref);
    }
    return dropped;
}

RefreshOutcome convergeSourceCheckoutFromRoutes(
    const QString &checkoutPath, const QList<MirrorRoute> &mirrorRoutes,
    const std::function<void()> &afterEachReachableRoute)
{
    RefreshOutcome combined;
    struct RouteGroup {
        QString key;
        QList<MirrorRoute> alternatives;
    };
    QList<MirrorRoute> routes;
    for (const MirrorRoute &raw : mirrorRoutes) {
        MirrorRoute route = raw;
        route.url = route.url.trimmed();
        route.fallbackGroup = route.fallbackGroup.trimmed();
        if (route.url.isEmpty())
            continue;
        auto duplicate = std::find_if(
            routes.begin(), routes.end(), [&route](const MirrorRoute &existing) {
                return existing.url == route.url;
            });
        if (duplicate == routes.end()) {
            routes.append(route);
        } else {
            if (duplicate->environment.isEmpty() &&
                !route.environment.isEmpty())
                duplicate->environment = route.environment;
            if (duplicate->fallbackGroup.isEmpty() &&
                !route.fallbackGroup.isEmpty())
                duplicate->fallbackGroup = route.fallbackGroup;
            if (duplicate->deletedBranches.isEmpty())
                duplicate->deletedBranches = route.deletedBranches;
        }
    }

    QList<RouteGroup> groups;
    for (const MirrorRoute &route : std::as_const(routes)) {
        // An ungrouped route is a required mirror of its own. Prefix the URL
        // so an explicit group can never collide with this internal key.
        const QString groupKey =
            route.fallbackGroup.isEmpty()
                ? QString(QChar(u'\x1f')) + route.url
                : route.fallbackGroup;
        auto group = std::find_if(
            groups.begin(), groups.end(), [&groupKey](const RouteGroup &value) {
                return value.key == groupKey;
            });
        if (group == groups.end()) {
            groups.append({groupKey, {route}});
        } else {
            group->alternatives.append(route);
        }
    }
    if (groups.isEmpty()) {
        combined.error = QStringLiteral("no mirror routes to refresh");
        return combined;
    }
    int failures = 0;
    for (const RouteGroup &group : std::as_const(groups)) {
        bool groupRead = false;
        for (const MirrorRoute &route : group.alternatives) {
            RefreshOutcome current =
                refreshCore(checkoutPath, route.url, /*forcedBranches=*/{},
                            /*repointOrigin=*/false, /*pruneGone=*/false,
                            /*recoverGeneratedStats=*/false,
                            /*reconcileCollaboration=*/true,
                            route.environment, route.deletedBranches);
            if (!current.fetchOk || !current.error.isEmpty())
                continue;
            groupRead = true;
            combined.fetchOk = true;
            combined.branchesUpdated += current.branchesUpdated;
            combined.branchesPruned += current.branchesPruned;
            combined.branchesKept += current.branchesKept;
            combined.headFastForwarded =
                combined.headFastForwarded || current.headFastForwarded;
            // Make each successfully recovered generation publishable
            // immediately. A later dead mirror may consume its whole bounded
            // fetch timeout; it must not hold already-integrated collaboration
            // work in the checkout while the served repository remains stale.
            if (afterEachReachableRoute)
                afterEachReachableRoute();
            break;
        }
        if (!groupRead)
            ++failures;
    }
    if (!combined.fetchOk)
        combined.error = QStringLiteral("fetch from every mirror failed");
    else if (failures > 0) {
        combined.error = QStringLiteral("%1 mirror%2 could not be read")
                             .arg(failures)
                             .arg(failures == 1 ? QString()
                                                : QStringLiteral("s"));
    }
    return combined;
}

} // namespace forkmesh::upstream

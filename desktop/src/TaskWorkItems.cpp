#include "TaskWorkItems.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>

namespace forkmesh::taskwork {

namespace {

constexpr int kGitTimeoutMs = 60 * 1000;

struct GitResult {
    bool ok = false;
    QString out;
    QString err;
};

QProcessEnvironment gitEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
                             "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY",
                             "GIT_NAMESPACE", "GIT_PREFIX"})
        environment.remove(QString::fromLatin1(name));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    return environment;
}

// code-health: allow(blocking-wait) only reached from the Tasks cleanup worker started with QtConcurrent::run
GitResult git(const QString &dir, const QStringList &args,
              const QProcessEnvironment &environment = gitEnvironment())
{
    GitResult result;
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), dir} + args);
    if (!process.waitForFinished(kGitTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        result.err = QStringLiteral("git did not finish in time");
        return result;
    }
    result.out = QString::fromUtf8(process.readAllStandardOutput());
    result.err = QString::fromUtf8(process.readAllStandardError()).trimmed();
    result.ok = process.exitStatus() == QProcess::NormalExit &&
                process.exitCode() == 0;
    return result;
}

QString cleanAbsolute(const QString &path)
{
    return QDir::cleanPath(QDir(path).absolutePath());
}

QString revision(const QString &dir, const QString &ref)
{
    const GitResult result =
        git(dir, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                  QStringLiteral("--quiet"), QStringLiteral("--end-of-options"),
                  ref + QStringLiteral("^{commit}")});
    return result.ok ? result.out.trimmed() : QString();
}

QString localBranchName(const QString &ref)
{
    return ref.startsWith(QLatin1String("refs/heads/")) ? ref.mid(11) : QString();
}

QString escapeJsonString(const QString &text)
{
    QString out;
    out.reserve(text.size() + 2);
    out.append(QLatin1Char('"'));
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        switch (code) {
        case '"': out.append(QStringLiteral("\\\"")); break;
        case '\\': out.append(QStringLiteral("\\\\")); break;
        case '\n': out.append(QStringLiteral("\\n")); break;
        case '\r': out.append(QStringLiteral("\\r")); break;
        case '\t': out.append(QStringLiteral("\\t")); break;
        case '\b': out.append(QStringLiteral("\\b")); break;
        case '\f': out.append(QStringLiteral("\\f")); break;
        default:
            if (code < 0x20)
                out.append(QStringLiteral("\\u%1").arg(code, 4, 16, QLatin1Char('0')));
            else
                out.append(ch);
        }
    }
    out.append(QLatin1Char('"'));
    return out;
}

QString canonicalText(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return QStringLiteral("null");
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (number == double(qint64(number)) && qAbs(number) < 9007199254740992.0)
            return QString::number(qint64(number));
        return QString::number(number, 'g', QLocale::FloatingPointShortest);
    }
    case QJsonValue::String:
        return escapeJsonString(value.toString());
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue &entry : value.toArray())
            parts.append(canonicalText(entry));
        return QLatin1Char('[') + parts.join(QLatin1Char(',')) + QLatin1Char(']');
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
            return a.toUcs4() < b.toUcs4();
        });
        QStringList parts;
        for (const QString &key : std::as_const(keys))
            parts.append(escapeJsonString(key) + QLatin1Char(':') +
                         canonicalText(object.value(key)));
        return QLatin1Char('{') + parts.join(QLatin1Char(',')) + QLatin1Char('}');
    }
    }
    return QStringLiteral("null");
}

} // namespace

QJsonObject toJson(const WorkItem &item)
{
    return QJsonObject{
        {QStringLiteral("repository"), item.repository},
        {QStringLiteral("branch"), item.branch},
        {QStringLiteral("worktree"), item.worktree},
        {QStringLiteral("node"), item.node},
    };
}

WorkItem fromJson(const QJsonObject &object)
{
    WorkItem item;
    item.repository = object.value(QStringLiteral("repository")).toString().trimmed();
    item.branch = object.value(QStringLiteral("branch")).toString().trimmed();
    item.worktree = object.value(QStringLiteral("worktree")).toString();
    item.node = object.value(QStringLiteral("node")).toString().trimmed().toLower();
    return item;
}

QList<WorkItem> fromJsonArray(const QJsonArray &array)
{
    QList<WorkItem> items;
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const WorkItem item = fromJson(value.toObject());
        if (!item.branch.isEmpty())
            items.append(item);
    }
    return items;
}

QJsonArray toJsonArray(const QList<WorkItem> &items)
{
    QJsonArray array;
    for (const WorkItem &item : items)
        array.append(toJson(item));
    return array;
}

bool sameKey(const WorkItem &a, const WorkItem &b)
{
    return a.repository.compare(b.repository, Qt::CaseInsensitive) == 0 &&
           a.branch == b.branch && a.node.toLower() == b.node.toLower();
}

QList<WorkItem> withItem(const QList<WorkItem> &items, const WorkItem &item)
{
    QList<WorkItem> merged;
    bool replaced = false;
    for (const WorkItem &existing : items) {
        if (sameKey(existing, item)) {
            if (!replaced)
                merged.append(item);
            replaced = true;
            continue;
        }
        merged.append(existing);
    }
    if (!replaced)
        merged.append(item);
    while (merged.size() > kMaxWorkItems)
        merged.removeFirst();
    return merged;
}

QList<WorkItem> withoutItem(const QList<WorkItem> &items, const WorkItem &item)
{
    QList<WorkItem> kept;
    for (const WorkItem &existing : items)
        if (!sameKey(existing, item))
            kept.append(existing);
    return kept;
}

QList<WorkItem> itemsForNode(const QList<WorkItem> &items, const QString &node)
{
    QList<WorkItem> mine;
    const QString wanted = node.trimmed().toLower();
    if (wanted.isEmpty())
        return mine;
    for (const WorkItem &item : items)
        if (item.node.toLower() == wanted)
            mine.append(item);
    return mine;
}

bool validBranch(const QString &branch)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9._/+-]{1,200}$"));
    if (!allowed.match(branch).hasMatch())
        return false;
    if (branch.startsWith(QLatin1Char('-')) || branch.startsWith(QLatin1Char('/')) ||
        branch.startsWith(QLatin1Char('.')) || branch.endsWith(QLatin1Char('/')) ||
        branch.endsWith(QLatin1Char('.')) || branch.endsWith(QLatin1String(".lock")) ||
        branch.contains(QLatin1String("..")) || branch.contains(QLatin1String("//")) ||
        branch == QLatin1String("HEAD"))
        return false;
    for (const QString &part : branch.split(QLatin1Char('/')))
        if (part.startsWith(QLatin1Char('.')) || part.endsWith(QLatin1String(".lock")))
            return false;
    return true;
}

bool validRepository(const QString &repository)
{
    static const QRegularExpression segment(QStringLiteral("^[A-Za-z0-9._-]{1,100}$"));
    if (repository.isEmpty())
        return true;
    const QStringList parts = repository.split(QLatin1Char('/'));
    return parts.size() == 2 && segment.match(parts.at(0)).hasMatch() &&
           segment.match(parts.at(1)).hasMatch();
}

bool validNode(const QString &node)
{
    static const QRegularExpression re(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    return node.isEmpty() || re.match(node).hasMatch();
}

bool validTaskId(const QString &taskId)
{
    static const QRegularExpression re(QStringLiteral("^[a-f0-9]{32}$"));
    return re.match(taskId).hasMatch();
}

QByteArray canonicalJson(const QJsonValue &value)
{
    return canonicalText(value).toUtf8();
}

QString updateDigest(const QJsonObject &body)
{
    QJsonObject signedBody = body;
    signedBody.remove(QStringLiteral("sessionToken"));
    if (signedBody.isEmpty())
        return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash(canonicalJson(signedBody),
                                 QCryptographicHash::Sha256)
            .toHex());
}

QList<WorktreeEntry> parseWorktreeList(const QString &porcelain)
{
    QList<WorktreeEntry> entries;
    WorktreeEntry current;
    bool open = false;
    const QStringList lines = porcelain.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.endsWith(QLatin1Char('\r')) ? raw.chopped(1) : raw;
        if (line.isEmpty()) {
            if (open)
                entries.append(current);
            current = WorktreeEntry();
            open = false;
            continue;
        }
        if (line.startsWith(QLatin1String("worktree "))) {
            if (open)
                entries.append(current);
            current = WorktreeEntry();
            current.path = line.mid(9);
            open = true;
        } else if (line.startsWith(QLatin1String("HEAD "))) {
            current.head = line.mid(5);
        } else if (line.startsWith(QLatin1String("branch "))) {
            current.branch = line.mid(7);
        } else if (line == QLatin1String("bare")) {
            current.bare = true;
        } else if (line == QLatin1String("detached")) {
            current.detached = true;
        }
    }
    if (open)
        entries.append(current);
    return entries;
}

bool samePath(const QString &a, const QString &b)
{
    if (a.trimmed().isEmpty() || b.trimmed().isEmpty())
        return false;
    if (cleanAbsolute(a) == cleanAbsolute(b))
        return true;
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
    return !ca.isEmpty() && ca == cb;
}

bool pathWithin(const QString &path, const QString &root)
{
    if (path.trimmed().isEmpty() || root.trimmed().isEmpty())
        return false;
    const auto inside = [](const QString &p, const QString &r) {
        if (p == r)
            return true;
        const QString prefix = r.endsWith(QLatin1Char('/')) ? r : r + QLatin1Char('/');
        return p.startsWith(prefix);
    };
    if (inside(cleanAbsolute(path), cleanAbsolute(root)))
        return true;
    const QString cp = QFileInfo(path).canonicalFilePath();
    const QString cr = QFileInfo(root).canonicalFilePath();
    return !cp.isEmpty() && !cr.isEmpty() && inside(cp, cr);
}

QString pathRefusal(const QString &worktree, const QList<WorktreeEntry> &entries,
                    const Guard &guard)
{
    if (worktree.trimmed().isEmpty())
        return QStringLiteral("no worktree path was recorded");
    if (!QDir::isAbsolutePath(worktree))
        return QStringLiteral("%1 is not an absolute path").arg(worktree);
    if (entries.isEmpty())
        return QStringLiteral("the repository lists no worktrees");
    int index = -1;
    for (int i = 0; i < entries.size(); ++i) {
        if (samePath(entries.at(i).path, worktree)) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return QStringLiteral("%1 is not a worktree of this repository").arg(worktree);
    if (index == 0 || entries.at(index).bare)
        return QStringLiteral("%1 is the repository's main checkout").arg(worktree);
    if (pathWithin(entries.at(0).path, worktree))
        return QStringLiteral("%1 contains the repository's main checkout").arg(worktree);
    if (!guard.home.isEmpty() && pathWithin(guard.home, worktree))
        return QStringLiteral("%1 is the home folder or contains it").arg(worktree);
    for (const QString &dir : guard.protectedDirs) {
        if (dir.trimmed().isEmpty())
            continue;
        if (pathWithin(worktree, dir) || pathWithin(dir, worktree))
            return QStringLiteral("%1 overlaps ForkMesh data at %2").arg(worktree, dir);
    }
    return QString();
}

QString branchRefusal(const QString &branch, const QString &mainCheckoutBranch,
                      const QString &defaultBranch)
{
    if (!validBranch(branch))
        return QStringLiteral("%1 is not a usable branch name").arg(branch);
    if (branch == QLatin1String("main") || branch == QLatin1String("master"))
        return QStringLiteral("%1 is a protected branch").arg(branch);
    if (!mainCheckoutBranch.isEmpty() && branch == mainCheckoutBranch)
        return QStringLiteral("%1 is checked out in the main checkout").arg(branch);
    if (!defaultBranch.isEmpty() && branch == defaultBranch)
        return QStringLiteral("%1 is the default branch").arg(branch);
    return QString();
}

Plan decide(bool refused, bool dirty, bool merged)
{
    if (refused)
        return Plan::Refuse;
    return !dirty && merged ? Plan::Remove : Plan::Confirm;
}

QString backupRef(const QString &taskId, const QString &branch)
{
    return QStringLiteral("refs/forkmesh/trash/tasks/%1/%2").arg(taskId, branch);
}

bool findLinkedWorktree(const QString &repoPath, const QString &path,
                        WorktreeEntry *entry)
{
    const GitResult listed = git(repoPath, {QStringLiteral("worktree"),
                                            QStringLiteral("list"),
                                            QStringLiteral("--porcelain")});
    if (!listed.ok)
        return false;
    const QList<WorktreeEntry> entries = parseWorktreeList(listed.out);
    for (int i = 1; i < entries.size(); ++i) {
        if (samePath(entries.at(i).path, path)) {
            if (entry)
                *entry = entries.at(i);
            return true;
        }
    }
    return false;
}

bool worktreeListed(const QString &repoPath, const QString &worktree)
{
    return findLinkedWorktree(repoPath, worktree, nullptr);
}

Inspection inspect(const QString &repoPath, const WorkItem &item,
                   const Guard &guard, const QString &defaultBranchHint)
{
    Inspection result;
    result.repoPath = repoPath;
    result.item = item;
    const auto refuse = [&result](const QString &why) {
        result.plan = Plan::Refuse;
        result.reason = why;
        return result;
    };
    const GitResult listed = git(repoPath, {QStringLiteral("worktree"),
                                            QStringLiteral("list"),
                                            QStringLiteral("--porcelain")});
    if (!listed.ok)
        return refuse(QStringLiteral("could not list the worktrees of %1: %2")
                          .arg(repoPath, listed.err));
    const QList<WorktreeEntry> entries = parseWorktreeList(listed.out);
    if (entries.isEmpty())
        return refuse(QStringLiteral("%1 lists no worktrees").arg(repoPath));
    const QString mainBranch = localBranchName(entries.first().branch);

    QString defaultBranch = defaultBranchHint.trimmed();
    if (!defaultBranch.isEmpty() &&
        revision(repoPath, QStringLiteral("refs/heads/") + defaultBranch).isEmpty())
        defaultBranch.clear();
    if (defaultBranch.isEmpty()) {
        const GitResult originHead =
            git(repoPath, {QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"),
                           QStringLiteral("--short"),
                           QStringLiteral("refs/remotes/origin/HEAD")});
        const QString remote = originHead.out.trimmed();
        if (originHead.ok && remote.startsWith(QLatin1String("origin/")))
            defaultBranch = remote.mid(7);
    }
    if (defaultBranch.isEmpty() &&
        !revision(repoPath, QStringLiteral("refs/heads/main")).isEmpty())
        defaultBranch = QStringLiteral("main");
    if (defaultBranch.isEmpty() &&
        !revision(repoPath, QStringLiteral("refs/heads/master")).isEmpty())
        defaultBranch = QStringLiteral("master");
    result.defaultBranch = defaultBranch;

    const QString branchWhy = branchRefusal(item.branch, mainBranch, defaultBranch);
    if (!branchWhy.isEmpty())
        return refuse(branchWhy);
    const QString branchRef = QStringLiteral("refs/heads/") + item.branch;
    result.branchExists = !revision(repoPath, branchRef).isEmpty();

    const WorktreeEntry *holder = nullptr;
    for (const WorktreeEntry &entry : entries)
        if (entry.branch == branchRef) {
            holder = &entry;
            break;
        }

    if (!item.worktree.isEmpty()) {
        const WorktreeEntry *listedEntry = nullptr;
        for (const WorktreeEntry &entry : entries)
            if (samePath(entry.path, item.worktree)) {
                listedEntry = &entry;
                break;
            }
        if (!listedEntry && QFileInfo::exists(item.worktree))
            return refuse(QStringLiteral("%1 is not a worktree of %2")
                              .arg(item.worktree, repoPath));
        if (listedEntry) {
            const QString pathWhy = pathRefusal(item.worktree, entries, guard);
            if (!pathWhy.isEmpty())
                return refuse(pathWhy);
            if (listedEntry->detached || listedEntry->branch.isEmpty())
                return refuse(QStringLiteral("%1 has a detached HEAD, not %2")
                                  .arg(item.worktree, item.branch));
            if (listedEntry->branch != branchRef)
                return refuse(QStringLiteral("%1 is on %2, not %3")
                                  .arg(item.worktree,
                                       localBranchName(listedEntry->branch),
                                       item.branch));
            result.worktreePath = listedEntry->path;
        }
    }
    if (result.worktreePath.isEmpty()) {
        if (!result.branchExists)
            return refuse(QStringLiteral("%1 and its worktree are already gone")
                              .arg(item.branch));
        if (holder)
            return refuse(QStringLiteral("%1 is checked out at %2")
                              .arg(item.branch, holder->path));
    }

    if (!result.worktreePath.isEmpty()) {
        const GitResult status =
            git(result.worktreePath,
                {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                 QStringLiteral("--untracked-files=all")});
        if (!status.ok)
            return refuse(QStringLiteral("could not read the status of %1: %2")
                              .arg(result.worktreePath, status.err));
        for (const QString &line : status.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            result.dirtyFiles.append(line);
        result.dirty = !result.dirtyFiles.isEmpty();
    }

    QString baseRef;
    if (!defaultBranch.isEmpty()) {
        const QString local = QStringLiteral("refs/heads/") + defaultBranch;
        const QString remote = QStringLiteral("refs/remotes/origin/") + defaultBranch;
        const bool haveLocal = !revision(repoPath, local).isEmpty();
        const bool haveRemote = !revision(repoPath, remote).isEmpty();
        baseRef = haveLocal ? local : haveRemote ? remote : QString();
        if (result.branchExists && haveLocal)
            result.merged = git(repoPath, {QStringLiteral("merge-base"),
                                           QStringLiteral("--is-ancestor"),
                                           branchRef, local}).ok;
        if (result.branchExists && !result.merged && haveRemote)
            result.merged = git(repoPath, {QStringLiteral("merge-base"),
                                           QStringLiteral("--is-ancestor"),
                                           branchRef, remote}).ok;
    }
    if (result.branchExists && !result.merged) {
        const GitResult log =
            git(repoPath, {QStringLiteral("log"), QStringLiteral("--format=%h %s"),
                           QStringLiteral("-n"), QStringLiteral("20"),
                           baseRef.isEmpty() ? branchRef
                                             : baseRef + QStringLiteral("..") + branchRef});
        if (log.ok)
            result.unmergedCommits = log.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }
    result.plan = decide(false, result.dirty, result.merged);
    return result;
}

Outcome removeMerged(const Inspection &inspection)
{
    Outcome outcome;
    if (inspection.plan != Plan::Remove) {
        outcome.message = QStringLiteral("%1 is not clean and merged; left alone")
                              .arg(inspection.item.branch);
        return outcome;
    }
    if (!inspection.worktreePath.isEmpty()) {
        const GitResult removed =
            git(inspection.repoPath, {QStringLiteral("worktree"), QStringLiteral("remove"),
                                      inspection.worktreePath});
        if (!removed.ok) {
            outcome.message = QStringLiteral("git worktree remove %1 failed: %2")
                                  .arg(inspection.worktreePath, removed.err);
            return outcome;
        }
    }
    if (inspection.branchExists) {
        const GitResult deleted =
            git(inspection.repoPath, {QStringLiteral("branch"), QStringLiteral("-d"),
                                      QStringLiteral("--"), inspection.item.branch});
        if (!deleted.ok) {
            outcome.message =
                QStringLiteral("%1 removed; git branch -d %2 refused: %3")
                    .arg(inspection.worktreePath.isEmpty() ? QStringLiteral("Nothing")
                                                           : inspection.worktreePath,
                         inspection.item.branch, deleted.err);
            return outcome;
        }
    }
    outcome.ok = true;
    outcome.message =
        inspection.worktreePath.isEmpty()
            ? QStringLiteral("Deleted merged branch %1.").arg(inspection.item.branch)
            : QStringLiteral("Removed worktree %1 and merged branch %2.")
                  .arg(inspection.worktreePath, inspection.item.branch);
    return outcome;
}

Outcome removeWithBackup(const Inspection &inspection, const QString &taskId)
{
    Outcome outcome;
    const QString repo = inspection.repoPath;
    const QString branch = inspection.item.branch;
    if (inspection.plan == Plan::Refuse || !validTaskId(taskId) || !validBranch(branch)) {
        outcome.message = QStringLiteral("%1 was not inspected as removable; left alone")
                              .arg(branch);
        return outcome;
    }
    QString target;
    const QString wipRef =
        QStringLiteral("refs/forkmesh/wip/tasks/%1/%2").arg(taskId, branch);
    if (inspection.dirty && !inspection.worktreePath.isEmpty()) {
        QTemporaryDir scratch;
        if (!scratch.isValid()) {
            outcome.message = QStringLiteral("no temporary directory for the backup index");
            return outcome;
        }
        QProcessEnvironment environment = gitEnvironment();
        environment.insert(QStringLiteral("GIT_INDEX_FILE"),
                           scratch.filePath(QStringLiteral("index")));
        const QString wt = inspection.worktreePath;
        if (!git(wt, {QStringLiteral("var"), QStringLiteral("GIT_COMMITTER_IDENT")}).ok) {
            environment.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("ForkMesh"));
            environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                               QStringLiteral("forkmesh@localhost"));
            environment.insert(QStringLiteral("GIT_COMMITTER_NAME"), QStringLiteral("ForkMesh"));
            environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                               QStringLiteral("forkmesh@localhost"));
        }
        const GitResult read =
            git(wt, {QStringLiteral("read-tree"), QStringLiteral("HEAD")}, environment);
        const GitResult added =
            read.ok ? git(wt, {QStringLiteral("add"), QStringLiteral("-A")}, environment)
                    : read;
        const GitResult tree =
            added.ok ? git(wt, {QStringLiteral("write-tree")}, environment) : added;
        const GitResult commit =
            tree.ok ? git(wt,
                          {QStringLiteral("commit-tree"), tree.out.trimmed(),
                           QStringLiteral("-p"), QStringLiteral("HEAD"),
                           QStringLiteral("-m"),
                           QStringLiteral("ForkMesh backup of %1 before task %2 was deleted")
                               .arg(branch, taskId)},
                          environment)
                    : tree;
        if (!commit.ok || commit.out.trimmed().isEmpty()) {
            outcome.message = QStringLiteral("could not snapshot %1: %2")
                                  .arg(wt, commit.err);
            return outcome;
        }
        target = commit.out.trimmed();
        if (!git(repo, {QStringLiteral("update-ref"), wipRef, target}).ok) {
            outcome.message = QStringLiteral("could not write %1").arg(wipRef);
            return outcome;
        }
    } else if (inspection.branchExists) {
        target = revision(repo, QStringLiteral("refs/heads/") + branch);
    } else if (!inspection.worktreePath.isEmpty()) {
        target = revision(inspection.worktreePath, QStringLiteral("HEAD"));
    }
    if (target.isEmpty()) {
        outcome.message = QStringLiteral("nothing to back up for %1").arg(branch);
        return outcome;
    }
    const QString trash = backupRef(taskId, branch);
    const GitResult wrote = git(repo, {QStringLiteral("update-ref"), trash, target});
    if (!wrote.ok || revision(repo, trash) != target) {
        outcome.message = QStringLiteral("could not write backup %1: %2").arg(trash, wrote.err);
        return outcome;
    }
    outcome.backupRef = trash;
    git(repo, {QStringLiteral("update-ref"), QStringLiteral("-d"), wipRef});
    if (!inspection.worktreePath.isEmpty()) {
        const GitResult removed =
            git(repo, {QStringLiteral("worktree"), QStringLiteral("remove"),
                       QStringLiteral("--force"), inspection.worktreePath});
        if (!removed.ok) {
            outcome.message = QStringLiteral("backed up to %1, but git worktree remove "
                                             "--force %2 failed: %3")
                                  .arg(trash, inspection.worktreePath, removed.err);
            return outcome;
        }
    }
    if (inspection.branchExists) {
        const GitResult deleted =
            git(repo, {QStringLiteral("branch"), QStringLiteral("-D"),
                       QStringLiteral("--"), branch});
        if (!deleted.ok) {
            outcome.message = QStringLiteral("backed up to %1, but git branch -D %2 "
                                             "failed: %3")
                                  .arg(trash, branch, deleted.err);
            return outcome;
        }
    }
    outcome.ok = true;
    outcome.message =
        QStringLiteral("Removed %1%2; backup kept at %3.")
            .arg(branch,
                 inspection.worktreePath.isEmpty()
                     ? QString()
                     : QStringLiteral(" and worktree %1").arg(inspection.worktreePath),
                 trash);
    return outcome;
}

QString lossSummary(const Inspection &inspection, int maxLines)
{
    QStringList lines;
    const auto section = [&lines, maxLines](const QString &heading,
                                            const QStringList &entries) {
        if (entries.isEmpty())
            return;
        lines.append(heading.arg(entries.size()));
        for (int i = 0; i < entries.size() && i < maxLines; ++i)
            lines.append(QStringLiteral("    ") + entries.at(i));
        if (entries.size() > maxLines)
            lines.append(QStringLiteral("    ... and %1 more").arg(entries.size() - maxLines));
    };
    section(QStringLiteral("  %1 uncommitted file(s):"), inspection.dirtyFiles);
    section(QStringLiteral("  %1 unmerged commit(s):"), inspection.unmergedCommits);
    return lines.join(QLatin1Char('\n'));
}

} // namespace forkmesh::taskwork

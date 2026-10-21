#include "PullStore.h"
#include "PullDecision.h"
#include "GitRefusal.h"
#include "AppPaths.h"
#include "SafeDelete.h"

#include "EventSigning.h"
#include "ForkMeshIdentity.h"
#include "GitIndexQueue.h"
#include "GracefulStop.h"
#include "StrictGitReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMutex>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <utility>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr int kGitTimeoutMs = 15000;
constexpr int kGitMergeTimeoutMs = 60000;
constexpr qint64 kMaxJsonSafeInteger = 9007199254740991LL;
// History scrubbing walks every reachable commit and already runs off the UI
// thread. Large repositories routinely need more than two minutes; aborting at
// that point leaves the requested deletion unfinished even though Git is still
// making progress.
constexpr int kGitRewriteTimeoutMs = 10 * 60 * 1000;

bool strictJsonSafeInteger(const QString &text, qint64 minimum,
                           qint64 *value = nullptr)
{
    bool ok = false;
    const qint64 parsed = text.toLongLong(&ok);
    if (!ok || parsed < minimum || parsed > kMaxJsonSafeInteger)
        return false;
    if (value)
        *value = parsed;
    return true;
}

// git filter-branch is a shell script that drives helper processes of its own.
// QProcess::kill() signals only that script, and its children go on running --
// long enough to recreate files inside the private rewrite directory we delete
// as soon as the call returns. Starting the script in its own process group
// lets a timeout end the whole tree at once.
void startInOwnProcessGroup(QProcess &process)
{
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
#else
    Q_UNUSED(process);
#endif
}

// Only ever pass ownProcessGroup for a process started by
// startInOwnProcessGroup(): signalling a group this process never created can
// reach unrelated processes that happen to share the number.
// code-health: allow(blocking-wait) timeout path only: SIGTERM first so git can drop index.lock, at most the grace period, then SIGKILL and a 200 ms reap
void killTimedOutProcess(QProcess &process, bool ownProcessGroup)
{
#ifdef Q_OS_UNIX
    const qint64 pid = process.processId();
    if (ownProcessGroup && pid > 0) {
        ::kill(static_cast<pid_t>(-pid), SIGTERM);
        if (!process.waitForFinished(forkmesh::kProcessStopGraceMs))
            ::kill(static_cast<pid_t>(-pid), SIGKILL);
    }
#else
    Q_UNUSED(ownProcessGroup);
#endif
    forkmesh::stopProcessGracefully(process);
}

// The one synchronous wait every PullStore git call goes through. Worker
// callers (runGit/runGitWithInput off the GUI thread, the pull conflict-check
// QThread) pass keepGuiAlive=false and take the plain bounded
// waitForFinished; only a GUI-thread caller pumps, in 40 ms slices.
// code-health: allow(blocking-wait, pump-in-loop) this is the keep-alive wait helper itself: the pump runs only when keepGuiAlive (runGit/runGitWithInput set it only on the GUI thread, processMergeQueue passes it from the GUI thread), 12 ms per 40 ms slice, bounded by timeoutMs and killed on expiry
bool waitForFinishedKeepAlive(QProcess &process, int timeoutMs, bool keepGuiAlive,
                              bool ownProcessGroup = false)
{
    if (!keepGuiAlive) {
        if (process.waitForFinished(timeoutMs))
            return true;
        // Killing here rather than leaving it to ~QProcess: the caller deletes
        // the rewrite directory the moment this returns.
        killTimedOutProcess(process, ownProcessGroup);
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(40)) {
        if (process.state() == QProcess::NotRunning)
            return true; // exited between polls; caller inspects the exit code
        if (timer.hasExpired(timeoutMs)) {
            killTimedOutProcess(process, ownProcessGroup);
            return false;
        }
        // This path is synchronous for legacy callers, but a slow repository
        // must not make the application stop accepting window/input events.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 12);
    }
    return true;
}

bool runGit(const QString &dir, const QStringList &args, QByteArray *output = nullptr,
            QString *errText = nullptr, int timeoutMs = kGitTimeoutMs)
{
    const bool rewriting = args.contains(QStringLiteral("filter-branch"));
    const int waitMs = forkmesh::gitTimeoutMsForArgs(args, timeoutMs);
    const forkmesh::GitAttempt attempt = forkmesh::runGitIndexSerialized(
        dir, args, [&dir, &args, rewriting, waitMs] {
            forkmesh::GitAttempt run;
            QProcess process;
            if (rewriting) {
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert(QStringLiteral("FILTER_BRANCH_SQUELCH_WARNING"),
                           QStringLiteral("1"));
                process.setProcessEnvironment(env);
                startInOwnProcessGroup(process);
            }
            process.start("git", QStringList{"-C", dir} + args);
            const QCoreApplication *app = QCoreApplication::instance();
            const bool onGuiThread =
                app && QThread::currentThread() == app->thread();
            if (!waitForFinishedKeepAlive(process, waitMs, onGuiThread, rewriting)) {
                run.timedOut = true;
                run.error = QStringLiteral("git timed out");
                return run;
            }
            run.output = process.readAllStandardOutput();
            if (process.exitStatus() != QProcess::NormalExit ||
                process.exitCode() != 0) {
                run.error = QString::fromUtf8(process.readAllStandardError())
                                .trimmed()
                                .left(300);
                return run;
            }
            run.ok = true;
            return run;
        });
    if (output && !attempt.timedOut)
        *output = attempt.output;
    if (!attempt.ok && errText)
        *errText = attempt.error;
    return attempt.ok;
}

bool runGitWithInput(const QString &dir, const QStringList &args,
                     const QByteArray &input, QByteArray *output = nullptr,
                     QString *errText = nullptr, int timeoutMs = kGitTimeoutMs)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    if (!process.waitForStarted(timeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git did not start");
        return false;
    }
    process.write(input);
    process.closeWriteChannel();
    const QCoreApplication *app = QCoreApplication::instance();
    const bool onGuiThread = app && QThread::currentThread() == app->thread();
    if (!waitForFinishedKeepAlive(process, timeoutMs, onGuiThread)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(300);
        return false;
    }
    return true;
}

QString pullMetadataBranchRef(int number)
{
    return QStringLiteral("refs/heads/forkmesh/pull/%1").arg(number);
}

QString headBranchFromFile(const QString &workTree)
{
    if (workTree.trimmed().isEmpty())
        return QString();

    QString gitDir;
    const QFileInfo dotGit(QDir(workTree).filePath(QStringLiteral(".git")));
    if (dotGit.isDir()) {
        gitDir = dotGit.absoluteFilePath();
    } else if (dotGit.isFile()) {
        QFile link(dotGit.absoluteFilePath());
        if (!link.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        const QString line = QString::fromUtf8(link.readLine(4096)).trimmed();
        if (!line.startsWith(QLatin1String("gitdir:")))
            return QString();
        const QString target = line.mid(7).trimmed();
        if (target.isEmpty())
            return QString();
        gitDir = QDir::isAbsolutePath(target)
                     ? target
                     : QDir(workTree).absoluteFilePath(target);
    } else if (QFileInfo::exists(
                   QDir(workTree).filePath(QStringLiteral("HEAD")))) {
        gitDir = workTree; // bare repository
    } else {
        return QString();
    }

    QFile head(QDir(gitDir).filePath(QStringLiteral("HEAD")));
    if (!head.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QString line = QString::fromUtf8(head.readLine(4096)).trimmed();
    static const QString prefix = QStringLiteral("ref: refs/heads/");
    return line.startsWith(prefix) ? line.mid(prefix.size()) : QString();
}

// Healing is worth exactly one pass per checkout per run: it costs a git call,
// and repeating it would race a delete that has removed a pull's files but not
// yet committed the removal (the restore would put them straight back).
bool claimPullMetadataHeal(const QString &dir)
{
    static QMutex mutex;
    static QSet<QString> healed;
    const QMutexLocker locker(&mutex);
    if (healed.contains(dir))
        return false;
    healed.insert(dir);
    return true;
}

// The pull-metadata worktree is a long-lived linked checkout of forkmesh/pulls,
// and every writable read below is a plain filesystem read of it. Anything that
// moves the branch without going through that checkout -- a forced fetch, a
// `git branch -f`, a history rewrite, a path migration commit, another node's
// ledger drained in by plumbing -- leaves HEAD pointing at the new tip while the
// files on disk stay behind. Paths the new tip *added* are simply absent, and
// then the desktop lists no pull requests at all while the branch carries every
// one of them; paths it *rewrote* keep their old contents, and then a pull the
// ledger has since closed still reads "status: open" here and never leaves the
// list. Restore every committed path the checkout disagrees with, added or
// rewritten. Untracked files are not touched, so a pull authored locally and not
// yet committed still survives; this runs once per checkout per run, before
// anything in this process has written to the ledger, so the only tracked edit
// it can discard is a leftover from a run that died between writing a file and
// committing it.
// code-health: allow(git-in-loop) argv chunking: one checkout per 200 stale ledger paths (normally none), once per checkout per run via claimPullMetadataHeal
void resyncPullMetadataCheckout(const QString &dir)
{
    // A ref that moved on its own also leaves the index describing the old
    // tip. Nothing here commits through the index -- every PullStore commit is
    // path-limited -- but a stale one reads as thousands of unrelated pending
    // changes, which is enough for deletePull()/setStatus() to refuse to touch
    // the ledger at all.
    runGit(dir, {"reset", "--mixed", "-q", "HEAD"}, nullptr, nullptr);

    QByteArray listing;
    if (!runGit(dir, {"diff", "--name-only", "-z", "HEAD", "--",
                      ".forkmesh/pulls/", ".forkmesh/branch-reviews/"},
                &listing, nullptr))
        return;
    QStringList stale;
    for (const QByteArray &entry : listing.split('\0')) {
        if (entry.isEmpty())
            continue;
        stale.append(QString::fromUtf8(entry));
    }
    while (!stale.isEmpty()) {
        // Chunked: a repository with a long pull history can list more paths
        // than one command line holds.
        const QStringList batch = stale.mid(0, 200);
        stale = stale.mid(batch.size());
        runGit(dir, QStringList{"checkout", "-f", "HEAD", "--"} + batch, nullptr,
               nullptr);
    }
}

bool deriveFromRefs(const QString &dir, const QString &base, const QString &head,
                    QString *patch, QString *commits)
{
    if (dir.isEmpty() || base.isEmpty() || head.isEmpty() ||
        head.contains(QLatin1Char(':')))
        return false;
    if (!runGit(dir, {"rev-parse", "--verify", "--quiet", base + "^{commit}"}) ||
        !runGit(dir, {"rev-parse", "--verify", "--quiet", head + "^{commit}"}))
        return false;
    if (patch) {
        QByteArray diff;
        runGit(dir, {"diff", "--binary", base + "..." + head}, &diff);
        *patch = QString::fromUtf8(diff);
    }
    if (commits) {
        QByteArray mbox;
        runGit(dir, {"format-patch", "--binary", "--stdout", base + ".." + head},
               &mbox);
        *commits = QString::fromUtf8(mbox);
    }
    return true;
}

bool validCommitOid(const QString &oid)
{
    if (oid.size() != 40 && oid.size() != 64)
        return false;
    for (const QChar ch : oid) {
        const ushort code = ch.unicode();
        if (!((code >= '0' && code <= '9') ||
              (code >= 'a' && code <= 'f') ||
              (code >= 'A' && code <= 'F'))) {
            return false;
        }
    }
    return true;
}

QString resolvedCommitOid(const QString &dir, const QString &ref)
{
    if (dir.isEmpty() || ref.isEmpty())
        return {};
    QByteArray output;
    if (!runGit(dir,
                {"rev-parse", "--verify", "--quiet", "--end-of-options",
                 ref + "^{commit}"},
                &output)) {
        return {};
    }
    const QString oid = QString::fromUtf8(output).trimmed().toLower();
    return validCommitOid(oid) ? oid : QString();
}

// Every file a patch changes in place, paired with the abbreviated blob its
// hunks expect to find there.
//
// A file the patch creates has an all-zero pre-image and so says nothing about
// which commit the patch was cut from; neither does a pure rename or a binary
// hunk, which record no `--- a/<path>` line to name a pre-image by. A quoted
// path (git escapes the non-ASCII ones) is skipped rather than unescaped here:
// leaving a file out only narrows what the search below can check, while
// guessing at the escaping would compare the wrong file.
QMap<QString, QString> patchPreimageBlobs(const QString &patch)
{
    QMap<QString, QString> blobs;
    QString blob;
    QString path;
    const auto flush = [&blobs, &blob, &path] {
        if (!blob.isEmpty() && !path.isEmpty())
            blobs.insert(path, blob);
        blob.clear();
        path.clear();
    };
    const auto abbreviatedBlob = [](const QString &text) {
        if (text.size() < 7 || text.size() > 64)
            return QString();
        bool allZero = true;
        for (const QChar ch : text) {
            const ushort code = ch.unicode();
            if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f') ||
                  (code >= 'A' && code <= 'F'))) {
                return QString();
            }
            allZero = allZero && code == '0';
        }
        return allZero ? QString() : text.toLower();
    };
    const QStringList lines = patch.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.endsWith(QLatin1Char('\r'))
                                 ? raw.chopped(1)
                                 : raw;
        if (line.startsWith(QLatin1String("diff --git "))) {
            flush();
        } else if (blob.isEmpty() && line.startsWith(QLatin1String("index "))) {
            blob = abbreviatedBlob(line.mid(6)
                                       .section(QLatin1String(".."), 0, 0)
                                       .trimmed());
        } else if (path.isEmpty() && line.startsWith(QLatin1String("--- a/"))) {
            const QString candidate = line.mid(6).trimmed();
            if (!candidate.isEmpty() && !candidate.startsWith(QLatin1Char('"')))
                path = candidate;
        }
    }
    flush();
    return blobs;
}

// The commit a stored patch was cut from, found in `baseRef`'s history.
//
// A patch that crossed the relay carries no base pointer of its own — only
// pulls opened on this node record `creationBaseOid` — so replaying it can
// only guess, and the base branch's current tip is the guess that fails for
// exactly the pulls "Fix" exists for. The patch does describe what it was cut
// from, though: each `index <pre>..<post>` line names the blob its hunks
// expect, and a commit of the base branch whose tree holds every one of those
// blobs *is* a base the author could have had. The patch applies there
// cleanly, and the branch that comes out then conflicts with the base tip
// exactly as the pull request itself does — which is the state an agent asked
// to resolve those conflicts needs to be handed.
//
// Only commits that touched those files can change whether they match, so the
// walk visits just those, newest first, and stops at the first tree that fits.
QString patchPreimageBaseCommit(const QString &dir, const QString &patch,
                                const QString &baseRef)
{
    const QMap<QString, QString> wanted = patchPreimageBlobs(patch);
    if (dir.isEmpty() || wanted.isEmpty() || baseRef.isEmpty())
        return QString();
    const QStringList paths = wanted.keys();

    // Bounded because this runs on the UI thread when "Fix" materializes a
    // pull: a base older than the last few hundred edits of the files it
    // touches is a patch nobody can replay usefully anyway.
    QByteArray revs;
    if (!runGit(dir,
                QStringList{"rev-list", "--max-count=200", "--end-of-options",
                            baseRef, "--"} +
                    paths,
                &revs)) {
        return QString();
    }
    // One `cat-file --batch-check` names the object at every candidate/path
    // pair, instead of an ls-tree process per candidate commit. Every query
    // line answers with exactly one line: the object id, or "<name> missing".
    QStringList checked;
    QByteArray query;
    const QList<QByteArray> candidates = revs.split('\n');
    for (const QByteArray &rawCandidate : candidates) {
        const QString candidate =
            QString::fromUtf8(rawCandidate).trimmed().toLower();
        if (!validCommitOid(candidate))
            continue;
        checked.append(candidate);
        for (const QString &path : paths)
            query += (candidate + QLatin1Char(':') + path).toUtf8() + '\n';
    }
    if (checked.isEmpty())
        return QString();
    QByteArray answers;
    if (!runGitWithInput(dir, {"cat-file", "--batch-check=%(objectname)"}, query,
                         &answers)) {
        return QString();
    }
    const QList<QByteArray> lines = answers.split('\n');
    for (qsizetype i = 0; i < checked.size(); ++i) {
        bool fits = true;
        for (qsizetype j = 0; fits && j < paths.size(); ++j) {
            const qsizetype at = i * paths.size() + j;
            const QByteArray line = at < lines.size() ? lines.at(at) : QByteArray();
            fits = !line.isEmpty() && !line.contains(' ') &&
                   QString::fromUtf8(line).toLower().startsWith(
                       wanted.value(paths.at(j)));
        }
        if (fits)
            return checked.at(i);
    }
    return QString();
}

bool deriveFromImmutableOids(const QString &dir, const QString &baseOid,
                             const QString &headOid, QString *patch,
                             QString *commits,
                             StrictGitReadInternal::Reader *strictReader = nullptr)
{
    if (dir.isEmpty() || !validCommitOid(baseOid) ||
        !validCommitOid(headOid)) {
        return false;
    }

    auto readGit = [&](const QStringList &args, QByteArray *output) {
        return strictReader ? strictReader->run(dir, args, output)
                            : runGit(dir, args, output);
    };
    QByteArray baseOutput;
    QByteArray headOutput;
    if (!readGit({"rev-parse", "--verify", "--quiet", "--end-of-options",
                  baseOid + "^{commit}"},
                 &baseOutput) ||
        !readGit({"rev-parse", "--verify", "--quiet", "--end-of-options",
                  headOid + "^{commit}"},
                 &headOutput)) {
        return false;
    }
    const QString resolvedBase = QString::fromUtf8(baseOutput).trimmed();
    const QString resolvedHead = QString::fromUtf8(headOutput).trimmed();
    if (resolvedBase.compare(baseOid, Qt::CaseInsensitive) != 0 ||
        resolvedHead.compare(headOid, Qt::CaseInsensitive) != 0) {
        return false;
    }

    QByteArray diff;
    QByteArray mbox;
    if (!readGit({"diff", "--no-ext-diff", "--no-textconv", "--binary",
                  baseOid + "..." + headOid},
                 &diff) ||
        !readGit({"format-patch", "--binary", "--stdout",
                  baseOid + ".." + headOid},
                 &mbox)) {
        return false;
    }
    if (patch)
        *patch = QString::fromUtf8(diff);
    if (commits)
        *commits = QString::fromUtf8(mbox);
    return true;
}

QString reachableHeadCommit(const QString &workTree, const QString &mirror,
                            const QString &head)
{
    if (workTree.isEmpty() || head.isEmpty() || head.contains(QLatin1Char(':')))
        return QString();
    QByteArray sha;
    if (runGit(workTree, {"rev-parse", "--verify", "--quiet", head + "^{commit}"},
               &sha) &&
        !sha.trimmed().isEmpty())
        return QString::fromUtf8(sha).trimmed();
    if (!mirror.isEmpty() &&
        runGit(workTree, {"fetch", "--no-tags", "--quiet", mirror,
                          "refs/heads/" + head})) {
        if (runGit(workTree,
                   {"rev-parse", "--verify", "--quiet", "FETCH_HEAD^{commit}"},
                   &sha) &&
            !sha.trimmed().isEmpty())
            return QString::fromUtf8(sha).trimmed();
    }
    return QString();
}

bool releaseWorktreeHoldingBranch(const QString &dir, const QString &branch)
{
    if (branch.trimmed().isEmpty())
        return false;
    QByteArray out;
    if (!runGit(dir, {"worktree", "list", "--porcelain"}, &out, nullptr))
        return false;
    const QString want = QStringLiteral("refs/heads/%1").arg(branch);
    QString currentPath;
    QString held;
    const QList<QByteArray> lines = out.split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QLatin1String("worktree ")))
            currentPath = line.mid(QStringLiteral("worktree ").size()).trimmed();
        else if (line.startsWith(QLatin1String("branch ")) &&
                 line.mid(QStringLiteral("branch ").size()).trimmed() == want &&
                 !currentPath.isEmpty() &&
                 QDir(currentPath).absolutePath() != QDir(dir).absolutePath()) {
            held = currentPath;
            break;
        }
    }
    if (held.isEmpty())
        return false;
    if (!runGit(dir, {"worktree", "remove", held}, nullptr, nullptr))
        return false;
    runGit(dir, {"worktree", "prune"}, nullptr, nullptr);
    return true;
}

QStringList parseApplyConflicts(const QString &text)
{
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral("^error: patch failed: (.+):\\d+$")),
        QRegularExpression(QStringLiteral(
            "^error: (.+): (?:patch does not apply|does not (?:exist|match) in index)$")),
        QRegularExpression(QStringLiteral("^Applied patch to '(.+)' with conflicts\\.$")),
    };
    QStringList files;
    const QStringList lines = text.split('\n');
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        for (const QRegularExpression &re : patterns) {
            const QRegularExpressionMatch m = re.match(trimmed);
            if (m.hasMatch()) {
                if (!files.contains(m.captured(1)))
                    files << m.captured(1);
                break;
            }
        }
    }
    return files;
}

bool writeTextFile(const QString &path, const QString &text, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    file.write(text.toUtf8());
    return true;
}

struct FrontMatter {
    QHash<QString, QString> values;
    QString body;
    bool valid = false;
    QString get(const QString &key) const { return values.value(key); }
    qint64 num(const QString &key) const { return values.value(key).toLongLong(); }
};

FrontMatter parseFrontMatter(const QByteArray &bytes)
{
    FrontMatter fm;
    const QString text = QString::fromUtf8(bytes);
    const QStringList lines = text.split('\n');
    if (lines.isEmpty() || lines.first().trimmed() != "---") {
        fm.body = text;
        return fm;
    }
    int i = 1;
    for (; i < lines.size() && lines.at(i) != "---"; ++i) {
        const int sep = lines.at(i).indexOf(": ");
        if (sep >= 0)
            fm.values.insert(lines.at(i).left(sep), lines.at(i).mid(sep + 2));
        else if (lines.at(i).endsWith(':'))
            fm.values.insert(lines.at(i).left(lines.at(i).size() - 1), QString());
    }
    if (i >= lines.size())
        return fm;
    QString body = lines.mid(i + 1).join('\n');
    while (body.startsWith('\n'))
        body.remove(0, 1);
    while (body.endsWith('\n'))
        body.chop(1);
    fm.body = body;
    fm.valid = true;
    return fm;
}

const QRegularExpression &eventFileRe()
{
    static const QRegularExpression re(QStringLiteral("^\\d{4}-[a-z-]+\\.md$"));
    return re;
}

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString stripEdgeNewlines(QString s)
{
    while (s.startsWith('\n'))
        s.remove(0, 1);
    while (s.endsWith('\n'))
        s.chop(1);
    return s;
}

QString encodeFrontMatterBlob(const QString &s)
{
    if (s.isEmpty())
        return QString();
    return QString::fromLatin1(
        s.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                            QByteArray::OmitTrailingEquals));
}

QString decodeFrontMatterBlob(const QString &s)
{
    if (s.isEmpty())
        return QString();
    return QString::fromUtf8(QByteArray::fromBase64(
        s.toLatin1(), QByteArray::Base64UrlEncoding |
                         QByteArray::OmitTrailingEquals));
}

PullEvent eventFromFrontMatter(const FrontMatter &fm)
{
    PullEvent ev;
    ev.type = fm.get("type");
    ev.id = fm.get("id");
    ev.author = fm.get("author");
    ev.authorName = fm.get("authorName");
    ev.ts = fm.num("ts");
    ev.state = fm.get("state");
    ev.path = fm.get("path");
    ev.side = fm.get("side");
    ev.line = int(fm.num("line"));
    ev.threadId = fm.get("threadId");
    ev.parentId = fm.get("parentId");
    ev.lineStart = int(fm.num("lineStart"));
    ev.lineEnd = int(fm.num("lineEnd"));
    ev.suggestionPatch = decodeFrontMatterBlob(fm.get("suggestionPatchB64"));
    ev.targetPath = fm.get("targetPath");
    ev.appliedCommit = fm.get("appliedCommit");
    ev.patchset = fm.get("patchset");
    ev.anchorLines = decodeFrontMatterBlob(fm.get("anchorLinesB64"));
    ev.sig = fm.get("sig");
    ev.body = fm.body;
    return ev;
}

bool removeOriginalRefs(const QString &workTree, QString *error)
{
    QByteArray refs;
    QString err;
    if (!runGit(workTree, {"for-each-ref", "--format=%(refname)", "refs/original"},
                &refs, &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git for-each-ref failed: ") + err;
        return false;
    }
    // One `update-ref --stdin` transaction deletes every backup ref, instead of
    // a git process per branch and tag the rewrite touched.
    QByteArray deletions;
    const QList<QByteArray> lines = refs.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray ref = line.trimmed();
        if (!ref.isEmpty())
            deletions += "delete " + ref + '\n';
    }
    if (deletions.isEmpty())
        return true;
    if (!runGitWithInput(workTree, {"update-ref", "--stdin"}, deletions, nullptr,
                         &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git update-ref failed: ") + err;
        return false;
    }
    return true;
}

// A directory has to be owner-writable and owner-executable before anything
// inside it can be unlinked, and an aborted rewrite can leave one that is
// neither. Restore just enough permission to delete the tree.
void restoreOwnerAccess(const QString &path)
{
    const QFileInfo info(path);
    QFile::Permissions wanted = QFile::ReadOwner | QFile::WriteOwner;
    if (info.isDir())
        wanted |= QFile::ExeOwner;
    QFile::setPermissions(path, QFile::permissions(path) | wanted);
    if (info.isSymLink() || !info.isDir())
        return;
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                 QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries)
        restoreOwnerAccess(entry.absoluteFilePath());
}

// QTemporaryDir's own cleanup is a single recursive-removal pass, and
// it announces defeat with "QTemporaryDir: Unable to remove ... most likely due
// to the presence of read-only files". The leftovers of a rewrite that timed
// out are exactly what that pass loses to, so delete the scratch tree here
// instead: fix up permissions and retry, because a git child that outlived the
// kill can still recreate a file underneath the walk.
// code-health: allow(dir-walk-in-loop) retry is bounded to 4 attempts, and the first removeTree() normally succeeds
void removeScratchTree(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    const QString parent = QFileInfo(path).absolutePath();
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (forkmesh::safedelete::removeTree(path, parent))
            return;
        QThread::msleep(25);
        restoreOwnerAccess(path);
    }
    qWarning() << "PullStore: could not delete the git rewrite scratch directory"
               << QDir::toNativeSeparators(path);
}

// filter-branch defaults to a fixed .git-rewrite folder relative to the
// worktree. The pull ledger is a persistent linked checkout, so that folder
// can be removed by a concurrent cleanup (or be a user's unrelated file)
// between filter-branch creating it and reading backup-refs. Give every
// rewrite an absolute, private directory instead.
bool runGitFilterBranch(const QString &workTree, const QStringList &args,
                        QByteArray *output, QString *error)
{
    QTemporaryDir scratch(QDir::tempPath() +
                          QStringLiteral("/forkmesh-filter-branch-XXXXXX"));
    if (!scratch.isValid()) {
        if (error)
            *error = QStringLiteral("could not create a private rewrite directory");
        return false;
    }
    // Hand git a sub-path rather than the scratch directory itself:
    // "filter-branch --force" starts by deleting its -d argument and recreating
    // it, so the two would take turns owning the same inode. Cleaning up by
    // hand also keeps QTemporaryDir from warning about what it cannot remove.
    scratch.setAutoRemove(false);
    const QString scratchPath = scratch.path();
    const QString rewritePath = scratch.filePath(QStringLiteral("rewrite"));
    const bool ok =
        runGit(workTree,
               QStringList{"filter-branch", "--force", "-d", rewritePath} + args,
               output, error, kGitRewriteTimeoutMs);
    removeScratchTree(scratchPath);
    return ok;
}

bool rewritePullHistoryWithoutPaths(const QString &workTree, int number,
                                    const QStringList &relPaths,
                                    QString *error)
{
    if (relPaths.isEmpty())
        return true;

    const QString pullsRef = QStringLiteral("refs/heads/forkmesh/pulls");
    QStringList refsToRewrite{pullsRef};
    QString err;
    // Metadata history is isolated per PR. Never rewrite another pull's ref
    // while deleting or scrubbing this one: that was the shared-ledger failure
    // mode this branch layout is specifically meant to remove.
    const QString branchRef = pullMetadataBranchRef(number);
    const QString metadataRef =
        QStringLiteral("refs/pr/%1/metadata").arg(number);
    if (runGit(workTree, {"rev-parse", "--verify", "-q", branchRef + "^{commit}"}))
        refsToRewrite << branchRef;
    if (runGit(workTree,
               {"rev-parse", "--verify", "-q", metadataRef + "^{commit}"}))
        refsToRewrite << metadataRef;

    QStringList historyArgs{"rev-list"};
    historyArgs << refsToRewrite << QStringLiteral("--") << relPaths;
    QByteArray history;
    if (!runGit(workTree, historyArgs, &history, &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git rev-list failed: ") + err;
        return false;
    }
    if (history.trimmed().isEmpty())
        return true;

    const QString indexFilter =
        QStringLiteral("git rm -r --cached --ignore-unmatch -- %1")
            .arg(relPaths.join(QLatin1Char(' ')));
    QStringList filterArgs{"--index-filter", indexFilter, "--prune-empty", "--"};
    filterArgs << refsToRewrite;
    QByteArray out;
    if (!runGitFilterBranch(workTree, filterArgs, &out, &err)) {
        const QString stdoutText = QString::fromUtf8(out);
        if (!err.contains(QStringLiteral("Not a valid object name HEAD")) &&
            !stdoutText.contains(QStringLiteral("was deleted"))) {
            if (error)
                *error = QStringLiteral("git history rewrite failed: ") + err;
            return false;
        }
    }

    if (!runGit(workTree, {"rev-parse", "--verify", "HEAD"}, nullptr, nullptr) &&
        !runGit(workTree,
                {"commit", "--allow-empty", "-m", QStringLiteral("Initial commit")},
                nullptr, &err)) {
        if (error)
            *error = QStringLiteral("git commit failed: ") + err;
        return false;
    }
    if (!removeOriginalRefs(workTree, error))
        return false;
    if (!runGit(workTree,
                {"reflog", "expire", "--expire=now",
                 "--expire-unreachable=now", "--all"},
                nullptr, &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git reflog expire failed: ") + err;
        return false;
    }
    if (!runGit(workTree, {"gc", "--prune=now"}, nullptr, &err,
                kGitRewriteTimeoutMs)) {
        if (error)
            *error = QStringLiteral("git gc failed: ") + err;
        return false;
    }
    return true;
}

bool hasUnrelatedTrackedChanges(const QString &workTree, const QString &relPath,
                                QString *error)
{
    QByteArray status;
    QString err;
    if (!runGit(workTree, {"status", "--porcelain", "--untracked-files=no"}, &status,
                &err)) {
        if (error)
            *error = QStringLiteral("git status failed: ") + err;
        return true;
    }
    const QString prefix = relPath + "/";
    const QList<QByteArray> lines = status.split('\n');
    for (const QByteArray &line : lines) {
        if (line.size() < 4)
            continue;
        const QString path = QString::fromUtf8(line.mid(3)).trimmed();
        if (path != relPath && !path.startsWith(prefix)) {
            if (error) {
                *error = QStringLiteral("Commit or stash unrelated tracked changes "
                                        "before deleting a pull request.");
            }
            return true;
        }
    }
    return false;
}

QString diffHeaderPath(const QString &diffLine)
{
    return diffLine.section(QStringLiteral(" b/"), 1);
}

bool bodyHasFileDiff(const QString &body)
{
    const QStringList lines = body.split('\n');
    for (const QString &line : lines)
        if (line.startsWith(QLatin1String("diff --git ")))
            return true;
    return false;
}

// Drop the unified-diff section(s) for `relPath` from a single diff body (one
// with no email framing — a raw `git diff`, or an mbox message with its trailing
// signature already split off). A section runs from its "diff --git" header up to
// the next header or the end. Sets *removed when at least one section matched.
QString removeFileSectionsFromDiff(const QString &body, const QString &relPath,
                                   bool *removed)
{
    const QStringList lines = body.split('\n');
    QStringList kept;
    kept.reserve(lines.size());
    bool skipping = false;
    bool any = false;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("diff --git "))) {
            skipping = (diffHeaderPath(line) == relPath);
            if (skipping)
                any = true;
        }
        if (!skipping)
            kept << line;
    }
    if (removed)
        *removed = any;
    return kept.join('\n');
}

// Split a format-patch message at its "-- \n<version>" signature, which is always
// the final such marker (a removed line whose content is "- " would render as
// "-- " inside a hunk, so we match from the end to land on the real one). Returns
// the body before it and the trailer from it onward, so reassembling as
// body + "\n" + trailer reproduces the message verbatim.
void splitMboxSignature(const QString &message, QString *body, QString *trailer)
{
    const QStringList lines = message.split('\n');
    int sig = -1;
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (lines.at(i) == QLatin1String("-- ")) {
            sig = i;
            break;
        }
    }
    if (sig < 0) {
        *body = message;
        trailer->clear();
        return;
    }
    *body = QStringList(lines.mid(0, sig)).join('\n');
    *trailer = QStringList(lines.mid(sig)).join('\n');
}

QString stripDiffstat(const QString &message)
{
    const QStringList lines = message.split('\n');
    int firstDiff = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QLatin1String("diff --git "))) {
            firstDiff = i;
            break;
        }
    }
    if (firstDiff < 0)
        return message;
    int cut = -1;
    for (int i = firstDiff - 1; i >= 0; --i) {
        if (lines.at(i) == QLatin1String("---")) {
            cut = i;
            break;
        }
    }
    if (cut < 0)
        return message; // no format-patch cut; nothing recognizable to drop
    QStringList kept = lines.mid(0, cut + 1);
    int i = cut + 1;
    while (i < firstDiff && (lines.at(i).isEmpty() || lines.at(i).startsWith(' ')))
        ++i;
    kept << QString(); // one blank line between the message and the diff
    for (; i < lines.size(); ++i)
        kept << lines.at(i);
    return kept.join('\n');
}

// Excise the diff for `relPath` from every commit in a format-patch mbox. A
// commit left with no remaining file diffs (it only touched that file) is dropped
// whole so `git am` is never handed an empty patch. Survivors keep their headers
// and signatures untouched. Returns an empty string when nothing is left.
QString removeFileFromMbox(const QString &mbox, const QString &relPath)
{
    static const QRegularExpression boundary(
        QStringLiteral("^From [0-9a-f]+ Mon Sep 17 00:00:00 2001$"));
    const QStringList lines = mbox.split('\n');
    QList<QStringList> messages;
    for (const QString &line : lines) {
        if (messages.isEmpty() || boundary.match(line).hasMatch())
            messages.append(QStringList{});
        messages.last() << line;
    }

    QStringList out;
    for (const QStringList &msgLines : messages) {
        const QString message = msgLines.join('\n');
        if (msgLines.isEmpty() || !boundary.match(msgLines.first()).hasMatch()) {
            out << message;
            continue;
        }
        QString body, trailer;
        splitMboxSignature(message, &body, &trailer);
        bool removed = false;
        const QString newBody = removeFileSectionsFromDiff(body, relPath, &removed);
        if (!removed) {
            out << message; // this commit doesn't touch the file
            continue;
        }
        if (!bodyHasFileDiff(newBody))
            continue; // the commit only touched the file — drop it entirely
        const QString trimmedBody = stripDiffstat(newBody);
        out << (trailer.isEmpty() ? trimmedBody : trimmedBody + "\n" + trailer);
    }

    const QString result = out.join('\n');
    return result.trimmed().isEmpty() ? QString() : result;
}

QString pullMetadataCompatibilityRef(int number)
{
    return QStringLiteral("refs/pr/%1/metadata").arg(number);
}

QString deletedPullBranchTombstoneRef(int number)
{
    return QStringLiteral("refs/forkmesh/trash/deleted/forkmesh/pull/%1").arg(number);
}

bool mergedPullStatus(const QString &status)
{
    const QString value = status.trimmed().toLower();
    return value == QLatin1String("merged") ||
           value == QLatin1String("closed-merged");
}

bool pullRecordMergedAt(const QString &dir, const QString &rev, int number)
{
    QByteArray text;
    if (!runGit(dir,
                {"cat-file", "blob",
                 rev + QStringLiteral(":.forkmesh/pulls/%1/pull.md").arg(number)},
                &text, nullptr))
        return false;
    return mergedPullStatus(parseFrontMatter(text).get(QStringLiteral("status")));
}

bool claimPullBranchRetirement(const QString &repo, int number,
                               const QString &tip)
{
    static QMutex mutex;
    static QSet<QString> claimed;
    const QMutexLocker locker(&mutex);
    const QString key = repo + QLatin1Char('#') + QString::number(number) +
                        QLatin1Char('#') + tip;
    if (claimed.contains(key))
        return false;
    claimed.insert(key);
    return true;
}

bool retirePullMetadataBranch(const QString &dir, int number, QString keep,
                              const QString &branchTip, QString *error)
{
    if (number <= 0 || !validCommitOid(keep) ||
        (!branchTip.isEmpty() && !validCommitOid(branchTip))) {
        if (error)
            *error = QStringLiteral("Invalid PR #%1 metadata.").arg(number);
        return false;
    }
    const QString metadataRef = pullMetadataCompatibilityRef(number);
    const QString current = resolvedCommitOid(dir, metadataRef);
    if (!current.isEmpty() && current != keep &&
        runGit(dir, {"merge-base", "--is-ancestor", keep, current}))
        keep = current;
    if (!branchTip.isEmpty() && branchTip != keep &&
        !runGit(dir, {"merge-base", "--is-ancestor", branchTip, keep})) {
        if (error)
            *error = QStringLiteral("PR #%1 metadata does not contain its "
                                    "branch tip.")
                         .arg(number);
        return false;
    }
    QString script = QStringLiteral("start\nupdate %1 %2").arg(metadataRef, keep);
    if (!current.isEmpty())
        script += QLatin1Char(' ') + current;
    script += QLatin1Char('\n');
    if (!branchTip.isEmpty()) {
        script += QStringLiteral("update %1 %2\ndelete %3 %2\n")
                      .arg(deletedPullBranchTombstoneRef(number), branchTip,
                           pullMetadataBranchRef(number));
    }
    script += QStringLiteral("prepare\ncommit\n");
    QString err;
    if (!runGitWithInput(dir, {"update-ref", "--stdin"}, script.toUtf8(),
                         nullptr, &err)) {
        if (error)
            *error = QStringLiteral("Could not retire PR #%1 metadata branch: %2")
                         .arg(number)
                         .arg(err);
        return false;
    }
    return true;
}

} // namespace


QJsonObject PullEvent::toJson() const
{
    return {{"type", type},   {"id", id},     {"author", author},
            {"authorName", authorName}, {"ts", double(ts)}, {"body", body},
            {"state", state}, {"path", path}, {"side", side},
            {"line", line},   {"threadId", threadId},
            {"parentId", parentId}, {"lineStart", lineStart},
            {"lineEnd", lineEnd}, {"suggestionPatch", suggestionPatch},
            {"targetPath", targetPath}, {"appliedCommit", appliedCommit},
            {"patchset", patchset}, {"anchorLines", anchorLines},
            {"sig", sig}};
}

PullEvent PullEvent::fromJson(const QJsonObject &obj)
{
    PullEvent ev;
    ev.type = obj.value("type").toString();
    ev.id = obj.value("id").toString();
    ev.author = obj.value("author").toString();
    ev.authorName = obj.value("authorName").toString();
    ev.ts = obj.value("ts").toVariant().toLongLong();
    ev.body = obj.value("body").toString();
    ev.state = obj.value("state").toString();
    ev.path = obj.value("path").toString();
    ev.side = obj.value("side").toString();
    ev.line = obj.value("line").toInt();
    ev.threadId = obj.value("threadId").toString();
    ev.parentId = obj.value("parentId").toString();
    ev.lineStart = obj.value("lineStart").toInt();
    ev.lineEnd = obj.value("lineEnd").toInt();
    ev.suggestionPatch = obj.value("suggestionPatch").toString();
    ev.targetPath = obj.value("targetPath").toString();
    ev.appliedCommit = obj.value("appliedCommit").toString();
    ev.patchset = obj.value("patchset").toString();
    ev.anchorLines = obj.value("anchorLines").toString();
    ev.sig = obj.value("sig").toString();
    return ev;
}

// ---- PullRequest (JSON wire format) ----------------------------------------

QJsonObject PullRequest::toJson() const
{
    return {{"number", number},   {"title", title},   {"description", description},
            {"base", base},       {"head", head},     {"status", status},
            {"ts", double(ts)},   {"author", author}, {"authorName", authorName},
            {"sig", sig},         {"patch", patch},   {"commits", commits},
            {"creationBaseOid", creationBaseOid},
            {"creationHeadOid", creationHeadOid},
            {"client", client}, {"clientSig", clientSig},
            {"aiModel", aiModel},
            {"rewardEstimateLamports", double(rewardEstimateLamports)},
            {"rewardRecipient", rewardRecipient}, {"rewardId", rewardId},
            {"rewardCondition", rewardCondition},
            {"rewardStatus", rewardStatus},
            {"rewardTxSignature", rewardTxSignature}};
}

PullRequest PullRequest::fromJson(const QJsonObject &obj)
{
    PullRequest pr;
    pr.number = obj.value("number").toInt();
    pr.title = obj.value("title").toString();
    pr.description = obj.contains("description")
                         ? obj.value("description").toString()
                         : obj.value("body").toString();
    pr.base = obj.value("base").toString();
    pr.head = obj.value("head").toString();
    pr.status = obj.value("status").toString("open");
    pr.ts = obj.value("ts").toVariant().toLongLong();
    pr.author = obj.value("author").toString();
    pr.authorName = obj.value("authorName").toString();
    pr.sig = obj.value("sig").toString();
    pr.patch = obj.value("patch").toString();
    pr.commits = obj.value("commits").toString();
    pr.creationBaseOid = obj.value("creationBaseOid").toString();
    pr.creationHeadOid = obj.value("creationHeadOid").toString();
    pr.client = obj.value("client").toString();
    pr.clientSig = obj.value("clientSig").toString();
    pr.aiModel = obj.value("aiModel").toString().simplified().left(80);
    pr.rewardEstimateLamports =
        qint64(obj.value("rewardEstimateLamports").toDouble());
    pr.rewardRecipient = obj.value("rewardRecipient").toString();
    pr.rewardId = obj.value("rewardId").toString();
    pr.rewardCondition = obj.value("rewardCondition").toString();
    pr.rewardStatus = obj.value("rewardStatus").toString();
    pr.rewardTxSignature = obj.value("rewardTxSignature").toString();
    PullStore::computeStats(pr);
    return pr;
}

QString PullRequest::reviewSummary() const
{
    QHash<QString, QString> latest;
    for (const PullEvent &ev : events) {
        if (ev.type != QLatin1String("review"))
            continue;
        if (ev.state == QLatin1String("approved") ||
            ev.state == QLatin1String("changes_requested"))
            latest.insert(ev.author, ev.state);
        else
            latest.remove(ev.author); // a plain comment-review clears prior state
    }
    bool approved = false;
    for (auto it = latest.constBegin(); it != latest.constEnd(); ++it) {
        if (it.value() == QLatin1String("changes_requested"))
            return QStringLiteral("changes_requested");
        if (it.value() == QLatin1String("approved"))
            approved = true;
    }
    return approved ? QStringLiteral("approved") : QString();
}

namespace {

// Latest standing review state per reviewer other than the pull request's own
// author. Approvals and change requests stick until that same reviewer posts a
// plain comment-review, which clears their standing.
QHash<QString, QString> latestIndependentReviewStates(const PullRequest &pr)
{
    QHash<QString, QString> latest;
    for (const PullEvent &ev : pr.events) {
        if (ev.type != QLatin1String("review") || ev.author.isEmpty() ||
            ev.author == pr.author)
            continue;
        if (ev.state == QLatin1String("approved") ||
            ev.state == QLatin1String("changes_requested"))
            latest.insert(ev.author, ev.state);
        else
            latest.remove(ev.author);
    }
    return latest;
}

} // namespace

int PullRequest::independentApprovalCount() const
{
    const QHash<QString, QString> latest = latestIndependentReviewStates(*this);
    int approvals = 0;
    for (auto it = latest.constBegin(); it != latest.constEnd(); ++it) {
        if (it.value() == QLatin1String("changes_requested"))
            return 0;
        if (it.value() == QLatin1String("approved"))
            ++approvals;
    }
    return approvals;
}

QStringList PullRequest::independentChangesRequestedBy() const
{
    const QHash<QString, QString> latest = latestIndependentReviewStates(*this);
    QHash<QString, QString> names; // signer pubkey -> most recent display name
    for (const PullEvent &ev : events) {
        if (ev.type == QLatin1String("review") && !ev.author.isEmpty() &&
            !ev.authorName.isEmpty())
            names.insert(ev.author, ev.authorName);
    }
    QStringList blocking;
    for (auto it = latest.constBegin(); it != latest.constEnd(); ++it) {
        if (it.value() != QLatin1String("changes_requested"))
            continue;
        const QString name = names.value(it.key());
        blocking << (name.isEmpty() ? it.key().left(10) : name);
    }
    blocking.sort(Qt::CaseInsensitive); // QHash iteration order is not stable
    return blocking;
}

bool PullRequest::hasIndependentChangesRequested() const
{
    return !independentChangesRequestedBy().isEmpty();
}

bool PullRequest::independentReviewGateSatisfied() const
{
    return !hasIndependentChangesRequested() &&
           independentApprovalCount() > 0;
}

namespace PullMergePolicy {

bool repositoryOwnedByAccount(const QString &repositoryOwner,
                              const QString &account,
                              const QStringList &linkedNodes)
{
    const QString wanted = repositoryOwner.trimmed();
    if (wanted.isEmpty())
        return false;
    if (!account.trimmed().isEmpty() &&
        wanted.compare(account.trimmed(), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return std::any_of(
        linkedNodes.cbegin(), linkedNodes.cend(),
        [&wanted](const QString &node) {
            return !node.trimmed().isEmpty() &&
                   wanted.compare(node.trimmed(), Qt::CaseInsensitive) == 0;
        });
}

bool repositoryOwnedByOrganizationOwner(const QString &repositoryOwner,
                                        const QJsonArray &organizations)
{
    const QString wanted = repositoryOwner.trimmed();
    if (wanted.isEmpty())
        return false;
    for (const QJsonValue &value : organizations) {
        const QJsonObject organization = value.toObject();
        if (organization.value(QStringLiteral("name")).toString().trimmed()
                .compare(wanted, Qt::CaseInsensitive) == 0 &&
            organization.value(QStringLiteral("role")).toString().trimmed()
                .compare(QLatin1String("owner"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool reviewGateBlocks(bool requirePeerReview, bool reviewReady,
                      bool repositoryOwner)
{
    return requirePeerReview && !reviewReady && !repositoryOwner;
}

bool ownerOverrideRequired(bool requirePeerReview, bool reviewReady,
                           bool repositoryOwner)
{
    return requirePeerReview && !reviewReady && repositoryOwner;
}

} // namespace PullMergePolicy

namespace PullAuthorTrust {

QString identityOf(const PullRequest &pr)
{
    const QString key = pr.author.trimmed();
    if (!key.isEmpty())
        return key;
    return pr.authorName.trimmed().toLower();
}

bool isEstablished(const PullRequest &pr, const QList<PullRequest> &history,
                   const QStringList &trusted)
{
    const QString identity = identityOf(pr);
    if (identity.isEmpty())
        return false; // an unattributable submission is never established
    // Only the signer key is matched for a pull that carries one: `authorName`
    // is chosen by whoever submitted it, so trusting a name would let a stranger
    // sign as themselves while calling themselves the repository owner. A pull
    // with no key at all never crossed the mesh — it was authored on this node —
    // so there its name is all there is, and it is as good as the name itself.
    const bool locallyAuthored = pr.author.trimmed().isEmpty();
    for (const QString &entry : trusted) {
        const QString candidate = entry.trimmed();
        if (candidate.isEmpty())
            continue;
        if (candidate == identity)
            return true;
        if (locallyAuthored &&
            QString::compare(candidate, pr.authorName.trimmed(),
                             Qt::CaseInsensitive) == 0)
            return true;
    }
    for (const PullRequest &other : history) {
        // Their *own* pull request proves nothing, and neither does an open one:
        // only work the maintainer already merged makes an author established.
        if (other.number == pr.number ||
            other.status != QLatin1String("merged") ||
            identityOf(other) != identity)
            continue;
        return true;
    }
    return false;
}

int pullOrdinal(const PullRequest &pr, const QList<PullRequest> &history)
{
    const QString identity = identityOf(pr);
    if (identity.isEmpty())
        return 1;
    int earlier = 0;
    for (const PullRequest &other : history) {
        if (other.number > 0 && other.number < pr.number &&
            identityOf(other) == identity)
            ++earlier;
    }
    return earlier + 1;
}

QString ordinal(int n)
{
    const int lastTwo = n % 100;
    const int last = n % 10;
    const char *suffix = "th";
    if (lastTwo < 11 || lastTwo > 13) {
        if (last == 1)
            suffix = "st";
        else if (last == 2)
            suffix = "nd";
        else if (last == 3)
            suffix = "rd";
    }
    return QString::number(n) + QLatin1String(suffix);
}

} // namespace PullAuthorTrust

namespace PullReward {

QString amountText(qint64 lamports)
{
    if (lamports <= 0)
        return QString();
    QString fraction = QString::number(lamports % 1000000000LL)
                           .rightJustified(9, QLatin1Char('0'));
    while (fraction.endsWith(QLatin1Char('0')) && fraction.size() > 1)
        fraction.chop(1);
    return QStringLiteral("%1.%2 SOL")
        .arg(lamports / 1000000000LL)
        .arg(fraction);
}

QString statusText(const PullRequest &pr)
{
    if (pr.rewardEstimateLamports <= 0 && pr.rewardId.isEmpty())
        return QString();
    const QString status = pr.rewardStatus.trimmed();
    if (status == QLatin1String("pending_merge"))
        return QStringLiteral("pays on merge");
    if (status == QLatin1String("pending_wallet"))
        return QStringLiteral("awaiting the recipient's wallet");
    if (status == QLatin1String("ready"))
        return QStringLiteral("ready to pay");
    if (status == QLatin1String("submitted"))
        return QStringLiteral("payment submitted");
    if (status == QLatin1String("paid"))
        return QStringLiteral("paid");
    if (status == QLatin1String("cancelled"))
        return QStringLiteral("cancelled");
    // An estimate with no recorded state is still an estimate the reader
    // should see; the condition says what it is waiting for.
    return pr.rewardCondition == QLatin1String("merged")
               ? QStringLiteral("pays on merge")
               : QStringLiteral("estimated");
}

bool advanceForMerge(PullRequest &pr)
{
    if (pr.rewardStatus.trimmed() != QLatin1String("pending_merge"))
        return false;
    // An estimate parked in pending_merge *is* the merge condition; a record
    // hand-authored without the condition line still means the same thing.
    // Anything explicitly conditioned on something else is left alone.
    if (!pr.rewardCondition.trimmed().isEmpty() &&
        pr.rewardCondition.trimmed() != QLatin1String("merged"))
        return false;
    pr.rewardStatus = QStringLiteral("pending_wallet");
    return true;
}

} // namespace PullReward

PullStore::PullStore(QString workTreePath, QString mirrorPath,
                     const ForkMeshIdentity *identity, QString authorName)
    : m_workTree(std::move(workTreePath)), m_mirror(std::move(mirrorPath)),
      m_identity(identity), m_authorName(std::move(authorName))
{
}

namespace {

bool runPullHeadGit(const QString &dir, const QStringList &args)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *key : {"GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_AUTHOR_DATE",
                            "GIT_COMMITTER_DATE"})
        env.remove(QString::fromLatin1(key));
    env.insert(QStringLiteral("GIT_COMMITTER_NAME"), QStringLiteral("ForkMesh"));
    env.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
               QStringLiteral("pulls@users.noreply.forkmesh.com"));
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(QStringLiteral("git"),
                  QStringList{"-C", dir, "-c", "core.hooksPath=/dev/null"} + args);
    const QCoreApplication *app = QCoreApplication::instance();
    const bool onGuiThread = app && QThread::currentThread() == app->thread();
    if (!waitForFinishedKeepAlive(process, kGitTimeoutMs, onGuiThread))
        return false;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QString replayPullSeries(const QString &workTree, int number, const QString &baseOid,
                         const QString &series)
{
    const QString stamp = QStringLiteral("%1-%2-%3")
                              .arg(number)
                              .arg(QDateTime::currentMSecsSinceEpoch())
                              .arg(baseOid.left(8));
    const QString scratchDir =
        QDir::temp().filePath(QStringLiteral("forkmesh-pr-ref-") + stamp);
    if (!runPullHeadGit(workTree, {"worktree", "add", "--detach", scratchDir, baseOid}))
        return QString();
    const QString mboxPath =
        QDir::temp().filePath(QStringLiteral("forkmesh-pr-ref-%1.mbox").arg(stamp));
    QString tipSha;
    if (writeTextFile(mboxPath, series, nullptr) &&
        runPullHeadGit(scratchDir, {"am", "--quiet", "--3way", "--no-gpg-sign",
                                    "--no-message-id", "--no-scissors", "--no-keep-cr",
                                    "--committer-date-is-author-date", mboxPath})) {
        const QString tip = resolvedCommitOid(scratchDir, QStringLiteral("HEAD"));
        if (tip != baseOid.toLower())
            tipSha = tip;
    }
    QFile::remove(mboxPath);
    runGit(scratchDir, {"am", "--abort"}, nullptr, nullptr);
    runGit(workTree, {"worktree", "remove", "--force", scratchDir}, nullptr, nullptr);
    forkmesh::safedelete::removeTree(scratchDir, QDir::tempPath(), nullptr,
                                     forkmesh::safedelete::Allow::OwnWorktree);
    runGit(workTree, {"worktree", "prune"}, nullptr, nullptr);
    return tipSha;
}

QString recordedPullHead(const QString &dir, const PullRequest &pr,
                         const QString &baseTip)
{
    if (baseTip.isEmpty() || !validCommitOid(pr.headOid) ||
        !validCommitOid(pr.headBaseOid))
        return QString();
    const QString head = resolvedCommitOid(dir, pr.headOid);
    const QString onto = resolvedCommitOid(dir, pr.headBaseOid);
    if (head.isEmpty() || onto.isEmpty() || head == onto ||
        !runGit(dir, {"merge-base", "--is-ancestor", onto, baseTip}) ||
        !runGit(dir, {"merge-base", "--is-ancestor", onto, head}))
        return QString();
    return head;
}

} // namespace

QString PullStore::pullBaseTip(const PullRequest &pr) const
{
    const QString base = resolvedCommitOid(m_workTree, pr.base.trimmed());
    return base.isEmpty() ? resolvedCommitOid(m_workTree, QStringLiteral("HEAD")) : base;
}

QString PullStore::pullHeadCommit(const PullRequest &pr) const
{
    const QString head = pr.branchBacked
                             ? reachableHeadCommit(m_workTree, m_mirror, pr.head)
                             : recordedPullHead(m_workTree, pr, pullBaseTip(pr));
    if (!head.isEmpty())
        return head;
    return resolvedCommitOid(m_workTree, QStringLiteral("refs/pr/%1/head").arg(pr.number));
}

bool PullStore::canWrite() const
{
    if (m_workTree.isEmpty() || !m_identity || !m_identity->isValid())
        return false;
    return QFileInfo::exists(m_workTree + "/.git");
}

QString PullStore::pullsDir() const
{
    const QString meta = metaWorkTree();
    return (meta.isEmpty() ? m_workTree : meta) + "/.forkmesh/pulls";
}
QString PullStore::pullDir(int number) const
{
    return pullsDir() + "/" + QString::number(number);
}

namespace {

QString pullMetaCheckoutDir(const QString &workTree)
{
    const QByteArray key = QCryptographicHash::hash(
                                QDir(workTree).absolutePath().toUtf8(),
                                QCryptographicHash::Sha256)
                                .toHex()
                                .left(16);
    return forkmesh::paths::dataPath(QStringLiteral("pull-meta")) + "/" + QString::fromUtf8(key);
}

QString existingPullMetaCheckout(const QString &workTree)
{
    if (workTree.isEmpty())
        return QString();
    if (headBranchFromFile(workTree) == QLatin1String("forkmesh/pulls"))
        return workTree;
    const QString dir = pullMetaCheckoutDir(workTree);
    if (QFileInfo::exists(dir + "/.git") &&
        headBranchFromFile(dir) == QLatin1String("forkmesh/pulls"))
        return dir;
    return QString();
}

} // namespace

QString PullStore::metaWorkTree() const
{
    if (m_workTree.isEmpty())
        return QString();
    if (!m_metaWorkTreeCache.isEmpty())
        return m_metaWorkTreeCache;
    if (headBranchFromFile(m_workTree) == QLatin1String("forkmesh/pulls")) {
        m_metaWorkTreeCache = m_workTree;
        return m_metaWorkTreeCache;
    }
    const QString dir = pullMetaCheckoutDir(m_workTree);
    // Reuse a checkout linked by a prior run only while it is still a live
    // worktree sitting on the pull ledger branch: a pruned or repointed one
    // reads as an empty ledger instead of failing, which looks exactly like
    // "this repository has no pull requests".
    if (QFileInfo::exists(dir + "/.git") &&
        headBranchFromFile(dir) == QLatin1String("forkmesh/pulls")) {
        if (claimPullMetadataHeal(dir))
            resyncPullMetadataCheckout(dir); // the branch may have moved past it
        QDir().mkpath(dir + "/.forkmesh/pulls");
        m_metaWorkTreeCache = dir;
        return m_metaWorkTreeCache;
    }
    QDir().mkpath(QFileInfo(dir).absolutePath());
    const bool branchExists = runGit(
        m_workTree, {"rev-parse", "--verify", "-q", "refs/heads/forkmesh/pulls^{commit}"});
    auto link = [&] {
        return branchExists
                   ? runGit(m_workTree, {"worktree", "add", dir, "forkmesh/pulls"})
                   : runGit(m_workTree,
                           {"worktree", "add", "-b", "forkmesh/pulls", dir, "HEAD"});
    };
    if (!link()) {
        forkmesh::safedelete::removeTree(dir, forkmesh::paths::dataPath(QStringLiteral("pull-meta")),
                                         nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
        runGit(m_workTree, {"worktree", "prune"}, nullptr, nullptr);
        if (!link())
            return QString();
    }
    QDir().mkpath(dir + "/.forkmesh/pulls");
    claimPullMetadataHeal(dir); // a checkout we just made is current by definition
    m_metaWorkTreeCache = dir;
    return m_metaWorkTreeCache;
}

bool PullStore::materializePullMetadataRef(int number, QString *error) const
{
    if (number <= 0)
        return true;
    const QString meta = metaWorkTree();
    const QString dir = meta.isEmpty() ? m_workTree : meta;
    if (dir.isEmpty())
        return true;

    const QString pullTreePath =
        QStringLiteral("HEAD:.forkmesh/pulls/%1").arg(number);
    if (!runGit(dir, {"cat-file", "-e", pullTreePath}, nullptr, nullptr))
        return true;
    QByteArray pullTree;
    QString err;
    if (!runGit(dir, {"rev-parse", "--verify", pullTreePath},
                &pullTree, &err) || pullTree.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not resolve pull metadata: ") + err;
        return false;
    }

    const QString branchRef = pullMetadataBranchRef(number);
    const QString metadataRef = pullMetadataCompatibilityRef(number);
    const QByteArray oldTip = resolvedCommitOid(dir, branchRef).toUtf8();
    const QByteArray parentTip =
        oldTip.isEmpty() ? resolvedCommitOid(dir, metadataRef).toUtf8() : oldTip;
    const bool merged = pullRecordMergedAt(dir, QStringLiteral("HEAD"), number);

    // A normal list refresh also performs the one-time migration from the
    // legacy aggregate branch. Avoid manufacturing a commit when this PR's
    // independent branch already contains the same snapshot.
    if (!parentTip.isEmpty()) {
        QByteArray oldPullTree;
        if (runGit(dir,
                   {"rev-parse", "--verify",
                    QString::fromUtf8(parentTip) +
                        QStringLiteral(":.forkmesh/pulls/%1").arg(number)},
                   &oldPullTree, nullptr) &&
            oldPullTree.trimmed() == pullTree.trimmed()) {
            if (merged)
                return retirePullMetadataBranch(dir, number,
                                                QString::fromUtf8(parentTip),
                                                QString::fromUtf8(oldTip), error);
            if (oldTip.isEmpty() &&
                !runGit(dir, {"update-ref", branchRef,
                              QString::fromUtf8(parentTip)},
                        nullptr, error))
                return false;
            return runGit(dir, {"update-ref", metadataRef,
                                QString::fromUtf8(parentTip)}, nullptr, error);
        }
    }

    // Build a root tree that contains only .forkmesh/pulls/<number>. Git tree
    // objects make this cheap and avoid a temporary checkout or index.
    QByteArray pullsTree;
    const QByteArray numberEntry =
        "040000 tree " + pullTree.trimmed() + '\t' +
        QByteArray::number(number) + '\n';
    if (!runGitWithInput(dir, {"mktree"}, numberEntry, &pullsTree, &err) ||
        pullsTree.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not build PR metadata tree: ") + err;
        return false;
    }
    QByteArray forkmeshTree;
    if (!runGitWithInput(dir, {"mktree"},
                         "040000 tree " + pullsTree.trimmed() + "\tpulls\n",
                         &forkmeshTree, &err) ||
        forkmeshTree.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not build PR metadata tree: ") + err;
        return false;
    }
    QByteArray rootTree;
    if (!runGitWithInput(dir, {"mktree"},
                         "040000 tree " + forkmeshTree.trimmed() +
                             "\t.forkmesh\n",
                         &rootTree, &err) ||
        rootTree.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not build PR metadata tree: ") + err;
        return false;
    }

    QStringList commitArgs{"commit-tree", QString::fromUtf8(rootTree).trimmed()};
    if (!parentTip.isEmpty())
        commitArgs << "-p" << QString::fromUtf8(parentTip);
    commitArgs << "-m" << QStringLiteral("pull #%1: metadata snapshot").arg(number);
    QByteArray newTip;
    if (!runGit(dir, commitArgs, &newTip, &err) || newTip.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not commit PR metadata: ") + err;
        return false;
    }
    if (merged)
        return retirePullMetadataBranch(dir, number,
                                        QString::fromUtf8(newTip).trimmed(),
                                        QString::fromUtf8(oldTip), error);
    QStringList updateBranch{"update-ref", branchRef,
                             QString::fromUtf8(newTip).trimmed()};
    if (!oldTip.isEmpty())
        updateBranch << QString::fromUtf8(oldTip);
    if (!runGit(dir, updateBranch, nullptr, &err)) {
        if (error)
            *error = QStringLiteral("Could not update PR metadata branch: ") + err;
        return false;
    }
    if (!runGit(dir, {"update-ref", metadataRef,
                      QString::fromUtf8(newTip).trimmed()}, nullptr, &err)) {
        if (error)
            *error = QStringLiteral("Could not attach metadata to PR #%1: %2")
                         .arg(number)
                         .arg(err);
        return false;
    }
    return true;
}

bool PullStore::materializePullRef(const PullRequest &pr, QString *error) const
{
    if (!materializePullMetadataRef(pr.number, error))
        return false;
    // Only a branch-backed PR declares its named head authoritative. A stored-
    // patch PR may retain pr.head as signed provenance even after a file edit
    // changed its content, so resolving that branch here would rematerialize the
    // stale pre-edit tree and silently override the current patch.
    QString sha = pr.branchBacked
                      ? reachableHeadCommit(m_workTree, m_mirror, pr.head)
                      : QString();
    const QString baseTip = pullBaseTip(pr);
    if (sha.isEmpty() && !pr.branchBacked)
        sha = recordedPullHead(m_workTree, pr, baseTip);
    if (sha.isEmpty() && !baseTip.isEmpty()) {
        QStringList baseOids;
        const auto addBase = [this, &baseOids, &baseTip](const QString &ref) {
            const QString oid = resolvedCommitOid(m_workTree, ref.trimmed());
            if (oid.isEmpty() || baseOids.contains(oid) ||
                !runGit(m_workTree, {"merge-base", "--is-ancestor", oid, baseTip}))
                return;
            baseOids << oid;
        };
        addBase(pr.headBaseOid);
        addBase(pr.creationBaseOid);
        addBase(patchPreimageBaseCommit(m_workTree, pr.patch, baseTip));
        addBase(baseTip);
        const QString series = pr.commits.isEmpty() ? syntheticMbox(pr) : pr.commits;
        for (const QString &baseOid : std::as_const(baseOids)) {
            sha = replayPullSeries(m_workTree, pr.number, baseOid, series);
            if (!sha.isEmpty())
                break;
        }
    }
    // An earlier materialization, or the checks runner publishing the head to
    // the served mirror, may already have recorded the commit this pull is
    // made of. That ref alone is not something git can check out an agent
    // worktree from, so adopt it as the numbered branch rather than leaving
    // the pull looking absent while its code sits right here.
    if (sha.isEmpty())
        sha = resolvedCommitOid(m_workTree,
                                QStringLiteral("refs/pr/%1/head").arg(pr.number));
    if (sha.isEmpty() && !m_mirror.isEmpty() &&
        runGit(m_workTree, {"fetch", "--no-tags", "--quiet", m_mirror,
                            QStringLiteral("refs/pr/%1/head").arg(pr.number)})) {
        sha = resolvedCommitOid(m_workTree, QStringLiteral("FETCH_HEAD"));
    }
    if (sha.isEmpty())
        return true; // best-effort: readers fall back to the legacy path
    QString err;
    if (!runGit(m_workTree,
               {"update-ref", QStringLiteral("refs/pr/%1/head").arg(pr.number), sha},
               nullptr, &err)) {
        if (error)
            *error = "Could not record the pull request ref: " + err;
        return false;
    }

    auto advanceBranch = [&](const QString &branchRef) {
        const QString oldSha = resolvedCommitOid(m_workTree, branchRef);
        if (oldSha == sha)
            return true; // repeated materialization is idempotent
        if (!oldSha.isEmpty() &&
            !runGit(m_workTree, {"merge-base", "--is-ancestor", oldSha, sha})) {
            return true; // independently advanced/diverged: preserve the branch
        }
        const QString expected = oldSha.isEmpty()
                                     ? QString(sha.size(), QLatin1Char('0'))
                                     : oldSha;
        if (runGit(m_workTree, {"update-ref", branchRef, sha, expected}, nullptr,
                   &err)) {
            return true;
        }
        const QString racedSha = resolvedCommitOid(m_workTree, branchRef);
        if (racedSha == sha ||
            (!racedSha.isEmpty() &&
             runGit(m_workTree, {"merge-base", "--is-ancestor", sha, racedSha}))) {
            return true;
        }
        return !oldSha.isEmpty() && !racedSha.isEmpty();
    };

    if (!advanceBranch(QStringLiteral("refs/heads/pr/%1").arg(pr.number))) {
        if (error)
            *error = "Could not record the pull request branch: " + err;
        return false;
    }

    // Patch-backed contributions can be stacked. Preserve their signed pr/*
    // head name as a safe alias so the next contribution's signed base resolves
    // locally; the numbered branch remains the stable, collision-free handle.
    // Never let a remote pull choose an arbitrary local branch namespace.
    if (pr.head.startsWith(QLatin1String("pr/")) &&
        runGit(m_workTree, {"check-ref-format", "--branch", pr.head}) &&
        !advanceBranch(QStringLiteral("refs/heads/") + pr.head)) {
        if (error)
            *error = "Could not record the stacked pull request branch: " + err;
        return false;
    }
    return true;
}

PullStore::BranchSource PullStore::ensureLocalBranch(const QString &branch,
                                                     int *pullNumber,
                                                     QString *error) const
{
    if (pullNumber)
        *pullNumber = 0;
    const QString name = branch.trimmed();
    const QString ref = QStringLiteral("refs/heads/") + name;
    if (name.isEmpty() || name.startsWith(QLatin1Char('-')) ||
        !runGit(m_workTree, {"check-ref-format", ref})) {
        if (error)
            *error = QStringLiteral("\"%1\" is not a valid branch name.").arg(branch);
        return BranchSource::Missing;
    }
    if (!resolvedCommitOid(m_workTree, ref).isEmpty())
        return BranchSource::Local;
    const auto adopt = [this, &ref](const QString &sha) {
        if (sha.isEmpty())
            return false;
        if (runGit(m_workTree,
                   {"update-ref", ref, sha, QString(sha.size(), QLatin1Char('0'))}))
            return true;
        return resolvedCommitOid(m_workTree, ref) == sha;
    };

    if (!m_mirror.isEmpty() &&
        runGit(m_workTree, {"fetch", "--no-tags", "--quiet", m_mirror, ref}) &&
        adopt(resolvedCommitOid(m_workTree, QStringLiteral("FETCH_HEAD"))))
        return BranchSource::Mirror;

    QString loadError;
    const QList<PullRequest> pulls = loadAll(&loadError);
    const PullRequest *open = nullptr;
    const PullRequest *closed = nullptr;
    for (const PullRequest &pr : pulls) {
        if (pr.head.trimmed() != name)
            continue;
        const PullRequest *&slot =
            pr.status == QLatin1String("open") ? open : closed;
        if (!slot || pr.number > slot->number)
            slot = &pr;
    }
    if (open) {
        QString err;
        if (!materializePullRef(*open, &err)) {
            if (error)
                *error = QStringLiteral("Pull request #%1 carries %2, but its "
                                        "commits could not be rebuilt: %3")
                             .arg(open->number)
                             .arg(name, err);
            return BranchSource::Missing;
        }
        if (!resolvedCommitOid(m_workTree, ref).isEmpty()) {
            if (pullNumber)
                *pullNumber = open->number;
            return BranchSource::Pull;
        }
        if (adopt(resolvedCommitOid(
                m_workTree, QStringLiteral("refs/pr/%1/head").arg(open->number)))) {
            if (pullNumber)
                *pullNumber = open->number;
            return BranchSource::Pull;
        }
        if (error)
            *error = QStringLiteral("Pull request #%1 carries %2, but its stored "
                                    "commits no longer apply to %3 or the base "
                                    "they were written against. Ask its author to "
                                    "update it from %3.")
                         .arg(open->number)
                         .arg(name, open->base.trimmed().isEmpty()
                                        ? QStringLiteral("the base branch")
                                        : open->base.trimmed());
        return BranchSource::Missing;
    }
    if (error) {
        if (closed)
            *error = QStringLiteral("%1 is not in this checkout or on the mirror, "
                                    "and pull request #%2 that carried it is "
                                    "already %3.")
                         .arg(name)
                         .arg(closed->number)
                         .arg(closed->status);
        else
            *error = QStringLiteral("%1 is not in this checkout, on the mirror, or "
                                    "in any open pull request, so there is nothing "
                                    "to merge. It may already have been merged and "
                                    "deleted; otherwise sync the repository, or ask "
                                    "whoever made the branch to push it again.")
                         .arg(name);
    }
    return BranchSource::Missing;
}

void PullStore::computeStats(PullRequest &pr)
{
    int files = 0, add = 0, del = 0;
    const QStringView patch(pr.patch);
    for (qsizetype start = 0; start <= patch.size();) {
        const qsizetype nl = patch.indexOf(u'\n', start);
        const QStringView line =
            patch.sliced(start, (nl < 0 ? patch.size() : nl) - start);
        if (line.startsWith(QLatin1String("diff --git ")))
            ++files;
        else if (line.startsWith(QLatin1String("+++ ")) ||
                 line.startsWith(QLatin1String("--- "))) {
        } else if (line.startsWith(u'+'))
            ++add;
        else if (line.startsWith(u'-'))
            ++del;
        if (nl < 0)
            break;
        start = nl + 1;
    }
    pr.filesChanged = files;
    pr.additions = add;
    pr.deletions = del;
}

void PullStore::setSigningRepository(const QString &owner, const QString &name)
{
    m_signingRepoKey = forkmesh::eventsigning::repoKey(owner, name);
}

QByteArray PullStore::canonicalString(const PullRequest &pr, const QString &repoKey)
{
    const QChar nul(QChar::Null);
    const QString content =
        pr.title + nul + pr.base + nul + pr.head + nul + pr.patch + nul + pr.commits;
    const QByteArray contentHash =
        QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha256).toHex();
    QByteArray canonical = forkmesh::eventsigning::prefix("pull-event", repoKey);
    canonical += pr.author.toUtf8() + "\n";
    canonical += QByteArray::number(pr.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

QByteArray PullStore::legacyCanonicalString(const PullRequest &pr)
{
    const QChar nul(QChar::Null);
    const QString content =
        pr.title + nul + pr.base + nul + pr.head + nul + pr.patch;
    const QByteArray contentHash =
        QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    QByteArray canonical = "forkmesh-pull-event-v1\n";
    canonical += pr.author.toUtf8() + "\n";
    canonical += QByteArray::number(pr.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

PullRequest PullStore::makeSignedPull(PullRequest pr) const
{
    pr.author = m_identity ? m_identity->publicKey() : QString();
    if (pr.authorName.isEmpty())
        pr.authorName = m_authorName;
    if (pr.ts == 0)
        pr.ts = QDateTime::currentMSecsSinceEpoch();
    pr.sig = m_identity ? m_identity->signData(canonicalString(pr, m_signingRepoKey)) : QString();
    return pr;
}


QString PullStore::contentForSigning(const PullEvent &ev)
{
    const QChar nul(QChar::Null);
    if (ev.type == "comment")
        return ev.body;
    if (ev.type == "review")
        return ev.state + nul + ev.body;
    // Anchored events append the patchset anchor to the signed content. An
    // event without one signs exactly the pre-anchor bytes, so every existing
    // signature (and every verifier that predates anchors) stays valid.
    const QString anchor =
        ev.hasAnchor() ? nul + ev.patchset + nul + ev.anchorLines : QString();
    if (ev.type == "line-comment")
        return ev.path + nul + ev.side + nul + QString::number(ev.line) + nul +
               ev.body + anchor;
    if (ev.type == "thread-comment")
        return ev.threadId + nul + ev.path + nul + ev.side + nul +
               QString::number(ev.lineStart) + nul + QString::number(ev.lineEnd) +
               nul + ev.body + nul + ev.suggestionPatch + anchor;
    if (ev.type == "thread-reply")
        return ev.threadId + nul + ev.parentId + nul + ev.body;
    if (ev.type == "thread-state")
        return ev.threadId + nul + ev.state + nul + ev.body;
    if (ev.type == "suggestion-state")
        return ev.threadId + nul + ev.state + nul + ev.appliedCommit + nul + ev.body;
    if (ev.type == "status")
        return ev.state + nul + ev.body;
    if (ev.type == "delete")
        return ev.body;
    return QString();
}

QByteArray PullStore::canonicalString(int number, const PullEvent &ev,
                                      const QString &repoKey)
{
    const QByteArray contentHash =
        QCryptographicHash::hash(contentForSigning(ev).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    QByteArray canonical = forkmesh::eventsigning::prefix("pull-comment", repoKey);
    canonical += ev.type.toUtf8() + "\n";
    canonical += QByteArray::number(number) + "\n";
    canonical += ev.author.toUtf8() + "\n";
    canonical += QByteArray::number(ev.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

PullEvent PullStore::makeSignedEvent(int number, PullEvent ev) const
{
    if (ev.id.isEmpty())
        ev.id = newId();
    if (ev.type == QLatin1String("thread-comment") && ev.threadId.isEmpty())
        ev.threadId = ev.id;
    ev.author = m_identity ? m_identity->publicKey() : QString();
    if (ev.authorName.isEmpty())
        ev.authorName = m_authorName;
    if (ev.ts == 0)
        ev.ts = QDateTime::currentMSecsSinceEpoch();
    ev.sig = m_identity ? m_identity->signData(canonicalString(number, ev, m_signingRepoKey))
                        : QString();
    return ev;
}

namespace {

QString coAuthorEmails(const QString &messages)
{
    static const QRegularExpression trailer(
        QStringLiteral("^[ \\t]*Co-authored-by:[^<\\n]*<([^<>\\s@,]+@[^<>\\s@,]+)>[ \\t]*$"),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::MultilineOption);
    QStringList emails;
    QRegularExpressionMatchIterator it = trailer.globalMatch(messages);
    while (it.hasNext() && emails.size() < 16) {
        const QString email = it.next().captured(1).toLower();
        if (email.size() <= 254 && !emails.contains(email))
            emails << email;
    }
    emails.sort();
    return emails.join(QLatin1Char(','));
}

QString commitMessages(const QString &workTree, const QString &from,
                       const QString &to)
{
    if (from.isEmpty() || to.isEmpty())
        return QString();
    QByteArray log;
    if (!runGit(workTree, {"log", "--format=%B", from + ".." + to}, &log))
        return QString();
    return QString::fromUtf8(log);
}

QList<PullEvent> readEventFilesIn(const QString &dir)
{
    QList<PullEvent> events;
    QStringList names = QDir(dir).entryList(QDir::Files, QDir::Name);
    names.sort();
    for (const QString &name : names) {
        if (!eventFileRe().match(name).hasMatch())
            continue;
        QFile ef(dir + "/" + name);
        if (ef.open(QIODevice::ReadOnly))
            events.append(eventFromFrontMatter(parseFrontMatter(ef.readAll())));
    }
    return events;
}

int nextEventIndexIn(const QString &dir)
{
    int max = 1; // pull.md is conceptually event 1
    const QStringList names = QDir(dir).entryList(QDir::Files);
    for (const QString &name : names) {
        if (!eventFileRe().match(name).hasMatch())
            continue;
        const int n = name.left(4).toInt();
        if (n > max)
            max = n;
    }
    return max + 1;
}

bool writeEventFileIn(const QString &dir, int index, const PullEvent &ev,
                      QString *error)
{
    QStringList lines;
    lines << "---";
    lines << "type: " + ev.type;
    lines << "id: " + ev.id;
    lines << "author: " + ev.author;
    lines << "authorName: " + ev.authorName;
    lines << "ts: " + QString::number(ev.ts);
    if (ev.type == "review")
        lines << "state: " + ev.state;
    if (ev.type == "line-comment") {
        lines << "path: " + ev.path;
        lines << "side: " + ev.side;
        lines << "line: " + QString::number(ev.line);
    }
    if (!ev.threadId.isEmpty())
        lines << "threadId: " + ev.threadId;
    if (!ev.parentId.isEmpty())
        lines << "parentId: " + ev.parentId;
    if (ev.type == "thread-comment") {
        lines << "path: " + ev.path;
        lines << "side: " + ev.side;
        lines << "lineStart: " + QString::number(ev.lineStart);
        lines << "lineEnd: " + QString::number(ev.lineEnd);
    }
    if (!ev.suggestionPatch.isEmpty())
        lines << "suggestionPatchB64: " + encodeFrontMatterBlob(ev.suggestionPatch);
    if (!ev.targetPath.isEmpty())
        lines << "targetPath: " + ev.targetPath;
    if (!ev.appliedCommit.isEmpty())
        lines << "appliedCommit: " + ev.appliedCommit;
    if (!ev.patchset.isEmpty())
        lines << "patchset: " + ev.patchset;
    if (!ev.anchorLines.isEmpty())
        lines << "anchorLinesB64: " + encodeFrontMatterBlob(ev.anchorLines);
    if ((ev.type == "thread-state" || ev.type == "suggestion-state" ||
         ev.type == "status") &&
        !ev.state.isEmpty())
        lines << "state: " + ev.state;
    lines << "sig: " + ev.sig;
    lines << "---";
    lines << "";
    const QString name =
        QStringLiteral("%1-%2.md").arg(index, 4, 10, QChar('0')).arg(ev.type);
    return writeTextFile(dir + "/" + name,
                         lines.join('\n') + "\n" + ev.body + "\n", error);
}

} // namespace

QList<PullEvent> PullStore::readEvents(int number) const
{
    return readEventFilesIn(pullDir(number));
}

int PullStore::nextEventIndex(int number) const
{
    return nextEventIndexIn(pullDir(number));
}

bool PullStore::writeEventFile(int number, int index, const PullEvent &ev,
                               QString *error) const
{
    return writeEventFileIn(pullDir(number), index, ev, error);
}

bool PullStore::appendEvent(int number, const PullEvent &ev,
                            const QString &commitMsg, QString *error)
{
    if (!QFileInfo::exists(pullDir(number) + "/pull.md")) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (!writeEventFile(number, nextEventIndex(number), ev, error))
        return false;
    return commit(commitMsg, error);
}

bool PullStore::addComment(int number, const QString &body, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "comment";
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);
    return appendEvent(number, ev,
                       QStringLiteral("pull #%1: comment").arg(number), error);
}

bool PullStore::addReview(int number, const QString &state, const QString &body,
                          QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "review";
    ev.state = state;
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);
    return appendEvent(number, ev,
                       QStringLiteral("pull #%1: review (%2)").arg(number).arg(state),
                       error);
}

bool PullStore::addLineComment(int number, const QString &path, const QString &side,
                               int line, const QString &body, QString *error)
{
    return addLineComment(number, path, side, line, body, PullAnchor(), error);
}

bool PullStore::addLineComment(int number, const QString &path, const QString &side,
                               int line, const QString &body,
                               const PullAnchor &anchor, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "line-comment";
    ev.path = path;
    ev.side = side;
    ev.line = line;
    ev.body = stripEdgeNewlines(body);
    ev.patchset = anchor.patchset;
    ev.anchorLines = anchor.anchorLines;
    ev = makeSignedEvent(number, ev);
    return appendEvent(
        number, ev,
        QStringLiteral("pull #%1: comment on %2:%3").arg(number).arg(path).arg(line),
        error);
}

bool PullStore::addThreadComment(int number, const QString &path,
                                 const QString &side, int lineStart, int lineEnd,
                                 const QString &body,
                                 const QString &suggestionPatch, QString *error)
{
    return addThreadComment(number, path, side, lineStart, lineEnd, body,
                            suggestionPatch, PullAnchor(), error);
}

bool PullStore::addThreadComment(int number, const QString &path,
                                 const QString &side, int lineStart, int lineEnd,
                                 const QString &body,
                                 const QString &suggestionPatch,
                                 const PullAnchor &anchor, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "thread-comment";
    ev.path = path;
    ev.side = side;
    ev.lineStart = lineStart;
    ev.lineEnd = lineEnd > 0 ? lineEnd : lineStart;
    ev.body = stripEdgeNewlines(body);
    ev.suggestionPatch = stripEdgeNewlines(suggestionPatch);
    ev.patchset = anchor.patchset;
    ev.anchorLines = anchor.anchorLines;
    ev = makeSignedEvent(number, ev);
    return appendEvent(
        number, ev,
        QStringLiteral("pull #%1: thread on %2:%3")
            .arg(number)
            .arg(path)
            .arg(lineStart),
        error);
}

bool PullStore::addThreadReply(int number, const QString &threadId,
                               const QString &parentId, const QString &body,
                               QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "thread-reply";
    ev.threadId = threadId;
    ev.parentId = parentId;
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);
    return appendEvent(number, ev,
                       QStringLiteral("pull #%1: thread reply").arg(number),
                       error);
}

bool PullStore::setThreadState(int number, const QString &threadId,
                               const QString &state, const QString &body,
                               QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "thread-state";
    ev.threadId = threadId;
    ev.state = state;
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);
    return appendEvent(
        number, ev,
        QStringLiteral("pull #%1: thread %2").arg(number).arg(state), error);
}

bool PullStore::setSuggestionState(int number, const QString &threadId,
                                   const QString &state,
                                   const QString &appliedCommit,
                                   const QString &body, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullEvent ev;
    ev.type = "suggestion-state";
    ev.threadId = threadId;
    ev.state = state;
    ev.appliedCommit = appliedCommit;
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);
    return appendEvent(
        number, ev,
        QStringLiteral("pull #%1: suggestion %2").arg(number).arg(state), error);
}

bool PullStore::applyRemoteEvent(int number, const PullEvent &ev, QString *error)
{
    if (!canWrite())
        return false;
    return appendEvent(
        number, ev,
        QStringLiteral("pull #%1: %2 (from %3)")
            .arg(number)
            .arg(ev.type == "review" ? QStringLiteral("review") : QStringLiteral("comment"),
                 ev.authorName.isEmpty() ? ev.author.left(8) : ev.authorName),
        error);
}

QString PullStore::branchReviewKey(const QString &branch)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(branch.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(24));
}

QString PullStore::branchReviewDir(const QString &branch) const
{
    const QString meta = metaWorkTree();
    if (meta.isEmpty() || branch.isEmpty())
        return QString();
    return meta + "/.forkmesh/branch-reviews/" + branchReviewKey(branch);
}

QList<PullEvent> PullStore::readBranchReviewEvents(const QString &branch) const
{
    if (branch.isEmpty())
        return {};
    if (canWrite()) {
        const QString meta = existingPullMetaCheckout(m_workTree);
        return meta.isEmpty()
                   ? QList<PullEvent>()
                   : readEventFilesIn(meta + "/.forkmesh/branch-reviews/" +
                                      branchReviewKey(branch));
    }
    if (m_mirror.isEmpty())
        return {};
    const QString ref = mirrorRef();
    if (ref.isEmpty())
        return {};
    const QString prefix =
        ".forkmesh/branch-reviews/" + branchReviewKey(branch) + "/";
    QByteArray listing;
    if (!runGit(m_mirror, {"ls-tree", "--name-only", ref, prefix}, &listing))
        return {};
    QStringList names;
    for (const QByteArray &raw : listing.split('\n')) {
        const QString path = QString::fromUtf8(raw).trimmed();
        if (!path.startsWith(prefix))
            continue;
        const QString name = path.mid(prefix.size());
        if (eventFileRe().match(name).hasMatch())
            names.append(name);
    }
    names.sort();
    QList<PullEvent> events;
    for (const QString &name : std::as_const(names)) {
        bool ok = false;
        const QByteArray text = showFromMirror(prefix + name, &ok, ref);
        if (ok)
            events.append(eventFromFrontMatter(parseFrontMatter(text)));
    }
    return events;
}

QByteArray PullStore::branchCanonicalString(const QString &branch,
                                            const PullEvent &ev,
                                            const QString &repoKey)
{
    const QByteArray contentHash =
        QCryptographicHash::hash(contentForSigning(ev).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    QByteArray canonical =
        forkmesh::eventsigning::prefix("branch-comment", repoKey);
    canonical += ev.type.toUtf8() + "\n";
    canonical += branch.toUtf8() + "\n";
    canonical += ev.author.toUtf8() + "\n";
    canonical += QByteArray::number(ev.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

PullEvent PullStore::makeSignedBranchEvent(const QString &branch,
                                           PullEvent ev) const
{
    if (ev.id.isEmpty())
        ev.id = newId();
    if (ev.type == QLatin1String("thread-comment") && ev.threadId.isEmpty())
        ev.threadId = ev.id;
    ev.author = m_identity ? m_identity->publicKey() : QString();
    if (ev.authorName.isEmpty())
        ev.authorName = m_authorName;
    if (ev.ts == 0)
        ev.ts = QDateTime::currentMSecsSinceEpoch();
    ev.sig = m_identity ? m_identity->signData(branchCanonicalString(
                              branch, ev, m_signingRepoKey))
                        : QString();
    return ev;
}

bool PullStore::appendBranchEvent(const QString &branch, const PullEvent &ev,
                                  const QString &commitMsg, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    const QString dir = branchReviewDir(branch);
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("Could not open the review ledger for %1.")
                         .arg(branch);
        return false;
    }
    const QString marker = dir + "/branch.md";
    if (!QFileInfo::exists(marker) &&
        !writeTextFile(marker, "---\nbranch: " + branch + "\n---\n", error))
        return false;
    if (!writeEventFileIn(dir, nextEventIndexIn(dir), ev, error))
        return false;
    const QString meta = metaWorkTree();
    const QString relative = ".forkmesh/branch-reviews/" + branchReviewKey(branch);
    QString err;
    if (!runGit(meta, {"add", "--", relative}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    if (!runGit(meta, {"commit", "-m", commitMsg, "--", relative}, nullptr,
                &err) &&
        !(err.contains("nothing to commit") || err.isEmpty())) {
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return true;
}

bool PullStore::addBranchThreadComment(const QString &branch, const QString &path,
                                       const QString &side, int lineStart,
                                       int lineEnd, const QString &body,
                                       const PullAnchor &anchor, QString *error)
{
    PullEvent ev;
    ev.type = "thread-comment";
    ev.path = path;
    ev.side = side;
    ev.lineStart = lineStart;
    ev.lineEnd = lineEnd > 0 ? lineEnd : lineStart;
    ev.body = stripEdgeNewlines(body);
    ev.patchset = anchor.patchset;
    ev.anchorLines = anchor.anchorLines;
    ev = makeSignedBranchEvent(branch, ev);
    return appendBranchEvent(
        branch, ev,
        QStringLiteral("branch %1: thread on %2:%3")
            .arg(branch, path)
            .arg(lineStart),
        error);
}

bool PullStore::addBranchThreadReply(const QString &branch,
                                     const QString &threadId,
                                     const QString &parentId,
                                     const QString &body, QString *error)
{
    PullEvent ev;
    ev.type = "thread-reply";
    ev.threadId = threadId;
    ev.parentId = parentId;
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedBranchEvent(branch, ev);
    return appendBranchEvent(
        branch, ev, QStringLiteral("branch %1: reply on thread").arg(branch),
        error);
}

bool PullStore::setBranchThreadState(const QString &branch,
                                     const QString &threadId,
                                     const QString &state, QString *error)
{
    PullEvent ev;
    ev.type = "thread-state";
    ev.threadId = threadId;
    ev.state = state;
    ev = makeSignedBranchEvent(branch, ev);
    return appendBranchEvent(
        branch, ev,
        QStringLiteral("branch %1: thread %2").arg(branch, state), error);
}

bool PullStore::validReviewBranchName(const QString &branch)
{
    if (branch.isEmpty() || branch.size() > 255 ||
        branch.startsWith(QLatin1Char('-')) ||
        branch.startsWith(QLatin1Char('/')) ||
        branch.endsWith(QLatin1Char('/')) ||
        branch.endsWith(QLatin1String(".lock")) ||
        branch.contains(QLatin1String("..")) ||
        branch.contains(QLatin1String("//")) ||
        branch.contains(QLatin1String("@{")))
        return false;
    for (const QChar ch : branch)
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f ||
            QStringLiteral(" ~^:?*[\\").contains(ch))
            return false;
    return true;
}

bool PullStore::applyRemoteBranchEvent(const QString &branch,
                                       const PullEvent &ev, QString *error)
{
    if (!validReviewBranchName(branch) ||
        (ev.type != QLatin1String("thread-comment") &&
         ev.type != QLatin1String("thread-reply") &&
         ev.type != QLatin1String("thread-state"))) {
        if (error)
            *error = QStringLiteral("Not a branch review event.");
        return false;
    }
    if (!ev.id.isEmpty() && canWrite()) {
        const QString dir = branchReviewDir(branch);
        for (const PullEvent &existing : readEventFilesIn(dir))
            if (existing.id == ev.id)
                return true;
    }
    return appendBranchEvent(
        branch, ev,
        QStringLiteral("branch %1: %2 (from %3)")
            .arg(branch, ev.type,
                 ev.authorName.isEmpty() ? ev.author.left(8) : ev.authorName),
        error);
}

int PullStore::nextNumber() const
{
    int max = 0;
    // Archived code-branch records and independent metadata branches reserve
    // their numbers too. Looking only at the aggregate checkout reused IDs
    // after metadata was archived or briefly missing.
    for (const PullRequest &pull : loadAllIncludingArchived())
        max = qMax(max, pull.number);
    return max + 1;
}

bool PullStore::writePull(const PullRequest &pr, QString *error) const
{
    QDir().mkpath(pullDir(pr.number));
    QStringList lines;
    lines << "---";
    lines << "schema: forkmesh-pull-v1";
    lines << "number: " + QString::number(pr.number);
    lines << "title: " + pr.title;
    lines << "base: " + pr.base;
    lines << "head: " + pr.head;
    lines << "status: " + pr.status;
    if (pr.branchBacked)
        lines << "derive: branch";
    if (!pr.creationBaseOid.isEmpty())
        lines << "creationBaseOid: " + pr.creationBaseOid;
    if (!pr.creationHeadOid.isEmpty())
        lines << "creationHeadOid: " + pr.creationHeadOid;
    if (!pr.headBaseOid.isEmpty())
        lines << "headBaseOid: " + pr.headBaseOid;
    if (!pr.headOid.isEmpty())
        lines << "headOid: " + pr.headOid;
    if (!pr.mergeBase.isEmpty())
        lines << "mergeBase: " + pr.mergeBase;
    if (!pr.mergeHead.isEmpty())
        lines << "mergeHead: " + pr.mergeHead;
    if (pr.mergedAt > 0)
        lines << "mergedAt: " + QString::number(pr.mergedAt);
    if (!pr.mergedBy.isEmpty())
        lines << "mergedBy: " + pr.mergedBy;
    if (!pr.coAuthors.isEmpty())
        lines << "coAuthors: " + pr.coAuthors;
    lines << "ts: " + QString::number(pr.ts);
    lines << "author: " + pr.author;
    lines << "authorName: " + pr.authorName;
    lines << "sig: " + pr.sig;
    // Carried through verbatim, never authored here. Dropping these on a
    // rewrite (a merge rewrites pull.md) would silently make an MCP-opened
    // pull unpayable as a microjob long after the work was done.
    if (!pr.client.isEmpty())
        lines << "client: " + pr.client;
    if (!pr.clientSig.isEmpty())
        lines << "clientSig: " + pr.clientSig;
    if (!pr.aiModel.simplified().isEmpty())
        lines << "aiModel: " + pr.aiModel.simplified();
    if (pr.rewardEstimateLamports > 0)
        lines << "rewardEstimateLamports: "
                     + QString::number(pr.rewardEstimateLamports);
    if (!pr.rewardRecipient.isEmpty())
        lines << "rewardRecipient: " + pr.rewardRecipient;
    if (!pr.rewardId.isEmpty())
        lines << "rewardId: " + pr.rewardId;
    if (!pr.rewardCondition.isEmpty())
        lines << "rewardCondition: " + pr.rewardCondition;
    if (!pr.rewardStatus.isEmpty())
        lines << "rewardStatus: " + pr.rewardStatus;
    if (!pr.rewardTxSignature.isEmpty())
        lines << "rewardTxSignature: " + pr.rewardTxSignature;
    lines << "---";
    lines << "";
    if (!writeTextFile(pullDir(pr.number) + "/pull.md",
                       lines.join('\n') + "\n" + pr.description + "\n", error))
        return false;
    if (pr.status != QLatin1String("open") || pr.branchBacked) {
        QFile::remove(pullDir(pr.number) + "/changes.patch");
        QFile::remove(pullDir(pr.number) + "/commits.mbox");
        return true;
    }
    if (!writeTextFile(pullDir(pr.number) + "/changes.patch", pr.patch, error))
        return false;
    if (!pr.commits.isEmpty())
        return writeTextFile(pullDir(pr.number) + "/commits.mbox", pr.commits, error);
    return true;
}

bool PullStore::readPull(int number, PullRequest &out) const
{
    QFile file(pullDir(number) + "/pull.md");
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const FrontMatter fm = parseFrontMatter(file.readAll());
    out.number = number;
    out.title = fm.get("title");
    out.base = fm.get("base");
    out.head = fm.get("head");
    out.status = fm.values.contains("status") ? fm.get("status")
                                              : QStringLiteral("open");
    out.ts = fm.num("ts");
    out.author = fm.get("author");
    out.authorName = fm.get("authorName");
    out.sig = fm.get("sig");
    out.branchBacked = fm.get("derive") == QLatin1String("branch");
    out.creationBaseOid = fm.get("creationBaseOid");
    out.creationHeadOid = fm.get("creationHeadOid");
    out.headBaseOid = fm.get("headBaseOid");
    out.headOid = fm.get("headOid");
    out.mergeBase = fm.get("mergeBase");
    out.mergeHead = fm.get("mergeHead");
    out.mergedAt = fm.num("mergedAt");
    out.mergedBy = fm.get("mergedBy");
    out.coAuthors = fm.get("coAuthors");
    out.client = fm.get("client");
    out.clientSig = fm.get("clientSig");
    out.aiModel = fm.get("aiModel");
    out.rewardEstimateLamports = fm.num("rewardEstimateLamports");
    out.rewardRecipient = fm.get("rewardRecipient");
    out.rewardId = fm.get("rewardId");
    out.rewardCondition = fm.get("rewardCondition");
    out.rewardStatus = fm.get("rewardStatus");
    out.rewardTxSignature = fm.get("rewardTxSignature");
    out.description = fm.body;
    out.patch.clear();
    out.commits.clear();
    const bool merged = !out.mergeBase.isEmpty() && !out.mergeHead.isEmpty();
    const QString rbase = merged ? out.mergeBase : out.base;
    const QString rhead = merged ? out.mergeHead : out.head;
    bool derived = false;
    if (out.status == QLatin1String("open") && out.branchBacked) {
        if (deriveFromRefs(m_workTree, rbase, rhead, &out.patch, &out.commits)) {
            derived = true;
            out.patchHeadOid = resolvedCommitOid(m_workTree, rhead);
        } else if (deriveFromRefs(m_mirror, rbase, rhead, &out.patch,
                                  &out.commits)) {
            derived = true;
            out.patchHeadOid = resolvedCommitOid(m_mirror, rhead);
        }
    }
    if (out.patchHeadOid.isEmpty())
        out.patchHeadOid = out.creationHeadOid;
    if (out.status == QLatin1String("open") && !derived) {
        QFile patch(pullDir(number) + "/changes.patch");
        if (patch.open(QIODevice::ReadOnly))
            out.patch = QString::fromUtf8(patch.readAll());
        QFile mbox(pullDir(number) + "/commits.mbox");
        if (mbox.open(QIODevice::ReadOnly))
            out.commits = QString::fromUtf8(mbox.readAll());
    }
    computeStats(out);
    out.events = readEvents(number);
    return true;
}

// A pull the ledger deleted keeps its forkmesh/pull/<n> branch wherever the
// deletion did not reach: the fleet's mirrors keep every such branch, and a
// checkout that syncs branches back from them recreates it. loadAll() treats
// that branch as authoritative over the aggregate index, so the deleted pull
// came back with the status of its last snapshot, usually "open" long after
// its work merged (#175 and #182 were deleted on 2026-09-02 and still listed
// as open pull requests on 2026-09-14). A branch whose snapshot is no newer
// than the ledger commit that removed its record is that leftover. A branch
// written after the removal, such as a number a later intake reused, stays.
static void dropDeletedPullMetadataRefs(const QString &repo,
                                        const QMap<int, PullRequest> &byNumber,
                                        QMap<int, QString> &refsByNumber,
                                        const QMap<int, qint64> &snapshotTimes)
{
    QStringList paths;
    for (auto it = refsByNumber.cbegin(); it != refsByNumber.cend(); ++it) {
        if (!byNumber.contains(it.key()))
            paths << QStringLiteral(".forkmesh/pulls/%1/pull.md").arg(it.key());
    }
    if (paths.isEmpty() || repo.trimmed().isEmpty())
        return;
    QByteArray log;
    if (!runGit(repo,
                QStringList{QStringLiteral("log"), QStringLiteral("--no-renames"),
                            QStringLiteral("--diff-filter=D"),
                            QStringLiteral("--format=%x01%ct"),
                            QStringLiteral("--name-only"),
                            QStringLiteral("refs/heads/forkmesh/pulls"),
                            QStringLiteral("--")} +
                    paths,
                &log, nullptr)) {
        return;
    }
    static const QRegularExpression deletedPath(
        QStringLiteral("^\\.forkmesh/pulls/([1-9][0-9]*)/pull\\.md$"));
    QMap<int, qint64> deletedAt;
    qint64 commitTime = 0;
    for (const QByteArray &raw : log.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QChar(0x01))) {
            commitTime = line.mid(1).toLongLong();
            continue;
        }
        const QRegularExpressionMatch match = deletedPath.match(line);
        // git log lists the newest deletion first; an older one is history.
        if (match.hasMatch() && !deletedAt.contains(match.captured(1).toInt()))
            deletedAt.insert(match.captured(1).toInt(), commitTime);
    }
    for (auto it = deletedAt.cbegin(); it != deletedAt.cend(); ++it) {
        const qint64 snapshot = snapshotTimes.value(it.key(), 0);
        if (snapshot > 0 && it.value() > 0 && snapshot <= it.value())
            refsByNumber.remove(it.key());
    }
}

namespace {

void adoptLedgerMerge(PullRequest &isolated, const PullRequest &ledger)
{
    if (ledger.status != QLatin1String("merged") ||
        isolated.status == QLatin1String("merged"))
        return;
    isolated.status = ledger.status;
    isolated.mergeBase = ledger.mergeBase;
    isolated.mergeHead = ledger.mergeHead;
}

constexpr qint64 kPullLoadSettledMs = 3000;
constexpr int kPullLoadCacheEntries = 8;

struct PullLoadStamp {
    qint64 newest = 0;
    qint64 entries = 0;
    qint64 bytes = 0;
};

void notePullLoadStamp(const QFileInfo &info, PullLoadStamp &stamp)
{
    if (!info.exists())
        return;
    stamp.newest = qMax(stamp.newest, info.lastModified().toMSecsSinceEpoch());
    ++stamp.entries;
    if (info.isFile())
        stamp.bytes += info.size();
}

void notePullLoadTree(const QString &root, PullLoadStamp &stamp)
{
    QDirIterator it(root,
                    QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        notePullLoadStamp(it.fileInfo(), stamp);
    }
}

QString pullLoadGitDir(const QString &dir)
{
    const QFileInfo dotGit(dir + QStringLiteral("/.git"));
    if (dotGit.isDir())
        return dotGit.absoluteFilePath();
    if (dotGit.isFile()) {
        QFile link(dotGit.absoluteFilePath());
        if (!link.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        const QString line = QString::fromUtf8(link.readLine(4096)).trimmed();
        if (!line.startsWith(QLatin1String("gitdir:")))
            return QString();
        const QString target = line.mid(7).trimmed();
        return target.isEmpty() ? QString()
                                : QDir::cleanPath(QDir(dir).absoluteFilePath(target));
    }
    if (QFileInfo::exists(dir + QStringLiteral("/HEAD")) &&
        QFileInfo(dir + QStringLiteral("/objects")).isDir())
        return QDir(dir).absolutePath();
    return QString();
}

QString pullLoadCommonDir(const QString &gitDir)
{
    QFile file(gitDir + QStringLiteral("/commondir"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return gitDir;
    const QString target = QString::fromUtf8(file.readLine(4096)).trimmed();
    return target.isEmpty() ? gitDir
                            : QDir::cleanPath(QDir(gitDir).absoluteFilePath(target));
}

bool notePullLoadRepository(const QString &dir, QStringList &gitDirs)
{
    if (dir.trimmed().isEmpty())
        return true;
    const QString gitDir = pullLoadGitDir(dir);
    if (gitDir.isEmpty())
        return false;
    for (const QString &candidate : {gitDir, pullLoadCommonDir(gitDir)}) {
        if (QFileInfo::exists(candidate + QStringLiteral("/reftable")))
            return false;
        if (!gitDirs.contains(candidate))
            gitDirs.append(candidate);
    }
    return true;
}

QString pullLoadStamp(const QString &workTree, const QString &mirror,
                      const QString &pullsDir)
{
    QStringList gitDirs;
    if (!notePullLoadRepository(workTree, gitDirs) ||
        !notePullLoadRepository(mirror, gitDirs))
        return QString();
    PullLoadStamp stamp;
    for (const QString &gitDir : std::as_const(gitDirs)) {
        notePullLoadStamp(QFileInfo(gitDir + QStringLiteral("/HEAD")), stamp);
        notePullLoadStamp(QFileInfo(gitDir + QStringLiteral("/packed-refs")), stamp);
        notePullLoadTree(gitDir + QStringLiteral("/refs"), stamp);
    }
    if (!pullsDir.isEmpty())
        notePullLoadTree(pullsDir, stamp);
    if (stamp.newest >=
        QDateTime::currentMSecsSinceEpoch() - kPullLoadSettledMs)
        return QString();
    return gitDirs.join(QLatin1Char('|')) + QLatin1Char('#') + pullsDir +
           QLatin1Char('#') + QString::number(stamp.newest) + QLatin1Char(':') +
           QString::number(stamp.entries) + QLatin1Char(':') +
           QString::number(stamp.bytes);
}

struct PullLoadCacheEntry {
    QString stamp;
    QList<PullRequest> pulls;
};

QMutex &pullLoadCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, PullLoadCacheEntry> &pullLoadCache()
{
    static QHash<QString, PullLoadCacheEntry> cache;
    return cache;
}

} // namespace

QList<PullRequest> PullStore::loadAll(QString *error) const
{
    const bool writable = canWrite();
    const QString cacheKey = m_workTree + QLatin1Char('\x1f') + m_mirror +
                             QLatin1Char('\x1f') +
                             (writable ? QLatin1Char('w') : QLatin1Char('r'));
    const QString cacheStamp =
        writable ? pullLoadStamp(m_workTree, m_mirror, pullsDir()) : QString();
    if (!cacheStamp.isEmpty()) {
        const QMutexLocker lock(&pullLoadCacheMutex());
        const auto cached = pullLoadCache().constFind(cacheKey);
        if (cached != pullLoadCache().constEnd() && cached->stamp == cacheStamp)
            return cached->pulls;
    }
    QList<PullRequest> pulls;
    bool complete = true;
    const QString repo = !m_workTree.trimmed().isEmpty() ? m_workTree : m_mirror;
    if (!writable) {
        pulls = loadFromMirror(error);
    } else {
        const QStringList entries = QDir(pullsDir()).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &entry : entries) {
            bool numeric = false;
            const int number = entry.toInt(&numeric);
            if (!numeric)
                continue;
            PullRequest pr;
            if (readPull(number, pr))
                pulls.append(pr);
            else
                complete = false;
        }
    }

    QMap<int, PullRequest> byNumber;
    for (PullRequest pull : std::as_const(pulls))
        byNumber.insert(pull.number, std::move(pull));

    QByteArray refs;
    static const QRegularExpression refPattern(
        QStringLiteral("^refs/heads/forkmesh/pull/([1-9][0-9]*)$"));
    static const QRegularExpression metadataRefPattern(
        QStringLiteral("^refs/pr/([1-9][0-9]*)/metadata$"));
    QMap<int, QString> refsByNumber;
    QMap<int, qint64> snapshotTimes;
    QHash<int, QString> branchTips;
    QMap<int, QPair<QString, qint64>> metadataRefs;
    const bool listedRefs =
        !repo.trimmed().isEmpty() &&
        runGit(repo,
               {"for-each-ref",
                "--format=%(refname) %(objectname) %(committerdate:unix)",
                "refs/heads/forkmesh/pull/", "refs/pr/"},
               &refs, nullptr);
    if (!repo.trimmed().isEmpty() && !listedRefs)
        complete = false;
    if (listedRefs) {
        for (const QByteArray &raw : refs.split('\n')) {
            const QStringList fields =
                QString::fromUtf8(raw).trimmed().split(QLatin1Char(' '));
            const QString ref = fields.value(0);
            const qint64 committed = fields.value(2).toLongLong();
            const QRegularExpressionMatch metadata = metadataRefPattern.match(ref);
            if (metadata.hasMatch()) {
                metadataRefs.insert(metadata.captured(1).toInt(), {ref, committed});
                continue;
            }
            const QRegularExpressionMatch match = refPattern.match(ref);
            if (!match.hasMatch())
                continue;
            const int number = match.captured(1).toInt();
            refsByNumber.insert(number, ref);
            branchTips.insert(number, fields.value(1).toLower());
            if (fields.size() >= 3)
                snapshotTimes.insert(number, committed);
        }
    }
    for (auto it = metadataRefs.cbegin(); it != metadataRefs.cend(); ++it) {
        if (refsByNumber.contains(it.key()))
            continue;
        refsByNumber.insert(it.key(), it.value().first);
        snapshotTimes.insert(it.key(), it.value().second);
    }
    dropDeletedPullMetadataRefs(repo, byNumber, refsByNumber, snapshotTimes);

    if (canWrite()) {
        // One-time migration: materialize only records that do not yet have an
        // independent branch. Once that branch exists it is authoritative; a
        // lagging compatibility index must never overwrite it during a read.
        for (const PullRequest &pull : std::as_const(pulls)) {
            if (refsByNumber.contains(pull.number))
                continue;
            if (materializePullMetadataRef(pull.number, nullptr))
                refsByNumber.insert(pull.number,
                                    mergedPullStatus(pull.status)
                                        ? pullMetadataCompatibilityRef(pull.number)
                                        : pullMetadataBranchRef(pull.number));
        }
    }
    QList<int> retirable;

    // Most repositories keep the compatibility index in sync, so loading N
    // independent refs one Git process at a time would make a 267-PR list
    // needlessly expensive. Compare all subtree IDs in one batch and parse
    // only refs that are missing from, or differ from, the aggregate view.
    QByteArray batchInput;
    for (auto it = refsByNumber.cbegin(); it != refsByNumber.cend(); ++it) {
        const QString path = QStringLiteral(":.forkmesh/pulls/%1").arg(it.key());
        batchInput += (it.value() + path).toUtf8() + '\n';
        batchInput += (QStringLiteral("refs/heads/forkmesh/pulls") + path).toUtf8() +
                      '\n';
    }
    QByteArray batchOutput;
    const bool compared = batchInput.isEmpty() ||
                          runGitWithInput(repo, {"cat-file", "--batch-check"},
                                          batchInput, &batchOutput, nullptr);
    if (!compared)
        complete = false;
    const QList<QByteArray> treeLines = batchOutput.split('\n');
    int treeLine = 0;
    const auto batchObjectId = [](const QByteArray &line) {
        const int space = line.indexOf(' ');
        return space < 0 ? line : line.left(space);
    };
    const PullStore refView(QString(), repo, m_identity, m_authorName);
    for (auto it = refsByNumber.cbegin(); it != refsByNumber.cend(); ++it) {
        QByteArray isolatedTree;
        QByteArray aggregateTree;
        if (compared && treeLine + 1 < treeLines.size()) {
            isolatedTree = batchObjectId(treeLines.at(treeLine));
            aggregateTree = batchObjectId(treeLines.at(treeLine + 1));
        }
        treeLine += 2;
        const bool onBranch = branchTips.contains(it.key()) &&
                              it.value() == pullMetadataBranchRef(it.key());
        if (byNumber.contains(it.key()) &&
            validCommitOid(QString::fromUtf8(isolatedTree)) &&
            isolatedTree == aggregateTree) {
            if (onBranch && mergedPullStatus(byNumber.value(it.key()).status))
                retirable.append(it.key());
            continue;
        }
        const int number = it.key();
        const QString ref = it.value();
        if (!ref.isEmpty()) {
            const QList<PullRequest> isolated =
                refView.loadFromMirror(nullptr, false, ref);
            bool found = false;
            for (PullRequest pull : isolated) {
                if (pull.number != number)
                    continue;
                found = true;
                const auto ledger = byNumber.constFind(number);
                if (onBranch && ledger != byNumber.cend() &&
                    mergedPullStatus(pull.status) &&
                    mergedPullStatus(ledger->status))
                    retirable.append(number);
                if (ledger != byNumber.cend())
                    adoptLedgerMerge(pull, *ledger);
                byNumber.insert(number, std::move(pull));
            }
            if (!found)
                complete = false;
        }
    }
    if (canWrite()) {
        for (const int number : std::as_const(retirable)) {
            const QString tip = branchTips.value(number);
            if (claimPullBranchRetirement(repo, number, tip))
                retirePullMetadataBranch(repo, number, tip, tip, nullptr);
        }
    }
    pulls = byNumber.values();
    std::sort(pulls.begin(), pulls.end(),
              [](const PullRequest &a, const PullRequest &b) { return a.number < b.number; });
    if (!cacheStamp.isEmpty() && complete) {
        const QMutexLocker lock(&pullLoadCacheMutex());
        QHash<QString, PullLoadCacheEntry> &cache = pullLoadCache();
        if (!cache.contains(cacheKey) && cache.size() >= kPullLoadCacheEntries)
            cache.clear();
        cache.insert(cacheKey, PullLoadCacheEntry{cacheStamp, pulls});
    }
    return pulls;
}

QList<PullRequest> PullStore::loadAllIncludingArchived(QString *error) const
{
    QList<PullRequest> pulls = loadAll(error);
    QSet<int> known;
    for (const PullRequest &pull : std::as_const(pulls))
        known.insert(pull.number);

    // Before pull metadata moved to its own forkmesh/pulls branch it lived on
    // the code branch. The repository badge deliberately counts both places,
    // so the All view must read both places too. Prefer the working checkout's
    // HEAD (it is the exact code revision being viewed), then fill any gaps
    // from the mirror's HEAD for read-only or lagging checkouts.
    QSet<QString> sources;
    const auto appendSource = [&](const QString &source) {
        const QString trimmed = source.trimmed();
        if (trimmed.isEmpty())
            return;
        const QString clean = QDir::cleanPath(trimmed);
        if (sources.contains(clean) ||
            !QFileInfo::exists(clean)) {
            return;
        }
        sources.insert(clean);

        const PullStore codeView(QString(), clean, nullptr);
        const QList<PullRequest> codePulls =
            codeView.loadFromMirror(nullptr, false, QStringLiteral("HEAD"));
        for (PullRequest pull : codePulls) {
            if (pull.number <= 0 || known.contains(pull.number))
                continue;
            pull.archivedOnCodeBranch = true;
            known.insert(pull.number);
            pulls.append(std::move(pull));
        }

        // Count compatibility includes the older JSON layout as well. Surface
        // those records so the All badge and rows cannot disagree merely
        // because a repository crossed both storage migrations.
        QByteArray listing;
        if (!runGit(clean,
                    {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                     QStringLiteral("--name-only"), QStringLiteral("HEAD"),
                     QStringLiteral("--"),
                     QStringLiteral(".forkmesh/pulls/open/")},
                    &listing)) {
            return;
        }
        static const QRegularExpression legacyPath(
            QStringLiteral(
                "^\\.forkmesh/pulls/open/(?:pull-)?([1-9][0-9]*)\\.json$"));
        QList<QPair<int, QString>> legacyFiles;
        QByteArray query;
        for (const QByteArray &raw : listing.split('\n')) {
            const QString path = QString::fromUtf8(raw).trimmed();
            const QRegularExpressionMatch match = legacyPath.match(path);
            if (!match.hasMatch())
                continue;
            const int number = match.captured(1).toInt();
            if (known.contains(number))
                continue;
            legacyFiles.append({number, path});
            query += (QStringLiteral("HEAD:") + path).toUtf8() + '\n';
        }
        if (legacyFiles.isEmpty())
            return;
        // One `cat-file --batch` reads every legacy record, instead of a
        // `git show` per file.
        QByteArray batch;
        if (!runGitWithInput(clean,
                             {QStringLiteral("cat-file"), QStringLiteral("--batch")},
                             query, &batch)) {
            return;
        }
        QHash<QString, QByteArray> jsonByPath;
        qsizetype pos = 0;
        for (const auto &legacy : std::as_const(legacyFiles)) {
            const qsizetype nl = batch.indexOf('\n', pos);
            if (nl < 0)
                break;
            const QList<QByteArray> header = batch.mid(pos, nl - pos).split(' ');
            pos = nl + 1;
            // "<name> missing" (or anything that is not a blob) has no body.
            if (header.size() != 3 || header.at(1) != QByteArrayLiteral("blob") ||
                !validCommitOid(QString::fromUtf8(header.at(0)))) {
                continue;
            }
            bool sizeOk = false;
            const qlonglong size = header.at(2).toLongLong(&sizeOk);
            if (!sizeOk || size < 0 || size >= batch.size() - pos)
                break;
            jsonByPath.insert(legacy.second, batch.mid(pos, size));
            pos += size + 1; // skip body and its trailing newline
        }
        for (const auto &legacy : std::as_const(legacyFiles)) {
            const int number = legacy.first;
            if (known.contains(number) || !jsonByPath.contains(legacy.second))
                continue;
            const QJsonDocument document =
                QJsonDocument::fromJson(jsonByPath.value(legacy.second));
            if (!document.isObject())
                continue;
            PullRequest pull = PullRequest::fromJson(document.object());
            pull.number = number; // the counted path is authoritative
            pull.archivedOnCodeBranch = true;
            const QJsonArray events = document.object()
                                          .value(QStringLiteral("events"))
                                          .toArray();
            for (const QJsonValue &event : events)
                if (event.isObject())
                    pull.events.append(PullEvent::fromJson(event.toObject()));
            known.insert(number);
            pulls.append(std::move(pull));
        }
    };
    appendSource(m_workTree);
    appendSource(m_mirror);

    std::sort(pulls.begin(), pulls.end(),
              [](const PullRequest &a, const PullRequest &b) {
                  return a.number < b.number;
              });
    return pulls;
}

// code-health: allow(git-in-loop, dir-walk-in-loop) git loops cover at most 2 checkouts (x 2 legacy JSON paths); removeRecursively only runs on a failure path that returns
bool PullStore::restoreArchivedPull(int number, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral(
                "Restoring a pull request needs a local working tree.");
        return false;
    }
    if (number <= 0) {
        if (error)
            *error = QStringLiteral("The pull request number is invalid.");
        return false;
    }
    if (QFileInfo::exists(pullDir(number)))
        return true;

    const QString prefix =
        QStringLiteral(".forkmesh/pulls/%1/").arg(number);
    QString source;
    QStringList paths;
    for (const QString &candidate : {m_workTree, m_mirror}) {
        if (candidate.trimmed().isEmpty() || !QFileInfo::exists(candidate))
            continue;
        QByteArray listing;
        if (!runGit(candidate,
                    {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                     QStringLiteral("--name-only"), QStringLiteral("HEAD"),
                     QStringLiteral("--"), prefix},
                    &listing)) {
            continue;
        }
        QStringList found;
        for (const QByteArray &raw : listing.split('\n')) {
            const QString path = QString::fromUtf8(raw).trimmed();
            if (path.startsWith(prefix))
                found.append(path);
        }
        if (found.contains(prefix + QStringLiteral("pull.md"))) {
            source = candidate;
            paths = std::move(found);
            break;
        }
    }
    if (source.isEmpty()) {
        // The oldest layout used one JSON file per open PR. Normalize it to
        // pull.md while restoring; its author/signature and portable diff stay
        // unchanged, and subsequent actions use the current ledger format.
        for (const QString &candidate : {m_workTree, m_mirror}) {
            if (candidate.trimmed().isEmpty() || !QFileInfo::exists(candidate))
                continue;
            for (const QString &path :
                 {QStringLiteral(".forkmesh/pulls/open/pull-%1.json").arg(number),
                  QStringLiteral(".forkmesh/pulls/open/%1.json").arg(number)}) {
                QByteArray json;
                if (!runGit(candidate,
                            {QStringLiteral("show"),
                             QStringLiteral("HEAD:") + path},
                            &json)) {
                    continue;
                }
                const QJsonDocument document = QJsonDocument::fromJson(json);
                if (!document.isObject())
                    continue;
                PullRequest pull = PullRequest::fromJson(document.object());
                pull.number = number;
                pull.archivedOnCodeBranch = false;
                if (!writePull(pull, error) ||
                    !commit(QStringLiteral("pull #%1: restore archived record")
                                .arg(number),
                            error)) {
                    return false;
                }
                materializePullRef(pull, nullptr);
                return true;
            }
        }
        if (error) {
            *error = QStringLiteral(
                         "Archived pull request #%1 could not be read from the "
                         "repository branch.")
                         .arg(number);
        }
        return false;
    }

    const QString destinationRoot = pullsDir();
    const QString destinationDir =
        destinationRoot + QLatin1Char('/') + QString::number(number);
    if (!QDir().mkpath(destinationDir)) {
        if (error)
            *error = QStringLiteral("Could not create the pull ledger folder.");
        return false;
    }
    // One `cat-file --batch` reads every file of the archived record, instead
    // of a `git show` per event file.
    QByteArray query;
    for (const QString &path : std::as_const(paths))
        query += (QStringLiteral("HEAD:") + path).toUtf8() + '\n';
    QHash<QString, QByteArray> contentsByPath;
    QByteArray batch;
    if (runGitWithInput(source, {QStringLiteral("cat-file"), QStringLiteral("--batch")},
                        query, &batch)) {
        qsizetype pos = 0;
        for (const QString &path : std::as_const(paths)) {
            const qsizetype nl = batch.indexOf('\n', pos);
            if (nl < 0)
                break;
            const QList<QByteArray> header = batch.mid(pos, nl - pos).split(' ');
            pos = nl + 1;
            // "<name> missing" (or anything that is not a blob) has no body.
            if (header.size() != 3 || header.at(1) != QByteArrayLiteral("blob") ||
                !validCommitOid(QString::fromUtf8(header.at(0)))) {
                continue;
            }
            bool sizeOk = false;
            const qlonglong size = header.at(2).toLongLong(&sizeOk);
            if (!sizeOk || size < 0 || size >= batch.size() - pos)
                break;
            contentsByPath.insert(path, batch.mid(pos, size));
            pos += size + 1; // skip body and its trailing newline
        }
    }
    for (const QString &path : std::as_const(paths)) {
        const auto found = contentsByPath.constFind(path);
        if (found == contentsByPath.constEnd()) {
            forkmesh::safedelete::removeTree(destinationDir, destinationRoot);
            if (error)
                *error = QStringLiteral("Could not read archived pull metadata.");
            return false;
        }
        const QByteArray &contents = found.value();
        const QString relative = path.mid(prefix.size());
        const QString destination =
            destinationDir + QLatin1Char('/') + relative;
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            forkmesh::safedelete::removeTree(destinationDir, destinationRoot);
            if (error)
                *error = QStringLiteral("Could not create the pull ledger folder.");
            return false;
        }
        QFile file(destination);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(contents) != contents.size()) {
            forkmesh::safedelete::removeTree(destinationDir, destinationRoot);
            if (error)
                *error = QStringLiteral("Could not restore archived pull metadata.");
            return false;
        }
    }
    if (!commit(QStringLiteral("pull #%1: restore archived record").arg(number),
                error)) {
        return false;
    }
    PullRequest restored;
    if (readPull(number, restored))
        materializePullRef(restored, nullptr);
    return true;
}

QList<PullRequest> PullStore::loadAllStrict(QString *error) const
{
    if (error)
        error->clear();
    if (canWrite()) {
        const PullStore mirrorView(QString(), m_workTree, nullptr);
        return mirrorView.loadFromMirror(error, true);
    }
    return loadFromMirror(error, true);
}

QList<PullRequest> PullStore::loadAllStrictAtRef(const QString &ref,
                                                 QString *error) const
{
    if (error)
        error->clear();
    if (canWrite()) {
        const PullStore mirrorView(QString(), m_workTree, nullptr);
        return mirrorView.loadFromMirror(error, true, ref);
    }
    return loadFromMirror(error, true, ref);
}

int PullStore::createPull(const QString &title, const QString &description,
                          const QString &base, const QString &head,
                          const QString &patch, const QString &commits,
                          bool branchBacked, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return -1;
    }
    if (!head.isEmpty()) {
        for (const PullRequest &existing : loadAll()) {
            if (existing.status == QLatin1String("open") && existing.head == head)
                return existing.number;
        }
    }
    PullRequest pr;
    pr.title = title;
    pr.description = description;
    pr.base = base;
    pr.head = head;
    pr.patch = patch;
    pr.commits = commits;
    // Branch-backed: the head branch ref carries the change as real commits, so
    // store only the signed pointer and reconstruct the diff from base..head.
    // Recompute patch/commits from the refs now so the signature (and any later
    // cross-node submission) matches exactly what readers will derive.
    if (branchBacked) {
        QString dpatch, dcommits;
        const QString baseOid = resolvedCommitOid(m_workTree, base);
        const QString headOid = resolvedCommitOid(m_workTree, head);
        if (!baseOid.isEmpty() && !headOid.isEmpty() &&
            deriveFromImmutableOids(m_workTree, baseOid, headOid, &dpatch,
                                    &dcommits) &&
            !dpatch.trimmed().isEmpty()) {
            pr.branchBacked = true;
            pr.creationBaseOid = baseOid;
            pr.creationHeadOid = headOid;
            pr.patch = dpatch;
            pr.commits = dcommits;
        }
    }
    pr = makeSignedPull(pr);
    pr.number = nextNumber();
    if (!writePull(pr, error))
        return -1;
    if (!commit(QStringLiteral("pull #%1: open").arg(pr.number), error))
        return -1;
    materializePullRef(pr, nullptr);
    return pr.number;
}

bool PullStore::setStatus(int number, const QString &status, QString *error)
{
    if (!canWrite())
        return false;
    if (status == QLatin1String("closed"))
        return deletePull(number, /*rewriteHistory=*/true, error);
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    pr.status = status;
    if (!writePull(pr, error))
        return false;
    if (!commit(QStringLiteral("pull #%1: %2").arg(number).arg(status), error))
        return false;
    return status == QLatin1String("open") ||
           purgePullPayloadHistory(number, error);
}

bool PullStore::recordRewardPayment(int number, const QString &rewardId,
                                    const QString &chainSignature,
                                    QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    const QString reward = rewardId.trimmed();
    const QString signature = chainSignature.trimmed();
    if (reward.isEmpty() || signature.isEmpty() ||
        pr.rewardId.trimmed() != reward) {
        if (error)
            *error = QStringLiteral(
                         "Pull request #%1 does not carry reward %2.")
                         .arg(number)
                         .arg(rewardId);
        return false;
    }
    const QString recorded = pr.rewardTxSignature.trimmed();
    // Already settled with this exact transfer: the reconciliation ran twice
    // (a retried receipt, a second desktop). Nothing to write, and nothing
    // wrong either.
    if (recorded == signature &&
        pr.rewardStatus.trimmed() == QLatin1String("paid"))
        return true;
    // A different signature already on the record means two transfers are
    // claiming the same estimate. Leave the first one standing rather than
    // silently rewriting which transfer the reader is told to verify.
    if (!recorded.isEmpty() && recorded != signature) {
        if (error)
            *error = QStringLiteral(
                         "Pull request #%1 already records a different reward transaction.")
                         .arg(number);
        return false;
    }
    if (pr.rewardStatus.trimmed() == QLatin1String("cancelled")) {
        if (error)
            *error = QStringLiteral("Pull request #%1's reward was cancelled.")
                         .arg(number);
        return false;
    }
    pr.rewardTxSignature = signature;
    pr.rewardStatus = QStringLiteral("paid");
    if (!writePull(pr, error))
        return false;
    return commit(QStringLiteral("pull #%1: reward paid").arg(number), error);
}

bool PullStore::isBranchBehindBase(int number, bool *behind, QString *error,
                                   int *behindCount) const
{
    if (behind)
        *behind = false;
    if (behindCount)
        *behindCount = 0;
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    // This check only needs the two branch names. readPull() also derives the
    // complete patch and format-patch series for a branch-backed pull. That is
    // especially costly when a commit deletes a binary (the mbox contains its
    // base64 payload), and this method runs while the detail action state is
    // being refreshed on the GUI thread.
    QFile file(pullDir(number) + QStringLiteral("/pull.md"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    const FrontMatter fm = parseFrontMatter(file.readAll());
    const QString base = fm.get("base");
    const QString head = fm.get("head");
    if (base.isEmpty() || head.isEmpty()) {
        if (error)
            *error = QStringLiteral("This pull request does not name a base and head branch.");
        return false;
    }
    QByteArray output;
    QString err;
    if (!runGit(m_workTree, {"rev-list", "--count", head + ".." + base},
                &output, &err)) {
        if (error)
            *error = QStringLiteral("Could not compare branches: %1").arg(err);
        return false;
    }
    const int count = QString::fromUtf8(output).trimmed().toInt();
    if (behind)
        *behind = count > 0;
    if (behindCount)
        *behindCount = count;
    return true;
}

bool PullStore::updateBranchFromBase(int number, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Updating needs a local working tree.");
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (pr.status != "open") {
        if (error)
            *error = forkmesh::pulldecision::decidedPullMessage(pr.status);
        return false;
    }

    bool behind = false;
    if (!isBranchBehindBase(number, &behind, error))
        return false;
    if (!behind)
        return true;

    QString err;
    QByteArray status;
    if (!runGit(m_workTree, {"status", "--porcelain"}, &status, &err) ||
        !status.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Commit or stash local changes before updating the branch.");
        return false;
    }

    QByteArray originalBranch;
    QByteArray originalCommit;
    runGit(m_workTree, {"rev-parse", "--abbrev-ref", "HEAD"}, &originalBranch, nullptr);
    runGit(m_workTree, {"rev-parse", "--verify", "HEAD"}, &originalCommit, nullptr);
    const QString current = QString::fromUtf8(originalBranch).trimmed();
    const QString restoreRef =
        current.isEmpty() || current == QLatin1String("HEAD")
            ? QString::fromUtf8(originalCommit).trimmed()
            : current;

    if (!runGit(m_workTree, {"checkout", pr.head}, nullptr, &err)) {
        const bool recovered = releaseWorktreeHoldingBranch(m_workTree, pr.head) &&
                               runGit(m_workTree, {"checkout", pr.head}, nullptr, &err);
        if (!recovered) {
            if (error)
                *error = QStringLiteral("Could not check out %1: %2").arg(pr.head, err);
            return false;
        }
    }
    QByteArray mergeOut;
    if (!runGit(m_workTree, {"merge", "--no-edit", pr.base}, &mergeOut, &err,
                kGitMergeTimeoutMs)) {
        runGit(m_workTree, {"merge", "--abort"}, nullptr, nullptr);
        if (!restoreRef.isEmpty() && restoreRef != pr.head)
            runGit(m_workTree, {"checkout", restoreRef}, nullptr, nullptr);
        QString detail = err.simplified();
        if (detail.isEmpty())
            detail = QString::fromUtf8(mergeOut).simplified().left(300);
        if (detail.isEmpty())
            detail = QStringLiteral("git merge failed without saying why");
        if (error)
            *error = QStringLiteral("Could not merge %1 into %2: %3")
                         .arg(pr.base, pr.head, detail);
        return false;
    }
    if (!restoreRef.isEmpty() && restoreRef != pr.head &&
        !runGit(m_workTree, {"checkout", restoreRef}, nullptr, &err)) {
        if (error)
            *error = QStringLiteral("Updated %1, but could not return to %2: %3")
                         .arg(pr.head, restoreRef, err);
        return false;
    }

    QByteArray diff;
    if (!runGit(m_workTree, {"diff", "--binary", pr.base + ".." + pr.head}, &diff,
                &err)) {
        if (error)
            *error = QStringLiteral("Could not refresh the pull request patch: %1").arg(err);
        return false;
    }
    pr.patch = QString::fromUtf8(diff);
    computeStats(pr);
    if (!writePull(pr, error))
        return false;
    if (!commit(QStringLiteral("pull #%1: update branch from %2")
                    .arg(number)
                    .arg(pr.base),
                error))
        return false;
    materializePullRef(pr, nullptr);
    return true;
}

static QString amBranchBackupRef(const QString &branch)
{
    return QStringLiteral("refs/forkmesh/am-backup/") + branch;
}

static QString amStateDir(const QString &workTree)
{
    QByteArray out;
    if (!runGit(workTree, {"rev-parse", "--git-path", "rebase-apply"}, &out))
        return QString();
    QString path = QString::fromUtf8(out).trimmed();
    if (path.isEmpty())
        return QString();
    if (!QDir::isAbsolutePath(path))
        path = workTree + "/" + path;
    return QFileInfo::exists(path) ? path : QString();
}

static QStringList unmergedFiles(const QString &workTree)
{
    QByteArray out;
    runGit(workTree, {"diff", "--name-only", "--diff-filter=U"}, &out);
    return QString::fromUtf8(out).split('\n', Qt::SkipEmptyParts);
}

static QMutex &liveAmMutex()
{
    static QMutex mutex;
    return mutex;
}

static QSet<QString> &liveAmWorkTrees()
{
    static QSet<QString> trees;
    return trees;
}

static QString amWorkTreeKey(const QString &workTree)
{
    const QFileInfo info(workTree);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

static void setAmSessionLive(const QString &workTree, bool live)
{
    if (workTree.isEmpty())
        return;
    const QString key = amWorkTreeKey(workTree);
    QMutexLocker lock(&liveAmMutex());
    if (live)
        liveAmWorkTrees().insert(key);
    else
        liveAmWorkTrees().remove(key);
}

static bool amSessionLive(const QString &workTree)
{
    const QString key = amWorkTreeKey(workTree);
    QMutexLocker lock(&liveAmMutex());
    return liveAmWorkTrees().contains(key);
}

static bool healStaleAmState(const QString &workTree)
{
    if (workTree.isEmpty() || amStateDir(workTree).isEmpty())
        return true;
    if (amSessionLive(workTree))
        return false;
    QByteArray tracked;
    runGit(workTree, {"status", "--porcelain", "--untracked-files=no"}, &tracked);
    const bool keepEdits =
        unmergedFiles(workTree).isEmpty() && !tracked.trimmed().isEmpty();
    runGit(workTree, {"am", keepEdits ? "--quit" : "--abort"}, nullptr, nullptr);
    if (!amStateDir(workTree).isEmpty())
        runGit(workTree, {"am", "--quit"}, nullptr, nullptr);
    const QString left = amStateDir(workTree);
    if (!left.isEmpty() && QFileInfo(left).fileName() == QLatin1String("rebase-apply"))
        forkmesh::safedelete::removeTree(left, QFileInfo(left).absolutePath());
    const bool healed = amStateDir(workTree).isEmpty();
    qWarning().noquote() << "git: stale am/rebase state in" << workTree
                         << (healed ? (keepEdits ? "quit, local edits kept"
                                                 : "aborted")
                                    : "could not be cleared");
    return healed;
}

static QString amBlockedMessage(const QString &workTree)
{
    if (amSessionLive(workTree))
        return QStringLiteral("Another pull-request operation is already in "
                              "progress; finish or cancel it first.");
    return QStringLiteral("A stale git am session in this checkout could not be "
                          "cleared; run \"git am --abort\" there.");
}

bool PullStore::mergePull(int number, QString *error, bool requirePeerReview,
                          bool ownerReviewOverride)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Merging needs a local working tree.");
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (pr.status != "open") {
        if (error)
            *error = forkmesh::pulldecision::decidedPullMessage(pr.status);
        return false;
    }
    const bool reviewReady = pr.independentReviewGateSatisfied();
    if (requirePeerReview && !reviewReady && !ownerReviewOverride) {
        if (error) {
            *error = QStringLiteral(
                "At least one approval from a peer other than the pull "
                "request author is required, with no unresolved peer "
                "request for changes.");
        }
        return false;
    }
    if (!healStaleAmState(m_workTree)) {
        if (error)
            *error = amBlockedMessage(m_workTree);
        return false;
    }
    QString err;
    QString headSha = pullHeadCommit(pr);
    if (headSha.isEmpty() && !pr.branchBacked && materializePullRef(pr, nullptr))
        headSha = pullHeadCommit(pr);
    const bool refMerge = !headSha.isEmpty();
    QString mergedMessages = pr.commits;
    if (refMerge)
        mergedMessages += QLatin1Char('\n') +
                          commitMessages(m_workTree, QStringLiteral("HEAD"), headSha);
    QString preBase, preHead;
    if (pr.branchBacked) {
        QByteArray bsha;
        if (runGit(m_workTree, {"rev-parse", "--verify", pr.base + "^{commit}"},
                   &bsha))
            preBase = QString::fromUtf8(bsha).trimmed();
        preHead = headSha; // the resolved head (local ref or fetched from mirror)
        if (preHead.isEmpty()) {
            QByteArray hsha;
            if (runGit(m_workTree, {"rev-parse", "--verify", pr.head + "^{commit}"},
                       &hsha))
                preHead = QString::fromUtf8(hsha).trimmed();
        }
    } else if (refMerge) {
        preBase = resolvedCommitOid(m_workTree, QStringLiteral("HEAD"));
        preHead = headSha;
    }
    bool applied = false;
    if (refMerge) {
        QByteArray pendingMerge;
        if (runGit(m_workTree, {"rev-parse", "-q", "--verify", "MERGE_HEAD"},
                   &pendingMerge) &&
            QString::fromUtf8(pendingMerge).trimmed() == headSha)
            runGit(m_workTree, {"merge", "--abort"}, nullptr, nullptr);
        applied = runGit(m_workTree,
                         {"merge", "--no-edit", "-m",
                          QStringLiteral("merge pull #%1: %2").arg(number).arg(pr.title),
                          headSha},
                         nullptr, &err);
        if (!applied) {
            runGit(m_workTree, {"merge", "--abort"}, nullptr, nullptr);
            if (error)
                *error = forkmesh::gitrefusal::overLocalChanges(err)
                             ? forkmesh::pulldecision::localChangesBlockMessage(
                                   number, QDir::toNativeSeparators(m_workTree), err)
                             : "Could not merge the pull request's branch cleanly: "
                                   + err;
            return false;
        }
    } else {
        const QString storedPatch = pullDir(number) + "/changes.patch";
        const QString storedMbox = pullDir(number) + "/commits.mbox";
        QString tempFile;
        auto materialize = [&](const QString &onDisk, const QString &content,
                               const char *suffix) -> QString {
            if (!pr.branchBacked && QFileInfo::exists(onDisk))
                return onDisk;
            tempFile = QDir::temp().filePath(
                QStringLiteral("forkmesh-merge-%1.%2").arg(number).arg(suffix));
            return writeTextFile(tempFile, content, error) ? tempFile : QString();
        };
        if (!pr.commits.isEmpty()) {
            const QString mbox = materialize(storedMbox, pr.commits, "mbox");
            if (mbox.isEmpty())
                return false;
            applied = runGit(m_workTree, {"am", "--3way", mbox}, nullptr, &err);
            if (!applied) {
                runGit(m_workTree, {"am", "--abort"}, nullptr, nullptr);
                if (error)
                    *error =
                        "Could not replay the pull request's commits cleanly: " + err;
            }
        } else {
            const QString patch = materialize(storedPatch, pr.patch, "patch");
            if (patch.isEmpty())
                return false;
            applied = runGit(m_workTree, {"apply", "--index", "--3way", patch},
                             nullptr, &err);
            if (!applied && error)
                *error = "Could not apply the patch cleanly: " + err;
        }
        if (!tempFile.isEmpty())
            QFile::remove(tempFile);
        if (!applied)
            return false;
    }
    pr.status = "merged";
    pr.mergedAt = QDateTime::currentMSecsSinceEpoch();
    pr.mergedBy = m_identity ? m_identity->publicKey() : QString();
    pr.coAuthors = coAuthorEmails(mergedMessages);
    PullReward::advanceForMerge(pr);
    if (!preBase.isEmpty() && !preHead.isEmpty()) {
        pr.mergeBase = preBase;
        pr.mergeHead = preHead;
    }
    if (!writePull(pr, error))
        return false;
    QString ledgerMessage = QStringLiteral("pull #%1: merged").arg(number);
    if (requirePeerReview && !reviewReady && ownerReviewOverride) {
        ledgerMessage += QStringLiteral(" (review gate waived by owner)");
    }
    if (!commit(ledgerMessage, error))
        return false;
    // The ledger already records the merge above. Dropping this pull's
    // payload from history is housekeeping, so a slow rewrite (git timed out
    // on the full ledger, 2026-09-09) must not report the merge itself as
    // failed and leave the operator retrying a merge that already landed.
    QString purgeError;
    if (!purgePullPayloadHistory(number, &purgeError))
        qWarning().noquote() << "pull" << number
                             << "payload purge skipped after merge:"
                             << purgeError;
    if (refMerge || !pr.commits.isEmpty())
        return true;
    if (!runGit(m_workTree, {"add", "-A"}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    if (!runGit(m_workTree,
               {"commit", "-m",
                QStringLiteral("merge pull #%1: %2").arg(number).arg(pr.title)},
               nullptr, &err)) {
        if (err.contains("nothing to commit"))
            return true;
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return true;
}


bool PullStore::prefetchForeignLedger(const QString &remoteUrl, QString *error)
{
    if (!canWrite() || remoteUrl.trimmed().isEmpty())
        return false;
    const QString dir = metaWorkTree();
    if (dir.isEmpty())
        return false;
    QString err;
    if (!runGit(dir, {"fetch", "--no-tags", "--quiet", remoteUrl,
                      "refs/heads/forkmesh/pulls"},
                nullptr, &err, kGitRewriteTimeoutMs)) {
        if (error)
            *error = "ledger fetch failed: " + err;
        return false;
    }
    return true;
}


// code-health: allow(git-in-loop) the loop walks `rev-list HEAD..FETCH_HEAD`, the served ledger commits this ledger lacks, and is skipped outright when the served tip is already an ancestor (the usual sweep); each commit needs its own diff-tree check and its own cherry-pick so it lands as its own ledger commit, which no single git invocation expresses. Called from the pull-ledger sweep completion on the GUI thread after the fetch ran on a worker, so every wait is runGit's keep-alive pumped wait
QList<int> PullStore::adoptForeignPulls(const QString &remoteUrl, QString *error,
                                        bool fetchFirst)
{
    QList<int> adopted;
    if (!canWrite() || remoteUrl.trimmed().isEmpty())
        return adopted;
    const QString dir = metaWorkTree();
    if (dir.isEmpty())
        return adopted;
    QString err;
    // Never adopt on top of unrelated pending ledger edits: each cherry-pick
    // below must land as the only change in its ledger commit.
    QByteArray pending;
    if (!runGit(dir, {"status", "--porcelain", "--", ".forkmesh/pulls"},
                &pending, &err) ||
        !QString::fromUtf8(pending).trimmed().isEmpty())
        return adopted;
    if (fetchFirst && !prefetchForeignLedger(remoteUrl, error))
        return adopted;
    QByteArray tip;
    if (!runGit(dir, {"rev-parse", "--verify", "-q", "FETCH_HEAD^{commit}"},
                &tip) ||
        tip.trimmed().isEmpty())
        return adopted;
    const QString foreign = QString::fromUtf8(tip).trimmed();
    if (runGit(dir, {"merge-base", "--is-ancestor", foreign, "HEAD"}))
        return adopted; // the served ledger has nothing this one lacks
    QByteArray revs;
    if (!runGit(dir, {"rev-list", "--reverse", "HEAD.." + foreign}, &revs, &err))
        return adopted;
    static const QRegularExpression pullEntryPath(
        QStringLiteral("^\\.forkmesh/pulls/(\\d+)/[^/]+$"));
    static const QRegularExpression sigLine(
        QStringLiteral("^sig:\\s*(\\S+)"), QRegularExpression::MultilineOption);
    const auto signatureOf = [](const QString &text) {
        return sigLine.match(text).captured(1);
    };
    // The unattended SSH mirror push never force-updates a ref, so a mirror
    // whose forkmesh/pulls diverged stays held back until this ledger's tip
    // *contains* the mirror's. Track whether every foreign commit was either
    // adopted here or already carried (same signed submission); only then may
    // the ancestry merge below declare the foreign history consumed.
    bool allConsumed = true;
    for (const QByteArray &line : revs.split('\n')) {
        const QString commit = QString::fromUtf8(line).trimmed();
        if (commit.isEmpty())
            continue;
        QByteArray listing;
        if (!runGit(dir, {"diff-tree", "--no-commit-id", "--name-status", "-r",
                          "--root", commit},
                    &listing)) {
            allConsumed = false;
            continue;
        }
        // Only pure additions of whole pull directories qualify: a mirror that
        // drained the inbox on a stale ledger appends exactly such commits.
        // Anything that edits or deletes existing entries stays foreign — the
        // signed local history for those pulls is authoritative here.
        bool eligible = false;
        QSet<int> numbers;
        for (const QByteArray &rawRow : listing.split('\n')) {
            const QString row = QString::fromUtf8(rawRow).trimmed();
            if (row.isEmpty())
                continue;
            const int tab = row.indexOf(QLatin1Char('\t'));
            const QString status = tab > 0 ? row.left(tab) : QString();
            const QString path = tab > 0 ? row.mid(tab + 1) : QString();
            const QRegularExpressionMatch match = pullEntryPath.match(path);
            if (status != QLatin1String("A") || !match.hasMatch()) {
                eligible = false;
                numbers.clear();
                break;
            }
            eligible = true;
            numbers.insert(match.captured(1).toInt());
        }
        if (!eligible || numbers.isEmpty()) {
            allConsumed = false;
            continue;
        }
        int present = 0;
        bool collision = false;
        for (const int number : std::as_const(numbers)) {
            QFile local(pullDir(number) + "/pull.md");
            if (!local.exists())
                continue;
            QString localSig;
            if (local.open(QIODevice::ReadOnly))
                localSig = signatureOf(QString::fromUtf8(local.readAll()));
            QByteArray foreignRecord;
            runGit(dir,
                   {"show", commit + QStringLiteral(":.forkmesh/pulls/%1/pull.md")
                                         .arg(number)},
                   &foreignRecord);
            const QString foreignSig =
                signatureOf(QString::fromUtf8(foreignRecord));
            // Same signed submission under the same number is this very pull,
            // already carried (however its status moved since); a different
            // signature is a numbering collision that stays foreign.
            if (!localSig.isEmpty() && localSig == foreignSig)
                ++present;
            else
                collision = true;
        }
        if (collision || (present && present < numbers.size())) {
            allConsumed = false;
            continue;
        }
        if (present == numbers.size())
            continue; // every pull in this commit is already carried
        if (!runGit(dir, {"cherry-pick", commit}, nullptr, &err,
                    kGitRewriteTimeoutMs)) {
            runGit(dir, {"cherry-pick", "--abort"}, nullptr, nullptr);
            if (error)
                *error = "could not adopt " + commit.left(12) + ": " + err;
            allConsumed = false;
            break;
        }
        for (const int number : std::as_const(numbers)) {
            adopted.append(number);
            materializePullMetadataRef(number, nullptr);
        }
    }
    // With every foreign commit accounted for, record the foreign tip as an
    // ancestor while keeping this tree exactly as adopted (-s ours). The next
    // SSH mirror pass then fast-forwards the held-back forkmesh/pulls ref and
    // the fleet converges without anyone force-pushing the ledger.
    if (allConsumed &&
        !runGit(dir,
                {"merge", "-s", "ours", "--no-edit", "-m",
                 QStringLiteral("pull ledger: consume mirror-drained history"),
                 foreign},
                nullptr, &err, kGitRewriteTimeoutMs)) {
        runGit(dir, {"merge", "--abort"}, nullptr, nullptr);
        if (error)
            *error = "could not record consumed ledger history: " + err;
    }
    std::sort(adopted.begin(), adopted.end());
    return adopted;
}


// code-health: allow(git-in-loop) one pass over the ledger's pulls: git runs only for open pulls (rev-parse of the head candidates and a merge-base --is-ancestor against the base or main) and for a merged pull whose reward still has to advance (one ledger commit each). Each head resolves through its own fallback chain (refs/pr/N/head, the mirror, origin tracking ref, creation oid), so batching is not a trivially equivalent rewrite. GUI-thread caller (pull-ledger sweep completion, agent branch merge) with runGit's keep-alive pumped waits
QList<int> PullStore::reconcileMergedPulls(QString *error,
                                           QList<int> *rewardsAdvanced)
{
    QList<int> flipped;
    if (rewardsAdvanced)
        rewardsAdvanced->clear();
    if (!canWrite())
        return flipped;
    // The landing target is the repository's integration branch, not only the
    // pull's recorded base: a stacked pull whose base branch merged first
    // still lands in main without its own base ever advancing.
    QByteArray defaultTip;
    QString defaultBranch;
    for (const QString &candidate :
         {QStringLiteral("main"), QStringLiteral("master")}) {
        if (runGit(m_workTree,
                   {"rev-parse", "--verify", "-q", candidate + "^{commit}"},
                   &defaultTip) &&
            !defaultTip.trimmed().isEmpty()) {
            defaultBranch = candidate;
            break;
        }
    }
    const QList<PullRequest> pulls = loadAll();
    for (const PullRequest &loaded : pulls) {
        if (loaded.status != QLatin1String("open")) {
            // Already merged, but its reward estimate never left pending_merge
            // — the ledger and the payout queue read that as "not payable
            // yet", so a landed contribution stays unpaid forever. Merging
            // records this itself now; this heals the records written before
            // it did.
            if (loaded.status != QLatin1String("merged"))
                continue;
            // Ask the rule against the copy loadAll() already read: a sweep
            // over a busy repository would otherwise re-read every merged
            // pull.md from disk just to learn it has nothing to settle.
            PullRequest candidate = loaded;
            if (!PullReward::advanceForMerge(candidate))
                continue;
            PullRequest stale;
            if (!readPull(loaded.number, stale) ||
                stale.status != QLatin1String("merged") ||
                !PullReward::advanceForMerge(stale))
                continue;
            if (!writePull(stale, error))
                continue;
            if (!commit(QStringLiteral("pull #%1: reward awaiting payout")
                            .arg(stale.number),
                        error))
                continue;
            if (rewardsAdvanced)
                rewardsAdvanced->append(stale.number);
            continue;
        }
        // Not only branch-backed pulls qualify: a patch-backed community pull
        // whose head branch was reconstructed in this repository (the agent
        // conflict-fix flow does exactly that) lands through that branch, and
        // resolving the recorded head name below finds it. A patch pull with
        // no such branch resolves nothing and is skipped as before.
        QString headSha;
        QByteArray sha;
        if (runGit(m_workTree,
                   {"rev-parse", "--verify", "-q",
                    QStringLiteral("refs/pr/%1/head^{commit}").arg(loaded.number)},
                   &sha) &&
            !sha.trimmed().isEmpty()) {
            headSha = QString::fromUtf8(sha).trimmed();
        }
        if (headSha.isEmpty())
            headSha = reachableHeadCommit(m_workTree, m_mirror, loaded.head);
        if (headSha.isEmpty() && !loaded.head.contains(QLatin1Char(':'))) {
            // A merged branch is usually deleted soon after it lands, but its
            // remote-tracking ref survives — still good enough to prove the
            // recorded head reached the base (how pull #91 was left open).
            QByteArray tracked;
            if (runGit(m_workTree,
                       {"rev-parse", "--verify", "-q",
                        "refs/remotes/origin/" + loaded.head + "^{commit}"},
                       &tracked) &&
                !tracked.trimmed().isEmpty())
                headSha = QString::fromUtf8(tracked).trimmed();
        }
        if (headSha.isEmpty() && validCommitOid(loaded.creationHeadOid) &&
            runGit(m_workTree,
                   {"rev-parse", "--verify", "-q",
                    loaded.creationHeadOid + "^{commit}"}))
            headSha = loaded.creationHeadOid;
        if (headSha.isEmpty())
            continue;
        QString landedIn;
        QString landedTip;
        QByteArray baseTipOut;
        if (runGit(m_workTree,
                   {"rev-parse", "--verify", "-q", loaded.base + "^{commit}"},
                   &baseTipOut) &&
            !baseTipOut.trimmed().isEmpty() &&
            runGit(m_workTree, {"merge-base", "--is-ancestor", headSha,
                                QString::fromUtf8(baseTipOut).trimmed()})) {
            landedIn = loaded.base;
            landedTip = QString::fromUtf8(baseTipOut).trimmed();
        } else if (!defaultBranch.isEmpty() && loaded.base != defaultBranch &&
                   runGit(m_workTree,
                          {"merge-base", "--is-ancestor", headSha,
                           QString::fromUtf8(defaultTip).trimmed()})) {
            landedIn = defaultBranch;
            landedTip = QString::fromUtf8(defaultTip).trimmed();
        }
        if (landedIn.isEmpty())
            continue;
        PullRequest pr;
        if (!readPull(loaded.number, pr) ||
            pr.status != QLatin1String("open"))
            continue;
        pr.status = "merged";
        pr.mergedAt = QDateTime::currentMSecsSinceEpoch();
        pr.coAuthors = coAuthorEmails(
            pr.commits + QLatin1Char('\n') +
            commitMessages(m_workTree, pr.creationBaseOid, headSha));
        const bool rewardAdvanced = PullReward::advanceForMerge(pr);
        pr.mergeBase = landedTip;
        pr.mergeHead = headSha;
        if (!writePull(pr, error))
            continue;
        if (!commit(QStringLiteral("pull #%1: merged").arg(pr.number), error))
            continue;
        if (rewardAdvanced && rewardsAdvanced)
            rewardsAdvanced->append(pr.number);
        // Deliberately no purgePullPayloadHistory here: the payload rewrite
        // walks and rewrites the whole ledger branch, which blocked the GUI
        // thread for seconds per flipped pull (2026-08-09 stalls.log). A
        // stale changes.patch in branch history costs kilobytes; an explicit
        // merge or delete still purges through its own flow.
        flipped.append(pr.number);
    }
    return flipped;
}


QString PullStore::syntheticMbox(const PullRequest &pr)
{
    const QString name = pr.authorName.trimmed().isEmpty()
                             ? QStringLiteral("ForkMesh contributor")
                             : pr.authorName.trimmed();
    const QString email = (pr.author.trimmed().isEmpty() ? QStringLiteral("contributor")
                                                         : pr.author.trimmed()) +
                          QStringLiteral("@forkmesh");
    const QDateTime when =
        QDateTime::fromMSecsSinceEpoch(pr.ts > 0 ? pr.ts : 0).toUTC();
    const QString title = pr.title.trimmed();
    QString out;
    out += "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\n";
    out += "From: " + name + " <" + email + ">\n";
    out += "Date: " + when.toString(Qt::RFC2822Date) + "\n";
    out += "Subject: [PATCH] " + (title.isEmpty() ? QStringLiteral("Pull request")
                                                  : title) +
           "\n\n";
    out += "---\n";
    QString patch = pr.patch;
    if (!patch.endsWith('\n'))
        patch += '\n';
    out += patch;
    return out;
}

bool PullStore::conflictMergeInProgress() const
{
    return (!m_workTree.isEmpty() && !amStateDir(m_workTree).isEmpty() &&
            amSessionLive(m_workTree)) ||
           (!m_editWorkTree.isEmpty() &&
            !amStateDir(m_editWorkTree).isEmpty());
}

bool PullStore::beginPullBranch(int number, QStringList *conflicted,
                                bool *cleanApply, QString *error)
{
    if (conflicted)
        conflicted->clear();
    if (cleanApply)
        *cleanApply = false;
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This needs a local working tree.");
        return false;
    }
    if (conflictMergeInProgress()) {
        if (error)
            *error = QStringLiteral("Another pull-request operation is already in "
                                    "progress; finish or cancel it first.");
        return false;
    }
    if (!healStaleAmState(m_workTree)) {
        if (error)
            *error = amBlockedMessage(m_workTree);
        return false;
    }
    QByteArray porcelain;
    runGit(m_workTree, {"status", "--porcelain"}, &porcelain);
    if (!porcelain.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Commit or stash your local changes first.");
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (pr.status != "open") {
        if (error)
            *error = forkmesh::pulldecision::decidedPullMessage(pr.status);
        return false;
    }

    QString err;
    QByteArray originalBranch;
    QByteArray originalCommit;
    runGit(m_workTree, {"rev-parse", "--abbrev-ref", "HEAD"}, &originalBranch, nullptr);
    runGit(m_workTree, {"rev-parse", "--verify", "HEAD"}, &originalCommit, nullptr);
    const QString current = QString::fromUtf8(originalBranch).trimmed();
    m_amRestoreRef = (current.isEmpty() || current == QLatin1String("HEAD"))
                         ? QString::fromUtf8(originalCommit).trimmed()
                         : current;

    QString branchName = pr.head.trimmed();
    // Cross-node submissions decorate the source as "<node>:<branch>" for
    // display.  That value is not a Git branch name (and the branch portion may
    // belong to a different mirror), so replay it onto this PR's canonical local
    // branch instead.
    if (branchName.contains(QLatin1Char(':')) || branchName.isEmpty() ||
        branchName == m_amRestoreRef ||
        branchName == pr.base.trimmed())
        branchName = QStringLiteral("pull/%1").arg(number);
    m_amBranch = branchName;

    QString startPoint = m_amRestoreRef;
    if (!pr.base.trimmed().isEmpty()) {
        QByteArray baseSha;
        if (runGit(m_workTree,
                   {"rev-parse", "--verify", "--quiet",
                    pr.base.trimmed() + "^{commit}"},
                   &baseSha) &&
            !baseSha.trimmed().isEmpty())
            startPoint = pr.base.trimmed();
    }
    QByteArray startSha;
    runGit(m_workTree, {"rev-parse", "--verify", startPoint}, &startSha, nullptr);
    m_amBase = QString::fromUtf8(startSha).trimmed();
    if (m_amBase.isEmpty())
        m_amBase = startPoint;

    const QString backupRef = amBranchBackupRef(m_amBranch);
    runGit(m_workTree, {"update-ref", "-d", backupRef}, nullptr, nullptr);
    const QString previousTip =
        resolvedCommitOid(m_workTree, QStringLiteral("refs/heads/") + m_amBranch);
    if (!previousTip.isEmpty())
        runGit(m_workTree, {"update-ref", backupRef, previousTip}, nullptr, nullptr);
    if (!runGit(m_workTree, {"checkout", "-B", m_amBranch, startPoint}, nullptr,
                &err)) {
        runGit(m_workTree, {"update-ref", "-d", backupRef}, nullptr, nullptr);
        const QString branch = m_amBranch;
        m_amBranch.clear();
        m_amBase.clear();
        m_amRestoreRef.clear();
        if (error)
            *error =
                QStringLiteral("Could not create the pull request branch %1: %2")
                    .arg(branch, err);
        return false;
    }

    const QString realMbox = pullDir(number) + "/commits.mbox";
    QString mboxPath;
    QString tempMbox;
    if (!pr.commits.isEmpty() && !pr.branchBacked && QFileInfo::exists(realMbox)) {
        mboxPath = realMbox;
    } else {
        tempMbox = QDir::temp().filePath(
            QStringLiteral("forkmesh-pull-%1.mbox").arg(number));
        if (!writeTextFile(tempMbox, pr.commits.isEmpty() ? syntheticMbox(pr)
                                                          : pr.commits,
                           error)) {
            abortConflictMerge();
            return false;
        }
        mboxPath = tempMbox;
    }

    // runGit keeps the window serviced while `git am` works, like every other
    // git step of this flow; on failure amError carries git's stderr.
    QString amError;
    const bool ok =
        runGit(m_workTree, {"am", "--3way", mboxPath}, nullptr, &amError, 60000);
    if (!tempMbox.isEmpty())
        QFile::remove(tempMbox); // git am has already copied it into its state dir

    if (ok) {
        if (cleanApply)
            *cleanApply = true;
        return true; // applied with no conflict; committed on the branch
    }
    QStringList files = unmergedFiles(m_workTree);
    if (amStateDir(m_workTree).isEmpty() || files.isEmpty()) {
        const QString detail = amError;
        abortConflictMerge();
        if (error) {
            if (detail.contains(QLatin1String("corrupt")))
                *error = QStringLiteral(
                             "This pull request's patch is corrupt, so it could "
                             "not be applied. Sync the repository to fetch an "
                             "intact copy of its commits, then try again. (%1)")
                             .arg(detail.isEmpty() ? QStringLiteral("corrupt patch")
                                                   : detail);
            else
                *error =
                    QStringLiteral("The pull request could not be applied: %1")
                        .arg(detail.isEmpty() ? QStringLiteral("patch did not apply")
                                              : detail);
        }
        return false;
    }
    setAmSessionLive(m_workTree, true);
    if (conflicted)
        *conflicted = files;
    return true;
}

bool PullStore::startConflictMerge(int number, QStringList *conflicted,
                                   bool *resolvedClean, QString *error)
{
    if (resolvedClean)
        *resolvedClean = false;
    bool clean = false;
    if (!beginPullBranch(number, conflicted, &clean, error))
        return false;
    if (clean) {
        if (resolvedClean)
            *resolvedClean = true;
        return finishConflictMerge(number, error);
    }
    return true; // conflicts left in the tree for the caller to resolve
}

bool PullStore::startConflictAgentEdit(int number, QStringList *conflicted,
                                       bool *resolvedClean, QString *error)
{
    if (resolvedClean)
        *resolvedClean = false;
    bool clean = false;
    if (!beginPullConflictWorkTree(number, conflicted, &clean, error))
        return false;
    if (clean) {
        if (resolvedClean)
            *resolvedClean = true;
        return finishConflictMerge(number, error);
    }
    return true;
}

bool PullStore::startPullFileEdit(int number, const QString &relPath,
                                  QString *content, QString *error)
{
    QStringList conflicted;
    bool clean = false;
    if (!beginPullBranch(number, &conflicted, &clean, error))
        return false;
    if (!clean) {
        abortConflictMerge();
        if (error)
            *error = QStringLiteral("This pull request has conflicts - use "
                                    "\"Resolve conflicts\" first, then edit.");
        return false;
    }
    const QString abs = m_workTree + "/" + relPath;
    QFile f(abs);
    if (!QFileInfo::exists(abs) || !f.open(QIODevice::ReadOnly)) {
        abortConflictMerge();
        if (error)
            *error = QStringLiteral("'%1' is not a file in this pull request.")
                         .arg(relPath);
        return false;
    }
    if (content)
        *content = QString::fromUtf8(f.readAll());
    f.close();
    return true; // branch left checked out; finishPullFileEdit commits the edit
}

bool PullStore::finishPullFileEdit(int number, const QString &relPath,
                                   const QString &content, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Editing needs a local working tree.");
        return false;
    }
    if (!writeTextFile(m_workTree + "/" + relPath, content, error))
        return false;
    QString err;
    if (!runGit(m_workTree, {"add", "--", relPath}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    QByteArray staged;
    runGit(m_workTree, {"diff", "--cached", "--name-only"}, &staged);
    if (staged.trimmed().isEmpty()) {
        abortConflictMerge();
        if (error)
            *error = QStringLiteral("No changes to commit.");
        return false;
    }
    if (!runGit(m_workTree,
                {"commit", "-m",
                 QStringLiteral("pull #%1: edit %2").arg(number).arg(relPath)},
                nullptr, &err)) {
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return finalizeOnPullBranch(
        number, QStringLiteral("pull #%1: edit %2").arg(number).arg(relPath),
        error);
}

QString PullStore::agentEditWorkTree() const
{
    return m_editWorkTree;
}

bool PullStore::beginPullEditWorkTree(int number, QString *error)
{
    discardPullEditWorkTree();
    if (!canWrite())
        return false; // caller falls back and reports the missing working tree
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (pr.status != "open") {
        if (error)
            *error = forkmesh::pulldecision::decidedPullMessage(pr.status);
        return false;
    }

    const QString baseRef = pr.base.trimmed();
    QString baseSha = resolvedCommitOid(m_workTree, baseRef);
    if (baseSha.isEmpty())
        baseSha = resolvedCommitOid(m_workTree, QStringLiteral("HEAD"));
    if (baseSha.isEmpty())
        return false; // nothing to branch from here — let the caller fall back

    QString branchName = pr.head.trimmed();
    if (branchName.contains(QLatin1Char(':')) || branchName == baseRef)
        branchName.clear();
    QString headSha;
    if (!branchName.isEmpty() && pr.branchBacked)
        headSha = resolvedCommitOid(m_workTree, "refs/heads/" + branchName);
    if (branchName.isEmpty())
        branchName = QStringLiteral("pull/%1").arg(number);

    const QString dir = QDir::temp().filePath(
        QStringLiteral("forkmesh-pr-edit-%1-%2")
            .arg(number)
            .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!runGit(m_workTree, {"worktree", "add", "--detach", dir,
                             headSha.isEmpty() ? baseSha : headSha})) {
        runGit(m_workTree, {"worktree", "prune"}, nullptr, nullptr);
        forkmesh::safedelete::removeTree(dir, QDir::tempPath(), nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
        return false; // no worktree available — fall back to the in-tree replay
    }
    m_editWorkTree = dir;

    if (headSha.isEmpty()) {
        const QString mboxPath =
            QDir::temp().filePath(QStringLiteral("forkmesh-pr-edit-%1.mbox").arg(number));
        const bool wrote = writeTextFile(
            mboxPath, pr.commits.isEmpty() ? syntheticMbox(pr) : pr.commits, nullptr);
        const bool applied = wrote && runGit(dir, {"am", "--3way", mboxPath});
        QFile::remove(mboxPath);
        if (!applied) {
            const bool conflicted = !unmergedFiles(dir).isEmpty();
            discardPullEditWorkTree();
            if (error)
                *error = conflicted
                             ? QStringLiteral("This pull request has conflicts - use "
                                              "\"Resolve conflicts\" first.")
                             : QStringLiteral("The pull request could not be "
                                              "applied to its base.");
            return false;
        }
    }

    m_amBranch = branchName;
    m_amBase = baseSha;
    m_amRestoreRef.clear(); // nothing was checked out in the user's tree
    return true;
}

bool PullStore::beginPullConflictWorkTree(int number, QStringList *conflicted,
                                          bool *cleanApply, QString *error)
{
    if (conflicted)
        conflicted->clear();
    if (cleanApply)
        *cleanApply = false;
    discardPullEditWorkTree();
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Conflict fixing needs a local working tree.");
        return false;
    }
    if (conflictMergeInProgress()) {
        if (error)
            *error = QStringLiteral("Another pull-request operation is already in "
                                    "progress; finish or cancel it first.");
        return false;
    }

    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (pr.status != "open") {
        if (error)
            *error = forkmesh::pulldecision::decidedPullMessage(pr.status);
        return false;
    }

    QString baseSha = resolvedCommitOid(m_workTree, pr.base.trimmed());
    if (baseSha.isEmpty())
        baseSha = resolvedCommitOid(m_workTree, QStringLiteral("HEAD"));
    if (baseSha.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not resolve the pull request base.");
        return false;
    }

    const QString dir = QDir::temp().filePath(
        QStringLiteral("forkmesh-pr-conflict-%1-%2")
            .arg(number)
            .arg(QDateTime::currentMSecsSinceEpoch()));
    QString err;
    if (!runGit(m_workTree,
                {"worktree", "add", "--detach", dir, baseSha},
                nullptr, &err)) {
        runGit(m_workTree, {"worktree", "prune"}, nullptr, nullptr);
        forkmesh::safedelete::removeTree(dir, QDir::tempPath(), nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
        if (error)
            *error = QStringLiteral(
                         "Could not create an isolated worktree for the agent: %1")
                         .arg(err);
        return false;
    }
    m_editWorkTree = dir;
    // Keep a local PR's existing head branch authoritative.  The scratch
    // worktree is only where the agent resolves the conflict; once complete,
    // move the PR branch to that resolved tip rather than inventing a parallel
    // "agent-fix" branch and changing the pull request's head.
    QString branchName = pr.head.trimmed();
    if (branchName.contains(QLatin1Char(':')) || branchName.isEmpty() ||
        branchName == pr.base.trimmed()) {
        // Cross-node source labels are not local Git refs.  Patch-only pulls
        // without a usable head need a durable local ref to receive the fix.
        branchName = QStringLiteral("pull/%1").arg(number);
    }
    m_amBranch = branchName;
    m_amBase = baseSha;
    m_amRestoreRef.clear();

    const QString mboxPath = QDir::temp().filePath(
        QStringLiteral("forkmesh-pr-conflict-%1.mbox").arg(number));
    if (!writeTextFile(mboxPath,
                       pr.commits.isEmpty() ? syntheticMbox(pr) : pr.commits,
                       error)) {
        abortConflictMerge();
        return false;
    }

    // runGit keeps the window serviced while `git am` works, like every other
    // git step of this flow; on failure amError carries git's stderr.
    QString amError;
    const bool ok = runGit(dir, {"am", "--3way", mboxPath}, nullptr, &amError, 60000);
    QFile::remove(mboxPath);
    if (ok) {
        if (cleanApply)
            *cleanApply = true;
        return true;
    }

    const QStringList files = unmergedFiles(dir);
    if (amStateDir(dir).isEmpty() || files.isEmpty()) {
        const QString detail = amError;
        abortConflictMerge();
        if (error)
            *error = QStringLiteral("The pull request could not be applied in "
                                    "the agent worktree: %1")
                         .arg(detail.isEmpty()
                                  ? QStringLiteral("patch did not apply")
                                  : detail);
        return false;
    }
    if (conflicted)
        *conflicted = files;
    return true;
}

void PullStore::discardPullEditWorkTree()
{
    if (m_editWorkTree.isEmpty())
        return;
    const QString dir = m_editWorkTree;
    m_editWorkTree.clear();
    if (!amStateDir(dir).isEmpty())
        runGit(dir, {"am", "--abort"}, nullptr, nullptr);
    runGit(m_workTree, {"worktree", "remove", "--force", dir}, nullptr, nullptr);
    forkmesh::safedelete::removeTree(dir, QDir::tempPath(), nullptr,
                                     forkmesh::safedelete::Allow::OwnWorktree);
    runGit(m_workTree, {"worktree", "prune"}, nullptr, nullptr);
}

bool PullStore::checkoutForReview(const PullRequest &pr, const QString &dir,
                                  PullReviewCheckout *out, QString *error) const
{
    if (out)
        *out = PullReviewCheckout{};
    const QString host = canWrite() ? m_workTree : m_mirror;
    if (host.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Reviewing a submission needs a local copy of "
                                    "this repository on this computer.");
        return false;
    }
    if (dir.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("No review folder was given.");
        return false;
    }

    discardReviewCheckout(dir);
    QDir().mkpath(QFileInfo(dir).absolutePath());

    // The exact base the author signed against is the most faithful thing to
    // replay onto; fall back to the named base branch, then to HEAD.
    QString baseSha = resolvedCommitOid(host, pr.creationBaseOid);
    if (baseSha.isEmpty())
        baseSha = resolvedCommitOid(host, pr.base.trimmed());
    if (baseSha.isEmpty())
        baseSha = resolvedCommitOid(host, QStringLiteral("HEAD"));
    if (baseSha.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not resolve the base this pull request "
                                    "applies to (%1).")
                         .arg(pr.base.trimmed().isEmpty()
                                  ? QStringLiteral("unnamed")
                                  : pr.base.trimmed());
        return false;
    }

    const QString headSha =
        pr.branchBacked ? reachableHeadCommit(m_workTree, m_mirror, pr.head)
                        : QString();

    QString err;
    if (!runGit(host,
                {"worktree", "add", "--detach", dir,
                 headSha.isEmpty() ? baseSha : headSha},
                nullptr, &err)) {
        runGit(host, {"worktree", "prune"}, nullptr, nullptr);
        forkmesh::safedelete::removeTree(dir, QFileInfo(dir).absolutePath(), nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
        if (error)
            *error = QStringLiteral("Could not create the review checkout: %1")
                         .arg(err);
        return false;
    }

    PullReviewCheckout result;
    result.path = dir;
    result.baseOid = baseSha;

    if (headSha.isEmpty()) {
        const QString stem =
            QStringLiteral("forkmesh-pr-review-%1-%2")
                .arg(pr.number)
                .arg(QDateTime::currentMSecsSinceEpoch());
        const QString mboxPath = QDir::temp().filePath(stem + ".mbox");
        const bool replayed =
            writeTextFile(mboxPath,
                          pr.commits.isEmpty() ? syntheticMbox(pr) : pr.commits,
                          nullptr) &&
            runGit(dir, {"am", "--3way", mboxPath});
        QFile::remove(mboxPath);
        if (!replayed) {
            if (!amStateDir(dir).isEmpty())
                runGit(dir, {"am", "--abort"}, nullptr, nullptr);
            const QString patchPath = QDir::temp().filePath(stem + ".patch");
            const bool wrote = !pr.patch.isEmpty() &&
                               writeTextFile(patchPath, pr.patch, nullptr);
            const bool applied =
                wrote && runGit(dir, {"apply", "--3way", "--whitespace=nowarn",
                                      patchPath},
                                nullptr, &err);
            const QStringList conflicts = wrote ? unmergedFiles(dir) : QStringList();
            QFile::remove(patchPath);
            if (!applied && conflicts.isEmpty()) {
                discardReviewCheckout(dir);
                if (error)
                    *error = QStringLiteral(
                                 "The pull request could not be applied to %1: %2")
                                 .arg(pr.base.trimmed().isEmpty()
                                          ? baseSha.left(8)
                                          : pr.base.trimmed(),
                                      err.isEmpty()
                                          ? QStringLiteral("no reviewable change "
                                                           "was submitted")
                                          : err);
                return false;
            }
            result.uncommitted = true;
            result.conflicts = conflicts;
        }
    }

    QByteArray tip;
    if (runGit(dir, {"rev-parse", "HEAD"}, &tip))
        result.headOid = QString::fromUtf8(tip).trimmed();
    QByteArray count;
    if (!result.uncommitted &&
        runGit(dir, {"rev-list", "--count", baseSha + "..HEAD"}, &count))
        result.commitCount = QString::fromUtf8(count).trimmed().toInt();

    QByteArray diff;
    runGit(dir,
           result.uncommitted
               ? QStringList{"diff", "--binary", "HEAD"}
               : QStringList{"diff", "--binary", baseSha + "..HEAD"},
           &diff);
    result.patch = QString::fromUtf8(diff);
    if (result.patch.trimmed().isEmpty())
        result.patch = pr.patch; // signed payload as submitted
    if (out)
        *out = result;
    return true;
}

void PullStore::discardReviewCheckout(const QString &dir) const
{
    if (dir.trimmed().isEmpty())
        return;
    const QString host = canWrite() ? m_workTree : m_mirror;
    if (QFileInfo::exists(dir) && !amStateDir(dir).isEmpty())
        runGit(dir, {"am", "--abort"}, nullptr, nullptr);
    if (!host.trimmed().isEmpty())
        runGit(host, {"worktree", "remove", "--force", dir}, nullptr, nullptr);
    forkmesh::safedelete::removeTree(dir, QFileInfo(dir).absolutePath(), nullptr,
                                     forkmesh::safedelete::Allow::OwnWorktree);
    if (!host.trimmed().isEmpty())
        runGit(host, {"worktree", "prune"}, nullptr, nullptr);
}

bool PullStore::startPullAgentEdit(int number, QString *error)
{
    QString worktreeError;
    if (beginPullEditWorkTree(number, &worktreeError))
        return true;
    if (!worktreeError.isEmpty()) {
        if (error)
            *error = worktreeError;
        return false;
    }

    QStringList conflicted;
    bool clean = false;
    if (!beginPullBranch(number, &conflicted, &clean, error))
        return false;
    if (!clean) {
        abortConflictMerge();
        if (error)
            *error = QStringLiteral("This pull request has conflicts - use "
                                    "\"Resolve conflicts\" first.");
        return false;
    }
    return true; // branch left checked out; finishPullAgentEdit commits the edits
}

bool PullStore::finishPullAgentEdit(int number, const QString &commitMsg,
                                    QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Editing needs a local working tree.");
        return false;
    }
    const QString dir = m_editWorkTree.isEmpty() ? m_workTree : m_editWorkTree;
    QString err;
    if (!runGit(dir, {"add", "-A"}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    QByteArray staged;
    runGit(dir, {"diff", "--cached", "--name-only"}, &staged);
    if (staged.trimmed().isEmpty()) {
        abortConflictMerge();
        if (error)
            *error = QStringLiteral("No changes to commit.");
        return false;
    }
    if (!runGit(dir, {"commit", "-m", commitMsg}, nullptr, &err)) {
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    if (!m_editWorkTree.isEmpty() && !moveBranchToEditTip(error))
        return false;
    return finalizeOnPullBranch(number, commitMsg, error);
}

bool PullStore::moveBranchToEditTip(QString *error)
{
    const QString tip = resolvedCommitOid(m_editWorkTree, QStringLiteral("HEAD"));
    if (tip.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not read the edited commit.");
        return false;
    }
    QString err;
    if (runGit(m_workTree, {"branch", "-f", m_amBranch, tip}, nullptr, &err))
        return true;
    if (releaseWorktreeHoldingBranch(m_workTree, m_amBranch) &&
        runGit(m_workTree, {"branch", "-f", m_amBranch, tip}, nullptr, &err))
        return true;
    QByteArray current;
    runGit(m_workTree, {"rev-parse", "--abbrev-ref", "HEAD"}, &current, nullptr);
    if (QString::fromUtf8(current).trimmed() == m_amBranch &&
        runGit(m_workTree, {"merge", "--ff-only", tip}, nullptr, &err))
        return true;
    discardPullEditWorkTree();
    if (error)
        *error = QStringLiteral("The fixes could not be put on %1: %2")
                     .arg(m_amBranch, err);
    return false;
}

bool PullStore::deletePullFile(int number, const QString &relPath, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Editing needs a local working tree.");
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    if (pr.status != "open") {
        if (error)
            *error = forkmesh::pulldecision::decidedPullMessage(pr.status);
        return false;
    }

    bool removed = false;
    const QString newPatch = removeFileSectionsFromDiff(pr.patch, relPath, &removed);
    if (!removed) {
        if (error)
            *error = QStringLiteral("'%1' is not a file in this pull request.")
                         .arg(relPath);
        return false;
    }
    if (!bodyHasFileDiff(newPatch)) {
        if (error)
            *error = QStringLiteral("'%1' is the only file changed by this pull "
                                    "request; close the pull request instead of "
                                    "deleting its last file.")
                         .arg(relPath);
        return false;
    }
    pr.patch = newPatch;
    if (!pr.commits.isEmpty())
        pr.commits = removeFileFromMbox(pr.commits, relPath);
    computeStats(pr); // refresh files/additions/deletions from the trimmed patch

    pr.branchBacked = false;
    pr.creationBaseOid.clear();
    pr.creationHeadOid.clear();
    pr.headBaseOid.clear();
    pr.headOid.clear();
    pr.mergeBase.clear();
    pr.mergeHead.clear();
    if (!writePull(pr, error))
        return false;
    if (pr.commits.isEmpty())
        QFile::remove(pullDir(number) + "/commits.mbox");
    if (!commit(QStringLiteral("pull #%1: delete %2").arg(number).arg(relPath),
               error))
        return false;
    materializePullRef(pr, nullptr);
    return true;
}

bool PullStore::finalizeOnPullBranch(int number, const QString &commitMsg,
                                     QString *error)
{
    QString err;
    if (!m_amRestoreRef.isEmpty() && m_amRestoreRef != m_amBranch &&
        !runGit(m_workTree, {"checkout", m_amRestoreRef}, nullptr, &err)) {
        if (error)
            *error = QStringLiteral("Committed on %1, but could not return to %2: %3")
                         .arg(m_amBranch, m_amRestoreRef, err);
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    const QString range = m_amBase + ".." + m_amBranch;
    QByteArray diff;
    if (!runGit(m_workTree, {"diff", "--binary", range}, &diff, &err)) {
        if (error)
            *error = "Could not refresh the pull request patch: " + err;
        return false;
    }
    QByteArray mbox;
    if (!runGit(m_workTree, {"format-patch", "--binary", "--stdout", range}, &mbox,
                &err)) {
        if (error)
            *error = "Could not refresh the pull request commits: " + err;
        return false;
    }
    pr.patch = QString::fromUtf8(diff);
    pr.commits = QString::fromUtf8(mbox);
    pr.head = m_amBranch;
    pr.headBaseOid = resolvedCommitOid(m_workTree, m_amBase);
    pr.headOid.clear();
    computeStats(pr);
    if (!writePull(pr, error))
        return false;
    discardPullEditWorkTree(); // no-op unless an agent edit ran in a scratch tree
    if (amStateDir(m_workTree).isEmpty())
        setAmSessionLive(m_workTree, false);
    m_amBranch.clear();
    m_amBase.clear();
    m_amRestoreRef.clear();
    if (!commit(commitMsg, error))
        return false;
    materializePullRef(pr, nullptr);
    return true;
}

bool PullStore::finishConflictMerge(int number, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Merging needs a local working tree.");
        return false;
    }
    const QString dir = m_editWorkTree.isEmpty() ? m_workTree : m_editWorkTree;
    for (const QString &rel : unmergedFiles(dir)) {
        QFile f(dir + "/" + rel);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        if (text.contains(QStringLiteral("\n<<<<<<< ")) ||
            text.startsWith(QStringLiteral("<<<<<<< ")) ||
            text.contains(QStringLiteral("\n>>>>>>> "))) {
            if (error)
                *error = QStringLiteral("Resolve every conflict marker in %1 first.")
                             .arg(rel);
            return false;
        }
    }
    QString err;
    if (!runGit(dir, {"add", "-A"}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    if (!amStateDir(dir).isEmpty() &&
        !runGit(dir, {"am", "--continue"}, nullptr, &err)) {
        if (error)
            *error = "Could not complete the merge: " + err;
        return false;
    }
    if (!m_editWorkTree.isEmpty() && !moveBranchToEditTip(error))
        return false;
    return finalizeOnPullBranch(
        number,
        QStringLiteral("pull #%1: resolve conflicts on %2")
            .arg(number)
            .arg(m_amBranch),
        error);
}

void PullStore::abortConflictMerge()
{
    if (m_workTree.isEmpty())
        return;
    if (!m_editWorkTree.isEmpty()) {
        discardPullEditWorkTree();
        m_amBranch.clear();
        m_amBase.clear();
        m_amRestoreRef.clear();
        return;
    }
    if (!amStateDir(m_workTree).isEmpty())
        runGit(m_workTree, {"am", "--abort"}, nullptr, nullptr);
    setAmSessionLive(m_workTree, false);
    if (!m_amRestoreRef.isEmpty() && m_amRestoreRef != m_amBranch)
        runGit(m_workTree, {"checkout", m_amRestoreRef}, nullptr, nullptr);
    if (!m_amBranch.isEmpty()) {
        const QString backupRef = amBranchBackupRef(m_amBranch);
        const QString previousTip = resolvedCommitOid(m_workTree, backupRef);
        if (!previousTip.isEmpty()) {
            if (runGit(m_workTree, {"branch", "-f", m_amBranch, previousTip}, nullptr,
                       nullptr))
                runGit(m_workTree, {"update-ref", "-d", backupRef}, nullptr, nullptr);
        } else {
            runGit(m_workTree, {"branch", "-D", m_amBranch}, nullptr, nullptr);
        }
    }
    m_amBranch.clear();
    m_amBase.clear();
    m_amRestoreRef.clear();
}

bool PullStore::checkMergeable(int number, bool *clean,
                               QStringList *conflictFiles, QString *error,
                               bool keepGuiAlive) const
{
    if (clean)
        *clean = false;
    if (conflictFiles)
        conflictFiles->clear();
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("Checking the merge needs a local working tree.");
        return false;
    }
    PullRequest pr;
    if (!readPull(number, pr)) {
        if (error)
            *error = QStringLiteral("Pull request #%1 not found.").arg(number);
        return false;
    }
    {
        const QString headSha = pullHeadCommit(pr);
        QByteArray baseSha;
        if (!headSha.isEmpty() &&
            runGit(m_workTree, {"rev-parse", "--verify", "--quiet", "HEAD"},
                   &baseSha) &&
            !baseSha.trimmed().isEmpty()) {
            QProcess git;
            git.start("git", {"-C", m_workTree, "merge-tree", "--write-tree",
                              QString::fromUtf8(baseSha).trimmed(), headSha});
            const bool finished =
                waitForFinishedKeepAlive(git, kGitTimeoutMs, keepGuiAlive);
            if (!finished) {
                if (error)
                    *error = QStringLiteral("git timed out checking the merge.");
                return false;
            }
            const bool normal = git.exitStatus() == QProcess::NormalExit;
            const int code = git.exitCode();
            if (normal && code == 0) {
                if (clean)
                    *clean = true;
                return true;
            }
            if (normal && code == 1) {
                if (conflictFiles) {
                    static const QRegularExpression stageLine(
                        QStringLiteral("^\\d+ [0-9a-f]+ [123]\t(.+)$"));
                    const QStringList lines =
                        QString::fromUtf8(git.readAllStandardOutput()).split('\n');
                    for (int i = 1; i < lines.size(); ++i) {
                        if (lines.at(i).trimmed().isEmpty())
                            break; // reached the informational-messages section
                        const QRegularExpressionMatch m = stageLine.match(lines.at(i));
                        if (m.hasMatch() && !conflictFiles->contains(m.captured(1)))
                            *conflictFiles << m.captured(1);
                    }
                }
                return true; // *clean stays false
            }
        }
    }
    QString patchPath = pullDir(number) + "/changes.patch";
    QString tempFile;
    if (!QFileInfo::exists(patchPath)) {
        if (pr.patch.trimmed().isEmpty()) {
            if (error)
                *error = QStringLiteral("This pull request has no patch to merge.");
            return false;
        }
        tempFile = QDir::temp().filePath(
            QStringLiteral("forkmesh-check-%1.patch").arg(number));
        if (!writeTextFile(tempFile, pr.patch, error))
            return false;
        patchPath = tempFile;
    }

    QProcess git;
    git.start("git",
              {"-C", m_workTree, "apply", "--check", "--3way", patchPath});
    const bool finished = waitForFinishedKeepAlive(git, kGitTimeoutMs, keepGuiAlive);
    if (!tempFile.isEmpty())
        QFile::remove(tempFile);
    if (!finished) {
        if (error)
            *error = QStringLiteral("git timed out checking the merge.");
        return false;
    }
    const QString output = QString::fromUtf8(git.readAllStandardError()) +
                           QString::fromUtf8(git.readAllStandardOutput());
    const bool exitedClean =
        git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0;
    const bool hasConflicts =
        output.contains(QStringLiteral("with conflicts"));
    if (exitedClean && !hasConflicts) {
        if (clean)
            *clean = true;
        return true;
    }
    if (conflictFiles)
        *conflictFiles = parseApplyConflicts(output);
    return true;
}

QString PullStore::baseTip() const
{
    if (!canWrite())
        return QString();
    QByteArray out;
    if (!runGit(m_workTree, {"rev-parse", "HEAD"}, &out, nullptr))
        return QString();
    return QString::fromUtf8(out).trimmed();
}

bool PullStore::applyRemotePull(const PullRequest &incoming, QString *error)
{
    if (!canWrite())
        return false;
    PullRequest pr = incoming;
    pr.headBaseOid.clear();
    pr.headOid.clear();
    // A relay reservation gives every eligible mirror the same proposed slot.
    // Deduplicate a replayed delivery first, then keep that reservation when it
    // is free. A stale catalog or concurrent local write is harmless: only an
    // occupied slot is reassigned to the next local number.
    for (const PullRequest &existing : loadAllIncludingArchived()) {
        const bool sameSig = !pr.sig.isEmpty() && existing.sig == pr.sig;
        const bool sameAuthorTs = pr.sig.isEmpty()
            && existing.author == pr.author && existing.ts == pr.ts
            && existing.title == pr.title;
        if (sameSig || sameAuthorTs)
            return true;
    }
    PullRequest occupied;
    const QList<PullRequest> reserved = loadAllIncludingArchived();
    const bool numberTaken =
        std::any_of(reserved.cbegin(), reserved.cend(), [&pr](const PullRequest &p) {
            return p.number == pr.number;
        });
    if (pr.number <= 0 || numberTaken || readPull(pr.number, occupied))
        pr.number = nextNumber();
    pr.status = "open";
    if (!writePull(pr, error))
        return false;
    if (!commit(QStringLiteral("pull #%1: opened (from %2)")
                    .arg(pr.number)
                    .arg(pr.authorName.isEmpty() ? pr.author.left(8) : pr.authorName),
                error))
        return false;
    materializePullRef(pr, nullptr);
    return true;
}


QString PullStore::missingPullMessage(int number)
{
    return QStringLiteral("Pull request #%1 not found.").arg(number);
}

bool PullStore::missingPullError(const QString &error, int number)
{
    return error.trimmed() == missingPullMessage(number);
}

void PullStore::removeMaterializedPullRefs(int number, bool keepTombstone) const
{
    const QString compatibilityRef =
        QStringLiteral("refs/pr/%1/head").arg(number);
    const QString visibleBranch =
        QStringLiteral("refs/heads/pr/%1").arg(number);
    const QString materializedTip =
        resolvedCommitOid(m_workTree, compatibilityRef);
    if (!materializedTip.isEmpty()) {
        runGit(m_workTree,
               {"update-ref", "-d", visibleBranch, materializedTip}, nullptr,
               nullptr);
        runGit(m_workTree,
               {"update-ref", "-d", compatibilityRef, materializedTip}, nullptr,
               nullptr);
    }
    runGit(m_workTree,
           {"update-ref", "-d", QStringLiteral("refs/pr/%1/metadata").arg(number)},
           nullptr, nullptr);
    if (!keepTombstone)
        runGit(m_workTree,
               {"update-ref", "-d", deletedPullBranchTombstoneRef(number)},
               nullptr, nullptr);
    const QString branchTip =
        resolvedCommitOid(m_workTree, pullMetadataBranchRef(number));
    if (branchTip.isEmpty())
        return;
    if (!keepTombstone) {
        runGit(m_workTree,
               {"update-ref", "-d", pullMetadataBranchRef(number), branchTip},
               nullptr, nullptr);
        return;
    }
    runGitWithInput(m_workTree, {"update-ref", "--stdin"},
                    QStringLiteral("start\nupdate %1 %2\ndelete %3 %2\n"
                                   "prepare\ncommit\n")
                        .arg(deletedPullBranchTombstoneRef(number), branchTip,
                             pullMetadataBranchRef(number))
                        .toUtf8(),
                    nullptr, nullptr);
}

bool PullStore::deletePull(int number, bool rewriteHistory, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    const QString dir = pullDir(number);
    // A record that is already absent still leaves its materialized refs
    // behind if an earlier pass stopped after the ledger commit. loadAll()
    // reads forkmesh/pull/<n> in preference to the aggregate index, so bailing
    // out here left the PR in the list with no way to ever remove it: every
    // retry took this same branch and touched nothing (adhoc #2091). Finish the
    // ref teardown, then report the miss so callers keep treating it as "this
    // node holds no such pull request" and carry on with their own cleanup.
    if (!QFileInfo::exists(dir)) {
        removeMaterializedPullRefs(number, !rewriteHistory);
        if (error)
            *error = missingPullMessage(number);
        return false;
    }
    const QString relPath = QStringLiteral(".forkmesh/pulls/%1").arg(number);
    QString err;
    const QString meta = metaWorkTree();
    const QString metaDir = meta.isEmpty() ? m_workTree : meta;
    if (rewriteHistory && hasUnrelatedTrackedChanges(metaDir, relPath, error))
        return false;

    runGit(metaDir, {"rm", "-r", "--ignore-unmatch", "--", relPath}, nullptr, nullptr);
    if (QDir(dir).exists() && !forkmesh::safedelete::removeTree(dir, pullsDir())) {
        if (error)
            *error = QStringLiteral("Could not remove pull request folder.");
        return false;
    }
    if (!runGit(metaDir, {"commit", "-m",
                          QStringLiteral("pull #%1: deleted").arg(number),
                          "--", relPath},
                nullptr, &err)) {
        if (!err.contains(QStringLiteral("nothing to commit")) && !err.isEmpty()) {
            if (error)
                *error = QStringLiteral("git commit failed: ") + err;
            return false;
        }
    }
    removeMaterializedPullRefs(number, !rewriteHistory);

    if (!rewriteHistory)
        return true;

    return rewritePullHistoryWithoutPaths(metaDir, number, {relPath}, error);
}

// code-health: allow(dir-walk-in-loop) only caller runs it on the QThread::create worker in deleteAllClosedAndMergedPullsAndBranches
bool PullStore::deletePulls(const QList<int> &numbers, bool rewriteHistory,
                            QList<int> *deleted, QList<int> *missing,
                            QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    QList<int> present;
    QStringList relPaths;
    for (int number : numbers) {
        if (present.contains(number))
            continue;
        if (!QFileInfo::exists(pullDir(number))) {
            // Same teardown deletePull gives an absent record, so a stranded
            // metadata ref cannot keep the pull in the list (adhoc #2091).
            removeMaterializedPullRefs(number, !rewriteHistory);
            if (missing)
                missing->append(number);
            continue;
        }
        present.append(number);
        relPaths << QStringLiteral(".forkmesh/pulls/%1").arg(number);
    }
    if (deleted)
        deleted->clear();
    if (present.isEmpty())
        return true;

    const QString meta = metaWorkTree();
    const QString metaDir = meta.isEmpty() ? m_workTree : meta;
    if (rewriteHistory) {
        // The rewrite needs a clean tree apart from the records going away;
        // one status pass answers that for the whole batch.
        QByteArray status;
        QString err;
        if (!runGit(metaDir, {"status", "--porcelain", "--untracked-files=no"},
                    &status, &err)) {
            if (error)
                *error = QStringLiteral("git status failed: ") + err;
            return false;
        }
        const QList<QByteArray> lines = status.split('\n');
        for (const QByteArray &line : lines) {
            if (line.size() < 4)
                continue;
            const QString path = QString::fromUtf8(line.mid(3)).trimmed();
            bool related = false;
            for (const QString &relPath : std::as_const(relPaths)) {
                if (path == relPath || path.startsWith(relPath + QLatin1Char('/'))) {
                    related = true;
                    break;
                }
            }
            if (!related) {
                if (error)
                    *error = QStringLiteral("Commit or stash unrelated tracked "
                                            "changes before deleting a pull "
                                            "request.");
                return false;
            }
        }
    }

    QStringList rmArgs{"rm", "-r", "--ignore-unmatch", "--"};
    rmArgs += relPaths;
    runGit(metaDir, rmArgs, nullptr, nullptr);
    for (int number : std::as_const(present)) {
        const QString dir = pullDir(number);
        if (QDir(dir).exists() && !forkmesh::safedelete::removeTree(dir, pullsDir())) {
            if (error)
                *error = QStringLiteral("Could not remove the folder of pull "
                                        "request #%1.")
                             .arg(number);
            return false;
        }
    }
    QStringList labels;
    for (int number : std::as_const(present))
        labels << QStringLiteral("#%1").arg(number);
    // Not "pull #N: …": that prefix is the per-pull ledger message whose
    // metadata ref commit() advances, and these refs are being dropped.
    QStringList commitArgs{"commit", "-m",
                           QStringLiteral("pulls: deleted %1")
                               .arg(labels.join(QStringLiteral(", "))),
                           "--"};
    commitArgs += relPaths;
    QString err;
    if (!runGit(metaDir, commitArgs, nullptr, &err)) {
        if (!err.contains(QStringLiteral("nothing to commit")) && !err.isEmpty()) {
            if (error)
                *error = QStringLiteral("git commit failed: ") + err;
            return false;
        }
    }
    for (int number : std::as_const(present))
        removeMaterializedPullRefs(number, !rewriteHistory);
    if (deleted)
        *deleted = present;

    if (!rewriteHistory)
        return true;
    // The per-pull metadata refs are gone by now, so this rewrites the shared
    // ledger ref once for every path instead of once per pull.
    return rewritePullHistoryWithoutPaths(metaDir, present.first(), relPaths,
                                          error);
}

bool PullStore::purgePullPayloadHistory(int number, QString *error) const
{
    const QString meta = metaWorkTree();
    const QString metaDir = meta.isEmpty() ? m_workTree : meta;
    const QString prefix = QStringLiteral(".forkmesh/pulls/%1/").arg(number);
    return rewritePullHistoryWithoutPaths(
        metaDir, number,
        {prefix + QStringLiteral("changes.patch"),
         prefix + QStringLiteral("commits.mbox")},
        error);
}

bool PullStore::commit(const QString &message, QString *error) const
{
    const QString meta = metaWorkTree();
    const QString dir = meta.isEmpty() ? m_workTree : meta;
    QString err;
    if (!runGit(dir, {"add", ".forkmesh/pulls"}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    if (!runGit(dir, {"commit", "-m", message, "--", ".forkmesh/pulls"}, nullptr,
                &err)) {
        if (!(err.contains("nothing to commit") || err.isEmpty())) {
            if (error)
                *error = "git commit failed: " + err;
            return false;
        }
    }

    // Every PullStore commit message begins with "pull #N". Advance only that
    // PR's metadata pointer to the new signed ledger state; this keeps comments,
    // reviews and status changes attached to their PR without adding them to the
    // PR's reviewed code branch.
    static const QRegularExpression pullNumber(
        QStringLiteral("^pull #(\\d+)(?:\\D|$)"));
    const QRegularExpressionMatch match = pullNumber.match(message);
    if (match.hasMatch() &&
        !materializePullMetadataRef(match.captured(1).toInt(), error)) {
        return false;
    }
    return true;
}


QString PullStore::mirrorRef() const
{
    QByteArray output;
    if (runGit(m_mirror,
               {"rev-parse", "--verify", "-q", "refs/heads/forkmesh/pulls^{commit}"},
               &output) &&
        !output.trimmed().isEmpty())
        return QStringLiteral("refs/heads/forkmesh/pulls");
    if (runGit(m_mirror, {"rev-parse", "--verify", "-q", "HEAD"}, &output) &&
        !output.trimmed().isEmpty())
        return QStringLiteral("HEAD");
    if (runGit(m_mirror,
               {"for-each-ref", "--format=%(refname)", "--count=1", "refs/heads/"},
               &output)) {
        const QString ref = QString::fromUtf8(output).trimmed();
        if (!ref.isEmpty())
            return ref;
    }
    return QString();
}

QByteArray PullStore::showFromMirror(const QString &repoRelPath, bool *ok,
                                     const QString &refOverride) const
{
    const QString ref = refOverride.isEmpty() ? mirrorRef() : refOverride;
    QByteArray output;
    const bool good = !ref.isEmpty() &&
                      runGit(m_mirror, {"show", ref + ":" + repoRelPath}, &output);
    if (ok)
        *ok = good;
    return good ? output : QByteArray();
}

QList<PullRequest> PullStore::loadFromMirror(QString *error, bool strict,
                                             const QString &refOverride) const
{
    QList<PullRequest> pulls;
    StrictGitReadInternal::Reader strictReader;
    auto readGit = [&](const QStringList &args, QByteArray *output = nullptr) {
        return strict ? strictReader.run(m_mirror, args, output)
                      : runGit(m_mirror, args, output);
    };
    auto readGitInput = [&](const QStringList &args, const QByteArray &input,
                            QByteArray *output) {
        return strict
                   ? strictReader.runInput(m_mirror, args, input, output)
                   : false;
    };
    auto recordStrictError = [&](const QString &message) {
        if (strict && error && error->isEmpty())
            *error = message;
    };
    if (m_mirror.isEmpty()) {
        recordStrictError(
            QStringLiteral("No local mirror to read pull metadata from."));
        return pulls;
    }
    QString ref = refOverride;
    if (ref.isEmpty() && strict) {
        QByteArray output;
        if (readGit({"rev-parse", "--verify", "-q",
                     "refs/heads/forkmesh/pulls^{commit}"},
                    &output) &&
            !output.trimmed().isEmpty()) {
            ref = QStringLiteral("refs/heads/forkmesh/pulls");
        } else if (readGit({"rev-parse", "--verify", "-q", "HEAD"},
                           &output) &&
                   !output.trimmed().isEmpty()) {
            ref = QStringLiteral("HEAD");
        } else if (readGit({"for-each-ref", "--format=%(refname)",
                            "--count=1", "refs/heads/"},
                           &output)) {
            ref = QString::fromUtf8(output).trimmed();
        }
    } else if (ref.isEmpty()) {
        ref = mirrorRef();
    }
    if (ref.isEmpty()) {
        recordStrictError(QStringLiteral("Could not resolve pull metadata HEAD."));
        return pulls;
    }
    QMap<int, QString> strictPullBases;
    QHash<QString, QStringList> strictEventNames;
    QHash<QString, QString> strictOidByPath;
    QHash<QString, QByteArray> strictContentByOid;
    QByteArray listing;
    const QStringList listingArgs =
        strict ? QStringList{QStringLiteral("ls-tree"), QStringLiteral("-r"),
                             ref, QStringLiteral(".forkmesh/pulls/")}
               : QStringList{QStringLiteral("ls-tree"), ref,
                             QStringLiteral(".forkmesh/pulls/")};
    if (!readGit(listingArgs, &listing)) {
        recordStrictError(
            QStringLiteral("Could not enumerate pull metadata."));
        return pulls;
    }

    if (strict) {
        QMap<QString, QString> relevantOids;
        const QString prefix = QStringLiteral(".forkmesh/pulls/");
        for (const QString &line :
             QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
            const int tab = line.indexOf('\t');
            if (tab < 0)
                continue;
            const QStringList meta =
                line.left(tab).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (meta.size() < 3 || meta.at(1) != QLatin1String("blob"))
                continue;
            const QString path = line.mid(tab + 1);
            if (!path.startsWith(prefix))
                continue;
            const QString rel = path.mid(prefix.size());
            const int slash = rel.indexOf(QLatin1Char('/'));
            if (slash <= 0)
                continue;
            const QString base = rel.left(slash);
            bool numeric = false;
            const int number = base.toInt(&numeric);
            if (!numeric || number <= 0 ||
                base != QString::number(number)) {
                recordStrictError(
                    QStringLiteral("A pull number is invalid."));
                continue;
            }
            const QString name = rel.mid(slash + 1);
            if (name == QLatin1String("pull.md"))
                strictPullBases.insert(number, base);
            if (name == QLatin1String("pull.md") ||
                name == QLatin1String("changes.patch") ||
                name == QLatin1String("commits.mbox") ||
                eventFileRe().match(name).hasMatch()) {
                relevantOids.insert(path, meta.at(2));
            }
        }

        QSet<QString> wantedOids;
        for (auto it = strictPullBases.constBegin();
             it != strictPullBases.constEnd(); ++it) {
            if (!strictReader.consumePullItem()) {
                recordStrictError(
                    QStringLiteral("The strict pull item limit was exceeded."));
                return pulls;
            }
            const QString base = it.value();
            const QString pullPrefix =
                QStringLiteral(".forkmesh/pulls/") + base + QLatin1Char('/');
            for (auto blob = relevantOids.lowerBound(pullPrefix);
                 blob != relevantOids.constEnd() &&
                 blob.key().startsWith(pullPrefix);
                 ++blob) {
                const QString name = blob.key().mid(pullPrefix.size());
                if (eventFileRe().match(name).hasMatch()) {
                    if (!strictReader.consumeEventItem()) {
                        recordStrictError(QStringLiteral(
                            "The strict pull event item limit was exceeded."));
                        return pulls;
                    }
                    strictEventNames[base].append(name);
                }
                strictOidByPath.insert(blob.key(), blob.value());
                wantedOids.insert(blob.value());
            }
            strictEventNames[base].sort();
        }

        if (!wantedOids.isEmpty()) {
            QStringList sortedOids = wantedOids.values();
            sortedOids.sort();
            QByteArray batchInput;
            for (const QString &oid : std::as_const(sortedOids))
                batchInput += oid.toUtf8() + '\n';
            QByteArray batch;
            if (!readGitInput({QStringLiteral("cat-file"),
                               QStringLiteral("--batch")},
                              batchInput, &batch)) {
                recordStrictError(
                    QStringLiteral("Could not read pull metadata blobs."));
                return pulls;
            }
            for (qsizetype pos = 0; pos < batch.size();) {
                const qsizetype nl = batch.indexOf('\n', pos);
                if (nl < 0) {
                    recordStrictError(QStringLiteral(
                        "Pull metadata blob framing is invalid."));
                    break;
                }
                const QList<QByteArray> header =
                    batch.mid(pos, nl - pos).split(' ');
                pos = nl + 1;
                if (header.size() != 3 ||
                    header.at(1) != QByteArrayLiteral("blob")) {
                    recordStrictError(
                        QStringLiteral("A pull metadata blob is missing."));
                    continue;
                }
                bool sizeOk = false;
                const qlonglong size = header.at(2).toLongLong(&sizeOk);
                if (!sizeOk || size < 0 || size > batch.size() - pos ||
                    pos + size >= batch.size() ||
                    batch.at(pos + size) != '\n') {
                    recordStrictError(QStringLiteral(
                        "Pull metadata blob framing is invalid."));
                    break;
                }
                strictContentByOid.insert(QString::fromUtf8(header.at(0)),
                                         batch.mid(pos, size));
                pos += size + 1;
            }
            if (strictContentByOid.size() != wantedOids.size()) {
                recordStrictError(QStringLiteral(
                    "Not all pull metadata blobs could be read."));
            }
        }
    }

    auto readBlob = [&](const QString &repoRelPath, bool *ok) {
        if (strict) {
            const QString oid = strictOidByPath.value(repoRelPath);
            const bool good = !oid.isEmpty() &&
                              strictContentByOid.contains(oid);
            if (ok)
                *ok = good;
            return good ? strictContentByOid.value(oid) : QByteArray();
        }
        QByteArray output;
        const bool good = readGit({"show", ref + ":" + repoRelPath}, &output);
        if (ok)
            *ok = good;
        return good ? output : QByteArray();
    };

    QStringList pullBases;
    if (strict) {
        pullBases = strictPullBases.values();
    } else {
        for (const QString &line :
             QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
            const int tab = line.indexOf('\t');
            if (tab < 0 || !line.contains(" tree "))
                continue;
            pullBases.append(line.mid(tab + 1).section('/', -1));
        }
    }
    for (const QString &base : std::as_const(pullBases)) {
        bool numeric = false;
        const int number = base.toInt(&numeric);
        if (!numeric)
            continue;
        bool ok = false;
        const QByteArray md =
            readBlob(".forkmesh/pulls/" + base + "/pull.md", &ok);
        if (!ok) {
            recordStrictError(
                QStringLiteral("A pull metadata file could not be read."));
            continue;
        }
        const FrontMatter fm = parseFrontMatter(md);
        qint64 timestamp = 0;
        const bool timestampOk = strictJsonSafeInteger(
            fm.get(QStringLiteral("ts")), 0, &timestamp);
        qint64 declaredNumber = 0;
        const bool declaredNumberOk = strictJsonSafeInteger(
            fm.get(QStringLiteral("number")), 1, &declaredNumber);
        const bool validFrontMatter =
            fm.valid && timestampOk && declaredNumberOk &&
            fm.get(QStringLiteral("schema")) ==
                QLatin1String("forkmesh-pull-v1") &&
            declaredNumber == number &&
            fm.values.contains(QStringLiteral("title")) &&
            fm.values.contains(QStringLiteral("base")) &&
            fm.values.contains(QStringLiteral("head")) &&
            fm.values.contains(QStringLiteral("ts")) &&
            fm.values.contains(QStringLiteral("author")) &&
            !fm.get(QStringLiteral("author")).isEmpty() &&
            fm.values.contains(QStringLiteral("sig")) &&
            !fm.get(QStringLiteral("sig")).isEmpty();
        if (!validFrontMatter) {
            recordStrictError(
                QStringLiteral("A pull metadata file is invalid."));
            if (strict)
                continue;
        }
        PullRequest pr;
        pr.number = number;
        pr.title = fm.get("title");
        pr.base = fm.get("base");
        pr.head = fm.get("head");
        pr.status = fm.values.contains("status") ? fm.get("status")
                                                  : QStringLiteral("open");
        pr.ts = strict ? timestamp : fm.num("ts");
        pr.author = fm.get("author");
        pr.authorName = fm.get("authorName");
        pr.sig = fm.get("sig");
        pr.branchBacked = fm.get("derive") == QLatin1String("branch");
        pr.creationBaseOid = fm.get("creationBaseOid");
        pr.creationHeadOid = fm.get("creationHeadOid");
        pr.headBaseOid = fm.get("headBaseOid");
        pr.headOid = fm.get("headOid");
        pr.mergeBase = fm.get("mergeBase");
        pr.mergeHead = fm.get("mergeHead");
        pr.client = fm.get("client");
        pr.clientSig = fm.get("clientSig");
        pr.description = fm.body;
        // Branch-backed open PRs carry no committed diff — reconstruct it from
        // the base/head refs the mirror already syncs. Strict contribution
        // verification also derives the immutable signed creation bytes for a
        // merged PR, but ordinary readers deliberately expose no non-open diff.
        bool derived = false;
        if (pr.branchBacked &&
            (pr.status == QLatin1String("open") || strict)) {
            if (strict) {
                derived = deriveFromImmutableOids(
                    m_mirror, pr.creationBaseOid, pr.creationHeadOid,
                    &pr.patch, &pr.commits, &strictReader);
            } else {
                const bool merged =
                    !pr.mergeBase.isEmpty() && !pr.mergeHead.isEmpty();
                const QString deriveBase = merged ? pr.mergeBase : pr.base;
                const QString deriveHead = merged ? pr.mergeHead : pr.head;
                derived = deriveFromRefs(m_mirror, deriveBase, deriveHead,
                                         &pr.patch, &pr.commits);
                if (derived)
                    pr.patchHeadOid = resolvedCommitOid(m_mirror, deriveHead);
            }
            if (strict && derived)
                pr.patchHeadOid = pr.creationHeadOid;
            if (strict && !derived) {
                recordStrictError(QStringLiteral(
                    "A branch-backed pull could not be derived from its refs."));
            }
        }
        if (pr.patchHeadOid.isEmpty())
            pr.patchHeadOid = pr.creationHeadOid;
        bool patchOk = false;
        bool mboxOk = false;
        if (pr.status == QLatin1String("open") && !derived) {
            pr.patch = QString::fromUtf8(
                readBlob(".forkmesh/pulls/" + base + "/changes.patch", &patchOk));
            const QByteArray mbox =
                readBlob(".forkmesh/pulls/" + base + "/commits.mbox", &mboxOk);
            if (mboxOk)
                pr.commits = QString::fromUtf8(mbox);
            if (strict && !pr.branchBacked && !patchOk) {
                recordStrictError(QStringLiteral(
                    "A pull request's signed patch could not be read."));
            }
            if (!patchOk && !mboxOk) {
                recordStrictError(QStringLiteral(
                    "A pull request's signed change data could not be read."));
            }
        }
        computeStats(pr);
        QStringList names;
        bool eventsEnumerated = true;
        if (strict) {
            names = strictEventNames.value(base);
        } else {
            QByteArray dirListing;
            eventsEnumerated =
                readGit({"ls-tree", ref, ".forkmesh/pulls/" + base + "/"},
                        &dirListing);
            if (eventsEnumerated) {
            for (const QString &l :
                 QString::fromUtf8(dirListing).split('\n', Qt::SkipEmptyParts)) {
                const int t = l.indexOf('\t');
                if (t < 0 || !l.contains(" blob "))
                    continue;
                const QString fname = l.mid(t + 1).section('/', -1);
                if (eventFileRe().match(fname).hasMatch())
                    names << fname;
            }
            names.sort();
            }
        }
        if (!eventsEnumerated) {
            recordStrictError(
                QStringLiteral("Could not enumerate pull event metadata."));
        }
        for (const QString &name : std::as_const(names)) {
            bool eok = false;
            const QByteArray evBytes =
                readBlob(".forkmesh/pulls/" + base + "/" + name, &eok);
            if (!eok) {
                recordStrictError(
                    QStringLiteral("A pull event file could not be read."));
                continue;
            }
            const FrontMatter eventFrontMatter = parseFrontMatter(evBytes);
            qint64 eventTimestamp = 0;
            const bool eventTimestampOk = strictJsonSafeInteger(
                eventFrontMatter.get(QStringLiteral("ts")), 0,
                &eventTimestamp);
            const bool validEvent =
                eventFrontMatter.valid && eventTimestampOk &&
                eventFrontMatter.values.contains(QStringLiteral("type")) &&
                !eventFrontMatter.get(QStringLiteral("type")).isEmpty() &&
                eventFrontMatter.values.contains(QStringLiteral("author")) &&
                !eventFrontMatter.get(QStringLiteral("author")).isEmpty() &&
                eventFrontMatter.values.contains(QStringLiteral("ts")) &&
                eventFrontMatter.values.contains(QStringLiteral("sig")) &&
                !eventFrontMatter.get(QStringLiteral("sig")).isEmpty();
            if (!validEvent) {
                recordStrictError(
                    QStringLiteral("A pull event file is invalid."));
                if (strict)
                    continue;
            }
            PullEvent event = eventFromFrontMatter(eventFrontMatter);
            if (strict)
                event.ts = eventTimestamp;
            pr.events.append(event);
        }
        pulls.append(pr);
    }
    std::sort(pulls.begin(), pulls.end(),
              [](const PullRequest &a, const PullRequest &b) { return a.number < b.number; });
    return pulls;
}

bool PullStore::isLifecycleEvent(const PullEvent &ev)
{
    return ev.type == QLatin1String("status") || ev.type == QLatin1String("delete");
}

bool PullStore::applyRemoteLifecycleEvent(int number, const PullEvent &ev, QString *error)
{
    if (!canWrite() || !isLifecycleEvent(ev))
        return false;
    PullRequest pr;
    if (!readPull(number, pr))
        return true;
    const QString status = pr.status.trimmed().toLower();
    if (status == QLatin1String("merged") || status == QLatin1String("closed-merged"))
        return true;
    if (ev.type == QLatin1String("delete"))
        return deletePull(number, /*rewriteHistory=*/false, error);
    if (ev.state != QLatin1String("closed") || status == QLatin1String("closed"))
        return true;
    const QString who = ev.authorName.isEmpty() ? ev.author.left(8) : ev.authorName;
    pr.status = QStringLiteral("closed");
    if (!writePull(pr, error))
        return false;
    return appendEvent(number, ev,
                       QStringLiteral("pull #%1: closed (from %2)").arg(number).arg(who),
                       error);
}

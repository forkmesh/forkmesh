#include "GitIndexQueue.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", qPrintable(message));
    ++failures;
}

bool git(const QString &path, const QStringList &arguments,
         QByteArray *output = nullptr)
{
    QProcess process;
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), path} + arguments);
    if (!process.waitForFinished(30000) || process.exitCode() != 0) {
        std::fprintf(stderr, "git %s: %s\n", qPrintable(arguments.join(' ')),
                     process.readAllStandardError().constData());
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

bool commitFile(const QString &repo, const QString &name, const QByteArray &bytes)
{
    return writeFile(QDir(repo).filePath(name), bytes) &&
           git(repo, {QStringLiteral("add"), name}) &&
           git(repo, {QStringLiteral("commit"), QStringLiteral("-q"),
                      QStringLiteral("-m"), name});
}

QString gitDirOf(const QString &checkout)
{
    QByteArray out;
    git(checkout, {QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")},
        &out);
    return QString::fromUtf8(out).trimmed();
}

bool makeLock(const QString &path, qint64 ageSeconds)
{
    QFile lock(path);
    if (!lock.open(QIODevice::WriteOnly))
        return false;
    bool ok = true;
    if (ageSeconds > 0)
        ok = lock.setFileTime(QDateTime::currentDateTime().addSecs(-ageSeconds),
                              QFileDevice::FileModificationTime);
    lock.close();
    return ok;
}

bool ageFile(const QString &path, qint64 ageSeconds)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    const bool ok =
        file.setFileTime(QDateTime::currentDateTime().addSecs(-ageSeconds),
                         QFileDevice::FileModificationTime);
    file.close();
    return ok;
}

std::unique_ptr<QProcess> holdOpen(const QString &path, const QString &script)
{
    auto process = std::make_unique<QProcess>();
    process->start(QStringLiteral("sh"),
                   {QStringLiteral("-c"), script, path});
    if (!process->waitForStarted(5000) || !process->waitForReadyRead(5000))
        return nullptr;
    return process;
}

void stop(std::unique_ptr<QProcess> &process)
{
    if (!process)
        return;
    process->kill();
    process->waitForFinished(5000);
    process.reset();
}

forkmesh::GitAttempt runOnce(const QString &dir, const QStringList &args,
                             std::atomic<int> *attempts = nullptr)
{
    if (attempts)
        ++*attempts;
    forkmesh::GitAttempt run;
    QProcess process;
    process.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(1000);
        run.timedOut = true;
        run.error = QStringLiteral("timed out");
        return run;
    }
    run.output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        run.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return run;
    }
    run.ok = true;
    return run;
}

void testClassification()
{
    using forkmesh::gitArgsMergeLike;
    using forkmesh::gitArgsWriteIndex;
    using forkmesh::gitSubcommand;
    using forkmesh::gitTimeoutMsForArgs;

    check(gitSubcommand({"-c", "core.quotepath=off", "-C", "/r", "merge", "main"}) ==
              QLatin1String("merge"),
          "subcommand skips -c and -C pairs");
    check(gitSubcommand({"--no-pager", "log"}) == QLatin1String("log"),
          "subcommand skips flag options");
    check(gitSubcommand({}).isEmpty(), "no subcommand in empty args");

    for (const QStringList &args :
         {QStringList{"merge", "main"}, QStringList{"merge", "--abort"},
          QStringList{"rebase", "--continue"}, QStringList{"am", "--abort"},
          QStringList{"cherry-pick", "abc"}, QStringList{"checkout", "main"},
          QStringList{"switch", "-c", "x"}, QStringList{"commit", "-m", "x"},
          QStringList{"-c", "user.name=x", "commit", "-m", "x"},
          QStringList{"reset", "--hard"}, QStringList{"add", "-A"},
          QStringList{"stash", "push"}, QStringList{"stash"},
          QStringList{"apply", "--index", "p.patch"},
          QStringList{"pull", "--ff-only"}})
        check(gitArgsWriteIndex(args),
              QStringLiteral("%1 writes the index").arg(args.join(' ')));
    for (const QStringList &args :
         {QStringList{"status", "--porcelain"}, QStringList{"diff", "--name-only"},
          QStringList{"merge-base", "--is-ancestor", "a", "b"},
          QStringList{"merge-tree", "--write-tree", "a", "b"},
          QStringList{"rev-parse", "HEAD"}, QStringList{"log", "-1"},
          QStringList{"stash", "list"}, QStringList{"stash", "show", "-p"},
          QStringList{"apply", "--check", "p.patch"},
          QStringList{"worktree", "list"}, QStringList{"fetch", "origin"}})
        check(!gitArgsWriteIndex(args),
              QStringLiteral("%1 does not take the index queue").arg(args.join(' ')));

    check(gitArgsMergeLike({"merge", "main"}) && gitArgsMergeLike({"rebase", "main"}) &&
              gitArgsMergeLike({"am", "x.mbox"}) &&
              gitArgsMergeLike({"cherry-pick", "a"}) &&
              gitArgsMergeLike({"pull"}),
          "merge, rebase, am, cherry-pick and pull are merge-like");
    check(!gitArgsMergeLike({"checkout", "main"}) &&
              !gitArgsMergeLike({"commit", "-m", "x"}) &&
              !gitArgsMergeLike({"merge-base", "a", "b"}),
          "checkout, commit and merge-base keep the short timeout");

    check(gitTimeoutMsForArgs({"-C", "/r", "merge", "main"}) ==
              forkmesh::kGitMergeTimeoutMs,
          "git merge main no longer times out after 8 s");
    check(forkmesh::kGitMergeTimeoutMs > 8000, "merge timeout is above 8 s");
    check(gitTimeoutMsForArgs({"-C", "/r", "status"}) == forkmesh::kGitDefaultTimeoutMs,
          "reads keep the 8 s timeout");
    check(gitTimeoutMsForArgs({"status"}, 15000) == 15000,
          "a caller's own default is kept for reads");
    check(gitTimeoutMsForArgs({"merge", "x"}, 600000) == 600000,
          "a caller's longer timeout is never shortened");
}

void testRepositoryKey(const QString &root)
{
    const QString repo = QDir(root).filePath(QStringLiteral("keyrepo"));
    const QString other = QDir(root).filePath(QStringLiteral("keyother"));
    const QString worktree = QDir(repo).filePath(QStringLiteral(".worktrees/feature"));
    QDir().mkpath(repo);
    QDir().mkpath(other);
    const bool ready =
        git(repo, {"init", "-q", "-b", "main"}) && commitFile(repo, "a.txt", "a\n") &&
        git(other, {"init", "-q", "-b", "main"}) &&
        git(repo, {"worktree", "add", "-q", "-b", "feature", worktree}) &&
        QDir().mkpath(QDir(worktree).filePath(QStringLiteral("sub/dir")));
    check(ready, "key fixture");
    const QString key = forkmesh::gitRepositoryKey(repo);
    check(!key.isEmpty() && key.endsWith(QLatin1String("/.git")),
          QStringLiteral("key names the common git dir (%1)").arg(key));
    check(forkmesh::gitRepositoryKey(worktree) == key,
          "a linked worktree shares its repository's queue");
    check(forkmesh::gitRepositoryKey(QDir(worktree).filePath("sub/dir")) == key,
          "a subdirectory of a worktree shares the queue");
    check(forkmesh::gitRepositoryKey(other) != key,
          "another repository has its own queue");
    check(forkmesh::gitDirForCheckout(worktree) == gitDirOf(worktree),
          "gitDirForCheckout follows the .git link of a linked worktree");
}

void testLaneSerializes(const QString &root)
{
    const QString repo = QDir(root).filePath(QStringLiteral("keyrepo"));
    const QString worktree = QDir(repo).filePath(QStringLiteral(".worktrees/feature"));
    const QString other = QDir(root).filePath(QStringLiteral("keyother"));

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    const auto hold = [&](const QString &dir, const QStringList &args) {
        const forkmesh::GitIndexWriteGuard guard(dir, args);
        const int now = ++active;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        QThread::msleep(120);
        --active;
    };
    std::vector<QThread *> threads;
    for (int i = 0; i < 4; ++i) {
        const QString dir = i % 2 ? worktree : repo;
        threads.push_back(QThread::create(
            [&hold, dir] { hold(dir, {"merge", "--no-edit", "main"}); }));
    }
    for (QThread *thread : threads)
        thread->start();
    for (QThread *thread : threads) {
        thread->wait();
        delete thread;
    }
    check(peak.load() == 1,
          QStringLiteral("index writes in one repository never overlap (peak %1)")
              .arg(peak.load()));

    {
        const forkmesh::GitIndexWriteGuard read(repo, {"status", "--porcelain"});
        check(!read.holdsLane() && !forkmesh::gitIndexLaneHeldOnThisThread(),
              "a read does not take the queue");
    }

    std::atomic<bool> otherHeld{false};
    std::atomic<bool> overlapped{false};
    QThread *first = QThread::create([&] {
        const forkmesh::GitIndexWriteGuard guard(repo, {"commit", "-m", "x"});
        QElapsedTimer timer;
        timer.start();
        while (!otherHeld.load() && !timer.hasExpired(3000))
            QThread::msleep(5);
        overlapped = otherHeld.load();
    });
    QThread *second = QThread::create([&] {
        QThread::msleep(30);
        const forkmesh::GitIndexWriteGuard guard(other, {"commit", "-m", "x"});
        otherHeld = true;
        QThread::msleep(50);
    });
    first->start();
    second->start();
    first->wait();
    second->wait();
    delete first;
    delete second;
    check(overlapped.load(), "different repositories do not wait for each other");

    QMutex orderMutex;
    QStringList order;
    std::vector<QThread *> queued;
    {
        const forkmesh::GitIndexWriteGuard owner(repo, {"merge", "main"});
        check(owner.holdsLane() && forkmesh::gitIndexLaneHeldOnThisThread(),
              "a merge holds the repository queue");
        {
            const forkmesh::GitIndexWriteGuard nested(worktree, {"checkout", "main"});
            check(nested.holdsLane(), "the owning thread re-enters its own queue");
        }
        for (int i = 0; i < 3; ++i) {
            const QString name = QString::number(i);
            QThread *thread = QThread::create([&, name] {
                const forkmesh::GitIndexWriteGuard guard(worktree, {"commit", "-m", name});
                QMutexLocker lock(&orderMutex);
                order << name;
            });
            queued.push_back(thread);
            thread->start();
            QThread::msleep(60);
        }
        QMutexLocker lock(&orderMutex);
        check(order.isEmpty(), "queued writers wait for the owner, even after it re-entered");
    }
    for (QThread *thread : queued) {
        thread->wait();
        delete thread;
    }
    check(order == QStringList({"0", "1", "2"}),
          QStringLiteral("queued writers run in arrival order (%1)").arg(order.join(',')));
    check(!forkmesh::gitIndexLaneHeldOnThisThread(), "the queue is released on scope exit");
}

void testLockState(const QString &root)
{
    const QString dir = QDir(root).filePath(QStringLiteral("locks"));
    QDir().mkpath(dir);
    const QString missing = QDir(dir).filePath(QStringLiteral("missing.lock"));
    check(forkmesh::inspectGitLock(missing) == forkmesh::GitLockState::Missing,
          "a missing lock reads as missing");

    const QString fresh = QDir(dir).filePath(QStringLiteral("fresh.lock"));
    check(makeLock(fresh, 0), "create fresh lock");
    check(forkmesh::inspectGitLock(fresh) == forkmesh::GitLockState::Fresh,
          "a new unheld lock is fresh");
    check(!forkmesh::removeGitLockIfStale(fresh) && QFileInfo::exists(fresh),
          "a fresh lock is never removed");

    const QString stale = QDir(dir).filePath(QStringLiteral("stale.lock"));
    check(makeLock(stale, 120), "create stale lock");
    check(forkmesh::inspectGitLock(stale) == forkmesh::GitLockState::Stale,
          "an old unheld lock is stale");
    check(forkmesh::removeGitLockIfStale(stale) && !QFileInfo::exists(stale),
          "a stale unheld lock is removed");

    check(forkmesh::gitLockContentionError(
              "fatal: Unable to create '/r/.git/index.lock': File exists."),
          "index.lock contention is recognised");
    check(forkmesh::gitLockContentionError(
              "error: Unable to create '/r/.git/packed-refs.new': File exists"),
          "packed-refs.new contention is recognised");
    check(!forkmesh::gitLockContentionError("fatal: Unable to write index."),
          "an index write failure is not lock contention");
    check(forkmesh::gitLockPathFromError(
              "fatal: Unable to create '/r/.git/worktrees/x/index.lock': File exists.\n\n"
              "Another git process seems to be running in this repository") ==
              QLatin1String("/r/.git/worktrees/x/index.lock"),
          "the lock path is read from git's message");

#ifdef Q_OS_LINUX
    const QString held = QDir(dir).filePath(QStringLiteral("held.lock"));
    check(makeLock(held, 0), "create held lock");
    std::unique_ptr<QProcess> holder =
        holdOpen(held, QStringLiteral("exec 3>>\"$0\"; echo ready; exec sleep 30"));
    check(holder != nullptr, "start a process holding the lock open");
    check(ageFile(held, 300), "age the held lock");
    check(forkmesh::gitLockHeldByLiveProcess(held),
          "a lock open in a live process is held");
    check(forkmesh::inspectGitLock(held) == forkmesh::GitLockState::Held,
          "an old lock with a live holder reads as held, not stale");
    check(!forkmesh::removeGitLockIfStale(held) && QFileInfo::exists(held),
          "a held lock is never removed, however old");
    stop(holder);
    check(forkmesh::inspectGitLock(held) == forkmesh::GitLockState::Stale,
          "the lock turns stale once its holder exits");
    check(forkmesh::removeGitLockIfStale(held), "and is then removed");

    const QString repo = QDir(root).filePath(QStringLiteral("keyrepo"));
    const QString worktree = QDir(repo).filePath(QStringLiteral(".worktrees/feature"));
    const QString sleepBinary = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    const QString fakeGit = QDir(root).filePath(QStringLiteral("bin/git"));
    QDir().mkpath(QDir(root).filePath(QStringLiteral("bin")));
    check(!sleepBinary.isEmpty() && QFile::copy(sleepBinary, fakeGit) &&
              QFile::setPermissions(fakeGit, QFile::ReadOwner | QFile::WriteOwner |
                                                 QFile::ExeOwner),
          "stage a process named git");
    const QString worktreeLock = gitDirOf(worktree) + QStringLiteral("/index.lock");
    const QString mainLock = gitDirOf(repo) + QStringLiteral("/index.lock");
    check(makeLock(worktreeLock, 300) && makeLock(mainLock, 300),
          "create old index locks in the main checkout and a nested worktree");
    QProcess gitInWorktree;
    gitInWorktree.setWorkingDirectory(QDir(worktree).filePath("sub/dir"));
    gitInWorktree.start(fakeGit, {QStringLiteral("30")});
    check(gitInWorktree.waitForStarted(5000), "start git inside the worktree");
    QThread::msleep(100);
    check(forkmesh::inspectGitLock(worktreeLock) == forkmesh::GitLockState::Held,
          "a git process working in the checkout holds its index.lock");
    check(forkmesh::inspectGitLock(mainLock) == forkmesh::GitLockState::Stale,
          "git in a nested worktree does not hold the enclosing checkout's lock");
    check(!forkmesh::removeGitLockIfStale(worktreeLock) && QFileInfo::exists(worktreeLock),
          "the lock of a checkout with a running git is kept");
    gitInWorktree.kill();
    gitInWorktree.waitForFinished(5000);
    check(forkmesh::removeGitLockIfStale(worktreeLock) &&
              forkmesh::removeGitLockIfStale(mainLock),
          "both locks are removed once no git runs there");
#endif
}

void testRetryPolicy(const QString &root)
{
    {
        forkmesh::GitLockRetryPolicy policy;
        check(!policy.next(QStringLiteral("CONFLICT (content): Merge conflict in a")).retry,
              "a merge conflict is never retried");
        check(!policy.next(QStringLiteral("fatal: Unable to write index.")).retry,
              "an index write failure is not retried blindly");
    }
    {
        const QString repo = QDir(root).filePath(QStringLiteral("keyrepo"));
        const QString lock = gitDirOf(repo) + QStringLiteral("/index.lock");
        QFile::remove(lock);
        forkmesh::GitLockRetryPolicy policy(repo);
        check(!policy.next(QStringLiteral("fatal: Unable to write index.")).retry,
              "merge's index write failure without an index.lock is not retried");
        check(!policy.next(QStringLiteral("fatal: unable to write index file")).retry,
              "a failed index commit is never retried");
        check(makeLock(lock, 120), "strand index.lock for merge's refusal");
        const forkmesh::GitLockRetry retry =
            policy.next(QStringLiteral("fatal: Unable to write index."));
        check(retry.retry && retry.removedLock == lock && !QFileInfo::exists(lock),
              "merge's \"Unable to write index.\" over an orphaned index.lock clears it");
    }
    {
        forkmesh::GitLockRetryPolicy policy;
        const QString err = QStringLiteral(
            "fatal: Unable to create '/nowhere/.git/HEAD.lock': File exists.");
        QList<int> waits;
        for (int i = 0; i < 10; ++i) {
            const forkmesh::GitLockRetry retry = policy.next(err);
            if (!retry.retry)
                break;
            waits << retry.waitMs;
        }
        check(waits == QList<int>({25, 50, 100, 200, 400}),
              "other lock contention keeps the short bounded backoff");
    }
    const QString dir = QDir(root).filePath(QStringLiteral("policy"));
    QDir().mkpath(dir);
    {
        const QString lock = QDir(dir).filePath(QStringLiteral("index.lock"));
        check(makeLock(lock, 120), "create orphaned index.lock");
        forkmesh::GitLockRetryPolicy policy;
        const forkmesh::GitLockRetry retry = policy.next(
            QStringLiteral("fatal: Unable to create '%1': File exists.").arg(lock));
        check(retry.retry && retry.waitMs == 0 && retry.removedLock == lock &&
                  !QFileInfo::exists(lock),
              "an orphaned index.lock is removed and the command retried at once");
    }
#ifdef Q_OS_LINUX
    {
        const QString lock = QDir(dir).filePath(QStringLiteral("index.lock"));
        check(makeLock(lock, 0), "create held index.lock");
        std::unique_ptr<QProcess> holder =
            holdOpen(lock, QStringLiteral("exec 3>>\"$0\"; echo ready; exec sleep 30"));
        check(ageFile(lock, 300), "age the held index.lock");
        forkmesh::GitLockRetryPolicy policy;
        const QString err =
            QStringLiteral("fatal: Unable to create '%1': File exists.").arg(lock);
        const forkmesh::GitLockRetry first = policy.next(err);
        const forkmesh::GitLockRetry second = policy.next(err);
        check(first.retry && first.waitMs == forkmesh::kGitHeldIndexLockRetryWaitMs &&
                  first.removedLock.isEmpty(),
              "an index.lock held by a live process is retried once after a short wait");
        check(!second.retry, "and only once");
        check(QFileInfo::exists(lock), "the held lock is left alone");
        stop(holder);
        QFile::remove(lock);
    }
#endif
}

void testRealGit(const QString &root)
{
    const QString repo = QDir(root).filePath(QStringLiteral("real"));
    QDir().mkpath(repo);
    bool ready = git(repo, {"init", "-q", "-b", "main"}) &&
                 commitFile(repo, "base.txt", "base\n");
    QStringList worktrees;
    for (int i = 0; i < 4; ++i) {
        const QString branch = QStringLiteral("agent-%1").arg(i);
        const QString path = QDir(root).filePath(QStringLiteral("real-wt-%1").arg(i));
        ready = ready && git(repo, {"worktree", "add", "-q", "-b", branch, path}) &&
                commitFile(path, QStringLiteral("agent-%1.txt").arg(i), "work\n");
        worktrees << path;
    }
    ready = ready && commitFile(repo, "main.txt", "main\n");
    check(ready, "real git fixture");

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> failed{0};
    const auto serialized = [&](const QString &dir, const QStringList &args) {
        const forkmesh::GitAttempt result = forkmesh::runGitIndexSerialized(
            dir, args, [&] {
                const int now = ++active;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                const forkmesh::GitAttempt run = runOnce(dir, args);
                --active;
                return run;
            });
        if (!result.ok) {
            ++failed;
            std::fprintf(stderr, "git %s in %s: %s\n", qPrintable(args.join(' ')),
                         qPrintable(dir), qPrintable(result.error));
        }
    };
    std::vector<QThread *> threads;
    for (const QString &path : std::as_const(worktrees))
        threads.push_back(QThread::create([&serialized, path] {
            serialized(path, {"merge", "--no-edit", "main"});
            serialized(path, {"commit", "--allow-empty", "-q", "-m", "after update"});
        }));
    threads.push_back(QThread::create([&serialized, repo] {
        for (int i = 0; i < 3; ++i)
            serialized(repo, {"commit", "--allow-empty", "-q", "-m",
                              QStringLiteral("main %1").arg(i)});
    }));
    for (QThread *thread : threads)
        thread->start();
    for (QThread *thread : threads) {
        thread->wait();
        delete thread;
    }
    check(failed.load() == 0, "concurrent updates from main and commits on main all succeed");
    check(peak.load() == 1, "the desktop never runs two index writers in one repository");
    for (const QString &path : std::as_const(worktrees))
        check(QFileInfo::exists(QDir(path).filePath("main.txt")),
              QStringLiteral("%1 was updated from main").arg(path));

    const QString worktree = worktrees.first();
    const QString indexLock = gitDirOf(worktree) + QStringLiteral("/index.lock");
    check(commitFile(repo, "second.txt", "second\n"), "advance main");
    check(makeLock(indexLock, 120), "strand an orphaned index.lock");
    std::atomic<int> attempts{0};
    forkmesh::GitAttempt merged = forkmesh::runGitIndexSerialized(
        worktree, {"merge", "--no-edit", "main"},
        [&] { return runOnce(worktree, {"merge", "--no-edit", "main"}, &attempts); });
    check(merged.ok && attempts.load() == 2 && !QFileInfo::exists(indexLock),
          QStringLiteral("a merge clears an orphaned index.lock and succeeds (%1 attempts: %2)")
              .arg(attempts.load())
              .arg(merged.error));

#ifdef Q_OS_LINUX
    check(makeLock(indexLock, 0), "create an index.lock a live process will release");
    std::unique_ptr<QProcess> briefHolder = holdOpen(
        indexLock,
        QStringLiteral("exec 3>>\"$0\"; echo ready; sleep 0.5; rm -f \"$0\"; exec sleep 30"));
    check(briefHolder != nullptr, "start the brief lock holder");
    attempts = 0;
    QElapsedTimer timer;
    timer.start();
    forkmesh::GitAttempt committed = forkmesh::runGitIndexSerialized(
        worktree, {"commit", "--allow-empty", "-q", "-m", "after wait"}, [&] {
            return runOnce(worktree, {"commit", "--allow-empty", "-q", "-m", "after wait"},
                           &attempts);
        });
    check(committed.ok && attempts.load() == 2 &&
              timer.elapsed() >= forkmesh::kGitHeldIndexLockRetryWaitMs - 100,
          QStringLiteral("a commit waits for a live index.lock holder and retries once "
                         "(%1 attempts: %2)")
              .arg(attempts.load())
              .arg(committed.error));
    stop(briefHolder);

    check(makeLock(indexLock, 0), "create an index.lock a live process keeps");
    std::unique_ptr<QProcess> holder =
        holdOpen(indexLock, QStringLiteral("exec 3>>\"$0\"; echo ready; exec sleep 30"));
    check(ageFile(indexLock, 300), "age the kept index.lock");
    attempts = 0;
    forkmesh::GitAttempt blocked = forkmesh::runGitIndexSerialized(
        worktree, {"commit", "--allow-empty", "-q", "-m", "blocked"}, [&] {
            return runOnce(worktree, {"commit", "--allow-empty", "-q", "-m", "blocked"},
                           &attempts);
        });
    check(!blocked.ok && attempts.load() == 2 && QFileInfo::exists(indexLock) &&
              blocked.error.contains(QLatin1String("index.lock")),
          QStringLiteral("a lock that stays held fails after one retry and is kept "
                         "(%1 attempts)")
              .arg(attempts.load()));
    stop(holder);
    QFile::remove(indexLock);
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    qputenv("GIT_AUTHOR_NAME", "Index Queue Test");
    qputenv("GIT_AUTHOR_EMAIL", "index-queue@example.invalid");
    qputenv("GIT_COMMITTER_NAME", "Index Queue Test");
    qputenv("GIT_COMMITTER_EMAIL", "index-queue@example.invalid");

    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary directory\n");
        return 1;
    }
    const QString root = QFileInfo(temp.path()).canonicalFilePath();

    testClassification();
    testRepositoryKey(root);
    testLaneSerializes(root);
    testLockState(root);
    testRetryPolicy(root);
    testRealGit(root);

    if (failures) {
        std::fprintf(stderr, "%d git index queue check(s) failed\n", failures);
        return 1;
    }
    std::printf("git index queue: all checks passed\n");
    return 0;
}

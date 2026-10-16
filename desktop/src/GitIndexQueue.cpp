#include "GitIndexQueue.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QWaitCondition>

#include <deque>

#ifdef Q_OS_LINUX
#include <climits>
#include <unistd.h>
#endif

namespace forkmesh {

namespace {

thread_local int t_heldLanes = 0;

int subcommandIndex(const QStringList &args)
{
    static const QSet<QString> valued = {
        QStringLiteral("-c"),          QStringLiteral("-C"),
        QStringLiteral("--git-dir"),   QStringLiteral("--work-tree"),
        QStringLiteral("--namespace"), QStringLiteral("--config-env"),
    };
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (valued.contains(arg)) {
            ++i;
            continue;
        }
        if (arg.startsWith(QLatin1Char('-')))
            continue;
        return i;
    }
    return -1;
}

QString firstOperand(const QStringList &args, int from)
{
    for (int i = from; i < args.size(); ++i) {
        if (!args.at(i).startsWith(QLatin1Char('-')))
            return args.at(i);
    }
    return QString();
}

bool onGuiThread()
{
    const QCoreApplication *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

QString cleanAbsolute(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString canonicalOrClean(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? cleanAbsolute(path) : canonical;
}

bool pathInside(const QString &path, const QString &root)
{
    return !root.isEmpty() &&
           (path == root || path.startsWith(root + QLatin1Char('/')));
}

QString readFirstLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readLine(4096)).trimmed();
}

#ifdef Q_OS_LINUX
QString readLink(const QString &path)
{
    char buffer[PATH_MAX];
    const QByteArray encoded = QFile::encodeName(path);
    const ssize_t size = ::readlink(encoded.constData(), buffer, sizeof buffer);
    if (size <= 0)
        return QString();
    return QFile::decodeName(QByteArray(buffer, static_cast<int>(size)));
}

bool linuxLockHeld(const QString &lockPath)
{
    const QString lock = canonicalOrClean(lockPath);
    const QString gitDir = canonicalOrClean(QFileInfo(lock).absolutePath());
    const QStringList pids =
        QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool numeric = false;
        pid.toLongLong(&numeric);
        if (!numeric)
            continue;
        const QString base = QStringLiteral("/proc/") + pid;
        if (readFirstLine(base + QStringLiteral("/comm")) == QLatin1String("git")) {
            const QString cwd = readLink(base + QStringLiteral("/cwd"));
            if (!cwd.isEmpty() &&
                (pathInside(cwd, gitDir) ||
                 canonicalOrClean(gitDirForCheckout(cwd)) == gitDir))
                return true;
        }
        const QDir fds(base + QStringLiteral("/fd"));
        const QStringList entries = fds.entryList(
            QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const QString &fd : entries) {
            if (readLink(fds.filePath(fd)) == lock)
                return true;
        }
    }
    return false;
}
#endif

} // namespace

QString gitSubcommand(const QStringList &args)
{
    const int at = subcommandIndex(args);
    return at < 0 ? QString() : args.at(at);
}

bool gitArgsWriteIndex(const QStringList &args)
{
    static const QSet<QString> writers = {
        QStringLiteral("merge"),       QStringLiteral("rebase"),
        QStringLiteral("am"),          QStringLiteral("cherry-pick"),
        QStringLiteral("revert"),      QStringLiteral("pull"),
        QStringLiteral("checkout"),    QStringLiteral("switch"),
        QStringLiteral("restore"),     QStringLiteral("reset"),
        QStringLiteral("commit"),      QStringLiteral("add"),
        QStringLiteral("rm"),          QStringLiteral("mv"),
        QStringLiteral("read-tree"),   QStringLiteral("update-index"),
        QStringLiteral("stash"),       QStringLiteral("apply"),
    };
    const int at = subcommandIndex(args);
    if (at < 0)
        return false;
    const QString sub = args.at(at);
    if (!writers.contains(sub))
        return false;
    if (sub == QLatin1String("stash")) {
        const QString action = firstOperand(args, at + 1);
        return action != QLatin1String("list") && action != QLatin1String("show");
    }
    if (sub == QLatin1String("apply")) {
        const QStringList rest = args.mid(at + 1);
        return rest.contains(QStringLiteral("--index")) ||
               rest.contains(QStringLiteral("--cached")) ||
               rest.contains(QStringLiteral("--3way")) ||
               rest.contains(QStringLiteral("-3"));
    }
    return true;
}

bool gitArgsMergeLike(const QStringList &args)
{
    static const QSet<QString> merges = {
        QStringLiteral("merge"),  QStringLiteral("rebase"),
        QStringLiteral("am"),     QStringLiteral("cherry-pick"),
        QStringLiteral("revert"), QStringLiteral("pull"),
    };
    return merges.contains(gitSubcommand(args));
}

int gitTimeoutMsForArgs(const QStringList &args, int defaultMs)
{
    return gitArgsMergeLike(args) ? qMax(defaultMs, kGitMergeTimeoutMs)
                                  : defaultMs;
}

QString gitDirForCheckout(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return QString();
    QDir at(cleanAbsolute(dir));
    for (int depth = 0; depth < 128; ++depth) {
        const QFileInfo dotGit(at.filePath(QStringLiteral(".git")));
        if (dotGit.isDir())
            return dotGit.absoluteFilePath();
        if (dotGit.isFile()) {
            const QString line = readFirstLine(dotGit.absoluteFilePath());
            if (!line.startsWith(QLatin1String("gitdir:")))
                return QString();
            const QString target = line.mid(7).trimmed();
            if (target.isEmpty())
                return QString();
            return QDir::isAbsolutePath(target)
                       ? QDir::cleanPath(target)
                       : QDir::cleanPath(at.absoluteFilePath(target));
        }
        if (QFileInfo(at.filePath(QStringLiteral("HEAD"))).isFile() &&
            (QFileInfo(at.filePath(QStringLiteral("objects"))).isDir() ||
             QFileInfo(at.filePath(QStringLiteral("commondir"))).isFile()))
            return at.absolutePath();
        if (!at.cdUp())
            break;
    }
    return QString();
}

QString gitRepositoryKey(const QString &dir)
{
    QString gitDir = gitDirForCheckout(dir);
    if (gitDir.isEmpty())
        return cleanAbsolute(dir);
    const QString common = readFirstLine(gitDir + QStringLiteral("/commondir"));
    if (!common.isEmpty())
        gitDir = QDir::isAbsolutePath(common) ? common
                                              : QDir(gitDir).absoluteFilePath(common);
    return canonicalOrClean(gitDir);
}

bool gitLockContentionError(const QString &err)
{
    if (!err.contains(QLatin1String("File exists"), Qt::CaseInsensitive))
        return false;
    return err.contains(QLatin1String(".lock"), Qt::CaseInsensitive) ||
           err.contains(QLatin1String("packed-refs.new"), Qt::CaseInsensitive);
}

QString gitLockPathFromError(const QString &err)
{
    static const QRegularExpression re(
        QStringLiteral("Unable to create '([^'\\n]+\\.lock)'"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(err);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool gitLockHeldByLiveProcess(const QString &lockPath)
{
    if (lockPath.isEmpty() || !QFileInfo::exists(lockPath))
        return false;
#if defined(Q_OS_LINUX)
    return linuxLockHeld(lockPath);
#elif defined(Q_OS_UNIX)
    QProcess lsof;
    lsof.start(QStringLiteral("lsof"), {QStringLiteral("-t"), QStringLiteral("--"),
                                        lockPath});
    if (!lsof.waitForStarted(1000))
        return false;
    if (!lsof.waitForFinished(3000)) {
        lsof.kill();
        lsof.waitForFinished(200);
        return true;
    }
    return !lsof.readAllStandardOutput().trimmed().isEmpty();
#else
    return false;
#endif
}

GitLockState inspectGitLock(const QString &lockPath, qint64 staleMs)
{
    const QFileInfo info(lockPath);
    if (lockPath.isEmpty() || !info.exists())
        return GitLockState::Missing;
    if (gitLockHeldByLiveProcess(lockPath))
        return GitLockState::Held;
    const qint64 age = info.lastModified().msecsTo(QDateTime::currentDateTime());
    return age >= staleMs ? GitLockState::Stale : GitLockState::Fresh;
}

bool removeGitLockIfStale(const QString &lockPath, qint64 staleMs)
{
    return inspectGitLock(lockPath, staleMs) == GitLockState::Stale &&
           QFile::remove(lockPath);
}

bool gitIndexWriteRefused(const QString &err)
{
    return err.contains(QLatin1String("Unable to write index."));
}

GitLockRetry GitLockRetryPolicy::next(const QString &error)
{
    GitLockRetry decision;
    QString lock = gitLockPathFromError(error);
    if (lock.isEmpty() && gitIndexWriteRefused(error) && !m_dir.isEmpty()) {
        const QString gitDir = gitDirForCheckout(m_dir);
        if (gitDir.isEmpty())
            return decision;
        lock = gitDir + QStringLiteral("/index.lock");
        if (!QFileInfo::exists(lock))
            return decision;
    } else if (!gitLockContentionError(error)) {
        return decision;
    }
    if (QFileInfo(lock).fileName() == QLatin1String("index.lock")) {
        switch (inspectGitLock(lock)) {
        case GitLockState::Held:
            if (m_heldRetried)
                return decision;
            m_heldRetried = true;
            decision.retry = true;
            decision.waitMs = kGitHeldIndexLockRetryWaitMs;
            return decision;
        case GitLockState::Stale:
            if (!m_removedStale && removeGitLockIfStale(lock)) {
                m_removedStale = true;
                decision.retry = true;
                decision.removedLock = lock;
                return decision;
            }
            break;
        case GitLockState::Missing:
        case GitLockState::Fresh:
            break;
        }
    }
    if (m_backoff + 1 >= kGitLockBackoffAttempts)
        return decision;
    decision.retry = true;
    decision.waitMs = 25 << m_backoff;
    ++m_backoff;
    return decision;
}

void waitBeforeGitRetry(int ms)
{
    if (ms <= 0)
        return;
    if (!onGuiThread()) {
        QThread::msleep(static_cast<unsigned long>(ms));
        return;
    }
    QElapsedTimer timer;
    timer.start();
    while (!timer.hasExpired(ms)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 12);
        QThread::msleep(8);
    }
}

struct GitIndexWriteGuard::Lane {
    QMutex mutex;
    QWaitCondition released;
    Qt::HANDLE owner = nullptr;
    int depth = 0;
    bool ownerIsGui = false;
    quint64 nextTicket = 0;
    std::deque<quint64> waiting;
};

namespace {

std::shared_ptr<GitIndexWriteGuard::Lane> laneFor(const QString &key)
{
    static QMutex registryMutex;
    static QHash<QString, std::weak_ptr<GitIndexWriteGuard::Lane>> lanes;
    QMutexLocker lock(&registryMutex);
    if (lanes.size() > 64) {
        for (auto it = lanes.begin(); it != lanes.end();)
            it = it->expired() ? lanes.erase(it) : std::next(it);
    }
    std::weak_ptr<GitIndexWriteGuard::Lane> &slot = lanes[key];
    std::shared_ptr<GitIndexWriteGuard::Lane> lane = slot.lock();
    if (!lane) {
        lane = std::make_shared<GitIndexWriteGuard::Lane>();
        slot = lane;
    }
    return lane;
}

} // namespace

GitIndexWriteGuard::GitIndexWriteGuard(const QString &dir, const QStringList &args)
{
    if (!gitArgsWriteIndex(args))
        return;
    m_key = gitRepositoryKey(dir);
    m_lane = laneFor(m_key);
    const Qt::HANDLE self = QThread::currentThreadId();
    const bool gui = onGuiThread();
    QMutexLocker lock(&m_lane->mutex);
    if (m_lane->owner == self) {
        ++m_lane->depth;
        m_holds = true;
        ++t_heldLanes;
        return;
    }
    const quint64 ticket = m_lane->nextTicket++;
    if (!gui)
        m_lane->waiting.push_back(ticket);
    QElapsedTimer waited;
    waited.start();
    while (m_lane->owner || (!gui && m_lane->waiting.front() != ticket)) {
        const qint64 elapsed = waited.elapsed();
        if (elapsed >= kGitIndexQueueWaitMs ||
            (m_lane->ownerIsGui && elapsed >= kGitIndexQueueGuiOwnerWaitMs)) {
            m_bypassed = true;
            break;
        }
        if (gui) {
            lock.unlock();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 12);
            lock.relock();
            m_lane->released.wait(&m_lane->mutex, 15);
        } else {
            m_lane->released.wait(&m_lane->mutex, 100);
        }
    }
    if (m_bypassed) {
        for (auto it = m_lane->waiting.begin(); it != m_lane->waiting.end(); ++it) {
            if (*it == ticket) {
                m_lane->waiting.erase(it);
                break;
            }
        }
        m_lane->released.wakeAll();
        lock.unlock();
        qWarning().noquote()
            << "git: index queue for" << m_key << "still busy after"
            << waited.elapsed() << "ms; running" << gitSubcommand(args)
            << "without waiting further";
        m_lane.reset();
        return;
    }
    if (!gui)
        m_lane->waiting.pop_front();
    m_lane->owner = self;
    m_lane->depth = 1;
    m_lane->ownerIsGui = gui;
    m_holds = true;
    ++t_heldLanes;
}

GitIndexWriteGuard::~GitIndexWriteGuard()
{
    if (!m_holds)
        return;
    --t_heldLanes;
    QMutexLocker lock(&m_lane->mutex);
    if (--m_lane->depth == 0) {
        m_lane->owner = nullptr;
        m_lane->ownerIsGui = false;
    }
    m_lane->released.wakeAll();
}

bool gitIndexLaneHeldOnThisThread()
{
    return t_heldLanes > 0;
}

GitAttempt runGitIndexSerialized(const QString &dir, const QStringList &args,
                                 const std::function<GitAttempt()> &attempt)
{
    const GitIndexWriteGuard lane(dir, args);
    GitLockRetryPolicy policy(dir);
    while (true) {
        GitAttempt result = attempt();
        if (result.ok || result.timedOut)
            return result;
        const GitLockRetry retry = policy.next(result.error);
        if (!retry.retry)
            return result;
        if (!retry.removedLock.isEmpty())
            qWarning().noquote() << "git: removed orphaned" << retry.removedLock
                                 << "(no git process held it) and retrying"
                                 << gitSubcommand(args);
        waitBeforeGitRetry(retry.waitMs);
    }
}

} // namespace forkmesh

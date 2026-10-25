#include "ActionRunner.h"
#include "AgentResources.h"

#include "CrashHandler.h"
#include "ProcessReaper.h"
#include "SafeDelete.h"
#include "SystemStats.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QtConcurrentRun>

#include <functional>

#ifndef Q_OS_WIN
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

constexpr qsizetype kActionProcessLogChunkBytes = 16 * 1024;
constexpr qsizetype kActionProcessLogMaxBytes = 1024 * 1024;
constexpr qsizetype kActionProcessLogTailBytes = 128 * 1024;
constexpr qsizetype kActionProcessLogMaxLineChars = 4096;
constexpr qsizetype kActionCrashOutputTailBytes = 12 * 1024;
constexpr int kActionCrashContextRefreshMs = 250;
constexpr qsizetype kActionCrashContextMaxChars = 12000;
constexpr qint64 kMaxLandedArtifactBytes = 1024LL * 1024 * 1024;
constexpr qint64 kHostRunFreeBytes = 1024LL * 1024 * 1024;

// Where a run's disposable trees live. These used to sit under QDir::tempPath(),
// which on this fleet is a size-capped tmpfs shared with every other checkout on
// the box — and the sandbox puts the read-only source, the writable clone, the
// step's home and the step's own /tmp inside it, so a build's object files land
// in RAM and compete for the same few gigabytes as unrelated work. Running out
// there does not fail loudly: a tool that cannot write a generated file may drop
// it and still report success, which is how "CI tests" reached `cmake --build`
// with a configured build directory that had no Makefile in it. Keep the trees
// beside the run's own records, on real disk.
// Log redaction replaces a variable's value wherever it appears, so a value
// short enough to occur in ordinary prose corrupts the log instead of protecting
// anything. With SSH_GATEWAY_PORT=22 exposed to the deploy workflow, pywrangler's
// "no Node >= v22 found ... (e.g. 'nvm install 22')" warning reached both the run
// log and the failure diagnostic as "no Node >= v*** found ... 'nvm install ***'"
// — every occurrence of "22" anywhere in the output, gone. Nothing this short is
// a credential, so leave it readable; anything longer is still masked.
constexpr qsizetype kMinimumRedactableSecretChars = 6;

bool valueIsWorthRedacting(const QString &value)
{
    return value.trimmed().size() >= kMinimumRedactableSecretChars;
}

QString sandboxBaseDir(const ActionStore *store)
{
    const QString base = store ? store->sandboxDir() : QString();
    if (base.isEmpty() || !QFileInfo(base).isWritable())
        return QDir::tempPath();
    return base;
}

// A finished run's tree is renamed to this prefix and deleted on a worker, so a
// crash mid-delete leaves a directory nothing will ever look at again. Both
// prefixes end in "-<ms>" so the sweeper below ages them the same way.
const QString kSandboxDiscardPrefix = QStringLiteral("forkmesh-discard-");

// A sandbox tree only belongs to a run while a live ActionRunner claims it, and
// a claim is dropped by the destructor as well as by cleanupWorktree(): a node
// that is restarted — or killed — mid-run must not leave a checkout, a clone and
// a build tree on the disk that nothing ever looks at again. Guarded by
// liveSandboxMutex() because the sweep runs on a worker thread.
QMutex &liveSandboxMutex()
{
    static QMutex mutex;
    return mutex;
}

QSet<QString> &liveSandboxRoots() // guarded by liveSandboxMutex()
{
    static QSet<QString> roots;
    return roots;
}

// Claims are keyed by the tree's directory name — "forkmesh-run-<id>-<ms>",
// unique per run — not by its path: the sweep builds paths through QDir while a
// run builds its own by concatenation, and one stray "//" between them would
// make a live run's tree look unclaimed and delete it mid-build.
void claimSandbox(const QString &root)
{
    if (root.isEmpty())
        return;
    QMutexLocker locker(&liveSandboxMutex());
    liveSandboxRoots().insert(QFileInfo(root).fileName());
}

void releaseSandboxClaim(const QString &root)
{
    if (root.isEmpty())
        return;
    QMutexLocker locker(&liveSandboxMutex());
    liveSandboxRoots().remove(QFileInfo(root).fileName());
}

bool sandboxIsClaimed(const QString &name)
{
    QMutexLocker locker(&liveSandboxMutex());
    return liveSandboxRoots().contains(name);
}

// How long an unclaimed tree is left alone. Nothing in this process needs it —
// a run claims its root before creating it — but a tree this young may belong
// to a node instance that is still starting up, and reclaiming one gigabyte a
// few minutes later costs nothing.
constexpr qint64 kSandboxReclaimGraceMs = 5 * 60 * 1000;

// The persistent build cache is the one Action mount nothing else prunes.
// ccache trims its own half, but a workflow may put anything here — a
// configured CMake tree, a generated Qt autogen directory — so the node keeps
// a hard ceiling and starts that repository's cache over rather than filling
// its disk. Walks every file in the cache, so this only ever runs on a worker.
// code-health: allow(dir-walk-in-loop) only called from the QtConcurrent::run cleanup worker in ActionRunner::cleanupWorktree()
void enforceCacheBudget(const QString &cache, qint64 maxBytes)
{
#ifndef Q_OS_WIN
    if (cache.isEmpty() || maxBytes <= 0 || !QDir(cache).exists())
        return;
    // Take the same lock a live run holds, and give up instead of waiting: a
    // cache some other run is already compiling against must not be walked,
    // let alone wiped, underneath it. Whichever run finishes next does this.
    const QByteArray lockPath =
        QFile::encodeName(cache + QStringLiteral("/.lock"));
    const int fd =
        ::open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return;
    }
    qint64 bytes = 0;
    QDirIterator it(cache,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const qint64 size = it.fileInfo().size();
        if (size > 0)
            bytes += size;
        if (bytes > maxBytes) {
            forkmesh::safedelete::removeTree(cache, QFileInfo(cache).absolutePath());
            break;
        }
    }
    ::close(fd);
#else
    Q_UNUSED(cache)
    Q_UNUSED(maxBytes)
#endif
}

QString humanBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QStringLiteral("%1 GiB")
            .arg(double(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(bytes / (1024 * 1024));
}

struct WorkspaceSnapshotCheck {
    bool ok = false;
    QString reason;
};

WorkspaceSnapshotCheck verifyWorkspaceSnapshot(const QString &workspace,
                                               const QString &expectedTree,
                                               const QString &expectedDigest)
{
    QString tree;
    QString digest;
    QString error;
    if (!ActionStore::repositoryStateDigest(
            workspace, QStringLiteral("HEAD"), &tree, &digest, &error)) {
        return {false, error};
    }
    if (tree != expectedTree || digest != expectedDigest) {
        return {false,
                QStringLiteral(
                    "the checked-out repository state no longer matches the "
                    "approved execution digest")};
    }
    return {true, QString()};
}

bool truthy(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("1") ||
           normalized == QLatin1String("true") ||
           normalized == QLatin1String("yes") ||
           normalized == QLatin1String("on");
}

bool dangerousEnvironmentName(const QString &name)
{
    static const QRegularExpression valid(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,127}$"));
    if (!valid.match(name).hasMatch())
        return true;
    const QString upper = name.toUpper();
    static const QSet<QString> exact{
        QStringLiteral("BASH_ENV"),       QStringLiteral("ENV"),
        QStringLiteral("HOME"),           QStringLiteral("IFS"),
        QStringLiteral("LD_LIBRARY_PATH"), QStringLiteral("LD_PRELOAD"),
        QStringLiteral("PATH"),           QStringLiteral("PYTHONHOME"),
        QStringLiteral("PYTHONPATH"),     QStringLiteral("SHELLOPTS"),
        QStringLiteral("BASHOPTS"),       QStringLiteral("TMPDIR"),
        QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("XDG_DATA_HOME"),
        QStringLiteral("XDG_STATE_HOME"), QStringLiteral("GIT_CONFIG"),
        QStringLiteral("GIT_CONFIG_GLOBAL"),
        QStringLiteral("GIT_CONFIG_SYSTEM"),
        QStringLiteral("GIT_CONFIG_COUNT"),
        QStringLiteral("FORKMESH_LOCAL_CHECKOUT"),
    };
    return exact.contains(upper) ||
           upper.startsWith(QLatin1String("GIT_CONFIG_KEY_")) ||
           upper.startsWith(QLatin1String("GIT_CONFIG_VALUE_"));
}

#ifndef Q_OS_WIN
void setLimit(int resource, rlim_t value)
{
    struct rlimit limit {
        value, value
    };
    ::setrlimit(resource, &limit);
}
#endif

QString oneLine(QString text)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return text.simplified();
}

QString diagnosticSafeText(const QString &input, qsizetype maxChars)
{
    QString text = input;
    QString prefix;
    if (text.size() > maxChars) {
        text = text.right(maxChars);
        prefix = QStringLiteral("...(diagnostic text truncated; showing tail)...\n");
    }

    QString out;
    out.reserve(text.size() + prefix.size());
    qsizetype col = 0;
    for (QChar ch : text) {
        const ushort u = ch.unicode();
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            out += QLatin1Char('\n');
            col = 0;
            continue;
        }
        if (ch == QLatin1Char('\t')) {
            out += ch;
            col += 4;
        } else if (u < 0x20 || (u >= 0x7f && u <= 0x9f)) {
            continue;
        } else {
            out += ch;
            ++col;
        }
        if (col >= kActionProcessLogMaxLineChars) {
            out += QStringLiteral("\n...[long line wrapped for display]...\n");
            col = 0;
        }
    }
    return prefix + out;
}

} // namespace

int actionCpuQuotaPercent()
{
    const int cores = qMax(1, QThread::idealThreadCount());
    return qBound(200, (cores - 1) * 100, 800);
}

qint64 actionScopeMemoryBytes(const ActionSandboxLimits &limits)
{
    const qint64 floorBytes =
        qMax<qint64>(64 * 1024 * 1024, limits.maxMemoryBytes);
    if (limits.maxScopeMemoryBytes > 0)
        return qMax(floorBytes, limits.maxScopeMemoryBytes);
    const qint64 perCpuBytes = 3LL * 1024 * 1024 * 1024 / 2;
    qint64 bytes = 1024LL * 1024 * 1024 +
                   qint64(qMax(1, actionCpuQuotaPercent() / 100)) * perCpuBytes;
    const qint64 host = SystemStats::totalMemoryBytes();
    if (host > 0)
        bytes = qMin(bytes, host / 2);
    return qMax(bytes, floorBytes);
}

qint64 actionAddressSpaceBytes(const ActionSandboxLimits &limits)
{
    // V8's WebAssembly trap-handler reserves a large sparse virtual range even
    // when the process uses little resident memory. Wrangler imports a WASM
    // BLAKE3/Pyodide modules on startup. A production D1 invocation fails at a
    // 24 GiB RLIMIT_AS and succeeds at 32 GiB on Node 22: applying the physical
    // memory budget as virtual-address space therefore reports a false OOM
    // before any Cloudflare request. The systemd scope's MemoryMax remains the
    // authoritative resident-memory ceiling (and swap stays disabled). Keep
    // enough sparse address headroom for runtime upgrades without granting one
    // extra byte of resident memory.
    constexpr qint64 wasmReservationFloor = 64LL * 1024 * 1024 * 1024;
    return qMax(wasmReservationFloor, actionScopeMemoryBytes(limits));
}

int actionScopeTasksMax(const ActionSandboxLimits &limits)
{
    if (limits.maxProcesses > 0)
        return qMax(8, limits.maxProcesses);
    const int cpus = qMax(1, actionCpuQuotaPercent() / 100);
    const int hostThreads = qMax(1, QThread::idealThreadCount());
    return qBound(128, 64 + cpus * hostThreads, 4096);
}

ActionRunner::ActionRunner(ActionStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    m_stepTimer = new QTimer(this);
    m_stepTimer->setSingleShot(true);
    m_stepTimer->setTimerType(Qt::PreciseTimer);
    connect(m_stepTimer, &QTimer::timeout, this,
            [this] { terminateCurrentProcess(true); });
    m_jobTimer = new QTimer(this);
    m_jobTimer->setSingleShot(true);
    connect(m_jobTimer, &QTimer::timeout, this, [this] {
        m_jobTimedOut = true;
        if (m_process && m_process->state() != QProcess::NotRunning) {
            terminateCurrentProcess(true);
        } else if (m_busy) {
            complete(false,
                     QStringLiteral("Workflow exceeded the total time limit."));
        }
    });
    m_crashContextTimer = new QTimer(this);
    m_crashContextTimer->setSingleShot(true);
    m_crashContextTimer->setInterval(kActionCrashContextRefreshMs);
    connect(m_crashContextTimer, &QTimer::timeout, this, [this] {
        if (m_busy)
            updateCrashContext();
    });
    m_terminationTimer = new QTimer(this);
    m_terminationTimer->setSingleShot(true);
    connect(m_terminationTimer, &QTimer::timeout, this, [this] {
        if (!m_process || m_process->state() == QProcess::NotRunning)
            return;
#ifndef Q_OS_WIN
        const qint64 pid = m_process->processId();
        if (pid > 0)
            ::kill(static_cast<pid_t>(-pid), SIGKILL);
#endif
        m_process->kill();
    });
}

// code-health: allow(blocking-wait) destructor; waits at most 1.5 s for an already-killed step process
ActionRunner::~ActionRunner()
{
    releaseBuildCache(); // the destructor never reaches cleanupWorktree()
    // Deleting the tree here would block the teardown on a recursive delete, so
    // drop the claim instead: the next run's sweep reclaims it rather than
    // leaving a gigabyte of checkout and build output on the disk for good.
    releaseSandboxClaim(m_sandboxRoot);
    m_stepTimer->stop();
    m_jobTimer->stop();
    m_terminationTimer->stop();
    m_crashContextTimer->stop();

    QProcess *const process = m_process;
    if (!process)
        return;
    QObject::disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning)
        terminateCurrentProcess(false);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1500);
    }
    m_process = nullptr;
    delete process;
}

void ActionRunner::setSandboxLimitsForTesting(
    const ActionSandboxLimits &limits)
{
    if (m_busy)
        return;
    m_limits = limits;
}

namespace {

// sandboxAvailable() answers with three PATH scans and a `systemctl --user
// show-environment` child process, and every queued run asks again before it
// starts — a 652 ms stall was sampled inside its QProcess::waitForFinished() on
// the GUI thread. None of what it probes (root, bwrap, systemd, a live user
// manager) changes minute to minute, so answer from a short-lived cache and let
// a genuinely changed host be picked up on the next window.
constexpr qint64 kSandboxProbeCacheMs = 60 * 1000;

QMutex &sandboxProbeMutex()
{
    static QMutex mutex;
    return mutex;
}

struct SandboxProbe {
    qint64 atMs = 0;
    bool available = false;
    QString reason;
};

SandboxProbe &sandboxProbe() // guarded by sandboxProbeMutex()
{
    static SandboxProbe probe;
    return probe;
}

} // namespace

bool ActionRunner::sandboxAvailable(QString *reason)
{
    if (reason)
        reason->clear();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker locker(&sandboxProbeMutex());
        const SandboxProbe &cached = sandboxProbe();
        if (cached.atMs > 0 && nowMs - cached.atMs < kSandboxProbeCacheMs) {
            if (reason)
                *reason = cached.reason;
            return cached.available;
        }
    }
    QString probedReason;
    const bool available = probeSandboxAvailability(&probedReason);
    {
        QMutexLocker locker(&sandboxProbeMutex());
        sandboxProbe() = {nowMs, available, probedReason};
    }
    if (reason)
        *reason = probedReason;
    return available;
}

// A run's tree is normally renamed away and deleted the moment the run lands,
// but nothing deletes it when the node stops mid-run: the destructor must not
// block a teardown on a recursive delete, and a killed process runs no cleanup
// at all. Each leftover holds a checkout, a clone and whatever the job built —
// close to a gigabyte for this repository — so a handful of them filled the
// node's disk, and every later run died inside `git clone` with "write error:
// No space left on device" (run 10372). Age is not what makes a tree safe to
// delete: a claim is. Sweep everything no live run holds, so a restart cannot
// strand a sandbox for the rest of the day.
// code-health: allow(dir-walk-in-loop) deleting the trees is the point; every caller runs this on a worker, never the GUI thread
ActionSandboxSweep ActionRunner::reclaimSandboxes(const QString &base,
                                                  qint64 graceMs)
{
    ActionSandboxSweep sweep;
    const QDir dir(base);
    if (base.isEmpty() || !dir.exists())
        return sweep;
    const qint64 grace = graceMs < 0 ? kSandboxReclaimGraceMs : graceMs;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 freeBefore = QStorageInfo(base).bytesAvailable();
    for (const QString &prefix :
         {QStringLiteral("forkmesh-run-"), kSandboxDiscardPrefix}) {
        const QStringList names =
            dir.entryList({prefix + QLatin1Char('*')},
                          QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &name : names) {
            if (sandboxIsClaimed(name))
                continue;
            const QString path = dir.absoluteFilePath(name);
            // Both prefixes end in "-<ms>"; a tree whose name says nothing is
            // aged by its own timestamp rather than kept forever.
            bool parsed = false;
            const qint64 startedMs =
                name.section(QLatin1Char('-'), -1).toLongLong(&parsed);
            const qint64 ageMs =
                nowMs - (parsed ? startedMs
                                : QFileInfo(path).lastModified().toMSecsSinceEpoch());
            if (ageMs < grace)
                continue;
            if (forkmesh::safedelete::removeTree(path, base, nullptr,
                                                 forkmesh::safedelete::Allow::OwnWorktree))
                ++sweep.removed;
        }
    }
    sweep.freeBytes = QStorageInfo(base).bytesAvailable();
    if (sweep.removed > 0 && freeBefore >= 0 && sweep.freeBytes > freeBefore)
        sweep.freedBytes = sweep.freeBytes - freeBefore;
    return sweep;
}

QSet<QString> ActionRunner::claimedSandboxNames()
{
    QMutexLocker locker(&liveSandboxMutex());
    return liveSandboxRoots();
}

QString ActionRunner::removeUnclaimedSandbox(const QString &base, const QString &path)
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    const QString root = QFileInfo(base).canonicalFilePath();
    if (root.isEmpty() || parent != root)
        return QStringLiteral("%1 is not directly inside the Actions sandbox folder").arg(path);
    if (info.isSymLink())
        return QStringLiteral("%1 is a link, not a sandbox tree").arg(path);
    if (!name.startsWith(QLatin1String("forkmesh-run-")) &&
        !name.startsWith(kSandboxDiscardPrefix))
        return QStringLiteral("%1 is not an Actions sandbox tree").arg(path);
    if (sandboxIsClaimed(name))
        return QStringLiteral("a running Action still uses %1").arg(name);
    QString why;
    if (!forkmesh::safedelete::removeTree(path, root, &why,
                                          forkmesh::safedelete::Allow::OwnWorktree))
        return why.isEmpty() ? QStringLiteral("could not remove all of %1").arg(path) : why;
    return QString();
}

bool ActionRunner::probeSandboxAvailability(QString *reason)
{
    if (reason)
        reason->clear();
#if defined(Q_OS_LINUX)
    if (::geteuid() == 0) {
        if (reason)
            *reason = QStringLiteral(
                "Actions refuse to run while the ForkMesh node is root");
        return false;
    }
    const QString bwrap =
        QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (bwrap.isEmpty()) {
        if (reason)
            *reason = QStringLiteral(
                "bubblewrap (bwrap) is required for Actions isolation");
        return false;
    }
    const QString systemdRun =
        QStandardPaths::findExecutable(QStringLiteral("systemd-run"));
    const QString systemctl =
        QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (systemdRun.isEmpty() || systemctl.isEmpty()) {
        if (reason)
            *reason = QStringLiteral(
                "systemd-run and systemctl are required for Actions cgroup "
                "limits");
        return false;
    }
    QProcess manager;
    manager.start(systemctl,
                  {QStringLiteral("--user"),
                   QStringLiteral("show-environment")});
    if (!manager.waitForStarted(1000) ||
        !manager.waitForFinished(3000) ||
        manager.exitStatus() != QProcess::NormalExit ||
        manager.exitCode() != 0) {
        manager.kill();
        if (reason)
            *reason = QStringLiteral(
                "the unprivileged node user has no active systemd user "
                "manager");
        return false;
    }
    return true;
#else
    if (reason)
        *reason = QStringLiteral(
            "this platform has no configured ForkMesh Actions sandbox");
    return false;
#endif
}

void ActionRunner::setStepStatus(int stepIndex, const QString &status)
{
    if (stepIndex < 0 || stepIndex >= m_workflow.steps.size())
        return;
    if (m_run.stepStatuses.size() != m_workflow.steps.size())
        m_run.stepStatuses = QStringList(
            m_workflow.steps.size(), ActionStepStatus::Pending);
    if (m_run.stepStatuses.at(stepIndex) == status &&
        m_run.currentStepIndex == stepIndex)
        return;
    m_run.stepStatuses[stepIndex] = status;
    m_run.currentStepIndex =
        status == ActionStepStatus::Running ? stepIndex : m_run.currentStepIndex;
    m_store->saveRun(m_run);
    emit stepChanged(m_run.id, stepIndex, status);
}

void ActionRunner::start(const ActionRun &run, const ActionWorkflow &workflow,
                         const QString &mirrorPath, const QString &workTreePath,
                         const QMap<QString, QString> &variables)
{
    if (m_busy)
        return;
    m_busy = true;
    m_stopping = false;
    m_completing = false;
    m_run = run;
    m_workflow = workflow;
    m_mirror = mirrorPath;
    m_repoWorkTree = workTreePath;
    m_variables = variables;
    m_exposedVariables = explicitWorkflowVariables();
    m_networkAllowed =
        truthy(variables.value(QStringLiteral("FORKMESH_ACTIONS_ALLOW_NETWORK")));
    m_stepIndex = 0;
    m_run.stepStatuses = QStringList(m_workflow.steps.size(),
                                     ActionStepStatus::Pending);
    m_run.currentStepIndex = -1;
    m_processOutputBytes = 0;
    m_processOutputSuppressedBytes = 0;
    m_processOutputTail.clear();
    m_processOutputLineChars = 0;
    m_crashOutputTail.clear();
    m_checkoutAttempts = 0;
    m_currentStepLabel = QStringLiteral("Checkout");
    m_currentCommand.clear();
    m_processOutputTruncated = false;
    m_processTimedOut = false;
    m_jobTimedOut = false;
    m_processStdin.clear();
    forkmesh::setTerminationSignalSurvivalEnabled(true);

    m_secrets.clear();
    for (const QString &value : m_exposedVariables.values())
        if (valueIsWorthRedacting(value))
            m_secrets.append(value);

    m_run.status = ActionStatus::Running;
    m_run.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_store->saveRun(m_run);
    emit statusChanged(m_run.id, m_run.status);

    emitLog(QString::fromUtf8("==> \xF0\x9F\x9A\x80 Workflow: %1 (%2)") // 🚀
                .arg(m_workflow.name, m_workflow.path));
    emitLog(QString::fromUtf8("==> \xF0\x9F\x93\x8D Commit:   %1 on %2") // 📍
                .arg(m_run.commit.left(12), m_run.ref));
    if (m_workflow.checkout)
        emitLog(QString::fromUtf8(
            "==> \xF0\x9F\x8C\xBF Checking out into a temporary worktree\xE2\x80\xA6")); // 🌿 …

    // An encrypted repository is served out of a temporary materialization, so
    // say plainly that the mirror went away rather than leaving a bare "fatal:
    // cannot change to '/tmp/ForkMesh-XXXXXX/repository.git'" behind a generic
    // "Checkout failed.".
    if (m_mirror.trimmed().isEmpty() || !QDir(m_mirror).exists()) {
        complete(false,
                 QStringLiteral("The served mirror for %1/%2 is no longer "
                                "available at %3.")
                     .arg(m_run.owner, m_run.name,
                          m_mirror.isEmpty() ? QStringLiteral("(unset)")
                                             : m_mirror));
        return;
    }

    QString sandboxReason;
    if (m_workflow.sandbox && !sandboxAvailable(&sandboxReason)) {
        complete(false, QStringLiteral("Actions isolation unavailable: %1.")
                            .arg(sandboxReason));
        return;
    }
    if (!m_workflow.sandbox)
        emitLog(QString::fromUtf8(
                    "==> \xE2\x9A\xA0\xEF\xB8\x8F  sandbox: false \xE2\x80\x94 "
                    "the steps run on this computer as you, outside the "
                    "Actions sandbox, with FORKMESH_LOCAL_CHECKOUT=%1.") // ⚠️ —
                    .arg(m_repoWorkTree.isEmpty() ? QStringLiteral("(none)")
                                                  : m_repoWorkTree));

    const qint64 sandboxStampMs = QDateTime::currentMSecsSinceEpoch();
    const QString sandboxBase = sandboxBaseDir(m_store);
    m_sandboxRoot = sandboxBase + QStringLiteral("/forkmesh-run-") +
                    QString::number(m_run.id) + QStringLiteral("-") +
                    QString::number(sandboxStampMs);
    m_worktree = m_sandboxRoot + QStringLiteral("/source");
    m_workspace = m_sandboxRoot + QStringLiteral("/workspace");
    m_sandboxHome = m_sandboxRoot + QStringLiteral("/home");
    m_sandboxTmp = m_sandboxRoot + QStringLiteral("/tmp");
    m_releaseStaging = m_sandboxRoot + QStringLiteral("/release-cas");
    // Claim the tree before it exists: the sweep below deletes every sandbox no
    // run holds, and this run's own must never be one of them.
    claimSandbox(m_sandboxRoot);
    if (!QDir().mkpath(m_sandboxRoot) ||
        !QDir().mkpath(m_sandboxHome) ||
        !QDir().mkpath(m_sandboxHome + QStringLiteral("/.local/bin")) ||
        !QDir().mkpath(m_sandboxTmp) ||
        !QDir().mkpath(m_releaseStaging)) {
        complete(false, QStringLiteral("Could not create the Actions sandbox."));
        return;
    }
    QFile::setPermissions(
        m_sandboxRoot,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner);

    // Everything above is discarded when the run ends, so a C++ build starts
    // with no objects and a cold ccache every time — around fifteen minutes
    // for the desktop client alone. Keep one writable directory per repository
    // outside the run's own tree and mount it at /cache, so ccache entries and
    // a configured CMake/Ninja tree live on into the next run. This is the one
    // Action mount that outlives a run: a run can write here and the next run
    // of the same repository will read it, which is why the scope is a single
    // repository and why only one run may hold it at a time.
    releaseBuildCache();
    if (!m_workflow.sandbox) {
        emitLog(QStringLiteral("==> No persistent build cache outside the "
                               "sandbox."));
    } else if (truthy(variables.value(
                   QStringLiteral("FORKMESH_ACTIONS_DISABLE_CACHE")))) {
        emitLog(QStringLiteral("==> Persistent build cache disabled by this "
                               "node's Actions policy."));
    } else {
        const QString cache = m_store->buildCacheDir(m_run);
        QString cacheReason;
        if (cache.isEmpty() ||
            !QDir().mkpath(cache + QStringLiteral("/ccache"))) {
            cacheReason = QStringLiteral("its directory is not writable");
        } else if (!acquireBuildCacheLock(cache)) {
            cacheReason =
                QStringLiteral("another run of this repository is using it");
        } else {
            m_buildCache = cache;
        }
        if (m_buildCache.isEmpty())
            emitLog(QStringLiteral("==> Building without the persistent cache: "
                                   "%1.")
                        .arg(cacheReason));
        else
            emitLog(QString::fromUtf8(
                        "==> \xE2\x99\xBB\xEF\xB8\x8F  Build cache kept between "
                        "runs at /cache (%1)") // ♻️
                        .arg(m_buildCache));
    }

    m_jobTimer->start(qMax(1000, m_limits.jobTimeoutMs));

    reclaimSandboxSpaceThenCheckout();
}

// The node's disposable trees are the one thing a run can reclaim before it
// needs the room, so sweep them and check what is left *before* the checkout
// rather than discovering the disk is full halfway through `git clone`, where
// it surfaces as "fatal: write error: No space left on device" followed by a
// truncated pack. Deleting a gigabyte-sized tree takes seconds and recorded
// stacks show recursive deletes stalling the GUI thread, so the sweep runs on a
// worker and the checkout waits for it.
void ActionRunner::reclaimSandboxSpaceThenCheckout()
{
    const int runId = m_run.id;
    const QString sandboxBase = sandboxBaseDir(m_store);
    auto *watcher = new QFutureWatcher<ActionSandboxSweep>(this);
    connect(watcher, &QFutureWatcher<ActionSandboxSweep>::finished, this,
            [this, watcher, runId] {
                const ActionSandboxSweep sweep = watcher->result();
                watcher->deleteLater();
                if (!m_busy || m_completing || m_run.id != runId)
                    return; // stopped, failed or reused while the sweep ran
                if (sweep.removed > 0)
                    emitLog(QStringLiteral(
                                "==> Reclaimed %1 disposable tree(s) left by "
                                "earlier runs (%2 freed).")
                                .arg(sweep.removed)
                                .arg(humanBytes(sweep.freedBytes)));
                const qint64 needed = requiredFreeBytes();
                if (sweep.freeBytes >= 0 && sweep.freeBytes < needed) {
                    complete(false,
                             QStringLiteral(
                                 "Not enough free disk space on this node: %1 "
                                 "available where a run needs %2. Run the "
                                 "Free disk space workflow, then run this "
                                 "again.")
                                 .arg(humanBytes(sweep.freeBytes),
                                      humanBytes(needed)));
                    return;
                }
                launchCheckout();
            });
    watcher->setFuture(QtConcurrent::run(
        [sandboxBase] { return ActionRunner::reclaimSandboxes(sandboxBase); }));
}

// The workspace quota is what a step is allowed to write, so a node with less
// than that free cannot host a run whatever it checks out.
qint64 ActionRunner::requiredFreeBytes() const
{
    if (!m_workflow.sandbox)
        return qMin<qint64>(qMax<qint64>(m_limits.maxWorkspaceBytes, 0),
                            kHostRunFreeBytes);
    return qMax<qint64>(m_limits.maxWorkspaceBytes, 0);
}

void ActionRunner::launchCheckout()
{
    if (!m_workflow.checkout) {
        if (!QDir().mkpath(m_worktree) || !QDir().mkpath(m_workspace)) {
            complete(false,
                     QStringLiteral("Could not create the Actions sandbox."));
            return;
        }
        emitLog(QString::fromUtf8(
                    "==> \xE2\x8F\xAD\xEF\xB8\x8F  No local checkout "
                    "(checkout: false); the steps get commit %1 as "
                    "FORKMESH_COMMIT.") // ⏭️
                    .arg(m_run.commit.left(12)));
        startSteps();
        return;
    }
    ++m_checkoutAttempts;
    launch(Phase::Checkout, QStringLiteral("git"),
           {QStringLiteral("-C"), m_mirror, QStringLiteral("worktree"),
            QStringLiteral("add"), QStringLiteral("--detach"), m_worktree,
            m_run.commit},
           QString());
}

bool ActionRunner::checkoutLostWorktreeRace(const QString &output)
{
    if (!output.contains(QLatin1String("worktrees/")))
        return false;
    return output.contains(QLatin1String("failed to read")) ||
           output.contains(QLatin1String("could not open")) ||
           output.contains(QLatin1String("No such file or directory"));
}

bool ActionRunner::retryCheckoutAfterWorktreeRace()
{
    constexpr int kMaxCheckoutAttempts = 3;
    if (m_checkoutAttempts >= kMaxCheckoutAttempts || m_stopping ||
        !checkoutLostWorktreeRace(QString::fromUtf8(m_crashOutputTail)))
        return false;
    m_worktree = m_sandboxRoot + QStringLiteral("/source-") +
                 QString::number(m_checkoutAttempts + 1);
    m_crashOutputTail.clear();
    emitLog(QString::fromUtf8(
                "==> \xF0\x9F\x94\x81 Another Git process changed the "
                "mirror's worktree list during the checkout; retrying (%1 of "
                "%2).")
                .arg(m_checkoutAttempts + 1)
                .arg(kMaxCheckoutAttempts));
    QTimer::singleShot(200 * m_checkoutAttempts, this, [this] {
        if (!m_busy || m_stopping || m_completing ||
            m_phase != Phase::Checkout || m_process)
            return;
        launchCheckout();
    });
    return true;
}

void ActionRunner::stop()
{
    if (!m_busy)
        return;
    m_stopping = true;
    emitLog(QString());
    emitLog(QString::fromUtf8("==> \xF0\x9F\x9B\x91 Stop requested by user.")); // 🛑

    if (m_process && m_process->state() != QProcess::NotRunning) {
        terminateCurrentProcess(false);
        return; // finished() → onProcessFinished → complete()
    }
    complete(false, QStringLiteral("Stopped."));
}

void ActionRunner::terminateCurrentProcess(bool timedOut)
{
    if (!m_process || m_process->state() == QProcess::NotRunning)
        return;
    if (timedOut)
        m_processTimedOut = true;
#if defined(Q_OS_LINUX)
    if (!m_systemdUnit.isEmpty()) {
        const QString systemctl =
            QStandardPaths::findExecutable(QStringLiteral("systemctl"));
        if (!systemctl.isEmpty()) {
            forkmesh::processreaper::startReleasedQuietly(
                systemctl, {QStringLiteral("--user"), QStringLiteral("kill"),
                            QStringLiteral("--kill-whom=all"), m_systemdUnit});
        }
    }
#endif
#ifndef Q_OS_WIN
    const qint64 pid = m_process->processId();
    if (pid > 0)
        ::kill(static_cast<pid_t>(-pid), SIGTERM);
#endif
    m_process->terminate();
    if (!m_terminationTimer->isActive())
        m_terminationTimer->start(1500);
}

void ActionRunner::launch(Phase phase, const QString &program,
                          const QStringList &args, const QString &workingDir)
{
    m_phase = phase;
    m_processOutputBytes = 0;
    m_processOutputSuppressedBytes = 0;
    m_processOutputTail.clear();
    m_processOutputLineChars = 0;
    m_processOutputTruncated = false;
    m_processTimedOut = false;
    if (m_currentStepLabel.isEmpty())
        m_currentStepLabel = phase == Phase::Checkout
                                 ? QStringLiteral("Checkout")
                                 : QStringLiteral("Step %1").arg(m_stepIndex + 1);
    if (phase != Phase::Step) {
        m_currentCommand = program;
        for (const QString &arg : args)
            m_currentCommand += QLatin1Char(' ') + arg;
    }
    updateCrashContext();
    auto *child = new QProcess(this);
    m_process = child;
    const QPointer<QProcess> process(child);
    child->setProcessChannelMode(QProcess::MergedChannels);
    if (!workingDir.isEmpty())
        child->setWorkingDirectory(workingDir);

#ifndef Q_OS_WIN
    const ActionSandboxLimits limits = m_limits;
    const bool sandboxedStep = phase == Phase::Step;
    const qint64 addressSpaceBytes = actionAddressSpaceBytes(limits);
    child->setChildProcessModifier([limits, sandboxedStep, addressSpaceBytes] {
        ::setsid();
        ::umask(0077);
        setLimit(RLIMIT_CORE, 0);
        if (!sandboxedStep)
            return;
        setLimit(RLIMIT_AS, static_cast<rlim_t>(addressSpaceBytes));
        setLimit(RLIMIT_FSIZE, static_cast<rlim_t>(
                                  qMax<qint64>(1024 * 1024,
                                               limits.maxFileBytes)));
        {
            // An unprivileged setrlimit above the inherited hard limit fails
            // outright and would leave the node's own limit in place, so clamp.
            rlim_t openFiles = static_cast<rlim_t>(qMax(32, limits.maxOpenFiles));
            struct rlimit inherited {};
            if (::getrlimit(RLIMIT_NOFILE, &inherited) == 0 &&
                inherited.rlim_max != RLIM_INFINITY &&
                openFiles > inherited.rlim_max)
                openFiles = inherited.rlim_max;
            setLimit(RLIMIT_NOFILE, openFiles);
        }
        setLimit(RLIMIT_CPU,
                 static_cast<rlim_t>(qMax(10, limits.maxCpuSeconds)));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    child->setUnixProcessParameters(QProcess::UnixProcessFlag::UseVFork);
#endif
#endif

    // Deliberately do not inherit the node process environment. This keeps
    // wallet, identity, SSH, cloud, desktop-session, and private-repository
    // credentials outside both trusted Git setup and the workflow sandbox.
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"),
               phase == Phase::Step
                   ? QStringLiteral(
                         "/home/forkmesh/.local/bin:/home/forkmesh/.cargo/bin:"
                         "/usr/local/bin:/usr/bin:/bin")
                   : QStringLiteral("/usr/local/bin:/usr/bin:/bin"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("CI"), QStringLiteral("true"));
    env.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    env.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    // With both config files taken away, git falls back to its built-in
    // "master" for every repository it creates here, and prints the long
    // init.defaultBranch advice block into the run log on each checkout. The
    // default branch is main everywhere in this project, so pin it for the
    // whole sandbox rather than per invocation.
    env.insert(QStringLiteral("GIT_CONFIG_COUNT"), QStringLiteral("1"));
    env.insert(QStringLiteral("GIT_CONFIG_KEY_0"),
               QStringLiteral("init.defaultBranch"));
    env.insert(QStringLiteral("GIT_CONFIG_VALUE_0"), QStringLiteral("main"));
    if (phase == Phase::Step) {
#if defined(Q_OS_LINUX)
        const QString runtimeDir =
            QStringLiteral("/run/user/%1").arg(::getuid());
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
        env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                   QStringLiteral("unix:path=%1/bus").arg(runtimeDir));
#endif
        if (m_workflow.sandbox) {
            env.insert(QStringLiteral("HOME"), QStringLiteral("/home/forkmesh"));
            env.insert(QStringLiteral("TMPDIR"), QStringLiteral("/tmp"));
            env.insert(QStringLiteral("XDG_CACHE_HOME"),
                       QStringLiteral("/home/forkmesh/.cache"));
            env.insert(QStringLiteral("XDG_CONFIG_HOME"),
                       QStringLiteral("/home/forkmesh/.config"));
            env.insert(QStringLiteral("XDG_DATA_HOME"),
                       QStringLiteral("/home/forkmesh/.local/share"));
            env.insert(QStringLiteral("XDG_STATE_HOME"),
                       QStringLiteral("/home/forkmesh/.local/state"));
        } else {
            const QString home = QDir::homePath();
            env.insert(QStringLiteral("HOME"), home);
            env.insert(QStringLiteral("PATH"),
                       home + QStringLiteral("/.local/bin:/usr/local/bin:/usr/bin:/bin"));
            env.insert(QStringLiteral("TMPDIR"), QStringLiteral("/tmp"));
            for (const QString &name : {QStringLiteral("USER"), QStringLiteral("LOGNAME")}) {
                const QString value = qEnvironmentVariable(name.toLatin1().constData());
                if (!value.isEmpty())
                    env.insert(name, value);
            }
            if (!m_repoWorkTree.isEmpty())
                env.insert(QStringLiteral("FORKMESH_LOCAL_CHECKOUT"), m_repoWorkTree);
        }
        env.insert(QStringLiteral("FORKMESH_ACTIONS_CPUS"),
                   QString::number(qMax(1, actionCpuQuotaPercent() / 100)));
        env.insert(QStringLiteral("FORKMESH_ACTIONS_MEMORY_MB"),
                   QString::number(qMax<qint64>(
                       64, actionScopeMemoryBytes(m_limits) / (1024 * 1024))));
        if (!m_buildCache.isEmpty()) {
            // Point ccache at the surviving mount by default, so a build that
            // simply runs cmake/make is already cached without the workflow
            // knowing this directory exists. Half the ceiling is ccache's;
            // enforceCacheBudget() is the backstop for the other half. These
            // land before the workflow's own env below, which may override
            // them.
            const qint64 ccacheGiB =
                qMax<qint64>(1, m_limits.maxCacheBytes / 2 /
                                    (1024LL * 1024 * 1024));
            env.insert(QStringLiteral("FORKMESH_ACTIONS_CACHE"),
                       QStringLiteral("/cache"));
            env.insert(QStringLiteral("CCACHE_DIR"),
                       QStringLiteral("/cache/ccache"));
            env.insert(QStringLiteral("CCACHE_TEMPDIR"),
                       QStringLiteral("/tmp"));
            env.insert(QStringLiteral("CCACHE_MAXSIZE"),
                       QStringLiteral("%1G").arg(ccacheGiB));
        }
        for (auto it = m_exposedVariables.constBegin();
             it != m_exposedVariables.constEnd(); ++it)
            env.insert(it.key(), it.value());
        for (auto it = m_workflow.env.constBegin();
             it != m_workflow.env.constEnd(); ++it) {
            if (!dangerousEnvironmentName(it.key()))
                env.insert(it.key(),
                           ActionFile::substitute(it.value(),
                                                  m_exposedVariables));
        }
        env.insert(QStringLiteral("FORKMESH_COMMIT"), m_run.commit);
        env.insert(QStringLiteral("FORKMESH_REF"), m_run.ref);
        env.insert(QStringLiteral("FORKMESH_RUN_ID"),
                   QString::number(m_run.id));
        if (m_run.ref.startsWith(QLatin1String("refs/tags/")))
            env.insert(QStringLiteral("FORKMESH_TAG"),
                       m_run.ref.mid(QStringLiteral("refs/tags/").size()));
        if (isReleaseRun()) {
            env.insert(QStringLiteral("FORKMESH_RELEASE_CAS"),
                       QStringLiteral("/release-cas"));
            if (!m_run.owner.isEmpty() && !m_run.name.isEmpty())
                env.insert(QStringLiteral("FORKMESH_REPO"),
                           m_run.owner + QLatin1Char('/') + m_run.name);
        }
    } else {
        env.insert(QStringLiteral("HOME"), m_sandboxHome);
        env.insert(QStringLiteral("TMPDIR"), m_sandboxTmp);
    }
    child->setProcessEnvironment(env);

    connect(child, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (!process || m_process != process.data())
            return;
        emitProcessOutput(process->readAllStandardOutput());
    });
    connect(child, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
                if (!process || m_process != process.data())
                    return;
                onProcessFinished(exitCode,
                                  status == QProcess::CrashExit);
            });
    connect(child, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError) {
                if (!process || m_process != process.data())
                    return;
                emitLog(QString::fromUtf8("!! \xE2\x9A\xA0\xEF\xB8\x8F  ") + // ⚠️
                        process->errorString());
            });
    connect(child, &QProcess::started, this, [this, process] {
        if (!process || m_process != process.data())
            return;
        if (!m_processStdin.isEmpty()) {
            process->write(m_processStdin);
            process->closeWriteChannel();
            m_processStdin.clear();
        }
    });

    child->setProgram(program);
    child->setArguments(args);
    child->start();
    const int timeoutMs =
        phase == Phase::Step
            ? currentStepTimeoutMs()
            : qMax(30000, qMin(m_limits.jobTimeoutMs, 5 * 60 * 1000));
    m_stepTimer->start(timeoutMs);
}

QMap<QString, QString> ActionRunner::explicitWorkflowVariables() const
{
    QSet<QString> referenced;
    static const QList<QRegularExpression> references{
        QRegularExpression(
            QStringLiteral(
                "\\$\\{\\{\\s*vars\\.([A-Za-z_][A-Za-z0-9_]*)\\s*\\}\\}")),
        QRegularExpression(
            QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}")),
        QRegularExpression(
            QStringLiteral("\\$([A-Za-z_][A-Za-z0-9_]*)")),
    };
    const auto collectReferences = [&referenced](const QString &text) {
        for (const QRegularExpression &expression : references) {
            auto matches = expression.globalMatch(text);
            while (matches.hasNext())
                referenced.insert(matches.next().captured(1));
        }
    };
    collectReferences(m_workflow.content);
    for (const QString &value : m_workflow.env)
        collectReferences(value);

    QMap<QString, QString> exposed;
    for (const QString &name : std::as_const(referenced)) {
        if (name == QLatin1String("FORKMESH_ACTIONS_ALLOW_NETWORK") ||
            name == QLatin1String("FORKMESH_ACTIONS_DISABLE_CACHE") ||
            dangerousEnvironmentName(name) || !m_variables.contains(name))
            continue;
        exposed.insert(name, m_variables.value(name));
    }
    return exposed;
}

QStringList ActionRunner::sandboxArguments(const QString &shell,
                                           const QString &command) const
{
    Q_UNUSED(command)
    QStringList args{
        QStringLiteral("--die-with-parent"),
        QStringLiteral("--new-session"),
        QStringLiteral("--unshare-user"),
        QStringLiteral("--unshare-pid"),
        QStringLiteral("--unshare-ipc"),
        QStringLiteral("--unshare-uts"),
        QStringLiteral("--unshare-cgroup-try"),
    };
    if (!m_networkAllowed)
        args << QStringLiteral("--unshare-net");
    args << QStringLiteral("--uid") << QStringLiteral("65534")
         << QStringLiteral("--gid") << QStringLiteral("65534")
         << QStringLiteral("--cap-drop") << QStringLiteral("ALL")
         << QStringLiteral("--hostname") << QStringLiteral("forkmesh-action")
         << QStringLiteral("--proc") << QStringLiteral("/proc")
         << QStringLiteral("--dev") << QStringLiteral("/dev")
         << QStringLiteral("--dir") << QStringLiteral("/run")
         << QStringLiteral("--dir") << QStringLiteral("/var")
         << QStringLiteral("--tmpfs") << QStringLiteral("/var/tmp")
         << QStringLiteral("--dir") << QStringLiteral("/etc");

    const auto bindReadOnlyIfPresent = [&args](const QString &path) {
        if (QFileInfo::exists(path))
            args << QStringLiteral("--ro-bind") << path << path;
    };
    bindReadOnlyIfPresent(QStringLiteral("/usr"));
    bindReadOnlyIfPresent(QStringLiteral("/bin"));
    bindReadOnlyIfPresent(QStringLiteral("/lib"));
    bindReadOnlyIfPresent(QStringLiteral("/lib64"));
    // Debian resolves /usr/bin/{awk,cc,c++,editor,…} through the
    // /etc/alternatives symlink farm. Without it those names are dangling
    // symlinks inside the namespace and every step that runs one dies with
    // "command not found" (exit 127) — how the v0.7.10 release publish failed
    // after a full desktop build. The farm only ever points back into /usr,
    // which is already bound read-only here.
    bindReadOnlyIfPresent(QStringLiteral("/etc/alternatives"));
    bindReadOnlyIfPresent(QStringLiteral("/etc/passwd"));
    bindReadOnlyIfPresent(QStringLiteral("/etc/group"));
    bindReadOnlyIfPresent(QStringLiteral("/etc/nsswitch.conf"));
    bindReadOnlyIfPresent(QStringLiteral("/etc/ssl"));
    bindReadOnlyIfPresent(QStringLiteral("/etc/ca-certificates"));
    if (m_networkAllowed) {
        bindReadOnlyIfPresent(QStringLiteral("/etc/resolv.conf"));
        bindReadOnlyIfPresent(QStringLiteral("/etc/hosts"));
    }

    args << QStringLiteral("--ro-bind") << m_worktree
         << QStringLiteral("/source")
         << QStringLiteral("--bind") << m_workspace
         << QStringLiteral("/workspace")
         << QStringLiteral("--dir") << QStringLiteral("/home")
         << QStringLiteral("--bind") << m_sandboxHome
         << QStringLiteral("/home/forkmesh")
         << QStringLiteral("--bind") << m_sandboxTmp
         << QStringLiteral("/tmp")
         << QStringLiteral("--bind") << m_releaseStaging
         << QStringLiteral("/release-cas");

    // The only writable mount that is not created fresh for this run. Absent
    // when the node disabled it or another run of the same repository holds
    // it, and a workflow has to keep working either way.
    if (!m_buildCache.isEmpty())
        args << QStringLiteral("--bind") << m_buildCache
             << QStringLiteral("/cache");

    // The sandbox home is deliberately fresh for every run, so a regular
    // per-user install on the node is not visible here. Expose only ForkMesh's
    // dedicated, host-owned toolchain directory at the first entry on the
    // Action PATH. A workflow can execute these files but cannot replace them.
    const QString hostToolchainBin = m_store->toolchainBinDir();
    if (QFileInfo(hostToolchainBin).isDir())
        args << QStringLiteral("--ro-bind") << hostToolchainBin
             << QStringLiteral("/home/forkmesh/.local/bin");

    const QString hostUserLib =
        QDir::homePath() + QStringLiteral("/.local/lib");
    const QStringList pythonDirs =
        QDir(hostUserLib).entryList({QStringLiteral("python*")},
                                    QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::Name);
    for (const QString &python : pythonDirs) {
        const QString site = hostUserLib + QLatin1Char('/') + python +
                             QStringLiteral("/site-packages");
        if (!QFileInfo(site).isDir())
            continue;
        args << QStringLiteral("--ro-bind") << site
             << QStringLiteral("/home/forkmesh/.local/lib/%1/site-packages")
                    .arg(python);
    }

    args << QStringLiteral("--unsetenv")
         << QStringLiteral("XDG_RUNTIME_DIR")
         << QStringLiteral("--unsetenv")
         << QStringLiteral("DBUS_SESSION_BUS_ADDRESS")
         << QStringLiteral("--chdir") << QStringLiteral("/workspace")
         << shell << QStringLiteral("-se");
    return args;
}

void ActionRunner::launchSandboxedStep(const QString &command)
{
#if defined(Q_OS_LINUX)
    const QString bwrap =
        QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    const QString systemdRun =
        QStandardPaths::findExecutable(QStringLiteral("systemd-run"));
    const QString shell =
        QFile::exists(QStringLiteral("/bin/bash")) ? QStringLiteral("/bin/bash")
                                                   : QStringLiteral("/bin/sh");
    // Feed the expanded script on stdin. Secrets therefore never appear in the
    // process command line, crash process descriptions, or `ps` output.
    m_processStdin = command.toUtf8();
    if (!m_processStdin.endsWith('\n'))
        m_processStdin.append('\n');
    m_systemdUnit =
        QStringLiteral("forkmesh-action-%1-%2-%3.scope")
            .arg(QCoreApplication::applicationPid())
            .arg(m_run.id)
            .arg(m_stepIndex);
    forkmesh::processreaper::registerOwnedUnit(m_systemdUnit);
    const int runtimeSeconds =
        qMax(1, (currentStepTimeoutMs() + 999) / 1000);
    QStringList args{
        QStringLiteral("--user"),
        QStringLiteral("--scope"),
        QStringLiteral("--quiet"),
        QStringLiteral("--unit=") + m_systemdUnit,
        QStringLiteral("--property=TasksMax=%1")
            .arg(actionScopeTasksMax(m_limits)),
        QStringLiteral("--property=MemoryMax=%1")
            .arg(actionScopeMemoryBytes(m_limits)),
        QStringLiteral("--property=MemorySwapMax=0"),
        QStringLiteral("--property=CPUQuota=%1%").arg(actionCpuQuotaPercent()),
        QStringLiteral("--property=CPUWeight=20"),
        QStringLiteral("--property=RuntimeMaxSec=%1").arg(runtimeSeconds + 60),
    };
    if (forkmesh::agentresources::status().available)
        args << QStringLiteral("--slice=") + forkmesh::agentresources::sliceName();
    if (!m_workflow.sandbox) {
        args << QStringLiteral("--") << shell << QStringLiteral("-se");
        launch(Phase::Step, systemdRun, args, m_workspace);
        return;
    }
    args << QStringLiteral("--") << bwrap;
    args += sandboxArguments(shell, command);
    launch(Phase::Step, systemdRun, args, QString());
#else
    Q_UNUSED(command)
    complete(false, QStringLiteral(
                        "Actions isolation is unavailable on this platform."));
#endif
}

void ActionRunner::verifyWorkspaceSnapshotAsync()
{
    m_phase = Phase::SnapshotVerification;
    m_currentStepLabel = QStringLiteral("Repository approval verification");
    updateCrashContext();

    const int runId = m_run.id;
    const QString workspace = m_workspace;
    const QString expectedTree = m_run.repositoryTree;
    const QString expectedDigest = m_run.executionDigest;
    auto *watcher = new QFutureWatcher<WorkspaceSnapshotCheck>(this);
    connect(watcher, &QFutureWatcher<WorkspaceSnapshotCheck>::finished, this,
            [this, watcher, runId] {
                const WorkspaceSnapshotCheck check = watcher->result();
                watcher->deleteLater();
                if (!m_busy || m_run.id != runId ||
                    m_phase != Phase::SnapshotVerification)
                    return; // stopped or this pool runner has already been reused
                if (!check.ok) {
                    complete(false,
                             QStringLiteral(
                                 "Repository approval verification failed: %1.")
                                 .arg(check.reason));
                    return;
                }
                emitLog(QString::fromUtf8(
                            "==> \xE2\x9C\x85 Approval bound to tree %1 and state "
                            "SHA-256 %2.") // ✅
                            .arg(m_run.repositoryTree.left(12),
                                 m_run.executionDigest.left(16)));
                startSteps();
            });
    watcher->setFuture(QtConcurrent::run(
        [workspace, expectedTree, expectedDigest] {
            return verifyWorkspaceSnapshot(workspace, expectedTree,
                                           expectedDigest);
        }));
}

void ActionRunner::startSteps()
{
    if (!m_workflow.sandbox) {
        emitLog(QStringLiteral("==> Steps use this computer's own network, "
                               "files and processes."));
        runNextStep();
        return;
    }
    emitLog(m_networkAllowed
                ? QStringLiteral(
                      "==> Network access explicitly enabled by this "
                      "node's Actions policy.")
                : QStringLiteral(
                      "==> Network namespace isolated (egress denied)."));
    runNextStep();
}

void ActionRunner::checkWorkspaceQuotaAsync()
{
    m_phase = Phase::QuotaCheck;
    updateCrashContext();

    const int runId = m_run.id;
    const int stepIndex = m_stepIndex;
    const QString sandboxRoot = m_sandboxRoot;
    const qint64 maxWorkspaceBytes = m_limits.maxWorkspaceBytes;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, runId, stepIndex] {
                const QString quotaReason = watcher->result();
                watcher->deleteLater();
                if (!m_busy || m_completing || m_run.id != runId ||
                    m_phase != Phase::QuotaCheck || m_stepIndex != stepIndex)
                    return;
                m_phase = Phase::Step;
                if (!quotaReason.isEmpty()) {
                    setStepStatus(m_stepIndex, ActionStepStatus::Failed);
                    complete(false, quotaReason + QLatin1Char('.'));
                    return;
                }
                setStepStatus(m_stepIndex, ActionStepStatus::Success);
                ++m_stepIndex;
                runNextStep();
            });
    watcher->setFuture(QtConcurrent::run([sandboxRoot, maxWorkspaceBytes] {
        QString reason;
        if (workspaceWithinQuota(sandboxRoot, maxWorkspaceBytes, &reason))
            return QString();
        return reason;
    }));
}

bool ActionRunner::workspaceWithinQuota(const QString &sandboxRoot,
                                        qint64 maxWorkspaceBytes,
                                        QString *reason)
{
    qint64 bytes = 0;
    QDirIterator it(sandboxRoot,
                    QDir::Files | QDir::NoDotAndDotDot |
                        QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (!info.isFile())
            continue;
        const qint64 size = info.size();
        if (size < 0 ||
            bytes > maxWorkspaceBytes - size) {
            if (reason)
                *reason = QStringLiteral(
                    "the Actions writable workspace exceeded its disk quota");
            return false;
        }
        bytes += size;
    }
    return true;
}

bool ActionRunner::acquireBuildCacheLock(const QString &cache)
{
#ifndef Q_OS_WIN
    releaseBuildCache();
    const QByteArray path =
        QFile::encodeName(cache + QStringLiteral("/.lock"));
    const int fd =
        ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    // Non-blocking on purpose: a run that cannot have the cache builds cold
    // instead of waiting out another run's fifteen-minute compile. The lock
    // lives on the descriptor, so a node that crashes releases it.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    m_buildCacheLock = fd;
    return true;
#else
    Q_UNUSED(cache)
    return false;
#endif
}

void ActionRunner::releaseBuildCache()
{
    m_buildCache.clear();
#ifndef Q_OS_WIN
    if (m_buildCacheLock >= 0) {
        ::close(m_buildCacheLock); // closing the descriptor drops the flock
        m_buildCacheLock = -1;
    }
#endif
}

void ActionRunner::onProcessFinished(int exitCode, bool crashed)
{
    m_stepTimer->stop();
    m_terminationTimer->stop();
    m_systemdUnit.clear();
    const QPointer<QProcess> finishedProcess(m_process);
    m_process = nullptr;
    QByteArray tail;
    if (finishedProcess) {
        QObject::disconnect(finishedProcess.data(), nullptr, this, nullptr);
        tail = finishedProcess->readAllStandardOutput();
        finishedProcess->deleteLater();
    }
    if (!tail.isEmpty())
        emitProcessOutput(tail);
    emitSuppressedProcessOutputTail();

    if (m_stopping) {
        complete(false, QStringLiteral("Stopped by user."));
        return;
    }
    if (m_processTimedOut) {
        complete(false,
                 m_jobTimedOut
                     ? QStringLiteral("Workflow exceeded the total time limit.")
                     : QStringLiteral("%1 exceeded the process time limit.")
                           .arg(m_currentStepLabel));
        return;
    }
    if ((crashed || exitCode != 0) && forkmesh::terminationSignalReceived()) {
        interruptForShutdown(crashed, exitCode);
        return;
    }

    if (m_phase == Phase::Checkout) {
        if (exitCode != 0 && retryCheckoutAfterWorktreeRace())
            return;
        if (exitCode != 0) {
            complete(false, QStringLiteral("Checkout failed."));
            return;
        }
        m_currentStepLabel = QStringLiteral("Isolated workspace");
        emitLog(QString::fromUtf8(
            "==> \xF0\x9F\x94\x92 Creating a disposable writable clone; "
            "the approved source stays read-only."));
        // Clone the exact detached worktree we just approved, not the mutable
        // bare mirror a synchronization can update between these two phases.
        // `git clone` only transfers objects reachable from the source's
        // advertised refs/HEAD. The detached worktree advertises this run's
        // commit as HEAD even after its PR ref moves, while cloning m_mirror
        // could succeed yet omit that commit's tree and make the following
        // checkout fail with "fatal: unable to read tree".
        QStringList cloneArgs{
            QStringLiteral("clone"), QStringLiteral("--no-checkout"),
            QStringLiteral("--no-hardlinks"), QStringLiteral("--no-local"),
            QStringLiteral("--upload-pack=git -c pack.window=0 -c pack.threads=2 upload-pack"),
            QStringLiteral("--single-branch")};
        if (m_workflow.fetchDepth > 0)
            cloneArgs << QStringLiteral("--depth=%1").arg(m_workflow.fetchDepth);
        cloneArgs << m_worktree << m_workspace;
        launch(Phase::WorkspaceClone, QStringLiteral("git"), cloneArgs, QString());
        return;
    }
    if (m_phase == Phase::WorkspaceClone) {
        if (exitCode != 0) {
            complete(false, QStringLiteral(
                                "Could not create the isolated workspace."));
            return;
        }
        launch(Phase::WorkspaceCheckout, QStringLiteral("git"),
               {QStringLiteral("-C"), m_workspace,
                QStringLiteral("checkout"), QStringLiteral("--detach"),
                m_run.commit},
               QString());
        return;
    }
    if (m_phase == Phase::WorkspaceCheckout) {
        if (exitCode != 0) {
            complete(false, QStringLiteral(
                                "Could not check out the approved commit."));
            return;
        }
        verifyWorkspaceSnapshotAsync();
        return;
    }

    if (crashed) {
        complete(false,
                 QStringLiteral(
                     "%1 was killed by signal %2 rather than exiting. The "
                     "sandbox stops a step this way when it exceeds its %3 MiB "
                     "memory budget (FORKMESH_ACTIONS_MEMORY_MB) — build with "
                     "fewer parallel jobs — or when something outside the run "
                     "terminates its scope.")
                     .arg(m_currentStepLabel)
                     .arg(exitCode)
                     .arg(actionScopeMemoryBytes(m_limits) / (1024 * 1024)));
        return;
    }
    if (exitCode != 0) {
        setStepStatus(m_stepIndex, ActionStepStatus::Failed);
        complete(false, QStringLiteral("Step failed with exit code %1.")
                            .arg(exitCode));
        return;
    }
    checkWorkspaceQuotaAsync();
}

void ActionRunner::runNextStep()
{
    if (m_stepIndex >= m_workflow.steps.size()) {
        complete(true, QStringLiteral("All steps completed."));
        return;
    }

    const ActionStep &step = m_workflow.steps.at(m_stepIndex);
    const QString command =
        ActionFile::substitute(step.run, m_exposedVariables);
    const QString label =
        step.name.isEmpty() ? QStringLiteral("Step %1").arg(m_stepIndex + 1)
                            : step.name;
    m_currentStepLabel = label;
    m_currentCommand = command;
    m_run.currentStepIndex = m_stepIndex;
    setStepStatus(m_stepIndex, ActionStepStatus::Running);
    updateCrashContext();
    emitLog(QString());
    emitLog(QString::fromUtf8("==> \xF0\x9F\x94\xA7 %1").arg(label)); // 🔧

    launchSandboxedStep(command);
}

void ActionRunner::complete(bool ok, const QString &finalMessage)
{
    if (m_completing)
        return; // the landing below is still in flight for this run
    m_completing = true;
    m_terminationTimer->stop();
    m_stepTimer->stop(); // nothing may re-enter complete() while artifacts land
    m_jobTimer->stop();

    // Landing artifacts walks and copies whole trees — the release output and
    // whatever the job put in forkmesh-artifacts — and 1.5 s stalls were sampled
    // inside copySandboxTree() from here. Copy on a worker, then finish the run
    // on the GUI thread with exactly the verdict the copies produced.
    if (!ok) {
        finishComplete(false, finalMessage, false);
        return;
    }
    const int runId = m_run.id;
    const bool releaseRun = isReleaseRun();
    const QString releaseStaging = m_releaseStaging;
    const QString releaseDestination =
        QDir(m_mirror).absoluteFilePath(QStringLiteral("forkmesh-releases"));
    const QString artifactStaging =
        QDir(m_sandboxHome)
            .absoluteFilePath(QStringLiteral("forkmesh-artifacts"));
    const QString artifactDestination =
        QDir(m_store->artifactsDir())
            .absoluteFilePath(m_run.repoKey() + QLatin1Char('/') +
                              QString::number(m_run.id));
    auto *watcher = new QFutureWatcher<ArtifactLanding>(this);
    connect(watcher, &QFutureWatcher<ArtifactLanding>::finished, this,
            [this, watcher, runId, finalMessage, artifactDestination] {
                const ArtifactLanding landing = watcher->result();
                watcher->deleteLater();
                if (m_run.id != runId)
                    return; // this pool runner has already been reused
                // Emit exactly what the synchronous landers used to log, in the
                // same order, then apply the same verdict.
                if (!landing.releaseOk) {
                    if (!landing.releaseError.isEmpty())
                        emitLog(QStringLiteral("!! %1").arg(landing.releaseError));
                    finishComplete(
                        false,
                        QStringLiteral("Release artifacts failed validation or "
                                       "could not be landed."),
                        false);
                    return;
                }
                const bool landed =
                    landing.releaseLanded ? landReleaseMetadata() : false;
                if (landed)
                    emit releaseMetadataPrepared(m_run.id);
                if (landing.artifactsAttempted) {
                    if (landing.artifactsOk)
                        emitLog(QStringLiteral("==> Artifacts landed at %1")
                                    .arg(artifactDestination));
                    else
                        emitLog(
                            QStringLiteral("!! %1").arg(landing.artifactsError));
                }
                if (!landing.artifactsOk) {
                    finishComplete(
                        false,
                        QStringLiteral("Action artifacts failed validation or "
                                       "could not be landed."),
                        landed);
                    return;
                }
                finishComplete(true, finalMessage, landed);
            });
    watcher->setFuture(QtConcurrent::run([releaseRun, releaseStaging,
                                          releaseDestination, artifactStaging,
                                          artifactDestination] {
        ArtifactLanding landing;
        if (releaseRun) {
            // landReleaseArtifacts() treated "nothing staged" as a failed
            // release too, and that verdict is what the caller reports.
            const QDir staged(releaseStaging);
            if (!staged.exists() ||
                staged.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                    .isEmpty()) {
                landing.releaseOk = false;
                return landing;
            }
            landing.releaseOk =
                copySandboxTree(releaseStaging, releaseDestination,
                                kMaxLandedArtifactBytes, &landing.releaseError);
            if (!landing.releaseOk)
                return landing;
            landing.releaseLanded = true;
        }
        if (QDir(artifactStaging).exists()) {
            landing.artifactsAttempted = true;
            landing.artifactsOk = copySandboxTree(
                artifactStaging, artifactDestination, kMaxLandedArtifactBytes,
                &landing.artifactsError);
        }
        return landing;
    }));
}

void ActionRunner::finishComplete(bool ok, const QString &finalMessage,
                                  bool landed)
{
    const QString resultMessage = finalMessage;
    if (!ok && !m_stopping)
        logFailureDiagnostic(resultMessage);
    emitLog(QString());
    emitLog((m_stopping ? QString::fromUtf8("==> \xF0\x9F\x9B\x91 STOPPED: ") // 🛑
                        : ok ? QString::fromUtf8("==> \xE2\x9C\x85 SUCCESS: ") // ✅
                             : QString::fromUtf8("==> \xE2\x9D\x8C FAILED: ")) + // ❌
            resultMessage);

    cleanupWorktree();

    if (m_stepIndex >= 0 && m_stepIndex < m_run.stepStatuses.size() &&
        m_run.stepStatuses.at(m_stepIndex) == ActionStepStatus::Running) {
        setStepStatus(m_stepIndex,
                      m_stopping ? ActionStepStatus::Cancelled
                                 : ActionStepStatus::Failed);
    }
    m_run.status = m_stopping ? ActionStatus::Cancelled
                              : ok ? ActionStatus::Success
                                   : ActionStatus::Failed;
    if (ok) {
        for (int index = 0; index < m_run.stepStatuses.size(); ++index)
            m_run.stepStatuses[index] = ActionStepStatus::Success;
        m_run.currentStepIndex = -1;
    } else if (m_stopping) {
        for (int index = 0; index < m_run.stepStatuses.size(); ++index) {
            if (m_run.stepStatuses.at(index) == ActionStepStatus::Pending ||
                m_run.stepStatuses.at(index) == ActionStepStatus::Queued)
                m_run.stepStatuses[index] = ActionStepStatus::Skipped;
        }
    }
    m_run.finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_store->saveRun(m_run);
    m_phase = Phase::Idle;
    m_busy = false;
    m_completing = false;
    m_stepTimer->stop();
    m_jobTimer->stop();
    m_crashContextTimer->stop();
    forkmesh::setCrashContext(QString());
    forkmesh::setTerminationSignalSurvivalEnabled(false);
    emit statusChanged(m_run.id, m_run.status);
    emit finished(m_run.id, ok);
    if (landed)
        emit releaseMetadataLanded(m_run.id);
}

void ActionRunner::interruptForShutdown(bool crashed, int exitCode)
{
    m_terminationTimer->stop();
    m_stepTimer->stop();
    m_jobTimer->stop();
    emitLog(QString());
    emitLog(QString::fromUtf8(
                "==> \xE2\x9A\xA0 INTERRUPTED: ForkMesh received a stop signal "
                "and %1 %2. The run stays active and restarts from its approved "
                "snapshot when ForkMesh starts again.")
                .arg(m_currentStepLabel,
                     crashed ? QStringLiteral("was killed by signal %1")
                                   .arg(exitCode)
                             : QStringLiteral("exited with code %1")
                                   .arg(exitCode)));
    cleanupWorktree();
    m_run.status = ActionStatus::Running;
    m_run.finishedAtMs = 0;
    m_store->saveRun(m_run);
    m_phase = Phase::Idle;
    m_busy = false;
    m_completing = false;
    m_crashContextTimer->stop();
    forkmesh::setCrashContext(QString());
    forkmesh::setTerminationSignalSurvivalEnabled(false);
    emit interrupted(m_run.id);
}

bool ActionRunner::isReleaseRun() const
{
    // A platform release may be re-run from the branch after another native
    // runner has committed its assets for the same immutable tag. Treat the
    // dedicated release workflow as a release even under workflow_dispatch so
    // those additive metadata/CAS results are landed instead of discarded.
    return m_run.ref.startsWith(QLatin1String("refs/tags/")) ||
           m_run.workflowPath == QLatin1String(".forkmesh/release.yml");
}

bool ActionRunner::landReleaseMetadata()
{
    if (m_repoWorkTree.isEmpty() || !QDir(m_repoWorkTree).exists())
        return false;
    const QDir produced(m_workspace + QStringLiteral("/.forkmesh/releases"));
    if (!produced.exists())
        return false;

    const QDir dest(m_repoWorkTree + QStringLiteral("/.forkmesh/releases"));
    const QString destRoot = dest.absolutePath();
    QDir().mkpath(destRoot);
    const QFileInfoList entries = produced.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    bool copiedAny = false;
    std::function<void(const QString &, const QString &)> copyTree =
        [&](const QString &srcDir, const QString &dstDir) {
            QDir().mkpath(dstDir);
            const QFileInfoList items = QDir(srcDir).entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo &fi : items) {
                const QString to = dstDir + QLatin1Char('/') + fi.fileName();
                if (fi.isDir()) {
                    copyTree(fi.absoluteFilePath(), to);
                } else {
                    QFile::remove(to); // overwrite an older manifest
                    if (QFile::copy(fi.absoluteFilePath(), to))
                        copiedAny = true;
                }
            }
        };
    for (const QFileInfo &fi : entries) {
        const QString to = destRoot + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) {
            copyTree(fi.absoluteFilePath(), to);
        } else {
            QFile::remove(to);
            if (QFile::copy(fi.absoluteFilePath(), to))
                copiedAny = true;
        }
    }
    if (!copiedAny)
        return false;

    QStringList paths{QStringLiteral(".forkmesh/releases")};
    if (landVersionHeader())
        paths << QStringLiteral("desktop/CMakeLists.txt");

    auto git = [&](const QStringList &args) {
        QProcess p;
        p.setWorkingDirectory(m_repoWorkTree);
        p.start(QStringLiteral("git"), args);
        p.waitForFinished(30000);
        return p.exitCode();
    };
    git(QStringList{QStringLiteral("add"), QStringLiteral("--")} + paths);
    if (git(QStringList{QStringLiteral("diff"), QStringLiteral("--cached"),
                        QStringLiteral("--quiet"), QStringLiteral("--")} +
            paths) == 0) {
        emitLog(QString::fromUtf8(
            "==> \xE2\x84\xB9\xEF\xB8\x8F  Release metadata unchanged; continuing "
            "mirror publication.")); // ℹ️
        // A rerun can be repairing missing CAS blobs rather than changing the
        // signed manifest. The artifacts were copied successfully above, so
        // report the release as landed and let the durable publication/rollout
        // path retry instead of waiting for an application restart.
        return true;
    }
    const QString tag = m_run.ref.mid(QStringLiteral("refs/tags/").size());
    const QString message =
        paths.contains(QStringLiteral("desktop/CMakeLists.txt"))
            ? QStringLiteral("release: publish %1 artifacts, bump version header")
                  .arg(tag)
            : QStringLiteral("release: publish %1 artifacts").arg(tag);
    const int rc = git(QStringList{QStringLiteral("-c"),
                                   QStringLiteral("user.email=actions@forkmesh.local"),
                                   QStringLiteral("-c"),
                                   QStringLiteral("user.name=ForkMesh Actions"),
                                   QStringLiteral("commit"), QStringLiteral("-m"),
                                   message, QStringLiteral("--")} +
                       paths);
    if (rc != 0) {
        emitLog(QString::fromUtf8(
            "!! Could not commit release metadata into the working copy."));
        return false;
    }
    emitLog(QString::fromUtf8(
                "==> \xF0\x9F\x93\xA6 Attached release metadata for %1; "
                "publishing\xE2\x80\xA6") // 📦 …
                .arg(tag));
    return true;
}

bool ActionRunner::landVersionHeader()
{
    if (m_repoWorkTree.isEmpty() || m_worktree.isEmpty())
        return false;
    const QString rel = QStringLiteral("desktop/CMakeLists.txt");
    QFile src(m_workspace + QLatin1Char('/') + rel);
    QFile dst(m_repoWorkTree + QLatin1Char('/') + rel);

    static const QRegularExpression versionLine(
        QStringLiteral("^(project\\(ForkMesh VERSION )([0-9]+\\.[0-9]+\\.[0-9]+)"),
        QRegularExpression::MultilineOption);

    if (!src.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QRegularExpressionMatch want =
        versionLine.match(QString::fromUtf8(src.readAll()));
    src.close();
    if (!want.hasMatch())
        return false; // workflow left the header alone (e.g. no clean semver tag)
    const QString version = want.captured(2);

    if (!dst.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString text = QString::fromUtf8(dst.readAll());
    dst.close();
    const QRegularExpressionMatch have = versionLine.match(text);
    if (!have.hasMatch() || have.captured(2) == version)
        return false; // header missing here or already at the release version
    text.replace(have.capturedStart(2), have.capturedLength(2), version);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const bool wrote = dst.write(text.toUtf8()) >= 0;
    dst.close();
    return wrote;
}

bool ActionRunner::copySandboxTree(const QString &source,
                                   const QString &destination,
                                   qint64 maxBytes, QString *error)
{
    if (error)
        error->clear();
    const QFileInfo sourceInfo(source);
    if (!sourceInfo.exists())
        return true;
    if (!sourceInfo.isDir() || sourceInfo.isSymLink()) {
        if (error)
            *error = QStringLiteral("artifact source is not a real directory");
        return false;
    }
    const QFileInfo destinationInfo(destination);
    if (destinationInfo.exists() &&
        (!destinationInfo.isDir() || destinationInfo.isSymLink())) {
        if (error)
            *error = QStringLiteral("artifact destination is unsafe");
        return false;
    }
    if (!QDir().mkpath(destination)) {
        if (error)
            *error = QStringLiteral("artifact destination could not be created");
        return false;
    }

    qint64 total = 0;
    QDir sourceDir(source);
    QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot |
                                QDir::System | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isSymLink()) {
            if (error)
                *error = QStringLiteral("artifact output contains a symlink");
            return false;
        }
        const QString relative = sourceDir.relativeFilePath(path);
        if (relative.isEmpty() || relative == QLatin1String(".") ||
            relative == QLatin1String("..") ||
            relative.startsWith(QLatin1String("../")) ||
            QDir::isAbsolutePath(relative)) {
            if (error)
                *error = QStringLiteral("artifact output escaped its directory");
            return false;
        }
        const QString target = QDir(destination).filePath(relative);
        if (info.isDir()) {
            const QFileInfo targetInfo(target);
            if ((targetInfo.exists() && targetInfo.isSymLink()) ||
                !QDir().mkpath(target)) {
                if (error)
                    *error = QStringLiteral("artifact directory is unsafe");
                return false;
            }
            continue;
        }
        if (!info.isFile()) {
            if (error)
                *error =
                    QStringLiteral("artifact output contains a special file");
            return false;
        }
        if (info.size() < 0 || info.size() > maxBytes - total) {
            if (error)
                *error = QStringLiteral("artifact output exceeds its size limit");
            return false;
        }
        total += info.size();
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("artifact output could not be read");
            return false;
        }
        const QFileInfo targetInfo(target);
        if (targetInfo.exists() &&
            (!targetInfo.isFile() || targetInfo.isSymLink())) {
            if (error)
                *error = QStringLiteral("artifact target is unsafe");
            return false;
        }
        QSaveFile output(target);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) {
            if (error)
                *error = QStringLiteral("artifact target could not be written");
            return false;
        }
        while (!input.atEnd()) {
            const QByteArray chunk = input.read(256 * 1024);
            if (chunk.isEmpty() && input.error() != QFileDevice::NoError) {
                output.cancelWriting();
                if (error)
                    *error = QStringLiteral("artifact read failed");
                return false;
            }
            if (output.write(chunk) != chunk.size()) {
                output.cancelWriting();
                if (error)
                    *error = QStringLiteral("artifact write failed");
                return false;
            }
        }
        output.setPermissions(QFileDevice::ReadOwner |
                              QFileDevice::WriteOwner);
        if (!output.commit()) {
            if (error)
                *error = QStringLiteral("artifact commit failed");
            return false;
        }
    }
    return true;
}

void ActionRunner::cleanupWorktree()
{
    releaseSandboxClaim(m_sandboxRoot);
    if (m_sandboxRoot.isEmpty()) {
        releaseBuildCache();
        return;
    }
    // Checking the build cache against its ceiling walks every file in it, so
    // it goes to the same worker that discards the sandbox below. Drop this
    // run's claim on the cache first and let that walk retake it: deleting a
    // finished sandbox takes seconds, and the next run of this repository
    // would otherwise start — and compile — cold for all of them.
    const QString cache = m_buildCache;
    const qint64 cacheCeiling = m_limits.maxCacheBytes;
    releaseBuildCache();
    // A finished run leaves a whole checkout plus whatever the job built behind.
    // Pruning it meant a blocking `git worktree remove` and two recursive
    // deletes on the GUI thread — a 561 ms stall was sampled inside
    // the recursive delete here. Rename the tree out of the run's own
    // namespace first, so it is gone for every reader the instant this returns,
    // and delete the renamed copy on a worker. `worktree prune` rather than
    // `remove`, because the checkout is no longer where git recorded it.
    const QString discarded =
        QDir(QFileInfo(m_sandboxRoot).absolutePath())
            .filePath(kSandboxDiscardPrefix + QString::number(m_run.id) +
                      QLatin1Char('-') +
                      QString::number(QDateTime::currentMSecsSinceEpoch()));
    const bool moved = QDir().rename(m_sandboxRoot, discarded);
    (void)QtConcurrent::run([mirror = m_mirror,
                             tree = moved ? discarded : m_sandboxRoot,
                             cache, cacheCeiling] {
        QProcess::execute(QStringLiteral("git"),
                          {QStringLiteral("-C"), mirror,
                           QStringLiteral("worktree"), QStringLiteral("prune")});
        forkmesh::safedelete::removeTree(tree, QFileInfo(tree).absolutePath(), nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
        enforceCacheBudget(cache, cacheCeiling);
    });
    m_sandboxRoot.clear();
    m_worktree.clear();
    m_workspace.clear();
    m_sandboxHome.clear();
    m_sandboxTmp.clear();
    m_releaseStaging.clear();
}

void ActionRunner::emitLog(const QString &text)
{
    const QString safe = redact(text);
    m_store->appendLog(m_run, safe);
    emit logLine(m_run.id, safe);
}

void ActionRunner::emitProcessOutput(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;

    rememberCrashProcessOutput(bytes);
    if (!m_crashContextTimer->isActive())
        m_crashContextTimer->start();

    if (m_processOutputBytes >= kActionProcessLogMaxBytes) {
        emitProcessOutputTruncationNotice();
        rememberSuppressedProcessOutput(bytes);
        return;
    }

    const qsizetype remainingBytes =
        kActionProcessLogMaxBytes - m_processOutputBytes;
    const qsizetype keepBytes = qMin(bytes.size(), remainingBytes);
    for (qsizetype offset = 0; offset < keepBytes;
         offset += kActionProcessLogChunkBytes) {
        const qsizetype chunkBytes =
            qMin(kActionProcessLogChunkBytes, keepBytes - offset);
        const QString safe =
            normalizeProcessOutputForLog(QString::fromUtf8(bytes.constData() + offset,
                                                           chunkBytes));
        if (!safe.isEmpty())
            emitLog(safe);
    }

    m_processOutputBytes += keepBytes;
    if (keepBytes < bytes.size()) {
        emitProcessOutputTruncationNotice();
        rememberSuppressedProcessOutput(bytes.sliced(keepBytes));
    }
}

void ActionRunner::emitProcessOutputTruncationNotice()
{
    if (m_processOutputTruncated)
        return;
    m_processOutputTruncated = true;
    emitLog(QStringLiteral(
                "\n!! Action output exceeded %1 KiB. Suppressing middle output "
                "to keep the app responsive; the last %2 KiB will be shown "
                "when this step exits.\n")
                .arg(kActionProcessLogMaxBytes / 1024)
                .arg(kActionProcessLogTailBytes / 1024));
}

void ActionRunner::rememberSuppressedProcessOutput(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;

    m_processOutputSuppressedBytes += bytes.size();
    m_processOutputTail.append(bytes);
    if (m_processOutputTail.size() > kActionProcessLogTailBytes)
        m_processOutputTail =
            m_processOutputTail.right(kActionProcessLogTailBytes);
}

void ActionRunner::emitSuppressedProcessOutputTail()
{
    if (!m_processOutputTruncated || m_processOutputTail.isEmpty())
        return;

    emitLog(QStringLiteral(
                "\n!! Showing the final %1 KiB of suppressed process output "
                "(%2 KiB omitted).\n")
                .arg((m_processOutputTail.size() + 1023) / 1024)
                .arg((m_processOutputSuppressedBytes + 1023) / 1024));
    m_processOutputLineChars = 0;
    for (qsizetype offset = 0; offset < m_processOutputTail.size();
         offset += kActionProcessLogChunkBytes) {
        const qsizetype chunkBytes =
            qMin(kActionProcessLogChunkBytes,
                 m_processOutputTail.size() - offset);
        const QString safe =
            normalizeProcessOutputForLog(QString::fromUtf8(
                m_processOutputTail.constData() + offset, chunkBytes));
        if (!safe.isEmpty())
            emitLog(safe);
    }
    emitLog(QStringLiteral("\n!! End of suppressed process output.\n"));
    m_processOutputTail.clear();
}

QString ActionRunner::normalizeProcessOutputForLog(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar ch : text) {
        const ushort u = ch.unicode();
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            out += QLatin1Char('\n');
            m_processOutputLineChars = 0;
            continue;
        }
        if (ch == QLatin1Char('\t')) {
            out += ch;
            m_processOutputLineChars += 4;
        } else if (u < 0x20 || (u >= 0x7f && u <= 0x9f)) {
            continue;
        } else {
            out += ch;
            ++m_processOutputLineChars;
        }
        if (m_processOutputLineChars >= kActionProcessLogMaxLineChars) {
            out += QStringLiteral("\n...[long line wrapped for display]...\n");
            m_processOutputLineChars = 0;
        }
    }
    return out;
}

void ActionRunner::rememberCrashProcessOutput(const QByteArray &bytes)
{
    m_crashOutputTail.append(bytes);
    if (m_crashOutputTail.size() > kActionCrashOutputTailBytes)
        m_crashOutputTail = m_crashOutputTail.right(kActionCrashOutputTailBytes);
}

int ActionRunner::currentStepTimeoutMs() const
{
    int timeoutMs = m_limits.stepTimeoutMs;
    if (m_stepIndex >= 0 && m_stepIndex < m_workflow.steps.size()) {
        const int minutes = m_workflow.steps.at(m_stepIndex).timeoutMinutes;
        if (minutes > 0)
            timeoutMs = qMin(minutes * 60 * 1000, m_limits.jobTimeoutMs);
    }
    return qMax(250, timeoutMs);
}

QString ActionRunner::phaseName() const
{
    switch (m_phase) {
    case Phase::Idle: return QStringLiteral("idle");
    case Phase::Checkout: return QStringLiteral("checkout");
    case Phase::WorkspaceClone: return QStringLiteral("workspace clone");
    case Phase::WorkspaceCheckout: return QStringLiteral("workspace checkout");
    case Phase::SnapshotVerification:
        return QStringLiteral("repository approval verification");
    case Phase::Step: return QStringLiteral("step");
    case Phase::QuotaCheck: return QStringLiteral("workspace quota check");
    }
    return QStringLiteral("invalid phase");
}

QString ActionRunner::crashContextHeader() const
{
    return QStringLiteral("action run id: %1\nworkflow: %2 (%3)\nrepo: %4/%5\n"
                          "commit: %6\nref: %7\nphase: %8\nstep: %9\ncommand: %10\n"
                          "worktree: %11\n")
        .arg(m_run.id)
        .arg(oneLine(m_workflow.name), oneLine(m_workflow.path),
             oneLine(m_run.owner), oneLine(m_run.name), oneLine(m_run.commit),
             oneLine(m_run.ref), phaseName(), oneLine(m_currentStepLabel),
             oneLine(redact(m_currentCommand)), oneLine(m_worktree));
}

QString ActionRunner::crashOutputTail() const
{
    const QString tail = diagnosticSafeText(
        redact(QString::fromUtf8(m_crashOutputTail)), 8000);
    return tail.trimmed().isEmpty() ? QString() : tail + QLatin1Char('\n');
}

QString ActionRunner::crashContext() const
{
    QString out = crashContextHeader();
    const QString tail = crashOutputTail();
    if (!tail.isEmpty())
        out += QStringLiteral("recent process output tail:\n") + tail;
    if (out.size() > kActionCrashContextMaxChars)
        out = QStringLiteral("...(crash context truncated; showing tail)...\n") +
              out.right(kActionCrashContextMaxChars);
    return out;
}

#ifdef FORKMESH_ACTION_STORE_TESTS
bool ActionRunner::crashContextRefreshPendingForTesting() const
{
    return m_crashContextTimer->isActive();
}
#endif

void ActionRunner::updateCrashContext() const
{
    m_crashContextTimer->stop();
    ++m_crashContextUpdates;
    const QString tail = crashOutputTail();
    QString head = crashContextHeader();
    if (!tail.isEmpty())
        head += QStringLiteral("recent process output tail:\n");
    forkmesh::setCrashContext(forkmesh::fitCrashContext(head, tail));
}

void ActionRunner::logFailureDiagnostic(const QString &finalMessage) const
{
    forkmesh::logDiagnosticEvent(
        QStringLiteral("Action failed before UI refresh"),
        crashContext() + QStringLiteral("final message: ") + finalMessage);
}

QString ActionRunner::redact(QString text) const
{
    for (const QString &secret : m_secrets)
        if (!secret.isEmpty())
            text.replace(secret, QStringLiteral("***"));
    return text;
}

#pragma once

#include <QByteArray>
#include <QMap>
#include <QSet>
#include <QObject>
#include <QtGlobal>
#include <QString>
#include <QStringList>

#include "ActionFile.h"
#include "ActionStore.h"

class QProcess;
class QTimer;

struct ActionSandboxLimits {
    qint64 maxMemoryBytes = 4LL * 1024 * 1024 * 1024;
    qint64 maxScopeMemoryBytes = 0;
    qint64 maxFileBytes = 1024LL * 1024 * 1024;
    qint64 maxWorkspaceBytes = 2LL * 1024 * 1024 * 1024;
    // The persistent per-repository build cache, which lives outside the
    // workspace and therefore outside maxWorkspaceBytes. ccache trims itself
    // to half of this; the rest is headroom for a cached CMake/Ninja tree.
    qint64 maxCacheBytes = 8LL * 1024 * 1024 * 1024;
    int maxProcesses = 0;
    // Per-process descriptor ceiling. JVM build tools keep every classpath jar
    // open: the Gradle 9.3.1 daemon loads ~308 distribution jars at startup and
    // held ~1,000 descriptors mid-way through an APK release build, so
    // the old 256 made every Android build die with "Unable to create daemon
    // log file (Too many open files)" before compiling anything. Descriptors are
    // kernel memory already charged to the step's scope, so a generous cap
    // costs no containment; it is clamped to the node's own hard limit.
    int maxOpenFiles = 65536;
    int maxCpuSeconds = 20 * 60;
    int stepTimeoutMs = 90 * 60 * 1000;
    int jobTimeoutMs = 4 * 60 * 60 * 1000;
};

// What a sandbox sweep reclaimed, and how much room the node was left with.
struct ActionSandboxSweep {
    int removed = 0;       // disposable trees deleted
    qint64 freedBytes = 0; // measured from the filesystem, so 0 under contention
    qint64 freeBytes = -1; // available on the sandbox filesystem, -1 = unknown
};

qint64 actionScopeMemoryBytes(const ActionSandboxLimits &limits);

qint64 actionAddressSpaceBytes(const ActionSandboxLimits &limits);

int actionScopeTasksMax(const ActionSandboxLimits &limits);

int actionCpuQuotaPercent();

class ActionRunner : public QObject
{
    Q_OBJECT
public:
    explicit ActionRunner(ActionStore *store, QObject *parent = nullptr);
    ~ActionRunner() override;

    bool busy() const { return m_busy; }
    int currentRunId() const { return m_busy ? m_run.id : -1; }

    void start(const ActionRun &run, const ActionWorkflow &workflow,
               const QString &mirrorPath, const QString &workTreePath,
               const QMap<QString, QString> &variables);

    void stop();

    void setSandboxLimitsForTesting(const ActionSandboxLimits &limits);

    // Cached for a minute; probeSandboxAvailability() is the uncached probe.
    static bool sandboxAvailable(QString *reason = nullptr);

    // Deletes every disposable tree under `base` that no live run claims, and
    // reports the space left afterwards. Blocking: callers run it on a worker.
    // `graceMs` < 0 keeps the default window that spares brand new trees; tests
    // pass 0 to sweep one they have just abandoned.
    static ActionSandboxSweep reclaimSandboxes(const QString &base,
                                               qint64 graceMs = -1);
    static QSet<QString> claimedSandboxNames();
    static QString removeUnclaimedSandbox(const QString &base, const QString &path);
    static bool checkoutLostWorktreeRace(const QString &output);

signals:
    void logLine(int runId, const QString &text);
    void statusChanged(int runId, const QString &status);
    void stepChanged(int runId, int stepIndex, const QString &status);
    void finished(int runId, bool ok);
    void interrupted(int runId);
    void releaseMetadataPrepared(int runId);
    void releaseMetadataLanded(int runId);

private:
    enum class Phase {
        Idle,
        Checkout,
        WorkspaceClone,
        WorkspaceCheckout,
        SnapshotVerification,
        Step,
        QuotaCheck
    };

    static bool probeSandboxAvailability(QString *reason);

    // Reclaims abandoned sandboxes off-thread, then either starts the checkout
    // or fails the run because the node has no room to check anything out.
    void reclaimSandboxSpaceThenCheckout();
    void launchCheckout();
    bool retryCheckoutAfterWorktreeRace();
    qint64 requiredFreeBytes() const;
    void launch(Phase phase, const QString &program, const QStringList &args,
                const QString &workingDir);
    void launchSandboxedStep(const QString &command);
    QStringList sandboxArguments(const QString &shell,
                                 const QString &command) const;
    QMap<QString, QString> explicitWorkflowVariables() const;
    void verifyWorkspaceSnapshotAsync();
    void startSteps();
    void checkWorkspaceQuotaAsync();
    static bool workspaceWithinQuota(const QString &sandboxRoot,
                                     qint64 maxWorkspaceBytes,
                                     QString *reason = nullptr);
    // Exactly one run at a time may own a repository's persistent cache, so a
    // second concurrent run cannot write into a build tree it is sharing.
    bool acquireBuildCacheLock(const QString &cache);
    void releaseBuildCache();
    void terminateCurrentProcess(bool timedOut);
    void onProcessFinished(int exitCode, bool crashed);
    void runNextStep();
    void setStepStatus(int stepIndex, const QString &status);
    void emitLog(const QString &text);
    void emitProcessOutput(const QByteArray &bytes);
    void emitProcessOutputTruncationNotice();
    void rememberSuppressedProcessOutput(const QByteArray &bytes);
    void emitSuppressedProcessOutputTail();
    QString normalizeProcessOutputForLog(const QString &text);
    void rememberCrashProcessOutput(const QByteArray &bytes);
    QString phaseName() const;
    QString crashContextHeader() const;
    QString crashOutputTail() const;
    QString crashContext() const;
    void updateCrashContext() const;
    void logFailureDiagnostic(const QString &finalMessage) const;
    // What the artifact copies decided, handed back from the worker.
    struct ArtifactLanding {
        bool releaseOk = true;      // false also means "a release staged nothing"
        bool releaseLanded = false; // a release tree really was copied
        QString releaseError;
        bool artifactsAttempted = false;
        bool artifactsOk = true;
        QString artifactsError;
    };

    // complete() starts the artifact landing off-thread; finishComplete() is the
    // GUI-thread half that records the verdict and ends the run.
    void complete(bool ok, const QString &finalMessage);
    void interruptForShutdown(bool crashed, int exitCode);
    void finishComplete(bool ok, const QString &finalMessage, bool landed);
    void cleanupWorktree();
    bool landReleaseMetadata();
    bool landVersionHeader();
    static bool copySandboxTree(const QString &source,
                                const QString &destination, qint64 maxBytes,
                                QString *error);
    bool isReleaseRun() const;
    QString redact(QString text) const;

    ActionStore *m_store;
    bool m_busy = false;
    bool m_stopping = false; // a stop() was requested; complete() records Cancelled
    bool m_completing = false; // artifacts are landing; complete() may not re-enter
    Phase m_phase = Phase::Idle;
    ActionRun m_run;
    ActionWorkflow m_workflow;
    QString m_mirror;
    QString m_sandboxRoot;
    QString m_worktree;
    QString m_workspace;
    QString m_sandboxHome;
    QString m_sandboxTmp;
    QString m_releaseStaging;
    QString m_buildCache; // empty when this run gets no persistent cache
    int m_buildCacheLock = -1; // flock descriptor for m_buildCache, or -1
    QString m_repoWorkTree; // owner's working copy (may be empty on a mirror-only node)
    QString m_currentStepLabel;
    QString m_currentCommand;
    QMap<QString, QString> m_variables;
    QMap<QString, QString> m_exposedVariables;
    QStringList m_secrets; // values to redact from logs
    bool m_networkAllowed = false;
    ActionSandboxLimits m_limits;
    int m_stepIndex = 0;
    int m_checkoutAttempts = 0;
    qsizetype m_processOutputBytes = 0;
    qsizetype m_processOutputSuppressedBytes = 0;
    qsizetype m_processOutputLineChars = 0;
    QByteArray m_processOutputTail;
    QByteArray m_crashOutputTail;
    bool m_processOutputTruncated = false;
    bool m_processTimedOut = false;
    bool m_jobTimedOut = false;
    QByteArray m_processStdin;
    QString m_systemdUnit;
    QTimer *m_stepTimer = nullptr;
    QTimer *m_jobTimer = nullptr;
    QTimer *m_terminationTimer = nullptr;
    QProcess *m_process = nullptr;
};

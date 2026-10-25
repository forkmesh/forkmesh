#pragma once

#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QString>

namespace ActionStatus {
inline const QString AwaitingApproval = QStringLiteral("awaiting-approval");
inline const QString Queued = QStringLiteral("queued");
inline const QString Running = QStringLiteral("running");
inline const QString Success = QStringLiteral("success");
inline const QString Failed = QStringLiteral("failed");
inline const QString Rejected = QStringLiteral("rejected");
inline const QString Cancelled = QStringLiteral("cancelled");
inline const QString Skipped = QStringLiteral("skipped");
} // namespace ActionStatus

namespace ActionStepStatus {
inline const QString Pending = QStringLiteral("pending");
inline const QString Queued = QStringLiteral("queued");
inline const QString Running = QStringLiteral("running");
inline const QString Success = QStringLiteral("success");
inline const QString Failed = QStringLiteral("failed");
inline const QString Cancelled = QStringLiteral("cancelled");
inline const QString Skipped = QStringLiteral("skipped");
} // namespace ActionStepStatus

struct ActionRun {
    int id = 0;
    QString owner;
    QString name;            // repository name
    QString workflowPath;    // ".forkmesh/deploy.yml"
    QString workflowName;    // display name from the YAML
    QString workflowContent; // raw YAML at the pushed commit (for diff/approval)
    QString commit;          // pushed newrev SHA
    QString ref;             // pushed refname
    QString machine;
    QString repositoryTree;
    QString executionDigest;
    QString status = ActionStatus::Queued;
    // One status per parsed workflow step. These are persisted as the runner
    // crosses each step boundary so the UI can show a live execution graph and
    // recover a useful final graph after a restart.
    QStringList stepStatuses;
    int currentStepIndex = -1;
    // Pull-request runs carry who proposed the code they are about to execute.
    // `firstTimePullAuthor` is the standing gate: a contributor with no landed
    // work in this repository has every one of their pull requests held for a
    // maintainer's approval, whatever the repository's auto-approve setting
    // says, because approving one of their runs must not silently arm the next.
    int pullNumber = 0;
    QString pullAuthor; // display name, or the signer key when unnamed
    bool firstTimePullAuthor = false;
    qint64 createdAtMs = 0;
    qint64 startedAtMs = 0;
    qint64 finishedAtMs = 0;

    // A process owned by the previous desktop instance cannot still be
    // observed after that instance exits. Restart the approved snapshot from
    // the beginning instead of turning an infrastructure interruption into a
    // workflow failure that permanently blocks every `needs:` dependent.
    bool requeueAfterDesktopInterruption();
    QString repoKey() const;
    QString legacyRepoKey() const;
    QJsonObject toJson() const;
    static ActionRun fromJson(const QJsonObject &obj);
};

namespace ActionNeeds {

enum class State {
    Ready,   // every dependency succeeded, or isn't scheduled on this node
    Waiting, // a dependency is still awaiting approval, queued, or running
    Blocked, // a dependency finished without succeeding
};

bool matches(const QString &token, const QString &workflowName,
             const QString &workflowPath);

State resolve(const ActionRun &run, const QStringList &needs,
              const QList<ActionRun> &history, QString *detail = nullptr);

} // namespace ActionNeeds

class ActionStore
{
public:
    explicit ActionStore(QString rootDir); // rootDir = <AppData>/actions

    QString spoolDir() const; // <root>/spool (push events land here)
    bool spoolPushEvent(const QString &owner, const QString &name,
                        const QString &commit, const QString &ref,
                        const QString &before = QString()) const;
    QString artifactsDir() const; // <root>/artifacts (validated run outputs)
    QString heavyRoot() const;
    QString legacySandboxDir() const;
    QString sandboxDir() const; // <root>/sandbox (disposable per-run trees)
    QString cacheRoot() const;
    // <root>/cache/<repoKey>: the one writable Action mount that outlives a
    // run, so compilers can reuse yesterday's objects. Scoped per repository —
    // never shared between two of them — because a run can write anything it
    // likes here and the next run of the same repository will read it.
    QString buildCacheDir(const ActionRun &run) const;
    // Host-owned persistent executables made available read-only to Actions.
    QString toolchainBinDir() const; // <root>/toolchain/bin

    QList<ActionRun> loadAllRuns() const;    // newest first
    ActionRun createRun(ActionRun run);      // assigns id + timestamps, persists
    bool saveRun(const ActionRun &run) const; // rewrite meta.json
    bool deleteRun(const ActionRun &run) const; // remove the run's dir (meta+log)
    // Same, for a batch: forgets them immediately, unlinks the trees on a worker.
    void deleteRunsInBackground(const QList<ActionRun> &runs) const;
    void appendLog(const ActionRun &run, const QString &text) const;
    QString readLog(const ActionRun &run) const;
    // Where readLog() reads from. Handed to agents so they open the whole file
    // instead of working from a tail that stopped before the failing line.
    QString logPath(const ActionRun &run) const;

    static QMap<QString, QString> variables();
    static void setVariables(const QMap<QString, QString> &vars);

    static bool isApproved(const QString &repoKey, const QString &path,
                           const QString &content,
                           const QString &repositoryTree,
                           const QString &executionDigest);
    static void approve(const QString &repoKey, const QString &path,
                        const QString &content,
                        const QString &repositoryTree,
                        const QString &executionDigest);
    static QString lastApprovedContent(const QString &repoKey, const QString &path);

    static bool repositoryStateDigest(const QString &repository,
                                      const QString &commit,
                                      QString *repositoryTree,
                                      QString *executionDigest,
                                      QString *error = nullptr);

#ifdef FORKMESH_ACTION_STORE_TESTS
    int runDirectoryScanCountForTesting() const;
#endif

private:
    QString runsDir() const; // <root>/runs
    QString runDir(const ActionRun &run) const;
    QString legacyRunDir(const ActionRun &run) const;
    QString existingRunDir(const ActionRun &run) const;
    void migrateLegacyRecords() const;
    void ensureRunCacheLoadedLocked() const;
    int nextId() const;

    QString m_root;
    // Status changes used to rescan and parse every persisted run on each
    // signal. ActionStore is the sole writer for run metadata after startup, so
    // keep a write-through in-memory index and pay that filesystem cost once.
    mutable QMutex m_runCacheMutex;
    mutable QHash<int, ActionRun> m_runCache;
    mutable bool m_runCacheLoaded = false;
    mutable int m_nextRunId = 1;
#ifdef FORKMESH_ACTION_STORE_TESTS
    mutable int m_runDirectoryScanCount = 0;
#endif
};

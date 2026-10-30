#pragma once

#include "AgentRuntimeSession.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

class QObject;

namespace AgentStatus {
inline const QString Queued = QStringLiteral("queued");
inline const QString Running = QStringLiteral("running");
inline const QString Waiting = QStringLiteral("waiting");
inline const QString Success = QStringLiteral("success");
inline const QString Failed = QStringLiteral("failed");
inline const QString Stopped = QStringLiteral("stopped");
inline const QString Cleared = QStringLiteral("cleared");

inline bool isTerminal(const QString &status)
{
    return status == Success || status == Failed || status == Stopped;
}
} // namespace AgentStatus

namespace AgentSessionSchema {
inline constexpr int CurrentVersion = 2;
inline constexpr int MinimumSupportedVersion = 1;
} // namespace AgentSessionSchema

struct AgentSession {
    // Version 1 is the legacy unversioned record. Version 2 separates the
    // runtime transport from the model provider and pins its resume protocol.
    int schemaVersion = AgentSessionSchema::CurrentVersion;
    int id = 0;
    // Operator-controlled run order. 1 is the highest priority and 99 the
    // lowest. Older session files did not carry this field and load at the
    // neutral midpoint so their existing relative order is preserved until a
    // row is dragged.
    int priority = 50;
    QString owner;
    QString name;
    int issueNumber = 0;
    QString issueTitle;
    QString prompt;
    // A follow-up waiting for the next process, and the copy handed to a
    // process that has not announced itself yet. Both are durable so closing
    // ForkMesh anywhere between pressing Add and the replacement agent's first
    // event cannot detach the prompt from this session. On startup an in-flight
    // message is moved back to pending because its old process is gone.
    QString pendingSteerMessage;
    QString inFlightSteerMessage;
    QString provider; // codex | openai | claude-api | claude-code
    // Runtime transport is intentionally separate from the model provider.
    // Older records omit it and migrate deterministically from provider.
    QString runtimeId;
    int runtimeProtocolVersion = 1;
    // The provider that owned the conversation before a user-initiated or
    // automatic model handoff. Persisting this marker lets the replacement
    // agent receive the saved transcript even after ForkMesh restarts between
    // the selection and the queued run.
    QString handoffFromProvider;
    QString model;
    // Permission mode this session runs under, as the label from the composer's
    // mode selector ("Ask" / "Edit" / "Plan" / "Auto"; sessions written before
    // shortened them say "Ask before edits" / "Auto mode" / …).
    // Captured when a follow-up prompt is sent so the next resume honors the live
    // selection, and shown in the agent header. Empty falls back to the global
    // kClaudeAutoModeSetting. Only "Auto" skips the CLI's permission prompts
    // today (see agentModeSkipsPermissions).
    QString mode;
    bool createPr = false;
    bool associationOnly = false;
    // Runs straight in the repository's own checkout: no agent branch, no
    // worktree (the prompt's "worktree" checkbox, unticked). branchName stays
    // empty, so every branch-keyed landing, merge and cleanup path skips it.
    bool directCheckout = false;
    bool yolo = false;
    bool vibe = false;
    bool genie = false;
    QString strength;
    bool orgTask = false;
    QString orgTaskId;
    bool orgTaskAttached = false;
    QString startedByBot;
    QString finishedByBot;
    QString titledBy;
    // The teammate that started this run, stamped when it launches. A shared
    // team run leaves teammateId empty and records every participant in
    // teammateIds instead. A teammate's own config only remembers its newest
    // session, so this durable attribution keeps the relationship after a
    // restart. Both are empty for a run the user started by hand.
    QString teammateId;
    QStringList teammateIds;
    QString teammateName;
    QString teammateAvatar; // the avatar key its portrait is drawn from
    // A mention in the private Team room binds human-facing assistant prose to
    // one chat thread. Tool calls, command output, and raw transcript events
    // remain visible only in the full agent session.
    QString chatConversation;
    QString chatThreadRootId;
    // Exact relay where a human started this Agent/Team thread. Replies are
    // delivered only while that same configured instance is active, so a late
    // completion cannot cross into another relay after the user switches.
    QString originInstanceId;
    QString originInstanceUrl;
    bool hasTeammateAttribution() const
    {
        return !teammateId.isEmpty() || !teammateIds.isEmpty();
    }
    // Snapshot of the skills installed into this run's prompt. Keeping the
    // names on the session makes the skills chart historical: editing or
    // deleting a teammate later must not rewrite what an earlier agent used.
    QStringList skillsUsed;
    // Operator assessment of a teammate task after it reaches a terminal
    // state. Zero means not yet graded; valid grades are 1 through 5.
    int taskGrade = 0;
    // A website-created organization job remains tied to its authenticated
    // Worker lease until the terminal result is acknowledged. Persisting the
    // exact transport envelope lets a resumed local run report completion after
    // ForkMesh itself restarts without claiming or executing the prompt twice.
    QJsonObject orgAgentJob;
    // The relay's id for the web session this run came from, kept for the life
    // of the session. orgAgentJob above is the live lease and is cleared the
    // moment the result is acknowledged; this outlives it, so the Agents list
    // can still tell that the finished local run and the website's row are one
    // run and show it once. Empty for a run started on this desktop.
    QString webSessionId;
    QString claudeAppSessionId;
    QString claudeImportId;
    QString claudeImportPath;
    QString claudeImportCwd;
    qint64 claudeImportBytes = 0;
    bool claudeImported() const { return !claudeImportId.isEmpty(); }
    bool claudeImportMirroring() const { return claudeImported() && claudeImportBytes >= 0; }
    int prNumber = 0;
    QString status = AgentStatus::Queued;
    bool held = false;
    QString heldBy;
    QString branchName;
    // Explicit directory name for newer sessions whose branch and worktree use
    // one shared semantic name. Empty preserves legacy derived worktree paths.
    QString worktreeName;
    QString baseRef;    // base commit SHA captured at run start (worktree/diff)
    QString baseBranch; // base branch the PR targets (e.g. main)
    QString mergeCandidateHead;
    bool merged = false;
    // Whether ForkMesh can vouch for that merge: it performed it itself, or the
    // pull ledger calls the session's PR merged. Verdicts the background sweep
    // only inferred from Git stay unattested, and are withdrawn again when the
    // branch turns out to have landed nothing of its own.
    bool mergeAttested = false;
    qint64 mergedAtMs = 0;
    // Set only on sessions read out of the recycle bin, from the bin's
    // own stamp rather than meta.json: a restored session is byte for
    // byte the record that was deleted, so this never round-trips
    // through toJson().
    qint64 deletedAtMs = 0;
    qint64 createdAtMs = 0;
    qint64 startedAtMs = 0;
    qint64 finishedAtMs = 0;
    int promptTokens = 0;
    int completionTokens = 0;
    int totalTokens = 0;
    int contextTokens = 0;
    int contextWindow = 0;
    int maxOutputTokens = 0;
    int estimatedCredits = 0;
    double costUsd = 0.0;
    double spendBeforeUsd = 0.0;
    double spendAfterUsd = 0.0;
    int numTurns = 0;
    qint64 durationMs = 0;
    QString lastError;

    // Wall clock this session has spent working, which durationMs does not
    // answer: that is the CLI's own duration_ms for the single turn that
    // reported last, so a run twenty minutes old can carry a 38 ms figure.
    // runningSinceMs anchors the stretch in flight, runningMs banks the ones
    // that already closed. Both are needed because a resumed conversation
    // deliberately keeps its first turn's startedAtMs (see
    // markAgentSessionRunning) and would otherwise count the hours it sat idle
    // between turns as time on task.
    qint64 runningMs = 0;
    qint64 runningSinceMs = 0;

    // Live-only, never serialized (see toJson): the CLI has reported its turn
    // finished but the process tree it started — a build, a test run, a server —
    // is still alive. The session stays Running until that drains, and these
    // carry what is still going so every status surface says the agent is
    // finishing background work instead of calling the run done.
    int backgroundProcesses = 0;
    bool backgroundProcessesBusy = false; // burning CPU, not just parked
    QString backgroundProcessSummary;     // "ninja (PID 1234), cc1plus (PID 1240)"

    // Live-only as well: this row stands for a cloud run the account queued on
    // one of its mirrors (AgentCloudSessions.h). Nothing behind it is on this
    // disk, so it is never saved; the list marks it with a cloud and the
    // actions go to the relay instead of a local process.
    bool cloud = false;
    QString cloudSessionId; // the relay's id for the run
    QString cloudNode;      // the node that claimed (or is asked to claim) it
    bool cloudHere = false; // that node is this desktop, which has not run it

    bool finishingBackgroundWork() const
    {
        return backgroundProcesses > 0 && !merged &&
               status == AgentStatus::Running;
    }

    bool awaitingStart() const
    {
        return held && status == AgentStatus::Queued && !merged;
    }

    bool genieInFlight() const
    {
        return genie && !merged &&
               (status == AgentStatus::Running || status == AgentStatus::Queued ||
                status == AgentStatus::Waiting);
    }

    // A turn that is working, or one blocked on the user's approval part-way
    // through: the run is open either way, so its clock is still going.
    bool runInFlight() const
    {
        return !merged && (status == AgentStatus::Running ||
                           status == AgentStatus::Waiting);
    }

    // Where the stretch now being timed began. Sessions written before the
    // ledger existed have no anchor, and for them the run started when
    // startedAtMs says it did.
    qint64 runAnchorMs() const
    {
        return runningSinceMs > 0 ? runningSinceMs : startedAtMs;
    }

    // Wall clock of the stretch runAnchorMs() opened: counting up while the run
    // is in flight, the closed span once it finished, 0 when there is nothing
    // to measure (never started, or requeued after a restart).
    qint64 currentRunMs(qint64 nowMs) const
    {
        const qint64 since = runAnchorMs();
        if (since <= 0)
            return 0;
        const qint64 end = runInFlight() ? nowMs : finishedAtMs;
        return end > since ? end - since : 0;
    }

    // Everything this session has spent running, the stretch in flight included.
    qint64 totalRunMs(qint64 nowMs) const
    {
        return runningMs + currentRunMs(nowMs);
    }

    // Open a new stretch at nowMs, banking the one it replaces. Call this
    // BEFORE flipping status or restamping startedAtMs, because which end the
    // banked stretch gets depends on the state it is leaving: one still in
    // flight — a turn resuming from a permission prompt — never stopped
    // working and is banked up to now, while one that already finished is
    // banked at its finish stamp, so the hours it then sat idle are not counted
    // as work. A stretch that ended without a stamp (a transport that died, a
    // requeue after a restart) is left unbanked rather than guessed at.
    void beginRun(qint64 nowMs)
    {
        runningMs += currentRunMs(nowMs);
        runningSinceMs = nowMs;
    }

    QString repoKey() const;
    QString effectiveRuntimeId() const;
    bool schemaSupported() const;
    bool canResumeWithRuntime(const AgentRuntimeDescriptor &runtime,
                              QString *reason = nullptr) const;
    QJsonObject toJson() const;
    static AgentSession fromJson(const QJsonObject &obj);
};

struct AgentModelOutcome {
    int runs = 0;
    int merged = 0;
};

class AgentStore
{
public:
    explicit AgentStore(QString rootDir);

    QList<AgentSession> loadAllSessions() const;
    static QList<int> queuedSessionIdsOldestFirst(
        const QList<AgentSession> &sessions);
    AgentSession createSession(AgentSession session);
    bool saveSession(const AgentSession &session) const;
    bool deleteSession(const AgentSession &session) const;

    // Deleting an agent moves its directory into the recycle bin instead of
    // removing it, so a delete stays recoverable for a bounded window
    // (issue #578). Retention is a user setting; the store only enforces
    // whatever window it is handed.
    static constexpr int kDefaultRecycleRetentionHours = 24;
    static constexpr int kMinRecycleRetentionHours = 1;
    static constexpr int kMaxRecycleRetentionHours = 24 * 30;
    bool recycleSession(const AgentSession &session) const;
    QList<AgentSession> loadRecycledSessions() const;
    bool restoreRecycledSession(int sessionId) const;
    bool purgeRecycledSession(int sessionId) const;
    // Returns how many bins were removed. nowMs is passed in rather than read
    // here so the sweep is testable without waiting a day.
    int purgeExpiredRecycledSessions(qint64 retentionMs, qint64 nowMs) const;
    void appendLog(const AgentSession &session, const QString &text) const;
    QString logPath(const AgentSession &session) const;
    void flushLog(const AgentSession &session) const;
    static void setLogObserver(QObject *context, std::function<void(int sessionId)> observer);

    static QString modelOutcomeKey(const QString &provider, const QString &model);
    QHash<QString, AgentModelOutcome> retiredModelOutcomes() const;
    QString readLog(const AgentSession &session) const;
    void appendEvent(const AgentSession &session, const QJsonObject &ev) const;
    QList<QJsonObject> loadEvents(const AgentSession &session) const;
    QList<QJsonObject> loadRecentEvents(const AgentSession &session,
                                        qint64 tailBytes) const;
    static constexpr qint64 kLatestOutputTailBytes = 512 * 1024;
    void clearEvents(const AgentSession &session) const;
    // Event and human-log appends each use one shared serial background lane. This
    // callback runs on context's thread after every write queued before it has
    // completed, and is used to keep process exit behind its final events.
    void whenTranscriptWritesDrained(QObject *context,
                                     std::function<void()> callback) const;
    static void flushTranscriptWrites();
    void writePatch(const AgentSession &session, const QString &patch) const;
    QString readPatch(const AgentSession &session) const;

    static constexpr qint64 kTranscriptSearchTailBytes = 512 * 1024;
    int searchTranscript(const AgentSession &session, const QString &needle,
                         QString *snippet = nullptr) const;

    static QStringList attachmentPathsIn(const QString &text);
    QStringList attachmentPaths(const AgentSession &session) const;
    QString transcriptStamp(const AgentSession &session) const;
    static constexpr qint64 kAttachmentScanBytes = 128 * 1024;

private:
    QString sessionsDir() const;
    QString recycleBinDir() const;
    QString recycledSessionDir(const AgentSession &session) const;
    QString findRecycledSessionDir(int sessionId) const;
    QString sessionDir(const AgentSession &session) const;
    QString modelOutcomesPath() const;
    void recordRetiredSession(const AgentSession &session) const;
    int nextId() const;

    QString m_root;
};

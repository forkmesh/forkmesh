#pragma once

// Cloud agent sessions: runs the account queued on the relay's owner agent
// queue and one of its mirrors (or another of its desktops) claimed. The relay
// answers GET /api/repo/{owner}/{repo}/agent-sessions with their projection;
// this is that payload turned into rows the Agents list can show next to the
// sessions this desktop runs itself. Qt6::Core only, so it is tested without a
// window (tests/test_agent_cloud_sessions.cpp).

#include "AgentStore.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace forkmesh::agents {

struct CloudAgentImage {
    QString id;
    QString mediaType;
    qint64 size = 0;
};

struct CloudAgentTurn {
    QString role; // "user" or "assistant"
    QString author;
    QString text;
    qint64 atMs = 0;
    QList<CloudAgentImage> images;
};

struct CloudAgentSession {
    QString sessionId;
    QString owner;
    QString relayOwner;
    QString name;
    QString provider;
    QString status; // the relay's word: queued, running, completed, failed …
    QString targetNode;
    QString title;
    QString taskId;
    QString createdBy;
    QString model;
    QString branchName;
    QString baseBranch;
    QString baseRef;
    QString runner;
    QString mergeState;
    bool canMerge = false;
    bool canOpenPull = false;
    bool canCleanup = false;
    bool branchDeleted = false;
    bool diffKnown = false;
    int diffFiles = 0;
    int diffAdded = 0;
    int diffRemoved = 0;
    int diffAhead = 0;
    QString diagnostic; // what the relay says is going on, in a sentence
    QString diagnosticCode;
    QString lastError;
    bool needsAttention = false;
    bool merged = false;
    bool createPr = false;
    // The relay queued this run for this very desktop. Shown all the same: a
    // job aimed here is only claimed while ForkMesh is running and syncing, so
    // the row is how a person sees the work that is waiting for this machine.
    bool targetsThisNode = false;
    bool onRunner = false;
    int localAgentId = 0; // the claiming node's own id; 0 until it starts
    int issueNumber = 0;
    int prNumber = 0;
    int promptTokens = 0;
    int completionTokens = 0;
    int totalTokens = 0;
    double costUsd = 0.0;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    qint64 completedAtMs = 0;
    qint64 startedAtMs = 0;
    qint64 finishedAtMs = 0;
    qint64 mergedAtMs = 0;
    qint64 durationMs = 0;
    QList<CloudAgentTurn> history;
    // The account-wide list carries the tail of a long conversation, not all
    // of it. Set when the relay said there is more, so opening the run reads
    // the session's own route for the rest.
    bool historyTruncated = false;
    bool organization = false;

    // One run, however often it is fetched: the list keeps a row's synthetic
    // id (and with it the selection) stable across refreshes by this.
    QString key() const;
    QString relayRepoOwner() const;
    // Whether the relay would take a follow-up: a node has started the run and
    // the run is not one the preflight turned away.
    bool promptable() const;
    // Whether this desktop could take the run over and run it itself: the
    // relay still has it waiting and no node has started it. The node it was
    // queued for does not matter — taking over an unclaimed run is what
    // answers "the mirror never picked this up".
    bool claimable() const;
};

// The sessions in one agent-sessions payload, addressed to owner/name. Runs
// targeted at `thisNode` are marked (targetsThisNode) rather than dropped; the
// window hides the ones it is already running itself, which it knows from its
// own sessions and not from the node name.
QList<CloudAgentSession> parseCloudAgentSessions(const QJsonObject &payload,
                                                 const QString &owner,
                                                 const QString &name,
                                                 const QString &thisNode);

// The same, for the account-wide payload (GET /api/account/agent-sessions):
// every repository at once, so each row names its own `org` and `repo`.
QList<CloudAgentSession> parseCloudAgentAccountSessions(
    const QJsonObject &payload, const QString &thisNode);

// The relay's status in the list's vocabulary (AgentStatus).
QString cloudAgentLocalStatus(const QString &status);

// The list row that stands for `cloud`, carrying `id` as its synthetic id.
AgentSession cloudAgentListEntry(const CloudAgentSession &cloud, int id);

// The run's conversation as transcript events: user turns as
// {"type":"user"}, the node's replies as {"type":"assistant"}, each stamped
// with "_ts" so the bubbles are dated.
QList<QJsonObject> cloudAgentTranscriptEvents(const CloudAgentSession &cloud,
                                              const QString &imageDirectory = QString());

QString cloudAgentImageFileName(const CloudAgentSession &cloud,
                                const CloudAgentImage &image);

// Proof prefixes the relay accepts from a desktop that holds the account's key
// and no session token (route_handlers.py, OWNER_AGENT_*_PROOF).
inline QString cloudAgentListProof()
{
    return QStringLiteral("forkmesh-owner-agent-list-v1");
}
// The account's whole queue in one read, whichever repository is open.
inline QString cloudAgentAccountListProof()
{
    return QStringLiteral("forkmesh-owner-agent-account-list-v1");
}
inline QString cloudAgentReadProof()
{
    return QStringLiteral("forkmesh-owner-agent-read-v1");
}
inline QString cloudAgentPromptProof()
{
    return QStringLiteral("forkmesh-owner-agent-prompt-v1");
}
inline QString cloudAgentDeleteProof()
{
    return QStringLiteral("forkmesh-owner-agent-delete-v1");
}
inline QString cloudAgentStopProof()
{
    return QStringLiteral("forkmesh-owner-agent-stop-v1");
}
inline QString cloudAgentQueueProof()
{
    return QStringLiteral("forkmesh-owner-agent-queue-v1");
}
inline QString cloudAgentBranchProof()
{
    return QStringLiteral("forkmesh-owner-agent-branch-v1");
}

QString cloudAgentBranchActionText(const QJsonObject &body);
QJsonObject cloudAgentMergeBody(const QString &requestId);
QJsonObject cloudAgentCleanupBody(const QString &requestId);
QJsonObject cloudAgentLinkPullBody(int number);
QJsonObject cloudAgentUpdateBody(const QString &requestId);
QString cloudAgentMergedTaskNote(const CloudAgentSession &cloud,
                                 const QJsonObject &payload);
QString cloudAgentMergeOutcome(const QJsonObject &payload,
                               const QString &branch, const QString &base);
enum class CloudAgentAction {
    ClaimHere,
    FollowUp,
    ContinueLocally,
    RetryInCloud,
    SignInRunner,
};

bool cloudAgentFinished(const CloudAgentSession &cloud);
bool cloudAgentDeleteFoundNothing(int httpStatus, const QJsonObject &payload);
bool cloudAgentNeedsRunnerSignIn(const CloudAgentSession &cloud);
QList<CloudAgentAction> cloudAgentActions(const CloudAgentSession &cloud);
QString cloudAgentOriginalPrompt(const CloudAgentSession &cloud);
QString cloudAgentContinuationPrompt(const CloudAgentSession &cloud);
QString cloudAgentContinuationTitle(const CloudAgentSession &cloud);
QString cloudAgentQueueRuntime(const QString &provider);

bool cloudAgentStoppable(const CloudAgentSession &cloud);
bool cloudAgentBranchMerged(const CloudAgentSession &cloud);
QString cloudAgentContinueBranch(const CloudAgentSession &cloud);
QString cloudAgentRunnerName(const CloudAgentSession &cloud);

enum class CloudContinueStart {
    SessionBranch,
    NoBranch,
    BranchMissing,
    BranchMerged,
};

struct CloudAgentContinuePlan {
    CloudContinueStart start = CloudContinueStart::NoBranch;
    QString branch;
    QString startBranch;
    QString landingBase;
    QString notice;
};

CloudAgentContinuePlan cloudAgentContinuePlan(const CloudAgentSession &cloud,
                                              bool branchFetched);
QString cloudAgentBranchContinuationPrompt(const CloudAgentSession &cloud,
                                           const CloudAgentContinuePlan &plan);

QString cloudAgentQueueText(const QString &provider, const QString &model,
                            bool createPr, const QString &fromDevice,
                            const QString &prompt);

QString cloudAgentTaskId(const QString &taskKey, const QString &rawTitle);
QString cloudAgentDisplayTitle(const QString &raw);
QString cloudAgentShortTitle(const QString &title, int maxChars);
void fileCloudAgentSessionsUnderLocalRepos(
    QList<CloudAgentSession> &sessions,
    const QList<QPair<QString, QString>> &localRepos);
qint64 cloudAgentAccountFinishedBefore(const QJsonObject &payload);
bool cloudAgentBeyondListedWindow(const CloudAgentSession &cloud,
                                  qint64 finishedBeforeMs);

// Where a desktop asks the relay to hand it a queued run to continue here.
// Signed with the node/account proof the job queue already uses, not with the
// list proofs above, so the relay sees the same claimant it sees on a drain.
inline QString cloudAgentClaimPath(const QString &sessionsPath)
{
    return sessionsPath + QStringLiteral("/claim");
}

// What the account key signs: the operation, the signer, the exact path, the
// digest of the follow-up when there is one, and the time.
QByteArray cloudAgentProofCanonical(const QString &proof, const QString &user,
                                    const QString &path, const QString &prompt,
                                    const QString &timestamp);

struct HandoffDesktop {
    QString key;
    QString node;
    QString label;
    qint64 lastSeenMs = 0;
};

QList<HandoffDesktop> handoffDesktops(const QJsonObject &payload);
QString handoffDesktopSeenText(qint64 lastSeenMs, qint64 nowMs);

} // namespace forkmesh::agents

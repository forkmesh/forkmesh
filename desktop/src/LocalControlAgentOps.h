#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// The "agentops." local control family (WS10; docs/native/DESKTOP_PARITY.md
// section 19): approvals, merge & clean up, bulk actions and usage for the owner's agents. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlAgentOps.cpp. Gate 2 creates it empty; WS10 owns it.
namespace forkmesh::localcontrol::agentops {

// The method prefix this family answers.
QString familyPrefix();

QStringList actions();
bool isKnownAction(const QString &action);
bool isDestructiveAction(const QString &action);

constexpr int kMaxBulkIds = 50;
constexpr int kMaxBinAgents = 500;
constexpr int kMaxApprovals = 64;
constexpr int kMaxWorktrees = 60;
constexpr int kMaxGrants = 200;
constexpr int kMaxQuestionChars = 1024;
constexpr int kMaxCommandChars = 2000;
constexpr int kMaxScannedEvents = 400;
constexpr int kMaxDiffChars = 400000;
constexpr int kMaxDiffFiles = 500;
constexpr int kMaxUntracked = 200;
constexpr qint64 kMaxDiffImageBytes = 2 * 1024 * 1024;

QString confirmationHolder(const QString &deviceKey, int connectionId);

QStringList lifetimes();
bool validLifetime(const QString &lifetime);
QString answerText(bool allow, const QString &lifetime);

QString approvalSummary(bool allow, const QString &lifetime,
                        const QString &question, const QString &sessionTitle);
QString mergeSummary(const QString &branch, const QString &base,
                     const QString &repo);
QString bulkSummary(const QString &action, int count);
QString deleteSummary(const QString &branch, const QString &repo, bool running = false);
QString outsideDeleteSummary(const QString &label, const QString &cwd);
QString purgeSummary(int count);

struct DeleteFacts {
    bool agentGone = false;
    bool settled = true;
    QString branch;
    bool branchExisted = false;
    bool branchLeft = false;
    QString worktree;
    bool worktreeExisted = false;
    bool worktreeLeft = false;
    int issue = 0;
    bool issueFound = false;
    QString issueStatus;
    int pull = 0;
    bool pullMerged = false;
    bool pullReachable = true;
    QString task;
    QString taskStatus;
    QString taskReason;
};
struct DeletePart {
    QString kind;
    QString name;
    QString status;
    QString reason;
};
QList<DeletePart> deleteParts(const DeleteFacts &facts);
QJsonArray deletePartsJson(const QList<DeletePart> &parts);

struct DeleteTrace {
    QString taskStatus;
    QString taskReason;
    std::function<void(bool)> done;
    void finish(bool ok);
};

struct MergeShortstat {
    QString sha;
    QString subject;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
};
QList<MergeShortstat> parseMergeShortstatLog(const QString &log);

struct MergeVelocityWindow {
    QString id;
    QString label;
    int count = 0;
    int top = 0;
    double avg = 0;
    QVector<int> series;
};
struct MergeStats {
    QString base;
    QString commit;
    int commits = -1;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
    QList<MergeVelocityWindow> velocity;
};
QJsonObject mergeStatsJson(const MergeStats &stats);
QJsonArray numstatRows(const QString &numstat, int max = kMaxDiffFiles);
QString capDiff(const QString &text, int budget, bool *truncated);
QStringList untrackedPaths(const QString &porcelain, int max = kMaxUntracked);
QString diffImageMime(const QString &path);
bool validDiffImagePath(const QString &path);
QString revokeSummary(const QString &capability, const QString &resource);

QString answerTarget(const QString &approvalId, bool allow,
                     const QString &lifetime);

QString approvalId(int sessionId, const QString &requestId,
                   const QByteArray &salt);
bool validApprovalId(const QString &id);

struct PendingQuestion {
    QString requestId;
    QString question;
    QJsonObject input;
    qint64 askedAtMs = 0;
};
QList<PendingQuestion> questionsInEvents(const QList<QJsonObject> &events,
                                         int maxScanned = kMaxScannedEvents);

QString approvalKind(const QString &capability);
QString commandFromQuestion(const QString &question);
QString capabilityFromQuestion(const QString &question);

struct ApprovalFacts {
    QString id;
    int sessionId = 0;
    QString title;
    QString repo;
    QString provider;
    QString capability;
    QString question;
    qint64 askedAtMs = 0;
};
QJsonObject approvalRow(const ApprovalFacts &facts);

QString approvalsSignature(QStringList ids);

bool canonicalIds(const QJsonArray &ids, QList<int> *out, int max = kMaxBulkIds);
QString bulkTarget(const QString &action, const QList<int> &ids);

struct BinEntry {
    int id = 0;
    QString title;
    QString owner;
    QString name;
    QString branch;
    QString provider;
    qint64 deletedAtMs = 0;
};
QString binTitle(int issueNumber, const QString &issueTitle, const QString &prompt);
QJsonObject binRow(const BinEntry &entry, qint64 retentionMs, qint64 nowMs);
QJsonObject binPayload(const QList<BinEntry> &entries, int retentionHours,
                       int minHours, int maxHours, qint64 nowMs);

struct WorktreeEntry {
    QString path;
    QString head;
    QString branch;
    bool bare = false;
    bool detached = false;
    bool locked = false;
    bool prunable = false;
};
QList<WorktreeEntry> parseWorktreePorcelain(const QString &text,
                                            int max = kMaxWorktrees);
QJsonObject worktreeRow(const WorktreeEntry &entry, bool isMain, int ahead,
                        int behind, int sessionId);
bool parseLeftRight(const QString &text, int *behind, int *ahead);

struct UsageWindow {
    QString id;
    QString label;
    int usedPercent = -1;
    qint64 resetAtMs = 0;
    bool exhausted = false;
    qint64 windowMs = 0;
};
struct UsageAccount {
    QString id;
    QString label;
    bool active = false;
    bool signedIn = false;
    QString identity;
    QList<UsageWindow> windows;
};
struct UsageProvider {
    QString id;
    QString label;
    QList<UsageWindow> windows;
    QList<UsageAccount> accounts;
    bool creditsEnabled = false;
    double creditsUsed = -1;
    double creditsLimit = -1;
    QString creditsCurrency;
};
QJsonObject usageSummary(const QList<UsageProvider> &providers,
                         double claudeSpendUsd, double openAiSpendUsd,
                         qint64 nowMs);
QList<UsageWindow> codexWindows(const QJsonArray &rows);

QJsonArray grantRows(const QJsonArray &persistentGrants, int max = kMaxGrants);
bool validGrantId(const QString &id);

constexpr int kApprovalSaltBytes = 32;
QByteArray approvalSaltFromStored(const QString &hex);
QByteArray newApprovalSalt();

constexpr int kMaxPingSummaryChars = 200;
constexpr int kMaxPingAgentChars = 80;
constexpr int kMaxPingLedger = 256;

QString approvalPingProof();
QString approvalPingPath();

struct ApprovalPing {
    QString sessionId;
    QString approvalId;
    QString agent;
    QString summary;
};
ApprovalPing approvalPingFor(const QJsonObject &approvalRow);
bool validPingField(const QString &value);
QString approvalPingResource(const ApprovalPing &ping);
QString approvalPingCanonical(const QString &user, const ApprovalPing &ping,
                              const QString &ts);
QJsonObject approvalPingBody(const ApprovalPing &ping);
QString approvalPingAgent(const QString &provider);
QString approvalPingSummary(const QString &capability, const QString &repo);
bool approvalPingRetryable(int httpStatus, bool networkError);

class ApprovalPingLedger
{
public:
    explicit ApprovalPingLedger(const QStringList &stored = {},
                                int max = kMaxPingLedger);
    bool contains(const QString &approvalId) const;
    bool add(const QString &approvalId);
    void remove(const QString &approvalId);
    QStringList ids() const { return m_ids; }
    QList<ApprovalPing> unsent(const QJsonArray &approvals) const;

private:
    QStringList m_ids;
    int m_max = kMaxPingLedger;
};

} // namespace forkmesh::localcontrol::agentops

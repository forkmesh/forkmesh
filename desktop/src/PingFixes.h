#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

namespace forkmesh::pingfixes {

inline constexpr int kTrustStreak = 3;
inline constexpr qint64 kSettleMs = 24LL * 60 * 60 * 1000;
inline constexpr double kPriorWeight = 2.0;
inline constexpr int kMaxSignatures = 400;

namespace fix {
inline constexpr char kDisableWorkflow[] = "workflow.disable";
inline constexpr char kEnableWorkflow[] = "workflow.enable";
inline constexpr char kRerun[] = "run.rerun";
inline constexpr char kApprove[] = "run.approve";
inline constexpr char kReject[] = "run.reject";
inline constexpr char kAgentFix[] = "agent.fix";
inline constexpr char kAgentRetry[] = "agent.retry";
}

enum class Cause {
    None,
    HostOffline,
    Network,
    Timeout,
    MissingTool,
    Auth,
    Disk,
    Test,
    Build,
    Unknown,
};

enum class Event {
    Other,
    ActionFailed,
    ActionStopped,
    ActionApproval,
    WorkflowAutoDisabled,
    AgentFailed,
    Warning,
};

struct Context {
    QString kind;
    QString title;
    QString body;
    bool warning = false;
    int runId = -1;
    QString runStatus;
    QString repo;
    QString workflowPath;
    QString workflowName;
    bool workflowDisabled = false;
    int agentSession = 0;
};

struct Fix {
    QString id;
    QString label;
    QString detail;
    QString icon;
    double prior = 0;
    double score = 0;
    bool autoSafe = false;
    bool trusted = false;
};

struct Stats {
    int offered = 0;
    int chosen = 0;
    int ok = 0;
    int failed = 0;
    int undone = 0;
    int streak = 0;
    int autoApplied = 0;
    qint64 pendingSinceMs = 0;
    qint64 lastMs = 0;
};

QString familyPrefix();
QString ledgerFileName();

QString causeToken(Cause cause);
Cause causeOf(const QString &text);
QString eventToken(Event event);
Event eventOf(const Context &context);
QString signature(const Context &context);
QString familySignature(const Context &context);
QList<Fix> candidates(const Context &context);
bool deferredOutcome(const QString &fixId);
QString undoFor(const QString &fixId);
QJsonObject fixJson(const Fix &fix, bool primary);

class Ledger
{
public:
    void load(const QJsonObject &json);
    QJsonObject toJson() const;

    void offered(const Context &context, qint64 nowMs);
    void chosen(const Context &context, const QString &fixId, qint64 nowMs);
    void outcome(const Context &context, const QString &fixId, bool ok, qint64 nowMs);
    void undone(const Context &context, const QString &fixId, qint64 nowMs);
    void autoApplied(const Context &context, const QString &fixId, qint64 nowMs);
    void recurred(const Context &context, qint64 nowMs);

    QList<Fix> ranked(const Context &context, qint64 nowMs) const;
    bool autoFix(const Context &context, qint64 nowMs, Fix *out) const;
    Stats stats(const QString &signature, const QString &fixId) const;
    bool isEmpty() const { return m_stats.isEmpty(); }

private:
    Stats settled(const QString &signature, const QString &fixId, qint64 nowMs) const;
    void touch(const QString &signature, const QString &fixId,
               const std::function<void(Stats &)> &change, qint64 nowMs);
    void trim();

    QHash<QString, QHash<QString, Stats>> m_stats;
};

double score(const Stats &stats, double prior);

}

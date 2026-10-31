#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::teammates {

inline const QString kScheduleOnDemand = QStringLiteral("on-demand");
inline const QString kScheduleContinuous = QStringLiteral("continuous");
inline const QString kScheduleHourly = QStringLiteral("hourly");
inline const QString kScheduleDaily = QStringLiteral("daily");
inline const QString kScheduleWeekly = QStringLiteral("weekly");
inline const QString kScheduleWeekdays = QStringLiteral("weekdays");
inline const QString kScheduleCustom = QStringLiteral("custom");
inline const QString kScheduleEvents = QStringLiteral("events");

inline const QString kTaskTypeOnDemand = QStringLiteral("on-demand");
inline const QString kTaskTypeScheduled = QStringLiteral("scheduled");
inline const QString kTaskTypeNewIssues = QStringLiteral("new-issues");
inline const QString kTaskTypeNewPulls = QStringLiteral("new-pulls");
inline const QString kTaskTypeAssignments = QStringLiteral("assignments");
inline const QString kTaskTypeMentions = QStringLiteral("mentions");
inline const QString kTaskTypeUiStalls = QStringLiteral("ui-stalls");
inline const QString kTaskTypeErrorLogs = QStringLiteral("error-logs");
inline const QString kErrorFixerId = QStringLiteral("error-fixer");

struct AgentChoice {
    QString provider;
    QString model;
};

struct LogMonitor {
    QString badge;
    int threshold = 0;
    QString taskType;
    bool active = true;
    // Older session-cumulative monitors fire once the count is greater than
    // their threshold. Batch monitors use an inclusive boundary so "after 10
    // errors" means the tenth new error starts the repair run.
    bool inclusive = false;

    QJsonObject toJson() const;
    static LogMonitor fromJson(const QJsonObject &object);
};

// Durable state for a teammate log monitor whose count resets only after a
// successful repair. The in-flight batch is kept separate so errors arriving
// while the agent works remain queued for the next run. A failed run restores
// its entire batch instead of advancing the repair checkpoint.
struct LogMonitorRuntime {
    qint64 monitoringSinceMs = 0;
    qint64 lastSuccessfulRunAtMs = 0;
    qint64 lastFixedThroughMs = 0;
    int pendingCount = 0;
    QStringList pendingSamples;
    int sessionId = 0; // -1 reserves a launch against nested event loops
    int inFlightCount = 0;
    QStringList inFlightSamples;
    qint64 inFlightThroughMs = 0;
    // A run that ended without Success holds the queue until this time, so a
    // run the provider refused - a spent usage window, say - is not handed
    // straight back on the next log line. Cleared by the next success.
    int failedRuns = 0;
    qint64 retryNotBeforeMs = 0;

    QJsonObject toJson() const;
    static LogMonitorRuntime fromJson(const QJsonObject &object);
    void observe(const QString &sample, qint64 observedAtMs);
    bool runIsDue(const LogMonitor &monitor, qint64 nowMs) const;
    bool reserveRun(qint64 throughMs);
    bool attachSession(int newSessionId);
    void finishRun(bool success, qint64 completedAtMs);
    void abandonRun();
    int discardPendingSamples(const QList<int> &indexes);
    // Hold the restored batch after a failed run: until `resumeAtMs` when the
    // provider named when its usage window resets, otherwise for a backoff
    // that doubles with each consecutive failure.
    void deferRetry(qint64 failedAtMs, qint64 resumeAtMs = 0);
    bool retryHeld(qint64 nowMs) const;
};

// Native workflows run inside the trusted desktop process instead of handing
// financial operations to a coding-agent prompt. Repository-authored teammate
// configuration may select a workflow, but each workflow still enforces its
// own local authorization and custody boundary.
inline const QString kWorkflowContributorPayouts =
    QStringLiteral("contributor-payouts");

struct Schedule {
    QString mode = kScheduleOnDemand;
    QTime time = QTime(9, 0);
    QList<int> days;
    QStringList events;
    bool active = false;

    QJsonObject toJson() const;
    static Schedule fromJson(const QJsonObject &object);
};

struct Config {
    QString id;
    QString name;
    QString avatar = QStringLiteral("sol");
    QString specialty;
    QString focus;
    QString description;
    QList<LogMonitor> logMonitors;
    QStringList skills;
    QStringList skillRefs;
    QString shortcut;
    QString instructions;
    QString workflow;
    // Every prompt-capable teammate carries a small, repository-local journal
    // forward. It is learned from the teammate's own closing reflections and
    // is deliberately data-only: it cannot add permissions or alter config.
    bool selfImproving = true;
    int improvementCycles = 0;
    QStringList improvementNotes;
    // Optional mesh room that receives a concise update when this teammate
    // finishes. The built-in device-local #team room admits configured
    // teammates; other private rooms reject relays. Empty keeps work in Agent UI.
    QString relayChannel;
    QStringList marketingChannels;
    Schedule schedule;
    QSet<QString> permissions;
    QStringList activity;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    qint64 lastRunAtMs = 0;
    int lastSessionId = 0;
    QString lastTrigger;
    bool enabled = true;
    int runWindowHours = 0;
    bool lowCostModel = false;

    QJsonObject toJson() const;
    static Config fromJson(const QJsonObject &object, QString *error = nullptr);
};

struct LoadResult {
    QList<Config> teammates;
    QStringList errors;
};

class Store
{
public:
    explicit Store(QString rootDirectory);

    QString rootDirectory() const { return m_rootDirectory; }
    QStringList directories() const;
    QString pathFor(const QString &id) const;
    LoadResult load() const;
    bool save(Config *config, QString *error = nullptr) const;
    bool remove(const QString &id, QString *error = nullptr) const;

    static QString safeId(const QString &value);
    static Config qualityAssuranceTemplate();
    static Config errorLogFixerTemplate();
    static Config chiefFinancialOfficerTemplate();

private:
    QString m_rootDirectory;
};

QString humanSchedule(const Schedule &schedule);
QString humanLastActive(qint64 timestampMs, const QDateTime &now = QDateTime::currentDateTime());
QDateTime nextScheduledInvocation(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
// Weekday numbers (1 = Monday ... 7 = Sunday) a calendar cadence fires on.
// Empty for cadences that are not anchored to weekdays.
QList<int> scheduleWeekdays(const Schedule &schedule);
QDateTime previousScheduledInvocation(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
// 0..1 countdown from the previous invocation toward the next one, so a card
// can draw how far through the current increment the teammate is.
double scheduleProgress(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
qint64 expectedRunIntervalSecs(const Config &config);
double runStaleness(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
bool ranMoreRecently(const Config &left, const Config &right);
enum class RunWindowState { Off, Fresh, DueSoon, Overdue };
constexpr int kMaxRunWindowHours = 24 * 30;
RunWindowState runWindowState(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
QString humanRunWindow(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
QString humanNextInvocation(
    const Config &config,
    const QDateTime &now = QDateTime::currentDateTime());
bool scheduleIsDue(const Config &config, const QDateTime &now = QDateTime::currentDateTime());
bool scheduleMatchesEvent(const Config &config, const QString &eventName);
// A continual teammate owns one long-lived conversation. It completes a
// bounded cycle, waits at an explicit human decision gate, then resumes the
// same session from that answer. Calendar cadences remain one-shot cron runs.
bool continualLoopIsEnabled(const Config &config);
QString continualLoopInstructions();
QString continualLoopDecisionPrompt();
QString normalizedTaskType(const QString &value);
QStringList knownTaskTypes();
QString taskTypeLabel(const QString &taskType);
// A briefing-only teammate reports repository state without changing it. A
// teammate with a linked shortcut uses the briefing run to complete that
// shortcut, so the run context must not contradict its implementation work.
QString dailyBriefingAssignment(const Config &config);
void setRecentAgentChoiceSource(std::function<AgentChoice()> source);
AgentChoice recentAgentChoice();
AgentChoice lowCostAgentChoice(const AgentChoice &choice);
AgentChoice agentChoiceFor(const Config &config);
// Restrict automatic fallback to provider messages that actually describe
// exhausted credits/quota, rather than transient transport rate limiting.
bool creditsAreExhausted(const QString &message);
// When a usage-limit refusal says the window reopens - Codex's "try again at
// Oct 9th, 2026 11:13 AM" / "try again in 2 hours", Claude's "resets 5pm
// (America/New_York)" - the epoch ms it names; 0 when it names no future time.
qint64 usageLimitResumeAtMs(const QString &message, qint64 nowMs);
bool usageLimitReached(const QString &message);
// A provider rejected the model itself (unsupported for this account, unknown,
// or no access), so every retry on it fails the same way. With a model id, the
// rejection must name it on the same line: the failure text includes the
// transcript tail, and an agent reading logs can quote these phrases.
bool modelIsUnavailable(const QString &message, const QString &model = QString());
// Read the one-line reflection requested at the end of a self-improving run.
// Only this labelled line is retained; the rest of the transcript stays in the
// session store.
QString improvementNoteFromSummary(const QString &summary);
QString runCaptionFromSummary(const QString &summary, int limit = 200);
// Add a successful run's lesson to the bounded learning journal.
// Configuration, permissions, and model routing are intentionally unchanged.
bool completeImprovementCycle(Config *config, const QString &summary);
bool teammateFromAgentOutput(const QString &output, Config *config,
                             QString *error = nullptr);
QString mentionHandle(const Config &config);
bool textMentionsTeammate(const QString &text, const Config &config);
// Explicit teammate-to-teammate hand-offs use "@Name: message" on one line.
// Ordinary prose mentions stay visible without accidentally starting work.
QString teammateMessage(const QString &text, const Config &config);
bool logMonitorMatches(const LogMonitor &monitor, const QString &badge,
                       int observedCount);
bool watchesAppLog(const Config &config);
QStringList knownPermissions();
QString permissionLabel(const QString &permission);

} // namespace forkmesh::teammates

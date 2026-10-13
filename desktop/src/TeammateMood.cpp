#include "TeammateMood.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QtGlobal>

#include <algorithm>
#include <array>

namespace forkmesh::teammates {

namespace {

using S = MoodSignals;

constexpr qint64 kMinuteMs = 60 * 1000;
constexpr qint64 kHourMs = 60 * kMinuteMs;
constexpr qint64 kDayMs = 24 * kHourMs;

struct Factor {
    const char *name;
    MoodKind mood;
    double weight;
    double (*strength)(const S &);
};

double on(bool value)
{
    return value ? 1.0 : 0.0;
}

double ramp(double value, double from, double to)
{
    if (to <= from)
        return on(value >= to);
    return qBound(0.0, (value - from) / (to - from), 1.0);
}

bool running(const S &s)
{
    return s.sessionStatus == QLatin1String("running");
}

bool idle(const S &s)
{
    return !s.inFlight && s.enabled;
}

qint64 idleMs(const S &s)
{
    if (s.lastRunAtMs <= 0 || s.nowMs <= 0)
        return 0;
    return qMax<qint64>(0, s.nowMs - s.lastRunAtMs);
}

bool triggerHas(const S &s, const char *word)
{
    return s.lastTrigger.contains(QLatin1String(word), Qt::CaseInsensitive);
}

bool errorHas(const S &s, const char *word)
{
    return s.lastError.contains(QLatin1String(word), Qt::CaseInsensitive);
}

double failureShare(const S &s)
{
    return s.recentRuns > 0 ? double(s.recentFailures) / s.recentRuns : 0.0;
}

const std::array<Factor, 100> &factors()
{
    static const std::array<Factor, 100> table{{
        {"is waiting for your reply", MoodKind::NeedsYou, 10.0,
         [](const S &s) { return on(s.sessionStatus == QLatin1String("waiting")); }},
        {"has a steer message queued", MoodKind::NeedsYou, 2.0,
         [](const S &s) { return on(s.pendingSteer); }},
        {"is held for approval", MoodKind::NeedsYou, 5.0,
         [](const S &s) { return on(s.held); }},
        {"finished a run you have not seen", MoodKind::Celebrating, 9.0,
         [](const S &s) { return on(s.doneUnseen); }},
        {"earned a top grade", MoodKind::Celebrating, 3.0,
         [](const S &s) { return on(s.doneUnseen && s.taskGrade >= 5); }},
        {"opened work for review", MoodKind::Celebrating, 2.0,
         [](const S &s) {
             return on(s.prNumber > 0 && s.sessionStatus == QLatin1String("success"));
         }},
        {"is on a three-run success streak", MoodKind::Proud, 3.0,
         [](const S &s) { return on(s.successStreak >= 3); }},
        {"keeps the streak going", MoodKind::Proud, 2.0,
         [](const S &s) { return ramp(s.successStreak, 4, 9); }},
        {"has a high average grade", MoodKind::Proud, 3.0,
         [](const S &s) { return on(s.gradedRuns > 0 && s.averageGrade >= 4.0); }},
        {"passed every recent run", MoodKind::Proud, 2.0,
         [](const S &s) { return on(s.recentRuns >= 4 && s.recentFailures == 0); }},
        {"has improved itself many times", MoodKind::Proud, 1.0,
         [](const S &s) { return on(s.improvementCycles >= 5); }},
        {"succeeded last time", MoodKind::Happy, 3.0,
         [](const S &s) {
             return on(!s.doneUnseen && !s.inFlight &&
                       s.sessionStatus == QLatin1String("success"));
         }},
        {"ran within its cadence", MoodKind::Happy, 2.0,
         [](const S &s) {
             return s.lastRunAtMs > 0 && idle(s) ? 1.0 - qBound(0.0, s.staleness, 1.0) : 0.0;
         }},
        {"is inside its run reminder", MoodKind::Happy, 2.0,
         [](const S &s) { return on(s.windowState == RunWindowState::Fresh); }},
        {"learned from its reflections", MoodKind::Happy, 1.0,
         [](const S &s) { return on(s.improvementNotes > 0); }},
        {"finished the last run quickly", MoodKind::Happy, 1.0,
         [](const S &s) {
             return on(s.sessionStatus == QLatin1String("success") && s.lastDurationMs > 0 &&
                       s.lastDurationMs < 5 * kMinuteMs);
         }},
        {"kept the last run cheap", MoodKind::Happy, 0.5,
         [](const S &s) {
             return on(s.sessionStatus == QLatin1String("success") && s.costUsd > 0.0 &&
                       s.costUsd < 0.25);
         }},
        {"has a schedule running", MoodKind::Calm, 1.5,
         [](const S &s) { return on(s.scheduleActive && s.enabled); }},
        {"is idle and fresh", MoodKind::Calm, 2.0,
         [](const S &s) { return on(idle(s) && s.lastRunAtMs > 0 && s.staleness < 0.3); }},
        {"is available", MoodKind::Calm, 1.0, [](const S &s) { return on(s.enabled); }},
        {"is working now", MoodKind::Focused, 6.0, [](const S &s) { return on(running(s)); }},
        {"has taken many turns", MoodKind::Determined, 2.0,
         [](const S &s) { return running(s) ? ramp(s.numTurns, 15, 40) : 0.0; }},
        {"has run over twenty minutes", MoodKind::Determined, 3.0,
         [](const S &s) { return on(running(s) && s.runningMs > 20 * kMinuteMs); }},
        {"has run over an hour", MoodKind::Overwhelmed, 3.0,
         [](const S &s) { return running(s) ? ramp(s.runningMs, kHourMs, 2 * kHourMs) : 0.0; }},
        {"has a busy background build", MoodKind::Determined, 2.0,
         [](const S &s) { return on(s.backgroundBusy); }},
        {"juggles several background processes", MoodKind::Overwhelmed, 2.0,
         [](const S &s) { return on(s.backgroundProcesses >= 3); }},
        {"spent over two dollars this run", MoodKind::Overwhelmed, 2.0,
         [](const S &s) { return on(s.inFlight && s.costUsd > 2.0); }},
        {"spent over ten dollars this run", MoodKind::Overwhelmed, 3.0,
         [](const S &s) { return on(s.inFlight && s.costUsd > 10.0); }},
        {"runs without asking", MoodKind::Determined, 1.0,
         [](const S &s) { return on(s.yolo && running(s)); }},
        {"runs in the cloud", MoodKind::Focused, 1.0,
         [](const S &s) { return on(s.cloud && s.inFlight); }},
        {"is queued to start", MoodKind::Thinking, 6.0,
         [](const S &s) { return on(s.sessionStatus == QLatin1String("queued")); }},
        {"started a moment ago", MoodKind::Thinking, 2.0,
         [](const S &s) { return on(running(s) && s.runningMs < kMinuteMs); }},
        {"has taken only a few turns", MoodKind::Thinking, 1.0,
         [](const S &s) { return on(running(s) && s.numTurns < 3); }},
        {"weighs a continual loop", MoodKind::Thinking, 1.5,
         [](const S &s) { return on(s.enabled && s.scheduleMode == kScheduleContinuous); }},
        {"listens for repository events", MoodKind::Curious, 1.0,
         [](const S &s) { return on(s.enabled && s.scheduleMode == kScheduleEvents); }},
        {"failed last time", MoodKind::Frustrated, 7.0,
         [](const S &s) { return on(!s.inFlight && s.sessionStatus == QLatin1String("failed")); }},
        {"failed twice in a row", MoodKind::Frustrated, 3.0,
         [](const S &s) { return on(s.failureStreak >= 2); }},
        {"failed three times in a row", MoodKind::Frustrated, 3.0,
         [](const S &s) { return on(s.failureStreak >= 3); }},
        {"hit a timeout", MoodKind::Worried, 2.0,
         [](const S &s) { return on(errorHas(s, "timeout") || errorHas(s, "timed out")); }},
        {"carries an error", MoodKind::Worried, 2.0,
         [](const S &s) { return on(!s.lastError.trimmed().isEmpty()); }},
        {"failed half of its recent runs", MoodKind::Worried, 3.0,
         [](const S &s) { return on(s.recentRuns >= 2 && failureShare(s) >= 0.5); }},
        {"failed some recent runs", MoodKind::Worried, 1.5,
         [](const S &s) { return on(s.recentFailures > 0); }},
        {"has a low average grade", MoodKind::Worried, 2.0,
         [](const S &s) { return on(s.gradedRuns > 0 && s.averageGrade < 2.5); }},
        {"was stopped last time", MoodKind::Worried, 1.5,
         [](const S &s) { return on(!s.inFlight && s.sessionStatus == QLatin1String("stopped")); }},
        {"was stopped several times", MoodKind::Worried, 1.0,
         [](const S &s) { return on(s.recentStopped >= 2); }},
        {"reached a usage limit", MoodKind::Stuck, 12.0,
         [](const S &s) { return on(usageLimitReached(s.lastError)); }},
        {"ran out of credits", MoodKind::Stuck, 12.0,
         [](const S &s) { return on(creditsAreExhausted(s.lastError)); }},
        {"found its model unavailable", MoodKind::Stuck, 11.0,
         [](const S &s) { return on(modelIsUnavailable(s.lastError)); }},
        {"cannot read repositories", MoodKind::Stuck, 5.0,
         [](const S &s) { return on(s.enabled && !s.canReadRepositories); }},
        {"is past its run reminder", MoodKind::MissingYou, 7.0,
         [](const S &s) { return on(idle(s) && s.windowState == RunWindowState::Overdue); }},
        {"is due soon by its reminder", MoodKind::Eager, 4.0,
         [](const S &s) { return on(idle(s) && s.windowState == RunWindowState::DueSoon); }},
        {"missed a whole scheduled run", MoodKind::MissingYou, 4.0,
         [](const S &s) { return on(idle(s) && s.scheduleActive && s.staleness >= 1.0); }},
        {"has never run", MoodKind::Curious, 4.0,
         [](const S &s) { return on(s.enabled && s.lastRunAtMs <= 0 && !s.inFlight); }},
        {"joined this week", MoodKind::Curious, 2.0,
         [](const S &s) {
             return on(s.createdAtMs > 0 && s.nowMs - s.createdAtMs < 7 * kDayMs);
         }},
        {"has only a few runs", MoodKind::Curious, 1.5,
         [](const S &s) { return on(s.recentRuns >= 1 && s.recentRuns <= 2); }},
        {"was edited today", MoodKind::Curious, 1.5,
         [](const S &s) {
             return on(s.updatedAtMs > 0 && s.nowMs - s.updatedAtMs < kDayMs);
         }},
        {"has many skills to use", MoodKind::Eager, 1.0,
         [](const S &s) { return on(s.skills >= 5); }},
        {"watches the logs", MoodKind::Curious, 1.0,
         [](const S &s) { return on(s.logMonitors > 0); }},
        {"runs again within the hour", MoodKind::Eager, 3.0,
         [](const S &s) {
             return on(idle(s) && s.nextRunInSecs >= 0 && s.nextRunInSecs < 3600);
         }},
        {"runs again within ten minutes", MoodKind::Eager, 2.0,
         [](const S &s) {
             return on(idle(s) && s.nextRunInSecs >= 0 && s.nextRunInSecs < 600);
         }},
        {"is drifting past its cadence", MoodKind::MissingYou, 2.0,
         [](const S &s) {
             return idle(s) && s.lastRunAtMs > 0 ? ramp(s.staleness, 0.5, 1.0) : 0.0;
         }},
        {"sat a week on demand", MoodKind::Bored, 3.0,
         [](const S &s) {
             return on(idle(s) && s.scheduleMode == kScheduleOnDemand &&
                       idleMs(s) > 7 * kDayMs);
         }},
        {"sat a day on demand", MoodKind::Bored, 1.5,
         [](const S &s) {
             return on(idle(s) && s.scheduleMode == kScheduleOnDemand && idleMs(s) > kDayMs);
         }},
        {"has been idle a month", MoodKind::Bored, 2.0,
         [](const S &s) { return on(idle(s) && idleMs(s) > 30 * kDayMs); }},
        {"has no schedule", MoodKind::Bored, 1.0,
         [](const S &s) { return on(idle(s) && !s.scheduleActive); }},
        {"has not run today", MoodKind::Bored, 1.0,
         [](const S &s) { return on(idle(s) && s.lastRunAtMs > 0 && s.runsToday == 0); }},
        {"is paused", MoodKind::Sleepy, 12.0, [](const S &s) { return on(!s.enabled); }},
        {"has its schedule paused", MoodKind::Sleepy, 2.0,
         [](const S &s) {
             return on(s.scheduleMode != kScheduleOnDemand && !s.scheduleMode.isEmpty() &&
                       !s.scheduleActive);
         }},
        {"is up in the small hours", MoodKind::Sleepy, 1.5,
         [](const S &s) { return on(!s.inFlight && s.hourOfDay >= 0 && s.hourOfDay < 6); }},
        {"is winding down for the night", MoodKind::Sleepy, 0.5,
         [](const S &s) { return on(!s.inFlight && s.hourOfDay >= 22); }},
        {"is resting over the weekend", MoodKind::Sleepy, 1.0,
         [](const S &s) { return on(!s.inFlight && s.weekday >= 6); }},
        {"is fresh in the morning", MoodKind::Eager, 1.0,
         [](const S &s) { return on(s.enabled && s.hourOfDay >= 7 && s.hourOfDay < 10); }},
        {"starts the week", MoodKind::Eager, 0.5,
         [](const S &s) { return on(s.enabled && s.weekday == 1); }},
        {"enjoys Friday afternoon", MoodKind::Happy, 1.0,
         [](const S &s) { return on(s.weekday == 5 && s.hourOfDay >= 15); }},
        {"is on a lunch break", MoodKind::Calm, 0.5,
         [](const S &s) { return on(!s.inFlight && s.hourOfDay == 12); }},
        {"sees the whole team busy", MoodKind::Overwhelmed, 1.0,
         [](const S &s) { return on(s.teamWorking >= qMax(3, s.teamSize / 2)); }},
        {"sees teammates failing", MoodKind::Worried, 1.0,
         [](const S &s) { return on(s.teamFailing >= 2); }},
        {"is the only one working", MoodKind::Focused, 1.0,
         [](const S &s) { return on(s.inFlight && s.teamWorking == 1); }},
        {"sees a quiet team", MoodKind::Bored, 1.0,
         [](const S &s) { return on(!s.inFlight && s.teamWorking == 0); }},
        {"watches a new special agent being found", MoodKind::Curious, 1.0,
         [](const S &s) { return on(s.creatorRunning); }},
        {"belongs to a large team", MoodKind::Calm, 0.5,
         [](const S &s) { return on(s.teamSize >= 8); }},
        {"posts updates to a room", MoodKind::Happy, 0.5,
         [](const S &s) { return on(s.hasRelayChannel); }},
        {"knows its purpose", MoodKind::Calm, 0.3,
         [](const S &s) { return on(s.hasDescription); }},
        {"has standing instructions", MoodKind::Calm, 0.3,
         [](const S &s) { return on(s.hasInstructions); }},
        {"has no instructions yet", MoodKind::Curious, 1.0,
         [](const S &s) { return on(!s.hasInstructions && !s.hasWorkflow); }},
        {"owns a native workflow", MoodKind::Determined, 0.5,
         [](const S &s) { return on(s.hasWorkflow); }},
        {"keeps a busy activity log", MoodKind::Happy, 0.5,
         [](const S &s) { return on(s.activityCount >= 10); }},
        {"ran three times today", MoodKind::Proud, 1.5,
         [](const S &s) { return on(s.runsToday >= 3); }},
        {"ran ten times this week", MoodKind::Determined, 1.0,
         [](const S &s) { return on(s.runsThisWeek >= 10); }},
        {"runs every hour", MoodKind::Eager, 0.5,
         [](const S &s) { return on(s.scheduleActive && s.scheduleMode == kScheduleHourly); }},
        {"keeps its daily slot", MoodKind::Happy, 0.5,
         [](const S &s) {
             return on(s.scheduleActive && s.scheduleMode == kScheduleDaily && s.staleness < 0.5);
         }},
        {"holds many permissions", MoodKind::Determined, 0.5,
         [](const S &s) { return on(s.permissions >= 6); }},
        {"holds few permissions", MoodKind::Calm, 0.5,
         [](const S &s) { return on(s.permissions > 0 && s.permissions <= 2); }},
        {"was woken by a repository event", MoodKind::Curious, 0.5,
         [](const S &s) {
             return on(triggerHas(s, "event") || triggerHas(s, "issue") ||
                       triggerHas(s, "pull"));
         }},
        {"was called by a mention", MoodKind::Happy, 1.0,
         [](const S &s) { return on(triggerHas(s, "mention")); }},
        {"was woken by error logs", MoodKind::Worried, 1.0,
         [](const S &s) { return on(triggerHas(s, "log") || triggerHas(s, "error")); }},
        {"is graded well over several runs", MoodKind::Proud, 0.5,
         [](const S &s) { return on(s.gradedRuns >= 3 && s.averageGrade >= 3.5); }},
        {"spent a lot recently", MoodKind::Worried, 1.0,
         [](const S &s) { return on(s.recentCostUsd > 20.0); }},
        {"usually takes a long time", MoodKind::Determined, 1.0,
         [](const S &s) { return on(s.averageDurationMs > 30 * kMinuteMs); }},
        {"usually finishes fast", MoodKind::Happy, 1.0,
         [](const S &s) {
             return on(s.averageDurationMs > 0 && s.averageDurationMs < 3 * kMinuteMs);
         }},
    }};
    return table;
}

} // namespace

int moodFactorCount()
{
    return static_cast<int>(factors().size());
}

QStringList moodFactorNames()
{
    QStringList names;
    names.reserve(moodFactorCount());
    for (const Factor &factor : factors())
        names << QString::fromUtf8(factor.name);
    return names;
}

QString moodMotionName(MoodMotion motion)
{
    switch (motion) {
    case MoodMotion::Still: return QStringLiteral("still");
    case MoodMotion::Wave: return QStringLiteral("wave");
    case MoodMotion::Jump: return QStringLiteral("jump");
    case MoodMotion::Bounce: return QStringLiteral("bounce");
    case MoodMotion::Nod: return QStringLiteral("nod");
    case MoodMotion::Pulse: return QStringLiteral("pulse");
    case MoodMotion::Float: return QStringLiteral("float");
    case MoodMotion::Wiggle: return QStringLiteral("wiggle");
    case MoodMotion::Shake: return QStringLiteral("shake");
    case MoodMotion::Spin: return QStringLiteral("spin");
    case MoodMotion::Doze: return QStringLiteral("doze");
    }
    return QStringLiteral("still");
}

QJsonObject moodJson(const Mood &mood)
{
    return {{QStringLiteral("key"), mood.key},
            {QStringLiteral("emoji"), mood.emoji},
            {QStringLiteral("label"), mood.label},
            {QStringLiteral("motion"), moodMotionName(mood.motion)},
            {QStringLiteral("attention"), mood.wantsAttention},
            {QStringLiteral("intensity"), mood.intensity},
            {QStringLiteral("reasons"), QJsonArray::fromStringList(mood.reasons)}};
}

Mood moodFor(MoodKind kind)
{
    Mood mood;
    mood.kind = kind;
    auto set = [&mood](const char *key, const QString &emoji, const char *label,
                       MoodMotion motion, bool attention) {
        mood.key = QString::fromLatin1(key);
        mood.emoji = emoji;
        mood.label = QString::fromLatin1(label);
        mood.motion = motion;
        mood.wantsAttention = attention;
    };
    switch (kind) {
    case MoodKind::Celebrating:
        set("celebrating", QStringLiteral("🥳"), "Celebrating", MoodMotion::Jump, true);
        break;
    case MoodKind::NeedsYou:
        set("needs-you", QStringLiteral("🙋"), "Needs you", MoodMotion::Wave, true);
        break;
    case MoodKind::MissingYou:
        set("missing-you", QStringLiteral("👋"), "Ready to run", MoodMotion::Wave, true);
        break;
    case MoodKind::Frustrated:
        set("frustrated", QStringLiteral("😣"), "Frustrated", MoodMotion::Shake, true);
        break;
    case MoodKind::Stuck:
        set("stuck", QStringLiteral("😵"), "Stuck", MoodMotion::Spin, true);
        break;
    case MoodKind::Worried:
        set("worried", QStringLiteral("😟"), "Worried", MoodMotion::Wiggle, false);
        break;
    case MoodKind::Overwhelmed:
        set("overwhelmed", QStringLiteral("🤯"), "Overwhelmed", MoodMotion::Shake, false);
        break;
    case MoodKind::Determined:
        set("determined", QStringLiteral("😤"), "Determined", MoodMotion::Pulse, false);
        break;
    case MoodKind::Focused:
        set("focused", QStringLiteral("🤓"), "Focused", MoodMotion::Nod, false);
        break;
    case MoodKind::Thinking:
        set("thinking", QStringLiteral("🤔"), "Thinking", MoodMotion::Float, false);
        break;
    case MoodKind::Eager:
        set("eager", QStringLiteral("🤩"), "Eager", MoodMotion::Bounce, false);
        break;
    case MoodKind::Proud:
        set("proud", QStringLiteral("😎"), "Proud", MoodMotion::Nod, false);
        break;
    case MoodKind::Happy:
        set("happy", QStringLiteral("😊"), "Happy", MoodMotion::Bounce, false);
        break;
    case MoodKind::Curious:
        set("curious", QStringLiteral("🧐"), "Curious", MoodMotion::Wiggle, false);
        break;
    case MoodKind::Calm:
        set("calm", QStringLiteral("🙂"), "Calm", MoodMotion::Float, false);
        break;
    case MoodKind::Bored:
        set("bored", QStringLiteral("🥱"), "Bored", MoodMotion::Doze, false);
        break;
    case MoodKind::Sleepy:
        set("sleepy", QStringLiteral("😴"), "Sleepy", MoodMotion::Doze, false);
        break;
    }
    return mood;
}

Mood assessMood(const MoodSignals &input)
{
    QHash<int, double> scores;
    QHash<int, QList<QPair<double, int>>> contributions;
    const auto &table = factors();
    for (int index = 0; index < static_cast<int>(table.size()); ++index) {
        const Factor &factor = table[index];
        const double value = factor.weight * qBound(0.0, factor.strength(input), 1.0);
        if (value <= 0.0)
            continue;
        const int mood = static_cast<int>(factor.mood);
        scores[mood] += value;
        contributions[mood].append({value, index});
    }
    int best = static_cast<int>(MoodKind::Calm);
    double bestScore = 0.0;
    for (int mood = 0; mood <= static_cast<int>(MoodKind::Sleepy); ++mood) {
        const double score = scores.value(mood);
        if (score > bestScore) {
            best = mood;
            bestScore = score;
        }
    }
    Mood mood = moodFor(static_cast<MoodKind>(best));
    mood.score = bestScore;
    mood.intensity = qBound(0.25, bestScore / 10.0, 1.0);
    QList<QPair<double, int>> reasons = contributions.value(best);
    std::stable_sort(reasons.begin(), reasons.end(),
                     [](const QPair<double, int> &left, const QPair<double, int> &right) {
                         return left.first > right.first;
                     });
    for (const auto &reason : std::as_const(reasons)) {
        if (mood.reasons.size() >= 3)
            break;
        mood.reasons << QString::fromUtf8(table[reason.second].name);
    }
    return mood;
}

} // namespace forkmesh::teammates

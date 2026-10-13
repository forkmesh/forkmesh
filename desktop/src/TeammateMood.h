#pragma once

#include "TeammateStore.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::teammates {

enum class MoodMotion { Still, Wave, Jump, Bounce, Nod, Pulse, Float, Wiggle, Shake, Spin, Doze };

enum class MoodKind {
    Celebrating,
    NeedsYou,
    MissingYou,
    Frustrated,
    Stuck,
    Worried,
    Overwhelmed,
    Determined,
    Focused,
    Thinking,
    Eager,
    Proud,
    Happy,
    Curious,
    Calm,
    Bored,
    Sleepy,
};

struct MoodSignals {
    bool enabled = true;
    bool scheduleActive = false;
    QString scheduleMode;
    RunWindowState windowState = RunWindowState::Off;
    double staleness = 0.0;
    qint64 nextRunInSecs = -1;
    int skills = 0;
    int permissions = 0;
    bool canReadRepositories = true;
    int logMonitors = 0;
    bool hasWorkflow = false;
    bool hasDescription = false;
    bool hasInstructions = false;
    bool hasRelayChannel = false;
    int improvementCycles = 0;
    int improvementNotes = 0;
    int activityCount = 0;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    qint64 lastRunAtMs = 0;
    QString lastTrigger;

    bool inFlight = false;
    bool doneUnseen = false;
    QString sessionStatus;
    qint64 runningMs = 0;
    qint64 lastDurationMs = 0;
    int numTurns = 0;
    double costUsd = 0.0;
    int backgroundProcesses = 0;
    bool backgroundBusy = false;
    bool held = false;
    bool cloud = false;
    bool yolo = false;
    bool pendingSteer = false;
    int prNumber = 0;
    int taskGrade = 0;
    QString lastError;

    int recentRuns = 0;
    int recentSuccesses = 0;
    int recentFailures = 0;
    int recentStopped = 0;
    int successStreak = 0;
    int failureStreak = 0;
    int gradedRuns = 0;
    double averageGrade = 0.0;
    double recentCostUsd = 0.0;
    qint64 averageDurationMs = 0;
    int runsToday = 0;
    int runsThisWeek = 0;

    qint64 nowMs = 0;
    int hourOfDay = 12;
    int weekday = 3;
    int teamSize = 1;
    int teamWorking = 0;
    int teamFailing = 0;
    bool creatorRunning = false;
};

struct Mood {
    MoodKind kind = MoodKind::Calm;
    QString key;
    QString emoji;
    QString label;
    MoodMotion motion = MoodMotion::Float;
    bool wantsAttention = false;
    double intensity = 0.0;
    double score = 0.0;
    QStringList reasons;
};

int moodFactorCount();
QStringList moodFactorNames();
Mood moodFor(MoodKind kind);
Mood assessMood(const MoodSignals &input);
QString moodMotionName(MoodMotion motion);
QJsonObject moodJson(const Mood &mood);

} // namespace forkmesh::teammates

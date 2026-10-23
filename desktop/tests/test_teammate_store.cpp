#include "TeammateStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>
#include <iostream>

using namespace forkmesh::teammates;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory is available");
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("teammates"));
    Store store(root);
    const Store emptyStore{QString()};
    check(emptyStore.pathFor(QStringLiteral("quality")).isEmpty() &&
              emptyStore.load().teammates.isEmpty(),
          "an empty store root never falls back to the process directory");

    const QString repositoryRoot =
        QDir(QStringLiteral(FORKMESH_SOURCE_DIR)).absoluteFilePath(
            QStringLiteral(".."));
    const LoadResult repositoryTeam =
        Store(QDir(repositoryRoot).filePath(
                  QStringLiteral(".forkmesh/teammates")))
            .load();
    const auto yCombinatorNewsAgent = std::find_if(
        repositoryTeam.teammates.cbegin(), repositoryTeam.teammates.cend(),
        [](const Config &candidate) {
            return candidate.id == QLatin1String("y-combinator-news-agent");
        });
    check(repositoryTeam.errors.isEmpty() &&
              yCombinatorNewsAgent != repositoryTeam.teammates.cend() &&
              continualLoopIsEnabled(*yCombinatorNewsAgent) &&
              yCombinatorNewsAgent->permissions.contains(
                  QStringLiteral("external-tools")) &&
              yCombinatorNewsAgent->instructions.contains(
                  QStringLiteral("generated or AI-edited comments")) &&
              QFileInfo::exists(QDir(repositoryRoot).filePath(
                  QStringLiteral(".forkmesh/shortcuts/%1")
                      .arg(yCombinatorNewsAgent->shortcut))),
          "the repository ships a valid continual Y Combinator News agent and launch assignment");

    Config quality = Store::qualityAssuranceTemplate();
    quality.relayChannel = QStringLiteral("#team-updates");
    check(quality.id == QLatin1String("quality") &&
              quality.specialty == QLatin1String("Quality Assurance") &&
              quality.shortcut == QLatin1String("check-bug-reports.md") &&
              quality.skills.contains(QStringLiteral("Discord QA")) &&
              quality.permissions.contains(QStringLiteral("external-tools")) &&
              quality.logMonitors.size() == 1 &&
              quality.logMonitors.first().threshold == 50,
          "the QA template is linked to Discord intake and usable tools");
    check(mentionHandle(quality) == QLatin1String("Quality") &&
              textMentionsTeammate(
                  QStringLiteral("Please ask @quality about this."), quality) &&
              !textMentionsTeammate(QStringLiteral("email@quality.dev"), quality) &&
              teammateMessage(
                  QStringLiteral("Done.\n@Quality: verify the release build"),
                  quality) == QLatin1String("verify the release build") &&
              teammateMessage(QStringLiteral("Quality helped with this."),
                              quality)
                  .isEmpty(),
          "one-word teammate handles distinguish mentions from explicit hand-offs");
    check(!logMonitorMatches(quality.logMonitors.first(),
                             QStringLiteral("STALL"), 50) &&
              logMonitorMatches(quality.logMonitors.first(),
                                QStringLiteral("stall"), 51),
          "QA starts its repair agent only after more than 50 stalls");
    check(creditsAreExhausted(QStringLiteral("insufficient_quota for this model")) &&
              creditsAreExhausted(
                  QStringLiteral("The credit balance is too low.")) &&
              creditsAreExhausted(
                  QStringLiteral("billing_error: account hard limit reached")) &&
              !creditsAreExhausted(
                  QStringLiteral("Temporary network rate limit; retry shortly.")),
          "only exhausted credits and quota count as exhausted credits");
    const QString codexRejection = QStringLiteral(
        "{\"type\":\"error\",\"status\":400,\"error\":{\"type\":"
        "\"invalid_request_error\",\"message\":\"The 'gpt-5.6-sol' model is "
        "not supported when using Codex with a ChatGPT account.\"}}");
    check(modelIsUnavailable(codexRejection, QStringLiteral("gpt-5.6-sol")) &&
              modelIsUnavailable(
                  QStringLiteral("There's an issue with the selected model "
                                 "(claude-foo). It may not exist or you may "
                                 "not have access to it."),
                  QStringLiteral("claude-foo")) &&
              !creditsAreExhausted(codexRejection),
          "a provider rejecting the model itself activates teammate fallback");
    check(!modelIsUnavailable(codexRejection, QStringLiteral("gpt-5.5")) &&
              !modelIsUnavailable(
                  QStringLiteral("Model metadata for `gpt-5.6-sol` not found. "
                                 "Defaulting to fallback metadata."),
                  QStringLiteral("gpt-5.6-sol")) &&
              !modelIsUnavailable(
                  QStringLiteral("grep: model is not supported\ngpt-5.6-sol ok"),
                  QStringLiteral("gpt-5.6-sol")),
          "a model rejection must name the session's model on the same line");
    check(!quality.schedule.active &&
              humanSchedule(quality.schedule).contains(
                  QStringLiteral("weekday"), Qt::CaseInsensitive),
          "the QA schedule is human readable and opt-in");
    check(quality.selfImproving && quality.improvementCycles == 0 &&
              quality.improvementNotes.isEmpty(),
          "new teammates start with a self-improvement journal");
    Config briefingOnly = quality;
    briefingOnly.shortcut.clear();
    const QString summaryBriefing = dailyBriefingAssignment(briefingOnly);
    const QString shortcutBriefing = dailyBriefingAssignment(quality);
    check(summaryBriefing.contains(QStringLiteral("do not edit files")) &&
              !summaryBriefing.contains(QStringLiteral("execution run")) &&
              summaryBriefing.contains(QStringLiteral("Main points:")) &&
              summaryBriefing.contains(QStringLiteral("Next-step choices:")) &&
              shortcutBriefing.contains(
                  QStringLiteral("Complete the linked shortcut")) &&
              shortcutBriefing.contains(
                  QStringLiteral("implement and verify")) &&
              shortcutBriefing.contains(QStringLiteral("stopping at a plan")) &&
              !shortcutBriefing.contains(QStringLiteral("do not edit files")),
          "daily briefings keep summaries read-only and linked shortcuts executable");
    const bool firstLessonRecorded = completeImprovementCycle(
        &quality,
        QStringLiteral("Work complete.\nImprovement note: Run the focused test "
                       "before broad validation.\nEvidence: passed."));
    check(firstLessonRecorded && quality.improvementCycles == 1 &&
              quality.improvementNotes ==
                  QStringList{QStringLiteral(
                      "Run the focused test before broad validation.")} &&
              improvementNoteFromSummary(
                  QStringLiteral("Improvement note: retain this lesson\nnext line")) ==
                  QLatin1String("retain this lesson") &&
              improvementNoteFromSummary(QStringLiteral("No reflection label"))
                  .isEmpty(),
          "a successful run retains only its labelled, one-line lesson");
    const QString labelledCaption = runCaptionFromSummary(
        QStringLiteral("## Work summary\n- Fixed the relay retry.\n"
                       "Caption: Fixed the relay retry and **verified** it on "
                       "`n1`.\nImprovement note: test first."));
    const QString derivedCaption = runCaptionFromSummary(
        QStringLiteral("## Summary\n\n- Reviewed [Discord intake](https://x.test) "
                       "and triaged 4 reports.\n```\nnoise\n```\n"
                       "Evidence:\n- @Quality: please check #12\n"
                       "Improvement note: keep it short."));
    QString longSummary;
    for (int i = 0; i < 30; ++i)
        longSummary += QStringLiteral("Checked node %1 health. ").arg(i);
    const QString longCaption = runCaptionFromSummary(longSummary);
    check(labelledCaption ==
                  QLatin1String("Fixed the relay retry and verified it on n1.") &&
              derivedCaption ==
                  QLatin1String("Reviewed Discord intake and triaged 4 reports.") &&
              longCaption.size() <= 200 && longCaption.size() > 100 &&
              longCaption.endsWith(QLatin1Char('.')) &&
              runCaptionFromSummary(QString()).isEmpty(),
          "a run caption prefers the labelled line and otherwise condenses the "
          "summary to about 200 plain characters");
    const bool repeatedLessonRecorded = completeImprovementCycle(
        &quality,
        QStringLiteral("Improvement note: run the focused test before broad validation."));
    const bool missingLessonRecorded = completeImprovementCycle(
        &quality, QStringLiteral("Work complete without a reflection."));
    check(!repeatedLessonRecorded && !missingLessonRecorded &&
              quality.improvementCycles == 1 &&
              quality.improvementNotes.size() == 1,
          "a run that adds no new lesson does not count as an improvement cycle");
    const bool secondLessonRecorded = completeImprovementCycle(
        &quality, QStringLiteral("Improvement note: Check the log tail first."));
    check(secondLessonRecorded && quality.improvementCycles == 2 &&
              quality.improvementNotes.size() == 2,
          "a new lesson advances the cycle");
    quality.selfImproving = false;
    const bool disabledLessonRecorded = completeImprovementCycle(
        &quality, QStringLiteral("Improvement note: this must not persist"));
    check(!disabledLessonRecorded && quality.improvementCycles == 2 &&
              quality.improvementNotes.size() == 2,
          "disabled self-improvement leaves the journal unchanged");
    quality.selfImproving = true;

    Config errorFixer = Store::errorLogFixerTemplate();
    check(errorFixer.id == QLatin1String("error-fixer") &&
              errorFixer.specialty == QLatin1String("Application Reliability") &&
              errorFixer.shortcut == QLatin1String("fix-error-logs.md") &&
              errorFixer.logMonitors.size() == 1 &&
              errorFixer.logMonitors.first().badge == QLatin1String("ERROR") &&
              errorFixer.logMonitors.first().threshold == 10 &&
              errorFixer.logMonitors.first().inclusive &&
              !logMonitorMatches(errorFixer.logMonitors.first(),
                                 QStringLiteral("ERROR"), 9) &&
              logMonitorMatches(errorFixer.logMonitors.first(),
                                QStringLiteral("error"), 10),
          "the error fixer starts on the tenth new error, not the eleventh");
    check(errorFixer.id == kErrorFixerId &&
              taskTypeLabel(kTaskTypeErrorLogs) ==
                  QLatin1String("Error-log repair"),
          "error-log repair has a dedicated task and owner");
    Config pausedMonitor = errorFixer;
    pausedMonitor.logMonitors.first().active = false;
    check(watchesAppLog(errorFixer) && !watchesAppLog(pausedMonitor) &&
              !watchesAppLog(Config{}),
          "only an active log monitor earns read access to the app log");

    LogMonitorRuntime monitorRuntime;
    for (int i = 1; i <= 9; ++i)
        monitorRuntime.observe(QStringLiteral("ERROR sample %1").arg(i),
                               1000 + i);
    check(!monitorRuntime.runIsDue(errorFixer.logMonitors.first(), 1300),
          "nine errors remain below the automatic repair boundary");
    monitorRuntime.observe(QStringLiteral("ERROR sample 10"), 1010);
    check(monitorRuntime.runIsDue(errorFixer.logMonitors.first(), 1300) &&
              monitorRuntime.reserveRun(1010) &&
              monitorRuntime.sessionId == -1 &&
              monitorRuntime.pendingCount == 0 &&
              monitorRuntime.inFlightCount == 10,
          "the tenth error is atomically reserved as one repair batch");
    check(monitorRuntime.attachSession(42),
          "the reserved repair batch records its exact agent session");
    monitorRuntime.observe(QStringLiteral("ERROR arrived during repair"), 1020);
    const LogMonitorRuntime persistedRuntime =
        LogMonitorRuntime::fromJson(monitorRuntime.toJson());
    check(persistedRuntime.sessionId == 42 &&
              persistedRuntime.inFlightCount == 10 &&
              persistedRuntime.pendingCount == 1,
          "the repair batch and errors arriving during it survive persistence");
    monitorRuntime.finishRun(true, 1100);
    check(monitorRuntime.lastSuccessfulRunAtMs == 1100 &&
              monitorRuntime.lastFixedThroughMs == 1010 &&
              monitorRuntime.pendingCount == 1 &&
              monitorRuntime.sessionId == 0,
          "success advances only through the dispatched errors and leaves newer errors queued");
    for (int i = 0; i < 9; ++i)
        monitorRuntime.observe(QStringLiteral("next ERROR %1").arg(i),
                               1200 + i);
    check(monitorRuntime.runIsDue(errorFixer.logMonitors.first(), 1300) &&
              monitorRuntime.reserveRun(1209) &&
              monitorRuntime.attachSession(43),
          "ten errors after the checkpoint open the next repair batch");
    monitorRuntime.observe(QStringLiteral("ERROR after failed dispatch"), 1210);
    monitorRuntime.finishRun(false, 1300);
    check(monitorRuntime.lastSuccessfulRunAtMs == 1100 &&
              monitorRuntime.lastFixedThroughMs == 1010 &&
              monitorRuntime.pendingCount == 11 &&
              monitorRuntime.runIsDue(errorFixer.logMonitors.first(), 1300),
          "a failed repair restores its batch without advancing the checkpoint");

    // A crash never passes through a badge count - the app died, so nothing in
    // it observed the fault - and one is enough to start a repair. The queue is
    // this same runtime, under the same "advance only on success" rule.
    LogMonitor crashTrigger;
    crashTrigger.badge = QStringLiteral("CRASH");
    crashTrigger.threshold = 1;
    crashTrigger.taskType = kTaskTypeErrorLogs;
    crashTrigger.active = true;
    crashTrigger.inclusive = true;
    LogMonitorRuntime crashRuntime;
    check(!crashRuntime.runIsDue(crashTrigger, 2400),
          "a launch that found no crash has no crash work");
    crashRuntime.observe(
        QStringLiteral("Crash: SIGSEGV (invalid memory access) at 2026-08-20T10:00:00"),
        2000);
    check(crashRuntime.runIsDue(crashTrigger, 2400) && crashRuntime.reserveRun(2000) &&
              crashRuntime.attachSession(51) && crashRuntime.inFlightCount == 1,
          "a single crash starts a repair immediately, with no threshold to reach");
    crashRuntime.observe(
        QStringLiteral("Crash: SIGABRT (abort) at 2026-08-20T10:05:00"), 2300);
    crashRuntime.finishRun(false, 2400);
    check(crashRuntime.lastSuccessfulRunAtMs == 0 &&
              crashRuntime.pendingCount == 2 &&
              crashRuntime.runIsDue(crashTrigger, 2400),
          "a crash stays open until a run actually repairs it");
    // The failed run does not go straight back out: fifteen dispatches of one
    // crash, thirteen of them into a spent Codex usage window, came from
    // re-sending a restored batch on the very next log line.
    crashRuntime.deferRetry(2400);
    constexpr qint64 kMinute = 60 * 1000;
    check(crashRuntime.failedRuns == 1 &&
              crashRuntime.retryNotBeforeMs == 2400 + 5 * kMinute &&
              crashRuntime.retryHeld(2401) &&
              !crashRuntime.runIsDue(crashTrigger, 2401) &&
              !crashRuntime.runIsDue(crashTrigger, 2400 + 5 * kMinute - 1) &&
              crashRuntime.runIsDue(crashTrigger, 2400 + 5 * kMinute),
          "a failed crash repair waits out a backoff before it is due again");
    const LogMonitorRuntime heldRuntime =
        LogMonitorRuntime::fromJson(crashRuntime.toJson());
    check(heldRuntime.failedRuns == 1 &&
              heldRuntime.retryNotBeforeMs == crashRuntime.retryNotBeforeMs &&
              !heldRuntime.runIsDue(crashTrigger, 2401),
          "the retry hold survives a restart instead of re-dispatching at launch");
    LogMonitorRuntime backoffRuntime = crashRuntime;
    backoffRuntime.deferRetry(10 * kMinute);
    check(backoffRuntime.retryNotBeforeMs == 20 * kMinute,
          "each consecutive failure doubles the hold");
    for (int i = 0; i < 12; ++i)
        backoffRuntime.deferRetry(10 * kMinute);
    check(backoffRuntime.retryNotBeforeMs == 10 * kMinute + 6 * 60 * kMinute,
          "the failure backoff is capped at six hours");
    LogMonitorRuntime limitRuntime = crashRuntime;
    limitRuntime.deferRetry(3000, 3000 + 90 * kMinute);
    check(limitRuntime.retryNotBeforeMs == 3000 + 91 * kMinute &&
              !limitRuntime.runIsDue(crashTrigger, 3000 + 90 * kMinute) &&
              limitRuntime.runIsDue(crashTrigger, 3000 + 91 * kMinute),
          "a usage-limit failure holds the crash until the window resets");
    limitRuntime.deferRetry(3000, 3000 + 16LL * 24 * 60 * kMinute);
    check(limitRuntime.retryNotBeforeMs == 3000 + 24 * 60 * kMinute,
          "a usage window days away is still retried daily");
    check(!limitRuntime.retryHeld(limitRuntime.retryNotBeforeMs -
                                  3 * 24 * 60 * kMinute),
          "a clock set back far past the hold does not park the queue for good");
    check(crashRuntime.runIsDue(crashTrigger, 2400 + 5 * kMinute) &&
              crashRuntime.reserveRun(2500) && crashRuntime.attachSession(52) &&
              crashRuntime.inFlightCount == 2,
          "the retry takes the crash that arrived during the failed run too");
    crashRuntime.finishRun(true, 2600);
    check(crashRuntime.lastSuccessfulRunAtMs == 2600 &&
              crashRuntime.lastFixedThroughMs == 2500 &&
              crashRuntime.pendingCount == 0 &&
              crashRuntime.failedRuns == 0 &&
              crashRuntime.retryNotBeforeMs == 0 &&
              !crashRuntime.runIsDue(crashTrigger, 2600),
          "a repaired crash queue is empty, clears its hold and starts nothing further");

    // The refusals session #396 actually recorded, read for when to retry.
    const QTimeZone newYork("America/New_York");
    const qint64 fourPmNewYork =
        QDateTime(QDate(2026, 9, 23), QTime(16, 0), newYork).toMSecsSinceEpoch();
    check(usageLimitResumeAtMs(
              QStringLiteral("You've hit your session limit \u00b7 resets 5pm "
                             "(America/New_York)"),
              fourPmNewYork) == fourPmNewYork + 60 * kMinute,
          "Claude's session-limit reset time is read in its named zone");
    check(usageLimitResumeAtMs(
              QStringLiteral("You've hit your session limit \u00b7 resets 3:30pm "
                             "(America/New_York)"),
              fourPmNewYork) == fourPmNewYork + (24 * 60 - 30) * kMinute,
          "a reset clock time already past today means tomorrow");
    const qint64 septemberNoon =
        QDateTime(QDate(2026, 9, 23), QTime(12, 0)).toMSecsSinceEpoch();
    check(usageLimitResumeAtMs(
              QStringLiteral("You've hit your usage limit. To continue using "
                             "Codex, start a free trial of Plus today, or try "
                             "again at Oct 9th, 2026 11:13 AM."),
              septemberNoon) ==
              QDateTime(QDate(2026, 10, 9), QTime(11, 13)).toMSecsSinceEpoch(),
          "Codex's dated try-again time is read in local time");
    check(usageLimitResumeAtMs(
              QStringLiteral("You've hit your usage limit. Try again at 5:32 PM."),
              septemberNoon) ==
              QDateTime(QDate(2026, 9, 23), QTime(17, 32)).toMSecsSinceEpoch(),
          "Codex's same-day try-again time is read in local time");
    check(usageLimitResumeAtMs(
              QStringLiteral("You've hit your usage limit. Try again in 2 days "
                             "3 hours 5 minutes."),
              septemberNoon) ==
              septemberNoon + ((2 * 24 + 3) * 60 + 5) * kMinute,
          "a relative try-again delay is added to the failure time");
    check(usageLimitResumeAtMs(QStringLiteral("the CLI hit an error while "
                                              "running the turn"),
                               septemberNoon) == 0 &&
              usageLimitResumeAtMs(QStringLiteral("resets 3 times at startup"),
                                   septemberNoon) == 0 &&
              usageLimitResumeAtMs(
                  QStringLiteral("You've hit your usage limit. Try again at "
                                 "Sep 1st, 2026 9:00 AM."),
                  septemberNoon) == 0,
          "no reset time is invented for other failures or a past window");
    check(usageLimitResumeAtMs(
              QStringLiteral("Claude AI usage limit reached|1800000000"),
              septemberNoon) == 1800000000LL * 1000,
          "Claude's epoch-stamped usage-limit refusal names its reset");
    check(usageLimitReached(QStringLiteral(
              "You've hit your session limit \u00b7 resets 5pm "
              "(America/New_York)")) &&
              usageLimitReached(QStringLiteral("You've hit your limit")) &&
              usageLimitReached(QStringLiteral(
                  "You've hit your usage limit. Try again at 5:32 PM.")) &&
              usageLimitReached(
                  QStringLiteral("Claude AI usage limit reached|1790000000")),
          "Claude and Codex usage-limit refusals are recognised");
    check(!usageLimitReached(QStringLiteral("the CLI hit an error while "
                                            "running the turn")) &&
              !usageLimitReached(
                  QStringLiteral("the run hit its maximum number of turns")) &&
              !usageLimitReached(QStringLiteral("429 rate limit, retrying")),
          "other failures are not read as a usage limit");

    Config cfo = Store::chiefFinancialOfficerTemplate();
    check(cfo.id == QLatin1String("cfo") &&
              cfo.avatar == QLatin1String("cfo") &&
              cfo.focus ==
                  QLatin1String("Revenue Strategy, Payouts & Reconciliation") &&
              cfo.description.contains(
                  QLatin1String("Continually identifies product revenue opportunities")) &&
              cfo.instructions.contains(
                  QLatin1String("Continually assess how the product can make money")) &&
              cfo.instructions.contains(
                  QLatin1String("never authorizes spending or payouts")) &&
              cfo.skills.contains(QStringLiteral("Revenue Strategy")) &&
              cfo.skills.contains(QStringLiteral("Monetization Analysis")) &&
              cfo.workflow == kWorkflowContributorPayouts &&
              cfo.permissions ==
                  QSet<QString>{QStringLiteral("read-repositories"),
                                QStringLiteral("process-payouts")},
          "the CFO template can read its repository and run only the native payout workflow");
    check(cfo.schedule.mode == kScheduleDaily &&
              cfo.schedule.time == QTime(10, 0) && !cfo.schedule.active,
          "the reusable CFO template is a guarded daily schedule");

    QString error;
    check(store.save(&quality, &error) && error.isEmpty(),
          "a teammate saves atomically");
    check(QFileInfo::exists(store.pathFor(QStringLiteral("quality"))),
          "the teammate receives a stable json path");
    const LoadResult loaded = store.load();
    check(loaded.errors.isEmpty() && loaded.teammates.size() == 1,
          "a saved teammate loads without warnings");
    if (!loaded.teammates.isEmpty()) {
        const Config roundTrip = loaded.teammates.first();
        check(roundTrip.name == quality.name &&
                  roundTrip.permissions == quality.permissions &&
                  roundTrip.skills == quality.skills &&
                  roundTrip.logMonitors.size() == 1 &&
                  roundTrip.workflow == quality.workflow &&
                  roundTrip.selfImproving &&
                  roundTrip.improvementCycles == 2 &&
                  roundTrip.improvementNotes.size() == 2 &&
                  roundTrip.relayChannel == QLatin1String("#team-updates"),
              "identity, monitors, workflow, learning, relay, permissions and skills survive a round trip");
        const QJsonObject saved = quality.toJson();
        check(!saved.contains(QStringLiteral("provider")) &&
                  !saved.contains(QStringLiteral("model")) &&
                  !saved.contains(QStringLiteral("mode")) &&
                  !saved.contains(QStringLiteral("backupModels")) &&
                  !saved.contains(QStringLiteral("taskAgents")),
              "a saved teammate prescribes no agent, model, mode, backups or task routes");
    }

    check(taskTypeLabel(kTaskTypeNewIssues) == QLatin1String("New issues"),
          "task types keep their labels");
    check(normalizedTaskType(QStringLiteral(" ../../UI Stalls!! ")) ==
              QLatin1String("ui-stalls"),
          "task type keys are normalized before persistence");

    check(store.save(&cfo, &error) && error.isEmpty(),
          "the native CFO teammate saves");
    check(store.save(&errorFixer, &error) && error.isEmpty(),
          "the automatic error fixer saves");
    const LoadResult withCfo = store.load();
    const auto cfoIt = std::find_if(
        withCfo.teammates.cbegin(), withCfo.teammates.cend(),
        [](const Config &candidate) { return candidate.id == QLatin1String("cfo"); });
    check(cfoIt != withCfo.teammates.cend() &&
              cfoIt->workflow == kWorkflowContributorPayouts,
          "the native workflow survives a teammate round trip");
    const auto errorFixerIt = std::find_if(
        withCfo.teammates.cbegin(), withCfo.teammates.cend(),
        [](const Config &candidate) {
            return candidate.id == QLatin1String("error-fixer");
        });
    check(errorFixerIt != withCfo.teammates.cend() &&
              errorFixerIt->logMonitors.size() == 1 &&
              errorFixerIt->logMonitors.first().inclusive,
          "the error fixer's exact trigger boundary survives a teammate round trip");

    check(Store::safeId(QStringLiteral(" ../../Quality Team!! ")) ==
              QLatin1String("quality-team") &&
              !store.pathFor(QStringLiteral("../../outside")).contains(
                  QStringLiteral("../")),
          "teammate ids cannot escape their store");
    QJsonObject encoded = quality.toJson();
    encoded.insert(QStringLiteral("shortcut"), QStringLiteral("../../secret.env"));
    QString parseError;
    const Config sanitized = Config::fromJson(encoded, &parseError);
    check(parseError.isEmpty() && sanitized.shortcut == QLatin1String("secret.env"),
          "linked shortcuts are reduced to a safe file name");
    encoded.insert(QStringLiteral("workflow"), QStringLiteral("send-anything"));
    Config rejectedWorkflow = Config::fromJson(encoded, &parseError);
    check(!parseError.isEmpty() && rejectedWorkflow.id.isEmpty(),
          "unknown native workflows are rejected rather than treated as prompts");
    encoded = quality.toJson();
    encoded.insert(QStringLiteral("provider"), QStringLiteral("codex"));
    encoded.insert(QStringLiteral("model"), QStringLiteral("gpt-5.6-sol"));
    encoded.insert(QStringLiteral("mode"), QStringLiteral("edit"));
    encoded.insert(QStringLiteral("backupModels"), QJsonArray{
        QJsonObject{{QStringLiteral("provider"), QStringLiteral("codex")},
                    {QStringLiteral("model"), QStringLiteral("gpt-5.6-terra")}},
    });
    encoded.insert(QStringLiteral("taskAgents"), QJsonArray{
        QJsonObject{{QStringLiteral("taskType"), QStringLiteral("scheduled")},
                    {QStringLiteral("provider"), QStringLiteral("codex")},
                    {QStringLiteral("model"), QStringLiteral("gpt-5.6-terra")}},
    });
    const Config legacyAgent = Config::fromJson(encoded, &parseError);
    check(parseError.isEmpty() && legacyAgent.id == quality.id &&
              !legacyAgent.toJson().contains(QStringLiteral("taskAgents")),
          "an older definition's agent settings are ignored, not rejected");
    encoded = quality.toJson();
    encoded.insert(QStringLiteral("relayChannel"),
                   QStringLiteral("office:private-room"));
    const Config sanitizedRelay = Config::fromJson(encoded, &parseError);
    check(parseError.isEmpty() && sanitizedRelay.relayChannel.isEmpty(),
          "teammate relays accept only explicit public mesh room names");

    Config scheduled = quality;
    scheduled.schedule.active = true;
    scheduled.schedule.mode = kScheduleWeekdays;
    scheduled.schedule.time = QTime(13, 0);
    scheduled.lastRunAtMs = 0;
    const QDateTime tuesday(QDate(2026, 8, 18), QTime(13, 30));
    check(scheduleIsDue(scheduled, tuesday),
          "an enabled weekday schedule becomes due after its time");
    const QDateTime beforeTuesdayRun(QDate(2026, 8, 18), QTime(12, 30));
    const QDateTime nextTuesday =
        nextScheduledInvocation(scheduled, beforeTuesdayRun);
    check(nextTuesday.date() == beforeTuesdayRun.date() &&
              nextTuesday.time() == QTime(13, 0) &&
              humanNextInvocation(scheduled, beforeTuesdayRun)
                  .contains(QStringLiteral("Today at")),
          "the next weekday invocation is exposed in human-readable form");
    scheduled.lastRunAtMs = tuesday.addSecs(-60).toMSecsSinceEpoch();
    check(!scheduleIsDue(scheduled, tuesday),
          "a schedule runs at most once on the same day");
    const QDateTime afterTuesdayRun = nextScheduledInvocation(scheduled, tuesday);
    check(afterTuesdayRun.date() == QDate(2026, 8, 19) &&
              afterTuesdayRun.time() == QTime(13, 0),
          "the next-invocation preview advances past a completed schedule day");

    scheduled.lastRunAtMs = 0;
    Schedule everyMonday;
    everyMonday.mode = kScheduleWeekly;
    everyMonday.days = {1};
    check(scheduleWeekdays(scheduled.schedule) == QList<int>({1, 2, 3, 4, 5}) &&
              scheduleWeekdays(everyMonday) == QList<int>({1}),
          "cadence dots light the weekdays a schedule actually fires on");
    // Wednesday 1 AM sits halfway between Tuesday and Wednesday at 1 PM.
    const QDateTime wednesdayEarly(QDate(2026, 8, 19), QTime(1, 0));
    const QDateTime beforeWednesday =
        previousScheduledInvocation(scheduled, wednesdayEarly);
    check(beforeWednesday.date() == QDate(2026, 8, 18) &&
              beforeWednesday.time() == QTime(13, 0) &&
              qAbs(scheduleProgress(scheduled, wednesdayEarly) - 0.5) < 0.01,
          "the countdown runs from the previous invocation to the next one");
    // Monday morning: the weekend gap is the increment, not a flat 24 hours.
    const QDateTime mondayMorning(QDate(2026, 8, 17), QTime(9, 0));
    check(previousScheduledInvocation(scheduled, mondayMorning).date() ==
              QDate(2026, 8, 14) &&
          qAbs(scheduleProgress(scheduled, mondayMorning) - 68.0 / 72.0) < 0.01,
          "a weekday countdown spans the weekend instead of a single day");
    Config unscheduled = scheduled;
    unscheduled.schedule.active = false;
    check(qFuzzyIsNull(scheduleProgress(unscheduled, wednesdayEarly)) &&
              !previousScheduledInvocation(unscheduled, wednesdayEarly).isValid(),
          "a paused schedule has no countdown to draw");
    {
        Config cadence = scheduled;
        check(expectedRunIntervalSecs(cadence) == 3 * 86400,
              "a weekday special agent is expected at least every three days");
        cadence.schedule.mode = kScheduleHourly;
        check(expectedRunIntervalSecs(cadence) == 3600,
              "an hourly special agent is expected every hour");
        cadence.schedule.mode = kScheduleWeekly;
        cadence.schedule.days = {3};
        check(expectedRunIntervalSecs(cadence) == 7 * 86400,
              "a weekly special agent is expected every week");
        cadence.schedule.mode = kScheduleCustom;
        cadence.schedule.days = {4, 1};
        check(expectedRunIntervalSecs(cadence) == 4 * 86400,
              "a custom cadence is expected within its widest gap");
        cadence.schedule.active = false;
        check(expectedRunIntervalSecs(cadence) == 7 * 86400,
              "a special agent without an active schedule is expected weekly");
        cadence.schedule.active = true;
        cadence.schedule.mode = kScheduleDaily;
        cadence.lastRunAtMs = 0;
        check(qFuzzyCompare(runStaleness(cadence, wednesdayEarly), 1.0),
              "a special agent that never ran is fully stale");
        cadence.lastRunAtMs = wednesdayEarly.toMSecsSinceEpoch();
        check(qFuzzyIsNull(runStaleness(cadence, wednesdayEarly)),
              "a special agent that just ran is fresh");
        check(qAbs(runStaleness(cadence, wednesdayEarly.addDays(1)) - 0.5) <
                  0.01,
              "a daily special agent is halfway stale when its next run is due");
        check(qFuzzyCompare(runStaleness(cadence, wednesdayEarly.addDays(3)),
                            1.0),
              "a special agent that missed its runs is fully stale");
        Config earlier = cadence;
        earlier.lastRunAtMs = wednesdayEarly.addDays(-1).toMSecsSinceEpoch();
        Config never = cadence;
        never.lastRunAtMs = 0;
        check(ranMoreRecently(cadence, earlier) &&
                  !ranMoreRecently(earlier, cadence),
              "the special agent that ran last sorts first");
        check(ranMoreRecently(earlier, never) && !ranMoreRecently(never, never),
              "a special agent that never ran sorts after every run");
    }
    {
        Config window = scheduled;
        window.runWindowHours = 0;
        window.lastRunAtMs = 0;
        check(runWindowState(window, wednesdayEarly) == RunWindowState::Off,
              "a special agent without a run reminder shows no dot");
        window.runWindowHours = 24;
        check(runWindowState(window, wednesdayEarly) == RunWindowState::Overdue,
              "a run reminder on a special agent that never ran is overdue");
        window.lastRunAtMs = wednesdayEarly.toMSecsSinceEpoch();
        check(runWindowState(window, wednesdayEarly.addSecs(3600)) ==
                  RunWindowState::Fresh,
              "a recent run keeps the reminder green");
        check(runWindowState(window, wednesdayEarly.addSecs(19 * 3600)) ==
                  RunWindowState::DueSoon,
              "the last six hours of a day-long reminder are due soon");
        check(runWindowState(window, wednesdayEarly.addSecs(24 * 3600)) ==
                  RunWindowState::Overdue,
              "a day without a run turns the reminder red");
        window.enabled = false;
        check(runWindowState(window, wednesdayEarly.addSecs(24 * 3600)) ==
                  RunWindowState::Off,
              "a paused special agent shows no reminder");
        window.enabled = true;
        check(Config::fromJson(window.toJson()).runWindowHours == 24,
              "the run reminder survives a save");
    }
    scheduled.schedule.active = false;
    scheduled.lastRunAtMs = 0;
    check(!scheduleIsDue(scheduled, tuesday),
          "a paused schedule never starts work");

    scheduled.schedule.active = true;
    scheduled.schedule.mode = kScheduleEvents;
    scheduled.schedule.events = {QStringLiteral("new-issues"),
                                 QStringLiteral("mentions")};
    check(scheduleMatchesEvent(scheduled, QStringLiteral("NEW-ISSUES")) &&
              !scheduleMatchesEvent(scheduled, QStringLiteral("new-pulls")) &&
              humanNextInvocation(scheduled, tuesday)
                  .contains(QStringLiteral("new issues")),
          "event schedules match only configured repository events");

    Config continual = quality;
    continual.schedule.active = true;
    continual.schedule.mode = kScheduleContinuous;
    continual.lastRunAtMs = tuesday.toMSecsSinceEpoch();
    const QString loopContract = continualLoopInstructions();
    const QString decisionPrompt = continualLoopDecisionPrompt();
    check(continualLoopIsEnabled(continual) &&
              scheduleIsDue(continual, tuesday) &&
              humanSchedule(continual.schedule).contains(
                  QStringLiteral("waits for your decision")) &&
              humanNextInvocation(continual, tuesday).contains(
                  QStringLiteral("Ready now")) &&
              loopContract.contains(QStringLiteral("yes/no")) &&
              loopContract.contains(QStringLiteral("exactly three")) &&
              loopContract.contains(QStringLiteral("free-form OK")) &&
              decisionPrompt.contains(QStringLiteral("interactive user-input")) &&
              decisionPrompt.contains(QStringLiteral("revised three-choice")),
          "continual mode stays immediately due and carries a human decision contract");
    continual.schedule.active = false;
    check(!continualLoopIsEnabled(continual) &&
              !scheduleIsDue(continual, tuesday),
          "pausing a continual teammate prevents another cycle");
    continual.schedule.active = true;
    continual.workflow = kWorkflowContributorPayouts;
    check(!continualLoopIsEnabled(continual) &&
              !scheduleIsDue(continual, tuesday),
          "native workflows do not enter the prompt-driven continual loop");

    Config bounded = Store::qualityAssuranceTemplate();
    bounded.id = QStringLiteral("bounded");
    bounded.name = QString(200, QLatin1Char('n'));
    bounded.instructions = QString(70 * 1024, QLatin1Char('i'));
    check(store.save(&bounded, &error) && bounded.name.size() == 80 &&
              bounded.instructions.size() == 64 * 1024,
          "saving normalizes oversized teammate fields before persistence");

    Config journalBounded = Store::qualityAssuranceTemplate();
    journalBounded.id = QStringLiteral("journal-bounded");
    for (int i = 0; i < 20; ++i) {
        completeImprovementCycle(
            &journalBounded,
            QStringLiteral("Improvement note: lesson %1 %2")
                .arg(i)
                .arg(QString(600, QLatin1Char('x'))));
    }
    check(store.save(&journalBounded, &error) &&
              journalBounded.improvementCycles == 20 &&
              journalBounded.improvementNotes.size() == 12 &&
              journalBounded.improvementNotes.first().size() == 500,
          "the evolving journal remains bounded across many runs");

    // load() answers from a memo of the folder's last parse, so the panels and
    // the mention sweep stop re-reading every file. It must never outlive what
    // is on disk: a save, a removal and a write from outside the app all show.
    {
        const int before = int(store.load().teammates.size());
        check(before > 0 && int(store.load().teammates.size()) == before,
              "loading an unchanged teammates folder twice gives the same team");
        Config renamed = quality;
        renamed.name = QStringLiteral("Quality Renamed");
        check(store.save(&renamed, &error), "a teammate saves over itself");
        const LoadResult afterSave = store.load();
        check(std::any_of(afterSave.teammates.cbegin(), afterSave.teammates.cend(),
                          [](const Config &c) {
                              return c.name == QLatin1String("Quality Renamed");
                          }),
              "a save shows in the very next load");
        QFile outside(store.pathFor(QStringLiteral("outside")));
        Config external = quality;
        external.id = QStringLiteral("outside");
        external.name = QStringLiteral("Outside");
        check(outside.open(QIODevice::WriteOnly) &&
                  outside.write(QJsonDocument(external.toJson()).toJson()) > 0,
              "a teammate file can be written from outside the store");
        outside.close();
        check(int(store.load().teammates.size()) == before + 1,
              "a file written from outside the app shows in the next load");
        check(store.remove(QStringLiteral("outside"), &error) &&
                  int(store.load().teammates.size()) == before,
              "a removal shows in the very next load");
        check(store.save(&quality, &error), "the teammate saves back");
    }

    check(store.remove(quality.id, &error) && !QFileInfo::exists(store.pathFor(quality.id)),
          "removing a teammate only removes its configuration");
    check(store.remove(cfo.id, &error),
          "the CFO teammate remains addressable by its stable id");
    check(store.remove(errorFixer.id, &error),
          "the error fixer remains addressable by its stable id");
    check(store.remove(bounded.id, &error),
          "the normalized teammate remains addressable by its stable id");
    check(store.remove(journalBounded.id, &error),
          "the bounded learning journal remains addressable by its stable id");
    {
        check(recentAgentChoice().provider.isEmpty(),
              "without a prompt settings source no agent is prescribed");
        setRecentAgentChoiceSource([] {
            return AgentChoice{QStringLiteral(" claude-code "),
                               QStringLiteral("claude-opus-5-5")};
        });
        check(recentAgentChoice().provider == QStringLiteral("claude-code") &&
                  recentAgentChoice().model ==
                      QStringLiteral("claude-opus-5-5"),
              "special agents run on the model selected in the prompt settings");
        Config cheap;
        cheap.id = QStringLiteral("genie");
        cheap.name = QStringLiteral("Genie");
        cheap.specialty = QStringLiteral("Genie Spider Coach");
        check(agentChoiceFor(cheap).model == QStringLiteral("claude-opus-5-5"),
              "a special agent without the low-cost flag keeps the prompt settings model");
        cheap.lowCostModel = true;
        check(agentChoiceFor(cheap).provider == QStringLiteral("claude-code") &&
                  agentChoiceFor(cheap).model == QStringLiteral("claude-haiku-4-5"),
              "a low-cost special agent runs Claude on Haiku 4.5");
        check(lowCostAgentChoice({QStringLiteral("codex"), QStringLiteral("gpt-5.5")}).model ==
                      QStringLiteral("gpt-5.6-luna") &&
                  lowCostAgentChoice({QStringLiteral("local-llama"), QStringLiteral("qwen")}).model ==
                      QStringLiteral("qwen"),
              "Codex drops to Luna and free local models stay as they are");
        check(Config::fromJson(cheap.toJson()).lowCostModel,
              "the low-cost flag round-trips");
        cheap.lowCostModel = false;
        check(!cheap.toJson().contains(QStringLiteral("lowCostModel")),
              "special agents without the flag do not write it");
        check(!cheap.toJson().contains(QStringLiteral("marketingChannels")),
              "special agents without marketing channels do not write the key");
        cheap.marketingChannels = {QStringLiteral(" Reddit "), QStringLiteral("hacker-news"),
                                   QStringLiteral("reddit"), QStringLiteral("not a channel"),
                                   QString()};
        check(Config::fromJson(cheap.toJson()).marketingChannels ==
                  QStringList({QStringLiteral("reddit"), QStringLiteral("hacker-news")}),
              "marketing channels round-trip as unique lower-case ids");
        setRecentAgentChoiceSource([] { return AgentChoice{}; });
        check(recentAgentChoice().provider.isEmpty(),
              "an empty prompt settings choice prescribes nothing");
        setRecentAgentChoiceSource({});
    }

    {
        const QString reply = QStringLiteral(
            "Releases keep breaking on Android.\n\n"
            "```json\n"
            "{\"name\": \"Ada\", \"specialty\": \"Release Engineering\", "
            "\"focus\": \"Ship safely\", \"workflow\": \"contributor-payouts\", "
            "\"schedule\": {\"mode\": \"daily\", \"time\": \"08:30\", "
            "\"active\": true}, \"permissions\": [\"modify-code\", \"merge\", "
            "\"process-payouts\", \"run-unattended\", \"run-git\", \"read-forkmesh-web\"], \"logMonitors\": [{\"badge\": \"ERROR\", "
            "\"threshold\": 10, \"taskType\": \"error-logs\"}], "
            "\"lastSessionId\": 42}\n"
            "```\n");
        Config created;
        QString parseError;
        check(teammateFromAgentOutput(reply, &created, &parseError),
              "a fenced teammate definition parses");
        check(created.id == QStringLiteral("ada") &&
                  created.name == QStringLiteral("Ada"),
              "a missing id is derived from the name");
        check(created.workflow.isEmpty() && created.lastSessionId == 0,
              "a generated teammate cannot select a native workflow or history");
        check(!created.schedule.active &&
                  created.schedule.mode == kScheduleDaily &&
                  created.logMonitors.size() == 1 &&
                  !created.logMonitors.first().active,
              "a generated teammate's schedule and monitors start paused");
        check(!created.permissions.contains(QStringLiteral("merge")) &&
                  !created.permissions.contains(
                      QStringLiteral("process-payouts")) &&
                  !created.permissions.contains(
                      QStringLiteral("run-unattended")) &&
                  !created.permissions.contains(QStringLiteral("run-git")) &&
                  !created.permissions.contains(QStringLiteral("read-forkmesh-web")) &&
                  created.permissions.contains(QStringLiteral("modify-code")) &&
                  created.permissions.contains(
                      QStringLiteral("read-repositories")),
              "a generated teammate drops merge, payout and unattended permissions");
        check(!teammateFromAgentOutput(QStringLiteral("No gap found."), &created,
                                       &parseError) &&
                  !parseError.isEmpty(),
              "a reply without JSON is refused with a reason");
        check(!teammateFromAgentOutput(
                  QStringLiteral("```json\n{\"name\": \"Nameless role\"}\n```"),
                  &created, &parseError),
              "a definition without a specialty is refused");
    }

    check(failures == 0, "all teammate store checks pass");
    if (failures == 0)
        std::cout << "teammate store tests passed\n";
    return failures == 0 ? 0 : 1;
}

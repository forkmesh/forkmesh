#include "PingFixes.h"

#include <iostream>

using namespace forkmesh::pingfixes;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static Context macOffline()
{
    Context context;
    context.kind = QStringLiteral("action");
    context.title = QStringLiteral("Action failed");
    context.body = QString::fromUtf8(
        "Native iOS ad hoc build on the local Mac \xC2\xB7 forkmesh/forkmesh\n"
        "Step: Find the build Mac\n"
        "Cause: ERROR: the build Mac is not on the local network (is it on?).");
    context.warning = true;
    context.runId = 41;
    context.runStatus = QStringLiteral("failed");
    context.repo = QStringLiteral("forkmesh/forkmesh");
    context.workflowPath = QStringLiteral(".forkmesh/ios-adhoc-local-mac.yml");
    context.workflowName = QStringLiteral("Native iOS ad hoc build on the local Mac");
    return context;
}

static Context testFailure()
{
    Context context = macOffline();
    context.body = QStringLiteral("Tests · forkmesh/forkmesh\nStep: pytest\n"
                                  "Cause: FAILED app/tests/test_x.py::test_y - AssertionError");
    context.workflowPath = QStringLiteral(".forkmesh/tests.yml");
    context.workflowName = QStringLiteral("Tests");
    return context;
}

static void testCauses()
{
    check(causeOf(macOffline().body) == Cause::HostOffline, "offline Mac is host-offline");
    check(causeOf(testFailure().body) == Cause::Test, "assertion is a test failure");
    check(causeOf(QStringLiteral("Cause: curl: (6) Could not resolve host: example.com")) ==
              Cause::Network,
          "dns failure is network");
    check(causeOf(QStringLiteral("Cause: bash: swift: command not found")) == Cause::MissingTool,
          "missing command is missing-tool");
    check(causeOf(QStringLiteral("Cause: write failed: No space left on device")) == Cause::Disk,
          "full disk is disk");
    check(causeOf(QStringLiteral("Cause: src/a.cpp:3: error: expected ';'")) == Cause::Build,
          "compiler error is build");
    check(causeOf(QString()) == Cause::None, "empty is none");
}

static void testOfflineMacOffersTurnOffFirst()
{
    const QList<Fix> fixes = candidates(macOffline());
    check(fixes.size() == 3, "failed run offers three fixes");
    check(!fixes.isEmpty() && fixes.first().id == QLatin1String(fix::kDisableWorkflow),
          "turning the action off leads for an offline host");
    check(!fixes.isEmpty() && fixes.first().label == QLatin1String("Turn off this action"),
          "the lead fix reads Turn off this action");
    const QList<Fix> code = candidates(testFailure());
    check(!code.isEmpty() && code.first().id == QLatin1String(fix::kAgentFix),
          "an agent leads for a failing test");
}

static void testDisabledWorkflowOffersEnable()
{
    Context context = macOffline();
    context.workflowDisabled = true;
    bool enable = false;
    bool disable = false;
    for (const Fix &fix : candidates(context)) {
        enable = enable || fix.id == QLatin1String(fix::kEnableWorkflow);
        disable = disable || fix.id == QLatin1String(fix::kDisableWorkflow);
    }
    check(enable && !disable, "a turned-off workflow offers turning it back on");
}

static void testEventsWithoutFixes()
{
    Context started;
    started.kind = QStringLiteral("action");
    started.title = QStringLiteral("Action started");
    started.runId = 3;
    started.runStatus = QStringLiteral("running");
    check(candidates(started).isEmpty(), "a started run has nothing to fix");
    Context approval = started;
    approval.title = QStringLiteral("Action waiting for approval");
    approval.runStatus = QStringLiteral("awaiting-approval");
    const QList<Fix> fixes = candidates(approval);
    check(fixes.size() == 2 && fixes.first().id == QLatin1String(fix::kApprove),
          "an approval ping offers approve first");
    Context agent;
    agent.kind = QStringLiteral("agent-done");
    agent.warning = true;
    agent.agentSession = 12;
    check(eventOf(agent) == Event::AgentFailed, "a failed agent ping is agent-failed");
    check(candidates(agent).size() == 1, "a failed agent offers a retry");
}

static void testLearningReordersFixes()
{
    Ledger ledger;
    Context context = testFailure();
    qint64 now = 1'000'000;
    for (int i = 0; i < 4; ++i) {
        ledger.offered(context, now);
        ledger.chosen(context, QString::fromLatin1(fix::kDisableWorkflow), now);
        now += 60'000;
    }
    const QList<Fix> fixes = ledger.ranked(context, now);
    check(!fixes.isEmpty() && fixes.first().id == QLatin1String(fix::kDisableWorkflow),
          "the fix the user keeps picking moves to the front");
    Context sibling = context;
    sibling.workflowPath = QStringLiteral(".forkmesh/other.yml");
    const QList<Fix> shared = ledger.ranked(sibling, now);
    double disableScore = 0;
    for (const Fix &fix : shared) {
        if (fix.id == QLatin1String(fix::kDisableWorkflow))
            disableScore = fix.score;
    }
    check(disableScore > 0.3, "what was learned carries over to the same kind of failure");
    check(!shared.isEmpty() && !shared.first().trusted, "trust never carries over to a sibling");
}

static void testTrustAndUndo()
{
    Ledger ledger;
    const Context context = macOffline();
    qint64 now = 5'000'000;
    Fix automatic;
    for (int i = 0; i < kTrustStreak; ++i) {
        check(!ledger.autoFix(context, now, &automatic), "not trusted before the streak");
        ledger.offered(context, now);
        ledger.chosen(context, QString::fromLatin1(fix::kDisableWorkflow), now);
        now += 1000;
    }
    check(ledger.autoFix(context, now, &automatic), "trusted after the streak");
    check(automatic.id == QLatin1String(fix::kDisableWorkflow), "the trusted fix turns it off");
    Context autoPing = context;
    autoPing.title = QStringLiteral("Action turned off automatically");
    autoPing.workflowDisabled = true;
    check(eventOf(autoPing) == Event::WorkflowAutoDisabled, "the auto ping has its own event");
    const QList<Fix> undo = candidates(autoPing);
    check(undo.size() == 1 && undo.first().id == QLatin1String(fix::kEnableWorkflow),
          "the auto ping offers undo");
    ledger.chosen(autoPing, QString::fromLatin1(fix::kEnableWorkflow), now);
    check(!ledger.autoFix(context, now, &automatic), "an undo withdraws trust");
    check(ledger.stats(signature(context), QString::fromLatin1(fix::kDisableWorkflow)).undone == 1,
          "the undo is recorded against the failure");
}

static void testDeferredOutcomes()
{
    Ledger ledger;
    const Context context = testFailure();
    qint64 now = 9'000'000;
    ledger.offered(context, now);
    ledger.chosen(context, QString::fromLatin1(fix::kAgentFix), now);
    Stats stats = ledger.stats(signature(context), QString::fromLatin1(fix::kAgentFix));
    check(stats.pendingSinceMs == now && stats.ok == 0, "an agent fix waits for its outcome");
    ledger.recurred(context, now + 60'000);
    stats = ledger.stats(signature(context), QString::fromLatin1(fix::kAgentFix));
    check(stats.failed == 1 && stats.pendingSinceMs == 0, "a recurrence marks the fix as failed");

    ledger.chosen(context, QString::fromLatin1(fix::kRerun), now);
    ledger.outcome(context, QString::fromLatin1(fix::kRerun), true, now + 1000);
    stats = ledger.stats(signature(context), QString::fromLatin1(fix::kRerun));
    check(stats.ok == 1 && stats.streak == 1, "a passing rerun counts as a success");

    ledger.chosen(context, QString::fromLatin1(fix::kAgentFix), now);
    ledger.recurred(context, now + kSettleMs + 1);
    stats = ledger.stats(signature(context), QString::fromLatin1(fix::kAgentFix));
    check(stats.ok == 1 && stats.failed == 1, "a fix that held for a day settles as a success");
}

static void testLedgerRoundTrip()
{
    Ledger ledger;
    const Context context = macOffline();
    ledger.offered(context, 100);
    ledger.chosen(context, QString::fromLatin1(fix::kDisableWorkflow), 200);
    Ledger copy;
    copy.load(ledger.toJson());
    const Stats stats = copy.stats(signature(context), QString::fromLatin1(fix::kDisableWorkflow));
    check(stats.offered == 1 && stats.chosen == 1 && stats.ok == 1 && stats.lastMs == 200,
          "the ledger survives a save and load");
    const QJsonObject json = fixJson(candidates(context).first(), true);
    check(json.value(QStringLiteral("primary")).toBool() &&
              json.value(QStringLiteral("id")).toString() == QLatin1String(fix::kDisableWorkflow),
          "the wire row names the primary fix");
}

int main()
{
    testCauses();
    testOfflineMacOffersTurnOffFirst();
    testDisabledWorkflowOffersEnable();
    testEventsWithoutFixes();
    testLearningReordersFixes();
    testTrustAndUndo();
    testDeferredOutcomes();
    testLedgerRoundTrip();
    if (failures)
        return 1;
    std::cout << "ping fixes tests passed\n";
    return 0;
}

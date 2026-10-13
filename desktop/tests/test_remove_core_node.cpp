#include "RemoveCoreNodeDialog.h"
#include "RemoveCoreNodeModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>

using namespace forkmesh::fleet;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

const char kPlan[] =
    R"({"name": "n4", "label": "fmnext-core-4", "provider": "vultr",)"
    R"( "instance_id": "cccccccc-0000-0000-0000-000000000004", "ip": "203.0.113.14",)"
    R"( "voter": true, "is_leader": true, "leader": "n4", "allowed": true, "resume": false,)"
    R"( "destroy_instance": false, "blocker": "",)"
    R"( "quorum": "rqlite: voters n1, n2, n3, n4; after removing n4: n1, n2, n3",)"
    R"( "voters_before": ["n1", "n2", "n3", "n4"], "voters_after": ["n1", "n2", "n3"],)"
    R"( "healthy_after": ["n1", "n2", "n3"], "warnings": ["n4 is the rqlite leader"],)"
    R"( "estimate_seconds": 394, "state": null,)"
    R"( "steps": [{"id": "preflight", "title": "Preflight", "estimate": 5},)"
    R"( {"id": "drain", "title": "Drain it from DNS", "estimate": 75},)"
    R"( {"id": "peers", "title": "Remaining cores drop its peer", "estimate": 240},)"
    R"( {"id": "commit", "title": "Commit", "estimate": 2}]})";

const char kRefused[] =
    R"({"name": "n3", "label": "fmnext-core-3", "provider": "vultr", "voter": true,)"
    R"( "allowed": false, "destroy_instance": false,)"
    R"( "blocker": "removing n3 would leave 2 core(s) (n1, n2); the fleet needs at least 3",)"
    R"( "steps": []})";

const char kFakeTool[] = R"PY(import json, os, sys, time
here = os.path.dirname(os.path.abspath(__file__))
command = sys.argv[1]
destroy = "--destroy-instance" in sys.argv
if command == "remove-core-plan":
    name = sys.argv[2]
    plan = json.load(open(os.path.join(here, "plan-%s.json" % name)))
    plan["destroy_instance"] = destroy
    if destroy:
        plan["steps"] = plan.get("steps", []) + [{"id": "destroy", "title": "Delete the cloud instance", "estimate": 30}]
    print("progress noise")
    print(json.dumps(plan))
    sys.exit(0)
with open(os.path.join(here, "args.log"), "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
name = sys.argv[2]
steps = json.load(open(os.path.join(here, "plan-%s.json" % name)))["steps"]
if destroy:
    steps = steps + [{"id": "destroy", "title": "Delete the cloud instance", "estimate": 30}]
fail_at = open(os.path.join(here, "fail_at")).read().strip() if os.path.exists(os.path.join(here, "fail_at")) else ""
def emit(**event):
    print("FMNEXT-REMOVE-CORE " + json.dumps(event), flush=True)
emit(event="start", name=name, execute=True, steps=steps)
for step in steps:
    emit(event="step", step=step["id"], status="running", title=step["title"])
    print("[execute] %s: working" % step["id"], flush=True)
    time.sleep(0.02)
    if step["id"] == fail_at:
        os.remove(os.path.join(here, "fail_at"))
        emit(event="step", step=step["id"], status="failed", error="boom")
        emit(event="result", status="failed", name=name, step=step["id"], error="boom", notes=["n4 is out of DNS and held"])
        print("next_fleet: step %s failed" % step["id"], file=sys.stderr)
        sys.exit(1)
    emit(event="step", step=step["id"], status="done", seconds=0.1)
emit(event="result", status="ok", name=name, summary="%s is out of the fleet; its instance is %s" % (name, "deleted" if destroy else "kept"), branch="fleet/remove-core-" + name, commit="0123456789abcdef", destroyed=destroy)
)PY";

struct FakeTool {
    QTemporaryDir dir;
    QString script;

    FakeTool()
    {
        QDir(dir.path()).mkpath(QStringLiteral("tools"));
        script = dir.filePath(QStringLiteral("tools/next_fleet.py"));
        write(QStringLiteral("tools/next_fleet.py"), QByteArray(kFakeTool));
        write(QStringLiteral("tools/plan-n4.json"), QByteArray(kPlan));
        write(QStringLiteral("tools/plan-n3.json"), QByteArray(kRefused));
    }
    void write(const QString &name, const QByteArray &data)
    {
        QFile file(dir.filePath(name));
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(data);
    }
    QString argsLog() const
    {
        QFile file(dir.filePath(QStringLiteral("tools/args.log")));
        file.open(QIODevice::ReadOnly);
        return QString::fromUtf8(file.readAll());
    }
};

template <typename T>
T *child(QWidget *dialog, const char *name)
{
    return dialog->findChild<T *>(QString::fromLatin1(name));
}

void testPlanParsing()
{
    const RemoveCorePlan plan = parseRemoveCorePlan(QByteArray("noise\n") + kPlan);
    check(plan.valid && plan.allowed, "plan parses after noise");
    check(plan.name == QLatin1String("n4") && plan.label == QLatin1String("fmnext-core-4"),
          "name and label");
    check(plan.isLeader && plan.voter, "leader and voter");
    check(plan.votersAfter == QStringList({"n1", "n2", "n3"}), "voters after");
    check(plan.steps.size() == 4 && plan.steps.first().id == QLatin1String("preflight"),
          "steps");
    const RemoveCorePlan refused = parseRemoveCorePlan(QByteArray(kRefused));
    check(refused.valid && !refused.allowed && refused.blocker.contains(QLatin1String("at least 3")),
          "refusal parsed");
    check(!parseRemoveCorePlan(QByteArray("garbage")).valid, "garbage is no plan");
}

void testBlockersAndConfirmation()
{
    const RemoveCorePlan plan = parseRemoveCorePlan(QByteArray(kPlan));
    check(removeCoreBlocker(plan, false, false, false, QStringLiteral("n4")).isEmpty(),
          "typed name and allowed plan can start");
    check(removeCoreBlocker(plan, false, false, false, QStringLiteral(" n4 ")).isEmpty(),
          "surrounding spaces are fine");
    check(removeCoreBlocker(plan, false, false, false, QStringLiteral("n")) ==
              QLatin1String("Type n4 to confirm."),
          "partial name blocks");
    check(!removeCoreBlocker(plan, false, false, false, QStringLiteral("N4")).isEmpty(),
          "confirmation is case sensitive");
    check(!removeCoreBlocker(plan, false, false, false, QString()).isEmpty(), "empty blocks");
    check(removeCoreBlocker(plan, true, false, false, QStringLiteral("n4")).startsWith(
              QLatin1String("Checking")),
          "loading blocks");
    check(removeCoreBlocker(plan, false, false, true, QStringLiteral("n4")).startsWith(
              QLatin1String("Checking")),
          "a plan for the other checkbox state blocks until reloaded");
    check(removeCoreBlocker(plan, false, true, false, QStringLiteral("n4")).contains(
              QLatin1String("already running")),
          "a running removal blocks");
    const RemoveCorePlan refused = parseRemoveCorePlan(QByteArray(kRefused));
    check(removeCoreBlocker(refused, false, false, false, QStringLiteral("n3"))
              .contains(QLatin1String("the fleet needs at least 3")),
          "quorum refusal blocks even with the name typed");
    check(!removeCoreBlocker(RemoveCorePlan(), false, false, false, QStringLiteral("n4")).isEmpty(),
          "no plan blocks");
    check(removeConfirmationMatches(QStringLiteral("n4"), QStringLiteral("n4")) &&
              !removeConfirmationMatches(QString(), QString()),
          "confirmation helper");
    check(coreCountBlocker(QStringLiteral("n3"), 3).contains(QLatin1String("breaks rqlite quorum")),
          "three cores: the table button is disabled");
    check(coreCountBlocker(QStringLiteral("n4"), 4).isEmpty(), "four cores: enabled");
    check(isRemovableCoreName(QStringLiteral("n12")) && !isRemovableCoreName(QStringLiteral("r1")) &&
              !isRemovableCoreName(QStringLiteral("a1")) &&
              !isRemovableCoreName(QStringLiteral("n123")),
          "only core names are removable");
}

void testArgumentsAndText()
{
    check(removeCoreArguments(QStringLiteral("s.py"), QStringLiteral("n4"), false) ==
              QStringList({"s.py", "remove-core", "n4", "--execute"}),
          "remove arguments without the instance");
    check(removeCoreArguments(QStringLiteral("s.py"), QStringLiteral("n4"), true).last() ==
              QLatin1String("--destroy-instance"),
          "destroy only when asked");
    check(removeCorePlanArguments(QStringLiteral("s.py"), QStringLiteral("n4"), false) ==
              QStringList({"s.py", "remove-core-plan", "n4", "--json"}),
          "plan arguments");
    check(removeCoreUnitName(QStringLiteral("20260930-120000")) ==
              QLatin1String("fmnext-remove-core-20260930-120000"),
          "unit name");
    const RemoveCorePlan plan = parseRemoveCorePlan(QByteArray(kPlan));
    const QString kept = removeCoreSummaryText(plan, false);
    check(kept.contains(QLatin1String("hand rqlite leadership")) &&
              kept.contains(QLatin1String("n1, n2, n3, n4 to n1, n2, n3")) &&
              kept.contains(QLatin1String("keep the Vultr instance fmnext-core-4")) &&
              !kept.contains(QLatin1String("DELETE")),
          "summary without destroy");
    check(removeCoreSummaryText(plan, true).contains(
              QLatin1String("DELETE the Vultr instance fmnext-core-4")),
          "summary with destroy");
    check(providerDisplayName(QStringLiteral("aws")) == QLatin1String("AWS") &&
              providerDisplayName(QString()) == QLatin1String("Vultr"),
          "provider names");
    AddCoreRun run;
    configureRemoveRun(run);
    run.consume(QStringLiteral("FMNEXT-ADD-CORE {\"event\": \"start\", \"name\": \"x\"}\n"));
    check(run.state() == AddCoreRun::State::Idle, "add-core lines are not removal events");
    run.consume(QStringLiteral("FMNEXT-REMOVE-CORE {\"event\": \"start\", \"name\": \"n4\", "
                               "\"steps\": [{\"id\": \"drain\", \"title\": \"Drain\"}]}\n"
                               "FMNEXT-REMOVE-CORE {\"event\": \"step\", \"step\": \"drain\", "
                               "\"status\": \"running\"}\n"));
    check(run.headline().startsWith(QLatin1String("Removing n4: step 1 of 1")), "removing headline");
    run.reset();
    run.consume(QStringLiteral("FMNEXT-REMOVE-CORE {\"event\": \"result\", \"status\": "
                               "\"failed\", \"name\": \"n4\", \"step\": \"drain\", \"error\": "
                               "\"x\"}\n"));
    check(run.headline().startsWith(QLatin1String("Removing n4 failed")),
          "reset keeps the removal prefix and verb");
}

void testDialogFlow()
{
    FakeTool tool;
    tool.write(QStringLiteral("tools/fail_at"), "peers");
    RemoveCoreNodeController controller(tool.script);
    check(controller.isAvailable(), "fake tool available");
    auto *dialog = new RemoveCoreNodeDialog(&controller, QStringLiteral("n4"));
    dialog->show();
    check(dialog->page() == RemoveCoreNodeDialog::Page::Plan, "opens on the plan");
    check(waitFor([&] { return controller.plan().valid && !controller.planLoading(); }),
          "plan loads");
    QCoreApplication::processEvents();
    auto *start = child<QPushButton>(dialog, "removeCoreStart");
    auto *confirm = child<QLineEdit>(dialog, "removeCoreConfirm");
    auto *destroy = child<QCheckBox>(dialog, "removeCoreDestroy");
    auto *blocker = child<QLabel>(dialog, "removeCoreBlocker");
    check(start && confirm && destroy && blocker, "widgets present");
    check(!start->isEnabled(), "start disabled before typing");
    check(blocker->text() == QLatin1String("Type n4 to confirm."), "blocker asks for the name");
    check(child<QLabel>(dialog, "removeCorePlanText")->text().contains(
              QLatin1String("keep the Vultr instance")),
          "plan text shown");
    check(child<QLabel>(dialog, "removeCoreQuorum")->text().contains(QLatin1String("after removing n4")),
          "quorum shown");
    confirm->setText(QStringLiteral("n3"));
    check(!start->isEnabled(), "wrong name keeps start disabled");
    confirm->setText(QStringLiteral("n4"));
    check(start->isEnabled(), "typed name enables start");
    destroy->setChecked(true);
    check(!start->isEnabled(), "toggling the box reloads the plan first");
    check(waitFor([&] { return controller.plan().destroyInstance && !controller.planLoading(); }),
          "destroy plan loads");
    QCoreApplication::processEvents();
    check(start->isEnabled(), "enabled again with the destroy plan");
    check(start->text() == QLatin1String("Remove n4 and delete its instance"), "button says delete");
    check(child<QLabel>(dialog, "removeCorePlanText")->text().contains(QLatin1String("DELETE")),
          "destroy spelled out");
    destroy->setChecked(false);
    check(waitFor([&] { return !controller.plan().destroyInstance && !controller.planLoading(); }),
          "plan without destroy loads");
    QCoreApplication::processEvents();
    bool finishedSeen = false;
    bool finishedOk = true;
    QObject::connect(&controller, &RemoveCoreNodeController::finished, [&](bool ok) {
        finishedSeen = true;
        finishedOk = ok;
    });
    start->click();
    check(dialog->page() == RemoveCoreNodeDialog::Page::Progress, "progress page");
    check(waitFor([&] { return finishedSeen; }), "failed run finishes");
    QCoreApplication::processEvents();
    check(!finishedOk && controller.run().state() == AddCoreRun::State::Failed, "run failed");
    check(tool.argsLog().trimmed() == QLatin1String("remove-core n4 --execute"),
          "no --destroy-instance without the box");
    check(child<QPushButton>(dialog, "removeCoreRetry")->isVisible(), "retry shown");
    check(child<QLabel>(dialog, "removeCoreHeadline")->text().startsWith(
              QLatin1String("Removing n4 failed")),
          "failure headline");
    check(child<QLabel>(dialog, "removeCoreNotes")->text().contains(QLatin1String("out of DNS")),
          "notes shown");
    auto *steps = child<QListWidget>(dialog, "removeCoreSteps");
    check(steps && steps->count() == 4, "steps listed");
    const QString log = child<QPlainTextEdit>(dialog, "removeCoreLog")->toPlainText();
    check(log.contains(QLatin1String("[execute] peers: working")) &&
              !log.contains(QLatin1String("FMNEXT-REMOVE-CORE")),
          "log without events");
    dialog->close();
    QCoreApplication::processEvents();
    auto *reopened = new RemoveCoreNodeDialog(&controller, QStringLiteral("n4"));
    reopened->show();
    check(reopened->page() == RemoveCoreNodeDialog::Page::Progress, "reopened on progress");
    finishedSeen = false;
    child<QPushButton>(reopened, "removeCoreRetry")->click();
    check(waitFor([&] { return finishedSeen; }), "retry finishes");
    QCoreApplication::processEvents();
    check(finishedOk && controller.run().state() == AddCoreRun::State::Succeeded, "retry succeeds");
    check(controller.lastArguments().mid(1) == QStringList({"remove-core", "n4", "--execute"}),
          "retry repeats the same command");
    check(child<QLabel>(reopened, "removeCoreHeadline")->text().contains(
              QLatin1String("fleet/remove-core-n4 (0123456789ab)")),
          "success names the branch");
    reopened->close();
    QCoreApplication::processEvents();
}

void testDestroyFlagReachesTheTool()
{
    FakeTool tool;
    RemoveCoreNodeController controller(tool.script);
    auto *dialog = new RemoveCoreNodeDialog(&controller, QStringLiteral("n4"));
    dialog->show();
    check(waitFor([&] { return controller.plan().valid && !controller.planLoading(); }),
          "plan loads for destroy");
    child<QCheckBox>(dialog, "removeCoreDestroy")->setChecked(true);
    check(waitFor([&] { return controller.plan().destroyInstance && !controller.planLoading(); }),
          "destroy plan loads for destroy");
    QCoreApplication::processEvents();
    child<QLineEdit>(dialog, "removeCoreConfirm")->setText(QStringLiteral("n4"));
    bool finishedSeen = false;
    QObject::connect(&controller, &RemoveCoreNodeController::finished,
                     [&](bool) { finishedSeen = true; });
    child<QPushButton>(dialog, "removeCoreStart")->click();
    check(waitFor([&] { return finishedSeen; }), "destroy run finishes");
    check(tool.argsLog().trimmed() == QLatin1String("remove-core n4 --execute --destroy-instance"),
          "the box adds --destroy-instance");
    check(controller.run().steps().last().id == QLatin1String("destroy"), "destroy step shown");
    dialog->close();
    QCoreApplication::processEvents();
}

void testRefusedPlanDisablesEverything()
{
    FakeTool tool;
    RemoveCoreNodeController controller(tool.script);
    auto *dialog = new RemoveCoreNodeDialog(&controller, QStringLiteral("n3"));
    dialog->show();
    check(waitFor([&] { return controller.plan().valid && !controller.planLoading(); }),
          "refused plan loads");
    QCoreApplication::processEvents();
    auto *confirm = child<QLineEdit>(dialog, "removeCoreConfirm");
    confirm->setText(QStringLiteral("n3"));
    check(!child<QPushButton>(dialog, "removeCoreStart")->isEnabled(), "refused: start disabled");
    check(!confirm->isEnabled(), "refused: confirmation disabled");
    check(child<QLabel>(dialog, "removeCoreBlocker")->text().contains(
              QLatin1String("the fleet needs at least 3")),
          "refused: the reason is shown");
    check(tool.argsLog().isEmpty(), "refused: nothing ran");
    dialog->close();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testPlanParsing();
    testBlockersAndConfirmation();
    testArgumentsAndText();
    testDialogFlow();
    testDestroyFlagReachesTheTool();
    testRefusedPlanDisablesEverything();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS remove-core-node\n");
    return 0;
}

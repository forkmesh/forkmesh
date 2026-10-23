#include "AddCoreNodeDialog.h"
#include "AddCoreNodeModel.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
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

const char kOptions[] =
    R"({"estimate_seconds": 847, "name": "n4", "plan": "vc2-1c-1gb", "region": "ewr",)"
    R"( "rev": "e416743eef92", "voter": false,)"
    R"( "voter_reason": "3 voters is odd: the new core joins as a non-voter",)"
    R"( "regions": ["ewr", "sjc"],)"
    R"( "plans": [{"id": "vc2-1c-1gb", "ram": 1024, "vcpu_count": 1, "monthly_cost": 5,)"
    R"( "locations": ["ewr", "sjc"]}, {"id": "vc2-2c-4gb", "ram": 4096, "vcpu_count": 2,)"
    R"( "monthly_cost": 20, "locations": ["ewr"]}],)"
    R"( "steps": [{"id": "plan", "title": "Plan", "estimate": 2},)"
    R"( {"id": "vm", "title": "Create the Vultr VM", "estimate": 120},)"
    R"( {"id": "mesh", "title": "Mesh", "estimate": 300},)"
    R"( {"id": "dns", "title": "Add it to DNS", "estimate": 15}]})";

const char kFakeTool[] = R"PY(import json, os, sys, time
here = os.path.dirname(os.path.abspath(__file__))
state = os.path.join(here, "state.json")
command = sys.argv[1]
replaces = sys.argv[sys.argv.index("--replace") + 1] if "--replace" in sys.argv else ""
if command == "add-core-options":
    aws = os.path.join(here, "options_aws.json")
    chosen = aws if "aws" in sys.argv and os.path.exists(aws) else os.path.join(here, "options.json")
    data = json.load(open(chosen))
    if replaces:
        data["replaces"] = replaces
        data["steps"].append({"id": "remove", "title": "Take the replaced core out", "estimate": 520})
    print(json.dumps(data))
    sys.exit(0)
if command == "add-core-status":
    print(open(state).read() if os.path.exists(state) else "null")
    sys.exit(0)
with open(os.path.join(here, "args.log"), "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
steps = json.load(open(os.path.join(here, "options.json")))["steps"]
if replaces:
    steps.append({"id": "remove", "title": "Take the replaced core out", "estimate": 520})
fail_at = open(os.path.join(here, "fail_at")).read().strip() if os.path.exists(os.path.join(here, "fail_at")) else ""
slow = os.path.exists(os.path.join(here, "slow"))
resume = "--name" in sys.argv
def emit(**event):
    line = "FMNEXT-ADD-CORE " + json.dumps(event)
    sys.stdout.write(line[:20]); sys.stdout.flush(); time.sleep(0.02)
    sys.stdout.write(line[20:] + "\n"); sys.stdout.flush()
emit(event="start", name="n4", execute=True, steps=steps, replaces=replaces)
done = {}
for step in steps:
    if resume and step["id"] != fail_at and step["id"] in ("plan", "vm"):
        emit(event="step", step=step["id"], status="done", resumed=True)
        continue
    emit(event="step", step=step["id"], status="running", title=step["title"])
    print("[execute] %s: working" % step["id"], flush=True)
    if slow:
        time.sleep(0.4)
    if step["id"] == fail_at and not resume:
        emit(event="step", step=step["id"], status="failed", error="n1 did not come back")
        emit(event="result", status="failed", name="n4", step=step["id"],
             error="n1 did not come back", notes=["n4 is out of DNS and held"])
        json.dump({"name": "n4", "failed": {"step": step["id"], "error": "n1 did not come back"},
                   "steps": dict(done, **{step["id"]: "failed"})}, open(state, "w"))
        print("next_fleet: n4: step %s failed" % step["id"], file=sys.stderr, flush=True)
        sys.exit(1)
    done[step["id"]] = "done"
    emit(event="step", step=step["id"], status="done", seconds=1.5)
emit(event="result", status="ok", name="n4", summary="n4 is serving, in DNS",
     branch="fleet/add-core-n4", commit="0123456789abcdef0123")
if os.path.exists(state):
    os.remove(state)
sys.stdout.write("n4 is serving, in DNS")
sys.stdout.flush()
)PY";

void testOptionsParsingAndFormatting()
{
    const AddCoreOptions options = parseAddCoreOptions(QByteArray(kOptions));
    check(options.valid, "options parse");
    check(options.name == QLatin1String("n4") && options.region == QLatin1String("ewr"),
          "options name and region");
    check(!options.voter && options.voterReason.contains(QLatin1String("non-voter")),
          "options voter choice");
    check(options.plans.size() == 2 && options.regions == QStringList({"ewr", "sjc"}),
          "options plans and regions");
    check(plansInRegion(options, QStringLiteral("sjc")).size() == 1, "plans filtered by region");
    check(plansInRegion(options, QStringLiteral("EWR")).size() == 2, "region match ignores case");
    const AddCorePlan *plan = findPlan(options, QStringLiteral("vc2-2c-4gb"));
    check(plan && plan->monthlyCost == 20.0, "findPlan cost");
    check(!findPlan(options, QStringLiteral("nope")), "findPlan unknown");
    check(formatMonthlyCost(5) == QLatin1String("$5.00/month"), "cost format");
    check(formatMonthlyCost(0) == QLatin1String("price unknown"), "unknown cost");
    check(formatDuration(847) == QLatin1String("about 14 min"), "duration minutes");
    check(formatDuration(45) == QLatin1String("45 s"), "duration seconds");
    check(planLabel(*plan) == QString::fromUtf8("vc2-2c-4gb \xc2\xb7 2 vCPU \xc2\xb7 4 GB "
                                                 "\xc2\xb7 $20.00/month"),
          "plan label");
    const QString confirm = confirmationText(options, QStringLiteral("ewr"),
                                             QStringLiteral("vc2-1c-1gb"));
    check(confirm.contains(QLatin1String("fmnext-core-4 in ewr (vc2-1c-1gb, $5.00/month)")),
          "confirmation names the VM and cost");
    check(confirm.contains(QLatin1String("billed")) && confirm.contains(QLatin1String("non-voter")),
          "confirmation mentions billing and suffrage");
    check(confirm.contains(QLatin1String("into DNS")) &&
              confirm.contains(QLatin1String("fleet/add-core-n4")),
          "confirmation mentions DNS and the branch");
    check(confirm.contains(QLatin1String("about 14 min")), "confirmation time");
    const AddCoreOptions broken = parseAddCoreOptions("Traceback: boom");
    check(!broken.valid && !broken.error.isEmpty(), "garbage options are an error");
    const AddCoreOptions unfinished = parseAddCoreOptions(
        R"({"name": "n5", "region": "ewr", "unfinished": {"name": "n4", "failed": {"step": "mesh", "error": "x"}}})");
    check(unfinished.unfinishedName == QLatin1String("n4") &&
              unfinished.unfinishedFailedStep == QLatin1String("mesh"),
          "unfinished run parsed");
    check(unfinished.regions == QStringList({"ewr"}), "default region always listed");
    check(unfinished.replaces.isEmpty() && unfinished.unfinishedReplaces.isEmpty(),
          "a plain add replaces nothing");
    const AddCoreOptions replacing = parseAddCoreOptions(
        R"({"name": "n5", "region": "ewr", "replaces": "n2", "voter_reason": "x",)"
        R"( "estimate_seconds": 1373, "unfinished": {"name": "n5", "replaces": "n2"}})");
    check(replacing.replaces == QLatin1String("n2") &&
              replacing.unfinishedReplaces == QLatin1String("n2"),
          "replace target parsed");
    const QString replaceConfirm = confirmationText(replacing, QStringLiteral("ewr"),
                                                    QStringLiteral("vc2-1c-1gb"));
    check(replaceConfirm.contains(QLatin1String("then take n2 out of the fleet")) &&
              replaceConfirm.contains(QLatin1String("instance is kept")) &&
              replaceConfirm.contains(QLatin1String("with n5 added and n2 removed")),
          "confirmation describes the removal and the single branch");
    check(replaceConfirm.contains(QLatin1String("a failure while removing n2 leaves n5 serving")),
          "confirmation says a failed removal keeps the new core");
}

void testArgumentsAndHostGuard()
{
    const QStringList fresh = addCoreArguments(QStringLiteral("/t/next_fleet.py"),
                                               QStringLiteral("ewr"),
                                               QStringLiteral("vc2-1c-1gb"),
                                               QStringLiteral("n4"), false);
    check(fresh == QStringList({"/t/next_fleet.py", "add-core", "--execute", "--region", "ewr",
                                "--plan", "vc2-1c-1gb"}),
          "fresh arguments");
    const QStringList resume = addCoreArguments(QStringLiteral("/t/next_fleet.py"),
                                                QStringLiteral("ewr"), QStringLiteral("x"),
                                                QStringLiteral("n4"), true);
    check(resume == QStringList({"/t/next_fleet.py", "add-core", "--execute", "--name", "n4"}),
          "resume arguments carry only the name");
    check(addCoreArguments(QStringLiteral("/t/x"), QStringLiteral("ewr"), QString(), QString(),
                           false, QStringLiteral("vultr"), QStringLiteral("n2")) ==
              QStringList({"/t/x", "add-core", "--execute", "--region", "ewr", "--replace", "n2"}),
          "replace arguments");
    check(addCoreArguments(QStringLiteral("/t/x"), QString(), QString(), QStringLiteral("n5"),
                           true, QString(), QStringLiteral("n2")) ==
              QStringList({"/t/x", "add-core", "--execute", "--name", "n5"}),
          "a resumed replacement keeps its target from the state file");
    check(addCoreOptionsArguments(QStringLiteral("/t/x"), QStringLiteral("vultr"),
                                  QStringLiteral("n2")) ==
              QStringList({"/t/x", "add-core-options", "--json", "--replace", "n2"}),
          "replace options arguments");
    const QStringList fleet{QStringLiteral("203.0.113.11")};
    check(isFleetInstanceHost(QStringLiteral("fmnext-core-4"), QString(), fleet),
          "fmnext label refused");
    check(isFleetInstanceHost(QStringLiteral("FMNEXT-run-1"), QString(), fleet),
          "fmnext label refused regardless of case");
    check(isFleetInstanceHost(QStringLiteral("my-box"), QStringLiteral(" 203.0.113.11"), fleet),
          "fleet address refused");
    check(!isFleetInstanceHost(QStringLiteral("mirror35"), QStringLiteral("198.51.100.9"), fleet),
          "ordinary host allowed");
}

void testRunParsing()
{
    AddCoreRun run;
    run.setDefaultSteps(AddCoreRun::stepsFromJson(QByteArray(kOptions)));
    check(run.steps().size() == 4 && run.state() == AddCoreRun::State::Idle, "default steps");
    check(run.remainingEstimateSeconds() == 437, "remaining estimate before start");
    QString log;
    log += run.consume(QStringLiteral("hello\nFMNEXT-ADD-CORE {\"event\": \"start\", "
                                      "\"name\": \"n4\", \"steps\": [{\"id\": \"plan\", "
                                      "\"title\": \"Plan\", \"estimate\": 2}, {\"id\": \"vm\", "
                                      "\"title\": \"VM\", \"estimate\": 120}]}\nFMNEXT-ADD"));
    check(log == QLatin1String("hello\n"), "log excludes events and keeps partial lines");
    check(run.state() == AddCoreRun::State::Running && run.steps().size() == 2, "start event");
    log += run.consume(QStringLiteral("-CORE {\"event\": \"step\", \"step\": \"plan\", "
                                      "\"status\": \"running\", \"title\": \"Plan\"}\n"));
    check(run.headline().contains(QLatin1String("step 1 of 2")), "running headline");
    log += run.consume(QStringLiteral("[execute] plan\r\nFMNEXT-ADD-CORE {\"event\": \"step\", "
                                      "\"step\": \"plan\", \"status\": \"done\", \"seconds\": 1.5}\n"));
    check(log == QLatin1String("hello\n[execute] plan\n"), "log keeps ordinary lines");
    check(run.doneCount() == 1 && run.steps().at(0).seconds == 1.5, "step done");
    run.consume(QStringLiteral("FMNEXT-ADD-CORE {\"event\": \"step\", \"step\": \"vm\", "
                               "\"status\": \"failed\", \"error\": \"no SSH\"}\n"
                               "FMNEXT-ADD-CORE {\"event\": \"result\", \"status\": \"failed\", "
                               "\"name\": \"n4\", \"step\": \"vm\", \"error\": \"no SSH\", "
                               "\"notes\": [\"n4 is out of DNS\"]}\n"));
    run.markProcessEnded(1, false);
    check(run.state() == AddCoreRun::State::Failed && run.failedStep() == QLatin1String("vm"),
          "failed result");
    check(run.notes() == QStringList({"n4 is out of DNS"}), "failure notes");
    check(run.headline().contains(QString::fromUtf8("failed at \xe2\x80\x9cVM\xe2\x80\x9d: no SSH")),
          "failure headline");
    check(run.consume(QStringLiteral("FMNEXT-ADD-CORE not json\n")) ==
              QLatin1String("FMNEXT-ADD-CORE not json\n"),
          "malformed events stay in the log");
    run.reset();
    check(run.state() == AddCoreRun::State::Idle && run.doneCount() == 0 &&
              run.steps().size() == 2,
          "reset keeps the step list");
    run.consume(QStringLiteral("FMNEXT-ADD-CORE {\"event\": \"start\", \"name\": \"n4\"}\n"
                               "FMNEXT-ADD-CORE {\"event\": \"step\", \"step\": \"plan\", "
                               "\"status\": \"running\"}\n"));
    run.markProcessEnded(137, true);
    check(run.state() == AddCoreRun::State::Failed && run.failedStep() == QLatin1String("plan") &&
              run.steps().at(0).status == AddCoreStep::Status::Failed,
          "a crash without a result fails the running step");
    AddCoreRun ok;
    ok.consume(QStringLiteral("FMNEXT-ADD-CORE {\"event\": \"result\", \"status\": \"ok\", "
                              "\"name\": \"n4\", \"summary\": \"n4 is serving, in DNS\", "
                              "\"branch\": \"fleet/add-core-n4\", \"commit\": \"abc\"}"));
    check(ok.state() == AddCoreRun::State::Idle, "an unterminated line waits for flush");
    ok.flush();
    ok.markProcessEnded(0, false);
    check(ok.state() == AddCoreRun::State::Succeeded &&
              ok.headline() == QLatin1String("n4 is serving, in DNS") &&
              ok.branch() == QLatin1String("fleet/add-core-n4"),
          "success result");
    AddCoreRun silent;
    silent.consume(QStringLiteral("FMNEXT-ADD-CORE {\"event\": \"start\", \"name\": \"n4\"}\n"));
    silent.markProcessEnded(0, false);
    check(silent.state() == AddCoreRun::State::Failed, "exit without a result is a failure");
}

void testStatusLoading()
{
    AddCoreRun run;
    run.setDefaultSteps(AddCoreRun::stepsFromJson(QByteArray(kOptions)));
    check(!run.loadStatus("null"), "no recorded run");
    check(run.loadStatus(R"({"name": "n4", "steps": {"plan": "done", "vm": "done", "mesh": "running", "dns": "pending"}})"),
          "interrupted run loads");
    check(run.state() == AddCoreRun::State::Failed && run.failedStep() == QLatin1String("mesh"),
          "an interrupted run is retryable");
    check(run.steps().at(2).status == AddCoreStep::Status::Failed, "interrupted step marked");
    AddCoreRun failed;
    failed.setDefaultSteps(AddCoreRun::stepsFromJson(QByteArray(kOptions)));
    failed.loadStatus(R"({"name": "n4", "failed": {"step": "mesh", "error": "boom"}, "steps": {"plan": "done", "vm": "done", "mesh": "failed"}})");
    check(failed.error() == QLatin1String("boom") && failed.doneCount() == 2, "failed status");
    AddCoreRun finished;
    finished.setDefaultSteps(AddCoreRun::stepsFromJson(QByteArray(kOptions)));
    finished.loadStatus(R"({"name": "n4", "finished": true, "steps": {"plan": "done", "vm": "done", "mesh": "done", "dns": "done"}})");
    check(finished.state() == AddCoreRun::State::Succeeded && finished.doneCount() == 4,
          "finished status");
    AddCoreRun replaced;
    replaced.setDefaultSteps(AddCoreRun::stepsFromJson(QByteArray(kOptions)));
    replaced.loadStatus(R"({"name": "n5", "replaces": "n2", "failed": {"step": "remove", "error": "quorum"},)"
                        R"( "steps": {"plan": "done", "remove": "failed"},)"
                        R"( "step_list": [{"id": "plan", "title": "Plan", "estimate": 2},)"
                        R"( {"id": "remove", "title": "Take the replaced core out", "estimate": 520}]})");
    check(replaced.replaces() == QLatin1String("n2") && replaced.steps().size() == 2 &&
              replaced.steps().at(1).status == AddCoreStep::Status::Failed,
          "a replacement's status brings its own step list");
}

void testFleetLandingText()
{
    AddCoreRun landed;
    landed.consume(QStringLiteral(
        "FMNEXT-ADD-CORE {\"event\": \"start\", \"name\": \"n5\", \"steps\": []}\n"
        "FMNEXT-ADD-CORE {\"event\": \"result\", \"status\": \"ok\", \"name\": \"n5\", "
        "\"branch\": \"fleet/add-core-n5\", \"commit\": \"0123456789abcdef\", "
        "\"merged\": \"fedcba9876543210\", \"merge_error\": \"\"}\n"));
    check(fleetLandingText(landed, QStringLiteral("include it")) ==
              QLatin1String("fleet/add-core-n5 (0123456789ab) is merged into main "
                            "(fedcba987654), so autodeploy and roll include it."),
          "landed fleet change named");
    AddCoreRun refused;
    refused.consume(QStringLiteral(
        "FMNEXT-ADD-CORE {\"event\": \"result\", \"status\": \"ok\", \"name\": \"n5\", "
        "\"branch\": \"fleet/add-core-n5\", \"commit\": \"0123456789abcdef\", "
        "\"merged\": \"\", \"merge_error\": \"deploy/fleet.toml is staged\"}\n"));
    const QString text = fleetLandingText(refused, QStringLiteral("include it"));
    check(text.contains(QLatin1String("not on main yet")) &&
              text.contains(QLatin1String("deploy/fleet.toml is staged")),
          "refused merge asks for one and gives the reason");
    check(fleetLandingText(AddCoreRun(), QStringLiteral("x")).isEmpty(), "no run, no landing text");
}

struct FakeTool {
    QTemporaryDir dir;
    QString script;

    FakeTool()
    {
        QDir(dir.path()).mkpath(QStringLiteral("tools"));
        script = dir.filePath(QStringLiteral("tools/next_fleet.py"));
        write(QStringLiteral("tools/next_fleet.py"), QByteArray(kFakeTool));
        write(QStringLiteral("tools/options.json"), QByteArray(kOptions));
    }
    void write(const QString &name, const QByteArray &data)
    {
        QFile file(dir.filePath(name));
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(data);
    }
    void remove(const QString &name) { QFile::remove(dir.filePath(name)); }
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

void grab(QWidget *dialog, const QString &directory, const QString &name)
{
    if (directory.isEmpty())
        return;
    QCoreApplication::processEvents();
    dialog->grab().save(QDir(directory).filePath(name));
}

const char kAwsOptions[] =
    R"({"estimate_seconds": 847, "name": "n4", "plan": "t3a.micro", "region": "us-east-1",)"
    R"( "provider": "aws", "providers": ["vultr", "aws"],)"
    R"( "price_note": "AWS prices are us-east-1 on-demand estimates",)"
    R"( "rev": "e416743eef92", "voter": false, "voter_reason": "non-voter",)"
    R"( "regions": ["us-east-1", "eu-west-1"],)"
    R"( "plans": [{"id": "t3a.micro", "ram": 1024, "vcpu_count": 2, "monthly_cost": 12.11,)"
    R"(   "locations": ["us-east-1", "eu-west-1"]},)"
    R"(  {"id": "t3a.small", "ram": 2048, "vcpu_count": 2, "monthly_cost": 18.97,)"
    R"(   "locations": ["us-east-1", "eu-west-1"]}],)"
    R"( "steps": [{"id": "plan", "title": "Plan", "estimate": 2},)"
    R"( {"id": "vm", "title": "Create the VM", "estimate": 120}]})";

void testAwsProvider(const QString &screenshots)
{
    const QStringList fresh = addCoreArguments(
        QStringLiteral("/t/next_fleet.py"), QStringLiteral("us-east-1"),
        QStringLiteral("t3a.micro"), QStringLiteral("n4"), false, QStringLiteral("aws"));
    check(fresh == QStringList({"/t/next_fleet.py", "add-core", "--execute", "--provider", "aws",
                                "--region", "us-east-1", "--plan", "t3a.micro"}),
          "aws arguments carry the provider");
    check(addCoreArguments(QStringLiteral("/t/x"), QString(), QString(), QStringLiteral("n4"),
                           true, QStringLiteral("aws")) ==
              QStringList({"/t/x", "add-core", "--execute", "--name", "n4"}),
          "a resume keeps the recorded provider");
    check(addCoreOptionsArguments(QStringLiteral("/t/x"), QStringLiteral("vultr")) ==
              QStringList({"/t/x", "add-core-options", "--json"}),
          "vultr options arguments unchanged");
    check(addCoreOptionsArguments(QStringLiteral("/t/x"), QStringLiteral("aws")) ==
              QStringList({"/t/x", "add-core-options", "--json", "--provider", "aws"}),
          "aws options arguments");
    const AddCoreOptions parsed = parseAddCoreOptions(QByteArray(kAwsOptions));
    check(parsed.provider == QLatin1String("aws") &&
              parsed.providers == QStringList({"vultr", "aws"}) && !parsed.priceNote.isEmpty(),
          "aws options parse");
    check(parseAddCoreOptions(QByteArray(kOptions)).provider == QLatin1String("vultr"),
          "options without a provider are vultr");
    const QString confirm = confirmationText(parsed, QStringLiteral("us-east-1"),
                                             QStringLiteral("t3a.micro"));
    check(confirm.contains(QLatin1String("new AWS VM fmnext-core-4 in us-east-1 (t3a.micro, "
                                         "$12.11/month), billed to the ForkMesh AWS account")),
          "aws confirmation names the cloud");
    check(confirm.contains(QLatin1String("forkmesh-next=1")) &&
              confirm.contains(QLatin1String("IMDSv2")),
          "aws confirmation names the guards");

    FakeTool tool;
    tool.write(QStringLiteral("tools/options_aws.json"), QByteArray(kAwsOptions));
    AddCoreNodeController controller(tool.script);
    auto *dialog = new AddCoreNodeDialog(&controller);
    dialog->show();
    check(waitFor([&] { return controller.options().valid && controller.statusLoaded(); }),
          "aws: vultr options load first");
    auto *provider = child<QComboBox>(dialog, "addCoreProvider");
    check(provider && provider->currentData().toString() == QLatin1String("vultr"),
          "aws: vultr is the default cloud");
    provider->setCurrentIndex(provider->findData(QStringLiteral("aws")));
    check(waitFor([&] { return controller.options().provider == QLatin1String("aws"); }),
          "aws: options reload for aws");
    QCoreApplication::processEvents();
    auto *region = child<QComboBox>(dialog, "addCoreRegion");
    auto *plan = child<QComboBox>(dialog, "addCorePlan");
    auto *cost = child<QLabel>(dialog, "addCoreCost");
    auto *next = child<QPushButton>(dialog, "addCoreNext");
    check(region->currentData().toString() == QLatin1String("us-east-1"), "aws: region");
    check(plan->currentData().toString() == QLatin1String("t3a.micro") && plan->count() == 2,
          "aws: plans");
    check(cost->text() == QLatin1String("$12.11/month (estimate)"), "aws: estimated cost");
    check(provider->currentData().toString() == QLatin1String("aws"), "aws: choice kept");
    check(next->isEnabled(), "aws: next enabled");
    grab(dialog, screenshots, QStringLiteral("add-core-aws-choose.png"));
    next->click();
    check(child<QLabel>(dialog, "addCoreConfirmText")->text().contains(QLatin1String("AWS VM")),
          "aws: confirm page");
    bool finishedSeen = false;
    QObject::connect(&controller, &AddCoreNodeController::finished,
                     [&](bool) { finishedSeen = true; });
    child<QPushButton>(dialog, "addCoreCreate")->click();
    check(waitFor([&] { return finishedSeen; }), "aws: run finishes");
    check(tool.argsLog().trimmed() ==
              QLatin1String("add-core --execute --provider aws --region us-east-1 --plan t3a.micro"),
          "aws: the tool got the provider");
    delete dialog;

    QByteArray missing(kAwsOptions);
    missing.replace("\"provider\": \"aws\",",
                    "\"provider\": \"aws\", \"plans_error\": \"missing /c/aws.env\",");
    FakeTool second;
    second.write(QStringLiteral("tools/options_aws.json"), missing);
    AddCoreNodeController blocked(second.script);
    blocked.setProvider(QStringLiteral("aws"));
    auto *blockedDialog = new AddCoreNodeDialog(&blocked);
    blockedDialog->show();
    check(waitFor([&] { return blocked.options().valid && blocked.statusLoaded(); }),
          "aws: options without credentials load");
    QCoreApplication::processEvents();
    check(!child<QPushButton>(blockedDialog, "addCoreNext")->isEnabled(),
          "aws: no credentials, no next");
    check(child<QLabel>(blockedDialog, "addCoreError")->text().contains(QLatin1String("aws.env")),
          "aws: the missing aws.env is explained");
    delete blockedDialog;
}

void testDialogFlow(const QString &screenshots)
{
    FakeTool tool;
    tool.write(QStringLiteral("tools/fail_at"), "mesh");
    AddCoreNodeController controller(tool.script);
    check(controller.isAvailable(), "fake tool available");
    auto *dialog = new AddCoreNodeDialog(&controller);
    dialog->show();
    check(dialog->page() == AddCoreNodeDialog::Page::Choose, "opens on the choose page");
    check(waitFor([&] { return controller.options().valid && controller.statusLoaded(); }),
          "options load");
    QCoreApplication::processEvents();
    auto *region = child<QComboBox>(dialog, "addCoreRegion");
    auto *plan = child<QComboBox>(dialog, "addCorePlan");
    auto *cost = child<QLabel>(dialog, "addCoreCost");
    auto *next = child<QPushButton>(dialog, "addCoreNext");
    check(region && region->currentData().toString() == QLatin1String("ewr"), "default region");
    check(plan && plan->currentData().toString() == QLatin1String("vc2-1c-1gb"), "default plan");
    check(plan && plan->count() == 2, "both ewr plans listed");
    check(cost && cost->text() == QLatin1String("$5.00/month"), "cost shown");
    check(child<QLabel>(dialog, "addCoreTime")->text().startsWith(QLatin1String("about 14 min")),
          "time shown");
    check(child<QLabel>(dialog, "addCoreRole")->text().startsWith(QLatin1String("non-voter")),
          "rqlite role shown");
    check(next && next->isEnabled(), "next enabled");
    grab(dialog, screenshots, QStringLiteral("add-core-1-choose.png"));
    plan->setCurrentIndex(plan->findData(QStringLiteral("vc2-2c-4gb")));
    check(cost->text() == QLatin1String("$20.00/month"), "cost follows the plan");
    region->setCurrentIndex(region->findData(QStringLiteral("sjc")));
    check(plan->count() == 1 && plan->currentData().toString() == QLatin1String("vc2-1c-1gb"),
          "plans follow the region");
    region->setCurrentIndex(region->findData(QStringLiteral("ewr")));
    plan->setCurrentIndex(plan->findData(QStringLiteral("vc2-1c-1gb")));
    next->click();
    check(dialog->page() == AddCoreNodeDialog::Page::Confirm, "confirm page");
    auto *confirm = child<QLabel>(dialog, "addCoreConfirmText");
    check(confirm->text().contains(QLatin1String("fmnext-core-4 in ewr (vc2-1c-1gb, $5.00/month)")),
          "confirm text");
    check(child<QPushButton>(dialog, "addCoreCreate")->text() == QLatin1String("Create and join n4"),
          "create button names the node");
    grab(dialog, screenshots, QStringLiteral("add-core-2-confirm.png"));
    child<QPushButton>(dialog, "addCoreBack")->click();
    check(dialog->page() == AddCoreNodeDialog::Page::Choose, "back returns");
    next->click();
    bool finishedOk = true;
    bool finishedSeen = false;
    QObject::connect(&controller, &AddCoreNodeController::finished, [&](bool ok) {
        finishedSeen = true;
        finishedOk = ok;
    });
    child<QPushButton>(dialog, "addCoreCreate")->click();
    check(dialog->page() == AddCoreNodeDialog::Page::Progress, "progress page");
    check(waitFor([&] { return finishedSeen; }), "failed run finishes");
    QCoreApplication::processEvents();
    check(!finishedOk && controller.run().state() == AddCoreRun::State::Failed, "run failed");
    check(tool.argsLog().trimmed() ==
              QLatin1String("add-core --execute --region ewr --plan vc2-1c-1gb"),
          "the tool got the chosen region and plan");
    auto *retry = child<QPushButton>(dialog, "addCoreRetry");
    check(retry && retry->isVisible(), "retry shown after a failure");
    auto *steps = child<QListWidget>(dialog, "addCoreSteps");
    check(steps && steps->count() == 4 && steps->item(2)->text().contains(QString::fromUtf8("\xe2\x9c\x97")),
          "failed step marked");
    check(steps->item(0)->text().contains(QString::fromUtf8("\xe2\x9c\x93")), "done step marked");
    check(child<QLabel>(dialog, "addCoreHeadline")->text().contains(QLatin1String("failed")),
          "failure headline");
    check(child<QLabel>(dialog, "addCoreNotes")->text().contains(QLatin1String("out of DNS")),
          "containment notes shown");
    const QString log = child<QPlainTextEdit>(dialog, "addCoreLog")->toPlainText();
    check(log.contains(QLatin1String("[execute] mesh: working")) &&
              log.contains(QLatin1String("step mesh failed")) &&
              !log.contains(QLatin1String("FMNEXT-ADD-CORE")),
          "log streams tool output without events");
    grab(dialog, screenshots, QStringLiteral("add-core-4-failed.png"));
    dialog->close();
    QCoreApplication::processEvents();
    auto *reopened = new AddCoreNodeDialog(&controller);
    reopened->show();
    check(reopened->page() == AddCoreNodeDialog::Page::Progress, "reopened on progress");
    check(child<QPushButton>(reopened, "addCoreRetry")->isVisible(), "reopened keeps retry");
    check(child<QPlainTextEdit>(reopened, "addCoreLog")->toPlainText().contains(
              QLatin1String("[execute] mesh: working")),
          "reopened keeps the log");
    finishedSeen = false;
    child<QPushButton>(reopened, "addCoreRetry")->click();
    check(waitFor([&] { return finishedSeen; }), "retry finishes");
    QCoreApplication::processEvents();
    check(finishedOk && controller.run().state() == AddCoreRun::State::Succeeded, "retry succeeds");
    check(controller.lastArguments().mid(1) ==
              QStringList({"add-core", "--execute", "--name", "n4"}),
          "retry resumes by name only");
    check(child<QLabel>(reopened, "addCoreHeadline")->text().startsWith(
              QLatin1String("n4 is serving, in DNS")),
          "success headline");
    check(child<QLabel>(reopened, "addCoreHeadline")->text().contains(
              QLatin1String("fleet/add-core-n4 (0123456789ab)")),
          "success names the branch to merge");
    check(!child<QPushButton>(reopened, "addCoreRetry")->isVisible(), "no retry after success");
    grab(reopened, screenshots, QStringLiteral("add-core-5-success.png"));
    reopened->close();
    QCoreApplication::processEvents();
}

void testReplaceFlow(const QString &screenshots)
{
    FakeTool tool;
    AddCoreNodeController controller(tool.script);
    controller.setReplaceTarget(QStringLiteral("n2"));
    auto *dialog = new AddCoreNodeDialog(&controller);
    dialog->show();
    check(waitFor([&] {
              return controller.options().valid && controller.statusLoaded() &&
                     controller.options().replaces == QLatin1String("n2");
          }),
          "replace: options load with the target");
    QCoreApplication::processEvents();
    check(dialog->windowTitle() == QLatin1String("Replace core node n2"), "replace: window title");
    check(child<QLabel>(dialog, "addCoreTitle")->text() ==
              QLatin1String("Replace n2 with a new core node"),
          "replace: page title");
    check(child<QLabel>(dialog, "addCoreIntro")->text().contains(
              QLatin1String("n2 is taken out of the fleet in the same run")),
          "replace: intro names the old core");
    child<QPushButton>(dialog, "addCoreNext")->click();
    check(child<QPushButton>(dialog, "addCoreCreate")->text() ==
              QLatin1String("Create n4 and remove n2"),
          "replace: create button names both cores");
    check(child<QLabel>(dialog, "addCoreConfirmText")->text().contains(
              QLatin1String("then take n2 out of the fleet")),
          "replace: confirmation");
    grab(dialog, screenshots, QStringLiteral("replace-core-1-confirm.png"));
    bool finishedSeen = false;
    QObject::connect(&controller, &AddCoreNodeController::finished,
                     [&](bool) { finishedSeen = true; });
    child<QPushButton>(dialog, "addCoreCreate")->click();
    check(waitFor([&] { return finishedSeen; }), "replace: run finishes");
    QCoreApplication::processEvents();
    check(tool.argsLog().trimmed() ==
              QLatin1String("add-core --execute --region ewr --plan vc2-1c-1gb --replace n2"),
          "replace: the tool got the target");
    check(controller.run().state() == AddCoreRun::State::Succeeded &&
              controller.run().replaces() == QLatin1String("n2"),
          "replace: run records the target");
    check(controller.activeReplaceTarget() == QLatin1String("n2"),
          "replace: the finished run still names its target");
    auto *steps = child<QListWidget>(dialog, "addCoreSteps");
    check(steps && steps->count() == 5 &&
              steps->item(4)->data(Qt::UserRole).toString() == QLatin1String("remove"),
          "replace: the removal is a step");
    grab(dialog, screenshots, QStringLiteral("replace-core-2-success.png"));
    dialog->close();
    QCoreApplication::processEvents();
    controller.clearRun();
    controller.setReplaceTarget(QString());
    check(waitFor([&] { return controller.options().valid && controller.options().replaces.isEmpty() &&
                               !controller.optionsLoading(); }),
          "replace: clearing the target reloads plain options");
    check(controller.activeReplaceTarget().isEmpty(), "replace: plain add after clearing");
}

void testRunSurvivesClosingTheDialog(const QString &screenshots)
{
    FakeTool tool;
    tool.write(QStringLiteral("tools/slow"), "1");
    AddCoreNodeController controller(tool.script);
    auto *dialog = new AddCoreNodeDialog(&controller);
    dialog->show();
    check(waitFor([&] { return controller.options().valid; }), "options load for slow run");
    QCoreApplication::processEvents();
    child<QPushButton>(dialog, "addCoreNext")->click();
    child<QPushButton>(dialog, "addCoreCreate")->click();
    check(waitFor([&] { return controller.run().doneCount() >= 1; }), "slow run progresses");
    check(child<QPushButton>(dialog, "addCoreClose")->text() ==
              QLatin1String("Close (keeps running)"),
          "close says the run continues");
    grab(dialog, screenshots, QStringLiteral("add-core-3-running.png"));
    dialog->close();
    QCoreApplication::processEvents();
    check(controller.isRunning(), "closing the dialog does not stop the run");
    check(waitFor([&] { return !controller.isRunning(); }), "slow run ends");
    check(controller.run().state() == AddCoreRun::State::Succeeded, "slow run succeeded");
    auto *reopened = new AddCoreNodeDialog(&controller);
    reopened->show();
    check(reopened->page() == AddCoreNodeDialog::Page::Progress, "reopened after the run");
    check(child<QLabel>(reopened, "addCoreHeadline")->text().startsWith(
              QLatin1String("n4 is serving, in DNS")),
          "reopened shows the result");
    reopened->close();
    QCoreApplication::processEvents();
}

void testUnfinishedRunBlocksANewOne()
{
    FakeTool tool;
    tool.write(QStringLiteral("tools/options.json"),
               QByteArray(kOptions).replace(R"("name": "n4")",
                                            R"("name": "n5", "unfinished": {"name": "n4", "failed": {"step": "mesh", "error": "x"}})"));
    tool.write(QStringLiteral("tools/state.json"),
               R"({"name": "n4", "failed": {"step": "mesh", "error": "x"}, "steps": {"plan": "done", "vm": "done", "mesh": "failed"}})");
    AddCoreNodeController controller(tool.script);
    auto *dialog = new AddCoreNodeDialog(&controller);
    dialog->show();
    check(waitFor([&] { return controller.options().valid && controller.statusLoaded(); }),
          "options and status load");
    QCoreApplication::processEvents();
    check(dialog->page() == AddCoreNodeDialog::Page::Progress, "a half-added core opens on progress");
    check(child<QPushButton>(dialog, "addCoreRetry")->isVisible(), "and offers retry");
    check(!child<QPushButton>(dialog, "addCoreNext")->isEnabled(), "a second core cannot start");
    dialog->close();
    QCoreApplication::processEvents();
}

void testSystemdHelpers()
{
    const QStringList arguments = systemdRunArguments(
        QStringLiteral("fmnext-add-core-1"), QStringLiteral("/l/run.log"), QStringLiteral("/src/tools"),
        QStringLiteral("python3"), {QStringLiteral("/src/tools/next_fleet.py"), QStringLiteral("add-core")},
        {{QStringLiteral("FMNEXT_CONFIG_DIR"), QStringLiteral("/c")}, {QStringLiteral("EMPTY"), QString()}});
    check(arguments.mid(0, 4) == QStringList({"--user", "--collect", "--quiet",
                                              "--unit=fmnext-add-core-1"}),
          "systemd-run unit arguments");
    check(arguments.contains(QStringLiteral("--property=StandardOutput=append:/l/run.log")) &&
              arguments.contains(QStringLiteral("--property=StandardError=append:/l/run.log")),
          "systemd-run appends output to the log");
    check(arguments.contains(QStringLiteral("--setenv=FMNEXT_CONFIG_DIR=/c")) &&
              !arguments.contains(QStringLiteral("--setenv=EMPTY=")),
          "systemd-run environment");
    check(arguments.mid(arguments.size() - 3) ==
              QStringList({"python3", "/src/tools/next_fleet.py", "add-core"}),
          "systemd-run command last");
    check(addCoreUnitName(QStringLiteral("20260930-120000; rm")) ==
              QLatin1String("fmnext-add-core-20260930-120000rm"),
          "unit name is sanitised");
    const UnitState running = parseUnitState("ActiveState=active\nLoadState=loaded\nExecMainStatus=0\n");
    check(running.known && running.running, "active unit");
    const UnitState failed = parseUnitState("ActiveState=failed\nLoadState=loaded\nExecMainStatus=1\n");
    check(failed.known && !failed.running && failed.exitStatus == 1, "failed unit");
    const UnitState gone = parseUnitState("ActiveState=inactive\nLoadState=not-found\nExecMainStatus=0\n");
    check(gone.known && !gone.running, "collected unit");
}

void testDetachedRunSurvivesTheController()
{
    if (qEnvironmentVariable("FORKMESH_ADD_CORE_TEST_SYSTEMD") != QLatin1String("1"))
        return;
    FakeTool tool;
    tool.write(QStringLiteral("tools/slow"), "1");
    QTemporaryDir runs;
    auto *first = new AddCoreNodeController(tool.script);
    first->setRunDirectory(runs.path());
    first->loadOptions();
    check(waitFor([&] { return first->options().valid; }), "detached: options load");
    first->start(QStringLiteral("ewr"), QStringLiteral("vc2-1c-1gb"));
    check(first->unitName().startsWith(QLatin1String("fmnext-add-core-")), "detached: unit started");
    check(QFile::exists(runs.filePath(QStringLiteral("current.json"))), "detached: current run recorded");
    check(waitFor([&] { return first->run().doneCount() >= 1; }), "detached: progress streamed");
    delete first;
    AddCoreNodeController second(tool.script);
    second.setRunDirectory(runs.path());
    check(second.reattach(), "detached: a new controller reattaches");
    check(second.isRunning(), "detached: still running after the first controller is gone");
    check(waitFor([&] { return !second.isRunning(); }, 30000), "detached: run ends");
    check(second.run().state() == AddCoreRun::State::Succeeded, "detached: success seen");
    check(second.log().contains(QLatin1String("[execute] plan: working")), "detached: log replayed");
    check(!QFile::exists(runs.filePath(QStringLiteral("current.json"))), "detached: record cleared");
    check(!second.reattach(), "detached: nothing left to reattach");
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QString screenshots;
    const QStringList arguments = app.arguments();
    const int flag = arguments.indexOf(QStringLiteral("--screenshots"));
    if (flag >= 0 && flag + 1 < arguments.size())
        screenshots = arguments.at(flag + 1);
    testOptionsParsingAndFormatting();
    testArgumentsAndHostGuard();
    testRunParsing();
    testStatusLoading();
    testFleetLandingText();
    testDialogFlow(screenshots);
    testReplaceFlow(screenshots);
    testRunSurvivesClosingTheDialog(screenshots);
    testUnfinishedRunBlocksANewOne();
    testSystemdHelpers();
    testDetachedRunSurvivesTheController();
    testAwsProvider(screenshots);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS add-core-node\n");
    return 0;
}

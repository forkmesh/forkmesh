#include "RepoWorkflow.h"
#include "RepoWorkflowRunner.h"
#include "TeammateStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::repoworkflows;
using forkmesh::teammates::Config;
using forkmesh::teammates::LoadResult;
using forkmesh::teammates::Store;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

QJsonObject node(const QString &id, const QString &type, const QString &name,
                 const QJsonObject &params = {})
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("type"), type},
            {QStringLiteral("name"), name},
            {QStringLiteral("params"), params}};
}

QJsonObject edge(const QString &from, const QString &to)
{
    return {{QStringLiteral("from"), from},
            {QStringLiteral("output"), QStringLiteral("main")},
            {QStringLiteral("to"), to}};
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(object).toJson()) > 0;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    Config quality = Store::qualityAssuranceTemplate();
    quality.improvementNotes = {QStringLiteral("Check the build before the tests.")};
    quality.lowCostModel = true;
    quality.relayChannel = QStringLiteral("#team");
    Workflow converted = fromTeammate(quality);
    check(validate(converted).isEmpty(), "a special agent converts to a valid workflow");
    check(converted.isSpecialAgent() && converted.primaryAgent() == QLatin1String("agent"),
          "the converted workflow has one special agent step");
    check(!converted.nodesOfType(kSchedule).isEmpty() && !converted.nodesOfType(kOutput).isEmpty() &&
              converted.nodesOfType(kMonitor).size() == quality.logMonitors.size(),
          "the conversion keeps the schedule, every log monitor and adds an output step");
    check(converted.icon == quality.avatar, "the workflow takes the agent's portrait as its icon");

    Config roundTrip;
    QString error;
    check(toTeammate(converted, &roundTrip, &error) && error.isEmpty(),
          "a workflow with an agent step reads back as a special agent");
    check(roundTrip.id == quality.id && roundTrip.name == quality.name &&
              roundTrip.specialty == quality.specialty && roundTrip.permissions == quality.permissions &&
              roundTrip.skills == quality.skills && roundTrip.shortcut == quality.shortcut &&
              roundTrip.instructions == quality.instructions &&
              roundTrip.improvementNotes == quality.improvementNotes && roundTrip.lowCostModel &&
              roundTrip.relayChannel == quality.relayChannel &&
              roundTrip.schedule.mode == quality.schedule.mode &&
              roundTrip.schedule.time == quality.schedule.time &&
              roundTrip.logMonitors.size() == quality.logMonitors.size() &&
              roundTrip.logMonitors.first().badge == quality.logMonitors.first().badge,
          "every quality of the special agent survives the round trip");

    Workflow extended = converted;
    extended.nodes.append(node(QStringLiteral("deploy"), kShortcut, QStringLiteral("Deploy"),
                               {{QStringLiteral("file"), QStringLiteral("deploy-new-version.sh")}}));
    extended.connections.append(edge(QStringLiteral("agent"), QStringLiteral("deploy")));
    Config edited = roundTrip;
    edited.focus = QStringLiteral("Release checks");
    edited.schedule.active = true;
    edited.logMonitors.clear();
    applyTeammate(&extended, edited);
    check(validate(extended).isEmpty() && extended.indexOf(QStringLiteral("deploy")) >= 0 &&
              extended.nodesOfType(kMonitor).isEmpty(),
          "saving the agent keeps the other steps and replaces the monitors");
    Config afterEdit;
    check(toTeammate(extended, &afterEdit) && afterEdit.focus == edited.focus && afterEdit.schedule.active,
          "an edited agent reads back from the workflow");
    const QStringList order = extended.ordered();
    check(order.indexOf(QStringLiteral("agent")) < order.indexOf(QStringLiteral("deploy")) &&
              order.indexOf(QStringLiteral("schedule")) < order.indexOf(QStringLiteral("agent")),
          "steps run after the steps they depend on");

    Workflow plain = shortcutWorkflow(QStringLiteral("clean"), QStringLiteral("Clean up"),
                                      QStringLiteral("clean-forkmesh-temporary-files.sh"));
    check(validate(plain).isEmpty() && !plain.isSpecialAgent() && !toTeammate(plain, nullptr, &error) &&
              error.isEmpty(),
          "a shortcut workflow is valid and is not a special agent");

    Workflow cycle = blank(QStringLiteral("loop"), QStringLiteral("Loop"));
    cycle.nodes.append(node(QStringLiteral("a"), kNote, QStringLiteral("A")));
    cycle.nodes.append(node(QStringLiteral("b"), kNote, QStringLiteral("B")));
    cycle.connections = {edge(QStringLiteral("a"), QStringLiteral("b")), edge(QStringLiteral("b"), QStringLiteral("a"))};
    check(validate(cycle) == QLatin1String("cycle"), "a loop of steps is refused");
    Workflow intoTrigger = blank(QStringLiteral("t"), QStringLiteral("T"));
    intoTrigger.nodes.append(node(QStringLiteral("a"), kNote, QStringLiteral("A")));
    intoTrigger.connections = {edge(QStringLiteral("a"), QStringLiteral("run"))};
    check(validate(intoTrigger) == QLatin1String("invalid_connection"), "nothing connects into a trigger");
    Workflow twoSchedules = blank(QStringLiteral("s"), QStringLiteral("S"));
    twoSchedules.nodes.append(node(QStringLiteral("s1"), kSchedule, QStringLiteral("One")));
    twoSchedules.nodes.append(node(QStringLiteral("s2"), kSchedule, QStringLiteral("Two")));
    check(validate(twoSchedules) == QLatin1String("too_many_schedules"), "a workflow has one schedule");
    Workflow escape = blank(QStringLiteral("e"), QStringLiteral("E"));
    escape.nodes.append(node(QStringLiteral("o"), kOutput, QStringLiteral("Out"),
                             {{QStringLiteral("path"), QStringLiteral("../outside.md")}}));
    check(validate(escape) == QLatin1String("invalid_output_path"), "output stays inside the repository");
    Workflow nested = blank(QStringLiteral("n"), QStringLiteral("N"));
    nested.nodes.append(node(QStringLiteral("x"), kShortcut, QStringLiteral("X"),
                             {{QStringLiteral("file"), QStringLiteral("../../etc/passwd")}}));
    check(validate(nested) == QLatin1String("shortcut_needs_file"), "a shortcut step names a file in the folder");
    Workflow unknown = blank(QStringLiteral("u"), QStringLiteral("U"));
    unknown.nodes.append(node(QStringLiteral("x"), QStringLiteral("core.http"), QStringLiteral("X")));
    check(validate(unknown) == QLatin1String("unknown_node_type"), "server-only steps are refused on the desktop");

    check(safeRepositoryPath(QStringLiteral("docs/reports/a.md")) == QLatin1String("docs/reports/a.md") &&
              safeRepositoryPath(QStringLiteral("/etc/passwd")).isEmpty() &&
              safeRepositoryPath(QStringLiteral(".git/config")).isEmpty() &&
              safeRepositoryPath(QStringLiteral(".forkmesh/shortcuts/x.sh")).isEmpty() &&
              safeRepositoryPath(QStringLiteral("docs/../../x")).isEmpty() &&
              safeRepositoryPath(QStringLiteral("C:/x")).isEmpty(),
          "repository output paths are checked");

    const QJsonObject list = catalog();
    bool agentListed = false;
    bool shortcutListed = false;
    for (const QJsonValue &value : list.value(QStringLiteral("core")).toArray()) {
        const QString type = value.toObject().value(QStringLiteral("type")).toString();
        agentListed = agentListed || type == kAgent;
        shortcutListed = shortcutListed || type == kShortcut;
    }
    check(agentListed && shortcutListed && list.value(QStringLiteral("rails")).toArray().size() == 4,
          "the catalog offers the agent and shortcut steps and four rails");

    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory is available");
    const QString forkmesh = QDir(temporary.path()).filePath(QStringLiteral(".forkmesh"));
    const QString workflows = QDir(forkmesh).filePath(QStringLiteral("workflows"));
    const QString legacy = QDir(forkmesh).filePath(QStringLiteral("teammates"));
    Config cfo = Store::chiefFinancialOfficerTemplate();
    check(writeJson(QDir(legacy).filePath(QStringLiteral("cfo.json")), cfo.toJson()),
          "a legacy special agent file is written");
    check(writeJson(QDir(workflows).filePath(QStringLiteral("clean.json")), plain.toJson()),
          "a plain workflow file is written");
    Store store(workflows);
    LoadResult loaded = store.load();
    check(loaded.errors.isEmpty() && loaded.teammates.size() == 1 &&
              loaded.teammates.first().id == QLatin1String("cfo"),
          "the store reads legacy special agents and skips plain workflows");
    check(Store(legacy).load().teammates.size() == 1, "a store opened on the legacy folder sees the same team");

    Config saved = loaded.teammates.first();
    saved.focus = QStringLiteral("Runway");
    check(store.save(&saved, &error), "a legacy special agent saves");
    check(QFileInfo::exists(QDir(workflows).filePath(QStringLiteral("cfo.json"))) &&
              !QFileInfo::exists(QDir(legacy).filePath(QStringLiteral("cfo.json"))),
          "saving moves a legacy special agent into the workflows folder");
    const QJsonObject file = readJson(QDir(workflows).filePath(QStringLiteral("cfo.json")));
    check(file.value(QStringLiteral("kind")).toString() == kFileKind &&
              file.value(QStringLiteral("nodes")).isArray(),
          "the saved special agent is a workflow file");
    loaded = store.load();
    check(loaded.teammates.size() == 1 && loaded.teammates.first().focus == QLatin1String("Runway"),
          "the moved special agent reads back");

    Workflow withStep = Workflow::fromJson(file);
    withStep.nodes.append(node(QStringLiteral("extra"), kNote, QStringLiteral("Extra")));
    withStep.connections.append(edge(QStringLiteral("agent"), QStringLiteral("extra")));
    check(writeJson(QDir(workflows).filePath(QStringLiteral("cfo.json")), withStep.toJson()),
          "a step is added to the workflow file by hand");
    saved = store.load().teammates.first();
    saved.description = QStringLiteral("Keeps the books.");
    check(store.save(&saved, &error) &&
              Workflow::fromJson(readJson(QDir(workflows).filePath(QStringLiteral("cfo.json"))))
                      .indexOf(QStringLiteral("extra")) >= 0,
          "saving the special agent keeps the steps added around it");

    Workflow broken = plain;
    broken.id = QStringLiteral("broken");
    broken.nodes.append(node(QStringLiteral("x"), QStringLiteral("nope"), QStringLiteral("Nope")));
    check(writeJson(QDir(workflows).filePath(QStringLiteral("broken.json")), broken.toJson()),
          "a broken workflow file is written");
    loaded = store.load();
    check(loaded.errors.size() == 1 && loaded.teammates.size() == 1, "a broken workflow file is reported");
    check(store.remove(QStringLiteral("cfo"), &error) && store.load().teammates.isEmpty(),
          "removing the special agent removes its workflow file");

    const QString runs = QDir(temporary.path()).filePath(QStringLiteral("runs/clean.json"));
    QStringList dropped;
    for (int index = 0; index < kMaxRunsKept + 2; ++index) {
        Run run;
        run.id = QStringLiteral("r%1").arg(index);
        run.workflowId = QStringLiteral("clean");
        run.startedAtMs = 1000 + index;
        run.status = kStatusSuccess;
        StepRun step;
        step.nodeId = QStringLiteral("shortcut");
        step.name = QStringLiteral("Clean up");
        step.type = kShortcut;
        step.status = kStatusSuccess;
        step.output = QStringLiteral("freed 2 GB");
        run.steps.append(step);
        check(saveRun(runs, run, &dropped), "a run saves");
    }
    const QList<Run> kept = loadRuns(runs);
    check(kept.size() == kMaxRunsKept && kept.first().id == QStringLiteral("r%1").arg(kMaxRunsKept + 1) &&
              dropped == QStringList({QStringLiteral("r0"), QStringLiteral("r1")}),
          "the newest runs are kept and the dropped ones are named");
    check(runMarkdown(kept.first()).contains(QStringLiteral("## Clean up (succeeded")) &&
              runSummaryText(kept.first()) == QLatin1String("succeeded"),
          "a run renders as Markdown with each step");

    QList<Pin> pins = pinWorkflow({}, {QStringLiteral("forkmesh/forkmesh"), QStringLiteral("cfo"), kRailTop});
    pins = pinWorkflow(pins, {QStringLiteral("forkmesh/forkmesh"), QStringLiteral("clean"), kRailBottom});
    pins = pinWorkflow(pins, {QStringLiteral("ForkMesh/ForkMesh"), QStringLiteral("cfo"), kRailRight});
    pins = pinWorkflow(pins, {QStringLiteral("forkmesh/forkmesh"), QStringLiteral("x"), QStringLiteral("middle")});
    check(pins.size() == 2 && pinnedRail(pins, QStringLiteral("forkmesh/forkmesh"), QStringLiteral("cfo")) == kRailRight,
          "pinning again moves the pin to the other rail");
    const QList<Pin> parsed = parsePins(serializePins(pins));
    check(parsed.size() == 2 && parsed.at(1).rail == kRailBottom, "pins round-trip through settings");
    check(unpinWorkflow(parsed, QStringLiteral("forkmesh/forkmesh"), QStringLiteral("cfo")).size() == 1,
          "a pin is removed");
    check(parsePins(QStringLiteral("[{\"repo\":\"a/b\",\"workflow\":\"w\",\"rail\":\"nowhere\"}]")).isEmpty(),
          "an unknown rail is ignored");

    const auto pinOrder = [](const QList<Pin> &list) {
        QStringList ids;
        for (const Pin &pin : list)
            ids << pin.workflowId + QLatin1Char('@') + pin.rail;
        return ids.join(QLatin1Char(' '));
    };
    QList<Pin> strips;
    for (const QString &id : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
        strips = pinWorkflow(strips, {QStringLiteral("o/r"), id, kRailTop});
    strips = pinWorkflow(strips, {QStringLiteral("o/r"), QStringLiteral("d"), kRailLeft});
    check(pinOrder(movePin(strips, {QStringLiteral("O/R"), QStringLiteral("c"), kRailTop}, 0)) ==
              QLatin1String("c@top a@top b@top d@left"),
          "a pin dragged to the front of its own strip moves there and keeps its repository's spelling");
    check(movePin(strips, {QStringLiteral("O/R"), QStringLiteral("c"), kRailTop}, 0).first().repo ==
              QLatin1String("o/r"),
          "a moved pin keeps the repository it was saved with");
    check(pinOrder(movePin(strips, {QStringLiteral("o/r"), QStringLiteral("a"), kRailLeft}, 0)) ==
              QLatin1String("b@top c@top a@left d@left"),
          "a pin dragged onto the front of another strip lands before that strip's first pin");
    check(pinOrder(movePin(strips, {QStringLiteral("o/r"), QStringLiteral("d"), kRailTop}, 1)) ==
              QLatin1String("a@top d@top b@top c@top"),
          "a pin dragged between two pins of another strip lands between them");
    check(pinOrder(movePin(strips, {QStringLiteral("o/r"), QStringLiteral("a"), kRailBottom}, 5)) ==
              QLatin1String("b@top c@top d@left a@bottom"),
          "a pin dragged onto an empty strip is the only pin there");
    check(pinOrder(movePin(strips, {QStringLiteral("o/r"), QStringLiteral("b"), kRailTop}, 9)) ==
              QLatin1String("a@top c@top b@top d@left"),
          "a slot past the end puts the pin last in its strip");
    check(movePin(strips, {QStringLiteral("o/r"), QStringLiteral("a"), QStringLiteral("middle")}, 0) == strips,
          "a drop on an unknown strip changes nothing");

    {
        qint64 clock = 1000;
        QStringList started;
        QHash<int, bool> finished;
        QList<Run> changes;
        QString markdown;
        QString lastInput;
        RunnerHooks hooks;
        hooks.now = [&clock] { return clock; };
        hooks.start = [&](const StepRequest &request) {
            const QString type = request.node.value(QStringLiteral("type")).toString();
            started.append(request.node.value(QStringLiteral("id")).toString());
            lastInput = request.input;
            StepStart result;
            if (type == kOutput) {
                result.finished = true;
                result.success = request.runMarkdown.contains(QStringLiteral("## Agent"));
                result.output = QStringLiteral("Saved");
                return result;
            }
            result.handle = int(started.size());
            result.sessionId = type == kAgent ? result.handle : 0;
            finished.insert(result.handle, false);
            return result;
        };
        hooks.poll = [&](const QString &, int handle, qint64) {
            StepPoll result;
            result.done = finished.value(handle);
            result.success = true;
            result.output = QStringLiteral("output %1").arg(handle);
            result.caption = handle == 1 ? QStringLiteral("All green") : QString();
            return result;
        };
        int stopped = 0;
        hooks.stop = [&](const QString &, int) { ++stopped; };
        hooks.changed = [&](const Run &run) { changes.append(run); };
        hooks.finished = [&](const Run &, const QString &text) {
            markdown = text;
            return QStringLiteral("/tmp/out.md");
        };
        Runner runner(hooks);
        Workflow chain = blank(QStringLiteral("chain"), QStringLiteral("Chain"));
        chain.nodes.append(node(QStringLiteral("agent"), kAgent, QStringLiteral("Agent"),
                                {{QStringLiteral("specialty"), QStringLiteral("Review")}}));
        chain.nodes.append(node(QStringLiteral("deploy"), kShortcut, QStringLiteral("Deploy"),
                                {{QStringLiteral("file"), QStringLiteral("deploy.sh")}}));
        chain.nodes.append(node(QStringLiteral("save"), kOutput, QStringLiteral("Save")));
        chain.connections = {edge(QStringLiteral("run"), QStringLiteral("agent")),
                             edge(QStringLiteral("agent"), QStringLiteral("deploy")),
                             edge(QStringLiteral("deploy"), QStringLiteral("save"))};
        QString startError;
        const QString runId = runner.start(chain, QStringLiteral("forkmesh/forkmesh"), 0, QStringLiteral("By hand"),
                                           QStringLiteral("on-demand"), QString(), &startError);
        check(!runId.isEmpty() && startError.isEmpty() && started == QStringList{QStringLiteral("agent")} &&
                  runner.isRunning(QStringLiteral("forkmesh/forkmesh"), QStringLiteral("chain")) &&
                  runner.sessionOf(runId) == 1,
              "a run starts its first step at once and reports the agent's session");
        check(runner.start(chain, QStringLiteral("ForkMesh/ForkMesh"), 0, QString(), QString(), QString(), &startError)
                      .isEmpty() &&
                  !startError.isEmpty(),
              "a workflow runs once at a time");
        runner.tick();
        check(started.size() == 1, "a step waits until its agent finishes");
        clock += 5000;
        finished[1] = true;
        runner.tick();
        check(started == QStringList({QStringLiteral("agent"), QStringLiteral("deploy")}) &&
                  lastInput.contains(QStringLiteral("output 1")),
              "the next step starts with the earlier step's output");
        finished[2] = true;
        runner.tick();
        check(!runner.isRunning(QStringLiteral("forkmesh/forkmesh"), QStringLiteral("chain")) && !changes.isEmpty(),
              "the run finishes after its last step");
        const Run done = changes.constLast();
        check(done.status == kStatusSuccess && done.caption == QLatin1String("All green") &&
                  done.output == QLatin1String("output 2") && done.outputFile == QLatin1String("/tmp/out.md") &&
                  done.step(QStringLiteral("save"))->status == kStatusSuccess &&
                  done.step(QStringLiteral("agent"))->sessionId == 1 &&
                  markdown.contains(QStringLiteral("## Deploy (succeeded")),
              "a finished run keeps its caption, output, output file and every step");

        Workflow failing = chain;
        failing.id = QStringLiteral("failing");
        started.clear();
        finished.clear();
        hooks.poll = [&](const QString &, int handle, qint64) {
            StepPoll result;
            result.done = true;
            result.success = handle != 1;
            result.error = QStringLiteral("tests failed");
            return result;
        };
        Runner strict(hooks);
        strict.start(failing, QStringLiteral("a/b"), 0, QString(), QString(), QString());
        strict.tick();
        const Run failed = changes.constLast();
        check(failed.status == kStatusError && failed.step(QStringLiteral("deploy"))->status == kStatusSkipped &&
                  failed.error.contains(QStringLiteral("tests failed")),
              "a failed step stops the run and skips the rest");

        Workflow tolerant = failing;
        tolerant.id = QStringLiteral("tolerant");
        QJsonObject agentNode = tolerant.node(QStringLiteral("agent"));
        agentNode.insert(QStringLiteral("continueOnFail"), true);
        tolerant.nodes.replace(tolerant.indexOf(QStringLiteral("agent")), agentNode);
        started.clear();
        Runner lenient(hooks);
        lenient.start(tolerant, QStringLiteral("a/b"), 0, QString(), QString(), QString());
        lenient.tick();
        check(started.contains(QStringLiteral("deploy")) && lastInput.contains(QStringLiteral("tests failed")),
              "a step that may fail hands its error to the next step");

        Workflow stoppable = chain;
        stoppable.id = QStringLiteral("stoppable");
        hooks.poll = [](const QString &, int, qint64) { return StepPoll{}; };
        Runner halting(hooks);
        halting.start(stoppable, QStringLiteral("a/b"), 0, QString(), QString(), QString());
        check(halting.stop(QStringLiteral("a/b"), QStringLiteral("stoppable")) && stopped == 1 &&
                  changes.constLast().status == kStatusStopped &&
                  !halting.isRunning(QStringLiteral("a/b"), QStringLiteral("stoppable")),
              "stopping a run stops its running step");

        Workflow off = chain;
        off.id = QStringLiteral("off");
        off.enabled = false;
        check(runner.start(off, QStringLiteral("a/b"), 0, QString(), QString(), QString(), &startError).isEmpty() &&
                  startError.contains(QStringLiteral("turned off")),
              "a workflow that is turned off does not run");
    }

    const QString repositoryRoot =
        QDir(QStringLiteral(FORKMESH_SOURCE_DIR)).absoluteFilePath(QStringLiteral(".."));
    const QDir shipped(QDir(repositoryRoot).filePath(kFolder));
    int shippedAgents = 0;
    for (const QFileInfo &info : shipped.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QString problem;
        const Workflow workflow = Workflow::fromJson(readJson(info.absoluteFilePath()), &problem);
        check(problem.isEmpty() && workflow.id == info.completeBaseName(),
              "every shipped workflow file is valid and named after its id");
        if (!problem.isEmpty())
            std::fprintf(stderr, "  %s: %s\n", qPrintable(info.fileName()), qPrintable(problem));
        shippedAgents += workflow.isSpecialAgent() ? 1 : 0;
    }
    check(shippedAgents >= 20, "the repository ships its special agents as workflows");

    if (failures == 0)
        std::printf("repo workflow tests passed\n");
    return failures == 0 ? 0 : 1;
}

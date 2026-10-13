#include "../src/AgentCredentialEnvironment.h"
#include "../src/AgentGuardrails.h"
#include "../src/AgentNetworkGate.h"
#include "../src/AgentRunRules.h"
#include "../src/ClaudeStreamSession.h"
#include "../src/CodexAppServerSession.h"
#include "../src/ProjectAgentAccess.h"
#include "../src/VirtualMachineRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace forkmesh;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const QString &what)
{
    ++checks;
    if (condition) {
        std::printf("PASS %s\n", qPrintable(what));
    } else {
        ++failures;
        std::printf("FAIL %s\n", qPrintable(what));
    }
    std::fflush(stdout);
}

template <typename Predicate> bool pump(Predicate predicate, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

bool writeText(const QString &path, const QByteArray &contents,
               QFileDevice::Permissions permissions = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool written = file.write(contents) == contents.size();
    file.close();
    if (permissions)
        file.setPermissions(permissions);
    return written;
}

const QByteArray kProbeScript = R"PY(#!/usr/bin/env python3
import json, os, socket, subprocess, sys
result = {"argv": sys.argv[1:], "env": dict(os.environ)}
try:
    with open(os.environ["FORKMESH_BOUNDARY_OUTSIDE_FILE"], "rb") as handle:
        result["outsideRead"] = handle.read().decode("utf-8", "replace")
except OSError as error:
    result["outsideRead"] = None
    result["outsideError"] = str(error)
try:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    socket.create_connection(listener.getsockname(), timeout=3).close()
    listener.close()
    result["tcp"] = True
except OSError as error:
    result["tcp"] = False
    result["tcpError"] = str(error)
try:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(3)
    client.connect(os.environ["FORKMESH_BOUNDARY_UNIX_SOCKET"])
    client.close()
    result["unix"] = True
except OSError as error:
    result["unix"] = False
    result["unixError"] = str(error)
try:
    result["exec"] = subprocess.run(["sh", "-c", "echo executed"], capture_output=True,
                                    text=True, timeout=10).stdout.strip() == "executed"
except OSError:
    result["exec"] = False
with open(os.path.join(os.environ["FORKMESH_BOUNDARY_PROBE_DIR"], "probe.json"), "w") as out:
    json.dump(result, out)
print(json.dumps({"type": "system", "subtype": "init", "session_id": "boundary-probe"}), flush=True)
)PY";

const QString kOutsideSecret = QStringLiteral("outside-secret-7f3a");
const QString kAnthropicCanary = QStringLiteral("canary-anthropic-key");
const QString kOpenAiCanary = QStringLiteral("canary-openai-key");
const QString kGitHubCanary = QStringLiteral("canary-gh-token");
const QString kCloudCanary = QStringLiteral("canary-aws-secret");

struct Fixture {
    QString home;
    QString outsideFile;
    QString socketPath;
};

struct Probe {
    bool ran = false;
    QStringList argv;
    QJsonObject env;
    bool outsideRead = false;
    bool tcp = false;
    bool unixSocket = false;
    bool exec = false;
    QString detail;
};

Probe readProbe(const QString &probeDir)
{
    Probe probe;
    QFile file(QDir(probeDir).filePath(QStringLiteral("probe.json")));
    if (!file.open(QIODevice::ReadOnly))
        return probe;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    probe.ran = !object.isEmpty();
    for (const QJsonValue &value : object.value(QStringLiteral("argv")).toArray())
        probe.argv << value.toString();
    probe.env = object.value(QStringLiteral("env")).toObject();
    probe.outsideRead =
        object.value(QStringLiteral("outsideRead")).toString() == kOutsideSecret;
    probe.tcp = object.value(QStringLiteral("tcp")).toBool();
    probe.unixSocket = object.value(QStringLiteral("unix")).toBool();
    probe.exec = object.value(QStringLiteral("exec")).toBool();
    probe.detail = QStringList{object.value(QStringLiteral("outsideError")).toString(),
                               object.value(QStringLiteral("tcpError")).toString(),
                               object.value(QStringLiteral("unixError")).toString()}
                       .join(QLatin1Char(' '))
                       .trimmed();
    return probe;
}

QStringList probeEnvironment(const Fixture &fixture, const QString &probeDir)
{
    return {QStringLiteral("FORKMESH_BOUNDARY_PROBE_DIR=%1").arg(probeDir),
            QStringLiteral("FORKMESH_BOUNDARY_OUTSIDE_FILE=%1").arg(fixture.outsideFile),
            QStringLiteral("FORKMESH_BOUNDARY_UNIX_SOCKET=%1").arg(fixture.socketPath)};
}

agentguard::Config projectLock(const QString &provider, const QString &worktree)
{
    agentguard::Config config;
    config.provider = provider;
    projects::AgentAccess access;
    access.sharedTools = false;
    projects::applyAgentAccess(config, access, worktree, {}, QDir::homePath(), false);
    return config;
}

Probe launchClaude(const Fixture &fixture, const QString &permissionMode, bool locked,
                   QString *stderrSeen = nullptr)
{
    QTemporaryDir worktree;
    const QString probeDir = worktree.filePath(QStringLiteral(".probe"));
    QDir().mkpath(probeDir);
    ClaudeStreamSession session;
    if (locked)
        session.setGuardrails(projectLock(QStringLiteral("claude-code"), worktree.path()));
    int exits = 0;
    QString seen;
    QObject::connect(&session, &ClaudeStreamSession::finished, [&](int) { ++exits; });
    QObject::connect(&session, &ClaudeStreamSession::stderrText,
                     [&](const QString &text) { seen += text; });
    session.start(worktree.path(),
                  agentenv::claudeCredentialVariables() + probeEnvironment(fixture, probeDir),
                  QString(), permissionMode);
    pump([&] { return exits > 0; });
    if (stderrSeen)
        *stderrSeen = seen;
    return readProbe(probeDir);
}

Probe launchCodex(const Fixture &fixture, bool locked)
{
    QTemporaryDir worktree;
    const QString probeDir = worktree.filePath(QStringLiteral(".probe"));
    QDir().mkpath(probeDir);
    CodexAppServerSession session;
    if (locked)
        session.setGuardrails(projectLock(QStringLiteral("codex"), worktree.path()));
    session.setAppServerCommand(
        QDir(fixture.home).filePath(QStringLiteral(".local/bin/codex")));
    int exits = 0;
    QObject::connect(&session, &CodexAppServerSession::finished, [&](int) { ++exits; });
    session.start(worktree.path(),
                  agentenv::codexCredentialVariables() + probeEnvironment(fixture, probeDir),
                  QString(), QString(), QString(), QStringLiteral("Auto"));
    pump([&] { return exits > 0; });
    return readProbe(probeDir);
}

bool hasFlag(const QStringList &argv, const QString &flag, const QString &value = QString())
{
    const int at = argv.indexOf(flag);
    if (at < 0)
        return false;
    return value.isEmpty() || (at + 1 < argv.size() && argv.at(at + 1) == value);
}

void testClaudeModes(const Fixture &fixture)
{
    const struct {
        const char *label;
        const char *id;
        const char *flag;
        const char *value;
    } rows[] = {
        {"Auto", "bypassPermissions", "--dangerously-skip-permissions", ""},
        {"Edit", "acceptEdits", "--permission-mode", "acceptEdits"},
        {"Plan", "plan", "--permission-mode", "plan"},
        {"Ask", "default", "--permission-mode", "default"},
    };
    for (const auto &row : rows) {
        QString stderrSeen;
        const Probe probe = launchClaude(fixture, QString::fromLatin1(row.id), false, &stderrSeen);
        const QString label = QString::fromLatin1(row.label);
        check(probe.ran, QStringLiteral("Claude %1 host: the provider process starts (%2)")
                             .arg(label, stderrSeen.trimmed().left(300)));
        const QStringList others{QStringLiteral("--dangerously-skip-permissions"),
                                 QStringLiteral("--permission-mode")};
        bool exclusive = true;
        for (const QString &other : others)
            if (other != QLatin1String(row.flag) && probe.argv.contains(other))
                exclusive = false;
        check(hasFlag(probe.argv, QString::fromLatin1(row.flag),
                      QString::fromLatin1(row.value)) &&
                  exclusive,
              QStringLiteral("Claude %1 host: the CLI is launched with %2 %3")
                  .arg(label, QString::fromLatin1(row.flag), QString::fromLatin1(row.value)));
        check(probe.outsideRead && probe.exec && probe.tcp && probe.unixSocket,
              QStringLiteral("Claude %1 host, folders unrestricted: reads outside the "
                             "worktree, runs subprocesses, opens TCP and Unix sockets (%2)")
                  .arg(label, probe.detail));
        check(!probe.env.contains(QStringLiteral("ANTHROPIC_API_KEY")) &&
                  probe.env.value(QStringLiteral("GH_TOKEN")).toString() == kGitHubCanary &&
                  probe.env.value(QStringLiteral("AWS_SECRET_ACCESS_KEY")).toString() ==
                      kCloudCanary,
              QStringLiteral("Claude %1 host: Anthropic credentials are scrubbed, unrelated "
                             "desktop secrets are inherited")
                  .arg(label));
    }
}

void testProjectLock(const Fixture &fixture)
{
#ifdef Q_OS_LINUX
    {
        QTemporaryDir project;
        agentguard::Config defaults;
        projects::applyAgentAccess(defaults, projects::AgentAccess(), project.path(), {},
                                   QDir::homePath(), false);
        check(defaults.projectLocked &&
                  (!QFileInfo(QStringLiteral("/tmp")).isDir() ||
                   defaults.projectReadWrite.contains(QStringLiteral("/tmp"))),
              QStringLiteral("default rules: agents are locked to the project, and shared "
                             "tools keep /tmp writable"));
    }
    QString stderrSeen;
    const Probe claude = launchClaude(fixture, QStringLiteral("bypassPermissions"),
                                      true, &stderrSeen);
    check(claude.ran, QStringLiteral("Claude Auto with the project lock: the provider "
                                     "process starts (%1)")
                          .arg(stderrSeen.trimmed().left(300)));
    check(claude.ran && !claude.outsideRead && claude.exec,
          QStringLiteral("Claude Auto with the project lock: a file outside the project "
                         "cannot be read; subprocesses still run"));
    check(claude.tcp && claude.unixSocket,
          QStringLiteral("Claude Auto with the project lock: TCP and Unix sockets still "
                         "connect, the lock is not an egress boundary (%1)")
              .arg(claude.detail));
    check(claude.env.value(QStringLiteral("GH_TOKEN")).toString() == kGitHubCanary &&
              !claude.env.contains(QStringLiteral("ANTHROPIC_API_KEY")),
          QStringLiteral("Claude Auto with the project lock: the environment is the same "
                         "as host mode, unrelated secrets included"));

    const Probe codex = launchCodex(fixture, true);
    check(codex.ran && !codex.outsideRead && codex.tcp,
          QStringLiteral("Codex with the project lock: outside file blocked, network open (%1)")
              .arg(codex.detail));
#else
    check(!projects::folderLockSupported(),
          QStringLiteral("off Linux there is no project folder lock"));
#endif
}

void testCodexHost(const Fixture &fixture)
{
    const Probe probe = launchCodex(fixture, false);
    check(probe.ran, QStringLiteral("Codex host: the app-server process starts"));
    check(probe.outsideRead && probe.exec && probe.tcp && probe.unixSocket,
          QStringLiteral("Codex host, folders unrestricted: reads outside the worktree, "
                         "runs subprocesses, opens TCP and Unix sockets (%1)")
              .arg(probe.detail));
    check(!probe.env.contains(QStringLiteral("OPENAI_API_KEY")) &&
              probe.env.value(QStringLiteral("ANTHROPIC_API_KEY")).toString() ==
                  kAnthropicCanary &&
              probe.env.value(QStringLiteral("GH_TOKEN")).toString() == kGitHubCanary,
          QStringLiteral("Codex host: OpenAI credentials are scrubbed, other providers' "
                         "keys and unrelated desktop secrets are inherited"));
}

void testKvm(const Fixture &fixture)
{
    QTemporaryDir scratch;
    const QByteArray savedTmp = qgetenv("TMPDIR");
    qputenv("TMPDIR", scratch.path().toUtf8());
    const QString workspace = scratch.filePath(QStringLiteral("workspace"));
    QDir().mkpath(workspace);
    vm::initialize(true, workspace, QStringLiteral("/missing/forkmesh-limactl"));

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SSH_AUTH_SOCK"), fixture.socketPath);
    vm::applyGuestEnvironmentPolicy(environment);
    const QStringList bridge =
        environment.value(QStringLiteral("LIMA_SHELLENV_ALLOW")).split(QLatin1Char(','));
    check(!environment.contains(QStringLiteral("GH_TOKEN")) &&
              !environment.contains(QStringLiteral("AWS_SECRET_ACCESS_KEY")) &&
              !environment.contains(QStringLiteral("SSH_AUTH_SOCK")) &&
              environment.value(QStringLiteral("LIMA_SHELLENV_BLOCK")) == QLatin1String("*") &&
              !bridge.contains(QStringLiteral("HOME")) &&
              !bridge.contains(QStringLiteral("GH_TOKEN")),
          QStringLiteral("KVM: unrelated secrets and the SSH agent socket do not cross into "
                         "the guest; the bridge is deny-by-default"));
    check(!vm::pathIsShared(fixture.outsideFile) &&
              !vm::mountedRoots().contains(QDir::homePath()),
          QStringLiteral("KVM: the host home and files outside the launch directory are "
                         "not mounted"));

    ClaudeStreamSession session;
    int exits = 0;
    int code = 0;
    QString seen;
    QObject::connect(&session, &ClaudeStreamSession::finished, [&](int c) {
        ++exits;
        code = c;
    });
    QObject::connect(&session, &ClaudeStreamSession::stderrText,
                     [&](const QString &text) { seen += text; });
    session.start(workspace, agentenv::claudeCredentialVariables(), QString(),
                  QStringLiteral("bypassPermissions"));
    pump([&] { return exits > 0; });
    check(exits == 1 && code != 0 && seen.contains(QStringLiteral("KVM launch failed")),
          QStringLiteral("KVM: with the VM unavailable the launch fails closed instead of "
                         "running on the host"));

    vm::initialize(false, workspace, QStringLiteral("/missing/forkmesh-limactl"));
    if (savedTmp.isNull())
        qunsetenv("TMPDIR");
    else
        qputenv("TMPDIR", savedTmp);
}

QString hookDecision(const QString &output)
{
    return QJsonDocument::fromJson(output.toUtf8())
        .object()
        .value(QStringLiteral("hookSpecificOutput"))
        .toObject()
        .value(QStringLiteral("permissionDecision"))
        .toString();
}

QString runHook(const QString &socketPath, const QJsonObject &event)
{
    QProcess hook;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(agentnet::SocketEnv, socketPath);
    env.insert(agentnet::SessionEnv, QStringLiteral("boundary"));
    env.insert(agentnet::LabelEnv, QStringLiteral("Adversarial fixture"));
    hook.setProcessEnvironment(env);
    hook.start(QStringLiteral("python3"), {QStringLiteral("-c"), agentnet::hookScript()});
    hook.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    hook.closeWriteChannel();
    pump([&] {
        hook.waitForFinished(20);
        return hook.state() == QProcess::NotRunning;
    });
    return QString::fromUtf8(hook.readAllStandardOutput());
}

int runShell(const QString &command, const QString &cwd, const QString &home,
             const agentguard::Config &guardrails, QString *output)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), home);
    const agentguard::LaunchCommand launch = agentguard::restrictedCommand(
        QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command}, cwd, guardrails, env);
    if (!launch.error.isEmpty()) {
        *output = launch.error;
        return -999;
    }
    QProcess process;
    process.setWorkingDirectory(cwd);
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(launch.program, launch.arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        *output = process.errorString();
        return -998;
    }
    *output = QString::fromUtf8(process.readAll());
    return process.exitCode();
}

void testAdversarialFixtures()
{
    QFile file(QStringLiteral(FORKMESH_AGENT_ADVERSARIAL_FIXTURES));
    check(file.open(QIODevice::ReadOnly),
          QStringLiteral("the adversarial fixture set is readable"));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray fixtures = root.value(QStringLiteral("fixtures")).toArray();
    QStringList vectors;
    for (const QJsonValue &value : fixtures)
        vectors << value.toObject().value(QStringLiteral("vector")).toString();
    check(vectors.contains(QStringLiteral("issue")) &&
              vectors.contains(QStringLiteral("skill")) &&
              vectors.contains(QStringLiteral("repository-instruction")) &&
              vectors.contains(QStringLiteral("tool-output")),
          QStringLiteral("the fixture set covers issue text, skill files, repository "
                         "instructions and tool output (%1 fixtures)")
              .arg(fixtures.size()));

    const bool python = !QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty();
    agentnet::Server server;
    QString listenError;
    const bool listening = python && server.listen(&listenError);
    check(!python || listening,
          QStringLiteral("the gate server listens (%1)").arg(listenError));

    agentrules::RunRules rules;
    QList<agentnet::Request> asked;
    agentrules::Verdict lastVerdict = agentrules::Verdict::Run;
    server.setRequestHandler([&](const agentnet::Request &request) {
        asked.append(request);
        const agentrules::Decision decision =
            agentrules::decide(rules, request.call, request.risk, {});
        lastVerdict = decision.verdict;
        server.answer(request.id,
                      decision.verdict == agentrules::Verdict::Run
                          ? agentnet::Server::Decision::Allow
                          : agentnet::Server::Decision::Deny,
                      decision.verdict == agentrules::Verdict::Ask
                          ? QStringLiteral("The person declined.")
                          : decision.reason);
    });

    QTemporaryDir home;
    QTemporaryDir worktree;
    writeText(home.filePath(QStringLiteral(".ssh/id_ed25519")), kOutsideSecret.toUtf8());
    writeText(home.filePath(QStringLiteral(".config/forkmesh-fixture/credentials")),
              kOutsideSecret.toUtf8());

    for (const QJsonValue &value : fixtures) {
        const QJsonObject fixture = value.toObject();
        const QString id = fixture.value(QStringLiteral("id")).toString();
        const QString expect = fixture.value(QStringLiteral("expect")).toString();
        const QString tool = fixture.value(QStringLiteral("tool")).toString();
        const QJsonObject input = fixture.value(QStringLiteral("input")).toObject();
        const QString command = input.value(QStringLiteral("command")).toString();

        rules = agentrules::RunRules();
        QStringList granted;
        for (const QJsonValue &permission :
             fixture.value(QStringLiteral("teammatePermissions")).toArray())
            granted << permission.toString();
        if (fixture.contains(QStringLiteral("teammatePermissions"))) {
            rules.teammateName = QStringLiteral("Fixture teammate");
            rules.deniedPermissions = agentrules::deniedTeammatePermissions(granted);
            rules.externalBlocked =
                rules.deniedPermissions.contains(QStringLiteral("external-tools"));
            rules.allowedToolRules = agentrules::teammateAllowedToolRules(granted);
            rules.access.allowedHosts += agentrules::teammateAllowedHosts(granted);
        }

        const agentrules::Decision decision = agentrules::decide(
            rules, agentnet::classifyToolCall(tool, input, worktree.path()),
            command.isEmpty() ? agentnet::CommandRisk()
                              : agentnet::classifyCommandRisk(command),
            {});
        const agentrules::Verdict wanted =
            expect == QLatin1String("ask")    ? agentrules::Verdict::Ask
            : expect == QLatin1String("deny") ? agentrules::Verdict::Deny
                                              : agentrules::Verdict::Run;
        check(decision.verdict == wanted,
              QStringLiteral("%1 (%2): the run rules answer %3")
                  .arg(id, fixture.value(QStringLiteral("vector")).toString(), expect));

        if (listening) {
            asked.clear();
            const QString output = runHook(
                server.socketPath(),
                QJsonObject{{QStringLiteral("tool_name"), tool},
                            {QStringLiteral("tool_input"), input},
                            {QStringLiteral("cwd"), worktree.path()}});
            if (expect == QLatin1String("ask") || expect == QLatin1String("deny"))
                check(asked.size() == 1 && lastVerdict == wanted &&
                          hookDecision(output) == QLatin1String("deny"),
                      QStringLiteral("%1: the Claude hook escalates to the gate and the "
                                     "call does not run (%2)")
                          .arg(id, output.trimmed().left(200)));
            else
                check(asked.isEmpty() && hookDecision(output).isEmpty(),
                      QStringLiteral("%1: the Claude hook lets the call through without "
                                     "asking (%2)")
                          .arg(id, output.trimmed().left(200)));
        }

        if (expect == QLatin1String("folder-lock")) {
#ifdef Q_OS_LINUX
            QString open;
            const int openExit =
                runShell(command, worktree.path(), home.path(), {}, &open);
            check(openExit == 0 && open.contains(kOutsideSecret),
                  QStringLiteral("%1: without the project lock the command reads the "
                                 "secret, so the fixture is live")
                      .arg(id));
            QString locked;
            const int lockedExit = runShell(command, worktree.path(), home.path(),
                                            projectLock(QStringLiteral("claude-code"), worktree.path()),
                                            &locked);
            check(lockedExit != 0 && !locked.contains(kOutsideSecret),
                  QStringLiteral("%1: the project lock refuses the read (%2)")
                      .arg(id, locked.trimmed().left(200)));
#endif
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    QTemporaryDir sandbox;
    const QString home = QDir(sandbox.path()).filePath(QStringLiteral("home"));
    QDir().mkpath(home);
    qputenv("HOME", home.toUtf8());
    qputenv("XDG_DATA_HOME", QDir(sandbox.path()).filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME",
            QDir(sandbox.path()).filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", QDir(sandbox.path()).filePath(QStringLiteral("cache")).toUtf8());
    qputenv("ANTHROPIC_API_KEY", kAnthropicCanary.toUtf8());
    qputenv("OPENAI_API_KEY", kOpenAiCanary.toUtf8());
    qputenv("GH_TOKEN", kGitHubCanary.toUtf8());
    qputenv("AWS_SECRET_ACCESS_KEY", kCloudCanary.toUtf8());
    QCoreApplication app(argc, argv);

    const QFileDevice::Permissions executable =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    writeText(QDir(home).filePath(QStringLiteral(".local/bin/claude")), kProbeScript,
              executable);
    writeText(QDir(home).filePath(QStringLiteral(".local/bin/codex")), kProbeScript,
              executable);
    writeText(QDir(home).filePath(QStringLiteral(".profile")),
              "PATH=\"$HOME/.local/bin:$PATH\"\n");
    QDir(home).mkpath(QStringLiteral(".claude"));
    QDir(home).mkpath(QStringLiteral(".codex"));

    Fixture fixture;
    fixture.home = home;
    fixture.outsideFile = QDir(sandbox.path()).filePath(QStringLiteral("outside/secret.txt"));
    writeText(fixture.outsideFile, kOutsideSecret.toUtf8());
    fixture.socketPath = QDir(sandbox.path()).filePath(QStringLiteral("outside/agent.sock"));
    QLocalServer socketServer;
    socketServer.setMaxPendingConnections(64);
    check(socketServer.listen(fixture.socketPath),
          QStringLiteral("a Unix socket outside the worktree is listening"));

    if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()) {
        check(false, QStringLiteral("python3 is required for the provider probe"));
    } else {
        testClaudeModes(fixture);
        testCodexHost(fixture);
        testProjectLock(fixture);
    }
    testKvm(fixture);
    testAdversarialFixtures();

    std::printf("%d/%d host boundary checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}

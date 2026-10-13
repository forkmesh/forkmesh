// Project agent access: the per-project policy in project.json, the folder
// lock it puts on agent launches, and the external-call gate that classifies
// tool calls and answers the Claude Code hook over a local socket, and the
// access-failure reports the post-tool hooks send it.

#include "AgentGuardrails.h"
#include "AgentNetworkGate.h"
#include "AgentReadOnlyCommand.h"
#include "AgentRunRules.h"
#include "CommandVariables.h"
#include "DestructiveCommandReview.h"
#include "ProjectAgentAccess.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>

using namespace forkmesh;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", qPrintable(what));
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", qPrintable(what));
    }
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
}

void expectExternal(const QString &command, const QString &target, const QString &cwd = {})
{
    const agentnet::ExternalCall call = agentnet::classifyCommand(command, cwd);
    check(call.external && call.target == target,
          QStringLiteral("external: %1 -> %2 (got %3 %4)")
              .arg(command, target, call.external ? QStringLiteral("external")
                                                  : QStringLiteral("local"),
                   call.target));
}

void expectLocal(const QString &command, const QString &cwd = {})
{
    const agentnet::ExternalCall call = agentnet::classifyCommand(command, cwd);
    check(!call.external, QStringLiteral("local: %1 (got %2)").arg(command, call.target));
}

void testPolicyStorage()
{
    QTemporaryDir temp;
    const QString project = temp.path();

    const projects::AgentAccess fresh = projects::loadAgentAccess(project);
    check(fresh.folders == projects::AgentAccess::Folders::Project,
          QStringLiteral("no project.json: locked to the project folder"));
    check(fresh.network == projects::AgentAccess::Network::Ask,
          QStringLiteral("no project.json: external calls ask"));
    check(fresh.sharedTools, QStringLiteral("no project.json: shared tools on"));

    writeFile(QDir(project).filePath(QStringLiteral("project.json")),
              R"({"schema":1,"name":"Demo","agentAccess":{"folders":"bogus","network":"weird"}})");
    const projects::AgentAccess odd = projects::loadAgentAccess(project);
    check(odd.folders == projects::AgentAccess::Folders::Project &&
              odd.network == projects::AgentAccess::Network::Ask,
          QStringLiteral("unknown values keep the locked default"));

    projects::AgentAccess access;
    access.folders = projects::AgentAccess::Folders::Unrestricted;
    access.network = projects::AgentAccess::Network::Block;
    access.sharedTools = false;
    access.readOnlyFolders = {QStringLiteral("/opt/sdk"), QStringLiteral("relative/ignored")};
    access.readWriteFolders = {QStringLiteral("/srv/share/")};
    access.allowedHosts = {QStringLiteral("https://GitHub.com/org"),
                           QStringLiteral("*.npmjs.org"), QStringLiteral("not a host")};
    QString error;
    check(projects::saveAgentAccess(project, access, &error),
          QStringLiteral("save agent access: %1").arg(error));
    QFile file(QDir(project).filePath(QStringLiteral("project.json")));
    file.open(QIODevice::ReadOnly);
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    check(saved.value(QStringLiteral("name")).toString() == QLatin1String("Demo"),
          QStringLiteral("save keeps the other project.json keys"));

    const projects::AgentAccess loaded = projects::loadAgentAccess(project);
    check(loaded.folders == projects::AgentAccess::Folders::Unrestricted,
          QStringLiteral("round trip: folders"));
    check(loaded.network == projects::AgentAccess::Network::Block,
          QStringLiteral("round trip: network"));
    check(!loaded.sharedTools, QStringLiteral("round trip: shared tools"));
    check(loaded.readOnlyFolders == QStringList{QStringLiteral("/opt/sdk")},
          QStringLiteral("round trip: relative folders dropped"));
    check(loaded.readWriteFolders == QStringList{QStringLiteral("/srv/share")},
          QStringLiteral("round trip: folders cleaned"));
    check(loaded.allowedHosts ==
              QStringList({QStringLiteral("github.com"), QStringLiteral("*.npmjs.org")}),
          QStringLiteral("round trip: hosts normalized (%1)")
              .arg(loaded.allowedHosts.join(QLatin1Char(','))));

    writeFile(QDir(project).filePath(QStringLiteral("project.json")), "{not json");
    check(!projects::saveAgentAccess(project, access, &error),
          QStringLiteral("save refuses to overwrite an unreadable project.json"));
}

void testGlobalRules()
{
    QTemporaryDir temp;
    const QString project = temp.path();
    const projects::AgentAccess fresh = projects::loadAgentAccess(project);
    check(fresh.followGlobal, QStringLiteral("no project.json: follows the global rules"));

    const QString rulesPath = projects::globalRulesPath(QDir(temp.path()).filePath("home"));
    check(rulesPath.endsWith(QLatin1String("home/config/agent-rules.json")),
          QStringLiteral("global rules live in the home project's config folder"));
    const projects::GlobalAgentRules defaults = projects::loadGlobalRules(rulesPath);
    check(defaults.defaults.folders == projects::AgentAccess::Folders::Project &&
              defaults.defaults.network == projects::AgentAccess::Network::Ask &&
              defaults.destructive == projects::GlobalAgentRules::Destructive::Ask &&
              !defaults.toolAllowlist,
          QStringLiteral("missing global rules: locked, ask, destructive commands ask"));

    projects::GlobalAgentRules rules;
    rules.defaults.network = projects::AgentAccess::Network::Allow;
    rules.defaults.allowedHosts = {QStringLiteral("pypi.org")};
    rules.defaults.readOnlyFolders = {QStringLiteral("/opt/shared-sdk")};
    rules.blockedHosts = {QStringLiteral("*.pastebin.com"), QStringLiteral("bad host")};
    rules.toolAllowlist = true;
    rules.allowedToolFamilies = {QStringLiteral("read"), QStringLiteral("grep")};
    rules.destructive = projects::GlobalAgentRules::Destructive::Block;
    QString error;
    check(projects::saveGlobalRules(rulesPath, rules, &error),
          QStringLiteral("save global rules: %1").arg(error));
    const projects::GlobalAgentRules loaded = projects::loadGlobalRules(rulesPath);
    check(loaded.defaults.network == projects::AgentAccess::Network::Allow &&
              loaded.blockedHosts == QStringList{QStringLiteral("*.pastebin.com")} &&
              loaded.toolAllowlist &&
              loaded.allowedToolFamilies ==
                  QStringList({QStringLiteral("read"), QStringLiteral("grep")}) &&
              loaded.destructive == projects::GlobalAgentRules::Destructive::Block,
          QStringLiteral("global rules round trip"));

    projects::AgentAccess following = fresh;
    following.allowedHosts = {QStringLiteral("github.com")};
    const projects::AgentAccess merged = projects::effectiveAgentAccess(following, loaded);
    check(merged.network == projects::AgentAccess::Network::Allow &&
              merged.allowedHosts.contains(QStringLiteral("github.com")) &&
              merged.allowedHosts.contains(QStringLiteral("pypi.org")) &&
              merged.readOnlyFolders.contains(QStringLiteral("/opt/shared-sdk")) &&
              merged.blockedHosts == loaded.blockedHosts,
          QStringLiteral("a following project takes the global defaults and keeps its lists"));
    check(projects::gatesExternalCalls(merged),
          QStringLiteral("blocked destinations keep the gate on when calls are allowed"));
    check(projects::hostBlocked(merged.blockedHosts, QStringLiteral("x.pastebin.com")),
          QStringLiteral("blocked wildcard matches subdomains"));

    projects::AgentAccess own;
    own.network = projects::AgentAccess::Network::Block;
    const projects::AgentAccess ownMerged = projects::effectiveAgentAccess(own, loaded);
    check(ownMerged.network == projects::AgentAccess::Network::Block &&
              ownMerged.allowedHosts.contains(QStringLiteral("pypi.org")),
          QStringLiteral("a project with its own policy keeps it and gains the global lists"));
    check(!projects::webSearchAllowed(ownMerged) && projects::webSearchAllowed(merged),
          QStringLiteral("web search follows the effective network setting"));

    following.followGlobal = true;
    check(projects::saveAgentAccess(project, following, &error) &&
              projects::loadAgentAccess(project).followGlobal &&
              projects::loadAgentAccess(project).allowedHosts ==
                  QStringList{QStringLiteral("github.com")},
          QStringLiteral("a saved project can keep following the global rules"));
}

void expectRisk(const QString &command, const QString &destructive, const QString &permission)
{
    const agentnet::CommandRisk risk = agentnet::classifyCommandRisk(command);
    check(risk.destructive == destructive && risk.permission == permission,
          QStringLiteral("risk: %1 -> [%2|%3] (got [%4|%5])")
              .arg(command, destructive, permission, risk.destructive, risk.permission));
}

void testCommandRisk()
{
    expectRisk(QStringLiteral("rm -rf build"), QStringLiteral("recursive delete"), {});
    expectRisk(QStringLiteral("sudo rm -fr /"), QStringLiteral("recursive delete"), {});
    expectRisk(QStringLiteral("rm --recursive out"), QStringLiteral("recursive delete"), {});
    expectRisk(QStringLiteral("rm -rf /tmp/scratch-1"), {}, {});
    expectRisk(QStringLiteral("rm -f a.o"), {}, {});
    expectRisk(QStringLiteral("find . -name '*.o' -delete"), QStringLiteral("recursive delete"),
               {});
    expectRisk(QStringLiteral("git push --force origin main"), QStringLiteral("force push"),
               QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git push origin +main"), QStringLiteral("force push"),
               QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git push origin :old"), QStringLiteral("force push"),
               QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git push origin HEAD"), {}, QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git -C repo reset --hard HEAD~1"),
               QStringLiteral("discard changes"), {});
    expectRisk(QStringLiteral("git clean -fdx"), QStringLiteral("delete untracked files"), {});
    expectRisk(QStringLiteral("git branch -D topic"), QStringLiteral("branch delete"), {});
    expectRisk(QStringLiteral("git branch --show-current"), {}, {});
    expectRisk(QStringLiteral("git branch feature/x"), {}, QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git checkout -b feature/y"), {}, QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git switch -c feature/z"), {}, QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("git stash drop"), QStringLiteral("stash drop"), {});
    expectRisk(QStringLiteral("git filter-branch --tree-filter x HEAD"),
               QStringLiteral("history rewrite"), {});
    expectRisk(QStringLiteral("git status && git diff"), {}, {});
    expectRisk(QStringLiteral("gh issue comment 12 --body hi"), {},
               QStringLiteral("manage-issues"));
    expectRisk(QStringLiteral("gh issue list"), {}, {});
    expectRisk(QStringLiteral("gh pr review 3 --approve"), {}, QStringLiteral("review-prs"));
    expectRisk(QStringLiteral("gh pr merge 3"), {}, QStringLiteral("merge"));
    expectRisk(QStringLiteral("gh pr create --fill"), {}, QStringLiteral("create-branches"));
    expectRisk(QStringLiteral("dd if=/dev/zero of=/dev/sda"), QStringLiteral("disk wipe"), {});
    expectRisk(QStringLiteral("python3 -c 'import shutil; shutil.rmtree(\"x\")'"),
               QStringLiteral("recursive delete"), {});
}

void expectMatch(const QString &command, const QString &part)
{
    const agentnet::CommandRisk risk = agentnet::classifyCommandRisk(command);
    const QString got = risk.matchStart < 0 ? QStringLiteral("<none>")
                                            : command.mid(risk.matchStart, risk.matchLength);
    check(risk.command == command && got == part,
          QStringLiteral("match: %1 -> \u201c%2\u201d (got \u201c%3\u201d)")
              .arg(command, part, got));
}

void testCommandRiskMatch()
{
    expectMatch(QStringLiteral("rm -rf build"), QStringLiteral("rm -rf build"));
    expectMatch(QStringLiteral("cd /repo && git status --short | head -20 && "
                               "rm -rf 'out dir' ; ls"),
                QStringLiteral("rm -rf 'out dir'"));
    expectMatch(QStringLiteral("git add -A\nrm -rf build/out\ngit status"),
                QStringLiteral("rm -rf build/out"));
    expectMatch(QStringLiteral("echo hi && sudo find . -name x -delete"),
                QStringLiteral("sudo find . -name x -delete"));
    expectMatch(QStringLiteral("git status && git push --force origin main"),
                QStringLiteral("git push --force origin main"));
    expectMatch(QStringLiteral("rm -rf \"${HOME}/x\" && true"),
                QStringLiteral("rm -rf \"${HOME}/x\""));
    expectMatch(QStringLiteral("git push origin HEAD"), QStringLiteral("<none>"));
}

void testScriptClassifier()
{
    QTemporaryDir temp;
    writeFile(QDir(temp.path()).filePath(QStringLiteral("tools/scout.py")),
              "import json\nimport urllib.request\nWWW = \"https://www.reddit.com\"\n");
    writeFile(QDir(temp.path()).filePath(QStringLiteral("tools/local.py")),
              "import json\nfrom urllib.parse import quote\nprint(quote('x'))\n");
    writeFile(QDir(temp.path()).filePath(QStringLiteral("tools/loopback.py")),
              "import urllib.request\nURL = \"http://127.0.0.1:8080/\"\n");
    writeFile(QDir(temp.path()).filePath(QStringLiteral("tools/post.mjs")),
              "await fetch(process.env.TARGET)\n");
    expectExternal(QStringLiteral("python3 tools/scout.py scan"),
                   QStringLiteral("www.reddit.com"), temp.path());
    expectExternal(QStringLiteral("cd x; ./tools/scout.py thread 1"),
                   QStringLiteral("www.reddit.com"), temp.path());
    expectLocal(QStringLiteral("python3 tools/local.py"), temp.path());
    expectLocal(QStringLiteral("python3 tools/loopback.py"), temp.path());
    expectLocal(QStringLiteral("python3 -m pytest tools/scout.py"), temp.path());
    expectExternal(QStringLiteral("node tools/post.mjs"), QStringLiteral("post.mjs network code"),
                   temp.path());
}

void testRunRules()
{
    using agentrules::Verdict;
    agentrules::RunRules rules;
    const QSet<QString> none;
    auto decide = [&](const QString &command) {
        return agentrules::decideCommand(rules, command, QDir::tempPath(), none);
    };
    check(agentrules::gatesCommands(rules),
          QStringLiteral("the default rules gate commands"));
    check(decide(QStringLiteral("make test")).verdict == Verdict::Run,
          QStringLiteral("a local command runs"));
    const agentrules::Decision destructive = decide(QStringLiteral("rm -rf src"));
    check(destructive.verdict == Verdict::Ask &&
              destructive.runKey == QLatin1String("destructive:recursive delete"),
          QStringLiteral("a destructive command asks by default"));
    check(agentrules::decideCommand(rules, QStringLiteral("rm -rf src"), QDir::tempPath(),
                                    {QStringLiteral("destructive:recursive delete")})
                  .verdict == Verdict::Run,
          QStringLiteral("a destructive rule approved for the run goes through"));
    rules.destructive = projects::GlobalAgentRules::Destructive::Block;
    check(decide(QStringLiteral("git reset --hard")).verdict == Verdict::Deny,
          QStringLiteral("blocked destructive commands are refused"));
    rules.destructive = projects::GlobalAgentRules::Destructive::Allow;
    rules.access.network = projects::AgentAccess::Network::Allow;
    check(!agentrules::gatesCommands(rules),
          QStringLiteral("everything allowed: no gate"));
    check(decide(QStringLiteral("curl https://example.com")).verdict == Verdict::Run,
          QStringLiteral("allowed network lets calls through"));
    rules.access.blockedHosts = {QStringLiteral("example.com")};
    check(agentrules::gatesCommands(rules) &&
              decide(QStringLiteral("curl https://example.com")).verdict == Verdict::Deny,
          QStringLiteral("a blocked destination is refused even when calls are allowed"));
    rules.access.blockedHosts.clear();
    rules.access.network = projects::AgentAccess::Network::Ask;
    check(decide(QStringLiteral("curl https://example.org")).verdict == Verdict::Ask,
          QStringLiteral("ask mode asks about external calls"));
    rules.access.allowedHosts = {QStringLiteral("example.org")};
    check(decide(QStringLiteral("curl https://example.org")).verdict == Verdict::Run,
          QStringLiteral("an approved destination goes through"));

    const QStringList hearth{QStringLiteral("read-repositories"),
                             QStringLiteral("manage-issues"),
                             QStringLiteral("external-tools"),
                             QStringLiteral("run-unattended")};
    rules.deniedPermissions = agentrules::deniedTeammatePermissions(hearth);
    rules.teammateName = QStringLiteral("Hearth");
    check(rules.deniedPermissions.contains(QStringLiteral("create-branches")) &&
              !rules.deniedPermissions.contains(QStringLiteral("manage-issues")),
          QStringLiteral("teammate permissions not granted are denied"));
    check(decide(QStringLiteral("git push origin HEAD")).verdict == Verdict::Deny,
          QStringLiteral("a teammate without create-branches cannot push"));
    check(decide(QStringLiteral("gh issue comment 4 -b ok")).verdict != Verdict::Deny,
          QStringLiteral("a teammate with manage-issues may comment"));
    rules.deniedPermissions << QStringLiteral("external-tools");
    rules.externalBlocked = true;
    check(decide(QStringLiteral("curl https://news.example.net")).verdict == Verdict::Deny,
          QStringLiteral("a teammate without external-tools cannot reach the network"));
    check(agentrules::teammateDisallowedTools({QStringLiteral("manage-issues"),
                                               QStringLiteral("external-tools")})
                  .contains(QStringLiteral("mcp__forkmesh__create_issue")),
          QStringLiteral("issue tools are removed without manage-issues"));

    const QStringList steward{QStringLiteral("modify-code"),
                              QStringLiteral("create-branches")};
    check(!agentrules::teammateWorkPermissions(steward, false)
               .contains(QStringLiteral("create-branches")) &&
              agentrules::teammateWorkPermissions(steward, false)
                  .contains(QStringLiteral("modify-code")),
          QStringLiteral("without its own worktree a teammate cannot create branches"));
    check(agentrules::teammateWorkPermissions(steward, true) == steward,
          QStringLiteral("with its own worktree a teammate keeps create-branches"));

    const QString autoMode = QStringLiteral("Auto");
    const QString edit = QStringLiteral("Edit");
    const QString plan = QStringLiteral("Plan");
    check(agentrules::teammateRunMode({}, {QStringLiteral("modify-code")}, autoMode, edit,
                                      plan) == edit,
          QStringLiteral("empty mode with modify-code runs in Edit, not the global mode"));
    check(agentrules::teammateRunMode({}, hearth, autoMode, edit, plan) == autoMode,
          QStringLiteral("empty mode with run-unattended runs in Auto"));
    check(agentrules::teammateRunMode(autoMode, {QStringLiteral("modify-code")}, autoMode, edit,
                                      plan) == edit,
          QStringLiteral("Auto needs run-unattended"));
    check(agentrules::teammateRunMode(autoMode, {QStringLiteral("read-repositories")}, autoMode,
                                      edit, plan) == plan,
          QStringLiteral("without modify-code a teammate plans"));

    check(agentrules::teammateAllowedToolRules(hearth).isEmpty() &&
              agentrules::teammateAllowedHosts(hearth).isEmpty(),
          QStringLiteral("run-unattended alone adds no allow rules"));
    const QStringList marketing{QStringLiteral("run-git"), QStringLiteral("run-tests"),
                                QStringLiteral("read-forkmesh-web")};
    const QStringList allow = agentrules::teammateAllowedToolRules(marketing);
    check(allow.contains(QStringLiteral("Bash(git:*)")) &&
              allow.contains(QStringLiteral("Bash(python3 -m pytest:*)")) &&
              allow.contains(QStringLiteral("Bash(node:*)")) &&
              allow.contains(QStringLiteral("WebFetch(domain:app.forkmesh.com)")) &&
              allow.contains(QStringLiteral("Bash(curl -s https://forkmesh.com/:*)")),
          QStringLiteral("the narrow grants map to Claude Code allow rules"));
    check(!agentrules::teammateAllowedToolRules({QStringLiteral("run-git")})
               .contains(QStringLiteral("Bash(node:*)")),
          QStringLiteral("each grant adds only its own rules"));
    rules.access.allowedHosts = agentrules::teammateAllowedHosts(marketing);
    rules.access.network = projects::AgentAccess::Network::Ask;
    rules.deniedPermissions.removeAll(QStringLiteral("external-tools"));
    rules.externalBlocked = false;
    check(decide(QStringLiteral("curl -s https://app.forkmesh.com/api/leaderboards")).verdict ==
                  Verdict::Run &&
              decide(QStringLiteral("curl -s https://forkmesh.com/nodeinfo/2.1")).verdict ==
                  Verdict::Run,
          QStringLiteral("read-forkmesh-web approves forkmesh.com and its subdomains"));
    check(decide(QStringLiteral("curl -s https://forkmesh.com.example.org/")).verdict ==
              Verdict::Ask,
          QStringLiteral("a look-alike host still asks"));

    const QString hooks = forkmesh::agentnet::claudeHookSettings(true);
    const QJsonObject merged = QJsonDocument::fromJson(
        agentrules::claudeSettingsWithAllowRules(hooks, allow).toUtf8()).object();
    check(merged.contains(QStringLiteral("hooks")) &&
              merged.value(QStringLiteral("permissions")).toObject()
                      .value(QStringLiteral("allow")).toArray().size() == allow.size(),
          QStringLiteral("allow rules join the hook settings without replacing them"));
    check(agentrules::claudeSettingsWithAllowRules(QString(), {}).isEmpty() &&
              agentrules::claudeSettingsWithAllowRules(hooks, {}) == hooks,
          QStringLiteral("no grants leave the settings untouched"));
    check(agentrules::claudeSettingsWithAllowRules(
              QString(), {QStringLiteral("Bash(rm -rf '/')")}).isEmpty(),
          QStringLiteral("a rule that could break shell quoting is dropped"));

    const QStringList logRules = agentrules::appLogReadRules(
        QStringLiteral("/home/f/projects/forkmesh/data/network_log.txt"),
        QStringLiteral("/home/f/projects/forkmesh/data/network_log.d"));
    check(logRules == QStringList{
                          QStringLiteral("Read(//home/f/projects/forkmesh/data/network_log.txt)"),
                          QStringLiteral("Read(//home/f/projects/forkmesh/data/network_log.d/**)")},
          QStringLiteral("a log monitor reads the live app log and its rotated segments"));
    const QJsonArray logAllow =
        QJsonDocument::fromJson(
            agentrules::claudeSettingsWithAllowRules(hooks, logRules).toUtf8())
            .object()
            .value(QStringLiteral("permissions")).toObject()
            .value(QStringLiteral("allow")).toArray();
    check(logAllow.size() == 2 &&
              logAllow.contains(QStringLiteral("Read(//home/f/projects/forkmesh/data/network_log.txt)")),
          QStringLiteral("the log read rules survive the settings rule filter"));
    check(agentrules::appLogReadRules(QStringLiteral("C:\\Users\\f\\ForkMesh\\network_log.txt"),
                                      QStringLiteral("C:\\Users\\f\\ForkMesh\\network_log.d")) ==
              QStringList{QStringLiteral("Read(//c/Users/f/ForkMesh/network_log.txt)"),
                          QStringLiteral("Read(//c/Users/f/ForkMesh/network_log.d/**)")},
          QStringLiteral("Windows log paths become Claude Code's POSIX form"));
    check(agentrules::appLogReadRules(QString(), QStringLiteral("relative/network_log.d"))
                  .isEmpty() &&
              agentrules::appLogReadRules(QStringLiteral("/"), QString()).isEmpty(),
          QStringLiteral("an empty, relative or root path grants nothing"));
    check(!agentrules::teammateAllowedToolRules({agentrules::kReadAppLogsGrant})
               .join(QLatin1Char(' ')).contains(QStringLiteral("Read(")),
          QStringLiteral("the log grant adds no rule without the log paths"));
}

void testHosts()
{
    check(projects::normalizeHost(QStringLiteral("User@Example.COM:8443/path")) ==
              QLatin1String("example.com"),
          QStringLiteral("normalize host strips userinfo, port and path"));
    check(projects::normalizeHost(QStringLiteral("[::1]:80")) == QLatin1String("::1"),
          QStringLiteral("normalize bracketed IPv6"));
    const QStringList allowed{QStringLiteral("github.com"), QStringLiteral("*.pypi.org")};
    check(projects::hostAllowed(allowed, QStringLiteral("github.com")),
          QStringLiteral("exact host allowed"));
    check(!projects::hostAllowed(allowed, QStringLiteral("api.github.com")),
          QStringLiteral("exact entry does not cover subdomains"));
    check(projects::hostAllowed(allowed, QStringLiteral("files.pypi.org")) &&
              projects::hostAllowed(allowed, QStringLiteral("pypi.org")),
          QStringLiteral("wildcard covers the domain and its subdomains"));
    check(!projects::hostAllowed(allowed, QStringLiteral("evilpypi.org")),
          QStringLiteral("wildcard is a label boundary"));
    check(agentnet::isLocalHost(QStringLiteral("localhost")) &&
              agentnet::isLocalHost(QStringLiteral("127.0.0.53")) &&
              agentnet::isLocalHost(QStringLiteral("[::1]")) &&
              !agentnet::isLocalHost(QStringLiteral("example.com")),
          QStringLiteral("loopback is local"));
}

void testProjectFolders()
{
    QTemporaryDir temp;
    const QDir root(temp.path());
    const QString home = root.filePath(QStringLiteral("home"));
    const QString project = root.filePath(QStringLiteral("home/projects/demo"));
    for (const char *name : {"src", "builds", "data", "config", "ssh", "logs", "state", "cache"})
        QDir().mkpath(QDir(project).filePath(QLatin1String(name)));
    QDir().mkpath(root.filePath(QStringLiteral("outside")));
    QFile::link(root.filePath(QStringLiteral("outside")),
                QDir(project).filePath(QStringLiteral("escape")));
    writeFile(QDir(project).filePath(QStringLiteral("project.json")), "{}");

    const QStringList folders = projects::projectAgentFolders(
        project, {QDir(project).filePath(QStringLiteral("data"))}, home);
    QStringList names;
    for (const QString &path : folders)
        names.append(QFileInfo(path).fileName());
    names.sort();
    check(names == QStringList({QStringLiteral("builds"), QStringLiteral("src")}),
          QStringLiteral("project folders skip private roots and escaping links (%1)")
              .arg(names.join(QLatin1Char(','))));

    check(projects::projectAgentFolders(home, {}, home).isEmpty(),
          QStringLiteral("a project folder that is the home folder grants nothing"));
    check(projects::projectAgentFolders(temp.path(), {}, home).isEmpty(),
          QStringLiteral("a project folder above the home folder grants nothing"));

    agentguard::Config config;
    projects::AgentAccess access;
    access.readOnlyFolders = {root.filePath(QStringLiteral("outside"))};
    projects::applyAgentAccess(config, access, project, {}, home, /*vmActive=*/false);
    if (projects::folderLockSupported()) {
        check(config.projectLocked && config.enforced() && !config.enabled,
              QStringLiteral("default access locks the launch"));
        check(config.projectReadOnly.contains(root.filePath(QStringLiteral("outside"))),
              QStringLiteral("granted read-only folder is carried"));
    }
    projects::applyAgentAccess(config, access, project, {}, home, /*vmActive=*/true);
    check(!config.projectLocked, QStringLiteral("no host lock inside the KVM guest"));
    access.folders = projects::AgentAccess::Folders::Unrestricted;
    projects::applyAgentAccess(config, access, project, {}, home, false);
    check(!config.enforced(), QStringLiteral("unrestricted access leaves the launch alone"));
}

void testRestrictedCommand()
{
    if (!projects::folderLockSupported())
        return;
    QTemporaryDir temp;
    const QDir root(temp.path());
    const QString worktree = root.filePath(QStringLiteral("project/src"));
    const QString builds = root.filePath(QStringLiteral("project/builds"));
    QDir().mkpath(worktree);
    QDir().mkpath(builds);

    agentguard::Config config;
    config.projectLocked = true;
    config.projectReadWrite = {builds};
    config.provider = QStringLiteral("claude-code");
    const agentguard::LaunchCommand launch = agentguard::restrictedCommand(
        QStringLiteral("bash"), {QStringLiteral("-lc"), QStringLiteral("true")}, worktree,
        config, QProcessEnvironment::systemEnvironment());
    check(launch.error.isEmpty() && launch.restricted,
          QStringLiteral("project lock wraps the launch (%1)").arg(launch.error));
    const int policyIndex = launch.arguments.indexOf(QStringLiteral("--policy"));
    const QJsonObject policy =
        policyIndex >= 0
            ? QJsonDocument::fromJson(QByteArray::fromBase64(
                                          launch.arguments.value(policyIndex + 1).toLatin1(),
                                          QByteArray::Base64UrlEncoding))
                  .object()
            : QJsonObject();
    const QStringList readWrite = [&] {
        QStringList list;
        for (const QJsonValue &value : policy.value(QStringLiteral("readWrite")).toArray())
            list.append(value.toString());
        return list;
    }();
    check(readWrite.contains(QFileInfo(worktree).canonicalFilePath()) &&
              readWrite.contains(QFileInfo(builds).canonicalFilePath()),
          QStringLiteral("locked policy writes the worktree and project folders"));
    check(!agentguard::promptInstruction(config).isEmpty(),
          QStringLiteral("locked launch tells the agent about its boundary"));
    // 2026-09-21: `bash -lc` under the lock re-ran /etc/profile (PATH reset to
    // the system default) but could not read ~/.profile, so `exec claude`
    // exited 127. The locked shell keeps the inherited PATH instead.
    const int shellIndex = launch.arguments.indexOf(QStringLiteral("--")) + 1;
    check(launch.arguments.value(shellIndex) == QStringLiteral("bash") &&
              launch.arguments.value(shellIndex + 1) == QStringLiteral("-c") &&
              launch.arguments.value(shellIndex + 2) == QStringLiteral("true"),
          QStringLiteral("a locked launch runs a plain -c shell, not a login "
                         "shell that resets PATH (%1)")
              .arg(launch.arguments.mid(shellIndex).join(QLatin1Char(' '))));
}

void testClassifier()
{
    expectLocal(QStringLiteral("ls -la && make -j8 2>&1 | tail -20"));
    expectLocal(QStringLiteral("git status; git commit -m 'see https://example.com'"));
    expectLocal(QStringLiteral("echo https://example.com"));
    expectLocal(QStringLiteral("curl -s http://localhost:8080/health"));
    expectLocal(QStringLiteral("curl http://127.0.0.1:3000"));
    expectLocal(QStringLiteral("npm test && npm run build"));
    expectLocal(QStringLiteral("uv run pytest"));
    expectLocal(QStringLiteral("python3 -m http.server 8000"));
    expectLocal(QStringLiteral("cargo build --release"));
    expectExternal(QStringLiteral("curl -fsSL https://api.github.com/repos/x/y"),
                   QStringLiteral("api.github.com"));
    // A URL held in a variable set earlier on the line names its real host.
    expectExternal(QStringLiteral("cd /tmp; R='https://app.forkmesh.com/api/raw?path='; "
                                  "curl -s --max-time 20 \"${R}releases%2Frelease.json\" | "
                                  "python3 -c \"import json\""),
                   QStringLiteral("app.forkmesh.com"));
    expectExternal(QStringLiteral("export BASE=https://example.net/v1 && curl $BASE/x"),
                   QStringLiteral("example.net"));
    expectExternal(QStringLiteral("BASE=https://example.net; curl ${BASE}/x"),
                   QStringLiteral("example.net"));
    // An unknown variable is never shown as the host.
    expectExternal(QStringLiteral("curl \"${UNSET}.forkmesh%2Fx\""), QStringLiteral("curl"));
    expectExternal(QStringLiteral("cd /tmp && wget example.org/file.tar.gz"),
                   QStringLiteral("example.org"));
    expectExternal(QStringLiteral("FOO=1 timeout 10 curl https://Example.com"),
                   QStringLiteral("example.com"));
    expectExternal(QStringLiteral("echo $(curl -s https://ifconfig.me)"),
                   QStringLiteral("ifconfig.me"));
    expectExternal(QStringLiteral("ssh deploy@203.0.113.9 uptime"), QStringLiteral("203.0.113.9"));
    expectExternal(QStringLiteral("scp build.tar user@host.example.net:/srv"),
                   QStringLiteral("host.example.net"));
    expectExternal(QStringLiteral("npm install left-pad"), QStringLiteral("registry.npmjs.org"));
    expectExternal(QStringLiteral("pip install requests"), QStringLiteral("pypi.org"));
    expectExternal(QStringLiteral("python3 -m pip install -r requirements.txt"),
                   QStringLiteral("pypi.org"));
    expectExternal(QStringLiteral("gh pr list"), QStringLiteral("github.com"));
    expectExternal(QStringLiteral("git clone https://github.com/org/repo.git"),
                   QStringLiteral("github.com"));
    expectExternal(QStringLiteral("git clone git@gitlab.com:org/repo.git"),
                   QStringLiteral("gitlab.com"));
    expectExternal(QStringLiteral("python3 -c \"import urllib.request; "
                                  "urllib.request.urlopen('https://x.example')\""),
                   QStringLiteral("x.example"));
    expectLocal(QStringLiteral("git clone /srv/mirrors/repo.git"));
    expectLocal(QStringLiteral("gh --version"));

    // Named remotes resolve through the repository's config, pushurl for a push.
    QTemporaryDir temp;
    const QString repo = temp.path();
    writeFile(QDir(repo).filePath(QStringLiteral(".git/config")),
              "[core]\n\tbare = false\n"
              "[remote \"origin\"]\n\turl = https://code.example.com/org/repo\n"
              "\tpushurl = /srv/mirrors/repo.git\n"
              "[remote \"local\"]\n\turl = ../other\n");
    expectExternal(QStringLiteral("git fetch"), QStringLiteral("code.example.com"), repo);
    expectExternal(QStringLiteral("git pull --rebase origin main"),
                   QStringLiteral("code.example.com"), repo);
    expectLocal(QStringLiteral("git push origin HEAD"), repo);
    expectLocal(QStringLiteral("git fetch local"), repo);
    expectExternal(QStringLiteral("git fetch upstream"), QStringLiteral("git remote upstream"),
                   repo);

    // Worktrees reach the shared config through gitdir + commondir.
    const QString worktree = QDir(temp.path()).filePath(QStringLiteral("wt"));
    const QString worktreeGitDir = QDir(repo).filePath(QStringLiteral(".git/worktrees/wt"));
    writeFile(QDir(worktree).filePath(QStringLiteral(".git")),
              ("gitdir: " + worktreeGitDir + "\n").toUtf8());
    writeFile(QDir(worktreeGitDir).filePath(QStringLiteral("commondir")), "../..\n");
    expectExternal(QStringLiteral("git -C . fetch origin"), QStringLiteral("code.example.com"),
                   worktree);

    const agentnet::ExternalCall fetch = agentnet::classifyToolCall(
        QStringLiteral("WebFetch"),
        QJsonObject{{QStringLiteral("url"), QStringLiteral("https://docs.example.org/a")}}, {});
    check(fetch.external && fetch.target == QLatin1String("docs.example.org"),
          QStringLiteral("WebFetch is external to its host"));
    const agentnet::ExternalCall search = agentnet::classifyToolCall(
        QStringLiteral("WebSearch"), QJsonObject{{QStringLiteral("query"), QStringLiteral("x")}},
        {});
    check(search.external, QStringLiteral("WebSearch is external"));
    check(!agentnet::classifyToolCall(QStringLiteral("Read"),
                                      QJsonObject{{QStringLiteral("file_path"),
                                                   QStringLiteral("/etc/hosts")}},
                                      {})
               .external,
          QStringLiteral("Read is not a network tool"));
}

void expectFailure(const QString &output, agentnet::AccessFailure::Kind kind,
                   const QString &target, const QString &command = {},
                   const QString &tool = QStringLiteral("Bash"))
{
    const agentnet::AccessFailure failure = agentnet::detectAccessFailure(
        tool, QJsonObject{{QStringLiteral("command"), command}}, output);
    check(failure.kind == kind && failure.target == target,
          QStringLiteral("failure: %1 -> %2 %3 (got %4 %5)")
              .arg(output.left(80), agentnet::accessFailureKindKey(kind), target,
                   agentnet::accessFailureKindKey(failure.kind), failure.target));
}

void testAccessFailures()
{
    using Kind = agentnet::AccessFailure::Kind;
    expectFailure(QStringLiteral("curl: (6) Could not resolve host: cdn.jsdelivr.net"),
                  Kind::Resolve, QStringLiteral("cdn.jsdelivr.net"));
    expectFailure(QStringLiteral("fatal: unable to access 'https://github.com/a/b.git/': "
                                 "Could not resolve host: github.com"),
                  Kind::Resolve, QStringLiteral("github.com"));
    expectFailure(QStringLiteral("ssh: Could not resolve hostname git.example.org: "
                                 "Temporary failure in name resolution"),
                  Kind::Resolve, QStringLiteral("git.example.org"));
    expectFailure(QStringLiteral("Error: getaddrinfo ENOTFOUND registry.npmjs.org"),
                  Kind::Resolve, QStringLiteral("registry.npmjs.org"));
    expectFailure(QStringLiteral("dial tcp: lookup proxy.golang.org on 127.0.0.53:53: "
                                 "server misbehaving"),
                  Kind::Resolve, QStringLiteral("proxy.golang.org"));
    expectFailure(QStringLiteral("urllib3.exceptions.NameResolutionError: <urllib3.connection."
                                 "HTTPSConnection object>: Failed to resolve 'pypi.org' "
                                 "([Errno -3] Temporary failure in name resolution)"),
                  Kind::Resolve, QStringLiteral("pypi.org"));
    // No host in the message: the command names it.
    expectFailure(QStringLiteral("urlopen error [Errno -3] Temporary failure in name resolution"),
                  Kind::Resolve, QStringLiteral("cdn.jsdelivr.net"),
                  QStringLiteral("python3 fetch.py https://cdn.jsdelivr.net/pyodide/x.tgz"));
    expectFailure(QStringLiteral("curl: (28) Failed to connect to example.com port 443 after "
                                 "130001 ms: Connection timed out"),
                  Kind::Connect, QStringLiteral("example.com"));
    expectFailure(QStringLiteral("curl: (7) Failed to connect to localhost port 8080: "
                                 "Connection timed out"),
                  Kind::None, QString());
    expectFailure(QStringLiteral("curl: (7) Failed to connect to example.com port 443: "
                                 "Connection refused"),
                  Kind::None, QString());
    expectFailure(QStringLiteral("cat: /srv/shared/notes.txt: Permission denied"),
                  Kind::Folder, QStringLiteral("/srv/shared/notes.txt"));
    expectFailure(QStringLiteral("mkdir: cannot create directory ‘/opt/cache/x’: "
                                 "Permission denied"),
                  Kind::Folder, QStringLiteral("/opt/cache/x"));
    expectFailure(QStringLiteral("EACCES: permission denied, open '/home/u/other/a.md'"),
                  Kind::Folder, QStringLiteral("/home/u/other/a.md"), QString(),
                  QStringLiteral("Read"));
    expectFailure(QStringLiteral("PermissionError: [Errno 13] Permission denied: '/data/x.db'"),
                  Kind::Folder, QStringLiteral("/data/x.db"));
    expectFailure(QStringLiteral("bash: line 1: foo: command not found\nall tests passed"),
                  Kind::None, QString());
    expectFailure(QStringLiteral("tests/test_agent_access.cpp:348:    expectFailure("
                                 "QStringLiteral(\"curl: (6) Could not resolve host: "
                                 "cdn.jsdelivr.net\"),"),
                  Kind::None, QString(), QStringLiteral("make check 2>&1 | tail -40"));
    expectFailure(QStringLiteral("docs/a.md-12-  Could not resolve host: cdn.jsdelivr.net"),
                  Kind::None, QString(), QStringLiteral("bash run.sh"));
    expectFailure(QStringLiteral("   348\tcurl: (6) Could not resolve host: cdn.jsdelivr.net"),
                  Kind::None, QString(), QStringLiteral("bash run.sh"));
    expectFailure(QStringLiteral("    expectFailure(QStringLiteral(\"curl: (6) Could not "
                                 "resolve host: cdn.jsdelivr.net\"),"),
                  Kind::None, QString(),
                  QStringLiteral("cd desktop && sed -n 340,360p tests/test_agent_access.cpp"));
    expectFailure(QStringLiteral("app.forkmesh.com, \"Host app.forkmesh.com not found\" "
                                 "(DNS denied) and"),
                  Kind::None, QString(),
                  QStringLiteral("git log -5 --format=%B 2>&1 | grep -i dns"));
    expectFailure(QStringLiteral("Could not resolve host: cdn.jsdelivr.net"), Kind::None,
                  QString(), QString(), QStringLiteral("Grep"));
    expectFailure(QStringLiteral("2026-09-27 10:00:01 curl: (6) Could not resolve host: "
                                 "cdn.jsdelivr.net"),
                  Kind::Resolve, QStringLiteral("cdn.jsdelivr.net"),
                  QStringLiteral("./fetch.sh 2>&1 | tee fetch.log"));
    expectFailure(QStringLiteral("curl: (6) Could not resolve host: cdn.jsdelivr.net"),
                  Kind::Resolve, QStringLiteral("cdn.jsdelivr.net"),
                  QStringLiteral("curl -fsSL https://cdn.jsdelivr.net/x | grep y"));
    expectFailure(QStringLiteral("getaddrinfo EAI_AGAIN developer.apple.com"), Kind::Resolve,
                  QStringLiteral("developer.apple.com"), QStringLiteral("node fetch.mjs"));
    check(agentnet::commandOnlyReadsText(
              QStringLiteral("cd desktop && grep -rn resolve src 2>/dev/null | head -20")) &&
              agentnet::commandOnlyReadsText(QStringLiteral("git -C repo show HEAD:a.cpp")) &&
              !agentnet::commandOnlyReadsText(QStringLiteral("cat urls | xargs curl -sI")) &&
              !agentnet::commandOnlyReadsText(QStringLiteral("git fetch origin")) &&
              !agentnet::commandOnlyReadsText(QString()),
          QStringLiteral("text readers are told apart from commands that can reach a host"));
    expectFailure(QStringLiteral(
                      "tests/test_agent_access.cpp:348:    expectFailure(QStringLiteral("
                      "\"curl: (6) Could not resolve host: cdn.jsdelivr.net\"),"),
                  Kind::None, QString());
    expectFailure(QStringLiteral(
                      "FAIL: failure: curl: (6) Could not resolve host: cdn.jsdelivr.net\\\" "
                      "-> resolve"),
                  Kind::None, QString());
    expectFailure(QStringLiteral(
                      "<code>desktop dialog [app] Agent network access — Agent #459 on "
                      "forkmesh could not resolve &lt;b&gt;cdn.jsdelivr.net&lt;/b&gt;. "
                      "Could not resolve host: cdn.jsdelivr.net&amp;quot;),&lt;/code&gt;"),
                  Kind::None, QString());
    expectFailure(QStringLiteral("curl: (6) Could not resolve host: cdn.jsdelivr.net"),
                  Kind::None, QString(), QString(), QStringLiteral("Read"));
    expectFailure(QStringLiteral("  348→    {Kind::Resolve, rx(QStringLiteral(R\"(dial tcp: "
                                 "lookup ([^\\s:]+))\"))},"),
                  Kind::None, QString(), QString(), QStringLiteral("Grep"));
    expectFailure(QStringLiteral("Could not resolve host: bad..host\n"
                                 "curl: (6) Could not resolve host: files.example.com"),
                  Kind::Resolve, QStringLiteral("files.example.com"));
    expectFailure(QStringLiteral("Error: connect ETIMEDOUT 2001:db8::7:443"),
                  Kind::Connect, QStringLiteral("2001:db8::7"));

    const agentnet::AccessFailure made = agentnet::detectAccessFailure(
        QStringLiteral("Bash"), {},
        QStringLiteral("mkdir: cannot create directory ‘/opt/cache/x’: Permission denied"));
    check(made.write, QStringLiteral("mkdir failure asks for write access"));
    const agentnet::AccessFailure read = agentnet::detectAccessFailure(
        QStringLiteral("Bash"), {}, QStringLiteral("cat: /srv/a: Permission denied"));
    check(!read.write, QStringLiteral("cat failure asks for read access"));
}

void testHookSettings()
{
    const QString settings = agentnet::claudeHookSettings();
    check(!settings.contains(QLatin1Char('\'')),
          QStringLiteral("hook settings hold no single quote"));
    const QJsonObject object = QJsonDocument::fromJson(settings.toUtf8()).object();
    check(object.value(QStringLiteral("hooks"))
                  .toObject()
                  .value(QStringLiteral("PreToolUse"))
                  .toArray()
                  .size() == 1,
          QStringLiteral("hook settings install one PreToolUse matcher"));
    check(object.value(QStringLiteral("hooks"))
                  .toObject()
                  .value(QStringLiteral("PostToolUseFailure"))
                  .toArray()
                  .size() == 1,
          QStringLiteral("hook settings report failed tool calls"));
    const QJsonObject ungated =
        QJsonDocument::fromJson(agentnet::claudeHookSettings(false).toUtf8()).object();
    check(!ungated.value(QStringLiteral("hooks")).toObject().contains(QStringLiteral("PreToolUse")) &&
              ungated.value(QStringLiteral("hooks")).toObject().contains(QStringLiteral("PostToolUse")),
          QStringLiteral("an ungated project still reports failures, without the gate"));
    const QString command = agentnet::withClaudeHookSettings(
        QStringLiteral("claude -p \"$(cat prompt.md)\" --dangerously-skip-permissions"));
    check(command.startsWith(QStringLiteral("claude --settings '{")),
          QStringLiteral("settings go right after the claude word"));
    check(agentnet::withClaudeHookSettings(QStringLiteral("codex exec x")) ==
              QLatin1String("codex exec x"),
          QStringLiteral("non-claude commands are left alone"));
}

// Runs the real hook script against the real server, as Claude Code would.
// With `lockedIn`, the hook runs inside a project lock on that folder, the
// way it does under a locked agent.
QString runHook(const QString &socketPath, const QJsonObject &event, int *exitCode,
                const QString &lockedIn = QString(),
                const QHash<QString, QString> &extra = {})
{
    QProcess hook;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        env.insert(it.key(), it.value());
    env.insert(agentnet::SocketEnv, socketPath);
    env.insert(agentnet::SessionEnv, QStringLiteral("s1"));
    env.insert(agentnet::LabelEnv, QStringLiteral("Agent 7"));
    hook.setProcessEnvironment(env);
    QString program = QStringLiteral("python3");
    QStringList arguments{QStringLiteral("-c"), agentnet::hookScript()};
    if (!lockedIn.isEmpty()) {
        agentguard::Config config;
        config.projectLocked = true;
        const agentguard::LaunchCommand launch =
            agentguard::restrictedCommand(program, arguments, lockedIn, config, env);
        program = launch.program;
        arguments = launch.arguments;
    }
    hook.start(program, arguments);
    hook.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    hook.closeWriteChannel();
    QElapsedTimer timer;
    timer.start();
    while (hook.state() != QProcess::NotRunning && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        hook.waitForFinished(20);
    }
    *exitCode = hook.exitCode();
    return QString::fromUtf8(hook.readAllStandardOutput());
}

void testServer()
{
    if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty())
        return;
    agentnet::Server server;
    QString error;
    check(server.listen(&error), QStringLiteral("gate server listens (%1)").arg(error));

    QList<agentnet::Request> seen;
    agentnet::Server::Decision next = agentnet::Server::Decision::Deny;
    server.setRequestHandler([&](const agentnet::Request &request) {
        seen.append(request);
        server.answer(request.id, next, QStringLiteral("Blocked by test."));
    });

    int exitCode = -1;
    QString output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("Bash")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("make test")}}},
                    {QStringLiteral("cwd"), QDir::tempPath()}},
        &exitCode);
    check(exitCode == 0 && output.trimmed().isEmpty() && seen.isEmpty(),
          QStringLiteral("local command passes without asking (%1)").arg(output));

    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("Bash")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("rm -rf build")}}},
                    {QStringLiteral("cwd"), QDir::tempPath()}},
        &exitCode);
    check(seen.size() == 1 && seen.first().risk.destructive == QLatin1String("recursive delete") &&
              output.contains(QLatin1String("\"deny\"")),
          QStringLiteral("a destructive local command reaches the handler (%1)").arg(output));
    seen.clear();

    QTemporaryDir agents;
    writeFile(QDir(agents.path()).filePath(QStringLiteral(".gitignore")),
              "# ForkMesh agent worktrees. Never part of the project.\n");
    const QString scratch = QDir(agents.path()).filePath(QStringLiteral(".scratch/7-run"));
    QDir().mkpath(scratch + QStringLiteral("/desktop"));
    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("Bash")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("command"),
                                  QStringLiteral("S=$FORKMESH_AGENT_SCRATCH/desktop && "
                                                 "rm -rf $S/marketplace && echo $GATE_TEST_TOKEN")}}},
                    {QStringLiteral("cwd"), QDir::tempPath()}},
        &exitCode, QString(),
        {{commandvars::ScratchName, scratch},
         {QStringLiteral("GATE_TEST_TOKEN"), QStringLiteral("s3cret")}});
    check(seen.size() == 1 && seen.first().risk.destructive == QLatin1String("scratch delete") &&
              seen.first().environment.sent &&
              seen.first().environment.values.value(commandvars::ScratchName) == scratch &&
              seen.first().environment.hidden.contains(QStringLiteral("GATE_TEST_TOKEN")) &&
              !seen.first().environment.values.contains(QStringLiteral("GATE_TEST_TOKEN")),
          QStringLiteral("the hook sends the variables a command uses, secrets withheld (%1)")
              .arg(seen.isEmpty() ? QString() : seen.first().risk.destructive));
    seen.clear();

    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("WebFetch")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.com")}}}},
        &exitCode);
    const QJsonObject denied = QJsonDocument::fromJson(output.toUtf8()).object();
    check(seen.size() == 1 && seen.first().call.target == QLatin1String("example.com") &&
              seen.first().label == QLatin1String("Agent 7") &&
              seen.first().session == QLatin1String("s1"),
          QStringLiteral("external call reaches the handler with its session"));
    check(denied.value(QStringLiteral("hookSpecificOutput"))
                  .toObject()
                  .value(QStringLiteral("permissionDecision"))
                  .toString() == QLatin1String("deny"),
          QStringLiteral("denied call returns a PreToolUse deny (%1)").arg(output));

    next = agentnet::Server::Decision::Allow;
    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("WebSearch")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("query"), QStringLiteral("qt")}}}},
        &exitCode);
    check(seen.size() == 2 && output.trimmed().isEmpty(),
          QStringLiteral("allowed call proceeds silently (%1)").arg(output));

    if (projects::folderLockSupported()) {
        // The socket is outside the locked folders: connecting to it is not a
        // filesystem access Landlock stops, so a locked agent can still ask.
        QTemporaryDir locked;
        next = agentnet::Server::Decision::Deny;
        output = runHook(server.socketPath(),
                         QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("Bash")},
                                     {QStringLiteral("tool_input"),
                                      QJsonObject{{QStringLiteral("command"),
                                                   QStringLiteral("curl https://example.net")}}}},
                         &exitCode, locked.path());
        check(seen.size() == 3 && output.contains(QLatin1String("Blocked by test.")),
              QStringLiteral("a hook inside the project lock reaches the gate (%1)")
                  .arg(output.left(300)));
    }

    // Post-tool reports: a failure reaches the failure handler, and its
    // answer comes back as the hook's additional context.
    QList<agentnet::Request> failures;
    server.setFailureHandler([&](const agentnet::Request &request) {
        failures.append(request);
        server.answerFailure(request.id, QStringLiteral("Granted by test."));
    });
    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("hook_event_name"), QStringLiteral("PostToolUseFailure")},
                    {QStringLiteral("tool_name"), QStringLiteral("Bash")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("command"),
                                  QStringLiteral("curl -O https://cdn.example.org/a.tgz")}}},
                    {QStringLiteral("error"),
                     QStringLiteral("Exit code 6\ncurl: (6) Could not resolve host: "
                                    "cdn.example.org")}},
        &exitCode);
    const QJsonObject specific = QJsonDocument::fromJson(output.toUtf8())
                                     .object()
                                     .value(QStringLiteral("hookSpecificOutput"))
                                     .toObject();
    check(failures.size() == 1 && failures.first().failure.target == QLatin1String("cdn.example.org") &&
              failures.first().session == QLatin1String("s1"),
          QStringLiteral("a failed lookup reaches the failure handler"));
    check(specific.value(QStringLiteral("hookEventName")).toString() ==
                  QLatin1String("PostToolUseFailure") &&
              specific.value(QStringLiteral("additionalContext")).toString() ==
                  QLatin1String("Granted by test."),
          QStringLiteral("the answer returns as additional context (%1)").arg(output));
    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("hook_event_name"), QStringLiteral("PostToolUse")},
                    {QStringLiteral("tool_name"), QStringLiteral("Bash")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}}},
                    {QStringLiteral("tool_response"),
                     QJsonObject{{QStringLiteral("stdout"), QStringLiteral("a\nb")},
                                 {QStringLiteral("stderr"), QString()}}}},
        &exitCode);
    check(failures.size() == 1 && output.trimmed().isEmpty(),
          QStringLiteral("an ordinary output passes without a report (%1)").arg(output));
    output = runHook(
        server.socketPath(),
        QJsonObject{{QStringLiteral("hook_event_name"), QStringLiteral("PostToolUse")},
                    {QStringLiteral("tool_name"), QStringLiteral("Bash")},
                    {QStringLiteral("tool_input"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("make")}}},
                    {QStringLiteral("tool_response"),
                     QJsonObject{{QStringLiteral("stdout"), QString()},
                                 {QStringLiteral("stderr"),
                                  QStringLiteral("make: *** No rule to make target 'x'. "
                                                 "Stop. not found")}}}},
        &exitCode);
    check(failures.size() == 1 && output.trimmed().isEmpty(),
          QStringLiteral("a hinted output with no access failure is answered at once"));

    output = runHook(QStringLiteral("/nonexistent/forkmesh-gate"),
                     QJsonObject{{QStringLiteral("hook_event_name"),
                                  QStringLiteral("PostToolUseFailure")},
                                 {QStringLiteral("tool_name"), QStringLiteral("Bash")},
                                 {QStringLiteral("error"),
                                  QStringLiteral("Could not resolve host: a.example")}},
                     &exitCode);
    check(exitCode == 0 && output.trimmed().isEmpty(),
          QStringLiteral("an unreachable gate lets failure reports pass"));

    output = runHook(QStringLiteral("/nonexistent/forkmesh-gate"),
                     QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("Bash")},
                                 {QStringLiteral("tool_input"),
                                  QJsonObject{{QStringLiteral("command"),
                                               QStringLiteral("ls")}}}},
                     &exitCode);
    check(output.contains(QLatin1String("\"deny\"")),
          QStringLiteral("an unreachable gate fails closed"));
}

} // namespace

void testAlwaysAllowedDestructive()
{
    QTemporaryDir temp;
    const QString project = temp.path();
    projects::AgentAccess access = projects::loadAgentAccess(project);
    access.allowedDestructive = {QStringLiteral("stash drop")};
    QString error;
    check(projects::saveAgentAccess(project, access, &error) &&
              projects::loadAgentAccess(project).allowedDestructive ==
                  QStringList{QStringLiteral("stash drop")} &&
              projects::loadAgentAccess(project).followGlobal,
          QStringLiteral("a project's always-allowed destructive kinds round trip"));

    const QString rulesPath = projects::globalRulesPath(QDir(temp.path()).filePath("home"));
    projects::GlobalAgentRules rules;
    rules.defaults.allowedDestructive = {QStringLiteral("branch delete")};
    check(projects::saveGlobalRules(rulesPath, rules, &error) &&
              projects::loadGlobalRules(rulesPath).defaults.allowedDestructive ==
                  QStringList{QStringLiteral("branch delete")},
          QStringLiteral("global always-allowed destructive kinds round trip"));
    const projects::AgentAccess merged =
        projects::effectiveAgentAccess(projects::loadAgentAccess(project),
                                       projects::loadGlobalRules(rulesPath));
    check(merged.allowedDestructive.contains(QStringLiteral("stash drop")) &&
              merged.allowedDestructive.contains(QStringLiteral("branch delete")),
          QStringLiteral("project and global always-allowed kinds merge"));

    using agentrules::Verdict;
    agentrules::RunRules run;
    run.access = merged;
    const QSet<QString> none;
    check(agentrules::decideCommand(run, QStringLiteral("git stash drop"), QDir::tempPath(), none)
                  .verdict == Verdict::Run,
          QStringLiteral("an always-allowed kind runs without asking"));
    check(agentrules::decideCommand(run, QStringLiteral("git reset --hard"), QDir::tempPath(),
                                    none)
                  .verdict == Verdict::Ask,
          QStringLiteral("other destructive kinds still ask"));
    run.destructive = projects::GlobalAgentRules::Destructive::Block;
    check(agentrules::decideCommand(run, QStringLiteral("git stash drop"), QDir::tempPath(), none)
                  .verdict == Verdict::Deny,
          QStringLiteral("blocking destructive commands overrides always-allowed kinds"));
}

void testScratchDelete()
{
    using Lean = destructive::Review::Lean;
    QTemporaryDir agents;
    writeFile(QDir(agents.path()).filePath(QStringLiteral(".gitignore")),
              "# ForkMesh agent worktrees. Never part of the project.\n");
    const QString scratch = QDir(agents.path()).filePath(QStringLiteral(".scratch/1705-run"));
    QDir().mkpath(scratch + QStringLiteral("/desktop"));
    QDir().mkpath(agents.path() + QStringLiteral("/outside"));
    QFile::link(agents.path() + QStringLiteral("/outside"), scratch + QStringLiteral("/away"));

    commandvars::Environment environment;
    environment.sent = true;
    environment.values.insert(commandvars::ScratchName, scratch);
    check(commandvars::trustedScratch(environment) == scratch,
          QStringLiteral("a scratch folder in the agent layout is trusted"));
    commandvars::Environment elsewhere;
    elsewhere.values.insert(commandvars::ScratchName, agents.path() + QStringLiteral("/outside"));
    check(commandvars::trustedScratch(elsewhere).isEmpty(),
          QStringLiteral("a scratch variable outside the agent layout is not trusted"));
    elsewhere.values.insert(commandvars::ScratchName, scratch + QStringLiteral("/../1705-run"));
    check(commandvars::trustedScratch(elsewhere).isEmpty(),
          QStringLiteral("an unclean scratch path is not trusted"));

    const QString example = QStringLiteral(
        "S=$FORKMESH_AGENT_SCRATCH/desktop && rm -rf $S/marketplace && cp -r marketplace "
        "$S/marketplace && touch $S/marketplace/packages/status-page/content/web/page.js && "
        "python3 $S/marketplace/tools/generate_desktop_packages.py --catalog "
        "$S/marketplace/catalog.json --qrc $S/gen/marketplace.qrc && grep -c \"status-check\" "
        "$S/gen/marketplace.qrc && rcc $S/gen/marketplace.qrc -o $S/gen/qrc_marketplace.cpp && "
        "ls -la $S/gen");
    const agentnet::CommandRisk risk = agentnet::classifyCommandRisk(example, scratch);
    check(risk.destructive == QLatin1String("scratch delete") &&
              example.mid(risk.matchStart, risk.matchLength) ==
                  QLatin1String("rm -rf $S/marketplace"),
          QStringLiteral("a delete proven inside the scratch folder is a scratch delete (%1)")
              .arg(risk.destructive));
    check(agentnet::classifyCommandRisk(example).destructive ==
              QLatin1String("recursive delete"),
          QStringLiteral("without a trusted scratch folder it stays a recursive delete"));

    const auto kind = [&scratch](const QString &command) {
        return agentnet::classifyCommandRisk(command, scratch).destructive;
    };
    const QStringList contained{
        QStringLiteral("rm -rf \"$FORKMESH_AGENT_SCRATCH/build\""),
        QStringLiteral("rm -rf ${FORKMESH_AGENT_SCRATCH}/a/b"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/*"),
        QStringLiteral("export S=$FORKMESH_AGENT_SCRATCH/x; mkdir -p $S && rm -r -- $S/y 2>/dev/null"),
        QStringLiteral("S=$FORKMESH_AGENT_SCRATCH; T=$S/desktop\nrm -rf $T/new $S/old"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/x && ln -s / $FORKMESH_AGENT_SCRATCH/x")};
    for (const QString &command : contained)
        check(kind(command) == QLatin1String("scratch delete"),
              QStringLiteral("contained: %1 (got %2)").arg(command, kind(command)));
    const QStringList uncontained{
        QStringLiteral("S=$OTHER/desktop && rm -rf $S/m"),
        QStringLiteral("rm -rf $S/m"),
        QStringLiteral("ls nothing && S=$FORKMESH_AGENT_SCRATCH/a; rm -rf $S/m"),
        QStringLiteral("S=$FORKMESH_AGENT_SCRATCH/a || true; rm -rf $S/m"),
        QStringLiteral("S=$FORKMESH_AGENT_SCRATCH/a; S=/; rm -rf $S/m"),
        QStringLiteral("S=$FORKMESH_AGENT_SCRATCH/a | cat; rm -rf $S/m"),
        QStringLiteral("ln -s / $FORKMESH_AGENT_SCRATCH/x && rm -rf $FORKMESH_AGENT_SCRATCH/x/etc"),
        QStringLiteral("unset S; rm -rf $FORKMESH_AGENT_SCRATCH/x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/../x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/.*"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/*/x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/away/x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a ~/x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a build"),
        QStringLiteral("rm -rf $(echo $FORKMESH_AGENT_SCRATCH)/a"),
        QStringLiteral("rm -rf '$FORKMESH_AGENT_SCRATCH/a'"),
        QStringLiteral("IFS=/; rm -rf $FORKMESH_AGENT_SCRATCH/a"),
        QStringLiteral("D=\"$FORKMESH_AGENT_SCRATCH/a b\"; rm -rf $D"),
        QStringLiteral("sudo rm -rf $FORKMESH_AGENT_SCRATCH/a"),
        QStringLiteral("FORKMESH_AGENT_SCRATCH=/home rm -rf $FORKMESH_AGENT_SCRATCH/a; "
                       "rm -rf $FORKMESH_AGENT_SCRATCH/b")};
    for (const QString &command : uncontained)
        check(kind(command) == QLatin1String("recursive delete"),
              QStringLiteral("not contained: %1 (got %2)").arg(command, kind(command)));
    check(kind(QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a && git reset --hard")) ==
              QLatin1String("discard changes"),
          QStringLiteral("another destructive part outranks a scratch delete"));

    agentrules::RunRules run;
    run.access.allowedDestructive = {QStringLiteral("scratch delete")};
    check(agentrules::decide(run, {}, risk, {}).verdict == agentrules::Verdict::Run,
          QStringLiteral("an always-allowed scratch delete runs without asking"));
    check(agentrules::decide(run, {}, agentnet::classifyCommandRisk(example), {}).verdict ==
              agentrules::Verdict::Ask,
          QStringLiteral("allowing scratch deletes does not allow other recursive deletes"));
    check(destructive::kinds().contains(QStringLiteral("scratch delete")) &&
              !destructive::kindExample(QStringLiteral("scratch delete")).isEmpty(),
          QStringLiteral("scratch delete is a kind the settings list"));

    environment.values.insert(QStringLiteral("HOME"), QDir::homePath());
    const QList<commandvars::Variable> variables = commandvars::variables(
        QStringLiteral("S=$FORKMESH_AGENT_SCRATCH/desktop && rm -rf $S/m $MISSING"),
        environment);
    check(variables.size() == 3 && variables.at(0).name == commandvars::ScratchName &&
              variables.at(0).source == commandvars::Variable::Source::Environment &&
              variables.at(1).name == QLatin1String("S") &&
              variables.at(1).source == commandvars::Variable::Source::Command &&
              variables.at(1).assigned == QLatin1String("$FORKMESH_AGENT_SCRATCH/desktop") &&
              variables.at(1).value == scratch + QStringLiteral("/desktop") &&
              variables.at(2).name == QLatin1String("MISSING") &&
              variables.at(2).source == commandvars::Variable::Source::Unset,
          QStringLiteral("the dialog lists each variable with where its value comes from"));
    const QHash<QString, QString> values =
        commandvars::valuesAt(example, environment, risk.matchStart);
    check(values.value(QStringLiteral("S")) == scratch + QStringLiteral("/desktop"),
          QStringLiteral("values are those in effect where the delete runs"));
    const destructive::Review review = destructive::review(
        example, risk.destructive, risk.matchStart, risk.matchLength, values, scratch);
    check(review.lean == Lean::Allow && review.forDeny.isEmpty() &&
              review.what.contains(scratch + QStringLiteral("/desktop/marketplace")) &&
              review.forAllow.join(' ').contains(QLatin1String("scratch folder")),
          QStringLiteral("a scratch delete review shows the real path and leans allow (%1)")
              .arg(review.forDeny.join(QStringLiteral(" | "))));
}

void testDeleteRules()
{
    using agentrules::Verdict;
    QTemporaryDir temp;
    const QString agents = QDir(temp.path()).filePath(QStringLiteral(".worktrees"));
    writeFile(QDir(agents).filePath(QStringLiteral(".gitignore")),
              "# ForkMesh agent worktrees. Never part of the project.\n");
    const QString scratch = QDir(agents).filePath(QStringLiteral(".scratch/1715-run"));
    QDir().mkpath(scratch + QStringLiteral("/mainbase"));
    QDir().mkpath(temp.path() + QStringLiteral("/outside"));
    QFile::link(temp.path() + QStringLiteral("/outside"), scratch + QStringLiteral("/away"));
    commandvars::Environment environment;
    environment.sent = true;
    environment.values.insert(commandvars::ScratchName, scratch);
    environment.values.insert(QStringLiteral("HOME"), QDir::homePath());

    const QString screenshot = QStringLiteral(
        "cd /x/app/host && $FORKMESH_AGENT_SCRATCH/venv/bin/pip install -q -r requirements.txt "
        "2>&1 | tail -1; (time $FORKMESH_AGENT_SCRATCH/venv/bin/python -m forkmesh_host build) > "
        "$FORKMESH_AGENT_SCRATCH/build-merge.log 2>&1; echo build=$?; tail -4 "
        "$FORKMESH_AGENT_SCRATCH/build-merge.log; cd ../.. && git status --short | grep -v "
        "\"^[MADR] \" | head; python3 app/tools/site_templates.py --check && echo templates-ok; "
        "rm -rf $FORKMESH_AGENT_SCRATCH/mainbase && mkdir -p $FORKMESH_AGENT_SCRATCH/mainbase && "
        "git archive main | tar -x -C $FORKMESH_AGENT_SCRATCH/mainbase && echo base-ready");
    const agentnet::CommandRisk risk = agentnet::classifyCommandRisk(screenshot, environment);
    check(risk.destructive == QLatin1String("recursive delete") && risk.onlyRemoves &&
              risk.removedPaths == QStringList{scratch + QStringLiteral("/mainbase")},
          QStringLiteral("a delete in a long command resolves to its real path (%1)")
              .arg(risk.removedPaths.join(QLatin1Char(' '))));

    agentrules::RunRules run;
    check(agentrules::decide(run, {}, risk, {}).verdict == Verdict::Ask,
          QStringLiteral("without a rule the delete asks"));
    run.access.allowedDeletePaths = {QStringLiteral("~/elsewhere/**")};
    check(agentrules::decide(run, {}, risk, {}).verdict == Verdict::Ask,
          QStringLiteral("a rule for another folder does not cover it"));
    for (const QString &pattern :
         {QStringLiteral("$FORKMESH_AGENT_SCRATCH/**"),
          QStringLiteral("${FORKMESH_AGENT_SCRATCH}/*"),
          agents + QStringLiteral("/.scratch/*/**"), agents + QStringLiteral("/*/**")}) {
        run.access.allowedDeletePaths = {pattern};
        check(agentrules::decide(run, {}, risk, {}).verdict == Verdict::Run,
              QStringLiteral("rule %1 lets the delete run").arg(pattern));
    }
    run.destructive = projects::GlobalAgentRules::Destructive::Block;
    check(agentrules::decide(run, {}, risk, {}).verdict == Verdict::Deny,
          QStringLiteral("blocked destructive commands stay blocked under a rule"));
    run.destructive = projects::GlobalAgentRules::Destructive::Ask;
    run.access.allowedDeletePaths = {QStringLiteral("$FORKMESH_AGENT_SCRATCH/**")};

    const auto verdict = [&](const QString &command) {
        return agentrules::decide(run, {}, agentnet::classifyCommandRisk(command, environment), {})
            .verdict;
    };
    const QStringList covered{
        QStringLiteral("rm -rf \"$FORKMESH_AGENT_SCRATCH/build dir\""),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/*"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/mainbase/"),
        QStringLiteral("S=$FORKMESH_AGENT_SCRATCH/a && rm -rf $S/b $S/c"),
        QStringLiteral("rm -rf %1/old && git status").arg(scratch),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a || true")};
    for (const QString &command : covered)
        check(verdict(command) == Verdict::Run, QStringLiteral("covered: %1").arg(command));
    const QStringList uncovered{
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/x$(echo /..)"),
        QStringLiteral("rm -rf \"$FORKMESH_AGENT_SCRATCH/x$(echo /..)\""),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/x`echo /..`"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/{a,..}"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/../x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/away/x"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a $OTHER"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a build"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a && git reset --hard"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/a; find /x -delete"),
        QStringLiteral("sudo rm -rf $FORKMESH_AGENT_SCRATCH/a"),
        QStringLiteral("ln -s / $FORKMESH_AGENT_SCRATCH/l; rm -rf $FORKMESH_AGENT_SCRATCH/l/etc"),
        QStringLiteral("FORKMESH_AGENT_SCRATCH=/; rm -rf $FORKMESH_AGENT_SCRATCH/etc"),
        QStringLiteral("(FORKMESH_AGENT_SCRATCH=/x); rm -rf $FORKMESH_AGENT_SCRATCH/etc"),
        QStringLiteral("read FORKMESH_AGENT_SCRATCH; rm -rf $FORKMESH_AGENT_SCRATCH/etc"),
        QStringLiteral("eval \"$X\"; rm -rf $FORKMESH_AGENT_SCRATCH/a"),
        QStringLiteral("IFS=/; rm -rf $FORKMESH_AGENT_SCRATCH/a"),
        QStringLiteral("rm() { echo; }; rm -rf $FORKMESH_AGENT_SCRATCH/a"),
        QStringLiteral("rm -rf $FORKMESH_AGENT_SCRATCH/$1"),
        QStringLiteral("rm -rf ~/x")};
    for (const QString &command : uncovered)
        check(verdict(command) == Verdict::Ask, QStringLiteral("not covered: %1").arg(command));

    const QString home = QDir::cleanPath(QDir::homePath());
    run.access.allowedDeletePaths = {QStringLiteral("~/fm-delete-rule-test/**")};
    check(verdict(QStringLiteral("rm -rf ~/fm-delete-rule-test/build")) == Verdict::Run &&
              agentrules::decideCommand(run, QStringLiteral("rm -rf ~/fm-delete-rule-test/b"),
                                        QDir::tempPath(), {})
                      .verdict == Verdict::Run,
          QStringLiteral("~ in a target and a pattern is the home folder"));
    commandvars::Environment otherHome = environment;
    otherHome.values.insert(QStringLiteral("HOME"), QStringLiteral("/somewhere"));
    check(agentnet::classifyCommandRisk(QStringLiteral("rm -rf ~/fm-delete-rule-test/b"),
                                        otherHome)
              .removedPaths.isEmpty(),
          QStringLiteral("an agent HOME that is not the user's home is not trusted"));

    check(commandvars::deletePatternMatches(QStringLiteral("~/a/*/b"), home + "/a/x/b", {}) &&
              !commandvars::deletePatternMatches(QStringLiteral("~/a/*/b"), home + "/a/x/y/b", {}) &&
              commandvars::deletePatternMatches(QStringLiteral("~/a/**/b"), home + "/a/x/y/b", {}) &&
              commandvars::deletePatternMatches(QStringLiteral("~/a/**/b"), home + "/a/b", {}) &&
              !commandvars::deletePatternMatches(QStringLiteral("~/a/**"), home + "/a", {}) &&
              commandvars::deletePatternMatches(QStringLiteral("~/a/b*"), home + "/a/build", {}) &&
              !commandvars::deletePatternMatches(QStringLiteral("~/a/b*"), home + "/a/*", {}) &&
              !commandvars::deletePatternMatches(QStringLiteral("~/a/?"), home + "/a/*", {}) &&
              commandvars::deletePatternMatches(QStringLiteral("~/a/*"), home + "/a/*", {}) &&
              !commandvars::deletePatternMatches(QStringLiteral("$FORKMESH_AGENT_SCRATCH/**"),
                                                 QStringLiteral("/a"), {}),
          QStringLiteral("* is one name, ** any number of folders, a glob only by * or **"));
    for (const QString &bad :
         {QStringLiteral("/**"), QStringLiteral("~/**"), QStringLiteral("~/*/x"),
          QStringLiteral("/home/**"), QStringLiteral("$OTHER/x/**"), QStringLiteral("build/**"),
          QStringLiteral("~/a/../b/**")})
        check(!commandvars::deletePatternProblem(bad).isEmpty(),
              QStringLiteral("pattern %1 is refused").arg(bad));
    check(commandvars::deletePatternProblem(QStringLiteral("~/projects/forkmesh/src/.worktrees/*/**"))
              .isEmpty(),
          QStringLiteral("a worktree pattern is accepted"));
    for (const QString &path :
         {QStringLiteral("/"), home, QFileInfo(home).path(), home + "/projects",
          home + "/projects/forkmesh", home + "/projects/forkmesh/src",
          home + "/projects/forkmesh/data/x", home + "/projects/forkmesh/config",
          home + "/projects/forkmesh/src/.worktrees",
          home + "/projects/forkmesh/src/.worktrees/.scratch",
          home + "/projects/forkmesh/src/.worktrees/1715-run",
          home + "/projects/forkmesh/src/.worktrees/1715-run/.git", home + "/w/.git/objects",
          home + "/.config/ForkMesh", home + "/projects/forkmesh-backup/a"})
        check(commandvars::protectedDeletePath(path),
              QStringLiteral("%1 is never covered by a rule").arg(path));
    for (const QString &path :
         {home + "/projects/forkmesh/src/.worktrees/1715-run/build",
          home + "/projects/forkmesh/src/.worktrees/.scratch/1715-run",
          home + "/projects/forkmesh/src/.worktrees/.scratch/1715-run/mainbase"})
        check(!commandvars::protectedDeletePath(path),
              QStringLiteral("%1 can be covered by a rule").arg(path));
    check(!commandvars::deletesAllowed({QStringLiteral("~/projects/**")},
                                       {home + "/projects/forkmesh/data/identity"}, {}),
          QStringLiteral("a broad rule still never covers ForkMesh data"));

    const QStringList suggested = commandvars::suggestedDeletePatterns(risk.removedPaths, scratch);
    check(suggested.value(0) == QLatin1String("$FORKMESH_AGENT_SCRATCH/**") &&
              suggested.contains((agents.startsWith(home + QLatin1Char('/'))
                                      ? QLatin1Char('~') + agents.mid(home.size())
                                      : agents) +
                                 QStringLiteral("/.scratch/*/**")),
          QStringLiteral("the prompt suggests rules that cover the delete (%1)")
              .arg(suggested.join(QLatin1Char(' '))));

    const QString project = QDir(temp.path()).filePath(QStringLiteral("project"));
    QDir().mkpath(project);
    projects::AgentAccess access = projects::loadAgentAccess(project);
    access.allowedDeletePaths = {QStringLiteral("$FORKMESH_AGENT_SCRATCH/**")};
    QString error;
    check(projects::saveAgentAccess(project, access, &error) &&
              projects::loadAgentAccess(project).allowedDeletePaths == access.allowedDeletePaths,
          QStringLiteral("a project's delete rules round trip"));
    const QString rulesPath = projects::globalRulesPath(QDir(temp.path()).filePath("home"));
    projects::GlobalAgentRules rules;
    rules.defaults.allowedDeletePaths = {QStringLiteral("~/builds/**")};
    check(projects::saveGlobalRules(rulesPath, rules, &error) &&
              projects::effectiveAgentAccess(projects::loadAgentAccess(project),
                                             projects::loadGlobalRules(rulesPath))
                      .allowedDeletePaths ==
                  QStringList({QStringLiteral("$FORKMESH_AGENT_SCRATCH/**"),
                               QStringLiteral("~/builds/**")}),
          QStringLiteral("project and global delete rules merge"));
}

destructive::Review reviewOf(const QString &command)
{
    const agentnet::CommandRisk risk = agentnet::classifyCommandRisk(command);
    return destructive::review(command, risk.destructive, risk.matchStart, risk.matchLength);
}

void testDestructiveReview()
{
    using Lean = destructive::Review::Lean;
    const QString own = QStringLiteral(
        "git stash push -u -m mkt-cpa-baseline-1450 >/dev/null && "
        "sha=$(git stash list --format='%H %gs' | grep mkt-cpa-baseline-1450 | cut -d' ' -f1); "
        "timeout 300 python3 -m pytest -q app/tests/test_a.py 2>&1 | grep FAILED | sort > "
        "\"$TMPDIR/before.txt\"; git stash apply $sha >/dev/null && "
        "n=$(git stash list --format='%gd %gs' | grep mkt-cpa-baseline-1450 | cut -d' ' -f1) && "
        "git stash drop $n >/dev/null; diff \"$TMPDIR/before.txt\" \"$TMPDIR/after.txt\"");
    const destructive::Review stash = reviewOf(own);
    check(stash.lean == Lean::Allow &&
              stash.forAllow.join(' ').contains(QLatin1String("mkt-cpa-baseline-1450")) &&
              stash.forAllow.join(' ').contains(QLatin1String("applies the entry back")),
          QStringLiteral("a tagged push, apply and drop of its own entry leans allow"));
    check(stash.forDeny.join(' ').contains(QLatin1String("<code>$n</code> is not quoted")) &&
              stash.forDeny.join(' ').contains(QLatin1String("/dev/null")),
          QStringLiteral("an unquoted ref and hidden output are reasons to deny (%1)")
              .arg(stash.forDeny.join(QStringLiteral(" | "))));
    check(stash.what.contains(QLatin1String("git stash drop $n")) && !stash.undo.isEmpty(),
          QStringLiteral("the stash review names the exact command and how to undo it"));

    check(reviewOf(QStringLiteral("git stash clear")).lean == Lean::Deny,
          QStringLiteral("clearing the shared stash leans deny"));
    check(reviewOf(QStringLiteral("git stash drop")).lean == Lean::Deny,
          QStringLiteral("dropping the newest entry without restoring it leans deny"));

    const destructive::Review build = reviewOf(QStringLiteral("rm -rf build node_modules"));
    check(build.lean == Lean::Allow && build.what.contains(QLatin1String("<code>build</code>")),
          QStringLiteral("deleting build output leans allow"));
    const destructive::Review variable = reviewOf(QStringLiteral("rm -rf $DIR/"));
    check(variable.lean == Lean::Deny &&
              variable.forDeny.join(' ').contains(QLatin1String("<code>/</code>")),
          QStringLiteral("an unquoted variable that can empty to / leans deny (%1)")
              .arg(variable.forDeny.join(QStringLiteral(" | "))));
    check(reviewOf(QStringLiteral("rm -rf ~/projects/forkmesh/data")).lean == Lean::Deny,
          QStringLiteral("deleting ForkMesh data leans deny"));
    check(reviewOf(QStringLiteral("rm -rf ~")).lean == Lean::Deny,
          QStringLiteral("deleting the home folder leans deny"));

    check(reviewOf(QStringLiteral("git push --force origin main")).lean == Lean::Deny,
          QStringLiteral("force pushing main leans deny"));
    check(reviewOf(QStringLiteral("git push --force-with-lease origin 1450-fix")).lean ==
              Lean::Allow,
          QStringLiteral("a leased push to a working branch leans allow"));
    check(reviewOf(QStringLiteral("git clean -fdx")).lean == Lean::Deny,
          QStringLiteral("git clean -x leans deny"));
    check(reviewOf(QStringLiteral("git clean -nf")).lean == Lean::Allow,
          QStringLiteral("a dry-run clean leans allow"));
    check(reviewOf(QStringLiteral("git branch -D main")).lean == Lean::Deny,
          QStringLiteral("deleting main leans deny"));
    check(reviewOf(QStringLiteral("gh repo delete me/x --yes")).lean == Lean::Deny,
          QStringLiteral("deleting a repository leans deny"));
}

void testDestructiveCommandHtml()
{
    QTemporaryDir temp;
    QDir(temp.path()).mkpath(QStringLiteral("app/tests"));
    const QString command = QStringLiteral(
        "cd app && n=$(git stash list | grep tag) && git stash drop $n >/dev/null; "
        "timeout 300 python3 -m pytest app/tests 2>&1 | sort > \"$TMPDIR/x\" # done <b>");
    const agentnet::CommandRisk risk = agentnet::classifyCommandRisk(command);
    const QString html = destructive::commandHtml(command, risk.matchStart, risk.matchLength,
                                                  temp.path(), QDir::homePath(), false);
    check(html.contains(QLatin1String("https://git-scm.com/docs/git-stash")) &&
              html.contains(QLatin1String("https://man7.org/linux/man-pages/man1/grep.1.html")) &&
              html.contains(QLatin1String("https://man7.org/linux/man-pages/man1/timeout.1.html")) &&
              html.contains(QLatin1String("https://docs.pytest.org/")) &&
              html.contains(QLatin1String("https://docs.python.org/3/using/cmdline.html")),
          QStringLiteral("programs and subcommands link to their documentation"));
    check(html.contains(QUrl::fromLocalFile(QDir(temp.path()).filePath("app/tests")).toString()),
          QStringLiteral("an existing path links to the folder"));
    check(html.contains(QLatin1String("<a name=\"match\"></a>")) &&
              html.contains(QLatin1String("background-color:#ffd166")),
          QStringLiteral("the matched part is marked and highlighted"));
    check(html.contains(QLatin1String("&lt;b&gt;")) && !html.contains(QLatin1String("<b>")),
          QStringLiteral("the command text is escaped"));
    QString text = html;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"))
        .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
        .replace(QStringLiteral("&quot;"), QStringLiteral("\""))
        .replace(QStringLiteral("&#39;"), QStringLiteral("'"))
        .replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    check(text == command, QStringLiteral("the highlighted text is the whole command (%1)").arg(text));
    check(destructive::subcommandUrl(QStringLiteral("gh"),
                                     {QStringLiteral("repo"), QStringLiteral("delete")}) ==
              QLatin1String("https://cli.github.com/manual/gh_repo_delete"),
          QStringLiteral("gh subcommands link to their manual page"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testPolicyStorage();
    testHosts();
    testProjectFolders();
    testRestrictedCommand();
    testClassifier();
    testAccessFailures();
    testHookSettings();
    testServer();
    testGlobalRules();
    testCommandRisk();
    testCommandRiskMatch();
    testScriptClassifier();
    testRunRules();
    testAlwaysAllowedDestructive();
    testDestructiveReview();
    testScratchDelete();
    testDeleteRules();
    testDestructiveCommandHtml();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

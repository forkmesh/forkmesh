#include "LocalControlDiagnostics.h"
#include "LocalControlFiles.h"
#include "LocalControlFleet.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

namespace fleet = forkmesh::localcontrol::fleet;
namespace files = forkmesh::localcontrol::filesbrowse;
namespace diag = forkmesh::localcontrol::diag;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
}

static void testFleetSubjects()
{
    QString error;
    check(fleet::subjectFor(QStringLiteral("deployAll"),
                            {{QStringLiteral("mode"), QStringLiteral("reinstall")}}) ==
              QStringLiteral("reinstall"),
          "deploy mode is the subject");
    check(fleet::subjectFor(QStringLiteral("deployAll"),
                            {{QStringLiteral("mode"), QStringLiteral("wipe")}}, &error)
                  .isEmpty() &&
              error == QStringLiteral("bad_mode"),
          "unknown deploy mode refused");
    check(fleet::subjectFor(QStringLiteral("fleetTarget"),
                            {{QStringLiteral("enabled"), true},
                             {QStringLiteral("desired"), 3}}) == QStringLiteral("on:3"),
          "fleet target binds on and count");
    check(fleet::subjectFor(QStringLiteral("fleetTarget"),
                            {{QStringLiteral("enabled"), true},
                             {QStringLiteral("desired"), 40}})
              .isEmpty(),
          "fleet target is bounded");
    check(fleet::subjectFor(QStringLiteral("add"),
                            {{QStringLiteral("host"), QStringLiteral("mirror9")},
                             {QStringLiteral("address"), QStringLiteral("203.0.113.9")},
                             {QStringLiteral("user"), QStringLiteral("root")}}) ==
              QStringLiteral("mirror9|203.0.113.9|root"),
          "add binds name, address and user");
    check(fleet::subjectFor(QStringLiteral("add"),
                            {{QStringLiteral("host"), QStringLiteral("mirror9")},
                             {QStringLiteral("address"), QStringLiteral("1.2.3.4; rm -rf /")},
                             {QStringLiteral("user"), QStringLiteral("root")}})
              .isEmpty(),
          "shell text is not an address");
    check(fleet::subjectFor(QStringLiteral("add"),
                            {{QStringLiteral("host"), QStringLiteral("mirror9")},
                             {QStringLiteral("address"), QStringLiteral("mirror9.example.com")},
                             {QStringLiteral("user"), QStringLiteral("Root$")}})
              .isEmpty(),
          "bad ssh user refused");
    check(fleet::subjectFor(QStringLiteral("actions"),
                            {{QStringLiteral("host"), QStringLiteral("Mirror3")},
                             {QStringLiteral("enabled"), false}}) ==
              QStringLiteral("mirror3|off"),
          "actions toggle binds the state");
    check(fleet::subjectFor(QStringLiteral("linkCode"),
                            {{QStringLiteral("code"), QStringLiteral("12345")}})
              .isEmpty(),
          "link code must be six digits");
    check(fleet::subjectFor(QStringLiteral("mirrorPush"),
                            {{QStringLiteral("repo"), QStringLiteral("f/../etc")}})
              .isEmpty(),
          "repo key rejects traversal");
    check(fleet::subjectFor(QStringLiteral("nuke"), {}, &error).isEmpty() &&
              error == QStringLiteral("unsupported_action"),
          "unknown action refused");
    check(fleet::subjectFor(QStringLiteral("deleteNode"),
                            {{QStringLiteral("host"), QStringLiteral("ForkMesh-MainNode")}}, &error)
                  .isEmpty() &&
              error == QStringLiteral("protected_node"),
          "the main node cannot be deleted");
    check(fleet::typedConfirmation(QStringLiteral("deleteNode"), QStringLiteral("mirror4")) ==
              QStringLiteral("DELETE mirror4"),
          "delete node needs DELETE name");
    check(fleet::typedConfirmation(QStringLiteral("deployAll"), QStringLiteral("reinstall")) ==
              QStringLiteral("REINSTALL ALL"),
          "reinstall all needs a phrase");
    check(fleet::typedConfirmation(QStringLiteral("deployAll"), QStringLiteral("install"))
              .isEmpty(),
          "plain install needs no phrase");
    check(!fleet::typedConfirmationMatches(QStringLiteral("delete mirror4"),
                                           QStringLiteral("DELETE mirror4")),
          "typed phrase is case sensitive");
    check(fleet::typedConfirmationMatches(QStringLiteral(" DELETE mirror4 "),
                                          QStringLiteral("DELETE mirror4")),
          "typed phrase ignores surrounding space");
    for (const QString &action : fleet::mutatingActions())
        check(!fleet::actionSummary(action, QStringLiteral("x|1.2.3.4|root")).isEmpty(),
              "every action has a summary");
}

static void testFleetReads()
{
    const QJsonArray hosts{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("mirror1")},
                    {QStringLiteral("provider"), QStringLiteral("Vultr")},
                    {QStringLiteral("region"), QStringLiteral("ewr")},
                    {QStringLiteral("planType"), QStringLiteral("vc2-1c-1gb")},
                    {QStringLiteral("monthlyCost"), 5.0}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("mirror2")},
                    {QStringLiteral("provider"), QStringLiteral("vultr")},
                    {QStringLiteral("region"), QStringLiteral("ewr")},
                    {QStringLiteral("planType"), QStringLiteral("vc2-1c-2gb")},
                    {QStringLiteral("monthlyCost"), 10.0}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("box")},
                    {QStringLiteral("provider"), QStringLiteral("")}}};
    const QJsonObject costs = fleet::costSummary(hosts);
    check(costs.value(QStringLiteral("vultr")).toInt() == 2, "two vultr hosts");
    check(costs.value(QStringLiteral("hosts")).toInt() == 3, "three hosts");
    check(qFuzzyCompare(costs.value(QStringLiteral("monthlyCost")).toDouble(), 15.0),
          "monthly cost summed");
    check(costs.value(QStringLiteral("byRegion")).toArray().size() == 1, "one region");
    check(costs.value(QStringLiteral("byPlan")).toArray().first().toObject()
                  .value(QStringLiteral("plan")).toString() == QStringLiteral("vc2-1c-2gb"),
          "priciest plan first");

    const QJsonObject provision = fleet::provisionStatus(
        true, QStringLiteral("mirror7"), 4, QStringLiteral("active"),
        QStringLiteral("<b>Waiting</b> for SSH"), QString(), QString(),
        {QString(), QString(), QString(), QStringLiteral("probe 3")}, false);
    const QJsonArray stages = provision.value(QStringLiteral("stages")).toArray();
    check(stages.size() == 6, "six stages");
    check(stages.at(2).toObject().value(QStringLiteral("status")).toString() ==
              QStringLiteral("done"),
          "earlier stage done");
    check(stages.at(3).toObject().value(QStringLiteral("status")).toString() ==
              QStringLiteral("active"),
          "current stage active");
    check(stages.at(4).toObject().value(QStringLiteral("status")).toString() ==
              QStringLiteral("pending"),
          "later stage pending");
    check(provision.value(QStringLiteral("detail")).toString() ==
              QStringLiteral("Waiting for SSH"),
          "html stripped");
    const QJsonObject failed = fleet::provisionStatus(
        false, QStringLiteral("m"), 5, QStringLiteral("failed"), {}, {}, {}, {}, true);
    check(failed.value(QStringLiteral("stages")).toArray().at(4).toObject()
                  .value(QStringLiteral("status")).toString() == QStringLiteral("failed"),
          "failed stage marked");

    const QStringList lines = fleet::tailLines(
        "one\ntwo\nVULTR_API_KEY=abcdef123456\nfour\n", 3, 100);
    check(lines.size() == 3 && lines.first() == QStringLiteral("two"), "tail keeps newest");
    check(!lines.at(1).contains(QStringLiteral("abcdef123456")), "api key redacted");
    check(fleet::redactSecrets(QStringLiteral("Authorization: Bearer abc.def.ghi123"))
              .contains(QStringLiteral("[redacted]")),
          "bearer redacted");

    const QJsonArray gateways = fleet::gatewayRows(
        {QStringLiteral("mirror1\t1000\t1000\t0"), QStringLiteral("mirror2\t5000\t5000\t0"),
         QStringLiteral("bad")},
        10000, 8000);
    check(gateways.size() == 2, "bad gateway record skipped");
    check(gateways.first().toObject().value(QStringLiteral("host")).toString() ==
              QStringLiteral("mirror2"),
          "newest outage first");
    check(gateways.at(1).toObject().value(QStringLiteral("retired")).toBool(),
          "long outage retired");

    check(fleet::validRemotePath(QStringLiteral("/var/lib/forkmesh")), "plain path ok");
    check(!fleet::validRemotePath(QStringLiteral("/var/../etc")), "dotdot refused");
    check(!fleet::validRemotePath(QStringLiteral("/tmp/$(reboot)")), "shell refused");
    check(!fleet::validRemotePath(QStringLiteral("relative")), "relative refused");
    check(fleet::diskUsageCommand(QStringLiteral("/tmp/`x`")).isEmpty(), "no command for bad path");
    const QJsonArray usage = fleet::parseDiskUsage(
        QStringLiteral("900\t/var\n500\t/var/lib\n300\t/var/log\n7\t/etc/passwd\n"),
        QStringLiteral("/var"));
    check(usage.size() == 3, "outside path dropped");
    check(usage.first().toObject().value(QStringLiteral("total")).toBool(), "total first");
    check(usage.at(1).toObject().value(QStringLiteral("name")).toString() ==
              QStringLiteral("lib"),
          "biggest child next");
}

static void testFilesSandbox()
{
    QTemporaryDir outside;
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("repo"));
    writeFile(root + QStringLiteral("/src/main.cpp"), "int main() {}\n");
    writeFile(root + QStringLiteral("/.git/config"), "[remote]\nurl = https://token@x\n");
    writeFile(root + QStringLiteral("/.env"), "SECRET=1\n");
    writeFile(root + QStringLiteral("/bin.dat"), QByteArray("\x00\x01\x02", 3));
    writeFile(outside.filePath(QStringLiteral("secret.txt")), "outside\n");
    QFile::link(outside.path(), root + QStringLiteral("/escape"));
    QFile::link(root + QStringLiteral("/src"), root + QStringLiteral("/srclink"));
    QFile::link(root + QStringLiteral("/.git"), root + QStringLiteral("/gitlink"));

    bool ok = false;
    check(files::normalizeRelativePath(QStringLiteral("a/./b//c"), &ok) ==
                  QStringLiteral("a/b/c") &&
              ok,
          "path normalized");
    files::normalizeRelativePath(QStringLiteral("../x"), &ok);
    check(!ok, "dotdot refused");
    files::normalizeRelativePath(QStringLiteral("/etc/passwd"), &ok);
    check(!ok, "absolute refused");
    files::normalizeRelativePath(QStringLiteral("a\\b"), &ok);
    check(!ok, "backslash refused");
    files::normalizeRelativePath(QStringLiteral("a\nb"), &ok);
    check(!ok, "control character refused");

    QString error;
    check(!files::resolveInside(root, QStringLiteral("src/main.cpp"), &error).isEmpty(),
          "file inside resolves");
    check(files::resolveInside(root, QStringLiteral("escape/secret.txt"), &error).isEmpty() &&
              error == QStringLiteral("outside_root"),
          "symlink escape refused");
    check(files::resolveInside(root, QStringLiteral(".git/config"), &error).isEmpty() &&
              error == QStringLiteral("not_allowed"),
          ".git refused");
    check(files::resolveInside(root, QStringLiteral("gitlink/config"), &error).isEmpty() &&
              error == QStringLiteral("not_allowed"),
          "symlink into .git refused");
    check(!files::resolveInside(root, QStringLiteral("srclink/main.cpp"), &error).isEmpty(),
          "symlink inside the root allowed");
    check(files::resolveInside(root, QStringLiteral("missing"), &error).isEmpty() &&
              error == QStringLiteral("not_found"),
          "missing file");
    check(files::resolveInside(dir.filePath(QStringLiteral("nope")), QString(), &error)
                  .isEmpty() &&
              error == QStringLiteral("root_unavailable"),
          "missing root");

    const files::Listing listing = files::listDirectory(root, root, 100);
    bool sawGit = false;
    bool escapeFlagged = false;
    bool envSensitive = false;
    QString first;
    for (const QJsonValue &value : listing.entries) {
        const QJsonObject row = value.toObject();
        const QString name = row.value(QStringLiteral("name")).toString();
        if (first.isEmpty())
            first = name;
        sawGit |= name == QStringLiteral(".git");
        if (name == QStringLiteral("escape"))
            escapeFlagged = row.value(QStringLiteral("escapes")).toBool();
        if (name == QStringLiteral(".env"))
            envSensitive = row.value(QStringLiteral("sensitive")).toBool();
    }
    check(!sawGit, ".git hidden from listings");
    check(escapeFlagged, "escaping link flagged");
    check(envSensitive, ".env flagged sensitive");
    check(first == QStringLiteral("escape") || first == QStringLiteral("src") ||
              first == QStringLiteral("srclink") || first == QStringLiteral("gitlink"),
          "directories sort first");
    const files::Listing small = files::listDirectory(root, root, 2);
    check(small.truncated && small.entries.size() == 2, "listing bounded");

    const QJsonObject text = files::readFileSlice(root + QStringLiteral("/src/main.cpp"), 0,
                                                  1 << 20);
    check(text.value(QStringLiteral("text")).toString() == QStringLiteral("int main() {}\n"),
          "text read");
    check(text.value(QStringLiteral("eof")).toBool(), "eof reported");
    const QJsonObject binary = files::readFileSlice(root + QStringLiteral("/bin.dat"), 0, 10);
    check(binary.value(QStringLiteral("binary")).toBool(), "binary detected");
    const QJsonObject part = files::readFileSlice(root + QStringLiteral("/src/main.cpp"), 4, 3);
    check(part.value(QStringLiteral("text")).toString() == QStringLiteral("main") ||
              part.value(QStringLiteral("text")).toString() == QStringLiteral("mai"),
          "slice read");
    check(!part.value(QStringLiteral("eof")).toBool(), "slice not at eof");
    writeFile(root + QStringLiteral("/utf.txt"), QString::fromUtf8("a\xC3\xA9z").toUtf8());
    const QJsonObject cut = files::readFileSlice(root + QStringLiteral("/utf.txt"), 0, 2);
    check(cut.value(QStringLiteral("text")).toString() == QStringLiteral("a") &&
              !cut.value(QStringLiteral("binary")).toBool(),
          "slice never splits a character");
    check(files::isSensitiveName(QStringLiteral("id_ed25519")), "ssh key sensitive");
    check(files::isSensitiveName(QStringLiteral("server.PEM")), "pem sensitive");
    check(!files::isSensitiveName(QStringLiteral("README.md")), "readme fine");
}

static void testFilesRootsAndStatus()
{
    QTemporaryDir dir;
    const QString repo = dir.filePath(QStringLiteral("repo"));
    const QString wt = dir.filePath(QStringLiteral("wt/feature-x"));
    writeFile(wt + QStringLiteral("/.git"), "gitdir: x\n");
    writeFile(repo + QStringLiteral("/.git/worktrees/feature-x/gitdir"),
              (wt + QStringLiteral("/.git\n")).toUtf8());
    writeFile(repo + QStringLiteral("/.git/worktrees/feature-x/HEAD"),
              "ref: refs/heads/agent/feature-x\n");
    writeFile(repo + QStringLiteral("/.git/worktrees/gone/gitdir"),
              dir.filePath(QStringLiteral("missing/.git")).toUtf8());
    const QList<files::Root> roots =
        files::discoverWorktrees(QStringLiteral("f/forkmesh"), repo, 10);
    check(roots.size() == 1, "one live worktree");
    check(!roots.isEmpty() && roots.first().id == QStringLiteral("f/forkmesh#feature-x"),
          "worktree id");
    check(!roots.isEmpty() && roots.first().branch == QStringLiteral("agent/feature-x"),
          "worktree branch");
    check(!files::rootRow(roots.value(0)).contains(QStringLiteral("path")),
          "root rows carry no path");
    check(files::validRootId(QStringLiteral("f/forkmesh#feature-x")), "valid id");
    check(!files::validRootId(QStringLiteral("f/../x")), "traversal id refused");
    check(!files::validRootId(QStringLiteral("/etc")), "absolute id refused");

    QByteArray status("## main...origin/main [ahead 2, behind 1]");
    status.append('\0');
    status.append(" M src/a.cpp");
    status.append('\0');
    status.append("M  src/b.cpp");
    status.append('\0');
    status.append("R  new.cpp");
    status.append('\0');
    status.append("old.cpp");
    status.append('\0');
    status.append("?? notes.txt");
    status.append('\0');
    status.append("UU conflict.cpp");
    status.append('\0');
    const QJsonObject parsed = files::parsePorcelainStatus(status, 3);
    check(parsed.value(QStringLiteral("branch")).toString() == QStringLiteral("main"), "branch");
    check(parsed.value(QStringLiteral("upstream")).toString() == QStringLiteral("origin/main"),
          "upstream");
    check(parsed.value(QStringLiteral("ahead")).toInt() == 2, "ahead");
    check(parsed.value(QStringLiteral("behind")).toInt() == 1, "behind");
    check(parsed.value(QStringLiteral("total")).toInt() == 5, "five changes");
    check(parsed.value(QStringLiteral("untracked")).toInt() == 1, "untracked");
    check(parsed.value(QStringLiteral("conflicted")).toInt() == 1, "conflicted");
    check(parsed.value(QStringLiteral("truncated")).toBool(), "status bounded");
    check(parsed.value(QStringLiteral("files")).toArray().at(2).toObject()
                  .value(QStringLiteral("from")).toString() == QStringLiteral("old.cpp"),
          "rename source kept");
    check(files::gitArguments(QStringLiteral("pull")).contains(QStringLiteral("--ff-only")),
          "pull is fast-forward only");
    check(files::gitArguments(QStringLiteral("push")).isEmpty(), "push not offered");
}

static void testDiagnostics()
{
    QTemporaryDir dir;
    const QString log = dir.filePath(QStringLiteral("network_log.txt"));
    QByteArray bytes;
    for (int i = 0; i < 50; ++i)
        bytes += QStringLiteral("2026-09-24 10:00:%1  line %2%3\n")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i)
                     .arg(i % 10 == 0 ? QStringLiteral(" failed") : QString())
                     .toUtf8();
    bytes += "2026-09-24 10:01:00  token=supersecretvalue here\n";
    writeFile(log, bytes);
    const diag::LogPage page = diag::logPage(log, -1, 10, QString(), QString());
    check(page.lines.size() == 10, "page bounded");
    check(page.more, "older lines exist");
    check(!page.lines.last().toObject().value(QStringLiteral("text")).toString()
               .contains(QStringLiteral("supersecretvalue")),
          "log secrets redacted");
    check(page.lines.first().toObject().value(QStringLiteral("t")).toDouble() > 0,
          "timestamp parsed");
    const diag::LogPage older = diag::logPage(log, page.start, 5, QString(), QString());
    check(older.lines.size() == 5, "older page");
    check(older.lines.last().toObject().value(QStringLiteral("offset")).toDouble() <
              page.start,
          "older page ends before the newer one");
    const diag::LogPage errors = diag::logPage(log, -1, 100, QString(), QStringLiteral("error"));
    check(errors.lines.size() == 5, "error filter");
    const diag::LogPage query = diag::logPage(log, -1, 100, QStringLiteral("LINE 4"), QString());
    check(query.lines.size() == 11, "case-insensitive query");
    check(diag::logPage(dir.filePath(QStringLiteral("none")), -1, 10, {}, {}).lines.isEmpty(),
          "missing log");

    const QString stall = QStringLiteral(
        "[2026-09-24 09:00:00] UI froze for 6 s - likely origin: git\nframe 1\nframe 2");
    const QJsonObject row = diag::stallRow(3, stall);
    check(row.value(QStringLiteral("when")).toString() == QStringLiteral("2026-09-24 09:00:00"),
          "stall when");
    check(row.value(QStringLiteral("lines")).toInt() == 3, "stall lines");
    const QJsonObject detail = diag::stallDetail(3, stall, 30);
    check(detail.value(QStringLiteral("truncated")).toBool(), "stall detail bounded");

    diag::HealthInput input;
    input.memoryTotal = 100;
    input.memoryAvailable = 50;
    input.diskTotal = 100;
    input.diskFree = 50;
    check(diag::healthLevel(input) == QStringLiteral("ok"), "healthy");
    input.memoryAvailable = 10;
    check(diag::healthLevel(input) == QStringLiteral("busy"), "low memory busy");
    input.pressureValid = true;
    input.memoryFullAvg10 = 20;
    check(diag::healthLevel(input) == QStringLiteral("strained"), "memory starvation strained");
    check(diag::healthSummary(input).value(QStringLiteral("agents")).isObject(), "summary shape");
    check(!diag::healthSummary(input).contains(QStringLiteral("power")), "power absent when unknown");
    input.powerKnown = true;
    input.onBattery = true;
    input.batteryPercent = 61;
    const QJsonObject power = diag::healthSummary(input).value(QStringLiteral("power")).toObject();
    check(power.value(QStringLiteral("onBattery")).toBool(), "power on battery");
    check(power.value(QStringLiteral("percent")).toInt() == 61, "power battery percent");

    check(diag::usedPercent(200, 50) == 75.0, "used percent");
    check(diag::usedPercent(0, 0) < 0 && diag::usedPercent(100, 150) < 0, "used percent unknown");
    QList<diag::VitalsSample> vitals;
    for (int i = 0; i < diag::kVitalsSeconds + 5; ++i)
        diag::appendVitals(vitals, {double(i), 1, 2, -1});
    check(vitals.size() == diag::kVitalsSeconds && vitals.first().memory == 5.0, "vitals bounded");
    const QJsonObject history = diag::vitalsHistoryJson(vitals);
    check(history.value(QStringLiteral("memory")).toArray().size() == diag::kVitalsSeconds &&
              history.value(QStringLiteral("swap")).toArray().first().toDouble() == -1.0,
          "vitals history shape");
    check(diag::vitalsSampleJson(vitals.last()).value(QStringLiteral("memory")).toDouble() ==
              double(diag::kVitalsSeconds + 4),
          "vitals sample shape");

    QHash<qint64, qint64> parents;
    const QList<diag::ProcessRow> rows = diag::parseProcessTable(
        "  100 1 f 204800 5.0 /usr/bin/claude --model x\n"
        "  200 100 f 1024 0.1 node server.js --token=abc\n"
        "bad line\n",
        1, &parents);
    check(rows.size() == 1 && rows.first().name == QStringLiteral("claude"), "ps parsed and bounded");
    check(parents.value(200) == 100, "parents kept beyond the row bound");
    check(!diag::commandSummary(QStringLiteral("node x --token=abc")).contains(QStringLiteral("abc")),
          "command secrets redacted");
    check(diag::subjectFor(QStringLiteral("stopAgent"), {{QStringLiteral("session"), 12}}) ==
              QStringLiteral("12"),
          "stop agent subject");
    check(diag::subjectFor(QStringLiteral("stopAgent"), {{QStringLiteral("session"), -1}})
              .isEmpty(),
          "bad session refused");
    check(diag::subjectFor(QStringLiteral("kill"), {}).isEmpty(), "unknown diag action");
    check(diag::mutatingActions().contains(QStringLiteral("freeDiskSpace")),
          "disk cleanup needs a confirmation token");
    check(diag::subjectFor(QStringLiteral("freeDiskSpace"), {{QStringLiteral("step"), QStringLiteral("agents")}}) ==
              QStringLiteral("agents"),
          "disk cleanup step subject");
    {
        QString why;
        check(diag::subjectFor(QStringLiteral("freeDiskSpace"),
                               {{QStringLiteral("step"), QStringLiteral("rm -rf /")}}, &why)
                      .isEmpty() &&
                  why == QStringLiteral("bad_step"),
              "unknown disk cleanup step refused");
    }
    check(diag::actionSummary(QStringLiteral("freeDiskSpace"), QStringLiteral("caches"), {})
              .contains(QStringLiteral("pip and npm caches")),
          "disk cleanup summary names the step");
    check(diag::freeDiskSpaceArguments(QStringLiteral("/home/u/src"), QStringLiteral("tests")) ==
              QStringList{QStringLiteral("-u"), QStringLiteral("/home/u/src/tools/free_disk_space.py"),
                          QStringLiteral("--execute"), QStringLiteral("--step"),
                          QStringLiteral("tests"), QStringLiteral("--repo"),
                          QStringLiteral("/home/u/src"), QStringLiteral("--json")},
          "disk cleanup runs one step of the checkout's tool");
    {
        const QJsonObject reply = diag::freeDiskSpaceReply(
            QStringLiteral("agents"),
            "del    1.0 GB  /tmp/fm123  (agent 123 finished)\n\nFreed 1.0 GB from 1 items.\n"
            "::result::{\"freeBytes\": 5, \"freedBytes\": 1073741824, \"items\": 1, "
            "\"step\": \"agents\", \"totalBytes\": 9}\n",
            1500);
        check(reply.value(QStringLiteral("freedBytes")).toDouble() == 1073741824.0 &&
                  reply.value(QStringLiteral("freeBytes")).toDouble() == 5.0 &&
                  reply.value(QStringLiteral("ms")).toDouble() == 1500.0,
              "disk cleanup result parsed");
        check(reply.value(QStringLiteral("lines")).toArray().size() == 2,
              "disk cleanup lines kept without the result line or blanks");
        QString why;
        check(diag::freeDiskSpaceReply(QStringLiteral("tests"), "::result::{\"step\": \"agents\"}", 1, &why)
                      .isEmpty() &&
                  why == QStringLiteral("cleanup_failed"),
              "a result for another step is refused");
    }

    check(diag::processPid({{QStringLiteral("pid"), 4242}}) == 4242, "process pid read");
    check(diag::processPid({{QStringLiteral("pid"), 0}}) == 0 &&
              diag::processPid({{QStringLiteral("pid"), 1.5}}) == 0 &&
              diag::processPid({{QStringLiteral("pid"), 4294967296.0}}) == 0 &&
              diag::processPid({{QStringLiteral("pid"), QStringLiteral("12")}}) == 0 &&
              diag::processPid({}) == 0,
          "bad process pid refused");
    check(diag::processSubject(42, QStringLiteral("9001")) == QStringLiteral("42:9001"),
          "process subject binds pid and start time");
    check(diag::processStoppable(42, 7) && !diag::processStoppable(1, 7) &&
              !diag::processStoppable(7, 7),
          "init and the desktop are protected");
    check(diag::sudoPasswordUsable(QStringLiteral("hunter2")) &&
              !diag::sudoPasswordUsable(QString()) &&
              !diag::sudoPasswordUsable(QStringLiteral("a\nb")) &&
              !diag::sudoPasswordUsable(QString(1025, QLatin1Char('x'))),
          "sudo password checked");
    const QString processDetail =
        diag::processDetailText(QString(diag::kMaxProcessDetailChars + 10, QLatin1Char('x')));
    check(processDetail.endsWith(QStringLiteral("[truncated]")), "process detail bounded");
}

static void testBackupStatus()
{
    check(diag::backupLocationLabel(QStringLiteral("/home/u/.local/share/ForkMesh/backups"),
                                    QStringLiteral("/home/u/.local/share/ForkMesh")) ==
              QStringLiteral("ForkMesh data › backups"),
          "backup location is a label");
    check(!diag::backupLocationLabel(QStringLiteral("/srv/x/backups"), QStringLiteral("/data"))
               .contains(QLatin1Char('/')),
          "outside the data folder only the folder name");
    check(diag::backupFailureCode(QStringLiteral("tar: /home/u/secret: Permission denied")) ==
              QLatin1String("failed"),
          "raw tar text never leaves");

    diag::BackupStatusInput input;
    input.enabled = true;
    input.keep = 24;
    input.location = QStringLiteral("ForkMesh data › backups");
    input.intervalMs = 3600000;
    input.nowMs = 10000000;
    QJsonObject out = diag::backupStatus(input);
    check(out.value(QStringLiteral("result")).toString() == QLatin1String("none"), "none yet");
    check(out.value(QStringLiteral("nextDueAtMs")).toDouble() == 10000000, "due now");

    input.snapshots = {{9000000, 500}, {5000000, 300}};
    out = diag::backupStatus(input);
    check(out.value(QStringLiteral("result")).toString() == QLatin1String("ok"), "ok");
    check(out.value(QStringLiteral("count")).toInt() == 2, "count");
    check(out.value(QStringLiteral("totalBytes")).toDouble() == 800, "total");
    check(out.value(QStringLiteral("lastBackupAtMs")).toDouble() == 9000000, "last");
    check(out.value(QStringLiteral("lastBackupBytes")).toDouble() == 500, "last size");
    check(out.value(QStringLiteral("nextDueAtMs")).toDouble() == 12600000, "next due");
    check(!out.contains(QStringLiteral("error")), "no error");

    input.failure = QStringLiteral("tar_failed");
    input.failureAtMs = 9500000;
    out = diag::backupStatus(input);
    check(out.value(QStringLiteral("result")).toString() == QLatin1String("failed"), "failed");
    check(out.value(QStringLiteral("error")).toString() == QLatin1String("tar_failed"), "code");

    input.failure = QStringLiteral("low_disk");
    out = diag::backupStatus(input);
    check(out.value(QStringLiteral("error")).toString() == QLatin1String("low_disk"),
          "a backup skipped for free space keeps its own code");
    input.failure = QStringLiteral("tar_failed");

    input.failureAtMs = 8000000;
    out = diag::backupStatus(input);
    check(out.value(QStringLiteral("result")).toString() == QLatin1String("ok"),
          "an older failure is superseded");

    input.running = true;
    check(diag::backupStatus(input).value(QStringLiteral("result")).toString() ==
              QLatin1String("running"),
          "running");

    diag::BackupStatusInput off;
    off.intervalMs = 3600000;
    out = diag::backupStatus(off);
    check(out.value(QStringLiteral("result")).toString() == QLatin1String("off"), "off");
    check(out.value(QStringLiteral("nextDueAtMs")).toDouble() == 0, "never due when off");
    for (const QString &key : out.keys())
        check(!out.value(key).toString().contains(QLatin1Char('/')), "no paths in the answer");
}

static void testBuildMacStatus()
{
    QJsonObject out = diag::buildMacStatus(false, QByteArrayLiteral("12\nci/abc\n"), 5000000);
    check(out.value(QStringLiteral("configured")).toBool(), "configured");
    check(out.value(QStringLiteral("state")).toString() == QLatin1String("offline"), "offline");
    check(!out.contains(QStringLiteral("job")), "no job while offline");

    out = diag::buildMacStatus(true, QByteArray(), 5000000);
    check(out.value(QStringLiteral("state")).toString() == QLatin1String("idle"), "no lock is idle");

    out = diag::buildMacStatus(true, QByteArrayLiteral("120\ndesktop-release v1.2.5"), 5000000);
    check(out.value(QStringLiteral("state")).toString() == QLatin1String("building"), "building");
    check(out.value(QStringLiteral("job")).toString() == QLatin1String("desktop-release v1.2.5"),
          "job");
    check(out.value(QStringLiteral("sinceMs")).toDouble() == 4880000, "since");
    check(out.value(QStringLiteral("checkedAtMs")).toDouble() == 5000000, "checked");

    out = diag::buildMacStatus(true, QByteArrayLiteral("20000\nci/abc-1"), 5000000);
    check(out.value(QStringLiteral("state")).toString() == QLatin1String("idle"),
          "a lock older than four hours is stale");

    out = diag::buildMacStatus(true, QByteArrayLiteral("30\n"), 5000000);
    check(out.value(QStringLiteral("state")).toString() == QLatin1String("idle"),
          "a lock with no run name is idle");

    out = diag::buildMacStatus(true, QByteArrayLiteral("30\nci/abc<script>;rm"), 5000000);
    check(out.value(QStringLiteral("job")).toString() == QLatin1String("ci/abcscriptrm"),
          "the job name is filtered");
    check(diag::buildMacProbeScript().contains(QLatin1String("ci/lock/run")), "probe reads the lock");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testFleetSubjects();
    testFleetReads();
    testFilesSandbox();
    testFilesRootsAndStatus();
    testDiagnostics();
    testBackupStatus();
    testBuildMacStatus();
    if (failures == 0)
        std::cout << "local control remote tests passed\n";
    return failures == 0 ? 0 : 1;
}

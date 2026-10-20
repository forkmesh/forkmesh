#include "CoreNodeLogs.h"
#include "IpCountry.h"
#include "IpWhois.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cstdio>

using namespace forkmesh::corelogs;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void checkEqual(const QString &actual, const QString &expected,
                       const char *what)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", what,
                     qPrintable(expected), qPrintable(actual));
        ++failures;
    }
}

static QByteArray journal(const char *unit, const char *identifier,
                          const char *priority, const QString &message,
                          const char *cursor = "s=abc;i=1;b=def;m=1;t=2;x=3")
{
    QString escaped = message;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral(
               "{\"PRIORITY\":\"%1\",\"_SYSTEMD_UNIT\":\"%2\","
               "\"SYSLOG_IDENTIFIER\":\"%3\",\"MESSAGE\":\"%4\","
               "\"__REALTIME_TIMESTAMP\":\"1790275067227436\","
               "\"__CURSOR\":\"%5\"}")
        .arg(QString::fromLatin1(priority), QString::fromLatin1(unit),
             QString::fromLatin1(identifier), escaped,
             QString::fromLatin1(cursor))
        .toUtf8();
}

static void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "write a test file");
    file.write(data);
}

static void testFleetDiscovery()
{
    const QByteArray json = R"({
        "fmnext-core-2": {"ip": "203.0.113.12"},
        "fmnext-core-1": {"ip": "203.0.113.11"},
        "fmnext-run-1": {"ip": "203.0.113.21"},
        "fmnext-core-3": {"ip": "203.0.113.13; rm -rf /"},
        "fmnext-core-10": {"ip": "10.0.0.10"},
        "fmnext-auto-2": {"ip": "45.0.0.2"},
        "fmnext-burst-1": {"ip": "45.0.1.1"},
        "mirror35": {"ip": "1.2.3.4"}
    })";
    const QList<CoreNode> all = parseFleetNodes(json);
    check(all.size() == 6, "cores, app replicas and runners with a plain address are followed");
    if (all.size() == 6) {
        checkEqual(all.at(3).name, QStringLiteral("a2"), "an app replica is aN, after the cores");
        checkEqual(all.at(3).role, QStringLiteral("core"), "an app replica reads like a core");
        checkEqual(all.at(5).name, QStringLiteral("b1"), "a burst runner is bN, after the runners");
        checkEqual(all.at(5).role, QStringLiteral("runner"), "a burst runner reads like a runner");
    }
    QList<CoreNode> nodes;
    for (const CoreNode &node : all) {
        if (!node.label.startsWith(QLatin1String("fmnext-auto-")) &&
            !node.label.startsWith(QLatin1String("fmnext-burst-")))
            nodes.append(node);
    }
    if (nodes.size() == 4) {
        checkEqual(nodes.at(0).name, QStringLiteral("n1"), "cores come first, in order");
        checkEqual(nodes.at(0).address, QStringLiteral("203.0.113.11"),
                   "the address comes from the instance record");
        checkEqual(nodes.at(0).role, QStringLiteral("core"), "a core's role");
        checkEqual(nodes.at(2).name, QStringLiteral("n10"),
                   "numbers sort numerically");
        checkEqual(nodes.at(3).name, QStringLiteral("r1"), "the runner is r1");
        checkEqual(nodes.at(3).role, QStringLiteral("runner"), "a runner's role");
        checkEqual(nodes.at(3).label, QStringLiteral("fmnext-run-1"),
                   "the Vultr label is kept");
    }
    check(parseFleetNodes("not json").isEmpty(), "a broken file names no node");

    const QByteArray vultr = R"({"instances":[
        {"label":"fmnext-run-7","main_ip":"45.0.0.7","tags":["forkmesh-next"]},
        {"label":"fmnext-run-8","main_ip":"0.0.0.0","tags":["forkmesh-next"]},
        {"label":"fmnext-run-9","main_ip":"45.0.0.9","tags":[]},
        {"label":"tradeskie-production","main_ip":"45.0.0.1","tags":["forkmesh-next"]},
        {"label":"fmnext-core-1","main_ip":"203.0.113.11","tags":["forkmesh-next"]},
        {"label":"fmnext-burst-3","main_ip":"45.0.0.33","tags":["forkmesh-next"]}
    ]})";
    const QList<CoreNode> listedAll = parseVultrInstances(vultr);
    check(listedAll.size() == 3, "only tagged fmnext instances with an address");
    QList<CoreNode> listed;
    for (const CoreNode &node : listedAll) {
        if (node.name != QLatin1String("b3"))
            listed.append(node);
    }
    check(listed.size() == 2, "the burst runner b3 is listed");
    const QList<CoreNode> merged = mergeFleetNodes(nodes, listed);
    QStringList names;
    for (const CoreNode &node : merged)
        names.append(node.name);
    checkEqual(names.join(QLatin1Char(',')), QStringLiteral("n1,n2,n10,r1,r7"),
               "an autoscaled runner joins the file's nodes once");

    QTemporaryDir root;
    qputenv("FORKMESH_PROJECTS_DIR", root.path().toUtf8());
    qunsetenv("FORKMESH_NEXT_HOME");
    qunsetenv("FMNEXT_CONFIG_DIR");
    qunsetenv("FMNEXT_LOGREAD_KEY");
    writeFile(root.path() + QStringLiteral("/forkmesh-next/config/fleet-instances.json"), json);
    writeFile(root.path() + QStringLiteral("/forkmesh-next/config/vultr.env"),
              "# comment\nexport VULTR_API_KEY='abc123'\n");
    checkEqual(fleetConfigDirectory(), root.path() + QStringLiteral("/forkmesh-next/config"),
               "the legacy fleet config is used while it is the only one");
    check(loadFleetNodes().size() == 6, "the fleet file is read");
    writeFile(root.path() + QStringLiteral("/forkmesh-next/config/autoscaled-instances.json"),
              R"({"fmnext-burst-5": {"id": "x", "ip": "45.0.5.5"},
                  "fmnext-core-9": {"id": "y", "ip": "45.0.9.9"},
                  "fmnext-auto-2": {"id": "z", "ip": "45.0.0.2"}})");
    QStringList discovered;
    for (const CoreNode &node : loadFleetNodes())
        discovered.append(node.name);
    checkEqual(discovered.join(QLatin1Char(',')),
               QStringLiteral("n1,n2,n10,a2,r1,b1,b5"),
               "autoscaled-instances.json adds burst runners and app replicas, never a core");
    checkEqual(autoscaledInstancesPath(),
               root.path() + QStringLiteral("/forkmesh-next/config/autoscaled-instances.json"),
               "the autoscaled nodes file sits next to fleet-instances.json");
    checkEqual(vultrApiKey(), QStringLiteral("abc123"), "the Vultr key is read");
    const FeedCommand missingKey = buildFeedCommand(nodes.at(0), QString());
    check(missingKey.program.isEmpty() && missingKey.error.contains(QLatin1String("logread")),
          "no reader key, no command");
    writeFile(root.path() + QStringLiteral("/forkmesh-next/ssh/next_logread_ed25519"), "k");
    checkEqual(readerKeyPath(),
               root.path() + QStringLiteral("/forkmesh-next/ssh/next_logread_ed25519"),
               "the reader key lives beside the fleet key");
    const FeedCommand command = buildFeedCommand(nodes.at(3), QString());
    check(!command.program.isEmpty(), "with the reader key there is a command");
    check(command.arguments.contains(QStringLiteral("root@203.0.113.21")),
          "the feed logs in to the node");
    check(command.arguments.contains(QStringLiteral("IdentitiesOnly=yes")) &&
              command.arguments.contains(QStringLiteral("BatchMode=yes")) &&
              command.arguments.contains(QStringLiteral("-T")),
          "only the reader key, no prompt, no pty");
    check(!command.arguments.join(QLatin1Char(' ')).contains(QLatin1String("next_fleet_ed25519")),
          "the fleet (root) key is never used for logs");
    checkEqual(command.arguments.last(), QStringLiteral("follow lines 200"),
               "the request is the reader's protocol, not a shell command");
    check(!command.arguments.join(QLatin1Char(' ')).contains(QLatin1String("Port=")),
          "without a fleet file the feed uses port 22");
    writeFile(root.path() + QStringLiteral("/forkmesh/src/deploy/fleet.toml"),
              "ssh_key_name = \"fmnext-fleet\"\nssh_port = 822\n");
    check(fleetSshPort() == 822, "the SSH port comes from fleet.toml");
    const FeedCommand moved = buildFeedCommand(nodes.at(3), QString());
    check(moved.arguments.contains(QStringLiteral("Port=822")) &&
              moved.arguments.contains(QStringLiteral("HostKeyAlias=203.0.113.21")),
          "the feed connects on the fleet port, host key pinned by address");
    checkEqual(moved.arguments.last(), QStringLiteral("follow lines 200"),
               "the request stays last");
    const FeedCommand legacy = buildFeedCommand(nodes.at(3), QString(), true);
    check(!legacy.arguments.join(QLatin1Char(' ')).contains(QLatin1String("Port=")),
          "a legacy retry uses port 22 for a node not moved yet");
    VultrInventory inventory;
    inventory.fetchedMs = QDateTime::currentMSecsSinceEpoch() - 1000;
    inventory.failures = 2;
    inventory.retryAtMs = inventory.fetchedMs + vultrInventoryRetryDelayMs(2, 0);
    inventory.nodes = listedAll;
    const QString inventoryPath = root.path() + QStringLiteral("/data/vultr-inventory.json");
    check(saveVultrInventory(inventoryPath, inventory), "the Vultr listing is cached on disk");
    const VultrInventory reloaded = loadVultrInventory(inventoryPath);
    check(reloaded.fetchedMs == inventory.fetchedMs && reloaded.failures == 2 &&
              reloaded.retryAtMs == inventory.retryAtMs && reloaded.nodes.size() == 3,
          "a restart reuses the cached listing and its backoff");
    check(QFileInfo(inventoryPath).permissions() ==
              (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
               QFileDevice::WriteUser),
          "the cache is owner-only");
    check(vultrInventoryDueMs(reloaded) == reloaded.retryAtMs,
          "a failing listing waits for its retry time");
    VultrInventory healthy;
    healthy.fetchedMs = 1000;
    check(vultrInventoryDueMs(healthy) == 1000 + kVultrInventoryRefreshMs,
          "a healthy listing is repeated once an hour");
    check(vultrInventoryDueMs(VultrInventory()) == kVultrInventoryRefreshMs,
          "nothing cached lists at once");
    check(vultrInventoryRetryDelayMs(1, 0) == kVultrInventoryRetryBaseMs,
          "the first failure waits the base delay");
    check(vultrInventoryRetryDelayMs(3, 0) == 4 * kVultrInventoryRetryBaseMs,
          "each failure doubles the wait");
    check(vultrInventoryRetryDelayMs(40, 0) == kVultrInventoryRetryMaxMs,
          "an outage settles at the cap");
    check(vultrInventoryRetryDelayMs(1, 2 * 60 * 60 * 1000) == 2 * 60 * 60 * 1000,
          "Retry-After is honoured when it asks for longer");
    check(vultrInventoryRetryDelayMs(1, 1000LL * 24 * 60 * 60 * 1000) ==
              kVultrInventoryRetryAfterMaxMs,
          "an absurd Retry-After is bounded");
    check(loadVultrInventory(root.path() + QStringLiteral("/missing.json")).nodes.isEmpty(),
          "no cache, no nodes");
    QDir(root.path()).mkpath(QStringLiteral("forkmesh/config/fleet"));
    checkEqual(fleetConfigDirectory(), root.path() + QStringLiteral("/forkmesh/config/fleet"),
               "the adopted layout wins once it exists");
    qunsetenv("FORKMESH_PROJECTS_DIR");
}

static void testReaderProtocol()
{
    checkEqual(readerRequest(QStringLiteral("s=95;i=25289;b=3e;m=bb;t=65;x=e8")),
               QStringLiteral("follow after s=95;i=25289;b=3e;m=bb;t=65;x=e8"),
               "a reconnect resumes after the handled cursor");
    checkEqual(readerRequest(QStringLiteral("x'; reboot; '")),
               QStringLiteral("follow lines 200"),
               "a cursor with shell characters falls back to the tail");
    const ReaderControl header = parseReaderControl(
        R"({"node":"n3","role":"core","logread":1,"resumed":true})");
    check(header.kind == ReaderControl::Header && header.node == QLatin1String("n3") &&
              header.resumed,
          "the reader's header is recognised");
    check(parseReaderControl(R"({"logread_idle":1790288943000})").kind ==
              ReaderControl::Idle,
          "an idle heartbeat is recognised");
    const ReaderControl failure =
        parseReaderControl(R"({"logread":1,"error":"unsupported request"})");
    check(failure.kind == ReaderControl::Failure &&
              failure.error == QLatin1String("unsupported request"),
          "a refused request is a failure");
    check(parseReaderControl(journal("caddy.service", "caddy", "6",
                                     QStringLiteral("{\"logread\":1}")))
                  .kind == ReaderControl::None,
          "a journal entry quoting the header is still an entry");
    check(readerFailureState(QStringLiteral(
                                 "root@1.2.3.4: Permission denied (publickey)."))
              .contains(QLatin1String("not accepted")),
          "a node without the reader key says so");

    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/fleet_logs/cursors.json");
    QHash<QString, QString> cursors;
    cursors.insert(QStringLiteral("n1"), QStringLiteral("s=1;i=2"));
    cursors.insert(QStringLiteral("r1"), QStringLiteral("bad cursor'"));
    check(saveCursors(path, cursors), "cursors are saved");
    const QHash<QString, QString> loaded = loadCursors(path);
    check(loaded.size() == 1 && loaded.value(QStringLiteral("n1")) == QLatin1String("s=1;i=2"),
          "cursors survive a restart; invalid ones are dropped");
    check((QFile(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
          "the cursor file is private");
}

static void testJournalParsing()
{
    const JournalEntry entry = parseJournalLine(
        journal("forkmesh-app.service", "python", "6",
                QStringLiteral("slow request 387ms GET /forkmesh/forkmesh")));
    check(entry.valid, "a journal record parses");
    checkEqual(entry.message, QStringLiteral("slow request 387ms GET /forkmesh/forkmesh"),
               "the message is kept");
    check(entry.priority == 6, "the priority is read");
    check(entry.realtimeMs == 1790275067227LL, "the timestamp is in ms");
    checkEqual(entry.cursor, QStringLiteral("s=abc;i=1;b=def;m=1;t=2;x=3"),
               "the cursor is kept");
    checkEqual(journalSource(entry), QStringLiteral("app"), "app unit");

    const JournalEntry bytes = parseJournalLine(
        R"({"MESSAGE":[104,105,10],"_SYSTEMD_UNIT":"rqlite.service","__CURSOR":"c"})");
    checkEqual(bytes.message, QStringLiteral("hi"),
               "a byte-array message is decoded and trimmed");
    check(!parseJournalLine("-- No entries --").valid,
          "a non-JSON line is not an entry");

    const auto sourceOf = [](const char *unit, const char *ident) {
        return journalSource(parseJournalLine(journal(unit, ident, "6",
                                                      QStringLiteral("x"))));
    };
    checkEqual(sourceOf("rqlite.service", "rqlited"), QStringLiteral("rqlite"),
               "rqlite unit");
    checkEqual(sourceOf("caddy.service", "caddy"), QStringLiteral("web"),
               "caddy unit");
    checkEqual(sourceOf("forkmesh-mirror-node.service", "forkmesh-mirror-node"),
               QStringLiteral("mirror"), "mirror node unit");
    checkEqual(sourceOf("fmnext-ids.service", "python3"), QStringLiteral("ids"),
               "IDS unit");
    checkEqual(sourceOf("fmnext-certsync.service", "python3"),
               QStringLiteral("ops"), "certsync unit");
    checkEqual(sourceOf("ssh.service", "sshd-session"), QStringLiteral("ssh"),
               "sshd");
    checkEqual(sourceOf("session-225.scope", "sshd-session"),
               QStringLiteral("ssh"), "an SSH session scope");
    checkEqual(sourceOf("user@0.service", "systemd"), QStringLiteral("system"),
               "systemd user manager");
    checkEqual(sourceOf("postfix@-.service", "postfix/smtpd"),
               QStringLiteral("mail"), "postfix smtpd");
    checkEqual(sourceOf("postfix@-.service", "postfix/qmgr"),
               QStringLiteral("mail"), "postfix queue manager");
    checkEqual(sourceOf("opendkim.service", "opendkim"), QStringLiteral("mail"),
               "opendkim");
    checkEqual(sourceOf("fmnext-mail-events.service", "python3"),
               QStringLiteral("mail"), "the mail events agent is mail, not ops");
    checkEqual(sourceOf("cron.service", "CRON"), QStringLiteral("system"), "cron");
}

static void testClassification()
{
    const auto entry = [](const char *unit, const char *ident,
                          const char *priority, const QString &message) {
        return parseJournalLine(journal(unit, ident, priority, message));
    };
    const auto classify = [](const JournalEntry &e) {
        return classifyEntry(e, journalSource(e));
    };
    const JournalEntry app = entry("forkmesh-app.service", "python", "6",
                                   QStringLiteral("Cron tick ran on n1"));
    check(classify(app).forward && classify(app).severity == Severity::Info,
          "app lines are forwarded as info");
    checkEqual(classify(app).unit, QStringLiteral("forkmesh-app"), "the unit drops .service");
    const JournalEntry logind = entry("systemd-logind.service", "systemd-logind",
                                      "6", QStringLiteral("Removed session 220."));
    check(classify(logind).forward && classify(logind).severity == Severity::Info,
          "session churn is forwarded as info");
    const JournalEntry oom = entry("init.scope", "kernel", "3",
                                   QStringLiteral("Out of memory: Killed process 42"));
    check(classify(oom).forward && classify(oom).severity == Severity::Error,
          "an err-level system line is forwarded as an error");
    const JournalEntry scanner = entry("ssh.service", "sshd", "3",
                                       QStringLiteral("error: kex_exchange_identification"));
    check(classify(scanner).forward && classify(scanner).severity == Severity::Info,
          "internet scanners hitting sshd are forwarded, never as errors");
    const JournalEntry delivered = entry(
        "postfix@-.service", "postfix/smtp", "6",
        QStringLiteral("4F2A1: to=<a@example.com>, status=sent (250 ok)"));
    check(classify(delivered).forward &&
              classify(delivered).severity == Severity::Info,
          "an info-level postfix delivery is forwarded");
    const JournalEntry bounced = entry("postfix@-.service", "postfix/smtp", "4",
                                       QStringLiteral("4F2A1: status=deferred"));
    check(classify(bounced).forward &&
              classify(bounced).severity == Severity::Warning,
          "a postfix warning keeps its severity");
    const JournalEntry kernelInfo = entry("", "kernel", "6",
                                          QStringLiteral("eth0: link up"));
    check(classify(kernelInfo).forward, "an info kernel line is forwarded");
    const JournalEntry cron = entry("cron.service", "CRON", "6",
                                    QStringLiteral("(root) CMD (true)"));
    check(classify(cron).forward, "an info cron line is forwarded");
    const JournalEntry blank = entry("cron.service", "CRON", "6", QString());
    check(!classify(blank).forward, "an empty line is not forwarded");
    const JournalEntry caddyPanic = entry(
        "caddy.service", "caddy", "6",
        QStringLiteral("{\"level\":\"error\",\"ts\":1.5,\"logger\":\"admin.api\","
                       "\"msg\":\"request error\",\"error\":\"loading config: "
                       "panic: runtime error\"}"));
    check(classify(caddyPanic).severity == Severity::Error,
          "a Caddy error-level JSON line is an error though journald says info");
    checkEqual(displayMessage(caddyPanic),
               QStringLiteral("error admin.api: request error error=loading config: "
                              "panic: runtime error"),
               "a Caddy JSON line reads as level, logger, message and error");
    const JournalEntry goPanic = entry("forkmesh-mirror-node.service",
                                       "forkmesh-mirror-node", "6",
                                       QStringLiteral("panic: runtime error: index out of range"));
    check(classify(goPanic).severity == Severity::Error, "a Go panic is an error");
    const JournalEntry idsInfo = entry(
        "fmnext-ids.service", "python3", "6",
        QStringLiteral("{\"detector\": \"ssh_tool_session\", \"severity\": \"info\"}"));
    check(classify(idsInfo).severity == Severity::Info, "an info IDS incident is info");
    const JournalEntry idsAlert = entry(
        "fmnext-ids.service", "python3", "6",
        QStringLiteral("{\"detector\": \"ssh_foreign_ip\", \"severity\": \"alert\"}"));
    check(isErrorEntry(idsAlert, journalSource(idsAlert)), "an IDS alert is an error");
    const JournalEntry idsIncident = entry(
        "fmnext-ids.service", "python3", "6",
        QStringLiteral("{\"detail\": {\"addresses\": [\"0.0.0.0\", \"::\"], "
                       "\"port\": 25, \"processes\": [{\"exe\": \"/usr/sbin/exim4\", "
                       "\"name\": \"exim4\", \"pid\": 812, \"unit\": \"exim4.service\"}], "
                       "\"proto\": \"tcp\", \"user\": \"root\"}, "
                       "\"detector\": \"listener_unexpected\", \"id\": \"c3Z2tm-jzynWAkBr\", "
                       "\"ids\": \"incident\", \"node\": \"n1\", \"severity\": \"alert\", "
                       "\"summary\": \"root listens on tcp port 25\"}"));
    checkEqual(displayMessage(idsIncident),
               QString::fromUtf8(
                   "alert listener_unexpected: root listens on tcp port 25 \xC2\xB7 "
                   "addresses=0.0.0.0, :: processes=(exe=/usr/sbin/exim4 name=exim4 "
                   "pid=812 unit=exim4.service) \xC2\xB7 incident c3Z2tm-jzynWAkBr"),
               "an IDS incident reads as severity, detector, summary, new detail and id");
    const JournalEntry idsStarted = entry(
        "fmnext-ids.service", "python3", "6",
        QStringLiteral("{\"ids\": \"started\", \"node\": \"n1\", \"role\": \"core\", "
                       "\"lockdown\": false}"));
    checkEqual(displayMessage(idsStarted),
               QStringLiteral("started lockdown=false node=n1 role=core"),
               "an IDS status line reads as its message and fields");
    const JournalEntry plainJson = entry(
        "forkmesh-mirror-node.service", "forkmesh-mirror-node", "6",
        QStringLiteral("{\"time\": \"2026-09-25T10:00:00Z\", \"level\": \"INFO\", "
                       "\"message\": \"peer synced\", \"peer\": \"n2\", \"refs\": 4}"));
    checkEqual(displayMessage(plainJson),
               QStringLiteral("INFO peer synced peer=n2 refs=4"),
               "a JSON line without msg reads as level, message and fields");
    const JournalEntry vsock = entry(
        "systemd-ssh-generator.service", "systemd-ssh-generator", "3",
        QStringLiteral("Failed to query local AF_VSOCK CID: Cannot assign requested address"));
    const JournalEntry generator = entry(
        "init.scope", "systemd", "3",
        QStringLiteral("/usr/lib/systemd/system-generators/systemd-ssh-generator "
                       "failed with exit status 1."));
    check(isKnownBootNoise(vsock) && isKnownBootNoise(generator) &&
              classify(vsock).severity == Severity::Info &&
              classify(generator).severity == Severity::Info &&
              !isKnownBootNoise(oom),
          "the ssh generator's vsock probe on a VM without vsock is not an error");
    JournalEntry replayed = app;
    const qint64 nowMs = 1790345000000LL;
    replayed.realtimeMs = nowMs - kBacklogAlertMs - 1000;
    JournalEntry fresh = app;
    fresh.realtimeMs = nowMs - 5000;
    JournalEntry untimed = app;
    untimed.realtimeMs = 0;
    check(isBacklogEntry(replayed, nowMs) && !isBacklogEntry(fresh, nowMs) &&
              !isBacklogEntry(untimed, nowMs),
          "a journal line older than the alert window is backlog");
    check(backlogNote(replayed).contains(QStringLiteral("replayed")) &&
              backlogNote(untimed).isEmpty(),
          "a replayed line says when it was logged");
    const JournalEntry logtest = entry("session-9.scope", "forkmesh-logtest", "5",
                                       QStringLiteral("hello from the fleet"));
    check(classify(logtest).forward, "a forkmesh-* identifier is forwarded");
    checkEqual(classify(logtest).unit, QStringLiteral("forkmesh-logtest"),
               "a session scope is labelled by its identifier");
    const JournalEntry job = entry("fmnext-job-abc.scope", "claude", "6",
                                   QStringLiteral("working"));
    checkEqual(journalSource(job), QStringLiteral("job"), "a runner job scope");
    checkEqual(classify(job).unit, QStringLiteral("job"), "job scopes share one unit label");
    const JournalEntry warn = entry("rqlite.service", "rqlited", "4",
                                    QStringLiteral("slow"));
    check(classify(warn).severity == Severity::Warning, "warning priority");
    check(isPoolChange(entry("forkmesh-mirror-node.service", "forkmesh-mirror-node",
                             "6", QStringLiteral("2026/09/24 runner pool: creating "
                                                 "fmnext-run-2 (queue 3)"))),
          "the autoscaler's create line is a pool change");
    check(isPoolChange(entry("forkmesh-mirror-node.service", "forkmesh-mirror-node",
                             "6", QStringLiteral("runner pool: destroying "
                                                 "fmnext-burst-2 (idle)"))),
          "a burst runner's destroy line is a pool change");
}

static void testTracebacks()
{
    TracebackAssembler assembler;
    const auto line = [](const QString &message, const char *cursor) {
        return parseJournalLine(journal("forkmesh-app.service", "python", "6",
                                        message, cursor));
    };
    check(assembler.feed(QStringLiteral("n1"),
                         line(QStringLiteral("Traceback (most recent call last):"), "c1"),
                         0).isEmpty(),
          "a traceback is held until it ends");
    check(assembler.feed(QStringLiteral("n1"),
                         line(QStringLiteral("  File \"/opt/app/src/entry.py\", line 812, "
                                             "in handle_request"), "c2"),
                         1).isEmpty(),
          "frames are held");
    check(assembler.feed(QStringLiteral("n1"),
                         line(QStringLiteral("    return rows[0]"), "c3"), 2).isEmpty(),
          "code lines are held");
    const QList<JournalEntry> done = assembler.feed(
        QStringLiteral("n1"), line(QStringLiteral("IndexError: list index out of range"), "c4"), 3);
    check(done.size() == 1, "the exception line ends the traceback");
    if (done.size() == 1) {
        checkEqual(done.first().message,
                   QString::fromUtf8("Traceback: IndexError: list index out of range "
                                     "\xC2\xB7 at entry.py:812 in handle_request"),
                   "one entry names the exception and the innermost frame");
        check(done.first().priority <= 3, "an assembled traceback is an error");
        checkEqual(done.first().cursor, QStringLiteral("c4"),
                   "the cursor advances past the whole traceback");
        check(done.first().detail.contains(QLatin1String("return rows[0]")),
              "the frames are kept as detail");
    }
    check(assembler.feed(QStringLiteral("n1"),
                         line(QStringLiteral("Traceback (most recent call last):"), "d1"),
                         10).isEmpty(),
          "a second traceback starts");
    check(assembler.takeStale(11).isEmpty(), "a fresh traceback is not stale");
    const auto stale = assembler.takeStale(5000);
    check(stale.size() == 1 && stale.first().first == QLatin1String("n1"),
          "an unfinished traceback is flushed after a quiet spell");
    const QList<JournalEntry> plain = assembler.feed(
        QStringLiteral("n2"), line(QStringLiteral("GET /api/version 200"), "e1"), 0);
    check(plain.size() == 1 && plain.first().message == QLatin1String("GET /api/version 200"),
          "other lines pass straight through");
}

static void testAccessLog()
{
    const QString raw = QStringLiteral(
        "{\"level\":\"info\",\"ts\":1790288990.8852656,\"logger\":"
        "\"http.log.access.log0\",\"msg\":\"handled request\",\"request\":"
        "{\"remote_ip\":\"67.245.233.0\",\"client_ip\":\"67.245.233.0\","
        "\"proto\":\"HTTP/2.0\",\"method\":\"GET\",\"host\":\"app.forkmesh.com\","
        "\"uri\":\"/api/repo/forkmesh/forkmesh/about?token=REDACTED\"},"
        "\"bytes_read\":0,\"duration\":0.132655155,\"size\":2703,\"status\":502,"
        "\"user_agent\":\"git/2.45.2\\tx\"}");
    const JournalEntry entry = parseJournalLine(journal("caddy.service", "caddy", "6", raw));
    const AccessRecord record = parseAccessRecord(entry, journalSource(entry));
    check(record.valid, "a Caddy access line is an access record");
    checkEqual(record.method, QStringLiteral("GET"), "method");
    checkEqual(record.host, QStringLiteral("app.forkmesh.com"), "host");
    checkEqual(record.path, QStringLiteral("/api/repo/forkmesh/forkmesh/about"),
               "the query string is not kept");
    check(record.status == 502 && record.durationMs == 133 && record.bytes == 2703,
          "status, duration in ms and size");
    check(record.atMs == 1790288990885LL, "the request time comes from Caddy");
    check(accessSiteGroup(record).isEmpty(), "an /api path is grouped by route");
    checkEqual(statusClassOf(record.status), QStringLiteral("5xx"), "status class");
    checkEqual(record.clientIp, QStringLiteral("67.245.233.0"), "client address");
    checkEqual(record.userAgent, QStringLiteral("git/2.45.2 x"),
               "the User-Agent from log_append, control characters flattened");
    checkEqual(accessLogMessage(record),
               QStringLiteral("GET app.forkmesh.com/api/repo/forkmesh/forkmesh/about "
                              "-> 502 \u00b7 67.245.233.0 \u00b7 git/2.45.2 x"),
               "a 5xx line has no duration, so repeats group and fold");
    checkEqual(accessLogCategory(record, false), QStringLiteral("ERROR"),
               "a live 5xx is an error");
    checkEqual(accessLogCategory(record, true), QStringLiteral("CLOUD"),
               "a replayed 5xx is not an alert");
    AccessRecord ok = record;
    ok.status = 200;
    checkEqual(accessLogCategory(ok, false), QStringLiteral("CLOUD"),
               "a 2xx request is a cloud line");
    ok.country = QStringLiteral("US");
    checkEqual(accessLogMessage(ok),
               QStringLiteral("GET app.forkmesh.com/api/repo/forkmesh/forkmesh/about "
                              "-> 200 133ms 2.6 KB \u00b7 \U0001F1FA\U0001F1F8 67.245.233.0 "
                              "\u00b7 git/2.45.2 x"),
               "an access record reads as one compact line without the query");
    const QList<CoreNode> fleet{
        {QStringLiteral("n2"), QStringLiteral("203.0.113.13")},
        {QStringLiteral("r1"), QStringLiteral("203.0.113.21"), QStringLiteral("runner")}};
    checkEqual(accessClientLabel(QStringLiteral("203.0.113.21"), fleet),
               QStringLiteral("r1"), "a fleet address is named by its node");
    checkEqual(accessClientLabel(QStringLiteral("127.0.0.1"), fleet),
               QStringLiteral("local"), "loopback is local");
    checkEqual(accessClientLabel(QStringLiteral("10.77.0.4"), fleet),
               QStringLiteral("fleet"), "the WireGuard subnet is the fleet");
    checkEqual(accessClientLabel(QStringLiteral("162.158.63.0"), fleet),
               QStringLiteral("Cloudflare"), "a Cloudflare egress address");
    checkEqual(accessClientLabel(QStringLiteral("2606:4700:10::1"), fleet),
               QStringLiteral("Cloudflare"), "a Cloudflare IPv6 egress address");
    check(accessClientLabel(QStringLiteral("67.245.233.0"), fleet).isEmpty() &&
              accessClientLabel(QStringLiteral("nope"), fleet).isEmpty(),
          "any other address has no label");
    AccessRecord relayed = ok;
    relayed.clientIp = QStringLiteral("162.158.63.0");
    relayed.clientLabel = QStringLiteral("Cloudflare");
    relayed.userAgent.clear();
    checkEqual(accessLogMessage(relayed),
               QStringLiteral("GET app.forkmesh.com/api/repo/forkmesh/forkmesh/about "
                              "-> 200 133ms 2.6 KB \u00b7 \U0001F1FA\U0001F1F8 "
                              "162.158.63.0 (Cloudflare)"),
               "a Cloudflare egress keeps its flag and is named");
    AccessRecord peer = relayed;
    peer.clientIp = QStringLiteral("203.0.113.21");
    peer.clientLabel = QStringLiteral("r1");
    checkEqual(accessLogMessage(peer),
               QStringLiteral("GET app.forkmesh.com/api/repo/forkmesh/forkmesh/about "
                              "-> 200 133ms 2.6 KB \u00b7 203.0.113.21 (r1)"),
               "a fleet peer is named without a flag");
    checkEqual(compactByteSize(0), QStringLiteral("0 B"), "zero bytes");
    checkEqual(compactByteSize(812), QStringLiteral("812 B"), "bytes");
    checkEqual(compactByteSize(2703), QStringLiteral("2.6 KB"), "kilobytes");
    checkEqual(compactByteSize(48 * 1024), QStringLiteral("48 KB"), "whole kilobytes");
    checkEqual(compactByteSize(3 * 1024 * 1024 + 300 * 1024), QStringLiteral("3.3 MB"),
               "megabytes");
    const QString webHtml = nodeLineHtml(
        QStringLiteral("Node n1 core caddy info: ") + accessLogMessage(ok), false);
    check(webHtml.contains(QStringLiteral(
              "<span style=\"font-family:'Noto Color Emoji','Apple Color Emoji',"
              "'Segoe UI Emoji'\">\U0001F1FA\U0001F1F8</span>")),
          "the flag is drawn in the colour-emoji face, not the monospace one");
    check(webHtml.contains(QLatin1String("#57606a'>app.forkmesh.com</span>")),
          "the host is muted so the path stands out");
    check(webHtml.contains(QLatin1String("#57606a'>2.6 KB</span>")), "the size is muted");
    check(webHtml.contains(QLatin1String("#57606a'>git/2.45.2 x</span>")),
          "the User-Agent is muted");
    const QString peerHtml = nodeLineHtml(
        QStringLiteral("Node n1 core caddy info: ") + accessLogMessage(peer), false);
    check(peerHtml.contains(QLatin1String("font-weight:600'>r1</span>")),
          "a fleet peer's name is bold");
    checkEqual(withEmojiFont(QStringLiteral("a b")), QStringLiteral("a b"),
               "no flag, nothing wrapped");
    AccessRecord bare = ok;
    bare.clientIp.clear();
    bare.userAgent.clear();
    checkEqual(accessLogMessage(bare),
               QStringLiteral("GET app.forkmesh.com/api/repo/forkmesh/forkmesh/about "
                              "-> 200 133ms 2.6 KB"),
               "no client fields, no suffix");
    checkEqual(countryFlag(QStringLiteral("de")), QStringLiteral("\U0001F1E9\U0001F1EA"),
               "a country code becomes its flag");
    check(countryFlag(QStringLiteral("D1")).isEmpty() && countryFlag(QString()).isEmpty(),
          "no flag for a bad code");
    const JournalEntry withHeaders = parseJournalLine(journal(
        "caddy.service", "caddy", "6",
        QStringLiteral("{\"logger\":\"http.log.access.log0\",\"request\":"
                       "{\"remote_ip\":\"2001:db8::1\",\"method\":\"GET\","
                       "\"host\":\"a\",\"uri\":\"/\",\"headers\":"
                       "{\"User-Agent\":[\"curl/8\"]}},\"status\":200}")));
    const AccessRecord fromHeaders = parseAccessRecord(withHeaders, journalSource(withHeaders));
    check(fromHeaders.clientIp == QLatin1String("2001:db8::1") &&
              fromHeaders.userAgent == QLatin1String("curl/8"),
          "remote_ip and the request header are the fallbacks");
    AccessRecord world = record;
    world.host = QStringLiteral("world.forkmesh.com");
    world.path = QStringLiteral("/assets/world.js");
    checkEqual(accessSiteGroup(world), QStringLiteral("world"), "world traffic");
    AccessRecord git = record;
    git.host = QStringLiteral("n3.next.forkmesh.com");
    git.path = QStringLiteral("/forkmesh/forkmesh/info/refs");
    checkEqual(accessSiteGroup(git), QStringLiteral("git"), "per-node name traffic is git");
    AccessRecord page = record;
    page.path = QStringLiteral("/forkmesh/forkmesh");
    checkEqual(accessSiteGroup(page), QStringLiteral("pages"), "app pages");
    const JournalEntry relay = parseJournalLine(journal(
        "caddy.service", "caddy", "6",
        QString(raw).replace(QStringLiteral("/api/repo"), QStringLiteral("/_app/api/repo"))));
    checkEqual(parseAccessRecord(relay, journalSource(relay)).path,
               QStringLiteral("/api/repo/forkmesh/forkmesh/about"),
               "the Worker's /_app relay counts as the App path");
    const JournalEntry notAccess = parseJournalLine(journal(
        "caddy.service", "caddy", "6",
        QStringLiteral("{\"level\":\"info\",\"logger\":\"tls\",\"msg\":\"x\"}")));
    check(!parseAccessRecord(notAccess, journalSource(notAccess)).valid,
          "other Caddy lines are not access records");

    const JournalEntry proxiedEntry = parseJournalLine(journal(
        "caddy.service", "caddy", "6",
        QStringLiteral("{\"logger\":\"http.log.access.log0\",\"request\":"
                       "{\"remote_ip\":\"172.71.203.9\",\"client_ip\":\"172.71.203.9\","
                       "\"method\":\"GET\",\"host\":\"app.forkmesh.com\","
                       "\"uri\":\"/forkmesh/forkmesh/info/refs\"},\"status\":503,"
                       "\"expected_degraded\":\"1\",\"cf_ip\":\"98.0.1.7\",\"cf_country\":\"us\","
                       "\"cf_region\":\"Virginia\",\"cf_city\":\"Ashburn\","
                       "\"edge_ip\":\"\",\"edge_country\":\"\",\"edge_where\":\"\"}")));
    AccessRecord proxied = parseAccessRecord(proxiedEntry, journalSource(proxiedEntry));
    checkEqual(proxied.clientIp, QStringLiteral("98.0.1.7"),
               "behind the Cloudflare proxy the visitor is Cf-Connecting-Ip");
    checkEqual(proxied.edgeIp, QStringLiteral("172.71.203.9"), "the edge is kept");
    checkEqual(proxied.country, QStringLiteral("US"), "Cloudflare's country is used");
    proxied.network = QStringLiteral("Charter Communications Inc, AS20115");
    checkEqual(accessLogMessage(proxied),
               QStringLiteral("GET app.forkmesh.com/forkmesh/forkmesh/info/refs -> 503 "
                              "degraded \u00b7 \U0001F1FA\U0001F1F8 98.0.1.7 (Ashburn, Virginia, US; "
                              "Charter Communications Inc, AS20115; via Cloudflare)"),
               "the visitor reads with place, network and the edge it came through");
    check(proxied.expectedDegraded, "the App's expected-degraded answer is recorded");
    checkEqual(accessLogCategory(proxied, false), QStringLiteral("CLOUD"),
               "an expected-degraded 5xx is not an alert");

    const JournalEntry relayedEntry = parseJournalLine(journal(
        "caddy.service", "caddy", "6",
        QStringLiteral("{\"logger\":\"http.log.access.log0\",\"request\":"
                       "{\"remote_ip\":\"104.23.190.4\",\"client_ip\":\"104.23.190.4\","
                       "\"method\":\"GET\",\"host\":\"app.forkmesh.com\","
                       "\"uri\":\"/forkmesh/forkmesh/info/refs\"},\"status\":200,"
                       "\"duration\":0.01,\"user_agent\":\"git/2.43.0\","
                       "\"cf_ip\":\"\",\"edge_ip\":\"2603:7000:8c00:1::5\","
                       "\"edge_country\":\"US\",\"edge_where\":"
                       "\"city=New+York&region=New%20York&asn=12271&org=Charter+%26+Co%3B(x)\"}")));
    const AccessRecord relayed2 = parseAccessRecord(relayedEntry, journalSource(relayedEntry));
    checkEqual(relayed2.clientIp, QStringLiteral("2603:7000:8c00:1::5"),
               "a relayed request names the visitor from x-forkmesh-edge-client");
    checkEqual(relayed2.relay, QStringLiteral("forkmesh.com"), "the relay is the Worker");
    checkEqual(relayed2.network, QStringLiteral("Charter & Cox, AS12271"),
               "the Worker's AS organisation and number, separators dropped");
    checkEqual(accessLogMessage(relayed2),
               QStringLiteral("GET app.forkmesh.com/forkmesh/forkmesh/info/refs -> 200 10ms "
                              "\u00b7 \U0001F1FA\U0001F1F8 2603:7000:8c00:1::5 (New York, US; "
                              "Charter & Cox, AS12271; via forkmesh.com) \u00b7 git/2.43.0"),
               "one place when city and region match, and the User-Agent stays last");
    const QString relayedHtml = nodeLineHtml(
        QStringLiteral("Node n1 core caddy info: ") + accessLogMessage(relayed2), false);
    check(relayedHtml.contains(QLatin1String("#57606a'>git/2.43.0</span>")),
          "the User-Agent is still muted after the visitor details");
    check(relayedHtml.contains(QLatin1String("#57606a'>via forkmesh.com</span>")),
          "the relay is muted");

    AccessRecord fleetPeer = proxied;
    fleetPeer.clientLabel = QStringLiteral("n2");
    fleetPeer.relay.clear();
    checkEqual(accessLogMessage(fleetPeer),
               QStringLiteral("GET app.forkmesh.com/forkmesh/forkmesh/info/refs -> 503 "
                              "degraded \u00b7 98.0.1.7 (n2)"),
               "a fleet node is only its name");
}

static void testIpWhois()
{
    using namespace forkmesh::ipwhois;
    const Info arin = parse(QStringLiteral(
        "# ARIN WHOIS data\n"
        "NetRange:       2603:7000:: - 2603:7fff:ffff:ffff:ffff:ffff:ffff:ffff\n"
        "CIDR:           2603:7000::/24\n"
        "NetName:        CHARTER-NET-V6\n"
        "OriginAS:       AS12271, AS20115\n"
        "Organization:   Charter Communications Inc (CC-3517)\n"
        "RegDate:        2016-04-14\n"
        "\n"
        "OrgName:        Charter Communications Inc\n"
        "Country:        us\n"
        "OrgAbuseEmail:  abuse@charter.net\n"));
    checkEqual(arin.owner, QStringLiteral("Charter Communications Inc"), "ARIN owner");
    checkEqual(arin.cidr, QStringLiteral("2603:7000::/24"), "ARIN CIDR");
    checkEqual(arin.asn, QStringLiteral("AS12271"), "the first origin AS");
    checkEqual(arin.country, QStringLiteral("US"), "country upper-cased");
    checkEqual(ownerLabel(arin), QStringLiteral("Charter Communications Inc, AS12271"),
               "owner and AS for the access line");
    checkEqual(describe(QStringLiteral("2603:7000:8c00::"), arin),
               QStringLiteral("whois 2603:7000:8c00:: \u00b7 owner: Charter Communications Inc "
                              "\u00b7 network: CHARTER-NET-V6 \u00b7 range: 2603:7000:: - "
                              "2603:7fff:ffff:ffff:ffff:ffff:ffff:ffff \u00b7 cidr: 2603:7000::/24 "
                              "\u00b7 country: US \u00b7 AS: AS12271 \u00b7 abuse: abuse@charter.net "
                              "\u00b7 registered: 2016-04-14"),
               "the whois line lists every fact");

    const Info ripe = parse(QStringLiteral(
        "NetRange:       185.0.0.0 - 185.255.255.255\n"
        "Organization:   RIPE Network Coordination Centre (RIPE)\n"
        "\n"
        "% This is the RIPE Database query service.\n"
        "inetnum:        185.199.108.0 - 185.199.111.255\n"
        "netname:        US-GITHUB-20170413\n"
        "descr:          GitHub, Inc.\n"
        "descr:          88 Colin P Kelly Jr St\n"
        "country:        US\n"
        "\n"
        "route:          185.199.108.0/24\n"
        "origin:         AS54113\n"));
    checkEqual(ripe.range, QStringLiteral("185.199.108.0 - 185.199.111.255"),
               "the referred registry's block wins");
    checkEqual(ripe.about, QStringLiteral("GitHub, Inc."), "the first descr line");
    checkEqual(ownerLabel(ripe), QStringLiteral("GitHub, Inc., AS54113"),
               "a registry is never the owner");
    check(parse(QStringLiteral("% no entries found\n")).isEmpty(), "no answer, nothing");

    check(worthLookingUp(QStringLiteral("98.0.1.7")) &&
              worthLookingUp(QStringLiteral("2603:7000:8c00::")),
          "public addresses are looked up");
    for (const char *address : {"10.1.2.3", "192.168.1.1", "127.0.0.1", "100.64.3.4",
                                "::1", "fd00::1", "fe80::1", "::ffff:10.0.0.1", "nope"})
        check(!worthLookingUp(QString::fromLatin1(address)), address);
    checkEqual(cacheKey(QStringLiteral("98.0.1.7")), QStringLiteral("98.0.1.0/24"),
               "IPv4 addresses share a /24");
    checkEqual(cacheKey(QStringLiteral("2603:7000:8c00:1::5")),
               QStringLiteral("2603:7000:8c00::/48"), "IPv6 addresses share a /48");
    checkEqual(cacheKey(QStringLiteral("::ffff:98.0.1.7")), QStringLiteral("98.0.1.0/24"),
               "a mapped address is IPv4");
}

static void testIpCountry()
{
    using namespace forkmesh::ipcountry;
    Table table;
    const int added = appendCsv(table, QByteArrayLiteral(
        "1.0.4.0,1.0.7.255,AU\n"
        "1.0.0.0,1.0.0.255,AU\r\n"
        "1.0.1.0,1.0.3.255,CN\n"
        "2001::,2001::ffff:ffff:ffff:ffff:ffff:ffff,US\n"
        "2a00:1450::,2a00:1450:ffff:ffff:ffff:ffff:ffff:ffff,IE\n"
        "5.0.0.0,5.0.0.255,ZZ\n"
        "bogus line\n"
        "9.9.9.9,9.9.9.0,US\n"));
    finish(table);
    check(added == 5, "valid rows are kept, unknown, reversed and junk rows are not");
    checkEqual(lookup(table, QStringLiteral("1.0.2.9")), QStringLiteral("CN"), "v4 inside a range");
    checkEqual(lookup(table, QStringLiteral("1.0.0.0")), QStringLiteral("AU"), "v4 first address");
    checkEqual(lookup(table, QStringLiteral("1.0.7.255")), QStringLiteral("AU"), "v4 last address");
    check(lookup(table, QStringLiteral("1.0.8.0")).isEmpty(), "v4 past the last range");
    check(lookup(table, QStringLiteral("0.255.255.255")).isEmpty(), "v4 before the first range");
    check(lookup(table, QStringLiteral("5.0.0.1")).isEmpty(), "ZZ is not a country");
    checkEqual(lookup(table, QStringLiteral("::ffff:1.0.1.1")), QStringLiteral("CN"),
               "a v4-mapped v6 address reads as v4");
    checkEqual(lookup(table, QStringLiteral("2a00:1450:4001:80b::200e")), QStringLiteral("IE"),
               "v6 inside a range");
    check(lookup(table, QStringLiteral("2a01::1")).isEmpty(), "v6 in a gap");
    check(lookup(table, QStringLiteral("not an ip")).isEmpty(), "junk has no country");
    check(csvLooksValid(QByteArrayLiteral("1.0.0.0,1.0.0.255,AU\n")) &&
              !csvLooksValid(QByteArrayLiteral("<html>\n")),
          "a download must start with a CSV row");
}

static void testNodeGroups()
{
    const auto entry = [](const char *unit, const char *ident,
                          const char *priority, const QString &message) {
        return parseJournalLine(journal(unit, ident, priority, message));
    };
    const auto classify = [](const JournalEntry &e) {
        return classifyEntry(e, journalSource(e));
    };
    const JournalEntry started = entry("init.scope", "systemd", "6",
                                       QStringLiteral("Started fmnext-cert-sync.service."));
    check(classify(started).forward, "an info systemd line reaches the main log");
    checkEqual(classify(started).unit, QStringLiteral("systemd"),
               "PID 1's init.scope reads as systemd");
    const JournalEntry kernel = entry("", "kernel", "6",
                                      QStringLiteral("eth0: link up"));
    check(classify(kernel).forward, "an info kernel line reaches the main log");
    const JournalEntry session = entry("init.scope", "systemd", "6",
                                       QStringLiteral("Started session-42.scope - Session 42 of User root."));
    check(classify(session).forward, "systemd's session lines reach the main log too");

    const JournalEntry daemon = entry(
        "forkmesh-mirror-node.service", "forkmesh-mirror-node", "6",
        QStringLiteral("{\"bytes\":29225,\"durationMs\":9,\"operation\":\"git-upload-pack\","
                       "\"requestId\":\"\",\"status\":200,\"time\":\"2026-09-24T23:47:18Z\"}"));
    checkEqual(displayMessage(daemon),
               QString::fromUtf8("git-upload-pack 200 \xC2\xB7 9 ms \xC2\xB7 28.5 KiB"),
               "a daemon request record reads as operation, status, time and size");

    const auto badge = [](const char *line) {
        return nodeLineBadge(QString::fromUtf8(line));
    };
    checkEqual(badge("Node n1 core forkmesh-app info: slow request 272ms GET /x"),
               QStringLiteral("APP"), "app lines are APP");
    checkEqual(badge("Node n1 core forkmesh-app info: cron minute 1 ran on n1"),
               QStringLiteral("CRON"), "app cron lines are CRON");
    checkEqual(badge("Node n2 core caddy warn: tls: x"), QStringLiteral("WEB"),
               "Caddy is WEB");
    checkEqual(badge("Node r1 runner forkmesh-mirror-node info: x"),
               QStringLiteral("DAEMON"), "the Go daemon is DAEMON");
    checkEqual(badge("Node n3 core fmnext-ids info: x"), QStringLiteral("IDS"),
               "the IDS is IDS");
    checkEqual(badge("Node n3 core fmnext-cert-sync info: x"), QStringLiteral("CERT"),
               "cert sync is CERT");
    checkEqual(badge("Node n3 core fmnext-certsync info: x"), QStringLiteral("CERT"),
               "the certsync pair server is CERT");
    checkEqual(badge("Node n1 core earlyoom info: mem avail: 1%"), QStringLiteral("OPS"),
               "earlyoom is OPS");
    checkEqual(badge("Node b2 runner job info: x"), QStringLiteral("JOB"),
               "a burst runner's job scope is JOB");
    checkEqual(badge("Node n1 core postfix info: x"), QStringLiteral("MAIL"),
               "Postfix is MAIL");
    checkEqual(badge("Node n1 core fmnext-mail-events info: x"), QStringLiteral("MAIL"),
               "the mail event reader is MAIL, not OPS");
    checkEqual(badge("Node a1 core systemd info: x"), QStringLiteral("SYSTEM"),
               "an autoscaled core's systemd line is SYSTEM");
    checkEqual(badge("Node n1 core rqlite info: x"), QStringLiteral("DB"),
               "rqlite's own log is DB");
    checkEqual(badge("Node n1 core rqlite query: SELECT 1"), QStringLiteral("DB"),
               "a statement is DB");
    checkEqual(badge("Node n1 core feed info: connected"), QStringLiteral("CLOUD"),
               "the desktop's feed notes stay CLOUD");
    checkEqual(badge("Node n1 core caddy err: GET a/b -> 503"), QStringLiteral("ERROR"),
               "a live error is ERROR");
    checkEqual(badge("Node n1 core caddy err: GET a/b -> 503 "
                     "(logged 2026-09-25 01:00:00, replayed)"),
               QStringLiteral("WEB"), "a replayed error keeps its group, not ERROR");
    checkEqual(nodeLineBadge(QStringLiteral("Node n1 core caddy err: x"), false),
               QStringLiteral("WEB"), "the group can be read without the error");
    check(nodeLineBadge(QStringLiteral("Network: sent")).isEmpty(),
          "a desktop line has no node group");
    check(isFleetBadge(QStringLiteral("CRON")) && isFleetBadge(QStringLiteral("DB")) &&
              isFleetBadge(QStringLiteral("MAIL")) && isFleetBadge(QStringLiteral("DEPLOY")) &&
              !isFleetBadge(QStringLiteral("ERROR")),
          "fleet groups are known; ERROR is not one");

    const QString html = nodeLineHtml(
        QString::fromUtf8("Node n2 core rqlite query: SELECT id FROM users WHERE name = ? "
                          "[1 arg] \xC2\xB7 from account_users.py in load <x>"),
        true, QStringLiteral("#f778ba"));
    check(html.contains(QLatin1String(">n2</span>")), "the node name is its own span");
    check(html.contains(QLatin1String("font-weight:600'>SELECT</span>")),
          "SQL keywords are highlighted");
    check(html.contains(QLatin1String("&lt;x&gt;")) && !html.contains(QLatin1String("<x>")),
          "the body is escaped");
    const QString daemonHtml = nodeLineHtml(
        QString::fromUtf8("Node n1 core forkmesh-mirror-node info: control 503 \xC2\xB7 "
                          "1200 ms"),
        false);
    check(daemonHtml.contains(QLatin1String("#cf222e; font-weight:600'>503</span>")),
          "a 5xx status is red");
    check(daemonHtml.contains(QLatin1String("#cf222e'>1200 ms</span>")),
          "a slow duration is red");
    check(nodeLineHtml(QStringLiteral("Network: sent"), false).isEmpty(),
          "a desktop line is not rendered as a node line");
    check(parseNodeLinePrefix(QStringLiteral("Node a3 core forkmesh-app info: x")).valid &&
              parseNodeLinePrefix(QStringLiteral("Node b1 runner job info: x")).valid,
          "autoscaled and burst nodes are node lines");
}

static void testNodeLines()
{
    CoreNode n3{QStringLiteral("n3"), QStringLiteral("203.0.113.13")};
    CoreNode r1{QStringLiteral("r1"), QStringLiteral("203.0.113.21"),
                QStringLiteral("runner")};
    const QString line = nodeLogLine(n3, QStringLiteral("forkmesh-app"),
                                     QStringLiteral("err"),
                                     QStringLiteral("Traceback: KeyError: 'x'"));
    checkEqual(line, QStringLiteral("Node n3 core forkmesh-app err: Traceback: KeyError: 'x'"),
               "a node line names the node, its role, the unit and the severity");
    checkEqual(nodeLogLine(r1, QStringLiteral("job"), QStringLiteral("info"),
                           QStringLiteral("x"), 3),
               QStringLiteral("Node r1 runner job info: x (+3 identical lines folded)"),
               "folded repeats are reported on the next admitted line");
    const NodeLinePrefix prefix = parseNodeLinePrefix(line);
    check(prefix.valid && prefix.node == QLatin1String("n3") &&
              prefix.role == QLatin1String("core") &&
              prefix.unit == QLatin1String("forkmesh-app") &&
              prefix.severity == QLatin1String("err"),
          "the tags are read back from the stored line");
    check(!parseNodeLinePrefix(QStringLiteral("Node diagnostics: ok")).valid,
          "other lines starting with Node are not node lines");
    checkEqual(nodeLogLine(n3, QStringLiteral("bad unit;rm"), QStringLiteral("info"),
                           QStringLiteral("x")),
               QStringLiteral("Node n3 core bad_unit_rm info: x"),
               "a unit name cannot break the tag format");

    StatementLine statement;
    statement.valid = true;
    statement.kind = QStringLiteral("primary query");
    statement.sql = QStringLiteral("SELECT 1");
    statement.originFile = QStringLiteral("account_users.py");
    checkEqual(mainLogStatementLine(n3, statement),
               QString::fromUtf8("Node n3 core rqlite primary: SELECT 1 \xC2\xB7 from "
                                 "account_users.py"),
               "a statement is a node line under rqlite");
    statement.caller = QStringLiteral("cron");
    checkEqual(mainLogStatementLine(n3, statement),
               QString::fromUtf8("Node n3 core rqlite primary: SELECT 1 \xC2\xB7 from "
                                 "account_users.py \xC2\xB7 for cron"),
               "a statement names the caller that ran it");

    NodeLogFilter filter;
    check(filter.accepts(QStringLiteral("anything")), "no filter shows everything");
    filter.nodes.insert(QStringLiteral("n3"));
    check(filter.accepts(line), "the chosen node's line shows");
    check(!filter.accepts(QStringLiteral("Git: fetched")),
          "a node filter hides the desktop's own lines");
    check(!filter.accepts(nodeLogLine(r1, QStringLiteral("job"), QStringLiteral("err"),
                                      QStringLiteral("x"))),
          "another node's line is hidden");
    filter.units.insert(QStringLiteral("caddy"));
    check(!filter.accepts(line), "a unit filter narrows further");
    filter.units.clear();
    filter.errorsOnly = true;
    check(filter.accepts(line), "an error passes errors only");
    check(!filter.accepts(nodeLogLine(n3, QStringLiteral("caddy"), QStringLiteral("info"),
                                      QStringLiteral("x"))),
          "info is hidden by errors only");
}

static void testStatements()
{
    const JournalEntry entry = parseJournalLine(journal(
        "forkmesh-app.service", "python", "6",
        QStringLiteral("D1 primary query: SELECT data FROM accounts WHERE "
                       "key_bi = ? [1 arg] (from account_users.py in "
                       "_account_row)")));
    const StatementLine statement = parseStatementLine(entry, journalSource(entry));
    check(statement.valid, "a D1 line from the app is a statement");
    checkEqual(statement.kind, QStringLiteral("primary query"), "statement kind");
    checkEqual(statement.sql,
               QStringLiteral("SELECT data FROM accounts WHERE key_bi = ? [1 arg]"),
               "statement text keeps the argument count");
    checkEqual(statement.originFile, QStringLiteral("account_users.py"),
               "origin module");
    checkEqual(statement.originFunction, QStringLiteral("_account_row"),
               "origin function");

    const JournalEntry called = parseJournalLine(journal(
        "forkmesh-app.service", "python", "6",
        QStringLiteral("D1 query: SELECT id FROM pulls WHERE repo IN (?) "
                       "[1 arg] (for GET /api/pulls, IP 203.0.113.9, user "
                       "alice, session abcd1234) (from repositories.py in "
                       "list_pulls)")));
    const StatementLine calledStatement =
        parseStatementLine(called, journalSource(called));
    checkEqual(calledStatement.sql,
               QStringLiteral("SELECT id FROM pulls WHERE repo IN (?) [1 arg]"),
               "the caller clause is not part of the statement");
    checkEqual(calledStatement.caller,
               QStringLiteral("GET /api/pulls, IP 203.0.113.9, user alice, "
                              "session abcd1234"),
               "the caller clause names the request, user and session");
    checkEqual(calledStatement.originFunction, QStringLiteral("list_pulls"),
               "the origin still parses after a caller clause");
    checkEqual(statement.caller, QString(), "a line without a caller has none");

    const JournalEntry rqlite = parseJournalLine(journal(
        "rqlite.service", "rqlited", "6", QStringLiteral("D1 query: SELECT 1")));
    check(!parseStatementLine(rqlite, journalSource(rqlite)).valid,
          "only the app's lines are statements");
}

static void testWebRequestBatches()
{
    const JournalEntry entry = parseJournalLine(journal(
        "forkmesh-app.service", "python", "6",
        QStringLiteral("web-requests {\"t\":1790275060000,\"s\":10,\"r\":["
                       "[\"accounts/*\",\"2xx\",3,42,30],"
                       "[\"version\",\"5xx\",1,7,7],"
                       "[\"\",\"2xx\",1,1,1],[\"bad\"]]}")));
    const WebRequestBatch batch =
        parseWebRequestBatch(entry, journalSource(entry));
    check(batch.valid, "a web-requests line from the app is a batch");
    check(batch.windowStartMs == 1790275060000LL && batch.windowSeconds == 10,
          "the batch keeps its window");
    check(batch.rows.size() == 2, "malformed and unnamed rows are skipped");
    if (batch.rows.size() == 2) {
        checkEqual(batch.rows.at(0).group, QStringLiteral("accounts/*"),
                   "row group");
        checkEqual(batch.rows.at(0).statusClass, QStringLiteral("2xx"),
                   "row status class");
        check(batch.rows.at(0).count == 3 &&
                  batch.rows.at(0).durationSumMs == 42 &&
                  batch.rows.at(0).durationMaxMs == 30,
              "row counts and durations");
    }
    check(!parseStatementLine(entry, journalSource(entry)).valid,
          "a batch is not a statement");

    const JournalEntry caddy = parseJournalLine(journal(
        "caddy.service", "caddy", "6",
        QStringLiteral("web-requests {\"t\":1,\"s\":10,\"r\":[]}")));
    check(!parseWebRequestBatch(caddy, journalSource(caddy)).valid,
          "only the app reports web requests");
    const JournalEntry plain = parseJournalLine(journal(
        "forkmesh-app.service", "python", "6",
        QStringLiteral("slow request 300ms GET /api/version")));
    check(!parseWebRequestBatch(plain, journalSource(plain)).valid,
          "other app lines are not batches");
}

static void testRepeatFilter()
{
    RepeatFilter filter;
    const QString first = QStringLiteral(
        "[http] 2026/09/24 18:35:29 failed to retrieve stats for registered "
        "reporter network");
    const QString second = QStringLiteral(
        "[http] 2026/09/24 18:35:31 failed to retrieve stats for registered "
        "reporter network");
    check(filter.admit(QStringLiteral("n1"), QStringLiteral("rqlite"), first, 0).admit, "a new line is admitted");
    check(!filter.admit(QStringLiteral("n1"), QStringLiteral("rqlite"), second, 1000).admit,
          "the same line with a new stamp is folded");
    check(filter.admit(QStringLiteral("n2"), QStringLiteral("rqlite"), second, 1000).admit,
          "another core's copy is its own line");
    const RepeatVerdict later =
        filter.admit(QStringLiteral("n1"), QStringLiteral("rqlite"), first, kRepeatWindowMs + 1);
    check(later.admit && later.foldedRepeats == 1,
          "after the window the line returns with its folded count");
}

static void testTallyAndRender()
{
    ActivityTally tally(2);
    const qint64 now = 1790275067000LL;
    tally.noteFeedState(QStringLiteral("n1"), true, QStringLiteral("following"),
                        now);
    tally.noteFeedState(QStringLiteral("n2"), false,
                        QStringLiteral("retrying in 30 s (exit 255)"), now);
    StatementLine a;
    a.valid = true;
    a.kind = QStringLiteral("query");
    a.sql = QStringLiteral("SELECT <b> FROM t");
    a.originFile = QStringLiteral("entry.py");
    StatementLine b = a;
    b.sql = QStringLiteral("UPDATE t SET x = ?");
    StatementLine c = a;
    c.sql = QStringLiteral("DELETE FROM t");
    tally.noteStatement(QStringLiteral("n1"), a, now);
    tally.noteStatement(QStringLiteral("n2"), a, now);
    tally.noteStatement(QStringLiteral("n1"), b, now);
    tally.noteStatement(QStringLiteral("n1"), c, now);
    tally.noteLine(QStringLiteral("n1"), QStringLiteral("app"), true, false,
                   true, now);
    check(tally.totalStatements() == 4, "every statement is counted");
    check(tally.overflowStatements() == 1,
          "statements past the capacity go to the overflow count");
    const QList<StatementRow> top = tally.topStatements(10);
    check(top.size() == 2 && top.at(0).count == 2 &&
              top.at(0).perNode.value(QStringLiteral("n2")) == 1,
          "statements rank by runs with per-node counts");
    const auto minutes = tally.timeline(now);
    check(minutes.size() == kTimelineMinutes &&
              minutes.last().second.value(QStringLiteral("n1")) == 3,
          "the per-minute timeline counts the current minute");

    QHash<QString, StatementLink> links;
    links.insert(statementLinkKey(a.originFile, a.sql),
                 {QStringLiteral("forkmesh-source:app/src/entry.py:10"),
                  QStringLiteral("app/src/entry.py:10")});
    const QString html = renderActivityHtml(tally, now, true, QString(), links);
    check(html.contains(QLatin1String("SELECT &lt;b&gt; FROM t")),
          "statement text is escaped");
    check(html.contains(QLatin1String("app/src/entry.py:10")),
          "a resolved statement links to its source");
    check(html.contains(QLatin1String("retrying in 30 s")),
          "the feed state is shown per core");
    check(html.contains(QLatin1String("Statements per minute")),
          "the per-minute chart is drawn");

    const QString empty = renderActivityHtml(ActivityTally(), now, false,
                                             QStringLiteral("switched off"), {});
    check(empty.contains(QLatin1String("FORKMESH_HOST_D1_LOG=1")),
          "an empty ranking says how statements are switched on");
    check(empty.contains(QLatin1String("switched off")),
          "the idle reason is shown");
}

static void testLineClientDetails()
{
    const JournalEntry entry = parseJournalLine(journal(
        "forkmesh-app.service", "python", "6",
        QStringLiteral("D1 query: SELECT data FROM push_devices WHERE owner = ? "
                       "[3 args] (for GET /api/push/alert, IP "
                       "2603:7000:8c00:1::5, user jett, "
                       "session AbCdEfGh) (from entry.py in push_alert_handler)")));
    const StatementLine statement = parseStatementLine(entry, journalSource(entry));
    const CoreNode n3{QStringLiteral("n3"), QStringLiteral("66.135.17.108")};
    const QString line = mainLogStatementLine(n3, statement);
    check(lineClientAddresses(line) ==
              QStringList{QStringLiteral("2603:7000:8c00:1::5")},
          "the client address is read out of an rqlite query line");

    QStringList asked;
    const auto describe = [&asked](const QString &address) {
        asked << address;
        ClientDetails details;
        if (address.startsWith(QLatin1String("2603:"))) {
            details.country = QStringLiteral("US");
            details.network = QStringLiteral("Charter Communications Inc");
        } else if (address == QLatin1String("127.0.0.1")) {
            details.label = QStringLiteral("local");
            details.country = QStringLiteral("US");
        } else if (address == QLatin1String("10.77.0.4")) {
            details.label = QStringLiteral("fleet");
            details.network = QStringLiteral("ignored");
        } else if (address == QLatin1String("162.158.63.9")) {
            details.label = QStringLiteral("Cloudflare");
            details.country = QStringLiteral("DE");
            details.network = QStringLiteral("Cloudflare, AS13335");
        }
        return details;
    };
    checkEqual(withClientDetails(line, describe),
               QString::fromUtf8(
                   "Node n3 core rqlite query: SELECT data FROM push_devices WHERE "
                   "owner = ? [3 args] \xC2\xB7 from entry.py in push_alert_handler "
                   "\xC2\xB7 for GET /api/push/alert, IP \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 "
                   "2603:7000:8c00:1::5 (Charter Communications "
                   "Inc), user jett, session AbCdEfGh"),
               "an rqlite line gets the flag and provider after IP");

    StatementLine primary = statement;
    primary.kind = QStringLiteral("primary query");
    primary.caller = QStringLiteral("GET /forkmesh/forkmesh/info/refs, IP 127.0.0.1");
    checkEqual(withClientDetails(mainLogStatementLine(n3, primary), describe)
                   .section(QString::fromUtf8("\xC2\xB7 for "), 1),
               QStringLiteral("GET /forkmesh/forkmesh/info/refs, IP 127.0.0.1 (local)"),
               "a loopback client on an rqlite primary line is local, with no flag");
    checkEqual(withClientDetails(QStringLiteral("sync from IP 10.77.0.4 done"), describe),
               QStringLiteral("sync from IP 10.77.0.4 (fleet) done"),
               "a mesh address is fleet, with no provider");
    checkEqual(withClientDetails(QStringLiteral("hit IP 162.158.63.9."), describe),
               QString::fromUtf8("hit IP \xF0\x9F\x87\xA9\xF0\x9F\x87\xAA 162.158.63.9 "
                                 "(Cloudflare; Cloudflare, AS13335)."),
               "a Cloudflare address keeps its flag, and a trailing full stop "
               "ends the address");
    checkEqual(withClientDetails(QStringLiteral("IP 198.51.100.7"), describe),
               QStringLiteral("IP 198.51.100.7"),
               "an address with nothing known is left as it was");

    asked.clear();
    const QString plain = QStringLiteral(
        "SELECT a FROM IP WHERE x = ? \u00b7 IP 1.2 \u00b7 VIP 1.2.3.4 "
        "\u00b7 IP 1.2.3.4x \u00b7 IP deadbeef");
    checkEqual(withClientDetails(plain, describe), plain,
               "text that is not an address after IP is untouched");
    check(lineClientAddresses(plain).isEmpty() && asked.isEmpty(),
          "no lookup runs for text without a client address");
    check(lineClientAddresses(QStringLiteral("no address here")).isEmpty(),
          "a line without IP has no client address");
    check(lineClientAddresses(QStringLiteral("IP 203.0.113.9, IP 2001:db8::1)")) ==
              QStringList({QStringLiteral("203.0.113.9"), QStringLiteral("2001:db8::1")}),
          "every IP field is found");

    const QString html = nodeLineHtml(
        withClientDetails(line, describe), false);
    check(html.contains(QLatin1String("Noto Color Emoji")),
          "the flag on an rqlite line renders with the emoji font");
    check(html.contains(QLatin1String("(Charter Communications Inc)</span>")),
          "the provider on an rqlite line is muted as one run");
}

static void testNodeVitals()
{
    const QByteArray line =
        R"({"logread_vitals":1790288943000,"cpus":2,"cpu_busy":37.5,)"
        R"("load":[0.52,0.4,0.31],"procs_running":3,"procs_total":187,)"
        R"("mem_total":1048576000,"mem_available":262144000,)"
        R"("swap_total":524288000,"swap_free":262144000,)"
        R"("disk_total":26843545600,"disk_free":10737418240,)"
        R"("inodes_total":1600000,"inodes_free":1400000,)"
        R"("files_open":2300,"files_max":9223372,"uptime":86461})";
    check(parseReaderControl(line).kind == ReaderControl::Vitals,
          "a vitals sample is a reader control line, not a journal entry");
    check(!parseJournalLine(line).valid,
          "an older desktop drops a vitals sample as a non-entry");
    const NodeVitals vitals = parseNodeVitals(line);
    check(vitals.valid && vitals.atMs == 1790288943000LL && vitals.cpus == 2,
          "the sample's stamp and CPU count are read");
    check(qFuzzyCompare(vitals.cpuBusy, 37.5) &&
              qFuzzyCompare(vitals.load1, 0.52) && vitals.procsTotal == 187,
          "CPU busy share, load and process counts are read");
    check(vitals.memTotal == 1048576000LL && vitals.memAvailable == 262144000LL &&
              vitals.swapFree == 262144000LL,
          "memory and swap are read in bytes");
    check(vitals.diskFree == 10737418240LL && vitals.inodesFree == 1400000 &&
              vitals.filesOpen == 2300 && vitals.uptimeSeconds == 86461,
          "disk, inodes, open files and uptime are read");
    const NodeVitals partial =
        parseNodeVitals(R"({"logread_vitals":5,"cpus":1})");
    check(partial.valid && partial.memTotal == -1 && partial.cpuBusy < 0 &&
              partial.load1 < 0,
          "fields a node did not send stay unknown");
    check(!parseNodeVitals(R"({"logread_idle":5})").valid &&
              !parseNodeVitals(journal("app.service", "python3", "6",
                                       QStringLiteral("logread_vitals")))
                   .valid,
          "only a vitals sample parses as one");
}

static void testStatementShapes()
{
    const auto shape = [](const char *sql) {
        return statementShape(QString::fromLatin1(sql));
    };
    check(shape("SELECT id FROM pulls WHERE repo IN (?)").verb == "SELECT" &&
              shape("SELECT id FROM pulls WHERE repo IN (?)").table == "pulls",
          "a select names its table");
    check(shape("insert or replace into push_devices (owner) values (?)")
                  .table == "push_devices" &&
              shape("insert or replace into push_devices (owner) values (?)")
                      .verb == "INSERT",
          "an upsert is an insert into its table");
    check(shape("REPLACE INTO sessions VALUES (?)").verb == "INSERT",
          "REPLACE counts as an insert");
    check(shape("UPDATE OR IGNORE users SET name = ?").table == "users",
          "an UPDATE OR IGNORE names the table after the conflict clause");
    check(shape("DELETE FROM host_metrics WHERE node_id=?").verb == "DELETE" &&
              shape("DELETE FROM host_metrics WHERE node_id=?").table ==
                  "host_metrics",
          "a delete names its table");
    check(shape("SELECT COUNT(*) FROM (SELECT 1 FROM tasks) AS t").table ==
              "tasks",
          "a subquery in FROM is looked through");
    check(shape("WITH r AS (SELECT id FROM repos) SELECT * FROM r").verb ==
                  "SELECT" &&
              shape("WITH r AS (SELECT id FROM repos) SELECT * FROM r")
                      .table == "repos",
          "a CTE keeps its body's verb and its first real table");
    check(shape("SELECT value FROM json_each(?)").table.isEmpty(),
          "a table-valued function is not a table");
    check(shape("CREATE TABLE IF NOT EXISTS locks (k TEXT)").verb == "CREATE" &&
              shape("CREATE TABLE IF NOT EXISTS locks (k TEXT)").table ==
                  "locks",
          "schema statements keep their verb and table");
    check(shape("SELECT 1").table.isEmpty() && shape("  vacuum").verb == "OTHER",
          "no table and unknown verbs stay empty and OTHER");
    check(nodeInkColour(QStringLiteral("n1")) == QLatin1String("#58a6ff") &&
              nodeInkColour(QStringLiteral("r1")) !=
                  nodeInkColour(QStringLiteral("n1")),
          "nodes keep the log's node colours");
}

int main()
{
    testFleetDiscovery();
    testNodeVitals();
    testStatementShapes();
    testReaderProtocol();
    testJournalParsing();
    testClassification();
    testTracebacks();
    testAccessLog();
    testIpCountry();
    testIpWhois();
    testNodeLines();
    testNodeGroups();
    testStatements();
    testLineClientDetails();
    testWebRequestBatches();
    testRepeatFilter();
    testTallyAndRender();
    if (failures == 0)
        std::fprintf(stderr, "core node log tests passed\n");
    return failures == 0 ? 0 : 1;
}

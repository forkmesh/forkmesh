// The Hosts live view multiplexes every log a host keeps onto one SSH
// connection, so two things have to hold: the remote script must actually open
// those taps, and every line it sends back must land under the right chip.
// Both are pure string work, so they are covered here rather than in the
// window suite.

#include "HostLogSources.h"

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cstdio>

using namespace forkmesh::hosts;

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

static void checkEqual(int actual, int expected, const char *what)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s\n  expected: %d\n  actual:   %d\n", what,
                     expected, actual);
        ++failures;
    }
}

static void testSourceCatalog()
{
    check(kLogSourceCount >= 2, "the catalogue has sources");
    QSet<QString> ids;
    for (int i = 0; i < kLogSourceCount; ++i) {
        const LogSource &source = kLogSources[i];
        const QString id = QString::fromLatin1(source.id);
        check(!ids.contains(id), "source ids are unique");
        ids.insert(id);
        check(logSourceIndex(id) == i, "logSourceIndex round-trips");
        // hostLiveEntryText pads the tag to six columns; a longer id would
        // break the alignment the pane relies on.
        check(id.size() <= 6, "source ids fit the six-column tag");
        check(qstrlen(source.icon) > 0, "every source names an octicon");
        check(QString::fromLatin1(source.accent).startsWith(QLatin1Char('#')),
              "every source has an accent colour");
        check(qstrlen(source.tip) > 0, "every source explains itself");
    }
    check(logSourceIndex(QStringLiteral("nope")) < 0,
          "an unknown id has no index");
    check(kFallbackLogSource >= 0 && kFallbackLogSource < kLogSourceCount,
          "the fallback source is in range");
    // Unattributable lines have to have somewhere to go.
    check(QString::fromLatin1(kLogSources[kFallbackLogSource].id) ==
              QLatin1String("host"),
          "notes fall back to the host source");
}

static void testIdentifierRouting()
{
    const struct {
        const char *identifier;
        const char *source;
    } cases[] = {
        {"forkmesh-mirror", "mirror"},
        {"forkmesh-mirror-node", "mirror"},
        {"forkmesh-hosted-repository-sync", "mirror"},
        {"caddy", "web"},
        {"nginx", "web"},
        {"cloudflared", "web"},
        {"sshd", "ssh"},
        {"sshd-session", "ssh"},
        {"ssh", "ssh"},
        {"systemd", "system"},
        {"kernel", "system"},
        {"cron", "system"},
    };
    for (const auto &probe : cases) {
        checkEqual(logSourceForIdentifier(QString::fromLatin1(probe.identifier)),
                   QString::fromLatin1(probe.source), probe.identifier);
        check(logSourceIndex(logSourceForIdentifier(
                  QString::fromLatin1(probe.identifier))) >= 0,
              "every routed identifier names a real source");
    }
}

static void testJournalLines()
{
    QString display;

    // journalctl -o short-iso, hostname column present.
    checkEqual(journalLogSource(
                   QStringLiteral("2026-09-17T15:12:52+0000 mirror35 "
                                  "forkmesh-mirror[812]: serving /health"),
                   &display),
               QStringLiteral("mirror"), "a mirror unit routes to Mirror");
    checkEqual(display,
               QStringLiteral("15:12:52 forkmesh-mirror: serving /health"),
               "the pane keeps the clock, the unit and the message");

    // Same line from a journalctl that was asked to drop the hostname.
    checkEqual(journalLogSource(
                   QStringLiteral("2026-09-17T15:12:52+0000 "
                                  "caddy[640]: GET /repo 200"),
                   &display),
               QStringLiteral("web"), "a hostname-less line still parses");
    checkEqual(display, QStringLiteral("15:12:52 caddy: GET /repo 200"),
               "a hostname-less line keeps its message");

    checkEqual(journalLogSource(
                   QStringLiteral("2026-09-17T15:13:09+0000 mirror37 "
                                  "sshd-session[2211]: Accepted publickey for "
                                  "root from 107.173.39.5"),
                   &display),
               QStringLiteral("ssh"), "an sshd variant routes to SSH");

    checkEqual(journalLogSource(
                   QStringLiteral("2026-09-17T15:13:09+0000 mirror37 "
                                  "kernel: [UFW BLOCK] IN=eth0"),
                   &display),
               QStringLiteral("system"), "the kernel routes to System");
    checkEqual(display, QStringLiteral("15:13:09 kernel: [UFW BLOCK] IN=eth0"),
               "a pid-less identifier keeps its name");

    // A message with runs of spaces must not be re-flowed.
    journalLogSource(QStringLiteral("2026-09-17T15:13:09+0000 mirror37 "
                                    "kernel:   indented   message"),
                     &display);
    checkEqual(display, QStringLiteral("15:13:09 kernel:   indented   message"),
               "message spacing survives");

    // Anything that does not parse is shown as it arrived rather than dropped.
    const char *const unparsed[] = {
        "-- No entries --",
        "",
        "    continuation of a wrapped message",
        "2026-09-17T15:13:09+0000 mirror37 no-colon-here",
    };
    for (const char *line : unparsed) {
        const QString raw = QString::fromLatin1(line);
        checkEqual(journalLogSource(raw, &display), QStringLiteral("system"),
                   line);
        checkEqual(display, raw.trimmed().isEmpty() ? raw.trimmed() : raw,
                   "an unparsed line is passed through");
    }
}

static void testRemoteScripts()
{
    const QString live = QString::fromUtf8(kLiveRemoteScript);
    // The bug this replaced: the pane followed only node.log, so a host that
    // runs ForkMesh from systemd showed one "no log yet" line and nothing else.
    check(live.contains(QLatin1String("journalctl")),
          "the live script follows the journal");
    check(live.contains(QLatin1String("-o short-iso")),
          "the journal feed uses the format journalLogSource parses");
    check(live.contains(QLatin1String("printf 'L node %s\\n'")),
          "the node log keeps its own tag");
    check(live.contains(QLatin1String("printf 'J %s\\n'")),
          "journal lines are tagged for classification");
    // Every note the script prints has to name a source the pane knows.
    for (const QString &line : live.split(QLatin1Char('\n'))) {
        const int note = line.indexOf(QLatin1String("printf 'M "));
        if (note < 0)
            continue;
        const QString tail = line.mid(note + 10);
        const QString id = tail.left(tail.indexOf(QLatin1Char(' ')));
        check(logSourceIndex(id) >= 0, qPrintable(
            QStringLiteral("note tag '%1' names a real source").arg(id)));
    }
    // The feeds are backgrounded and reaped, so leaving the view does not
    // leave a journalctl -f behind on every mirror.
    check(live.contains(QLatin1String("kill \"$jp\"")),
          "the journal feed is reaped when the shell goes");
    check(live.contains(QLatin1String("tail --pid=\"$me\"")),
          "the node tail follows the sampling shell");
    // The sampler the traffic and connection readouts depend on is untouched.
    check(live.contains(QLatin1String("/proc/net/dev")), "traffic is sampled");
    check(live.contains(QLatin1String("state established")),
          "connections are sampled");

    const QString feed = QString::fromUtf8(kMainLogFeedScript);
    check(feed.contains(QLatin1String("-o short-iso")) &&
              feed.contains(QLatin1String("printf 'J %s\\n'")) &&
              feed.contains(QLatin1String("printf 'L node %s\\n'")),
          "the main-log feed follows the node log and the journal");
    check(feed.contains(QLatin1String("kill \"$jp\"")) &&
              feed.contains(QLatin1String("tail --pid=\"$me\"")),
          "the main-log feed reaps its taps when the channel closes");
    check(feed.contains(QLatin1String("echo K")),
          "the main-log feed writes keepalives for the stall watchdog");
    check(!feed.contains(QLatin1String("state established")),
          "the main-log feed does not sample connections");
    for (const QString &line : feed.split(QLatin1Char('\n'))) {
        const int note = line.indexOf(QLatin1String("printf 'M "));
        if (note < 0)
            continue;
        const QString tail = line.mid(note + 10);
        check(logSourceIndex(tail.left(tail.indexOf(QLatin1Char(' ')))) >= 0,
              "feed note tags name a real source");
    }

    const QString single = QString::fromUtf8(kSingleHostLogScript);
    check(single.contains(QLatin1String("journalctl")),
          "the single-host dialog follows the journal too");
    check(!single.contains(QLatin1String("exit 1")),
          "a missing node log no longer ends the single-host stream");
}

static void testMainLogForwarding()
{
    // Every source a host has reaches the main log, not just ForkMesh's own.
    for (int source = 0; source < kLogSourceCount; ++source) {
        check(forwardsToMainLog(source),
              qPrintable(QStringLiteral("%1 reaches the main log")
                             .arg(QString::fromLatin1(kLogSources[source].id))));
    }
    check(!forwardsToMainLog(-1), "an unknown source is not forwarded");
    checkEqual(mainLogLine(QStringLiteral("mirror35"),
                           logSourceIndex(QStringLiteral("ssh")),
                           QStringLiteral("Accepted publickey for root")),
               QStringLiteral("Mirror mirror35 ssh: Accepted publickey for root"),
               "a forwarded line names its host and source");

    QString text;
    checkEqual(feedLogLine(QStringLiteral("L node pinned refs"), &text),
               logSourceIndex(QStringLiteral("node")), "node line");
    checkEqual(text, QStringLiteral("pinned refs"), "node line text");
    checkEqual(feedLogLine(QStringLiteral("M host journalctl is missing"), &text),
               kFallbackLogSource, "note");
    checkEqual(text, QStringLiteral("journalctl is missing"), "note text");
    checkEqual(feedLogLine(QStringLiteral("M bogus note"), &text),
               kFallbackLogSource, "an unknown tag falls back to the notes");
    checkEqual(text, QStringLiteral("bogus note"), "unknown tag text kept whole");
    checkEqual(
        feedLogLine(QStringLiteral("J 2026-09-17T15:12:52+0000 mirror35 "
                                   "sshd-session[99]: Accepted publickey"),
                    &text),
        logSourceIndex(QStringLiteral("ssh")), "journal line is classified");
    checkEqual(text, QStringLiteral("15:12:52 sshd-session: Accepted publickey"),
               "journal line text");
    checkEqual(feedLogLine(QStringLiteral("J -- No entries --"), &text),
               logSourceIndex(QStringLiteral("system")), "journal notice");
    for (const char *sampler : {"K", "S", "E", "N 1.0 2 3", "C 1 2 a b"}) {
        checkEqual(feedLogLine(QString::fromLatin1(sampler), &text), -1,
                   "keepalives and sampler rows are not log lines");
    }

    // Byte-for-byte what tools/mirror_gateway.py node_logs_message verifies.
    checkEqual(nodeLogsMessage(QStringLiteral("mirror35"),
                               QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
                               QStringLiteral("0123456789abcdef"),
                               2000000000000),
               QStringLiteral("forkmesh-node-logs-v1\nmirror35\n"
                              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
                              "0123456789abcdef\n2000000000000"),
               "the node-logs capability message");

    ForwardedHostLines forwarded(2);
    const QString a = QStringLiteral("15:12:52 forkmesh-mirror-node: synced");
    const QString b = QStringLiteral("15:12:53 forkmesh-mirror-node: pinned");
    const QString c = QStringLiteral("15:12:54 forkmesh-mirror-node: served");
    check(forwarded.firstSighting(QStringLiteral("mirror35"), a),
          "a new line is forwarded");
    check(!forwarded.firstSighting(QStringLiteral("mirror35"), a),
          "a replayed line is skipped");
    check(forwarded.firstSighting(QStringLiteral("mirror36"), a),
          "hosts are remembered apart");
    check(forwarded.firstSighting(QStringLiteral("mirror35"), b), "second line");
    check(forwarded.firstSighting(QStringLiteral("mirror35"), c), "third line");
    check(forwarded.firstSighting(QStringLiteral("mirror35"), a),
          "memory per host is bounded");
}

int main()
{
    testSourceCatalog();
    testIdentifierRouting();
    testJournalLines();
    testRemoteScripts();
    testMainLogForwarding();
    if (failures == 0)
        std::fprintf(stderr, "host log source tests passed\n");
    return failures == 0 ? 0 : 1;
}

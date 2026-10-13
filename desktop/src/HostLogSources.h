#pragma once

// The taps behind the Hosts live view. One SSH connection per host carries
// every log the host keeps — the ForkMesh node log file and the systemd
// journal — multiplexed onto one stdout, so the pane needs a way to say which
// line came from where. These ids are that answer: the remote script writes
// them as a wire tag, the pane counts lines under them, and each one gets a
// chip with an icon and a badge that hides its lines again.
//
// Core only and header-only on purpose: the journal classifier is the part
// most likely to meet an unfamiliar line, and it stays unit-testable without
// linking the window.

#include <QChar>
#include <QHash>
#include <QLatin1Char>
#include <QLatin1String>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QtGlobal>

#include <iterator>

namespace forkmesh {
namespace hosts {

struct LogSource {
    const char *id;     // wire tag and pane key
    const char *label;  // chip caption
    const char *icon;   // octicon shipped in resources/icons.qrc
    const char *accent; // chip colour once the source has produced a line
    const char *tip;    // chip tooltip
};

// Ordered as they read left to right under the pane's "Output" caption:
// ForkMesh's own logs first, the host's supporting services next, and the
// stream's own notes last.
inline constexpr LogSource kLogSources[] = {
    {"node", "Node", "terminal", "#2ea043",
     "The ForkMesh node's own log file on the host "
     "(~/.local/share/forkmesh/node.log)"},
    {"mirror", "Mirror", "broadcast", "#58a6ff",
     "Journal entries from the host's forkmesh* systemd services — the mirror "
     "gateway, the mirror node and the refresh timers"},
    {"web", "Web", "cloud", "#f6821f",
     "Journal entries from the host's web front ends — Caddy, nginx and "
     "cloudflared"},
    {"ssh", "SSH", "key", "#a371f7",
     "Journal entries from sshd — logins, sessions and authentication "
     "failures, including this connection"},
    {"system", "System", "server", "#8b949e",
     "Everything else in the host's journal, the kernel included"},
    {"host", "Host", "info", "#d29922",
     "Notes from the stream itself: a missing log, an SSH error, the login "
     "shell's own output"},
};

inline constexpr int kLogSourceCount = int(std::size(kLogSources));

// Notes, SSH errors and anything that cannot be attributed land here.
inline constexpr int kFallbackLogSource = kLogSourceCount - 1;

inline int logSourceIndex(const QString &id)
{
    for (int i = 0; i < kLogSourceCount; ++i) {
        if (id == QLatin1String(kLogSources[i].id))
            return i;
    }
    return -1;
}

// Every line a host sends — its node log and its whole journal, the web front
// ends, sshd, systemd and the kernel included, plus the stream's own notes — is
// copied into the main log as well, under the MIRROR badge and named by host
// and source, so one place shows everything happening across the system.
inline bool forwardsToMainLog(int source)
{
    return source >= 0 && source < kLogSourceCount;
}

// The main-log text for one forwarded line: "Mirror mirror35 ssh: <line>".
inline QString mainLogLine(const QString &node, int source, const QString &text)
{
    const QString id = source >= 0 && source < kLogSourceCount
                           ? QString::fromLatin1(kLogSources[source].id)
                           : QString::fromLatin1(kLogSources[kFallbackLogSource].id);
    return QStringLiteral("Mirror %1 %2: %3").arg(node, id, text);
}

// The bytes a log viewer signs to open a host's GET /v1/node/logs stream over
// QUIC; tools/mirror_gateway.py node_logs_message builds the same string.
inline QString nodeLogsMessage(const QString &node, const QString &viewerKey,
                               const QString &requestId, qint64 issuedAtMs)
{
    return QStringLiteral("forkmesh-node-logs-v1\n%1\n%2\n%3\n%4")
        .arg(node, viewerKey, requestId, QString::number(issuedAtMs));
}

// The feed replays the tail of every log each time it connects, and the live
// view reconnects whenever it comes back on screen, so the main log remembers
// the lines it already took from each host and skips the replay.
class ForwardedHostLines
{
public:
    explicit ForwardedHostLines(int perHost = 4096) : m_perHost(perHost) {}

    // True the first time a host sends this line.
    bool firstSighting(const QString &node, const QString &line)
    {
        Seen &seen = m_hosts[node];
        if (seen.lines.contains(line))
            return false;
        seen.lines.insert(line);
        seen.order.enqueue(line);
        while (seen.order.size() > m_perHost)
            seen.lines.remove(seen.order.dequeue());
        return true;
    }

private:
    struct Seen {
        QSet<QString> lines;
        QQueue<QString> order;
    };
    QHash<QString, Seen> m_hosts;
    int m_perHost;
};

// Maps a journal entry's syslog identifier onto one of the ids above. Matching
// on a prefix keeps the variants together: sshd, sshd-session and sshd-auth are
// one source, and so are forkmesh-mirror, forkmesh-mirror-node and
// forkmesh-hosted-repository-sync.
inline QString logSourceForIdentifier(const QString &identifier)
{
    const QString ident = identifier.toLower();
    if (ident.startsWith(QLatin1String("forkmesh")))
        return QStringLiteral("mirror");
    static const char *const kWebIdentifiers[] = {
        "caddy", "nginx", "cloudflared", "apache", "httpd", "haproxy", "traefik",
    };
    for (const char *name : kWebIdentifiers) {
        if (ident.startsWith(QLatin1String(name)))
            return QStringLiteral("web");
    }
    if (ident.startsWith(QLatin1String("ssh")))
        return QStringLiteral("ssh");
    return QStringLiteral("system");
}

// Classifies one `journalctl -o short-iso` line and rewrites it for the pane.
//
// That format is "<iso-stamp> <hostname> <ident>[<pid>]: <text>"; journalctl
// can be asked to drop the hostname column, but not every host ships a version
// that knows the option, so both shapes are accepted. Anything that does not
// parse — journalctl's own "-- …" notices, a wrapped continuation line — is
// left as it arrived and counted as System rather than dropped.
inline QString journalLogSource(const QString &raw, QString *display)
{
    QString line = raw;
    while (line.endsWith(QLatin1Char('\r')) || line.endsWith(QLatin1Char(' ')))
        line.chop(1);
    if (display)
        *display = line;
    if (line.isEmpty() || line.startsWith(QLatin1String("-- ")))
        return QStringLiteral("system");

    int cursor = 0;
    const auto nextField = [&line, &cursor]() -> QString {
        while (cursor < line.size() && line.at(cursor) == QLatin1Char(' '))
            ++cursor;
        const int start = cursor;
        while (cursor < line.size() && line.at(cursor) != QLatin1Char(' '))
            ++cursor;
        return line.mid(start, cursor - start);
    };

    const QString stamp = nextField();
    if (stamp.isEmpty() || !stamp.at(0).isDigit())
        return QStringLiteral("system");
    // "2026-09-17T15:12:52+0000" — the pane only has room for the clock.
    QString clock;
    const int t = stamp.indexOf(QLatin1Char('T'));
    if (t > 0 && stamp.size() >= t + 9)
        clock = stamp.mid(t + 1, 8);

    QString ident = nextField();
    if (!ident.endsWith(QLatin1Char(':')))
        ident = nextField(); // the first field was the hostname column
    if (!ident.endsWith(QLatin1Char(':')))
        return QStringLiteral("system");
    const QString message = line.mid(qMin(cursor + 1, line.size()));

    ident.chop(1);
    const int bracket = ident.indexOf(QLatin1Char('['));
    if (bracket > 0)
        ident.truncate(bracket);
    if (ident.isEmpty())
        return QStringLiteral("system");

    if (display) {
        *display = clock.isEmpty()
                       ? QStringLiteral("%1: %2").arg(ident, message)
                       : QStringLiteral("%1 %2: %3").arg(clock, ident, message);
    }
    return logSourceForIdentifier(ident);
}

// Reads one line of a feed script's output (kLiveRemoteScript or
// kMainLogFeedScript). A log line — "L <source> …", "M <source> …" or
// "J <journal line>" — yields its source index and display text; the live
// view's sampler rows and the feed's keepalives yield -1.
inline int feedLogLine(const QString &line, QString *text)
{
    if (line.startsWith(QLatin1String("J "))) {
        QString display;
        const int source = logSourceIndex(journalLogSource(line.mid(2), &display));
        if (text)
            *text = display;
        return source;
    }
    if (!line.startsWith(QLatin1String("L ")) && !line.startsWith(QLatin1String("M ")))
        return -1;
    const QString tagged = line.mid(2);
    const int space = tagged.indexOf(QLatin1Char(' '));
    int source = space > 0 ? logSourceIndex(tagged.left(space)) : -1;
    if (text)
        *text = source >= 0 ? tagged.mid(space + 1) : tagged;
    if (source < 0)
        source = kFallbackLogSource;
    return source;
}

// Runs on the host under `sh -lc`, multiplexing every feed onto stdout with a
// tag per line:
//   L <source> <line>  a log line already attributed to one of kLogSources
//   J <line>           a journal line, classified here by journalLogSource
//   M <source> <note>  a note from the stream itself
//   N <uptime> <rx> <tx>   host clock plus total bytes over every non-loopback
//                          interface (using the host's clock keeps the rates
//                          honest when SSH delivers samples in bursts)
//   S / C … / E        one snapshot of established TCP sockets
//
// Most hosts run ForkMesh as a systemd service and have no node.log at all,
// which is why the journal is followed alongside the file rather than only as
// a fallback: a mirror's real output is in journald. The feeds are backgrounded
// and the sampler loop stays in the foreground, so every feed ends when the SSH
// channel closes — tail follows the sampling shell's pid, and journalctl is
// watched by a sibling that kills it once that shell is gone.
inline constexpr char kLiveRemoteScript[] = R"SH(
log="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
if [ ! -f "$log" ] && [ -f "$HOME/.forkmesh/node.log" ]; then log="$HOME/.forkmesh/node.log"; fi
me=$$
if [ -f "$log" ]; then
  { tail --pid="$me" -n 80 -F "$log" 2>/dev/null || tail -n 80 -F "$log"; } 2>&1 |
    while IFS= read -r l; do printf 'L node %s\n' "$l"; done &
else
  printf 'M host No ForkMesh node log file at %s; following the systemd journal instead\n' "$log"
fi
if command -v journalctl >/dev/null 2>&1; then
  if journalctl --no-pager -q -n 1 -o cat >/dev/null 2>&1; then
    { journalctl --no-pager -q -n 60 -f -o short-iso 2>/dev/null &
      jp=$!
      while kill -0 "$me" 2>/dev/null && kill -0 "$jp" 2>/dev/null; do sleep 5; done
      kill "$jp" 2>/dev/null; } |
      while IFS= read -r l; do printf 'J %s\n' "$l"; done &
  else
    printf 'M host journalctl is installed but %s cannot read the journal here\n' "$(id -un 2>/dev/null)"
  fi
else
  printf 'M host journalctl is not installed; only the node log file is followed\n'
fi
if ! command -v ss >/dev/null 2>&1; then
  printf 'M host ss (iproute2) is not installed; connection activity is unavailable\n'
fi
while :; do
  awk 'NR > 2 { sub(/:/, " "); if ($1 != "lo") { rx += $2; tx += $10 } } END { getline up < "/proc/uptime"; split(up, u, " "); printf "N %s %.0f %.0f\n", u[1], rx, tx }' /proc/net/dev 2>/dev/null
  echo S
  { ss -Htnp state established 2>/dev/null || ss -tn state established 2>/dev/null; } |
    while IFS= read -r c; do printf 'C %s\n' "$c"; done
  echo E
  sleep 2
done
)SH";

// The background feed behind the main log: the same two log taps as the live
// view, without its samplers, kept open for every saved host whether or not
// the Hosts page is on screen. The foreground loop writes a keepalive so a
// dropped SSH channel ends it (and with it tail and journalctl) on its next
// write rather than leaving them running on the host.
inline constexpr char kMainLogFeedScript[] = R"SH(
log="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
if [ ! -f "$log" ] && [ -f "$HOME/.forkmesh/node.log" ]; then log="$HOME/.forkmesh/node.log"; fi
me=$$
if [ -f "$log" ]; then
  { tail --pid="$me" -n 80 -F "$log" 2>/dev/null || tail -n 80 -F "$log"; } 2>&1 |
    while IFS= read -r l; do printf 'L node %s\n' "$l"; done &
fi
if command -v journalctl >/dev/null 2>&1; then
  if journalctl --no-pager -q -n 1 -o cat >/dev/null 2>&1; then
    { journalctl --no-pager -q -n 60 -f -o short-iso 2>/dev/null &
      jp=$!
      while kill -0 "$me" 2>/dev/null && kill -0 "$jp" 2>/dev/null; do sleep 5; done
      kill "$jp" 2>/dev/null; } |
      while IFS= read -r l; do printf 'J %s\n' "$l"; done &
  else
    printf 'M host journalctl is installed but %s cannot read the journal here\n' "$(id -un 2>/dev/null)"
  fi
elif [ ! -f "$log" ]; then
  printf 'M host Neither a ForkMesh node log (%s) nor journalctl is here to follow\n' "$log"
fi
while :; do
  echo K
  sleep 30
done
)SH";

// The single-host "Logs" dialog follows the same two taps, but writes plain
// lines: that view has no chips to feed, so each line carries its own prefix.
inline constexpr char kSingleHostLogScript[] = R"SH(
log="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
if [ ! -f "$log" ] && [ -f "$HOME/.forkmesh/node.log" ]; then log="$HOME/.forkmesh/node.log"; fi
me=$$
if [ -f "$log" ]; then
  echo "[node] following $log"
  { tail --pid="$me" -n 200 -F "$log" 2>/dev/null || tail -n 200 -F "$log"; } 2>&1 |
    while IFS= read -r l; do printf '[node] %s\n' "$l"; done &
else
  echo "[host] No ForkMesh node log file at $log"
fi
if command -v journalctl >/dev/null 2>&1; then
  echo "[host] following the systemd journal"
  exec journalctl --no-pager -q -n 200 -f -o short-iso
fi
echo "[host] journalctl is not installed on this host"
wait
)SH";

} // namespace hosts
} // namespace forkmesh

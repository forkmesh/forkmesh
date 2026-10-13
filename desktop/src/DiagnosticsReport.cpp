#include "DiagnosticsReport.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace forkmesh::diagreport {

namespace {

QString bytesText(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("unknown");
    const double mib = double(bytes) / (1024.0 * 1024.0);
    if (mib >= 1024.0)
        return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(mib, 0, 'f', 0);
}

QString durationText(qint64 ms)
{
    if (ms < 0)
        return QStringLiteral("unknown");
    const qint64 minutes = ms / 60000;
    if (minutes < 1)
        return QStringLiteral("%1 s").arg(ms / 1000);
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    if (minutes < 48 * 60)
        return QStringLiteral("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
    return QStringLiteral("%1 days").arg(minutes / (24 * 60));
}

QString percentText(qint64 part, qint64 whole)
{
    if (part < 0 || whole <= 0)
        return QStringLiteral("unknown");
    return QStringLiteral("%1%").arg(100.0 * double(part) / double(whole), 0, 'f', 0);
}

QString clip(const QString &text, int maxChars)
{
    if (text.size() <= maxChars)
        return text;
    return text.left(qMax(0, maxChars - 3)) + QStringLiteral("...");
}

QString change(qint64 before, qint64 after, const QString &unit)
{
    if (before < 0 || after < 0)
        return after < 0 ? QStringLiteral("unknown") : QString::number(after) + unit;
    const qint64 delta = after - before;
    const QString sign = delta > 0 ? QStringLiteral("+") : QString();
    return QStringLiteral("%1%4 -> %2%4 (%3%5%4)")
        .arg(before)
        .arg(after)
        .arg(sign)
        .arg(unit)
        .arg(delta);
}

double usedPercent(qint64 total, qint64 free)
{
    if (total <= 0 || free < 0 || free > total)
        return -1.0;
    return 100.0 * double(total - free) / double(total);
}

struct ProblemGroup {
    QString signature;
    QString badge;
    QString example;
    int count = 0;
    int newestIndex = -1;
};

QList<ProblemGroup> problemGroups(const QList<LogEntry> &log)
{
    QHash<QString, int> index;
    QList<ProblemGroup> groups;
    for (int i = 0; i < log.size(); ++i) {
        const LogEntry &entry = log.at(i);
        if (!isProblemBadge(entry.badge))
            continue;
        const QString signature = problemSignature(entry.message);
        if (signature.isEmpty())
            continue;
        int at = index.value(signature, -1);
        if (at < 0) {
            at = groups.size();
            index.insert(signature, at);
            groups.append(ProblemGroup{signature, entry.badge, QString(), 0, -1});
        }
        ProblemGroup &group = groups[at];
        ++group.count;
        group.newestIndex = i;
        group.example = entry.line.isEmpty() ? entry.message : entry.line;
    }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const ProblemGroup &a, const ProblemGroup &b) {
                         if (a.count != b.count)
                             return a.count > b.count;
                         return a.newestIndex > b.newestIndex;
                     });
    return groups;
}

} // namespace

bool isProblemBadge(const QString &badge)
{
    return badge == QLatin1String("ERROR") || badge == QLatin1String("STALL") ||
           badge == QLatin1String("BGBLOCK");
}

QString problemSignature(const QString &message)
{
    static const QRegularExpression url(QStringLiteral("\\b[a-z][a-z0-9+.-]*://\\S+"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression quoted(
        QStringLiteral("\"[^\"]*\"|'[^']*'|`[^`]*`"));
    static const QRegularExpression path(QStringLiteral("(?:~|\\.{0,2})?/[^\\s:,;)\\]]+"));
    static const QRegularExpression uuid(QStringLiteral(
        "\\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hex(QStringLiteral("\\b(?:0x)?[0-9a-f]*[0-9][0-9a-f]*\\b"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression space(QStringLiteral("\\s+"));
    QString text = message;
    text.replace(url, QStringLiteral("<url>"));
    text.replace(quoted, QStringLiteral("<str>"));
    text.replace(uuid, QStringLiteral("<id>"));
    text.replace(path, QStringLiteral("<path>"));
    text.replace(hex, QStringLiteral("N"));
    text.replace(space, QStringLiteral(" "));
    return text.trimmed().left(160);
}

QHash<QString, int> problemCounts(const QList<LogEntry> &log)
{
    QHash<QString, int> counts;
    for (const ProblemGroup &group : problemGroups(log))
        counts.insert(group.signature, group.count);
    return counts;
}

Baseline baselineFrom(const Snapshot &snapshot, int sequence)
{
    Baseline baseline;
    baseline.sequence = sequence;
    baseline.takenAtMs = snapshot.takenAtMs;
    baseline.stallCount = snapshot.stallCount;
    baseline.crashes = snapshot.crashes.size();
    baseline.residentBytes = snapshot.residentBytes;
    baseline.loopLagP95Ms = snapshot.loopLagP95Ms;
    const QList<ProblemGroup> groups = problemGroups(snapshot.log);
    for (int i = 0; i < groups.size() && i < 200; ++i)
        baseline.problems.insert(groups.at(i).signature, groups.at(i).count);
    return baseline;
}

QJsonObject toJson(const Baseline &baseline)
{
    QJsonObject problems;
    for (auto it = baseline.problems.cbegin(); it != baseline.problems.cend(); ++it)
        problems.insert(it.key(), it.value());
    return QJsonObject{
        {QStringLiteral("sequence"), baseline.sequence},
        {QStringLiteral("takenAtMs"), baseline.takenAtMs},
        {QStringLiteral("stallCount"), baseline.stallCount},
        {QStringLiteral("crashes"), baseline.crashes},
        {QStringLiteral("residentBytes"), baseline.residentBytes},
        {QStringLiteral("loopLagP95Ms"), baseline.loopLagP95Ms},
        {QStringLiteral("problems"), problems},
    };
}

Baseline baselineFromJson(const QJsonObject &object)
{
    Baseline baseline;
    baseline.sequence = object.value(QStringLiteral("sequence")).toInt();
    baseline.takenAtMs =
        qint64(object.value(QStringLiteral("takenAtMs")).toDouble());
    baseline.stallCount = object.value(QStringLiteral("stallCount")).toInt();
    baseline.crashes = object.value(QStringLiteral("crashes")).toInt();
    baseline.residentBytes =
        qint64(object.value(QStringLiteral("residentBytes")).toDouble(-1));
    baseline.loopLagP95Ms =
        qint64(object.value(QStringLiteral("loopLagP95Ms")).toDouble(-1));
    const QJsonObject problems = object.value(QStringLiteral("problems")).toObject();
    for (auto it = problems.begin(); it != problems.end(); ++it)
        baseline.problems.insert(it.key(), it.value().toInt());
    return baseline;
}

QStringList findings(const Snapshot &s)
{
    QStringList out;
    auto add = [&out](const char *level, const QString &text) {
        out << QStringLiteral("%1: %2").arg(QLatin1String(level), text);
    };
    if (!s.crashes.isEmpty())
        add("CRITICAL",
            QStringLiteral("%1 crash record(s) in the log. Fix the faulting frame "
                           "first; a crash loses the user's work.")
                .arg(s.crashes.size()));
    if (s.stallCount > 0)
        add("WARNING",
            QStringLiteral("%1 UI stall(s) this session: the GUI thread was "
                           "blocked. Move the blocking call in each backtrace off "
                           "the main thread.")
                .arg(s.stallCount));
    if (s.loopLagP95Ms >= 100)
        add("WARNING",
            QStringLiteral("Event-loop lag p95 is %1 ms (aim for under 50 ms). "
                           "Find the slot or timer doing heavy work per tick.")
                .arg(s.loopLagP95Ms));
    else if (s.loopLagPeakMs >= 1000)
        add("INFO", QStringLiteral("Event-loop lag peaked at %1 ms this session.")
                        .arg(s.loopLagPeakMs));
    const qint64 gib = 1024LL * 1024 * 1024;
    if (s.residentBytes >= 2 * gib)
        add("WARNING",
            QStringLiteral("The app holds %1 resident. Look for caches, log "
                           "buffers or pixmaps that grow without a cap.")
                .arg(bytesText(s.residentBytes)));
    else if (s.residentBytes >= gib)
        add("INFO", QStringLiteral("The app holds %1 resident.")
                        .arg(bytesText(s.residentBytes)));
    if (s.cpuPercent >= 50.0)
        add("WARNING",
            QStringLiteral("The app used %1% CPU in the last second. Check timers "
                           "and repaints that run while nothing changes.")
                .arg(s.cpuPercent, 0, 'f', 0));
    const double hostMemory = usedPercent(s.hostMemoryTotal, s.hostMemoryAvailable);
    if (hostMemory >= 85.0)
        add("WARNING", QStringLiteral("Host memory is %1% used.")
                           .arg(hostMemory, 0, 'f', 0));
    const double swap = usedPercent(s.swapTotal, s.swapFree);
    if (swap >= 50.0)
        add("INFO", QStringLiteral("Swap is %1% used.").arg(swap, 0, 'f', 0));
    const double disk = usedPercent(s.diskTotal, s.diskFree);
    if (disk >= 90.0)
        add(disk >= 97.0 ? "CRITICAL" : "WARNING",
            QStringLiteral("The home drive is %1% full (%2 free). Builds and "
                           "logs fail when it fills.")
                .arg(disk, 0, 'f', 0)
                .arg(bytesText(s.diskFree)));
    if (s.openFiles > 0 && s.openFileLimit > 0 &&
        s.openFiles * 100 >= s.openFileLimit * 70)
        add("WARNING",
            QStringLiteral("%1 of %2 file descriptors are open. Look for a "
                           "leak (processes, sockets or files never closed).")
                .arg(s.openFiles)
                .arg(s.openFileLimit));
    if (s.timers >= 1500)
        add("INFO", QStringLiteral("%1 QTimers exist. Check for timers created per "
                                   "row or per refresh and never deleted.")
                        .arg(s.timers));
    if (s.widgets >= 20000)
        add("INFO", QStringLiteral("%1 widgets exist. Check for widgets rebuilt on "
                                   "refresh without deleting the old ones.")
                        .arg(s.widgets));
    const double minutes = s.uptimeMs > 0 ? double(s.uptimeMs) / 60000.0 : 0.0;
    if (minutes >= 5.0) {
        const double requestsPerMinute = double(s.networkRequests) / minutes;
        if (requestsPerMinute >= 120.0)
            add("WARNING",
                QStringLiteral("%1 network requests per minute on average. "
                               "Look for polling that could back off or be "
                               "replaced by a push.")
                    .arg(requestsPerMinute, 0, 'f', 0));
        const double linesPerMinute = double(s.logLinesThisSession) / minutes;
        if (linesPerMinute >= 300.0)
            add("INFO",
                QStringLiteral("%1 log lines per minute. Quiet the noisiest "
                               "category below if it carries nothing useful.")
                    .arg(linesPerMinute, 0, 'f', 0));
    }
    const int groups = problemGroups(s.log).size();
    if (groups > 0)
        add(groups >= 10 ? "WARNING" : "INFO",
            QStringLiteral("%1 distinct problem line(s) in the log buffer.")
                .arg(groups));
    return out;
}

QString build(const Snapshot &s, const Baseline &previous)
{
    const int sequence = previous.sequence + 1;
    const bool havePrevious = previous.sequence > 0 && previous.takenAtMs > 0;
    const QList<ProblemGroup> groups = problemGroups(s.log);
    QSet<QString> current;
    for (const ProblemGroup &group : groups)
        current.insert(group.signature);
    QStringList gone;
    if (havePrevious)
        for (auto it = previous.problems.cbegin(); it != previous.problems.cend(); ++it)
            if (!current.contains(it.key()))
                gone << QStringLiteral("- was x%1: %2").arg(it.value()).arg(it.key());
    gone.sort();

    QString out;
    out += QStringLiteral("# ForkMesh desktop diagnostic report #%1\n\n").arg(sequence);
    out += QStringLiteral("Copied %1")
               .arg(QDateTime::fromMSecsSinceEpoch(s.takenAtMs)
                        .toString(Qt::ISODate));
    if (havePrevious)
        out += QStringLiteral("; report #%1 was copied %2 earlier.\n\n")
                   .arg(previous.sequence)
                   .arg(durationText(s.takenAtMs - previous.takenAtMs));
    else
        out += QStringLiteral("; this is the first report from this machine.\n\n");

    out += QStringLiteral("## What to do with this report\n\n");
    out += QStringLiteral(
               "You are a coding agent in the ForkMesh repository (this build's "
               "source: %1). The user copied this report from the running "
               "desktop app so you can fix its errors and make it faster. Work "
               "through it in this order:\n\n")
               .arg(s.sourceDir.isEmpty() ? QStringLiteral("unknown") : s.sourceDir);
    out += QStringLiteral(
        "1. Crashes, then problems marked NEW, then the recurring problems with "
        "the highest counts. A line ending in [file:line] names the code that "
        "logged it. Fix the cause, not the log line, and add or extend a test "
        "that would have caught it.\n"
        "2. UI stalls: each one has a backtrace of the blocked GUI thread. Move "
        "the slow work off the main thread, or skip it when nothing changed.\n"
        "3. Findings: each crossed threshold points at something to optimize. "
        "Measure before and after.\n"
        "4. Problems under \"Not seen since the last report\" were probably fixed "
        "already: check git log before touching them again.\n"
        "5. Follow AGENTS.md: no comments in source, the reason for each change "
        "goes in the matching docs/*.md page, and deletes go through the "
        "guarded helpers.\n"
        "6. Build, run the tests you touched, and commit. End with a short list "
        "of what you fixed, what you could not reproduce, and what needs the "
        "user. The user will restart the app and copy a new report, which "
        "compares itself with this one.\n\n");

    out += QStringLiteral("## Since the last report\n\n");
    if (!havePrevious) {
        out += QStringLiteral("Nothing to compare yet. The next copy will show what "
                              "changed after these fixes.\n\n");
    } else {
        int fresh = 0;
        for (const ProblemGroup &group : groups)
            if (!previous.problems.contains(group.signature))
                ++fresh;
        out += QStringLiteral("- Problem groups: %1 -> %2 (%3 new, %4 not seen)\n")
                   .arg(previous.problems.size())
                   .arg(groups.size())
                   .arg(fresh)
                   .arg(gone.size());
        out += QStringLiteral("- UI stalls this session: %1\n")
                   .arg(change(previous.stallCount, s.stallCount, QString()));
        out += QStringLiteral("- Crash records in the log: %1\n")
                   .arg(change(previous.crashes, s.crashes.size(), QString()));
        out += QStringLiteral("- Resident memory: %1 -> %2\n")
                   .arg(bytesText(previous.residentBytes), bytesText(s.residentBytes));
        out += QStringLiteral("- Event-loop lag p95: %1\n\n")
                   .arg(change(previous.loopLagP95Ms, s.loopLagP95Ms,
                               QStringLiteral(" ms")));
    }

    out += QStringLiteral("## Findings\n\n");
    const QStringList found = findings(s);
    if (found.isEmpty())
        out += QStringLiteral("No threshold crossed.\n\n");
    else
        out += QStringLiteral("- ") + found.join(QStringLiteral("\n- ")) +
               QStringLiteral("\n\n");

    out += QStringLiteral("## Problems in the log\n\n");
    if (groups.isEmpty()) {
        out += QStringLiteral("No ERROR, STALL or BGBLOCK lines in the log buffer.\n\n");
    } else {
        out += QStringLiteral("Grouped by message with numbers, paths, URLs and "
                              "quoted text folded; most frequent first.\n\n");
        for (int i = 0; i < groups.size() && i < kMaxErrorGroups; ++i) {
            const ProblemGroup &group = groups.at(i);
            const bool isNew =
                havePrevious && !previous.problems.contains(group.signature);
            out += QStringLiteral("- %1%2 x%3: %4\n")
                       .arg(isNew ? QStringLiteral("NEW ") : QString(), group.badge)
                       .arg(group.count)
                       .arg(clip(group.example, kMaxLineChars));
        }
        if (groups.size() > kMaxErrorGroups)
            out += QStringLiteral("- ... %1 more group(s); see the app log.\n")
                       .arg(groups.size() - kMaxErrorGroups);
        out += QLatin1Char('\n');
    }

    if (!gone.isEmpty()) {
        out += QStringLiteral("## Not seen since the last report\n\n");
        out += gone.mid(0, kMaxErrorGroups).join(QLatin1Char('\n'));
        out += QStringLiteral("\n\n");
    }

    if (!s.crashes.isEmpty()) {
        out += QStringLiteral("## Crashes\n\n");
        for (const QString &crash : s.crashes)
            out += clip(crash, kMaxStallChars) + QStringLiteral("\n\n");
    }

    out += QStringLiteral("## UI stalls\n\n");
    if (s.stalls.isEmpty()) {
        out += QStringLiteral("%1 stall(s) recorded this session.\n\n").arg(s.stallCount);
    } else {
        out += QStringLiteral("%1 stall(s) recorded this session; newest first.\n\n")
                   .arg(s.stallCount);
        int shown = 0;
        for (int i = s.stalls.size() - 1; i >= 0 && shown < kMaxStalls; --i, ++shown)
            out += QStringLiteral("```\n%1\n```\n\n")
                       .arg(clip(s.stalls.at(i).trimmed(), kMaxStallChars));
    }

    out += QStringLiteral("## App\n\n");
    out += QStringLiteral("- Version: %1\n- Source: %2\n- Platform: %3\n- Qt: %4\n"
                          "- Uptime: %5\n- Offline mode: %6\n")
               .arg(s.version, s.sourceDir, s.platform, s.qtVersion,
                    durationText(s.uptimeMs),
                    s.offline ? QStringLiteral("on") : QStringLiteral("off"));
    if (s.runningAgents >= 0)
        out += QStringLiteral("- Agents running: %1\n").arg(s.runningAgents);
    out += QLatin1Char('\n');

    if (!s.logLocations.trimmed().isEmpty())
        out += QStringLiteral("## Where to look\n\n%1\n\n").arg(s.logLocations.trimmed());

    out += QStringLiteral("## Resources\n\n");
    out += QStringLiteral("- App CPU: %1\n")
               .arg(s.cpuPercent < 0
                        ? QStringLiteral("unknown")
                        : QStringLiteral("%1%").arg(s.cpuPercent, 0, 'f', 0));
    out += QStringLiteral("- App resident memory: %1\n").arg(bytesText(s.residentBytes));
    if (s.threads > 0)
        out += QStringLiteral("- App threads: %1\n").arg(s.threads);
    if (s.openFiles > 0)
        out += QStringLiteral("- Open files: %1 of %2\n")
                   .arg(s.openFiles)
                   .arg(s.openFileLimit);
    if (s.hostMemoryTotal > 0)
        out += QStringLiteral("- Host memory: %1 used of %2\n")
                   .arg(percentText(s.hostMemoryTotal - s.hostMemoryAvailable,
                                    s.hostMemoryTotal),
                        bytesText(s.hostMemoryTotal));
    if (s.swapTotal > 0)
        out += QStringLiteral("- Swap: %1 used of %2\n")
                   .arg(percentText(s.swapTotal - s.swapFree, s.swapTotal),
                        bytesText(s.swapTotal));
    if (s.diskTotal > 0)
        out += QStringLiteral("- Home drive: %1 used, %2 free\n")
                   .arg(percentText(s.diskTotal - s.diskFree, s.diskTotal),
                        bytesText(s.diskFree));
    out += QStringLiteral("- Network requests this session: %1\n")
               .arg(s.networkRequests);
    out += QStringLiteral("- Log lines this session: %1\n\n").arg(s.logLinesThisSession);

    if (!s.healthFindings.trimmed().isEmpty())
        out += QStringLiteral("## Self-diagnostics\n\n%1\n\n")
                   .arg(s.healthFindings.trimmed());

    if (!s.categoryCounts.isEmpty()) {
        QList<QPair<QString, int>> counts;
        for (auto it = s.categoryCounts.cbegin(); it != s.categoryCounts.cend(); ++it)
            counts.append({it.key(), it.value()});
        std::sort(counts.begin(), counts.end(), [](const auto &a, const auto &b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        QStringList parts;
        for (const auto &count : counts)
            parts << QStringLiteral("%1 %2").arg(count.first).arg(count.second);
        out += QStringLiteral("## Log lines by category\n\n%1\n\n")
                   .arg(parts.join(QStringLiteral(", ")));
    }

    if (!s.log.isEmpty()) {
        out += QStringLiteral("## Recent log\n\n```\n");
        for (int i = qMax(0, int(s.log.size()) - kMaxRecentLines); i < s.log.size(); ++i) {
            const LogEntry &entry = s.log.at(i);
            out += clip(entry.line.isEmpty() ? entry.message : entry.line,
                        kMaxLineChars) +
                   QLatin1Char('\n');
        }
        out += QStringLiteral("```\n\n");
    }

    if (!s.qtReport.trimmed().isEmpty())
        out += QStringLiteral("## Qt runtime\n\n```\n%1\n```\n\n")
                   .arg(clip(s.qtReport.trimmed(), kMaxQtReportChars));

    if (out.size() > kMaxReportChars)
        out = out.left(kMaxReportChars) +
              QStringLiteral("\n\n(report truncated; the full logs are at the paths "
                             "above)\n");
    return out;
}

} // namespace forkmesh::diagreport

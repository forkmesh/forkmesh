#include "LocalControlDiagnostics.h"

#include "LocalControlFleet.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace forkmesh::localcontrol::diag {

QString familyPrefix()
{
    return QStringLiteral("diag.");
}

QString severityOf(const QString &line)
{
    const QString lower = line.toLower();
    static const QRegularExpression error(QStringLiteral(
        "\\b(error|errors|failed|failure|fatal|crash(ed)?|refused|exception|panic)\\b"));
    static const QRegularExpression warning(QStringLiteral(
        "\\b(warn(ing)?|retry(ing)?|timed out|timeout|slow|stall(ed)?|froze|degraded)\\b"));
    if (error.match(lower).hasMatch())
        return QStringLiteral("error");
    if (warning.match(lower).hasMatch())
        return QStringLiteral("warning");
    return QStringLiteral("info");
}

qint64 timestampOf(const QString &line)
{
    if (line.size() < 19)
        return 0;
    const QDateTime when =
        QDateTime::fromString(line.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    return when.isValid() ? when.toMSecsSinceEpoch() : 0;
}

LogPage logPage(const QString &path, qint64 before, int maxLines,
                const QString &query, const QString &severity)
{
    LogPage page;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return page;
    const qint64 size = file.size();
    const qint64 end = before < 0 ? size : qBound<qint64>(0, before, size);
    const qint64 start = qMax<qint64>(0, end - kMaxLogScanBytes);
    const int wanted = qBound(1, maxLines, kMaxLogLines);
    if (!file.seek(start))
        return page;
    QByteArray bytes = file.read(end - start);
    qint64 firstLineOffset = start;
    if (start > 0) {
        const qsizetype newline = bytes.indexOf('\n');
        if (newline < 0) {
            page.start = end;
            page.end = end;
            page.more = start > 0;
            return page;
        }
        bytes.remove(0, newline + 1);
        firstLineOffset = start + newline + 1;
    }
    const QString needle = query.trimmed().left(200);
    struct Line {
        qint64 offset;
        QString text;
    };
    QList<Line> kept;
    qint64 offset = firstLineOffset;
    for (const QByteArray &raw : bytes.split('\n')) {
        const qint64 lineOffset = offset;
        offset += raw.size() + 1;
        QString text = QString::fromUtf8(raw);
        if (text.endsWith(QLatin1Char('\r')))
            text.chop(1);
        if (text.trimmed().isEmpty())
            continue;
        ++page.scanned;
        if (!needle.isEmpty() && !text.contains(needle, Qt::CaseInsensitive))
            continue;
        if (!severity.isEmpty() && severity != QLatin1String("all")) {
            const QString level = severityOf(text);
            if (severity == QLatin1String("warning")
                    ? level == QLatin1String("info")
                    : level != severity)
                continue;
        }
        kept.append({lineOffset, text});
    }
    const qsizetype from = qMax<qsizetype>(0, kept.size() - wanted);
    for (qsizetype i = from; i < kept.size(); ++i) {
        const QString text = fleet::redactSecrets(kept.at(i).text.left(2000));
        page.lines.append(QJsonObject{{QStringLiteral("offset"), double(kept.at(i).offset)},
                                      {QStringLiteral("t"), double(timestampOf(text))},
                                      {QStringLiteral("severity"), severityOf(text)},
                                      {QStringLiteral("text"), text}});
    }
    page.start = from > 0 ? kept.at(from).offset : firstLineOffset;
    page.end = end;
    page.more = page.start > 0;
    return page;
}

QJsonObject stallRow(int index, const QString &entry)
{
    const QString head = entry.section(QLatin1Char('\n'), 0, 0);
    QString when;
    QString summary = head;
    static const QRegularExpression stamp(QStringLiteral("^\\[([^\\]]+)\\]\\s*(.*)$"));
    const auto match = stamp.match(head);
    if (match.hasMatch()) {
        when = match.captured(1);
        summary = match.captured(2);
    }
    return {{QStringLiteral("index"), index},
            {QStringLiteral("when"), when.left(64)},
            {QStringLiteral("summary"), fleet::redactSecrets(summary.left(400))},
            {QStringLiteral("lines"), int(entry.count(QLatin1Char('\n')) + 1)},
            {QStringLiteral("bytes"), int(entry.toUtf8().size())}};
}

QJsonObject stallDetail(int index, const QString &entry, int maxChars)
{
    QJsonObject row = stallRow(index, entry);
    const QStringList lines = entry.split(QLatin1Char('\n'));
    QStringList redacted;
    int used = 0;
    bool truncated = false;
    for (const QString &line : lines) {
        const QString clean = fleet::redactSecrets(line.left(2000));
        if (used + clean.size() + 1 > maxChars) {
            truncated = true;
            break;
        }
        used += clean.size() + 1;
        redacted.append(clean);
    }
    row.insert(QStringLiteral("text"), redacted.join(QLatin1Char('\n')));
    row.insert(QStringLiteral("truncated"), truncated);
    return row;
}

QString healthLevel(const HealthInput &input)
{
    const bool memoryStarved = input.pressureValid && input.memoryFullAvg10 >= 10.0;
    const bool ioStarved = input.pressureValid && input.ioFullAvg10 >= 30.0;
    const double availableShare =
        input.memoryTotal > 0 && input.memoryAvailable >= 0
            ? double(input.memoryAvailable) / double(input.memoryTotal)
            : 1.0;
    const double diskShare = input.diskTotal > 0 && input.diskFree >= 0
                                 ? double(input.diskFree) / double(input.diskTotal)
                                 : 1.0;
    if (memoryStarved || ioStarved || availableShare < 0.05 || diskShare < 0.03)
        return QStringLiteral("strained");
    const bool loaded = input.cpuCount > 0 && input.loadAverage1 > input.cpuCount * 1.5;
    if (loaded || availableShare < 0.15 || diskShare < 0.10 ||
        (input.pressureValid && input.memorySomeAvg10 >= 10.0) ||
        (input.openFileLimit > 0 && input.openFiles > input.openFileLimit * 0.8))
        return QStringLiteral("busy");
    return QStringLiteral("ok");
}

QJsonObject healthSummary(const HealthInput &input)
{
    QJsonObject memory{{QStringLiteral("total"), double(input.memoryTotal)},
                       {QStringLiteral("available"), double(input.memoryAvailable)},
                       {QStringLiteral("swapTotal"), double(input.swapTotal)},
                       {QStringLiteral("swapFree"), double(input.swapFree)},
                       {QStringLiteral("appResident"), double(input.selfResident)}};
    QJsonObject pressure{{QStringLiteral("valid"), input.pressureValid},
                         {QStringLiteral("memorySome"), input.memorySomeAvg10},
                         {QStringLiteral("memoryFull"), input.memoryFullAvg10},
                         {QStringLiteral("ioFull"), input.ioFullAvg10},
                         {QStringLiteral("cpuSome"), input.cpuSomeAvg10}};
    QJsonObject summary{{QStringLiteral("level"), healthLevel(input)},
            {QStringLiteral("cpuPercent"), input.cpuPercent},
            {QStringLiteral("cpuCount"), input.cpuCount},
            {QStringLiteral("load1"), input.loadAverage1},
            {QStringLiteral("memory"), memory},
            {QStringLiteral("pressure"), pressure},
            {QStringLiteral("disk"),
             QJsonObject{{QStringLiteral("total"), double(input.diskTotal)},
                         {QStringLiteral("free"), double(input.diskFree)}}},
            {QStringLiteral("agents"),
             QJsonObject{{QStringLiteral("running"), input.agentsRunning},
                         {QStringLiteral("queued"), input.agentsQueued},
                         {QStringLiteral("total"), input.agentsTotal}}},
            {QStringLiteral("stalls"), input.stalls},
            {QStringLiteral("process"),
             QJsonObject{{QStringLiteral("openFiles"), input.openFiles},
                         {QStringLiteral("openFileLimit"), input.openFileLimit},
                         {QStringLiteral("threads"), input.threads}}}};
    if (input.powerKnown)
        summary.insert(QStringLiteral("power"),
                       QJsonObject{{QStringLiteral("onBattery"), input.onBattery},
                                   {QStringLiteral("percent"), input.batteryPercent}});
    return summary;
}

double usedPercent(qint64 total, qint64 free)
{
    if (total <= 0 || free < 0 || free > total)
        return -1;
    return 100.0 * double(total - free) / double(total);
}

QJsonObject vitalsSampleJson(const VitalsSample &sample)
{
    return {{QStringLiteral("memory"), sample.memory},
            {QStringLiteral("cpu"), sample.cpu},
            {QStringLiteral("disk"), sample.disk},
            {QStringLiteral("swap"), sample.swap}};
}

QJsonObject vitalsHistoryJson(const QList<VitalsSample> &samples)
{
    QJsonArray memory, cpu, disk, swap;
    for (const VitalsSample &sample : samples) {
        memory.append(sample.memory);
        cpu.append(sample.cpu);
        disk.append(sample.disk);
        swap.append(sample.swap);
    }
    return {{QStringLiteral("memory"), memory},
            {QStringLiteral("cpu"), cpu},
            {QStringLiteral("disk"), disk},
            {QStringLiteral("swap"), swap}};
}

void appendVitals(QList<VitalsSample> &samples, const VitalsSample &sample)
{
    samples.append(sample);
    while (samples.size() > kVitalsSeconds)
        samples.removeFirst();
}

QList<ProcessRow> parseProcessTable(const QByteArray &psOutput, int maxRows,
                                    QHash<qint64, qint64> *parents)
{
    QList<ProcessRow> rows;
    for (const QByteArray &raw : psOutput.split('\n')) {
        const QList<QByteArray> fields = raw.simplified().split(' ');
        if (fields.size() < 6)
            continue;
        bool pidOk = false;
        bool rssOk = false;
        ProcessRow row;
        row.pid = fields.at(0).toLongLong(&pidOk);
        row.ppid = fields.at(1).toLongLong();
        row.user = QString::fromLocal8Bit(fields.at(2)).left(32);
        row.rssKb = fields.at(3).toLongLong(&rssOk);
        row.percent = QString::fromLocal8Bit(fields.at(4)).toDouble();
        row.command = QString::fromLocal8Bit(QByteArrayList(fields.mid(5)).join(' '));
        if (!pidOk || !rssOk || row.pid <= 0 || row.command.isEmpty())
            continue;
        if (parents && row.ppid > 0)
            parents->insert(row.pid, row.ppid);
        QString name = row.command.section(QLatin1Char(' '), 0, 0);
        const int slash = name.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0)
            name = name.mid(slash + 1);
        row.name = (name.isEmpty() ? row.command : name).left(80);
        if (rows.size() < maxRows)
            rows.append(row);
    }
    return rows;
}

QString commandSummary(const QString &command, int maxChars)
{
    QStringList kept;
    for (const QString &part : command.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const QString lower = part.toLower();
        if (lower.contains(QLatin1String("token")) || lower.contains(QLatin1String("key=")) ||
            lower.contains(QLatin1String("password")) || lower.contains(QLatin1String("secret"))) {
            kept.append(QStringLiteral("[redacted]"));
            continue;
        }
        kept.append(part);
    }
    QString text = fleet::redactSecrets(kept.join(QLatin1Char(' ')));
    if (text.size() > maxChars)
        text = text.left(maxChars - 1) + QChar(0x2026);
    return text;
}

qint64 processPid(const QJsonObject &params)
{
    const double pid = params.value(QStringLiteral("pid")).toDouble(0);
    if (pid < 1 || pid > 2147483647.0 || pid != double(qint64(pid)))
        return 0;
    return qint64(pid);
}

QString processSubject(qint64 pid, const QString &identity)
{
    return QStringLiteral("%1:%2").arg(pid).arg(identity);
}

bool processStoppable(qint64 pid, qint64 selfPid)
{
    return pid > 1 && pid != selfPid;
}

bool sudoPasswordUsable(const QString &password)
{
    return !password.isEmpty() && password.size() <= 1024 &&
           !password.contains(QLatin1Char('\r')) && !password.contains(QLatin1Char('\n')) &&
           !password.contains(QChar(0));
}

QString processDetailText(const QString &text)
{
    QString clean = fleet::redactSecrets(text);
    if (clean.size() > kMaxProcessDetailChars)
        clean = clean.left(kMaxProcessDetailChars) + QStringLiteral("\n[truncated]");
    return clean;
}

QStringList mutatingActions()
{
    return {QStringLiteral("stopAgent"), QStringLiteral("sendStalls"),
            QStringLiteral("sendStall")};
}

QString subjectFor(const QString &action, const QJsonObject &params, QString *error)
{
    const auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return QString();
    };
    if (action == QLatin1String("stopAgent")) {
        const int session = params.value(QStringLiteral("session")).toInt(0);
        if (session <= 0)
            return fail("bad_session");
        return QString::number(session);
    }
    if (action == QLatin1String("sendStall")) {
        const int index = params.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index > 1000)
            return fail("bad_index");
        return QString::number(index);
    }
    if (action == QLatin1String("sendStalls"))
        return QStringLiteral("all");
    return fail("unsupported_action");
}

QString actionSummary(const QString &action, const QString &subject,
                      const QString &label)
{
    if (action == QLatin1String("stopAgent"))
        return QStringLiteral("Stop the agent run \"%1\". Everything it started goes "
                              "down with it; its worktree and branch are kept.")
            .arg(label.isEmpty() ? QStringLiteral("#") + subject : label);
    if (action == QLatin1String("sendStalls"))
        return QStringLiteral("Start an agent on the desktop to fix the recorded UI "
                              "stalls.");
    if (action == QLatin1String("sendStall"))
        return QStringLiteral("Start an agent on the desktop to fix this UI stall%1.")
            .arg(label.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(label));
    return {};
}

QString backupLocationLabel(const QString &root, const QString &dataRoot)
{
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanData = QDir::cleanPath(dataRoot);
    if (!cleanData.isEmpty() && cleanData != QLatin1String(".") &&
        cleanRoot.startsWith(cleanData + QLatin1Char('/'))) {
        const QString relative = cleanRoot.mid(cleanData.size() + 1);
        if (!relative.isEmpty() && !relative.contains(QLatin1Char('/')))
            return QStringLiteral("ForkMesh data › ") + relative;
        return QStringLiteral("ForkMesh data › ") + QFileInfo(cleanRoot).fileName();
    }
    const QString name = QFileInfo(cleanRoot).fileName();
    return name.isEmpty() ? QStringLiteral("Backups") : name;
}

QString backupFailureCode(const QString &failure)
{
    static const QStringList known{QStringLiteral("tar_missing"), QStringLiteral("tar_failed"),
                                   QStringLiteral("nothing_to_back_up"),
                                   QStringLiteral("rename_failed")};
    if (failure.isEmpty())
        return {};
    return known.contains(failure) ? failure : QStringLiteral("failed");
}

QJsonObject backupStatus(const BackupStatusInput &input)
{
    qint64 lastAt = 0;
    qint64 lastBytes = 0;
    qint64 totalBytes = 0;
    int count = 0;
    for (const BackupSnapshotFacts &snapshot : input.snapshots) {
        if (count >= kMaxBackupSnapshots)
            break;
        ++count;
        totalBytes += qMax<qint64>(0, snapshot.bytes);
        if (snapshot.takenAtMs > lastAt) {
            lastAt = snapshot.takenAtMs;
            lastBytes = qMax<qint64>(0, snapshot.bytes);
        }
    }
    const QString failure = backupFailureCode(input.failure);
    const bool failed = !failure.isEmpty() && input.failureAtMs >= lastAt;
    QString result;
    if (input.running)
        result = QStringLiteral("running");
    else if (failed)
        result = QStringLiteral("failed");
    else if (count == 0)
        result = input.enabled ? QStringLiteral("none") : QStringLiteral("off");
    else
        result = QStringLiteral("ok");
    qint64 nextDue = 0;
    if (input.enabled && input.intervalMs > 0)
        nextDue = lastAt > 0 ? qMax(lastAt + input.intervalMs, input.nowMs) : input.nowMs;
    QJsonObject out{
        {QStringLiteral("enabled"), input.enabled},
        {QStringLiteral("keep"), input.keep},
        {QStringLiteral("running"), input.running},
        {QStringLiteral("result"), result},
        {QStringLiteral("location"), input.location.left(120)},
        {QStringLiteral("count"), count},
        {QStringLiteral("totalBytes"), double(totalBytes)},
        {QStringLiteral("lastBackupAtMs"), double(lastAt)},
        {QStringLiteral("lastBackupBytes"), double(lastBytes)},
        {QStringLiteral("nextDueAtMs"), double(nextDue)},
    };
    if (failed) {
        out.insert(QStringLiteral("error"), failure);
        out.insert(QStringLiteral("failedAtMs"), double(input.failureAtMs));
    }
    return out;
}

QString buildMacProbeScript()
{
    return QStringLiteral(
        "if [ -d ci/lock ]; then echo \"$(( $(date +%s) - $(stat -f %m ci/lock) ))\"; "
        "head -c 200 ci/lock/run 2>/dev/null; fi; true");
}

QJsonObject buildMacStatus(bool reachable, const QByteArray &probeOutput, qint64 checkedAtMs)
{
    QJsonObject out{{QStringLiteral("configured"), true},
                    {QStringLiteral("checkedAtMs"), double(checkedAtMs)}};
    if (!reachable) {
        out.insert(QStringLiteral("state"), QStringLiteral("offline"));
        return out;
    }
    const QStringList lines = QString::fromUtf8(probeOutput).split(QLatin1Char('\n'));
    bool ageOk = false;
    const qint64 age = lines.value(0).trimmed().toLongLong(&ageOk);
    QString job = lines.value(1).trimmed();
    job.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9 ._/+-]")));
    if (!ageOk || age < 0 || age >= kBuildMacStaleLockSeconds || job.isEmpty()) {
        out.insert(QStringLiteral("state"), QStringLiteral("idle"));
        return out;
    }
    out.insert(QStringLiteral("state"), QStringLiteral("building"));
    out.insert(QStringLiteral("job"), job.left(80));
    out.insert(QStringLiteral("sinceMs"), double(checkedAtMs - age * 1000));
    return out;
}

} // namespace forkmesh::localcontrol::diag

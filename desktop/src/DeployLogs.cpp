#include "DeployLogs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace forkmesh {
namespace deploylogs {

namespace {

constexpr int kPollMs = 2000;
constexpr int kJournalRetryMs = 30 * 1000;
constexpr qint64 kCursorSaveMs = 5000;
constexpr int kMaximumLineChars = 2000;
const char kCursorKey[] = "user-journal";

QString clipped(const QString &text)
{
    if (text.size() <= kMaximumLineChars)
        return text;
    return text.left(kMaximumLineChars - 3) + QStringLiteral("...");
}

} // namespace

QString autodeployDirectory()
{
    return corelogs::fleetLogsDirectory() + QStringLiteral("/autodeploy");
}

QString opsDirectory()
{
    return corelogs::fleetLogsDirectory() + QStringLiteral("/ops");
}

QString fileKind(const QString &directoryKind, const QString &fileName)
{
    static const QRegularExpression stamped(
        QStringLiteral("^[0-9]{8}-[0-9]{6}-(.+)\\.log$"));
    const QRegularExpressionMatch match = stamped.match(fileName);
    QString rest = match.hasMatch() ? match.captured(1)
                                    : QFileInfo(fileName).completeBaseName();
    if (directoryKind == QLatin1String("ops")) {
        const int dash = rest.indexOf(QLatin1Char('-'));
        if (dash > 0)
            rest[dash] = QLatin1Char(' ');
        return rest;
    }
    return rest.isEmpty() ? directoryKind : directoryKind + QLatin1Char(' ') + rest;
}

QString withoutStamp(const QString &line)
{
    static const QRegularExpression stamp(
        QStringLiteral("^[0-9]{2}:[0-9]{2}:[0-9]{2} "));
    QString text = line;
    text.remove(stamp);
    return text;
}

bool looksFailed(const QString &line)
{
    static const QRegularExpression pytestCase(
        QStringLiteral("^FAILED [^ ]+::"));
    if (pytestCase.match(line).hasMatch())
        return false;
    return line.contains(QLatin1String("FAILED")) ||
           line.contains(QLatin1String("result: fail"), Qt::CaseInsensitive) ||
           line.contains(QLatin1String("Traceback"));
}

QString lineCategory(const QString &line, int priority)
{
    return priority <= 3 || looksFailed(line) ? QStringLiteral("ERROR")
                                              : QStringLiteral("DEPLOY");
}

DeployLine fileLine(const TailedLine &line)
{
    DeployLine out;
    const QString message = withoutStamp(line.raw);
    out.text = clipped(QStringLiteral("Deploy %1: %2").arg(line.fileKind, message));
    out.category = lineCategory(message);
    return out;
}

QStringList journalUnits()
{
    return {QStringLiteral("forkmesh-source-publish.service"),
            QStringLiteral("forkmesh-mirror-fanout.service"),
            QStringLiteral("forkmesh-node-deploy.service")};
}

QString journalRole(const corelogs::JournalEntry &entry)
{
    const QString unit = entry.userUnit.isEmpty() ? entry.unit : entry.userUnit;
    if (unit == QLatin1String("forkmesh-node-deploy.service"))
        return QStringLiteral("agent");
    if (unit == QLatin1String("forkmesh-source-publish.service") ||
        unit == QLatin1String("forkmesh-mirror-fanout.service"))
        return QStringLiteral("publisher");
    return QString();
}

QStringList journalArguments(const QString &afterCursor)
{
    QStringList arguments = {QStringLiteral("--user"), QStringLiteral("--follow"),
                             QStringLiteral("--output=json"),
                             QStringLiteral("--no-pager")};
    for (const QString &unit : journalUnits())
        arguments << QStringLiteral("--unit=") + unit;
    if (corelogs::validJournalCursor(afterCursor))
        arguments << QStringLiteral("--after-cursor=") + afterCursor;
    else
        arguments << QStringLiteral("--lines=0");
    return arguments;
}

DeployLine journalLine(const corelogs::JournalEntry &entry)
{
    DeployLine out;
    const QString role = journalRole(entry);
    if (!entry.valid || role.isEmpty() || entry.message.trimmed().isEmpty())
        return out;
    const QString message = withoutStamp(entry.message);
    out.text = clipped(QStringLiteral("Deploy %1: %2").arg(role, message));
    out.category = lineCategory(message, entry.priority);
    return out;
}

QList<TailedLine> FileTailer::poll()
{
    QList<TailedLine> lines;
    const bool priming = !m_primed;
    m_primed = true;
    for (const auto &directory : std::as_const(m_directories)) {
        const QFileInfoList files =
            QDir(directory.second)
                .entryInfoList({QStringLiteral("*.log")}, QDir::Files, QDir::Time);
        for (int index = 0; index < files.size(); ++index) {
            const QFileInfo &info = files.at(index);
            const QString path = info.absoluteFilePath();
            if (priming) {
                m_offsets.insert(path, info.size());
                continue;
            }
            if (index >= kFilesPerDirectory && m_offsets.contains(path))
                break;
            qint64 offset = m_offsets.value(path, 0);
            if (info.size() < offset)
                offset = 0;
            if (info.size() == offset) {
                m_offsets.insert(path, offset);
                continue;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
                continue;
            const QByteArray chunk = file.read(kMaximumReadBytes);
            int end = chunk.lastIndexOf('\n');
            if (end < 0) {
                if (chunk.size() < kMaximumReadBytes)
                    continue;
                end = chunk.size() - 1;
            }
            m_offsets.insert(path, offset + end + 1);
            const QString kind = fileKind(directory.first, info.fileName());
            for (const QByteArray &raw : chunk.left(end + 1).split('\n')) {
                QString text = QString::fromUtf8(raw);
                while (!text.isEmpty() && text.back().isSpace())
                    text.chop(1);
                if (!text.trimmed().isEmpty())
                    lines.append({directory.first, kind, text});
            }
        }
    }
    return lines;
}

bool CrossSourceDedupe::admit(const QString &source, const QString &line,
                              qint64 nowMs)
{
    expire(nowMs);
    auto found = m_seen.find(line);
    if (found != m_seen.end() && found->source != source && found->count > 0) {
        if (--found->count == 0)
            m_seen.erase(found);
        return false;
    }
    if (found == m_seen.end() || found->source != source) {
        m_seen.insert(line, Seen{source, 1, nowMs});
        return true;
    }
    ++found->count;
    found->lastMs = nowMs;
    return true;
}

void CrossSourceDedupe::expire(qint64 nowMs)
{
    if (nowMs - m_expiredMs < 30 * 1000 && m_seen.size() < 4096)
        return;
    m_expiredMs = nowMs;
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (nowMs - it->lastMs >= kDedupeWindowMs)
            it = m_seen.erase(it);
        else
            ++it;
    }
    if (m_seen.size() >= 4096)
        m_seen.clear();
}

DeployLogFollower::DeployLogFollower(const QString &cursorPath, Sink sink,
                                     QObject *parent)
    : QObject(parent), m_cursorPath(cursorPath), m_sink(std::move(sink)),
      m_tailer({{QStringLiteral("autodeploy"), autodeployDirectory()},
                {QStringLiteral("ops"), opsDirectory()}})
{
    m_io.setMaxThreadCount(1);
}

DeployLogFollower::~DeployLogFollower()
{
    stop();
    m_io.waitForDone();
}

void DeployLogFollower::start()
{
    if (m_running)
        return;
    m_running = true;
    if (m_cursor.isEmpty()) {
        m_cursor =
            corelogs::loadCursors(m_cursorPath).value(QLatin1String(kCursorKey));
        m_cursorWritten = m_cursor;
    }
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(kPollMs);
        connect(m_timer, &QTimer::timeout, this, [this] {
            pollFiles();
            saveCursor(false);
        });
    }
    pollFiles();
    m_timer->start();
    launchJournal();
}

void DeployLogFollower::stop()
{
    if (!m_running)
        return;
    m_running = false;
    ++m_generation;
    if (m_timer)
        m_timer->stop();
    if (m_journal) {
        m_journal->disconnect(this);
        m_journal->kill();
        m_journal->waitForFinished(1000);
        m_journal->deleteLater();
        m_journal = nullptr;
    }
    saveCursor(true);
}

void DeployLogFollower::pollFiles()
{
    if (m_pollBusy)
        return;
    m_pollBusy = true;
    m_io.start([this] {
        const QList<TailedLine> lines = m_tailer.poll();
        QMetaObject::invokeMethod(
            this, [this, lines] { deliverFileLines(lines); },
            Qt::QueuedConnection);
    });
}

void DeployLogFollower::deliverFileLines(const QList<TailedLine> &lines)
{
    m_pollBusy = false;
    if (!m_running)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const TailedLine &line : lines) {
        if (line.directoryKind == QLatin1String("autodeploy") &&
            !m_dedupe.admit(QStringLiteral("file"), line.raw, now))
            continue;
        const DeployLine out = fileLine(line);
        m_sink(out.text, out.category);
    }
}

void DeployLogFollower::launchJournal()
{
    if (!m_running || m_journal)
        return;
    const QString program =
        QStandardPaths::findExecutable(QStringLiteral("journalctl"));
    if (program.isEmpty())
        return;
    const quint64 generation = ++m_generation;
    auto *process = new QProcess(this);
    m_journal = process;
    m_journalTail.clear();
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this] { readJournal(); });
    connect(process, &QProcess::finished, this, [this, process, generation] {
        process->deleteLater();
        if (m_journal == process)
            m_journal = nullptr;
        if (!m_running || generation != m_generation)
            return;
        QTimer::singleShot(kJournalRetryMs, this, [this, generation] {
            if (generation == m_generation)
                launchJournal();
        });
    });
    process->start(program, journalArguments(m_cursor));
}

void DeployLogFollower::readJournal()
{
    if (!m_journal)
        return;
    m_journalTail.append(m_journal->readAllStandardOutput());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int start = 0;
    for (int newline = m_journalTail.indexOf('\n'); newline >= 0;
         newline = m_journalTail.indexOf('\n', start)) {
        const QByteArray raw = m_journalTail.mid(start, newline - start);
        start = newline + 1;
        const corelogs::JournalEntry entry = corelogs::parseJournalLine(raw);
        if (!entry.valid)
            continue;
        if (corelogs::validJournalCursor(entry.cursor)) {
            m_cursor = entry.cursor;
            m_cursorDirty = true;
        }
        if (journalRole(entry) == QLatin1String("agent") &&
            !m_dedupe.admit(QStringLiteral("journal"), entry.message, now))
            continue;
        const DeployLine out = journalLine(entry);
        if (!out.text.isEmpty())
            m_sink(out.text, out.category);
    }
    m_journalTail.remove(0, start);
    if (m_journalTail.size() > 4 * 1024 * 1024)
        m_journalTail.clear();
}

void DeployLogFollower::saveCursor(bool force)
{
    if (!m_cursorDirty)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && (m_cursorSaving || now - m_cursorSavedMs < kCursorSaveMs))
        return;
    m_cursorSavedMs = now;
    m_cursorDirty = false;
    if (m_cursor == m_cursorWritten)
        return;
    m_cursorWritten = m_cursor;
    m_cursorSaving = true;
    m_io.start([this, path = m_cursorPath, cursor = m_cursor] {
        corelogs::saveCursors(path, {{QLatin1String(kCursorKey), cursor}});
        QMetaObject::invokeMethod(
            this, [this] { m_cursorSaving = false; }, Qt::QueuedConnection);
    });
}

} // namespace deploylogs
} // namespace forkmesh

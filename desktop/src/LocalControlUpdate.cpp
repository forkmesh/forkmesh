#include "LocalControlUpdate.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace forkmesh::localcontrol::update {

QString familyPrefix()
{
    return QStringLiteral("update.");
}

QString actionName()
{
    return QStringLiteral("updateRestart");
}

QString subjectName()
{
    return QStringLiteral("desktop");
}

QString actionSummary(int agentsRunning, const QString &version)
{
    QString text = QStringLiteral(
        "Rebuild ForkMesh from this desktop's source checkout and restart it");
    if (!version.isEmpty())
        text += QStringLiteral(" (now on %1)").arg(version);
    text += QStringLiteral(". The desktop drops this connection while it restarts.");
    if (agentsRunning == 1)
        text += QStringLiteral(" 1 agent is running; it is queued again after the restart.");
    else if (agentsRunning > 1)
        text += QStringLiteral(" %1 agents are running; they are queued again after the restart.")
                    .arg(agentsRunning);
    return text;
}

QString computerRestartActionName()
{
    return QStringLiteral("computerRestart");
}

QString computerRestartSubjectName()
{
    return QStringLiteral("computer");
}

QString computerRestartSummary(int agentsRunning)
{
    QString text = QStringLiteral(
        "Restart this entire computer. ForkMesh and every other running program will stop.");
    if (agentsRunning == 1)
        text += QStringLiteral(" 1 agent is running; it is queued again after the restart.");
    else if (agentsRunning > 1)
        text += QStringLiteral(" %1 agents are running; they are queued again after the restart.")
                    .arg(agentsRunning);
    return text;
}

bool computerRestartDenied(const QString &output)
{
    return output.contains(QLatin1String("Access denied"), Qt::CaseInsensitive) &&
           !output.contains(QLatin1String("Interactive authentication required"),
                            Qt::CaseInsensitive);
}

QStringList computerRestartBlockers(const QByteArray &inhibitorsJson,
                                    const QStringList &sessionApps)
{
    QStringList out;
    const QJsonArray data =
        QJsonDocument::fromJson(inhibitorsJson).object().value(QStringLiteral("data")).toArray();
    const QJsonArray rows = data.isEmpty() ? QJsonArray() : data.at(0).toArray();
    for (const QJsonValue &value : rows) {
        const QJsonArray row = value.toArray();
        if (row.size() < 4)
            continue;
        if (!row.at(0).toString().split(QLatin1Char(':')).contains(QLatin1String("shutdown")) ||
            row.at(3).toString() != QLatin1String("block"))
            continue;
        const QString who = row.at(1).toString().simplified();
        const QString why = row.at(2).toString().simplified();
        QStringList entries;
        if (why == QLatin1String("user session inhibited") && !sessionApps.isEmpty()) {
            entries = sessionApps;
        } else {
            QString entry = who.isEmpty() ? QStringLiteral("a program") : who;
            if (!why.isEmpty())
                entry += QStringLiteral(" (%1)").arg(why);
            entries.append(entry);
        }
        for (const QString &entry : std::as_const(entries))
            if (!out.contains(entry))
                out.append(entry);
    }
    return out;
}

QString computerRestartFailure(int exitCode, const QString &output, const QStringList &blockers)
{
    const QString detail = output.simplified();
    if (detail.contains(QLatin1String("Interactive authentication required"), Qt::CaseInsensitive))
        return QStringLiteral(
            "Could not restart the computer: it lets only the account on its screen restart "
            "it, and another user is on the screen or nobody is logged in. On the desktop, turn "
            "on Settings, General, Let my phones restart this computer (it asks for an "
            "administrator password once), then try again.");
    if (computerRestartDenied(detail) && !blockers.isEmpty())
        return QStringLiteral(
            "Could not restart the computer: %1 is holding off shutdown. Try again when it has "
            "finished, or close it on the desktop.")
            .arg(blockers.join(QStringLiteral(", ")));
    if (computerRestartDenied(detail))
        return QStringLiteral(
            "Could not restart the computer: it refused (Access denied). A program was probably "
            "holding off shutdown and has finished, or the computer was too busy to check, so "
            "try again; if it keeps failing, run systemd-inhibit --list on the desktop to see "
            "what blocks it.");
    if (detail.isEmpty())
        return QStringLiteral("Could not request a computer restart (systemctl exited %1).")
            .arg(exitCode);
    return QStringLiteral("Could not request a computer restart: %1").arg(detail);
}

QString cleanLine(const QString &line)
{
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[ -/]*[@-~]"));
    QString out = line;
    out.remove(ansi);
    out.remove(QChar(0x1B));
    while (!out.isEmpty() && out.back().isSpace())
        out.chop(1);
    if (out.size() > kMaxLineChars)
        out = out.left(kMaxLineChars - 1) + QChar(0x2026);
    return out;
}

void ProgressFeed::reset()
{
    m_lines.clear();
    m_carry.clear();
    m_status.clear();
    m_error.clear();
    m_relaunching = false;
    ++m_run;
}

void ProgressFeed::push(const QString &line)
{
    const QString clean = cleanLine(line);
    if (clean.trimmed().isEmpty())
        return;
    m_lines.append(clean);
    ++m_seq;
    if (m_lines.size() > kMaxBacklogLines)
        m_lines.remove(0, m_lines.size() - kMaxBacklogLines);
}

void ProgressFeed::append(const QString &chunk)
{
    if (chunk.isEmpty())
        return;
    const QString text = m_carry + chunk;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('\n') && c != QLatin1Char('\r'))
            continue;
        push(text.mid(start, i - start));
        start = i + 1;
    }
    m_carry = text.mid(start).right(kMaxLineChars * 4);
}

void ProgressFeed::noteStatus(const QString &status, bool isError)
{
    m_status = status;
    if (isError)
        m_error = status;
    m_relaunching = !isError && status.startsWith(QLatin1String("Relaunching"));
}

QJsonArray ProgressFeed::backlog(int maxLines) const
{
    QJsonArray out;
    const qsizetype from = std::max<qsizetype>(0, m_lines.size() - std::max(0, maxLines));
    for (qsizetype i = from; i < m_lines.size(); ++i)
        out.append(m_lines.at(i));
    return out;
}

QJsonObject ProgressFeed::batchSince(qint64 sentSeq) const
{
    if (sentSeq > m_seq || sentSeq < 0)
        sentSeq = 0;
    const qint64 firstKept = m_seq - m_lines.size() + 1;
    const qint64 wanted = sentSeq + 1;
    const qint64 floor = std::max(wanted, firstKept);
    qint64 first = m_seq + 1;
    int chars = 0;
    for (qint64 s = m_seq; s >= floor; --s) {
        const QString &line = m_lines.at(qsizetype(s - firstKept));
        if (m_seq - s >= kMaxBatchLines || (chars + line.size() > kMaxBatchChars && s != m_seq))
            break;
        chars += int(line.size());
        first = s;
    }
    QJsonArray lines;
    for (qint64 s = first; s <= m_seq; ++s)
        lines.append(m_lines.at(qsizetype(s - firstKept)));
    return {{QStringLiteral("first"), double(first)},
            {QStringLiteral("seq"), double(m_seq)},
            {QStringLiteral("skipped"), double(lines.isEmpty() ? 0 : first - wanted)},
            {QStringLiteral("lines"), lines}};
}

QJsonObject stateObject(const ProgressFeed &feed, const RunState &run)
{
    return {{QStringLiteral("run"), feed.run()},
            {QStringLiteral("running"), run.running},
            {QStringLiteral("kind"), run.kind},
            {QStringLiteral("percent"), std::clamp(run.percent, 0, 100)},
            {QStringLiteral("phase"), run.phase},
            {QStringLiteral("activity"), run.activity},
            {QStringLiteral("status"), feed.status()},
            {QStringLiteral("error"), feed.error()},
            {QStringLiteral("relaunching"), feed.relaunching()}};
}

} // namespace forkmesh::localcontrol::update

#include "PlannedRestarts.h"

#include <QRegularExpression>

#include <algorithm>

namespace forkmesh {

namespace {

const QRegularExpression &replayedSuffix()
{
    static const QRegularExpression pattern(QStringLiteral(
        " \\(logged [0-9-]+ [0-9:]+, replayed\\)$"));
    return pattern;
}

bool restartingFleetStep(const QString &step)
{
    return step == QLatin1String("daemon") || step == QLatin1String("files") ||
           step == QLatin1String("start");
}

qint64 statedWindowMs(const QString &line)
{
    static const QRegularExpression minutes(
        QStringLiteral("\\b([0-9]{1,4}) min\\b"));
    const QRegularExpressionMatch match = minutes.match(line);
    if (!match.hasMatch())
        return PlannedRestarts::kMaxWindowMs;
    const qint64 stated = match.captured(1).toLongLong() * 60 * 1000;
    if (stated <= 0)
        return PlannedRestarts::kMaxWindowMs;
    return std::min(stated, PlannedRestarts::kMaxWindowMs);
}

bool openingSignal(const QString &line, QString *node,
                   PlannedRestarts::Kind *kind)
{
    static const QRegularExpression execute(QStringLiteral(
        "^Deploy [^:]*: \\[execute\\] ([nrab][0-9]{1,4}): tell the IDS to "
        "expect (next_deploy|next_fleet)(?: ([A-Za-z0-9_-]+))?"));
    static const QRegularExpression accepted(QStringLiteral(
        "^Node ([nrab][0-9]{1,4}) (?:core|runner) fmnext-ids info: "
        "expectation accepted\\b.*\\breason=(next_deploy|next_fleet)"
        "(?: ([A-Za-z0-9_-]+))?"));
    QRegularExpressionMatch match;
    if (line.startsWith(QLatin1String("Deploy ")))
        match = execute.match(line);
    else if (line.startsWith(QLatin1String("Node ")) &&
             !replayedSuffix().match(line).hasMatch())
        match = accepted.match(line);
    if (!match.hasMatch())
        return false;
    const bool deploy = match.captured(2) == QLatin1String("next_deploy");
    if (!deploy && !restartingFleetStep(match.captured(3)))
        return false;
    *node = match.captured(1);
    *kind = deploy ? PlannedRestarts::Kind::Deploy : PlannedRestarts::Kind::Roll;
    return true;
}

enum class Return { None, Deployed, BackInDns };

Return closingSignal(const QString &line, QString *node)
{
    if (!line.startsWith(QLatin1String("Deploy ")))
        return Return::None;
    static const QRegularExpression rolled(QStringLiteral(
        "^Deploy [^:]*: ([nrab][0-9]{1,4}): rolled and back in DNS\\b"));
    static const QRegularExpression probes(QStringLiteral(
        "^Deploy [^:]*: ([nrab][0-9]{1,4}) passed [0-9]+ health probes and is "
        "back in DNS\\b"));
    static const QRegularExpression active(QStringLiteral(
        "^Deploy [^:]*: ([nrab][0-9]{1,4}): current [^ ,]+, running [^ ,]+, "
        "app active\\b"));
    for (const QRegularExpression *pattern : {&rolled, &probes}) {
        const QRegularExpressionMatch match = pattern->match(line);
        if (match.hasMatch()) {
            *node = match.captured(1);
            return Return::BackInDns;
        }
    }
    const QRegularExpressionMatch match = active.match(line);
    if (match.hasMatch()) {
        *node = match.captured(1);
        return Return::Deployed;
    }
    return Return::None;
}

bool localOrPeerClient(const QString &rest)
{
    if (!rest.contains(QString::fromUtf8(" \xC2\xB7 ")))
        return true;
    static const QRegularExpression peer(
        QStringLiteral("\\((?:local|fleet|[nrab][0-9]{1,4})[;)]"));
    return peer.match(rest).hasMatch();
}

}

PlannedRestarts::Change PlannedRestarts::observe(const QString &line,
                                                 qint64 nowMs)
{
    Change change;
    QString node;
    Kind kind = Kind::Deploy;
    if (openingSignal(line, &node, &kind)) {
        const qint64 deadline = nowMs + statedWindowMs(line);
        auto found = m_windows.find(node);
        change.node = node;
        if (found == m_windows.end()) {
            Window window;
            window.kind = kind;
            window.openedMs = nowMs;
            window.deadlineMs = deadline;
            m_windows.insert(node, window);
            change.type = Change::Opened;
            change.kind = kind;
            return change;
        }
        if (kind == Kind::Roll)
            found->kind = Kind::Roll;
        found->deadlineMs = std::max(found->deadlineMs, deadline);
        change.type = Change::Extended;
        change.kind = found->kind;
        return change;
    }
    const Return back = closingSignal(line, &node);
    if (back == Return::None)
        return change;
    auto found = m_windows.find(node);
    if (found == m_windows.end())
        return change;
    if (back == Return::Deployed && found->kind == Kind::Roll)
        return change;
    change.type = Change::Closed;
    change.node = node;
    change.kind = found->kind;
    m_windows.erase(found);
    return change;
}

bool PlannedRestarts::isRestartNoise(const QString &line, QString *node)
{
    if (!line.startsWith(QLatin1String("Node ")))
        return false;
    static const QRegularExpression prefix(QStringLiteral(
        "^Node ([nrab][0-9]{1,4}) (?:core|runner) ([A-Za-z0-9@._:-]+) "
        "[a-z]+: (.*)$"));
    const QRegularExpressionMatch match = prefix.match(line);
    if (!match.hasMatch())
        return false;
    if (match.captured(2).contains(QLatin1String("ids"), Qt::CaseInsensitive))
        return false;
    const QString message = match.captured(3);
    static const QRegularExpression refused(QStringLiteral(
        "\\b127\\.0\\.0\\.1:(?:8080|8790)\\b.*\\bconnection refused\\b"));
    static const QRegularExpression access(QStringLiteral(
        "^[A-Z]+ \\S+ -> (5[0-9]{2})\\b(.*)$"));
    static const QRegularExpression upgrade(
        QStringLiteral("\\bupgrade returned HTTP 5[0-9]{2}\\b"));
    static const QRegularExpression sealed(QStringLiteral("\\bsealed\\b"));
    bool noise = refused.match(message).hasMatch() ||
                 upgrade.match(message).hasMatch() ||
                 sealed.match(message).hasMatch();
    if (!noise) {
        const QRegularExpressionMatch request = access.match(message);
        if (request.hasMatch()) {
            const int status = request.captured(1).toInt();
            noise = status == 502 || status == 503 ||
                    (status == 500 && localOrPeerClient(request.captured(2)));
        }
    }
    if (noise && node)
        *node = match.captured(1);
    return noise;
}

bool PlannedRestarts::quiets(const QString &line, qint64 nowMs, QString *node,
                             Kind *kind) const
{
    QString noisy;
    if (!isRestartNoise(line, &noisy))
        return false;
    const auto found = m_windows.constFind(noisy);
    if (found == m_windows.constEnd() || nowMs > found->deadlineMs)
        return false;
    if (node)
        *node = noisy;
    if (kind)
        *kind = found->kind;
    return true;
}

bool PlannedRestarts::claimAnnouncement(const QString &node)
{
    auto found = m_windows.find(node);
    if (found == m_windows.end() || found->announced)
        return false;
    found->announced = true;
    return true;
}

QList<PlannedRestarts::Expired> PlannedRestarts::takeExpired(qint64 nowMs)
{
    QList<Expired> expired;
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if (nowMs < it->deadlineMs) {
            ++it;
            continue;
        }
        Expired item;
        item.node = it.key();
        item.kind = it->kind;
        item.windowMs = it->deadlineMs - it->openedMs;
        expired.append(item);
        it = m_windows.erase(it);
    }
    std::sort(expired.begin(), expired.end(),
              [](const Expired &a, const Expired &b) { return a.node < b.node; });
    return expired;
}

qint64 PlannedRestarts::nextDeadlineMs() const
{
    qint64 next = 0;
    for (const Window &window : m_windows)
        if (next == 0 || window.deadlineMs < next)
            next = window.deadlineMs;
    return next;
}

QString PlannedRestarts::kindWord(Kind kind)
{
    return kind == Kind::Roll ? QStringLiteral("roll") : QStringLiteral("deploy");
}

QString PlannedRestarts::announcementTitle(const QString &node, Kind kind)
{
    return QString::fromUtf8("%1 restarting for %2 \xE2\x80\x94 errors during "
                             "the restart are in the Log")
        .arg(node, kindWord(kind));
}

QString PlannedRestarts::failureLine(const Expired &expired)
{
    return QStringLiteral("%1 did not come back after its %2 - no \"back in "
                          "DNS\" or \"app active\" within %3 min of its "
                          "planned restart")
        .arg(expired.node, kindWord(expired.kind))
        .arg(std::max<qint64>(1, (expired.windowMs + 59999) / 60000));
}

}

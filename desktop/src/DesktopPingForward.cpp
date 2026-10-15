#include "DesktopPingForward.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace forkmesh::pingforward {

namespace {

const QRegularExpression &pingIdPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.:-]{1,64}$"));
    return pattern;
}

const QRegularExpression &nodePattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    return pattern;
}

const QRegularExpression &kindPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{0,40}$"));
    return pattern;
}

const QRegularExpression &sessionPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{0,12}$"));
    return pattern;
}

const QRegularExpression &repoPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^$|^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$"));
    return pattern;
}

bool knownCategory(const QString &category)
{
    return category.isEmpty() || category == QLatin1String("agent-finished") ||
           category == QLatin1String("agent-failed");
}

QString kindToken(const QString &kind)
{
    QString out;
    for (const QChar c : kind) {
        if (out.size() >= 40)
            break;
        if (c.isLetterOrNumber() && c.unicode() < 128)
            out.append(c);
        else if (c == QLatin1Char('_') || c == QLatin1Char('-'))
            out.append(c);
    }
    return out;
}

QString sessionFor(const Source &source)
{
    if (source.linkKind != QLatin1String("agent") || source.linkNumber <= 0)
        return {};
    return QString::number(source.linkNumber);
}

}

QString proof()
{
    return QStringLiteral("forkmesh-account-desktop-ping-v1");
}

QString deleteProof()
{
    return QStringLiteral("forkmesh-account-desktop-ping-delete-v1");
}

QString path()
{
    return QStringLiteral("/api/notifications/desktop");
}

QString kind()
{
    return QStringLiteral("desktop_ping");
}

QString settingKey()
{
    return QStringLiteral("notifications/phonePings");
}

QString onlineKind()
{
    return QStringLiteral("desktop-online");
}

QString onlineSettingKey()
{
    return QStringLiteral("notifications/phoneOnlinePing");
}

QString coreOnlineFeature()
{
    return QStringLiteral("online-ping");
}

bool phoneLinked(const QList<Phone> &phones, const QString &account,
                 qint64 nowMs)
{
    const QString owner = account.trimmed().toLower();
    for (const Phone &phone : phones) {
        if (phone.platform == QLatin1String("web"))
            continue;
        const QString user = phone.user.trimmed().toLower();
        if (!user.isEmpty() && !owner.isEmpty() && user != owner)
            continue;
        if (phone.connected)
            return true;
        if (phone.lastSeenMs > 0 && nowMs - phone.lastSeenMs <= kPhoneRecentMs)
            return true;
    }
    return false;
}

bool forwardsKind(const QString &kind)
{
    static const QSet<QString> skipped{
        QStringLiteral("chat"),   QStringLiteral("dm"),
        QStringLiteral("direct-message"), QStringLiteral("mention"),
        QStringLiteral("search"), QStringLiteral("local"),
        QStringLiteral("dialog"), QStringLiteral("error"),
        QStringLiteral("prompt"), QStringLiteral("timer"),
        QStringLiteral("timer_finished"), QStringLiteral("alarm"),
        QStringLiteral("desktop_ping"),
    };
    return !skipped.contains(kind.trimmed().toLower());
}

QString categoryFor(const QString &kind, const QString &linkKind, bool warning)
{
    if (kind == QLatin1String("agent-done"))
        return QStringLiteral("agent-finished");
    if (linkKind == QLatin1String("agent"))
        return warning ? QStringLiteral("agent-failed")
                       : QStringLiteral("agent-finished");
    return {};
}

QString fitText(const QString &text, int max, bool singleLine)
{
    QString clean;
    clean.reserve(text.size());
    for (const QChar c : text) {
        if (c == QLatin1Char('\n'))
            clean.append(singleLine ? QLatin1Char(' ') : c);
        else if (c.category() == QChar::Other_Control ||
                 c.category() == QChar::Other_Format)
            clean.append(QLatin1Char(' '));
        else
            clean.append(c);
    }
    if (singleLine) {
        clean = clean.simplified();
    } else {
        QStringList lines = clean.split(QLatin1Char('\n'));
        for (QString &line : lines)
            line = line.simplified();
        lines.removeAll(QString());
        clean = lines.join(QLatin1Char('\n'));
    }
    if (max <= 0)
        return {};
    if (clean.size() <= max)
        return clean;
    QString cut = clean.left(max - 1);
    if (!cut.isEmpty() && cut.back().isHighSurrogate())
        cut.chop(1);
    return cut.trimmed() + QString::fromUtf8("\xE2\x80\xA6");
}

QString nodeToken(const QString &node)
{
    QString out;
    for (const QChar c : node.trimmed().toLower()) {
        if (out.size() >= 64)
            break;
        const bool ascii = c.unicode() < 128;
        if ((ascii && c.isLetterOrNumber()) || c == QLatin1Char('_') ||
            c == QLatin1Char('.') || c == QLatin1Char('-'))
            out.append(c);
        else
            out.append(QLatin1Char('-'));
    }
    return out.isEmpty() ? QStringLiteral("desktop") : out;
}

Ping pingFor(const Source &source)
{
    Ping ping;
    ping.pingId = QStringLiteral("%1-%2").arg(source.timestampMs).arg(source.localId);
    ping.node = nodeToken(source.node);
    ping.kind = kindToken(source.kind);
    ping.category = categoryFor(source.kind, source.linkKind, source.warning);
    ping.sessionId = sessionFor(source);
    const QString repo = source.repo.trimmed();
    ping.repo = repoPattern().match(repo).hasMatch() ? repo : QString();
    ping.warning = source.warning;
    ping.title = fitText(source.title, kTitleMax, true);
    ping.body = fitText(source.body, kBodyMax, false);
    return ping;
}

bool valid(const Ping &ping)
{
    return pingIdPattern().match(ping.pingId).hasMatch() &&
           nodePattern().match(ping.node).hasMatch() &&
           kindPattern().match(ping.kind).hasMatch() &&
           sessionPattern().match(ping.sessionId).hasMatch() &&
           repoPattern().match(ping.repo).hasMatch() &&
           knownCategory(ping.category) && ping.title.size() <= kTitleMax &&
           ping.body.size() <= kBodyMax &&
           !(ping.title.trimmed().isEmpty() && ping.body.trimmed().isEmpty());
}

QString resource(const Ping &ping)
{
    return QStringList{ping.pingId,
                       ping.node,
                       ping.kind,
                       ping.category,
                       ping.sessionId,
                       ping.repo,
                       ping.warning ? QStringLiteral("1") : QStringLiteral("0"),
                       ping.title,
                       ping.body}
        .join(QLatin1Char('\n'));
}

QString canonical(const QString &user, const Ping &ping, const QString &ts)
{
    return proof() + QLatin1Char('\n') + user + QLatin1Char('\n') + resource(ping) +
           QLatin1Char('\n') + ts;
}

QString deleteResource(const QString &node, const QStringList &pingIds)
{
    return QStringList{node, pingIds.join(QLatin1Char('\n'))}.join(QLatin1Char('\n'));
}

QString deleteCanonical(const QString &user, const QString &node,
                        const QStringList &pingIds, const QString &ts)
{
    return deleteProof() + QLatin1Char('\n') + user + QLatin1Char('\n') +
           deleteResource(node, pingIds) + QLatin1Char('\n') + ts;
}

QJsonObject deleteBody(const QString &node, const QStringList &pingIds)
{
    return QJsonObject{{QStringLiteral("node"), node},
                       {QStringLiteral("pingIds"), QJsonArray::fromStringList(pingIds)}};
}

QJsonObject body(const Ping &ping)
{
    return QJsonObject{
        {QStringLiteral("pingId"), ping.pingId},
        {QStringLiteral("node"), ping.node},
        {QStringLiteral("kind"), ping.kind},
        {QStringLiteral("category"), ping.category},
        {QStringLiteral("sessionId"), ping.sessionId},
        {QStringLiteral("repo"), ping.repo},
        {QStringLiteral("warning"), ping.warning},
        {QStringLiteral("title"), ping.title},
        {QStringLiteral("body"), ping.body},
    };
}

QString duplicateKey(const Ping &ping)
{
    QString key;
    const QString text = (ping.title + QLatin1Char(' ') + ping.body).toLower();
    for (const QChar c : text) {
        if (c.isLetterOrNumber())
            key.append(c);
    }
    return key;
}

bool isOwnEcho(const QJsonObject &alert, const QString &node)
{
    if (alert.value(QStringLiteral("kind")).toString() != kind())
        return false;
    const QString from = alert.value(QStringLiteral("meta"))
                             .toObject()
                             .value(QStringLiteral("node"))
                             .toString();
    return !from.isEmpty() && from == nodeToken(node);
}

bool isFreshCelebration(const QJsonObject &alert, qint64 nowMs)
{
    if (alert.value(QStringLiteral("id")).toString().trimmed().isEmpty() ||
        alert.value(QStringLiteral("readAt")).toDouble() > 0)
        return false;
    if (alert.value(QStringLiteral("meta"))
            .toObject()
            .value(QStringLiteral("celebrate"))
            .toString()
            .trimmed()
            .isEmpty())
        return false;
    const qint64 ts =
        static_cast<qint64>(alert.value(QStringLiteral("ts")).toDouble());
    return ts > 0 && qAbs(nowMs - ts) <= kCelebrationFreshMs;
}

bool cameOnline(qint64 lastOnlineMs, qint64 nowMs)
{
    return lastOnlineMs <= 0 || nowMs - lastOnlineMs >= kOnlineGapMs;
}

QString offlineSpan(qint64 ms)
{
    const qint64 minutes = qMax<qint64>(1, ms / 60000);
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 24)
        return minutes % 60 ? QStringLiteral("%1 h %2 min").arg(hours).arg(minutes % 60)
                            : QStringLiteral("%1 h").arg(hours);
    const qint64 days = hours / 24;
    return hours % 24 ? QStringLiteral("%1 d %2 h").arg(days).arg(hours % 24)
                      : QStringLiteral("%1 d").arg(days);
}

Source onlineSource(const QString &node, qint64 lastOnlineMs, qint64 nowMs)
{
    Source source;
    source.timestampMs = nowMs;
    source.node = node;
    source.kind = onlineKind();
    const QString name = node.trimmed().isEmpty() ? QStringLiteral("Your desktop")
                                                  : node.trimmed();
    source.title = QStringLiteral("%1 is online").arg(name);
    source.body = lastOnlineMs <= 0
                      ? QStringLiteral("ForkMesh started on this desktop and reached the relay.")
                      : QStringLiteral("Back online after %1 offline.")
                            .arg(offlineSpan(nowMs - lastOnlineMs));
    return source;
}

bool Gate::admit(const Ping &ping, qint64 nowMs)
{
    while (!m_sent.isEmpty() && nowMs - m_sent.first().atMs > kBurstWindowMs)
        m_sent.removeFirst();
    if (m_sent.size() >= kBurstLimit)
        return false;
    const QString key = duplicateKey(ping);
    const QString session = ping.sessionId;
    for (const Sent &sent : std::as_const(m_sent)) {
        if (nowMs - sent.atMs > kDuplicateWindowMs)
            continue;
        if (!session.isEmpty() && sent.key.startsWith(session + QLatin1Char(':')))
            return false;
        const QString previous = sent.key.mid(sent.key.indexOf(QLatin1Char(':')) + 1);
        const QString &shorter = previous.size() < key.size() ? previous : key;
        const QString &longer = previous.size() < key.size() ? key : previous;
        if (shorter.size() >= 12 && longer.contains(shorter.left(48)))
            return false;
    }
    m_sent.append({nowMs, session + QLatin1Char(':') + key});
    return true;
}

} // namespace forkmesh::pingforward

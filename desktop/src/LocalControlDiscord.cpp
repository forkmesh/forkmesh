#include "LocalControlDiscord.h"

#include <QCryptographicHash>
#include <QJsonArray>

namespace forkmesh::localcontrol::discord {

namespace {

bool isSnowflake(const QString &id)
{
    if (id.isEmpty() || id.size() > 24)
        return false;
    for (const QChar ch : id) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return false;
    }
    return true;
}

QString bounded(const QString &value, int max)
{
    return value.left(max);
}

} // namespace

QString familyPrefix()
{
    return QStringLiteral("discord.");
}

bool validChannelId(const QString &id)
{
    return isSnowflake(id);
}

bool validReplyId(const QString &id)
{
    if (id.isEmpty())
        return true;
    static const QString prefix = QStringLiteral("discord-message:");
    const QString raw = id.startsWith(prefix) ? id.mid(prefix.size()) : id;
    return isSnowflake(raw);
}

QString conversationForChannelId(const QString &id)
{
    if (!validChannelId(id))
        return QString();
    return QStringLiteral("#discord/") + id;
}

int boundedLimit(int requested)
{
    if (requested <= 0)
        return kDefaultMessages;
    return qMin(requested, kMaxMessages);
}

QString normalizedSendText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxSendChars)
        return QString();
    return trimmed;
}

QString sendTarget(const QString &channelId, const QString &text,
                   const QString &replyTo)
{
    const QByteArray digest = QCryptographicHash::hash(
        (text + QLatin1Char('\n') + replyTo).toUtf8(), QCryptographicHash::Sha256);
    return channelId + QLatin1Char(':') +
           QString::fromLatin1(digest.toHex().left(32));
}

QString sendSummary(const QString &displayName, const QString &text, bool reply)
{
    QString preview = text.simplified();
    if (preview.size() > 80)
        preview = preview.left(79) + QChar(0x2026);
    const QString where = displayName.trimmed().isEmpty()
                              ? QStringLiteral("Discord")
                              : bounded(displayName.trimmed(), kMaxNameChars);
    return (reply ? QStringLiteral("Reply in %1: “%2”")
                  : QStringLiteral("Send to %1: “%2”"))
        .arg(where, preview);
}

QJsonObject channelRow(const ChannelFacts &facts)
{
    return QJsonObject{
        {QStringLiteral("id"), facts.id},
        {QStringLiteral("name"), bounded(facts.shortName.isEmpty() ? facts.display
                                                                  : facts.shortName,
                                         kMaxNameChars)},
        {QStringLiteral("display"), bounded(facts.display, kMaxNameChars)},
        {QStringLiteral("group"), bounded(facts.group, kMaxNameChars)},
        {QStringLiteral("topic"), bounded(facts.topic.simplified(), kMaxTopicChars)},
        {QStringLiteral("direct"), facts.direct},
        {QStringLiteral("guild"), facts.guild},
        {QStringLiteral("canSend"), facts.canSend},
    };
}

QJsonObject messageRow(const MessageFacts &facts)
{
    QJsonObject row{
        {QStringLiteral("id"), bounded(facts.id, 64)},
        {QStringLiteral("sender"), bounded(facts.senderName, kMaxNameChars)},
        {QStringLiteral("text"), bounded(facts.text, kMaxRowTextChars)},
        {QStringLiteral("ts"), double(facts.timestampMs)},
        {QStringLiteral("self"), facts.self},
    };
    if (facts.text.size() > kMaxRowTextChars)
        row.insert(QStringLiteral("truncated"), true);
    if (!facts.threadRootId.isEmpty())
        row.insert(QStringLiteral("root"), bounded(facts.threadRootId, 64));
    if (!facts.fileName.isEmpty()) {
        row.insert(QStringLiteral("fileName"), bounded(facts.fileName, 180));
        row.insert(QStringLiteral("fileMime"), bounded(facts.fileMime, 100));
    }
    if (facts.edited)
        row.insert(QStringLiteral("edited"), true);
    if (!facts.reactions.isEmpty()) {
        QJsonObject reactions;
        int count = 0;
        for (auto it = facts.reactions.constBegin();
             it != facts.reactions.constEnd() && count < kMaxReactions; ++it, ++count) {
            if (!it.value().isEmpty())
                reactions.insert(bounded(it.key(), 32), int(it.value().size()));
        }
        if (!reactions.isEmpty())
            row.insert(QStringLiteral("reactions"), reactions);
    }
    return row;
}

} // namespace forkmesh::localcontrol::discord

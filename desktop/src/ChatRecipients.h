#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cmath>

namespace forkmesh::recipients {

inline constexpr int kMaxRecipients = 1000000;
inline constexpr int kRememberedRefs = 256;
inline constexpr int kReceivedRefsPerFrame = 64;
inline constexpr int kMaxRecipientNames = 20;

inline int countFromValue(const QJsonValue &value)
{
    if (!value.isDouble())
        return -1;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 || number > kMaxRecipients ||
        std::floor(number) != number)
        return -1;
    return int(number);
}

inline int countFromEnvelope(const QJsonObject &envelope)
{
    return countFromValue(envelope.value(QLatin1String("recipients")));
}

inline QStringList namesFromValue(const QJsonValue &value)
{
    static const QRegularExpression handle(
        QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$"));
    QStringList names;
    const QJsonArray entries = value.toArray();
    for (const QJsonValue &entry : entries) {
        const QString name = entry.toString();
        if (handle.match(name).hasMatch() && !names.contains(name))
            names.append(name);
        if (names.size() == kMaxRecipientNames)
            break;
    }
    return names;
}

inline QStringList namesFromEnvelope(const QJsonObject &envelope)
{
    return namesFromValue(envelope.value(QLatin1String("recipientNames")));
}

inline bool isAckFrame(const QJsonObject &envelope)
{
    return envelope.value(QLatin1String("kind")).toString() ==
           QLatin1String("forkmesh-recipients");
}

inline bool isServerCopiesFrame(const QJsonObject &envelope)
{
    return envelope.value(QLatin1String("kind")).toString() ==
           QLatin1String("forkmesh-server-copies");
}

inline int serverCopiesFromEnvelope(const QJsonObject &envelope)
{
    return countFromValue(envelope.value(QLatin1String("stored")));
}

inline QString relayRef(const QJsonObject &envelope)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{40}$"));
    const QString ref = envelope.value(QLatin1String("ref")).toString();
    return pattern.match(ref).hasMatch() ? ref : QString();
}

inline QByteArray refsFrame(const QString &kind, const QStringList &refs)
{
    QJsonObject frame;
    frame.insert(QStringLiteral("kind"), kind);
    frame.insert(QStringLiteral("v"), 1);
    frame.insert(QStringLiteral("refs"), QJsonArray::fromStringList(refs));
    return QJsonDocument(frame).toJson(QJsonDocument::Compact);
}

inline QByteArray receivedFrame(const QStringList &refs)
{
    return refsFrame(QStringLiteral("forkmesh-received"), refs);
}

inline QByteArray supersededFrame(const QStringList &refs)
{
    return refsFrame(QStringLiteral("forkmesh-superseded"), refs);
}

inline bool isHistoryEndFrame(const QJsonObject &envelope)
{
    return envelope.value(QLatin1String("kind")).toString() ==
           QLatin1String("forkmesh-history-end");
}

inline QString serverCopiesLabel(int stored)
{
    if (stored < 0)
        return QString();
    if (stored == 0)
        return QStringLiteral("Not stored on the server");
    if (stored == 1)
        return QStringLiteral("1 message on the server");
    return QStringLiteral("%1 messages on the server").arg(stored);
}

inline QString serverCopiesTooltip(int stored, const QString &peer)
{
    if (stored < 0)
        return QString();
    if (stored == 0)
        return QStringLiteral("Every message @%1 received has been deleted "
                              "from the ForkMesh server.")
            .arg(peer);
    return QStringLiteral("Stored encrypted on the ForkMesh server until @%1 "
                          "receives %2, then deleted.")
        .arg(peer, stored == 1 ? QStringLiteral("it") : QStringLiteral("them"));
}

inline QString frameRef(const QByteArray &frame)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(frame, QCryptographicHash::Sha256)
            .toHex()
            .left(40));
}

inline QString label(int count)
{
    if (count < 0)
        return QString();
    return QStringLiteral("to %1").arg(count);
}

inline QString tooltip(int count, const QStringList &names = {})
{
    if (count < 0)
        return QString();
    const QString title = count == 1
                              ? QStringLiteral("Delivered to 1 person")
                              : QStringLiteral("Delivered to %1 people").arg(count);
    const QStringList shown = names.mid(0, count);
    if (shown.isEmpty())
        return title;
    QStringList handles;
    for (const QString &name : shown)
        handles.append(QLatin1Char('@') + name);
    QString text = title + QStringLiteral(": ") + handles.join(QStringLiteral(", "));
    if (count > shown.size())
        text += QStringLiteral(" and %1 more").arg(count - shown.size());
    return text;
}

}

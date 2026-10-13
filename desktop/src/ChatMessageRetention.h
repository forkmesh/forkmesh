#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSettings>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace forkmesh::chatretention {

inline constexpr qint64 kHourMs = qint64(60) * 60 * 1000;
inline constexpr qint64 kDayMs = 24 * kHourMs;
inline constexpr qint64 kDefaultMs = 7 * kDayMs;
inline constexpr qint64 kMinMs = kHourMs;
inline constexpr qint64 kPruneIntervalMs = 5 * 60 * 1000;
inline constexpr int kMaxChats = 500;
inline constexpr int kMaxKeyLength = 160;
inline constexpr char kSetting[] = "chat/retentionPreferences";
inline constexpr char kEndpointLeaf[] = "chat-retention";

struct Choice {
    qint64 ms;
    QString label;
};

inline QList<Choice> choices()
{
    return {
        {kDefaultMs, QStringLiteral("7 days")},
        {3 * kDayMs, QStringLiteral("3 days")},
        {kDayMs, QStringLiteral("1 day")},
        {8 * kHourMs, QStringLiteral("8 hours")},
        {kHourMs, QStringLiteral("1 hour")},
    };
}

inline qint64 sanitizeMs(qint64 ms)
{
    if (ms <= 0)
        return 0;
    return qBound(kMinMs, ms, kDefaultMs);
}

inline qint64 clampMs(qint64 ms)
{
    const qint64 clean = sanitizeMs(ms);
    return clean ? clean : kDefaultMs;
}

inline QString label(qint64 ms)
{
    const qint64 value = clampMs(ms);
    for (const Choice &choice : choices()) {
        if (choice.ms == value)
            return choice.label;
    }
    if (value % kDayMs == 0) {
        const qint64 days = value / kDayMs;
        return QStringLiteral("%1 day%2").arg(days).arg(days == 1 ? "" : "s");
    }
    const qint64 hours = qMax<qint64>(1, (value + kHourMs / 2) / kHourMs);
    return QStringLiteral("%1 hour%2").arg(hours).arg(hours == 1 ? "" : "s");
}

inline bool validKey(const QString &key)
{
    if (key.trimmed().isEmpty() || key.size() > kMaxKeyLength)
        return false;
    for (const QChar ch : key) {
        if (ch.unicode() < 32 || ch.unicode() == 127)
            return false;
    }
    return true;
}

inline QString officeKey(const QString &channelId)
{
    return QStringLiteral("channel:") + channelId;
}

struct Preferences {
    qint64 defaultMs = kDefaultMs;
    QHash<QString, qint64> chats;
    qint64 updatedAt = 0;

    qint64 overrideMs(const QString &key) const { return chats.value(key, 0); }

    qint64 effectiveMs(const QString &key) const
    {
        const qint64 chat = overrideMs(key);
        return chat ? chat : defaultMs;
    }

    bool operator==(const Preferences &other) const
    {
        return defaultMs == other.defaultMs && chats == other.chats &&
               updatedAt == other.updatedAt;
    }
};

inline Preferences fromJson(const QJsonObject &object)
{
    Preferences prefs;
    prefs.defaultMs =
        clampMs(object.value(QStringLiteral("defaultMs")).toVariant().toLongLong());
    const QJsonObject chats = object.value(QStringLiteral("chats")).toObject();
    for (auto it = chats.constBegin(); it != chats.constEnd(); ++it) {
        if (prefs.chats.size() >= kMaxChats)
            break;
        const qint64 ms = sanitizeMs(it.value().toVariant().toLongLong());
        if (ms && validKey(it.key()))
            prefs.chats.insert(it.key(), ms);
    }
    prefs.updatedAt = qMax<qint64>(
        0, object.value(QStringLiteral("updatedAt")).toVariant().toLongLong());
    return prefs;
}

inline QJsonObject toJson(const Preferences &prefs)
{
    QJsonObject chats;
    for (auto it = prefs.chats.constBegin(); it != prefs.chats.constEnd(); ++it)
        chats.insert(it.key(), it.value());
    return {
        {QStringLiteral("defaultMs"), prefs.defaultMs},
        {QStringLiteral("chats"), chats},
        {QStringLiteral("updatedAt"), prefs.updatedAt},
    };
}

inline QJsonObject defaultBody(qint64 ms)
{
    const qint64 clean = sanitizeMs(ms);
    return {{QStringLiteral("defaultMs"),
             clean ? QJsonValue(clean) : QJsonValue(QJsonValue::Null)}};
}

inline QJsonObject chatBody(const QString &key, qint64 ms)
{
    const qint64 clean = sanitizeMs(ms);
    return {{QStringLiteral("chat"), key},
            {QStringLiteral("retentionMs"),
             clean ? QJsonValue(clean) : QJsonValue(QJsonValue::Null)}};
}

inline Preferences &mutableState()
{
    static Preferences prefs = [] {
        const QByteArray raw =
            QSettings().value(QLatin1String(kSetting)).toByteArray();
        return fromJson(QJsonDocument::fromJson(raw).object());
    }();
    return prefs;
}

inline const Preferences &preferences()
{
    return mutableState();
}

inline bool setPreferences(const Preferences &prefs)
{
    if (mutableState() == prefs)
        return false;
    mutableState() = prefs;
    QSettings().setValue(QLatin1String(kSetting),
                         QJsonDocument(toJson(prefs)).toJson(QJsonDocument::Compact));
    return true;
}

inline std::function<QString(const QString &)> &keyResolver()
{
    static std::function<QString(const QString &)> resolver;
    return resolver;
}

inline QString chatKeyFor(const QString &conversation)
{
    const auto &resolver = keyResolver();
    const QString key = resolver ? resolver(conversation) : QString();
    return key.isEmpty() ? conversation : key;
}

inline qint64 defaultMs()
{
    return preferences().defaultMs;
}

inline qint64 overrideMsFor(const QString &conversation)
{
    return preferences().overrideMs(chatKeyFor(conversation));
}

inline qint64 retentionMsFor(const QString &conversation)
{
    return preferences().effectiveMs(chatKeyFor(conversation));
}

inline qint64 expiryCutoff(const QString &conversation, qint64 nowMs)
{
    return nowMs - retentionMsFor(conversation);
}

inline Preferences withChat(Preferences prefs, const QString &key, qint64 ms)
{
    const qint64 clean = sanitizeMs(ms);
    if (clean)
        prefs.chats.insert(key, clean);
    else
        prefs.chats.remove(key);
    return prefs;
}

inline Preferences withDefault(Preferences prefs, qint64 ms)
{
    prefs.defaultMs = clampMs(ms);
    return prefs;
}

} // namespace forkmesh::chatretention

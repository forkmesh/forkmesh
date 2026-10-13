#include "UserStatus.h"

#include <QJsonValue>
#include <QTimeZone>

namespace forkmesh::userstatus {

namespace {

// world._emoji_base: the code points a status emoji may start (or join) with.
bool emojiBase(char32_t c)
{
    switch (c) {
    case 0x00A9: case 0x00AE: case 0x203C: case 0x2049: case 0x2122:
    case 0x2139: case 0x24C2: case 0x3030: case 0x303D: case 0x3297:
    case 0x3299: case 0x231A: case 0x231B: case 0x2328: case 0x23CF:
    case 0x25B6: case 0x25C0: case 0x2B50: case 0x2B55:
        return true;
    default:
        break;
    }
    return (c >= 0x2194 && c <= 0x21FF) || (c >= 0x23E9 && c <= 0x23F3) ||
           (c >= 0x23F8 && c <= 0x23FA) || (c >= 0x25AA && c <= 0x25AB) ||
           (c >= 0x25FB && c <= 0x25FE) || (c >= 0x2600 && c <= 0x27BF) ||
           (c >= 0x2934 && c <= 0x2935) || (c >= 0x2B05 && c <= 0x2B07) ||
           (c >= 0x2B1B && c <= 0x2B1C) ||
           (c >= 0x1F000 && c <= 0x1FAFF && !(c >= 0x1F1E6 && c <= 0x1F1FF));
}

bool isRegionalIndicator(char32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }
bool isTag(char32_t c) { return c >= 0xE0020 && c <= 0xE007E; }

// One pictograph with its optional variation selector, skin tone and tag
// run starting at `at`; the index after it, or -1.
int consumePictograph(const QList<uint> &cps, int at)
{
    const int size = cps.size();
    if (at >= size || !emojiBase(cps.at(at)))
        return -1;
    const uint base = cps.at(at++);
    if (at < size && (cps.at(at) == 0xFE0E || cps.at(at) == 0xFE0F))
        ++at;
    if (at < size && cps.at(at) >= 0x1F3FB && cps.at(at) <= 0x1F3FF)
        ++at;
    if (at < size && isTag(cps.at(at))) {
        if (base != 0x1F3F4) // only the black flag carries subdivision tags
            return -1;
        while (at < size && isTag(cps.at(at)))
            ++at;
        if (at >= size || cps.at(at) != 0xE007F)
            return -1;
        ++at;
    }
    return at;
}

QString clockText(const QDateTime &at)
{
    return at.toString(QStringLiteral("h:mm AP"));
}

} // namespace

QString cleanEmoji(const QString &value)
{
    const QString emoji =
        value.trimmed().normalized(QString::NormalizationForm_C);
    if (emoji.isEmpty() || emoji.toUtf8().size() > 96)
        return QString();
    const QList<uint> cps = emoji.toUcs4();
    if (cps.size() > 24)
        return QString();
    if (cps.size() == 2 && isRegionalIndicator(cps.at(0)) &&
        isRegionalIndicator(cps.at(1)))
        return emoji;
    if ((cps.size() == 2 || cps.size() == 3) &&
        cps.at(0) < 0x80 &&
        QStringLiteral("#*0123456789").contains(QChar(char16_t(cps.at(0)))) &&
        cps.last() == 0x20E3 &&
        (cps.size() == 2 || cps.at(1) == 0xFE0F))
        return emoji;
    int at = consumePictograph(cps, 0);
    if (at < 0)
        return QString();
    while (at < cps.size()) {
        if (cps.at(at) != 0x200D)
            return QString();
        at = consumePictograph(cps, at + 1);
        if (at < 0)
            return QString();
    }
    return emoji;
}

QString cleanText(const QString &value)
{
    return value.simplified().left(kTextMax).trimmed();
}

UserStatus fromJson(const QJsonObject &object)
{
    UserStatus status;
    status.away = object.value(QStringLiteral("away")).toBool(false);
    status.emoji = cleanEmoji(object.value(QStringLiteral("emoji")).toString());
    status.text = cleanText(object.value(QStringLiteral("text")).toString());
    const double expires = object.value(QStringLiteral("expiresAt")).toDouble(0);
    status.expiresAtMs = expires > 0 ? qint64(expires) : 0;
    if (!status.hasCustom())
        status.expiresAtMs = 0;
    return status;
}

QJsonObject toJson(const UserStatus &status)
{
    return {{QStringLiteral("away"), status.away},
            {QStringLiteral("emoji"), status.emoji},
            {QStringLiteral("text"), status.text},
            {QStringLiteral("expiresAt"), double(status.expiresAtMs)}};
}

bool readFrom(const QJsonObject &record, UserStatus *status)
{
    const QJsonValue value = record.value(QStringLiteral("userStatus"));
    if (!value.isObject())
        return false;
    if (status)
        *status = fromJson(value.toObject());
    return true;
}

bool isExpired(const UserStatus &status, qint64 nowMs)
{
    return status.expiresAtMs != 0 && status.expiresAtMs <= nowMs;
}

UserStatus visible(const UserStatus &status, qint64 nowMs)
{
    if (!isExpired(status, nowMs))
        return status;
    UserStatus shown;
    shown.away = status.away;
    return shown;
}

QString clearAfterLabel(ClearAfter choice)
{
    switch (choice) {
    case ClearAfter::Never:
        return QStringLiteral("Don't clear");
    case ClearAfter::ThirtyMinutes:
        return QStringLiteral("30 minutes");
    case ClearAfter::OneHour:
        return QStringLiteral("1 hour");
    case ClearAfter::FourHours:
        return QStringLiteral("4 hours");
    case ClearAfter::Today:
        return QStringLiteral("Today");
    case ClearAfter::ThisWeek:
        return QStringLiteral("This week");
    case ClearAfter::Custom:
        return QStringLiteral("Custom…");
    }
    return QString();
}

QList<ClearAfter> clearAfterChoices()
{
    return {ClearAfter::Never,     ClearAfter::ThirtyMinutes,
            ClearAfter::OneHour,   ClearAfter::FourHours,
            ClearAfter::Today,     ClearAfter::ThisWeek,
            ClearAfter::Custom};
}

qint64 clearAfterExpiry(ClearAfter choice, const QDateTime &now,
                        const QDateTime &custom)
{
    const qint64 nowMs = now.toMSecsSinceEpoch();
    constexpr qint64 kMinute = 60 * 1000;
    const QTime endOfDay(23, 59, 59, 999);
    switch (choice) {
    case ClearAfter::Never:
        return 0;
    case ClearAfter::ThirtyMinutes:
        return nowMs + 30 * kMinute;
    case ClearAfter::OneHour:
        return nowMs + 60 * kMinute;
    case ClearAfter::FourHours:
        return nowMs + 4 * 60 * kMinute;
    case ClearAfter::Today:
        return QDateTime(now.date(), endOfDay, now.timeZone())
            .toMSecsSinceEpoch();
    case ClearAfter::ThisWeek: {
        // Qt counts Monday as 1 and Sunday as 7, so on a Sunday this is
        // the end of today.
        const QDate sunday = now.date().addDays(7 - now.date().dayOfWeek());
        return QDateTime(sunday, endOfDay, now.timeZone()).toMSecsSinceEpoch();
    }
    case ClearAfter::Custom:
        return custom.isValid() ? custom.toMSecsSinceEpoch() : 0;
    }
    return 0;
}

QList<Preset> presets()
{
    return {
        {QStringLiteral("📅"), QStringLiteral("In a meeting"),
         ClearAfter::OneHour},
        {QStringLiteral("🚌"), QStringLiteral("Commuting"),
         ClearAfter::ThirtyMinutes},
        {QStringLiteral("🤒"), QStringLiteral("Out sick"), ClearAfter::Today},
        {QStringLiteral("🌴"), QStringLiteral("Vacationing"),
         ClearAfter::Never},
        {QStringLiteral("🏡"), QStringLiteral("Working remotely"),
         ClearAfter::Today},
    };
}

QString presetLabel(const Preset &preset)
{
    return QStringLiteral("%1 %2 — %3")
        .arg(preset.emoji, preset.text, clearAfterLabel(preset.clearAfter));
}

QStringList quickEmoji()
{
    return {QStringLiteral("📅"), QStringLiteral("🚌"), QStringLiteral("🤒"),
            QStringLiteral("🌴"), QStringLiteral("🏡"), QStringLiteral("🎧"),
            QStringLiteral("🍔"), QStringLiteral("💻"), QStringLiteral("🎉"),
            QStringLiteral("🔕")};
}

QString untilLabel(qint64 expiresAtMs, const QDateTime &now)
{
    if (expiresAtMs <= 0)
        return QString();
    const QDateTime at =
        QDateTime::fromMSecsSinceEpoch(expiresAtMs, now.timeZone());
    const qint64 days = now.date().daysTo(at.date());
    if (days <= 0)
        return QStringLiteral("until %1").arg(clockText(at));
    if (days == 1)
        return QStringLiteral("until tomorrow %1").arg(clockText(at));
    if (days < 7)
        return QStringLiteral("until %1 %2")
            .arg(at.toString(QStringLiteral("ddd")), clockText(at));
    if (at.date().year() == now.date().year())
        return QStringLiteral("until %1")
            .arg(at.toString(QStringLiteral("MMM d")));
    return QStringLiteral("until %1")
        .arg(at.toString(QStringLiteral("MMM d, yyyy")));
}

QString summary(const UserStatus &status)
{
    if (status.emoji.isEmpty())
        return status.text;
    if (status.text.isEmpty())
        return status.emoji;
    return status.emoji + QLatin1Char(' ') + status.text;
}

QString tooltip(const UserStatus &status, qint64 nowMs)
{
    const UserStatus shown = visible(status, nowMs);
    QStringList lines;
    QString line = summary(shown);
    if (!line.isEmpty()) {
        const QString until = untilLabel(
            shown.expiresAtMs, QDateTime::fromMSecsSinceEpoch(nowMs));
        if (!until.isEmpty())
            line += QStringLiteral(" — ") + until;
        lines << line;
    }
    if (shown.away)
        lines << QStringLiteral("Away");
    return lines.join(QLatin1Char('\n'));
}

QString saveErrorText(const QString &code, int httpStatus)
{
    if (code == QLatin1String("bad_status_emoji"))
        return QStringLiteral("Pick a single emoji for your status.");
    if (code == QLatin1String("status_text_too_long"))
        return QStringLiteral("Keep the status text to %1 characters.")
            .arg(kTextMax);
    if (code == QLatin1String("bad_status_expiry"))
        return QStringLiteral("Choose a valid time for the status to clear.");
    if (code == QLatin1String("status_expiry_in_past"))
        return QStringLiteral(
            "That clear time has already passed. Pick a later one.");
    if (code == QLatin1String("status_expiry_too_far"))
        return QStringLiteral("A status can clear at most a year from now. "
                              "Pick \"Don't clear\" to keep it.");
    if (code == QLatin1String("unauthorized") || httpStatus == 401)
        return QStringLiteral(
            "Could not authenticate. Log in to your account again.");
    if (httpStatus == 0)
        return QStringLiteral("Could not reach the server. Try again.");
    return QStringLiteral("Could not save your status (%1).")
        .arg(code.isEmpty() ? QStringLiteral("HTTP %1").arg(httpStatus)
                            : code);
}

} // namespace forkmesh::userstatus

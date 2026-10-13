#include "UserStatus.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimeZone>

#include <iostream>

using namespace forkmesh::userstatus;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

// Thursday 2026-09-17 14:05 UTC: a fixed clock, in a fixed zone, so the
// "end of today" and "end of Sunday" arithmetic is the same on every box.
QDateTime thursdayAfternoon()
{
    return QDateTime(QDate(2026, 9, 17), QTime(14, 5), QTimeZone::utc());
}

qint64 utcMs(int y, int m, int d, int h, int min, int s = 0, int ms = 0)
{
    return QDateTime(QDate(y, m, d), QTime(h, min, s, ms), QTimeZone::utc())
        .toMSecsSinceEpoch();
}

QJsonObject statusJson(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

void checkEmoji()
{
    check(cleanEmoji(QStringLiteral("🌴")) == QStringLiteral("🌴"),
          "a plain pictograph is one status emoji");
    check(cleanEmoji(QStringLiteral("  🎧 ")) == QStringLiteral("🎧"),
          "surrounding whitespace is trimmed");
    check(cleanEmoji(QStringLiteral("❤️")) == QStringLiteral("❤️"),
          "a BMP symbol with its emoji variation selector is accepted");
    check(cleanEmoji(QStringLiteral("👍🏽")) == QStringLiteral("👍🏽"),
          "a skin-tone modifier stays with its base");
    check(cleanEmoji(QStringLiteral("👩‍💻")) == QStringLiteral("👩‍💻"),
          "a zero-width-joiner sequence is one grapheme");
    check(cleanEmoji(QStringLiteral("🇩🇪")) == QStringLiteral("🇩🇪"),
          "a regional-indicator pair is a flag");
    check(cleanEmoji(QStringLiteral("1️⃣")) == QStringLiteral("1️⃣"),
          "a keycap sequence is accepted");
    check(cleanEmoji(QStringLiteral("🏴󠁧󠁢󠁳󠁣󠁴󠁿")) == QStringLiteral("🏴󠁧󠁢󠁳󠁣󠁴󠁿"),
          "a subdivision flag's tag run is accepted on the black flag");

    check(cleanEmoji(QString()).isEmpty(), "nothing is no emoji");
    check(cleanEmoji(QStringLiteral("a")).isEmpty(), "a letter is refused");
    check(cleanEmoji(QStringLiteral("🌴🌴")).isEmpty(),
          "two adjacent emoji are refused");
    check(cleanEmoji(QStringLiteral("🌴 hi")).isEmpty(),
          "an emoji followed by text is refused");
    check(cleanEmoji(QStringLiteral("🇩")).isEmpty(),
          "a lone regional indicator is refused");
    check(cleanEmoji(QStringLiteral("👩‍")).isEmpty(),
          "a dangling zero-width joiner is refused");
    check(cleanEmoji(QStringLiteral("1")).isEmpty(),
          "a digit without the keycap mark is refused");
}

void checkJson()
{
    const UserStatus full = fromJson(statusJson(
        R"({"away":true,"emoji":"🌴","text":"  On   holiday\nuntil Monday ","expiresAt":1790000000000})"));
    check(full.away, "away is read");
    check(full.emoji == QStringLiteral("🌴"), "the emoji is read");
    check(full.text == QStringLiteral("On holiday until Monday"),
          "the text is one line with whitespace runs collapsed");
    check(full.expiresAtMs == 1790000000000LL, "expiresAt is read in ms");

    const UserStatus hostile = fromJson(statusJson(
        R"({"away":"yes","emoji":"<b>x</b>","text":123,"expiresAt":-5})"));
    check(!hostile.away && hostile.emoji.isEmpty() && hostile.text.isEmpty() &&
              hostile.expiresAtMs == 0,
          "malformed fields read as their defaults");

    const UserStatus noCustom =
        fromJson(statusJson(R"({"away":false,"emoji":"","text":"","expiresAt":42})"));
    check(noCustom.expiresAtMs == 0,
          "an expiry without an emoji or text is meaningless and dropped");

    const UserStatus longText = fromJson(QJsonObject{
        {QStringLiteral("text"), QString(250, QLatin1Char('x'))}});
    check(longText.text.size() == kTextMax, "text is cut to the server limit");

    check(fromJson(toJson(full)) == full, "toJson round-trips through fromJson");
    const QJsonObject wire = toJson(full);
    check(wire.value(QStringLiteral("expiresAt")).toDouble() == 1790000000000.0,
          "toJson writes expiresAt as epoch ms");

    // A frame from an older client has no userStatus key: it must not
    // replace the directory's copy with "no status".
    UserStatus kept = full;
    check(!readFrom(statusJson(R"({"type":"presence","sender":"bob"})"), &kept),
          "a record without userStatus reports none");
    check(kept == full, "and leaves the caller's status untouched");
    check(!readFrom(statusJson(R"({"userStatus":"away"})"), &kept),
          "a non-object userStatus is not a status");
    UserStatus read;
    check(readFrom(statusJson(
                       R"({"userStatus":{"away":true,"emoji":"","text":"","expiresAt":0}})"),
                   &read) &&
              read.away && !read.hasCustom(),
          "a present userStatus is read, even when it only says away");
    check(readFrom(statusJson(R"({"userStatus":{}})"), &read) && read.isEmpty(),
          "an explicit empty status is a present, empty status");
}

void checkExpiry()
{
    UserStatus status;
    status.away = true;
    status.emoji = QStringLiteral("📅");
    status.text = QStringLiteral("In a meeting");
    status.expiresAtMs = 1000;
    check(!isExpired(status, 999), "a status before its expiry is live");
    check(isExpired(status, 1000), "a status lapses at its expiry, inclusive");
    const UserStatus lapsed = visible(status, 1000);
    check(!lapsed.hasCustom() && lapsed.expiresAtMs == 0,
          "a lapsed status shows no emoji or text");
    check(lapsed.away, "the away flag never lapses");
    check(visible(status, 999) == status, "a live status shows unchanged");
    status.expiresAtMs = 0;
    check(!isExpired(status, 1LL << 50), "expiresAt 0 never clears");
}

void checkClearAfter()
{
    const QDateTime now = thursdayAfternoon();
    const qint64 nowMs = now.toMSecsSinceEpoch();
    check(clearAfterExpiry(ClearAfter::Never, now) == 0,
          "Don't clear sends expiresAt 0");
    check(clearAfterExpiry(ClearAfter::ThirtyMinutes, now) ==
              nowMs + 30 * 60 * 1000,
          "30 minutes from now");
    check(clearAfterExpiry(ClearAfter::OneHour, now) == nowMs + 60 * 60 * 1000,
          "1 hour from now");
    check(clearAfterExpiry(ClearAfter::FourHours, now) ==
              nowMs + 4 * 60 * 60 * 1000,
          "4 hours from now");
    check(clearAfterExpiry(ClearAfter::Today, now) ==
              utcMs(2026, 9, 17, 23, 59, 59, 999),
          "Today ends at the end of the local day");
    check(clearAfterExpiry(ClearAfter::ThisWeek, now) ==
              utcMs(2026, 9, 20, 23, 59, 59, 999),
          "This week ends at the end of the local Sunday");
    const QDateTime sunday(QDate(2026, 9, 20), QTime(9, 0), QTimeZone::utc());
    check(clearAfterExpiry(ClearAfter::ThisWeek, sunday) ==
              utcMs(2026, 9, 20, 23, 59, 59, 999),
          "on a Sunday, This week is the end of today");
    const QDateTime custom(QDate(2026, 10, 1), QTime(8, 30), QTimeZone::utc());
    check(clearAfterExpiry(ClearAfter::Custom, now, custom) ==
              custom.toMSecsSinceEpoch(),
          "Custom uses the picked time");
    check(clearAfterExpiry(ClearAfter::Custom, now) == 0,
          "Custom without a picked time never clears");

    const QList<ClearAfter> choices = clearAfterChoices();
    check(choices.size() == 7 && choices.first() == ClearAfter::Never &&
              choices.last() == ClearAfter::Custom,
          "the combo lists Don't clear first and Custom last");
    check(clearAfterLabel(ClearAfter::Never) == QStringLiteral("Don't clear"),
          "Never is labelled Don't clear");
}

void checkPresetsAndLabels()
{
    const QList<Preset> list = presets();
    check(list.size() == 5, "five suggested presets");
    check(!list.isEmpty() &&
              presetLabel(list.first()) ==
                  QStringLiteral("📅 In a meeting — 1 hour"),
          "the first preset reads like the web's");
    bool allValid = true;
    for (const Preset &preset : list)
        allValid = allValid && cleanEmoji(preset.emoji) == preset.emoji;
    for (const QString &emoji : quickEmoji())
        allValid = allValid && cleanEmoji(emoji) == emoji;
    check(allValid, "every preset and quick-pick emoji passes the server rule");
    check(quickEmoji().size() == 10, "ten quick picks");

    const QDateTime now = thursdayAfternoon();
    check(untilLabel(0, now).isEmpty(), "a status that never clears has no until");
    check(untilLabel(utcMs(2026, 9, 17, 17, 30), now) ==
              QStringLiteral("until 5:30 PM"),
          "later today shows the time");
    check(untilLabel(utcMs(2026, 9, 18, 9, 0), now) ==
              QStringLiteral("until tomorrow 9:00 AM"),
          "tomorrow is named");
    check(untilLabel(utcMs(2026, 9, 21, 17, 0), now) ==
              QStringLiteral("until Mon 5:00 PM"),
          "within the week shows the weekday");
    check(untilLabel(utcMs(2026, 10, 30, 12, 0), now) ==
              QStringLiteral("until Oct 30"),
          "later this year shows the date");
    check(untilLabel(utcMs(2027, 1, 2, 12, 0), now) ==
              QStringLiteral("until Jan 2, 2027"),
          "next year shows the year");

    UserStatus status;
    check(summary(status).isEmpty(), "no custom status, no summary");
    status.emoji = QStringLiteral("🌴");
    check(summary(status) == QStringLiteral("🌴"), "an emoji alone");
    status.text = QStringLiteral("Vacationing");
    check(summary(status) == QStringLiteral("🌴 Vacationing"),
          "emoji then text");
    status.away = true;
    check(tooltip(status, 0) == QStringLiteral("🌴 Vacationing\nAway"),
          "the tooltip names the status and the away state");
    status.expiresAtMs = 1000;
    check(tooltip(status, 2000) == QStringLiteral("Away"),
          "a lapsed status leaves only Away in the tooltip");
    check(tooltip(UserStatus(), 0).isEmpty(), "nothing to say, no tooltip");
}

void checkErrors()
{
    check(saveErrorText(QStringLiteral("bad_status_emoji"), 400)
              .contains(QStringLiteral("single emoji")),
          "bad_status_emoji asks for one emoji");
    check(saveErrorText(QStringLiteral("status_text_too_long"), 400)
              .contains(QStringLiteral("100")),
          "status_text_too_long names the limit");
    check(saveErrorText(QStringLiteral("status_expiry_in_past"), 400)
              .contains(QStringLiteral("passed")),
          "status_expiry_in_past explains itself");
    check(saveErrorText(QStringLiteral("status_expiry_too_far"), 400)
              .contains(QStringLiteral("year")),
          "status_expiry_too_far explains the cap");
    check(saveErrorText(QStringLiteral("bad_status_expiry"), 400)
              .contains(QStringLiteral("valid time")),
          "bad_status_expiry asks for a valid time");
    check(saveErrorText(QStringLiteral("unauthorized"), 401)
              .contains(QStringLiteral("Log in")),
          "unauthorized asks to log in again");
    check(saveErrorText(QString(), 0).contains(QStringLiteral("reach")),
          "no reply is a network error");
    check(saveErrorText(QString(), 500).contains(QStringLiteral("HTTP 500")),
          "anything else names the status code");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    checkEmoji();
    checkJson();
    checkExpiry();
    checkClearAfter();
    checkPresetsAndLabels();
    checkErrors();
    if (failures == 0)
        std::cout << "user status tests passed\n";
    return failures == 0 ? 0 : 1;
}

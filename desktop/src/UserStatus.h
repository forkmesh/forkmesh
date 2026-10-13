#pragma once

// A member's self-set away flag and custom status (one emoji, a short line of
// text, an optional auto-clear time): the pure half.
//
// The server (app/src/account_status.py) keeps the status in the account
// record and hands it out as "userStatus" on the user directory, the profile
// lookup and POST /api/accounts/user-status; peers also carry it on their E2EE
// relay hello/presence frames. Everything here treats those copies as
// untrusted input and never trusts the server to have dropped an expired
// status. No QObject and no widgets, so the rules compile and test on their
// own (desktop/tests/test_user_status.cpp).

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::userstatus {

// USER_STATUS_TEXT_MAX and USER_STATUS_MAX_DURATION_MS in account_status.py.
inline constexpr int kTextMax = 100;
inline constexpr qint64 kMaxDurationMs = qint64(366) * 24 * 60 * 60 * 1000;

struct UserStatus {
    bool away = false;
    QString emoji;          // one emoji grapheme, or empty
    QString text;           // one line, at most kTextMax characters
    qint64 expiresAtMs = 0; // epoch ms the emoji/text clear at; 0 = never

    bool hasCustom() const { return !emoji.isEmpty() || !text.isEmpty(); }
    bool isEmpty() const { return !away && !hasCustom(); }
    bool operator==(const UserStatus &other) const
    {
        return away == other.away && emoji == other.emoji &&
               text == other.text && expiresAtMs == other.expiresAtMs;
    }
    bool operator!=(const UserStatus &other) const { return !(*this == other); }
};

// One emoji grapheme (pictograph with optional variation selector, skin tone
// and tag sequence, ZWJ joins of those, a flag pair or a keycap), or empty
// for anything else. A port of world.clean_status_emoji, so the dialog can
// refuse what the server would answer bad_status_emoji to.
QString cleanEmoji(const QString &value);
// One line with runs of whitespace collapsed, cut to kTextMax.
QString cleanText(const QString &value);

// {away, emoji, text, expiresAt} with each field cleaned. Missing or
// malformed fields read as their defaults.
UserStatus fromJson(const QJsonObject &object);
QJsonObject toJson(const UserStatus &status);
// Reads record["userStatus"]. Returns false, leaving *status untouched, when
// the record does not carry one: a relay frame from an older client says
// nothing about the sender's status and must not wipe the directory's copy.
bool readFrom(const QJsonObject &record, UserStatus *status);

// The custom part has lapsed by this clock (the away flag never expires).
bool isExpired(const UserStatus &status, qint64 nowMs);
// What to paint right now: the status with a lapsed emoji/text removed.
UserStatus visible(const UserStatus &status, qint64 nowMs);

// The "Clear after" choices of the Set a status dialog.
enum class ClearAfter {
    Never,
    ThirtyMinutes,
    OneHour,
    FourHours,
    Today,    // end of the local day
    ThisWeek, // end of the local Sunday
    Custom,
};
QString clearAfterLabel(ClearAfter choice);
QList<ClearAfter> clearAfterChoices();
// The expiresAt a choice means when picked at `now`: 0 for Never, the
// custom time (in epoch ms) for Custom.
qint64 clearAfterExpiry(ClearAfter choice, const QDateTime &now,
                        const QDateTime &custom = QDateTime());

struct Preset {
    QString emoji;
    QString text;
    ClearAfter clearAfter = ClearAfter::Never;
};
// The suggestions listed under the status fields.
QList<Preset> presets();
// "📅 In a meeting — 1 hour"
QString presetLabel(const Preset &preset);
// The quick-pick row above the free emoji field.
QStringList quickEmoji();

// "until 5:30 PM", "until tomorrow 9:00 AM", "until Mon 5:00 PM",
// "until Sep 30"; empty for a status that never clears.
QString untilLabel(qint64 expiresAtMs, const QDateTime &now);
// "🌴 Vacationing" / "🌴" / "Vacationing"; empty without a custom status.
QString summary(const UserStatus &status);
// The hover text beside a name: the summary, its clear time and "Away",
// one per line. Empty when there is nothing to say.
QString tooltip(const UserStatus &status, qint64 nowMs);

// The message a failed POST /api/accounts/user-status should show.
QString saveErrorText(const QString &code, int httpStatus);

} // namespace forkmesh::userstatus

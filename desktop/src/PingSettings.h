#pragma once

// Ping settings (issue #347): which ping kinds are on, which also email, and
// the daily quiet hours. The relay is authoritative for all of it — this node
// keeps a copy in QSettings, pushes a local change on the next signed
// heartbeat, and adopts the relay's values from every other reply, so a switch
// flipped on the website or the phone lands here too.
//
// kinds() mirrors PING_KIND_CATALOG in app/src/entry.py (a test holds the two
// together); the website and phone read that catalog from the relay instead.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>

namespace forkmesh::pings {

struct GroupInfo {
    const char *key;
    const char *label;
};

struct KindInfo {
    const char *kind;
    const char *group;
    const char *label;
    // Has an email switch on the relay (NOTIFICATION_EMAIL_DEFAULTS), and
    // what it defaults to there.
    bool email;
    bool emailDefault;
};

inline const QList<GroupInfo> &groups()
{
    static const QList<GroupInfo> list{
        {"conversations", "Conversations"},
        {"repositories", "Repositories"},
        {"organizations", "Organizations"},
        {"rewards", "Money & rewards"},
        {"devices", "Devices & apps"},
        {"admin", "Administration"},
    };
    return list;
}

inline const QList<KindInfo> &kinds()
{
    static const QList<KindInfo> list{
        {"mention", "conversations", "Mentions", true, true},
        {"subscribed", "conversations", "Activity on things you follow", true,
         true},
        {"chat_webhook", "conversations", "Chat webhook posts", false, false},
        {"channel_unread", "conversations", "Unread channel messages", true,
         true},
        {"direct_unread", "conversations", "Unread direct messages", true,
         true},
        {"calendar_invitation", "conversations", "Calendar invitations", true,
         true},
        {"calendar_reminder", "conversations", "Calendar reminders", true,
         true},
        {"calendar_rsvp", "conversations", "Calendar replies", true, true},
        {"issue_opened", "repositories", "New issues", true, true},
        {"pull_submitted", "repositories", "New pull requests", true, true},
        {"issue_assigned", "repositories", "Issue assignments", true, true},
        {"pending_inbox", "repositories", "Inbox updates", true, true},
        {"code_push", "repositories", "Code pushes", false, false},
        {"repo_activity", "repositories", "Merged pull requests", false,
         false},
        {"release_published", "repositories", "Releases", true, true},
        {"repo_shared", "repositories", "Repositories shared with you", true,
         true},
        {"mirror_request", "repositories", "Mirror requests", true, true},
        {"repository_hosted", "repositories", "Imports finished hosting",
         false, false},
        {"package_review", "repositories", "Reviews of your Market packages",
         true, true},
        {"org_succession", "organizations", "Organization succession", true,
         true},
        {"organization_task_update", "organizations", "Tasks", false, false},
        {"bounty_funded", "rewards", "Bounties funded", true, true},
        {"bounty_paid", "rewards", "Bounties paid", true, true},
        {"pending_reward", "rewards", "Pending rewards", true, true},
        {"credits_refilled", "rewards", "Credits refilled", true, true},
        {"tip_sent", "rewards", "Tips", true, true},
        {"hackathon_squad", "rewards", "Hackathon squad progress", false,
         false},
        {"hackathon_reward", "rewards", "Hackathon rewards", false, false},
        {"host_online", "devices", "Host came online", true, false},
        {"host_offline", "devices", "Host went offline", true, false},
        {"device_added", "devices", "New device signed in", false, false},
        {"timer_finished", "devices", "Timers and alarms", false, false},
        {"desktop_ping", "devices", "Pings from your desktop", false, false},
        {"plugin_activity", "devices", "App package activity", false, false},
        {"operational_alert", "admin", "Operational alerts", false, false},
        {"error_group", "admin", "New error groups", false, false},
        {"account_email_sent", "admin", "Account emails sent", false, false},
        {"security_alert", "admin", "Security alerts (intrusion detection)", false, false},
    };
    return list;
}

inline const KindInfo *kindInfo(const QString &kind)
{
    for (const KindInfo &info : kinds()) {
        if (kind == QLatin1String(info.kind))
            return &info;
    }
    return nullptr;
}

// The #general digest is an email switch with no ping kind behind it.
inline const QString kGeneralChatEmailKind = QStringLiteral("general_chat");

const QString kMutedKindsSetting = QStringLiteral("notifications/mutedKinds");
const QString kQuietHoursSetting = QStringLiteral("notifications/quietHours");
const QString kEmailEnabledSetting =
    QStringLiteral("notifications/email/enabled");
// A local change the relay has not acknowledged yet. Persisted like the
// pause's flag, so a restart cannot silently drop the unsent intent.
const QString kSettingsPushPendingSetting =
    QStringLiteral("notifications/settingsPushPending");

// Where this node keeps each email switch. The first thirteen predate the
// catalog and keep their historical keys so an upgrade reads the old choice.
inline QString emailSettingKey(const QString &kind)
{
    static const QList<QPair<QString, QString>> legacy{
        {QStringLiteral("mention"), QStringLiteral("mention")},
        {QStringLiteral("subscribed"), QStringLiteral("subscribed")},
        {QStringLiteral("pull_submitted"), QStringLiteral("pullSubmitted")},
        {QStringLiteral("issue_assigned"), QStringLiteral("issueAssigned")},
        {QStringLiteral("repo_shared"), QStringLiteral("repoShared")},
        {QStringLiteral("bounty_funded"), QStringLiteral("bountyFunded")},
        {QStringLiteral("bounty_paid"), QStringLiteral("bountyPaid")},
        {QStringLiteral("release_published"),
         QStringLiteral("releasePublished")},
        {QStringLiteral("pending_inbox"), QStringLiteral("pendingInbox")},
        {QStringLiteral("credits_refilled"), QStringLiteral("creditsRefilled")},
        {QStringLiteral("general_chat"), QStringLiteral("generalChat")},
        {QStringLiteral("host_online"), QStringLiteral("hostOnline")},
        {QStringLiteral("host_offline"), QStringLiteral("hostOffline")},
    };
    for (const auto &pair : legacy) {
        if (pair.first == kind)
            return QStringLiteral("notifications/email/") + pair.second;
    }
    return QStringLiteral("notifications/email/kind/") + kind;
}

inline bool emailDefault(const QString &kind)
{
    if (kind == kGeneralChatEmailKind)
        return true;
    const KindInfo *info = kindInfo(kind);
    return info && info->emailDefault;
}

// Every email switch the relay knows, in catalog order plus the digest.
inline QStringList emailKinds()
{
    QStringList list;
    for (const KindInfo &info : kinds()) {
        if (info.email)
            list << QString::fromLatin1(info.kind);
    }
    list << kGeneralChatEmailKind;
    return list;
}

inline bool emailKindEnabled(const QString &kind)
{
    return QSettings().value(emailSettingKey(kind), emailDefault(kind)).toBool();
}

inline bool emailEnabled()
{
    return QSettings().value(kEmailEnabledSetting, true).toBool();
}

inline QStringList mutedKinds()
{
    QStringList muted =
        QSettings().value(kMutedKindsSetting).toStringList();
    muted.removeIf([](const QString &kind) { return !kindInfo(kind); });
    muted.removeDuplicates();
    std::sort(muted.begin(), muted.end());
    return muted;
}

inline bool kindEnabled(const QString &kind)
{
    return !mutedKinds().contains(kind);
}

inline void storeMutedKinds(QStringList muted)
{
    muted.removeIf([](const QString &kind) { return !kindInfo(kind); });
    muted.removeDuplicates();
    std::sort(muted.begin(), muted.end());
    if (muted.isEmpty())
        QSettings().remove(kMutedKindsSetting);
    else
        QSettings().setValue(kMutedKindsSetting, muted);
}

inline void setKindEnabled(const QString &kind, bool enabled)
{
    QStringList muted = mutedKinds();
    muted.removeAll(kind);
    if (!enabled)
        muted << kind;
    storeMutedKinds(muted);
}

// Pings this desktop raises by itself (an agent finishing, a mirror, a
// kind-less "System" ping) never reach the relay, so a Pings row's gear turning one
// off is remembered here instead of in the synced list above.
const QString kMutedDesktopKindsSetting =
    QStringLiteral("notifications/mutedDesktopKinds");

// A ping's kind as the switches see it: a kind-less ping files as "desktop".
inline QString pingKindKey(const QString &kind)
{
    const QString key = kind.trimmed();
    return key.isEmpty() ? QStringLiteral("desktop") : key;
}

inline QStringList mutedDesktopKinds()
{
    QStringList muted =
        QSettings().value(kMutedDesktopKindsSetting).toStringList();
    muted.removeIf([](const QString &kind) {
        return kind.trimmed().isEmpty() || kindInfo(kind);
    });
    muted.removeDuplicates();
    std::sort(muted.begin(), muted.end());
    return muted;
}

inline void setDesktopKindEnabled(const QString &kind, bool enabled)
{
    QStringList muted = mutedDesktopKinds();
    muted.removeAll(kind);
    if (!enabled && !kind.trimmed().isEmpty() && !kindInfo(kind))
        muted << kind;
    std::sort(muted.begin(), muted.end());
    if (muted.isEmpty())
        QSettings().remove(kMutedDesktopKindsSetting);
    else
        QSettings().setValue(kMutedDesktopKindsSetting, muted);
}

// Whether a ping of this kind may alert here: a relay kind follows its
// synced switch, anything else this desktop's own list.
inline bool pingKindEnabled(const QString &kind)
{
    const QString key = pingKindKey(kind);
    return kindInfo(key) ? kindEnabled(key)
                         : !mutedDesktopKinds().contains(key);
}

// Flips whichever switch owns the kind. True when it was a relay kind, whose
// change still has to be pushed to the relay.
inline bool setPingKindEnabled(const QString &kind, bool enabled)
{
    const QString key = pingKindKey(kind);
    if (kindInfo(key)) {
        setKindEnabled(key, enabled);
        return true;
    }
    setDesktopKindEnabled(key, enabled);
    return false;
}

// The daily quiet window, in the same shape as the relay's
// notification_quiet_hours: minutes after local midnight, an end at or before
// the start runs across midnight, and days are the weekdays a window may START
// on with Monday = 0.
struct QuietHours {
    bool enabled = false;
    int start = 22 * 60;
    int end = 7 * 60;
    QList<int> days{0, 1, 2, 3, 4, 5, 6};
    QString timezone;

    bool operator==(const QuietHours &other) const
    {
        return enabled == other.enabled && start == other.start &&
               end == other.end && days == other.days &&
               timezone == other.timezone;
    }
};

inline int cleanQuietMinute(const QJsonValue &value, int fallback)
{
    if (!value.isDouble())
        return fallback;
    const int minute = value.toInt(-1);
    return minute >= 0 && minute < 24 * 60 ? minute : fallback;
}

inline QuietHours quietHoursFromJson(const QJsonObject &object)
{
    QuietHours quiet;
    quiet.start = cleanQuietMinute(object.value(QStringLiteral("start")),
                                   quiet.start);
    quiet.end = cleanQuietMinute(object.value(QStringLiteral("end")), quiet.end);
    if (object.value(QStringLiteral("days")).isArray()) {
        QList<int> days;
        for (const QJsonValue &value :
             object.value(QStringLiteral("days")).toArray()) {
            const int day = value.toInt(-1);
            if (day >= 0 && day <= 6 && !days.contains(day))
                days << day;
        }
        std::sort(days.begin(), days.end());
        quiet.days = days;
    }
    quiet.enabled = object.value(QStringLiteral("enabled")).toBool() &&
                    !quiet.days.isEmpty() && quiet.start != quiet.end;
    quiet.timezone =
        object.value(QStringLiteral("timezone")).toString().trimmed();
    return quiet;
}

inline QJsonObject quietHoursToJson(const QuietHours &quiet)
{
    QJsonArray days;
    for (int day : quiet.days)
        days.append(day);
    QJsonObject object{{QStringLiteral("enabled"), quiet.enabled},
                       {QStringLiteral("start"), quiet.start},
                       {QStringLiteral("end"), quiet.end},
                       {QStringLiteral("days"), days}};
    if (!quiet.timezone.isEmpty())
        object.insert(QStringLiteral("timezone"), quiet.timezone);
    return object;
}

inline QuietHours loadQuietHours()
{
    return quietHoursFromJson(
        QJsonDocument::fromJson(
            QSettings().value(kQuietHoursSetting).toByteArray())
            .object());
}

inline void storeQuietHours(const QuietHours &quiet)
{
    QSettings().setValue(
        kQuietHoursSetting,
        QJsonDocument(quietHoursToJson(quiet)).toJson(QJsonDocument::Compact));
}

inline QTimeZone quietZone(const QuietHours &quiet)
{
    if (!quiet.timezone.isEmpty()) {
        const QTimeZone zone(quiet.timezone.toUtf8());
        if (zone.isValid())
            return zone;
    }
    return QTimeZone::systemTimeZone();
}

// End (ms epoch) of the quiet window nowMs falls in, else 0. The same wall
// clock arithmetic as ping_quiet_until on the relay, so both agree across a
// DST change.
inline qint64 quietUntilMs(const QuietHours &quiet, qint64 nowMs)
{
    if (!quiet.enabled || quiet.days.isEmpty() || quiet.start == quiet.end)
        return 0;
    const QTimeZone zone = quietZone(quiet);
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(nowMs, zone);
    const int minute = local.time().hour() * 60 + local.time().minute();
    const int weekday = local.date().dayOfWeek() - 1; // Monday = 0
    const QDate today = local.date();
    QDate endDate;
    if (quiet.start < quiet.end) {
        if (!quiet.days.contains(weekday) || minute < quiet.start ||
            minute >= quiet.end)
            return 0;
        endDate = today;
    } else if (minute >= quiet.start && quiet.days.contains(weekday)) {
        endDate = today.addDays(1);
    } else if (minute < quiet.end && quiet.days.contains((weekday + 6) % 7)) {
        endDate = today;
    } else {
        return 0;
    }
    const QDateTime finish(endDate, QTime(quiet.end / 60, quiet.end % 60),
                           zone);
    const qint64 deadline = finish.toMSecsSinceEpoch();
    return deadline > nowMs ? deadline : 0;
}

inline qint64 quietUntilMs()
{
    return quietUntilMs(loadQuietHours(), QDateTime::currentMSecsSinceEpoch());
}

inline QString minuteLabel(int minute)
{
    return QTime(minute / 60, minute % 60).toString(QStringLiteral("HH:mm"));
}

// "Quiet hours 22:00-07:00, every day" or "..., Mon Tue Wed".
inline QString quietHoursSummary(const QuietHours &quiet)
{
    if (!quiet.enabled)
        return QStringLiteral("Quiet hours are off");
    static const char *names[] = {"Mon", "Tue", "Wed", "Thu",
                                  "Fri", "Sat", "Sun"};
    QString when;
    if (quiet.days.size() == 7) {
        when = QStringLiteral("every day");
    } else {
        QStringList picked;
        for (int day : quiet.days)
            picked << QString::fromLatin1(names[day]);
        when = picked.join(QLatin1Char(' '));
    }
    return QStringLiteral("Quiet hours %1%2%3, %4")
        .arg(minuteLabel(quiet.start), QString::fromUtf8("\xE2\x80\x93"),
             minuteLabel(quiet.end), when);
}

// Everything a push carries, so an acknowledgement can tell whether the reply
// answered the settings that are current now.
inline QJsonObject localSnapshot(const QString &delivery)
{
    QJsonObject prefs;
    for (const QString &kind : emailKinds())
        prefs.insert(kind, emailKindEnabled(kind));
    QJsonArray muted;
    for (const QString &kind : mutedKinds())
        muted.append(kind);
    return QJsonObject{
        {QStringLiteral("emailNotifications"), emailEnabled()},
        {QStringLiteral("notificationPreferences"), prefs},
        {QStringLiteral("notificationDelivery"), delivery},
        {QStringLiteral("pingMutedKinds"), muted},
        {QStringLiteral("notificationQuietHours"),
         quietHoursToJson(loadQuietHours())},
    };
}

// Adopt the relay's copy (a heartbeat reply). Returns true when anything
// changed, so the caller can repaint the settings page.
inline bool adoptRelaySettings(const QJsonObject &reply,
                               const QString &deliverySettingKey)
{
    QSettings settings;
    bool changed = false;
    if (reply.value(QStringLiteral("emailNotifications")).isBool()) {
        const bool on = reply.value(QStringLiteral("emailNotifications")).toBool();
        if (emailEnabled() != on) {
            settings.setValue(kEmailEnabledSetting, on);
            changed = true;
        }
    }
    const QJsonObject prefs =
        reply.value(QStringLiteral("notificationPreferences")).toObject();
    for (auto it = prefs.begin(); it != prefs.end(); ++it) {
        if (!it.value().isBool() || !emailKinds().contains(it.key()))
            continue;
        if (emailKindEnabled(it.key()) != it.value().toBool()) {
            settings.setValue(emailSettingKey(it.key()), it.value().toBool());
            changed = true;
        }
    }
    const QString delivery =
        reply.value(QStringLiteral("notificationDelivery")).toString();
    if ((delivery == QLatin1String("digest") ||
         delivery == QLatin1String("instant")) &&
        settings.value(deliverySettingKey, QStringLiteral("digest")).toString() !=
            delivery) {
        settings.setValue(deliverySettingKey, delivery);
        changed = true;
    }
    if (reply.value(QStringLiteral("pingMutedKinds")).isArray()) {
        QStringList muted;
        for (const QJsonValue &value :
             reply.value(QStringLiteral("pingMutedKinds")).toArray())
            muted << value.toString();
        const QStringList before = mutedKinds();
        storeMutedKinds(muted);
        changed = changed || before != mutedKinds();
    }
    if (reply.value(QStringLiteral("notificationQuietHours")).isObject()) {
        const QuietHours quiet = quietHoursFromJson(
            reply.value(QStringLiteral("notificationQuietHours")).toObject());
        if (!(quiet == loadQuietHours())) {
            storeQuietHours(quiet);
            changed = true;
        }
    }
    return changed;
}

} // namespace forkmesh::pings

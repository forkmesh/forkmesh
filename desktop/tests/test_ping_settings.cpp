// Ping settings (#347), offscreen: the kind catalog, the quiet-hours window
// (the same wall-clock rule as ping_quiet_until on the relay), adopting the
// relay's copy and the snapshot a push carries, and the Settings > Pings panel
// writing, repainting and highlighting its switches.
#include "PingSettings.h"
#include "PingSettingsPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimeEdit>
#include <QWidget>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    std::fprintf(condition ? stdout : stderr, "%s: %s\n",
                 condition ? "PASS" : "FAIL", message);
    if (!condition)
        ++failures;
}

qint64 utcMs(int year, int month, int day, int hour, int minute)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute),
                     QTimeZone::UTC)
        .toMSecsSinceEpoch();
}

QCheckBox *findBox(QWidget *root, const QString &objectName,
                   const QString &kind)
{
    for (QCheckBox *box : root->findChildren<QCheckBox *>(objectName)) {
        if (box->property("pingKind").toString() == kind)
            return box;
    }
    return nullptr;
}

void checkCatalog()
{
    QSet<QString> seen;
    bool unique = true;
    bool grouped = true;
    QSet<QString> groupKeys;
    for (const auto &group : forkmesh::pings::groups())
        groupKeys.insert(QString::fromLatin1(group.key));
    for (const auto &info : forkmesh::pings::kinds()) {
        const QString kind = QString::fromLatin1(info.kind);
        unique = unique && !seen.contains(kind);
        seen.insert(kind);
        grouped = grouped && groupKeys.contains(QString::fromLatin1(info.group));
    }
    check(unique, "every ping kind appears once in the catalog");
    check(grouped, "every ping kind names a known group");
    check(seen.contains(QStringLiteral("timer_finished")) &&
              seen.contains(QStringLiteral("repo_activity")) &&
              seen.contains(QStringLiteral("error_group")),
          "the catalog covers desktop, repository and admin kinds");
    check(forkmesh::pings::emailSettingKey(QStringLiteral("pull_submitted")) ==
              QLatin1String("notifications/email/pullSubmitted"),
          "a pre-catalog email switch keeps its historical settings key");
    check(forkmesh::pings::emailSettingKey(QStringLiteral("tip_sent")) ==
              QLatin1String("notifications/email/kind/tip_sent"),
          "a newer email switch gets a per-kind key");
    check(!forkmesh::pings::emailDefault(QStringLiteral("host_online")) &&
              forkmesh::pings::emailDefault(QStringLiteral("mention")) &&
              forkmesh::pings::emailDefault(QStringLiteral("general_chat")),
          "email defaults match the relay's NOTIFICATION_EMAIL_DEFAULTS");
    check(forkmesh::pings::emailKinds().last() == QLatin1String("general_chat") &&
              !forkmesh::pings::emailKinds().contains(
                  QStringLiteral("timer_finished")),
          "the email list is the emailable kinds plus the #general digest");
}

void checkQuietHours()
{
    forkmesh::pings::QuietHours quiet;
    quiet.enabled = true;
    quiet.start = 22 * 60;
    quiet.end = 7 * 60;
    quiet.timezone = QStringLiteral("Europe/Paris");
    // 2026-09-22 is a Tuesday. 12:00 UTC is 14:00 in Paris.
    check(forkmesh::pings::quietUntilMs(quiet, utcMs(2026, 9, 22, 12, 0)) == 0,
          "an afternoon is outside a 22:00-07:00 window");
    const qint64 expected = utcMs(2026, 9, 23, 5, 0); // 07:00 Paris
    check(forkmesh::pings::quietUntilMs(quiet, utcMs(2026, 9, 22, 21, 30)) ==
              expected,
          "23:30 Paris is quiet until 07:00 the next morning");
    check(forkmesh::pings::quietUntilMs(quiet, utcMs(2026, 9, 23, 4, 0)) ==
              expected,
          "06:00 Paris is still inside the window that started last night");

    quiet.days = {0}; // Monday nights only
    check(forkmesh::pings::quietUntilMs(quiet, utcMs(2026, 9, 22, 1, 0)) ==
              utcMs(2026, 9, 22, 5, 0),
          "Tuesday 03:00 belongs to the window Monday night started");
    check(forkmesh::pings::quietUntilMs(quiet, utcMs(2026, 9, 22, 21, 30)) == 0,
          "Tuesday night is not quiet when only Monday is picked");

    forkmesh::pings::QuietHours daytime;
    daytime.enabled = true;
    daytime.start = 9 * 60;
    daytime.end = 17 * 60;
    daytime.timezone = QStringLiteral("UTC");
    daytime.days = {1};
    check(forkmesh::pings::quietUntilMs(daytime, utcMs(2026, 9, 22, 12, 0)) ==
              utcMs(2026, 9, 22, 17, 0),
          "a same-day window ends at its end time");

    const QJsonObject json = forkmesh::pings::quietHoursToJson(quiet);
    check(forkmesh::pings::quietHoursFromJson(json) == quiet,
          "quiet hours survive a JSON round trip");
    QJsonObject broken = json;
    broken.insert(QStringLiteral("start"), 5000);
    broken.insert(QStringLiteral("days"), QJsonArray{9, 1, 1});
    const auto cleaned = forkmesh::pings::quietHoursFromJson(broken);
    check(cleaned.start == 22 * 60 && cleaned.days == QList<int>{1},
          "an out-of-range minute and bad days are cleaned like the relay does");
    check(forkmesh::pings::quietHoursSummary(quiet).contains(
              QStringLiteral("22:00")) &&
              forkmesh::pings::quietHoursSummary(quiet).contains(
                  QStringLiteral("Mon")),
          "the summary names the window and its days");
}

void checkRelaySync()
{
    QSettings().clear();
    forkmesh::pings::setKindEnabled(QStringLiteral("mention"), false);
    forkmesh::pings::setKindEnabled(QStringLiteral("bogus"), false);
    check(forkmesh::pings::mutedKinds() == QStringList{QStringLiteral("mention")},
          "switching a kind off stores it; unknown kinds are dropped");

    const QJsonObject reply{
        {QStringLiteral("emailNotifications"), false},
        {QStringLiteral("notificationPreferences"),
         QJsonObject{{QStringLiteral("tip_sent"), false},
                     {QStringLiteral("general_chat"), false}}},
        {QStringLiteral("notificationDelivery"), QStringLiteral("instant")},
        {QStringLiteral("pingMutedKinds"),
         QJsonArray{QStringLiteral("host_online")}},
        {QStringLiteral("notificationQuietHours"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("start"), 60},
                     {QStringLiteral("end"), 120},
                     {QStringLiteral("days"), QJsonArray{0, 1}},
                     {QStringLiteral("timezone"), QStringLiteral("UTC")}}},
    };
    const QString deliveryKey = QStringLiteral("notifications/email/delivery");
    check(forkmesh::pings::adoptRelaySettings(reply, deliveryKey),
          "a relay reply that differs is adopted");
    check(!forkmesh::pings::emailEnabled() &&
              !forkmesh::pings::emailKindEnabled(QStringLiteral("tip_sent")) &&
              !forkmesh::pings::emailKindEnabled(QStringLiteral("general_chat")) &&
              QSettings().value(deliveryKey).toString() ==
                  QLatin1String("instant") &&
              forkmesh::pings::mutedKinds() ==
                  QStringList{QStringLiteral("host_online")} &&
              forkmesh::pings::loadQuietHours().start == 60,
          "every switch the reply carries lands in this node's copy");
    check(!forkmesh::pings::adoptRelaySettings(reply, deliveryKey),
          "adopting the same reply again changes nothing");

    const QJsonObject snapshot =
        forkmesh::pings::localSnapshot(QStringLiteral("instant"));
    check(snapshot.value(QStringLiteral("pingMutedKinds")).toArray() ==
                  QJsonArray{QStringLiteral("host_online")} &&
              snapshot.value(QStringLiteral("emailNotifications")) ==
                  QJsonValue(false) &&
              snapshot.value(QStringLiteral("notificationPreferences"))
                      .toObject()
                      .value(QStringLiteral("tip_sent")) == QJsonValue(false) &&
              snapshot.value(QStringLiteral("notificationQuietHours"))
                      .toObject()
                      .value(QStringLiteral("start")) == QJsonValue(60),
          "the push snapshot carries every account-wide switch");
}

void checkPanel()
{
    QSettings().clear();
    int changes = 0;
    QWidget host;
    auto *panel = new PingSettingsPanel(&host);
    panel->changed = [&changes] { ++changes; };
    auto *combo = new QComboBox(&host);
    combo->addItem(QStringLiteral("digest"), QStringLiteral("digest"));
    combo->addItem(QStringLiteral("instant"), QStringLiteral("instant"));
    panel->attachDeliveryCombo(combo,
                               QStringLiteral("notifications/email/delivery"));
    QWidget *kinds = panel->kindsWidget();
    QWidget *quiet = panel->quietHoursWidget();

    QCheckBox *mentionPing = findBox(kinds, QStringLiteral("pingKindCheck"),
                                     QStringLiteral("mention"));
    QCheckBox *mentionEmail = findBox(kinds, QStringLiteral("pingEmailCheck"),
                                      QStringLiteral("mention"));
    check(mentionPing && mentionPing->isChecked() && mentionEmail &&
              mentionEmail->isChecked() && mentionEmail->isEnabled(),
          "an untouched install shows mentions on, with email");
    check(!findBox(kinds, QStringLiteral("pingEmailCheck"),
                   QStringLiteral("timer_finished")),
          "a kind the relay never emails has no email switch");
    int kindRows = kinds->findChildren<QFrame *>(QStringLiteral("pingKindRow")).size();
    check(kindRows == int(forkmesh::pings::kinds().size()),
          "every catalog kind has a row on the page");

    if (mentionPing)
        mentionPing->click();
    check(forkmesh::pings::mutedKinds() ==
                  QStringList{QStringLiteral("mention")} &&
              changes == 1,
          "switching a ping type off stores it and asks for a push");
    check(mentionEmail && !mentionEmail->isEnabled(),
          "a ping type that is off cannot be emailed");

    QCheckBox *master = panel->emailMasterCheck();
    if (master)
        master->click();
    QCheckBox *tipEmail = findBox(kinds, QStringLiteral("pingEmailCheck"),
                                  QStringLiteral("tip_sent"));
    check(!forkmesh::pings::emailEnabled() && tipEmail && !tipEmail->isEnabled(),
          "the master email switch disables every email switch");

    auto *quietCheck =
        quiet->findChild<QCheckBox *>(QStringLiteral("pingQuietHoursCheck"));
    if (quietCheck)
        quietCheck->click();
    const auto stored = forkmesh::pings::loadQuietHours();
    check(stored.enabled && stored.start == 22 * 60 && stored.end == 7 * 60 &&
              stored.days.size() == 7 && !stored.timezone.isEmpty(),
          "turning quiet hours on stores the default window in this zone");

    // The relay's copy wins once nothing of ours is pending.
    forkmesh::pings::adoptRelaySettings(
        QJsonObject{{QStringLiteral("pingMutedKinds"), QJsonArray{}},
                    {QStringLiteral("emailNotifications"), true},
                    {QStringLiteral("notificationDelivery"),
                     QStringLiteral("instant")}},
        QStringLiteral("notifications/email/delivery"));
    const int before = changes;
    panel->refresh();
    check(mentionPing && mentionPing->isChecked() && mentionEmail &&
              mentionEmail->isEnabled() && combo->currentIndex() == 1 &&
              changes == before,
          "refresh repaints from the adopted copy without echoing a push");

    check(!panel->revealKind(QStringLiteral("error_group")),
          "an admin-only kind is not revealed to a member");
    panel->setAdmin(true);
    QWidget *row = panel->revealKind(QStringLiteral("error_group"));
    check(row && !row->styleSheet().isEmpty(),
          "an administrator's admin row lights up when revealed");
    QWidget *mentionRow = panel->revealKind(QStringLiteral("mention"));
    check(mentionRow && mentionRow->property("pingKind").toString() ==
                            QLatin1String("mention"),
          "revealing a kind hands back its own row to scroll to");
    check(!panel->revealKind(QStringLiteral("not_a_kind")),
          "an unknown kind reveals nothing");
}

} // namespace

// A Pings row's gear (#390): relay kinds flip their synced switch, anything else a
// desktop-only list, and the panel lists those so each can come back on.
void checkPingKinds()
{
    using namespace forkmesh::pings;
    QSettings().remove(kMutedKindsSetting);
    QSettings().remove(kMutedDesktopKindsSetting);
    check(pingKindKey(QString()) == QLatin1String("desktop") &&
              pingKindKey(QStringLiteral(" agent_done ")) ==
                  QLatin1String("agent_done"),
          "a kind-less ping is keyed as a desktop ping");
    check(pingKindEnabled(QStringLiteral("mention")) &&
              pingKindEnabled(QStringLiteral("agent_done")),
          "every kind alerts until turned off");

    check(setPingKindEnabled(QStringLiteral("mention"), false) &&
              !kindEnabled(QStringLiteral("mention")) &&
              mutedDesktopKinds().isEmpty(),
          "turning off a relay kind flips its synced switch");
    check(!setPingKindEnabled(QStringLiteral("agent_done"), false) &&
              mutedDesktopKinds() == QStringList{QStringLiteral("agent_done")} &&
              !mutedKinds().contains(QStringLiteral("agent_done")),
          "turning off a desktop kind stays on this desktop");
    setPingKindEnabled(QString(), false);
    check(!pingKindEnabled(QString()) &&
              !pingKindEnabled(QStringLiteral("desktop")) &&
              !pingKindEnabled(QStringLiteral("mention")) &&
              !pingKindEnabled(QStringLiteral("agent_done")) &&
              pingKindEnabled(QStringLiteral("mirror")),
          "only the kinds turned off stop alerting");
    QSettings().setValue(kMutedDesktopKindsSetting,
                         QStringList{QStringLiteral("tip_sent"),
                                     QStringLiteral("agent_done")});
    check(mutedDesktopKinds() == QStringList{QStringLiteral("agent_done")},
          "a relay kind never hides in the desktop-only list");
    setDesktopKindEnabled(QStringLiteral("desktop"), false);

    PingSettingsPanel panel;
    panel.desktopKindLabel = [](const QString &kind) {
        return kind == QLatin1String("agent_done") ? QStringLiteral("Agent done")
                                                   : kind;
    };
    panel.refresh();
    QWidget *root = panel.kindsWidget();
    QCheckBox *agent =
        findBox(root, QStringLiteral("pingDesktopKindCheck"),
                QStringLiteral("agent_done"));
    QWidget *box = root->findChild<QWidget *>(QStringLiteral("pingDesktopKinds"));
    check(agent && !agent->isChecked() && box && !box->isHidden(),
          "a desktop kind turned off from its Pings row is listed, switched off");
    bool labelled = false;
    if (agent) {
        for (QLabel *label : agent->parentWidget()->parentWidget()
                                 ->findChildren<QLabel *>()) {
            labelled = labelled || label->text() == QLatin1String("Agent done");
        }
    }
    check(labelled, "the listed kind wears its Pings name");
    check(!findBox(root, QStringLiteral("pingDesktopKindCheck"),
                   QStringLiteral("mention")),
          "relay kinds keep their own row, not a desktop one");
    if (agent)
        agent->setChecked(true);
    check(pingKindEnabled(QStringLiteral("agent_done")) &&
              mutedDesktopKinds() == QStringList{QStringLiteral("desktop")},
          "switching a listed kind back on alerts again");
    panel.refresh();
    check(findBox(root, QStringLiteral("pingDesktopKindCheck"),
                  QStringLiteral("agent_done")) == agent,
          "a kind switched back on keeps its row for the session");
    check(panel.revealKind(QStringLiteral("desktop")) != nullptr,
          "a Pings row's gear can land on a desktop kind's row");

    QSettings().remove(kMutedKindsSetting);
    QSettings().remove(kMutedDesktopKindsSetting);
}

void checkAlertLead()
{
    using namespace forkmesh::pings;
    check(alertLeadTitle(QStringLiteral("bob"), QStringLiteral("acme/widgets"), 42) ==
              QString::fromUtf8("bob \xC2\xB7 acme/widgets #42"),
          "an alert title leads with who acted, then where");
    check(alertLeadTitle(QStringLiteral("bob"), QStringLiteral("acme/widgets")) ==
              QString::fromUtf8("bob \xC2\xB7 acme/widgets"),
          "an alert title without a number names the repository only");
    check(alertLeadTitle(QString(), QStringLiteral("acme/widgets"), 7) ==
              QStringLiteral("acme/widgets #7"),
          "an alert title without an actor keeps the place");
    check(alertLeadTitle(QStringLiteral(" carol "), QString()) == QStringLiteral("carol"),
          "an alert title without a place is the actor alone");
    check(alertSnippet(QStringLiteral("  fix \n the   login  ")) ==
              QStringLiteral("fix the login"),
          "an alert snippet collapses whitespace");
    const QString longText(400, QLatin1Char('x'));
    const QString cut = alertSnippet(longText);
    check(cut.size() == 180 && cut.endsWith(QString::fromUtf8("\xE2\x80\xA6")),
          "an alert snippet is bounded and ends with an ellipsis");
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("forkmesh-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("ping-settings"));

    checkCatalog();
    checkQuietHours();
    checkRelaySync();
    checkPanel();
    checkPingKinds();
    checkAlertLead();

    std::fprintf(failures ? stderr : stdout, "%s: %d failure(s)\n",
                 failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}

#include "PingSettingsPanel.h"
#include "Theme.h"

#include "PingSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kSwitchColumnWidth = 58;
constexpr int kRevealHighlightMs = 2600;

QLabel *columnCaption(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("statusLine"));
    label->setAlignment(Qt::AlignCenter);
    label->setFixedWidth(kSwitchColumnWidth);
    return label;
}

QCheckBox *switchBox(const QString &accessibleName)
{
    // Text-less, centred in its column: the column caption names it and the
    // accessible name says which row it belongs to.
    auto *box = new QCheckBox;
    box->setAccessibleName(accessibleName);
    box->setCursor(Qt::PointingHandCursor);
    return box;
}

QWidget *centred(QWidget *child)
{
    auto *cell = new QWidget;
    cell->setFixedWidth(kSwitchColumnWidth);
    auto *layout = new QHBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addStretch();
    layout->addWidget(child);
    layout->addStretch();
    return cell;
}

} // namespace

PingSettingsPanel::PingSettingsPanel(QObject *parent) : QObject(parent)
{
    buildQuietHours();
    buildKinds();
    refresh();
}

void PingSettingsPanel::buildQuietHours()
{
    m_quietBox = new QWidget;
    auto *column = new QVBoxLayout(m_quietBox);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    m_quietEnabled = new QCheckBox(QStringLiteral("Quiet hours from"));
    m_quietEnabled->setObjectName(QStringLiteral("pingQuietHoursCheck"));
    m_quietEnabled->setToolTip(QStringLiteral(
        "Every day in this window pings still arrive on the Pings page, but "
        "no email, phone banner, desktop alert or sound is raised. It follows "
        "this computer's time zone."));
    m_quietStart = new QTimeEdit;
    m_quietEnd = new QTimeEdit;
    for (QTimeEdit *edit : {m_quietStart.data(), m_quietEnd.data()}) {
        edit->setDisplayFormat(QStringLiteral("HH:mm"));
        edit->setWrapping(true);
    }
    m_quietStart->setAccessibleName(QStringLiteral("Quiet hours start"));
    m_quietEnd->setAccessibleName(QStringLiteral("Quiet hours end"));
    row->addWidget(m_quietEnabled);
    row->addWidget(m_quietStart);
    row->addWidget(new QLabel(QStringLiteral("to")));
    row->addWidget(m_quietEnd);
    row->addStretch();
    column->addLayout(row);

    auto *days = new QHBoxLayout;
    days->setContentsMargins(22, 0, 0, 0);
    days->setSpacing(8);
    static const char *names[] = {"Mon", "Tue", "Wed", "Thu",
                                  "Fri", "Sat", "Sun"};
    for (int day = 0; day < 7; ++day) {
        auto *box = new QCheckBox(QString::fromLatin1(names[day]));
        box->setToolTip(QStringLiteral(
            "Start a quiet window on this evening (a window that runs past "
            "midnight belongs to the day it starts on)."));
        m_quietDays << box;
        days->addWidget(box);
    }
    days->addStretch();
    column->addLayout(days);

    m_quietStatus = new QLabel;
    m_quietStatus->setObjectName(QStringLiteral("statusLine"));
    m_quietStatus->setWordWrap(true);
    column->addWidget(m_quietStatus);

    connect(m_quietEnabled, &QCheckBox::toggled, this,
            [this] { saveQuietHours(); });
    connect(m_quietStart, &QTimeEdit::timeChanged, this,
            [this] { saveQuietHours(); });
    connect(m_quietEnd, &QTimeEdit::timeChanged, this,
            [this] { saveQuietHours(); });
    for (const QPointer<QCheckBox> &box : std::as_const(m_quietDays))
        connect(box, &QCheckBox::toggled, this, [this] { saveQuietHours(); });

    // The status names when the current window lifts; re-read it each minute
    // while the page exists rather than trusting a label drawn at 06:59.
    auto *tick = new QTimer(m_quietBox);
    tick->setInterval(60 * 1000);
    connect(tick, &QTimer::timeout, this, [this] { updateQuietStatus(); });
    tick->start();
}

void PingSettingsPanel::buildKinds()
{
    m_kindsBox = new QWidget;
    auto *column = new QVBoxLayout(m_kindsBox);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    auto *intro = new QLabel(QStringLiteral(
        "A ping type that is off is never delivered anywhere — not to this "
        "page, email, your phone or this desktop."));
    intro->setObjectName(QStringLiteral("statusLine"));
    intro->setWordWrap(true);
    column->addWidget(intro);

    m_emailMaster = new QCheckBox(QStringLiteral("Email me about pings"));
    m_emailMaster->setObjectName(QStringLiteral("pingEmailMasterCheck"));
    m_emailMaster->setToolTip(QStringLiteral(
        "Master switch for ping email. Off stops every email below without "
        "forgetting which ones you picked."));
    connect(m_emailMaster, &QCheckBox::toggled, this, [this](bool on) {
        if (m_refreshing)
            return;
        QSettings().setValue(forkmesh::pings::kEmailEnabledSetting, on);
        updateEmailEnablement();
        notifyChanged();
    });

    column->addWidget(m_emailMaster);

    auto *captions = new QHBoxLayout;
    captions->setContentsMargins(6, 6, 6, 0);
    captions->setSpacing(0);
    captions->addStretch();
    captions->addWidget(columnCaption(QStringLiteral("Ping")));
    captions->addWidget(columnCaption(QStringLiteral("Email")));
    column->addLayout(captions);

    for (const forkmesh::pings::GroupInfo &group : forkmesh::pings::groups()) {
        const bool adminGroup = QLatin1String(group.key) == QLatin1String("admin");
        auto *heading = new QLabel(QString::fromUtf8(group.label));
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        heading->setContentsMargins(6, 8, 0, 2);
        column->addWidget(heading);
        if (adminGroup)
            m_adminOnly << heading;
        for (const forkmesh::pings::KindInfo &info : forkmesh::pings::kinds()) {
            if (QLatin1String(info.group) != QLatin1String(group.key))
                continue;
            const QString kind = QString::fromLatin1(info.kind);
            const QString label = QString::fromUtf8(info.label);
            auto *row = new QFrame;
            row->setObjectName(QStringLiteral("pingKindRow"));
            row->setProperty("pingKind", kind);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(6, 2, 6, 2);
            rowLayout->setSpacing(0);
            auto *name = new QLabel(label);
            name->setToolTip(kind);
            rowLayout->addWidget(name, 1);
            auto *ping = switchBox(label + QStringLiteral(" pings"));
            ping->setObjectName(QStringLiteral("pingKindCheck"));
            ping->setProperty("pingKind", kind);
            rowLayout->addWidget(centred(ping));
            m_pingChecks.insert(kind, ping);
            connect(ping, &QCheckBox::toggled, this, [this, kind](bool on) {
                if (m_refreshing)
                    return;
                forkmesh::pings::setKindEnabled(kind, on);
                updateEmailEnablement();
                notifyChanged();
            });
            if (info.email) {
                auto *email = switchBox(label + QStringLiteral(" email"));
                email->setObjectName(QStringLiteral("pingEmailCheck"));
                email->setProperty("pingKind", kind);
                rowLayout->addWidget(centred(email));
                m_emailChecks.insert(kind, email);
                connect(email, &QCheckBox::toggled, this,
                        [this, kind](bool on) {
                            if (m_refreshing)
                                return;
                            QSettings().setValue(
                                forkmesh::pings::emailSettingKey(kind), on);
                            notifyChanged();
                        });
            } else {
                auto *none = new QWidget;
                none->setFixedWidth(kSwitchColumnWidth);
                none->setToolTip(QStringLiteral("This ping type has no email."));
                rowLayout->addWidget(none);
            }
            m_rows.insert(kind, row);
            column->addWidget(row);
            if (adminGroup)
                m_adminOnly << row;
        }
    }

    m_desktopBox = new QWidget;
    m_desktopBox->setObjectName(QStringLiteral("pingDesktopKinds"));
    auto *desktopColumn = new QVBoxLayout(m_desktopBox);
    desktopColumn->setContentsMargins(0, 0, 0, 0);
    desktopColumn->setSpacing(2);
    auto *desktopHeading = new QLabel(QStringLiteral("Turned off on this desktop"));
    QFont bold = desktopHeading->font();
    bold.setBold(true);
    desktopHeading->setFont(bold);
    desktopHeading->setContentsMargins(6, 8, 0, 2);
    desktopColumn->addWidget(desktopHeading);
    auto *desktopIntro = new QLabel(QStringLiteral(
        "Pings this desktop raises by itself, turned off from a Pings row's "
        "gear. They are still filed on the Pings page; only the alert sound "
        "and phone forwarding are off."));
    desktopIntro->setObjectName(QStringLiteral("statusLine"));
    desktopIntro->setWordWrap(true);
    desktopIntro->setContentsMargins(6, 0, 6, 2);
    desktopColumn->addWidget(desktopIntro);
    m_desktopBox->hide();
    column->addWidget(m_desktopBox);
    setAdmin(false);
}

void PingSettingsPanel::addDesktopKindRow(const QString &kind)
{
    if (!m_desktopBox || m_desktopChecks.contains(kind))
        return;
    auto *row = new QFrame;
    row->setObjectName(QStringLiteral("pingKindRow"));
    row->setProperty("pingKind", kind);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(6, 2, 6, 2);
    rowLayout->setSpacing(0);
    auto *name = new QLabel;
    name->setToolTip(kind);
    rowLayout->addWidget(name, 1);
    auto *ping = switchBox(kind + QStringLiteral(" alerts on this desktop"));
    ping->setObjectName(QStringLiteral("pingDesktopKindCheck"));
    ping->setProperty("pingKind", kind);
    rowLayout->addWidget(centred(ping));
    auto *none = new QWidget;
    none->setFixedWidth(kSwitchColumnWidth);
    none->setToolTip(QStringLiteral("This ping type has no email."));
    rowLayout->addWidget(none);
    connect(ping, &QCheckBox::toggled, this, [this, kind](bool on) {
        if (m_refreshing)
            return;
        // Local only: nothing for the relay to hear, so no changed().
        forkmesh::pings::setDesktopKindEnabled(kind, on);
    });
    m_desktopChecks.insert(kind, ping);
    m_desktopNames.insert(kind, name);
    m_rows.insert(kind, row);
    m_desktopBox->layout()->addWidget(row);
    m_desktopBox->show();
}

void PingSettingsPanel::attachDeliveryCombo(QComboBox *combo,
                                            const QString &settingKey)
{
    m_deliveryCombo = combo;
    m_deliverySettingKey = settingKey;
    refresh();
}

void PingSettingsPanel::attachEmailCheck(const QString &kind, QCheckBox *box)
{
    m_emailChecks.insert(kind, box);
    refresh();
}

void PingSettingsPanel::refresh()
{
    m_refreshing = true;
    const QStringList muted = forkmesh::pings::mutedKinds();
    for (auto it = m_pingChecks.cbegin(); it != m_pingChecks.cend(); ++it) {
        if (it.value())
            it.value()->setChecked(!muted.contains(it.key()));
    }
    const QStringList mutedHere = forkmesh::pings::mutedDesktopKinds();
    for (const QString &kind : mutedHere)
        addDesktopKindRow(kind);
    for (auto it = m_desktopChecks.cbegin(); it != m_desktopChecks.cend(); ++it) {
        if (it.value())
            it.value()->setChecked(!mutedHere.contains(it.key()));
        if (QLabel *name = m_desktopNames.value(it.key()))
            name->setText(desktopKindLabel ? desktopKindLabel(it.key())
                                           : it.key());
    }
    for (auto it = m_emailChecks.cbegin(); it != m_emailChecks.cend(); ++it) {
        if (it.value()) {
            const QSignalBlocker blocker(it.value());
            it.value()->setChecked(forkmesh::pings::emailKindEnabled(it.key()));
        }
    }
    if (m_emailMaster)
        m_emailMaster->setChecked(forkmesh::pings::emailEnabled());
    if (m_deliveryCombo && !m_deliverySettingKey.isEmpty()) {
        const QSignalBlocker blocker(m_deliveryCombo);
        const int index = m_deliveryCombo->findData(
            QSettings()
                .value(m_deliverySettingKey, QStringLiteral("digest"))
                .toString());
        m_deliveryCombo->setCurrentIndex(index < 0 ? 0 : index);
    }

    const forkmesh::pings::QuietHours quiet = forkmesh::pings::loadQuietHours();
    if (m_quietEnabled) {
        m_quietEnabled->setChecked(quiet.enabled);
        m_quietStart->setTime(QTime(quiet.start / 60, quiet.start % 60));
        m_quietEnd->setTime(QTime(quiet.end / 60, quiet.end % 60));
        for (int day = 0; day < m_quietDays.size(); ++day) {
            if (m_quietDays.at(day))
                m_quietDays.at(day)->setChecked(quiet.days.contains(day));
        }
    }
    m_refreshing = false;
    updateEmailEnablement();
    updateQuietStatus();
}

void PingSettingsPanel::setAdmin(bool admin)
{
    m_admin = admin;
    for (const QPointer<QWidget> &widget : std::as_const(m_adminOnly)) {
        if (widget)
            widget->setVisible(admin);
    }
}

QWidget *PingSettingsPanel::revealKind(const QString &kind)
{
    QFrame *row = m_rows.value(kind);
    if (!row || (!m_admin && m_adminOnly.contains(QPointer<QWidget>(row))))
        return nullptr;
    row->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QFrame#pingKindRow { background-color: rgba(56, 139, 253, 0.22); "
        "border-radius: 6px; }")));
    QPointer<QFrame> guard(row);
    QTimer::singleShot(kRevealHighlightMs, this, [guard] {
        if (guard)
            guard->setStyleSheet(QString());
    });
    QCheckBox *ping = m_pingChecks.value(kind);
    if (!ping)
        ping = m_desktopChecks.value(kind);
    if (ping)
        ping->setFocus(Qt::OtherFocusReason);
    return row;
}

void PingSettingsPanel::saveQuietHours()
{
    if (m_refreshing || !m_quietEnabled)
        return;
    forkmesh::pings::QuietHours quiet;
    quiet.enabled = m_quietEnabled->isChecked();
    quiet.start = m_quietStart->time().hour() * 60 + m_quietStart->time().minute();
    quiet.end = m_quietEnd->time().hour() * 60 + m_quietEnd->time().minute();
    quiet.days.clear();
    for (int day = 0; day < m_quietDays.size(); ++day) {
        if (m_quietDays.at(day) && m_quietDays.at(day)->isChecked())
            quiet.days << day;
    }
    // The window is wall-clock time here, so the relay is told whose wall.
    quiet.timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    forkmesh::pings::storeQuietHours(quiet);
    updateQuietStatus();
    notifyChanged();
}

void PingSettingsPanel::updateEmailEnablement()
{
    const bool master = forkmesh::pings::emailEnabled();
    for (auto it = m_emailChecks.cbegin(); it != m_emailChecks.cend(); ++it) {
        if (!it.value())
            continue;
        QCheckBox *ping = m_pingChecks.value(it.key());
        const bool pingOn = !ping || ping->isChecked();
        it.value()->setEnabled(master && pingOn);
        it.value()->setToolTip(
            !master ? QStringLiteral("Turn on \"Email me about pings\" first.")
            : !pingOn ? QStringLiteral("This ping type is off, so it is never "
                                       "emailed either.")
                      : QString());
    }
}

void PingSettingsPanel::updateQuietStatus()
{
    if (!m_quietStatus)
        return;
    const forkmesh::pings::QuietHours quiet = forkmesh::pings::loadQuietHours();
    const bool on = m_quietEnabled && m_quietEnabled->isChecked();
    bool anyDay = false;
    for (const QPointer<QCheckBox> &box : std::as_const(m_quietDays))
        anyDay = anyDay || (box && box->isChecked());
    QString text = forkmesh::pings::quietHoursSummary(quiet);
    if (on && !anyDay)
        text = QStringLiteral("Pick at least one day for quiet hours.");
    else if (on && quiet.start == quiet.end)
        text = QStringLiteral("Quiet hours need different start and end times.");
    const qint64 until = forkmesh::pings::quietUntilMs();
    if (until > 0) {
        text += QStringLiteral(" — quiet now, until %1").arg(
            QLocale().toString(QDateTime::fromMSecsSinceEpoch(until).time(),
                               QLocale::ShortFormat));
    }
    if (quiet.enabled) {
        text += QStringLiteral(" (%1)").arg(
            QString::fromUtf8(forkmesh::pings::quietZone(quiet).id()));
    }
    m_quietStatus->setText(text);
    if (m_quietStart)
        m_quietStart->setEnabled(on);
    if (m_quietEnd)
        m_quietEnd->setEnabled(on);
    for (const QPointer<QCheckBox> &box : std::as_const(m_quietDays)) {
        if (box)
            box->setEnabled(on);
    }
}

void PingSettingsPanel::notifyChanged()
{
    if (changed)
        changed();
}

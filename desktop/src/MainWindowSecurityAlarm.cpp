#include "MainWindow.h"
#include "HiddenTerms.h"
#include "MainWindowInternal.h"
#include "UiSounds.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

using namespace forkmesh::ui;

namespace {

constexpr int kSecurityAlarmToneIntervalMs = 3000;

}

QWidget *MainWindow::buildSecurityAlarmBanner()
{
    m_securityAlarmBanner = new QFrame;
    m_securityAlarmBanner->setObjectName("securityAlarmBanner");
    auto *icon = new QLabel;
    icon->setObjectName("securityAlarmBannerIcon");
    icon->setPixmap(themedOcticon(QStringLiteral("alert"), QColor("#f85149"), 18)
                        .pixmap(18, 18));
    m_securityAlarmLabel = new QLabel;
    m_securityAlarmLabel->setObjectName("securityAlarmBannerLabel");
    m_securityAlarmLabel->setTextFormat(Qt::PlainText);
    m_securityAlarmLabel->setWordWrap(true);

    auto *open = new QPushButton(QStringLiteral("Open results"));
    open->setObjectName("primaryButton");
    open->setCursor(Qt::PointingHandCursor);
    setOcticon(open, "search", 14);
    connect(open, &QPushButton::clicked, this, [this] {
        QTimer::singleShot(0, this, [this] { openSecurityAlarmResults(); });
    });
    auto *acknowledge = new QPushButton(QStringLiteral("Acknowledge"));
    acknowledge->setObjectName("ghostButton");
    acknowledge->setCursor(Qt::PointingHandCursor);
    acknowledge->setToolTip(QStringLiteral(
        "Silence the alarm until these searches find results you have not "
        "acknowledged"));
    setOcticon(acknowledge, "check", 14);
    connect(acknowledge, &QPushButton::clicked, this, [this] {
        QTimer::singleShot(0, this, [this] { acknowledgeSecurityAlarms(); });
    });

    auto *layout = new QHBoxLayout(m_securityAlarmBanner);
    layout->setContentsMargins(16, 10, 12, 10);
    layout->setSpacing(10);
    layout->addWidget(icon);
    layout->addWidget(m_securityAlarmLabel, 1);
    layout->addWidget(open);
    layout->addWidget(acknowledge);
    m_securityAlarmBanner->hide();
    QTimer::singleShot(0, this, [this] { evaluateSecurityAlarms(); });
    return m_securityAlarmBanner;
}

void MainWindow::evaluateSecurityAlarms()
{
    const SecurityAlarmUpdate update =
        m_securityAlarmState.update(savedSearchStore().searches());
    const bool announce = m_securityAlarmPrimed;
    m_securityAlarmPrimed = true;

    if (m_securityAlarmBanner) {
        m_securityAlarmBanner->setVisible(!update.active.isEmpty());
        if (m_securityAlarmLabel)
            m_securityAlarmLabel->setText(SecurityAlarmState::summary(update.active));
    }

    if (update.active.isEmpty()) {
        if (m_securityAlarmTone)
            m_securityAlarmTone->stop();
    } else {
        if (!m_securityAlarmTone) {
            m_securityAlarmTone = new QTimer(this);
            m_securityAlarmTone->setInterval(kSecurityAlarmToneIntervalMs);
            connect(m_securityAlarmTone, &QTimer::timeout, this,
                    &MainWindow::playSecurityAlarmTone);
        }
        if (!m_securityAlarmTone->isActive()) {
            m_securityAlarmTone->start();
            playSecurityAlarmTone();
        } else if (!update.raised.isEmpty()) {
            playSecurityAlarmTone();
        }
    }

    if (update.changed)
        refreshSavedSearchList();
    if (!announce || update.raised.isEmpty())
        return;
    QApplication::alert(this, 0);
    for (const SecurityAlarm &alarm : update.raised) {
        NotificationLink link;
        link.kind = QStringLiteral("search");
        if (!alarm.scope.isEmpty()) {
            const int slash = alarm.scope.indexOf(QLatin1Char('/'));
            link.owner = alarm.scope.left(slash);
            link.name = alarm.scope.mid(slash + 1);
        }
        link.ref = alarm.id;
        addNotification(
            QStringLiteral("Security alarm: a security-pertinent saved search has %1 "
                           "result%2")
                .arg(alarm.results)
                .arg(alarm.results == 1 ? QString() : QStringLiteral("s")),
            forkmesh::hiddenterms::redact(
                QStringLiteral("Security-pertinent search in %1. Open it from the "
                               "alarm banner or Saved searches; the alarm sounds "
                               "until you acknowledge it on the desktop.")
                    .arg(alarm.scope.isEmpty() ? QStringLiteral("all repositories")
                                               : alarm.scope)),
            true, link);
    }
}

void MainWindow::acknowledgeSecurityAlarms()
{
    SavedSearchStore &store = savedSearchStore();
    bool changed = false;
    for (const SecurityAlarm &alarm : m_securityAlarmState.active())
        changed = store.acknowledgeSecurityAlarm(alarm.id) || changed;
    if (changed && !store.save())
        flashMessage(QStringLiteral("Couldn't write saved searches to disk."), true);
    evaluateSecurityAlarms();
}

void MainWindow::openSecurityAlarmResults()
{
    const QVector<SecurityAlarm> &active = m_securityAlarmState.active();
    if (active.isEmpty())
        return;
    openSavedSearch(active.first().id);
}

void MainWindow::playSecurityAlarmTone()
{
    using forkmesh::sonify::Cue;
    using forkmesh::sonify::UiSounds;
    if (!UiSounds::defaultEnabled())
        return;
    if (UiSounds *sounds = UiSounds::instance())
        sounds->preview(Cue::Error);
    else
        QApplication::beep();
}

void MainWindow::setSavedSearchSecurityPertinent(const QString &id, bool pertinent)
{
    SavedSearchStore &store = savedSearchStore();
    if (store.setSecurityPertinent(id, pertinent)) {
        if (!store.save())
            flashMessage(QStringLiteral("Couldn't write saved searches to disk."), true);
        else
            flashMessage(pertinent
                             ? QStringLiteral("Security pertinent: any result for that "
                                              "saved search sounds an alarm.")
                             : QStringLiteral("That saved search is no longer "
                                              "security pertinent."),
                         false, QString(), QStringLiteral("search"));
    }
    refreshSavedSearchList();
    scheduleSavedSearchAlerts();
    evaluateSecurityAlarms();
    if (pertinent)
        checkSavedSearchAlerts();
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testSecurityAlarmBannerVisible() const
{
    return m_securityAlarmBanner && !m_securityAlarmBanner->isHidden();
}
#endif

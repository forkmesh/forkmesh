// The top-bar clock. The installed Timers package owns the model — what a
// typed request means, what a chip reads, when a countdown or alarm has run
// out — and this file is the host side of that contribution: the clock button
// and its chips in the window chrome, their menus, the once-a-second repaint,
// and the package-owned settings the entries are stored in.
//
// Everything here is inert while the package is not installed. attach/detach
// are the whole lifecycle: removing Timers takes the clock off the top bar and
// its entries out of settings (PackageRegistry::uninstall clears the package's
// own data prefix), and installing it again brings back a bare clock rather
// than yesterday's timers.

#include "MainWindow.h"

#include "ForkMeshPackageRegistry.h"
#include "MainWindowInternal.h"
#include "TimersFeature.h"

#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QWidget>

using forkmesh::timers::Timer;
using forkmesh::timers::TimerKind;
using forkmesh::timers::TimersFeature;

namespace {

// How much of an entry's name fits on a chip before the figure stops being
// the thing the eye lands on.
constexpr int kChipLabelLength = 16;
constexpr qint64 kFiveMinutesMs = 5 * 60 * 1000;
// A countdown that ran out while the app was closed is still announced here
// when it opens, but a phone buzzing about it an hour later is noise. Matches
// TIMER_PING_MAX_AGE_MS in app/src/timer_pings.py, which refuses older ones.
constexpr qint64 kTimerPingMaxAgeMs = 15 * 60 * 1000;

const QString kTimersClockPlacementSetting =
    QStringLiteral("ui/timersClockPlacement");
const QString kTimersClockLeft = QStringLiteral("left");
const QString kTimersClockCenter = QStringLiteral("center");
const QString kTimersClockRight = QStringLiteral("right");

class TimersClockDragFilter : public QObject
{
public:
    TimersClockDragFilter(QPushButton *button,
                          std::function<void(const QPoint &)> onDrop)
        : QObject(button), m_button(button), m_onDrop(std::move(onDrop))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_button)
            return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_pressGlobal = mouse->globalPosition().toPoint();
                m_pressed = true;
                m_dragging = false;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (!m_pressed || !(mouse->buttons() & Qt::LeftButton))
                break;
            if (!m_dragging &&
                (mouse->globalPosition().toPoint() - m_pressGlobal)
                        .manhattanLength() >= QApplication::startDragDistance()) {
                m_dragging = true;
                m_button->setDown(false);
                QApplication::setOverrideCursor(Qt::ClosedHandCursor);
            }
            if (m_dragging)
                return true;
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton)
                break;
            m_pressed = false;
            if (!m_dragging)
                break;
            m_dragging = false;
            QApplication::restoreOverrideCursor();
            m_button->setDown(false);
            m_onDrop(mouse->globalPosition().toPoint());
            return true;
        }
        default:
            break;
        }
        return false;
    }

private:
    QPushButton *m_button;
    std::function<void(const QPoint &)> m_onDrop;
    QPoint m_pressGlobal;
    bool m_pressed = false;
    bool m_dragging = false;
};

QString chipLabel(const QString &label)
{
    if (label.size() <= kChipLabelLength)
        return label;
    return label.left(kChipLabelLength - 1).trimmed() +
           QString::fromUtf8("\xE2\x80\xA6");
}

} // namespace

qint64 MainWindow::timersNowMs() const
{
    return QDateTime::currentMSecsSinceEpoch() + m_timersClockOffsetMs;
}

QWidget *MainWindow::createTimersNavStrip()
{
    if (m_timersNavStrip)
        return m_timersNavStrip;

    m_timersNavStrip = new QWidget;
    m_timersNavStrip->setObjectName(QStringLiteral("timersNavStrip"));
    m_timersChipRow = new QHBoxLayout(m_timersNavStrip);
    m_timersChipRow->setContentsMargins(0, 0, 0, 0);
    m_timersChipRow->setSpacing(4);

    m_timersClockButton = new QPushButton;
    m_timersClockButton->setObjectName(QStringLiteral("timersClockButton"));
    m_timersClockButton->setCursor(Qt::PointingHandCursor);
    m_timersClockButton->setAccessibleName(
        QStringLiteral("Clock - add a timer, countdown or alarm"));
    m_timersClockButton->setToolTip(QString::fromUtf8(
        "Timers, countdowns and alarms \xE2\x80\x94 click to add one, drag "
        "to move it along the top bar"));
    forkmesh::ui::setOcticon(m_timersClockButton, QStringLiteral("clock"), 14);
    connect(m_timersClockButton, &QPushButton::clicked, this,
            &MainWindow::showTimersClockMenu);
    // Right-click offers the same package menu as a rail button: settings,
    // export and remove.
    attachPackageRailMenu(m_timersClockButton, QStringLiteral("builtin:timers"));
    m_timersClockButton->installEventFilter(new TimersClockDragFilter(
        m_timersClockButton,
        [this](const QPoint &globalPos) { dropTimersClock(globalPos); }));
    m_timersChipRow->addWidget(m_timersClockButton);

    // One shared tick for the whole strip: chips only ever show whole seconds,
    // so a QTimer per entry would buy nothing but wakeups. It runs only while
    // there is something on the strip.
    m_timersTick = new QTimer(this);
    m_timersTick->setInterval(1000);
    m_timersTick->setTimerType(Qt::CoarseTimer);
    connect(m_timersTick, &QTimer::timeout, this,
            &MainWindow::refreshTimersNavStrip);

    m_timersNavStrip->hide();
    return m_timersNavStrip;
}

void MainWindow::installTimersClockSlots(QWidget *chrome, QBoxLayout *left,
                                         QBoxLayout *center, QBoxLayout *right,
                                         QWidget *rightBefore)
{
    m_timersChromeBar = chrome;
    m_timersClockSlots.clear();
    m_timersClockSlots.insert(kTimersClockLeft, {left, nullptr});
    m_timersClockSlots.insert(kTimersClockCenter, {center, nullptr});
    m_timersClockSlots.insert(kTimersClockRight, {right, rightBefore});
    setTimersClockPlacement(timersClockPlacement());
}

QString MainWindow::timersClockPlacement() const
{
    const QString stored =
        QSettings().value(kTimersClockPlacementSetting).toString();
    if (stored == kTimersClockCenter || stored == kTimersClockRight)
        return stored;
    return kTimersClockLeft;
}

void MainWindow::setTimersClockPlacement(const QString &placement)
{
    const QString key = placement == kTimersClockCenter ||
                                placement == kTimersClockRight
                            ? placement
                            : kTimersClockLeft;
    if (key == kTimersClockLeft)
        QSettings().remove(kTimersClockPlacementSetting);
    else
        QSettings().setValue(kTimersClockPlacementSetting, key);
    const TimersClockSlot target = m_timersClockSlots.value(key);
    if (!m_timersNavStrip || !target.row ||
        target.row->indexOf(m_timersNavStrip) >= 0)
        return;
    const bool hidden = m_timersNavStrip->isHidden();
    for (auto it = m_timersClockSlots.constBegin();
         it != m_timersClockSlots.constEnd(); ++it) {
        if (it.value().row)
            it.value().row->removeWidget(m_timersNavStrip);
    }
    const int index = target.before ? target.row->indexOf(target.before) : -1;
    target.row->insertWidget(index, m_timersNavStrip, 0, Qt::AlignVCenter);
    m_timersNavStrip->setVisible(!hidden);
}

void MainWindow::dropTimersClock(const QPoint &globalPos)
{
    if (!m_timersChromeBar)
        return;
    const int width = qMax(1, m_timersChromeBar->width());
    const int x = m_timersChromeBar->mapFromGlobal(globalPos).x();
    setTimersClockPlacement(x < width / 3       ? kTimersClockLeft
                            : x > width * 2 / 3 ? kTimersClockRight
                                                : kTimersClockCenter);
}

void MainWindow::addTimersClockPlacementActions(QMenu *menu)
{
    if (!menu || !m_timersFeature)
        return;
    menu->addSection(QStringLiteral("Clock position"));
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    const QString current = timersClockPlacement();
    const QList<QPair<QString, QString>> placements = {
        {kTimersClockLeft, QStringLiteral("Left, beside the indicators")},
        {kTimersClockCenter, QStringLiteral("Middle, beside search")},
        {kTimersClockRight, QStringLiteral("Right, beside the window buttons")},
    };
    for (const auto &placement : placements) {
        const QString key = placement.first;
        QAction *action = menu->addAction(placement.second);
        action->setObjectName(QStringLiteral("timersClockPlacement-%1").arg(key));
        action->setCheckable(true);
        action->setChecked(key == current);
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this,
                [this, key] { setTimersClockPlacement(key); });
    }
}

void MainWindow::attachTimersPackage()
{
    if (m_timersFeature)
        return;
    m_timersFeature = std::make_unique<TimersFeature>();
    if (m_packageRegistry) {
        const QByteArray stored =
            m_packageRegistry
                ->packageSetting(QStringLiteral("timers"),
                                 QStringLiteral("state"))
                .toString()
                .toUtf8();
        m_timersFeature->load(stored);
    }
    if (m_timersNavStrip)
        m_timersNavStrip->show();
    refreshTimersNavStrip();
    // A countdown that ran out while the app was closed has something to say,
    // but attach() runs while the window is still being built. The chips are
    // painted now; the announcement waits for a window that can show it.
    QTimer::singleShot(0, this, [this] {
        if (!m_timersFeature)
            return;
        m_timersAnnouncementsReady = true;
        refreshTimersNavStrip();
    });
}

void MainWindow::detachTimersPackage()
{
    if (!m_timersFeature && !m_timersNavStrip)
        return;
    m_timersFeature.reset();
    m_timersAnnouncementsReady = false;
    if (m_timersTick)
        m_timersTick->stop();
    if (m_timersNavStrip)
        m_timersNavStrip->hide();
    // The chips are the package's UI contribution, so they go with it rather
    // than lingering as dead buttons until the next repaint.
    refreshTimersNavStrip();
}

void MainWindow::saveTimers()
{
    if (!m_timersFeature || !m_packageRegistry)
        return;
    m_packageRegistry->setPackageSetting(
        QStringLiteral("timers"), QStringLiteral("state"),
        QString::fromUtf8(m_timersFeature->serialize()));
}

void MainWindow::refreshTimersNavStrip()
{
    if (!m_timersNavStrip || !m_timersChipRow)
        return;

    // Drop the previous chips; the clock is the one permanent child.
    while (m_timersChipRow->count() > 1) {
        QLayoutItem *item = m_timersChipRow->takeAt(1);
        if (QWidget *widget = item->widget()) {
            // Off the strip now rather than whenever the deletion is
            // delivered: a chip that is still a child would answer a lookup
            // for the current strip with last second's figure.
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    if (!m_timersFeature) {
        m_timersNavStrip->setVisible(false);
        return;
    }

    const qint64 now = timersNowMs();
    const QList<Timer> fired = m_timersAnnouncementsReady
                                   ? m_timersFeature->takeNewlyExpired(now)
                                   : QList<Timer>();
    for (const Timer &timer : fired) {
        const bool alarm = timer.kind == TimerKind::Alarm;
        const QString title = alarm ? QStringLiteral("Alarm")
                                    : QStringLiteral("Countdown finished");
        const QString body =
            alarm ? QStringLiteral("%1 (%2)")
                        .arg(timer.label, TimersFeature::figureText(timer, now))
                  : timer.label;
        flashMessage(QStringLiteral("%1: %2").arg(title, body));
        logSystem(QStringLiteral("%1: %2.").arg(title, body));
        // A countdown or alarm is usually set so the reader can look away, so
        // it reaches past this window: the OS card when allowed, and a Ping.
        // When this desktop can send it, the Ping is the account's own — the
        // relay files it and pushes it to the phone — so the desktop files no
        // second, local-only row beside it.
        const bool pinged = sendTimerPing(timer, title, body);
        postNotification(title, body, false, QString(), QString(), false,
                         !pinged);
        QApplication::alert(this, 0);
    }
    if (!fired.isEmpty())
        saveTimers();

    for (const Timer &timer : m_timersFeature->ordered(now)) {
        const bool finished = TimersFeature::isFinished(timer, now);
        auto *chip = new QPushButton(
            QStringLiteral("%1  %2")
                .arg(TimersFeature::figureText(timer, now), chipLabel(timer.label)));
        chip->setObjectName(timer.paused ? QStringLiteral("timerChipPaused")
                            : finished   ? QStringLiteral("timerChipDone")
                                         : QStringLiteral("timerChip"));
        chip->setProperty("timerId", timer.id);
        chip->setProperty("timerKind", TimersFeature::kindName(timer.kind));
        // The figure alone cannot tell "17:00" the alarm from "17:00" left on
        // a countdown, so timers and alarms carry their own glyph.
        if (timer.kind == TimerKind::Timer)
            forkmesh::ui::setOcticon(chip, QStringLiteral("stopwatch"), 12);
        else if (timer.kind == TimerKind::Alarm)
            forkmesh::ui::setOcticon(chip, QStringLiteral("bell"), 12);
        chip->setCursor(Qt::PointingHandCursor);
        const QString spoken = TimersFeature::spokenStatus(timer, now);
        chip->setAccessibleName(spoken);
        chip->setToolTip(spoken);
        const QString id = timer.id;
        connect(chip, &QPushButton::clicked, this, [this, chip, id] {
            showTimerChipMenu(id, chip->mapToGlobal(QPoint(0, chip->height())));
        });
        m_timersChipRow->addWidget(chip);
    }

    m_timersNavStrip->setVisible(true);
    if (m_timersTick) {
        // A finished chip counts up and a paused one holds still, but both
        // still sit in a strip that has to notice the next expiry, so the
        // second hand runs while any entry exists and stops when none does.
        const bool ticking = m_timersFeature->count() > 0;
        if (ticking && !m_timersTick->isActive())
            m_timersTick->start();
        else if (!ticking && m_timersTick->isActive())
            m_timersTick->stop();
    }
}

bool MainWindow::sendTimerPing(const Timer &timer, const QString &title,
                               const QString &body)
{
    if (timer.kind == TimerKind::Timer || !m_networkAccess)
        return false;
    // A window test moves the strip's clock to watch an expiry happen; that
    // is not a real one, and the relay would refuse its future timestamp.
    if (m_timersClockOffsetMs != 0)
        return false;
    if (QDateTime::currentMSecsSinceEpoch() - timer.endsAtMs >
        kTimerPingMaxAgeMs)
        return false;
    QNetworkRequest request;
    QByteArray payload;
    if (!prepareTimerPingRequest(timer.id, TimersFeature::kindName(timer.kind),
                                 timer.endsAtMs, timer.label, &request,
                                 &payload))
        return false; // signed out: the desktop's own Ping is all there is
    QNetworkReply *reply = m_networkAccess->post(request, payload);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, title, body] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 &&
            status < 300)
            return;
        logSystem(QStringLiteral("Couldn't send the timer ping for \"%1\" to "
                                 "your other devices (%2).")
                      .arg(body.isEmpty() ? title : body,
                           status > 0 ? QString::number(status)
                                      : reply->errorString()));
        // The relay filed nothing, so without this the Pings page would have
        // no row for a countdown the reader may have missed. Its OS card was
        // already shown when it ran out, so this row stays quiet.
        AppNotification item;
        item.title = title;
        item.body = body;
        item.kind = QStringLiteral("desktop");
        item.quiet = true;
        recordNotification(item);
    });
    return true;
}

bool MainWindow::prepareTimerPingRequest(const QString &timerId,
                                         const QString &kind, qint64 endsAtMs,
                                         const QString &label,
                                         QNetworkRequest *request,
                                         QByteArray *body)
{
    if (!request || !body)
        return false;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/notifications/timer"));
    url.setQuery(QString());
    // Every credential this desktop holds, as the payout notice does: a
    // desktop signed in by its key has no session token, and a stored one
    // can have expired. The relay reads the session first, then the proof.
    const QString sessionToken = accountSessionTokenForUrl(url);
    // The proof names this one expiry — timer, kind, instant and label — so
    // a captured one cannot announce anything else. The signer travels as
    // user=, never node=.
    const QString resource = timerId + QLatin1Char('\n') + kind +
                             QLatin1Char('\n') + QString::number(endsAtMs) +
                             QLatin1Char('\n') + label;
    if (!m_profileIdentity.isValid())
        m_profileIdentity.load();
    QString user, ts, sig;
    if (signOrgTaskProof(forkmesh::ui::kTimerPingProof, resource, &user, &ts,
                         &sig)) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("user"), user);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
    }
    if (sessionToken.isEmpty() && !url.hasQuery())
        return false;
    *request = QNetworkRequest(url);
    request->setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json"));
    request->setTransferTimeout(15000);
    if (!sessionToken.isEmpty()) {
        request->setRawHeader(
            "Authorization",
            (QStringLiteral("Bearer ") + sessionToken).toUtf8());
    }
    *body = QJsonDocument(QJsonObject{
                              {QStringLiteral("timerId"), timerId},
                              {QStringLiteral("kind"), kind},
                              {QStringLiteral("endsAt"), endsAtMs},
                              {QStringLiteral("label"), label},
                          })
                .toJson(QJsonDocument::Compact);
    return true;
}

bool MainWindow::addTimer(const QString &request, TimerKind kind)
{
    // A refused request is the reader's typo, not a failure of the app, so it
    // keeps the red pill but stays out of the shared error reports.
    if (!m_timersFeature) {
        flashMessage(QStringLiteral(
                         "Timers is not installed. Install it from the Market "
                         "list."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    QString error;
    if (!m_timersFeature->addFromText(request, timersNowMs(), &error, kind)) {
        flashMessage(error, true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    saveTimers();
    refreshTimersNavStrip();
    return true;
}

bool MainWindow::addUsageResetCountdown(const QString &label, qint64 resetAtMs)
{
    if (!m_timersFeature) {
        flashMessage(QStringLiteral(
                         "Timers is not installed. Install it from the Market "
                         "list."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    // Whole seconds, rounded up, so the chip reads the same figure as the
    // "resets in" note rather than a second short of it. The reset is wall
    // time, so the strip's test offset does not move it.
    const qint64 remainingMs =
        resetAtMs - QDateTime::currentMSecsSinceEpoch();
    const qint64 durationMs = (remainingMs + 999) / 1000 * 1000;
    if (remainingMs <= 0 || durationMs > TimersFeature::kMaximumDurationMs) {
        flashMessage(QStringLiteral("That limit has no reset ahead to count "
                                    "down to."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    if (m_timersFeature->count() >= TimersFeature::kMaximumTimers) {
        flashMessage(QStringLiteral("The top bar holds %1 timers, countdowns "
                                    "and alarms. Remove one first.")
                         .arg(TimersFeature::kMaximumTimers),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    Timer timer;
    timer.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    timer.label = label;
    timer.kind = TimerKind::Countdown;
    timer.durationMs = durationMs;
    timer.endsAtMs = timersNowMs() + durationMs;
    if (!m_timersFeature->add(timer)) {
        flashMessage(QStringLiteral("That could not be added to the clock."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    saveTimers();
    refreshTimersNavStrip();
    return true;
}

void MainWindow::showTimersClockMenu()
{
    if (!m_timersFeature || !m_timersClockButton)
        return;

    QMenu menu(this);
    const QColor menuIconColor = menu.palette().color(QPalette::PlaceholderText);
    QAction *startTimer =
        menu.addAction(QString::fromUtf8("Start a timer\xE2\x80\xA6"));
    startTimer->setIcon(forkmesh::ui::themedOcticon(QStringLiteral("stopwatch"),
                                                    menuIconColor, 16));

    menu.addSeparator();
    struct Preset {
        const char *text;
        const char *request;
    };
    static const Preset kPresets[] = {
        {"5 minute countdown", "5m"},   {"10 minute countdown", "10m"},
        {"15 minute countdown", "15m"}, {"25 minute countdown", "25m"},
        {"1 hour countdown", "1h"},
    };
    for (const Preset &preset : kPresets) {
        QAction *action = menu.addAction(QString::fromLatin1(preset.text));
        action->setData(QString::fromLatin1(preset.request));
    }
    QAction *customCountdown =
        menu.addAction(QString::fromUtf8("Custom countdown\xE2\x80\xA6"));

    menu.addSeparator();
    QAction *setAlarm =
        menu.addAction(QString::fromUtf8("Set an alarm\xE2\x80\xA6"));
    setAlarm->setIcon(
        forkmesh::ui::themedOcticon(QStringLiteral("bell"), menuIconColor, 16));

    QAction *clearFinished = nullptr;
    const qint64 now = timersNowMs();
    int finished = 0;
    for (const Timer &timer : m_timersFeature->ordered(now)) {
        if (TimersFeature::isFinished(timer, now))
            ++finished;
    }
    if (finished > 0) {
        menu.addSeparator();
        clearFinished = menu.addAction(
            QStringLiteral("Clear %1 finished").arg(finished));
    }

    addTimersClockPlacementActions(&menu);

    QAction *chosen = menu.exec(m_timersClockButton->mapToGlobal(
        QPoint(0, m_timersClockButton->height())));
    if (!chosen || !m_timersFeature)
        return;
    if (chosen == clearFinished) {
        const qint64 clearAt = timersNowMs();
        for (const Timer &timer : m_timersFeature->ordered(clearAt)) {
            if (TimersFeature::isFinished(timer, clearAt))
                m_timersFeature->remove(timer.id);
        }
        saveTimers();
        refreshTimersNavStrip();
        return;
    }

    const auto ask = [this](const QString &title, const QString &prompt,
                            bool allowEmpty, QString *typed) {
        bool ok = false;
        *typed = QInputDialog::getText(this, title, prompt, QLineEdit::Normal,
                                       QString(), &ok);
        return ok && (allowEmpty || !typed->trimmed().isEmpty());
    };
    QString typed;
    if (chosen == startTimer) {
        if (ask(QStringLiteral("Start a timer"),
                QStringLiteral("What are you timing? (optional)"), true, &typed))
            addTimer(typed, TimerKind::Timer);
        return;
    }
    if (chosen == customCountdown) {
        if (ask(QStringLiteral("New countdown"),
                QString::fromUtf8("How long, and what for?\n"
                                  "\"25m standup\", \"1h30m deploy freeze\", "
                                  "\"coffee in 4m\", \"17:00 demo\""),
                false, &typed))
            addTimer(typed, TimerKind::Countdown);
        return;
    }
    if (chosen == setAlarm) {
        if (ask(QStringLiteral("Set an alarm"),
                QString::fromUtf8("When should it ring, and what for?\n"
                                  "\"7:30 standup\", \"demo at 5pm\", "
                                  "\"in 20m stretch\""),
                false, &typed))
            addTimer(typed, TimerKind::Alarm);
        return;
    }
    if (chosen->actionGroup())
        return;
    const QString request = chosen->data().toString();
    if (!request.isEmpty())
        addTimer(request, TimerKind::Countdown);
}

void MainWindow::showTimerChipMenu(const QString &id, const QPoint &globalPos)
{
    if (!m_timersFeature)
        return;
    const Timer *timer = m_timersFeature->timer(id);
    if (!timer)
        return;
    const qint64 now = timersNowMs();
    const bool finished = TimersFeature::isFinished(*timer, now);

    QMenu menu(this);
    QAction *pause = nullptr;
    QAction *extend = nullptr;
    QAction *reset = nullptr;
    QString removeText = QStringLiteral("Remove");
    switch (timer->kind) {
    case TimerKind::Timer:
        pause = menu.addAction(timer->paused ? QStringLiteral("Resume")
                                             : QStringLiteral("Pause"));
        reset = menu.addAction(QStringLiteral("Reset to 0:00"));
        break;
    case TimerKind::Countdown:
        // The chip reads what is left; the menu answers the other question,
        // what time that is. A label, not a choice, so it cannot be picked.
        menu.addAction(TimersFeature::finishText(*timer, now))->setEnabled(false);
        menu.addSeparator();
        if (!finished) {
            pause = menu.addAction(timer->paused ? QStringLiteral("Resume")
                                                 : QStringLiteral("Pause"));
        }
        extend = menu.addAction(QStringLiteral("Add 5 minutes"));
        reset = menu.addAction(
            QStringLiteral("Restart (%1)")
                .arg(TimersFeature::remainingText(timer->durationMs)));
        break;
    case TimerKind::Alarm:
        extend = menu.addAction(finished ? QStringLiteral("Snooze 5 minutes")
                                         : QStringLiteral("Push back 5 minutes"));
        removeText = finished ? QStringLiteral("Dismiss")
                              : QStringLiteral("Cancel alarm");
        break;
    }
    menu.addSeparator();
    QAction *remove = menu.addAction(removeText);

    // The menu runs its own event loop, and a tick inside it would delete the
    // very chip this menu belongs to. The refresh at the end starts the second
    // hand again.
    if (m_timersTick)
        m_timersTick->stop();
    QAction *chosen = menu.exec(globalPos);
    if (!m_timersFeature)
        return;
    if (!chosen) {
        refreshTimersNavStrip();
        return;
    }
    if (chosen == pause)
        m_timersFeature->togglePause(id, timersNowMs());
    else if (chosen == extend)
        m_timersFeature->extend(id, kFiveMinutesMs, timersNowMs());
    else if (chosen == reset)
        m_timersFeature->reset(id, timersNowMs());
    else if (chosen == remove)
        m_timersFeature->remove(id);
    saveTimers();
    refreshTimersNavStrip();
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testAddTimer(const QString &request, const QString &kind)
{
    TimerKind parsedKind = TimerKind::Countdown;
    if (!kind.isEmpty() && !TimersFeature::kindFromName(kind, &parsedKind))
        return false;
    return addTimer(request, parsedKind);
}

QString MainWindow::testTimersClockSlot() const
{
    if (!m_timersNavStrip)
        return {};
    for (auto it = m_timersClockSlots.constBegin();
         it != m_timersClockSlots.constEnd(); ++it) {
        if (it.value().row && it.value().row->indexOf(m_timersNavStrip) >= 0)
            return it.key();
    }
    return {};
}

void MainWindow::testDropTimersClock(qreal fraction)
{
    if (!m_timersChromeBar)
        return;
    dropTimersClock(m_timersChromeBar->mapToGlobal(
        QPoint(qRound(m_timersChromeBar->width() * fraction),
               m_timersChromeBar->height() / 2)));
}

QStringList MainWindow::testTimerChipTexts() const
{
    QStringList texts;
    if (!m_timersChipRow)
        return texts;
    for (int i = 1; i < m_timersChipRow->count(); ++i) {
        if (auto *chip =
                qobject_cast<QPushButton *>(m_timersChipRow->itemAt(i)->widget()))
            texts << chip->text().simplified();
    }
    return texts;
}

bool MainWindow::testTimerCommand(const QString &labelPrefix,
                                  const QString &command)
{
    if (!m_timersFeature)
        return false;
    const qint64 now = timersNowMs();
    for (const Timer &timer : m_timersFeature->ordered(now)) {
        if (!timer.label.startsWith(labelPrefix, Qt::CaseInsensitive))
            continue;
        bool done = false;
        if (command == QLatin1String("pause"))
            done = m_timersFeature->togglePause(timer.id, now);
        else if (command == QLatin1String("reset"))
            done = m_timersFeature->reset(timer.id, now);
        else if (command == QLatin1String("extend") ||
                 command == QLatin1String("snooze"))
            done = m_timersFeature->extend(timer.id, kFiveMinutesMs, now);
        else if (command == QLatin1String("remove"))
            done = m_timersFeature->remove(timer.id);
        if (done) {
            saveTimers();
            refreshTimersNavStrip();
        }
        return done;
    }
    return false;
}

// Move the strip's own clock forward so a test can watch a countdown run out
// without waiting for it. Only the strip reads this offset.
void MainWindow::testAdvanceTimers(qint64 milliseconds)
{
    m_timersClockOffsetMs += milliseconds;
    refreshTimersNavStrip();
}

QUrl MainWindow::testTimerPingRequest(const QString &timerId,
                                      const QString &kind, qint64 endsAtMs,
                                      const QString &label, QByteArray *body)
{
    QNetworkRequest request;
    QByteArray payload;
    if (!prepareTimerPingRequest(timerId, kind, endsAtMs, label, &request,
                                 &payload))
        return {};
    if (body)
        *body = payload;
    return request.url();
}
#endif

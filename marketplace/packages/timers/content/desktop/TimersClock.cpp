#include "TimersClock.h"

#include "OcticonIcons.h"

#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

namespace forkmesh::timers {
namespace {

constexpr int kChipLabelLength = 16;
constexpr qint64 kFiveMinutesMs = 5 * 60 * 1000;
constexpr qint64 kTimerPingMaxAgeMs = 15 * 60 * 1000;

const QString kPlacementSetting = QStringLiteral("ui/timersClockPlacement");
const QString kSlotLeft = QStringLiteral("left");
const QString kSlotCenter = QStringLiteral("center");
const QString kSlotRight = QStringLiteral("right");
const QString kPingPath = QStringLiteral("/api/notifications/timer");
const QString kPingProof = QStringLiteral("forkmesh-account-timer-ping-v1");
const QString kCountdownCommand = QStringLiteral("countdown.add");

class ClockDragFilter : public QObject
{
public:
    ClockDragFilter(QPushButton *button, std::function<void(const QPoint &)> onDrop)
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

bool containsFold(const QString &text, const QString &query)
{
    return text.contains(query, Qt::CaseInsensitive);
}

}

TimersClock::TimersClock(const forkmesh::market::NativePackageHost &host,
                         QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("timersNavStrip"));
    m_chipRow = new QHBoxLayout(this);
    m_chipRow->setContentsMargins(0, 0, 0, 0);
    m_chipRow->setSpacing(4);

    m_clockButton = new QPushButton(this);
    m_clockButton->setObjectName(QStringLiteral("timersClockButton"));
    m_clockButton->setCursor(Qt::PointingHandCursor);
    m_clockButton->setAccessibleName(
        QStringLiteral("Clock - add a timer, countdown or alarm"));
    m_clockButton->setToolTip(QString::fromUtf8(
        "Timers, countdowns and alarms \xE2\x80\x94 click to add one, drag "
        "to move it along the top bar"));
    forkmesh::ui::setOcticon(m_clockButton, QStringLiteral("clock"), 14);
    connect(m_clockButton, &QPushButton::clicked, this,
            &TimersClock::showClockMenu);
    if (m_host.attachPackageMenu)
        m_host.attachPackageMenu(m_clockButton);
    m_clockButton->installEventFilter(new ClockDragFilter(
        m_clockButton, [this](const QPoint &globalPos) { dropAt(globalPos); }));
    m_chipRow->addWidget(m_clockButton);

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    m_tick->setTimerType(Qt::CoarseTimer);
    connect(m_tick, &QTimer::timeout, this, &TimersClock::refresh);

    if (m_host.setting)
        m_feature.load(m_host.setting(QStringLiteral("state"), QString())
                           .toString()
                           .toUtf8());
    refresh();
    QTimer::singleShot(0, this, [this] {
        m_announcementsReady = true;
        refresh();
    });
}

qint64 TimersClock::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch() + m_clockOffsetMs;
}

void TimersClock::save()
{
    if (m_host.setSetting)
        m_host.setSetting(QStringLiteral("state"),
                          QString::fromUtf8(m_feature.serialize()));
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

void TimersClock::flash(const QString &text)
{
    if (m_host.flashMessage)
        m_host.flashMessage(text, true);
}

QString TimersClock::chromeSlot() const
{
    const QString stored = QSettings().value(kPlacementSetting).toString();
    if (stored == kSlotCenter || stored == kSlotRight)
        return stored;
    return kSlotLeft;
}

void TimersClock::setChromeSlot(const QString &slot)
{
    const QString key =
        slot == kSlotCenter || slot == kSlotRight ? slot : kSlotLeft;
    if (key == kSlotLeft)
        QSettings().remove(kPlacementSetting);
    else
        QSettings().setValue(kPlacementSetting, key);
    if (m_host.chromeSlotChanged)
        m_host.chromeSlotChanged();
}

void TimersClock::dropAt(const QPoint &globalPos)
{
    if (m_host.chromeSlotAt)
        setChromeSlot(m_host.chromeSlotAt(globalPos));
}

void TimersClock::addChromeMenuActions(QMenu *menu)
{
    if (!menu)
        return;
    menu->addSection(QStringLiteral("Clock position"));
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    const QString current = chromeSlot();
    const QList<QPair<QString, QString>> placements = {
        {kSlotLeft, QStringLiteral("Left, beside the indicators")},
        {kSlotCenter, QStringLiteral("Middle, beside search")},
        {kSlotRight, QStringLiteral("Right, beside the window buttons")},
    };
    for (const auto &placement : placements) {
        const QString key = placement.first;
        QAction *action = menu->addAction(placement.second);
        action->setObjectName(QStringLiteral("timersClockPlacement-%1").arg(key));
        action->setCheckable(true);
        action->setChecked(key == current);
        action->setActionGroup(group);
        connect(action, &QAction::triggered, this,
                [this, key] { setChromeSlot(key); });
    }
}

void TimersClock::refresh()
{
    while (m_chipRow->count() > 1) {
        QLayoutItem *item = m_chipRow->takeAt(1);
        if (QWidget *widget = item->widget()) {
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }

    const qint64 now = nowMs();
    const QList<Timer> fired =
        m_announcementsReady ? m_feature.takeNewlyExpired(now) : QList<Timer>();
    for (const Timer &timer : fired) {
        const bool alarm = timer.kind == TimerKind::Alarm;
        const QString title = alarm ? QStringLiteral("Alarm")
                                    : QStringLiteral("Countdown finished");
        const QString body =
            alarm ? QStringLiteral("%1 (%2)")
                        .arg(timer.label, TimersFeature::figureText(timer, now))
                  : timer.label;
        if (m_host.flashMessage)
            m_host.flashMessage(QStringLiteral("%1: %2").arg(title, body), false);
        if (m_host.logActivity)
            m_host.logActivity(QStringLiteral("%1: %2.").arg(title, body));
        const bool pinged = sendPing(timer, title, body);
        if (m_host.notify)
            m_host.notify(title, body, !pinged);
        QApplication::alert(window(), 0);
    }
    if (!fired.isEmpty())
        save();

    for (const Timer &timer : m_feature.ordered(now)) {
        const bool finished = TimersFeature::isFinished(timer, now);
        auto *chip = new QPushButton(
            QStringLiteral("%1  %2")
                .arg(TimersFeature::figureText(timer, now), chipLabel(timer.label)));
        chip->setObjectName(timer.paused ? QStringLiteral("timerChipPaused")
                            : finished   ? QStringLiteral("timerChipDone")
                                         : QStringLiteral("timerChip"));
        chip->setProperty("timerId", timer.id);
        chip->setProperty("timerKind", TimersFeature::kindName(timer.kind));
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
            showChipMenu(id, chip->mapToGlobal(QPoint(0, chip->height())));
        });
        m_chipRow->addWidget(chip);
    }

    const bool ticking = m_feature.count() > 0;
    if (ticking && !m_tick->isActive())
        m_tick->start();
    else if (!ticking && m_tick->isActive())
        m_tick->stop();
}

bool TimersClock::pingRequest(const QString &timerId, const QString &kind,
                              qint64 endsAtMs, const QString &label,
                              QNetworkRequest *request, QByteArray *body)
{
    if (!request || !body || !m_host.accountApiUrl)
        return false;
    QUrl url = m_host.accountApiUrl(kPingPath);
    if (!url.isValid())
        return false;
    const QString sessionToken =
        m_host.accountSessionToken ? m_host.accountSessionToken(url) : QString();
    const QString resource = timerId + QLatin1Char('\n') + kind +
                             QLatin1Char('\n') + QString::number(endsAtMs) +
                             QLatin1Char('\n') + label;
    QString user, ts, sig;
    if (m_host.signProof && m_host.signProof(kPingProof, resource, &user, &ts, &sig)) {
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
    if (!sessionToken.isEmpty())
        request->setRawHeader("Authorization",
                              (QStringLiteral("Bearer ") + sessionToken).toUtf8());
    *body = QJsonDocument(QJsonObject{
                              {QStringLiteral("timerId"), timerId},
                              {QStringLiteral("kind"), kind},
                              {QStringLiteral("endsAt"), endsAtMs},
                              {QStringLiteral("label"), label},
                          })
                .toJson(QJsonDocument::Compact);
    return true;
}

bool TimersClock::sendPing(const Timer &timer, const QString &title,
                           const QString &body)
{
    if (timer.kind == TimerKind::Timer || !m_host.network)
        return false;
    if (m_clockOffsetMs != 0)
        return false;
    if (QDateTime::currentMSecsSinceEpoch() - timer.endsAtMs > kTimerPingMaxAgeMs)
        return false;
    QNetworkRequest request;
    QByteArray payload;
    if (!pingRequest(timer.id, TimersFeature::kindName(timer.kind),
                     timer.endsAtMs, timer.label, &request, &payload))
        return false;
    QNetworkReply *reply = m_host.network->post(request, payload);
    const auto log = m_host.logActivity;
    const auto record = m_host.recordQuietNotification;
    connect(reply, &QNetworkReply::finished, reply,
            [reply, title, body, log, record] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 &&
            status < 300)
            return;
        if (log)
            log(QStringLiteral("Couldn't send the timer ping for \"%1\" to "
                               "your other devices (%2).")
                    .arg(body.isEmpty() ? title : body,
                         status > 0 ? QString::number(status)
                                    : reply->errorString()));
        if (record)
            record(title, body);
    });
    return true;
}

bool TimersClock::addTimer(const QString &request, TimerKind kind)
{
    QString error;
    if (!m_feature.addFromText(request, nowMs(), &error, kind)) {
        flash(error);
        return false;
    }
    save();
    refresh();
    return true;
}

bool TimersClock::addCountdownUntil(const QString &label, qint64 endsAtMs)
{
    const qint64 remainingMs = endsAtMs - QDateTime::currentMSecsSinceEpoch();
    const qint64 durationMs = (remainingMs + 999) / 1000 * 1000;
    if (remainingMs <= 0 || durationMs > TimersFeature::kMaximumDurationMs) {
        flash(QStringLiteral("That limit has no reset ahead to count down to."));
        return false;
    }
    if (m_feature.count() >= TimersFeature::kMaximumTimers) {
        flash(QStringLiteral("The top bar holds %1 timers, countdowns and "
                             "alarms. Remove one first.")
                  .arg(TimersFeature::kMaximumTimers));
        return false;
    }
    Timer timer;
    timer.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    timer.label = label;
    timer.kind = TimerKind::Countdown;
    timer.durationMs = durationMs;
    timer.endsAtMs = nowMs() + durationMs;
    if (!m_feature.add(timer)) {
        flash(QStringLiteral("That could not be added to the clock."));
        return false;
    }
    save();
    refresh();
    return true;
}

bool TimersClock::acceptsHostCommand(const QString &command) const
{
    return command == kCountdownCommand;
}

bool TimersClock::hostCommand(const QString &command, const QJsonObject &args)
{
    if (command != kCountdownCommand)
        return false;
    return addCountdownUntil(args.value(QStringLiteral("label")).toString(),
                             qint64(args.value(QStringLiteral("endsAt")).toDouble()));
}

QList<forkmesh::market::NativePackageSearchHit>
TimersClock::searchHits(const QString &rawQuery, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString query = rawQuery.trimmed();
    if (query.isEmpty())
        return hits;
    const qint64 now = nowMs();
    for (const Timer &timer : m_feature.ordered(now)) {
        if (hits.size() >= limit)
            break;
        const QString kind = TimersFeature::kindName(timer.kind);
        if (!containsFold(timer.label, query) && !containsFold(kind, query))
            continue;
        hits.append({timer.id, timer.label.isEmpty() ? kind : timer.label,
                     TimersFeature::spokenStatus(timer, now)});
    }
    return hits;
}

void TimersClock::activateSearchHit(const QString &id)
{
    if (id.isEmpty()) {
        showClockMenu();
        return;
    }
    for (QPushButton *chip : findChildren<QPushButton *>()) {
        if (chip->property("timerId").toString() == id) {
            showChipMenu(id, chip->mapToGlobal(QPoint(0, chip->height())));
            return;
        }
    }
}

void TimersClock::activate()
{
    showClockMenu();
}

void TimersClock::showClockMenu()
{
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
    const qint64 now = nowMs();
    int finished = 0;
    for (const Timer &timer : m_feature.ordered(now)) {
        if (TimersFeature::isFinished(timer, now))
            ++finished;
    }
    if (finished > 0) {
        menu.addSeparator();
        clearFinished =
            menu.addAction(QStringLiteral("Clear %1 finished").arg(finished));
    }

    addChromeMenuActions(&menu);

    const QPointer<TimersClock> guarded(this);
    QAction *chosen = menu.exec(
        m_clockButton->mapToGlobal(QPoint(0, m_clockButton->height())));
    if (!chosen || !guarded)
        return;
    if (chosen == clearFinished) {
        const qint64 clearAt = nowMs();
        for (const Timer &timer : m_feature.ordered(clearAt)) {
            if (TimersFeature::isFinished(timer, clearAt))
                m_feature.remove(timer.id);
        }
        save();
        refresh();
        return;
    }

    const auto ask = [this](const QString &title, const QString &prompt,
                            bool allowEmpty, QString *typed) {
        bool ok = false;
        *typed = QInputDialog::getText(window(), title, prompt, QLineEdit::Normal,
                                       QString(), &ok);
        return ok && (allowEmpty || !typed->trimmed().isEmpty());
    };
    QString typed;
    if (chosen == startTimer) {
        if (ask(QStringLiteral("Start a timer"),
                QStringLiteral("What are you timing? (optional)"), true, &typed) &&
            guarded)
            addTimer(typed, TimerKind::Timer);
        return;
    }
    if (chosen == customCountdown) {
        if (ask(QStringLiteral("New countdown"),
                QString::fromUtf8("How long, and what for?\n"
                                  "\"25m standup\", \"1h30m deploy freeze\", "
                                  "\"coffee in 4m\", \"17:00 demo\""),
                false, &typed) &&
            guarded)
            addTimer(typed, TimerKind::Countdown);
        return;
    }
    if (chosen == setAlarm) {
        if (ask(QStringLiteral("Set an alarm"),
                QString::fromUtf8("When should it ring, and what for?\n"
                                  "\"7:30 standup\", \"demo at 5pm\", "
                                  "\"in 20m stretch\""),
                false, &typed) &&
            guarded)
            addTimer(typed, TimerKind::Alarm);
        return;
    }
    if (chosen->actionGroup())
        return;
    const QString request = chosen->data().toString();
    if (!request.isEmpty())
        addTimer(request, TimerKind::Countdown);
}

void TimersClock::showChipMenu(const QString &id, const QPoint &globalPos)
{
    const Timer *timer = m_feature.timer(id);
    if (!timer)
        return;
    const qint64 now = nowMs();
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

    m_tick->stop();
    const QPointer<TimersClock> guarded(this);
    QAction *chosen = menu.exec(globalPos);
    if (!guarded)
        return;
    if (!chosen) {
        refresh();
        return;
    }
    if (chosen == pause)
        m_feature.togglePause(id, nowMs());
    else if (chosen == extend)
        m_feature.extend(id, kFiveMinutesMs, nowMs());
    else if (chosen == reset)
        m_feature.reset(id, nowMs());
    else if (chosen == remove)
        m_feature.remove(id);
    save();
    refresh();
}

QStringList TimersClock::chipTexts() const
{
    QStringList texts;
    for (int i = 1; i < m_chipRow->count(); ++i) {
        if (auto *chip = qobject_cast<QPushButton *>(m_chipRow->itemAt(i)->widget()))
            texts << chip->text().simplified();
    }
    return texts;
}

bool TimersClock::command(const QString &labelPrefix, const QString &command)
{
    const qint64 now = nowMs();
    for (const Timer &timer : m_feature.ordered(now)) {
        if (!timer.label.startsWith(labelPrefix, Qt::CaseInsensitive))
            continue;
        bool done = false;
        if (command == QLatin1String("pause"))
            done = m_feature.togglePause(timer.id, now);
        else if (command == QLatin1String("reset"))
            done = m_feature.reset(timer.id, now);
        else if (command == QLatin1String("extend") ||
                 command == QLatin1String("snooze"))
            done = m_feature.extend(timer.id, kFiveMinutesMs, now);
        else if (command == QLatin1String("remove"))
            done = m_feature.remove(timer.id);
        if (done) {
            save();
            refresh();
        }
        return done;
    }
    return false;
}

void TimersClock::advance(qint64 milliseconds)
{
    m_clockOffsetMs += milliseconds;
    refresh();
}

namespace {

const forkmesh::market::NativePackageRegistration timersRegistration({
    QStringLiteral("builtin:timers"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Timers"),
    QStringLiteral("clock"),
    kPingPath,
    kPingProof,
    nullptr,
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new TimersClock(host, parent);
    },
    {},
});

}

}

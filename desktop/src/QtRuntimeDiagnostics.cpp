#include "QtRuntimeDiagnostics.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QStyle>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cstring>

namespace forkmesh::ui {

namespace {
constexpr int kHeartbeatMs = 100;
constexpr int kMaxLagSamples = 3000; // five minutes at 10 Hz
constexpr int kMaxSnapshots = 300;   // five minutes at 1 Hz
constexpr qint64 kStructureRefreshNs = 5'000'000'000LL;
constexpr qint64 kObjectWalkRefreshNs = 60'000'000'000LL;
qint64 g_lastObjectWalkNs = -1;
constexpr qint64 kMinimumRateWindowNs = 250'000'000LL;

const char *receiverClassName(QObject *receiver)
{
    return receiver && receiver->metaObject()
               ? receiver->metaObject()->className()
               : "(null)";
}

constexpr int kMaxPaintReceivers = 256;
constexpr int kTopPaintReceivers = 8;

QString paintReceiverName(QObject *receiver)
{
    QString name = QString::fromLatin1(receiverClassName(receiver));
    int depth = 0;
    for (QObject *at = receiver; at && depth < 8; at = at->parent(), ++depth) {
        const QString objectName = at->objectName();
        if (objectName.isEmpty())
            continue;
        return at == receiver
                   ? QStringLiteral("%1 %2").arg(name, objectName)
                   : QStringLiteral("%1 in %2").arg(name, objectName);
    }
    return name;
}
} // namespace

QtRuntimeDiagnostics *QtRuntimeDiagnostics::s_instance = nullptr;

QtRuntimeDiagnostics::QtRuntimeDiagnostics(QObject *parent) : QObject(parent)
{
    s_instance = this;
    setObjectName(QStringLiteral("qtRuntimeDiagnostics"));
    m_clock.start();
    m_nextHeartbeatMs = kHeartbeatMs;
    m_lastSampleNs = m_clock.nsecsElapsed();
    m_lagHistory.reserve(kMaxLagSamples);
    m_history.reserve(kMaxSnapshots);

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setTimerType(Qt::PreciseTimer);
    m_heartbeatTimer->setInterval(kHeartbeatMs);
    QObject::connect(m_heartbeatTimer, &QTimer::timeout, this,
                     [this] { heartbeat(); });
    m_heartbeatTimer->start();

    m_sampleTimer = new QTimer(this);
    m_sampleTimer->setInterval(1000);
    QObject::connect(m_sampleTimer, &QTimer::timeout, this,
                     [this] { sampleNow(); });
    m_sampleTimer->start();
    sampleNow();
}

QtRuntimeDiagnostics::~QtRuntimeDiagnostics()
{
    setDetailedEnabled(false);
    if (s_instance == this)
        s_instance = nullptr;
}

void QtRuntimeDiagnostics::setDetailedEnabled(bool enabled)
{
    if (m_detailed == enabled)
        return;
    m_detailed = enabled;
    if (enabled) {
        if (qApp && !m_filterInstalled) {
            qApp->installEventFilter(this);
            m_filterInstalled = true;
        }
        connectDispatcher();
        g_lastObjectWalkNs = -1;
        refreshStructure();
        m_lastStructureRefreshNs = m_clock.nsecsElapsed();
    } else {
        if (qApp && m_filterInstalled)
            qApp->removeEventFilter(this);
        m_filterInstalled = false;
        disconnectDispatcher();
    }
    m_snapshot.detailed = enabled;
    if (onSnapshot)
        onSnapshot(m_snapshot);
}

bool QtRuntimeDiagnostics::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (m_detailed && event) {
        const EventGroup group = groupForEvent(event->type());
        ++m_intervalEvents[group].count;
        ++m_sessionEvents[group].count;
    }
    return false;
}

void QtRuntimeDiagnostics::recordEventDelivery(const char *receiverClass,
                                                int receiverClassLength,
                                                QEvent::Type eventType,
                                                qint64 elapsedUs,
                                                const QString &paintReceiver)
{
    if (s_instance && s_instance->m_detailed)
        s_instance->noteDelivery(receiverClass, receiverClassLength,
                                 eventType, elapsedUs, paintReceiver);
}

void QtRuntimeDiagnostics::noteDelivery(const char *receiverClass,
                                        int receiverClassLength,
                                        QEvent::Type eventType,
                                        qint64 elapsedUs,
                                        const QString &paintReceiver)
{
    if (!paintReceiver.isEmpty()) {
        auto tally = m_paintReceivers.find(paintReceiver);
        if (tally == m_paintReceivers.end()) {
            const QString key = m_paintReceivers.size() < kMaxPaintReceivers
                                    ? paintReceiver
                                    : QStringLiteral("(other receivers)");
            tally = m_paintReceivers.find(key);
            if (tally == m_paintReceivers.end())
                tally = m_paintReceivers.insert(key, PaintReceiver{key});
        }
        ++tally->count;
        tally->totalUs += qMax<qint64>(0, elapsedUs);
    }
    const EventGroup group = groupForEvent(eventType);
    EventStats &interval = m_intervalEvents[group];
    EventStats &session = m_sessionEvents[group];
    interval.totalUs += qMax<qint64>(0, elapsedUs);
    session.totalUs += qMax<qint64>(0, elapsedUs);
    if (elapsedUs > interval.maxUs) {
        interval.maxUs = elapsedUs;
        interval.slowestReceiverClass = QString::fromLatin1(
            receiverClass, receiverClassLength);
    }
    if (elapsedUs > session.maxUs) {
        session.maxUs = elapsedUs;
        session.slowestReceiverClass = QString::fromLatin1(
            receiverClass, receiverClassLength);
    }
}

QtRuntimeDiagnostics::EventGroup
QtRuntimeDiagnostics::groupForEvent(QEvent::Type type)
{
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Shortcut:
    case QEvent::ShortcutOverride:
    case QEvent::InputMethod:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
        return Input;
    case QEvent::Timer:
        return Timer;
    case QEvent::MetaCall:
        return Queued;
    case QEvent::Paint:
    case QEvent::UpdateRequest:
    case QEvent::Expose:
        return Paint;
    case QEvent::LayoutRequest:
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::Polish:
    case QEvent::PolishRequest:
    case QEvent::StyleChange:
    case QEvent::FontChange:
    case QEvent::PaletteChange:
        return Layout;
    case QEvent::DeferredDelete:
        return DeferredDelete;
    default:
        return Other;
    }
}

QString QtRuntimeDiagnostics::eventGroupName(EventGroup group)
{
    static const char *const names[] = {
        "Input", "Timer", "Queued/meta-call", "Paint/update", "Layout/geometry",
        "Deferred delete", "Other"};
    const int index = static_cast<int>(group);
    return index >= 0 && index < EventGroupCount
               ? QString::fromLatin1(names[index])
               : QStringLiteral("Unknown");
}

void QtRuntimeDiagnostics::heartbeat()
{
    const qint64 now = m_clock.elapsed();
    const qint64 gap = now - (m_nextHeartbeatMs - kHeartbeatMs);
    if (gap > 10000) {
        // Suspend/resume is not a GUI stall and should not poison the peak.
        m_nextHeartbeatMs = now + kHeartbeatMs;
        return;
    }
    const qint64 lag = qMax<qint64>(0, now - m_nextHeartbeatMs);
    m_nextHeartbeatMs += kHeartbeatMs;
    if (m_nextHeartbeatMs <= now)
        m_nextHeartbeatMs = now + kHeartbeatMs;
    m_snapshot.loopLagMs = lag;
    m_snapshot.peakLoopLagMs = qMax(m_snapshot.peakLoopLagMs, lag);
    if (m_lagHistory.size() < kMaxLagSamples) {
        m_lagHistory.append(lag);
    } else {
        m_lagHistory[m_lagHistoryNext] = lag;
        m_lagHistoryNext = (m_lagHistoryNext + 1) % kMaxLagSamples;
    }
}

qint64 QtRuntimeDiagnostics::percentile95(QVector<qint64> values)
{
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    const int index = qBound(0, int((values.size() - 1) * 0.95),
                             values.size() - 1);
    return values.at(index);
}

void QtRuntimeDiagnostics::sampleNow()
{
    const qint64 nowNs = m_clock.nsecsElapsed();
    const qint64 elapsedNs = qMax<qint64>(1, nowNs - m_lastSampleNs);

    // Immediate dialog/test refreshes can arrive milliseconds after the
    // one-second timer. Dividing their few events by that tiny window produced
    // artificial five-digit event and paint rates in copied reports. Publish
    // session counters, but retain the interval until it spans real time.
    if (elapsedNs < kMinimumRateWindowNs) {
        m_snapshot.sessionEvents = m_sessionEvents;
        m_snapshot.topPaintReceivers = topPaintReceivers();
        m_snapshot.sampledAtMs = QDateTime::currentMSecsSinceEpoch();
        m_snapshot.detailed = m_detailed;
        if (onSnapshot)
            onSnapshot(m_snapshot);
        return;
    }
    m_lastSampleNs = nowNs;

    quint64 allEvents = 0;
    for (const EventStats &stats : m_intervalEvents)
        allEvents += stats.count;
    const double seconds = double(elapsedNs) / 1.0e9;
    m_snapshot.eventsPerSecond = qRound(allEvents / seconds);
    m_snapshot.paintsPerSecond =
        qRound(m_intervalEvents[Paint].count / seconds);
    m_snapshot.layoutsPerSecond =
        qRound(m_intervalEvents[Layout].count / seconds);
    m_snapshot.wakeupsPerSecond = qRound(m_intervalWakeups / seconds);
    m_snapshot.dispatcherBusyPercent =
        qBound(0.0, 100.0 * double(m_intervalBusyNs) / double(elapsedNs), 100.0);
    m_snapshot.intervalEvents = m_intervalEvents;
    m_snapshot.sessionEvents = m_sessionEvents;
    m_snapshot.topPaintReceivers = topPaintReceivers();
    m_snapshot.sampledAtMs = QDateTime::currentMSecsSinceEpoch();
    m_snapshot.detailed = m_detailed;

    if (!m_lagHistory.isEmpty()) {
        qint64 total = 0;
        for (qint64 lag : std::as_const(m_lagHistory))
            total += lag;
        m_snapshot.averageLoopLagMs = total / m_lagHistory.size();
        m_snapshot.p95LoopLagMs = percentile95(m_lagHistory);
    }
    if (m_detailed &&
        (m_lastStructureRefreshNs <= 0 ||
         nowNs - m_lastStructureRefreshNs >= kStructureRefreshNs)) {
        refreshStructure();
        m_lastStructureRefreshNs = nowNs;
    }

    appendSnapshot(m_snapshot);
    m_intervalEvents = {};
    m_intervalBusyNs = 0;
    m_intervalWakeups = 0;
    if (onSnapshot)
        onSnapshot(m_snapshot);
}

QVector<QtRuntimeDiagnostics::PaintReceiver>
QtRuntimeDiagnostics::topPaintReceivers() const
{
    QVector<PaintReceiver> top;
    top.reserve(m_paintReceivers.size());
    for (const PaintReceiver &tally : m_paintReceivers)
        top.append(tally);
    const int keep = qMin(kTopPaintReceivers, int(top.size()));
    std::partial_sort(top.begin(), top.begin() + keep, top.end(),
                      [](const PaintReceiver &a, const PaintReceiver &b) {
                          return a.count != b.count ? a.count > b.count
                                                    : a.name < b.name;
                      });
    top.resize(keep);
    return top;
}

void QtRuntimeDiagnostics::appendSnapshot(const Snapshot &snapshot)
{
    if (m_history.size() < kMaxSnapshots) {
        m_history.append(snapshot);
        return;
    }
    m_history[m_historyNext] = snapshot;
    m_historyNext = (m_historyNext + 1) % kMaxSnapshots;
}

QVector<QtRuntimeDiagnostics::Snapshot> QtRuntimeDiagnostics::history() const
{
    if (m_history.size() < kMaxSnapshots || m_historyNext == 0)
        return m_history;
    QVector<Snapshot> ordered;
    ordered.reserve(m_history.size());
    for (int i = m_historyNext; i < m_history.size(); ++i)
        ordered.append(m_history.at(i));
    for (int i = 0; i < m_historyNext; ++i)
        ordered.append(m_history.at(i));
    return ordered;
}

void QtRuntimeDiagnostics::refreshStructure()
{
    if (!qApp)
        return;
    const auto widgets = QApplication::allWidgets();
    m_snapshot.widgetCount = widgets.size();
    m_snapshot.visibleWidgetCount = 0;
    for (QWidget *widget : widgets)
        if (widget && widget->isVisible())
            ++m_snapshot.visibleWidgetCount;

    const auto windows = QGuiApplication::allWindows();
    m_snapshot.windowCount = windows.size();
    m_snapshot.visibleWindowCount = 0;
    for (QWindow *window : windows)
        if (window && window->isVisible())
            ++m_snapshot.visibleWindowCount;

    const qint64 nowNs = m_clock.nsecsElapsed();
    if (g_lastObjectWalkNs < 0 || nowNs < g_lastObjectWalkNs
        || nowNs - g_lastObjectWalkNs >= kObjectWalkRefreshNs) {
        g_lastObjectWalkNs = nowNs;
        int objectCount = 0;
        int timerCount = 0;
        int activeTimerCount = 0;
        const auto countTree = [&](QObject *root, bool includeRoot) {
            if (includeRoot)
                ++objectCount;
            objectCount += int(root->findChildren<QObject *>().size());
            const auto timers = root->findChildren<QTimer *>();
            timerCount += int(timers.size());
            for (QTimer *timer : timers)
                if (timer->isActive())
                    ++activeTimerCount;
        };
        countTree(qApp, false);
        for (QWidget *topLevel : QApplication::topLevelWidgets())
            if (topLevel && !topLevel->parent())
                countTree(topLevel, true);
        m_snapshot.objectCount = objectCount;
        m_snapshot.timerCount = timerCount;
        m_snapshot.activeTimerCount = activeTimerCount;
    }
    if (QThreadPool *pool = QThreadPool::globalInstance()) {
        m_snapshot.threadPoolActive = pool->activeThreadCount();
        m_snapshot.threadPoolMax = pool->maxThreadCount();
    }
}

void QtRuntimeDiagnostics::connectDispatcher()
{
    disconnectDispatcher();
    QAbstractEventDispatcher *dispatcher =
        QAbstractEventDispatcher::instance(qApp ? qApp->thread() : nullptr);
    if (!dispatcher)
        return;
    m_awakeConnection = QObject::connect(
        dispatcher, &QAbstractEventDispatcher::awake, this, [this] {
            ++m_intervalWakeups;
            m_dispatchAwakeNs = m_clock.nsecsElapsed();
        });
    m_blockConnection = QObject::connect(
        dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (m_dispatchAwakeNs >= 0)
                m_intervalBusyNs +=
                    qMax<qint64>(0, m_clock.nsecsElapsed() - m_dispatchAwakeNs);
            m_dispatchAwakeNs = -1;
        });
}

void QtRuntimeDiagnostics::disconnectDispatcher()
{
    QObject::disconnect(m_awakeConnection);
    QObject::disconnect(m_blockConnection);
    m_awakeConnection = {};
    m_blockConnection = {};
    m_dispatchAwakeNs = -1;
}

void QtRuntimeDiagnostics::resetSession()
{
    const bool detailed = m_detailed;
    m_lagHistory.clear();
    m_lagHistoryNext = 0;
    m_history.clear();
    m_historyNext = 0;
    m_intervalEvents = {};
    m_sessionEvents = {};
    m_paintReceivers.clear();
    m_intervalBusyNs = 0;
    m_intervalWakeups = 0;
    m_snapshot = {};
    m_snapshot.detailed = detailed;
    m_nextHeartbeatMs = m_clock.elapsed() + kHeartbeatMs;
    m_lastSampleNs = m_clock.nsecsElapsed();
    m_lastStructureRefreshNs = 0;
    g_lastObjectWalkNs = -1;
    if (detailed) {
        refreshStructure();
        m_lastStructureRefreshNs = m_clock.nsecsElapsed();
    }
    if (onSnapshot)
        onSnapshot(m_snapshot);
}

QString QtRuntimeDiagnostics::report() const
{
    const Snapshot s = m_snapshot;
    QStringList lines;
    lines << QStringLiteral("ForkMesh Qt runtime diagnostics")
          << QStringLiteral("Captured: %1")
                 .arg(QDateTime::fromMSecsSinceEpoch(s.sampledAtMs)
                          .toString(Qt::ISODateWithMs))
          << QStringLiteral("Detailed event sampling: %1")
                 .arg(s.detailed ? QStringLiteral("on") : QStringLiteral("off"))
          << QString()
          << QStringLiteral("Qt runtime")
          << QStringLiteral("  Runtime/build: %1 / %2")
                 .arg(QString::fromLatin1(qVersion()),
                      QString::fromLatin1(QT_VERSION_STR))
          << QStringLiteral("  Platform: %1")
                 .arg(QGuiApplication::platformName())
          << QStringLiteral("  Style: %1")
                 .arg(qApp && qApp->style() && qApp->style()->metaObject()
                          ? QString::fromLatin1(
                                qApp->style()->metaObject()->className())
                          : QStringLiteral("unavailable"));
    if (QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance())
        lines << QStringLiteral("  Dispatcher: %1")
                     .arg(QString::fromLatin1(
                         dispatcher->metaObject()->className()));
    lines << QStringLiteral("  Library paths: %1")
                 .arg(QCoreApplication::libraryPaths().join(QStringLiteral("; ")))
          << QString()
          << QStringLiteral("Event loop")
          << QStringLiteral("  Lag current/average/p95/peak: %1/%2/%3/%4 ms")
                 .arg(s.loopLagMs)
                 .arg(s.averageLoopLagMs)
                 .arg(s.p95LoopLagMs)
                 .arg(s.peakLoopLagMs)
          << QStringLiteral("  Events/wakeups: %1/s / %2/s")
                 .arg(s.eventsPerSecond)
                 .arg(s.wakeupsPerSecond)
          << QStringLiteral("  Dispatcher active: %1%")
                 .arg(s.dispatcherBusyPercent, 0, 'f', 1)
          << QString()
          << QStringLiteral("Objects and workers")
          << QStringLiteral("  Widgets visible/total: %1/%2")
                 .arg(s.visibleWidgetCount)
                 .arg(s.widgetCount)
          << QStringLiteral("  Windows visible/total: %1/%2")
                 .arg(s.visibleWindowCount)
                 .arg(s.windowCount)
          << QStringLiteral("  Application QObject tree: %1")
                 .arg(s.objectCount)
          << QStringLiteral("  QTimers active/total: %1/%2")
                 .arg(s.activeTimerCount)
                 .arg(s.timerCount)
          << QStringLiteral("  Global thread pool active/max: %1/%2")
                 .arg(s.threadPoolActive)
                 .arg(s.threadPoolMax)
          << QString()
          << QStringLiteral("Event delivery this session");
    for (int i = 0; i < EventGroupCount; ++i) {
        const EventStats &stats = s.sessionEvents.at(i);
        const double averageUs = stats.count > 0
                                     ? double(stats.totalUs) / stats.count
                                     : 0.0;
        lines << QStringLiteral("  %1: %2 events, %3 us average, %4 us max%5")
                     .arg(eventGroupName(static_cast<EventGroup>(i)))
                     .arg(stats.count)
                     .arg(averageUs, 0, 'f', 1)
                     .arg(stats.maxUs)
                     .arg(stats.slowestReceiverClass.isEmpty()
                              ? QString()
                              : QStringLiteral(" (%1)")
                                    .arg(stats.slowestReceiverClass));
    }
    if (!s.topPaintReceivers.isEmpty()) {
        lines << QString() << QStringLiteral("Most painted widgets this session");
        for (const PaintReceiver &tally : s.topPaintReceivers) {
            lines << QStringLiteral("  %1: %2 paints, %3 us average")
                         .arg(tally.name)
                         .arg(tally.count)
                         .arg(tally.count > 0
                                  ? double(tally.totalUs) / tally.count
                                  : 0.0,
                              0, 'f', 1);
        }
    }
    lines << QString() << QStringLiteral("Screens");
    for (QScreen *screen : QGuiApplication::screens()) {
        if (!screen)
            continue;
        lines << QStringLiteral(
                     "  %1: %2x%3 at %4,%5; available %6x%7; DPR %8; "
                     "logical/physical DPI %9/%10; %11 Hz")
                     .arg(screen->name())
                     .arg(screen->geometry().width())
                     .arg(screen->geometry().height())
                     .arg(screen->geometry().x())
                     .arg(screen->geometry().y())
                     .arg(screen->availableGeometry().width())
                     .arg(screen->availableGeometry().height())
                     .arg(screen->devicePixelRatio(), 0, 'f', 2)
                     .arg(screen->logicalDotsPerInch(), 0, 'f', 1)
                     .arg(screen->physicalDotsPerInch(), 0, 'f', 1)
                     .arg(screen->refreshRate(), 0, 'f', 1);
    }
    lines << QString() << QStringLiteral("Recent samples (newest last)");
    const QVector<Snapshot> recentHistory = history();
    const int first = qMax(0, recentHistory.size() - 12);
    for (int i = first; i < recentHistory.size(); ++i) {
        const Snapshot &sample = recentHistory.at(i);
        lines << QStringLiteral(
                     "  %1  loop %2 ms  events %3/s  paint/layout %4/%5/s  "
                     "busy %6%  widgets %7/%8  pool %9/%10")
                     .arg(QDateTime::fromMSecsSinceEpoch(sample.sampledAtMs)
                              .toString(QStringLiteral("HH:mm:ss")))
                     .arg(sample.loopLagMs)
                     .arg(sample.eventsPerSecond)
                     .arg(sample.paintsPerSecond)
                     .arg(sample.layoutsPerSecond)
                     .arg(sample.dispatcherBusyPercent, 0, 'f', 1)
                     .arg(sample.visibleWidgetCount)
                     .arg(sample.widgetCount)
                     .arg(sample.threadPoolActive)
                     .arg(sample.threadPoolMax);
    }
    return lines.join(QLatin1Char('\n'));
}

QtEventDeliveryProbe::QtEventDeliveryProbe(QObject *receiver, QEvent *event)
{
    m_active = QtRuntimeDiagnostics::s_instance &&
               QtRuntimeDiagnostics::s_instance->detailedEnabled() && event &&
               QThread::currentThread() ==
                   QtRuntimeDiagnostics::s_instance->thread();
    if (m_active) {
        // A receiver may delete itself while handling this event. Capture the
        // payload-free metadata before dispatch and never dereference the
        // QObject (or event) from the probe destructor after notify returns.
        const char *className = receiverClassName(receiver);
        m_receiverClassLength = qMin(
            int(std::strlen(className)), int(m_receiverClass.size()) - 1);
        std::copy_n(className, m_receiverClassLength, m_receiverClass.begin());
        m_eventType = event->type();
        if (m_eventType == QEvent::Paint)
            m_paintReceiver = paintReceiverName(receiver);
        m_elapsed.start();
    }
}

QtEventDeliveryProbe::~QtEventDeliveryProbe()
{
    if (m_active)
        QtRuntimeDiagnostics::recordEventDelivery(
            m_receiverClass.data(), m_receiverClassLength, m_eventType,
            m_elapsed.nsecsElapsed() / 1000, m_paintReceiver);
}

} // namespace forkmesh::ui

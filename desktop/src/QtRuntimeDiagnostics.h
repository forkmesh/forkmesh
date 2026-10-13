#pragma once

#include <QElapsedTimer>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QHash>
#include <QString>
#include <QVector>

#include <array>
#include <functional>

class QAbstractEventDispatcher;
class QTimer;

namespace forkmesh::ui {

// A bounded, payload-free view of the Qt GUI runtime. The heartbeat is cheap
// enough to run for the session; per-event accounting is installed only while
// the footer debug strip is open.
class QtRuntimeDiagnostics final : public QObject
{
public:
    enum EventGroup {
        Input = 0,
        Timer,
        Queued,
        Paint,
        Layout,
        DeferredDelete,
        Other,
        EventGroupCount
    };

    struct EventStats {
        quint64 count = 0;
        qint64 totalUs = 0;
        qint64 maxUs = 0;
        QString slowestReceiverClass;
    };

    struct PaintReceiver {
        QString name;
        quint64 count = 0;
        qint64 totalUs = 0;
    };

    struct Snapshot {
        qint64 sampledAtMs = 0;
        qint64 loopLagMs = 0;
        qint64 averageLoopLagMs = 0;
        qint64 p95LoopLagMs = 0;
        qint64 peakLoopLagMs = 0;
        int eventsPerSecond = 0;
        int paintsPerSecond = 0;
        int layoutsPerSecond = 0;
        int wakeupsPerSecond = 0;
        double dispatcherBusyPercent = 0.0;
        int widgetCount = 0;
        int visibleWidgetCount = 0;
        int windowCount = 0;
        int visibleWindowCount = 0;
        int objectCount = 0;
        int activeTimerCount = 0;
        int timerCount = 0;
        int threadPoolActive = 0;
        int threadPoolMax = 0;
        bool detailed = false;
        std::array<EventStats, EventGroupCount> intervalEvents{};
        std::array<EventStats, EventGroupCount> sessionEvents{};
        QVector<PaintReceiver> topPaintReceivers;
    };

    explicit QtRuntimeDiagnostics(QObject *parent = nullptr);
    ~QtRuntimeDiagnostics() override;

    void setDetailedEnabled(bool enabled);
    bool detailedEnabled() const { return m_detailed; }
    Snapshot snapshot() const { return m_snapshot; }
    QVector<Snapshot> history() const;
    QString report() const;
    void resetSession();

#ifdef FORKMESH_QT_RUNTIME_DIAGNOSTICS_TESTS
    void appendSnapshotForTest(const Snapshot &snapshot)
    {
        appendSnapshot(snapshot);
    }
    void refreshStructureForTest() { refreshStructure(); }
#endif

    // Produces an immediate one-second-style snapshot. Public so tests and the
    // diagnostics dialog can refresh deterministically without waiting a tick.
    void sampleNow();

    static EventGroup groupForEvent(QEvent::Type type);
    static QString eventGroupName(EventGroup group);

    // ForkMeshApplication wraps QApplication::notify and reports the inclusive
    // delivery cost here. This is a no-op unless detailed sampling is active.
    static void recordEventDelivery(const char *receiverClass,
                                    int receiverClassLength,
                                    QEvent::Type eventType, qint64 elapsedUs,
                                    const QString &paintReceiver = QString());

    std::function<void(const Snapshot &)> onSnapshot;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class QtEventDeliveryProbe;
    void heartbeat();
    void refreshStructure();
    void connectDispatcher();
    void disconnectDispatcher();
    void noteDelivery(const char *receiverClass, int receiverClassLength,
                      QEvent::Type eventType, qint64 elapsedUs,
                      const QString &paintReceiver);
    QVector<PaintReceiver> topPaintReceivers() const;
    void appendSnapshot(const Snapshot &snapshot);
    static qint64 percentile95(QVector<qint64> values);

    static QtRuntimeDiagnostics *s_instance;

    QTimer *m_heartbeatTimer = nullptr;
    QTimer *m_sampleTimer = nullptr;
    QElapsedTimer m_clock;
    qint64 m_nextHeartbeatMs = 0;
    qint64 m_lastSampleNs = 0;
    qint64 m_lastStructureRefreshNs = 0;
    qint64 m_dispatchAwakeNs = -1;
    qint64 m_intervalBusyNs = 0;
    int m_intervalWakeups = 0;
    bool m_detailed = false;
    bool m_filterInstalled = false;
    QVector<qint64> m_lagHistory;
    int m_lagHistoryNext = 0;
    QVector<Snapshot> m_history;
    int m_historyNext = 0;
    Snapshot m_snapshot;
    std::array<EventStats, EventGroupCount> m_intervalEvents{};
    std::array<EventStats, EventGroupCount> m_sessionEvents{};
    QHash<QString, PaintReceiver> m_paintReceivers;
    QMetaObject::Connection m_awakeConnection;
    QMetaObject::Connection m_blockConnection;
};

// RAII timing hook kept tiny because every delivered Qt event crosses it.
class QtEventDeliveryProbe final
{
public:
    QtEventDeliveryProbe(QObject *receiver, QEvent *event);
    ~QtEventDeliveryProbe();

private:
    // Class names are copied before delivery because a receiver may delete
    // itself. A fixed buffer avoids allocating a QString for every one of the
    // thousands of events observed each second while detailed mode is open.
    std::array<char, 96> m_receiverClass{};
    int m_receiverClassLength = 0;
    QEvent::Type m_eventType = QEvent::None;
    QString m_paintReceiver;
    QElapsedTimer m_elapsed;
    bool m_active = false;
};

} // namespace forkmesh::ui

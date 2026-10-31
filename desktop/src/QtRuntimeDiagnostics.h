#pragma once

#include <QElapsedTimer>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QHash>
#include <QString>
#include <QVector>

#include <QPointer>

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
    int walkStructureInSlicesForTest(const std::function<void(int)> &between)
    {
        beginStructureWalk(true);
        int slices = 1;
        while (!advanceStructureWalk(m_clock.nsecsElapsed())) {
            if (between)
                between(slices);
            ++slices;
        }
        finishStructureWalk();
        return slices;
    }
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

    struct PaintKey {
        const QMetaObject *meta = nullptr;
        QString objectName;
        bool self = false;
        friend bool operator==(const PaintKey &a, const PaintKey &b)
        {
            return a.meta == b.meta && a.self == b.self
                && a.objectName == b.objectName;
        }
        friend size_t qHash(const PaintKey &key, size_t seed = 0)
        {
            return qHashMulti(seed, key.meta, key.self, key.objectName);
        }
    };

    struct SlowestReceiver {
        std::array<char, 96> name{};
        int length = 0;
        bool dirty = false;
    };

    struct WalkFrame {
        QObject *object = nullptr;
        int next = 0;
    };

    struct StructureWalk {
        bool active = false;
        bool countObjects = false;
        QVector<QPointer<QObject>> roots;
        QVector<bool> rootIncluded;
        int nextRoot = 0;
        QVector<WalkFrame> frames;
        QVector<QPointer<QObject>> savedFrames;
        QVector<int> savedNext;
        int widgets = 0;
        int visibleWidgets = 0;
        int objects = 0;
        int timers = 0;
        int activeTimers = 0;
    };

    void heartbeat();
    void refreshStructure();
    void refreshCheapStructure();
    void beginStructureWalk(bool countObjects);
    bool advanceStructureWalk(qint64 deadlineNs);
    void finishStructureWalk();
    void cancelStructureWalk();
    void continueStructureWalkSlice();
    void connectDispatcher();
    void disconnectDispatcher();
    void noteDelivery(const char *receiverClass, int receiverClassLength,
                      QEvent::Type eventType, qint64 elapsedUs,
                      const QString &paintReceiver);
    void noteProbeDelivery(const char *receiverClass, int receiverClassLength,
                           QEvent::Type eventType, qint64 elapsedUs,
                           int paintSlot, quint32 paintGeneration);
    void noteEventTiming(const char *receiverClass, int receiverClassLength,
                         QEvent::Type eventType, qint64 elapsedUs);
    int paintSlotFor(QObject *receiver);
    int paintSlotForName(const QString &name);
    void tallyPaint(int slot, qint64 elapsedUs);
    void publishSlowestReceivers();
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
    std::array<SlowestReceiver, EventGroupCount> m_intervalSlowest{};
    std::array<SlowestReceiver, EventGroupCount> m_sessionSlowest{};
    QVector<PaintReceiver> m_paintTallies;
    QHash<PaintKey, int> m_paintSlots;
    QHash<QString, int> m_paintNamedSlots;
    int m_otherPaintSlot = -1;
    quint32 m_paintGeneration = 0;
    StructureWalk m_walk;
    QTimer *m_walkTimer = nullptr;
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
    int m_paintSlot = -1;
    quint32 m_paintGeneration = 0;
    QElapsedTimer m_elapsed;
    bool m_active = false;
};

} // namespace forkmesh::ui

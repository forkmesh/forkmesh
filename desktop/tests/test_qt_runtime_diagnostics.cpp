#include "../src/QtRuntimeDiagnostics.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <new>

namespace {

union ReceiverStorage {
    QPushButton button;
    QLabel label;

    ReceiverStorage() {}
    ~ReceiverStorage() {}
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    forkmesh::ui::QtRuntimeDiagnostics diagnostics;
    diagnostics.setDetailedEnabled(true);

    ReceiverStorage storage;
    auto *original = ::new (static_cast<void *>(&storage.button)) QPushButton;
    QLabel *replacement = nullptr;
    QEvent event(QEvent::User);
    {
        forkmesh::ui::QtEventDeliveryProbe probe(original, &event);
        original->~QPushButton();
        replacement = ::new (static_cast<void *>(&storage.label)) QLabel;

        // Ensure this delivery becomes the group's maximum even on a coarse
        // monotonic clock. The replacement deliberately occupies the exact
        // address that the destroyed receiver used.
        QThread::usleep(1000);
    }
    replacement->~QLabel();

    diagnostics.sampleNow();
    const auto stats = diagnostics.snapshot().sessionEvents.at(
        forkmesh::ui::QtRuntimeDiagnostics::Other);
    if (stats.slowestReceiverClass != QStringLiteral("QPushButton")) {
        std::fprintf(stderr,
                     "delivery probe retained '%s' instead of the receiver's "
                     "pre-dispatch class\n",
                     stats.slowestReceiverClass.toUtf8().constData());
        return 1;
    }

    diagnostics.resetSession();
    for (int i = 0; i < 307; ++i) {
        forkmesh::ui::QtRuntimeDiagnostics::Snapshot sample;
        sample.sampledAtMs = i;
        diagnostics.appendSnapshotForTest(sample);
    }
    const auto history = diagnostics.history();
    if (history.size() != 300 || history.constFirst().sampledAtMs != 7 ||
        history.constLast().sampledAtMs != 306) {
        std::fprintf(stderr,
                     "snapshot ring retained %d entries from %lld through "
                     "%lld\n",
                     history.size(),
                     static_cast<long long>(history.isEmpty()
                                                ? -1
                                                : history.constFirst()
                                                      .sampledAtMs),
                     static_cast<long long>(history.isEmpty()
                                                ? -1
                                                : history.constLast()
                                                      .sampledAtMs));
        return 1;
    }

    diagnostics.resetSession();
    QThread *worker = QThread::create([] {
        QTimer workerReceiver;
        QEvent workerEvent(QEvent::User);
        forkmesh::ui::QtEventDeliveryProbe probe(&workerReceiver, &workerEvent);
        QThread::usleep(2000);
    });
    worker->start();
    worker->wait();
    delete worker;
    diagnostics.sampleNow();
    const auto workerStats = diagnostics.snapshot().sessionEvents.at(
        forkmesh::ui::QtRuntimeDiagnostics::Other);
    if (!workerStats.slowestReceiverClass.isEmpty() || workerStats.maxUs != 0) {
        std::fprintf(stderr,
                     "a worker-thread delivery wrote the GUI thread's event "
                     "stats (slowest '%s', %lld us)\n",
                     workerStats.slowestReceiverClass.toUtf8().constData(),
                     static_cast<long long>(workerStats.maxUs));
        return 1;
    }
    {
        QWidget window;
        auto *activeTimer = new QTimer(&window);
        activeTimer->start(60000);
        new QTimer(&window);
        auto *child = new QWidget(&window, Qt::Window);
        new QTimer(child);
        diagnostics.resetSession();
        const auto counted = diagnostics.snapshot();
        diagnostics.resetSession();
        const int baseObjects = counted.objectCount;
        const int baseTimers = counted.timerCount;
        if (baseTimers < 3 || counted.activeTimerCount < 1) {
            std::fprintf(stderr,
                         "object walk counted %d timers (%d active)\n",
                         baseTimers, counted.activeTimerCount);
            return 1;
        }
        new QTimer(&window);
        for (int i = 0; i < 20; ++i)
            diagnostics.refreshStructureForTest();
        if (diagnostics.snapshot().timerCount != baseTimers
            || diagnostics.snapshot().objectCount != baseObjects) {
            std::fprintf(stderr,
                         "the whole-object walk ran again within a minute\n");
            return 1;
        }
        diagnostics.resetSession();
        if (diagnostics.snapshot().timerCount != baseTimers + 1
            || diagnostics.snapshot().objectCount != baseObjects + 1) {
            std::fprintf(stderr,
                         "a reset did not recount: %d timers, %d objects\n",
                         diagnostics.snapshot().timerCount,
                         diagnostics.snapshot().objectCount);
            return 1;
        }
    }

    {
        QWidget window;
        QWidget *doomed = nullptr;
        for (int i = 0; i < 40; ++i) {
            auto *row = new QWidget(&window);
            for (int j = 0; j < 20; ++j)
                new QTimer(row);
            if (i == 30)
                doomed = row;
        }
        diagnostics.resetSession();
        const auto whole = diagnostics.snapshot();
        const int slices = diagnostics.walkStructureInSlicesForTest({});
        const auto sliced = diagnostics.snapshot();
        if (slices < 2 || sliced.objectCount != whole.objectCount ||
            sliced.timerCount != whole.timerCount ||
            sliced.widgetCount != whole.widgetCount) {
            std::fprintf(stderr,
                         "a sliced walk counted %d objects / %d timers in %d "
                         "slices; the whole walk counted %d / %d\n",
                         sliced.objectCount, sliced.timerCount, slices,
                         whole.objectCount, whole.timerCount);
            return 1;
        }
        diagnostics.walkStructureInSlicesForTest([&](int slice) {
            if (slice == 1 && doomed) {
                delete doomed;
                doomed = nullptr;
            }
        });
        const auto after = diagnostics.snapshot();
        if (doomed || after.objectCount > whole.objectCount ||
            after.objectCount < whole.objectCount - 21) {
            std::fprintf(stderr,
                         "a walk that lost a subtree mid-way counted %d "
                         "objects (whole walk %d)\n",
                         after.objectCount, whole.objectCount);
            return 1;
        }
    }

    {
        diagnostics.resetSession();
        QWidget host;
        host.setObjectName(QStringLiteral("paintHost"));
        auto *spinner = new QLabel(&host);
        QEvent paint(QEvent::Paint);
        for (int i = 0; i < 3; ++i)
            forkmesh::ui::QtEventDeliveryProbe probe(spinner, &paint);
        {
            forkmesh::ui::QtEventDeliveryProbe probe(&host, &paint);
        }
        QEvent other(QEvent::User);
        {
            forkmesh::ui::QtEventDeliveryProbe probe(spinner, &other);
        }
        diagnostics.sampleNow();
        const auto top = diagnostics.snapshot().topPaintReceivers;
        const QString report = diagnostics.report();
        if (top.size() != 2 ||
            top.at(0).name != QStringLiteral("QLabel in paintHost") ||
            top.at(0).count != 3 ||
            top.at(1).name != QStringLiteral("QWidget paintHost") ||
            top.at(1).count != 1 ||
            !report.contains(QStringLiteral("Most painted widgets")) ||
            !report.contains(QStringLiteral("QLabel in paintHost: 3 paints"))) {
            std::fprintf(stderr,
                         "paint receivers were not tallied by widget: %s\n",
                         report.toUtf8().constData());
            return 1;
        }
        diagnostics.resetSession();
        diagnostics.sampleNow();
        if (!diagnostics.snapshot().topPaintReceivers.isEmpty()) {
            std::fprintf(stderr, "a reset kept the paint receiver tallies\n");
            return 1;
        }
    }

    return 0;
}

#include "../src/ActionTelemetry.h"
#include "../src/BackgroundActivity.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

std::atomic<quint64> openTicket{0};
std::atomic<int> retiredAtExit{0};
std::atomic<int> liveListeners{0};
QTemporaryDir *sandbox = nullptr;

struct ListenerGuard {
    ListenerGuard() { liveListeners.fetch_add(1); }
    ListenerGuard(const ListenerGuard &) { liveListeners.fetch_add(1); }
    ~ListenerGuard() { liveListeners.fetch_sub(1); }
};

QString telemetryPath()
{
    return sandbox->filePath(QStringLiteral("actions.jsonl"));
}

void retireAfterStaticTeardown()
{
    forkmesh::BackgroundActivity::end(openTicket.load(), QStringLiteral("cancelled"));
    forkmesh::ActionTelemetry::instant(QStringLiteral("late"), QStringLiteral("after the writer stopped"));
    QFile file(telemetryPath());
    const bool flushed = file.open(QIODevice::ReadOnly) &&
                         file.readAll().contains("\"exit-probe\"");
    file.close();
    int failures = 0;
    if (liveListeners.load() != 1) {
        std::printf("FAIL: static teardown leaves the background bus and its listener alive\n");
        ++failures;
    }
    if (retiredAtExit.load() != 1) {
        std::printf("FAIL: a ticket retired from an exit handler still reaches the listener\n");
        ++failures;
    }
    if (!flushed) {
        std::printf("FAIL: the telemetry writer flushes its records at exit\n");
        ++failures;
    }
    delete sandbox;
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}

}

int main(int argc, char **argv)
{
    std::atexit(retireAfterStaticTeardown);
    QCoreApplication app(argc, argv);
    sandbox = new QTemporaryDir;
    if (!sandbox->isValid()) {
        std::printf("FAIL: no temporary directory\n");
        return 1;
    }
    forkmesh::ActionTelemetry::initialize(telemetryPath());
    forkmesh::BackgroundActivity::setListener(
        [guard = ListenerGuard()](const forkmesh::BackgroundTicket &ticket, bool started) {
            Q_UNUSED(guard);
            if (!started && ticket.outcome == QLatin1String("cancelled"))
                retiredAtExit.fetch_add(1);
        });
    openTicket = forkmesh::BackgroundActivity::begin(QStringLiteral("exit-probe"),
                                                    QStringLiteral("left open at exit"));
    return 0;
}

#include "DesktopPowerAlert.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PowerSourceReader.h"

#include <QDateTime>
#include <QTimer>

namespace poweralert = forkmesh::poweralert;

namespace {

class PowerAlertState : public QObject
{
public:
    explicit PowerAlertState(QObject *parent) : QObject(parent), reader(this)
    {
        setObjectName(QStringLiteral("forkmeshPowerAlertState"));
    }

    poweralert::PowerSourceReader reader;
    poweralert::Watch watch;
    poweralert::Reading last;
    QTimer timer;
};

}

void MainWindow::startPowerAlertWatch()
{
    if (findChild<QObject *>(QStringLiteral("forkmeshPowerAlertState"),
                             Qt::FindDirectChildrenOnly))
        return;
    auto *state = new PowerAlertState(this);
    state->timer.setInterval(poweralert::kPollMs);
    connect(&state->timer, &QTimer::timeout, state, [this, state] {
        const poweralert::Reading reading = state->reader.read();
        state->last = reading;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        switch (state->watch.observe(reading.source, now)) {
        case poweralert::Change::LostPower:
            raisePowerAlert(poweralert::lostSource(machineNodeName(), reading, now));
            break;
        case poweralert::Change::Restored:
            raisePowerAlert(poweralert::restoredSource(
                machineNodeName(), state->watch.lastOutageMs(), now));
            break;
        case poweralert::Change::None:
            break;
        }
    });
    state->timer.start();
    state->last = state->reader.read();
    state->watch.observe(state->last.source, QDateTime::currentMSecsSinceEpoch());
}

poweralert::Reading MainWindow::currentPowerReading()
{
    if (QObject *state = findChild<QObject *>(QStringLiteral("forkmeshPowerAlertState"),
                                              Qt::FindDirectChildrenOnly))
        return static_cast<PowerAlertState *>(state)->last;
    return poweralert::PowerSourceReader(nullptr).read();
}

void MainWindow::raisePowerAlert(const forkmesh::pingforward::Source &source)
{
    logSystem(source.title + QStringLiteral(": ") + source.body);
    AppNotification item;
    item.title = source.title;
    item.body = source.body;
    item.warning = source.warning;
    item.kind = source.kind;
    item.timestampMs = source.timestampMs;
    recordNotification(item);
}

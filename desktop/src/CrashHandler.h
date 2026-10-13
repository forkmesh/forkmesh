#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;


namespace forkmesh {

void installCrashHandler(const QString &crashLogPath = QString(),
                         const QString &mainLogPath = QString());

// Turn an external stop request (SIGTERM/SIGINT/SIGHUP/SIGQUIT) into an orderly
// shutdown instead of an abrupt death. Call once, from the GUI thread, after the
// application object and main window exist: onTerminate then runs on the event
// loop for the first such signal — close the window and quit, so closeEvent()
// still saves geometry, chat history, the pings journal and the network log. A
// second signal, a signal arriving before this is armed, or a shutdown that
// takes longer than the internal deadline still terminates the process
// immediately. `context` owns the notifier and scopes the callback's lifetime.
void enableGracefulTerminationShutdown(QObject *context,
                                       std::function<void()> onTerminate);

inline constexpr int kCrashContextCapacityBytes = 4095;

inline QString fitCrashContext(const QString &head, const QString &tail,
                               int capacityBytes = kCrashContextCapacityBytes)
{
    const QByteArray headBytes = head.toUtf8();
    const qsizetype room = capacityBytes - headBytes.size();
    if (room <= 0)
        return QString::fromUtf8(headBytes.left(capacityBytes));
    QByteArray tailBytes = tail.toUtf8();
    if (tailBytes.size() > room) {
        tailBytes = tailBytes.right(room);
        const qsizetype lineStart = tailBytes.indexOf('\n');
        tailBytes = lineStart < 0 ? QByteArray() : tailBytes.mid(lineStart + 1);
    }
    return QString::fromUtf8(headBytes + tailBytes);
}

void setCrashContext(const QString &context);

void setTerminationSignalSurvivalEnabled(bool enabled);

bool terminationSignalReceived();

void logDiagnosticEvent(const QString &context, const QString &details);

void logCaughtFault(const QString &context, const QString &what);

} // namespace forkmesh

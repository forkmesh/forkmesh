#pragma once

#include <QString>
#include <QtGlobal>

#include <functional>

namespace forkmesh {

bool isPlatformSizeHintNoise(const QString &message);

bool isFontDatabaseNoise(const QString &message);

void installPlatformLogFilter();

using AppLogSink =
    std::function<void(QtMsgType, const QString &, const QString &, int)>;

// Send every surviving message to `sink` — the app's Log view — instead of the
// terminal. This is where the app's own qInfo() progress lines (catalog
// publishes, mirror syncs, restart phases) and Qt's own warnings ("QProcess:
// Destroyed while process ("git") is still running.") belong: the desktop is
// launched from a terminal nobody watches, while the Log view is on screen and
// persisted to disk. keepConsoleEcho leaves the terminal copy in place as well,
	// which is useful for an operator who deliberately launched the desktop from
	// a terminal and wants both views.
// Fatal messages always reach the console (and Qt's abort) regardless: they are
// the last thing the process says and the app log never gets flushed after one.
// A sink that logs re-entrantly is forwarded to the console instead, so it
// cannot loop.
void setAppLogSink(AppLogSink sink, bool keepConsoleEcho);

void clearAppLogSink();

} // namespace forkmesh

#include "PlatformLogFilter.h"

#include <QLatin1String>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>

namespace forkmesh {
namespace {

QtMessageHandler g_previousMessageHandler = nullptr;

QMutex g_sinkMutex;
AppLogSink g_sink;
bool g_keepConsoleEcho = false;
thread_local bool g_insideSink = false;

bool routeToAppLog(QtMsgType type, const QMessageLogContext &context,
                   const QString &message)
{
    if (type == QtFatalMsg || g_insideSink)
        return false;
    QMutexLocker lock(&g_sinkMutex);
    if (!g_sink)
        return false;
    struct ReentryGuard {
        ReentryGuard() { g_insideSink = true; }
        ~ReentryGuard() { g_insideSink = false; }
    } guard;
    g_sink(type, message,
           context.file ? QString::fromUtf8(context.file) : QString(),
           context.line);
    return !g_keepConsoleEcho;
}

void filterPlatformNoise(QtMsgType type, const QMessageLogContext &context,
                         const QString &message)
{
    if (isPlatformSizeHintNoise(message))
        return;
    if (isFontDatabaseNoise(message))
        return;
    if (routeToAppLog(type, context, message))
        return;
    if (g_previousMessageHandler) {
        g_previousMessageHandler(type, context, message);
        return;
    }
    fprintf(stderr, "%s\n",
            qFormatLogMessage(type, context, message).toLocal8Bit().constData());
    fflush(stderr);
    if (type == QtFatalMsg)
        abort();
}

} // namespace

bool isPlatformSizeHintNoise(const QString &message)
{
    return message.contains(QLatin1String("propagateSizeHints"));
}

bool isFontDatabaseNoise(const QString &message)
{
    return message.startsWith(QLatin1String("OpenType support missing for"));
}

void installPlatformLogFilter()
{
    g_previousMessageHandler = qInstallMessageHandler(filterPlatformNoise);
}

void setAppLogSink(AppLogSink sink, bool keepConsoleEcho)
{
    QMutexLocker lock(&g_sinkMutex);
    g_sink = std::move(sink);
    g_keepConsoleEcho = keepConsoleEcho;
}

void clearAppLogSink()
{
    QMutexLocker lock(&g_sinkMutex);
    g_sink = {};
    g_keepConsoleEcho = false;
}

} // namespace forkmesh

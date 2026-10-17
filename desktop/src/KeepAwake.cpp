#include "KeepAwake.h"

#include "BackgroundMode.h"
#include "SingleInstance.h"

#include <QDebug>

#if defined(Q_OS_LINUX) && defined(FORKMESH_HAVE_PORTAL)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#endif

namespace forkmesh::keepawake {

struct SleepInhibitor::Lock {
#if defined(Q_OS_LINUX) && defined(FORKMESH_HAVE_PORTAL)
    QDBusUnixFileDescriptor fd;
#endif
};

SleepInhibitor::SleepInhibitor(std::function<bool()> wanted, QObject *parent)
    : QObject(parent), m_wanted(std::move(wanted))
{
    m_timer.setInterval(kRetryMs);
    connect(&m_timer, &QTimer::timeout, this, [this] { update(); });
    m_timer.start();
    update();
}

SleepInhibitor::~SleepInhibitor() = default;

bool SleepInhibitor::held() const
{
    return m_lock != nullptr;
}

void SleepInhibitor::update()
{
    const bool wanted = m_wanted && m_wanted();
    if (wanted && !held())
        take();
    else if (!wanted && held())
        release();
}

void SleepInhibitor::release()
{
    m_lock.reset();
    qInfo().noquote() << "ForkMesh no longer keeps the computer awake.";
}

void SleepInhibitor::take()
{
#if defined(Q_OS_LINUX) && defined(FORKMESH_HAVE_PORTAL)
    if (m_pending)
        return;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Inhibit"));
    call << QStringLiteral("sleep") << QStringLiteral("ForkMesh")
         << QStringLiteral("ForkMesh serves your phones and agents") << QStringLiteral("block");
    m_pending = true;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        m_pending = false;
        const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *done;
        if (reply.isError() || !reply.value().isValid()) {
            const QString error = reply.isError() ? reply.error().message()
                                                  : QStringLiteral("logind returned no lock");
            if (error != m_lastError)
                qWarning().noquote()
                    << "ForkMesh could not keep the computer awake:" << error
                    << "- it retries every minute; Settings > General > STARTUP > Never let "
                       "this computer go to sleep turns sleep off for good.";
            m_lastError = error;
            return;
        }
        if (!(m_wanted && m_wanted()))
            return;
        m_lock = std::make_unique<Lock>();
        m_lock->fd = reply.value();
        m_lastError.clear();
        qInfo().noquote() << "ForkMesh keeps the computer awake while it runs (logind sleep "
                             "inhibitor).";
    });
#endif
}

void holdWhileRunning(QObject *owner)
{
    new SleepInhibitor(
        [] {
            return isBackgroundInstance() ||
                   (background::supported() && background::keepRunning());
        },
        owner);
}

} // namespace forkmesh::keepawake

#include "PowerSourceReader.h"

#include <QDir>
#include <QFile>

#if defined(Q_OS_LINUX) && defined(FORKMESH_HAVE_PORTAL)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace forkmesh::poweralert {

namespace {

#if defined(Q_OS_LINUX)
QString readAttribute(const QString &dir, const char *name)
{
    QFile file(dir + QLatin1Char('/') + QLatin1String(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.read(64)).trimmed();
}

Reading readSysfs()
{
    const QString root = QStringLiteral("/sys/class/power_supply");
    QList<Supply> supplies;
    const QStringList names =
        QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &name : names) {
        const QString dir = root + QLatin1Char('/') + name;
        supplies.append({readAttribute(dir, "type"), readAttribute(dir, "scope"),
                         readAttribute(dir, "online"), readAttribute(dir, "status"),
                         readAttribute(dir, "capacity")});
    }
    return fromSupplies(supplies);
}
#endif

#if defined(Q_OS_MACOS)
Reading readIOKit()
{
    Reading reading;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info)
        return reading;
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    if (type) {
        if (CFStringCompare(type, CFSTR(kIOPMACPowerKey), 0) == kCFCompareEqualTo)
            reading.source = Source::Mains;
        else if (CFStringCompare(type, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo ||
                 CFStringCompare(type, CFSTR(kIOPMUPSPowerKey), 0) == kCFCompareEqualTo)
            reading.source = Source::Battery;
    }
    if (CFArrayRef sources = IOPSCopyPowerSourcesList(info)) {
        long current = 0;
        long maximum = 0;
        for (CFIndex i = 0; i < CFArrayGetCount(sources); ++i) {
            CFDictionaryRef description =
                IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(sources, i));
            if (!description)
                continue;
            const auto kind = static_cast<CFStringRef>(
                CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey)));
            if (!kind || CFStringCompare(kind, CFSTR(kIOPSInternalBatteryType), 0) !=
                             kCFCompareEqualTo)
                continue;
            int now = -1;
            int max = -1;
            const auto nowRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey)));
            const auto maxRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)));
            if (nowRef && maxRef && CFNumberGetValue(nowRef, kCFNumberIntType, &now) &&
                CFNumberGetValue(maxRef, kCFNumberIntType, &max) && now >= 0 && max > 0) {
                current += now;
                maximum += max;
            }
        }
        CFRelease(sources);
        if (maximum > 0)
            reading.percent = int(qBound(0L, (current * 100 + maximum / 2) / maximum, 100L));
    }
    CFRelease(info);
    return reading;
}
#endif

#if defined(Q_OS_WIN)
Reading readWin32()
{
    Reading reading;
    SYSTEM_POWER_STATUS status;
    if (!GetSystemPowerStatus(&status))
        return reading;
    if (status.ACLineStatus == 1)
        reading.source = Source::Mains;
    else if (status.ACLineStatus == 0)
        reading.source = Source::Battery;
    if (status.BatteryLifePercent <= 100)
        reading.percent = status.BatteryLifePercent;
    return reading;
}
#endif

}

PowerSourceReader::PowerSourceReader(QObject *owner) : m_owner(owner) {}

Reading PowerSourceReader::read()
{
#if defined(Q_OS_LINUX)
    refreshUPower();
    return combine(m_upowerOnBattery, readSysfs());
#elif defined(Q_OS_MACOS)
    return readIOKit();
#elif defined(Q_OS_WIN)
    return readWin32();
#else
    return {};
#endif
}

void PowerSourceReader::refreshUPower()
{
#if defined(Q_OS_LINUX) && defined(FORKMESH_HAVE_PORTAL)
    if (m_upowerPending || m_upowerMissing || !m_owner)
        return;
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_upowerMissing = true;
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.UPower"), QStringLiteral("/org/freedesktop/UPower"),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    call << QStringLiteral("org.freedesktop.UPower") << QStringLiteral("OnBattery");
    m_upowerPending = true;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 2000), m_owner);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, m_owner,
                     [this](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        m_upowerPending = false;
        const QDBusPendingReply<QDBusVariant> reply = *done;
        if (reply.isError()) {
            m_upowerOnBattery = -1;
            if (reply.error().type() == QDBusError::ServiceUnknown)
                m_upowerMissing = true;
            return;
        }
        m_upowerOnBattery = reply.value().variant().toBool() ? 1 : 0;
    });
#endif
}

} // namespace forkmesh::poweralert

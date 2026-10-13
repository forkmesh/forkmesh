#include "DesktopPowerAlert.h"

namespace forkmesh::poweralert {

namespace {

QString token(const QString &value)
{
    return value.trimmed().toLower();
}

bool isBatteryType(const QString &type)
{
    const QString kind = token(type);
    return kind == QLatin1String("battery") || kind == QLatin1String("ups");
}

bool isSystemSupply(const Supply &supply)
{
    return token(supply.scope) != QLatin1String("device");
}

QString nodeName(const QString &node)
{
    const QString name = node.trimmed();
    return name.isEmpty() ? QStringLiteral("Your desktop") : name;
}

}

QString lostKind()
{
    return QStringLiteral("power-battery");
}

QString restoredKind()
{
    return QStringLiteral("power-restored");
}

QString settingKey()
{
    return QStringLiteral("notifications/phonePowerAlarm");
}

bool isPowerKind(const QString &kind)
{
    return kind == lostKind() || kind == restoredKind();
}

Reading fromSupplies(const QList<Supply> &supplies)
{
    Reading reading;
    bool mainsOnline = false;
    bool mainsPresent = false;
    bool discharging = false;
    bool charging = false;
    bool batteryPresent = false;
    for (const Supply &supply : supplies) {
        if (!isSystemSupply(supply))
            continue;
        if (isBatteryType(supply.type)) {
            batteryPresent = true;
            const QString status = token(supply.status);
            if (status == QLatin1String("discharging"))
                discharging = true;
            else if (status == QLatin1String("charging") ||
                     status == QLatin1String("full"))
                charging = true;
            bool ok = false;
            const int percent = supply.capacity.trimmed().toInt(&ok);
            if (ok && percent >= 0 && percent <= 100 && reading.percent < 0)
                reading.percent = percent;
            continue;
        }
        if (token(supply.type).isEmpty())
            continue;
        mainsPresent = true;
        if (supply.online.trimmed() == QLatin1String("1"))
            mainsOnline = true;
    }
    if (mainsOnline)
        reading.source = Source::Mains;
    else if (discharging)
        reading.source = Source::Battery;
    else if (mainsPresent && batteryPresent)
        reading.source = Source::Battery;
    else if (charging)
        reading.source = Source::Mains;
    return reading;
}

Reading combine(int upowerOnBattery, const Reading &supplies)
{
    Reading reading = supplies;
    if (upowerOnBattery == 1)
        reading.source = Source::Battery;
    else if (upowerOnBattery == 0)
        reading.source = Source::Mains;
    return reading;
}

pingforward::Source lostSource(const QString &node, const Reading &reading,
                               qint64 nowMs)
{
    pingforward::Source source;
    source.timestampMs = nowMs;
    source.node = node;
    source.kind = lostKind();
    source.warning = true;
    source.title = QStringLiteral("%1 lost power").arg(nodeName(node));
    source.body = reading.percent >= 0
                      ? QStringLiteral("Running on battery, %1% left. Plug it "
                                       "back in or save your work.")
                            .arg(reading.percent)
                      : QStringLiteral("Running on battery. Plug it back in "
                                       "or save your work.");
    return source;
}

pingforward::Source restoredSource(const QString &node, qint64 outageMs,
                                   qint64 nowMs)
{
    pingforward::Source source;
    source.timestampMs = nowMs;
    source.node = node;
    source.kind = restoredKind();
    source.title = QStringLiteral("%1 is back on mains power").arg(nodeName(node));
    source.body = QStringLiteral("Power came back after %1 on battery.")
                      .arg(pingforward::offlineSpan(outageMs));
    return source;
}

Change Watch::observe(Source source, qint64 nowMs)
{
    if (source == Source::Unknown)
        return Change::None;
    if (source == Source::Mains) {
        m_seenMains = true;
        const qint64 since = m_batterySinceMs;
        m_batterySinceMs = 0;
        if (!m_alarmed)
            return Change::None;
        m_alarmed = false;
        m_lastOutageMs = since > 0 ? qMax<qint64>(0, nowMs - since) : 0;
        return Change::Restored;
    }
    if (!m_seenMains)
        return Change::None;
    if (m_batterySinceMs <= 0)
        m_batterySinceMs = nowMs;
    if (m_alarmed || nowMs - m_batterySinceMs < kConfirmMs)
        return Change::None;
    m_alarmed = true;
    return Change::LostPower;
}

} // namespace forkmesh::poweralert

#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace forkmesh::usblink {

inline QString defaultSysRoot() { return QStringLiteral("/sys/bus/usb/devices"); }

struct Port {
    QString id;
    QString peer;
    QString controller;
    QString place;
    QString occupant;
    int mbps = 0;
    bool usbC = false;
};

struct Finding {
    QString id;
    QString name;
    int speedMbps = 0;
    int capableMbps = 0;
    int availableMbps = 0;
    int portMbps = 0;
    bool throughHub = false;
    QString controller;
    QVector<Port> fasterPorts;
};

inline QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll()).trimmed();
}

inline int readSpeedMbps(const QString &deviceDir)
{
    bool ok = false;
    const double value = readText(QDir(deviceDir).filePath(QStringLiteral("speed"))).toDouble(&ok);
    return ok ? int(value) : 0;
}

inline bool isStorage(const QString &deviceDir)
{
    const QDir dir(deviceDir);
    const QString prefix = QFileInfo(deviceDir).fileName() + QLatin1Char(':');
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.startsWith(prefix) &&
            readText(dir.filePath(name + QStringLiteral("/bInterfaceClass"))) == QLatin1String("08"))
            return true;
    }
    return false;
}

inline bool uasCapable(const QByteArray &descriptors)
{
    int offset = 0;
    while (offset + 2 <= descriptors.size()) {
        const int length = quint8(descriptors.at(offset));
        if (length < 2 || offset + length > descriptors.size())
            break;
        if (quint8(descriptors.at(offset + 1)) == 4 && length >= 9 &&
            quint8(descriptors.at(offset + 5)) == 0x08 &&
            quint8(descriptors.at(offset + 7)) == 0x62)
            return true;
        offset += length;
    }
    return false;
}

inline int capabilityFromName(const QString &name)
{
    const QString text = name.toLower();
    if (text.contains(QRegularExpression(QStringLiteral("gen\\s*2\\s*x\\s*2|20\\s*gbps|usb\\s*4|thunderbolt"))))
        return 20000;
    if (text.contains(QRegularExpression(QStringLiteral("gen\\s*2|3\\.[12]|10\\s*gbps"))))
        return 10000;
    if (text.contains(QRegularExpression(QStringLiteral("3\\.0|gen\\s*1|superspeed|5\\s*gbps"))))
        return 5000;
    return 0;
}

inline int capability(const QString &name, bool uas, const QString &version)
{
    bool ok = false;
    const double usb = version.trimmed().toDouble(&ok);
    int best = capabilityFromName(name);
    if (uas || (ok && usb >= 3.0))
        best = qMax(best, 5000);
    return best;
}

inline int bosMbps(const QByteArray &bos)
{
    int best = 0;
    int offset = 0;
    while (offset + 3 <= bos.size()) {
        const int length = quint8(bos.at(offset));
        if (length < 2 || offset + length > bos.size())
            break;
        const bool capabilityDescriptor = quint8(bos.at(offset + 1)) == 0x10;
        const int type = quint8(bos.at(offset + 2));
        if (capabilityDescriptor && type == 0x03)
            best = qMax(best, 5000);
        if (capabilityDescriptor && type == 0x0a && length >= 12) {
            const int count = (quint8(bos.at(offset + 4)) & 0x1f) + 1;
            for (int i = 0; i < count && 16 + 4 * i <= length; ++i) {
                const int at = offset + 12 + 4 * i;
                const int exponent = (quint8(bos.at(at)) >> 4) & 0x3;
                const int mantissa = quint8(bos.at(at + 2)) | (quint8(bos.at(at + 3)) << 8);
                if (exponent == 3)
                    best = qMax(best, mantissa * 1000);
                else if (exponent == 2)
                    best = qMax(best, mantissa);
            }
        }
        offset += length;
    }
    return best;
}

inline int fastestHostMbps(const QString &sysRoot)
{
    int best = 0;
    const QDir dir(sysRoot);
    for (const QString &name : dir.entryList({QStringLiteral("usb*")},
                                             QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
        best = qMax(best, readSpeedMbps(dir.filePath(name)));
    return best;
}

inline QString deviceName(const QString &deviceDir)
{
    return (readText(deviceDir + QStringLiteral("/manufacturer")) + QLatin1Char(' ') +
            readText(deviceDir + QStringLiteral("/product")))
        .simplified();
}

inline QString busOf(const QString &portId)
{
    return portId.mid(3).section(QLatin1Char('-'), 0, 0);
}

inline QString deviceOnPort(const QString &portId)
{
    return busOf(portId) + QLatin1Char('-') + portId.section(QStringLiteral("-port"), 1);
}

inline QString rootPortId(const QString &deviceId)
{
    return QStringLiteral("usb") + deviceId.section(QLatin1Char('-'), 0, 0) +
           QStringLiteral("-port") +
           deviceId.section(QLatin1Char('-'), 1).section(QLatin1Char('.'), 0, 0);
}

inline QString hubPath(const QString &sysRoot, const QString &portId)
{
    return QDir(sysRoot).filePath(QStringLiteral("usb") + busOf(portId));
}

inline QString portPath(const QString &sysRoot, const QString &portId)
{
    return hubPath(sysRoot, portId) + QLatin1Char('/') + busOf(portId) +
           QStringLiteral("-0:1.0/") + portId;
}

inline QString controllerOf(const QString &hubDir)
{
    const QString real = QFileInfo(hubDir).canonicalFilePath();
    return real.isEmpty() ? QString() : QFileInfo(real).absolutePath();
}

inline QString peerOf(const QString &portDir)
{
    const QString target = QFileInfo(portDir + QStringLiteral("/peer")).symLinkTarget();
    return target.isEmpty() ? QString() : QFileInfo(target).fileName();
}

inline bool hasConnector(const QString &portDir)
{
    const QFileInfo link(portDir + QStringLiteral("/connector"));
    return link.exists() || link.isSymLink();
}

inline QString rawPlace(const QString &portDir)
{
    const QString base = portDir + QStringLiteral("/physical_location/");
    const QString panel = readText(base + QStringLiteral("panel"));
    if (panel.isEmpty() || panel == QLatin1String("unknown"))
        return QString();
    return panel + QLatin1Char('|') + readText(base + QStringLiteral("vertical_position")) +
           QLatin1Char('|') + readText(base + QStringLiteral("horizontal_position"));
}

inline QString placeText(const QString &raw)
{
    const QStringList parts = raw.split(QLatin1Char('|'));
    const QString panel = parts.value(0);
    QString text;
    if (panel == QLatin1String("left") || panel == QLatin1String("right"))
        text = QStringLiteral("on the %1 side").arg(panel);
    else if (!panel.isEmpty())
        text = QStringLiteral("on the %1").arg(panel);
    QStringList spot;
    for (const QString &part : {parts.value(1), parts.value(2)})
        if (!part.isEmpty() && part != QLatin1String("center"))
            spot << part;
    if (!text.isEmpty() && !spot.isEmpty())
        text += QStringLiteral(", ") + spot.join(QLatin1Char(' '));
    return text;
}

inline QVector<Port> rootPorts(const QString &sysRoot)
{
    QVector<Port> ports;
    const QDir dir(sysRoot);
    for (const QString &hub : dir.entryList({QStringLiteral("usb*")},
                                            QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const QString hubDir = dir.filePath(hub);
        const QDir iface(hubDir + QLatin1Char('/') + hub.mid(3) + QStringLiteral("-0:1.0"));
        for (const QString &id : iface.entryList({hub + QStringLiteral("-port*")},
                                                 QDir::Dirs | QDir::NoDotAndDotDot |
                                                     QDir::System)) {
            const QString path = iface.filePath(id);
            const QString type = readText(path + QStringLiteral("/connect_type"));
            if (type == QLatin1String("not used") || type == QLatin1String("hardwired"))
                continue;
            Port port;
            port.id = id;
            port.mbps = readSpeedMbps(hubDir);
            port.controller = controllerOf(hubDir);
            port.peer = peerOf(path);
            const QString peerDir = port.peer.isEmpty() ? QString() : portPath(sysRoot, port.peer);
            port.usbC = hasConnector(path) || (!peerDir.isEmpty() && hasConnector(peerDir));
            port.place = rawPlace(path);
            if (port.place.isEmpty() && !peerDir.isEmpty())
                port.place = rawPlace(peerDir);
            for (const QString &onPort : {id, port.peer}) {
                if (onPort.isEmpty() || !port.occupant.isEmpty())
                    continue;
                const QString device = dir.filePath(deviceOnPort(onPort));
                if (QFileInfo(device).isDir()) {
                    const QString name = deviceName(device);
                    port.occupant = name.isEmpty() ? QStringLiteral("a USB device") : name;
                }
            }
            ports.append(port);
        }
    }
    QStringList places;
    int placed = 0;
    for (const Port &port : ports) {
        if (port.place.isEmpty())
            continue;
        ++placed;
        if (!places.contains(port.place))
            places << port.place;
    }
    const bool placeholder = placed > 1 && places.size() == 1;
    for (Port &port : ports)
        port.place = placeholder ? QString() : placeText(port.place);
    return ports;
}

inline QVector<Finding> scan(const QString &sysRoot = defaultSysRoot())
{
    QVector<Finding> findings;
    const QDir dir(sysRoot);
    const int host = fastestHostMbps(sysRoot);
    const QVector<Port> ports = rootPorts(sysRoot);
    for (const QString &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        if (id.startsWith(QLatin1String("usb")) || id.contains(QLatin1Char(':')))
            continue;
        const QString path = dir.filePath(id);
        if (!isStorage(path))
            continue;
        const QString name = deviceName(path);
        QFile descriptors(path + QStringLiteral("/descriptors"));
        const QByteArray raw = descriptors.open(QIODevice::ReadOnly) ? descriptors.readAll()
                                                                    : QByteArray();
        QFile bos(path + QStringLiteral("/bos_descriptors"));
        const int capable =
            qMax(capability(name, uasCapable(raw), readText(path + QStringLiteral("/version"))),
                 bosMbps(bos.open(QIODevice::ReadOnly) ? bos.readAll() : QByteArray()));
        const int speed = readSpeedMbps(path);
        const int available = qMin(capable, host);
        if (capable <= 0 || speed <= 0 || speed >= available)
            continue;
        Finding finding;
        finding.id = id;
        finding.name = name.isEmpty() ? QStringLiteral("USB drive") : name;
        finding.speedMbps = speed;
        finding.capableMbps = capable;
        finding.availableMbps = available;
        const QString portId = rootPortId(id);
        const QString peer = peerOf(portPath(sysRoot, portId));
        finding.throughHub = id.contains(QLatin1Char('.'));
        finding.controller = controllerOf(hubPath(sysRoot, portId));
        finding.portMbps = readSpeedMbps(hubPath(sysRoot, portId));
        if (!peer.isEmpty())
            finding.portMbps = qMax(finding.portMbps, readSpeedMbps(hubPath(sysRoot, peer)));
        for (const Port &port : ports)
            if (port.mbps >= available && port.id != portId && port.id != peer)
                finding.fasterPorts.append(port);
        std::stable_sort(finding.fasterPorts.begin(), finding.fasterPorts.end(),
                         [](const Port &a, const Port &b) {
                             const auto rank = [](const Port &port) {
                                 return (port.occupant.isEmpty() ? 0 : 4) +
                                        (port.usbC ? 0 : 2) + (port.place.isEmpty() ? 1 : 0);
                             };
                             return rank(a) < rank(b);
                         });
        findings.append(finding);
    }
    return findings;
}

inline QString speedText(int mbps)
{
    if (mbps <= 0)
        return QStringLiteral("unknown speed");
    if (mbps < 1000)
        return mbps == 480 ? QStringLiteral("480 Mbps (USB 2.0)")
                           : QStringLiteral("%1 Mbps").arg(mbps);
    return QStringLiteral("%1 Gbps").arg(mbps / 1000);
}

inline QString message(const Finding &finding)
{
    const QString target = speedText(finding.availableMbps);
    QString text = QStringLiteral("%1 is connected at %2 but this computer can run it at %3.")
                       .arg(finding.name, speedText(finding.speedMbps), target);
    const QString plug =
        QStringLiteral(" Shut down, then plug the drive straight into %1, not through a hub.");
    if (finding.portMbps >= finding.availableMbps) {
        if (finding.throughHub)
            return text +
                   QStringLiteral(" It goes through a hub, and the computer port behind that "
                                  "hub does %1.")
                       .arg(target) +
                   plug.arg(QStringLiteral("that port"));
        return text + QStringLiteral(" Its port already does %1, so the cable is the likely "
                                     "limit. Shut down, then connect it with the cable that "
                                     "came with the drive.")
                          .arg(target);
    }
    if (finding.throughHub)
        text += QStringLiteral(" It goes through a hub.");
    else if (finding.portMbps > 0)
        text += QStringLiteral(" Its port only does %1.").arg(speedText(finding.portMbps));

    QVector<Port> free;
    QStringList busy;
    for (const Port &port : finding.fasterPorts) {
        if (port.occupant.isEmpty())
            free.append(port);
        else if (!busy.contains(port.occupant))
            busy << port.occupant;
    }
    if (free.isEmpty() && busy.isEmpty())
        return text + plug.arg(QStringLiteral("a faster port on the computer (a rear USB-C or "
                                              "%1 port)")
                                   .arg(target));
    if (free.isEmpty())
        return text + QStringLiteral(" Every %1 port is in use by %2. Shut down, move one of "
                                     "those to a slower port, then plug the drive straight "
                                     "into the port it frees.")
                          .arg(target, busy.join(QStringLiteral(", ")));

    const Port &best = free.first();
    if (best.usbC || !best.place.isEmpty()) {
        QString port = QStringLiteral("the free %1 %2")
                           .arg(target, best.usbC ? QStringLiteral("USB-C port")
                                                  : QStringLiteral("port"));
        if (!best.place.isEmpty())
            port += QLatin1Char(' ') + best.place;
        return text + plug.arg(port);
    }

    bool separate = !finding.controller.isEmpty();
    for (const Port &port : free)
        separate = separate && port.controller == best.controller &&
                   port.controller != finding.controller;
    const bool one = free.size() == 1;
    text += one ? QStringLiteral(" This computer has one free %1 port").arg(target)
                : QStringLiteral(" This computer has %1 free %2 ports").arg(free.size()).arg(target);
    if (separate)
        text += one ? QStringLiteral(", on a separate USB controller")
                    : QStringLiteral(", all on a separate USB controller");
    text += one ? QStringLiteral(". Linux cannot tell which socket it is; ")
                : QStringLiteral(". Linux cannot tell which sockets they are; ");
    if (separate)
        text += one ? QStringLiteral("on most motherboards that controller drives a rear "
                                     "USB-C port or a red or teal USB-A port.")
                    : QStringLiteral("on most motherboards that controller drives the rear "
                                     "USB-C port and the red or teal USB-A port beside it.");
    else
        text += QStringLiteral("try a rear USB-C port first.");
    return text + plug.arg(one ? QStringLiteral("it") : QStringLiteral("one of them"));
}

} // namespace forkmesh::usblink

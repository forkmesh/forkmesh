#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace forkmesh::usblink {

inline QString defaultSysRoot() { return QStringLiteral("/sys/bus/usb/devices"); }

struct Finding {
    QString id;
    QString name;
    int speedMbps = 0;
    int capableMbps = 0;
    int availableMbps = 0;
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

inline int fastestHostMbps(const QString &sysRoot)
{
    int best = 0;
    const QDir dir(sysRoot);
    for (const QString &name : dir.entryList({QStringLiteral("usb*")},
                                             QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
        best = qMax(best, readSpeedMbps(dir.filePath(name)));
    return best;
}

inline QVector<Finding> scan(const QString &sysRoot = defaultSysRoot())
{
    QVector<Finding> findings;
    const QDir dir(sysRoot);
    const int host = fastestHostMbps(sysRoot);
    for (const QString &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        if (id.startsWith(QLatin1String("usb")) || id.contains(QLatin1Char(':')))
            continue;
        const QString path = dir.filePath(id);
        if (!isStorage(path))
            continue;
        const QString name =
            (readText(path + QStringLiteral("/manufacturer")) + QLatin1Char(' ') +
             readText(path + QStringLiteral("/product")))
                .simplified();
        QFile descriptors(path + QStringLiteral("/descriptors"));
        const QByteArray raw = descriptors.open(QIODevice::ReadOnly) ? descriptors.readAll()
                                                                    : QByteArray();
        const int capable =
            capability(name, uasCapable(raw), readText(path + QStringLiteral("/version")));
        const int speed = readSpeedMbps(path);
        const int available = qMin(capable, host);
        if (capable <= 0 || speed <= 0 || speed >= available)
            continue;
        findings.append({id, name.isEmpty() ? QStringLiteral("USB drive") : name, speed,
                         capable, available});
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
    return QStringLiteral("%1 is connected at %2 but this computer can run it at %3. "
                          "Shut down, then plug it straight into a faster port on the "
                          "computer (a rear USB-C or 10 Gbps port), not through a hub or "
                          "front-panel port.")
        .arg(finding.name, speedText(finding.speedMbps), speedText(finding.availableMbps));
}

} // namespace forkmesh::usblink

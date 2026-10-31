#pragma once

#include "SafeDelete.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QVector>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace forkmesh::fastdisk {

inline QString label() { return QStringLiteral("TMP"); }
inline QString defaultLabelDir() { return QStringLiteral("/dev/disk/by-label"); }
inline QString defaultMountsFile() { return QStringLiteral("/proc/self/mounts"); }
inline QString rootName() { return QStringLiteral("ForkMesh"); }
inline QString ownerFileName() { return QStringLiteral("worktree-root"); }
inline QString defaultFstabFile() { return QStringLiteral("/etc/fstab"); }
inline QString defaultUuidDir() { return QStringLiteral("/dev/disk/by-uuid"); }
inline QString defaultUdevDataDir() { return QStringLiteral("/run/udev/data"); }
inline QString defaultSysBlockDir() { return QStringLiteral("/sys/class/block"); }

struct Volume {
    QString device;
    QString mountPoint;
    QString fsType;
    qint64 totalBytes = -1;
    qint64 freeBytes = -1;
    bool writable = false;

    bool present() const { return !device.isEmpty(); }
    bool mounted() const { return !mountPoint.isEmpty(); }
    bool usable() const { return mounted() && writable; }
};

struct MountEntry {
    QString source;
    QString target;
    QString type;
};

inline QString unescapeMountField(QString value)
{
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return value;
}

inline QVector<MountEntry> parseMounts(const QByteArray &text)
{
    QVector<MountEntry> entries;
    for (const QByteArray &line : text.split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 2 || fields.at(0).isEmpty())
            continue;
        entries.append({unescapeMountField(QString::fromUtf8(fields.at(0))),
                        unescapeMountField(QString::fromUtf8(fields.at(1))),
                        fields.size() > 2 ? QString::fromUtf8(fields.at(2)) : QString()});
    }
    return entries;
}

inline QString deviceForLabel(const QString &labelDir = defaultLabelDir())
{
    const QFileInfo link(QDir(labelDir).filePath(label()));
    if (!link.exists())
        return QString();
    return link.canonicalFilePath();
}

inline QString canonicalOr(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? path : canonical;
}

inline bool writableDir(const QString &path)
{
#if defined(Q_OS_UNIX)
    return ::access(QFile::encodeName(path).constData(), W_OK | X_OK) == 0;
#else
    return QFileInfo(path).isWritable();
#endif
}

inline Volume probe(const QString &labelDir = defaultLabelDir(),
                    const QString &mountsFile = defaultMountsFile())
{
    Volume volume;
    volume.device = deviceForLabel(labelDir);
    if (volume.device.isEmpty())
        return volume;
    QFile mounts(mountsFile);
    if (!mounts.open(QIODevice::ReadOnly))
        return volume;
    for (const MountEntry &entry : parseMounts(mounts.readAll())) {
        if (canonicalOr(entry.source) != volume.device)
            continue;
        if (entry.target.isEmpty() || entry.target == QLatin1String("/") ||
            !QFileInfo(entry.target).isDir())
            continue;
        volume.mountPoint = QDir::cleanPath(entry.target);
        volume.fsType = entry.type;
        break;
    }
    if (!volume.mounted())
        return volume;
    const QStorageInfo storage(volume.mountPoint);
    volume.totalBytes = storage.bytesTotal();
    volume.freeBytes = storage.bytesAvailable();
    volume.writable = !storage.isReadOnly() && writableDir(volume.mountPoint);
    return volume;
}

inline QString rootOn(const Volume &volume)
{
    if (!volume.mounted())
        return QString();
    return QDir(volume.mountPoint).filePath(rootName());
}

struct BootMount {
    QString uuid;
    QString target;
    QString fsType;
    QString options;
    int pass = 0;
    bool configured = false;

    bool possible() const { return !configured && !uuid.isEmpty() && !target.isEmpty(); }
    QString fstabLine() const
    {
        return QStringLiteral("UUID=%1 %2 %3 %4 0 %5").arg(uuid, target, fsType, options).arg(pass);
    }
};

inline bool safeUuid(const QString &uuid)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9A-Fa-f-]{4,64}$"));
    return pattern.match(uuid).hasMatch();
}

inline bool safeMountTarget(const QString &path)
{
    static const QRegularExpression pattern(QStringLiteral("^/[A-Za-z0-9._/-]+$"));
    return pattern.match(path).hasMatch() && !path.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

inline QString uuidForDevice(const QString &device, const QString &uuidDir = defaultUuidDir())
{
    if (device.isEmpty())
        return QString();
    for (const QFileInfo &entry :
         QDir(uuidDir).entryInfoList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        if (entry.canonicalFilePath() == device)
            return entry.fileName();
    }
    return QString();
}

inline QString fsTypeFromUdev(const QString &device, const QString &udevDir = defaultUdevDataDir(),
                              const QString &sysBlockDir = defaultSysBlockDir())
{
    QFile numbers(QDir(sysBlockDir).filePath(QFileInfo(device).fileName() + QStringLiteral("/dev")));
    if (!numbers.open(QIODevice::ReadOnly))
        return QString();
    const QString majorMinor = QString::fromLatin1(numbers.readAll()).trimmed();
    QFile data(QDir(udevDir).filePath(QStringLiteral("b") + majorMinor));
    if (majorMinor.isEmpty() || !data.open(QIODevice::ReadOnly))
        return QString();
    for (const QByteArray &line : data.readAll().split('\n')) {
        if (line.startsWith("E:ID_FS_TYPE="))
            return QString::fromLatin1(line.mid(13)).trimmed();
    }
    return QString();
}

inline bool fstabCovers(const QByteArray &fstab, const QString &device, const QString &uuid)
{
    for (const QByteArray &raw : fstab.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QString source = unescapeMountField(QString::fromUtf8(line.simplified().split(' ').first()));
        if (!uuid.isEmpty() &&
            (source.compare(QStringLiteral("UUID=") + uuid, Qt::CaseInsensitive) == 0 ||
             source == QStringLiteral("/dev/disk/by-uuid/") + uuid))
            return true;
        if (source == QStringLiteral("LABEL=") + label() ||
            source == QDir(defaultLabelDir()).filePath(label()))
            return true;
        if (source.startsWith(QLatin1Char('/')) && !device.isEmpty() && canonicalOr(source) == device)
            return true;
    }
    return false;
}

inline BootMount planBootMount(const Volume &volume, const QByteArray &fstab,
                               const QString &defaultTarget, uint uid, uint gid,
                               const QString &uuidDir = defaultUuidDir(),
                               const QString &udevDir = defaultUdevDataDir(),
                               const QString &sysBlockDir = defaultSysBlockDir())
{
    BootMount plan;
    if (!volume.present())
        return plan;
    plan.uuid = uuidForDevice(volume.device, uuidDir);
    plan.configured = fstabCovers(fstab, volume.device, plan.uuid);
    if (!safeUuid(plan.uuid)) {
        plan.uuid.clear();
        return plan;
    }
    plan.target = volume.mounted() && safeMountTarget(volume.mountPoint) ? volume.mountPoint
                                                                        : defaultTarget;
    if (!safeMountTarget(plan.target))
        plan.target.clear();
    plan.fsType = volume.fsType.isEmpty() ? fsTypeFromUdev(volume.device, udevDir, sysBlockDir)
                                          : volume.fsType;
    static const QRegularExpression typePattern(QStringLiteral("^[a-z0-9]{2,16}$"));
    if (!typePattern.match(plan.fsType).hasMatch())
        plan.fsType = QStringLiteral("auto");
    plan.options = QStringLiteral("defaults,noatime,nofail,x-systemd.device-timeout=10s");
    static const QStringList ownerless = {QStringLiteral("vfat"), QStringLiteral("exfat"),
                                          QStringLiteral("ntfs"), QStringLiteral("ntfs3")};
    if (ownerless.contains(plan.fsType))
        plan.options += QStringLiteral(",uid=%1,gid=%2").arg(uid).arg(gid);
    static const QStringList checked = {QStringLiteral("ext2"), QStringLiteral("ext3"),
                                        QStringLiteral("ext4")};
    plan.pass = checked.contains(plan.fsType) ? 2 : 0;
    return plan;
}

inline QString defaultBootTarget(const QString &userName)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]+$"));
    if (pattern.match(userName).hasMatch() && userName != QLatin1String(".") &&
        userName != QLatin1String(".."))
        return QStringLiteral("/media/%1/%2").arg(userName, label());
    return QStringLiteral("/mnt/%1").arg(label());
}

inline QString bootMountScript()
{
    return QStringLiteral(
        "set -e\n"
        "if grep -Eiq \"^[[:space:]]*UUID=$3[[:space:]]\" /etc/fstab; then exit 0; fi\n"
        "cp -p /etc/fstab \"/etc/fstab.forkmesh-$(date +%Y%m%d%H%M%S)\"\n"
        "if [ -s /etc/fstab ] && [ -n \"$(tail -c 1 /etc/fstab)\" ]; then echo >> /etc/fstab; fi\n"
        "printf '%s\\n' \"$1\" >> /etc/fstab\n"
        "mkdir -p \"$2\"\n"
        "systemctl daemon-reload 2>/dev/null || true\n"
        "mountpoint -q \"$2\" || mount \"$2\"\n");
}

inline QStringList bootMountArguments(const BootMount &plan)
{
    return {QStringLiteral("sh"), QStringLiteral("-c"), bootMountScript(), QStringLiteral("sh"),
            plan.fstabLine(), plan.target, plan.uuid};
}

namespace detail {

inline QMutex &activeMutex()
{
    static QMutex mutex;
    return mutex;
}

inline QString &activeValue()
{
    static QString value;
    return value;
}

} // namespace detail

inline QString activeRoot()
{
    QMutexLocker locker(&detail::activeMutex());
    return detail::activeValue();
}

inline void setActiveRoot(const QString &root)
{
    QMutexLocker locker(&detail::activeMutex());
    detail::activeValue() = root;
}

inline QString activate(const Volume &volume, bool enabled)
{
    QString root = enabled && volume.usable() ? rootOn(volume) : QString();
    if (!root.isEmpty() && !QDir().mkpath(root))
        root.clear();
    setActiveRoot(root);
    return root;
}

inline QString scratchBase(const QString &root)
{
    return root.isEmpty() ? QString() : root + QStringLiteral("/scratch");
}

inline QString ccacheDir(const QString &root)
{
    return root.isEmpty() ? QString() : root + QStringLiteral("/ccache");
}

inline QString actionsDir(const QString &root)
{
    return root.isEmpty() ? QString() : root + QStringLiteral("/actions");
}

inline QString scratchKey(const QString &worktreeRoot)
{
    const QString clean = QDir::cleanPath(QDir(worktreeRoot).absolutePath());
    return QString::fromLatin1(
        QCryptographicHash::hash(clean.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

inline QString scratchParent(const QString &root, const QString &worktreeRoot)
{
    if (root.isEmpty() || worktreeRoot.trimmed().isEmpty())
        return QString();
    return scratchBase(root) + QLatin1Char('/') + scratchKey(worktreeRoot);
}

inline bool ignoredTopEntry(const QString &name)
{
    return name == QLatin1String("lost+found") || name.startsWith(QLatin1String(".Trash-"));
}

struct Usage {
    qint64 usedBytes = -1;
    bool forkmeshFiles = false;
    int otherEntries = 0;
    bool hasData() const { return forkmeshFiles || otherEntries > 0; }
};

inline bool containsFile(const QString &path)
{
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    return it.hasNext();
}

inline Usage usage(const Volume &volume)
{
    Usage result;
    if (!volume.mounted())
        return result;
    const QStorageInfo storage(volume.mountPoint);
    if (storage.isValid())
        result.usedBytes = storage.bytesTotal() - storage.bytesFree();
    const QDir top(volume.mountPoint);
    for (const QFileInfo &entry :
         top.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (ignoredTopEntry(entry.fileName()))
            continue;
        if (entry.fileName() == rootName() && entry.isDir() && !entry.isSymLink())
            result.forkmeshFiles = containsFile(entry.absoluteFilePath());
        else
            ++result.otherEntries;
    }
    return result;
}

struct CleanResult {
    int removed = 0;
    QStringList failures;
};

inline QString readOwner(const QString &keyDir)
{
    QFile file(QDir(keyDir).filePath(ownerFileName()));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll()).trimmed();
}

inline void removeCounted(const QString &path, const QString &parent, CleanResult *result)
{
    QString why;
    if (safedelete::removeTree(path, parent, &why))
        ++result->removed;
    else
        result->failures << why;
}

inline void cleanOldTemp(const QString &tmpDir, qint64 cutoffMs, CleanResult *result)
{
    const QDir dir(tmpDir);
    if (!dir.exists())
        return;
    for (const QFileInfo &entry :
         dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (entry.lastModified().toMSecsSinceEpoch() < cutoffMs)
            removeCounted(entry.absoluteFilePath(), dir.absolutePath(), result);
    }
}

inline CleanResult cleanUnused(const QString &root, qint64 nowMs, qint64 tempMaxAgeMs)
{
    CleanResult result;
    const QString base = scratchBase(root);
    const QDir scratch(base);
    if (base.isEmpty() || !scratch.exists())
        return result;
    const qint64 cutoffMs = nowMs - tempMaxAgeMs;
    for (const QFileInfo &key : scratch.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const QString keyDir = key.absoluteFilePath();
        const QString owner = readOwner(keyDir);
        if (owner.isEmpty() || !QFileInfo(owner).isDir()) {
            removeCounted(keyDir, scratch.absolutePath(), &result);
            continue;
        }
        const QDir folders(QDir(keyDir).filePath(QStringLiteral(".scratch")));
        for (const QFileInfo &folder :
             folders.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
            const QFileInfo worktree(QDir(owner).filePath(folder.fileName()));
            if (!worktree.exists() && !worktree.isSymLink())
                removeCounted(folder.absoluteFilePath(), folders.absolutePath(), &result);
            else
                cleanOldTemp(QDir(folder.absoluteFilePath()).filePath(QStringLiteral("tmp")),
                             cutoffMs, &result);
        }
    }
    return result;
}

struct EraseResult {
    bool ok = false;
    int removed = 0;
    QString error;
};

inline EraseResult eraseAll(const Volume &expected, const QString &labelDir = defaultLabelDir(),
                            const QString &mountsFile = defaultMountsFile())
{
    EraseResult result;
    const Volume current = probe(labelDir, mountsFile);
    if (!current.present()) {
        result.error = QStringLiteral("No disk labelled %1 is connected.").arg(label());
        return result;
    }
    if (!current.mounted()) {
        result.error = QStringLiteral("The disk labelled %1 is not mounted.").arg(label());
        return result;
    }
    if (current.device != expected.device || current.mountPoint != expected.mountPoint) {
        result.error = QStringLiteral("The disk labelled %1 changed since it was checked; "
                                      "look again before erasing.")
                           .arg(label());
        return result;
    }
    if (!current.writable) {
        result.error = QStringLiteral("The disk labelled %1 is read-only.").arg(label());
        return result;
    }
    const QDir top(current.mountPoint);
    for (const QFileInfo &entry :
         top.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (entry.fileName() == QLatin1String("lost+found"))
            continue;
        QString why;
        if (safedelete::removeTree(entry.absoluteFilePath(), current.mountPoint, &why)) {
            ++result.removed;
        } else if (result.error.isEmpty()) {
            result.error = why;
        }
    }
    result.ok = result.error.isEmpty();
    return result;
}

inline QString formatBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("?");
    const char *units[] = {"B", "K", "M", "G", "T"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 || value >= 10.0
               ? QStringLiteral("%1%2").arg(qRound64(value)).arg(QLatin1String(units[unit]))
               : QStringLiteral("%1%2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

} // namespace forkmesh::fastdisk

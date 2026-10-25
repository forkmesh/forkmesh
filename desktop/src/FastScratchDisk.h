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

struct Volume {
    QString device;
    QString mountPoint;
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
                        unescapeMountField(QString::fromUtf8(fields.at(1)))});
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

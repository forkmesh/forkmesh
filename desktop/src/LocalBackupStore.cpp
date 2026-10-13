#include "LocalBackupStore.h"
#include "AppPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace {

const QString kBackupPrefix = QStringLiteral("forkmesh-backup-");
const QString kBackupSuffix = QStringLiteral(".tar.gz");
const QString kBackupStampFormat = QStringLiteral("yyyyMMdd-HHmmss");

} // namespace

namespace forkmesh {

bool autoBackupDefault(const QString &cloudflareApiToken)
{
    // A control node's live store can be several gigabytes. Keeping 24 hourly
    // compressed copies filled both desktops and the small VPSes they manage.
    // Backups remain available as an explicit opt-in, but credentials must
    // never silently turn a storage-heavy recurring job on.
    (void)cloudflareApiToken;
    return false;
}

QString defaultBackupRoot()
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/backups");
}

QString backupFileName(const QDateTime &when)
{
    return kBackupPrefix + when.toString(kBackupStampFormat) + kBackupSuffix;
}

QDateTime backupTimestampFromName(const QString &fileName)
{
    if (!fileName.startsWith(kBackupPrefix) || !fileName.endsWith(kBackupSuffix))
        return QDateTime();
    const QString stamp = fileName.mid(kBackupPrefix.size(),
                                       fileName.size() - kBackupPrefix.size() -
                                           kBackupSuffix.size());
    if (stamp.size() != kBackupStampFormat.size())
        return QDateTime();
    return QDateTime::fromString(stamp, kBackupStampFormat);
}

QList<BackupSnapshot> listBackups(const QString &root)
{
    QList<BackupSnapshot> snapshots;
    const QFileInfoList entries =
        QDir(root).entryInfoList(QDir::Files, QDir::NoSort);
    for (const QFileInfo &fi : entries) {
        const QDateTime taken = backupTimestampFromName(fi.fileName());
        if (!taken.isValid())
            continue;
        snapshots.append(
            {fi.absoluteFilePath(), fi.fileName(), taken, fi.size()});
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const BackupSnapshot &a, const BackupSnapshot &b) {
                  if (a.taken == b.taken)
                      return a.fileName > b.fileName;
                  return a.taken > b.taken; // newest first
              });
    return snapshots;
}

QList<BackupSnapshot> backupsToPrune(const QList<BackupSnapshot> &snapshots,
                                     int keep)
{
    const int floor = std::max(keep, kBackupKeepMin);
    if (snapshots.size() <= floor)
        return {};
    return snapshots.mid(floor);
}

bool backupIsDue(const QDateTime &last, const QDateTime &now, qint64 intervalMs)
{
    if (!last.isValid())
        return true; // nothing on disk yet
    const qint64 elapsed = last.msecsTo(now);
    if (elapsed < 0)
        return false; // clock went backwards; wait for the hourly timer instead
    return elapsed >= intervalMs;
}

QStringList configArchiveTarArgs(const QString &archivePath,
                                 const QString &appDataDir,
                                 const QString &configFile,
                                 const QStringList &excludeRoots)
{
    const QString appData = QDir(appDataDir).absolutePath();
    const QString appParent = QFileInfo(appData).absolutePath();
    const bool haveApp = QFileInfo::exists(appData);
    const bool haveConfig = QFileInfo::exists(configFile);
    if (!haveApp && !haveConfig)
        return {};

    QStringList args;
    args << QStringLiteral("-czf") << archivePath;
    for (const QString &root : excludeRoots) {
        if (root.isEmpty())
            continue;
        const QString rel =
            QDir(appParent).relativeFilePath(QDir(root).absolutePath());
        if (rel.isEmpty() || rel.startsWith(QStringLiteral("..")))
            continue;
        args << (QStringLiteral("--exclude=") + rel);
    }
    if (haveApp)
        args << QStringLiteral("-C") << appParent << QFileInfo(appData).fileName();
    if (haveConfig)
        args << QStringLiteral("-C") << QFileInfo(configFile).absolutePath()
             << QFileInfo(configFile).fileName();
    return args;
}

} // namespace forkmesh

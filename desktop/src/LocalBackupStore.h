#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>


namespace forkmesh {

struct BackupSnapshot {
    QString path;     // absolute path to the .tar.gz
    QString fileName; // base name
    QDateTime taken;  // parsed from the file name (local time)
    qint64 bytes = 0;
};

constexpr qint64 kBackupIntervalMs = 60 * 60 * 1000;
constexpr int kBackupKeepDefault = 24;
constexpr int kBackupKeepMin = 1;
constexpr int kBackupKeepMax = 240;

// The default for backup/hourlyEnabled when the user has never chosen is off
// everywhere. A rolling day of multi-gigabyte control-node snapshots can fill
// a disk in hours, so even credentials-bearing control nodes must explicitly
// opt in from Settings -> Data. The parameter is retained for source/API
// compatibility with older callers.
bool autoBackupDefault(const QString &cloudflareApiToken);

QString defaultBackupRoot();

QString backupFileName(const QDateTime &when);
QDateTime backupTimestampFromName(const QString &fileName);

QList<BackupSnapshot> listBackups(const QString &root);

QList<BackupSnapshot> backupsToPrune(const QList<BackupSnapshot> &snapshots,
                                     int keep);

bool backupIsDue(const QDateTime &last, const QDateTime &now,
                 qint64 intervalMs = kBackupIntervalMs);

QStringList configArchiveTarArgs(const QString &archivePath,
                                 const QString &appDataDir,
                                 const QString &configFile,
                                 const QStringList &excludeRoots);

} // namespace forkmesh

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

namespace forkmesh::backups {

enum class Shelf { Snapshots, Kept, PackageData, MovedAside, Fleet };

enum class VerifyKind { None, TarGzip, Gzip, PackageData };

struct Locations {
    QString snapshots;
    QString dataRoot;
    QString settingsFile;
    QString fleet;

    QString kept() const;
    QString packageData() const;
};

struct BackupItem {
    Shelf shelf = Shelf::Snapshots;
    QString path;
    QString name;
    QDateTime taken;
    qint64 bytes = -1;
    bool directory = false;
    bool encrypted = false;
    QString label;
};

struct ShelfTotals {
    int count = 0;
    qint64 bytes = 0;
    QDateTime newest;
};

struct Health {
    QString text;
    bool warning = false;
};

class BackupShelf
{
public:
    static constexpr int kMaxLabels = 1000;
    static constexpr int kMaxLabelLength = 120;

    static QList<Shelf> shelves();
    static QString title(Shelf shelf);
    static QString description(Shelf shelf);
    static QString key(Shelf shelf);

    static QString fleetDirectory(const QString &backupsOverride,
                                  const QString &dataOverride,
                                  const QString &home);

    static QList<BackupItem> scan(const Locations &locations,
                                  const QHash<QString, QString> &labels);
    static QList<BackupItem> scanShelf(Shelf shelf, const Locations &locations);

    static QDateTime fleetTimestamp(const QString &name);
    static QDateTime movedAsideTimestamp(const QString &name, const QString &base);
    static QString packageDataId(const QString &name);

    static bool canKeep(const BackupItem &item);
    static bool canRelease(const BackupItem &item);
    static bool canRestore(const BackupItem &item);
    static bool canCopy(const BackupItem &item);
    static bool canDelete(const BackupItem &item, const Locations &locations);
    static QString keptPath(const BackupItem &item, const Locations &locations);
    static QString releasedPath(const BackupItem &item, const Locations &locations);

    static VerifyKind verifyKind(const BackupItem &item);
    static QStringList verifyArguments(const BackupItem &item);
    static bool packageDataValid(const QByteArray &body, QString *packageId = nullptr,
                                 int *keys = nullptr);

    static QString normalizedLabel(const QString &label);
    static QByteArray serializeLabels(const QHash<QString, QString> &labels);
    static QHash<QString, QString> parseLabels(const QByteArray &body);

    static QMap<Shelf, ShelfTotals> totals(const QList<BackupItem> &items);
    static QSet<QString> leavingAtNextBackup(const QList<BackupItem> &items, int keep);
    static QString ageText(const QDateTime &when, const QDateTime &now);
    static Health health(const QList<BackupItem> &items, bool hourly,
                         const QDateTime &now);
    static bool matches(const BackupItem &item, const QString &query);
    static QString itemId(const BackupItem &item);
};

} // namespace forkmesh::backups

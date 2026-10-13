#include "BackupShelf.h"

#include "LocalBackupStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>

namespace forkmesh::backups {
namespace {

const QString kMovedAsideStamp = QStringLiteral("yyyyMMdd-HHmmss");
const QRegularExpression kFleetName(
    QStringLiteral("^rqlite-(\\d{8}T\\d{6}Z)\\.sqlite3\\.gz(\\.age)?$"));
const QRegularExpression kPackageDataName(
    QStringLiteral("^forkmesh-([a-z0-9][a-z0-9-]{0,63})-data\\.json$"));

QString parentOf(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absolutePath());
}

bool directlyInside(const QString &path, const QString &folder)
{
    return !folder.isEmpty() &&
           parentOf(path) == QDir::cleanPath(QDir(folder).absolutePath());
}

bool validLabelName(const QString &name)
{
    return !name.isEmpty() && name.size() <= 255 &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
           name != QLatin1String(".") && name != QLatin1String("..");
}

QList<BackupItem> snapshotItems(Shelf shelf, const QString &root)
{
    QList<BackupItem> items;
    if (root.isEmpty())
        return items;
    for (const forkmesh::BackupSnapshot &snapshot : forkmesh::listBackups(root)) {
        if (QFileInfo(snapshot.path).isSymLink())
            continue;
        BackupItem item;
        item.shelf = shelf;
        item.path = snapshot.path;
        item.name = snapshot.fileName;
        item.taken = snapshot.taken;
        item.bytes = snapshot.bytes;
        items.append(item);
    }
    return items;
}

QList<BackupItem> packageDataItems(const QString &root)
{
    QList<BackupItem> items;
    const QFileInfoList entries =
        QDir(root).entryInfoList({QStringLiteral("forkmesh-*-data.json")}, QDir::Files);
    for (const QFileInfo &info : entries) {
        if (info.isSymLink() || BackupShelf::packageDataId(info.fileName()).isEmpty())
            continue;
        BackupItem item;
        item.shelf = Shelf::PackageData;
        item.path = info.absoluteFilePath();
        item.name = info.fileName();
        item.taken = info.lastModified();
        item.bytes = info.size();
        items.append(item);
    }
    return items;
}

void appendMovedAside(QList<BackupItem> *items, const QString &live, bool directory)
{
    if (live.isEmpty())
        return;
    const QString base = QFileInfo(live).fileName();
    const QDir folder(parentOf(live));
    const QFileInfoList entries = folder.entryInfoList(
        {base + QStringLiteral(".bak-*")},
        (directory ? QDir::Dirs : QDir::Files) | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        if (info.isSymLink())
            continue;
        const QDateTime taken = BackupShelf::movedAsideTimestamp(info.fileName(), base);
        if (!taken.isValid())
            continue;
        BackupItem item;
        item.shelf = Shelf::MovedAside;
        item.path = info.absoluteFilePath();
        item.name = info.fileName();
        item.taken = taken;
        item.directory = directory;
        item.bytes = directory ? -1 : info.size();
        items->append(item);
    }
}

QList<BackupItem> fleetItems(const QString &root)
{
    QList<BackupItem> items;
    if (root.isEmpty())
        return items;
    const QFileInfoList entries =
        QDir(root).entryInfoList({QStringLiteral("rqlite-*")}, QDir::Files);
    for (const QFileInfo &info : entries) {
        const QDateTime taken = BackupShelf::fleetTimestamp(info.fileName());
        if (info.isSymLink() || !taken.isValid())
            continue;
        BackupItem item;
        item.shelf = Shelf::Fleet;
        item.path = info.absoluteFilePath();
        item.name = info.fileName();
        item.taken = taken;
        item.bytes = info.size();
        item.encrypted = info.fileName().endsWith(QStringLiteral(".age"));
        items.append(item);
    }
    return items;
}

} // namespace

QString Locations::kept() const
{
    return snapshots.isEmpty() ? QString() : snapshots + QStringLiteral("/kept");
}

QString Locations::packageData() const
{
    return dataRoot.isEmpty() ? QString() : dataRoot + QStringLiteral("/package-data");
}

QList<Shelf> BackupShelf::shelves()
{
    return {Shelf::Snapshots, Shelf::Kept, Shelf::PackageData, Shelf::MovedAside,
            Shelf::Fleet};
}

QString BackupShelf::title(Shelf shelf)
{
    switch (shelf) {
    case Shelf::Snapshots:
        return QStringLiteral("Snapshots");
    case Shelf::Kept:
        return QStringLiteral("Kept");
    case Shelf::PackageData:
        return QStringLiteral("Package data");
    case Shelf::MovedAside:
        return QStringLiteral("Moved aside");
    case Shelf::Fleet:
        return QStringLiteral("Fleet database");
    }
    return QString();
}

QString BackupShelf::description(Shelf shelf)
{
    switch (shelf) {
    case Shelf::Snapshots:
        return QStringLiteral(
            "Hourly and on-demand snapshots of this desktop's identity, account and "
            "settings. Only the newest ones are kept; older ones are removed as new "
            "ones are taken. Keep a snapshot to take it out of rotation.");
    case Shelf::Kept:
        return QStringLiteral(
            "Snapshots you chose to keep. Rotation never removes them; return one to "
            "rotation or delete it when you no longer need it.");
    case Shelf::PackageData:
        return QStringLiteral(
            "Copies of a package's data saved when it was removed from the rail. "
            "Installing the package again offers to import its copy.");
    case Shelf::MovedAside:
        return QStringLiteral(
            "The data folder and settings a restore moved aside before replacing "
            "them. Delete them once the restored desktop is working.");
    case Shelf::Fleet:
        return QStringLiteral(
            "Nightly database backups pulled from the ForkMesh servers by "
            "tools/next_backup.py. Its own retention removes old ones, so they are "
            "read-only here.");
    }
    return QString();
}

QString BackupShelf::key(Shelf shelf)
{
    switch (shelf) {
    case Shelf::Snapshots:
        return QStringLiteral("snapshots");
    case Shelf::Kept:
        return QStringLiteral("kept");
    case Shelf::PackageData:
        return QStringLiteral("package-data");
    case Shelf::MovedAside:
        return QStringLiteral("moved-aside");
    case Shelf::Fleet:
        return QStringLiteral("fleet");
    }
    return QString();
}

QString BackupShelf::fleetDirectory(const QString &backupsOverride,
                                    const QString &dataOverride, const QString &home)
{
    if (!backupsOverride.trimmed().isEmpty())
        return QDir::cleanPath(backupsOverride.trimmed());
    if (!dataOverride.trimmed().isEmpty())
        return QDir::cleanPath(dataOverride.trimmed() + QStringLiteral("/backups"));
    if (home.isEmpty())
        return QString();
    return QDir::cleanPath(home + QStringLiteral("/data/fleet/backups"));
}

QList<BackupItem> BackupShelf::scanShelf(Shelf shelf, const Locations &locations)
{
    switch (shelf) {
    case Shelf::Snapshots:
        return snapshotItems(shelf, locations.snapshots);
    case Shelf::Kept:
        return snapshotItems(shelf, locations.kept());
    case Shelf::PackageData:
        return locations.packageData().isEmpty() ? QList<BackupItem>()
                                                 : packageDataItems(locations.packageData());
    case Shelf::MovedAside: {
        QList<BackupItem> items;
        appendMovedAside(&items, locations.dataRoot, true);
        appendMovedAside(&items, locations.settingsFile, false);
        std::sort(items.begin(), items.end(), [](const BackupItem &a, const BackupItem &b) {
            return a.taken == b.taken ? a.name > b.name : a.taken > b.taken;
        });
        return items;
    }
    case Shelf::Fleet: {
        QList<BackupItem> items = fleetItems(locations.fleet);
        std::sort(items.begin(), items.end(), [](const BackupItem &a, const BackupItem &b) {
            return a.taken == b.taken ? a.name > b.name : a.taken > b.taken;
        });
        return items;
    }
    }
    return {};
}

QList<BackupItem> BackupShelf::scan(const Locations &locations,
                                    const QHash<QString, QString> &labels)
{
    QList<BackupItem> items;
    for (Shelf shelf : shelves()) {
        for (BackupItem item : scanShelf(shelf, locations)) {
            item.label = labels.value(item.name);
            items.append(item);
        }
    }
    return items;
}

QDateTime BackupShelf::fleetTimestamp(const QString &name)
{
    const QRegularExpressionMatch match = kFleetName.match(name);
    if (!match.hasMatch())
        return QDateTime();
    QDateTime taken =
        QDateTime::fromString(match.captured(1), QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    if (!taken.isValid())
        return QDateTime();
    taken.setTimeZone(QTimeZone::utc());
    return taken.toLocalTime();
}

QDateTime BackupShelf::movedAsideTimestamp(const QString &name, const QString &base)
{
    const QString prefix = base + QStringLiteral(".bak-");
    if (base.isEmpty() || !name.startsWith(prefix) ||
        name.size() != prefix.size() + kMovedAsideStamp.size())
        return QDateTime();
    return QDateTime::fromString(name.mid(prefix.size()), kMovedAsideStamp);
}

QString BackupShelf::packageDataId(const QString &name)
{
    const QRegularExpressionMatch match = kPackageDataName.match(name);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool BackupShelf::canKeep(const BackupItem &item)
{
    return item.shelf == Shelf::Snapshots;
}

bool BackupShelf::canRelease(const BackupItem &item)
{
    return item.shelf == Shelf::Kept;
}

bool BackupShelf::canRestore(const BackupItem &item)
{
    return item.shelf == Shelf::Snapshots || item.shelf == Shelf::Kept;
}

bool BackupShelf::canCopy(const BackupItem &item)
{
    return !item.directory;
}

bool BackupShelf::canDelete(const BackupItem &item, const Locations &locations)
{
    const QFileInfo info(item.path);
    if (item.name != info.fileName() || info.isSymLink() || !info.exists())
        return false;
    switch (item.shelf) {
    case Shelf::Snapshots:
        return info.isFile() && directlyInside(item.path, locations.snapshots) &&
               forkmesh::backupTimestampFromName(item.name).isValid();
    case Shelf::Kept:
        return info.isFile() && directlyInside(item.path, locations.kept()) &&
               forkmesh::backupTimestampFromName(item.name).isValid();
    case Shelf::PackageData:
        return info.isFile() && directlyInside(item.path, locations.packageData()) &&
               !packageDataId(item.name).isEmpty();
    case Shelf::MovedAside: {
        const QStringList lives = {locations.dataRoot, locations.settingsFile};
        for (int i = 0; i < lives.size(); ++i) {
            const QString &live = lives.at(i);
            const bool directory = i == 0;
            if (live.isEmpty() || info.isDir() != directory)
                continue;
            if (parentOf(item.path) == parentOf(live) &&
                movedAsideTimestamp(item.name, QFileInfo(live).fileName()).isValid() &&
                QDir::cleanPath(info.absoluteFilePath()) !=
                    QDir::cleanPath(QFileInfo(live).absoluteFilePath()))
                return true;
        }
        return false;
    }
    case Shelf::Fleet:
        return false;
    }
    return false;
}

QString BackupShelf::keptPath(const BackupItem &item, const Locations &locations)
{
    if (!canKeep(item) || locations.kept().isEmpty())
        return QString();
    return QDir(locations.kept()).filePath(item.name);
}

QString BackupShelf::releasedPath(const BackupItem &item, const Locations &locations)
{
    if (!canRelease(item) || locations.snapshots.isEmpty())
        return QString();
    return QDir(locations.snapshots).filePath(item.name);
}

VerifyKind BackupShelf::verifyKind(const BackupItem &item)
{
    if (item.directory || item.encrypted)
        return VerifyKind::None;
    switch (item.shelf) {
    case Shelf::Snapshots:
    case Shelf::Kept:
        return VerifyKind::TarGzip;
    case Shelf::PackageData:
        return VerifyKind::PackageData;
    case Shelf::Fleet:
        return VerifyKind::Gzip;
    case Shelf::MovedAside:
        return VerifyKind::None;
    }
    return VerifyKind::None;
}

QStringList BackupShelf::verifyArguments(const BackupItem &item)
{
    switch (verifyKind(item)) {
    case VerifyKind::TarGzip:
        return {QStringLiteral("-tzf"), item.path};
    case VerifyKind::Gzip:
        return {QStringLiteral("-t"), item.path};
    case VerifyKind::PackageData:
    case VerifyKind::None:
        return {};
    }
    return {};
}

bool BackupShelf::packageDataValid(const QByteArray &body, QString *packageId, int *keys)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject bundle = document.object();
    const QString id = bundle.value(QStringLiteral("packageId")).toString();
    const QJsonValue data = bundle.value(QStringLiteral("data"));
    if (bundle.value(QStringLiteral("format")).toString() !=
            QLatin1String("forkmesh-package-data") ||
        bundle.value(QStringLiteral("formatVersion")).toInt() != 1 ||
        packageDataId(QStringLiteral("forkmesh-%1-data.json").arg(id)).isEmpty() ||
        !data.isObject())
        return false;
    if (packageId)
        *packageId = id;
    if (keys)
        *keys = static_cast<int>(data.toObject().size());
    return true;
}

QString BackupShelf::normalizedLabel(const QString &label)
{
    QString text = label.simplified();
    text.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    return text.left(kMaxLabelLength);
}

QByteArray BackupShelf::serializeLabels(const QHash<QString, QString> &labels)
{
    QStringList names = labels.keys();
    names.sort();
    QJsonObject object;
    for (const QString &name : names) {
        const QString label = normalizedLabel(labels.value(name));
        if (!validLabelName(name) || label.isEmpty())
            continue;
        if (object.size() >= kMaxLabels)
            break;
        object.insert(name, label);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QHash<QString, QString> BackupShelf::parseLabels(const QByteArray &body)
{
    QHash<QString, QString> labels;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return labels;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (labels.size() >= kMaxLabels)
            break;
        const QString label = normalizedLabel(it.value().toString());
        if (validLabelName(it.key()) && !label.isEmpty())
            labels.insert(it.key(), label);
    }
    return labels;
}

QMap<Shelf, ShelfTotals> BackupShelf::totals(const QList<BackupItem> &items)
{
    QMap<Shelf, ShelfTotals> result;
    for (Shelf shelf : shelves())
        result.insert(shelf, ShelfTotals());
    for (const BackupItem &item : items) {
        ShelfTotals &total = result[item.shelf];
        ++total.count;
        if (item.bytes > 0)
            total.bytes += item.bytes;
        if (item.taken.isValid() && (!total.newest.isValid() || item.taken > total.newest))
            total.newest = item.taken;
    }
    return result;
}

QSet<QString> BackupShelf::leavingAtNextBackup(const QList<BackupItem> &items, int keep)
{
    QList<BackupItem> rotation;
    for (const BackupItem &item : items) {
        if (item.shelf == Shelf::Snapshots)
            rotation.append(item);
    }
    std::sort(rotation.begin(), rotation.end(), [](const BackupItem &a, const BackupItem &b) {
        return a.taken == b.taken ? a.name > b.name : a.taken > b.taken;
    });
    QSet<QString> leaving;
    const int floor = std::max(keep, forkmesh::kBackupKeepMin);
    for (int i = floor - 1; i >= 0 && i < rotation.size(); ++i)
        leaving.insert(rotation.at(i).name);
    return leaving;
}

QString BackupShelf::ageText(const QDateTime &when, const QDateTime &now)
{
    if (!when.isValid())
        return QString();
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60)
        return QStringLiteral("just now");
    const auto unit = [](qint64 count, const char *one, const char *many) {
        return QStringLiteral("%1 %2 ago")
            .arg(count)
            .arg(QLatin1String(count == 1 ? one : many));
    };
    if (seconds < 3600)
        return unit(seconds / 60, "minute", "minutes");
    if (seconds < 86400)
        return unit(seconds / 3600, "hour", "hours");
    if (seconds < 86400 * 60)
        return unit(seconds / 86400, "day", "days");
    return unit(seconds / (86400 * 30), "month", "months");
}

Health BackupShelf::health(const QList<BackupItem> &items, bool hourly,
                           const QDateTime &now)
{
    QDateTime newest;
    for (const BackupItem &item : items) {
        if ((item.shelf == Shelf::Snapshots || item.shelf == Shelf::Kept) &&
            item.taken.isValid() && (!newest.isValid() || item.taken > newest))
            newest = item.taken;
    }
    if (!newest.isValid()) {
        if (hourly)
            return {QStringLiteral("No snapshot yet. The first hourly one is taken shortly "
                                   "after ForkMesh starts."),
                    false};
        return {QStringLiteral("There is no snapshot of this desktop. Back up now, or turn "
                               "on hourly backups."),
                true};
    }
    const qint64 age = newest.secsTo(now);
    const QString last = QStringLiteral("Last snapshot %1.").arg(ageText(newest, now));
    if (hourly && age > 2 * 3600)
        return {last + QStringLiteral(" Hourly backups are on but have not run since; "
                                      "check the status below or back up now."),
                true};
    if (!hourly)
        return {last + QStringLiteral(" Hourly backups are off."), age > 7 * 86400};
    return {last + QStringLiteral(" Hourly backups are on."), false};
}

bool BackupShelf::matches(const BackupItem &item, const QString &query)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return true;
    for (const QString &word : needle.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!item.name.contains(word, Qt::CaseInsensitive) &&
            !item.label.contains(word, Qt::CaseInsensitive) &&
            !title(item.shelf).contains(word, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

QString BackupShelf::itemId(const BackupItem &item)
{
    return key(item.shelf) + QLatin1Char(':') + item.name;
}

} // namespace forkmesh::backups

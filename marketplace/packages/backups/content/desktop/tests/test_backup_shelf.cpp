#include "BackupShelf.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>

using forkmesh::backups::BackupItem;
using forkmesh::backups::BackupShelf;
using forkmesh::backups::Locations;
using forkmesh::backups::Shelf;
using forkmesh::backups::VerifyKind;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void touch(const QString &path, const QByteArray &body = QByteArray("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(body);
}

const BackupItem *find(const QList<BackupItem> &items, Shelf shelf, const QString &name)
{
    for (const BackupItem &item : items) {
        if (item.shelf == shelf && item.name == name)
            return &item;
    }
    return nullptr;
}

Locations sandbox(const QTemporaryDir &root)
{
    Locations locations;
    locations.dataRoot = root.filePath(QStringLiteral("home/data"));
    locations.snapshots = locations.dataRoot + QStringLiteral("/backups");
    locations.settingsFile = root.filePath(QStringLiteral("home/config/ForkMesh.conf"));
    locations.fleet = root.filePath(QStringLiteral("home/data/fleet/backups"));
    return locations;
}

void testScanSortsEveryShelf()
{
    QTemporaryDir root;
    const Locations locations = sandbox(root);
    touch(locations.snapshots + QStringLiteral("/forkmesh-backup-20260920-100000.tar.gz"));
    touch(locations.snapshots + QStringLiteral("/forkmesh-backup-20260921-100000.tar.gz"));
    touch(locations.snapshots + QStringLiteral("/forkmesh-backup-20260921-110000.tar.gz.part"));
    touch(locations.snapshots + QStringLiteral("/notes.txt"));
    touch(locations.kept() + QStringLiteral("/forkmesh-backup-20260101-090000.tar.gz"));
    touch(locations.packageData() + QStringLiteral("/forkmesh-timers-data.json"));
    QDir().mkpath(locations.dataRoot + QStringLiteral(".bak-20260915-080000"));
    touch(locations.dataRoot + QStringLiteral(".bak-20260915-080000/identity.key"), "key");
    QDir().mkpath(locations.dataRoot + QStringLiteral(".bak-later"));
    touch(locations.settingsFile + QStringLiteral(".bak-20260915-080000"));
    touch(locations.fleet + QStringLiteral("/rqlite-20260926T030000Z.sqlite3.gz.age"));
    touch(locations.fleet + QStringLiteral("/rqlite-20260925T030000Z.sqlite3.gz"));
    touch(locations.fleet + QStringLiteral("/rqlite-latest.sqlite3.gz"));

    const QHash<QString, QString> labels = {
        {QStringLiteral("forkmesh-backup-20260101-090000.tar.gz"), QStringLiteral("Before rekey")}};
    const QList<BackupItem> items = BackupShelf::scan(locations, labels);

    check(items.size() == 8, "only recognised backups are listed, one row each");
    check(find(items, Shelf::Snapshots, QStringLiteral("forkmesh-backup-20260921-100000.tar.gz")),
          "rotating snapshots are on the Snapshots shelf");
    check(!find(items, Shelf::Snapshots,
                QStringLiteral("forkmesh-backup-20260921-110000.tar.gz.part")),
          "an unfinished snapshot is not offered as a backup");
    const BackupItem *kept =
        find(items, Shelf::Kept, QStringLiteral("forkmesh-backup-20260101-090000.tar.gz"));
    check(kept && kept->label == QStringLiteral("Before rekey"),
          "a kept snapshot carries its label");
    check(find(items, Shelf::PackageData, QStringLiteral("forkmesh-timers-data.json")),
          "a saved package data copy is on its own shelf");
    const BackupItem *aside =
        find(items, Shelf::MovedAside, QStringLiteral("data.bak-20260915-080000"));
    check(aside && aside->directory && aside->bytes < 0,
          "a data folder a restore moved aside is listed and measured later");
    check(find(items, Shelf::MovedAside, QStringLiteral("ForkMesh.conf.bak-20260915-080000")),
          "a settings file a restore moved aside is listed");
    check(!find(items, Shelf::MovedAside, QStringLiteral("data.bak-later")),
          "a folder without a restore stamp is left alone");
    const BackupItem *fleet =
        find(items, Shelf::Fleet, QStringLiteral("rqlite-20260926T030000Z.sqlite3.gz.age"));
    check(fleet && fleet->encrypted, "an age-encrypted fleet backup is marked encrypted");

    int firstFleet = -1;
    for (int i = 0; i < items.size(); ++i) {
        if (items.at(i).shelf == Shelf::Fleet) {
            firstFleet = i;
            break;
        }
    }
    check(firstFleet >= 0 &&
              items.at(firstFleet).name == QStringLiteral("rqlite-20260926T030000Z.sqlite3.gz.age"),
          "each shelf lists its newest backup first");
}

void testTimestamps()
{
    const QDateTime fleet =
        BackupShelf::fleetTimestamp(QStringLiteral("rqlite-20260926T030000Z.sqlite3.gz"));
    check(fleet.isValid() &&
              fleet.toUTC() == QDateTime(QDate(2026, 9, 26), QTime(3, 0), QTimeZone::utc()),
          "fleet backup names are read as UTC");
    check(!BackupShelf::fleetTimestamp(QStringLiteral("rqlite-20260926T030000Z.sqlite3")).isValid(),
          "a fleet name without the gzip suffix is not a backup");
    check(BackupShelf::movedAsideTimestamp(QStringLiteral("data.bak-20260915-080000"),
                                           QStringLiteral("data")) ==
              QDateTime(QDate(2026, 9, 15), QTime(8, 0)),
          "a moved-aside stamp is the local restore time");
    check(!BackupShelf::movedAsideTimestamp(QStringLiteral("data.bak-20260915-080000x"),
                                            QStringLiteral("data"))
               .isValid(),
          "trailing text after the stamp is refused");
    check(BackupShelf::packageDataId(QStringLiteral("forkmesh-voice-changer-data.json")) ==
              QStringLiteral("voice-changer"),
          "a package data copy names its package");
}

void testWhatEachShelfAllows()
{
    QTemporaryDir root;
    const Locations locations = sandbox(root);
    touch(locations.snapshots + QStringLiteral("/forkmesh-backup-20260921-100000.tar.gz"));
    touch(locations.kept() + QStringLiteral("/forkmesh-backup-20260101-090000.tar.gz"));
    QDir().mkpath(locations.dataRoot + QStringLiteral(".bak-20260915-080000"));
    touch(locations.fleet + QStringLiteral("/rqlite-20260925T030000Z.sqlite3.gz"));
    const QList<BackupItem> items = BackupShelf::scan(locations, {});

    const BackupItem *snapshot =
        find(items, Shelf::Snapshots, QStringLiteral("forkmesh-backup-20260921-100000.tar.gz"));
    const BackupItem *kept =
        find(items, Shelf::Kept, QStringLiteral("forkmesh-backup-20260101-090000.tar.gz"));
    const BackupItem *aside =
        find(items, Shelf::MovedAside, QStringLiteral("data.bak-20260915-080000"));
    const BackupItem *fleet =
        find(items, Shelf::Fleet, QStringLiteral("rqlite-20260925T030000Z.sqlite3.gz"));
    if (!snapshot || !kept || !aside || !fleet) {
        check(false, "the permission fixture scanned");
        return;
    }

    check(BackupShelf::canKeep(*snapshot) && !BackupShelf::canKeep(*kept),
          "only a rotating snapshot can be kept");
    check(BackupShelf::canRelease(*kept) && !BackupShelf::canRelease(*snapshot),
          "only a kept snapshot can go back into rotation");
    check(BackupShelf::keptPath(*snapshot, locations) ==
              locations.kept() + QStringLiteral("/forkmesh-backup-20260921-100000.tar.gz"),
          "keeping moves the snapshot into the kept folder");
    check(BackupShelf::releasedPath(*kept, locations) ==
              locations.snapshots + QStringLiteral("/forkmesh-backup-20260101-090000.tar.gz"),
          "releasing moves it back beside the rotating snapshots");
    check(BackupShelf::canRestore(*snapshot) && BackupShelf::canRestore(*kept) &&
              !BackupShelf::canRestore(*fleet) && !BackupShelf::canRestore(*aside),
          "only desktop snapshots are restored from this page");
    check(BackupShelf::canDelete(*snapshot, locations) && BackupShelf::canDelete(*kept, locations) &&
              BackupShelf::canDelete(*aside, locations),
          "desktop backups can be deleted");
    check(!BackupShelf::canDelete(*fleet, locations),
          "fleet backups are never deleted here; their own retention owns them");
    check(!BackupShelf::canCopy(*aside) && BackupShelf::canCopy(*fleet),
          "files can be copied elsewhere, whole folders cannot");

    BackupItem moved = *snapshot;
    moved.path = root.filePath(QStringLiteral("elsewhere/") + moved.name);
    touch(moved.path);
    check(!BackupShelf::canDelete(moved, locations),
          "a file outside its shelf's folder is never deleted");
    BackupItem live = *aside;
    live.path = locations.dataRoot;
    live.name = QStringLiteral("data");
    check(!BackupShelf::canDelete(live, locations), "the live data folder is never deletable");
    BackupItem renamed = *aside;
    renamed.name = QStringLiteral("data.bak-20260915-080001");
    check(!BackupShelf::canDelete(renamed, locations),
          "a row whose name no longer matches its path is refused");
    QFile::remove(snapshot->path);
    check(!BackupShelf::canDelete(*snapshot, locations), "a vanished file is not deletable");

    check(BackupShelf::verifyKind(*kept) == VerifyKind::TarGzip &&
              BackupShelf::verifyArguments(*kept) ==
                  QStringList({QStringLiteral("-tzf"), kept->path}),
          "a snapshot is verified by reading the whole archive");
    check(BackupShelf::verifyKind(*fleet) == VerifyKind::Gzip,
          "a plain fleet backup is verified with gzip -t");
    BackupItem sealed = *fleet;
    sealed.encrypted = true;
    check(BackupShelf::verifyKind(sealed) == VerifyKind::None,
          "an encrypted fleet backup needs the offline key and is not verified here");
}

void testPackageData()
{
    QString id;
    int keys = -1;
    check(BackupShelf::packageDataValid(
              R"({"format":"forkmesh-package-data","formatVersion":1,"packageId":"timers","data":{"a":1,"b":2}})",
              &id, &keys) &&
              id == QStringLiteral("timers") && keys == 2,
          "a package data copy reads back with its package and size");
    check(!BackupShelf::packageDataValid(R"({"format":"other","formatVersion":1,"packageId":"timers","data":{}})"),
          "another JSON format is refused");
    check(!BackupShelf::packageDataValid(R"({"format":"forkmesh-package-data","formatVersion":1,"packageId":"../x","data":{}})"),
          "a package id that is not a package name is refused");
    check(!BackupShelf::packageDataValid("{"), "truncated JSON is refused");
}

void testLabels()
{
    QHash<QString, QString> labels;
    labels.insert(QStringLiteral("a.tar.gz"), QStringLiteral("  before   the\tmove  "));
    labels.insert(QStringLiteral("../evil"), QStringLiteral("x"));
    labels.insert(QStringLiteral("b.tar.gz"), QStringLiteral("   "));
    labels.insert(QStringLiteral("c.tar.gz"), QString(500, QLatin1Char('z')));
    const QHash<QString, QString> round =
        BackupShelf::parseLabels(BackupShelf::serializeLabels(labels));
    check(round.value(QStringLiteral("a.tar.gz")) == QStringLiteral("before the move"),
          "labels are stored with their whitespace tidied");
    check(!round.contains(QStringLiteral("../evil")) && !round.contains(QStringLiteral("b.tar.gz")),
          "path-like names and blank labels are dropped");
    check(round.value(QStringLiteral("c.tar.gz")).size() == BackupShelf::kMaxLabelLength,
          "a long label is cut to the limit");
    check(BackupShelf::parseLabels("[1,2]").isEmpty() && BackupShelf::parseLabels("nope").isEmpty(),
          "a hand-edited label store that is not an object is ignored");
}

BackupItem snapshotAt(const QString &name, const QDateTime &taken, Shelf shelf = Shelf::Snapshots)
{
    BackupItem item;
    item.shelf = shelf;
    item.name = name;
    item.taken = taken;
    item.bytes = 100;
    return item;
}

void testRotationAndHealth()
{
    const QDateTime now(QDate(2026, 9, 27), QTime(12, 0));
    QList<BackupItem> items;
    for (int i = 0; i < 4; ++i)
        items.append(snapshotAt(QStringLiteral("s%1").arg(i), now.addSecs(-3600 * (i + 1))));
    items.append(snapshotAt(QStringLiteral("k"), now.addDays(-90), Shelf::Kept));

    check(BackupShelf::leavingAtNextBackup(items, 3) ==
              QSet<QString>({QStringLiteral("s2"), QStringLiteral("s3")}),
          "the next backup pushes out everything past the newest keep-1");
    check(BackupShelf::leavingAtNextBackup(items, 10).isEmpty(),
          "nothing leaves while rotation has room");
    check(!BackupShelf::leavingAtNextBackup(items, 1).contains(QStringLiteral("k")),
          "a kept snapshot never leaves with rotation");

    const auto totals = BackupShelf::totals(items);
    check(totals.value(Shelf::Snapshots).count == 4 &&
              totals.value(Shelf::Snapshots).bytes == 400 &&
              totals.value(Shelf::Kept).count == 1 && totals.value(Shelf::Fleet).count == 0,
          "each shelf totals its own backups");

    check(!BackupShelf::health(items, true, now).warning, "an hour-old snapshot is healthy");
    check(BackupShelf::health(items, true, now.addSecs(4 * 3600)).warning,
          "hourly backups that stopped running are flagged");
    check(BackupShelf::health({}, false, now).warning, "no snapshot at all is flagged");
    check(!BackupShelf::health({}, true, now).warning,
          "a fresh desktop with hourly backups on is waiting, not failing");
    check(BackupShelf::ageText(now.addSecs(-7200), now) == QStringLiteral("2 hours ago") &&
              BackupShelf::ageText(now.addDays(-1), now) == QStringLiteral("1 day ago"),
          "ages read the way people say them");
}

void testSearchAndFleetDirectory()
{
    BackupItem item = snapshotAt(QStringLiteral("forkmesh-backup-20260921-100000.tar.gz"),
                                 QDateTime::currentDateTime(), Shelf::Kept);
    item.label = QStringLiteral("Before rekey");
    check(BackupShelf::matches(item, QStringLiteral("rekey kept")),
          "every search word must match the name, label or shelf");
    check(!BackupShelf::matches(item, QStringLiteral("rekey fleet")), "a missing word hides the row");
    check(BackupShelf::itemId(item) ==
              QStringLiteral("kept:forkmesh-backup-20260921-100000.tar.gz"),
          "search ids name the shelf and file");

    check(BackupShelf::fleetDirectory(QString(), QString(), QStringLiteral("/h")) ==
              QStringLiteral("/h/data/fleet/backups"),
          "fleet backups default to the operator's fleet data folder");
    check(BackupShelf::fleetDirectory(QStringLiteral("/b"), QStringLiteral("/d"),
                                      QStringLiteral("/h")) == QStringLiteral("/b"),
          "FMNEXT_BACKUPS_DIR wins, as it does for next_backup.py");
    check(BackupShelf::fleetDirectory(QString(), QStringLiteral("/d"), QStringLiteral("/h")) ==
              QStringLiteral("/d/backups"),
          "FMNEXT_DATA_DIR moves the fleet backups with it");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testScanSortsEveryShelf();
    testTimestamps();
    testWhatEachShelfAllows();
    testPackageData();
    testLabels();
    testRotationAndHealth();
    testSearchAndFleetDirectory();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}

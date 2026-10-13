# Backups

One rail page in the desktop for every backup ForkMesh keeps on this computer,
sorted onto shelves. Free, bundled with the desktop, off by default; install
it from the Market.

## Shelves

| Shelf | Where | What you can do |
| --- | --- | --- |
| Snapshots | `<data>/backups/forkmesh-backup-<stamp>.tar.gz` | The hourly and "Back up now" snapshots. Rotation keeps the newest *N* (the "in rotation" box, the same `backup/keepCount` setting as Settings → Data); a row marked *Removed at the next backup* is the next to go. Keep, label, verify, restore, copy, show, delete. |
| Kept | `<data>/backups/kept/` | Snapshots you kept. The desktop's rotation lists only the files directly in `<data>/backups`, so it never sees them. Return to rotation, label, verify, restore, copy, show, delete. |
| Package data | `<data>/package-data/forkmesh-<id>-data.json` | Copies saved when a `localData` package was removed from the rail. Verify reads the bundle back; reinstalling the package imports it. |
| Moved aside | `<data>.bak-<stamp>/`, `<settings>.bak-<stamp>` | What a restore moved aside before replacing the live data folder and settings file. Folder sizes are measured off the GUI thread. Delete once the restored desktop works. |
| Fleet database | `~/projects/forkmesh/data/fleet/backups/rqlite-*.sqlite3.gz[.age]` (`FMNEXT_BACKUPS_DIR`, then `FMNEXT_DATA_DIR/backups`, as in `tools/next_backup.py`) | Read-only. Shown only when the folder has backups. Plain files verify with `gzip -t`; `.age` files need the offline key, so verify them with `tools/next_backup.py verify`. |

The page header says how old the newest snapshot is and warns when hourly
backups are on but have not run for two hours, or when there is no snapshot at
all. The top search box filters the page and finds backups by file name, label
or shelf.

## How it reaches the desktop

The package never includes `MainWindow`. Taking a snapshot, restoring one and
the hourly/rotation settings stay with the desktop (see D5 in
`docs/marketplace-extraction-inventory.md`); the desktop publishes them through
the neutral `desktop/src/BackupHostFeed.h` socket (`backupfeed::Host`), and
every Settings → Data backup status line reaches the page as a
`backupfeed::Listener::backupStatus` call, which rescans it. Restore goes
through the desktop's own confirmation, which moves the live data aside and
restarts.

Delete checks the row again right before removing it: the file or folder must
still be directly inside its shelf's folder, match that shelf's name pattern
exactly and not be a symlink, and a moved-aside folder can never be the live
data folder. Fleet backups are never deleted here; `tools/next_backup.py prune`
owns their retention.

## Data

Labels are the package's only data: a JSON object from file name to label in
`market/packages/backups/data/labels` (`dataLifecycle.localData: true`), so
removing the package offers to save them and installing offers to import them.
Labels follow a snapshot between Snapshots and Kept because the file name does
not change. Removing the package never touches a backup file.

## Code

- `content/desktop/BackupShelf.{h,cpp}`: scanning, what each shelf allows,
  verification commands, labels, rotation preview, health. Qt Core only.
- `content/desktop/BackupsFeature.{h,cpp}`: the rail page and its
  `builtin:backups` registration.
- Tests: `forkmesh-backups-tests` (`content/desktop/tests/test_backup_shelf.cpp`),
  run against temporary folders.

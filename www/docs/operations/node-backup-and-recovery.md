# Node backup, recovery, and key moves

`forkmesh-mirror-node` backs up, restores, and checks the node it runs, with no
extra tooling. The operations are:

| Command | What it does |
| --- | --- |
| `--preflight` | Read-only checks of configuration, identity, Git version, disk space, and backup/drill freshness |
| `--backup PATH` | Writes one encrypted archive of everything the node needs to come back |
| `--restore-drill PATH` | Restores an archive into scratch space, checks it, times it against the recovery time objective, then deletes it |
| `--restore PATH` | Restores an archive onto a host, checks it, and removes what it created if any check fails |
| `--export-identity PATH` / `--import-identity PATH` | Moves only the node's Ed25519 identity key |

Every command also takes `--config` (the service configuration, which is
`/var/lib/forkmesh/mirror-node.json` on installer-managed hosts). Results are
printed as JSON. Run these commands as the service account so that files keep
the owner the service needs:

```sh
node() {
  runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node \
    --config /var/lib/forkmesh/mirror-node.json "$@"
}
```

## Passphrases

Archives and identity exports contain the node's private key and its connector
token, so they are always encrypted: AES-256-GCM in 1 MiB authenticated chunks,
with the key derived from a passphrase using PBKDF2-SHA256. A wrong passphrase,
a changed byte, a missing chunk, or a cut-off file all fail to open.

The passphrase is read only from `--passphrase-file`. That file must belong to
the account running the command, must not be readable by anyone else, and must
hold at least 12 bytes. It is never accepted as a command-line argument or an
environment variable. Keep a copy of the passphrase **somewhere other than the
node**: a backup cannot be opened without it.

```sh
install -o forkmesh-node -g forkmesh-node -m 0600 /dev/null /var/lib/forkmesh/backup.passphrase
openssl rand -base64 32 > /var/lib/forkmesh/backup.passphrase   # then store a copy off-host
```

## Preflight

```sh
node --preflight          # human-readable
node --preflight --json   # for scripts and monitoring
```

Each check reports `ok`, `warn`, or `error`. The command exits `2` only when
there is an error, meaning the service cannot work as configured:

- the configuration or gateway configuration does not load;
- the gateway script, Python, the identity key, or `git` is missing;
- `git` is older than 2.27, which atomic merges and restores need;
- the identity key does not match the public key the gateway advertises (the
  daemon will refuse to start);
- the connector token is not owner-only;
- a volume holding node state has less than 512 MiB free.

Warnings need attention but do not stop the service:

- the node reports version `dev`;
- no connector token has been written yet;
- a repository has not been cloned yet;
- free space is under 10% of the volume, or is less than the repositories take
  up (a backup or repack needs about that much again);
- fewer than 5% of inodes are free;
- the last backup is older than 24 hours (the recovery point objective);
- the last restore drill is more than 92 days old, took longer than its
  objective, or has never been run.

Run preflight before and after installing or updating a node, and from
monitoring. If it fails after an update, reinstall the previous release binary
(see [rolling migration](mirror-node.md#rolling-migration)) before you debug.

## Backups

```sh
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --backup /var/backups/forkmesh/node-$(date -u +%Y%m%dT%H%M%SZ).fmbackup
```

The service can keep running during a backup. A backup contains:

- the service configuration and the gateway configuration;
- the identity key and the connector token;
- the whole gateway state directory (`mirror-gateway/`): the signed mirror
  manifest, dashboard replica, pull-ledger job results, agent job state, and
  node-ops records;
- every path the gateway configuration names: repository `gitDir`s, release
  stores, `privateReplicaStore`, `dynamicRepositoriesPath` (the fork
  registry), `writableRepositoryRoot`, and operator policy state.

Each bare repository is stored as a `git bundle --all`, which is a consistent
snapshot of its refs and objects even while pushes are arriving. The
repository's `HEAD`, `config`, hooks, and release store are stored beside the
bundle.

Temporary data is left out: `tmp/` and `.cache/` directories, agent job
worktrees (`agent-jobs/work/`), merge worktrees (`pull-jobs/.apply-*`), and
half-written temporary files. Sign-ins for agent CLIs (Claude Code, Codex) are
also left out. They are third-party credentials, so sign in again after a
restore.

The archive is written under a temporary name and then linked into place, so
it never replaces an existing file. Symbolic links inside the backed-up state
are refused. The one exception is a configured path that is itself a link to
another disk; its contents are backed up under the configured name.

Each backup is recorded in `mirror-gateway/node-ops/last-backup.json`. That
record contains the size, SHA-256, duration, and ref count, and no secrets.

### Scheduling

```ini
# /etc/systemd/system/forkmesh-node-backup.service
[Unit]
Description=ForkMesh node backup

[Service]
Type=oneshot
User=forkmesh-node
Group=forkmesh-node
ExecStart=/bin/sh -c 'exec /usr/local/bin/forkmesh-mirror-node --config /var/lib/forkmesh/mirror-node.json --passphrase-file /var/lib/forkmesh/backup.passphrase --backup /var/backups/forkmesh/node-$(date -u +%%Y%%m%%dT%%H%%M%%SZ).fmbackup'
ExecStartPost=/usr/bin/find /var/backups/forkmesh -name 'node-*.fmbackup' -mtime +14 -delete

# /etc/systemd/system/forkmesh-node-backup.timer
[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
```

Copy archives off the host as well. A backup that exists only on the node does
not survive losing the node.

## Restore drills

```sh
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --restore-drill /var/backups/forkmesh/node-20260913T020000Z.fmbackup --rto 30m
```

A drill decrypts the whole archive into a scratch directory under `--work-dir`
(default `TMPDIR`). It then checks all of the following:

- every file's size, mode, and SHA-256;
- that each repository's refs digest matches the backup, and that
  `git fsck --connectivity-only` passes;
- that the identity key loads and matches the recorded public key;
- that the service and gateway configurations load and name that identity.

The drill then deletes the scratch directory. It changes nothing on the node
except the record in `mirror-gateway/node-ops/last-restore-drill.json`. It
exits `3` if it took longer than `--rto` (default 30 minutes).

Run a drill at least once a quarter. Preflight warns once the last drill is
more than 92 days old. `/v1/status` shows both objectives under `recovery`:

```json
"recovery": {
  "lastBackupAt": "2026-09-13T02:00:04.1Z",
  "lastBackupAgeSeconds": 3600,
  "lastBackupBytes": 734003200,
  "lastBackupDurationMs": 81234,
  "lastRestoreDrillAt": "2026-09-01T03:10:12.9Z",
  "lastRestoreDrillAgeSeconds": 1036800,
  "lastRestoreDrillDurationMs": 95310,
  "recoveryTimeObjectiveMs": 1800000,
  "lastRestoreDrillWithinObjective": true
}
```

## Restoring a node

To replace a lost or broken host:

1. Install ForkMesh on the new host as usual, but do **not** bootstrap a new
   identity or start `forkmesh-mirror-node.service`. The service account and
   `/var/lib/forkmesh` must exist; the repositories and configuration must
   not.
2. Copy the archive and the passphrase file to the new host, owned by
   `forkmesh-node` with mode `0600`.
3. Optionally rehearse with `--dry-run`, which runs the same checks as a drill
   without touching the host:

   ```sh
   node --passphrase-file /var/lib/forkmesh/backup.passphrase \
     --restore /var/lib/forkmesh/node.fmbackup --dry-run
   ```

4. Restore:

   ```sh
   node --passphrase-file /var/lib/forkmesh/backup.passphrase \
     --restore /var/lib/forkmesh/node.fmbackup
   ```

5. Run `node --preflight`, then `systemctl enable --now forkmesh-mirror-node`,
   and confirm `/readyz`.

A restore puts every file back at its original absolute path. Use
`--restore-root DIR` to place them all under another directory instead. It
**never overwrites** an existing file. It refuses to restore into an existing
repository directory, and it refuses to run as root (restored files would
belong to root, and the service could not read them). After the files are
written it runs the same checks as a drill. If any step fails, it deletes
every file and directory it created and nothing else, so you can fix the
problem and run it again. `--config` must match the configuration path stored
in the backup.

The restored node has the same name and key, so it takes over the old node's
catalog entry, endpoint lease, and account without re-linking. Do not run the
old host and its restored copy at the same time.

## Moving or replacing an identity key

To move only the key, for example onto a node that already has its
repositories:

```sh
# on the old host
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --export-identity /var/lib/forkmesh/mirror17.fmkey

# on the new host (same node name)
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --import-identity /var/lib/forkmesh/mirror17.fmkey --dry-run
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --import-identity /var/lib/forkmesh/mirror17.fmkey --replace-identity
```

An export is tied to its node name, and importing it into a node with any other
name is refused. The import reports one of three actions:

- `install`: the host had no key, so the imported key is installed.
- `unchanged`: the host already has this exact key.
- `replace`: the host has a different key. This requires `--replace-identity`.
  The current key is kept beside the new one as `ed25519.pem.replaced-<unix
  time>` and is never deleted.

When the key changes, the import also updates the public key in the gateway
configuration (the daemon refuses to start if they differ). On a tunnel-free
node it also re-signs the direct-origin manifest. Restart the service
afterwards.

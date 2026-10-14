# Node backup, recovery, and key moves

`forkmesh-mirror-node` backs up, restores, and checks the node it runs:

| Command | What it does |
| --- | --- |
| `--preflight` | Read-only checks of configuration, identity, Git version, disk space, and backup/drill freshness |
| `--backup PATH` | Writes one encrypted archive of everything the node needs |
| `--restore-drill PATH` | Restores into scratch space, verifies, times it against the RTO, then deletes it |
| `--restore PATH` | Restores onto a host, verifies, and removes what it created if a check fails |
| `--export-identity PATH` / `--import-identity PATH` | Moves only the Ed25519 identity key |

Every command takes `--config` and prints JSON. Run them as the service
account:

```sh
node() {
  runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node \
    --config /var/lib/forkmesh/mirror-node.json "$@"
}
```

## Passphrases

Archives and identity exports contain the private key and connector token, so
they are encrypted with AES-256-GCM in 1 MiB authenticated chunks, keyed by
PBKDF2-SHA256. Any tampering or truncation fails to open. The passphrase is read
only from `--passphrase-file`, which must be owner-only and at least 12 bytes;
it is never accepted as an argument or environment variable. Keep a copy off
the node.

## Preflight

`node --preflight [--json]` reports `ok`, `warn`, or `error` per check and
exits `2` on any error: configuration does not load, a required binary or the
identity key is missing, `git` is older than 2.27, the key does not match the
advertised public key, the connector token is not owner-only, or a state volume
has under 512 MiB free. Warnings cover low free space or inodes, uncloned
repositories, a backup older than 24 hours (RPO), and a restore drill older
than 92 days, over its objective, or never run. Run preflight before and after
every install or update; if it fails after an update, reinstall the previous
release (see [rolling migration](mirror-node.md#rolling-migration)).

## Backups

```sh
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --backup /var/backups/forkmesh/node-$(date -u +%Y%m%dT%H%M%SZ).fmbackup
```

The service can keep running. An archive contains the service and gateway
configurations, identity key, connector token, the gateway state directory
(`mirror-gateway/`), and every path the gateway configuration names
(repositories, release stores, private replica store, fork registry, writable
root, operator policy state). Each bare repository is stored as a
`git bundle --all` snapshot plus its `HEAD`, `config`, hooks, and releases.

Excluded: `tmp/`, `.cache/`, agent and merge worktrees, partial files, and
agent CLI sign-ins (sign in again after a restore). Archives are written under
a temporary name and never replace an existing file; symlinks in state are
refused. Each run is recorded, without secrets, in
`mirror-gateway/node-ops/last-backup.json`.

Schedule backups daily (for example a systemd timer running the command above
as `forkmesh-node`, pruning old archives) and copy archives off the host.

## Restore drills

```sh
node --passphrase-file /var/lib/forkmesh/backup.passphrase \
  --restore-drill /var/backups/forkmesh/node-<timestamp>.fmbackup --rto 30m
```

A drill decrypts into `--work-dir`, checks every file's size, mode, and
SHA-256, each repository's refs digest and `git fsck --connectivity-only`, and
that the key and configurations load and agree. It then deletes the scratch
copy, records `last-restore-drill.json`, and exits `3` if it exceeded `--rto`.
Run one at least quarterly. `/v1/status` reports both objectives under
`recovery`.

## Restoring a node

1. Install ForkMesh on the new host without bootstrapping an identity or
   starting the service.
2. Copy the archive and passphrase file over, owned by `forkmesh-node`, mode
   `0600`.
3. Optionally rehearse: `node --passphrase-file ... --restore <archive> --dry-run`.
4. Restore: `node --passphrase-file ... --restore <archive>`.
5. Run `node --preflight`, enable the service, and confirm `/readyz`.

Files return to their original absolute paths (or under `--restore-root DIR`).
A restore never overwrites existing files, refuses existing repository
directories, refuses to run as root, and on failure removes only what it
created. `--config` must match the path stored in the backup. The restored node
keeps its name and key, so it takes over the old node's catalog entry and
lease; never run the old and restored hosts at once.

## Moving or replacing an identity key

```sh
node --passphrase-file ... --export-identity /var/lib/forkmesh/node.fmkey          # old host
node --passphrase-file ... --import-identity /var/lib/forkmesh/node.fmkey --dry-run  # new host
node --passphrase-file ... --import-identity /var/lib/forkmesh/node.fmkey --replace-identity
```

An export is bound to its node name. Import reports `install` (no key yet),
`unchanged` (same key), or `replace` (different key; requires
`--replace-identity` and keeps the old key as `ed25519.pem.replaced-<unix time>`).
A changed key also updates the gateway's advertised public key and, on a
tunnel-free node, re-signs the direct-origin manifest. Restart the service
afterwards.

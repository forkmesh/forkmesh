# Headless mirror refresh and publication

`tools/headless_mirror_refresh.py` is the fail-closed bridge between an
SSH-writable bare Git repository and `mirror_gateway.py`. It never loads a node
private key, age identity, API token or wallet key: all signing and encryption
go through the bounded protocol of `headless_mirror_identity.py`.

## Modes

| Mode | What it does |
| --- | --- |
| `refresh` | `git fsck --full --strict` on the bare source, new age-encrypted snapshot, staged gateway config validated with `mirror_gateway.py --check`, then atomically installed |
| `register` | Re-validates the active generation, then signs and POSTs the endpoint registration (`/api/mirrors/https`) and the catalog-v2 record (`/api/repositories`) |
| `renew` | Re-authenticates the active generation and republishes the signed endpoint/catalog lease, without fsck or a second materialization |
| `check` | Full active-state validation with no network and no reseal (too slow for `ExecStartPre`; use the gateway's own `--check` there) |
| `configure-actions` | Reads one boolean request on stdin and changes only `catalog.actionsEnabled`, then publishes like `renew` |

The intended sequence is `refresh` → restart the gateway → `register`, so the
catalog never advertises refs the running gateway does not serve. A failed
`register` leaves the valid generation active and can simply be retried.

## Configuration

The contract is `www/docs/headless-mirror-refresh.schema.json`
(`type: forkmesh.headless-mirror-refresh`). The runtime also rejects duplicate,
unknown and secret-shaped fields, oversized JSON, unsafe modes, wrong owners and
symlinks. Key fields:

- Paths: `sourceRepository` (a real bare repository), `archiveDirectory`,
  `gatewayConfigPath`, `identityStateDirectory`, `manifestPath`, optional
  `releaseStore`, and the program paths for Python, Git, the gateway and the
  identity helper.
- Identity: `nodeOwner` must match the identity helper's `public-info` and
  appear in `ownerAliases`; every alias is a gateway route onto the same refs,
  but only `nodeOwner` is published to the catalog.
- Origins: `workerOrigin`, `publicOrigin` and optional `mirrorUrl` are
  credential-free HTTPS origins pinned by the node identity.
- `operations`: the read operations to serve. `merge-pull` adds a fixed
  `mergeExecutorCommand` that verifies the pinned pull, stages objects in an
  owner-only quarantine and journal, and moves `main` and `forkmesh/pulls` in one
  compare-and-swap. `actions-status` and `actions-control` expose the
  [Actions summary and control](mirror-actions-control.md) files.
  `git-receive-pack` is never accepted here.
- Push hosting: `writableRepositoryRoot`, `writableOwners` (named owners or
  `["*"]`) and `receiveMaxInputBytes`; absent on a read-only node. See
  [direct-HTTPS mirror gateway](direct-https-mirror-gateway.md).
- `catalog`: description, branch, `actionsEnabled`, and opt-in
  `reportCpu`/`reportMemory`/`reportDisk` aggregates. Unavailable readings are
  omitted, never reported as zero.

Never put a seed phrase, private key, token or password in this file.

## Storage and permissions

Run the identity helper, refresh tool and gateway as one dedicated
unprivileged account. The config is a single-link `0600` file; identity,
archive and gateway-state directories are `0700` and owned by that account;
programs are not group- or world-writable; no sensitive path may traverse a
symlink. Set `TMPDIR` to an owner-only disk-backed directory sized for two
expanded copies when `/tmp` is a small tmpfs. Under systemd keep
`ProtectSystem=strict` and grant `ReadWritePaths` only to the identity, archive,
gateway-state, temp and bare-source directories.

## Running it

```bash
python3 -I headless_mirror_refresh.py --config <mirror-refresh.json> refresh
python3 -I headless_mirror_refresh.py --config <mirror-refresh.json> register
```

Output is a minimal JSON object such as
`{"aliasCount":2,"event":"refresh_complete","ok":true}`; paths, names, Git
output and remote bodies are never printed. HTTP calls disable redirects and
ambient proxies and bound response sizes.

`forkmesh-mirror-renew.timer` (in `server/packaging/systemd/`) runs `renew`
every four minutes, inside the ten-minute endpoint lease, so a dead mirror drops
out of routing. All modes share one owner-only lock.

After an SSH push (see [Git SSH gateway](git-ssh-gateway.md)) a systemd path
unit runs: `refresh` → restart gateway → verify signed health over loopback and
the public origin → `register`. Concurrent ref movement during sealing fails the
refresh so the next notification retries.

## Last-good and recovery

Archives are stored under content-addressed names and the generated gateway
JSON is the only active pointer, replaced atomically after fsck, sealing, ref
stability and `--check` all pass. Old ciphertext is never deleted
automatically. If `register` fails, confirm the gateway is live and rerun
`check` and `register`; if `check` reports source drift, run the full sequence.

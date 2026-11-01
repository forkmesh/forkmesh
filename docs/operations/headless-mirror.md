# Headless mirror identity and refresh

Part of [Operations](README.md).

Two Python tools run a non-GUI mirror host beside the
[mirror gateway](direct-https-mirror-gateway.md): the identity helper holds the
node's keys and signs a fixed set of messages, and the refresh tool turns an
SSH-writable bare repository into a verified gateway generation and publishes
it. The Go [mirror node](mirror-node.md) daemon supervises the same gateway.
Background: [mirroring](../mirroring.md), [self-hosting](../self-hosting.md).

## Identity

`tools/headless_mirror_identity.py` is the local cryptographic boundary. It
runs as the same dedicated, unprivileged account as the gateway and:

- keeps one Ed25519 node identity and one age identity in an owner-only
  directory;
- signs only the bounded manifest, health, reclaim, registration, and public
  catalog-v2/state protocols, building each canonical message itself (there is
  no arbitrary-message signing);
- verifies routing capabilities only with the configured router public key.

It takes no private key, seed, token, or password on the command line. Requests
are bounded JSON on standard input; responses hold only public keys,
signatures, hashes, and public records.

Requirements: Python 3.12+, `cryptography`, `age`/`age-keygen`, and Git. The
identity directory must be an absolute path with mode `0700` (files `0600`),
outside any checkout, image, or shared repository.

### Initialize and configure

```bash
printf '%s\n' '{
  "schemaVersion": 1,
  "type": "forkmesh.headless-mirror-identity-init",
  "nodeName": "mirror-two",
  "routerPublicKey": "<MIRROR_ROUTER_PUBLIC_KEY>",
  "allowedOrigins": ["https://mirror.example.org"]
}' | python3 tools/headless_mirror_identity.py \
  --state-dir /var/lib/forkmesh/identity init
```

All values are public. `allowedOrigins` pins which hostnames the node key will
sign for. `init` is idempotent and reuses a valid existing key after an
interruption. `public-info` prints the public half. `configure`
(`forkmesh.headless-mirror-identity-configure`) changes only the router key and
origin pins, never the private identities.

### Gateway integration

Reference the helper from the gateway configuration as direct command arrays,
never through a shell:

```json
{
  "requestVerifierCommand": ["/usr/bin/python3", "/opt/forkmesh/tools/headless_mirror_identity.py",
    "--state-dir", "/var/lib/forkmesh/identity", "capability-verify"],
  "healthSignerCommand": ["/usr/bin/python3", "/opt/forkmesh/tools/headless_mirror_identity.py",
    "--state-dir", "/var/lib/forkmesh/identity", "health-sign"]
}
```

Public repositories are served straight from the bare repository with an
`integrity.expectedRefsSha256` pin; the age seal/materialize modes exist only
for upgrade compatibility. `manifest-sign` signs endpoint manifests for a
tunnel or public HTTPS origin, using `nodePublicKey` from `public-info`.

After each push, the refresh orchestrator computes the heads-and-tags pin and
atomically installs a gateway configuration pointing at the bare repository; it
skips archives and full fsck, which the periodic `check` command runs.

Run the service with a restrictive umask, no privilege escalation, a read-only
application tree, and write access only to identity, repository, and gateway
state. The gateway listens only on loopback behind a TLS proxy or tunnel.

### Operator-only signatures

These modes are for local operator use only, never exposed over HTTP:

- `sign-reclaim` (`forkmesh.account-reclaim-signing`: `nodeName`, six-digit
  `linkCode`) returns the `/api/accounts/reclaim-node` body.
- `sign-endpoint-registration` (`forkmesh.https-endpoint-registration-signing`:
  `node`, `baseUrl`) returns the `/api/mirrors/https` body.
- `sign-catalog-v2` (`forkmesh.catalog-v2-publication-signing`: a public
  `record` with `owner`, `name`, `visibility: "public"`, and `stateHash`)
  returns the record with `stateSig` and `catalogSig` for `/api/repositories`.

### Key handling and rotation

Back up the identity directory through an encrypted, owner-controlled process;
it cannot be recovered. File permissions do not protect against root or a
compromised service account; use an encrypted volume or hardware signer when
that matters. To rotate the router key, update the server first, then run
`configure` on each mirror. To rotate the node identity, provision a new state
directory, reclaim the node, republish manifests and catalog state, verify
health, and retire the old directory. Node-level backups and key moves are in
[node backup and recovery](node-backup-and-recovery.md).

## Refresh and publication

`tools/headless_mirror_refresh.py` is the fail-closed bridge between an
SSH-writable bare Git repository and `mirror_gateway.py`. It never loads a node
private key, age identity, API token or wallet key: all signing and encryption
go through the identity helper's bounded protocol ([Identity](#identity)).

### Modes

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

### Configuration

The contract is
[`headless-mirror-refresh.schema.json`](../headless-mirror-refresh.schema.json)
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

### Storage and permissions

Run the identity helper, refresh tool and gateway as one dedicated
unprivileged account. The config is a single-link `0600` file; identity,
archive and gateway-state directories are `0700` and owned by that account;
programs are not group- or world-writable; no sensitive path may traverse a
symlink. Set `TMPDIR` to an owner-only disk-backed directory sized for two
expanded copies when `/tmp` is a small tmpfs. Under systemd keep
`ProtectSystem=strict` and grant `ReadWritePaths` only to the identity, archive,
gateway-state, temp and bare-source directories.

### Running it

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

### Last-good and recovery

Archives are stored under content-addressed names and the generated gateway
JSON is the only active pointer, replaced atomically after fsck, sealing, ref
stability and `--check` all pass. Old ciphertext is never deleted
automatically. If `register` fails, confirm the gateway is live and rerun
`check` and `register`; if `check` reports source drift, run the full sequence.

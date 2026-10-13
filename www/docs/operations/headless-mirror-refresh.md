# Headless mirror refresh and publication

`tools/headless_mirror_refresh.py` is the fail-closed bridge between an
SSH-writable bare Git repository and `mirror_gateway.py`. It does not load a
node private key, an age identity, a Cloudflare token, or a wallet key. All
signing and encryption operations go through the bounded protocols in
`headless_mirror_identity.py`.

The refresh, publication, and renewal modes form an intentional deployment
sequence:

1. `refresh` checks the exact bare source with `git fsck --full --strict`,
   creates a new age-encrypted snapshot, validates a staged gateway
   configuration, and atomically installs that configuration.
2. The service manager restarts `mirror_gateway.py` against the new
   configuration.
3. `register` validates the active source/archive/configuration, registers the
   HTTPS endpoint, and then publishes the node-owner catalog-v2 record.
4. `renew` re-authenticates the immutable active generation and republishes the
   signed endpoint/catalog lease without a full Git fsck or a second encrypted
   repository materialization.

`configure-actions` is a separate owner-local control mode. It accepts one
bounded, secret-free boolean request on stdin, atomically changes only
`catalog.actionsEnabled` while preserving all other refresh fields and file
permissions, then performs the same signed endpoint/catalog publication as a
renewal. Workflow variables and other secret material are never accepted by
this mode and must never be placed in the refresh configuration.

`check` performs the full active-state validation from step 3 but makes no
network request and does not reseal the repository. That operator check
includes `git fsck --full --strict`, so do not put it on the gateway's
`ExecStartPre` path. The gateway's own `--check` mode is the bounded startup
guard: it validates the generated configuration, endpoint manifest,
materialized public repositories, and exact refs integrity pins without
repeating the source-repository fsck. Both the precheck and `ExecStart` must
read the same generated gateway configuration.

This separation prevents the public catalog from advertising new refs before
the gateway has restarted. If endpoint registration or catalog publication
fails, the valid local generation remains active and `register` can be retried.

## Storage and permissions

Run the identity helper, refresh tool, and gateway as the same dedicated
unprivileged account. The following are mandatory:

- The refresh JSON is a regular, single-link, owner-only file, normally mode
  `0600`.
- The identity, archive, and gateway-state directories are owned by that
  account and mode `0700`.
- The bare source is a real directory owned by that account and is not
  world-writable. Group write is permitted for a deliberately configured SSH
  repository-sharing group.
- The helper, gateway, Python, and Git programs are owned by root or the
  service account and are not group- or world-writable.
- The manifest is a real regular file owned by root or the service account and
  is not group- or world-writable.
- Sensitive paths may not be symbolic links or traverse symbolic-link
  components.

For example:

```bash
install -d -o forkmesh-mirror -g forkmesh-mirror -m 0700 \
  /var/lib/forkmesh-mirror/identity \
  /var/lib/forkmesh-mirror/encrypted \
  /var/lib/forkmesh-mirror/gateway

install -o root -g root -m 0644 \
  tools/headless_mirror_identity.py \
  tools/headless_mirror_refresh.py \
  tools/mirror_gateway.py \
  /opt/forkmesh/tools/
```

The source repository must already be a bare Git repository. Do not point the
tool at a working tree, an alternate-object overlay, or a symlink.

## Secret-free configuration

The exact contract is
`www/docs/headless-mirror-refresh.schema.json`. The runtime rejects duplicate
fields, unknown fields, secret-shaped fields, oversized JSON, unsafe file
modes, unexpected owners, and symlinks even when an external schema validator
was not run.

```json
{
  "schemaVersion": 1,
  "type": "forkmesh.headless-mirror-refresh",
  "sourceRepository": "/srv/git/forkmesh.git",
  "archiveDirectory": "/var/lib/forkmesh-mirror/encrypted",
  "releaseStore": "/var/lib/forkmesh-mirror/releases",
  "gatewayConfigPath": "/var/lib/forkmesh-mirror/gateway/mirror-gateway.json",
  "identityStateDirectory": "/var/lib/forkmesh-mirror/identity",
  "identityHelperPath": "/opt/forkmesh/tools/headless_mirror_identity.py",
  "mirrorGatewayPath": "/opt/forkmesh/tools/mirror_gateway.py",
  "pythonProgram": "/usr/bin/python3",
  "gitProgram": "/usr/bin/git",
  "manifestPath": "/var/lib/forkmesh-mirror/gateway/forkmesh-mirror.json",
  "nodeOwner": "mirror2",
  "repositoryName": "forkmesh",
  "ownerAliases": [
    "mirror2",
    "forkmesh"
  ],
  "workerOrigin": "https://app.forkmesh.com",
  "publicOrigin": "https://mirror2.forkmesh.com",
  "mirrorUrl": "https://git.mirror2.example.com",
  "listen": {
    "host": "127.0.0.1",
    "port": 8790
  },
  "operations": [
    "git-info-refs",
    "git-upload-pack",
    "tree",
    "blobs",
    "blob",
    "raw",
    "history",
    "commit",
    "compare",
    "branches",
    "search",
    "stats",
    "sizes",
    "release-blob",
    "merge-pull",
    "actions-status"
  ],
  "catalog": {
    "description": "ForkMesh mirror",
    "branch": "main",
    "platform": "git",
    "actionsEnabled": false,
    "reportCpu": false,
    "reportMemory": false,
    "reportDisk": false
  }
}
```

`merge-pull` is an explicit write capability. When present, the generated
gateway configuration includes a fixed `mergeExecutorCommand` pointing back to
this refresh controller. The public request supplies only a pull number,
idempotency id, and exact Git object ids. The node accepts branch-backed open
pulls whose committed metadata pins those ids, performs a hook-free atomic CAS
of `main` and `forkmesh/pulls`, reseals the repository, and republishes signed
health before reporting the merge complete. Omit `merge-pull` to keep a node
strictly read-only.

All object-producing Git plumbing runs first in a mode-0700, owner-only object
quarantine outside the bare repository. A conflict drops that quarantine
without importing an object. A successful candidate is durably journaled,
installed, and made reachable through hidden
`refs/forkmesh/merge-staging/<request>/...` refs before the public-ref CAS.
The same transaction advances both public refs, writes hidden completion
markers, and removes the staging pair. If the CAS loses a race, the staging refs
remain to keep every speculative object reachable. Startup/refresh replays a
valid owner-only journal before `git fsck`, so an interruption cannot strand
unreachable objects in the source. It never runs a broad prune.

The author's PullStore signature remains intact. The signature-covered title,
base, head, derived patch/mbox, author, and timestamp are preserved, as are the
immutable creation OIDs and signature bytes. Only the established owner-applied
lifecycle fields (`status`, `mergeBase`, and `mergeHead`) change. Idempotency
results live in bounded, mode-0600 node-owner state rather than a fetchable Git
object. The Worker keeps its blind-indexed routing record for seven days,
expires in bounded batches, and enforces both per-repository and global row
ceilings; in-flight jobs are never evicted merely to admit another merge.

`nodeOwner` must match the node name in the identity helper's `public-info`
response and must appear in `ownerAliases`. Every alias becomes a separate
public gateway route, but all entries point to the exact same plaintext bare
repository and refs digest. Only the `nodeOwner`
record is sent to `/api/repositories`, because catalog publication is bound to
that registered account's Ed25519 key. An organization alias such as
`forkmesh/forkmesh` remains an explicit Worker-side organization/repository
mapping; an alias does not grant organization membership or private access.

`workerOrigin`, `publicOrigin`, and the optional `mirrorUrl` are credential-free
HTTPS origins. The node identity must already pin every configured origin.
When `mirrorUrl` is absent, Git keeps using the masked Tunnel fallback. When it
is present, registration and the signed catalog advertise
`mirrorUrl/<nodeOwner>/<repositoryName>.git`; it is replaceable transport
metadata and never changes repository identity. `catalog.solana`, when present, is
only a public, self-custodial payout address. Never place a seed phrase, private
key, API token, password, or wallet credential in this file.

Host resource reporting is explicit and off by default. Set
`catalog.reportCpu`, `catalog.reportMemory`, or `catalog.reportDisk` to `true`
only when the operator wants that aggregate metric included in the node's
public, signed catalog record and World mirror display. CPU is a short aggregate
Linux `/proc/stat` sample; memory uses `MemTotal` and `MemAvailable`; disk uses
the filesystem containing `sourceRepository`. ForkMesh publishes only bounded
integer percentages or used/total byte pairs. Disabled, unavailable, partial,
or malformed readings are omitted and remain “not shared”—they are never
reported as zero. No process list, path, file name, repository content, or
per-process activity is collected.

Actions publication is also explicit and off by default.
`catalog.actionsEnabled` is the owner-approved upper bound; it does not by
itself claim that a workflow is running. The local executor writes
`actions-state.json` next to `gatewayConfigPath`, using the exact contract in
`www/docs/mirror-actions-state.schema.json`. A same-account deployment uses a
regular, single-link, producer-owned mode-0600 file. The packaged root control
daemon instead uses only the fixed
`/var/lib/forkmesh-mirror/gateway/actions-state.json` handoff: the parent
remains `forkmesh-mirror:forkmesh-mirror` mode 0700 and the atomically replaced
file is `root:forkmesh-mirror` mode 0640. The service-account reader requires
that exact parent owner/group/mode and accepts the group-readable form only at
that fixed path with root ownership, its own effective group, one link, and no
symbolic-link traversal. The service account can remove the file (causing a
safe fallback), but cannot forge a root-owned replacement. The node must match
`nodeOwner`, the state must be exactly `disabled`, `enabled`, or `running`, and
the update/expiry timestamps must fit a maximum 15-minute lease horizon. A
disabled catalog setting can never be elevated by a lease. Missing, unsafe,
malformed, stale, or implausibly dated leases fall back to the configured
enabled/disabled state without breaking ordinary mirror renewal. Workflow
definitions, variables, commands, paths, logs, and secret values are never
copied into the signed public catalog.

`actions-status` is a separate explicit read operation. When it is present,
the generated gateway configuration contains one fixed `actionsSummaryPath`
next to `gatewayConfigPath`; without the operation, that path is omitted. The
desktop executor atomically maintains `actions-summary.json` following
`www/docs/mirror-actions-summary.schema.json`. Same-account deployments retain
producer-owned mode 0600. The packaged root daemon may cross the account
boundary only through
`/var/lib/forkmesh-mirror/gateway/actions-summary.json`, with the same protected
parent and exact `root:forkmesh-mirror` mode-0640, regular, single-link contract
as the state lease. The gateway rechecks parent and file metadata on every
open, uses no-follow descriptors, and fails closed on replacement or spoofed
ownership. It contains at most the 20 newest runs, exact lifecycle timestamps,
and at most 16 KiB of valid UTF-8 from each ActionStore log tail. ActionRunner
redacts configured variable values before the log reaches ActionStore. The
summary never serializes workflow source, commands, variable maps, local
paths, or private repository data; a version 2 summary names workflows only by
their repository-relative `.forkmesh/*.yml` path. Its complete document is
limited to 256 KiB. Its positive expiry is ten minutes, below the enforced
15-minute maximum; the desktop refreshes the lease periodically, publishes
lifecycle changes on the next event-loop turn, and coalesces high-volume
running output rather than writing once per line.

`actions-control` is the matching explicit control operation and is accepted
only together with `actions-status`. When it is present, the cycle creates the
mode-0700 `actions-control` directory next to `gatewayConfigPath` if it is
absent and renders it as `actionsControlPath`; without the operation, the key
is omitted. The gateway queues router-signed control envelopes there and the
executor verifies and consumes them, as specified in
[Mirror Actions control and summary v2](mirror-actions-control.md). No derived,
hosted, or push-created repository inherits either Actions operation.

`writableRepositoryRoot`, `writableOwners`, and `receiveMaxInputBytes` are the
optional push-hosting keys. They are absent on a read-only node, and their
absence is what makes it impossible for this cycle to render `git-receive-pack`
for any repository. When present they are rendered verbatim into the generated
gateway configuration, and the cycle additionally consumes the gateway's
`hosted-pushes.json` sidecar next to `gatewayConfigPath` to render the
repositories an authorized first push created. `writableOwners` accepts the
single entry `["*"]` — every owner the control plane authorized — for a node
that holds repositories created from the website, whose owners it cannot
enumerate; the wildcard may not be mixed with named owners. `operations` itself stays
read-only: adding `git-receive-pack` there is rejected. The full operator story
- directory permissions, what appears where, who publishes the catalog record,
how push-created repositories are exempted from the immutable refs pin, and how
to verify a push became routable - is in
[`direct-https-mirror-gateway.md`](direct-https-mirror-gateway.md) under
"Push-created repositories".

`releaseStore` is optional. When configured, it must be an existing owner-only
directory containing the node's content-addressed `sha256/<prefix>/<digest>/data`
release assets. It is rendered onto every public alias backed by this public
repository so immutable release downloads can round-robin with clone and browse
traffic. The refresh never copies assets into that directory and never treats
it as repository source; operators must replicate a published blob and verify
its SHA-256 before exposing it.

Install the final configuration with:

```bash
install -o forkmesh-mirror -g forkmesh-mirror -m 0600 \
  /path/to/mirror-refresh.json \
  /var/lib/forkmesh-mirror/gateway/mirror-refresh.json
```

## Initial refresh and service validation

Run as the dedicated service account:

```bash
/usr/bin/python3 /opt/forkmesh/tools/headless_mirror_refresh.py \
  --config /var/lib/forkmesh-mirror/gateway/mirror-refresh.json \
  refresh

/usr/bin/python3 /opt/forkmesh/tools/headless_mirror_refresh.py \
  --config /var/lib/forkmesh-mirror/gateway/mirror-refresh.json \
  check
```

A successful invocation emits only a small object such as:

```json
{"aliasCount":2,"event":"refresh_complete","ok":true}
```

It intentionally does not print paths, node names, repository names, HTTP
response bodies, Git output, or helper output. Subprocess stderr is discarded.
Detailed repository identities remain in the owner-only configuration, while
normal gateway request logs retain their existing generalized format.

Configure the gateway service to read the generated
`gatewayConfigPath`. A typical start boundary is:

```ini
ExecStartPre=/usr/bin/python3 -I /opt/forkmesh-mirror/mirror_gateway.py --config /var/lib/forkmesh-mirror/gateway/mirror-gateway.json --check
ExecStart=/usr/bin/python3 /opt/forkmesh-mirror/mirror_gateway.py --config /var/lib/forkmesh-mirror/gateway/mirror-gateway.json
```

Keep the gateway listener on loopback. An operator-managed HTTPS reverse proxy
is the public TLS boundary; Cloudflare Tunnel remains the default fallback.

The gateway materializes one authenticated working copy of the encrypted bare
repository for its process lifetime, and `refresh` materializes a second copy
before the atomic cutover. On hosts where `/tmp` is a small tmpfs, create an
owner-only directory on a disk-backed filesystem and set `TMPDIR` for both the
gateway service and manual refresh process:

```ini
[Service]
Environment=TMPDIR=/var/lib/forkmesh-mirror/runtime-tmp
ReadWritePaths=/var/lib/forkmesh-mirror/identity /var/lib/forkmesh-mirror/encrypted /var/lib/forkmesh-mirror/gateway /var/lib/forkmesh-mirror/runtime-tmp -/var/lib/forkmesh-mirror/source -/srv/forkmesh-git
```

The directory must already exist, be owned by the mirror service account, and
have mode `0700`. The refresh subprocess forwards `TMPDIR` only when it is an
absolute, normalized, real owner-only directory; unsafe values are ignored.
Size it for at least two expanded copies during validation. This storage is
ephemeral materialized public-repository data, not a replacement for the
encrypted archive, and should remain inside the service's protected local
storage boundary.

The packaged gateway keeps `ProtectSystem=strict` and makes only its identity,
encrypted-generation, gateway-state, runtime-temporary, and bare-source
subtrees writable. Release CAS data remains read-only. The leading `-` on each
alternative bare-source layout tells systemd that an absent alternative is
acceptable: packaged nodes use `/var/lib/forkmesh-mirror/source`, while some
existing operators use `/srv/forkmesh-git`. This write boundary is required
when `merge-pull` is enabled: the fixed executor records a terminal idempotency
result, updates exact checked refs, reseals the new generation, and renews the
signed catalog. A deployment using any other source or state location must
replace these entries with its exact dedicated paths in a systemd drop-in; do
not grant the gateway a broad filesystem path such as `/`.

## Register after restart

After the gateway and Tunnel are running with the new generation:

```bash
/usr/bin/python3 /opt/forkmesh/tools/headless_mirror_refresh.py \
  --config /var/lib/forkmesh-mirror/gateway/mirror-refresh.json \
  register
```

Registration performs these operations in order:

1. Reload and validate the owner-local public identity.
2. Run strict Git fsck and require the active refs pin to equal the exact bare
   source.
3. Validate the active encrypted archive and run the gateway's own `--check`.
4. Ask the identity helper to sign the bounded endpoint-registration request
   and POST it to `/api/mirrors/https`.
5. Ask the helper to sign the normalized public catalog-v2/state record and
   POST it to `/api/repositories`.

No Cloudflare API token is needed for these signed Worker endpoints. Redirects
and ambient HTTP proxies are disabled, TLS certificate verification uses the
system trust store, response sizes are bounded, and remote response bodies are
never included in an error.

The endpoint must already be publicly reachable, its signed manifest must match
the selected Tunnel or direct-origin mode, and its node key must already be
bound to the registered node account. A configured `mirrorUrl` must also pass
public DNS and signed health checks. The Worker rejects registration otherwise.

## Renew the signed health lease

Healthy endpoints are deliberately short-lived so a dead or disconnected
mirror falls out of routing. Each mirror therefore renews its own signed lease
every four minutes:

```bash
/usr/bin/python3 /opt/forkmesh-mirror/headless_mirror_refresh.py \
  --config /var/lib/forkmesh-mirror/gateway/mirror-refresh.json \
  renew
```

`renew` verifies the owner-local identity, active configuration, complete
encrypted-archive digest, exact source refs, and signatures before publishing.
It omits only the expensive full Git fsck and second gateway materialization;
the Worker still performs a fresh signed repository challenge and leaves the
endpoint fail-closed if the gateway or Tunnel is unavailable.

Install and enable `forkmesh-mirror-renew.service` and
`forkmesh-mirror-renew.timer` from `server/packaging/systemd/`. The timer runs after
boot and every four minutes. Its cadence, scheduling jitter, and bounded
five-minute service runtime remain below the ten-minute endpoint-registration
lease, so independently healthy mirrors stay eligible for round-robin
selection. A registration lease alone never authorizes repository bytes: the
Worker still requires a node-signed repository proof matching the canonical
refs, and caches a successful proof for no more than one minute before
revalidating it on later use.
Renewal and repository refresh share the same exclusive owner-only lock, so a
push refresh completes before a queued renewal can publish.
The root post-receive orchestrator pauses the renewal timer before changing a
gateway generation and resumes it after the new generation is published. This
prevents a restart-triggered renewal from racing the post-push publication.
Transient standalone renewal failures retry after twenty seconds.

## Configure the public Actions capability

The desktop controller normally invokes the installed helper through its fixed
SSH stdin mode after it has prepared an isolated Actions clone and committed
the device-local configuration. For direct operator use, pass only this
document:

```bash
printf '%s\n' \
  '{"schemaVersion":1,"type":"forkmesh.mirror-actions-catalog-configuration","actionsEnabled":true}' \
  | /usr/bin/python3 -I \
      /opt/forkmesh-mirror/headless_mirror_refresh.py \
      --config /var/lib/forkmesh-mirror/gateway/mirror-refresh.json \
      configure-actions
```

The request contract is
`www/docs/mirror-actions-catalog-configuration.schema.json`. The mode reads at most
1 KiB from stdin, rejects duplicate/unknown fields and non-boolean values, and
never accepts deployment variables. Its output contains only `ok`, the generic
event name, and the resulting boolean. The refresh configuration remains
owner-only and the signed publication still contains no workflow or secret
data.

## SSH post-receive integration

The SSH forced-command gateway may write the bare source, but it must not
receive access to the node or age private files. Run Git and mirror publication
under separate accounts. After a successful receive, the Git gateway invokes a
fixed root-owned notifier with no arguments or push-derived environment. A
systemd path unit consumes its coalesced marker and runs the refresh tool as the
mirror account. The complete account, socket-broker, notification, signed
health, and service boundary is documented in
[`git-ssh-gateway.md`](git-ssh-gateway.md).

The post-receive flow is:

```text
successful receive-pack
  -> start serialized refresh service
  -> headless_mirror_refresh.py refresh
  -> restart mirror gateway
  -> within one bounded deadline, verify identity-bound signed health
     over both loopback and the configured public HTTPS origin
  -> headless_mirror_refresh.py register
```

The health clients ignore ambient proxy settings and reject redirects, so a
different endpoint cannot satisfy either proof. The tracked example allows 180
seconds for the gateway and its configured TLS proxy to become ready. Registration
fails closed if either signed proof is unavailable or invalid; the independent
four-minute renewal timer remains the fallback for a later transient outage.

Do not place this sequence behind `sh -c` with user-controlled repository
arguments. Use fixed command arrays and a fixed config path. A service manager
should serialize the sequence; the tool also takes an owner-only advisory lock
so overlapping refresh/check/register/renew invocations cannot interleave.

Concurrent Git ref movement is detected by comparing the refs before sealing,
the helper's sealed-snapshot digest, and the refs after a second strict fsck.
If they differ, refresh fails and the next post-receive notification can retry.

## Last-good and recovery behavior

Encrypted archives are installed under a SHA-256 content-addressed filename.
The generated gateway JSON is the only active pointer and is replaced
atomically only after:

- strict fsck succeeds;
- the helper has atomically produced and authenticated an age file;
- the source refs remain stable;
- every configured owner alias references identical returned metadata; and
- `mirror_gateway.py --check` materializes and validates the staged generation.

The previous ciphertext is not overwritten or deleted. A failure before the
config rename leaves the old config/ciphertext pair active. A newly created but
unreferenced content-addressed archive may remain after a failed staged
validation; retain it until the incident is understood, then remove only files
that are not referenced by the active gateway configuration or an operator
backup. The tool deliberately performs no automatic destructive garbage
collection.

If `register` fails, do not reseal merely to retry publication. Confirm that
the gateway and Tunnel are live, then rerun `check` and `register`. If `check`
reports source drift, run the full refresh/restart/register sequence.

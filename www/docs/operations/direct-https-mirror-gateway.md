# Direct-HTTPS independent mirror gateway

`tools/mirror_gateway.py` is the repository-byte origin for an independently
operated ForkMesh mirror. It serves loopback HTTP only. An operator-controlled
HTTPS reverse proxy creates the public TLS connection. Git clients may use the
mirror's standard `/<owner>/<repository>.git` Smart-HTTP URL directly; the main
ForkMesh Worker retains its stable clone URL and redirects there only after a
fresh signed mirror proof. Cloudflare Tunnel remains the fallback for nodes
that do not advertise an independently reachable URL.

This is separate from the multiplayer socket:

- Git clone/fetch, trees, blobs, raw files, history, commits, branches, search,
  statistics, size maps, and release downloads use ordinary HTTPS.
- Chat, avatar movement, emotes, and transient world presence may use the
  multiplayer socket.
- Repository and replica changes are announced over the node event WebSocket;
  the small signed endpoint-lease renewal shares that connection. The daemon
  coalesces events and fetches changed Git/replica bytes over direct HTTPS; a
  bounded reconcile interval remains only as recovery for a missed or
  disconnected event stream. Large Git payloads do not travel inside the
  WebSocket. Bootstrap marks exact managed peer URLs as event-driven, which
  prevents direct mirrors from being mistaken for third-party sources and
  polled every 15 seconds; their dropped-event recovery fetch runs once every
  30 minutes.

The gateway's Git data plane is read-only. It implements Git `upload-pack`; it
does not implement `receive-pack`, general pushes, issue writes, administration,
or arbitrary Git service selection. An operator may separately opt one
repository into the typed exact-OID `merge-pull` control operation described
below, opt individual repositories into `git-receive-pack`, or configure a
`writableRepositoryRoot` plus `writableOwners` allowlist for first pushes.
The main Worker answers receive-pack advertisement and POST requests that
lack valid signed device headers with `401 push_unauthorized` before reading
a body (www/docs/mobile/mobile-node-contract.md §7); there is no
repository-byte socket fallback.
Owner-authorized stock Git writes use the separately published SSH gateway.
The generated [stock Git compatibility matrix](../git-compatibility.md) keeps
that transport split explicit.

## Security boundary

The local process enforces all of these properties:

- The listener must be `127.0.0.1`, `::1`, or `localhost`.
- Standard public Git upload-pack reads need no routing capability. Every
  control, mutation, private, and non-Git repository request keeps the fresh,
  single-use Ed25519 capability from the configured routing Worker.
- The capability binds the exact method, path plus query, body digest, node,
  request ID, and epoch-millisecond issue time.
- Unknown, private, disabled, and integrity-quarantined repositories all return
  the same `404 {"ok":false,"error":"not_found"}` response.
- Health and manifest responses never enumerate repository names.
- Only explicitly configured public repositories can enter the serving index.
- Enabled public repositories need a SHA-256 pin over their sorted heads and
  tags. The pin is checked at startup, during signed repository health checks,
  and before serving.
- Git is invoked directly without a shell. System/global Git configuration,
  prompts, optional locks, external protocol helpers, credential helpers,
  fsmonitor commands, alternate-ref commands, and upload-pack pack hooks are
  disabled at the serving boundary.
- `refs/forkmesh/` is internal node state. Upload-pack hides the entire
  namespace and rejects arbitrary tip/reachable-object wants, so merge recovery
  refs and their otherwise-unreachable objects cannot be advertised or fetched.
  The SSH gateway applies the same upload rule and also uses
  `receive.hideRefs` so a push cannot inspect, create, update, or delete those
  refs.
- Release files are content addressed and re-hashed before any bytes are sent.
- Default HTTP logging is disabled because it would expose client addresses and
  raw query paths. Operational logs contain only time, a bounded request ID,
  generalized operation, status, duration, and byte count.
- Git stderr, repository paths, source contents, request queries, user agents,
  and client addresses are not logged.
- The gateway accepts no private-key, token, password, seed, mnemonic, keypair,
  or credential field in its JSON configuration.

The strict configuration contract is
`www/docs/mirror-gateway-config.schema.json`. A direct endpoint manifest uses
`www/docs/mirror-gateway-manifest.schema.json`; it records a non-proxied public
A/AAAA origin, `mainProxyMode: direct`, and the standard
`/{owner}/{repository}.git` template. Tunnel-shaped manifests remain readable
only for rolling upgrades from older nodes. The managed fleet does not run
`cloudflared`, and a new node must prove its direct HTTPS endpoint before it is
published.

## Identity helpers

Python's standard library does not provide Ed25519. ForkMesh does not substitute
an invented cipher or a shared HMAC. Two direct, shell-free local commands
provide the cryptographic boundary:

For a non-GUI VPS, the production implementation and command-array examples
are in [Headless mirror identity](headless-mirror-identity.md).

1. `healthSignerCommand` is supplied by `forkmesh-mirror-node` and accesses the
   managed host's owner-only node identity (or another owner-controlled signer).
2. `requestVerifierCommand` verifies signatures made by the routing Worker's
   configured public Ed25519 key.

Both commands receive one bounded JSON object on standard input and return one
bounded JSON object on standard output. The gateway removes ambient environment
variables whose names look like credentials. Helpers should access private
identity material from the owner-controlled keychain or hardware device, not
from their command arguments or response.

Health signer input:

```json
{
  "schemaVersion": 1,
  "type": "forkmesh.health-challenge-signing",
  "algorithm": "Ed25519",
  "encoding": "base64url-no-padding",
  "publicKey": "<32-byte public key>",
  "messageBase64": "<canonical message>",
  "messageSha256": "<sha256>"
}
```

Health signer output:

```json
{
  "publicKey": "<same public key>",
  "signature": "<64-byte Ed25519 signature>"
}
```

Capability verifier input:

```json
{
  "schemaVersion": 1,
  "type": "forkmesh.request-capability-verification",
  "algorithm": "Ed25519",
  "encoding": "base64url-no-padding",
  "publicKey": "<routing Worker public key>",
  "messageBase64": "<canonical message>",
  "messageSha256": "<sha256>",
  "signature": "<64-byte Ed25519 signature>"
}
```

Capability verifier output:

```json
{
  "valid": true,
  "publicKey": "<same routing Worker public key>"
}
```

No helper response may contain a private-key-shaped field.

## Configuration

First compute the mirror's refs pin:

```bash
python3 tools/mirror_gateway.py \
  --refs-sha256 /srv/forkmesh/mirrors/alice-project.git
```

Create an operator-owned JSON file outside the checkout:

```json
{
  "schemaVersion": 1,
  "node": {
    "name": "alice-mirror",
    "publicKey": "<base64url Ed25519 public key>"
  },
  "routerPublicKey": "<base64url routing Worker Ed25519 public key>",
  "publicOrigin": "https://mirror1.example.com",
  "listen": {
    "host": "127.0.0.1",
    "port": 8790
  },
  "manifestPath": "/etc/forkmesh/forkmesh-mirror.json",
  "requestVerifierCommand": [
    "/usr/local/bin/forkmesh-verify-router-capability"
  ],
  "healthSignerCommand": [
    "/usr/local/bin/forkmesh-sign-health"
  ],
  "limits": {
    "maxReleaseBytes": 2147483648
  },
  "repositories": [
    {
      "owner": "alice",
      "name": "project",
      "visibility": "public",
      "enabled": true,
      "gitDir": "/srv/forkmesh/mirrors/alice-project.git",
      "releaseStore": "/srv/forkmesh/releases/alice-project",
      "integrity": {
        "expectedRefsSha256": "<64 lowercase hex characters>"
      }
    },
    {
      "owner": "alice",
      "name": "private-project",
      "visibility": "private",
      "enabled": false
    }
  ]
}
```

An omitted `visibility` fails closed to private. A private entry cannot be
enabled and cannot include a storage path. Its name exists only in the local
operator file; the process does not inspect, materialize, index, or announce it.

An omitted `releaseStore` falls back to `<gitDir>/forkmesh-releases` — where
`tools/forkmesh-release-publish.sh` stages artifacts and where the desktop host
mirrors them — when that path is a real directory rather than a symlink. Point
`releaseStore` elsewhere only if this node keeps its release CAS outside the
served mirror; a node serving `release-blob` with no store answers 404 for every
artifact it holds.

Validate without opening a listener:

```bash
python3 tools/mirror_gateway.py \
  --config /etc/forkmesh/mirror-gateway.json \
  --check
```

Run locally:

```bash
python3 tools/mirror_gateway.py \
  --config /etc/forkmesh/mirror-gateway.json
```

## Independent mirror operation

A mirror published on its own subdomain — for example `mirror33.forkmesh.com`
reachable directly over HTTP/HTTPS — can offer the full ForkMesh experience
without routing anything through the control-plane Worker: browse repositories
in the same web dashboard, and `git clone`, `git push`, fork, and submit pull
requests over SSH. Every capability below is opt-in and additive; a mirror that
enables none of them behaves exactly as before. None of them change repository
identity — the dashboard origin, SSH origin, and mirror URLs are all
replaceable, discoverable metadata over the same canonical repositories.

### Same-codebase dashboard

The gateway can serve the identical website the Worker serves, sourced from the
build outputs (`app/public/**`, `www/public/**`) already present as Git blobs
inside a mirrored public repository. Because those assets arrive through
ordinary repository sync and stay covered by the integrity pin, the mirror
tracks dashboard updates with no separate deployment artifact. The shipped
`api-client.js` resolves its API origin to `location.origin` on any
non-canonical host, so the unmodified bundle talks to this gateway's read-only
`/api` subset with no injected configuration.

```json
"dashboard": {
  "enabled": true,
  "assetsRepository": "forkmesh/forkmesh",
  "canonicalOrigin": "https://app.forkmesh.com",
  "sshOrigin": "ssh://git@ssh.forkmesh.com",
  "replicaPath": "/var/lib/forkmesh-mirror/gateway/dashboard-replica.json"
}
```

- `assetsRepository` must name an enabled public repository in this same file.
- `canonicalOrigin` (optional unless `replicaPath` is set) is where `/login`
  and `/signup` redirect when the browser does not already hold a valid
  portable mirror-session grant. A mirror never receives a password,
  controller bearer token, or D1 session row.
- `sshOrigin` (optional) is advertised as each repository's `sshUrl` clone/push
  command in the UI.
- `replicaPath` (optional) is a service-owned `0600` last-good bundle of the
  controller's allowlisted public projections. The native daemon refreshes it
  during repository synchronization; failed refreshes retain the prior valid
  documents, so `/api/network/overview`, `/api/status`, and `/api/status/world`
  remain available during a controller outage.

The replica is intentionally **not a D1 copy**. `app/src/mirror_replica.py` and
`app/src/public_snapshots.py` are the shared Worker/mirror contract and admit
only the same exact public response schemas already used by App snapshots.
Passwords, controller sessions, encrypted account rows, notifications, private
inboxes, DMs, notes, payments, and administration state cannot enter the file.
Login continuity is separate: the controller may issue an expiry-bounded,
Ed25519-signed portable grant containing only opaque account/session ids and a
public username. The mirror verifies that grant with the published router key;
it does not replicate the controller session. The installer stages the shared
replica and portable-session modules beside `mirror_gateway.py`; the Go daemon
invokes the gateway's bounded `--sync-dashboard-replica` helper rather than
maintaining another schema implementation.

Repository code and data follow a separate authority rule: dashboard assets,
files, branches, releases, issues, pulls, and discussions come from the local
Git mirror. Existing signed collaboration intake and SSH submission spools
converge those repository-native records when the controller returns. Thus a
node stays useful from its last validated state without becoming a second
credential or payment authority.

The long-lived gateway uses bounded memory as its primary read tier. Rendered
repository JSON (including Git-backed issue/pull/discussion metadata), Smart
HTTP advertisements, dashboard assets, the repository catalog, portable
session verification, and every dashboard-replica document stay resident until
their authoritative generation changes. Repository and configuration changes
arrive through the daemon's existing SIGHUP generation swap; an accepted local
receive-pack invalidates its generation synchronously; a successful atomic
dashboard-replica replacement sends SIGUSR1 and preloads the new file exactly
once. There is no time-based cache expiry that would turn an unchanged hot
mirror back into a disk workload. Entry and byte ceilings apply independently
to each cache, with LRU eviction when either ceiling is reached.

Plain `/health` includes cache entries, bytes, hit/miss, age, eviction,
invalidation, warmup, backend-read, and avoided-read counters. The unsigned
plain response is public, credential-free JSON and returns wildcard CORS when
an `Origin` is present so independent mirrors can diagnose peers directly;
signed challenge responses and all repository/control routes expose no CORS
access. Cache loss never loses state: process restart validates and warms again
from the atomic replica file and integrity-pinned bare Git repositories. Large
raw blobs, release downloads, private ciphertext, and upload-pack streams
deliberately remain streaming paths rather than placing unbounded or sensitive
bodies in process memory.

The canonical App uses invisible reverse proxying for browser/API reads, so a
visitor can remain at `app.forkmesh.com/<owner>/<repo>` while a healthy direct
mirror serves the bytes. Smart HTTP clone/fetch may instead receive a temporary
redirect that Git follows. Every repository page also lists the independently
reachable `mirrorN.forkmesh.com/<owner>/<repo>` dashboards for the case where
the canonical App origin itself is unavailable. Follow the root
`deploy-checklist.md` before retiring a node's Cloudflare Tunnel.

The dashboard adds these routes to the loopback gateway:

- static assets and pages: `/`, `/dashboard`, `/dashboard/<page>`,
  `/dashboard.js`, `/membership-prompt.js`, `/api-client.js`,
  `/dashboard/tailwind.css`, `/favicon/*`, `/assets/*`, and the repository page
  shell for every `/<owner>/<repo>[/…]`;
- read-only API: `GET /api/repositories`, `GET /api/version`,
  `GET /api/repo/<owner>/<repo>/<read-op>` (an alias for the existing
  `/v1/public/repositories/...` reads — Code, README, branches, commits, diffs,
  and the issue/pull/discussion lists that live as `.forkmesh/*` blobs),
  `GET /api/repo/<owner>/<repo>/mirrors`, and its `.../mirrors/<node>/reachability`
  check;
- `GET /api/accounts/sessions` returns the single bounded portable mirror
  session when the separate `forkmesh_portable` grant verifies, and `401` when
  it is absent, expired, malformed, or signed by another controller. The
  response never exposes the portable token or a controller credential.
- A logged-in App mirror link opens a no-store controller handoff page that
  submits the signed grant in a form body to
  `POST /api/accounts/session-handoff`. The mirror accepts only the configured
  canonical App origin, writes the grant to a shared-domain HttpOnly cookie,
  and redirects to the clean repository URL. The grant never appears in the
  URL, referrer, or local storage.
- When `replicaPath` is configured, the other allowlisted public snapshot
  routes use the validated local last-good projection and disclose that source
  through `X-ForkMesh-Replica: public-projection` plus fetch/generation times.

Dashboard responses carry a real Content-Security-Policy (the same allowlist as
the site's `_headers`) instead of the control routes' `default-src 'none'`;
`/health`, `/forkmesh-mirror.json`, `/v1/*`, and the Smart HTTP clone routes are
untouched. Put the gateway behind your TLS reverse proxy exactly as for the Git
routes.

### SSH writes without the control plane

`tools/ssh_gateway.py` and `tools/ssh_authorization_broker.py`
([git-ssh-gateway.md](git-ssh-gateway.md)) normally authorize every key against
the Worker's `POST /api/ssh/authorize`. Two additive broker modes let a mirror
authorize locally:

- `"localAuthorization": {"mode": "fallback", "grantsFile": "…"}` keeps the
  Worker as the source of truth but consults a signed local grants file **only
  when the Worker is unreachable** — never after an authoritative denial, so
  revocation is never bypassed by cutting the uplink.
- `"localAuthorization": {"mode": "local", "grantsFile": "…"}` never contacts a
  Worker; the grants file is the whole policy. Use this for a fully autonomous
  mirror.

The grants file (`type: forkmesh.ssh-local-grants`) maps each authorized SSH key
to an account and its per-repository access (`read-only`, `read-write`, or
`pull-request`). A `pull-request` grant may push only to
`refs/heads/pr/<account>/*` — enforced with `receive.hideRefs` so contributors
open PR branches without write access to `main`.

### Contributor pull requests over SSH

With a `pullSubmissionSpool` configured, the gateway accepts a signed pull
request or review comment through the forced command
`forkmesh-pull-submit <owner>/<repo>`, reading the JSON submission on stdin. The
gateway verifies the submission's own Ed25519 signature (the same
`forkmesh-pull-event-v1` / `forkmesh-pull-comment-v1` canonical form the Worker
and desktop use — the author field *is* the raw public key, so no key registry
is needed) and spools it durably with per-key and global caps. A timer replays
the spool to the controller's public inbox once it is reachable:

```bash
python3 tools/ssh_gateway.py --config … spool-forward \
  --api-origin https://app.forkmesh.com
```

Delivered items move to `forwarded/`, permanently rejected ones to `rejected/`,
and transient failures stay for the next run. The controller's own pull inbox,
numbering, and merge flow are unchanged.

### Forking over SSH

With a `dynamicRepositoriesPath` registry configured, the forced command
`forkmesh-fork <owner>/<repo>` clones an existing served repository into a new
bare repository under the requesting account (heads and tags only — the source
`forkmesh/pulls` ledger and internal `refs/forkmesh/*` are not copied), appends
it to the registry, and signals a gateway refresh. The gateway loads registry
entries at startup as pin-less public repositories (they trust their startup ref
generation instead of an owner-signed pin), and the broker treats an account's
own registry forks as implicitly writable. Point both the gateway
`dynamicRepositoriesPath` and the broker `localAuthorization.dynamicRepositoriesFile`
at the same file. Fork publication to the global catalog still flows through the
owner's normal signed path; the registry only makes the fork locally serveable
and pushable.

Under the Go `forkmesh-mirror-node` daemon, add the `dashboard` block and
`dynamicRepositoriesPath` to the gateway config the daemon manages; it preserves
both across its restart-time rewrite. The SSH gateway, broker, and its systemd
units are provisioned separately from the daemon
([git-ssh-gateway.md](git-ssh-gateway.md)).
### Push-created repositories

A node may additionally host repositories created by a first authorized push
from a ForkMesh device (`www/docs/mobile/mobile-node-contract.md` section 7).
Push hosting is entirely opt-in: with these keys omitted the refresh cycle can
never render `git-receive-pack` for any repository, and no repository can be
created or served under a writable root. (The separate per-repository opt-in
described above - naming `git-receive-pack` in one repository's `operations` in
a hand-written gateway configuration - remains available for a repository that
already exists; it is not how push-created repositories work, and a
refresh-managed node has its gateway configuration rewritten on every cycle.)

Set these keys in the **refresh** configuration
(`www/docs/headless-mirror-refresh.schema.json`), not by hand in the generated
gateway file, because `headless_mirror_refresh.py refresh` rewrites that file
on every cycle:

```json
{
  "writableRepositoryRoot": "/srv/forkmesh-git/pushed",
  "writableOwners": ["mirror2"],
  "receiveMaxInputBytes": 268435456
}
```

- `writableRepositoryRoot` must already exist as a real, absolute, mode-0700
  directory owned by the mirror service account. Create it with
  `install -d -o forkmesh-mirror -g forkmesh-mirror -m 0700 <path>`.
- `writableOwners` is the allowlist of owner names whose repositories may be
  created **and served**. It requires `writableRepositoryRoot`, must be unique
  valid owner names, and is capped at 64 entries. The single entry `["*"]`
  accepts **every** owner the control plane authorized; it may not be mixed
  with named owners, so no configuration can read as narrower than it is. That
  wildcard is what a node hosting repositories created from the website needs
  (issue #676): the owner of a repository created in a browser has no node of
  their own, and a hosting node cannot enumerate the accounts it will hold.
  Authorization itself does not move — every request reaching a write already
  carried a verified router capability, and the Worker applies the ordinary
  push permission checks before signing one. Removing an owner from it is
  the revocation control: from the next refresh cycle the owner's push-created
  repositories are no longer rendered into the gateway configuration, and the
  restarted gateway refuses to open them from `hosted-pushes.json` at all, so
  both reads and further pushes stop. The bare repositories are left untouched
  on disk; delete them separately if the data must go too.
- `receiveMaxInputBytes` is optional; it is rendered as
  `limits.receiveMaxInputBytes` and becomes `receive.maxInputSize`. It also
  requires `writableRepositoryRoot`. The gateway defaults it to 256 MiB (the
  SSH gateway's figure) and it is the effective size limit of one HTTPS
  push: the Worker holds at most `HTTPS_MIRROR_BODY_MAX_BYTES` (8 MiB) of a
  push body and streams anything larger to this gateway, which reads a
  receive-pack POST up to this value once the request's router capability
  verifies (nothing unsigned makes it hold more than 8 MiB), spools the
  chunked body to disk past 8 MiB, keeps at most two such pushes in flight
  (others get `503 push_busy` with `Retry-After`), and scales the
  receive-pack deadline with the pack. Because the Worker cannot digest a
  pack it never holds, a streamed push's capability carries the fixed
  streamed-body digest (`sha256("forkmesh-streamed-request-body-v1")`) or,
  for a signing device, the digest the device stated and signed; the gateway
  honours the fixed digest only on a receive-pack POST. Size this to the
  node's memory: a push holds its whole pack in RAM while git indexes it.

The three keys are rendered verbatim into the generated gateway configuration.
`operations` in the refresh configuration is still restricted to the read-only
public routing contract; adding `git-receive-pack` there is rejected.

**What happens on a push.** The Worker forwards a capability-bound
`git-receive-pack` request. The gateway creates
`<writableRepositoryRoot>/<owner>/<repo>.git` only when the owner is accepted
by `writableOwners` (named, or under the `["*"]` wildcard), runs `git receive-pack --stateless-rpc` with deletes,
non-fast-forwards, and hooks refused and objects fsck-checked, then records the
repository in the atomic `hosted-pushes.json` sidecar next to the gateway
configuration and drops `/run/forkmesh-mirror-refresh/pending`. The packaged
`forkmesh-mirror-refresh.path` unit consumes that marker and runs the ordinary
refresh, gateway restart, signed-health, and registration sequence. Between the
push and that cycle the gateway already answers reads for the sidecar
repository, so a clone never has to wait for the refresh.

**What appears where.**

| Artifact | Written by | Contains |
| --- | --- | --- |
| `<writableRepositoryRoot>/<owner>/<repo>.git` | gateway, on first push | the bare repository |
| `<gateway config dir>/hosted-pushes.json` | gateway, after each accepted push | owner, name, `gitDir`, first/last push timestamps |
| `/run/forkmesh-mirror-refresh/pending` | gateway, after each accepted push | the fixed coalescing marker |
| generated `mirror-gateway.json` | refresh cycle | the repository entry carrying `git-receive-pack` |
| `/api/mirrors/https` endpoint record | refresh `register` | this node's signed health, which now advertises the repository |
| `/api/repositories` catalog record | **the pushing device**, not this node | the public repository listing |

**Catalog ownership.** The refresh cycle publishes exactly one catalog record,
for the node's own mirror. It never publishes a catalog record for a
push-created repository: that record is published by the device that pushed,
signed with its own key (`catalogSigVersion: 2`, `source: "local-node"`). The
two therefore cannot fight. Re-running `refresh` and `register` after a push
that changed nothing is a byte-identical no-op, and a device re-publishing the
same record is resolved by the Worker's existing device-scoped
`repo_source_authorities` rules: a non-authoritative device is answered `202`
with `mirror: true` rather than replacing the canonical record, and an older
record is refused with `409 stale_update`. Operators must not hand-publish a
catalog record for a push-created repository; doing so would take the source
authority away from the device that owns it.

**Integrity pinning.** Every ordinary mirror keeps the strict immutable pin:
its refs are authenticated once against `integrity.expectedRefsSha256` from the
signed configuration, and any later drift of its serving generation quarantines
it. A repository whose rendered `operations` contain `git-receive-pack` is the
single exception, because an authorized push is supposed to move its refs. For
those repositories integrity means the bare repository is still readable and
its refs still enumerate; the exact served generation is re-advertised on every
signed repository proof and re-verified by the routing Worker. The rendered
`expectedRefsSha256` for such an entry is therefore a record of the generation
at render time, not a freeze.

**Fail-closed behaviour.** `hosted-pushes.json` is machine state, so both the
gateway and the refresh tool treat it as hostile. A missing, symlinked,
oversized, non-owner-owned, unparseable, wrongly typed, or over-long sidecar is
ignored in full, and one unusable entry is skipped without aborting the rest.
An entry is rendered only when its owner is accepted by `writableOwners`, its name
passes the shared repository regex, and its `gitDir` is exactly
`<writableRepositoryRoot>/<owner>/<name>.git` as a real, symlink-free,
service-account-owned bare repository. The gateway applies the same owner
allowlist when it opens a sidecar repository at startup or resolves one for a
request, so the two sides can never disagree about which owners are writable.
Push-created entries render last and can never displace a configured
repository or an import. None of this can fail the
node's own refresh.

**Verify it.**

```bash
# 1. The sidecar names the repository the push created.
sudo -u forkmesh-mirror cat \
  /var/lib/forkmesh-mirror/gateway/hosted-pushes.json

# 2. The rendered configuration carries git-receive-pack for it, and for
#    nothing else.
sudo -u forkmesh-mirror python3 -c 'import json,sys
value = json.load(open(sys.argv[1]))
for item in value["repositories"]:
    if "git-receive-pack" in item["operations"]:
        print(item["owner"], item["name"])' \
  /var/lib/forkmesh-mirror/gateway/mirror-gateway.json

# 3. The signed repository proof advertises it.
curl -sS "https://<public-origin>/health?nonce=$(head -c18 /dev/urandom \
  | base64 | tr '+/' '-_' | tr -d '=')&issuedAt=$(( $(date +%s) * 1000 ))\
&owner=<owner>&repo=<repo>" | python3 -m json.tool

# 4. The refresh cycle is idempotent: this must report no change.
sudo -u forkmesh-mirror python3 -I /opt/forkmesh-mirror/headless_mirror_refresh.py \
  --config /var/lib/forkmesh-mirror/gateway/mirror-refresh.json check
```

If step 2 is empty after a successful push, the marker never reached the
refresh cycle. Confirm `/run/forkmesh-mirror-refresh` exists with the packaged
`server/packaging/tmpfiles.d/forkmesh-mirror-refresh.conf` (which grants the
mirror account write+traverse through an ACL) and that
`forkmesh-mirror-refresh.path` is enabled.

## Public and private storage

Every enabled public mirror uses an absolute `gitDir` pointing to an
owner-controlled bare repository. Public repository data is intentionally not
encrypted: avoiding seal, age, tar, and materialization work keeps push-driven
updates in the seconds range. The gateway requires the directory to be real,
owned by its service account, not world-writable, structurally bare, and pinned
by `integrity.expectedRefsSha256`. Public `encryptedArchive` entries are
rejected.

Private repositories retain the encryption boundary. They are never
materialized by the public gateway. The Qt control
  node persists them with `PrivateMirrorStore`; the gateway's separate
  `/v1/private-replicas/<opaque-id>` route streams that validated ciphertext
  only after a short-lived routing-Worker capability. It has no route from a
  private repository name to an opaque id and cannot decrypt the archive.

### Private replica boundary

`desktop/src/PrivateMirrorStore.cpp` is the private-repository storage
boundary. It stores no owner account name, organization, repository name,
source path, or private identity material. A replica filename is a random
256-bit opaque id and the file contains only:

- an AES-256-GCM encrypted whole-mirror archive;
- a hybrid X25519 + ML-KEM-768 content-key wrap for each authorized recipient;
- the owner's public-key identifier, ciphertext and exact-file digests, key
  epoch, and non-identifying timestamps; and
- recipient public-key identifiers needed to select a key wrap.

The replica directory is owner-only and each file is mode `0600`. Writes and
recipient rotations are atomic. Rotation decrypts locally with the current
owner identity, creates a fresh content key and wraps it to the exact new
recipient set, increments the authenticated key epoch, and replaces the stored
ciphertext. Removing a recipient therefore revokes access to new epochs.
Private identity halves stay in the owner or collaborator's separate local
identity vault; they are never stored beside replica ciphertext or sent to the
Worker.

A mirror operator can observe that opaque ciphertext exists, its approximate
size, timestamps, and recipient key identifiers. The operator cannot derive the
repository name or plaintext without being an authorized recipient. An
authorized recipient—and anyone who controls that recipient's unlocked
device—can read the decrypted repository. Revocation cannot erase an older
ciphertext or plaintext copy that a recipient already retained, so owners
should rotate immediately and apply an appropriate backup/retention policy.
Disk encryption is recommended to reduce filesystem-journal and swap leakage
during authorized local decryption.

The Qt control-node workflow is:

1. The device derives a vault-unlock secret from its local Ed25519 identity and
   loads or creates a hybrid X25519 + ML-KEM-768 identity. Its private halves
   are AES-256-GCM encrypted in a separate mode-`0600` vault. A signed-in,
   desktop-capable account registers only the public bundle through
   `POST /api/security/owner-keys`.
2. An owner sync bundles a local Git repository in memory. A remote source is
   accepted only as a canonical HTTPS URL without user info, query, fragment,
   or control characters. Git runs with system/global configuration,
   credential helpers, URL rewrites, hooks, redirects, and non-HTTPS protocols
   disabled. The compact authorization header is supplied only as an explicit
   process argument and is never logged.
3. The first seal creates an opaque replica; every later sync uses a fresh
   content key and increments the epoch. A migrated managed plaintext bare
   mirror is deleted only after the encrypted replica authenticates and a new
   temporary materialization succeeds.
4. Before refreshing a published private repository, and after every share
   add/remove, the owner fetches a fresh owner-signed `/shares` response. Qt
   requires a one-to-one match between grantees and public recipient bundles,
   verifies every key identifier, and requires `encryptionReady: true`,
   `missingEncryptionKeys: []`, and `privateKeysStored: false`. Any mismatch
   blocks rotation and publication. Successful rotation seals to the exact
   owner-plus-collaborator set before catalog-v2 and private-route publication.
5. An authorized collaborator registers their public bundle. Their signed,
   ACL-gated, no-store catalog response then includes a random 64-hex
   `privateAccessId`, and Qt downloads
   `GET /api/private-replicas/<privateAccessId>` with a short-lived
   grantee-signed Basic authorization header. Owner and repository names never
   appear in this request URL or query string. Qt follows no redirect, accepts
   only `application/vnd.forkmesh.private-replica+json`, requires the exact
   quoted SHA-256 ETag and content length, and buffers at most 256 MiB. Import
   rejects file-digest mismatches, conflicting same-epoch bytes, owner changes,
   and epoch rollback.
6. Owners and collaborators decrypt only with their local vault identity. The
   bundle is staged mode `0600`, imported into a mode-`0700` temporary bare
   repository, and deleted immediately; the temporary repository is recursively
   removed when its authorization scope ends. Private repositories do not open
   a named repository control socket, so their identity cannot leak through
   presence or probing.

Private-replica code must not log plaintext, repository identities, source
paths, opaque ids, envelope bodies, or decryption errors containing user data.
Gateway logs continue to use only the generalized `private-replica` operation
and a bounded opaque request id.

Abrupt termination, swap, crash dumps, filesystem journals, or an administrator
of an unlocked recipient device can expose authorized runtime plaintext. The
application's cleanup narrows that window but cannot eliminate operating-system
artifacts. Revocation protects new epochs and prevents future authorized
downloads; it cannot erase ciphertext or plaintext a former recipient already
retained.

For public repositories, confidentiality is optional but authenticity is not.
The main router must compare the signed observed refs digest with the repository
owner's signed canonical state, not blindly trust a mirror operator's local pin.

## One-click Cloudflare Tunnel

`tools/cloudflare_tunnel_bootstrap.py` creates or reuses:

- A remotely managed Cloudflare Tunnel.
- A remote ingress from the requested hostname to
  `http://127.0.0.1:<gateway-port>`.
- A catch-all `http_status:404` ingress rule.
- A proxied CNAME from the hostname to
  `<tunnel-id>.cfargotunnel.com`.
- An optional public signed `forkmesh-mirror.json`.

This tool does not replace or change `tools/cloudflare_bootstrap.py`. The
existing bootstrap remains responsible for the Worker, D1, migrations, static
assets, and Worker route. A deployment can run both, or an existing ForkMesh
instance can add the independent gateway later.

Use a scoped Cloudflare API token with Account read, Zone read, DNS edit for the
selected zone, and Cloudflare Tunnel/Connector write. Supply it through
`CLOUDFLARE_API_TOKEN`, a hidden terminal prompt, or one line on stdin:

```bash
export CLOUDFLARE_API_TOKEN="locally-supplied-token"  # forkmesh-secret-scan:ignore-line
export CLOUDFLARE_ACCOUNT_ID="account-id"
export FORKMESH_NODE_PUBLIC_KEY="<public Ed25519 key>"
export FORKMESH_MIRROR_MANIFEST_SIGNER="/path/to/local-manifest-signer"

python3 tools/cloudflare_tunnel_bootstrap.py \
  --hostname mirror1.example.com \
  --zone example.com \
  --tunnel-name alice-mirror \
  --node-name alice-mirror \
  --origin-port 8790 \
  --manifest-output /etc/forkmesh/forkmesh-mirror.json \
  --gateway-config /etc/forkmesh/mirror-gateway.json \
  --launch
```

`--launch` passes the connector credential to `cloudflared` only in its process
environment. It never passes the Cloudflare API token to either child.

For a system service, explicitly choose a connector token file:

```bash
python3 tools/cloudflare_tunnel_bootstrap.py \
  --hostname mirror1.example.com \
  --zone example.com \
  --tunnel-name alice-mirror \
  --node-name alice-mirror \
  --origin-port 8790 \
  --tunnel-token-file /etc/forkmesh/cloudflared-token

cloudflared tunnel run --token-file /etc/forkmesh/cloudflared-token
```

The connector file is created with mode `0600`. It is a sensitive Tunnel
connector credential and should be placed in the operator's secret store and
rotated through Cloudflare. It is not the Cloudflare API token, a ForkMesh
identity key, a repository decryption key, or a wallet key.

With neither `--launch` nor `--tunnel-token-file`, the tool provisions the
Cloudflare resources and deliberately does not retrieve or print the connector
credential. Use `--dry-run` for read-only account/zone/existing-tunnel checks.
Conflicting DNS fails closed unless `--replace-dns` is explicit.

A new headless node can provision all of this at install time. Running
`install.sh` with `FORKMESH_TUNNEL_HOSTNAME` and `CLOUDFLARE_API_TOKEN` set
(optionally `FORKMESH_TUNNEL_ZONE`, `FORKMESH_CLOUDFLARE_ACCOUNT_ID`, and
`FORKMESH_RELAY_HOSTNAME`) stages the pinned tools, installs the SHA-256-pinned
`cloudflared`, discovers the relay's mirror-router public key, runs this
bootstrap as the service user with the API token only in the child environment,
and restarts the node. The managed service then supervises its gateway, Tunnel
connector, and signed endpoint registration whenever the connector token file
exists. A provisioning failure is soft: the managed install keeps running
without a tunnel.

### Desktop provisioning and managed-host lifecycle

The desktop Control page is a provisioning client; it never runs the repository
gateway or publishes the catalog:

1. **Deploy Worker + mirror** runs the pinned Worker bootstrap for the relay
   hostname, then the pinned direct-HTTPS bootstrap for a distinct mirror
   hostname. The DNS API token is read over the SSH process's standard input,
   cleared immediately, redacted from displayed output, and never written to
   settings or argv.
2. The direct bootstrap creates the DNS record, obtains/renews the host TLS
   certificate, writes the signed direct-origin manifest, and verifies the
   public endpoint before publishing it. It does not create a Tunnel connector
   or credential file.
3. The Hosts deployment installs and supervises `forkmesh-mirror-node`, the
   loopback gateway, and the host HTTPS proxy. `cloudflared` is disabled. The
   daemon signs health and owns catalog/endpoint publication; its retries and
   service lifecycle are independent of whether the desktop or Worker is
   reachable.
4. The desktop reads saved-host origins directly and accepts a node as online
   only after verifying its repository-specific signed `/health` proof. It does
   not poll endpoint registration or start/stop local serving processes.

Configuration refreshes caused by sync, visibility, or serving-permission
changes are consumed by managed hosts. An unavailable or integrity-invalid
public repository is omitted rather than serving an unpinned path.
Private repositories are represented only by the optional opaque ciphertext
store; they are never added as named repository entries.

## Main Worker integration contract

The main Worker owns authorization, selection, geographic/latency preference,
and bounded failover. D1 stores discovery and health metadata only. It never
stores repository bytes.

Configure a dedicated Ed25519 capability identity in the main instance's
gitignored `app/.env.production`:

```bash
python3 - <<'PY'
import base64
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

private = Ed25519PrivateKey.generate()
seed = private.private_bytes(
    serialization.Encoding.Raw,
    serialization.PrivateFormat.Raw,
    serialization.NoEncryption(),
)
public = private.public_key().public_bytes(
    serialization.Encoding.Raw,
    serialization.PublicFormat.Raw,
)
encode = lambda value: base64.urlsafe_b64encode(value).decode().rstrip("=")
print("MIRROR_ROUTER_PUBLIC_KEY=" + encode(public))
print("MIRROR_ROUTER_SIGNING_SEED=" + encode(seed))
PY
```

Move the seed directly into the instance operator's secret manager or
`.env.production`; do not put it in a repository, D1, a mirror manifest, or a
gateway command argument. On first install the complete App deploy publishes
the pair. Later `deploy.sh secrets` calls deliberately preserve it; use the
fail-closed `deploy.sh rotate-mirror-router` path for an intentional change.
This is a service capability key, not a user, repository-decryption, Solana,
or wallet key. Mirrors receive only its public key.

### 1. Endpoint registration

`GET /api/mirrors/https` returns the routing public key and protocol identifiers
without listing endpoints or repositories. A node registers or rotates its
endpoint with:

```http
POST /api/mirrors/https
Content-Type: application/json

{
  "node": "alice-mirror",
  "baseUrl": "https://alice-mirror.example.com",
  "mirrorUrl": "https://git.alice-mirror.example.com",
  "publicKey": "<account-bound Ed25519 public key>",
  "issuedAt": 1784800000000,
  "signature": "<base64url Ed25519 signature>"
}
```

The signature covers:

```text
forkmesh-https-endpoint-v2
<node>
<normalized base URL>
<normalized mirror URL>
<public key>
<issuedAt>
```

Omitting `mirrorUrl` preserves the v1 message and Tunnel-only behavior. The URL
is replaceable routing metadata; the catalog's root commit and canonical clone
URL continue to identify the repository.

The timestamp must be fresh and updates are monotonic. The public key must
already be the node account's primary key or an enabled owner-sign device key.
The response reports public organization aliases currently linked to the node;
actual organization routing continues to use the authoritative
`org_repos.node_owner` link. An org name never grants a mirror access to a
private repository.

Fetch `https://<origin>/forkmesh-mirror.json` over HTTPS and require:

- The fetched origin equals `endpoint.origin`,
  `endpoint.healthUrl`, and `endpoint.manifestUrl`.
- `transport` is `direct-https`; `mainProxyMode` is `masked` for a Tunnel
  endpoint or `direct` for an independently reachable origin.
- The manifest node/public key matches the account-bound mirror identity.
- `payloadSha256` equals SHA-256 of compact sorted-key UTF-8 JSON after removing
  `signature`.
- The Ed25519 signature is valid.
- The base URL passes the public HTTPS/SSRF policy.
- A Tunnel manifest has a proxied CNAME targeting the manifest's
  `<tunnel-id>.cfargotunnel.com`. A direct manifest has a non-proxied public
  A/AAAA record and `edge.kind: public-https-origin`.

A signed DNS claim is not sufficient on its own. Registration resolves each
origin through DNS-over-HTTPS. Tunnel addresses must fall within Cloudflare's
current bounded IPv4/IPv6 range documents; direct addresses must be globally
routable. The scheduled health and direct-proof paths repeat the applicable
fail-closed check with short in-memory caches.

Store or update `mirror_https_endpoints`:

```text
node_bi, node_name, base_url, mirror_url, public_key, registration_sig, issued_at,
checked_at, latency_ms, region, healthy, integrity,
health_sig, forkmesh_verified_at, forkmesh_refs_sha256,
forkmesh_operations_sha256, forkmesh_active, updated_at
```

The row contains no repository bytes, private names, keys, raw IP addresses, or
wallet material.

### 2. Signed health and repository proof

Generic challenge:

```text
GET /health?nonce=<base64url-16..128>&issuedAt=<epoch-ms>
```

The node signs this newline-joined UTF-8 message:

```text
forkmesh-https-health-v1
<node>
<nonce>
<issuedAt>
```

To prove eligibility for mirroring the public `forkmesh/forkmesh` repository:

```text
GET /health?nonce=<nonce>&issuedAt=<epoch-ms>&owner=forkmesh&repo=forkmesh
```

The response adds:

```json
{
  "repositoryProof": {
    "owner": "forkmesh",
    "repository": "forkmesh",
    "available": true,
    "integrity": "ok",
    "refsSha256": "<observed heads/tags digest>",
    "operations": ["blob", "git-info-refs", "git-upload-pack"],
    "operationsSha256": "<digest of newline-joined sorted operations>"
  }
}
```

The signature covers this newline-joined UTF-8 message:

```text
forkmesh-https-health-repository-v1
<node>
<nonce>
<issuedAt>
<owner>
<repo>
<1 when available, otherwise 0>
<ok or unavailable>
<refsSha256>
<operationsSha256>
```

For an unavailable, unknown, or private repository, both digest fields are the
SHA-256 of empty bytes/list and the signed state is `0` plus `unavailable`.
Private and unknown are not distinguished.

Reward eligibility may project the verified public proof into:

```text
forkmesh_active INTEGER NOT NULL DEFAULT 0
forkmesh_verified_at INTEGER NOT NULL DEFAULT 0
forkmesh_refs_sha256 TEXT NOT NULL DEFAULT ''
forkmesh_operations_sha256 TEXT NOT NULL DEFAULT ''
```

Set `forkmesh_active=1` only after checking freshness, node signature,
`available=true`, `integrity=ok`, observed refs against the owner-signed
canonical refs state, and the required operation set. Clear it after a failed or
stale proof. A validly signed proof from a healthy node that is merely
inadmissible right now — `available=false` while the gateway adopts a new
generation, or a digest the pin set has not admitted yet — neither renews nor
clears an unexpired lease: `forkmesh_verified_at` stays put, so the lease still
ends at the normal freshness deadline if the node never proves an admitted
state again. Every merge reaches the whole fleet at once, and clearing on those
proofs used to take all mirrors out together for a minute or two. Reward
selection must additionally require endpoint
`healthy=1`, fresh `checked_at`, and general `integrity='ok'`.

The every-minute cron challenges an oldest-first bounded batch. A failed,
expired, revoked-key, malformed, or invalidly signed response immediately clears
`healthy` and `forkmesh_active`; routing accepts health for no more than two
minutes. A healthy endpoint that does not carry `forkmesh/forkmesh` may still
serve other public repositories, but it is not eligible for the flagship mirror
reward projection.

### Repository URL checks

The repository Mirrors table exposes a `URL check` action for every registered
`mirrorUrl`. It sends `POST
/api/repo/<owner>/<repo>/mirrors/<node>/reachability`; the existing `GET` on the
same path remains the control-plane README reachability probe.

The POST never fails over to another node. It repeats the public-DNS check,
requests a fresh repository proof from that exact origin, verifies the node's
Ed25519 signature and advertised Git operation, and compares the signed refs
digest with the source-attested current/recent pins retained in D1. Only then
does it send a bounded `HEAD` to the exact Smart HTTP upload-pack advertisement
URL and require its standard content type. This remains usable while the source
node is offline because the source's signed pin history is durable metadata;
the mirror cannot choose a replacement digest for itself.

The result is stored in `mirror_repository_integrity`, keyed by repository blind
index and node blind index. It contains only the registered public origin,
reachability, integrity verdict, matching refs digest, status, latency, reason,
and check time—never repository bytes or private identities. The public mirror
discovery response exposes that verdict and digest. A changed `mirrorUrl` or
rotated endpoint registration never inherits an old badge: stored evidence is
applied only when its normalized URL and signed registration timestamp exactly
match the node's current registration. Anonymous checks are coalesced for 30
seconds per repository/node to bound outbound work.

### 3. Per-request capability

After normal public/private repository authorization and mirror selection, map
the client route to one internal target:

```text
/v1/repositories/<owner>/<repo>/<operation>?<allowed-query>
```

Compute the SHA-256 of the exact request body and sign this newline-joined UTF-8
message with the routing Worker's Ed25519 key:

```text
forkmesh-masked-proxy-v1
<node>
<GET|HEAD|POST>
<exact path and query>
<lowercase body sha256>
<request id>
<epoch-millisecond issuedAt>
```

Send these headers:

```text
X-ForkMesh-Node: <node_name>
X-ForkMesh-Request-Id: <12..80 base64url characters>
X-ForkMesh-Issued-At: <epoch milliseconds>
X-ForkMesh-Body-Sha256: <64 lowercase hex>
X-ForkMesh-Signature: <64-byte Ed25519 signature, base64url no padding>
```

The request ID is single-use for the 60-second acceptance window. The gateway
recomputes the body digest and verifies the signature through its configured
external verifier.

### 4. Operations

Native clients may bypass the routing Worker for read-only public repository
data using:

```text
/v1/public/repositories/<owner>/<repo>/<operation>?<allowed-query>
```

That route accepts only `GET`/`HEAD` and only the read operations below. It is
resolved exclusively against repositories explicitly configured as public on
the node. Private replicas, Git upload POSTs, Actions output, pull mutations,
and every other operation continue to require the signed masked-proxy
capability described above.

| Operation | Method | Allowed query | Response |
| --- | --- | --- | --- |
| `git-info-refs` | GET/HEAD | `service=git-upload-pack` | Git upload-pack advertisement |
| `merge-pull` | POST | none; bounded typed JSON body | Asynchronous exact-OID pull merge through an explicitly configured node executor; a published result may carry the node-signed `state`/`stateTs`/`stateSig` refs attestation the relay pins |
| `update-pull-branch` | POST | none; bounded typed JSON body | Asynchronous exact-OID merge of the base branch into a pull request's head branch through the same executor |
| `actions-status` | GET | none | Owner/write-authorized bounded redacted run summaries from this executor |
| `actions-control` | POST | none; typed JSON body of at most 4 KiB | `202` once the router-signed control request is queued for this executor |
| `git-upload-pack` | POST | none | streamed Git upload-pack result |
| `tree` | GET/HEAD | `path`, `ref` | bounded JSON tree with sizes/commit activity |
| `blobs` | GET/HEAD | repeated `path`, optional `ref` | bounded compatibility batch for the web UI |
| `blob` | GET/HEAD | `path`, `ref` | bounded UTF-8/base64 JSON blob |
| `raw` | GET/HEAD | `path`, `ref` | streamed blob bytes |
| `history` | GET/HEAD | `ref` | last 60 commits |
| `commit` | GET/HEAD | `path` (commit hash) | metadata, file list, bounded diff |
| `branches` | GET/HEAD | none | heads/remotes |
| `search` | GET/HEAD | `path` (query), `ref` | bounded fixed-string results |
| `stats` | GET/HEAD | `ref` | files, extensions, contributors |
| `sizes` | GET/HEAD | `ref` | bounded directory-size tree |
| `release-blob` | GET/HEAD | `sha256` | verified streamed release bytes |

`actions-status` is never enabled by default. Its gateway configuration must
name an absolute, normalized `actionsSummaryPath` in the gateway account's
real mode-0700 directory. Same-account files must be owner mode 0600. The
packaged cross-account handoff is accepted only at
`/var/lib/forkmesh-mirror/gateway/actions-summary.json`, where it must be a
regular, single-link `root:forkmesh-mirror` mode-0640 file. The gateway opens
the protected parent and no-follow leaf, checks the effective UID/GID and
inode, and rejects every other group-readable file. A compromised service
account can remove the handoff and make status unavailable, but cannot forge
root ownership. The file is limited to 256 KiB, twenty runs, 16 KiB of
already-redacted log tail per run, and a fifteen-minute lease. The gateway
filters it to the exact requested repository, and the Worker independently
revalidates every field and disables caching. Workflow variables, commands,
local paths, and full logs are not part of this response or the public mirror
catalog. A version 2 summary additionally carries the repository's workflow
catalog, per-run steps, and recent control outcomes; the gateway validates
every added field just as strictly and answers a version 1 document exactly
as before. On the mirror fleet the executor is the Go daemon's sandboxed
runner, which writes version 2 and only names these paths after checking that
this script reads them.

`actions-control` is never enabled by default either, is not a direct public
read, and cannot be reached through the dashboard's `/api/repo` alias. A
repository that lists it must also list `actions-status`, and the gateway
configuration must name `actionsControlPath`: an absolute, normalized path to
a real (non-symlink) directory called `actions-control`, owned by the
gateway's effective UID/GID with mode 0700
(`/var/lib/forkmesh-mirror/gateway/actions-control` when packaged). The
gateway is a courier, not an authority. It validates the body's exact keys,
refuses while no current version 2 summary shows a live executor, and then
atomically writes `{requestId}.json` (mode 0640) holding the verified router
capability next to the verbatim request body; the executor verifies that
Ed25519 signature itself before acting. At most 32 requests wait at once,
files older than ten minutes are removed first, and repeating a `requestId`
is accepted again only with a byte-identical body. The complete wire
contract, including the envelope
([`mirror-actions-control.schema.json`](../mirror-actions-control.schema.json)),
the actions, and summary version 2, is
[Mirror Actions control and summary v2](mirror-actions-control.md).

For upload-pack, require:

```text
Content-Type: application/x-git-upload-pack-request
```

An upload-pack request may use `Content-Encoding: gzip`. The capability body
digest covers the transported compressed bytes; the gateway verifies that
digest first, then expands gzip with the same 8 MiB bound before invoking Git.
Other content encodings fail closed.

Upload-pack advertises standard object filters under Git protocol v2, so
`--filter=blob:none` partial clones and later lazy blob requests remain ordinary
Git operations. Protocol v0's later unadvertised-object request stays blocked
by the hidden-object policy; use `git -c protocol.version=2` or make a full
clone. Shallow boundaries use Git's standard deepen negotiation. The
conformance suite checks both paths, compares signed commit bytes and object
IDs, then runs strict fsck against healthy and deliberately corrupted fixtures.

Upload-pack explicitly leaves `allowTipSHA1InWant`,
`allowReachableSHA1InWant`, and `allowAnySHA1InWant` disabled. This is required
in addition to hiding `refs/forkmesh/`: enabling an object-id escape hatch can
make a guessed internal ref tip fetchable even when its name is absent from the
advertisement.

Do not forward client cookies, authorization headers, Cloudflare credentials,
raw client IP headers, or arbitrary query fields to the mirror. Only a proven
public Git endpoint may be exposed through the intentional redirect; masked
fallback and non-Git responses do not reveal an origin through `Location`,
`Server`, cookies, HTML, or error bodies.

### 5. Direct Git preference and masked failover

For Git upload-pack traffic, the main Worker redirects to a registered
`mirrorUrl` only after a fresh signed repository proof succeeds at that exact
origin. If no direct URL is advertised or its proof fails, the Worker streams
the selected Tunnel response under the original
`https://forkmesh.com/<owner>/<repo>...` URL. Non-Git repository responses keep
the existing masked behavior.

Retry the next eligible endpoint only before response bytes have been committed.
A started Git pack/raw/release stream cannot safely jump mirrors midway. A
subsequent request can use the next healthy endpoint. No eligible endpoint
returns the existing truthful mirror-unavailable response; it must not fall
back to sending repository bytes over the multiplayer socket.

The Worker applies this path to public smart-HTTP upload-pack clone traffic,
repository browse APIs, raw files, and release blobs. Endpoint candidates must
belong to the same public catalog mirror group, pass owner-attested refs pins,
have fresh signed health, and report integrity `ok`.
Approximate request country and measured challenge latency influence ordering;
the per-repository cursor rotates ties, and the clone advertisement pins its
node briefly so the following upload-pack request starts with the same mirror.
Selection is capped at a bounded failover window, so an endpoint the request is
already committed to is held inside that window instead of being rotated
through it: the pinned clone node, the node an in-flight merge job recorded,
and — for an owner-authorized `merge-pull` or `update-pull-branch` — the
mirrors whose last signed health advertisement carried that operation. The
advertisement is only an ordering preference; the per-repository signed
repository proof still decides whether a node may serve the operation.

Private and unknown repositories never enter public endpoint selection and are
never included in endpoint, health, or routing listings. Unauthenticated probes
receive the same not-found response. After a valid owner/share token, private
reads use the separate opaque ciphertext path below; the Worker never falls
back to the desktop content socket.

### 6. Owner-authorized private ciphertext routes

The owner registers or revokes a route with `POST /api/mirrors/private`. The
body names the repository only inside the authenticated TLS request:

```json
{
  "owner": "alice",
  "repository": "private-project",
  "node": "alice-mirror",
  "opaqueId": "<64 lowercase hex characters>",
  "replicaSha256": "<sha256 of the exact .fm-private file>",
  "keyEpoch": 3,
  "active": true,
  "issuedAt": 1784800000000,
  "signature": "<owner Ed25519 signature>"
}
```

The signature covers newline-joined UTF-8:

```text
forkmesh-private-route-v1
<owner>
<repository>
<node>
<opaqueId>
<replicaSha256>
<keyEpoch>
<1 when active, otherwise 0>
<issuedAt>
```

The Worker requires a fresh timestamp, the registered owner's key, an exact
private `owner-sealed-v1` catalog record at the same key epoch, and an existing
account-bound HTTPS endpoint. Updates are monotonic. Migration
`0050_private_mirror_routes.sql` stores only blind repository/binding indexes,
the endpoint blind index, random opaque id, ciphertext digest, epoch, public
signature, and timestamps. There is no list API.
Migration `0053_private_replica_access.sql` adds the exact opaque-id lookup
index for databases that applied the original route migration before the
identity-free client endpoint existed.

An owner or authorized collaborator downloads through
`GET /api/private-replicas/<privateAccessId>` using the existing
`forkmesh-view-v1` or `forkmesh-share-view-v1` token in HTTP Basic. Query
credentials and named private-repository download routes are not accepted.
The Worker first resolves the random id to exactly one encrypted private
catalog record, then checks the owner/share signature. Unknown, inactive,
ambiguous, and unauthorized ids all return the same no-store `404` response.
Only after that authorization does the Worker select a healthy,
integrity-verified endpoint and internally request:

```text
/v1/private-replicas/<opaqueId>
```

The repository name, viewer token, cookies, authorization header, raw client
address, and query string are not forwarded. The gateway validates the
`.fm-private` envelope shape and embedded ciphertext digest, then streams the
exact file with an exact-body SHA-256 ETag. The Worker compares that ETag with
the owner-signed route before streaming under the original ForkMesh URL and
forces `Cache-Control: no-store`. The recipient verifies and decrypts locally.
Wrong authorization, unknown repositories, and missing private repositories
remain indistinguishable before authorization.

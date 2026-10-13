# ForkMesh deployment and scheduled automation

This document covers the isolated deployment and automation tools. None of
these tools writes wallet keys, Cloudflare API tokens, or GitHub tokens to the
repository.

## Cloudflare Worker, D1, DNS, and relay bootstrap

`tools/cloudflare_bootstrap.py` creates or reuses the main D1 and KV storage,
applies all ForkMesh migrations, deploys one complete App Worker, creates
proxied DNS, attaches one broad `hostname/*` App route, and verifies health,
release identity, public snapshots, and an authenticated database/encryption
probe. The App directly owns scheduled ingress, health, version, mirror
identity, and every Durable Object phase; no sibling control script or exact
overlay route is created.

The deployment is a ForkMesh relay and mirror-routing instance. Setting
`--main-relay-url` lets its mirror operators participate in the existing relay
mesh after the main relay approves the new relay. Repository bytes remain on
independently operated mirror hosts; Cloudflare is discovery, routing, and
failover infrastructure rather than permanent repository storage.

Create a scoped Cloudflare API token with only:

- Account Settings read
- Workers Scripts edit
- Workers KV Storage edit
- D1 edit
- Cloudflare Tunnel edit
- Zone read
- DNS edit for the selected zone
- Workers Routes edit for the selected zone

Every App has a dedicated Ed25519 router identity, separate from both the
node/manifest identity and `DATA_KEY`. A new one-click installation needs no
router secret in the environment. On apply, the bootstrap uses the operating
system CSPRNG to create a matched seed/public-key pair without printing it,
validates the pair with Ed25519 public derivation, and atomically creates this
owner-only recovery file:

```text
~/.config/forkmesh/secrets/<worker>.router-identity.json
```

The file is outside the repository with mode `0600`; its parent directory is
created with mode `0700`. Back it up in the operator's encrypted secret store.
Use `--router-identity-backup /secure/path/router-identity.json` or
`FORKMESH_ROUTER_IDENTITY_BACKUP` to choose another out-of-repository path.
`--dry-run` reports that generation would happen on apply, but generates no seed
and creates no file.

Then run the one-click path directly:

```bash
export CLOUDFLARE_API_TOKEN="locally-supplied-token"  # forkmesh-secret-scan:ignore-line
python3 tools/cloudflare_bootstrap.py --auto-configure
```

For an encrypted-backup restore, operators may instead provide both
`MIRROR_ROUTER_SIGNING_SEED` and `MIRROR_ROUTER_PUBLIC_KEY` in the local
environment. The CLI automatically sends them through Wrangler's secret-stdin
path and creates or checks the recovery file. Before Cloudflare mutation it
derives the Ed25519 public key from the 32-byte seed and requires an exact
match. The seed is never put in argv, a temporary config, a summary, a log
message, or JSON output. Secret staging publishes the signing seed
first and the public key last, so an interrupted installation continues to fail
closed instead of advertising a key without its matching signer.

Add `--with-world` to provision the optional World at
`forkmesh-world.<zone>`. The bootstrap stages `world/public`, deploys a
separate static Worker with `APP_ORIGIN=https://forkmesh.<zone>`, and creates
an `APP` service binding to the selected App worker plus only that custom
DNS/route pair. It never reuses the official
`world.forkmesh.com` route. Advanced layouts can set `--world-hostname` and
`--world-worker-name`; both remain inside the selected zone/account. The App
publishes that World in `WORLD_ORIGIN` and credentialed CORS only after
the World Worker, proxied DNS, route, and configured health check succeed. An
App-only install renders an empty `WORLD_ORIGIN` and trusts only its own origin;
a dormant `forkmesh-world.<zone>` hostname is not trusted.

On a new database this generates `DATA_KEY` with the operating system CSPRNG,
writes a mode-`0600` recovery copy under
`~/.config/forkmesh/secrets/<worker>.data-key`, uploads the key directly as a
Worker secret, and proves that D1 plus encryption work before reporting
success. The key is never printed or included in JSON output. Copy that file
into the operator's encrypted backup or secret manager; losing both it and the
Worker secret makes encrypted D1 data unrecoverable. Use
`--data-key-backup /secure/path/forkmesh.data-key` to select another path
outside the repository.

A rerun reads the recovery copy and uses it for the authenticated encryption
probe without replacing an existing remote secret. An older deployment that
has a remote `DATA_KEY` but no recovery copy fails closed: restore the value
from the operator secret store through `--secret-env DATA_KEY`, and the rerun
will create the protected recovery file. Cloudflare exposes secret names but
cannot export secret values.

Router identity has a separate rerun guard. Before mutation, the bootstrap
confirms the named App Worker exists, reads all routes that can overlap the
selected hostname, and proves the hostname has only proxied A, AAAA, or CNAME
records in the requested zone. TXT verification records do not affect that DNS
proof. Only the canonical broad App route or an App-owned custom domain is
accepted. A legacy exact control route is an external cutover blocker;
`--replace-route` cannot bypass or delete it.

Only after those ownership checks does a rerun read
`https://<hostname>/api/mirrors/https` with cache bypass over public HTTPS. It
accepts only the exact ForkMesh identity contract, a canonical 32-byte
base64url public key, HTTP 200, the App ownership marker, and no redirect. An
unavailable, redirected, malformed, or unexpected `503` response fails before
D1, DNS, routes, secrets, or the Worker changes. A local pair or recovery file
must match the published key and cannot rotate it.

An interrupted first installation may resume from its validated mode-`0600`
recovery file only when no live public identity can have been established: for
example, the App has no hostname route yet, or its exact fail-closed identity
response is `503` on an App-owned proxied route. The recovery path is also
used when pending DNS is not yet proxied; the public endpoint is never trusted
through unproxied DNS. A valid live public identity always wins and can never be
overridden by the recovery file.

After deployment and route creation, success requires `/api/mainnode` to prove
that its selected public key has a matching signing seed and the public App
identity route to expose that exact key. `/api/mainnode` and `/api/version`
must report the same deterministic build revision, product version, bundle
fingerprint, and App role. Self-host App and optional World Workers disable
their alternate `workers.dev` origins.

The token-only path succeeds only when the scoped token can see exactly one
account and one active zone. It derives `forkmesh.<zone>` for the relay,
`mirror.<zone>` for the direct endpoint, and stable Worker, D1, and node names.
It fails closed instead of choosing among multiple accounts or zones.

For a broader token or custom topology, provide the advanced fields explicitly:

```bash
export CLOUDFLARE_API_TOKEN="locally-supplied-token"  # forkmesh-secret-scan:ignore-line
export CLOUDFLARE_ACCOUNT_ID="account-id"
export FORKMESH_NODE_PUBLIC_KEY="base64url-ed25519-public-key"
export FORKMESH_MIRROR_MANIFEST_SIGNER="local-forkmesh-identity-signer"

python3 tools/cloudflare_bootstrap.py \
  --hostname mesh.example.com \
  --zone example.com \
  --worker-name example-forkmesh \
  --database-name example-forkmesh \
  --node-name example-node \
  --relay-label "Example relay" \
  --main-relay-url https://app.forkmesh.com
```

If a custom deployment's token can access exactly one account,
`CLOUDFLARE_ACCOUNT_ID` may be omitted. If the token is not in the environment,
the tool prompts for it on a terminal or reads one line from stdin in a
headless invocation.

Important security behavior:

- There is no `--api-token` command-line argument, so the token does not appear
  in the process list.
- The token is passed to Wrangler only through the child-process environment.
- A temporary Wrangler configuration contains public IDs and URLs only. It is
  permission-restricted and removed after deployment.
- The bootstrap creates one App script and one App route. It never deletes or
  replaces a legacy sibling control resource implicitly.
- New installs generate and persist a cryptographically matched router identity
  before Cloudflare mutation. Restored local values must match each other;
  reruns pin the public key already served by the owned App origin and fail
  before mutation if that identity cannot be recovered safely.
- The checked-in `wrangler.toml` and local `.env` files are not modified.
- Existing Worker application secrets can be named explicitly with
  `--secret-env NAME`. Each value is read from the local environment and piped
  directly to `wrangler secret put`; values are never logged or written to the
  generated configuration.
- The `DATA_KEY` recovery file is rejected if it is a symlink, is inside the
  repository, belongs to another user, or grants group/other access.
- `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` are explicitly rejected as
  Worker-secret names.
- Solana private keys are not accepted by this bootstrapper.

A trusted mirror deployment also requires the existing local ForkMesh Ed25519
identity. The public key is supplied through `FORKMESH_NODE_PUBLIC_KEY` or
`--mirror-public-key`. The command in `FORKMESH_MIRROR_MANIFEST_SIGNER` receives
a JSON signing request on stdin and returns:

```json
{
  "publicKey": "the-same-base64url-public-key",
  "signature": "64-byte-ed25519-signature-as-unpadded-base64url"
}
```

The request contains the canonical public payload as base64 and its SHA-256
digest. The signer command is invoked directly, without a shell, and private
identity material remains inside the Qt client, hardware signer, keychain, or
other local signer. A relay-only installation can explicitly use
`--skip-mirror-manifest`; it must not be selected as a trusted repository
mirror until it later publishes a valid signed manifest. When neither a public
key nor signer is supplied, automatic setup selects this safe relay-only mode;
supplying only one is rejected.

Example for an existing, locally managed application secret:

```bash
export DATA_KEY="value-from-the-operator-secret-store"
python3 tools/cloudflare_bootstrap.py \
  --hostname mesh.example.com \
  --zone example.com \
  --skip-mirror-manifest \
  --secret-env DATA_KEY \
  --data-key-backup "$HOME/.config/forkmesh/secrets/example-forkmesh.data-key"
```

After the command succeeds, remove `DATA_KEY` from the shell and confirm the
recovery file is present in the encrypted operator backup:

```bash
unset DATA_KEY
test "$(stat -c '%a' "$HOME/.config/forkmesh/secrets/example-forkmesh.data-key")" = 600
```

Use `--dry-run` for read-only account, zone, DNS, Worker-presence, route, and
router-identity validation. Existing matching D1, DNS, and route resources are
reused. A single unproxied address record can be replaced only with explicit
`--replace-dns`; mixed address records and unexpected routes overlapping the
App hostname fail closed for identity safety even when a replace flag is set.

The bootstrap does not approve itself on another operator’s main relay and does
not install a repository host on an unrelated computer. Those actions require
the respective owner’s authorization.

### Direct-HTTPS mirror discovery contract

Trusted deployments publish:

```text
https://<mirror-host>/forkmesh-mirror.json
```

The strict JSON Schema is `www/docs/mirror-endpoint.schema.json`. Its signed payload
contains:

- The node name and base64url Ed25519 public key.
- `https://<mirror-host>/health`.
- `https://<mirror-host>/{owner}/{repository}` as the direct repository URL
  template.
- The proxied DNS record name and `<mirror-host>/*` Worker route.
- `transport: direct-https` and `mainProxyMode: masked`, allowing the main
  Worker to stream the response while the browser keeps the main ForkMesh URL.
- `repositoryBytesInD1: false`. D1 is limited to discovery, health, routing
  metadata, and signed manifests; repository bytes remain on the independently
  operated host.

The `signature` covers every other manifest field using compact UTF-8 JSON with
sorted keys (`forkmesh-json-sort-v1`). A main router should fetch the manifest
over HTTPS, validate the schema and health URL, recompute `payloadSha256`,
verify the Ed25519 signature against the account-bound node public key, and
reject manifests whose hostname does not match the fetched origin.

## Daily one-way GitHub issue synchronization

`.github/workflows/github-issue-sync.yml` runs every day at 04:17 UTC and can
also be started manually. Manual runs default to dry-run mode.

The implemented direction is:

```text
one GitHub repository -> another GitHub repository
```

It is deliberately not bidirectional. It does not write destination edits back
to the source and does not synchronize pull requests or comments.

Configure these repository variables:

- `FORKMESH_ISSUE_SYNC_SOURCE`: optional `owner/name`; defaults to the workflow
  repository.
- `FORKMESH_ISSUE_SYNC_DESTINATION`: required destination `owner/name`.

For synchronization within the workflow repository, the scoped
`GITHUB_TOKEN` fallback can be sufficient. Cross-repository synchronization
requires encrypted Actions secrets because a repository token cannot access a
different repository:

- `FORKMESH_ISSUE_SYNC_SOURCE_TOKEN`: fine-grained token with source repository
  metadata read and issues read.
- `FORKMESH_ISSUE_SYNC_DESTINATION_TOKEN`: fine-grained token with destination
  repository metadata read and issues read/write.

`FORKMESH_ISSUE_SYNC_TOKEN` can be used instead when one narrowly scoped GitHub
App or fine-grained token has both sets of permissions. Tokens are read only
from environment variables and are never added to reports.

Each destination issue contains a hidden `forkmesh-issue-sync:v1` mapping with
the source repository, source issue number, and synchronized content digest.
That persistent mapping prevents duplicates. Unchanged digests perform no
destination write. New or changed issues preserve:

- Title and body
- Labels, creating missing destination labels
- Open or closed state
- Original author attribution
- Source creation and update timestamps in an attribution footer

The script retries rate limits and temporary server failures with bounded
backoff. It publishes a JSON mapping/count report and Markdown job summary. On
failure, the workflow creates or updates a single administrator-visible issue
in the destination repository and links to the failed workflow run.

Run the same logic locally without exposing tokens on the command line:

```bash
export SOURCE_GITHUB_TOKEN="source-token"
export DESTINATION_GITHUB_TOKEN="destination-token"

python3 tools/github_issue_sync.py sync \
  --source source-owner/source-repo \
  --destination destination-owner/destination-repo \
  --source-token-env SOURCE_GITHUB_TOKEN \
  --destination-token-env DESTINATION_GITHUB_TOKEN \
  --dry-run \
  --report /tmp/forkmesh-issue-sync.json \
  --summary /tmp/forkmesh-issue-sync.md
```

## Native daily public-safe security scan

Daily scans run on each repository owner or mirror node, not in a GitHub
workflow and not through a centrally enumerable queue. Schedule
`tools/native_security_scan_runner.py` once per hour with systemd, launchd, or
the desktop control node. The runner decides whether the repository is due,
uses an exclusive local lease, and requests a short-lived relay lease bound to
the exact repository and 40-character commit.

```bash
export FORKMESH_SESSION_TOKEN="owner-session-token"
python3 tools/native_security_scan_runner.py \
  --checkout /srv/forkmesh/widget \
  --repository alice/widget \
  --relay-url https://app.forkmesh.com \
  --state-dir /var/lib/forkmesh/security-scans/widget
```

The session token stays in process memory. The relay returns a one-use
repository-and-commit-scoped capability and stores only its blind index. The
runner checks Git HEAD before and after scanning, so a moving checkout cannot
be published under the leased commit. Concurrent leases return a bounded retry
time; completed scans are idempotent for 24 hours.

When the relay is offline, use `--offline` or allow the runner's connectivity
fallback. It writes only redacted public-safe artifacts with owner-only file
permissions, records `offline_artifact_pending`, and retries with exponential
backoff capped at six hours. Retention is bounded to the newest 30 commit
directories. No source excerpts, matched secret values, private diagnostics,
session credentials, or lease capabilities are written to disk.

The scanner:

- Inventories public-registry npm, PyPI, and Pub lockfile dependencies.
- Queries OSV with package names and versions only. No source code is sent.
- Detects credential-shaped content without publishing the matched value.
- Performs focused Python and JavaScript static checks.
- Emits only rule names, paths, line numbers, controlled summaries,
  recommendations, and identifiers derived only from the non-secret rule,
  path, and line. Secret matches are never included in a public digest.

Each local artifact directory contains:

- `security-scan.json`
- `security-clipboard.json`
- `security-scan.md`

The rich artifact contract explicitly prohibits raw secret values and source
excerpts. The compact clipboard converter preserves the scan status, commit,
scanner/policy identity, scope, severity/category counts, recommendations, and
limitations in the shape consumed by the virtual world. OSV outages produce a
`Scope limited` report instead of silently claiming complete coverage.

Owners and scoped security reviewers can triage an immutable scan through
`GET/PATCH /api/repo/<owner>/<repo>/security-scans/triage`. Review rows are an
encrypted overlay keyed by opaque scan/finding fingerprints; they do not rewrite
the scanner artifact. Public clipboards expose only aggregate
`unreviewed`/`confirmed`/`dismissed` counts. Per-finding paths, notes, and review
controls require owner or `security_reviewer` authorization, and every mutation
is recorded in the sensitive-action audit log.

The leased ingest request uses `Authorization: Bearer …` and a JSON envelope:

```json
{
  "schemaVersion": 1,
  "type": "forkmesh.security-scan-ingest",
  "rich": {},
  "clipboard": {}
}
```

The Worker authenticates the one-use lease before reading the body, rejects
bodies over 1 MiB, validates both schemas and their cross-document counts, and
binds the body repository and commit to the lease. It never logs the
credential, request body, artifact, or endpoint response body.

The validated envelope is encrypted with the normal Worker `DATA_KEY` before it
is written to D1. D1 stores only a keyed repository blind index, bounded
metadata, and at most the 90 newest encrypted artifacts for that repository.
Re-publishing the same canonical artifact is idempotent. The generic platform
administrator table browser cannot read or edit this table.

Read routes are:

```text
GET /api/repo/<owner>/<repo>/security-scans/latest
GET /api/repo/<owner>/<repo>/security-scans/history?limit=10
```

Anonymous reads of an explicitly public repository receive the compact,
redacted `clipboard` only. `?detail=rich` requires the repository owner's
normal signed-node query or authenticated owner account session. Every read of
a private repository requires the same owner authorization; unauthorized
private and missing repositories return the identical `404 {"error":
"not_found"}` response. Platform-administrator status is not a substitute for
repository-owner authorization.

Run an offline, deterministic scan locally:

```bash
python3 tools/security_scan.py \
  --offline \
  --repository local/forkmesh \
  --output /tmp/security-scan.json \
  --clipboard-output /tmp/security-clipboard.json \
  --summary /tmp/security-scan.md
```

Every report states that automated scans can miss vulnerabilities, applies only
to the scanned commit, and is not a guarantee of security.

## External Solana signer and reward scheduling

`tools/reward_scheduler.py` is isolated from the legacy Worker payout paths. It
does not generate or accept a private key.

The scheduler:

- Requires online, healthy, attested, integrity-checked ForkMesh mirrors.
- Requires a valid self-custodial Solana destination address.
- De-duplicates operator identities and wallet addresses for each round.
- Freezes and hashes the eligible snapshot.
- Selects one recipient deterministically from public entropy that must become
  unpredictable after the snapshot closes.
- Defaults production intents to `mainnet-beta`. `devnet` and `testnet` require
  an explicit development/test selection and matching RPC.

An external wallet or hardware-signer adapter receives the public intent over
stdin and returns a signed transaction over stdout. The signer command is
invoked directly without a shell. Responses containing fields such as
`privateKey`, `seed`, `mnemonic`, or `keypair` are rejected.

Pending rewards are metadata reservations for at most 24 hours. Funds do not
move from the source wallet until an external signer approves a claim. Expiry
marks the allocation returned to source without creating a ForkMesh-controlled
wallet or requiring an on-chain refund transfer.

This is scheduler and signer-protocol support, not a replacement of the legacy
custodial Worker paths. Those paths must not be presented as non-custodial until
they are retired or migrated.

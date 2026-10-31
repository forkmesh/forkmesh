# ForkMesh deployment and scheduled automation

Isolated deployment and automation tools. None of them writes wallet keys, API
tokens or GitHub tokens to the repository.

## Cloudflare Worker, D1, DNS, and relay bootstrap

`tools/cloudflare_bootstrap.py` creates or reuses D1 and KV storage, applies all
migrations, deploys one complete App Worker, creates proxied DNS, attaches
one broad `hostname/*` App route, and verifies health, release identity, public
snapshots and an authenticated database/encryption probe. Repository bytes stay
on independently operated mirrors; Cloudflare provides discovery and routing.
`--main-relay-url https://app.forkmesh.com` joins the instance to an existing
relay mesh once that relay approves it.

The scoped API token needs only: Account Settings read, Workers Scripts edit,
Workers KV Storage edit, D1 edit, Cloudflare Tunnel edit, Zone read, and DNS
edit plus Workers Routes edit for the selected zone. With exactly one account
and one active zone, `--auto-configure` derives every name; otherwise pass
`--hostname`, `--zone`, `--worker-name`, `--database-name` and `--node-name`.

```bash
export CLOUDFLARE_API_TOKEN="locally-supplied-token"  # forkmesh-secret-scan:ignore-line
python3 tools/cloudflare_bootstrap.py --auto-configure
```

### Secrets and identities

- **`DATA_KEY`.** On a new database the bootstrap generates `DATA_KEY` with the
  OS CSPRNG, writes a mode-`0600` recovery copy outside the repository
  (`--data-key-backup` selects the path), uploads it as a Worker secret and
  proves D1 plus encryption work. Losing both copies makes encrypted data
  unrecoverable. A rerun without a recovery copy fails closed until the value
  is restored with `--secret-env DATA_KEY`.
- **Router identity.** A new one-click installation needs no
  router secret in the environment: an Ed25519 router seed and public key are
  generated, cross-checked and saved to an owner-only recovery file
  (`--router-identity-backup`). For a restore, supply
  `MIRROR_ROUTER_SIGNING_SEED` and `MIRROR_ROUTER_PUBLIC_KEY`; the seed must
  derive the key exactly. The seed is never put in argv, a temporary config,
  a log or JSON output. Staging uploads the signing seed
  first and the public key last, so an interrupted install fails closed.
- **Token handling.** There is no `--api-token` flag; the token reaches
  Wrangler only through the child environment. `--secret-env NAME` pipes other
  secrets to `wrangler secret put`. The checked-in `wrangler.toml` is untouched.

### Rerun safety

Before any change the bootstrap confirms the App Worker exists, that the
hostname has only proxied address records, and that only the canonical App
route overlaps it. A legacy exact control route is a blocker that
`--replace-route` cannot remove. It then reads the public App identity route
`https://<hostname>/api/mirrors/https`; an unavailable, redirected or
unexpected response fails before D1, DNS, routes, secrets or the Worker change,
and a local key can never rotate a published one. Success requires
`/api/mainnode` and `/api/version` to report the same build and the matching
key. `--dry-run` performs all checks without mutation. `--with-world` also
deploys the optional World Worker on its own hostname.

### Direct-HTTPS mirror discovery contract

Trusted mirrors publish `https://<mirror-host>/forkmesh-mirror.json`
(schema: `www/docs/mirror-endpoint.schema.json`), signed with the node's
Ed25519 key over sorted compact JSON (`forkmesh-json-sort-v1`). It names the
node key, health URL, repository URL template, `transport: direct-https`,
`mainProxyMode: masked` and `repositoryBytesInD1: false`. The node key comes
from `FORKMESH_NODE_PUBLIC_KEY`; signatures come from the shell-free command in
`FORKMESH_MIRROR_MANIFEST_SIGNER`. `--skip-mirror-manifest` installs a
relay-only node.

## Daily one-way GitHub issue synchronization

`.github/workflows/github-issue-sync.yml` (daily, manual runs default to
dry-run) copies issues from `FORKMESH_ISSUE_SYNC_SOURCE` to
`FORKMESH_ISSUE_SYNC_DESTINATION`, one direction only. Cross-repository runs
need `FORKMESH_ISSUE_SYNC_SOURCE_TOKEN` and
`FORKMESH_ISSUE_SYNC_DESTINATION_TOKEN` (or one `FORKMESH_ISSUE_SYNC_TOKEN`). A
hidden `forkmesh-issue-sync:v1` marker maps each copy and its content digest, so
unchanged issues are not rewritten. `tools/github_issue_sync.py sync` runs the
same logic locally with token names passed via `--*-token-env`. Until
`FORKMESH_ISSUE_SYNC_DESTINATION` is set, the scheduled run writes a notice
and skips the sync instead of failing; a manual run without it still fails.

## Native daily public-safe security scan

`tools/native_security_scan_runner.py` runs hourly on the owner's node, decides
whether a scan is due, and leases a one-use capability bound to the repository
and commit. `tools/security_scan.py` inventories lockfile dependencies against
OSV (names and versions only), flags credential-shaped content without
recording values, and runs focused static checks. Artifacts
(`security-scan.json`, `security-clipboard.json`, `security-scan.md`) contain no
source excerpts or secret values; offline runs keep them locally and retry. The
relay encrypts ingested reports with `DATA_KEY`, keeps the 90 newest per
repository, and serves `security-scans/latest` and `/history`: public
repositories expose the redacted clipboard, everything else needs owner
authorization. Reports are not a guarantee of security.

## External Solana signer and reward scheduling

`tools/reward_scheduler.py` selects one eligible, attested mirror per round from
a frozen snapshot using public entropy, and never generates or accepts a private
key. An external signer receives the intent on stdin and returns a signed
transaction; responses containing key material are rejected. Pending rewards
are 24-hour reservations; funds move only after the signer approves a claim.

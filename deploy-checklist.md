# Independent HTTPS mirror deployment checklist

This checklist promotes a public mirror from a Cloudflare Tunnel-backed Git
relay to an independently reachable HTTPS Git and dashboard node. Complete it
for every node before deleting its tunnel.

## Expected routing behavior

- [ ] Confirm canonical browser reads are an invisible reverse proxy, not an
  HTTP redirect. A visit to `https://app.forkmesh.com/forkmesh/forkmesh/`
  remains at that URL while the app obtains eligible public repository data
  from a healthy direct mirror.
- [ ] Confirm Smart HTTP clone/fetch may receive a temporary direct-mirror
  redirect. Git follows it; this does not change a dashboard browser URL.
- [ ] Confirm the repository page lists `https://mirrorN.forkmesh.com/<owner>/<repo>`
  direct-access links. Those are the fallback when `app.forkmesh.com` itself is
  unreachable; no proxy can preserve an unavailable origin's address bar.

## Data authority and outage scope

- [ ] Treat Git as the replicated repository database: code, refs, releases,
  and repository-native `.forkmesh` issues, pulls, reviews, and discussions
  synchronize as content and signed events.
- [ ] Configure the allowlisted public dashboard projection in `replicaPath`.
  This last-good file is validated by the shared `mirror_replica.py` and
  `public_snapshots.py` contract; it is not a general controller-database replica.
- [ ] Never copy controller account rows, password hashes, bearer sessions, private
  inboxes/repositories, DMs, notifications, payments, or administration state
  to a public mirror. Breakout login uses a separate signed authorization
  projection: account id, enrolled device/passkey public keys, repository roles,
  and monotonic revocation epochs only.
- [ ] Store the authorization projection in service-owned SQLite, apply it
  transactionally after controller signature/schema/sequence validation, and
  retain the last good generation. It is a purpose-built replica, not a
  controller-database copy, and contains no password-equivalent verifier.
- [ ] Configure signed collaboration and SSH submission spools. During an
  outage, accepted repository events remain durable locally and are forwarded
  when the controller returns.
- [ ] Record the supported outage boundary: public browsing, clone/fetch, and
  configured local Git/collaboration operations remain available. An enrolled
  device/passkey may open a short-lived node-local breakout session for the
  repositories named by the last signed authorization generation. Password-
  only login, billing, private data, and account administration remain closed.

## Before deployment

- [ ] Back up the controller database, node configs, local grants, signing
  keys, repository storage, fork registry, and submission spool.
- [ ] Apply controller migrations first, including the direct mirror URL and
  integrity tables.
- [ ] Run `python3 tools/quality_gate.py run --profile core`.
- [ ] Run the mirror gateway, dashboard, replica, routing, SSH, and Go daemon
  tests from the release commit.
- [ ] Ensure each repository has at least two independent direct mirror
  upstreams so synchronization does not depend on `app.forkmesh.com`.

## Configure each public node

- [ ] Give the node a stable public DNS A/AAAA record and a valid TLS
  certificate. Direct-endpoint validation requires DNS that resolves to the
  node rather than a Cloudflare Tunnel/proxy hostname.
- [ ] Put `tools/mirror_gateway.py` behind a local TLS reverse proxy such as
  Caddy or nginx; keep the Python gateway bound to loopback.
- [ ] Set the gateway `publicOrigin` to the node origin and enable the
  `dashboard` block with the mirrored asset repository, canonical origin,
  optional SSH origin, and service-owned `replicaPath`.
- [ ] Set the daemon's `disableCloudflared` to `true` only after the public TLS
  listener is working. Keep the tunnel available for rollback until the
  observation window completes.
- [ ] Publish each repository's replaceable `mirrorUrl` metadata as
  `https://mirrorN.forkmesh.com/<owner>/<repo>.git`; do not change repository
  identity, owner, or root commit.
- [ ] Configure local or fallback SSH authorization only where outage writes
  are required, using signed least-privilege grants.

## Validate direct service

- [ ] `GET https://mirrorN.forkmesh.com/health` succeeds directly and reports
  the expected version.
- [ ] `GET /forkmesh-mirror.json` has a valid signature, current expiry,
  expected repository, and direct public origin.
- [ ] The controller marks the direct endpoint healthy and integrity-valid.
- [ ] `git clone https://mirrorN.forkmesh.com/<owner>/<repo>.git` succeeds, and
  fetch negotiates repository objects without a Tunnel.
- [ ] `https://mirrorN.forkmesh.com/<owner>/<repo>` loads the shared dashboard,
  file tree, README, branches, commits, issues, pulls, discussions, releases,
  and mirror roster from local Git/validated public projection.
- [ ] The canonical repository page stays on `app.forkmesh.com` during normal
  browsing and lists every healthy direct mirror URL.
- [ ] If writes are enabled, test an authorized push, fork, and signed pull
  submission; verify unauthorized refs and keys are rejected.

## Controller outage drill

- [ ] Block controller access from the node without stopping direct node-to-node
  mirror traffic.
- [ ] Re-run direct page, API, clone, fetch, and authorized write checks.
- [ ] Verify the last-good public projection remains readable and is clearly
  marked as a replica response.
- [ ] Verify signed events/submissions accumulate in the durable local spool.
- [ ] Confirm controller-only login, billing, private, and administration
  routes fail closed rather than exposing or inventing local state.

## Recovery and reconciliation drill

- [ ] Restore controller connectivity and run dashboard replica refresh plus
  submission `spool-forward`.
- [ ] Republish catalog and direct endpoint advertisements.
- [ ] Compare repository refs/object integrity across controller and mirrors;
  resolve divergent writable refs according to the signed event/ref policy.
- [ ] Confirm spooled events are idempotently accepted and the local spool is
  acknowledged only after durable controller acceptance.
- [ ] Confirm the next validated public projection supersedes the stale
  last-good copy without schema or signature errors.

## Retire Cloudflare Tunnel

- [ ] Observe stable direct health, dashboard, clone/fetch, and synchronization
  for the agreed window while the Tunnel remains a disabled rollback path.
- [ ] Stop and disable `cloudflared` on the node.
- [ ] Re-run every direct-service and outage check from outside the node's
  network.
- [ ] Delete the Tunnel connector, obsolete Tunnel DNS record, and scoped
  credentials only after the direct endpoint has passed the observation
  window. Record the deleted resource IDs for audit.
- [ ] Keep a tested rollback procedure: restore the connector/config, clear
  `disableCloudflared`, republish endpoint metadata, and revalidate health.

## Go/no-go

- [ ] All compatibility tests pass and existing controller APIs are unchanged.
- [ ] No public mirror contains controller secrets or non-public controller tables.
- [ ] Every advertised mirror URL is HTTPS, signed, reachable, and replaceable.
- [ ] Canonical invisible proxying, direct fallback pages, controller outage,
  recovery reconciliation, and Tunnel-free operation have all been exercised.

See `www/docs/operations/direct-https-mirror-gateway.md`,
`www/docs/operations/mirror-node.md`, and
`www/docs/operations/git-ssh-gateway.md` for configuration details.

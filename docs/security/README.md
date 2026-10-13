# Security

Security is ForkMesh's first priority. This page maps the trust boundaries of
the served stack (forkmesh.com, app.forkmesh.com, world.forkmesh.com), says
where each key lives (by name and path, never by value), and points to the
page that owns each mechanism. The product-level model (identities, signed
records, sealed chat, private repositories) is described in
[`www/docs/security/`](../../www/docs/security/) and the
[threat model](../../www/docs/protocol/threat-model.md). Report
vulnerabilities as described in [`SECURITY.md`](../../SECURITY.md).

## Code map

| Area | Code | Detail |
| --- | --- | --- |
| Node identity | `desktop/src/ForkMeshIdentity.{h,cpp}` | Ed25519 key in `<data>/identity/ed25519.pem`. |
| Signed events | `app/src/events.py`, `app/src/releases.py` | Canonical strings `forkmesh-<thing>-v1\n…` (and v2, below) verified with Ed25519. |
| Encryption at rest | `app/src/entry.py` (`seal` adapters, blind indexes) | [`docs/app/d1.md`](../app/d1.md) |
| Sealed secrets vault | `app/host/forkmesh_host/vault.py`, `vault_crypto.py`, `tools/next_vault.py`, `deploy/core/templates/fmnext-unseal` | [VAULT.md](../VAULT.md) |
| Re-key off the Cloudflare-era `DATA_KEY` | `tools/next_rekey.py` | [REKEY.md](../REKEY.md) |
| Chat encryption | `app/src/entry.py`, web and desktop chat clients | [`www/docs/security/sealed-chat.md`](../../www/docs/security/sealed-chat.md) |
| Private repositories | App, desktop, mirror gateways | [`www/docs/security/private-repository-confidentiality.md`](../../www/docs/security/private-repository-confidentiality.md) |
| Organization roles | `app/src/organization_rbac.py` | [`www/docs/security/organization-roles.md`](../../www/docs/security/organization-roles.md) |
| Node ownership changes (link grants need the node's key and the new owner's acceptance) | `app/src/entry.py` `_account_link_grant` | [NODE_LINK_GRANT.md](NODE_LINK_GRANT.md) |
| Signup bot check | `app/src/turnstile.py` | [OPERATIONS.md](../OPERATIONS.md) "Signup challenge" |
| Front-door checks (Host allowlist, body caps, edge-client MAC) | `app/host/forkmesh_host/request_guard.py`, `asgi.py` | [`deploy/core/edge_client.md`](../../deploy/core/edge_client.md) |
| Outbound (SSRF) guard for the App's `fetch` | `app/host/forkmesh_host/egress.py` | [OPERATIONS.md](../OPERATIONS.md) section 9 |
| Host intrusion detection, lockdown, quarantine | `deploy/core/ids.py`, `app/host/forkmesh_host/ids_records.py`, `ids_alerts.py`, `quarantine.py`, `tools/next_ids.py` | [IDS.md](../IDS.md) |
| Node hardening (Caddy, certsync, nftables, sshd, units) | `deploy/core/render.py`, `deploy/core/certsync.py`, `tools/next_fleet.py` | [OPERATIONS.md](../OPERATIONS.md) section 9 |
| Runner isolation (job user, scopes, egress rules, per-job grants) | `server/runner.go`, `server/sandbox.go`, `server/jobhome.go`, `server/runnergrant.go`, `deploy/core/render.py` | [RUNNERS.md](../RUNNERS.md) |
| Encrypted backups | `tools/next_backup.py` (age) | [OPERATIONS.md](../OPERATIONS.md) |
| Secret scanning before replication | `tools/secret_gate.py`, `conformance/secrets/v1` | [`www/docs/security/secret-scanning.md`](../../www/docs/security/secret-scanning.md) |
| World speech bridge | `world/` | [`www/docs/security/world-local-speech-bridge.md`](../../www/docs/security/world-local-speech-bridge.md) |
| Desktop outbound firewall | `desktop/src/BackoffNetworkAccessManager.{h,cpp}`, `desktop/src/MainWindowChat.cpp` | Every external destination needs an Allow or Deny. |
| Local mesh firewall | `desktop/src/MeshFirewall.{h,cpp}` | Detects a host firewall and offers the one command to let the mesh through, run only on the user's click. |
| Agent filesystem guard | `tools/forkmesh_agent_guard.py` (embedded by `tools/sync_agent_guard_header.py`) | Fail-closed allowlist. |
| Host-mode agent boundaries (per provider and mode: read, execute, reach, secrets, egress, injection fixtures, spend) | `desktop/tests/test_agent_host_boundaries.cpp`, `conformance/agent-adversarial/v1/fixtures.json`, `desktop/src/AgentCredentialEnvironment.h` | [agent-host-boundaries.md](agent-host-boundaries.md) |
| MCP server | `tools/forkmesh_mcp_server.py` | Reads from Git objects only; no shell, no key access. |
| Releases | `tools/forkmesh-release-publish.sh`, `www/public/install.sh` | [`docs/release/README.md`](../release/README.md) |
| Mirror host hardening (the mirror hosts are deleted, [MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)) | `tools/harden_mirror_sshd.sh`, `tools/mirror_fail2ban.sh` | [`docs/server/fleet-operations.md`](../server/fleet-operations.md) |
| Password Vault | `marketplace/packages/vault/` | [`marketplace/packages/vault/README.md`](../../marketplace/packages/vault/README.md) |
| Package runtime | `app/src/entry.py` (`_PluginApiRuntime`) | [`docs/marketplace/README.md`](../marketplace/README.md) |
| Protocol threat model | `conformance/protocol/v1/threats.json` | [`www/docs/protocol/threat-model.md`](../../www/docs/protocol/threat-model.md) |

## Architecture

Trust boundaries, from the user's device outward:

1. **The device.** Holds the node identity key, repository decryption keys,
   SSH private keys, agent-provider logins, wallet signing keys and
   microphone audio. None of these leave it
   ([`www/docs/capability-matrix.md`](../../www/docs/capability-matrix.md)).
2. **The core nodes (the relay).** Three trusted cores (`fmnext-n1..3`) run
   the App on CPython (`app/host/forkmesh_host`), with rqlite as the
   database and Durable Objects clustered across them. The App stores
   routing, authorization and collaboration metadata, encrypted at rest
   under `DATA_KEY` with HMAC blind indexes for lookups. It holds
   `DATA_KEY`, so this protects a stolen database copy, not data from the
   operator. The App never stores Git packfiles in its database. The cores
   run no agent or repository code: `role: core` forces agent jobs, agent
   login and Actions off in the daemon.
3. **Git hosts.** The cores' mirror gateways (and any mirror node a user
   runs) hold Git bytes and release blobs, publish signed health, and are
   eligible to serve a clone only with a fresh account-bound signature and a
   repository proof that matches an accepted revision. The managed mirrors
   `mirror35`–`38` and the tunnel mirrors `mirror2`–`34` are retired and
   their hosts deleted ([MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)).
4. **Runners (untrusted compute).** The runner tier (`fmnext-r1`) runs agent
   jobs. Runners hold no core secret, are not WireGuard peers, keep no
   repositories, and push only `refs/heads/agent/<session>` with a per-job
   grant ([RUNNERS.md](../RUNNERS.md)).
5. **Third parties.** Listed with what they observe in
   [`www/docs/external-dependency-map.md`](../../www/docs/external-dependency-map.md).
   Cloudflare keeps the DNS zone and the forkmesh.com homepage Worker
   `forkmesh-www`, which relays to the App over the internet
   ([CUTOVER.md](../CUTOVER.md) "Workers retired").

Keys on the served stack:

- `DATA_KEY` exists on the operator desktop and, while a node is unsealed,
  only in that App's memory. Bootstrap credentials (rqlite password, mesh
  secret, certsync secret, IDS key) are host-encrypted systemd credentials.
  The Cloudflare zone token stays on the desktop.
- The cores run on their own `DATA_KEY` since the 2026-09-24 re-key; the
  Cloudflare deployment's key was read only into the re-key process.
- The operator desktop must never list `fmnext-*` hosts in its saved hosts.

Signed records: issues, pulls, comments, releases and profile metadata are
signed with the author's Ed25519 identity over a canonical string
(`app/src/events.py`). Release channels for the desktop app are signed with a
separate release publisher key.

## Protocols and interfaces

- Protocol suite: [`www/docs/protocol/README.md`](../../www/docs/protocol/README.md),
  including [signatures and authentication](../../www/docs/protocol/signatures-and-authentication.md)
  and the [threat model](../../www/docs/protocol/threat-model.md).
- Website sessions: [`docs/app/auth-sessions.md`](../app/auth-sessions.md).
- Unseal: unix socket on each core; `DATA_KEY` arrives on SSH stdin, never as
  an argument, environment variable or file on the node.
- Peer unseal between cores over the mesh (`vault_peer_unseal`).
- IDS: signed expected-change manifests and signed quarantine records.
- `x-forkmesh-edge-client` from the `forkmesh-www` Worker: an HMAC-signed
  visitor address and country. The host verifies it before the App sees the
  request and the header never reaches App code. On direct requests Caddy
  drops every `Cf-*` header and sets `Cf-Connecting-Ip` to the TCP peer.
- Signed issue, discussion and pull events (TM-004, open since the 2026-07
  audit). The v1 strings `forkmesh-issue-event-v1`,
  `forkmesh-discussion-event-v1`, `forkmesh-pull-event-v1` and
  `forkmesh-pull-comment-v1` bind no repository, so a signed record could be
  replayed into another repository. Each has a v2 form that inserts the
  repository on the second line: `forkmesh-<kind>-v2\n<owner>/<repo>\n`
  followed by the unchanged v1 body, where `<owner>/<repo>` is the route's
  owner and name, lowercased. Behind an org alias either the alias owner or
  the host owner is accepted. The JSON records do not change; verifiers try v2
  for the target repository first and then v1.
  - API intake (the App's `issues`/`pulls`/`discussions` routes and the
    microjobs issue filer in `app/src/events.py`, the mirror gateway's
    dashboard intake in `tools/mirror_gateway.py`, the SSH `pull-submit` verb in
    `tools/ssh_gateway.py`) accepts v1 only until
    `EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS` (2027-01-01T00:00:00Z), a constant in
    each of those three files. After it, a v1 record is refused on every
    route, so a record signed for one repository cannot be replayed into
    another.
  - Records already committed in a repository's own `.forkmesh/` tree stay
    valid as v1 for good: they were admitted by that repository's intake or
    its owner, and the Git history is the proof of where they belong.
  - Rollout order: (1) verifiers that accept v2 ship first (the App and
    both gateways); (2) signers switch to v2 (web dashboard, desktop, Go
    daemon, native iOS/Android). The desktop signs these records but never
    verifies them, so no desktop release has to land first; (3) the v1 intake
    window closes on the cutoff date. Move the cutoff if step 2 slips.
    TM-004 in the hash-pinned threat register stays `open` until step 3.
  - Status: step 1 and the web dashboard's v2 signing are deployed. Every
    client signs v2 with no switch: desktop (`desktop/src/EventSigning.h`,
    `IssueStore`/`PullStore`/`DiscussionStore::setSigningRepository`), native iOS
    (`FMApi/Services/EventSigning.swift`, used by `InboxService` and
    `FMCode/Collab/CollabSigning.swift`) and native Android
    (`api/EventSigning.kt`, used by `InboxService` and
    `collab/CollabSigning.kt`). The native and desktop helpers fall back to
    the v1 prefix only when owner or name is empty or holds `/`, a newline or
    NUL, the same names the server's `event_signing_repo_key` cannot bind.
    A desktop store signs for the repository it posts to: a cross-owner pull
    submission and the AI review's inbox comments bind the target repository,
    not the checkout they were made from. The AI review also posts its
    comments to the inbox of the repository it reviewed, not the one open in
    the detail view when the model replies, so a repository switch mid-review
    cannot send them where their signature does not verify. The desktop does
    not verify issue, discussion or pull events it syncs, so no desktop
    release had to precede this switch.
  - `tools/export_collaboration_log.py` reports a committed issue event as
    valid when it verifies as v1 or as v2 for the owner and name of the
    `--repository` identity (`forkmesh.com/OWNER/NAME`).
  - The desktop's "Send to source of truth" resends a synced pull as
    authored; the intake decides whether its signature is still accepted.

## Configuration

Where each secret lives. Values never appear in docs or the repository.

| Secret | Where |
| --- | --- |
| Node identity key | `<data>/identity/ed25519.pem` (`~/projects/forkmesh/data/identity/` on the projects layout) |
| Mirror-fleet SSH key and other SSH keys | `<project>/ssh/`; the fleet key is `~/projects/forkmesh/ssh/fleet/next_fleet_ed25519` |
| Tool secrets | `<project>/config/tools/secrets` ([`docs/tools/projects-layout.md`](../tools/projects-layout.md)) |
| Operator files | `~/projects/forkmesh/config/fleet/`: `DATA_KEY`, `secrets.env`, `cloudflare.env`, `cloudflare-dns.env`, `vultr.env`, `backup-age.key` (kept offline), `ids-operator.key` ([OPERATIONS.md](../OPERATIONS.md) section 1) |
| App secrets | Named in `[app] secrets` and `optional_secrets` in `deploy/fleet.toml`, for example `DATA_KEY`, `ADMIN_PASS`, `MIRROR_ROUTER_SIGNING_SEED`, `WWW_EDGE_SECRET`, `MAILTRAP_API_TOKEN`, `APNS_AUTH_KEY`, `DISCORD_BOT_TOKEN`, `TURNSTILE_SECRET_KEY`, `SENTRY_DSN`. Values in `secrets.env`, sealed into the rqlite table `host_secrets` by `tools/next_vault.py sync` ([VAULT.md](../VAULT.md)) |
| IDS settings | `[ids]` in `deploy/fleet.toml` ([IDS.md](../IDS.md)) |
| Release publisher private key | Actions variable `FORKMESH_RELEASE_SIGNING_KEY_PEM` |
| APK keystore, Authenticode certificate | `<data>/release/` on the release host ([`docs/release/README.md`](../release/README.md)) |

## Build, run, test

```sh
python3 -m pytest app/tests/host deploy/tests
python3 -m pytest app/host/tests_cluster     # needs rqlited; see app/host/RQLITE_FINDINGS.md
python3 -m pytest app/tests/test_secret_gate.py app/tests/test_crypto.py app/tests/test_browser_security_headers.py
python3 tools/security_scan.py --help
python3 tools/secret_gate.py --help
```

## Operations

- Rollout order for security changes: node files before the App release
  (006fefd; [OPERATIONS.md](../OPERATIONS.md) section 9).
- After an IDS lockdown: [IDS.md](../IDS.md) "Release".
- The mirror hosts (the tunnel mirrors `mirror2`–`34` and the Vultr mirrors
  `mirror35`–`38`) are deleted; their roles run on the cores and `fmnext-r1`
  ([MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)). The sshd key-only and
  fail2ban scripts they used remain in `tools/`
  ([`docs/server/fleet-operations.md`](../server/fleet-operations.md)).
- Daily mirror-node security scan: `tools/native_security_scan_runner.py`.
- Dependency audit: [`www/docs/security/dependency-security-audit.md`](../../www/docs/security/dependency-security-audit.md)
  (snapshot 2026-09-13; native iOS/Android dependencies not yet scored).
- Incident and embargo process: the governance charter's security decisions
  ([`www/docs/governance/README.md`](../../www/docs/governance/README.md)).

## Security

This whole page. Rules that apply everywhere: keys in memory or in
permission-restricted files only, no plaintext exports of user data left on
disk, least-privilege credentials, and every new outbound fetch treated as a
possible SSRF. Ongoing review is the Sentinel teammate's
([sentinel.md](sentinel.md)); fixed findings and the public open items are in
[findings.md](findings.md).

## Traps

- **User-data wipes.** `install.sh --uninstall/--reinstall/--purge` and
  `uninstall.sh --purge` erase the identity key. They now refuse outside
  `HOME` and from an agent shell, but agents must never run them
  ([`AGENTS.md`](../../AGENTS.md)).
- **A restarted node comes back sealed** and serves nothing until unsealed
  by a peer or the desktop (`fmnext-unseal.timer` on the operator box).
- **The Vultr web console cannot log in as root** after hardening; use the
  rescue ISO.
- **`DATA_KEY` cannot be rotated in place**: ciphertexts carry no key
  version ([`docs/app/d1.md`](../app/d1.md)). Changing it means re-keying
  every row offline ([REKEY.md](../REKEY.md)).
- **Public chat channels are readable by the relay operator** by design
  (shared key served to signed-in accounts, so ForkBot, MCP and webhooks can
  take part). Private channels and DMs are end-to-end encrypted between
  devices: the relay holds only recipient ciphertext. It still publishes the
  device public keys unauthenticated, so an active operator could inject a
  key for future messages
  ([`www/docs/security/sealed-chat.md`](../../www/docs/security/sealed-chat.md)).
- **v1 canonical strings do not bind the repository.** For example
  `forkmesh-pull-comment-v1` signs event type, number, author, time and
  content hash (`app/src/events.py`), so a signed body could be replayed into
  another repository with the same pull number (2026-07-10 audit, TM-004).
  Every client that posts to an inbox now signs the repository-bound v2 form
  and v1 intake closes on 2027-01-01; see "Protocols and interfaces". The
  in-tree writers in `tools/` (`forge_sync.py`, `github_forkmesh_bridge.py`,
  `forkmesh_mcp_server.py`, `branch_pr_review.py`) still sign v1 because
  their records are written straight into the repository they belong to.
- **The CSP allows inline scripts.** `www/public/_headers` and the App's
  fallback headers (`SITE_DOCUMENT_FALLBACK_SECURITY_HEADERS` in
  `app/src/app_runtime.py`) set a Content-Security-Policy, but its
  `script-src` includes `'unsafe-inline'` because the dashboard uses inline
  scripts; a strict policy needs nonces.
- **Shared service credentials.** `optional_secrets` in `deploy/fleet.toml`
  holds service credentials (Mailtrap, APNs, Discord, Sentry, Turnstile), not
  node identity. Its commented-out `SENTRY_DSN` line predates the cutover;
  the name is listed and active further down. Without `TURNSTILE_SECRET_KEY`
  the cores refuse signup and password reset (fail closed).
- **Banned personal identifiers come back through old-lineage branches.**
  Two private names were scrubbed by rewriting `main`. On 2026-10-02 they
  reappeared because an agent branch had been cut from the pre-rewrite
  history (it shared no commit with `main` after June), and the desktop's
  search read that branch, its reflogs and the cores' seed snapshot
  (`refs/inspect/core-main`). `app/tests/test_no_banned_identifiers.py` now
  fails when any tracked file, file name, or commit author/committer on
  `HEAD` contains either name; it stores only SHA-256 hashes of the names,
  so the guard itself names neither. A branch whose merge base with `main`
  is far older than its first commit is the tell: rebuild it on `main`
  (cherry-pick its own commits), point every copy at the rebuilt tip
  (checkout, `data/mirrors/forkmesh-forkmesh.git`), delete the reflog
  entries that lead into the old history, then `git gc`. The fan-out pushes
  branches to the cores without force, so the cores never accepted the old
  lineage.

## Recent changes

- 2026-10-02: banned-identifier guard (`test_no_banned_identifiers.py`)
  after an old-lineage agent branch brought two scrubbed names back; that
  branch was rebuilt on `main` and the old history purged locally (see
  Traps).
- 2026-09-30: Sentinel, the security teammate, reviews every trust-boundary
  change on weekdays and keeps the findings register
  ([sentinel.md](sentinel.md), [findings.md](findings.md)).
- 2026-09-28: Private chat rooms: creator and invited members only, no
  administrator bypass, 404 for non-members on every endpoint and socket
  ([app/chat-private-rooms.md](../app/chat-private-rooms.md)).
- 2026-09-27: `SECURITY.md` names the private report form
  (`/security-report`, and Desktop → Settings → Security) instead of
  "the project website", and routes components the form does not list
  to **Other**. The form's "security policy" link pointed at
  `/SECURITY.md`, which www does not serve (404); it now opens the file
  on app.forkmesh.com. `app/tests/test_public_security_claims.py` checks
  the policy has no stray markup, its relative links resolve, both forms
  offer exactly `SECURITY_REPORT_COMPONENTS`, and the policy link is
  reachable. Kept from `agent/adhoc-2067-sage-demand-work`, whose scope
  rewrite described the retired Cloudflare relay and Flutter client.
- 2026-09-27: the Cloudflare Workers `forkmesh-app`, `forkmesh-world` and
  `forkmesh-next-www` and the D1 database were deleted after a final backup
  ([CUTOVER.md](../CUTOVER.md)); the tunnel mirrors' DNS records were removed
  ([MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)).
- 2026-09-24: IDS (6df712f); security hardening of Caddy, certsync, nftables,
  units and sshd (3cf01fa); host request guard and egress guard (02f5237);
  re-key (6d2cea5); age-encrypted backups (0ba6371); zone-wide DNS token and
  the old Sentry DSN dropped from the vault (0b3ee3b).
- 2026-09-23: sealed vault (b8b95bf).
- 2026-09-22: installer refuses user-data wipes from agent shells or outside
  `HOME` (74f3694a0); mirror sshd key-only and fail2ban.
- 2026-09-18: Password Vault package (end to end encrypted, suite `fmv1`).
- 2026-09-14: one secret-scanning contract before replication (#499).
- 2026-09-14: desktop firewall requires an Allow or Deny for every external
  destination (3e19f77e0).

## Related docs

- [`SECURITY.md`](../../SECURITY.md)
- [VAULT.md](../VAULT.md) · [IDS.md](../IDS.md) · [RUNNERS.md](../RUNNERS.md) ·
  [REKEY.md](../REKEY.md) · [OPERATIONS.md](../OPERATIONS.md) ·
  [CUTOVER.md](../CUTOVER.md)
- [NODE_LINK_GRANT.md](NODE_LINK_GRANT.md) · [sentinel.md](sentinel.md) ·
  [findings.md](findings.md)
- [`www/docs/security/`](../../www/docs/security/)
- [`www/docs/protocol/threat-model.md`](../../www/docs/protocol/threat-model.md)
- [`docs/architecture.md`](../architecture.md)

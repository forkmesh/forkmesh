# Security

Security is ForkMesh's first priority. This page is the security home for users
and developers: it maps the trust boundaries, says where each key lives (by
name and path, never by value), and lists every security page and the code
that owns each mechanism. Algorithms and encryption layers are on
[Encryption and signatures](../encryption.md); who can attack what is in the
[threat model](../protocol/threat-model.md). Report a vulnerability as
[SECURITY.md](../../SECURITY.md) describes.

## Pages

| Page | Covers |
| --- | --- |
| [Private repository confidentiality](private-repository-confidentiality.md) | What a private repository leaks at rest, key epochs, revocation, keyless inspection, GA evidence |
| [Chat encryption and public integrations](sealed-chat.md) | Conversation policies, device keys, sealed messages, the DM ratchet, safety numbers, limits |
| [Organization roles](organization-roles.md) | The deny-by-default role and capability matrix |
| [Node link grants](node-link-grants.md) | Moving a node account to a new owner with consent from both sides |
| [Secret scanning](secret-scanning.md) | `forkmesh-secret-scan-v1`: stopping credentials before the first replica |
| [Hidden Unicode](hidden-unicode.md) | `forkmesh-hidden-unicode-v1`: invisible characters removed from every text field and request body |
| [Host-mode agent boundaries](agent-host-boundaries.md) | What a desktop coding agent can read, run and reach, proven by named checks |
| [World-to-desktop speech bridge](world-local-speech-bridge.md) | Local speech-to-text for a World page without sending audio anywhere |
| [Third-party dependencies](dependency-security-audit.md) | Every dependency we keep and why, and its risk score |
| [Sentinel](sentinel.md) | The security teammate that reviews changes between audits |
| [Findings register](findings.md) | Fixed findings with their guard tests, and open items already public |

Related core pages: [architecture](../architecture.md) (cores, DNS round robin,
runners, read-only mode), [mirroring](../mirroring.md) (signed mirror health and
release verification), [self-hosting](../self-hosting.md) and
[censorship resilience](../censorship-resilience.md) (what still depends on a
provider). Back to the [documentation index](../README.md).

## Trust boundaries

1. **The device** holds the node identity key, repository decryption keys,
   chat device keys, SSH keys, agent-provider logins and wallet keys; none
   leave it.
2. **The core nodes (relay)** run the app with rqlite and are reached by DNS
   round robin ([architecture](../architecture.md)). They store routing,
   authorization and collaboration metadata encrypted at rest under
   `DATA_KEY` with HMAC blind indexes. This protects a stolen database copy,
   not data from the operator. Cores run no agent or untrusted code.
3. **Git hosts** (core gateways and user mirrors) hold Git bytes and release
   blobs, publish signed health, and serve a clone only with a fresh
   account-bound signature and a repository proof matching an accepted
   revision.
4. **Runners** run agent jobs, hold no core secret, are not mesh peers, keep
   no repositories, and push only `refs/heads/agent/<session>` with a per-job
   grant ([runners](../operations/runners.md)).
5. **Third parties** are listed with what they observe in the
   [external dependency map](../external-dependency-map.md).
   Cloudflare hosts the DNS zone and the forkmesh.com homepage Worker
   (`www/worker.js`); it does not proxy app or world traffic.

## Where each key lives

| Key or secret | Where |
| --- | --- |
| Node identity key (Ed25519) | `<data>/identity/ed25519.pem` on the device |
| Chat device keys | browser storage wrapped by a non-extractable key, the desktop secure store, the iOS keychain, Android Keystore-wrapped storage ([chat encryption](sealed-chat.md#device-keys)) |
| Private repository recipient keys | authorized devices only; an organization may escrow one in its own KMS ([private repositories](private-repository-confidentiality.md#key-lifecycle)) |
| SSH keys | `<project>/ssh/`; the fleet key under `ssh/fleet/` |
| Tool secrets | `<project>/config/tools/secrets` |
| `DATA_KEY` | the operator box; on a core only in the app's memory while the node is unsealed ([vault](../operations/vault.md)) |
| Operator files | `~/projects/forkmesh/config/fleet/` (a link into `data/core/config/fleet/`): `DATA_KEY`, `secrets.env`, provider credentials, `ids-operator.key`; the backup decryption key is kept offline |
| DNS token | the operator box only |
| Bootstrap credentials | host-encrypted systemd credentials on each core |
| App secrets | named in `[app] secrets` and `optional_secrets` in `deploy/fleet.toml`, sealed in the vault |
| Release signing key | an Actions variable on the release host |
| Mobile and Windows signing material | `<data>/release/` on the release host |

Fleet hosts are never added to the desktop's saved hosts.

## Code map

| Mechanism | Code | Detail |
| --- | --- | --- |
| Node identity | `desktop/src/ForkMeshIdentity.{h,cpp}`, `server/identity.go` | [portable identities](../portable-identities.md) |
| Signed events | `app/src/events.py`, `app/src/releases.py` | [signatures and authentication](../protocol/signatures-and-authentication.md) |
| Encryption at rest | `app/src/entry.py` (row sealing, blind indexes) | [encryption](../encryption.md#data-on-the-service) |
| Secrets vault and unseal | `app/host/forkmesh_host/vault.py`, `vault_crypto.py`, `tools/next_vault.py` | [vault](../operations/vault.md) |
| Key rotation | `tools/next_rekey.py` | [re-key](../operations/rekey.md) |
| Organization roles | `app/src/organization_rbac.py` | [organization roles](organization-roles.md) |
| Node ownership changes | `_account_link_grant` | [node link grants](node-link-grants.md) |
| Signup bot check | `app/src/turnstile.py` | [operations](../operations/README.md) |
| Front-door checks | `app/host/forkmesh_host/request_guard.py`, `request_policy.py` | [host](../host/README.md) |
| Edge-client header | `x-forkmesh-edge-client`, verified before the app sees the request | [edge client](../www/edge-client.md) |
| Outbound request guard | `app/host/forkmesh_host/egress.py` | [host](../host/README.md) |
| ActivityPub inbox | `ap_inbox_handler` | [ActivityPub federation](../design/activitypub-federation.md#wire-protocol) |
| Hidden Unicode in input | `app/src/hidden_unicode.py`, `www/public/safe-input.js`, `desktop/src/HiddenUnicodeGuard.{h,cpp}`, `HiddenUnicode.swift`, `HiddenUnicode.kt`, `conformance/hidden-unicode/v1` | [hidden Unicode](hidden-unicode.md) |
| Host intrusion detection | `deploy/core/ids.py`, `tools/next_ids.py` | [intrusion detection](../operations/intrusion-detection.md) |
| Node hardening | `deploy/core/render.py`, `deploy/core/certsync.py`, `tools/next_fleet.py` | [operations](../operations/README.md) |
| Runner isolation | `server/runner.go`, `sandbox.go`, `jobhome.go`, `runnergrant.go` | [runners](../operations/runners.md) |
| Encrypted backups | `tools/next_backup.py` (age) | [disaster recovery](../disaster-recovery/README.md) |
| Secret scanning | `tools/secret_gate.py`, `conformance/secrets/v1` | [secret scanning](secret-scanning.md) |
| Desktop outbound firewall | `desktop/src/BackoffNetworkAccessManager.{h,cpp}` | every external destination needs an Allow or Deny |
| Agent filesystem guard | `tools/forkmesh_agent_guard.py` | fail-closed allowlist |
| Agent host boundaries | `desktop/tests/test_agent_host_boundaries.cpp`, `conformance/agent-adversarial/` | [agent host boundaries](agent-host-boundaries.md) |
| MCP server | `tools/forkmesh_mcp_server.py` | reads Git objects only |
| Organization Discord OAuth | `_oauth_callback_browser_bound` in `app/src/organization_discord.py` | [below](#browser-bound-oauth-callbacks) |
| Security audit log | `sensitive_audit_log` | [below](#security-audit-log) |
| Releases | `www/public/install.sh`, release tooling | [releases](../release/README.md) |
| Destructive actions | guarded delete helpers | [destructive actions](../engineering/destructive-actions.md) |
| Vulnerability reports | `security_report_handler` in `app/src/entry.py` | [Sentinel](sentinel.md#reports-from-the-security-webhook) |
| Protocol threat register | `conformance/protocol/v1/threats.json` | [threat model](../protocol/threat-model.md) |

## Mechanisms

### Unseal

Each core has a unix socket for unsealing. `DATA_KEY` arrives on SSH stdin,
never as an argument, environment variable or file. Peers unseal each other
over the mesh; a restarted node comes back sealed until then
([vault](../operations/vault.md)). IDS changes use signed expected-change
manifests and signed quarantine records
([intrusion detection](../operations/intrusion-detection.md)).

### Edge-client header

`x-forkmesh-edge-client` is an HMAC-signed visitor address from the homepage
Worker, verified before the app sees the request; direct requests have
client-address headers replaced by the TCP peer
([edge client](../www/edge-client.md)).

### Browser-bound OAuth callbacks

The organization Discord OAuth callback must come from the browser that
started the connection. Each Connect sets its own transaction cookie,
`forkmesh_discord_oauth_<first 16 hex of the state>` (SameSite=Lax, path
`/api/integrations/discord/callback`, ten minutes), and the callback finishes
only when a cookie holds exactly the returned state
(`_oauth_callback_browser_bound` in `app/src/organization_discord.py`).
Without it an owner could send the authorize link to another server's admin
and bind that server to their organization; state and PKCE alone do not stop
that. The session cookie cannot be checked instead: it is SameSite=Strict and
is not sent on the redirect back from discord.com. Two tabs or two Connect
clicks both finish (one cookie each). A missing cookie or a different browser
gets `invalid_context`, and the World Discord panel says nothing was
connected.

### Security audit log

`sensitive_audit_log` keeps 365 days (`SECURITY_AUDIT_RETAIN_MS`, pruned by
the `sensitive-cleanup` cron). An agent run's status report
(`organization.task_agent_status`, `world/src/world_office_tasks.py`) writes an
audit row only when the run's status changes from the stored one; a repeated
report of the same status still updates the task but adds no row, because one
row per desktop status heartbeat fills the log on 1 GB nodes (19 MiB seen).

## Practice

- Keys only in memory or permission-restricted files; no plaintext exports of
  user data left on disk.
- Least-privilege credentials, scoped per tool and per provider.
- Treat every new outbound fetch as a possible server-side request forgery
  and route it through the egress guard.
- Validate input at trust boundaries; render user text as text, never HTML.
- Sign security-relevant records over canonical strings that bind their
  context (repository, node, time, nonce).
- Fail closed when a security dependency (key, challenge secret, signature)
  is missing.
- Roll node-file security changes before the app release that relies on them.

## Traps

- User-data wipes (`install.sh --uninstall/--reinstall/--purge`,
  `uninstall.sh --purge`) erase the identity key; agents must never run them.
- After hardening, the provider web console cannot log in as root; use the
  rescue image.
- `DATA_KEY` cannot be rotated in place; rotation is an offline re-key.
- Public chat channels are readable by the relay operator by design; private
  channels and direct messages are end-to-end encrypted, and the server still
  knows which two accounts a DM joins ([chat encryption](sealed-chat.md)).
- The content security policy still allows inline scripts; a strict policy
  needs nonces.
- `app/tests/test_no_banned_identifiers.py` guards against personal
  identifiers returning through branches cut from rewritten history; rebuild
  such branches on `main`. It also bans the owner's personal Reddit handle
  (words joined by `_` count as one word, so the handle matches as a whole).
  Docs and tests use the desk's accounts or the `ada_lovelace` placeholder.

## Build, run, test

```sh
python3 -m pytest app/tests/host deploy/tests
python3 -m pytest app/tests/test_secret_gate.py app/tests/test_crypto.py app/tests/test_browser_security_headers.py
python3 tools/security_scan.py --help
python3 tools/secret_gate.py --help
```

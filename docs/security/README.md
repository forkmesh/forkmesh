# Security

Security is the first priority. This page maps the trust boundaries of the
served stack, says where each key lives (by name and path, never by value),
and points to the page that owns each mechanism. The product-level model
(identities, signed records, sealed chat, private repositories) is published
under `www/docs/security/`; report vulnerabilities as described in
`SECURITY.md`.

## Code map

| Area | Code | Detail |
| --- | --- | --- |
| Node identity | `desktop/src/ForkMeshIdentity.{h,cpp}`, `server/identity.go` | Ed25519 key in `<data>/identity/ed25519.pem` |
| Signed events | `app/src/events.py`, `app/src/releases.py` | canonical strings verified with Ed25519 |
| Encryption at rest | `app/src/entry.py` (row sealing, blind indexes) | |
| Secrets vault | `app/host/forkmesh_host/vault.py`, `vault_crypto.py`, `tools/next_vault.py` | [VAULT.md](../VAULT.md) |
| Key rotation | `tools/next_rekey.py` | [REKEY.md](../REKEY.md) |
| Organization roles | `app/src/organization_rbac.py` | |
| Node ownership changes | `_account_link_grant` | [NODE_LINK_GRANT.md](NODE_LINK_GRANT.md) |
| Signup bot check | `app/src/turnstile.py` | [OPERATIONS.md](../OPERATIONS.md) |
| Front-door checks | `app/host/forkmesh_host/request_guard.py`, `request_policy.py` | [host/README.md](../host/README.md) |
| Outbound request guard | `app/host/forkmesh_host/egress.py` | [host/README.md](../host/README.md) |
| Host intrusion detection | `deploy/core/ids.py`, `tools/next_ids.py` | [IDS.md](../IDS.md) |
| Node hardening | `deploy/core/render.py`, `deploy/core/certsync.py`, `tools/next_fleet.py` | [OPERATIONS.md](../OPERATIONS.md) |
| Runner isolation | `server/runner.go`, `sandbox.go`, `jobhome.go`, `runnergrant.go` | [RUNNERS.md](../RUNNERS.md) |
| Encrypted backups | `tools/next_backup.py` (age) | |
| Secret scanning | `tools/secret_gate.py`, `conformance/secrets/v1` | |
| Desktop outbound firewall | `desktop/src/BackoffNetworkAccessManager.{h,cpp}` | every external destination needs an Allow or Deny |
| Agent filesystem guard | `tools/forkmesh_agent_guard.py` | fail-closed allowlist |
| Agent host boundaries | `desktop/tests/test_agent_host_boundaries.cpp`, `conformance/agent-adversarial/` | [agent-host-boundaries.md](agent-host-boundaries.md) |
| MCP server | `tools/forkmesh_mcp_server.py` | reads Git objects only |
| Releases | `www/public/install.sh`, release tooling | [release/README.md](../release/README.md) |
| Destructive actions | guarded delete helpers | [engineering/destructive-actions.md](../engineering/destructive-actions.md) |
| Protocol threat model | `conformance/protocol/v1/threats.json` | |

## Trust boundaries

1. **The device** holds the node identity key, repository decryption keys,
   SSH keys, agent-provider logins and wallet keys; none leave it.
2. **The core nodes (relay)** run the app with rqlite. They store routing,
   authorization and collaboration metadata encrypted at rest under
   `DATA_KEY` with HMAC blind indexes. This protects a stolen database copy,
   not data from the operator. Cores run no agent or untrusted code.
3. **Git hosts** (core gateways and user mirrors) hold Git bytes and release
   blobs, publish signed health, and serve a clone only with a fresh
   account-bound signature and a repository proof matching an accepted
   revision.
4. **Runners** run agent jobs, hold no core secret, are not mesh peers, keep
   no repositories, and push only `refs/heads/agent/<session>` with a per-job
   grant.
5. **Third parties** are listed with what they observe in
   `www/docs/external-dependency-map.md`. The DNS zone and the homepage Worker
   remain on a CDN provider.

Keys: `DATA_KEY` exists on the operator box and, while a node is unsealed,
only in its app's memory. Bootstrap credentials are host-encrypted systemd
credentials. The DNS token stays on the operator box. Fleet hosts are never
added to the desktop's saved hosts.

## Protocols

- Unseal: unix socket on each core; `DATA_KEY` arrives on SSH stdin, never as
  an argument, environment variable or file. Peers unseal each other over the
  mesh.
- IDS: signed expected-change manifests and signed quarantine records.
- `x-forkmesh-edge-client`: an HMAC-signed visitor address from the homepage
  Worker, verified before the app sees the request; direct requests have
  client-address headers replaced by the TCP peer.
- **Repository-bound event signatures.** The v1 strings
  `forkmesh-issue-event-v1`, `forkmesh-discussion-event-v1`,
  `forkmesh-pull-event-v1` and `forkmesh-pull-comment-v1` bind no
  repository, so a signed record could be replayed into another repository.
  The v2 form inserts the repository on the second line:
  `forkmesh-<kind>-v2\n<owner>/<repo>\n` followed by the v1 body (owner and
  name lowercased; behind an organization alias either owner is accepted).
  Verifiers try v2 first. API intake accepts v1 only until
  `EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS` (a constant in each intake file);
  records already committed in a repository's own `.forkmesh/` tree stay valid
  as v1. All interactive clients sign v2.

## Where secrets live

| Secret | Where |
| --- | --- |
| Node identity key | `<data>/identity/ed25519.pem` |
| SSH keys | `<project>/ssh/`; the fleet key under `ssh/fleet/` |
| Tool secrets | `<project>/config/tools/secrets` |
| Operator files | `~/projects/forkmesh/config/fleet/` (`DATA_KEY`, `secrets.env`, provider credentials, `ids-operator.key`; the backup decryption key kept offline) |
| App secrets | named in `[app] secrets` and `optional_secrets` in `deploy/fleet.toml`, sealed in the vault |
| Release signing key | an Actions variable on the release host |
| Mobile and Windows signing material | `<data>/release/` on the release host |

## Build, run, test

```sh
python3 -m pytest app/tests/host deploy/tests
python3 -m pytest app/tests/test_secret_gate.py app/tests/test_crypto.py app/tests/test_browser_security_headers.py
python3 tools/security_scan.py --help
python3 tools/secret_gate.py --help
```

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
- Ongoing review: [sentinel.md](sentinel.md); fixed findings and open items:
  [findings.md](findings.md).

## Security audit log

`sensitive_audit_log` keeps 365 days (`SECURITY_AUDIT_RETAIN_MS`, pruned by
the `sensitive-cleanup` cron). An agent run's status report
(`organization.task_agent_status`, `world/src/world_office_tasks.py`) writes an
audit row only when the run's status changes from the stored one; a repeated
report of the same status still updates the task but adds no row. Why: the
2026-10-16 table audit found the log at 19 MiB and 74.8k rows on 1 GB nodes,
mostly one identical row per desktop status heartbeat.

## Traps

- User-data wipes (`install.sh --uninstall/--reinstall/--purge`,
  `uninstall.sh --purge`) erase the identity key; agents must never run them.
- A restarted node comes back sealed until unsealed.
- After hardening, the provider web console cannot log in as root; use the
  rescue image.
- `DATA_KEY` cannot be rotated in place; rotation is an offline re-key.
- Public chat channels are readable by the relay operator by design; private
  channels and direct messages are end-to-end encrypted. Until chat protocol
  v3 ([../app/chat-protocol-v3.md](../app/chat-protocol-v3.md)) an active
  operator could inject a device key for future messages; v3 signs device
  bundles, holds a new device of a DM contact until the user approves it, and
  shows safety numbers. The server still knows which two accounts a DM joins.
- The content security policy still allows inline scripts; a strict policy
  needs nonces.
- `app/tests/test_no_banned_identifiers.py` guards against personal
  identifiers returning through branches cut from rewritten history; rebuild
  such branches on `main`.

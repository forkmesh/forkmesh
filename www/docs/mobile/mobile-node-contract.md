# Mobile node contract

**Current-behavior note (2026-09-28).** Principle 1.2's “never
device-to-device” boundary is superseded for optional desktop control. A phone
can now use the desktop's authenticated, end-to-end encrypted control protocol
directly on the LAN or forwarded through the instance relay. On current
`main`, a signed-in phone can discover enabled desktops in its own account
without LAN pairing; the signed v1.2.5 phone build still has to learn a desktop
on LAN once. The phone remains independently useful, and it never becomes a
repository host. See [Mobile access to owner-encrypted agent
sessions](mobile-agent-access.md#controlling-a-desktop-on-the-lan-or-through-the-relay).
The rest of this v1 record preserves the original node and owned-Git contract;
its “Worker” name means the instance App/relay control plane in the current
self-hosted stack.

Status: v1 (2026-08-23). This is the wire and policy contract that makes the
iOS and Android apps first-class **mobile nodes** under a ForkMesh account — a peer of
the desktop node, coupled to it only through the account on the Worker.

Companion documents: [`owned-git-engine-spec.md`](owned-git-engine-spec.md),
[`owned-git-threat-model.md`](owned-git-threat-model.md),
[`owned-git-compatibility-matrix.md`](owned-git-compatibility-matrix.md).

## 1. Principles

1. **Nodes author and publish; mirrors serve.** A mobile node creates, commits,
   and publishes repositories. It never hosts clones and never runs in the
   background. Managed mirrors and desktop/server nodes serve bytes.
2. **Coupled through the account, never device-to-device.** Desktop and mobile
   never talk directly. Either is fully useful alone. The desktop-command queue
   stays an optional convenience and degrades to "no desktop node on this
   account".
3. **Login is the bind.** Password (+ TOTP only when enrolled) authenticates the
   human; the phone's Ed25519 key is registered as a `mobile_node` device on that
   login. There is no QR code, no desktop approval, no second ceremony.
4. **The web app is the node registry.** Listing, revoking one node, and
   signing out everywhere happen on the web (the phone can read the list).
5. **No app lock (for now).** User presence (Face ID / Touch ID / device
   passcode) is demanded only for the destructive list in §6, never for read,
   browse, commit, push, publish, comment, issue, or PR.
6. **Capability strings limit blast radius.** A `mobile_node` key can publish
   and sign as the owner but can never host, mirror, touch wallet/payout
   settings, secrets, or mirror process control.

## 2. Device kinds and capabilities

| kind | capabilities | notes |
|---|---|---|
| `desktop_node` | `browse,comment,submit_issue,submit_pr,host_repo,mirror_repo,publish_repo,owner_sign` | unchanged |
| `mobile_node` | `browse,comment,submit_issue,submit_pr,publish_repo,owner_sign` | **new**. No `host_repo`, no `mirror_repo`. |
| (client) | `browse,comment,submit_issue,submit_pr` | web/anonymous, unchanged |

`contribution_key_proof` is appended to either node kind once the device proof
verifies (same as today).

A `mobile_node` row must never satisfy a check that requires hosting:
`desktop_node_proof_required` paths, `host_repo` / `mirror_repo` gates, mirror
process control, and `/api/accounts/reserve`/`claim-node` remain desktop-only.

`_owner_signing_pubkeys(owner)` already unions every enabled, non-revoked
`owner_sign` device; a mobile key therefore signs `/api/sync`, catalog
publications, repository-delete and other owner-signed requests with no
further change.

## 3. Bind (login)

`POST /api/accounts/login`

```json
{
  "identifier": "alice",
  "password": "…",
  "totp": "123456",
  "pubkey": "<base64url raw Ed25519 public key>",
  "deviceTs": "<unix ms, string>",
  "deviceSig": "<base64url Ed25519 signature>",
  "deviceKind": "mobile_node",
  "deviceLabel": "iPhone 15 Pro · iOS 19"
}
```

- `deviceSig` signs the existing canonical
  `forkmesh-device-bind-v1\n{account}\n{pubkey}\n{deviceTs}`
  (`_device_bind_canonical`). Skew limit and proof failure (`device_proof_required`,
  `device_key_conflict`) behave exactly as for desktop.
- `deviceKind` absent or `desktop_node` → existing desktop path, unchanged.
- `deviceKind == "mobile_node"` →
  - `_register_account_device(kind="mobile_node", …)` with
    `MOBILE_NODE_CAPABILITIES` (+ `contribution_key_proof`).
  - **Never** sets the account's primary `rec["pubkey"]`, never calls
    `touch_registered_node` as a desktop host, never counts as `desktopCapable`.
  - Any other `deviceKind` value → `400 {"error":"invalid_device_kind"}`.
- A session minted on a bound login records the device: `account_sessions`
  gains `device_bi TEXT NOT NULL DEFAULT ''`.

Response (additions / values for a mobile bind):

```json
{
  "sessionToken": "…",
  "sessionKind": "mobile_node",
  "deviceKind": "mobile_node",
  "deviceKeyMatched": true,
  "desktopCapable": false,
  "capabilities": ["browse","comment","submit_issue","submit_pr","publish_repo","owner_sign"],
  "canHost": false,
  "canPublish": true,
  "canOwnerSign": true,
  "deviceId": "<device_bi[:16]>",
  "devices": [ { "id": "...", "kind": "desktop_node", "label": "…", "enabled": true, "lastSeen": 0, "capabilities": [] } ]
}
```

First bind of a *new* `mobile_node` row raises a best-effort "new node added"
in-app notification for the account (and email when the account has a verified
address); failures never fail the login.

## 4. Node registry (web is source of truth)

All routes require an account session. Revocation routes additionally require
**sudo re-auth**: body `{"password": "…", "totp": "…"}` re-verified against the
account even inside a live session (protects against a hijacked web session).

| Route | Behaviour |
|---|---|
| `GET /api/accounts/devices` | `{ "devices": [ { id, kind, label, capabilities[], enabled, lastSeen, createdAt, current } ] }`. `current` is true for the device bound to the calling session. Revoked rows are included with `enabled:false` and `revokedAt`. |
| `DELETE /api/accounts/devices/{id}` | Sudo. Sets `revoked_at=now, enabled=0`; revokes every `account_sessions` row with that `device_bi`; audit `account.device_revoke`. Applies to `desktop_node` too (decision: revocable, sudo-gated). Returns `{ ok, currentRevoked }`. |
| `POST /api/accounts/devices/check` | Body `{ "pubkey": "…" }`. Returns `{ ok, owned, nodeName }`: `owned` is true only when the key is the caller's account primary key or one of its enabled, unrevoked `owner_sign` device keys. Answers only about the caller's own account and returns no keys. The desktop uses it to let a phone on the same account control it without a pairing code (`docs/desktop/networking.md`). |
| `POST /api/accounts/devices/revoke-all` | Sudo. Revokes every device except the caller's own and every session except the current; audit `account.device_revoke_all`. Body may carry `{"kinds":["mobile_node"]}` to restrict. |

Effects of revocation:
- `_owner_signing_pubkeys` already excludes the key → every owner-signed call
  (sync, catalog publish, push capability) fails closed on the next request.
- Session lookups for a revoked device's sessions fail as revoked sessions
  (`401`). The Worker adds `"error": "device_revoked"` when the session row's
  `device_bi` points at a revoked device so clients can distinguish it.
- A revoked `account_devices` row is permanent (`ON CONFLICT … enabled` stays 0);
  re-binding therefore needs a **new key**. The app, on `device_revoked` /
  invalid session, clears the session, **rotates its Ed25519 seed**, keeps
  local repositories (user data), and returns to login with the banner
  "This device was signed out from the web."

## 5. Audit and notifications

- `account.device_bind` (kind, label) on first registration of a row.
- `account.device_revoke`, `account.device_revoke_all`.
- Catalog publications and pushes carry the signing device pubkey; the catalog
  record already stores `maintainer` (the signing key) so web can show which
  node published.

## 6. Destructive actions (user presence required on the phone)

- delete a local repository
- delete / transfer / archive a published repository
- revoke a device / sign out everywhere (if ever surfaced in-app)
- change password / TOTP / security settings

Implementation: `local_auth` (`biometricOnly: false`, falls back to device
passcode). If the device has no passcode/biometric enrolled the action proceeds
with an explicit in-app confirmation dialog instead.

## 7. Publish and push (Phase 3)

The owned Git engine speaks smart HTTP (protocol v2 for reads, receive-pack for
writes) to the **app origin only**; the Worker is the masked proxy in front of
the mirror gateway for both reads and writes, exactly as for clones today.

- `GET  /{owner}/{repo}/info/refs?service=git-receive-pack`
- `POST /{owner}/{repo}/git-receive-pack`

Authentication of a write: signed device headers (a browser-issued Basic
credential is the one alternative — see the note at the end of this section)
`X-ForkMesh-Pubkey`, `X-ForkMesh-Ts`, `X-ForkMesh-Sig` over
`forkmesh-git-push-v1\n{owner}\n{repo}\n{method}\n{ts}\n{sha256hex(body)}`
(the advertisement GET signs the sha256 of the empty body; `ts` is bounded by
the login skew window). The Worker accepts when the pubkey is the owner
account's primary key, or ∈ `_owner_signing_pubkeys(owner)` **and** the
device row carries `publish_repo` and is enabled, or the key resolves to an
account holding org write permission on a linked repository. It then forwards
to the gateway with the existing per-request masked-proxy capability
(`forkmesh-masked-proxy-v1`, method GET/POST, target
`/v1/repositories/{owner}/{repo}/git-receive-pack`). The Worker holds a push
body of up to 8 MB (`HTTPS_MIRROR_BODY_MAX_BYTES`) to verify the signature
over it. A signer wanting a larger pack also sends `X-ForkMesh-Body-Sha256`
with the same `sha256hex(body)` it signed: the Worker then verifies the
signature against the stated digest, streams the body to the gateway
without holding it, and binds the routing capability to that digest, so the
gateway refuses the pack unless its bytes hash to what the device signed.
Without the header a signed body above 8 MB is refused with
`413 push_too_large`. A pack is further bounded by the gateway's
`receiveMaxInputBytes` (256 MiB by default) and by the app origin's edge
request-body ceiling (100 MB on Cloudflare's Free plan). A stock git with a
Basic credential is streamed the same way, bound to a fixed streamed-body
capability digest because it cannot state one.
Endpoint selection prefers healthy routed endpoints whose fresh signed
repository proof advertises `git-receive-pack`; a repository with no such
endpoint (the first push of a new repository) falls back to the node named by
the Worker env var `HOSTED_GIT_WRITE_NODE`, resolved through the same
endpoint registration store, else `503 push_unavailable`.

Gateway (`tools/mirror_gateway.py`): accepts `git-receive-pack` only under a
valid capability whose signed method/target match exactly; runs
`git receive-pack --stateless-rpc` with `receive.denyDeletes=true`,
`receive.denyNonFastForwards=true`, `receive.fsckObjects=true`, hooks
disabled (`core.hooksPath=/dev/null`), and a bounded
`receive.maxInputSize` (`limits.receiveMaxInputBytes`); creates the bare
repository `<writableRepositoryRoot>/<owner>/<repo>.git` on first authorized
push when the owner is accepted by the gateway's `writableOwners` allowlist
(a named owner, or the `["*"]` wildcard a node holding website-created
repositories uses), and both
creates and serves such a repository only while that owner remains on the
allowlist, so removing an owner revokes push and read together; records
accepted pushes in the atomic `hosted-pushes.json` sidecar next to the
gateway config and touches the refresh path-unit marker so the existing
refresh/registration cycle serves and health-registers the repository like
any mirror (the gateway itself already answers reads for sidecar
repositories).

Catalog: after a successful first push the phone publishes the catalog record
itself (`POST /api/repositories`, `catalogSigVersion: 2`, `maintainer` = its own
pubkey, `source: "local-node"`). The `account_required` gate (primary pubkey)
is relaxed to "at least one enabled `owner_sign` device" so a phone-only
account can publish.

Collision gate: repositories are namespaced per owner, so a different owner
can never collide. Within the account, the existing device-scoped source
authority already covers re-publication: a publish from a device that is not
the stored authority is accepted only as a subordinate device mirror
(`202` with `mirror: true` and the `canonicalNodeId`), never replaces the
canonical record, and a replayed older record is refused with
`409 stale_update` — the app treats a `mirror: true` answer for a name it
believes it authored as the signal to ask the user to rename.

Engine guardrails on the client: no force, no delete, no hook execution,
foreground-only, isolates, resource caps, cancellation (see engine spec).

**Web-created repositories (issue #676).** A stock `git` cannot sign a
per-request body digest, so the same receive-pack route additionally accepts
one HTTP Basic credential: username = account name, password = a keyed MAC
over (account, owner, repository, browser session id, expiry) that the website
issues from a signed-in session (`app/src/hosted_git.py`). Nothing is stored —
signing that session out revokes it — and the ordinary owner/organization push
permissions are re-derived on every request. An unauthorized receive-pack
request answers `401` with `WWW-Authenticate: Basic realm="forkmesh"`, which a
signing client ignores. Because a web-only account holds no signing key, the
catalog record for such a repository is Worker-authored (`source:
"hosted-web"`) instead of node-signed, and the refs attestation a public read
routes against is recorded by the Worker from the hosting node's own signed
repository proof after each accepted push. See
[hosted-repositories.md](../hosted-repositories.md).

## 8. App Store / platform notes

No background modes in the phone app. Owned Git engine, no runtime code download, no hook
execution. Non-exempt encryption already declared. `NSFaceIDUsageDescription`
required for the §6 gate. Listing copy says "publish", never "host" or "server".

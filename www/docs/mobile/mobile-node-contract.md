# Mobile node contract

The wire and policy contract that makes the iOS and Android apps **mobile
nodes** of a ForkMesh account: peers of the desktop node, coupled to it through
the account on the relay. The owned Git engine is specified in
[`owned-git-engine-spec.md`](owned-git-engine-spec.md).

Principle 1.2's "never device-to-device" boundary is
superseded for optional desktop control: a phone may open the desktop's authenticated, end-to-end
encrypted control protocol on the LAN or through the relay. See
[Mobile access to owner-encrypted agent sessions](mobile-agent-access.md#controlling-a-desktop-on-the-lan-or-through-the-relay).
The phone stays independently useful and never hosts repositories.

## 1. Principles

1. **Nodes author and publish; mirrors serve.** A mobile node creates, commits
   and publishes repositories. It never hosts clones and never runs in the
   background.
2. **Coupled through the account.** Desktop and phone are each fully useful
   alone (optional desktop control aside).
3. **Login is the bind.** Password (plus TOTP when enrolled) authenticates the
   person; the phone's Ed25519 key is registered as a `mobile_node` device on
   that login. No QR code or second ceremony.
4. **The web app is the node registry.** Listing and revoking nodes happens on
   the web.
5. **User presence only for destructive actions** (section 6).
6. **Capabilities limit blast radius.** A `mobile_node` key can publish and
   sign as the owner but never host, mirror, or touch payout settings, secrets
   or mirror process control.

## 2. Device kinds and capabilities

| Kind | Capabilities |
| --- | --- |
| `desktop_node` | `browse,comment,submit_issue,submit_pr,host_repo,mirror_repo,publish_repo,owner_sign` |
| `mobile_node` | `browse,comment,submit_issue,submit_pr,publish_repo,owner_sign` |
| client (web) | `browse,comment,submit_issue,submit_pr` |

`contribution_key_proof` is appended once the device proof verifies. A
`mobile_node` row never satisfies a hosting check (`host_repo`, `mirror_repo`,
mirror process control, node reservation). `_owner_signing_pubkeys(owner)`
unions every enabled, unrevoked `owner_sign` device, so a phone key signs
sync, catalog publications and other owner-signed requests.

## 3. Bind (login)

`POST /api/accounts/login` with `identifier`, `password`, optional `totp`,
`pubkey`, `deviceTs`, `deviceSig`, `deviceKind: "mobile_node"` and
`deviceLabel`.

- `deviceSig` signs `forkmesh-device-bind-v1\n{account}\n{pubkey}\n{deviceTs}`.
- A mobile bind registers the device with `MOBILE_NODE_CAPABILITIES`, never
  becomes the account's primary key and never counts as `desktopCapable`.
  Unknown kinds return `400 invalid_device_kind`.
- The session records the device (`account_sessions.device_bi`). The response
  reports `sessionKind`, `capabilities`, `canHost: false`, `canPublish`,
  `canOwnerSign`, `deviceId` and the account's devices.
- The first bind of a new device raises a "new node added" notification.

## 4. Node registry

| Route | Behaviour |
| --- | --- |
| `GET /api/accounts/devices` | Devices with kind, label, capabilities, enabled, last seen, and `current` |
| `DELETE /api/accounts/devices/{id}` | Re-authenticated (password, TOTP). Revokes the device and every session it opened |
| `POST /api/accounts/devices/check` | Whether a key belongs to the caller's account; used by the desktop to admit same-account phones |
| `POST /api/accounts/devices/revoke-all` | Re-authenticated. Revokes every other device and session, optionally by kind |

Revocation removes the key from owner signing at once, sessions fail with
`401` and `device_revoked`, and a revoked row is permanent: the app rotates its
seed, keeps local repositories and returns to login.

## 5. Audit

`account.device_bind`, `account.device_revoke` and
`account.device_revoke_all` are audited. Catalog records store the signing key
as `maintainer`, so the web can show which node published.

## 6. Destructive actions

Deleting a local repository; deleting, transferring or archiving a published
one; revoking devices; changing password or security settings. These require
Face ID, Touch ID or the device passcode, or an explicit confirmation when none
is enrolled.

## 7. Publish and push

The engine speaks smart HTTP to the app origin only; the relay is a masked
proxy in front of the mirror gateway.

- `GET /{owner}/{repo}/info/refs?service=git-receive-pack`
- `POST /{owner}/{repo}/git-receive-pack`

Writes carry `X-ForkMesh-Pubkey`, `X-ForkMesh-Ts` and `X-ForkMesh-Sig` over
`forkmesh-git-push-v1\n{owner}\n{repo}\n{method}\n{ts}\n{sha256hex(body)}`. The
relay accepts the owner's primary key, an enabled `owner_sign` device with
`publish_repo`, or a key with organization write permission, then forwards
under a per-request masked-proxy capability for
`/v1/repositories/{owner}/{repo}/git-receive-pack`.

- Bodies up to 8 MB are verified in full. Larger packs send
  `X-ForkMesh-Body-Sha256`; the relay streams the body and binds the capability
  to that digest. Otherwise `413 push_too_large`.
- The gateway runs `git receive-pack --stateless-rpc` with deletes and
  non-fast-forwards denied, `fsckObjects` on, hooks disabled and a bounded
  input size. It creates a bare repository on first push only for owners on
  its `writableOwners` allowlist, and removing an owner revokes push and read.
- With no healthy push endpoint, the first push goes to
  `HOSTED_GIT_WRITE_NODE`, else `503 push_unavailable`.
- After a first push the phone publishes the signed catalog record itself. A
  publish from a non-authoritative device is accepted only as a subordinate
  mirror (`202`, `mirror: true`); replays get `409 stale_update`.
- Stock Git can push with a Basic credential the website issues from a
  signed-in session (see [hosted repositories](../hosted-repositories.md)).

Client guardrails: no force, no delete, no hooks, foreground only, resource
caps and cancellation.

## 8. Platform notes

No background modes, no runtime code download, no hook execution. Store
listings say "publish", never "host" or "server".

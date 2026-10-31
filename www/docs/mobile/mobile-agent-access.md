# Mobile access to owner-encrypted agent sessions

Agent sessions are end-to-end encrypted to the owner's devices. The phone is an
owner device with its own key, so it can read sessions, queue prompts for a
mirror, and control a desktop live on the LAN or through the relay.

## The owner-device model

Each owner device holds a hybrid recipient key and registers only the public
halves at `POST /api/security/owner-keys`. The desktop seals every session
snapshot and transcript tail into one `forkmesh.owner-sealed` envelope with one
wrap entry per enrolled device. The relay stores ciphertext and holds no
private key.

```json
{
  "kind": "forkmesh.owner-sealed", "v": 1,
  "alg": "x25519+mlkem768/aes256gcm",
  "nonce": "…", "tag": "…", "body": "…",
  "recipients": [{ "kid": "…", "x25519": "…", "mlkem768": "…", "nonce": "…", "tag": "…", "key": "…" }]
}
```

- One random AES-256-GCM content key encrypts the payload; it is wrapped per
  device under X25519 + ML-KEM-768.
- 1 to 6 recipients (`MAX_OWNER_ENVELOPE_RECIPIENTS`), no duplicates; the
  sealing device's entry comes first.
- A recipient id is `base64url(SHA-256(x25519Pub || mlkemPub))`.
- Wrap key: `HKDF-SHA256(ssX25519 || ssMlKem, salt "forkmesh-mirror-kem-v1",
  info ephemeralX25519Pub || mlkemCiphertext)`.
- Each device opens its own entry by key id.

**Authorization is membership.** The repository privacy policy names the
desktop vault key (`ownerKeyId`). The relay requires that key to be among the
recipients, so the desktop can seal to itself plus every companion device; an
envelope without it is rejected with `wrong_owner_key`.

The desktop refreshes companion bundles every ten minutes and re-seals when the
set changes. Revocation is not retroactive: stored envelopes stay readable by
the devices they were sealed to.

| Runtime | Role | Source |
| --- | --- | --- |
| Relay | Validates framing and bounds, never decrypts | `app/src/security_controls.py` |
| Desktop | Generates the vault key, seals to vault plus companions | `desktop/src/MirrorCrypto.cpp` |
| iOS, Android | Enroll the device key, open their own entries | `FMCrypto/OwnerCrypto.swift`, `security/OwnerCrypto.kt` |

When sessions exist but are sealed to other devices, the app says how many
and how to enroll this one, instead of showing an empty list. Cross-runtime
tests seal with the desktop binary and open in Swift and Kotlin.

## Sending a prompt to a mirror

A prompt typed on the phone uses the relay-readable agent queue, as the web
does. `GET /api/repo/{o}/{r}/agent-sessions` (or the organization
`agent-bots` route) lists nodes that can take the job; an online mirror is
suggested first. Failed or stopped runs can be retried: a claimed session
continues on its branch, an unstarted one is re-queued. Prompts can carry
photos, downscaled to 2048 px with metadata stripped, within a 10 MB total.
The Cloud section lists runs that mirrors are working; the relay's
`cloud-agents` event topic refreshes it. A queued session shows its chain:
prompt, mirror, branch, pull request, merge.
Merging a mirror agent's branch straight onto its base is owner-only: the
node's owner, or an owner of the organization. Anyone else gets `forbidden`
and opens a pull request instead.

## Controlling a desktop on the LAN or through the relay

Live control opens the desktop's local-control protocol directly over the mesh
TCP port on the same network, or inside `{"type":"control"}` chunks forwarded
by the account event relay when away. The Ed25519-authenticated handshake
derives per-direction AES-256-GCM keys, so the relay sees only routing
metadata.

Discovery: `GET /api/accounts/desktops` returns the caller's enabled desktop
keys and labels. The phone caches the list for ten minutes, tries at most four
candidates, and checks the answering desktop's public key. No LAN pairing is
needed: this is automatic same-account desktop discovery.

Admission: the desktop checks the phone key against its account's enabled
`owner_sign` devices. A same-account phone is accepted without a code and
stored as an account-held, desktop-local pairing (`"approval": "account"` in
`data/local_control/paired-devices.json`). The desktop re-checks the account on
connect and every ten minutes, and closes the channel (`revoked`) when the
phone is gone. Through the relay, a phone is admitted only if the account
confirmed it within 24 hours (`account_check_failed` otherwise). A phone from
another account must pair on the LAN with a six-digit code.

Channel recovery: idle relayed channels are pinged every 30 seconds; a desktop
that lost a channel answers `channel_gone` so the phone redials; the desktop
closes channels silent for 15 minutes, holds at most eight, and limits
half-open relayed hellos to 16 with a 30-second expiry.

| Action | Methods | Boundary |
| --- | --- | --- |
| List, start, read, prompt, stop, continue | `agents.*` | Authenticated channel |
| Attach photos | `agents.image.begin`, `agents.image.chunk` | Desktop checks size, type and SHA-256 |
| Answer tool requests | `agentops.approvals`, `agentops.prepare`, `agentops.answer` | One-use desktop confirmation plus phone device authentication |
| Review changes and images | `agentops.diff`, `agentops.file` | Read-only; credentials redacted; images up to 2 MiB |
| Merge and clean up | `agentops.prepare`, `agentops.merge` | Agent must be idle; unstaged edits in the desktop checkout that clash with the merge can be set aside first (**Set aside and merge**, backed up on a `wip/` branch) |
| Delete agent, worktree and branch | `agentops.prepare`, `agentops.delete` | Explicit confirmation; agent must be idle |

An older desktop answers `unsupported` and the phone asks for an update.

## Limitations

- Away-from-home control needs the desktop online, signed in and connected to
  the relay.
- A relay reconnect closes the live channel; the next action opens a new one.
- File transfer is LAN-only.

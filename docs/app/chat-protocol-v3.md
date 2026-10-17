# Chat protocol v3: signed, post-quantum, forward-secret chat

Status: adopted 2026-10-16. Replaces RoomSeal v2 (`type:"sealed", v:2`,
`type:"e2ee-key", v:2`) on every client (iOS `FMCrypto/RoomSeal.swift`,
Android `crypto/RoomSeal.kt`, desktop `desktop/src/RoomSeal.cpp`, web
`app/public/chat-seal.js`). There is no v2 fallback: a client that speaks v3
ignores v2 frames, so nothing can be downgraded. Every client and the server
are deployed together.

Reference implementation: `tools/chat_v3_reference.py`. Wire vectors that
every client must open: `conformance/chat/v3/vectors.json` (checked by
`app/tests/test_chat_v3_vectors.py` and each client's own vector test).

## Why

The 2026-10-16 audit of v2 found (details in
[chat-private-rooms.md](chat-private-rooms.md)):

1. The server could read future DMs: it holds the outer room passphrase,
   device keys were bound to nothing, every client added any key it saw and
   re-sealed to it, and nobody was told.
2. No forward secrecy or post-compromise security: one static X25519 key per
   install wrapped every message.
3. Senders could be forged: `senderId` was a plaintext field, and a recipient
   could swap the body of a frame because the wrap bound only the body nonce.
4. No replay protection: the relay could replay or reorder frames.
5. Relay `@peer` DMs were sealed to the whole room.
6. Push payloads named the sender to Apple even with previews hidden.
7. Mesh links accepted any self-claimed identity under an app-wide key.

v3 fixes all seven. The user asked for chat at least as strong as Signal; the
comparison is in "Compared with Signal" below.

## Encodings

- `b64u`: base64url without padding. `b64`: standard base64 with padding.
- Public keys, key ids, signatures and hashes inside signed strings are `b64u`;
  nonces, tags, ciphertexts and wrapped keys are `b64` (as in v2).
- Integers in signed strings are base-10 ASCII with no sign or leading zeros.
- All strings are UTF-8. `\n` is byte 0x0A.
- `H(x)` is SHA-256. `id16(x)` is the first 16 characters of `b64u(H(x))`.
- AES-GCM is AES-256-GCM with a 12-byte random nonce and a 16-byte tag.
- HKDF is HKDF-SHA256. Ed25519 is RFC 8032 (pure). X25519 is RFC 7748; an
  all-zero shared secret is an error. ML-KEM is ML-KEM-768 (FIPS 203).

## Device keys

Each installation keeps, in the platform secure store (iOS keychain in the
shared group, Android Keystore-wrapped storage, desktop secure store, web
IndexedDB with a non-extractable wrapping key where available):

| Key | Store key | Lifetime |
| --- | --- | --- |
| Identity key `IK`: Ed25519 | `chat/e2ee/ed25519IdentityV3` (32-byte seed, `b64`) | Life of the install |
| Signed prekeys `SPK`: X25519 + ML-KEM-768 | `chat/e2ee/prekeysV3` (JSON list) | Rotated every 24 h, deleted at expiry |

- `kid = id16(IK.pub)`: the device's public key id, stable for the install.
- A prekey is `{id, x, pq, ts, exp}` with `x` the X25519 public key (32 bytes),
  `pq` the ML-KEM-768 encapsulation key (1184 bytes), `id = id16(x || pq)`,
  `ts` its creation time in ms and `exp = ts + 8 days`. The stored private
  half is the X25519 private key and the 64-byte ML-KEM seed `d || z`.
- A new prekey is made when the newest one is 24 h old (checked at start and
  before each seal). Private halves are wiped once `now > exp`; at most 10 are
  kept. 8 days covers the relay's 7-day pending copies plus a day of skew, so
  forward secrecy has an 8-day window: once a prekey's private half is wiped,
  nothing captured under it can be opened, even with the device in hand.

## Bundles (`e2ee-key` v3)

A device advertises one bundle per room, signed by its identity key:

```json
{"type":"e2ee-key","v":3,"kid":"<16>","idk":"<b64u 32>","acct":"<account>","node":"<sender id>",
 "spk":{"id":"<16>","x":"<b64u 32>","pq":"<b64u 1184>","ts":0,"exp":0},
 "sig":"<b64u 64>"}
```

- `acct` is the signed-in account name, lowercased, the same on every device
  of that account (a guest uses its node id). It decides trust.
- `node` is the value this client puts in `senderId` of its plaintext frames
  in that room (the relay self id, which differs per device). It is what the
  forgery check compares.

Both are needed: with only a per-device id, a device the server injected
would always look like the first device of a new account and be approved.

`sig` is Ed25519 by `IK` over:

```
forkmesh-chat-e2ee-bundle-v3\n<room>\n<kid>\n<idk>\n<acct>\n<node>\n<spk.id>\n<spk.x>\n<spk.pq>\n<spk.ts>\n<spk.exp>
```

A receiver accepts a bundle only if `kid == id16(idk)`, the signature
verifies, `spk.id == id16(x || pq)`, `0 < exp - ts <= 9 days`,
`ts <= now + 1 day`, `exp > now`, and `acct` and `node` are each 1 to 128
printable characters without `\n`. In a DM room `acct` must also be one of the
conversation's two participants (the lowercased account names of the DM
record), otherwise the bundle is dropped. For a `kid` it already knows,
`idk`, `acct` and `node` must match the stored record (otherwise the bundle is
dropped); a newer `spk.ts` replaces the
stored prekey. A device re-advertises its bundle when its prekey rotates and
on every (re)connect, as v2 did.

## Trust: approved and pending devices

Each room has a trust policy and a trust scope (where approvals are stored):

| Room | Policy | Trust scope |
| --- | --- | --- |
| Server DM `direct:<id>` | strict | `direct:<id>` (survives key-version bumps) |
| Relay channels, office rooms, `#mesh` | open | the room name |

- **open**: every valid bundle is approved.
- **strict**: the first device seen for an `acct` is approved (trust on first
  use). Any further device for an `acct` that already has an approved device,
  including the user's own account, is **pending**. Pending devices get
  nothing sealed to them and their frames are held, not shown, until the user
  approves them. The user can also block a device. Signal by default keeps
  sending after a safety-number change; v3 asks first.
- Records are stored per trust scope at `chat/e2ee/trustV3/<b64u(scope)>`:
  `{kid, idk, acct, node, spk, state: approved|pending|blocked, firstSeen,
  verified}`.

Clients show a system line in the conversation for every new pending device
("A new device for Alex wants to join this chat. Check the safety number,
then approve or block.") and a banner with Approve and Block.

## Sealed frames v3

```json
{"type":"sealed","v":3,"kid":"<sender>","idk":"<b64u>","ctr":1,"ts":0,
 "nonce":"<b64>","tag":"<b64>","body":"<b64>",
 "keys":[{"kid":"<recipient>","spk":"<prekey id>","eph":"<b64u 32>",
          "ct":"<b64 1088>","nonce":"<b64>","tag":"<b64>","key":"<b64 32>"}],
 "sig":"<b64u 64>"}
```

Sealing `plain` (a JSON object) in `room` by sender `S`:

1. `ctr` = this device's next counter for `room` (starts at 1, persisted at
   `chat/e2ee/ctrV3/<b64u(room)>` before the frame is sent); `ts` = now in ms.
2. Content key `CK` = 32 random bytes. `body, nonce, tag` = AES-GCM(`CK`,
   `UTF8(JSON(plain))`, aad =
   `forkmesh-chat-e2ee-content-v3\n<room>\n<S>\n<ctr>\n<ts>`).
3. Recipients = approved devices with an unexpired prekey, plus `S` itself
   (its own newest prekey), at most 64, sorted by `kid` (ASCII ascending). In
   an open room with more devices, the sender keeps itself and the 63 most
   recently seen.
4. For each recipient `R` with prekey `P`:
   - `eph` = a fresh X25519 key pair; `ss1` = X25519(`eph.priv`, `P.x`).
   - `ct, ss2` = ML-KEM-768.Encaps(`P.pq`).
   - `KEK` = HKDF(ikm = `ss1 || ss2`,
     salt = `H("ForkMesh chat e2ee salt v3:" + room)`,
     info = `forkmesh-chat-e2ee-kek-v3\n<room>\n<S>\n<R>\n<P.id>\n<b64u(eph.pub)>\n<b64u(H(ct))>`,
     length 32).
   - `key, nonce, tag` = AES-GCM(`KEK`, `CK`, aad =
     `forkmesh-chat-e2ee-wrap-v3\n<room>\n<S>\n<R>\n<body nonce b64>\n<body tag b64>`).
   - `eph.priv`, `ss1`, `ss2` and `KEK` are wiped.
5. `sig` = Ed25519 by `IK` over:

```
forkmesh-chat-e2ee-sealed-v3\n<room>\n<S>\n<idk>\n<ctr>\n<ts>\n<nonce>\n<tag>\n<b64u(H(body bytes))>
```

followed, for each entry of `keys` in frame order, by
`\n<kid>|<spk>|<eph>|<b64u(H(ct bytes))>|<nonce>|<tag>|<key>`.

Opening a frame on device `D` returns one of: `opened(plain, senderKid,
senderAcct)`, `held(kid)`, `notForMe`, `replay`, `invalid`.

1. Shape: `v == 3`, `kid == id16(idk)`, 1 to 64 `keys` sorted by `kid` with
   no duplicates, field lengths as above. Otherwise `invalid`.
2. Sender: the trust record for `kid` must exist with the same `idk`. Unknown,
   pending or blocked senders give `held(kid)` (blocked frames are then
   dropped by the client); the client keeps up to 64 held frames per room,
   persisted with the trust records, and opens them again on approval or when
   the sender's bundle arrives.
3. `sig` verifies under `idk`, otherwise `invalid`.
4. Replay: per (room, sender kid) the device keeps the highest counter `M`
   and the set of counters seen in `(M - 1024, M]`, persisted at
   `chat/e2ee/seenV3/<b64u(room)>`. `ctr <= M - 1024` or already seen gives
   `replay`. `ts` must be within `[now - 8 days, now + 1 day]`, otherwise
   `invalid`.
5. The entry with `kid == D`; none gives `notForMe`. Its `spk` must be a
   prekey whose private half is still kept, otherwise `notForMe`.
6. Recompute `KEK` (ML-KEM.Decaps, X25519), unwrap `CK`, decrypt `body`. Any
   failure gives `invalid`.
7. The plaintext `senderId`, when present, must equal the sender record's
   `node`, otherwise `invalid` (a forgery). A frame without `senderId` is
   treated as sent by that `node`. A bot or agent line posted by a device keeps
   the device's own `senderId` and names the bot in a separate `bot` field.
8. Only now record `ctr` as seen and return `opened`.

`ctr` and the seen set are updated only for frames that verify, so a forged
frame cannot burn a counter. Edits and deletes are accepted only from the
device that sent the message: the editing frame's authenticated `node` must
equal the original's. A reaction's `reactorId` must equal its frame's `node`.
Every client chose this stricter rule over matching `acct`, so a message cannot
yet be edited from the sender's other device.

Frames the relay sends as its history replay on connect skip only the
already-seen part of step 4; signature, `ts`, trust and `node` checks still
apply. A client that keeps no history of its own (the web) would otherwise show
an empty room after every reload. Clients drop duplicates by message id and
order messages by the signed `ts`, so a replayed frame cannot appear as new.

The outer `RoomCrypto` layer (server passphrase) stays as transport padding;
nothing in v3 relies on it.

## Relay `@peer` DMs

A relay `type:"dm"` frame is sealed only to the devices whose bundle `node` is
the recipient's id, plus the sender's own approved devices in that room. Other
room members can no longer open it.

## Safety numbers

For a strict room, the safety number covers every approved device of both
accounts. With `idks` = the approved identity keys of all accounts in the trust
scope, own included, sorted by raw bytes:

```
h = SHA-512("forkmesh-safety-v3\n" + scope + "\n" || idk1 || idk2 || ...)
repeat 5199 times: h = SHA-512(h || idk1 || idk2 || ...)
```

The first 60 bytes of `h` are split into twelve 5-byte big-endian numbers;
each is taken mod 100000 and written as five digits. The 60 digits are shown
in 12 groups. Both people see the same number. The QR code encodes
`forkmesh-safety:3:<scope>:<b64u(first 32 bytes of h)>`. Scanning the other
phone's code (or comparing the digits) marks every approved device `verified`.
Any change to the approved set changes the number, clears `verified`, and
shows "Your safety number with Alex changed".

## Mesh links v3

LAN TCP and Bluetooth links between devices are authenticated and encrypted
with a fresh key pair per link, so a link has forward secrecy of its own and
cannot be joined by a device claiming someone else's identity.

1. On connect each side sends a length-prefixed plaintext hello:
   `{"t":"fm-link-hello","v":3,"idk":"<b64u>","eph":"<b64u 32>","nonce":"<b64u 16>"}`.
   The dialer (TCP outbound, Bluetooth central) is the initiator `I`, the
   other side the responder `Rs`.
2. `T = H("forkmesh-mesh-link-v3\n" + I.idk + "\n" + I.eph + "\n" + I.nonce + "\n" + Rs.idk + "\n" + Rs.eph + "\n" + Rs.nonce)`.
3. Each side sends a length-prefixed plaintext `{"t":"fm-link-auth","sig":"<b64u>"}`: Ed25519 by its `IK`
   over `forkmesh-mesh-link-auth-v3\n<i|r>\n<b64u(T)>` (`i` from the
   initiator, `r` from the responder). A bad signature, the side's own `idk`,
   or a blocked `kid` closes the link.
4. `ss = X25519(own eph, peer eph)`; `k_i2r = HKDF(ss, salt = T, info =
   "forkmesh-mesh-link-v3 i2r", 32)`, `k_r2i` with `r2i`. Ephemeral keys and
   `ss` are wiped.
5. Every later frame is length-prefixed `ciphertext || tag` from AES-GCM with
   the direction's key, nonce = 4 zero bytes then the 8-byte big-endian frame
   counter (0, 1, ...), aad `forkmesh-mesh-link-v3`, plaintext the JSON frame.
   A frame that fails to open closes the link.
6. The link's peer is `kid = id16(peer idk)`, authenticated.

Frames inside an authenticated link are JSON objects of these types only:

| `type` | Content |
| --- | --- |
| `e2ee-key` | A v3 bundle for the `#mesh` room (`acct` = node id), sent first |
| `sealed` | A v3 frame of the `#mesh` room (open policy); its plaintext is the mesh envelope (`hello`, `chat`, `typing`, `bye`, `wake`) |
| `direct-sealed`, `direct-ack`, `focus` | DM delivery, below |

The desktop's local-control port tells the two protocols apart by the first
frame: `fm-control-hello` starts local control, `fm-link-hello` a mesh link.

Beacons keep the shared discovery key but carry only
`{t, v:3, id: kid, port, ts, reply, control, node}` (`node`, the desktop's
Ed25519 node id for local control, only with `control: true`). `reply` and
`control` may be left out when false; receivers treat a missing one as false.
The mesh `hello` envelope also carries `port`, so a dropped link can be
redialled.

`#mesh` is multi-hop. A device forwards a new `#mesh` bundle to its other
links, forwards a sealed `#mesh` frame it cannot open to the links whose device
is among the frame's recipients (and floods frames it opened, as v2 did), and
re-seals its own history to a device it learns about through another device.
Forwarded frames keep their original signatures and counters, so every hop
still checks them. Names, account
and platform travel in the mesh `hello` inside the encrypted link. The `#mesh`
room's messages are v3 sealed frames (open policy) carried over links; a
forwarded frame keeps its original signature.

## Direct delivery over the mesh

A DM frame of any kind (text, file, edit, delete, reaction, receipt, typing,
live taps and ink) goes to a linked device as:

```json
{"type":"direct-sealed","channel":"direct:<id>","frame":<v3 sealed frame>}
```

The frame is sealed with the DM room's seal to that device's `kid` (and the
sender). The receiver requires the link's authenticated `kid` to equal
`frame.kid`, opens it with the DM seal (strict trust, counters, `acct` check)
and handles the plaintext exactly as a relay frame of the same `type`. It
answers `{"type":"direct-ack","channel":...,"id":"<message id>"}` once the
message is accepted or already known.

Files go over a TCP link up to the DM file limit (1 MiB) and over Bluetooth
up to 256 KiB.

### Focus and direct-only

Each side sends `{"type":"focus","channel":"direct:<id>","on":true|false}`
over its links whenever its own focus on that conversation changes, and its
current focus right after a link authenticates. A device has focus when the
app is in the foreground and that conversation's transcript is on screen.

When sending a DM frame:

- If this device has focus, and a linked, approved device of the other person
  has reported focus on the same conversation, the frame goes **only** over
  the mesh. If no `direct-ack` comes back within 3 s, or the link drops, the
  client sends the normal relay copy (sealed again to every approved device;
  the message id is the same, so receivers drop the duplicate).
- Otherwise the frame goes over every linked approved device of the
  conversation **and** through the relay as before.

This is the rule the user asked for: while both people are looking at the
chat on linked phones nothing reaches the server; as soon as either one
leaves, the relay copy keeps every device in step. A message delivered direct
only is not on the users' other devices and raises no push.

Clients show the route under the conversation name: "Direct · Wi-Fi" or
"Direct · Bluetooth" while a linked approved device is present, "Direct only"
while the focus rule holds.

## Push notifications

APNs payloads for chat and DMs no longer carry the sender or channel name:
the visible title is "New message" and there is no `sender` field. The iOS
notification service extension opens the v3 frame (the prekeys, identity and
trust records are in the shared keychain group) and fills the sender and text
according to the preview setting. A frame from a pending or unknown device
shows "New message".

## On the device

- Chat history and held frames are excluded from device backups (iOS
  `isExcludedFromBackup`, Android backup rules). Files keep their current
  protection classes.
- iOS and Android cover the app with a privacy screen in the app switcher
  (setting "Hide chats in the app switcher", on by default).
- An optional "Lock chats with Face ID / fingerprint" setting asks for the
  device owner before the Chat tab opens after the app was in the background.

## Compared with Signal

| | Signal | ForkMesh v3 |
| --- | --- | --- |
| Post-quantum key agreement | PQXDH at session start | ML-KEM-768 hybrid on every message |
| Forward secrecy | Per message (Double Ratchet) | Per prekey: 8-day window, then wiped |
| Post-compromise recovery | Next ratchet step | Next prekey rotation (24 h) |
| Sender authentication | Implicit (session MAC) | Ed25519 signature on every frame |
| New device of a contact | Warns, keeps sending | Holds until the user approves |
| Server key injection | Shown as a safety-number change | Held, plus safety-number change |
| Safety numbers / QR | Yes | Yes |
| Replay protection | Ratchet counters | Signed counters with a 1024 window |
| Nearby chats without a server | No | Direct-only over LAN/Bluetooth |
| Sender hidden from the push service | Yes | Yes |
| Metadata at the server | Sealed sender hides sender | Server still knows the two accounts of a DM |

Signal is ahead on per-message forward secrecy (the ratchet) and on hiding the
sender from its own server. v3 is ahead on post-quantum coverage, sender
signatures, new-device approval and server-free nearby chat. A Double Ratchet
on top of v3 bundles, and sealed-sender delivery, are the next steps; v3 keeps
the frame shape so a ratchet header can be added as `v:4`. None of this has had
an outside audit yet; that should come before it is described publicly as
stronger than Signal.

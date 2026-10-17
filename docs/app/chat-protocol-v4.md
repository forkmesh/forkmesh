# Chat protocol v4: a per-message ratchet for direct messages

Status: adopted 2026-10-16. Builds on [chat-protocol-v3.md](chat-protocol-v3.md):
keys, bundles, trust, safety numbers, mesh links, direct delivery and push
are unchanged. v4 replaces only the sealed frame of **strict rooms** (server
DMs, `direct:<id>`, and their `direct-sealed` mesh copies). Channels, Office
rooms, relay `@peer` DMs and `#mesh` keep v3 frames.

Reference implementation: `tools/chat_v4_reference.py`. Wire vectors:
`conformance/chat/v4/vectors.json` (checked by
`app/tests/test_chat_v4_vectors.py` and each client's vector test).

## Why

v3 wraps each message for the recipient's signed prekey. A prekey's private
half lives 8 days, so a stolen device opens every captured message sealed to a
prekey it still holds: up to 8 days of traffic. Signal's Double Ratchet derives
a new key for every message and deletes it once used. v4 adds that ratchet to
DMs, with ML-KEM-768 mixed into every ratchet step, so a device stolen today
opens nothing it had already received, and a device that was compromised
recovers on the next reply (and against a quantum attacker on the next prekey
rotation).

Channels keep v3: the web client keeps no history of its own and reopens a
channel's relay history on every load, which a per-message ratchet forbids.

## Sessions

Every pair of devices in a DM room has a ratchet session, kept per room and
per peer `kid`. A device seals to each recipient with its session to that
device and never to itself: a v4 frame has no entry for its sender, and every
client keeps its own sent messages in its own history.

Primitives: `HKDF` is HKDF-SHA256, `HMAC` is HMAC-SHA256, `KDF64(ikm, salt,
info)` is 64 bytes of HKDF split into two 32-byte halves.

- `KDF_INIT = KDF64(ss1 || ss2, H("ForkMesh chat e2ee salt v4:" + room),
  "forkmesh-chat-e2ee-init-v4\n<room>\n<S>\n<R>\n<spk>\n<sid>")` gives
  `(RK, CK)`.
- `KDF_RK(RK, dh, ss) = KDF64(dh || ss, RK, "forkmesh-chat-e2ee-ratchet-v4")`
  gives `(RK', CK)`.
- `KDF_CK(CK)`: message key `MK = HMAC(CK, 0x01)`, next chain key
  `HMAC(CK, 0x02)`.

### Starting a session (sender `S`, recipient `R` with prekey `P`)

`eph` = a fresh X25519 pair, `ss1 = X25519(eph, P.x)`, `ct, ss2 =
ML-KEM.Encaps(P.pq)`, `sid = id16(eph.pub || ct)`, `RK, CKs = KDF_INIT(...)`.
`S` keeps `{sid, RK, DHs = eph, DHr = none, CKs, Ns = 0, PN = 0, sq = {spk:
P.id, ct}, CKr = none, Nr = 0}`.

`R`, on the first entry of an unknown `sid` with `sid == id16(dh || ct)` and
`pn == 0`, decapsulates with its kept prekey `spk` and derives the same
`RK, CK`; its session is `{sid, RK, DHs = none, DHr = dh, CKr = CK, Nr = 0,
CKs = none, Ns = 0, PN = 0}`. An unknown `sid` that is not such an entry, or a
prekey no longer kept, gives `noSession`.

### Sending

With the current session for `R` (none: start one):

- If `CKs` is none (the peer's ratchet key changed since the last send):
  `DHs` = a fresh X25519 pair, `ct, ss = ML-KEM.Encaps(R's newest prekey)`,
  `RK, CKs = KDF_RK(RK, X25519(DHs, DHr), ss)`, `sq = {spk, ct}`.
- `MK, CKs = KDF_CK(CKs)`; the entry header is `sid`, `spk = sq.spk`,
  `ct = sq.ct`, `dh = DHs.pub`, `pn = PN`, `n = Ns`; then `Ns += 1`.

The session is stored before the frame leaves the device (as the counter is).

### Receiving

1. A stored skipped key for `(dh, n)`: use it and delete it.
2. `dh == DHr`: derive forward to `n` (below).
3. Otherwise (a new ratchet key): `DHs` none gives `noSession`; skip the
   current receiving chain to `pn`; `ss = ML-KEM.Decaps(own prekey spk, ct)`;
   `RK, CKr = KDF_RK(RK, X25519(DHs, dh), ss)`; `DHr = dh`, `Nr = 0`,
   `PN = Ns`, `Ns = 0`, `CKs = none`, `sq = none`; derive forward to `n`.

"Derive forward to `m`": if `m < Nr` the key is gone, `replay`; if
`m - Nr > 256`, `noSession`; otherwise store each skipped `MK` as
`{dh: DHr, n, mk, ts}` and step `CKr`. Then `MK, CKr = KDF_CK(CKr)`,
`Nr += 1`.

The work is done on a copy of the session and kept only if the frame opens.
A session that opened a frame becomes the peer's current session; this is how
two devices that started sessions at the same time settle on one.

Limits: 4 sessions per peer (least recently used dropped, never the current
one), 512 skipped keys per session, skipped keys wiped after 8 days.

## Frame

```json
{"type":"sealed","v":4,"kid":"<sender>","idk":"<b64u>","ctr":1,"ts":0,
 "nonce":"<b64>","tag":"<b64>","body":"<b64>",
 "keys":[{"kid":"<recipient>","sid":"<16>","spk":"<16>","ct":"<b64 1088>",
          "dh":"<b64u 32>","pn":0,"n":0,"nonce":"<b64>","tag":"<b64>","key":"<b64 32>"}],
 "sig":"<b64u 64>"}
```

- `body` = AES-GCM(`CK`, plaintext, aad
  `forkmesh-chat-e2ee-content-v4\n<room>\n<S>\n<ctr>\n<ts>`) with a random
  content key `CK`, as in v3.
- Each entry wraps `CK` with its `MK`: AES-GCM, aad
  `forkmesh-chat-e2ee-wrap-v4\n<room>\n<S>\n<R>\n<sid>\n<spk>\n<b64u(H(ct))>\n<dh>\n<pn>\n<n>\n<body nonce>\n<body tag>`.
- `sig` = Ed25519 by `IK` over
  `forkmesh-chat-e2ee-sealed-v4\n<room>\n<S>\n<idk>\n<ctr>\n<ts>\n<nonce>\n<tag>\n<b64u(H(body))>`
  followed, per entry in frame order, by
  `\n<kid>|<sid>|<spk>|<b64u(H(ct))>|<dh>|<pn>|<n>|<nonce>|<tag>|<key>`.
- `keys` holds 0 to 64 entries sorted by `kid`, no duplicates, none for the
  sender. A frame with no entries is legal: it is what a device sends before
  any other device is approved, and the outbox re-seals it later.
- Recipients are the approved devices of the trust scope other than the
  sender, with an unexpired prekey, filtered by `only` as in v3.

Opening on device `D` returns `opened(plain, senderKid, senderAcct)`,
`held(kid)`, `own`, `notForMe`, `noSession(kid)`, `replay` or `invalid`:

1. Shape as above (otherwise `invalid`). `kid == D` gives `own`.
2. Trust, signature, counter window and `ts` exactly as v3 steps 2 to 4.
   There is no history-replay exception: a frame already seen is `replay`,
   because its key is gone.
3. No entry for `D`: `notForMe`.
4. Ratchet receive and unwrap of `CK`. A missing session or prekey, too many
   skipped keys or a failed unwrap give `noSession(kid)`; a key already used
   gives `replay`.
5. Body and the `senderId` check as v3; failures are `invalid`.
6. Commit the session copy and the counter; return `opened`.

`open` takes a `commit` flag. With `commit = false` (the iOS notification
service extension) nothing is stored, so the app opens the same frame again
later.

## Recovery: `e2ee-reset`

A frame that gives `noSession` is not acknowledged, so the relay replays it on
every reconnect. A client therefore skips recovery when it has already opened a
later frame from that device (a higher `ctr` than this one): the session works
again and the frame is lost. Otherwise, on `noSession(kid)` a client, at most
once per room and peer every 60 s:

1. deletes its sessions with that `kid` in the room;
2. sends a persisted, non-notifying sealed frame with plaintext
   `{"type":"e2ee-reset","senderId":<node>}` sealed only to that `kid` (it
   starts a new session);
3. sends one persisted key advertisement, as v3 does for `notForMe`.

A client that opens an `e2ee-reset` shows nothing, and re-seals every outbox
entry to that device alone (same message id, `"resend": true`). It sends no
`forkmesh-superseded` for these: the original copy still serves the other
devices. Receivers drop ids they already show.

## Client rules

- Outbox re-seal (chat-private-rooms.md, "Re-sealing"): an entry is complete
  when it is wrapped for every approved device **other than this one**.
- Own frames coming back from the relay (`own`) are ignored; they are already
  in the device's history and are never acknowledged. A client that keeps DM
  history only in memory (the web, Android, and the desktop's outbox) no
  longer gets its own earlier messages back from the relay after a restart;
  under v3 it reopened them through its self entry.
- Stored per room at `chat/e2ee/ratchetV4/<b64u(room)>` (each platform's
  secure store, beside `ctrV3`/`seenV3`), canonical JSON:
  `{"v":4,"peers":{"<kid>":{"current":"<sid>","sessions":[{"sid","rk","dhs":{"priv","pub"}|null,"dhr"|null,"cks"|null,"ns","pn","sq":{"spk","ct"}|null,"ckr"|null,"nr","skipped":[{"dh","n","mk","ts"}],"created","used"}]}}}`
  with secrets (`rk`, `cks`, `ckr`, `mk`, `dhs.priv`, `ct`) in `b64` and
  public keys (`dhs.pub`, `dhr`, `dh`) in `b64u`. The vectors ship states in
  this format, so every client can import them.
- Web: every tab of a browser is the same device. Seal and open of a v4 frame
  run under one Web Lock (`navigator.locks`, name `forkmesh-chat-ratchet`) and
  read the ratchet and seen state fresh from storage inside it. A tab that
  opens a frame stores the result for 10 minutes (at most 256, encrypted like
  the rest of the state, keyed by SHA-256 of the frame JSON) so other tabs that
  get the same frame show it instead of `replay`. The same cache lets another
  tab show a DM this tab sent (`own` with its plaintext), still never
  acknowledged. Web store keys also carry the relay host, as `ctrV3` does.
- iOS: the notification service extension opens with `commit = false`; the
  ratchet state lives in the shared keychain group like the prekeys.

## Compared with v3 and Signal

| | v3 | v4 (DMs) | Signal |
| --- | --- | --- | --- |
| Forward secrecy | Per prekey, 8 days | Per message | Per message |
| Messages before the first reply | Per prekey, 8 days | Per prekey, 8 days (the first chain derives from it) | Signed prekey plus a one-time prekey |
| Post-compromise recovery | Next prekey (24 h) | Next reply; against a quantum attacker the next prekey rotation | Next reply (SPQR adds ML-KEM) |
| Post-quantum | ML-KEM on every message | ML-KEM at session start and on every ratchet step | PQXDH plus SPQR |

Signal also uses one-time prekeys, which v4 does not: a first message that the
other device has not yet answered still depends on the signed prekey for up to
8 days, as in v3.

# Group call end-to-end encryption (call e2ee v2)

Status: v1 designed and built 2026-10-21 (commit fc5bdd5210); v2 built the
same day on top of it. Live in group calls on the newer call engine: the
browser's [fmm-browser.md](../app/fmm-browser.md) (`fmm-e2ee.js`) and the
iPhone's native group call (`NativeGroupCall.swift`, `FmmVerifyViews.swift`)
run the keys and the verification round. Idea 5 of "Beyond
FaceTime" in [fm-media.md](fm-media.md). Media keys come from a hybrid
X25519 + ML-KEM-768 exchange on every epoch, with a new epoch on every join
and leave. A short verification code is made safe by a commit-then-reveal
round, and every frame is authenticated to its sender by signed digest
batches. The servers carry only ciphertext and headers.

| Part | Code |
| --- | --- |
| Go (call server tests, the FM Media engine core, the phones through gomobile) | `server/rtc/e2ee/`: `wire.go` (presences, key messages), `kem.go` (wraps, frame keys), `session.go`, `sas.go` and `verify.go` (verification round), `auth.go` and `frame.go` (frames and media authentication) |
| Browser | `app/public/chat-call-e2ee.js`, with ML-KEM-768 from `app/public/chat-mlkem.js` |
| Frame encryption in the browser (unchanged, takes the 32-byte keys) | `app/public/chat-group-call-crypto.js` |
| Tests | `server/rtc/e2ee/*_test.go`, `app/tests/call_e2ee.test.mjs`, `app/tests/test_call_e2ee.py` |
| Vectors | `server/rtc/e2ee/testdata/{go-vectors,js-vectors,fips203-mlkem768}.json` |

What v1 replaced: today ([chat-group-calls.md](../app/chat-group-calls.md),
"Frame encryption") each sender draws a random AES-128 key and sends the raw
key to the channel in a `group-call-key` frame. That frame is sealed by chat
protocol v3 ([chat-protocol-v3.md](../app/chat-protocol-v3.md)), so the
relay cannot read it. But the key goes to every device of the channel,
including members who are not in the call. It is wrapped under prekeys that
live up to 8 days, nothing ties it to the call, and nobody can check who
holds it.

## What v2 changes

v1's own review left two gaps that kept it short of "better than FaceTime".
v2 closes both and keeps everything else.

| | v1 | v2 |
| --- | --- | --- |
| Verification code | 30 digits, because a malicious server can grind keys offline against a short code | 4 emoji (24 bits) or 6 digits, made safe by a commit-then-reveal round: an attacker gets one guess per round, never an offline search. A QR code carries the full 256-bit value. |
| Media from a participant | Any participant colluding with the SFU can forge another participant's media, because frame keys are shared | Every frame is covered by a signed batch of frame digests under the sender's own per-epoch Ed25519 key. A forged frame is caught and the stream is dropped. |
| Key message | sender secret wraps | plus the sender's media auth public key `ak` |
| Version | `v:1`, labels `-v1` | `v:2`, every label `-v2`. A v1 message is `invalid`. |

**No downgrade.** v1 was never wired into a live call, so no v1 client
exists to stay compatible with. v2 parsers accept only `v:2`, and every
signature and KDF label ends `-v2`, so a v1 signature can never verify as v2.
A v2 frame key cannot be installed without a media auth key (Go
`FrameSender.SetKey`/`FrameReceiver.SetKey` and JS `createMediaVerifier.setKey`
refuse it), and Go accepts only 32-byte AES keys. The frame byte layout is
unchanged.

## Properties

| Property | How |
| --- | --- |
| No key material on the SFU | Presences, key messages, commits and reveals travel in the chat channel's sealed frames, never over the call server's socket. The SFU forwards AES-GCM frames and signed digest batches. |
| Post-quantum confidentiality | Every sender key is wrapped under X25519 and ML-KEM-768 together; breaking one of the two is not enough. |
| Forward secrecy | Presence key pairs are fresh per epoch and deleted 30 s after the next one. Epoch secrets are wiped once the frame key is derived. Frame keys are dropped after three newer ones. Media auth keys and verification nonces are per epoch and per round. |
| Post-compromise security | A participant's next presence rotation draws new key pairs from fresh randomness, and every sender then draws a new secret wrapped to them. |
| Only the people in the call | Keys are wrapped to each participant's own presence key. A leaver is left out of the next epoch; a joiner never gets earlier secrets. |
| Authenticated senders | Presences, key messages and commits are signed by the chat v3 device identity key; a peer id belongs to the first identity heard for it. |
| Authenticated media | Each frame's digest is signed by its sender's per-epoch media auth key. The key message, signed by the sender's identity, carries that key. A participant who holds the shared frame key still cannot make a frame pass as someone else's. |
| Verification | A 4-emoji or 6-digit code per round over every participant's identity, account, peer id and fresh nonce plus the call id. A commit-then-reveal round gives an attacker one guess per round. The QR form carries the whole 256-bit transcript. |

## Design decisions

**Identity: the chat v3 device identity key.** Every client (web, iOS,
Android, desktop) already has an Ed25519 identity key `IK` per installation
(`chat/e2ee/ed25519IdentityV3`), with `kid = id16(IK.pub)`. The signed v3
bundle binds it to the account, and it is kept in the platform secure store.
In the browser its seed sits in `localStorage`, wrapped by a non-extractable
IndexedDB key. A separate per-call key would add a second key to protect and
a second thing to verify. Call signatures use their own domain prefix,
`forkmesh-call-e2ee-`. Chat v3 signs only strings starting
`forkmesh-chat-e2ee-` or `forkmesh-mesh-link-auth-v3`. So a call signature
can never pass as a chat signature, nor the reverse. The signer refuses any
other prefix (`signerFromSeed`; the seal store's call signer must do the same,
see "Integration").

**Pairwise sender-key wraps, not TreeKEM.** Each sender wraps its own
32-byte epoch secret separately to each receiver's presence key.

- Cost at our size (16 people at most) is small: a rekey is 15 X25519 and 15
  ML-KEM encapsulations, about 60 ms and 24 KB in the browser, and a
  receiver does one decapsulation (11 ms).
- It needs no ordering. TreeKEM (MLS) needs one agreed sequence of commits,
  and the chat relay does not order frames. Two people joining at once would
  fork the tree, and resolving that needs a delivery service we do not have.
  Pairwise wraps let every sender rekey on its own schedule. That also
  matches the per-sender keys the SFrame-style frames already use.
- TreeKEM's log-size updates only pay off in large groups, and its
  post-compromise healing needs every member to update its path, the same
  round of work as here.
- Each wrap stands alone and can be checked in isolation (the cross-language
  vectors do exactly that).

**AES-256-GCM for frames.** ML-KEM-768 is NIST category 3 (about AES-192).
Leaving frames on AES-128 would make frames the weakest link against
Grover-style search. AES-256-GCM costs a few percent more CPU at call
bitrates. The browser worker already accepts 32-byte keys: `importKey("raw")`
picks AES-256 from the length.

**Frame keys derived per sender.** The frame key is HKDF over the sender's
random epoch secret, with the call id, sender peer id, sender identity key
and epoch in the info. Two senders can never share a key. A participant also
cannot pass another sender's secret off as its own: under its own peer id it
derives a different key.

**X25519 and Ed25519 in the browser come from WebCrypto, with no JS
fallback.** Both ship in current Chrome, Firefox and Safari. `chat-seal.js`
already requires both, so a browser without them cannot open the private
channel the call belongs to.

**ML-KEM-768 in the browser is the existing `chat-mlkem.js`.** It is our own
FIPS 203 implementation (ported from the Android `MlKem768.kt`), checked
against NIST's ACVP vectors and Go's `crypto/mlkem` (see "Tests").

**A short code needs commit-then-reveal, not a longer hash.** v1's code was
a function of public, long-lived values: identity keys and the call id. A
server that wants a split view can search offline for two fake keys whose
codes collide. Only length (and 5200 hash iterations) stopped that, which is
why v1 needed 30 digits. v2 puts fresh randomness from every participant into
the code. Each participant commits to its random nonce before it can see
anyone else's, and reveals only after it holds everyone's commitment. The
attacker must then fix its own contribution before it learns the honest one,
so the code it ends up with is a fresh random value it cannot steer. This is
the ZRTP short authentication string (RFC 6189, section 4.4.1.1: the hash
commitment in `Commit` before `DHPart2`) extended to a group, and it is why
ZRTP's 16-bit and 20-bit codes are safe. Key stretching is useless here (the
attacker gets no offline search to slow down), so v2 drops the 5200
iterations, and the code costs one SHA-256.

**Generations, so the round converges without an ordering service.** A
round is identified by the roster and a generation number `gen`. Each
participant's `gen` only goes up, and participants adopt the highest
compatible `gen` they see. Without it, two participants who re-committed for
the same roster could each reveal against the other's previous commitment.
Each would then see the other's new commitment arrive after its own reveal,
and they would restart each other forever. With `gen`, a participant reveals
only when every member has committed to exactly its `(roster, gen)`. A member
can make at most one commitment per `(roster, gen)` (a second one is
`equivocation`). A higher compatible `gen` makes everyone move to it once
(`TestTransientRosterChangeConvergesWithoutLivelock`).

**Media authentication: batch signatures (option A), not TESLA (option B).**

- TESLA (delayed disclosure of a MAC-chain key) is only secure if a receiver
  knows a frame arrived before the sender disclosed that frame's key. That
  needs a bound on clock offset and delivery delay between sender and
  receiver. Here the SFU controls delivery timing, phone clocks can be off by
  seconds, and our only clock check is the ±5-minute presence window. A
  colluding SFU could hold Alice's disclosure back from Bob, let the
  colluding participant forge with the disclosed key, and deliver the forgery
  first. The fix is an interactive clock sync with every sender over a path
  the attacker also controls, which is complex and slow to converge.
- Batch signatures need no clocks. A receiver checks each frame against its
  sender's signed digest list whenever both have arrived, in any order.
- Loss: a lost TESLA disclosure is recovered by the chain, and a lost batch
  only leaves its own window (about 200 ms) unverified. The window is never
  reused, so nothing is invalidated permanently. The next batch verifies the
  next frames. A sender may also repeat earlier digests in a later batch
  (overlapping windows; receivers accept any consistent overlap).
- Latency: both verify one interval after the frame. Batch signatures add
  no buffering at the sender.

**Digests of the whole sealed frame, not GCM tags.** The brief suggested
signing the frames' tags. That would not stop the attack it is meant to stop.
A GCM tag is a polynomial MAC under `H = AES_K(0)`, and the colluding
participant holds `K`. Given any target tag it can solve for a ciphertext
block that produces exactly that tag (GHASH is linear in the ciphertext once
`H` is known). So a signed tag would also cover its forged frame. The digest
is instead SHA-256 over the whole sealed frame (clear header bytes,
ciphertext, tag, counter, key id), cut to 10 bytes.

**10-byte (80-bit) digests.** A colluding participant who wants to replace
Alice's frame `F` needs another frame with the same digest: a second preimage.
It has from when `F` is sent until the receiver stops accepting that counter,
about 1 to 2 s (the auth deadline plus jitter; frames later than that are
also useless to the jitter buffer). 64 bits would need about 2^64 SHA-256 per
second. That is within an order of magnitude of the whole Bitcoin network's
fixed-function SHA-256d rate, so too thin a margin. 80 bits needs about
2^80, out of reach for anyone. Each extra 2 bytes costs about 100 B/s at 50
frames a second.

**The media auth key rides in the key message, not the presence.** Each
send epoch draws a fresh Ed25519 key pair (`ak`). The key message, signed by
the sender's identity, names the send epoch whose frames the key signs. The
presence names only presence epochs, and a receiver cannot map a sender's
send epoch back to its presence epoch. So binding the key there would need
another field and leave a gap at rotation. The key message binds it to
exactly the frame key it goes with. A separate key, instead of signing with
the identity key, keeps the identity key out of the media engine and the
browser worker (it stays in the seal store's `ctx.exclusive`). It costs no
identity-key operation 5 times a second, and it is forgotten with the epoch.

**Verify after playout.** Audio cannot wait 200 ms for its batch. So
receivers play frames as they decrypt and judge them when their batch
arrives. A frame already known to be bad (its batch came first) is never
played. The guarantee is therefore detection within about one interval plus
delivery, followed by dropping the stream (see "Media authentication"). It is
not prevention of the first forged 200 ms. A strict mode that holds video
until verified is possible in the engine at the cost of one interval of
latency; it is not built.

## Encodings

- `b64u` is base64url without padding, decoded strictly: non-canonical
  trailing bits and padding are rejected in Go and JS alike.
- `H(x)` is SHA-256, `id16(x)` the first 16 characters of `b64u(H(x))`.
- Integers in JSON are exact safe integers (|n| < 2^53). Go parses them the
  way JavaScript does, so `1.0` is 1 on both, and both reject quoted numbers.
- Strings in signed messages are joined with `\n` (0x0A).
- `call` is 1 to 256 printable ASCII characters. `peer` is 1 to 64 of
  `[A-Za-z0-9_-]` (the app issues 32 hex characters). `acct` is 1 to 128
  code points with no C0 or C1 controls, no U+FFFD and no lone surrogates.
- JSON fields are matched case-sensitively; unknown fields are ignored.
- `u8`, `u16be`, `u32be`, `u64be` are big-endian unsigned integers in binary
  messages.

## Presence

Each participant holds, per presence epoch `p` (1, 2, ...), an X25519 key
pair and an ML-KEM-768 key pair (generated from a 64-byte seed `d || z`), and
announces them:

```json
{"v":2,"call":"<call>","peer":"<peer>","acct":"<account>","kid":"<16>","idk":"<b64u 32>",
 "epoch":1,"ts":1790000000000,"x":"<b64u 32>","pq":"<b64u 1184>","sig":"<b64u 64>"}
```

`sig` is Ed25519 by `IK` over:

```
forkmesh-call-e2ee-presence-v2\n<call>\n<peer>\n<acct>\n<kid>\n<idk>\n<epoch>\n<ts>\n<x>\n<b64u(H(pq bytes))>
```

A receiver accepts it when:

1. The shape is valid and `kid == id16(idk)`; otherwise `invalid`. A
   presence without `pq`, with a short `pq` or with another version is
   `invalid`.
2. `call` is this call (`call`), and `peer` is not the receiver's own
   (`own`, or `peer-bound` when someone else signs it).
3. `ts` is within 5 minutes of the receiver's clock (`clock`).
4. A peer already in the roster must keep its `idk` (`peer-bound`). A
   new peer beyond 15 others is refused (`full`).
5. The signature verifies (`signature`).
6. For a known peer: `acct` must not change (`peer-bound`); a lower epoch is
   `stale`; the same epoch with different `x` or `pq` is `equivocation`, the
   same keys with an older `ts` is `stale`, otherwise a heartbeat (`same`);
   a higher epoch replaces the keys (`updated`). A new peer is `joined`.

A peer not heard from for 60 s drops out (`Expire`), which releases its peer
id, as the roster does today.

## Key messages (`group-call-key`)

For send epoch `e` (1, 2, ..., per peer id), a sender draws a 32-byte secret
`S`, a 32-byte media auth seed (Ed25519 key pair `ak`), and a fresh X25519
key pair `eph` for this message. For every receiver `R` in the roster, using
`R`'s newest presence (`R.epoch`, `R.x`, `R.pq`):

```
ss1      = X25519(eph.priv, R.x)                      (all zero: invalid)
ct, ss2  = ML-KEM-768.Encaps(R.pq)
salt     = H("forkmesh-call-e2ee-salt-v2\n" + call)
info     = "forkmesh-call-e2ee-wrap-v2\n" call \n S.peer \n b64u(S.idk) \n e \n
           R.peer \n b64u(R.idk) \n R.epoch \n b64u(R.x) \n b64u(H(R.pq)) \n b64u(eph.pub) \n b64u(H(ct))
kek || n = HKDF-SHA256(ikm = ss1 || ss2, salt, info, 44)    (32-byte key, 12-byte nonce)
key      = AES-256-GCM(kek, n, S, aad = info)               (48 bytes)
```

The KEK is single use (fresh `eph` and fresh ML-KEM randomness every time),
so the derived nonce never repeats under it. The info binds both identities,
both peer ids, both epochs and both halves of each exchange (X25519 public
keys, ML-KEM key and ciphertext), as the X-Wing and TLS hybrid combiners do.
`eph.priv`, `ss1`, `ss2` and `kek` are wiped after use.

```json
{"v":2,"call":"<call>","peer":"<sender peer>","kid":"<16>","idk":"<b64u>","epoch":1,"ts":0,
 "eph":"<b64u 32>","ak":"<b64u 32>",
 "keys":[{"peer":"<receiver>","epoch":1,"ct":"<b64u 1088>","key":"<b64u 48>"}],
 "sig":"<b64u 64>"}
```

`keys` holds 1 to 31 entries sorted by peer (ASCII), without duplicates or the
sender. `sig` is Ed25519 by `IK` over

```
forkmesh-call-e2ee-keys-v2\n<call>\n<peer>\n<kid>\n<idk>\n<epoch>\n<ts>\n<eph>\n<ak>
```

followed, per entry in order, by `\n<peer>|<epoch>|<b64u(H(ct bytes))>|<key>`.

A receiver checks these in order, each failing with the code shown:

1. The shape is valid, including a 32-byte `ak` (`invalid`).
2. `call` matches (`call`).
3. The message is not its own (`own`).
4. The sender has an accepted presence (`unknown-peer`) with the same `idk`
   (`peer-bound`).
5. `ts` is within 5 minutes (`clock`).
6. The signature verifies (`signature`).
7. `epoch` is above the last epoch opened from that sender (`stale`, or
   `duplicate` for the same one).
8. There is an entry for it (`not-for-me`).
9. It still holds the presence epoch the entry names (`recipient-epoch`).
10. The unwrap succeeds (`invalid`).

Then

```
frame key = HKDF-SHA256(ikm = S, salt, "forkmesh-call-e2ee-frame-v2\n" call \n S.peer \n b64u(S.idk) \n e, 32)
key id    = e mod 256
auth key  = ak (receivers), ak's seed (the sender)
```

and `S` is wiped. Go and JS check in the same order and return the same error
codes (`ErrorCode` in Go, `CallE2eeError.code` in JS); the shared reject
vectors hold them to it.

## Epochs

| Event | Every participant |
| --- | --- |
| Joins (has its peer id) | Creates the session (presence epoch 1), announces it, rekeys and commits once it has heard others |
| Hears a new participant (`joined`) | Rotates its presence, rekeys, commits (new round) |
| Hears a new presence epoch (`updated`) | Rekeys |
| Somebody leaves (`out`) or expires | Rotates its presence, rekeys, commits (new round) |
| Every 20 s | Re-sends its current presence (re-signed, new `ts`) |
| Every 10 minutes | Rotates its presence (the verification round is unaffected) |
| `recipient-epoch` from a key message, or a sender's frames have an unknown key id 3 s after its presence | Rotates its presence (at most once per 10 s) |

Rekeys and commits are debounced by 250 ms. A join, which makes everyone
rotate, then costs each sender one or two key messages and one commit.

A rotation keeps the previous presence key pair for 30 s (`PresenceGrace`,
`Prune`), and at most two are kept, so a key message wrapped just before a
rotation still opens. A sender installs a new send key 1 s after sending the
key message (the delay `chat-group-calls.js` already uses on leave).
Receivers keep three keys per sender, so frames from the previous epoch still
decode during the switch.

A joiner sees nothing from a sender until that sender's next key reaches it,
about a second. That is the price of not giving joiners the epoch in progress.

## Frames

The frame layout is the one in [chat-group-calls.md](../app/chat-group-calls.md)
and [rtc.md](rtc.md):

- readable codec bytes as additional data (Opus 1; VP8 10 on a keyframe, 3
  otherwise);
- the AES-256-GCM ciphertext with its 16-byte tag;
- an 8-byte big-endian counter;
- a 1-byte key id (epoch mod 256).

The IV is four zero bytes and the counter. Receivers install a sender's keys
only in increasing epoch order (`OpenKeys` refuses older epochs, and Go's
`FrameReceiver.SetKey` refuses a rollback).

`server/rtc/e2ee` has `FrameSender` and `FrameReceiver` for the Go engine
core:

- one counter per sender, guarded by a mutex;
- a 1024-frame replay window, moved only after the tag verifies;
- three keys per sender;
- since v2, the media authentication below.

They produce exactly the bytes the browser worker produces; the vectors check
both ways.

## Verification code

### Round

The roster is every participant the device sees, itself included, as
`(idk, acct, peer)`. A round is the roster plus a generation `gen` (1, 2, ...):

```
entry      = idk (32) || u16be(len acct) || acct (UTF-8) || u8(len peer) || peer
entries    = all entries, sorted bytewise
round      = H("forkmesh-call-e2ee-round-v2\n" call "\n" || u32be(gen) || u8(count) || entries)
```

For each round a participant draws a fresh 32-byte nonce `N`. It sends a
commit:

```json
{"v":2,"t":"commit","call":"<call>","peer":"<peer>","kid":"<16>","idk":"<b64u 32>",
 "round":"<b64u 32>","gen":1,"ts":1790000000000,"c":"<b64u 32>","sig":"<b64u 64>"}
```

```
c   = H("forkmesh-call-e2ee-nonce-v2\n" call \n peer \n b64u(idk) \n b64u(round) \n gen \n || N)
sig = Ed25519 by IK over
      forkmesh-call-e2ee-commit-v2\n<call>\n<peer>\n<kid>\n<idk>\n<round>\n<gen>\n<ts>\n<c>
```

Once it holds, from every other member, a commit for exactly its own
`(round, gen)`, and not before, it sends a reveal:

```json
{"v":2,"t":"reveal","call":"<call>","peer":"<peer>","round":"<b64u 32>","gen":1,"nonce":"<b64u 32>"}
```

A reveal needs no signature: it is only accepted if it opens the signed
commit, and before that only the committer knows `N`.

`gen` rules (`Session.Commit`):

- A participant's new round uses `gen = max(own last gen + 1, the highest
  gen among members' latest commits whose round matches this roster)`, so
  gens never repeat for one participant and everyone lands on the same one.
- A commit with a higher `gen` whose round matches the receiver's current
  roster makes the receiver's round stale. Its next `Commit` joins that `gen`
  with a fresh nonce.
- A commit more than 65536 above the receiver's own `gen` is `invalid`. That
  stops a malicious member from exhausting the counter in one step.

### Code

When a participant has revealed and holds every member's opened nonce:

```
transcript = H("forkmesh-call-e2ee-sas-v2\n" || round || the nonces, in the order of entries)
emoji      = the first 24 bits of transcript as four 6-bit indices into the table below
digits     = u32be(transcript[3..7]) mod 1000000, shown as "ddd ddd"
qr         = "FMCALL2:" + base32 (RFC 4648, no padding) of transcript   (60 characters)
```

Example (the three-member round in the stored `go-vectors.json`, gen 1):
emoji indices 37 4 51 54, Umbrella, Unicorn, Train, Rocket; digits
`293 475`; QR `FMCALL2:SRGPMT3YOGRRMIAXAGALUTFIWAFYPN7B5N6VTNQG5ETQP76TIPHA`.

The emoji table is 64 entries, chosen for being easy to tell apart and say
aloud across platforms (the same list Matrix uses for its SAS). Index:
emoji, name.

| 0-15 | 16-31 | 32-47 | 48-63 |
| --- | --- | --- | --- |
| 0 🐶 Dog | 16 🌳 Tree | 32 🎩 Hat | 48 🔨 Hammer |
| 1 🐱 Cat | 17 🌵 Cactus | 33 👓 Glasses | 49 ☎️ Telephone |
| 2 🦁 Lion | 18 🍄 Mushroom | 34 🔧 Spanner | 50 🏁 Flag |
| 3 🐎 Horse | 19 🌏 Globe | 35 🎅 Santa | 51 🚂 Train |
| 4 🦄 Unicorn | 20 🌙 Moon | 36 👍 Thumbs up | 52 🚲 Bicycle |
| 5 🐷 Pig | 21 ☁️ Cloud | 37 ☂️ Umbrella | 53 ✈️ Aeroplane |
| 6 🐘 Elephant | 22 🔥 Fire | 38 ⌛ Hourglass | 54 🚀 Rocket |
| 7 🐰 Rabbit | 23 🍌 Banana | 39 ⏰ Clock | 55 🏆 Trophy |
| 8 🐼 Panda | 24 🍎 Apple | 40 🎁 Gift | 56 ⚽ Ball |
| 9 🐓 Rooster | 25 🍓 Strawberry | 41 💡 Light bulb | 57 🎸 Guitar |
| 10 🐧 Penguin | 26 🌽 Corn | 42 📕 Book | 58 🎺 Trumpet |
| 11 🐢 Turtle | 27 🍕 Pizza | 43 ✏️ Pencil | 59 🔔 Bell |
| 12 🐟 Fish | 28 🎂 Cake | 44 📎 Paperclip | 60 ⚓ Anchor |
| 13 🐙 Octopus | 29 ❤️ Heart | 45 ✂️ Scissors | 61 🎧 Headphones |
| 14 🦋 Butterfly | 30 😀 Smiley | 46 🔒 Lock | 62 📁 Folder |
| 15 🌷 Flower | 31 🤖 Robot | 47 🔑 Key | 63 📌 Pin |

The code tables hold code points (with U+FE0F where the emoji needs it), not
literal characters, so neither source file depends on an editor's handling
of invisible selectors. The vectors carry the whole table so Go and JS are
checked to match.

The emoji and the digits come from different bytes of the same transcript.
They are two views of one round; a group compares one of them. The QR form is
for in-person scanning: the scanning device decodes it and compares the full
256-bit transcript with its own in constant time (`CheckQR`). A match marks
the round verified at full strength without anyone reading anything aloud.

### Checks on receipt

Commit, in this order, each failing with the code shown:

1. The shape is valid, with `v:2` and `t:"commit"` (`invalid`).
2. `call` matches (`call`).
3. It is not its own (`own`, or `peer-bound`).
4. The sender is a known member (`unknown-peer`) with the same `idk`
   (`peer-bound`).
5. `ts` is within 5 minutes (`clock`).
6. `gen` is at most 65536 above the receiver's own (`invalid`).
7. The signature verifies (`signature`).
8. Against that member's latest commit: a lower `gen` is `stale`; the same
   `gen` is `duplicate` if identical, otherwise `equivocation`.

Reveal, in this order:

1. The shape is valid, with `v:2` and `t:"reveal"` (`invalid`).
2. `call` matches (`call`).
3. It is not its own (`own`).
4. The sender is a known member (`unknown-peer`).
5. A reveal for a `gen` above the member's latest commit is held, one per
   member, until that commit arrives, and reports `waiting`. A reveal below
   it is `stale`.
6. It must name the commit's round and open `c` (`commitment`); a second one
   is `duplicate`.

### Joins, leaves and "verified"

- Every join or leave changes the roster and so the round. Everyone commits
  again (a fresh nonce), the old code disappears, and the new one appears
  once the round finishes. A presence rotation does not change the round.
- The UI shows "verified" from identities, not codes. When the user confirms
  a code (`MarkVerified(round)`) or a QR scan matches (`CheckQR`), every
  `(idk, acct)` in that round's roster joins the device's verified set.
  `Verification()` reports `Verified` while every current member is in the
  set, and `Newcomers` for those who are not. A leave keeps "verified"; a
  rejoin of the same device (same identity, new peer id) keeps it. A newcomer
  makes the call "not verified" until a round including them is confirmed.
  This is sound because everything is signed by the identity keys: a
  confirmed round shows that everyone saw the same identity keys. A later
  man in the middle would need a different key, which is a newcomer.
- `MarkVerified` accepts only the round whose code is on screen now, so a
  user cannot confirm a stale code by tapping late.

### Late, missing and withheld reveals

- `Verification().Waiting` lists the members whose commit or reveal for the
  current round is missing. After 10 s (`RevealTimeout`) `Late` is set, and
  the UI says who it is waiting for.
- The integration re-sends its own current commit and reveal (the same
  bytes, `Commit()` and `Reveal()` return them again) every 2 s while
  `Waiting` is not empty, and immediately on a `joined` event. Chat frames
  are not ordered, so this also repairs a commit that arrived before its
  sender's presence (`unknown-peer`).
- A round in which this device revealed but never got a code counts as
  unfinished: when the roster changes, a higher `gen` takes over, or 30 s
  pass (`RoundTimeout`). `Unfinished` is reported. That pattern is exactly
  what a man in the middle produces when it sees the codes will not match and
  withholds the rest of the round (see the analysis below).
- `Rounds` counts the rounds this device revealed in.

UI text:

| Situation | Text |
| --- | --- |
| Code shown | "Compare these with everyone in the call" above the four emoji with their names below; "Or compare numbers: 123 456"; buttons "They match" and "They don't match"; "Scan a code instead" |
| Waiting | "Waiting for Bob and Carol to confirm…" |
| Code changed | "The verification code changed because Erin joined" |
| Newcomer | "Erin is not verified. Compare the new code to verify everyone." |
| They don't match | "Someone in this call may not be who they seem. Leave the call or ask everyone to check their devices." |
| `Unfinished` ≥ 3 or `Rounds` > 32 | "Verification keeps restarting. Someone may be interfering with this call. Scan each other's codes if you can." |

### Why a short code is safe: the analysis

The attack the code exists to catch is a man in the middle. The server shows
Alice a fake Bob (Bob′, its own key) and shows Bob a fake Alice (Alice′), and
re-wraps everything in the middle. Alice's code is computed over
`(Alice, Bob′)` with nonces `N_A, N_B′`; Bob's over `(Alice′, Bob)` with
`N_A′, N_B`. The attack succeeds if the two codes are equal.

1. Alice reveals `N_A` only after she holds Bob′'s signed commitment for her
   round. So the attacker fixes `N_B′` before it learns anything about
   `N_A`. Likewise it fixes `N_A′` before Bob reveals `N_B`.
2. The commitment is binding (SHA-256), and a second commitment for the same
   round is `equivocation`. Only a higher `gen` (a new round, a new `N_A`)
   would let it change its value.
3. So whatever order the attacker delivers things in, one of the two codes
   includes an honest nonce that was unknown when the attacker's value
   toward that side was fixed. Say Bob's side completes first, so the
   attacker knows Bob's code. Alice's code still depends on `N_A`, drawn
   after `N_B′` was fixed, and is uniform over the code space. The two match
   with probability 2^-24 for the emoji, 10^-6 for the digits, per round.
   That holds whatever the attacker computes, so there is no offline
   grinding at all: the attacker gets one guess per round.
4. Groups: with more members the argument is the same for each honest
   participant. Each honest code includes that participant's own nonce, drawn
   after every commitment it uses. Two honest participants with different
   views have independent uniform codes. Making a whole group agree needs at
   least one such match: at most 2^-24 per round.
5. Abort and retry. Once both sides have revealed, the attacker knows both
   codes. It can withhold the rest (no code appears) and force a new round
   for another guess, for example by making Bob′ leave and rejoin. Each
   attempt is visible: a roster change, and a round this device revealed in
   that never finished (`Unfinished`). ZRTP has the same limit, where each
   failed attempt is a dropped call. Over `k` rounds the chance that some
   round produced matching codes is at most `k · 2^-24`. The UI warns at
   three unfinished rounds or more than 32 revealed rounds:
   - at most 4 · 2^-24 ≈ 2.4 · 10^-7 before the unfinished warning;
   - at most 32 · 2^-24 ≈ 1.9 · 10^-6 before the rounds warning;
   - with the digits, 32 · 10^-6 ≈ 3.2 · 10^-5 before the rounds warning.

   Even then, the attacker has to win the specific round the users compare.
6. A malicious participant (a member, not a server) can always withhold its
   own reveal. That denies verification to everyone (they see "Waiting for
   Mallory"), but it cannot make codes match, since honest codes still
   include honest nonces it could not see in time. It can also force rounds
   by flapping its own presence. Those count, and the remedy is to remove it.
7. What the code does not show: that a device is honest, or that an account
   has only the devices you expected. An extra device added by the server for
   an existing account is an extra roster entry, so the integration shows
   every device and warns when one account is in the call twice (unchanged
   from v1).

Comparison with existing schemes:

- ZRTP's SAS is 16 or 20 bits for two parties with exactly this commitment
  argument. v2 gives 20 to 24 bits for up to 16 parties, and counts aborted
  attempts instead of relying on a dropped call being noticed.
- Signal's safety numbers (60 digits) are static per pair of identities and
  stay long for the same reason as v1's code. Signal and FaceTime have no
  group call code; group membership is checked pairwise, out of band, if at
  all.
- Matrix's emoji SAS (7 emoji, 42 bits) is pairwise and interactive in the
  same commit-then-reveal style.

v2 is shorter than Signal's code with stronger online guarantees, and it
covers the whole call at once.

## Media authentication

### Auth message

A sender keeps a window of digests of the frames it seals:

```
digest = H("forkmesh-call-e2ee-digest-v2" || sealed frame)[0..10]
```

The sealed frame is the bytes exactly as the receiver gets them: clear header
bytes, ciphertext and tag, counter, key id. Every `AuthInterval` (200 ms, the
engine's choice between 100 and 500), or when the window reaches 100 frames,
or when the send key changes, the window is closed into one auth message
(binary, at most 1079 bytes, so it fits one FM Media datagram):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `0x21` (version 2, type 1) |
| 1 | 1 | key id (must be epoch mod 256) |
| 2 | 4 | epoch, u32be (≥ 1) |
| 6 | 8 | first counter, u64be (≥ 1, and the range must not wrap) |
| 14 | 1 | n, frames covered (1..100) |
| 15 | 10·n | the digests of counters first, first+1, ..., first+n-1 |
| 15+10·n | 64 | Ed25519 by the epoch's auth key over `"forkmesh-call-e2ee-media-v2\n" call "\n" peer "\n"` and bytes 0..15+10·n |

Counters within one send key are consecutive (Go `FrameSender` guarantees
it). The JS signer, fed by the worker, starts a new window whenever a counter
does not follow the previous one.

### Receiver

`FrameReceiver.Open` (Go) and `createMediaVerifier().frame` (JS, called by
the worker after it decrypts) work as follows:

1. The frame's digest is computed.
2. If a verified auth message already covers its counter, the digests are
   compared. On a match the frame is `verified`. Otherwise it is `forged` and
   is not played (`ErrForged`, `"forged"`).
3. If no auth message covers the counter yet, the frame plays and waits.
4. When an auth message arrives (`AcceptAuth` / `accept`), it is checked:
   the shape (`invalid`); a key with that key id for that sender
   (`unknown-peer`); the key's epoch (`stale`); the signature, by that
   sender's `ak` for this call and peer (`signature`). It must agree with
   every overlapping window already held. A different digest for the same
   counter is sender `equivocation` and counts as forged; an identical window
   is `duplicate`. Then every waiting frame in its range is judged
   `verified` or `forged`.
5. A frame with no verdict 1 s after it arrived (`AuthDeadline`) is
   `missing`. Windows are kept for 1 s, so frames that arrive after their auth
   message verify at once.

Per sender state, from the verdicts:

| State | When | Tile | Media |
| --- | --- | --- | --- |
| `pending` | nothing judged yet (the first ~200 ms of an epoch) | normal | plays |
| `verified` | the last judgement was verified | normal (a small check mark in the call info) | plays |
| `missing` | a frame went missing in the last 2 s (`AuthRecent`) | amber: "Can't confirm this is Bob right now" | plays, then dropped once no frame has verified for 2 s (`MissingGrace`) while frames keep going missing (`ErrUnverified`, `"unverified"`); resumes at the next verified frame |
| `forged` | a forged frame or a sender equivocation in the last 10 s (`AuthHold`) | red: "Bob's video failed a check. Someone in this call may be faking it." | dropped at once and for 10 s after the last forged frame (`ErrForged`); then back to normal |

Each change of state is an event: the `OnAuth(peer, state)` callback in Go
and `onState(peer, state)` in JS. `Status(peer)` returns the state,
`Dropping`, and counts of verified, missing and forged frames.

Why the two failure cases differ:

- A forged frame is proof. Only a holder of the frame key (a participant) can
  make a frame that passes GCM, and only the sender's auth key can sign the
  digest it contradicts. So there is no grace: the stream stops for 10 s and
  the user sees why. Frames keep being decrypted and judged during the drop,
  so the counts stay right and the stream comes back by itself.
- Missing auth is ambiguous: loss, a slow path, or an SFU dropping auth
  messages so that a colluder can inject into the gaps. A forgery in an
  unverified window is played but never verified. The tile turns amber within
  about 1 s, and media stops after the 2 s grace if authentication does not
  come back. A short gap (one lost auth message) leaves the stream playing
  with an amber flash (`TestAShortAuthGapDoesNotDrop`).

### Overhead

Bytes, from `TestAuthOverheadPerSecond` (Go) and the JS cost test (same
numbers), at the default 200 ms interval:

| Sender | Frames/s | Auth messages/s | Auth bytes/s | One 64-byte signature per frame |
| --- | --- | --- | --- | --- |
| Audio only (Opus 20 ms) | 50 | 5 | 895 B/s (7.2 kbit/s) | 3200 B/s (25.6 kbit/s) |
| Audio + video 30 fps | 80 | 5 | 1195 B/s (9.6 kbit/s) | 5120 B/s |
| Audio + two simulcast layers | 110 | 5 | 1495 B/s (12.0 kbit/s) | 7040 B/s |

That is 10 bytes per frame plus 79 bytes per message (15 header, 64
signature), carried once per publisher however many subscribers there are
(the SFU forwards one copy to each). On a weak audio-only link (the face
stream's case, Opus at 6 to 12 kbit/s), 7.2 kbit/s is a lot. The engine can
cut it two ways:

- a 500 ms interval: 660 B/s;
- 60 ms Opus frames, which a weak link uses anyway: 17 digests a second,
  370 B/s at 500 ms.

The interval only moves detection time, not security.

CPU, measured on the build box while nothing else ran (Go `go test -bench`,
Node 20 for JS):

| What | Go (i9-10980XE, one core) | JS (Node 20, WebCrypto) |
| --- | --- | --- |
| Seal an audio frame (60 B), digest included, signing amortized over 10 frames | 3.9 µs (AES-GCM alone: 0.2 µs) | 111 µs per frame (digest plus amortized signing) |
| Seal a video frame (3 KB) | 15 µs | 64 to 118 µs per frame across the three profiles |
| Seal a keyframe (30 KB) | 101 µs | |
| Open an audio frame, digest and amortized auth check included | 12.7 µs | 78 to 118 µs per frame |
| Open a video frame (3 KB) | 24 µs | |
| Sign one auth message (22 digests) | 31 µs | |
| Verify one auth message | 69 µs | |

In Go the added cost is one SHA-256 over the frame plus 5 signatures (sender)
or 5 verifications per sender (receiver) a second: well under 1% of a core
for a full 16-person call. In JS the cost is mostly WebCrypto's promise round
trip per `digest()` call, not hashing. A receiver of 15 senders at 110 frames
a second spends about 15% of one core in `frame()`. If that shows up on
phones' browsers, a synchronous SHA-256 in the worker removes most of it (the
digest is public data, so constant time does not matter); it is not done
yet.

### Threats this covers and does not

- A participant colluding with the SFU forges Alice's media: caught. The
  frame passes GCM, but its digest is not the one Alice signed. Before her
  auth message arrives it plays (at most about one interval plus delay); when
  it arrives the tile turns red and the stream stops (`TestColludingParticipantCannotForgeMedia`).
- The same, delivered after Alice's auth message: refused before playing.
- The SFU withholds auth messages so the colluder can forge into the gaps:
  amber within about 1 s, dropped after 2 s more; the forged frames are
  never shown as verified.
- Replay: frames are still replay-checked by counter. An auth message is
  bound to its epoch's key, call and peer, and a replayed one is `duplicate`.
- The colluder signs its own auth message for Alice's frames: `signature`,
  since it lacks Alice's auth key.
- Alice herself equivocates (two different digests for one counter): forged.
- Not covered: Alice's own device sending media she did not mean to send, and
  someone holding Alice's identity key (they can sign a key message with
  their own auth key; the code cannot show that, see "Compromise").

## Engine integration (FM Media, not done; no change to `server/rtc/fmm` in this work)

Where auth messages travel: a third track kind, `TrackAuth = 3`, with one
layer, carried in ordinary media datagrams (kind 0) and forwarded by the SFU
like an audio track. Two options were rejected:

- The control stream (a new `MsgMediaAuth`): it is reliable, but a lost
  packet would block `Nack`, `Report` and `KeyframeRequest` behind it (head
  of line). It also adds an SFU relay path for every publisher.
- A new datagram kind: it would need its own loss recovery.

As a track, auth messages get the per-stream Nack retransmission and FEC
that already protect audio, and the SFU needs only:

- `validTrack` accepting kind 3 with one layer;
- forwarding it like audio: never thinned by layer selection, counted in the
  subscriber's audio budget;
- `TrackAdded` naming the publisher's peer id, as it does today.

Each message fits one datagram (1079 + 16 header bytes, under the
1162-byte budget left after the FEC reserve).

What the engine calls (Go, `server/rtc/e2ee`):

```go
var tx e2ee.FrameSender                    // one per local sender
tx.SetKey(key)                             // key from Session.Rekey, 1 s after sending its key message
sealed, _ := tx.Seal(encodedFrame, clear)  // then SendAudio / SendVideoFrame(sealed)
for _, m := range tx.Flush() {             // every AuthInterval from Tick, and before Leave
    client.SendAudio(authStream, m, now)   // the TrackAuth stream published at join
}

var rx e2ee.FrameReceiver                  // one per call
rx.OnAuth = func(peer string, s e2ee.AuthState) { /* event to the UI */ }
rx.SetKey(key)                             // keys from Session.OpenKeys
plain, err := rx.Open(publisherPeer, payload, clear)
// err "forged" or "unverified": treat the frame as lost (conceal audio, freeze video), do not decode it
rx.AcceptAuth(publisherPeer, authPayload)  // for every frame on that publisher's auth track
rx.Status(publisherPeer)                   // on the UI tick
```

Subscribing: when `TrackAdded` announces a kind-3 track, subscribe to it
whenever any of that publisher's tracks is subscribed. Every received frame
needs its publisher's digests, whatever layer is chosen. Digests for
simulcast layers a subscriber does not get are carried anyway and expire
unused. The cost of that is in the table above.

Browser: the worker (`chat-group-call-crypto.js`) imports
`chat-call-e2ee.js`:

- After each `encrypt` it calls `signer.add(sealed)` on a
  `createMediaSigner(key)` for the current send key, and on a 200 ms timer it
  posts `signer.flush()` to the page, which sends it on the auth track.
- Before releasing a decrypted frame it calls `verifier.frame(peer, sealed)`
  on one `createMediaVerifier({ onState })` and drops the frame unless it
  returns `""`.
- Auth messages from the page go to `verifier.accept(peer, bytes)`.

`setKey` takes the key that `openKeys` returned (`auth` included).

## API

Go (`server/rtc/e2ee`, standard library only: `crypto/mlkem`, `crypto/ecdh`,
`crypto/ed25519`, `crypto/hkdf`, `crypto/aes`):

```go
func NewSession(Config{Call, Peer, Acct string; Identity ed25519.PrivateKey; Rand io.Reader; Now func() time.Time}) (*Session, error)
func (s *Session) Presence() ([]byte, error)                // current presence, re-signed (heartbeat)
func (s *Session) Rotate() ([]byte, error)                  // new presence epoch
func (s *Session) AcceptPresence(b []byte) (Event, error)   // EventJoined, EventUpdated, EventSame
func (s *Session) Leave(peer string) bool
func (s *Session) Expire() []string
func (s *Session) Prune()
func (s *Session) Rekey() ([]byte, FrameKey, error)         // key message (nil when alone) and own send key (with AuthSeed)
func (s *Session) OpenKeys(b []byte) (FrameKey, error)      // a sender's key (with Auth)
func (s *Session) Commit() ([]byte, error)                  // commit for the current round (same bytes until it changes)
func (s *Session) AcceptCommit(b []byte) error
func (s *Session) Reveal() ([]byte, error)                  // "waiting" until every commit is in
func (s *Session) AcceptReveal(b []byte) error              // "waiting" when held for its commit
func (s *Session) Verification() VerifyStatus               // Round, Gen, NeedCommit, Code *SAS, Waiting, Late, Rounds, Unfinished, Verified, Newcomers
func (s *Session) MarkVerified(round []byte) error
func (s *Session) CheckQR(text string) (bool, error)
func (s *Session) Unverify()
func (s *Session) Peers() []string
func (s *Session) Close()
func ErrorCode(err error) string
type FrameKey struct { Call, Peer string; Epoch uint32; KeyID byte; Key []byte; Auth ed25519.PublicKey; AuthSeed []byte }
type SAS struct { Round, Transcript []byte; Emoji [4]int; Digits, QR string }    // EmojiText(), EmojiNames(); EmojiTable
func (s *FrameSender) SetKey(FrameKey) error; Seal(frame []byte, clear int) ([]byte, error); Flush() [][]byte
func (r *FrameReceiver) SetKey(FrameKey) error; Open(peer string, frame []byte, clear int) ([]byte, error)
func (r *FrameReceiver) AcceptAuth(peer string, b []byte) error; Status(peer string) AuthStatus; Forget(peer string)
```

Messages go in and out as JSON (or binary for auth messages) bytes, so a
gomobile wrapper only needs `[]byte`, `string` and `int` at its boundary.
`Session`, `FrameSender` and `FrameReceiver` are safe for concurrent use;
`OnAuth` is called outside the receiver's lock. The phones keep `Identity` in
the keychain or Keystore. The binding passes in the 32-byte seed and builds
the key with `ed25519.NewKeyFromSeed`.

JS (`app/public/chat-call-e2ee.js`, an ES module):

```js
const session = await createCallSession({ call, peer, acct, signer, now, random });
await session.presence(); await session.rotate();           // presence objects
await session.acceptPresence(obj);                            // { kind: "joined" | "updated" | "same", peer }
session.leave(peer); session.expire(); session.prune(); session.peers();
const { message, key } = await session.rekey();               // key = { call, peer, epoch, keyId, raw, auth, authSeed }
const key = await session.openKeys(obj);                      // { call, peer, epoch, keyId, raw, auth }
await session.commit(); await session.acceptCommit(obj);
await session.reveal(); await session.acceptReveal(obj);
const st = await session.verification();                      // { round, gen, needCommit, code, waiting, late, rounds, unfinished, verified, newcomers }
await session.markVerified(st.code.round); await session.checkQr(text); session.unverify(); session.close();
const signer = await createMediaSigner(key); await signer.add(sealed); const msgs = await signer.flush();
const verifier = createMediaVerifier({ now, onState }); verifier.setKey(key);
const drop = await verifier.frame(peer, sealed);              // "" to play, "forged" or "unverified" to drop
await verifier.accept(peer, bytes); verifier.status(peer);    // { state, dropping, verified, missing, forged }
```

`signer` is `{ idk: Uint8Array(32), sign(bytes) -> Promise<Uint8Array(64)> }`.
Session methods run one at a time in call order. Errors are `CallE2eeError`
with `.code`. `code` is `{ round, transcript, emoji, emojiText, emojiNames,
digits, qr }`. The low-level functions (`parsePresence`, `parseCommit`,
`roundId`, `computeSas`, `parseAuthMessage`, `frameDigest`, ...) are exported
for the vector tests.

## Integration (not done yet)

1. **Signer.** Add a `signCall(bytes)` method to `createRoomSealStore`
   (`chat-seal.js`). It loads the identity inside `ctx.exclusive`, refuses
   bytes that do not start with `forkmesh-call-e2ee-`, and returns the
   Ed25519 signature. The phones and the desktop add the same method to
   their keyring.
   `signer = { idk: b64u decode of store.identity().idk, sign: store.signCall }`.
   Media auth keys do not go through it.
2. **Session.** In `chat-group-calls.js`, after `joined` gives the peer id:
   `createCallSession({ call: <the channel's chat room name>, peer, acct:
   store.account(), signer })`. The call id is the chat room name, not the
   SFU room id, because the client knows it without asking the server, and
   the chat v3 seal is already bound to it.
3. **Presence.** `group-call-presence` frames carry
   `e2ee: await session.presence()` (`rotate()` where the table above says
   so). On receipt, the presence's `idk` must equal the sealed frame's
   authenticated sender identity (`unsealFrame`'s sender `idk`), and its
   `acct` the sender's account; otherwise drop it. Then `acceptPresence`,
   and on `joined`/`updated` schedule the debounced rekey; on `joined`, the
   debounced commit too.
4. **Keys.** `group-call-key` frames become `{peer, e2ee: message}`. On
   `openKeys` success, give the key to the worker (`{op: "key", ...}`) and
   to the media verifier. After a rekey, post `send-key` 1 s later and swap
   the media signer. On `recipient-epoch`, rotate (rate-limited).
5. **Verification.** A new `group-call-verify` frame type carries
   `{e2ee: commit}` or `{e2ee: reveal}` (`t` tells them apart). The sealed
   frame's sender identity must equal the commit's `idk`, and for a reveal
   the identity of the member named. After every roster change, call
   `commit()` (debounced 250 ms) and send it. After every accepted commit,
   try `reveal()` and send it when it does not return `waiting`. After every
   accepted reveal, read `verification()`. Re-send both every 2 s while
   `waiting` is not empty. When `needCommit` is set, commit again.
6. **Leave.** `out` or a 60 s silence calls `leave`/`expire`, forgets the
   worker's and the verifier's keys for that peer, rotates, rekeys and
   commits. Leaving the call calls `close()`.
7. **Code and media state in the UI.** As in "UI text" and the media state
   table. iOS shows it first, and the change is registered in
   `docs/native/SYNC.md` for Android.
8. **No fallback.** Every client switches at once, as chat v3 did. A client
   that speaks call e2ee ignores `group-call-key` frames without `e2ee` and
   any message with another version, so nobody can be downgraded.
9. **Native.** `native/webrtc/fm/fm_frame_crypto.h` has
   `kFrameKeyBytes = 16`; it must take 32-byte keys (AES-256-GCM) before the
   phones install these keys. The FM Media engine core uses `FrameSender`
   and `FrameReceiver` directly, as in "Engine integration".

## Threat model and review

Adversaries:

- the call server (SFU);
- the chat relay and app server, which deliver the sealed chat frames, issue
  peer ids and tickets, and run channel membership;
- a participant, alone or colluding with either server;
- a network observer who records everything for a future quantum computer;
- a thief who gets a device's memory once.

**A malicious SFU.** It never sees a presence, a key message, a commit or a
reveal: those travel in the chat channel's v3-sealed frames. What it can do:

- Drop, delay or reorder media and auth messages. Dropped auth turns tiles
  amber and then stops their media: a denial of service, which it could cause
  anyway by dropping the media.
- Relabel one person's stream with another's peer id. The receiver then holds
  the wrong key, the tag fails, and nothing decodes.
- Replay frames, which the replay window drops, or auth messages, which are
  `duplicate`.
- Observe packet sizes and timing, which show who is talking. Audio frames
  are not padded, a known limit; auth messages add a steady 5 per second per
  sender, which hides nothing and reveals nothing new.

It cannot decrypt or forge a frame.

**A malicious chat relay or app server.** It cannot forge a presence or a
commit without the identity key. What it can do:

- Add its own device as a participant. That shows as an extra tile, a new
  round, and a "not verified" newcomer.
- Show different rosters to different people. Each split needs a fresh round
  per attempt, and the codes differ except with probability 2^-24 per round
  (the analysis above).
- Replay an old presence. The call id, the ±5 minute `ts` window, the
  per-join peer id and the monotonic epochs stop that, except for a presence
  from someone's own join in the last 5 minutes. That one adds a ghost tile
  with the same identity. It forces a new round that cannot finish until the
  ghost expires (it never commits), and keys wrapped to it are unreadable.
- Replay an old commit. Its `gen` is not above the member's latest (`stale`
  or `duplicate`), and its round names an old roster or generation.
- Drop a reveal. Retransmission repairs honest loss. If it keeps dropping,
  the round never finishes and is counted as unfinished: one wasted guess for
  it, not a free one.
- Hand two people the same peer id. The first binding wins, the same denial
  of service as today.
- Withhold frames. That is a denial of service.

A private channel's v3 trust policy is "open", so the server can add a chat
device to the channel. The code, the roster and the "newcomer" state are
what catch it in a call.

**A malicious participant.** It holds every sender's frame key while it is
in the call; that is what being in the call means. What it cannot do:

- Take another peer id. The first binding wins, and every message is signed.
- Pass another sender's secret off as its own, since the frame keys are
  derived per sender.
- Forge another participant's media, even with the SFU's help (see "Media
  authentication").
- Steer the verification code. It can withhold its reveal (denial of
  verification, shown as "Waiting for Mallory") or flap its presence to force
  rounds (counted).
- Read anything after it leaves. The next epoch is not wrapped to it, and it
  is out about 1 s plus the delivery time after it goes.

It can send different secrets to different receivers, which only breaks its
own media. It can craft an Ed25519 signature that one platform accepts and
another rejects (implementations differ on non-canonical encodings). That
splits the roster, and the codes differ.

**Colluding SFU and participant.** The two powers combine, but each of
media forgery and code steering still needs a key the pair does not hold
(Alice's auth key; honest nonces before they commit). What remains is denial
of service and a few hundred milliseconds of unverified media before the
tile turns amber or red.

**Replay of old presence.** A presence is bound to the call id and peer id
by its signature. It must be fresh (±5 minutes), its epoch must not go back,
the same epoch must carry the same keys (`equivocation` otherwise), and a
heartbeat's `ts` must not go back. A presence from another call is `call`.

**Denial of service by never revealing.** Covered above: verification
stalls with a named culprit, media keeps flowing, and nothing becomes falsely
"verified". `MarkVerified` needs a finished round, and the identities already
verified stay verified.

**Downgrade from hybrid to classical, or from v2 to v1.** There is no
negotiation to downgrade. Version 2 is the only version, and it needs both
keys in every presence and an auth key in every key message. The signature
covers `x`, `H(pq)` and `ak`, and the KEK needs both shared secrets. Breaking
X25519 alone (a quantum computer) or ML-KEM alone (a cryptanalytic surprise)
is not enough: HKDF over `ss1 || ss2`, with both exchanges in the info, stays
secret while either holds. The chat transport under it is itself X25519 +
ML-KEM. Ed25519 is not post-quantum. A future quantum computer could forge
signatures (presences, commits, auth messages), but that helps only an active
attack during a call; it does not open recordings.

**Key-id confusion.** The key id is only a selector (epoch mod 256). The
AES key itself is derived from the call, the sender's peer id and identity
and the epoch, so a frame opens only under exactly that sender's key for that
epoch. Receivers keep three keys per sender, install only newer epochs, and
start a new replay window for a new key. When an id comes round again after
256 epochs, it selects the newest key, and a frame from the older epoch fails
its tag (`TestKeyIDWrapDoesNotConfuseEpochs`). An auth message names the full
epoch, and one whose epoch differs from the installed key's is `stale`.

**Nonce reuse across epochs.** Every epoch has a fresh random 256-bit secret
and so a fresh key. The counter restarts only when the key changes, and
re-installing the same key keeps it running (Go `FrameSender`, the worker's
`send-key`). Audio and video share one sender and one counter. A rejoin gets
a new peer id and new secrets. Even a server that hands out the same peer id
again gets a different key, because the secret is random. Each wrap's
nonce comes from HKDF under a KEK used once.

**A lost or late epoch message.**

- A late key message still opens while the receiver holds the presence
  epoch it was wrapped to (current and previous, 30 s grace). One older than
  the last opened is `stale`.
- A lost key message leaves the receiver without that sender's new key. It
  sees unknown key ids, rotates its presence, and every sender rekeys to it.
- A wrap to a presence key already dropped is `recipient-epoch` and triggers
  the same recovery.
- A lost presence means senders wrap to the previous one, which the grace
  covers.
- Frames from the previous epoch keep decoding for three epochs.

**Compromise and healing.** Someone who reads a device's memory gets:

- that device's current and previous presence private keys;
- the frame keys it holds;
- its current media auth key and round nonce;
- its identity key.

They do not get recordings older than the presence keys and frame keys the
device still held. Once the device rotates (at the latest 10 minutes later)
and senders rekey to it, a passive attacker learns nothing new. A stolen
identity key lets an active attacker sign as that device until the device
is blocked in chat. The code cannot show that, because the key is the same
one.

**What memory can and cannot be wiped.**

- Overwritten: secrets in byte arrays (epoch secrets, KEKs, shared secrets,
  ML-KEM decapsulation keys in JS, the seed and X25519 bytes before import,
  round nonces, Go's media auth private key).
- Released, not overwritten: WebCrypto `CryptoKey`s (non-extractable), and
  Go's `crypto/ecdh` and `crypto/mlkem` keys, which hold their secrets
  internally.

The JS ML-KEM selects in constant time but runs on a JIT, so it does not
promise constant time against a local timing attacker.

## Costs

Measured with Node 20 on the build box (Go is faster); the media
authentication costs are in its own section:

| What | Size or time |
| --- | --- |
| Presence | 1.9 KB of JSON |
| Key message to 15 people | 24 KB, 60 ms to build |
| Opening a key message | 11 ms |
| Presence rotation | 3 ms |
| Commit / reveal | about 400 / 210 bytes of JSON; one signature, one SHA-256 |
| Code for 16 people | under 1 ms (one SHA-256; v1 took 180 ms for 5200 SHA-512) |
| Auth messages | see "Media authentication" |

A join in a 16-person call moves about 16 × 24 KB of key messages through the
chat channel. Each one is a chat v3 frame whose body (about 32 KB once
base64'd) is sealed again to every device in the channel (about 2.2 KB per
device, up to 64). So a frame can reach about 175 KB: well under
`MAX_TEXT_BYTES` (4 MiB), but worth checking against the relay limits when
wiring ([chat-private-rooms.md](../app/chat-private-rooms.md)). A join costs
each participant one presence, one or two key messages, one commit and one
reveal: inside the 20-frames-per-10-s message budget
(`ROOM_MSG_MAX_PER_WINDOW`).

## Tests

```
cd server && GOTOOLCHAIN=local go vet ./rtc/e2ee/... && GOTOOLCHAIN=local go test -race ./rtc/e2ee/...
cd server && GOTOOLCHAIN=local go test ./rtc/e2ee/ -run '^$' -fuzz '^FuzzParseCommit$' -fuzztime 30s
   (and FuzzParsePresence, FuzzParseKeyMessage, FuzzSessionInput, FuzzFrameOpen, FuzzParseReveal,
    FuzzVerificationInput, FuzzParseAuthMessage, FuzzAcceptAuth, FuzzComputeSAS, FuzzParseQR)
cd server && GOTOOLCHAIN=local go test ./rtc/e2ee/ -run '^$' -bench . -benchmem
cd app && node --test tests/call_e2ee.test.mjs          (CALL_E2EE_BENCH=1 prints the JS costs)
python3 -m pytest -q app/tests/test_call_e2ee.py
```

- **FIPS 203.** `testdata/fips203-mlkem768.json` holds the ML-KEM-768 groups
  of NIST's ACVP vectors (ACVP-Server commit `975de31eb83d`,
  `ML-KEM-keyGen-FIPS203` and `ML-KEM-encapDecap-FIPS203`): 25 key
  generations, 25 encapsulations, 10 decapsulations (with implicit rejection
  of modified ciphertexts) and 10 checks each of encapsulation and
  decapsulation keys. The JS ML-KEM passes all of them. Go checks key
  generation and the encapsulation-key check, the only parts `crypto/mlkem`
  exposes with fixed inputs.
- **Go `crypto/mlkem`.** The JS ML-KEM reproduces Go's accumulated vector
  test (C2SP CCTV): 100 rounds by default, 10000 (80 s) with
  `CALL_E2EE_ACCUMULATED=10000`.
- **Cross-language.** Go wrote `go-vectors.json`
  (`go test ./rtc/e2ee -run TestGoVectors -update`) and JS wrote
  `js-vectors.json` (`CALL_E2EE_WRITE_JS_VECTORS=<path>`). Each language
  checks the other's file:
  - identities and presences (re-encoded byte for byte);
  - every wrap's shared secrets, HKDF info, KEK and nonce, and the
    unwrapped secret;
  - the frame keys, key ids (epoch 257 checks the wrap of the id) and media
    auth keys;
  - a full session that rebuilds the receiver from its recorded secrets and
    opens the key message;
  - a three-person verification round: every commit and reveal re-encoded
    and opened, and a session rebuilt from the recorded nonce that must
    produce the same commit and reveal bytes and the same code;
  - further codes (round id, transcript, emoji, digits, QR) for other rosters
    and generations;
  - the 64-entry emoji table;
  - frames sealed by the browser worker and by `FrameSender`, which must be
    byte-identical;
  - the auth message over those frames: byte-identical from Go's
    `FrameSender` and JS's `createMediaSigner` (Ed25519 is deterministic). It
    must verify every frame and catch a forged frame sealed under the same
    key;
  - reject cases with the expected error code: 13 presence, 13 key message,
    13 commit, 8 reveal and 11 auth message.

  `test_call_e2ee.py` also generates fresh vectors in each language, checks
  them in the other on every run, and checks that both stored files are v2.
- Session tests in both languages cover:
  - three-way calls, a split view, a leaver and a joiner;
  - rotation grace, replays, stolen peer ids, clock and call checks,
    monotonic key epochs, 16 people, and concurrent or interleaved use;
  - verification: reveal waits for every commit, re-committing after a reveal
    (`equivocation`), generation bumps and convergence without livelock,
    held early reveals, withheld reveals counted once, verified surviving
    leaves but not newcomers, the QR check;
  - media: every frame verified, late frames, a colluding participant's
    forgery before and after the auth message, missing auth turning
    unverified then dropping and recovering, one lost auth message not
    dropping, sender equivocation, window splitting at 100 frames and on a key
    change, auth from the wrong peer or call.
- Fuzzing (11 targets): every parser round-trips what it accepts; the
  session, verification and auth state machines never panic. Every opened
  frame ends up verified, forged or missing exactly once, and no code appears
  while members are still waiting.

# Group call end-to-end encryption (call e2ee v1)

Status: designed and built 2026-10-21, not yet wired into the live calls.
Idea 5 of "Beyond FaceTime" in [fm-media.md](fm-media.md): media keys come
from a hybrid X25519 + ML-KEM-768 exchange on every epoch, a new epoch on
every join and leave, and a short verification code every participant can
compare. The servers carry only ciphertext and headers.

| Part | Code |
| --- | --- |
| Go (call server tests, the FM Media engine core, the phones through gomobile) | `server/rtc/e2ee/` |
| Browser | `app/public/chat-call-e2ee.js`, with ML-KEM-768 from `app/public/chat-mlkem.js` |
| Frame encryption in the browser (unchanged, takes the new 32-byte keys) | `app/public/chat-group-call-crypto.js` |
| Tests | `server/rtc/e2ee/*_test.go`, `app/tests/call_e2ee.test.mjs`, `app/tests/test_call_e2ee.py` |
| Vectors | `server/rtc/e2ee/testdata/{go-vectors,js-vectors,fips203-mlkem768}.json` |

What it replaces: today ([chat-group-calls.md](../app/chat-group-calls.md),
"Frame encryption") each sender draws a random AES-128 key and sends the raw
key to the channel in a `group-call-key` frame. That frame is sealed by chat
protocol v3 ([chat-protocol-v3.md](../app/chat-protocol-v3.md)), so the
relay cannot read it. The key, though, goes to every device of the channel,
including members who are not in the call. It is wrapped under prekeys that
live up to 8 days, nothing ties a key to the call, and nobody can check who
holds it.

## Properties

| Property | How |
| --- | --- |
| No key material on the SFU | Presences and key messages travel in the chat channel's sealed frames, never over the call server's socket. The SFU forwards AES-GCM frames. |
| Post-quantum confidentiality | Every sender key is wrapped under X25519 and ML-KEM-768 together; breaking one of the two is not enough. |
| Forward secrecy | Presence key pairs are fresh per epoch and deleted 30 s after the next one; epoch secrets are wiped as soon as the frame key is derived; frame keys are dropped after three newer ones. |
| Post-compromise security | A participant's next presence rotation draws new key pairs from fresh randomness, and every sender then draws a new secret wrapped to them. |
| Only the people in the call | Keys are wrapped to each participant's own presence key, not to every device in the channel. A leaver is left out of the next epoch; a joiner never gets earlier secrets. |
| Authenticated senders | Presences and key messages are signed by the chat v3 device identity key; a peer id belongs to the first identity heard for it. |
| Verification | A 30-digit code over every participant's identity key and account name plus the call id. |

## Design decisions

**Identity: the chat v3 device identity key.** Every client (web, iOS,
Android, desktop) already has an Ed25519 identity key `IK` per installation
(`chat/e2ee/ed25519IdentityV3`), with `kid = id16(IK.pub)`. It is bound to
the account by the signed v3 bundle and kept in the platform secure store; in
the browser its seed sits in `localStorage` wrapped by a non-extractable
IndexedDB key. A separate per-call
key would add a second key to protect and a second thing to verify. Call
signatures use their own domain prefix, `forkmesh-call-e2ee-`. Chat v3
signs only strings starting `forkmesh-chat-e2ee-` or
`forkmesh-mesh-link-auth-v3`, so a call signature can never be mistaken for
a chat signature, and the reverse holds too. The signer refuses any
other prefix (`signerFromSeed`; the seal store's call signer must do the
same, see "Integration").

**Pairwise sender-key wraps, not TreeKEM.** Each sender wraps its own
32-byte epoch secret separately to each receiver's presence key.

- Cost at our size (16 people at most) is small: a rekey is 15 X25519 and 15
  ML-KEM encapsulations, about 60 ms and 24 KB in the browser, and a
  receiver does one decapsulation (11 ms).
- It needs no ordering. TreeKEM (MLS) needs one agreed sequence of commits.
  The chat relay does not order frames, two people joining at once would
  fork the tree, and resolving that needs a delivery service we do not have.
  Pairwise wraps let every sender rekey on its own schedule, which also
  matches the per-sender keys the SFrame-style frames already use.
- TreeKEM's log-size updates only pay off in large groups, and its
  post-compromise healing needs every member to update its path, the same
  round of work as here.
- Each wrap stands alone and can be checked in isolation (the cross-language
  vectors do exactly that).

**AES-256-GCM for frames.** ML-KEM-768 is NIST category 3 (about AES-192).
Leaving frames on AES-128 would make frames the weakest link against
Grover-style search. AES-256-GCM costs a few percent more CPU at call
bitrates. The frame byte layout does not change (see "Frames"), and the
browser worker already accepts 32-byte keys: `importKey("raw")` picks
AES-256 from the length (checked by the vector test, which seals the same
bytes in Go and in the worker).

**Frame keys derived per sender.** The frame key is HKDF over the sender's
random epoch secret with the call id, sender peer id, sender identity key and
epoch in the info. Two senders can never share a key, and a participant
cannot pass another sender's secret off as its own: under its own peer id it
derives a different key.

**X25519 in the browser comes from WebCrypto, with no JS fallback.**
WebCrypto X25519 and Ed25519 ship in current Chrome, Firefox and Safari.
`chat-seal.js` already requires both, so a browser without them cannot open
the private channel the call belongs to, and a JS fallback would add no
browsers.

**ML-KEM-768 in the browser is the existing `chat-mlkem.js`.** It is our own
FIPS 203 implementation (ported from the Android `MlKem768.kt`) and already
runs the encapsulation-key modulus check and the decapsulation-key hash check.
A second copy would mean two implementations to keep correct. The call tests
check it against NIST's ACVP vectors and Go's `crypto/mlkem`
(see "Tests").

**A 30-digit verification code.** See "Verification code" for why the code
is long.

## Encodings

- `b64u` is base64url without padding, decoded strictly: non-canonical
  trailing bits and padding are rejected in Go and JS alike.
- `H(x)` is SHA-256, `id16(x)` the first 16 characters of `b64u(H(x))`.
- Integers are JSON numbers that are exact safe integers (|n| < 2^53). Go
  parses them the way JavaScript does, so `1.0` is 1 on both, and rejects
  quoted numbers on both.
- Strings in signed messages are joined with `\n` (0x0A).
- `call` is 1 to 256 printable ASCII characters. `peer` is 1 to 64 of
  `[A-Za-z0-9_-]` (the app issues 32 hex characters). `acct` is 1 to 128
  code points with no C0 or C1 controls, no U+FFFD and no lone surrogates.
- JSON fields are matched case-sensitively; unknown fields are ignored.

## Presence

Each participant holds, per presence epoch `p` (1, 2, ...), an X25519 key
pair and an ML-KEM-768 key pair (generated from a 64-byte seed `d || z`), and
announces them:

```json
{"v":1,"call":"<call>","peer":"<peer>","acct":"<account>","kid":"<16>","idk":"<b64u 32>",
 "epoch":1,"ts":1790000000000,"x":"<b64u 32>","pq":"<b64u 1184>","sig":"<b64u 64>"}
```

`sig` is Ed25519 by `IK` over:

```
forkmesh-call-e2ee-presence-v1\n<call>\n<peer>\n<acct>\n<kid>\n<idk>\n<epoch>\n<ts>\n<x>\n<b64u(H(pq bytes))>
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

## Key messages (`group-call-key` v2)

For send epoch `e` (1, 2, ..., per peer id) a sender draws a 32-byte secret
`S`, a fresh X25519 key pair `eph` for this message, and for every receiver
`R` in the roster, using `R`'s newest presence (`R.epoch`, `R.x`, `R.pq`):

```
ss1      = X25519(eph.priv, R.x)                      (all zero: invalid)
ct, ss2  = ML-KEM-768.Encaps(R.pq)
salt     = H("forkmesh-call-e2ee-salt-v1\n" + call)
info     = "forkmesh-call-e2ee-wrap-v1\n" call \n S.peer \n b64u(S.idk) \n e \n
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
{"v":1,"call":"<call>","peer":"<sender peer>","kid":"<16>","idk":"<b64u>","epoch":1,"ts":0,
 "eph":"<b64u 32>","keys":[{"peer":"<receiver>","epoch":1,"ct":"<b64u 1088>","key":"<b64u 48>"}],
 "sig":"<b64u 64>"}
```

`keys` holds 1 to 31 entries sorted by peer (ASCII), without duplicates or the
sender. `sig` is Ed25519 by `IK` over

```
forkmesh-call-e2ee-keys-v1\n<call>\n<peer>\n<kid>\n<idk>\n<epoch>\n<ts>\n<eph>
```

followed, per entry in order, by `\n<peer>|<epoch>|<b64u(H(ct bytes))>|<key>`.

A receiver opens it when, in this order: the shape is valid (`invalid`); `call` matches (`call`);
it is not its own (`own`); the sender has an accepted presence (`unknown-peer`)
with the same `idk` (`peer-bound`); `ts` is within 5 minutes (`clock`); the
signature verifies (`signature`); `epoch` is above the last epoch opened from
that sender (`stale`, or `duplicate` for the same one); there is an entry for
it (`not-for-me`); it still holds the presence epoch the entry names
(`recipient-epoch`); and the unwrap succeeds (`invalid`). Then

```
frame key = HKDF-SHA256(ikm = S, salt, "forkmesh-call-e2ee-frame-v1\n" call \n S.peer \n b64u(S.idk) \n e, 32)
key id    = e mod 256
```

and `S` is wiped. Go and JS check in the same order and return the same error
codes (`ErrorCode` in Go, `CallE2eeError.code` in JS); the shared reject
vectors hold them to it.

## Epochs

| Event | Every participant |
| --- | --- |
| Joins (has its peer id) | Creates the session (presence epoch 1), announces it, rekeys once it has heard others |
| Hears a new participant (`joined`) | Rotates its presence, rekeys |
| Hears a new presence epoch (`updated`) | Rekeys |
| Somebody leaves (`out`) or expires | Rotates its presence, rekeys |
| Every 20 s | Re-sends its current presence (re-signed, new `ts`) |
| Every 10 minutes | Rotates its presence |
| `recipient-epoch` from a key message, or a sender's frames have an unknown key id 3 s after its presence | Rotates its presence (at most once per 10 s) |

Rekeys are debounced by 250 ms so a join, which makes everyone rotate,
costs each sender one or two key messages, not one per presence. A
rotation keeps the previous presence key pair for 30 s
(`PresenceGrace`, `Prune`), and at most two are kept, so a key message
wrapped just before a rotation still opens. A sender installs a new send key
1 s after sending the key message (the delay `chat-group-calls.js` already
uses on leave), and receivers keep three keys per sender, so frames from
the previous epoch still decode during the switch. A joiner sees nothing from
a sender until that sender's next key reaches it, about a second; this
is the price of not giving joiners the epoch in progress.

The receiver's 60 s timeout, the 20 s heartbeat and the 16-person cap are the
existing roster rules, unchanged.

## Frames

The frame layout is the one in [chat-group-calls.md](../app/chat-group-calls.md)
and [rtc.md](rtc.md): readable codec bytes (Opus 1; VP8 10 on a keyframe, 3
otherwise) as additional data, AES-GCM ciphertext with the 16-byte tag, an
8-byte big-endian counter, and a 1-byte key id. The IV is four zero bytes and
the counter. What changes with call e2ee v1:

- the key is 32 bytes (AES-256-GCM), derived as above;
- the key id is the sender's epoch mod 256;
- receivers install a sender's keys only in increasing epoch order
  (`OpenKeys` refuses older epochs; Go's `FrameReceiver.SetKey` also refuses
  a rollback).

Nothing in the frame says which scheme it uses: a receiver knows from how
the key arrived. A key from a v1 key message is 32 bytes; a legacy
`group-call-key` key is 16. `server/rtc/e2ee` has `FrameSender` and
`FrameReceiver` (one counter per sender guarded by a mutex, the 1024-frame
replay window moved only after the tag verifies, three keys per sender) for
the Go engine core. They produce exactly the bytes the browser worker
produces; the vectors check both ways.

## Verification code

```
E   = for each member sorted by (idk bytes, acct UTF-8 bytes), exact duplicates dropped:
      idk (32 bytes) || uint16_be(len(acct)) || acct
h   = SHA-512("forkmesh-call-e2ee-code-v1\n" + call + "\n" || E)
repeat 5199 times: h = SHA-512(h || E)
code = the first 30 bytes of h as six 5-byte big-endian numbers, each mod 100000, five digits each
```

Example (from `go-vectors.json`): `94120 42257 61327 99522 80836 55013`. Everyone in the call
(including the device itself) is a member. The code is the same on every
device exactly when every device sees the same set of (identity key, account)
pairs. It changes whenever that set changes, so on every join and leave of
someone else; a device that leaves and rejoins brings back the earlier code.
The account name is included, which the brief did not ask for, so a server
that labels one key as "bob" for Alice and "carol" for Dave also changes the
code. The iteration count matches the chat safety number.

Why 30 digits: the attack the code exists to catch is a split view. The
server shows Alice a fake Bob and shows Bob a fake Alice, then re-wraps
everything in the middle. To pass the check, it needs two fake keys whose
codes collide. Identity keys are long-lived and the call id is stable, so the
attacker can search for such a pair offline, and a birthday search costs
about 2^(bits/2) code evaluations. At 30 digits (99.7 bits) with 5200
SHA-512s per evaluation that is about 2^62 hashes, out of reach. Comparing
only the first group or two (17 to 33 bits) still catches any attacker who
did not grind against this call, but not one who did: users should compare
all six groups, or scan a QR code holding the whole thing. A commit-then-reveal round (as in ZRTP) would make a
four-digit code safe, but needs an ordering the chat relay does not give; it
is the next step if the long code proves too slow to read out.

The code shows that everybody sees the same participants. It does not show that a
participant's device is honest, and an extra device the server added for
an existing account shows up as an extra entry for that account. The
integration therefore shows every device in the roster and warns when one
account is in the call twice.

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
func (s *Session) Rekey() ([]byte, FrameKey, error)         // key message (nil when alone) and own send key
func (s *Session) OpenKeys(b []byte) (FrameKey, error)
func (s *Session) Code() (string, error)
func (s *Session) Peers() []string
func (s *Session) Close()
func ErrorCode(err error) string
type FrameKey struct { Peer string; Epoch uint32; KeyID byte; Key []byte }
func (s *FrameSender) SetKey(FrameKey) error; func (s *FrameSender) Seal(frame []byte, clear int) ([]byte, error)
func (r *FrameReceiver) SetKey(FrameKey) error; Open(peer string, frame []byte, clear int) ([]byte, error); Forget(peer string)
```

Messages go in and out as JSON bytes, so a gomobile wrapper only needs
`[]byte`, `string` and `int` at its boundary. The `Session` is safe for
concurrent use. The phones keep `Identity` in the keychain or Keystore; the
binding passes in the 32-byte seed and builds the key with
`ed25519.NewKeyFromSeed`.

JS (`app/public/chat-call-e2ee.js`, an ES module):

```js
const session = await createCallSession({ call, peer, acct, signer, now, random });
await session.presence(); await session.rotate();           // presence objects
await session.acceptPresence(obj);                            // { kind: "joined" | "updated" | "same", peer }
session.leave(peer); session.expire(); session.prune(); session.peers();
const { message, key } = await session.rekey();               // message object or null; key = { peer, epoch, keyId, raw }
const key = await session.openKeys(obj);
await session.code(); session.close();
```

`signer` is `{ idk: Uint8Array(32), sign(bytes) -> Promise<Uint8Array(64)> }`.
Methods run one at a time in call order. Errors are `CallE2eeError` with
`.code`. The low-level functions (`parsePresence`, `wrapFrameSecret`,
`unwrapFrameSecret`, `deriveFrameKey`, `verificationCode`, ...) are exported
for the vector tests.

## Integration (not done yet)

1. **Signer.** Add a `signCall(bytes)` method to `createRoomSealStore`
   (`chat-seal.js`). It loads the identity inside `ctx.exclusive`, refuses
   bytes that do not start with `forkmesh-call-e2ee-`, and returns the
   Ed25519 signature. The phones and the desktop add the same method to
   their keyring. `signer = { idk: b64u decode of store.identity().idk, sign: store.signCall }`.
2. **Session.** In `chat-group-calls.js`, after `joined` gives the peer id:
   `createCallSession({ call: <the channel's chat room name>, peer, acct:
   store.account(), signer })`. The call id is the chat room name, not the
   SFU room id, because the client knows it without asking the server; the
   chat v3 seal is already bound to it.
3. **Presence.** `group-call-presence` frames carry `e2ee: await
   session.presence()` (`rotate()` where the table above says so). On
   receipt, the presence's `idk` must equal the sealed frame's authenticated
   sender identity (`unsealFrame`'s sender `idk`) and its `acct` the sender's
   account; otherwise drop it. That check ties the call identity to the chat
   device the user may already have verified by safety number. Then
   `acceptPresence`, and on `joined`/`updated` schedule the debounced rekey.
4. **Keys.** `group-call-key` frames become `{peer, e2ee: message}`. On
   `openKeys` success post `{op: "key", peer, keyId, raw}` to the worker;
   after a rekey post `send-key` 1 s later. On `recipient-epoch` rotate
   (rate-limited).
5. **Leave.** `out` or a 60 s silence calls `leave`/`expire`, forgets the
   worker's keys for that peer, rotates and rekeys. Leaving the call calls
   `close()`.
6. **Code.** The call panel shows `session.code()` with every participant's
   name and device and "Verification code changed" when it does. iOS shows
   it first, and the change is registered in `docs/native/SYNC.md` for Android.
7. **No fallback.** Every client switches at once, as chat v3 did: a client
   that speaks call e2ee ignores `group-call-key` frames without `e2ee`, so
   nobody can be downgraded to raw keys.
8. **Native.** `native/webrtc/fm/fm_frame_crypto.h` has
   `kFrameKeyBytes = 16`; it must take 32-byte keys (AES-256-GCM) before the
   phones install v1 keys. The FM Media engine core uses `FrameSender` and
   `FrameReceiver` directly.

## Threat model and review

Adversaries: the call server (SFU); the chat relay and app server, which
deliver the sealed chat frames, issue peer ids and tickets, and run channel
membership; a participant; a network observer who records everything for a
future quantum computer; and a thief who gets a device's memory once.

**A malicious SFU.** It never sees a presence or a key message, since those
travel in the chat channel's v3-sealed frames. Even if they leaked, they
hold only public keys, KEM ciphertexts and signatures. What it can do: drop,
delay or reorder media; relabel one person's stream with another's peer id
(the receiver then holds the wrong key, the tag fails, nothing decodes);
replay frames, which the per-key replay window drops; and observe packet
sizes and timing, which show who is talking (audio frames are not padded;
that is a known limit). It cannot decrypt or forge a frame.

**A malicious chat relay or app server.** It cannot forge a presence
without the identity key. What it can do:

- Add its own device as a participant. That shows as an extra tile and
  changes the code.
- Show different rosters to different people. The codes then differ.
- Replay an old presence. The call id, the ±5 minute `ts` window, the
  per-join peer id and the monotonic epochs stop that, except for a
  presence from someone's own join in the last 5 minutes. That one adds a
  ghost tile with the same identity, which leaves the code unchanged and
  disappears after 60 s. Keys wrapped to the ghost are wrapped to a
  presence key that was deleted, so nobody can read them.
- Hand two people the same peer id. The first binding wins, the same
  denial of service as today.
- Withhold frames. That is a denial of service.

A private channel's v3 trust policy is "open", so the server can add a chat
device to the channel; the code and the roster are what catch it in a call.

**A malicious participant.** It holds every sender's frame key while it is
in the call; that is what being in the call means. What it cannot do:

- Take another peer id. The first binding wins, and every message is
  signed.
- Pass another sender's secret off as its own, since the frame keys are
  derived per sender.
- Read anything after it leaves. The next epoch is not wrapped to it, and it
  is out about 1 s plus the delivery time after it goes.

With a colluding SFU it could forge media that appears to come from another
participant, inside an epoch whose key it holds, because frame keys are
symmetric. Stopping that needs a signature on every frame (SFrame lists it
as an option), which costs about 64 bytes and one Ed25519 signature per
frame; not done. It can send different secrets to different receivers, which
only breaks its own media. It can craft an Ed25519 signature that one
platform accepts and another rejects (implementations differ on non-canonical
encodings). That splits the roster, and the codes differ.

**Replay of old presence.** A presence is bound to the call id and peer id
by its signature. It must be fresh (±5 minutes), its epoch must not go back,
the same epoch must carry the same keys (`equivocation` otherwise), and a
heartbeat's `ts` must not go back. A presence from another call is `call`.

**Downgrade from hybrid to classical.** There is no negotiation to downgrade.
Version 1 is the only version and needs both keys in every presence; the
signature covers `x` and `H(pq)`, and the KEK needs both shared secrets. A
presence without `pq` is `invalid` (a reject vector). Breaking X25519 alone
(a quantum computer) or ML-KEM alone (a cryptanalytic surprise) is not
enough: HKDF over `ss1 || ss2` with both exchanges in the info stays secret
while either holds. The legacy raw-key frames are ignored once v1 ships (step
7 above), and the chat transport under it is itself X25519 + ML-KEM. Ed25519
is not post-quantum. A future quantum computer could forge signatures, but
that helps only an active attack during a call; it does not open recordings.

**Key-id confusion.** The key id is only a selector (epoch mod 256). The
AES key itself is derived from the call, the sender's peer id and identity
and the epoch, so a frame opens only under exactly that sender's key for that
epoch. Receivers keep three keys per sender, install only newer epochs, and
start a new replay window for a new key. When an id comes round again after
256 epochs it selects the newest key, and a frame from the older epoch fails
its tag (`TestKeyIDWrapDoesNotConfuseEpochs`). A frame relabelled with
another peer's id fails its tag as well.

**Nonce reuse across epochs.** Every epoch has a fresh random 256-bit secret
and so a fresh key; the counter restarts only when the key changes, and
re-installing the same key keeps it running (Go `FrameSender`, the worker's
`send-key`). Audio and video share one sender and one counter. A rejoin gets
a new peer id and new secrets. Even a server that hands out the same peer id
again gets a different key, because the secret is random. Each wrap's
nonce comes from HKDF under a KEK used once.

**A lost or late epoch message.**

- A late key message still opens while the receiver holds the presence epoch
  it was wrapped to (current and previous, 30 s grace). One older than the
  last opened is `stale`.
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
- its identity key.

They do not get recordings older than the presence keys and frame keys the
device still held. Once the device rotates (at the latest 10 minutes later)
and senders rekey to it, a passive attacker learns nothing new. A stolen
identity key lets an active attacker sign as that device until the device
is blocked in chat; the code cannot show that, because the key is the same
one.

**What memory can and cannot be wiped.** Secrets in byte arrays (epoch
secrets, KEKs, shared secrets, ML-KEM decapsulation keys in JS, the seed and
X25519 bytes before import) are overwritten. WebCrypto `CryptoKey`s are
non-extractable and are released, not overwritten. Go's `crypto/ecdh` and
`crypto/mlkem` keys hold their secrets internally and are released, not
overwritten. The JS ML-KEM selects in constant time but runs on a JIT, so it
does not promise constant time against a local timing attacker.

## Costs

Measured with Node 20 on the build box (Go is faster):

| What | Size or time |
| --- | --- |
| Presence | 1.9 KB of JSON |
| Key message to 15 people | 24 KB, 60 ms to build |
| Opening a key message | 11 ms |
| Presence rotation | 3 ms |
| Code for 16 people | 180 ms (5200 SHA-512 calls) |

A join in a 16-person call moves about 16 × 24 KB of key messages through the
chat channel. Each one is a chat v3 frame whose body (about 32 KB once
base64'd) is sealed again to every device in the channel (about 2.2 KB per
device, up to 64), so a frame can reach about 175 KB: well under
`MAX_TEXT_BYTES` (4 MiB), but worth checking against the relay limits when
wiring ([chat-private-rooms.md](../app/chat-private-rooms.md)). A join costs
each participant one presence and one or two key messages, inside the
20-frames-per-10-s message budget (`ROOM_MSG_MAX_PER_WINDOW`).

## Tests

```
cd server && GOTOOLCHAIN=local go vet ./rtc/e2ee/... && GOTOOLCHAIN=local go test -race ./rtc/e2ee/...
cd server && GOTOOLCHAIN=local go test ./rtc/e2ee/ -run '^$' -fuzz '^FuzzParsePresence$' -fuzztime 30s   (and FuzzParseKeyMessage, FuzzSessionInput, FuzzFrameOpen, FuzzVerificationCode)
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
- **Cross-language.** `go-vectors.json` was written by Go
  (`go test ./rtc/e2ee -run TestGoVectors -update`) and `js-vectors.json` by
  JS (`CALL_E2EE_WRITE_JS_VECTORS=<path>`). Each language checks the other's
  file:
  - identities;
  - presences (re-encoded byte for byte);
  - every wrap's X25519 and ML-KEM shared secrets, HKDF info, KEK and
    nonce, and the unwrapped secret;
  - the frame keys and key ids (epoch 257 checks the wrap of the id);
  - a full session that rebuilds the receiver from its recorded secrets and
    opens the key message;
  - the verification codes;
  - frames sealed by the browser worker and by `FrameSender`, which must be
    byte-identical;
  - 22 reject cases with the expected error code.

  `test_call_e2ee.py` also generates fresh vectors in each language and
  checks them in the other on every run.
- Session tests in both languages cover three-way calls, a split view, a
  leaver and a joiner, rotation grace, replays, stolen peer ids, clock and
  call checks, monotonic key epochs, 16 people, and concurrent or
  interleaved use.

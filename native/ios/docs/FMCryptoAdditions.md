# FMCrypto additions: owner crypto and room seal

Two protocol files on top of the FMCrypto primitives. `import FMCrypto` (and `FMBase` for
`JSONObject`). Both are synchronous and thread-safe; ML-KEM and Keychain I/O take milliseconds, so
keep `loadOrCreate`/`seal`/`open` off the main thread on hot paths.

## OwnerCrypto (`OwnerCrypto.swift`)

This device's owner-encryption identity (X25519 + ML-KEM-768), byte-compatible with the Qt
MirrorCrypto and the relay's `validate_owner_envelope`. Conforms to `OwnerKeys`
(SecurityContracts.swift), which is what the rest of the app should depend on.

- `OwnerCrypto.loadOrCreate(secureStore:)` (key `ownerCrypto/hybridV1`, the JSON the retired Flutter app wrote; a corrupt
  blob is replaced by a fresh identity), `OwnerCrypto.forget(secureStore:)`, `OwnerCrypto.generate()`
  (not persisted).
- `keyId` (base64url SHA-256 of both public keys), `publicBundle: JSONObject` (`{v, x25519,
  mlkem768}` for POST /api/security/owner-keys), `x25519Public`, `mlkemPublic`.
- `OwnerCrypto.recipientKeyIds(envelope) -> [String]` (`[]` for bad framing; first = sealer),
  `isRecipientOf(envelope)`, `open(envelope) -> [UInt8]?`, `openJson(envelope) -> JSONObject?`
  (nil for anything unreadable, never distinguished by cause).
- `seal(bytes, to: [bundle]) throws -> JSONObject?` and `sealJson(object, to:)`; also static
  `OwnerCrypto.seal(_:to:)` (sealing needs no private key). nil for 0 or > 6 recipients, a bad
  bundle, or a duplicate key id; throws `CryptoArgumentError` for an ML-KEM key that fails the
  FIPS 203 modulus check. Include your own bundle to read the result back.
- `v` fields compare numerically (`1.0` counts as `1`).

## Chat protocol v3 (`ChatKeyring.swift`, `ChatTrust.swift`, `RoomSeal.swift`, `MeshLinkCrypto.swift`)

The wire format is [docs/app/chat-protocol-v3.md](../../../docs/app/chat-protocol-v3.md); this is
the Swift surface. v2 frames are ignored.

- `ChatKeyring`: the install's Ed25519 identity (`chat/e2ee/ed25519IdentityV3`, 32-byte seed,
  standard base64) and its signed prekeys (`chat/e2ee/prekeysV3`, a JSON list of
  `{id, xPriv, x, pqSeed, pq, ts, exp}`). `ChatKeyring.shared(store)` is one instance per
  `SecureStore` (rebuilt if the stored identity changes, as on a project switch), so every room
  sees the same prekeys and counters. `load(store, create: false)` never creates keys (the
  notification extension). `rotateIfNeeded(now:)` adds a prekey once the newest is 24 h old,
  wipes private halves past `exp` and keeps at most 10; `wipeExpired(now:)`. Counters
  (`chat/e2ee/ctrV3/<b64u(room)>`) and the 1024-wide seen window per sender
  (`chat/e2ee/seenV3/<b64u(room)>`, at most 256 senders a room) live here too.
- `ChatTrustStore` (`keyring.trust(scope)`): records `{kid, idk, acct, node, spk, state,
  firstSeen, verified, lastSeen}`, held frames (64 a room, at most 1 MiB of them persisted) and
  the last room name of the scope, at `chat/e2ee/trustV3/<b64u(scope)>`. `approve`, `block`,
  `markVerified`, `verify(scanned:)`, `acknowledgeSafetyChange`, `safetyNumber()`
  (`ChatSafetyNumber`: `digits`, `groups`, `qr`, `verified`, `changed`). `changed` is set when the
  approved set changes after it already held two accounts, which also clears `verified`.
- `RoomSeal(room:scope:policy:acct:node:participants:keyring:store:)` or
  `RoomSeal.loadOrCreate(room, scope:, policy:, acct:, node:, participants:, secureStore:)`.
  `acct` is the account (trust), `node` the per-device `senderId` (forgery check);
  `participants` drops DM bundles naming anyone else. `keyFrame(now:)` (the bundle, and this
  device's own record), `acceptBundle(frame, now:) -> ChatBundleResult?`,
  `needsAdvertisement(now:)` (the prekey rotated since the last `keyFrame`),
  `shouldPersistAdvertisement(now:)`, `seal(plain)`, `seal(plain, for: [kid])`,
  `seal(plain, forNodes:)` (relay `@peer` DMs), `open(frame, now:, history:) -> RoomSealOpen`
  (`opened(plain, senderKid, senderAcct, senderNode)`, `held(kid)`, `notForMe`, `replay`,
  `invalid`; `name` gives the spec's string), `reopenHeld(now:)`, `approve`, `block`,
  `safetyNumber()`. `authenticatedPlain` is the opened plaintext with `senderId` set to the signed
  `node`, which every author check downstream relies on. `unseal(frame)` is a read-only `peek`
  (no counters, no holding) for tests. A frame from an unknown sender is held only when it is
  addressed to this device. An open room with more than 64 devices seals to the 63 most recently
  seen.
- `MeshLinkCrypto` and `MeshLinkHandshake`: the `fm-link-hello` / `fm-link-auth` exchange, the
  transcript, per-direction keys and the counter-nonce frames of a mesh link.
- `OverlaySecureStore`: reads through to a store and keeps every write in memory. The push
  opener wraps the shared keychain in one, so opening a frame in the extension never moves the
  app's counters, seen windows or ratchet sessions; the push opener also opens v4 frames with
  `commit: false`.

## Chat protocol v4 (`ChatRatchet.swift`, `RoomSealV4.swift`)

The wire format is [docs/app/chat-protocol-v4.md](../../../docs/app/chat-protocol-v4.md), a
per-message Double Ratchet with ML-KEM-768 in every step, for strict rooms only (server DMs and
their `direct-sealed` mesh copies). `tools/chat_v4_reference.py` is the byte-level reference.

- `RoomSeal.version`: 4 for `policy: .strict`, 3 otherwise; `version:` in the initializer overrides
  it (the v3 vector tests pin 3). A strict room seals and opens only v4 (a v3 frame is `invalid`);
  an open room opens only v3 (a v4 frame is `invalid`). `isSealedFrame` accepts both versions (v4
  with 0 to 64 entries), `frameVersion(frame)` tells them apart.
- `seal`, `seal(plain, for:)` and `seal(plain, forNodes:)` in a v4 room wrap the content key for
  each approved device other than this one with that device's ratchet session; no self entry, and
  a frame with no recipients is legal (it is still sent and resealed later).
- `open(frame, now:, history:, commit:)` adds two results: `own` (this device sent it) and
  `noSession(kid)` (no session, a prekey no longer kept, more than 256 skipped, or a wrap that
  fails). v4 has no history-replay exception. With `commit: false` nothing is stored (no ratchet,
  seen window, trust or held frame), so the same frame opens again later; `peek` is
  `open(commit: false)`. `reopenHeld` goes through the ratchet. `resetSessions(with: kid)` drops
  this room's sessions with that device; `highestCounter(from: kid)` is the highest `ctr` opened
  from it in this room (the client skips recovery for an older `noSession` frame).
- `ChatV4`: constants (`maxSkip` 256, `skippedKept` 512, `skippedLifeMs` 8 days, `sessionsKept`
  4, `resetType` `e2ee-reset`), `kdfInit`, `kdfRk`, `kdfCk`, `sessionId`.
- State lives in `ChatKeyring`, one entry per room at `chat/e2ee/ratchetV4/<b64u(room)>`
  (`ChatKeyring.ratchetStoreKey`) in the spec's canonical JSON, next to the prekeys and trust in
  the shared keychain group. A seal stores it before it returns the frame. Seal and open of v4
  frames run under one ratchet lock per keyring, so two rooms or tasks never interleave a session
  update. `ratchetPeer(room, kid)`, `resetRatchet`, `exportRatchets` and `importRatchets` (the
  vector `states` import as is). Sessions are kept per peer (the current one plus the most
  recently used, 4 at most); skipped keys older than 8 days or past 512 are dropped whenever the
  session is stored.

Tests: `Tests/FMCryptoTests/OwnerCryptoTests.swift` (the Qt interop case skips unless
`desktop/build/forkmesh-tests` is built) and `RoomSealTests.swift`: `RoomSealVectorTests` loads
`conformance/chat/v3/vectors.json` and checks what `app/tests/test_chat_v3_vectors.py` checks
(including that ML-KEM keys from the stored seeds and the link frames match byte for byte);
`RoomSealRoundTripTests` covers sealing, strict trust, persistence and rotation.
`ChatRatchetTests.swift`: `ChatRatchetVectorTests` loads `conformance/chat/v4/vectors.json` and
checks the `kdf` vectors, replays the `sequence` as `app/tests/test_chat_v4_vectors.py` does (a
fresh device from `devices` and the `bundles` per `state` step, `states` imported, `clearSeen` by
deleting the seen window and reloading), checks `bobAfterM1` and that the `states` export
unchanged; `ChatRatchetRoundTripTests` covers shuffled multi-turn delivery, a late message from an
old chain, simultaneous first messages, reset recovery, `commit: false` and the extension overlay,
`MAX_SKIP`, expired prekeys, bounded sessions, skipped-key expiry, own and zero-entry frames, held
frames and reload. All of it runs on Linux.

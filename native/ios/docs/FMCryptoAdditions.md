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
  app's counters or seen windows.

Tests: `Tests/FMCryptoTests/OwnerCryptoTests.swift` (the Qt interop case skips unless
`desktop/build/forkmesh-tests` is built) and `RoomSealTests.swift`: `RoomSealVectorTests` loads
`conformance/chat/v3/vectors.json` and checks what `app/tests/test_chat_v3_vectors.py` checks
(including that ML-KEM keys from the stored seeds and the link frames match byte for byte);
`RoomSealRoundTripTests` covers sealing, strict trust, persistence and rotation.

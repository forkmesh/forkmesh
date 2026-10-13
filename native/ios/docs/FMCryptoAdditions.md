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

## RoomSeal (`RoomSeal.swift`)

Automatic device-to-device chat E2EE inside the room cipher: a persistent X25519 key per install,
a fresh AES-256-GCM content key per frame wrapped for every known room device.

- `RoomSeal.loadOrCreate(room, storageScope:, secureStore:) throws` (key `chat/e2ee/x25519PrivateV2`,
  standard base64 of the clamped key; peers under `chat/e2ee/peersV2/<b64url(scope\nroom)>`;
  scope defaults to the room). Throws `CryptoArgumentError` for an
  empty room. `RoomSeal.fromPrivateKey(room, key32, storageScope:, secureStore:)` for vectors.
- `keyId` (16 chars), `peerCount`, `keyFrame() -> JSONObject`, `acceptKeyFrame(frame) -> Bool`
  (true when new; persisted), `seal(plain) throws -> JSONObject` (sealed to every peer and to
  itself), `unseal(frame) -> JSONObject?` (a verified unknown sender becomes a peer),
  `shouldPersistAdvertisement(now:)` (once a day per scope and room).
- Static `isKeyFrame`, `isSealedFrame`, `frameKeyId`.
- At most 63 peers; the oldest is evicted. Wire nonces/tags/bodies are standard padded base64,
  `pub` is unpadded base64url, exactly as the browser and Qt clients.

Tests: `Tests/FMCryptoTests/OwnerCryptoTests.swift` (the Qt interop case skips unless
`desktop/build/forkmesh-tests` is built) and `RoomSealTests.swift` (the browser v2 wire vector and
the Kotlin storage cases).

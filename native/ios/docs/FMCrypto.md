# FMCrypto: primitives

`import FMCrypto` (and `FMBase` for `JSONObject`, `B64`, `SecureStore`). The
module holds every cryptographic primitive the app uses, in pure Swift over
fixed-width limbs, with no dependency beyond Foundation. The protocol files
built on top of it (`OwnerCrypto`, `RoomSeal`) are described in
[FMCryptoAdditions.md](FMCryptoAdditions.md).

Where one exists, each source file names the Kotlin code in `native/android/core`
it follows.

## Primitives

| Type | File | What it is |
| --- | --- | --- |
| `AesGcm` | `AesGcm.swift` | AES-256-GCM, 12-byte nonce, 16-byte tag. `encrypt` returns a `SecretBox` (`cipherText`, `nonce`, `mac`); `decrypt` throws `AuthenticationError`; `decryptOrNull`. `encryptPortable`/`decryptPortable` are the non-CryptoKit path. |
| `Argon2id` | `Argon2id.swift` | RFC 9106 Argon2id v0x13, `deriveKey(password:salt:parallelism:memoryKiB:iterations:hashLength:...)`. Used to stretch the vault master password. Pure Swift on every platform. |
| `Blake2b` | `Blake2b.swift` | RFC 7693 BLAKE2b, unkeyed; the hash inside Argon2id. |
| `CryptoBytes` | `CryptoBytes.swift` | `random`, `concat`, `constantTimeEquals`, `isAllZero`, `wipe`, `hex`, `fromHex`. |
| `Ed25519` | `Ed25519.swift` | RFC 8032: `generateSeed`, `publicKey(seed:)`, `sign`, `verify`, `verifyStrict`, `isCanonicalScalar`. |
| `Field25519` | `Field25519.swift` | GF(2^255 - 19) arithmetic in the ref10 limb layout; internal to Ed25519/X25519. |
| `HmacSha256`, `Hkdf`, `Pbkdf2` | `Kdf.swift` | HMAC-SHA256, HKDF-SHA256, PBKDF2-HMAC-SHA256 (each with a `...Portable` twin). |
| `Keccak` | `Keccak.swift` | SHA3-256, SHA3-512, SHAKE128, SHAKE256 (FIPS 202). |
| `MlKem768` | `MlKem768.swift` | FIPS 203 ML-KEM-768: `generateKeyPair`, `encapsulate`, `decapsulate`. Interoperable with OpenSSL, Go and the desktop's `MirrorCrypto`. |
| `Sha256`, `Sha512` | `Sha2.swift` | SHA-2 (CryptoKit when available, `hashPortable` otherwise). |
| `X25519` | `X25519.swift` | RFC 7748: `clampPrivateKey`, `generatePrivateKey`, `publicKey`, `sharedSecret`, `scalarMult`. Constant-time ladder. |

## Identity and envelopes

| Type | File | What it is |
| --- | --- | --- |
| `DeviceIdentity`, `OwnerKeys` | `SecurityContracts.swift` | Protocols the rest of the app depends on, so auth, API, inbox, agents and chat test against fakes. `nodeId` is the base64url public key. |
| `Identity` | `Identity.swift` | This install's Ed25519 identity, stored in the secure store under `identity/ed25519Seed`. Matches the desktop's `ForkMeshIdentity` and the Worker's `ed25519_verify`. `loadOrCreate`, `fromSeed`, `sign`, `rotate`, `clearStoredSeed`. |
| `DeployApproval` | `DeployApproval.swift` | The merge-as-deploy-approval signature (`app/src/deploy_approvals.py`). `message` builds the nine `\n`-joined lines (`forkmesh-deploy-approval-v1`, kind, owner lowercased, repo, target, base, head, requestId, ts in ms; no trailing newline); `sign` returns the `{publicKey, ts, signature}` body field, both base64url without padding, signed with the node's existing identity key. `DeployApprovalTests` pins the server's golden vectors (seed `00..1f`). |
| `PortableIdentity` | `PortableIdentity.swift` | The encrypted identity export shared with the web (`app/public/portable-identity.js`) and the desktop; format in `www/docs/portable-identities.md`. |
| `RoomCrypto` | `RoomCrypto.swift` | The relay-derived room cipher, byte-compatible with the desktop's `RoomCrypto.cpp` and `app/public/chat-crypto.js`. |

## Rules

- **Ed25519 never uses CryptoKit.** CryptoKit's `Curve25519.Signing`
  signatures are randomised; the Kotlin client produces the
  deterministic RFC 8032 signatures, and signature vectors are pinned across
  clients. Verification stays here too, so the acceptance rules (canonical S,
  canonical encodings, the strict subgroup check) are identical everywhere.
- CryptoKit is used only where its output is byte-identical (SHA-2, AES-GCM,
  HMAC/HKDF/PBKDF2, and X25519 for canonical peer keys) and only under
  `#if canImport(CryptoKit)`; the `...Portable` path is what Linux tests
  exercise. `X25519.scalarMult` explains which inputs stay on the Swift ladder.
- Every primitive stays synchronous. Argon2id and ML-KEM take milliseconds or
  more: keep them off the main thread.

## Tests

`Tests/FMCryptoTests` carries the cross-client and Kotlin vectors (ML-KEM KATs in
`MlKem768Kat.swift`):

```sh
cd native/ios/ForkMeshCore
FM_ONLY=FMCrypto swift test
```

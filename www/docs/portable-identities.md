# Portable identities and recovery

Keys, the recovery kit and authenticators stay with the user; ForkMesh stores
only public authority state. Never send keys or kits to support.

## Enroll and revoke authenticators

Use a passkey for sign-in plus a hardware key and a second device. Revoking a
device kills its key and sessions; revoked keys only verify old signatures.
Delegates get scoped, expiring authority.

## Recovery drills

Practise each before relying on an identity: lost device, compromised device
(revoke first), key rotation (old key revoked atomically), social recovery
(threshold of contacts, who never hold the key) and organization delegation
(minimal scope, expiry).

## Portable encrypted export

The kit is JSON of type `forkmesh.portable-identity`, `schemaVersion` 1:
PBKDF2-HMAC-SHA-256 (600000 iterations, 16-byte salt) and AES-256-GCM (12-byte
IV, tag appended, associated data `forkmesh.portable-identity/v1`) over at most
128 KiB of plaintext, all base64url without padding. Passkey and hardware-key
secrets are never exported. Web, iOS, Android and desktop share the format.

## External-account proofs

Signed short-lived challenges with `private`, `members` or `public` visibility;
revocable, and shown unverified once revoked or expired.

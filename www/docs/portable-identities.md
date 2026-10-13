# Portable identities and recovery

ForkMesh identity is deliberately not a hosting account. Your active signing
keys, recovery kit, and approved authenticators remain under your control;
ForkMesh records only the public authority state needed to reject revoked or
expired authority. Do not send a private key, passphrase, recovery code, or
recovery-kit file to support.

## Enroll and revoke authenticators

Use a passkey for routine sign-in and keep at least one hardware key and one
separate enrolled device before relying on a device for work. Name each device
so it can be recognized later. The Devices view lets an authenticated owner
revoke a lost or compromised device; it immediately disables the key, revokes
sessions it opened, and retains the revocation record. That key cannot be
enrolled again—generate a fresh keypair on the replacement device.
Revoked keys remain useful only for verifying the historical signatures that
already name them; they have no authority to create accepted new events.

An authenticator is not an all-purpose signing authority. Passkeys and
hardware keys approve enrollment, recovery, and sensitive changes. A device
key receives only the capabilities it needs. An organization delegate receives
an explicit scope, expiry, and a limited signing authority rather than an
owner's private key.

## Recovery drills

Run these drills before storing important work under an identity:

| Drill | Safe outcome |
| --- | --- |
| Lost device | On another enrolled authenticator, revoke the lost device; restore the encrypted kit to a new device and enroll its fresh public key. |
| Compromised device | Revoke the device first, rotate every authority it held, and review its sessions and delegated grants. Do not use the device to approve its own replacement. |
| Key rotation | Have the current key sign the successor-key record. The old device record is revoked atomically with the rotation, so it cannot sign new accepted events. Historical records remain verifiable against their embedded public keys. |
| Social recovery | Require the configured threshold of distinct recovery contacts, check their signed approvals and expiry, then enroll a fresh key. Recovery contacts never receive the account's private key. |
| Organization delegation | Grant a distinct delegate key only the repository/action scopes required, set an expiry, and test a denied request outside that scope. Revoke the grant as soon as the work ends. |

Treat a missing recovery contact or unavailable passkey as an expected failure:
use another configured factor, or wait for the recovery policy's threshold.
Never lower a threshold during an incident. Record the recovery and revocation
in the security audit trail.

## Portable encrypted export

The offline recovery kit is a provider-neutral JSON file. It is encrypted
locally; the relay is neither an escrow holder nor a recipient of its
passphrase. It can contain local signing material, public credential metadata,
recovery-policy references, and public proof records. It must not contain a
passkey private key or a hardware-key private key: those are intentionally
non-exportable.

The web client (`app/public/portable-identity.js`), iOS client
(`native/ios/ForkMeshCore/Sources/FMCrypto/PortableIdentity.swift`), Android
client (`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/PortableIdentity.kt`),
and Qt desktop client
(`ForkMeshIdentity`) read and write version 1 of this format. A conforming
third-party client need only implement the following standard envelope:

```json
{
  "type": "forkmesh.portable-identity",
  "schemaVersion": 1,
  "createdAt": 1783000000000,
  "kdf": {
    "name": "PBKDF2-HMAC-SHA-256",
    "iterations": 600000,
    "salt": "base64url-without-padding-16-bytes"
  },
  "encryption": {
    "name": "AES-256-GCM",
    "iv": "base64url-without-padding-12-bytes",
    "ciphertext": "base64url-without-padding-ciphertext-followed-by-16-byte-tag"
  }
}
```

The plaintext is UTF-8 JSON (maximum 128 KiB). Derive a 256-bit key using the
given passphrase, salt, and PBKDF2 parameters. Encrypt/decrypt with AES-256-GCM
and authenticated data exactly equal to UTF-8
`forkmesh.portable-identity/v1`; WebCrypto's ciphertext includes the 16-byte
tag at its end. Reject non-canonical base64url, wrong lengths, unknown versions,
wrong authenticated data, and decryption failures. The web and mobile clients
use this exact layout, making a kit created on either client recoverable by the
other without a ForkMesh-hosted conversion.

Keep two encrypted copies under separate physical control, test one on a spare
device, and update it after a key rotation, recovery-policy change, or proof
revocation. A recovery kit is sensitive even when encrypted: use a unique,
long passphrase and never place it in source control, email, chat, or a shared
drive without independent access controls.

## External-account proofs

Link an external account by signing the exact short-lived challenge from that
account and storing a proof with its provider, subject, issued time, and
visibility (`private`, `members`, or `public`). A proof is revocable and must
be shown as unverified after revocation or expiry. Public profiles expose only
the explicitly public label; provider identifiers and proof material remain
private by default. Automated evidence must not treat a display name or avatar
as an external-account proof.

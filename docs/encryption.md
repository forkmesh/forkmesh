# Encryption and signatures

What ForkMesh signs, what it encrypts, with which algorithms, and what each
layer does not protect. Each section links the page with the full
specification.

Part of the core documentation: [architecture](architecture.md) ·
[encryption](encryption.md) · [mirroring and distribution](mirroring.md) ·
[self-hosting](self-hosting.md) · [censorship resilience](censorship-resilience.md)
· [threat model](protocol/threat-model.md).

## At a glance

| What | Protection | Algorithms | Spec |
| --- | --- | --- | --- |
| Node and device identity | Signing key that never leaves the device | Ed25519 | [portable identities](portable-identities.md) |
| Issues, pull requests, reviews, releases | Signed records stored in the repository | Ed25519 over domain-separated canonical bytes | [signatures and authentication](protocol/signatures-and-authentication.md) |
| Requests from nodes (push, view, key rotation) | Time-bound signed proofs | Ed25519, 5-minute window, body digest bound | same |
| Repository integrity on mirrors | Signed refs digests; clones go only to a mirror whose refs match | SHA-256, Ed25519 | [mirroring and distribution](mirroring.md) |
| Chat in channels | Device-to-device sealing inside a room cipher | X25519 + ML-KEM-768 hybrid, HKDF-SHA256, AES-256-GCM | [chat encryption](security/sealed-chat.md) |
| Direct messages | The above plus a per-message ratchet and safety numbers | same | same |
| Group calls | Per-epoch media keys, signed frame digests, short verification code | X25519 + ML-KEM-768, AES-256-GCM, Ed25519 | `docs/server/call-e2ee.md` |
| Private repositories | Sealed on an authorized device before replication | AES-256-GCM content key wrapped to X25519 + ML-KEM-768 recipients | [private repository confidentiality](security/private-repository-confidentiality.md) |
| Stored service data | Row encryption and keyed lookups on the cores | AES-256-GCM under `DATA_KEY`; HMAC-SHA256 blind indexes | `docs/operations/rekey.md` |
| Service secrets | Sealed vault; nodes hold the key only in memory | HKDF-SHA256, AES-256-GCM with per-row AAD | `docs/operations/vault.md` |
| Releases and installers | Signed manifest checked before install or update | Ed25519 key pinned in the installer and desktop | `docs/release/README.md` |
| Transport | HTTPS everywhere; WireGuard between cores | TLS (ACME certificates), WireGuard | [architecture](architecture.md) |
| Database backups | Pulled over SSH, verified, then stored only encrypted | age | `docs/operations/README.md` |
| Passwords | Salted hash; TOTP required for administrators | PBKDF2-SHA256 | `docs/app/auth-sessions.md` |

## Identity and signed records

Every desktop node, mirror and device has an Ed25519 key. Records are signed
over a frozen, scheme-specific prefix plus fields the verifier rebuilds itself,
so a signature for one scheme, version or HTTP method never verifies as
another. Verifiers reject malleable signatures, non-canonical points and
small-order keys (FM-SIG-001). A collaboration event binds its repository,
object and policy inside the signed bytes, so it cannot be replayed into
another repository. Because the records live under `.forkmesh/` in Git, anyone
with a clone can verify the project's history without trusting the server that
served it.

Key rotation is signed by the old key (`forkmesh-rotate-v1`). Recovery of a lost
key still relies on account credentials and the recovery kit; a normative,
independently verifiable recovery protocol is open work (TM-018 in the
[threat model](protocol/threat-model.md)).

## End-to-end encrypted chat and calls

Each device keeps an Ed25519 identity key and rotating prekeys, each an X25519
key paired with an ML-KEM-768 key, so a recording made today cannot be opened by
a future quantum computer. A message gets a fresh AES-256-GCM key, wrapped for
each approved device with a hybrid X25519 + ML-KEM-768 exchange and
HKDF-SHA256; the whole frame is signed and carries a replay counter. Direct
messages add a per-message ratchet like Signal's Double Ratchet, hold new
devices until the other person approves them, and show a 60-digit safety
number. Group calls derive media keys per epoch (a new epoch on every join or
leave), sign batches of frame digests so one participant cannot forge another's
video, and use a commit-then-reveal round so a short code is safe to compare.

What it does not hide: the server still sees who talks to whom, when and how
much, and public channels are readable by anyone who joins, including the
server. Details and limits: [chat encryption](security/sealed-chat.md),
[safety numbers](chat-safety-numbers.md), [calls](calls.md).

## Private repositories

Repository objects are sealed on an authorized device into
`forkmesh.private-replica` ciphertext: a fresh AES-256-GCM content key per
update, wrapped to every recipient's X25519 and ML-KEM-768 keys. Mirror and
storage operators hold no recipient key and see only opaque identifiers, sizes
and timestamps. Removing a collaborator reseals to the new recipient set under
a new key epoch; copies they already hold cannot be erased. A missing recipient
key never falls back to plaintext. `tools/audit_private_surfaces.py` checks a
database export and replica files for leaks without any key.
Full contract: [private repository confidentiality](security/private-repository-confidentiality.md).

## Data on the service

On the cores, collaboration rows and account data are encrypted with
AES-256-GCM under the instance `DATA_KEY`, and equality lookups use HMAC-SHA256
blind indexes derived from it. `DATA_KEY` is never written to a core's disk: a
node starts sealed and receives the key into memory from the operator or an
unsealed peer, and service secrets are sealed under it in the vault. The key can
be rotated offline (`docs/operations/rekey.md`). An operator holding the running service and
`DATA_KEY` is inside this boundary; someone holding only a disk, a database copy,
a log or a backup is not. What the service keeps and for how long:
[relay data retention](relay-data-retention.md).

## Software distribution

Every release has a `release.json` manifest listing each file's SHA-256, signed
with one Ed25519 release key. `install.sh` and the desktop's updater pin that
key and refuse an unsigned or mismatched download, whichever mirror served it.
A checksum taken from the fetched manifest alone is never trusted. See
[mirroring and distribution](mirroring.md#software-distribution).

## Known gaps

- An independent cryptographic review is a release requirement for private
  repositories and has not been published yet.
- Open protocol threats, including legacy record replay (TM-004), unsigned
  catalog v1 fields (TM-006), event withholding (TM-008) and key recovery
  (TM-018), are tracked in the [threat model](protocol/threat-model.md).
- Request proofs carry no nonce; a captured proof can be replayed inside its
  5-minute window (TM-012). Push proofs bind the body digest, so a replay can
  only resend the identical pack.
- Passwords use PBKDF2-SHA256 at 100,000 iterations, below current guidance for
  that function.
- Channels have per-prekey forward secrecy (up to 8 days), not per-message.
- A modified web client served by a compromised host can read what it shows;
  the desktop and phone apps ship as signed builds instead.

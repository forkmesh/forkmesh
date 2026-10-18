# Secure Drive (suite `fmsd1`)

Package: `marketplace/packages/secure-drive/` (desktop rail page). The
user-facing description, threat table and limits are in the package
[README](../../marketplace/packages/secure-drive/README.md); this page holds the
format and the reasons behind it.

## Why it is built this way

- **No server.** The shared medium is a dumb folder (a router's SMB share, a
  NAS, a USB stick). It can store and delete files and nothing else, so every
  rule (who may read, who may write, what happened) is enforced by
  cryptography on the computers, and every computer verifies everything it
  reads.
- **Each computer writes only its own objects.** A network share has no
  locking worth trusting, so there is no shared index file. Each computer
  appends to its own log segments and writes immutable, randomly named blobs.
  The drive's state is the fold of every computer's records, ordered by
  Lamport clock, then device id, then sequence. Two saves of the same file
  never overwrite each other; the later one wins and both versions stay.
- **Approval, not just a code.** An invitation code alone would hand the drive
  key to whoever reads it first, including someone watching the share. The code
  only lets a computer post a join request the members can read, and lets the
  approver prove to the newcomer that the key came from a member. The key moves
  only after a person compares safety numbers.
- **Removal re-encrypts.** Every blob key sits in log records encrypted under
  the drive key, so a removed computer that kept the old drive key could
  otherwise read files saved later under old blob keys. Removal therefore makes
  a new drive key, seals it to the remaining computers, and re-encrypts every
  version under fresh blob keys recorded only under the new key.
- **Plaintext stays in memory for built-in types.** Documents, spreadsheets
  and drawings use formats other software opens (HTML, CSV, SVG) so an
  exported copy is useful, but the editors work on in-memory buffers.

## Objects

Every object is `<32 hex>.fmsd` in the drive folder, starting with `FMSD`,
version byte `1` and a kind byte. Anything else in the folder is counted and
reported, never read.

| Kind | Bytes after the 6-byte header | Readable by |
| --- | --- | --- |
| 1 keyring | X25519 ephemeral (32) · ML-KEM-768 ciphertext (1088) · nonce (12) · AES-GCM | the one computer it is sealed to |
| 2 join request | nonce (12) · AES-GCM under `HKDF(code, "request")` | members, through the invitation record |
| 3 log segment | key id (8) · nonce (12) · AES-GCM under `HKDF(drive key, "log")` | members holding that drive key |
| 4 blob | nonce prefix (8) · chunks of 1 MiB plaintext + 16-byte tag | whoever holds the version's blob key |

Associated data binds each object to the drive id and its own file name, and
each blob chunk to its index and to whether it is the last chunk, so objects
cannot be moved, reordered, truncated or swapped between drives. Blob
plaintext is padded (Padmé, minimum 4 KiB) and the true length lives in the
encrypted record.

## Keys

- **This computer:** X25519, an ML-KEM-768 seed and Ed25519, plus every drive
  membership (drive id, root device, drive keys by id, chain head, the heads it
  has seen, pending join secrets), in `device.json` under the local data root,
  sealed with AES-256-GCM under Argon2id of the passphrase. The Argon2id floor
  (19 MiB, 2 passes) is enforced on load, as in the Password Vault.
- **Drive keys** are 32 random bytes identified by `HKDF(key, "key-id")[0..8]`.
  A computer writes with the highest-numbered key it holds. Keyrings carry all
  of them, so a newcomer reads the whole history.
- **Keyring payload:** drive id, recipient, epochs, root device and its Ed25519
  key, issuer id and public keys, time, and for a join the invitation id and
  `mac = HMAC(HKDF(code, "approve"), payload)`, all signed by the issuer. A
  joining computer trusts the first keyring because of the MAC (only someone
  who saw the code and the member-only invitation record can make it); later
  keyrings must be signed by a computer on the verified roster.

## Records

A record is `{d device, s seq, l lamport, t time, a action, x data, p prev
hash, w witness, sig}`. The signature covers
`"forkmesh-secure-drive/fmsd1/record\n" + drive id + "\n" + compact JSON`
without `sig`; the hash is SHA-256 of those bytes plus the signature. `w` maps
every other computer to the `[seq, hash]` of its newest record this computer
had seen.

Verification on every scan:

1. Open keyrings addressed to this computer; collect drive keys.
2. Decrypt log segments; group records by device and sequence. Two different
   records with one sequence number are `log_forked`.
3. Build the roster from the root (pinned when this computer joined): verify
   each trusted device's chain (signature, sequence gaps as `records_missing`,
   `prev` links as `chain_broken`), and trust devices its `device_added`
   records introduce. Chains from anyone else are `unknown_device`.
4. Check keyring issuers against the roster; drop keys from unsigned keyrings
   and decrypt again without them.
5. Fold the records. A `device_removed` record carries `upTo`, the removed
   computer's newest sequence the remover had seen; anything later from it is
   `after_removal`. (A removed computer cannot read new records, so its Lamport
   clock stays low; the cutoff has to be by sequence.)
6. Compare every witness and this computer's remembered heads with the chains:
   missing records are `records_missing` or `rolled_back`, different hashes
   `log_forked`. This computer's own chain shorter than it remembers is
   `own_records_missing`.

A deletion is therefore reported as soon as any computer that saw the record
(including the one that wrote it) checks the drive. A record nobody but its
writer saw, deleted while the writer is away, is reported when the writer
comes back.

Actions: `drive_created`, `drive_opened`, `device_added`, `device_removed`,
`invite_created`, `invite_used`, `invite_revoked`, `join_denied`,
`file_created`, `file_saved` (with `base`, to count conflicts),
`file_restored`, `file_renamed`, `file_deleted`, `file_rekeyed`,
`file_opened` (purpose `view`, `edit`, `external` or `export`),
`log_exported`, `integrity_alert`.

Opening a file writes `file_opened` after decrypting; if the record cannot be
written the open fails, so nothing is read without a log entry.

## The page

- Store calls run on a pool thread one at a time (`SecureDriveFeature::run`);
  rendering and search never touch the drive while one runs. Lock waits for it.
- The page re-reads the drive every 10 seconds while visible, only when the
  folder listing (names, sizes, modification times) changed.
- Idle lock is 15 minutes, postponed while an editor has unsaved changes or a
  decrypted copy is open.
- External copies go to `<local data>/secure-drive/open/<random>/` (owner-only)
  and are zero-filled and removed on Delete decrypted copies, on lock, on page
  destruction and at the next start.

## Tests

`forkmesh-secure-drive-tests` (from `content/desktop/build.json`, Qt Core and
OpenSSL only) covers the crypto primitives, invitation codes, a full
create/invite/approve/edit flow across three computers on one folder, that no
name or content appears in the folder, ciphertext tampering, planted objects,
log rollback, removal with re-encryption and refusal of the removed
computer's later records, passphrase changes, formulas and drawings.

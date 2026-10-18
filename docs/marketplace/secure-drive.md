# Secure Drive (suite `fmsd1`, container format 2)

Package: `marketplace/packages/secure-drive/`. The user-facing description,
threat table and limits are in the package
[README](../../marketplace/packages/secure-drive/README.md). This page is the
format specification every client implements byte for byte, and the reasons
behind it.

| Client | Code |
| --- | --- |
| Desktop (reference) | `marketplace/packages/secure-drive/content/desktop/SecureDrive*.{h,cpp}` |
| iPhone | `native/ios/ForkMeshCore/Sources/FMPackages/SecureDrive*.swift`, screens in `native/ios/ForkMesh/Features/Packages/SecureDrive*.swift` |
| Android | `native/android/core/.../securedrive/`, screens in `native/android/app/.../feature/securedrive/` |

## Why it is built this way

- **One object.** The share holds a single file of random-looking bytes. File
  names, contents, how many files there are, versions, the roster and the
  access log are all pages inside it, so the router or anyone who copies the
  USB drive learns only the file's size (which grows in steps) and which
  offsets change when. Hiding the write pattern too would need oblivious RAM;
  that is out of scope and the README says so.
- **No server.** The share can store and delete bytes, nothing else. Every rule
  is enforced by cryptography on the devices, and every device verifies
  everything it reads.
- **A writer lease, then verify.** A single file needs one writer at a time.
  SMB byte-range locks are not reliable across Windows, macOS, Linux, the iOS
  Files app and Android document providers, so the lease is a page in the file
  itself: write, wait, read back. The commit re-checks the lease and both index
  slots just before it writes the index, so a writer that lost the lease (a
  phone suspended mid-write) aborts instead of overwriting newer state.
- **Copy-on-write index.** Data and log pages are written to free pages first;
  the commit point is one index unit written to the older of two slots (A/B). A
  torn write leaves the other slot intact. Freed pages are overwritten with
  random bytes only after the new index is written and read back.
- **Approval, not just a code.** The invitation code only lets a device post a
  join request members can read, and lets the approver prove to the newcomer
  that the key came from a member. The drive key moves only after a person
  compares safety numbers.
- **Removal re-encrypts.** Blob keys are recorded under the drive key, so a
  removed device that kept the old drive key could read later saves under old
  blob keys. Removal makes a new drive key, seals it to the remaining devices,
  wipes the removed device's key slot and re-encrypts every version.
- **Signatures over stored bytes.** Every signed thing (record, index, keyring,
  join request) is stored as `{"b": base64url(body bytes), "s":
  base64url(signature)}` and verified over exactly those body bytes, so the
  three implementations never need identical JSON serializers.

## Primitives

All base64 is base64url without padding. `label(x)` is the ASCII bytes
`forkmesh-secure-drive/fmsd1/` followed by `x`.

| Name | Definition |
| --- | --- |
| `HKDF(secret, info, n=32)` | HKDF-SHA-256, salt 32 zero bytes, info `label(info)`, `n` bytes out |
| AEAD | AES-256-GCM, 12-byte nonce, 16-byte tag |
| `unit(key, aad, plain, size)` | `nonce(12) ‖ AES-GCM(key, nonce, aad, plain ‖ zeros)(size−12)`: the plaintext is padded with zero bytes so the unit is exactly `size` bytes. JSON plaintexts end at the first zero byte. |
| `sealToDevice(pub, L, plain)` | fresh X25519 key `e`; `ssX = X25519(e, pub.x25519)` (all-zero result refused); `(ctM, ssM) = ML-KEM-768.Encaps(pub.mlkem768)`; `shared = SHA3-256(ssM ‖ ssX ‖ ctX ‖ pub.x25519 ‖ 5c2e2f2f5e5c)` (X-Wing); `k = HKDF(shared, "seal/" ‖ L)`; output `ctX(32) ‖ ctM(1088) ‖ nonce(12) ‖ AES-GCM(k, nonce, label("seal/") ‖ L ‖ ctX ‖ ctM, plain)` |
| Device keys | X25519 private (32 random bytes), ML-KEM-768 seed `d ‖ z` (64), Ed25519 seed (32). Public: `{"x25519", "mlkem768", "ed25519"}` base64url. |
| `safety(context, pub)` | `d = SHA-256(label("safety") ‖ context ‖ ed25519 ‖ x25519 ‖ mlkem768)`; four groups, group `i` = big-endian u32 of `d[4i..4i+4]` mod 100000, five digits zero-padded, joined by single spaces. Device safety numbers use context `"device"`; join requests use the 12 raw invitation-id bytes. |
| Invitation code | 16 random bytes `S`; `c = SHA-256(label("invite-check") ‖ S)[0]`; Crockford base32 (`0123456789ABCDEFGHJKMNPQRSTVWXYZ`, MSB first) of `S ‖ c`, first 27 characters, groups of three joined by `-`. Parsing upper-cases, drops `-` and spaces, maps `O→0`, `I`/`L→1`, needs 27 characters, and the 7 bits after the 128 secret bits must equal `c >> 1`. |
| From `S` | invitation id `HKDF(S, "invite-id", 12)` (base64url in records), request key `HKDF(S, "request")`, approve key `HKDF(S, "approve")`, mailbox `big-endian u16 of HKDF(S, "mailbox", 2) mod 64` |
| Drive key id | hex of `HKDF(key, "key-id", 8)` |
| Padding | `padded(n)`: 4096 when `n ≤ 4096`; else `E = ⌊log2 n⌋`, `S = ⌊log2 E⌋ + 1`, `m = 2^(E−S) − 1`, `(n + m) & ~m` (Padmé) |

## The container file

Page size 16384 bytes; a unit's overhead is 28 bytes, so a blob chunk carries
16356 bytes. The file is a whole number of pages; offsets are page × 16384.

| Pages | Content | Unit |
| --- | --- | --- |
| 0 | lease | one 16384-byte unit, key `HKDF(drive key, "lease")`, AAD `label("lease")`, plaintext `{"device","token"}` |
| 1–4 | index slot A | one 65536-byte unit, key `HKDF(drive key, "root")`, AAD `label("root/A")` |
| 5–8 | index slot B | same, AAD `label("root/B")` |
| 9–40 | mailbox: 64 slots of 8192 bytes | join requests and keyrings |
| 41 … | data pages | log units and blob chunks |

A new drive is 73 pages (32 data pages) of random bytes. Everything not in use
is random: an unused page, slot or a released lease cannot be told from a used
one without a key.

### Index (root)

Body: `{"v":2, "drive", "gen", "pages", "logs": {device: [[start, count], …]},
"writer", "time", "prev"}`, signed by the writer over `label("root\n") ‖ body`;
hash `base64url(SHA-256(label("root\n") ‖ body ‖ signature))`; `prev` is the
previous index's hash. Generation `g` goes to slot A when `g` is even, B when
odd.

A reader opens both slots with every drive key it holds and takes, among the
indexes whose writer is not on its local list of removed devices, the one
under the highest-numbered key, then the highest generation. Each device keeps
the newest index it accepted (`cachedRoot`, with its generation and hash); when
the file holds nothing newer than that copy, the reader uses its copy
(`index_restored`) and its next commit writes it back.

### Log units

A log unit is `count` consecutive pages sealed as one unit: key `HKDF(drive
key, "log")`, AAD `label("log/") ‖ drive id ‖ "/" ‖ decimal start page`,
plaintext `{"device", "records": [{"b","s"}, …]}`. The index lists each device's
units in order. A commit copies the device's last unit plus the new records
into a newly allocated unit (up to 4 pages; beyond that the new records start a
fresh unit) and frees the old one.

### Records

Body `{"d" device, "s" seq, "l" lamport, "t" ms, "a" action, "x" data, "p"
previous record hash, "w" {device: [seq, hash]}}`, signed over
`label("record\n") ‖ drive id ‖ "\n" ‖ body`; hash = base64url SHA-256 of that
message followed by the signature. `w` is the newest record of every other
device the writer had seen.

| Action | `x` |
| --- | --- |
| `drive_created` | `label`, `device {id, name, keys}`, `mailbox` (root device only, seq 0) |
| `drive_opened`, `log_exported`, `drive_rekeyed` | `{}` |
| `device_added` | `device`, `name`, `keys`, `invite`, `mailbox` |
| `device_removed` | `device`, `name`, `epoch`, `keyId`, `upTo` (the removed device's newest seq the remover had seen) |
| `invite_created` | `invite`, `requestKey`, `approveKey`, `mailbox`, `expires` |
| `invite_used` | `invite`, `device`, `name` |
| `invite_revoked` | `invite` |
| `join_denied` | `invite`, `device`, `name` |
| `file_created` | `file`, `name`, `kind`, `blob`, `pages` (`[[start,count],…]`), `key`, `size`, `sha256` (hex of the plaintext) |
| `file_saved` | as `file_created` without `kind`, plus `base` (the blob the editor started from) |
| `file_restored` | `file`, `name`, `version`, `blob`, `pages`, `key`, `size`, `sha256` |
| `file_renamed` | `file`, `name`, `from` |
| `file_deleted` | `file`, `name` |
| `file_rekeyed` | `file`, `name`, `versions: [{old, blob, pages, key}]` |
| `file_opened` | `file`, `name`, `version`, `purpose` (`view`, `edit`, `external`, `export`) |
| `integrity_alert` | `code`, `file`, `detail` |

Kinds: `document` (`.html`, `.htm`), `spreadsheet` (`.csv`), `drawing`
(`.svg`), `text` (`.txt`, `.md`, `.json`, `.log`), `image` (`.png`, `.jpg`,
`.jpeg`, `.gif`, `.bmp`, `.webp`), otherwise `file`.

### Blobs

A version of `n` bytes is `max(1, ⌈padded(n) / 16356⌉)` chunks, each in its
own page (any free pages, recorded as runs). Chunk `i` in page `p` is a
16384-byte unit under the version's random 32-byte key with AAD `label("blob/")
‖ drive ‖ "/" ‖ blob ‖ "/" ‖ decimal i ‖ "/" ‖ ("1" if last chunk else "0") ‖
"/" ‖ decimal p`. The plaintext is the next 16356 bytes of the file, zero
padded. A reader checks the chunk count against the size, decrypts in order,
takes `n` bytes and checks the SHA-256.

### Mailbox

Slot `i` lives at byte `9 × 16384 + i × 8192`.

- **Join request** (written by the joining device, without the lease): unit of
  8192 bytes, key = request key, AAD `label("request/") ‖ decimal slot`,
  plaintext `{"b","s"}` with body `{"suite":"fmsd1","invite","device","name",
  "keys","time"}` signed by the joining device over `label("request\n") ‖ body`.
- **Keyring**: `sealToDevice(recipient, "keyring/" ‖ decimal slot, plaintext)`
  where the plaintext is `{"b","s"[,"m"]}` padded with zeros to 7044 bytes, so
  the slot is exactly 8192 bytes. Body `{"suite","drive","device","mailbox",
  "epochs":[{"id","n","key"}],"root":{"device","ed25519"},"issuer",
  "issuerKeys","time"[,"invite"]}` signed by the issuer over
  `label("keyring\n") ‖ body`; for an approval `m = HMAC-SHA-256(approve key,
  body)`.

Every member owns one slot (the creator's is random, a joiner's is its
request's slot) and finds its keyring there. Members know which slots are taken
(members' and open invitations'); a new invitation draws codes until its slot
is free.

## Operations

**Lease.** Read page 0. It is free when no drive key opens it, when it names
this device or a removed device, or when its bytes have not changed for 120
seconds of this device's own clock (clocks are never compared). To take it,
write a fresh lease (new random token), sync, wait 600 ms, read it back; if the
bytes are still ours the lease is held, otherwise keep waiting. A holder
rewrites it (renews) at least every 15 seconds during long work. Release writes
random bytes, only if the page still holds our lease. Give up with
`drive_busy` after twice the stale time plus 10 seconds.

**Commit** (inside every change): load, take the lease, load again, write data
pages and the new log unit to free pages (growing the file by
`max(shortfall, 32, pages/8)` random pages when needed), build the index with
`gen + 1`, check that page 0 still holds our lease (`drive_busy`) and both
index slots are byte-identical to what was loaded (`drive_changed`), write the
index, sync, read it back, run the commit's mailbox writes (keyrings, wiped
slots), overwrite freed pages with random bytes, release.

**Join.** The joining device writes its request into the slot the code names.
A member sees requests in the slots of open invitations, shows the safety
number, and on approval records `device_added` and `invite_used`, then writes
the keyring into that slot. The joining device opens its slot, checks the MAC
and the issuer's signature, and adopts the drive id, root device and keys.
Turning down or cancelling an invitation records it and fills the slot with
random bytes.

**Removal.** One commit: a new drive key (number + 1), `device_removed`, the
index under the new key written into **both** slots (each sealed with its own
slot's AAD, so no index under the old key survives), then fresh keyrings in
every remaining member's slot and random bytes in the removed device's slot. A second commit re-encrypts every
version into new pages under new keys (`file_rekeyed`) and frees the old pages.

## Verification

On every load:

1. Open this device's keyring slot. When a device that once had a keyring no
   longer finds one, it looks at where the index came from (step 2): if an
   index in the file opened with its keys, the slot was merely overwritten (a
   stale invitation code writes its request into a random slot), so it reports
   `keyring_restored` and its next commit writes a keyring issued by itself
   back into the slot. If no index in the file opens and only its cached copy
   is left, it has been removed (`keyring_missing`) and makes no further
   changes.
2. Choose the index (above); a writer not on the roster, removed, or with a bad
   signature is `untrusted_root`; an index older than the newest this device
   accepted is `rolled_back` or `index_restored`.
3. Decrypt every log unit; units no key opens are `unreadable_objects`.
4. Build the roster from the root device pinned at join time: verify each
   trusted device's chain (signature `bad_signature`, gaps `records_missing`,
   `prev` links `chain_broken`), trust devices its `device_added` records
   introduce, report other chains as `unknown_device`.
5. Check the keyring's issuer against the roster (`untrusted_keyring`, and its
   keys are dropped).
6. Fold the records in `(lamport, device, seq)` order. Records from a removed
   device after its `upTo` are `after_removal`. A `file_saved` whose `base` is
   not the current version counts a conflict; both versions are kept.
7. Compare every witness and this device's remembered heads with the chains
   (`records_missing`, `rolled_back`, `log_forked`); this device's own chain
   shorter than it remembers is `own_records_missing`.

Opening a file writes `file_opened` after decrypting; when that record cannot
be committed the open fails, so nothing is read without a log entry. A chunk
that fails to decrypt is refused (`content_damaged`) and logged as an
`integrity_alert`.

## Local state

Each client keeps its device keys and memberships on the device only, sealed
under its own platform protection (desktop: Argon2id 64 MiB/3 passes of a
passphrase, then AES-256-GCM, in `<local data>/secure-drive/device.json`). A
membership holds: `id`, `container`, `label`, `drive`, `root`, `rootEd25519`,
`epochs`, `mailbox`, `lastSeq`, `lastHash`, `lamport`, `heads`,
`rootGeneration`, `rootHash`, `removed`, `cachedRoot`, `keyringSeen`,
`joinSecret`, `joinedAt`. Fixtures use exactly these names.

## Interoperability fixtures

`marketplace/packages/secure-drive/tests/fixtures/interop-from-<writer>.{fmsd,json}`.
Each implementation writes one (`forkmesh-secure-drive-tests --write-fixture
<dir>` on the desktop) and reads every one present. The JSON (format
`forkmesh-secure-drive-interop-v1`) holds the test-only passphrase, `members`
(device id, name, keys and membership, container given as the file name),
`pending` (a device whose join request is in the file), `joinRequest` (its slot,
name, safety number), `openInviteCode`, `files` (id, name, kind, version count,
size, SHA-256), `deletedFiles`, `minimumRecords` and `deviceSafety`. Unused
space in a fixture is zero-filled so it compresses; real drives keep it random.
Fixture writers set the invitation lifetime to 100 years (it is 24 hours
everywhere else), because a join request is only shown while its invitation is
open and a fixture must keep passing after the day it was written.

## Platform differences

- Android treats `EINVAL` or `EROFS` from `fsync` on a document provider's
  descriptor as success: some providers cannot sync a proxied descriptor and
  write through on close instead.
- Phones discard unsaved editor changes when they lock (after a minute in the
  background); the desktop postpones its idle lock while an editor has unsaved
  changes.

A reader must, on a copy of the file: open it as every member with no
problems, match the file list, decrypt every file to its SHA-256, match the
safety numbers and the pending request, add a file and save a version as a
member, approve the pending device and have it join and read both, and find the
drive still clean.

## The desktop page

- Store calls run on a pool thread one at a time (`SecureDriveFeature::run`);
  rendering and search never touch the drive while one runs.
- The page re-reads the drive every 10 seconds while visible, only when the
  file's size or anything in pages 1–40 (both index slots and the whole
  mailbox, so new join requests are noticed) changed.
- Idle lock is 15 minutes, postponed while an editor has unsaved changes or a
  decrypted copy is open. Editors pass the version they opened as `base`.
- External copies go to `<local data>/secure-drive/open/<random>/`
  (owner-only) and are zero-filled and removed on Delete decrypted copies, on
  lock, on page destruction and at the next start.

## Tests

`forkmesh-secure-drive-tests` (Qt Core and OpenSSL) covers the primitives,
invitation codes, a create/invite/approve/edit flow, that the share holds one
opaque file, that small saves do not change its size, two devices writing at
the same time under the lease, ciphertext tampering, rolling the file back,
destroying both index slots and repairing from the cached index, removal with
re-encryption and a removed device unable to write, passphrase changes,
formulas, drawings, and every interoperability fixture present.

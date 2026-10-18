# Secure Drive package

Secure Drive keeps an end-to-end encrypted shared drive in **one file** on
your local network: a USB drive plugged into your router, a NAS share or a USB
stick. Every document, spreadsheet, drawing and other file, every version, the
list of approved devices and the access log are encrypted pages inside that
one file. The router and anyone who copies the USB drive see a single file of
random-looking bytes; they cannot tell how many files it holds, what they are
called or who opened them. The keys live on the computers and phones you
approve and nowhere else: not on the router, not in the file, not at ForkMesh.

It runs on the desktop (a rail page), on iPhone and on Android, and is opt-in
from the Market. All three open the same drive file.

## What it does

- **Documents, spreadsheets and drawings, built in.** New, then Document,
  Spreadsheet or Drawing. The editors decrypt into memory and never write
  plaintext to the device's storage:
  - documents are rich text (headings, bold, italic, underline, lists), stored
    as HTML;
  - spreadsheets are a grid with a formula bar (`+ - * / ^`, comparisons, `SUM`,
    `AVERAGE`, `MIN`, `MAX`, `COUNT`, `ROUND`, `ABS`, `IF` over cells and
    ranges), stored as CSV, which Excel, Numbers and LibreOffice open;
  - drawings have a pen, lines, boxes, ovals, an eraser, colours and widths,
    stored as SVG.

  Plain text files open in a text editor and pictures in a viewer.
- **Any other file type.** Add files (on the desktop, drag them onto the
  page). Open in another app (Share on phones) writes a decrypted copy to a
  private folder for the app that opens it, and the desktop offers to save
  changes back. Copies are overwritten and deleted when you lock and the next
  time Secure Drive starts. Save a decrypted copy writes one wherever you
  choose, after a warning.
- **Versions.** Every save is a new encrypted version. Open any version
  read-only or make it the current one. When two devices edit the same file at
  once, both versions are kept and the file says so.
- **Full access log.** Every connect, open, edit, export, rename, delete,
  invitation, approval, removal and log export is a record signed by the device
  that did it, stored encrypted in the drive file and shown under Activity. A
  file is never opened unless its record is written first. Activity exports as
  CSV, which is itself logged.
- **Tamper evidence.** Each device's records form a hash chain, every record
  states how far it had seen every other device's chain, and each device
  remembers the newest state it verified. A record that is deleted, edited,
  forged, replayed or rolled back, a damaged file, or an old copy of the drive
  file put back shows up as a problem at the top of the page. A device that
  finds the drive's index damaged or rolled back uses its own verified copy and
  writes it back on the next change.
- **Devices.** Invite a computer or phone with a one-time code. It asks to join
  and shows a safety number; approve it only if your screen shows the same
  number. Removing a device replaces the drive key, re-encrypts every file, and
  that device can no longer read or change anything stored after its removal.
- **Several devices at once.** Devices take turns writing through a lease kept
  inside the drive file, so two devices saving at the same moment never
  overwrite each other. A device that disappears while writing is taken over
  after two minutes.
- **Locking.** Each device's keys are encrypted under a passphrase (Argon2id).
  The desktop locks after 15 idle minutes, phones after a minute in the
  background, and Lock clears the keys from memory.

## Setting it up on a router

1. Plug a USB drive into the router and turn on its file sharing (often called
   USB storage, network share, SMB or Samba). Prefer a share that needs a user
   name and password, and turn off FTP or internet access to the drive if the
   router offers it.
2. Open the share once on each device:
   - Windows: `\\router\usb`; macOS and Linux: `smb://router/usb`.
   - iPhone: Files, ⋯, Connect to Server, `smb://router`.
   - Android: the Files app or an SMB document provider app that offers the
     share to other apps (it must allow opening a file for reading and writing).
3. On the first device: Secure Drive, New drive, save the drive file on the
   share (for example `SecureDrive.fmsd`).
4. Under Computers (Devices on phones), Invite. On the other device: Join a
   drive, pick the same file, type the code. Compare the safety numbers, then
   approve.

## What it protects against, and what it cannot

Nothing is "unhackable", and this page does not claim it. What it does:

| Someone who… | can… | cannot… |
| --- | --- | --- |
| controls the router or copies the USB drive | see one file, its total size (it grows in steps of at least 512 KiB) and which parts of it change when; delete or corrupt it | read any file name or content, count the files, read the log or tell which device did what; plant a file, a version or a record that is accepted; roll the drive back without it being reported |
| knows an invitation code | post a join request | get a key: a request opens nothing until a member approves it, and the safety numbers differ |
| steals a device that is locked | copy its key file | open it without the passphrase (Argon2id, 64 MiB, 3 passes) |
| has a removed device | keep what it opened or saved before; damage the file if it can still write to the share | open anything stored after its removal; get records or an index accepted |
| has an approved, unlocked device | everything that device can do | (approved devices are trusted; remove a lost one from another device) |

Further limits:

- Encryption cannot stop deletion: whoever can write to the share can delete
  or overwrite the file. Secure Drive detects it and says so. Keep a backup
  copy of the drive file (copying it is safe), and when you remove a device,
  also change the share's password so it can no longer write to it.
- Someone watching the share over time sees when and roughly how much is
  written. Hiding that would need oblivious storage, which this does not do.
- An app opened through Open in another app or Share sees plaintext, and so
  may the operating system's caches. Built-in editors avoid storage but cannot
  stop the operating system from swapping memory.
- On Android, some storage apps cannot open a file for reading and writing in
  place; Secure Drive says so instead of opening the drive.
- FAT32-formatted USB drives cannot hold a drive file over 4 GiB; use exFAT or
  NTFS.
- It has not been audited by a third party.

## Cryptography (suite `fmsd1`)

| Purpose | Primitive |
| --- | --- |
| Each device's key file | Argon2id (64 MiB, 3 passes) then AES-256-GCM |
| Handing the drive key to a device | X25519 and ML-KEM-768 together (X-Wing combiner), so opening it takes breaking both |
| Signing every record, the index, key slots and join requests | Ed25519 |
| Pages of the drive file | AES-256-GCM; every page and every version has its own key or binding, so pages cannot be moved, swapped or replayed |
| Key derivation | HKDF-SHA-256 with one label per use |

The desktop uses OpenSSL 3.5; the phones use the apps' own implementations of
the same primitives. All three read and write the same files, which the test
suites check against drive files written by each of them. The format, the
verification rules and the reasons behind them are in
[docs/marketplace/secure-drive.md](../../../docs/marketplace/secure-drive.md).

## Layout

- `content/desktop/SecureDriveCrypto.*`: primitives, invitation codes, safety
  numbers, padding.
- `content/desktop/SecureDriveStore.*`: this device's key file (`LocalState`)
  and the drive (`Drive`): the container file, lease, index, key slots,
  records, verification, files, versions, removal and re-encryption.
- `content/desktop/SecureDriveFormats.*`: the spreadsheet (CSV and formulas)
  and drawing (SVG) formats.
- `content/desktop/SecureDriveEditors.*`, `content/desktop/SecureDriveFeature.*`:
  the editors and the rail page.
- `content/desktop/tests/test_secure_drive.cpp`: the headless suite
  (`forkmesh-secure-drive-tests`); `--write-fixture <dir>` writes the
  desktop's interoperability fixture.
- `tests/fixtures/interop-from-*.{fmsd,json}`: one small drive file per
  implementation, with test-only keys, that every implementation must open,
  verify and write to.
- iPhone: `native/ios/ForkMeshCore/Sources/FMPackages/SecureDrive*.swift` and
  `native/ios/ForkMesh/Features/Packages/SecureDrive*.swift`.
- Android: `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/securedrive/`
  and `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/securedrive/`.

On the desktop, keys and drive memberships live in
`<local data>/secure-drive/device.json`, encrypted as above; nothing about a
drive is kept in settings except the id of the last drive opened.

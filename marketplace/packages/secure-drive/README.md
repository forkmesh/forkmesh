# Secure Drive package

Secure Drive turns a folder on your local network, such as a USB drive
plugged into your router, a NAS share or a USB stick, into an end-to-end
encrypted shared drive. Everything written to the folder is encrypted on the
computer first. The folder holds only random-named `.fmsd` objects. The keys
live on the computers you approve and nowhere else: not on the router, not on
the drive, not at ForkMesh.

It is a desktop rail page (`builtin:secure-drive`), opt-in from the Market.

## What it does

- **Documents, spreadsheets and drawings, built in.** New, then Document,
  Spreadsheet or Drawing. They open in an editor that decrypts into memory only
  and never writes plaintext to this computer's disk:
  - documents are rich text (headings, bold, italic, underline, lists), stored
    as HTML;
  - spreadsheets are a grid with a formula bar (`+ - * / ^`, comparisons, `SUM`,
    `AVERAGE`, `MIN`, `MAX`, `COUNT`, `ROUND`, `ABS`, `IF` over cells and
    ranges), stored as CSV, which Excel, Numbers and LibreOffice open;
  - drawings have a pen, lines, boxes, ovals, an eraser, colours and widths,
    stored as SVG.

  Plain text files (`.txt`, `.md`, `.json`, `.log`) open in a text editor and
  pictures in a viewer.
- **Any other file type.** Add files (or drag them onto the page). Open in
  another app writes a decrypted copy to a private folder (owner-only
  permissions), opens it with the system's app, and offers Save changes to the
  drive. The copy is overwritten and deleted when you choose Delete decrypted
  copies, when you lock, and the next time the page starts. Save a decrypted
  copy writes one wherever you choose, after a warning.
- **Versions.** Every save is a new encrypted version. Open any version
  read-only or make it the current one. When two computers save the same file
  at once, both versions are kept and the file says so.
- **Full access log.** Every connect, open, edit, export, rename, delete,
  invitation, approval, removal and log export is a record signed by the
  computer that did it, encrypted on the drive, and shown under Activity.
  Activity exports as CSV, which is itself logged.
- **Tamper evidence.** Each computer's records form a hash chain. Every record
  also states how far it had seen every other computer's chain, and each
  computer remembers the furthest it has seen. A record that is deleted,
  edited, forged, replayed, rolled back or written by a removed computer shows
  up as a problem above the tabs. So does a file whose ciphertext was changed:
  it is refused rather than opened, and the refusal is logged.
- **Computers.** Invite a computer with a one-time code. It asks to join and
  shows a safety number; you approve it only if your screen shows the same
  number. Removing a computer replaces the drive key and re-encrypts every file
  under the new key.
- **Locking.** The keys on this computer are encrypted under a passphrase
  (Argon2id). The page locks after 15 idle minutes and when you press Lock,
  and clears the keys from memory.

## Setting it up on a router

1. Plug a USB drive into the router and turn on its file sharing (often called
   USB storage, network share, SMB or Samba). Prefer a router that requires a
   user name and password for the share, and turn off FTP or internet access to
   the drive if the router offers it.
2. On each computer, open the share once (`\\router\usb` on Windows,
   `smb://router/usb` on macOS and Linux) so the system mounts it.
3. On the first computer: Secure Drive, New drive, pick an empty folder on the
   share.
4. Under Computers, Invite a computer. On the other computer: Join a drive,
   pick the same folder, type the code. Compare the safety numbers, then
   approve.

## What it protects against, and what it cannot

Nothing is "unhackable", and this page does not claim it. What it does:

| Someone who… | can… | cannot… |
| --- | --- | --- |
| controls the router or copies the USB drive | see how many objects there are, roughly how large each is (sizes are padded, up to about 12%), and when they change; delete or corrupt objects | read any file name, file content, the log, or which computer did what; plant a file, a version or a log record that is accepted; delete or roll back records without it being reported |
| knows an invitation code | drop a join request | get a key: a request opens nothing until a member approves it, and the safety numbers differ |
| steals a computer that is locked | copy its key file | open it without the passphrase (Argon2id, 64 MiB, 3 passes) |
| has a removed computer | keep what it opened or saved before | open anything stored after its removal; its later records are refused |
| has an approved, unlocked computer | everything that computer can do | (approved computers are trusted; remove a lost one from another computer) |

Further limits:

- Deletion cannot be prevented by encryption: whoever can write to the share
  can delete objects. Secure Drive detects it and says so; keep a backup of the
  drive folder (copying the encrypted objects is safe).
- An app opened with Open in another app sees plaintext, and so does the
  operating system's cache of it. Built-in editors avoid the disk but cannot
  stop the operating system from swapping memory to disk.
- An SSD may keep old blocks of an overwritten decrypted copy.
- FAT32-formatted USB drives cannot hold files over 4 GiB; use exFAT or NTFS.
- It has not been audited by a third party.

## Cryptography (suite `fmsd1`)

OpenSSL 3.5 supplies every primitive; nothing is reimplemented. A computer
with an older OpenSSL is told so instead of creating keys.

| Purpose | Primitive |
| --- | --- |
| This computer's key file | Argon2id (64 MiB, 3 passes) then AES-256-GCM |
| Handing the drive key to a computer | X25519 and ML-KEM-768 together (X-Wing combiner), so opening it takes breaking both |
| Signing every record, key file and join request | Ed25519 |
| Files, versions and the log | AES-256-GCM; a fresh random key per version, 1 MiB chunks bound to their position and to the last chunk |
| Key derivation | HKDF-SHA-256 with one label per use |

The full format, the verification rules and the reasons behind them are in
[docs/marketplace/secure-drive.md](../../../docs/marketplace/secure-drive.md).

## Layout

- `content/desktop/SecureDriveCrypto.*`: primitives, invitation codes, safety
  numbers, padding.
- `content/desktop/SecureDriveStore.*`: this computer's key file
  (`LocalState`) and the drive (`Drive`): objects, records, verification,
  invitations, files, versions, removal and re-encryption.
- `content/desktop/SecureDriveFormats.*`: the spreadsheet (CSV and formulas)
  and drawing (SVG) formats.
- `content/desktop/SecureDriveEditors.*`: the built-in editors.
- `content/desktop/SecureDriveFeature.*`: the rail page.
- `content/desktop/tests/test_secure_drive.cpp`: the headless suite
  (`forkmesh-secure-drive-tests`): two and three computers on one folder,
  tampering, planted objects, rollback, removal, formulas and drawings.

Keys and drive memberships live in `<local data>/secure-drive/device.json`,
encrypted as above; nothing about a drive is kept in settings except the id of
the last drive opened.

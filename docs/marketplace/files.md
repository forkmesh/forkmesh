# Files package

`marketplace/packages/files` (Market id `files`, entrypoint `builtin:files`)
is a file explorer on the desktop, iPhone and Android for each device's own
files and for SMB network shares on the local network. It was asked for so a
USB disk shared by the router (see [openwrt.md](openwrt.md), Network shares)
can be browsed from ForkMesh, from the files editor and from a Files shortcut,
with the same explorer working on every device. The user-facing description
is the package [README](../../marketplace/packages/files/README.md).

It is bundled, off by default and account-synced, with
`install.installedBefore: "2026-10-19"` so existing accounts get it
([core-plugins.md](core-plugins.md)). The phones show a **Files** Home tile
(route `files`, web path `/dashboard/files`). On the desktop the rail's
**Files** button and Files section, which were never gated, are the shortcut;
the explorer and editor gained a **Network** source there.

## Network shares (SMB)

There is no SMB library on any of the three: the phone builds are offline and
dependency-free, so each platform carries the same small SMB 2 client, the
way the OpenWrt client is written three times. They share one design and the
same error strings.

- **Transport.** TCP 445 (or the `:port` typed in the address), Direct TCP
  framing. Hosts must resolve to local-network addresses, the OpenWrt
  `isLocalHost` rule ("Network shares must be on your local network."), so an
  NTLM exchange never leaves the LAN.
- **Dialects.** 2.0.2 and 2.1 only. SMB 3 would need encryption and
  pre-authentication integrity; ksmbd, Samba, Windows and macOS all accept
  2.1. A server whose minimum is SMB 3 gets "<host> does not speak SMB 2.1."
  Reads and writes go in chunks of at most 64 KiB, so each request costs one
  credit.
- **Sign-in.** NTLMv2 wrapped in SPNEGO, no key exchange, workstation
  `FORKMESH`, domain only when typed as `DOMAIN\user`. **Sign in as guest**
  tries user `guest` with an empty password (servers that map unknown users
  to guest accept it), then an anonymous session.
  Requests are signed (HMAC-SHA256) only when the server requires signing and
  the session is not guest or anonymous. The server's signature on replies is
  not checked.
- **Calls.** List (query directory, `.` and `..` dropped), read, write
  (overwrite), new folder, delete (non-empty folders are walked first; SMB has
  no trash, so the confirmation says it is permanent), rename/move within a
  share (`FileRenameInformation`), and **Find shares**: srvsvc
  `NetShareEnumAll` level 1 over the `srvsvc` pipe on `IPC$`, keeping disk
  shares and hiding `$` shares. If it fails the user types the share name.
- **Connections.** One per share, requests serialized, interim
  `STATUS_PENDING` replies skipped, one reconnect and retry after a dropped
  socket.
- **Errors.** Logon failure "The user name or password is wrong.", access
  denied "Access denied.", bad network name "There is no share named <share>
  on <host>.", not found "That file or folder no longer exists.", collision
  "Something with that name is already there.", not empty "The folder is not
  empty.", sharing violation "The file is open somewhere else.", disk full
  "The disk is full.", write protected "The share is read only.", no answer
  "Could not reach <host>. Is file sharing on?", anything else "The file server
  answered 0x%08X."

Saved shares hold a name, address, share, user, guest flag and whether to
remember the password. The password is stored only when **Remember password**
is on, and only in the device's secret store (below); otherwise it lives in
memory for the session.

## The explorer

The same features on every platform:

- **Places.** This device (desktop: the computer's folders; iPhone: **On this
  iPhone**, the app's Documents folder, also visible in the Files app, plus
  folders added with **Add folder…**, which covers iCloud Drive and servers
  connected in the Files app; Android: **On this phone**, `filesDir/Files`,
  plus folders added through the system folder picker), saved network shares,
  **Add network share** (Address, Share, Find shares, User, Password, Sign in
  as guest, Remember password) and, on the phones, the router's shares as
  suggestions once the Router package has signed in.
- **Browsing.** Path bar, Up, Refresh (pull to refresh on phones), folders
  first, sort by Name, Date or Size, a filter for the open folder, Show hidden
  files, "This folder is empty."
- **Actions.** Open, Rename, Delete, Copy, Move, Paste, Download, Share
  (phones), Info, New folder, New file, Upload (desktop also drag and drop).
  Copy and Move work between any two places, device and network included,
  staged through a temporary file; folders copy recursively; a name clash gets
  a numbered name. Transfers show progress and can be cancelled.
- **Opening.** Text and code (by extension or a UTF-8 check of the first
  8 KB; up to 5 MB on the phones, 4 MB on the desktop) opens in the existing
  editor, and Save writes back where the file came from, over SMB for network
  files. Pictures open in a zoomable viewer and PDFs in PDFKit on the iPhone;
  everything else goes to QuickLook / the share sheet (iPhone), a FileProvider
  `ACTION_VIEW` (Android) or the system app after a temporary download
  (desktop).
- **Local deletes** never use a raw recursive delete
  ([../engineering/destructive-actions.md](../engineering/destructive-actions.md)).
  The desktop moves local files to the trash (`QFile::moveToTrash`). The phones
  walk the folder entry by entry, without following links, and refuse to
  delete the place's root or anything outside it.

## Code map

| Surface | Protocol and logic | Screens | Tests |
| --- | --- | --- | --- |
| Desktop | `desktop/src/SmbProtocol.*` (wire format, NTLM/SPNEGO, srvsvc NDR, URLs, strings), `SmbClient.*` (blocking client over an injectable `Transport`), `NetworkShares.*` (`SavedShare`, `ShareStore`, `Connection`: one worker thread per share) | `NetworkFilesPanel.*` (the Network source, share dialog, process-wide file clipboard), `MainWindowNetworkFiles.cpp`, `MainWindowFiles.cpp` (local tree actions) | `forkmesh-smb-tests`, `forkmesh-network-files-tests`, `desktop/tests/FakeSmbServer.h` |
| iPhone | `ForkMeshCore/Sources/FMFiles/` (`SmbBytes`, `SmbHashes`, `SmbNtlm`, `SmbMessages`, `SmbStatus`, `SmbSrvsvc`, `SmbClient`, `FileModels`, `FileSources`, `NetworkShares`) | `ForkMesh/Features/Files/`, `Platform/SmbNetworkTransport.swift` (Network.framework) | `FMFilesTests` (`FakeSmbServer`, `SmbClientTests`, `FileBrowsingTests`) |
| Android | `core/.../files/` (`Md4`, `SmbWire`, `Ntlm`, `SmbClient`, `SrvSvc`, `FileSource`, `FilesModel`) | `app/.../feature/files/` (`FilesFeature`, `FilesStore`, `SafFileSource`, `FilesScreens`) | `core/.../files/` (`FakeSmbServer`, `SmbClientTest`, `FilesModelTest`) |

Each fake server is an in-memory SMB 2.1 server that checks NTLMv2 proofs and
signatures and can answer with pending replies, guest-only access, required
signing or SMB 3 only. The tests also run the MS-NLMP 4.2.4 NTLMv2 vectors and
MD4 vectors (MD4 and HMAC-MD5 are hand-written on iOS and Android, since
neither platform offers MD4 portably).

## Storage

| Platform | Saved shares and places | Remembered passwords |
| --- | --- | --- |
| Desktop | `QSettings` `files/networkShares` | `<configRoot>/network-share-passwords.json`, owner-only (0600), like the desktop's other credentials, since it has no OS keychain integration |
| iPhone | key-value store `files/network-shares`; `UserDefaults` `files/saved-folders` (bookmark data), `files/show-hidden`, `files/sort` | Keychain `files.network-share.password.<id>` |
| Android | preferences `market.packages.files` | `SecureStore` (Keystore) `files.smb.<id>` |

Android exposes only the `files-open` cache folder through the
`composer.files` FileProvider and clears copies older than a day at start.

## Desktop specifics

Network files open as editor tabs keyed by their `smb://` URL
(`networkFile=<url>` on the tab), so the Files section's editor is the files
editor for shares too. The Router page asks the window to open a share through
`NativePackageHost::openNetworkShare(url, guest)`.

## Tests

- Desktop: build per [../desktop/build-and-test.md](../desktop/build-and-test.md),
  then run `forkmesh-smb-tests`, `forkmesh-network-files-tests` and
  `forkmesh-openwrt-tests`.
- iPhone: `FM_ONLY=FMFiles swift build --build-tests` then `swift test
  --skip-build` in `native/ios/ForkMeshCore`.
- Android: `./gradlew --offline :core:coreTest -Pfilter=nativeapp.files` in
  `native/android`.

## Limits

- SMB 2.0.2 and 2.1 only: no SMB 3 encryption, NTLM only (no Kerberos).
- Not yet tried against a real ksmbd or Samba server from this machine; the
  clients were checked against the fake servers and the published vectors.
- Desktop: cancel applies to transfers, not folder listings, and one tracked
  transfer runs at a time.
- Copies between places go through a temporary file rather than a stream.

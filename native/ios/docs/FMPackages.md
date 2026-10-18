# FMPackages

The organization packages (CRM, Marketing, Analytics, Competitors) and the Market. `import
FMPackages` (re-import `FMBase` for `JSONObject`). The SwiftUI views are in
`ForkMesh/Features/Packages`; tests are in `ForkMeshCore/Tests/FMPackagesTests`.

## Marketing Reddit desk

- `MarketingRedditSection` (`Features/Packages/MarketingSections.swift`) lists the team's Reddit
  accounts under "Your accounts" (`MarketingOverview.redditAccounts`, parsed from
  `redditDesk.accounts`; each row links u/name to its profile URL, or reads "No Reddit accounts
  yet."). They are edited in the `.redditWatch` form's Accounts field (one per line, up to 10; a
  name, `u/name` or a profile link), sent as `reddit.accounts` in the same settings PUT as
  subreddits and searches. More than 10 is refused before the request; the server's
  `invalid_reddit_account` and `too_many_reddit_accounts` map to messages in `PackagesError`.
- Each thread row has a caption from `MarketingRecord.redditActivity(now:)`
  (`MarketingWorkspace.swift`, kept in core so it is unit tested): comments, points and views of
  the post; "your comment: N views, N points" when a comment is watched (`monitorCommentUrl`);
  "views show only to u/…" (`commentAuthor`, else `author`) when the thread was read but Reddit
  hid every view count; and "read 2h ago" from `checkedAt`, followed by "as u/…" (`readAs`, the
  account the browser extension was signed in as), or "not read yet". Points and comments moved from the
  row's facts to this caption so they are not shown twice. Same rules as Android
  ([docs/native/ANDROID.md](../../../docs/native/ANDROID.md), Marketing Reddit desk).

## Secure Drive

The iPhone client of the Secure Drive package. The format, every rule and the reasons behind them
are in [docs/marketplace/secure-drive.md](../../../docs/marketplace/secure-drive.md); the desktop
code in `marketplace/packages/secure-drive/content/desktop` is the reference and this port follows
it function by function (same JSON names, AADs, labels, issue codes and record wording, with
"computer" read as "iPhone" or "device").

- `SecureDriveCrypto.swift`: the primitives on FMCrypto (HKDF with the `fmsd1` label, AES-GCM
  units, X-Wing `sealToDevice`, safety numbers, invitation codes, Padmé padding, Argon2id).
  `Sha256Stream` hashes blobs as they stream: CryptoKit on Apple platforms, a portable FIPS 180-4
  loop on Linux, checked against `Sha256.hash` in the tests.
- `SecureDriveContainer.swift`: the page layout and the file behind the `SecureDriveStorage`
  protocol (`withFile`, `createEmpty`, `exists`, `removeCreated`, `canonical`, `displayName`), so
  the store never knows whether the container is a plain path (tests, Linux) or a security-scoped
  document on an SMB share (the app's storage starts access and coordinates around each call).
  A file the store created and could not finish is removed with `unlink`, never a recursive delete.
- `SecureDriveState.swift`: memberships with the spec's field names and `SecureDriveLocalState`
  (`device.json`, Argon2id 64 MiB / 3 passes then AES-GCM under `label("local")`, owner-only).
  The sealed inner object also carries `bookmarks` (membership id to bookmark data), so the
  location of a drive is as private as its keys; `exportDevice` leaves it out.
  `init(directory:testingKdf:)` is the only way to use a cheaper Argon2id, and only tests call it.
- `SecureDrive.swift`: scan/load (keyring, index choice with `cachedRoot`, log units, roster from
  the pinned root, fold, witnesses), the lease, the commit and every operation. Lease timing is an
  instance property (`timing`) so tests run with settle 0 and a short stale time;
  `inviteLifetimeMs` defaults to the spec's 24 hours.
- `SecureDriveSheet.swift` and `SecureDriveDrawing.swift`: the spreadsheet CSV and formula engine
  and the drawing SVG model, ported from `SecureDriveFormats.cpp`.

Tests: `SecureDriveCryptoTests`, `SecureDriveFlowTests` (the desktop's drive flow on a temp
container, plus cancel, deny, revoke and the standard passphrase cost), `SecureDriveFormatsTests`
and `SecureDriveInteropTests`, which reads every
`marketplace/packages/secure-drive/tests/fixtures/interop-from-*.json` the way the desktop's
`readsFixture()` does. `SECURE_DRIVE_WRITE_FIXTURE=<dir>` makes `testOpensWhatItWrites` also copy
`interop-from-swift.{fmsd,json}` there:

    SECURE_DRIVE_WRITE_FIXTURE=$PWD/../../../marketplace/packages/secure-drive/tests/fixtures \
      FM_ONLY=FMPackages swift test --filter SecureDriveInteropTests/testOpensWhatItWrites

The Swift fixture's invitations are written with a 100-year lifetime: a committed fixture is read
long after it was made, and with the 24-hour default its pending request would stop being shown
(requests are only read for open invitations) a day after it was written.

The screens are in `ForkMesh/Features/Packages/SecureDrive*.swift`. `SecureDriveSession.swift`
(core) holds `SecureDriveService`, which runs every store call on one private serial queue (the
lease waits and fsyncs never block the main actor or the cooperative pool), the value snapshots the
views show, and `SecureDriveScratchFiles`, which zero-fills and unlinks decrypted copies under
`Caches/secure-drive-open/<random>/` (owner-only, wiped on lock and at the next start; only
random-named folders under that checked parent are removed, with `rmdir`). The app's storage
(`SecureDriveModel.swift`) opens the drive file through its security-scoped bookmark inside an
`NSFileCoordinator` (`.forMerging` to write, `.withoutChanges` to read). A new drive is made in an
app-private folder and handed to `.fileMover`, so the user saves it where they like (an SMB share in
Files), and the moved file is bookmarked. Joining picks the `.fmsd` with `.fileImporter`. Each
commit holds a `UIApplication` background task; the app locks 60 seconds after leaving the
foreground and after 15 idle minutes unless an editor has unsaved changes or a decrypted copy is
open, and re-reads the open drive every 10 seconds only when its fingerprint changed.

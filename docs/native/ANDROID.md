# Native Android app: design notes

`native/android` is the ForkMesh Android app, in Kotlin and Jetpack Compose.
Its source is code only; the reasons behind non-obvious decisions live here.
Build, test and release steps are in `native/android/README.md`.

Paths: `A/` is `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/`,
`C/` is `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/`.

## 1. Toolchain and dependencies

- Gradle wrapper, Android Gradle Plugin with built-in Kotlin (do not apply
  `kotlin-android`), Compose BOM, compileSdk/targetSdk 37, minSdk 26.
- First-party dependencies only: AndroidX/Compose and Kotlin coroutines. HTTP,
  JSON, WebSocket, cryptography, the Git engine, QR codes and icons are
  implemented in this tree. Consequences: home-screen widgets use
  `RemoteViews` (not Glance), the QR scanner uses Camera2 (not CameraX), the
  background revive job uses `JobScheduler` (not WorkManager), chat video uses
  `VideoView`, and view-once capture uses the legacy camera API with
  `MediaRecorder` (not Media3).
- **Icon font.** A subset of MaterialIcons-Regular (Apache-2.0, shared with
  iOS) in `app/src/main/assets/fonts`: every codepoint name that appears as a
  word in the tracked tree plus `help_outline` and the iOS
  `MaterialIconCodepoints` table. A name outside the subset draws
  `help_outline`. The full table is `tools/material_icons/MaterialIcons.codepoints`;
  after adding an icon name run `python3 tools/subset_material_icons.py`
  (needs fonttools; `--font` takes the full font). `app/tests/test_material_icon_subset.py`
  fails until you do.
- `:core` is pure Kotlin/JVM (no `android.*`), tested on the host JVM by an
  in-tree runner registered as `coreTest`:
  `./gradlew :core:coreTest -Pfilter=Json` runs classes whose name contains
  `Json`.
- `res/xml/network_security_config.xml` refuses cleartext except `10.0.2.2`,
  `localhost` and `127.0.0.1`.

## 2. Architecture

- **Feature seams.** `A/feature/Features.kt` is the only place the shell meets
  the areas. Each area owns one `*Feature` object and package, exposes
  `install(app)` and screen factories, and registers services in
  `C/core/Services.kt`. Cross-area wiring happens in `Features.connectAreas`.
  Install order: account, local repos, chat, agents, then the rest.
- **Projects.** A project switch clears `Services` and runs `Features.install`
  again, rebuilding every service. Each install must be repeatable, launch
  coroutines in `ProjectLifecycle.scope`, and register sockets, threads and
  audio with `ProjectLifecycle.onClose`. Object-level state that outlives a
  switch is a bug.
- **Threads.** `:core` is blocking; the app calls it from `Dispatchers.IO`.
  `C/chat/EventLoop.kt`: one thread owns a service's state, blocking work hops
  to a worker and posts back with a generation check; virtual time lets tests
  advance timers.
- **Screens.** `A/ui/navigation/Navigator.kt`: a `Screen` is a plain object
  that owns its state and survives lower in the stack. Work that must continue
  after a push runs on the screen's own scope.
- **Navigation.** `C/navigation` holds `Route`, the link parser and the guest
  gate. Areas register with `NavigationFeature.register`. `GuestGate` applies
  to every route whatever its source. Unknown `forkmesh://` paths open the home
  tab; unknown web links go to the browser; page names match before
  repositories.

## 3. Storage, secrets and identity

- **Preferences** (`A/platform/Prefs.kt`) keep the key names the retired
  Flutter app wrote, so migrated values need no renaming. A value stored with
  another type reads as absent. Sign-out writes synchronously.
- **Secrets** (`A/platform/SecureStore.kt`): AES-256-GCM under an Android
  Keystore key, with the key name as associated data. A value that no longer
  opens reads as absent. Secrets found in plain preferences are moved in once.
- **Per-project isolation.** The original project owns the un-prefixed
  namespace; others use `p/<projectId>/`; sessions live under
  `auth/session/<projectId>`. `Services.get<SecretStore>()` returns the active
  project's view.
- **Node identity** (`C/security/Identity.kt`): the Ed25519 public key
  (base64url, no padding) is the node id. Stored as padded base64 of the
  32-byte seed under `identity/ed25519Seed`. A corrupt value throws rather than
  replacing the key, and `SecurityBootstrap` leaves the device unregistered.
  The key rotates only on `device_revoked` or `device_key_conflict`; local
  repositories are untouched.
- **Owner encryption** (`C/security/OwnerCrypto.kt`): `forkmesh.owner-sealed`
  envelopes; one AES-256-GCM content key wrapped per owner device under hybrid
  X25519 + ML-KEM-768; wrap key = HKDF-SHA256(ssX25519 || ssMlKem, salt
  `forkmesh-mirror-kem-v1`, info ephPub || mlkemCiphertext). Stored as
  `{"v":1,"x25519Pub","x25519Priv","mlkemPub","mlkemPriv"}`. Key id =
  base64url(SHA-256(x25519Pub || mlkemPub)). Recipient cap matches
  `MAX_OWNER_ENVELOPE_RECIPIENTS` in `app/src/security_controls.py`. Failed
  opens are never distinguished by cause.
- **Portable identity** (`C/security/PortableIdentity.kt`, shared with web and
  desktop, `www/docs/portable-identities.md`): PBKDF2-HMAC-SHA-256 600000
  iterations, 16-byte salt, AES-256-GCM with associated data
  `forkmesh.portable-identity/v1`. Restore validates type, schema, a canonical
  32-byte seed and the matching public key, and signs out the old session.

## 4. Upgrading the Flutter APK in place

Users of the retired Flutter APK get the native app as an update of the same
app. The release APK is built with `-Pforkmesh.appId=com.forkmesh.forkmesh`
and signed with the v3 key (`native/android/README.md`); the default
application id `com.forkmesh.nativeapp` installs beside it.

- **Same application id and signing key.** Keystore keys and app data survive
  only because the id and signer stay the same. The launcher label follows the
  id (`app/build.gradle.kts`).
- **Component names.** A launcher alias `com.forkmesh.forkmesh.MainActivity`
  and an autofill service `com.forkmesh.forkmesh.vault.VaultAutofillService`
  keep Flutter's component names, so pinned icons and the chosen autofill
  service keep working.
- **Version.** Above every Flutter version name and versionCode.
- **Data migration** (`C/migration`, `A/feature/account/projects/FlutterDataMigration.kt`),
  on first launch before any store is read, and from `MY_PACKAGE_REPLACED`:
  - `flutter_secure_storage` (RSA-OAEP or PKCS#1 key wrapping, AES-GCM or
    AES-CBC values, all marker locations), including the
    EncryptedSharedPreferences layout read without Tink (AES-SIV and CMAC in
    core, checked against RFC vectors and a Tink fixture);
  - `shared_preferences` `flutter.<key>` values in both list encodings;
  - rules: idempotent; defers when the Keystore is unavailable; a Flutter value
    wins until the migration is done; every copy is read back; Flutter data is
    deleted only after the relay accepts the migrated session
    (`GET /api/accounts/devices`) and never while a Flutter secret failed to
    decrypt;
  - local repositories stay at `app_flutter/forkmesh/repos` and
    `projects/<slug>/forkmesh/repos` (`FlutterFiles`, `LocalRepoStore.rootIn`);
  - vault device keys, autofill Keystore aliases and
    `noBackupFilesDir/vault-autofill` keep Flutter's names.
- **Retired hosts.** A stored project on a retired `next.*` host folds into the
  current instance at launch; scoped secrets, session, guest flag, background
  switch and ping ledger move when only the old project has a session, and a
  differing value is kept under `fold/replaced/`.

## 5. Accounts and sessions

- **Sign-in binds the phone** as a `mobile_node`: `deviceSig` over
  `forkmesh-device-bind-v1\n{account}\n{pubkey}\n{deviceTs}`. An email
  identifier first logs in without a key to learn the account, then binds; the
  intermediate session is revoked best effort. `device_key_conflict` rotates
  the key and retries once.
- **401 handling.** A plain expired session clears local auth. `device_revoked`
  or an invalid device-bound session also rotates the key and shows the
  "signed out from the web" banner. A 401 from `/api/repo/` signs out only on
  `invalid_session` or `device_revoked`, because owner-signed inbox reads answer
  `unauthorized` to every app session. iOS `ApiService.endsSession` matches.
- **Sign-out** clears local state first, then revokes the token best effort.
- **Login** has "Forgot password?" (`AuthController.forgotPasswordUrl`, the
  relay's `/forgot-password`). **Sign up** opens `/signup` in the browser and
  switches the flow to the login form with a "log in here" hint, so returning
  to the app lands on login.
- **Update prompt.** `ClientVersionGate.DEFAULT_STORE_URL` is
  `https://forkmesh.com/mobile` (`/download` returns 404) and is used when
  `/api/status` sends no `androidDownloadUrl`.
- **FmNeedsDesktopNote** ends with "Get the desktop app"
  (`ProjectActions.desktopUrl`, the project homepage's `/desktop`). Route
  placeholders say the screen "is not in this version of the app yet".
- **Environment** (`C/api/AppEnv.kt`): anything but an explicit dev marker is
  prod. Release builds ignore a stored relay override.
- **Guests** post only in `#general` and the local mesh room, named "Guest
  ####" (SHA-256 of the base64url node key, first four bytes big-endian,
  1000 + value % 9000).
- **Appearance** (`appearance/themeMode`, `appearance/accent`) is global.

## 6. Network, events and push

- **WebSocket** (`C/net/WebSocketClient.kt`): RFC 6455 over
  `Socket`/`SSLSocket` with SNI and hostname verification, one reader thread,
  exactly one `onClosed`; protocol violations close with 1002/1007/1009. It does
  not follow redirects.
- **Account event channel** (`/api/nodes/events`, ticket from
  `GET /api/accounts/event-ticket`): payload-free frames naming a topic; the
  app answers with the read it would otherwise poll for. Keepalive 4 min.
- **Background connection** (`A/feature/push`, `C/push`), used instead of FCM:
  - foreground service type `remoteMessaging` (`dataSync` is time-capped on
    Android 15+);
  - one socket per signed-in project with the switch on; backoff 2 s doubling
    to 5 min with ±30% jitter; paused by Doze and network loss; no wakelocks;
    `START_STICKY`;
  - a persisted 15-minute `JobScheduler` revive job; when a foreground restart
    is refused it reads each inbox and posts pings itself;
  - started on boot and `MY_PACKAGE_REPLACED`;
  - mesh-only mode with no signed-in project keeps the process alive for the
    `#mesh` room ([MESH_PUSH.md](MESH_PUSH.md)); off when notifications or the
    project's background switch are off;
  - posting follows the server's rules (`enqueue_notification`,
    `ping_quiet_until`): one post per ping id, reads elsewhere cancel, nothing
    on the first read after install, nothing older than 24 h, pause, quiet
    hours and muted kinds from cached ping settings.
- **Ping snapshots.** Every fetched ping page is saved per project and account
  (last 100 rows, `inbox/pings.v1/<project>/<node>`, `PingSnapshots`), so the
  Inbox shows at once. A notification carries the whole row
  (`PushTarget.EXTRA_ROW`); a tap inserts it and navigates before marking read.
- **Direct messages** (`C/chat/DirectMessages.kt`, rules in
  `docs/app/chat-private-rooms.md`):
  - one socket to the selected DM and every DM with unread messages; reconnect
    1 s doubling to 5 min; selecting marks read;
  - frames sent before `connect` returns are queued and flushed;
  - key advertisements are never acknowledged; the first sealed frame not
    wrapped for this device triggers one persisted advertisement;
  - up to 64 own durable frames are kept 7 days and re-sealed with
    `resend: true` when a new peer key appears, followed by
    `forkmesh-superseded`;
  - DM notifications decrypt on the phone: a `direct-messages` frame names
    `conversation` and `ref`; the service reads
    `/api/chat/direct-messages/<id>/pending?ref=<ref>` (side-effect free),
    decrypts with `RoomCrypto` and unseals with the existing device key
    (`RoomSeal.existing`). One `MessagingStyle` notification per conversation,
    lock-screen version "New message". The "Notification content" setting
    (`notifications/content`) chooses name and message, name only, or neither.
    Any failure falls back to a generic notification.
- **Relay room** (`C/chat/RelayRoom.kt`): every frame encrypted twice
  (`RoomCrypto` room key and `RoomSeal` to the room's devices), except public
  `#channel` chat. Dial timeout 15 s; ping every 25 s; reconnect 1, 2, 4, 8,
  16, then 30 s; presence every 60 s, peers dropped after 180 s. Envelope
  `version` matches iOS.
- **RoomCrypto**: AES-256-GCM, key PBKDF2-HMAC-SHA256(passphrase or app key,
  salt SHA-256("ForkMesh room:" + room)[0..16], 210000 rounds), cached per
  (room, secret).
- **RoomSeal**: a persistent X25519 key per installation; each protected frame
  gets a fresh content key wrapped per known device.
- **Local mesh** (`C/chat/LocalMeshRoom.kt`): byte-compatible with the
  desktop's `LocalMeshNode`. Encrypted multicast beacon (TTL 1), direct TCP
  links, local-address checks before any socket, subnet broadcast and a paced
  /24 sweep where multicast is filtered, Bluetooth RFCOMM, and a BLE scan for
  the iPhone's GATT service (`C/chat/MeshBle.kt`, wire format in
  [networking.md](../desktop/networking.md)). Needs a Wi-Fi multicast lock.
- **Node sync** (`C/chat/NodeSyncEngine.kt`, `NodeSyncApplier.kt`):
  `GET /api/sync` is destructive, so the phone stands by while a desktop node
  is enrolled. Poll 60 s, backoff 2 s to 30 s with ±25% jitter. Requests sign
  `forkmesh-issues-pull-v1\n{owner}\n{ts}`. An item is acked only after it is
  durably committed. Written formats match the desktop's stores and
  `server/intake.go` (issue JSON with four-space indent and sorted keys;
  pulls only on `forkmesh/pulls`).
- **Chat retention.** `POST /api/accounts/chat-retention` reads and writes the
  account's retention (default 7 days, clamped 1 hour to 7 days, per-chat
  overrides keyed by conversation string or `channel:<roomId>`). A message is
  expired when `ts <= now - retention` (`C/model/ChatRetention.kt`); every room
  refuses expired messages and the app prunes every 5 minutes. Cached per
  account (`chat_retention/<account>`).
- **Pull lookup.** `ApiService.publishedPull` reads one pull from a fresh list,
  else `pull.md` and events on the ledger, else `forkmesh/pull/<n>`; the list
  reads the gateway's `pullRefs`. The detail screen retries a miss with
  backoff.
- **Sponsored ads** carry no viewer data, cache 30 min, render plain text.
  **Chat links** with `javascript:`, `file:` or `data:` targets show as words.

## 7. Cryptography and wire compatibility

- JCA where Android 26 has it (AES-GCM, SHA-2, HMAC, PBKDF2); pure Kotlin
  otherwise: Ed25519 (RFC 8032, constant time; strict verify as
  `app/src/collaboration.py`), X25519 (constant-time ladder), SHA-3/SHAKE,
  ML-KEM-768 (FIPS 203, interoperable with OpenSSL, Go and the desktop).
- PBKDF2 uses JCA only for non-empty 7-bit ASCII passwords, else a byte-exact
  HMAC loop. HMAC with an empty key equals one zero byte (RFC 2104).
- The retired Dart client's behaviour is reproduced where it reaches the wire
  or storage: Dart's `String.trim`, base64 acceptance, UTF-8 with U+FFFD for
  lone surrogates, `jsonEncode` output (`C/json/Json.kt`), whole doubles with
  `.0`.
- **Catalog signatures** (`C/localrepos/CatalogCanonical.kt`) reproduce
  CPython's `json.dumps(record, sort_keys=True, separators=(",", ":"))` with
  `ensure_ascii` exactly; `clean_string` truncates by code point.
- **Trust lists** (`C/security/TrustLists.kt`) are pinned to
  `conformance/trust-lists/v1/vectors.json`. **Sync status**
  (`C/api/SyncStatus.kt`) uses only counts, timestamps and closed vocabularies,
  pinned to `conformance/sync-status/v1/model.json`.
- **Vault** (`fmv1`): Argon2id and BLAKE2b in pure Kotlin, checked against RFC
  9106/OpenSSL vectors and the interop files. **Coves** open the desktop's files
  with the iOS fixtures.
- **QR**: encoder bit-exact with the desktop; decoder handles rotation,
  perspective, inversion and Reed-Solomon repair.

## 8. Git engine and repositories

- A pure-Kotlin Git engine (`C/git`), blocking, run off the main thread.
  `GitIsolateRunner` serializes operations per repository; cancel trips the
  operation's token and returns only after the worker stops.
- **Fail-closed transport** (`C/git/transport/IoHttpTransport.kt`): https only,
  no user-info, strict TLS, zero redirects (unless `surfaceRedirects`), no
  cache or decompression, bounded headers and bodies, one connection per
  request, a watchdog that closes sockets on cancel or timeout.
- **Protocol**: no thin packs on fetch; push requires `report-status(-v2)` and
  never `delete-refs`; fetch refspecs may only write `refs/remotes/<remote>/`.
- **Locks**: stale `.lock` files at known engine paths older than a threshold
  are removed on open.
- **SHA-1 collision screening** (`C/git/crypto/Sha1Dc*.kt`, a port of
  sha1collisiondetection). Untrusted data without a detector fails with
  `hash_screening_unavailable`.
- **Clone to device** uses the public owner and
  `GET /api/repo/<owner>/<repo>/mirrors` (healthy, fastest first, relay last,
  failover on errors). Clones are shallow single-branch depth 1
  (`GitWorkspace.meshCloneDepth`) with `GitWorkspace.deviceLimits` (1 GiB pack
  and inflated, 256 MiB object, 30 s connect, 60 s idle, 20 min deadline).
  History walks stop at `.git/shallow`.
- **One copy per repository.** A repository is keyed by `publishedAs`
  (case-insensitive); a second clone is refused. Sync is fetch then
  fast-forward only (`GitWorkspaceOps.pull`, `C/localrepos/LocalSync.kt`).
- **Mirror redirect** (`C/localrepos/ForkMeshRedirect.kt`): follows exactly one
  `307` from `GET …/info/refs?service=git-upload-pack` to a credential-free
  https mirror and sends that operation's later requests to the mirror
  unsigned. Any other redirect is `remoteRejected`; `origin` stays the relay.
- **Shallow push** (`C/localrepos/ShallowPushProof.kt`): on `nonFastForward`
  from a shallow clone, fetch, ask `GitRepository.ancestry`, deepen 50/250/1000
  then `unshallow` until known; then retry, `push_diverged`, or
  `shallow_history_limit`.
- **Packed objects on demand** (`C/git/pack/PackObjectStore.kt`): `.idx`
  lookup, memory-mapped pack, delta chains up to 4096 deep against a 32 MB
  per-pack base cache, hash (and screen) every result. Fetch and clone still
  parse the whole received pack.
- **Checkout** writes only changed paths without per-file fsync and reports
  progress.
- **Local store** (`LocalRepoStore`): names `[A-Za-z0-9._-]{1,100}`, never via
  symlinks; delete removes only a direct child; imports never adopt hooks,
  config, objects, symlinks or special files. Folders are the authority:
  records are rebased onto the current root and unlisted `<root>/<name>/.git`
  folders adopted.
- **Push signing** (`C/localrepos/ForkMeshRemote.kt`):
  `forkmesh-git-push-v1\n{owner}\n{repo}\n{method}\n{ts}\n{sha256hex(body)}`.
- **Publishing** (`PublishService`): the record matches the Worker's
  `safe_catalog_record` exactly; `stateHash`/`stateSig` are omitted; the
  catalog `nodeId` is a stable per-install id; clones publish `forkedFrom`.
- **Pull requests** are `PatchBuilder` patches for `git apply --index --3way`
  with `index old..new mode` lines; the branch is pushed first.
- **Quick edit** refuses binary, non-UTF-8, oversized or mismatched blobs.
  **Zip import** reads the central directory (zip64), keeps the first
  duplicate, and bounds every read.
- **Provider mirrors**: `ProviderMirrorScreen` over `C/ci/ProviderMirror.kt`
  ([../app/provider-mirrors.md](../app/provider-mirrors.md)).

## 9. Signed writes

- Inbox writes (`C/api/InboxService.kt`) mirror the desktop's
  `submit*ToInbox` and the Worker's `verify_issue_event`, `verify_pull_event`
  and `verify_pull_comment_event`; canonical strings must match
  `app/src/entry.py` byte for byte or the relay answers 401 `bad_signature`.
- Desktop commands (`C/control/DesktopCommandContract.kt`) post without a
  session token, never sign out on 401, refuse unsupported commands before
  signing, and appear only after the route probe answers.
- Destructive host and node operations use the desktop's prepare/run one-use
  token and a typed name after the screen lock (`UserPresence.confirm`).
  Approving an agent request always opens the app; only Deny works from a
  notification.

## 10. Platform choices

- **Haptics** map to `VIRTUAL_KEY`, `KEYBOARD_TAP`, `CONTEXT_CLICK`, all
  suppressed under reduced motion.
- **Timers** ring through `AlarmManager` (`TimerAlarmReceiver`, rescheduled
  after boot) with `SCHEDULE_EXACT_ALARM`; they are device-wide.
- **Glasses Alert** (`feature/glasses/`, core `glasses/`) scans in a
  `connectedDevice` foreground service with manufacturer-data filters (Android
  pauses unfiltered scans with the screen off). The alarm uses `USAGE_ALARM`,
  raises `STREAM_ALARM` while ringing and, with notification-policy access,
  allows alarms through DND. In the foreground an unfiltered census scan feeds
  the device grid (`GlassesCensus`); `GlassesHoming` and `GlassesCompass` drive
  closeness and direction; `GlassesRadarChip` sits in the top bar. Device kinds
  match iOS (`native/ios/docs/FMTools.md`).
- **Notifications** need `POST_NOTIFICATIONS` on 13+; asked after sign-in and
  when a timer starts.
- **Crash reports** come from `ApplicationExitInfo`. **Stall log** is
  `files/diagnostics/stalls.log`; sampling runs only while an activity is
  visible.
- **App Links** for `app.forkmesh.com` and `forkmesh.com` need
  `/.well-known/assetlinks.json` with the v3 release signer
  (`DEFAULT_SIGNER_SHA256` in `tools/forkmesh-release-host.py`).
- **Feedback is inline, never a toast.** There is no `Toast`, snackbar or
  floating notice. Results go into an `FmStatus` (`A/ui/kit/FmInlineStatus.kt`)
  held by the screen, sheet or row that ran the action and drawn in layout
  flow, with optional action and dismiss buttons; errors stay until dismissed.
  Actions with no owning screen report where the thing is listed
  (`NavigationFeature.linkProblem`, `InboxFeature.actionProblem`,
  `agentsNotice`). The vault autofill activity shows its error inside its own
  dialog.
- **Pasting images into chat** goes through `PhotoIntake.prepare` (2048 px
  JPEG, no metadata) and `ChatHub.sendAttachment`.

## 11. Differences kept on purpose

- No APNs/FCM: the background connection replaces push. Channel messages get
  no notification on Android because the account socket carries no channel
  topic.
- No first-run choice screen or one-tap account switch (iOS has neither).
- Worktrees list-only, milestones and database admin read-only, as on iOS.
- `minSdk` 26.
- Chat and ping times use a 12-hour clock; agent prompts keep 24-hour stamps.
- The prompt composer has no mic button; dictation is `/dictate`, and a quiet
  stop after words sends the prompt (`SpeechDictation` `onQuiet`).

## 12. Shell prompt targets

- The shell prompt is an overlay over the tab stack and `TabStack` composes
  only the top screen of the selected tab, so a page that registers a prompt
  target in a `DisposableEffect` counts only while on top. The registry is pure
  Kotlin in `:core`; `ComposerFeature` exposes it as a `StateFlow`. The mode
  button cycles page target, agent and chat.
- Calorie photos are re-encoded from the composer's stripped JPEG (1024 px for
  meals, 1600 px for receipts) under the server's 1.6 MB limit; the screen
  re-reads the overview before answering.

## 13. Core features as plugins

The Home grid, bar tabs and repository tiles follow Market packages, off by
default in new projects except the chosen starter pack
(`docs/marketplace/core-plugins.md`, "Phone starter packs").

- **One gate.** `C/market/PackageGate.kt` maps hub ids, `Route.Kind`s, bar tabs
  and repository tiles (`RepoHubSection`) to package ids;
  `PackageSurfaces.of(installed)` returns what to hide. Repos, Search, Market,
  Settings, Projects, Set status and Ads are never gated. Every gated package
  also needs `mobile` among its places ([NAVIGATION.md](NAVIGATION.md)).
- **Effective install state.** `AccountPackagesStore.isInstalled` uses the
  account row, else `PackageGate.fallback` (installed when `default`, or when
  `installedBefore` and the project is legacy, or a package of the project's
  `StarterPack` in a new project). In a new project a pack package stays
  installed until the project removes it. Dependencies apply. The cache is per
  project (`ProjectScope.prefs`).
- **Starter pack.** `StarterPack` (`MINIMAL` = `code`; `STANDARD` =
  `PackageGate.STARTER`: agents, chat, inbox, code, tour; `EVERYTHING` = every
  gated package except the organization packages) sits beside `STARTER` in
  `C/market/PackageGate.kt`. A new project with no `packages/starterPack` in
  its prefs shows `A/feature/market/StarterPackScreen.kt` once ("Choose how to
  start", Standard preselected, Continue) from `Features.Root`, driven by
  `PackageGateFeature.starterPackPending`; Continue stores the pack id
  (`PackageGateFeature.chooseStarterPack`) and republishes the gate. Unset
  reads as `STANDARD`; legacy projects are never asked. `MarketModel.starterPack`
  feeds the same pack to the Market's fallback. Same ids and contents as iOS.
- **Legacy marker.** `PackageGate.legacyProject` writes
  `packages/legacyProject` once per project, before other features install,
  from existing prefs or a saved session; it never changes.
- **State.** `A/feature/market/PackageGateFeature.kt` publishes
  `surfaces: StateFlow<PackageSurfaces>`, rebuilt on account, sign-in or
  organization changes.
- **Surfaces gated**: hub, menu, bottom bar and rail, shortcuts, the new agent
  button, global search sources, command palette, launcher shortcuts and
  repository tiles. Removing the selected tab's package moves to Agents or
  Repos. Routes stay registered, so links and widgets still open hidden
  features.
- **Organization packages** (CRM, Marketing, Analytics, Competitors) show only
  when one of the user's organizations installed them (`/api/plugins/<org>`,
  refreshed at most every ten minutes, cached in
  `packages/organization/installed`).
- **Sign in** is per phone (`packages/sign-in/on`, on until removed, whatever
  the pack); Settings always keeps Sign out and Log in so nobody is locked out.
- **Market.** `A/feature/market/MarketScreens.kt` and `C/market/MarketModels.kt`
  offer the web Market's sorts and filters, usage from
  `/api/market/installs` and `/api/market/reviews/summary`, and one-tap
  install (`MarketModel.cardAction`). Desktop-only packages
  (`PackageInstallMode.DESKTOP`) offer no phone install. The list pads the
  safe area and insets so the last row is reachable.
  Sorts include Most opened and Installed first, as on iOS. Installed cards
  show Uninstall and each package page has a ⋯ menu with Rate and Uninstall,
  both driven by `MarketModel.uninstallAction` and confirmed through
  `A/feature/market/PackageUninstall.kt`: account packages leave every
  device, per-phone and organization packages with a phone switch hide on
  this phone, and the organization-wide removal stays on the package page's
  Organizations section.
- **Face Changer.** `A/feature/facechanger/` hosts `/dashboard/face-changer`
  in a WebView limited to the server's host (`C/market/FaceChangerPage.kt`),
  grants the camera and microphone only there after the runtime prompt, and
  receives shared clips over `FmShareBridge`. `C/market/ShareTransfer.kt`
  validates each begin, chunk and end message (media type, 512 MiB cap, safe
  name); the file goes to the `face-changer-share` cache path and out through
  an `ACTION_SEND` chooser.

## 14. Chat features

- **View-once video and voice** (DMs only, `docs/app/chat-private-rooms.md`):
  `C/chat/ViewOnceMedia.kt`, `A/feature/chat/ViewOnceMessages.kt`. Media is
  encrypted with a fresh key, uploaded in 512 KiB parts, then announced by a
  sealed `chat` frame. The viewer uses `FLAG_SECURE`, decrypts in memory to one
  temp file, and calls `consume` on playback. `ViewOnceStore` states: `sent`,
  `ready`, `opened`, `expired`, `withdrawn`; only a ready recipient entry holds
  the key. Receipts for unopened view-once frames are held so the relay replays
  them after a restart. `MessageCaptureFilters` is a pass-through seam.
- **Receipts** (`ChatReceipt`, drawn by `A/feature/chat/ChatReceiptMark.kt`):
  Sending, Sent, Delivered, Read. DMs advance on `forkmesh-recipients`,
  `forkmesh-delivered` and `forkmesh-read`; Office rooms stop at Sent; relay
  rooms show Sent.
- **DM replies, reactions, edits, delete for everyone** use the relay-room
  frames sealed through `sendSealed`. Edits and deletes are honoured only from
  the target's author; early ones are held (128 targets, 8 frames each).
  Deletes also go into the persisted hidden-message list and send
  `forkmesh-superseded`.
- **Chat list** (`A/feature/chat/ChatChannelList.kt`): server DMs listed newest
  first with unread pills and avatars; each row has a last-line preview built
  only from messages on the phone (`C/chat/ChatListPreview.kt`).

## 15. You hub, diagnostics and docs

- The hub's last row shows `versionName (versionCode)`, opens `WhatsNewPage`
  from the bundled `changelog.json` ([MAC_BUILD.md](MAC_BUILD.md)), and has
  Send diagnostics (`tools/PerformanceDiagnostics.kt`, `tools/AppChangelog.kt`,
  `feature/tools/AboutAndDiagnostics.kt`).
- **Docs package**: the public docs library from the same bundle iOS ships
  (`native/ios/ForkMesh/Resources/DocsLibrary.json`,
  `forkmesh-docs-library-v1`), copied to `docs/library.json` by the
  `bundleDocsLibrary` Gradle task. `C/docs/` parses, searches (desktop scores)
  and resolves links; `DocsMarkdown` renders a safe Markdown subset;
  `A/feature/docs/` holds the screens. Guests may open it. Tests:
  `DocsLibraryTest`, `NavigationTest`, `PackageGateTest`.

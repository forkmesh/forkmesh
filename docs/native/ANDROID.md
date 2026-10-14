# Native Android app: design notes

`native/android` is the ForkMesh Android app, in Kotlin + Jetpack Compose.
Its source is code only: no comments or KDoc. The
reasons behind non-obvious decisions live here. Build, test and release steps
are in `native/android/README.md`; progress is tracked in
`docs/native/PROGRESS.md` (Android section).

Paths: `A/` is `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/`,
`C/` is `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/`.

## 1. Toolchain and dependencies

- Gradle 9.7.1 (wrapper), Android Gradle Plugin 9.4.0 with built-in Kotlin (do
  not apply `kotlin-android`), Kotlin 2.4.20, Compose BOM 2026.09.00,
  compileSdk/targetSdk 37, minSdk 26. Compose 1.12 needs compileSdk 37.
- First-party dependencies only: AndroidX/Compose from Google and Kotlin
  coroutines from JetBrains. HTTP, JSON, WebSocket, cryptography, the Git
  engine, QR codes and icons are implemented in this tree; nothing else is
  downloaded. The icon font is Apache-2.0; its codepoints and license are in
  `app/src/main/assets/fonts`. Since 2026-10-13 the font (shared with iOS) is
  a subset of Flutter's MaterialIcons-Regular (1.57 MB → 0.11 MB): every
  codepoint name that appears as a word anywhere in the tracked tree, plus
  `help_outline` and the iOS `MaterialIconCodepoints` table (about 880 icons).
  Icon names are app literals only (server fields such as CRM flow icons map
  through `CrmFlowIcon.symbol`), and a name outside the subset draws
  `help_outline`, never a box. The full table is kept in
  `tools/material_icons/MaterialIcons.codepoints`; after adding an icon name,
  run `python3 tools/subset_material_icons.py` (needs fonttools; reads the
  full font from git history at `ae985af229`, or `--font`: the same file is
  Flutter's `bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf`,
  from `https://storage.googleapis.com/flutter_infra_release/flutter/fonts/3012db47f3130e62f7cc0beabff968a33cbec8d8/fonts.zip`), and
  `app/tests/test_material_icon_subset.py` fails until you do. Choices this
  forced:
  - home-screen widgets use `RemoteViews`, not Glance;
  - the QR scanner uses Camera2 directly, not CameraX;
  - the background-connection revive job uses the platform `JobScheduler`,
    not WorkManager (which pulls in Room);
  - video in chat uses the platform `VideoView`, not media3.
- `:core` is pure Kotlin/JVM (no `android.*`) and is tested on the host JVM
  by an in-tree runner, not JUnit (`core/build.gradle.kts` registers
  `coreTest`; Gradle's default test task would find nothing).
  `./gradlew :core:coreTest -Pfilter=Json` runs only classes whose name
  contains `Json`.
- `res/xml/network_security_config.xml`: cleartext is refused everywhere
  except `10.0.2.2`, `localhost` and `127.0.0.1`, for a local development
  server.
- `res/values/themes.xml`: the window theme only paints the background behind
  Compose; `ForkMeshTheme` draws everything else.

## 2. Architecture

- **Feature seams.** `A/feature/Features.kt` is the only place the shell meets
  the areas. Each area owns one `*Feature` object and its package, exposes an
  `install(app)` hook and screen factories, and registers its services in
  `C/core/Services.kt`. This is what let many agents port areas in parallel without editing the same files.
  Cross-area wiring happens in one place, `Features.connectAreas`.
- **Install order**: account (settings, identity,
  owner keys, API, auth), local repos, chat (relay, mesh, node sync), agents,
  then the rest.
- **Projects.** A project switch clears `Services` and runs
  `Features.install` again, so every service is rebuilt for the new instance
  instead of being re-pointed. Each install must be safe to repeat, launch its
  coroutines in `ProjectLifecycle.scope`, and register sockets, threads and
  audio with `ProjectLifecycle.onClose`. Object-level state that outlives a
  switch is a bug (the agents hub once kept the first project's connection).
- **Threads.** `:core` is blocking; the app calls it from `Dispatchers.IO`.
  `C/chat/EventLoop.kt`: one thread owns a service's state, blocking work hops
  to a worker and posts back with a generation check, and virtual time lets
  tests advance timers.
- **Screens.** `A/ui/navigation/Navigator.kt`: a `Screen` is a plain object
  that owns its state, so it survives while lower in the stack. Work that must
  continue after a push (a signed write started from a pushed composer) runs
  on the screen's own scope, not a composition scope, which is cancelled while
  another screen covers it.
- **Navigation.** `C/navigation` holds the `Route` model, the link parser and
  the guest gate. Areas register screens with `NavigationFeature.register`.
  The router applies `GuestGate` to every route whatever its source (hub,
  link, shortcut, widget, tile, notification), so account-only screens always
  ask a guest to sign in first. An unknown `forkmesh://` path opens the home
  tab; an unknown web link goes to the browser. Page names are matched before
  repositories in `forkmesh://` links.

## 3. Storage, secrets and identity

- **Preferences** (`A/platform/Prefs.kt`) keep the key names the retired
  Flutter app wrote (`server/url`, `auth/session`, `appearance/themeMode`,
  ...), so migrated values need no renaming. A value stored with another type
  reads as absent. Sign-out writes synchronously so it survives an
  immediate process death.
- **Secrets** (`A/platform/SecureStore.kt`): each value is sealed with
  AES-256-GCM under a key that lives in the Android Keystore, with the key
  name as associated data (a value cannot be moved to another key). A value
  that no longer opens (backup restored on another device, reset keystore)
  reads as absent instead of throwing. Secrets an older build kept in plain
  preferences are moved into the store once and the plain copy dropped.
- **Per-project isolation.** Production owns the legacy un-prefixed
  namespace; every other project's secrets live under `p/<projectId>/`, and
  sessions under `auth/session/<projectId>`. `Services.get<SecretStore>()`
  returns the active project's scoped view.
- **Node identity** (`C/security/Identity.kt`): the Ed25519 public key,
  base64url without padding, is the node id (chat sender, repository owner
  namespace, `author` of signed inbox events). Stored as padded standard
  base64 of the 32-byte seed under `identity/ed25519Seed` (the migrated
  format). A
  corrupt stored value throws rather than silently replacing the device key;
  `SecurityBootstrap` then leaves it unregistered so sign-in reports the key
  is not ready instead of crashing every launch. The key is rotated only when
  the Worker reports `device_revoked` or `device_key_conflict`: a revoked
  device row is permanent. Local repositories are never touched by a rotation.
- **Owner encryption** (`C/security/OwnerCrypto.kt`): agent sessions,
  transcripts and prompts travel as `forkmesh.owner-sealed` envelopes. One
  AES-256-GCM content key is wrapped per owner device under hybrid X25519 +
  ML-KEM-768; wrap key = HKDF-SHA256(ssX25519 || ssMlKem, salt
  `forkmesh-mirror-kem-v1`, info ephPub || mlkemCiphertext), so mixing wrap
  fields between recipients yields nothing. Stored as
  `{"v":1,"x25519Pub","x25519Priv","mlkemPub","mlkemPriv"}` (padded base64,
  X25519 private key stored clamped). Key id =
  base64url(SHA-256(x25519Pub || mlkemPub)). Recipient cap matches
  `MAX_OWNER_ENVELOPE_RECIPIENTS` in `app/src/security_controls.py`. A failed
  open is never distinguished by cause.
- **Portable identity** (`C/security/PortableIdentity.kt`): the recovery kit
  shared with the web client and the desktop (`www/docs/portable-identities.md`):
  PBKDF2-HMAC-SHA-256, 600000 iterations, 16-byte salt, AES-256-GCM with
  associated data `forkmesh.portable-identity/v1`. Restoring checks the payload
  type and schema, a 32-byte canonical base64url seed and a matching public
  key, and signs out the session bound to the old key.

## 4. Upgrading the Flutter APK in place

Users of the retired Flutter APK (`com.forkmesh.forkmesh`) get the native app
as an update of the same app. Release step: build with
`-Pforkmesh.appId=com.forkmesh.forkmesh`, signed with the v3 key
(`native/android/README.md`); the default id `com.forkmesh.nativeapp` is for
builds that install beside it.

- **Same application id and signing key.** Keystore keys and app data survive
  only because `com.forkmesh.forkmesh` and the v3 signer stay the same. The id
  is switched with `-Pforkmesh.appId`; the launcher label follows it
  (`app/build.gradle.kts`), so it never shows "ForkMesh Native" in the real app.
- **Component names.** A launcher alias `com.forkmesh.forkmesh.MainActivity`
  and an autofill service declared as `com.forkmesh.forkmesh.vault.VaultAutofillService`
  (a subclass of the native one) keep Flutter's component names, so pinned
  home-screen icons and the user's chosen autofill service keep working.
- **Version.** Default 1.4.0 / 10400: above Flutter's 1.3.1 name and the
  release host's versionCode for v1.2.4 (10204); iOS native also ships 1.4.0.
- **Data migration** (`C/migration`, `A/feature/account/projects/FlutterDataMigration.kt`),
  first launch, before any service reads a store, also run from
  `MY_PACKAGE_REPLACED` so it happens before the first open:
  - `flutter_secure_storage` 10.3.4: RSA-OAEP or PKCS#1 key wrapping,
    AES-GCM or AES-CBC values, all three places the algorithm markers can
    live, and 9.x's misspelled key name. Also the EncryptedSharedPreferences
    layout the Flutter app used from 2026-08-08 to 2026-09-14, read without
    Tink (AES-SIV and CMAC in core, checked against RFC vectors and a fixture
    made by real Tink 1.7.0). Version 10 leaves the old ESP keysets behind, so
    only entries not already in the new format are tried as ESP; otherwise
    every secret also counts as undecryptable and cleanup never happens.
  - `shared_preferences_android` 2.4.28 `flutter.<key>` values, both list
    encodings.
  - Rules: idempotent; defers without writing when the Keystore is
    unavailable; a Flutter value wins over a native one until the migration is
    marked done; every copied value is read back before it counts; Flutter
    data is deleted only after the relay accepts a migrated session
    (`GET /api/accounts/devices`) and never while any Flutter secret failed to
    decrypt. A migration that found no session (a guest, or only an identity
    or vault) keeps the Flutter data; an empty migration is cleaned up at once.
  - Local repositories stay where Flutter kept them:
    `app_flutter/forkmesh/repos`, and `projects/<slug>/forkmesh/repos` per
    project (`FlutterFiles`, `LocalRepoStore.rootIn`).
  - Vault device keys (`vault/<account>/quickUnlock|autofill|autoLockMinutes`)
    and the autofill Keystore aliases and `noBackupFilesDir/vault-autofill`
    files keep Flutter's names, so device unlock and autofill carry over.
- **Next fold.** After the 2026-09-24 cutover `app.forkmesh.com` is served by
  the Next stack, so the built-in "ForkMesh Next" project is gone and fresh
  installs start on ForkMesh. A stored Next project (`app.next.forkmesh.com`
  or any `next.*` host) folds into ForkMesh at every launch while one remains:
  if only Next has a session its scoped secrets, session, guest flag,
  background-connection switch and ping ledger move to ForkMesh (a differing
  ForkMesh value is kept under `fold/replaced/`); if both have sessions,
  ForkMesh's is kept and only the Next entry is dropped. A guest on Next stays
  a guest on ForkMesh.

## 5. Accounts and sessions

- **Sign-in binds the phone** as a `mobile_node`: `deviceSig` over
  `forkmesh-device-bind-v1\n{account}\n{pubkey}\n{deviceTs}`. An email
  identifier first logs in without a key to learn the account name, then
  binds; the intermediate session is revoked best effort.
  `device_key_conflict` rotates the key and retries exactly once.
- **401 handling.** A plain expired session clears local auth. `device_revoked`,
  or any invalid session on a device-bound session, also rotates the key and
  shows the one-shot "signed out from the web" banner. A 401 from an
  `/api/repo/` path signs out only on `invalid_session` or `device_revoked`:
  the owner-signed inbox reads (`/issues`, `/pulls` GET) answer `unauthorized`
  to every app session, and treating that as a dead session sent the user back
  to the login screen as soon as a repository was opened (2026-09-29). iOS
  `ApiService.endsSession` does the same.
- **Sign-out** clears local state first so a slow Worker cannot keep the
  signed-in shell up, then revokes the token best effort.
- **Environment.** `C/api/AppEnv.kt`: anything but an explicit dev marker is
  prod, so a typo cannot ship a build aimed at a developer machine. Release
  builds ignore a stored relay override, and a relay URL from another
  environment is dropped only when the environment actually changes.
- **Design preview.** The empty-form login shortcut exists only in debug
  builds; release builds always validate.
- **Guests** may post only in `#general` and the local mesh room. A guest's
  chat name is "Guest ####": SHA-256 of the base64url node key, first four
  bytes big-endian, 1000 + value % 9000, as the desktop derives it. The node
  key is never shown as the person.
- **Appearance** (`appearance/themeMode`, `appearance/accent`) is global, not
  per project.

## 6. Network, events and push

- **WebSocket** (`C/net/WebSocketClient.kt`): RFC 6455 over `Socket`/`SSLSocket`
  with SNI and hostname verification; one daemon reader thread; exactly one
  `onClosed`; protocol violations close with 1002/1007/1009.
- **Account event channel** (`/api/nodes/events`, ticket from
  `GET /api/accounts/event-ticket`): payload-free frames naming a topic
  (`pings`, `issues`, `direct-messages`); the app answers with the read it
  would otherwise poll for. Keepalive 4 min. Direct-message frames carry no
  sender or text; since 2026-10-13 they name the conversation, the relay
  `ref` and `keyVersion`, and the background link decrypts the message on
  the phone (section 15). When that is not possible (no `ref`, row already
  received, key missing) the app reads `GET /api/chat/direct-messages`
  and takes the newest conversation with unread messages as the sender: the
  notification then says "Direct message from <name>" and carries that
  account's avatar (`accountAvatarPng`) as its large icon. When that read
  fails it says only "New direct message". It cannot take an inline reply.
  The same read gives the conversation id, carried in the notification's
  href (`/chat/direct-messages/<id>`, `directMessageTargetConversation`) so a
  tap re-lists the DMs and opens `direct:<id>` instead of guessing from the
  relay's `@…` unread list. The id is not part of the notification id, so
  there is still one DM notification per project. In the foreground the same
  topic re-lists the DMs (`ChatDepth`).
- **Direct messages** (`C/chat/DirectMessages.kt`, wired in
  `A/feature/chat/ChatDepth.kt` beside the Office service; closed on sign-out
  and project close). The client rules are in
  `docs/app/chat-private-rooms.md` ("Server copies of direct messages"):
  - Sockets: a message is only received over a socket to its room, so the
    service keeps one open to the selected DM and to every DM with unread
    messages (`keepOpen` after each list read). A dropped socket that is still
    wanted reconnects after 1 s doubling to 5 min, reset at
    `forkmesh-history-end`. Opening a socket no longer marks the DM read;
    selecting it does. Selecting a `direct:` id the list does not know re-lists
    first.
  - Receipts and every other frame sent before `connect` returns are queued
    and flushed once the socket is set: `WebSocketClient` starts its reader
    before returning, so the history replay and `forkmesh-history-end` can
    arrive first, and their receipts used to be dropped.
  - R1: a key advertisement is never acknowledged, so it stays on the relay
    for the other person's other devices. R2: the first sealed frame per
    connection not wrapped for this device's key gets one persisted key
    advertisement, past the once-a-day throttle; that is what tells the
    sender to re-seal. The advertisement on connect is persisted at most once
    a day (`shouldPersistAdvertisement`), as on the relay room.
  - R3: each connection keeps up to 64 of this device's own durable frames
    for 7 days (from each send, and from own frames in the history replay,
    which are the ones still undelivered). After `forkmesh-history-end` and
    whenever a new peer key is learned, an entry not wrapped for every known
    peer key is re-sealed and sent with `resend: true`, then its old refs go
    in `forkmesh-superseded`. `stored: 0` clears the outbox. The message id is
    unchanged, so a receiver shows a resend it already has once.
- **Background connection** (`A/feature/push`, `C/push`), the user's choice
  over FCM (no Google dependency):
  - Foreground service type `remoteMessaging`: `dataSync` is capped at about
    6 h/day on Android 15+ and cannot start from `BOOT_COMPLETED` there.
  - One socket per signed-in project with the switch on; backoff 2 s doubling
    to 5 min with ±30 % jitter, reset on connect; paused by Doze and network
    loss, reconnected at once when they end; no wakelocks; `START_STICKY`.
  - Revive: a persisted 15-minute `JobScheduler` job. Android 12+ forbids
    starting a foreground service from a background job, so when the restart
    is refused the job reads each project's inbox and posts the pings itself.
  - Started on boot and on `MY_PACKAGE_REPLACED` (the Flutter-to-native
    update); its sockets close on sign-out or when the relay rejects the
    session.
  - Mesh-only mode (2026-10-06): with no signed-in project, or a guest, the
    service still runs, titled "Listening on #mesh", with no sockets. It only
    keeps the process alive, so the `#mesh` room keeps listening and posts
    nearby messages (see [MESH_PUSH.md](MESH_PUSH.md#local-alerts-over-the-mesh)).
    It stops when notifications are off, or when a signed-in project has its
    background switch turned off: that switch is the user saying "no
    background service", and it covers the mesh too.
  - Posting rules follow the server (`enqueue_notification`,
    `ping_quiet_until`): one post per ping id; a ping read elsewhere cancels
    its notification; the first read after install or re-enable posts
    nothing; pings older than 24 h never post; pause, quiet hours (Monday = 0)
    and muted kinds from the ping settings, cached 15 min, and everything
    posts if they cannot be read.
- **Relay room** (`C/chat/RelayRoom.kt`): every frame is encrypted twice,
  `RoomCrypto` with the room key and `RoomSeal` to the room's devices, except
  public `#channel` chat, which stays readable to the controller for
  integrations. Dial timeout 15 s; ping every 25 s answered within 25 s;
  reconnect 1, 2, 4, 8, 16 then 30 s; presence every 60 s and peers dropped
  after 180 s. Resuming from the background reconnects at once. The shared
  room key is fetched from the relay once per room. `version` in every
  envelope is the one the iOS app sends, so the relay cannot tell the apps
  apart. The WebSocket client does not follow redirects, so a relay host that
  answers 308 leaves chat connecting (the reason the Next fold matters).
- **RoomCrypto**: AES-256-GCM, key PBKDF2-HMAC-SHA256(passphrase or baked app
  key, salt = SHA-256("ForkMesh room:" + room)[0..16], 210000 rounds). 210000
  rounds cost seconds on a phone and the relay reopens on every reconnect, so
  keys are cached per (room, secret) and derived once.
- **RoomSeal**: a persistent X25519 key per installation; each protected frame
  gets a fresh content key wrapped for every known device with an X25519/HKDF
  pairwise key, so the relay can store and forward but not read.
- **Local mesh** (`C/chat/LocalMeshRoom.kt`): byte-compatible with the
  desktop's LocalMeshNode. Encrypted multicast beacon with TTL 1 (no router
  forwards it), direct TCP links, and every peer address checked as local
  before a socket opens. Where multicast is filtered: subnet broadcast, a
  paced /24 sweep, reply beacons and a fallback dial. Bluetooth RFCOMM
  (service UUID shared with the desktop) works with no network at all; it
  only carries frames that are already encrypted. After each RFCOMM search
  the phone also runs a 15 s BLE scan (Android 12+, `BLUETOOTH_SCAN`) for
  the iPhone's GATT service and dials it as a central (`C/chat/MeshBle.kt`;
  wire format in [networking.md](../desktop/networking.md#over-bluetooth)). Android needs a Wi-Fi
  multicast lock to receive the beacon.
- **Node sync** (`C/chat/NodeSyncEngine.kt`, `NodeSyncApplier.kt`):
  `GET /api/sync` is destructive on the relay (claims inbox rows for five
  minutes, deletes About edits and legacy agent prompts on read), so the phone
  stands by whenever a desktop node is enrolled. Poll every 60 s, 20 s
  timeouts, socket backoff 2 s to 30 s with ±25 % full jitter so a fleet does
  not reconnect in lockstep. Both routes sign `forkmesh-issues-pull-v1\n{owner}\n{ts}`.
  Ack contract: returning true promises the item is durably committed; only
  then is its row acked by exact id. Only issues, pulls and discussions are
  ever acked. Written formats are the desktop's (`IssueStore`,
  `DiscussionStore`, `PullStore`) and the mirror daemon's (`server/intake.go`):
  issue JSON with four-space indent, sorted keys and trailing newline;
  discussion and pull front matter one key per line (values never carry a
  newline); pulls only on the `forkmesh/pulls` ledger branch; nothing pushed.
  An applied-item ledger (fingerprints) recognises a re-offered row whose ack
  never landed.
- **Sponsored ads**: the serve call carries no viewer data at all (no
  `Authorization`, account or repository); results are cached 30 min and
  never retried in between; every field is plain text; a failure renders
  nothing.
- **Chat links**: `javascript:`, `file:` and `data:` targets are shown as
  plain words, as the web and desktop renderers do.
- **Opening a pull request from a push (2026-09-28)**: tapping a "new pull
  request" notification showed "Pull request #N is not published." or loaded
  slowly. The detail screen listed every pull on the `forkmesh/pulls` ledger
  and picked the number, and that list is cached for 60 s, so a list read
  before the push lacked the new PR; a brand-new PR also often lives only on
  its own `forkmesh/pull/<n>` branch until the ledger catches up.
  `ApiService.publishedPull` now reads just that PR: from a still-fresh,
  already-loaded list if it has the number, otherwise `pull.md` and events at
  the ledger, then at `forkmesh/pull/<n>`. A hit drops the cached list and the
  ledger's `.forkmesh/pulls` tree so Back shows the new PR on top; an offline
  repo still raises its error. The detail screen retries a miss after 1.5, 3,
  6, 12 and 24 s before showing "not published", and stops when the screen is
  removed. The list itself now also reads the gateway's `pullRefs` (per-pull
  branch tips on the `.forkmesh/pulls` tree response, as the web dashboard
  does): a pull that is only on its branch is read at that commit. iOS does
  the same.
- **Pings open at once from a notification (2026-09-28)**: a tap awaited
  `markNotificationsRead` before navigating, and a ping whose `href` named no
  record went nowhere; the Inbox also started empty on every cold start
  because pings lived only in memory. Now every ping page fetched by
  `NotificationInbox`, `InboxModel` or the background link's
  `PingAlerts.fetchAndDeliver` is saved per project and account (the last
  100 rows, `inbox/pings.v1/<project>/<node>` in the shared prefs,
  `PingSnapshots`); both inboxes show it at construction, before any network
  call, and a background fetch is handed to a live `NotificationService`. The
  notification carries the whole row as JSON (`PushTarget.EXTRA_ROW`, next to
  the old extras). A tap puts the row at the top of the Inbox and the
  snapshot, then navigates: a record link opens that record, any other ping
  opens its Inbox detail over the Inbox list, and chat messages still open
  Chat. Marking read and refreshing run afterwards, through the Inbox sync
  queue when offline. An inserted row is kept for five minutes, or until a
  fetched page contains its id, so a refresh that lags the push does not drop
  it. Signing out clears that account's snapshot.
- **Disappearing messages (2026-09-28).** Chat messages disappear after a
  retention time kept in the user's account, so desktop, web, iOS and Android
  apply the same one. The default is 7 days; the user can shorten it for all
  chats (Settings > Chat) or for one chat (long-press a channel, or the timer
  in the chat header, which also covers DMs and `#mesh`). Presets are 7 days,
  3 days, 1 day, 8 hours and 1 hour; a per-chat picker also offers "Use
  default", which clears the override. `POST /api/accounts/chat-retention`
  reads (`{}`) and writes (`{"defaultMs": ms|null}`,
  `{"chat": key, "retentionMs": ms|null}`) the preference; the server clamps
  every value into 1 hour to 7 days, and the login payload carries the same
  `chatRetention` object.
  - Chat keys shared by every platform: a relay channel or DM is its
    conversation string (`#general`, `@<peerId>`); an Office channel is
    `channel:<roomId>` (its server channel id); anything else, such as
    `#mesh`, is the conversation string.
  - A message is expired when `ts <= now - (chats[key] ?: defaultMs)`.
    `C/model/ChatRetention.kt` holds the rule. `RelayRoom`,
    `OfficeChannelService`, `ChatExtras` (thread replies) and `LocalMeshRoom`
    refuse expired messages as they arrive or are replayed; restoring and
    saving the on-device history (`ChatHistoryCodec.bounded`, `ChatHub`) drop
    them too; and the app prunes what is in memory every 5 minutes and at
    once when the preference changes (`A/feature/chat/ChatFeature.kt`), so
    messages vanish while the app is open.
  - `AuthService` caches the preference per account in `PrefStore`
    (`chat_retention/<account>`), so an offline start still applies it; it is
    refreshed after sign-in and each time the app comes to the foreground,
    and the newer of the cache and the login payload (by `updatedAt`) wins.

## 7. Cryptography and wire compatibility

- JCA where Android 26 has it (AES-GCM, SHA-256/512, HMAC, PBKDF2); pure
  Kotlin otherwise: Ed25519 (RFC 8032, constant-time comb; strict verify adds
  identity and prime-order checks as `app/src/collaboration.py`), X25519
  (constant-time ladder; private keys stored clamped), SHA-3/SHAKE
  (Android 26 has no SHA-3), ML-KEM-768 (FIPS 203, interoperable with
  OpenSSL, Go and the desktop's MirrorCrypto).
- PBKDF2 uses the JCA provider only for non-empty 7-bit ASCII passwords;
  anything else takes a byte-exact HMAC loop so no provider's text encoding can
  change the key. HMAC with an empty key equals a single zero byte (RFC 2104),
  which JCA would otherwise reject.
- The retired Dart client's behaviour is reproduced where it reaches the wire
  or storage, because existing rooms, salts, stored keys and signatures depend
  on it: Dart's `String.trim` (Kotlin's `trim` uses a different whitespace
  set, which would change room names and therefore salts and storage keys); base64 acceptance rules; UTF-8
  with U+FFFD for lone surrogates; `jsonEncode` output (`C/json/Json.kt`,
  compact, non-ASCII kept) so signed payloads are byte-identical; whole doubles
  printed with `.0`; `1 == 1.0` for decoded versions.
- **Catalog signatures** (`C/localrepos/CatalogCanonical.kt`): the Worker
  hashes `json.dumps(record, sort_keys=True, separators=(",", ":"))` with
  CPython's `ensure_ascii`; a general writer would break every signature, so
  core reproduces CPython exactly, and `clean_string` truncation is by code
  point.
- **Trust lists** (`C/security/TrustLists.kt`): pinned to
  `conformance/trust-lists/v1/vectors.json`; nothing is trusted unless
  followed, trust never leaves its role and scope, equivocation or revocation
  cuts a list back to its last good version, disagreement stays a visible
  conflict.
- **Sync status** (`C/api/SyncStatus.kt`): built only from counts, timestamps
  and closed vocabularies so node names, addresses and raw errors never reach
  the summary; pinned to `conformance/sync-status/v1/model.json`.
- **Vault** (`fmv1`): Argon2id and BLAKE2b in pure Kotlin, checked against RFC
  9106/OpenSSL vectors and the JS, C++ and Dart interop files, and end to end
  against the vault Worker module. **Coves** open the desktop's password,
  account and legacy v1 files with the same fixtures iOS uses.
- **QR**: encoder bit-exact with the desktop on eight samples; the decoder
  handles rotation, perspective, inverted codes and damaged modules
  (Reed-Solomon).

## 8. Git engine and repositories

- A pure-Kotlin Git engine (`C/git`). Blocking; run off the main thread.
- **Transport is fail-closed** (`C/git/transport/IoHttpTransport.kt`): https
  only, no user-info, strict TLS unless the caller accepts an explicit
  challenge, zero redirects (a 3xx is `remoteRejected`, unless the caller
  asks for it back with `surfaceRedirects`; see the mirror redirect below),
  no cache, no decompression, bounded headers and bodies, one connection per request. A
  blocking read in `HttpURLConnection` cannot be interrupted, so a watchdog
  closes the request's sockets on cancel or timeout.
- **Protocol**: fetch never asks for thin packs (every delta base is in the
  pack); push requires `report-status(-v2)` so every ref gets a typed result,
  and never requests `delete-refs`. Fetch refspecs may only write
  `refs/remotes/<remote>/`, so they cannot rewrite local branches or tags.
- **Locks**: stale `.lock` files left by a killed process are removed on open
  (known engine lock paths only, and only when older than a threshold).
- **SHA-1 collision screening** (`C/git/crypto/Sha1Dc*.kt`): a pure-Kotlin
  port of sha1collisiondetection (MIT; license beside the files), generated
  from the C iOS vendored. Workspaces and the repository store use it; the
  engine itself defaults to no detector, and untrusted data then
  fails closed with `hash_screening_unavailable`.
- **Clone to device** uses the repository's public owner (never the serving
  node, e.g. `mirror35`) and reads `GET /api/repo/<owner>/<repo>/mirrors`:
  healthy and fastest mirrors first, the relay last, failover on any error
  except cancel or the size guard. Clones are shallow single-branch at depth
  1 (`GitWorkspace.meshCloneDepth`), and every workspace repository opens with
  `GitWorkspace.deviceLimits`: the engine hard maxima (1 GiB pack, 1 GiB
  inflated, 256 MiB object, 30 s connect, 60 s idle read, 20 min deadline).
  The old 96 MB guard refused the flagship once its depth-25 pack reached
  158 MB (2026-10-04, `native/ios/docs/FMCodeLocal.md`); shallow fetches skip tags,
  `shallow-info` lines may lack the trailing LF, and every history walk stops at
  `.git/shallow` commits. The relay falls back to a full clone when it cannot
  deepen; `origin` then points back at the relay.
- **One copy per repository, and Sync** (ported from iOS 2026-10-02). A
  repository on the phone is known by its `publishedAs` (trimmed,
  case-insensitive); a catalog row matches by public owner, then by serving
  node (`LocalRepoStore.recordFor`). The store refuses a second clone, and
  `cloneToDevice` refuses before any mirror lookup. Sync is fetch, then
  fast-forward only (`GitWorkspaceOps.pull`, `GitRepository.fastForward`,
  results in `C/localrepos/LocalSync.kt`): a branch that already holds
  ForkMesh's is "ahead", a diverged or shallow-unprovable one is left alone,
  and local changes refuse it.
  Sync, like Fetch, asks the relay only: iOS also fails over to other mirrors.
  The bottom bar holds Sync and Push (Fetch in the ⋯ menu, the sync state in
  the mesh banner) so nothing truncates.
- **Mirror redirect** (`C/localrepos/ForkMeshRedirect.kt`, ported from iOS
  2026-09-27). The relay answers a fetch's upload-pack advertisement with a
  `307` to a mirror (`X-ForkMesh-Transport: direct-mirror`); before this the
  fail-closed transport turned it into `remoteRejected` and the fetch stalled.
  `forkMeshRemoteAccess` now wraps the signing transport: it follows exactly
  one redirect, only from a `GET …/info/refs?service=git-upload-pack` to a
  credential-free https upload-pack advertisement on another base, and sends
  that request and every later one in the operation to the mirror **unsigned**
  (the device signature is for the relay). A second redirect, a redirect on
  push, or any other target is still `remoteRejected`. `origin` stays the
  relay, and the fetch result names the mirror (`servedBy`, "Fetched 1 branch
  from mirror35.test.").
- **Shallow push** (`C/localrepos/ShallowPushProof.kt`, ported from iOS
  2026-09-27). The engine refuses a push it cannot prove is a fast-forward,
  and in a shallow clone the remote tip is often unknown or below the shallow
  boundary, so every such push failed. When a push from a shallow clone fails
  with `nonFastForward`, the workspace fetches the branch, then asks
  `GitRepository.ancestry` (ancestor, not ancestor, or unknown across the
  shallow boundary) and deepens with `deepen 50/250/1000` (`deepen-relative`)
  and finally `unshallow` until the answer is known. Ancestor: the push is
  retried. Not an ancestor: `push_diverged`, without downloading more history.
  Still unknown, or a deepen over the 1 GiB pack limit:
  `shallow_history_limit`, which tells the user to push to a new branch or
  from a full clone.
- **Packed objects on demand** (`C/git/pack/PackObjectStore.kt`, ported from
  iOS 2026-10-02). Every workspace call opens a new `GitRepository`, and its
  pack store used to read and fully parse the pack (inflate, resolve and
  screen every object) on the first packed read, so opening a cloned
  repository paid for indexing the whole pack several times. The store now
  looks the object up in the `.idx`, memory-maps the pack (no
  `Inflater.setInput(ByteBuffer)` below API 30, so each entry's bytes up to the
  next indexed offset are copied out), resolves ofs and ref delta chains (up to
  4096 deep; thin packs, cycles and bad offsets refused) against a 32 MB FIFO
  delta-base cache per pack, then hashes the result (and screens it, when the
  repository has a detector) and refuses it unless it is the id asked for. The
  pack header and the idx-to-trailer checksum are checked once when a pack is
  opened; a miss re-lists the pack folder once, so a pack installed later is
  found. Fetch and clone still parse the whole received pack, which is what
  vets untrusted data.
- **Checkout writes only what changed.** Clone and branch switch skip paths
  whose blob and mode did not change, write worktree files without an fsync
  each (the index and refs are still flushed), delete removed symlinks, delete
  the old entry first when a path turns into or out of a symlink, and prune
  folders a removal empties. Progress counts up: "Indexing 512 of 9743
  objects" while a fetched pack is resolved, "Checking out 64 of 3000 files"
  every 64 files during a clone (a branch switch reports nothing).
- **Public owner.** Catalog rows served by mirrors are merged under their
  public owner (issue #717), and repository reads go to the public owner
  first with the serving node as fallback. History is `/history`; `/commits` is not a
  repository read on the server.
- **Workers**: `GitIsolateRunner` runs each operation on a short-lived thread
  and serializes operations per repository (the engine's lock files would
  race otherwise). A JVM thread cannot be killed, so cancel trips the operation's token and the caller returns only once the
  worker has stopped.
- **Local store** (`LocalRepoStore`): names `[A-Za-z0-9._-]{1,100}`, never
  through a symlink; delete removes only a direct child directory and never
  follows links; imports never adopt foreign hooks, config or objects and
  never copy symlinks or special files.
- **Push signing** (`C/localrepos/ForkMeshRemote.kt`):
  `forkmesh-git-push-v1\n{owner}\n{repo}\n{method}\n{ts}\n{sha256hex(body)}`
  with the device key; the key never leaves `DeviceIdentity`.
- **Publishing** (`PublishService`): the record is built exactly as the
  Worker's `safe_catalog_record` normalizes it (optional fields omitted rather
  than empty, or the hash changes). `stateHash`/`stateSig` are deliberately
  omitted: the relay pins them and refuses to serve a mirror whose live
  advertisement does not match, and a phone can only guess the served ref set.
  The catalog `nodeId` is a stable per-install id, not the device key, because
  a revoked device rotates its key and would lose source authority. A cloned
  repository publishes its upstream as `forkedFrom`.
- **Pull requests** are patches from `PatchBuilder` for `git apply --index
  --3way`; every file header carries an `index old..new mode` line, and the
  branch is pushed first so the receiver has the pre-image blobs for the
  three-way fallback.
- **Quick edit** refuses binary, non-UTF-8 or oversized files, and bytes whose
  blob id disagrees with what the server reported.
- **Zip import** reads the central directory (zip64 included), keeps the first
  member of a duplicated name, and bounds every read so a
  lying header cannot inflate past the limit.

- **Mirror to GitHub or GitLab** (2026-10-13): `ProviderMirrorScreen` (`A/feature/code/review`) over `C/ci/ProviderMirror.kt`, opened from the repository top bar's `sync_alt` button (owners) and Settings › More. It renders the server's catalog; behaviour and storage are in [../app/provider-mirrors.md](../app/provider-mirrors.md).

## 9. Signed writes

- Inbox writes (`C/api/InboxService.kt`) mirror the desktop's `submit*ToInbox`
  and the Worker's `verify_issue_event`, `verify_pull_event` and
  `verify_pull_comment_event`: every canonical string must match
  `app/src/entry.py` byte for byte or the relay answers 401 `bad_signature`.
- Desktop commands (`C/control/DesktopCommandContract.kt`): posted without a
  session token and never signing the user out on a 401; unsupported commands
  are refused before signing; the buttons appear only after the server's
  route probe answers (re-probed every 15 min).
- Destructive host and node operations use the desktop's prepare/run one-use
  token and a typed name, checked locally, after the screen lock
  (`UserPresence.confirm`). Approving an agent request always opens the app
  for that check; only Deny is answered from a notification.
- Tips and reward payouts are signed by the desktop; the phone only asks,
  after the screen lock.

## 10. Platform choices

- **Haptics**: selection is a tick, a light impact `VIRTUAL_KEY`, a medium
  one `KEYBOARD_TAP`, a heavy one `CONTEXT_CLICK`; all are suppressed under
  reduced motion; Agent Watch vibration defaults off when system animations are off. Android
  waveforms carry only strength, so iOS haptic sharpness is dropped.
- **Theme**: `FmColors.senderColor` hashes UTF-16 code units;
  the monospace face is the platform's; system-bar icons follow the app
  theme, not the system's.
- **Timers** ring through `AlarmManager` (`TimerAlarmReceiver`, rescheduled
  after boot and update). `SCHEDULE_EXACT_ALARM`, not `USE_EXACT_ALARM`, which
  Play restricts to dedicated alarm and calendar apps; without the exact-alarm
  grant Android may ring late, and the screen says so. Timers are
  device-wide, not per project, like iOS's store.
- **Glasses Alert** (`feature/glasses/`, core `glasses/GlassesWatch.kt`) scans in
  `GlassesWatchService`, a `connectedDevice` foreground service. It uses
  `ScanFilter.setManufacturerData` for each Meta/Luxottica company ID,
  because Android pauses unfiltered scans when the screen is off. The alarm
  (`GlassesAlarm`, one per process) plays the default alarm ringtone with
  `USAGE_ALARM` and vibrates with `VibrationAttributes.USAGE_ALARM`, so ringer
  silent and vibrate modes do not mute it. It raises `STREAM_ALARM` to max
  while ringing and restores it. With `ACCESS_NOTIFICATION_POLICY` granted,
  it switches `INTERRUPTION_FILTER_NONE` to `ALARMS` while ringing and back.
  Its notification channel is silent (the service drives sound and
  vibration) with `setBypassDnd`. Stop and swipe-away go to
  `GlassesSilenceReceiver`. Android 11 and older scan with
  `ACCESS_FINE_LOCATION` (`maxSdkVersion 30`).
  While the app is in the foreground the service runs a second, unfiltered
  `SCAN_MODE_BALANCED` scan (`censusCallback`) that feeds `GlassesCensus`
  (core `glasses/GlassesCensus.kt`) for the compact three-column device grid;
  its tiny glass tiles show kind, name, address, distance and colour-coded
  strength. The census smooths RSSI before sorting strongest first and keeps a
  quiet device for 45 seconds so the grid does not churn. It skips adverts
  carrying a glasses company ID, sends adverts with a
  glasses name to the normal match, and stops when the app leaves the
  foreground. `GlassesHoming` (core `glasses/GlassesHoming.kt`) follows the
  strongest sighting and turns its RSSI, plus the heading from
  `GlassesCompass` (`TYPE_ROTATION_VECTOR`, on only while the app is in front
  and something is being followed), into the closeness, trend and arrow the
  screen and the top-bar radar show. The heading is the horizontal direction of
  the phone's top plus its back, so it holds whether the phone lies flat or
  stands upright.
  Tapping a device tile opens `NearbyDeviceSheet`; `GlassesWatchService.track`
  points a second `GlassesHoming` (`trackHoming`) at that address, fed by the
  census scan and re-followed every census tick with `seenAt` = now, so a
  device that goes quiet shows "Signal lost" instead of being dropped. The
  sheet shows its closeness, distance guess and arrow (`state.tracked`), and
  the compass also runs while it is open. While watching, `GlassesRadarChip`
  sits in the home `TopBar` right after the project logo and title and opens
  Glasses Alert. It replaced the floating `GlassesPill` ("Watching, N other
  devices"), which covered page content. It draws one dot per device from
  core `GlassesRadarLayout`: the distance from the centre comes from signal
  strength. Each device keeps a fixed angle hashed from its address, because
  BLE gives no direction. Followed glasses with a compass bearing sit at
  their arrow angle. It has a count badge and turns red while glasses are
  near (same layout as iOS, see `native/ios/docs/FMTools.md`). `advert()`
  also passes the service UUIDs (listed and service data, through
  `NearbyKinds.shortUuid`) and the GAP appearance parsed from the raw record
  (`NearbyKinds.appearance`). The 25 device kinds and their order of evidence
  are the same as on iOS (FMTools.md). `NearbyKinds` also sorts lights (Govee, Hue, LIFX…)
  and appliances (Nespresso Vertuo, kettles, vacuums…) and more TV brands.
- **Notifications** need `POST_NOTIFICATIONS` on 13+; the app asks after
  sign-in and when a countdown or alarm starts.
- **Crash reports** come from `ApplicationExitInfo`, the counterpart of iOS
  MetricKit.
- **App Links** for `app.forkmesh.com` and `forkmesh.com` need
  `/.well-known/assetlinks.json` on the server with the v3 release signer
  (`E8:C0:28:A4:99:36:35:02:73:A1:BC:B4:E8:22:C0:D4:F8:FE:DB:98:C3:41:7B:A9:EB:9D:3C:61:7A:33:F4:9A`,
  `DEFAULT_SIGNER_SHA256` in `tools/forkmesh-release-host.py`).
- **Stall log** lives in the app's private files
  (`files/diagnostics/stalls.log`).
- **Diagnostics sampling** runs only while an activity is visible; a
  suspended process would otherwise read as one giant stall.
- **Feedback is inline, never a toast** (2026-10-09). The user asked for no
  toasts anywhere, with the information shown inline where it belongs, so the
  app has no `Toast.makeText`, no snackbar and no floating notice.
  `FmMessenger` keeps only dialogs and sheets; `showSnackBar`,
  `FmSnackbarHost` and `FM_SNACKBAR_MILLIS` are gone. The result of an action
  goes into an `FmStatus` (`A/ui/kit/FmInlineStatus.kt`) held by the screen,
  sheet or row that ran it, and `FmInlineStatus` draws it there in normal
  layout flow: the message in the muted colour (`show`) or the danger colour
  (`error`), an optional action button (Undo, Open, View) and a dismiss
  button. Errors stay until dismissed or replaced. A sheet that closes after
  its action reports in the screen section that opened it. Where no screen
  ran the action, the line goes to the place that lists the thing:
  - links opened from anywhere (`NavigationFeature.openUri`, `openLink`,
    `openInBrowser`) put a failure ("No browser can open …", "Could not
    switch to …", "… is not one of your projects.") in
    `NavigationFeature.linkProblem`, which the shell shows as a line under
    the connection banner; the You hub and the all-pages menu
    (`rememberHubItemRunner`) instead keep their sheet open and show the
    failure at its top, and the Update required page shows it under its
    button;
  - the bottom bar's Code button with no repository open shows
    `CodeButton.NO_REPO_NOTICE` in a line just above the bar (in the shell on
    wide screens), cleared on the next tab change;
  - a notification action (Reply, Deny, Merge) that fails in the background
    leaves the notification in place and puts the reason in
    `InboxFeature.actionProblem`, shown at the top of the Inbox;
  - the approval page (`ApprovalRouteScreen`) stays open after Approve or
    Deny and shows the answer under the buttons;
  - the vault autofill activity
    (`A/feature/tools/vault/VaultAutofillActivity.kt`) is a dialog with no
    app screen behind it, so when the password cannot be read it shows "Open
    ForkMesh and unlock your vault to refresh autofill." in red inside its own
    dialog, above the search field when the picker is open or with an OK
    button that closes it otherwise, instead of a toast and an immediate
    close.
  Where the lines sit, by area:
  - Agents: the run screen's results under its action buttons (Create PR
    keeps an Open button), "Sent." above its composer; results of actions
    that close the run screen (delete, merge, continue on desktop) go to
    `agentsNotice` (`AllAgentsScreen.kt`), shown at the top of the Agents
    tab, All agents and Desktop agents; Stop all, swipe delete, Retry and
    move-to-top under the Agents tab's counts.
  - Composer: attachment and file errors inside the composer; a page
    target's reply (Calorie Counter's "Logged to breakfast…") in the shell's
    line above the bottom bar.
  - Code: review, comment, merge, queue, CI, release and settings results in
    the card or list the button belongs to; `DesktopCommands` and
    `CollabEnv` carry their own `FmStatus`; Quick edit stays open after
    Propose and says so in its bottom bar; Clone and Sync report under the
    repository hub's action row.
  - Chat and Discord: notices, send failures, `/issue`, retention errors and
    copies in a line above the composer (threads too).
  - Account: each Settings card reports under its own buttons; Ping
    settings under the header; orgs and teams under the header; Installed
    apps as the first list row; a failed project switch from the drawer
    reopens the drawer to show the error.
  - Inbox and pings: delete all, pause and failed notification actions at
    the top of the list; quick fixes and copies under the ping's Actions
    card.
  - Tools: Calorie Counter under its tab bar (above the install button
    before it is installed); a note's save, publish, delete and convert
    results under the note's meta line, restore errors in the History sheet,
    share errors in the Share sheet; Tasks, Calendar, CRM, Marketing,
    Ledger and Telemetry in the section of the button; the bug report sheet
    stays open with its thanks and a Done button.
  - Local repositories: a repository's commit, publish, push, fetch and
    sync results and git errors in a line just above its bottom bar;
    sync-all, import and create between the repository list and its
    buttons; the file editor under its app bar.
  - Hosts, coves, desktop remote: in the screen or card that ran the
    action; a wrong cove password shows in the unlock dialog, which stays
    open.
  Copy confirmations ("Copied to clipboard") show under the message or row
  that was copied (`copyMessageText(context, text, status)`). The kit gallery's
  section 9 demonstrates the line with status, error and action buttons.
- **Pasting images into chat**: `ChatComposerBar` passes
  `PromptComposer(onPasteImages:)` wherever it can send a file, so the
  composer's image-paste toolbar, `contentReceiver` and Ctrl-V work in chat
  without photo chips. Each pasted image goes through `PhotoIntake.prepare`
  (2048 px JPEG, no metadata) and `ChatHub.sendAttachment` at once; the
  pasted text stays in the draft.

## 11. Differences kept on purpose

- No APNs/FCM: the background connection above replaces push.
- No first-run choice screen and no one-tap account switch: iOS has neither.
- No admin delete of other people's chat messages: iOS has none.
- Worktrees are a list only, milestones read-only, database admin read-only,
  as on iOS.
- `minSdk` 26: phones on Android 7.x running the Flutter APK cannot take the
  update.
- Android still shows the Solana payout address, the Patreon membership row,
  the payout-readiness section and the repository donation card. iOS removed
  them on 2026-09-26 (`bdbd52fef`); removing them on Android is an open
  product decision.
- Chat message times (the message header, Discord rows and "Last reply")
  use a 12-hour clock with a meridiem, "9:05 AM", on Android
  (`messageTime`) and iOS (`ChatHub.timeLabel`), matching the 12-hour stamps
  the activity log already shows (issue #597).
- Pings (issue #592): Inbox rows and the Notifications list show a 12-hour
  time (`notificationTimeLabel`, "2:40 AM"), and tapping an Inbox row opens
  a ping page instead of jumping to the link: `A/feature/inbox/InboxItemScreen.kt`
  and iOS `Features/Inbox/InboxItemScreen.swift`. The page marks the item
  read and shows the whole item with its date and time (`pingDetailWhen`,
  "Mon, Feb 2, 2026 · 2:40 AM"), repository, sender and link, with Open (its
  route, else the web page), Send to agent (the compose route with
  `InboxModel.prompt`), Copy ping, Copy link, Ping settings (pings only) and
  Delete. Delete asks first: a ping is deleted on the server, tasks and
  phone-only items are only hidden. On Android the long-press menu's Open
  still goes straight to the link. The agent prompt keeps its 24-hour stamp
  (`pingWhen`, `pingAgentPrompt`) so it reads the same as the desktop's.
- iOS Send to agent on the ping page, the Inbox long-press menu and the
  task page's Start agent opens the shell composer (`NotificationActions.onCompose`,
  the same one Chat's Send to agent uses) with the prompt filled in and the
  item's repository chosen when it has one (2026-09-29). They used to open a
  sheet on the `.compose` route, which no screen registers, so the button
  showed a "Coming soon" placeholder. The sheet stays as the fallback when
  the shell is not up.
- The prompt composer has no mic button on Android (`A/feature/composer/PromptComposer.kt`)
  or iOS (`Kit/PromptComposer.swift`) since 2026-09-27 (issue #603); the
  phone keyboard keeps its own. Dictation stays as the `/dictate` command:
  the field reads "Listening…" while it runs, and `/dictate` again or
  sending stops it. When the recognizer ends on its own after a quiet
  moment with words heard (`SpeechDictation` `onQuiet`, since 2026-10-02),
  the composer sends the prompt, as on iOS; `/dictate` again stops without
  sending.

## 12. Gap sweep (2026-09-27)

On 2026-09-27 the Android column in PROGRESS.md (74 of 75 rows Done at
`f3f820372`) was re-checked against the code rather than the tracker, and
every iOS commit since 2026-09-18 was traced into Android. The gaps found and
what was done about them:

| Feature | Flutter | iOS | Android before | Android now |
| --- | --- | --- | --- | --- |
| Calories: log by prompt (typed meals, weigh-ins, meal photos, receipts) through the shell prompt | `services/calories_prompt.dart`, `services/shell_prompt_targets.dart` | `FMTools/CaloriesQuick.swift`, `ShellPromptTargets.swift` | Missing | Done: `C/calories/CaloriesPrompt.kt`, `C/composer/ShellPromptTargets.kt`, `A/feature/composer/ShellPrompt.kt`, `A/feature/tools/CaloriesScreens.kt` |
| Calories: Quick add sheet (My foods, "You log often", common foods, read receipt) | `calories_screen.dart` `_QuickAddSheet` | `CaloriesSheets.swift` | Missing | Done: `QuickAddSheet` in `CaloriesScreens.kt` |
| Calories: search as you type, brand-aware | `calories_screen.dart` `_SearchSheet` | `CaloriesSearchModel` | Search on submit only | Done: `C/calories/CaloriesPrompt.kt` `FoodSearch` |
| "Where you're signed in": list and log out sessions (`GET`/`DELETE /api/accounts/sessions`) | `settings_screen.dart` `_AccountSessionsCard` | not ported | Missing | Done: `C/settings/AccountSignIns.kt`, `A/feature/account/settings/AccountSessionsCard.kt` |
| Pull merge, merge preview and "may merge" addressed by the public owner; cache dropped after a merge | `api_service.dart` `canMergePulls`, `mergePull` (`609143b76`) | Done: `ApiService+Agents.swift`, `PullMergeModel`; `RepoPullsModel.start` re-reads on every appearance so a merge shows on the way back | Serving node's owner | Done: `C/collab/ReviewSession.kt`, `A/feature/code/review/CodeKit.kt` `canMergePulls` (organization ownership only: a node owner's own route is refused by the relay, so `isOwner` would offer a button that 403s), `RepoScreens.kt` |
| Org agent queue by the public owner | `api_service.dart` (`61a1f3169`) | not checked | `readOwner` never passed | Done: `C/agents/AgentQueueApi.kt` `from` |
| Local repositories: rebase records onto the current root, adopt unlisted `<root>/<name>/.git` folders | `services/git/local_repo_store.dart` (`d7d393f25`, `4cbf9478f`) | `LocalRepoStore.swift` | Dropped them | Done: `C/localrepos/LocalRepoStore.kt` |
| Files received from the desktop kept where Flutter wrote them | `local_control_service.dart` (`app_flutter/ForkMesh from desktop`) | n/a | `files/ForkMesh from desktop` | Done: `FlutterFiles.documentsDir` in `A/feature/agents/AgentsFeature.kt` |
| Profile picture refreshed from `GET /api/accounts/<name>` | `auth_service.dart` `refreshAvatar` | not ported | Missing | Done: `C/api/AuthController.kt`, `A/feature/account/AuthService.kt`, called from `A/shell/HomeShell.kt` |
| Settings sections hidden when the relay lacks the feature; "Serve as a node" disabled without `nodeSync` | `settings_screen.dart` | not checked | Always shown | Done: `SettingsScreen.kt`, `serveAsNodeNote` in `C/account/SettingsLogic.kt` |
| Login screen names the project when there are several | `auth_flow.dart` `_relaySubtitle` | not ported | Generic line | Done: `loginSubtitle` in `C/account/projects/Project.kt` |
| Vault recovery code: Done waits for "I have saved this code." | `vault_screen.dart` | not checked | Done enabled at once | Done: `VaultScreens.kt` |
| Agent sign-in terminal: copy text, Start again, signed-out state | `agent_login_terminal_screen.dart` | not checked | Missing | Done: `AgentLoginTerminalScreen.kt`, `canRestart`/`plainText` in `C/agents/AgentTerminal.kt` |
| Relative ping links (`/dashboard/...`) open on the project's web host | `notifications_screen.dart` `webLink` | not checked | Did nothing | Done: `InboxRoutes.webLink` in `C/inbox/Inbox.kt` |
| Branch history (a branch row opens that branch's commits; warns when the server ignores `ref`) | n/a | `f4653f44a` | Opened the tip commit | Done: `C/code/RepoHistory.kt`, `BranchHistoryScreen` in `CommitsTab.kt` |
| `newIssue` route | n/a | registered | "Coming soon" | Done: `NewIssueScreen.forRoute` |
| Default branch for new on-device repositories | n/a | `b665f3110` | Setting stored, never used | Done: `GitRepositoryInit.initialBranch`, Settings > Repositories on this phone |
| Feature | iOS | Android before | Android now |
| --- | --- | --- | --- |
| Calories: log by prompt (typed meals, weigh-ins, meal photos, receipts) through the shell prompt | `FMTools/CaloriesQuick.swift`, `ShellPromptTargets.swift` | Missing | Done: `C/calories/CaloriesPrompt.kt`, `C/composer/ShellPromptTargets.kt`, `A/feature/composer/ShellPrompt.kt`, `A/feature/tools/CaloriesScreens.kt` |
| Calories: Quick add sheet (My foods, "You log often", common foods, read receipt) | `CaloriesSheets.swift` | Missing | Done: `QuickAddSheet` in `CaloriesScreens.kt` |
| Calories: search as you type, brand-aware | `CaloriesSearchModel` | Search on submit only | Done: `C/calories/CaloriesPrompt.kt` `FoodSearch` |
| "Where you're signed in": list and log out sessions (`GET`/`DELETE /api/accounts/sessions`) | not built | Missing | Done: `C/settings/AccountSignIns.kt`, `A/feature/account/settings/AccountSessionsCard.kt` |
| Pull merge, merge preview and "may merge" addressed by the public owner; cache dropped after a merge | `ApiService+Agents.swift` (public owner not checked) | Serving node's owner | Done: `C/collab/ReviewSession.kt`, `A/feature/code/review/CodeKit.kt`, `RepoScreens.kt` |
| Org agent queue by the public owner | not checked | `readOwner` never passed | Done: `C/agents/AgentQueueApi.kt` `from` |
| Local repositories: rebase records onto the current root, adopt unlisted `<root>/<name>/.git` folders | `LocalRepoStore.swift` | Dropped them | Done: `C/localrepos/LocalRepoStore.kt` |
| Files received from the desktop kept where the Flutter app wrote them (`app_flutter/ForkMesh from desktop`) | n/a | `files/ForkMesh from desktop` | Done: `FlutterFiles.documentsDir` in `A/feature/agents/AgentsFeature.kt` |
| Profile picture refreshed from `GET /api/accounts/<name>` | not built | Missing | Done: `C/api/AuthController.kt`, `A/feature/account/AuthService.kt`, called from `A/shell/HomeShell.kt` |
| Settings sections hidden when the relay lacks the feature; "Serve as a node" disabled without `nodeSync` | not checked | Always shown | Done: `SettingsScreen.kt`, `serveAsNodeNote` in `C/account/SettingsLogic.kt` |
| Login screen names the project when there are several | not built | Generic line | Done: `loginSubtitle` in `C/account/projects/Project.kt` |
| Vault recovery code: Done waits for "I have saved this code." | not checked | Done enabled at once | Done: `VaultScreens.kt` |
| Agent sign-in terminal: copy text, Start again, signed-out state | not checked | Missing | Done: `AgentLoginTerminalScreen.kt`, `canRestart`/`plainText` in `C/agents/AgentTerminal.kt` |
| Relative ping links (`/dashboard/...`) open on the project's web host | not checked | Did nothing | Done: `InboxRoutes.webLink` in `C/inbox/Inbox.kt` |
| Branch history (a branch row opens that branch's commits; warns when the server ignores `ref`) | `f4653f44a` | Opened the tip commit | Done: `C/code/RepoHistory.kt`, `BranchHistoryScreen` in `CommitsTab.kt` |
| `newIssue` route | registered | "Coming soon" | Done: `NewIssueScreen.forRoute` |
| Default branch for new on-device repositories | `b665f3110` | Setting stored, never used | Done: `GitRepositoryInit.initialBranch`, Settings > Repositories on this phone |

Why the pieces are shaped as they are:

- **Page prompt targets.** The shell prompt is an overlay over the tab stack,
  and `TabStack` composes only the top screen of the selected tab, so a
  screen that registers in a `DisposableEffect` is by construction the page
  the user is looking at: a target counts only while its page is on top,
  without a navigator observer. The registry is pure Kotlin in `:core`
  (listeners, no coroutines) and `ComposerFeature` turns its changes into a
  `StateFlow` tick. The mode button cycles page target, agent, chat and back,
  as iOS does, so the agent and chat stay one tap away while Calories is open.
  Photos go to the page target too (iOS sends text only).
- **Calories photos** are re-encoded from the composer's already stripped,
  upright JPEG (1024 px for meals, 1600 px for receipts) and must stay under
  the server's 1.6 MB data URL limit. The Quick add receipt picker goes
  through `PhotoIntake` so EXIF orientation is applied first.
- **After a prompt log** the screen re-reads the overview itself before
  answering, so "Today: N of G kcal" counts what was just logged.
- **Sessions**: revoking the session this app uses signs this device out
  locally too (`currentRevoked`).
- **Local repositories**: the index records where a worktree *was*; a moved
  app container (or the Flutter-to-native upgrade) would otherwise make every
  record look vanished, and the rewrite would persist an empty index over
  repositories still on disk. Folders are the authority; adopted rows carry
  only name, path and folder time until opened.
- **Desktop files**: the app reads the Flutter app's documents directory
  (`app_flutter`) for everything else kept there, so received files follow
  the same rule instead of being orphaned by the upgrade.
- **Default branch** only applies to repositories created on the phone;
  clones and imports keep the branch they come with.

Still open after this sweep:

- Device runs: none of the above has been exercised on a phone or emulator.
  Verified: `./gradlew :core:coreTest` (1561 passed, 0 failed) and
  `./gradlew :app:assembleDebug` (debug APK builds).
- A quick project switcher sheet; Android reaches projects through the hub
  and Settings.
- Editing a project's World host from the UI (`ProjectsService.update(worldUrl=)`
  exists) and the "Not available on <relay>" summary in Settings.
- Skipping device-key rotation while other projects are signed in
  (`auth_service.dart` 688-700); each project has its own key on Android, so
  the risk Flutter guards against may not apply.
- Per-ping delete and a ping detail sheet in the Inbox (swipe and "Send to
  agent" exist).
- iOS work beyond Flutter: failover to the next mirror when the redirected one fails, report-a-bug in the bottom bar, reselecting a tab scrolls to the top,
  desktop agent pictures through the relay, cove notices over the relay, and
  on-device Gemma (iOS only, MLX).

## 13. Core features as plugins (2026-10-10)

The Home grid, bar tabs and repository tiles now follow Market packages, off
by default in new projects; the rule and the package list are in
`docs/marketplace/core-plugins.md`. On Android (`C/` is `core/src/main/kotlin/com/forkmesh/nativeapp`,
`A/` is `app/src/main/kotlin/com/forkmesh/nativeapp`):

- **One gate.** `C/market/PackageGate.kt` maps hub ids, `Route.Kind`s, bar
  tabs (Agents, Inbox, Chat) and repository tiles (`RepoHubSection`) to
  package ids. `PackageSurfaces.of(installed)` turns an "is it installed"
  answer into the hidden hub ids, route kinds, tabs and repo tiles. The Code
  bar button follows `code`; Repos, Search, Market, Settings, Projects, Set
  status and Ads are not gated. Workflows follows `workflows` since
  2026-10-13, and every gated package also needs `mobile` among its places
  ([NAVIGATION.md 2.0.90](NAVIGATION.md)).
- **Effective install state.** `AccountPackagesStore.isInstalled` uses the
  account row when there is one, otherwise `PackageGate.fallback`: installed
  when the package is `default`, or when it has `installedBefore` and the
  project is legacy; a package the cached catalog does not know follows the
  legacy flag. A package also needs its dependencies (special-agents needs
  agents, pull-requests needs branches). `installedBefore` is read from market
  rows (top level, or `install.installedBefore`) into `MarketPackage` and
  `AccountCatalogEntry` and kept in the cached catalog JSON. The store now
  keeps its cache in the project's prefs (`ProjectScope.prefs`), so projects
  no longer share one cached answer; for the original project this is the
  same store as before.
- **Legacy marker.** `PackageGate.legacyProject` writes
  `packages/legacyProject` once per project, before any other feature
  installs (`PackageGateFeature.install`, right after `AccountFeature`). The
  project is legacy when its prefs already hold any of
  `packages/account/state`, `packages/account/catalog`,
  `packages/status-page/monitors`, `nav/tour.v1`, `nav/archive.v1`,
  `packages/design-system/on`, `packages/tour/on`, `profile/name`,
  `server/room`, `node/serve`, or when the project already has a saved
  session or chose guest mode (`AuthService.hasAppAccess`). The answer never
  changes afterwards.
- **App-wide state.** `A/feature/market/PackageGateFeature.kt` publishes
  `surfaces: StateFlow<PackageSurfaces>`, rebuilt whenever the account store
  changes (`StatusPageFeature.publish`), the sign-in switch moves or the
  organization installs change. It replaces the Status-only boolean that
  2.0.72 introduced.
- **Surfaces gated.** Hub sheet and its recents, shell menu, bottom bar and
  side rail (Agents, Inbox, Code button, Chat; Repos always stays), shell menu
  "Go to" entries, Ctrl+1..4 and Ctrl+N, the prompt (new agent) button in the
  top bar, rail and menu, the running agent accessory, global search
  (destinations, issues, pulls, commits and chat sources), the command palette
  (suggestions, tabs, destinations, recents), launcher shortcuts
  (`ShellQuickAction.visible`, republished when the gate changes) and the
  repository tiles plus its Agent and Issue buttons; counts are not fetched
  for hidden tiles. When the selected tab's package is removed the shell moves
  to Agents, or to Repos when Agents is gone; back goes to the same home tab.
  Routes stay registered: pushes, widgets, deep links and the palette's
  direct `owner/repo#12` entries still open hidden features.
- **Organization packages.** CRM, Marketing, Analytics and Competitors show on
  Home only when an organization the user belongs to has them installed
  (`/api/plugins/<org>` for each of `myOrgs`, refreshed at most every ten
  minutes in the foreground and on sign-in, cached in
  `packages/organization/installed`; the Market updates it after an install
  or removal). This matches iOS `HubPackageState`.
- **Tour and Design** lost their per-phone switches (`packages/tour/on`,
  `packages/design-system/on`, and the `AccountSettings` getters): both follow
  the account. The first-run tour only opens when Tour is installed; it is
  not marked seen otherwise, so installing Tour later shows it once.
- **Sign in** is per phone: `PhonePackages` stores `packages/sign-in/on` in the
  project prefs, defaulting to the legacy marker. Off hides the hub's Sign in
  and Sign out items and the guest banner. The top bar avatar always shows
  (2026-10-13; it was an "apps" button with Sign in off), and the top bar
  Glasses radar follows glasses-alert. Settings keeps its Sign out card
  and the guest card's Log in and Create an account buttons, so nobody is
  locked out.
- **Market.** Every package keeps the account switch with the removal
  confirmation naming what goes along; nested packages (Status Page checks,
  pull-requests under branches, special-agents under agents) now ask before
  removal too. Sign in shows an "Installed on this phone" switch. Native
  destinations show "On this phone" and an Open button
  (`MarketPackage.NATIVE_DESTINATIONS`, `routeFor` in `MarketScreens.kt`).
- **Not gated on Android.** Home screen widgets (Agents, Inbox) and the New
  agent quick settings tile: the user places them, and they open the route,
  which still works. Android has no Steps, Water, Sleep, Sugar Break, Stretch
  or Speech screens, so those packages only appear in the Market.

## 14. Market list redesign (2026-10-13)

`A/feature/market/MarketScreens.kt` (`MarketScreen`), `MarketCards.kt`;
model in `C/market/MarketModels.kt` (`MarketModel`, `MarketOptions`,
`MarketSort`, `MarketText`).

- **Nothing behind the bars.** The status tabs, search field and list were
  three fixed bands; in landscape (rail layout, nothing consumes the
  navigation bar) the list ran under the system navigation bar and the last
  rows could not be reached, and the floating bug button sat on the last row's
  button. Search and the chip row are now the first items of the list, the
  list pads the horizontal safe area, and its tail is the unconsumed
  navigation bar plus IME inset and 72 dp, so the last row scrolls clear of
  the bottom bar, the gesture bar, the keyboard and the bug button.
- **Sorts and filters** match the web Market (`www/templates/pages/market.html`):
  Featured (category shelves), Most installed, Most downloaded, Most used
  (people, then opens), Top rated, Name. Filters are what
  `/api/market/packages` carries: installed or not, category, price (`free`),
  platform (phone surface, `desktop`, `web`, `world`), who installs it
  (`PackageInstallMode`: account, organizations, this phone, desktop app) and
  type. Newest, recently updated and publisher/official are not offered:
  the package rows have no dates and no `creator`.
- **Usage.** `/api/market/installs` gives installs and downloads;
  `/api/market/reviews/summary` already returns `usage` (people, opens) next
  to the ratings, now parsed (`PackagesClient.marketSummary`). Each row shows
  the parts that are non-zero.
- **Install state.** Every catalog row is `bundled: true`, which only means it
  ships with the desktop, yet the model read it as installed for every package
  that is neither account-synced nor organization-scoped (Winamp, VSCodium,
  the model packages, the World boards), so the Market said Installed for
  packages nobody installed. Those are now `PackageInstallMode.DESKTOP`:
  installed means `install.default`, the row says Desktop or World, and the
  phone offers no install. Organization packages kept a stale or empty
  answer when `myOrgs` or one `/api/plugins/<org>` failed: a failed org fetch
  now keeps its last state, an unknown org list (`organizationNamesOrNull`)
  falls back to the Home gate's cached answer
  (`PackageGateFeature.organizationCached`). Account packages the store does
  not know yet follow `PackageGate.fallback` (installedBefore plus the legacy
  marker), like the Home gate. Test: `MarketTest.testInstallStateFollowsWhatThePhoneCanKnow`.
- **One-tap install** (`MarketModel.cardAction`): account packages call
  `AccountPackagesStore.install` (dependencies follow), organization packages
  install for the only manageable organization or offer a menu when there are
  several, and open the package page when there is none; phone switches turn
  on. Installed packages with a native route show Open. Errors stay on the row.
- Items read `version` so a sort, filter or install recomposes the chip row
  and rows; LazyColumn item lambdas that only capture the model are skipped.

## 15. Secure chat: receipts, DM replies and deletes, message previews (2026-10-13)

Shared design for all clients: branch 1974 spec, `docs/app/chat-private-rooms.md`
and `docs/app/chat-push-previews.md`.

- **Receipt marks** (`ChatReceipt` in `C/model/Chat.kt`, `ChatRow.receipt`,
  drawn by `A/feature/chat/ChatReceiptMark.kt` on a Compose `Canvas` in the
  shared 22 x 14 geometry, 17 x 11 dp, accessible name "Sending", "Sent",
  "Delivered", "Read"). Every own message and own thread reply shows one,
  next to the time on a header row and at the trailing edge of a grouped
  row; ephemeral app rows and #mesh do not (#mesh keeps its "Queued" line).
  `ChatHub.receipt` asks the service that owns the room and falls back to
  Sent:
  - DMs (`DirectMessageService`): a send is Sending until the relay's
    `forkmesh-recipients` for its ref, then Sent; `forkmesh-delivered` naming
    the ref makes it Delivered; `forkmesh-read` turns every own message
    already Sent or Delivered in that DM into Read. An own frame that arrives
    from another of my devices or in the history replay is Sent. The service
    keeps bounded maps (256 each) of message id -> state, ref -> id and
    id -> latest ref; the ref is recorded before the frame is written, so an
    ack that beats `sendText` back is not lost, and a re-seal updates it.
  - Office rooms: Sending from `sendFrame` until the recipients ack
    (`OfficeChannelService.receipt`), then Sent; history rows are Sent.
    Channels never reach Delivered or Read (the relay only reports those for
    direct rooms) and keep the "to N" label after the mark.
  - Relay rooms (`#channel`, legacy `@` DMs): the shared room relay sends no
    ack, and `sendMessage` returns only after the socket accepted the frame,
    so own messages show Sent.
- **DM replies, reactions, edits, delete for everyone.** The frames are the
  relay-room ones (`thread-reply`, `reaction`, `edit`, `delete`, with
  `conversation`/`channel` = `direct:<id>`), sealed like `chat` through
  `sendSealed`; replies notify, the others persist without `notify`.
  `ChatExtras` stores DM thread replies (it took only `#` channels),
  reactions and edits as for channels, and its author lookup now goes through
  `ChatHub.messages`, so DM and Office messages are found. `ChatHub` routes
  `toggleReaction` (the emoji character goes on the wire, as on the web),
  `edit`, `deleteMessage` and `replyInThread` for `direct:` to the service.
  This fixes "Delete for everyone" in a DM, which used to call the relay
  room's delete and send it into the wrong room.
  - Receiving: an `edit` or `delete` is honoured only when its `senderId` is
    the target's author (DM history or a DM thread reply). One whose target is
    not here yet is held (128 targets, 8 frames each) and applied when the
    target arrives, so a replayed delete that overtakes its message still
    removes it and no notification is raised for it.
  - A delete removes the message from the transcript, its reactions, edits
    and attachment, and its own outbox entry (and edits or reactions on it),
    so a re-seal cannot send it again. Android keeps no DM transcript on disk
    (DMs come back from the relay's replay), so the id also goes into the
    persisted hidden-message list (`extras.hideMessage`, saved with the chat
    history) and a later replay cannot show it.
  - After sending the delete, the sender sends `forkmesh-superseded` with the
    original's ref when it knows it (own sends, own replayed frames), so the
    relay drops the stored ciphertext the other person has not received. The
    delete frame stays on the relay until their device receives it.
  - UI: long-press offers reactions, Reply in thread (`ChatHub.canReplyInThread`),
    Edit and Delete (for everyone or for me) on DM rows; the thread screen
    works for `direct:` (`ChatHub.rootMessage` reads `messages`, title
    "Thread with <name>", the composer stays enabled because a DM send opens
    its socket on demand).
  - Not done: the same frames in Office rooms. `OfficeChannelService` still
    accepts only `chat`, and the long-press sheet keeps reactions and edits
    off there.
- **Message text in DM notifications, the Signal way** (`C/push/DirectMessagePreview.kt`,
  `A/feature/push/PingAlerts.kt`, `PingNotifier.postDirectMessagePreview`).
  Android has no push transport; the background link's account event socket
  (`AccountEventLoop.onEvent`, which now passes the whole frame) gets a
  `direct-messages` frame naming `conversation` and `ref`. The service then
  `GET`s `/api/chat/direct-messages/<id>/pending?ref=<ref>` (side-effect free:
  it does not mark read, acknowledge or delete), decrypts the stored envelope
  with the room passphrase (`RoomCrypto`, PBKDF2 cached per process) and
  unseals it with this device's X25519 key (`RoomSeal.existing`: it reads the
  project's key and never creates one, and it does not persist peers, so it
  cannot race the chat screen's own seal). Only sealed `chat` or
  `thread-reply` frames for that conversation are shown. The server never
  sees the text.
  - One notification per conversation (tag project, id from
    `direct-messages/<id>`), with `Notification.MessagingStyle` listing each
    message (up to 8, deduplicated by message id) and the sender as a
    `Person` with their avatar. The platform style is used (API 28+, minSdk
    26 gets `BigTextStyle`), so no new dependency. A tap opens the
    conversation as before. The lock screen gets a public version that says
    only "New message".
  - The "Notification content" setting (Pings settings, "This phone" card;
    `NotificationContent`, stored in the app's `Prefs` under
    `notifications/content` for the whole phone): Name and message (default),
    Name only (title is the sender, body "New message"), No name or message
    (nothing is fetched; the fallback notification is posted without the
    sender's name).
  - Falls back to the old notification when the frame has no `ref`, the row
    is gone (already received on another device), the frame is not sealed
    for this device or anything fails. Quiet hours and pause hold both.
  - Channel messages: the account event socket carries no channel event
    (channel pushes are APNs only, `push_chat_room_apns`), so Android shows
    no channel notifications; this needs a socket topic first.
- Tests: `DirectMessagesTest` (receipt states, replies and reactions, own
  edits, delete for everyone with superseded, author checks and held
  mutations), `DirectMessagePreviewTest` (event parse, on-device decrypt,
  refusals, content setting, line merge), `OfficeAndDiscordTest` (Office
  Sending until the ack).

## Appendix: port map

The Flutter (or Flutter Android) source each Kotlin file was ported from, as
the per-file headers recorded it before the comments were removed.

| Kotlin file | Ported from |
| --- | --- |
| `A/feature/account/AccountEventChannel.kt` | `mobile/lib/services/account_event_channel.dart` |
| `A/feature/account/AccountFeature.kt` | `mobile/lib/main.dart`, `mobile/lib/services/{settings, auth,api,inbox,notification,identity,owner_crypto,secure_store}_service` |
| `A/feature/account/AccountUi.kt` | `mobile/lib/services/legal_links.dart` |
| `A/feature/account/AuthService.kt` | `mobile/lib/services/auth_service.dart` |
| `A/feature/account/NotificationService.kt` | `mobile/lib/services/notification_service.dart` |
| `A/feature/account/PortableIdentityActions.kt` | `mobile/lib/services/portable_identity.dart` |
| `A/feature/account/PrefsStore.kt` | `mobile/lib/services/{settings,auth}_service.dart` |
| `A/feature/account/PushNotificationService.kt` | `mobile/lib/services/push_notification_service.dart` |
| `A/feature/account/SecurityBootstrap.kt` | `mobile/lib/main.dart` |
| `A/feature/account/SettingsService.kt` | `mobile/lib/services/settings_service.dart` |
| `A/feature/account/auth/AuthFlow.kt` | `mobile/lib/screens/auth_flow.dart` |
| `A/feature/account/notifications/NotificationsScreen.kt` | `mobile/lib/screens/notifications_screen.dart` |
| `A/feature/account/notifications/PingSettingsScreen.kt` | `mobile/lib/screens/ping_settings_screen.dart` |
| `A/feature/account/orgs/OrgDetailScreen.kt` | `mobile/lib/screens/org_detail_screen.dart` |
| `A/feature/account/orgs/OrgWidgets.kt` | `mobile/lib/screens/orgs_screen.dart` |
| `A/feature/account/orgs/OrgsScreen.kt` | `mobile/lib/screens/orgs_screen.dart` |
| `A/feature/account/settings/SettingsScreen.kt` | `mobile/lib/screens/settings_screen.dart` |
| `A/feature/account/settings/SponsoredAdCard.kt` | `mobile/lib/widgets/sponsored_ad_card.dart` |
| `A/feature/chat/ChatFeature.kt` | `mobile/lib/screens/chat_screen`, `mobile/lib/screens/activity_screen.dart` |
| `A/feature/code/AboutTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/ActionsTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/AgentsTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/CodeFeature.kt` | `mobile/lib/screens/repos_screen.dart`, `mobile/lib/screens/{repos,repo_detail, mirror_import_sheet}` |
| `A/feature/code/CodeTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/CommitDetailScreen.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/CommitsTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/MirrorImportSheet.kt` | `mobile/lib/screens/mirror_import_sheet.dart` |
| `A/feature/code/MirrorsTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/ReleasesTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/RepoCommon.kt` | `mobile/lib/screens/repo_detail_screen.dart`, `mobile/lib/theme.dart` |
| `A/feature/code/RepoDetailScreen.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/RepoFileScreen.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/ReposScreen.kt` | `mobile/lib/screens/repos_screen.dart` |
| `A/feature/code/WorktreesTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/CollabEnv.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/CollabWidgets.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/CollaborationScreens.kt` | `mobile/lib/screens/repo_detail_screen.dart`, `mobile/lib/screens/new_issue_screen.dart` |
| `A/feature/code/collab/Composers.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/DiscussionScreens.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/IssueDetailScreen.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/IssuesTab.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/NewIssueScreen.kt` | `mobile/lib/screens/new_issue_screen.dart` |
| `A/feature/code/collab/PatchViews.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/code/collab/PullScreens.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `A/feature/tools/DiagnosticsRuntime.kt` | `mobile/lib/services/app_telemetry.dart` |
| `A/feature/tools/TelemetryScreen.kt` | `mobile/lib/screens/telemetry_screen.dart` |
| `A/feature/tools/TelemetryStrip.kt` | `mobile/lib/widgets/telemetry_strip.dart` |
| `A/feature/tools/ToolsCompose.kt` | `mobile/lib/widgets/telemetry_strip.dart` |
| `A/feature/tools/ToolsFeature.kt` | `mobile/lib/main.dart` |
| `A/ui/kit/ComposeIdentityBar.kt` | `mobile/lib/widgets/compose_identity_bar.dart` |
| `A/ui/kit/ConnectionDot.kt` | `mobile/lib/widgets/connection_dot.dart` |
| `A/ui/kit/FmExplorer.kt` | `mobile/lib/widgets/fm_ui.dart` |
| `A/ui/kit/FmInlineStatus.kt` | none (replaced the snack bars of `mobile/lib/theme.dart`) |
| `A/ui/kit/FmLoading.kt` | `mobile/lib/widgets/fm_ui.dart` |
| `A/ui/kit/FmMaterial.kt` | `mobile/lib/theme.dart` |
| `A/ui/kit/FmOverlays.kt` | `mobile/lib/theme.dart`, `mobile/lib/screens/local_repo_screen.dart` |
| `A/ui/kit/FmSurfaces.kt` | `mobile/lib/widgets/fm_ui.dart` |
| `A/ui/kit/GuestGate.kt` | `mobile/lib/widgets/guest_gate.dart` |
| `A/ui/kit/KitGalleryScreen.kt` | `mobile/lib/widgets/{fm_ui,connection_dot,guest_gate,compose_identity_bar}.dart`, `mobile/lib/theme.dart` |
| `A/ui/theme/ForkMeshTheme.kt` | `mobile/lib/theme.dart` |
| `C/account/AuthFlowLogic.kt` | `mobile/lib/screens/auth_flow.dart` |
| `C/account/GuestGateLogic.kt` | `mobile/lib/widgets/guest_gate.dart` |
| `C/account/NotificationsLogic.kt` | `mobile/lib/screens/notifications_screen.dart` |
| `C/account/OrgsLogic.kt` | `mobile/lib/screens/orgs_screen.dart` |
| `C/account/PingSettingsLogic.kt` | `mobile/lib/screens/ping_settings_screen.dart` |
| `C/account/SettingsLogic.kt` | `mobile/lib/screens/settings_screen.dart` |
| `C/api/AccountEventLoop.kt` | `mobile/lib/services/account_event_channel.dart` |
| `C/api/AccountSettings.kt` | `mobile/lib/services/settings_service.dart` |
| `C/api/ApiResults.kt` | `mobile/lib/services/api_service.dart` |
| `C/api/ApiService.kt` | `mobile/lib/services/api_service.dart` |
| `C/api/AppEnv.kt` | `mobile/lib/config/app_env.dart` |
| `C/api/AuthController.kt` | `mobile/lib/services/auth_service.dart` |
| `C/api/HttpTransport.kt` | `mobile/lib/services` |
| `C/api/InboxService.kt` | `mobile/lib/services/inbox_service.dart` |
| `C/api/NotificationInbox.kt` | `mobile/lib/services/notification_service.dart` |
| `C/api/PrefStore.kt` | `mobile/lib/services/{settings,auth}_service.dart` |
| `C/api/PushBinder.kt` | `mobile/lib/services/push_notification_service.dart` |
| `C/api/ServerUrl.kt` | `mobile/lib/services/{api,auth,inbox}_service.dart` |
| `C/api/SyncStatus.kt` | `mobile/lib/services/sync_status.dart` |
| `C/chat/ChatLinkText.kt` | `mobile/lib/widgets/chat_link_text.dart` |
| `C/chat/ChatScreenLogic.kt` | `mobile/lib/screens/chat_screen.dart`, `mobile/lib/widgets/guest_gate.dart` |
| `C/chat/EventLoop.kt` | `mobile/lib/services/{relay_service,local_mesh_service,node_sync_service}.dart` |
| `C/chat/LocalMeshRoom.kt` | `mobile/lib/services/local_mesh_service.dart` |
| `C/chat/MeshTransport.kt` | `mobile/lib/services/mesh_transport.dart` |
| `C/chat/NodeActivityCopy.kt` | `mobile/lib/screens/activity_screen.dart` |
| `C/chat/NodeSync.kt` | `mobile/lib/services/node_sync_service.dart` |
| `C/chat/NodeSyncApplier.kt` | `mobile/lib/services/node_sync_applier.dart` |
| `C/chat/NodeSyncEngine.kt` | `mobile/lib/services/node_sync_service.dart` |
| `C/chat/RelayRoom.kt` | `mobile/lib/services/relay_service.dart` |
| `C/code/CodeHighlighter.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/code/CommitDiffs.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/code/MirrorImport.kt` | `mobile/lib/screens/mirror_import_sheet.dart` |
| `C/code/RepoActivity.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/code/RepoCatalog.kt` | `mobile/lib/screens/repos_screen.dart` |
| `C/code/RepoExplorer.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/collab/Collaboration.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/collab/DiscussionThread.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/collab/IssueFilters.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/collab/IssueThread.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/collab/NewIssueDraft.kt` | `mobile/lib/screens/new_issue_screen.dart` |
| `C/collab/PullThread.kt` | `mobile/lib/screens/repo_detail_screen.dart` |
| `C/crypto/AesGcm.kt` | `mobile/lib/services/owner_crypto.dart` |
| `C/crypto/Ed25519.kt` | `mobile/lib/services/identity.dart`, `mobile/lib/services/trust_lists.dart` |
| `C/crypto/Encoding.kt` | `mobile/lib/services/identity.dart` |
| `C/crypto/Field25519.kt` | `mobile/lib/services/identity.dart` |
| `C/crypto/Hashes.kt` | `mobile/lib/services/owner_crypto.dart` |
| `C/crypto/Keccak.kt` | `mobile/lib/services/owner_crypto.dart` |
| `C/crypto/MlKem768.kt` | `mobile/lib/services/owner_crypto.dart` |
| `C/crypto/RoomCrypto.kt` | `mobile/lib/services/room_crypto.dart` |
| `C/crypto/RoomSeal.kt` | `mobile/lib/services/room_seal.dart` |
| `C/crypto/X25519.kt` | `mobile/lib/services/room_seal.dart` |
| `C/format/Format.kt` | `mobile/lib/format.dart` |
| `C/git/ForkMeshGit.kt` | `mobile/packages/forkmesh_git/lib/forkmesh_git.dart` |
| `C/git/bytes/CheckedBytes.kt` | `mobile/packages/forkmesh_git/lib/src/bytes/checked_bytes.dart` |
| `C/git/bytes/DartConvert.kt` | `mobile/packages/forkmesh_git/lib/src` |
| `C/git/bytes/Encoding.kt` | `mobile/packages/forkmesh_git/lib/src/bytes/encoding.dart` |
| `C/git/crypto/CollisionScreen.kt` | `mobile/packages/forkmesh_git/lib/src/crypto/collision_screen.dart` |
| `C/git/crypto/Digest.kt` | `mobile/packages/forkmesh_git/lib/src/crypto/digest.dart` |
| `C/git/crypto/Sha1.kt` | `mobile/packages/forkmesh_git/lib/src/crypto/sha1.dart` |
| `C/git/crypto/Sha256.kt` | `mobile/packages/forkmesh_git/lib/src/crypto/sha256.dart` |
| `C/git/errors/GitError.kt` | `mobile/packages/forkmesh_git/lib/src/errors/git_error.dart` |
| `C/git/index/IndexStore.kt` | `mobile/packages/forkmesh_git/lib/src/index/index_store.dart` |
| `C/git/index/IndexV2.kt` | `mobile/packages/forkmesh_git/lib/src/index/index_v2.dart` |
| `C/git/objects/CommitObject.kt` | `mobile/packages/forkmesh_git/lib/src/objects/commit_object.dart` |
| `C/git/objects/GitObject.kt` | `mobile/packages/forkmesh_git/lib/src/objects/git_object.dart` |
| `C/git/objects/HeaderBytes.kt` | `mobile/packages/forkmesh_git/lib/src/objects/header_bytes.dart` |
| `C/git/objects/LooseObjectStore.kt` | `mobile/packages/forkmesh_git/lib/src/objects/loose_object_store.dart` |
| `C/git/objects/TagObject.kt` | `mobile/packages/forkmesh_git/lib/src/objects/tag_object.dart` |
| `C/git/objects/TreeObject.kt` | `mobile/packages/forkmesh_git/lib/src/objects/tree_object.dart` |
| `C/git/pack/Crc32.kt` | `mobile/packages/forkmesh_git/lib/src/pack/crc32.dart` |
| `C/git/pack/PackIndexV2.kt` | `mobile/packages/forkmesh_git/lib/src/pack/pack_index_v2.dart` |
| `C/git/pack/PackObjectStore.kt` | `mobile/packages/forkmesh_git/lib/src/pack/pack_object_store.dart` |
| `C/git/pack/PackReader.kt` | `mobile/packages/forkmesh_git/lib/src/pack/pack_reader.dart` |
| `C/git/pack/PackWriter.kt` | `mobile/packages/forkmesh_git/lib/src/pack/pack_writer.dart` |
| `C/git/protocol/FetchResponse.kt` | `mobile/packages/forkmesh_git/lib/src/protocol/fetch_response.dart` |
| `C/git/protocol/PktLine.kt` | `mobile/packages/forkmesh_git/lib/src/protocol/pkt_line.dart` |
| `C/git/protocol/ProtocolV2.kt` | `mobile/packages/forkmesh_git/lib/src/protocol/protocol_v2.dart` |
| `C/git/protocol/ReceivePack.kt` | `mobile/packages/forkmesh_git/lib/src/protocol/receive_pack.dart` |
| `C/git/protocol/Sideband.kt` | `mobile/packages/forkmesh_git/lib/src/protocol/sideband.dart` |
| `C/git/protocol/SmartHttp.kt` | `mobile/packages/forkmesh_git/lib/src/protocol/smart_http.dart` |
| `C/git/refs/RefStore.kt` | `mobile/packages/forkmesh_git/lib/src/refs/ref_store.dart` |
| `C/git/refs/Refspec.kt` | `mobile/packages/forkmesh_git/lib/src/refs/refspec.dart` |
| `C/git/repository/GitOperation.kt` | `mobile/packages/forkmesh_git/lib/src/repository/git_operation.dart` |
| `C/git/repository/GitRepository.kt` | `mobile/packages/forkmesh_git/lib/src/repository/git_repository.dart` |
| `C/git/repository/RemoteOperations.kt` | `mobile/packages/forkmesh_git/lib/src/repository/remote_operations.dart` |
| `C/git/repository/RepositoryConfig.kt` | `mobile/packages/forkmesh_git/lib/src/repository/repository_config.dart` |
| `C/git/security/Cancellation.kt` | `mobile/packages/forkmesh_git/lib/src/security/cancellation.dart` |
| `C/git/security/OperationJournal.kt` | `mobile/packages/forkmesh_git/lib/src/security/operation_journal.dart` |
| `C/git/security/Quarantine.kt` | `mobile/packages/forkmesh_git/lib/src/security/quarantine.dart` |
| `C/git/security/ResourceLimits.kt` | `mobile/packages/forkmesh_git/lib/src/security/resource_limits.dart` |
| `C/git/transport/HttpTransport.kt` | `mobile/packages/forkmesh_git/lib/src/transport/http_transport.dart` |
| `C/git/transport/IoHttpTransport.kt` | `mobile/packages/forkmesh_git/lib/src/transport/io_http_transport.dart` |
| `C/git/transport/RemoteUrl.kt` | `mobile/packages/forkmesh_git/lib/src/transport/remote_url.dart` |
| `C/git/worktree/DartIo.kt` | `mobile/packages/forkmesh_git/lib/src` |
| `C/git/worktree/WorktreePath.kt` | `mobile/packages/forkmesh_git/lib/src/worktree/worktree_path.dart` |
| `C/localrepos/CatalogCanonical.kt` | `mobile/lib/services/git/catalog_canonical.dart` |
| `C/localrepos/ForkMeshRedirect.kt` | `native/ios/ForkMeshCore/Sources/FMCode/Local/ForkMeshRedirect.swift` |
| `C/localrepos/ForkMeshRemote.kt` | `mobile/lib/services/git/forkmesh_remote.dart` |
| `C/localrepos/GitIsolateRunner.kt` | `mobile/lib/services/git/git_isolate_runner.dart` |
| `C/localrepos/GitWorkspace.kt` | `mobile/lib/services/git/git_workspace.dart` |
| `C/localrepos/GitWorkspaceModels.kt` | `mobile/lib/services/git/git_workspace.dart` |
| `C/localrepos/GitWorkspaceOps.kt` | `mobile/lib/services/git/git_workspace.dart` |
| `C/localrepos/LocalImportSource.kt` | `mobile/lib/services/git/git_workspace.dart` |
| `C/localrepos/LocalRepoFlows.kt` | `mobile/lib/screens/{clone_to_device,local_repos_section,local_repo_screen}.dart` |
| `C/localrepos/LocalRepoStore.kt` | `mobile/lib/services/git/local_repo_store.dart` |
| `C/localrepos/PatchBuilder.kt` | `mobile/lib/services/git/patch_builder.dart` |
| `C/localrepos/PublishService.kt` | `mobile/lib/services/git/publish_service.dart` |
| `C/localrepos/PullRequestService.kt` | `mobile/lib/services/git/pull_request_service.dart` |
| `C/localrepos/ShallowPushProof.kt` | `native/ios/ForkMeshCore/Sources/FMCode/Local/ShallowPushProof.swift` |
| `C/localrepos/ZipArchiveReader.kt` | `mobile/lib/services/git/git_workspace.dart` |
| `C/model/Actions.kt` | `mobile/lib/models/models.dart` |
| `C/model/Agents.kt` | `mobile/lib/models/models.dart` |
| `C/model/Bounty.kt` | `mobile/lib/models/models.dart` |
| `C/model/Chat.kt` | `mobile/lib/models/models.dart` |
| `C/model/Discussions.kt` | `mobile/lib/models/models.dart` |
| `C/model/Issues.kt` | `mobile/lib/models/models.dart` |
| `C/model/ModelJson.kt` | `mobile/lib/models/models.dart` |
| `C/model/Network.kt` | `mobile/lib/models/models.dart` |
| `C/model/NotificationDeepLink.kt` | `mobile/lib/services/notification_deep_link.dart` |
| `C/model/Notifications.kt` | `mobile/lib/models/models.dart` |
| `C/model/Orgs.kt` | `mobile/lib/models/models.dart` |
| `C/model/PingSettings.kt` | `mobile/lib/screens/ping_settings_screen.dart` |
| `C/model/Pulls.kt` | `mobile/lib/models/models.dart` |
| `C/model/Releases.kt` | `mobile/lib/models/models.dart` |
| `C/model/RepoContent.kt` | `mobile/lib/models/models.dart` |
| `C/model/Repository.kt` | `mobile/lib/models/models.dart` |
| `C/model/SponsoredAd.kt` | `mobile/lib/models/models.dart` |
| `C/model/auth/AuthModels.kt` | `mobile/lib/services/auth_service.dart` |
| `C/net/WebSocketClient.kt` | `mobile/lib/services/{account_event_channel,relay_service}.dart` |
| `C/security/Identity.kt` | `mobile/lib/services/identity.dart` |
| `C/security/OwnerCrypto.kt` | `mobile/lib/services/owner_crypto.dart` |
| `C/security/PortableIdentity.kt` | `mobile/lib/services/portable_identity.dart` |
| `C/security/TrustLists.kt` | `mobile/lib/services/trust_lists.dart` |
| `C/tools/ActivityRules.kt` | `mobile/lib/screens/activity_screen.dart` |
| `C/tools/AppBuild.kt` | `mobile/lib/config/app_build.dart` |
| `C/tools/AppLog.kt` | `mobile/lib/services/app_telemetry.dart` |
| `C/tools/AppTelemetry.kt` | `mobile/lib/services/app_telemetry.dart` |
| `C/tools/BugReport.kt` | `mobile/lib/widgets/bug_report_button.dart`, `mobile/lib/screens/report_bug_sheet.dart` |
| `C/tools/ChangeNotifier.kt` | `mobile/lib/services/app_telemetry.dart` |
| `C/tools/DevPerformanceText.kt` | `mobile/lib/widgets/dev_performance_overlay.dart` |
| `C/tools/PerformanceMonitorService.kt` | `mobile/lib/services/performance_monitor_service.dart` |
| `C/tools/StallLogFile.kt` | `mobile/lib/services/stall_log.dart` |
| `C/tools/TelemetryViews.kt` | `mobile/lib/widgets/telemetry_strip.dart`, `mobile/lib/screens/telemetry_screen.dart` |
| `C/tools/ToolsFormat.kt` | `mobile/lib/services/app_telemetry.dart` |
- Whether to skip device-key rotation while other projects are signed in;
  each project has its own key on Android, so the risk may not apply.
- iOS work not yet on Android: fetch through the relay's mirror redirect and deepening shallow clones,
  report-a-bug in the bottom bar, reselecting a tab scrolls to the top,
  desktop agent pictures through the relay, cove notices over the relay, and
  on-device Gemma (iOS only, MLX).

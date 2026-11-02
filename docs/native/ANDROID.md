# Native Android app: design notes

`native/android` is the ForkMesh Android app, in Kotlin and Jetpack Compose.
Its source is code only; the reasons behind non-obvious decisions live here.
Build, test and release steps are in
[native/android/README.md](../../native/android/README.md); the phone apps'
home page is [README.md](README.md). Encryption and signatures are explained
once in [encryption.md](../encryption.md).

Paths: `A/` is `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/`,
`C/` is `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/`.

Ids such as S010 name rows of the retired iOS → Android sync ledger
(`git show 63e84567bb:docs/native/SYNC.md`). Since 2026-10-28 a phone change
lands in both apps in the same pull request.

## 1. Toolchain and dependencies

- Gradle wrapper, Android Gradle Plugin with built-in Kotlin (do not apply
  `kotlin-android`), Compose BOM, compileSdk/targetSdk 37, minSdk 26.
- First-party dependencies only: AndroidX/Compose and Kotlin coroutines. HTTP,
  JSON, WebSocket, cryptography, the Git engine, QR codes and icons are in this
  tree. So widgets use `RemoteViews` (not Glance), the QR scanner Camera2 (not
  CameraX), the revive job `JobScheduler` (not WorkManager), chat video
  `VideoView`, and view-once capture the legacy camera API with `MediaRecorder`
  (not Media3).
- **Icon font.** A subset of MaterialIcons-Regular (Apache-2.0, shared with
  iOS) in `app/src/main/assets/fonts`: every codepoint name that appears as a
  word in the tracked tree, plus `help_outline`, the iOS
  `MaterialIconCodepoints` table and the `_outlined` twin of each (section 19).
  A name outside the subset draws `help_outline`. The full table is
  `tools/material_icons/MaterialIcons.codepoints`; after adding an icon name run
  `python3 tools/subset_material_icons.py` (needs fonttools; `--font` takes the
  full font). `app/tests/test_material_icon_subset.py` fails until you do.
- `:core` is pure Kotlin/JVM (no `android.*`), tested on the host JVM by an
  in-tree runner: `./gradlew :core:coreTest -Pfilter=Json` runs classes whose
  name contains `Json`.
- **Build memory.** `gradle.properties` caps the Gradle daemon at 2 GB heap
  (768 MB metaspace) and the Kotlin daemon at 1.5 GB, and stops an idle Gradle
  daemon after 15 minutes. A 4 GB cap left both daemons holding about 6.5 GB,
  which next to the emulator and the iOS Simulator panicked a 16 GB Mac.
  `./gradlew --stop` frees the daemon at once. Raise the caps only on
  `OutOfMemoryError` or `Metaspace` failures.
- `res/xml/network_security_config.xml` refuses cleartext except `10.0.2.2`,
  `localhost` and `127.0.0.1`.

## 2. Architecture

- **Feature seams.** `A/feature/Features.kt` is the only place the shell meets
  the areas. Each area owns one `*Feature` object and package, exposes
  `install(app)` and screen factories, and registers services in
  `C/core/Services.kt`. Cross-area wiring is in `Features.connectAreas`.
  Install order: account, local repos, chat, agents, then the rest.
- **Projects.** A project switch clears `Services` and runs `Features.install`
  again. Each install must be repeatable, launch coroutines in
  `ProjectLifecycle.scope`, and register sockets, threads and audio with
  `ProjectLifecycle.onClose`. Object-level state that outlives a switch is a
  bug.
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
  repositories ([NAVIGATION.md](NAVIGATION.md)).

## 3. Storage, secrets and identity

- **Preferences** (`A/platform/Prefs.kt`) keep their key names across
  releases. A value stored with another type reads as absent. Sign-out writes
  synchronously.
- **Secrets** (`A/platform/SecureStore.kt`): AES-256-GCM under an Android
  Keystore key, with the key name as associated data. A value that no longer
  opens reads as absent. Secrets found in plain preferences are moved in once.
- **Per-project isolation.** The original project owns the un-prefixed
  namespace; others use `p/<projectId>/`; sessions live under
  `auth/session/<projectId>`. `Services.get<SecretStore>()` returns the active
  project's view. The Inbox's local pings (`inbox/local-pings.v1`) live in the
  project's preferences and are rebuilt on a switch. Removing a project erases
  its chat history files file by file first (`ChatHistoryStore.eraseAll`,
  `ProjectsService.eraseProject`), so the original project, whose files are not
  under `projects/`, loses its history too. Every performance report is
  scrubbed (`DiagnosticsBundle.report`), including conversation, channel, room
  and query names in JSON and `name=value` form.
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
  desktop, [portable-identities.md](../portable-identities.md)):
  PBKDF2-HMAC-SHA-256 600000 iterations, 16-byte salt, AES-256-GCM with
  associated data `forkmesh.portable-identity/v1`. Restore validates type,
  schema, a canonical 32-byte seed and the matching public key, and signs out
  the old session.

## 4. Store application id and updates

The store release is an update of the app published as `com.forkmesh.forkmesh`.
The release APK is built with `-Pforkmesh.appId=com.forkmesh.forkmesh` and
signed with the v3 key ([native/android/README.md](../../native/android/README.md));
the default application id `com.forkmesh.nativeapp` installs beside it.

- **Same application id and signing key.** Keystore keys and app data survive
  an update only because the id and signer stay the same. The launcher label
  follows the id (`app/build.gradle.kts`).
- **Component names.** The launcher alias `com.forkmesh.forkmesh.MainActivity`
  and the autofill service `com.forkmesh.forkmesh.vault.VaultAutofillService`
  keep their published names, so pinned icons and the chosen autofill service
  keep working.
- **Version.** The version name and versionCode stay above every release
  already published under the store id.
- **Stable data paths.** Local repositories stay under `AppFiles.documentsDir`
  (`forkmesh/repos`) and `projects/<slug>/forkmesh/repos`
  (`LocalRepoStore.rootIn`); vault device keys, autofill Keystore aliases and
  `noBackupFilesDir/vault-autofill` keep their names.
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
  `unauthorized` to every app session (as iOS `ApiService.endsSession`).
- **Sign-out** clears local state first, then revokes the token best effort.
- **Login** has "Forgot password?" (`AuthController.forgotPasswordUrl`, the
  relay's `/forgot-password`). **Sign up** opens `/signup` in the browser and
  switches to the login form with a "log in here" hint.
- **Update prompt.** `ClientVersionGate.DEFAULT_STORE_URL` is
  `https://forkmesh.com/mobile` (`/download` returns 404), used when
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
  `appearance/layoutMode` (`controlTower` or `focus`, default Control Tower) is
  the phone's copy of the desktop's two layout modes; it sets the CRM's
  density and shows under Theme in Settings > Appearance.

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
  - with notifications off it still runs as a calls-only plan
    (`LinkPlan.callsOnly`, "Listening for calls"): no `#mesh`, and `deliver`
    and `deliverDirectMessage` return at once so unshown alerts are not marked
    delivered; a `calls` event still wakes the active project's call engine
    and `CallRinger` rings (ringing needs no notification permission);
  - posting follows the server's rules (`enqueue_notification`,
    `ping_quiet_until`): one post per ping id, reads elsewhere cancel, nothing
    on the first read after install, nothing older than 24 h, pause, quiet
    hours and muted kinds from cached ping settings.
- **Ping snapshots.** Every fetched ping page is saved per project and account
  (last 100 rows, `inbox/pings.v1/<project>/<node>`, `PingSnapshots`), so the
  Inbox shows at once. A notification carries the whole row
  (`PushTarget.EXTRA_ROW`); a tap inserts it and navigates before marking read.
- **Ping banners** (`PingNotifier.post`, `PingLead.of`, S078,
  [content-first-pings.md](../app/content-first-pings.md)): a ping with an
  actor reads "bob · acme/widgets #42" over the text (the event label when
  there is no text); without one it keeps its label with "#42" in front. The
  label drops a trailing "for/in/on owner/repo" or a leading "owner/repo" as
  `_notification_digest_title` does. Test: `PingLeadTest`.
- **Lock-screen privacy.** Every notification that can carry someone's words
  is `VISIBILITY_PRIVATE` with a generic public version ("ForkMesh" and "New
  message", or "New notification" for pings and desktop alarms). Nearby
  `#mesh` alerts (`PingNotifier.postMesh`, `MeshAlert.shown`) follow the
  "Notification content" setting (`notifications/content`: name and message,
  name only, or neither).
- **Direct messages** (`C/chat/DirectMessages.kt`, rules in
  [chat-private-rooms.md](../app/chat-private-rooms.md)):
  - one socket to the selected DM and every DM with unread messages; reconnect
    1 s doubling to 5 min; selecting marks read; frames sent before `connect`
    returns are queued and flushed;
  - chat protocol v4 ([chat-protocol-v4.md](../app/chat-protocol-v4.md)): a
    per-message ratchet over v3 bundles and trust
    ([chat-protocol-v3.md](../app/chat-protocol-v3.md)). A DM room seals and
    opens only v4 frames (v3 is ignored there, and open rooms ignore v4). Each
    frame has one entry per approved device other than this one; a frame with
    no entry is still sent and the outbox re-seals it later. Own frames that
    come back (`own`) are neither shown nor acknowledged;
  - strict trust in scope `direct:<id>`: bundle `acct` is the signed-in
    account (lowercased; a guest uses its node id), `node` the DM self id; a
    bundle whose `acct` is not one of the two participants is dropped;
  - a further device of an account that already has an approved device is
    pending: nothing is sealed to it, its frames are held (64 per room,
    persisted with the trust records; frames over 96 KiB stay in memory only),
    and a system line and a banner with Approve and Block appear. Several
    waiting devices share one banner and one system line
    (`DirectTrustNotices.pending`, `kids`; "2 new devices for pat want to join
    this chat…"; Block asks first). A pending device on your own account reads
    "A new device was added to your account. Approve it if it is yours,
    otherwise block it.";
  - edits and deletes must come from the author's `node`, reactions from their
    `reactorId`, which must also be the frame's `node`; early edits and
    deletes are held (128 targets, 8 frames each); deletes go into the
    persisted hidden list and send `forkmesh-superseded`;
  - key advertisements are never acknowledged; a frame not wrapped for this
    device triggers one persisted advertisement;
  - up to 64 own durable frames are kept 7 days and re-sealed with
    `resend: true` when a new device is approved, followed by
    `forkmesh-superseded`. The outbox belongs to the conversation, not the
    socket, so it survives a reconnect (own frames cannot be reopened from the
    relay's replay). An entry is complete when wrapped for every approved
    device other than this one;
  - `noSession(kid)` (no session, prekey gone, more than 256 skipped keys or a
    failed unwrap): at most once per room and device every 60 s the phone
    deletes its sessions with that device, sends a persisted, non-notifying
    `{"type":"e2ee-reset","senderId":<node>}` sealed only to it, and one
    persisted key advertisement. It skips this when it already opened a later
    frame (higher `ctr`) from that device, because the frame is a leftover of
    an older session. An opened `e2ee-reset` is acknowledged, not shown, and
    re-seals every outbox entry to that device alone (`resend: true`, no
    `forkmesh-superseded`). Over the mesh the reset goes back as
    `direct-sealed`;
  - no history-replay exception in DMs: an already opened frame is `replay`
    because its message key is gone; after a restart the phone shows what the
    chat history file holds (section 14) plus frames still waiting on the
    relay;
  - Safety number (room menu): 60 digits in 12 groups, the QR string as a QR
    code, "Scan their code" with the pairing scanner, and "Mark as verified";
    any change to the approved set clears verified and adds "Your safety number
    with <name> changed". A scan returns `DirectSafetyScan`: verified, not a
    ForkMesh safety number, a different conversation (the scope between
    `forkmesh-safety:3:` and the last `:` differs), or a mismatch;
  - direct delivery: every DM frame type the phone sends also goes as
    `direct-sealed` to each linked approved device, sealed to that device only;
    files over 256 KiB skip Bluetooth. Calls and receipts stay on the relay.
    Focus is "app in front, that DM on screen, the channel drawer closed"; when
    both sides have focus the frame goes only over the mesh and the relay copy
    follows after 3 s without a `direct-ack` or when the link drops. The route
    shows under the name, joined with " · ": "Direct · Wi-Fi", "Direct ·
    Bluetooth" or "Direct only";
  - DM notifications decrypt on the phone: a `direct-messages` frame names
    `conversation` and `ref`; the service reads
    `/api/chat/direct-messages/<id>/pending?ref=<ref>` (side-effect free),
    decrypts with `RoomCrypto` and opens the v4 frame with `commit = false`
    (`RoomSeal.existing`: no ratchet step, counter or held frame stored, so the
    DM socket can open the same frame later). One `MessagingStyle`
    notification per conversation, lock-screen version "New message"; a
    pending or unknown device, or any failure, gives the generic notification;
  - every `direct-messages` frame, foreground or not, first posts
    `/api/chat/direct-messages/<id>/delivered?ref=<ref>`
    (`DirectMessagePreviews.confirmDelivery`), so the sender sees Delivered
    while this phone has no DM socket open
    ([chat-private-rooms.md](../app/chat-private-rooms.md#delivery-ticks)).
- **Relay room** (`C/chat/RelayRoom.kt`): every frame encrypted twice
  (`RoomCrypto` room key and a v3 `RoomSeal`, open trust, scope = room name),
  public `#channel` chat included, and the bundle carries `nsig` by the node
  key (section 35). A frame with a non-empty `to` other than `hello` is sealed
  to the devices whose bundle `node` is the recipient plus this account's own
  devices (`RelayRoom.sealRecipients`). The relay carries no DMs, private
  channels or invites (section 35). `hello` and its replies list only
  `RelayRoom.advertisedChannels` (no hidden or `#office/` channel), and a
  peer's `hello` adds only `#` channels that are neither; a chat frame or
  restored history never brings a hidden channel back. v2 frames are ignored.
  Dial timeout 15 s; ping every 25 s; reconnect 1, 2, 4, 8, 16, then 30 s;
  presence every 60 s, peers dropped after 180 s. Envelope `version` matches
  iOS.
- **RoomCrypto**: AES-256-GCM, key PBKDF2-HMAC-SHA256(passphrase or app key,
  salt SHA-256("ForkMesh room:" + room)[0..16], 210000 rounds), cached per
  (room, secret).
- **RoomSeal** (`C/crypto/RoomSeal.kt`, `ChatV3.kt`, `ChatDeviceKeys.kt`,
  `ChatTrust.kt`): chat protocol v3. One Ed25519 identity per secure-store
  namespace (`chat/e2ee/ed25519IdentityV3`), signed prekeys X25519 +
  ML-KEM-768 (`MlKem768`, seed `d || z`) in `chat/e2ee/prekeysV3`, a new one
  every 24 h, private halves wiped after 8 days, at most 10. The prekey list
  stores only private halves; public keys are recomputed on load. The
  in-memory key set, trust records and counters are shared per underlying
  store and namespace (`ScopedSecretStore` wrappers compare equal), because two
  copies would rotate prekeys apart and lose frames. Counters are reserved 32
  at a time (one store write per 32 frames, never reused after a crash). Held
  frames are kept only when the signature verifies under the frame's own key
  and the frame has an entry for this device, so junk cannot push real frames
  out of the 64 slots. The legacy v2 key `chat/e2ee/x25519PrivateV2` is deleted
  when the v3 identity is created. Vectors:
  `core/src/test/.../crypto/ChatV3VectorsTest.kt` reads
  `conformance/chat/v3/vectors.json` (copy in test resources).
- **Chat key across a reinstall** (S124). `ChatDeviceKeys.forStore` derives a
  missing chat identity key from the node seed (`identity/ed25519Seed`, same
  HKDF as iOS) and writes `chat/e2ee/ctrFloorV3`, which `RoomCounters` starts
  above; a stored key is kept. `DataFolderIdentity.restoreSeed` removes the
  stored chat key when the restored node key differs, so the old install's
  chat key, kid and safety numbers come back. Guard: `ChatKeyReinstallTest`.
- **v4 ratchet** (`C/crypto/ChatV4.kt`, used by `RoomSeal` under strict
  policy): sessions per room and peer `kid`, at most 4 per peer (least recently
  used dropped, never the current one), 512 skipped keys per session, skipped
  keys older than 8 days dropped whenever that peer's state is stored, at most
  256 keys skipped in one step. State lives at
  `chat/e2ee/ratchetV4/<b64u(room)>` in the spec's canonical JSON, shared per
  store and room like the counters. Seal and open run under one lock per room
  on a copy of the peer's state; a seal writes the new state before returning
  the frame, an open writes it (then the counter) only when the frame opened,
  and a write failure leaves stored and in-memory state unchanged. Replaced
  chain, root and message keys are zeroed. Vectors: `ChatV4VectorsTest.kt`
  replays `conformance/chat/v4/vectors.json` like
  `app/tests/test_chat_v4_vectors.py`; `ChatV4RatchetTest.kt` covers
  reordering, old chains, simultaneous starts, reset, `commit = false`, limits
  and reload.
- **Local mesh** (`C/chat/LocalMeshRoom.kt`): mesh links v3. Encrypted
  multicast beacon (TTL 1) carrying only `{t, v:3, id, port, ts, reply}`, `id`
  random at start and whenever the address set changes; direct TCP links,
  local-address checks before any socket, subnet broadcast and a paced /24
  sweep where multicast is filtered, Bluetooth RFCOMM, and a BLE scan for the
  iPhone's GATT service (`C/chat/MeshBle.kt`, wire format in
  [networking.md](../desktop/networking.md)). Needs a Wi-Fi multicast lock.
  Every link starts with plaintext `fm-link-hello` and `fm-link-auth` frames
  (length-prefixed) and then carries AES-GCM frames with per-direction
  counters; a bad frame closes the link. Peers are keyed by the authenticated
  kid; names travel in the sealed mesh `hello`. The phone forwards every new
  `#mesh` bundle to its other links and relays sealed frames it cannot open to
  links whose device is addressed. A late joiner gets only frames already
  sealed to it: the history file (v2) keeps each message's original sealed
  frame and backfill resends only those. The same links carry `direct-sealed`,
  `direct-ack` and `focus`. The room does not start until `#mesh` is first on
  screen (section 27, S135).
- **Bluetooth stays off the air during calls** (`C/net/RadioPolicy.kt`,
  `A/platform/AudioActivityProbe.kt`). The mesh search pass (paired devices,
  classic inquiry up to 15 s, a 15 s BLE scan, every 2 minutes) and Glasses
  Alert's BLE scan share one radio with Bluetooth headsets and, on combo
  chips, 2.4 GHz Wi-Fi; at full duty they made other apps' calls go silent in
  bursts. Any audio mode other than `MODE_NORMAL` is a call; otherwise
  `isMusicActive` means audio is playing. During a call the mesh skips the pass
  and checks again every 10 s, and a running inquiry or BLE window ends within
  a second. While audio plays it skips the inquiry and scans in
  `SCAN_MODE_LOW_POWER`. Paired devices are dialled only when their class is
  computer, phone or uncategorised, never headsets, car kits or watches.
  Glasses Alert scans in `SCAN_MODE_LOW_POWER` while audio plays or a call is
  up (re-checked on its 5 s tick) and drops the census scan during a call.
- **Node sync** (`C/chat/NodeSyncEngine.kt`, `NodeSyncApplier.kt`):
  `GET /api/sync` is destructive, so the phone stands by while a desktop node
  is enrolled. Poll 60 s, backoff 2 s to 30 s with ±25% jitter. Requests sign
  `forkmesh-issues-pull-v1\n{owner}\n{ts}`. An item is acked only after it is
  durably committed. Written formats match the desktop's stores and
  `server/intake.go` (issue JSON with four-space indent and sorted keys; pulls
  only on `forkmesh/pulls`).
- **Chat privacy** (General settings, `A/feature/chat/ChatPrivacy.kt`): "Hide
  chats in the app switcher" (on by default) sets `FLAG_SECURE`; "Lock chats
  with fingerprint" (needs a screen lock) asks with `BiometricPrompt` on
  Android 9+ and the keyguard credential screen before, when Chat opens after
  the app was in the background. Chat history, the mesh history, `nexus/` and
  the secure store are excluded from cloud backup and device transfer
  (`res/xml/data_extraction_rules.xml`, `backup_rules.xml`; `allowBackup` is
  false); `projects/` is excluded whole because rules cannot match the
  per-project `chat-history` folders.
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

How repositories reach mirrors is in [mirroring.md](../mirroring.md).

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
- **Publishing** (`PublishService`): the record matches the relay's
  `safe_catalog_record` (`app/src/catalog.py`) exactly; `stateHash`/`stateSig`
  are omitted; the catalog `nodeId` is a stable per-install id; clones publish
  `forkedFrom`.
- **Pull requests** are `PatchBuilder` patches for `git apply --index --3way`
  with `index old..new mode` lines; the branch is pushed first.
- **Quick edit** refuses binary, non-UTF-8, oversized or mismatched blobs.
  **Zip import** reads the central directory (zip64), keeps the first
  duplicate, and bounds every read.
- **Provider mirrors**: `ProviderMirrorScreen` over `C/ci/ProviderMirror.kt`
  ([provider-mirrors.md](../app/provider-mirrors.md)).

## 9. Signed writes

- Inbox writes (`C/api/InboxService.kt`) mirror the desktop's
  `submit*ToInbox` and the relay's `verify_issue_event`, `verify_pull_event`
  and `verify_pull_comment_event` (`app/src/events.py`); canonical strings must
  match `app/src/entry.py` byte for byte or the relay answers 401
  `bad_signature`.
- Desktop commands (`C/control/DesktopCommandContract.kt`) post without a
  session token, never sign out on 401, refuse unsupported commands before
  signing, and appear only after the route probe answers.
- Destructive host and node operations use the desktop's prepare/run one-use
  token and a typed name after the screen lock (`UserPresence.confirm`).
  Approving an agent request always opens the app; only Deny works from a
  notification.
  Approving waiting checks from a notification opens the pull request's
  Checks card with the approval queued for five minutes and bound to the
  notification's head; the card asks for the screen lock and then sends it
  (iOS approves in place after Face ID). Approve and Run workflow on the
  Actions screens ask for the screen lock too.

## 10. Platform choices

- **Notification intent token** (`C/security/IntentToken.kt`,
  `platform/IntentTokens.kt`). `MainActivity` is exported, so any app could
  send it `com.forkmesh.inbox.ACTION` (Deny, Retry, Continue run, Mark read),
  `com.forkmesh.call.ACCEPT` or `com.forkmesh.push.OPEN`. Every PendingIntent
  the app builds for these (Inbox actions, the call notification's show,
  accept and decline, the ping tap including call rings) carries
  `com.forkmesh.intent.TOKEN`, 32 random bytes made once per install in the
  private `forkmesh.intents` preferences. `InboxFeature.onIntent`,
  `NotificationActionReceiver`, `DirectCallFeature.onIntent`,
  `CallActionReceiver` and `PushTarget.fromIntent` compare it in constant time
  and drop an intent without it. Even with the token, a row carried in the
  intent is only shown, never written to the inbox cache
  (`PushTarget.cachedRow` looks the ping up in the cache and snapshots only).
  Guard: `core/.../security/PushIntentTrustTest.kt`.
- **Haptics** map to `VIRTUAL_KEY`, `KEYBOARD_TAP`, `CONTEXT_CLICK`, all
  suppressed under reduced motion.
- **Timers** ring through `AlarmManager` (`TimerAlarmReceiver`, rescheduled
  after boot) with `SCHEDULE_EXACT_ALARM`; they are device-wide.
- **Glasses Alert** (`feature/glasses/`, core `glasses/`) scans in a
  `connectedDevice` foreground service with manufacturer-data filters for every
  watched company ID and service-UUID and service-data filters for every
  watched 16-bit service (Android pauses unfiltered scans with the screen off);
  matches by name alone come from the foreground census scan. The alarm uses
  `USAGE_ALARM`, raises `STREAM_ALARM` while ringing and, with
  notification-policy access, passes DND. In the foreground an unfiltered
  census scan feeds the device grid (`GlassesCensus`); `GlassesHoming` and
  `GlassesCompass` drive closeness and direction; `GlassesRadarChip` sits in
  the top bar. Device kinds match iOS
  ([FMTools.md](ios/FMTools.md)).
- **Router** (`feature/openwrt/`, core `openwrt/`) talks to an OpenWrt router
  through `RouterHttp`, a small HTTP/1.0 client on a plain or TLS socket,
  because the network security config forbids cleartext and a domain rule
  cannot name every private address. It is used only for addresses
  `OpenWrtModel.isLocalHost` accepts
  ([openwrt.md](../marketplace/openwrt.md#phones)).
- **Phone Security** (`feature/phonesecurity/`, core `phonesecurity/`) is the
  local checkup in [phone-security.md](../marketplace/phone-security.md).
  `PhoneSecurityProbe` fills `PhoneSecurityFacts` on `Dispatchers.IO` when the
  page opens, on **Run again** and on return from Settings; a refused read
  leaves its field `null` and the check reports Unavailable. Scanner, `/proc`
  parsers, filters and summaries run on the host JVM
  (`./gradlew :core:coreTest -Pfilter=PhoneSecurity`). Root and Xposed manager
  packages are declared in the manifest's `<queries>` (no new permission);
  `PhoneSecurityCatalogTest` fails if one is missing. A fix starts its
  `android.settings.*` action (ForkMesh's own app details use a `package:`
  URI), falling back to `Settings.ACTION_SETTINGS`. Android limits:
  `lock_screen_allow_private_notifications`, `enabled_notification_listeners`,
  `private_dns_mode` and `/sys/fs/selinux/enforce` are hidden on many builds,
  so those checks report Unavailable (Private DNS falls back to
  `LinkProperties.isPrivateDnsActive`); hardware-keystore feature flags exist
  only from API 28 (StrongBox) and 31, so older phones are judged from an
  existing key's `KeyInfo`; Nearby devices is Unavailable before Android 12;
  `/data/adb` is unreadable, so the Magisk, KernelSU and APatch probe catches
  only what SELinux lets through. The relay rule treats `10.0.2.2` as loopback,
  matching `AppEnv.isLoopbackHost`.
- **Notifications** need `POST_NOTIFICATIONS` on 13+ (when it is asked:
  section 27, S135).
- **Crash reports** come from `ApplicationExitInfo`. **Stall log** is
  `files/diagnostics/stalls.log`; sampling runs only while an activity is
  visible.
- **App Links** for `app.forkmesh.com` and `forkmesh.com` need
  `/.well-known/assetlinks.json` with the v3 release signer
  (`DEFAULT_SIGNER_SHA256` in `tools/forkmesh-release-host.py`).
- **Feedback is inline, never a toast.** No `Toast`, snackbar or floating
  notice. Results go into an `FmStatus` (`A/ui/kit/FmInlineStatus.kt`) held by
  the screen, sheet or row that ran the action, drawn in layout flow with
  optional action and dismiss buttons; errors stay until dismissed. Actions
  with no owning screen report where the thing is listed
  (`NavigationFeature.linkProblem`, `InboxFeature.actionProblem`,
  `agentsNotice`). The vault autofill activity shows errors in its own dialog.
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
- Approving waiting checks from a notification opens the app (the screen lock
  needs an on-screen dialog to fall back to); iOS approves from the banner.

iOS features Android does not have, because they need an Apple-only API:

- **iPhone Duo's side column**: iOS moves bars under the outer camera, a system
  layout tied to that hardware. Android foldables have no camera column, so
  they keep their bottom bar and rail.
- **Cloud Dragon** (2026-11-01): the second Genie mascot on iPhone
  ([GENIE.md](GENIE.md#the-cloud-dragon)). It is iOS only until the owner asks
  for it on Android; the Android spider is unchanged and Android has no mascot
  choice yet.
- **Eye Control** (S050): a gaze cursor needs ARKit TrueDepth face tracking.
- **Spider Room** (S051): an ARKit and RealityKit scene; ARCore would be a
  new dependency.
- **App Store update gate** (S052): Android is distributed directly and has
  its own update notice.
- **On-device Gemma** (S053): MLX runs on Apple silicon only.
- **The system call screen** (S083, S138): CallKit and PushKit; Android rings
  through its own `CallStyle` notification, in China too.
- **The native call engine** (S085, S086, S089): Android one-to-one calls run
  in the WebView call page, which has the voice changer, call backgrounds
  and the face changer; group calls run on FM Media in the same page.
- **3D face tracking in the face changer** (S087): blend shapes and depth
  edges need TrueDepth. The call page runs the hair network and the edge
  matte iOS moved to Metal.
- **Dictation during a call**: Android's `SpeechRecognizer` opens a second
  microphone capture that can take the microphone from the call, so it is
  off while a call runs.
- **The build Mac light and per-runner agent counts by the clock** (S119):
  Android has no data for either, and the desktop light hides on a phone
  that never paired a desktop instead of blinking red.
- **The "face filter is slow" call diagnosis** (S128): the call page sends
  no face-filter timings.
- **A face stream for the phone's own weak uplink** (S139): the shared web
  engine `fmm-call.js` has no separate face stream, so Android switches to a
  face stream when the server asks for viewers on weak links but not for its
  own uplink. Closing it means changing the web engine for every client.

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
([core-plugins.md](../marketplace/core-plugins.md), "Phone starter packs").

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
  gated package except the organization packages) sits beside `STARTER`. A new
  project with no `packages/starterPack` shows `StarterPackPage`
  (`A/feature/market/StarterPackScreen.kt`) once as a step of
  `A/feature/account/OnboardingFlow.kt` ("Choose how to start", Standard
  preselected, "Start with <pack>"), driven by
  `PackageGateFeature.starterPackPending`; Continue stores the pack id
  (`chooseStarterPack`) and republishes the gate. Unset reads as `STANDARD`;
  legacy projects are never asked. `MarketModel.starterPack` feeds the same
  pack to the Market's fallback. Same ids and contents as iOS.
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
  offer the web Market's sorts (including Most opened and Installed first) and
  filters, usage from `/api/market/installs` and
  `/api/market/reviews/summary`, and one-tap install (`MarketModel.cardAction`).
  Desktop-only packages (`PackageInstallMode.DESKTOP`) offer no phone install.
  Installed cards show Uninstall; each package page has a ⋯ menu with Rate,
  Send feedback or report a bug, and Uninstall (`MarketModel.uninstallAction`,
  confirmed in `A/feature/market/PackageUninstall.kt`): account packages leave
  every device, per-phone and organization packages with a phone switch hide
  on this phone, and organization-wide removal stays in the package page's
  Organizations section. `routeFor` also accepts the iOS destination names
  `fileExplorer`, `speech`, `logs`, `diagnostics` and the capitalised `Agents`,
  `Inbox` and `Chat`, so a catalog row written for iOS opens the same screen.
  The navigation line (`MarketNavBar.kt`, S029) is iOS's: the 44 dp glass Back
  circle, a 36 dp search capsule in the title's place, a sort menu and a filter
  menu whose icons turn accent when set off the defaults (`filter_list` to
  `filter_alt`); each filter row opens its own choice menu (Compose has no
  nested menus). Refresh is pull to refresh.
- **Plugin ⋯ menu.** Every plugin's own screens carry the same ⋯ at the top
  right of the app bar (user request). It opens one small popup: the plugin's
  name, stars, one text box, "Send to agent" and "Send", then Uninstall under a
  divider (one row per organization for organization packages). Rules match
  iOS ([NAVIGATION.md](NAVIGATION.md), Plugin pages) and live in
  `C/market/PluginFeedback.kt`: Send saves a tapped rating and files any text
  as package-tagged feedback; below five stars the text is the review's problem
  and a second box asks how to improve it; Send to agent opens the composer
  (`NavigationFeature.compose`) with a prompt naming the plugin and files
  nothing. A screen opened through `NavigationFeature.screen(route)` is tagged
  with its package (`Screen.pluginPackage`, from `C/market/PackageMenu.kt`,
  which reuses `PackageGate.ROUTE_KINDS`); `NavigatorHost` and the tab stacks
  provide it as `LocalScreen`, and `ToolsAppBar`, `CodeAppBar` /
  `DetailScaffold`, `RoutePage`, `CollabTopBar`, `RssScaffold` and
  `OrgScreenHeader` draw `PluginMenuButton()` for a tagged screen, so new
  plugin screens get the menu free. Tab roots (Agents, Inbox, Chat), the
  composer, the shared repository page and screens a plugin pushes directly
  are not tagged. Screens that already had a ⋯ (note editor, event, CRM, CRM
  contact, Coves, Special agents) pass `pluginMenu = false` and add
  `PluginMenu.Items(divided = true)` (one "Rate, report or uninstall…" row).
  `A/feature/market/PluginMenu.kt` loads the catalog and the viewer's review
  only when the popup first opens and posts feedback to `POST /api/bug-reports`
  with `package`; it never needs the Bug reports package. The full
  `RateScreen` and `ReportBugSheet` stay on the Market page. After an
  uninstall the plugin screen pops; an organization removal waits for its
  export to be saved, and a failure shows a dialog.
- **Marketing Reddit desk.** `A/feature/market/MarketingScreens.kt`
  (`redditSection`) lists the team's Reddit accounts under "Your accounts"
  (`redditDesk.accounts`, each opening u/name's posts), edited in the
  REDDIT_WATCH form's Accounts field (one per line, up to 10; a name, `u/name`
  or a profile link) and sent as `reddit.accounts` in the same settings PUT as
  subreddits and searches. Each thread row has a second line from
  `MarketingRecord.redditActivity` (`C/market/MarketingWorkspace.kt`):
  comments, points and views, the watched comment's views and points, "views
  show only to u/…" when Reddit hid them, the account the extension read as,
  and when it last read.
- **Face Changer.** Native, not the web page. `C/facechanger/FaceChangerLook`
  holds face, background and voice choices, catalogs, preference keys and
  listeners. `A/feature/facechanger/` holds `FaceChangerFeature` (the one look,
  saved to preferences and photo files, the shared `face-changer` worker
  thread and the `WebCallPeers` factory hosting `direct-call.html`),
  `FaceChangerControls` (Face, Background and Voice buttons and pickers) and
  `FaceChangerScreen`, which previews the camera and records a clip into the
  `face-changer-share` cache path for an `ACTION_SEND` chooser. Direct calls
  build on the same look, factory and worker
  ([call-filters.md](../app/call-filters.md), Face Changer on the
  phones). A stop with nothing recorded (`empty_recording`, S037) says
  "Nothing was recorded. Record for a little longer, then stop."
- **App Store Status.** `A/feature/appstorestatus/` draws
  `/dashboard/app-store-status` natively (Insights, route `appStoreStatus`)
  from `GET /api/app-store-status` through `MarketFeature.client()`.
  `C/market/AppStoreStatus.kt` decodes leniently (missing field takes its
  default, unknown tone is neutral, unknown keys ignored) so an older app
  keeps working as the report grows. Tones map to `danger`, `warning`,
  `success` and `textTertiary`. Refresh only re-reads the stored snapshot; a
  newer one needs the desktop's **iOS App Store status** shortcut
  ([app-store-status.md](../marketplace/app-store-status.md)). TestFlight
  links open in the browser, `https://` only. `AppStoreStatusLoader.load`
  (S035) asks the paired desktop (`stats.appStoreStatus`, 120 s, through
  `HubControlCalling`) when the server has no page or its snapshot predates
  tester screenshots (`AppStoreSnapshot.lacksScreenshots`: screenshot feedback
  exists and none has a picture). A stale snapshot is replaced only by a
  desktop answer with pictures; otherwise the page keeps it with "Tester
  screenshots appear once the iOS App Store status shortcut runs again on your
  desktop." A desktop answer reads "From your desktop <time>".
- **Usage.** `A/feature/usage/` draws `/dashboard/usage` natively (Insights,
  route and package `usage`) from the desktop's `agentops.usage` through
  `AgentUsageModel`, with the account actions and the agent-in-a-terminal
  form; the Usage card on Desktop agents opens it
  ([Usage on Android](../marketplace/usage.md#usage-on-android)).
  Refresh sends `agentops.usage` with `refresh: true`; opening is a plain read;
  each meter shows `UsageFormat.resetClock` ("Resets 3:45 PM", "Resets Tue
  9:00 AM", "Resets Oct 26, 10:00 AM"; `UsageResetClockTest`).
- **Shortcut steps.** `C/teammates/ShortcutsStore.kt` decodes the optional
  `steps` of each `insights.shortcuts` entry into `ShortcutStep` (name, and
  `shortcut` when the step runs another shortcut), leniently: non-list
  `steps`, non-object items and items with no name are dropped (a step with
  only a `shortcut` is named after it), at most 32 kept, the desktop's limit
  ([shortcuts.md](../desktop/shortcuts.md)). `ShortcutCatalog.target` resolves
  a link to an entry in the same scope (exact file, else without extension),
  never to the shortcut itself. `A/feature/teammates/ShortcutsScreen.kt` shows
  `N steps` under a row and draws the steps as a numbered timeline in the
  detail (circles sized in `sp`, rows at least 48 dp, names wrap, linked steps
  in `accent` with a chevron that opens and scrolls to the linked shortcut).
  TalkBack reads "Step 2 of 7, Deploy Android build, opens shortcut".
- **RSS Reader** (`rss-reader`, route `rssReader`, Personal group;
  `/dashboard/rss`, `forkmesh://rss`, `forkmesh://rss-reader`). Local only:
  `C/rssreader/` holds everything testable on the JVM (parsing, the guid rule,
  sanitizing, discovery, reader extraction, OPML, library and storage) and
  `A/feature/rssreader/` the screens and `RssReaderService`. Nothing is sent to
  an account; the only traffic is feed, page and icon requests
  ([rss-reader.md](../marketplace/rss-reader.md)).
  - **XML**: an in-tree forgiving parser (`RssXml`) that copes with HTML
    entities, bare `&` and undeclared `dc:` / `content:` prefixes, and skips
    `<!DOCTYPE …>` unread, so there are no external entities (no XXE).
  - **Guid**: guid, then link, then `rdf:about`; else `title|published-ms`
    (just the title without a date); Python's whitespace set collapsed and the
    result cut to 600 code points (`RssParserTest.testGuidRuleMatchesTheContract`).
  - **Storage**: JSON under `filesDir/rss-reader/` (`library.json`;
    `articles/<feedId>.json`; `icons/<feedId>`), written to a temporary file
    and renamed; saves are debounced and rewrite only what changed.
  - **Fetching**: `RssUrlConnectionHttp` follows up to five redirects by hand
    so permanent ones (301/308) land in `previousUrls`, sends conditional GETs,
    caps feeds at 2 MB and sends `ForkMesh-Reader/1.0 (Android)`. Refresh runs
    when the reader opens and on pull to refresh (six-thread pool, no timers);
    a feed is due 30 minutes after its last check; failures back off up to a
    day.
  - **Cleartext** stays off: an http feed is fetched over https and keeps its
    http address (`viaHttps` in Feed Info); when https fails too its error is
    `http_only`.
  - **Article view**: a `WebView` with JavaScript, file and content access, DOM
    storage and geolocation off; the HTML is sanitized again from the allowlist
    and carries a `default-src 'none'` CSP. With Load Remote Images off, images
    and media become links and the CSP drops http(s). Links open through
    `ACTION_VIEW`; the feed icon is a `data:` URI from the cache. The article
    screen shows the timeline's unread count right of Back (`RssCounts.of`,
    S031), nothing at zero.
  - **Appearance**: Light or Dark wraps the reader in `ForkMeshTheme(dark = …)`;
    Automatic follows the app.
- **Vault import and export (S033).** `C/vault/VaultTransfer.kt` is iOS
  `VaultTransfer.swift`, `VaultZip.swift` and the text of
  `VaultTransferModel.swift`: the same CSV reader (header aliases, quoted
  fields, BOM, UTF-8 with Latin-1 fallback, 16 MB cap), source detection order
  (Bitwarden, Firefox, 1Password before Apple's `OTPAuth`, Chrome, DuckDuckGo),
  the ZIP reader for Safari's export (`java.util.zip` `Inflater` and `CRC32`),
  the duplicate plan and the sentences. An archive whose CSV entry is itself
  an archive is refused (`zip_unreadable`), not followed. `VaultTransferTest`
  runs the iOS cases with the same fixtures. `VaultService.importLogins` saves
  through `VaultClient.save` and stops at the first error.
  `A/feature/tools/vault/VaultTransferSection.kt` (Vault settings, between
  autofill and locking) imports through `OpenDocument` and exports
  `ForkMesh Passwords.csv` through `CreateDocument` after asking. Credential
  Exchange is iOS-only, so there is no Send to another app.
- **CRM** (`A/feature/market/Crm*.kt`, core `market/`):
  - **Zoho Mail status (S034).** `CrmZohoStatus.fromJson` reads `settings.zoho`
    (`clientId`, `hasClientSecret`, `canSend` defaulting to `connected` for
    older servers, `lastSentAt`, `lastSyncError`, `steps` without text
    dropped). `CrmZohoBar` shows the status word (Not connected, Read only,
    Sync failed in orange; Reads and sends in green), last synced and sent,
    and each missing step with Sync now or (Re)connect Zoho Mail when the
    organization has one-click Zoho and the viewer manages it, otherwise the
    Self Client or ask-an-admin line. Settings add Sending (Allowed or Read
    only) and Reconnect. `CrmZohoCode.of(status)` prefills client ID and data
    centre; the secret field may stay empty while the client ID is the saved
    one (`usesSavedSecret`).
  - **Contact paging (CRM 1.46.0).** Both phones open the CRM with
    `GET /api/crm/<org>?contacts=250` (iOS `PackagesClient.crmFirstContacts`,
    Android `CRM_FIRST_CONTACTS`), show those contacts, then read the rest
    1000 at a time (`crmContactsPage`, following `next`) in the background and
    append them, skipping ids already shown; the header reads "Loading
    contacts, N of M" meanwhile. The summary leaves out `customers` when it
    pages, so `CrmOrganizationData` counts stage `customer` itself
    (`countsCustomers`). Every `load()` bumps a generation (iOS also cancels
    `contactsTask`), so a refresh or an organization switch drops an older
    load's pages. A failed page stops the load, shows "Some contacts did not
    load" and clears the revision, because the summary's `since` would
    otherwise answer unchanged while contacts are missing. The in-memory
    organization cache is written only once every page is in. iOS runs the
    pages in a main-actor `Task`; Android in a background thread
    (`CrmModel.background`) that calls `onContactsChanged` after each page,
    which the app posts to the main thread as `CrmRevision.bump()`. Callers
    outside the CRM screen (Genie, the toolbox, navigator counts) still read
    every contact in one summary. Tests: iOS `CrmPagingTests`, Android
    `CrmPagingTest`.
  - **Layout setting (S150).** `LayoutModeChoice` (`core/.../settings/Appearance.kt`)
    is stored by name in `appearance/layoutMode` (anything else reads as
    Control Tower). Settings > Appearance shows it under Theme
    (`LayoutModeTrack`, tags `appearance-layout-*`). `crmDense()` reads it, so
    the CRM redraws as the mode changes; the CRM's top bar has a layout button
    and its More tab repeats both tracks.
  - **Sections (S149).** `CrmSection` (`core/.../market/CrmDashboard.kt`) is
    Home, Contacts, Pipeline, Activity, More, in a glass `CrmSectionBar`.
    `CrmModel.show` picks a matching tab. Home (`CrmHomePane`) has a greeting,
    the Zoho alert, four KPI tiles from `CrmDashboard`, the busiest pipeline's
    stage bar, two "up next" picks across every contact
    (`nextContact(mode, everyone = true)`) and recent emails; Control Tower
    puts KPIs first in filled tiles, Focus puts the picks first on plain
    cards. Pipeline offers Board or Stages (collapsible groups with a chip
    that moves a contact on, Android's stand-in for the iOS swipe); Control
    Tower opens on the board. More holds settings, mail, lists and pipelines,
    CSV import and export and the plugin options. Contact rows, header and
    buttons are tighter in Control Tower. The contact page switches Overview,
    Activity and Notes with a segment track.
  - **Reliable adds (S151).** The editor keeps Contact, Pipeline and Notes open
    and folds the rest into "More details" with a filled count. A new contact
    sends a `requestId` made once per form (`CrmModel.newRequestId`), and
    `save` retries a dropped connection or a 408, 425 or 5xx twice (0.4 s, then
    1.5 s) only with one, so a retry cannot add the contact twice. A
    `contact_email_exists` reply names the contact (`PackagesException.from`
    copies `contact` into `contactId`, `contactName`, `contactEmail`); the form
    shows Open and "Add these details", which sends `CrmContactEdit.additions`
    (only fields the record lacks, notes appended, new tags as `addTags`, the
    list as `views`). The editor saves on its own coroutine, so the error shows
    once at the top of the form.
  - **Email reading report (S059).** `CrmActivity` decodes `automated`,
    `opens`, `clicks`, `readSeconds`, `scrollDepth` (capped at 100),
    `slideCount`, `slideReached` (capped at the slide count), `slideTitle`,
    `mailClient` and `readingReported`; counts are read as longs and clamped
    to 0..Int.MAX_VALUE so a hostile payload cannot overflow.
    `engagementLine` ("2 opens · read 4 min+ · reached slide 9/14 · Market ·
    iPhone · Apple Mail", or "Gmail doesn't report read time") sits under each
    sent email, and `CrmEmailScreen` adds a Reading card. Test:
    `CrmReadingReportTest`.
- **Reported bugs inbox (S036).** As iOS `ApiService.bugReportInbox`: a 404 or
  405 from the inbox, a status change (unless `report_missing`) or an
  attachment (unless `attachment_missing`) reads "This relay cannot list
  reported bugs yet. Update the relay and try again." An interrupted load
  (`InterruptedIOException` other than a timeout, `ClosedByInterruptException`,
  `CancellationException`) returns `BugReportInbox.CANCELLED_LOAD`, which only
  clears the spinner so the next load runs.
- **Calendar saves survive a late list (S040).** Each `load()` takes a token
  and the change revision it started from. A list that answers after a newer
  load started is dropped, and one that answers after a create, update, RSVP
  or delete is merged with the local changes made since, so a new event is not
  lost and a deleted one does not return. Saves run on other IO threads, so
  token, revision and event list share one lock. Test: `CalendarSaveTest`.
- **Mastodon** (`mastodon`, route `Route.Mastodon`, Personal group;
  `/dashboard/mastodon`, `forkmesh://mastodon`). `C/mastodon/` holds the
  host-JVM-tested part (server parsing, PKCE, the callback `state` check,
  post HTML to segments, the character count, Link paging, models and the
  client over a `MastodonTransport`); `A/feature/mastodon/` the Compose
  screens and `MastodonState`. The browser returns to
  `forkmesh://mastodon/callback`, which `MastodonFeature.onIntent` takes
  before `NavigationFeature` so the code never becomes a route; the pending
  sign-in lives in the project's `SecretStore` (`mastodon.pending.v1`), so a
  process the system ended while the browser was open still finishes. The
  iPhone uses `ASWebAuthenticationSession` for the same step
  ([Mastodon](../marketplace/mastodon.md#on-the-phone)).

## 14. Chat features

- **View-once video and voice** (DMs only,
  [chat-private-rooms.md](../app/chat-private-rooms.md)):
  `C/chat/ViewOnceMedia.kt`, `A/feature/chat/ViewOnceMessages.kt`. Media is
  encrypted with a fresh key, uploaded in 512 KiB parts, then announced by a
  sealed `chat` frame. The viewer uses `FLAG_SECURE`, decrypts in memory to one
  temp file, and calls `consume` on playback. `ViewOnceStore` states: `sent`,
  `ready`, `opened`, `expired`, `withdrawn`; only a ready recipient entry holds
  the key. Receipts for unopened view-once frames are held so the relay replays
  them after a restart. `MessageCaptureFilters` is a pass-through seam.
- **Receipts** (`ChatReceipt`, `A/feature/chat/ChatReceiptMark.kt`): Sending,
  Sent, Delivered, Read. DMs advance on `forkmesh-recipients`,
  `forkmesh-delivered` and `forkmesh-read`; Office rooms stop at Sent; relay
  rooms show Sent. `DirectMessageService` stamps `ChatReceiptTimes` (delivered,
  read) from its clock the first time a message reaches each mark (relay frames
  carry no time); restoring never stamps. Saved as `receiptTimes` (epoch ms)
  beside `receipts`.
- **DM replies, reactions, edits, delete for everyone** use the relay-room
  frames sealed through `sendSealed` (author rules in section 6).
- **Message long press** (`messageActionsFlow`, `A/feature/chat/ChatSheets.kt`):
  quick-reaction circles, then 44 dp action rows in three groups (sticker,
  thread, edit, copy, select; prompt, issue, profile; delete) that scroll at
  the sheet's 9/16-screen cap, so Delete stays reachable on small phones. The
  circles and reaction chips are a translucent `textPrimary` wash with a border
  (a `bgRaised` fill vanished on the white sheet).
- **More reactions** (`ChatReactionPicker`, `ReactionCatalog.groups`): about
  320 emoji in seven tabs. The 18 catalog emoji are sent as their slug
  (`valueForEmoji`); any other as the literal emoji, which desktop, iOS and web
  show as text (up to 16 code points).
- **Stickers** (`StickerCatalog`, `ChatStickers`): tilted badges on a
  message's corner, sent as reaction frames
  ([chat-stickers.md](../app/chat-stickers.md#android)); in bubble rooms they
  sit on the bubble's bottom corner (`ChatBubbleActions.onSticker`).
- **Brief inactive steps** (S095, iOS `AppReturnGate`). `MainActivity` reports
  `AppLifecycle.visibility` (`C/core/AppReturnGate.kt`: `RESUMED` on resume,
  `INACTIVE` on start and pause, `HIDDEN` on stop) and `visible`, so a
  permission prompt, biometric sheet or share sheet no longer closes sockets.
  The account event socket, node sync, DM focus and live sharing follow
  `visible`; `ChatFeature` runs `RelayRoom.onResumed` and the mesh announce only
  on a resume after `HIDDEN` (and the first after install).
  `ChatHub.applyReadMarks` skips a fetch while one runs or within 5 s of the
  last (`force` for the read-marks push and a sign-in), and
  `DirectMessageService.refresh` shows `loading` only while the list is empty
  and notifies only when records, cursor or error changed. Other features keep
  `foreground`.
- **Chat list** (`A/feature/chat/ChatChannelList.kt`): server DMs newest first
  with unread pills and avatars; the last-line preview is built only from
  messages on the phone (`C/chat/ChatListPreview.kt`). A DM tile's long press
  offers Block.
- **Link previews (S100).** `core/.../chat/ChatLinkPreview.kt`,
  `ChatLinkSites.kt` and `ChatLinkPreviewService.kt` port iOS (site catalog,
  oEmbed and Open Graph, browser retry, cache per project in
  `cacheDir/chat-link-previews-<scope>.json`, dropped by `leaveProject`).
  `ChatLinkPreviewPolicy.autoLoads`: your own links, DMs and private office
  rooms (not `general`, `random`, `welcome`) load at once; shared rooms show
  "Show preview of host", because a fetch tells the site this phone's address.
  Every fetch, redirect hop and image goes through `ChatLinkGuardedHttp`
  (`ChatLinkAddresses.kt`): the host must resolve only to public addresses
  (`ChatLinkAddresses.isPublic`: no private, loopback, link-local, CGNAT,
  multicast, documentation, ULA, site-local or v4-mapped/NAT64/6to4 forms),
  redirects are followed by hand, and `ChatLinkPublicSocketFactory` checks the
  address the socket connected to, so DNS rebinding is refused. Settings > Chat
  has the switch. Guards: `ChatLinkAddressesTest`, `ChatLinkPreviewTest`.
- **Local file names.** Chat media go to
  `cacheDir/chat-media/<first 24 hex of SHA-256(message id)>-<name>`
  (`ChatMediaFile`; a 32-bit hash let another sender collide and replace your
  file) and the path must stay inside the folder. Files open/share copies use
  `FilesModel.localTarget`: the last part of the listed name; `.`, `..` and
  empty names are refused with "That file name cannot be saved on this phone."
  Guards: `ChatMediaFileTest`, `FilesModelTest`.
- **Guest lines (S114).** A line that reads "Guest" is the bot line ("forkbot ·
  via Guest 1234") and the roster owner. `NodeOwners` keeps a "no owner" answer
  one minute (`RETRY_MS`), and `watch(node, claim)` (from `RelayRoom` and
  `OfficeChannelService` for unconfirmed `chat` and `thread-reply` lines) asks
  again every 61 s, at most ten times and 256 nodes, until the server names the
  claimed account; then lines and roster are relabelled. An answer naming
  another account leaves them as guests. Guard: `NodeOwnersTest`.

### Direct calls

Files: `A/feature/chat/DirectCall*.kt`, `PreCallScreen.kt`, `CallRinger.kt`,
`IncomingCallNotifier.kt`; engine `C/chat/DirectCalls.kt`; protocol in
[chat-direct-calls.md](../app/chat-direct-calls.md). Calls run in the
`direct-call.html` WebView (section 11); reconnects, the mini window, staged
video, Diagnose and call audio are in section 27, "Calls".

- **Pre-call check.** Voice call and Video call set `DirectCallFeature.precall`
  after the permission prompt, and `DirectCallOverlay` draws `PreCallScreen`
  as a full-screen layer (not a Navigator screen, so the call screen replaces
  it cleanly). Refused while a call is active or `WebCallPeers.busy`. It opens
  a preview through `WebCallPeers.preview(…, media, owner = "precall")`: video
  with the look's face, background and voice; voice with the microphone only
  and the avatar. The WebView is attached from the first frame (under the
  avatar for voice), because a detached WebView never starts `getUserMedia`.
  Face, Background, Voice and Hear myself match the Face Changer screen.
  "Record test clip" records up to 10 s into `cacheDir/precall-test` and plays
  it back (`VideoView` or `MediaPlayer`, media audio attributes and
  `CallSpeaker`, so it plays on the loudspeaker even while the WebView holds
  communication mode) with Replay, Re-record and Discard. The page says "Calls
  go directly between your devices when possible."
- **Handoff.** `makePeer` reuses the previewing peer when its media equals the
  call's, so the camera or microphone is not reopened. Leaving the pre-call
  layer ends the preview only when no call is active (checked on the worker
  after the queued `start`). A call that ends without taking the preview calls
  `endPreview("precall")`, which leaves a Face Changer preview alone.
- **Quality widget.** `direct-call.html` posts `quality` about once a second
  (`app/public/call-quality-meter.js`). `C/chat/CallQuality.kt` mirrors the web
  thresholds (RTT 150/300/500 ms, loss 1/3/8 %, jitter 30/60/100 ms), labels
  and text; `WebCallPeers.quality` keeps a `CallQualityHistory` (last 50 voice
  levels, 60 live samples), reset per peer. `CallQualityWidget.kt` draws a
  dark pill at the top start (signal bars in the grade colour, a 20-sample
  mirrored waveform, RTT); tapping opens Voice, Network and, on video, Video in
  and out rows; the open state is the `fm.call.qualityOpen` pref (web key
  name). Before connect and on the pre-call screen the page sends `live:false`
  and the pill shows only the mic. The Face Changer studio shows the mic-only
  pill too. Test: `CallQualityTest`.
- **Layout** (iOS `DraggableCallPreview`, `ViewThatFits`). The page draws the
  self view; `DraggableSelfView` lays a transparent 110x160 dp box over it and
  moves `#local` with `window.fmCallPlaceLocal(x, y, w, h, settle)` (CSS px
  equal dp at `width=device-width`). On release it snaps to the nearest corner
  after the fling's last 0.15 s (`C/chat/CallScreenLayout.kt`,
  `CallPreviewLayout.settle`), clear of header and controls, kept in
  `fm.call.previewCorner`. It shows only in a live video call with the camera
  on. Buttons (`CallControlsLayout.arrange`): one row with 16 dp gaps, then
  8 dp, then two rows (face, background, voice above; mute, camera and a 64 dp
  hang up below).
- **Ringback.** While the phase is RINGING (caller only) `CallRinger` plays
  `ToneGenerator(STREAM_VOICE_CALL, 80)` `TONE_SUP_RINGTONE`, released by
  `ProjectLifecycle.onClose` and `DirectCallFeature.uninstall`. It follows the
  voice-call stream, so it is quiet through the earpiece on some phones.
- **Incoming calls.** While an invite is pending and no call is active,
  `CallRinger` plays the default ringtone with a looping `MediaPlayer`
  (`USAGE_NOTIFICATION_RINGTONE`) and a repeating vibration, following the
  ringer mode. In the background it also posts `IncomingCallNotifier`'s
  notification on "Incoming calls" (`IMPORTANCE_HIGH`, no channel sound because
  the app rings itself): `CATEGORY_CALL`, ongoing, public, a full-screen intent
  (`com.forkmesh.call.SHOW`), Accept (`com.forkmesh.call.ACCEPT` via
  `Features.onIntent`) and Decline (`CallActionReceiver`), both carrying the
  call id and the intent token (section 10). Accept from the notification
  answers at once only when the permissions are granted, else it opens the
  in-app screen. API 31+ uses `CallStyle.forIncomingCall` (plain actions if
  that fails); API 34+ uses the full-screen intent only when
  `canUseFullScreenIntent()`. The manifest declares `USE_FULL_SCREEN_INTENT`.
  Everything stops on accept, decline, a stale invite or an active call.
- **Lock screen.** `CallWindowEffect` sets `setShowWhenLocked` while an invite
  rings or a call is active and `setTurnScreenOn` only while ringing.
- **Incoming screen.** Full screen: caller, "Video call" or "Voice call", large
  red Decline and green Accept. It sits above the pre-call layer, which stays
  composed so its preview survives a decline.
- **Route note.** The page posts `route` (`direct` or `relay`) on connect and
  when the ICE pair changes (`WebCallPeers.route`). The call screen shows
  "Direct · peer-to-peer, no server in between" or "Relayed · through a TURN
  relay server (third party)", and the live line reads "Connected · end-to-end
  encrypted".
- **Call rings.** `DirectMessageService.sendCallFrame` marks every
  `call-invite` `ring: true`, and the relay sends the callee a `calls` account
  event naming the conversation (`CallRings.conversation`, `C/push/CallRing.kt`;
  it does not coalesce, so a redial rings at once). In the foreground
  `ChatDepth` calls `DirectMessageService.openRinging`, which opens the room
  socket so the next resent invite (every 3 s) shows the incoming screen. In
  the background `BackgroundLinkService.ringCall` wakes the active project's
  call engine (`DirectCallFeature.wake`) and falls back after 6 s
  (`CallRings.WAKE_WAIT_MS`) to `IncomingCallNotifier.postRing`; with no engine
  running (another project, or a background-only process) it posts at once.
  That notification uses "Incoming call alerts" (`incoming-call-rings`,
  ringtone and vibration, `IMPORTANCE_HIGH`), `FLAG_INSISTENT`,
  `setTimeoutAfter(45 s)`, a full-screen intent and Answer, opening
  `/chat/direct-messages/<id>` through `PushTarget`; the caller's name comes
  from `ApiService.directMessagePartner`. A ring needs the background
  connection (section 6; no FCM), so a force-stopped app does not ring.
- **Group calls** (S084, S140; protocol in
  [chat-group-calls.md](../app/chat-group-calls.md)): section 27, "Group calls
  on FM Media".

### Chat history file (S024)

- The file is `chat-history/relay-<scope>.json` in the project's support folder,
  scope `ChatHistoryCodec.projectScope(projectId, room)` (`"project-" +
  scopeName(projectId, room)`, falling back to the relay URL), as iOS, so a
  relay URL change keeps history. On first read the store adopts files under
  `formerScopes(serverUrl, room)` (old URL hashes, the retired `rooms/general`
  path, production aliases), merges them newest first, writes the project file
  and renames each old one `<name>.migrated`. An unparsable file is renamed
  `<name>.unreadable-<ms>`; one that exists but cannot be read blocks every
  save to that path until a read succeeds, so it is never overwritten. Any
  `v >= 1` loads.
- It holds relay rooms, DMs (`direct:<id>`, with `receipts` and `receiptTimes`)
  and Office `#office/<name>` channels; restored Office messages are replaced
  when the live copy arrives. Android leaves out iOS keys it has no feature for
  (`removed`, `looks`, `lookEvents`, the reply `r` field); both apps ignore
  unknown keys.
- Across a sign-out `ChatHub.directSessionChanged` holds the DMs and receipts
  before `DirectMessageService.clear()`, and the next sign-in restores them only
  for the same account (trimmed, lower-cased).

### Conversation screens (S011, S022, S025 to S027)

- **Which rooms are bubbles.** `ChatHub.isConversation`: server DMs and
  private office rooms that are neither server-visible nor read-only draw as
  bubbles (`ChatConversationList.kt`, `ChatBubbles.kt`); public channels,
  office channels and `#mesh` keep Slack-style rows.
- **Layout.** `ChatConversationLayout` (core) ports iOS: a bold time line after
  a 15-minute gap or a new day ("Today 3:42 PM", "Yesterday …", weekday, "Oct 1
  at …", then with year); runs of one sender joined at 5 dp corners and 2 dp
  apart (8 dp before a run); the sender's name at the start of a run in groups
  and the avatar beside the last bubble; own messages always carry a receipt;
  up to three emoji alone drawn at 52 sp without a bubble. System lines break
  runs. Emoji detection uses `BreakIterator` clusters and an Emoji_Presentation
  table, because `Character.isEmojiPresentation` needs Android 15 and core runs
  on the JVM.
- **Bubble.** `ChatBubbleShape` is iOS's path (18 dp corners, 5 dp inset on the
  sender's side, cubic corners at 0.45, mirrored for own), no tails. Sent
  0A84FF with white text, received E9E9EB (light) or 26262A (dark). Over a
  backdrop picture a bubble is `fmGlass` tinted at 88% (own) or 50%; on the
  solid canvas it is iOS's pre-26 sheen so colours stay readable. The
  transcript is pure white or black.
- **Inside the bubble:** the 12-hour time, an 11 dp `ChatExpiryRing` (a hand and
  a wedge for the retention left, refreshed each minute, read as "Disappears in
  5 hours") and the receipt, with a 4 dp dot while an own DM is still stored on
  the server. The meta sits on the last text line when it fits; photos carry it
  in a dark capsule; large emoji and view-once put it under the bubble.
- **Gestures.** Long press opens the actions sheet; a double tap (or tapping
  the reaction badge) opens the tapback picker over a dimmed page with who
  reacted. Reactions are a capsule badge on the outer top corner. Typing is a
  received bubble with three pulsing dots.
- **Message Info (S022).** A mostly-horizontal leftward drag (1.5x more sideways
  than vertical) pulls the page in from the right while the transcript eases
  30% left; it opens past 30% of the width or on a fling; Back or a rightward
  swipe closes it. Content from `ChatMessageInfo.page`, matching iOS
  `MessagesMessageInfo.swift` ("Today at 11:04 PM", seconds underneath). Under
  the latest own DM message a line reads "Delivered 11:04 PM" / "Read Yesterday
  3:20 PM" (hidden while sending or only sent); tapping it opens Message Info.
- **A notification tap reveals the conversation (S025).** Opening from a
  notification or route goes through `ChatHub.switchConversation`, which counts
  `revealRequests`; `ChatScreen` then closes the channel drawer, Message Info
  and its sheets and dialogs (cancelling their coroutines, since `FmMessenger`
  has no dismiss).
- **Message from People (S027).** People's Message opens `Route.Dm(name)`,
  "@name" in chat. `ChatHub.switchConversation` treats "@name" as an account (a
  roster peer gives its verified owner): it selects that account's DM or starts
  one with `POST /api/chat/direct-messages`. A guest or a signed-out phone
  opens nothing and shows a notice. `Route.Dm` also accepts `direct:<id>`.

### Live touch in direct messages (S023)

- **Wire format.** `DirectLiveEngine` and `DirectLiveFrames` (core
  `DirectLive.kt`) port iOS: sealed `type: "live"` frames with `taps` (flat
  `[x, y]` in thousandths, `y` from the middle in widths), `ink` chunks (`s`,
  `p`, `c`, `w` in tenths, `e`), `motion` samples (`[dt, gx, gy, gz, ax, ay,
  az]` in hundredths of g against `ts`), `clear` and `motionOff`. Scaled
  numbers round half away from zero like Swift's `rounded()`, so both phones
  send the same integers. `DirectMessageService.sendLive` sends over the mesh
  first, then the relay with `"live": true` and no `persist` or `notify`,
  dropping envelopes over 16 KiB; the relay's `forkmesh-live` budget sets the
  pace (`windowMs * 4 / (perWindow * 3)`, at least 80 ms, 120 ms until known).
  Live frames never become messages and get no mesh ack. Taps are pruned after
  1.4 s, ink after 45 s (fading over the last 5 s).
- **Tilt.** `DirectLiveMotionFeed` samples at 100 Hz from `TYPE_GRAVITY` and
  `TYPE_LINEAR_ACCELERATION` (the two vectors the pose needs), falling back to
  the raw accelerometer with iOS's 0.1 low-pass, divided by -9.81 so an upright
  phone sends `gy = -1` like an iPhone; `stop` always unregisters, so a
  half-failed start cannot leak its listener. Sharing runs while a DM is on screen
  and the app is in front, with no setting (as iOS
  `MessagesConversationView.startSharing`); an impact of 1.4 g sends a centred
  tap (at most one per 500 ms).
- **Buzz.** A received tap always vibrates (a thud, then a 0.55 click 90 ms
  later), even with haptics off, because the tap is the message; sending gives
  a light tick; received ink ticks only with haptics on.
- **Transcript and drawing.** A short tap on the transcript (not a bubble)
  sends a tap there; with the keyboard up the first tap only closes it. Ripples
  (white yours, pink theirs) draw behind bubbles at 30 fps. The pencil opens
  the canvas (colour grid, Clear, Done, "Tap to buzz. Draw on the
  background."); a drag past 8 dp draws a 5 dp stroke.
- **Partner phone.** In a server DM the header shows the partner's 3D phone
  (`DirectLivePhonePose`, 76 dp hanging out of the 50 dp slot, drawn outside the
  glass) with their avatar on its screen and their recent bumps; grey and
  upright when they are not sharing. It swallows touches; TalkBack gets a "Send
  a tap" action.
- **Presence.** `ChatHub.peerPresence` (core `ChatPresence.kt`) finds a server
  DM's person on the roster by the trusted devices' nodes, then by account
  owner, then by name; typing, a tap, shared motion or a "Direct only" route
  also count as online. The header shows a blinking green light (steady with
  reduced motion); the DM list's dot uses the same rule.

## 15. You hub, diagnostics and docs

- **The hub drops down from the avatar** (`YouHubPanel`,
  `A/feature/navigation/NavigationScreens.kt`, as iOS `HomeShell.hubDropdown`,
  S013): a 320 dp card (at most 640 dp tall), 54 dp below the status bar and
  12 dp from the right, 18 dp corners, over a 20% black scrim that closes it,
  unrolling with the iOS spring (damping 0.86, about 0.36 s). Inside: avatar,
  name and status, a "Set a status" chip, Pocket mode, Suggestion mode, the
  Diagnostics switch, the account chips (Settings, Sign in or Sign out; other
  links are on Home and the bar) and the version capsule.
- The version capsule shows `versionName (versionCode)`, opens `WhatsNewPage`
  from the bundled `changelog.json` ([MAC_BUILD.md](MAC_BUILD.md)), and has
  Send diagnostics (the prompt points the agent at
  [PERFORMANCE_CHECKS.md](PERFORMANCE_CHECKS.md); `tools/PerformanceDiagnostics.kt`,
  `tools/AppChangelog.kt`, `feature/tools/AboutAndDiagnostics.kt`).
- **Performance report** (S043). `DiagnosticsBundle.report` adds Stalls by
  name, Stalls by screen and Slow operations by name ahead of the slowest
  events (count, total, max, and the newest stall's time), so slow network
  calls do not push stalls out. `PerformanceMonitorService.screenName` comes
  from `NavigationFeature.screenTitle()` (tab, pushed screens and full-screen
  page joined with " > "), read on any thread because frame stalls arrive on
  the frame-metrics thread; stalls without one group under "not recorded".
- **Docs package**: the public docs library from the bundle iOS ships
  (`native/ios/ForkMesh/Resources/DocsLibrary.json`,
  `forkmesh-docs-library-v1`), copied to `docs/library.json` by the
  `bundleDocsLibrary` Gradle task, so Android always ships iOS's library.
  `C/docs/` parses, searches (desktop scores) and resolves links;
  `DocsMarkdown` renders a safe Markdown subset; `A/feature/docs/` holds the
  screens. Guests may open it. Tests: `DocsLibraryTest`, `NavigationTest`,
  `PackageGateTest`.
- **Logs and Diagnostics.** `Route.Logs` and `Route.Diagnostics` (packages
  `logs` and `diagnostics`, Operate after Network) open `TelemetryScreen` on
  the Logs or Stats tab; no link is parsed for them, as on iOS.

## 16. iOS look: glass and the floating tab bar

The phone shell follows the iOS app ([NAVIGATION.md](NAVIGATION.md), iOS
`Shell/ShellBottomBar.swift`, `Kit/FmSurfaces.swift`) instead of a Material
bottom bar.

- **Glass** (`A/ui/kit/FmGlass.kt`). `FmBackdropHost` records the page into a
  `GraphicsLayer`; `Modifier.fmGlass(shape)` on a sibling draws that layer
  shifted to its own position, blurred and saturated 1.8x (`RenderEffect`,
  API 31+), then the tint, a top specular gradient, an edge and a shadow
  (Liquid Glass details: section 23). Below API 31, or with high-contrast text,
  glass is a near-opaque fill (the iOS Reduce Transparency fallback).
  `Modifier.blur` is not used: it blurs the element, not what is behind it.
- Glass inside a recorded page does not sample its own page (it would draw a
  layer into itself): `FmBackdropHost` clears `LocalFmBackdrop` for its
  content, so such glass uses the solid fill. Only the host's overlay blurs.
- **Bar look** (`A/shell/ShellBottomBar.kt`). Filled icons and the Lens
  selection by default: a colourless `ShellBarLens` slides behind the cells,
  `barSheen` and `barBevel` replace an outline, `ShellBar.lensMargin` insets it.
- **Tab bar** (`GlassTabBar`, `A/shell/HomeShell.kt`). One glass capsule: 12 dp
  side margin, 60 dp tall, radius 30, 52 dp cells, 24 dp icons, 10 sp labels.
  The selected cell is an accent wash at 14% (radius 26 at the ends, 10
  inside); selected icon and label accent, unselected icons textPrimary and
  labels textSecondary. Pressed cells scale to 0.94 at 70% opacity. The bar
  sits `max(navigation bar inset, 34 dp) + 5 dp` above the bottom edge.
- **Home tab** comes first and replaces the old More sheet. Home is shell
  state, not an `AppTab`, so routes and stored tabs are unchanged; back at any
  tab root returns to Home, and the app opens on Home. The Home glyph is the
  filled `home` because the subset has no outline house.
- **Page clearance.** Pages are laid out full height under the bar. A screen
  gets bottom padding `ShellBar.clearance()` unless it sets
  `Screen.extendsUnderTabBar`; then it reads `LocalShellClearance` and pads its
  own list so rows scroll under the glass (Repos and Inbox).
- The kit gallery (`--ez fm_kit_gallery true --ei fm_kit_section 10`) shows a
  glass capsule over colour stripes to check the blur.
- **Top bar** (`TopBar`, as iOS `Shell/ShellTopBar.swift`): no fill, 16 dp side
  and 4 dp vertical padding; the 40 dp project icon (opens the project drawer;
  a long press closes hub and palette and selects Home), the chips (Glasses
  Alert, `StepsWalkChip`, `ShellMailChip`), the new-agent button and the
  avatar (38 dp, corners 28%, as `AvatarWithDot(rounded:)`: picture or initial,
  a 2 dp status ring touching it with no gap, the status emoji bottom-left;
  S014). A guest
  sees a person icon and a "Sign in" chip. Search is on Home, not the bar. The
  telemetry strip: 12 x 10 dp sparklines coloured by `TelemetryMetric.health`.
- **New-agent button** (`PromptButton`): a 44 dp accent gradient circle with a
  white plus and a glow that grows with running agents; each running agent
  adds a small static four-point star. Hold it for the voice prompt (section 30).
- **Pushed-page header** (`FmNavBar`), the iOS 26 inline bar: page-coloured,
  52 dp, centred `titleMedium` title, a 44 dp glass Back circle, actions on the
  right. Actions get the bar width less 52 dp (at least 184 dp,
  `FmNavActionsRoom`) in a scroller that starts at the trailing end, so many
  actions scroll instead of covering Back (S074). `ToolsAppBar`, `CodeAppBar`
  and `RoutePage` draw it.
- **Segment and action bars.** `FmSegmentTabBar` is a 46 dp strip with tabs in
  a 12 dp glass panel; `FmBottomActionBar` is a glass panel (24 dp corners, 8 dp
  side and 4 dp vertical margin).
- **Composer.** The prompt composer card and slash menu use `fmGlass` with the
  composer's tint (iOS `Kit/PromptComposer.swift`, 28 dp).
- **Chat header** (iOS `Features/Chat/ChatScreen.swift`): on the Chat tab the
  shell bar is hidden and the room draws one glass row: a 48 dp channels circle,
  the room pill (icon, bold name, "n online • public"), a compose circle (opens
  the channels panel), the new-agent button and an avatar capsule. The
  disappearing-messages timer and the encryption note live in the room pill's
  menu.
- **Sheets** keep the iOS pre-26 look (sheetSurface, 12 dp corners, 35% and 55%
  scrim); a blurred sheet would need `Window.setBackgroundBlurRadius`.

## 17. iOS controls (sync ledger S005, S008, S009)

The kit draws the iOS controls instead of Material's ripple, switch and icon
button:

- **Press feedback.** `ForkMeshTheme` provides `FmPressIndication`
  (`ui/kit/FmPress.kt`) as `LocalIndication` and turns the ripple off
  (`LocalRippleConfiguration provides null`): a flat `textPrimary` wash at 8%.
  List rows (`FmListRow`) use `accentSubtle`.
- **Switch.** `FmSwitch`: a 51 x 31 dp capsule, accent track when on and
  `#787880` at 16% light / 32% dark when off, a white 27 dp thumb that springs
  across, and the selection haptic. Material took "on" from `primary`, which is
  `textPrimary` here (black in light mode), so every switch goes through
  `FmSwitch`.
- **Checkbox.** `FmCheckbox` is accent-filled with an `onAccent` tick.
- **Icon buttons.** `FmIconButton`: 48 dp target, 24 dp glyph in `textPrimary`,
  a 40 dp circular wash at 8% while pressed, 38% opacity disabled.
- **Kit details (S008).** Filled accent and dialog confirm buttons have white
  labels in both themes (`onAccent` was near-black in dark mode); confirm
  dialogs have 14 dp corners; `FmBottomBar` is a glass capsule (50 dp, 8 dp side
  and 4 dp vertical margin).
- **Tap a message to open it in the composer (S008).** As on iOS, messages
  carry no copy or send icons: `Modifier.fmMessageTools(text)`
  (`ui/kit/FmMessageTools.kt`) makes the message a tap target that opens the
  composer with `C/navigation/MessageHandoff.agentPrompt` ("The ForkMesh
  Android app showed this message: …"). `FmEmptyState`, `FmInlineStatus`,
  `FmConnectionBanner` and `FmConfirmDialog` use it; `NavigationFeature.install`
  sets the hook (`FmMessageHandoff.compose`) so the kit does not depend on
  feature code.
  `FmMessageTools` takes an optional `handoff` (section 27, S144).
- **Notices close themselves.** A dismissible `FmInlineStatusLine` closes
  after 6 seconds (10 for an error, `FmNoticeTiming`); `FmCountdownClose`
  draws a ring around the X that empties over that time and restarts when the
  message changes. Tapping the notice opens the composer and dismisses it.
- **Haptics.** `FmHaptics` has `send`, `palette`, `success`, `warning` and
  `error`; success and error use the platform `Confirm` and `Reject` patterns.
- **Menus.** `FmDropdownMenu` is the iOS 26 pull-down: an opaque panel (22 dp
  corners, `FmMenu.surface`: `#2C2C2E` dark, white light, a 0.5 dp hairline), at
  least 250 dp wide, 44 dp rows with a leading outlined icon and 17 sp text. The
  surface must stay opaque: the shadow draws under the whole shape and the
  popup window clips it, so a translucent fill shows a grey rectangle.
- **Action popover.** `FmActionMenu` and `FmActionMenuButton` are the iOS 26
  confirmation popover (a 32 dp panel of stacked 48 dp capsule buttons, accent
  text, red for destructive). Home's touch-and-hold menu uses it at the press
  point ("Show all update dots", "Clear all update dots", "Home settings…",
  "Bottom bar settings…", "Select area to copy…", "Refresh everything").

## 18. Home grid (sync ledger S010)

Home draws the iOS outline icons (`ShellHomeTile`, after iOS
`Shell/ShellHomeIcon.swift`): 5 across, 66 dp tiles, a glyph in a 39 dp box
(`iconSize * 1.75`) with the iOS vertical gradient (`textPrimary` to 82% light
/ 74% dark, accent when selected or running), a 12 sp label, the count pill
(`ShellHomeCountMark`, `FmPalette.count`) and the desktop-offline dot.

`ShellHomeGrid` (`A/shell/ShellHome.kt`) lays Home out with
`C/shell/ShellHomeStyle.arranged` and the iOS key `shell/home-style`: icon
style (outline, filled, accent, tiles), 4/5/6 across, eight sorts, grouping,
category titles, order and hidden categories, the frequent-use glow
(`ShellBarOrder.heat` over recorded taps) and counts. Rows shrink to fit
(`ShellBarOrder.fittedTile`, at least 50 dp). Touch and hold the background or
an icon for the settings sheets; hold a category title to hide it.
`ShellHomeSettingsForm` is the iOS form; "Hide icons in the bottom bar" is the
Android setting `home/hideBarIcons`. Search, custom order and drag: section 30.

**Count pill and loading (S065, S105).** The pill hangs from the glyph's
top-right corner and rises at most 2 dp above the icon box
(`ShellHomeMetrics.anchorRise`), drawing over the 6 dp update dot. While a count
is read a small spinner (`ShellHomeCountSpinner`, TalkBack "Loading") sits in
its place (`ShellBadges.pending`); an unread badge still wins. Home has no
loading bar. The current repository's counts are saved per repository
(`shell/repoCounts/<owner>/<repo>`, `ShellRepoCounts.encode`), shown at once
and read again once a session. **Refresh everything**
(`ShellBadges.refreshEverything`) re-reads tasks, bug reports, repository
counts and pulls, organization and account packages, status checks, the inbox
and RSS feeds.

## 19. Outline icons (sync ledger S002)

SF Symbols may not ship in an Android app (Apple's licence), so Android uses
Material Icons **Outlined**: `FmIcon(name, outlined = true)` picks
`<name>_outlined` when the font has it, else the regular glyph. The tab bar
uses outlined glyphs idle and filled when selected; Home tiles and the hub are
outlined. Home glyphs are drawn at 26 dp in their 22 dp slot because Material
glyphs carry 2 dp of padding inside the em box.

`tools/subset_material_icons.py` keeps the `_outlined` twin of every icon it
keeps (the font is about 194 KB; the iOS copy is the same file). Pass `--font`
with any full `MaterialIcons-Regular.otf`; the Flutter SDK's
`bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf` has all 8,825
codepoints of `tools/material_icons/MaterialIcons.codepoints` with identical
glyphs. Git history is not a source: a shallow or rewritten clone lacks it.

## 20. Bottom bar look and settings (sync ledger S012)

The tab bar is drawn from the iOS bar style (`ShellStyledBar`, `ShellBarRows`,
after iOS `ShellBottomBar.swift` and `ShellBarLook.swift`), stored under the
iOS key `shell/bar-style` through `C/shell/ShellBarPrefs`:

- Shape (capsule, panel, squares, circles, icons only), fill (glass, clear,
  tinted, solid), corner and spacing sliders, full width or fit, and Single or
  **Double** height (two rows in one panel, `ShellBarStyle.rowSplit`; slots keep
  the iOS 64 dp cap).
- Icon style, size S to XL, labels (always, selected, never), the selected mark
  (fill, outline, glow, dot, line, none), counts, activity, badges, highlight
  frequently used, and Reset bar look.
- "Bottom bar settings…" opens `ShellBarSettingsSheet`: a live preview, chips,
  segmented pickers, sliders (`FmIosSlider`), a stepper and switches.
- `ShellBar.clearance()` follows the bar's measured height.

**Choosing what sits in the bar** follows iOS `ShellBarCustomize.swift`. Every
Home item is a candidate (`rememberShellBarCandidates`): the tabs (ids `home`,
the `AppTab.raw` names, `repos`, `code`), Home entries, the current
repository's sections and every hub item. The saved layout (`shell/bar-layout`:
hidden, folders, order and the bar list) picks the slots. Holding a bar icon
offers Remove from bar, Move left / right / to the front / to the end, Move to
folder… (or New folder…), Take out of the folder, Reset bar layout and Bottom
bar settings…; holding a Home icon adds "Add to bar" or "Add to bar in place
of…". A folder shows a 2×2 grid of its first icons with the summed badge, opens
as a popover, and offers Open, Move, Rename folder…, New folder…, Remove folder
from bar and Remove folder (keeps its items). With "Hide icons in the bottom
bar" on, Home hides exactly what the bar holds.

## 21. Update dots on Home (sync ledger S018)

`C/market/AccountPackages.kt` remembers each plugin's latest Market version
and the version last opened, under the iOS key `packages/versions`
(`{"seen": {...}, "latest": {...}}`). A plugin whose latest differs from the
seen one (including never opened) shows the accent dot at the glyph's bottom
right (`ShellHomeUpdateDot`); opening it from Home marks it viewed. Hub items
map to plugins through `PackageGate.HUB_ITEMS` and `ROUTE_KINDS`. "Clear update
dots" is `markAllViewed`, "Show all update dots" `markAllUnviewed`. Latest
versions come from every catalog the app reads (the Market, and the refresh on
each return to the foreground).

## 22. Shell badges, repository icons and slow-load bar (sync ledger S017, S019, S020)

**Badges (S017).** The current repository's sections (`ShellRepoGroup`, id
`repo`, iOS tile ids `repo-<section>`: Pull requests, Issues, Commits,
Branches, Releases, Actions, Discussions, each gated by
`PackageSurfaces.showsRepoSection`) form a Home category and can sit in the
bar. A pull counts as open unless closed or merged (`PublishedPull.isOpen`).
Where one pill is drawn, new pull requests (accent) win over the open count.

- `ShellPullsWatch` (`A/shell/ShellPullsWatch.kt`) is the one reader of the
  current repository's pull list: on a repository change, return to the
  foreground, account socket reconnect and the relay `pulls` frame
  (`ShellBadges.install` chains `AccountEventChannel.onTopic`). It does not
  poll ([polling-elimination.md](../operations/polling-elimination.md));
  core keeps `NewPullsMarker.pollEvery` only for the iOS tests. A read that
  lands while one runs queues one more. The Pull requests page takes the
  watch's list, supports pull to refresh, keeps its list when a refresh fails
  and marks pulls seen while open.
- `NewPullsMarker` (`C/shell/NewPulls.kt`) keeps the highest pull number seen
  per repository under `shell/pulls-seen` (40 repositories; first sight records
  a baseline without a badge), stored as the iOS lines (`<number> <slug>`)
  joined by newlines because `PrefStore` has no string list.
- `ShellBadges` (`A/shell/ShellBadges.kt`) holds the other counts: Tasks shows
  open tasks assigned to you (`TaskBoard.mineOpen`), refreshed with Home, on
  every `pings` frame and when Tasks loads; RSS Reader shows the Unread count
  (`RssLibrary.counts().unread`). Both ride `hubBadges` as `ShellMenuBadge`.
- The floating bug button shows administrators the number of `new` reports
  (`bugReportInbox(status = "new", limit = 1)`), updated when the Report a bug
  inbox loads or a status changes.

**Repository icons follow the composer (S019).** `HomeShell` watches the
composer's repository (`QuickSendState.repository`, `AgentStartTarget.ref`) and
calls `CodeFeature.follow`, which moves `CodeFeature.currentRepo` and
`code/current-repo`. Opening a repository in Code moves the composer back
(`ComposerFeature.FollowRepository`).

**Slow-load bar and Agents first load (S020).** `ShellLoadingOverlay`
(`A/shell/ShellLoadingBar.kt`) is iOS `fmLoading`: 2 dp, accent at 75%, a 30%
segment sweeping in 1.2 s, shown only after 250 ms, a steady 40% bar under
reduced motion. It shows on the Agents list, the repository overview and the
RSS timeline. `AgentListRings` (`C/agents/AgentListRings.kt`) is the iOS ring
rule: a ring never restarts a load in flight, one that lands before the
per-repository reads go out is answered by them, a later one queues one more
load. (Keying a `LaunchedEffect` on the ring signal cancelled the first load
and read every repository twice.)

## 23. Liquid Glass renderer (sync ledger S003)

`fmGlass` (`A/ui/kit/FmGlass.kt`) follows iOS 26 Liquid Glass:

- **Refraction.** On Android 13+ the sampled backdrop goes through an AGSL lens
  (`FmGlass.LENS`, a `RuntimeShader` after the blur): within 18 dp of the edge,
  samples are pulled inward along the edge normal by up to 9 dp, reading the
  corner radius from the shape's outline. Android 12 keeps the plain blur;
  below 12, inside a recorded page, and with high-contrast text the solid fill
  stays.
- **Clear glass.** An 18 dp blur and a light tint (`bgRaised` at 30% dark, 34%
  light) with a soft top highlight.
- **Specular rim.** A 1 dp gradient stroke, bright at the top (white 85% light,
  28% dark) fading toward the bottom.
- **Press response** (`fmGlass(interactive = true)`, iOS
  `.glassEffect(.regular.interactive())`): while a finger is down the glass
  swells to 108 % on a soft spring and a white radial light follows the finger
  (16 % dark, 30 % light), fading over 260 ms. It watches the initial pointer
  pass without consuming, so the click still runs. On for glass circles (Back,
  icon buttons, chat header circles and pills, the Market bar, the walking
  chip, live touch); panels and the composer stay still. iOS
  `GlassEffectContainer` morphing has no Compose equivalent.
- **What floats over content.** The bottom bar, the chat header and composer
  (section 30) and, with a background picture, the wallpaper refract what is
  behind them. Pushed pages keep an opaque `bgBase` bar with glass buttons and
  the scroll-edge fade, which is how the iOS 26 bar looks over a plain
  background; action bars at the foot of pushed pages use the solid fallback.

`half` is a type in AGSL; a variable named `half` makes the shader fail to
compile at run time, and `FmGlass.lens` then falls back to the plain blur.

## 24. Navigation motion (sync ledger S006, S009)

- **Push and pop** (`TabStack`, `A/shell/HomeShell.kt`, and `NavigatorHost`,
  `A/ui/navigation/Navigator.kt`) move like `UINavigationController`: the new
  page slides in full width from the right while the page under it slides 30%
  left, 350 ms on `FmMotion.pushEasing` (cubic-bezier 0.2, 0.8, 0.2, 1); a pop
  reverses it with the leaving page on top.
- **Interactive back.** `TabStack` drives its transition from a
  `SeekableTransitionState`, and a `PredictiveBackHandler` scrubs the pop with
  the system back gesture; letting go finishes (`Navigator.handleBack`),
  cancelling springs back.
- **Tab switches** do not animate (each tab's stack is keyed on its navigator).
- Reduce motion snaps instead of sliding and turns the interactive back off.

## 25. Area parity with iOS (ledger rows)

Most area rows now live with their area (sections 13 and 14). These remain:

- **Inbox bar (S030).** `A/feature/inbox/InboxBar.kt` is iOS `InboxFilterBar`
  and `InboxToolbarMenu`: one centred row of 28 dp glass squares (8 dp corners,
  4 dp apart, 16 dp icons): the six filters, Mark all read, Unread only, a pause
  menu (bedtime, or a warning-coloured bell-off while paused), Discard changes
  that could not sync (only when a change ran out of attempts), Ping settings
  and Delete all pings. The selected filter and an active Unread only take the
  accent. Counts are a 14 dp badge offset 5 dp up and right, always blue
  (`fm.count`), "99+" above 99. Holding a square shows its name; a narrow phone
  scrolls the row. The squares draw glass without the 12 dp shadow, which
  swamps a 28 dp shape.
- **Inbox instance logo (S122).** `SwipeRow` (`A/feature/inbox/InboxScreen.kt`)
  stacks the project's 16 dp `ProjectIcon` under the icon of every non-local
  ping and adds "From <project>" to its TalkBack label.
- **New project naming and logo (S039, S068).** `ProjectProbe.verify` labels a
  candidate with no typed name with `/api/instance`'s `name`, and the New
  project dialog fills an empty Name with it after Check instance; a typed name
  wins (relays are projects, so there is no separate Add relay form). Once the
  check passes, `AddProjectDialog` fetches the instance's logo as the project
  image (spinner in the logo box); candidates are
  `InstanceDirectory.logoCandidates` (core `account/projects`): `/api/instance`'s
  `logo`, the directory's logo, then the two favicons (none for a branded
  instance). A picked logo is never replaced; an instance logo goes when the
  host is edited. "Instances on ForkMesh" lists ForkMesh plus every `listed`
  https entry of `GET https://app.forkmesh.com/api/start/instances`
  (directory logos only in the `/api/start/instances/<32 hex>/logo?v=<n>`
  shape); Add checks, loads the logo and creates the project in one tap, and an
  existing one shows "Added". An unreadable directory lists only ForkMesh. Test:
  `InstanceDirectoryTest`.
- **Match the instance's theme (S110).** Settings > Appearance switch
  `appearance/matchInstanceTheme`. While on, `HomeShell` asks
  `AppearanceStore.refreshInstanceTheme` for the project's `GET /api/instance`
  at launch, on return and on a project switch; `InstanceTheme` (`C/settings`)
  maps `themeColors` (or an older `theme.dark`/`theme.light`) onto the 13 tokens,
  kept in `appearance/instanceThemeColors` so the next launch starts in it.
  `MainActivity` paints `Appearance.effectiveTheme`; the readable switch still
  applies. No instance or the default look clears the colours, a failed read
  keeps them, and switching off drops them.

## 26. Typeface (sync ledger S001)

SF Pro may not ship in an Android app (Apple's licence), so Android uses
**Inter** 4.1 (SIL Open Font License; `res/font/inter_variable.ttf`, licence in
`assets/fonts/Inter-LICENSE.txt`), the variable font from `rsms/inter` v4.1
(`Inter-4.1.zip`, SHA-256
`9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`). Its
optical-size axis stands in for SF Text and Display: `FmType.ui` sets `opsz` 14
and `FmType.uiDisplay` `opsz` 32, and `fmTypography` picks the display cut at
20 sp and above (`FmType.family`). Sizes, weights, leading and tracking are the
iOS values. Text that builds its own `TextStyle(...)` must name
`fontFamily = FmType.ui`; Compose does not merge a fresh `TextStyle` with the
theme, so it would fall back to Roboto.

Monospace is **JetBrains Mono** 2.304 (SIL Open Font License;
`res/font/jetbrains_mono_variable.ttf`, licence in
`assets/fonts/JetBrainsMono-LICENSE.txt`) in place of Menlo, from
`JetBrains/JetBrainsMono` v2.304 (`JetBrainsMono-2.304.zip`, SHA-256
`6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf`).
`FmType.mono` is the only monospace family; code names it instead of
`FontFamily.Monospace`.

## 27. Feature parity, second round (ledger rows)

- **Diff toolbar squares (S044).** `StreamToolbar`
  (`A/feature/code/DiffReviewStream.kt`) has no raised bar: every button is a
  30 dp `fmGlass` square (8 dp corners, 4 dp apart, 17 dp icon, accent on
  accent-subtle glass when on), and the seen/total file button widens for its
  count. Holding a square shows its name (`FmTooltip`). Android keeps extra
  buttons (previous and next change, next file, text size) because it has no
  pinch zoom; the find bar keeps small flat buttons, as iOS keeps `tiny`.
- **Other apps' calls (S038).** See [OTHER_APP_CALLS.md](OTHER_APP_CALLS.md#android):
  `OtherAppCall` (app `platform/`, pure state in
  `core/.../net/OtherAppCallState.kt`) quietens interface sounds, Agent Watch
  and the Glasses Alert ring during another app's call; since S130 ForkMesh's
  own calls count too.

### Calls (ledger S125 to S133)

Android calls run in the `direct-call.html` WebView, which already carries the
web resilience code; the native side turns it on and draws the iOS chrome. The
earlier call pieces are in section 14, "Direct calls".

- **Reconnect and give up (S125).** `WebCallPeers` calls `setup` with
  `{resilient: true, staged: true}`; the page posts `connection` (connected,
  disconnected, failed, stalled after 4 s without inbound audio) instead of
  `closed`, and `restart` returns an ICE-restart offer. `DirectCallEngine`
  follows iOS `DirectCalls.swift`: the callee resends its accept every 2 s (8
  times) until an offer arrives; the caller resends an unanswered offer every
  3 s (5 times) and on a repeated accept; a repeated offer gets the stored
  answer and a new one renegotiates; ICE is held during a restart; the caller
  restarts after 2 s on a drop (at once on failed or silent audio) and every
  6 s after; the status reads "Reconnecting…"; the call ends with `DROPPED`
  after 30 s without recovery or `CONNECT_FAILED` when it never connects in
  30 s, posted as a chat notice. The page re-acquires an ended microphone, else
  `micLost` shows iOS's notice. Test: `DirectCallResilienceTest` (manual
  clock).
- **Mini window (S126).** Back calls `DirectCallFeature.minimize()`;
  `DirectCallMiniWindow` is an in-app floating tile (116x164 dp video,
  156x96 dp voice, 12 dp margin, 14 dp corners) that hosts the call WebView so
  the page keeps playing (`layout("mini")` hides the self view): their video or
  initial, name, timer or "Reconnecting…", Mute and Hang up. A drag snaps to
  the nearer side at the released height, kept in `fm.call.miniSide` /
  `fm.call.miniY` (`CallMiniLayout`). Picture-in-picture would move the whole
  activity out of the app, while iOS keeps chat usable under the tile.
- **Staged video (S127, S132).** The page fills the screen with your camera
  (`body.staged.waiting`) until it decodes the other side's frames (first
  stats tick, 1 s) and hides their video again when the track mutes or stalls,
  posting `remote-video` (`WebCallPeers.remoteVideo`). The call screen shows
  avatar and name until then, and the self view only once their video shows.
  Video calls use the loudspeaker without a headset (`CallSpeaker`:
  `setCommunicationDevice` on API 31+, `isSpeakerphoneOn` below).
- **Diagnose (S128).** `C/chat/CallDiagnostics.kt` ports iOS
  `CallDiagnostics.swift`, checked against the shared
  `call-diagnostics-parity.json` (kept equal by
  `app/tests/test_call_diagnostics_native_parity.py`); numbers use JS `toFixed`
  rounding. The dot beside the quality pill turns red on a problem; its sheet
  lists issues, ranked fixes, a copyable prompt and a LESSON field, saved under
  `fm.call.diagnostics`. A pending fix counts as worked when its problem clears
  and as failed when the call ends (`DirectCallAudio`), not on minimizing.
- **Audio (S130).** `DirectCallAudio` holds audio focus (`AUDIOFOCUS_GAIN`,
  voice-communication attributes) while a call has a phase. When focus returns
  or the app comes forward it calls the page's `wake` (replays media,
  re-acquires a dead microphone) and re-applies the speaker route. Our own call
  counts in `OtherAppCall` (`OtherAppCallState.ownCall`, group calls included),
  so interface sounds, Agent Watch and alarm rings step back
  ([OTHER_APP_CALLS.md](OTHER_APP_CALLS.md#android), S038). Dictation does not
  start during a call (section 11).
- **Answering (S131).** The accept permission prompt lives in
  `DirectCallOverlay`, so its result survives the invite screen closing;
  `holdInvite()` keeps the invite fresh while it is open.
  `DirectCallFeature.answer` declines with iOS's message when permission is
  refused, and says "The call ended before it could be answered." or "Hang up
  the current call to answer this one."; the notification's Answer says the
  same.
- **Microphone test (S129, S133)** is the pre-call test clip (section 14),
  played with media audio attributes on the loudspeaker; media audio is not
  muted by silent mode.

### Diagnostics lights by the clock and several desktops (ledger S113, S115, S119, S141)

- **Where it sits.** Android has no Dynamic Island, so `ShellIsland.layout`
  (`core/.../shell/ShellIsland.kt`) maps the iOS ears onto the status bar and
  the top display cutout (`View.rootWindowInsets.displayCutout`, API 28+; a
  cutout counts only when near the middle and above the bottom of the status
  bar). The ears are the status bar beside the cutout (126 dp in the middle
  when there is none). When the room above a 16 dp clock line is at least
  13 dp (most punch-hole phones), the lights sit there 24 dp from the left
  corner and the app charts in the right ear, and the top bar drops its chart
  strip (`rememberShellIslandEars`); the status dots sit above a punch hole or
  under the status bar for a notch. On a short status bar lights and dots share
  one 12 dp row under it and the charts stay in the top bar.
  `ShellIslandStatus` (`A/shell/ShellIslandStatus.kt`) shows only while
  Diagnostics is on, takes no touches and is offset by its window position so
  the rail layout lines up. (It replaced S093's always-on `TopStrip`.)
- **What it shows.** The service dots (`StatusDotRow.services`, without
  `core:`/`runner:` systems), then the desktop light, other desktops stacked
  4 dp over each other with presence dots, the core nodes stacked the same way
  and the runners (`StatusDotRow.fleet`, `FleetLight.lights`, following the
  installed status checks and "only problems"). Cores carry tiny CPU, memory
  and disk bars from `/api/status` `resources` (`NodeResources`, clamped 0 to
  100, amber from 75, red from 90). Android has no desktop vitals stream, so the
  desktop light's bars and colour come from `diag.health` (`DiagnosticsStore`),
  read while a desktop link is open, every 15 minutes when healthy and every 2
  when not. Charts are filled at 35 %.
- **Token charts** (`AgentTokenChartsOverlay`, `AgentTokenCharts.kt`, S145):
  two 12 dp token tanks, 126 dp together, under the status bar centred on the
  top cutout (screen centre otherwise), shown with Diagnostics
  (`showTelemetryStrip`) and the Agents package, poured every second and
  ignoring touches. `core/.../agents/AgentTokenTank.kt` is the same liquid as
  iOS ([NAVIGATION.md](NAVIGATION.md), agent token tanks); the accelerometer
  goes through Genie's `screenVector` for the display rotation and is
  registered only while a tank holds or owes grains. The tanks advance and
  redraw at most 30 times a second (`AgentTokenTank.FRAME_NANOS`).
- **Several desktops (S141).** `DesktopControlHub` keeps the chosen desktop
  (`desktop/preferred-key` in project preferences), its `choices` with presence
  (Running, Not running or not reachable, Checking…), and `checkDesktops`,
  which opens and closes a relay session to each unconnected desktop (forced on
  the Desktops page, at most every 5 minutes after a warm link when there are
  several). Only the chosen desktop is dialled; a choice whose desktop left the
  account falls back to any. The Desktops page lists the others with a Use
  button; Runs on in the prompt sheet lists every desktop
  (`QuickLaunchBackend.desktopNames`/`chooseDesktop`).

### Agents (ledger S042, S074, S079, S102, S106, S112, S116, S118, S142, S143, S145, S146, S152)

- **Eagle view (S042, S112).** `AgentListView.Eagle` is the fourth view
  (Compact, Zen, Full, Eagle; icon `flutter_dash`, the nearest Material bird)
  and the default: a missing or unknown `agents/listView` reads as Eagle, and a
  phone that stored another view is switched once
  (`agents/listViewEagleDefault`). An Eagle row
  (`feature/agents/AgentEagleViews.kt`) has no card: shared hairline rules, the
  title with file count and churn, a 12 dp ticker of the agent's last line, and
  a 32 dp tile on the right. `AgentEagleStrip` scrolls copies 5 dp apart left at
  80 dp/s after a 0.5 s hold on a `Canvas` with a faded edge; only running rows
  move, and nothing moves with animator duration scale 0. `AgentEagleFeed`
  (`core/.../agents`): running desktop rows stream through `ControlWatchFeeds`
  (at most six, re-read only when the run's revision moved); cloud runs read
  `liveText` while running and the last non-user message after
  (`AgentEagle.dialog`); a stopped desktop row with no line reads its transcript
  once (`AgentControlCenter.transcript`, three at a time). The feed runs only
  while Agents shows Eagle; `AgentEagleState` copies lines into a Compose map on
  the main thread so only the changed row redraws.
- **Tile (S116, S142).** `feature/agents/AgentEagleTile.kt` follows iOS
  `AgentEagleTile` after S142: the border is the token ring
  (`AgentTokenRing.kt`; it covers S116's churn ring), churn is a 0.75 dp outline
  on the file-count pill, then up to four badges (`AgentLineBadge.of` in
  `AgentLineChips.kt`, most urgent first), then a green `done` bar or thin
  background-task lines (ten, or seven and `+n`). The status dot sits top right
  (accent running, warning waiting, success done, `#a371f7` merged, danger
  failed, tertiary otherwise). `backgroundTasks` comes from the desktop's
  `agents.list` (`LocalAgentRow`, clamped at 0), and the spoken summary
  (`AgentLineChips.eagleSummary`) ends with "N background tasks". Rows carry no
  image thumbnail, so the tile is a plain `bgOverlay` square.
- **Token ring (S143, S145).** `core/.../agents/AgentTokenFlow.kt` ports
  `AgentTokenPace`, `AgentTokenSampler`, `AgentTokenSeries` and the phase clock.
  The ring reads the row's `tokensIn` / `tokensOut` (`agents.list`, else the
  session's stored counts) and, for the desktop in use, the live pair from
  `diag.vitals` (`control/DesktopTokenFeed.kt`), sampled each second while
  active and settling after 10 s without growth; lanes are 1 dp dashes
  (2.5/2 dp) moving in opposite directions, still under reduced motion.
  `DesktopTokenFeed` is reference-counted, sends `diag.watch` on the link in
  use and `diag.unwatch` when the last holder leaves; the desktop keeps one
  watcher per connection, so any future vitals store must share this feed.
- **Set aside and merge (S118).** `agentops.merge` always sends
  `offerSetAside: true`; a `checkout_dirty` refusal sets
  `MergeCleanupModel.canSetAside`, and the button becomes **Set aside and
  merge**, which merges again with `setAside: true`. The desktop's `setAside`
  note is appended to the merge message.
- **Every desktop in one list (S146).** The hub has `activeKey`,
  `otherDesktops`, `link(desktopKey)` / `request(..., desktopKey)`, `openLink`,
  per-desktop event taps and `channel(desktopKey)`; another desktop is reached
  over its own relay channel in `DesktopSideLinks`. `AgentControlCenter` loads
  up to four other desktops after each full refresh and when one rings
  `agents.changed`; their rows carry `desktopKey`, `elsewhere` and key-prefixed
  ids, are labelled with the desktop's name, and an unreachable one adds "<name>:
  Not reachable". Stop, prompt, continue, transcript, live stream, conflict
  fixes, agent ops (`AgentOpsFeature.client(desktopKey)`), the terminal
  (`DesktopAgentTerminal(desktopKey = ...)`) and Worktrees go to the row's
  desktop; such rows never match the desktop in use's sessions, queue, read
  marks, approvals or Agent Watch numbers. The LAN link forwards events to its
  desktop's taps even when another is chosen. `choose` keeps the old relay
  channel as a side link and `link()` reuses a side link, so switching back and
  forth dials nothing new; a side link counts as Online in `presenceOf`.
- **Agent alert page (S152).** `AgentRouteScreen` looks the id up with
  `AgentControlCenter.row(forRoute)` (another desktop's `<key>:desktop:<n>` when
  only one desktop has that number, a relay run by session id, or a relay run
  the desktop took over), waits while lists load behind a skeleton, and offers
  Try again when nothing matches. Relay runs are published as soon as they land.
- **Pinned last prompt (S079, S102).** `AgentPromptDeck`
  (`A/feature/agents/AgentPromptDeck.kt`) pins your last prompt above the
  transcript of the unified agent page, the desktop session page, the queued
  session page and the phone's own agent. `PromptDeck` (`C/control`) holds
  prompts newest first (`Prompt` blocks and `Steer.fromYou`, a queued run's user
  turns, or `AgentUserMessage` events). One line (tap for up to eight), `3/5`
  when there are several; up to two edges peek below, drawn with
  `matchParentSize` so they never stretch past the card. Swipe right for an
  earlier prompt, left for a later one (tilt 0.04° per dp, resistance at the
  ends); long press offers Copy prompt and Back to the latest. TalkBack gets
  Earlier and Later prompt actions; reduced motion fades without the tilt. The
  agent page's action row scrolls in the width left after the badges (S074).
- **Terminal keys and scrollback (S106).** `AgentTerminalPane` (desktop,
  runner, host and sign-in terminals) has two rows of equal-width keys
  (`TerminalKeyBar`: Esc, Tab, Ctrl, ^C, ⌫, Paste; ←, ↑, ↓, →, Enter at double
  width). Ctrl is one-shot: the next letter is sent as its control byte
  (`terminalChordTyped`, `terminalControlBytes`) or the next arrow as
  `ESC [1;5` + letter (`terminalKeyBytes(control = true)`). The grid follows
  output only while scrolled to the bottom; scrolled up it stays put and shows
  a jump-to-newest button.

### DJ Studio queue (ledger S120, S123)

- `DjQueueThumb` (`A/feature/djstudio/DjQueue.kt`) reads its own pointer
  gesture: a tap or a 350 ms hold opens the menu; a move past touch slop across
  the queue lifts the song onto a deck; a move along the queue is left
  unconsumed so the queue scrolls. TalkBack gets Load on Deck A/B and Remove
  from Queue.
- `DjAutomix.playNow`: a song added while no deck plays, nothing is scratched
  and no fade runs starts on the deck the crossfader favours (or the empty one
  near the middle); the rest of a batch is queued.
- `DjQueueItem.addedAt` (ms, saved with the queue) gives `addedStamp` (time
  today, else "Oct 23, 3:45 PM") on a jam thumbnail and `addedLine` ("Added by
  bob on Oct 23, 2026 at 3:45 PM") in the menu and TalkBack label.

### Code: failed run handoff and pull requests (ledger S144, S153, S155)

- S144: the run log card carries `FmMessageTools` on a failed or rejected run;
  its send button opens the composer with `CiPrompt.composeTarget` (workflow,
  run, commit, failed steps and the excerpt around the last failure line).
- S153: `PublishedPull` carries `author`, `authorName`, `createdMs`, `mergedMs`
  and `mergedBy` from `pull.md`. `PullState` (core `collab/PullThread.kt`) gives
  the Open/Merged/Closed pill, summary line and done copy; `PullTimeline.events`
  adds Merged by time (`epochMs` takes seconds or milliseconds).
  `PullDetailScreen` follows iOS: header, changed files and patch, the
  Conversation card (description first, then events), federated replies,
  `PullWorkflowCards` (`PullDoneCard` replaces the merge card once merged or
  closed), Review (Approve and Request changes only while open) and desktop
  requests. `ApiService.pullCommits` reads `commits.mbox` beside `pull.md`
  (ledger ref, then `forkmesh/pull/<n>`), so cards say "N commits"; a Commits
  card under the changed files lists them newest first, each opening
  `PatchCommitScreen` (Android's changes pages have no tabs).
- The New pull request dialog (`A/feature/code/collab/NewPullDialog.kt`, core
  `collab/PullCompose.kt`) compares two branch menus on the mirror, fills the
  title from the head branch and keeps "Paste a patch instead". When
  `remote-forks` lists forks on other instances, Send to offers them;
  `InboxService.openRemotePull` signs for the target's owner and name with head
  `<this host>/<owner>/<repo>:<branch>` and posts `{owner, repo, pull}` to the
  target's `/pulls` with no session, as iOS and the website do.
- S155: `LiveReleases.platformTile` shows "build N" from build 1, since every
  version starts at build 0.

### Shell: the tour and first-launch permissions (ledger S134, S135)

- **Tour (S134).** The catalog is plain Kotlin in
  `core/.../navigation/TourCatalog.kt` (`TourTrack`, `TourScene`, `TourStop`,
  `TourJump`, `TourPlace`, `TourProgress`; `TourCatalogTest`). Words follow iOS
  except Apple surfaces: the palette shortcut is Ctrl-K, and the system stop
  names the Agents and Inbox widgets, the running-agents notification and the
  New agent Quick Settings tile instead of Siri, the Live Activity and Control
  Center. Progress keeps `nav/tour.v1` and adds `nav/tour.done.v1` and
  `nav/tour.place.v1`. `TourScreen` (`A/feature/navigation/TourScreen.kt`) is
  the chooser (three track cards, "Pick up where you left off", "Not now") and
  the player (`HorizontalPager` of stops, Back, Show me, Next/Finish).
  `TourStage.kt` holds the 330 dp stage, the 1.9 s autoplay a touch pauses for
  6 s, the pulsing `tourTarget` ring and mock pieces; `TourScenes.kt` has the 18
  scenes. With animations off the ring, ripples and sparkle stop and steps
  still advance. Show me pops the tour and 300 ms later calls the shell or
  `NavigationFeature.open(route)`. Home's Tour icon counts tracks not done
  (`ShellTour.left`).
- **No permission prompt at launch (S135).** `PermissionPacing`
  (`core/.../account`, `PermissionPacingTest`) holds the intro text and the
  `permissions/intro` and `permissions/nearby-opened` keys.
  `PermissionsIntro.install` runs after the package gate with
  `freshInstall = needsStarterPack()`, so older installs never see it.
  `PermissionsIntroPage` is the step before the starter pack in
  `OnboardingFlow` ([ONBOARDING.md](ONBOARDING.md)). The notification ask is
  `PushFeature.askWhenUseful`, after the first chat message is sent or when a
  nearby `#mesh` peer appears; `LocalMeshRoom.waitsForRoom` keeps the mesh off
  until `ChatFeature.setMeshOnScreen` first sees `#mesh`, so the Bluetooth
  prompt comes then. Android has no local-network prompt for Wi-Fi sockets.

### Group calls on FM Media (ledger S140, S139, S087)

- **The page.** Android has no native WebRTC engine (FM Media replaces
  libwebrtc), so group calls run the browser's `chat-group-calls.js` in a
  WebView. `GroupCallPage` loads
  `https://appassets.androidplatform.net/group-call/index.html` and serves only
  `assets/group-call/` through `shouldInterceptRequest`. The copies of
  `chat-group-calls.js`, `chat-group-call-crypto.js` (started from a blob URL),
  `chat-call-e2ee.js`, `chat-mlkem.js`, `call-quality-meter.js` and `fmm/*.js`
  are kept byte-identical to `app/public` by `app/tools/sync_android_group_call.py`
  (`--check` in `app/tests/test_android_group_call_page.py`, which also joins in
  headless Chromium); `index.html` and `group-call-host.js` are hand-written.
  The page draws the tile grid (150 px minimum columns, 6 px gaps, 3:4 tiles,
  name capsule, initial avatar, mirrored "You"); `GroupCallViews.kt` draws the
  title, state line, quality pill ("Others"), the encryption note and Mute,
  Camera and Leave.
- **Native side.** `OfficeChannelService.sendLiveFrame` seals `group-call-*`
  frames into the private channel's room without `persist`/`notify` (page
  fields like `sender` or `id` are ignored); incoming ones go to `onLiveFrame`,
  not history; `callTicket` asks for the ticket. `GroupCallFeature` keeps a
  `GroupCallRoster` for the header's "Join · N" capsule and the last minute of
  frames (`GroupCallFrameBuffer`) to replay before join. Header buttons show
  for private, writable channels; a direct call and a group call refuse each
  other.
- **FM Media (S140).** `group-call-host.js` passes the worker, worklet and
  face-stream URLs, asks for `engines: ["fmm", "webrtc"]`, and falls back to
  WebRTC when the WebView lacks WebTransport or WebCodecs (`fmmSupported`) or
  the ticket has no FM Media block. The CSP adds `https:` to `connect-src` and
  `blob:` to `img-src`. The call key is the room seal's identity key (as iOS
  `OfficeChannelService.callIdentity`); the private key never enters the page,
  which signs through the synchronous `FmGroupBridge.sign(call, base64)`: only
  messages starting `forkmesh-call-e2ee-`, under 64 KB, naming the current room
  (`GroupCallIdentity.sign`). The identity goes to the page on join and with
  every `rekeyed`. `OfficeChannelService.accept` adds `sealKid`/`sealAcct` to
  incoming `group-call-*` frames and drops claimed copies elsewhere
  (`GroupCallE2ee.annotate`), so `requireSealed` works.
- **Compare the code.** The page reports `engine` and the verification view in
  its `state` event; `GroupCallVerify` (core) holds iOS's wording and order. A
  capsule under the state line ("Verify", "Compare the code", "Verified") opens a
  sheet with the summary, the 40 sp emoji with names, "Or compare numbers", and
  "They don't match" / "They match" (`confirmCode`); after a mismatch it shows
  the warning and "Leave the call". Tiles mark a missing (orange) or forged
  (red) media signature.
- **Face stream and look (S139).** `chat-call-facestream.js` and its imports
  render a tile's face stream (`representation: face`) with a "Face stream"
  badge. `GroupCallFeature` passes `GroupCallLook.page(FaceChangerFeature.look)`
  (face and background on video calls, `effectiveVoice`) in `join`; the page's
  `prepareLocal` builds `createFaceStage` and `createVoiceStage`. Photos,
  backgrounds and networks are not copied twice: `GroupCallPage` serves
  `group-call/call-data/{face-photos,backgrounds,models}.js` cut from the
  `fm-call-*` script blocks of the bundled `direct-call.html`
  (`GroupCallPageData`), loaded only when needed. `createFaceFeed` runs the
  browser's face mesh on the camera at 15 fps when no stand-in face is drawn,
  so `fmm-call.js` can send face frames when the server switches the sender to
  the face stream; reference pictures come from the stage canvas when a
  background is chosen, so the real room is never sent. The tracker starts with
  the first due face frame. Differences: no own-uplink face stream (section
  11), and a look changed mid-call applies from the next call.
- **Hair and edges (S087).** The call page's face changer (`direct-call.html`,
  built by `app/tools/sync_native_call_faces.py`) already runs the browser's
  `hair-192` network and guided-filter matte, byte-identical to iOS's copy; the
  TrueDepth parts are not built (section 11).

### Speech playground

`Route.Speech` (`/dashboard/speech`, package `speech`) opens
`A/feature/tools/SpeechScreen.kt`, iOS `SpeechScreen`: text card with the spoken
word highlighted, four samples, Speak / Pause / Resume, Stop, Save audio, the
voice card and sheet with previews, and Speed and Pitch sliders (0.5 to 2, 0.05
steps). `SpeechPlaygroundModel` lives in `core/.../tools/SpeechModels.kt` with
`SpeechTest`. `AndroidSpeechEngine` speaks through `TextToSpeech`:

- Speed goes to `setSpeechRate` as is (1.0 is normal there).
- Quality from `Voice.getQuality`: 500 Premium, 400 Enhanced, else Standard.
  Voices still to download are left out; network voices get an Online badge
  and lose ties. `en-us-x-iol-local` shows as "Voice IOL".
- No Personal Voice on Android; the no-natural-voice hint has Get more voices
  (`ACTION_INSTALL_TTS_DATA`, falling back to TTS settings).
- `TextToSpeech` takes at most `getMaxSpeechInputLength()` (4,000) characters a
  call and cannot pause. `SpeechText.chunks` splits at sentence ends, then
  spaces, never inside a surrogate pair; each piece is queued with a
  `SpeechUtterance` id (session, offset, last) so `onRangeStart` maps to the
  whole text and stopped sessions' callbacks drop. Pause stops; Resume restarts
  from the last word started.
- Save audio renders pieces with `synthesizeToFile` and streams them into one
  WAV under `cache/speech-share/` after one `SpeechWav.header`, shared as
  `audio/wav`.
- The manifest declares the `TTS_SERVICE` intent in `<queries>`; without it
  Android 11+ hides the speech engine (Stretch Break and the Genie need it too).

### Shell: the list beside the detail on a foldable (iOS iPhone Duo)

iOS shows list and detail side by side on iPhone Duo's inner display
([NAVIGATION.md](NAVIGATION.md) 7.1). Android does the same on any window at
least 600 dp wide and 480 dp tall.

- `ShellPanes` (`C/navigation/ShellPanes.kt`): list width 40% of the content,
  280 to 380 dp. Cover screen, landscape phone and split screen keep the phone
  stack. Guard: `ShellPanesTest`.
- `TabPanes` (`A/shell/HomeShell.kt`) draws the tab's root in the list column,
  a hairline, then the tab's stack in the detail column (a glass placeholder
  when empty). Chat keeps its own channel sidebar.
- The list column's `PaneListNavigator` routes pushes through `Navigator.pick`,
  which replaces the detail; pushes inside the detail stack as usual.
- The open item's row stays highlighted (`shellPaneSelected`,
  `A/ui/navigation/ShellPaneSelection.kt`, via `LocalShellPaneOpen`).
- Hub pages open in the detail column. From 760 dp wide the navigation rail
  replaces the bottom bar.

## 28. NEXUS network inspection (sync ledger S046)

NEXUS ports the iOS package at `native/ios/Nexus` ([NEXUS.md](NEXUS.md#android)).
Logic is plain Kotlin in `core/.../nexus` (`NexusTest` on the host JVM); screens
and Android-only pieces are in `app/.../feature/nexus`.

- **Same split as the Swift targets.** `NexusAddress` (addresses, networks,
  `Hostname`, `LocalNetwork`), `NexusTargets` (target parsing,
  `ScanAuthorizationPolicy`), `NexusPorts`, `NexusModels`, `NexusDns` (UDP wire
  format and resolver), `NexusScan` (scanner, banners, `SecurityInsights`),
  `NexusDiscovery`, `NexusWeb` (HTTP/TLS inspector) and `NexusClient` (NexusKit,
  remote-agent contract, reports, history). Core has no coroutines, so the
  scanner runs a fixed thread pool and stops through a `NexusCancel` token;
  `NexusState` calls it on `Dispatchers.IO`.
- **java.net in place of Network.framework.** TCP connects are `Socket.connect`
  with the scan timeout, UDP DNS uses `DatagramSocket`, and `NexusFailures` maps
  `ECONNREFUSED`, `EHOSTUNREACH`, `ENETUNREACH`, timeouts and `EPERM` to the iOS
  `ConnectionFailure` cases (refused is Closed, a timeout "No response"). Scan
  target hostnames resolve through the system resolver, so names like `nas.lan`
  work; the DNS explorer queries 1.1.1.1 directly like iOS.
- **NsdManager in place of Bonjour.** `NsdBrowser` browses each service type for
  8 s, then resolves each found service (`resolveService` with an executor on
  Android 14+). No runtime permission; Android has no Local Network prompt.
- **More than iOS shows, never less.** The TLS observation comes from an
  `SSLSocket` handshake with hostname checking on, so Android lists the full
  chain (issuer, validity, SANs, key size, signature algorithm, serial,
  SHA-256), protocol and cipher suite. Validation is never bypassed: an
  untrusted certificate is "Not trusted" with the reason and no chain. Timings
  add connect and first-byte times. Discovered devices get an *inferred* kind
  from their services (`DeviceClassifier`), labelled as such.
- **History and reports.** Android keeps history in
  `filesDir/nexus/history.json` (50 scans, 20 discoveries, the device list; iOS
  keeps it only until quit), with a Settings switch and a History menu to
  clear it. A scan's detail shares `NexusReportExporter` JSON or CSV
  (`nexus-share/` in the FileProvider paths).
- **Settings reach the scanner.** Timeout, concurrency and "Allow internet
  targets" are stored in the `nexus` preferences and passed into every scan.
  When the policy answers *requires attestation*, the Scan tab asks for the iOS
  attestation statement and retries; internet ranges, more than four internet
  hosts and multicast/broadcast targets stay refused. An empty target is filled
  from `LocalNetwork.current()` with a "Use this network" row (S067,
  `NexusTest.testLocalNetworkSuggestion`). `10.0.0.0/8` and `100.64.0.0/10`
  count as local without confirmation (`IPv4Addr.isLocalScope`, as iOS), which
  on mobile data covers the carrier's network: a product decision open for both
  apps.
- **Not a Market package.** iOS `PackageGate` has no entry for NEXUS (its
  `PackageRoutesTests` asserts that), so the You hub's Workspace group
  (`HubCatalog.workspace`) always lists it and `/dashboard/nexus` and
  `forkmesh://nexus` open it. The desktop's `nexus` package is separate.
- **Presentation.** `NexusScreen` hides the shell chrome and draws its own glass
  tab bar (`FmBottomBar`) with six tabs; detail screens push on the normal
  stack. `Route.Nexus` is not modal because Android's router drops modal routes
  from deep-link stacks.

## 29. Composer actions and screen capture (sync ledger S028, S032)

- **Prompt bar actions that start no agent (S028).** As iOS
  `QuickPromptActions`, the model menu starts with Bug report, Task · assign,
  Manual · create issue, ActivityPub · post and Take a note
  (`C/composer/QuickPromptActions.kt`, provider `prompt-action`). The pick is
  kept under `agent/quick/action` (`""` once a model is picked); until then the
  chip shows Bug report whenever nothing can run an agent
  (`QuickAgentLauncher.hasAgent`: signed out, no destination, or no API key),
  so Send always works. Each ends in `QuickLaunchOutcome.Done`, shown in the
  shell's notice. `A/feature/composer/AppPromptActions.kt` sends them:
  `POST /api/bug-reports` (screen `composer`, photos attached, no account
  needed), `POST /api/tasks` (first line as title up to 160 characters, the
  prompt as details, General, priority 50), a signed issue through
  `InboxService`, a signed manual `discussion`/`open` `ap-publish`
  (`ApiCiClient.announcePost`) and a private account note. A Task shows Assign
  to (you, then the board's members when you manage it;
  `tasks/composerAssignee`). With an action chosen the picker hides Runs on and
  shows Repository only for an issue or post. The diagnostics report always
  starts an agent (`send(agentOnly = true)`).
- **Hold a blank area to copy part of the screen (S032).** See
  [CAPTURE_MARKUP.md](CAPTURE_MARKUP.md#android-long-press-a-blank-area-to-copy-part-of-the-screen).
  The hold is a window touch watcher installed from `ComposerFeature.install`;
  Home's background menu has "Select area to copy…". `CaptureOverlay` says
  "Copied to clipboard" only when the copy succeeded, else "Could not copy the
  picture".

## 30. Shell and kit parity (sync ledger S003 to S016)

- **Scroll edge, sheets and large titles (S006).** `fmScrollEdge()` fades
  content into the background under the bar once the page has scrolled (the iOS
  26 soft scroll edge); `DetailScaffold` and `RoutePage` apply it.
  `FmModalBottomSheet` opens at the medium detent unless `isScrollControlled`;
  every sheet uses `FmSheet.corner` (28 dp). iOS shows large titles on five
  screens (Status, Network, Stats, Admin, Search), so scaffolds take
  `largeTitle = true`: `FmLargeTitleBox` draws a 34 sp bold title that a nested
  scroll connection collapses before the content scrolls; the bar title fades
  in over the last 30 %.
- **Chat header and composer are glass over the messages (S004).** The chat
  room is an `FmBackdropHost`: the transcript is the content and the header
  (with its banners) and the composer are the overlay, so both refract the
  messages beneath. Their measured heights become the list's content padding. A
  short gradient behind the header keeps the status bar readable.
  `PromptComposer` passes no tint when a backdrop is present, so the card is
  clear glass there and solid elsewhere.

### Activity halo and the shortcut progress ring (S016)

`shell/ShellBarActivity.kt` ports iOS `ShellActivityHalo` and
`ShellBarProgressRing`. A cell is `active` while its page has work running
(Agents with running agents, Timers with a running timer, Shortcuts with a
running run; iOS `ShellNavigatorCounts.active`) and draws the breathing glow and
a turning 70 % arc when Show activity is on. While a shortcut runs, Shortcuts
shows the progress ring instead and the glyph shrinks to 78 %.
`ShellBarProgress` (core) fills elapsed over expected length, capped at 99 %,
redrawn twice a second, a spinning short arc without an earlier run to estimate
from; with several it follows the one expected to finish last. TalkBack reads
"Running, N percent". The bar reads `TeammatesFeature.startedShortcuts`, so it
never starts the shortcuts store itself (it looks every 2 s).

### Busy ring on buttons and the side rail

`ui/kit/FmBusyRing.kt` draws the halo's arc without the glow: a faint accent
track with a 70 % sweep turning every 1.6 s, drawing only (no touches, not seen
by TalkBack, still under reduced motion). The rule matches desktop and iOS: a
button that turns disabled within 1 s of a tap is working, and the ring turns
around its icon (or its outline, without an icon) until it is enabled again, at
most 60 s. The kit buttons (`FmIconButton`, `FmToneButton`, `FmTextButton`,
`FmElevatedButton`, `FmPillButton`, and `FmFilledButton`/`FmOutlinedButton` in
`feature/code/RepoCommon.kt`) record the tap and watch `enabled`
(`rememberFmBusyTap`), so `enabled = !busy` screens get the ring free. Each also
takes `busy: Boolean?` (`null` guesses). Buttons that disable after a tap for
another reason (a send whose field empties, a stepper at its limit, Move up on
the first row, Undo with nothing left, a choice that turns itself off) pass the
real flag (`busy = sending`) or `busy = false`; otherwise a condition like
`!sending && text.isNotBlank()` would spin for 60 s.
`FmExtendedFloatingActionButton` is always enabled and rings only with `busy`.
Material buttons draw the sweep as their border (`fmBusyBorder`); drawn kit
buttons use `Modifier.fmBusyRing`. Nothing animates while no button is busy. On
a tablet or in landscape the side rail rings Agents while agents run and Home
while a hub page the bar would mark active is running.

### Home search, custom order and drag (S010, S012)

- **Custom order.** `ShellMenuLayout` (core, key `shell/menu-layout`) keeps
  where you moved each page, per group; unknown pages stay at the end of their
  group, and a page that leaves and returns comes back to its place.
  `ShellBarPrefs.arranged` applies it before Home's sort; `moveInMenu` lands a
  forward move after the target and a backward move before it.
- **Drag.** `shell/ShellDrag.kt` is one drag shared by Home and the bar. Touch,
  hold and move: the menu closes, the source dims to 35 % and a glass copy
  follows the finger (accent over the bar). It watches the initial pointer pass
  so press and long press still work, and keeps root coordinates so a tile
  moving under the finger does not jump. On Home, passing over a tile moves
  there (grouped layouts only) and switches the sort to Custom order. Dropping
  on the bar puts the page in that slot (`putInBar`, replacing when full);
  dropping a bar cell off the bar removes it; onto another cell moves it. Home
  stays in the bar. `shellDragSource` ends the drag in a `finally`, so a
  cancelled gesture never leaves the preview on screen.
- **Search.** Pull Home down 56 dp (`ShellBarOrder.revealsSearch`) for the
  glass search capsule. It filters pages by title, or a whole group whose title
  matches, shows hidden groups while searching, and opens the first result on
  the Search key. With no text the trailing control is the settings menu; with
  text it clears. "Search everywhere for …" opens global search. The field
  hides when it loses focus empty.

### Wallpaper behind the glass (S007)

Settings → Appearance ends with a Background row (iOS
`AppearanceBackgroundRow`): a 48 x 84 thumbnail, Choose image / Change image
(`PickVisualMedia`, no permission), Remove, and a Dim slider (`FmIosSlider`, 0
to 90 % in 5 % steps, default 40 %).

- Keys are iOS's: `appearance/backgroundImage` (a file name, empty for none; a
  name with `/` is none) and `appearance/backgroundDim` (a double). `Wallpaper`
  (core `settings/Wallpaper.kt`, `WallpaperTest`) holds clamp, default, name rule
  and sizing; `AppearanceStore` has `setWallpaperImage`, `previewWallpaperDim`
  (while dragging, nothing written) and `setWallpaperDim` (on release).
- The picture is copied to `filesDir/wallpapers/wallpaper-<millis>.jpg`
  (`platform/WallpaperStore.kt`), decoded at just over screen size
  (`ImageDecoder` on API 28+, `BitmapFactory` with a sample size below); the
  previous file is deleted. HEIC needs API 28+.
- `FmWallpaperLayer` draws it cropped to fill, then `bgBase` at the dim opacity
  (iOS dims with the theme's base colour, so a light theme brightens it), under
  the app in `MainActivity` and as the first layer in `FmBackdropHost` so the
  bar's glass refracts it.
- `FmPalette.wallpaper` (`ForkMeshTheme(wallpaper = ...)`) gives `canvas`,
  `readingSurface` and a 60 % `listSurface`; `canvas` is `bgBase` without a
  picture and transparent with one. The shell, the screen host, every
  full-screen page fill and the Material `background` paint `canvas`. Left out:
  iOS's 62 % card fills and softer borders, the 80 % terminal surface,
  non-full-screen `bgBase` shapes, the RSS reader's theme and the rail.

### The voice prompt (S015)

Touch and hold the new-agent button to speak a prompt; it listens until you let
go and starts an agent with your last repository and model (iOS voice prompt,
[NAVIGATION.md](NAVIGATION.md) section 2.3). The command palette is the
**Commands** button on the voice screen.

- **Gesture** (`holdToTalk`, `feature/composer/ShellPrompt.kt`). One pointer
  gesture, because a long click ends when it fires and cannot see the lift.
  Lifting before 350 ms (`VoiceLaunchRules.HOLD_MS`) is a tap (opens the
  composer); moving more than 10 dp first does nothing; after 350 ms the screen
  listens while the finger stays down and letting go sends. TalkBack's
  long-click "Speak a prompt" opens it hands-free. Wired to the top bar and rail
  buttons.
- **Screen** (`ShellVoiceOverlay.kt`). A blurred, dimmed shell (`Modifier.blur`
  on Android 12+, else 92% black), the orb springing out of the button with the
  summon chime and a haptic, 72 spokes following the voice meter, ripples, the
  words, a white arc filling over the 3 s pause (hands-free only), a spinner
  while sending, a launch burst, and Cancel, Type and Commands. Tapping outside
  or Cancel flies the orb back; Type moves the words into the composer draft.
  Reduce motion fades instead of flying. Sounds follow the interface sounds
  setting; other cues are silenced (`FmSoundCues.quieten`).
- **Listening** (`ShellVoiceLaunch.kt`, `SpeechDictation.kt`). Android's
  recognizer stops after a quiet spell, so while the finger is down
  `DictationSession.holds` restarts it and `VoiceTranscript` joins the passes.
  Letting go keeps the microphone 250 ms more, then sends on the final result
  (at most 2 s). Letting go before anything was heard switches to hands-free:
  send after 3 s of quiet or on an orb tap. The 120 s limit and the Cancel,
  Type and nothing-heard paths ("I didn't hear anything", closing after 2.2 s)
  are iOS's. The microphone opens after the summon chime (0.42 s held, up to
  0.8 s hands-free).
- **Sending** goes through `rememberQuickAgentActions`, the composer's own
  launcher (last repository, destination, agent and model). The orb is the
  launch flight's source (`LaunchFlight.source`). A failure puts the words in
  the composer draft and opens it with the usual fix.
- **Chimes** (`core/agentwatch/VoiceLaunchChimes.kt`, `VoiceLaunchChimeTest`)
  are iOS's note tables, rendered by `renderNotes` and played from a `SoundPool`
  (`ui/kit/VoiceLaunchSounds.kt`, cached under `sound-cues/`).
- **Pure logic** (`core/composer/VoiceLaunch.kt`, `VoiceLaunchTest`): release
  rules, timings, arc fill, hints, transcript joiner and the voice meter
  (`onRmsChanged` decibels to 0..1).
- **Differences:** `SpeechRecognizer` is created at touch down
  (`SpeechDictation.prime`); no fast-results switch, and some devices beep on
  each restart; the star field does not tilt; the words do not shimmer (the
  "Listening" line pulses); the first hold ends hands-free after the
  microphone prompt.

### Action sheets (S005)

`ui/kit/FmActionSheet.kt` is the iPhone action sheet of `confirmationDialog`: a
14 dp rounded group at the bottom with title and message in 13 sp secondary
text, 57 dp rows in 20 sp (accent, or danger for destructive) split by
hairlines, and a detached bold Cancel 8 dp below. `FmMessenger.showActionSheet`
returns the chosen index (null for Cancel or outside); `confirmAction` is the
one-button form. Wherever iOS uses `confirmationDialog` Android uses it with
the same wording; `showConfirmDialog` stays the centred alert where iOS uses
`.alert` or `fmDialog`.

- Action sheets: Code (delete or merge a branch, delete an issue, move an issue
  to a task, merge-queue actions, Actions run controls, delete a webhook, Merge
  all with AI, disconnect a provider mirror); Agents (end a desktop terminal,
  drop a queued session with Cancel reading "Keep", continue on the desktop,
  open a run in the provider's terminal, stop and delete an outside agent,
  delete a desktop agent account, close an SSH session); Coves (discard edits,
  delete a document, delete an Actions secret); chat delete-message,
  delete-channel and leave-channel; send to Discord through the desktop,
  discard photo markup, sign out of the project, and Folder or Zip when
  importing a local repository.
- Centred alerts: discard or delete in the local repository and file editor,
  "not a node yet", "name is taken", the encryption note, the
  merge-without-review waiver, remove an SSH key, revoke an installed app,
  forget a paired desktop, switch account, "Cove proposed", "Cannot open a pull
  request", the passkey fallback of "Confirm it's you", and actions iOS does not
  confirm at all. Pickers iOS builds as menus or sheets stay bottom sheets.

## 31. Health tools, Aquarium and Pocket mode (sync ledger S048, S049, S057)

Ported from iOS `Features/Tools`, `Features/Aquarium` and
`Shell/ShellPocket.swift`.

### Health and wellbeing tools (S048)

Routes `Steps`, `Water`, `Sleep`, `SugarBreak`, `Stretch` and `FoodGuide` (paths
`steps`, `water`, `sleep`, `sugar-break`, `stretch`, `food-guide`; same package
ids) follow Calories in the Personal group. `feature/health/HealthFeature.kt`
registers them; logic lives in `core/.../health/` with the iOS pref keys
(`HealthPrefs`) and the same `/api/steps`, `/api/water`, `/api/sleep`,
`/api/sugar-break` and `/api/stretch` bodies, so web and desktop read the same
days. There is no Health Connect dependency; where iOS uses Apple Health,
Android uses the phone's sensors or leaves the part out:

- **Pedometer.** `Sensor.TYPE_STEP_COUNTER` counts from boot, so
  `StepsDailyLog` turns readings into day totals (`health.steps.android.days`,
  `.last`), notices reboots and splits steps across midnight by waking hours.
  Counting starts when Steps is switched on (`ACTIVITY_RECOGNITION` asked then).
  The accelerometer (about 50 Hz, in g) feeds `StrideMeter` for cadence only
  while the app is open and something listens. Distance is steps x 0.72 m;
  floors and active energy are 0. `WalkActivity` is iOS
  `WalkActivityController`; synced days carry device `android`. No Save to
  Apple Health. `StepsSensors.refresh` is `@Synchronized` (main and background
  callers could register twice).
- **Walk notification (S108).** `WalkLiveService`, a `health` foreground
  service (`FOREGROUND_SERVICE_HEALTH`; on Android 14+ it needs the
  `ACTIVITY_RECOGNITION` steps already require), stands in for the Live
  Activity: an ongoing, public notification from the moment `WalkDetector`
  starts a walk ("Walking · N steps", distance, today's steps, a chronometer).
  If the app leaves the screen while you have been moving for 10 s
  (`LEAVE_START_AFTER`) the walk starts then; during a walk the step counter and
  detector keep running in the background (the accelerometer stops) until
  three minutes of stillness. `WalkDetectorTest` ports the iOS cases.
  `StepsWalkChip` (`feature/health/StepsWalkChip.kt`) shows in the top bar
  while `WalkActivity.stepping` and opens the Pedometer.
- **Sleep.** No activity history without Play Services, so `SleepRestLog` keeps
  8 days of still/moving spans from the step counter (under 0.02 steps a second
  is still; a reboot is unknown) for the ported `SleepDetector`,
  `SleepSchedule` and `SleepUsageLog`. A car ride reads as still; the usage log,
  schedule window and 3-hour minimum keep false nights out.
- **Water Intake.** Same model and screen without the Apple Health card; drinks
  an iPhone imported still show "From Apple Health".
- **Stretch Break.** iOS session, poses and logging; cues use `TextToSpeech`,
  which cannot pause mid-line, so resume repeats the line. Source `android`,
  `health` false.
- **Sugar Break.** Same model and contract card (drawn on an
  `android.graphics.Canvas` at 3x, shared as PNG); Apple Health sugar grams and
  the "log that I had sugar" nudge are left out.
- **Food Guide.** iOS catalog (99 products, 31 recipes), scoring and diet
  filters on the phone; the budget comes from the Calorie Counter (or 2,000
  kcal). Without HealthKit nutrition history it uses everyday fiber and
  potassium drivers and says so.

### Aquarium (S049)

`core/.../aquarium/` is a line-for-line port of iOS `AquariumTank` and
`AquariumFish` (tricks, swimmers, seeded FNV species and colours, the iOS tests
with the same seeds). "aquarium" is a phone package
(`PhonePackages.AQUARIUM`, `packages/aquarium/on`, off until installed from the
Market); `PackageGateFeature.publish()` refreshes `AquariumFeature.on`.
`AquariumLayer(hidden, floor)` draws the tank on a `Canvas` at about 30 fps from
`withFrameNanos`, stops in the background or while hidden, never takes touches,
and treats reduce motion as animator scale 0. `shell/HomeShell.kt` overlays it
with `hidden = composerOpen || keyboardUp || voiceOpen` and the floor at the top
of the bottom bar. Until agents feed `AquariumFeature.setSwimmers` the tank
shows three guest fish; without a screen scan feeding `setLandmarks` the peek,
boop and read tricks do not fire.

### Pocket mode (S057)

`feature/pocket/PocketMode.kt` is iOS `ShellPocket`: `PocketMode.lock()` /
`unlock()` and `PocketLockOverlay()`, drawn above everything in
`Features.Root`. Black, with the date and a 72 sp clock, the Glasses Alert radar
and counts (or "Glasses Alert is off"), a pings card (unread and local pings,
newest three titles), a walking card from `WalkActivity`, and slide to unlock
(past 85 % of its travel, else it springs back). Back does nothing, the system
bars hide and the screen stays on; lock plays a commit haptic, unlock a success
one. Copy and slider rule: `core/.../pocket/PocketText.kt`. `PocketModeRow` is
the You hub row ("Pocket mode", or "Pocket mode, walking" with a pulsing icon);
Settings → General's Suggestion mode card also holds it.

## 32. The Genie spider and outreach (sync ledger S047, S058)

The iOS Genie ([GENIE.md](GENIE.md#android)) on Android. Pure logic is in
`core/.../genie` (`GenieOutreachTest`, `GenieAutoSendTest`, `GenieGravityTest`;
`./gradlew :core:coreTest -Pfilter=genie`); views in `app/.../feature/genie`.
Storage keys and server routes match iOS.

- **Spider.** `GenieOverlay` sits in `Features.Root` beside
  `PingCelebrationOverlay`, over the whole app; only the 92 dp spider box takes
  touches. `GeniePainter` draws iOS's figure on a `Canvas` in the same 100-unit
  box (trick poses and particles left out). It walks `GenieArena` (screen
  sides, the ceiling under the status bar plus 56 dp, the floor at
  `ShellBar.clearance()`) with `GenieGravity.crawl`, `wander` every 5 to 13 s and
  `silk`, ported with the same numbers. Frame rate follows iOS
  `GenieSpiderLayer.frameRate`: 24 fps at rest or crawling, 30 in a reaction, 60
  only while held or flying (each frame redraws the window).
- **Tilt.** `TYPE_GRAVITY` (rotated to the screen) is the gravity
  `GenieGravity.fly` pulls with, across and down, and tilts the body (`leanX`);
  a push over 0.25 g from `TYPE_LINEAR_ACCELERATION` knocks it loose (`loosen`).
  The liquid tumble, table walk and keep-off-buttons placement need iOS's
  accessibility screen scan and are not ported.
- **Touch.** Tap shows the next `GenieHints` line for 5 s (`genie-line`); drag
  and flick fly it; a gentle release near a side tucks it a third off screen
  (`genie/parked`). Long press: Talk, Surprise me, Someone to reach out to, Stop
  suggesting people, Stay put, Come out, Reset size and place, Hide the spider
  (`genie/hidden`). Settings → General's Genie card has "Show the Genie spider"
  (`GenieSpiderSwitchRow`).
- **Badge cheers.** `PingCelebration.badgeLine` (core `model/`) turns a fresh
  `badge` ping into "You earned Stargazer!" or "You reached Silver Fan Club!",
  shown with the cheer when `NotificationService.celebration` counts up.
- **Voice.** Talk opens `GenieTalkBubble`: `SpeechDictation` in, `TextToSpeech`
  (pitch 0.92) out, last three lines shown. `GenieChat` keeps four exchanges for
  10 minutes, answers "say …" and "stop listening" without a model, and
  otherwise asks the saved agent key's provider (`AgentCredentialStore`,
  strongest model). Without a key it says it needs one. The iOS tool loop and
  always listening are not ported.
- **Outreach coach.** `GenieOutreachCoach` checks every minute and shows a
  never-contacted CRM contact beside the spider (`GenieOutreach.pick`: 2 h
  apart, 3 min after the app comes forward, 30 s on screen, `genie/outreach_*`).
  Tapping writes the email (`GenieOutreach.write`, the saved key's model, 90 s,
  else the template) and opens it in Zoho Mail on the CRM's region with the
  organization's other members on cc (`memberEmails`, read in `GenieCrm` so
  `CrmOrganizationData` is untouched), else the share sheet; investor pipelines
  first mint a tracked deck link.
- **Suggestion mode and the mail chip (S058).** `ShellSuggestions`
  (`shell/suggestion_mode`) refreshes every 10 minutes. `ShellMailChip` (top bar,
  beside Glasses Alert) is a 44 dp portrait with today's count (accent while
  auto-send is on, with the 864 s countdown ring), a stage mark (search, edit,
  send, check for 3 s, pause in warning colour) and a spinner while writing. Tap
  writes to the contact; long press turns auto-send off at once, but turning it
  on first asks "Send emails for you?", because it sends one immediately and up
  to 100 a day unseen. Contact pictures are read with a bounded loop
  (`readNBytes(int)` exists only from Android 13) and decoded at about 256 px,
  so a huge CRM picture cannot exhaust memory. `SuggestionModeRow` is the You
  hub switch; Settings → General has a Suggestion mode card.
- **Auto-send.** `ShellAutoSend` with `GenieAutoSendPace` (`shell/auto_send*`)
  sends one email every 864 s, 100 a day, through
  `POST /api/crm/<org>/outreach/auto` to the best contact in a CRM with Zoho
  connected, skips refused contacts (five a turn), pauses with the reason
  otherwise, and posts the phone's draft when the server has no model. It runs
  only while the app is in front. Each send opens `GenieAutoSendReviewHost`'s
  sheet (1 to 5 stars and a note, `POST /outreach/rate`, "Sent · 2 more").
  Messages go through `GenieMailStatus.say` to `GenieMailNoticeLine` (under the
  top bar, `shell-mail-notice`) and device-local Pings
  (`InboxFeature.recordLocalFailure`, limited to 8 an hour). Writers
  (`GenieFeature.outreachWriters`, S066) are the saved agent key, then
  `desktopWriter()`: with a paired desktop the prompt goes to `agents.write`
  over `AgentsFeature.hub` (LAN or relay, 300 s,
  `GenieOutreach.DESKTOP_TIMEOUT`) and its default agent writes the email
  (`GenieDesktopWriter`). No writer and no server model pauses with
  `GenieAutoSend.NO_WRITER`; writers that all failed give `UNWRITTEN`.
- Left out on purpose: hand play (TrueDepth), screen plays and the screen
  scanner, tricks and effects, and vitals.

## 33. Video Mix DJ Studio (sync ledger S045)

The package id is `video-mix-dj-studio`, route `Route.VideoMixDjStudio` (paths
`video-mix-dj-studio` and `dj-studio`).

- **Shared logic (core `djstudio/`).** Playlists, tags and smart playlists
  (`DjCrate`, `DjSmart`), AUTOMIX, BPM detection and the gain curve (`DjAudio`,
  `DjStudio`), play counts and jams (`DjJam`, `DjJamApi`), with the iOS tests.
  It parses links and builds addresses; it fetches nothing.
- **Playback rule.** An earlier studio app (pull request #475, `pr/475`) was not
  merged: it pulled YouTube's raw stream out of a hidden WebView, played it
  through its own `AudioTrack` outside YouTube's player and ads, and scraped
  search results, which breaks YouTube's terms and Google Play policy. Like
  iOS, an Android studio must play through the visible YouTube IFrame player
  with YouTube's controls and ads.
- **AUTO DJ (S063, S104, S109).** A sparkle button next to AUTOMIX
  (`DjAutoDjButton`, `DjStudioAutoDj.kt`). While on it turns AUTOMIX on (and off
  again if it did), reads the playing song's Invidious recommendations
  (`invidiousData`, else a search for the song), keeps songs only and ranks the
  artist's own, Topic, VEVO, official or remastered uploads first, drops the
  seed, anything on a deck, played this session or heard under another upload,
  checks the like counts of the first six
  (`/api/v1/videos/<id>?fields=likeCount,viewCount`) and ranks by likes and
  relatedness (`DjAutoDj`, core, iOS rules and tests). `preload` takes the next
  pick before the replay pool; shake loads a new pick. Saved with AUTOMIX
  (`DjSaved.autoDj`). Android's own decoder knows the length, so iOS's `DjEnd`
  is not needed.
- **Repeat.** A `repeat` button by each deck's clock (`DjRepeatButton`) and
  "Repeat song" in the deck sheet, saved with the deck (`DjSavedDeck.repeats`),
  cleared by Reset All; AUTOMIX never mixes out of a repeating deck. The deck
  wraps from where the sound ends to where it starts, skipping the quiet head
  and tail (`DjRepeat.span`, `step`: wrap at the end or schedule it under 0.6 s
  away); a decoder end on a repeating deck restarts there too.

## 34. Open product decisions (2026-10-20)

Kept as on iOS, worth a decision for both apps: a direct chat shares the
phone's motion with the other person while open, with no setting (section 14,
"Live touch"); and NEXUS counts `10.0.0.0/8` and `100.64.0.0/10` as local
without confirmation (section 28).

## 35. Chat relay hardening (2026-10-22)

Android's side of [chat-relay-hardening.md](../app/chat-relay-hardening.md) (R1
to R7). Shared rooms are joined by every device of an instance, guests included,
so the relay carries only public content and nobody can speak, edit or delete
as somebody else. Nothing here talks to older builds.

- **R1, bundles bound to the node key.** `ChatNodeKey` (`C/crypto/ChatNodeKey.kt`)
  signs with the node key (`DeviceIdentity.nodeId` is already its `b64u`
  Ed25519 public key); `RoomSeal.nodeKey` adds `nsig` over
  `forkmesh-chat-e2ee-node-v1\n<room>\n<kid>\n<idk>\n<acct>` to every open-room
  bundle (relay, office rooms, `#mesh`), and an open room drops a bundle whose
  `node` is not a canonical 32-byte key or whose `nsig` fails
  (`ChatV3.verifyNodeBinding`). Strict rooms keep the v3 bundle.
  `ChatNodeBindingTest` replays `conformance/chat/v3/node-binding.json`
  including its three rejects.
- **R2, nothing private on the relay.** `RelayFrames.isPrivate` (`dm`,
  `invite`, `cove-invite`, `cove-open`, `private: true`, a conversation
  starting with `@`) is refused by `RelayRoom.send` and dropped on receipt
  before history, roster hooks or `ChatExtras`. Private channels are office
  rooms created with `POST /api/chat/channels`. Message on a person, a reply to
  "@name" and share targets for people go through the person's server DM; a
  guest has no Message action. `ChatHub.withoutRelayPrivate` drops `@peer`
  conversations and relay private channels, with their replies, reactions,
  stickers, edits and receipts, from saved history on every load. `#mesh` keeps
  its own frames. Cove invites are server grants (`CovesModel.invite`).
- **R3, only authenticated frames act for a person.** Every person frame is
  sealed, public channel chat included. `RelayFrames.authenticated` fills a
  missing `senderId` with the sealed sender's stored node (RoomSeal rejects a
  different one). An unsealed frame is accepted only with envelope `srv: true`,
  only as `chat` in a `#` channel, its sender id prefixed `srv:` unless it
  already names an integration (`webhook:`, `app:`, `system:`), account claims
  removed and `accountKind: integration`; it never reaches the roster, and
  edits, deletes, reactions and thread frames are never taken from it. A delete
  for a message with an unknown author is ignored. Office rooms do the same
  (`OfficeChannelService.accept`) in every server channel room, `#email-log`
  included, live or from HTTP history; DM rooms never take an unsealed frame. A
  server frame with `serverVisible: true` (webhooks) is marked
  "server-visible" (`ChatMessage.serverVisible`, saved as `sv`); webhook
  settings stay on web and desktop. ForkBot replies keep this phone's
  `senderId` and name the bot in `bot`; since any sealed sender can set `bot`, a
  bot line shows as "<bot> · via <verified account or Guest ####>"
  (`RelayFrames.botLabel`) everywhere, with sender id `bot:<node>:<bot>`,
  relabelled when `NodeOwners` confirms the account (`relabelBots`).
- **R4, accounts checked with the server.** `NodeOwners` asks
  `GET /api/chat/node-owner?node=` once per node and hour (a failure retried
  after a minute); the roster shows a peer's `ownerUser` only when it matches,
  otherwise the peer is a guest. Sign-in binds the node key
  (`AuthController.bindLogin` sends `publicKeyB64url`).
- **R5, blocks kept by the server.** `ChatBlocks` keeps the list (prefs
  `chat/blockedAccounts`), pushes it with `PUT /api/chat/blocks` on every change
  and reads it with `GET` at sign-in (pushing first if a push failed).
  `ChatHub` hides a blocked account's messages and DM; the channel list shows a
  Blocked section with Unblock.
- **R7, less about you.** Relay frames carry no time zone, host name or `dg`;
  the Solana address goes out only with "Accept tips in chat" (Settings,
  Funding and payouts, `AccountSettings.chatTipsEnabled`, off by default).
  `#mesh`: bundles use the node id as `acct`, frames carry no `ownerUser` (a
  received one is ignored), backfill resends only frames already sealed to that
  device, the beacon id is random (a beacon is matched to a linked peer by
  address), `setFocus` goes only to links of the DM partner's approved devices
  (`DirectMessageService.partnerKids`), and the header and info sheet say
  "Anyone nearby with ForkMesh open can read #mesh." There is no setting that
  turns the mesh off.

## 36. Account data (S101)

Settings > Data follows iOS ([account-data.md](../app/account-data.md)).
`AccountSyncService` (`C/settings/AccountSync.kt`) saves three items to
`/api/accounts/data/store/<key>`: `android.home` (`shell/*` and the Insights
layout keys), `android.rss` (`RssLibrary.syncData`, without articles, activity
or fetch state) and `android.settings` (every other preference). Keys differ
from iOS's `ios.*` because the apps encode settings differently
(`DataFolder.exportSettings` writes typed `forkmesh.android-settings` values),
so neither phone overwrites the other. Preferences under
`AccountSyncKeys.skippedPrefixes` never leave the phone: project-scoped keys
(`p/`), sign-in, identity, owner and chat keys, chat history, server and relay
addresses, caches, read state, recents and counters; secrets live in
`SecureStore`. A value over 32 KiB is skipped. Saving is off until "Save my
settings to ForkMesh" is on; then `AccountSyncSetup` saves on every foreground
change, only items whose SHA-256 differs from the last copy
(`accountSync/hash/<account>/<key>`). Restore asks first, writes preferences
directly (Appearance reloads at once, the rest on next launch) and replaces RSS
subscriptions with `RssLibrary.restoreSync`, keeping a matching feed's id,
articles and fetch state. `AccountDataBrowser`
(`C/settings/AccountDataInventory.kt`) lists the inventory by subject, category
and table, pages rows 50 at a time, removes a row only where the server marks it
`deletable`, and deletes one or all saved items; export downloads
`/api/accounts/export`.

## 37. Pinned Settings headings (2026-10-31)

The Settings header and the heading of the section being read stay in view on
every platform (user request).

- **iPhone.** `SettingsScreen` keeps "Settings" in the navigation bar and the
  search field under it (`navigationBarDrawer(displayMode: .always)`). Each
  `SettingsSection` reports its frame (`settingsSectionFrame`,
  `SettingsSectionFramesKey`), and the scroll view draws the heading crossing
  its top edge over the page (`settingsPinnedHeaders`,
  `SettingsPinnedHeader.swift`); the next heading pushes it out
  (`SettingsPinnedSection.pick`). The pinned copy is hidden from VoiceOver.
- **Android.** `SettingsScreen` builds each section with `LazyListScope.section`:
  the heading is a `stickyHeader` on the canvas colour
  (`settings-section-header`) and the body an item after it, so Compose pins and
  pushes it. The hub's search field (`SettingsHubScreen`) is a `stickyHeader`
  too; the app bar sits outside the list.

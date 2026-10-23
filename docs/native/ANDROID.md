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

- **Preferences** (`A/platform/Prefs.kt`) keep their key names across
  releases, so stored values need no renaming. A value stored with another
  type reads as absent. Sign-out writes synchronously.
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

## 4. Store application id and updates

The store release is an update of the app published as `com.forkmesh.forkmesh`.
The release APK is built with `-Pforkmesh.appId=com.forkmesh.forkmesh` and
signed with the v3 key (`native/android/README.md`); the default application id
`com.forkmesh.nativeapp` installs beside it.

- **Same application id and signing key.** Keystore keys and app data survive
  an update only because the id and signer stay the same. The launcher label
  follows the id (`app/build.gradle.kts`).
- **Component names.** A launcher alias `com.forkmesh.forkmesh.MainActivity`
  and an autofill service `com.forkmesh.forkmesh.vault.VaultAutofillService`
  keep their published names, so pinned icons and the chosen autofill service
  keep working.
- **Version.** The version name and versionCode stay above every release
  already published under the store id.
- **Stable data paths.** Local repositories stay under `AppFiles.documentsDir`
  (`forkmesh/repos`) and `projects/<slug>/forkmesh/repos`
  (`LocalRepoStore.rootIn`); vault device keys, autofill Keystore aliases and
  `noBackupFilesDir/vault-autofill` keep their names, so an update finds them
  where the previous release left them.
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
  - chat protocol v4 ([chat-protocol-v4.md](../app/chat-protocol-v4.md)):
    a per-message ratchet over v3 bundles and trust
    ([chat-protocol-v3.md](../app/chat-protocol-v3.md)). A DM room seals and
    opens only v4 frames (a v3 frame there is ignored, and open rooms ignore
    v4). Each frame has one entry per approved device other than this one;
    a frame with no entry is still sent and the outbox re-seals it later.
    Own frames that come back (`own`) are not shown and not acknowledged;
  - strict trust in scope `direct:<id>`: bundle `acct` is the signed-in
    account (lowercased; a guest uses its node id), `node` the DM self id;
    a bundle whose `acct` is not one of the two participants is dropped;
  - a further device of an account that already has an approved device is
    pending: nothing is sealed to it, its frames are held (up to 64 per
    room, persisted with the trust records; frames over 96 KiB stay in
    memory only so the secure store stays small), a system line and a
    banner with Approve and Block appear; approving opens the held frames;
  - edits and deletes must come from the author's `node`, reactions from
    their `reactorId`, which must also be the frame's `node`;
  - key advertisements are never acknowledged; a frame not wrapped for this
    device triggers one persisted advertisement;
  - up to 64 own durable frames are kept 7 days and re-sealed with
    `resend: true` when a new device is approved, followed by
    `forkmesh-superseded`. The outbox belongs to the conversation, not the
    socket, so it survives a reconnect: own frames can no longer be reopened
    from the relay's replay to rebuild it. An entry is complete when it is
    wrapped for every approved device other than this one;
  - `noSession(kid)` (no session, prekey gone, more than 256 skipped keys or
    a failed unwrap): at most once per room and device every 60 s the phone
    deletes its sessions with that device, sends a persisted, non-notifying
    `{"type":"e2ee-reset","senderId":<node>}` sealed only to it, and one
    persisted key advertisement. It skips this when it has already opened a
    later frame (higher `ctr`) from that device: the frame is a leftover from
    an older session and a reset would break the working one. An opened
    `e2ee-reset` is acknowledged, not shown, and re-seals every outbox entry
    to that device alone (`resend: true`, no `forkmesh-superseded`, because
    the original still serves the other devices). Over the mesh, with no
    relay socket, the reset goes back as `direct-sealed`;
  - there is no history-replay exception in DMs: a frame already opened is
    `replay` because its message key is gone. DM history is not kept on
    disk, so after a restart the phone shows only frames still waiting on
    the relay for it; its own earlier messages are not shown again;
  - Safety number (room menu): 60 digits in 12 groups, the QR string as a
    QR code, "Scan their code" with the camera scanner used for pairing,
    and "Mark as verified"; any change to the approved set clears verified
    and adds "Your safety number with <name> changed";
  - direct delivery: every DM frame type the phone sends (text, files,
    edits, deletes, reactions, thread replies, view-once) also goes as
    `direct-sealed` to each linked approved device, sealed to that device
    only; files over 256 KiB skip Bluetooth links. Calls and receipts stay on
    the relay. Focus is "app in the foreground, that DM on screen, the
    channel drawer closed"; when this phone and a linked device of the other
    person both have focus, the frame goes only over the mesh and the relay
    copy follows after 3 s without a `direct-ack` or when the link drops. The
    route shows under the name: "Direct · Wi-Fi", "Direct · Bluetooth" or
    "Direct only";
  - DM notifications decrypt on the phone: a `direct-messages` frame names
    `conversation` and `ref`; the service reads
    `/api/chat/direct-messages/<id>/pending?ref=<ref>` (side-effect free),
    decrypts with `RoomCrypto` and opens the v4 frame with `commit = false`
    (`RoomSeal.existing`: no ratchet step, counter or held frame stored, so
    the DM socket can still open the same frame later); a pending or unknown device
    gives the generic notification. One `MessagingStyle` notification per conversation,
    lock-screen version "New message". The "Notification content" setting
    (`notifications/content`) chooses name and message, name only, or neither.
    Any failure falls back to a generic notification.
  - every notification that can carry someone's words is `VISIBILITY_PRIVATE`
    with a generic public version ("ForkMesh" and "New message", or "New
    notification" for pings and desktop alarms), so a locked phone shows
    neither who wrote nor what (2026-10-22; before, only the decrypted DM
    preview had one). Nearby `#mesh` alerts (`PingNotifier.postMesh`) follow
    the same "Notification content" setting through `MeshAlert.shown`: name
    only drops the text, neither shows "ForkMesh" / "New message"; before,
    they always showed the sender and the text.
  - every `direct-messages` frame, foreground or not, first posts
    `/api/chat/direct-messages/<id>/delivered?ref=<ref>`
    (`DirectMessagePreviews.confirmDelivery`), so the sender sees Delivered
    while this phone has no DM socket open
    ([chat-private-rooms.md](../app/chat-private-rooms.md#delivery-ticks)).
- **Relay room** (`C/chat/RelayRoom.kt`): every frame encrypted twice
  (`RoomCrypto` room key and a v3 `RoomSeal`, open trust, scope = room name),
  public `#channel` chat included, and the bundle carries `nsig` by the node
  key (section 35). A frame with a non-empty `to` other than `hello` is
  sealed to the devices whose bundle `node` is the recipient plus this
  account's own devices (`RelayRoom.sealRecipients`). The relay carries no
  DMs, private channels or invites (section 35). The `hello` and its replies
  list only `RelayRoom.advertisedChannels` (no hidden or `#office/` channel),
  and a peer's `hello` adds only `#` channels that are neither `#office/` nor
  hidden; a chat frame or restored history no longer brings a hidden channel
  back. v2 frames are ignored. Dial timeout 15 s; ping every 25 s; reconnect
  1, 2, 4, 8, 16, then 30 s; presence every 60 s, peers dropped after 180 s.
  Envelope `version` matches iOS.
- **RoomCrypto**: AES-256-GCM, key PBKDF2-HMAC-SHA256(passphrase or app key,
  salt SHA-256("ForkMesh room:" + room)[0..16], 210000 rounds), cached per
  (room, secret).
- **RoomSeal** (`C/crypto/RoomSeal.kt`, `ChatV3.kt`, `ChatDeviceKeys.kt`,
  `ChatTrust.kt`): chat protocol v3. One Ed25519 identity per secure-store
  namespace (`chat/e2ee/ed25519IdentityV3`), signed prekeys X25519 +
  ML-KEM-768 (`MlKem768`, seed `d || z`) in `chat/e2ee/prekeysV3`, a new one
  every 24 h, private halves wiped after 8 days, at most 10. The prekey list
  stores only the private halves; public keys are recomputed on load. The
  in-memory key set, trust records and counters are shared per underlying
  store and namespace (`ScopedSecretStore` wrappers compare equal), because
  two copies would rotate prekeys apart and lose frames. Counters are
  reserved 32 at a time so a send costs one store write per 32 frames and a
  counter is never reused after a crash. Held frames are only kept when the
  signature verifies under the frame's own key and the frame has an entry for
  this device, so junk cannot push real frames out of the 64 slots. The
  legacy v2 key `chat/e2ee/x25519PrivateV2` is deleted when the v3 identity is
  created. Vectors: `core/src/test/.../crypto/ChatV3VectorsTest.kt` reads
  `conformance/chat/v3/vectors.json` (copy in test resources when the repo is
  not reachable).
- **v4 ratchet** (`C/crypto/ChatV4.kt`, used by `RoomSeal` when the policy is
  strict): sessions per room and peer `kid`, at most 4 per peer (least
  recently used dropped, never the current one), 512 skipped keys per
  session, skipped keys older than 8 days dropped whenever that peer's state
  is stored, at most 256 keys skipped in one step. The state for a room
  lives in the secure store at `chat/e2ee/ratchetV4/<b64u(room)>` in the
  spec's canonical JSON, shared per store and room like the counters. Seal
  and open run under one lock per room and work on a copy of the peer's
  state; a seal writes the new state before it returns the frame, an open
  writes it and then the counter only when the frame opened, and a write
  failure leaves the stored and in-memory state unchanged. Replaced chain,
  root and message keys are zeroed. Vectors:
  `core/src/test/.../crypto/ChatV4VectorsTest.kt` replays
  `conformance/chat/v4/vectors.json` (copy in test resources) like
  `app/tests/test_chat_v4_vectors.py`; `ChatV4RatchetTest.kt` covers
  reordering, old chains, simultaneous starts, reset, `commit = false`,
  limits and reload from the store.
- **Local mesh** (`C/chat/LocalMeshRoom.kt`): mesh links v3. Encrypted
  multicast beacon (TTL 1) carrying only `{t, v:3, id, port, ts, reply}`,
  `id` a random value made at start and whenever the address set changes
  (section 35), direct TCP links, local-address checks before any
  socket, subnet broadcast and a paced /24 sweep where multicast is filtered,
  Bluetooth RFCOMM, and a BLE scan for the iPhone's GATT service
  (`C/chat/MeshBle.kt`, wire format in
  [networking.md](../desktop/networking.md)). Needs a Wi-Fi multicast lock.
  Every link starts with plaintext `fm-link-hello` and `fm-link-auth`
  frames (length-prefixed like every frame) and then carries AES-GCM frames
  with per-direction counters; a bad frame closes the link. Peers are keyed
  by the authenticated kid; names travel in the sealed mesh `hello`. The
  phone forwards every new `#mesh` bundle to its other links and relays
  sealed frames it cannot open to links whose device is addressed. It no
  longer re-seals its earlier messages to a device it learns about later
  (section 35): a late joiner gets only frames already sealed to it. The
  history file (v2) keeps each message's original sealed frame, its own
  included, and backfill resends only those. The same links carry `direct-sealed`, `direct-ack` and `focus`.
- **Bluetooth stays off the air during calls** (`C/net/RadioPolicy.kt`,
  `A/platform/AudioActivityProbe.kt`). The mesh search pass (paired devices,
  classic inquiry up to 15 s, a 15 s BLE scan, every 2 minutes) and Glasses
  Alert's continuous BLE scan share the phone's one radio with Bluetooth
  headsets and, on combo chips, 2.4 GHz Wi-Fi. At full duty they stole enough
  airtime that another app's call (Signal, reported 2026-10-19) went silent
  in bursts. The audio mode decides: any mode other than `MODE_NORMAL`
  (ringing, a phone call or a VoIP call from any app) is a call, otherwise
  `isMusicActive` means audio is playing. During a call the mesh skips the
  whole pass and checks again every 10 s; an inquiry or BLE window already
  running ends within a second of the call starting. While audio plays it
  skips the inquiry and scans in `SCAN_MODE_LOW_POWER`. Paired devices are
  dialled only when their class is computer, phone or uncategorised (as the
  inquiry already filtered), never headsets, car kits or watches. Glasses
  Alert scans in `SCAN_MODE_LOW_POWER` while audio plays or a call is up
  (re-checked on its 5 s refresh tick) and drops the census scan during a
  call.
- **Node sync** (`C/chat/NodeSyncEngine.kt`, `NodeSyncApplier.kt`):
  `GET /api/sync` is destructive, so the phone stands by while a desktop node
  is enrolled. Poll 60 s, backoff 2 s to 30 s with ±25% jitter. Requests sign
  `forkmesh-issues-pull-v1\n{owner}\n{ts}`. An item is acked only after it is
  durably committed. Written formats match the desktop's stores and
  `server/intake.go` (issue JSON with four-space indent and sorted keys;
  pulls only on `forkmesh/pulls`).
- **Chat privacy** (General settings, `A/feature/chat/ChatPrivacy.kt`):
  "Hide chats in the app switcher" (on by default) sets `FLAG_SECURE` on the
  activity; "Lock chats with fingerprint" (needs a screen lock) asks with
  the framework `BiometricPrompt` (fingerprint, face or screen lock) on
  Android 9 and later and the keyguard credential screen before, when Chat
  opens after the app was in the background. Chat history, the mesh history
  and the secure store (keys, trust records, held frames) are excluded from
  cloud backup and device transfer (`res/xml/data_extraction_rules.xml`,
  `backup_rules.xml`); `projects/` is excluded whole because rules cannot
  match the per-project `chat-history` folders.
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

- **Notification intent token** (`C/security/IntentToken.kt`,
  `platform/IntentTokens.kt`, 2026-10-22). `MainActivity` is exported, so any
  app could send it `com.forkmesh.inbox.ACTION` (Deny, Retry, Continue run and
  Mark read ran without a prompt) or `com.forkmesh.call.ACCEPT`. Every
  PendingIntent the app builds for these (Inbox notification actions, the
  call notification's show, accept and decline) now carries
  `com.forkmesh.intent.TOKEN`, 32 random bytes made once per install and kept
  in the private `forkmesh.intents` preferences. `InboxFeature.onIntent`,
  `NotificationActionReceiver`, `DirectCallFeature.onIntent` and
  `CallActionReceiver` compare it in constant time and drop an intent without
  it.
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
- **Router** (`feature/openwrt/`, core `openwrt/`) talks to an OpenWrt router
  through `RouterHttp`, a small HTTP/1.0 client on a plain or TLS socket, not
  `HttpURLConnection`: the network security config forbids cleartext except
  to the emulator host, and a domain rule cannot name every private address.
  The socket path is used only for addresses `OpenWrtModel.isLocalHost`
  accepts ([`docs/marketplace/openwrt.md`](../marketplace/openwrt.md#phones)).
- **Phone Security** (`feature/phonesecurity/`, core `phonesecurity/`) is the
  local checkup described in
  [`docs/marketplace/phone-security.md`](../marketplace/phone-security.md).
  `PhoneSecurityProbe` fills `PhoneSecurityFacts` on `Dispatchers.IO` when the
  page opens, on **Run again** and when the app returns from Settings; every
  read is wrapped so a refused one leaves its field `null` and the check reports
  Unavailable. The core scanner, `/proc` parsers, filters and summaries run on
  the host JVM (`./gradlew :core:coreTest -Pfilter=PhoneSecurity`). Root and
  Xposed manager packages are declared in the manifest's `<queries>` (no new
  permission); `PhoneSecurityCatalogTest` fails if one is missing. A fix starts
  its `android.settings.*` action (ForkMesh's own app details use a `package:`
  URI) and falls back to `Settings.ACTION_SETTINGS` when the phone has no such
  page. Android limits met: `lock_screen_allow_private_notifications`,
  `enabled_notification_listeners`, `private_dns_mode` and
  `/sys/fs/selinux/enforce` are hidden from apps on many builds, so those
  checks report Unavailable (Private DNS falls back to
  `LinkProperties.isPrivateDnsActive`); the hardware-keystore feature flags
  exist only from API 28 (StrongBox) and 31, so older phones are judged from an
  existing Keystore key's `KeyInfo`; Nearby devices is Unavailable before
  Android 12, which has no such permission; `/data/adb` is unreadable to apps,
  so the Magisk, KernelSU and APatch folder probe only catches what SELinux
  lets through. The relay rule also treats `10.0.2.2`, the emulator's alias for
  the host, as loopback, matching `AppEnv.isLoopbackHost`.
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
- **Marketing Reddit desk.** `A/feature/market/MarketingScreens.kt`
  (`redditSection`) lists the team's Reddit accounts under "Your accounts"
  (`redditDesk.accounts`, each opening u/name's posts), edited in the
  REDDIT_WATCH form's Accounts field (one per line, up to 10; a name, `u/name`
  or a profile link), sent as `reddit.accounts` in the same settings PUT as
  subreddits and searches. Each thread row has a second line from
  `MarketingRecord.redditActivity` in `C/market/MarketingWorkspace.kt`
  (kept in core so it is unit tested): comments, points and views, the
  watched comment's views and points, "views show only to u/…" when Reddit
  hid them, the account the extension read as, and when it last read.
- **Sign in** is per phone (`packages/sign-in/on`, on until removed, whatever
  the pack); Settings always keeps Sign out and Log in so nobody is locked out.
- **Market.** `A/feature/market/MarketScreens.kt` and `C/market/MarketModels.kt`
  offer the web Market's sorts and filters, usage from
  `/api/market/installs` and `/api/market/reviews/summary`, and one-tap
  install (`MarketModel.cardAction`). Desktop-only packages
  (`PackageInstallMode.DESKTOP`) offer no phone install. The list pads the
  safe area and insets so the last row is reachable.
  Sorts include Most opened and Installed first, as on iOS. Installed cards
  show Uninstall and each package page has a ⋯ menu with Rate, Send feedback
  or report a bug, and Uninstall, the uninstall driven by
  `MarketModel.uninstallAction` and confirmed through
  `A/feature/market/PackageUninstall.kt`: account packages leave every
  device, per-phone and organization packages with a phone switch hide on
  this phone, and the organization-wide removal stays on the package page's
  Organizations section.
- **Plugin ⋯ menu.** Every plugin's own screens carry the same ⋯ at the top
  right of their app bar (user request, 2026-10-15). Since 2026-10-18 it
  opens one small popup instead of a menu of separate pages (user request):
  the plugin's name, a row of stars, one text box, "Send to agent" and
  "Send", then Uninstall under a divider (one row per organization for
  organization packages). The rules match iOS (`docs/native/NAVIGATION.md`,
  Plugin pages) and live in `C/market/PluginFeedback.kt`: Send saves a tapped
  rating and files any text as package-tagged feedback; below five stars the
  text is the review's problem and a second box asks how to improve it; Send
  to agent opens the composer (`NavigationFeature.compose`) with a prompt
  naming the plugin and carrying the text, and files nothing. A screen opened through
  `NavigationFeature.screen(route)` is tagged with its package
  (`Screen.pluginPackage`, from `C/market/PackageMenu.kt`, which reuses
  `PackageGate.ROUTE_KINDS`); `NavigatorHost` and the shell's tab stacks
  provide the screen as `LocalScreen`, and `ToolsAppBar`, `CodeAppBar` /
  `DetailScaffold`, `RoutePage`, `CollabTopBar`, `RssScaffold` and
  `OrgScreenHeader` draw `PluginMenuButton()` for a tagged screen, so new
  plugin screens get the menu without extra code. Tab roots (Agents, Inbox,
  Chat), the composer and the shared repository page reached through commit
  links are not tagged. Screens that already had a ⋯ (note editor, event,
  CRM, CRM contact, Coves, Special agents) pass `pluginMenu = false` and add
  `PluginMenu.Items(divided = true)` (one "Rate, report or uninstall…" row
  that opens the popup) under their own items, so each screen
  keeps one ⋯. Screens a plugin pushes directly (not by route) carry no menu;
  the plugin's entry screen does. `A/feature/market/PluginMenu.kt` loads the
  catalog and the viewer's review only when the popup first opens, and the
  feedback it sends goes to `POST /api/bug-reports` with `package` (left out
  when there is none) so the public issue is tagged with the plugin. The full
  `RateScreen` and `ReportBugSheet` stay on the Market's package page. Feedback
  never depends on the Bug reports package being installed. After a
  successful uninstall the plugin screen pops; an organization removal waits
  for its export to be saved first, and a failure shows a dialog.
- **Face Changer.** A native feature, not the web page. `C/facechanger/`
  holds `FaceChangerLook` (face, background and voice choices, catalogs,
  preference keys, change listeners). `A/feature/facechanger/` holds the rest:
  `FaceChangerFeature` (the one look, restored from and saved to the
  preferences and photo files, the shared `face-changer` worker thread and the
  `WebCallPeers` factory hosting `direct-call.html`), `FaceChangerControls`
  (the Face, Background and Voice buttons and pickers), and
  `FaceChangerScreen`, which previews the camera after the runtime prompt and
  records a clip into the `face-changer-share` cache path for an
  `ACTION_SEND` chooser. Direct calls (`A/feature/chat/DirectCallFeature.kt`)
  build their engine on that look, factory and worker and show the same
  controls; the conversation header no longer has a preview (eye) button.
  `docs/app/chat-direct-calls.md` (Face Changer on the phones) has the
  details.
- **App Store Status.** `A/feature/appstorestatus/` draws
  `/dashboard/app-store-status` natively (Insights group, route
  `appStoreStatus`). It reads `GET /api/app-store-status` through
  `MarketFeature.client()`, so the session and the package API share one
  client; `C/market/AppStoreStatus.kt` decodes the snapshot leniently (a
  missing field takes its default, an unknown tone is neutral, unknown keys
  are ignored) so an older app keeps working after the report grows. Tones
  map to `danger`, `warning`, `success` and `textTertiary`. Pull to refresh
  and the Refresh button only re-read the stored snapshot: a newer one needs
  the desktop's **iOS App Store status** shortcut to run
  ([App Store Status](../marketplace/app-store-status.md)). TestFlight public
  links open in the browser, and only `https://` links are offered.
- **Usage.** `A/feature/usage/` draws `/dashboard/usage` natively (Insights
  group, route `usage`, package `usage`) from the desktop's `agentops.usage`
  through `AgentUsageModel`, with the account actions (use for new agents,
  add, rename, delete, log in and out, terminal as an account) and the
  agent-in-a-terminal form; tapping the Usage card on Desktop agents opens it
  ([Usage on Android](../marketplace-usage-feed-mastodon.md#usage-on-android)).
- **Shortcut steps.** `C/teammates/ShortcutsStore.kt` decodes the optional
  `steps` of each `insights.shortcuts` entry into `ShortcutStep` (name, and
  `shortcut` when the step runs another shortcut). Decoding is lenient: a
  missing or non-list `steps`, non-object items and items with no string
  name are dropped (a step with only a `shortcut` is named after it), and at
  most 32 are kept, the desktop's own limit ([shortcuts](../desktop/shortcuts.md)).
  `ShortcutCatalog.target` resolves a link to an entry in the same scope (the
  exact file, else the file without its extension) and never to the shortcut
  itself, so a step pointing nowhere shows as plain text. In
  `A/feature/teammates/ShortcutsScreen.kt` a row with steps shows `N steps`
  under its details, leaving the Run button where it was. The shortcut
  detail (the section below the list) draws the steps as a numbered
  timeline. Circles are sized in `sp`, so they grow with the font scale. Rows
  are at least 48 dp tall, and names wrap rather than truncate on narrow
  phones. A linked step is drawn in `accent` with a chevron. Tapping it opens
  the linked shortcut's detail and scrolls to it, two frames after the text
  loads so the list has been laid out. TalkBack reads each step as one node,
  for example "Step 2 of 7, Deploy Android build, opens shortcut".
- **RSS Reader** (`rss-reader`, route `rssReader`, Personal group;
  `/dashboard/rss`, `forkmesh://rss`, `forkmesh://rss-reader`). Local only:
  `C/rssreader/` holds everything testable on the host JVM (parsing, the
  guid rule, sanitizing, discovery, reader extraction, OPML, the library and
  storage) and `A/feature/rssreader/` the Compose screens and
  `RssReaderService`. Nothing is sent to an account; the only traffic is the
  feed, page and icon requests ([RSS Reader](../marketplace/rss-reader.md)).
  - **XML**: an in-tree forgiving parser (`RssXml`), identical on the JVM and
    on Android, that copes with HTML entities, bare `&`, and undeclared `dc:`
    / `content:` prefixes; it skips `<!DOCTYPE …>` without reading it, so
    there are no external entities and no XXE.
  - **Guid**: guid, then link, then `rdf:about`; else `title|published-ms`
    (just the title when the feed gave no date); Python's whitespace set is
    collapsed and the result cut to 600 code points
    (`RssParserTest.testGuidRuleMatchesTheContract`).
  - **Storage**: JSON under `filesDir/rss-reader/` (`library.json` for feeds,
    folders, settings and activity; `articles/<feedId>.json`;
    `icons/<feedId>`), written to a temporary file and renamed; saves are
    debounced and rewrite only what changed.
  - **Fetching**: `RssUrlConnectionHttp` follows up to five redirects by hand
    so permanent ones (301/308) land in `previousUrls`, sends conditional
    GETs, caps feeds at 2 MB and sends `ForkMesh-Reader/1.0 (Android)`.
    Refresh runs when the reader opens and on pull to refresh through a
    six-thread pool, with no timers; a feed is due 30 minutes after its last
    check and failures back off up to a day.
  - **Cleartext** stays off (`network_security_config.xml` unchanged): an
    http feed is fetched over https and keeps its http address (`viaHttps` in
    Feed Info); when https fails too the feed's error is `http_only`.
  - **Article view**: a `WebView` with JavaScript, file and content access,
    DOM storage and geolocation off; the HTML is sanitized again from the
    allowlist and carries a `default-src 'none'` CSP. With Load Remote Images
    off, images and media become links and the CSP drops http(s). Link taps
    open the browser through an `ACTION_VIEW` Intent; the feed icon is a
    `data:` URI from the on-device cache.
  - **Appearance**: Light or Dark wraps the reader in
    `ForkMeshTheme(dark = …)`; Automatic follows the app.

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
- **Message long press** (`messageActionsFlow` in `A/feature/chat/ChatSheets.kt`):
  a row of quick-reaction circles, then 44 dp action rows in three groups
  (sticker, thread, edit, copy, select; prompt, issue, profile; delete) that scroll
  when the sheet hits its 9/16-screen cap. The old 60 dp rows with chevrons
  cut off Delete on small phones. The circles and the reaction chips under
  messages are a translucent `textPrimary` wash with a border: their old
  `bgRaised` fill was invisible on the white sheet in light mode.
- **More reactions** (`ChatReactionPicker`, list in `ReactionCatalog.groups`):
  about 320 emoji in seven tabbed groups. The 18 catalog emoji are sent as
  their slug (`valueForEmoji`); any other is sent as the literal emoji, which
  desktop, iOS and web already accept and show as text (up to 16 code points).
- **Stickers** (`StickerCatalog`, `ChatStickers`): tilted badges on a message's
  top-end corner, sent as reaction frames; see [chat-stickers.md](../app/chat-stickers.md#android).
- **Chat list** (`A/feature/chat/ChatChannelList.kt`): server DMs listed newest
  first with unread pills and avatars; each row has a last-line preview built
  only from messages on the phone (`C/chat/ChatListPreview.kt`).
- **Direct calls** (`A/feature/chat/DirectCall*.kt`, `PreCallScreen.kt`,
  `CallRinger.kt`, `IncomingCallNotifier.kt`; engine `C/chat/DirectCalls.kt`;
  protocol in `docs/app/chat-direct-calls.md`).
  - **Pre-call check.** Voice call and Video call in the conversation header
    no longer ring at once. After the permission prompt they set
    `DirectCallFeature.precall`, and `DirectCallOverlay` draws
    `PreCallScreen` as a full-screen layer, not a Navigator screen, so the
    call screen can replace it without leaving a screen in the stack. It is
    refused while a call is active or `WebCallPeers.busy`. The screen opens a
    preview through `WebCallPeers.preview(…, media, owner = "precall")`:
    video uses the camera with the look's face, background and voice; voice
    uses the microphone only (`media` "audio") and shows the avatar. The
    factory's WebView is attached from the first frame, under the avatar
    for voice, because a detached WebView never starts `getUserMedia`. A
    spinner shows while it opens, and a failure shows a message, Try again
    and a Call button that still works. Face, Background, Voice and Hear
    myself match the Face Changer screen. "Record test clip" records up to
    10 s (countdown, auto-stop) through the page's `record`, saves it under
    `cacheDir/precall-test`, then plays it back at once (`VideoView` for
    video, `MediaPlayer` for audio) with Replay, Re-record and Discard. The
    folder is emptied on the worker when the layer goes away.
    `WebCallPeer.finishRecording` keeps `audio/*` mimes now, because a voice
    preview records an audio clip. Call runs `engine.start` on the worker,
    and the engine listener clears `precall` as soon as a phase exists.
  - **Handoff.** `makePeer` reuses the previewing peer whenever the preview's
    media equals the call's media (video as before, and now voice too), so
    the camera or microphone is not reopened. Leaving the pre-call layer ends
    the preview only when no call is active, checked on the worker after the
    queued `start`. A preview is tagged with its owner; when a call ends
    without having taken the preview (declined or missed while ringing),
    `DirectCallFeature` calls `endPreview("precall")`, which leaves a Face
    Changer screen preview alone. `FaceChangerFeature` skips face and
    background updates for an audio preview.
  - **Live quality widget (2026-10-22).** `direct-call.html` posts a
    `quality` event about once a second (the web meter in
    `app/public/call-quality-meter.js`). `WebCallPeer` parses it with
    `C/chat/CallQuality.kt`, which mirrors the web thresholds (RTT
    150/300/500 ms, loss 1/3/8 %, jitter 30/60/100 ms), labels and text,
    and `WebCallPeers.quality` keeps a `CallQualityHistory` for the current
    preview or live peer: the last 50 voice levels (5 s) and the last 60
    live stat samples (1 min). It resets when a new peer opens and when no
    peer is left; a preview handed to the call keeps its levels.
    `A/feature/chat/CallQualityWidget.kt` draws a small dark translucent pill
    at the top start of the call screen, under the name and state lines,
    away from the self-view in the bottom-right corner and the buttons: four
    signal bars in the grade colour (on-dark success, warning, danger), a
    20-sample mirrored waveform (your mic up in accent blue, the other side
    down in grey) and the RTT. Tapping it opens a card with Voice, Network
    (RTT line, amber loss ticks from 1 %, red from 8 %) and, on video
    calls, Video in and Video out rows; the open state is the
    `fm.call.qualityOpen` pref, shared with the web key name. Before the call
    connects, and on the pre-call screen, the page sends `live:false`, so the
    pill shows only the mic waveform. The pre-call screen hides it while a
    test clip plays. The Face Changer studio (`FaceChangerScreen.kt`) shows
    the same mic-only pill under its hint line while the preview is ready or
    recording, because it captures audio and video too. Android has no
    group-call screen. Tests:
    `C/chat/CallQualityTest.kt`.
  - **Ringback.** `CallRinger` listens to the engine on the main thread.
    While the phase is RINGING (only a caller rings) it plays
    `ToneGenerator(STREAM_VOICE_CALL, 80)` `TONE_SUP_RINGTONE` and stops it on
    any other phase, which covers connect, decline, missed and hang-up. The
    `ToneGenerator` is released by `ProjectLifecycle.onClose` and by
    `DirectCallFeature.uninstall`.
  - **Incoming calls.** While an invite is pending and no call is active,
    `CallRinger` plays the default ringtone (`RingtoneManager.TYPE_RINGTONE`)
    with a looping `MediaPlayer` (`USAGE_NOTIFICATION_RINGTONE`) and a
    repeating vibration, following the ringer mode: silent plays nothing,
    vibrate only vibrates. While the app is not in the foreground it also
    posts `IncomingCallNotifier`'s notification on the "Incoming calls"
    channel (`IMPORTANCE_HIGH`, no channel sound or vibration because the app
    rings itself): `CATEGORY_CALL`, ongoing, public on the lock screen, a
    full-screen intent to `MainActivity` (`com.forkmesh.call.SHOW`), and
    Accept (`com.forkmesh.call.ACCEPT`, an activity intent routed through
    `Features.onIntent` to `DirectCallFeature.onIntent`) and Decline (the
    manifest receiver `CallActionReceiver`). Both carry the call id and the
    install's intent token (section 10) and act only on that invite; Accept from the notification accepts at once only
    when the microphone (and camera) permissions are already granted,
    otherwise it opens the in-app incoming screen. On API 31+ the
    notification uses `Notification.CallStyle.forIncomingCall` and falls back
    to plain actions if posting it fails. On API 34+ the full-screen intent
    is used only when `canUseFullScreenIntent()` is true; otherwise the
    notification is a heads-up with no CallStyle. The notification is not
    posted in the foreground (the in-app screen is showing) and is cancelled
    when the app comes forward. The ring, vibration and notification stop on
    accept, decline, a cleared or stale invite, or once a call is active.
    The manifest declares `USE_FULL_SCREEN_INTENT`.
  - **Lock screen.** `CallRinger.window` drives `CallWindowEffect` in
    `DirectCallOverlay`: `setShowWhenLocked` while an invite rings or any
    call is active (so an answered call stays visible over the keyguard) and
    `setTurnScreenOn` only while ringing; both reset afterwards (window flags
    on API 26).
  - **Incoming screen.** The small top banner is gone. `DirectCallOverlay`
    shows a full-screen incoming UI: caller, "Video call" or "Voice call",
    and large red Decline and green Accept buttons (Accept goes through the
    permission prompt). It sits above the pre-call layer, which stays
    composed underneath so its preview survives a declined invite.
  - **Route note.** The page posts a `route` event (`direct` or `relay`)
    when the call connects and when the selected ICE pair changes.
    `WebCallPeer.onRoute` feeds `WebCallPeers.route`, a `StateFlow<String>`
    that is reset when a peer is made and when the live peer is destroyed,
    so `DirectCallPeer` is unchanged. Once live, the call screen shows
    "Direct · peer-to-peer, no server in between" or "Relayed · through a
    TURN relay server (third party)" under the status line, and the LIVE
    status line reads "Connected · end-to-end encrypted" (relayed media is
    still encrypted end to end). The pre-call layer says "Calls go directly
    between your devices when possible."
  - **Call rings** (since 2026-10-21; protocol in
    `docs/app/chat-direct-calls.md`). `DirectMessageService.sendCallFrame`
    marks every `call-invite` envelope `ring: true`, and the relay sends the
    callee a `calls` account event naming the conversation
    (`CallRings.conversation`, `C/push/CallRing.kt`). In the foreground
    `ChatDepth` hears it through `AccountEventChannel.onEvent` and calls
    `DirectMessageService.openRinging`, which reloads the list if the
    conversation is unknown and opens its room socket, so the next resent
    invite (every 3 s) brings up the incoming screen. In the background
    `BackgroundLinkService.ringCall` handles it: when the ringing project is
    the active one and `DirectCallFeature.wake` opens the DM, the normal
    ringer and call notification take over, and if no invite has shown up
    after 6 s (`CallRings.WAKE_WAIT_MS`) it falls back to
    `IncomingCallNotifier.postRing`. When the call engine is not running (the
    process was started for the background link only, or another project is
    active) it posts that notification at once. It goes on the "Incoming call
    alerts" channel (`incoming-call-rings`: the default ringtone and a
    vibration pattern, `IMPORTANCE_HIGH`), `CATEGORY_CALL`,
    `FLAG_INSISTENT` so the ringtone repeats, `setTimeoutAfter(45 s)`, a
    full-screen intent and an Answer action that open the DM through the
    same `PushTarget` a DM notification uses (`/chat/direct-messages/<id>`).
    The caller's name comes from `ApiService.directMessagePartner`.
    `CallRinger` cancels it as soon as an invite or call exists or the app is
    in front.
  - **Limits.** A ring needs the background connection to be on and
    connected (it is paused by Doze and network loss; there is no FCM), so a
    force-stopped app does not ring. The ringback tone follows the voice-call
    stream, so it is quiet through the earpiece on some phones.

### Chat rows ported from iOS (SYNC.md S022 to S027)

- **Chat history file (S024).** The file is
  `chat-history/relay-<scope>.json` in the project's support folder, and its
  scope is now `ChatHistoryCodec.projectScope(projectId, room)`
  (`"project-" + scopeName(projectId, room)`, the project id falling back to
  the relay URL), the same name iOS uses, so a relay URL change or a move to
  another relay host no longer starts an empty history. On first read the
  store adopts the files kept under `formerScopes(serverUrl, room)` (the URL
  hash of the current URL, the retired `rooms/general` path and the
  production aliases, as on iOS), merges them newest file first, writes the
  project file and renames each old one to `<name>.migrated`. A file that
  does not parse is renamed `<name>.unreadable-<ms>` and history starts
  empty; a file that exists but cannot be read (permissions, a locked
  volume) blocks every save to that path until a later read succeeds, so it
  is never overwritten. The codec accepts any `v >= 1`, as iOS does, so a
  file from a newer build still loads.
- **What the file holds on Android.** Relay rooms as before, plus direct
  messages (`direct:<id>` conversations) with their `receipts` (`delivered`
  or `read`, the iOS wire names) and Office `#office/<name>` channels. Office
  messages restored from disk are replaced in place when the live copy
  arrives. Android writes the iOS keys it has data for and leaves out the
  ones it has no feature for yet (`removed`, `looks`, `lookEvents`, the
  quoted-reply `r` field); iOS reads a file without them, and both apps
  ignore keys they do not know.
- **Receipt times and Message Info (S022).** `DirectMessageService` stamps
  `ChatReceiptTimes` (delivered, read) from its clock the first time a
  message reaches each mark, as iOS does (the relay frames carry no time);
  restoring from disk never stamps, and evicting or removing a receipt drops
  its times. They are saved as `receiptTimes` (`{"delivered", "read"}` epoch
  ms) beside `receipts`. In the transcript a mostly-horizontal leftward drag
  on a message (1.5 times more sideways than vertical, so scrolling and the
  long-press menu are untouched) pulls the Message Info page in from the
  right while the transcript eases 30% left; it opens past 30% of the width
  or on a fling and springs back otherwise, and Back or a rightward swipe
  closes it. The page content, labels and colours come from
  `ChatMessageInfo.page` and match iOS (`MessagesMessageInfo.swift`); the
  dates use the iOS wording ("Today at 11:04 PM", seconds underneath). Under
  the latest own message in a direct conversation a small line reads
  "Delivered 11:04 PM" / "Read Yesterday 3:20 PM" (just "Delivered"/"Read"
  without a recorded time, hidden while sending or only sent); tapping it
  opens Message Info. Android rows are still the list style until S011
  brings the iMessage bubbles, so the swipe and line sit on those rows.
- **A notification tap reveals the conversation (S025).** Opening a
  conversation from a notification or a route goes through
  `ChatHub.switchConversation`, which counts `revealRequests` like iOS
  (picking a room inside Chat does not). `ChatDepth.revealRequests` carries
  the count to `ChatScreen`, which then closes the channel drawer, the
  Message Info page and the sheets and dialogs it opened (their coroutines
  are cancelled, which removes the overlay, since `FmMessenger` has no
  dismiss call). Pushed pages were already popped by the push and route
  handlers.
- **Route label, own-device notices and safety scans (S026).** The
  conversation header already showed the route label ("Direct · Wi-Fi",
  "Direct · Bluetooth", "Direct only"); its detail line now joins its parts
  with " · " like iOS. The new-device banner uses `DirectTrustNotices`: a
  pending device on your own account (its account equals this device's
  seal account) reads "A new device was added to your account. Approve it if
  it is yours, otherwise block it.", anyone else's keeps the "wants to join
  this chat" line. Scanning a safety-number QR returns a `DirectSafetyScan`
  with the iOS wording: verified, not a ForkMesh safety number, a
  different conversation (the scope between `forkmesh-safety:3:` and the
  last `:` differs), or a mismatch.
- **Message from People opens the account's DM (S027).** People's Message
  button opens `Route.Dm(name)`, which reaches chat as "@name".
  `ChatHub.switchConversation` treats "@name" as an account, as iOS does (a
  matching roster peer gives its verified owner account, section 35): it
  selects that account's existing direct conversation, or starts one with
  `POST /api/chat/direct-messages`. A guest, or no direct-message service
  (signed out), opens nothing and shows a notice; there is no relay `@peer`
  path any more. `Route.Dm` also accepts a `direct:<id>` conversation
  unchanged.
- **Direct messages across a sign-out (S024).** `ChatHub.directSessionChanged`
  mirrors iOS: on sign-out the hub holds the service's DMs and receipts
  before `DirectMessageService.clear()` empties it, a save while signed out
  keeps them, and the next sign-in restores them only for the same account
  (compared trimmed and lower-cased); another account drops them.

### Message bubbles (SYNC.md S011)

- **Which rooms.** `ChatHub.isConversation` is iOS's rule: server DMs
  (`direct:<id>`) and private office rooms that are neither server-visible
  nor read-only (relay DMs and relay private channels are gone, section 35). Those draw as
  bubbles (`ChatConversationList.kt`, `ChatBubbles.kt`); public channels,
  office channels and `#mesh` keep the Slack-style rows.
- **Layout.** `ChatConversationLayout` (core) ports iOS's: a bold time line
  after a 15-minute gap or a new day ("Today 3:42 PM", "Yesterday …",
  the weekday within a week, "Oct 1 at …", then with the year), runs of one
  sender joined at 5 dp corners on the sender's side and 2 dp apart (8 dp
  before a run), the sender's name at the start of a run in groups and the
  avatar beside the last bubble, own messages always carrying a receipt
  (Sent when nothing newer is known), and up to three emoji alone drawn at
  52 sp without a bubble. System lines (new device, own device) break runs
  instead of being bubbles. Emoji detection uses `BreakIterator` grapheme
  clusters and a table of Emoji_Presentation ranges, because
  `Character.isEmojiPresentation` needs Android 15 and core also runs on the
  JVM.
- **Bubble.** `ChatBubbleShape` is iOS's path (18 dp corners, a 5 dp inset on
  the sender's side where a tail used to be, cubic corners at 0.45, mirrored
  for own messages); there are no tails. Sent bubbles are 0A84FF with white
  text, received E9E9EB (light) or 26262A (dark). Over a backdrop picture a
  bubble is the shared glass (`fmGlass`) tinted with that colour at 88% for
  own and 50% for others, as iOS 26 tints `glassEffect`; on the solid canvas,
  where glass has nothing to bend, it is iOS's pre-26 sheen (a fill gradient,
  a top highlight, a hairline rim and a soft shadow), so the colours stay
  readable. The transcript behind bubbles is pure white or black like iOS.
- **Time, expiry ring and receipt inside the bubble.** Every bubble ends in
  the 12-hour send time, an 11 dp clock (`ChatExpiryRing`: a hand sweeping
  from 12 and a faint wedge for the time left of the room's retention,
  refreshed every minute, read as "Disappears in 5 hours") and the receipt
  mark, with a 4 dp dot before the time while an own DM is still stored on the
  server. The meta sits on the text's last line when it fits and drops under
  it otherwise; photos carry it in a dark capsule in the corner, and large
  emoji and view-once messages put it under the bubble.
- **Gestures.** Long press keeps Android's message actions sheet. A double tap
  (and a tap on the reaction badge) opens iOS's tapback picker: the quick
  reactions in a glass capsule over a dimmed page, with who reacted listed
  under it. Reactions show as a capsule badge on the bubble's outer top
  corner instead of chips. Swipe left for Message Info and the
  "Delivered/Read" line under the last own message work as before. The typing
  line becomes a received bubble with three pulsing dots.

### Live touch in direct messages (SYNC.md S023)

- **Wire format.** `DirectLiveEngine` and `DirectLiveFrames` (core
  `DirectLive.kt`) port iOS's: sealed `type: "live"` frames with `taps`
  (flat `[x, y]` in thousandths, `y` from the middle in widths), `ink` chunks
  (`s`, `p`, `c`, `w` in tenths, `e`), `motion` samples (`[dt, gx, gy, gz, ax,
  ay, az]` in hundredths of g against the frame's `ts`), `clear` and
  `motionOff`. Scaled numbers round half away from zero like Swift's
  `rounded()`, not Kotlin's half-even, so both phones send the same integers.
  `DirectMessageService.sendLive` sends over the mesh first, then the relay
  with `"live": true` and no `persist` or `notify`, dropping an envelope over
  16 KiB; the relay's `forkmesh-live` budget sets the pace
  (`windowMs * 4 / (perWindow * 3)`, at least 80 ms, 120 ms until it
  arrives). Received live frames never become messages and get no mesh ack.
  The engine batches on an `EventLoop` timer per conversation and prunes
  taps (1.4 s) and ink (45 s, fading over the last 5 s) once a second.
- **Tilt.** `DirectLiveMotionFeed` samples at 100 Hz like iOS. iOS reads
  CoreMotion gravity and user acceleration; Android reads the fused
  `TYPE_GRAVITY` and `TYPE_LINEAR_ACCELERATION` sensors (built from the same
  sensor fusion as the rotation vector, and already split into the two
  vectors the pose needs), falling back to the raw accelerometer with the
  same 0.1 low-pass iOS uses. Android reports reaction force in m/s²; the
  feed divides by -9.81 so a phone held upright sends `gy = -1` like an
  iPhone. Sharing runs while a direct conversation is on screen and the app
  is in the foreground; an impact of 1.4 g sends a centred tap (at most one
  per 500 ms).
- **Buzz.** A received tap always vibrates (a strong thud, then a 0.55
  click 90 ms later, `VibrationEffect` compositions where the motor supports
  them, a one-shot otherwise), even with haptics off in Settings, because the
  tap is the message; sending one gives a light tick. Received ink ticks only
  with haptics on.
- **Transcript.** A short tap on a direct conversation's transcript (not on
  a bubble) sends a tap at that spot; with the keyboard up the first tap only
  closes it. Ink and tap ripples (white for yours, pink for theirs) draw
  behind the bubbles at 30 fps while any are alive.
- **Drawing.** The pencil beside the conversation header opens the canvas:
  a glass toolbar at its top (current colour with a chevron that opens a 4 by
  2 colour grid, Clear, Done) and a "Tap to buzz. Draw on the background."
  hint. A drag past 8 dp draws a 5 dp stroke, a short touch sends a tap.
- **Partner phone.** In a server DM the header's room icon gives way to the
  partner's 3D phone (`DirectLivePhonePose`, 76 dp tall hanging out of the
  50 dp slot, drawn outside the glass so it is not clipped): the projected
  silhouette with real thickness, their avatar on a screen mapped by the face
  affine, the strongest bump of the last 260 ms held and faded, redrawn every
  frame while they share. It is grey and upright when they are not sharing
  and swallows touches. iOS removed the floating large 3D view and the
  phone's tap and long press on 2026-10-18, so Android has neither; TalkBack
  gets a "Send a tap" action on the phone instead.
- **Presence.** `ChatHub.peerPresence` (core `ChatPresence.kt`) finds a
  server DM's person on the roster by the nodes of the devices the DM trusts
  (`DirectDevice.node`), then by account owner, then by name; typing, a tap
  or shared motion from them, or a "Direct only" route, also counts as
  online. The header shows a blinking green light instead of the word
  "Online" (steady with reduced motion), and the DM list's dot uses the same
  rule, so server DMs no longer show Offline while the person is chatting.

## 15. You hub, diagnostics and docs

- **The hub drops down from the avatar** (`YouHubPanel`,
  `A/feature/navigation/NavigationScreens.kt`), as iOS `HomeShell.hubDropdown`
  and `Navigation/YouHubSheet.swift` do on a phone (sync ledger S013): a
  floating card 320 dp wide (at most 640 dp tall), 54 dp below the status bar
  and 12 dp from the right edge, 18 dp corners, a 24 dp shadow at 28%, over a
  20% black scrim that closes it. It unrolls from the top right with the iOS
  spring (damping 0.86, about 0.36 s). Inside: the avatar and name with the
  status text, a "Set a status" chip, the Diagnostics switch, the account
  actions as small chips (Settings, Sign in or Sign out; every other hub link
  is on Home and the bar, as on iOS), and the version capsule. What's new and
  Send diagnostics open as sheets over it. Until 2026-10-20 the hub was a
  full-height bottom sheet listing recents and every hub group.
- The hub's last row shows `versionName (versionCode)`, opens `WhatsNewPage`
  from the bundled `changelog.json` ([MAC_BUILD.md](MAC_BUILD.md)), and has
  Send diagnostics (the prompt points the agent at
  [PERFORMANCE_CHECKS.md](PERFORMANCE_CHECKS.md); `tools/PerformanceDiagnostics.kt`, `tools/AppChangelog.kt`,
  `feature/tools/AboutAndDiagnostics.kt`).
- **Docs package**: the public docs library from the same bundle iOS ships
  (`native/ios/ForkMesh/Resources/DocsLibrary.json`,
  `forkmesh-docs-library-v1`), copied to `docs/library.json` by the
  `bundleDocsLibrary` Gradle task. `C/docs/` parses, searches (desktop scores)
  and resolves links; `DocsMarkdown` renders a safe Markdown subset;
  `A/feature/docs/` holds the screens. Guests may open it. Tests:
  `DocsLibraryTest`, `NavigationTest`, `PackageGateTest`.

## 16. iOS look: glass and the floating tab bar

The phone shell follows the iOS app (`docs/native/NAVIGATION.md`, iOS
`Shell/ShellBottomBar.swift`, `Kit/FmSurfaces.swift`) instead of a Material
bottom bar.

- **Glass** (`A/ui/kit/FmGlass.kt`). `FmBackdropHost` records the page into a
  `GraphicsLayer`; `Modifier.fmGlass(shape)` on a sibling draws that layer
  shifted to its own position, blurred 24 dp and saturated 1.8x
  (`RenderEffect`, API 31+), then the iOS `FmGlassSurface` tint (bgRaised at
  66% light, 55% dark), a top specular gradient, a 0.5 dp edge and a 12 dp
  shadow. Below API 31, or with high-contrast text on, glass is a near-opaque
  fill (the iOS Reduce Transparency fallback). `Modifier.blur` is not used: it
  blurs the element itself, not what is behind it. No dependency is added.
- **Tab bar** (`GlassTabBar`, `A/shell/HomeShell.kt`). One glass capsule:
  12 dp side margin, 60 dp tall, radius 30, 52 dp cells, 24 dp icons (an SF
  Symbol at 20 pt draws about that size), 10 sp labels. The selected cell is an
  accent wash at 14% with radius 26 at the bar ends and 10 inside; selected
  icon and label are accent, unselected icons textPrimary and labels
  textSecondary. Pressed cells scale to 0.94 at 70% opacity. The bar sits
  `max(navigation bar inset, 34 dp) + 5 dp` above the bottom edge, the iOS home
  indicator gap.
- **Home tab.** Home comes first, as on iOS, and replaces the old More sheet.
  It is the hub grid (`ShellMenuGrid` with `plain = true`: six columns, no tile
  fills, uppercase section titles with a rule). Home is shell state, not an
  `AppTab`, so routes and stored tabs are unchanged; back at any tab root
  returns to Home, and the app opens on Home. The Home glyph is the filled
  `home` because the icon subset has no outline house (adding one needs
  `tools/subset_material_icons.py` and fonttools).
- **Page clearance.** Pages are laid out full height under the bar. A screen
  gets bottom padding equal to `ShellBar.clearance()` unless it sets
  `Screen.extendsUnderTabBar`; then it reads `LocalShellClearance` and pads its
  own list so rows scroll under the glass (Repos and Inbox do).
- The kit gallery (`--ez fm_kit_gallery true --ei fm_kit_section 10`) shows a
  glass capsule over colour stripes to check the blur.
- **Top bar** (`TopBar`, `A/shell/HomeShell.kt`), as iOS `Shell/ShellTopBar.swift`
  on a phone: no fill, 16 dp side and 4 dp vertical padding; the 40 dp project
  icon (`AccountFeature.ProjectSlot`, opens the project drawer; a long press
  closes the hub and palette and selects Home, as on iOS), the Glasses
  chip, then the new-agent button and the avatar (36 dp, corners 28% of its size, as iOS `AvatarWithDot(rounded:)`: the account picture or its initial, a 2 dp ring in the status colour 3 dp outside it, and the status emoji on its bottom-left corner; S014). A
  guest sees a person icon and a "Sign in" chip instead, and no guest banner.
  Search is not in the phone bar (iOS shows it only on iPad); it is on Home.
  The telemetry strip is the iOS one: 12 x 10 dp sparklines centred in the
  row, coloured by `TelemetryMetric.health` (the iOS warning and bad limits).
- **New-agent button** (`PromptButton`): a 44 dp accent gradient circle with a
  white plus and an accent glow that grows with running agents; each running
  agent adds a small four-point star (static; iOS animates them with the
  accelerometer).
- **Pushed-page header** (`FmNavBar`, `A/ui/kit/FmGlass.kt`), the iOS 26 inline
  navigation bar: page-coloured, 52 dp, title centred in `titleMedium`, a 44 dp
  glass circle with `chevron_left` for Back, actions on the right.
  `ToolsAppBar`, `CodeAppBar` and `RoutePage` all draw it.
- Glass inside a recorded page does not sample its own page (that would draw a
  layer into itself): `FmBackdropHost` clears `LocalFmBackdrop` for its
  content, so such glass uses the solid fill. Only the host's overlay (the tab
  bar) blurs.
- **Segment bars and action bars.** `FmSegmentTabBar` is a 46 dp strip whose
  tabs sit in a 12 dp glass panel (no fill, no hairlines), and
  `FmBottomActionBar` is a glass panel with 24 dp corners, 8 dp side and 4 dp
  vertical margin, as iOS `Kit/FmExplorer.swift` and `Kit/FmSurfaces.swift`.
- **Composer.** The prompt composer card and its slash menu use `fmGlass` with
  the composer's own translucent tint (iOS `Kit/PromptComposer.swift`, 28 dp).
- **Chat header**, as iOS `Features/Chat/ChatScreen.swift`: on the Chat tab the
  shell bar is hidden and the room draws one row of glass: a 48 dp channels
  circle, the room pill (icon, bold name, "n online • public"), a compose
  circle, the new-agent button and an avatar capsule. Android has no separate
  new-message picker, so compose opens the channels panel. The disappearing
  messages timer and the encryption note moved from the header into the room
  pill's menu, keeping the row to the iOS five controls.
- **Sheets** keep the iOS pre-26 look (sheetSurface, 12 dp corners, 35% and 55%
  scrim); a blurred sheet would need `Window.setBackgroundBlurRadius` on the
  sheet's own window.

## 17. iOS controls (sync ledger S005, S008, S009)

Android used Material's ripple, switch and icon button, which do not look or
feel like the iPhone app. Since 2026-10-20 the kit draws the iOS versions:

- **Press feedback.** `ForkMeshTheme` provides `FmPressIndication`
  (`ui/kit/FmPress.kt`) as `LocalIndication` and turns the Material ripple
  off (`LocalRippleConfiguration provides null`). A pressed control shows a
  flat wash, `textPrimary` at 8%, the iOS `FmPressStyle`, with no spreading
  circle. List rows (`FmListRow`) use `accentSubtle`, as iOS settings rows do.
- **Switch.** `FmSwitch` is the iOS switch: a 51 x 31 dp capsule, accent track
  when on and grey (`#787880` at 16% light, 32% dark) when off, a white 27 dp
  thumb with a soft shadow that springs across, and the selection haptic. The
  Material switch took its "on" colour from `primary`, which is `textPrimary`
  here, so it was black in light mode. Every switch in the app goes through
  `FmSwitch`.
- **Checkbox.** `FmCheckbox` is accent-filled with an `onAccent` tick (iOS uses
  accent check circles).
- **Icon buttons.** `FmIconButton` matches iOS `FmIconButton`: a 48 dp target,
  24 dp glyph in `textPrimary` by default (Android had 20 dp in
  `textSecondary`), a 40 dp circular wash at 8% while pressed, and 38% opacity
  when disabled.
- **Kit details (S008).** Filled accent buttons and dialog confirm buttons
  have white labels in both themes (iOS `FmToneButton`, `FmDialogActions`;
  Android used `onAccent`, near-black in dark mode); confirm dialogs have
  14 dp corners; `FmBottomBar` is the iOS glass capsule (50 dp, 8 dp side and
  4 dp vertical margin) instead of a flat strip with a top rule.
- **Copy and send to agent (S008).** As on iOS (`Kit/FmMessageTools.swift`),
  messages carry two 13 dp icons, copy and send to agent
  (`ui/kit/FmMessageTools.kt`): `FmEmptyState`, `FmInlineStatus`,
  `FmConnectionBanner` and `FmConfirmDialog` include them. Send to agent opens
  the composer with `C/navigation/MessageHandoff.agentPrompt` ("The ForkMesh
  Android app showed this message: …"); `NavigationFeature.install` sets the
  hook (`FmMessageHandoff.compose`) so the kit does not depend on feature code.
- **Haptics.** `FmHaptics` gains `send`, `palette`, `success`, `warning` and
  `error`, the iOS cues; success and error use the platform `Confirm` and
  `Reject` patterns.
- **Menus.** `FmDropdownMenu` (every menu in the app) is the iOS 26 pull-down
  menu: an opaque rounded panel (22 dp corners, `FmMenu.surface`: `#2C2C2E`
  dark, white light, a 0.5 dp hairline), at least 250 dp wide, 44 dp rows
  with a leading outlined icon in the text colour and 17 sp text. The surface
  must stay opaque: the menu's shadow draws under the whole shape and the
  popup window clips it, so a translucent fill shows a grey rectangle.
- **Action popover.** `FmActionMenu` and `FmActionMenuButton` are the iOS 26
  confirmation popover (a 32 dp panel of stacked 48 dp capsule buttons with
  centred accent text, red for destructive). Home's touch-and-hold menu uses
  it, centred on the press point, with the iOS items and wording ("Show all
  update dots", "Clear all update dots", "Home settings…", "Bottom bar
  settings…").

## 18. Home grid (sync ledger S010)

Home draws the iOS outline icons (`ShellHomeTile`,
`A/feature/navigation/NavigationScreens.kt`, after iOS
`Shell/ShellHomeIcon.swift`): 5 across, 66 dp tiles, a 22 dp glyph in a
39 dp box (`iconSize * 1.75`) painted with the iOS vertical gradient
(`textPrimary` to 82% light / 74% dark, accent when selected or running), a
12 sp label (medium, semibold when selected), the iOS count pill
(`ShellHomeCountMark`: accent capsule for unread, a blue-tinted pill for
running counts, `FmPalette.count`) at the glyph's top right, and the
desktop-offline dot at its bottom left.

`ShellHomeGrid` (`A/shell/ShellHome.kt`, after iOS `Shell/ShellHome.swift`)
lays Home out with `C/shell/ShellHomeStyle.arranged` and the iOS key
`shell/home-style`: icon style (outline, filled, accent, tiles), 4/5/6
across, eight sorts, grouping, category titles, category order, hidden
categories, the frequent-use glow (`ShellBarOrder.heat` over the usage that
every Home tap records) and counts. Rows shrink to fit the screen
(`ShellBarOrder.fittedTile`, 50 dp at least) as on iOS. Touch and hold the
background or any icon for "Home screen settings…" and "Bottom bar
settings…"; touch and hold a category title to hide it. The settings sheet
(`ShellHomeSettingsForm`) is the iOS form; "Hide icons in the bottom bar"
stays the existing Android setting (`home/hideBarIcons`). Not yet ported:
dragging icons to reorder Home and the Home search field (S010 stays
`doing`).


## 19. Outline icons (sync ledger S002)

iOS draws its chrome with SF Symbols: outline glyphs that switch to the filled
variant when selected. SF Symbols may not ship in an Android app (Apple's
licence), so Android uses the closest family it has, Material Icons
**Outlined**: `FmIcon(name, outlined = true)` picks `<name>_outlined` when the
font has it and falls back to the regular glyph. The tab bar uses outlined
glyphs when idle and filled when selected; Home tiles (the iOS "Outline"
style) and the hub's rows and chips are outlined. Home glyphs are drawn at
26 dp in their 22 dp slot because Material glyphs carry 2 dp of padding
inside the em box where SF Symbols do not.

`tools/subset_material_icons.py` now keeps the `_outlined` twin of every icon
it keeps (the font grew from 112 KB to 194 KB; the iOS copy is the same file
and only gains glyphs). The full font used to come from git history
(`FULL_FONT_REVISION`), which a shallow or rewritten clone does not have;
pass `--font` with any full `MaterialIcons-Regular.otf`. The Flutter SDK's
`bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf` has all 8,825
codepoints of `tools/material_icons/MaterialIcons.codepoints`, and its
glyphs are identical to the ones the subset shipped (checked 2026-10-20).

## 20. Bottom bar look and settings (sync ledger S012)

The tab bar is drawn from the iOS bar style (`ShellStyledBar` and
`ShellBarRows`, `A/shell/ShellBottomBar.kt`, after iOS
`Shell/ShellBottomBar.swift` and `ShellBarLook.swift`), stored under the iOS
key `shell/bar-style` through `C/shell/ShellBarPrefs`:

- Shape (capsule, panel, squares, circles, icons only), fill (glass, clear,
  tinted, solid), corner and spacing sliders, full width or fit, and Single or
  **Double** height. Double height splits the icons into two rows
  (`ShellBarStyle.rowSplit`) inside one rounded panel; slots keep the iOS
  64 dp cap, so a short row starts at the leading edge as on iOS.
- Icon style (outline, filled, accent, tiles), size S to XL, labels (always,
  selected, never), the selected mark (fill, outline, glow, dot, line,
  none), counts, activity, badges like Home or plain, highlight frequently
  used, and Reset bar look.
- Touch and hold any bar icon for "Bottom bar settings…", which opens
  `ShellBarSettingsSheet`: the iOS form with a live preview over the accent
  gradient, choice chips, segmented pickers, sliders (`FmIosSlider`, the
  iOS round thumb), a stepper and switches.
- `ShellBar.clearance()` follows the bar's measured height, so pages clear a
  double-height bar.

**Choosing what sits in the bar** follows iOS `ShellBarCustomize.swift`. Every
Home item is a bar candidate (`rememberShellBarCandidates`, `A/shell/ShellHome.kt`):
the tabs first (ids `home`, the `AppTab.raw` names, `repos`, `code`), then the
Home entries, the current repository's sections (so Pull requests can sit in
the bar with its open count, S017) and every hub item. The saved layout
(`shell/bar-layout`: hidden, folders, order and the explicit bar list) picks
the slots through `C/shell/ShellBarPrefs`. Touch and hold a bar icon for the
iOS item menu: Remove from bar, Move left / right / to the front / to the end,
Move to folder… (or New folder…), Take out of the folder, Reset bar layout and
Bottom bar settings…. Touch and hold a Home icon for the same menu with "Add
to bar", or "Add to bar in place of…" when the bar is full. A folder shows a
2×2 grid of its first icons with the summed badge; tapping it opens its
members in a popover, and holding it offers Open, Move, Rename folder…, New
folder…, Remove folder from bar and Remove folder (keeps its items). With
"Hide icons in the bottom bar" on, Home hides exactly what the bar holds, so an
icon removed from the bar reappears on Home. Drag to reorder is not ported
(the Move items do the same), so S012 stays `doing`.

## 21. Update dots on Home (sync ledger S018)

As iOS `AccountPackagesStore` does, `C/market/AccountPackages.kt` remembers
each plugin's latest Market version and the version you last opened, under
the iOS key `packages/versions` (`{"seen": {...}, "latest": {...}}`). A
plugin whose latest version differs from the one you saw, including one you
never opened since installing, is "updated": its Home tile shows the iOS
accent dot at the glyph's bottom right (`ShellHomeUpdateDot`), and opening it
from Home marks it viewed. Hub items map to their plugin through
`PackageGate.HUB_ITEMS` and `ROUTE_KINDS`. Touch and hold Home for "Clear
update dots" (`markAllViewed`) or, when none are left, "Show all update
dots" (`markAllUnviewed`). The latest versions come from every catalog the
app reads (the Market, and the catalog refresh each time the app comes to
the foreground).

## 22. Shell badges, repository icons and slow-load bar (sync ledger S017, S019, S020)

**Badges (S017).** Android has no Pull requests icon of its own in the bar
(choosing bar items is not ported, S012), so the open count lives where the
repository does on Android: the Code icon in the bar and on Home, plus a new
Home category named after the current repository (`ShellRepoGroup`, id
`repo`, the iOS `repo-<section>` tile ids: Pull requests, Issues, Commits,
Branches, Releases, Actions, Discussions, each gated by
`PackageSurfaces.showsRepoSection`). The bar and Home draw one pill, so a
count of new pull requests (accent) wins over the open count (tinted pill on
Home, accent on the bar); iOS draws both. A pull counts as open unless it is
closed or merged (`PublishedPull.isOpen`, also used by the repository
overview).

- `ShellPullsWatch` (`A/shell/ShellPullsWatch.kt`, after iOS
  `ShellPullsWatch.swift`) is the one reader of the current repository's pull
  list. It reads on a repository change, when the app comes to the
  foreground, when the account socket reconnects, on the relay `pulls` frame
  (`ShellBadges.install` chains `AccountEventChannel.onTopic`). It does not
  poll: the pull request's port added a 20 s foreground loop
  (`NewPullsMarker.pollEvery`, as iOS has), which the no-display-polling rule
  (`www/docs/operations/polling-elimination.md`) forbids, and the `pulls`
  frame already says when the list changed, so the loop was removed; core
  keeps `pollEvery` with its iOS tests. A read that lands while one is running
  queues one more. The Pull requests page
  (`PullsTabContent`) takes the watch's list when it changes, supports pull to
  refresh, keeps its list when a refresh fails, and marks pulls seen while it
  is open.
- `NewPullsMarker` (`C/shell/NewPulls.kt`) keeps the highest pull number seen
  per repository under the iOS key `shell/pulls-seen`, 40 repositories, a
  first sight recording a baseline without a badge. Android's `PrefStore` has
  no string list, so the value is the iOS lines (`<number> <slug>`) joined by
  newlines in one string.
- `ShellBadges` (`A/shell/ShellBadges.kt`) holds the other counts. Tasks
  shows the open tasks assigned to the signed-in user (`TaskBoard.mineOpen`,
  first page of the board), refreshed with Home, on every `pings` frame and
  whenever the Tasks page loads. RSS Reader shows the Unread smart feed's
  count (`RssLibrary.counts().unread`, recomputed on each library version;
  Home opens the library and refreshes feeds that are due, as iOS does).
  Both ride `hubBadges` in `HomeShell` as `ShellMenuBadge`.
- The Bug icon is the floating bug button (Android has no Bug tile on Home):
  for administrators it shows the number of reports still `new`, read with
  `bugReportInbox(status = "new", limit = 1)` and written back whenever the
  Report a bug sheet's inbox loads or a status changes.

**Repository icons follow the composer (S019).** `HomeShell` watches the
composer's repository (`QuickSendState.repository`, `AgentStartTarget.ref`,
owner and name as iOS) and calls `CodeFeature.follow`, which moves
`CodeFeature.currentRepo` and the `code/current-repo` key. Opening a
repository in Code already moves the composer back
(`ComposerFeature.FollowRepository`). Android prepares the composer when it
first opens, not at launch, so the very first repository it settles on is
followed from then on.

**Slow-load bar and Agents first load (S020).** `ShellLoadingOverlay`
(`A/shell/ShellLoadingBar.kt`) is the iOS `fmLoading` bar: 2 dp, accent at
75%, a 30% segment sweeping in 1.2 s, shown only after the load has run
250 ms, a steady 40% bar under reduced motion. It sits in `shell/` because
`ui/kit` was being changed in parallel; move it to the kit when the kit's own
loading bar (S008) lands. It shows on Home (counts, tasks, RSS, repository
counts), the Agents list, the repository overview and the RSS timeline.
`AgentListRings` (`C/agents/AgentListRings.kt`) is the iOS ring rule for the
Agents list: a ring never restarts a load in flight, one that lands before
the per-repository reads go out is answered by them, a later one queues one
more load after the current one has published. `AgentsScreen` used to key one
`LaunchedEffect` on the ring signal, so the ring at connect cancelled the
first load and read every repository twice.

## 23. Liquid Glass renderer (sync ledger S003)

`fmGlass` (`A/ui/kit/FmGlass.kt`) moved from the iOS pre-26 frosted look
(`bgRaised` at 55/66% over a 24 dp blur, hairline edge) toward iOS 26 Liquid
Glass:

- **Refraction.** On Android 13 and later the sampled backdrop goes through
  an AGSL lens (`FmGlass.LENS`, a `RuntimeShader` chained after the blur):
  within 18 dp of the shape's edge, samples are pulled inward along the edge
  normal by up to 9 dp, so content behind the rim magnifies and bends as it
  does under iOS glass. The shader reads the rounded-rectangle radius from the
  shape's outline. Android 12 keeps the plain blur; below 12, inside a
  recorded page, and with high-contrast text the solid fill stays.
- **Clearer glass.** An 18 dp blur and a lighter tint (`bgRaised` at 30% dark,
  34% light) with a softer top highlight, so the content behind keeps its
  colour.
- **Specular rim.** A 1 dp gradient stroke, bright at the top (white 85%
  light, 28% dark) fading to a faint edge at the bottom, instead of the flat
  hairline.

`half` is a type in AGSL; a variable named `half` makes the shader fail to
compile at run time, and `FmGlass.lens` then falls back to the plain blur.
Press response and merging neighbouring glass pieces are not ported.

## 24. Navigation motion (sync ledger S006, S009)

- **Push and pop** (`TabStack`, `A/shell/HomeShell.kt`, and `NavigatorHost`,
  `A/ui/navigation/Navigator.kt`) move like `UINavigationController`: the new
  page slides in full width from the right while the page under it slides
  30% to the left, 350 ms on `FmMotion.pushEasing`
  (cubic-bezier 0.2, 0.8, 0.2, 1); a pop reverses it with the leaving page on
  top. Android used a 25% slide with a fade, 200 ms.
- **Interactive back.** `TabStack` drives its transition from a
  `SeekableTransitionState`, and a `PredictiveBackHandler` scrubs the pop with
  the system back gesture, as the iOS edge swipe does; letting go finishes the
  pop (`Navigator.handleBack`) and cancelling springs the page back.
- **Tab switches** do not animate (each tab's stack is keyed on its
  navigator), as iOS shows a tab's pages in place.
- Reduce motion snaps instead of sliding and turns the interactive back off.

## 25. Area parity with iOS (ledger rows)

Rows of [SYNC.md](SYNC.md) ported to Android area by area. Each follows the
cited iOS commit; wire and storage formats are unchanged.

- **Market navigation line (S029).** `A/feature/market/MarketNavBar.kt` draws
  the iOS `MarketScreen` toolbar as one slim line with no fill: the 44 dp glass
  Back circle, a 36 dp search capsule in the title's place, then a sort menu
  and a filter menu whose icons turn accent (and the filter icon fills,
  `filter_list` to `filter_alt`) when set off the defaults. The filter menu
  lists every filter as a row that opens its own choice menu (Compose has no
  nested menus), then Clear filters. `FmNavBar` has no title slot, so the line
  is local to the Market rather than a kit change. The search field and the
  chip row under it are gone; the Refresh button became pull to refresh, as
  iOS `fmRefreshable`.
- **Inbox bar (S030).** `A/feature/inbox/InboxBar.kt` is iOS `InboxFilterBar`
  and `InboxToolbarMenu`: one centred row of 28 dp glass squares (8 dp
  corners, 4 dp apart, 16 dp icons), the six filters then Mark all read,
  Unread only, a pause menu (bedtime, or a warning-coloured bell-off while
  paused), Discard changes that could not sync (only when a change ran out of
  attempts), Ping settings and Delete all pings. The selected filter and an
  active Unread only take the accent with an accent-subtle glass fill. Counts
  are a 14 dp badge offset 5 dp up and right, always the blue accent (iOS
  `fm.count`), "99+" above 99. Holding a square shows its name. A phone too
  narrow for the row scrolls it sideways. The squares draw glass without the
  12 dp shadow, which swamps a 28 dp shape.
- **RSS article unread count (S031).** The article screen passes
  `RssCounts.of(scope)` for the timeline it was opened from (or the one Next
  Unread moved to) to `RssScaffold`, which draws it in secondary
  tabular-figure text right of the Back button, as iOS `RSSUnreadCountToolbar`
  and NetNewsWire do. Opening an article marks it read, bumps the library
  version and the count drops. Nothing shows at zero.
- **Vault import and export (S033).** `C/vault/VaultTransfer.kt` is iOS
  `FMVault/VaultTransfer.swift`, `VaultZip.swift` and the text of
  `VaultTransferModel.swift`: the same CSV reader (header aliases, quoted
  fields, BOM, UTF-8 with a Latin-1 fallback, 16 MB cap), the same source
  detection order (Bitwarden, Firefox, 1Password before Apple's `OTPAuth`,
  Chrome, DuckDuckGo), the ZIP reader for Safari's export (`java.util.zip`
  `Inflater` and `CRC32` stand in for zlib), the duplicate plan and the
  review, report and error sentences. `VaultTransferTest` runs the iOS test
  cases with the same ZIP fixtures. `VaultService.importLogins` saves each new
  login through `VaultClient.save` and stops at the first error, as iOS.
  `A/feature/tools/vault/VaultTransferSection.kt` is the Move passwords section
  of Vault settings (between autofill and locking, as iOS): Import from a file
  opens the system picker (`OpenDocument`), Export to a file asks first and
  writes `ForkMesh Passwords.csv` through `CreateDocument`. The help text names
  the Android-side export paths; Credential Exchange is iOS-only, so there is
  no Send to another app button and no incoming import.
- **CRM Zoho Mail status (S034).** `CrmZohoStatus.fromJson` reads the
  `settings.zoho` fields iOS reads (`clientId`, `hasClientSecret`, `canSend`
  defaulting to `connected` for older servers, `lastSentAt`, `lastSyncError`,
  `steps` without text dropped). `A/feature/market/CrmZohoBar.kt` is iOS
  `CrmZohoBar` under the header's stat tiles: a glass card with the status
  word (Not connected, Read only, Sync failed in orange; Reads and sends in
  green), last synced and last sent, and each missing step with Sync now or
  (Re)connect Zoho Mail when the organization has one-click Zoho and the
  viewer manages it, otherwise the Self Client or ask-an-admin line. CRM
  settings add Sending (Allowed or Read only) and Reconnect Zoho Mail.
  `CrmZohoCode.of(status)` prefills the client ID and data centre; the secret
  field reads "Saved secret (leave blank to keep)" and may stay empty while
  the client ID is the saved one (`usesSavedSecret`), as iOS `CrmZohoCode`.
- **App Store Status desktop fallback (S035).** `AppStoreStatusLoader.load`
  in `C/market/AppStoreStatus.kt` is iOS `AppStoreStatusModel.load`: it asks
  the paired desktop (`stats.appStoreStatus`, 120 s, through
  `HubControlCalling`) when the server has no page ("not there" codes or no
  snapshot) and when the server's snapshot predates tester screenshots
  (`AppStoreSnapshot.lacksScreenshots`: screenshot feedback exists and none
  has a picture; crash reports do not count). A stale snapshot is replaced
  only by a desktop answer that has pictures, and a desktop failure then
  stays silent; the page keeps the server's snapshot with "Tester screenshots
  appear once the iOS App Store status shortcut runs again on your desktop."
  under Tester feedback. A desktop answer reads "From your desktop <time>".
  Android had no desktop fallback before, so the loader brings the missing-
  page fallback in the same change.
- **Reported bugs inbox (S036).** As iOS `ApiService.bugReportInbox`: a 404
  or 405 from the inbox, a status change (unless the body says
  `report_missing`) or an attachment (unless `attachment_missing`) means the
  relay has no inbox route and reads "This relay cannot list reported bugs
  yet. Update the relay and try again." An interrupted load
  (`InterruptedIOException` other than a timeout, `ClosedByInterruptException`,
  `CancellationException`) returns `BugReportInbox.CANCELLED_LOAD`, and
  `BugReportInboxState.received` then only clears the spinner and stays
  unloaded, so the next load runs, instead of "Could not reach ForkMesh". A
  cancelled coroutine already drops its result on Android, so no change was
  needed in the sheet.
- **Face Changer empty recording (S037).** The shared `direct-call.html`
  asset already asks for H.264 level 4.0 (`avc1.42E028`, iOS commit
  `a4dbcbb101` changed both copies). A stop with nothing recorded
  (`WebCallPeer.finishRecording` sees size 0, `empty_recording`) now says
  "Nothing was recorded. Record for a little longer, then stop." instead of
  "The clip could not be saved. Try again."
- **Unnamed instance takes the instance's name (S039).** As iOS `RelayProbe`,
  `ProjectProbe.verify` labels a candidate that has no typed name (its label
  is still its host) with `/api/instance`'s `name`, and the New project
  dialog fills an empty Name field with it after Check instance, so the
  project is saved under "Acme Code" rather than `relay.acme.dev`. A typed
  name always wins. Android has no separate Add relay form (relays are
  projects), so the iOS relay change needs nothing more here; the instance
  logo was already adopted on create.
- **Calendar saves survive a late list (S040).** As iOS `CalendarModel`,
  each `load()` takes a token and the change revision it started from. A
  list that answers after a newer load started is dropped, and a list that
  answers after a create, update, RSVP or delete is merged with the local
  changes made since it was requested, so a new event is not lost and a
  deleted one does not come back; changes the list already reflects are
  forgotten. Android loads on `Dispatchers.IO` while saves run on other IO
  threads, so the token, revision and event list are guarded by one lock.
  `CalendarSaveTest` interleaves a save and a second load inside a list
  request.

## 26. Typeface (sync ledger S001)

iOS sets every text style in SF Pro, switching from SF Text to SF Display at
20 pt. SF Pro may not ship in an Android app (Apple's licence), so Android
uses **Inter** 4.1 (SIL Open Font License; `res/font/inter_variable.ttf`,
licence in `assets/fonts/Inter-LICENSE.txt`), the variable font from the
official release (`rsms/inter` v4.1, `Inter-4.1.zip`, SHA-256
`9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`,
downloaded 2026-10-20). It has a weight axis and an optical-size axis, which
stand in for SF's two cuts: `FmType.ui` sets `opsz` 14 and `FmType.uiDisplay`
`opsz` 32, and the type scale (`fmTypography`) picks the display cut at 20 sp
and above (`FmType.family`). Sizes, weights, leading and tracking are the
iOS values, unchanged. Text that builds its own `TextStyle(...)` names
`fontFamily = FmType.ui`; Compose does not merge a fresh `TextStyle` with the
theme, so a style without a family falls back to Roboto.

Monospace text uses **JetBrains Mono** 2.304 (SIL Open Font License;
`res/font/jetbrains_mono_variable.ttf`, licence in
`assets/fonts/JetBrainsMono-LICENSE.txt`) in place of iOS's Menlo, the
variable font from the official release (`JetBrains/JetBrainsMono` v2.304,
`JetBrainsMono-2.304.zip`, SHA-256
`6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf`,
downloaded 2026-10-20). `FmType.mono` is the only monospace family; code
names it instead of `FontFamily.Monospace` (Droid Sans Mono).

## 27. Feature parity, second round (ledger rows)

Rows of [SYNC.md](SYNC.md) ported on `android-feature-round2`. Each follows
the cited iOS commit.

- **Stalls in their own sections (S043).** `DiagnosticsBundle.report` adds
  Stalls by name, Stalls by screen and Slow operations by name ahead of the
  Slowest events list, as iOS `PerformanceReport`: one row per name (or
  screen) with count, total, max and, for stalls, the clock time of the
  newest, so slow network calls no longer push stalls out of the report.
  `PerformanceMonitorService.screenName` is read for every frame, event-loop
  and sync stall that does not already name its screen;
  `DiagnosticsRuntime` points it at `NavigationFeature.screenTitle()` (the
  selected tab, then the titles of the screens pushed on it and of any
  full-screen page above the shell, joined with " > "). iOS reads the screen
  only on the main thread; Android frame stalls arrive on the frame-metrics
  thread, so the source is read on any thread (Compose state lists are safe
  to read there) and a failure leaves the stall without a screen. Stalls
  without one group under "not recorded".
- **Diff toolbar squares (S044).** `StreamToolbar` in
  `A/feature/code/DiffReviewStream.kt` drops the raised bar, its bottom rule
  and the separators: every button is a 30 dp `fmGlass` square (8 dp
  corners, 4 dp apart, 17 dp icon, accent icon on an accent-subtle glass when
  on), and the seen/total file button is a glass square that widens for its
  count. Holding any square shows its name through `FmTooltip`, as the Inbox
  squares (S030). Android keeps its own extra buttons (previous and next
  change, next file, text size) because it has no pinch zoom; the find bar
  keeps the small flat buttons, as iOS keeps `tiny` there.
- **Eagle view of agents (S042).** `AgentListView.Eagle` is the fourth view
  (Compact, Zen, Full, Eagle; the view button shows `flutter_dash`, the
  nearest Material bird to iOS `bird`) and the default: a missing or unknown
  `agents/listView` reads as Eagle, and a phone that stored another view is
  switched once (`agents/listViewEagleDefault`), as iOS. An Eagle row
  (`feature/agents/AgentEagleViews.kt`) has no card: hairline rules above
  and below (the list spacing drops to 0 so neighbours share one), the status
  glyph with the unread dot on it, the title with the file count and churn
  bars, and a 12 dp ticker of the agent's last line. `AgentEagleStrip` is iOS
  `AgentEagleStrip`: a train of copies 5 dp apart that scrolls left at 80
  dp/s after a 0.5 s hold, drawn on a `Canvas` from a frame loop with a
  faded right edge; only running rows move, a stopped row holds its newest
  line still, and with animations off (animator duration scale 0) nothing
  moves. `AgentEagleFeed` (`core/.../agents`) is iOS `AgentEagleFeed`:
  running desktop rows stream through `ControlWatchFeeds` (at most six,
  which gained iOS's per-row revision so a row is re-read only when its run
  moved); cloud runs read `liveText` while running and the last non-user
  message after (`AgentEagle.dialog`); a stopped desktop row with no line
  reads its transcript once through `AgentControlCenter.transcript` when it
  appears, at most three at a time, on background threads. The feed runs
  only while the Agents screen shows Eagle and stops when it leaves.
  `AgentEagleState` copies the feed's lines into a Compose state map on the
  main thread so only the row whose line changed redraws.
- **Other apps' calls (S038).** See [OTHER_APP_CALLS.md](OTHER_APP_CALLS.md#android):
  `OtherAppCall` (app `platform/`) with its pure state in
  `core/.../net/OtherAppCallState.kt` quietens interface sounds, Agent Watch
  and the Glasses Alert ring during another app's call; ForkMesh's own calls
  do not count.

## 28. NEXUS network inspection (sync ledger S046)

NEXUS is the port of the iOS package at `native/ios/Nexus` (see
[NEXUS.md](NEXUS.md#android)). The logic is plain Kotlin in
`core/.../nexus` so `NexusTest` runs on the host JVM; the screens and the
Android-only pieces are in `app/.../feature/nexus`.

- **Same split as the Swift targets.** `NexusAddress` (NexusCore addresses,
  networks, `Hostname`, `LocalNetwork`), `NexusTargets` (target parsing and
  `ScanAuthorizationPolicy`), `NexusPorts` (port lists, presets, service
  catalog), `NexusModels` (sessions, results, devices, certificates), `NexusDns`
  (the UDP wire format and resolver), `NexusScan` (scanner, banner
  identification, `SecurityInsights`), `NexusDiscovery` (inventory merge and
  change list), `NexusWeb` (HTTP/TLS inspector and reachability) and
  `NexusClient` (NexusKit, the remote-agent contract, reports and history).
  Core has no coroutines, so the scanner runs a fixed thread pool and stops
  through a `NexusCancel` token; `NexusState` calls it on `Dispatchers.IO`.
- **java.net in place of Network.framework.** TCP connects are `Socket.connect`
  with the scan timeout, UDP DNS uses `DatagramSocket`, and `NexusFailures`
  maps the platform errors (`ECONNREFUSED`, `EHOSTUNREACH`, `ENETUNREACH`,
  timeouts, `EPERM`) to the iOS `ConnectionFailure` cases, so a refused port is
  Closed and a timeout is "No response". Hostnames in a scan target resolve
  through the system resolver, so router-assigned names such as `nas.lan`
  work; the DNS explorer still queries 1.1.1.1 directly like iOS.
- **NsdManager in place of Bonjour.** `NsdBrowser` browses each service type
  for the iOS duration (8 s), then resolves every found service for its
  addresses, port and TXT record (`registerServiceInfoCallback`-era
  `resolveService` with an executor on Android 14+). It needs no runtime
  permission; Android has no equivalent of the iOS Local Network prompt.
- **More than iOS shows, never less.** The TLS observation comes from an
  `SSLSocket` handshake with hostname checking on, so Android lists the full
  chain (issuer, validity, SANs, key size, signature algorithm, serial,
  SHA-256), the protocol and the cipher suite, where iOS 17 only has the
  subject. Validation is never bypassed: an untrusted certificate is reported
  as "Not trusted" with the reason and no chain. Timings add connect and
  first-byte times to the iOS total. Discovered devices get an *inferred* kind
  from their advertised services (`DeviceClassifier`), labelled as such.
- **History and reports.** iOS hosts NEXUS without a `NexusStore`, so its
  history lasts until the app quits. Android keeps it in
  `filesDir/nexus/history.json` (50 scans, 20 discoveries, the device list)
  with a Settings switch to turn that off and a History menu to clear it. A
  scan's detail screen shares the `NexusReportExporter` JSON or CSV through
  the share sheet (`nexus-share/` in the FileProvider paths).
- **Settings are wired to the scanner.** Timeout, concurrency and "Allow
  internet targets" are stored in the `nexus` preferences and passed into
  every scan. When the policy answers *requires attestation*, the Scan tab
  asks for the iOS attestation statement and retries with it; internet ranges,
  more than four internet hosts and multicast/broadcast targets stay refused.
- **Not a Market package.** iOS `PackageGate` deliberately has no entry for
  NEXUS (its `PackageRoutesTests` asserts that), so Android does the same: the
  You hub's Workspace group always lists it and `/dashboard/nexus` and
  `forkmesh://nexus` open it. The desktop's `nexus` Market package is a
  separate desktop feature.
- **Presentation.** iOS shows `NexusRootView` as a full-screen cover with a
  bottom `TabView`. `NexusScreen` hides the shell chrome and draws its own
  glass tab bar (`FmBottomBar`) with the six tabs, and its detail screens push
  on the normal stack. `Route.Nexus` is not marked modal because Android's
  router drops modal routes from deep-link stacks.

## 29. Composer actions and screen capture (sync ledger S028, S032)

- **Prompt bar actions that start no agent (S028).** As iOS
  `QuickPromptActions`, the model menu starts with Bug report, Task · assign,
  Manual · create issue, ActivityPub · post and Take a note
  (`C/composer/QuickPromptActions.kt`, provider `prompt-action`). The pick is
  kept under `agent/quick/action` (`""` once a model is picked); until then
  the chip shows Bug report whenever nothing can run an agent
  (`QuickAgentLauncher.hasAgent`: signed out, no destination, or this phone
  without an API key), so a phone with no agent still has a working Send.
  Each ends in `QuickLaunchOutcome.Done`, whose message shows in the shell's
  notice once the composer closes. `A/feature/composer/AppPromptActions.kt`
  sends them like iOS `ShellPromptActions`: `POST /api/bug-reports` (screen
  `composer`, photos attached, no account needed), `POST /api/tasks` (first
  line as title up to 160 characters, the whole prompt as details, General,
  priority 50), a signed issue through `InboxService`, a signed manual
  `discussion`/`open` `ap-publish` (`ApiCiClient.announcePost`) and a private
  account note. A Task shows an Assign to menu above the context line and an
  Assign to section in the picker (you, then the board's members when you can
  manage it; `tasks/composerAssignee`). With an action chosen the picker
  hides Runs on, and shows Repository only for an issue or a post. The
  diagnostics report always starts an agent (`send(agentOnly = true)`), as
  iOS keeps spoken and Watch launches agents. Only `feature/composer` and
  core changed; the shell needs no new call.
- **Hold a blank area to copy part of the screen (S032).** See
  [CAPTURE_MARKUP.md](CAPTURE_MARKUP.md#android-long-press-a-blank-area-to-copy-part-of-the-screen).
  The hold is a feature-level window touch watcher installed from
  `ComposerFeature.install`, so `shell/` is unchanged; the one thing left for
  the shell is Home's background menu entry "Select area to copy…".

## 30. Shell and kit parity (sync ledger S003 to S016)

- **Scroll edge, sheets and large titles (S006).** `fmScrollEdge()`
  (`ui/kit/FmGlass.kt`) fades content into the background under the bar once
  the page has scrolled, as the iOS 26 soft scroll edge does. `DetailScaffold`
  and `RoutePage` apply it to every pushed page. `FmModalBottomSheet` opens at
  the medium detent unless `isScrollControlled`, and every sheet uses
  `FmSheet.corner` (28 dp). iOS shows large titles on five screens only
  (Status, Network, Stats, Admin, Search), so both scaffolds take
  `largeTitle = true` instead of making it the default: `FmLargeTitleBox`
  draws a 34 sp bold title that a nested scroll connection collapses before
  the content scrolls and restores when the content is back at the top. The
  bar's own title fades in over the last 30 % of the collapse.
- **Chat header and composer are glass over the messages (S004).** The chat
  room is an `FmBackdropHost`: the transcript is its content and the header
  (with the topic, mesh, pending-device and unread banners under it) and the
  composer stack are its overlay, so both bars refract the messages that
  scroll beneath them. Their measured heights become the list's top and
  bottom content padding, so the first and last messages still rest clear of
  the bars. A short gradient from the canvas colour behind the header keeps
  the status bar readable over messages. `PromptComposer` passes no tint to
  `fmGlass` when a backdrop is present, so the composer card is clear glass
  there and stays the solid surface everywhere else.

### Activity halo and the shortcut progress ring (S016)

`shell/ShellBarActivity.kt` ports iOS `ShellActivityHalo` and
`ShellBarProgressRing`. A bar cell is `active` while its page has work
running: the Agents tab with running agents, Timers with a running timer,
Shortcuts with a running run or catalog entry (iOS
`ShellNavigatorCounts.active`). Active cells draw the breathing glow and the
turning 70 % arc around the glyph when Show activity is on. While a shortcut
runs, Shortcuts swaps the halo for the progress ring and the glyph shrinks to
78 %. `ShellBarProgress` (core) holds the run's start and expected length;
the ring fills as elapsed over expected, capped at 99 %, redraws twice a
second, and spins a short arc when there is no earlier run to estimate
from. When several run it follows the one expected to finish last. The cell
reads "Running, N percent" to TalkBack. The bar reads
`TeammatesFeature.startedShortcuts`, so it never starts the shortcuts store
itself; it looks for it every 2 s until something opens Shortcuts.

### Home search, custom order and drag (S010, S012)

- **Custom order.** `ShellMenuLayout` (core, iOS `ShellMenuLayout`, key
  `shell/menu-layout`) keeps where you moved each page, per group; pages it
  does not know keep their place at the end of their own group, and a page
  that leaves (a repo page with no repo open) comes back to where it was.
  `ShellBarPrefs.arranged` applies it before Home's own sort, and
  `moveInMenu` lands a forward move after the target and a backward move
  before it, as iOS.
- **Drag.** `shell/ShellDrag.kt` is one drag shared by Home and the bar, the
  part of iOS `onDrag`/`onDrop` that Compose has no system for. Touch and
  hold a tile or bar cell, then move: the long-press menu closes, the source
  dims to 35 % and a glass copy of the icon follows the finger (tinted accent
  over the bar). The gesture watches the initial pointer pass, so the tile's
  own press and long-press still work when you do not move, and positions are
  kept in root coordinates, so a tile that moves under the finger during a
  reorder does not make the pointer jump. On Home, passing over another
  tile moves the dragged one there (grouped layouts only, as iOS
  `reorders`) and switches the sort to Custom order. Dropping on the bar puts
  the page in the slot under the finger (`putInBar`, replacing it when the
  bar is full); dropping a bar cell anywhere off the bar takes it out, and
  dropping it on another cell moves it there. Home stays in the bar.
- **Search.** Pull Home down 56 dp (`ShellBarOrder.revealsSearch`) for the
  glass search capsule. It filters pages by title, or a whole group when the
  group's title matches, shows hidden groups while searching, and opens the
  first result on the keyboard's Search key. With no text the trailing
  control is the settings menu (Home settings, Bottom bar settings); with
  text it clears. "Search everywhere for …" under the results opens global
  search. The field hides again when it loses focus empty.

### Wallpaper behind the glass (S007)

Settings → Appearance ends with a Background row, as iOS
`AppearanceBackgroundRow`: a 48 x 84 thumbnail, Choose image / Change image
(the system photo picker, `PickVisualMedia`, so no permission), Remove, and
once a picture is set a Dim slider (`FmIosSlider`, 0 to 90 % in 5 % steps,
default 40 %). The note under it is the iOS text.

The two settings use the iOS keys, `appearance/backgroundImage` (a file name,
empty for none) and `appearance/backgroundDim` (a double, stored by `Prefs`
as it stores every double). `Wallpaper` (core, `settings/Wallpaper.kt`, with
`WallpaperTest`) holds the clamp, the default, the file name rule (a name
with a `/` is treated as none, as iOS `backgroundImage`) and the sizing
maths; `Appearance` carries it and `AppearanceStore` has `setWallpaperImage`,
`previewWallpaperDim` (while the thumb moves, nothing written) and
`setWallpaperDim` (on release).

The picture is copied into `filesDir/wallpapers/wallpaper-<millis>.jpg`
(`platform/WallpaperStore.kt`). It is decoded at just over the screen's size
(`ImageDecoder` on API 28+, `BitmapFactory` with a sample size below), where
iOS caps the long side at 2800 px; the previous file is deleted when a new
one is chosen or the picture is removed. Nothing is written outside app
storage.

`FmWallpaperLayer` draws the picture cropped to fill, then `bgBase` at the dim
opacity (iOS `FmWallpaperBackdrop` dims with the theme's base colour, not
black, so a light theme brightens it). It is drawn twice: under the app in
`MainActivity`, and as the first layer inside `FmBackdropHost`, so the
bottom bar's glass samples and refracts the picture.

`FmPalette` has the iOS `wallpaper` flag (`ForkMeshTheme(wallpaper = ...)`)
with `canvas`, `readingSurface` and a 60 % `listSurface`. `canvas` is `bgBase`
without a picture and transparent with one. The shell's `Surface`, the
screen host and every page that painted
`fillMaxSize().background(Fm.colors.bgBase)` (40 files) now paint `canvas`, and
the Material `background` colour is `canvas`, so Scaffolds clear too; Agents,
Inbox and Home paint nothing of their own and show the picture directly.
Without a picture nothing changes.

Left out: the iOS card fills at 62 % and softer borders on the chat channel
list, the 80 % terminal surface and the pre-glass sheet backgrounds that
follow `wallpaper`; pages that draw `bgBase` in some other shape than the
full-screen fill (headers, the Genie mail line, chat bubbles) stay opaque; the
RSS reader's own theme wrapper and the wide-screen navigation rail do not
carry the flag. HEIC needs API 28 or later to be read.

### The voice prompt (S015)

Touch and hold the new-agent button to speak a prompt; it listens until you let
go and starts an agent with your last repository and model. This is the iOS
voice prompt ([NAVIGATION.md](NAVIGATION.md) section 2.3), and it replaces the
old long press, which opened the command palette. The palette is now the
**Commands** button on the voice screen.

What was ported, and where it lives:

- **Gesture** (`PromptButton`, `holdToTalk` in `feature/composer/ShellPrompt.kt`).
  One pointer gesture instead of a click plus a long click, because a long
  click ends the moment it fires and cannot tell when the finger lifts. Lifting
  before 350 ms (`VoiceLaunchRules.HOLD_MS`) is a tap and opens the composer;
  moving more than 10 dp first does nothing. After 350 ms the voice prompt
  opens and listens while the finger stays down; letting go sends. TalkBack
  gets "Speak a prompt" as the long-click action, which opens the screen
  hands-free (no finger to wait for). The button also shrinks while pressed.
  It is wired to the top bar button and the wide-layout rail button.
- **Screen** (`ShellVoiceOverlay.kt`, drawn above the shell and below the
  launch sparkles). A dimmed, blurred shell (`Modifier.blur`, Android 12 and
  later; a solid 92% black backdrop on older phones or with high contrast
  text, as `fmGlass` falls back), the orb springing out of the button into the
  upper middle with the summon chime and a long-press haptic, 72 spokes that
  follow the voice meter, ripples that speed up with the voice, the words under
  the orb, a white arc that fills over the 3 s pause (hands-free only), a
  spinner arc while sending, a shock ring and stars on launch, and Cancel,
  Type and Commands under the orb. Tapping outside the orb or Cancel flies the
  orb back into the button. Type moves the words into the shell composer
  draft with the keyboard up. Reduce motion (the app's setting) fades instead
  of flying and stops the ripples, twinkle and burst. Sounds follow the
  interface sounds setting, and the shell's other cues are silenced while the
  screen is open (`FmSoundCues.quieten`).
- **Listening** (`ShellVoiceLaunch.kt`, `SpeechDictation.kt`). The same
  `SpeechDictation` as the composer, asked to prefer the device. Android's
  recognizer stops on its own after a quiet spell, so while the finger is down
  `DictationSession.holds` makes it start again at once and
  `VoiceTranscript` joins what each pass heard; the words never reset. Letting
  go keeps the microphone open for another 250 ms, stops the recognizer and
  sends when its final result arrives (at most 2 s later). Letting go before
  the microphone opens, or before anything was heard or the meter saw a voice,
  turns the session into the hands-free mode: it sends after the recognizer's
  3 s of quiet, or when the orb is tapped. The 120 s limit and the Cancel,
  Type and nothing-heard paths are the iOS ones (a falling `fizzle` chime and
  "I didn't hear anything" closing after 2.2 s). The microphone opens after
  the summon chime (0.42 s held, up to 0.8 s hands-free).
- **Sending** goes through `rememberQuickAgentActions`, the composer's own
  launcher, so the run uses the repository, destination, agent and model last
  used there, and the screen does not switch the repository to the page's.
  The orb is the source of the launch flight into the Agents cell
  (`LaunchFlight.source`). A send that needs a fix or fails puts the words in
  the shell composer draft and opens the composer, where the usual message and
  fix action are shown.
- **Chimes** (`core/agentwatch/VoiceLaunchChimes.kt`, test
  `VoiceLaunchChimeTest`) are the iOS note tables, rendered by the same
  `renderNotes` synthesiser and played from a `SoundPool`
  (`ui/kit/VoiceLaunchSounds.kt`, cached under `sound-cues/`).
- **Pure logic with tests** (`core/composer/VoiceLaunch.kt`, `VoiceLaunchTest`):
  release rules, hold and pause timings, the pause arc fill, hints and labels,
  the transcript joiner and the voice meter (`onRmsChanged` decibels to 0..1).

Left out or different:

- No primed recognizer: `SpeechRecognizer` is only created early at touch
  down (`SpeechDictation.prime`), there is no analyzer model to preload.
- Partial words arrive as the recognizer sends them; there is no "fast
  results" switch, and some devices play the system listening beep each time
  the recognizer restarts while the finger is down.
- The orb's star field is a fixed twinkle, not tilted by the accelerometer
  (the button's stars are static for the same reason, section 16).
- The words do not shimmer; the "Listening" line pulses instead.
- The microphone permission prompt appears when listening starts, so a first
  hold ends in the hands-free mode after the prompt is answered.

### Action sheets (S005)

`ui/kit/FmActionSheet.kt` is the iPhone action sheet that iOS
`confirmationDialog` shows: a 14 dp rounded group at the bottom with the
title and message centred in 13 sp secondary text, 57 dp rows in 20 sp
(accent, or the danger colour for a destructive choice) split by hairlines,
and a detached bold Cancel 8 dp below it. It rises with a spring and drops
away before the choice is reported. `FmMessenger.showActionSheet` returns the
chosen index (or null for Cancel and taps outside), and `confirmAction` is
the one-button form. A confirmation goes through it wherever iOS uses
`confirmationDialog`; `showConfirmDialog` stays the centred alert for the
places iOS uses `.alert`. The chat delete-message, delete-channel and
leave-channel confirmations moved first; the kit gallery's "actions" demo
shows it.

Screens that now use the action sheet, with the iOS wording (title, message and
button) copied from the matching `confirmationDialog`:

- Code: delete or merge a branch, delete an issue, move an issue to a task,
  merge-queue actions, Actions run controls (cancel, re-run and the like),
  delete a webhook, Merge all with AI, disconnect a provider mirror.
- Agents: end a desktop terminal, drop a queued session (its Cancel reads
  "Keep", as on iOS), continue on the desktop (agent and run screens), open a
  run in the provider's terminal, stop and delete an agent started outside
  ForkMesh, delete a desktop agent account, close an SSH session.
- Coves: discard cove edits, delete a cove document, delete an Actions secret.
- Other: send to Discord through the desktop, discard photo markup, sign out of
  the project from the You hub, and the Folder or Zip choice when importing a
  local repository.

Kept as centred alerts because iOS shows them with `.alert` or `fmDialog`
(`FmConfirmDialog`), or they only inform: discard or delete in the local
repository and file editor, "not a node yet" and "name is taken", the
end-to-end encryption note, the merge-without-review waiver, remove an SSH
key, revoke an installed app, forget a paired desktop, switch account, the
"Cove proposed" and "Cannot open a pull request" notices, and the passkey
fallback of "Confirm it's you". Some have no confirmation on iOS at all (log a
desktop account out of a provider, replace the identity, remove a project, open
a repository on another project, delete a feed post) and keep their alert.
Pickers that iOS builds as menus or sheets (the review verdict, the attach
sheet, message actions, move to group) stay bottom sheets.

### Glass press response (S003) and glass over content (S004)

- `fmGlass(interactive = true)` is iOS `.glassEffect(.regular.interactive())`:
  while a finger is down the glass swells to 108 % on a soft spring and a
  white radial light follows the finger (16 % in dark, 30 % in light), fading
  out over 260 ms on release. It watches the initial pointer pass without
  consuming, so the control's own click still runs. It is on for the glass
  circles (back button, icon buttons, chat header circles and pills, the
  Market bar, the walking chip, live touch); panels and the composer stay
  still, as iOS keeps non-interactive glass still. Left out: iOS
  `GlassEffectContainer` morphing, where neighbouring glass shapes melt into
  one as they approach; Compose has no equivalent, and the shell bar's
  capsule already draws its cells as one piece.
- Glass that floats over moving content refracts it: the bottom bar (the
  shell's `FmBackdropHost`), the chat header and composer (section above),
  and, with a background picture set, the wallpaper. Pushed pages keep an
  opaque `bgBase` bar with glass buttons and the scroll-edge fade under it,
  which is what the iOS 26 bar looks like over a plain background; the
  action bars at the foot of pushed pages are glass capsules drawn with the
  solid fallback because they sit below the content, not over it.

## 31. Health tools, Aquarium and Pocket mode (sync ledger S048, S049, S057)

Ported on `android-health-aquarium` from iOS `Features/Tools`,
`Features/Aquarium` and `Shell/ShellPocket.swift`.

### Health and wellbeing tools (S048)

Routes `Steps`, `Water`, `Sleep`, `SugarBreak`, `Stretch` and `FoodGuide`
(paths `steps`, `water`, `sleep`, `sugar-break`, `stretch`, `food-guide`;
package ids the same) sit after Calories in the personal hub group, as iOS.
`feature/health/HealthFeature.kt` registers the six screens. The logic lives
in `core/.../health/` with the iOS pref keys (`HealthPrefs`) and the same
`/api/steps`, `/api/water`, `/api/sleep`, `/api/sugar-break` and
`/api/stretch` bodies, so web and desktop read the same days.

The app has no Health Connect dependency, and none was added. Where iOS reads
or writes Apple Health, Android uses the phone's own sensors or leaves the
part out:

- **Pedometer.** `Sensor.TYPE_STEP_COUNTER` replaces Core Motion and HealthKit.
  The counter counts from boot, so `StepsDailyLog` turns readings into day
  totals in `PrefStore` (`health.steps.android.days`, `.last`), notices
  reboots, and splits steps that span midnight by waking hours. Counting
  starts when Steps is switched on; earlier days need Add steps. The
  accelerometer (about 50 Hz, converted to g as Core Motion reports) feeds the
  ported `StrideMeter` for cadence and running only while the app is open and
  something listens. Distance is steps x 0.72 m and floors and active energy
  are 0, since Android has no pedometer distance. `ACTIVITY_RECOGNITION` is
  asked for when Steps is turned on. `WalkActivity` is iOS
  `WalkActivityController` without the Live Activity. Synced days carry
  device `android`. The tap-tempo heart card keeps its logic but not Save to
  Apple Health.
- **Sleep.** iOS reads Core Motion activity history; Android has none without
  Play Services, so `SleepRestLog` keeps 8 days of still/moving spans from the
  step counter (under 0.02 steps a second is still; a reboot is unknown) and
  the ported `SleepDetector`, `SleepSchedule` and `SleepUsageLog` run on it.
  A ride in a car reads as still; the usage log, the schedule window and the
  3-hour minimum keep false nights out. Apple Health sleep reads and writes are
  left out.
- **Water Intake.** Same model and screen; the Apple Health share card and
  its import/export are left out. Drinks an iPhone imported still show
  "From Apple Health".
- **Stretch Break.** The guided session, pose interpolation and logging
  rules are the iOS ones; cues are spoken with `TextToSpeech`, which cannot
  pause mid-line, so resume repeats the line. Sessions log with source
  `android` and `health` false; workout writes are left out.
- **Sugar Break.** Same model, screen and contract card (drawn on an
  `android.graphics.Canvas` at 3x and shared as a PNG through the share
  sheet). Apple Health sugar grams, the per-day grams and the "log that I had
  sugar" nudge are left out.
- **Food Guide.** The catalog (99 products, 31 recipes), scoring and diet
  filters are the iOS ones and run on the phone. The budget comes from the
  Calorie Counter overview (or 2,000 kcal). The 14-day HealthKit nutrition
  read is left out, so the guide cannot spot low iron and the like; it uses
  the everyday fiber and potassium drivers and says the history is not
  available.

### Aquarium (S049)

`core/.../aquarium/` is a line-for-line port of iOS `AquariumTank` and
`AquariumFish` (tricks, swimmers, the seeded FNV species and colour pick, and
the iOS tests with the same seeds); CoreGraphics types became
`AquariumPoint`/`AquariumSize`/`AquariumRect`, and the Genie landmark types
the tank reads live beside it. "aquarium" is a phone package as on iOS
(`PhonePackages.AQUARIUM`, key `packages/aquarium/on`, off until installed on
this phone from the Market); `PackageGateFeature.publish()` refreshes
`AquariumFeature.on`. `feature/aquarium/AquariumLayer(hidden, floor)` draws
the tank on a Compose `Canvas`, steps it from `withFrameNanos` at about 30 fps,
stops in the background or while hidden, never takes touches (iOS
`allowsHitTesting(false)`), and maps reduce motion to an animator scale of 0.
It is an overlay of the home shell, as iOS `HomeShell`:
`AquariumLayer(hidden = composerOpen || keyboardUp || voiceOpen, floor = <top of the bottom bar>)`
in `shell/HomeShell.kt`, which belongs to the shell owner. Until agents feed
`AquariumFeature.setSwimmers` (iOS `AgentsScreen.swimmers`) the tank shows the
three guest fish, and with no screen scan feeding `setLandmarks` the peek,
boop and read tricks do not fire; Android has no Genie spider, so its tricks
stay idle.

### Pocket mode (S057)

`feature/pocket/PocketMode.kt` is iOS `ShellPocket`: `PocketMode.lock()` /
`unlock()` and `PocketLockOverlay()`, drawn above everything in
`Features.Root` (iOS presents it as a full-screen cover). The lock screen is
black with the date and a 72 sp clock that ticks each minute, the Glasses
Alert radar and counts (or "Glasses Alert is off"), a pings card (unread
pings and local pings, newest three titles) and a walking card from
`WalkActivity` (the walking figure badge and steps a minute while stepping;
"Not walking" or "Walk detection is off" otherwise), then the slide-to-unlock
slider that unlocks past 85 percent of its travel and springs back short of
it. Back does nothing while locked, the system bars hide (a swipe shows them
for a moment) and the screen stays on. Lock plays a commit haptic and unlock
a success one. The copy and slider rule are `core/.../pocket/PocketText.kt`.
`PocketModeRow(onClose)` is the You hub row ("Pocket mode", or "Pocket mode,
walking" with a pulsing walking icon while `WalkActivity.moving`); the hub
panel belongs to the shell owner, who adds it to `YouHubContent`. The iOS
Settings button for Pocket mode sits in its Suggestion mode card, which
Android does not have yet.

## 32. The Genie spider and outreach (sync ledger S047, S058)

The iOS Genie ([GENIE.md](GENIE.md#android)) on Android. Pure logic is in
`core/.../genie` (tests `GenieOutreachTest`, `GenieAutoSendTest`,
`GenieGravityTest`, run with `./gradlew :core:coreTest -Pfilter=genie`); the
views are in `app/.../feature/genie`. Same storage keys and server routes as
iOS, so a user switching phones keeps the same switches.

- **Spider.** `GenieOverlay` is mounted in `Features.Root` beside
  `PingCelebrationOverlay`, so it sits over the whole app, and only the 92 dp
  spider box takes touches. `GeniePainter` is iOS `GeniePainter` drawn with a
  Compose `Canvas` in the same 100-unit box (aura, tail, eight legs, head,
  violet turban with silver band, plume, gem, six eyes, mouth); trick poses and
  particles are left out. It walks the arena of `GenieArena` (screen sides, the
  ceiling under the status bar plus 56 dp, the floor at `ShellBar.clearance()`)
  with `GenieGravity.crawl`, `wander` every 5 to 13 s and `silk` from the
  ceiling, all ported from iOS with the same numbers.
- **Tilt.** `TYPE_GRAVITY` (rotated to the screen) is the gravity
  `GenieGravity.fly` pulls with, both across and down the screen, as the iOS
  App Store build does since 64807bab53; it also tilts the body (`leanX`). A
  push over 0.25 g from `TYPE_LINEAR_ACCELERATION` knocks it loose, iOS's
  `loosen`; the liquid tumble, flat-on-a-table glass walk and keep-off-buttons
  placement are not ported (they lean on iOS's accessibility screen scan).
- **Touch.** Tap shows the next `GenieHints` line at the top for 5 s
  (`genie-line`); drag and flick fly it with the finger's velocity; a gentle
  release near the left or right edge tucks it a third off screen
  (`genie/parked`). Long-press opens the menu: Talk, Surprise me, Someone to
  reach out to, Stop suggesting people, Stay put, Come out, Reset size and
  place, Hide the spider (`genie/hidden`).
- **Badge cheers (da641ed9a5).** `PingCelebration.badgeLine` (core `model/`)
  turns a fresh `badge` ping into "You earned Stargazer!" or "You reached
  Silver Fan Club!"; the overlay shows it and plays the cheer when
  `NotificationService.celebration` counts up.
- **Voice.** Talk opens `GenieTalkBubble`: the composer's `SpeechDictation`
  (Android `SpeechRecognizer`) in, `TextToSpeech` (pitch 0.92) out, the last
  three lines shown. `GenieChat` keeps four exchanges for 10 minutes, answers
  "say …" and "stop listening" without a model, and otherwise asks the saved
  agent key's provider (`AgentCredentialStore`, the strongest model of that
  provider). Android has no Apple Intelligence or on-device Gemma here, so
  without a saved key the Genie says it needs one. The iOS tool loop
  (open screens, read the latest, start agents) and always listening are not
  ported yet.
- **Outreach coach.** `GenieOutreachCoach` checks every minute and shows a
  never-contacted CRM contact beside the spider (`GenieOutreach.pick`, 2 h
  apart, 3 min after the app comes forward, 30 s on screen, `genie/outreach_*`
  keys). Tapping the face writes the email with `GenieOutreach.write`
  (the saved agent key's model, 90 s, else the template) and opens it in Zoho
  Mail on the CRM's region with the organization's other members on cc
  (`memberEmails`, read from the raw CRM reply in `GenieCrm` so the shared
  `CrmOrganizationData` is untouched), else the share sheet; investor
  pipelines first mint a tracked deck link.
- **Suggestion mode and the mail chip (S058).** `ShellSuggestions`
  (`shell/suggestion_mode`) refreshes every 10 minutes; `ShellMailChip` is the
  44 dp portrait with today's count (top right, accent while auto-send is on,
  with the 864 s countdown ring redrawn each second), the stage mark (bottom
  right: search, edit, send, check for 3 s, pause on the warning colour) and a
  spinner while an email is in progress. Tap writes to the contact;
  long-press turns auto-send off at once, but turning it on (long-press or
  the accessibility action) first asks "Send emails for you?", because it
  sends the first email immediately and then up to 100 a day without showing
  each one. Contact pictures are read with a bounded loop (`readNBytes(int)`
  exists only from Android 13) and decoded downsampled to about 256 px, so a
  huge picture in CRM data cannot exhaust memory. `SuggestionModeRow` is the You hub switch.
- **Auto-send.** `ShellAutoSend` with `GenieAutoSendPace` (`shell/auto_send*`)
  sends one email every 864 s, 100 a day, through `POST
  /api/crm/<org>/outreach/auto` to the best email contact in a CRM with Zoho
  connected, skips refused contacts (up to five a turn), pauses with the
  reason on anything else, and when the server has no model posts the
  phone's draft back. It runs only while the app is in front. Every send opens
  `GenieAutoSendReviewHost`'s sheet (1 to 5 stars and a note, `POST
  /outreach/rate`, "Sent · 2 more" queue). Messages go through
  `GenieMailStatus.say` to `GenieMailNoticeLine` (`shell-mail-notice`) and the
  device-local Pings (`InboxFeature.recordLocalFailure`, which keeps its 8 an
  hour limit unlike iOS).
- **Shell hooks.** The top bar, the notice under it and the You hub belong to
  the shell; they call `ShellMailChip()` (top bar, beside Glasses Alert),
  `GenieMailNoticeLine()` (under the top bar) and `SuggestionModeRow()` (You
  hub). `GenieSpiderSwitchRow()` is the Settings "Show the Genie spider"
  switch.
- Left out on purpose: hand play (TrueDepth), screen plays and the
  accessibility screen scanner, tricks and genie effects, vitals (health from
  code and marketing), the aquarium and pocket mode.

### Shell wiring for the Genie, Pocket mode and walking

The merge into `feat/add-adnriod-updates` placed the hooks where iOS has
them. The top bar's chip row (iOS `ShellTopBar`) is Glasses Alert,
`StepsWalkChip` and `ShellMailChip`. `StepsWalkChip`
(`feature/health/StepsWalkChip.kt`) is the glass walking badge that shows
while `WalkActivity.stepping` and the Steps package is placed, and opens the
Pedometer. `GenieMailNoticeLine` sits under the link-problem line, as iOS
draws the mail notice under the top bar. The You hub has Pocket mode, then
Suggestion mode, then Diagnostics, the order of iOS `YouHubSheet`. Settings →
General ends with a Genie card (the spider switch) and a Suggestion mode card
(the switch, its note and the Pocket mode row), as iOS `GenieSpiderCard` and
`SuggestionModeCard`. The iOS DJ now-playing chip comes with the DJ studio
app (section 33), which is not merged.
The spider's frame clock follows iOS `GenieSpiderLayer.frameRate`: 24 fps
at rest or crawling, 30 during a reaction, 60 only while it is held or flying.
Every frame redraws the window, which a software-rendered emulator feels far
more than a phone does.

## 33. Video Mix DJ Studio (sync ledger S045): shared logic only

The package id is `video-mix-dj-studio` and the route `Route.VideoMixDjStudio`
(paths `video-mix-dj-studio` and `dj-studio`) exists in core, but no screen is
registered for it yet, so the Home tile and Market entry open nothing of
their own.

- **Merged: shared logic (core `djstudio/`).** Playlists, tags and smart
  playlists (`DjCrate`, `DjSmart`), AUTOMIX, BPM detection and the gain curve
  (`DjAudio`, `DjStudio`), play counts and jam sessions (`DjJam`, and
  `DjJamApi` for the jam server routes), with their iOS tests. This code
  parses links and builds addresses; it fetches nothing itself.
- **Not merged: the studio app (pull request #475, branch `pr/475`).** Its
  audio path loaded YouTube's embed page in a 2x2, 1%-opacity WebView,
  rewrote that page to inject a script that pulled out the raw stream
  address, then downloaded the whole stream and played it through its own
  `AudioTrack`, outside YouTube's player and its ads, and searched by
  scraping YouTube's results page. That breaks YouTube's terms of service and
  Google Play's policy, and fixing it means replacing the playback engine the
  pull request is built around, so it was left open. iOS plays through the
  YouTube IFrame player; an Android studio should do the same (a visible
  player, YouTube's own controls and ads) before it lands. The top bar's
  `DjNowPlayingChip`, `DjMediaService` and the `dj-share` file path come with
  it.


## 34. Fixes made when the parity pull requests merged (2026-10-20)

- **Stickers in bubble conversations.** Direct chats and private rooms draw
  bubbles (`ChatBubbleRow`), which did not show `row.stickers` although the
  long-press menu offered Add sticker. The stickers now sit on the bubble's
  bottom corner and toggle through `ChatBubbleActions.onSticker`.
- **Screen capture.** `CaptureOverlay` says "Copied to clipboard" only when
  the copy succeeded, else "Could not copy the picture".
- **Home drag.** `shellDragSource` ends the drag in a `finally`, so a gesture
  cancelled mid-drag (a tile dragged across sections) no longer leaves the
  floating preview on screen.
- **Sensors.** `StepsSensors.refresh` is `@Synchronized` (it is called from
  the main thread and from background threads, which could register a
  listener twice or leave one registered), and `DirectLiveMotionFeed.stop`
  always unregisters, so a half-failed start cannot leak its listener.
- **Backups.** `nexus/` (scan history) is excluded from backup and device
  transfer next to `chat-history/`; the app already sets
  `allowBackup="false"`.
- **Vault import.** `VaultTransfer.readCsv` refuses an archive whose CSV entry
  is itself an archive (`zip_unreadable`) instead of following it, so a
  self-containing ZIP can no longer recurse until the app crashes
  (`testAnArchiveInsideTheArchiveIsRefusedNotFollowed`).
- **Kept as on iOS, worth a product decision for both apps.** A direct chat
  shares the phone's motion with the other person while it is open
  (`DirectLiveFeature`, as iOS `MessagesConversationView.startSharing`), with
  no setting; and NEXUS counts `10.0.0.0/8` and `100.64.0.0/10` as the local
  network without asking for confirmation, which on mobile data covers the
  carrier's network (`IPv4Addr.isLocalScope`, as iOS `IPAddress`).

## 35. Chat relay hardening (2026-10-22)

Android's side of [chat-relay-hardening.md](../app/chat-relay-hardening.md)
(R1 to R7). Shared rooms are joined by every device of an instance, guests
included, so the relay now carries only public content and nobody can speak,
edit or delete as somebody else. Nothing here talks to older builds.

- **R1, bundles bound to the node key.** The node id was already the `b64u`
  Ed25519 public key of `Identity` (`DeviceIdentity.nodeId`), so it did not
  change. `ChatNodeKey` (`C/crypto/ChatNodeKey.kt`) signs with that key;
  `RoomSeal.nodeKey` adds `nsig` over
  `forkmesh-chat-e2ee-node-v1\n<room>\n<kid>\n<idk>\n<acct>` to every
  open-room bundle (relay, office rooms, `#mesh`), and an open room drops a
  bundle whose `node` is not a canonical 32-byte key or whose `nsig` fails
  (`ChatV3.verifyNodeBinding`). Strict rooms keep the v3 bundle.
  `ChatNodeBindingTest` replays `conformance/chat/v3/node-binding.json`
  (copy in test resources) including its three rejects.
- **R2, nothing private on the relay.** `RelayFrames.isPrivate` (`dm`,
  `invite`, `cove-invite`, `cove-open`, `private: true`, a conversation
  starting with `@`) is refused by `RelayRoom.send` and dropped on receipt
  before the history, the roster hooks or `ChatExtras` see it. `ChatExtras`
  lost its relay private channels and invites; private channels are office
  rooms created with `POST /api/chat/channels` (the existing New channel
  sheet). Message on a person, a reply to "@name", and share targets for
  people go through the person's server DM; a guest has no Message action.
  `ChatHub.withoutRelayPrivate` drops `@peer` conversations and relay private
  channels, with their replies, reactions, stickers, edits and receipts, from
  saved history every time it is loaded, so the first start with these rules
  purges them and later starts find nothing to purge. `#mesh` keeps its own
  frames. Android never sent cove invites over the relay (an invite is a
  server cove grant, `CovesModel.invite`), so there was nothing to move.
- **R3, only authenticated frames act for a person.** Every person frame is
  sealed, public channel chat included (it used to go in the clear).
  `RelayFrames.authenticated` fills a missing `senderId` with the sealed
  sender's stored node (RoomSeal already rejects a different one). An
  unsealed frame is accepted only when the envelope has `srv: true`, only as
  `chat` in a `#` channel, with its sender id prefixed `srv:` unless it
  already names an integration (`webhook:`, `app:`, `system:`), account
  claims removed and `accountKind: integration`; it never reaches the roster,
  and edits, deletes, reactions and thread frames are never taken from it. A
  delete for a message whose author is unknown is ignored. Office rooms do
  the same (`OfficeChannelService.accept`) in every server channel room,
  public or private, the `#email-log` notices channel included, live or from
  the HTTP history; DM rooms never take an unsealed frame. A server frame
  with `serverVisible: true` (webhooks) is marked "server-visible" next to
  its sender name (`ChatMessage.serverVisible`, kept in saved history as
  `sv`). Android has no channel webhook settings, so creating or rotating a
  webhook stays on the web and desktop. ForkBot replies posted by this
  phone keep its own `senderId` and name the bot in `bot`. Since any sealed
  sender, a guest too, can set `bot`, a bot line shows as
  "<bot> · via <verified account or Guest ####>" (`RelayFrames.botLabel`)
  in the transcript, thread replies, office rooms, list previews and pings;
  its sender id is `bot:<node>:<bot>`, and labels are redrawn when
  `NodeOwners` confirms the account (`relabelBots`). A ping filed before
  the answer keeps the guest label.
- **R4, accounts checked with the server.** `NodeOwners` asks
  `GET /api/chat/node-owner?node=` once per node and hour (a failure is
  retried after a minute); the roster shows a peer's `ownerUser` only when it
  matches, otherwise the peer is a guest. Sign-in already binds the node key
  (`AuthController.bindLogin` sends `publicKeyB64url`).
- **R5, blocks kept by the server.** Android had no account blocks.
  `ChatBlocks` keeps the list (prefs `chat/blockedAccounts`), pushes it with
  `PUT /api/chat/blocks` on every change, and at sign-in reads it with `GET`,
  unless a push failed, in which case it pushes first. `ChatHub` hides a
  blocked account's messages and DM. A DM tile's long press offers Block;
  the channel list shows a Blocked section with Unblock.
- **R7, less about you.** Relay frames never had a time zone, host name or
  `dg`; the Solana address now goes out only with "Accept tips in chat"
  (Settings, Funding and payouts, `AccountSettings.chatTipsEnabled`, off by
  default). `#mesh`: bundles use the node id as `acct`, frames carry no
  `ownerUser` (and a received one is ignored), `resealOwnHistory` is gone and
  backfill resends only frames already sealed to that device, the beacon id
  is random (new at start and on a network change; a beacon is matched to a
  linked peer by address), `setFocus` goes only to links of the DM partner's
  approved devices (`DirectMessageService.partnerKids`), and the `#mesh`
  header and info sheet say "Anyone nearby with ForkMesh open can read
  #mesh." Android has no setting that turns the mesh off, so it still starts
  with chat.

## 36. Sync ledger, third round (2026-10-22)

- **Agent page actions (S074).** `FmNavBar` gives its trailing actions the bar
  width less 52 dp for the Back button (never under 184 dp,
  `FmNavActionsRoom`) in a horizontal scroller that starts at the trailing
  end, so a page with many actions scrolls them instead of drawing the first
  one under the Back button. The unified agent page's action row does the
  same: its buttons scroll inside the width left after the badges.

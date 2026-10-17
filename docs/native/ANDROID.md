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
  - every `direct-messages` frame, foreground or not, first posts
    `/api/chat/direct-messages/<id>/delivered?ref=<ref>`
    (`DirectMessagePreviews.confirmDelivery`), so the sender sees Delivered
    while this phone has no DM socket open
    ([chat-private-rooms.md](../app/chat-private-rooms.md#delivery-ticks)).
- **Relay room** (`C/chat/RelayRoom.kt`): every frame encrypted twice
  (`RoomCrypto` room key and a v3 `RoomSeal`, open trust, scope = room name),
  except public `#channel` chat. `@peer` DMs are sealed to the devices whose
  bundle `node` is the recipient plus this account's own devices. v2 frames
  are ignored. Dial timeout 15 s; ping every 25 s; reconnect 1, 2, 4, 8,
  16, then 30 s; presence every 60 s, peers dropped after 180 s. Envelope
  `version` matches iOS.
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
  multicast beacon (TTL 1) carrying only `{t, v:3, id: kid, port, ts,
  reply, control, node}`, direct TCP links, local-address checks before any
  socket, subnet broadcast and a paced /24 sweep where multicast is filtered,
  Bluetooth RFCOMM, and a BLE scan for the iPhone's GATT service
  (`C/chat/MeshBle.kt`, wire format in
  [networking.md](../desktop/networking.md)). Needs a Wi-Fi multicast lock.
  Every link starts with plaintext `fm-link-hello` and `fm-link-auth`
  frames (length-prefixed like every frame) and then carries AES-GCM frames
  with per-direction counters; a bad frame closes the link. Peers are keyed
  by the authenticated kid; names travel in the sealed mesh `hello`. The
  phone forwards every new `#mesh` bundle to its other links and relays
  sealed frames it cannot open to links whose device is addressed, and when
  it learns a device it has no link to it re-seals its own history to that
  device: without that a late joiner two hops away could never read anything,
  because a v3 frame can only be opened by the devices it was sealed to. The
  history file (v2) keeps each received message's original sealed frame for
  backfill. The same links carry `direct-sealed`, `direct-ack` and `focus`.
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
  right of their app bar (user request, 2026-10-15): Rate <name> (or Edit
  your rating), Send feedback or report a bug, a divider, then Uninstall (one
  row per organization for organization packages). A screen opened through
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
  `PluginMenu.Items(divided = true)` under their own items, so each screen
  keeps one ⋯. Screens a plugin pushes directly (not by route) carry no menu;
  the plugin's entry screen does. `A/feature/market/PluginMenu.kt` loads the
  catalog and the viewer's review only when the menu first opens, pushes
  `RateScreen` (which loads the review page itself when it was not loaded),
  and opens `ReportBugSheet` with the package: the sheet is titled "Feedback
  on <name>" and `POST /api/bug-reports` carries `package` (left out when
  there is none) so the public issue is tagged with the plugin. Feedback
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
    manifest receiver `CallActionReceiver`). Both carry the call id and act
    only on that invite; Accept from the notification accepts at once only
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
  - **Limits.** Calls ring only while the app is running and the room
    socket is connected: the invite arrives over the DM socket, and there is
    no call push yet, so a killed or long-backgrounded app does not ring.
    Ringing a killed app needs a server-sent push that starts the
    notification (server work). The ringback tone follows the voice-call
    stream, so it is quiet through the earpiece on some phones.

## 15. You hub, diagnostics and docs

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
  icon (`AccountFeature.ProjectSlot`, opens the project drawer), the Glasses
  chip, then the new-agent button and the avatar (36 dp, 12 dp corners). A
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

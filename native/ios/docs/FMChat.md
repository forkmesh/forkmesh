# FMChat

The relay chat room, relay switching, the local mesh room, the local control channel (the phone
driving a desktop, and serving its on-device model to it), node sync, direct messages and calls,
and the chat view models. `import FMChat` (deps FMBase, FMCrypto, FMNet, FMAccount, FMApi,
FMLocalAI). Every service is a `@MainActor @Observable final class`; `addListener`/`removeListener`
(FMAccount `Listeners`) for code outside SwiftUI; each has a `revision` bumped on every change.

## Wiring: `ChatFeature.register()` (ForkMesh/Features/Chat/ChatFeature.swift)

Called by AppServices after Account and Api. Reads SettingsService, AuthService, ApiService,
DeviceIdentity, SecureStore, KeyValueStore, HTTPClient and WebSocketConnector from `Services`
(optional: PerformanceMonitorService, NotificationService, AccountEventChannel,
PushNotificationService, AppLifecycle). Registers:

| Service | Registered as |
| --- | --- |
| `RelayService` | itself and `OwnStatusAnnouncer` |
| `ChatHub` | itself and `ShellChatBridge` |
| `LocalMeshService` | itself |
| `LocalControlService` | itself and `ShellLocalDesktopBridge` |
| `RelaySwitchController` | itself (suspends and resumes node sync, account events, push, pings) |
| `NodeSyncService` | itself |
| `DirectMessageService`, `OfficeChannelService`, `DirectCallEngine` | built by `ChatInstaller` |

It sets `relay.roomPassphraseProvider` (`api.roomChatPassphrase()` with a session),
`control.inferenceProvider` (LocalAI's `LocalInferenceService`, looked up per request),
`UserPresence.instance.prompt = LocalAuthPresencePrompt()`, forwards AppLifecycle, and starts the
mesh, the relay and node sync. The relay-switch overlay is `RelaySwitchScope { content }`.

## Relay (`Relay/`)

- `RelayService`: connection state, channels, current conversation (`#general`, `@nodeId`),
  roster, unread counts, messages, presence. Actions: `connect`, `disconnect`, `sendMessage`
  (shown at once, removed on failure), `deleteMessage`, `switchConversation`, `mark*Read`,
  `markBadgeSeen`, `setOwnStatus`, `resetForRelaySwitch`, `identityChanged`. Wire: frames carry
  `version: "flutter-1.0"` (a wire value); public channel chat uses the room cipher only;
  everything else is RoomSeal-sealed; `persist`/`notify` ride outside the seal.
- `RelayService.groups`: one person row per account. Members key on `owner` (`ownerUser`); a
  member without one (a signed-in web `/chat` tab sends `sender` and `accountKind: "user"` but no
  `ownerUser`) joins the account row whose owner equals its name, so the Online sheet shows
  "jett (you)" once instead of "jett (you)" plus "jett".
- `ChatPersona`: the name the device speaks as: the Display name or account when signed in,
  else `guestChatName(nodeKey)` ("Guest ####"). `stamp(&frame)` adds `accountKind` and
  `ownerUser` to every frame and mesh beacon.
- `RelayProbe`: `check`/`verify` a relay profile, returning status, message, profile, version
  and missing optional features.
- `RelaySwitchController`: `switchTo(id) async -> RelaySwitchResult` (`ok`, `needsSignIn`,
  `offline`, `error`), `removeRelay`, phase and status text, with suspend/resume hooks.
- `CrossRelayCatalog`: repositories per configured relay.

## Local mesh (`Mesh/`)

BSD sockets in the core (`MeshSockets.swift`), not Network.framework, so Linux runs the real
loopback cases. The unicast sweep is sent from the discovery port so a stateful firewall passes
the answer; EHOSTUNREACH means Local Network access is off.

- `LocalMeshService`: `start`, `stop`, `sendMessage` (returns the id; "" when not sent),
  `sendTyping`, `connectToPeer`, `markRead`; state for messages, peers, Bluetooth and network
  activity, the queue and diagnostics. History file: Application Support/local_mesh_history.json.
- `MeshNativeTransport` is a protocol; the app passes `CoreBluetoothMeshTransport` (BLE GATT,
  peripheral and central), tests get `UnavailableMeshTransport`. `MeshBle`/`MeshBleOutbox` hold
  the platform-free parts (UUIDs, dial tie-break, chunking, long-write reassembly). Bluetooth
  permission is requested only once `#mesh` has been opened.
- Constants: `meshConversation` ("#mesh"), `meshDiscoveryPort` (48732), `meshMulticastGroup`.

iOS needs `NSLocalNetworkUsageDescription`; multicast also needs the
`com.apple.developer.networking.multicast` entitlement (the subnet sweep works without it).

## Local control (`LocalControl/`)

- Protocol functions byte-identical to `desktop/src/LocalControl.cpp` (vectors in the tests):
  `controlTranscript`, `deriveControlKeys`, `controlPairingCode`, `sealControlFrame`,
  `openControlFrame`. `LocalControlConnection.open(...)` gives `request(method, params)`,
  an `events` stream, `sendEvent`, `close`, and the pairing code.
- `LocalControlService`: status (`idle`, `connecting`, `awaitingApproval`, `connected`,
  `failed`), paired desktops (`localControl/pairedDesktops`), the desktop's agents, and actions
  to start, prompt, stop and continue agents, read transcripts and send files. Received files go
  to Documents/"ForkMesh from desktop".

### On-device AI for paired desktops

Opt-in setting `localControl/serveLocalAI` (default false). Only the desktop this phone is
connected to and paired with is answered; the model policy is FMLocalAI's
(`LocalModelManager.modelForPeers`).

Everything travels as control-channel event frames, so it is additive: older peers ignore
unknown events. Requests are not `{id, method}` frames because older phone apps treat any int
`id` from the desktop as an answer to their own request.

```text
phone -> desktop  {"event":"node.capabilities","data":{"ai":{"v":1,"enabled":true,
                    "methods":["ai.info","ai.chat"],"stream":true}}}      on approval and on every switch change
desktop -> phone  {"event":"ai.request","data":{"rid":7,"method":"ai.chat","stream":true,
                    "params":{"messages":[...],"tools":[...],"max_tokens":512,...}}}
                  {"event":"ai.cancel","data":{"rid":7}}
phone -> desktop  {"event":"ai.delta","data":{"rid":7,"text":"Hel"}}                 (stream only)
                  {"event":"ai.tool_call","data":{"rid":7,"tool_call":{...}}}        (stream only)
                  {"event":"ai.response","data":{"rid":7,"ok":true,"model":"...","message":{...},
                    "finish_reason":"stop","usage":{...},"tokens_per_second":38.5}}  exactly one per request
                  {"event":"ai.response","data":{"rid":7,"ok":false,"error":"disabled","message":"..."}}
```

`rid` is a positive int chosen by the desktop. Methods: `ai.info` (`available`, `model`,
`max_tokens`, `busy`, `device`) and `ai.chat` (OpenAI Chat Completions shape, see
[FMLocalAI](FMLocalAI.md)). Errors: `disabled`, `unavailable`, `unknown_method`, `bad_request`,
plus FMLocalAI's `model_unavailable`, `busy`, `generation_failed`. A closed channel cancels
running requests; iOS stops GPU work in the background (`finish_reason: "cancelled"`).

## Node sync (`NodeSync/`)

`NodeSyncService` polls and applies items carried for this node: `start`, `stop`, `syncNow`,
counts and errors; prefs `nodeSync/carried/<relay>` and `nodeSync/applied`.
`LocalNodeSyncApplier` works against the protocols in `NodeSyncWorkspace.swift` (FMChat cannot
import FMCode); FMCode's `LocalNodeSyncAdapter.swift` implements them and must throw from
`readFile` for a missing path, create parent directories on write, stage deletions, and throw
`NodeSyncWorkspaceError.nothingStaged` for an empty commit. Without an applier the node acks
nothing.

## View models (`ViewModels/`)

- `ChatHub` (also `ShellChatBridge`): `rows: [ChatRow]` (message, grouping, queued, deletable,
  receipt, attachment, mentions), title, badge, member groups, channels, `select`, `delete`,
  `sendChatText`. App commands: `/apps` and installed apps' slash commands run instead of
  posting; mentionable apps in public office rooms get the message via `POST /api/apps/mentions`.
  Statics for segments, time labels, grouping and day dividers.
- `RelaySettingsModel`, `AddRelayModel`, `AddProjectModel`, `RelayNetworksModel`: relay and
  project management.
- `LocalDesktopText`, `LocalNewAgentModel`, `LocalAgentModel`: the desktop-control screens.
- `UserPresence`: `confirm(reason:dialog:) async -> Bool` (local authentication prompt).

## Chat history file (`Threads/ChatHistoryStore.swift`)

`ChatHub.attachHistory` restores `relay-<scope>.json` from Application Support once; on every
relay change `scheduleSave` saves again 2 s later. The main thread only takes a copy-on-write
snapshot (`historyParts()`); bounding (200 messages and 2 MB of text per channel), retention
filtering, encoding and the protected atomic write run on a serial utility queue
(`ChatHistoryWriter`), which skips the write when the snapshot is unchanged. Only `#` and `@`
rooms are written; direct-message history is never stored on disk.

## Disappearing messages

Messages disappear after the account's retention (FMApi `ChatRetentionService`; default seven
days, shorter per account or per chat, synced through the server). The hub hands one
`ChatExpiry` closure to `RelayService`, `LocalMeshService`, `ChatExtras` and
`OfficeChannelService`; each drops expired messages on arrival and restore and in
`pruneExpired()`, run every five minutes and on resume. Retention keys are shared with other
clients: `#general`, `@<peerId>`, `channel:<roomId>`, `#mesh`.

## Conversations (direct messages and private groups)

`ChatHub.isConversation(_)` selects rooms drawn as message bubbles: relay DMs (`@<peerId>`),
server DMs (`direct:<id>`), relay private channels and private office rooms. Public channels and
`#mesh` keep the transcript layout.

- **Layout** (`ChatConversationLayout`): time lines after 15-minute gaps or a new day, runs of
  one sender, sender names in groups, a receipt mark under own messages, large emoji-only
  messages.
- **Shared look** (`Threads/ChatLook.swift`): background, group name and group photo travel as
  sealed `look` frames, last-writer-wins on `(ts, id)`; each change is shown as an event.
  Pictures are capped at 600 KB because a frame is base64-encoded three times and the room
  WebSocket refuses messages over 2 MiB.
- **Media**: `ChatHub.sendMedia` caps media at 600 KB; photos are re-encoded as JPEG (longest
  side 1,600), voice messages recorded as 32 kbit/s mono AAC, up to two minutes. Pasted images
  (`ChatTextField`, a UIKit text view so Paste offers images) go through the same path.
- **Group management** (`MessagesDetailsScreen`): name, photo, people, background, photos,
  retention and encryption summary; leave a relay private channel; delete history locally.
- **Live touch and drawing** in direct messages: `DirectLiveEngine` batches taps, strokes and
  motion samples into sealed `live` frames at the relay-announced pace (wire format in
  `docs/app/chat-private-rooms.md`).

## Workspace features

- **New message** (`ChatNewMessageSheet`, `ChatRecipientPicker`): recipients from
  `ChatDirectory` (relay roster, DM partners, recent recipients, office members, account search
  of two or more characters). `ChatHub.conversation(for:)` routes one channel to that room, one
  person to an existing or new server DM (else a relay DM when they are online), several people
  to a private office room containing exactly them; mixing people and a channel is refused.
- **`ChatWorkspace`** (per phone, KeyValueStore `chat/workspace/<project id>/...`): saved
  items (200), muted and starred conversations, drafts (4,000 characters), recent recipients
  (20), bookmarks (50 per channel), canvases (20,000 characters), notification levels.
- **Unread**: opening the Chat tab clears only the tab badge (`markBadgeSeen`); each
  conversation's dot clears when that conversation is opened.
- **Search** (`ChatSearch`): `from:`, `in:`, `has:file`, `has:link` and words over history
  loaded on the phone, newest first, 200 at most; Mentions and Saved tabs.
- **Message actions**: save, reminders (local notifications), share as a quote.
- **Composer**: `#channel` and `:emoji:` completion, a Format menu (`*bold*`, `_italic_`,
  `~strike~`, code, quote, list), slash commands `/msg`, `/dm`, `/shrug`, `/me`, `/mute`,
  `/search`, `/saved`. `ChatRichText.spans` renders Slack-style markers only at word
  boundaries, so `snake_case` stays plain.
- **List preview** (`ChatListPreview`): the last visible message per conversation, plain text up
  to 160 characters, "You:" or sender prefixes, a short time label.
- **Smart mentions** (`ChatSmartMentions`): person, you and broadcast (`@here`, `@channel`,
  `@everyone`) tags outside code; a warning above 6 other people.
- **Everyone channels** (`ChatEveryone`): `kind:"everyone"` office rooms, the relay
  `#general` shown as "World", and the member directory from
  `GET /api/chat/channels/<general>/members`. `ChatHub.canLeaveChannel(_:)` is false for
  the fixed everyone rooms and World, so they never offer a local Leave action; ordinary
  channels do.
- **Channel list on open**: the list is saved on the phone and shown as soon as chat opens,
  then refreshed from the server, retrying if the first load fails.
- **People**: the Online tile opens everyone online; a name opens the DM with that account, an
  avatar opens the person card (`ChatPersonSheet`: picture, presence, status, bio, local time,
  Message button, profile link). Local time uses the profile's time zone, else the time zone the
  person's phone shares in chat (`ChatLocalTime`).

## Direct message delivery

Rules shared by all clients are in `docs/app/chat-private-rooms.md` ("Server copies of direct
messages"). On iOS they live in `DirectMessageService`:

- **Sockets** for the open DM and every DM with unread messages, at most
  `DirectMessageFrames.maxWatchedRooms` (16). Reconnect after 1 s doubling to 30 s with a fresh
  `room-access` ticket; nothing reconnects in the background, and resume reconnects at once.
- **R1.** A key advertisement (`e2ee-key`) is never acknowledged, so it stays for the other
  person's other devices.
- **R2.** The first sealed frame with no wrap for this device triggers one persisted key
  advertisement.
- **R3.** Each durable send is kept in a per-DM outbox (64 entries, 7 days) with its relay ref
  (first 40 hex of SHA-256 of the exact text sent) and the key ids it was wrapped for. When a
  new peer key appears, entries not wrapped for every known peer are resealed and resent
  (`resend: true`), and the old refs are sent in `forkmesh-superseded` frames.
- **Dedupe.** A resend keeps the message id, so receivers drop the second copy.
- **Unconfirmed sends.** A DM the server has not confirmed within a few seconds is sent again
  over a fresh connection, so it does not stay at "Sending".

### Receipts, deletes, reactions and replies

- **Receipt marks** (`ChatReceipt`: `sending`, `sent`, `delivered`, `read`; only moves
  forward). DMs: `forkmesh-recipients` makes a ref `sent`, `forkmesh-delivered` `delivered`,
  `forkmesh-read` `read`. Office rooms reach `sent` only. Drawn by `ChatReceiptMark` inside
  the sender's own bubble.
- **Delete for everyone**: a sealed `delete` frame, removal from the outbox, and a
  `forkmesh-superseded` for the original ref. A received delete is honoured only from the
  target's sender; a delete arriving before its target is held (64 per room). Removed ids are
  remembered (2,000) so replays cannot restore them, and matching delivered notifications are
  removed.
- **Reactions, edits and thread replies** are `reaction`, `edit` and `thread-reply` frames
  through `DirectMessageService.sendFrame` or `OfficeChannelService.postFrame`, applied by
  `ChatExtras.applyRoomFrame` (edits author-checked; an early edit is held).

## Message text in push notifications

The push itself carries no plaintext. A DM push carries `ref`, `keyVersion`, `conversation` and,
when it fits in 4 KB, the sealed `frame`; a channel push `channelId` and `ref`. The notification
service extension asks with the account session for the room passphrase and the stored frame
(`GET /api/chat/direct-messages/<id>/pending?ref=` or `/api/chat/channels/<id>/pending?ref=`,
side-effect free on the server), decrypts on the device and shows sender and text. When the
stored DM row is already gone (the desktop received and acknowledged it first, which is the usual
case) the route answers with an empty `frame` and `PushChatOpener.withPushedFrame` uses the one the
push carried. Any failure leaves the server's fallback alert. The extension never acknowledges, marks read or replies.

The one thing it sends is a delivery receipt: for every DM push, whatever the preview mode, it
posts `POST /api/chat/direct-messages/<id>/delivered?ref=` (`PushChatRequest.deliveredPath`) so the
sender's mark turns Delivered while the app is closed, the way Signal's does. The server copy is
kept, because the app reads it when it opens. The banner waits for that request (5 s timeout)
before it is posted, since iOS may end the extension as soon as the banner is handed over.

Logic is in **FMPush** (`ForkMeshCore/Sources/FMPush/PushChat.swift`, deps FMBase and FMCrypto):

- `PushChatRequest.parse` validates `ref` (40 lowercase hex), `conversation` or `channelId`.
- `PushChatOpener.open` checks the pending answer names the same conversation, decrypts the
  `RoomCrypto` envelope, unseals with the device's X25519 key, and accepts only `chat` and
  `thread-reply`. DMs must be sealed.
- `PushRoomKeys` caches derived room keys (one keychain item, 32 entries) so only the first push
  of a room pays the 210,000 PBKDF2 rounds.
- `PushPreviewMode` ("Notification content" setting): name and message (default), name only, or
  neither.

The extension reads the device key named in the shared push credential
(`Shared/PushCredentialStore.swift`) and links only the `ForkMeshPushCore` product.

## Voice and video calls

`DirectCallEngine` (`Direct/DirectCalls.swift`) is the signaling state machine for `direct:`
rooms; signaling rides the DM room's sealed socket (`direct.sendCallFrame`, `onCallFrame`).
Missed calls post the same "Missed audio/video call" line as the web.

- **Media.** `WebCallPeerFactory` loads `direct-call.html` into a `WKWebView` with the base URL
  `https://call.forkmesh.com/` (a secure context for `getUserMedia`) and drives it through the
  `fmCall` message handler. The page is inlined and never fetched from that host; the host is
  only what WebKit names if it ever shows its own prompt, so it is a real ForkMesh domain rather
  than `.invalid`. Camera and microphone are granted only to that origin, after the one-time
  system permission prompt, by the `WKUIDelegate` method
  `webView(_:decideMediaCapturePermissionsFor:initiatedBy:type:) async`. That is the Swift name
  of the async form; the older spelling `requestMediaCapturePermissionFor:initiatedByFrame:`
  without a `decisionHandler` matches nothing (the compiler only warns "nearly matches"), so
  WebKit fell back to asking "Allow call.forkmesh.invalid to use your microphone?" on every
  call. `FaceChangerPage` uses the same method. `app/tools/sync_native_call_faces.py` generates
  the page for both apps;
  `app/tests/test_chat_call_faces.py` fails when they drift.
- **Faces, voices and backgrounds** are engine state saved under `chat.callFace`,
  `chat.callVoice` and `chat.callBackground` (custom images in Application Support with complete
  file protection). Face landmarks and person masks come from Apple Vision
  (`CallFaceLandmarks`: `VNDetectFaceLandmarksRequest`, `VNGeneratePersonSegmentationRequest`),
  returned to the page; the page falls back to its own tracker after three failures. Details:
  `docs/app/chat-direct-calls.md`.
- **TrueDepth camera.** On phones with a front TrueDepth camera, `CallDepthCamera` owns the
  camera (WebKit gets audio only). It pairs video and filtered depth, renders each frame upright
  (not mirrored, at most 480 px) and runs Vision on it. It adds per-point depth
  `z = (dFace - d) * fPx / dFace` (pixels, larger is nearer, `[x, y, z]`) and pushes
  `window.fmCallFrame({ image, width, height, regions, mask, time })`. The page draws the frame
  with `stage.pushFrame` and answers `{ event: "frame-done", landmarks, mask }`; one frame is in
  flight. The page controls the camera with `{ event: "camera", id, op: "start" | "stop" }`, and
  `window.fmCallCamera(id, { ok, error })` answers. Any failure (no device, denied, no frame within
  1.5 s, `fmCallCameraFailed`) falls back to `getUserMedia` video. The depth maths
  (`CallFaceDepth`, `CallDepthField`, `CallDepthOrientation`) is in `Direct/CallFaceDepth.swift`.
  Protocol and rules: `docs/app/chat-direct-calls.md` ("iOS TrueDepth camera").
- **Preview and recording.** The pre-call preview shows the WebView immediately (WebKit holds
  capture until the view is visible), with a 30 s watchdog. "Record message" records the page's
  outgoing tracks (`record`, `recordStop`, `recordChunk` of up to 768 KiB, `recordDrop`), up to
  60 s, then sends it as a view-once video.
- **Limits.** Ringing reaches the phone only while the DM's socket is open (foreground); there is
  no CallKit or VoIP push.

## View-once video and voice messages

Wire format and server rules: `docs/app/chat-private-rooms.md` ("View-once video and voice
messages"). Core (`Direct/ViewOnceMedia.swift`):

- `ViewOnceFrames` parses and builds the sealed frame's `viewOnce` field (version 1, `video` or
  `voice`, 32-hex media id, 32-byte key and 12-byte IV, MIME type, duration and size limits,
  `parts = ceil(size / 524288)`) and the relay's `forkmesh-media` state frames.
- `ViewOnceCrypto`: AES-GCM with a fresh key and IV per message, ciphertext followed by the tag
  (the WebCrypto layout), split into 512 KiB parts.
- `ViewOnceTransfer`: create, upload parts, finish (withdraw on failure), download and verify,
  consume, withdraw, status reads. HTTP 410 becomes `ViewOnceError.gone`.
- `ViewOnceStore`: per-message entries; the key is dropped as soon as the state is final
  (opened, expired, withdrawn, failed) and the sender never holds it. Spent media ids are
  remembered under `chat/viewOnce/spent` (512).
- **Held receipts.** Because the phone keeps no DM history on disk, an unopened view-once frame
  is not acknowledged (refs held per message, at most 800) until the message is final.
  A failed offline `consume` is retried later.

App (`Features/Chat/Messages/`): capture with `AVCaptureSession`/`AVAssetWriter` (H.264 480x640
about 1 Mbit/s, AAC 64 kbit/s, 60 s) or `AVAudioRecorder` (AAC 32 kbit/s, 120 s), review before
sending (a button saves the recorded video to Photos), protected temporary files deleted on
send, discard or close. The viewer decrypts in
memory, plays without share controls, consumes as playback starts, blanks video while the screen
is captured and notes screenshots. The sender can unsend an unopened message.

**Captions.** Optional (`chat.videoCaptions`): on-device speech recognition only
(`SFSpeechURLRecognitionRequest` with `requiresOnDeviceRecognition`), word timings grouped by
`ViewOnceCaptions.lines` and burned into the frames with a per-frame Core Image composition
(karaoke-style word highlight). The re-encode uses `AVAssetWriter` with a bitrate that keeps the
file under the 12 MiB limit.

## Link previews

- **Catalog** (`Threads/ChatLinkSites.swift`): 50+ sites in 14 kinds with host matching, badge
  colour, symbol and resolver.
- **Resolvers**: public APIs where available (oEmbed providers, Wikipedia, Hacker News, package
  registries, App Store, Steam, Stack Exchange), otherwise the page's `og:*`/`twitter:*` tags,
  title and description from the first 2 MB.
- **Fetching** uses an ephemeral client (no cookies, no cache), 8 s per request, at most 4
  manually followed redirects with every hop checked: only `http(s)` on ports 80/443, public
  names only (loopback, private, link-local, CGNAT, multicast, documentation, numeric and
  single-label hosts and local suffixes refused). Images are capped at 6 MB and decoded to at
  most 1,200 px off the main thread.
- **Cache**: 400 previews in memory and `Caches/chat-link-previews.json` (3 days); misses are
  not retried for 6 hours.
- **Switch**: `chat/link-previews/enabled` (default on). Each reader's phone fetches previews,
  so turning it off stops every fetch.

## Views (ForkMesh/Features/Chat)

`ChatScreen`, `RelaySettingsSection`, `RelayNetworksScreen`, `LocalDesktopScreen` (and
`LocalAgentScreen`, `LocalDesktopChip`), `ComposeIdentityBar`, `RelaySwitchScope`,
`ChatNewMessageSheet`, `ChatShareMessageSheet`, `ChatSearchSheet`, `ChatChannelSheet` and its
pages, `ChatMoveChannelSheet`, `ChatLinkPreviewCard`. Conversations are drawn by `Messages/`:
`MessagesConversationView`, `MessagesBubbles.swift`, `MessagesMedia.swift`,
`MessagesDetailsScreen`; calls by `Calls/` (`DirectCallViews.swift`, `DirectCallPreview.swift`,
`WebCallPeer.swift`, `CallFaceLandmarks.swift`, `CallDepthCamera.swift`).

## Tests and verification

Core tests (Linux `swift test`) cover the relay, mesh (loopback sockets), local-control vectors,
history store, direct messages, receipts, view-once media and captions, push decryption, link
parsing, calls and call face depth (`CallFaceDepthTests`). The SwiftUI views and the AVFoundation, Speech, Vision and WebKit paths are
syntax-checked on Linux and compiled only on macOS CI; the Local Network prompt, multicast and
device capture paths need on-device verification.

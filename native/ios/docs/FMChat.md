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
  `version: "native-1.0"` (a wire value); public channel chat uses the room cipher only;
  everything else is sealed with chat protocol v3 (open trust, scope = the room name,
  `acct` = the account or the node id for a guest, `node` = `selfId`); an `@peer` `dm` is sealed
  only to the devices whose bundle `node` is the recipient, plus this account's own devices;
  `persist`/`notify` ride outside the seal. Frames from a device whose bundle has not arrived are
  held and opened when it does.
- `RelayService.groups`: one person row per account. Members key on `owner` (`ownerUser`); a
  member without one (a signed-in web `/chat` tab sends `sender` and `accountKind: "user"` but no
  `ownerUser`) joins the account row whose owner equals its name, so the Online sheet shows
  "jett (you)" once instead of "jett (you)" plus "jett".
- `ChatPersona`: the name the device speaks as: the Display name or account when signed in,
  else `guestChatName(nodeKey)` ("Guest ####"). `stamp(&frame)` adds `accountKind` and
  `ownerUser` to every frame and mesh beacon.
- `ChatNodeOwners` / `GuestLineRenames`: a sender's account claim shows only once
  `/api/chat/node-owner` confirms the node. A "no owner" answer is trusted for `retryAfter` (60 s),
  not the hour a confirmed account is kept, so a device bound just after it spoke is asked again
  soon. A `chat` or `thread-reply` line shown as `Guest ####` while it claimed an account is
  remembered (at most 256 nodes and 256 lines each); `GuestLineRenames` rechecks that node every
  `retryAfter` up to ten times, and when an answer observer (`addAnswerObserver`) reports the claimed
  account, `RelayService` and `OfficeChannelService` rename those lines to the sender name the
  frame carried (and the relay roster row gets the owner), which the chat history save then keeps.
  An answer naming another account leaves them as guests. Bot "via" lines are not renamed.
- `RelayProbe`: `check`/`verify` a relay profile, returning status, message, profile, version
  and missing optional features.
- `RelaySwitchController`: `switchTo(id) async -> RelaySwitchResult` (`ok`, `needsSignIn`,
  `offline`, `error`), `removeRelay`, phase and status text, with suspend/resume hooks.
- `CrossRelayCatalog`: repositories per configured relay.

### Room keys off the main thread (`Relay/RoomSealCache.swift`)

`RoomSeal.loadOrCreate` reads (and on first use writes) the Keychain, and a first install also
generates the identity and an ML-KEM prekey. `RelayService` runs it in `Task.detached`;
`DirectMessageService` and `OfficeChannelService` go through a `RoomSealCache`: one detached load
per room, concurrent first loads share the task, and the result is kept for the session (reloaded
when the account, `node` or DM participants change). All rooms share one `ChatKeyring` per
secure store.

Office rooms: private rooms are sealed (open trust, scope = the room name); public rooms stay
readable by the server. A forkbot line keeps this device's `senderId` and names the bot in a
`bot` field; `ChatFrameText.shownSender` shows such lines as `forkbot`.

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
- Chat protocol v3 links: each side sends a plaintext `fm-link-hello`, then a plaintext
  `fm-link-auth` (the dialer or Bluetooth central is the initiator); only after the peer's
  signature verifies does anything else go out, as AES-GCM counter frames. A bad signature, the
  device's own identity, a blocked key id or 10 s without finishing closes the link. Before
  authentication frames are capped at 4 KiB. The link's peer is the authenticated key id
  (`MeshLink.peerId`); duplicate links are resolved by key id as before by node id.
- Beacons carry only `{t, v:3, id: kid, port, ts, reply, control, node}`; names arrive in the
  sealed `hello` inside the link, which also carries `port` for redials. Peers are keyed by key
  id; `members()` shows the node id from the hello. `controlHosts()` uses the beacon's `node`.
- `#mesh` frames are v3 sealed frames (open trust, scope `forkmesh-lan-mesh-v1`, `acct` the
  account or node id, `node` the node id). The link sends this device's bundle first and then
  every bundle it knows; new bundles are forwarded to other links. Opened `chat` frames are
  flooded to every other link, other frames forwarded only to links whose device is a
  recipient, each once (by signature). `hello`, `bye` and `typing` count only from the link's own
  device. A device learned through another device gets this device's last 32 own messages
  resealed to it, and a linked device gets them on its hello; that is the backfill now that
  other devices' messages cannot be resealed.
- Direct delivery: `directLinks()`, `peerFocus(kid, channel:)`, `sendDirectSealed`,
  `sendDirectAck`, `sendFocus`, and the `onDirectSealed` / `onDirectAck` / `onDirectFocus` /
  `onLinkAuthenticated` / `onLinkClosed` callbacks, which `DirectMessageService.attachMesh` takes
  (`ChatHub.direct` attaches its mesh).

iOS needs `NSLocalNetworkUsageDescription`; multicast also needs the
`com.apple.developer.networking.multicast` entitlement (the subnet sweep works without it).

## Local control (`LocalControl/`)

- Protocol functions byte-identical to `desktop/src/LocalControl.cpp` (vectors in the tests):
  `controlTranscript`, `deriveControlKeys`, `controlPairingCode`, `sealControlFrame`,
  `openControlFrame`. `LocalControlConnection.open(...)` gives `request(method, params)`,
  an `events` stream, `sendEvent`, `close`, and the pairing code.
- `LocalControlService`: status (`idle`, `connecting`, `awaitingApproval`, `connected`,
  `failed`), paired desktops (`localControl/pairedDesktops`), their last LAN addresses
  (`localControl/lanAddresses`, probed first through `LocalMeshService.rememberedBeaconTargets`),
  `lanPending`, the desktop's agents, and actions
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

Chat history files and the `#mesh` history are excluded from device backups
(`ChatFiles.excludeFromBackup`, chat protocol v3 "On the device"); held frames live in the
keychain, whose items are this-device-only.

`ChatHub.attachHistory` restores `relay-<scope>.json` from Application Support once; on every
relay change `scheduleSave` saves again 2 s later. The main thread only takes a copy-on-write
snapshot (`historyParts()`); bounding (200 messages and 2 MB of text per channel), retention
filtering, encoding and the protected atomic write run on a serial utility queue
(`ChatHistoryWriter`), which skips the write when the snapshot is unchanged. `#`, `@` and
`direct:` rooms are written. Direct messages are saved because the relay deletes its copy as
soon as the other person's device takes it; before 2026-10-15 only the replay of undelivered
copies brought a DM back after a relaunch, so a sent message either vanished or came back at
"Sent". `DirectMessageService.onChange` schedules the save; `restore(_:receipts:)` loads the
messages when `hub.direct` is set (it is set after `attachHistory`). The file also keeps
`receipts` (message id to `delivered` or `read`) for the kept messages, so a mark does not drop
back to "Sent" after a relaunch. Signing out clears the service, and the next save drops the DMs.
`receiptTimes` (message id to `{"delivered", "read"}` epoch milliseconds) is saved beside it so
the Message Info page still shows when a message arrived and was read after a relaunch.

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
  messages. Bubbles in one run join on the sender's side: `MessagesBubbleShape` takes
  `joinsAbove` and `joinsBelow` and drops those corners to 5 pt, photos use the same corners,
  rows in a run are 2 pt apart, and the last bubble keeps full corners (it also carries the
  sender's avatar in a group). Bubbles have no tails: the user asked for plain rounded bubbles.
- **Bubbles** (`MessagesBubbles.swift`): on iOS 26 each bubble is Liquid Glass tinted with the
  bubble colour (`messagesBubbleGlass`, blue for own messages, grey for others); earlier
  systems draw `MessagesBubbleSheen`, a material and gradient with a top highlight and a hairline
  rim. Every bubble ends in `MessagesBubbleMeta`: the send time in 12-hour form ("3:42 PM"), a
  11 pt clock (`MessagesExpiryRing`): a circle with a hand that sweeps clockwise from 12 and a
  faint wedge (`MessagesClockWedge`) for the time left, shrinking as the message nears the end of its retention
  (`ChatHub.expiresAt`, the conversation's retention from `ChatHub.lifetime`; VoiceOver reads
  "Disappears in 5 hours"), and the receipt mark. A small dot before the time marks an own
  direct message the relay still stores: the receipt is Sent, not yet Delivered, and the room's
  server-copies count is not 0 (`ChatHub.isOnServer`). The time sits on the text's last line
  when it fits and drops under it otherwise.
- **Message Info** (`MessagesMessageInfo.swift`): swiping a bubble left drags the Message Info
  page in from the right edge, following the finger, with the transcript easing 30% of the way
  left like a navigation push; letting go past 30% of the width (or flinging) opens it, anything
  less springs back. Back, or a swipe right on the page, closes it; VoiceOver offers it as the
  "Message Info" action. It shows the message, who sent it, when it was sent, and for own direct
  messages when it was delivered and read (with seconds), plus recipients, edits, the
  disappearing time and the server-copy state when they apply. The pan is a
  `UIPanGestureRecognizer` that only begins for a mostly-horizontal leftward move, so vertical
  scrolling and the long-press menu are untouched; iOS 17 falls back to a simultaneous
  `DragGesture`.
- **Receipt status line** (`MessagesReceiptLine`): under your most recent sent message in a
  direct conversation, as in iMessage, a small right-aligned line says "Delivered 11:04 PM" or
  "Read 11:06 PM" (an earlier day reads "Yesterday 3:20 PM" or the weekday), from
  `ChatHub.receiptTimes(for:)`; with no recorded time it says just "Delivered" or "Read". It
  hides while the message is Sending or Sent. Tapping it opens Message Info for that message.
- **Header** (`MessagesConversationView.header`): back circle, then one glass pill with the
  avatar and, to its right, the name over "Online · N messages on the server", then the call
  buttons. It has no new agent button or own avatar; those stay in the channel list's top bar.
- **Presence** (`ChatHub.peerPresence`, also used for the online dot of `directRows`): a server
  DM (`direct:<id>`) finds the person's roster group by the nodes of the devices the DM trusts,
  then by the account (`otherUser`, case-insensitive); a relay DM (`@<node>`) by that node's
  group. Any online node of theirs makes them Online (Away when their status says so). Typing, a
  tap, shared phone direction or a mesh focus report in that DM also counts as Online. Until
  2026-10-17 only relay DMs looked anyone up, so every server DM said "Offline" even with the
  person chatting, and the list dot compared the owner to `otherUser` case-sensitively.
- **Shared look** (`Threads/ChatLook.swift`): background, group name and group photo travel as
  sealed `look` frames, last-writer-wins on `(ts, id)`; each change is shown as an event.
  Pictures are capped at 600 KB because a frame is base64-encoded three times and the room
  WebSocket refuses messages over 2 MiB.
- **Share or override the background** (`MessagesBackgroundPicker`): a segmented choice,
  "Share" or "Just for me". Sharing sends the `look`
  frame as before and drops any own background, so the sender sees what they shared. "Just for
  me" sends nothing: `ChatHub.setOwnBackground` stores a preset, a picture or "None" in the
  conversation's `ChatLook` (`ownBackground`, saved in the history file as `ob`, `obp`, `obi`),
  and `conversationLook` returns `ChatLook.shown`, which puts it over the shared one. Shared
  changes keep arriving underneath (`conversationSharedLook`) and show again after "Use Shared
  Background" in Details. Restoring the history file keeps an own background even when a shared
  look arrived first.
- **Media**: `ChatHub.sendMedia` caps media at 600 KB; photos are re-encoded as JPEG (longest
  side 1,600), voice messages recorded as 32 kbit/s mono AAC, up to two minutes. Pasted images
  (`ChatTextField`, a UIKit text view so Paste offers images) go through the same path.
- **Group management** (`MessagesDetailsScreen`): name, photo, people, background, photos,
  retention and encryption summary; leave a relay private channel; delete history locally.
- **Live touch, direction and drawing** in direct messages: `DirectLiveEngine` batches taps,
  strokes and motion samples into sealed `live` frames at the relay-announced pace (wire format
  in `docs/app/chat-private-rooms.md`). The phone samples motion at 100 Hz
  (`DirectLiveMotionFeed.rate`, 60 Hz until 2026-10-18; a frame still holds under the
  40-sample cap at the relay's pace) and, before the relay's `forkmesh-live` budget arrives or over
  the mesh alone, paces at 120 ms (`DirectLiveFrames.fallbackIntervalMs`, under the relay's 150
  frames per 10 s). The partner's phone icon redraws at 60 fps while they share. Until
  2026-10-17 the fallback was 2.5 s and the feed 20 Hz, so the icon could move in jumps seconds
  behind the real phone. While a direct conversation is visible, its motion feed
  shares phone direction automatically and an impact sends a centred taptic tap. In a direct
  conversation the header capsule shows the peer's phone (`MessagesLivePartnerPhone`) where the
  round avatar used to be, 76 pt tall (twice the old avatar), hanging out of the capsule. It is
  a 3D phone with their avatar on its screen, drawn from the full gravity pose
  (`DirectLivePhonePose` in FMChat): the outline is the projected silhouette of a phone with
  real thickness, and the screen is mapped onto its front with an affine transform, so a phone
  lying flat on a table is seen edge-on as a thin pink line and a tilt is never clamped. Every
  bump moves it: the user acceleration is turned into screen space and offsets and scales the
  phone, and the strongest kick of the last 260 ms (`kickHoldMs`) is held and faded, so a
  one-sample bounce on a table still shows for several frames. The pink rim brightens with the
  kick; the phone is grey and upright while they are not sharing. The phone has no tap, long
  press or menu, and it swallows touches so the capsule's glass does not bounce under it (the
  Floating 3D Face menu and its idle bob were removed on 2026-10-18). When the peer is online
  the line under the name drops the word "Online" for a blinking green light
  (`MessagesOnlineLight`, steady with Reduce Motion; `ChatHub.conversationStatusLine` and
  `conversationPeerOnline`); VoiceOver still reads "Online". A short tap anywhere on a direct conversation's transcript sends
  a tap at that spot (`transcriptTapped`); with the keyboard up the first tap only closes it. There
  is no tap button any more, so VoiceOver gets a "Send a tap" action on the phone icon. The pencil
  in the header, beside the phone icon, opens the coloured drawing canvas, where a short tap also sends a tap and a drag
  draws; its toolbar sits at the top under the header, and the ink colour is a drop-down (the
  current swatch and a chevron open a 4 by 2 grid of colours, which closes on a pick or a touch on
  the canvas). Sending and receiving a tap always vibrate (`DirectLiveHaptics.sent` and
  `tapped`), even with app haptics turned off in Settings, because the tap is the message.
- **No pings for private conversations**: `ChatHub.filePing` skips every conversation
  `isPrivateConversation` names (relay DMs, server DMs, private office rooms, relay private
  channels) and drops that conversation's stored chat pings. A ping stores the decrypted text in
  the local ping log (UserDefaults, shared by every account and project on the phone, in device
  backups), where it outlived the message's retention, deletion and sign-out. Setting
  `ChatHub.pings` and restoring the chat history purge any private chat pings filed by earlier
  builds (`LocalPingLog.removeChats`). Public channels still ping; anyone on the relay can read
  them anyway.

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
- **Channel list top** (`ChatChannelSidebar.recentSection`): "Recent" lists the five newest
  conversations of any kind (`ChatHub.recentConversations(limit:)`, by the last message or the
  DM's `updatedAt`) so one tap goes back to a chat. Its header carries a small "N online" pill.
  The Channels, Unread and Online count tiles were removed on 2026-10-15.
- **People**: the "N online" pill opens everyone online; a name opens the DM with that account, an
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
- **Ratchet (chat protocol v4).** DM frames are v4 (`docs/app/chat-protocol-v4.md`): a frame
  has no entry for its sender, and a frame with no recipients is still sent (the outbox reseals
  it once a device is approved). Own frames coming back from the relay (`own`) are ignored: no
  ack, no advertisement, no outbox rebuild; this device's history already holds them (one that
  comes back with a ref this device tracked still marks the message Sent). v3 frames in a DM room
  are ignored. A frame with no entry for this device is `notForMe` (R2) without being opened.
- **Lost sessions.** `noSession(kid)` (from the relay, a held frame reopened on approval, or a
  `direct-sealed` copy), at most once per DM and device every 60 s
  (`DirectMessageFrames.resetIntervalMs`), unless this device already opened a later frame from
  that device (the frame's `ctr` is below `RoomSeal.highestCounter(from:)`, the seen window's
  maximum: the relay replays an unacknowledged frame on every reconnect, and recovering again
  would reset the session that works): drop the room's sessions with that device, send a
  persisted, non-notifying frame `{"type":"e2ee-reset","senderId":<selfId>}` sealed only to it
  (over the mesh when no socket is open), and one persisted key advertisement. The frame that
  failed is not acknowledged. An opened `e2ee-reset` is not shown: every outbox entry (7 days) is
  sealed again for that device alone and sent with the same id, `persist`, its `notify` and
  `resend: true`, with no `forkmesh-superseded` (the original still serves the other devices).
- **R3.** Each durable send is kept in a per-DM outbox (64 entries, 7 days) with its relay ref
  (first 40 hex of SHA-256 of the exact text sent) and the key ids it was wrapped for. When a
  new approved device appears (first device of an account, or one the user approved), entries
  not wrapped for every approved device other than this one are resealed and resent
  (`resend: true`), and the old refs are sent in `forkmesh-superseded` frames. Pending devices
  get nothing.
- **Trust (v3 trust, v4 frames, strict).** Scope `direct:<id>`, `acct` = the signed-in account (lowercased,
  `displayName` unless an `account` closure is given), `node` = `selfId`, participants = this
  account and `otherUser`. A bundle for anyone else is dropped; a second device of an account
  is pending and its frames are held until `approveDevice`, which reopens them through the
  ratchet. History-replay frames get no exception: a frame this device already opened is a
  `replay` (acknowledged, never shown again), since its key is gone. A frame not addressed to
  this device still asks for one durable advertisement.
- **Direct over the mesh.** Every DM frame type except calls goes to each linked approved device
  as `direct-sealed` (a v4 frame sealed to that device only, so each copy advances its session); files up to 1 MiB over Wi-Fi and
  256 KiB over Bluetooth. When this device has focus (foreground and `setFocus(conversation,
  true)`) and a linked approved device of the other account reported focus, the frame goes over
  the mesh only and waits 3 s for `direct-ack`; no ack or a dropped link sends the relay copy with
  the same id. Otherwise it goes over the mesh and the relay. A `direct-ack` also marks an own
  message delivered. Frames from a device the conversation has no record of are dropped.
- **Dedupe.** A resend keeps the message id, so receivers drop the second copy.
- **Unconfirmed sends.** A DM the server has not confirmed within a few seconds is sent again
  over a fresh connection, so it does not stay at "Sending".
- **Delivered entries leave the outbox.** `forkmesh-delivered` removes those messages from the
  outbox and `reseal` skips anything `delivered` or `read`. Before, a new device key (the
  sender's own laptop, say) resent messages the other person already had, and each resend was a
  new server copy that stayed until their phone opened the room again.
- **Acks are retried.** `flushReceived` keeps its refs when the socket is gone and sends them
  right after the next connect, instead of dropping them.
- **Server copies label** is cleared when the socket drops or closes, so a stale count is never
  shown; the relay sends a fresh count on join.

### Receipts, deletes, reactions and replies

- **Receipt marks** (`ChatReceipt`: `sending`, `sent`, `delivered`, `read`; only moves
  forward). DMs: `forkmesh-recipients` makes a ref `sent`, `forkmesh-delivered` `delivered`,
  `forkmesh-read` `read`. `forkmesh-read` marks every own message in the DM's history read
  (except ones still sending), not only refs seen since launch. Delivered and read marks are
  saved with the history file. Office rooms reach `sent` only. Drawn by `ChatReceiptMark` inside
  the sender's own bubble.
- **Receipt times** (`ChatReceiptTimes`, `DirectMessageService.receiptTime(_:)`,
  `ChatHub.receiptTimes(for:)`): the first time this device saw a message reach `delivered` and
  `read`, stamped from the service clock when the relay frame arrives. The protocol carries no
  times, so these are when the phone learned of the receipt; a receipt that arrives while the
  phone is offline is stamped when it reconnects. Restoring from disk never stamps a time, so a
  mark saved before times were kept shows "Delivered"/"Read" without one.
- **Quoted replies** (DMs): Reply in a message's menu sets `ChatHub.replyingTo`; the composer
  shows the quote with a cancel button, and the next text sent carries
  `"replyTo": {"id", "senderId", "sender", "text"}` (text up to 280 characters,
  `ChatReplyQuote`) in the sealed `chat` frame. The bubble draws the quote above the text with a
  bar on the left (`MessagesReplyQuote`, "You" for own messages), and tapping it scrolls to the
  original when it is loaded. Switching conversation cancels a pending reply. Other clients
  ignore the field and show the text alone. `thread-reply` frames from other clients still show
  as "N replies" under the root.
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
- `PushChatOpener.open(pending, request:, secureStore:, keys:) -> PushChatOpened`
  (`message`, `unknownSender`, `unavailable`) checks the pending answer names the same
  conversation, decrypts the `RoomCrypto` envelope and opens the frame with the shared
  identity, prekeys, trust records and ratchet state (strict and v4 for DMs, open and v3 for
  channels) through an `OverlaySecureStore`, so nothing is written back. DM frames open with
  `commit: false`: the extension reads the ratchet state in the shared keychain group but never
  advances it or the seen window, so the app opens the same frame again when it connects. Only `chat` and `thread-reply` become
  messages, and DMs must be sealed. A frame from a pending or unknown device is
  `unknownSender`, shown as "New message".
- Pushes carry no sender: both "Name and message" and "Name only" open the frame to learn it
  (`PushPreviewText.preview(opened, request:, mode:)`), and the portrait is fetched for the
  decrypted sender.
- `PushRoomKeys` caches derived room keys (one keychain item, 32 entries) so only the first push
  of a room pays the 210,000 PBKDF2 rounds. Each entry also carries a hash of its room name
  (`roomId`), so a DM push that carries its frame opens on the phone with no request at all
  (`PushChatOpener.openPushed`; the room is `chat-direct:<id>:v<keyVersion>`, so a new key
  version misses and asks the relay). The pending fetch runs only when that fails, and is retried
  once after 1 s on a transport error, 429 or 5xx.
- Photos, voice messages, videos and view-once messages with no text read "📷 Photo",
  "🎤 Voice message", "🎬 Video" and "🎬 Video message" (`PushChatMedia`); before, a view-once
  message had neither text nor a file name and stayed "New message".
- `PushPreviewMode` ("Notification content" setting): name and message (default), name only, or
  neither.

The extension reads the chat keys through `PushSecureReader` (`Shared/PushCredentialStore.swift`):
the project prefix comes from the `deviceKeyName` in the shared push credential
(`RoomSeal.privateKeyStoreKey` is now the v3 identity key; a v2 name still yields the prefix). It
links only the `ForkMeshPushCore` product.

**Tapping a chat push.** `ApplePushBridge.openFields` copies `conversation` and `channelId` into
`PushOpen`. Office channel pushes now carry only `channelId` (the server stopped naming the
room), so `HomeShellModel.routePush(..., channelId:)` asks the chat bridge
(`ShellChatBridge.officeConversation(channelId:)`, `ChatHub` answers from
`OfficeChannelService.rooms`) for the room and `pushChatNamed` passes it on as if the push had
named it. A named conversation still wins, DMs ignore `channelId`, and an unknown id falls back
to the first unread room as before. `ChatHub.switchConversation` and `revealRequests` are
unchanged.

## Voice and video calls

`DirectCallEngine` (`Direct/DirectCalls.swift`) is the signaling state machine for `direct:`
rooms; signaling rides the DM room's sealed socket (`direct.sendCallFrame`, `onCallFrame`).
Missed calls post the same "Missed audio/video call" line as the web: when the ring times
out or when the caller hangs up before an answer (`hangUp` while `.ringing`). Android does the same.

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
  call. `app/tools/sync_native_call_faces.py` generates
  the page for both apps;
  `app/tests/test_chat_call_faces.py` fails when they drift.
- **Faces, voices and backgrounds** live in `FaceChangerLook` (`FaceChanger/FaceChangerLook.swift`),
  which the Face Changer plug-in owns and saves under `chat.callFace`, `chat.callVoice` and
  `chat.callBackground` (custom images in Application Support with complete file protection).
  The engine takes that look in its initializer, reads it, and listens to it, so a change made in
  the plug-in reaches a live video peer; `detach()` stops listening when the engine is replaced.
  The page host, Vision and TrueDepth code are in the app's `Features/FaceChanger/`. Face landmarks and person masks come from Apple Vision
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
  60 s, then sends it as a view-once video. `finishRecording()` keeps `audio/*` mimes (an
  audio-only preview records the microphone through the changed voice) and
  `WebCallRecording.fileExtension` names the file `mp4`, `m4a` or `webm`.
- **Pre-call check** (`Calls/DirectCallPreview.swift`). Video call and Voice call in the
  conversation header no longer ring at once. `DirectCallActions.open` asks for camera and
  microphone access, refuses while a call is active, an invite is pending or the factory is busy,
  and sets `DirectCallPrecheck.shared.request`. `DirectCallPreviewScreen` ("Call @name") then runs
  `WebCallPeerFactory.preview(media:)`: video previews the camera with the look's face, background
  and voice and offers the Face, Background, Voice and Hear myself controls; voice runs an
  audio-only preview (`media: .audio`, page class `audio preview`) with the avatar, Voice and Hear
  myself. The page's WebView is on screen from the first frame (opacity 0.01 for voice) because
  WebKit does not start `getUserMedia` for an off-screen WebView; a 30 s watchdog ends a stuck
  preview and Try again reopens it. "Record test clip" records up to 10 s with a countdown, stops
  itself, and plays the clip back at once (`MessagesViewOnceReviewVideo` for video,
  `CallClipAudioPlayer` with play, pause and replay for audio) with Re-record and Discard. The
  player never changes the audio session category (WebKit's capture owns `.playAndRecord`); without
  headphones it overrides the output to the speaker and undoes that override when the clip is
  discarded or the call starts. Temporary clip files are deleted on discard, re-record, call and
  close. Call turns Hear myself off and calls `DirectCallActions.start` (access check, then
  `engine.start`). The preview is handed to the call: `makePeer` adopts a previewing peer for every
  video call (as before) and for a voice call when the preview is audio (`previewMedia`), running
  `setup` on the page, which keeps the open stream. Closing the screen ends the preview only while
  no call is active; if the call ends before `makePeer` adopted it, `DirectCallLayer` ends that
  preview. A Face Changer look change reaches an audio preview as a voice change only.
- **Call layer at the root.** `.directCallsRoot()` is attached once, on `HomeShell` in
  `App/ForkMeshApp.swift`, so calls show over every screen, not only Chat. One `fullScreenCover`
  shows, in order, the call (`engine.active` and not minimized), the incoming call
  (`engine.invite`) or the pre-call check, so the WebView moves between screens without a second
  presentation. Its binding no longer hangs up when the cover goes away (before 2026-10-23 the
  setter called `hangUp()`): a dismissed active call is minimized instead. `WebCallSurface`
  hosts the WebView in a holder view and only takes it back when it has no superview, so two
  surfaces never fight over it.
- **Incoming call.** `DirectCallIncomingScreen` replaces the old top banner: caller, "Video call"
  or "Voice call", and large red Decline and green Accept buttons.
- **Ringing** (`Calls/CallRinger.swift`). `CallRinger.shared` is attached to the engine in
  `installCalls` and watches `phase` and `invite` with `withObservationTracking`, so it works with
  no view on screen. While the caller's phase is `ringing` it loops a ringback (440 + 480 Hz, 2 s
  on, 4 s off); while an invite is pending it loops a two-burst ring (400 + 450 Hz, 0.4 s on, 0.2 s
  off, 0.4 s on, 2 s off) and vibrates every 2 s. Accept silences it at once (`hush()`), so the
  ring does not play over the permission prompt; connecting, live, decline, missed, hang-up or a
  cleared invite stop it. Both tones are synthesized by `DirectCallTones` (FMChat) and written as
  WAV with `wavData`. The ringer leaves the session alone when it is already `.playAndRecord`
  (a preview or call is capturing) and otherwise uses `.playback` with `.mixWithOthers`, like
  `GenieSounds`. While the app is not in the foreground, a pending invite also posts a local
  notification (`direct-call-<call id>`, title "Incoming video call" or "Incoming voice call",
  body "@name is calling you", the ring sound below, `interruptionLevel = .timeSensitive`) with
  `topic: direct-messages` and the conversation, so a tap opens that DM where the full-screen
  incoming view waits. It is removed when the invite ends or the app returns to the foreground.
  There are no Accept and Decline notification actions: the categories come from the shared
  `NotificationCategoryId` list in FMApi, and a call category there needs matching server and
  Android work.
- **Call pushes** (since 2026-10-21; protocol in `docs/app/chat-direct-calls.md`).
  `sendCallFrame` opens the DM's socket when it is not open and marks every `call-invite`
  envelope `ring: true`; the relay then sends the callee a `calls` account event and an APNs
  alert. The callee's app reacts in four places, each calling
  `DirectMessageService.openRinging(conversation)` (it reloads the conversation list when the
  conversation is not known yet, then `open`s it), so the next resent invite (every 3 s)
  arrives and `DirectCallIncomingScreen` shows:
  - `AccountEventChannel.onEvent` (new: topic and whole frame, before `onTopic`) with topic
    `DirectCallEngine.ringTopic` (`calls`), wired in `AppServices+Chat.swift`;
  - `ApplePushBridge` `willPresent` for a `calls` push in the foreground, which shows the banner
    with sound unless the engine is already in a call or showing that invite;
  - a tap on the push (`receivedTap`); `PushOpen.isCall` counts as a direct message, so the tap
    also opens that DM like a DM push;
  - `CallRinger.sweepPushes`: whenever the ringer refreshes it removes delivered `calls` pushes
    for the ringing conversation, for one whose invite just ended, and all of them while the app
    is in front; a push under 45 s old found in the foreground with no invite yet also opens its
    DM, so opening the app from its icon during a ring still shows the call.

  `CallRinger.attach` writes `DirectCallEngine.ringSound` (`forkmesh-call-ring.wav`, nine
  three-second ring cycles from `DirectCallTones.ringLoop`, 27 s, under Apple's 30 s cap) to
  `Library/Sounds` with `completeUntilFirstUserAuthentication` protection, so a push on a locked
  phone can read it, and the local call notification uses it too. The notification extension
  delivers a `calls` push as it came (the relay also leaves `mutable-content` off), so it is never
  rewritten into an inbox row.
- **CallKit and PushKit** (2026-10-21). Incoming calls show on the system call screen.
  `DirectCallKit` (`Calls/DirectCallKit.swift`) owns one `CXProvider` (video supported, one call,
  generic handles "@name", kept out of the phone's Recents because those sync through iCloud) and
  a `PKPushRegistry` for `.voIP`. `AppDelegate` starts the registry in `didFinishLaunching`, so a
  VoIP push that launches the app is delivered; `installCalls` attaches the engine and the DM
  service. The decisions live in `DirectCallKitLedger` (FMChat, `Direct/DirectCallKitLedger.swift`,
  Linux tests in `DirectCallKitLedgerTests`); the app class only turns its effects into CallKit
  and engine calls.
  - **Token.** The PushKit token goes to `PushNotificationService.didReceiveVoipToken`, which
    re-registers the device with `voipToken` beside the alert token.
  - **VoIP push.** Every VoIP push is reported to CallKit before its completion runs, because iOS
    ends an app that does not and then stops delivering VoIP pushes. A malformed one is reported
    and ended at once. The CallKit UUID is the invite's call id. The push then opens the DM
    (`openRinging`, held until the services are attached; on a cold launch `DirectCallKit.wake`
    starts `AppBoot.shared`) so the resent invite arrives and fills in the caller and
    voice/video (`reportCall(updated:)`). With no invite within 20 s
    (`DirectCallKitLedger.inviteWait`) the call ends as unanswered. A second push for a call
    still ringing reports the same UUID again, and one for a call already over is reported and
    ended at once.
  - **Socket invites** that arrive with no push (app open, or Android or web callers on an old
    relay) are reported to CallKit too.
  - **Answer** (CallKit `CXAnswerCallAction`) accepts the invite through the same access check as
    the in-app Accept, or waits for the invite if it has not arrived yet. **Decline or end**
    (`CXEndCallAction`) declines the invite, hangs up a live call, or declines the invite once it
    arrives. **Mute** maps to `toggleMute`. Accepting in the app asks CallKit to mark the call
    answered (`CXAnswerCallAction` transaction); a call the caller ends, or that ends in the app,
    is reported ended (`remoteEnded`).
  - **Ringing.** While CallKit has the call, `CallRinger` neither plays its own ring nor posts the
    local call notification (`DirectCallKit.ringing`). If CallKit refuses a report for any reason
    other than Focus or the block list (for example `unentitled`), the ledger drops the call and
    the in-app ring, notification and incoming screen work as before. A call Focus filters stays
    silent, as the user asked.
  - **Region check.** CallKit must not be used in apps on the mainland China App Store. Both are
    off when `DirectCallKitRegion.allowed` says no: the App Store storefront is `CHN`, or the
    device region is `CN`. `DirectCallKit.start` decides at launch from the last storefront it
    saw (`callkit.storefront` in `UserDefaults`, so a VoIP push on a cold launch never waits on
    StoreKit) and the region, then reads `Storefront.current` and follows `Storefront.updates`.
    While it is off, no `PKPushRegistry` exists (so no VoIP push can arrive that would have to be
    reported), the VoIP token is withdrawn from the relay (re-registration without `voipToken`,
    so the phone gets the alert push), and the `CXProvider` is never created. Calls then ring
    through `CallRinger`, its local notification and the in-app incoming screen. Turning it off
    while calls are on the CallKit screen ends them there (`DirectCallKitLedger.endAll`) but not
    in the app; a VoIP push that lands during the switch is reported and ended at once, as iOS
    requires.
  - **Our call counts as a call.** `OtherAppCall` (`docs/native/OTHER_APP_CALLS.md`) sees our CallKit
    call too, so interface sounds and chimes stay quiet during it. Since 2026-10-23 every call of our
    own counts, CallKit or not (see "Audio session during a call" below).
- **Native call engine** (2026-10-21, `Calls/Native/`, compiled only when `canImport(WebRTC)`,
  i.e. when `native/ios/WebRTCPackage/WebRTC.xcframework` exists and `tools/generate_project.rb`
  links it; without it the WebView path above is unchanged). The framework is our own build of
  libwebrtc from `native/webrtc/` (`docs/server/rtc.md`, "Phone media engine").
  - `NativeCallMedia` owns the one `RTCPeerConnectionFactory`, built by our patch's
    `RTCFmFactory.factory(withCaptureProcessor:)` so microphone audio passes through
    `NativeVoicePipeline` (the ported `CallVoiceProcessor`, skipped when the voice is off) after
    echo cancellation. `RTCAudioSession` runs in manual-audio mode: CallKit's
    `provider(_:didActivate:)` hands its session to WebRTC (`callKitActivated`), and calls CallKit
    does not own (outgoing, or the China region) activate it themselves (`beginAudio`/`endAudio`).
    That is what gives a call answered on the lock screen its audio at once.
  - `NativeCameraPipeline` wraps `RTCCameraVideoCapturer` (front camera, nearest format) and runs
    every frame through a `NativeFrameFilter`. While a face is chosen on a phone with a TrueDepth
    front camera it switches the camera to `NativeFaceTrackingSource` (`ARFaceTrackingConfiguration`,
    `ARFrame.capturedImage` fed to the same `RTCVideoSource`, with the face mesh, blend shapes and
    depth passed to the filter) and back when the face is cleared, the session fails or no frame
    arrives within 1.5 s; while a look is active it also caps the frame rate and encode size by
    thermal state (`CallFaceQuality.capture`, `CallFaceFrameGate`); `NativeLookFilter` draws the chosen face, then
    replaces or blurs the background with `VNGeneratePersonSegmentationRequest` and Core Image
    (pictures in `Resources/CallBackgrounds`, the custom photo from the look). Both follow
    `FaceChangerLook` through `NativeCallMedia.follow`.
  - `NativeCallPeer` is the one-to-one `DirectCallPeer`: two `sendrecv` transceivers (audio,
    video) like `/chat`, the same `{type, sdp}` and candidate JSON, `CallMediaTuning.tune` on every
    local description (the web page's Opus parameters, checked against the JS output), sender caps,
    the 4 s route check. Since 2026-10-23 it no longer closes itself on `failed` or after 8 s
    `disconnected`: it reports `onConnection` and the engine restarts ICE (`connection.restartIce()`
    and a fresh offer in `restartOffer()`); see "Resilience" below. The camera and microphone go on with
    `addTrack` (`connection.add(_:streamIds:)`), not `addTransceiver(with:)` (fixed 2026-10-21):
    when the iPhone answers, WebRTC matches the caller's offer only to transceivers made by
    `addTrack` (JSEP); ones made by `addTransceiver` stay unused, the answer comes back
    `recvonly`, and the person who placed the call got no picture or sound from the iPhone while
    the iPhone still showed theirs. A voice call keeps a trackless video transceiver so its offer
    has the same two m-lines as `/chat`. `NativeCallPeerFactory` is the engine's
    `peerFactory`; `DirectCallScreen` draws `NativeCallSurface` (remote video plus a mirrored self
    view) and `NativePreCallScreen` replaces the WebView pre-call check.
  - **Call screen layout** (2026-10-21). The self view is `DraggableCallPreview`: drag it
    anywhere and it springs to the nearest of four corners (clear of the name at the top and
    the buttons at the bottom), following the drag's predicted end so a flick works; the corner
    is kept in `@AppStorage("fm.call.previewCorner")`. The buttons (face, background, voice, mute,
    camera, hang up) use `ViewThatFits`: one row with 16 pt gaps, then 8 pt gaps, then two rows,
    the look pickers above and mute, camera and hang up below. Before, six 56-64 pt buttons in
    one row needed about 424 pt and ran off the edges of every iPhone narrower than a Pro Max.
  - `NativeGroupCall` joins group calls in private channels (`docs/app/chat-group-calls.md`): it
    asks `OfficeChannelService.callTicket` for a ticket, speaks the call-server protocol over a
    `URLSessionWebSocketTask`, attaches one `RTCFmFrameEncryptor` from our patch to both its senders
    (one frame counter for audio and video, kept for the whole call) and an
    `RTCFmFrameDecryptor` to every forwarded receiver (keyed by the stream's peer id), sends
    presence and keys with `sendLiveFrame` (sealed, never stored), draws a new key on every
    `joined`, rotates its key when someone leaves and rejoins on another core when its server goes
    away. `ChatHub.onGroupCallFrame`
    feeds it the channel's group-call frames; the roster and key rules are `GroupCallRoster`
    (`FMChat/Group/GroupCalls.swift`). The channel header shows `GroupCallHeaderButtons` (Join · N
    while a call runs) and `.groupCallsRoot()` presents `GroupCallPanel`.
  - Faces are drawn natively: `NativeLookFilter` runs `NativeFaceRenderer` (Metal, Vision
    landmarks or the ARKit face mesh, the shared `FMChat/CallFace` maths, `CallFaceStage` for the
    frame loop) before the background, and hands the background step a keep mask so the
    stand-in's hair is not cut away; a blur background is drawn inside the face pass from the
    reconstructed room. `NativeFaceSegment` runs the browser's hair network
    (`Resources/CallSegmentation/hair-192.fmnn`, `CallSegmentModel`/`CallSegmentPlan` in
    `FMChat/CallFace`) as Metal compute kernels and the browser's guided-filter person matte on
    Vision's mask. Stand-in photos are in `Resources/CallFaces`; the custom face is built from the
    look's photo. Passes, threading, fallbacks, cost and the differences from the browser are in
    `docs/app/chat-direct-calls.md`, "Native face renderer (iOS)".
- **Route note.** The page posts `{event: "route", route, local, remote, protocol}` when the call
  connects and when the selected candidate pair changes. `WebCallPeer` passes it to
  `WebCallPeerFactory.route` (a `DirectCallRoute`, reset for each new call and when the peer
  closes), and `DirectCallScreen` shows, under the state line once live, "Direct · peer-to-peer,
  no server in between" or "Relayed · through a TURN relay server (third party)". The live state
  line is "Connected · end-to-end encrypted", since the route is unknown until then. The pre-call
  screen shows the hint "Calls go directly between your devices when possible."
- **Live quality widget** (2026-10-22). Mirrors `app/public/call-quality-meter.js` (thresholds,
  strings and chart shapes are the same on web, Android and iOS). `CallQualityWidget`
  (`Calls/CallQualityViews.swift`) is a small dark-material pill under the call's name and state
  line: four signal bars coloured by grade (success green, warning amber, danger red), a mirrored
  waveform of the last 20 voice levels (your mic up in accent blue, the other side down in grey)
  and the round-trip time ("48 ms"). VoiceOver reads "Call quality: Good connection · 48 ms ·
  0.0% loss · 3 ms jitter". A tap opens a card (kept open across calls with
  `@AppStorage("fm.call.qualityOpen")`) with the grade and 120 × 22 pt Canvas charts: Voice (50
  levels, 5 s), Network (RTT line over 60 samples, 1 min, with amber loss ticks from 1 % and red
  from 8 %) and, in video calls, Video in and Video out (kbps line; resolution, fps, rate and
  "limited by bandwidth/CPU"). Before a call is connected it shows only the mic waveform.
  - **Pure logic** is in FMChat `Direct/CallQuality.swift`, tested in `CallQualityTests`:
    `CallQuality` (bars = 4 − the worst of RTT ≥ 150/300/500 ms, loss ≥ 1/3/8 %, jitter ≥
    30/60/100 ms, at least 1; 0 = "Measuring connection" when RTT and loss are both unknown;
    levels are `clamp((20·log10(rms) + 60) / 60, 0, 1)`), `CallStatsSummary` (the web's
    `summarizeRtcStats`: selected candidate pair RTT, inbound loss and byte deltas, jitter, largest
    incoming video, outbound `qualityLimitationReason`, `remote-inbound-rtp` `fractionLost`),
    `CallQualityEvent` (the page's `quality` event) and `CallQualityHistory` (50 levels, 60 samples).
  - **WebView engine.** The page posts `{event: "quality", live, rttMs, lossPct, …, mic, peer}`
    about once a second (`live: false` in the pre-call preview and the Face Changer studio).
    `WebCallPeer` passes it to `WebCallPeerFactory.quality` (a `CallQualityMonitor`, reset per
    call and preview), which plays the ~10 levels of each event out at 10 Hz so the waveform moves
    smoothly, about a second behind.
  - **Native engine.** `NativeQualityProbe` (`Calls/Native/NativeCallQuality.swift`) reads
    `RTCPeerConnection.statistics` every 200 ms once live (100 ms before 2026-10-23: each read is a
    full libwebrtc stats collection hopping across its threads, so ten a second cost more than the
    waveform is worth). Each read queues two levels, the midpoint and the new one, which
    `CallQualityMonitor.queue(levels:)` plays out at 10 Hz like the WebView's, so the waveform still
    moves every 100 ms. While the call is minimized or the app is not in front
    (`NativeQualityProbe.levelsShown`) it skips the level reads and only takes the once-a-second
    sample, which the media watchdog needs. Levels come from `audioLevel` (`media-source`
    for the mic, forced to 0 while muted; `inbound-rtp` audio for the other side) and, every tenth
    read, a full sample through `CallStatsSummary`. One-to-one calls feed
    `NativeCallPeerFactory.quality`; WebRTC group calls feed `NativeGroupCall.quality` (one
    connection to the call server, so the worst RTT/loss and the largest incoming video across
    everyone). FMM group calls take levels from `FmmAudioEngine` (peak per 100 ms of the sent frame
    and of the mixed playout) and once a second `FmmmobileCall.stats()` (RTT, loss, target bitrate
    as the send rate; no receive rate). The native pre-call screen meters the mic with an
    `AVAudioRecorder` writing to `/dev/null` (`NativeMicMeter`), stopped before the call starts.
  - **Where.** `DirectCallScreen`, `DirectCallPreviewScreen`, `NativePreCallScreen`,
    `GroupCallPanel` ("Others ▼" instead of "Them ▼") and `FaceChangerScreen`.
- **Resilience** (2026-10-23). Real calls showed a callee who could not pick up, a caller looking at
  a black screen, and calls that went silent mid-way until someone hung up. `DirectCallEngine` now
  follows the same rules as web `/chat` (`docs/app/chat-direct-calls.md`, "Resilience"), with no
  new frame types, so older clients still work. All timers use the injected `sleep`; the Linux tests
  are `DirectCallResilienceTests`.
  - The callee resends `call-accept` every 2 s (`acceptResend`, at most `acceptResends` = 8) until
    the caller's offer arrives. The caller answers a repeated accept from the device it picked with
    its outstanding offer (same SDP), and resends an unanswered offer every 3 s (`offerResend`, at
    most 5 per offer). Before, one lost accept or offer on the relay left both sides on
    "Connecting…".
  - The callee keeps the last offer it answered and its answer: the same SDP again gets the stored
    answer back; a different SDP from the same device is a renegotiation (ICE restart), answered on
    the existing peer with `answer(offer:)`. A duplicate offer that arrives while the first is still
    being answered is dropped (before, it could make a second peer). The caller ignores an answer
    when no offer is outstanding or it is the answer already applied, and a restart answer that
    fails to apply never ends the call (only a failing first answer does, as before).
  - A call that was accepted but is not live after 30 s (`connectTimeout`) ends with `call-bye` and
    `engine.ending = DirectCallEngine.connectFailed` ("Could not connect" / "The call could not
    connect. Try again."); `DirectCallLayer` posts the message as a chat notice and calls
    `dismissEnding()`. This also fixes invites being declined as "busy" forever after a call got
    stuck connecting.
  - **Reconnect.** `DirectCallPeer` gained default-implemented hooks: `onConnection`
    (`DirectCallConnection`: `connected`, `disconnected`, `failed`, `stalled`), `restartOffer()`
    (nil when the peer cannot restart) and `resumeMedia()`. A peer without them behaves as before
    (it closes itself and the call ends). On `disconnected` for 2 s (`dropGrace`), or at once on
    `failed` or `stalled`, the caller makes a restart offer and sends it with the resends above,
    then again every 6 s (`restartEvery`) while still down; the callee waits for it. Both show
    "Reconnecting…" (`engine.reconnecting`, the state line) while live, and end the call with
    `call-bye` and `DirectCallEngine.dropped` ("Call lost") when it has not come back within 30 s
    (`reconnectBudget`). `connected` (or `live` again) clears it.
  - **Candidates during a restart.** While a restart offer is being made, and while the callee
    answers one, the engine holds the peer's new ICE candidates and sends them after the offer or
    answer; incoming candidates wait in the queue while a restart offer is unanswered or an offer is
    being answered. Otherwise candidates for the new ICE credentials could reach the other side
    first and be thrown away against the old description.
  - Timer tasks clear their own handle before calling `end`, so `end` does not cancel the task that
    is sending `call-bye` (a cancelled task could abort the socket send). The ring timeout does the
    same.
- **WebView engine and glue** (2026-10-23). `WebCallPeerFactory` passes
  `{ resilient: true, staged: true }` as `setup`'s eighth argument (Android passes nothing and keeps
  the old page behaviour). The page then posts `connection` states instead of closing itself, a
  `stalled` state when inbound audio packets stop for 4 s while connected (the "went silent" case:
  the connection looked fine, so nothing reacted), `remote-video` (`on`, `seen`) and `mic` (`ok`)
  after re-acquiring an ended microphone. `WebCallPeer` maps them to `onConnection`,
  `factory.remoteVideo`/`remoteVideoSeen` and `factory.micLost` (the call screen then says "Your
  microphone stopped and could not restart. Check that no other app is using it."). `restartOffer`
  runs the page's `restart` op, `resumeMedia` its `wake` op, and `setLayout(mini:)` its `layout` op.
  Protocol and CSS: `docs/app/chat-direct-calls.md`, "Reconnects, media watchdog and the staged
  view". The glue also gives audio senders `high` and video `medium` priority and prefers RED then
  Opus on the audio transceiver, on both phones.
- **Native engine resilience.** `NativeCallPeer` reports `onConnection` from
  `RTCPeerConnectionState` (`.closed` still closes), runs the same 4 s inbound-audio watchdog on the
  once-a-second stats sample the quality probe already takes (no extra stats call), and derives
  `onRemoteVideoActive` from `framesDecoded` (at least 2 new frames over the last three samples).
  `capSenders` sets `networkPriority`/`bitratePriority` from `CallMediaTuning.audioPriority`
  (`high`, 4) and `videoPriority` (`medium`, 2) and runs on every `connected`. RED: the WebRTC
  Objective-C API for capabilities and `setCodecPreferences:error:` is called through selectors
  (`rtpReceiverCapabilitiesForKind:`, read with KVC), because its Swift spelling differs between
  libwebrtc branches and the app cannot be compiled here; when the build lacks it, nothing changes.
  The order comes from `CallMediaTuning.orderedAudioCodecs`. A renegotiation answer no longer turns
  a camera the user switched off back on. `resumeMedia` re-activates `RTCAudioSession` (unless
  CallKit owns it) and toggles `isAudioEnabled` so the audio unit restarts.
- **No blank screen** (2026-10-23). Until the first remote video frame, a video call shows your own
  camera full screen and mirrored, with the partner's avatar and name over it and the state line at
  the top: during ringing the previewing page already does (`body.preview`), after `setup` the page
  keeps it with `body.staged.waiting`, and `NativeCallSurface` shows `localVideo` full screen until
  `remoteVideoActive`. When the remote video stops (camera off, a stall) the remote view is hidden
  and the avatar returns over black, with your self view back in its corner.
- **Back to chat and the mini window** (2026-10-23, `Calls/DirectCallMini.swift`). The chevron at the
  top left of `DirectCallScreen` ("Back to chat") sets `DirectCallPresentation.shared.minimized`:
  the cover closes without hanging up and `DirectCallLayer` overlays `DirectCallMiniWindow` on the
  whole shell. It shows the remote video (avatar, name and the live duration, "Calling…",
  "Connecting…" or "Reconnecting…" for voice or while no remote video arrives), a mute toggle and
  hang up; a tap returns to full screen. Drag it anywhere: it springs to the nearer side, keeps its
  height along that edge, and snaps to the corner within the top or bottom 12 %; the side and height
  are kept in `@AppStorage("fm.call.miniSide")` and `"fm.call.miniY"`. The WKWebView moves into the
  tile (it must stay in a window or WebKit stops capture; a voice call keeps it at opacity 0.01),
  and `layout("mini")` hides the page's self view. To hand the WebView over without leaving it
  out of a window, the tile also shows while the cover is not yet on screen
  (`DirectCallPresentation.fullShown`, set from the cover's `onAppear`/`onDisappear`): minimizing
  shows the tile first, and on the way back the tile goes only once the cover's surface has taken
  the WebView. The tile hosts the WebView only while minimized or returning
  (`hostsPage`: `expand()` sets `returning`, the cover's `onAppear` clears it). At the start of a
  call the cover is not on screen yet either, and a tile that took the WebView then could keep it
  after the cover's surface was made, leaving the full call screen black; there it shows only the
  avatar. Ending the call clears `minimized`, so the next invite or pre-call check presents as
  before. The mini window is under any sheet `HomeShell` presents.
- **Answering** (2026-10-23). `DirectCallActions.accept`, used by the incoming screen and CallKit's
  answer, calls `engine.holdInvite()` before the permission prompt, so the invite cannot go stale
  (9 s without a resend) while the prompt is open, and `acceptInvite()` now returns whether it
  accepted. When it fails the chat shows why: "The call ended before it could be answered." (the
  caller hung up, or the invite was gone), "Hang up the current call to answer this one.", or the
  Settings hint for camera or microphone access (now per media). CallKit's answer for an invite
  that is already gone posts the first message too.
- **Diagnostics button** (2026-10-23). The web widget's red-dot diagnostics
  (`app/public/call-quality-meter.js`: `diagnoseCall`, the fix catalogue, `rankFixes`, the memory
  of reports, fixes and lessons, `diagnosticsPrompt`) is ported to FMChat
  `Direct/CallDiagnostics.swift` with the same thresholds and text, including JavaScript's
  `toFixed`/`Math.round` rounding (`CallDiagnostics.fixed`, `round`). `CallDiagnosticsTests` reads
  `Fixtures/call-diagnostics-parity.json`, written by `node tools/call_diagnostics_parity.mjs
  <path>` from the JS module (17 sample sets, three memories, lessons, the report cap);
  `app/tests/test_call_diagnostics_native_parity.py` fails when the JS changes and the fixture was
  not regenerated. `CallQualityMonitor` diagnoses on every new sample (once a second, not on each
  level), and `CallDiagnosticsButton`, its own view so the 10 Hz waveform does not redraw it, sits
  right of the pill once the call is live: grey dot when fine, red dot on a problem, red ring when
  severe. It opens `CallDiagnosticsSheet`: each problem with what was measured, why, and the ranked
  fixes ("worked here before", "tried here before"), the prompt with Copy prompt, and a field for
  the assistant's `LESSON:` line. `CallDiagnosticsBook` keeps the memory in `UserDefaults` under
  `fm.call.diagnostics` (the web's JSON shape) and, like the web widget, counts the first-ranked fix
  as having worked when a problem clears after the sheet was opened, and as failed when the call
  ends with it still there. The prompt's device line is "ForkMesh iOS <version> (<model>; iOS
  <version>; native call screen)" with the core count and memory. The WebView `quality` event has
  no face-filter timings, so the face-filter check only runs when a sample carries them.
- **Audio session during a call** (2026-10-23). Our own calls are not reported to CallKit, so
  `OtherAppCall.inProgress` was false during them and app chimes, speech, Genie sounds,
  `PromptDictation` overhearing, `DesktopAlarm` and `GlassesWatcher` could switch the shared
  `AVAudioSession` to `.playback` or deactivate it mid-call, which kills WebKit's or WebRTC's
  microphone: the other person heard silence. `DirectCallAudio` (`Calls/DirectCallAudio.swift`,
  attached in `installCalls`) now sets `OtherAppCall.shared.setOwnCall` whenever the engine has a
  phase or a native group call is active, which also runs the usual hush (overhearing stops, alarms
  hush) and keeps everything gated on `inProgress` quiet. `OtherAppCall.ownCall` and
  `OtherAppCall.deactivateSession()` (a no-op during our call) guard the places that change the
  session without checking `inProgress`: speech (`SpeechSynthesis`, the stretch coach),
  voice-message recording and playback (`MessagesMedia`), view-once voice capture, review and
  viewing, dictation (`PromptDictation` keeps the call's category and never deactivates), the DJ
  studio, and the alarms' `hush`/`stop` (the hush that runs when a call starts used to deactivate
  the session right as it began). `NativeMicMeter.stop(deactivate:)` is not asked to deactivate
  during a call. `DirectCallAudio` also observes `AVAudioSession.interruptionNotification`: when an
  interruption ends during a call it re-activates the session for the WebView engine and calls
  `engine.resumeMedia()` (both engines); the same runs when the app returns to the front.
  `CallClipAudioPlayer` and `CallRinger.prepareSession` already leave a `.playAndRecord` session
  alone.
- **Native pre-call test** (2026-10-23). `NativePreCallScreen` shows the live camera first (a spinner
  until it starts) and now has "Test microphone": it pauses the mic meter, records 10 s
  (`AVAudioRecorder`, AAC 48 kHz mono, to a temporary file from `MessagesViewOnceFiles.fresh`) while the camera preview
  stays live, feeds the waveform from the recorder's meter, then plays it back with
  `CallClipAudioPlayer` (on the speaker without headphones), with Again and Discard. The file is
  deleted on discard, again, call, cancel and when the screen goes away.
- **Remote video, loudspeaker and the silent test clip** (2026-10-23, second pass). Real calls
  showed each side only its own camera full screen: the other person's picture never replaced it.
  `NativeCallPeer` no longer waits for the once-a-second `framesDecoded` sample to decide the
  remote video is on. It takes the remote track straight from the video transceiver's receiver
  when the peer is made (Unified Plan gives every transceiver its receiver track up front, so the
  view no longer depends on `didAdd rtpReceiver` arriving), and attaches `NativeFrameCounter`, an
  `RTCVideoRenderer` that counts the frames WebRTC actually hands to renderers. The first frame
  turns `remoteVideoActive` on at once; the 2 s stall check uses the larger of the rendered count
  and `framesDecoded`. `receiverAdded` compares track ids, since the Objective-C wrapper is a new
  object on every `receiver.track` read.
  - Video calls now run the audio session in `.videoChat` mode (`beginAudio(video:)` sets it as
    WebRTC's default configuration too, because the audio unit re-applies that configuration when
    it starts), which plays through the loudspeaker like FaceTime; `.voiceChat` sent a video
    call's sound to the earpiece, too quiet to hear with the phone at arm's length. Voice calls
    keep `.voiceChat`. The pre-call mic meter and the test recording use the same mode, so the
    ringback of a video call is on the speaker as well. `.playAndRecord` already ignores the
    ring/silent switch.
  - The test clip recorded silence: the button's click cue (`FmHaptics.tap` →
    `FmSoundCues`) switched the shared session to `.playback` a moment after the recorder
    started, because nothing counted as a call yet. `DirectCallAudio` now treats an open pre-call
    check (`DirectCallPrecheck.shared.request`) as our own call, so app sounds, speech and
    dictation leave the session alone there too. `NativeCallPeerFactory.endPreview` decides
    whether to deactivate from `DirectCallAudio.callBusy` (a real call or group call) instead of
    `OtherAppCall.ownCall`, so cancelling the check still releases the session. Before playback
    the screen sets `.playAndRecord` with mode `.default` and `.defaultToSpeaker` (loud, on the
    speaker, and not muted by the silent switch); the meter goes back to the call mode afterwards.
- **Limits.** Media runs in a WebView, which only captures the camera and microphone while the
  app is in front, so a call answered on the lock screen connects its signaling at once but its
  audio and picture start once the user unlocks and opens ForkMesh (the app button on the call
  screen). CallKit's audio session activation and WebKit's capture session have not been tried
  together on a device. Outgoing calls are not reported to CallKit (our own calls still count as
  calls for the app's sounds and audio session, see above). The alert push (phones
  without a VoIP token) uses time-sensitive delivery, which only breaks through Focus once the
  app has the Time Sensitive Notifications capability
  (`com.apple.developer.usernotifications.time-sensitive`); it is not switched on yet. The root `fullScreenCover` cannot present while another screen of `HomeShell` is
  showing a sheet or cover; the call then appears when that sheet closes, and the mini window sits
  under such a sheet. The ringback plays on whatever route WebKit's `.playAndRecord` session uses
  (often the earpiece). The 2026-10-23 changes to the app target (mini window, diagnostics sheet,
  audio session guards, interruption handling, native restart and RED) were written on Linux, where
  only `swiftc -parse` checks the app files; they have not run on a device yet.

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
`MessagesDetailsScreen`, `MessagesTrust.swift` (pending-device banners, trust lines,
`MessagesSafetyNumberScreen`); `ChatPrivacyViews.swift` holds `ChatLockGate`,
`ChatPrivacyShield` and the Settings card; calls by `Calls/` (`DirectCallViews.swift`, `DirectCallPreview.swift`,
`CallRinger.swift`) and `FaceChanger/` (`WebCallPeer.swift`, `CallFaceLandmarks.swift`,
`CallDepthCamera.swift`).

## Tests and verification

Core tests (Linux `swift test`) cover the relay, mesh (loopback sockets), local-control vectors,
history store, direct messages, receipts, view-once media and captions, push decryption, link
parsing, v4 delivery (`DirectMessagesTests`: own frames ignored, one reset and advertisement per
lost session, a received reset reseals the outbox to that device only, no superseded), the v3 UI logic (`DirectTrustUITests`: trust lines and banners, transcript order, safety
rows and scan results, the privacy switches and lock, focus reporting, office channel ids), calls (including the state lines, route notes and the ringback and ring tones in
`DirectCallsTests`), call face depth (`CallFaceDepthTests`), the call face maths
(`CallFaceParityTests`, `CallFaceStageTests`), the segmentation network against the browser's
WebGL output (`CallSegmentTests`, fixture from `tools/call_segment_parity.mjs`) and the ARKit-side
logic: blend-shape mapping, mesh binding, capture format, frame gate and quality levels
(`CallFaceTrackingTests`). The SwiftUI views and the AVFoundation, Speech, Vision and WebKit paths are
syntax-checked on Linux and compiled only on macOS CI; the Local Network prompt, multicast and
device capture paths need on-device verification.

## API for the UI (chat protocol v3)

All on `DirectMessageService` (`@MainActor @Observable`; read `trustRevision` / `routeRevision`,
which the methods below touch, to redraw):

| Call | Use |
| --- | --- |
| `devices(conversation) -> [ChatDeviceInfo]` | Every device of the DM: `kid`, `acct`, `node`, `state` (`approved`, `pending`, `blocked`), `firstSeen`, `verified`, `isSelf`, `isOwnAccount`. |
| `pendingDevices(conversation)` | The ones waiting; show the banner with Approve and Block. |
| `approveDevice(conversation, kid:) async` | Approves, opens its held frames and reseals the outbox to it. |
| `blockDevice(conversation, kid:) async` | Blocks; its frames are dropped from then on. |
| `safetyNumberChanged(conversation) -> Bool` | The cheap check for the "changed" line; `safetyNumber` runs the 5,200 SHA-512 rounds each call. |
| `safetyNumber(conversation) -> ChatSafetyNumber?` | `digits` (60), `groups` (12 of 5), `qr` (`forkmesh-safety:3:<scope>:<b64u>`), `verified`, `changed`. |
| `markVerified(conversation)` / `verifySafetyCode(conversation, scanned:) -> Bool` | After comparing digits / scanning the other phone's QR. |
| `acknowledgeSafetyChange(conversation)` | Hides the "changed" notice. |
| `onTrustEvent: (DirectTrustEvent) -> Void` | `.pendingDevice(conversation:kid:)` and `.safetyNumberChanged(conversation:)`. |
| `route(conversation) -> DirectRoute` | `.none`, `.wifi`, `.bluetooth`, `.directOnly`; `label` is "", "Direct · Wi-Fi", "Direct · Bluetooth", "Direct only". |
| `setFocus(conversation, on)` | Call when the transcript appears or goes away; app lifecycle is already applied. |

`ChatHub.setFocus(conversation, on)` and `ChatHub.directRoute(conversation)` forward to it.
`DirectTrustText` has the line and banner strings: `pendingDevice(name)` ("A new device for
Alex wants to join this chat. Check the safety number, then approve or block."),
`safetyNumberChanged(name)`, `approve`, `block`.

## Chat protocol v3 in the app

The views call `ChatHub` (`ViewModels/ChatHubTrust.swift`), which forwards to the service:
`trustBanners`, `trustNotices`, `trustName`, `approveDevice`, `blockDevice`, `safetyNumber`,
`safetyVerified`, `markSafetyVerified`, `verifySafetyCode -> DirectSafetyScan`,
`acknowledgeSafetyChange`, `trustRevision`, `conversationRouteLine`, `conversationDetailLine` and
`officeConversation(channelId:)`. The texts are in `DirectTrustText` and `ChatPrivacy`.

**Route line.** The DM header's detail line is `ChatHub.conversationDetailLine`: presence, then
`conversationRouteLine` (`DirectRoute.label`: "Direct · Wi-Fi", "Direct · Bluetooth",
"Direct only"), then the server-copies note. `route` reads `routeRevision` and `trustRevision`,
so the header redraws when a link comes or goes or the focus rule starts to hold.

**Focus.** `MessagesConversationView` owns a `DirectFocusReporter` and calls `update` on appear,
disappear, conversation change, `scenePhase` change and `shellTabHidden` change. Focus is on
only while a `direct:` transcript is on screen, its tab is the visible one, the scene is active
and the chat is not locked (the lock replaces the transcript, which then disappears). Mounted
tabs stay alive behind the selected one, so `onDisappear` alone would leave focus on after
switching tabs; that is why the tab flag is part of it. The reporter turns the old conversation
off before turning the new one on.

**Pending devices and trust lines.** `ChatHub.trustBanners(conversation)` lists the pending
devices of a `direct:` conversation (`DirectTrustNotices.pending`), oldest first. Each shows as a
banner under the header with the `DirectTrustText` line, **Approve** (accent, prominent) and
**Block** (destructive, asks first) and a link to the safety number. A device of the user's own
account reads `DirectTrustText.ownDevice` ("A new device was added to your account. ..."). The
same entries, plus "Your safety number with X changed" while `safetyNumberChanged` holds, join
the transcript as `ChatConversationItem.notice` lines (`ChatHub.trustNotices`, merged by
`ChatConversationLayout.items(notices:)`): a pending line sits at the device's `firstSeen`, the
changed line at the time `onTrustEvent(.safetyNumberChanged)` arrived (the hub keeps it in
memory; after a relaunch it is the time the line was first drawn). The lines come from the trust
state rather than from stored events, so they go away once the device is approved or blocked, or
once the change is acknowledged. Tapping the changed line opens the safety number.

**Safety number.** `MessagesDetailsScreen` has a "Safety number" row for `direct:` conversations
(verified state on the right, from `ChatHub.safetyVerified`, which reads the device list and not
the number). `MessagesSafetyNumberScreen` shows the 60 digits as `ChatSafetyNumber.rows` (3 rows
of 4 groups of 5, monospaced), the QR code of `qr` (`QrCodeImage`, CoreImage
`qrCodeGenerator`), **Scan their code** (the existing `QrScannerSheet`, AVFoundation, camera
string already in Info.plist), **Mark as verified** and the Verified badge. A scan returns a
`DirectSafetyScan` with its message: verified, not a safety code, another conversation's code,
or a mismatch. Opening the screen while the number is marked changed calls
`acknowledgeSafetyChange`; the screen keeps showing the changed notice until it closes. The
number is recomputed only when `trustRevision` moves.

**Privacy.** `ChatPrivacy` (FMChat, registered by `ChatFeature.register`) holds two switches in
the shared preferences, shown in Settings > Chat privacy (`SettingsSectionId.chatPrivacy`):

- `chat/privacy/app-switcher-cover`, "Hide chats in the app switcher", on by default.
  `ChatPrivacyShield` puts an opaque window (launch colour and logo) over each scene on
  `UIScene.willDeactivateNotification` and `didEnterBackgroundNotification`, and removes it on
  `didActivateNotification`. It is UIKit and synchronous because the app switcher card and
  snapshot are taken before SwiftUI would draw a state change. The cover also shows while
  Notification Center, Control Center or a system prompt (Face ID included) is in front.
- `chat/privacy/lock`, "Lock chats with Face ID" (Touch ID, Optic ID or "your passcode" from
  `LAContext.biometryType`), off by default. When on, the app starts locked and locks again each
  time it goes to the background (`AppLifecycleState.paused`). `ChatLockGate` wraps
  `ChatScreen`: while locked it shows a lock placeholder instead of the chat and, once the Chat
  tab is visible and the scene active (opening the tab or a chat push tap), asks
  `LAContext.evaluatePolicy(.deviceOwnerAuthentication)`. A failed or cancelled prompt is not
  repeated by itself; the placeholder's **Unlock** asks again. A phone with no passcode cannot
  evaluate the policy, so the switch is disabled there and an existing lock opens. The lock
  only gates the Chat tab: notification previews follow the "Notification content" setting.


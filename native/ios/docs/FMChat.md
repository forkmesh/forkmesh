# FMChat

The relay chat room, relay switching, the local mesh room, the local control channel (the phone
driving a desktop, and serving its on-device model to it), node sync, and the chat view models.
`import FMChat` (deps FMBase, FMCrypto, FMNet, FMAccount, FMApi, FMLocalAI). Every service is a
`@MainActor @Observable final class`; `addListener`/`removeListener` (FMAccount `Listeners`) for
code outside SwiftUI; each has a `revision` bumped on every change.

Ports: relay_service, relay_probe, relay_switch_controller, cross_relay_catalog,
local_mesh_service, mesh_transport, node_sync_service, node_sync_applier, user_presence,
local_control/local_control_protocol + local_control_service, and the logic of chat_screen,
relay_settings, relay_networks_screen, local_desktop_screen, relay_switch_scope.

## Wiring: `ChatFeature.register()` (ForkMesh/Features/Chat/ChatFeature.swift)

`@MainActor static func register()`, no arguments, called by AppServices after Account and Api.
Reads from `Services`: SettingsService, AuthService, ApiService, DeviceIdentity (a throwaway
in-memory identity if the device key failed to load), SecureStore, KeyValueStore, HTTPClient,
WebSocketConnector; optional PerformanceMonitorService, NotificationService, AccountEventChannel,
PushNotificationService, AppLifecycle. Registers:

| Service | Registered as |
| --- | --- |
| `RelayService` | itself and `OwnStatusAnnouncer` (AppServices sets `userStatus.relay` from it) |
| `ChatHub` | itself and `ShellChatBridge` |
| `LocalMeshService` | itself |
| `LocalControlService` | itself and `ShellLocalDesktopBridge` |
| `RelaySwitchController` | itself (suspend/resume: node sync, account events, push, pings) |
| `NodeSyncService` | itself (no applier yet: see "Node sync") |
| `CrossRelayCatalog` | built per screen, not registered |

It also sets `relay.roomPassphraseProvider` (`api.roomChatPassphrase()` only with a session),
`control.inferenceProvider = { Services.get(LocalInferenceService.self) }` (LocalAI registers
after Chat, so it is looked up per request), `UserPresence.instance.prompt =
LocalAuthPresencePrompt()`, forwards AppLifecycle to relay and node sync, and starts the mesh,
the relay and node sync. The relay-switch overlay is `RelaySwitchScope { content }` /
`.relaySwitchScope()` (Features/Chat); the shell should wrap its root in it.

## Relay (`Relay/`)

- `RelayService(settings:identity:secureStore:connector:performanceMonitor:platform:clock:sleep:)`.
  State: `state: RelayConnectionState` (`.offline/.connecting/.connected`), `channels`,
  `currentConversation` ("#general", "@nodeId"), `log` ("HH:mm:ss  line", 500 max),
  `framesReceived`, `ownStatus`, `selfId`, `onlineCount`, `totalUnread`, `badgeUnread`, `unreadConversations`.
  Reads: `roster() -> [Member]` (self first), `rosterGroups() -> [MemberGroup]`, `messages(conv)`,
  `recentMessages`, `unreadMessages`, `notificationMessages`, `unseenNotificationCount`,
  `liveStatusFor(senderId)`, `hasUnread(_)`, `unreadCountFor(_)`.
  Actions: `connect() async`, `disconnect() async`, `appLifecycleChanged(_)`,
  `sendMessage(_) async -> Bool` (shows at once, removed on failure), `deleteMessage(_) async`,
  `switchConversation`, `markConversationRead`, `markAllRead`, `markBadgeSeen`, `noteUnread(_)`,
  `markNotificationsSeen`,
  `setOwnStatus(_)`, `resetForRelaySwitch()`, `dispose()`, `identityChanged()`. Hooks:
  `roomPassphraseProvider`, `onLogLine`, `accountProvider`. Wire: frames say `version: "flutter-1.0"` (a wire value, kept as is); public #channel
  chat is room-cipher only, everything else RoomSeal-sealed; `persist`/`notify` ride outside.
- `ChatPersona` (`Threads/ChatPersona.swift`, port of Android's PARITY #38): who the device speaks
  as in the relay and mesh rooms. `resolve(nodeKey:account:displayName:)` gives the Display name
  or the account when signed in, else `guestChatName(nodeKey)` ("Guest ####", the desktop's
  derivation); never the node key's short form, which is what the iPhone chat showed for a
  signed-in user with no Display name saved (a migrated or restored session). A Display name left
  from an earlier sign-in no longer outlives the session. `stamp(&frame)` adds `accountKind` and
  `ownerUser` to every frame and mesh beacon, so web chat keeps the message and the desktop counts
  the phone as a person. `ChatFeature` sets `accountProvider` to `auth.session?.nodeName` on both
  rooms and calls `relay.identityChanged()` on auth and settings changes, re-announcing presence.
- `RelayProbe(http:timeout:clock:)`: `check(input, existing:, label:)`, `verify(profile)` ->
  `RelayProbeResult` (`status`, `ok`, `message` (the sentence to show), `profile` (ready to
  save), `rev`, `version`, `missingOptional`). TLS failures are recognised from the transport
  error text.
- `RelaySwitchController(settings:auth:api:relay:onSuspend:onResume:joinTimeout:minimumDuration:)`;
  `api`/`relay` are protocols (`RelaySwitchCache`: ApiService; `RelaySwitchSocket`:
  RelayService). `switchTo(id) async -> RelaySwitchResult` (`ok`, `needsSignIn`, `offline`,
  `error`), `removeRelay(id)`, `phase`, `busy`, `statusLine`, `overlayTitle`, `target`,
  `clearError()`. Hooks are settable (`onSuspend`, `onResume`, `async throws`).
- `CrossRelayCatalog(settings:auth:http:timeout:)`: `refresh() async`, `entries:
  [RelayCatalogEntry]` (`relay`, `active`, `signedIn`, `repositories`, `loading`, `error`),
  `loading`, `totalRepositories`.

## Local mesh (`Mesh/`)

Sockets: BSD sockets in the core (`MeshSockets.swift`), not Network.framework. The room's logic
is written against socket behaviour (a unicast sweep sent from the discovery port so a stateful
firewall passes the answer, EHOSTUNREACH as "Local Network is off", send-buffer-full vs refused),
and Linux runs the real loopback cases.
`MeshAddress` (numeric v4/v6, `address`, `loopbackIPv4`), `isLocalNetworkAddress`,
`sweepAddresses`, `sweepBroadcastAddress`, `MeshTCPConnection.connect(to:port:timeout:)`
(`events` AsyncStream of `.data`/`.closed`, `write` never blocks), `MeshTCPListener.bind`,
`MeshUDPSocket.bind` (a refused send does not close it), `MeshInterfaces.list`.

- `LocalMeshService(settings:identity:secureStore:transport:historyFile:platform:clock:)`:
  `start() async -> Bool`, `stop() async`, `dispose()`, `sendMessage(_) async -> String` (id, ""
  = not sent), `sendTyping`, `connectToPeer(address, port:)`, `markRead()`. State: `messages`,
  `running`, `networkActive`, `bluetoothActive`, `unread`, `hasUnread`, `peerCount`,
  `listenPort`, `localNetworkBlocked`, `isQueued(id)`, `members()`, `controlHosts()`,
  `displayName` (the persona's name), `accountProvider`, `diagnosticsLine()`, `diagnosticsDetail()` (not observed: refresh on a timer).
  History file: `defaultMeshHistoryFile()` (Application Support/local_mesh_history.json).
- `MeshNativeTransport` (Bluetooth + multicast lock) is a protocol. The app passes
  `CoreBluetoothMeshTransport` (BLE GATT, peripheral and central; built only where
  CoreBluetooth exists). Tests and Linux get `UnavailableMeshTransport`. `MeshBle` /
  `MeshBleOutbox` hold the platform-free parts (UUIDs, dial tie-break, chunking,
  long-write reassembly) and are tested on Linux (`MeshBleTests`).
  `roomOpened()` lets the transport ask for Bluetooth permission, which it does
  only once `#mesh` has been opened.
- `MeshBeacon`, `meshFrameBytes`, `meshTakeFrame`, `MeshByteBuffer`, constants `meshConversation`
  ("#mesh"), `meshRoomName`, `meshDiscoveryPort` (48732), `meshMulticastGroup`.

iOS needs `NSLocalNetworkUsageDescription` in Info.plist; multicast additionally needs the
`com.apple.developer.networking.multicast` entitlement (without it the subnet sweep finds peers).

## Local control (`LocalControl/`)

- Protocol functions (byte-identical to desktop/src/LocalControl.cpp, vectors in the tests):
  `controlTranscript`, `deriveControlKeys`, `controlPairingCode`, `sealControlFrame`,
  `openControlFrame`, `controlB64`, `controlFromB64`. `LocalControlConnection.open(address:port:
  identity:deviceName:user:platform:expectedKey:timeout:)`: `request(method, params, timeout:)
  async throws -> JSONObject`, `events` (AsyncStream of `{event, data}`), `sendEvent(name,
  data)` (new), `done() async`, `close()`, `serverKey/Name/User`, `pairingCode`.
  Errors: `LocalControlException(code)`.
- `LocalControlService(mesh:identity:prefs:userName:connector:filesDirectory:platform:clock:sleep:)`:
  `status` (`.idle/.connecting/.awaitingApproval/.connected/.failed`), `pairingCode`, `error`,
  `desktopKey/Name/User`, `host`, `hosts`, `paired`, `isPaired`, `agents:
  LocalAgentsSnapshot?`, `agentsLoading`, `receivedFiles`, `uploadName`, `uploadProgress`;
  `connect(host, automatic:)`, `disconnect(hold:)`, `forget(key)`, `refreshAgents()`,
  `startAgent(owner:name:task:provider:)`, `sendPrompt`, `stopAgent`, `continueAgent`,
  `transcript`, `sendFile(url, name:)`. Prefs: `localControl/pairedDesktops` (JSON list).
  Received files go to Documents/"ForkMesh from desktop". Conforms to `ShellLocalDesktopBridge`.

### On-device AI for paired desktops (new; opt-in)

Setting `localControl/serveLocalAI` (Bool, default false): `servesLocalAI`,
`setServesLocalAI(_)`, shown as a switch on LocalDesktopScreen. Only the desktop this phone is
connected to AND paired with (status `.connected`) is answered; the model policy is
FMLocalAI's (`LocalModelManager.modelForPeers`: on-device AI switched on, model downloaded).

Everything travels as control-channel event frames, so it is strictly additive: an older phone
ignores unknown events, and an older desktop's `LocalControlServer::handleMessage` drops any
frame without a positive `id` and a `method` (frame counters still advance, so the channel stays
in step). Requests are not `{id, method}` because the retired Flutter app treats any int `id` from the
desktop as the answer to its own request.

```text
phone -> desktop  {"event":"node.capabilities","data":{"ai":{"v":1,"enabled":true,
                    "methods":["ai.info","ai.chat"],"stream":true}}}      on approval and on every
                                                                          switch change ({"v":1,"enabled":false} when off)
desktop -> phone  {"event":"ai.request","data":{"rid":7,"method":"ai.chat","stream":true,
                    "params":{"messages":[...],"tools":[...],"max_tokens":512,...}}}
                  {"event":"ai.cancel","data":{"rid":7}}
phone -> desktop  {"event":"ai.delta","data":{"rid":7,"text":"Hel"}}                 (stream only)
                  {"event":"ai.tool_call","data":{"rid":7,"tool_call":{...}}}        (stream only)
                  {"event":"ai.response","data":{"rid":7,"ok":true,"model":"...","message":{...},
                    "finish_reason":"stop","usage":{...},"tokens_per_second":38.5}}  exactly one per request
                  {"event":"ai.response","data":{"rid":7,"ok":false,"error":"disabled","message":"..."}}
```

`rid` is a positive int chosen by the desktop, unique among its open requests. Methods:
`ai.info` (answer = LocalInferenceService.info: `available`, `model`, `max_tokens`, `busy`,
`device`) and `ai.chat` (OpenAI Chat Completions request/reply, docs/FMLocalAI.md). Errors:
`disabled` (switch off), `unavailable` (no model in this build), `unknown_method`,
`bad_request`, plus FMLocalAI's `model_unavailable`, `busy`, `generation_failed`. A channel that
closes cancels running requests; iOS stops GPU work in the background (`finish_reason:
"cancelled"`).

## Node sync (`NodeSync/`)

`NodeSyncService(settings:identity:session:http:connector:pollInterval:requestTimeout:random:clock:sleep:)`:
`mode`, `connection`, `payload`, `items`, `carried`, `counts`, `lastSyncAt`, `lastError`,
`syncing`, `ackedTotal`, `pendingTotal`, `owner`, `hasDesktopNode`; `start()`, `stop() async`,
`refreshSession()`, `syncNow(manual:) async`, `loadCarried()`, `clearCarried()`,
`appLifecycleChanged(_)`; `applier`, `onNewItems`, `onUnauthorized`. Prefs `nodeSync/carried/<relay>`,
`nodeSync/applied` (in the project's namespace). `LocalNodeSyncApplier(repos: NodeSyncRepositorySource,
author:prefs:ledgerKey:clock:)` works against the protocols in `NodeSyncWorkspace.swift` (FMChat
cannot import FMCode); the adapter over LocalRepoStore/GitWorkspace is FMCode's
`LocalNodeSyncAdapter.swift`, and it must: throw from `readFile` for a missing path,
create parent directories on write, stage deletions, throw `NodeSyncWorkspaceError.nothingStaged`
(or any error saying "Nothing is staged") for an empty commit. The LocalRepos feature installs
the applier on the registered `NodeSyncService`; without one (previews, tests) the node acks
nothing ("no applier" mode).

## View models (`ViewModels/`)

- `ChatHub(relay:mesh:isGuest:clock:)` (also `ShellChatBridge`): `rows: [ChatRow]` (`message`,
  `continuesGroup`, `queued`, `deletable`), `title`, `titleIcon`, `badge`, `emptyTitle`,
  `emptyMessage`, `inMesh`, `otherRoomsUnread`, `composerConnected`, `composeBlocked` ("post in
  #x" for guests), `memberGroups`, `channels`, `liveStatus(for:)`, `select(_)`, `roomShown()`,
  `delete(_)`, `sendChatText(_) async -> Bool`, `notice` / `clearNotice()`, `onGuestBlocked`.
  ForkMesh Apps (WS22): `apps: AppsClient?`, `appCommands`, `refreshAppCommands(force:)`,
  `resetAppCommands(clearReplies:)`, `slashCommands(_ draft)` (built-ins plus app commands),
  `appSuggestions(_)`, `appsChannelId(_ conversation)` (a public office room's id, else ""),
  `interact(_ message, actionId:value:)`, `isAppBusy(_:actionId:)`. `sendChatText` runs `/apps` and an
  installed app's command instead of posting it; a reply only the sender sees is kept in memory and
  merged into `rows` (`transcript`). After a post in a public office room that names a `mentionable`
  app, the message is forwarded to `POST /api/apps/mentions`. `OfficeChannelService.post(_:text:asBot:)`
  returns the sent frame id; `send` wraps it.
  Statics: `segments(_)` (``` fences), `timeLabel` (12-hour, "9:05 AM"), `senderLabel`, `platformIcon`,
  `unreadLabel`, `continuesGroup` (never across a local day), `startsNewDay(_:after:)` and
  `dayDividerLabel(_:now:)` ("Today", "Yesterday", "Monday, October 5", with the year when it is not
  the current one) for the transcript's Slack-style day separators. `guestChatChannelName` = "general".
- `RelaySettingsModel(settings:auth:relay:switcher:probe:)`: `rows: [RelayRowInfo]`, `testing`,
  `notice`, `busy`, `connection`, `missingFeatures`, `missingExplanation`, `buildLine`,
  `explanation`; `switchTo`, `test`, `rename`, `removeConfirmation`, `remove`, `added`,
  `setLogo`, `connectToggle`, `stateLabel`. `AddRelayModel(relays:probe:)`: `host`, `label`, `check()`,
  `result`, `checking`, `canAdd`, `checkLabel`, `missingLine`, `add() -> RelayProfile?`.
  `AddProjectModel(relays:probe:)`: `name`, `host`, `check()`, `result`, `known`, `canCreate`,
  `needsCheck`, `createLabel`, `checkLabel`, `missingLine`, `create(logo:) -> RelayProfile?` (a name
  alone makes an instance-less project; a host must pass the probe first).
  `RelayNetworksModel(catalog:settings:switcher:)`: `summary`, `refresh`, `open(on:)`,
  `switchTo`, `sectionTitle`, `signedOutNote`.
- `LocalDesktopText` (card, chip, codes, error sentences, sizes), `LocalNewAgentModel(control:)`,
  `LocalAgentModel(control:sessionId:)` (`start()`/`stop()` follow the desktop's bells).
- `UserPresence` (`instance`, `override`, `prompt: PresencePrompt?`, `forceDialog`,
  `confirm(reason:dialog:) async -> Bool`); the app's prompt is `LocalAuthPresencePrompt`.

## On-device history file (`Threads/ChatHistoryStore.swift`)

`ChatHub.attachHistory` restores `relay-<scope>.json` from Application Support
once, and on every relay change asks `ChatHistoryStore.scheduleSave` to save
again 2 s later. Until 2026-10-14 (branch 1994) that save ran on the main
thread. It bounded every channel (up to 200 messages and 2 MB of text each),
JSON-encoded the whole history, and wrote it atomically with file protection.
It did this even when nothing had changed, because the relay's 60 s presence
tick notifies its listeners too. Now:

- The main thread only takes `ChatHub.historyParts()`. That is a copy of the
  relay history, the thread, reaction, edit and look state, the retention and
  the time, all copy-on-write, so it is cheap.
- `ChatHistoryParts.snapshot` bounds and filters (retention, orphaned
  reactions and roots) on a serial utility queue (`ChatHistoryWriter`), which
  then encodes and writes.
- The writer remembers the last snapshot it wrote or loaded for each file, and
  skips the encode and write when the new one is equal.
- `erase(scope:)` cancels a pending save and removes the file on the same
  queue, after any write already queued.
- `save(_:scope:)` still writes synchronously.
- `flush()` waits for queued writes and returns how many were made.

The restore at attach time still reads and decodes on the main thread, once
per scope. Tests: `ChatHistoryStoreTests`.

## Disappearing messages

Chat messages disappear after the account's retention (FMApi `ChatRetentionService`; default
seven days, shorter per account or per chat, synced through the server). `ChatHub.retention` is
the current preference and `ChatHub.applyRetention(_:)` adopts a new one and prunes at once.
FMChat does the enforcing, because FMApi cannot see chat state: the hub hands one
`ChatExpiry` closure (`(key, timestamp) -> Bool`) to `RelayService`, `LocalMeshService`,
`ChatExtras` (thread replies) and, when it is set, `OfficeChannelService`. Each drops an expired
message on arrival (live frames, relay history replays, office history pages, mesh joins) and on
restore, and has `pruneExpired()` for what is already in memory; the mesh also drops it from the
history it hands to joining devices and from its file. `ChatHistoryCodec.bounded(_:retention:now:)`
leaves expired messages out of the saved file. `startRetentionPruning()` prunes every five minutes
and the installer prunes again on resume, so a message goes within five minutes of its cutoff
while the app is open.

Keys are shared with the other clients: relay channels and DMs use the conversation as is
(`#general`, `@<peerId>`), office channels `channel:<roomId>` (`retentionKey(_:)`, falling back
to the conversation when the room is unknown), the mesh room `#mesh`. The pickers are
`ChatRetentionMenu` in the channel and person context menus of `ChatChannelSidebar` (presets plus
"Use default (...)", which clears the override) and `ChatRetentionSettingsRow` under Profile in
Settings.

## Conversations: direct messages and private groups (iMessage-style)

Requested 2026-10-02: on the phone, a direct message should look and work like iMessage, with a
shared custom background, group management and photos. `ChatHub.isConversation(_)` decides which
rooms get that view: relay DMs (`@<peerId>`), server DMs (`direct:<id>`), relay private channels
(`extras.isPrivate`) and private office rooms that are neither server-visible nor read-only.
Public channels and `#mesh` keep the Slack-style transcript. `ChatTranscript` in `ChatScreen.swift`
switches to `MessagesConversationView` for those rooms and passes its own row actions, edit alert
and send, so reactions, edits, deletes and `/issue` behave the same in both layouts.

**Layout (`ViewModels/ChatConversation.swift`).** `ChatConversationLayout.items(rows:events:group:timeZone:)`
turns `rows` plus the look events into `ChatConversationItem`s: a centred time line when the gap
from the previous entry is 15 minutes or more or the entry starts a new local day (`timeZone:`,
default `.current`) ("Today 6:39 AM", "Yesterday", a weekday within a
week, then "Sep 3 at" / "Sep 3, 2025 at"), runs of one sender (`ChatBubblePosition`; only the last
bubble of a run has a tail), the sender's name above a run in groups, and a receipt mark under
every one of your own messages (`ChatBubble.receipt`, a `ChatReceipt`; see "Receipt marks" below).
One to three emoji with nothing else are drawn large without a bubble (`isJumboEmoji`).

**Shared look (`Threads/ChatLook.swift`).** A conversation's background (one of
`ChatBackgroundCatalog.presets` or a JPEG), group name and group photo travel as `look` frames
(`field`: `background` / `name` / `photo`; `preset`, `image` base64, `name`). They go through the
same end-to-end sealed transport as the conversation's messages: `ChatExtras.sendLook` on the relay
(routed like a reaction, persisted for channels) and `OfficeChannelService.postFrame` for office
rooms, which also feeds every unsealed office frame to `onFrame` so the hub can apply looks and
attached files from live frames and history pages. Each field is last-writer-wins on `(ts, id)`
(`ChatLookStamp`), so replayed history cannot undo a newer change. Every change also becomes a
`ChatLookEvent` shown in the transcript ("Pat changed the background."), at most 40 per
conversation and subject to disappearing-message retention. A DM `look` frame is applied only when
it is addressed to this device (`to`) or sent by it, under `@<sender>`. Looks and events are saved
in the chat history file (`looks`, `lookEvents`). Pictures are capped at 600 KB
(`ChatLookFrames.maxImageBytes`): a frame is base64-encoded three times on its way out (file,
sealed frame, room envelope), and the served stack's room WebSocket refuses messages over 2 MiB
(`app/host/forkmesh_host/durable_cluster.py`).

**Shared looks in direct messages (2026-10-13).** End-to-end direct conversations
(`DirectMessageFrames.isDirect`) send a background as a sealed `look` frame through
`DirectMessageService.sendLook` (`persist`, no `notify`), and both phones apply it through
`ChatExtras.applyRoomFrame`. Earlier the same day it was kept on the device only
(`setLocalLook`, now removed). Tested in `DirectLiveTests.testADirectBackgroundIsSharedWithThePeer`.

**Touch, movement and drawing in direct messages (2026-10-13).** `DirectLiveEngine`
(`Direct/DirectLive.swift`) queues taps, stroke points and motion samples per conversation and
sends them as one sealed `live` frame at the pace the relay announced (`forkmesh-live`, read by
`DirectMessageService.liveInterval`). `DirectMessageService.makeLiveEngine()` wires it to
`sendLive` and `onLiveFrame`; `ChatHub.live` holds it. Wire format, relay budget and the iPhone UI:
`docs/app/chat-private-rooms.md`, "Touch, movement and drawing in direct messages".

**Photos and voice messages.** `ChatHub.sendMedia(fileName:mime:data:)` caps media at 600 KB for the
same reason. The app re-encodes photos as JPEG (longest side 1,600, quality stepped down until they
fit) and records voice messages as 32 kbit/s mono AAC, up to two minutes. Relay rooms send through
`sendAttachment`; office rooms send a `chat` frame with `fileName`, `fileMime` and `file`.
`ChatRow.attachment` is now filled for any message with a file, office ones included. The relay
used to drop the sender's own copy of a DM file frame (`dm` frames were only appended when
addressed to this node); `RelayService.handle` now files a self-sent `dm` under `@<to>`.

**Group management.** `MessagesDetailsScreen` shows name and photo (shared look), the people
(office: the server member list with add and swipe-to-remove for managers, plus Group Settings;
relay private channels: everyone seen posting, with Add People sending relay invites), the
background picker, a grid of the conversation's photos, disappearing messages and the encryption
summary. "Delete Conversation" hides its messages on this phone only (`clearConversationHistory`);
"Leave this Conversation" leaves a relay private channel (`leaveGroupConversation`). Relay private
channels have no server roster, so nobody can be removed from one.

**Not done.** Inline quoted replies (replies open a thread instead), message effects, stickers and
live location need fields the other clients do not send or understand yet. FaceTime-style calls
exist only for `direct:` rooms (see "Voice and video calls" below).

## Slack-style workspace

Requested 2026-10-02: on the phone's chat page, a way to start a new message to someone, with
autocomplete, plus the other features Slack has.

**Server direct messages are now wired.** `DirectMessageService` (`direct:<id>` rooms,
`/api/chat/direct-messages`) existed in the core but the app never created it. `ChatInstaller` now
builds it next to `OfficeChannelService`, sets `hub.direct`, refreshes it on sign-in and calls
`clear()` (close sockets, drop records and history) on sign-out. The sidebar refreshes it when it
appears and on pull to refresh. Server DMs reach someone who is offline; relay DMs (`@<nodeId>`)
only reach a node that is online.

**New message (`ChatNewMessageSheet`, `ChatRecipientPicker`).** The compose button sits in the
room header, the sidebar's jump bar, the Direct messages header and the channel add menu. The To:
field shows chips and a suggestion list from `ChatDirectory` (FMChat, `ViewModels/ChatWorkspace.swift`):
`ChatHub.directorySource` merges the relay roster, server DM partners, recent recipients, office room
members, senders seen in relay history and the account search (`DirectMessageService.searchUsers`,
debounced 250 ms, two characters or more, signed in only). People are merged by account (a roster
member's `owner`), or by name when there is none; this device's own names, ForkBot, apps and
transient guests are left out. Order with an empty query: recent recipients, then online, then
existing DMs, then the rest. With a query: prefix, then word prefix, then substring. `@` limits the
list to people, `#` to channels. Picking a channel replaces the other chips; with people chosen, only
people are offered. `ChatHub.conversation(for:)` decides where the message goes:

- one channel: that room;
- one person: an existing server DM with that account, else `direct.start(username:)`, else (no
  account, or the account was not found) a relay DM to their node when they are online;
- several people: a private office room whose members are exactly them and you, reused when one
  exists, else created with `DirectMessageFrames.groupChannelName`;
- people and a channel together are refused; guests get the sign-in sheet.

`sendNewMessage(to:text:)` opens the conversation, records the recipients (`workspace.noteRecipients`)
and sends the text through `deliver(_:to:)`, which is the old body of `sendChatText` with the
conversation as a parameter. An empty text just opens the conversation ("Open" in the sheet).

**Workspace state (`ChatWorkspace`).** Saved items, muted and starred conversations, per-conversation
drafts and recent recipients, in the KeyValueStore under `chat/workspace/<project id>/...` (saved
items and drafts as JSON, the rest as string lists). It is per phone; other clients do not see it.
At most 200 saved items, 20 recent people and 4,000 characters per draft. Leaving a channel forgets
its star, mute and draft.

- Drafts: the transcript saves the composer text for the room it belongs to when the room changes or
  the screen goes away, and loads the new room's draft. A delivered message clears its room's draft.
  The sidebar shows a pencil on rooms with a draft.
- Mute: no unread dot, a bell-slash icon, the row dimmed (`ChatHub.isUnread`). It is local only:
  server pushes are not muted.
- Unread dots and the Chat tab badge are counted apart. Opening the Chat tab (or tapping a chat
  push) calls `markBadgeSeen`, which clears the tab badge (`badgeUnread`, what arrived since the
  tab was last opened) but leaves every conversation's dot until that conversation is opened.
  It used to call `markAllRead`, so after two pushed messages the channel list showed nothing new
  by the time it was on screen. The "N unread messages" banner in a room still marks all read.
  Office channels have no relay history, so a live message in one that is not open is counted
  with `relay.noteUnread`; one that arrived while its socket was closed is not.
- Star: a Starred section on top of the sidebar; starred rooms leave their usual section.
- Saved for later: message menu; a "Saved for later" line on the message; the Saved tab of search.
  A saved item keeps a copy of the text; `ChatHub.pruneExpired` drops it once its message would
  have disappeared under the room's retention, like the message itself.

**Search (`ChatSearchSheet`).** Messages, Mentions and Saved. `ChatSearch.parse` reads `from:`,
`in:`, `has:file`, `has:link` and words (all must match, case-insensitive). `ChatHub.searchMessages`
looks through every room this phone has loaded (relay channels and DMs, office rooms, server DMs,
`#mesh`) and thread replies, newest first, 200 at most; it searches only what is on the phone.
`jumpTargets` lists matching conversations above the messages. Mentions are messages from others with
`@<your name or account>`, `@here`, `@channel` or `@everyone`.

**Message actions.** Save for later, Remind me about this (a local notification in 20 minutes, 1 or
3 hours, tomorrow 9:00, next Monday 9:00; `ChatReminderOption`, `ChatReminders`), and Share message
(`ChatShareMessageSheet`: the same To: picker plus an optional comment; the shared text is a `> `
quote with "— sender in #room" under it, `ChatRichText.forwardText`). Both transcript layouts get them
through `ChatMessageLaterMenu`.

**Composer.** `#channel` completion (`ChatComposerTools.channelToken`), `:emoji:` completion from
`ChatEmojiShortcodes` (two letters or more), and a Format menu (`ChatRichText.apply`: bold `*x*`,
italic `_x_`, strike `~x~`, code, code block, quote, bulleted list; applied to the whole draft and
toggled off when already applied). `deliver` expands known `:shortcodes:` to emoji unless the text
has a backtick, so other clients see the emoji. Slash commands: `/msg` and `/dm @person text`,
`/shrug [text]`, `/me text` (sent in italics), `/mute`, `/search words`, `/saved`; they win over an
app command of the same name.

**Pasted images and the + menu.** Both chat fields (`ChatComposerBar` for rooms and threads,
`MessagesComposer` for conversations) are `ChatTextField`, a UIViewRepresentable over the agent
composer's `ComposerUITextView`: a SwiftUI `TextField` never offers Paste for an image. Where the
composer can send files (rooms with attachments, conversations with `canSendMedia`), long-press Paste
and Cmd-V take images through `PromptPasteboard.read()` and `ChatPastedImages.send` sends each one at
once as a JPEG (1600 px, 600 KB, `MessagesImages.jpeg`) through `sendMedia`; the pasted text stays in
the draft. The fields bind focus as a plain `Binding<Bool>` (UIKit first responder, as
`CodeEditorView` does) because `@FocusState` cannot follow a UIKit view. The room composer's "Photo or
video" item is a button that raises `.photosPicker(isPresented:)` on the bar: a `PhotosPicker` inside a
`Menu` is torn down when the menu closes and never opened. Picked photos go through the same JPEG path
(an iPhone HEIC is often over the 2 MB file limit, and browsers cannot show HEIC); videos are sent as
they are, up to 2 MB.

**Formatting on screen.** `ChatRichText.spans` reads Slack's markers (and `**bold**`): a marker opens
at the start or after a space or opening punctuation, the next character is not a space, and it
closes before a space, punctuation or the end, on the same line; so `snake_case` and `2*3*4` stay
plain. `chatRichAttributed` (Kit/ChatLinkText.swift) turns links and spans into an AttributedString
for `ChatLinkText` and the iMessage bubbles; `ChatMessageBody` draws `>` lines with a bar.

**Not done.** Huddles, scheduled sends, pins and bookmarks shared with the room, read receipts,
"mark unread", workflows and canvases need fields or servers the other clients do not have. Mute,
star, saved items, drafts and reminders stay on this phone. Search covers loaded history only.
The Android app has not been changed.

## Direct message delivery

Task 1820: direct messages stayed on the relay ("9 messages on the server") and never reached the
other person. Two causes: the phone only had a socket to the DM that was open on screen, and a
message written before the sender knew the recipient's device key was sealed to the sender alone, so
nobody could ever open it. The rules every client follows are in `docs/app/chat-private-rooms.md`
("Server copies of direct messages"). On iOS they live in `DirectMessageService`:

- **Which DMs have a socket.** A DM the user opened (`open`, through `ChatHub.select` or `send`)
  and every DM with unread messages, at most `DirectMessageFrames.maxWatchedRooms` (16). The unread
  ones are connected after each `refresh()`. Opening a socket no longer clears the unread count;
  `markRead` does.
- **Reconnect.** A dropped socket to a wanted DM reconnects after 1 s, doubling to 30 s, with a
  fresh `room-access` ticket each time. Nothing reconnects while the app is in the background;
  `appLifecycleChanged(.resumed)` resets the backoff and reconnects at once.
- **New DMs.** `ChatInstaller` refreshes the list on the `direct-messages` account event and on
  resume, and `ApplePushBridge` refreshes it when a `direct-messages` push arrives in the
  foreground. A new DM with an unread message then gets a socket. A tapped push still routes
  through `ChatHub.select`.
- **R1.** A key advertisement (`e2ee-key`) is never acknowledged with `forkmesh-received`, so it
  stays on the relay for the other person's other devices.
- **R2.** The first sealed frame on a connection that has no wrap for this device's key makes the
  phone send one key advertisement with `persist: true`, whatever the daily throttle says. The
  advertisement sent on connect still persists at most once a day
  (`RoomSeal.shouldPersistAdvertisement`), as on the relay rooms.
- **R3.** Each durable send is kept in a per-DM outbox (64 entries, 7 days): the plaintext, the
  relay ref (first 40 hex of SHA-256 of the exact text sent) and the key ids it was wrapped for.
  Own frames in the history replay are added too, under the relay's `ref`. The outbox empties when
  the relay reports `stored: 0`. After `forkmesh-history-end`, and when a live frame teaches a new
  peer key, each entry not wrapped for every known peer is sealed again and sent with
  `persist: true`, its original `notify` and `resend: true`; the old refs then go out in
  `forkmesh-superseded` frames (64 refs each). `RoomSeal.peerKeyIds` lists the known peers for
  this check.
- **Dedupe.** The message id inside the plaintext does not change on a resend, so the receiver's
  existing per-room id set drops the second copy and does not ring or count it again.

Not verified: no Swift toolchain was available when this was written, so the change and its tests
(`DirectMessagesTests`) were checked by reading only, not compiled or run.

## Receipt marks, deletes, reactions and replies in direct messages (branch 1974)

Asked 2026-10-13: Signal-style marks on every message, "delete for everyone" for what you sent,
and replies and reactions on anyone's message in a direct message. The shared design and wire
rules are in the branch spec (`docs/app/chat-private-rooms.md` on the server side).

**Receipt marks.** `ChatReceipt` (FMApi `Models/Chat.swift`) is `sending`, `sent`, `delivered` or
`read`, with `label` ("Sending", ...) and a `rank` so a mark only moves forward. `ChatRow.receipt`
is set by `ChatHub.receipt(for:queued:)` for your own messages only: direct messages ask
`DirectMessageService.receipt(_)`, office rooms `OfficeChannelService.receipt(_)`, a queued
`#mesh` message is `sending`, and everything else (relay rooms, history) is `sent`.
`ChatConversationLayout.receipt(_)` gives every own bubble its mark, not only the newest one.
The app draws it with `ChatReceiptMark` (`ForkMesh/Features/Chat/Messages/ChatReceiptMark.swift`),
a `Canvas` in the shared 22 x 14 geometry scaled to 17 x 11: a dotted ring at 70 % for sending, a
ring with a tick for sent, two overlapping rings with ticks for delivered (the back ring and tick
are clipped outside the front ring), and two filled accent-blue discs with ticks cut out in the
background colour for read. VoiceOver reads the label. Bubbles and the Slack-style rows both show
it; office rooms keep their "to N" label beside it.

- Direct messages: a chat or thread reply is shown at once as `sending` and its relay ref (first
  40 hex of SHA-256 of the exact envelope) goes into a per-conversation `DirectRefBook` (256 refs,
  id to ref and back). `forkmesh-recipients` for that ref makes it `sent`;
  `{"kind":"forkmesh-delivered","refs":[...]}` makes those `delivered`; `{"kind":"forkmesh-read"}`
  turns every own message already `sent` or `delivered` into `read` (a message still `sending`
  stays). Own frames replayed from history or sent from another device of the same account are
  `sent` and their relay `ref` is tracked too. A send that cannot reach the relay takes the
  message back out (unless the mesh delivered it, which counts as `sent`). Marks live in
  `receipts` (512 ids) for the session; after a restart own messages show `sent` until the relay
  says more.
- Office rooms: `post` and `postFrame` show the message as `sending` before the socket write and
  `forkmesh-recipients` makes it `sent`. Channels never reach `delivered` or `read`: the relay
  sends those only for direct rooms.

**Delete for everyone.** `DirectMessageService.deleteForEveryone(_:messageId:)` sends a sealed
`delete` frame (`conversation`, `target`, `persist`, no `notify`), removes the message, drops it
from the outbox so a later reseal never sends it again, and then sends
`{"kind":"forkmesh-superseded","refs":[<original ref>]}` when the ref is known, so the relay drops
a copy the other person has not received yet. The delete frame itself stays on the relay for an
offline recipient. `OfficeChannelService.deleteForEveryone` does the same through `postFrame`.
Receiving: a `delete` is honoured only when its `senderId` is the target's `senderId`; a delete
whose target has not arrived yet is held (64 per room) and swallows the target when it comes, so a
history replay in any order ends with the message gone. Every removal calls `onRemoved`, and
`ChatHub.messageRemoved` records the id in `ChatExtras.recordRemoved` (2,000 ids, saved in the
chat history file as `removed`), drops its reactions, edit and attachment, removes the local ping
(`chat:<id>`) and calls `ChatHub.onMessageRemoved`; the app uses that to remove any delivered
notification whose `userInfo["messageId"]` is that id (`Platform/ChatNotificationCleanup.swift`).
`isRemoved` keeps a removed id from coming back from a replay after a restart.

**Reactions, edits and replies.** `ChatHub.toggleReaction`, `edit`, `delete`, `deleteReply` and
`replyInThread` send `reaction`, `edit`, `delete` and `thread-reply` frames through
`DirectMessageService.sendFrame(_:type:fields:)` (sealed like chat; `thread-reply` keeps
`channel` and notifies, the others carry `conversation` and do not) or
`OfficeChannelService.postFrame` for office rooms. Incoming room frames reach
`ChatExtras.applyRoomFrame`, which now applies replies (under the room's conversation, so threads
work for `direct:` and `#office/` rooms), reactions, edits (author-checked; an edit for a message
not seen yet is held until it arrives) and reply deletes. `ChatHub.rows` gives DM and office rows
reactions, edited text, thread summaries and edit/delete for your own messages; read-only office
rooms get none. The context menus offer Reply, the reactions, Copy, Edit and Delete (Delete for
everyone or Delete for me); a bubble with replies shows "N replies", which opens the thread.
`ChatThreadScreen` takes a `direct:` channel (the route no longer prefixes `#` to it).

Tests: `DirectMessageReceiptsTests` (sending to read, replayed own frames, failed sends, delete for
everyone with superseded, author check and delete-before-target, and a hub test covering reactions,
edits held for a late message, replies, deletes and the saved tombstones), the office test in
`OfficeChannelsTests`, and `ChatConversationTests` for the marks.

## Chat list last line, avatars and threads (branch 1974, round 2)

Asked 2026-10-13: "Make sure threads still work too also in the chat list show the last line per
channel and per message with users also show their user icon in the chat list".

**Last line.** `ChatHub.listPreview(_:)` (`ViewModels/ChatListPreview.swift`) walks
`messages(in:)` from the newest message back and returns the first one that is not hidden
(Delete for me), removed (Delete for everyone), a thread reply or an ephemeral app message, with
its timestamp. The text is `ChatExtras.text(for:)`, so an edit of the newest message updates the
line (the `#mesh` room has no edits and uses the plain text). `ChatListPreviews` makes the line:
`plain(_:)` drops quote markers, code fences and the bold, italic, strike and code markers
(through `ChatRichText.spans`), collapses newlines and runs of spaces, and cuts at 160 characters
with an ellipsis; `line(_:text:direct:)` gives "You: <text>" for your own message, "<text>" for
the other person's in a direct message (`direct:` or `@peer`) and "<sender>: <text>" in a channel;
a file with no text reads "📎 <file name>". A message whose line would be empty is skipped. The line
is built from what the phone already holds and decrypted; nothing new is stored or sent.
`timeLabel(_:now:timeZone:)` gives the short time on the name line: the clock time today, the
weekday ("Tue") within six days, "Oct 3" this year, "Mar 14, 2025" before. A conversation with no
message yet keeps an empty second line so every row has the same height.

**Rows.** `ChatChannelTile` takes `preview`, `time` and `avatar`. Channel, office, `#mesh`,
starred and direct-message tiles all pass a preview. Direct-message tiles (also when starred) show
`MessagesPersonAvatar` at 28 pt, the same loader and `ChatAvatarCache` the conversation header
uses, falling back to the initials avatar, with the presence dot (ringed in the row background)
over its bottom-right corner; channel icons sit in a 28 pt column so titles line up.

**Threads after round 1.** Checked and covered by tests: a peer's thread reply in a direct message
builds the thread, stays out of the conversation and the list line, and is not deletable; an own
reply replayed from another device is own, deletable and `sent`; a reply sent here goes `sending`
to `sent` on `forkmesh-recipients` and carries `notify`; DM replies are saved in the chat history
file under their `direct:` conversation. The same checks run for a public office channel (peer
reply, own reply with `rootId` and `#name` channel, ack, reply count). No regression was found.

Tests: `ChatListPreviewTests` (plain text, sender prefixes, file fallback, skipped messages and
edits, short times, and a relay channel where a thread reply, an edit, an own message and Delete
for me move the line), `DirectMessageReceiptsTests.testThreadRepliesFromEitherSideBuildTheDirectMessageThread`
(also the DM list line before and after Delete for everyone) and
`OfficeChannelsTests.testThreadRepliesInAnOfficeChannelGoBothWays`.

## Message text in push notifications (branch 1974)

Asked 2026-10-13: "make it so that chat text shows in the push notifications - signal has a way
of doing it securely so lets do it like they do". The push carries no text. A direct-message push
now carries `ref` (the relay ref of the frame that rang), `keyVersion` and `conversation`; a
channel push carries `channelId` and `ref`. `NotificationService` asks the relay for the
still-encrypted frame with the account's session (`GET /api/chat/direct-messages/<id>/pending?ref=`
or `/api/chat/channels/<id>/pending?ref=`; side-effect free on the server: no read mark, no
acknowledgement), opens it on the phone and shows the sender as the title and the text (or
"📎 <file>") as the body; a channel reads "<sender> in #<channel>". It sets `threadIdentifier` to
the conversation and `userInfo["messageId"]` to the message id. Anything that fails (row already
received, frame not sealed for this device, timeout) leaves the server's generic alert. The
extension never acknowledges, marks read or sends anything to the relay.

The logic is in a new module, **FMPush** (`ForkMeshCore/Sources/FMPush/PushChat.swift`, deps FMBase
and FMCrypto), tested on Linux by `FMPushTests/PushChatTests`:

- `PushChatRequest.parse(_)` reads `ref` (40 lowercase hex), `conversation` (`direct:<32 hex>`) or
  `channelId` (32 hex), `keyVersion` and `sender`; `pendingPath` and `threadIdentifier` follow.
- `PushChatOpener.open(_:request:privateKey:keys:)` checks the pending answer names the same
  conversation or channel, decrypts the outer `RoomCrypto` envelope, unseals it with the device's
  X25519 key (`RoomSeal.fromPrivateKey`, no store) and accepts only `chat` and `thread-reply` for
  that conversation. A direct message must be sealed; a public channel frame is plain inside the
  room cipher.
- `PushRoomKeys` caches derived room keys (one keychain item, `room-keys`, 32 entries, keyed by a
  hash of room and passphrase) so only the first push of a room pays the 210,000 PBKDF2 rounds.
  `RoomCrypto.derivedKey(_:passphrase:)` and `RoomCrypto.withKey(_)` were added for it.
- `PushPreviewMode` is the "Notification content" setting (Settings, Notifications): Name and
  message (default), Name only ("New message" under the name, no fetch), No name or message
  (title "ForkMesh", body "New message", no portrait). `PushPreviewText.preview` builds the text.

What the extension needs comes through the shared keychain (`Shared/PushCredentialStore.swift`):
the credential JSON written at registration now also holds `deviceKeyName` (the full keychain
account of `chat/e2ee/x25519PrivateV2` in the active project's namespace,
`ProjectKeyPolicy.secureKey`) and `preview`. `PushCredentialStore.deviceKey(named:)` reads that
key from the app's `com.forkmesh.forkmesh.secure` items (no access group given, so every group the
extension holds is searched; the app writes without one, which lands in its first
`keychain-access-groups` entry, the group the two targets share). `setPreview` rewrites the
preference into every stored credential when the setting changes. The extension links the new
`ForkMeshPushCore` product (FMBase, FMCrypto, FMPush), not the whole core.

## Voice and video calls

Requested 2026-10-12: call buttons in one-to-one chats on the phone, plus the five video-call
faces the web has (docs/app/chat-direct-calls.md). `DirectCallEngine` (`Direct/DirectCalls.swift`)
held the whole signaling state machine but nothing created it. `ChatInstaller.installCalls` now
builds it next to `DirectMessageService`, so `hub.calls` is set and `Services` has it:

- signaling goes out through `direct.sendCallFrame` and comes in through `direct.onCallFrame`,
  so it rides the DM room's sealed socket exactly like the web's frames;
- `partnerFor` is the DM record's `otherUser`, and `onMissed` posts the same
  "📞 Missed audio/video call" line the web posts;
- `peerFactory` is `WebCallPeerFactory` (app, `Features/Chat/Calls/WebCallPeer.swift`), which
  loads `direct-call.html` into a `WKWebView` (`loadHTMLString` with an `https://` base URL, so
  the page is a secure context and `getUserMedia` works) and drives it through the `fmCall`
  message handler. The WebView grants camera and microphone only to that origin; iOS still
  asks the user first, through `AVCaptureDevice.requestAccess`, before a call starts or is
  answered.

The face is engine state: `DirectCallEngine.faces` (Maya, Theo, Kofi, Ingrid, Ren),
`setFace(_:)`, and `onFaceChanged`, which the installer saves under `chat.callFace` and restores
at launch. A video peer gets the face as soon as it is attached, before it offers or answers,
and `setFace` mid-call forwards it. `DirectCallPeer.setFace` has a default empty body, so other
peers need not implement it. `direct-call.html` carries the web's four face modules and the
five face photos inlined (`app/tools/sync_native_call_faces.py` writes both the iOS and the
Android copy; `app/tests/test_chat_call_faces.py` fails when they drift). Its `face` operation
swaps the outgoing video with `RTCRtpSender.replaceTrack`, as the web does.

The voice is engine state too: `voiceChoice` ("face", "off" or a face id), saved under
`chat.callVoice`, and `effectiveVoice`, which `WebCallPeerFactory.voice` passes to the page's
`setup`. `DirectCallPeer.setVoice` and `observeVoice` have default empty bodies; `WebCallPeer`
forwards the page's `{event: "voice"}` so `engine.voice` is the voice actually applied. Audio is
processed inside the page (an AudioWorklet), not with AVAudioEngine; see
docs/app/chat-direct-calls.md, "Voices". `CallAudioRoute.headphonesConnected` and
`WebCallPeer.preview` / `setMonitor` are there for the pre-call preview.

The background is engine state as well: `DirectCallEngine.backgrounds` (No background, Blur,
Strong blur, five images, Your photo), `setBackground(_:image:)` and `onBackgroundChanged`. The
installer saves the id under `chat.callBackground` and the picked photo as
`Application Support/ForkMesh/call-background.jpg` (`CallBackgroundStore`). A video peer gets it
before it offers or answers, like the face. `DirectCallPreview.swift` holds the pre-call
preview (the eye button in a DM header): `WebCallPeerFactory.preview(face:background:image:)`
loads the page in preview mode, and `makePeer` reuses that page for the call, so the camera and
the disguise keep running. `CallFaceLandmarks` also answers person masks: the page asks for
`{landmarks, mask}`, and Vision's `VNGeneratePersonSegmentationRequest` (`.balanced`, 8-bit)
runs in the same request; the mask goes back as 256-px-wide base64 bytes. See
docs/app/chat-direct-calls.md, "Backgrounds" and "Models on the device".

The face is a real photo warped onto the caller's face over a 68-point mesh
(docs/app/chat-direct-calls.md, "Face swap"). On iOS the landmarks come from Apple Vision:
the page posts `{event: "landmarks", id, image}` with a 480-px JPEG of the frame,
`WebCallPeer` hands it to `CallFaceLandmarks.detect` (`Features/Chat/Calls/CallFaceLandmarks.swift`,
`VNDetectFaceLandmarksRequest` with the 76-point constellation, on its own queue), and the
regions go back in image pixels, top-left origin, through `window.fmCallLandmarks(id, regions)`.
The page turns them into the 68 points itself, so a different Vision constellation still works.
If three requests fail, the page falls back to its own tracker.

UI (`Features/Chat/Calls/DirectCallViews.swift`): video and phone buttons in the iMessage-style
header of a `direct:` conversation (`DirectCallHeaderButtons`), an incoming-call banner and a
full-screen call screen (`.directCalls(hub)` on the chat screen). The call screen has mute,
camera, Face (a menu: "Your own face" plus the five) and hang up.

Limits: ringing reaches the phone only while the DM's room socket is open (the app in the
foreground, with that DM among the watched rooms). There is no CallKit or VoIP push. The app
target cannot be compiled on Linux: `swiftc -parse` passes for the new files, but they have not
been type-checked or run on a device. That includes `CallFaceLandmarks.swift`: the Vision path
was tested in Chromium with a stand-in host that returns Vision-shaped regions, not with Vision.

### Preview stuck on the spinner; recording from the preview (2026-10-14)

Asked: "record a message ahead of time and send it as an attachment they can watch and that is
removed after watching", and "the preview only shows a spinning indicator, no video or sound".

**Spinner.** `DirectCallPreviewScreen` put the WebView on screen only after
`factory.preview(...)` returned, but that call waits for the page's `getUserMedia`. WebKit
(`getUserMediaRequiresFocus`, on by default) holds a capture request until the document is
visible, and a `WKWebView` outside a window never is, so the camera never started and the
spinner stayed for ever. The call screen did not have the bug because it shows
`factory.current` as soon as the peer exists. The preview now does the same: it shows
`peer ?? factory.current` from the start, with the spinner over it until the page answers. A
30 s watchdog ends a preview that still has not started, and the failure text has "Try again".

**Record message.** In a DM (`hub.canSendViewOnce`) the preview has "Record message" beside
"Start video call" (Close moved to an X at the top left to make room). It records the page's
outgoing tracks, so the message is exactly the previewed picture and voice: face, background
and changed voice included. The page operations (`direct-call.html`, both apps):

| Op | Does |
| --- | --- |
| `record(face, background, voice)` | Only while previewing (`not_previewing` otherwise or after `setup`). Refuses at once when the page is not showing what the app asked for (`face_unavailable`, `background_unavailable`, `voice_unavailable`), then waits up to 20 s for `stage.ready()` and the voice stage (`face_not_ready`). Starts `MediaRecorder` on the stage track and the voice track: MP4 (H.264/AAC) where supported, else WebM; 900 kbit/s video, 64 kbit/s audio |
| `recordStop()` | `{mime, durationMs, size}`; keeps the bytes in the page |
| `recordChunk(offset)` | Base64 of up to 768 KiB from `offset` |
| `recordDrop()` | Zeroes and forgets the bytes; `close` and `setup` do it too |

`WebCallPeer.startRecording` / `finishRecording` / `dropRecording` drive them;
`finishRecording` reads every chunk, checks the total, drops the page copy and returns
`WebCallRecording` (MIME without parameters). The screen stops at 60 s with a countdown, turns
"Hear myself" off, writes a protected temporary file (`MessagesViewOnceFiles`) and shows the
review (`MessagesViewOnceReviewVideo`, routed to the speaker without headphones): Discard,
Re-record, Send. Send is `hub.sendViewOnce(kind: .video, ...)`, the same encrypted view-once
message as the composer's, and closes the preview; the file is deleted on send, discard or
close. If a face, background or voice cannot start, nothing is recorded and the screen says
why, rather than falling back to the raw camera or microphone.

Tests: `app/browser_tests/tests/native-call-record.spec.js` loads the iOS `direct-call.html` in
Chromium with a fake camera and microphone: preview with a changed voice, refusals, a 2.5 s
recording read back in chunks that decodes as video, the copy dropped; a background preview
whose recording waits for the stage; no recording before preview or after `setup`. The Swift
files pass `swiftc -parse` only (the app target does not build on Linux); not yet run on a
device.

## View-once video and voice messages

Requested 2026-10-13 (task 1998): record a video or voice message in a direct message and have it
deleted once the other person has played it. The wire format, server routes and deletion rules are
in `docs/app/chat-private-rooms.md` ("View-once video and voice messages"); this is the iOS side.

ForkMeshCore (`Direct/ViewOnceMedia.swift`, Linux-tested):

- `ViewOnceFrames` parses and builds the sealed frame's `viewOnce` field (version 1, `video` or
  `voice`, a 32-hex media id, a 32-byte key and 12-byte IV in base64url with or without padding, a
  MIME type under `video/` or `audio/`, `durationMs` within the server limit, `size` within the
  kind's limit plus 1 KiB, `parts = ceil(size / 524288)`). Anything else is not a view-once
  message, and the row falls back to the frame's text. It also reads the outer
  `{"kind":"forkmesh-media","v":1,"id","state"}` relay frame (only `opened`, `expired`,
  `withdrawn`) and the `GET media` status objects.
- `ViewOnceCrypto` seals with `AesGcm` (CryptoKit on Apple, the portable path on Linux): a fresh
  random key and IV per message, body = ciphertext followed by the 16-byte tag, as WebCrypto
  produces it. `split` cuts the body into 512 KiB parts.
- `ViewOnceTransfer` drives the routes through `ChatAccountCalling` (the app's `ApiService`):
  create, `PUT` each part as base64 JSON, `finish` (a failed upload is withdrawn with `DELETE`);
  download every part, check index, count and total size, then decrypt; `consume`; `withdraw`;
  `GET media/<id>` and `GET media?ids=` in batches of 50. `media_gone` (410) becomes
  `ViewOnceError.gone`; the service then reads that media's status to tell opened, expired and
  withdrawn apart.
- `ViewOnceStore` holds one `ViewOnceEntry` per message: kind, MIME type, duration, whether it is
  ours, state, and the key material for a received message that is still `ready`. The key is
  dropped the moment the state turns final (opened, expired, withdrawn, failed), and the sender's
  entry never holds it. Final media ids (never keys) are remembered in an optional
  `KeyValueStore` under `chat/viewOnce/spent` (512 at most), so a history replay after a restart
  does not pick the key up again.
- `DirectMessageService`: a received `chat` frame with a valid `viewOnce` registers an entry and
  becomes a `ChatMessage` whose text is "🎥 Video message" / "🎤 Voice message" (the frame's own
  `text` and any `fileName` are ignored). The relay frame updates the state. After the first
  `forkmesh-history-end` on each socket the service reads `GET media?ids=` once for the
  conversation's unresolved entries and retries any `consume` that failed offline.
  `sendViewOnce` checks size and length, encrypts, uploads, then sends the sealed frame
  (`persist`, `notify`); if the frame cannot be sent, the upload is withdrawn. `playViewOnce`
  downloads and decrypts in memory; `consumeViewOnce` marks it opened locally (dropping the key) and
  calls `consume`. The outbox entry that would reseal the frame for a late device is removed once
  the message is final, so the key does not linger there either. DM history is not written to the
  chat history file (`ChatHistoryCodec.bounded` keeps only `#` and `@` rooms), so the key is never
  on disk.
- `ChatHub.canSendViewOnce`, `viewOnce(for:)` and `sendViewOnce(...)` (`ViewModels/ChatViewOnce.swift`).

Tests: `Tests/FMChatTests/ViewOnceMediaTests.swift` (round trip and tamper, the WebCrypto layout,
part counts, the frame codec and its refusals, text ignored, relay frame parsing, key dropped and
spent ids remembered, upload/download/consume against a fake media server, failed upload withdrawn,
quota message, the full send and receive flows over a fake socket, 410 resolved to the server's
state, the status read once per socket and a consume retried).

App (`Features/Chat/Messages/`):

- `MessagesViewOnceCapture.swift`: the composer's More menu offers "Video Message (Plays Once)"
  and "Voice Message (Plays Once)" in a one-to-one DM (renamed from "View-once Video/Voice" on
  2026-10-14: the user did not recognise the old names as the record-ahead feature). The call
  preview's "Record message" is the other way in (below). Video uses `AVCaptureSession` with the front camera and
  microphone, a live preview, and `AVAssetWriter` to H.264 480x640 at about 1 Mbit/s with AAC
  64 kbit/s mono, so 60 s is about 8 MB, well under the 12 MiB limit. Recording stops by itself at
  60 s, with a countdown. Voice uses `AVAudioRecorder` (AAC, 22.05 kHz, 32 kbit/s) for up to 120 s
  with a live meter. Both end in a review step: play back, Discard, Re-record, Send (with upload
  progress). Recordings are temporary files in `NSTemporaryDirectory()` with
  `FileProtectionType.complete`, deleted on send, discard or close.
- `MessagesViewOnceViewer.swift`: the bubble shows "Video message" / "Voice message", the duration
  and the state ("Tap to play once", "Sent", "Opened", "Expired", "Withdrawn"). Only a received,
  ready message is a button; these rows have no context menu, so no copy, save, share or forward.
  The viewer downloads and decrypts, writes a protected temporary file, starts playback
  (`AVPlayerLayer` for video, so there are no system share controls; `AVAudioPlayer` with a level
  meter and progress for voice), and calls `consume` as playback starts. When playback ends or the
  viewer closes, the player and file are dropped. Closing before playback starts leaves the
  message unopened. A MIME type iOS cannot play (`audio/webm`, `video/webm` from a browser) is
  reported and not consumed.
- Screen capture: while `UIScreen.isCaptured` is true the video is blanked and a notice says so;
  after `userDidTakeScreenshotNotification` the viewer says a screenshot was taken and that
  ForkMesh cannot prevent screenshots or recordings. Nothing is sent to the sender.
- `Config/ForkMesh-Info.plist`: the camera and microphone strings now mention messages.

Filter seam: `MessageCaptureFilters` (in `MessagesViewOnceCapture.swift`) has `processVideo`
(per `CVPixelBuffer`, before the writer's pixel buffer adaptor) and `processAudio` (per
`CMSampleBuffer`) hooks on the video recorder. Only `MessagePassThroughFilters` exists today; the
call face and voice stages (`createFaceStage` / `createVoiceStage`) run in the call WebView and
are not wired in. Voice messages go through `AVAudioRecorder`, which has no sample hook, so a
voice filter would need an `AVAudioEngine` recorder first.

Held receipts: the phone keeps no DM history on disk, so acknowledging a view-once frame
(`forkmesh-received`) would let the relay delete the only copy of an unopened message. A sealed
chat frame with a valid `viewOnce` from the other account is therefore not acknowledged, live or
replayed, while the message is unopened or its `consume` is still owed. Its refs are kept per
message id (`heldReceipts`, at most 800) and sent once the message is final: `consume` succeeded
or answered 410, or the relay frame or the status read said opened, expired or withdrawn. With no
socket open at that moment the refs stay held; a later replay of a final message is acknowledged at
once. A `consume` that failed offline is remembered under `chat/viewOnce/consume` and retried when
the conversation's status is next read. Same rule as web (`holdReceipt`) and Android.

Unsend: the sender's unopened row has an "Unsend" button (with a confirmation) that calls
`ChatHub.unsendViewOnce`, which withdraws the media (`DELETE`); the row then says "Withdrawn".

Limits: desktop and web clients that do not know `viewOnce` show only the fallback text.

Verified: ForkMeshCore tests on Linux (`swift test`, the new suite and the whole package). The
held receipts and Unsend came later and were checked with the FMChat tests and `swiftc -parse`
only. The rest of the app target was compiled on the office Mac (Xcode 26.6, Debug, iOS Simulator x86_64, no signing,
in a scratch folder) with no errors or warnings in the new files. Nothing was run on a device:
capture, playback and the screen-capture notices are untested on hardware.

## Link previews (branch 1996)

Requested 2026-10-14: Slack-style cards for links in chat, for 50 kinds of
popular site and for any site's own social image.

- **Catalog** (`Threads/ChatLinkSites.swift`). `ChatLinkSites.all` has 64
  sites in 14 kinds (`ChatLinkKind`: video, music, social, code, package,
  document, article, shop, app, map, image, chat, reference, page). Each entry
  has hosts (a suffix match after `www.`/`m.`/`mobile.`/`old.`/`new.`, or
  `host/path` such as `google.com/maps`), a brand colour and SF Symbol for the
  badge, a kind and a resolver. A test keeps the catalog at 50 or more.
- **Resolvers** (`ChatLinkPreviewService.special`). Sites with a public API
  are asked through it, not scraped: YouTube (oEmbed for the title and
  channel, `i.ytimg.com/vi/<id>/hqdefault.jpg`; watch, youtu.be, shorts, live
  and embed links), oEmbed for Vimeo, TikTok, Dailymotion, Loom, Spotify,
  SoundCloud, CodePen and Flickr, X/Twitter (`publish.twitter.com/oembed`, the
  post's text), Wikipedia (REST summary), Hacker News (Firebase item), npm,
  PyPI, crates.io, the App Store (iTunes lookup), Steam (appdetails), xkcd
  (`info.0.json`) and Stack Exchange (api.stackexchange.com). Everything else,
  and any API that fails, falls back to the page: `og:*`, `twitter:*`,
  `<title>`, `description`, `author`, `link rel=image_src`, read anywhere in the
  first 2 MB (Pinterest and Google Play put theirs in the body). A link to an
  image file is shown as the image. Known sites always show the catalog's
  name; unknown ones show `og:site_name` or the host.
- **Fetching.** Through its own `URLSessionHTTPClient` (ephemeral: no cookies,
  no cache, not the telemetry-wrapped client), 8 s per request, redirects
  followed by hand (at most 4) so that every hop is checked. Only `http(s)`
  on ports 80/443 without user info, to a public name: loopback, private,
  link-local, CGNAT, multicast and documentation addresses (v4, v6 and
  v4-mapped), numeric or hex-dotted hosts, single-label names and `.local`,
  `.lan`, `.home.arpa`, `.internal` and similar are refused, the same check
  for image URLs. A host name that resolves to a private address is not
  caught (the phone does not resolve before connecting). The first try says
  `ForkMeshBot … facebookexternalhit/1.1 Twitterbot/1.0`, because X, Instagram,
  Facebook, TikTok and Threads only give crawlers their tags; a 401/403/429/503
  or a page with no picture or description is retried once as iPhone Safari
  (forkmesh.com's WAF refuses "bot" agents). Pages are cut at 2 MB, images
  refused over 6 MB and decoded off the main thread to at most 1,200 px.
  Linux's URLSession reports Twitch's `103 Early Hints` as the status, so a
  1xx with a body is read as the page.
- **Cache.** `ChatLinkPreviewService` keeps up to 400 previews in memory and
  in `Caches/chat-link-previews.json` (3 days); a miss is not retried for 6
  hours in a launch; image bytes are kept to 48 MB, decoded images in an
  `NSCache` of 120.
- **Switch.** `chat/link-previews/enabled` (on by default), in a channel's
  Settings & Details. Previews are fetched by each reader's phone, so the site
  learns that someone opened the chat; turning the switch off stops every
  fetch.
- **Live check, 2026-10-14** (a throwaway probe against the real sites from
  the Linux box, not part of the suite): 62 of 68 links gave a card, including
  the YouTube link of the request ("- Spotlight - Project 40 - - CVE
  Lite CLI", Vandana Verma, the thumbnail). Medium, Etsy, Reuters and eBay
  refused this box (Cloudflare, DataDome) with either agent; a private Google
  Doc and a Bitbucket single-page app have no tags. Those links stay plain.

## Channel details, members and smart mentions (branch 1996)

- `ViewModels/ChatChannelInfo.swift`: `ChatChannelMember`,
  `ChatHub.channelMembers` / `memberCount` / `onlineMemberCount` (see
  docs/native/NAVIGATION.md 2.0.97 for who counts), `channelFiles` (files and
  links in loaded history, newest first), `savedItems(in:)`, `bookmarkCount`,
  `channelTabCount`, `channelDetail`, `addBookmark`, `saveCanvas`,
  `notifyLevel` / `setNotifyLevel`, `unreadCount`, `unreadMessages`,
  `hasUnreadMention`, `firstUnreadId`, `broadcastWarning`,
  `broadcastSuggestions`, `broadcastAudience`, and
  `ChatLinkPreviewParsing.links(in:)` (http(s) links outside code fences,
  sentence punctuation and unbalanced brackets trimmed, Slack's `<url|label>`).
- `ChatWorkspace` gains `ChatBookmark` (50 per channel, http(s) only),
  `ChatCanvas` (20,000 characters) and `ChatNotifyLevel`; `forget` clears them
  when a channel is left.
- `ChatSmartMentions`: `split` turns text into plain, person, you and
  broadcast pieces (an `@` after a space or opening bracket, outside
  backticks; `team@here.com` and `@hereafter` are not tags), `broadcasts`,
  `strongest` (everyone, then channel, then here), `mentionsYou`.
  `ChatRow.mentionsYou` is set in `rows`. Warnings start at
  `ChatSmartMentions.warnAt` (6) people other than you.
- `ChatHub.unreadStart`: the conversation that was opened and the id of its
  first unread message, for the NEW line; it is replaced on the next `select`.

## Views (ForkMesh/Features/Chat)

`ChatScreen(showComposer:)`, `RelaySettingsSection()`, `RelayNetworksScreen()`,
`LocalDesktopScreen()` (+ `LocalAgentScreen(sessionId:)`, `LocalDesktopChip()`),
`ComposeIdentityBar(verb:)`, `RelaySwitchScope`, `ChatFeature`, `ChatNewMessageSheet`,
`ChatShareMessageSheet`, `ChatSearchSheet`, `ChatChannelSheet` and its pages
(`ChatChannelPages.swift`: files and links, bookmarks, canvas, members, agents
and apps, notifications, settings), `ChatMoveChannelSheet` (Channels/),
`ChatLinkPreviews` / `ChatLinkPreviewCard` (Threads/). Conversations are drawn by
`Messages/`: `MessagesConversationView` (header with the avatar and name that open the details,
transcript over the shared background, double-tap tapback picker, composer with photos, camera,
files, background, emoji and a voice-message recorder), `MessagesBubbles.swift` (tailed bubble
shape, iMessage colours, tapback badge, typing bubble, system lines), `MessagesMedia.swift` (JPEG
re-encoding, backdrop, camera, photo viewer, voice recorder and player, background picker) and
`MessagesDetailsScreen`.

## Not verified

The BSD sockets are exercised on Linux loopback only; on an iPhone the Local Network prompt,
multicast refusal (EHOSTUNREACH) and Wi-Fi sweeps have not run. The SwiftUI views have been
syntax-checked, not compiled.

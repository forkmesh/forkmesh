# #mesh push: waking phones that left the local room

`#mesh` is the LAN and Bluetooth room (`desktop/src/LocalMeshNode.*`, iOS
`FMChat/Mesh/LocalMeshService.swift`, Android `chat/LocalMeshRoom.kt`). Its
frames go device to device and never reach the server, and iOS suspends an
app's sockets soon after it goes to the background. Two mechanisms keep a
phone that is not watching the room informed: server wake pushes (iOS) and
local alerts raised by the phone itself.

## How it works

1. **Minting.** A signed-in iPhone asks the server for a wake token
   (`POST /api/push/mesh-wake`) when the room starts and whenever the app comes
   back to the foreground. It asks again once fewer than 24 hours are left.
2. **Advertising.** The phone stamps the token as `wake` on its `hello`,
   `chat` and `wake` frames. A `wake` frame is sent once, when a token arrives
   after links are already open. Chat frames are kept in every device's room
   history and replayed to joiners, so the token stays with the room after the
   phone leaves. Old clients ignore the field and the extra frame type.
3. **Remembering.** Every client (iOS, Android, desktop) keeps the newest
   token per sender id. Tokens come from live frames and from the history file
   on start. Up to 32 tokens are kept, and leaving the room does not remove a
   sender's token. That is the point: the phone that left is the one to wake.
4. **Waking.** A device that sends a `#mesh` chat posts every usable token it
   knows, except its own, to `POST /api/push/mesh`:
   - phones use their session;
   - the desktop uses a signed `user`/`ts`/`sig` over
     `forkmesh-mesh-wake-v1\n<user>\n<token>…\n<ts>`
     (`MainWindow::wakeLocalMeshPhones`, `signOrgTaskProof`).

   The server checks each token, drops the sender's own account and coalesces
   each recipient to one push per 5 s. It then sends `New message in #mesh` on
   the `chat` APNs topic to the token owner's phones. The message text never
   leaves the LAN.

## Local alerts over the mesh

The wake push needs an account, the internet, and a sender that knew the
phone's token. Phones also raise a notification themselves for every `#mesh`
message that arrives over the mesh while the room is not on screen. Nothing
leaves the devices and no account is needed.

- **The rule** (`meshAlert`, Android `chat/MeshAlert.kt`, iOS
  `FMChat/Mesh/MeshAlert.swift`): alert for a message from someone else that
  was added to the room just now. Skip it while the user is watching `#mesh`
  (app in the foreground, Chat on screen, `#mesh` open). Skip it if it is more
  than 10 minutes old, so a history backfill from a peer does not ring for old
  messages. The title is `<sender> in #mesh`, plus `· N new` once more than one
  is unread. The body is the text, cut to 180 characters.
- **One notification.** Each alert replaces the last. Opening `#mesh` marks
  the room read, which removes it. On iOS it also removes any wake pushes
  ("New message in #mesh") still in Notification Center, and posting a local
  alert removes the generic wake banners it supersedes.
- **Read only while visible.** `#mesh` is marked read only while the app is in
  the foreground, otherwise each alert would be cleared as it is posted.
- **The hook.** `LocalMeshRoom.onIncoming` / `LocalMeshService.onIncoming`
  fire once per new chat frame from another device, with the unread count.
  They do not fire for the device's own messages or for history loaded from
  disk.
- **Tap.** Opens Chat on `#mesh`. Android uses href `/chat/mesh`
  (`PushFeature.openMesh`). iOS sets `topic: chat`, `conversation: #mesh` and
  `meshLocal`. `ApplePushBridge.willPresent` still hides room-chat pushes in
  the foreground but shows `meshLocal` ones: they are only posted when the
  room is not on screen.
- **Permission.** Both apps ask for notification permission once, when the
  first nearby device links, so a phone that never signed in still gets
  alerts (Android `PushFeature.PermissionPrompt`, iOS
  `MeshAlerts.askOnce`).

### Staying in the room with the app closed

- **Android.** The background connection service also runs in a
  mesh-only mode when no project is signed in ([ANDROID.md](ANDROID.md)). The
  process stays alive, so the room keeps its LAN and Bluetooth links. Its
  notification channel is "Nearby #mesh messages" (high importance).
- **iOS.** `UIBackgroundModes` includes `bluetooth-central` and
  `bluetooth-peripheral`. The Bluetooth links (`CoreBluetoothMeshTransport`,
  which already scans for its service UUID, as background scanning requires)
  keep running and wake the app for each frame, so a backgrounded iPhone
  hears `#mesh` over Bluetooth. LAN sockets are still suspended in the
  background; for those, the wake push above remains the path.

## The token

`mw1.<account blind index>.<expires ms>.<hmac>`. The HMAC key is the data
secret plus `:mesh-wake-token-v1`. Tokens last 7 days.

- **What it reveals.** It names the account only by its blind index, so a LAN
  peer learns nothing it could not already see in the room.
- **What holding one allows.** It lets an authenticated account buzz that
  phone with a fixed banner, and nothing else. A peer cannot mint a token for
  someone else, and the sender must be signed in.
- **Limits.** There is no revocation. It is bounded by the 7-day expiry and
  the 5 s coalescing.

The `chat` topic keeps the existing iOS behaviour: the banner is suppressed
while the app is in the foreground (`ApplePushBridge.willPresent`), and a tap
opens Chat on `#mesh` (the payload's `conversation`, see `docs/app/README.md`). Ping mutes and quiet hours do not apply, the same as `#general`
and channel pushes.

## Limits

- Only iOS receives wake pushes. Android has no APNs; Android and the desktop
  only send wakes and never advertise a token. Android gets its `#mesh`
  messages from the local alerts above, as long as its background service
  runs.
- An iPhone that is backgrounded but still linked over Bluetooth can get both
  a local alert and a wake push. The local alert removes wake banners already
  delivered, but a wake that lands afterwards still shows.
- iOS stops background Bluetooth work after a force quit, and a suspended
  app's LAN links drop. Neither can be helped from the app.
- Waking needs the sender to reach the internet. A `#mesh` message sent with
  the internet down stays local, and nothing retries the wake later.
- A sender that never heard the phone, either live or through room history,
  cannot wake it.

## Code map

| Piece | Where |
| --- | --- |
| Routes, token, coalescing | `app/src/mesh_push.py`, wired in `app/src/app_runtime.py` and `app/src/entry.py`, listed in `world/tools/build_worker_footprint.py`; tests in `app/tests/test_mesh_push.py` |
| iOS | `LocalMeshService` (`wakeTokenProvider`, `wakeSender`, `wakeTargets`), `MeshWire.swift` (`meshWakeTokenExpiry`), wired in `ForkMesh/Features/Chat/ChatFeature.swift`; tests in `LocalMeshTests.swift` |
| Android | `LocalMeshRoom.wakeSender` / `wakeTargets`, `meshWakeTokenExpiry`, wired in `feature/chat/ChatFeature.kt`; tests in `LocalMeshQueueTest.kt` |
| Local alerts | `meshAlert` (`MeshAlert.kt`, `MeshAlert.swift`; tests `MeshAlertTest.kt`, `MeshAlertTests.swift`), `onIncoming` (tests in `LocalMeshQueueTest.kt`, `LocalMeshTests.swift`), Android `ChatFeature.alertMesh` + `PingNotifier.postMesh`, iOS `ForkMesh/Features/Chat/MeshAlerts.swift` |
| Desktop | `LocalMeshNode::wakeRequested` / `wakeTargets`, `forkmesh::localmesh::wakeTokenExpiry`, `MainWindow::wakeLocalMeshPhones` (`MainWindowPingForward.cpp`); test `testLocalMeshWakesDevicesThatLeft` in `tests/test_chat_transport.cpp` |

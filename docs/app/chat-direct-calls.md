# One-to-one calls in direct messages

Part of [the App docs](README.md). End-to-end encrypted peer-to-peer audio
and video calls in direct messages on `/chat` and in the phone apps:
signaling, resilience, rings and pushes, the pre-call check and the live
quality widget. Direct messages themselves are in
[chat-private-rooms.md](chat-private-rooms.md); group calls in
[chat-group-calls.md](chat-group-calls.md); face, background and voice
filters in [call-filters.md](call-filters.md); how chat is encrypted in
[encryption](../encryption.md).

## Signaling and media

`app/public/chat-calls.js` adds one-to-one calls to DMs on `/chat`.

- **Signaling** rides the DM room socket as non-persisted, sealed frames:
  `call-invite`, `call-accept`, `call-decline`, `call-offer`, `call-answer`,
  `call-ice`, `call-bye`. The relay sees only ciphertext and stores nothing.
- **Media** is WebRTC (DTLS-SRTP) directly between peers, with a public STUN
  server and no TURN, so two peers behind symmetric NAT cannot connect. Once
  connected, the call reads the selected ICE candidate pair from `getStats()`
  every 4 s and shows **Direct** (host, srflx or prflx candidates) or
  **Relayed** (a `relay` candidate: a TURN server, a third party, carries the
  encrypted media). With STUN only a call is direct or fails; the label makes
  a future TURN server visible. The state line says "Connected · end-to-end
  encrypted" and does not claim peer-to-peer on its own.
- **Flow**: fixed transceiver order (audio, video), the caller offers only
  after accept, trickle ICE with a pre-answer queue, `replaceTrack` instead of
  renegotiation (an ICE restart is the only renegotiation). Frames carry `to`
  so multi-device accounts converge on one answering device.

## Resilience

Each signaling frame is sent once over the relay, so one lost frame would
stall a call and a network blip would end it. `chat-calls.js` follows a shared
resilience spec, also used by the iPhone engine, with no new frame types (an
old client that ignores the extra frames still works):

- *Accept resend.* After `call-accept` the callee resends it every 2 s until
  the caller's `call-offer` arrives or the call ends, at most 8 times.
- *Duplicate accept.* The caller answers a repeated accept from the device it
  picked with its current offer (same SDP) while that offer is unanswered, so
  a lost offer is sent again; an accept from another device, or before the
  offer exists, is ignored.
- *Offer resend.* An unanswered offer is resent every 3 s, at most 5 times.
- *Duplicate offer.* The callee remembers the last offer it answered and its
  answer; the same SDP gets the stored answer back, a different SDP from the
  same device is an ICE restart and is applied and answered. Offers are
  handled one at a time, and ICE candidates that arrive while an offer is
  being answered (or, on the caller, while its offer is unanswered) wait in
  the queue, so a restart's candidates are not applied to the old description.
- *Duplicate answer.* Ignored when the caller has no outstanding offer, the
  signaling state is stable, or it is the answer already applied (a duplicate
  would throw in `setRemoteDescription`).
- *Connect timeout.* An accepted call not live 30 s later ends with
  `call-bye` and "Could not connect · The call could not connect. Try again."
- *Reconnect.* `failed` and `disconnected` do not end the call. The caller
  drives an ICE restart (`restartIce()` and a fresh offer, with the resends
  above) when the connection fails or stays disconnected for 2 s, and retries
  every 6 s while it is down; the callee shows "Reconnecting…" and answers the
  restart offer. Either side ends with `call-bye` and "Call lost" after 30 s
  without recovery.
- *Media watchdog.* Once a second while live, the call sums `inbound-rtp`
  audio `packetsReceived`. No growth for 4 s while the connection says
  connected is a drop (the "audio went silent" case): the caller restarts
  ICE, the callee shows "Reconnecting…", and the drop ends when packets flow.
  Opus runs without DTX, so a muted peer still sends packets.
- *Microphone recovery.* When the outgoing microphone track ends (device
  unplugged, OS interruption, permission revoked) the call reopens it with
  the same constraints, swaps it in with `replaceTrack` (rebuilding the voice
  stage when a voice is on), keeps the mute state and says "Microphone
  reconnected"; on failure the state line says to check the microphone for
  five seconds.
- *Invites while answering.* A resent invite only refreshes the 9 s stale
  timer and never replaces the pending invite, so Accept still works while a
  camera permission prompt is open; an invite stays up while an accept is in
  progress, and if the caller hangs up meanwhile, or the camera cannot open,
  the panel says so. An invite that arrives during your own pre-call check
  rings instead of answering "busy".
- *Declines say why.* The caller sees "Call declined" or "Busy" with the
  reason (declined, on another call, camera or microphone unavailable); these
  end states close after 8 s or on Close.
- *Rings reach a web callee in another conversation.* `/chat` keeps one room
  socket, the open conversation's. The `calls` account event
  (`{topic: "calls", conversation: "direct:<id>"}`) opens that conversation
  (`openRingingConversation` in `chat.js`) when no call or pre-call check is
  running; the next resent invite then shows the incoming call. Switching
  conversations during a call stops its signaling; the mini window keeps you
  in the conversation instead.
- Every timer goes through the injected `setTimeoutImpl` / `setIntervalImpl`
  and a per-call registry that teardown clears;
  `app/tests/test_chat_call_resilience.py` drives each rule with a fake clock
  and peer connection.

## Rings and pushes

A ring must reach the callee on any screen or device, not only one with that
DM open. Every client marks the `call-invite` envelope with a plaintext
`ring: true` (never persisted; the invite stays sealed). The relay
(`ForkMeshRoom._ring_direct` in `entry.py`) answers a live `ring` frame in a
direct room with `notify_account_event(<other participant>, "calls", …)`, at
most once every 20 s per conversation and caller
(`CHAT_DIRECT_RING_EVERY_MS`), so the invite resent every 3 s rings twice over
a 45 s ring. A new call id rings at once; each conversation and caller gets at
most 8 rings a minute (`CHAT_DIRECT_RINGS_PER_WINDOW`), so ever-new call ids
cannot keep a phone ringing. `calls` is in `ACCOUNT_EVENT_PUSH_EVERY_TOPICS`
and `ACCOUNT_EVENT_SOCKET_EVERY_TOPICS`, outside the 2 s coalescing window, so
calling again right after a decline still rings. The call id is cut to 64
characters before it is remembered. The event goes two ways:

- the account event socket gets `{"type":"event","topic":"calls",
  "conversation":"direct:<id>"}`; the client opens that DM's room socket, the
  next resent invite arrives, and the incoming-call screen shows;
- APNs gets an alert (`direct_call_alert` in `apns.py`): "📞 Incoming call",
  body "Incoming call", `topic: calls` and the conversation, the
  `forkmesh-call-ring.wav` sound (a 27 s ring the iOS app writes to
  `Library/Sounds`; the default sound before the app has run once),
  `interruption-level: time-sensitive`, no `mutable-content`, collapse id
  `call-direct:<id>` so the second push replaces the first, and
  `apns-expiration` 60 s so an offline phone never rings for an old call.

**VoIP push for iPhones.** The invite envelope also carries the plaintext
`call` id (a random UUID, the same as inside the sealed invite). The iPhone
registers a PushKit token beside its alert token (`voipToken` on
`POST /api/accounts/register-device`, inside the row's encrypted `data`). For a
`calls` event `_apns_send_call` sends each phone with one a VoIP push
(`apns.send_voip`: `apns-push-type: voip`, topic `<bundle id>.voip`,
`apns-expiration: 0`, payload `{topic, conversation, callId, origin}`) instead
of the alert, once per phone and call (`_APNS_VOIP_CALL_MEMO`, 2 min), so the
20 s re-ring does not report the call twice. A phone without a VoIP token, or
whose VoIP push fails, gets the alert; a VoIP token Apple calls dead is
dropped and the alert token kept. iOS shows it as a CallKit call
(`docs/native/ios/FMChat.md`). A VoIP push needs no notification permission,
so `PushNotificationService.start` always calls `PushPlatform.registerForCalls`
(`registerForRemoteNotifications`, which iOS answers whatever the permission)
and registers both tokens without prompting; with notifications off, calls
still ring through CallKit.

**No caller name in pushes.** Neither push names the caller, so neither Apple
nor a lock screen sees who is calling; the app shows the name after opening
the sealed invite. Nothing rings when the callee has blocked the caller
([chat-relay-hardening](chat-relay-hardening.md#server-blocks)). The relay learns that a
call is ringing in that conversation, when, and its random id (inferable from
the traffic anyway), not whether it is voice or video.

**Missed calls.** A call never answered posts a persisted "📞 Missed
audio/video call" line from the caller, which takes the normal notification
path: when the 45 s ring runs out, and when the caller hangs up while it still
rings (`hangUp` in `chat-calls.js`; the `call-bye` carries
`reason: "missed"`). A declined call posts nothing.

## Before and during a call

- `/chat` has a `Permissions-Policy` override for camera and microphone and
  `mediastream:` in its CSP `media-src`.
- **Pre-call check.** The Voice call and Video call buttons open the call
  panel in preview: your camera picture first (video) with the face,
  background and voice pickers, or just the microphone (voice), plus "Record a
  10-second test clip", which records exactly what the other person would get
  (the stage track and the changed voice) and plays it back. "Start voice
  call" / "Start video call" then rings with the same camera and microphone.
  A `/chat?dm=<name>&call=audio|video` link opens this check, so a link cannot
  ring anyone by itself. The desktop has no WebRTC stack; its call buttons
  open that link.
- **Ringing.** The caller hears a ringback (440 + 480 Hz, 2 s on, 4 s off).
  The callee gets a full-screen overlay with Accept and Decline, a ring
  (400 + 450 Hz double ring), `navigator.vibrate` on phones, a flashing tab
  title and, when the tab is hidden or unfocused and notifications are
  allowed, a system notification with `requireInteraction` until the ring
  ends. Notification permission is asked the first time you open a pre-call
  check. Both tones are synthesized with Web Audio (`chat-call-tones.js`); a
  browser that blocks audio before any click still shows the overlay and the
  notification.
- **Quality.** `chat-call-quality.js` asks for 1280×720 at up to 30 fps and
  echo cancellation, noise suppression and automatic gain at 48 kHz mono. It
  caps senders at 2.5 Mbps video and 128 kbps audio, with `balanced`
  degradation where allowed, and adds Opus in-band FEC, no DTX, mono,
  `maxplaybackrate=48000`, a 128 kbps average and
  `sprop-maxcapturerate=48000` (the microphone really captures full band) to
  the SDP it sends, so each side's encoder sends full-band, loss-resilient
  voice. Audio senders get `priority` and `networkPriority` "high" and video
  "medium", so under congestion the browser keeps the voice and lowers the
  picture. When `RTCRtpSender.getCapabilities("audio")` lists `audio/red`,
  `preferCallAudioCodecs` orders the audio codecs RED, Opus, then the rest
  with `setCodecPreferences` (caller before its offer, callee before its first
  answer): RED (RFC 2198) repeats the previous Opus frame in each packet, so a
  lost packet costs no audio. The same constants are in
  `CallMediaTuning.swift` (`audioMaxBitrate`, `audioPriority`,
  `videoPriority`, `audioCodecOrder`, `orderedAudioCodecs`) and the phone page
  glue (`AUDIO_MAX_BITRATE`, `OPUS_FMTP`). The Face Changer and Voice Changer
  pages share these constraints through `call-studio.js`. The face stage
  draws at up to 30 fps and at most 960 px on the long side, so a filtered
  picture is limited by that, not by the camera.
- **Self view.** Your video (`#chat-call-local`) drags with mouse or finger
  (pointer events, `touch-action: none`) and snaps to the nearest corner
  (`data-corner`, kept in `localStorage` `fm.call.previewCorner`); it does not
  drag in the pre-call check, where it fills the panel. Up to 480 px wide the
  panel spans the width with 8 px margins and scrolls if taller than the
  screen; Start / Mute / Camera off / Hang up share each row at 44 px tall,
  and Accept and Decline share the card's width. The phone apps follow the
  same idea (`docs/native/ios/FMChat.md`).
- **Large view and mini window.** A live or ringing video call
  (`.is-call.is-video`) fills most of the window (up to 1100 px wide, centred,
  remote video up to the window height less the controls, self view up to
  220 px). "Back to chat" (`#chat-call-minimize`) turns it into a floating
  window (`.is-mini`): the other person's video (or initial for voice), Mute,
  Hang up and Open. Click, tap or Open makes it large again. It drags anywhere
  (movement under 6 px is a tap), stays 8 px inside the window, is re-placed
  on resize, and keeps its position in `localStorage` `fm.call.miniPosition`
  (`{left, top}` px; first time 16 px from the bottom-right). The self-view
  drag is hidden in the mini window. Up to 480 px the large view spans the
  width with 8 px margins and the mini window is 150 px (168 px for video).
- **No blank screen.** Until the first remote frame, and whenever the other
  side's video stops, the remote area shows the partner's initial and
  "Waiting for @name's video…" (`#chat-call-placeholder`), with your camera
  still in its corner. The remote video counts as showing when its track
  unmutes or the element reports a frame size, and as stopped when the track
  mutes or the stats show fewer than 2 new decoded frames in 2 s (a camera
  turned off sends about one black frame a second). Voice calls show the
  initial and name. If the browser blocks autoplay with sound
  (`NotAllowedError`) the call plays muted and offers "Tap to turn on sound"
  (`#chat-call-sound`).

## Live quality widget

Every call shows a small pill with live charts. `call-quality-meter.js` is the
one implementation: the direct call puts it in the panel head next to the
route line (`.fm-q` uses `display: contents`); group calls and the Office
([../office/README.md](../office/README.md)) mount the same widget.

- **Pill.** Four signal bars coloured by grade (success, attention, danger), a
  mirrored voice waveform (your microphone up in accent blue, the other side
  down in grey) and the round-trip time. Clicking toggles the charts (kept in
  `localStorage` `fm.call.qualityOpen`). In the pre-call check and while
  ringing it shows only the microphone waveform.
- **Charts.** Voice (last 5 s of levels, 10 per second), Network (round-trip
  time over the last minute, amber ticks at 1 % loss, red at 8 %), and on
  video calls Video in and Video out (bitrate over the last minute, with
  resolution, frame rate and `qualityLimitationReason`).
- **Data.** Voice levels come from a Web Audio `AnalyserNode` on the outgoing
  and each received audio track (RMS mapped from -60..0 dB to 0..1), falling
  back to `audioLevel` in `getStats()`. Once a second `rtcStatsReader` reads
  `getStats()` from every peer connection: the selected pair's
  `currentRoundTripTime`, `inbound-rtp` loss, jitter and bytes,
  `remote-inbound-rtp` `fractionLost`, frame size and rate. Several
  connections (Office) are summed for bitrate and take the worst round-trip
  time, loss and jitter. `fmmStatsReader` turns the FM Media worker's `stats`
  reply into the same sample for group calls.
- **Grade.** Bars are 4 minus the worst step reached by round-trip time (150,
  300, 500 ms), loss (1, 3, 8 %) or jitter (30, 60, 100 ms), at least 1. With
  no round-trip time and no loss yet it says "Measuring connection". The
  phone apps use the same thresholds.
- **Cost.** The pill is written only when a value changes, charts redraw only
  on a new sample, and nothing draws while not visible (the `visible` option;
  the direct call passes "not minimized and the tab is shown"). The route
  check, stats watchdog and meter share one `getStats()` call a second
  (`sharedStats`, reused for 800 ms).
- **Phones.** `app/tools/sync_call_quality.py` inlines the module into both
  copies of `direct-call.html` as `window.FmCallQuality`;
  `app/tests/test_call_quality_meter.py` fails if a copy is stale. The page
  posts `{event: "quality", live, grade, bars, rttMs, lossPct, jitterMs,
  sendKbps, recvKbps, inWidth, inHeight, inFps, outWidth, outHeight, outFps,
  limit, mic: [...], peer: [...]}` once a second (`mic`/`peer` are the level
  samples since the last event; `live` is false in preview). The apps draw
  the widget natively (`docs/native/ios/FMChat.md`,
  [../native/ANDROID.md](../native/ANDROID.md)).
- **Diagnose button.** Right of the pill (`.fm-q-bar` holds both), labelled
  "Diagnose" with a dot, in the foreground colour with a stronger border so it
  reads as a button; `aria-label` "Call diagnostics" before the first
  measurement, then the summary. The dot is grey while measuring and green
  (`--online`) when nothing is wrong. It shows on every live call (the Office
  uses the same label). Dot and text turn red (`--danger`), with a light red
  fill when severe, when `diagnoseCall` finds a problem in the last 8 samples
  (at least 3 needed): latency (median round trip 300 ms or more, severe at
  500), loss (mean 3 %, severe at 8), jitter (median 60 ms), video out limited
  by bandwidth or CPU in most samples, video out under 15 fps (severe under
  7.5), the other side's video under 15 fps or 360p while it still arrives at
  100 kb/s or more (so a camera turned off is not a problem), and the face
  filter over 34 ms a frame (severe over 50). Camera checks are skipped while
  your camera is off (browsers then send black frames at about 1 fps). The
  face numbers come from the face stage's `stats`, which the meter turns into
  per-second `faceFps`, `faceMs` and `faceLevel` when given
  `face: () => stage.stats`.
- **The panel.** Lists each problem with what was measured, why it matters and
  the fixes in order, then a prompt to paste into an AI assistant (Copy
  prompt) and a field for its answer. Nothing is sent anywhere.
- **Self-improving prompt.** `fm.call.diagnostics` in `localStorage` keeps the
  last 20 reports, a worked/failed count per fix and the last 8 lessons. When
  the panel opens with a problem, its first fix is the one being tried; if
  the problem clears during the call the fix counts as worked, if the call
  ends with it still there, as failed. `rankFixes` orders fixes by
  worked × 2 − failed. The prompt (`diagnosticsPrompt`, numbered v1, v2, ...
  by the reports behind it) gives the measurements, the device, each problem
  with how often it was seen, what worked and what did not, and the saved
  lessons, and asks the assistant to end with a `LESSON:` line; pasting that
  line saves it for the next prompt.

## Mobile apps

iOS and Android run the same signaling state machine (`DirectCallEngine`)
through their `DirectMessageService` and interoperate with `/chat`. Media runs
in a WebView loading `direct-call.html` from an `https://` app-local origin (a
secure context for `getUserMedia`); the native side drives it through
`window.fmCall(id, op, args)` and receives replies through
`webkit.messageHandlers.fmCall` or `FmCallBridge.post`. A ring reaches the
phone through the `calls` account event and push above: iOS reports incoming
calls to CallKit (system call screen, lock screen, system ringtone), woken by
a VoIP push even when suspended or killed; Android's background connection
wakes the call engine or posts a ringing full-screen call notification
(`docs/native/ios/FMChat.md`, [../native/ANDROID.md](../native/ANDROID.md)).

The page applies the same quality settings as `/chat` (constraints, sender
caps, Opus FEC on the SDP it returns from `offer`/`answer`) and posts
`{event: "route", route: "direct"|"relay", local, remote, protocol}` when the
call connects and whenever the selected pair changes (`route` op returns the
last one); the apps show it under the state line. Both apps open a pre-call
check from Voice call and Video call (live preview, a 10-second test clip,
then Call), play a ringback while the caller waits, and ring the callee over
any screen. `preview` and `record` also work with media `audio` (microphone
only, `body.audio.preview`, an `audio/*` clip) for the voice-call check.

The page, its host (`WebCallPeer` and its factory) and the face, background
and voice choices belong to the Face Changer plug-in, not to chat; calls use
them from there (see [Face Changer on the phones](call-filters.md#face-changer-on-the-phones)).

`app/tools/sync_native_call_faces.py` and
`app/tools/sync_native_call_voice.py` inline the face modules, assets, models
and the voice code into both copies of `direct-call.html`;
`app/tests/test_chat_call_faces.py` fails if a copy is stale. The hand-written
glue `<script>` at the end of the page is not generated, but the two copies
must stay byte-identical, so a glue change goes into both.

### Reconnects, media watchdog and the staged view (page glue)

`setup` takes an eighth argument, an options object. A host that leaves it out
(Android today) gets the plain page: it posts `closed` when the connection
fails or stays `disconnected` for 8 s, and none of the events below. The
iPhone host passes `{ resilient: true, staged: true }`, because its
`DirectCallEngine` implements the [Resilience](#resilience) rules itself.

- **`resilient`.** The page does not close on `failed` or `disconnected`; it
  posts `{ event: "connection", state: "connected" | "disconnected" |
  "failed" }` and lets native decide. `live` is posted on every `connected`,
  but the route watch and quality meter start once per connection, so a
  reconnect keeps the charts. A 1 s watchdog posts
  `{ event: "connection", state: "stalled" }` once when inbound audio
  `packetsReceived` has not grown for 4 s while `connected`, and `connected`
  when packets flow again. When the outgoing microphone track ends, the page
  asks `getUserMedia` for the same `AUDIO_CONSTRAINTS` up to three times a
  second apart, swaps the track in (rebuilding the voice stage when a voice is
  on) with `replaceTrack`, keeps `mute`, and posts `{ event: "mic", ok }`.
- **Ops.** `restart` calls `pc.restartIce()` (when present), makes an
  `iceRestart` offer, sets it, re-applies the sender caps and returns the
  tuned offer like `offer`; a later `answer`/`accept` completes it, so the
  callee reuses `answer`. `wake` plays both videos again and, when resilient,
  re-acquires an ended microphone (native calls it after an audio
  interruption and on return to the foreground). `layout("mini" | "full")`
  toggles `body.mini`, which hides the self view in the floating mini window.
- **`staged`.** The body gets `staged`, and `waiting` until the first remote
  video frame (`framesDecoded` rises): CSS shows the mirrored self view full
  screen and hides `#remote`, so a ringing video call shows your camera, not
  black. After the first frame the self view goes back to its corner. When
  fewer than 2 frames decode in 2 s, or the remote track mutes, the body gets
  `remote-off` and `#remote` is hidden. Each change posts
  `{ event: "remote-video", on, seen }`; the iPhone draws the partner's avatar
  while `on` is false. `setup` keeps `mini` across the class reset.
- **Media tuning (both hosts).** Audio senders get `priority` and
  `networkPriority` `high`, video `medium`, and the audio transceiver prefers
  RED then Opus when `RTCRtpReceiver.getCapabilities("audio")` lists
  `audio/red`.

`app/tests/test_native_call_page_resilience.py` runs the glue in node with a
fake `RTCPeerConnection`, media devices and timers and checks both hosts.

## Tests

`app/tests/test_chat_direct_calls_frontend.py`,
`app/tests/test_chat_call_checks.py` (pre-call check, tones, incoming alert,
quality and route),
`app/tests/test_chat_call_layout.py` (the answering iPhone's tracks, the
self-view drag and small-screen buttons),
`app/browser_tests/tests/native-call-record.spec.js`, plus the iOS and
Android `DirectCall*` tests. Filter tests: [call-filters.md](call-filters.md#tests).

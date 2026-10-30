# Direct messages in the app, group add, and encrypted peer-to-peer calls

Direct-message surfaces on the dashboard, turning a DM into a group, and
one-to-one audio/video calls with on-device face, background and voice
filters. Access rules and DM delivery are in
[chat-private-rooms.md](chat-private-rooms.md).

## Direct messages on the dashboard

The dashboard chat page is gone: `/dashboard/chat` redirects to `/chat`, which
lists direct messages in its own rail ([web-chat-layout.md](web-chat-layout.md)).
The strip below stays in `dashboard-chat.js` but only runs where the old page's
channel header exists.

`mountDirectMessageStrip` (in `dashboard-chat.js`) inserts a strip of DM chips
under the channel header. Each chip links to `/chat?dm=<name>` with its unread
badge; the `+` chip links to `/chat?dm=`, which opens the new-DM picker. The
strip is read once from `GET /api/chat/direct-messages?limit=20` and refreshed
only when the account event channel pushes the `direct-messages` topic
(re-broadcast as the `forkmesh:direct-messages` window event). It is skipped
in the World embed and for signed-out visitors.

## "Add person" in a direct message

A DM is strictly one-to-one, so "+ Add person" creates a **new private
channel** with the current partner and the picked person
(`POST /api/chat/channels`) and switches to it. The name is built from the
participants' slugs plus a random suffix within `CHANNEL_NAME_RE`. History is
not copied: it is end-to-end encrypted to the original two participants.

## Calls

`app/public/chat-calls.js` adds one-to-one calls to DMs on `/chat`.

- **Signaling** rides the DM room socket as non-persisted, sealed frames:
  `call-invite`, `call-accept`, `call-decline`, `call-offer`, `call-answer`,
  `call-ice`, `call-bye`. The relay sees only ciphertext and stores nothing.
- **Media** is WebRTC (DTLS-SRTP) directly between peers, with a public STUN
  server and no TURN, so two peers behind symmetric NAT cannot connect.
  Once connected, the call reads the selected ICE candidate pair from
  `getStats()` every 4 s and shows whether the media is **Direct**
  (peer-to-peer, no server in between: host, srflx or prflx candidates) or
  **Relayed** (a `relay` candidate, meaning a TURN server, a third party,
  carries the encrypted media). With today's STUN-only setup a call is
  always direct or fails; the note is there so a TURN server added later is
  visible to the people on the call. The live state line says "Connected ·
  end-to-end encrypted" and no longer claims peer-to-peer on its own.
- **Flow**: fixed transceiver order (audio, video), the caller offers only
  after accept, trickle ICE with a pre-answer queue, `replaceTrack` instead of
  renegotiation (an ICE restart is the only renegotiation, see Resilience
  below). Frames carry `to` so multi-device accounts converge on one
  answering device.
- **Resilience** (2026-10-23). Real calls showed a callee stuck on
  "Connecting…", a caller looking at a black box, and calls that went silent
  mid-way until someone hung up. Each signaling frame was sent once, so one
  lost frame on the relay stalled the call, and any network blip ended it.
  `chat-calls.js` now follows the shared resilience spec, also used by the
  iPhone engine. No new frame types: an old client that ignores the extra
  frames still works.
  - *Accept resend.* After `call-accept` the callee resends the same frame
    every 2 s until the caller's `call-offer` arrives or the call ends, at
    most 8 times.
  - *Duplicate accept.* The caller answers a repeated accept from the device
    it picked with its current offer (same SDP) while that offer has no
    answer yet; an accept from another device, or one that arrives before the
    offer exists, is ignored. Before, any accept after the first returned
    early, so a lost offer was never sent again.
  - *Offer resend.* The caller resends an unanswered offer every 3 s, at most
    5 times per offer.
  - *Duplicate offer.* The callee remembers the last offer it answered and
    its answer; the same SDP again gets the stored answer back, a different
    SDP from the same device is an ICE restart and is applied and answered.
    Offers are handled one at a time, and ICE candidates that arrive while an
    offer is being answered (or, on the caller, while its offer is
    unanswered) wait in the queue, so a restart's candidates are not thrown
    away against the old description.
  - *Duplicate answer.* The caller ignores an answer when it has no
    outstanding offer, the signaling state is already stable, or it is the
    answer it already applied. A duplicate used to throw in
    `setRemoteDescription` and end the call.
  - *Connect timeout.* A call that was accepted but is not live 30 s later
    ends with `call-bye` and shows "Could not connect · The call could not
    connect. Try again." Before, it could sit on "Connecting…" forever.
  - *Reconnect.* `failed` no longer ends the call at once and `disconnected`
    no longer ends it after 8 s. The caller drives an ICE restart
    (`restartIce()` and a fresh offer, with the resends above) when the
    connection fails, or stays disconnected for 2 s, and tries again every
    6 s while it is still down; the callee shows "Reconnecting…" and answers
    the restart offer. Either side ends the call with `call-bye` and "Call
    lost" when it has not come back within 30 s of the drop. Back to
    `connected`, the state line returns to "Connected · end-to-end encrypted".
  - *Media watchdog.* Once a second while live, the call reads `getStats()`
    and sums `inbound-rtp` audio `packetsReceived`. If it has not grown for
    4 s while the connection still says connected, that is a drop: the caller
    restarts ICE, the callee shows "Reconnecting…", and the drop ends when
    packets flow again. Opus runs without DTX, so a muted peer still sends
    packets and does not trigger it. This is the "audio went completely
    silent" case: the connection looked fine, so nothing used to react.
  - *Microphone recovery.* When the outgoing microphone track ends (device
    unplugged, an OS interruption, permission revoked) the call opens the
    microphone again with the same constraints, swaps it into the local
    stream and onto the audio sender with `replaceTrack` (rebuilding the
    voice stage when a voice is on), keeps the mute state, and says
    "Microphone reconnected"; when that fails the state line says to check
    the microphone for five seconds.
  - *Invites while answering.* The resent invite used to replace the pending
    invite object, and accepting compared against the old object, so
    pressing Accept while the camera permission prompt was open (longer than
    the 3 s invite resend) silently did nothing: "I could not pick up". The
    resent invite now only refreshes the 9 s stale timer, an invite stays up
    while an accept is in progress, and if the caller hangs up meanwhile, or
    the camera cannot open, the panel says so. An invite that arrives during
    your own pre-call check now rings instead of answering "busy".
  - *Declines say why.* The caller sees "Call declined" or "Busy" with the
    reason (declined, on another call, camera or microphone unavailable)
    instead of a panel that just closes. These end states close after 8 s
    or on Close.
  - *Rings reach a web callee in another conversation.* `/chat` keeps one
    room socket, the open conversation's, so a ring for a DM you were not
    looking at never arrived on the web. The `calls` account event
    (`{topic: "calls", conversation: "direct:<id>"}`) now opens that
    conversation (`openRingingConversation` in `chat.js`) when no call or
    pre-call check is running; the next resent invite then shows the incoming
    call. Signaling still needs that conversation open, so switching to
    another conversation during a call stops its signaling; the mini window
    below keeps you in the conversation instead.
  - Every timer goes through the injected `setTimeoutImpl` /
    `setIntervalImpl` and a per-call registry, and teardown clears all of
    them; `app/tests/test_chat_call_resilience.py` drives each rule with a
    fake clock and peer connection.
- **Call rings reach the callee wherever they are** (added 2026-10-21; before
  that a ring reached only a device that already had that DM open, so a call
  to a phone sitting on another screen, or in the background, showed nothing).
  Every client marks the `call-invite` envelope with a plaintext
  `ring: true` (never persisted; the invite itself stays sealed). The relay
  (`ForkMeshRoom._ring_direct` in `entry.py`) answers a live `ring` frame in
  a direct room with `notify_account_event(<other participant>, "calls", …)`,
  at most once every 20 s per conversation and caller
  (`CHAT_DIRECT_RING_EVERY_MS`), so the invite resent every 3 s rings twice
  over a 45 s ring. A new call id rings straight away. Each conversation and
  caller gets at most 8 rings a minute (`CHAT_DIRECT_RINGS_PER_WINDOW`), so
  sending invites with ever-new call ids cannot keep the other phone ringing.
  `calls` is in `ACCOUNT_EVENT_PUSH_EVERY_TOPICS` and
  `ACCOUNT_EVENT_SOCKET_EVERY_TOPICS` (2026-10-21): before, it shared the
  account event's 2 s coalescing window, so calling again right after a
  decline or hang-up sent neither the socket frame nor the push, and the
  second call never showed. The call id is cut to 64 characters before it is
  remembered. The event goes two ways:
  - the account event socket gets `{"type":"event","topic":"calls",
    "conversation":"direct:<id>"}`; a client that sees it opens that DM's
    room socket, the next resent invite arrives, and the normal incoming-call
    screen shows;
  - APNs gets an alert (`direct_call_alert` in `apns.py`): "📞 Incoming call",
    body "Incoming call", `topic: calls` and the conversation, the
    `forkmesh-call-ring.wav` sound (a 27 s ring the iOS app writes to
    `Library/Sounds`; iOS falls back to the default sound before the app has
    run once), `interruption-level: time-sensitive`, no `mutable-content`
    (the notification extension never touches it), collapse id
    `call-direct:<id>` so the second push replaces the first, and an
    `apns-expiration` of 60 s so a phone that was offline never rings for a
    call that is long over.

  - **VoIP push for iPhones** (2026-10-21). The invite envelope also carries
    the plaintext `call` id (a random UUID, the same one inside the sealed
    invite). The iPhone registers a PushKit token beside its alert token
    (`voipToken` on `POST /api/accounts/register-device`, kept inside the
    row's encrypted `data`, so no schema change). For a `calls` event
    `_apns_send_call` sends each phone that has one a VoIP push
    (`apns.send_voip`: `apns-push-type: voip`, topic `<bundle id>.voip`,
    `apns-expiration: 0` so it is delivered now or never, payload
    `{topic, conversation, callId, origin}`) instead of the alert,
    once per phone and call (`_APNS_VOIP_CALL_MEMO`, 2 min), so the 20 s
    re-ring does not report the call twice. A phone without a VoIP token, or
    whose VoIP push fails, gets the alert push as before; a VoIP token Apple
    calls dead is dropped from the row and the alert token kept. iOS shows the
    VoIP push as a CallKit call (`native/ios/docs/FMChat.md`).
  - **No caller name in pushes** (2026-10-22). The alert body used to be
    "@caller is calling you" and both payloads carried a `caller` field, so
    Apple and anyone reading the lock screen saw who was calling in
    plaintext. Neither push names the caller now; the app shows the name
    after it opens the sealed invite. Nothing rings when the callee has
    blocked the caller ([chat-relay-server](chat-relay-server.md#r5-blocks)).
  - **Notifications turned off on the iPhone** (2026-10-21). A VoIP push
    needs no notification permission, but the phone used to register nothing
    when the system prompt was declined or notifications were off in
    Settings, so the relay had no VoIP token and the call never rang.
    `PushNotificationService.start` now calls `PushPlatform.registerForCalls`
    (`registerForRemoteNotifications`, which iOS answers with a device token
    whatever the permission) and registers both tokens as usual, without
    prompting again. iOS drops the alert pushes that phone gets, and calls
    ring through CallKit. Settings' quick setup still counts notifications as
    off.

  The relay learns that a call is ringing in that conversation and when, and
  the call's random id, which it could already infer from the traffic; it
  does not learn whether it is a voice or video call. A call that is never answered posts a persisted "📞 Missed audio/video call" chat
  line from the caller, which takes the normal notification path, so the
  person who missed it sees it in the conversation later. That happens both
  when the 45-second ring runs out and when the caller hangs up while it is
  still ringing (`hangUp` in `chat-calls.js`; the `call-bye` then carries
  `reason: "missed"`). Before 2026-10-20 only the timeout posted the line, so
  a caller who gave up early left no trace. A declined call posts nothing.
- `/chat` has a `Permissions-Policy` override for camera and microphone and
  `mediastream:` in its CSP `media-src`.
- **Pre-call check.** The Voice call and Video call buttons no longer ring
  straight away. They open the call panel in preview: your camera picture
  (video) with the face, background and voice pickers, or just the
  microphone (voice), plus "Record a 10-second test clip", which records
  exactly what the other person would get (the stage track and the changed
  voice) and plays it back in the panel. "Start voice call" / "Start video
  call" then rings, reusing the same camera and microphone. A
  `/chat?dm=<name>&call=audio|video` link opens this check instead of a
  `confirm()`, so a link still cannot ring anyone by itself. The separate
  eye (preview) button is gone.
- **Ringing.** While the caller waits for an answer they hear a ringback
  tone (440 + 480 Hz, 2 s on, 4 s off). The callee gets a full-screen
  incoming-call overlay with Accept and Decline, a ring (400 + 450 Hz double
  ring), `navigator.vibrate` on phones, a flashing tab title and, when the
  tab is hidden or not focused and notifications are allowed, a system
  notification with `requireInteraction` that stays until the ring ends.
  The browser asks for notification permission the first time you open a
  pre-call check. Both tones are synthesized with Web Audio in
  `chat-call-tones.js`; a browser that blocks audio before any click on the
  page still shows the overlay and the notification.
- **Quality.** `chat-call-quality.js` asks for 1280×720 at up to 30 fps and
  for echo cancellation, noise suppression and automatic gain at 48 kHz
  mono. It caps the senders at 2.5 Mbps video and 128 kbps audio, with
  `balanced` degradation where the browser allows it. It also adds Opus
  in-band FEC, no DTX, mono, `maxplaybackrate=48000`, a 128 kbps average
  and `sprop-maxcapturerate=48000` to the SDP sent to the other side, so
  each side's encoder sends full-band, loss-resilient voice.
  - *Richer audio* (2026-10-23). The audio cap went from 96 to 128 kbps
    (`maxaveragebitrate` and the sender's `maxBitrate`), and
    `sprop-maxcapturerate=48000` tells the far encoder the microphone really
    captures full band. Audio senders get `priority` and `networkPriority`
    "high" and video "medium" (both used to be "high"), so under congestion
    the browser keeps the voice and lowers the picture. When the browser
    lists `audio/red` in `RTCRtpSender.getCapabilities("audio")`,
    `preferCallAudioCodecs` orders the audio transceiver's codecs RED, then
    Opus, then the rest with `setCodecPreferences` (the caller before its
    offer, the callee before its first answer): RED (RFC 2198) repeats the
    previous Opus frame in each packet, so a lost packet costs no audio.
    Browsers without RED or `setCodecPreferences` are left as they were. The
    same constants are in `CallMediaTuning.swift` (`audioMaxBitrate`,
    `audioPriority`, `videoPriority`, `audioCodecOrder`,
    `orderedAudioCodecs`) and in the phone page glue (`AUDIO_MAX_BITRATE`,
    `OPUS_FMTP`).
  The Face Changer and Voice Changer pages share these constraints through
  `call-studio.js`. The face stage draws at up to 30 fps and at most 960 px
  on the long side, so a filtered picture is limited by that, not by the
  camera.
- **Layout** (2026-10-21). Your own video in the call panel (`#chat-call-local`) can be
  dragged with a mouse or a finger (pointer events, `touch-action: none`); on release it snaps
  to the nearest corner of the video (`data-corner`), and the corner is kept in `localStorage`
  under `fm.call.previewCorner`. It does not drag in the pre-call check, where it fills the
  panel. On screens up to 480 px wide the panel spans the width with 8 px margins and scrolls
  if taller than the screen, and the Start / Mute / Camera off / Hang up buttons share each row
  equally at 44 px tall, so they are easy to hit; the incoming call's Accept and Decline share
  the card's width. The phone apps' call screens follow the same idea
  (`native/ios/docs/FMChat.md`).
- **Large view, Back to chat and the mini window** (2026-10-23). A live or
  ringing video call (`.is-call.is-video`) fills most of the window (up to
  1100 px wide, centred, remote video up to the window height less the
  controls, your own video up to 220 px), so the call is not a 360 px box
  in a corner. "Back to chat" in the panel's head (`#chat-call-minimize`)
  turns it into a small floating window (`.is-mini`): the other person's
  video (or their initial for a voice call), Mute, Hang up and Open. Click or
  tap the window, or press Open, to make it large again. Drag it anywhere
  with a mouse or a finger (pointer events, `touch-action: none`, movement
  under 6 px counts as a tap); it stays 8 px inside the window, is placed
  again when the window is resized, and its position is kept in
  `localStorage` under `fm.call.miniPosition` (`{left, top}` in px; the
  first time it sits 16 px from the bottom-right corner). The quality widget
  stops drawing while the call is small or the tab is hidden. The self-view
  corner drag works as before in the large view and is hidden in the mini
  window. On screens up to 480 px the large view spans the width with 8 px
  margins as before, and the mini window is 150 px (168 px for video).
- **No blank screen** (2026-10-23). Until the first remote frame arrives,
  and whenever the other side's video stops, the remote area shows the
  partner's initial and "Waiting for @name's video…" (`#chat-call-placeholder`)
  over the video instead of a black box, with your own camera still in its
  corner. The remote video counts as showing when its track unmutes or the
  element reports a frame size, and as stopped when the track mutes or the
  once-a-second stats show fewer than 2 new decoded frames for 2 seconds
  (a camera turned off sends about one black frame a second). Voice calls
  show the initial and name. After the stream is attached the call calls
  `play()`; if the browser blocks autoplay with sound (`NotAllowedError`)
  it plays muted so the picture still shows and offers "Tap to turn on
  sound" (`#chat-call-sound`), which unmutes and plays again.
- **Pre-call check first.** The Voice call and Video call buttons still open
  the pre-call check before anything rings; the camera picture is the first
  thing in the panel, under the name, with the test clip and pickers below.
- The desktop has no WebRTC stack; its call buttons open
  `/chat?dm=<name>&call=audio|video`, which asks for confirmation before
  ringing, so a link can never ring someone on its own.
- **Live quality widget** (2026-10-22). Every call shows a small pill with
  live charts, so you can see what the call is doing without opening
  developer tools. `call-quality-meter.js` is the one implementation; the
  direct call panel puts the pill in its head next to the route line (the
  `.fm-q` element uses `display: contents`), group calls and the Office
  (`docs/office/README.md`) mount the same widget.
  - **Pill.** Four signal bars coloured by grade (success, attention, danger),
    a tiny mirrored voice waveform (your microphone up in the accent blue,
    the other side down in grey) and the round-trip time. Clicking it opens
    or closes the charts; the choice is kept in `localStorage` under
    `fm.call.qualityOpen`. In the pre-call check and while ringing it shows
    only the microphone waveform, so you can see the microphone works.
  - **Charts.** Voice (the last 5 s of levels, 10 per second), Network
    (round-trip time over the last minute, with amber ticks at 1 % packet
    loss and red ticks at 8 %), and on video calls Video in and Video out
    (bitrate over the last minute, with resolution, frame rate and WebRTC's
    `qualityLimitationReason`, such as "limited by bandwidth").
  - **Data.** Voice levels come from a Web Audio `AnalyserNode` on the
    outgoing microphone track and on each received audio track (RMS mapped
    from -60..0 dB to 0..1), falling back to `audioLevel` in `getStats()`.
    Once a second `rtcStatsReader` reads `getStats()` from every peer
    connection: the selected candidate pair's `currentRoundTripTime`,
    `inbound-rtp` loss, jitter and bytes, `remote-inbound-rtp`
    `fractionLost`, frame size and rate. Several connections (Office) are
    summed for bitrate and take the worst round-trip time, loss and jitter.
    `fmmStatsReader` turns the FM Media worker's `stats` reply into the same
    sample for group calls on FM Media.
  - **Grade.** Bars are 4 minus the worst step reached by round-trip time
    (150, 300, 500 ms), loss (1, 3, 8 %) or jitter (30, 60, 100 ms), at
    least 1: 4 is a good connection, 1 a poor one, 2 and 3 fair. With no
    round-trip time and no loss yet it says "Measuring connection". The
    phone apps use the same thresholds.
  - **Fewer writes.** The widget used to rewrite the pill's text, title,
    `aria-label` and bars ten times a second; it now writes them only when
    they change, redraws the charts only when a new stats sample arrives,
    and skips drawing while it is not visible (the `visible` option; the
    direct call passes "not minimized and the tab is shown"). The direct
    call's route check, stats watchdog and quality meter share one
    `getStats()` call a second (`sharedStats`, reused for 800 ms) instead of
    each reading it.
  - **Phones.** `app/tools/sync_call_quality.py` inlines the module into both
    copies of `direct-call.html` as `window.FmCallQuality`;
    `app/tests/test_call_quality_meter.py` fails if a copy is stale. The page posts
    `{event: "quality", live, grade, bars, rttMs, lossPct, jitterMs, sendKbps,
    recvKbps, inWidth, inHeight, inFps, outWidth, outHeight, outFps, limit,
    mic: [...], peer: [...]}` once a second: `mic` and `peer` are the level
    samples taken since the last event, `live` is false during the preview
    (microphone only) and true once connected. The apps draw the widget
    natively (`native/ios/docs/FMChat.md`, `docs/native/ANDROID.md`).
  - **Diagnostics** (2026-10-22). A button sits right of the pill
    (`.fm-q-bar` holds both). Until 2026-10-23 it was a 22 px grey circle
    with a grey dot on a grey background and no text, shown only once the
    call had stats, so people did not see it or did not know it was a
    button. It now reads "Diagnose" next to its dot, in the foreground
    colour with a stronger border, with `aria-label` "Call diagnostics"
    before the first measurement (then the summary below); the dot is grey
    while measuring and green (`--online`) when nothing is wrong. It shows
    on every live call, and the Office widget uses the same label. Its dot
    and text turn red (`--danger`), and on a severe problem the button gets
    a light red fill, when `diagnoseCall` finds a problem in
    the last 8 samples (at least 3 needed): latency (median round trip
    300 ms or more, severe at 500), loss (mean 3 %, severe at 8),
    jitter (median 60 ms), video out limited by bandwidth or by CPU in most
    samples, video out under 15 fps (severe under 7.5), the other side's
    video under 15 fps or 360p while it still arrives at 100 kb/s or more
    (so a camera turned off over there is not a problem), and the face
    filter taking over 34 ms a frame (severe over 50). The camera checks
    are skipped while your camera is off, because browsers send black
    frames at about 1 fps then. The face numbers come from the face stage's
    `stats` (frames, total frame time, quality level), which the meter
    turns into per-second `faceFps`, `faceMs` and `faceLevel` on each
    sample when it is given `face: () => stage.stats`.
  - **The panel.** Pressing the button lists each problem with what was
    measured, why it matters and the fixes in order, then a prompt to paste
    into an AI assistant (Copy prompt) and a field for the assistant's
    answer. Nothing is sent anywhere: the report stays on the device.
  - **Self-improving prompt.** `fm.call.diagnostics` in `localStorage` keeps
    the last 20 reports, a worked/failed count per fix and the last 8
    lessons. When the panel opens with a problem, its first fix is taken as
    the one being tried; if that problem then clears during the call, the
    fix counts as worked, and if the call ends with it still there, as
    failed. `rankFixes` orders fixes by worked × 2 − failed, so fixes that
    work on this device move up and ones that did not move down. The prompt
    (`diagnosticsPrompt`, numbered v1, v2, ... by the reports behind it)
    gives the measurements, the device, each problem with how often it was
    seen before, what worked and what did not, and the saved lessons, and
    asks the assistant to end with a `LESSON:` line. Pasting that line into
    the field saves it, so the next prompt starts from it.

### Mobile apps

iOS and Android run the same signaling state machine (`DirectCallEngine`)
through their `DirectMessageService` and interoperate with `/chat`. Media runs
in a WebView loading `direct-call.html` from an `https://` app-local origin (a
secure context for `getUserMedia`); the native side drives it through
`window.fmCall(id, op, args)` and receives replies through
`webkit.messageHandlers.fmCall` or `FmCallBridge.post`. A ring reaches the
phone through the `calls` account event and push above: iOS reports incoming
calls to CallKit (the system call screen, on the lock screen too, and the
system ringtone), woken by a VoIP push even when the app is suspended or
killed; Android's background connection wakes the call engine or posts a
ringing full-screen call notification
(`native/ios/docs/FMChat.md` and `docs/native/ANDROID.md` have the details).

The page applies the same quality settings as `/chat` (constraints, sender
caps, Opus FEC on the SDP it returns from `offer`/`answer`) and posts a
`{event: "route", route: "direct"|"relay", local, remote, protocol}` event
when the call connects and whenever the selected pair changes (`route` op
returns the last one). The apps show it under the call's state line. Both
apps open a pre-call check from the conversation's Voice call and Video
call buttons (live preview, a 10-second test clip played back, then Call),
play a ringback while the caller waits, and ring the callee over any
screen; the per-platform details are in `native/ios/docs/FMChat.md` and
`docs/native/ANDROID.md`. `preview` and `record` also work with media
`audio` (microphone only, `body.audio.preview`, an `audio/*` clip), which
the voice-call check uses.

The page, its host (`WebCallPeer` and its factory) and the face, background
and voice choices belong to the Face Changer plug-in, not to chat; calls use
them from there (see [Face Changer on the phones](#face-changer-on-the-phones)).

`app/tools/sync_native_call_faces.py` and
`app/tools/sync_native_call_voice.py` inline the face modules, assets, models
and the voice code into both copies of `direct-call.html`;
`app/tests/test_chat_call_faces.py` fails if a copy is stale. The hand-written
glue `<script>` at the end of the page is not generated, but the two copies
must stay byte-identical, so a glue change goes into both.

#### Reconnects, media watchdog and the staged view (page glue, 2026-10-23)

`setup` takes an eighth argument, an options object. A host that leaves it out
(Android today) gets the old page exactly: it posts `closed` when the
connection fails or stays `disconnected` for 8 s, and none of the events
below. The iPhone host passes `{ resilient: true, staged: true }`, because its
`DirectCallEngine` follows the Resilience rules under [Calls](#calls) (accept
and offer resends, duplicate offers and answers, the 30 s connect timeout,
caller-driven ICE restart) itself.

- **`resilient`.** The page no longer closes on `failed` or `disconnected`;
  it posts `{ event: "connection", state: "connected" | "disconnected" |
  "failed" }` and lets native decide. `live` is still posted on every
  `connected`, but the route watch and quality meter start only once per
  connection, so a reconnect keeps the charts. Once connected, a 1 s
  watchdog reads `getStats()`: when inbound audio `packetsReceived` has not
  grown for 4 s while the connection says `connected`, it posts
  `{ event: "connection", state: "stalled" }` once, and `connected` when
  packets flow again (DTX is off, so a muted peer still sends packets). When
  the outgoing microphone track ends (device gone, interruption, permission
  revoked), the page asks `getUserMedia` for the same `AUDIO_CONSTRAINTS` up to
  three times a second apart, swaps the new track into the stream, rebuilds the
  voice stage when a voice is on, puts the result on the audio sender with
  `replaceTrack`, keeps the mute (the page now remembers `mute`), and posts
  `{ event: "mic", ok }`.
- **New ops.** `restart` calls `pc.restartIce()` (when present), makes an
  `iceRestart` offer, sets it, re-applies the sender caps and returns the
  tuned offer like `offer`; a later `answer`/`accept` completes it, so the
  callee's renegotiation reuses `answer`. `wake` plays both videos again and,
  when resilient, re-acquires a microphone whose track has ended (native calls
  it after an audio interruption and when the app comes back to the front).
  `layout("mini" | "full")` toggles `body.mini`, which hides the self view
  while the WebView sits in the floating mini window.
- **`staged`.** The body gets `staged`, and `waiting` until the first remote
  video frame (the watchdog sees `framesDecoded` rise): CSS then shows the
  mirrored self view full screen and hides `#remote`, so a ringing or
  connecting video call shows your own camera instead of black. After the
  first frame the self view goes back to its corner. When fewer than 2 frames
  are decoded over 2 s, or the remote video track mutes, the body gets
  `remote-off` and `#remote` is hidden. Each change posts
  `{ event: "remote-video", on, seen }`; the iPhone draws the partner's avatar
  over the page while `on` is false. `setup` keeps `mini` across the class
  reset.
- **Media tuning in the glue (both hosts).** Audio senders get `priority` and
  `networkPriority` `high`, video `medium` (was `high` for both), and the
  audio transceiver prefers RED then Opus with `setCodecPreferences` when
  `RTCRtpReceiver.getCapabilities("audio")` lists `audio/red`; otherwise
  nothing changes.

`app/tests/test_native_call_page_resilience.py` runs the glue in node with a
fake `RTCPeerConnection`, media devices and timers and checks both hosts: the
old self-close without options, the connection states, `restart`, the stall
and remote-video events, microphone recovery with the mute kept, and
`layout`.

#### iOS TrueDepth camera (face depth)

On an iPhone with a front TrueDepth camera the page does not open video with
`getUserMedia`: WebKit would own the camera and native code could not read the
infrared depth. Instead `CallDepthCamera` (`Features/FaceChanger/`) runs an
`AVCaptureSession` at 640x480, 24-30 fps, with video and filtered
`DepthFloat32` depth paired by `AVCaptureDataOutputSynchronizer`, and pushes
each frame into the page. Android exposes no front depth sensor to apps, so it
never advertises the camera and keeps `getUserMedia` (same glue, inert there).

- **Availability.** When the TrueDepth device exists and camera access is not
  denied, `WebCallPeer` adds a document-start `WKUserScript` setting
  `window.FmCallDepthCamera = { available: true }`.
- **Opening.** For video (preview or call) the page opens
  `getUserMedia({ audio: true, video: false })`, makes a manual stage with
  `createFaceStage({ cameraStream: null, manual: true, ... })` (also with no
  face or background chosen: it then passes the camera through, so the call
  track and view-once recording come from `stage.track` as before) and asks
  native to start the camera.
- **Page to native** (`fmCall` message handler):
  `{ event: "camera", id, op: "start" | "stop", landmarks, mask }`, answered by
  `window.fmCallCamera(id, { ok, error? })`; and
  `{ event: "frame-done", landmarks, mask }` after each pushed frame. The
  `landmarks`/`mask` flags say what native should compute for the next frame
  (landmarks when a face is chosen; a person mask when a face or a background
  is), so a plain camera skips Vision.
- **Native to page.** `window.fmCallFrame({ image, width, height, regions,
  mask, time })`: `image` is a JPEG data URL (at most 480 px, quality 0.75),
  upright and not mirrored like iOS `getUserMedia` front frames (the page
  mirrors only for display); `regions`/`mask` have the landmark reply's shape
  in that image's pixels. The page decodes it, calls
  `stage.pushFrame(img, { regions, mask }, performance.now())`, then posts
  `frame-done`. One frame is in flight: native drops camera frames until
  `frame-done` (or 2 s pass). `window.fmCallCameraFailed(reason)` reports a
  session runtime error or an interruption other than backgrounding.
- **Fallback.** Any failure falls back to the old `getUserMedia` video path
  and the page does not try the depth camera again: no TrueDepth device,
  permission denied, the start request failing or taking over 5 s, no rendered
  frame within 1.5 s of the start, a stage without `pushFrame`, or a later
  `fmCallCameraFailed`. Mid-call, the page stops the native session, waits
  for its stop reply, opens a `getUserMedia` video track and swaps it onto the
  sender through `refreshStage`. Turning the camera off stops the native
  session (the light goes out); turning it on starts it again.
- **Frames.** Native renders the sensor buffer upright (`.right`, mirroring
  turned off on both connections) and at most 480 px into one `CGImage`.
  Vision landmarks, the person mask and the JPEG all use that image, so they
  share one pixel space. The depth map stays in sensor orientation and is
  sampled through the same orientation mapping.
- **Depth (`z`).** With depth, every landmark point is `[x, y, z]`. `z` is
  relative depth in the same pixels as `x`/`y`, larger is nearer, roughly 0 at
  the face: `z = (dFace - d) * fPx / dFace`. `d` is the median of valid
  samples (0.1-5 m, holes skipped) in a 3x3 depth-pixel patch at the point.
  `dFace` is the median over all landmark samples, after dropping samples more
  than 0.15 m from a first median (background behind the jaw line). `fPx` is
  the depth calibration's `fx` scaled from `intrinsicMatrixReferenceDimensions`
  to the image's sensor-oriented width, or comes from
  `activeFormat.videoFieldOfView`. A point without depth takes the nearest
  sampled point in its region (else `dFace`), so either all points carry `z`
  or, when under half of them were sampled, none do. The pure maths lives in
  `ForkMeshCore` `FMChat/Direct/CallFaceDepth.swift` (`CallDepthOrientation`,
  `CallDepthField`, `CallFaceDepth`) with Linux tests in
  `CallFaceDepthTests`.

## Face, background and voice filters

Everything runs on the device before encryption. Models and images are
bundled; nothing is fetched from a third party at runtime.

### Modules (`app/public/`)

| Module | Role |
| --- | --- |
| `chat-call-faces.js` | Built-in stand-in faces (`CALL_FACES`), `createFaceStage`, adaptive quality, iOS bridge |
| `chat-call-face-geometry.js` | Landmarks, head model, `buildHeadMesh`, `solvePose`, `liveHead` |
| `chat-call-face-render.js` | WebGL 1 compositor |
| `chat-call-segment.js` | WebGL inference engine for the bundled models; person and hair mattes |
| `chat-call-face-mesh.js` | Face detection and 468-point mesh mapped to 68 points |
| `chat-call-face-tracker.js` | Heuristic fallback tracker |
| `chat-call-face-expression.js` | Blink, gaze and tongue measures from the camera image |
| `chat-call-face-custom.js` | Builds a stand-in from an uploaded photo ("Your photo", id `custom`) |
| `chat-call-backgrounds.js` | Background list, saved choice, custom photo preparation, portrait image choice |
| `chat-call-outfits.js` | Outfit list (`CALL_OUTFITS`), garment drawing, where the outfit sits on the body |
| `chat-call-voice.js`, `chat-call-voice-worklet.js` | Voice list (`CALL_VOICES`) and the AudioWorklet engine |

### Head swap

The chosen face replaces the outgoing video track with a canvas track
(`captureStream`, 30 fps) through `replaceTrack`. Per frame: landmarks
(Apple Vision on iOS, the face mesh elsewhere) smoothed by the landmark
smoother (below); a person matte refined with a guided filter; a hair matte around the
head; a 2.5D warp of the whole stand-in head with a depth buffer so near
surfaces win when turning; removal of the caller's head and hair outside the
stand-in, filled from an estimate of the room; per-frame skin and hair colour
matching; eyelid, gaze and tongue transfer. Stand-in assets
(`app/public/assets/call-faces/<id>.jpg` and `<id>-matte.png`) are built by
`app/tools/build_call_face_assets.py`.

**Faster faces (2026-10-22).** Faces lagged behind the caller and the lips
looked mushy while talking. Two causes:

- The stage drew on a fixed 24 fps timer, out of step with a 30 fps camera,
  so a frame waited up to 42 ms before it was even looked at, and the
  timer's beat against the camera made motion judder. The stage now runs
  from `requestVideoFrameCallback` on its hidden camera `<video>`, so each
  camera frame is processed as soon as it is decoded, and captures at 30 fps.
  The 30 fps timer stays as a fallback: it only processes a frame when no
  camera frame arrived for three frame times (browsers without the callback,
  or a throttled tab), so a camera that slows to 15 fps in the dark is not
  processed twice.
- All 68 points went through their own One Euro filter in image pixels
  (1.5 Hz minimum cutoff, the speed estimate itself filtered at 1 Hz), so
  a moving head trailed by about 100 ms, the lips lagged speech, and each
  lip point sped up at its own rate, which bent the lip shape mid-word.
  `createLandmarkSmoother` (in `chat-call-face-geometry.js`) splits the
  head from its expression. The two eye centres go through one
  `GroupFilter` (`SMOOTH_HEAD`: 3 Hz, beta 5, speed filtered at 4 Hz), and
  that steady frame carries the head. Every point is also expressed in the
  raw eye frame (in eye spans) and filtered per group (`SMOOTH_GROUPS`:
  jaw, brows, nose, eyes, lips). A `GroupFilter` applies one smoothing
  factor to all its coordinates, driven by the group's fastest point, so
  the lips (2.5 Hz, beta 6, speed at 4 Hz) open and close as one shape and
  follow speech within about a frame. A still face stays as steady as
  before. On a synthetic talking head with tracker noise
  (`app/tests/test_call_face_smoothing.py`), lip error while talking fell
  from 1.55 to 0.67 px and head-follow error from 5.7 to 1.9 px, with rest
  jitter still about a third of the input noise.

**Face depth and pose.** Landmarks may carry depth: a region point is
`[x, y, z]` with `z` in the image's pixel units, larger = nearer. The face
mesh supplies it from its own per-vertex depth output (negated and scaled to
frame pixels in `chat-call-face-mesh.js`); on iOS the TrueDepth camera
supplies measured depth (see "iOS TrueDepth camera" above).
`landmarksFromRegions`, the point smoother and `closeLids` carry `z` through.
When every pose landmark has a `z` and the depths spread by more than 4% of
the eye span, `solvePose` fits the head model in 3D (Horn's quaternion
method, then a 2D least-squares scale) instead of the 2D affine fit, so yaw
and pitch no longer depend on the nose-direction flip heuristic. Flat or
missing depth falls back to the 2D fit.

**Rigid nose.** Raw nose landmarks jitter and disagree with the stand-in's
nose shape, and warping the photo between them bent and twisted the nose.
`liveHead` now places the nose (points 27-35) by projecting the stand-in's
own nose through the solved pose, shifted by the nose group's mean offset
from the live points, and keeps 15% of the live positions
(`NOSE_RIGID = 0.85`). Neighbouring cheek triangles absorb the difference.

**Turning the head.** The stand-in used to be pinned to every live landmark,
including the jaw line. On a turned head the far jaw line is only the cheek's
silhouette (Vision and the face mesh both report it there), so the photo's
far jaw was dragged onto the cheek while the hair ring, placed from the pose,
stayed out: the head stretched across the near cheek and showed a step at eye
level. `liveHead` now places the head as one rigid 3D object through the
solved pose and only adds a smooth residual (a Gaussian field over brows,
nose and eye corners) for the caller's own proportions:

- Jaw points follow the live jaw only as far as they face the camera
  (`jawTrust`: the pose's view direction against an outward jaw normal,
  `JAW_FACING` 0.08-0.38, at most `JAW_FOLLOW` 0.7). The pose is solved twice,
  the second time with the jaw weights scaled by that trust, so a sliding
  silhouette cannot bend yaw or scale.
- 15 cheek and chin vertices (`CHEEK_COUNT`, after the side ring) sit between
  the jaw and the nose, eyes and mouth with a forward bulge in depth, so the
  far cheek foreshortens and the near cheek widens like a curved surface
  instead of one flat triangle from nose to ear. The lip corners and lower
  lip also feed their field, so cheeks follow a smile and the chin follows a
  dropping lower lip.
- The forehead row curves back toward the temples (brow depth x1.5 - 0.12).
- The hair ring is a silhouette, not a surface: `projectOutline` keeps its
  shape and only moves it with the head centre (`HEAD_CENTER`, 1.1 eye spans
  behind the eyes) and roll, the way a round head's outline behaves. A ring
  rotated as a flat curve swung behind the face on the far side and took the
  hair with it.
- A side ring (one point per hair-ring point, `SIDE_REACH` 0.4 of the way out
  from the nearest jaw or forehead point, `SIDE_BACK` 0.35 behind it) carries
  the photo's face edge: on the far side that strip folds away behind the
  cheek and only hair fills the outline; on the near side it opens out.
- Triangles that fold on screen (opposite winding to the photo) are drawn
  collapsed, except around the lips, so a turned-away surface never smears
  past the silhouette. Photo triangles with zero area are dropped.

**Mouth opening.** The mouth used to open as the polygon of the 8 inner-lip
points with a hard edge, so it looked like a box. `buildHeadMesh` now adds
mouth vertices after the cheeks (`MOUTH_EXTRA_COUNT`): each lip is a
Catmull-Rom curve through its inner-lip points (corners kept sharp, 4
samples per segment), plus an inner ring 14% of the way to the other lip
(`MOUTH_FEATHER`) and a deep ring 40% of the way (`MOUTH_DEEP`). Every extra
vertex is a fixed linear combination of the 68 landmarks, so the photo
layout, the 3D model and the live points stay consistent. Triangles carry a
per-corner `hole` value (0 on the lip line, 1 on the inner ring, 2 on the
deep ring, at most 0.5 at the corners); the warp shader turns 0-1 into a
soft edge with `smoothstep` and writes the hole and the depth past it into
the aux target's blue channel (half each). The composite darkens the feather
band as a lip shadow and shades the opening from 60% at the lips
(`MOUTH_EDGE`) to full brightness at the deep ring, so the caller's teeth and
tongue read as inside a mouth rather than behind a cut-out.

Even with curves the opening was still a box (and the stand-in's lips
smeared in a spiral) when the tracker put the quarter points of the inner
lip almost at the corners, as Vision does on a wide-open mouth: the lip
triangles folded over. `regularMouth` now rebuilds the 20 lip points in the
mouth's own frame: every lip point keeps the posed stand-in's position along
the mouth (its photo spacing, foreshortened by the pose) and takes only its
height from the live lip curves; the inner lips keep at least 65% of the
stand-in's lip thickness (`LIP_KEEP`); and the opening tapers into the
corners (`CORNER_TAPER`). The template fit used by the fallback tracker drops
the lower lip with a centre-weighted profile instead of a flat shift.

**Blinks.** `createBlinkTracker` (in `chat-call-face-expression.js`) turned
false blinks into closed eyes less often: the image edge baseline now moves
with a time constant (0.8 s up, 6 s down) instead of jumping 25% toward any
bright frame, and is frozen while an eye is closing; when the landmarks
follow eyelids (face mesh, Vision, pushed frames) a blink also needs the
lid gap to fall below 90% of its open baseline (full weight at 60%);
closure starts above 0.4 and ends below 0.18; the two eyes are pulled 75%
toward the more open one, because real blinks are bilateral and one-eye
drops are usually glare; nothing closes while the head moves faster than
2.2 eye spans per second.

**Blending.** The stand-in photo is sampled with a small mip bias (0.35) so
it is about as soft as a webcam image, and a faint per-frame grain (±0.6%)
is added to the stand-in so it does not look cleaner than the video around
it.

**Skin tone (2026-10-18).** Skin used to take the full ratio of the caller's
blurred skin colour to the stand-in's (clamped 0.3-3x) and was then clipped
at white. A pale caller wearing a darker stand-in pushed the face up to 3x:
the highlights blew out and the stand-in lost its own complexion; a darker
caller turned a fair stand-in muddy grey. The overall difference is now split
from the local lighting pattern: the pattern (`local / global`, 0.6-1.6) still
follows the caller's light, but only part of the overall gap is applied
(level^`SKIN_LEVEL` 0.4, hue^`SKIN_HUE` 0.6, clamped to `SKIN_RANGE`
0.55-1.8), because a camera's auto exposure already evens out exposure and most of
what remains is complexion. A soft shoulder above `HIGHLIGHT_KNEE` (0.82)
replaces the hard clip. On the test composites a Kofi stand-in on a pale
caller went from the caller's skin (RGB 166/147/141) to 131/107/98, against
111/83/70 in Kofi's photo.

**Halo (2026-10-18).** With no background chosen, a large ellipse around the
head showed a blurred, inpainted plate instead of the real room, to hide the
caller's own head outline. The inpainting smears nearby colours, so every
head carried a dirty-glass ring of brown and green smudges. The blurred
backdrop now carries the background-visibility weight in its alpha, and the
halo only blurs where that weight says the caller's body was
(`HALO_CLEAR` 0.45-0.92); the real background around it stays sharp.

**Pushed frames.** `stage.pushFrame(image, reply, time)` renders a frame
whose landmark reply was computed elsewhere (`{regions, mask}` in the
image's pixels), used by the iOS TrueDepth camera.

**Privacy rule:** when tracking fails the frame is blurred until a face is
found again; raw video is never shown in place of a chosen face.

#### Native face math

Phone calls are moving from the WebView to native libwebrtc
([../server/rtc.md](../server/rtc.md), "Phone media engine"). Backgrounds and
the voice are already native. For faces, everything that is plain maths or data
is now ported line for line, with no new behaviour, so a native renderer
only has to draw. The iOS renderer (Metal) is described in "Native face
renderer (iOS)" below; the Android one (OpenGL ES) is not written yet.

| Platform | Code |
|---|---|
| iOS (Foundation only) | `native/ios/ForkMeshCore/Sources/FMChat/CallFace/` |
| Android `:core` (pure JVM) | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/chat/face/` |

Both ports have the same types and names. Kotlin writes constants in
upper case (`CallFaceGeometry.LANDMARK_COUNT`), uses `ByteArray`/`FloatArray`
for buffers, and makes `CallOutfitPaint` and `CallOutfitCommand` sealed
classes.

| JS | Native |
|---|---|
| `chat-call-face-geometry.js`, all of it | `CallFacePoint` (`x`, `y`, optional `z`), `CallFaceGeometry` (template, `canonicalShape`, `fitTemplate`, `headLayout`, `headModel`, `buildHeadMesh`, Delaunay, `solvePose` with and without depth, `liveHead`, `vertexDepths`, `landmarksFromRegions`, `resamplePolyline`...), `CallFaceOneEuroFilter`, `CallFacePointSmoother`, `CallFaceGroupFilter`, `CallFaceLandmarkSmoother` (`head`, `groups`) |
| `chat-call-faces.js` data and pure parts | `CallFaces` (`all` with points and head rings, `template`, `facePoints`, `head(face)` → layout, model and mesh, cached for the built-in faces, `choices`, `pupils`, `located` = `hostResult`, `outputSize`, hold and fade constants), `CallFaceAnchorSmoother` |
| `chat-call-face-expression.js` | `CallFaceExpression` (`eyeBox`, `tongueWindow`, `sampleBox`, `skinReference`, `measureEye`, `measureTongue`, `eyeOpenness`, `pairClosure`, `closeLids`, `probeCrop`), `CallFaceBlinkTracker`, `CallFaceExpressionProbe` |
| `chat-call-face-mesh.js` pure parts | `CallFaceMesh` (`regions`, `blazeAnchors`, `decodeDetections`, `roiFromDetection`, `roiFromMesh`, `meshPoints`, `sigmoid`, `meshRegions`, `meshToLandmarks`), `CallFaceLetterbox`, `CallFaceRoi` |
| `chat-call-face-custom.js` pure parts | `CallFaceCustom` (`transform`, `apply`, `boxBlur`, `headRing`, `polygons`, `rasterize`, `skinModel`, `skinLikeness`, `personWeights`, `buildMatte`, `matteRGBA`, `validate`, `face`) |
| `chat-call-outfits.js` | `CallOutfits` (catalogue, `frame` = `outfitFrame`, `uv`), `CallOutfitSmoother`, `CallOutfitDrawing` (`commands(id)`, `fabricTile`) |
| `chat-call-face-render.js` pure helpers | `CallFaceWarp` (`warpVertices`, `vertexRegion`, `coverRect`, `hairRegion`, `photoEyes`, `lookUniform`, `axisFrame`, `zoneFrame`) |

**Where the JS needed a cut.** Three places in the JS read or write a canvas
in the middle of the maths, so the native API splits them:

- The expression probe draws the camera into a small canvas and reads it
  back. `CallFaceExpression.probeCrop(points)` returns the crop (`minX`,
  `minY`, `scale`, `width`, `height`). The renderer draws the camera into a
  `width` x `height` RGBA image with `x' = (x - minX) * scale`, and
  `probe.measure(image:crop:points:time:pupils:lids:)` does the rest.
- The custom face fills three polygons on a canvas. `CallFaceCustom.polygons`
  returns them (face, inner face, neck). The renderer rasterises them as
  0/255 bytes, or `rasterize` does it at pixel centres, and `buildMatte`
  takes the bytes. The photo itself is drawn with `transform(points).matrix`
  (a canvas `setTransform` matrix).
- `drawCallOutfit` paints straight on a canvas. `CallOutfitDrawing.commands(id)`
  returns the same drawing as a list of Canvas 2D commands (`save`,
  `moveTo`, `quadTo`, `arc`, `fillStyle(.fabric/.color/.gradient)`,
  `composite("destination-out")`...) in outfit pixels (1024 square, neck at
  512,176, 96 px per unit). The renderer replays them on CoreGraphics or
  `android.graphics.Canvas`. Fabrics are 256 px repeating tiles from
  `fabricTile`.

**Per frame (what the renderer calls).** Once per face:
`head = CallFaces.head(face)` and `eyes = CallFaceWarp.photoEyes(head.layout, photoSize: 512)`.
Keep one `CallFaceLandmarkSmoother()`, one
`CallFaceExpressionProbe` and one `CallOutfitSmoother`, and reset them when no
face has been seen for `CallFaces.holdMs`. Then, for each frame:

1. `located = CallFaces.located(regions, scale:)`. The regions are Vision's
   `[String: [[Double]]]` (the format `CallFaceLandmarks` already returns), in
   the pixels of the analysed image, and `scale` is that image's size over the
   output frame. This gives 68 points in frame pixels, plus pupils.
2. `points = smoother.smooth(located.points, time: ms)`.
3. `reading = probe.measure(...)` as above. Keep the previous reading when it
   returns nil.
4. `drawn = CallFaceExpression.closeLids(points, reading?.close)`.
5. `live = CallFaceGeometry.liveHead(head.model, drawn)`,
   `depths = CallFaceGeometry.vertexDepths(head.model, live.pose)`, and
   `CallFaceWarp.warpVertices(mesh: head.mesh, layout: head.layout, live: live.points, photoSize: 512, depths: depths)`.
   The last call gives the triangle-list vertex buffer for the warp pass:
   7 floats per vertex (frame x, frame y, photo u, photo v, hole, region,
   depth).
6. Uniforms come from `CallFaceWarp.zoneFrame(drawn, live.pose)`,
   `hairRegion(live.pose, width:height:)` (the hair matte's crop),
   `lookUniform(reading.look)`, `reading.close`, `reading.skinRed`, and
   `axisFrame(CallFaceExpression.tongueWindow(drawn))` with `reading.tongue`.
7. For the outfit, `outfitSmoother.smooth(CallOutfits.frame(points))` gives
   the garment's centre, axes and unit. `CallOutfits.uv` maps a frame pixel
   into the garment texture.

Android has no Vision, so its landmarks come from the bundled detector and
mesh networks once something runs them. The decode steps are
`decodeDetections` → `roiFromDetection(_, CallFaceLetterbox(width:height:))`,
then crop and run the mesh network, `meshPoints(raw:roi:)`, the face score
`sigmoid(flag, limit: 60) >= CallFaceMesh.faceScore`, `roiFromMesh` for the
next frame's crop, and `meshToLandmarks`.

**Left for the renderer.** Everything in `chat-call-face-render.js` except the
helpers above: the warp, composite, zone, blur, fill and backdrop shaders,
tone and skin matching, and texture uploads. Also: the person and hair mattes
(the networks in `chat-call-segment.js` and the guided filter), running the
detector and mesh networks, `createFaceStage`'s frame loop (hold, fades,
adaptive quality), drawing the outfit commands and their thumbnails, and
loading photos and mattes. iOS now has all of this, including the hair
network and the guided filter (see below); Android still has none of it.

**Not ported:**

- `chat-call-face-tracker.js`, the heuristic tracker for browsers without the
  face mesh. Vision and the mesh networks replace it.
- Everything that is only DOM, storage or network:
  - `canvasGrabber` and `createHostLocator`;
  - `hostReply`'s base64 mask decode (native code has the mask bytes already);
  - `callFacesSupported`;
  - the `FmCallFacePhotos` override (`photoPath` and `mattePath` give the
    plain asset paths);
  - `savedCall*` and `remember*` (localStorage);
  - `customFaceFromFile` and `resizeMask`;
  - `createFaceMesh`'s crop shader and texture read-back.

**Keeping the JS numbers.** Both ports keep the order of every floating-point
expression. Five other behaviours from the JS are kept too:

- V8's `Math.hypot` normalises by the largest argument and sums with Kahan
  compensation, so it differs from `sqrt(x*x + y*y)` in the last bit about
  a third of the time. The ports reimplement it (`CallFaceJS.hypot`),
  because span, frame and Delaunay inputs feed discrete decisions.
- Sorts are stable, ties included.
- `Math.min`/`max` propagate NaN.
- `Math.round` rounds halves up.
- `Uint8ClampedArray` stores round half to even.

Float32 storage is kept wherever the JS uses `Float32Array`: the eye grid,
the blurs and mattes, and the warp buffer. Float32 values are compared with
double constants, as JS does. The weight maps keep insertion order, and so do
the Delaunay edge map and `Object.entries` (integer keys ascending). Kotlin
uses `StrictMath`.

**Parity fixture.** `tools/call_face_parity.mjs` imports the JS modules from
`app/public` in node and stubs only `document`.

- The probe's canvas paints a synthetic face image (skin plus integer noise,
  with ellipses for sclera, iris, closed lids, the mouth and a tongue) in
  world coordinates.
- The custom face's canvas fills polygons at pixel centres.
- The outfit canvas records every call.

It records the outputs below into
`native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-face-parity.json`
and a byte-identical
`native/android/core/src/test/resources/chat/call-face-parity.json`
(1.2 MB). Numbers are rounded to 1e-8, and large buffers are stored as
length, sum, samples and an FNV-1a hash.

- **Catalogue:** the faces, choices and outfits, and the template.
- **Each built-in face:** canonical points, layout, model, mesh (triangles,
  holes, mouth and cull flags) and photo eyes.
- **A Delaunay set.**
- **Twelve static cases:** neutral, turned left and right, tilted, nodding,
  mouth open, eyes closed, wink and smile, two with depth, `medianLine`
  instead of `noseCrest` with the jaw reversed, and a small face far away.
  Each records points, frame, pose, live head, depths, warp buffer, eye boxes,
  openness, tongue window, closed lids, outfit frame and UV, and template fit.
  The landmarks are Vision-style regions resampled from a posed 3D template.
- **Every face on a flat and a depth input.**
- **A 60-frame sequence** through the stage's pipeline: host result, point
  smoother, probe, lids, live head, warp, outfit smoother, anchor smoother
  and fit. It moves and blinks (both eyes, then a wink), opens the mouth,
  shows the tongue, drops one frame, repeats one timestamp and drops the
  yaw on some frames.
- **The One Euro and blink filters on their own.**
- **Detection decoding** on xorshift inputs.
- **A 468-point mesh** through `meshToLandmarks`.
- **The custom face:** transform, skin model, likeness, and the matte with
  and without a person mask, plus its head mesh.
- **Every outfit's recorded canvas commands and all 16 fabric tiles.**

The inputs are stored in the fixture, or rebuilt in the tests from integer
generators.

`CallFaceParityTests` (Swift) and `CallFaceParityTest` (Kotlin) check
every number to within 1e-6 relative (`|a - b| <= 1e-6 * max(1, |b|)`).
Mesh indices, flags, strings and command names must match exactly, and tile
bytes within 1. Each test prints a summary line. Measured on 2026-10-21, both
ports land on the fixture's rounding floor in every section: a worst relative
error of 5e-9. Every buffer hash is bit-identical to the JS on both:

- 82 warp buffers;
- 8 custom matte buffers;
- 16 fabric tiles.

Swift was measured on Linux with glibc. Apple's libm has not been measured
yet, and the tolerance leaves room for it.

`app/tests/test_call_face_native_parity.py` regenerates the fixture in a
temporary directory. It fails if any number differs from the committed copy
by more than 1e-7, or if the two copies differ. A change to any of these JS
modules therefore fails that test until the fixture is regenerated and both
ports follow:

```sh
node tools/call_face_parity.mjs \
  native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-face-parity.json \
  native/android/core/src/test/resources/chat/call-face-parity.json
```

Then run `FM_ONLY=FMChat swift test --filter CallFace` from
`native/ios/ForkMeshCore`, and `./gradlew --offline :core:coreTest -Pfilter=CallFace`
from `native/android`.

#### Native face renderer (iOS)

The native call engine draws the face changer itself, so a face picked in the
call's look picker shows on the native engine as it does in the browser.

| Piece | Code |
|---|---|
| Frame loop (hold, fades, smoothing, probe, outfit placement) and the CPU half of `draw()` | `CallFaceStage`, `CallFaceDrawPlan`, `CallFaceFrameMap` in `native/ios/ForkMeshCore/Sources/FMChat/CallFace/CallFaceStage.swift` (Linux tests in `CallFaceStageTests`) |
| Shaders, ported from `chat-call-face-render.js` | `native/ios/ForkMesh/Features/Chat/Calls/Native/NativeFaceRenderer.metal` |
| Landmarks, GPU passes, buffers | `NativeFaceRenderer.swift` (same folder) |
| Stand-in photos, the custom face, outfit textures | `NativeFaceAssets.swift` (same folder) |
| Stand-in photos and mattes | `native/ios/ForkMesh/Resources/CallFaces/` (copies of `app/public/assets/call-faces/`) |
| Segmentation network runner, hair matte, guided person matte (Metal) | `NativeFaceSegment.swift`, `NativeFaceSegment.metal` (same folder); model parsing and plan in `FMChat/CallFace/CallSegmentModel.swift` (Linux tests in `CallSegmentTests`) |
| Matte sizes, quality levels, blend shapes, mesh binding, capture policy | `CallFaceMatte.swift`, `CallFaceQuality.swift`, `CallFaceTracking.swift` in `FMChat/CallFace` (Linux tests in `CallFaceTrackingTests`) |
| ARKit face-tracking camera | `NativeFaceTracking.swift`; switching in `NativeCameraPipeline` (`NativeCallMedia.swift`) |
| Hair network weights | `native/ios/ForkMesh/Resources/CallSegmentation/hair-192.fmnn` (copy of `app/public/assets/call-segmentation/hair-192.fmnn`) |

**Wiring.** `NativeCallMedia.follow(look)` passes the look's face and custom
photo to `NativeLookFilter.apply(face:customImage:)`. For each camera frame
`NativeLookFilter` runs the face first and the background second, as the
browser does:

1. If a face or an outfit is chosen, `NativeFaceRenderer.render` draws it and
   returns the new frame plus a full-size "keep" mask (the person after the
   head swap: the person matte minus the removed head zone, plus the drawn
   stand-in).
2. If a picture background is chosen, the existing Core Image step replaces
   the background of that frame, using the keep mask instead of Vision's mask.
   A blur background is drawn by the face pass itself (see "Blur backgrounds"
   below) and Core Image is skipped; with no face or outfit drawn, Core Image
   blurs as before.
   Without the keep mask the stand-in's hair outside the caller's own outline
   would be cut away. The person segmentation runs at most once per frame (or
   every 2nd or 3rd frame at the lower quality levels) and is shared by both
   steps.

With no face and no outfit, the filter does no extra work. Outfits are wired
(`NativeLookFilter.apply(outfit:)`), but calls do not offer outfits yet, as in
the browser, so nothing calls it.

**Coordinates.** The camera buffer arrives in sensor orientation (NV12 or
BGRA) with an `RTCVideoRotation`. Vision runs with the matching orientation
(`.right` for 90 degrees), so landmarks and all the face maths are in upright
pixels, as in the browser. The GPU works in buffer pixels: the warp vertex
shader maps upright points into the buffer, and the composite maps each
buffer pixel back to upright pixels for the zone, tongue, grain and outfit
maths (`CallFaceFrameMap.toBufferMatrix` / `toUprightMatrix`). The output is
a BGRA buffer the size of the input with the same rotation. The low-resolution
targets are sized from the upright width (160 and 192 px), as in the browser.

**Passes** (one command buffer per frame, one wait at the end):

1. Camera: NV12 (601 or 709 matrix, full or video range, read from the
   buffer) or BGRA into an RGBA8 texture, zero-copy through
   `CVMetalTextureCache`.
2. Warp: the head mesh into the warp and aux targets in one pass with two
   colour attachments and a depth buffer (the browser draws twice), with the
   gaze, eyelid and mip-bias maths unchanged.
3. Tone: reduce plus Gaussian blur into the low camera, low face and wide
   camera/face targets (RGBA16F).
4. Person matte: temporal, seed, 8 grow rounds, clean and the guided filter
   (below), when a face, outfit or background needs it.
5. Hair matte: crop, the hair network (162 compute dispatches) and the frame
   pass, when a face is shown (below).
6. Fill: weigh, push and pull pyramid (6 levels) for the room behind the
   caller, weighed by the guided matte.
7. Halo (no background chosen) or blur backdrop (blur chosen): the blurred
   room from pyramid level 1.
8. Hide: the blurred plain frame, only while hiding.
9. Composite: the browser's composite in mode "off" or "blur" (a picture
   background comes after), with the guided matte, the hair zone and the
   hair-aware collar, writing the frame and the keep mask.

**Threading.** Faces, custom faces and outfits are built on a loader queue;
frames pass through until the Metal device and pipelines exist (they are made
on the first face or outfit choice). Landmarks run on the frame's thread while
Vision takes under 16 ms on average (exponential average, the first two runs
ignored as warm-up). Above that they move to their own queue: at most one
detection in flight, frames in between reuse the last points without
re-smoothing (`CallFaceStageInput.stale`), and the renderer returns to
inline detection below 10 ms. The probe crop is read from the analysed buffer
on the CPU (bilinear, at most about 120 x 150 px), so it always matches the
points it measures.

**Fallbacks.** A missing Metal device, library or pipeline, an unsupported
pixel format, a failed encoder or a command buffer that does not complete
returns the plain frame. A failed segmentation draws the stand-in without
head removal. A face that is chosen but not found, or still loading, follows
the browser's privacy rule: the frame fades to the blurred plain frame
(`hide`), not to the raw camera.

**Cost** (before the hair network and guided matte; see "Parity" below for
those). Per frame: one Vision landmark request, one person segmentation (it
also feeds the background), about 40 small render passes, and one full-size
composite. Textures, depth, vertex buffer, pixel-buffer pools and pipeline
states are kept and only rebuilt when the frame size or rotation changes. The
per-frame allocations left are the arrays the shared maths API returns
(`warpVertices` and the landmark points), Vision's own objects, and Metal's
encoders. Checked on 2026-10-21 on the office Mac (Intel Iris Plus 650,
640x480, under a heavy build): the GPU passes take about 4 ms per frame,
Vision landmarks about 75 ms and segmentation about 130 ms on its CPU. Not yet
measured on an iPhone, where Vision runs on the Neural Engine. Building a
custom face on that Mac takes about 0.9 s once Vision's models are loaded
(0.74 s Vision accurate, 0.16 s `buildMatte`) and 8.5 s the first time in a
process; it runs on the loader queue.

**Beyond the browser (2026-10-21).** The iPhone now does everything the
browser does to the matte and the head zone, and more on phones with a
TrueDepth front camera:

| | Browser | iPhone |
|---|---|---|
| Hair matte | `hair-192` network in WebGL | the same network and weights as Metal compute kernels |
| Person matte | selfie network or host mask, guided filter | Vision's person mask (a larger model than the selfie network), the same temporal, region and guided-filter passes |
| Head pose | 2D landmarks (3D with the WebView's depth path) | TrueDepth: ARKit's face mesh gives every landmark a measured depth, so `solvePose` runs its 3D path every frame |
| Eyes, blinks, tongue | pixel probe | ARKit blend shapes (`eyeBlink*`, `eyeLook*`, `tongueOut`) |
| Matte edge | colour guided filter | colour guided filter plus the depth map (TrueDepth) |
| Blur background | blur of the reconstructed room | the same, inside the face pass |
| Quality levels | by frame cost | by frame cost, device class and thermal state; capture rate and encode size step down when hot |

**Segmentation networks** (`CallSegmentModel.swift`, `CallSegmentReference.swift`
in `FMChat/CallFace`, `NativeFaceSegment.swift` and `.metal` in the app):

- `CallSegmentModel.parse` reads the browser's `.fmnn` files (magic, JSON
  header, half-float weights) and expands `mean` into the row and column
  passes as `expandLayers` does. `CallSegmentPlan` lays every tensor out in one
  NHWC half-float arena (offsets aligned to 64 values) and packs each layer's
  parameters into 32 integers.
- `fmSegLayer` is one compute kernel for every layer op the networks use (conv,
  dilated conv, transposed conv, depthwise, mean, add/mul with broadcast,
  resize, argmax, unpool, concat, pad, max pool) and every activation, one
  thread per output value. It keeps the WebGL shader's arithmetic: half-float
  storage between layers, float accumulation, the input channels summed four
  at a time, `exp` clamped at ±30. Each layer is one dispatch in one serial
  compute encoder.
- `CallSegmentReference` is the same maths on the CPU, used only by the tests.
- `hair-192.fmnn` is bundled in `Resources/CallSegmentation/` (byte-identical
  to `app/public/assets/call-segmentation/`, checked by `CallSegmentTests`) with
  its licence. It loads on the face loader queue the first time a face is
  chosen.

**Hair matte** (`NativeHairMatte`): `fmHairCrop` fills the network input from
the camera (3x3 box per input pixel, the previous matte as the fourth channel)
inside `CallFaceWarp.hairRegion`, now part of `CallFaceDrawPlan`; the network
runs; `fmHairFrame` turns the logits into a hair probability and blends it with
the previous frame, as `createHairMatte` does (3 warm rounds on a fresh matte,
rate 0.5). The matte is kept upright (`CallFaceMatte.hairSize`: at most 256
pixels wide), so the composite reads it in upright pixels whatever the buffer
rotation. The composite uses it as the browser does: the head zone becomes the
face oval plus the dilated hair (`uHasHair` 1), and an outfit stays behind the
hair at the collar. With TrueDepth, hair more than 0.25-0.55 m behind the face
is dropped.

**Person matte** (`NativePersonMatte`): `createMatte`'s host-mask path, pass for
pass: temporal hold (0.55), seeds from the bottom band and the face circle,
8 grow rounds on a 64-pixel-wide grid, clean, then the guided filter (moments,
radius 4 box, `eps` 0.0012, coefficients, box) at the quality level's guide
width. The composite and the room fill use `MATTE_SAMPLE`'s blend of the hard
mask and the guided value, so the edge follows hair and colour edges instead
of Vision's soft mask, and the keep mask handed to Core Image for picture
backgrounds has the same edge. The passes run in buffer orientation; the seed
maps each texel to upright coordinates (`CallFaceFrameMap.toUprightUvMatrix`)
so the bottom band is always below the caller.

**Depth fusion (TrueDepth).** When an ARKit depth map is less than 150 ms old,
`fmMatteFuse` moves Vision's mask 60% of the way towards "nearer than 0.35-0.7 m
behind the face" wherever the depth is valid (0.1-5 m); holes (dark hair, edges)
keep Vision's value. The guided filter then snaps the fused edge to colour
edges. The depth map is assumed to cover the same field of view as the image,
or its middle when the aspect differs (`CallFaceMatte.depthUv`).

**TrueDepth tracking** (`NativeFaceTracking.swift`, `CallFaceTracking.swift`):

- Only while a face is chosen, on a device where
  `ARFaceTrackingConfiguration.isSupported`, with camera access and Eye Control
  off, `NativeCameraPipeline` stops `RTCCameraVideoCapturer` and runs an
  `ARSession` instead. `ARFrame.capturedImage` (sensor orientation, not
  mirrored, like the WebRTC capturer's buffers) goes to the same
  `RTCVideoSource` with the rotation `RTCCameraVideoCapturer` would use for the
  device orientation (`CallFaceCapture.rotation`). The video format is the one
  whose rate is closest above the call's and whose size covers it
  (`CallFaceCapture.pick`); a 60 fps format is halved by `CallFaceFrameGate`.
- Each frame carries the 1220 mesh vertices projected into the image
  (`ARCamera.projectPoint`, `.landscapeRight`, image size) with their camera
  distance, the triangle list, the blend shapes, `fx`, the face distance and
  the latest depth map.
- Landmarks: the mesh has no 68-point labels, so `CallFaceMeshBinding` learns
  them. The first three Vision results are bound to the mesh (each landmark to
  the triangle most frames chose, with averaged barycentric weights, kept only
  if the mean error is under 0.1 eye spans), after which landmarks come from
  the mesh every frame with no Vision, each with `z` from the vertex depths
  (`CallFaceDepth.z`). Vision runs again every 2 s on its own queue to check
  and refine the binding; a binding that keeps failing (for example a mirrored
  projection) is dropped after 3 misses and the renderer stays on Vision
  landmarks while still using the blend shapes.
- Expression: `CallFaceBlendMapping` maps `eyeBlink*` to the eyelid closure
  (0.2-0.75), `eyeLook*` to the gaze (±`lookLimit`) and `tongueOut` to the
  tongue (0.15-0.5). Image-left eye (points 36-41) is the caller's right eye.
  The probe still supplies the skin colour. Mouth shapes (`jawOpen`,
  `mouthSmile*`, `cheekPuff`) arrive through the mesh landmarks.
- Fallbacks: no frame within 1.5 s of starting, an `ARSession` error, or a
  device without TrueDepth (and the simulator) keep or return to the WebRTC
  camera and the Vision path for the rest of that call. An interruption re-runs
  the session when it ends. A frame where ARKit has no face falls back to
  Vision, and a face that is found by neither still fades to the blurred plain
  frame.
- Info.plist and entitlements: nothing new. ARKit face tracking needs only
  `NSCameraUsageDescription`, which exists (its text does not mention calls).
  Do not add `arkit` to `UIRequiredDeviceCapabilities`: phones without it must
  still install the app. App Review asks apps that use face tracking to say in
  their privacy policy that face data stays on the device.

**Quality levels** (`CallFaceQuality`, `CallFaceQualityGovernor`). Levels as in
the browser: segmentation every 1, 1, 2, 3 frames, hair network every 1, 2,
3, 4 frames, guide width 320, 320, 256, 192, and Vision's `.fast`
segmentation from level 2. The start level comes from the GPU family (Apple 7
and later 0, Apple 6 1, Apple 5 2, anything else 3). The level steps up when
the frame's cost (render plus segmentation) averages over 28 ms and down under
16 ms, after 24 steady frames. `.serious` thermal state holds it at 2 or
above and `.critical` at 3; while a look is active the pipeline also caps the
capture rate at 24 fps and the encode size at 75% (`.serious`), or 15 fps and
50% (`.critical`), through `RTCVideoSource.adaptOutputFormat`.

**Blur backgrounds** are now drawn in the composite whenever the face pass
draws (mode 1): the blur comes from the room fill with the caller removed
(`buildBlurBackdrop`, strength 0.4 or 1), so the caller never smears into it,
and Core Image is skipped. Picture backgrounds still go through Core Image with
the keep mask.

**Still different from the browser:**

- The person matte's source is Vision's mask rather than the selfie network
  (the network runs natively too; it is not used because Vision's mask is
  better and runs on the Neural Engine).
- Picture backgrounds are composited by Core Image after the face pass.
- The custom face uses Vision's full-resolution person mask instead of the
  256 px one the WebView gets.

**Parity.** `tools/call_segment_parity.mjs` runs the browser's own
`createNetwork`, `createHairMatte` and `createMatte` in headless Chromium
(WebGL 1 on SwiftShader, half-float targets, through `playwright-core` from
`app/browser_tests/node_modules`) and writes
`native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-segment-parity.json`:
the inputs (stand-in photos over the office background, the hair input with a
previous matte from a first run), the hair and square networks' outputs as
half floats with a sum per layer, three hair-matte steps and two person-matte
steps. The page must enable `OES_texture_half_float_linear`, as the renderer
does, or every linearly sampled matte target reads black. Regenerating gives
byte-identical output:

```sh
node tools/call_segment_parity.mjs native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-segment-parity.json
```

Measured on 2026-10-21:

- `CallSegmentTests` (Linux): the CPU reference matches the browser's hair
  network bit for bit (every logit), the selfie network to 0.012 at worst
  (mean 3e-5).
- macOS harness on the office Mac (Intel Iris Plus 650, built from the same
  Swift and Metal files): `fmSegLayer` matches the browser's hair network bit
  for bit and the selfie network to 0.027 at worst (mean 3e-5); the hair matte
  to 0.06-0.08/255 on average (worst 19-24/255 in a few edge pixels, from
  8-bit bilinear sampling) at 0, 90 and 270 degrees; the person matte's clean
  mask to 0.0007 at worst and the guided alpha to 0.0004 on average (worst
  0.028), at 0 and 90 degrees.
- Costs on that Mac (loaded by other builds): hair network 10-12 ms GPU per
  run (36.5 M multiply-adds, 162 dispatches), selfie network 13 ms, person
  matte 0.3-0.45 ms GPU, a fresh hair matte (3 rounds) 30 ms and a normal
  update 10 ms; a whole 640x480 frame 7-15 ms median at level 3 with a cached
  mask. Vision on its CPU: segmentation 200 ms `.balanced` and 52 ms `.fast`,
  landmarks 82 ms. With a synthetic mesh in place of ARKit the renderer binds
  after the first Vision results and then draws at 9-10 ms a frame with no
  per-frame Vision.
- Not measured: anything on an iPhone. ARKit face tracking, the projection of
  the mesh into the front camera image, blend-shape sides, the depth map's
  field of view, and the costs on the Neural Engine and Apple GPUs need a
  TrueDepth iPhone.

Checked on the office Mac with the earlier harness too: stand-ins on 0, 90 and
270 degree buffers, NV12 and BGRA, a custom face, outfits on a drawn torso, and
the hide path with no face.

### Backgrounds

No background, Blur, Strong blur, built-in images
(`app/public/assets/call-backgrounds/`, rendered in code by
`app/tools/render_call_backgrounds.cjs`), or a custom photo. Blur weights only
room pixels so the person never bleeds in. Choices are saved in
`localStorage` (`forkmesh.chat.callFace`, `forkmesh.chat.callFaceImage`,
`forkmesh.chat.callBackground`, `forkmesh.chat.callBackgroundImage`) or app
preferences (`chat.callFace`, `chat.callBackground`).

**Portrait.** Each built-in image has a portrait render
(`<stem>-portrait.jpg`, 720x1280) beside the 1920x1080 one. The stage uses it
whenever the picture is taller than wide (`isPortraitFrame`: height above
1.05x width), which is how phones and turned tablets send video; covering a
16:9 image into a 9:16 frame kept only its middle third and cut signs and
windows in half. The renders come from the same scenes with a portrait camera
(`render_call_backgrounds.cjs --portrait`): horizontal field of view 0.8x the
landscape's vertical one, aimed per scene where needed (the office turns left
to keep the sign in view, the World turns 16 degrees instead of 24). 720x1280
is enough because the stage never draws more than 960 pixels on a side, and
keeps all fifteen files under the 1.5 MB budget (portrait adds about 400 KB;
the native call pages, which inline them, grow by about 570 KB). When
the camera turns mid-call the stage loads the other image and keeps the old
one on screen until it arrives. "Your photo" is covered into either shape.

### Outfits

Your own clothes, Suit and tie, Hoodie, Cable-knit sweater, Denim jacket,
Flannel shirt and Turtleneck (`chat-call-outfits.js`). Each garment is drawn
in code onto a 1024x1024 canvas (fabric tiles from per-pixel noise, then
lapels, collars, pockets, cords and buttons), so there are no image files and
the native pages need nothing extra. Garment space is measured in eye spans
(the distance between the eye centres, 96 texels each) from the base of the
neck.

**Placement.** `outfitFrame(points)` puts the neck base 1.35 spans below the
chin, horizontally halfway between the chin and the eye midpoint. The unit is
the larger of the eye span and the eye-to-chin distance / 1.9, so turning the
head (which shrinks the eye span) does not shrink the clothes. Shoulders roll
at 0.3x the head's roll, because people tilt their heads without tilting
their shoulders. `createOutfitSmoother` eases centre, size and roll (0.35 per
frame) and resets when the face is lost or the frame size changes.

**Compositing** happens in the main composite pass, on the person colour
(`front`) before the background is added, and only with a person matte:

- The garment texture is uploaded premultiplied, so its edges filter cleanly.
  Its alpha is the garment shape; the neck opening and everything above the
  shoulder line are cut out. The person matte clips it to your actual
  outline, so one shape fits every build.
- Shading comes from the camera: the pixel's brightness over the mean of
  eight taps 0.35 spans away, raised to 0.4 and clamped to 0.75-1.12. Folds
  and shadows carry over; a printed pattern on the real shirt shows only as a
  faint ghost.
- Hands stay in front: below 1.2-1.9 spans under the neck base, pixels whose
  chroma is within 0.035-0.07 of the face's (five taps on the cheeks and nose)
  and whose brightness is 0.45-2x the face's keep the camera. Hair from the
  hair matte (when a stand-in face is on) and the head zone are kept as well.
- The outfit fades in and out with tracking (the same 0.3/0.2 fade as faces)
  and then shows your own clothes, not a blur: clothing is not the identity
  the privacy rule protects.

An outfit alone (no stand-in face) still runs the face tracker and the person
matte (`wantsPoints`, `wantsMatte`), and asks the host for landmarks on iOS.
The composite uses 14 texture units, the `MIN_UNITS` floor that the
background picker already requires. The Face Changer page has the picker
(saved under `forkmesh.chat.callOutfit`); calls do not offer outfits yet, and
`createFaceStage` defaults to none.

Checked on head-and-shoulders test frames (a call-face photo and its matte
over a drawn torso with a striped shirt and a hand) through
`call_video_harness.cjs render --outfit ID`, in landscape and portrait, with
and without a background. Necklines are sized for a real neck (about 1.75
spans wide, crew neckline about 1.5 spans under the chin); with a short neck
the old collar shows inside the opening.

### Models

`app/public/assets/call-segmentation/*.fmnn`: selfie segmentation (landscape
and square), face detector, face mesh, and hair segmentation, all Apache 2.0
(licence text in `LICENSE` beside them). `.fmnn` is a JSON graph plus float16
weights, produced from `.tflite` by
`app/tools/build_call_segmentation_model.py`. `chat-call-segment.js` runs one
fragment-shader draw per layer on half-float textures; without
`OES_texture_half_float` and 12 texture units the background picker is
hidden and faces use the fallback tracker. On iOS, Apple Vision supplies
landmarks and the person mask; the hair model still runs in the page.

**Adaptive quality** steps between four levels (how often the selfie and hair
models run, guide resolution) based on per-frame time, stepping down above
34 ms and up below 20 ms.

### Voices

The microphone track goes through an `AudioWorklet` after the browser's echo
cancellation, and the processed track replaces the audio sender's track. If
the worklet cannot start, the plain microphone is sent. The engine is a port
of the desktop voice changer: high-pass and light denoise, an STFT formant
(envelope) warp, PSOLA pitch shifting anchored on glottal pulses, a McLeod
pitch tracker with hysteresis and an octave guard, and a look-ahead limiter.
Added latency is 1728 samples (36 ms at 48 kHz).

How the engine keeps a changed voice natural (2026-10 realism pass, judged by
spectrograms of piper speech and by inter-harmonic noise on a steady glottal
vowel, where the dry signal sits near -54 dB re total):

- **Sub-sample pulse marks and grain placement.** Marks were whole samples and
  grains were dropped on rounded output positions, so every pulse carried up
  to half a sample of timing error. That jitter smeared the partials into a
  rough, hoarse noise at -20 to -30 dB between the harmonics, even at a pitch
  ratio near 1. Marks are now refined to a fraction of a sample with a parabola
  on a full-rate correlation, and each grain is read with 4-point cubic
  interpolation and a fractional Hann window. A steady vowel now stays at -40
  to -64 dB.
- **Peak envelope.** The formant envelope used to be the cepstral mean of the
  log spectrum. That mean follows the valleys between harmonics, which move
  with the frame's position against the pulses, so the warp gain changed from
  frame to frame and added another -31 to -37 dB of noise. The log spectrum is
  now peak-held over half a pitch period before the cepstral lifter, and the
  envelope glides with a 6 ms time constant, so it runs through the harmonic
  peaks and stays still. Updating it only every other frame was tried and
  rejected: it aliases the pitch-rate wobble into sidebands between the
  harmonics.
- **Warp only where the vocal tract decides.** The tract scale applies in
  full up to 5 kHz, then eases back to no warp by 9 kHz. Unvoiced frames (s,
  sh, f) get 40% of the warp, following a voicing value that glides over about
  10 ms. Fricatives now move 4-9% where the warp is 20-25%, so a lowered voice
  no longer turns "s" into "sh", and the top band no longer empties out.
  Unvoiced sound was already left at its own pitch.
- **Breath after the pitch shift.** Breath noise used to be added inside the
  STFT before PSOLA. Grains repeated it, so it came out comb-filtered at the
  new pitch, and at about -15 dB relative it could not be heard. The engine
  now synthesises noise shaped by the warped envelope and tilt (rising from
  800 Hz to full at 3.8 kHz) on a second OLA path at half the frame rate. It
  delays that noise by the shifter latency, modulates it once per output pulse
  (peak a quarter period before each pulse, 70% depth), and adds it after
  PSOLA. At breath 0.1 the noise between harmonics in 2-5 kHz rises about
  20 dB, while 100-1000 Hz stays below -45 dB.

Latency is unchanged. In node on the development box, CPU per 128-sample
quantum went from about 270 to 300 µs (11% of real time) for voices without
breath, and to 320 µs (12%) with breath. The extra cost is the second
inverse FFT, which runs every other frame. The female-to-deep cases are still
the noisiest, at about -42 dB. What remains there is time aliasing of the
STFT warp, which a shorter lifter would fix only by blurring formant
positions.

- **Full-period mark correlation (2026-10-18).** Each pulse mark was placed by
  correlating half a period on each side of the last mark. On recorded
  speech (CMU ARCTIC, three male and two female speakers) mark spacing jumped
  0.45-0.57 semitones from one cycle to the next for male voices (about 3%,
  where a healthy voice is under 1%), and grains are spaced by the mark
  period, so the output carried 1.6-3.4% cycle-to-cycle jitter and sounded
  hoarse. The window is now a full period on each side: mark jitter roughly
  halves for male voices, output jitter drops about 30% (Maya from a male
  voice 1.57% to 1.14%, Theo 2.50% to 1.72%, Kofi 3.39% to 2.56%), the
  periodicity of female-to-Kofi rises from 0.965 to 0.977 and the spectral
  distance of the female targets falls (for example 3.26 to 2.87 dB). Smoothing the
  period across neighbouring marks was tried and rejected: an exponential
  glide lagged pitch glides (half-semitone accuracy fell from 95% to 82%),
  and a centred average rarely has the next mark in time without more
  latency.

`test_chat_call_voice_realism.py` checks each of these points. On steady
vowels it measures inter-harmonic noise (at most -40 dB), the harmonic ratio
against the planned ratio, and the second formant against the planned tract
scale. It also checks how far a fricative's centroid moves, how much breath
noise lands between the harmonics, and voiced/unvoiced boundaries against a
natural voice synthesised at the target pitch and size.

Each voice in `CALL_VOICES` is a target median pitch, a vocal-tract scale,
brightness, breath and optional `range`, `jitter` and `tremor` (clamped by
`VOICE_LIMITS`). The pitch ratio is `target / callerMedian`, where the
caller's median is tracked continuously and saved in
`forkmesh.chat.callVoicePitch`. The choice ("Match face", "Your own voice" or a
voice id) is stored in `forkmesh.chat.callVoice` / `chat.callVoice`; a face
without a matching voice id gets the caller's own voice.

#### Native voice processor

Phone calls are moving from the WebView to native libwebrtc
([../server/rtc.md](../server/rtc.md), "Phone media engine"), so the voice
changer has to run on the captured 48 kHz mono PCM before it reaches the
encoder. The engine is ported line for line, with no new behaviour:

| Platform | Code |
|---|---|
| iOS (Foundation only) | `native/ios/ForkMeshCore/Sources/FMChat/CallVoice/`: `CallVoiceProcessor`, `CallVoiceCatalog` |
| Android `:core` (pure JVM) | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/chat/voice/`: `CallVoiceProcessor`, `CallVoiceCatalog` |

Each port has the same pieces as `chat-call-voice-worklet.js`: the radix-2 FFT
and packed real FFT, the McLeod pitch detector, the PSOLA `PitchShifter`, the
look-ahead `PeakLimiter` and `VoiceEngine` (high-pass, denoise, peak envelope,
warp, breath path, AGC). `CallVoiceCatalog` carries `CALL_VOICES`,
`VOICE_LIMITS`, `normalizeVoiceChoice`, `resolveCallVoice` (match face / off /
a voice id), the labels and the choice list. The order of every floating-point
expression is kept, and so are the two xorshift32 generators with their seeds
(`0x6d2b79f5` for grain jitter, `0x2545f491` for breath phases), so the output
is deterministic. JS `Math.round` rounds halves up, so both ports use
`floor(x)` plus one when the remainder is at least 0.5.

The API is `CallVoiceProcessor(sampleRate, userPitchHz)`, then `setVoice(id)`
(an unknown id or `""` is the caller's own voice), `setUserPitch(hz)`,
`setNoiseReduction(amount)` and `process(samples)` in place
(`UnsafeMutableBufferPointer<Float>`, `inout [Float]` or a pointer and count on
iOS; `FloatArray` with offset and count on Android). `latencySamples` is 1728
at 48 kHz, as on the web. Every buffer is allocated in the initializer (raw
pointers on iOS so there is no copy-on-write), and `process` allocates nothing.
Setters only store a request: Android swaps it through an `AtomicReference`,
iOS behind an `NSLock` that `process` takes with `try()`, so the audio thread
never waits. A request made between two `process` calls applies at the first
sample of the next one, which is what the worklet's message port does. Unlike
the worklet, a non-finite input sample is read as 0, because one NaN would
otherwise stick in the AGC and silence the rest of the call.

`userPitchHz` (the tracked caller median, to save in `chat.callVoicePitch`
like the web's stats message) and `pitchHz` are published at the end of each
`process` call.

**Parity fixture.** `tools/call_voice_parity.mjs` runs the JS engine in node on
one second of 48 kHz input: a 220 to 180 Hz sawtooth through a one-pole
low-pass, a noise burst, then a 125 Hz sawtooth, over a light noise floor. The
generator uses only arithmetic and xorshift32, so Swift and Kotlin rebuild the
same Float32 input exactly. It records every voice, "off", `maya` seeded with
a 120 Hz caller, and three switches in the middle of the stream
(`maya`→`kofi`, `off`→`teen`, `robot`→`off`). For each case it stores the RMS
of every 10 ms block, every 149th sample (rounded to 1e-8), the caller pitch
it ends on, and an FNV-1a hash of the Float32 output bits. The fixture has two
identical copies:
`native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-voice-parity.json`
(read through `#filePath`) and
`native/android/core/src/test/resources/chat/call-voice-parity.json`.

`CallVoiceProcessorTests` (Swift) and `CallVoiceProcessorTest` (Kotlin) check:

- samples within 1e-6 absolute, block RMS within 1e-5 relative and the final
  caller pitch within 1e-6 Hz, for every case;
- "off" is the input delayed by exactly 1728 samples, bit for bit;
- a switch in the middle of the stream never steps further between neighbouring
  samples than 1.2 times the largest step of either voice running alone;
- 128, 480, 960 and 1024-sample buffers give exactly the output of one long
  buffer;
- a NaN or infinite input sample leaves the output finite;
- the catalogue matches the JS voices field by field.

Measured on 2026-10-21, both ports land on the fixture's rounding floor: a
worst sample error of 5e-9 and a worst relative RMS error of 5e-6. The hash
shows how many cases come out bit for bit. Kotlin uses `StrictMath` (fdlibm,
the same as V8's `Math`), and 17 of 19 cases are bit-identical. Kofi and Young
man differ in the last bits because the JVM has no fdlibm `log2`, so the port
uses `log(x) / log(2)`. Swift on Linux uses glibc, and 12 of 19 are
bit-identical. The tolerances leave room for Apple's libm, which has not been
measured yet. A real divergence, such as a pitch mark landing one sample away,
would show up as errors orders of magnitude above the tolerance.

`app/tests/test_call_voice_native_parity.py` regenerates the fixture from the
JS in a temporary directory and fails if any number differs from the committed
copy by more than 1e-7 (the hash is left out of that comparison, so a V8
`Math` change cannot fail it on its own). It also checks that the two copies
are the same file. A change to the worklet or to `CALL_VOICES` therefore fails
that test until the fixture is regenerated and both ports follow:

```sh
node tools/call_voice_parity.mjs \
  native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-voice-parity.json \
  native/android/core/src/test/resources/chat/call-voice-parity.json
```

Then run the native tests: `swift test --filter CallVoice` from
`native/ios/ForkMeshCore` (`FM_ONLY=FMChat` keeps the build small), and
`./gradlew --offline :core:coreTest -Pfilter=CallVoice` from `native/android`.

### Your avatar as the face

The face picker in a direct call offers "Your avatar" (id `avatar`, added
2026-10-24) after "Your own face". It uses the account's profile picture
(`userSession().avatarPng`, passed to `createChatCalls` as `avatarImage`) as
the stand-in, through the same path as "Your photo…": `stageFaceFor` hands the
stage the `custom` face with the avatar image, and the picker shows "Your
avatar" while the stage reports `custom`. The option is hidden without a
profile picture, and an avatar with no face the mesh can find says so and
falls back to your own face, like an unusable photo. `createFmmLocal` reads
the same saved choice, so group calls and the World's "send video" use it too
(`avatarImage` and `faceChoice` options). `faceImageFor`, `photoFaceId` and
`stageFaceFor` in `chat-call-face-custom.js` are the shared helpers.

### Hand tracking

`chat-call-hand-tracker.js` runs MediaPipe's palm detector and 21-point hand
landmark model (`app/public/assets/hand-tracking/`, Apache 2.0) on the same
WebGL runner and names gestures. The World uses it to move avatar arms and
send reactions ([../world/voice.md](../world/voice.md), "Hands and
reactions"). The converter now maps `FULLY_CONNECTED` on a 1 x 1 input to a
1 x 1 convolution and knows fused `RELU6`, and the runner has `relu6`; the
existing models convert to the same bytes.

### Preview and reuse outside a call

`/chat`'s pre-call check (above) opens the camera through the same stage
with Face, Background and Voice pickers and "Hear myself"; Start video call
keeps the stage and offers its track, with no camera restart. The phone apps
have their own pre-call check, and the Face Changer plug-in's screen
([below](#face-changer-on-the-phones)) is where the look is set up outside a
call.

```js
const stage = createFaceStage({ cameraStream, faceId, backgroundId, backgroundImage, outfitId });
stage.track;                      // MediaStreamTrack, 30 fps canvas capture
stage.setFace(id);                // "" for the caller's own face
stage.setBackground(id, image);   // ids from CALL_BACKGROUNDS; image = data URL for "custom"
stage.setOutfit(id);              // ids from CALL_OUTFITS; "" for your own clothes
stage.stop();
```

`createVoiceStage({sourceTrack, voiceId, userPitchHz, moduleUrl, onStats})`
returns a processed `track` plus `setVoice`, `monitor(on)` and `stop()`.
Neither needs an `RTCPeerConnection`, so view-once video messages record from
them ([chat-private-rooms.md](chat-private-rooms.md)). With `{manual: true}`,
`stage.processFrame(source, time)` renders on demand for tests.

#### Plug-ins

The Marketplace `face-changer` page (`/dashboard/face-changer`) and the
`voice-changer` web page (`/dashboard/voice-changer`) run these same modules,
imported from the App at `/chat-call-faces.js`, `/chat-call-voice.js` and
friends; neither package carries a copy. Their shared glue is
`app/public/call-studio.js`: camera and microphone open with the call
constraints, the face, background and voice choices under the call's
`localStorage` keys (a face picked there is the next call's face),
`createVoicePreview` with the call's worklet URL plus a stats tap, clips
through the view-once `recordViewOnce`, PNG snapshots of `stage.canvas`, and a
pop-out window showing `stage.track`. Both pages mount the Preview's
`mountVoicePreviewControls`, and the face page shows only `stage.track`, so
the stage's blur-on-lost-face rule holds there too. Each manifest's
`dashboard.page` lists `devices`; `build_app_plugins.py` turns that into
`plugin_registry.PLUGIN_DEVICE_PAGES`, and `_apply_browser_security_headers`
gives exactly those routes `camera=(self)` (when asked) and `mediastream:`
in `media-src`, as `/chat` has.

Clips can go to a share sheet: `shareStudioBlob` sends the file to the
ForkMesh phone app when it hosts the page (`fmShare` on iOS, `FmShareBridge`
on Android, as begin, 384 KiB base64 chunks and end messages) and otherwise
calls `navigator.share` with the file where the browser can share files
(`canShareStudioFiles`), so phone browsers get their own share sheet. The
phone apps no longer load this page (next section), so the `fmShare` and
`FmShareBridge` paths only matter to an older app build.

#### Face Changer on the phones

On iOS and Android the Face Changer is a native feature, not the web page in a
web view. It owns everything a face change needs, and calls reuse it:

- **The look.** `FaceChangerLook` (Android `core/.../facechanger/`, iOS
  `FMChat/FaceChanger/`) holds the face, face photo, background, background
  photo and voice choice, the catalogs and labels, validation, and the
  `chat.callFace`, `chat.callBackground` and `chat.callVoice` keys. Each change
  tells its listeners which part changed. One look per app: the plug-in
  (`FaceChangerFeature` on Android, `FaceChangerStudio` on iOS) restores it from
  the preferences and photo files and saves every change.
- **The engine.** The plug-in owns the `direct-call.html` host
  (`WebCallPeer`/`WebCallPeers`, Android `feature/facechanger/`, iOS
  `Features/FaceChanger/` with the Vision and TrueDepth code) and one factory,
  so the plug-in screen and calls share a single page and camera. The factory
  refuses a preview while a call holds the page (`busy`), so opening the
  plug-in during a call cannot take the camera away from it.
- **The plug-in screen** (route `face-changer`) previews the camera through the
  bundled page with Face, Background, Voice and Hear myself, and records a clip
  with the page's `record`/`recordStop`/`recordChunk` operations (the ones
  view-once video uses), capped at five minutes like the web studio. Android
  hands the clip to the share sheet; iOS shows it with Save and Share.
- **The page must be on screen while it opens the camera.** WebKit does not
  start `getUserMedia` for a `WKWebView` that is not in a window, and an
  Android WebView that is not attached does not render or play. The first
  native screens (2026-10-16) attached the WebView only after `preview`
  returned, so the screen spun until the watchdog fired, then said "The
  camera could not start". Because the one-thread worker was still waiting,
  the Face, Background and Voice buttons did nothing in the meantime. The
  iOS screen now shows the factory's previewing peer as soon as it exists,
  and Android sets `previewing` before running the page's `preview`, so the
  WebView is attached from the start (the same rule the old iOS call
  preview learned on 2026-10-14).
- **The camera video must be on the page too (2026-10-18).** The face stage
  read the camera through a `<video>` it never added to the document.
  WebKit (every iPhone browser, `WKWebView`, Safari) does not play a
  `MediaStream` in a detached, `display:none` or off-screen video, so the
  stage never got a frame: `play()` never settled, the stage drew nothing and
  the screen stayed on its spinner over black. This hit iPhones without
  TrueDepth, the fallback when the TrueDepth frames do not arrive, and the
  web page in Safari. The stage now adds that video to the page as a 1 px,
  transparent, fixed element (`CAMERA_STYLE`), calls `play()` again if WebKit
  pauses it, and removes it on `stop()`. Chromium never needed it.
- **The recorder asks for an H.264 level that fits the picture
  (2026-10-18).** The page's `record` and the web test clip asked for
  `video/mp4;codecs=avc1.42E01E`: Baseline level 3.0, at most 1,620
  macroblocks (about 720x576). That was enough for WebKit's default
  640x480 camera, but since the call-quality change of 2026-10-16 the camera
  opens at 1280x720 and the face stage draws up to 960x540, both over the
  level. On the iPhone the recording then failed or came back empty, and the
  Face Changer said "The clip could not be saved". The first choice is now
  `avc1.42E028` (level 4.0, up to 1920x1080 at 30 fps); the plain `avc1`
  and `video/mp4` fallbacks are unchanged. The view-once recorder keeps
  level 3.0 because it opens the camera at 640 px. An empty recording (a stop
  straight after the start) now says "Nothing was recorded" instead.
- **The preview does not wait on the voice.** The page's `preview` used to
  return only after the voice worklet started, and `addModule` has no time
  limit of its own, so a slow or stuck worklet kept the native screens
  spinning until their watchdog fired. `preview` now waits at most
  `VOICE_READY_MS` (2.5 s); the voice carries on starting and reports itself
  with the `voice` event. `createVoiceStage` gives `addModule` 6 s
  (`MODULE_TIMEOUT_MS`).
- **Calls.** `DirectCallEngine` takes the plug-in's look instead of keeping its
  own copy and listens to it: a change made in the plug-in, or from the call
  screen's Face, Background and Voice controls (the plug-in's own controls),
  reaches the live video peer at once, and a new call starts with the current
  look. The plug-in pushes changes to its preview peer the same way, so neither
  screen sends anything to the page by hand.
- **No eye button.** The conversation header has only Voice call and Video
  call; the camera preview it used to open is the plug-in screen. The view-once
  video message recorded from that preview on iOS went with it; the
  conversation's own view-once camera still sends one.

The face page also has the Outfit picker (thumbnails drawn by
`callOutfitThumbnail`, one per tick after the page loads so it never blocks),
and its stage takes the picture's own shape: the `<video>`'s `resize` event
sets the box's `aspect-ratio` to the stream's, and a portrait stream gets a
centred box up to 78% of the window high instead of a pillarboxed strip in a
16:9 box.

## Measuring

`app/tools/call_video_harness.cjs` drives the real modules in headless
Chromium (`render`, `native`, `models`; `PLAYWRIGHT_DIR` names a Playwright
install, `--gpu` uses the machine GPU). `app/tools/call_video_metrics.py`
reports landmark jitter and output stability; it needs only Pillow. Test footage is kept out of the
repository.

## Tests

`app/tests/test_chat_direct_calls_frontend.py`,
`app/tests/test_chat_call_checks.py` (pre-call check, tones, incoming alert,
quality and route),
`app/tests/test_chat_call_layout.py` (the answering iPhone's tracks, the
self-view drag and small-screen buttons),
`app/tests/test_chat_call_faces.py`, `app/tests/test_chat_call_voice.py`,
`app/tests/test_chat_call_voice_realism.py`,
`app/tests/test_call_face_native_parity.py` (with the iOS
`CallFaceParityTests` and Android `CallFaceParityTest`),
`app/browser_tests/tests/native-call-record.spec.js`, plus the iOS and
Android `DirectCall*` tests.

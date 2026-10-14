# Direct messages in the app, group add, and encrypted peer-to-peer calls

Three additions shipped together on branch `858-show-direct-messages-app`.

## Direct messages surface on the dashboard

The full `/chat` page has always listed direct messages for signed-in
accounts; the dashboard's chat panel (every page `dashboard-chat.js` mounts
on) did not show them at all. `mountDirectMessageStrip` now inserts a strip
under the channel header — built dynamically, like the "Open private
channels" link, so none of the static dashboard HTML pages change. Each chip
links to `/chat?dm=<name>` and carries the conversation's unread badge; the
trailing `+` chip links to `/chat?dm=` (an empty `dm` parameter), which the
chat page now reads as "open the new-direct-message picker".

The strip is read once from `GET /api/chat/direct-messages?limit=20` and
then refreshed only when the per-account event channel pushes the
`direct-messages` topic. `dashboard.js` re-broadcasts that topic as a
`forkmesh:direct-messages` window event so the strip needs no socket of its
own and no poll (docs/operations/polling-elimination.md). The strip is
skipped inside the World embed (`worldEmbed=1`) and for signed-out visitors.

## "Add person" inside a direct message

A direct conversation is strictly one-to-one — `chat_direct_conversations`
keys the pair with a unique blind index and the participant set never
changes. So the "+ Add person" button in a DM's heading does the only
correct thing: it opens the existing people picker in "add" mode and, on
selection, creates a **new private channel** whose members are the current
partner and the picked person (`POST /api/chat/channels`), then switches to
it. The generated channel name is the three participants' slugs (10
characters each, 34 total) plus a random 4-hex suffix to stay inside
`CHANNEL_NAME_RE` (40 chars) and avoid `name_bi` collisions. The dialog says
so explicitly before anything is created; message history is not copied —
it cannot be, the history is end-to-end encrypted to the two original
participants' devices.

## Encrypted peer-to-peer calls in direct messages

`app/public/chat-calls.js` adds one-to-one audio/video calls to direct
conversations on `/chat`. The design reuses what already exists:

- **Signaling** rides the DM's own room WebSocket as non-persisted frames
  (`call-invite`, `call-accept`, `call-decline`, `call-offer`,
  `call-answer`, `call-ice`, `call-bye`). DM rooms always require the
  device seal, so every signaling frame is end-to-end encrypted
  (chat-seal X25519 layer) before the AES room layer; the relay sees only
  ciphertext and retains nothing (no `persist`, so `_maybe_retain` never
  stores them).
- **Media** is WebRTC, mandatory DTLS-SRTP, flowing directly between the
  two browsers. ICE uses the same public STUN server the Office uses
  (`stun:stun.cloudflare.com:3478`, mirroring `OFFICE_STUN_URLS`). There is
  no TURN server (docs/office/README.md), so peers that are both behind
  symmetric NAT will fail to connect — the same limitation the Office has.
- **Call flow** follows `office/public/rtc.js`: fixed transceiver order
  (audio, video), the caller offers only after the callee accepts, trickle
  ICE with a pre-answer queue, `replaceTrack` instead of renegotiation.
  Mute and camera-off toggle `track.enabled`. Frames carry `to` (the peer's
  roster id) so multi-device accounts converge on one answering device; a
  second device that sees its own account's `call-accept` dismisses its
  ring banner.

Limitations, accepted deliberately for this first version:

- Ringing reaches the callee only while they have the conversation open
  (the room socket is per-active-conversation). An unanswered 45-second
  ring ends with the caller posting a persisted "📞 Missed … call" chat
  line, which travels the normal notification path (unread badge, APNs,
  `direct-messages` topic).
- Switching to another conversation mid-call keeps the media flowing (it
  is peer-to-peer) but the signaling channel follows the active room, so
  a remote hang-up is then detected by the peer connection's state change
  instead of the `call-bye` frame.
- Calls exist only in DMs, not channels, and not in the Office embed.

`/chat` and `/chat.html` get a `Permissions-Policy` override in
`www/public/_headers` (camera was disabled site-wide) and `mediastream:` in
their CSP `media-src`, mirroring the Office's headers.

Contract tests: `app/tests/test_chat_direct_calls_frontend.py`.

## Call buttons in the desktop, the `call` link, and video-call faces

The desktop's chat header shows **Voice call** and **Video call** buttons on
an Office direct message (`#office/@name`). The Qt app has no WebRTC stack,
so the buttons open `/chat?dm=<name>&call=audio|video` on the account's web
origin (`catalogApiUrl()`), the same peer-to-peer call `/chat` already
places. `/chat` reads `call` after opening the conversation and asks with a
`confirm()` before ringing. That way a pasted link can never ring someone
on its own, and the click gives the page the user activation its
`AudioContext` needs.

A video call's panel has a **Face** picker: the caller's own face, or one
of five faces (Maya, Theo, Kofi, Ingrid, Ren). A chosen face replaces the
outgoing video track with a WebGL canvas track (`captureStream`, 24 fps)
through `RTCRtpSender.replaceTrack`, so no renegotiation is needed. The local
preview shows the same disguised track the other person receives. Choosing
"Your own face" stops the canvas and sends the camera again. The choice is
kept in `localStorage` (`forkmesh.chat.callFace`) and applied to the next
video call.

### Face swap (rewritten 2026-10-13)

The first version painted hair, brows and a skin tint over the caller's own
face; it never looked like another person ("doesn't work well at all"). It
was replaced by the method the open-source face-swap projects use: the
iOS ones built on Apple Vision (for example Alex13832/FaceSwap-iOS, which
moved from dlib to Vision landmarks), the Python/OpenCV ones (learnopencv's
Face Swap, Delaunay triangles from landmarks, one affine warp per triangle),
and Matthew Earl's "Switching Eds" colour correction (scale the warped face
by `blur(camera) / blur(face)`). There are no third-party libraries and no
model downloads; everything runs on the device before encryption.

Four modules in `app/public/`:

- `chat-call-faces.js`: the five faces, the capture → landmarks → render
  loop (`createFaceStage`), and the stage API `chat-calls.js` and the
  native page use (`track`, `face()`, `setFace`, `stop`).
- `chat-call-face-geometry.js`: the 68-point layout (iBUG order), the mean
  face (`buildTemplate`, built at load time from the five annotated photos,
  mirrored and given a hand-set depth per point), `fitTemplate` (places the
  mean face on tracked eyes and mouth, turns it by yaw, stretches the lower
  face to the mouth, drops the jaw for an open mouth), Delaunay
  triangulation (Bowyer–Watson), `landmarksFromRegions` (Apple Vision's
  regions → 68 points by arc-length resampling, so any Vision constellation
  works), and a One Euro filter for jitter.
- `chat-call-face-tracker.js`: the landmark source where the platform has
  none (browsers, the Android WebView).
- `chat-call-face-render.js`: the WebGL renderer.

**The photos.** `app/public/assets/call-faces/*.jpg` are 512 px crops of
StyleGAN faces of people who do not exist, marked public domain on
Wikimedia Commons ("This Person Does Not Exist example.jpg" → Maya,
"Male and female deepfake.jpg" → Theo and Ren, "Man 2.jpg" → Kofi,
"Woman 1.jpg" → Ingrid). Faces of real people were ruled out: a
wearable copy of a real person's face is an impersonation tool. The 68
landmarks of each photo were placed by hand on zoomed grids and are
stored with the face in `CALL_FACES`; they are resampled at load time
(`canonicalShape`) so the photo and a live Vision face use the same
spacing along the jaw, eyes and lips.

**The mesh.** One topology for every face: Delaunay over the mean face
with its mouth open, plus ten forehead points above the brows and an
inner ring 14 % inside the jaw. Jaw and forehead points carry alpha 0 and
everything inside alpha 1, so the edge feathers into the caller's own
skin, hair and ears. A face can narrow this (`look` in `CALL_FACES`):
Ingrid's bangs reach her brows, so her forehead band is short and her brow
points carry alpha 0. The swap then fades out between her eyelids and the
caller's own brows, instead of painting a strip of fringe on a
short-haired caller. Triangles made only of inner-lip points are the
mouth hole: alpha 0, so the caller's real teeth and tongue show when they
open their mouth.

**Rendering, per frame** (WebGL 1): upload the frame; draw the mesh with
live positions and photo texture coordinates into a full-size target;
reduce the camera and the warped face (both weighted by the mask) to a
160-px-wide target and blur them twice: once at 0.22 × the eye distance
(local shading) and once at about 0.6 × (face-wide colour). The composite
multiplies the photo by the face-wide colour ratio and by a clamped
(0.65–1.5) local luminance ratio, then mixes it over the camera by the
mask. A per-channel local ratio, as in Earl's original, turned dark bangs
yellow over a light forehead, so only luminance is corrected locally.

**Where the landmarks come from:**

- **iOS**: Apple Vision. The page posts a 480-px JPEG of the frame
  (`{event: "landmarks"}`); `CallFaceLandmarks.swift` runs
  `VNDetectFaceLandmarksRequest` (76-point constellation) off the main
  thread and answers with the regions in image pixels through
  `window.fmCallLandmarks`. Up to two frames are in flight, and each
  frame is drawn with its own landmarks, so the face never lags the
  picture. Three failed requests switch the page to the tracker.
- **Web and Android** (`chat-call-face-tracker.js`), no trained model:
  an adaptive skin model (generic YCbCr box, then a Gaussian learned from
  the caller's cheeks), eye candidates from an eye map in the spirit of
  Hsu, Abdel-Mottaleb & Jain (local darkness, grey-level contrast and the
  Cb/Cr eye chroma), eye pairs checked for skin below and between the
  eyes and forehead skin above, and scored by normalised correlation with
  an average face. There are two 24×28 averages: shading and local
  darkness, built from the five photos and their mirrors, compared after
  removing a lighting plane, at three vertical stretches. Mouth evidence
  (lip redness against the learned skin and the dark lip line) breaks
  ties. Eye centres are refined with Timm & Barth's gradient method. A
  new face must be found twice (the second time in a canonical window
  around the first), the lock re-checks itself every 16 frames, and two
  agreeing re-checks move it. The mouth opening is measured from the dark
  gap between the lips. Microphone level is not used: when it disagrees
  with the picture, the mouth hole shows closed lips inside open ones.

**Privacy when tracking fails.** The disguise holds the last landmarks for
half a second, but the frame starts blurring (a 160-px copy blurred four
times) on the first missed frame, and stays blurred until a face is
found again, including at the start of the call. The real face is not
sent unblurred while a disguise is chosen.

**Measured** (headless Chromium, `app/tests/test_chat_call_faces.py`
covers the pure parts): on moving sequences built from the five photos
and a held-out face, the tracker kept the lock on every frame with a
median eye error of 2–6 % of the eye distance, at 11–21 ms per frame
on a desktop CPU. Cold starts on single stills with random scale,
rotation, lighting, noise and backgrounds (up to ten tries each) locked
the right eyes in 80 of 96 cases: 52 of 60 on the five photo faces and
28 of 36 on three held-out faces. The misses lock onto the hairline or the
brows; on video, the 16-frame re-check usually moves such a lock back.
Vision on iOS does not have that weakness.

Limits: the canvas is driven by `setInterval`, which browsers slow to
about 1 Hz in a background tab, so the outgoing video stutters while
`/chat` is not the visible tab. Strong side profiles (beyond about 40°)
are drawn as a turned flat face. Without WebGL or `canvas.captureStream`
the picker is hidden.

## Calls and faces in the iOS and Android apps

Requested 2026-10-12 ("make it work on mobile"). Both apps already had
`DirectCallEngine` (the same signaling state machine as `chat-calls.js`)
and a WebRTC page, `direct-call.html`, but nothing created the engine or
showed a screen. Now:

- **Buttons.** A `direct:` conversation's header has video and voice call
  buttons. Tapping one asks for the microphone (and the camera for video)
  through the OS permission prompt, then rings. A ring shows an
  Accept/Decline banner over the chat. A live call is a full-screen
  screen with mute, camera, **Face** and hang up.
- **Signaling** goes through each app's `DirectMessageService`
  (`sendCallFrame` out, `onCallFrame` in), so frames are sealed to the
  DM's devices exactly like the web's, and the apps interoperate with
  `/chat`. An unanswered ring posts the same "📞 Missed … call" line.
- **Media** runs in a WebView that loads `direct-call.html` with an
  `https://` base URL (iOS `call.forkmesh.invalid`, Android
  `appassets.androidplatform.net`), so the page is a secure context and
  `getUserMedia` works. Camera and microphone are granted only to that
  origin, and on Android only when the app already holds the permission.
  The native side drives the page through `window.fmCall(id, op, args)`.
  Replies come back through `webkit.messageHandlers.fmCall` (iOS) or
  `FmCallBridge.post` (Android).
- **Faces.** `direct-call.html` carries `chat-call-faces.js` inlined:
  `app/tools/sync_native_call_faces.py` rewrites the
  `<script id="fm-call-faces">` block in both copies, and
  `test_chat_call_faces.py` fails if either copy is stale or the two
  differ. The engine keeps the chosen face (`face`, `setFace`,
  `onFaceChanged`, saved under the `chat.callFace` pref), gives it to a
  video peer before the offer or answer, and forwards changes mid-call.
  The page's `face` operation swaps the outgoing track with
  `replaceTrack`. The sync tool inlines all four face modules and writes
  the five photos as data URLs into `<script id="fm-call-face-photos">`
  (the page's CSP allows `img-src data:` for that). On iOS the page asks
  the app for Apple Vision landmarks (above); Android uses the tracker.

Details per platform: `native/ios/docs/FMChat.md` ("Voice and video
calls"). On Android, `feature/chat/DirectCallFeature.kt` installs the
engine from `ChatDepth` and runs every engine call on one `direct-call`
worker thread. `WebCallPeer.kt` turns each page operation into a blocking
call (posted to the main thread, at most 20 s), and `DirectCallOverlay`
sits above navigation in `Features.Root`, so a call survives moving
between screens.

Limits: a phone rings only while that DM's room socket is open (the app
in the foreground with the conversation open or unread). Call frames are
not persisted or pushed, and there is no CallKit, VoIP push or Android
foreground service, so a call can drop when the app is backgrounded. The
apps have not been run on a device for calls: iOS core tests pass on
Linux, but the iOS app target was only parse-checked. Android core tests
pass and `:app:assembleDebug` builds; there was no emulator run.

Contract and runtime tests: `app/tests/test_chat_call_faces.py`.

## Voices (added 2026-10-13, branch `1994-call-voice-per-face`)

Requested with the faces: "the caller's voice changes to match the chosen
face, at the highest quality". Each face has a voice, and calls can send it
on the web, on iOS and on Android.

**Where it runs.** All three platforms run the same page code. The
microphone track from `getUserMedia` (with echo cancellation and noise
suppression on) goes through an `AudioWorklet` into a
`MediaStreamAudioDestinationNode`. That node's track replaces the audio
sender's track with `RTCRtpSender.replaceTrack`, so no renegotiation is
needed, just as with the face. The effect is applied after the browser's own
echo cancellation, so that cancellation still works. On iOS and Android the
audio flows inside the WebView (`direct-call.html`), not through native
audio, so no `AVAudioEngine` or Android audio code was needed. If the
worklet cannot start (no `AudioWorklet`, or an `AudioContext` that will not
run within 2 s), the call sends the plain microphone and the indicator
shows no voice.

**The engine** (`app/public/chat-call-voice-worklet.js`) is a port of the
desktop voice changer (`marketplace/packages/voice-changer`, `VoiceEngine.cpp`):

- A 70 Hz high-pass and a gentle minimum-statistics denoise (floor −11 dB).
  The browser has already suppressed most noise, and anything left over
  turns into static once the pitch is shifted.
- A 1024-point STFT (8x overlap, an asymmetric window with a 512-sample
  synthesis tail) that only warps the cepstral spectral envelope (formants)
  and its tilt. Each bin keeps its phase, and the input bin is multiplied
  by `gain · warpedEnvelope · tilt / envelope`, so no trigonometry is needed.
  FFTs are real-input (a half-size complex FFT).
- Time-domain PSOLA for the pitch. Marks sit one period apart, each refined
  by cross-correlation, and the synthesis hop uses the *measured* mark
  spacing. When the shift lowers the voice by more than an octave
  (ratio < 0.5, for example a woman choosing Kofi), the grains are widened to
  half the hop. Before that fix, two-period grains left silent gaps between
  pulses.
- A McLeod pitch tracker on the pre-warp input (12 kHz, an 85 ms window,
  run every 10.7 ms, median of three readings).
- A 1.3 ms look-ahead peak limiter at 0.95 (sliding minimum, release, then a
  box filter), so the gain never jumps.

Added latency is 1728 samples, **36 ms** at 48 kHz. The engine works per
sample, so the 128-sample render quantum (or any block size) does not
matter. Switching voices glides the ratio, formant, tilt and breath, and
cross-fades wet and dry over 30 ms. "Your own voice" in the middle of a call
keeps the stage running and fades to the delayed input, which is then
bit-exact. A call that starts with no voice never builds the stage, so it
has no added latency.

**Per-face voices** (`CALL_VOICES` in `app/public/chat-call-voice.js`;
`chat-call-faces.js` is not touched). Each voice is a target median pitch
and a vocal-tract scale relative to an average man (1.0; an average woman is
about 1.17), plus tilt and breath. The voices were chosen from the photos:

| Face | Looks like | Pitch | Tract | Brightness | Breath |
| --- | --- | --- | --- | --- | --- |
| Maya | woman, late 30s | 196 Hz | 1.14 | 0 | 0.04 |
| Theo | man, mid 20s | 124 Hz | 1.03 | +0.10 | 0 |
| Kofi | man, late 30s | 96 Hz | 0.93 | −0.15 | 0 |
| Ingrid | woman, about 20 | 228 Hz | 1.21 | +0.12 | 0.08 |
| Ren | woman, mid 20s | 210 Hz | 1.17 | +0.05 | 0.02 |

(Ren is a woman: she is the female half of "Male and female deepfake.jpg".)

The shift depends on the caller. The engine keeps a median of the caller's
pitch: a histogram in quarter-semitone bins with a half-life of 3000 voiced
readings. The pitch ratio is `target / callerMedian`. The formant ratio is
`targetTract / tractFromPitch(callerMedian)`, which maps 115 Hz to 1.0 and
205 Hz to 1.17 on a log scale, so a man choosing Maya gets about ×1.7 pitch
and ×1.14 formants, and a woman choosing Maya gets almost none. The median is
saved every second (`forkmesh.chat.callVoicePitch`, in the page's
`localStorage` on the apps too) and seeds the next call with a weight of 60
readings. A first call starts from 150 Hz and settles once the caller has
spoken for a few seconds. The early estimate follows the first phrases,
which are often higher.

**Choice and UI.** The choice is "Match face" (the default), "Your own
voice" (`off`) or one of the five voices. It is stored in `localStorage`
`forkmesh.chat.callVoice` on the web and under the `chat.callVoice` pref in
the apps (`DirectCallEngine.voiceChoice` and `setVoiceChoice` on iOS and
Android). "Match face" uses the face shown in a video call. In a voice-only
call it uses the saved face (the persona). `/chat` shows a Voice select in
the call panel with a "Voice: <name>" indicator. The apps add a waveform
button that opens a picker, plus a "Voice: <name>" line under the call
state. The apps show the voice the page reports as applied (the page's
`{event: "voice", id}`), not the one that was requested.

**Native page.** `app/tools/sync_native_call_voice.py` inlines
`chat-call-voice.js` as `window.FmCallVoice` and the worklet source as a
`<script type="text/plain" id="fm-call-voice-worklet">`. The page loads the
worklet from a `blob:` URL, so the page CSP has `script-src 'unsafe-inline'
blob:`. It is a separate tool from the face sync, so the two never rewrite
each other's blocks. Page operations:

- `setup(media, ice, face, voice)`: the fourth argument starts the voice
  before the offer.
- `voice(id)` and `voiceList()`.
- `preview(media, voice)` and `monitor(on)`, described below.

`offer` and `answer` wait up to 2.5 s for a starting voice, so the first
packets already carry it. On the web, `/chat` waits the same way before it
sends `call-accept` or the offer.

### Hearing yourself, and using the processor outside a call

Requested 2026-10-13: before a call, people must be able to hear
themselves with the voice change, and recorded messages will reuse the same
processor. `chat-call-voice.js` exports:

- `createVoiceStage({sourceTrack, voiceId, userPitchHz, moduleUrl, onStats})`
  returns `{track, voice(), setVoice(id), monitor(on), monitoring(),
  monitorLatencyMs(), latencyMs(), stats(), stop()}`. It needs no
  `RTCPeerConnection`. `track` is a processed `MediaStreamTrack` that a
  `MediaRecorder` can record directly. `monitor(true)` also plays the
  processed voice to the local output, through a gain that ramps over 15 ms.
- `createVoicePreview({micStream?, choice?, faceId?})` builds the stage from
  the saved choice and pitch. It turns the monitor on only when
  `headphonesLikely()` is true and returns `{track(), setChoice, setFace,
  monitor, monitoring, headphones, supported, stop}`.
  `mountVoicePreviewControls(container, preview)` adds the Voice select, a
  "Hear myself" toggle and a hint. The hint says headphones were detected,
  or warns that a speaker will echo and may howl.
- `headphonesLikely()` reads the output device labels (headphones, headset,
  earbuds, AirPods, Bluetooth, hands-free). Browsers expose labels only
  after microphone permission, so this is a heuristic. The apps can ask the
  OS instead: `CallAudioRoute.headphonesConnected` on iOS (AVAudioSession
  route: headphones or Bluetooth A2DP/HFP/LE) and
  `callHeadphonesConnected(context)` on Android (wired, Bluetooth A2DP/SCO,
  USB or BLE headset). `WebCallPeer.preview(media:voice:)` and `setMonitor`
  drive the page before `setup`, and `setup` reuses the previewed
  microphone.
- During a call, `createChatCalls(...).monitorVoice(on)` does the same (off by
  default).

What you hear yourself is delayed by the 36 ms processing plus the output
device's own latency. Headless Chromium reported 71–103 ms in total with its
fake output device. Bluetooth headphones add their usual 100–200 ms, which
nothing in the page can remove. The pre-call Preview screen is being built
by the face agent (branch `1993-call-face-head-and-backgrounds`). It is to
call `createVoicePreview` / `mountVoicePreviewControls` on the web and the
`preview`/`monitor` page operations in the apps. That wiring happens when
the branches merge.

### Measured

Measured with piper speech as callers: lessac, a female voice with a
192 Hz median, and ryan pitched down with formants preserved
(`rubberband`), a male voice with a 120 Hz median. Each was rendered
through every voice. F0 was measured with an independent YIN tracker, on
frames aligned for latency.

- With a seeded caller pitch, the median output lands within 0.7 semitone of
  every target, for example a man choosing Maya: 194 Hz against 196, and a
  woman choosing Kofi: 100 Hz against 96. Between 87 % and 99 % of voiced
  frames are within one semitone of the intended ratio, and octave errors
  are under 3 %.
- Through a real WebRTC loopback with Opus (headless Chromium, the fake
  microphone fed from the WAV, `replaceTrack` onto a live sender), the
  remote side heard Kofi at 93 to 98 Hz and Maya at 190 to 194 Hz.
- On sines in the tracked range, with the voice switching every half second:
  no clicks (worst normalised second difference 0.08), no gaps, and the
  output is identical for any render block size.
- CPU: 0.25 to 0.65 ms on average per 128-sample quantum, out of 2.67 ms, in
  Node on this (shared) desktop. Headless Chromium renders offline at 9 to
  12x real time.

Tests: `app/tests/test_chat_call_voice.py` (the DSP on synthetic voices,
latency, block-size independence, clicks, limiter, CPU, the `/chat`
wiring, native page sync and app wiring), `DirectCallsTests` on iOS
(`testVoiceMatchesTheFaceUntilAnotherVoiceIsChosen`) and
`DirectCallVoicesTest` on Android.

**Not verified on a device.** Nothing here has run on a phone, and no person
has listened to it:

- WKWebView supports `AudioWorklet` and `getUserMedia` since iOS 14.5 and
  14.3. Unverified: that an `AudioContext` runs alongside WebKit's capture
  session without a gesture (`mediaTypesRequiringUserActionForPlayback = []`
  should allow it), and how the voice-processing audio session sounds with
  the monitor on.
- The Android WebView is Chromium, so it should behave like the headless
  runs above.
- The iOS app files were only parse-checked, and the Android app was only
  compiled (`:app:compileDebugKotlin`).

## Direct-message header on a phone

Reported 2026-10-13: the DM screen was too wide for a phone and its header
buttons were cut off. A DM header holds more fixed-width controls than a
channel header, because it adds the call buttons and "Add person".

- **iOS.** `MessagesConversationView` centred the title by reserving its
  width (up to 220 pt) in the middle of an `HStack` and splitting what
  remained between the two sides. A DM's right-hand group (video, voice,
  new agent, avatar) is about 198 pt wide, more than half of a 393 pt
  screen, so the row overflowed and its edges were clipped.
  `MessagesHeaderLayout` now measures both side groups first and gives the
  title the room left between them. The title stays centred on the screen
  when it fits; otherwise it shifts toward the side with more room and
  truncates its name.
- **Web `/chat` at 680 px and below.** `chat.css` loads after the inline
  `<style>` in `chat.html` and set `#chat-status` back to `inline-flex`,
  which undid the inline rule that hides the status text on phones. That
  text never wraps, so it took header width. On phones the status now keeps
  only its coloured dot, and "Add person" and the encryption pill show only
  their icons (both labels stay available to screen readers). The
  disappearing-messages select is capped at 84 px, and the server-copies
  line ends in an ellipsis. `.chat-workbench` clips overflow, so the
  existing `scrollWidth` check in `direct-messages.spec.js` could not catch
  this.
- **Android.** The DM header showed the "ENCRYPTED" text badge next to two
  call buttons and the timer, which left the title with no width on a
  360 dp phone. In a DM the badge is now a lock icon button. It is labelled
  with the badge text and opens the same encryption summary.

None of the three was run on a device or in a browser for this change.

### The back button on smaller iPhones

Reported 2026-10-13: the DM header's back chevron did nothing on smaller
phones. `MessagesHeaderLayout` placed the right-hand group at the right edge
whatever its width, and that group grows with Dynamic Type: the new agent
circle is a `@ScaledMetric` 44 pt. On a 320 pt screen (an iPhone SE or mini
with Display Zoom), or with large text, the group slid left over the chevron.
It was drawn after the chevron, so it took the taps.

- The right-hand group never starts left of the chevron and its gap; when
  it does not fit it runs past the right edge instead.
- The chevron's group has `zIndex(1)`, so it is drawn and hit-tested above
  the title and the right-hand group whatever overlaps.
- The right-hand group stops growing at `.xxxLarge` text, like a system
  bar. The new agent button already shows the large content viewer when
  held at larger sizes.
- Only parse-checked on Linux (`swiftc -parse`); not run on a device or in
  the simulator.

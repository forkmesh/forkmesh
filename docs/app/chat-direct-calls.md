# Direct messages in the app, group add, and encrypted peer-to-peer calls

Direct-message surfaces on the dashboard, turning a DM into a group, and
one-to-one audio/video calls with on-device face, background and voice
filters. Access rules and DM delivery are in
[chat-private-rooms.md](chat-private-rooms.md).

## Direct messages on the dashboard

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
  renegotiation. Frames carry `to` so multi-device accounts converge on one
  answering device.
- A ring reaches the callee only while the DM's room socket is open. A call
  that is never answered posts a persisted "📞 Missed audio/video call" chat
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
  mono. It caps the senders at 2.5 Mbps video and 96 kbps audio, with high
  priority and `balanced` degradation where the browser allows it. It also
  adds Opus in-band FEC, no DTX and a 96 kbps average to the SDP sent to the
  other side, so each side's encoder sends full-band, loss-resilient voice.
  The Face Changer and Voice Changer pages share these constraints through
  `call-studio.js`. The face stage still draws at 24 fps and at most 960 px
  on the long side, so a filtered picture is limited by that, not by the
  camera.
- The desktop has no WebRTC stack; its call buttons open
  `/chat?dm=<name>&call=audio|video`, which asks for confirmation before
  ringing, so a link can never ring someone on its own.

### Mobile apps

iOS and Android run the same signaling state machine (`DirectCallEngine`)
through their `DirectMessageService` and interoperate with `/chat`. Media runs
in a WebView loading `direct-call.html` from an `https://` app-local origin (a
secure context for `getUserMedia`); the native side drives it through
`window.fmCall(id, op, args)` and receives replies through
`webkit.messageHandlers.fmCall` or `FmCallBridge.post`. There is no CallKit,
VoIP push or foreground service, so a call can drop in the background, and a
ring reaches a phone only while the app runs with the conversation's room
socket open. Ringing a phone whose app was killed or suspended needs VoIP
push and CallKit on iOS and a push-started call notification on Android,
which need server work.

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
(`captureStream`, 24 fps) through `replaceTrack`. Per frame: landmarks
(Apple Vision on iOS, the face mesh elsewhere) smoothed with a One Euro
filter; a person matte refined with a guided filter; a hair matte around the
head; a 2.5D warp of the whole stand-in head with a depth buffer so near
surfaces win when turning; removal of the caller's head and hair outside the
stand-in, filled from an estimate of the room; per-frame skin and hair colour
matching; eyelid, gaze and tongue transfer. Stand-in assets
(`app/public/assets/call-faces/<id>.jpg` and `<id>-matte.png`) are built by
`app/tools/build_call_face_assets.py`.

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

### Preview and reuse outside a call

`/chat`'s pre-call check (above) opens the camera through the same stage
with Face, Background and Voice pickers and "Hear myself"; Start video call
keeps the stage and offers its track, with no camera restart. The phone apps
have their own pre-call check, and the Face Changer plug-in's screen
([below](#face-changer-on-the-phones)) is where the look is set up outside a
call.

```js
const stage = createFaceStage({ cameraStream, faceId, backgroundId, backgroundImage, outfitId });
stage.track;                      // MediaStreamTrack, 24 fps canvas capture
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
reports landmark jitter and output stability. Test footage is kept out of the
repository.

## Tests

`app/tests/test_chat_direct_calls_frontend.py`,
`app/tests/test_chat_call_checks.py` (pre-call check, tones, incoming alert,
quality and route),
`app/tests/test_chat_call_faces.py`, `app/tests/test_chat_call_voice.py`,
`app/tests/test_chat_call_voice_realism.py`,
`app/browser_tests/tests/native-call-record.spec.js`, plus the iOS and
Android `DirectCall*` tests.

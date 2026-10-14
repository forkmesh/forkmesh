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

### Face swap: the whole head (rewritten 2026-10-13, branch `1993-call-face-head-and-backgrounds`)

The first rewrite warped only the inner face (68 landmarks) and left the
caller's own hair, ears and head outline around it. The user asked for the
chosen face to cover "the whole face and hair" and to move naturally, plus
custom and blurred backgrounds, at the "highest quality". The head swap is
now a 2.5D warp of the whole stand-in head (face, forehead, hair, ears,
neck), the caller's own hair is cut out with a hair model and replaced by
the room behind it, and the colour of the stand-in is matched per frame.
Everything runs on the device before encryption; models are bundled, never
downloaded from a third party at runtime.

Modules in `app/public/` (all inlined into the apps' `direct-call.html`):

- `chat-call-faces.js`: the five faces (`CALL_FACES`: 68 landmarks and a
  28-point head contour per photo), the stage (`createFaceStage`), the
  adaptive quality levels, and the iOS host bridge.
- `chat-call-face-geometry.js`: landmarks and the mean face (as before),
  `headLayout`/`headModel` (landmarks + 10 forehead points + the head
  contour, each with a depth), `buildHeadMesh` (Delaunay over the mean face
  with an open mouth plus the face's contour, so every face has a mouth
  hole), `solvePose` (weak-perspective pose from the 68 points against the
  stand-in's own 3D model: least-squares affine, then the closest rotation)
  and `liveHead` (projects the forehead and contour with that pose and adds
  the landmark residuals with Gaussian weights, so the hair stays attached
  to the jaw and brows when the caller's face is wider or opens its mouth).
- `chat-call-face-render.js`: the WebGL 1 compositor.
- `chat-call-segment.js`: a small WebGL inference engine for the bundled
  models, the person matte and the hair matte.
- `chat-call-face-mesh.js`: face detection and 468-point face mesh for
  browsers and Android, mapped to the 68-point layout.
- `chat-call-face-tracker.js`: the old heuristic tracker, now only a
  fallback while the face models load or when float textures are missing.
- `chat-call-backgrounds.js`: the background list, saving the choice, and
  preparing a picked photo.

**The stand-in photos.** `app/public/assets/call-faces/<id>.jpg` are now the
whole original public-domain StyleGAN frames (the same Wikimedia Commons
files as before, 1024 px), padded by 192 px at the top so heads whose hair
touched the frame edge get a completed crown (an ellipse fitted to the
hair outline, filled with mirrored hair), scaled to 512 px. Next to each is
`<id>-matte.png`: red = head matte (person × head region, plus hair), green
= skin, blue = hair. `app/tools/build_call_face_assets.py` builds both from
the originals (`numpy` and Pillow; it runs the MediaPipe selfie and hair
models in numpy offline): person and hair segmentation, a colour guided
filter for strand-level edges, the neck kept only where it is skin
coloured (chroma likeness against the cheeks, so collars and shirts fade
out), a fade at the bottom edge, colour decontamination of the edge
pixels (the background colour is estimated by push-pull and removed, so
no blue or green fringe), the remapped landmarks and the 28-ray head
contour. The landmark annotations are the hand-placed ones from the first
rewrite, mapped through the old crop (`(160, 272)`, 704 px of 1024).

**Per frame:**

1. Landmarks (Vision on iOS; the face mesh elsewhere), smoothed with a One
   Euro filter (min cutoff 1.5 Hz, beta 1.5). Pose and head points are a
   function of the smoothed landmarks, so they need no second filter.
2. The person matte: the selfie model's 256×144 (or 256×256 for portrait
   frames) mask, a temporal blend that follows big changes at once and
   halves small ones (`rate = mix(0.55, 1, smoothstep(0.06, 0.4, |Δ|))`),
   then a fast guided filter (luma guide at 320 px, radius 4, ε 0.0012),
   applied at full resolution in the compositor. Hair strands and the
   space between arm and body come out clean.
3. With a face chosen, the hair matte: the hair model on a 192×192 crop
   around the head (6.4 eye distances, kept inside the frame and
   stretched, never letterboxed: black bands made it miss half the hair),
   fed its own previous mask as the fourth channel like MediaPipe does,
   warmed up with three rounds on the first frame, and blended in frame
   space.
4. The head mesh is drawn twice into full-size targets: the photo with the
   matte as alpha, and a second pass with skin, hair, the mouth hole and a
   "face region" weight (1 on the inner face, 0.25 at jaw and brows, 0 on
   the forehead and contour).
5. The caller's head is removed where the stand-in does not cover it:
   `zone = max(face ellipse without the neck column, dilated hair mask)`,
   times the caller's matte. Nothing below the shoulders is touched, so a
   suit or a dress stays (an earlier ellipse-only zone cut into shoulders).
   The mouth hole keeps the caller's real mouth.
6. Colour: the skin colour ratio camera/stand-in is measured only where both
   are face (the region weight), heavily blurred, and blended with the
   global ratio where the weight runs out, so the forehead never picks up
   the caller's fringe. Local luminance shading (0.7–1.4) applies only
   where the inner face weight is high. Hair gets the square root of the
   global hue and level change, so blond stays blond on a dark-skinned
   caller while matching the room's white balance.
7. Composite: `out = backdrop · (1 − keep) + front · (1 − zone)` with
   `keep = α (1 − zone)` and `front = clamp(cam − (1 − α) · fill, 0, α)`,
   where `fill` is a push-pull estimate of the room behind the caller.
   Removing the estimated room from the edge pixels is what stops halos
   and leftover "ghost" hair at the cut-out edge. The backdrop is the
   blurred room, the chosen image, or (no background) the real room with a
   soft blur halo around the head, so the filled-in area behind removed
   hair does not show as a smudge.

**Privacy when tracking fails** is unchanged: the frame blurs from the
first missed frame until a face is found again; with a background chosen,
the blurred person is drawn over the background.

## Backgrounds (added 2026-10-13)

The call panel and the pre-call preview have a **Background** picker with
thumbnails: No background, Blur, Strong blur, five built-in images, and
Your photo. It works with or without a face. The choice is saved like the
face (`forkmesh.chat.callBackground` in `localStorage`, the picked photo as
a JPEG data URL under `forkmesh.chat.callBackgroundImage`, at most 1920 px;
the apps keep `chat.callBackground` in their prefs and the photo as
`call-background.jpg` in app storage) and applied to the next call. The
local preview is the outgoing track itself, so it shows exactly what the
other side receives.

- **Blur** is a normalized blur of the room only: room pixels are weighted
  by `(1 − α)³` with α taken as the maximum over a small neighbourhood, so
  the person never bleeds into the blur, and the hole they leave is filled
  by push-pull from the room around it.
- **Images** are drawn cover-fit; the centre of every built-in image is kept
  calm so that the centre 9:16 strip phones send also looks right.
- The matte edge uses the same spill removal as the head swap.

**The five built-in images** (`app/public/assets/call-backgrounds/<id>.jpg`,
1920×1080, plus 320×180 `-thumb.jpg`, 1.08 MB in all) were requested on
2026-10-13 ("one must be a ForkMesh World scene, one ForkMesh corporate
offices", all professional and licence-clean). They are three.js renders
made by `app/tools/render_call_backgrounds.cjs` (scenes in
`app/tools/call-backgrounds/scenes.js`; `node
app/tools/render_call_backgrounds.cjs [world office meeting library
skyline]`, headless Chromium, the repo's own three.js 0.184, about seven
minutes; `PLAYWRIGHT_DIR` points at a Playwright install). Each is 24–40
jittered renders averaged (soft shadows, slight depth of field), then a
light blur, vignette and JPEG quality picked for a size target. No photos
or third-party images are used: every texture, painting, book, plant and
sky is generated in code.

- **ForkMesh World**: the real World scene (`world/public/world`) at 16:30,
  ground level, the office tower on the right third and the roller coaster
  on the left. The World's flat sky colour is replaced by a gradient with
  clouds drawn in code, and the office's upper floors are made visible. The
  World's beach was left out: its photographic sand texture has no recorded
  source.
- **ForkMesh office**: a dedicated set (the World's own office lobby reads
  as a game interior): oak-slat wall with the repo logo
  (`www/public/assets/logo.svg`) and the wordmark in the site font (ABC
  Favorit, `www/public/assets/fonts/ABCFavorit-Bold.woff2`, rendered into
  the image as the site already uses it), green accent, concrete floor,
  desks, plants, window walls. The sign sits outside the centre 9:16 strip.
- **Meeting room**, **Home library** (sage wall, walnut bookcase, lamp,
  window) and **City window** (dusk skyline of generated towers through
  floor-to-ceiling glass).

The local self-view is mirrored like other video apps, so text in a
background reads backwards to the caller; the other side sees it the
right way round.

## Models on the device

| File (`app/public/assets/call-segmentation/`) | Source model | Size | Use |
| --- | --- | --- | --- |
| `selfie-landscape.fmnn`, `selfie-square.fmnn` | MediaPipe Selfie Segmentation (landscape 144×256, general 256×256) | 224 KB each | person matte |
| `face-detector.fmnn` | MediaPipe BlazeFace short range | 211 KB | finds the face when tracking starts or is lost |
| `face-mesh.fmnn` | MediaPipe Face Mesh (468 points) | 1.2 MB | landmarks in browsers and Android |
| `hair-192.fmnn` | MediaPipe Hair Segmentation, reshaped to a 192×192 input | 389 KB | the caller's hair |

All five are Apache 2.0 (Google; the model cards say "Licensed under
Apache License, Version 2.0"); `LICENSE` next to them is the licence text.
The hair model is also used offline by the asset tool. `.fmnn` is our own
container: a JSON graph and float16 weights.
`app/tools/build_call_segmentation_model.py` converts a `.tflite` file with
only the standard library (it reads the FlatBuffer directly, folds
DEQUANTIZE, fuses activations including PReLU into the producing layer,
turns RESHAPE/CONCATENATION tails into output groups, and can re-infer
shapes for a new input size: `--size 192,192`). Each converted graph was
checked against LiteRT on real frames: selfie and face models match to
1e-5 in numpy and to fp16 precision on the GPU (largest landmark error
0.07 px of 192; person mask disagreement 0.02 % of pixels). The hair model
has custom ops LiteRT cannot run, so it was checked layer by layer against
an independent numpy implementation of the raw `.tflite`.

`chat-call-segment.js` runs a graph as one fragment-shader draw per layer
on half-float textures (four channels per texel, channel slices side by
side), with generated shaders per layer: convolution (with dilation),
depthwise, transposed convolution, add, multiply, global mean (two passes),
max pool, argmax pooling and unpooling, channel concat and pad, bilinear
resize, and ReLU, hard-swish, sigmoid and PReLU. Weights are float16
textures. It needs `OES_texture_half_float` with renderable half floats (or
float) and 12 texture units; without them the background picker is hidden
and the face falls back to the old tracker.

Why a bundled model and not "no model downloads": the heuristic tracker
cannot give a head pose or a hair outline, and no browser API segments a
person. The models are part of the app (same origin, same CSP, inlined in
the native page), so nothing is fetched from a third party, and nothing
leaves the device.

**Where segmentation and landmarks come from:**

- **iOS**: Apple Vision for both. The page asks the app for `{landmarks,
  mask}` with each 480-px JPEG; `CallFaceLandmarks.swift` runs
  `VNDetectFaceLandmarksRequest` and `VNGeneratePersonSegmentationRequest`
  (quality `.balanced`, 8-bit output) in one Vision request, box-downsamples
  the mask to 256 px wide and returns it as base64 next to the regions
  (about 50 KB per frame; the page uploads it as a LUMINANCE texture and
  runs the same temporal blend and guided filter). If twelve answers in a
  row arrive without a mask (an old app), the page runs the bundled selfie
  model instead. The hair model runs in the page on iOS too: Vision has no
  hair segmentation. Kept in the page because the compositing, hair model
  and colour transfer already live there; moving compositing native would
  need a second implementation of all of it.
- **Browsers and the Android WebView**: everything in the page. The face
  mesh tracks from its own previous result; BlazeFace runs only to find a
  face again.

**Adaptive quality.** The stage measures each frame from grab to composite
(on iOS, the composite only) and moves between four levels after 24 steady
frames: selfie model every frame/hair every frame and a 320-px guide;
hair every 2nd frame; selfie every 2nd, hair every 3rd, 256-px guide;
selfie every 3rd, hair every 4th, 192-px guide. It steps down above 34 ms
and back up below 20 ms. A skipped hair frame reuses the last hair logits
placed with the current head crop, so the hair mask still moves with the
head.

## Preview before a call, and the stage API (added 2026-10-13)

`/chat` has a **Preview** button (eye) next to the call buttons in a DM. It
opens the camera through the same stage the call uses, mirrored and large,
with the Face and Background pickers and the voice preview controls from
`chat-call-voice.js` (`createVoicePreview` + `mountVoicePreviewControls`:
Voice, "Hear myself", headphone hint). **Start video call** turns the
preview into the call: the camera stream and the stage are kept and the
same canvas track is offered, so there is no camera restart or flash; the
voice preview stops and the call's own voice stage starts. An incoming
call accepted during a preview reuses it the same way. `createChatCalls`
exposes `preview`, `previewing`, `setBackground`, `background` and `stage`.

The apps have the same screen (`DirectCallPreview.swift`,
`DirectCallPreviewScreen` in `DirectCallViews.kt`): an eye button in the
DM header opens the page in preview mode (`preview(media, voice, face,
background, image)`), with Face, Background (thumbnail sheet; "Your photo"
uses the system photo picker), Voice and Hear myself. Start video call
hands the previewing WebView to the call (`WebCallPeerFactory.makePeer` /
`WebCallPeers.makePeer` run `setup` on it instead of loading a new page).
On iOS, in a DM, the preview also has **Record message**: the page records
its own outgoing tracks (`record`, `recordStop`, `recordChunk`,
`recordDrop`) and the app sends the result as a view-once video message
(native/ios/docs/FMChat.md, "Preview stuck on the spinner; recording from
the preview"). The iOS preview shows the WebView from the start, because
WebKit does not start `getUserMedia` in a page that is not on screen.

**Using the pipeline outside a call** (for recorded video messages):

```js
const stage = createFaceStage({ cameraStream, faceId, backgroundId, backgroundImage });
stage.track;                      // MediaStreamTrack, 24 fps canvas capture
stage.setFace(id);                // "" for the caller's own face
stage.setBackground(id, image);   // ids from CALL_BACKGROUNDS; image = data URL for "custom"
stage.face(); stage.background();
stage.stop();
```

It needs no `RTCPeerConnection`; `stage.track` can go straight into a
`MediaRecorder` together with `createVoiceStage(...).track`. With
`{manual: true}` and no camera, `stage.processFrame(source, time)` renders
a given image or video frame on demand (the test harness uses this). In
the apps the same functions are `window.FmCallFaces` in `direct-call.html`;
a record screen can call the page's `preview(...)` operation and then
record from the page, or call `FmCallFaces.createFaceStage` itself.

## Measuring the video pipeline

`app/tools/call_video_harness.cjs` drives the real modules in headless
Chromium (Playwright; set `PLAYWRIGHT_DIR` to an installed
`playwright` package): `render` processes a folder of frames one by one
through `processFrame` and writes the output frames, the landmarks and
timings; `native` loads `direct-call.html` with a fake camera
(`--use-file-for-fake-video-capture` with a `.y4m` clip) and opens a
preview; `models` times each network. `--gpu` uses the machine's GPU
through ANGLE (`--use-angle=gl-egl`), otherwise SwiftShader.
`app/tools/call_video_metrics.py` reports landmark and head-contour jitter
(the median second difference per frame in eye distances) and the mean
frame-to-frame change of the output against the camera.

Test footage (kept out of the repo): public-domain US government videos
from Wikimedia Commons (White House weekly addresses of 8 Oct 2016 and of
the First Lady, May 2014), cropped to 640×360 and 360×640 at 24 fps, plus
a version with synthetic roll (±12°), pan and zoom.

**Measured** (2026-10-13, final code; the AMD RX 580 in this box through
ANGLE/GL, with other builds running on the machine; a mid-range phone GPU
is several times slower, and the adaptive levels exist for that):

- Network times (run + read back): selfie 4.3 ms, face mesh 2.7 ms,
  BlazeFace 2.3 ms, hair 5.1 ms. They are bound by per-draw overhead (67
  to 162 draws each), not arithmetic.
- Whole frame through `processFrame` with a forced GPU sync, 640×360,
  median / p90: camera only 12 / 14 ms (upload, composite and the sync
  itself), blur 15.5 / 18 ms, face + background 22–24 / 25–27 ms (portrait
  360×640: 24 / 27 ms). With SwiftShader (CPU rendering, far slower than
  any phone) a face frame took 260 ms and the stage stepped down to level
  3 as designed.
- Jitter on a still frame with camera noise (σ 2.5 grey levels): landmarks
  0.15 % of the eye distance per frame (p95 0.24 %), head contour 0.28 %;
  the output changed 0.53 grey levels per frame against 1.87 in the
  camera. On the clip with synthetic roll, pan and zoom (mean motion 4.3 %
  of the eye distance per frame): landmarks 1.4 %, contour 1.7 %, output
  change 2.5 against 5.9 grey levels in the camera.
- iOS path, simulated in Chromium with Vision-format replies (regions and
  an 8-bit mask): composited correctly, no in-page selfie model ran;
  72 KB per frame through the bridge (JPEG and mask).

Limits and what is not verified: the apps' new screens and Vision person
segmentation were not run on a device (the iOS app target was only
parse-checked with `swiftc -parse`; the FMChat core and its tests pass on
Linux; Android `:core:coreTest` passes and `:app:assembleDebug` builds,
with no emulator run). Phone frame times were not measured. Turns beyond
about 40° still show a turned flat head, and hair behind the shoulders is
kept. The canvas is driven by `setInterval`, which browsers slow down in a
background tab.

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
  `replaceTrack`. The sync tool inlines the face modules (each in its own
  function scope, so private names cannot clash), writes the photos and
  mattes as data URLs into `<script id="fm-call-face-photos">`, the
  background images into `<script id="fm-call-backgrounds">` and the models
  as base64 into `<script id="fm-call-models">` (the page is about 5 MB).
  On iOS the page asks the app for Apple Vision landmarks and person
  masks; Android runs the bundled models in the WebView. The engines keep
  the background too (`background`, `setBackground(id, image)`,
  `onBackgroundChanged`, the `chat.callBackground` pref).

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

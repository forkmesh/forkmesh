# Face stream

When a call cannot carry real video (below about 150 kbps), the sender stops
sending camera frames and sends a *face stream* instead: per frame, the head's
3D pose, the 68 landmarks and seven expression channels, about 37 bytes at
30 fps (9 kbit/s), plus now and then a *reference picture*, a real, recent
still of the sender's face (about 10 kB of JPEG). The receiver animates the
reference picture with the face changer's warp renderer, so it shows a live,
talking face instead of a frozen frame. This is idea 1 of "Beyond FaceTime" in
[../server/fm-media.md](../server/fm-media.md).

The face pipeline this builds on (landmarks, `liveHead`, `warpVertices`, the
custom-photo stand-in, the native ports) is described in
[chat-direct-calls.md](chat-direct-calls.md), "Head swap", "Native face math"
and "Native face renderer (iOS)".

## Where the code is

| Piece | Code |
| --- | --- |
| Reference codec (Go), fuzz targets, parity fixture generator | `server/rtc/facestream/` |
| Browser: codec, sender helpers, receiver, WebGL renderer, tile | `app/public/chat-call-facestream.js` |
| iPhone and Linux: codec, sender helpers, receiver maths | `native/ios/ForkMeshCore/Sources/FMChat/FaceStream/` |
| iPhone app: Metal renderer, reference capture, tile badge | `native/ios/ForkMesh/Features/Chat/Calls/Native/NativeFaceStream*.swift`, `NativeFaceStream.metal` |
| Android: codec only (no renderer yet) | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/chat/facestream/` |
| Quality harness (headless Chromium) | `tools/face_stream_eval.mjs` |
| Shared fixture | `server/rtc/facestream/testdata/face-stream-parity.json`, copied byte for byte to `native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/` and `native/android/core/src/test/resources/chat/` |

The browser renderer reuses the face changer's warp pass: the face changer's
renderer now exports `FACE_WARP` (its warp shaders and its `compile`,
`texture` and `target` helpers), which changes nothing else in it. The native
call pages that inline the face modules were regenerated for the new export
(`app/tools/sync_native_call_faces.py`).

## Wire format

Two kinds of packet travel on the face stream: frames (key or delta) and
reference pictures. All multi-byte fields are big endian. Every packet starts
with one header byte:

| Bits | Field |
| --- | --- |
| 7-6 | kind: 0 delta frame, 1 key frame, 2 reference picture, 3 refused |
| 5-4 | version, 1 |
| 3-0 | key id (frames) or reference id (pictures), counting modulo 16 |

### Frames

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | header |
| 1 | 1 | sequence number, modulo 256, one per frame |
| 2 | 2 | key frames only: camera frame width in pixels (1-4096) |
| 4 | 2 | key frames only: camera frame height (1-4096) |
| 6 | 1 | key frames only: nominal frame rate (1-120) |
| 7 | 1 | key frames only: precision, landmark steps per eye span (16-255, default 96) |
| 8 | 4 | key frames only: capture time in ms, wrapping |
| 2 or 12 | rest | range-coded payload |

A delta frame carries the key id of the key frame it belongs to. A frame
packet is at most 1024 bytes.

**Range coder.** A binary range coder in the style of LZMA's: 32-bit range,
12-bit probabilities adapting by 1/16 per symbol, carry propagation through a
cache byte. The first output byte (always 0) is not sent, and the encoder
ends at the value with the most trailing zero bytes inside the final interval
and drops trailing zero bytes; the decoder reads zeros past the end. A frame
costs about one byte of coder overhead. Only integer arithmetic is involved,
so the four ports produce the same bytes.

**Integers.** A signed value is a zero flag (with a context: was the previous
value in the same class non-zero), a sign, the bit length in unary (one
adaptive bit per position, at most 16 bits), the bit below the leading one
with an adaptive bit per length, and the remaining bits sent raw. Unsigned
values drop the sign. Values are clamped to ±65535.

**Payload order.**

1. Delta frames: the time since the previous frame minus the nominal interval
   (`round(1000 / fps)` ms), as a signed integer.
2. Pose. The rotation is a unit quaternion with `w >= 0`; `x`, `y`, `z` are
   sent in steps of 1/512 (`w` is rebuilt as `sqrt(1 - x² - y² - z²)` and the
   quaternion renormalised). The translation `X`, `Y` (where the template's
   origin, the midpoint of the eyes, lands, in camera pixels) is sent in half
   pixels, and the scale (pixels per template unit, one unit being the eye
   span) in eighths of a pixel. Key frames send them as raw bits (11, 11, 11,
   16, 16 and 15 bits, offset to be non-negative); delta frames send the
   difference from their key frame's pose as six signed integers.
3. Expression, seven channels sent absolutely in every frame:

   | Channel | Range | Step | Used for |
   | --- | --- | --- | --- |
   | lid closure, image-left eye | 0-1 | 1/16 | `closeLids` and the lid shader |
   | lid closure, image-right eye | 0-1 | 1/16 | same |
   | gaze x | ±0.5 | 1/64 | the gaze shader (both eyes) |
   | gaze y | ±0.5 | 1/64 | same |
   | mouth open (inner-lip gap / half an eye span) | 0-1 | 1/16 | concealment after a loss, reference choice |
   | smile (mouth width over the eye span, 0.96-1.26) | 0-1 | 1/16 | spare, for renderers that want it |
   | tongue | 0-1 | 1/16 | the synthetic tongue |

4. Landmarks: 68 points, x then y for each, in pixel axes, quantised to
   `scale / precision` (one ninety-sixth of the eye span by default, about one
   pixel for a face at a normal distance at 720p). Each coordinate is
   predicted and only the rounded residual is sent:
   - in a key frame, from the codec's 3D template (68 fixed points, the face
     changer's template rounded to 4 decimals) projected through the frame's
     own pose;
   - in a delta frame, from the previous frame's reconstruction moved by how
     the template's projection moved between the two poses
     (`recon[t-1] + proj(pose[t]) - proj(pose[t-1])`), so rigid head motion,
     including turning, costs almost nothing and only the face's own motion
     is sent.

   The encoder predicts from its own reconstruction (closed loop), so the
   error never accumulates: every decoded coordinate is within half a step of
   the input. Residuals use twelve contexts (jaw, brows, nose, eyes, outer
   lips, inner lips, each for x and y). Key frames start their contexts fresh;
   delta frames keep adapting the same contexts from one frame to the next
   until the next key frame.

Pose and expression are decodable from any delta frame whose key frame the
receiver holds; only the landmark residuals depend on the previous frame.

**Losses.** A lost frame breaks the landmark chain until the next key frame
(every 30 frames by default, or sooner when asked). In between, the decoder
still decodes pose and expression and conceals the landmarks: the last good
shape moved by the template's motion between the two poses, marked `stale`.
The receivers then also open and close the mouth from the mouth channel (the
lower lip and chin move by the change in mouth opening since the last good
frame), so speech keeps moving. A receiver that sees `stale`
(`needsKey`) should ask the sender for a key frame (`requestKey()`), which on
FM Media is the existing keyframe request. A delta frame for a key frame the
receiver does not hold is refused (`need-key`); old or repeated sequence
numbers are refused (`old`). A gap in time of more than 4 s makes the sender
send a key frame (it carries the absolute time).

### Reference pictures

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | header (kind 2, reference id) |
| 1 | 4 | capture time in ms |
| 5 | 2 | crop x in camera pixels |
| 7 | 2 | crop y |
| 9 | 2 | crop width (at least 1) |
| 11 | 2 | crop height (at least 1) |
| 13 | 2 | picture width (1-1024) |
| 15 | 2 | picture height (1-1024) |
| 17 | 272 | the picture's own 68 landmarks, x then y, signed 16-bit, in 1/16 picture pixels |
| 289 | 1 | image format: 0 JPEG, 1 HEIC, 2 WebP |
| 290 | 1 | mask width (0-128, 0 for none) |
| 291 | 1 | mask height (0 exactly when the width is 0) |
| 292 | 2 | coded mask length |
| 294 | n | person mask, one bit per cell, range coded with a context of the left, upper, upper-left and upper-right cells |
| 294 + n | rest | image bytes (at least one) |

A reference packet is at most 256 KB; the transport fragments it. The
landmarks let the receiver build the warp model without running a face
detector; the crop places the picture in the camera frame, so frame landmarks
(in camera pixels) map into it. A 64 x 80 mask codes to 44 bytes on the test
blob. JPEG is the default because every receiver decodes it; HEIC is only for
calls where every receiver says it can decode it (browsers other than Safari
cannot).

## Bitrate and error

Go, synthetic head (turning ±17°, nodding, rolling, talking, blinking, half a
pixel of tracker noise), 600 frames at 1280 x 720 and 30 fps
(`go test -v -run RoundTrip ./rtc/facestream/`):

| Precision | Bytes/frame | Key | Delta | kbit/s | RMS error | Worst |
| --- | --- | --- | --- | --- | --- | --- |
| 48 | 32.5 | 57.9 | 31.7 | 7.8 | 0.82 px | 1.03 px |
| 64 | 36.0 | 63.7 | 35.1 | 8.7 | 0.61 px | 0.77 px |
| 96 (default) | 42.0 | 73.5 | 40.9 | 10.1 | 0.41 px | 0.52 px |
| 128 | 47.1 | 81.5 | 45.9 | 11.3 | 0.31 px | 0.39 px |

Error is the decoded landmarks against the sender's landmarks, in pixels at
720p (per point, both axes). The worst case is always half a step.

Browser harness (below), the stand-in "theo" as the sender, the face mesh's
One Euro smoother on 0.6 px of tracker noise, the pose solved from the
landmarks as the real sender does:

| Precision | Bytes/frame | kbit/s | RMS error | Worst |
| --- | --- | --- | --- | --- |
| 48 | 27.9 | 6.7 | 0.81 px | 1.06 px |
| 64 | 31.7 | 7.6 | 0.62 px | 0.79 px |
| 96 (default) | 36.8 | 8.8 | 0.41 px | 0.53 px |
| 128 | 41.1 | 9.9 | 0.31 px | 0.40 px |

Key frames there cost 84-100 bytes (the person's shape against the template),
delta frames 35-37 bytes. With one 256 x 320 JPEG reference (about 10 kB)
every 10 s the whole stream is about 22 kbit/s, under a seventh of the
150 kbps where video gives up, and the reference refresh is the knob: every
20 s halves its share.

## Sender

Per camera frame the face pipeline already has the 68 landmarks (smoothed),
the expression probe's reading (or ARKit blend shapes) and, with TrueDepth or
the face mesh, a depth per landmark. The sender:

1. solves the pose against the codec's template with the face changer's
   `solvePose` (3D when the landmarks carry depth, the 2D fit otherwise) and
   turns its rotation rows into a quaternion (`quatFromRows`);
2. maps the reading to the seven channels (`faceStreamExpression`): lid
   closures and the mean gaze of both eyes from the reading, mouth opening
   from the inner lips, smile from the mouth width, tongue from the reading;
3. encodes the frame (`sender.frame(...)`, `FaceStreamSender.frame`).

**When to grab a reference picture** (`referenceScore`, `referenceDue`):

- the face is frontal: |yaw| and |pitch| under 0.26 rad (15°), |roll| under
  0.35 rad;
- the eyes are open: both lid closures under 0.2;
- the mouth is closed: opening less than 0.12 above the sender's own resting
  opening (a minimum that rises by 0.002 per frame, so a person whose lips
  part at rest still gets pictures);
- no tongue;
- good light: the luma over the eyes, nose and mouth averages 0.22-0.85 with a
  standard deviation of at least 0.035;
- the eye span is at least 36 px;
- at most one picture every 2 s; a new one when the current one is 10 s old,
  or earlier when the new score beats the current picture's by 0.15.

The score is 0.45 frontal + 0.2 eyes + 0.2 mouth + 0.15 light, each 1 at its
best. **The crop** (`referenceCrop`) is 4.6 eye spans tall and 4:5 wide,
centred 0.35 eye spans below the eyes (head, hair and neck with a margin),
shrunk to fit inside the camera frame and moved inside it. It is scaled to
256 x 320 and encoded as JPEG at quality 0.72. On iPhone the picture is cut
from the upright camera image with Core Image, the light is measured on it,
the score is checked again with the light, Vision's person segmentation runs
on the 256 x 320 picture for the mask, and the work happens on a utility
queue, one picture at a time (`NativeFaceStreamSender`). In the browser the
caller passes a person mask when the call already has one (the face changer's
matte, in camera pixels); without a mask the receiver uses the stand-in
oval.

## Receiver

**Mapping.** Frame landmarks are in camera pixels; the tile shows the current
reference's crop, so each point maps by `(p - crop) * tile / crop size`. Every
point also gets a depth, `scale * (r3 · template point)` from the transmitted
rotation, so `liveHead` solves the pose in 3D and never guesses which way the
head is turned.

**Browser** (`createFaceStreamView`, `createFaceStreamRenderer`). When a
reference arrives (`prepareFaceStreamReference`):

1. the JPEG is decoded (`createImageBitmap`);
2. the face changer's custom-photo builder (`buildCustomFace`) turns the
   picture, its transmitted landmarks and the mask into a 512 px stand-in:
   photo, matte (face, skin, hair) and head ring; `faceHead` builds its
   layout, 3D model and mesh;
3. the backdrop is built on the CPU.

Then per frame: `closeLids` with the decoded lid closures, `liveHead`,
`warpVertices` with `vertexDepths`, and two GPU passes: the face changer's
warp shaders draw the photo and its aux values into two targets with a depth
test (gaze and eyelid uniforms as in the face changer), and a composite draws
the backdrop, the mouth interior, a lip shadow, and the warped face over it
with a faint grain.

**Backdrop (chosen).** The reference picture's own surroundings, softly
blurred: inside the head zone (the face changer's head ellipse, 2.55 x 2.9
eye spans around a point 0.35 eye spans above the eyes, down to the chin)
where the mask says person, the picture is filled from the room around it by
a push-pull pyramid, then the whole picture is blurred (two box passes of
radius 2 at 256 x 320). The moving head never leaves a ghost of the head that
was there when the picture was taken, the room and the shoulders stay where
they were, and the softness keeps them from competing with the face. Without
a mask nothing is filled (the original head stays behind the warped one,
which only shows at the edges of large movements). A neutral backdrop was
rejected: it removes the person's own setting, which is most of what makes
the tile look like their video. Before the first reference the tile is a
flat grey (`#2c2e34`).

**Mouth.** The reference is taken with the mouth closed, so the warp opens a
hole between the lips that has nothing behind it (the face changer shows the
camera there). The composite fills it: a dark red cavity shaded deeper
towards the inner ring, upper teeth along the top of the opening, a hint of
lower teeth, and a tongue that rises with the tongue channel, all placed in
the mouth's own frame (inner-lip centre, the 60-64 axis, the 62-66 gap).

**iPhone** (`NativeFaceStreamRenderer`). The same steps: the reference
becomes a `NativeFaceAsset` through `CallFaceCustom` with the transmitted
landmarks (no Vision on the receiver) and the mask drawn through the same
transform; the backdrop comes from `FaceStreamView.removal` and
`FaceStreamView.backdrop` (the same fill and blur as the browser); the warp
pass reuses `fmFaceWarpVertex` and `fmFaceWarpFragment` with an identity
buffer mapping; `fmFaceStreamComposite` (`NativeFaceStream.metal`) is the
browser's composite. It renders into a BGRA `CVPixelBuffer` from a pool, ready
to wrap as a video frame for the remote tile. References are prepared on a
loader queue; frames keep drawing with the previous reference until the new
one is ready.

**Android.** The codec, the reference packets and the fixture test only. The
OpenGL renderer comes with the face changer's Android renderer.

## Privacy

- A reference picture is a real photo of the sender. It is only taken while
  the sender's camera is on and the call would otherwise send their video,
  and it shows no more than that video would: a crop of the same camera
  frame. Turning the camera off stops the face stream and the pictures.
- Face-stream packets are media like any other and must be end-to-end
  encrypted with the call's media keys before they leave the device (the FM
  Media engine's frame encryption, or the WebRTC path's insertable streams),
  exactly as camera frames are. The SFU and the servers see sizes and timing
  only, never landmarks or pictures. The modules here produce plaintext
  packets and leave encryption to the transport, like the video encoder.
- Pictures and landmarks are kept only in memory for the call; the receiver
  holds the newest picture and drops it when the call ends.
- The receiver must label the tile so viewers know they are not seeing live
  video: an animated photo can look real.

## UI label

| Where | Text |
| --- | --- |
| Badge on the tile | **Face stream** |
| Badge while frames are being concealed after a loss | **Face stream: catching up** |
| Tooltip and accessibility label | "Face stream. Animated from a recent photo while the connection is weak" |

The strings are `FACE_STREAM_LABEL`, `FACE_STREAM_STALE_DETAIL` and
`FACE_STREAM_DETAIL` in the browser and `FaceStream.label`, `.staleDetail`
and `.detail` on the phones (a test checks they match). The browser tile
draws the badge itself (bottom left, white on 55% black); the iPhone has
`NativeFaceStreamBadge`.

## Switching (to be wired)

The FM Media engine decides when real video no longer fits (its publisher
hints, [../server/fmm-protocol.md](../server/fmm-protocol.md)). The intended
wiring, not yet done:

- below about 150 kbps the sender publishes the face stream in place of the
  video track: key frames as FM Media keyframes, delta frames with reference
  delta 1, reference pictures as their own keyframe-flagged frames (they are
  fragmented like video);
- the receiver swaps the tile to the face-stream view and shows the badge;
- the engine's keyframe request maps to `requestKey()`;
- when 1.5 x the lowest video layer fits again, the camera track comes back
  and the badge goes away.

## Quality harness

```sh
node tools/face_stream_eval.mjs /tmp/face-stream --face theo   # also --frames, --every, --precision, --loss
```

It runs in headless Chromium (SwiftShader, through `playwright-core` from
`app/browser_tests/node_modules`, as `tools/call_segment_parity.mjs` does):

1. **Ground truth video.** One of the face changer's stand-in photos is warped
   with the face changer's own warp onto the office background at 1280 x 720,
   driven by a synthetic sequence with known landmarks: the stand-in's own
   shape, posed and turning, talking (jaw and lower lip), smiling, blinking
   every 3.2 s, the gaze moving, the tongue out for 0.8 s every 5 s.
2. **Sender.** The landmarks get 0.6 px of tracker noise and go through the
   face mesh's One Euro smoother; the pose is solved from them; the
   reference policy picks pictures from the rendered video, with the person
   mask taken from where the frame differs from the background.
3. **Receiver.** Frames are decoded and rendered at 320 x 400 three ways:
   decoded landmarks, the sender's exact landmarks (to isolate the codec), and
   decoded with 10% random frame loss.
4. **Report** (`face-stream-eval.json`): bytes per frame (key and delta),
   kbit/s with and without references, landmark error against the sender's
   landmarks and against the noiseless truth, and PSNR and SSIM of the
   rendered tile against the ground-truth video cropped the same way (whole
   tile and the face region), next to the same for a frozen frame (the
   reference picture itself, which is what a stalled video would show).
   Strips (truth, face stream, face stream with loss) are saved as PNGs every
   36 frames.

Measured on 2026-10-21, 180 frames (6 s), precision 96, one reference:

| Sender | Bytes/frame | Total kbit/s | Face PSNR (stream / frozen) | Face SSIM (stream / frozen) | Codec only, PSNR | With 10% loss, face PSNR |
| --- | --- | --- | --- | --- | --- | --- |
| maya | 38.6 | 22.7 | 21.2 / 12.9 dB | 0.73 / 0.32 | 46.7 dB | 20.3 dB |
| theo | 36.8 | 22.3 | 20.5 / 14.5 dB | 0.76 / 0.43 | 47.0 dB | 20.3 dB |
| kofi | 37.1 | 22.5 | 19.6 / 15.0 dB | 0.72 / 0.40 | 48.1 dB | 19.3 dB |

- The codec costs almost nothing visible: the tile from decoded landmarks is
  47 dB from the tile drawn with the exact ones.
- Against a frozen frame the face stream gains 5-8 dB on the face and roughly
  doubles its SSIM.
- What remains between it and the truth is the reference's resolution (a
  256 x 320 JPEG upscaled), where the reference's hair outline differs from
  the original photo's, and the landmark smoother's lag (2.7 px RMS from the
  noiseless truth, the same as the face changer has).
- With 10% loss and a key frame every second, 107 of 180 frames were drawn
  from concealed landmarks; with the mouth concealment their landmarks stay
  1.4-1.6 px RMS from the sent ones (2.3 px without it) and the face PSNR
  drops by under 1 dB. Asking for a key frame on loss would shorten those
  runs.
- The truth here is itself a warped photo, so these numbers say how well the
  stream reproduces a moving face, not how close a warped photo comes to a
  real camera. That needs a recorded call.

## Parity and tests

The Go package is the reference. `TestParityFixtureIsCurrent` builds
`testdata/face-stream-parity.json` from two synthetic streams (75 frames at
720p with a requested key frame and two dropped frames; 40 frames at 640 x 480,
24 fps, precision 48, key interval 12, a 6 s time jump, decoding from frame 3
so the first deltas are refused) and two reference pictures (with and without
a mask): inputs, packets (hex), and every decoded frame or error. Each port
encodes the inputs and must produce the same bytes, and decodes the packets to
the same frames (within 1e-9; in practice 0 in JavaScript and Kotlin and 6e-16
in Swift, from Foundation's JSON number parsing). Regenerate after a format
change:

```sh
cd server && FACESTREAM_WRITE_FIXTURE=1 GOTOOLCHAIN=local go test ./rtc/facestream/
```

| Port | Command | Result on 2026-10-21 |
| --- | --- | --- |
| Go | `GOTOOLCHAIN=local go test -race ./rtc/facestream/...` (from `server/`) | pass |
| Go fuzz | `go test -run X -fuzz FuzzDecode$ -fuzztime 60s ./rtc/facestream/` and `FuzzDecodeReference` | 220k and 7.7M runs, no failure |
| Browser | `pytest app/tests/test_chat_call_facestream.py` | 12 pass; 115 packets identical |
| iPhone (Linux) | `FM_ONLY=FMChat swift test --filter FaceStream` (from `native/ios/ForkMeshCore`) | 10 pass; 115 packets identical |
| Android | `./gradlew --offline :core:coreTest -Pfilter=FaceStream` (from `native/android`) | 4 pass; 115 packets identical |

The decoders bound everything they read: frame packets at 1024 bytes,
reference packets at 256 KB, sizes and pose values against their ranges, and
the range decoder is refused if it reads more than 8 bytes past the end. The
Go fuzz targets check that no input panics or yields a non-finite point; the
Swift, Kotlin and JavaScript tests feed a few hundred random packets each.

## Not done yet

- Wiring into the call: publishing on FM Media, the switch at the bitrate
  threshold, the tile swap in `chat-calls.js` and the native call screens.
- An Android renderer.
- A measurement on a real recorded call and on an iPhone (the Metal renderer
  is compiled but has not drawn on a device).
- HEIC references between iPhones.

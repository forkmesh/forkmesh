# Face stream

When a call cannot carry real video (below about 150 kbps), the sender stops
sending camera frames and sends a *face stream* instead: per frame, the head's
3D pose, the 68 landmarks and seven expression channels, about 37 bytes at
30 fps (9 kbit/s), plus now and then a *reference picture*, a real, recent
still of the sender's face (about 10 kB of JPEG). The receiver animates the
reference picture with the face changer's warp renderer, so it shows a live,
talking face instead of a frozen frame. Now and then the sender also sends a
*mouth atlas*: a few tiny crops of its own mouth (closed, open, wide, round),
so the receiver can show real lips, teeth and a real mouth interior instead of
a painted one. This is idea 1 of "Beyond FaceTime" in
[../server/fm-media.md](../server/fm-media.md).

Version 2 of the receiver (October 2026, "v2" below) reworked the hair edge,
the mouth, the body and the evaluation after the first renders showed slivers
of background around the hair, a flat dark band for a mouth, a smeared torso
and a "truth" that was itself a warped photo. Everything that changed and why
is in "Receiver" and "Quality harness"; the measured before/after is in
"Quality harness".

The face pipeline this builds on (landmarks, `liveHead`, `warpVertices`, the
custom-photo stand-in, the native ports) is described in
[chat-direct-calls.md](chat-direct-calls.md), "Head swap", "Native face math"
and "Native face renderer (iOS)".

## Where the code is

| Piece | Code |
| --- | --- |
| Reference codec (Go), fuzz targets, parity fixture generator | `server/rtc/facestream/` |
| Browser: codec, sender helpers, receiver, WebGL renderer, tile | `app/public/chat-call-facestream.js` |
| The 68-point 3D template (shared with the World's avatar rig) | `app/public/chat-call-face-template.js`, re-exported as `FACE_STREAM_TEMPLATE` |
| iPhone and Linux: codec, sender helpers, receiver maths | `native/ios/ForkMeshCore/Sources/FMChat/FaceStream/` |
| iPhone app: Metal renderer, reference capture, tile badge | `native/ios/ForkMesh/Features/Chat/Calls/Native/NativeFaceStream*.swift`, `NativeFaceStream.metal` |
| Android: codec only (no renderer yet) | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/chat/facestream/` |
| Receiver maths for the phones (matte, plate, mouth atlas, warp anchor) | `native/ios/ForkMeshCore/Sources/FMChat/FaceStream/FaceStreamLayers.swift` |
| Quality harness (headless Chromium) | `tools/face_stream_eval.mjs` |
| Real talking-head test clips, pinned by hash, with their licences | `tools/testdata/face-stream/` (`clips.json`) |
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
| 7-6 | kind: 0 delta frame, 1 key frame, 2 reference picture, 3 mouth atlas |
| 5-4 | version, 1 |
| 3-0 | key id (frames), reference id (pictures) or atlas id, counting modulo 16 |

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
and drops trailing zero bytes, at most 8 of them; the decoder reads zeros past
the end. A frame costs about one byte of coder overhead.

The cap of 8 matches the decoder's sanity check (a frame that reads more than
8 bytes past its end is refused as corrupt). The decoder reads exactly as many
bytes as the encoder wrote before trimming, so trimming more than 8 would make
a valid frame look corrupt. That happened: a face that holds perfectly still
codes every symbol as its most likely value, the whole payload is zero bytes,
the first encoder trimmed all of them, and every decoder refused the next delta
frame. The first real-video run hit it within 31 frames. Packets that trimmed
8 or fewer zeros are unchanged, so older fixtures still match; the "still"
stream in the fixture locks the behaviour in all four ports. Only integer arithmetic is involved,
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
blob, the 96 x 120 mask the senders now send (to give the receiver's matte
refinement a finer start, see "Receiver") to about 62 bytes on a similar
shape. JPEG is the default because every receiver decodes it; HEIC is only for
calls where every receiver says it can decode it (browsers other than Safari
cannot).

### Mouth atlas

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | header (kind 3, atlas id) |
| 1 | 4 | capture time in ms of the newest tile |
| 5 | 1 | tile width in pixels (8-128, default 64) |
| 6 | 1 | tile height (8-128, default 48) |
| 7 | 1 | tile count n (1-8) |
| 8 | 1 | image format: 0 JPEG, 1 HEIC, 2 WebP |
| 9 | 83 per tile | state (0 closed, 1 open "ah", 2 wide "ee", 3 round "oo"), mouth and smile channels when it was taken (0-16, sixteenths, as in frames), then the tile's 20 lip landmarks (48-67), x then y, signed 16-bit in 1/16 tile pixels |
| 9 + 83n | rest | one image: the n tiles side by side, n x tile width by tile height |

An atlas packet is at most 64 KB and always complete: a receiver replaces its
atlas with the newest one, so a lost atlas only delays the next. A tile is
upright: it is cut along the mouth's own axis (48 to 54), centred on the outer
lip, 1.6 mouth widths wide, so its lip landmarks are what the receiver warps
from. Four 64 x 48 tiles are one 256 x 48 JPEG of 2-3 kB (one JPEG instead of
four saves about 600 bytes of tables per tile). Older decoders refuse kind 3
like any unknown kind, and the frame decoders refuse it as `not-a-frame`.

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

**Picture budget.** References and atlases share a byte budget in the sender:
a bucket of 10.5 kB that refills at 1.4 kB/s (11 kbit/s). A picture is only
sent when the bucket holds at least the size of the last one of its kind.
With frames at 7-9 kbit/s, any window of 8 s or more stays at or under about
30 kbit/s; over a long call it settles near 8 kbit/s of pictures (a reference
every 10 s, an atlas when a mouth state is refreshed, at most once a minute
per state). Without it the better-picture rule replaced references every few
seconds while the person settled, and an 8 s clip came to 31-40 kbit/s.

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
- the eyes are open: both lid closures under 0.2, where the closure is the
  larger of the probe's reading and how far the eyes' landmark aspect ratio
  (`faceStreamEyeOpening`, the two lid gaps over the eye width) falls below
  the widest the sender has seen (at least 0.25, forgetting 0.0005 per
  frame);
- the mouth is closed: opening less than 0.12 above the sender's own resting
  opening (a value that starts at 0 and rises by at most 0.006 per frame
  towards the smallest opening seen, so a person whose lips part at rest
  still gets pictures after two or three seconds);
- no tongue;
- good light: the luma over the eyes, nose and mouth averages 0.22-0.85 with a
  standard deviation of at least 0.035;
- the eye span is at least 36 px;
- at most one picture every 4 s; a new one when the current one is 10 s old,
  or earlier when the new score beats the current picture's by 0.15;
- the picture budget (see "Bitrate and error") has room for it.

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
oval. The mask is sent at 96 x 120.

The eye and rest-mouth rules came from the first real clip: the probe reported
no lid closure at all for a man looking down with half-closed eyes, and the
old rest mouth started at the first frame's opening, so frame 0 (eyes lowered,
mouth open on his teeth) became the reference and every later frame showed
half-shut eyes and the old teeth.

**Mouth atlas** (`mouthTile`, `atlasDue`, `atlas`; Swift `mouthCandidate`,
`storeMouthTile`, `atlasSheet`, `atlas`). Every frame the sender classifies
the mouth (`mouthState`): closed (opening at most 0.05), open (at least 0.25),
or, between 0.1 and 0.25, wide when the mouth is at least 6% wider than its
own closed width and round when at least 6% narrower (the width is measured
from the landmarks against an average of the closed width, because the
transmitted smile channel saturates at 0 for many faces). The face must be
within 0.35 rad of frontal. A state's tile is cut (64 x 48, rotated upright)
when the state has none yet, its tile is a minute old, or the new frame fits
the state better by 0.15. An atlas is sent when a tile changed, at least one
non-closed tile exists (a closed mouth is what the reference already shows),
3 s have passed since the last atlas, and the budget has room. On iPhone the
tile is cut with Core Image and Core Graphics on the picture queue
(`considerMouth`).

## Receiver

**Mapping.** Frame landmarks are in camera pixels; the tile shows the current
reference's crop, so each point maps by `(p - crop) * tile / crop size`. Every
point also gets a depth, `scale * (r3 · template point)` from the transmitted
rotation, so `liveHead` solves the pose in 3D and never guesses which way the
head is turned.

**Browser** (`createFaceStreamView`, `createFaceStreamRenderer`). When a
reference arrives (`prepareFaceStreamReference`):

1. the JPEG is decoded (`createImageBitmap`);
2. **matte** (`faceStreamMatte`): the 1-bit mask is upsampled bilinearly and
   refined by a guided filter on the picture's own luma (radius 4 px,
   eps 0.002), only inside a band around the coarse edge; deep inside stays 1,
   far outside 0. Hair strands and the outline follow the picture, not the
   mask's 2.7 px cells;
3. **layers** (`faceStreamLayers`): the *head* layer is the matte faded out
   between 0.25 and 0.9 eye spans below the chin (head, hair, neck top); the
   *removal* is the matte faded out between 0.15 and 0.6 below the chin,
   grown by 2 px;
4. **plate** (`faceStreamPlate`): the picture with the removal filled from the
   room around it by a push-pull pyramid (bilinear upsampling between levels;
   the first version picked the nearest coarse pixel, which left 30 px blocks),
   blurred only inside the fill. Outside the head the plate is the picture
   itself, crisp: the room, and the shoulders and torso;
5. **unmix** (`faceStreamUnmix`): where the matte is partial (hair edges),
   the plate's colour is taken out, `F = (I - (1 - a) P) / a`, so a moved
   strand carries the hair's colour and not the wall it was in front of. This
   and step 2 are what removed the slivers of original background at the hair;
6. **stand-in photo** (`buildFaceStreamFace`): the unmixed picture, padded by
   20% on each side by repeating its edge pixels (a head cut by the top of the
   camera frame no longer shows a straight cut when it moves down), is placed
   in a 512 px photo at 96 px per eye span with the eyes at (256, 210). The
   face changer's placement (118 px per span, eyes at y 285) ended 0.17 spans
   below the chin, so the neck could not move with the head. The face
   changer's `buildCustomMatte` supplies the skin and hair channels; the
   matte channel is the head layer, and the head ring is traced on it;
7. **head model from the raw landmarks** (`faceStreamHead`): the layout,
   model and mesh are built directly from the transmitted landmarks. The face
   changer's `faceHead` first resamples them into its canonical shape, which
   suits stand-in photos but moved this person's eyes, lips and jaw away from
   the tracker's own points;
8. the mouth colours for the painted interior (`faceStreamMouthColours`): the
   lips' mean colour and, when the picture shows teeth, their colour; otherwise
   ivory scaled to the skin's brightness.

Then per frame: `closeLids` with the decoded lid closures, `liveHead` for the
3D motion of the parts the landmarks do not cover, and
`faceStreamWarpPoints`: the landmarks from the brows down are the live ones
(inner lips that cross are joined), and every other vertex is `liveHead`'s
position plus an **anchor** offset (`faceStreamAnchor`, computed once per
reference and tile size, rotated and scaled with the face) equal to where
`liveHead` puts that vertex for the reference's own landmarks versus where it
really is in the picture. The face changer's head fit is built for another
person's photo; for the same person it pushed the hair and ears about 10%
outwards, so the identity frame (the reference's own landmarks) rendered at
18 dB from the reference picture. With the anchor it renders at 43 dB, and
motion only adds what `liveHead` changes from there.

Two GPU passes as before (the face changer's warp shaders, with the photo
sampled without the face changer's 0.35 mip bias, which blurred a picture
that is already close to tile resolution), then, when an atlas covers the
current mouth, a third: the mouth layer. The composite draws the plate, the
mouth interior where the warp opened a hole, a lip shadow, the warped head
(its alpha is the head layer), and the mouth layer over it, with a faint
grain.

**Body (chosen): static and crisp.** The shoulders and torso stay where the
reference picture has them, sharp, in the plate. A body that follows the
head's translation was built and rendered first (the picture's lower part as
its own layer, moved by 0.7 of the motion of a neck pivot projected through
the head pose, scaled with the head): on the real clips the body barely moves
while the head turns and nods, so the layer slid against the static neck,
opened bright gaps along the collar and lowered the face PSNR. The plate keeps
the neck and collar; the head layer's neck fades out over them, so a nod or a
turn stretches the neck naturally.

**Mouth.** With an atlas (`prepareFaceStreamAtlas`, `faceStreamMouthBlend`,
`faceStreamMouthMesh`): of the tiles opened at least half as wide as the live
mouth (and at least 0.08), the two closest in mouth opening, smile channel
and inner-lip shape (gap over width, from the landmarks on both sides) are
blended. A mesh of the inner-lip fan, the lip band and a feather ring (the
outer lip pushed out by 35%) maps each tile's own lip landmarks onto the live
ones, so the tile's teeth and dark interior sit exactly in the opening and its
lips over the warped lips (alpha 1 inside, 0.85 on the lips, 0 at the
feather). Each tile is colour-matched to the reference by the ratio of the
skin ring around the lips in both (`faceStreamAtlasGains`). The layer fades
in between openings of 0.08 and 0.2 and out when the nearest tile is far
(distance 0.3-0.6). Closed tiles are never used for the interior: their inner
lips meet, so they would stretch a line of lip over the opening.

Without an atlas (and under it where it is partial) the hole is painted in
the mouth's own pixel frame: a cavity from the lip colour (30% at the lips to
7% deep inside, darker at the centre), upper teeth of a fixed physical height
(0.12 eye spans, shorter towards the corners, slight gaps, shaded under the
upper lip) that appear between gaps of 0.04 and 0.16 eye spans, lower teeth
and the floor of the tongue only for wide openings, and, for gaps under 0.1
spans, a blend to the lip colour so a nearly closed mouth reads as lips
touching, not a black slit.

**iPhone** (`NativeFaceStreamRenderer`). The same steps through
`FaceStreamView` in FMChat (`matte`, `layers`, `grow`, `plate`, `unmix`,
`padPixels`, `photoTransform`, `face`, `anchor`, `warpPoints`, `mouthShape`,
`mouthColours`, `mouthBlend`, `mouthMesh`, `atlasGains`), with Core Graphics
drawing the padded picture and layers into the 512 px photo. The Metal
shaders are in `NativeFaceStream.metal`: `fmFaceStreamWarpVertex` and
`fmFaceStreamWarpFragment` (the face changer's warp without the mip bias),
`fmFaceStreamMouthVertex` and `fmFaceStreamMouthFragment`, and
`fmFaceStreamComposite`, the browser's composite. It renders into a BGRA
`CVPixelBuffer` from a pool, ready to wrap as a video frame for the remote
tile. References and atlases are prepared on a loader queue; frames keep
drawing with the previous ones until the new ones are ready.

**Android.** The codec (frames, reference pictures, mouth atlases) and the
fixture test only. The OpenGL renderer comes with the face changer's Android
renderer.

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
node tools/face_stream_eval.mjs <dir> --clip keshav --baseline HEAD   # a real clip, compared with the renderer at a git revision
node tools/face_stream_eval.mjs <dir> --face theo                     # a stand-in photo; also --frames, --every, --precision, --loss, --stills
```

It runs in headless Chromium (SwiftShader, through `playwright-core` from
`app/browser_tests/node_modules`, as `tools/call_segment_parity.mjs` does).

**Sources.**

- `--clip`: real camera footage. Two talking-head clips cut from Wikimedia
  Commons are stored in `tools/testdata/face-stream/` and pinned by SHA-256 in
  `clips.json`, which the harness checks before it runs:
  - `keshav-talking.webm`: "WIKITONGUES- Keshav speaking Maithili",
    Wikitongues, recorded by Hardeep Singh, CC BY-SA 4.0. A webcam-style head
    and shoulders against kitchen cabinets: beard, grey hair cut by the top of
    the frame, small head movements, speech.
  - `frania-talking.webm`: "Wikipedians speak - Konkani Wikipedian Frania
    Pereira", Subhashish Panigrahi, CC BY-SA 3.0. A close-up with a fringe
    against a bright window, wide mouth movements and visible teeth.

  Both are 8 s at 1080 x 606 and 25 fps, without audio, re-encoded as VP9
  (`clips.json` lists the source page, the source file's SHA-1, the segment
  and the changes). The trimmed clips are adaptations, so they carry the same
  licences; credit the authors as above. The clip is decoded in the page from
  a `blob:` URL (a routed response is not seekable, and seeking it silently
  returned frame 0). Each frame is tracked by the face changer's face mesh
  with its One Euro smoother, the expression probe reads it, and the person
  mask for a reference is the face changer's segmentation matte. The truth is
  the real frame.
- `--face`: the old synthetic source, kept for the browser test. A stand-in
  photo warped onto the office background with known landmarks (turning,
  talking, smiling, blinking, the tongue out), 0.6 px of tracker noise, the
  mask from where the frame differs from the background. Its truth is itself
  a warped photo.

**Pipeline.** For each version (the working tree as v2, and with `--baseline`
the same module served from a git revision as v1, each with its own sender
policy) the sender encodes every frame and picks references and atlases; the
receiver is the module's own `createFaceStreamView`, fed the packets in order.
v2 is also drawn from the sender's exact landmarks (codec only) and with 10%
of frame packets dropped.

**Metrics** (`face-stream-eval.json`): bytes per frame, kbit/s with pictures,
references and atlases sent, PSNR and SSIM of the 320 x 400 tile against the
real frame cropped the same way (whole tile, the face box, the mouth box), the
same for a frozen frame (the reference picture), and a lip-sync proxy: the
face mesh is run on the truth tile and on each rendered tile, and the
inner-lip gap over the eye span is correlated over all frames (Pearson r) and
compared (mean absolute error). The proxy measures what a viewer sees, but it
is noisy: the tracker's gap on a 320 x 400 tile has about 0.02 of jitter.
`compare-NNN.png` shows truth, v1 and v2 side by side with the mouths at 2x;
`face-stream-NNN.png` shows truth, v2 and v2 with loss.

Measured on 2026-10-21, v1 = commit c368c4b198, precision 96, 10% loss for the
loss column:

| Source | Version | Total kbit/s (refs + atlases) | Face PSNR | Face SSIM | Mouth PSNR | Tile PSNR | Lip sync r | Frozen face PSNR |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keshav (real, 200 frames) | v1 | 16.7 (1 + 0) | 19.0 dB | 0.648 | 23.6 dB | 19.6 dB | 0.79 | 17.1 dB |
| | v2 | 21.1 (1 + 2) | 21.9 dB | 0.734 | 25.9 dB | 22.0 dB | 0.72 | 17.0 dB |
| frania (real, 200 frames) | v1 | 39.8 (3 + 0) | 17.2 dB | 0.614 | 19.1 dB | 17.1 dB | 0.80 | 16.5 dB |
| | v2 | 23.5 (1 + 3) | 17.3 dB | 0.616 | 19.6 dB | 17.4 dB | 0.86 | 14.9 dB |
| theo (stand-in, 180 frames) | v1 | 22.6 (1 + 0) | 20.4 dB | 0.753 | 22.5 dB | 20.6 dB | 0.59 | 14.4 dB |
| | v2 | 28.7 (1 + 2) | 22.9 dB | 0.831 | 22.8 dB | 23.9 dB | 0.57 | 14.4 dB |
| maya (stand-in) | v1 | 23.1 (1 + 0) | 20.9 dB | 0.717 | 20.7 dB | 20.2 dB | 0.61 | 12.7 dB |
| | v2 | 25.7 (1 + 1) | 22.2 dB | 0.741 | 20.6 dB | 19.5 dB | 0.63 | 12.1 dB |
| kofi (stand-in) | v1 | 22.8 (1 + 0) | 19.5 dB | 0.710 | 23.8 dB | 20.2 dB | 0.49 | 15.0 dB |
| | v2 | 29.1 (1 + 2) | 23.3 dB | 0.816 | 24.6 dB | 24.3 dB | 0.59 | 15.0 dB |

v2 with 10% loss: keshav 20.4, frania 16.6, theo 22.4, maya 20.8, kofi 22.8 dB
face PSNR. The codec alone stays 41.6-46.7 dB from the tile drawn with the
exact landmarks. Frames cost 6.6-7.2 kbit/s on the real clips (keys 103-108
bytes, deltas 30-33 bytes at 25 fps) and 8.8-9.3 kbit/s on the stand-ins at
30 fps.

- On the real clips the face gains up to 2.9 dB and 0.09 SSIM (keshav), the
  mouth 0.4-2.3 dB, at a total of 21-24 kbit/s. Frania's face numbers are
  level with v1's, but v1 needed three references (40 kbit/s) to get there
  and v2 one, inside the budget.
- What the numbers do not show and the strips do: v1's hair carried slivers
  and flecks of the original background, its face was soft (the mip bias), a
  seam ran down the cheek where the head layer was clipped, the shoulders were
  a smear and the mouth a white line. In v2 the head sits in the room like
  video, the shoulders are sharp, and with the atlas the open mouth shows
  the person's own lips, teeth and dark interior.
- Lip sync: on frania the rendered mouth follows the camera better than v1's
  (0.86 against 0.80); on keshav, whose mouth barely opens (gap 0.01-0.22),
  slightly worse (0.72 against 0.79). v1's white line between the lips is
  easy for the tracker to measure; the real interior is darker and closer to
  the lips' colour.
- The rest of the gap to the camera is geometry. A 68-point warp cannot follow
  every crease of a real face, the reference shows the face at one moment,
  and the face mesh's smoother lags. Most of the remaining error is in
  textured regions (beard, hair) moved by a pixel or two.
- The synthetic stand-ins swing their heads further than a person on a call
  (±40 px); there the plate's smooth fill shows as a soft patch beside the
  head. The real clips do not show it.
- Before/after sheets of every run (three stills each, mouths at 2x) are saved
  next to the reports when the harness is run from the scratch folder; the
  browser test runs the stand-in for 75 frames and checks the bitrate, the
  codec error, the gain over a frozen frame and the face SSIM.

## Parity and tests

The Go package is the reference. `TestParityFixtureIsCurrent` builds
`testdata/face-stream-parity.json` from three synthetic streams (75 frames at
720p with a requested key frame and two dropped frames; 40 frames at 640 x 480,
24 fps, precision 48, key interval 12, a 6 s time jump, decoding from frame 3
so the first deltas are refused; 40 frames of a face holding perfectly still,
whose deltas are 5 bytes and exercise the trimming cap), two reference
pictures (with and without a mask) and two mouth atlases (four 64 x 48 tiles
in JPEG; one 32 x 20 tile in WebP): inputs, packets (hex), and every decoded
frame, picture, atlas or error. Each port
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
| Go fuzz | `go test -run X -fuzz FuzzDecode$ -fuzztime 30s ./rtc/facestream/`, `FuzzDecodeReference`, `FuzzDecodeAtlas` | 235k and 4.8M runs (frames, atlases), no failure |
| Browser | `pytest app/tests/test_chat_call_facestream.py` | 14 pass; 155 frame packets, 2 pictures and 2 atlases identical |
| iPhone (Linux) | `FM_ONLY=FMChat swift test --filter FaceStream` (from `native/ios/ForkMeshCore`) | 15 pass; same |
| iPhone app | `xcodebuild -scheme ForkMesh` for the simulator (Metal shaders included) | builds |
| Android | `./gradlew --offline :core:coreTest -Pfilter=FaceStream` (from `native/android`) | 5 pass; same |

The decoders bound everything they read: frame packets at 1024 bytes,
reference packets at 256 KB, atlas packets at 64 KB and 8 tiles, sizes and
pose values against their ranges, and the range decoder is refused if it reads
more than 8 bytes past the end. The
Go fuzz targets check that no input panics or yields a non-finite point; the
Swift, Kotlin and JavaScript tests feed a few hundred random packets each.

## Not done yet

- Wiring into the call: publishing on FM Media, the switch at the bitrate
  threshold, the tile swap in `chat-calls.js` and the native call screens.
- An Android renderer.
- A measurement on an iPhone (the Metal renderer and the mouth-tile capture
  are compiled but have not run on a device) and on a recorded call between
  two real clients; the harness now runs on real camera footage, but the
  sender there is the browser pipeline on a stored clip.
- Re-sending the atlas and the reference on a keyframe request for a viewer
  who joins late (`requestAtlas()` exists in the browser sender).
- Better inpainting for large disocclusions: the plate's fill is a smooth
  push-pull, fine for the thin band a moving head uncovers, visibly soft if
  the head moves far.
- HEIC references between iPhones.

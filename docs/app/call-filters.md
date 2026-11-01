# Face, background and voice filters in calls

Part of [the App docs](README.md). The on-device filters a caller can put on
their picture and voice: a stand-in face (head swap), backgrounds, outfits,
voices, an avatar as the face and hand tracking, on the web and in the phone
apps, plus the Face Changer pages that preview them outside a call. Calls
themselves are in [chat-direct-calls.md](chat-direct-calls.md) (one-to-one)
and [chat-group-calls.md](chat-group-calls.md); the low-bandwidth face stream
that animates a still is in [face-stream.md](face-stream.md).

Everything runs on the device before encryption. Models and images are
bundled; nothing is fetched from a third party at runtime.

## Modules (`app/public/`)

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

## Head swap

The chosen face replaces the outgoing video track with a canvas track
(`captureStream`, 30 fps) through `replaceTrack`. Per frame: landmarks (Apple
Vision on iOS, the face mesh elsewhere) smoothed by the landmark smoother
(below); a person matte refined with a guided filter; a hair matte around the
head; a 2.5D warp of the whole stand-in head with a depth buffer so near
surfaces win when turning; removal of the caller's head and hair outside the
stand-in, filled from an estimate of the room; per-frame skin and hair colour
matching; eyelid, gaze and tongue transfer. Stand-in assets
(`app/public/assets/call-faces/<id>.jpg` and `<id>-matte.png`) are built by
`app/tools/build_call_face_assets.py`.

**Frame timing.** The stage runs from `requestVideoFrameCallback` on its
hidden camera `<video>`, so each camera frame is processed as soon as it is
decoded, and captures at 30 fps. A fixed timer would make frames wait (up to
42 ms at 24 fps) and judder against the camera. A 30 fps timer remains as a
fallback that only processes a frame when no camera frame arrived for three
frame times (no callback support, or a throttled tab), so a camera slowed to
15 fps in the dark is not processed twice.

**Landmark smoothing.** Per-point filters in image pixels made the head trail
by about 100 ms and bent the lip shape mid-word, since each lip point sped up
at its own rate. `createLandmarkSmoother` (`chat-call-face-geometry.js`)
splits the head from its expression. The two eye centres go through one
`GroupFilter` (`SMOOTH_HEAD`: 3 Hz, beta 5, speed filtered at 4 Hz), and that
steady frame carries the head. Every point is also expressed in the raw eye
frame (in eye spans) and filtered per group (`SMOOTH_GROUPS`: jaw, brows,
nose, eyes, lips). A `GroupFilter` applies one smoothing factor to all its
coordinates, driven by the group's fastest point, so the lips (2.5 Hz, beta
6, speed at 4 Hz) move as one shape and follow speech within about a frame,
while a still face stays steady. On a synthetic talking head
(`app/tests/test_call_face_smoothing.py`) lip error while talking is 0.67 px
(was 1.55) and head-follow error 1.9 px (was 5.7), with rest jitter about a
third of the input noise.

**Face depth and pose.** A region point may be `[x, y, z]` with `z` in the
image's pixel units, larger = nearer. The face mesh supplies it from its
per-vertex depth output (negated and scaled to frame pixels in
`chat-call-face-mesh.js`); on iOS the TrueDepth camera supplies measured depth
(see [iOS TrueDepth camera](#ios-truedepth-camera-face-depth)). `landmarksFromRegions`, the point smoother
and `closeLids` carry `z` through. When every pose landmark has a `z` and the
depths spread by more than 4% of the eye span, `solvePose` fits the head model
in 3D (Horn's quaternion method, then a 2D least-squares scale), so yaw and
pitch do not depend on the nose-direction flip heuristic; flat or missing
depth falls back to the 2D affine fit.

**Rigid nose.** Raw nose landmarks jitter and disagree with the stand-in's
nose, and warping between them bends it. `liveHead` places the nose (points
27-35) by projecting the stand-in's own nose through the solved pose, shifted
by the nose group's mean offset from the live points, keeping 15% of the live
positions (`NOSE_RIGID = 0.85`). Neighbouring cheek triangles absorb the
difference.

**Turning the head.** On a turned head the far jaw line the trackers report is
only the cheek's silhouette, so pinning the stand-in to every live landmark
stretches the head across the near cheek. `liveHead` places the head as one
rigid 3D object through the solved pose and adds only a smooth residual (a
Gaussian field over brows, nose and eye corners) for the caller's proportions:

- Jaw points follow the live jaw only as far as they face the camera
  (`jawTrust`: the pose's view direction against an outward jaw normal,
  `JAW_FACING` 0.08-0.38, at most `JAW_FOLLOW` 0.7). The pose is solved twice,
  the second time with jaw weights scaled by that trust, so a sliding
  silhouette cannot bend yaw or scale.
- 15 cheek and chin vertices (`CHEEK_COUNT`, after the side ring) sit between
  the jaw and the nose, eyes and mouth with a forward bulge in depth, so the
  far cheek foreshortens and the near cheek widens like a curved surface. The
  lip corners and lower lip also feed their field, so cheeks follow a smile
  and the chin follows a dropping lower lip.
- The forehead row curves back toward the temples (brow depth x1.5 - 0.12).
- The hair ring is a silhouette, not a surface: `projectOutline` keeps its
  shape and moves it only with the head centre (`HEAD_CENTER`, 1.1 eye spans
  behind the eyes) and roll, as a round head's outline behaves (a ring rotated
  as a flat curve swings behind the face and takes the hair with it).
- A side ring (one point per hair-ring point, `SIDE_REACH` 0.4 of the way out
  from the nearest jaw or forehead point, `SIDE_BACK` 0.35 behind it) carries
  the photo's face edge: on the far side it folds away behind the cheek and
  only hair fills the outline; on the near side it opens out.
- Triangles that fold on screen (opposite winding to the photo) are drawn
  collapsed, except around the lips, so a turned-away surface never smears
  past the silhouette. Zero-area photo triangles are dropped.

**Mouth opening.** A plain inner-lip polygon with a hard edge looks like a
box. `buildHeadMesh` adds mouth vertices after the cheeks
(`MOUTH_EXTRA_COUNT`): each lip is a Catmull-Rom curve through its inner-lip
points (corners kept sharp, 4 samples per segment), plus an inner ring 14% of
the way to the other lip (`MOUTH_FEATHER`) and a deep ring 40% of the way
(`MOUTH_DEEP`). Every extra vertex is a fixed linear combination of the 68
landmarks, so photo layout, 3D model and live points stay consistent.
Triangles carry a per-corner `hole` value (0 on the lip line, 1 on the inner
ring, 2 on the deep ring, at most 0.5 at the corners); the warp shader turns
0-1 into a soft edge with `smoothstep` and writes the hole and the depth past
it into the aux target's blue channel (half each). The composite darkens the
feather band as a lip shadow and shades the opening from 60% at the lips
(`MOUTH_EDGE`) to full brightness at the deep ring, so teeth and tongue read
as inside a mouth.

When the tracker puts the inner lip's quarter points almost at the corners (as
Vision does on a wide-open mouth) the lip triangles fold. `regularMouth`
rebuilds the 20 lip points in the mouth's own frame: each keeps the posed
stand-in's position along the mouth and takes only its height from the live
lip curves; the inner lips keep at least 65% of the stand-in's lip thickness
(`LIP_KEEP`); the opening tapers into the corners (`CORNER_TAPER`). The
fallback tracker's template fit drops the lower lip with a centre-weighted
profile.

**Blinks.** `createBlinkTracker` (`chat-call-face-expression.js`) avoids false
blinks: the image edge baseline moves with a time constant (0.8 s up, 6 s
down) and is frozen while an eye is closing; when landmarks follow eyelids
(face mesh, Vision, pushed frames) a blink also needs the lid gap below 90% of
its open baseline (full weight at 60%); closure starts above 0.4 and ends
below 0.18; the two eyes are pulled 75% toward the more open one (real blinks
are bilateral, one-eye drops are usually glare); nothing closes while the head
moves faster than 2.2 eye spans per second.

**Blending.** The stand-in photo is sampled with a small mip bias (0.35) so
it is about as soft as a webcam image, with a faint per-frame grain (±0.6%)
so it does not look cleaner than the video around it.

**Skin tone.** Applying the full ratio of the caller's skin colour to the
stand-in's blows out highlights or turns a fair stand-in grey, because most of
the remaining difference after auto exposure is complexion. So the overall
difference is split from the local lighting pattern: the pattern
(`local / global`, 0.6-1.6) follows the caller's light, but only part of the
overall gap is applied (level^`SKIN_LEVEL` 0.4, hue^`SKIN_HUE` 0.6, clamped to
`SKIN_RANGE` 0.55-1.8), and a soft shoulder above `HIGHLIGHT_KNEE` (0.82)
replaces a hard clip at white. On the test composites a Kofi stand-in on a
pale caller comes out RGB 131/107/98 (caller's skin 166/147/141, Kofi's photo
111/83/70).

**Halo.** With no background chosen, the blurred backdrop carries the
background-visibility weight in its alpha, and the halo only blurs where that
weight says the caller's body was (`HALO_CLEAR` 0.45-0.92); the real
background around it stays sharp. (An inpainted plate in a large ellipse left
a ring of smudges around every head.)

**Pushed frames.** `stage.pushFrame(image, reply, time)` renders a frame
whose landmark reply was computed elsewhere (`{regions, mask}` in the image's
pixels), used by the iOS TrueDepth camera.

**Privacy rule:** when tracking fails the frame is blurred until a face is
found again; raw video is never shown in place of a chosen face.

### Native face math

Phone calls are moving from the WebView to a native media engine
([../server/rtc.md](../server/rtc.md), "Phone media engine"). Backgrounds and
the voice are native. For faces, everything that is plain maths or data is
ported line for line, with no new behaviour, so a native renderer only has to
draw. The iOS renderer (Metal) is described in "Native face
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
loading photos and mattes. iOS has all of this, including the hair network
and the guided filter (see below); Android has none of it yet.

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

### Native face renderer (iOS)

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
   blurs. The keep mask stops the stand-in's hair outside the caller's own
   outline being cut away. The person segmentation runs at most once per frame (or
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

**Beyond the browser.** The iPhone does everything the browser does to the
matte and the head zone, and more on phones with a TrueDepth front camera:

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
inside `CallFaceWarp.hairRegion` (part of `CallFaceDrawPlan`); the network
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

**Blur backgrounds** are drawn in the composite whenever the face pass
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

## Backgrounds

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
1.05x width), as phones and turned tablets send video; covering a 16:9 image
into a 9:16 frame would keep only its middle third. The renders come from the same scenes with a portrait camera
(`render_call_backgrounds.cjs --portrait`): horizontal field of view 0.8x the
landscape's vertical one, aimed per scene where needed (the office turns left
to keep the sign in view, the World turns 16 degrees instead of 24). 720x1280
is enough because the stage never draws more than 960 pixels on a side, and
keeps all fifteen files under the 1.5 MB budget (portrait adds about 400 KB;
the native call pages, which inline them, grow by about 570 KB). When
the camera turns mid-call the stage loads the other image and keeps the old
one on screen until it arrives. "Your photo" is covered into either shape.

## Outfits

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

## Models

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

## Voices

The microphone track goes through an `AudioWorklet` after the browser's echo
cancellation, and the processed track replaces the audio sender's track. If
the worklet cannot start, the plain microphone is sent. The engine is a port
of the desktop voice changer: high-pass and light denoise, an STFT formant
(envelope) warp, PSOLA pitch shifting anchored on glottal pulses, a McLeod
pitch tracker with hysteresis and an octave guard, and a look-ahead limiter.
Added latency is 1728 samples (36 ms at 48 kHz).

How the engine keeps a changed voice natural (judged by spectrograms of piper
speech and by inter-harmonic noise on a steady glottal vowel, where the dry
signal sits near -54 dB re total):

- **Sub-sample pulse marks and grain placement.** Whole-sample marks and
  rounded grain positions give up to half a sample of timing error per pulse,
  which smears the partials into hoarse noise (-20 to -30 dB between the
  harmonics) even at a pitch ratio near 1. Marks are refined to a fraction of
  a sample with a parabola on a full-rate correlation, and each grain is read
  with 4-point cubic interpolation and a fractional Hann window; a steady
  vowel stays at -40 to -64 dB.
- **Peak envelope.** A cepstral mean of the log spectrum follows the valleys
  between harmonics, which move with the frame's position against the pulses,
  so the warp gain would change frame to frame (-31 to -37 dB of noise). The
  log spectrum is peak-held over half a pitch period before the cepstral
  lifter, and the envelope glides with a 6 ms time constant, so it runs
  through the harmonic peaks and stays still. Updating it only every other
  frame aliases the pitch-rate wobble into sidebands; do not.
- **Warp only where the vocal tract decides.** The tract scale applies in full
  up to 5 kHz, then eases back to no warp by 9 kHz. Unvoiced frames (s, sh, f)
  get 40% of the warp, following a voicing value that glides over about
  10 ms, so fricatives move 4-9% where the warp is 20-25%: a lowered voice does
  not turn "s" into "sh", and the top band does not empty out. Unvoiced sound
  keeps its own pitch.
- **Breath after the pitch shift.** Breath added inside the STFT before PSOLA
  is repeated by the grains and comes out comb-filtered and inaudible. The
  engine synthesises noise shaped by the warped envelope and tilt (rising
  from 800 Hz to full at 3.8 kHz) on a second OLA path at half the frame rate,
  delays it by the shifter latency, modulates it once per output pulse (peak a
  quarter period before each pulse, 70% depth), and adds it after PSOLA. At
  breath 0.1 the noise between harmonics in 2-5 kHz rises about 20 dB, while
  100-1000 Hz stays below -45 dB.
- **Full-period mark correlation.** Each pulse mark is placed by correlating a
  full period on each side of the last mark. Half a period gave male voices
  0.45-0.57 semitone mark jumps per cycle on recorded speech (CMU ARCTIC), so
  the output carried 1.6-3.4% cycle-to-cycle jitter and sounded hoarse; the
  full period cuts output jitter about 30% (Maya from a male voice 1.14%,
  Theo 1.72%, Kofi 2.56%). Smoothing the period across neighbouring marks was
  rejected: an exponential glide lags pitch glides, and a centred average
  needs more latency.

CPU per 128-sample quantum in node is about 300 µs (11% of real time) without
breath and 320 µs (12%) with breath; the extra cost is the second inverse FFT,
every other frame. Female-to-deep voices are the noisiest, at about -42 dB:
time aliasing of the STFT warp, which a shorter lifter would fix only by
blurring formant positions.

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

### Native voice processor

Phone calls are moving from the WebView to a native media engine
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

## Your avatar as the face

The face picker in a direct call offers "Your avatar" (id `avatar`) after
"Your own face". It uses the account's profile picture
(`userSession().avatarPng`, passed to `createChatCalls` as `avatarImage`) as
the stand-in, through the same path as "Your photo…": `stageFaceFor` hands the
stage the `custom` face with the avatar image, and the picker shows "Your
avatar" while the stage reports `custom`. The option is hidden without a
profile picture, and an avatar with no face the mesh can find says so and
falls back to your own face, like an unusable photo. `createFmmLocal` reads
the same saved choice, so group calls and the World's "send video" use it too
(`avatarImage` and `faceChoice` options). `faceImageFor`, `photoFaceId` and
`stageFaceFor` in `chat-call-face-custom.js` are the shared helpers.

## Hand tracking

`chat-call-hand-tracker.js` runs MediaPipe's palm detector and 21-point hand
landmark model (`app/public/assets/hand-tracking/`, Apache 2.0) on the same
WebGL runner and names gestures. The World uses it to move avatar arms and
send reactions ([../world/voice.md](../world/voice.md), "Hands and
reactions"). The converter maps `FULLY_CONNECTED` on a 1 x 1 input to a
1 x 1 convolution and knows fused `RELU6`, and the runner has `relu6`.

## Preview and reuse outside a call

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

### Plug-ins

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
phone apps do not load this page (next section); the `fmShare` and
`FmShareBridge` paths only matter to older app builds.

### Face Changer on the phones

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
  Android WebView that is not attached does not render or play; the screen
  would spin until its watchdog fired. The iOS screen shows the factory's
  previewing peer as soon as it exists, and Android sets `previewing` before
  running the page's `preview`, so the WebView is attached from the start.
- **The camera video must be in the document.** WebKit (every iPhone browser,
  `WKWebView`, Safari) does not play a `MediaStream` in a detached,
  `display:none` or off-screen video, so the stage would never get a frame.
  The stage adds its camera `<video>` to the page as a 1 px, transparent,
  fixed element (`CAMERA_STYLE`), calls `play()` again if WebKit pauses it,
  and removes it on `stop()`.
- **The recorder asks for an H.264 level that fits the picture.** The camera
  opens at 1280x720 and the face stage draws up to 960x540, above Baseline
  level 3.0 (at most 1,620 macroblocks, about 720x576), which makes iPhone
  recordings fail or come back empty. The page's `record` and the web test
  clip ask first for `video/mp4;codecs=avc1.42E028` (level 4.0, up to
  1920x1080 at 30 fps), then plain `avc1` and `video/mp4`. The view-once
  recorder keeps level 3.0 (`avc1.42E01E`) because it opens the camera at
  640 px. An empty recording (stop straight after start) says "Nothing was
  recorded".
- **The preview does not wait on the voice.** `addModule` has no time limit
  of its own, so `preview` waits at most `VOICE_READY_MS` (2.5 s) for the
  voice worklet; the voice carries on starting and reports itself with the
  `voice` event. `createVoiceStage` gives `addModule` 6 s
  (`MODULE_TIMEOUT_MS`).
- **Calls.** `DirectCallEngine` takes the plug-in's look instead of keeping its
  own copy and listens to it: a change made in the plug-in, or from the call
  screen's Face, Background and Voice controls (the plug-in's own controls),
  reaches the live video peer at once, and a new call starts with the current
  look. The plug-in pushes changes to its preview peer the same way, so neither
  screen sends anything to the page by hand.
- **No eye button.** The conversation header has only Voice call and Video
  call; the camera preview is the plug-in screen. The conversation's own
  view-once camera sends view-once video.

The face page also has the Outfit picker (thumbnails drawn by
`callOutfitThumbnail`, one per tick after the page loads so it never blocks),
and its stage takes the picture's own shape: the `<video>`'s `resize` event
sets the box's `aspect-ratio` to the stream's, and a portrait stream gets a
centred box up to 78% of the window high instead of a pillarboxed strip in a
16:9 box.

## iOS TrueDepth camera (face depth)

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

## Measuring

`app/tools/call_video_harness.cjs` drives the real modules in headless
Chromium (`render`, `native`, `models`; `PLAYWRIGHT_DIR` names a Playwright
install, `--gpu` uses the machine GPU). `app/tools/call_video_metrics.py`
reports landmark jitter and output stability; it needs only Pillow. Test footage is kept out of the
repository.

## Tests

`app/tests/test_chat_call_faces.py`, `app/tests/test_chat_call_voice.py`,
`app/tests/test_chat_call_voice_realism.py`,
`app/tests/test_call_face_native_parity.py` (with the iOS
`CallFaceParityTests` and Android `CallFaceParityTest`).

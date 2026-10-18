# Eye Control package

`marketplace/packages/eye-control` (Market id `eye-control`) is an
experimental iPhone section: a cursor follows where the user looks, and a
firm blink taps whatever is under it. It is off by default, synced with the
account, shown on phones only (iOS; there is no Android version yet), and
opens from **You → Personal → Eye Control**, the Market's **Open** button, or
`forkmesh://dashboard/eye-control`.

## Pieces

| Piece | File | Notes |
|---|---|---|
| Gaze, blink, smoothing and calibration maths | `native/ios/ForkMeshCore/Sources/FMTools/EyeControlModels.swift` | Pure Foundation, tested on Linux (`FMToolsTests/EyeControlTests.swift`). |
| Snapping and live calibration maths | `native/ios/ForkMeshCore/Sources/FMTools/EyeControlTargets.swift` | Pure Foundation, same tests. |
| Camera session, cursor window | `native/ios/ForkMesh/Platform/EyeTracker.swift` | `EyeTracker.shared`, ARKit. |
| Turning a blink into a tap | `native/ios/ForkMesh/Platform/EyeClicker.swift` | Public UIKit and accessibility API only. |
| Screen and calibration view | `native/ios/ForkMesh/Features/Tools/EyeControlScreen.swift` | Route `eyeControl`. |

## Tracking

- `ARFaceTrackingConfiguration` runs the TrueDepth camera (colour camera,
  infrared camera and dot projector). ARKit supports it on Face ID devices
  and, without depth, on A12 and later; `ARFaceTrackingConfiguration.isSupported`
  decides, and the Start button is disabled otherwise.
- Each frame, the face anchor is moved into camera space with
  `ARCamera.viewMatrix(for:)` for the current interface orientation, so x is
  screen right and y screen up whatever way the phone is held. ARKit's
  front-camera space is mirrored the way its selfie preview is, which is why
  camera x maps to screen x without a flip.
- `EyeGaze.screenPoint` casts a ray from each eye along its own forward (+z)
  axis, intersects each with the screen plane (camera-space z = 0) and
  averages the two hits, so the eyes' convergence on a near screen is kept
  instead of being averaged away. Both eyes must hit. The hit, in metres, becomes
  points with a nominal density (155 pt/inch on iPhone, 132 on iPad) measured
  from the camera's edge of the screen (top in portrait). The estimate is
  rough on purpose; calibration corrects scale, offset and any skew.
- `EyeOneEuro` (the 1€ filter) smooths each axis: heavy smoothing when the
  eyes are still, less when they move fast. **Steady**, **Balanced** and
  **Quick** set its minimum cutoff (0.3, 0.7, 1.5 Hz) and speed factor.

## Clicking

- `EyeBlinkDetector` reads ARKit's `eyeBlinkLeft` / `eyeBlinkRight` blend
  shapes. Either eye past 0.3 starts a blink and freezes the cursor (the eyes
  roll as the lids close, which would drag it). Both eyes past 0.55 count as
  shut. When both are back under 0.3, the blink clicks if both were shut for
  at least the chosen length and at most 1.5 s, with 0.4 s between clicks.
  Winks and half-closes never click.
- Lengths: **Any blink** 0.05 s, **Firm blink** 0.25 s (default), **Long
  blink** 0.5 s. A natural blink keeps the lids shut for roughly 0.1 to 0.15 s,
  so the default ignores them; **Any blink** is there for users who cannot
  hold their eyes shut.
- `EyeCursorEngine` clicks where the cursor was 0.12 s before the blink began,
  from a one-second history, again because the gaze drops as the lids close.
  If the cursor was snapped to a target then, the click goes to that target
  (see Snapping).
- `EyeClicker` finds what to press in the app's own window (never the cursor
  window), in order:
  1. a text field or text view under the point becomes first responder;
  2. a `UIControl` under the point is pressed like a finger would: a switch
     flips and sends `valueChanged`, a segmented control picks the segment
     under the cursor, a slider jumps to it, anything else gets
     `performPrimaryAction()` (iOS 17.4, else `primaryActionTriggered`) and
     `touchUpInside`;
  3. otherwise the accessibility tree is walked (at most 4,000 nodes) for
     elements whose `accessibilityFrame` holds the point; interactive ones
     (button, link, adjustable, tab, key, search field) come first, smallest
     first, and the first whose `accessibilityActivate()` succeeds wins. This
     is how SwiftUI buttons and navigation links are reached;
  4. otherwise the collection or table cell under the point is selected
     through its delegate.
- Synthesising real touches needs private API, which App Review rejects, so
  swipes, scrolling and long presses are not supported.

## Snapping

**Snap to buttons** (on by default) makes the cursor lock onto the control
the user is looking at, outline it, and press that control on a blink even
when the gaze lands a little beside it.

- `EyeClicker.targets()` lists what can be pressed, four times a second: it
  walks the app window (views, then accessibility elements, the same budget
  as clicking) and keeps enabled `UIControl`s, text fields and editable text
  views, table and collection cells (and the buttons inside them), and
  accessibility elements with the button, link, adjustable, keyboard key,
  search field or toggle traits. Each frame is clipped to the window; targets
  under 6 pt or over half the window are dropped, as is anything another view
  covers (a hit test at its centre must land inside the view that owns it, so
  a screen behind a sheet does not count). The overlay's **Stop eye control**
  button is added as a target too.
- `EyeCursorEngine` keeps an uncertainty, sigma, for the cursor: the RMS
  spread of the last 0.3 s of mapped gaze (halved, because the filter takes
  out much of it) combined with the calibration's estimated error (60 pt when
  uncalibrated), held between 14 and 90 pt.
- `EyeSnapper` scores each target within 2.5 sigma (at most 110 pt) as
  distance / sigma + 0.35 · ln(1 + area / 4 sigma²), so a target the gaze is
  inside wins over one beside it, and a small button wins over the card it
  sits in. It keeps the current target until the gaze is 3.5 sigma away, and
  only switches to a rival that beats it by 0.6 for 0.12 s, so the highlight
  does not flicker between neighbours. Targets are matched across rescans by
  identity, or by a frame that overlaps the old one by 60% or more (SwiftUI
  recreates elements, and scrolling moves them).
- A snapped target is pressed at its centre. Sliders, segmented controls and
  steppers are *precise*: they are pressed where the gaze is, clamped 6 pt
  inside the target, because the point inside them matters.

## Cursor window

- `EyeCursorOverlay` adds an `EyePassthroughWindow` at `UIWindow.Level.alert`
  to the active scene. Its hit test only returns controls, so every touch
  outside the **Stop eye control** button reaches the app underneath.
- The cursor is a 30 pt ring that dims when the face is lost, shrinks and
  fills while the eyes are closing, and a ring pulses where a click lands
  (blue when something was pressed, grey when nothing was there).
- When snapped, the cursor is drawn 60% of the way from the gaze to the
  target's press point and shrinks a little, and a blue rounded outline
  (4 pt outside the target) pops in around the target, slides to the next one
  as the gaze moves, brightens with how close the gaze is, and fills while
  the eyes are closing for a click.
- Blinking on **Stop eye control** stops tracking; the button sits 64 pt
  above the bottom safe area.
- While tracking, the idle timer is off so the screen stays on; stopping
  restores the previous value. iOS pauses the camera in the background, and
  ARKit resumes the session when the app returns.

## Calibration

- `EyeCalibrationRun` shows nine dots (a 3 × 3 grid inset 12% from the
  edges). For each, it waits 0.8 s for the eyes to settle, then collects raw
  gaze for at least 1 s, and up to 2 s until it has 20 samples; a ring fills
  around the dot as it does. Glances away are dropped: samples further than
  2.5 robust standard deviations (1.4826 × the median distance) from the
  median are removed and the rest averaged. With at least five dots seen, the
  dots seed an `EyeLiveCalibration`; otherwise the screen says calibration
  failed.
- `EyeCalibration` maps raw points to the screen with an affine map, plus,
  when it is *curved*, the terms u², uv and v² (u, v = raw / 1000), which
  follow the bend that eye rotation puts into the mapping near the edges. Its
  weighted least-squares fit puts a small ridge (0.05 × total weight) on the
  curved terms so a few samples cannot bend it wildly.
- `EyeLiveCalibration` fits from the dots plus everything learned since
  (below). Each fit is robust: two Huber reweighting passes shrink the weight
  of any sample more than 1.5 × the median residual (at least 4 pt) off. The
  curved model is chosen only with eight samples or more and only when its
  leave-one-out error is under 90% of the affine one; without dots, an affine
  fit must also beat a plain offset on leave-one-out error. The winning
  leave-one-out error is the accuracy the screen shows and the calibration
  part of the cursor's sigma.

### Live calibration

**Keep learning** (on by default) keeps calibrating while Eye Control is in
use, so the map follows the user when they move the phone or shift how they
sit:

- A blink-click on a snapped target teaches that the user was looking at its
  centre: the median raw gaze of the 0.4 s before the click on that target
  (at least five frames, holding still) becomes a sample with weight 1.5.
- A fixation (the cursor staying within 40 pt, width plus height, for
  0.25 s) that rests on the same snapped target for 0.9 s teaches the same
  with weight 0.4, once per fixation.
- Only targets up to 140 pt on each side that are not precise teach
  anything, since where the user looked inside a big card or a slider is
  unknown. A sample whose miss under the current map is more than 3 × the
  estimated error (at least 100 pt; 240 pt while uncalibrated) is refused.
- The newest 48 learned samples are kept; each counts half as much for every
  24 newer ones. After every accepted sample the map is refitted, and then a
  drift offset is added: the weighted mean miss of the latest eight samples
  under the refitted map (each half as much per three newer), shrunk toward
  zero. That is what makes a sudden shift (the phone held lower) show within
  a few taps, while the global fit absorbs it over time.
- Without a dot calibration, learning still works: first as an offset, then
  as an affine or curved map once there are enough well-spread samples.
- The dots, learned samples and current map are stored together as JSON
  under `eyeControl.calibration` in the device's key-value store, after each
  accepted sample; the older bare affine map still loads, as the prior.
  **Reset** removes everything. The map belongs to the orientation it was
  made in, which is why the screen suggests calibrating again after a
  rotation.

## Settings

Prefs (device only): `eyeControl.smoothing`, `eyeControl.blinkLength`,
`eyeControl.calibration`, `eyeControl.snapping` and `eyeControl.learning`
(both default on).

## Privacy

Camera frames are processed by ARKit on the device and never stored or sent.
`NSCameraUsageDescription` names Eye Control. The manifest asks for
`device.camera.read` (high) and `app.input.control` (sensitive: it taps
controls inside ForkMesh on the user's behalf). Eye Control cannot act
outside ForkMesh.

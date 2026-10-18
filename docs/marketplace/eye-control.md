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
- `EyeGaze.screenPoint` starts a ray halfway between the two eye positions,
  points it along the average of the eyes' forward (+z) axes, and intersects
  it with the screen plane (camera-space z = 0). The hit, in metres, becomes
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

## Cursor window

- `EyeCursorOverlay` adds an `EyePassthroughWindow` at `UIWindow.Level.alert`
  to the active scene. Its hit test only returns controls, so every touch
  outside the **Stop eye control** button reaches the app underneath.
- The cursor is a 30 pt ring that dims when the face is lost, shrinks and
  fills while the eyes are closing, and a ring pulses where a click lands
  (blue when something was pressed, grey when nothing was there).
- Blinking on **Stop eye control** stops tracking; the button sits 64 pt
  above the bottom safe area.
- While tracking, the idle timer is off so the screen stays on; stopping
  restores the previous value. iOS pauses the camera in the background, and
  ARKit resumes the session when the app returns.

## Calibration

- `EyeCalibrationRun` shows nine dots (a 3 × 3 grid inset 12% from the
  edges). For each, it waits 0.8 s for the eyes to settle, then takes the
  median of 1 s of raw gaze points. With at least five dots seen,
  `EyeCalibration.fit` solves a least-squares affine map from raw points to
  dot positions; otherwise the screen says calibration failed.
- The map is stored as JSON under `eyeControl.calibration` in the device's
  key-value store; **Reset** removes it. It belongs to the orientation it was
  made in, which is why the screen suggests calibrating again after a
  rotation.

## Settings

Prefs (device only): `eyeControl.smoothing`, `eyeControl.blinkLength`,
`eyeControl.calibration`.

## Privacy

Camera frames are processed by ARKit on the device and never stored or sent.
`NSCameraUsageDescription` names Eye Control. The manifest asks for
`device.camera.read` (high) and `app.input.control` (sensitive: it taps
controls inside ForkMesh on the user's behalf). Eye Control cannot act
outside ForkMesh.

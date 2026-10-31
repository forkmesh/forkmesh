# Face Changer on the desktop

Added 2026-10-25. The desktop's Face Changer used to be a rail button that
opened `/dashboard/face-changer` in the system browser. That page can only
show its picture in a browser tab or a pop-out window; a meeting in Google
Meet, Zoom or Teams cannot pick it as a camera. The desktop now has a native
rail page whose output is a camera every app lists: on Linux, choose
**ForkMesh Camera** in Google Meet's camera menu (or any other call app) and
the meeting gets the changed picture.

Code: `marketplace/packages/face-changer/content/desktop/`. Package copy names
no other apps (`test_copy_names_no_other_apps`), so the UI says "any call or
meeting app"; this page may name them.

## Pieces

- **`FaceChangerFeature`** (the rail page, no `Q_OBJECT`). Start camera,
  a camera picker, Face, Outfit and Background pickers (with "Photo…" for a
  face or background from a file), the "Use it in a call or meeting" card, and
  "Open in browser" for the web page (clips and snapshots stay there). The
  chosen camera is kept in `QSettings` under
  `market/packages/face-changer/data/face.camera`; the look is kept by the
  engine in the page's `localStorage` under the call keys, like the web page,
  in a persistent profile at `data/face-changer/web`.
- **`FaceStudioPage.cpp`** (the engine page). A small HTML page loaded with
  `setHtml` and the active host as its base URL (`NativePackageHost::webPageUrl`,
  added for this; `publicPageUrl` maps app.forkmesh.com to forkmesh.com, which
  does not serve the engine). It imports `/chat-call-faces.js`,
  `/call-studio.js`, `/chat-call-backgrounds.js`, `/chat-call-outfits.js` and
  `/chat-call-face-custom.js` from that host, so the desktop runs the same
  face, outfit and background engine as calls and the web page, with no copy
  of it. It reports the catalogs to C++ (`fmface:` console messages) and
  exposes `fmStart`, `fmStop`, `fmTake`, `fmFace`, `fmBackground`, `fmOutfit`,
  `fmFacePhoto`, `fmBackgroundPhoto` and `fmLook`.
- **`FaceCamera`** (the virtual camera). Finds v4l2loopback devices in
  `/sys/class/video4linux` (they have a `max_openers` or `buffers`
  attribute), prefers the one labelled ForkMesh, sets it to 1280x720 YUYV at
  30 fps with `VIDIOC_S_FMT` on the output queue and `write()`s frames.
  `yuyvFrame` letterboxes the picture into the frame and converts it with
  BT.601 limited-range maths.

## Why it is built this way

- **The engine stays in JavaScript.** The face tracker, the segmentation
  network and the warp renderer are about 9,000 lines of JavaScript and WebGL
  in `app/public`. The phone apps already host that engine in a web view
  behind a native screen (`docs/app/chat-direct-calls.md`, Face Changer on the
  phones); the desktop does the same with Qt WebEngine, which every desktop
  build that has a browser pane already ships. A build without WebEngine
  shows a note and "Open in browser".
- **Video only.** The page asks for the camera alone (`audio: false`) and the
  permission handler grants only `MediaVideoCapture` to the page's own host,
  denying everything else. A changed voice for meetings is Voice Changer's
  job: it adds a "ForkMesh Voice" microphone.
- **C++ drives every frame.** The stage runs with `manual: true` and
  `fmTake()` calls `stage.processFrame()` on its own hidden camera `<video>`.
  A QTimer calls `fmTake` every 33 ms through `runJavaScript`, which Chromium
  does not throttle. The stage's own `setInterval` and
  `requestVideoFrameCallback` would slow to once a second when the page is
  hidden (the user went to another rail page or minimised the window) and the
  meeting would freeze.
- **Frames cross as JPEG.** `fmTake(1)` returns the latest picture as a JPEG
  data URL (quality 0.85, up to 960x540, about 80 KB) and starts the next
  one; one call is in flight at a time, so a slow engine sends fewer frames
  rather than queueing them. Decoding, scaling and the YUYV conversion run on
  the Qt thread pool (`QtConcurrent`), one frame at a time. `fmTake(0)`, used
  when there is no ForkMesh Camera, only renders the preview.
- **The page never reads its own output.** With `exclusive_caps=1` the
  loopback device only shows up as a camera while ForkMesh writes to it, and
  the page also skips any camera whose label looks virtual (`forkmesh`,
  `dummy video`, `loopback`, `virtual`) and picks a real one. If the only
  camera is virtual it says so.
- **Setting up the driver needs root.** "Set up ForkMesh Camera" runs
  `pkexec modprobe v4l2loopback devices=1 exclusive_caps=1 card_label="ForkMesh Camera"`
  and the card shows the matching `sudo` command for a terminal. The module
  does not survive a restart, so the button comes back then; ForkMesh writes
  nothing to `/etc`. A missing module says to install `v4l2loopback-dkms`
  (Debian, Ubuntu, Arch) or `akmod-v4l2loopback` (Fedora). Chrome and other
  browsers need `exclusive_caps=1` to list the device. If another app (OBS)
  already writes to the loopback device, `VIDIOC_S_FMT` fails with `EBUSY`
  and the card says so.
- **Windows and macOS.** A camera other apps can pick needs a signed driver
  (DirectShow/Media Foundation filter, CoreMediaIO extension). Until there is
  one, the card says so and suggests sharing the Face Changer window.

## Tests

`python3 -m pytest marketplace/packages/face-changer/tests` checks the
registration, the host hook, the permission handling, the engine page
(`node --check`) and builds `content/desktop/tests/test_face_camera.cpp`
(also the `forkmesh-face-changer-tests` ctest target): colours, letterboxing,
device discovery against a fake sysfs, the setup command and its errors, and
that a plain file is refused as a camera.

Checked by hand on 2026-10-25 with an offscreen harness (Qt 6.8 WebEngine,
Chromium's fake camera, SwiftShader WebGL): the page loaded the engine from
app.forkmesh.com, reported 7 faces, 7 outfits and 9 backgrounds, went live,
and `fmTake` returned 128 frames at 960x540 in 8 s (software WebGL; a real GPU
is faster). Writing to a real v4l2loopback device was not tried on that
machine, which has no module.

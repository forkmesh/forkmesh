# Capture and markup in the native composer

The desktop's `capture` package selects a screen region, opens a markup window
and attaches the result to the next prompt (DESKTOP_PARITY 11.15). The phone
apps do the same three steps within what a phone allows.

## Behaviour

- **Capture screen.** The composer's "+" menu and the `/screenshot` command
  snapshot the app's own window and open the markup editor. Neither platform
  lets an app capture another app's screen; for anything else the user takes a
  system screenshot and shares it (iOS share extension `ForkMeshShare`,
  Android `ACTION_SEND` image intent).
- **Mark up any attached photo.** Tapping a composer thumbnail opens the editor;
  "Done" replaces the photo in place, keeping its position and name.
- **Discard is explicit.** Closing the editor with marks on it (Discard,
  Android back, iOS swipe-down) asks first; iOS disables swipe-down.

## iOS: long-press a blank area to copy part of the screen

With the `capture` package on (Market, "Installed on this device"; on by
default, stored as `packages/capture/on`), holding a finger still for 0.6 s on
any blank area of any screen opens a small pop-up at the finger with
**Select Area** and **Whole Screen**. On Home the existing background menu
gets **Select area to copy…** instead, so one hold never opens two menus.

- **Select Area** freezes the screen, dims it, and the user drags a box over
  the part to keep (boxes under 16 pt are ignored). **Whole Screen** skips the
  box. Either way the crop opens the same PencilKit markup editor as the
  composer, with **Copy** in place of **Add**; Copy flattens the ink onto the
  crop (at most 2048 px on the long side) and puts it on the clipboard as an
  image, then shows
  "Copied to clipboard". Discard leaves the clipboard alone.
- Blank means nothing is under the finger: no enabled control, text field,
  list row or cell in the hit view's ancestors, and no accessibility element
  smaller than half the screen at that point within the touched screen's
  view controller. Text, icons and images therefore keep their own long-press
  behaviour; wallpaper, margins and gaps between rows do not.
- A hold on a control with its own hold never opens the pop-up. The New
  agent button's hold fires at 0.35 s and opens voice or the palette, so by
  0.6 s the finger was over that new layer's blank space and the pop-up
  appeared too (fixed 2026-10-18). Now the recogniser notes the top presented
  view at touch-down and refuses to begin if it changed during the hold, and
  every custom hold (New agent, the top-bar project icon, the suggestion
  chip, the Genie, the listening chip, explorer rows, held names, log lines)
  calls `CaptureHold.noteHandled()` so in-place overlays are covered too.
- `ForkMesh/Platform/CaptureHold.swift`: one `UILongPressGestureRecognizer`
  on the app window (installed by `CaptureHoldAnchor` in `ThemedAppView`),
  recognising alongside every other gesture and never cancelling touches. The
  pop-up is a `UIEditMenuInteraction` on the top presented view. The picker,
  editor and notice run in their own window above alerts, so sheets and
  menus underneath are captured as they look. The screen is drawn 350 ms
  after the pop-up closes, as with the composer's capture.
- Turning the package off stops the hold at once; nothing else changes.

## iOS: PencilKit

`ForkMesh/Platform/PromptMarkup.swift`. A `PKCanvasView` sits over the
aspect-fitted photo with the system `PKToolPicker`. The default tool is a red
pen.

- The canvas and picker are forced to the light appearance, because PencilKit
  stores dynamic ink colours and dark mode would invert black strokes in the
  rendered image.
- The flattened image is rendered at the photo's pixel size (capped at
  2048 px), ink scaled by `pixelWidth / canvasWidth`, then goes through
  `PromptPhotoPreparer` (JPEG q80, no metadata).
- `PromptScreenCapture.keyWindow()` draws the key window with
  `drawHierarchy(afterScreenUpdates: true)` 350 ms after the menu closes. The
  keyboard window is not captured.

## Android: Compose canvas

`feature/composer/PhotoMarkup.kt`. Pen, line, arrow, rectangle and ellipse,
eight swatches (red first), undo, Discard and "Add to prompt" (or "Done").

- Strokes are stored normalized to the photo (`C/composer/Markup.kt`), so one
  renderer (`MarkupRender.flatten`) draws the preview and the full-size image.
  Stroke width is 0.6% of the shorter side, at least 3 px.
- The flattened bitmap is scaled to 2048 px and encoded once through
  `PhotoPrep.attachment`, which strips metadata.
- `ScreenCapture.window` uses `PixelCopy` 250 ms after the sheet closes.
  Dialogs, sheets and the keyboard are separate windows. A `FLAG_SECURE`
  window cannot be copied; the composer says "This screen could not be
  captured."

## iOS share extension hand-off

- `tools/generate_project.rb` always adds `ForkMeshShare`
  (`FORKMESH_NATIVE_SHARE=0` leaves it out). Bundle id
  `com.forkmesh.forkmesh.Share`, registered by `tools/asc_bundle_ids.py ensure`.
- The hand-off uses the keychain group `742ZANCB57.com.forkmesh.forkmesh`
  (shared by the app and its extensions), not an App Group.
  `ShareKeychainDrop` (FMSurfaces) writes one item per image plus a manifest
  (`ShareDrop`) under service `com.forkmesh.forkmesh.share`, readable after
  first unlock on this device only. A new share replaces the previous one; the
  app takes and deletes it.
- The extension shrinks each image to 2048 px while loading (JPEG, or PNG with
  transparency) to stay inside its memory limit and under
  `ShareDrop.maxImageBytes` (8 MB) per item.
- When the share includes an image, `ShareModel.load` saves it at once and
  opens `forkmesh://surface/open?route=...` (the compose route) through the
  responder chain's `UIApplication`; text or links alone show the sheet. If the
  app cannot be opened the sheet shows "Saved for ForkMesh" with an
  **Open ForkMesh** button that tries again.
- The automatic send waits for `viewDidAppear` as well as the load
  (2026-10-17). Sending from `viewDidLoad`'s task could run before the sheet
  was in a window, so the responder chain had no `UIApplication`, the app did
  not open and the image had to be shared a second time. The extension now
  closes when `openURL:options:completionHandler:` reports success (or after
  1.5 s with no answer) and falls back to the sheet when it reports failure.
- YouTube video links with no images are sent at once too, with the DJ studio
  route instead of the compose route; the app adds them to the DJ queue
  (`docs/marketplace/video-mix-dj-studio.md`).
- The app picks the share up on every resume and on sign-in
  (`SurfacesCoordinator.pickUpShare`). Signing out deletes an unclaimed share.
- `ForkMeshShare/PrivacyInfo.xcprivacy` declares `CA92.1`; `push_check` in
  `codemagic.yaml` refuses an IPA whose share extension lacks the keychain
  group.

## Tests

- Android `C/composer/MarkupTest.kt` (fit, normalization, stroke width, arrow
  heads, naming): `./gradlew :core:coreTest -Pfilter=Markup`.
- iOS `FMAgentsTests/PromptMarkupTests.swift` (fit, pixel size, ink scale,
  naming; `CaptureAreaTests` for the dragged box, minimum size, pixel crop and
  content size): `FM_ONLY=FMAgents swift test --filter 'PromptMarkupTests|CaptureAreaTests'`.
- Markup happens after the hand-off, never inside the share extension.

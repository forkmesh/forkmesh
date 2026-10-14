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
  app cannot be opened the sheet shows "Saved for ForkMesh".
- The app picks the share up on every resume and on sign-in
  (`SurfacesCoordinator.pickUpShare`). Signing out deletes an unclaimed share.
- `ForkMeshShare/PrivacyInfo.xcprivacy` declares `CA92.1`; `push_check` in
  `codemagic.yaml` refuses an IPA whose share extension lacks the keychain
  group.

## Tests

- Android `C/composer/MarkupTest.kt` (fit, normalization, stroke width, arrow
  heads, naming): `./gradlew :core:coreTest -Pfilter=Markup`.
- iOS `FMAgentsTests/PromptMarkupTests.swift` (fit, pixel size, ink scale,
  naming): `FM_ONLY=FMAgents swift test --filter PromptMarkupTests`.
- Markup happens after the hand-off, never inside the share extension.

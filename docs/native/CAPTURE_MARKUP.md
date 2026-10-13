# Capture and markup in the native composer

DESKTOP_PARITY 11.15. The desktop's `capture` package selects a screen
region, opens a markup window and attaches the result to the next prompt.
The phone apps get the same three steps, adapted to what a phone allows
(added 2026-09-27).

## What the user sees

- **Capture screen.** The composer's "+" menu (iOS "Capture Screen", Android
  "Capture screen") and the `/screenshot` command snapshot the app's own
  window and open the markup editor on it. Neither platform lets an app
  capture another app's screen, so for anything outside ForkMesh the user
  takes a system screenshot and shares it: on iOS the "Send to agent" share
  extension (`ForkMeshShare`) hands the images to the composer, on Android the
  existing `ACTION_SEND` image intent does.
- **Mark up any attached photo.** Tapping a thumbnail in the composer opens
  the editor on that photo; "Done" replaces it in place, keeping its position
  and name. That covers library photos, camera photos, pasted images and
  shared screenshots, so markup is not a separate source in the menu.
- **Discard is explicit.** As on the desktop, where Escape is ignored so an
  accidental key press does not lose annotations, closing the editor with
  marks on it (Discard, Android back, iOS swipe-down) asks first; iOS
  disables the swipe-down.

## iOS: PencilKit

`ForkMesh/Platform/PromptMarkup.swift`. A `PKCanvasView` sits exactly over
the aspect-fitted photo, with the system `PKToolPicker` (pen, marker, pencil,
eraser, lasso, ruler, colors, undo and redo), so the phone gets Apple's
standard markup tools instead of a copy of the desktop's toolbar. The canvas
is handed a red pen, the desktop's default color; the picker's own selection
wins once the user touches it.

The canvas and the picker's colors are forced to the light appearance.
PencilKit stores dynamic ink colors, and in dark mode a black stroke would be
drawn white on screen and in `PKDrawing.image`; forcing light keeps what is
drawn and what is sent the same color. The flattened image is rendered at the
photo's pixel size (capped at 2048 px) with the ink scaled by
`pixelWidth / canvasWidth`, then goes through `PromptPhotoPreparer` like any
picked photo: JPEG q80 and no metadata.

`PromptScreenCapture.keyWindow()` draws the key window with
`drawHierarchy(afterScreenUpdates: true)` 350 ms after the menu closes, so the
menu is gone. The keyboard is in its own window and is not captured; the
composer blurs the field first anyway.

## Android: a Compose canvas

`feature/composer/PhotoMarkup.kt`. The desktop's shape tools, without text and
move: pen, line, arrow, rectangle and ellipse, the desktop's eight swatches
(red first), undo, Discard and "Add to prompt" (or "Done" when editing an
attached photo). Strokes are stored in coordinates normalized to the photo
(`C/composer/Markup.kt`), so the editor draws them over the scaled preview
and `MarkupRender.flatten` draws the same strokes on the full-size image with
one renderer. Stroke width is 0.6% of the shorter side (at least 3 px) of
whatever it is drawn on, so the lines keep their weight in the attachment.
The flattened bitmap is scaled to the 2048 px limit and encoded once at the
composer's JPEG quality through `PhotoPrep.attachment`, which strips metadata.

`ScreenCapture.window` uses `PixelCopy` on the activity window 250 ms after
the bottom sheet closes. Dialogs, sheets and the keyboard are separate windows
and are not in the capture. A window with `FLAG_SECURE` (the vault autofill
activity) cannot be copied; the composer shows "This screen could not be
captured."

## Tests

The geometry is tested on the host:

- Android `C/composer/MarkupTest.kt`: fit, normalization, stroke growth,
  arrow heads, width, naming. `./gradlew :core:coreTest -Pfilter=Markup`.
- iOS `FMAgentsTests/PromptMarkupTests.swift`: fit, pixel size, ink scale,
  naming. `FM_ONLY=FMAgents swift test --filter PromptMarkupTests`.

## Not verified

- The iOS editor has not been built by Xcode or run on a device yet; the
  sources parse on Linux (`tools/check_swift_syntax.sh`).
- The Android editor compiles; it has not been tried on a device.
- Neither platform marks up inside the share extension; the images are marked
  up after the hand-off, from the composer.

## The iOS share extension hand-off (2026-10-01, issue #1177)

The user asked for the share sheet to work: share an image to ForkMesh and
have it open in the prompt.

- Until now `ForkMeshShare` was only in projects generated with
  `FORKMESH_NATIVE_EXTENSIONS=1`, which the ad hoc and release workflows never
  set, so it was not in any installed build. Even there it could not have
  worked: it saved to the App Group `group.com.forkmesh.forkmesh`, which is
  provisioned for no id and named only in the extensions' entitlements, so the
  app could never read it; and `NSExtensionContext.open` always fails in a
  share extension, so the app never opened.
- `tools/generate_project.rb` now always adds `ForkMeshShare`
  (`FORKMESH_NATIVE_SHARE=0` leaves it out); the widgets stay behind
  `FORKMESH_NATIVE_EXTENSIONS` (AutoFill followed on 2026-10-02,
  [IOS_RELEASE.md](IOS_RELEASE.md)). Its bundle id
  `com.forkmesh.forkmesh.Share` is already registered by
  `tools/asc_bundle_ids.py ensure`, so cloud signing at export makes its
  profile.
- The hand-off goes through the keychain group
  `742ZANCB57.com.forkmesh.forkmesh`, the one the app and the notification
  extension already share and every profile allows, instead of the App Group.
  `ForkMeshShare.entitlements` names that group and no App Group.
  `ShareKeychainDrop` (FMSurfaces) writes one item per image and a manifest
  (`ShareDrop`, tested on Linux) under the service
  `com.forkmesh.forkmesh.share`, readable after first unlock on this device
  only. A new share replaces the previous one; the app takes and deletes it.
- The extension shrinks each image to 2048 px as it loads it (JPEG, or PNG
  when it has transparency), which keeps it inside the extension's memory
  limit and each keychain item under `ShareDrop.maxImageBytes` (8 MB). The
  composer re-prepares it with `PromptPhotoPreparer`, which strips metadata.
- After Send the extension opens `forkmesh://surface/open?route=...` (the
  compose route) through the responder chain's `UIApplication`
  (`openURL:options:completionHandler:`), the route that still works from a
  share extension on iOS 18, and falls back to "Open ForkMesh" if there is
  none.
- The app picks the share up on every resume and on sign-in
  (`SurfacesCoordinator.pickUpShare`): the images become the composer's
  attachments and the compose route opens the composer with any shared text
  and links. Signing out of an account deletes an unclaimed share.
- `ForkMeshShare/PrivacyInfo.xcprivacy` declares `CA92.1` for the
  `UserDefaults` FMBase may link in, and `push_check` in `codemagic.yaml`
  refuses an IPA whose share extension lacks the keychain group.
- Not run on a device: Linux parses the Swift and runs the FMSurfaces tests.

## Images skip the share sheet (2026-10-01, issue #1192)

The user asked for shared images to go straight into the prompt without the
extension's "Send to agent" step. When anything shared to `ForkMeshShare`
includes an image, `ShareModel.load` sends it as soon as it has loaded, so
the app opens on the composer with the images attached, as Android's
`ACTION_SEND` intent already did; the prompt is typed there. Text or links
alone still show the sheet. If saving fails, the sheet shows the error; if the
app cannot be opened, it shows "Saved for ForkMesh".

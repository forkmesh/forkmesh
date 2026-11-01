# Login, onboarding and permission prompts

Part of [the native docs](README.md).
The phone apps' first screens: login and sign-up, the onboarding flow, and
the permissions intro that keeps a fresh install free of system prompts until
a feature needs one. The first screens share one look and one set of pieces. iOS keeps
them in `ForkMesh/Kit/FmOnboardingKit.swift`, Android in
`ui/kit/FmOnboardingKit.kt`. Both draw the same colours, sizes and timings, so
a change to one belongs in the other.

## What shows, and when

1. **Welcome, Log in, Sign up** (`AuthFlowView`, `AuthFlow`). One background
   stays in place while the pages change. "Forgot password?" opens the relay's
   `/forgot-password` (`AuthService.forgotPasswordUrl`) in an in-app Safari
   sheet. Sign up opens `/signup` in the same sheet and moves the flow to the
   login form with "When your email is confirmed, log in here with your new
   account." (`AuthFlowModel.signupOpened`), so closing the sheet lands on
   login instead of leaving the app.
2. **Onboarding** (`OnboardingFlowView`, `OnboardingFlow`). On a fresh install,
   after sign-in or guest: the permissions page
   ([below](#the-permissions-intro)), then the starter pack. The
   steps come from what is still pending, so a phone that only owes one sees
   only that one. Back returns a step and keeps the chosen pack. Nothing is
   stored until the flow finishes, so leaving midway shows it again. Skip
   finishes with the default pack.
3. **Tour**: a sheet over Home ([NAVIGATION.md](NAVIGATION.md#2022-the-tour)).

All of these screens are dark whatever the phone's appearance
(`fmForcedDark`, `FmForcedDark`).

## Pieces

- `FmPillButton`: primary is a white capsule with a soft top-to-bottom shade,
  58pt high; `dark` is the secondary. These replace the flat auth buttons on
  these screens only; `FmFilledButton` and the rest of the app are unchanged.
- `FmPillField`: the label stays above the field, 58pt high, radius 20, a thin
  border that brightens on focus.
- `FmAuroraBackground`: the sky gradient and a dotted world map. The map is a
  table of land spans drawn in code, not an image, so it stays sharp at any
  size and adds nothing to the download. It fades out at the top and bottom so
  it never sits behind the header or a title.
- `FmGlassIcon`, `FmAgentOrb`, `FmOnboardingProgress`,
  `FmOnboardingBackButton`.

## Motion

Taken from a reference recording, frame by frame.

- **Press:** the button grows 3% with a soft glow.
- **Leaving a page:** content fades in 160ms; the header stays. The new page
  waits for that fade, so the two never overlap.
- **Progress:** the long pill slides to the next step over 450ms.
- **Arriving:** a fixed order, about 800ms in all. The hero rises and scales
  in, each title line wipes in from the left with a soft edge and a tilt that
  settles (`FmRevealLine`), the subtitle follows the same way, the button
  brightens from grey (`fmBrighten`), and the last text line comes last. Small
  items such as chips and badges appear one after another.
- **Counting:** "Asked up front" runs from the permission count to 0.
- **Idle:** the orb's colours turn and its eyes blink every 2.8s, the hero card
  tilts slowly, and the map dots breathe.

Reduce Motion turns all of this off: every piece appears in its final state.

On a short screen the permissions page drops the card's header, strip and
counter and keeps the permission list, because the full card pushed the title
under the buttons on iPhone Duo's 678 pt cover screen. iOS picks the first
version that fits (`ViewThatFits`), then scrolls; Android goes compact when the
space above the buttons is under 600 dp and still scrolls.

## The permissions intro

A fresh install of the iOS app shows no system permission prompt at launch or
at sign-in. Each permission is asked the first time a feature needs it, and a
short screen says up front what may come up and why.

`PermissionsIntroPage` (`native/ios/ForkMesh/Features/Account/`) is the first
step of `OnboardingFlowView`. It shows once on
a fresh install, after sign-in or "Explore as guest" and before the starter
pack: a hero card that counts "Asked up front" down to 0 and lists one row per
permission with a short `hint` for the moment it is asked, the title "We only
ask when it matters", and the note that saying no keeps the rest of the app
working. VoiceOver reads each row's full `reason`. "Let's Get Started" moves to
the starter pack; Skip and "skip and start" finish onboarding with the default
pack. Finishing sets the intro to seen.

The text lives in `PermissionPacing` (FMAccount) so a core test keeps it to
five short rows. Installs that predate the screen never see it:
`PermissionPacing.settle` marks them `seen` on their first launch with it
([FMAccount.md](ios/FMAccount.md)).

## When each permission is asked (iOS)

| Permission | Asked when | Code |
| --- | --- | --- |
| Notifications | after the first chat message is sent, or "Enable notifications" on the Settings push card; also by the features that already asked on use (a nearby `#mesh` peer, Timers, scheduling a message for later, Glasses Alert) | `PushNotificationService.askWhenUseful` / `enable` |
| Local network | the first time `#mesh` is opened (the mesh's listener, discovery and sweep do not start before it) | `LocalMeshService.waitsForRoom` |
| Bluetooth | the first time `#mesh` is opened, or Glasses Alert is turned on | `CoreBluetoothMeshTransport.promptAllowed`, `GlassesWatcher.start` |
| Camera, microphone, speech | starting a call, recording a message, dictating, Genie hand play | each feature |
| Photos | attaching or saving a picture | each feature |
| Health, motion | turning on Pedometer, Water Intake, Sleep or another Health tool | `HealthFeature` |

Starting `#mesh` at launch would raise the Local Network prompt before the
user has seen anything, so it waits. Calls ring while notifications are not
asked yet: `start` registers for calls only
([FMApi.md](ios/FMApi.md)).

Once `#mesh` has been opened, `permissions/nearby-opened` is stored and the
mesh starts at launch, so nearby messages keep arriving.

## Permissions on Android

Android shows the same page (`PermissionsIntroPage`,
`native/android/app/.../feature/account/`), with its text in `PermissionPacing`
(`core/.../account`) and the same `permissions/intro` and
`permissions/nearby-opened` keys. Older installs skip it and keep the mesh
starting at launch.

| Permission | Asked when | Code |
| --- | --- | --- |
| Notifications (`POST_NOTIFICATIONS`, Android 13 and later) | after the first chat message is sent, or when a nearby `#mesh` peer appears; also by Timers and Glasses Alert on use, and "Allow notifications" on the Pings background card | `PushFeature.askWhenUseful`, `PushFeature.requestNotificationPermission` |
| Nearby devices (`BLUETOOTH_CONNECT`, `BLUETOOTH_SCAN`) | the first time `#mesh` is opened (the mesh does not start before it), or Glasses Alert is turned on | `LocalMeshRoom.waitsForRoom`, `ChatFeature.setMeshOnScreen`, `GlassesWatchService` |
| Local network and Wi-Fi | no prompt: the mesh's sockets need only normal permissions, and they also wait for `#mesh` | `LocalMeshRoom.roomOpened` |
| Camera, microphone | starting a call, recording a message, dictating, scanning a QR code, Face Changer | each feature |
| Photos | the system photo picker, which needs no permission | `PhotoPicker` |
| Physical activity | turning on Pedometer or Sleep | `StepsScreen`, `SleepScreen` |

See [ANDROID.md](ANDROID.md), section 27.

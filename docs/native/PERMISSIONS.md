# Permission prompts on the phone

A fresh install of the iOS app shows no system permission prompt at launch or
at sign-in. Each permission is asked the first time a feature needs it, and a
short screen says up front what may come up and why.

## The intro screen

`PermissionsIntroView` (`native/ios/ForkMesh/Features/Account/`) shows once on
a fresh install, after sign-in or "Explore as guest" and before the starter
pack. It is one scrollable page: a drawn illustration (a shield with
notification, nearby, camera and photo badges, gently floating unless Reduce
Motion is on), the title "We only ask when it matters", one row per
permission with the moment it is asked, a note that saying no keeps the rest
of the app working, and a "Sounds good" button. It requests nothing itself.

The text lives in `PermissionPacing` (FMAccount) so a core test keeps it to
five short rows. Installs that predate this change never see the screen:
`PermissionPacing.settle` marks them `seen` on their first launch with this
build (`native/ios/docs/FMAccount.md`).

## When each permission is asked

| Permission | Asked when | Code |
| --- | --- | --- |
| Notifications | after the first chat message is sent, or "Enable notifications" on the Settings push card; also by the features that already asked on use (a nearby `#mesh` peer, Timers, scheduling a message for later, Glasses Alert) | `PushNotificationService.askWhenUseful` / `enable` |
| Local network | the first time `#mesh` is opened (the mesh's listener, discovery and sweep do not start before it) | `LocalMeshService.waitsForRoom` |
| Bluetooth | the first time `#mesh` is opened, or Glasses Alert is turned on | `CoreBluetoothMeshTransport.promptAllowed`, `GlassesWatcher.start` |
| Camera, microphone, speech | starting a call, recording a message, dictating, Genie hand play | each feature |
| Photos | attaching or saving a picture | each feature |
| Health, motion | turning on Pedometer, Water Intake, Sleep or another Health tool | `HealthFeature` |

Before this change the app asked for notifications at sign-in and started the
`#mesh` room at launch, which brought up the Local Network prompt before the
user had seen anything. Calls still ring while notifications are not asked
yet: `start` registers for calls only (`native/ios/docs/FMApi.md`).

Once `#mesh` has been opened, `permissions/nearby-opened` is stored and the
mesh starts at launch as before, so nearby messages keep arriving.

## Android

Android shows the same screen (`PermissionsIntroScreen`,
`native/android/app/.../feature/account/`), with its text in `PermissionPacing`
(`core/.../account`) and the same `permissions/intro` and
`permissions/nearby-opened` keys. Installs that predate the change skip it and
keep the mesh starting at launch.

| Permission | Asked when | Code |
| --- | --- | --- |
| Notifications (`POST_NOTIFICATIONS`, Android 13 and later) | after the first chat message is sent, or when a nearby `#mesh` peer appears; also by Timers and Glasses Alert on use, and "Allow notifications" on the Pings background card | `PushFeature.askWhenUseful`, `PushFeature.requestNotificationPermission` |
| Nearby devices (`BLUETOOTH_CONNECT`, `BLUETOOTH_SCAN`) | the first time `#mesh` is opened (the mesh does not start before it), or Glasses Alert is turned on | `LocalMeshRoom.waitsForRoom`, `ChatFeature.setMeshOnScreen`, `GlassesWatchService` |
| Local network and Wi-Fi | no prompt: the mesh's sockets need only normal permissions, and they also wait for `#mesh` | `LocalMeshRoom.roomOpened` |
| Camera, microphone | starting a call, recording a message, dictating, scanning a QR code, Face Changer | each feature |
| Photos | the system photo picker, which needs no permission | `PhotoPicker` |
| Physical activity | turning on Pedometer or Sleep | `StepsScreen`, `SleepScreen` |

Before this change Android asked for notifications as soon as a session
existed and started `#mesh` at launch, so the nearby devices prompt came up
before the user had seen anything. See [ANDROID.md](ANDROID.md), section 27.

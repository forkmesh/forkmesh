# Android

The Android app has the iPhone app's features and its look, the same Liquid
Glass theme, and talks to the same endpoints, so the server cannot tell the
two apart. Since 2026-10-28 every phone change ships in both apps in the same
pull request. Back to the [phone apps](README.md).

## Install

1. On the phone, open [forkmesh.com/#downloads](https://forkmesh.com/#downloads)
   and download `forkmesh-android-universal.apk`.
2. Open the file. Android asks once to allow installs from your browser;
   allow it, then **Install**.
3. Open ForkMesh and [sign in](getting-started.md#2-sign-in).

Requires Android 8.0 or later. The APK is signed with ForkMesh's release key;
Android refuses an update signed with any other key.

## Updates

The app is not in a store, so it tells you itself when a newer version is out
and opens the download page (your instance's Android download link, or
`https://forkmesh.com/mobile`). Install the new APK over the old one: the app
id and signing key stay the same, so your sign-in, keys and local repositories
are kept.

## Notifications

Android has no push service in ForkMesh. Instead the app keeps one
**background connection** per signed-in project, shown as a quiet ongoing
notification:

```text
   ForkMesh cores ◄──── one socket per project ──── Android app
                         reconnects with backoff      foreground service,
                         paused by Doze and when      restarted on boot and
                         the network drops            after an update
```

- It carries pings, direct messages and calls. Channel messages do not raise a
  notification on Android.
- With notifications off it still listens for calls only ("Listening for
  calls").
- It holds no wakelock; Doze and network loss pause it, and a 15-minute
  background job revives it if Android stopped it.
- A force-stopped app does not ring until you open it again.

## ↔️ Differences kept on purpose

| On Android | Notes |
| --- | --- |
| Background connection instead of push | The app uses no Google push service; see [Notifications](#notifications) |
| Approving from a notification opens the app | The screen lock needs an on-screen dialog to fall back to |
| Dictation is `/dictate` in the prompt composer, with no mic button | A quiet stop after words sends the prompt |
| Chat and ping times use a 12-hour clock | Agent prompts keep 24-hour stamps |
| Calls run in the app's call page | One-to-one and group calls, with the voice changer, call backgrounds and face changer; Android rings through its own call notification |
| No dictation during a call | A second microphone capture could take the microphone from the call |

iPhone features that need an Apple-only API and so are not on Android: Eye
Control and Spider Room (ARKit), the on-device Gemma model (MLX), CallKit's
system call screen, 3D face tracking in the face changer (TrueDepth), the
Cloud Dragon mascot and the App Store update gate. The full list, with
reasons, is in [docs/native/ANDROID.md](../native/ANDROID.md#11-differences-kept-on-purpose).

## Keys and AutoFill

Your device key and the Password Vault's device keys live in the Android
Keystore. The vault can be chosen as Android's **AutoFill service** in system
settings. More in [privacy and security](privacy-and-security.md).

Related: [getting started](getting-started.md) ·
[Android internals](../native/ANDROID.md) · [phone apps](README.md)

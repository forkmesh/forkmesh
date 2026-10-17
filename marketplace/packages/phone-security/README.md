# Phone Security

Phone Security is a free ForkMesh package for iPhone and Android. It runs a
security checkup on the phone, explains every result in plain language, says
whether the thing it found is in use, and opens the setting that fixes it
when the phone allows that.

Open it from **You → Personal → Phone Security**, from the Market's **Open**
button, or with the `forkmesh://dashboard/phone-security` link. The checkup
runs when the page opens; pull down or tap **Run again** to repeat it.

## What it checks

- **Lock screen:** a passcode or screen lock, Face ID, Touch ID or fingerprint,
  the screen timeout, and whether notification text shows on the lock screen.
- **Device integrity:** jailbreak apps, tools and folders on iPhone; `su`,
  Magisk, KernelSU, APatch, BusyBox, Xposed and LSPosed on Android; Frida and
  other code injected into ForkMesh; a writable system partition, SELinux,
  test-key and debug builds, debuggers and emulators.
- **System:** the iOS version, or Android's version, security patch level,
  storage encryption, developer options, USB and wireless debugging, and
  automatic time.
- **App protection:** where ForkMesh was installed from, file encryption,
  encrypted connections only, an HTTPS relay, secure key hardware and screen
  recording.
- **Network:** HTTP proxies, VPNs, certificate authorities you added and
  private DNS.
- **Apps with access (Android):** accessibility services, apps that read
  notifications, device admin apps and third-party keyboards.
- **Privacy:** what ForkMesh itself can use (camera, microphone, photos,
  speech, Bluetooth or nearby devices, motion, notifications) and which
  features need it.

Results are **Pass**, **Review**, **Warning**, **Critical** or
**Unavailable**, worst first, with filters, a category menu and search.

## Privacy

The checkup runs on the phone. It reads settings, files and ForkMesh's own
process, opens no network connection, uploads nothing and changes nothing. It
has no Worker, no account data and no web or desktop page.

A clean result is not proof that the phone is safe: a phone app sees only
part of the system, and a well-hidden jailbreak or root kit can pass every
check. The desktop app has its own [Security](../computer-security/README.md)
package for computers.

Developer notes: `docs/marketplace/phone-security.md`.

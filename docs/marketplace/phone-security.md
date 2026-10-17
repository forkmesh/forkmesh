# Phone Security package

`marketplace/packages/phone-security` (Market id `phone-security`) is the
phone counterpart of the desktop [Security package](computer-security.md). It
runs a local checkup on iPhone and Android, explains every result, says
whether the thing it found is in use, and opens the right settings page when
the phone allows it. It is off by default, synced with the account, shown on
phones only, and opens from **You → Personal → Phone Security**, the Market's
**Open** button, or `forkmesh://dashboard/phone-security`.

## Why a separate package

The desktop checkup reads `/etc`, `/proc` and `/sys` and fixes things through
`pkexec`. A phone app can read none of that and change no system setting, so
the checks, the evidence and the fixes are all different. Keeping the desktop
package's id (`computer-security`) for computers and giving phones their own
id keeps both Market listings accurate.

## Shape (both platforms)

The same model as the desktop scanner, in each app's testable core:

- A **facts** snapshot holds everything the platform probe read. A field the
  probe could not read is empty (`nil` / `null`), and its check reports
  **Unavailable** rather than guessing.
- A pure **scanner** turns facts (and the current time) into findings. It does
  no I/O, so tests feed it fixtures.
- Every **finding** carries its check (id, title, category, explanation), a
  result, a detail line, an optional **usage** line ("do you need it?") and an
  optional **fix**.
- Results are **Pass**, **Review**, **Warning**, **Critical** or
  **Unavailable**, sorted Critical, Warning, Review, Unavailable, Pass, then by
  title.
- Filters (**All**, **Needs attention**, **Critical**, **Warnings**, **For
  review**, **Can fix**, **Passed**, **Unavailable**), a category filter and a
  search over title, category, detail and usage. Summary and empty-filter
  wording match the desktop: `N checks: P pass, R need review, W warnings, C
  critical, U unavailable`, `Showing M of N checks in <scope> matching "<q>".
  <summary>`, `No checks in <scope> match "<q>". Try a different filter or
  search.`
- A passing or unavailable check never offers a fix.

The probe runs off the main thread when the page opens and again on **Run
again** (or a pull to refresh on iPhone). It reads settings, files and the
app's own process; it starts no process, opens no network connection, uploads
nothing and changes nothing. The one write is the iPhone sandbox test: it
creates a uniquely named file in `/private` and unlinks it at once, which only
succeeds when the sandbox is broken.

## Fixes

Phones do not let an app change system security settings, so a fix opens the
place where the user can:

- **Android:** a specific `Settings` page (`android.settings.SECURITY_SETTINGS`,
  `BIOMETRIC_ENROLL`, `APPLICATION_DEVELOPMENT_SETTINGS`,
  `ACCESSIBILITY_SETTINGS`, `ACTION_NOTIFICATION_LISTENER_SETTINGS`,
  `DATE_SETTINGS`, `DISPLAY_SETTINGS`, `WIFI_SETTINGS`, `WIRELESS_SETTINGS`,
  `INPUT_METHOD_SETTINGS`, `APPLICATION_DETAILS_SETTINGS` for ForkMesh,
  `SYSTEM_UPDATE_SETTINGS`, `NOTIFICATION_SETTINGS`). If the phone has no
  activity for it, the main Settings page opens.
- **iPhone:** ForkMesh's own page in Settings (permissions) or its
  notification settings. iOS has no link to system pages such as Face ID &
  Passcode, so those findings show the steps instead (`Settings → Face ID &
  Passcode → Turn Passcode On`) with no button.

Signs of a jailbreak, root or injected code have no fix: the right answer is a
human look and usually a restore.

## iPhone checks

| Id | Title | Category | Rule |
| --- | --- | --- | --- |
| `ios.passcode` | Device passcode | Lock screen | Not set: critical. |
| `ios.biometrics` | Face ID or Touch ID | Lock screen | Enrolled: pass. Hardware but nothing enrolled, or no passcode: review. No hardware: pass. |
| `ios.notification-previews` | Notification previews | Lock screen | Previews **Always**: review. When unlocked or never: pass. Notifications off for ForkMesh: pass. |
| `ios.jailbreak-apps` | Jailbreak apps | Device integrity | Any of Cydia, Sileo, Zebra, Installer, Filza, blackra1n, FakeCarrier, Icy, IntelliScreen, SBSettings, Dopamine, palera1n, TrollStore: critical. |
| `ios.jailbreak-tools` | Tools left by a jailbreak | Device integrity | `bash`, `sshd`, `ssh`, `apt`, Cydia's state, `cycript`, `sftp-server`, `/private/var/stash`: critical. |
| `ios.rootless-jailbreak` | Rootless jailbreak folders | Device integrity | `/var/jb`, `/private/preboot/jb`, palera1n's `binpack`: critical. |
| `ios.tweak-frameworks` | Tweak frameworks on disk | Device integrity | MobileSubstrate, Substitute, libhooker, TweakInject, ElleKit: critical. |
| `ios.injected-libraries` | Code injected into ForkMesh | Device integrity | A loaded image named like Substrate, Substitute, libhooker, TweakInject, ElleKit, Frida, cynject, Cycript, SSL Kill Switch or a jailbreak-hiding tweak: critical; the usage names them. |
| `ios.dyld-environment` | Library injection variables | Device integrity | `DYLD_INSERT_LIBRARIES`, `DYLD_LIBRARY_PATH`, `DYLD_FRAMEWORK_PATH`, `_MSSafeMode`, `_SafeMode`: critical; review in a debug build or the simulator, where Xcode sets them. |
| `ios.sandbox` | App sandbox | Device integrity | ForkMesh could write outside its sandbox: critical. |
| `ios.system-links` | System folders replaced by links | Device integrity | `/Applications`, `/Library/Ringtones`, `/Library/Wallpaper`, `/usr/include`, `/usr/libexec`, `/usr/share`, `/usr/arm-apple-darwin9` is a symbolic link: critical. |
| `ios.debugger` | Debugger attached | Device integrity | Traced: warning; review in a debug build. |
| `ios.simulator` | Real device | Device integrity | Running in the simulator: review. |
| `ios.os-version` | iOS version | System | Major at or above the newest known: pass; one behind: review; older: warning. |
| `ios.install-source` | Where ForkMesh came from | App protection | App Store or TestFlight: pass. Development or ad hoc signing: review. |
| `ios.debug-build` | Release build | App protection | Debug build: review. |
| `ios.data-protection` | ForkMesh file encryption | App protection | `complete`, `completeUnlessOpen`, `completeUntilFirstUserAuthentication`: pass. `none`: warning. |
| `ios.transport-security` | Encrypted connections only | App protection | `NSAllowsArbitraryLoads`: warning. Domains allowed over HTTP: review. |
| `ios.relay-https` | Relay address uses HTTPS | App protection | `https`: pass. `http` to `localhost`, a loopback or `.local` address: review. Other `http`: critical. |
| `ios.screen-capture` | Screen recording or mirroring | App protection | Screen captured: warning. |
| `ios.http-proxy` | HTTP proxy | Network | HTTP, HTTPS or automatic (PAC) proxy set: review; the usage names it. |
| `ios.vpn` | VPN | Network | Always passes; the detail says whether a VPN interface (`utun`, `ipsec`, `ppp`, `tap`, `tun`) carries traffic. |
| `ios.permission.camera` | Camera access | Privacy | Allowed: review, with what uses it. Otherwise: pass. |
| `ios.permission.microphone` | Microphone access | Privacy | As above. |
| `ios.permission.photos` | Photo library access | Privacy | Full access: review. Limited, denied or not asked: pass. |
| `ios.permission.speech` | Speech recognition | Privacy | Allowed: review. |
| `ios.permission.bluetooth` | Bluetooth access | Privacy | Allowed: review. |
| `ios.permission.motion` | Motion and fitness | Privacy | Allowed: review. |

The newest known iOS major is `PhoneSecurityPolicy.newestIOSMajor` (26).
Bump it when Apple ships a new major; a phone newer than it passes, so a stale
value never raises a false alarm.

## Android checks

| Id | Title | Category | Rule |
| --- | --- | --- | --- |
| `android.screen-lock` | Screen lock | Lock screen | Not secure: critical. |
| `android.biometrics` | Fingerprint or face unlock | Lock screen | Enrolled: pass. None enrolled: review. No hardware: pass. |
| `android.lock-timeout` | Screen timeout | Lock screen | Up to 5 minutes: pass; up to 30: review; longer: warning. |
| `android.lock-notifications` | Notification content on the lock screen | Lock screen | Private content shown: review. |
| `android.stay-awake` | Stay awake while charging | Lock screen | On: review. |
| `android.su-binary` | `su` binary | Device integrity | `su` in any common root path: critical. |
| `android.root-apps` | Root manager apps | Device integrity | Magisk, SuperSU, Superuser, KernelSU, APatch, KingRoot installed: critical. |
| `android.magisk-files` | Root framework files | Device integrity | Magisk, KernelSU or APatch folders: critical. |
| `android.busybox` | BusyBox | Device integrity | BusyBox in a system path: warning. |
| `android.test-keys` | Release signing | Device integrity | `Build.TAGS` has `test-keys`: warning. |
| `android.build-type` | Production build | Device integrity | `userdebug` or `eng`: warning. |
| `android.system-rw` | System partition read-only | Device integrity | `/`, `/system` or `/vendor` mounted `rw` in `/proc/mounts`: warning. |
| `android.hooking-frameworks` | Xposed or LSPosed | Device integrity | Xposed, LSPosed, EdXposed or LSPatch installed, their files present, Xposed classes loadable, or their libraries mapped: critical. |
| `android.injected-libraries` | Code injected into ForkMesh | Device integrity | Frida, Substrate or Xposed libraries in `/proc/self/maps`: critical. |
| `android.frida-server` | Instrumentation server | Device integrity | `frida-server` in `/data/local/tmp` or a system path: critical. |
| `android.selinux` | SELinux enforcing | Device integrity | `0` in `/sys/fs/selinux/enforce`: critical. |
| `android.emulator` | Real device | Device integrity | Emulator build: review. |
| `android.debugger` | Debugger attached | Device integrity | Attached: warning; review in a debuggable build. The fix opens Developer options. |
| `android.app-debuggable` | Release build of ForkMesh | App protection | Debuggable: review. |
| `android.install-source` | Where ForkMesh came from | App protection | Google Play, Galaxy Store, Amazon Appstore, F-Droid: pass. Sideloaded or unknown installer: review; the fix opens ForkMesh's app info. |
| `android.cleartext` | Encrypted connections only | App protection | Cleartext allowed for ForkMesh: warning. |
| `android.relay-https` | Relay address uses HTTPS | App protection | As on iPhone, and `10.0.2.2` (the emulator's address for the host computer) counts as local. |
| `android.hardware-keystore` | Secure hardware for keys | App protection | StrongBox or hardware keystore: pass. Neither: review. |
| `android.patch-level` | Security patch level | System | Up to 90 days old: pass; 180: review; 365: warning; older: critical. |
| `android.os-version` | Android version | System | API 34 or later: pass; 31–33: review; older: warning. |
| `android.encryption` | Storage encryption | System | Active (per user): pass. Default key: review. Inactive or unsupported: critical. |
| `android.developer-options` | Developer options | System | On: review. |
| `android.usb-debugging` | USB debugging | System | On: warning. |
| `android.wireless-debugging` | Wireless debugging | System | On: warning. |
| `android.auto-time` | Automatic date and time | System | Off: review. |
| `android.http-proxy` | HTTP proxy | Network | Set: review. |
| `android.vpn` | VPN | Network | Always passes; the detail says whether a VPN is on. |
| `android.user-certificates` | Added certificate authorities | Network | Any user-installed CA: warning; the usage names them. |
| `android.private-dns` | Private DNS | Network | Off: review. Automatic or a named provider: pass. |
| `android.accessibility-services` | Accessibility services | Apps with access | Any enabled: review; the usage names them. |
| `android.notification-listeners` | Apps that read notifications | Apps with access | Any other app: review. |
| `android.device-admins` | Device admin apps | Apps with access | Any: review. |
| `android.keyboard` | Keyboard | Apps with access | A system keyboard or Gboard, Samsung, SwiftKey: pass. Any other: review. |
| `android.permission.camera` | Camera access | Privacy | Granted: review, with what uses it. |
| `android.permission.microphone` | Microphone access | Privacy | Granted: review. |
| `android.permission.nearby` | Nearby devices | Privacy | Bluetooth scan or connect granted: review. |
| `android.permission.notifications` | Notifications | Privacy | Granted: pass. Refused: review (calls and pings cannot ring). |

## Severity choices

- A sign of jailbreak, root or injected code is critical because it needs a
  human look, not because it proves compromise: these tools turn off the
  sandbox that keeps the node key and chat keys private to ForkMesh.
- A missing screen lock is critical: without it the phone's storage encryption
  and the Keychain/Keystore protect nothing from someone holding the phone.
- Debug builds, debuggers and the simulator only review, since developers see
  them every day.
- Settings whose right answer depends on the user (developer options, a proxy,
  accessibility services, permissions) are reviews, with a usage line that
  says what needs them.

## Boundaries and limits

- A phone app sees only part of the system; a jailbreak or root kit that hides
  itself well passes every check. A clean result is not proof.
- Android hides some settings from apps (lock-screen notifications, private
  DNS, notification listeners on some versions); those report unavailable.
- Apps cannot read `/data/adb`, so the root framework folder check only sees
  what SELinux lets through; root manager apps and `su` carry most of the
  detection.
- Nearby devices is unavailable before Android 12, which has no such
  permission. Android's page also runs the checkup again when the app returns
  to the foreground, so a fix made in Settings shows at once.
- Nothing is uploaded and nothing changes unless the user follows a fix into
  Settings.

## Code and tests

| Path | Responsibility |
| --- | --- |
| `native/ios/ForkMeshCore/Sources/FMTools/PhoneSecurity*.swift` | Facts, catalog, explanations, scanner rules, filters, summaries. |
| `native/ios/ForkMesh/Platform/PhoneSecurityProbe.swift` | Reads the iPhone's facts. |
| `native/ios/ForkMesh/Features/Tools/PhoneSecurityScreen.swift` | The page: summary, filters, findings, detail sheet, settings links. |
| `native/ios/ForkMeshCore/Tests/FMToolsTests/PhoneSecurity*Tests.swift` | Every rule, boundary, filter and string, against fixture facts. |
| `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/phonesecurity/` | Facts, catalog, explanations, scanner rules, `/proc` parsers, filters, summaries, settings intents. |
| `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/phonesecurity/` | Probe, route registration and the page. |
| `native/android/core/src/test/kotlin/com/forkmesh/nativeapp/phonesecurity/` | Every rule, parser, boundary, filter and string, on the host JVM. |
| `marketplace/packages/phone-security/tests/test_phone_security.py` | The manifest, and that both apps implement exactly the check ids in this page. |

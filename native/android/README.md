# ForkMesh native Android app

The ForkMesh Android app, written in **Kotlin + Jetpack Compose**. Development
builds use the application id `com.forkmesh.nativeapp`; release builds ship as
`com.forkmesh.forkmesh`, the store/APK id, and take over the data the retired
Flutter app left on the device (see
[What carries over from the Flutter app](#what-carries-over-from-the-flutter-app)).

The developer home page for both native apps (this one and `native/ios`) is
[`docs/native/README.md`](../../docs/native/README.md); the reasons behind
Android-specific decisions are in
[`docs/native/ANDROID.md`](../../docs/native/ANDROID.md).
Since 2026-09-27 native development happens in this repository again. From
2026-09-24 it ran in the ForkMesh Next repository (`~/projects/forkmesh-backup/src`);
everything from there was brought back here and its copy of `native/` removed.

## Build, test, run

```sh
cd native/android
export JAVA_HOME=~/.forkmesh/toolchain/jdk   # any JDK 17+
./gradlew :core:coreTest                     # host-JVM unit tests
./gradlew :core:coreTest -Pfilter=Json       # only classes containing "Json"
./gradlew :app:assembleDebug                 # app/build/outputs/apk/debug/app-debug.apk
./gradlew :app:installDebug                  # onto a running emulator/device
# debug builds: every ui/kit component in light and dark
adb shell am start -n com.forkmesh.nativeapp/.MainActivity --ez fm_kit_gallery true
```

`tools/android-toolchain.sh`, sourced from the repository root, sets up a
rootless JDK and Android SDK under the shared toolchain folder and exports
`JAVA_HOME`, `ANDROID_HOME` and `PATH`; it reports the outcome in
`ANDROID_TOOLCHAIN_READY`, `ANDROID_TOOLCHAIN_STATUS` and
`ANDROID_TOOLCHAIN_REASON`. On a host whose IPv4 loopback is broken, Gradle
cannot reach its daemon; run it through `tools/loopback-netns.sh`
(`../../tools/loopback-netns.sh ./gradlew ...`), which runs the command
unchanged on a healthy host.

The **Launch native app (Android)** shortcut
(`.forkmesh/shortcuts/launch-native-android.sh`) installs the JDK and SDK when
missing, and targets `$ANDROID_SERIAL`, else the running `forkmesh-native-36`
emulator (Android 36, Pixel 7 profile; the Android 37 x86_64 emulator image
crash-loops its SurfaceFlinger on this host), else a single attached physical device,
else starts `forkmesh-native-36` on its own free console port. Other emulators are
never used or disturbed. It then builds, installs and opens the app.

The **Launch native app in emulator (Android)** shortcut
(`.forkmesh/shortcuts/launch-native-android-emulator.sh`) runs the same script
with `FORKMESH_NATIVE_ANDROID_TARGET=emulator`, which ignores `$ANDROID_SERIAL`
and attached phones and always uses `forkmesh-native-36`, starting it when it is
not running.

Toolchain: Gradle 9.7.1 (wrapper), Android Gradle Plugin 9.4.0 with built-in
Kotlin, Kotlin 2.4.20, Compose BOM 2026.09.00 (Compose 1.12), Activity 1.13,
coroutines 1.11, compileSdk/targetSdk 37, minSdk 26.

## Dependency policy

First-party only: AndroidX/Compose from Google and Kotlin coroutines from
JetBrains (`app/build.gradle.kts`). No OkHttp, Retrofit, Coil, kotlinx
serialization, Room, DI frameworks, icon packs or test frameworks. Everything
else is in this tree:

| Need                     | In this tree                                         |
| ------------------------ | ---------------------------------------------------- |
| HTTP                     | `core/net/HttpClient` over `HttpURLConnection`       |
| WebSocket                | `core/net` WebSocket client (RFC 6455)               |
| Cryptography (incl. post-quantum) | `core/crypto` (JCA where the platform has it, pure Kotlin otherwise) |
| Git                      | `core/git`                                           |
| Preferences              | `app/platform/Prefs`                                 |
| Secret storage           | `app/platform/SecureStore` (Android Keystore AES-GCM)|
| Service registry / state | `core/core/Services` + `StateFlow`                    |
| Links, file picking, biometrics, device info, app directories | Intents, Storage Access Framework, `BiometricPrompt`, `Build`, `Context.filesDir` |
| Archives                 | `java.util.zip`                                      |
| Icons (Material, MingCute) | `FmIcon("name")` over the bundled icon fonts       |

Adding any other dependency needs an explicit decision, not a convenience.

## Layout

```
core/   Pure Kotlin/JVM, no android.*: json, net (HTTP, WebSocket), crypto,
        git, models + API client + protocols. Tested on the host JVM.
app/    Android + Compose.
  platform/     Prefs, SecureStore, AppLifecycle
  ui/theme/     ForkMeshTheme (design tokens), FmIcon
  ui/navigation Screen, Navigator, NavigatorHost
  ui/kit/       shared Compose components
  shell/        HomeShell
  feature/      One package per area, each with a *Feature object:
    account/    settings, identity, auth flow, API, inbox, notifications, orgs
    code/       repositories and repository detail
    localrepos/ on-device Git
    chat/       relay, chat, local mesh, node sync, account events
    agents/     agent sessions and on-device coding agents
    tools/      activity, telemetry, logs, performance, bug reports
```

## Conventions

- **Wire and storage compatibility.** Endpoints, storage keys and signing
  formats match the other ForkMesh clients (the iOS app, the desktop and the
  web client) and what the retired Flutter app stored on the device; the relay
  and the desktop app cannot tell the clients apart.
- **Code only.** No comments or KDoc in the source. The reasons behind
  non-obvious decisions go in `docs/native/ANDROID.md`.
- **Feature seams.** The shell only calls the `*Feature` objects. A feature
  area owns its package and its `*Feature.kt`; it registers services in
  `install(app)` with `Services.put(...)` and exposes screens through
  factories. Do not edit another area's package.
- **State.** Services hold state in `StateFlow`s (read in Compose with
  `collectAsState()`); a `Screen` object holds its own UI state in
  `mutableStateOf` fields, so it survives while lower in the stack or on a
  hidden tab.
- **Threads.** `core` is blocking; call it with
  `withContext(Dispatchers.IO)` from coroutines (`rememberCoroutineScope()`,
  `LaunchedEffect`, or `AppLifecycle.scope` for services).
- **Tests.** Logic lives in `core` and is tested there: a class named `*Test`
  with public `test*()` methods, asserting with `expect`, `assertEq`,
  `assertFails` (`core/src/test/.../testing/Check.kt`). Test vectors go in
  `core/src/test/resources`.

## Release builds (`com.forkmesh.forkmesh`)

The release APK is built by the release host (`BUILD_APK_SCRIPT` and
`build_apk` in `tools/forkmesh-release-host.py`) as `com.forkmesh.forkmesh`,
the application id of the released Android APK. Every other build is
`com.forkmesh.nativeapp`, so it installs next to the released app.

### Build properties

| Property | Default | Purpose |
| --- | --- | --- |
| `-Pforkmesh.appId=com.forkmesh.forkmesh` | `com.forkmesh.nativeapp` | Build as the store/APK app |
| `-Pforkmesh.versionCode=<n>` | `10400` | Must be above every earlier `com.forkmesh.forkmesh` APK. The release host uses `MAJOR*10000 + MINOR*100 + PATCH` (v1.2.4 is 10204) |
| `-Pforkmesh.versionName=<x.y.z>` | `1.4.0` | Shown in Settings and the Play/installer metadata. The release host sets it from the release tag |

Release signing comes from four variables:
`FORKMESH_ANDROID_KEYSTORE`, `FORKMESH_ANDROID_KEYSTORE_PASSWORD`,
`FORKMESH_ANDROID_KEY_ALIAS`, `FORKMESH_ANDROID_KEY_PASSWORD` (all or none).
A `com.forkmesh.forkmesh` release build without them fails; it is never signed
with the debug key.

### What carries over from the Flutter app

Earlier releases of `com.forkmesh.forkmesh` were the Flutter app, which has
been retired. On the first launch after the update the native app copies the
Flutter app's data into its own stores (`core/.../migration`,
`app/.../feature/account/projects/FlutterDataMigration.kt`), before any
service reads them:

- `flutter_secure_storage` (10.3.4 custom ciphers: RSA-OAEP or PKCS#1 wrapped
  AES-GCM or AES-CBC keys under `<applicationId>.FlutterSecureStoragePluginKey[OAEP]`,
  and the 9.2.4 EncryptedSharedPreferences layout from 2026-08-08 to 2026-09-14)
  into the Keystore-backed `SecureStore`, under the same key names: node key,
  owner key, per-project sessions (`auth/session/<projectId>`), vault device
  copy, agent credentials, `p/<projectId>/` scopes.
- `FlutterSharedPreferences` `flutter.<key>` into the native preferences,
  including the project list and the active project. An upgraded install with
  no recorded project lands on ForkMesh; a fresh install starts on ForkMesh
  (`app.forkmesh.com`), and a stored ForkMesh Next project is folded into it.
- The Flutter copies are deleted only after the relay accepts the migrated
  session (`GET /api/accounts/devices`), and never while any Flutter secret
  could not be decrypted. The migration is idempotent and retries while the
  Keystore is unavailable.

The Keystore keys only survive because the application id and signing key stay
the same: a reinstall under another id or key starts empty.

### Release checklist

1. The migration has been checked on a device that ran the last Flutter APK
   (signed in, with a local repository and a vault unlocked): after the update
   the user is still signed in on every project, the vault opens, and the
   Flutter prefs files are gone after the first verified session.
2. To build the release APK by hand, work in a worktree at the release commit
   and use the **v3** direct-distribution key the APK is signed with since
   v1.2.3 (`~/projects/forkmesh/data/release/android-direct-distribution-v3.jks`,
   alias `forkmesh`, password file `.apk-keystore-password` next to it; never
   print it):
   ```sh
   cd native/android
   export JAVA_HOME=~/.forkmesh/toolchain/jdk
   ./gradlew :core:coreTest :app:assembleRelease \
     -Pforkmesh.appId=com.forkmesh.forkmesh \
     -Pforkmesh.versionCode=<MAJOR*10000+MINOR*100+PATCH> \
     -Pforkmesh.versionName=<MAJOR.MINOR.PATCH>
   ```
   Output: `app/build/outputs/apk/release/app-release.apk`.
3. Verify: `apksigner verify --print-certs` must report the single signer
   `e8c028a49936350273a1bcb4e822c0d4f8fedb98c3417ba9eb9d3c617a33f49a`
   (`DEFAULT_SIGNER_SHA256` in `tools/forkmesh-release-host.py`), and
   `aapt2 dump badging` must show `package: name='com.forkmesh.forkmesh'` with
   the expected `versionCode`. The release host runs both checks on its own
   build.
4. The asset name is `forkmesh-android-universal.apk`: the homepage download
   widget (`www/public/homepage-downloads.js`), the install support matrix
   (`www/docs/install-support-matrix.{json,md}`), the status page's release
   checks (`app/src/status_monitoring.py`) and the release manifests
   (`.forkmesh/releases/*/release.json`, `SHASUMS256.txt`) use it. Publish the
   APK into the candidate with `tools/forkmesh-release-publish.sh` and promote
   it as usual (`.forkmesh/releases/README.md`).
5. Device support: the native app's `minSdk` is 26. A phone on Android 7.x
   that runs the Flutter APK cannot take the update; say so in the release
   notes.

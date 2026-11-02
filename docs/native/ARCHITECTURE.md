# Phone apps: architecture and development

Part of [the native docs](README.md). How `native/ios` and `native/android`
are laid out, the rules both apps follow, the interfaces they speak, their
configuration, how to build, test and release them, their security
boundaries and the traps met so far. Where the phones sit in the system:
[architecture](../architecture.md); keys and signatures:
[encryption](../encryption.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `native/ios/ForkMeshCore/` | Swift package with everything that is not a view; builds and tests on Linux. Modules include FMBase, FMCrypto, FMNet, FMGit, FMDiff, FMVault, FMAccount, FMApi, FMChat, FMCode, FMLocalAI, FMAgents, FMTools, FMAssistant, FMPush, FMControl and the per-area modules in [DESKTOP_PARITY.md](DESKTOP_PARITY.md); API notes in [ios/](ios/README.md). |
| `native/ios/Nexus/` | Standalone Swift 6 network-intelligence package: `NexusKit` (host API), `NexusUI` (embeddable SwiftUI). |
| `native/ios/ForkMesh/` | The app target: SwiftUI views and iOS glue (`App/`, `Platform/`, `Theme/`, `Kit/`, `Shell/`, `Features/`). |
| `native/ios/NotificationService/`, `native/ios/Shared/` | Notification service extension (links `ForkMeshPushCore`: FMBase, FMCrypto, FMPush); code compiled into several targets. |
| `native/ios/ForkMeshWidgets/`, `ForkMeshShare/`, `ForkMeshAutoFill/`, `ForkMeshWatch/`, `ForkMeshWatchWidgets/` | Extensions and the watch app. |
| `native/ios/Config/` | Info.plists, entitlements, `Version.xcconfig`, `Package.resolved`, `ExportOptions.plist`. |
| `native/ios/tools/` | `generate_project.rb` (the Xcode project is generated), `check_swift_syntax.sh`, `asc_bundle_ids.py`, the Mac build scripts. |
| `native/android/core/` | Pure Kotlin/JVM (no `android.*`), tested on the host JVM. |
| `native/android/app/` | Android + Compose: platform, theme, navigation, kit, shell, one package per feature area. |
| `changelog/changelog.json` | The changelog the apps' What's new and forkmesh.com/changelog show ([../changelog.md](../engineering/changelog.md)). |

Shortcuts (`.forkmesh/shortcuts/`):

| Shortcut | Does |
| --- | --- |
| `launch-native-android.sh`, `launch-native-android-emulator.sh`, `test-native-ios.sh` | Build and launch Android (device or emulator); run the iOS core tests |
| `deploy-native-ios-ad-hoc.sh` | `tools/next_codemagic.py build ios-native-adhoc --mirror --execute --wait`: waits for the public mirror to serve `main`, starts the Codemagic build on the app that holds the `app_store_connect` group, and follows it live |
| `deploy-native-ios-ad-hoc-mac.sh`, `deploy-native-ios-app-store-mac.sh`, `build-desktop-macos-mac.sh` | The same builds on the office Mac ([MAC_BUILD.md](MAC_BUILD.md)) |
| `ios-app-store-status.sh`, `testflight-feedback-to-tasks.sh` | Read-only App Store Connect report; TestFlight feedback to tasks ([IOS_RELEASE.md](IOS_RELEASE.md)) |
| `task-mobile-app-store-deploy.sh`, `task-mobile-play-store-deploy.sh` | Create a signed reminder task with the version, tag, `main` head and the release steps for the App Store or the Play Store |

The reminder goes to the release maintainer named by `FORKMESH_RELEASE_ASSIGNEE`,
else by the one line in `~/projects/forkmesh/data/core/config/release-assignee`
(kept out of the public repository). With neither set the task is created
unassigned and the shortcut says so, because shortcuts run from the phone or
desktop do not export the variable.

The Play Store path is only that reminder: no tool uploads to Google Play, and
Android releases ship as the signed `forkmesh-android-universal.apk` in the
release manifest.

## Architecture

- **One design, two apps.** Where the Kotlin app already solved a problem
  (pure-language crypto, the Git engine, vectors), the Swift app follows it.
- **Logic in the core, views in the app.** iOS: `ForkMeshCore` has no
  Apple-only imports outside `#if canImport`, so Linux compiles and tests all
  logic; views bind to `@Observable` view models. Android: `core` holds the
  logic, services hold `StateFlow`s, feature areas register through `*Feature`
  objects.
- **First-party dependencies only.** OS frameworks or code in the tree. The
  one exception is on-device models on iOS: MLX (`mlx-swift`, `mlx-swift-lm`)
  and `swift-transformers` run Gemma, fenced into the app target
  (`ForkMesh/Features/LocalAI/MLXGemmaEngine.swift`).
- **Installer hooks.** Areas plug into
  `native/ios/ForkMesh/App/AppServices+<Area>.swift` installers and never edit
  `AppServices.swift` ([PARITY.md](PARITY.md), section 3.0).
- **Projects.** A project is a name plus everything the phone holds for it
  (session, node key, owner key, vault copy, local repositories, agent records,
  settings), usually attached to one ForkMesh instance whose origin is its id.
  The original project owns the unprefixed namespace; others use
  `p/<projectId>/`.
  - An instance belongs to one project. Adding an instance another project
    already uses (compared by `instanceId`, the origin) opens that project or
    is refused with "That instance already belongs to the project …", on both
    platforms.
  - A project made from a name alone has `hasInstance` false, a
    `local-<slug>-<random>` id and no server URL; it works on the device alone,
    and choosing an instance is offered, never required.
  - The instance check reads `GET /api/instance`: its `name` replaces
    "ForkMesh" in "Connected to …" and, on iOS, fills an empty Name field.
  - Project logo: a project with no logo takes the instance's `logo`, else its
    favicon. An instance older than the `logo` field (`branded: true`, no
    `logo` key) is asked for `/favicon/site.webmanifest` and its first
    `/instance-logo.png|jpg|webp` icon is used. A branded instance never gives
    its favicon (that is the stock ForkMesh icon), so with no logo it keeps the
    letter icon. Phones cannot draw SVG, so the brand tool keeps a PNG copy
    ([INSTANCES.md](../operations/instances.md#white-label)).
- **New project sheet (iOS).** Once "Check instance" passes, the sheet shows
  the instance's logo as the project image before creation. Candidates come
  from `AddProjectModel.logoCandidates`, in order: `/api/instance` `logo`, the
  directory logo, then the two favicons (not for a branded instance). A logo
  the user picked is never replaced; an instance logo goes when the host
  changes. "Instances on ForkMesh" lists ForkMesh plus every `listed` https
  entry of `GET https://app.forkmesh.com/api/start/instances`
  (`FMChat/Relay/InstanceDirectory.swift`; directory logos only in the
  `/api/start/instances/<id>/logo?v=<n>` shape). Add fills, checks and creates
  in one tap; an existing project shows "Added". If the directory cannot be
  read, only ForkMesh is listed.
- **Relay features.** `/api/version` declares a `features` list; the core
  (`accounts`, `repositories`, `repoRead`) is required, every other feature
  gates its own UI, and a missing one leaves the UI absent rather than broken.
  A 404 under `/api/repo/<owner>/<name>/...` never marks a feature missing.
- **No polling for display.** Screens reload when the account event socket,
  the relay or the local network says something changed.
- **No toasts.** Feedback shows inline where the action ran.
- **Calendar saves (iOS)** (`FMPlanning/CalendarModel.swift`,
  `Features/Planning/Calendar/CalendarScreen.swift`):
  - Each `load()` has a token and only the newest answer is applied. Every
    create, edit, delete and RSVP is recorded with a revision and re-applied on
    top of a list requested before it, until a list requested after it
    arrives; otherwise a stale list wiped new events and revived deleted ones.
  - Save stays enabled once there is a title. Tapping it shows any validation
    problem (more than five reminders, a meeting link without `https://`, an
    end before the start) at the top of the form, scrolls there and plays the
    warning haptic; a spinner replaces Save while the request runs. No error
    shows before Save is tapped.
  - `CalendarEventScreen` shows its own editor sheet.
  - Server side, `calendar_api.handle` answers `invalid_json` only for a body
    that does not parse or is not an object, so a coding error is a 500, not a
    misleading "Invalid json." (`app/tests/host/test_calendar_runtime.py` runs
    the real `_CalendarRuntime.json()`).
- **Chat history survives updates and restarts (iOS)**:
  [FMChat](ios/FMChat.md#chat-history-file-threadschathistorystoreswift).
- **Sign-ins survive a phone restart (iOS)**:
  [FMAccount](ios/FMAccount.md#freshinstallreset-freshinstallresetswift).

## Protocols and interfaces

- Relay REST (`/api/...`) and WebSockets: the relay room socket and the
  account event socket (`/api/nodes/events`). The phone as a node:
  [mobile-node-contract.md](../mobile/mobile-node-contract.md);
  agents from the phone:
  [mobile-agent-access.md](../mobile/mobile-agent-access.md).
- Push: `app/src/apns.py` pushes the `pings`, `direct-messages`, chat,
  `code-push` and `calls` topics to iOS (`calls` is a PushKit VoIP push when
  the phone registered a VoIP token, see
  [chat-direct-calls.md](../app/chat-direct-calls.md)); every payload carries
  `origin` so a tap routes to the right project. Android has no push
  transport (no FCM, by design): a foreground service keeps the account event
  socket open ([ANDROID.md](ANDROID.md), section 6). A desktop's pings reach
  its linked phone as `desktop_ping` rows ([DESKTOP_PINGS.md](DESKTOP_PINGS.md)).
- Desktop control: on the same Wi-Fi the phone finds the desktop through the
  `#mesh` beacon and drives it over the mesh TCP port; away from home the same
  protocol is forwarded through the relay. Both use the signed handshake and
  per-direction encryption. A phone signed in to the desktop's account gets the
  enabled desktop keys from `GET /api/accounts/desktops` and needs no LAN
  meeting; any other phone needs a six-digit pairing code approved on the LAN.
  Frames are the twin of `desktop/src/LocalControl.{h,cpp}`, pinned by shared
  test vectors.
- Owner-only families (hosts, agents, merge queue, ...) compare the account
  name the desktop holds for the phone with its own account:
  - The name a phone sends with its first hello is only a claim: the prompt
    reads "Guest phone (says it is alice)", and a code approval drops a
    claimed name equal to the desktop's owner.
  - When a code-approved phone later names an account its record lacks, the
    desktop asks `devices/check`; only if the account lists the phone's key is
    the name recorded (approval stays `code`, so signing out never forgets it).
  - Owner-only families and `hosts.*` require an account-approved device or a
    name confirmed by `devices/check` (`userConfirmed` in
    `paired-devices.json`, `LocalControlServer::userVerified`). Older records
    naming the owner without that are rechecked on next connect.
  - A guest still starts, steers and stops agents and sends files; owner-only
    screens show "Only the account that owns this desktop ...".
- Cryptography is pinned by shared vectors across Kotlin, Swift and the
  desktop: [FMCrypto.md](ios/FMCrypto.md).

## Configuration

- iOS: `native/ios/Config/Version.xcconfig` holds `MARKETING_VERSION`,
  `CURRENT_PROJECT_VERSION`, and the build's `FORKMESH_ENV`,
  `FORKMESH_API_HOST`, `FORKMESH_WORLD_URL`, `FORKMESH_BUILD_COMMIT`.
- Android: `native/android/app/build.gradle.kts`: `-Pforkmesh.appId`,
  `-Pforkmesh.versionCode`, `-Pforkmesh.versionName`, `-Pforkmesh.changelog`;
  release signing from `FORKMESH_ANDROID_KEYSTORE`,
  `FORKMESH_ANDROID_KEYSTORE_PASSWORD`, `FORKMESH_ANDROID_KEY_ALIAS`,
  `FORKMESH_ANDROID_KEY_PASSWORD` (without them a `com.forkmesh.nativeapp`
  release build uses the debug key).
- CRM Zoho Self Client: the client ID, secret and refresh token stay in the
  server's encrypted organization settings. The API returns a
  `hasClientSecret` flag, never the secret; the iOS reconnect form loads the
  client ID and data center and reuses the saved secret only for that same
  client ID. Partial saves and redacted or empty credential fields keep the
  stored credentials; only disconnect removes them. Refresh failures from
  connectivity, rate limits or outages stay retryable; only a definitive OAuth
  credential error asks for reconnection. A successful sync or send clears
  earlier rejection markers, and a refresh answer without scopes keeps the
  known permissions.

## Build, run, test

```sh
# iOS core, on Linux (swift.org toolchain)
export PATH="$HOME/projects/forkmesh/toolchain/swift/usr/bin:$PATH"
cd native/ios/ForkMeshCore && swift test          # FM_ONLY=<Module> for one module
native/ios/tools/check_swift_syntax.sh

# Android
cd native/android
./gradlew :core:coreTest
./gradlew :app:assembleDebug
. tools/android-toolchain.sh                      # rootless Android SDK/JDK toolchain
tools/loopback-netns.sh ./gradlew …               # hosts whose IPv4 loopback is broken
```

Gradle fails on hosts that drop loopback TCP with errors that never mention
loopback; `tools/loopback-netns.sh` runs the command in a namespace with a
working loopback.

The iOS app target compiles only on macOS: on Codemagic (`ios-native-build`,
`ios-native-release`, `ios-native-adhoc` in `/codemagic.yaml`) and on the
office Mac, which runs `ios-native-adhoc` for every push to `native/ios` and
emails a tap-to-install link ([MAC_BUILD.md](MAC_BUILD.md)).

## Operations

- **iOS release.** `ios-native-release` archives, uploads, adds the build to
  TestFlight internal testing and, for tagged releases, submits it for App
  Review ([IOS_RELEASE.md](IOS_RELEASE.md)).
- **Android APK.** `tools/forkmesh-release-host.py` builds
  `forkmesh-android-universal.apk` as `com.forkmesh.forkmesh`, signed with the
  v3 direct-distribution key (`android-direct-distribution-v3.jks` under the
  home project's `data/release/`). See [../release/README.md](../release/README.md)
  and, for how releases reach users,
  [mirroring](../mirroring.md).
- `native/ios/tools/asc_bundle_ids.py ensure` registers the bundle ids and
  capabilities (reads `ASC_KEY_ID`, `ASC_ISSUER_ID` and the key under
  `~/.appstoreconnect/private_keys/`).
- **Diagnostics.** "Send diagnostics" (the composer's "+" menu and
  `/diagnostics`) puts a performance report into the prompt, trimming its
  lowest-ranked sections to fit and listing crashes as the attributed thread's
  top frames. `PerformanceReport.text` runs `DiagnosticsBundle.scrub` on every
  report (tokens, emails, message text, conversation, channel and room names).
  The prompt sends the agent to [PERFORMANCE_CHECKS.md](PERFORMANCE_CHECKS.md)
  (which also holds the iOS performance rules) and asks it to add its own
  check there.

## Security

Threat model for phones: [threat-model.md](../protocol/threat-model.md).

- Keys live in the Keychain (`native/ios/ForkMesh/Platform/KeychainStore.swift`)
  and the Android Keystore (`app/platform/SecureStore`).
- iOS Keychain groups (`FMAccount/KeychainPlacement.swift`): the app lists
  `…forkmesh.private`, `…forkmesh.push` and `…forkmesh`, in that order. The
  node key, owner key, sessions, vault copy and every other secure-store item
  live in `private`, which no extension lists, so no extension can read them.
  The notification service extension lists only `push`: push session items,
  room keys, the verified-names map and the chat keyring (`chat/e2ee/…`), which
  it needs to open sealed chat pushes. Share, Widgets and AutoFill keep
  `…forkmesh` for the share drop, surface snapshot and AutoFill cache.
  `KeychainStore` moves old items out of `…forkmesh` once per launch (add to
  the new group, then delete the old copy).
- Ed25519 is never delegated to CryptoKit (randomised signatures would break
  the pinned deterministic vectors).
- "Confirm it's you" fails closed. The plain Confirm dialog is only for a phone
  with no screen lock (Android `security/UserPresencePolicy`; iOS
  `LocalAuthPresencePrompt` returns nil only for `passcodeNotSet`). Any other
  error, a sensor timeout included, is a refusal, otherwise a person holding an
  unlocked phone could wait out the prompt and tap Confirm. Android 8 and 9 have
  no prompt that accepts the passcode, so `UserPresenceActivity` opens the
  system's credential screen instead.
- Notification actions that drive an agent need an unlocked phone: Deny, Retry
  and Continue join Approve, Merge and Approve PR in `requiresAuthentication`.
  iOS sets `.authenticationRequired`. Android sets `setAuthenticationRequired` on
  API 31 and later, and `NotificationActionReceiver` refuses them while the device
  is locked on every version, because a broadcast action otherwise runs from the
  lock screen. Mark read and chat Reply stay available there.
- A LAN desktop is not trusted for saying it paired the phone:
  [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md#confirming-a-desktop-on-the-phone).
- The Mastodon sign-in belongs to one ForkMesh session in one project. Clearing the
  local ForkMesh session (sign-out, revoked device, expired session, deleted
  account) deletes `mastodon.*` first and then revokes the token on the server
  (`AuthService.onLocalSessionCleared` on iOS, `AuthController.onLocalSessionCleared`
  on Android), so the next account on the phone never inherits it. Android's
  `MastodonState` is a process-wide object, so it also resets on
  `ProjectLifecycle.onClose`; iOS builds `MastodonHub` per screen over the
  project-routed store.
- Mastodon URL fields (`url`, `uri`, avatar, header, media) pass
  `MastodonServer.webLink`, which keeps only `https` and `http`, like post bodies.
  "Open in browser" and media tiles would otherwise hand a server-chosen
  `shortcuts://` or `forkmesh://` address to the system.
- The iOS app shows no prices, plans or paid features and moves no money
  ([IOS_RELEASE.md](IOS_RELEASE.md), "App Review: nothing paid on iOS").
- **Phone Security** (`phone-security`) is the phones' checkup, the
  counterpart of the desktop's Security package
  ([../marketplace/phone-security.md](../marketplace/phone-security.md)).
- **Eye Control** (`eye-control`, iOS only, experimental) moves a cursor with
  ARKit face tracking and taps on a firm blink (route `eyeControl`;
  [../marketplace/eye-control.md](../marketplace/eye-control.md)).
- **Router** (`openwrt`) manages an OpenWrt router on the phone's Wi-Fi (route
  `openWrtRouter`, web path `/dashboard/openwrt`). Logic in
  `FMPackages/OpenWrt*.swift` and core `openwrt/`; screens in
  `ForkMesh/Features/Router/` and `feature/openwrt/`. How it gets past ATS and
  Android's cleartext policy:
  [../marketplace/openwrt.md](../marketplace/openwrt.md#phones).

## Traps

- An unsigned iOS archive loses `aps-environment` at export unless the release
  entitlements are embedded first; `codemagic.yaml` refuses a build that
  cannot receive push.
- Core tests can fail on macOS where Linux passes; `ios-native-build` runs them
  last so they cannot hide whether the app compiles.
- On Linux, a synchronous `@MainActor func test…()` in a class that is not
  `@MainActor`, or a synchronous `throws` test in a `@MainActor` class, aborts
  the whole test bundle at discovery. Declare such tests `async`.
- Keeping Gemma loaded needs the increased-memory-limit capability enabled in
  the developer portal before it is added to the entitlements, or the export
  finds no matching profile.
- Never start an endless animation with `withAnimation(….repeatForever(…))` in
  `onAppear`: a layout change caught by that transaction replays forever. The
  `Kit/` loaders draw from a `TimelineView(.animation)` clock instead.
- `AVAudioEngine` must be built fresh after each audio-session switch;
  installing a tap whose format does not match the hardware input throws.
  Nothing else may switch the session to `.playback` while dictation listens.

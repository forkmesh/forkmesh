# Native apps (iOS and Android)

`native/ios` (Swift + SwiftUI) and `native/android` (Kotlin + Jetpack Compose)
are the ForkMesh phone apps. Each signs in to a ForkMesh instance, browses and
edits repositories, runs and watches agents, chats, and carries the Notes,
Tasks, Calories and Password Vault tools. A phone is a *mobile node*: it holds
its own Ed25519 node key and owner-encryption key and can clone and commit
with its own Git engine. The two apps share endpoints, storage keys and
signing formats, so the relay and the desktop cannot tell them apart.

- iOS ships as the App Store app **ForkMesh** (`com.forkmesh.forkmesh`) from
  1.4.0 ([IOS_RELEASE.md](IOS_RELEASE.md)).
- The Android release APK (the desktop release's
  `forkmesh-android-universal.apk` asset) is built as
  `com.forkmesh.forkmesh`; local builds default to
  `com.forkmesh.nativeapp`, which installs beside it
  ([ANDROID.md](ANDROID.md) section 4).
- Both replace the retired Flutter app, whose data they migrate on the first
  launch (below). The Flutter source (`mobile/`), its CI workflows
  (`.forkmesh/build-android.yml`, `build-ios.yml`, `release-ios.yml`,
  `codemagic-adhoc.yml`) and its Codemagic workflows were removed on
  2026-09-28; the migration code stays for users upgrading from a store build.
  Its build output (`.dart_tool`, `.gradle`, generated plugin registrants)
  was committed back as `mobile/` minutes later (c020be2610) and removed
  again; `.gitignore` now ignores `/mobile/`.

**Where development happens.** Here. Since 2026-09-27 native development
happens in this repository again. From 2026-09-24 it ran in the ForkMesh Next
repository (`~/projects/forkmesh-backup/src`); its native code and the tracker
updates left on its unmerged branches were brought back here, and its copies of
`native/` and `docs/native/` were removed. Do not develop native code in the
backup.

Desktop agents on the phone (relay without LAN pairing, diff, merge, delete):
[DESKTOP_AGENTS.md](DESKTOP_AGENTS.md).

Updating and restarting the desktop from its page, with the build log
streamed live: [DESKTOP_UPDATE.md](DESKTOP_UPDATE.md).

Using Siri with the iPhone app (the user guide):
[`www/docs/mobile/siri.md`](../../www/docs/mobile/siri.md).

The diff review stream (viewed marks, health scores, find, the toolbar):
[DIFF_REVIEW.md](DIFF_REVIEW.md).

The Commits screen's Graph tab (lane graph, where Desktop, Web and Mobile are):
[COMMIT_GRAPH.md](COMMIT_GRAPH.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `native/ios/ForkMeshCore/` | Swift package with everything that is not a view; builds and tests on Linux. Modules FMBase, FMCrypto, FMNet, FMGit, FMDiff, FMVault, FMAccount, FMApi, FMChat, FMCode, FMLocalAI, FMAgents, FMTools, FMAssistant, FMPush. |
| `native/ios/ForkMesh/` | The app target: SwiftUI views and iOS glue (`App/`, `Platform/`, `Theme/`, `Kit/`, `Shell/`, `Features/`). |
| `native/ios/NotificationService/`, `native/ios/Shared/` | Notification service extension (links the `ForkMeshPushCore` product: FMBase, FMCrypto, FMPush); code compiled into both targets. |
| `native/ios/Config/` | Info.plists, entitlements, `Version.xcconfig`, `Package.resolved`, `ExportOptions.plist`. |
| `native/ios/tools/` | `generate_project.rb` (the Xcode project is generated), `check_swift_syntax.sh`, `asc_bundle_ids.py`. |
| `native/ios/docs/` | One API note per core module. |
| `native/android/core/` | Pure Kotlin/JVM (no `android.*`), tested on the host JVM. |
| `native/android/app/` | Android + Compose: platform, theme, navigation, kit, shell, one package per feature area. |
| `.forkmesh/shortcuts/launch-native-android.sh`, `.forkmesh/shortcuts/launch-native-android-emulator.sh`, `.forkmesh/shortcuts/test-native-ios.sh` | Desktop shortcuts to build and launch Android (on a device or its own emulator, or always in the emulator), and to run the iOS core tests. |
| `.forkmesh/shortcuts/deploy-native-ios-ad-hoc.sh` | A script shortcut that runs `tools/next_codemagic.py build ios-native-adhoc --mirror --execute --wait`: it waits until `https://forkmesh.com/forkmesh/forkmesh` serves this checkout's `main` (or a newer commit), starts the build on the `forkmesh` Codemagic app (`6aac6e1e…`), follows it live (each step as it starts and ends, plus up to 20 new lines of the running step's log every 15 s, Codemagic's HTML colouring stripped) and ends with the steps, the IPA artefact or the failing step's log. It replaced a prompt shortcut on 2026-10-01: an agent run of the same steps did not ship a build, and a script does the same thing every time. It runs from the desktop's Shortcuts tab or from the phone's repository Shortcuts tile, which streams its output. Why `--mirror` (2026-10-01): the first script version pushed `main` to the core nodes' private copy for the `forkmesh-next` app, and every run stopped there, because that copy still holds the history from before ForkMesh Next was adopted into this repository, so the push is rejected as non-fast-forward. The `forkmesh-next` app also has no `app_store_connect` group, so it could not sign an ad hoc build. The `forkmesh` app has the group and builds from the public mirror (docs/OPERATIONS.md, "Native app builds"). |
| `.forkmesh/shortcuts/deploy-native-ios-ad-hoc-mac.sh` | The same ad hoc deploy on the office MacBook instead of Codemagic (no Codemagic minutes): runs `tools/mac_adhoc_deploy.py --execute`, which waits for the cores to serve `main`, builds Codemagic's `ios-native-adhoc` steps on the Mac with the live log, then publishes the IPA and emails the install link. See [MAC_BUILD.md](MAC_BUILD.md), "The shortcut". |
| `.forkmesh/shortcuts/build-desktop-macos-mac.sh` | Builds the universal macOS desktop `.dmg` from `main` on the office MacBook: runs `tools/mac_adhoc_deploy.py --desktop --execute` and copies the image to `~/projects/forkmesh/data/mac-adhoc/desktop/<time>/`. The iPhone's Desktops page runs both Mac shortcuts from the Office MacBook row ([MAC_BUILD.md](MAC_BUILD.md), "The macOS desktop shortcut") |
| `.forkmesh/shortcuts/deploy-native-ios-app-store-mac.sh` | Ships `main` to the App Store for review from the office MacBook: runs `tools/mac_adhoc_deploy.py --app-store --execute`, which waits for the cores to serve `main`, builds Codemagic's `ios-native-release` steps on the Mac with `FORKMESH_IOS_APP_STORE=1`, uploads the build, adds it to TestFlight internal testing and submits it for App Review. See [MAC_BUILD.md](MAC_BUILD.md), "The App Store shortcut". |
| `.forkmesh/shortcuts/ios-app-store-status.sh` | Read-only report on the iOS app in App Store Connect: runs `tools/mac_adhoc_deploy.py --store-status`, using `ASC_KEY_ID` and `ASC_ISSUER_ID` from its environment or desktop Actions variables, then the authenticated office-Mac connection if needed. It shows App Store versions (waiting for review, in review, rejected), App Review submissions, the app's downloads (all time and the last 7 and 30 days, from the Sales reports; needs `ASC_VENDOR_NUMBER`, added 2026-10-13), the newest TestFlight builds and their groups, every TestFlight group, tester feedback and customer reviews ([IOS_RELEASE.md](IOS_RELEASE.md), "Store status shortcut"). Added 2026-10-09. |
| `.forkmesh/shortcuts/testflight-feedback-to-tasks.sh` | Turns TestFlight tester feedback into tasks in `forkmesh/forkmesh`, each with the tester's comment, device, build and screenshots attached, and deletes a feedback from App Store Connect once its task is done: runs `tools/testflight_feedback_tasks.py` (`--dry-run` changes nothing). See [IOS_RELEASE.md](IOS_RELEASE.md), "TestFlight feedback to tasks". |
| `.forkmesh/shortcuts/task-mobile-app-store-deploy.sh` | Creates a reminder task, "Reminder: deploy the iOS app to the App Store", in `forkmesh/forkmesh`, assigned to Kaif (`mdkaifansari04`), through `create_task` in `tools/forkmesh_mcp_server.py`, signed with this node's identity as the desktop account. The details give the iOS `MARKETING_VERSION`, the newest `v*` tag and `main`'s head, the `native/ios/tools/asc_appstore.py status` and `plan` commands, how to start `ios-native-release` with `FORKMESH_IOS_APP_STORE=1`, and a link to [IOS_RELEASE.md](IOS_RELEASE.md), "App Store rollout for a tagged release". Added 2026-10-07 as a task for the Mobile special agent; the same day it became a plain reminder for Kaif, who does the App Store releases, and the instructions written for the agent were dropped. |
| `.forkmesh/shortcuts/task-mobile-play-store-deploy.sh` | Creates a reminder task, "Reminder: deploy the Android app to the Play Store", in `forkmesh/forkmesh`, assigned to Kaif (`mdkaifansari04`), the same way as the App Store reminder above. The details give the Android `versionName` and `versionCode` defaults from `native/android/app/build.gradle.kts`, the newest `v*` tag and `main`'s head, and the steps: sign in to Play Console with the ForkMesh Google account already created, build `:app:bundleRelease` as `com.forkmesh.forkmesh` with the upload key, keep Play App Signing (or request an upload key reset if Play holds an older upload key), fill in the listing, Data safety, content rating and privacy policy, and go through Internal testing to Production. Added 2026-10-09 at the user's request. |

## Architecture

- **One design, two apps.** Where the Kotlin app already solved a problem
  (pure-language crypto, the Git engine, vectors), the Swift app follows it.
- **Logic in the core, views in the app.** iOS: `ForkMeshCore` has no
  Apple-only imports outside `#if canImport`, so Linux compiles and tests all
  logic; views bind to `@Observable` view models. Android: `core` holds the
  logic, services hold `StateFlow`s, feature areas register through
  `*Feature` objects.
- **First-party dependencies only.** OS frameworks or code in the tree
  (tables in each README). The one exception is
  on-device models on iOS: MLX (`mlx-swift`, `mlx-swift-lm`) and
  `swift-transformers` run Gemma 4 on the phone, fenced into the app target
  (`ForkMesh/Features/LocalAI/MLXGemmaEngine.swift`).
- **Workstreams** (projects, push, composer, agent Watch, watch app)
  plug into `native/ios/ForkMesh/App/AppServices+<Area>.swift` installers so
  they never edit `AppServices.swift`.
- **Projects.** A project is a name plus everything the phone holds for it
  (session, node key, owner key, vault copy, local repositories, agent
  records, settings), usually attached to one ForkMesh instance whose origin
  is its id. Production owns the unprefixed legacy namespace; every other
  project uses `p/<projectId>/`. A project made from a name alone has
  `hasInstance` false, a `local-<slug>-<random>` id and no server URL: it is
  linked to no instance and works on the device alone; "Choose an instance"
  is offered, never required (see 2026-10-13 below). A project row may name a
  `logo` file.
- **Relay features.** `/api/version` declares a `features` list; the core
  (`accounts`, `repositories`, `repoRead`) is required, every other feature
  gates its own UI, and a missing one leaves the UI absent rather than broken.
  A 404 under `/api/repo/<owner>/<name>/...` never marks a feature missing;
  only relay-wide routes do.
- **No polling for display.** Screens reload when the account event socket,
  the relay or the local network says something changed.

## Protocols and interfaces

- Relay REST (`/api/...`) and WebSockets: the relay room socket and the
  account event socket (`/api/nodes/events`). The phone as a node:
  [`www/docs/mobile/mobile-node-contract.md`](../../www/docs/mobile/mobile-node-contract.md);
  agents from the phone:
  [`www/docs/mobile/mobile-agent-access.md`](../../www/docs/mobile/mobile-agent-access.md).
- Push: `app/src/apns.py` pushes the `pings`, `direct-messages`, chat and
  `code-push` topics to iOS; every payload carries `origin` so a tap routes to
  the right project. Android has no push transport (no FCM, by design): a
  foreground service keeps the account event socket open
  ([ANDROID.md](ANDROID.md) section 6). A desktop's own pings reach its
  linked phone the same way, filed as `desktop_ping` rows
  ([DESKTOP_PINGS.md](DESKTOP_PINGS.md)).
- Desktop control: on the same Wi-Fi the phone finds the desktop through the
  `#mesh` beacon and drives it over the mesh TCP port; away from home the same
  protocol is forwarded through the relay. Both transports use the signed
  handshake and per-direction encryption. On current `main`, an authenticated
  phone gets the enabled desktop keys for its own account from
  `GET /api/accounts/desktops`, so same-account control needs no LAN meeting.
  The signed v1.2.5 phone build instead has to learn the desktop on LAN once.
  A phone outside the desktop's account still needs a six-digit first-pairing
  code approved on that LAN.
  The frames are the twin of `desktop/src/LocalControl.{h,cpp}`; the test
  suites pin the same vectors. Transcripts stream (`agents.follow`), the relay
  link is opened while the app is in the foreground, and iOS asks the relay for
  HTTP/3; [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md) explains where QUIC is and is
  not used.
- Cryptography is pinned by shared vectors across Kotlin, Swift and the
  desktop: [`native/ios/docs/FMCrypto.md`](../../native/ios/docs/FMCrypto.md).

## Configuration

- iOS: `native/ios/Config/Version.xcconfig` holds `MARKETING_VERSION`,
  `CURRENT_PROJECT_VERSION`, and the build's `FORKMESH_ENV`,
  `FORKMESH_API_HOST`, `FORKMESH_WORLD_URL`.
- Android: `native/android/app/build.gradle.kts`: `-Pforkmesh.appId`,
  `-Pforkmesh.versionCode`, `-Pforkmesh.versionName`; release signing from
  `FORKMESH_ANDROID_KEYSTORE`, `FORKMESH_ANDROID_KEYSTORE_PASSWORD`,
  `FORKMESH_ANDROID_KEY_ALIAS`, `FORKMESH_ANDROID_KEY_PASSWORD` (without them a
  `com.forkmesh.nativeapp` release build uses the debug key).

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
an unsigned build and then the core tests on macOS; `ios-native-release` for
TestFlight; and `ios-native-adhoc` in `/codemagic.yaml`), and on the office Mac,
which runs `ios-native-adhoc` for every push to `native/ios` and emails a
tap-to-install link ([MAC_BUILD.md](MAC_BUILD.md)). Details in
[`native/ios/README.md`](../../native/ios/README.md) and
[`native/android/README.md`](../../native/android/README.md).

## Operations

- **iOS release.** `ios-native-release` in `/codemagic.yaml` archives,
  uploads and puts the build in TestFlight internal testing; the steps, how
  the widgets and the watch share data and the open release items are in
  [IOS_RELEASE.md](IOS_RELEASE.md).
- **Android APK.** `tools/forkmesh-release-host.py` builds
  `forkmesh-android-universal.apk` from `native/android` on the release host
  as `com.forkmesh.forkmesh`, signed with the v3 direct-distribution key
  (`android-direct-distribution-v3.jks` under the home project's
  `data/release/`). The v2 keystore was lost on 2026-09-22; v3 signs from
  v1.2.3, so older sideloads must reinstall once. See
  [`docs/release/README.md`](../release/README.md).
- **Migration from the Flutter app.** Users upgrading from the store Flutter
  build keep their data: on the first launch each app copies the Flutter
  app's Keychain/Keystore secrets and `flutter.*` preferences into its own
  stores under the same key names, reads every value back, and deletes the
  Flutter copies only after the relay accepts the migrated session. iOS:
  `FMAccount/FlutterMigration.swift` ([IOS_RELEASE.md](IOS_RELEASE.md),
  "Flutter-to-native migration"); Android: `C/migration` and
  `FlutterDataMigration.kt` ([ANDROID.md](ANDROID.md) section 4).
- `native/ios/tools/asc_bundle_ids.py ensure` registers the bundle ids with
  push enabled in the Apple developer account (reads `ASC_KEY_ID`,
  `ASC_ISSUER_ID` and the key file under `~/.appstoreconnect/private_keys/`).

## Security

- Keys live in the Keychain (`native/ios/ForkMesh/Platform/KeychainStore.swift`)
  and the Android Keystore (`app/platform/SecureStore`).
- Ed25519 is never delegated to CryptoKit (randomised signatures would break
  the pinned deterministic vectors).

## Traps

- An unsigned iOS archive loses `aps-environment` at export unless the
  release entitlements are embedded first; `codemagic.yaml` refuses a build
  that cannot receive push.
- Core tests pass on Linux but the first macOS run found failures Linux never
  shows; `ios-native-build` runs them last so they cannot hide whether the
  app compiles.
- On Linux, a synchronous `@MainActor func test…()` in a class that is not
  `@MainActor` aborts the whole test bundle at discovery ("Could not cast
  value of type … -> @Swift.MainActor () -> ()"). Declare such tests `async`.
- Before a device keeps Gemma 4 loaded, the increased-memory-limit
  entitlement must be enabled in the developer portal first, then added to
  the entitlements, or the export finds no matching profile.
- Never start an endless animation with
  `withAnimation(….repeatForever(…))` in `onAppear`. A view that appears
  during a push (or while its screen's bars settle) has its layout change
  caught by that transaction, and the change replays forever: the page
  slowly slides down and back. The loaders in `Kit/` (`FmSkeleton`,
  `FmCircularProgressIndicator`, `FmLinearProgressIndicator`) draw their
  motion from a `TimelineView(.animation)` clock instead, which holds no
  animation state at all.

## Recent changes

- 2026-10-14 (branch 1992, iOS): a project with no instance works without
  one. Asked: "allow for creating new repositories on the device without
  having an instance connected, and we can remove the On this device only
  choose instance warning, and put that small in the project list next to
  the project". Changes:
  - The shell no longer shows the "On this device only" banner
    (`NoInstanceBanner` and `AttachInstanceModel.bannerText` are gone). A
    project with no instance shows no connection banner at all.
  - In the project drawer (`ProjectSwitcher`), each project with no instance
    has a small **Choose instance** button beside its row. It opens
    `ChooseInstanceView` for that project, active or not.
  - Changes that stay on the phone use `AccountFeature.requireLocalAccess`
    instead of `requireAccount`. These are creating, importing and mirroring
    a repository; editing and deleting files; staging and committing;
    discarding changes; creating branches; and deleting a repository. With
    no instance, they go ahead without sign-in. With an instance, guests
    still sign in first. Publish, push and sync still need an instance, so
    they open the chooser.
  - The Repos tab neither loads nor shows the Network catalog (or its
    header) while the project has no instance. The catalog loads once one
    is attached.
  - The SwiftUI files are only parse-checked; they have not been run on a
    device. Android still shows its `NoInstanceBanner`.

- 2026-10-13 (iOS performance round, branch `1996-ios-performance`; asked:
  "the iPhone app is feeling a bit laggy, let's optimize it").
  - Evidence. No phone performance data reaches the server: performance
    reports go into an agent prompt ("Send diagnostics"), and `bug_reports`
    (read-only query through the operator rqlite helper) holds only prose
    reports, the newest iOS one from 2026-09-18 (Flutter 1.2.0). The latest
    real reports are the 2026-10-02 ones below (1.4.0 (19), Agents tab: 219
    stalls over 200 ms in 2 h, frames to 497 ms, loop blocks to 371 ms).
    Profiling on the office Mac: `xctrace` cannot record there (Developer
    mode is off, so it waits for an admin prompt), so the round used
    `sample` on the simulator app plus its CPU time per phase of a scripted
    UI tour (guest mode: idle on Home, switching tabs, Market). A Debug
    build idling on Home kept the main thread busy; most of that
    is the Genie spider's 24 fps `TimelineView` (`GeniePainter`, its
    accessibility block, the screen scan every 1.5 to 3 s).
  - Relay roster: `RelayService.rememberPeer` wrote the observed `peers` on
    every inbound frame (last-seen time), so `HomeShell` (via `onlineCount`)
    and the chat transcript (via `liveStatus`) redrew once per relay frame.
    Readers now observe `rosterRevision`, bumped only when a peer joins,
    leaves, changes, returns from stale, or goes stale at the presence tick.
  - Shell counts: `HomeShell.body` read the relay roster, the relay log
    (the Activity count), chat unread, downloads, teammates and every hub
    count, so any of them re-ran the whole shell body (both bar groups, the
    bottom bar, the Home grid). `ShellLiveCounts` gathers them through
    `ThrottledSnapshot` (FMBase): re-read at most 4 times a second, readers
    notified only when a value changed.
  - Hidden tabs: tabs stay mounted at opacity 0 (and all of them under the
    Home page), so their frame-rate animations kept running. The tab sets
    `shellTabHidden`; Agents dots and Eagle tickers, the sparkle
    new-agent button (and its 30 Hz accelerometer), skeletons and spinners
    pause on it. The Eagle ticker is capped at 60 fps, skeletons at 30.
  - Eagle rows: each run's latest line is its own observed `AgentEagleText`
    read by `AgentEagleLine`, so a streamed line redraws one row, not the
    Agents list.
  - Home grid: `ShellHomeLayout` is built once per body; each tile used to
    re-run the arrangement, re-lay the bottom bar and rescan heat
    (quadratic per redraw).
  - Chat: a typing frame no longer bumps `ChatExtras.revision` (it rebuilt
    every `ChatRow`); pictures, backgrounds and avatars go through
    `FmImageCache` (decoded once, display-ready, 48 MB cap) instead of
    `UIImage(data:)` in `body`; status times reuse their `DateFormatter`s.
    Main's own avatar cache from the same day (`FmAvatarImages`, a
    256-entry `NSCache` for first-frame avatars) now reads through
    `FmImageCache`, so the app keeps one image cache.
  - Market: `MarketModel.visible` groups nested packages in one pass
    (160 packages, Linux release: 7.9 ms to 0.5 ms per evaluation).
  - Measured: before-and-after numbers for the tour were not recorded; the
    round stopped before the after run.
  - Not changed: the spider's 24 fps painting and its screen scans (the
    largest idle cost; slowing them changes how it moves), the always-on 60
    Hz `TelemetryFrameProbe` (telemetry files are another branch's), JSON
    parse and decrypt of relay and control frames on the main actor,
    `PerformanceMonitor.track` symbolicating a stack per request,
    `MessagesConversationView` still reading typing at its top, Repos'
    offset ids, Android.
  - Tests (Linux toolchain): `testRepeatFramesFromAKnownPeerDoNotRedrawTheOnlineCount`,
    `ThrottledSnapshotTests`, `testEagleFeedRedrawsOnlyTheRowWhoseLineChanged`,
    `testTypingDoesNotRebuildTheTranscript`; FMPackages and UserStatus
    suites pass. The app compiles for the simulator on the office Mac.
  - How to measure again: build for the simulator in a scratch folder on the
    Mac (MAC_BUILD.md), launch with `simctl`, drive a tour with an XCUITest
    that `activate()`s `com.forkmesh.forkmesh`, record `ps -o time` of the
    app each second and `sample <pid> <s>` per phase. Without a signed-in
    account the relay never connects, so the relay fixes are covered by the
    unit tests, not the tour.

- 2026-10-13 (branch 1974, secure chat, round 2): the iOS chat list shows a
  second, muted line per channel and direct message with the newest
  top-level message the phone holds ("You: ..." for your own, "<sender>: ..."
  in channels, "📎 <file>" for a file), a short time on the name line, and
  the other person's profile picture with the presence dot on direct-message
  rows. Asked: "Make sure threads still work too also in the chat list show
  the last line per channel and per message with users also show their user
  icon in the chat list". Thread replies in direct messages and office
  channels were checked with new tests; no regression found. Details:
  [native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md), "Chat list
  last line, avatars and threads". The SwiftUI row is parse-checked only.
- 2026-10-13 (branch 1974, secure chat): iOS draws Signal-style receipt marks
  on every own message (sending, sent, delivered, read; channels stop at
  sent), lets you delete for everyone what you sent, and react to, edit and
  reply in a thread on messages in direct messages and office rooms. Chat
  pushes now show the message: the extension fetches the still-encrypted
  frame (`/pending?ref=`), decrypts it with the device key and shows sender
  and text, as Signal does; Settings, Notifications has "Notification
  content" (Name and message, Name only, No name or message). Asked: "add
  these symbols for each chat message", "make it so that you can delete for
  everyone something you sent", "make it so that chat text shows in the push
  notifications - signal has a way of doing it securely so lets do it like
  they do", "also have it so you can reply and react to chats from others in
  p2p". The new FMPush module holds the testable push logic and is linked
  into the extension as `ForkMeshPushCore`. Details:
  [native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md), "Receipt
  marks, deletes, reactions and replies in direct messages" and "Message text
  in push notifications". Not verified on a device: the extension's keychain
  read of the device key, the PBKDF2 time inside the extension, and the
  SwiftUI views (parse-checked only).
- 2026-10-13: a project does not need an instance. Asked: "Don't make
  instance a requirement projects can manage repos on the device for example
  without an instance". The full-screen **Choose an instance** gate is gone: a
  project with no instance opens the shell (no sign-in or guest step), so its
  on-device repositories and other local features work. The shell shows an
  "On this device only" banner (`NoInstanceBanner`, removed on iOS 2026-10-14; in place of the offline
  banner, since there is no relay to reach) whose **Choose instance** opens the
  chooser as a sheet (iOS, from `SignInPresenter`) or a full-screen dialog
  (Android, `ChooseInstanceHost` in `AccountFeature.Gate`); its **Not now**
  closes it. An action that needs an account (`requireAccount`, `openSignIn`)
  opens the chooser instead of sign-in and does not go ahead.
- 2026-10-13: a new project is not linked to any instance unless you name
  one. Before, a project made from a name alone stored the build's default
  server URL, so it signed in on app.forkmesh.com and listed that instance's
  repositories. Now `RelayProfile.named` / `Project.named` store an empty
  `serverUrl`, `fromJSON` / `fromJson` drop any stored URL from a row with
  `hasInstance: false` (rows saved by the old code included), and `httpOrigin`
  is empty for such a project. The cross-project repository catalog and the
  Android background link skip it, the switch does not open a relay socket or
  wait for one, and Android hides "Test connection" for it. While the active
  project has no instance the app root shows **Choose an instance**
  (`ChooseInstanceView` on iOS, `ChooseInstance` on Android) instead of
  sign-in, guest mode or the shell: the host is checked against
  `/api/version` and "Connect" calls `RelayDirectory.attach` /
  `ProjectDirectory.attach`, which keeps the project's id and namespace and
  sets its URL, features and `hasInstance`; the switch controller (iOS
  `RelaySwitchController.attach`) or `ProjectsService.attachInstance`
  (Android) then brings the account and relay up on it. The desktop already
  worked this way (`project/instancePending`, docs/desktop/README.md).
- 2026-10-13: a CRM list opens with two sections, **Reach out** (the best
  never-contacted lead) and **Follow up** (an unanswered reply, else the
  longest quiet past 14 days), as on the desktop and the web. Asked: "show two
  people up top one as the new best qualfiyign lead to reach out to and one as
  the person who we should follow up with". Each names the person, says why,
  and has **Email** (or Mastodon / X), **Contacted** and **Skip**. The rule
  and the lead ranking are in the CRM package README, "Reach out and follow
  up". Code and tests: `CrmNextContact.pick` (iOS `FMPackages`, Android
  `core/market`), `CrmModel.nextContact(_:)` / `nextContact(mode)`, the
  `CrmNextContactSection` / `CrmNextContactCard` views;
  `CrmParityTests.swift`, `CrmParityTest.kt`.
- 2026-10-10: Chat unread counts follow you across devices on iOS and
  Android (issue 1876). `ChatReadMarks` posts the newest message you have seen
  in a conversation and clears unread when another device's mark covers it.
  It fetches marks on the silent `chat-read` account event, on reconnect, on
  resume and on sign-in. See
  [../app/chat-read-sync.md](../app/chat-read-sync.md).
- 2026-10-10: invite someone to the active project from the phone, as on the
  desktop (`docs/desktop/README.md`, "Inviting someone to a project"). The
  project drawer has "Invite people to <project>" (iOS `ProjectSwitcher`,
  Android `ProjectDrawerContent`), which opens `ProjectInviteSheet`
  (`Kit/ProjectInviteSheet.swift`) or `InviteToProjectDialog`
  (`feature/account/projects/ProjectInviteUi.kt`).
  - Someone with an account on the instance: `DirectMessageService.start` then
    `send` with a message naming the project and the instance to add.
    `user_not_found` points them to the email invitation. An email address
    typed here moves to the email field.
  - Anyone else: "Open email" opens the mail app with a `mailto:` link holding
    the referral link `https://<instance>/r/<your account>` (`/signup` when
    not signed in). With no mail app the text is copied. A project with no
    instance can only send the email, without a link.
  - The text and links are built in `ProjectInvite` (FMAccount,
    `account/projects/ProjectInvite.kt`), mirroring `desktop/src/ProjectInvite.cpp`.
    Tests: `ProjectInviteTests` (iOS), `ProjectInviteTest` (Android core).

- 2026-10-09: the iOS Inbox (Pings) list is compact and flat. Rows no longer
  sit on an inset-grouped card: the list is plain, each row is divided from
  the next by a thin line across the whole screen, and the disclosure chevron
  is gone (the row still opens `InboxItemScreen`). Each ping's icon is drawn
  bare in its kind's colour, read or unread, from `InboxItem.tone` /
  `notificationToneForKind` (`FMApi`), which follows the desktop Pings grid's
  `pingIconFor`: errors red, hosts and finished agents green, issues,
  approvals and offline hosts amber, mentions, commits and calendar blue,
  pulls, tasks, releases and chat purple. Before, every read row's icon was
  grey and every unread one the accent colour. Titles are one size smaller
  and the body is one line. Test: `InboxFeedTests`.
- 2026-10-09: second App Review 3.1.1 rejection (1.4.0 (19)). The iOS
  sponsored card loses "Why this ad?" (its page lists Patreon prices),
  Notifications no longer lists "Credits refilled" or "Tips", the nodes link
  opens the web Nodes tab, and `asc_appstore.py submit` writes App Review
  notes saying the app sells nothing
  ([IOS_RELEASE.md](IOS_RELEASE.md#app-review-nothing-paid-on-ios)). Tests:
  `PingSettingsTests.swift`, `tools/test_ios_app_store_release.py`.

- 2026-10-09: direct messages also go over the local mesh to the other
  person's linked phone or desktop, sealed to that device's key only, so two
  people in the same place keep messaging with the relay down
  ([../app/chat-private-rooms.md](../app/chat-private-rooms.md#direct-messages-over-the-local-mesh-2026-10-09)).
  `LocalMeshRoom.sendDirect` / `LocalMeshService.sendDirect` and
  `DirectMessageService.acceptMeshDirect` on both phones. Tests:
  `MeshDirectMessagesTest.kt`, `MeshDirectMessagesTests.swift`.

- 2026-10-09: someone with no organization can now install CRM from a phone.
  Asked: "make it so that someone can install the CRM plug-in from mobile".
  The owner/admin install path already existed, but a new account hit a dead
  end. The CRM screen said "Join or create an organization" and only linked
  to the Market, and the Market's Organizations section said "You are not in
  an organization yet." with nothing to tap. Both now open the Organizations
  screen to create one. On iOS that is `PackageNotInstalledView`, so it also
  covers Analytics, Competitor Watch and Marketing, plus the Market package
  page. On Android it is the new `CrmNoOrganizationPanel` (shown when
  `CrmModel.needsOrganization`: a successful `GET /api/crm` returned no
  organizations), plus the Market package page. On return the screen reloads
  (iOS `.task`; Android re-checks when it is back on top of the navigator).
  After that, the creator is the owner, and the usual **Install CRM** button
  posts `POST /api/plugins/<org>/crm/install`.

- 2026-10-08: the Marketing Content section opens with **Ready for X**, the
  post Herald keeps ready (`GET`/`PATCH /api/marketing/<org>/ready-post`,
  [../marketing/x-ready-post.md](../marketing/x-ready-post.md)). Asked: "make
  it one click post from mobile". **Post to X** hands the image and the text
  to the system share sheet (iOS `AgentActivityView` with a completion
  callback that marks the post posted when an app that posts finishes;
  Android `ACTION_SEND` straight to `com.twitter.android` when installed,
  after copying the text). Copy, Mark posted and Skip sit beside it. Parsing
  is in `MarketingWorkspace` on both phones. Tests:
  `MarketingWorkspaceTests` (iOS) and `MarketingWorkspaceTest` (Android).

- 2026-10-07: opening the Chat tab no longer wipes the channel list's unread
  dots. Asked: "I have two push notifications for new messages but nothing
  indicated as new on the mobile channel list in chat." Both phones called
  the relay's `markAllRead` from the shell's tab select (and a tapped chat
  push selects the tab), so every conversation was marked read before the
  list was seen. The relay now keeps a separate badge count: the tab calls
  `markBadgeSeen` (iOS `HomeShellModel.select`, Android `HomeShell.select`),
  which clears only the tab badge (`badgeUnread`); each conversation keeps
  its dot until it is opened. Office channels, which were never marked
  (Android drew them with `unread = false`), now count live messages through
  `noteUnread`; an Office message that arrived while its room's socket was
  closed is still not counted. Tests: `testSeeingTheBadgeKeepsEachConversationUnread`
  in `RelayServiceTests.swift` and `ChatScreenLogicTest.kt`.
- 2026-10-07: the iOS Agents tab no longer shows a yellow
  "CancellationError()" line (screenshot: "Fix this cancellation error").
  `AgentControlCenter.loadDesktop` and `loadCloud` run inside the caller's
  task (pull to refresh, the screen's `.task`), so leaving the tab or letting
  go of the pull cancelled the `agents.list` or account agent-sessions read,
  and the catch wrote `String(describing: error)` into `desktopNote` /
  `cloudNote`, which `AgentsScreen.controlNotes` and `AllAgentsScreen` show.
  `ControlErrorText.isCancellation` (`CancellationError` or
  `HTTPError.cancelled`) now makes a cancelled read keep the rows it had and
  leave the notes alone, and `DesktopControlHub.link` records a cancelled dial
  as `.offline("")` rather than the error text. Tests:
  `AgentControlTests.testACancelledCloudReadShowsNoNote`,
  `testACancelledDesktopDialShowsNoNote`.

- 2026-10-07: Marketing on both phones manages everything the desktop
  Marketing package does, and the CRM closes the gaps left after 2026-10-02.
  Asked: "update mobile to have parity with the desktop marketing plugin and
  crm so we can manage all aspects of marketing on mobile too."
  - **Marketing sections:** Overview, Content, Campaigns, Channels, Ads,
    Growth, Reddit and Performance (`MarketingSection`; the desktop's and
    web's `dashboard`, `content`, `ads-manager` and `reddit-desk` route
    names map onto them, and an unknown section opens Overview instead of
    the plan).
    - Overview has the counts, the server's next-step suggestions (each opens
      the matching form), the AI assistant (suggestions become a content
      draft, a campaign task, a keyword or a campaign), the weekly report
      (shared as Markdown), the tagged-link builder and Post to X.
    - Content creates, edits, deletes and moves posts, filters by status and
      channel, and posts X content through the connected account.
    - Campaigns create from scratch or from the server's templates, edit,
      tick and add tasks, copy the tagged link, plan content for the
      campaign and ask the assistant about it.
    - Channels shows health with its checks and the playbook, refreshes
      server-readable channels one at a time or all stale ones, logs numbers
      by hand, adds channels from the 100-channel catalog, edits a channel's
      link, name and write-only X or GitHub token, and tracks posted links.
    - Ads keeps per-currency totals, creates and edits ads, logs results,
      pauses and resumes, and opens the platform's ads manager.
    - Growth logs funnel weeks against targets, runs ICE-scored experiments,
      tracks keywords with readings and imports Search Console or Bing
      exports (the desktop's parser).
    - Reddit shows the desk, the watch list with each subreddit's rules, and
      the thread queue with the reply checks the server enforces.
    - Performance reads Analytics over 7, 28 or 90 days with goals.
    - Owners and admins also edit link tracking, funnel targets, conversion
      goals, the Reddit watch list and the X connection.
  - **One form for every record:** `MarketingForms` describes each record's
    fields once (text, choice, channel, date, money, count and so on) and
    builds the exact body the worker expects. Examples: the full channel
    list for a settings `PUT`, so no channel is dropped; `publishedAt` when
    content becomes published; a keyword reading only when one is typed; no
    `currency` key unless one is typed, because the worker refuses an empty
    one. Both apps render that description with a generic form
    (`MarketingFormSheet` on iOS, `MarketingFormScreen` on Android), so a new
    worker field is one line in `fields(_:)`.
  - **Left to the browser:** only the browser extension's page capture.
    "Update with the browser extension" opens the web page, as before.
  - **CRM additions:**
    - Unsent drafts: a contact-note paragraph starting "DRAFT, not sent.",
      parsed as the desktop's `noteDrafts` does. An email draft opens the
      compose form, and sending it rewrites the mark to "DRAFT, sent
      <date>."; a Mastodon reply copies the text and opens the thread.
    - The contact editor sets or clears the last-contacted date.
    - Copy link on a contact.
    - Deleted built-in columns stay out of the column editor and can be
      restored (`removedColumns`).
    - The summary request sends the device's UTC offset as `tz`, so the team
      chart's days match the desktop.
    - Owners can connect Zoho Mail with a Self Client code
      (`POST /zoho/exchange`) when one-click connect is off.
  - **Tests:**
    - iOS: `MarketingWorkspaceTests` and `CrmParityTests`.
    - Android: `MarketingWorkspaceTest` and `CrmParityTest`.
    - `:app:compileDebugKotlin` passes. The SwiftUI files are parse-checked
      only.
- 2026-10-06: the Sounds switch is easier to find from the Settings search on
  both phones. Asked: "Make sure a user can turn sounds off on mobile through
  the Settings page; make settings searchable on mobile if it's not already."
  Both apps already had the switch (Settings, General, Sounds) and a settings
  search; "mute", "silent", "audio", "volume" and similar now match it, the
  `sounds` section alias opens General, and the Android search results name
  the matching setting (NAVIGATION.md §6.5).

- 2026-10-05: the channel transcript on iOS and Android shows a day
  separator before the first message of each local day, like Slack and like
  the web chat (`app/public/chat-date-groups.js`) and the Qt desktop: a line
  with a centred pill reading "Today", "Yesterday", or "Monday, October 5"
  (with the year when it is not the current one). A sender's run of messages
  no longer groups across midnight, so every day starts with an avatar and
  name. The logic is `ChatHub.startsNewDay` / `ChatHub.dayDividerLabel`
  (`FMChat`) and `startsNewDay` / `dayDividerLabel` in
  `ChatScreenLogic.kt`; the views are `ChatDayDivider` in `ChatScreen.swift`
  and `ChatMessageParts.kt`. Labels are English like the rest of the
  transcript's time labels, and are computed when a row is drawn, so a
  transcript left open past midnight relabels as rows recompose. Thread
  views (`ChatThreadScreen` on both platforms) show the same separator
  before the first reply of each day, comparing the first reply with the
  thread's root message. The iOS iMessage-style conversations keep their own
  centred time lines, but now always put one at the first entry of a new
  local day, even when it is less than 15 minutes after the last one.
- 2026-10-04: Apple rejected 1.4.0 (18) under guideline 3.1.1 (paid content
  without In-App Purchase) and said it had raised it before, so the iOS app
  lost what was left of paid content and payments: the Ads
  screen's owner side (it showed the membership tier, and ad slots are what a
  membership buys) is now the review queue for ad reviewers only; the Market
  no longer labels packages "Free" or "Paid"; "Ledger & tips" is "Ledger"
  without Send, Treasury or the payout-wallet balance (`WalletClient` and the
  `wallet.` control family are gone); and the unused Patreon status code
  (`MembershipStatus`, `GET /api/accounts/patreon`) is deleted. The rule and
  what was left in place on purpose are in `IOS_RELEASE.md`, "App Review:
  nothing paid on iOS". Android and the desktop are unchanged.
- 2026-10-04 (phone clone of forkmesh/forkmesh failed with plenty of free
  space). Cause: at depth 25 the flagship had grown to a 158 MB pack,
  1.3 GB inflated, with a 77.5 MB object, over the 96 MB clone guard and the
  engine's 768 MiB inflated and 64 MiB object defaults. Both apps now clone at
  depth 1 (72.5 MB, 159 MB inflated) and open every workspace repository
  with `GitWorkspace.deviceLimits`, the engine hard maxima (1 GiB pack and
  inflated, 256 MiB object, 60 s idle read, 20 min deadline). The clone
  dialog no longer quotes a size limit. Details in
  `native/ios/docs/FMCodeLocal.md` and `ANDROID.md`.
- 2026-10-02 (performance report from 1.4.0 (19), Agents tab root, 2h2m
  up: 70 build-time frame stalls of 120 to 500 ms in the hour, about one
  a minute, a 283 ms main-thread block alongside a 473 ms frame, 6
  `/api/account/agent-sessions` reads up to 2.1 s in five minutes, and the
  same 04:17 SIGABRT and 01:34 SIGSEGV).
  - Cause: the 2026-10-02 fix moved JSON parsing of large replies off the
    main actor only for `getJson`, `postJson` and `deleteJson`. The agent
    queue reads go through `agentJson`, which still parsed on the main
    actor. Those reads are `/api/account/agent-sessions` (on every
    `cloud-agents` ring) and each repository's `agent-sessions` (on every
    `agents` ring). Their replies carry each run's history (up to 80 turns
    per run on the per-repository list, 8 on the account list) and 16 K of
    live text per running run, so a reply can run to megabytes. It was
    parsed, and every `AgentLaunchSession` built from it, on the main
    actor once or twice a minute while agents run.
  - `agentJson`, `accountApiCall` (calories, notes, calendar, steps and
    sleep sync: the reads the report lists as in flight during stalls),
    `orgRequest`, the runner diff page and the merge preview now parse a
    reply of 16 KB or more on a detached task (`ApiParse.parseAsync`).
    `agentLaunchOptions` and `accountAgentSessions` also build their
    models there (`ApiParse.modelAsync`).
  - Not changed: the crashes (no new signature; both predate the fixes
    below); the desktop control link over the relay still decrypts and
    parses each frame (up to 200 `agents.list` rows) on the main actor;
    Android.
  - Test: `testLargeAgentAndAccountRepliesDecodeOffTheMainActorTheSame` in
    `ApiAgentsTests` (Linux Swift toolchain).

- 2026-10-02: tapping a project in the iOS project drawer opens that
  project's Home page. The active row used to do nothing; now it closes the
  drawer, the You sheet and any presented page and shows Home
  (`ProjectHome.request()`, observed by `HomeShell`). Picking another project
  sets `ProjectHome.requestOnArrival()` before the switch; the rebuilt
  `HomeShell.start()` takes it and lands on Home even on iPad or with a
  restored presented route, instead of the project's last tab and path.

- 2026-10-02: iOS interface sounds are on unless the user switches them off
  in Settings. A stored `device/sounds` value (left off by the 2026-09-24
  default and legacy migration) is deleted at launch and the switch now writes
  `device/soundsOff`; and a dictation or voice message no longer leaves every
  cue silent until relaunch (NAVIGATION.md §6.5).
- 2026-10-02: Speech Playground, a text-to-speech package, iOS first. Asked:
  "Also have speech to text very nice sounding one. Have it be a plug-in where
  it's like a playground where we can enter some text into a box and have it
  speak. Have it be native? Let's do this on iOS first please" (the
  description is text to speech, so that is what was built).
  - New package `marketplace/packages/speech` (manifest `ios: builtin:speech`,
    no Worker, data scope `none`); the phone maps it to the new `.speech`
    route, listed under You › Personal and opened from the Market.
  - "Very nice sounding": the page picks the phone's best voice for its
    language (Personal Voice, then Premium, then Enhanced) and keeps novelty
    and Eloquence voices out of the automatic pick; when only Standard voices
    are installed it says where to download a Premium one. Personal Voice is
    one tap on iOS 17+.
  - Native AVSpeechSynthesizer with the spoken word highlighted, speed and
    pitch sliders, voice previews, four sample texts, and Save audio (AAC
    `.m4a` to the share sheet). Logic and tests in FMTools
    (`SpeechModels.swift`, `SpeechTests.swift`); details in
    [FMTools.md](../../native/ios/docs/FMTools.md). Android has no Speech
    page yet.

- 2026-10-02: the Stats board fills tile by tile, and a slow desktop no longer
  reads as an unreachable one. Asked from the phone with a screenshot of
  every tile stuck on a grey placeholder while the desktop was on the LAN:
  "Please fix these to work - desktop is reachable and add spinners when they
  load."
  - Cause: `StatsBoardModel.reload` read network stats, online history,
    `/api/status`, `diag.health` and `stats.codemagic` one after another and
    published the tiles only at the end, so one slow desktop answer held back
    every tile, the server's included.
  - The five sources now load at once (`async let` on iOS, one thread each
    on Android) and each publishes its tiles as it answers. `pending` /
    `isLoading(id)` name the tiles still waiting. A waiting tile shows a
    spinner; on a refresh, the spinner sits in its title row next to the
    old value. A tile whose source failed says "No data · could not load"
    instead of keeping the placeholder.
  - LAN timeouts: `LanControlLink.request` (iOS) dropped the caller's timeout,
    so every LAN call waited the default 20 s. Quality (45 s) and Codemagic
    (25 s) were cut short and answered `timeout`. `LocalControlService.call`
    now takes the timeout. Android already passed it.
  - Wording: `DesktopStatsClient.explain` turned `timeout` into "Your desktop
    is not reachable right now." A timeout on an open channel means the
    desktop is slow, so it now reads "Your desktop took too long to answer."
    `not_paired`, `not_connected`, `offline` and `unreachable` keep the old
    sentence. `sentry_not_installed` and `sentry_not_connected` read as
    sentences instead of codes.
  - Spinners elsewhere: `InsightsLoadingRows` (Desktop log, Sentry, Quality,
    Status, Network, Admin) starts with a spinner and "Loading…" row.
  - Tests: `testServerTilesFillWhileTheDesktopIsStillAnswering` and
    `testADesktopTimeoutIsSlowNotUnreachable` (iOS `InsightsModelTests`),
    `testBoardServerTilesFillWhileTheDesktopIsStillAnswering` (Android
    `InsightsTest`).

- 2026-10-02: the iOS Actions page is split into Runs and Workflows (a
  segmented switch under the "Live from <desktop>" line). Runs are grouped
  into In progress and Recent, with a workflow menu instead of tapping a
  workflow row to filter. Workflows are grouped into Run by hand, Automatic
  and Turned off; each row shows readable triggers ("Push · Pull request")
  and its latest run. A row opens a workflow page with an On switch, its
  file, triggers, runs-on labels and recent runs; swiping a row or its
  context menu also turns it off or on. Turning one on asks for Face ID or
  the passcode, turning one off does not. The switch goes to the desktop
  as `workflow.enable` / `workflow.disable` through `repos.prepare` and
  `repos.run` (NAVIGATION.md §3.4), or to a node as
  `set-workflow-enabled`. An older desktop answers `unsupported_action`,
  shown as "Update the desktop to do that from here." Android still hides
  the switch. Test: `testWorkflowSwitchGoesThroughPrepareAndRun` in
  `DesktopActionsTests.swift`.
- 2026-10-02 (performance report from 1.4.0 (19), Agents tab root, 4m48s
  up: "frozen for a second" while scrolling the list; a 580 ms build frame
  and a 501 ms main-thread block together, 28 `tree` and 25 `blobs` reads
  of `forkmesh/forkmesh` in five minutes, the same 04:17 SIGABRT and 01:34
  SIGSEGV).
  - Freeze: the Agents list reads the pull ledger to learn whether a run's
    pull merged, and `publishedPullHeads` parsed every `pull.md` with the
    full `pullFromMarkdown` on the main actor. A `pull.md` carries the whole
    patch, and that parse split each file into lines three times and ran a
    regex over the body for the patch, all to read four front-matter keys.
    Heads now come from `pullHeadFromMarkdown` (front matter only, `body`
    and `patch` empty) on a detached task. `frontMatter` and `firstHeading`
    stop at the closing `---` or the first heading (`eachLine`), and
    `publishedPulls` parses each `pull.md` off the main actor too.
  - Reads: the queued run screen (`LaunchedAgentSessionModel.refresh`) and
    the widgets' open-pull count (`SurfacesCoordinator.loadWork`) read
    `publishedPulls`, which lists every pull's folder and reads its event
    files: about two requests per pull, the report's `tree`/`blobs` pairs.
    Both use only number, title, status and head, and now read
    `publishedPullHeads` (one listing and one batch per 50 pulls).
  - `dartTrimmed` copied the whole string into a scalar array and rebuilt
    it even with nothing to trim; the list calls it on every launched
    run's first prompt on each redraw. It now slices.
  - Not changed: the crashes (no new signature, no symbolicated stack);
    Android.
  - Tests: `PublishedRecordsTests` and `testDartTrim` (Linux Swift
    toolchain). The SwiftUI side needs an iOS build to check.
- 2026-10-02 (performance report from 1.4.0 (19), Agents tab root, 11m10s
  up: a 54,137 ms "frame", 10 slow `forkmesh/forkmesh` `tree` and 11 slow
  `blobs` reads up to 2.7 s, frame stalls of 120 to 260 ms, and the same
  04:17 SIGABRT and 01:34 SIGSEGV as the report below).
  - The 54 s frame was time away from the app. The main-thread heartbeat,
    which stops while the app is hidden, saw at most 370 ms in that hour.
    Build 19 predates the frame source's lifecycle reset (the 31 min report
    below). As a backstop the monitor now drops any frame gap that reaches
    back before the heartbeat last started, and every gap while it is
    stopped, so a display tick that races the lifecycle notifications
    cannot log a suspension as a stall either.
  - Pull ledger: the Agents list reads `publishedPullHeads` (one `tree` of
    `.forkmesh/pulls` plus a `blobs` read per 50 `pull.md` files) for any
    repository with a branched run. The API cache lasts 60 s, so nearly
    every ring re-read the whole `forkmesh/forkmesh` ledger and parsed it on
    the main actor. A ring-driven load now keeps the pulls it already has
    unless a branched run is new or changed its status, branch, pull
    number or merged flag, or the last read is `pullsRefreshInterval`
    (5 minutes) old and a run's pull is not yet merged or closed. The
    header button, pull-down and first load still read it.
  - JSON: `getJson` and the POST/DELETE path parse a 2xx reply of 16 KB or
    more on a detached task (`decodeResponseAsync`), so a large `tree`,
    `blobs` or `agents/list` reply no longer blocks the main actor while it
    is decoded. Smaller replies and every error path still go through
    `decodeResponse`.
  - Crashes: both are the ones the report below fixed after build 19; no
    new crash signature.
  - Not changed: the watch bridge keeps its own `AgentsListModel`, so with
    a paired watch each ring loads the list twice; Android.
  - Tests: `testARingKeepsThePullLedgerItReadUntilATaskMovesOrItGoesStale`
    in `AgentsListModelTests`, `testAFrameGapReachingBackIntoTheBackgroundIsNotAStall`
    and the updated `testFrameSourceAndLifecycleDriveTheMonitors` in
    `PerformanceMonitorTests` (Linux Swift toolchain).
- 2026-10-02 (performance report from 1.4.0 (19), Agents tab root, 6m51s
  up: 15 frame stalls of 120 to 250 ms in five minutes, all build time, 18
  `/api/account/agent-sessions` reads, per-repository `agent-sessions` and
  `agents/list` reads failing as "request cancelled", and a SIGABRT through
  `libc++abi` and `libobjc` 36 s after the prompt started listening as it
  opens).
  - Redraws: `AgentsScreen.lines` (merge every desktop session and relay run
    into `AgentLine`s, then sort) was a computed property read four times per
    body: the list, the running ids, the running count and the total. Body
    now computes it once and passes it down.
  - Unchanged reloads: `AgentsListModel.refresh` and
    `AgentControlCenter.reload` assigned `groups`, `rows`, `desktopSnapshot`,
    `desktopNote`, `cloudNote`, `loadError` and `hasLoaded` on every load.
    An `@Observable` property notifies on every write, equal or not, so each
    ring redrew the whole tab and re-ran `onChange(of: model.groups)`'s deep
    compare. They are now written only when they change, and
    `isPendingFirstLoad`/`isFailed` read `groups` first, so a tab with rows
    no longer depends on `isLoading` flipping at each load's start and end.
  - Cancelled reads: a ring during a ring-driven load cancelled the
    debounce task, and that task was the one running the load, so its
    repository reads failed as "request cancelled" and a new load started
    from scratch. A ring during a load now marks it, and one more load
    follows when the running one finishes. A cancelled load no longer sets
    `loadError`.
  - Crash: `PromptDictation` kept one `AVAudioEngine` for its life, and
    `finish()` read `engine.inputNode`, which builds the input node under
    whatever session was active then (the Watch's `.playback`). The next
    start installed a tap with that stale format, and AVAudioEngine throws
    an Objective-C exception when a tap's format does not match the hardware
    input. Each start now builds a fresh engine after the session is
    `playAndRecord`, takes the input's hardware format, and `finish()`
    touches only an engine it started. The Agents tab's Watch speaker
    (`PlatformAgentWatchOutput`) and motion sound (`WatchMotionSound`) no
    longer switch the session to `.playback` while dictation listens (as
    `FmSoundCues` already did), and a phrase is not played on an engine the
    session change stopped (`AVAudioPlayerNode.play` throws there too).
  - Not changed: the 01:34 SIGSEGV has no stack in the report; JSON is
    still parsed on the main actor; Android's Agents screen.
  - Tests: `testARingDuringALoadLetsItFinishAndReadsOnceMore` and
    `testAnUnchangedReloadDoesNotRedrawTheList` in `AgentsListModelTests`
    (Linux Swift toolchain). The SwiftUI and AVFoundation changes need an
    iOS build to check.
- 2026-10-02 (performance report from 1.4.0 (19), Agents tab, 31 min up,
  pasted cut at 4,000 characters). The report now goes into the shell prompt
  through "Send diagnostics" (the "+" menu and `/diagnostics`) instead of the
  hub's clipboard button, trims its lowest-ranked sections instead of losing
  its tail, and shows MetricKit crashes as the attributed thread's top frames.
  The frame source no longer counts time in the background as one late frame
  (that report's 85,667 ms "frame"). See NAVIGATION.md, hub Diagnostics.
- 2026-10-02 (performance report from 1.4.0 (19), Inbox tab, 38 s after
  launch: 11 stalls up to 1.1 s, 3 `register-device`, 3 `event-ticket`, 4
  `/api/notifications` reads, and Agents reads for every repository). Launch
  now asks for each thing once.
  - Tabs: `HomeShell` built all four tab roots at launch and hid three of
    them with opacity, so every tab's `.task` loads (the Agents fan-out per
    repository, Chat's app commands, Code's repositories) decoded on the
    main actor at the same moment. The selected tab is built first, Agents
    2 s later (it feeds the bar count and the Home dots), and Chat and Code
    on their first visit (`NAVIGATION.md`). The hidden Chat tab also called
    `hub.roomShown()` at launch, which marked #mesh read unseen.
  - Push: `PushNotificationService.publish` only remembered a registration
    after the POST answered, so the launch rebind, the resume rebind and
    APNs's token callback each sent one. The fingerprint being posted now
    counts as registered until the answer; a failure clears it, so the next
    rebind tries again.
  - Account events: a resume while `AccountEventChannel.open` was waiting
    for its ticket started a second open, which discarded the first ticket.
    A pending open for the current generation is now joined; `restart`
    still opens fresh.
  - Inbox: the screen loaded on `.task` and again on `scenePhase == .active`,
    which fires during a cold launch. Both now call `InboxModel.loadIfStale`,
    which joins a running load and skips one finished in the last 15 s;
    pull to refresh and the sync-queue discard still force a read.
  - Paired desktops: `DesktopControlHub.refreshAccountDesktops` calls that
    overlap (the warm dial and the Agents tab) share one read; `force` still
    reads.
  - Not changed: JSON is still parsed on the main actor (`ApiService` is
    `@MainActor`), relay frames are still decrypted there, and the MetricKit
    CPU exception (90 s of CPU in 149 s) and SIGABRT come without a
    symbolicated stack. Those are the next things to look at if stalls
    remain.
  - Tests: `PushNotificationServiceTests`, `AccountEventChannelTests`,
    `InboxFeedTests` and `ProjectSwitchResetTests`, run with the Linux Swift
    toolchain. The two overlap tests fail on the old code with exactly the
    report's 3 tickets and 3 registrations. The `HomeShell` change needs
    an iOS build to check.

- 2026-10-01 (performance report from 1.4.0 (154), Agents tab open): the
  Agents tab no longer reads the same things over and over.
  - Node runs: 80 full-session reads of each of two runs in five minutes. The
    tab's hidden watchers re-read every live node run on every ring. They now
    share one read per run through `AgentControlCenter.watchTranscript`, kept
    until the run's row changes or for 30 s (`DESKTOP_AGENTS.md`, Agent Watch).
  - Pull ledger: 61 `tree` and 45 `blobs` reads of forkmesh/forkmesh. The
    list read every pull's timeline to learn its status. It now uses
    `publishedPullHeads` (ledger listing plus one batch), and `blobs` reuses
    cached files (`native/ios/docs/FMApi.md`).
  - Refused repositories (`vm1/forkmesh`, `newnewnode`): the 10-minute
    refusal memory is shared by the Agents screen and the watch bridge
    (`AgentReadRefusals.shared`).
  - Not changed: the ~115 ms frame stalls, the 10 s `relay.open` and the two
    SIGABRT crashes. The report gives no stack for them, and fewer main-actor
    JSON decodes of full sessions should lower the stall count. Android's
    Agents screen also reads the full pull ledger (`AgentsScreen.kt`) and is
    not changed here.
  - Checked on Linux only (no Swift toolchain); the new tests are in
    `AgentControlTests`, `ApiServiceRepoTests` and `AgentsListModelTests`.

- 2026-10-01: the iOS Stats page is a compact board that looks like the
  desktop's. It also has trends and a Codemagic minutes tile.
  - Layout: `StatsGridLayout` (`Features/Insights/StatsScreen.swift`) packs the
    tiles into three columns, and a wide tile spans two. Each tile shows its
    title, value, note and a 16 pt sparkline in caption-sized type.
  - Status colours: a tile with a status has a green, orange or red wash,
    border and left edge, and a status symbol, as `StatsDashboard::paintTile`
    does on the desktop.
  - Trends: the chip is the desktop's (`StatsFormat.trendText`, ▲/▼, hidden
    under 1%). A move in the bad direction is red.
  - Trend history: the server only sends history for node activity. Every
    other tile now carries a numeric `reading`. `StatsHistoryStore` keeps the
    last 48 readings per tile under `insights/stats/history.v1`, scoped per
    project, at most one per 10 minutes, and `StatsTiles.withHistory` turns
    them into the tile's series. Trends therefore appear after a few visits
    spaced 10 minutes apart.
  - Codemagic minutes: the phone asks the paired desktop with
    `stats.codemagic`, because the token (`CODEMAGIC_API_TOKEN`) lives only in
    the desktop's Actions variables. The desktop calls
    `GET https://api.codemagic.io/user` and reads
    `buildTimes.monthlyFreeBuildTimeLimit` and `buildTimes.currentPeriod.free`,
    both in seconds. It falls back to `billing.usage` and caches the answer for
    5 minutes (`statsctl::codemagicUsage`).
  - Reset date: the free period resets on the 1st of the month (UTC). The
    count was back to that day's builds on 2026-10-01.
  - Tile colours: green above 30% of the minutes left, orange above 10%, red
    below that.
  - Errors: `codemagic_not_configured`, `codemagic_unauthorized` and
    `codemagic_unreachable` show on the tile as a sentence.
  - Android's board does not have these changes yet.

- 2026-10-01: opening a repository's Code page (or any page that loads
  behind a skeleton, spinner or progress bar) no longer makes the page creep
  down. The three loaders started a `repeatForever` animation in `onAppear`;
  they now animate from a `TimelineView` clock (see Traps).

- 2026-10-01: project switching is faster, the switcher slides in from the
  left, and a switch no longer shows the previous project's agents or
  plugins.
  - Drawer (iOS): `View.projectDrawer(isPresented:)` in
    `Kit/ProjectSwitcher.swift` replaces the switcher sheet everywhere it
    opened (`ShellProjectSlot`, `ProjectSwitcherButton`, the command palette's
    switch-project action). `ProjectDrawer` is a full-screen cover with no
    presentation animation and a clear background; inside it the switcher
    slides in from the leading edge at 75% of the width over a 45% black
    scrim. Tapping the scrim or the VoiceOver escape gesture closes it;
    swiping does not move it (NAVIGATION 2.0.96). Picking a project closes the drawer first (0.18 s), then starts
    the switch, because the switch re-creates the view tree that presents the
    drawer.
  - Faster: the overlay used to stay up until the new project's chat socket
    was connected (room-key fetch, PBKDF2 on a first visit, up to the 12 s
    join wait), plus push re-registration and an inbox re-read, with a 0.42 s
    floor. `RelaySwitchController.covering` is now true only for `leaving`,
    `clearing` and `adopting`, the local steps; the overlay follows it, and
    the floor (0.2 s) applies to that part. `joining` (`onResume`, then the
    socket) runs under the new project's screens. `ChatFeature`'s `onResume`
    no longer waits for `push.rebind` or `NotificationService.refresh`; the
    inbox read already drops an answer for an account no longer active.
  - A switch started while the previous one is still `joining` takes over
    instead of being refused: it waits for the previous `onResume` to finish
    (so suspend and resume never interleave), then runs its own phases; the
    previous call returns abandoned, and `RelayService`'s open generation
    discards its socket. A switch during the covered phases is still refused.
    `RelaySettingsModel.canSwitch` gates the switcher rows on this.
  - Other projects' agents and plugins: the agents list, the desktop link and
    the installed business packages lived in app-wide services that the
    switch never cleared, so the rebuilt Agents screen showed the previous
    project's rows (and counted them on the bottom bar) and the More grid its
    plugins. `ProjectRuntime.activate` now calls `forgetPreviousProject`:
    `LocalControlService.resetForProjectSwitch` (drops the LAN connection
    made with the old node key and re-reads the project's paired desktops,
    which were loaded once at launch; auto-connect runs on the next main-actor
    turn, after the new session is adopted),
    `DesktopControlHub.resetForProjectSwitch` (account desktops, last reached
    desktop, the relay channel, keep-warm retry),
    `AgentControlCenter.resetForProjectSwitch` (rows, notes, cloud sessions),
    `AgentSessionService.load()` (branch records are per project),
    `HubPackageState.reset()` and the bottom bar's agent count. Each of these
    carries an epoch, so a load started for the old project (account desktops,
    `agents.list`, the cloud session list, a relay or LAN dial, the installed
    packages read) cannot write its answer after the switch; a relay dial that
    finishes late is closed and redialled for the new project.
  - Tests: `RelaySwitchControllerTests` (cover lifts before the socket, a
    switch during `joining` takes over in order, one during the covered
    phases is refused), `ProjectSwitchResetTests` (FMControl),
    `LocalControlServiceTests.testASwitchToAnotherProjectDropsThisProjectsDesktop`.
  - Android drawer: `ProjectDrawer` / `ProjectDrawerHost` in
    `feature/account/projects/ProjectsUi.kt` (75% width, 250 ms slide, 50%
    scrim; scrim tap and Back close it, a swipe does not move it, NAVIGATION 2.0.96).
    `AccountFeature.Gate` draws it above the app and below the switch overlay,
    outside the part rebuilt on a switch, so it keeps animating closed while
    the switch runs. The top-bar `ProjectSwitcherTitle`, the sign-in
    `ProjectChip` and the command palette's Switch project (which used to open
    the Settings projects page) open it; inside the You sheet the title closes
    the sheet first. Manage projects is hidden on the sign-in screen, where
    the projects page would open behind it.
  - Android speed: `ProjectSwitchController.switchTo` no longer waits for the
    relay (an offline relay read as "still connecting", so a dead one always
    took the full 12 s) nor the 420 ms floor. `awaitConnection(projectId)`
    runs that wait afterwards in the background and stops when another switch
    starts or another project is active; `ProjectActions.switchTo` shows the
    "not answering yet" snackbar from it. `SwitchResult.offline` is gone.
    `ChatFeature` removes the old room's listener and log hook at once and
    runs `room.disconnect()` (up to 5 s waiting for the "bye") in the
    background; sync, mesh and Bluetooth teardown stay blocking because the
    new mesh needs the old port.
  - Android agents and plugins: `AgentSessionService` and
    `LocalControlService` now get the project's namespace (empty for the
    built-in project, so its data is unchanged; a desktop paired under
    another project before this has to be paired there again).
    `AgentsFeature` keeps an epoch bumped on every close and install that the
    account-desktops loader and the relay connector check, and the keep-warm
    watcher runs on the project scope (on the app scope every past project's
    desktop link came back on foreground). Close and install clear runner
    attention, `AgentAttachmentCache` and `AgentSentPhotos` (session ids
    repeat across relays). `SurfacesFeature` empties its agents summary and
    drops a refresh started under the old project. The shared `sharedMarket`
    package list and `planningModels` are dropped on a switch.
- 2026-10-01: the main prompt has two tiny toggles at the top, **YOLO**
  (merge, push and delete the agent when it finishes) and **No worktree**
  (the next run works straight in the repository folder), when the run goes
  to the desktop. See [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md), "YOLO and No
  worktree on the main prompt".
- 2026-10-01: starting an agent from the composer now sends the whole
  composer into the Agents tab. It shrinks in place, then flies into the
  tab and bursts there. The star still charges on the arrow while the run
  is sent. It now flies only when there is no picture of the composer
  (Android's "Start on desktop"). See [NAVIGATION.md](NAVIGATION.md),
  "Launch sparkle".
- 2026-09-30: "Add project" in the top-left project switcher of both apps no
  longer needs an instance. The new-project form asks for a name, an optional
  logo and an optional instance host; with a host it is checked against
  `/api/version` as before, without one the project is created at once
  (`RelayDirectory.addNamed` / `ProjectDirectory.addNamed`) with its own
  storage namespace, so its sign-in, node key and data stay separate, and it
  signs in on the build's default instance (ForkMesh). The phone has no
  offline mode, so this default is what keeps an instance-less project usable;
  the desktop instead asks which instance to use when the project first opens.
  Rows carry two optional JSON fields, written only when set so older rows
  stay byte-compatible: `logo` (a file name) and `hasInstance: false`. The
  subtitle of an instance-less project reads "No instance attached".
  Logos are shrunk to 256 px and saved as PNG under
  `Application Support/project-logos` (iOS) or `files/project-logos`
  (Android), and deleted when the project is removed or its logo is replaced.
  "Manage projects" (iOS) and the Projects card (Android) can choose, change
  or remove any project's logo. The project icon, like the desktop's picker,
  shows the logo first. Without a logo, app.forkmesh.com keeps the bundled
  ForkMesh mark, and any other project with an instance shows that
  instance's favicon (`/favicon.ico`, then `/favicon/favicon.ico` as ForkMesh
  relays serve it, the order the desktop uses), cached under
  `Caches/project-favicons` / `cacheDir/project-favicons`. Everything else
  shows the initial on its sender colour. The iOS switcher and the Android
  Projects card now show each project's icon in its row.
- 2026-10-02: chat makes no sound on either phone app: taps, typing, send and
  an arriving message stay silent on the chat, thread and Discord screens
  (NAVIGATION.md §6.5).
- 2026-10-02: iOS interface sounds no longer wait for the Agent Watch screen.
  The cue player activates the `.playback` audio session at launch and each
  time the app comes back to the foreground. Before, only the Watcher
  activated it (NAVIGATION.md §6.5).
- 2026-10-01: interface sounds play from the moment the app opens, silent
  mode included; the Settings Sounds switch is the one way to mute them
  (NAVIGATION.md §6.5).
- 2026-09-30: interface sounds on both apps. The phones play the desktop's
  fourteen interface cues (click, select, navigate, open, close, toggle,
  send, arrive, success, warn, error, delete, keystroke) through the Agent
  Watch synth, on by default and muted by the silent switch or ringer mode.
  They replace three system beeps that were off by default. Every
  `FmHaptics` call now also sounds, and the kit's buttons, dialogs, sheets,
  switches, tabs and navigation do too. See
  [NAVIGATION.md](NAVIGATION.md) §6.5.
- 2026-09-30: the iOS Actions screen reloads every 30 s, or every 5 s while a
  control is pending or a run is active, as Android's already did. It used to
  load once. So a 429 stayed on screen as "Too many attempts. Try again
  later." until the user pulled to refresh. On build 1.4.0 (82), opening
  forkmesh/forkmesh sent about 500 single `/blob` reads in 4 s. That tripped
  the limit in the entry below, and the Actions request that followed got a
  429 (n1 access log). The batching below fixes that burst, but no iOS build
  had shipped it: main did not compile (`HealthFeature.swift` missing
  `import FMAccount`), and the next Mac build failed in its checkout (see
  [MAC_BUILD.md](MAC_BUILD.md)). Test: `testRateLimitedLoadRecoversOnTheNextPoll`
  in `CITests.swift`.
- 2026-09-29: the issue, pull, discussion and release lists read their record
  files in batches of 50 through `GET /api/repo/<owner>/<name>/blobs`
  (`ApiService.blobs` on both platforms), as the web dashboard does. They used
  to send one `/blob` request per record, all at once. On forkmesh/forkmesh
  (700+ issues) one issue list fired about 500 requests in a few seconds.
  That tripped the host's per-address limit (240 requests per 10 s, then 30 s
  of 429s, `app/host/forkmesh_host/request_policy.py`), so the next request
  from that phone was refused. "Delete all pings" then failed with "Pings could not be
  deleted." (seen in the n1 access log, iOS build 74). Paths a batch misses
  fall back to single `/blob` reads, two at a time. Record event files are
  batched per folder, and at most four records load their events at once.
  Tests: `testPublishedIssuesReadRecordsInBatchesInsteadOfOneRequestEach` in
  `ApiServiceRepoTests.swift` and `ApiServiceTest.kt`.
- 2026-09-29: the shell composer only starts agents. Its Agent/Chat toggle
  (iOS mode pill and `/chat` command, Android mode button) is gone, because
  the Chat screen has its own composer. Opening the composer on the Chat tab,
  or with "Send to prompt" on a message, now prefills a new agent prompt. The
  pill remains only while a page registers a prompt target (Calories), where
  it switches between that target and Agent.
- 2026-09-29: the Agents tab lists the desktop's idle and finished agents,
  not only running ones (Android under "On <desktop>", iOS in its merged
  list), and says when the desktop could not be reached. Agents on organization repositories were invisible
  between turns because the desktop pushes no snapshots for them. See
  [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md).
- 2026-09-29: #email-log shows its notes on iOS and Android. The server writes
  that channel with the room key only (no device seal), and
  `OfficeChannelService.accept` dropped every unsealed frame in a private
  room, so the phones listed #email-log but it stayed empty (the desktop had
  the same bug until 2026-09-28, [MAIL_LOG.md](../MAIL_LOG.md)). `OfficeRoom`
  now reads `serverVisible` and `readOnly` from `/api/chat/channels`; room-layer
  frames are accepted only in a server-visible room, and every other private
  room still refuses them. A read-only room refuses `send`, the composer is
  replaced by "Only ForkMesh posts in this channel.", the header badge reads
  **server-visible** and the seal sheet says the server writes and can read
  the notes. Tests: `testAServerVisibleNoticesChannelShowsServerNotes` in
  `OfficeAndDiscordTest.kt` and `OfficeChannelsTests.swift`.
- 2026-09-29: the iOS file preview (`RepoFileScreen`'s `RepoCodePreviewView`)
  wraps long lines and scrolls vertically only. It used to scroll both ways
  with each line unwrapped, so a vertical swipe on a phone drifted sideways
  and the code slid under the gutter. A wrapped line keeps one line number at
  its top. `RepoCodePreview.estimatedWidth` is no longer used by the app. The
  editor (`CodeEditorView`) still does not wrap.
- 2026-09-29: starting an agent no longer shows a snackbar over the bottom
  bar. A sparkle with a trail flies from the composer to the Agents tab,
  and the running count there goes up with a pop. On Android, the Agents
  badge shows running runs when nothing is waiting on you. See
  [NAVIGATION.md](NAVIGATION.md), "Launch sparkle".
- 2026-09-30: the launch sparkle now starts on the composer's up arrow.
  The arrow pops and a star charges on it as soon as Send is tapped, then
  the star climbs in a big arc and drops into the Agents tab. It used to
  appear near the composer's bottom-right corner once the server answered
  (on Android, above the tab). See [NAVIGATION.md](NAVIGATION.md),
  "Launch sparkle".
- 2026-09-29: every ping a desktop raises (an agent finishing, waiting or
  failing, workflow runs, usage resets) also reaches a phone paired with it as
  a push notification; agent pings open the agent. See
  [DESKTOP_PINGS.md](DESKTOP_PINGS.md).
- 2026-09-28: Tapping any push opens its ping at once, and the Inbox shows the
  last-known pings before the network answers. A tap used to wait for the
  inbox to be re-read (twice, via the trailing refresh) and for mark-read
  before it moved, and a ping with no record link only opened the Inbox list
  (Android did not navigate at all). Now:
  - `PingSnapshot` (`FMApi/Inbox/PingSnapshot.swift`) keeps the last 100 pings
    per account in `KeyValueStore`. `NotificationService` and `InboxModel` show
    them when created and save every fetched page into it.
  - iOS builds the ping from the tapped banner (`PushOpen.ping`: the extension
    already put `alertId`, `href`, `kind` and `repo` on it; the actor comes from
    the "repo · by actor" subtitle) and inserts it with `insertFromPush`.
    Delivered notifications are inserted at launch and on every resume
    (`AppServices.seedDeliveredPings`), and a push shown in the foreground is
    inserted as it arrives. A row inserted this way survives refreshes that
    do not have it yet, for ten minutes counted from the ping's own time
    (`ts`), not from the insert. Re-seeding a banner on resume therefore
    cannot bring back a ping that was deleted on the relay, for example from
    the desktop's Pings page (2026-10-06, [DESKTOP_PINGS.md](DESKTOP_PINGS.md)
    "Deleting a ping").
  - `HomeShellModel.routePush` marks read in the background (`markReadSoon`)
    and returns `.ping(id)` when there is no record to open; `InboxScreen`
    picks that up from `NotificationService.openRequest` and pushes
    `InboxItemScreen`. It only refreshes over the network when the push
    carried nothing to build a row from.
  - Android: see [ANDROID.md](ANDROID.md) §6.
- 2026-09-28: Tapping a "new pull request" push opens that pull request on
  both apps instead of "has not been published yet". The detail screen used
  to list every pull on the `forkmesh/pulls` ledger (cached 60 s, so a list
  read before the push lacked the new one) and pick the number, and a new
  pull often lives only on its own `forkmesh/pull/<n>` branch until the
  ledger catches up. `publishedPull` (iOS `ApiService+Repo.swift`, Android
  `ApiService.kt`) reads that one record at the ledger, then at
  `forkmesh/pull/<n>`, and on a hit drops the cached list and ledger tree so
  Back shows it at the top. `PullDetailModel` (iOS) and `PullDetailScreen`
  (Android) retry a miss after 1.5, 3, 6, 12 and 24 s. The pulls list also
  reads the gateway's `pullRefs` so branch-only pulls are listed, as on the
  web. Details: [ANDROID.md](ANDROID.md) §6.
- 2026-09-28 (iOS): chat messages disappear after a per-account time, seven
  days unless the account picks 3 days, 1 day, 8 hours or 1 hour, in Settings
  under Profile or per chat from a channel's or person's context menu.
  `ChatRetentionService` (FMApi) reads and writes `POST
  /api/accounts/chat-retention` and caches the answer per account; `ChatHub`
  (FMChat) drops expired relay, office, mesh and thread messages on arrival,
  on restore, every five minutes and on every change. Details in
  `native/ios/docs/FMChat.md` and `native/ios/docs/FMApi.md`.
- 2026-09-28: long-pressing any Chat message offers Delete message: relay
  rooms, direct messages, thread replies, office channels and the mesh room.
  Your own relay messages can still be deleted for everyone (a `delete`
  frame). Every message can also be deleted for this device only: its id goes
  into `ChatExtras`' hidden set, `ChatHub.rows`/`threadRows` and thread
  summaries skip it, and the ids persist in the chat history file under
  `hiddenMessages` (the newest `ChatLimits` 2,000), so a history replay or
  restart does not bring the message back. Nothing is sent to peers. Other
  people's messages cannot be deleted for everyone from a phone. The desktop's
  admin delete is checked against the account's primary key, and a phone
  signs with its own device key. iOS shows one confirmation dialog with both
  choices. Android shows a choice sheet when both apply, and a confirmation
  otherwise.
- 2026-09-28: The new-channel sheet offers Public or Private, and Private
  creates a relay-enforced room with the typed invitees
  ([app/chat-private-rooms.md](../app/chat-private-rooms.md)). It used to offer
  a mesh "Private (invite only)" room, which was a label on World #general
  frames that every device could read, and its server option became public
  whenever no member was typed. Not built here (no Swift or Kotlin toolchain on
  the agent box).
- 2026-09-28: A relay reconnect no longer puts a strip under the top bar
  ("Reconnecting to ForkMesh…" on iOS, "Connecting to the relay..." on
  Android). Every dropped socket reconnects, so the strip flashed up now and
  then; the avatar's connection dot already turns amber while connecting. The
  strip (`FmConnectionBanner` on iOS, `ConnectionBanner` on Android) shows only
  when the relay is offline. The status emoji moved from beside the avatar onto
  it, as a small badge at its bottom left (the connection dot keeps the bottom
  right): `AvatarWithDot(statusEmoji:)` on iOS, the top bar's `AvatarWithDot`
  on Android. Android `:app:compileDebugKotlin` passes; the iOS files are
  parse-checked only (SwiftUI does not build on the Linux agent box).
- 2026-10-01: The status emoji sits to the left of the top-bar avatar again,
  larger (half the avatar's size on iOS, 17 sp on Android) and with no badge
  background. The connection dot lost its white ring: it is a solid dot in the
  status colour, and turns amber while the member has marked themselves away
  (`AvatarWithDot(away:)` on iOS, fed from `ShellTopBarState.statusAway`; the
  top bar's `away` on Android, from `account.userStatus.away`).
- 2026-10-01: Admin > Database works: the server now answers
  `/api/admin/phone/database` ([app/d1.md](../app/d1.md)). The iOS page is
  compact: the table list is one line per table (monospaced name, a lock for
  restricted tables, an orange mark for audit gaps, the row count), with the
  search field pinned under the title so the floating tab bar no longer
  covers it. A table opens as a spreadsheet grid (`DatabaseGrid` sizes each
  column to its longest value, 3 to 24 characters) with a pinned header row,
  horizontal scrolling and the next page loading as the last row appears; a
  row opens a two-column detail. The other `/api/admin/phone/*` sections
  (overview, errors, cron, storage, pending, relays) still have no server
  route.
- 2026-10-02: the CRM on both native apps does what the desktop CRM does. The screen has the
  desktop's tab strip, one chip per list, a pipeline tab for each pipeline no
  list is bound to, and "No pipeline" while any contact's flow is gone
  (`CrmOrganizationData.contactTabs`, the same membership rule as the
  worker's `_contact_in_view`). After those come Activity (sends and synced
  mailbox messages by day), Mail (Zoho boxes, move, delete, sync or connect),
  Outreach (the daily quota, templates, recent sends and, for managers, the
  outreach team), Team (the 7 or 30 day leaderboard and stacked daily chart),
  Decks (every investor-deck link with views, time and pages), and "Shared
  with me" (lists other organizations shared, editable where allowed, and
  copyable into this one). Above the strip are four tiles: contacts, pipeline
  value, emails and live decks. The organization switches from the title menu.
  - **Pipeline tabs:** these open as a list grouped by stage under a stage
    strip. The strip is a proportion bar plus a pill per stage with its count
    and value; tapping a pill filters to that stage. They can also open as a
    board whose cards drag between stage columns.
  - **Rows:** each row has an initials avatar and the stage, value, next
    step, tags, owner and last-contacted time. The desktop's Gravatar lookups
    are left out: they send a hash of every contact's email to a third party. A list's visible
    custom columns show under the name. Swipe right to advance the stage or
    mark contacted, swipe left to email or delete.
  - **Long press:** emails, calls, moves the contact to a stage, pipeline or
    list, mints and copies a deck link, copies the desktop's outreach prompt,
    or merges the contact.
  - **Select:** one `contacts/bulk` request per action. Set stage, move to a
    pipeline list (dropping sibling lists), move to a pipeline, add to a list,
    move to a list only, remove from a list, set owner, mark contacted, merge
    or delete.
  - **Sort and filter:** by stage, name, value, updated, least recently
    contacted or newest, and by owner, tag or stage.
  - **Contact page:** stage chips, a pipeline picker, list membership chips,
    every field with profile links, custom fields, notes and a timeline of
    deck links and emails. An email opens with its body, status and mailbox
    link; email records can be logged, set and deleted. The page also offers
    possible duplicates to merge, "Draft outreach with an agent" (opens the
    composer with the prompt) and copying the prompt. The editor covers
    every field, including socials, owner, source and custom fields.
  - **Email:** goes out through the CRM (`emails/send`), as outreach
    (`outreach/send`) or by opening Zoho Mail or Gmail and logging the send.
  - **Lists:** create (plain or bound to a pipeline, private or not), rename,
    rebind, move left or right, share with usernames (view or edit), share a
    link, show, hide and reorder columns, delete a column, merge into another
    list, read its history, and delete.
  - **Pipelines:** owners and admins create one (with its list), rename it,
    set its kind and add, rename, reorder or remove stages. Contacts at a
    renamed or removed stage are moved in one bulk request. Pipelines can be
    reordered and deleted, except the last one.
  - **Settings:** sender, mailbox, mail boxes, sent-mail link template and
    Zoho connect.
  - **CSV:** import previews the file, takes a target per column (including
    first/last name, own column or skip) and a new or existing list, then
    uploads in parts of 100 with progress. Export shares the CSV.
  - **Not ported:** the GitHub-contributor, Mastodon-follower and
    ForkMesh-user imports (they need a local clone or the desktop's
    accounts) and the profile "Recent updates" reads. The manual Zoho
    Self Client code came on 2026-10-07.
  - **Android:** has the same tabs, panes, sheets and actions as Compose
    screens. Its board moves a card with a long press or the card's
    "→ next stage" chip instead of dragging. The organization switches from
    the app bar, and CSV export goes out through the share sheet.
  - **Code and tests:**
    - iOS: the logic is in `FMPackages` (`CrmWorkspaceModels`,
      `CrmViewModel`, `PackagesClient+Crm`), tested by `CrmWorkspaceTests`.
      The SwiftUI files are parse-checked only.
    - Android: the logic is in `core/.../market/CrmWorkspace.kt` and
      `CrmModel.kt`, tested by `CrmWorkspaceTest`. `:app:compileDebugKotlin`
      passes.
- 2026-09-28: The top-left of both apps is the project switcher, shown as the
  active project's icon: the ForkMesh logo for app.forkmesh.com, otherwise the
  project's initial on its sender colour. On iOS, `ShellProjectSlot` (the
  icon and name) opens `ProjectSwitcher`, which has "Add
  project" and "Manage projects". The iPad sidebar puts
  `ProjectSwitcherButton(compact: true)` at its top-left. On Android,
  `ProjectSwitcherTitle(icon = true)` in `HomeShell`'s top bar opens a menu of
  projects that ends with "Add project"; a project added there is switched to
  straight away.
- 2026-09-27: direct send from the composer (Flutter 1.3.2+17,
  `feature/mobile-direct-send`) is on both native apps. Send already started
  the run through `QuickAgentLauncher`; what was left is brought over. With
  no repository on screen and none remembered, the default is the signed-in
  account's own most recently updated repository, then one already on the
  device, then the most recently updated one (it used to be the first in the
  list, which put an arbitrary mirror first). The on-device repository page
  is a context like a repository page: iOS `LocalRepoScreen` enters it on
  `ShellComposer`, Android's calls `ComposerFeature.FollowRepository`. On
  iOS the Agents tab's "+" and New section open the composer instead of the
  "Start an agent" form, as Android and Flutter already do; the form is kept
  only for "Mirror to this iPhone", which needs its clone progress.
- 2026-09-27: CRM on both native apps is always listed under Business in the
  You hub (iOS used to hide it until an organization had it installed, which
  left no way in for an organization about to adopt it). The CRM screen
  installs the package for an owner or admin (`POST
  /api/plugins/<org>/crm/install`) and tells members to ask one. Contacts can
  be deleted (`DELETE /api/crm/<org>/contacts/<id>`) behind a confirmation.
  iOS moves a contact to any stage from the contact screen or a row's context
  menu and shows Bluesky, X and Source; Android's editor picks the pipeline as
  well as the stage, and its CRM list ends with recent email activity.
- 2026-09-27: every chat message takes a long press. Relay chat and thread
  rows already had the action sheet. Android Discord rows now offer Reply and
  Copy text as iOS did; Reply shows "Replying to" above the composer and
  passes the message id to `DiscordBridge.prepareSend`. The #team room, agent
  session chats, on-device agent transcripts and the iOS on-device AI chat
  get Copy text and Select text from `FmMessageActions` (Android `ui/kit`,
  iOS `Kit`). Their bodies, and chat code fences, used to be selectable in
  place, which took the long press, so selection moved into the Select text
  sheet. Android chat rows open the sheet even without text, since relay
  rooms still offer reactions, threads and profiles. Single-block transcripts
  (desktop sessions, All agents, local desktop agents) and issue or pull
  request timelines keep in-place selection: they are not per-message rows.
- 2026-09-27: chat channel groups are saved per project and room
  (`chat/channelGroups/project-<hash of project id|room>`), not per server
  URL. The old key hashed the full URL, so editing a project's URL (scheme,
  path, alias fold) left the groups behind under a key nothing read. The
  project id is the normalised origin and does not change with those edits.
  On first open the store adopts groups still saved under the current URL's
  key and deletes that key (`ChannelGroupsStore.forProject` on Android, the
  `projectId:` initialiser on iOS). Groups already stranded by an earlier URL
  change cannot be found: their key is a hash of a URL the app no longer
  knows. Neither native app nor the Flutter app depends on a Sentry SDK, and
  none ever has: crash reports come from MetricKit on iOS and
  `ApplicationExitInfo` on Android, and Sentry exists only on the server,
  which calls its HTTP API directly (`app/src/entry.py`). So there is no Sentry
  package version to bump or pin, and no SDK is added.
- 2026-09-27: gate2's desktop control handlers built against members main had
  removed. `fleet.` now reports fleet sizing as unavailable and answers
  `fleetTarget`/`reconcile` with `hosts_page_unavailable` (sizing moved to the
  Network > Hosts autoscaling panel, `fb0fae748`); `insights.` peers show
  addresses, since the host-name cache left with the Watch page for the
  `agent-watch` package (`620f22684`).
- 2026-09-27: native development returns to this repository. The Next
  repository's `native/` held nothing newer than this copy; its unmerged
  tracker rows (agent attachments, App Store cutover, Gate 2 and WS8–WS21
  status, the Android workstreams) were ported into `PROGRESS.md` and
  `DESKTOP_PARITY.md`, and `native/` and `docs/native/` were deleted there.
- 2026-09-26: the iOS port has no payments UI, because ForkMesh is free to
  use. Settings lost Membership ("Become a member", paid plans) and Funding
  and payouts (the Solana payout address); the repository About tab lost the
  donation-address card and Activity lost Payout readiness. The app no longer
  stores, shows or sends a payout address: relay envelopes carry no `solana`,
  `AuthSession` dropped `solana` and `hasPayoutAddress`, `Repository.solana`,
  `Member.solanaAddress` and `PayoutNode` are gone, the chat roster tells
  same-named people apart by node id, network stats are fetched without
  `payouts=1`, and the privacy manifest no longer declares Payment Info.
  Bounty panels, Funds received and sponsored messages stay. The Android app
  still shows these sections (see `PARITY.md` #17, #33, #44, #70, #76).
- 2026-09-24: `AppServices+<Area>` workstream hooks; Swift packages pinned to
  the first successful ad hoc archive (93a0d7089). Native development moves to
  the Next repository.
- 2026-09-23: the Swift port finished, including on-device Gemma 4
  (fd23bd069); Codemagic `ios-native-*` workflows (690a4fd1c).
- 2026-09-14: Android port: crypto and identity, Git engine, account and
  collaboration screens.

## Related docs

- [`PARITY.md`](PARITY.md): the phone feature matrix, iOS work breakdown,
  watchOS design, bundle ids.
- [`PROGRESS.md`](PROGRESS.md), [`DESKTOP_PARITY.md`](DESKTOP_PARITY.md), [`NAVIGATION.md`](NAVIGATION.md), [`IOS_RELEASE.md`](IOS_RELEASE.md), [`ANDROID.md`](ANDROID.md), [`MAC_BUILD.md`](MAC_BUILD.md), [`AGENT_ATTACHMENTS.md`](AGENT_ATTACHMENTS.md), [`DESKTOP_PINGS.md`](DESKTOP_PINGS.md), [`MESH_PUSH.md`](MESH_PUSH.md).
- [`native/ios/README.md`](../../native/ios/README.md), [`native/ios/docs/`](../../native/ios/docs/), [`native/android/README.md`](../../native/android/README.md).
- [`www/docs/mobile/`](../../www/docs/mobile/): node contract, agent access, file editing, Git engine spec and threat model.
- [`www/docs/operations/mobile-push-notifications.md`](../../www/docs/operations/mobile-push-notifications.md).

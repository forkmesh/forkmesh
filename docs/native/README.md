# Native apps (iOS and Android)

`native/ios` (Swift + SwiftUI) and `native/android` (Kotlin + Jetpack Compose)
are the ForkMesh phone apps. Each signs in to a ForkMesh instance, browses and
edits repositories, runs and watches agents, chats, and carries the Market's
phone tools. A phone is a *mobile node*: it holds its own Ed25519 node key and
owner-encryption key and can clone and commit with its own Git engine. The two
apps share endpoints, storage keys and signing formats, so the relay and the
desktop cannot tell them apart.

- iOS ships as the App Store app **ForkMesh** (`com.forkmesh.forkmesh`)
  ([IOS_RELEASE.md](IOS_RELEASE.md)).
- The Android release APK (the desktop release's
  `forkmesh-android-universal.apk` asset) is built as `com.forkmesh.forkmesh`;
  local builds default to `com.forkmesh.nativeapp`, which installs beside it
  ([ANDROID.md](ANDROID.md), section 4).

Topic pages:

| Page | Covers |
| --- | --- |
| [SYNC.md](SYNC.md) | iOS → Android sync ledger: every iOS feature or UI change Android still has to match, and `tools/native_sync.py` |
| [PARITY.md](PARITY.md) | Phone feature matrix, platform decisions, iOS workstream hooks, the watchOS app, bundle ids |
| [DESKTOP_PARITY.md](DESKTOP_PARITY.md) | Parity with the Qt desktop, route contract, desktop control families, extension targets |
| [NAVIGATION.md](NAVIGATION.md) | Tabs, routes, Home grid, bars, sitemap and URLs |
| [THEMES.md](THEMES.md) | Accent contrast, surface families, the theme editor and its stored keys |
| [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md) | Desktop agents on the phone (relay control, diff, merge, delete) |
| [RUNNER_AGENTS.md](RUNNER_AGENTS.md) | Cloud runner agents: branches, merge and clean up, pull requests |
| [AGENT_ATTACHMENTS.md](AGENT_ATTACHMENTS.md) | Photos and files in agent transcripts and prompts |
| [DIFF_REVIEW.md](DIFF_REVIEW.md) | The diff review stream |
| [COMMIT_GRAPH.md](COMMIT_GRAPH.md) | The Commits screen's Graph tab |
| [GENIE.md](GENIE.md) | The on-device voice assistant |
| [OTHER_APP_CALLS.md](OTHER_APP_CALLS.md) | Sounds, listening, ducking and Bluetooth held back while another app's call is up |
| [DESKTOP_PINGS.md](DESKTOP_PINGS.md), [PING_QUICK_FIXES.md](PING_QUICK_FIXES.md), [PUSH_ICONS.md](PUSH_ICONS.md) | Desktop pings, one-tap fixes, banner icons |
| [MESH_PUSH.md](MESH_PUSH.md) | Wake pushes and local alerts for the `#mesh` room |
| [PUSH_ON_SCREEN.md](PUSH_ON_SCREEN.md) | No banner, or no phone ping, for what is already on screen |
| [DESKTOP_UPDATE.md](DESKTOP_UPDATE.md) | Rebuilding and restarting the desktop from the phone |
| [STATS.md](STATS.md) | The Stats board: every desktop tile over `stats.tiles`, and the release-version tiles |
| [CAPTURE_MARKUP.md](CAPTURE_MARKUP.md), [REFERENCE_LINKS.md](REFERENCE_LINKS.md) | Screen capture and markup; issue and pull links in messages |
| [IOS_RELEASE.md](IOS_RELEASE.md), [MAC_BUILD.md](MAC_BUILD.md), [ANDROID.md](ANDROID.md) | iOS release pipeline, office Mac builds, Android design notes |
| [DATA_FOLDER.md](DATA_FOLDER.md) | The projects folder (automatic, or one the user picks) that keeps settings, plugin data and the encrypted identity key through a reinstall |
| [notes.md](notes.md) | Notes: filters, autosave, conflicts, preview, sharing, links, export |
| [NEXUS.md](NEXUS.md) | NEXUS package, permissions, safety policy, host integration and verification |

Siri user guide: [`www/docs/mobile/siri.md`](../../www/docs/mobile/siri.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `native/ios/ForkMeshCore/` | Swift package with everything that is not a view; builds and tests on Linux. Modules include FMBase, FMCrypto, FMNet, FMGit, FMDiff, FMVault, FMAccount, FMApi, FMChat, FMCode, FMLocalAI, FMAgents, FMTools, FMAssistant, FMPush, FMControl and the per-area modules listed in DESKTOP_PARITY.md. |
| `native/ios/Nexus/` | Standalone Swift 6 network-intelligence package. `NexusKit` is its host API; `NexusUI` is its embeddable SwiftUI interface. |
| `native/ios/ForkMesh/` | The app target: SwiftUI views and iOS glue (`App/`, `Platform/`, `Theme/`, `Kit/`, `Shell/`, `Features/`). |
| `native/ios/NotificationService/`, `native/ios/Shared/` | Notification service extension (links `ForkMeshPushCore`: FMBase, FMCrypto, FMPush); code compiled into several targets. |
| `native/ios/ForkMeshWidgets/`, `ForkMeshShare/`, `ForkMeshAutoFill/`, `ForkMeshWatch/`, `ForkMeshWatchWidgets/` | Extensions and the watch app. |
| `native/ios/Config/` | Info.plists, entitlements, `Version.xcconfig`, `Package.resolved`, `ExportOptions.plist`. |
| `native/ios/tools/` | `generate_project.rb` (the Xcode project is generated), `check_swift_syntax.sh`, `asc_bundle_ids.py`, the Mac build scripts. |
| `native/ios/docs/` | One API note per core module. |
| `native/android/core/` | Pure Kotlin/JVM (no `android.*`), tested on the host JVM. |
| `native/android/app/` | Android + Compose: platform, theme, navigation, kit, shell, one package per feature area. |
| `native/changelog/notes.json` | Hand-written What's new notes per platform and build. |

Shortcuts (`.forkmesh/shortcuts/`):

| Shortcut | Does |
| --- | --- |
| `launch-native-android.sh`, `launch-native-android-emulator.sh`, `test-native-ios.sh` | Build and launch Android (device or emulator); run the iOS core tests |
| `deploy-native-ios-ad-hoc.sh` | `tools/next_codemagic.py build ios-native-adhoc --mirror --execute --wait`: waits for the public mirror to serve `main`, starts the Codemagic build on the app that holds the `app_store_connect` group, and follows it live |
| `deploy-native-ios-ad-hoc-mac.sh`, `deploy-native-ios-app-store-mac.sh`, `build-desktop-macos-mac.sh` | The same builds on the office Mac ([MAC_BUILD.md](MAC_BUILD.md)) |
| `ios-app-store-status.sh`, `testflight-feedback-to-tasks.sh` | Read-only App Store Connect report; TestFlight feedback to tasks ([IOS_RELEASE.md](IOS_RELEASE.md)) |
| `task-mobile-app-store-deploy.sh`, `task-mobile-play-store-deploy.sh` | Create a signed reminder task with the version, tag, `main` head and the release steps for the App Store or the Play Store |

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
  and `swift-transformers` run Gemma on the phone, fenced into the app target
  (`ForkMesh/Features/LocalAI/MLXGemmaEngine.swift`).
- **Installer hooks.** Areas plug into
  `native/ios/ForkMesh/App/AppServices+<Area>.swift` installers and never edit
  `AppServices.swift` ([PARITY.md](PARITY.md), section 3.0).
- **Projects.** A project is a name plus everything the phone holds for it
  (session, node key, owner key, vault copy, local repositories, agent records,
  settings), usually attached to one ForkMesh instance whose origin is its id.
  The original project owns the unprefixed namespace; every other project uses
  `p/<projectId>/`. A project made from a name alone has `hasInstance` false, a
  `local-<slug>-<random>` id and no server URL; it works on the device alone,
  and choosing an instance is offered, never required. A project row may name a
  `logo` file. The instance check also reads `GET /api/instance`: its `name`
  replaces "ForkMesh" in the "Connected to …" line and, on iOS, fills an empty
  Name field so a new project or relay is saved under the instance's name, not
  its host. Attaching an instance to a project with no logo (or creating a
  project or relay on one without picking a logo) saves the instance's `logo`,
  else its favicon, as the project logo. An instance from before the `logo`
  field (it answers `branded: true` with no `logo` key) is asked for
  `/favicon/site.webmanifest`, and its first `/instance-logo.png|jpg|webp`
  icon is used. A branded instance never gives its favicon: that is the stock
  ForkMesh icon, which the brand tool does not replace, so a branded instance
  with no logo keeps the letter icon. Phones cannot draw SVG, which is why the
  brand tool keeps a PNG copy of an SVG logo (see
  [INSTANCES.md](../INSTANCES.md#white-label)).
- **New project sheet (iOS, 2026-10-20).** Once "Check instance" passes, the
  sheet fetches the instance's logo straight away and shows it as the project
  image, so the user sees it before creating the project and can still change
  or remove it. The candidates come from `AddProjectModel.logoCandidates`, in
  order: the `logo` from `/api/instance`, the instance's logo in the ForkMesh
  directory, then the two favicons (left out for a branded instance). A logo
  the user picked is never replaced; one taken from the instance goes when the
  host changes. Under the form, "Instances on ForkMesh" lists ForkMesh itself
  plus every `listed` https entry of
  `GET https://app.forkmesh.com/api/start/instances` (`FMChat/Relay/InstanceDirectory.swift`;
  directory logos are only taken when they match the
  `/api/start/instances/<id>/logo?v=<n>` shape). Its Add button fills the
  instance in, checks it, loads its logo and creates the project in one tap; an
  instance that is already a project shows "Added". If the directory cannot
  be read, only ForkMesh is listed.
- **Relay features.** `/api/version` declares a `features` list; the core
  (`accounts`, `repositories`, `repoRead`) is required, every other feature
  gates its own UI, and a missing one leaves the UI absent rather than broken.
  A 404 under `/api/repo/<owner>/<name>/...` never marks a feature missing.
- **No polling for display.** Screens reload when the account event socket,
  the relay or the local network says something changed.
- **No toasts.** Feedback shows inline where the action ran.
- **Calendar saves (iOS, 2026-10-18).** Adding an event on the phone could
  look like it did nothing or vanish afterwards, though `POST /api/calendar`
  and its answer matched the server (`app/src/calendar_api.py`). Fixed in
  `FMPlanning/CalendarModel.swift` and
  `Features/Planning/Calendar/CalendarScreen.swift`:
  - `load()` replaced the event list with whatever answer came back last. A
    list requested before a save (the screen loads on every appear, step and
    pull) wiped the new event, and a deleted one came back. Each load now has
    a token, so only the newest answer is applied, and every create, edit,
    delete and RSVP is recorded with a revision. A list requested before a
    change gets that change applied on top; once a list requested after the
    change comes back, the server's copy is used.
  - The editor greyed out Save for any validation problem (more than five
    reminders, a meeting link without `https://`, an end before the start)
    and printed the reason, and any save error, under Notes at the bottom of
    the form, where nobody saw it. Save now stays on once there is a title.
    Tapping it shows the problem at the top of the form, scrolls there and
    plays the warning haptic. A spinner replaces Save while the request runs.
    The editor shows no error until Save is tapped, so an old "could not be
    loaded" message no longer appears in a new event.
  - Edit on a pushed event page set a sheet on the calendar page under it.
    `CalendarEventScreen` now shows its own editor sheet.
  - Every save, edit, delete and RSVP then failed with "Invalid json.". The
    server's `_CalendarRuntime.json()` (`app/src/entry.py`) called
    `_json_request_limit`, which `entry.py` never imported from
    `request_bodies.py`, and `calendar_api.handle` turned the `NameError`
    into `invalid_json`. Web and desktop calendar writes failed the same way.
    `entry.py` now exports the helper; `handle` answers `invalid_json` only
    for a body that does not parse or is not an object, so a coding error
    is a 500 again. `app/tests/host/test_calendar_runtime.py` runs the real
    `json()`; the older tests swapped it for a fake and missed this.
- **Chat history survives updates and restarts (iOS).** The phone is the only
  place most chat history can be read again: DMs are deleted from the server
  once acknowledged and use a per-message ratchet, sealed channel frames can
  only be opened while the prekey that sealed them is alive (8 days), and a
  channel's server history holds its last 200 messages. `ChatHistoryStore`
  keeps it in `Application Support/chat-history/`. Until 2026-10-18 history
  went missing after an app update for three reasons, all fixed in
  `ChatHistoryStore.swift`, `ChatHub.swift`, `OfficeChannels.swift` and
  `AppServices+Chat.swift`:
  - The file was named after a hash of the relay URL, and launch rewrites the
    built-in relay's URL to the build's (`RelayDirectory.ensureBuiltIn`) and
    folds `rooms/general` and the `forkmesh.com` / Next aliases onto
    `app.forkmesh.com`. A changed URL started an empty file. The file is now
    named after the project id and room (`ChatHistoryCodec.projectScope`); on
    first launch the files under every former URL of that relay
    (`ChatHistoryCodec.formerScopes`) are merged into it and renamed
    `*.migrated`, which also brings back history a previous update orphaned.
  - Server-side group channels (`#office/…`, which include `#general`,
    `#random` and `#welcome`) were never written to disk, so a restart showed
    only what the server could still give and decrypt. They are saved with the
    rest and restored into `OfficeChannelService`; a copy from the server
    replaces the restored one in place.
  - A sign-out or an expired session (401) cleared the DMs in memory and the
    next save wrote the file without them. The hub now holds them while signed
    out and gives them back when the same account signs in again; another
    account drops them.
  The store also never overwrites what it could not read: a file that exists
  but cannot be read yet (data protection before first unlock) blocks writes
  to it until a later read succeeds, a file that does not parse is renamed
  `*.unreadable-<ms>` instead of being replaced, and a file from a newer or
  older build (`v` other than 1) is read field by field instead of discarded.

## Protocols and interfaces

- Relay REST (`/api/...`) and WebSockets: the relay room socket and the
  account event socket (`/api/nodes/events`). The phone as a node:
  [`www/docs/mobile/mobile-node-contract.md`](../../www/docs/mobile/mobile-node-contract.md);
  agents from the phone:
  [`www/docs/mobile/mobile-agent-access.md`](../../www/docs/mobile/mobile-agent-access.md).
- Push: `app/src/apns.py` pushes the `pings`, `direct-messages`, chat,
  `code-push` and `calls` (an incoming call, sent as a PushKit VoIP push
  when the phone registered a VoIP token, see
  [chat-direct-calls.md](../app/chat-direct-calls.md)) topics to iOS; every payload carries `origin` so a tap routes to
  the right project. Android has no push transport (no FCM, by design): a
  foreground service keeps the account event socket open
  ([ANDROID.md](ANDROID.md), section 6). A desktop's pings reach its linked
  phone as `desktop_ping` rows ([DESKTOP_PINGS.md](DESKTOP_PINGS.md)).
- Desktop control: on the same Wi-Fi the phone finds the desktop through the
  `#mesh` beacon and drives it over the mesh TCP port; away from home the same
  protocol is forwarded through the relay. Both use the signed handshake and
  per-direction encryption. A phone signed in to the desktop's account gets the
  enabled desktop keys from `GET /api/accounts/desktops` and needs no LAN
  meeting; a phone outside that account needs a six-digit pairing code approved
  on the LAN. Frames are the twin of `desktop/src/LocalControl.{h,cpp}`, pinned
  by shared test vectors.
- Cryptography is pinned by shared vectors across Kotlin, Swift and the
  desktop: [`native/ios/docs/FMCrypto.md`](../../native/ios/docs/FMCrypto.md).

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

The CRM's Zoho Self Client settings retain the client ID, secret and refresh
token in the server's encrypted organization settings. The iOS reconnect form
loads the saved client ID and data center and labels the secret as saved;
leaving it blank reuses it only for that same client ID. The API returns a
`hasClientSecret` flag rather than the secret itself. Partial settings saves
and redacted or empty credential fields preserve the existing credentials;
disconnecting explicitly removes them.

Zoho refresh failures caused by connectivity, rate limits or server outages
remain retryable and do not mark the saved sign-in as rejected. Only a
definitive OAuth credential error requires reconnection. A successful sync or
send also clears rejection markers left by earlier failures, and refresh
responses that omit scopes retain the known permissions so a mailbox-specific
send refusal does not incorrectly become a read-only grant.

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
emails a tap-to-install link ([MAC_BUILD.md](MAC_BUILD.md)). Details in
[`native/ios/README.md`](../../native/ios/README.md) and
[`native/android/README.md`](../../native/android/README.md).

## Operations

- **iOS release.** `ios-native-release` archives, uploads, adds the build to
  TestFlight internal testing and, for tagged releases, submits it for App
  Review ([IOS_RELEASE.md](IOS_RELEASE.md)).
- **Android APK.** `tools/forkmesh-release-host.py` builds
  `forkmesh-android-universal.apk` as `com.forkmesh.forkmesh`, signed with the
  v3 direct-distribution key (`android-direct-distribution-v3.jks` under the
  home project's `data/release/`). See
  [`docs/release/README.md`](../release/README.md).
- `native/ios/tools/asc_bundle_ids.py ensure` registers the bundle ids and
  capabilities (reads `ASC_KEY_ID`, `ASC_ISSUER_ID` and the key under
  `~/.appstoreconnect/private_keys/`).
- **Diagnostics.** "Send diagnostics" (the composer's "+" menu and
  `/diagnostics`) puts a performance report into the prompt; it trims its
  lowest-ranked sections to fit and lists crashes as the attributed thread's
  top frames. The prompt sends the agent to
  [PERFORMANCE_CHECKS.md](PERFORMANCE_CHECKS.md) (how to read the bundle,
  every earlier check) and asks it to add its own check there.

## Security

- Keys live in the Keychain (`native/ios/ForkMesh/Platform/KeychainStore.swift`)
  and the Android Keystore (`app/platform/SecureStore`).
- Ed25519 is never delegated to CryptoKit (randomised signatures would break
  the pinned deterministic vectors).
- The iOS app shows no prices, plans or paid features and moves no money
  ([IOS_RELEASE.md](IOS_RELEASE.md), "App Review: nothing paid on iOS").
- The **Phone Security** package (`phone-security`) is the phones' checkup,
  the counterpart of the desktop's Security package; checks, rules and code
  are in [../marketplace/phone-security.md](../marketplace/phone-security.md).
- The **Eye Control** package (`eye-control`, iOS only, experimental) moves a
  cursor with ARKit face tracking and taps on a firm blink (route
  `eyeControl`); see [../marketplace/eye-control.md](../marketplace/eye-control.md).
- The **Router** package (`openwrt`) manages an OpenWrt router on the phone's
  Wi-Fi (route `openWrtRouter`, web path `/dashboard/openwrt`). Logic is in
  `FMPackages/OpenWrt*.swift` and core `openwrt/`; screens in
  `ForkMesh/Features/Router/` and `feature/openwrt/`. How it reaches the
  router past ATS and Android's cleartext policy is in
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

### iOS performance rules

The shell's account avatar shares the phone-motion feed with the Genie view so
it reflects the phone's direction while the shell is visible. Agent Watch feel
defaults on unless the user explicitly turns it off; Reduce Motion stops the
avatar movement but does not silently change that saved feedback preference.

- No Keychain write per received frame on the main actor. `SecItemDelete`
  plus `SecItemAdd` is a round trip to `securityd`; the chat replay book
  (up to 256 senders by 1024 counters) was re-encoded and written on every
  fresh relay frame, about 65 times in the second after a reconnect to a
  busy room. It is now written at most every 250 ms off the main thread
  (`ChatKeyring.persistReplay`).
- Parse API replies of 16 KB or more off the main actor
  (`decodeResponseAsync`, `ApiParse.parseAsync`, `ApiParse.modelAsync`).
  The documents inside a reply count too: repository files read in
  batches (issue records, release manifests) are parsed in a detached
  task, not one by one on the main actor (`publishedIssues`).
- Write `@Observable` properties only when the value changes; every write
  notifies, equal or not. This includes refreshes that rerun on every return
  to the app: `AccountPackagesStore`, `HubPackageState`,
  `OrganizationPackagesLedger` and `ShellNavigatorCounts` used to rewrite
  what Home reads on each resume.
- A scene-phase refresh runs on every inactive-to-active change (Control
  Center, Notification Center, a system alert), not only after the
  background; gate it on age (`AccountPackagesStore.refreshIfStale`).
- Compute derived lists once per body and pass them down.
- Do not let a ring cancel a load in flight, whatever started it; mark it and
  run one more load afterwards, or none when the load's reads had not gone out
  yet. Join in-flight loads (`loadIfStale`, shared reads).
- Fan out independent reads (`PackagesClient.organizationPackages([String])`).
  Home's package gate asked each organization in turn, so its tiles waited
  for up to 20 round trips one after another.
- Read only what a screen needs (`publishedPullHeads`, front matter only) and
  keep ledgers between rings unless something relevant changed.
- Build the selected tab first; build other tabs later or on first visit.
  Hidden tabs set `shellTabHidden` and pause frame-rate animations.
- Fan out live values to small observed objects (one row redraws, not the
  list); throttle shell counts (`ThrottledSnapshot`, at most 4 reads a
  second); bump roster revisions only on real changes.
- Decode images once through `FmImageCache` (48 MB cap), never in `body`.
- Home and the bottom bar scale with the installed packages, so the shell
  builds their groups once per redraw and hands the installed set down
  (`HubGroups.current(guest:installed:)`). `installedGatedPackages` reads the
  project settings once and indexes the account state and catalog by id; a
  per-package scan of both made every redraw quadratic in the package count.
- Home tiles take their lookups (heat, frequent marks, owning section) from the
  layout built once per body. `ShellHomeIcon` keeps only the shown fields
  (`ShellHomeIconFace`), not the item's action closure, so SwiftUI skips icons
  that did not change.
- The performance monitor ignores frame gaps that reach back into the
  background.
- Replies that bypass `ApiService` follow the same 16 KB rule: the packages
  transport (`ApiPackagesTransport.parse`) parses large CRM, Market and
  marketing replies off the main actor, and `PackagesClient.crm` builds
  `CrmOrganizationData` in a detached task. A large organization's CRM read
  used to parse and map every contact on the main thread.
- Anything that walks the view or accessibility tree (the Genie spider and the
  aquarium, `GenieScreenScan`) runs on the main thread and can cost more than
  it measures: asking SwiftUI for its accessibility elements builds them. Scan
  on page and tab changes, then rarely, and cache everything one walk finds.
- Parse agent transcripts off the main actor. `beatsFromTranscript` costs about
  58 ms per 64 KB and `AgentTranscriptFormat.blocks` about 16 ms;
  `TranscriptBeatCache` (the watch bridge) parses in a detached task, once per
  transcript length. A live feed re-reads only the transcript that moved
  (`ControlWatchFeeds.revision`, `AgentEagleFeed`), not every followed run on
  each delta.
- Wrap main-actor work that grows with the account's data in
  `PerformanceMonitorService.section(name)`. A section over 8 ms is named in
  the `sections` detail of any stall it falls inside; one over 200 ms is its
  own `sync` stall.

## Related docs

- [`native/ios/README.md`](../../native/ios/README.md), [`native/ios/docs/`](../../native/ios/docs/), [`native/android/README.md`](../../native/android/README.md).
- [`www/docs/mobile/`](../../www/docs/mobile/): node contract, agent access, file editing, Git engine spec and threat model.
- [`www/docs/operations/mobile-push-notifications.md`](../../www/docs/operations/mobile-push-notifications.md).
- [WORKFLOWS.md](WORKFLOWS.md): the native Workflows screens and the on-phone engine (JavaScriptCore on iPhone, a Kotlin port on Android).

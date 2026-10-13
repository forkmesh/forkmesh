# ForkMesh native iOS app

The ForkMesh iOS app in **Swift + SwiftUI**, the iOS sibling of
`native/android`. Since 1.4.0 it is the App Store app ForkMesh: the App Store
app id `com.forkmesh.forkmesh` (App Store Connect app 6803060735).

**Upgrading from the retired Flutter app.** On the first native launch
`FlutterMigration` (FMAccount, run by `ForkMesh/App/FlutterMigrationRunner.swift`
before any service exists) copies the Flutter app's Keychain items (service
`flutter_secure_storage_service`) and preferences (`flutter.<key>` in
UserDefaults) into the native stores (service `com.forkmesh.forkmesh.secure`,
`fm.<key>`). The key names are the same in both apps, per-project namespaces
included, so the user stays signed in, on the same project (ForkMesh unless
Flutter recorded another), with the same node key. It retries while the
Keychain is locked, never overwrites once done, and deletes the Flutter copies
only after the relay accepts the migrated session. Files need no move.

## Layout

```
ForkMeshCore/        Swift package: everything that is not a SwiftUI view.
  Sources/FMBase     ordered JSON, encodings, formatting, storage protocols, config
  Sources/FMCrypto   Ed25519, X25519, ML-KEM-768, AEADs, KDFs; room seal, owner crypto, identity
  Sources/FMNet      HTTP and WebSocket clients behind protocols
  Sources/FMGit      the Git engine: objects, packs, refs, index, worktree, transport
  Sources/FMDiff     unified diffs, syntax highlighting, patch building
  Sources/FMVault    the password vault's fmv1 suite and client
  Sources/FMAccount  settings, auth and session, trust lists, sync status, stall log, performance monitor
  Sources/FMApi      models, ApiService, inbox, notifications, account events, user status
  Sources/FMChat     relay, rooms, local mesh, node sync
  Sources/FMCode     repositories, diffs, highlighting, collaboration, on-device Git
  Sources/FMLocalAI  on-device models: catalog, weight downloads, engine protocol, mesh-node handler
  Sources/FMAgents   agent sessions, launch model, terminal, on-device agents
  Sources/FMTools    activity, telemetry, logs, notes, tasks, calories, bug reports
  Sources/FMWatchWire phone-to-watch frames and commands (builds for watchOS)
  Tests/<Module>Tests
ForkMesh/            The app target: SwiftUI views and iOS glue only.
  App/               entry point, service wiring
  Platform/          UserDefaults prefs, Keychain secure store, URLSession, lifecycle
  Theme/             design tokens (colours, type, spacing, haptics), FmIcon
  Kit/               shared SwiftUI components (surfaces, explorer, loading, overlays)
  Shell/             HomeShell, the top bar and the prompt bar
  Features/<Area>/   one folder per area: Account, Code, Chat, Agents, LocalAI, Tools
  Resources/         icon fonts, asset catalog
NotificationService/ the notification service extension
Shared/              compiled into both the app and the extension (PushCredentialStore)
Config/              Info.plists, entitlements, Version.xcconfig, Package.resolved, ExportOptions.plist
ForkMeshWatch/       the watchOS app (SwiftUI; see "Watch app" below)
ForkMeshWatchWidgets/ its complications and Smart Stack widget
tools/               generate_project.rb, check_swift_syntax.sh, asc_bundle_ids.py
ForkMesh.xcodeproj   generated; regenerate with tools/generate_project.rb
docs/                one API note per core module
```

Module notes: [FMBase](docs/FMBase.md), [FMCrypto](docs/FMCrypto.md) and
[its additions](docs/FMCryptoAdditions.md), [FMNet](docs/FMNet.md),
[FMVault](docs/FMVault.md), [FMAccount](docs/FMAccount.md),
[FMApi](docs/FMApi.md) ([models](docs/FMApiModels.md),
[view models](docs/FMApiViewModels.md)), [FMChat](docs/FMChat.md),
FMCode ([repos](docs/FMCodeRepos.md), [collaboration](docs/FMCodeCollab.md),
[local Git](docs/FMCodeLocal.md)), [FMLocalAI](docs/FMLocalAI.md),
[FMAgents](docs/FMAgents.md), [FMTools](docs/FMTools.md), [FMAssistant](docs/FMAssistant.md). The developer home
page for both native apps is [`docs/native/README.md`](../../docs/native/README.md);
the parity matrix is [`docs/native/PARITY.md`](../../docs/native/PARITY.md).

**Where development happens.** Here. Since 2026-09-27 native development happens in this repository again. From
2026-09-24 it ran in the ForkMesh Next repository (`~/projects/forkmesh-backup/src`);
everything from there was brought back here and its copy of `native/` removed.

## Build and test

There is no Mac in the ForkMesh fleet, so the split above is what keeps the
app testable: `ForkMeshCore` has no Apple-only imports outside `#if canImport`
blocks, and builds and tests on Linux with the swift.org toolchain.

```sh
export PATH="$HOME/projects/forkmesh/toolchain/swift/usr/bin:$PATH"   # swift.org 6.3, debian12 build
# (before the projects layout move this was ~/.forkmesh/toolchain; see docs/tools/projects-layout.md)
cd native/ios/ForkMeshCore
swift test                                   # every module
FM_ONLY=FMCrypto swift test                  # one module, what it depends on, and its tests
native/ios/tools/check_swift_syntax.sh       # parse (not type-check) the SwiftUI sources
```

`swift test` on a Mac builds ForkMeshCore for macOS 15, not 14. With a
macOS 14 target, Xcode 26's Swift 6.2 debug build routes the concurrency
APIs that gained an `isolation:` parameter (`withTaskCancellationHandler`,
`withChecked(Throwing)Continuation`) through their `@backDeployed` thunks,
and the first `GitNetHttpTransport.send` in
`LocalEngineTransportTests.testAStalledBodyFailsAfterTheIdleReadTimeoutOrTheDeadline`
aborted xctest with SIGABRT, "freed pointer was not the last allocation"
(swiftlang/swift#84793 is the same failure). Linux has no back-deployment
and never hit it. macOS is only the test host (the apps target iOS and
watchOS), and Xcode 26 itself needs macOS 15.6. So that iOS 17 debug builds
of the app are safe too, `GitNetHttpTransport` does not use
`withTaskCancellationHandler`: it waits on the head and the body by polling
every 50 ms for task cancellation, the cancellation token, the deadline and
the idle-read timeout, and cancels the inner task when one fires. Other
callers of the back-deployed APIs (FMNet's URLSession client and web socket,
`GitOperationRunner`, the local-control and mesh continuations) are unchanged.
Still open on macOS (Codemagic 6ab4eb18db035ce88d7966e0): cancelling the
task that calls `GitNetHttpTransport.send` while it waits aborts xctest with
the same "freed pointer" message, so there is no test for that path yet.

Xcode 26.6 (Swift 6.3) brought the abort back with the polling transport and
a macOS 15 target: Codemagic 6abe1549083ff0a9a53af457 (2026-10-01) died in
`LocalEngineTransportTests`, the suite after `LocalEnginePushTests`, with no
change to the transport or its tests since 2026-09-28. The other Swift 6.3.x
reports of this abort (swiftlang/swift#86204, and
GoogleCloudPlatform/recaptcha-enterprise-mobile-sdk#183 and #192) are
compiler-generated async frames freed out of order, which safe Swift code
cannot do. Codemagic first ran `swift test` with
`SWIFT_DEBUG_ENABLE_TASK_SLAB_ALLOCATOR=0` so the task allocator would use
`malloc`/`free`, but Swift 6.3.3 (Xcode 26.6 on the office Mac) no longer
reads that variable ("unknown environment variable"), and on 2026-10-08
`AppsChatTests` aborted with "freed pointer was not the last allocation" on
every run while the same suite passed in an optimized build. So the
ForkMeshCore test step now runs `swift test -c release -Xswiftc
-enable-testing --scratch-path .build-release`: the optimizer does not emit
the out-of-order async frames, `-enable-testing` keeps `@testable import`
working, and the separate scratch path keeps Debug builds' cache intact. The
step's alarm is 3600 seconds because the optimized build is slower. The
office Mac's sparse checkout includes `/conformance/`
because `AgentTranscriptFormatTests` reads the shared transcript fixtures
from `conformance/agent-transcript/v1/`. App
builds made with Swift 6.3 can still hit the abort in Debug on the same
paths. When `swift test` fails,
Codemagic also prints the last `Test Case … started` line, so the log names
the test that crashed.

Tests that wait for a download to reach N partial bytes watch the one file
being downloaded, not `partialBytes(for:)`: that total also counts the small
files downloaded and promoted first, so the wait could end, and cancel,
before the weights started.

Free space for a download (2026-10-09): `LocalModelStore.availableCapacity`
reads `volumeAvailableCapacityForImportantUsage`, but macOS returns 0 for it
(and for the opportunistic figure) to a process with no login session, such
as `build` over ssh on the office Mac, while `volumeAvailableCapacity` and
`systemFreeSize` give the real 148 GB. Every test download then failed with
"Not enough free storage ... 0 B is free", and the tests that wait for N
partial bytes (`HuggingFaceSnapshotTests.testCancellationKeepsPartialFile`,
`LocalModelManagerTests.testDownloadFailureAndCancel`) looped until the
step's alarm. That held `~/ci/lock` for an hour and failed the App Store
shortcut's 2026-10-08 runs, and it is why the cancellation test looked like
an optimized-build hang and was skipped for a run. A 0 from the
important-usage key now falls back to `systemFreeSize`, and nothing is
skipped.

Codemagic runs `swift test` with `NSUnbufferedIO=YES`. Piped stdout is
otherwise block-buffered in 4 KB chunks, and a crash drops the name of the
test that caused it: the crash above was blamed on
`LocalEngineInteropTests` for that reason.
The step also kills `swift test` after 30 minutes, so a hung test still
publishes its log instead of running into the 90-minute build timeout, which
publishes nothing.

Tests that run the real `git` (`GitBinary` in FMGitTests, `LocalTestGit` in
FMCodeTests) give it an empty private `HOME`, no system or global config and
a fixed identity. They wait on the termination handler and drain stdout and
stderr concurrently, so the run loop never spins and a full pipe cannot
stall the child.

The app target itself (SwiftUI, Keychain, URLSession WebSocket, the extension)
only compiles on macOS. Codemagic is where that happens (`/codemagic.yaml`,
all started by hand, `working_directory: native/ios`, `mac_mini_m2`):

| Workflow | What it does |
| --- | --- |
| `ios-native-build` | Generates the project, resolves the Swift packages (Metal toolchain first), builds Debug for the simulator, archives Release unsigned, zips `ForkMesh-native.app.zip`, then runs the `ForkMeshCore` tests on macOS last, so a core test failure cannot hide whether the app compiles. |
| `ios-native-release` | Registers the bundle ids (`tools/asc_bundle_ids.py ensure`), picks the next App Store Connect build number, cloud-signs at export, uploads, and submits to TestFlight. Uses the `app_store_connect` variable group. Stops before the archive if the App Store Connect app record does not exist yet. |
| `ios-native-adhoc` | Ad hoc IPA for the devices registered in the developer account. |

The version is `Config/Version.xcconfig` (`MARKETING_VERSION`); release
overrides `CURRENT_PROJECT_VERSION` with the next free build number. The same
file carries `FORKMESH_ENV`, `FORKMESH_API_HOST` and `FORKMESH_WORLD_URL`
into the Info.plist. On a Mac:

```sh
cd native/ios
ruby tools/generate_project.rb               # needs the xcodeproj gem (ships with CocoaPods)
xcodebuild -project ForkMesh.xcodeproj -scheme ForkMesh \
  -destination 'generic/platform=iOS Simulator' build CODE_SIGNING_ALLOWED=NO
```

Toolchain: Swift 5 language mode (tools 5.9), iOS 17.0 deployment target
(Observation), Xcode 16 or newer (the project uses file-system synchronized
groups, so adding a source file never edits the project).

## Home Screen widgets

`ForkMeshWidgets` never calls the server. The app writes one
`SurfaceSnapshot` (FMSurfaces) to the keychain group it shares with its
extensions (`SurfaceSnapshotStore.shared()`), and every widget reads it;
`SurfacesCoordinator` rewrites it and reloads only the widget kinds whose
section changed.

The **Overview** widget (`com.forkmesh.widget.overview`; small, medium, Lock
Screen rectangular and inline) shows six counts:

| Count | Source |
|---|---|
| Agents | every run the Agents tab lists (`AgentsSurface.total`, counted before the six-card cap) |
| Running | running and waiting runs (`AgentsSurface.running`) |
| Issues, PRs | open issues and open pull requests in the account's own repositories: `/api/repositories` rows whose owner or public owner is the signed-in node, newest 20, each read through `/api/repo/<owner>/<name>/issues` and `/pulls` |
| Tasks | tasks on the organization board (`/api/tasks`) not marked done, up to five pages; `N+` when more pages remain |
| Pings | the inbox's unread count (`NotificationService.unreadCount`) |

No endpoint returns issues or pull requests across repositories, so the
coordinator adds them up itself (`SurfacesCoordinator.loadWork`). It does
this on resume and at sign-in, at most every five minutes, and only while
an Overview widget is placed. A count it could not load shows `–`: for
example Tasks for an account that is not an organization member, or Issues
when the repository list, or every repository in it, failed to load. The medium size links each tile to its screen.
The other sizes open Agents (Needs you when an approval is waiting).
Adding `work` and `AgentsSurface.total` raised `SurfaceSnapshot.currentVersion`
to 2. A version-1 file on disk reads as no snapshot until the app writes the
next one.

## Watch app

`ForkMeshWatch` (bundle id `com.forkmesh.forkmesh.watchkitapp`) lists the
account's agents, shows each one live on the Watch field, plays its work as
sound on the watch or AirPods, taps the wrist for tool calls, errors and
finished runs, and offers Stop, Continue, Run again and a dictated prompt.
The design is `docs/native/PARITY.md`, section 4.

The watch is not a ForkMesh client of its own: the iPhone app's
`Platform/WatchBridge.swift` reads the sessions and transcripts, sends compact
`FMWatchWire.AgentFrame`s over WatchConnectivity and carries out the watch's
commands (over the local control channel when a desktop is on the phone's
network, else through the relay queue). Pause and Approve are not offered
until they exist server-side. `ForkMeshWatchWidgets`
(`...watchkitapp.widgets`) reads the last summary from the watch's keychain
group `742ZANCB57.com.forkmesh.forkmesh.watchkitapp` (`WatchGroup.shared()`). Beats, voices and the synth are WS4's
`FMAgentWatch`, the same sonifier as the phone's Listen; the watch links it
through the package's `ForkMeshWatchCore` product.

The watch app ships inside the iOS IPA; `ios-native-build` also builds it on
its own for the watchOS simulator.

Signing. Neither the watch nor the phone's widgets use an App Group, which
the App Store Connect API cannot provision; until 2026-10-09 that kept both
out of every release (docs/native/IOS_RELEASE.md, "Widgets, Live Activities
and the watch"). Every workflow now builds them (`FORKMESH_NATIVE_WATCH=0` or
`FORKMESH_NATIVE_WIDGETS=0` leaves one out).

Ad hoc builds install only on registered devices, watches included:
`tools/asc_bundle_ids.py register-device "Name" <UDID>` (ASC_KEY_ID,
ASC_ISSUER_ID and the key as for `ensure`; `devices` lists them, `has-watch`
says whether a watch is registered), then build again. App Store: add watch
screenshots in App Store Connect (the icon is in `ForkMeshWatch/Assets.xcassets`).

## Dependency policy

First-party only, as in `native/android`: Foundation, SwiftUI, CryptoKit,
Network, Security, LocalAuthentication, UserNotifications and zlib from the
OS. No CocoaPods, and one deliberate exception among Swift packages (below).
Everything else is in this tree:

| Need                     | In this tree                                              |
| ------------------------ | --------------------------------------------------------- |
| HTTP                     | `FMNet` `HTTPClient` over `URLSession`                    |
| WebSocket                | `FMNet` `WebSocketClient` over `URLSessionWebSocketTask`  |
| Cryptography             | `FMCrypto` (CryptoKit where the OS has it, pure Swift otherwise) |
| Git                      | `FMGit`                                                   |
| Preferences              | `FMBase.KeyValueStore`, `Platform/UserDefaultsStore`      |
| Secure storage           | `FMBase.SecureStore`, `Platform/KeychainStore`            |
| State                    | `@Observable` models from the core, SwiftUI environment   |
| Links, files, device auth, device info, paths | `openURL`, `fileImporter`, `LAContext`, `UIDevice`, `FileManager` |
| Compression              | `FMGit`'s zlib wrapper                                    |
| Material icons / MingCute | `FmIcon("name")` over the bundled icon fonts             |

**The exception: on-device models.** The app links
[mlx-swift-lm](https://github.com/ml-explore/mlx-swift-lm) (`MLXLLM`,
`MLXLMCommon`, from 3.31.4), [mlx-swift](https://github.com/ml-explore/mlx-swift)
(`MLX`, from 0.31.4) and
[swift-transformers](https://github.com/huggingface/swift-transformers)
(`Tokenizers`, from 1.3.4), each up to the next minor version, with every
resolved version (the three and their dependencies) pinned in
`Config/Package.resolved`, which `tools/generate_project.rb` copies into the
project. They run Google's
Gemma 4 on the phone's own GPU (Apple's MLX; public demos show about 40
tokens/s on recent iPhones), which makes an iPhone a local agent node in the
mesh rather than only a client: a private chat, an on-device coding agent that
needs no API key, and a model paired peers can use. No OS framework loads these checkpoints, and
writing a transformer runtime is out of scope. The exception is fenced in:

- App target only. `ForkMeshCore` stays dependency-free and Linux-tested;
  `FMLocalAI` holds the catalog, downloads, device fit, view models and the
  engine protocol, and `ForkMesh/Features/LocalAI/MLXGemmaEngine.swift` is the
  one file that imports MLX, all of it under `#if canImport(MLXLLM)`, so
  removing the packages leaves a building app whose engine reports itself
  unavailable. The notification extension links none of it.
- Weights are downloaded by our own code (`FMLocalAI`, over `FMNet`) from
  Hugging Face at pinned commits, not by the Hub SDK; the MLXHuggingFace macro
  target is not used.
- The packages bring their own transitive dependencies (mlx-swift's C++ and
  Metal sources, swift-jinja, swift-collections, swift-crypto, yyjson,
  swift-huggingface, and swift-syntax for mlx-swift-lm's unused macros).
  Codemagic installs Xcode 26's Metal toolchain and resolves them in a step of
  its own (`native_packages`).

Adding any other dependency needs an explicit decision, not a convenience.

## Status and known gaps

The core runs about 1,500 XCTests on Linux. The app target compiles on
Codemagic (the first `ios-native-build` and `ios-native-adhoc` runs were on
2026-09-23/24), and the Swift packages are pinned to what the first successful
ad hoc archive resolved (`Config/Package.resolved`). Parity workstreams
(projects, push, composer, agent Watch, watch bridge) plug in through
`AppServices+<Area>.swift` installers so they never edit
`ForkMesh/App/AppServices.swift`. Progress and gaps are tracked in
[`docs/native/PARITY.md`](../../docs/native/PARITY.md). Known gaps:

- **Core tests on macOS.** The first macOS run found failures Linux never
  shows (`HuggingFaceSnapshotTests.testCancellationKeepsPartialFile`, and an
  abort in `LocalEngineInteropTests.testSystemGitVerifiesAndReadsAForkMeshGeneratedPack`);
  that is why `ios-native-build` runs the core tests last. The first one was
  the free-space reading below, not the platform.
- **Gemma 4 memory.** E2B (4-bit) needs about 2.6 GB resident, E4B about
  4.2 GB. Before a real device will keep them loaded, add
  `com.apple.developer.kernel.increased-memory-limit` to both app
  entitlements files, *after* enabling "Increased Memory Limit" for
  `com.forkmesh.forkmesh` in the developer portal (otherwise the export
  cannot find a matching profile). Generation, tool calls and the Metal path
  have only been type-checked, not run.
- **Bluetooth mesh** runs over BLE GATT (`CoreBluetoothMeshTransport`), not
  RFCOMM; it has only been parse-checked on Linux, not built on a Mac or run
  on a device.
  LAN mesh and the desktop control channel use BSD sockets. Multicast discovery would need the
  `com.apple.developer.networking.multicast` entitlement; the subnet sweep
  works without it.

## Keychain items stay on the device

Every keychain item uses a `...ThisDeviceOnly` accessibility class, so no
session, key or vault copy travels in an iCloud or encrypted Finder backup to
another phone. The push session that `Shared/PushCredentialStore.swift`
shares with the notification service extension was the exception until
2026-10-12: it used `kSecAttrAccessibleAfterFirstUnlock`, so the same bearer
session the app keeps device-only was restorable from a backup. It now uses
`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`, which the extension can
still read while the phone is locked; the next `save` rewrites an old item.
`StoreConfigTests.testEveryKeychainItemStaysOnThisDevice` fails on any other
class.

## Bundle ids and the App Store

The app is the App Store app id `com.forkmesh.forkmesh`
(`APP_BUNDLE_ID` in `tools/generate_project.rb`); its extensions are
`<app>.NotificationService`, `<app>.Widgets`, `<app>.Share` and
`<app>.AutoFill`, the watch app `<app>.watchkitapp` and its widgets
`<app>.watchkitapp.widgets`. Earlier native builds shipped as a separate App
Store Connect app, `com.forkmesh.nativeapp`; `app/src/apns.py` still accepts
push tokens for either topic (`APNS_BUNDLE_IDS`). Releasing is
[`docs/native/IOS_RELEASE.md`](../../docs/native/IOS_RELEASE.md).

## Conventions

- **One app on two platforms.** Keep behaviour, copy, endpoints, storage keys
  and signing formats identical to `native/android`; the relay and the
  desktop app cannot tell the apps apart. Where the Kotlin app already solved
  a problem (pure-language crypto, the Git engine, test vectors), read it too.
- **Logic in the core, views in the app.** State, networking, parsing,
  validation and view models (`@Observable` classes) go in `ForkMeshCore`,
  where Linux compiles and tests them. A SwiftUI view binds to a view model
  and lays things out; it does not fetch, parse or decide.
- **Core portability.** No `UIKit`, `SwiftUI`, `Combine` or `CryptoKit`
  outside `#if canImport(...)`; `import FoundationNetworking` under
  `#if canImport(FoundationNetworking)`. Platform services reach the core
  through protocols (`KeyValueStore`, `SecureStore`, `HTTPClient`,
  `WebSocketConnector`) implemented in `ForkMesh/Platform`.
- **Conservative SwiftUI.** Nobody can compile the app target before it
  reaches Codemagic. Use iOS 17 APIs you are certain of, keep view bodies
  small (split into computed subviews; long bodies time out the type
  checker), avoid clever generics, and run `tools/check_swift_syntax.sh`.
- **Tests** are XCTest, in `Tests/<Module>Tests`, and share their vectors and
  cases with the Kotlin ones (`native/android/core/src/test`).
- **Every message can be copied or handed to an agent** (2026-10-13). Each
  alert, error, warning, notice and status line carries two small icons, copy
  and send to agent (`Kit/FmMessageTools.swift`). Use `FmMessageTools(text)`
  or `.fmMessageTools(text)` on any new message; `FmNoticeLine`,
  `FmEmptyState`, `FmDialog` (so `FmConfirmDialog` and `FmPromptDialog`),
  `FmConnectionBanner` and `FmNeedsDesktopNote` already include it. A system
  `.alert` showing an error gets "Copy" and "Send to agent" buttons instead.
  Send to agent closes any open sheet and opens the composer with the prompt
  from `MessageHandoff.agentPrompt` (`ForkMeshCore/Sources/FMApi/Navigation`),
  which quotes the message and asks the agent to find the cause and fix it.
  Chat and transcript bubbles keep their long-press menu
  (`Kit/FmMessageActions.swift`) instead. `native/android` does not have the
  icons yet.

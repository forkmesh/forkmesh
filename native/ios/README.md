# ForkMesh native iOS app

The ForkMesh iOS app in **Swift + SwiftUI**, the iOS sibling of
`native/android`. App Store app id `com.forkmesh.forkmesh`.

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
  Kit/               shared SwiftUI components
  Shell/             HomeShell, the top bar and the prompt bar
  Features/<Area>/   one folder per area: Account, Code, Chat, Agents, LocalAI, Tools
  Resources/         icon fonts, asset catalog
NotificationService/ the notification service extension
Shared/              compiled into both the app and the extension (PushCredentialStore)
Config/              Info.plists, entitlements, Version.xcconfig, Package.resolved, ExportOptions.plist
ForkMeshWatch/       the watchOS app
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
[FMAgents](docs/FMAgents.md), [FMTools](docs/FMTools.md),
[FMAssistant](docs/FMAssistant.md). The developer home page for both native
apps is [`docs/native/README.md`](../../docs/native/README.md); the parity
matrix is [`docs/native/PARITY.md`](../../docs/native/PARITY.md).

## Build and test

`ForkMeshCore` has no Apple-only imports outside `#if canImport` blocks, so it
builds and tests on Linux with the swift.org toolchain:

```sh
export PATH="$HOME/projects/forkmesh/toolchain/swift/usr/bin:$PATH"   # swift.org 6.3
cd native/ios/ForkMeshCore
swift test                                   # every module
FM_ONLY=FMCrypto swift test                  # one module, its dependencies and its tests
../tools/check_swift_syntax.sh               # parse (not type-check) the SwiftUI sources
```

macOS notes:

- On a Mac, `swift test` builds ForkMeshCore for macOS 15. Debug builds with
  Swift 6.2/6.3 can abort xctest with "freed pointer was not the last
  allocation" in async code (a toolchain issue with back-deployed concurrency
  thunks). CI therefore runs the core tests as
  `swift test -c release -Xswiftc -enable-testing --scratch-path .build-release`,
  with `NSUnbufferedIO=YES` so a crash log names the failing test, and a
  30-minute kill so a hang still publishes its log.
- `GitNetHttpTransport` polls (every 50 ms) for cancellation, deadline and
  idle-read timeout instead of using `withTaskCancellationHandler`, which
  avoids the same abort in iOS 17 debug builds.
- `LocalModelStore.availableCapacity` falls back to `systemFreeSize` when
  `volumeAvailableCapacityForImportantUsage` reports 0 (as it does for a
  process without a login session, such as a build over SSH).
- The CI checkout must include `/conformance/`, because
  `AgentTranscriptFormatTests` reads `conformance/agent-transcript/v1/`.

Tests that run the real `git` (`GitBinary` in FMGitTests, `LocalTestGit` in
FMCodeTests) give it an empty private `HOME`, no system or global config and a
fixed identity, and drain stdout and stderr concurrently. Tests that wait for a
download to reach N partial bytes watch the single file being downloaded, not
the aggregate `partialBytes(for:)`.

### Codemagic

The app target (SwiftUI, Keychain, URLSession WebSocket, the extensions) only
compiles on macOS, which happens on Codemagic (`/codemagic.yaml`, started by
hand, `working_directory: native/ios`, `mac_mini_m2`):

| Workflow | What it does |
| --- | --- |
| `ios-native-build` | Generates the project, installs Xcode 26's Metal toolchain and resolves the Swift packages (`native_packages`), builds Debug for the simulator, archives Release unsigned, zips `ForkMesh-native.app.zip`, builds the watch app for the watchOS simulator, then runs the `ForkMeshCore` tests last so a core test failure cannot hide whether the app compiles. |
| `ios-native-release` | Registers the bundle ids (`tools/asc_bundle_ids.py ensure`), picks the next App Store Connect build number, cloud-signs at export, uploads to TestFlight (`app_store_connect` variable group). With `FORKMESH_IOS_APP_STORE=1`, which a tagged release sets, it also submits the build for App Store review ([IOS_RELEASE.md](../../docs/native/IOS_RELEASE.md)). Stops before the archive if the app record does not exist. |
| `ios-native-adhoc` | Ad hoc IPA for the devices registered in the developer account. |

From 2026-09-24 to 2026-10-15 `ios-native-build` ran a merged diagnostic
script ("Cancel probes") that compiled nothing and always exited 0, so the job
was green whatever the app's state. `app/tests/test_codemagic_native_build.py`,
in the critical worker suite, now fails if the job stops building the app or
ends in an unconditional `exit 0`.

The version is `Config/Version.xcconfig` (`MARKETING_VERSION`); release
overrides `CURRENT_PROJECT_VERSION` with the next free build number. The same
file carries `FORKMESH_ENV`, `FORKMESH_API_HOST` and `FORKMESH_WORLD_URL` into
the Info.plist. On a Mac:

```sh
cd native/ios
ruby tools/generate_project.rb               # needs the xcodeproj gem
xcodebuild -project ForkMesh.xcodeproj -scheme ForkMesh \
  -destination 'generic/platform=iOS Simulator' build CODE_SIGNING_ALLOWED=NO
```

Toolchain: Swift 5 language mode (tools 5.9), iOS 17.0 deployment target
(Observation), Xcode 16 or newer (file-system synchronized groups, so adding a
source file never edits the project). `FORKMESH_NATIVE_WATCH=0` or
`FORKMESH_NATIVE_WIDGETS=0` leaves the watch app or widgets out of a build.

## Home Screen widgets

`ForkMeshWidgets` never calls the server. The app writes one `SurfaceSnapshot`
(FMSurfaces) to the keychain group it shares with its extensions
(`SurfaceSnapshotStore.shared()`); every widget reads it, and
`SurfacesCoordinator` rewrites it and reloads only the widget kinds whose
section changed. `SurfaceSnapshot.currentVersion` is 2; an older file reads as
no snapshot until the app writes the next one.

The **Overview** widget (`com.forkmesh.widget.overview`; small, medium, Lock
Screen rectangular and inline) shows six counts:

| Count | Source |
|---|---|
| Agents | every run the Agents tab lists (`AgentsSurface.total`) |
| Running | running and waiting runs (`AgentsSurface.running`) |
| Issues, PRs | open issues and pull requests in the account's own repositories (newest 20 from `/api/repositories`, each read through `/api/repo/<owner>/<name>/issues` and `/pulls`) |
| Tasks | organization board tasks (`/api/tasks`) not done, up to five pages; `N+` when more remain |
| Pings | the inbox's unread count |

The coordinator sums issues and pull requests itself on resume and sign-in, at
most every five minutes, and only while an Overview widget is placed. A count
it could not load shows `–`.

## Watch app

`ForkMeshWatch` (bundle id `com.forkmesh.forkmesh.watchkitapp`) lists the
account's agents, shows each one live, plays its work as sound, taps the wrist
for tool calls, errors and finished runs, and offers Stop, Continue, Run again
and a dictated prompt. The design is `docs/native/PARITY.md`, section 4.

The watch is not a client of its own: the iPhone app's
`Platform/WatchBridge.swift` reads sessions and transcripts, sends compact
`FMWatchWire.AgentFrame`s over WatchConnectivity and carries out the watch's
commands (over the local control channel when a desktop is on the phone's
network, else through the relay queue). `ForkMeshWatchWidgets` reads the last
summary from the watch's keychain group (`WatchGroup.shared()`). The sonifier
is `FMAgentWatch`, linked through the package's `ForkMeshWatchCore` product.
The watch app ships inside the iOS IPA. Neither the watch nor the widgets use
an App Group, which the App Store Connect API cannot provision.

Ad hoc builds install only on registered devices, watches included:
`tools/asc_bundle_ids.py register-device "Name" <UDID>` (`devices` lists them,
`has-watch` reports whether a watch is registered).

## Dependency policy

First-party only, as in `native/android`: Foundation, SwiftUI, CryptoKit,
Network, Security, LocalAuthentication, UserNotifications and zlib from the
OS. No CocoaPods, and one deliberate exception among Swift packages (below).

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
| Icons                    | `FmIcon("name")` over the bundled icon fonts              |

**The exception: on-device models.** The app links
[mlx-swift-lm](https://github.com/ml-explore/mlx-swift-lm) (`MLXLLM`,
`MLXLMCommon`), [mlx-swift](https://github.com/ml-explore/mlx-swift) (`MLX`)
and [swift-transformers](https://github.com/huggingface/swift-transformers)
(`Tokenizers`), each up to the next minor version, with every resolved version
pinned in `Config/Package.resolved` (copied into the project by
`tools/generate_project.rb`). They run Gemma 4 on the phone's GPU, making an
iPhone a local agent node: a private chat, an on-device coding agent and a
model paired peers can use. The exception is fenced in:

- App target only. `ForkMeshCore` stays dependency-free and Linux-tested;
  `FMLocalAI` holds the catalog, downloads, device fit, view models and the
  engine protocol, and `ForkMesh/Features/LocalAI/MLXGemmaEngine.swift` is the
  one file that imports MLX, under `#if canImport(MLXLLM)`. Removing the
  packages leaves a building app whose engine reports itself unavailable. The
  notification extension links none of it.
- Weights are downloaded by `FMLocalAI` over `FMNet` from Hugging Face at
  pinned commits, not by the Hub SDK.
- The packages bring their own transitive dependencies (mlx-swift's C++ and
  Metal sources, swift-jinja, swift-collections, swift-crypto, yyjson,
  swift-huggingface, swift-syntax). Codemagic installs Xcode 26's Metal
  toolchain and resolves them in a separate step (`native_packages`).

Adding any other dependency needs an explicit decision.

## Known gaps

Progress is tracked in [`docs/native/PARITY.md`](../../docs/native/PARITY.md).
Parity workstreams plug in through `AppServices+<Area>.swift` installers so they
never edit `ForkMesh/App/AppServices.swift`.

- **Gemma 4 memory.** E2B (4-bit) needs about 2.6 GB resident, E4B about
  4.2 GB. A real device needs
  `com.apple.developer.kernel.increased-memory-limit` in both app entitlements
  files, after enabling "Increased Memory Limit" for `com.forkmesh.forkmesh` in
  the developer portal. Generation, tool calls and the Metal path are
  type-checked, not yet run on a device.
- **Bluetooth mesh** runs over BLE GATT (`CoreBluetoothMeshTransport`), parse
  checked only. LAN mesh and the desktop control channel use BSD sockets;
  multicast discovery would need the `com.apple.developer.networking.multicast`
  entitlement, while the subnet sweep works without it.

## Keychain items stay on the device

Every keychain item uses a `...ThisDeviceOnly` accessibility class, so no
session, key or vault copy travels in a backup to another phone. The push
session shared with the notification service extension
(`Shared/PushCredentialStore.swift`) uses
`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`, readable while the phone is
locked. `StoreConfigTests.testEveryKeychainItemStaysOnThisDevice` fails on any
other class.

## Bundle ids and the App Store

The app is `com.forkmesh.forkmesh` (`APP_BUNDLE_ID` in
`tools/generate_project.rb`); its extensions are `<app>.NotificationService`,
`<app>.Widgets`, `<app>.Share` and `<app>.AutoFill`, the watch app
`<app>.watchkitapp` and its widgets `<app>.watchkitapp.widgets`.
`app/src/apns.py` also accepts push tokens for the older
`com.forkmesh.nativeapp` topic (`APNS_BUNDLE_IDS`). Releasing is
[`docs/native/IOS_RELEASE.md`](../../docs/native/IOS_RELEASE.md).

## Conventions

- **One app on two platforms.** Keep behaviour, copy, endpoints, storage keys
  and signing formats identical to `native/android`; the relay and the desktop
  cannot tell the apps apart.
- **Register what Android must match.** A change an iPhone user can see adds
  a row to [`docs/native/SYNC.md`](../../docs/native/SYNC.md) in the same pull
  request (`python3 tools/native_sync.py add <sha> "<sentence>" --area <Area>`).
- **Logic in the core, views in the app.** State, networking, parsing,
  validation and view models (`@Observable` classes) go in `ForkMeshCore`. A
  SwiftUI view binds to a view model and lays things out; it does not fetch,
  parse or decide.
- **Core portability.** No `UIKit`, `SwiftUI`, `Combine` or `CryptoKit` outside
  `#if canImport(...)`; `import FoundationNetworking` under
  `#if canImport(FoundationNetworking)`. Platform services reach the core
  through protocols (`KeyValueStore`, `SecureStore`, `HTTPClient`,
  `WebSocketConnector`) implemented in `ForkMesh/Platform`.
- **Conservative SwiftUI.** The app target is compiled only on CI. Use iOS 17
  APIs you are certain of, keep view bodies small, avoid clever generics, and
  run `tools/check_swift_syntax.sh`.
- **Tests** are XCTest, in `Tests/<Module>Tests`, sharing vectors and cases
  with the Kotlin ones (`native/android/core/src/test`).
- **Every message can be copied or handed to an agent.** Alerts, errors,
  warnings, notices and status lines carry copy and send-to-agent icons
  (`Kit/FmMessageTools.swift`; use `FmMessageTools(text)` or
  `.fmMessageTools(text)`). `FmNoticeLine`, `FmEmptyState`, `FmDialog`,
  `FmConnectionBanner` and `FmNeedsDesktopNote` include them; a system
  `.alert` showing an error gets "Copy" and "Send to agent" buttons. Send to
  agent opens the composer with `MessageHandoff.agentPrompt`
  (`ForkMeshCore/Sources/FMApi/Navigation`). Chat and transcript bubbles keep
  their long-press menu (`Kit/FmMessageActions.swift`).
- **Only the home shell shows the composer.** `Route.compose` has no screen
  of its own: `RouteDestinationView(route: .compose(...))` in a sheet shows
  the "New agent is not in this version of the app yet" placeholder. Hand a
  prepared prompt to `FmMessageHandoff.compose(target)`, which closes any
  open sheet and asks the shell's composer to open. The failed Actions run's
  "Send to agent" used its own sheet and showed the placeholder until
  2026-10-15.

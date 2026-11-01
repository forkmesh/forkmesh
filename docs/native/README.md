# Native apps (iOS, watchOS and Android)

Home page for the phone apps; the developer docs index is
[../README.md](../README.md). `native/ios` (Swift + SwiftUI) and
`native/android` (Kotlin + Jetpack Compose) sign in to a ForkMesh instance,
browse and edit repositories, run and watch agents, chat, and carry the
Market's phone tools. A phone is a *mobile node*: it holds its own Ed25519 node
key and owner-encryption key and clones and commits with its own Git engine.
The two apps share endpoints, storage keys and signing formats, so the relay
and the desktop cannot tell them apart. Where the phones sit in the system:
[architecture](../architecture.md); keys and signatures:
[encryption](../encryption.md).

- iOS ships as the App Store app **ForkMesh** (`com.forkmesh.forkmesh`)
  ([IOS_RELEASE.md](IOS_RELEASE.md)).
- The Android release APK (the desktop release's
  `forkmesh-android-universal.apk` asset) is built as `com.forkmesh.forkmesh`;
  local builds default to `com.forkmesh.nativeapp`, which installs beside it
  ([ANDROID.md](ANDROID.md), section 4).
- The Apple Watch app (`native/ios/ForkMeshWatch`) ships inside the iOS app
  and works through the iPhone over WatchConnectivity; a standalone mode is
  phase 2 ([PARITY.md](PARITY.md), section 4).

Public phone guides (Siri, Apple Health, the node contract, agent access, file
editing, the Git engine) are in [docs/mobile/](../mobile/README.md); the user
overview of every app is [apps.md](../apps.md). Per-app READMEs:
[native/ios/README.md](../../native/ios/README.md),
[native/android/README.md](../../native/android/README.md).

## Pages

| Page | Covers |
| --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Code map, shortcuts, design rules, projects, interfaces, configuration, build and test, operations, security, traps |
| [ios/](ios/README.md) | API notes for the iOS core modules (`ForkMeshCore`) |
| [PARITY.md](PARITY.md) | Phone feature matrix, platform decisions, iOS workstream hooks, the watchOS app, bundle ids |
| [DESKTOP_PARITY.md](DESKTOP_PARITY.md) | Parity with the Qt desktop, route contract, desktop control families, extension targets |
| [ANDROID.md](ANDROID.md) | Android design notes, and the "Differences kept on purpose" list |
| [NAVIGATION.md](NAVIGATION.md) | Tabs, routes, Home grid, bars, the tour, sitemap and URLs |
| [THEMES.md](THEMES.md) | Accents and contrast, surface families, the theme editor, the background image |
| [ONBOARDING.md](ONBOARDING.md) | Login and first-run screens, and when each system permission prompt comes up |
| [DATA_FOLDER.md](DATA_FOLDER.md) | The projects folder that keeps settings, plugin data and the encrypted identity key through a reinstall |
| [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md) | Desktop agents on the phone (relay control, diff, merge, delete) |
| [RUNNER_AGENTS.md](RUNNER_AGENTS.md) | Cloud runner agents: branches, merge and clean up, pull requests |
| [AGENT_ATTACHMENTS.md](AGENT_ATTACHMENTS.md) | Photos and files in agent transcripts and prompts |
| [DIFF_REVIEW.md](DIFF_REVIEW.md) | The diff review stream |
| [COMMIT_GRAPH.md](COMMIT_GRAPH.md) | The Commits screen's Graph tab |
| [WORKFLOWS.md](WORKFLOWS.md) | Native Workflows screens and the on-phone engine (JavaScriptCore on iPhone, a Kotlin port on Android) |
| [GENIE.md](GENIE.md) | The on-device voice assistant |
| [NEXUS.md](NEXUS.md) | NEXUS package, permissions, safety policy, host integration and verification |
| [notes.md](notes.md) | Notes: filters, autosave, conflicts, preview, sharing, links, export |
| [RSS_READER_IOS.md](RSS_READER_IOS.md) | The local-only RSS Reader on iPhone |
| [STATS.md](STATS.md) | The Stats board: every desktop tile over `stats.tiles`, and the release-version tiles |
| [CAPTURE_MARKUP.md](CAPTURE_MARKUP.md) | Screen capture and markup in the composer, and selectable text |
| [REFERENCE_LINKS.md](REFERENCE_LINKS.md) | Issue, task, pull request and commit links in messages |
| [opus.md](opus.md) | Opus from a pinned Xiph commit with DRED, deep PLC and OSCE (`native/opus/`): pin, weights, `build.sh`, sizes, smoke tests, linking |
| [OTHER_APP_CALLS.md](OTHER_APP_CALLS.md) | Sounds, listening, ducking and Bluetooth held back while another app's call is up |
| [DESKTOP_PINGS.md](DESKTOP_PINGS.md) | Desktop pings on the phone, and their one-tap quick fixes |
| [PUSH_BANNERS.md](PUSH_BANNERS.md) | Push banner icons, and no banner for what is already on screen |
| [MESH_PUSH.md](MESH_PUSH.md) | Wake pushes and local alerts for the `#mesh` room |
| [DESKTOP_UPDATE.md](DESKTOP_UPDATE.md) | Rebuilding and restarting the desktop from the phone |
| [PERFORMANCE_CHECKS.md](PERFORMANCE_CHECKS.md) | iOS performance rules, reading a diagnostics bundle, and every earlier performance check |
| [IOS_RELEASE.md](IOS_RELEASE.md) | The iOS release pipeline and App Review rules |
| [MAC_BUILD.md](MAC_BUILD.md) | iOS and macOS builds on the office Mac |

## Related

- Core pages: [architecture](../architecture.md),
  [encryption](../encryption.md), [mirroring](../mirroring.md),
  [self-hosting](../self-hosting.md) (the instance the phones sign in to),
  [censorship resilience](../censorship-resilience.md),
  [threat model](../protocol/threat-model.md).
- [Mobile push notifications](../operations/mobile-push-notifications.md)
  (APNs setup on the server).
- Back to the [developer docs index](../README.md).

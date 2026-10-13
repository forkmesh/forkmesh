# Native phone apps: feature matrix, workstreams and the watch app

Status as of 2026-09-24, the snapshot this plan was written from. Later
changes are tracked in [PROGRESS.md](PROGRESS.md): its iOS checklist and its
Android column supersede the status cells below. The plan makes `native/ios`
the iPhone app and `native/android` the Android app, and adds an Apple Watch
app. Row numbers (`#n`) are stable; PROGRESS.md, DESKTOP_PARITY.md and the
source cite them.

Companion documents: [DESKTOP_PARITY.md](DESKTOP_PARITY.md) (parity with the
Qt desktop, workstreams WS8–WS21), [NAVIGATION.md](NAVIGATION.md) (information
architecture) and [PROGRESS.md](PROGRESS.md) (the progress tracker).

Status words, used the same way in every table:

- **Done**: built, and for ForkMeshCore also covered by the Linux XCTests.
  On 2026-09-24 much of the iOS app target had not yet been compiled, so
  "Done" meant the code was written, not that it had run on a device.
- **Partial**: some of the behaviour exists, and the cell says what is missing.
- **Missing**: nothing has been built.
- **N/A**: does not apply to that platform (see section 2).

Effort (S/M/L) is the work left on **iOS**: S is up to 2 days, M up to 2
weeks, L more than that. Paths are relative to the repository root. `NI/` is
`native/ios/`, `NIC/` is `native/ios/ForkMeshCore/Sources/`, and `NA/` is
`native/android/`, where `A/` means `app/src/main/kotlin/com/forkmesh/nativeapp/`
and `C/` means `core/src/main/kotlin/com/forkmesh/nativeapp/`.

## Headline (2026-09-24)

| | iOS native | Android native |
|---|---|---|
| Features compared (rows below, N/A excluded) | 74 | 71 |
| Done | 56 | 19 |
| Partial | 7 | 12 |
| Missing | 11 | 40 |

iOS covered almost every row and added on-device Gemma 4 through MLX. Its
gaps were:

- the projects isolation and the switcher;
- per-project push and code-push pings;
- the ping settings screen;
- the agent Watch/Listen sonifier;
- the composer (photos, dictation, the model catalog).

Android was then mostly the Account area: Code, Chat, Agents and Tools were
`STUB:` placeholders in `A/feature/Features.kt`. The AW workstreams in
PROGRESS.md have since filled the Android column.

---

## 1. Feature matrix

### 1.1 Onboarding, auth and identity

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 1 | Log in with password and TOTP, bound to the device key | **Done**: `NIC/FMAccount/AuthService.swift`, `NI/ForkMesh/Features/Account/AuthFlowView.swift`, `NIC/FMApi/ViewModels/AuthFlowLogic.swift` | **Done**: `A/feature/account/auth/AuthFlow.kt`, `C/api/AuthController.kt` | — | — |
| 2 | Sign up (hands off to the website `/signup`) | **Done**: `AuthSignupPage` in `AuthFlowView.swift` | **Done**: `AuthFlow.kt` | — | — |
| 3 | Guest mode and the guest gate on writes | **Done**: `NI/ForkMesh/Kit/GuestGate.swift` | **Done**: `A/ui/kit/GuestGate.kt`, `C/account/GuestGateLogic.kt` | — | — |
| 4 | Device Ed25519 identity and owner-device enrolment (X25519 + ML-KEM-768), needed to read sealed agent transcripts | **Done**: `NIC/FMCrypto/Identity.swift`, `OwnerCrypto.swift`, `MlKem768.swift`, `NIC/FMAccount/SecurityBootstrap.swift` | **Done**: `C/security/Identity.kt`, `OwnerCrypto.kt`, `A/feature/account/SecurityBootstrap.kt` | — | — |
| 5 | Portable identity recovery kit (core library; no screen on iOS) | **Done** (core only): `NIC/FMCrypto/PortableIdentity.swift` | **Done**, and it goes further with a UI: `A/feature/account/PortableIdentityActions.kt` | — | — |
| 6 | Account nodes, sessions and this-device cards; revoke | **Done**: `NI/ForkMesh/Features/Account/SettingsCards.swift` | **Done**: `A/feature/account/settings/SettingsScreen.kt` | — | — |
| 7 | iOS UDID lookup (`forkmesh://ios-udid`), iOS only | **Done**: `NIC/FMAccount/IosUdid.swift`, `AppServices.openURL` | N/A | — | — |
| 8 | User-presence gate (biometrics before a destructive action) | **Done**: `NIC/FMChat/ViewModels/UserPresence.swift`, `NI/ForkMesh/Platform/LocalAuthVaultDeviceAuth.swift` | **Missing** | — | — |
| 9 | Sign out and delete account (password plus typing `DELETE`) | **Done**: `AuthService.swift`, `SettingsScreen.swift` | **Done**: `SettingsScreen.kt` | — | — |

Passkeys/WebAuthn and QR device linking are not rows; see section 2.3.

### 1.2 Projects (multi-instance: ForkMesh, ForkMesh Next, self-hosted)

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 10 | Project list, active project, probing a new instance, per-instance feature flags | **Done** under the name "relays": `NIC/FMAccount/RelayProfile.swift`, `RelayDirectory.swift`, `RelayFeatures.swift`, `NIC/FMChat/Relay/RelayProbe.swift` | **Missing**: one `serverUrl` (`C/api/ServerUrl.kt`) | — | — |
| 11 | Separate login per project | **Done**: `auth/session/<relayId>` in `AuthService.swift` | **Missing** | — | — |
| 12 | **Full per-project isolation**: node key, owner key, vault device copy, local repos, agent branch records, node-sync rows, prefs; the legacy namespace for the first project, `p/<id>/` for the others | **Partial**: only the session is per relay. `identity/ed25519Seed` and `ownerCrypto/hybridV1` are global (`Identity.swift:17`, `OwnerCrypto.swift:25`), and so are local repos and prefs. No `ProjectScope` exists | **Missing** | M | #10 |
| 13 | Project runtime: every service is rebuilt on a switch rather than re-pointed; removing a project erases its own namespace | **Partial**: `NIC/FMChat/Relay/RelaySwitchController.swift` re-points sockets, node sync, the event channel and push in phases, but does not rebuild or erase | **Missing** | M | #12 |
| 14 | Quick project switcher (account menu, guest top bar, sign-in screens) | **Missing** (switching lives only in `NI/ForkMesh/Features/Chat/RelaySettingsSection.swift`) | **Missing** | S | #13 |
| 15 | Projects card in Settings (add, test, edit, remove, switch) | **Done**: `RelaySettingsSection.swift` | **Partial**: URL edit and reset only (`SettingsScreen.kt`) | — | — |
| 16 | All-networks catalog (repositories from every project side by side) | **Done**: `NIC/FMChat/Relay/CrossRelayCatalog.swift`, `NI/ForkMesh/Features/Chat/RelayNetworksScreen.swift` | **Missing** | — | — |
| 17 | Per-project legal links (terms, privacy, membership) | **Partial**: `NIC/FMAccount/IosUdid.swift` hosts the terms and privacy URLs (no membership link, dropped on purpose 2026-09-26), but the per-project host logic added in `2454591cb` is not built | **Done** (single server) | S | #12 |

### 1.3 Shell, navigation and theme

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 18 | Home shell: Code, Agents and Chat pages; Tools menu (Activity, Notes, Tasks, Calories, Vault, World, Desktop, Projects); bottom prompt bar; bell; avatar | **Done**: `NI/ForkMesh/Shell/HomeShell.swift`, `ShellTopBar.swift`, `ShellPromptBar.swift`, `NIC/FMApi/ViewModels/HomeShellModel.swift` | **Partial**: four tabs, of which Code, Agents and Chat are placeholders (`A/shell/HomeShell.kt`, `A/shell/PlaceholderScreen.kt`) | — | — |
| 19 | Theme, accents, frosted surfaces, design kit | **Done**: `NI/ForkMesh/Theme/*`, `NI/ForkMesh/Kit/*` | **Done**: `A/ui/theme/*`, `A/ui/kit/*` | — | — |
| 20 | World (the `world-general` room plus the external World link) | **Done**: `HomeShell.swift` (`FORKMESH_WORLD_URL`) | **Missing** | — | — |

### 1.4 Code, repositories and Git

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 21 | Repository catalog (filters, sort) | **Done**: `NI/ForkMesh/Features/Code/ReposScreen.swift` | **Missing**: API only (`C/api/ApiService.kt` `repositories`) | — | — |
| 22 | Repository detail: code explorer, go-to-file, highlighted preview; commits; mirrors; releases; about; actions (runs and logs); agents; worktrees | **Done**: `RepoDetailScreen.swift`, `RepoCodeTab.swift`, `RepoFileScreen.swift`, `RepoCommitsTab.swift`, `RepoActivityTabs.swift`, `RepoInfoTabs.swift` | **Missing**: `A/feature/code/CodeFeature.kt` is a placeholder | — | — |
| 23 | Issues: filters, detail, timeline, sub-issues, subscribe, new issue | **Done**: `NI/ForkMesh/Features/Code/Collab/IssueDetailScreen.swift`, `NewIssueScreen.swift`, `RepoIssuesTab.swift`, `NIC/FMCode/Collab/*` | **Partial**: screens built but unreachable (`A/feature/code/collab/IssuesTab.kt`, `IssueDetailScreen.kt`, `NewIssueScreen.kt`) | — | — |
| 24 | Pull requests: detail, files changed and commits, review, timeline, merge plan and merge (branch- and patch-backed) | **Done**: `PullDetailScreen.swift`, `PullChangesScreen.swift`, `RepoPullsTab.swift`, `ApiService+Agents.swift` (`pullMergePlan`, `mergePull`) | **Partial**: `A/feature/code/collab/PullScreens.kt`, unreachable | — | — |
| 25 | Discussions, including federated replies | **Done**: `DiscussionDetailScreen.swift` | **Partial**: `DiscussionScreens.kt`, unreachable | — | — |
| 26 | Branch diff and diff viewer (unified or side by side) | **Done**: `NI/ForkMesh/Features/Code/Collab/BranchDiffScreen.swift`, `DiffReviewStream.swift`, `NIC/FMCode/Collab/DiffReview.swift`, `NIC/FMDiff/*` | **Done**: `feature/code/BranchDiffScreen.kt`, `DiffReviewStream.kt`, `core/.../code/DiffReview.kt` ([DIFF_REVIEW.md](DIFF_REVIEW.md)) | — | — |
| 27 | Quick edit over the API into a proposed change or PR | **Done**: `NI/ForkMesh/Features/LocalRepos/QuickEditScreen.swift`, `NIC/FMDiff/PatchBuilder.swift` | **Missing** | — | — |
| 28 | Code editor with key bar, and the highlighter | **Done**: `NI/ForkMesh/Features/Code/CodeEditorView.swift`, `NIC/FMDiff/CodeHighlighter.swift` | **Missing** | — | — |
| 29 | Local repositories: create, import zip, browse, edit, commit, publish, PR | **Done**: `NI/ForkMesh/Features/LocalRepos/*`, `NIC/FMCode/Local/*`, `NIC/FMGit/*` | **Partial**: Git engine done in `C/git/**`; the UI (`A/feature/localrepos/LocalReposFeature.kt`) is empty | — | #12 (scope repos per project) |
| 30 | Clone to device from the mesh | **Done**: `CloneToDeviceButton.swift`. Also: one copy per repository (the button opens or syncs it), Sync from the repository, the clone button and "On this device", clones badged in the catalog (`native/ios/docs/FMCodeLocal.md`) | **Missing** | — | — |
| 31 | Mirror import (GitHub, GitLab, Codeberg) and import tokens | **Done**: `NI/ForkMesh/Features/Code/MirrorImportSheet.swift` | **Partial**: API plus token fields in Settings | — | — |
| 32 | Signed inbox writes (issues, PRs, comments) | **Done**: `NIC/FMApi/Services/InboxService.swift` | **Done**: `C/api/InboxService.kt` | — | — |
| 33 | Bounties, funding and the donation wallet panels (read-only; no wallet keys) | **Done** for bounties: `NIC/FMApi/Models/Bounty.swift`. The funding and donation-address panels were dropped on purpose 2026-09-26 | **Partial**: Funding and payouts section in Settings only | — | — |
| 34 | Serve as a node (node-sync drain and applier) | **Partial**: `NIC/FMChat/NodeSync/NodeSyncService.swift` and `NodeSyncApplier.swift` exist, but the only `NodeSyncWorkspace` is `InMemoryNodeSyncWorkspace`, so nothing is acknowledged (README "Known gaps") | **Missing**: registration only (`C/api/SyncStatus.kt`) | M | #12 |

### 1.5 Organizations and chat

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 35 | Organizations: list, create, members, teams, repo links, delete | **Done**: `NI/ForkMesh/Features/Account/OrgsScreen.swift`, `OrgDetailScreen.swift` | **Done**: `A/feature/account/orgs/*` | — | — |
| 36 | Chat: channels, roster, DMs (`@`), unread banner, link text | **Done**: `NI/ForkMesh/Features/Chat/ChatScreen.swift`, `NIC/FMChat/Relay/RelayService.swift`, `NIC/FMChat/ViewModels/ChatHub.swift` | **Missing**: `A/feature/chat/ChatFeature.kt` is a stub | — | — |
| 37 | Room crypto and per-device seal | **Done**: `NIC/FMCrypto/RoomCrypto.swift`, `RoomSeal.swift` | **Done** (core): `C/crypto/RoomCrypto.kt`, `RoomSeal.kt` | — | — |
| 38 | Chat persona: speak as the account or a stable "Guest ####", never as the node key | **Done**: `ChatPersona` in FMChat; relay and mesh frames carry `accountKind`/`ownerUser` | **Done**: `ChatPersona` in core chat (0b72a703c) | S | — |
| 39 | Local mesh room over the LAN (multicast beacon, TCP, subnet sweep) | **Done**: `NIC/FMChat/Mesh/*` (subnet sweep; multicast needs an entitlement) | **Missing** | — | — |

### 1.6 Tools: notes, tasks, calories, vault, activity

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 40 | Notes: list, search, editor, visibility, history, share | **Done**: `NI/ForkMesh/Features/Tools/NotesScreen.swift`, `NIC/FMTools/NotesModels.swift` | **Missing** | — | — |
| 41 | Tasks: create, check in, complete, reply | **Done**: `TasksScreen.swift`, `NIC/FMTools/TasksModels.swift` | **Missing** | — | — |
| 42 | Calories: ring, charts, meals, goals, challenges, photo estimate | **Done**: `CaloriesScreen.swift`, `CaloriesSheets.swift`, `CaloriesCharts.swift` | **Missing** | — | #50 (the photo picker is shared) |
| 43 | Password vault: fmv1 suite, unlock, reveal and copy with a 30 s clipboard clear, device-lock quick unlock | **Done**: `VaultScreen.swift`, `VaultSubscreens.swift`, `NIC/FMVault/*` | **Missing** | — | #12 (vault device copy per project) |
| 44 | Activity: network stats, leaderboards, payouts, relay connection log | **Done**: `ActivityScreen.swift`, `NIC/FMTools/ActivityModels.swift`; payout readiness dropped on purpose 2026-09-26 | **Missing** | — | — |
| 45 | Sponsored ads (text only, can be turned off) | **Done**: `NI/ForkMesh/Features/Account/SponsoredAdCard.swift` | **Done**: `SponsoredAdCard.kt` | — | — |

Calendar, CRM, the Market and a wallet/ledger are not rows here: they came
with the desktop-parity work (DESKTOP_PARITY.md, WS15 and WS19). "Plugins" in
this matrix is only the relay feature flag that gates notes, tasks and
calories, and the wallet is the read-only funding panel in #33.

### 1.7 Pings, notifications and push

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 46 | Pings inbox: filters, swipe delete, mark read, context sheet, "start agent from ping" | **Done**: `NotificationsScreen.swift`, `NotificationsAgentHandoff.swift`, `NIC/FMApi/Services/NotificationService.swift` | **Done**: `A/feature/account/notifications/NotificationsScreen.kt` | — | — |
| 47 | Ping settings screen: per-kind switches, pause, quiet hours, email, push card with test push, admin monitor alerts | **Done** (WS2, not yet compiled): `NIC/FMApi/Models/PingSettings.swift`, `NIC/FMApi/ViewModels/PingSettingsModel.swift`, `NI/ForkMesh/Features/Account/PingSettingsScreen.swift`, opened from the gear in `NotificationsScreen` and from the "Ping settings" row under Settings → Notifications (issue #872; `showsPingSettings` in `SettingsLogic.swift`, signed-in non-guest accounts only, as on Android) | **Done**: `PingSettingsScreen.kt` (853 lines) | M | — |
| 48 | Code-push pings (`codePushes` switch; a push opens the commit or branch) | **Done** (WS2): "Code pushes" switch; `PushOpen.isCodePush`; `NotificationDeepLink` parses `commits/<branch>` into `branch` (selecting that branch in the commits tab is still FMCode work) | **Missing** | S | #47 |
| 49 | Live account events (`/api/nodes/events` doorbell) and the agent-list doorbell (`agents`, `cloud-agents` topics) | **Done**: `NIC/FMApi/Services/AccountEventChannel.swift`, `AgentListSignal` | **Done**: `C/api/AccountEventLoop.kt`, `A/feature/account/AccountEventChannel.kt` | — | — |
| 50 | APNs register, unregister, environment, test push, badge, pending tap | **Done** (WS2): client complete (`NIC/FMApi/Services/PushNotificationService.swift`, `NI/ForkMesh/Platform/ApplePushBridge.swift`); the server now sends each token under the bundle id it registered with (`apns_device_tokens.bundle_id`, 5.2) | **Missing**: no FCM, by design (`A/feature/account/PushNotificationService.kt`) | S (server) | — |
| 51 | Notification service extension: `GET /api/push/alert`, portrait, badge, de-duplication; a direct message's sender portrait from `GET /api/accounts/<sender>` (the push's top-level `sender`, 2026-09-29) | **Done**: `NI/NotificationService/NotificationService.swift` | N/A (no push) | — | #50 |
| 52 | **Per-project push**: one keychain item per project host; the extension picks the project named in the push; a tap switches project | **Done** (WS2): per-host keychain items, extension picks the push's `origin`, per-project registration (`attach`), a tap switches project (`AppServices+Push.swift`); project id read from `settings.relays` until WS1's `ProjectScope` lands | **Missing** | M | #12, #50 |
| 53 | In-app deep links from a ping (repo, tab, number, commit, branch) | **Done**: `NIC/FMApi/Models/NotificationDeepLink.swift`, `HomeShell.swift` | **Partial**: `C/model/NotificationDeepLink.kt`; its target is a placeholder | — | — |

### 1.8 Agents

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 54 | Agents tab: one Agents list (each row marked Desktop or Cloud) and a New segment; session detail; sealed-session notice | **Done**: `NI/ForkMesh/Features/Agents/AgentsScreen.swift`, `AgentSessionScreens.swift`, `NIC/FMAgents/AgentsListModel.swift` | **Missing**: `A/feature/agents/AgentsFeature.kt` is a placeholder; the API is partial | — | — |
| 55 | Queue a run on a desktop or mirror; steer, retry, cancel (queued only); task-chain stages | **Done**: `AgentStartCard.swift`, `NIC/FMAgents/AgentStartModel.swift`, `NIC/FMApi/Services/ApiService+Agents.swift` | **Missing** | — | — |
| 56 | Coding agent on the phone, using the user's own Anthropic or OpenAI key against a local repository | **Done**: `AgentScreen.swift`, `NIC/FMAgents/CodingAgent.swift`, `AgentProvider.swift`, `AgentTools.swift`, `AgentSessionService.swift` | **Missing** | — | — |
| 57 | Model catalog (Opus 5.5 default; Opus 5, Sonnet 5, Haiku 4.5, Fable 5; GPT-5.6 Terra, Sol and Luna) | **Done** (WS3): `NIC/FMAgents/AgentModelCatalog.swift` (queued runs, chip), `AgentCredentials.swift` (Opus 5.5 first on the device agent) | **Missing** | — | — |
| 58 | Provider-login terminal (pty streamed over the event socket) | **Done**: `NIC/FMAgents/AgentTerminal.swift`, `AgentLoginTerminalScreen.swift` | **Missing** | — | — |
| 59 | Desktop on this network: pairing code, start, prompt, stop, continue, transcript, send a file | **Done**: `NIC/FMChat/LocalControl/LocalControlService.swift`, `LocalControlProtocol.swift`, `NI/ForkMesh/Features/Chat/LocalDesktopScreen.swift` | **Missing** | — | — |
| 60 | **Agent Watch screen** (field of bodies, faces, figures, dot display, sound list with the two switches) | **Missing** | **Missing** | L | #61, #62 |
| 61 | **Listen sonifier engine**: motions, voices on the D-major pentatonic grid, marimba synth with ping-pong echo, cadences | **Missing** | **Missing** | M | — |
| 62 | Sound and haptics output (AVAudioEngine voice pool; Core Haptics pulses) | **Missing**: `NI/ForkMesh/Theme/FmHaptics.swift` has only UIKit taps | **Missing** | S | — |
| 63 | Announcer (sportscaster text-to-speech; desktop only, Piper) | N/A | N/A | — | — |
| 64 | On-device Gemma 4 (E2B/E4B) through MLX | Native only: `NI/ForkMesh/Features/LocalAI/MLXGemmaEngine.swift`, `NIC/FMLocalAI/*`, `NIC/FMAgents/GemmaAgentProvider.swift` | N/A | — | See 2.2 |

"Cloud handoff" as a feature exists on neither platform. The Cloud segment in #54
is queued relay and mirror runs, and it is built. Tool-call **approvals**
existed nowhere on 2026-09-24: not on the relay and not in the LAN control
protocol (WS10 later built them over desktop control, DESKTOP_PARITY 2.13). `NIC/FMApi/ViewModels/SettingsLogic.swift:405` keeps remote desktop
commands locked "until a desktop approval flow is added".

### 1.9 Composer

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 65 | One floating frosted composer for every agent prompt and chat: growing field, "+", model chip, send/stop, `/` commands, extra controls (no mic button since 2026-09-27; dictation is the `/dictate` command); surfaces that cannot take photos hide "+" | **Done** (WS3, awaiting its first Codemagic type-check): `NI/ForkMesh/Kit/PromptComposer.swift`; drives the shell (`Shell/ShellPromptBar.swift`, opened from the bottom bar's prompt button), `AgentScreen.swift` and queued-session follow-ups. Send starts the agent at once (`NIC/FMAgents/QuickAgentLaunch.swift`) | **Missing** | — | — |
| 66 | Photo attachments: camera or library; downscale to 2048 px; JPEG q80 with all metadata stripped; sent as image blocks to the model | **Done** (WS3): `NI/ForkMesh/Platform/PromptPhotoPicker.swift` (PhotosPicker, camera, Files; ImageIO), `NIC/FMAgents/PromptAttachments.swift` (strip, sizes), `NIC/FMApi/Models/PromptAttachment.swift` (wire, limits) | **Missing** | — | #65 |
| 67 | Dictation (the composer's `/dictate` command, and on iOS the shell prompt listening as it opens; listens only while it is on; nothing written to disk) | **Done** (WS3): `NI/ForkMesh/Platform/PromptDictation.swift` (Speech, on-device when supported; SpeechAnalyzer on iOS 26, see "Dictation on iOS" below) | **Missing** | — | #65 |
| 68 | Image blocks through the providers and the session queue | **Done** (WS3): `AgentProvider.swift`, `CodingAgent.swift`, `AgentSessionService.swift`; queued runs send `attachments` | **Missing** | — | #66 |

### 1.10 Status, settings, diagnostics and cross-cutting

| # | Feature | iOS native | Android native | Effort | Depends on |
|---|---|---|---|---|---|
| 69 | User status and presence (away, emoji, text, expiry, dot) | **Done**: `UserStatusSheet.swift`, `UserStatusWidgets.swift`, `NIC/FMApi/Services/UserStatusService.swift`, `NI/ForkMesh/Kit/ConnectionDot.swift`; the member's own emoji sits left of the top-bar avatar as on the web header (`NI/ForkMesh/Shell/ShellTopBar.swift`) | **Done**: `A/feature/account/settings/UserStatusControls.kt`; own emoji left of the top-bar avatar (`A/shell/HomeShell.kt`), read at sign-in, not only when Settings opens | — | — |
| 70 | Settings: profile, avatar, payout address, this device, serve as node, room passphrase, import tokens, orgs, membership, ads, appearance, legal | **Done**: `SettingsScreen.swift`, `SettingsCards.swift`, `NIC/FMAccount/SettingsService.swift`; payout address and membership dropped on purpose 2026-09-26 | **Done**: `SettingsScreen.kt` | — | — |
| 71 | Telemetry screen and strip, log categories | **Done**: `NIC/FMTools/AppTelemetry.swift`, `TelemetryScreen.swift`, `TelemetryStrip.swift` | **Missing** | — | — |
| 72 | Performance monitor, debug overlay, stall log | **Done**: `NIC/FMAccount/PerformanceMonitor.swift`, `StallLog.swift`, `DevPerformanceOverlay.swift` | **Missing** | — | — |
| 73 | Bug report button and sheet (`POST /api/bug-reports`) | **Done**: `BugReportButton.swift`, `ReportBugSheet.swift` | **Partial**: API only | — | — |
| 74 | Trust lists v1 resolver (core library) | **Done**: `NIC/FMAccount/TrustLists.swift` | **Done**: `C/security/TrustLists.kt` | — | — |
| 75 | Accessibility labels | **Done**: 77 `accessibilityLabel` in 39 files; VoiceOver announcements in `Kit/FmLoading.swift`, `FmSurfaces.swift`. A device pass with VoiceOver and Dynamic Type is still owed after the first compile | **Partial**: about 120 `contentDescription`/`semantics` | S | Compile gate |
| 76 | Privacy manifest | **Done**: `NI/ForkMesh/Resources/PrivacyInfo.xcprivacy`, `NI/NotificationService/PrivacyInfo.xcprivacy`; re-check the store privacy disclosures once photos and mic land (IOS_RELEASE.md, "Open items"); no Payment Info since the payout address went (2026-09-26) | N/A | S | #66, #67 |

These have no row: localization (hard-coded English; iOS emits loc strings
through `SWIFT_EMIT_LOC_STRINGS=YES`), third-party analytics (none by
design), and background work on iOS (no BGTaskScheduler; background delivery
is APNs plus the extension; Android's background connection is in ANDROID.md
section 6). Home-screen widgets, a share extension and Live Activities came
later with WS12 (DESKTOP_PARITY.md). Rows 63 and 64 are not counted in the
headline. Rows 7, 51 and 76 are counted for iOS only.

---

## 2. Platform limits and decisions

### 2.1 Android-only, with no direct iOS equivalent

| Item | Why it cannot move as-is | Proposed alternative |
|---|---|---|
| **Bluetooth mesh transport** (RFCOMM links, multicast lock) | iOS gives third-party apps no Bluetooth Classic/RFCOMM (MFi only), so it cannot talk to the desktop's RFCOMM service UUID | Done 2026-09-28: a BLE GATT transport. The iPhone advertises and scans (`FMChat/Mesh/CoreBluetoothMeshTransport.swift`); the desktop (`BleMeshLink`) and Android (`AndroidMeshTransport`) dial it as BLE centrals. Wire format: [networking.md](../desktop/networking.md#over-bluetooth). Pending a device check |
| **Vault autofill** into other apps | Android `AutofillService` | A new iOS **AutoFill Credential Provider extension** (`ASCredentialProviderViewController`) with its own bundle id, App Group and keychain group, reusing `NIC/FMVault`. New work, M–L (WS21 builds it). `NIC/FMVault/VaultSettingsModel.swift` carried the "unsupported" copy until then |
| Wi-Fi multicast discovery (Android takes a multicast lock) | iOS needs the `com.apple.developer.networking.multicast` entitlement, which Apple grants by request | The subnet sweep already works without it. Request the entitlement only if discovery is too slow |
| `PROCESS_TEXT` query and the logcat stall path | Android platform features | None needed on iOS |
| Composer photo picking, re-encoding and dictation | Need platform frameworks | `PhotosPicker` and `UIImagePickerController(.camera)`, ImageIO (`CGImageDestination` with no metadata properties, 2048 px max), `SFSpeechRecognizer` with `AVAudioEngine`. All first-party, so they fit the no-dependency policy in `NI/README.md` |

### 2.2 Limits the platform imposes

| Item | Reason | Proposed alternative |
|---|---|---|
| **On-device Gemma memory** (native-only feature) | E2B 4-bit needs about 2.6 GB resident and E4B about 4.2 GB. iOS jetsams without `com.apple.developer.kernel.increased-memory-limit`, which is not yet in `NI/Config/ForkMesh-*.entitlements`. E4B is unrealistic below 8 GB devices | Enable "Increased Memory Limit" for the App ID, add the entitlement to both app entitlements files, gate E4B on `os_proc_available_memory()` in `NIC/FMLocalAI` device-fit, and keep E2B as the default |
| No JIT / no downloaded code | App Review forbids executing downloaded code | Nothing in the app does this. MLX runs precompiled Metal kernels, which is fine. Model **weights** are data and allowed |
| Background execution | iOS suspends the app. The event socket, node sync ("serve as node") and LAN control stop in the background | Keep behaviour identical: foreground only, APNs to wake. Optional later: a `BGProcessingTask` for node sync on charge, and an APNs `agents` topic (5.3) |
| On-device coding agent on long tasks | Same suspension. A run dies when the app backgrounds for more than about 30 s | `beginBackgroundTask` for the tail of a turn; tell the user runs pause in the background |
| Agent-run approvals | Implemented for desktop agents over direct or relayed desktop control: `agentops.approvals`, one-use `prepare`/`answer`, phone authentication before approval, and a signed `agent_approval` ping | Remaining limit: the desktop must be online to accept the answer; revocation and timeout fail closed |
| **Stop** of a running session away from LAN | Implemented for desktop runs through relayed `agents.stop`; current phone clients also use the durable cloud stop route for claimed mirror sessions | Remaining limit: direct desktop control needs the desktop online; for a cloud run, only the target node can end an already-running provider process after it picks up the queued stop |

### 2.3 Outside the first scope

These were in the brief but outside the first feature set:

- Passkeys/WebAuthn and QR device linking.
- Calendar, CRM, marketplace/plugins, and a wallet/ledger beyond the read-only
  funding panels.
- The announcer. It is desktop-only (Piper TTS). An iOS or watch version
  would use `AVSpeechSynthesizer`; see section 4.
- Home-screen widgets, a share extension, Live Activities, and localization.

Recommendation: **out of the first scope**. Passkeys and a Live Activity for
a running agent are the best-value follow-ups. Calendar, CRM, the Market and
the ledger later came with WS15 and WS19; widgets, the share extension and
Live Activities with WS12.

---

## 3. iOS work breakdown

### 3.0 Gate: first compile (do this before the workstreams)

The app target has never been type-checked. Run `ios-native-build` on
Codemagic and fix every compile error on one branch, before any workstream
starts. This touches files everywhere, so it cannot run in parallel with
anything. It also sets the MLX Package.resolved (README "Known gaps").
Effort: M.

`NI/ForkMesh/App/AppServices.swift` is the one file every area registers
into. It has one hook method per workstream: `installProjects()`,
`installPush()`, `installComposer()`, `installAgentWatch()`,
`installWatchBridge()`. Swift cannot override a method from an extension of
the same type, so each hook forwards to an installer type that conforms to
`AppServicesInstaller` (`func install(into services: AppServices)`, a no-op
by default) and lives in its own file:

| Hook | Installer type | File (owner) |
|---|---|---|
| `installProjects()` | `ProjectsInstaller` | `NI/ForkMesh/App/AppServices+Projects.swift` (WS1) |
| `installPush()` | `PushInstaller` | `NI/ForkMesh/App/AppServices+Push.swift` (WS2) |
| `installComposer()` | `ComposerInstaller` | `NI/ForkMesh/App/AppServices+Composer.swift` (WS3) |
| `installAgentWatch()` | `AgentWatchInstaller` | `NI/ForkMesh/App/AppServices+AgentWatch.swift` (WS4) |
| `installWatchBridge()` | `WatchBridgeInstaller` | `NI/ForkMesh/App/AppServices+WatchBridge.swift` (watch bridge) |

Each file starts as an empty type (`struct PushInstaller:
AppServicesInstaller {}`); the workstream adds `install(into:)` there and
never edits `AppServices.swift`. An installer sees everything on
`AppServices` that is not `private`. `AppServices.start` calls
`installProjects()` before `registerFeatures()`, because the project scope
decides the storage namespace, and the other four after `wire()`, in the
order above, so they can extend or replace the base wiring. The workstreams
below have **no file overlap**, and each file is owned by exactly one.

### WS1: Projects and multi-instance isolation (shared infrastructure, first)

Covers rows #12, #13, #14 and #17: `ProjectScope`, `ProjectStorage` and the
switch-by-rebuild model.

Legacy-namespace rule: the first project keeps the unprefixed keys, the
others use `p/<id>/`. Byte-compatible key names matter for the migration in
section 5.

Owns:

- `NIC/FMAccount/`: new `ProjectScope.swift` and `ProjectStorage.swift`, plus `RelayProfile.swift`, `RelayDirectory.swift`, `AuthService.swift`, `SettingsService.swift`, `FallbackSecureStore.swift`, `IosUdid.swift` (legal links).
- `NIC/FMChat/Relay/RelaySwitchController.swift`, `RelayProbe.swift`, `CrossRelayCatalog.swift`; `NIC/FMChat/ViewModels/RelayModels.swift`.
- `NI/ForkMesh/Platform/KeychainStore.swift`, `UserDefaultsStore.swift`.
- `NI/ForkMesh/App/AppServices+Projects.swift` (new), `NI/ForkMesh/App/ForkMeshApp.swift` (rebuild the service graph on a switch).
- `NI/ForkMesh/Kit/ProjectSwitcher.swift` (new), `NI/ForkMesh/Shell/ShellTopBar.swift`, `NI/ForkMesh/Features/Account/AuthFlowView.swift`, `NI/ForkMesh/Features/Chat/RelaySettingsSection.swift`, `RelayNetworksScreen.swift`.
- Tests: `native/ios/ForkMeshCore/Tests/FMAccountTests/Project*Tests.swift` (new).

Exports `ProjectScope` (id, namespace, secure view, directory), which WS2 and
WS5 consume. The contract is fixed on day 1 so they can start at once.
Effort: L.

### WS2: Push, pings and code-push (shared infrastructure)

Covers rows #47, #48, #50 and #52, and the server fix from 5.2.

Owns:

- `NI/Shared/PushCredentialStore.swift` (per-host items, under the names the Flutter app wrote).
- `NI/NotificationService/NotificationService.swift`.
- `NI/ForkMesh/App/AppDelegate.swift`, `NI/ForkMesh/Platform/ApplePushBridge.swift`, `NI/ForkMesh/App/AppServices+Push.swift` (new).
- `NIC/FMApi/Services/PushNotificationService.swift`, `NotificationService.swift`.
- `NIC/FMApi/Models/NotificationDeepLink.swift`, `PingSettings.swift` (new).
- `NIC/FMApi/ViewModels/NotificationsLogic.swift`, `NotificationsScreenModel.swift`, `PingSettingsModel.swift` (new).
- `NI/ForkMesh/Features/Account/NotificationsScreen.swift`, `PingSettingsScreen.swift` (new), `NotificationsAgentHandoff.swift`.
- Server: `app/src/apns.py` and `register_apns_device_token`/`_apns_send_tokens` in `app/src/entry.py` (store and send a per-token topic), with `app/tests/test_apns_push.py`.

Effort: M.

### Dictation on iOS

- The shell prompt (the bottom bar's prompt button) starts dictation as it
  opens. Settings, Voice, "Listen when the prompt opens"
  (`voice/dictateOnOpen`, `SettingsService.dictatesOnOpen`) turns that off; it
  is on by default. Other composers (the agent screen, queued follow-ups) only
  listen on `/dictate`, because they are on screen without being opened.
- Listening stops after 8 seconds with no words, 4 seconds of quiet after
  words, or two minutes in all. Typing (any edit the recognizer did not
  make), sending, closing the prompt or tapping the red "Listening" chip
  stops it at once. While it listens, a late result can no longer overwrite
  what the person typed or bring back a sent prompt.
- When listening ends on the 4 seconds of quiet after words, the composer
  sends the prompt (since 2026-10-02): speak, pause, and it goes.
  `PromptDictation` reports `onDone(true)` only for that quiet stop, and
  `PromptComposer` sends only if the recognizer added words this time. The
  8-second no-words stop, the two-minute limit, the chip, typing and closing
  the prompt end listening without sending.
- A person who refused the microphone or speech recognition is not asked
  again or shown an error on every open; `/dictate` still explains.
- The audio session is `playAndRecord` with `duckOthers`, so opening the
  prompt lowers music instead of stopping it. Each start builds a fresh
  `AVAudioEngine` after the session switches, so the microphone tap takes
  the current hardware format; a reused engine kept the format of an
  earlier session and AVAudioEngine aborted the app on the mismatch
  (`docs/native/README.md`, 2026-10-02 Agents tab root report). The Watch
  sounds leave the session alone while dictation listens.
- Engine: on iOS 26, Apple's on-device `SpeechAnalyzer` with a
  `SpeechTranscriber` for the chosen language (`PromptSpeechAnalysis`). Its
  language model is a system asset: the app installs it through
  `AssetInventory` five seconds after launch, and the Voice settings card
  shows whether it is installed with an "Install speech model" button. Until
  the model is installed, the language has no model, or on iOS 17 to 25,
  dictation uses `SFSpeechRecognizer` (on the device when it can), as before.
  Microphone buffers are converted to the analyzer's format in memory
  (`PromptAnalyzerFeed`); nothing is written to disk.

### WS3: Composer, photos, dictation and model catalog

Covers rows #57 and #65–#68.

Owns:

- `NI/ForkMesh/Kit/PromptComposer.swift` (new).
- `NI/ForkMesh/Platform/PromptPhotoPicker.swift` and `PromptDictation.swift` (new; PhotosUI, ImageIO, Speech).
- `NI/ForkMesh/Shell/ShellPromptBar.swift`, `NI/ForkMesh/Features/Agents/AgentScreen.swift`, `AgentSetupSheet.swift`, `NI/ForkMesh/Features/Chat/ChatScreen.swift`, `ComposeIdentityBar.swift`.
- `NIC/FMAgents/AgentModelCatalog.swift` (new), `AgentCredentials.swift`, `AgentProvider.swift`, `CodingAgent.swift`, `AgentSessionService.swift`, `AgentScreenModel.swift`.
- `NIC/FMAgents/PromptAttachments.swift` (new: size and strip rules, testable on Linux).
- `NI/Config/ForkMesh-Info.plist` (camera, photo, mic and speech strings), `NI/ForkMesh/Resources/PrivacyInfo.xcprivacy`.
- `NI/ForkMesh/App/AppServices+Composer.swift` (new).

Also fold in #38 (chat persona) through `NIC/FMChat/ViewModels/ChatHub.swift`,
which this workstream owns. Effort: M.

The composer card and its `/` menu lay `sheetSurface` over the system blur
(78% in light, 72% in dark, as the Dart `_ComposerPalette.surface`), and the
shell's backdrop behind the open composer is 12% black in light mode and 28%
in dark (`ShellPromptBar.backdropOpacity`). `.ultraThinMaterial` alone over a
28% backdrop turned the open composer as grey as the page in light mode
(#CBCBCB on #B8B8B8, PR #158), so nothing looked focused.

### WS4: Agent Watch and Listen on the iPhone (also the base for the watch app)

Covers rows #60–#62.

Owns:

- New module `NIC/FMAgentWatch/`: `AgentMotion.swift`, `AgentVoice.swift`, `AgentSynth.swift`, `AgentFeel.swift`, `AgentWatchController.swift`, `AgentWatchBeats.swift`. Depends on FMBase only, so it builds on watchOS and Linux.
- Tests `native/ios/ForkMeshCore/Tests/FMAgentWatchTests/` (golden PCM and note tests).
- `native/ios/ForkMeshCore/Package.swift` (add the module and the `.watchOS(.v10)` platform).
- `NIC/FMAgents/AgentWatchTargets.swift` (new: relay and LAN targets).
- `NI/ForkMesh/Features/AgentWatch/` (new): `AgentWatchScreen.swift`, `AgentWatchField.swift`, `AgentWatchFigures.swift`, `AgentWatchSoundList.swift`.
- `NI/ForkMesh/Platform/AgentWatchOutput.swift` (new: AVAudioEngine plus Core Haptics).
- Entry points `NI/ForkMesh/Features/Agents/AgentsScreen.swift`, `AgentSessionScreens.swift`, `NI/ForkMesh/Features/Chat/LocalDesktopScreen.swift`; `NI/ForkMesh/App/AppServices+AgentWatch.swift`.

Effort: L (the field renderer is most of it).

### WS5: Serve as a node, and the Git engine move

Covers row #34.

Owns:

- `NIC/FMChat/NodeSync/*`.
- `NIC/FMCode/Local/*`, including the `NodeSyncWorkspace` adapter, per-project roots from WS1's `ProjectScope`, and moving `FMCode/Local/Engine` into FMGit.
- `NIC/FMGit/*`.
- `NI/ForkMesh/Features/LocalRepos/*`.

Effort: M.

### WS6: watchOS app and build pipeline

Covers section 4.

Owns:

- New `NI/ForkMeshWatch/**` and `NI/ForkMeshWatchWidgets/**`.
- New `NIC/FMWatchWire/` with tests.
- New `NI/ForkMesh/Platform/WatchBridge.swift` and `NI/ForkMesh/App/AppServices+WatchBridge.swift`.
- New `NI/Config/ForkMeshWatch-Info.plist`, `ForkMeshWatch.entitlements`, `ForkMeshWatchWidgets-Info.plist`, `ForkMeshWatchWidgets.entitlements`.
- `NI/tools/generate_project.rb`, `NI/tools/asc_bundle_ids.py`, `codemagic.yaml` (native section).

Depends on WS4's FMAgentWatch; a thin stub module is fine until it lands.
Effort: L.

### WS7: Hosts, live host logs, and agents over the relay

Beyond the first feature set: the desktop's Hosts page and agent control, from
the phone, on the LAN and away from home.

Design:

- **One channel, two transports.** The LAN local control channel
  (`desktop/src/LocalControl.h`) also rides the relay: the phone holds a
  browser-role account event socket, the desktop its node-role socket, and the
  per-owner `ForkMeshNodes` object forwards `{"type":"control"}` chunks between
  them (`app/src/node_control.py`, advertised as `relays: ["terminal",
  "control"]`, metered on its own window). Inside run the same hello, welcome,
  signed transcripts and AES-256-GCM frames as on the LAN, so the relay sees
  only ciphertext. A phone signed in to the desktop's account is approved on
  either transport without a code (2026-09-27); any other phone is approved
  only on the LAN, a relayed channel from it is refused with `pair_on_lan`, and
  the phone accepts only the desktop key it paired with.
- **Hosts** (`hosts.list|detail|options|prepare|run|logs.start|logs.ack|
  logs.stop`, desktop `MainWindowLocalControlHosts.cpp`): owner-only (the
  approved device must name the desktop's account); every change needs a
  one-use, one-minute token from `hosts.prepare` bound to device, action and
  host, and destroy also the typed host name; every change is logged on the
  desktop. The operations are the Hosts page's own paths (installer, the
  mirror-node SSH transport, Vultr create/destroy); keys never leave the
  desktop. Logs tap the desktop's existing host log feed, in acknowledged
  batches (window 4, 2000-line buffer, drops counted).
- **Host SSH terminal and Install (binary)** (2026-10-06, the request "fully
  manage all hosts, everything the desktop does, and add an SSH terminal"):
  a host row's `actions` now also lists `installBinary` (the row's
  "Install (binary)" button: `runHostInstall(true)`, uploading the desktop's
  own build) and, on every desktop but Windows, `terminal`. Both go through
  `hosts.prepare`/`hosts.run` like any change. `hosts.run` with
  `action: "terminal"` (plus `cols`/`rows`) starts `ssh -tt user@host` with
  no remote command, i.e. the account's login shell, in a pty on the desktop
  (`startLocalControlHostTerminal`, `buildHostLoginShellSshCommand`): the
  saved host's managed key or session password, the ForkMesh known_hosts in
  accept-new mode, `BatchMode` without a password. It answers the
  `terminal.*` object (`provider` and `mode` `"ssh"`, `host`, `label`
  "SSH to <host>"), and the phone attaches to it with the existing
  `terminal.attach|input|resize|detach|close` methods and
  `terminal.output`/`terminal.exit` events, so the SSH key and password
  never leave the desktop and the relay only carries ciphertext.
  `terminal.start` refuses `ssh`: a shell only opens through a fresh
  confirmation. Mode `"ssh"` keeps the session out of the agents list; the
  host page lists its open sessions from `terminal.list` (filtered by
  `host`) so a phone can rejoin or close one. Leaving the screen detaches and
  keeps the shell open, like an agent terminal; the desktop caps all
  terminals at six. Older desktops list neither action, so the phones show
  neither button.
- **Agents**: the existing `agents.*` methods work over the relay unchanged;
  `agents.transcript` takes `since` for live deltas. Node runs come from the
  relay queue (`GET /api/account/agent-sessions`), with the new
  `POST .../agent-sessions/<sid> {"action":"stop"}` (a `kind:"stop"` job the
  Go mirror node and the desktop honour).

Owns:

- `NIC/FMControl/` (new: `DesktopControlLink`, `RelayControlConnection`,
  `DesktopControlHub`, `AgentControl`), `NIC/FMHosts/` (new), their tests, and
  `NIC/FMApi/Services/ApiService+RemoteControl.swift` (new).
- `NI/ForkMesh/Features/Hosts/**`, `NI/ForkMesh/Features/Agents/AllAgents/**`,
  `NI/ForkMesh/App/AppServices+Hosts.swift` (new).

Small additions outside its files, noted for their owners:

- `native/ios/ForkMeshCore/Package.swift` (WS4): two `Module(...)` lines.
- `NIC/FMChat/LocalControl/LocalControlService.swift`: public `call(_:_:)` and
  `addEventTap`/`removeEventTap`.
- **For WS3 (shell) / WS4 (Agents tab):** place `HostsDestination()` (Tools
  menu or a tab) and `AllAgentsDestination()` (the Agents area); nothing else
  is needed. For the gate: `AppServices.installHosts()` calling
  `HostsInstaller().install(into:)` after `installWatchBridge()`; until it
  exists, the services are built on first use.
- For WS6: the watch reaches agents through the phone's `AgentControlling`
  (`FMControl/AgentControl.swift`); `UnifiedAgentRow` and
  `AgentTranscriptDelta` are plain `Sendable` values to map onto
  `FMWatchWire`.

Priority: **gate → WS1 → WS2 → WS3 → WS4 → WS5**, with WS6 in parallel once
WS4's module exists. Android has its own plan: the AW workstreams in
PROGRESS.md.

---

## 4. watchOS app: control agents, see and hear them

### 4.1 Agent data that exists today

| Data or action | Where | Watch use |
|---|---|---|
| Sessions per repository: id, title, status (`isActive`, `statusLabel`, `isFailed`), provider, model, branch, turns, tokens, context window, cost, credits, files changed, ahead/behind, conflicted, PR number, merged, rate-limit reset | `NIC/FMApi/Models/Agents.swift` (`AgentSession`); relay `POST /api/repo/<o>/<r>/agents/list` (owner-sealed) | List rows, the complication and the Smart Stack |
| Transcripts (owner-sealed per enrolled device: X25519 + ML-KEM-768 wrap, AES-256-GCM body) | `ApiService+Agents.swift` `agentTranscript`; `NIC/FMCrypto/OwnerCrypto.swift` | Source of the "beats" the sonifier plays |
| Change doorbells | Account events topics `agents` and `cloud-agents`; LAN `agents.changed` | Trigger a refresh; nothing polls |
| Relay controls | Launch `POST …/agent-sessions`, steer `POST …/agent-sessions/<id>`, retry, cancel `DELETE` (queued only) | Quick prompt and retry |
| LAN controls (desktop on the same network) | `LocalControlService.swift`: `agents.list`, `agents.start`, `agents.prompt`, `agents.stop`, `agents.continue`, `agents.transcript`, `agents.handoff` (move an on-device agent to the desktop), `agents.takeover` (move a runner run to the desktop, `RUNNER_AGENTS.md`); `LocalAgentRow.stoppable` | Stop and continue, at home |
| Listen sonifier | The desktop's `AgentSonifier`; on iOS, the WS4 FMAgentWatch module; on Android, `C/agentwatch` | Plays on the watch |
| Announcer | Desktop only (Piper) | New: `AVSpeechSynthesizer` one-liners ("forkmesh: tests passed, run done") |
| Approvals | **None exist** | Designed but disabled until a relay permission event exists (2.2) |
| Pause | **None exists**; only stop and continue | Show Stop and Continue; no Pause until the desktop supports it |

### 4.2 Architecture

**Option A: WatchConnectivity bridge (recommended for v1).** The iPhone app
stays the only ForkMesh client. It unseals transcripts, turns them into
compact beat lists (`FMWatchWire.AgentFrame`: id, title, repo, status, model
face, the latest 64 beats, turn and token counts), and sends them to the
watch:

- `WCSession.sendMessage` while both apps are reachable;
- `updateApplicationContext` for the latest list;
- `transferCurrentComplicationUserInfo` for complication refreshes (50 a day).

Watch commands travel back as messages: `stop`, `continue`, `prompt(text)`,
`retry`, `listen(on/off)`, `diff`, `start(text, repo)`. iOS wakes the phone app in the background to
handle a message from the watch, and the phone runs the HTTP or LAN call.

Why A:

1. **Auth.** The phone's session and keys sit in its Keychain.
   `KeychainStore.swift` uses `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`,
   and `keychain-access-groups` (`$(AppIdentifierPrefix)com.forkmesh.forkmesh`)
   is a per-device namespace. A shared access group does **not** reach the
   watch, so a standalone watch would need its own login.
2. **Sealing.** A transcript is wrapped per enrolled owner device. For a
   standalone watch to read it, the watch would have to be enrolled as one
   more owner device, and every desktop would have to wrap to it. That is
   possible (FMCrypto is pure Swift), but it widens the key set and the UX.
3. **Networking.** watchOS allows URLSession data tasks, but restricts
   low-level networking, including `URLSessionWebSocketTask` and BSD sockets,
   to narrow cases (TN3135). So the event doorbell and the LAN control
   channel cannot run on the watch. The phone already runs both.
4. **Battery and code size.** The watch runs only rendering, the synth and
   haptics.

**Option B: standalone networking** (later, as phase 2, for a watch away
from its phone). The watch gets its own login through a code shown on the
phone, and its own Ed25519 node key and owner enrolment. It reads over HTTP
only, refreshing on the WidgetKit budget and on user action; it has no
sockets and no LAN. It needs FMNet (HTTP only), FMCrypto, FMAccount and
FMApi to build for watchOS.

Tokens handed from phone to watch go into the **watch's own** keychain group
`$(AppIdentifierPrefix)com.forkmesh.forkmesh.watchkitapp`, shared with the
watch widget extension.

### 4.3 Screens and behaviour

- **Agent list** (root `NavigationStack`): one row per active or recent
  session. Each row shows the model face, title, repo, a status dot, and a
  turns and tokens gauge. Sorting is running first, then by recency. A
  Digital Crown scroll and pull to refresh send a `refresh` message.
- **Live view per agent:** the agent's body from the Watch field, drawn in
  `Canvas` and `TimelineView(.animation)`, with a reduced
  `AgentWatchFigures` set. The body is coloured by instrument, the ring holds
  the recent marks, and the last reply scrolls on the dot display under it.
  A full-screen field mode is a vertical page. Status and elapsed time sit in
  the toolbar.
- **Controls** (bottom toolbar and a Crown-scrolled sheet):
  - **Stop:** LAN `agents.stop` when the phone sees the desktop; relay cancel for queued runs; hidden when neither applies.
  - **Continue:** LAN `agents.continue`, or relay retry.
  - **Prompt:** a `TextField`, which opens dictation or Scribble on watchOS, followed by steer (relay) or `agents.prompt` (LAN). The prompt is limited to the relay's maximum length.
  - **Approve:** designed but hidden until 2.2 is resolved.
  - **Pause:** omitted (4.1).
- **Review changes** (a row on the live page and the controls sheet, shown
  when the frame carries `WireControls.diff`): the agent's diff, read through
  the phone. See 4.3.1.
- **Haptics:** Core Haptics is not on watchOS. Map `AgentFeel` pulses to
  `WKInterfaceDevice.current().play(_:)`: tool call `.click`, file write
  `.directionUp`, error `.failure`, run done `.success`, and needs-attention
  `.notification`. Rate-limit to at most one per 0.6 s.
- **Sound:** the FMAgentWatch synth renders PCM on the watch.
  `AVAudioEngine` plays it with `AVAudioSession` in `.playback`. Short
  phrases play on the built-in speaker while the app is in front. Continuous
  Listen uses `routeSharingPolicy = .longFormAudio`, which routes to AirPods
  or Bluetooth, together with the `audio` background mode, so Listen keeps
  playing with the wrist down. The announcer uses `AVSpeechSynthesizer` on
  the same session. A Listen toggle and a per-agent mute (the sound list's
  two switches) are in settings.
- **Complications** (WidgetKit, in the watch widget extension): circular
  (count of running agents with a progress ring), rectangular (top agent
  title, status and model face), inline ("2 agents running"), and corner.
  They read the last summary from the watch's keychain group
  `742ZANCB57.com.forkmesh.forkmesh.watchkitapp`.
- **Smart Stack:** the same widget with `TimelineEntryRelevance` raised while
  a run is active or has just finished, so it surfaces on its own. Tapping
  it deep-links to the agent's live view.
- **Notifications:** phone pings mirror to the watch automatically. Suggest a
  server `agents` APNs alert when a run finishes or fails (`app/src/apns.py`
  `APNS_PUSH_TOPICS` today are `pings`, `direct-messages`, `chat`), with a
  notification category that offers "Continue" and "Open".

#### 4.3.1 Reviewing a diff on the watch

The watch asks with `WatchCommand.diff(key:)`. The phone answers that one
command with an envelope of its own kind, `diff`, holding a `WatchDiff`
instead of a `WatchReply`, so a single round trip carries the whole review.
An iPhone app too old to know the command answers with the usual "Update
ForkMesh on your iPhone" reply, and the watch shows that sentence.

The phone reads the diff the way the phone's own Changes button does
(`AgentRunScreen.changesLink`):

| Frame | Read with | Notes |
|---|---|---|
| Desktop or LAN agent | `AgentOpsClient.diff` over the desktop control hub | Committed, uncommitted and untracked files |
| Queued node run with a branch | `ApiService.runnerAgentDiff`, first page of 100 files | Committed only |

The phone sets `WireControls.diff` on a frame only when one of those routes
exists: a desktop control hub for desktop and LAN frames, a branch for queued
runs. `AgentWatchDiff.make` (FMAgents) parses the patch with FMDiff and turns
each file into a `WatchDiffFile`: path, old path for renames, status letter,
group, line counts and its lines, with hunk headers carried as `@` lines that
hold the hunk's first new line number. FMDiff stays off the watch; the watch
only decodes the result.

A WCSession reply has the same ~64 KB ceiling as a message, so
`WatchWire.fitted(_: WatchDiff)` keeps the body under `maxBodyBytes`:

1. Every line is cut to 160 characters and every file to 600 lines.
2. While still too big, every file is cut to 300, 160, 80, 40, 16 and then 0
   lines. The file list matters more than any one file's body.
3. Last, files are dropped from the end, found with a binary search over the
   prefix length.

Each cut is counted (`hiddenLines` per file, `hiddenFiles` for the diff) and
the watch says how much more is on the iPhone.

On the watch, the Changes list shows the branch and its base, the totals, and
"n of m reviewed", then the files grouped Committed, Uncommitted and
Untracked. A file opens to its lines in a small monospaced font, wrapped,
with green and red rows and a cyan "Line N" row per hunk. The bottom toolbar
has the reviewed check and Next file. Checking a file plays a click and moves
to the next unreviewed file. Checking the last one plays a success tap.

Reviewed marks are local to the watch, in `UserDefaults` (the newest 600).
Each mark is keyed by the agent key, the branch, the file's group and path,
and its added, removed and line counts. A file the agent changes again
therefore comes back unreviewed. The marks are not shared with the phone's
viewed marks (DIFF_REVIEW.md), which are keyed by blob ids the watch never
receives.

#### 4.3.2 Starting an agent with Siri

The watch app has its own App Intent, `NewAgentIntent`
(`ForkMeshWatch/NewAgentIntent.swift`), and its own `AppShortcutsProvider`
with the iPhone's "Start a new agent in ForkMesh" phrases. Siri on the watch
runs the watch app's intents, never the iPhone's, so the iPhone's
`StartAgentIntent` (which opens the composer) does not reach the wrist. The
user guide is `www/docs/mobile/siri.md`, "On Apple Watch".

1. Siri asks for the prompt (a `String` parameter; App Shortcut phrases cannot
   hold one) and reads it back for a confirmation, because a misheard
   dictation would otherwise start a paid run.
2. The intent opens the watch app (`openAppWhenRun`). A watch app in the
   background is not `isReachable` to its iPhone, and `start` needs an
   answer, so it is not queued with `transferUserInfo` like `refresh`.
   `WatchStore.connected(within:)` waits up to 6 s for the activated, reachable
   session.
3. The watch sends `WatchCommand.start(text:repo:)`. The command has no agent
   key: it makes one. `repo` is the Shortcuts-only Repository field, or nil.
   An iPhone app too old to know `start` cannot decode it and answers "Update
   ForkMesh on your iPhone to use this.", which Siri says.
4. `WatchBridge.start` runs the composer's own `QuickAgentLauncher`, the one
   behind the iPhone's Send button, so the repository, destination (node,
   runner or desktop) and model are what the user last picked on the phone.
   It refuses while the composer is busy sending.
5. `QuickAgentLauncher.send(prompt:repository:onDevice:)` is shared with
   Genie's start-agent tool. It resolves a spoken repository by key,
   `owner/name`, the public owner, or the bare name (spaces against dashes
   ignored; the most recently updated wins a tie), sends, and puts the
   composer back on the repository it had. With `onDevice: false` (the watch)
   a run whose destination is this iPhone is refused before anything is
   consumed: an on-device run needs the iPhone's screen.
6. The reply is Genie's outcome sentence (`GenieFeature.outcomeLine`). On a
   start the bridge refreshes and publishes, so the new run is on the watch's
   list by the time Siri answers.

### 4.4 Sharing ForkMeshCore

`native/ios/ForkMeshCore/Package.swift` declares `platforms: [.iOS(.v17),
.macOS(.v14)]`. Add `.watchOS(.v10)` and a second product:

| Module | On watch? | Notes |
|---|---|---|
| FMBase | Yes | Pure Swift |
| FMAgentWatch (new, WS4) | Yes | Motion, voice, synth, feel, controller; FMBase only |
| FMWatchWire (new, WS6) | Yes | `Codable` phone-to-watch frames, commands and `WatchDiff`; shared by the app and the watch; FMBase only |
| FMCrypto, FMNet, FMAccount, FMApi | Phase 2 only | Needed for standalone mode. FMNet's `URLSessionWebSocket.swift` compiles on watchOS but must not be used there |
| FMChat, FMCode, FMGit (CZlib), FMDiff, FMAgents, FMLocalAI, FMTools, FMVault | No | Sockets, Git, MLX: phone only. The phone parses diffs for the watch (4.3.1) |

The new product is `.library(name: "ForkMeshWatchCore", targets: ["FMBase",
"FMAgentWatch", "FMWatchWire"])`. Keep tests Linux-runnable.

### 4.5 Project generator changes (`native/ios/tools/generate_project.rb`)

The project is generated by a Ruby `xcodeproj` script, not by XcodeGen.
Changes:

1. Give `native_target` a platform and deployment-target argument
   (`configuration_list(project, :watchos, '10.0', …)`). `:ios` is
   hard-coded today.
2. Create the new targets **after** `REMOTE_PACKAGES`, so the deterministic
   `F0…` object ids of existing objects do not move.
3. Target `ForkMeshWatch` (`:application`):
   - Settings: `SDKROOT=watchos`, `WATCHOS_DEPLOYMENT_TARGET=10.0`, `TARGETED_DEVICE_FAMILY=4`, `PRODUCT_BUNDLE_IDENTIFIER=com.forkmesh.forkmesh.watchkitapp`, `INFOPLIST_FILE=Config/ForkMeshWatch-Info.plist`, `CODE_SIGN_ENTITLEMENTS=Config/ForkMeshWatch.entitlements`, `ASSETCATALOG_COMPILER_APPICON_NAME=AppIcon`.
   - Info.plist: `WKApplication=YES`, `WKCompanionAppBundleIdentifier=com.forkmesh.forkmesh`, `WKRunsIndependentlyOfCompanionApp=NO` (YES in phase 2), `UIBackgroundModes=[audio]`.
   - Sources: synchronized group `ForkMeshWatch`. Links `ForkMeshWatchCore`.
4. Target `ForkMeshWatchWidgets` (`:app_extension`, `NSExtensionPointIdentifier=com.apple.widgetkit-extension`):
   - `SDKROOT=watchos`, bundle `com.forkmesh.forkmesh.watchkitapp.widgets`.
   - Embedded in the watch app's `PlugIns` (`:plug_ins` copy phase).
5. Embed the watch app in the iOS app: `app.add_dependency(watch)` and a
   copy-files phase with `dst_subfolder_spec = 16` (products directory) and
   `dst_path = '$(CONTENTS_FOLDER_PATH)/Watch'`.
6. Entitlements:
   - Watch app and widgets both get `com.apple.security.application-groups = [group.com.forkmesh.forkmesh.watch]` and `keychain-access-groups = [$(AppIdentifierPrefix)com.forkmesh.forkmesh.watchkitapp]`.
   - The watch needs no `aps-environment`: phone notifications are mirrored.
   - The iOS app adds nothing for WatchConnectivity.
7. Add a shared `ForkMeshWatch` scheme. The `ForkMesh` scheme builds the
   watch as a dependency. MLX stays linked into the app target only.

### 4.6 Codemagic, signing and App Store Connect

**Bundle ids to register.** Add these to `BUNDLE_IDS` in
`native/ios/tools/asc_bundle_ids.py`, which is the "Register the bundle ids"
step (`&native_bundle_ids`):

- `com.forkmesh.forkmesh.watchkitapp`: "ForkMesh Watch".
- `com.forkmesh.forkmesh.watchkitapp.widgets`: "ForkMesh Watch Widgets".

(Registered 2026-09-24; they were `com.forkmesh.nativeapp.watchkitapp[.widgets]`
before the native app took over `com.forkmesh.forkmesh`, section 5.)

The public App Store Connect API cannot create the App Group
`group.com.forkmesh.forkmesh.watch` (`/v1/appGroups` is 404) or assign it, and
neither can Xcode automatic signing with the API key (tried 2026-09-24,
"Authentication failed" on both watch targets), so releases were phone-only.
Since 2026-10-09 the watch shares its summary through its keychain group
instead and ships in every build (docs/native/IOS_RELEASE.md, "Widgets, Live
Activities and the watch").

**`&native_embed_entitlements`.** Sign nested code innermost first:

1. `…/ForkMesh.app/Watch/ForkMeshWatch.app/PlugIns/ForkMeshWatchWidgets.appex` with `Config/ForkMeshWatchWidgets.entitlements`;
2. `…/Watch/ForkMeshWatch.app` with `Config/ForkMeshWatch.entitlements`;
3. then the NotificationService appex and the app, as today.

Fail the step if `Watch/ForkMeshWatch.app` is missing.

**`&push_check`.** Also assert that the watch app and the widgets carry
`com.apple.security.application-groups`.

**`ios-native-build`.** Add a step
`xcodebuild build -scheme ForkMeshWatch -destination "generic/platform=watchOS Simulator" CODE_SIGNING_ALLOWED=NO`,
with its own log. If the image lacks the watchOS platform, run
`xcodebuild -downloadPlatform watchOS` first, the same way the Metal
toolchain step does. `swift test` in ForkMeshCore also covers FMAgentWatch
and FMWatchWire.

**Release and TestFlight.** The watch app ships **inside** the iOS IPA:

- the same archive, the same `-exportArchive` with `-allowProvisioningUpdates`, which mints the two extra App Store profiles once the ids exist;
- the same upload and the same App Store Connect app record;
- the same `Version.xcconfig`, since the watch targets must use the same `MARKETING_VERSION`/`CURRENT_PROJECT_VERSION`.

App Store Connect needs watch screenshots and a 1024 px watch icon in
`ForkMeshWatch/Assets.xcassets`.

**Ad hoc (`ios-native-adhoc`).** Apple Watch UDIDs must be registered as
devices; they list under platform IOS with device class `APPLE_WATCH`. The
"Check for registered devices" logic already counts them. The ad hoc profile
for the watch id only covers watches registered at build time.

---

## 5. Bundle ids and the move from the Flutter app

### 5.1 Bundle ids

| Platform | Ids |
|---|---|
| iOS | `com.forkmesh.forkmesh`, `.NotificationService`, `.watchkitapp`, `.watchkitapp.widgets` (`native/ios/tools/generate_project.rb`); keychain group `742ZANCB57.com.forkmesh.forkmesh`; secure-store service `com.forkmesh.forkmesh.secure`; push items `com.forkmesh.forkmesh.push` (the names the Flutter app wrote); App Group `group.com.forkmesh.forkmesh.watch` (watch targets only) |
| Android | `com.forkmesh.forkmesh` for the release APK (`-Pforkmesh.appId`, passed by `tools/forkmesh-release-host.py`); `com.forkmesh.nativeapp` by default (`native/android/app/build.gradle.kts`), which installs beside it |

The native iOS app is the App Store app ForkMesh (App Store Connect app
6803060735) from 1.4.0 on, shipped as the next version over the retired
Flutter builds (last upload 1.2.3 (16)). The old separate iOS id
`com.forkmesh.nativeapp` (and its watch ids) is unused; no App Store Connect
app was ever made for it. Release steps and open items:
[IOS_RELEASE.md](IOS_RELEASE.md).

### 5.2 Shipping native as an update under `com.forkmesh.forkmesh`

Reusing the id keeps the App Store listing, ratings, TestFlight groups and
every installed user. It keeps the Keychain and the app container, so nobody
is signed out, and it matches the server's APNs topic.

- **APNs topic per token** (`apns_device_tokens.bundle_id`, migration 0192).
  The app sends `bundleId` = `Bundle.main.bundleIdentifier` on registration,
  `com.forkmesh.forkmesh`, which `apns.normalize_bundle_id` allowlists and
  `apns_topic_for` sends under; rows without one mean that id.
- **Ids.** `APP_BUNDLE_ID` in `generate_project.rb` and `asc_bundle_ids.py`;
  the keychain groups in all four entitlements files; `PushCredentialStore`
  uses `742ZANCB57.com.forkmesh.forkmesh` / `com.forkmesh.forkmesh.push` with
  the per-host item names the Flutter app wrote; the watch ids, the App Group
  (`FMWatchWire.WatchGroup`) and `WKCompanionAppBundleIdentifier`.
- **Version** 1.4.0 (17), above every Flutter version; `ios-native-release`
  takes one past App Store Connect's highest build.
- **One-time data migration** (`FMAccount/FlutterMigration.swift`, run by
  `ForkMesh/App/FlutterMigrationRunner.swift` before any store is built;
  tests in `FlutterMigrationTests`). The Flutter Keychain items and
  `flutter.*` preferences are copied key for key under the same names
  (`identity/ed25519Seed`, `ownerCrypto/hybridV1`, `auth/session/<projectId>`,
  `vault/<account>/…`, `chat/e2ee/…`, `agent/credentials-v1`, the
  `p/<projectId>/` prefixes); files need no move. Rules:
  [IOS_RELEASE.md](IOS_RELEASE.md), "Flutter-to-native migration".
- **Push token** survives the update (same bundle id and team); the app
  re-registers on launch for every project with a session.
- **URL scheme** `forkmesh://`; routes in [NAVIGATION.md](NAVIGATION.md).
- **Universal links**: none existed at the cutover; the associated domains
  (an AASA with `appID 742ZANCB57.com.forkmesh.forkmesh` plus the
  entitlement) are Gate 2 work.
- **Android** makes the same move: the release APK is built as
  `com.forkmesh.forkmesh` and signed with the same v3 key as the Flutter APK,
  and its migrator copies the Flutter app's preferences and secure storage
  into `A/platform/SecureStore.kt` / `Prefs.kt` ([ANDROID.md](ANDROID.md)
  section 4).

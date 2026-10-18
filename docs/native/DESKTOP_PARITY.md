# Native app parity with the Qt desktop

This page compares the native apps with the Qt desktop: which desktop
capabilities belong on a phone, how the phone reaches them, and how the work is
split into workstreams WS8 to WS21. [PARITY.md](PARITY.md) covers the phone's
own feature set (WS1 to WS7) and the watch app; [NAVIGATION.md](NAVIGATION.md)
covers where each capability lives in the app.

## Legend

Paths: `D/` is `desktop/src/`, `MP/` is `marketplace/packages/`, `NI/` is
`native/ios/`, `NIC/` is `native/ios/ForkMeshCore/Sources/`.

**Phone verdict** for each desktop capability:

- **Yes**: the same capability in a phone-shaped UI.
- **Adapted**: reduced or reshaped for a phone.
- **Desktop-only**: stays on the desktop. Typical reasons: it builds or
  instruments the Qt app itself, needs local compilers or CLIs, needs a
  long-running background daemon (iOS suspends apps and forbids daemons), holds
  keys that must not leave the desktop, or is GPU- or window-heavy.

**Transport**:

- **API**: the account's server (HTTP, the account event socket, signed inbox
  writes).
- **DC**: desktop control. The phone drives the owner's desktop over the local
  control channel (`D/LocalControl.h`: pairing, AES-256-GCM frames, method
  families), on the same LAN or end-to-end encrypted through the owner's node
  socket on the relay. DC is how anything that lives only on the desktop (saved
  hosts, SSH keys, worktrees, CLI sessions, local ledgers) reaches the phone.
- **Device**: runs on the phone.

## Summary

- The phone covers the "read" half of the desktop natively: repositories,
  code, commits, issues, pull requests, discussions, chat, pings, notes and
  tasks.
- The "act" half (review and merge with inline comments, agent approvals,
  merge and clean up of agent branches, fleet operations, business packages)
  is reached mostly over DC or the API.
- About one capability in seven is desktop-only on purpose. The phone controls
  those over DC and does not reimplement them.

## Areas

| # | Area | On the phone | Desktop-only |
|---|---|---|---|
| 1 | Shell, navigation, search, window chrome | Tabs, prompt button, Home grid, per-tab stacks, global search, saved searches, find in page, interface sound cues, sponsored strip, push instead of tray | Layout presets, window backdrop, splash, quick rebuild loop, Capture/Resize, single-instance lock, Qt widget infrastructure |
| 2 | Agents | Session list (cloud and desktop), launch, transcripts, steer/stop/continue, merge and clean up, stop all, usage, shortcuts and skills (`insights.` family), local models through MLX | Agent jail, runtime adapters and CLI spawning, Claude IDE bridge, external session scan, agent insights boards (tool efficiency, guardrails, network sampler, announcer) |
| 3 | Teammates | Grid, editor (core fields), run now, schedules, stats, prompt/stop/continue (`teammates.` family) | Stall-monitoring teammates |
| 4 | Repositories and code | Catalog, fork/mirror, overview, files, commits and graph, branches, diffs and review ([DIFF_REVIEW.md](DIFF_REVIEW.md)), code map, repository settings | Size map, stale git lock and editor actions, private mirror serving, the desktop's own source tree |
| 5 | Issues, pull requests, discussions, projects, tasks | Full read and write through the API | None |
| 6 | Actions, security, webhooks, releases | Runs, logs, approvals, releases, security alerts, webhooks | Action runner, git hooks, action telemetry, artifact replication |
| 7 | Hosts, mirrors, fleet, Control page | Saved hosts and fleet operations over `fleet.` | Cloud deploy tooling, reward pool signing, system export/import, local VMs |
| 8 | Network, mesh, nodes, users | Mesh room, nodes, users | Host firewall check (iOS asks for local network permission) |
| 9 | Pings, chat, DMs, World | Inbox, chat depth, Discord bridge over `discord.` | World speech bridge |
| 10 | Notes, calendar, timers | Notes, calendar, timers (local notifications and Live Activities) | None |
| 11 | Marketplace and packages | Market, CRM, marketing, analytics, ads, ledger, tips, password vault and AutoFill | System voice changer, IDE and browser extensions, suggested host apps, package publishing |
| 12 | Stats, health, logs, diagnostics | Stats, desktop health summary over `diag.`, logs, admin console | Component health, memory page, Qt diagnostics, background attribution |
| 13 | Settings | Account, appearance, pings, SSH keys, MCP, identity, devices, data | Suggested apps |
| 14 | Accounts, identity, keys, devices | Sign-in, keys, devices, organizations | Claude Code device setup |
| 15 | Projects and multi-instance | One isolated scope per project (PARITY.md, WS1) | None |
| 16 | Coves and secrets | Coves and secrets (`FMCoves`, `FMVault`) | None |
| 17 | Desktop releases, updates, backups | Rebuild and restart the desktop remotely ([DESKTOP_UPDATE.md](DESKTOP_UPDATE.md)) | Auto-update (the App Store updates the app), hourly local backups |
| 18 | Background capabilities | Push, background refresh within iOS limits | Mirror sweep and replica sync, claiming agent jobs as a runner, website-requested branch operations |

## 19. Workstreams WS8 to WS21

Conventions (shared with [PARITY.md](PARITY.md), section 3):

- **One owner per file.** Two workstreams never edit the same file at the same
  time. Ownership of an existing file moves only after the previous owner has
  merged.
- **Installer hooks.** Each workstream registers into `AppServices` through
  its own `NI/ForkMesh/App/AppServices+<Area>.swift` installer and never edits
  `AppServices.swift`.
- **Linux-testable core.** Logic lives in `NIC/` with XCTests that run on
  Linux; views live in `NI/`.
- **One core module per new area.** `Package.swift` registers `FMSurfaces`,
  `FMDiscord`, `FMPlanning`, `FMDesktopRemote`, `FMTeammates`, `FMPackages`,
  `FMInsights` and `FMCoves`; each is owned by its workstream. `FMAgents` and
  `FMCode` may use `FMControl`.

### 19.1 AppServices hooks

`installNavigation()`, `installInbox()`, `installAgentOps()`,
`installReview()`, `installSurfaces()`, `installChat()`, `installCI()`,
`installPlanning()`, `installDesktopRemote()`, `installTeammates()`,
`installAccount()`, `installPackages()`, `installInsights()`,
`installCoves()` and `installHosts()`, each in its own installer file.
`installNavigation()` runs last, so the shell sees every route the other
installers registered.

### 19.2 Route contract

`NIC/FMApi/Navigation/Route.swift` holds every destination in
[NAVIGATION.md](NAVIGATION.md)'s sitemap as a `Codable, Hashable` case, and
`RouteRegistry.swift` maps route kinds to destinations. A feature registers
`registry.register(.calendar) { CalendarScreen() }` from its installer. An
unregistered route shows a placeholder, so the whole sitemap is navigable
before every screen exists.

- `RouteRegistry` is generic (`RouteRegistry<Destination>`) so it tests on
  Linux. `Route.Kind` names each case, its owning workstream and its home tab.
  `RouteParser` parses `forkmesh://` and https links.
- `AppServices.routes` is the `RouteRegistry<AnyView>`; every tab stack has one
  `navigationDestination(for: Route.self)`.

### 19.3 Desktop control families

`LocalControlServer::registerFamily(prefix, handler)` routes a method by the
longest registered prefix, before `onRequest` and before the built-in
`files.*` transfer. `MainWindowLocalControlFamilies.cpp` registers the
families, all owner-only with the same check as `hosts.*`. Each family answers
in `MainWindowLocalControl<Family>.cpp`, with a pure, testable half in
`D/LocalControl<Family>.{h,cpp}`. On the phone,
`FMControl/Families/DesktopControlFamily` calls a family over
`DesktopControlHub` (LAN or relay) and receives its `*.changed` bells.

| Family | Owner |
|---|---|
| `agentops.` | WS10 |
| `mergequeue.` | WS11 |
| `fleet.`, `files.browse.`, `diag.` | WS16 |
| `teammates.`, `insights.` | WS17 |
| `discord.` | WS13 |
| `wallet.` | WS19 |

Later families (`update.`, `pings.`, `pulls.`) follow the same pattern.
`pulls.` carries Merge all with AI from the phones' pull request lists; see
[../desktop/pull-merge-all.md](../desktop/pull-merge-all.md#from-a-phone).

### 19.4 Extension targets

`native/ios/tools/generate_project.rb` adds `ForkMeshWidgets` (WidgetKit and
Live Activities), `ForkMeshShare` (share extension) and `ForkMeshAutoFill`
(credential provider), with bundle ids in `asc_bundle_ids.py` and the
Codemagic embed and signing steps. All three share data with the app through
the keychain group, not an App Group (see [IOS_RELEASE.md](IOS_RELEASE.md),
"Widgets, Live Activities and the watch" and "AutoFill", and
[CAPTURE_MARKUP.md](CAPTURE_MARKUP.md)).

### 19.5 Associated domains

`NI/Config/ForkMesh-{Debug,Release}.entitlements` list `applinks:` for the
served hosts. `app/src/app_links.py` serves
`/.well-known/apple-app-site-association` on every host the app serves.

### 19.6 Workstreams

| WS | Area | Owns |
|---|---|---|
| WS8 | Navigation shell and information architecture | `NI/ForkMesh/Shell/**`, `NI/ForkMesh/Navigation/**`, `NI/ForkMesh/Search/**`, `NIC/FMApi/Navigation/**`, `NIC/FMApi/Search/**`, `HomeShellModel.swift`, `HomeShellLogic.swift`, `FmHaptics.swift`, `AppServices+Navigation.swift`, `app/src/app_links.py` |
| WS9 | Inbox and notification actions | `NI/ForkMesh/Features/Inbox/**`, `NIC/FMApi/Inbox/**`, `NotificationActions.swift`, `AppServices+Inbox.swift` |
| WS10 | Agents over desktop control: approvals, merge and clean up, bulk actions, usage | `D/LocalControlAgentOps.{h,cpp}`, `NIC/FMAgents/Remote/**`, `NI/ForkMesh/Features/Agents/Remote/**`, `AppServices+AgentOps.swift` |
| WS11 | Code review and collaboration | `NI/ForkMesh/Features/Code/**` (except `RepoActivityTabs.swift`), `NIC/FMCode/Collab/**`, `NIC/FMDiff/**`, `D/LocalControlMergeQueue.{h,cpp}`, `AppServices+Review.swift` |
| WS12 | System surfaces: widgets, Live Activities, App Intents, Siri, Spotlight, share extension | `NI/ForkMeshWidgets/**`, `NI/ForkMeshShare/**`, `NI/ForkMesh/Intents/**`, `NIC/FMSurfaces/**`, `app/src/apns_live_activity.py`, `AppServices+Surfaces.swift` |
| WS13 | Chat depth | `NI/ForkMesh/Features/Chat/{Threads,Channels,Office}/**`, `NIC/FMChat/{Threads,Office}/**`, `D/LocalControlDiscord.{h,cpp}`, `AppServices+Chat.swift` |
| WS14 | CI, releases and security | `RepoActivityTabs.swift`, `NI/ForkMesh/Features/Code/{CI,Releases,Security,Webhooks}/**`, `NIC/FMCode/CI/**`, `AppServices+CI.swift` |
| WS15 | Planning: tasks, calendar, notes, timers, projects, milestones | `NI/ForkMesh/Features/Planning/**`, `NIC/FMPlanning/**`, `AppServices+Planning.swift` |
| WS16 | Desktop and fleet remote | `D/LocalControlFleet`, `D/LocalControlFiles`, `D/LocalControlDiagnostics`, `NI/ForkMesh/Features/Desktop/**`, `NIC/FMDesktopRemote/**`, `AppServices+DesktopRemote.swift` |
| WS17 | Teammates | `D/LocalControlTeammates`, `D/LocalControlInsights`, `NI/ForkMesh/Features/Teammates/**`, `NIC/FMTeammates/**`, `AppServices+Teammates.swift` |
| WS18 | Settings, account, identity and devices | `NI/ForkMesh/Features/Account/**`, `NIC/FMAccount/Identity/**`, `SettingsLogic.swift`, `AppServices+Account.swift` |
| WS19 | Business packages and the Market | `NI/ForkMesh/Features/Packages/**`, `NIC/FMPackages/**`, `D/LocalControlWallet.{h,cpp}`, `AppServices+Packages.swift` |
| WS20 | Stats, health, logs and the admin console | `NI/ForkMesh/Features/{Insights,Admin}/**`, `NIC/FMInsights/**`, `MetricKitSink.swift`, `AppServices+Insights.swift` |
| WS21 | Coves, secrets and AutoFill | `NIC/FMCoves/**`, `NIC/FMVault/**`, `NI/ForkMesh/Features/{Coves,Secrets}/**`, `NI/ForkMeshAutoFill/**`, `AppServices+Coves.swift` |

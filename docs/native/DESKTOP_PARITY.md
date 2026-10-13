# Native app parity with the Qt desktop

Status as of 2026-09-24. The desktop was read at production `main` `87faa9064`
(`desktop/src`, 419 files, about 338k lines, plus the desktop halves of the
marketplace packages in `marketplace/packages/*/content/desktop`). The native
app was read at the same commit (`native/ios`). The WS1–WS7 branches were read
as they stood that day.

This is the companion to [PARITY.md](PARITY.md), which lists the phone app's
own feature set, workstreams WS1–WS7 and the watch app. This file compares the
native app with the **desktop**, walks
every rail page, panel, dialog and background capability, says which ones belong
on a phone, and groups the work into workstreams WS8–WS21. How the app is
navigated, and where each capability below lives, is in
[NAVIGATION.md](NAVIGATION.md). The progress of each workstream is tracked in
[PROGRESS.md](PROGRESS.md).

**Home repository and server.** The native apps are built for **ForkMesh
Next**. Native code lives in this repository (it ran in the Next repository,
`~/projects/forkmesh-backup/src`, from 2026-09-24 to 2026-09-27), and the
app's default instance was Next. "API" in this file therefore means
Next's server, `https://app.next.forkmesh.com`. It runs the same Python app and
the same `/api/...` routes as the production relay, on CPython with rqlite
instead of Cloudflare and D1 (`docs/PLAN.md` in the Next repo). In the Next
repo, `desktop/` is kept for reference only. Desktop-side changes listed below
(new `desktop/src/LocalControl*.cpp` files) are therefore made in the desktop's
own repository, and they reach Next users through the desktop's project picker,
which already runs one project per instance.

## Legend

**Paths.** `D/` is `desktop/src/`. `MP/` is `marketplace/packages/`. As in PARITY.md, `NI/` is
`native/ios/`, and `NIC/` is `native/ios/ForkMeshCore/Sources/`. `#n` in
PARITY.md refers to a row of PARITY.md's feature matrix.

**Native iOS**, the state in `native/ios` today:

- **Done**: built (for the phone's own feature set, see PARITY.md; the app
  target has been compiled on Codemagic, see PROGRESS.md).
- **Partial**: some of it exists, and the cell says what is missing.
- **Missing**: nothing exists.
- **WSn**: being built by an in-flight workstream (WS1–WS7, WS22).

**Phone**, whether the capability belongs on an iPhone:

- **Yes**: the same capability, in a phone-shaped UI.
- **Adapted**: a reduced or reshaped version. The cell says how.
- **Desktop-only**: it stays on the desktop, and the cell gives the reason.
  Typical reasons: it builds or instruments the Qt app itself, it needs local
  compilers or CLIs, it needs a long-running background daemon (iOS suspends
  apps and forbids daemons), it holds keys that must not leave the desktop, or
  it is GPU- or window-heavy.

**Transport**, how the phone reaches the capability:

- **API**: Next's server directly (`app.next.forkmesh.com`: HTTP, the account
  event socket, signed inbox writes).
- **DC**: desktop control. This is the phone driving the user's own desktop
  over the local control channel (`D/LocalControl.h`: pairing, AES-256-GCM
  frames, `agents.*`, `files.*`), either on the same LAN or, once WS7 lands,
  carried end-to-end encrypted through the owner's node socket on the server.
  DC is how anything that lives only on the desktop (saved hosts, SSH keys,
  worktrees, CLI sessions, local ledgers) reaches the phone.
- **Device**: runs on the phone.

**Effort** is the iOS work left: **S** is up to 2 days, **M** up to 2 weeks,
**L** more than that. A dash means nothing is left, or the row is desktop-only.

## Headline

| | Count |
|---|---|
| Desktop capabilities inventoried (rows below) | 283 |
| Phone: **Yes** | 157 |
| Phone: **Adapted** | 83 |
| Phone: **Desktop-only** | 43 |
| Native today: Done | 37 |
| Native today: Partial | 56 |
| Native today: Missing | 132 |
| Native today: in flight (WS1–WS7, WS22) | 15 |
| Native today: N/A (the desktop-only rows) | 43 |
| Rows that need desktop control (DC) | 72 |

The counts come from the tables and are recounted by the script in
"How the counts are made" at the end.

What this means:

- **The phone already covers the "read" half of the desktop.** Repositories,
  code, commits, issues, pull requests, discussions, chat, pings, notes and
  tasks exist natively (PARITY.md).
- **Most of the gap is the "act" half.** That is reviewing and merging with
  inline comments; approving what an agent wants to do; merging and cleaning up
  agent branches; running the fleet; and the business packages (CRM, marketing,
  ledger), which the phone has never had.
- **About one capability in seven is desktop-only, on purpose.** These are the
  build and restart loop, the Qt diagnostics pages, the mirror-serving daemon,
  CI runners, CLI runtimes and wallet signing. The phone controls them over DC
  and does not reimplement them.
- **Desktop control is the multiplier.** 72 rows need DC. WS7 makes DC work
  away from home, and the new Gate 2 gives each later workstream its own
  method family on the desktop without touching a shared file.

---

## 1. Shell, navigation, search and window chrome

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 1.1 | Activity rail: Repo, Repos, Market, Ledger, Tasks, Calendar, Notes, Pings, Agents, Chat, Settings, Tour, Log, Control, Network, Users, Team, Files, Stats, Health, Memory, Restart, Capture, Resize, with badges and package rail menus | `D/MainWindowNavigation.cpp`, `D/MainWindowMarket.cpp` (`attachPackageRailMenu`) | **Partial**: three tabs (Code, Agents, Chat) and a page menu (`NI/ForkMesh/Shell/HomeShell.swift`) | Adapted: four tabs, the prompt button and the You hub (NAVIGATION.md) | Device | M | WS8 |
| 1.2 | Back/forward history with a history menu, restored per page and sub-place | `D/MainWindowRepoDetail.cpp` (`createNavHistoryButtons`, `navigateBack`, `showNavHistoryMenu`) | **Partial**: one NavigationStack per tab, no restoration | Adapted: stack per tab, back swipe, long-press Back for the history menu, state restoration; no Forward | Device | S | WS8 |
| 1.3 | Project picker in the top bar (a project per folder under `~/projects`) | `D/MainWindowProjectPicker.cpp`, `D/ProjectRegistry.h`, `D/AppPaths.h` | **WS1** | Adapted: a project is an instance scope on the phone | Device | — | WS1 |
| 1.4 | Relay/server picker with favicons; add, rename, remove, switch | `D/MainWindowChat.cpp` (`loadServers`, `switchToServer`, `fetchFavicon`) | **Done** as relays (`NI/ForkMesh/Features/Chat/RelaySettingsSection.swift`) | Yes | API | — | WS1 |
| 1.5 | Global search overlay (Ctrl+K): repositories, issues, pull requests, commits, files, packages, settings | `D/MainWindowSearch.cpp`, `D/GlobalSearchMatch.h` | **Missing**: only per-screen filters | Yes | API + Device | M | WS8 |
| 1.6 | Header search dropdown and the full results page, with buckets (stop, delete, spinners) | `D/MainWindowRepoDetail.cpp` (`buildSearchResultsSection`, `startSearchProcess`) | **Missing** | Yes | API | M | WS8 |
| 1.7 | Saved searches, each with the counts from its last run | `D/SavedSearchStore.h`, `D/MainWindowSearch.cpp` | **Missing** | Yes | Device + API | S | WS8 |
| 1.8 | Find in page (F3, Shift+F3) | `D/MainWindowSearch.cpp` (`openPageFind`) | **Missing** | Adapted: `UIFindInteraction` in the file viewer, diff and transcript | Device | S | WS11 |
| 1.9 | Keyboard shortcuts across the app | `D/MainWindow*.cpp` (`QKeySequence`) | **Missing** | Adapted: iPad hardware keyboard `.keyboardShortcut`, shown in the Command-key overlay | Device | S | WS8 |
| 1.10 | Command palette: act on anything from the keyboard | `D/MainWindowSearch.cpp` (the Ctrl+K overlay runs actions too) | **Missing** | Yes: long-press the prompt button | Device | M | WS8 |
| 1.11 | Layout presets ("Change view") | `D/AppViewMode.h`, `D/MainWindowViews.cpp` | N/A | Desktop-only: window layouts; iPad follows size classes | — | — | — |
| 1.12 | Window backdrop colour or image | `D/WindowBackdrop.h` | N/A | Desktop-only: window decoration; iOS uses system materials (Liquid Glass) | — | — | — |
| 1.13 | Themes, including package skins (Winamp QSS) | `D/Theme.h`, `MP/winamp` | **Partial**: accents, light and dark | Adapted: accent and appearance only; QSS skins do not apply | Device | S | WS18 |
| 1.14 | Interface sound cues | `D/UiSoundCues.h`, `D/UiSounds.h` | **Done** (`NI/ForkMesh/Theme/FmSoundCues.swift`, `NIC/FMAgentWatch/InterfaceCues.swift`; Android `ui/kit/FmSoundCues.kt`): the desktop's fourteen cues, see NAVIGATION.md §6.5 | Adapted: on by default and audible in silent mode, only the Sounds switch mutes them; no per-control event filter, cues ride on `FmHaptics` and the kit | Device | S | WS8 |
| 1.15 | Onboarding tour with a spotlight overlay | `marketplace/packages/tour/content/desktop/OnboardingTour.h`, `D/MainWindowOnboarding.cpp` | **Missing** | Adapted: a short first-run sequence plus TipKit tips | Device | S | WS8 |
| 1.16 | First-run choice and Quick Setup | `D/MainWindowOnboarding.cpp`, `D/MainWindowSettings.cpp` (`buildQuickSetupTab`) | **Partial**: auth flow only | Adapted: sign in, pick a project, pair a desktop | API | S | WS18 |
| 1.17 | Startup splash and startup trace | `D/StartupSplash.h`, `D/StartupTrace.h` | N/A | Desktop-only: iOS draws the launch screen | — | — | — |
| 1.18 | Footer status bar: Git identity, commit, diagnostics tiles, website status lights | `D/MainWindowDiagnostics.cpp` (`updateFooter*`), `D/MainWindowChat.cpp` (`buildStatusBar`, `refreshFooterWebsiteStatus`) | **Partial**: `TelemetryStrip.swift`, `ConnectionDot.swift` | Adapted: a connection dot, and a Status page in the hub | API | S | WS20 |
| 1.19 | Floating prompt overlay (detach, collapse, placement) | `D/MainWindowChat.cpp` (`setPromptOverlay*`) | **WS3** | Adapted: the trailing prompt button opens the composer | Device | — | WS3 |
| 1.20 | Prompt drafts kept on disk across a restart or crash | `D/PromptDraftStore.h` | **Missing** | Yes | Device | S | WS3 |
| 1.21 | Spell-check squiggles in the prompt | `D/PromptSpellCheckHighlighter.h` | **WS3**: comes free with the system text view | Yes | Device | — | WS3 |
| 1.22 | Sponsored strip (ForkMesh Ads, text only) | `D/AdBannerModel.h`, `D/AdBannerWidget.h`, `D/MainWindowAds.cpp` | **Done** (`SponsoredAdCard.swift`) | Yes | API | — | — |
| 1.23 | Membership prompt (Patreon) | `D/MainWindowMembership.cpp` | N/A on iOS | Not on iOS: no membership status, tier or link (`IOS_RELEASE.md`, "App Review: nothing paid on iOS") | — | — | WS18 |
| 1.24 | Restart and quick rebuild; rebuild queue and timing | `D/MainWindowSettings.cpp` (`quickRebuildRestart`, `rebuildAndRelaunch`), `D/RestartTiming.h` | N/A | Desktop-only: builds and relaunches the Qt app itself | — | — | — |
| 1.25 | Capture and Resize rail buttons | `D/MainWindowNavigation.cpp` | N/A | Desktop-only: window tooling; the phone uses the system screenshot | — | — | — |
| 1.26 | Desktop notifications and the tray | `D/MainWindowActions.cpp` (`showPingTrayMessage`, `activateDesktopNotification`) | **WS2** (APNs); every desktop ping is forwarded to a paired phone ([DESKTOP_PINGS.md](DESKTOP_PINGS.md)) | Adapted: push notifications | API | — | WS2 |
| 1.27 | Single-instance lock | `D/SingleInstance.h` | N/A | Desktop-only: iOS runs one instance | — | — | — |
| 1.28 | "Build too old" gate for retired builds | `D/ClientVersionGate.h` | **Missing** | Yes: an "Update required" screen | API | S | WS8 |
| 1.29 | Widget infrastructure (tooltips, kebab headers, flow and wrap layouts, scroll-jump buttons) | `D/Tooltips.h`, `D/KebabHeaderView.h`, `D/FlowLayout.h`, `D/HeaderWrapLayout.h`, `D/ScrollJumpButtons.h`, `D/CurrentPageStack.h` | N/A | Desktop-only: SwiftUI replaces it | — | — | — |

## 2. Agents

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 2.1 | Sessions list: status, provider, model, branch, turns, tokens, cost, files changed, ahead/behind, conflicts, PR | `D/MainWindowAgentList.cpp`, `D/AgentStore.h` | **Partial**: cloud rows plus the LAN desktop list (`AgentsScreen.swift`) | Yes | API + DC | M | WS7, WS10 |
| 2.2 | Sort (12 orders) and filters (teammates only, cloud) | `D/MainWindowAgents.cpp` | **Missing** | Yes | Device | S | WS10 |
| 2.3 | Launch: repo, issue, provider, model, permission mode, thinking level, priority, images | `D/MainWindowAgentLaunch.cpp`, `D/AgentRunner.h`, `D/AgentPromptImages.h` | **Partial**: `AgentStartCard.swift` (queued and LAN); photos and models are WS3 | Yes | API + DC | M | WS3, WS10 |
| 2.4 | Transcript (Claude and Codex styles, tool calls, timestamps, a Run button that replays a command) | `D/ClaudeTranscriptView.cpp`, `D/CodexTranscriptStyle.h`, `D/MainWindowAgentTranscript.cpp` | **Done** (sealed transcripts; LAN transcripts) | Yes: no Run replay (that needs the desktop shell) | API + DC | — | — |
| 2.5 | Steer, stop and continue a running session | `D/MainWindowAgentActions.cpp`, `D/MainWindowAgentCliSession.cpp` | **Done** on the LAN and through the relay, also without LAN pairing ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)) | Yes | DC | — | WS7 |
| 2.6 | Session detail card (stamps, usage, worktree, PR, task link) | `D/MainWindowAgentDetail.cpp` | **Partial** | Yes | API + DC | S | WS10 |
| 2.7 | Merge & clean up: merge the agent branch into base, remove the worktree, delete the branch; "Del merged"; "Delete selected" | `D/MainWindowAgentLanding.cpp` (`mergeAgentBranchIntoBase`, `cleanupStreamWorktree`), `D/MainWindowBranches.cpp` (`deleteAllMergedAgentSessions`) | **Partial**: merge & clean up with the celebration's merge stats and velocity, the same stats on demand (`agentops.mergestats`, Leaderboard), delete one agent, and its diff (`agentops.diff`) on both apps ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)); no "Del merged" | Yes: confirmed with Face ID | DC | M | WS10 |
| 2.8 | Open a PR for a finished run automatically | `D/MainWindowAgentLanding.cpp` (`maybeCreatePullForStreamSession`, `landAgentPullForSession`) | **Missing** | Yes | DC | S | WS10 |
| 2.9 | Cloud sessions (queued on the server, claimed by a runner or another desktop): list, detail, follow-up, delete | `D/MainWindowAgentCloud.cpp`, `D/AgentCloudSessions.h` | **Done** (Cloud segment) | Yes | API | — | — |
| 2.10 | Move to cloud (hand a desktop session to a runner) | `D/MainWindowAgentCloud.cpp`, `D/AgentResumeIdentity.h` | **Missing** | Yes | DC | S | WS10 |
| 2.11 | Stop all, Start all, Update all | `D/MainWindowAgents.cpp` | **Partial**: Stop all in the Agents header on both apps ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)); no Start all or Update all | Yes | DC | S | WS10 |
| 2.12 | Run order and priority (1–99) | `D/AgentStore.h` | **Partial**: read-only; the Agents list sorts by it (`AgentLineSort.priority`), no reordering | Yes | DC | S | WS10 |
| 2.13 | **Approvals**: the command approval chip and dialog, external-call gate prompts, access-failure prompts | `D/MainWindowCommandPermissions.cpp`, `D/AgentNetworkGate.h`, `D/AgentPermissionController.h`, `D/AgentAccessFailure.h`, `D/MainWindowAgentAccess.cpp` | **Missing**. No approval flow exists on the server or in LAN control; `NIC/FMApi/ViewModels/SettingsLogic.swift` keeps remote commands locked | Yes: answer from a notification or the lock screen | DC + push | M | WS10 |
| 2.14 | Capability grants (once, session, worktree, persistent) | `D/AgentCapabilityBroker.h`, `D/AgentCapabilityStore.h` | **Missing** | Adapted: answer prompts; list and revoke persistent grants | DC | S | WS10 |
| 2.15 | Agent jail (private TMPDIR, cache, memory cap) and guard script | `D/AgentJail.h`, `D/AgentGuardScript.h` | N/A | Desktop-only: enforced by the desktop's process launcher | — | — | — |
| 2.16 | Per-agent network sampler (who talks to whom) | `marketplace/packages/agent-watch/content/desktop/AgentNetworkSampler.h` | **Missing** | Desktop-only: the phone apps dropped Agent insights on 2026-09-29 | DC | — | WS17 |
| 2.17 | Watch: the fleet as a field of bodies (Market package `agent-watch`) | `marketplace/packages/agent-watch/content/desktop/`, `D/AgentActivityFeed.h` | **WS4** | Yes | Device (fed by API and DC) | — | WS4 |
| 2.18 | Listen: a run played as music | `D/AgentSonifier.h` | **WS4** | Yes | Device | — | WS4 |
| 2.19 | Announcer (sportscaster speech with a Piper voice) | `D/AgentAnnouncer.h`, `D/AgentAnnouncerVoice.h` | **Missing** | Desktop-only: the phone apps dropped Agent insights on 2026-09-29, and the phone announcer went with it | Device | — | WS17 |
| 2.20 | Usage meters (Claude Code usage, Codex remaining, rate-limit reset) and usage-limit reminders | `D/MainWindowAgentUsage.cpp`, `D/UsageLimitCalendar.h`, `D/AgentTokenPricing.h` | **Missing** | Yes: reminders become local notifications | DC | S | WS10 |
| 2.21 | Several agent accounts per provider (add, rename, probe identity, pick one) | `D/MainWindowChat.cpp` (`showAgentAccountMenu`, `selectAgentAccount`) | **Missing** | Adapted: pick the account at launch; sign-in uses the provider-login terminal (#58) | DC | S | WS10 |
| 2.22 | Provider-login terminal (pty) for the desktop and for hosts | `D/AgentLoginPty.h`, `D/MainWindowHosts.cpp` (`openHostAgentLoginTerminal`) | **Done** (`AgentLoginTerminalScreen.swift`) | Yes | API + DC | — | — |
| 2.23 | Claude IDE bridge (ForkMesh acts as the `claude` CLI's IDE) | `D/ClaudeIdeBridge.h` | N/A | Desktop-only: a localhost WebSocket server for a local CLI | — | — | — |
| 2.24 | Runtime adapters: Claude stream, Codex app-server, JSONL framer, CLI locator, local-agent script | `D/ClaudeStreamSession.h`, `D/CodexAppServerSession.h`, `D/AgentRuntimeRegistry.h`, `D/AgentRuntimeSession.h`, `D/AgentJsonlFramer.h`, `D/AgentCliLocator.h`, `D/CloudflareAgentScript.h` | N/A (the phone's own agent is `NIC/FMAgents/CodingAgent.swift`, **Done**) | Desktop-only: spawns CLI processes | — | — | — |
| 2.25 | Scan of external `claude` sessions | `D/ClaudeSessionScan.h` | N/A | Desktop-only: reads `/proc` | — | — | — |
| 2.26 | Tool efficiency board, ledger, usage table, improvement log; turning off tool families | `D/AgentToolEfficiencyBoard.h`, `D/AgentToolEfficiencyLedger.h`, `D/AgentToolUsageTable.h`, `D/AgentToolImprovementLog.h`, `D/AgentToolPolicy.h`, `D/AgentToolCatalog.h` | **Missing** | Desktop-only: the phone apps dropped Agent insights on 2026-09-29 | DC | — | WS17 |
| 2.27 | Guardrails board and ledger (repository boundaries, Approve/Deny history) | `D/AgentGuardrails.h`, `D/AgentGuardrailBoard.h`, `D/AgentGuardrailLedger.h` | **Missing** | Desktop-only: the phone apps dropped Agent insights on 2026-09-29 | DC | — | WS17 |
| 2.28 | Repository Shortcuts tab: shortcuts and skills (edit, run, stop) and the skill usage chart | `D/MainWindowShortcuts.cpp`, `D/SkillUsageChart.h` | **Partial** (`NI/ForkMesh/Features/Teammates/ShortcutsScreen.swift`, `NIC/FMTeammates/ShortcutsStore.swift`): list, read, run and watch the output; no editing, stopping or usage chart | Adapted: the Shortcuts page (the navigator's Operate group since 2026-10-01, a repository tile before; NAVIGATION.md §2.0.29) lists the desktop's shortcuts and skills over `insights.shortcuts`; Run goes through `insights.prepare`/`insights.run`. A prompt shortcut or skill starts an agent on the desktop. A script shortcut (`.sh`) runs on the desktop exactly as its card would, alongside any other running script since 2026-10-09 (before that one at a time, `shortcut_running`; running the same script again restarts it since 2026-10-06), and its merged stdout/stderr streams to the phone as `insights.shortcutOutput` events (`id`, `repo`, `file`, `name`, `running`, `ok`, `message`, `chunk`); `insights.shortcutRun` returns the newest run, or the one named by `id`, with up to 96 KB of its output, plus `runs` (every run the desktop keeps, without output), so a phone that opens Shortcuts mid-run or joins after a reconnect catches up. The phones keep a run per id (`ShortcutsStore.runs`, the newest 20): an event for an unknown id fetches that run, `lastRun` is the newest, and the iOS output screen picks between runs when there are several (Android lists each running run's card). The phone shows it in an Output section and on the shortcut's own page; confirming a script's Run opens the live terminal (iOS pushes the output screen, a black terminal that follows the tail; Android scrolls to the output card). Since 2026-10-10 each run row also carries `expectedMs` (the shortcut's last measured run, -1 when it has none) and `elapsedMs` (how long it has run, -1 once it stops), and each `insights.shortcuts` entry carries `lastMs`, `expectedMs` and `elapsedMs`; a dedicated-agent shortcut or skill whose agent is queued or working is also `running`, and the desktop broadcasts `insights.changed` when such an agent starts or finishes. The phones turn `elapsedMs` into a start time on their own clock when the row arrives (`startedAtMs`), so a clock difference with the desktop does not move the bar. Every row shows "took 1m 30s" from `lastMs`, and a running row, the Output card (Android) and the run status (iOS) show a bar filling against `expectedMs` with "1m 05s of about 1m 30s · 25s left", the same wording as the desktop (`ShortcutProgress` in both `ShortcutsStore`s); with no timed run there is no bar, only "Running for …". Each `insights.shortcuts` entry also carries `running`, `lastOutput` (the last non-blank line of its latest run, without colour codes or carriage-return progress frames, at most 200 characters; the live line while it runs), `lastOk`, `finished` and `lastRunAtMs`. The desktop keeps the last line with each shortcut's run stats in `shortcuts/runStats`, so it outlasts a restart. Every row in the list shows that line under its name, with a spinner, a check or a cross; the phones replace it with the streamed line while the run is live. Desktops before 2026-10-01 refuse scripts (`script_on_desktop`). Since 2026-10-05 the iPhone also runs prompt shortcuts and skills itself (Run on this iPhone opens the composer aimed at this iPhone) and, with no desktop answering, lists them from its own mirror of the repository (`PhoneShortcuts`, `ShortcutMirror`; NAVIGATION.md §2.0.57) | DC | — | WS17 |
| 2.29 | Issue looper (work through issues one after another) | `D/MainWindowRepoDetail.cpp` (`LooperToggle`), `D/MainWindowActions.cpp` (`persistLooperState`) | **Missing** | Yes: a toggle and its status | DC | S | WS10 |
| 2.30 | Runs linked to an organization task, with status synced to the task | `D/MainWindowAgentLanding.cpp` (`syncOrgTaskAgentStatus`, `completeOrgTaskForSession`) | **Missing** | Yes | API | S | WS15 |
| 2.31 | Agent recycle bin | `D/MainWindowSettings.cpp` (`showAgentRecycleBinDialog`) | **Missing** | Yes | DC | S | WS10 |
| 2.32 | Agent records synced to the account (owner-sealed) | `D/MainWindowAgentSync.cpp` | **Done** (read side) | Yes | API | — | — |
| 2.33 | Local models (Gemma 4 through Ollama) | `MP/gemma-4`, `D/AgentRunner.h` | **Done**, native only: Gemma 4 through MLX (`NI/ForkMesh/Features/LocalAI/*`) | Adapted: MLX on the device | Device | — | — |
| 2.34 | High-memory processes panel (kill, stop agent) | `D/MainWindowDiagnostics.cpp` (`showHighMemoryProcessPanel`, `stopHighMemoryAgent`) | **Missing** | Adapted: see the list and stop an agent | DC | S | WS16 |
| 2.35 | Agent events: finished, failed, needs attention | `D/MainWindowAgentEvents.cpp` | **Partial**: doorbell only, no "agents" push topic | Yes: push plus a Live Activity | API | S | WS12 |
| 2.36 | Project agent access (folders, network) per project | `D/ProjectAgentAccess.h`, `D/MainWindowAgentAccess.cpp` | **Missing** | Yes | DC | S | WS10 |

## 3. Teammates (the Team page)

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 3.1 | Teammate grid and dashboard (status, duties, skills, output, assignment) | `D/MainWindowTeammates.cpp`, `D/TeammateStore.h` | **Missing** | Yes | DC | M | WS17 |
| 3.2 | Teammate editor: identity, provider and model, backup models, task agents, skills, schedule, log monitors, relay channel, permissions | `D/MainWindowTeammates.cpp` (`openTeammateEditor`, `appendTeammate*Fields`) | **Missing** | Adapted: core fields only; log monitors and permissions stay on the desktop | DC | M | WS17 |
| 3.3 | Run the team now; generate and read briefings | `D/MainWindowTeammates.cpp` (`runTeammateTeamNow`, `generateTeammateBriefing`) | **Missing** | Yes | DC | S | WS17 |
| 3.4 | Schedules (daily or on events); enable or disable all | `D/MainWindowTeammates.cpp` (`enableTeammateSchedule`, `disableAllTeammateSchedules`) | **Missing** | Yes | DC | S | WS17 |
| 3.5 | Team stats and task grades | `D/MainWindowTeammates.cpp` (`buildTeamStatsPage`, `setTeammateTaskGrade`) | **Missing** | Yes | DC | S | WS17 |
| 3.6 | Prompt, stop or continue a teammate | `D/MainWindowTeammates.cpp` (`sendTeammatePrompt`, `stopTeammate`, `continueTeammateSession`) | **Missing** | Yes | DC | S | WS17 |
| 3.7 | Stall-monitoring teammates | `D/MainWindowDiagnostics.cpp` (`maybeTriggerStallMonitoringTeammates`) | N/A | Desktop-only: watches the desktop's own UI thread | — | — | — |

## 4. Repositories and code

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 4.1 | Network repository catalog: remote and private/local tabs, commit sparkline, merge chart, mirrors cell | `D/MainWindowNetworkRepos.cpp` | **Partial**: `ReposScreen.swift`; no sparkline or merge chart | Yes | API | S | WS11 |
| 4.2 | Fork or mirror a network repository | `D/MainWindowNetworkRepos.cpp` (`forkNetworkRepo`, `mirrorNetworkRepo`) | **Partial**: mirror import (#31) | Yes | API | S | WS11 |
| 4.3 | Admin purge of a repository | `D/CatalogAdminDelete.h`, `D/MainWindowNetworkRepos.cpp` | **Missing** | Adapted: admins only, with Face ID | API | S | WS20 |
| 4.4 | Repository overview: README, latest commit, files | `D/MainWindowRepoDetail.cpp` (`buildRepoOverviewPage`), `D/MarkdownRender.h` | **Done** | Yes | API | — | — |
| 4.5 | File tree, preview, file tabs, go to file | `D/MainWindowRepoDetail.cpp` (`loadRepoFileTree`, `openRepoFile`), `D/FileTreeSupport.h` | **Done** (`RepoCodeTab.swift`, `RepoFileScreen.swift`) | Yes | API | — | — |
| 4.6 | Edit a file and save; propose a PR from an edit | `D/MainWindowRepoDetail.cpp` (`saveWorkingTreeFileEdit`, `proposePullFromMirrorEdit`) | **Done** (`QuickEditScreen.swift`) | Yes | API | — | — |
| 4.7 | New, rename and delete file entries | `D/MainWindowRepoDetail.cpp` (`newRepoFileEntry`, `renameRepoFileEntry`, `deleteRepoFileEntry`) | **Partial**: local repositories only | Yes | API | S | WS11 |
| 4.8 | File history | `D/MainWindowRepoDetail.cpp` (`showRepoFileHistory`) | **Missing** | Yes | API | S | WS11 |
| 4.9 | Code map: a treemap with the source drawn in each cell, and a 3D view | `D/CodeMapModel.h`, `D/CodeMapView.h`, `D/MainWindowCodeMap.cpp` | **Missing** | Adapted: 2D treemap only; the 3D view is GPU-heavy | API | M | WS11 |
| 4.10 | Size map (disk usage of volumes, elevated rescan) | `D/MainWindowSourceControl.cpp` (`buildSizeMapTab`), `D/DirectorySizeScan.h` | N/A | Desktop-only: scans the desktop's own filesystem (host disk usage is 7.8) | — | — | — |
| 4.11 | Files page: the real filesystem, editor tabs, find and replace, go to line, commit from the tab | `D/MainWindowFiles.cpp` | **Missing** | Adapted: browse the desktop's project folders read-only and send files both ways | DC | M | WS16 |
| 4.12 | Code editor: highlighting, find and replace, go to line | `D/MainWindowFiles.cpp`, `D/MarkdownEditor.h` | **Partial**: `CodeEditorView.swift`; no find/replace or go to line | Yes | Device | S | WS11 |
| 4.13 | Symbol outline and trace (what defines, what calls) | `D/CodeSymbols.h`, `D/MainWindowFiles.cpp` (`traceFilesSymbol`) | **Missing** | Adapted: an outline sheet with a callers list | Device | S | WS11 |
| 4.14 | Commits list: stats, load more, file rows, markers | `D/MainWindowRepoDetail.cpp` (`loadCommits`, `fillCommitStats`) | **Done** (`RepoCommitsTab.swift`) | Yes | API | — | — |
| 4.15 | Commit workspace: changes, one commit, and a range page | `D/MainWindow.h` (`kCommitWorkspace*`), `D/MainWindowRepoDetail.cpp` | **Partial**: a commit's diff; no range page | Yes | API | S | WS11 |
| 4.16 | Commit graph | `D/CommitGraph.h` | **Done** (`RepoGraphView.swift`, the Commits screen's Graph tab; [COMMIT_GRAPH.md](COMMIT_GRAPH.md)) | Adapted: a compact lane graph beside the commits list | API | M | WS11 |
| 4.17 | Source control: stage, unstage, discard, revert to base, ignore, delete, commit, commit and push, AI commit message, undo the latest outgoing commit | `D/MainWindowSourceControl.cpp` | **Partial**: on-device repositories commit and publish (#29) | Adapted: on-device repositories on the phone; the desktop's working trees over DC | Device + DC | M | WS16 |
| 4.18 | Branches: local and remote list, new, delete, delete merged, compare with base, range diff, pull base into all, update from base, merged celebration | `D/MainWindowBranches.cpp`, `D/WebBranchOps.h` | **Partial**: branch list and branch diff (`BranchDiffScreen.swift`) | Yes | API | M | WS11 |
| 4.19 | Clear a stale Git lock; open a branch in VSCodium (the `vscodium` package) | `D/MainWindowBranches.cpp` (`clearBranchGitLock`), `marketplace/packages/vscodium/content/desktop/VSCodiumDesktopExtension.cpp` | N/A | Desktop-only: acts on the desktop's clone and editor | — | — | — |
| 4.20 | Worktrees: list, switch, merge into main, remove, update from main, edit or resolve conflicts, commit | `D/MainWindowBranches.cpp` | **Partial**: read-only worktrees tab | Adapted: the actions go over DC | DC | M | WS10 |
| 4.21 | Diff viewer: unified and side by side, sticky file header, "Viewed" stamps with auto-mark on scroll, hunk navigation, search, zoom, in-place file swap, multi-page fold | `D/MainWindowBranches.cpp`, `D/MainWindowPulls.cpp`, `D/MainWindowSourceControl.cpp` | **Done** on both apps: one scrolling stream with sticky file headers, "Viewed" marks (fold, auto-mark on scroll, fingerprinted and persisted), change and file navigation, find, zoom, and the six health scores ([DIFF_REVIEW.md](DIFF_REVIEW.md)); no multi-page fold | Yes | Device | M | WS11 |
| 4.22 | Merge conflict editor and AI conflict fix | `D/MainWindowPulls.cpp` (`runMergeConflictEditor`, `aiFix*`), `D/MainWindowBranches.cpp` (`fixBranchConflictsWithAgent`) | **Missing** | Adapted: pick ours or theirs per hunk, or hand it to an agent | API + DC | M | WS11 |
| 4.23 | Stash-free flows (every change in its own worktree; no `git stash`) | design rule across `D/MainWindowBranches.cpp` | **Done** by design: the phone never stashes | Yes | — | — | — |
| 4.24 | Fetch, pull and sync a repository; sync badges | `D/MainWindowRepoFetch.cpp`, `D/MainWindowRepoDetail.cpp` (`fetchCurrentRepo`, `pullCurrentRepo`) | **Partial**: on-device repositories (#29) | Adapted: the desktop's repositories over DC | DC | S | WS16 |
| 4.25 | Download a ZIP | `D/MainWindowRepoDetail.cpp` (`downloadCurrentRepoZip`) | **Missing** | Yes: through the share sheet | API | S | WS11 |
| 4.26 | Fork a repository; configure the fork source | `D/MainWindowRepoDetail.cpp` (`forkCurrentRepo`, `configureForkSource`) | **Missing** | Yes | API | S | WS11 |
| 4.27 | Repository settings: General, Git, Agents, Automation, Danger zone; remotes; relay identity | `D/MainWindowRepoSettings.cpp` | **Missing** | Adapted: General, Agents, Automation and Danger zone (with Face ID); not remotes | API | M | WS11 |
| 4.28 | Repository permissions | `D/MainWindowControlNode.cpp` ("Repository permissions") | **Missing** | Yes | API | S | WS11 |
| 4.29 | Insights tab | `D/MainWindowSourceControl.cpp` (`buildInsightsTab`) | **Missing** | Yes | API | S | WS20 |
| 4.30 | Quality tab (code health) | `D/MainWindowSourceControl.cpp` (`buildRepoQualityTab`), `D/CodeHealth.h` | **Missing** | Adapted: read-only, computed by the desktop | DC | S | WS20 |
| 4.31 | Repository stats and Ratchet mode | `D/RepoStatsStore.h`, `D/MainWindowDiagnostics.cpp` (`toggleRepositoryRatchet`) | **Missing** | Adapted: view it and toggle it | DC | S | WS20 |
| 4.32 | Clone to device; mirror import | `D/MainWindowRepos.cpp` | **Done** (#30, #31) | Yes | API | — | — |
| 4.33 | Private mirrors: end-to-end encrypted replicas | `D/PrivateMirrorRuntime.h`, `D/PrivateMirrorStore.h`, `D/MirrorCrypto.h`, `D/PublicMirrorRuntime.h` | N/A | Desktop-only: serving replicas needs a long-running host | — | — | — |
| 4.34 | Reference links (`#12`, `owner/repo#3`, commit ids) | `D/ReferenceLinks.h`, `D/ChatLinkText.h` | **Done**: every message surface, both platforms ([REFERENCE_LINKS.md](REFERENCE_LINKS.md)) | Yes | Device | S | WS11 |
| 4.35 | The desktop's own source tree: upstream checkout sync, source watch | `D/UpstreamCheckoutSync.h`, `D/SourceCheckout.h`, `D/SourceTreeWatch.h` | N/A | Desktop-only: the Qt app's own rebuild loop | — | — | — |
| 4.36 | Git plumbing on the GUI thread | `D/StrictGitReader.h`, `D/GuiPump.h` | N/A | Desktop-only: infrastructure (the phone uses FMGit, WS5) | — | — | — |

## 5. Issues, pull requests, discussions, projects and tasks

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 5.1 | Issues list: tabs, filters, query, labels, milestones, priority chips, progress drag | `D/MainWindowIssues.cpp`, `D/IssueStore.h` | **Partial**: list and filters (`RepoIssuesTab.swift`); no priority or progress editing | Yes | API | S | WS11 |
| 5.2 | Issue thread: sub-issues, checklist, reactions, federated thread | `D/MainWindowIssues.cpp` (`renderIssueThread`, `addIssueSubIssuesSection`, `addIssueRemoteThread`) | **Partial**: `IssueDetailScreen.swift`; no reactions or checklist | Yes | API | S | WS11 |
| 5.3 | New issue with images; quick add with history; issue from a chat message; publish to ActivityPub | `D/MainWindowIssues.cpp` (`promptNewIssue`, `quickAddIssue`, `promptIssueFromChatMessage`, `publishQuickAddToActivityPub`) | **Partial**: `NewIssueScreen.swift`; no images | Yes | API | S | WS11 |
| 5.4 | Inline edits: title, labels, milestone, dates, priority, assignees; label definitions | `D/MainWindowIssues.cpp` (`save*Inline`, `editIssueLabelDefinition`) | **Partial** | Yes | API | S | WS11 |
| 5.5 | Issue board (columns, move a card, edit columns) | `D/MainWindowIssues.cpp` (`buildIssueBoard`, `moveIssueToColumn`) | **Missing** | Adapted: a paged board; "Move to…" on each card; drag on iPad | API | M | WS11 |
| 5.6 | Burnup chart | `D/IssueBurnup.h`, `D/MainWindowIssues.cpp` (`showIssueBurnupChart`) | **Missing** | Yes (Swift Charts) | API | S | WS20 |
| 5.7 | `@model` mentions in threads, and "Ask AI" | `D/IssueModelMention.h`, `D/MainWindowIssues.cpp` (`askAiForCurrentIssue`) | **Missing** | Yes | API + DC | S | WS11 |
| 5.8 | Reprioritize the backlog; prioritize from the README | `D/MainWindowIssues.cpp` (`reprioritizeBacklog`, `prioritizeIssuesFromReadme`) | **Missing** | Yes: queues an agent | API | S | WS11 |
| 5.9 | Link an issue to a PR, and close linked issues | `D/MainWindowPulls.cpp` (`linkPullToIssueFromIssuePage`, `closeIssuesLinkedFromPull`) | **Missing** | Yes | API | S | WS11 |
| 5.10 | Copy an issue or its thread; send it to the prompt | `D/MainWindowIssues.cpp` (`copyIssueThreadToClipboard`, `appendTextToActivePrompt`) | **Missing** | Yes | Device | S | WS11 |
| 5.11 | Delete an issue (with its history) | `D/MainWindowIssues.cpp` (`deleteCurrentIssueWithHistory`) | **Missing** | Adapted: owners only, with Face ID | API | S | WS11 |
| 5.12 | PR list with a branch-state column (conflicts, ahead/behind, dirty worktree) | `D/MainWindowPulls.cpp`, `D/PullBadgeWidget.h` | **Partial**: `RepoPullsTab.swift` | Yes | API | S | WS11 |
| 5.13 | PR detail: Conversation, Commits, Files and Checks | `D/MainWindowPulls.cpp` (`buildPullConversation`, `buildPullCommitsList`, `buildPullFilesPage`, `buildPullChecksPage`) | **Partial**: timeline in `PullDetailScreen.swift`; no Files or Checks page | Yes | API | M | WS11 |
| 5.14 | Inline review comments, batched review, thread replies and resolution, thread anchoring | `D/MainWindowPulls.cpp` (`openPullInlineComposer`, `submitPullInlineReviewBatch`), `D/PullReviewModel.h` | **Missing** | Yes | API | M | WS11 |
| 5.15 | Submit a review (approve, request changes) | `D/MainWindowPulls.cpp` (`submitPullReview`) | **Missing** | Yes | API | S | WS11 |
| 5.16 | Merge with the plan, blockers and conflict card; close, reopen; update the branch | `D/MainWindowPulls.cpp` (`mergeCurrentPull`, `updatePullMergeBlockers`, `updateCurrentPullBranch`) | **Partial**: merge plan and merge (`ApiService+Agents.swift`) | Yes | API | S | WS11 |
| 5.17 | AI review; fix findings or conflicts with an agent (or the one that wrote it); apply a suggestion | `D/PullAiReview.h`, `D/MainWindowPulls.cpp` (`reviewCurrentPullWithAi`, `fixCurrentPullFindingsWithAgent`, `applyPullSuggestionFix`) | **Missing** | Yes: queues a job | API + DC | M | WS11 |
| 5.18 | New PR from a branch, a directory, a source, or an imported patch | `D/MainWindowPulls.cpp` (`promptNewPull*`, `importPatchAsPull`) | **Partial**: from on-device repositories and quick edit | Adapted: from a branch, or a patch from the Files app | API | S | WS11 |
| 5.19 | Merge queue per repository: enable, pause, add, remove, reorder, process | `D/MergeQueue.h`, `D/MainWindowMergeQueue.cpp` | **Missing** | Yes: view and edit; the desktop processes it | DC | S | WS11 |
| 5.20 | Build and preview a PR; run or approve its checks | `D/MainWindowPulls.cpp` (`buildAndPreviewCurrentPull`, `runChecksForCurrentPull`, `approveChecksForCurrentPull`) | **Missing** | Yes | API + DC | S | WS14 |
| 5.21 | Bounties: fund a merged PR, auto-bounty, watch the payout | `D/MainWindowPulls.cpp` (`fundBountiesForMergedPull`, `pollBountyPayout`) | **Partial**: read-only (#33) | Adapted: no wallet keys on the phone; funding is signed on the desktop | API + DC | S | WS19 |
| 5.22 | Discussions: list, detail, create, comment, status, delete, inbox sync, federated replies | `D/MainWindowDiscussions.cpp`, `D/DiscussionStore.h`, `D/FederatedThreadView.h`, `D/DiscussionInboxBackoff.h` | **Partial**: detail and federated replies (`DiscussionDetailScreen.swift`); no create, status or delete | Yes | API | S | WS11 |
| 5.23 | Repository projects (signed project events) with a Gantt chart and linked issues | `D/MainWindowProjects.cpp`, `D/ProjectStore.h`, `D/ProjectGantt.h` | **Missing** | Adapted: a timeline list on iPhone and a Gantt on iPad | API | M | WS15 |
| 5.24 | Milestones: create, update, progress | `D/MainWindowIssues.cpp` (`refreshIssueMilestones`, `editIssueMilestone`) | **Partial**: chosen in `NewIssueScreen.swift` | Yes | API | S | WS15 |
| 5.25 | Organization tasks: list, detail, history, create or quick add, follow-up, assign to an agent, queue the agent, check-in, comment, actions, move to an issue, delete closed, badge, attachments | `D/MainWindowTasks.cpp`, `MP/tasks` | **Partial**: create, check in, remind the assignee, complete, reply (`TasksScreen.swift`) | Yes | API | M | WS15 |
| 5.26 | Signed inbox writes for issues, PRs and comments | `D/MainWindowRepoSync.cpp` | **Done** (`InboxService.swift`) | Yes | API | — | — |
| 5.27 | Contributor reward estimates and payout receipts | `D/ContributorPayoutReceipt.h` | **Partial**: payouts in Activity | Yes (read) | API | S | WS19 |

## 6. Actions (CI), security, webhooks and releases

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 6.1 | Actions: workflows, runs table, run log, step progress, manual run, node picker, parallel runs, "watch all" | `D/MainWindowActions.cpp`, `D/ActionFile.h`, `D/ActionStore.h` | **Partial**: runs and logs, read-only (`RepoActivityTabs.swift`) | Yes: dispatch and watch; runs happen on runners or desktops | API + DC | M | WS14 |
| 6.2 | Approve, reject, rerun or stop a run; the approval gate (hash of the workflow file) | `D/MainWindowActions.cpp` (`approveActionRun`, `rerunActionRun`) | **Missing** | Yes: approve with Face ID | API + DC | S | WS14 |
| 6.3 | Send a failed run to an agent | `D/MainWindowActions.cpp` | **Missing** | Yes | API | S | WS14 |
| 6.4 | Checks on PRs | `D/MainWindowActions.cpp` (`refreshOpenPullChecks`, `queueChecksForOpenPulls`) | **Partial** | Yes | API | S | WS14 |
| 6.5 | Action runner with a persistent ccache and build cache | `D/ActionRunner.h` | N/A | Desktop-only: local compilers and long jobs | — | — | — |
| 6.6 | Push hooks, commit-signal hooks, action spool, mirror actions configuration | `D/MainWindowActions.cpp` (`ensurePushHook`), `D/MirrorActionsConfiguration.h`, `D/MirrorActionsSummary.h` | N/A | Desktop-only: Git hooks in the desktop's clones | — | — | — |
| 6.7 | Action telemetry (where a record came from) | `D/ActionTelemetry.h` | N/A | Desktop-only: runner infrastructure | — | — | — |
| 6.8 | Security tab: dependency scan, vulnerability snapshot, open a file at a line | `D/RepoSecurity.h`, `D/MainWindowSourceControl.cpp` (`buildRepoSecurityTab`, `runRepoDependencyScan`); server `app/src/security_scan_ingest.py` | **Missing** | Yes: results, and "scan now" | API | S | WS14 |
| 6.9 | Report a vulnerability | `D/MainWindowSettings.cpp` (`buildVulnReportTab`) | **Missing** | Yes | API | S | WS18 |
| 6.10 | Repository webhooks (list, add, delete, test; four event points) | `D/MainWindowActions.cpp`, `D/MainWindowSettings.cpp`; server `app/src/repository_webhooks.py` | **Missing** | Yes | API | S | WS14 |
| 6.10a | Mirror a repository to GitHub or GitLab (token, per-feature direction, manual pick of imports) | web Settings card; server `app/src/repository_provider_mirror.py` ([../app/provider-mirrors.md](../app/provider-mirrors.md)) | **Done** (2026-10-13): Settings › Mirror to GitHub or GitLab, and the mirror button in the repository's top bar (`ProviderMirrorSections.swift`, `FMCode/CI/ProviderMirrorModel.swift`) | Yes | API | S | — |
| 6.11 | Releases: list, detail, new release, notes written by an agent, delete a tag, prune tags | `D/MainWindowReleases.cpp` | **Partial**: list and detail (`RepoInfoTabs.swift`) | Yes | API | M | WS14 |
| 6.12 | Announce a release on the Fediverse or Mastodon | `D/MainWindowReleases.cpp` (`announceReleaseOnFediverse`, `postReleaseToMastodon`) | **Missing** | Yes | API | S | WS14 |
| 6.13 | Artifacts tab: list, delete, prune | `D/MainWindowReleases.cpp` (`buildArtifactsTab`, `pruneReleaseArtifacts`) | **Partial**: release assets | Yes | API | S | WS14 |
| 6.14 | Release artifact replication and rollout to mirrors | `D/MainWindowReleases.cpp` (`replicateReleaseArtifacts`, `pushReleaseToMirrors`) | N/A | Desktop-only: moves blobs between the desktop and its hosts; the phone shows the rollout state (6.11) | — | — | — |
| 6.15 | Repository mirrors on Network > Nodes, merged into the node rows (was the per-repository Mirror nodes tab): status lights, online only, SSH metadata, roles, a diagnostics prompt | `D/MainWindowReleases.cpp` (`buildMirrorNodesSection`), `D/MainWindowNodes.cpp` (`mergeMirrorRowsIntoNodesTable`) | **Partial**: mirrors tab | Yes | API | S | WS14 |
| 6.16 | Mirror push (fan out over SSH remotes) | `D/MainWindowMirrorPush.cpp`, `D/MirrorPushOutcome.h` | **Missing** | Adapted: trigger it and see the outcome; the SSH keys stay on the desktop | DC | S | WS16 |
| 6.17 | Mirror sync verdicts and human-readable sync status | `D/MirrorSyncVerdict.h`, `D/SyncStatusModel.h` | **Partial** | Yes (read) | API | S | WS14 |

## 7. Hosts, mirrors, fleet and the Control page

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 7.1 | Hosts table: saved hosts, fleet summary, probe, health | `D/MainWindowHosts.cpp` (`buildHostsSection`, `probeSavedHosts`) | **WS7** | Yes | DC | — | WS7 |
| 7.2 | Live view of host logs (node log and journal over one SSH connection) | `D/HostLogSources.h`, `D/MainWindowHosts.cpp` (`buildHostsLiveView`) | **WS7** | Yes | DC | — | WS7 |
| 7.3 | Add or forget a host; SSH password prompt | `D/MainWindowHosts.cpp` (`addHostFromForm`, `forgetHostAtRow`, `promptHostDiskSshPassword`) | **Missing** | Adapted: the desktop stores credentials; they never reach the phone | DC | S | WS16 |
| 7.4 | Install, reinstall, update from source, deploy to all in parallel, uninstall | `D/MainWindowHostInstall.cpp` | **Missing** | Yes: start and follow; uninstall needs Face ID | DC | M | WS16 |
| 7.5 | Host link code (link a host to the account) | `D/MainWindowHostInstall.cpp` (`promptHostLinkCode`) | **Missing** | Yes | DC + API | S | WS16 |
| 7.6 | Vultr mirror provisioning: stages, progress, resume, DNS, seed push, publication check | `D/MainWindowVultrMirrors.cpp` | **Missing** | Yes: start, resume and follow | DC | M | WS16 |
| 7.7 | Destroy a Vultr instance; reconcile the desired fleet, with a countdown | `D/MainWindowHosts.cpp` (`destroyVultrHostAtRow`, `reconcileDesiredMirrorFleet`) | **Missing** | Adapted: confirmed with Face ID | DC | S | WS16 |
| 7.8 | Host disk usage browser | `D/MainWindowHosts.cpp` (`browseHostDiskUsageForSelection`) | **Missing** | Yes | DC | S | WS16 |
| 7.9 | Install the agent CLIs on a host | `D/MainWindowHosts.cpp` (`installAgentClisForHost`) | **Missing** (the login terminal is **Done**) | Yes | DC | S | WS16 |
| 7.10 | Host actions configuration (runner labels) | `D/MainWindowHosts.cpp` (`configureHostActionsForSelection`) | **Missing** | Adapted: toggle labels | DC | S | WS16 |
| 7.11 | Control > Mirror client | `D/MainWindowControlNode.cpp`, `D/ControlNode.h` | **Missing** | Adapted: status only | DC | S | WS16 |
| 7.12 | Control > Keys and wallet (the public wallet address) | `D/MainWindowControlNode.cpp` | **Missing** | Adapted: show the public address and a QR code | API | S | WS18 |
| 7.13 | Control > Cloudflare relay bootstrap and Site deployment | `D/MainWindowControlNode.cpp` | N/A | Desktop-only: operator deploy tooling (Next runs no Cloudflare Worker for the app) | — | — | — |
| 7.14 | Control > Reward pool (owner signs Solana transfers) | `D/MainWindowRewardPool.cpp`, `D/RewardPoolSigner.h` | N/A | Desktop-only: the signing key stays on the desktop; the phone sees the pool in Ledger (11.8) | — | — | — |
| 7.15 | Control > API token | `D/MainWindowControlNode.cpp` | **Missing** | Adapted: show and rotate it | API | S | WS18 |
| 7.16 | Control > Export system, Import and deploy | `D/MainWindowControlNode.cpp` | N/A | Desktop-only: whole-system migration of the desktop | — | — | — |
| 7.17 | Mirror gateway health | `D/MirrorGatewayHealth.h` | **Partial** | Yes (read) | API | S | WS16 |
| 7.18 | Local virtual machine runtime | `D/VirtualMachineRuntime.h`, `D/MainWindowSettings.cpp` (`buildSettingsVmParts`) | N/A | Desktop-only: runs a VM on the desktop | — | — | — |
| 7.19 | Serve as a node: chat history, heartbeat, node event socket, catalog publish | `D/ServerNode.h`, `D/NodeEventSocket.h`, `D/MainWindowSetup.cpp` (`sendNodeHeartbeat`) | **WS5** | Adapted: serves only while iOS lets the app run | API | — | WS5 |
| 7.20 | Node self-diagnostics pushed in heartbeats | `D/NodeDiagnostics.h` | **Missing** | Adapted: shown on each node's row | API | S | WS20 |
| 7.21 | SSH terminal to a saved host | `D/MainWindowHosts.cpp` (`openHostAgentLoginTerminal`, the live shell behind Sign in) | **Done** (`NI/ForkMesh/Features/Hosts/HostTerminalScreen.swift`; Android `feature/hosts/HostTerminalScreen.kt`) | Adapted: the desktop runs `ssh` in a pty with its own key; the phone confirms, then attaches over `terminal.*` (PARITY.md, WS7 notes) | DC | — | WS7 |
| 7.22 | Install (binary): upload the desktop's build to one host | `D/MainWindowHosts.cpp` (row button), `D/MainWindowHostInstall.cpp` (`runHostInstall(true)`) | **Done** (a host action, `installBinary`) | Yes | DC | — | WS7 |

## 8. Network, mesh, nodes and users

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 8.1 | Network page: Relays, Nodes, Hosts, Endpoints, WebSocket objects, Web requests | `D/MainWindowNetworkDiagnostics.cpp` | **Partial**: relays, and the connection log in Activity | Adapted: Relays, Nodes and Endpoints, read-only | API | M | WS20 |
| 8.2 | Outbound firewall: rules, recent decisions, and Allow/Deny/Deny-once prompts | `D/BackoffNetworkAccessManager.h`, `D/NetworkBackoff.h`, `D/MainWindowNetworkDiagnostics.cpp` | **Missing** | Adapted: answer the desktop's prompts and see recent decisions; iOS gives apps no firewall of their own | DC | S | WS10 |
| 8.3 | Nodes directory: platform, online, owner, catalog info | `D/MainWindowNodes.cpp` | **Missing** (Activity has network stats) | Yes | API | S | WS20 |
| 8.4 | Delete a node completely: destroy its Vultr server, remove its DNS | `D/MainWindowNodes.cpp` (`deleteMeshNodeCompletely`, `destroyVultrServerForNode`) | **Missing** | Adapted: confirmed with Face ID | DC | S | WS16 |
| 8.5 | Node profile: hosting stats, account status, linked nodes, link a node to a user, SOL balance | `D/MainWindowNodes.cpp` (`buildNodeProfilePanel`, `promptLinkNodeToUser`, `checkNodeBalance`) | **Partial**: account nodes card (#6) | Yes | API | S | WS18 |
| 8.6 | Mesh firewall check and the "Allow" banner | `D/MeshFirewall.h`, `D/MainWindowSettings.cpp` (`checkLocalMeshFirewall`) | N/A | Desktop-only: a host firewall; iOS asks for local network permission instead | — | — | — |
| 8.7 | Local mesh room (chat over the LAN) | `D/LocalMeshNode.h` | **Done** (#39) | Yes | Device | — | — |
| 8.8 | Local control: the phone pairs with and drives the desktop | `D/LocalControl.h`, `D/MainWindowLocalControl.cpp` | **Done** on the LAN (#59); **WS7** away from home | Yes | DC | — | WS7 |
| 8.9 | Users admin: all users, signup and referral codes, activity, quarantine and unquarantine, delete | `D/MainWindowUsers.cpp`; server `app/src/account_users.py` | **Missing** | Adapted: admins only; batch deletes need Face ID | API | S | WS20 |
| 8.10 | HTTP/3 client | `D/Http3Get.h` | **Done** by the system (URLSession speaks HTTP/3) | Yes | API | — | — |
| 8.11 | App log archive and web-request log | `D/NetworkLogArchive.h`, `D/PlatformLogFilter.h` | **Done** for the phone's own log (`AppTelemetry.swift`) | Yes | Device | — | — |
| 8.12 | QR codes (node linking, payment addresses) | `D/QrCode.h` | **Missing** | Yes: show them, and scan the desktop's with the camera | Device | S | WS18 |

## 9. Pings, chat, DMs and World

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 9.1 | Pings page: list, delete selected, web alerts pager, open a row, acknowledge, re-raise unacknowledged, notification journal | `D/MainWindowActions.cpp` (`buildNotificationsSection`, `refreshWebAlerts`), `D/PingSyncState.h` | **Done** (`NotificationsScreen.swift`) | Yes | API | — | — |
| 9.2 | Pause pings menu; send a ping to the prompt | `D/MainWindowActions.cpp` (`popupPingPauseMenu`, `sendPingToPrompt`) | **Missing** | Yes | API | S | WS9 |
| 9.3 | Ping settings: kinds, email, quiet hours, desktop-only kinds | `D/PingSettings.h`, `D/PingSettingsPanel.h` | **WS2** | Yes | API | — | WS2 |
| 9.4 | Chat channels, roster, unread, typing, avatars, sender profile | `D/MainWindowMessages.cpp`, `D/MainWindowChat.cpp` | **Partial**: channels, roster, unread, DMs (#36); no typing or profiles | Yes | API | S | WS13 |
| 9.5 | Threads (reply count, latest replier, faces) | `D/MessageRow.h`, `D/MainWindowMessages.cpp` (`openChatThread`) | **Missing** | Yes | API | M | WS13 |
| 9.6 | Reactions | `D/ReactionCatalog.h`, `D/ReactionEmoji.h` | **Missing** | Yes: system emoji for the same slugs | API | S | WS13 |
| 9.7 | Edit or delete your own message; admin delete | `D/MainWindowMessages.cpp` (`promptEditMessage`, `confirmAdminDeleteMessage`) | **Missing** | Yes | API | S | WS13 |
| 9.8 | Channel groups: create, collapse, move a channel | `D/ChannelGroups.h`, `D/MainWindowMessages.cpp` | **Missing** | Yes | Device | S | WS13 |
| 9.9 | Private channels: add, invite, rename, delete; channel settings; room seal prompt | `D/MainWindowMessages.cpp` (`promptAddPrivateChannel`, `promptInviteToPrivateChannel`, `promptRoomSeal`) | **Missing** | Yes | API | M | WS13 |
| 9.10 | Direct messages | `D/MainWindowMessages.cpp` (`openDirectChat`) | **Done** | Yes | API | — | — |
| 9.11 | Mention autocomplete, emoji picker, insert link | `D/MainWindowMessages.cpp` (`updateMentionPopup`, `showEmojiPicker`) | **Missing** | Yes | Device | S | WS13 |
| 9.12 | Ask ForkBot in chat | `D/MainWindowMessages.cpp` (`maybeAskForkbot`) | **Missing** | Yes | API | S | WS13 |
| 9.13 | Turn a message into an issue or a prompt | `D/MainWindowMessages.cpp` (`sendMessageToPrompt`), `D/MainWindowIssues.cpp` (`createIssueFromChatCommand`) | **Missing** | Yes | API + Device | S | WS13 |
| 9.14 | Media in chat (audio and video player, images) | `D/MainWindowMessages.cpp` | **Missing** | Yes: `AVPlayer` | API | S | WS13 |
| 9.15 | Office channels (separately encrypted rooms with 60 s tickets) | `D/OfficeChannelMirror.h` | **Missing** | Yes | API | M | WS13 |
| 9.16 | The desktop's `#team` room for teammates | `D/MainWindowMessages.cpp` (`ensureTeamRoom`) | **Missing** | Adapted: read the teammates' updates | DC | S | WS17 |
| 9.17 | Discord bridge (a bot's channels, threads and DMs in the chat sidebar) | `MP/discord` (`DiscordClient.cpp`) | **Missing** | Adapted: read and reply through the desktop, which keeps the bot token | DC | M | WS13 |
| 9.18 | Visitor presence and the welcome announcement | `D/ChatVisitorPresence.h`, `D/MainWindowMessages.cpp` (`maybeAnnounceWelcome`) | **Partial** | Yes | API | S | WS13 |
| 9.19 | Chat history limits and local persistence | `D/ChatHistoryLimits.h`, `D/MainWindowSetup.cpp` (`saveChatHistory`) | **Partial** | Yes | Device | S | WS13 |
| 9.20 | User status and avatars | `D/UserStatus.h`, `D/UserAvatarStore.h` | **Done** (#69) | Yes | API | — | — |
| 9.21 | Room crypto and room seal | `D/RoomCrypto.h`, `D/RoomSeal.h` | **Done** (#37) | Yes | Device | — | — |
| 9.22 | World (the hosted 3D world) | `world/`, the desktop's link | **Done**: opens World (#20) | Adapted: the hosted World in the browser | API | — | — |
| 9.23 | World speech bridge (the desktop's speech-to-text for World) | `D/WorldSpeechBridge.h` | N/A | Desktop-only: a localhost bridge to desktop speech-to-text | — | — | — |
| 9.24 | ForkMesh Apps in chat: app slash commands with suggestions and `/apps`, replies only you see, interactive app messages (block kit buttons and selects), forwarding a message that names an app | None yet (web: `app/public/chat-apps.js`; docs/APPS.md "Client surfaces") | **WS22** | Yes | API | — | WS22 |

## 10. Notes, calendar and timers

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 10.1 | Notes: local and cloud list, Markdown editor with live preview, visibility, share, versions | `D/MainWindowNotes.cpp`, `D/MarkdownEditor.h`, `D/NoteListEntry.h`, `MP/notes` | **Done** (`NotesScreen.swift`, #40) | Yes | API | — | — |
| 10.2 | Notes: dropped or pasted images; attach a conversation; turn a note into a task or an issue | `D/MainWindowNotes.cpp` (`attachNoteConversation`, `convertNoteToTask`, `convertNoteToIssue`), `D/MarkdownEditor.h` | **Missing** | Yes | API | S | WS15 |
| 10.3 | Calendar: month and week views, personal and organization events, repository commits and merges, event dialog | `D/MainWindowCalendar.cpp`, `D/RepositoryCalendar.h`, `MP/calendar`; server `app/src/calendar_api.py` | **Missing** | Yes: plus an optional EventKit export | API | M | WS15 |
| 10.4 | Timers: the top-bar clock; timer, countdown and alarm chips that survive a restart; timer pings | `D/MainWindowTimers.cpp`, `MP/timers`; server `app/src/timer_pings.py` | **Missing** | Yes: local notifications, a Live Activity for a countdown, AlarmKit for alarms | Device + API | S | WS15 |

## 11. Marketplace and packages

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 11.1 | Market: All, Installed and Not installed; install and remove; package settings; install counts; export and import package data | `D/MainWindowMarket.cpp`, `D/ForkMeshPackageRegistry.h` | **Missing** | Adapted: browse, and enable account-level packages; desktop-native packages say "on your desktop" | API | M | WS19 |
| 11.2 | Package extensions: native package pages, log tabs, search contributions, branch diff actions | `D/DesktopPackageExtension.h`, `D/NativePackageExtension.h`, `D/MainWindowPackageExtensions.cpp` | **Missing** | Adapted: packages with a web or worker surface render natively; others are listed only | API | S | WS19 |
| 11.3 | CRM: pipeline stage strip, contacts, organizations, activity, outreach, tracked investor-deck links, linked Gmail/Zoho/Mastodon threads, shared sheets | `MP/crm/content/desktop/CrmFeature.cpp`, `CrmStageStrip.cpp` | **Done** on iOS and Android: `NI/ForkMesh/Features/Packages/Crm*.swift`, Android `feature/market/Crm*.kt` (README 2026-10-02); unsent note drafts, last-contacted dates, contact links, restoring deleted columns, the team chart's `tz` and the Zoho Self Client code added 2026-10-07. Not the clone-based imports or the desktop table's named column presets | Yes | API (package worker) | L | WS19 |
| 11.4 | Marketing: content plan and ads across 100 channels | `MP/marketing/content/desktop/MarketingFeature.cpp` | **Done** on iOS and Android (README 2026-10-07): every desktop section (dashboard, content, campaigns, channels, ads manager, growth, Reddit desk, performance) and its settings; only the browser extension's page capture opens the web page | Yes | API (package worker) | L | WS19 |
| 11.5 | Product analytics: events, funnels, cohorts, session replay, heatmaps, flags, experiments, surveys (web and worker only; no desktop page) | `MP/analytics` | **Missing** | Adapted: read-only dashboards; no replay or heatmaps | API | M | WS19 |
| 11.6 | Competitor watchlist (web and worker) | `MP/competitor` | **Missing** | Yes | API | S | WS19 |
| 11.7 | ForkMesh Ads for advertisers and admin review | server `app/src/ad_network.py`, `/dashboard/ads` | **Done** for review (`NI/ForkMesh/Features/Packages/AdsScreen.swift`) | Adapted: ad reviewers get the review queue. Advertisers manage ads on the web; ad slots are a membership perk, so iOS does not show them (`IOS_RELEASE.md`, "App Review: nothing paid on iOS") | API | S | WS19 |
| 11.8 | Ledger: public financial records, your own expense and income records, wallet balances, paying pending contributor rewards | `D/MainWindowLedger.cpp`, `MP/ledger`; server `app/src/ledger.py` | **Done** (`NI/ForkMesh/Features/Packages/LedgerScreens.swift`) | Adapted: read the public records and rewards, and keep your own records. No wallet balance and no paying from iOS (`IOS_RELEASE.md`, "App Review: nothing paid on iOS") | API | M | WS19 |
| 11.9 | Tips between members (Solana, key in a passphrase-encrypted vault) | `D/MainWindowTips.cpp`, `D/TipSigner.h` | N/A on iOS | Not on iOS: removed 2026-10-04 (`IOS_RELEASE.md`, "App Review: nothing paid on iOS") | — | — | WS19 |
| 11.10 | Treasury donation dialog | `D/MainWindowDiagnostics.cpp` (`showTreasuryDonateDialog`) | N/A on iOS | Not on iOS: removed 2026-10-04 (`IOS_RELEASE.md`, "App Review: nothing paid on iOS") | — | — | WS19 |
| 11.11 | Password vault | `MP/vault/content/desktop/VaultFeature.cpp` | **Done** (#43) | Yes | API | — | — |
| 11.12 | Vault sharing with a user or organization; fills in other apps | `MP/vault`, `MP/browser-extension` | **Partial**: sharing is not verified on iOS; AutoFill and import/export from Apple Passwords and DuckDuckGo built 2026-10-02, not yet run on a device | Adapted: an iOS AutoFill credential provider | API + Device | M | WS21 |
| 11.13 | Voice changer ("ForkMesh Voice" microphone for other apps) | `MP/voice-changer/content/desktop/*` | N/A | Desktop-only: iOS lets no app provide a system microphone to other apps | — | — | — |
| 11.14 | Calories | `MP/calories/content/desktop/CaloriesFeature.cpp` | **Done** (#42) | Yes | API | — | — |
| 11.15 | Capture: screen region, markup, attach to a prompt | `MP/capture/content/desktop/*` | **Done** (`PromptMarkup.swift`; Android `PhotoMarkup.kt`), not yet run on a device ([CAPTURE_MARKUP.md](CAPTURE_MARKUP.md)) | Adapted: capture the app's own screen from the composer, the share extension "Send to agent" for other apps, and PencilKit markup on any attached photo | Device | — | — |
| 11.16 | Sentry: unresolved issues, Log activity lights | `MP/sentry/content/desktop/*` | **Missing** | Adapted: a read-only list | DC | S | WS20 |
| 11.17 | ForkMesh IDE (VS Code extension) | `MP/forkmesh-ide` | N/A | Desktop-only: an editor extension | — | — | — |
| 11.18 | Browser extension (Chrome) | `MP/browser-extension` | N/A | Desktop-only: a desktop browser extension; the phone's equivalent is 11.12 | — | — | — |
| 11.19 | World packages (sky office, banner boards) | `MP/forkmesh-sky-office`, `MP/*-banner-board` | **Done** through World (9.22) | Adapted | API | — | — |
| 11.20 | Suggested apps (install host programs such as whisper.cpp, Parakeet, VSCodium) | `D/SuggestedApps.h`, `D/MainWindowSettings.cpp` (`buildSuggestedAppsTab`) | N/A | Desktop-only: installs programs on the desktop | — | — | — |
| 11.21 | Publishing a package from the Market | `D/MainWindowMarket.cpp` | N/A | Desktop-only: the package authoring and build toolchain | — | — | — |
| 11.22 | Package star ratings and required improvement reviews: rate 1–5 stars (under 5 needs "what didn't work" and "how to improve"), reviews list with owner replies and "addressed in vX.Y", helpful votes, abuse reports; maintainers' "how to improve" view; report install/use with `POST /api/market/reviews/uses` | web `/market-reviews` (`app/public/market-reviews.js`); server `app/src/marketplace_reviews.py`; API and payloads in `docs/MARKETPLACE_RATINGS.md` | **Missing** | Yes: the star picker expands the required fields below 5 stars; show the server's `fields` errors | API | S | WS19 |

## 12. Stats, health, logs and diagnostics

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 12.1 | Stats board: tiles for repositories, issues, agents, tool efficiency, host load and logs; add, remove, drag and resize; layout kept | `D/MainWindowStats.cpp`, `D/StatsDashboard.h` | **Partial** (`NI/ForkMesh/Features/Insights/StatsScreen.swift`, `NIC/FMInsights/StatsBoard*.swift`): three-column board with status colours, trend chips and sparklines from a local reading history, plus Codemagic minutes left over `stats.codemagic`; network, status and desktop-health tiles only | Adapted: a fixed tile grid you can reorder (Swift Charts) | API + DC | M | WS20 |
| 12.2 | Component health: a card per source file, drill-down to functions, score | `D/MainWindowComponentHealth.cpp`, `D/ComponentHealthBoard.h`, `D/CodeHealth.h` | N/A | Desktop-only: measures the desktop app's own source | — | — | — |
| 12.3 | Memory page: mappings, allocator, the app's own internals | `D/MainWindowMemory.cpp`, `D/ProcessMemory.h`, `D/MemoryUsageChart.h` | N/A (the phone has its own performance monitor, #72) | Desktop-only: the desktop process's `/proc` | — | — | — |
| 12.4 | Log page > Activity: the desktop's log with categories, filters, follow, pop-out, full log | `D/MainWindowLogs.cpp`, `D/MainWindowSettings.cpp` (log functions), `D/LogSource.h` | **Done** for the phone's own log (`TelemetryScreen.swift`) | Adapted: tail the desktop's log | DC | S | WS16 |
| 12.5 | Log page > the 30-day activity timeline | `D/LogTimelineChart.h` | **Missing** | Adapted | DC | S | WS20 |
| 12.6 | Log page > Most frequent, Usage, D1 usage, Build (worker footprint), Cron | `D/MainWindowLogs.cpp` | **Missing** | Adapted: admins only, read-only; on Next these become database usage and job health (rqlite, no Worker) | API | M | WS20 |
| 12.7 | Log page > Database: live table inventory, paged rows, audited insert, update and delete | `D/D1DatabaseAdmin.h`, `D/DatabaseManagerWidget.h`, `D/D1QuerySource.h`; server `app/src/admin_database.py` | **Partial** (iOS reads via `/api/admin/phone/database`, 2026-10-01) | Adapted: read-only on the phone; writes stay on the desktop | API | M | WS20 |
| 12.8 | Admin error console and website status probes | `D/MainWindowChat.cpp` (`openAdminErrorConsole`, `refreshDesktopWebsiteProbes`); server `app/src/status_monitoring.py` | **Missing** | Yes | API | S | WS20 |
| 12.9 | The desktop's stall watchdog and stall log; send one to an agent | `D/StallWatchdog.h`, `D/MainWindowDiagnostics.cpp` (`sendStallLogToAgent`) | **Done** for the phone's own stalls (#72) | Adapted: read the desktop's and send one to an agent | DC | S | WS16 |
| 12.10 | Crash handler, crash log scan, crash repair dispatch | `D/CrashHandler.h`, `D/CrashLogScan.h` | **Missing** | Adapted: MetricKit crash and hang reports on the phone, with "Send to agent" | Device | S | WS20 |
| 12.11 | Diagnostics dialog, Qt diagnostics, fd pressure, system pressure, system stats | `D/MainWindowDiagnostics.cpp`, `D/QtRuntimeDiagnostics.h`, `D/SystemPressure.h`, `D/SystemStats.h` | N/A | Desktop-only: Qt and host internals; the phone shows the desktop's summary in its Health card (12.12) | — | — | — |
| 12.12 | Desktop health summary (CPU, memory, pressure, agents running) | `D/SystemStats.h`, `D/SystemPressure.h` | **Missing** | Adapted: one card per paired desktop | DC | S | WS16 |
| 12.13 | Failures of local operations reported as notices or pings | `D/ClientErrorReports.h` | **Partial** | Yes | Device | S | WS9 |
| 12.14 | Bug report | `D/MainWindowSettings.cpp` | **Done** (#73) | Yes | API | — | — |
| 12.15 | Background activity attribution; display readiness | `D/BackgroundActivity.h`, `D/DesktopDisplayReadiness.h` | N/A | Desktop-only: desktop infrastructure | — | — | — |

## 13. Settings (every section of Settings)

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 13.1 | Profile: rename, avatar (push, adopt the web avatar), account profile dialog | `D/MainWindowAccounts.cpp`, `D/MainWindowSettings.cpp` (`pushAccountAvatar`, `showEditAccountProfileDialog`) | **Done** (#70) | Yes | API | — | — |
| 13.2 | General: startup, appearance, footer, repository sync | `D/MainWindowSettings.cpp` (`buildSettingsGeneralTab`) | **Partial**: appearance | Adapted: appearance, haptics and sounds; startup and footer are desktop-only | Device | S | WS18 |
| 13.3 | Apps (suggested apps) | `D/MainWindowSettings.cpp` (`buildSuggestedAppsTab`) | N/A | Desktop-only: see 11.20 | — | — | — |
| 13.4 | Repos: sync interval, default locations | `D/MainWindowSettings.cpp` (`buildSettingsReposTab`) | **Missing** | Adapted: defaults for on-device repositories | Device | S | WS18 |
| 13.5 | SSH Keys: list, add, remove public keys | `D/MainWindowSettings.cpp` (`buildSettingsSshKeysTab`); server `app/src/ssh_keys.py` | **Missing** | Yes | API | S | WS18 |
| 13.6 | Pings (and email notifications) | `D/MainWindowSettings.cpp` (`buildSettingsPingsTab`, `buildSettingsEmailNotifyParts`) | **WS2** | Yes | API | — | WS2 |
| 13.7 | Agents: providers, backend, usage, behaviour, which models the composer shows, recycle bin | `D/MainWindowSettings.cpp` (`buildSettingsAgent*Parts`, `refreshComposerModelVisibilityList`) | **Partial**: provider keys in `AgentSetupSheet.swift`; the model catalog is WS3 | Adapted: the phone's own agent settings, plus the desktop's over DC | Device + DC | S | WS18 |
| 13.8 | Voice: install whisper.cpp or Parakeet, mic test | `D/MainWindowSettings.cpp` (`buildSettingsVoiceTab`, `installWhisperCpp`, `toggleMicTest`) | **Missing** | Adapted: the dictation language (on-device Speech) | Device | S | WS18 |
| 13.9 | Secrets: import tokens; Actions variables and secrets | `D/MainWindowSettings.cpp` (`buildSettingsImportAndSecretsParts`), `D/MainWindowActions.cpp` | **Partial**: import tokens | Yes: Actions secrets are write-only | API | S | WS21 |
| 13.10 | Organizations | `D/MainWindowOrganizations.cpp` | **Done** (#35) | Yes | API | — | — |
| 13.11 | MCP connector (`connector.json`, remote MCP) | `D/McpConnector.h`, `D/MainWindowMcp.cpp`; server `app/src/remote_mcp.py` | **Missing** | Adapted: show the remote MCP URL, and mint or revoke its token | API | S | WS18 |
| 13.12 | Phone: pairing code and approved phones | `D/MainWindowSettings.cpp` (`buildLocalControlTab`) | **Done** from the phone's side (#59) | Yes: a Desktops list (rename, forget) | DC | S | WS18 |
| 13.13 | Security (report a vulnerability) | see 6.9 | **Missing** | Yes | API | — | WS18 |
| 13.14 | Data: data folders, export and import the configuration, restore, hourly backups, back up now, prune, delete one folder or all data, relaunch | `D/MainWindowData.cpp`, `D/LocalBackupStore.h` | **Missing** | Adapted: export account data, clear caches, erase a project (WS1); the desktop's backup status over DC | Device + DC | S | WS18 |
| 13.15 | One scrolling settings page with a section list | `D/SettingsSectionsView.h` | **Partial** | Yes: a searchable Settings | Device | S | WS18 |
| 13.16 | Installed apps: scopes, last used, revoke; organization admins approve or deny install requests; the App Directory and app consent open in the browser | None yet (web: `/dashboard/apps`, `app/public/dashboard/js/09-apps.js`) | **WS22** | Yes | API | — | WS22 |

## 14. Accounts, identity, keys and devices

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 14.1 | Switch account, recent accounts, log out, uninstall | `D/MainWindowAccounts.cpp`, `D/AccountSessionCache.h`, `D/AccountCapability.h` | **Partial**: one account per project (WS1) | Yes: several accounts per project | API | S | WS18 |
| 14.2 | Identity key and the "back up your key" nag | `D/ForkMeshIdentity.h`, `D/MainWindowSettings.cpp` (`backUpIdentityKey`, `refreshIdentityBackupNag`) | **Partial**: `PortableIdentity.swift` has no UI (#5) | Yes: a recovery kit to Files, or a printed code | Device | S | WS18 |
| 14.3 | Claude Code device setup | `D/MainWindowSettings.cpp` (`showClaudeCodeDeviceSetup`) | N/A | Desktop-only: configures the local CLI | — | — | — |
| 14.4 | Owner-device enrolment (sealed transcripts) | `D/MainWindowAgentSync.cpp` | **Done** (#4) | Yes | API | — | — |
| 14.5 | Trust lists | `D/TrustLists.h` | **Done** in the core (#74); no UI | Adapted: a read-only view of the lists in force | API | S | WS18 |
| 14.6 | Link a node to a user with a code or QR; poll for the grant | `D/MainWindowNodes.cpp` (`promptLinkNodeToUser`, `pollLinkNodeGrant`) | **Missing** | Yes: scan the desktop's QR code | API | S | WS18 |
| 14.7 | Solana balance of a node | `D/MainWindowNodes.cpp` (`querySolanaBalance`) | **Missing** | Yes (read) | API | S | WS19 |

## 15. Projects and multi-instance

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 15.1 | Projects registry (each project a folder with its own data, config, logs, ssh) | `D/ProjectRegistry.h`, `D/AppPaths.h` | **WS1** | Adapted: a project is an instance scope; ForkMesh Next is the default | Device | — | WS1 |
| 15.2 | Relay scope of each repository record | `D/RelayRepositoryScope.h` | **WS1** | Yes | Device | — | WS1 |

## 16. Coves (encrypted file vaults) and secrets

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 16.1 | Coves: create, unlock with a password, invite a user, explorer tree, ten quick-create document types, editor | `D/MainWindowCoves.cpp`, `D/CoveStore.h`, `D/CoveCrypto.h`, `D/CoveFileEditor.h`, `D/CoveFileSupport.h` | **Missing** | Yes | API + Device (crypto on the phone) | M | WS21 |
| 16.2 | Repository settings > Coves | `D/MainWindowRepoSettings.cpp` | **Missing** | Yes | API | S | WS21 |

## 17. Releases of the desktop itself, updates and backups

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 17.1 | Auto-update | `D/MainWindowSettings.cpp` (`maybeAutoUpdate`) | N/A | Desktop-only: the App Store updates the app; 1.28 covers retired builds | — | — | — |
| 17.2 | Hourly local backups | `D/LocalBackupStore.h` | N/A | Desktop-only: the phone relies on the account sync and the device backup | — | — | — |

## 18. Background capabilities

| # | Capability | Desktop source | Native iOS | Phone | Transport | Effort | WS |
|---|---|---|---|---|---|---|---|
| 18.1 | Mirror sweep, source convergence, peer mirror updates, encrypted replica sync | `D/MainWindowRepoSync.cpp` | N/A | Desktop-only: a long-running daemon, which iOS forbids | — | — | — |
| 18.2 | Claiming agent jobs from the account queue (the desktop as a runner) | `D/MainWindowAgentCloud.cpp`, `D/AgentRunner.h` | N/A | Desktop-only: runs CLIs for minutes to hours | — | — | — |
| 18.3 | Branch operations requested on the website | `D/WebBranchOps.h`, `D/MainWindowBranches.cpp` (`applyWebBranchOps`) | N/A | Desktop-only: executed in the desktop's clones; the phone makes the same request through the API (4.18) | — | — | — |
| 18.4 | Discussion inbox drain with backoff | `D/DiscussionInboxBackoff.h`, `D/MainWindowDiscussions.cpp` (`drainDiscussionsInboxFor`) | **Done** by the signed inbox (#32) | Yes | API | — | — |
| 18.5 | Ping sync queue for offline pings | `D/PingSyncState.h` | **Partial** | Yes | API | S | WS9 |

---

## 19. New workstreams (WS8–WS21)

They follow PARITY.md §3's conventions:

- **One owner per file.** Two workstreams never edit the same file at the same
  time. Ownership of an existing file moves only when the previous owner has
  merged. Each workstream below says which files it takes over, and after whom.
- **Installer hooks.** Each workstream registers into `AppServices` through
  its own `NI/ForkMesh/App/AppServices+<Area>.swift` installer file. It never edits
  `AppServices.swift`.
- **A contract fixed on day 1.** WS8's route table (`Route`) is published by
  Gate 2, so screens can register destinations before the new shell exists.
- **Linux-testable core.** Logic lives in `NIC/` and is covered by XCTests
  that run on Linux. Views live in `NI/`.

### Gate 2: extension points for WS8+ (serial, before WS8+ start)

Gate 2 is one short branch, like the first gate. It runs after WS6 (which owns
the project generator) and WS7 (which owns `D/LocalControl.*`) have merged, or
it is agreed with their owners.

1. **`AppServices` hooks.** Add `installNavigation()`, `installInbox()`,
   `installAgentOps()`, `installReview()`, `installSurfaces()`, `installChat()`,
   `installCI()`, `installPlanning()`, `installDesktopRemote()`,
   `installTeammates()`, `installAccount()`, `installPackages()`,
   `installInsights()` and `installCoves()`. Each gets an empty installer file,
   `NI/ForkMesh/App/AppServices+<Area>.swift`.
2. **Route contract.** Add `NIC/FMApi/Navigation/Route.swift` (every
   destination in NAVIGATION.md's sitemap as a `Codable, Hashable` case) and
   `RouteRegistry.swift`. A feature registers
   `registry.register(.calendar) { CalendarScreen() }` from its installer. An
   unregistered route shows a "Coming soon" placeholder, so the sitemap is
   navigable from day 1. WS8 owns both files after Gate 2.
3. **Desktop control families.** Add a handler registry to the desktop's
   local control:
   `LocalControl::registerFamily("teammates.", handler)`, with the method
   prefix routed to a handler object. Add empty
   `D/LocalControl<Family>.{h,cpp}` files for each family below, listed once in
   `desktop/CMakeLists.txt`. On the phone, add a public
   `LocalControlService.call(method:params:)` together with an event stream for
   `*.changed` bells. Families and their owners:
   - `agentops.` for WS10;
   - `mergequeue.` for WS11;
   - `fleet.`, `files.browse.` and `diag.` for WS16;
   - `teammates.` and `insights.` for WS17;
   - `discord.` for WS13;
   - `wallet.` for WS19.
4. **Extension targets.** Add `ForkMeshWidgets` (WidgetKit and Live
   Activities), `ForkMeshShare` (share extension) and `ForkMeshAutoFill`
   (credential provider) as empty targets in `native/ios/tools/generate_project.rb`.
   Add their bundle ids to `asc_bundle_ids.py`, with the App Group
   `group.com.forkmesh.nativeapp` and the codemagic embed and signing steps.
5. **Associated domains.** Add
   `applinks:app.next.forkmesh.com` and `applinks:next.forkmesh.com` to
   `NI/Config/ForkMesh-{Debug,Release}.entitlements`. Add a server route
   that serves `/.well-known/apple-app-site-association` (new
   `app/src/app_links.py` in the Next repository, with a test).

Effort: M.

**As built** (production branch `agent/native-gate2`, off `agent/native-appstore`,
with WS7, WS1, the agent-attachments work and the LocalEngine interop fix merged
in; the phone code builds from the production repository for now):

- Hooks: the fourteen above plus `installHosts()` (WS7's installer, which had
  no hook). `installNavigation()` runs last, so the shell sees every route the
  other installers registered. `AppServices.routes` is the
  `RouteRegistry<AnyView>`; every tab stack has one
  `navigationDestination(for: Route.self)`, and `ShellPage.route(Route)` opens
  any route from today's shell. WS7's `HostsDestination` and
  `AllAgentsDestination` are registered as `.hosts` and `.allAgents` and listed
  in the tools menu until WS8's You hub replaces it.
- `RouteRegistry` is generic (`RouteRegistry<Destination>`) so it tests on
  Linux; `Route.Kind` names each case, its owning workstream and its home tab.
  `RouteParser` is WS8's.
- Desktop: `LocalControlServer::registerFamily(prefix, handler)` routes by the
  longest registered prefix, before `onRequest` and before the built-in
  `files.*` transfer (so `files.browse.` is a family).
  `MainWindowLocalControlFamilies.cpp` registers the nine families, owner-only
  with the same check as `hosts.*`; each answers in its own
  `MainWindowLocalControl<Family>.cpp` (today: `unsupported`) with a pure half
  in `LocalControl<Family>.{h,cpp}`. Phone: `FMControl/Families/DesktopControlFamily`
  over `DesktopControlHub` (LAN or relay), with the family's bells only.
- Core modules for the new areas are registered in `Package.swift` (the
  coordinator's file): `FMSurfaces`, `FMDiscord` (WS13's `discord.` client:
  `FMChat` cannot depend on `FMControl`), `FMPlanning`, `FMDesktopRemote`,
  `FMTeammates`, `FMPackages`, `FMInsights`, `FMCoves`. `FMAgents` and `FMCode`
  may now use `FMControl`.
- Extensions: `ForkMeshWidgets` ships in every build since 2026-10-09,
  sharing its snapshot through the keychain group
  ([IOS_RELEASE.md](IOS_RELEASE.md), "Widgets, Live Activities and the
  watch"); `ForkMeshShare` ships in every build since
  2026-10-01 and hands off through the keychain group instead
  ([CAPTURE_MARKUP.md](CAPTURE_MARKUP.md)), and `ForkMeshAutoFill` since
  2026-10-02, keeping its sealed cache in that keychain group too
  ([IOS_RELEASE.md](IOS_RELEASE.md), "AutoFill"). The App Group, no longer used by any target, was
  `group.com.forkmesh.forkmesh`, not `group.com.forkmesh.nativeapp`, because
  the app now ships as `com.forkmesh.forkmesh` (the watch keeps
  `group.com.forkmesh.forkmesh.watch`, now replaced by the watch's keychain group too).
- Associated domains cover `app.forkmesh.com` and `forkmesh.com` as well, for
  after production moves onto the Next stack. The AASA route is in Next's
  `app/src/app_links.py`, served on every host the app serves.

### Workstreams, ordered by user value

| WS | Name | Covers (rows) | Owns (exclusively) | Depends on | Effort |
|---|---|---|---|---|---|
| **WS8** | **Navigation shell and information architecture** | 1.1, 1.2, 1.5–1.7, 1.9, 1.10, 1.14, 1.15, 1.28; all of NAVIGATION.md | `NI/ForkMesh/Shell/HomeShell.swift`, `ShellBridges.swift`; new `NI/ForkMesh/Navigation/**` (tab root, iPad split root, You hub, route views, command palette); new `NI/ForkMesh/Search/**`; `NIC/FMApi/Navigation/**` (`Route`, `RouteRegistry`, `RouteParser` for `forkmesh://` and https, restoration) and its tests; `NIC/FMApi/Search/**`; `NIC/FMApi/ViewModels/HomeShellModel.swift`, `HomeShellLogic.swift`; `NI/ForkMesh/Theme/FmHaptics.swift`; `NI/ForkMesh/App/AppServices+Navigation.swift`; server `app/src/app_links.py` | Gate 2. It places WS3's prompt button beside the tabs (WS3 owns `ShellPromptBar.swift`) and WS1's `ProjectSwitcher.swift` in the hub (WS1 owns it) | L |
| **WS9** | **Inbox and notification actions** | 9.2, 12.13, 18.5, and the Inbox tab (pings, review requests, assigned issues, mentions, approvals) | new `NI/ForkMesh/Features/Inbox/**`; new `NIC/FMApi/Inbox/**` (feed merge, read state, tests); new `NI/ForkMesh/Platform/NotificationActions.swift` (categories: Reply, Approve, Deny, Merge, Open, Continue); `NI/ForkMesh/App/AppServices+Inbox.swift` | WS2 (`NotificationsScreen.swift`, the push delegate forwards unknown actions to WS9's router), WS8 | M |
| **WS10** | **Agents over desktop control: approvals, merge & clean up, bulk actions, usage** | 2.1 (desktop half), 2.2, 2.3 (desktop half), 2.6–2.8, 2.10–2.14, 2.20, 2.21, 2.29, 2.31, 2.36, 4.20, 8.2 | `D/LocalControlAgentOps.{h,cpp}` and its test; new `NIC/FMAgents/Remote/**`; new `NI/ForkMesh/Features/Agents/Remote/**`; after WS4 merges, `NI/ForkMesh/Features/Agents/AgentsScreen.swift` and `AgentSessionScreens.swift`; the `agent-approval` ping kind in `app/src/ping_settings.py`; `NI/ForkMesh/App/AppServices+AgentOps.swift` | WS7 (DC away from home), WS4 (file handover), Gate 2 | L |
| **WS11** | **Code review and collaboration** | 1.8, 4.1, 4.2, 4.7–4.9, 4.12, 4.13, 4.15, 4.16, 4.18, 4.21, 4.22, 4.25–4.28, 4.34, 5.1–5.5, 5.7–5.19, 5.22 | `NI/ForkMesh/Features/Code/**` except `RepoActivityTabs.swift` (`RepoDetailScreen`, `RepoCodeTab`, `RepoFileScreen`, `RepoCommitsTab`, `RepoInfoTabs`, `CodeEditorView`, `ReposScreen`, `Collab/**`) plus new `Review/**`, `Branches/**`, `Board/**`, `CodeMap/**`, `RepoSettings/**`; `NIC/FMCode/Collab/**`, `NIC/FMDiff/**` (WS5 keeps `FMCode/Local` and `FMGit`); `D/LocalControlMergeQueue.{h,cpp}`; `NI/ForkMesh/App/AppServices+Review.swift` | WS8 (routes); the WS2 handover of `RepoDetailTab` is not needed because sections are routes | L |
| **WS12** | **System surfaces: widgets, Live Activities, App Intents, Siri, Spotlight, share extension** | 2.35, 11.15; NAVIGATION.md §6 | new `NI/ForkMeshWidgets/**` (agent and timer Live Activities, Home and Lock Screen widgets, Control Center controls); new `NI/ForkMeshShare/**`; new `NI/ForkMesh/Intents/**` (App Intents, App Shortcuts, `AppEntity` for repo, issue, agent and note, Spotlight indexing); new `NIC/FMSurfaces/**` (snapshot models, tests); server `app/src/apns_live_activity.py` (the `liveactivity` push type), and the `agents` push topic added after WS2 merges | Gate 2 (targets), WS2 (APNs sender), WS8 (routes for deep links) | L |
| **WS13** | **Chat depth** | 9.4–9.9, 9.11–9.15, 9.17–9.19 | after WS3 merges: `NI/ForkMesh/Features/Chat/ChatScreen.swift`, `ComposeIdentityBar.swift`, `NIC/FMChat/ViewModels/ChatHub.swift`; new `NI/ForkMesh/Features/Chat/Threads/**`, `Channels/**`, `Office/**`; new `NIC/FMChat/Threads/**`, `NIC/FMChat/Office/**`; `D/LocalControlDiscord.{h,cpp}`; `NI/ForkMesh/App/AppServices+Chat.swift` | WS3 (file handover), Gate 2 | M |
| **WS14** | **CI, releases and security** | 5.20, 6.1–6.4, 6.8, 6.10–6.13, 6.15, 6.17 | `NI/ForkMesh/Features/Code/RepoActivityTabs.swift`; new `NI/ForkMesh/Features/Code/CI/**`, `Releases/**`, `Security/**`, `Webhooks/**`; new `NIC/FMCode/CI/**`; `NIC/FMApi/Models/Actions.swift`, `Releases.swift`; `NI/ForkMesh/App/AppServices+CI.swift` | WS8 (routes) | M |
| **WS15** | **Planning: tasks, calendar, notes depth, timers, projects and milestones** | 2.30, 5.23–5.25, 10.2–10.4 | `NI/ForkMesh/Features/Tools/NotesScreen.swift`, `TasksScreen.swift`; new `NI/ForkMesh/Features/Planning/**` (Calendar, Timers, Projects, Milestones); `NIC/FMTools/NotesModels.swift`, `TasksModels.swift`; new `NIC/FMPlanning/**`; `NI/ForkMesh/App/AppServices+Planning.swift` | WS8; WS12 renders the timer Live Activity from WS15's model | M |
| **WS16** | **Desktop and fleet remote** | 2.34, 4.11, 4.17 (desktop half), 4.24, 6.16, 7.3–7.11, 7.17, 8.4, 12.4, 12.9, 12.12 | `D/LocalControlFleet.{h,cpp}`, `D/LocalControlFiles.{h,cpp}`, `D/LocalControlDiagnostics.{h,cpp}` and their tests; new `NI/ForkMesh/Features/Desktop/**` and `NI/ForkMesh/Features/Hosts/Operations/**` (WS7 keeps whatever it creates under `Features/Hosts/`); after WS4 merges, `NI/ForkMesh/Features/Chat/LocalDesktopScreen.swift`; new `NIC/FMDesktopRemote/**`; `NI/ForkMesh/App/AppServices+DesktopRemote.swift` | WS7, WS4 (file handover), Gate 2 | L |
| **WS17** | **Teammates (agent insights dropped from the phone apps on 2026-09-29)** | 2.16, 2.19, 2.26–2.28, 3.1–3.6, 9.16 | `D/LocalControlTeammates.{h,cpp}`, `D/LocalControlInsights.{h,cpp}`; new `NI/ForkMesh/Features/Teammates/**`, `NI/ForkMesh/Features/AgentInsights/**`; new `NIC/FMTeammates/**`; new `NIC/FMAgents/Announcer/**` (a port of the phrase logic in `AgentAnnouncer`) and `NI/ForkMesh/Platform/AnnouncerSpeech.swift`; `NI/ForkMesh/App/AppServices+Teammates.swift` | WS7, Gate 2 | M |
| **WS18** | **Settings, account, identity and devices** | 1.13, 1.16, 1.23, 6.9, 7.12, 7.15, 8.5, 8.12, 13.2, 13.4, 13.5, 13.7, 13.8, 13.11–13.15, 14.1, 14.2, 14.5, 14.6 | `NI/ForkMesh/Features/Account/SettingsScreen.swift`, `SettingsCards.swift`, `OrgsScreen.swift`, `OrgDetailScreen.swift`; `NIC/FMApi/ViewModels/SettingsLogic.swift`, `SettingsModels.swift`, `AccountCardModels.swift`; new `NI/ForkMesh/Features/Account/{SshKeys,Mcp,Identity,Desktops,Data}/**`; new `NIC/FMAccount/Identity/**`; after WS1 merges, `NIC/FMAccount/SettingsService.swift`; `NI/ForkMesh/App/AppServices+Account.swift` | WS1 (file handover), WS2 (pings settings stay in WS2's screen) | M |
| **WS19** | **Business packages and the Market** | 5.21, 5.27, 11.1–11.10, 11.22, 14.7 | new `NI/ForkMesh/Features/Packages/**` (Market, CRM, Marketing, Analytics, Competitor, Ads, Ledger, Tips); new `NIC/FMPackages/**`; `D/LocalControlWallet.{h,cpp}` (tip and bounty signing requests); `NI/ForkMesh/App/AppServices+Packages.swift` | WS8, Gate 2 | L |
| **WS20** | **Stats, health, logs and the admin console** | 1.18, 4.3, 4.29–4.31, 5.6, 7.20, 8.1, 8.3, 8.9, 11.16, 12.1, 12.5–12.8, 12.10 | `NI/ForkMesh/Features/Tools/ActivityScreen.swift`, `NIC/FMTools/ActivityModels.swift`; new `NI/ForkMesh/Features/Insights/**`, `NI/ForkMesh/Features/Admin/**`; new `NIC/FMInsights/**`; new `NI/ForkMesh/Platform/MetricKitSink.swift`; `NI/ForkMesh/App/AppServices+Insights.swift` | WS8; WS16's `diag.` family for the desktop-computed parts | M |
| **WS21** | **Coves, secrets and AutoFill** | 11.12, 13.9, 16.1, 16.2 | new `NIC/FMCoves/**` (a port of `CoveCrypto` and `CoveStore`, tested with vectors from `desktop/tests`); new `NI/ForkMesh/Features/Coves/**`, `NI/ForkMesh/Features/Secrets/**`; `NIC/FMVault/**`, `NI/ForkMesh/Features/Tools/VaultScreen.swift`, `VaultSubscreens.swift`; new `NI/ForkMeshAutoFill/**`; `NI/ForkMesh/App/AppServices+Coves.swift` | Gate 2 (the AutoFill target) | M |

Order and parallelism: Gate 2 comes first. **WS8** starts at once, because
everything else lands in its information architecture. After that:

- **WS9, WS11, WS14, WS15, WS19, WS20 and WS21** need only the API. They can
  run in parallel as soon as the route contract exists.
- **WS10, WS16 and WS17** need WS7's control relay. WS10 and WS16 also need
  WS4's handover.
- **WS12** follows WS2's server change.
- **WS13** follows WS3.
- **WS18** follows WS1.

---

## How the counts are made

Every capability row starts with a section number such as 4.21. The fifth column
is the Phone verdict (`Yes`, `Adapted...` or `Desktop-only...`), and the
fourth is the native state:

```sh
awk -F'|' '$2 ~ /^ [0-9]+\.[0-9]+ $/ {n++; p=$6; s=$5; t=$7;
  if (p ~ /^ *Yes/) y++; else if (p ~ /^ *Adapted/) a++; else if (p ~ /^ *Desktop-only/) d++;
  if (t ~ /DC/) dc++;
  if (s ~ /^ *N\/A/) na++; else if (s ~ /^ *\*\*WS[0-9]/) w++; else if (s ~ /^ *\*\*Done/) dn++;
  else if (s ~ /^ *\*\*Partial/) pa++; else if (s ~ /^ *\*\*Missing/) mi++}
  END {print n, "rows:", y, "yes,", a, "adapted,", d, "desktop-only;", dn, "done,", pa,
       "partial,", mi, "missing,", w, "in flight,", na, "n/a;", dc, "need DC"}' docs/native/DESKTOP_PARITY.md
```

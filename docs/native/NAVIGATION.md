# Native app navigation and information architecture

How the native apps are organised: tabs, the shell, routes, the You hub,
search, system surfaces, the iPad layout, the sitemap and URLs, and the rules
every new screen follows. Targets are iOS 26 and iPadOS 26 (Liquid Glass) and
Android; iPhone-specific shell details are marked. Row numbers such as **2.13**
refer to [DESKTOP_PARITY.md](DESKTOP_PARITY.md); `#54` refers to
[PARITY.md](PARITY.md).

## 1. Principles

1. **The phone's job is to answer, steer and review.** People check on agents,
   answer what needs them, review code and talk; long work runs on the desktop.
2. **One tap to prompt.** The new agent button is always on screen and opens
   the composer aimed at whatever is on screen.
3. **Everything else is one level away**: Home, the You hub, search, the
   command palette, Siri or Spotlight. Nothing lives only behind a gesture.
4. **Every screen is a route.** Each screen has a typed `Route` value, a URL
   and a restorable place in a stack. Deep links, notifications, widgets,
   intents and state restoration all go through that one type.
5. **System components first** (`TabView`, `NavigationStack`,
   `NavigationSplitView`, `.searchable`, `ContentUnavailableView`, system sheets
   and toolbars). Custom chrome must match them item for item (§10).
6. **No toasts.** Feedback is an inline line where the action happened (§9).

## 2. The primary destinations: tabs and the prompt button

| Tab | Root screen | Badge | Why it is primary |
|---|---|---|---|
| **Agents** (default) | Every agent: desktop sessions, cloud and runner runs, outside CLIs ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)) | Agents count; running count with a heartbeat | The main reason to open the app |
| **Inbox** | Pings, review requests, assigned issues and tasks, mentions, approvals | Unread count | One place for "what needs me" |
| **Code** (labelled **Repos**) | Repositories: the desktop's, yours, organizations, catalog, on-device | None | Review, browse and merge |
| **Chat** | Channels, DMs, `#mesh`, office rooms | Unread mentions and DMs | Conversation, one tap away |

`AppTab.code` is labelled **Repos** (`books.vertical`) and always shows the
list. A separate **Code** button (`chevron.left.forwardslash.chevron.right`)
returns to the repository in use, remembered across launches
(`CurrentRepoMemory`, key `code/current-repo`); with none yet it shows the list
and says so inline. Android follows `CodeButton.plan`.

The **new agent button** opens the composer (tap) or the command palette (long
press). On iPhone it is a 44 pt circle left of the avatar in the top bar, the
chat room header and the DM header (`ShellNewAgentButton`), showing one
four-point star per running agent (`ShellCounts.shared.agents`, at most 24)
drawn by `ShellSparkleField`, which tilts with the accelerometer
(`ShellSparkleTilt`, 30 Hz while visible). Reduce Motion stops the motion;
Reduce Transparency keeps only the stars on a flat circle. The iPad keeps it in
`floatingButtons`.

Not tabs: **Search** (reached from tab roots, Home, the palette and the iPad
sidebar), the **You hub** (the avatar, §4) and a More tab (Home replaces it).

### 2.0 The iPhone shell

`HomeShell` holds the tab stacks, the top chrome, the bottom bar and shell
pages that load in place.

**Bottom bar** (`ShellBottomBar`). One Liquid Glass bar at the screen's
bottom edge holding up to `ShellBarStyle.limit` slots (3 to 8, default 6; a
folder is one slot); Home is always one of them (`ShellBarPrefs.keep`).

- Every pick runs through `ShellBottomBar.run`, which records the use and acts
  with animations disabled, so pages appear in place.
- Every bottom-bar button lands on its first page: `HomeShell.landOnTop` gives
  the tab's `NavigationStack` a new identity (`stackResets`) when untracked
  pages cover it (`TabStackTops` counts tracked pages). A hub page already on
  the stack pops back to itself (`AppRouter.openFromHub`).
- Back returns to Home when a pick from Home was popped (`HomeShell.leavePage`,
  `PageReturn`).
- **Item menu** (long press, `ShellBarItemMenu`): package settings and removal
  (the package's Market page), move, move to folder, add to or remove from
  the bar (swap when full), hide, reset. Dragging places icons in the bar,
  reorders them, or takes them out onto Home. **Folders** show a 2x2 preview and
  open a popover.
- **Sort** (`ShellBarSort`, `shell/bar-sort`): usual order, recently used, most
  used. Uses are recorded in `shell/bar-recent` (48 ids) and `shell/bar-uses`
  (`count:id` lines); visits from other routes count too (`ShellBarPrefs.visit`).
- **Layout** (`ShellBarLayout` in FMApi, `shell/bar-layout`): hidden ids,
  folders, order and the ids allowed in the bar. Shared by every project.
- **Style** (`ShellBarStyle`, `shell/bar-style`, edited in
  `ShellBarSettingsSheet`): shape (capsule, panel, squares, circles, icons
  only), background (glass, clear, tinted, solid), corners, spacing, width,
  icon style and size, labels, selected-icon mark, slot count, counts,
  activity and frequent-use glow. The default writes no key.

**Home** (`ShellPage.home`, `ShellHome`) is the icon grid of every
destination, in sections, with counts and activity:

- Sections: Go to, the current repository's sections (Pull requests, Issues,
  Commits, Branches, Releases, Actions, Discussions), and the hub groups (§4).
  Order is draggable and saved in `ShellMenuLayout` (`shell/menu-layout`).
- Icons (`ShellHomeIcon`): style Outline (default), Filled, Accent or Tiles; 4,
  5 (default) or 6 per row; sorting (custom, name, most or least used, recent,
  needs attention, biggest count); categories can be hidden. Settings live in
  `ShellHomeSettingsSheet` and Settings > General > Home screen.
- Counts come from data already loaded or fetched when Home opens
  (`ShellNavigatorCounts`, at most once a minute; `ShellCounts`); `nil` means
  unknown and shows no pill; zero shows no pill. Count pills use `fm.count`
  (blue). Items with live work (`ShellBarItem.active`) show an activity halo
  drawn from a `TimelineView` (30 fps cap, still with Reduce Motion).
- Frequently used tiles glow (`ShellBarOrder.heat`, log scale).
- Search is revealed by pulling Home down 56 pt (`ShellBarOrder.searchPull`);
  it filters tiles by title or section and offers **Search everywhere**. With
  VoiceOver on the field always shows.
- Home is built to fit one screen when possible (`fittedTile`).

**Top chrome** (`ShellTopBar`). The project icon (`ShellProjectSlot`, 40 pt,
opens the project drawer), the telemetry strip centred (tiny health-coloured
charts of phone metrics and, when a desktop answers `diag.watch`, desktop
memory, CPU, disk and swap), the new agent button and the avatar
(`AvatarWithDot`, rounded square, status emoji badge, connection dot). The top
bar has no background: on a tab root (`HomeShell.floatsChrome`, empty path) the
list scrolls under it; pushed pages start below it so their back button is
never covered. The offline strip appears only after the relay has been down
`HomeShell.offlineGrace` (4 s).

**Bare top.** Pages that own the top of the screen (agent run page, diff
pages, Watch) use `bareShellTop()`; `ShellChrome` counts registrations per tab
and hides the top bar while any is on top. `ShellRootBarHider` hides the
system navigation bar on tab roots and corrects stale top safe-area insets; on
a pop back to the root it fades the top bar in during the transition
(`ShellChrome.returnToRoot(on:)`).

**Bottom clearance.** The bar floats over pages. `HomeShell.barClearance`
(bar height plus `barMargin`, 32 pt; zero with the keyboard up or on iPad)
travels in each tab's `ShellDock` and is applied by `shellPageClearance()` as
bottom safe area to every tab root and route page. Pages pushed by a
`NavigationLink` closure or their own `navigationDestination` must add
`.shellPageClearance()` themselves. Lists scroll under the glass; pinned bottom
controls sit above it.

**Docked controls.** A page's bottom composer (agent run page, on-device agent,
chat room, threads, DMs) uses `.shellDock { … }`: it sits `ShellDock.gap` above
the capsule, measured inside `.ignoresSafeArea` against `ShellDock.barBottom`.
With the keyboard up or on iPad it sits at the page's bottom.

**Tab mounting.** The selected tab is built first; Agents is mounted 2 s after
launch (it feeds the counts); Chat and Code on first visit; mounted tabs stay
hidden (`HomeShell.mountedTabs`) and pause frame-rate animations
(`shellTabHidden`).

**Launch.** `AppRootView` fades the app in over the launch colour, and the shell
starts on Home unless a restored route, sheet or push wins.

**Diagnostics footer.** The version (with an **Update** pill when the App Store
has a newer version, `ClientVersionGate.availableUpdate`), the desktop light
(load from `diag.health`'s `level` via `DesktopLoadLightModel`; green through
LAN or relay, a globe badge for relay), the office MacBook light
(`BuildMacLightModel`, `diag.buildMac`), core node and runner lights
(`ShellFleetLights`, from `/api/status`; runners show their agent counts), and
service status dots (`ShellStatusDots`, systems claimed by installed
`status-check-*` packages plus custom monitors). It lives in Settings > Profile
and shows while the Diagnostics switch is on. Reads happen at shell start and
app activation only, waiting 15 minutes after an all-green read and 2 minutes
otherwise; no timers. Any loaded check in trouble turns the avatar dot red.

### 2.0.1 Chat room layout (iOS)

- The room header replaces the top bar: a back circle that opens the channel
  list (dot for unread rooms elsewhere), a pill with the room name and
  `ChatHub.headerDetail` (for a channel `ChatHub.channelDetail`, "N members •
  N tabs"), the new agent button and avatar. Tapping the pill opens the
  channel details sheet (`Channels/ChatChannelSheet.swift`) or the DM's
  encryption explanation.
- The channel list is a full-width drawer from the left that hides the room
  header and shows the shell top bar; it closes with its × button, picking a
  room, or Back on Android. Drawers do not follow swipes. Opening it hides the
  keyboard.
- The Chat tab reopens the last conversation per project
  (`ChatWorkspace.lastConversation`, Android `chat/lastConversation`) unless a
  notification or link chose one.
- The composer is one glass line (`ChatComposerBar(glass: true)`) docked on the
  bar. Transcripts use `.defaultScrollAnchor(.bottom)` plus
  `chatKeepsEndInView`.
- Chat screens make no interface sounds (`.fmQuietSounds()`).
- Members of relay `#channels` are counted from presence and posters (the relay
  stores ciphertext only); office and server rooms use the server's list.
  `@here`, `@channel` and `@everyone` are offered first, highlight as mentions,
  and ask before notifying six or more people. Link previews
  (`ChatLinkPreviews`) are described in `native/ios/docs/FMChat.md`.
- Avatars paint on the first frame from `FmAvatarImages` and a persisted
  `ChatAvatarCache`.

### 2.0.2 Plugins on the phone

Every Home tile except Repos, Search, Market and Settings is a Market package,
off by default in new projects except the chosen starter pack
([core-plugins.md](../marketplace/core-plugins.md#phone-starter-packs);
Android: [ANDROID.md](ANDROID.md), section 13).

- `PackageGate` (iOS `FMApi/Navigation/PackageGate.swift`) maps Home, bar,
  repository, hub, route-kind, tab and search-bucket ids to package ids. The
  app publishes the set of packages shown here (`PackageGate.publish`) whenever
  the account answer, catalog, organization installs or device switches change.
- `AccountPackagesStore.isInstalled`/`isShownHere`: the account's row from
  `/api/plugins/@account` wins (a `legacy` entry counts only in a legacy
  project); otherwise `default`, or `installedBefore` in a legacy project;
  dependencies must be installed; `places` must include `mobile`. In projects
  created on the phone, installs belong to the project
  (`<namespace>packages/project-installs`), and the packages of the chosen
  `StarterPack` count as installed until the project removes them.
- `ProjectLegacyMarker` is written once per project from existing per-project
  data; projects created on the phone are marked new.
- **Starter pack.** A new project shows `StarterPackView` ("Choose how to
  start": Minimal, Standard, Everything; Standard preselected; Continue) once,
  from `AppHomeView` while `SettingsService.needsStarterPack` (new project, no
  `<namespace>packages/starter-pack` yet). Continue stores the pack id
  (`PhonePackages.chooseStarterPack`) and republishes the gate. Unset reads as
  `standard`; legacy projects never see it.
- Gated surfaces: Home, the bar, the iPad sidebar, the You hub, global search,
  the command palette, Genie routes, Home Screen quick actions, the new agent
  and bug buttons, the tour, status dots and the Glasses radar. Routes stay
  registered, so links and notifications still open hidden features. Business
  packages (CRM, Marketing, Analytics, Competitors) show only when one of the
  user's organizations installed them.
- **Sign in** is the one per-device package (`packages/sign-in/on`, on until
  removed, whatever the pack); Settings always keeps Sign in and Sign out.

### 2.0.2.1 First run

- **Login** has "Forgot password?", which opens the relay's
  `/forgot-password` (`AuthService.forgotPasswordUrl`) in an in-app Safari
  sheet.
- **Sign up** opens `/signup` in the same sheet and moves the flow to the
  login form with "When your email is confirmed, log in here with your new
  account." (`AuthFlowModel.signupOpened`), so closing the sheet lands on
  login instead of leaving the app.
- **FmNeedsDesktopNote** ends with "Get the desktop app", which opens the
  project homepage's `/desktop` (`LegalLinks.desktop`).
- **Placeholders** (`RoutePlaceholderView`, unbuilt repository sections) say
  the screen "is not in this version of the app yet" and never name the
  internal owner.

### 2.0.3 Other pages

- **Market** (`MarketScreen`): search in the navigation bar drawer, sort and
  filter chips as the list's top inset, flat cards with a 16:9 preview, rating,
  installs and users, and one-tap Install/Open (`MarketModel.cardAction`).
  Install state follows the account, organizations and `install.default` for
  desktop-only packages. Package pages set "Show on" places.
  An installed card also shows Uninstall, and each package page has a ⋯ menu
  (top right) with Rate and Uninstall. Both ask `MarketModel.uninstallAction`
  how this phone removes it: account packages leave every device
  (`AccountPackagesStore.uninstall`, naming the packages that go with them),
  per-phone switches turn off (`PackageGateSync.setOnDevice`), packages with a
  phone screen hide from this phone (`PhonePackages.setRemoved`), and
  organization packages with no phone screen are removed from an organization
  you manage, with the data export to save after. Nothing is removed without a
  confirmation (`PackageUninstall.swift`). Android mirrors it with the same
  rules (`MarketModel.uninstallAction`, `PackageUninstall.kt`).
- **Face Changer** (`.faceChanger`, `/dashboard/face-changer`): the Market
  package's web page in a web view on the server's own https origin, with the
  camera and microphone granted only to that origin. A recorded clip (or a
  snapshot) goes to the share sheet: the page hands the file to the app in
  384 KiB base64 chunks through the `fmShare` script handler (Android:
  `FmShareBridge`), `ShareTransferWriter` checks the type (video, image,
  audio), size (512 MiB at most) and name, and writes it to one temporary
  folder that each new share empties.
- **People** (`.people`, `/dashboard/people`): a campfire of five engagement
  rings with Follow and Message ([People](../marketplace/people.md)).
- **Docs** (`.docs`): the public documentation library bundled with the app.
- **Shortcuts** (`.shortcuts(RepoRef?)`): the desktop's shortcuts and skills
  for the repository in use (`ShortcutsStore.load(preferred:)`); prompt
  shortcuts can also run on the iPhone (`PhoneShortcuts`, `ShortcutMirror`).
- **Desktops**: paired desktops, the office MacBook (with buttons that run the
  Mac shortcuts), runners and core nodes.
- **Pocket mode** and **Suggestion mode**: see [GENIE.md](GENIE.md).

### 2.1 Behaviour of the tab row

- **Selecting and reselecting.** Tapping a tab always shows its root, popping
  whatever is pushed; tapping the selected root scrolls to the top. iOS:
  `AppRouter.select` (bar, iPad sidebar, ⌘1-4, palette); deep links use
  `AppRouter.open` and keep the stack. Android: `HomeShell.select` with
  `reselect = true` from the bar and rail.
- **Hiding.** The bar stays on pushed screens; it hides under full-screen
  covers and while the keyboard is up for a page's own field
  (`HomeShell.pageKeyboardUp`; Android uses the IME inset). Under the prompt
  composer it fades out.
- **Placement.** On iPhone the bar floats over pages (§2.0, bottom clearance);
  on Android it is the last item of the shell column and pages end at its top
  edge, consuming the navigation-bar inset once.
- **Badges.** Counts come from the account event channel; the app icon badge
  equals the Inbox badge.
- **Launch sparkle.** Starting an agent shows no notice: the Send button pops,
  a star charges on it, and on acceptance a snapshot of the composer card
  (`PromptScreenCapture.region`) shrinks and flies into the Agents cell, which
  bounces as its count rises (the shell holds count + 1 for 12 s). Without a
  snapshot the star flies alone. Reduce Motion skips the flight. The message is
  announced to screen readers. iOS `Shell/ShellLaunchSparkles.swift`
  (`ShellLaunchFlight`), Android `shell/LaunchSparkles.kt` (`LaunchFlight`).

### 2.2 The prompt button

| Gesture | Result |
|---|---|
| Tap | The composer opens at once with the keyboard up, aimed at the current context |
| Long press | The command palette (§5.2) |

Context rules, most specific first: an open agent session steers it; an open
PR or issue starts an agent on that repository with the item attached; an open
repository starts an agent there; an open chat channel posts there; otherwise
the last-used repository, with a chip to change it. The composer always shows
its target as an editable chip. The composer card is Liquid Glass with a close
button; agent composers carry a **Fix** button (`Please fix: <text>`,
`FMAgents/PromptFix.swift`, `ComposerRules.fixPrompt`), a mode chip and a model
chip; the shell composer shows **YOLO** and **No worktree** toggles when the run
goes to the desktop ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)).

## 3. Navigation inside a tab

### 3.1 One `NavigationStack` per tab, with typed routes

- Each tab owns a `NavigationStack(path: $router.paths[tab])` of `Route`
  values. `Route` is one `Codable, Hashable` enum listing every destination in
  §8.
- Features register destination views in `RouteRegistry` from their
  installers. A route nobody has registered yet opens a placeholder, so the
  whole sitemap is navigable.
- Screens push values (`NavigationLink(value: Route.pull(repo, 12))`), never
  views. There is one `navigationDestination(for: Route.self)` per stack.
- Agent rows use one `navigationDestination(item:)` on the screen
  (`AgentsScreen.opened`), so list rebuilds never close an open page.

### 3.2 Titles and toolbars

- Tab roots use their own compact headers; pushed details use inline titles
  with a subtitle where it helps.
- At most three toolbar items per side; more go in a trailing menu or a
  horizontal scroller (`AgentRunActionsCap`). Destructive actions sit in menus
  or confirmation dialogs, or act at once with Undo.
- Liquid Glass belongs to bars and floating controls, never behind content.

### 3.3 Back navigation

- Never hide or replace the system Back button on pushed pages; it provides the
  edge swipe, the long-press history menu and VoiceOver's scrub.
- Content that scrolls sideways scrolls inside its own `ScrollView`; nothing
  attaches a full-width horizontal drag to a pushed screen. Horizontal row
  gestures claim only clearly sideways drags.
- Sheets with unsaved input set `interactiveDismissDisabled(hasChanges)` and
  offer Save or Discard.
- There is no Forward button.

### 3.4 Repository screens

One `RepoDetailScreen` serves every entry point (`Route.repo(r, nil)`, the
catalog, the desktop's repositories, deep links):

1. a header (icon, `owner/name` with online dot, description, default branch,
   language, stars, forks, mirrors, visibility);
2. an action row: **Agent**, **Issue**, **Files**, Clone to device; the toolbar
   menu keeps Fork, Download ZIP, Code map, Issue board, Branches and Settings;
3. a grid of sections with count badges (Code, Pull requests, Issues, Commits,
   Branches, Agents, Worktrees, Actions, Releases, Discussions, Mirrors,
   Projects, Security, Insights, Artifacts, About, Settings for admins). Counts
   come from `RepoOverviewModel` (FMCode): issues and pulls from the published
   ledgers, commits, branches and releases from served reads, mirrors from the
   catalog; a failed read leaves no count;
4. a README row.

Each tile pushes `RepoSectionScreen`. Android matches it (`RepoDetailScreen`,
`RepoOverview.kt`).

- **Desktop repositories.** The list shows the paired desktop's repositories
  above the phone's own and the catalog, from `repos.list`
  (`DesktopReposModel`), reloaded on `repos.changed`; an older desktop falls
  back to its `agents.list` repositories.
- **Actions.** `RepoActionsModel` asks the desktop's `repos.actions` first and
  maps its reply onto the server's v2 summary ("Live from <desktop>"). Run logs
  follow `repos.actions.log` with `since` (64K cap). Controls go through
  `repos.prepare` then `repos.run` (`run.stop`, `workflow.run`,
  `workflow.enable`, `workflow.disable`).

### 3.5 State restoration

- The selected tab and each tab's path are stored with `@SceneStorage` as
  `Route` values, restored within 8 hours; a saved path that no longer decodes
  starts fresh.
- A route whose object disappeared falls back to its parent with an inline
  line; never an error page.
- Composer drafts are restored. Paths are stored per project.

## 4. The You hub

The avatar drops a panel down from the top bar (`hubDropdown`, `YouHubSheet`,
`ShellHubUnroll`; at most 380 x 640 pt over a scrim). Choosing an item closes
it and pushes the destination onto the current tab. On iPad the same items are
sidebar sections (§7).

| Group | Items |
|---|---|
| Header | Avatar, name, status; project switcher; account switcher; Pocket mode; Suggestion mode |
| Workspace | Notes, Tasks, Calendar, Timers |
| Operate | Desktops, Special agents (teammates), Shortcuts, Hosts & fleet, Network & nodes |
| Business | Market, CRM, Marketing, Analytics, Competitors, Ad review (reviewers only), Ledger |
| Personal | Vault, Coves, Calories, Steps, Water, Sleep, Sugar Break, Stretch Break, Glasses Alert, Food Guide, On-device AI, Activity |
| Insights | Stats, Status, Admin (admins only) |
| App | Settings, Organizations, People, Docs, Design, Report a bug, Tour, World, Workflows, Sign in/out |

Items follow the package gate (§2.0.2). The hub shows up to five recent
destinations. On iPhone, where Home and the bar hold the links, the hub keeps
the header, recents and an Account group (Settings, Sign in or Sign out).

- **Diagnostics switch** (top-leading, `hub-diagnostics-strip`;
  `diagnostics/showStrip`, on by default) shows or hides the telemetry strip and
  the profile footer.
- **Version row** (last row; iOS `YouHubSheet.versionRow`, Android
  `HubVersionRow`): "Version x (build)" opening **What's new** from the bundled
  changelog ([MAC_BUILD.md](MAC_BUILD.md)), and a **Send diagnostics** capsule
  that turns red ("Slow, send report") when `PerformanceAlert.evaluate` finds,
  since the last send, an OS diagnostic in 24 h, a main-thread stall of 250 ms
  or more in 10 min, or five or more dropped-frame stalls in 10 min.
- **Send diagnostics** (also the composer's "+" menu and `/diagnostics`) builds
  a report (`PerformanceReport`/`PerformanceReportBuilder` on iOS,
  `DiagnosticsBundle.report` on Android): build, device, thermal state, memory,
  metrics, slowest events, in-flight operations during stalls, top endpoints,
  OS crash reports with attributed thread frames, stall log tail and recent log
  lines. Sections are ranked and trimmed to fit the 8,000-character agent prompt
  (`PerformanceReport.maxChars` 4,000) rather than cut at the end.
  `DiagnosticsBundle.scrub` removes tokens, passwords, cookies, sessions, JWTs,
  message bodies, emails, query strings, phone numbers, long opaque strings and
  user folders. The report starts an agent on `forkmesh/forkmesh` through the
  composer's launcher. Request query strings are never logged.
- The frame monitor forgets its last timestamp across backgrounding, so time
  away is never counted as a stall.

## 5. Search, the command palette and quick actions

### 5.1 Search

- **Scoped search** on tab roots: agents, the Inbox, repositories, channels and
  messages, with "Search everywhere for '…'" first.
- **Global search** (`Route.search(query, scope)`): scopes All, Code, Issues &
  PRs, Chat, People. Buckets in order: Repositories (catalog plus on-device;
  on-device ones open `Route.localRepo(id:)`), Go to, People (chat roster and
  `GET /api/chat/direct-messages/users?query=`, real accounts only), Commits
  (subject, author or a hex prefix of 4+), Issues, Pull requests, Code, Chat.
  Issues, pulls, commits and code search at most three repositories (named
  matches first, then the composer's and recent ones). Matching falls back to
  ignoring spaces and punctuation.
- Saved searches appear above recent searches. File viewer, diff and transcript
  support `UIFindInteraction`.
- Search is debounced 250 ms; cached results first, then the server's; offline
  searches only cached content.

### 5.2 The command palette

A long press on the prompt button opens a sheet with the field focused:
suggested actions for the context (each an App Intent, §6), recent
destinations, then results (actions, destinations by title, content hits). ⌘K
opens it with a keyboard.

### 5.3 Home Screen quick actions

The app icon's long-press menu offers New agent, Inbox, Search and New note,
plus the most recent repository and running agent, filtered by the package
gate (`ShellQuickAction.availableActions`).

### 5.4 Keyboard

| Keys | Action |
|---|---|
| ⌘1 to ⌘4 | Switch tab |
| ⌘K | Command palette |
| ⌘F | Find in screen |
| ⌘N | New agent |
| ⌘⇧N | New issue |
| ⌘R | Refresh |
| ⌘[ | Back |
| ⌘, | Settings |
| ⌘↩ | Send (composer) |
| Esc | Dismiss |

Every command uses `.keyboardShortcut`, so it shows in the hold-⌘ overlay.

## 6. System surfaces

### 6.1 App Intents, Siri and Shortcuts

User guide: [`www/docs/mobile/siri.md`](../../www/docs/mobile/siri.md); update it
when a phrase or answer changes.

- **Entities**: repositories, agent sessions, issues, pulls, notes, projects,
  with cache-backed `EntityQuery`s, indexed for Spotlight.
- **Voice commands** (`native/ios/ForkMesh/Intents/AgentVoiceIntents.swift`,
  phrases in `ForkMeshShortcuts`). A desktop agent is named by its session
  number (`AgentRunEntity` title "Agent 754"); `FMSurfaces/AgentVoice` reads
  spoken numbers and matches titles. `updateAppShortcutParameters()` runs
  whenever the agent list changes.

  | Say | Does |
  |---|---|
  | Merge agent N in ForkMesh | Merge & clean up after reading back the desktop's summary (phone unlocked) |
  | Stop / Continue agent N in ForkMesh | Stops it, or continues a finished desktop agent |
  | Tell agent N in ForkMesh | Asks for a message and sends it |
  | How is agent N doing / What did agent N change | Status, files and lines changed |
  | What are my ForkMesh agents doing | Summary of all agents |
  | Approve / Deny ForkMesh request | Reads the oldest request, confirms, answers (phone unlocked) |
  | Start a new agent in ForkMesh | Opens the composer with the keyboard up |
  | Open inbox / approvals / agents / … in ForkMesh | Opens that screen (`OpenScreenIntent`, optional repository) |
  | Delete agent N / Delete all agents in ForkMesh | Confirms with the desktop's summary or counts, then deletes (`DeleteAgentIntent`, `AgentDeleteScope`; desktop's own sessions only for "all") |
  | Start / Stop / Toggle listening in ForkMesh | The Genie's Always listen (`ListeningIntent`), showing a hidden spider first ([GENIE.md](GENIE.md)) |

  Apple requires the app's name in every phrase and allows ten App Shortcuts
  per app; other intents (Stop all agents, Start a timer, Open agent run, Open
  inbox, Open approvals) are Shortcuts actions without a built-in phrase. Merge
  and delete spend the desktop's confirmation token; the spoken confirmation
  and `.requiresAuthentication` stand in for the in-app Face ID prompt. A cold
  launch from Siri's compose request is picked up in `HomeShell.start()`.
- The watch app has its own `NewAgentIntent` ([PARITY.md](PARITY.md), 4.3.2).
- **Focus filter**: choose which project the app shows.

### 6.2 Widgets, Live Activities and controls

- **Live Activities** per running agent (face, title, step, elapsed time,
  tokens; Stop, and Approve with authentication), timers, and walks. The app
  updates them locally; they go stale after 15 minutes. They are requested
  without a push type: the served stack has no `liveactivity` sender or token
  route.
- **Widgets**: running agents, Inbox count and top items, a pinned
  repository's open PRs, the timer; Lock Screen accessories for running agents
  and unread items. Data is shared through the keychain group
  ([IOS_RELEASE.md](IOS_RELEASE.md)).
- **Controls**: New agent and Approvals.
- A widget tap deep-links through `Route` (§8.2).

### 6.3 Notification actions

| Category | Actions | Options |
|---|---|---|
| `agent-approval` | **Approve**, **Deny**, Open | Approve requires authentication; time-sensitive; threaded by session |
| `agent-finished` | Open, **Continue**, **Merge & clean up** | Merge requires authentication; threaded by session |
| `agent-failed` | Open, Retry, Send to agent | Threaded by session |
| `pr-review` | Open, **Approve PR** | Approve requires authentication |
| `mention`, `dm` | **Reply** (text input), Mark read | Threaded by conversation |
| `ci-failed` | Open log, Send to agent | Threaded by repository |
| `ping` | Open, Mark read | Threaded by kind |

Each action ends in the same App Intent or route as its in-app button.

### 6.4 Haptics

| Event | Feedback |
|---|---|
| Send a prompt, post a message | `.impact(weight: .light)` |
| Approve, merge, run succeeded | `.success` |
| Run failed, merge blocked | `.error` |
| Destructive confirmation shown | `.warning` |
| Tab reselect, segment change, pickers | `.selection` |
| Long press opens the palette | `.impact(weight: .medium)` |

No haptics on scroll or refresh ticks.

### 6.5 Interface sounds

Both apps play the desktop's fourteen interface cues (`desktop/src/UiSoundCues.cpp`)
with the same notes and celesta, rendered by the Agent Watch synth.

| Where | Code |
|---|---|
| Cue table, weights, label rule, throttle, WAV writer | iOS `FMAgentWatch/InterfaceCues.swift`, Android `core/.../ui/SoundCues.kt` |
| Player | iOS `Theme/FmSoundCues.swift` (one `AVAudioPlayer` per clip), Android `ui/kit/FmSoundCues.kt` (`SoundPool`) |
| Tests | `InterfaceCueTests`, `SoundCuesTest` |

| Event | Cue |
|---|---|
| `FmHaptics` calls | selection, click, success, error, send, warn, open (overridable per call) |
| Buttons labelled Delete/Remove/Discard or Send/Post/Publish | delete or send (`forLabel`) |
| Tab change, screen pushed | navigate |
| Modal, dialog or sheet shown / dismissed; screen popped | open / close |
| Switch flipped | toggle-on or toggle-off (not for 0.4 s after it appears) |
| Pull to refresh | click |
| Pager page or slider step | select |
| Typing in the composer | keystroke (six seeds) |
| Prompt submitted | send; agent started success; prompt-target reply arrive |

- One gesture is one sound: cues in the same main-loop turn collapse to the
  weightiest, and within 45 ms only a weightier cue plays.
- **Touch fallback**: every tap sounds. iOS `FmTouchSoundRecognizer` compares
  the view-controller tree before and after a touch (open, close, navigate or
  click; slides that change nothing are silent; checked 80 ms after a tap,
  450 ms after a slide). Android `Modifier.fmTapSounds()` rings a click for a
  consumed tap that raised no cue.
- Chat screens are silent (`.fmQuietSounds()`, `FmQuietSounds()`).
- Sounds are on by default and play in silent mode (iOS `.playback` with
  `.mixWithOthers`, activated at launch and on activation; Android the media
  stream). Only Settings > General > Sounds turns them off (iOS
  `device/soundsOff`). They stay quiet while dictation or a voice message
  records. Settings search finds the switch under sound, mute, silent, audio,
  volume, noise, beep and click.

## 7. iPad: a split view with a sidebar

- `TabView` with `.tabViewStyle(.sidebarAdaptable)`: a sidebar in regular
  width, the phone layout in compact width.
- Sidebar: the four tabs, `TabSection`s for the hub groups, then Search and
  Settings; customizable with `.tabViewCustomization`.
- `NavigationSplitView` per list-and-detail destination: Agents (sessions,
  transcript), Inbox, Code (repositories, sections, content), Chat (channels,
  conversation, threads in an inspector), Notes, Tasks, CRM.
- Inspectors (`.inspector`) for PR metadata, issue fields and agent details.
- The new agent button floats in its own row at the bottom of the detail
  column, so content ends above it.
- Hover effects, drag and drop onto the composer and board columns, and "Open
  in New Window" for repositories, PRs and agent sessions; each scene restores
  separately.

## 8. Sitemap and routes

### 8.1 Every destination

"Tab" is where a deep link opens the route; hub destinations open on the
current tab. Desktop-only capabilities have no destination.

| Destination | Route | Tab | DESKTOP_PARITY rows |
|---|---|---|---|
| Agents root | `.agents(filter)` | Agents | 2.1, 2.2, 2.11, 2.12, 2.35 |
| Agent session (transcript, steer, stop, continue, merge, delete) | `.agent(id)` | Agents | 2.4-2.10, 2.29, 2.31 |
| Approval request | `.approval(id)` | Inbox | 2.13, 2.14 |
| New agent (composer) | sheet, `.compose(target)` | any | 2.3, 2.21 |
| Provider sign-in terminal | `.agentLogin(target)` | current | 2.22 |
| Watch field and Listen | `.agentWatch(filter)` | Agents | 2.17-2.18 |
| Teammates, teammate, editor | `.teammates`, `.teammate(id)`, `.teammateEditor(id)` | current | 3.1-3.6 |
| Shortcuts and skills | `.shortcuts(r?)` | current | 2.28 |
| Inbox | `.inbox(filter)` | Inbox | 9.1, 9.2 |
| Ping settings | `.pingSettings` | current | 9.3, 13.6 |
| Repositories | `.repos(scope)` | Code | 4.1, 4.2 |
| Repository root | `.repo(r, nil)` | Code | 4.4 |
| Code browser, file, file history, code map | `.tree(r, ref, path)`, `.file(r, ref, path)`, `.fileHistory(…)`, `.codeMap(r, ref)` | Code | 4.5-4.13 |
| Commits, commit, range | `.commits(r, ref)`, `.commit(r, sha)`, `.range(r, base, head)` | Code | 4.14-4.16 |
| Branches, compare, conflicts | `.repo(r, .branches)`, `.compare(r, base, head)`, `.conflicts(r, branch)` | Code | 4.18, 4.21, 4.22 |
| Worktrees | `.repo(r, .worktrees)` | Code | 4.20 |
| Issues, issue, new issue, board | `.repo(r, .issues)`, `.issue(r, n)`, `.newIssue(r)`, `.board(r)` | Code | 5.1-5.11 |
| Pull requests, PR, review | `.repo(r, .pulls)`, `.pull(r, n, tab)`, `.review(r, n)` | Code | 5.12-5.19 |
| Discussions, discussion | `.repo(r, .discussions)`, `.discussion(r, n)` | Code | 5.22 |
| Actions, run | `.repo(r, .actions)`, `.run(r, id)` | Code | 5.20, 6.1-6.4 |
| Releases, release, artifacts | `.repo(r, .releases)`, `.release(r, tag)`, `.repo(r, .artifacts)` | Code | 6.11-6.13 |
| Security | `.repo(r, .security)` | Code | 6.8 |
| Mirrors | `.repo(r, .mirrors)` | Code | 6.15, 6.17 |
| Projects, milestones | `.repo(r, .projects)`, `.project(r, id)`, `.milestones(r)` | Code | 5.23, 5.24 |
| Insights, quality | `.repo(r, .insights)`, `.repo(r, .quality)` | Code | 4.29-4.31 |
| Repository settings | `.repoSettings(r, section)` | Code | 4.27, 4.28, 6.10 |
| On-device repositories, local repo, quick edit | `.localRepos`, `.localRepo(id)`, `.quickEdit(…)` | Code | 4.17, 4.23 |
| Chat channels | `.chat` | Chat | 9.4, 9.8, 9.9 |
| Conversation, thread | `.channel(id)`, `.thread(channel, id)` | Chat | 9.5-9.19 |
| DM | `.dm(user)` | Chat | 9.10 |
| Discord (through the desktop) | `.discord(guild, channel)` | Chat | 9.17 |
| Mesh room | `.channel(.mesh)` | Chat | 8.7 |
| Profile (`/@name`) | `.profile(name)` | current | 8.5 |
| People | `.people` | current | 8.5, 9.10 |
| Docs | `.docs` | current | — |
| Global search | `.search(query, scope)` | current | 1.5-1.7 |
| Notes, note | `.notes`, `.note(id)` | current | 10.1, 10.2 |
| Tasks, task | `.tasks(filter)`, `.task(id)` | current | 2.30, 5.25 |
| Calendar, event | `.calendar(date, mode)`, `.event(id)` | current | 10.3 |
| Timers | `.timers` | current | 10.4 |
| Desktops, desktop | `.desktops`, `.desktop(id, section)` | current | 2.34, 4.11, 12.4, 12.12 |
| Hosts & fleet, host, provisioning | `.hosts`, `.host(name, section)`, `.provision(id)` | current | 6.16, 7.1-7.11 |
| Network, node | `.network(section)`, `.node(name)` | current | 8.1, 8.3, 8.4 |
| Market, package | `.market(filter)`, `.package(id)` | current | 11.1, 11.2 |
| CRM, contact, organization | `.crm(pipeline)`, `.contact(id)`, `.crmOrg(id)` | current | 11.3 |
| Marketing, analytics, competitors, ads | `.marketing(…)`, `.analytics(…)`, `.competitors`, `.ads` | current | 11.4-11.7 |
| Ledger | `.ledger(section)` | current | 11.8 |
| Vault, coves, calories, on-device AI, activity | `.vault(…)`, `.coves(r)`, `.cove(id)`, `.calories`, `.localAI`, `.activity` | current | 2.33, 11.11-11.14, 16.1 |
| Stats, status, admin | `.stats`, `.status`, `.admin(section)` | current | 12.1, 12.5-12.8 |
| Settings and sections | `.settings(section)` | current | 13.x, 14.x |
| Installed apps | `.settings(section: "apps")` | current | 13.16 |
| Organizations | `.orgs`, `.org(name)` | current | 13.10 |
| Projects, project agent access | `.projects`, `.projectAccess(id)` | current | 1.3, 2.36, 15.x |
| Tour | `.tour` | current | 1.15 |
| Report a bug | shell page or sheet | any | 12.14 |
| World | opens the hosted World | — | 9.22 |
| Share extension, then markup | extension, then `.compose(target)` | Agents | 11.15 |
| Update required | full-screen cover | — | 1.28 |

### 8.2 URLs

**Universal links (canonical).** Links on the served hosts
(`app.forkmesh.com`, `forkmesh.com`, and the older `app.next.forkmesh.com`
still listed in the entitlements) open the app when installed and the path
is supported, otherwise the website. The site serves
`/.well-known/apple-app-site-association` (`app/src/app_links.py`). Paths are
the website's own:

| Path | Route |
|---|---|
| `/<owner>/<repo>` | `.repo(r, nil)` |
| `/<owner>/<repo>/tree/<ref>/<path>`, `/blob/<ref>/<path>` | `.tree`, `.file` |
| `/<owner>/<repo>/issues[/<n>]`, `/pulls[/<n>]`, `/discussions[/<n>]` | `.repo(r, .issues)`, `.issue`, … |
| `/<owner>/<repo>/commit/<sha>`, `/commits/<ref>` | `.commit`, `.commits` |
| `/<owner>/<repo>/actions[/runs/<id>]`, `/releases[/tag/<t>]`, `/mirrors`, `/agents`, `/worktrees`, `/about`, `/shortcuts` | the matching section, `.run`, `.release`, `.shortcuts(r)` |
| `/@<name>` | `.profile(name)` |
| `/dashboard/chat`, `/notes`, `/tasks`, `/calendar`, `/settings[/<section>]`, `/network`, `/ads`, `/repos`, `/profile`, `/people`, `/docs` | the matching route |
| `/dashboard/apps` | `.settings(section: "apps")` |
| `/ledger.html`, `/market.html`, `/status.html` | `.ledger`, `.market`, `.status` |
| anything else | opens in the browser |

`oauth` is a reserved owner, so `/oauth/authorize` always opens in the
browser, as does `/dashboard/apps/build`.

**The custom scheme.** `forkmesh://` takes the same paths plus app-only routes:
`forkmesh://agents/<id>`, `forkmesh://approvals/<id>`,
`forkmesh://compose?repo=o/r&text=…`, `forkmesh://search?q=…&scope=…`,
`forkmesh://tasks/<id>`, `forkmesh://ios-udid`. `RouteParser` returns nothing
for `forkmesh://ios-udid` and `forkmesh://surface/open?route=…`, which
`AppServices.openURL` and `SurfaceRouter` handle. Add `?project=<id>` to target
a project; otherwise the link opens in the project whose host matches, or the
active one. Any app can claim a custom scheme, so links shared with people use
universal links; widgets, intents and notifications carry an encoded `Route`.

**Handling.**

1. `RouteParser.parse(url) -> (project, [Route])` returns the whole stack, so
   Back walks up naturally.
2. The router selects the route's tab, dismisses sheets, then sets that tab's
   path.
3. A signed-out or guest session keeps the route pending until sign-in.

The parser is pure, in `NIC/FMApi/Navigation`, covered by Linux XCTests.

## 9. States, offline and feedback

### 9.1 Loading, empty and error

| State | Pattern |
|---|---|
| First load | Placeholder rows or a thin progress bar after a short delay; never a full-screen spinner when anything can show |
| Refresh | Pull to refresh on every list; account event bells refresh silently |
| Empty | `ContentUnavailableView`: symbol, one line why, the next action as a button |
| Whole-screen error | The reason in plain words and Retry; details behind a disclosure |
| Partial error | An inline line above the content; shown content stays |
| Action result or failure | `FmNotice`/`FmNoticeLine` (iOS) or `FmStatus`/`FmInlineStatus` (Android) in the section where the action happened, with Undo or Retry when they apply; it stays until replaced or dismissed. Messages that belong to no page show in the shell's notice line under the top bar |
| Long operation | A progress row that stays, and a Live Activity when it may outlast the app |
| Needs the desktop | "Your desktop '<name>' is offline" with last seen, and what still works |
| Needs an admin or owner | Hide the entry point; deep links show a reason screen |

### 9.2 Offline

- One app-wide connectivity state; a slim banner says data is from the cache
  and when.
- Seen lists and details are cached per project and shown read-only.
- Signed inbox writes go to an outbox with "Pending" chips and are sent on
  reconnect; destructive and money actions are never queued.
- On the LAN without internet, desktop control and `#mesh` keep working.
- The composer keeps its draft offline.

## 10. Accessibility

- **Dynamic Type** up to AX5: rows stack, toolbars fold into menus, the diff
  goes unified and wraps.
- **VoiceOver**: reading order title, content, toolbar, tab row; one element
  per list row combining title, status and count; swipe actions are also
  accessibility actions; rotors for headings, diff files and unresolved
  threads; run status changes announced at most every 10 s.
- **Colour and contrast**: status is never colour alone; Increase Contrast and
  Reduce Transparency are supported (glass becomes solid).
- **Reduce Motion**: Watch shows a static summary, transitions become
  cross-fades, decorative animations stop.
- **Targets and input**: at least 44 x 44 pt, visible labels for Voice Control,
  full keyboard navigation on iPad, dictation in every field.
- **Audio** always has a visual counterpart.

## 11. Rules for new screens (checklist)

- [ ] It has a `Route` case (`NIC/FMApi/Navigation/Route.swift`) registered in
      `RouteRegistry` from its area's installer.
- [ ] It is in the sitemap (§8.1), and its URL in §8.2 if the website has the
      same page.
- [ ] `RouteParser` tests cover its URL, and a restoration round-trip test
      encodes and decodes its route.
- [ ] It pushes onto a stack or opens a sheet; it never replaces a tab root or
      hides Back.
- [ ] Toolbars: at most three items per side; destructive actions in menus,
      confirmation dialogs, or with Undo.
- [ ] Loading, empty, error, partial-error and offline states (§9) from the
      shared components; feedback inline, never a toast.
- [ ] Destructive, money and remote-machine actions confirm, with Face ID where
      DESKTOP_PARITY says so.
- [ ] If it can be the composer's context, it tells the composer.
- [ ] Main actions exist as App Intents where useful (§6.1); haptics follow
      §6.4.
- [ ] Accessibility (§10) checked.
- [ ] iPad: right split-view column, inspectors, hover, keyboard shortcuts,
      "Open in New Window" for document-like objects.
- [ ] Liquid Glass only for bars and floating controls.
- [ ] iPhone: a closure-pushed page adds `.shellPageClearance()`; a bottom
      composer uses `.shellDock`. A floating action button never covers the
      last row (iOS `.safeAreaInset(edge: .bottom)`, Android 88 dp list
      padding).
- [ ] Guest writes are wrapped in `GuestGate`.
- [ ] Scoped to the active project: cache keys and restoration paths are per
      project.
- [ ] Logic in `NIC/` with Linux XCTests, views in `NI/`.

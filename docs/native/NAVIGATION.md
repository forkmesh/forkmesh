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

The **new agent button** opens the composer (tap) or the voice prompt (touch
and hold, listening until the finger lifts, §2.3; Android does the same since S015). On iPhone it is a 44 pt circle left of the avatar in the top bar and the
chat room header (`ShellNewAgentButton`); the DM and group conversation header
leaves it and the avatar out to make room for the name, and opening the channel
list brings the top bar back. It shows one
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
bottom edge holding up to `ShellBarStyle.capacity` slots: `limit` (3 to 8,
default 6; a folder is one slot) per row, times one or two rows. Home is always
one of them (`ShellBarPrefs.keep`).

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
  activity, badge look (`ShellBarMarks`), frequent-use glow and height. The
  default writes no key.
- **Height** (`ShellBarStyle.rows`, `ShellBarRows`). `single` (default) is one
  row. `double` makes the bar twice as tall: the slots fill two rows, the first
  row taking the extra one when the count is odd (`rowSplit`), so it holds twice
  the slot count without scrolling. Both rows scroll sideways together. Rows
  sit `gap` apart; a full-width squares or circles bar spreads each row across
  the bar. A double capsule turns into a rounded rectangle with a one-row
  capsule's corner radius, and only the four corner slots take the outer
  radius, so the glass does not clip the icons. One slot keeps a single row.
- **Badge look** (`ShellBarStyle.marks`). `home` (default) draws bar badges
  and activity exactly as `ShellHomeIcon` does, so the bar matches the grid
  above it: solid accent `ShellHomeCountMark` pills anchored inside the icon's
  top-right corner, the heartbeat count mirrored on the top-left, and
  `ShellHomeActivityRing` tracing the tile (a halo for untiled icons). `plain`
  keeps the older bare blue numbers beside the glyph. Capsule and panel bars
  clip their content, so on them the pill is lowered until it overhangs the
  glyph by no more than 6 pt. Home-style marks became the default after the
  operator asked for the bar to look like the grid (2026-10-15); saves without
  the key get it.

**Home** (`ShellPage.home`, `ShellHome`) is the icon grid of every
destination, in sections, with counts and activity:

- Sections: Go to, the current repository's sections (Pull requests, Issues,
  Commits, Branches, Releases, Actions, Discussions), and the hub groups (§4).
  Order is draggable and saved in `ShellMenuLayout` (`shell/menu-layout`).
- Icons (`ShellHomeIcon`): style Outline (default), Filled, Accent or Tiles; 4,
  5 (default) or 6 per row; sorting (custom, name, most or least used, recent,
  needs attention, biggest count); categories can be hidden. Settings live in
  `ShellHomeSettingsSheet` and Settings > General > Home screen.
- **Hide icons in the bottom bar** (on by default). Icons already in the
  bottom bar, folder members included, leave Home so each destination shows
  once; a Home search still finds them. iOS: `ShellHomeStyle.hideBarIcons`
  (saves without the key get it on), applied in `ShellHomeStyle.arranged`
  with the ids from `ShellBarPrefs.barIds`; the toggle sits above the Bottom
  bar link in Home screen settings. Android: `DeviceSettings.homeHidesBarIcons`
  (`home/hideBarIcons`), toggled in Settings > General > Home screen, drops
  the bar's tabs (Agents, Inbox, Repos, Code, Chat) from Home's Go to section
  (`HomeShell.homeEntries`). The operator asked for it on 2026-10-17.
- Counts come from data already loaded or fetched when Home opens
  (`ShellNavigatorCounts`; `ShellCounts`); `nil` means unknown and shows no
  pill; zero shows no pill. `NavigatorCountPlan` (FMApi) decides the fetch:
  only destinations on the grid (`HubGroups.current`, so uninstalled plugins
  and hidden admin pages cost nothing), three at a time, at most once a minute,
  every five minutes when iOS reports a serious thermal state or Low Power
  Mode, and not at all when it is critical (the counts already shown stay).
  A 2026-10-16 phone report (build 262, thermal state critical, 1.6 s of
  freezes on the Chat tab) showed every launch and Home visit starting about
  twenty loads at once (calories, steps, water, sleep, sugar break, stretch,
  notes, tasks, calendar, orgs, admin, CRM, marketing, analytics), each
  building its full screen model on the main actor; their replies landing
  together blocked the UI for 450 to 850 ms and kept the radio and CPU busy on
  a phone that was already throttling.
- **Cached counts.** iOS saves the fetched counts, the assigned-tasks badge
  and the bug count per project and user (`NavigatorCountCache`, FMApi, one
  JSON string under `shell.navigatorCounts.<project>.<user>`), so Home shows
  them at once, across launches, without asking the network. Home fetches
  only the counts it has never had (`NavigatorCountPlan.missing`), each at
  most once a session; a count is refreshed when its page opens
  (`ShellNavigatorCounts.refresh(_:)`, from `RouteDestinationView`), with
  the thin `fmLoading` bar at the top of that page while it loads. Repos and
  Tasks write theirs from the list the page already loaded
  (`ShellNavigatorCounts.adopt`, `adoptTasks`) instead of a second request.
  Day counts (calendar, calories, steps, water, sleep, sugar break,
  stretch: `NavigatorCountCache.daily`) are dropped at midnight so Home
  fetches today's once. The operator asked for it on 2026-10-19 to make
  going back to Home faster.
- **Loading on each icon.** Home has no loading bar. While an icon's count is
  being fetched, a small spinner (`ShellHomeCountSpinner`) sits where its
  count pill goes: `ShellNavigatorCounts.pending` holds the kinds of the
  running batch, and each count lands on its icon the moment its own reply
  arrives (`landed`) instead of all together at the end. The repository
  section icons and Code spin while `loadRepoCounts` reads the current
  repository (`ShellCounts.loadingRepo`), Market while the organisation
  packages load. An unread badge still wins over the spinner. VoiceOver reads
  "Loading". The operator asked for per-icon indicators on 2026-10-19.
- **Nothing resizes on load.** Home fits its tiles to the measured screen
  (`fittedTile`), and every measurement used to start at zero each time Home
  opened (its `@State` is rebuilt) and at launch (the bar height, the safe
  area and the top chrome are measured after the first frame), so the grid
  was laid out at full tile size and then shrank, scaling every icon. Those
  sizes are now remembered on the device (`ShellLayoutMemory`,
  `shell.layout.*`: bar height, bottom safe area, top chrome without a notice
  or offline strip, Home's height and section header height) and the first
  frame uses them. The organisation packages behind the Business icons (CRM,
  Marketing, Analytics and the rest) were also read from the network on every
  launch, so those icons appeared a moment later and pushed the grid into
  more rows; `HubPackageState` now saves them per project and user
  (`hub.orgPackages.<project>.<user>`, a reviewer mark included) and a read
  that misses an organisation keeps the saved ones. The current repository's
  section counts are saved per repository (`shell.repoCounts.<slug>`) and are
  read again once a session (`ShellCounts.isFresh`). Reported by the operator
  on 2026-10-19 ("choppy and shifty", "all the icons are resizing").
- **Refresh everything.** The long-press menu on Home's background (and on a
  tile or section) has **Refresh everything**: it re-reads the organisation
  packages, then every count on the grid regardless of the cache, the
  thermal state or the once-a-minute limit
  (`ShellNavigatorCounts.refresh(guest:everything:)`, the bug count and the
  RSS feeds included), the current repository's counts and pulls, the
  account packages with the Market catalog, the fleet lights, the inbox and
  the composer's targets (`HomeShell.refreshEverything`). Each icon shows its
  spinner until its number lands. The operator asked for it on 2026-10-19.
  Unread pills and the
  running-agents heartbeat count are solid `fm.accent` (the user's highlight
  colour) with `fm.onPrimary` digits, which keeps the bright dark-mode accents
  (lime, amber) legible; plain totals stay tinted `fm.count` (blue). Items
  with live work (`ShellBarItem.active`) show an activity halo drawn from a
  `TimelineView` (30 fps cap, still with Reduce Motion).
- While a shortcut runs, the Shortcuts icon swaps that halo for a circular
  progress bar (`ShellBarProgressRing`) drawn inside the icon's cell, around
  the glyph (inside the tile when icons are tiles), and the glyph shrinks to
  78% to make room. `ShellNavigatorCounts.progress` reads the running runs and
  catalog entries of `ShortcutsStore` (`startedAtMs`, `expectedMs`, the last
  run's duration) and keeps the one expected to finish last; the ring fills
  as `elapsed / expected`, capped at 99% like the Shortcuts page's bar, and
  updates twice a second. With no earlier run to estimate from it spins a
  short arc instead. It follows the Show activity setting, and VoiceOver reads
  "Running, N percent". The operator asked for it on 2026-10-18.
  The activity halo stays on outside the ring (`ShellBarCellLabel.haloOutset`
  widens it 8 pt when icons are not tiles, so the two do not overlap); the
  operator asked for the halo back the same day.
- The heartbeat (`ShellHeartbeatCount`) does not keep its own clock: it beats
  once per turn of the activity ring (`ShellActivityHalo.turn`), when the
  ring's bright head passes the top-left corner where the count sits
  (`ShellHeartbeatCount.crossing`, 225°), so pulse and ring move together.
  Both read the same wall clock, so they stay in step across separate
  `TimelineView`s. The operator asked for the accent pills and the synced
  pulse on 2026-10-15.
- The current repository's section counts (`ShellCounts.repoCount`) load
  from the shell, not only from Home, so a Pull requests icon pinned to the
  bottom bar shows its open count on any page. The Pull requests page writes
  its own open count back once its list loads (`RepoPullsTab`), and Home,
  the bar and the page's Open chip all count a pull as open unless it is
  closed or merged (`PullStatusFilter.open`). The operator asked for the
  count on the mobile Pull requests icon on 2026-10-16.
- The repository behind those icons and the **Code** button follows the
  composer: picking a repository there (or the one it settles on at launch)
  moves `ShellCounts.currentRepo` and `code/current-repo` to it
  (`CodeCurrentRepo.follow`, from `AgentStartTarget.ref`), so Pull requests
  opens that repository's pulls and shows its open count. Opening a
  repository in Code still moves them too, and the composer picks that
  repository the next time it opens there. The operator asked for the
  repository icons to follow the composer on 2026-10-18.
- **New pull requests show at once.** `ShellPullsWatch` (iOS) is the one
  reader of the current repository's pull list. It reads it when the
  repository changes, when the app comes back to the foreground, when the
  account event channel reconnects, and the moment the relay pushes a `pulls`
  event frame (sent to the repository owner's channel by `notify_repo_host`
  whenever a pull is submitted or changes). Repositories whose
  owner is someone else get no frame, so the watch also polls every 20 s on a
  cool phone, every 5 minutes when warm or in Low Power Mode, and not at all
  when critical (`NewPullsMarker.pollEvery`). The Pull requests page takes
  the watch's list instead of reading its own (`RepoPullsModel.adopt`),
  supports pull to refresh, re-reads after the New PR sheet closes, and keeps
  the list on screen when a refresh fails. Before 2026-10-18 the page read
  its list once on open and the shell never re-read it, so a new pull did not
  appear until the page was reopened; the operator asked for it to show
  instantly.
- **Unread badges on Pull requests, RSS Reader and Tasks.** The Pull
  requests icon (and the Code icon beside Repos, whose count is the open
  pulls) carries a solid accent badge with the number of open pulls newer
  than the user last saw (`NewPullsMarker`, FMCode, `shell/pulls-seen`: the
  highest pull number seen per repository, 40 repositories kept). A
  repository seen for the first time records its highest number without a
  badge, so a fresh install does not flag every open pull; the page clears
  the badge while it is open. RSS Reader shows its unread article count, the
  same number as its Unread smart feed, read from the reader's saved library
  (`RSSReaderFeature.loaded`, cached by model revision); Home's first count
  refresh of a session also refreshes the feeds that are due. Tasks shows the
  open tasks assigned to the signed-in user (`TasksListModel.mineOpen`),
  saved with Home's counts, refreshed on every `pings` frame (an assignment arrives as a ping) and
  whenever the Tasks page reloads (`ShellNavigatorCounts.adoptTasks`). The
  operator asked for the three badges on 2026-10-18.
- **Bug count.** For an administrator the Bug icon shows how many reported
  bugs are still `new`, the same number as the Report a bug sheet's Reported
  tab. While no count is saved, Home's count refresh asks
  `/api/bug-reports/inbox?status=new&limit=1` for the counts only, and the sheet writes its counts back whenever the
  inbox reloads or a status changes (`ShellNavigatorCounts.adoptBugReports`).
  Reports are readable by administrators only, so everyone else sees the
  plain icon. The operator asked for the count on 2026-10-18.
- Frequently used tiles glow (`ShellBarOrder.heat`, log scale).
- **Update dot.** A plugin tile whose Market package changed version since
  the user last opened it shows a small accent dot at the icon's bottom right
  (`ShellHomeUpdateDot`; VoiceOver reads "Updated"). `AccountPackagesStore`
  keeps the catalog version and the last viewed version of each package under
  `packages/versions` on the device. A package never opened on this device
  counts as unseen, so on a new install every plugin tile has a dot until it is
  first opened; after that the dot comes back only when a new version is
  published. Opening the plugin clears it: a Home pick
  (`markViewed(hubItem:)`), any route or tab that belongs to the package
  (`PackageGate.packageId(for:)`), its Market page, or installing it. The
  operator asked for the dot on 2026-10-16, and on 2026-10-17 for it to show
  on every unopened plugin after a new install.
- **Clear all update dots.** Long-pressing Home's empty background (half a
  second, `ShellHome.backgroundHold`, not while dragging a tile) opens a menu
  with **Refresh everything**, **Clear all update dots**, **Home settings…**
  and **Bottom bar settings…**. Clearing calls `AccountPackagesStore.markAllViewed()`, which
  records every package's latest version as seen, so the dots return only when
  a new version is published. The same item appears in the tile and section
  long-press menus while any dot shows. The background fills at least one
  screen (`minHeight: viewHeight`) so the space below a short grid answers the
  press. The operator asked for it on 2026-10-18.
  The background menu also offers **Show all update dots** while any plugin
  tile with a known Market version has no dot:
  `AccountPackagesStore.markAllUnviewed()` forgets every seen version, so each
  such tile shows its dot again until it is tapped. The operator asked for it
  the same day, to clear dots one icon at a time.
- Search is revealed by pulling Home down 56 pt (`ShellBarOrder.searchPull`);
  it filters tiles by title or section and offers **Search everywhere**. With
  VoiceOver on the field always shows.
- Home is built to fit one screen when possible (`fittedTile`).
- **Logs** and **Diagnostics** are separate, install-gated Operate tiles. Logs
  opens the live log tab; Diagnostics opens metric charts. Both share the
  telemetry data captured on the phone.
- Without section headers the grid starts 16 pt below the top chrome
  (`gridTop`, counted in the fit), so the count pills that sit above the
  first row's icons are not clipped by the bar. The operator asked for this
  on 2026-10-15; at 8 pt the pills were cut off.

**Top chrome** (`ShellTopBar`). The project icon (`ShellProjectSlot`, 40 pt,
opens the project drawer; touch and hold for 0.5 s goes to Home from any
tab, closing open overlays, the same as `openProjectHome`, 2026-10-18), the telemetry strip centred (tiny health-coloured
charts of phone metrics and, when a desktop answers `diag.watch`, desktop
memory, CPU, disk and swap), the new agent button and the avatar
(`AvatarWithDot`, rounded square). The connection status is a 2 pt stroke
around the square in the status colour (green online, amber away or
connecting, red when diagnostics need attention, grey offline), not a corner
dot, and the status emoji sits off the bottom-left corner, clear of the stroke
(2026-10-18). The top
bar has no background: on a tab root (`HomeShell.floatsChrome`, empty path) the
list scrolls under it; pushed pages start below it so their back button is
never covered. The offline strip appears only after the relay has been down
`HomeShell.offlineGrace` (4 s).

**Bare top.** Pages that own the top of the screen (agent run page, diff
pages, Watch) use `bareShellTop()`; `ShellChrome` counts registrations per tab
and hides the top bar while any is on top. `ShellRootBarHider` hides the
system navigation bar on tab roots and corrects stale top safe-area insets; on
a pop back to the root it fades the top bar in during the transition
(`ShellChrome.returnToRoot(on:)`). The hider sets the bar hidden through
UIKit, so a page pushed straight from a notification tap into a freshly shown
tab could inherit the hidden bar and lose its Back button. Ping detail pages
(`InboxItemScreen`, `PingDetailScreen`, every ping kind and every entry point:
inbox row, notification tap, Notifications list) use
`shellKeepsNavigationBar()` (`Shell/ShellPageBarKeeper.swift`), which shows the
bar again whenever the page is on top. Android's ping pages draw their own
back button (`RoutePage`, `FmPanelHeader`) and need nothing extra.

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

**Launch.** `AppRootView` shows the app over the launch colour the moment the
services exist, with no fade (the operator asked for Home to snap in on
2026-10-19), and the shell starts on Home unless a restored route, sheet or
push wins.

**Forced update.** When the App Store has a newer version than the running
build (`ClientVersionGate.availableUpdate`, the iTunes lookup for the app id) or
the server's `/api/version` names a higher `minimumClientVersions.ios` (or
answers 426), `AppHomeView` shows `UpdateRequiredView` in place of the sign-in
flow, starter pack and shell: no way past it, only **Open the App Store** (the
lookup's `trackViewUrl`). The store is read at launch and on each activation,
at most every 15 minutes (`ClientVersionGate.storeRecheckInterval`); once found,
the block stays until the updated app relaunches. Builds whose version cannot be
parsed (`dev`) and builds newer than the store (TestFlight, App Review) are never
blocked.

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
- DMs and groups use the conversation header instead: back circle, a pill with
  the avatar and the name beside it over the presence, route ("Direct · Wi-Fi",
  "Direct · Bluetooth", "Direct only") and server-copies line, and the call
  buttons ([native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md)).
- Under a DM's header, each device waiting for approval gets a banner with
  Approve, Block and the safety number; the transcript carries the matching
  system lines and "Your safety number with X changed". The DM details sheet
  has a "Safety number" row that pushes the digits, QR code and scanner.
- With "Lock chats with Face ID" on (Settings > Chat privacy), the Chat tab
  shows a lock placeholder after the app was in the background and asks for
  Face ID or the passcode when the tab is opened; "Hide chats in the app
  switcher" (on by default) covers the whole window whenever the app is not
  active.
- The channel list opens with "Recent" (the five newest conversations) and a
  small "N online" pill in that header; there are no count tiles.
- The channel list is a full-width drawer from the left that hides the room
  header and shows the shell top bar; it closes with its × button, picking a
  room, or Back on Android. Drawers do not follow swipes. Opening it hides the
  keyboard.
- The Chat tab reopens the last conversation per project
  (`ChatWorkspace.lastConversation`, Android `chat/lastConversation`) unless a
  notification or link chose one.
- An office channel push names only its `channelId`; the tap opens the room
  with that id (`ShellChatBridge.officeConversation`).
- A notification or link that opens a conversation (`ChatHub.switchConversation`)
  bumps `ChatHub.revealRequests`. The chat screens then close what was left
  covering the transcript: the channel drawer, a page pushed inside Chat, and
  their sheets and alerts. Before this, a tap switched the conversation
  underneath, so a phone left on the channel list or a details sheet stayed
  there instead of showing the chat the banner named.
- `.dm(user)` (the People sheet's Message button) names an
  account. On iOS `ChatHub.switchConversation("@user")` opens that account's
  direct message, or starts one (`ChatHub.accountRecipient` then `openPerson`),
  as the chat person sheet does; only an exact relay peer id stays a peer room.
  Before this it only matched people online right now, so anyone else opened an
  empty room titled "@direct message". A `direct:` id is opened as is, not
  wrapped as `@direct:…`.
- The composer is one glass line (`ChatComposerBar(glass: true)`) docked on the
  bar. Transcripts use `.defaultScrollAnchor(.bottom)` plus
  `chatKeepsEndInView`, and `chatFollowsOwnSend` scrolls the end into view
  whenever the newest message of your own gets newer (sending, in a room, a
  thread or Messages). It waits one main-queue turn so the new row has been
  laid out: before 2026-10-17 a sent message could stay hidden under the
  composer, because the only scroll ran in the same update that inserted the
  row, and it ran only when the last row changed, which a later-stamped message
  or event below yours prevents. Deleting your last message or loading older
  history does not scroll.
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

- **Inbox** (`InboxScreen`): one centered bar of small square icons at the
  top of the list. The filters (All, Pings, Reviews, Assigned, Mentions,
  Approvals) carry their unread count as a badge; the actions that used to
  sit in a ⋯ menu (mark all read, unread only, pause pings, discard unsynced
  changes, ping settings, delete all) are squares in the same bar, so every
  control is one tap. A phone too narrow for the bar scrolls it sideways.
  Holding a square shows its name in a small popover (`FmHoldName`), and the
  tap it ends with is dropped. The unread badges are drawn over the whole bar
  (`InboxBadgeKey` anchors) rather than on each square, because on iOS 26 the
  glass container composited the squares above their own badges (2026-10-18).
- **Market** (`MarketScreen`): one slim navigation bar line with no
  background: back, the search field (`MarketSearchField`, in the title's
  place), then a sort menu and a filter menu (`MarketSortMenu`,
  `MarketFilterMenu`) whose icons turn blue and fill when set off the
  defaults; the filter menu holds every filter as a submenu, the two toggles
  and Clear filters. The search drawer and the row of filter chips under it
  stacked three bands above the list. Flat cards with a 16:9 preview, rating,
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
  The package page's ⋯ menu also has "Send feedback or report a bug".
- **Plugin pages** (iOS): every installed plugin's own screen has a ⋯ button
  at the top right (accessibility id `plugin-more`). It opens one small popup
  (`PluginQuickSheet`, user request 2026-10-18) instead of a menu of separate
  pages: the plugin's name, a row of stars, one text box, "Send to agent" and
  "Send", and, when this phone can remove the plugin, Uninstall under a
  divider (one row per organization for an organization package).
  `RouteDestinationView` applies `pluginMenu(for:)` (`PluginMenuViews.swift`)
  to every route `PluginMenu.packageId(for:)` (FMPackages) names: the home
  screen of each package with a phone screen. Detail screens (a contact, a
  note, a cove), the Market itself, the Agents, Inbox and Chat tabs and the
  tour get none. To decide whether Uninstall is offered, opening a plugin page
  loads only the catalog, the account's packages and your organizations'
  installs (`MarketModel.refreshInstalls`, without the install counts and
  ratings the Market shows); the package's reviews load when the popup opens.
  `PluginFeedback` (FMPackages) holds what the popup sends:
  - **Send** saves the rating when a star was tapped and files the text as
    feedback when there is any; either alone is enough. Five stars need no
    words. Below five, the Market's review rules still apply, so the text box
    is the review's "what didn't work" and a second box, "What would make it
    five stars?", appears for the improvement; the field errors show under
    each box. The text goes to `POST /api/bug-reports` with `package`, so the
    issue is tagged with the plugin, whether or not the Report a bug package
    is installed. A short thanks shows on the plugin page after.
  - **Send to agent** closes the popup and opens the composer
    (`SurfaceRouter.open(.compose(...))`) with a prompt naming the plugin,
    its package id and the screen, then the text (and the stars, if tapped).
    Nothing is filed with ForkMesh unless you also press Send.
  - **Uninstall** closes the popup, then asks with the Market's confirmation
    and rules; once it is done the page closes (after the data export is
    saved, for an organization package).
  The full rating form (`RatePackageSheet`) and the full report form
  (`ReportBugSheet`, with severity and files) stay on the Market's package
  page.
  One ⋯ per screen: a screen that already has its own ⋯ menu adds
  `PluginMenuItems()` (one "Rate, report or uninstall…" row that opens the
  same popup) to the end of that menu instead, and
  `PluginMenu.screenOwnsMenu` lists those routes so the shared button stays
  off them (CRM, Special Agents, and Coves for a repository; Coves without a
  repository is the repository picker and gets the shared button).
  Calendar's Today moved from the system overflow (which draws its own ⋯) to
  a plain button for the same reason.
- **Face Changer** (`.faceChanger`, `/dashboard/face-changer`): a native
  screen (`Features/FaceChanger/FaceChangerScreen.swift`), not the package's
  web page. It previews the camera through the bundled call page with the
  Face, Background, Voice and Hear myself controls that calls also use, and
  records a clip of up to five minutes to save or share. The choices live in
  `FaceChangerStudio.shared.look` and are the ones the next call uses
  (`docs/app/chat-direct-calls.md`, Face Changer on the phones).
- **App Store Status** (`.appStoreStatus`, `/dashboard/app-store-status`,
  Insights group): the App Store Connect snapshot the desktop last published,
  read from `GET /api/app-store-status` and drawn natively
  (`AppStoreStatusModels.swift` decodes it leniently: a missing field takes
  its default, an unknown tone is neutral). Pull to refresh re-reads the
  snapshot; a newer one needs the **iOS App Store status** shortcut to run
  again, and that shortcut's detail page links here
  ([App Store Status](../marketplace/app-store-status.md)). Android:
  `Route.AppStoreStatus`, `AppStoreStatus.kt`, `feature/appstorestatus/`.
- **Runners** (`.runners`, `/dashboard/runners`, `forkmesh://runners`,
  Operate group after Hosts): the runners the account registered, with
  status, provider sign-in and accounts, and Pause, Resume and Revoke
  (`RunnersScreen.swift`, `RunnerList.swift`, `RunnersModel.swift`)
  ([Runners](../marketplace/runners.md)). Android: `Route.Runners`,
  `runners/RunnerList.kt`, `feature/runners/`.
- **Usage** (`.usage`, `/dashboard/usage`, Insights group): the paired
  desktop's Claude Code and Codex accounts, usage windows and credits, with
  the account actions and terminals (`UsageScreen.swift`, body
  `AgentAccountsPanel`), drawn natively instead of the package's web page.
  The Agents usage bars push it too
  ([Usage](../marketplace-usage-feed-mastodon.md#usage-on-ios)).
- **RSS Reader** (`.rssReader`, `/dashboard/rss`, `forkmesh://rss-reader`,
  Personal group): a local-only feed reader laid out after NetNewsWire. Feeds
  are fetched, parsed and stored on the phone (`RSSReader*.swift` in
  FMPackages; storage, refresh and App Transport Security in
  [RSS_READER_IOS.md](RSS_READER_IOS.md)). Feeds screen: Smart Feeds (Today,
  All Unread, Starred) and "On This iPhone" (folders and feeds sorted
  together), a filter for feeds with unread articles, a floating Settings
  button and a + menu (Add Feed, Add Folder); the timeline and the article
  view (JavaScript off, sanitized, reader view) push on the current stack.
  The About screen credits NetNewsWire
  ([RSS Reader](../marketplace/rss-reader.md)). Android: `Route.RssReader`,
  `feature/rssreader/`.
- **People** (`.people`, `/dashboard/people`): a campfire of five engagement
  rings with Follow and Message ([People](../marketplace/people.md)).
- **Docs** (`.docs`): the public documentation library bundled with the app.
- **Shortcuts** (`.shortcuts(RepoRef?)`): the desktop's shortcuts and skills
  for the repository in use (`ShortcutsStore.load(preferred:)`); prompt
  shortcuts can also run on the iPhone (`PhoneShortcuts`, `ShortcutMirror`).
  A shortcut whose `insights.shortcuts` entry carries `steps` (at most 32,
  each `{name, shortcut?}`; `ShortcutStep.list` drops anything malformed)
  reads as a workflow: its row adds an "N steps" line under the description,
  leaving the play button alone, and its detail page shows a numbered step
  timeline above the file text. A step whose `shortcut` names another
  shortcut of the same scope in the catalog (`ShortcutCatalog.linked`) is a
  row in the accent colour with the file name and a chevron that opens that
  shortcut's detail page; VoiceOver reads it as "Step 2 of 7, Deploy Android
  build, opens shortcut". A link to a shortcut the catalog lacks shows the
  file name in grey and does not open anything.
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
| Long press | The voice prompt (§2.3), on iOS and Android; the command palette (§5.2) is its **Commands** button |

Context rules, most specific first: an open agent session steers it; an open
PR or issue starts an agent on that repository with the item attached; an open
repository starts an agent there; an open chat channel posts there; otherwise
the last-used repository, with a chip to change it. The composer always shows
its target as an editable chip. The composer card is Liquid Glass with a close
button; agent composers carry a **Fix** button (`Please fix: <text>`,
`FMAgents/PromptFix.swift`, `ComposerRules.fixPrompt`), a mode chip and a model
chip; the shell composer shows **YOLO** and **No worktree** toggles when the run
goes to the desktop ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)).

### 2.3 The voice prompt (iOS and Android)

Holding the new agent button starts an agent by voice, without the keyboard.
`ShellVoiceLaunch` (`Shell/ShellVoiceLaunch.swift`) runs it and
`ShellVoiceLaunchOverlay` draws it over the whole shell.

- **Hold to talk** (2026-10-17). The button tracks the finger itself with one
  `DragGesture` (`ShellNewAgentPress`) instead of a tap plus a long press,
  because a long press ends the moment it is recognized and so could not tell
  when the finger lifts. Lifting before 0.35 s (`ShellNewAgentButton.holdSeconds`)
  is a tap and opens the composer; moving more than 10 pt first does nothing.
  After 0.35 s the voice prompt opens and listens for as long as the finger
  stays down: pauses no longer send (`PromptDictation.holds` skips the quiet
  timers, the two-minute limit stays). Letting go sends. The microphone keeps
  recording for another 0.25 s so the last word is not clipped, then the
  recognizer is told the audio has ended (`finishSpeaking`: `endAudio`, or
  `finalizeAndFinishThroughEndOfInput` on iOS 26) and the words go out once its
  final result arrives (at most 2 s later). The orb shows the sending state at
  once. Letting go before the microphone opens, or before anything was heard or
  the meter saw a voice, falls back to the hands-free mode below (send after a
  pause or a tap on the orb). The VoiceOver action also opens it hands-free.
- **Faster start.** Touching the button already prepares the recognizer
  (`PromptDictation.prime`: on iOS 26 it builds the `SpeechAnalyzer` and calls
  `prepareToAnalyze`, so the model is loaded before the microphone opens), and
  a held press opens the microphone after the summon chime's 0.42 s
  (`VoiceLaunchChime.summonSeconds`) instead of waiting up to 0.8 s for the
  chime's tail. The voice prompt's transcriber asks for fast results
  (`.fastResults` with `.volatileResults`), so words appear while they are
  being said; the composer's dictation keeps the default.

- **Open.** The button's sparkle circle springs out of the top bar into a large
  orb in the upper middle of a dimmed, blurred screen, with the
  `VoiceLaunchChime.summon` chime (`FMAgentWatch/VoiceLaunchChimes.swift`) and
  a medium haptic. The microphone opens when the chime ends (0.4 to 0.8 s),
  because the chime and the recording cannot share the audio session.
- **Listen.** `PromptDictation` listens as the composer's dictation does (on the
  device when it can, nothing kept). Spokes around the orb follow
  `DictationMeter`, ripples speed up with the voice, and the words appear
  under the orb as they are recognized. When the voice stops, a white arc
  fills around the orb over the pause.
- **Send.** On letting go of a held press, or, hands-free, after 3 s of quiet
  (`ShellVoiceLaunch.pause`) or a tap on the orb,
  the words go to `QuickAgentLauncher.send(prompt:images:)`: the same
  launcher as the composer, so the run uses the repository, destination,
  agent and model last used there. The screen does not switch the repository
  to the page's own, unlike opening the composer. On success the orb bursts
  into a shock ring and stars, `VoiceLaunchChime.launch` plays, and a
  snapshot of the orb flies into the Agents cell (`ShellLaunchFlight`, with
  the orb as source and card).
- **Problems.** Nothing heard: a falling `fizzle` chime, a line, and the
  screen closes after 2.2 s. Dictation refused or failing: the line stays
  until dismissed. A send that needs a fix or fails hands the words to the
  composer with its usual message and Fix button.
- **Way out.** Tapping outside the orb or **Cancel** flies the orb back into
  the button. **Type** moves the words into the composer with the keyboard
  up. **Commands** opens the command palette, so the palette is still one
  touch from the button; VoiceOver offers both as actions on the button.
- Reduce Motion fades instead of flying and stops the ripples and burst;
  Reduce Transparency uses a solid backdrop. Sounds follow the interface
  sounds setting (§6.5).
- **Android** (S015) draws the same screen with `ShellVoiceLaunch` and
  `ShellVoiceOverlay` (`feature/composer`) and differs where the platform
  does; see [ANDROID.md](ANDROID.md), "The voice prompt (S015)".

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
`ShellHubUnroll`). On iOS it is one small floating gray card (`sheetSurface`,
320 pt wide, as tall as its contents and at most 640 pt, scrolling only past
that) over a light scrim, with no title, Done button, recents or project
switcher: tapping outside it or choosing an item closes it, and choosing an
item pushes the destination onto the current tab. On iPad the same items are
sidebar sections (§7).

| Group | Items |
|---|---|
| Header | Avatar, name, status and a small Set a status button on the same row; Pocket mode, Suggestion mode and the Diagnostics switch as compact rows |
| Workspace | Notes, Tasks, Calendar, Timers, Files, NEXUS |
| Operate | Desktops, Special agents (teammates), Shortcuts, Hosts & fleet, Network & nodes |
| Business | Market, CRM, Marketing, Analytics, Competitors, Ad review (reviewers only), Ledger |
| Personal | Vault, Coves, Calories, Steps, Water, Sleep, Sugar Break, Stretch Break, Glasses Alert, Food Guide, On-device AI, Activity |
| Insights | Stats, Status, Admin (admins only) |
| App | Settings, Organizations, People, Docs, Design, Report a bug, Tour, World, Workflows (native, [WORKFLOWS.md](WORKFLOWS.md)), Sign in/out |

Items follow the package gate (§2.0.2). On iPhone, where Home and the bar hold
the links, the hub keeps the header rows and the Account items (Settings, Sign
in or Sign out) as small side-by-side buttons; projects are switched from the
command palette's Switch project (and the iPad sidebar).

- **Diagnostics switch** (under Suggestion mode, `hub-diagnostics-strip`;
  `diagnostics/showStrip`, on by default) shows or hides the telemetry strip and
  the profile footer.
- **Version row** (last row; iOS `YouHubSheet.versionRow`, one small darker
  rectangle; Android `HubVersionRow`): "Version x (build)" with a slightly
  larger **What's new** under it, opening the bundled changelog in a sheet
  ([MAC_BUILD.md](MAC_BUILD.md)), and a compact **Send diagnostics** capsule.
  None of the three truncates: on iOS each keeps its full width, and when the
  320 pt card is too narrow for both (a long build number, "Slow, send
  report", larger text) the capsule drops to its own line under the version
  instead of squeezing it. The capsule turns red ("Slow, send report") when `PerformanceAlert.evaluate` finds,
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

The prompt button's **Commands** action (in the voice prompt, §2.3, and for
VoiceOver; on Android only the voice screen) opens a sheet with the field focused:
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
  inbox, Open approvals, Start a new iOS build) are Shortcuts actions without a
  built-in phrase. **Start a new iOS build** (`StartIOSBuildIntent`,
  `Intents/BuildIntents.swift`) runs the office MacBook's
  `deploy-native-ios-ad-hoc-mac.sh` through `ShortcutsStore`, the same
  prepare/run pair as the Desktops page's iOS button (`BuildMacShortcut`): a
  shortcut keeps the intent's title as its name, so "Hey Siri, start a new iOS
  build" works without saying ForkMesh. It speaks its own confirmation (the
  desktop's summary is written for the screen) and declines while that
  shortcut is already running, since a second run would restart it. Merge
  and delete spend the desktop's confirmation token; the spoken confirmation
  and `.requiresAuthentication` stand in for the in-app Face ID prompt. A cold
  launch from Siri's compose request is picked up in `HomeShell.start()`.
- The watch app has its own `NewAgentIntent` ([PARITY.md](PARITY.md), 4.3.2).
- **Focus filter**: choose which project the app shows.

### 6.2 Widgets, Live Activities and controls

- **One summary Live Activity** (`ActivitySummaryAttributes`,
  `ForkMeshWidgets/SummaryLiveActivity.swift`) instead of one per agent, so the
  Dynamic Island holds a single item: agents needing you, running and queued;
  the first three active agents with their elapsed time; running desktop
  shortcuts with a progress bar timed from the last run's duration; the
  planning timer; and the unread count on the Lock Screen. Approve (with
  authentication) shows for the first pending request, Stop when exactly one
  agent runs. Compact view: the lead icon and a count of agents plus
  shortcuts, then a shortcut's progress ring, the timer, or the elapsed time.
  `ActivitySummaryPlanner` starts it while the app is in front, updates it,
  and ends it when nothing is left (kept while the agent list is failing to
  load). Walks keep their own Live Activity, since a walk starts in the
  background. The app updates them locally; they go stale after 15 minutes
  (or a minute after the timer ends). They are requested without a push type:
  the served stack has no `liveactivity` sender or token route.
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
| Long press opens the voice prompt | `.impact(weight: .medium)` (Android: `LongPress`) |

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
  records, and while another app's call is up (iOS,
  [OTHER_APP_CALLS.md](OTHER_APP_CALLS.md)). Settings search finds the switch
  under sound, mute, silent, audio, volume, noise, beep and click.

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
| `/market/<package-id>` (plugin share link; not `/market/previews/…`) | `.market`, then `.package(id)` |
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
| Any load (iOS) | `.fmLoading(isLoading)`: a 2 pt accent bar across the top of the surface, shown only once the load has run 250 ms and faded out when it ends. Core Animation moves the bar, so it costs no `body` redraws. Screens with nothing to show yet use `FmSkeletonList`; the 36 pt `FmCircularProgressIndicator` is not used as a page loader |
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

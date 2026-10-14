# Native app navigation and information architecture

Status: design, 2026-09-24. WS8 implements it (see
[DESKTOP_PARITY.md](DESKTOP_PARITY.md) §19). Targets are iOS 26 and iPadOS 26
(Liquid Glass), following Apple's Human Interface Guidelines. The app's
default instance is **ForkMesh Next** (`app.next.forkmesh.com`).

Row numbers such as **2.13** refer to DESKTOP_PARITY.md. Row numbers such as
**#54** refer to PARITY.md.

## 1. Principles

1. **The phone's job is to answer, steer and review.** On a phone, people check
   on agents, answer what needs them, review code, and talk. The desktop is
   where long work runs. The four tabs follow that order of use.
2. **One tap to prompt.** The prompt button is always on screen, next to the
   tabs. It opens the composer immediately, already aimed at whatever is on
   screen.
3. **Everything else is one level away.** It is two taps through the You hub,
   or one search away through search, the command palette, Siri or Spotlight.
   Nothing lives only in a hamburger menu or a gesture.
4. **Every screen is a route.** Each screen has a typed `Route` value, a URL,
   and a restorable place in a stack. Deep links, notifications, widgets,
   intents and state restoration all go through that one type.
5. **System components first.** Use `TabView`, `NavigationStack`,
   `NavigationSplitView`, `.searchable`, `ContentUnavailableView`, system
   sheets and toolbars. They get Liquid Glass, accessibility and iPad
   behaviour for free. Custom chrome has to match them item for item (§10).

## 2. The primary destinations: four tabs and the prompt button

WS3 is building the bottom row: **four Liquid Glass tab buttons, left-aligned,
and a separate round prompt button on the right**. The user asked for this,
and it stays. The four tabs are:

| Position | Tab | SF Symbol | Root screen | Badge | Why it is primary |
|---|---|---|---|---|---|
| 1 (default) | **Agents** | `sparkles` | Needs you, Running, Queued, Recent; a filter for All, Desktop and Cloud | Count of runs waiting on you (approvals, failures) | It is the main reason to open the app. The composer and the "send straight to agent" work both assume agent-first use. Pings and Live Activities send people here |
| 2 | **Inbox** | `tray` | One list: pings, review requests, assigned issues and tasks, mentions, agent approvals | Unread count (the bell badge today) | The desktop's Pings page and the bell are how people come back. Merging them with review requests and assignments gives one place for "what needs me" |
| 3 | **Code** | `chevron.left.forwardslash.chevron.right` | Repositories: pinned and recent, yours, organizations, network catalog, on-device | None | Review, browse and merge. Today it is the first tab, but people usually arrive at a specific PR or issue from Inbox or a link |
| 4 | **Chat** | `bubble.left.and.bubble.right` | Channels grouped by category, DMs, `#mesh`, office rooms | Unread mentions and DMs | Conversation is frequent but secondary. It stays one tap away because DMs are time-sensitive |
| Trailing | **Prompt button** | chosen by WS3 | Opens the composer sheet (WS3) with the current context: repo, issue, PR, agent or channel | Dot while a draft exists | Starting or steering an agent is the one action worth a permanent control |

These are not tabs:

- **Search.** iOS 26's trailing search tab would compete with the prompt
  button. Search is reached from every tab root (§5), the command palette and
  the iPad sidebar.
- **The You hub.** It opens from the avatar button in the top-trailing corner
  of every tab root, as in the App Store, Music and Photos. It holds everything
  that is not one of the four tabs (§4).
- **More.** HIG discourages a More tab, and the hub replaces it.

### 2.0 The iPhone carousel (2026-09-29)

On iPhone the user asked for the bottom row to become one full-width Liquid
Glass capsule that scrolls sideways (`ShellBottomBar`, a horizontal
`ScrollView` clipped to the capsule). It holds, left to right: the four tabs,
**Search**, **Bug** (Report a bug), then every link that used to be in the
avatar hub's groups (Workspace, Operate, Business, Personal, Insights and the
App links: Settings, Organizations, Tour, World), built from
`HubGroups.current` so guest gating and package filtering still apply.

- Every item has a small label under its icon, and a red count pill when it
  has a count: Agents shows how many agents its list holds
  (`AgentsScreen` publishes it to `ShellCounts.agentsTotal`; running runs
  until 2026-10-01, when the user asked for the total), Inbox its unread count, Chat its unread messages. Repos
  has no count (§2.0.1). The hub links have no counts yet.
- The row scrolls the selected tab into view when the tab changes.
- The prompt button moved to the header, left of the avatar, where Search
  was. A tap opens the composer, a long press the command palette, as before.
  On 2026-10-01 it moved again (§2.0.12).
- The avatar hub on iPhone keeps only the header, recents and Sign in or Sign
  out (`YouHubSheet.linksInBar`), plus Settings since 2026-10-09 (§4). The
  iPad keeps its sidebar, full hub,
  header Search button and floating new agent button.
- The header's desktop-on-this-network button showed a question mark:
  `desktop_windows_outlined` was missing from `MaterialIconCodepoints`, so
  `FmIcon` drew `help_outline`. The header now draws the `desktopcomputer`
  symbol, and the table has the codepoint for the other screens that use it.

### 2.0.1 Repos and Code (2026-09-29)

The user asked for two buttons in place of the Code tab: **Repos**, the
repository list, and **Code**, which returns to the repository in use.

- The third tab is labelled **Repos** (`books.vertical`) and still is
  `AppTab.code` underneath, so routes, saved navigation, deep links and the
  keyboard shortcut are unchanged. It shows no count: the user asked on
  2026-09-30 for the repository count to come off the icon on both phones.
  A tap always shows the list: whatever is pushed on the tab is popped.
- **Code** (`chevron.left.forwardslash.chevron.right`) sits right after
  Repos. It opens the repository in use: the last repository screen shown,
  remembered across launches (`CurrentRepoMemory`, key
  `code/current-repo`). It always lands on that repository's own screen with
  nothing pushed above it, from any tab and from any page inside the
  repository (the user asked on 2026-09-30 for every bottom button to go to
  its top page). Before any repository has been opened it shows the list and
  says so inline (iOS: the shell's notice line under the top bar; Android: a
  line just above the bottom bar, cleared on the next tab change; no snack bar since 2026-10-09, the user asked for no
  toasts).
- Repos is highlighted while the tab shows the list, Code while a screen is
  pushed on it.
- Android does the same in `BottomMenu` and the wide-screen rail
  (`library_books` and `code` icons; the rule is `CodeButton.plan` in core):
  when the repository's screen is already at the bottom of the Code stack it
  pops back to it and keeps its state, otherwise it pops the stack and opens
  a fresh repository screen.

### 2.0.2 The chat room header (2026-09-29, iOS)

The user asked for the chat room to look like a Slack channel on iPhone: no
top bar, the room's details in its place.

- On the Chat tab the shell's top bar and telemetry strip are hidden
  (`HomeShell.shellColumn`); the connection banner stays. Screens pushed on
  the Chat stack keep their own navigation bar.
- The room header is a row of glass shapes: a round back chevron that opens
  the channel list (red dot for unread rooms elsewhere), a pill with the room
  icon, name and `ChatHub.headerDetail` ("346 members • 4 tabs" for a
  channel since 2.0.97, "Direct message • end-to-end", or the mesh
  transports), and a capsule with the
  prompt button and the account avatar, since the top bar that held them is
  gone. Tapping the pill explains the room's encryption.
- An empty relay or Office room shows a Slack-style intro at the bottom: the
  icon and name large, "This is the very beginning of the … channel." and,
  when this account can do them, **Add description** (puts `/topic ` in the
  composer, managed Office rooms without a topic) and **Add people** (the
  invite sheet for a private relay room, channel settings for a private
  Office room). The empty mesh room keeps its explanation of where messages
  travel.
- The composer's placeholder names the room ("Message general").
- The channel list slides in from the left as a drawer, not a bottom sheet
  (2026-09-30, iOS and Android phones): the back chevron (iOS) or menu button
  (Android) opens it over a dimmed room, about 85% of the screen wide (at most
  360 pt/dp; full width since 2.0.80). Its buttons, picking a room or the
  system back (Android) close it; swiping does not move it (2.0.96). Wide
  layouts keep the fixed sidebar.
  On iOS phones the open drawer swaps the room header for the shell's top
  bar (2.0.11).

### 2.0.3 The More button and the page grid (2026-09-30)

The user asked for the last icon of the phone's bottom bar to become a
three-dot button that opens a compact grid of every page, organized.

- **More** (`ellipsis` on iOS, `more_horiz` on Android) is always the last
  item of the bar. On iPhone it sits inside the glass capsule, pinned to the
  right of the scrolling carousel behind a thin divider, so it shows without
  scrolling. On Android it takes the slot the prompt button held, after Chat.
- A tap opens a sheet of small tiles, five to a row (icon, label and count
  pill, as in the bar), under section headers: **Go to** (the bar's own
  items: Agents, Inbox, Repos, Code, Chat, then Search and Bug on iOS; Prompt
  first and Search and Commands last on Android), then the hub's groups in
  the hub's order (Workspace, Operate, Business, Personal, Insights, App).
  The groups keep the hub's guest, admin and package filtering and only list
  pages (`HubCatalog.pages` on Android, `HomeShell.movesToBar` on iOS): Set
  status, Sign in and Sign out stay in the avatar hub.
- iOS first ran the picked item's action once the sheet had gone (`onDismiss`),
  so items that open their own sheet (Search, Bug) could. The grid's tiles are
  never the launch sparkle's target; only the bar's Agents cell is.
- Android's prompt button moved to the header, between the command palette
  button and the avatar (38 dp, long press opens the command palette), as on
  iOS (§2.0). The header only shows at a tab's root, so the grid's **Prompt**
  tile opens the composer from pushed screens. The wide-screen rail keeps its
  prompt button in the rail header and has no More item: the avatar hub
  covers it there.
- Later on 2026-09-30 the user asked for the sheet to open full, for smaller
  tiles, and for counts on the tiles where they apply. The sheet opens at
  full height (iOS `.large` detent only; Android's grid fills the expanded
  sheet). Tiles are 46 pt / 52 dp tall with 17 pt / 18 dp icons and tighter
  section headers, still five across so labels such as "Notifications" fit.
- Hub tiles (and the same items in the iPhone carousel) show counts that
  are already loaded app-wide, with no fetch
  when the sheet opens: **Timers** (running timers; iOS mirrors the count
  into `ShellCounts.timers` from `PlanningTimersRuntime.reschedule`), **Hosts**
  (runners waiting for sign-in, from runner attention) and, on Android,
  **All agents** (`AgentsSurface.activeCount`; iOS has no such hub tile).
  Counts of things running use the accent pill, as the bar's Agents badge
  does when nothing needs you; counts that need attention stay red. Pages
  whose data loads only when opened (Desktops, Teammates, Coves, Tasks) and
  counts that are never zero (online nodes, paired desktops) get no pill.
- Later on 2026-09-30 the user asked for the change from a grid pick to the
  page to be a lot faster. iOS waited for the sheet's dismiss animation to
  finish (`onDismiss`) before opening the page, which then played its own
  push animation. A tile now opens its page at once, with the push animation
  off (`ShellBottomBar.runUnderSheet`), so the page is already in place as
  the sheet slides away. Items that open a sheet of their own still wait for
  `onDismiss` (`ShellBarItem.presents`): Search, Bug, modal routes, and pages
  that ask a guest to sign in first (`HomeShell.hubPresents`). Android's
  grid already closed and navigated in the same tap. Superseded on iOS by
  §2.0.6: the grid is no longer a sheet.

### 2.0.4 Navigator settings: recently used first (2026-09-30, iPhone)

The user asked for settings on the phone's bottom navigator, the first being
to sort the carousel's icons by recently used, with a pick from the More
grid animating to the front of the row.

- The settings are a gear menu at the top of the More sheet (titled
  **Navigator**), and the same items on a long press of the More button:
  **Sort by recently used** (off by default) and **Clear recent order**.
- Every tap on a carousel cell or a grid tile is recorded, whether the
  setting is on or not, so turning it on already reflects recent use. The
  list keeps the last 48 ids (`ShellBarOrder` in `HomeShellLogic.swift`,
  keys `shell/bar-recent-first` and `shell/bar-recent`, shared by every
  project). More itself is never recorded.
- With the setting on, the carousel lists the used items most recent first,
  then the rest in their usual order. The reorder springs into view and the
  carousel scrolls to its leading edge (on a change of the most recent id)
  to show the item arriving at the front. Since §2.0.6 the grid is a page
  the bar stays under, so a grid pick is recorded in the same tap. Reduce
  Motion drops the animation.
- The grid itself keeps its fixed order, so its tiles stay where the hand
  expects them. The iPad sidebar and Android's fixed bar are unchanged.
- On 2026-10-01 the user asked for a most-frequently-used sort as well. The
  toggle became a **Sort** picker with three choices (`ShellBarSort`, key
  `shell/bar-sort`): **Usual order**, **Recently used** and **Most used**.
  An old `shell/bar-recent-first` switch reads as Recently used and is
  dropped on the first pick. Every tap also bumps a per-item use count
  (`shell/bar-uses`, `count:id` lines); Most used lists items by count, ties
  going to the more recent one, then the unused items in their usual order.
  The counts keep 48 ids, dropping the least used but never the item just
  tapped, so a new item can climb. With no counts stored yet, each id in the
  recent list counts once, so Most used starts out as the recent order.
  **Clear usage history** empties both lists. The carousel scrolls to its
  leading edge when the item at the front of the active sort changes.

### 2.0.5 The version under the navigation (2026-10-01, iOS)

The user asked for the app's version to move from the header to under the
page navigation, with an Update button when a newer release is out. This was
first built on the desktop by mistake and reverted there.

- The `v1.2.5+14` badge left the header's project slot. `ShellVersionFooter`
  (`ShellTopBar.swift`) shows it centered under the bottom bar on iPhone and
  at the foot of the sidebar on iPad (`safeAreaInset`). It keeps the build
  number, since consecutive TestFlight builds differ only there, and hides
  with the bar while the composer is open.
- At launch `UpdateRequiredCheck.checkStore()` asks the App Store once
  (`ClientVersionGate.availableUpdate`, `itunes.apple.com/lookup?id=…`). If
  its version is newer than the running one, ignoring the build number, an
  accent **Update v1.3.0** pill sits left of the version and opens the
  release's App Store page (`trackViewUrl`, else `defaultStoreURL`). A
  failed or empty lookup shows nothing. The required-update screen
  (`/api/version` minimums) is unchanged.

### 2.0.6 Bottom bar pages load in place (2026-10-01, iPhone)

The user asked for every page the bottom navigation opens to load instantly
and be a regular page, with no transitions or modals.

- Every carousel cell and grid tile runs its action through
  `ShellBottomBar.run`, which records the use and then runs the action in a
  transaction with animations disabled: tab switches, the Code cell's
  repository, Search and the hub's pages appear without the push slide. A
  guest's hub pick that first asks for an account opens its page the same
  way once signed in.
- **More** no longer opens a sheet. `HomeShell.page` (`ShellPage.menu`) shows
  the grid (`ShellMenuPage`, accessibility id `shell-menu-page`) in the page
  area, above the bar (full screen, without the header, since §2.0.15), with
  More highlighted; a second tap on More returns to the page underneath.
- **Bug** no longer opens a sheet either: `ShellPage.reportBug` shows the
  report form (`ReportBugSheet` with `onClose`) in the page area, with Bug
  highlighted. The iPad's floating bug button shows it the same way. The
  avatar hub's Report a bug, inside the hub sheet, is unchanged.
- The tabs stay mounted under such a page, hidden. Any bar pick, a tab
  switch, a change of the current tab's stack, a routed push or link, Back
  and Escape on a keyboard all close it. The chat tab's own header is not
  shown over it, so the shell header is.
- A tab is built the first time it is selected and then stays mounted,
  hidden (`HomeShell.mountedTabs`), so switching back is instant. Agents is
  the exception: it is mounted 2 s after launch even if never opened,
  because its list feeds the bar's running-agents count (`ShellCounts`) and
  the Home page's agent dots (`AgentDotsFeed`). Chat and Code wait for their
  first visit (2026-10-02: building all four at launch ran every tab's
  loads at once and caused the launch stalls).
- `ShellBarItem.presents`, `HomeShell.hubPresents` and the sheet's
  `onDismiss` hand-off (`runUnderSheet`, `landed`, `picked`) are gone.
- The dead second `enum ShellPage` in `ShellBridges.swift` (the old page
  list with `notifications`, `settings`, `route(Route)`...) redeclared the
  live one in `ShellBottomBar.swift` and was removed with the Home page.

### 2.0.7 Home: the heads-up display (2026-10-01, iPhone)

The user asked for a home screen with tiny stats all over it: about seven
heads-up stats above the fold, quick links to everything, animations of what
is in progress, and a striking look.

- **Where it lives.** Home is a page like More and Bug (`ShellPage.home`),
  not a fifth tab, so `AppTab`, the saved navigation and the four-tab rules
  above are unchanged. The carousel's first cell is **Home** (`house`,
  accessibility id `bottom-bar-home`). The page is `ShellHome` (accessibility
  id `shell-home`), under the header, telemetry strip and agent dots.
- **At launch.** On iPhone the shell opens on Home, from the first frame since
  §2.0.17, and keeps it once the saved navigation is restored. A quick action
  or notification action that opened a route, a presented sheet, or a tapped
  push wins over it. The iPad's split view has no bar cell for Home, so it
  does not open there.
- **Staying open.** Restoring navigation changes the selected tab and stack
  after the page is set, which would close any page. `HomeShell.homeHold`
  records the tab and stack Home was opened over, and the tab and stack
  `onChange` handlers keep Home while they still match. Any real change closes
  it as before.
- **The stats.** Four tiles in one row (`HomeDashboard` in FMApi, tested in
  `HomeDashboardTests`): Agents, Pings (unread inbox), and the app's memory
  and requests per second with their last minute drawn behind the number.
  Agents counts every running agent: working plus waiting, where working is
  at least the active lines the Agents tab lists (`ShellHome.live` takes the
  larger of those and the watch's tally), not only the dozen the watch
  streams. The detail names failed, then waiting, then "running", idle or
  done. On 2026-10-02 the user crossed out Chat, Mesh, Timers and Hosts from a
  screenshot, so `HomeDashboard.stats` returns only Agents and Pings; the
  other builders and their tests stay. Each has a tone: idle grey,
  live accent with a moving waveform, attention amber with a pulse, alert red.
  A tile opens its place: the Agents or Inbox tab, or Diagnostics on that
  metric in a sheet. Numbers roll with
  `numericText` transitions.
- **The hero.** A radar (`ShellHomeRadar`) with a rotating sweep: every agent
  the Agents tab lists (the same merged lines its badge counts, not only the
  dozen it streams; 2026-10-01, the user had 100 and the radar showed 18) is a
  blip coloured by status: purple active (running or queued), orange waiting
  on you (ringed), red failed or stopped, green done. They sit on a sunflower
  spiral, active ones nearest the centre and done ones toward the rim, and
  shrink as the count grows so a hundred still fit. A blip flares as the beam
  passes, people online are faint dots on the edge, and the core beats in the
  relay's colour. Beside it: a greeting with the first name (the clock beside it was
  dropped 2026-10-02 at the user's request), a
  one-line headline of what needs the user, and chips for the relay, the
  app's uptime and the current repository. Corner brackets frame the hero and
  the idle card for the heads-up look; the page sits on a faint dot grid.
- **Next task (replaced In progress, 2026-10-01).** The user asked to drop the
  row of in-progress agent cards (the agent dots and the radar already show
  them) and have the on-device Gemma suggest the next task instead, learn from
  what is picked, refresh on demand, and start an agent in one tap. The card
  (`ShellHomeNextTask`) shows the title, with a thumbs-down, **New idea** and
  **Start agent** as small icon buttons on its right and the repository and
  model under it. On 2026-10-02 the user crossed out the instruction and the
  reason and asked for the buttons on the right to make the card compact; the
  instruction is still what the agent receives and is the title's VoiceOver
  hint. The model is
  `NextTaskAdvisor` in FMAgents (tested in `NextTaskAdvisorTests`), shared app
  wide as `ShellNextTask.advisor` so a suggestion survives leaving Home.
  - *Context.* `ShellHome.gather` hands Gemma the composer's repository, the
    live agents (title, state, last line), the fifteen newest agent sessions
    and relay launches across repositories (status, failure text, merges),
    unread pings, hosts that need sign-in and the time of day
    (`NextTaskPrompt.describe`). For a repository on the mesh,
    `NextTaskRepoReader.load` adds the code history and documentation (opened
    up 2026-10-01 after Gemma kept suggesting the same walk-detection task
    with nothing but titles to go on): the 30 newest commit subjects (merges
    dropped), the ten most important open issues with a line of each body,
    the ten newest closed issues, the file list of `docs/` and excerpts of up
    to four docs (README, docs index, roadmap, todo, plan, architecture,
    changelog, agents files; code blocks, badges and HTML stripped; README
    2,000 characters, others 1,200, 5,000 in all). Commits, closed issues
    and finished agent work are marked done. Gemma answers in three lines,
    `TITLE:`, `TASK:`, `WHY:`; `NextTaskPrompt.parse` tolerates markdown and
    loose prose.
  - *Learning.* Every suggestion's fate is kept in `home/next-task/ledger`
    (newest 40): **Start agent** records it as started, **New idea** records
    the one on screen as skipped, and the thumbs-down (a menu: Already done,
    Not useful, Too vague, Wrong project, Just not this) records it as
    rejected with the reason and asks for another. Rejections also go to
    `home/next-task/denied` (newest 200) so they outlive the rolling ledger.
    Started titles, rejected ones with their reasons and skipped titles go
    back into the prompt. A run of skips or rejections raises the temperature
    (0.6 + 0.1 each, up to 1.2). Every four decisions (a rejection counts
    two) Gemma rewrites up to four short preference notes from the ledger
    and its old notes (`home/next-task/lessons`), rejections and reasons
    first; the notes lead the next prompt. The eyebrow reads
    "Gemma · learning" while that runs, and a suggestion waits for it.
  - *No repeats.* Matching by title alone let reworded copies through.
    `NextTaskSuggestion.repeats` compares stemmed content words (walking,
    walks and walk are one word; add, implement, fix and similar verbs are
    ignored) of the titles, and of the tasks when both have them. A
    suggestion that repeats a rejected, started, skipped, already shown,
    running or finished task, or a closed issue (`NextTaskPrompt.avoid`), is
    sent back in the same conversation ("X is too close to Y, which the
    developer rejected"), up to three tries, each 0.2 hotter. Failed agent
    work may be suggested again. If every try repeats, the card falls back
    to the most important open issue that is not a repeat, or says so.
  - *Training data.* Every decision is also appended to
    `Application Support/NextTask/next-task-feedback.jsonl` on the iPhone
    (`NextTaskTrainingLog`, newest ~2 to 4 MB kept): verdict, reason, the
    suggestion, the repository and the exact system and user prompt it came
    from, so started and rejected pairs can later tune a model.
  - *Start.* **Start agent** sends the task through the composer's
    `QuickAgentLauncher.send`, so it runs with the last repository, the last
    place that repository ran and the model chip, shown under the buttons.
    When nothing can take it, the composer opens with the task typed in and
    the usual one-tap fix. No new suggestion is generated right after a
    start, so an agent running on this iPhone's Gemma is not competing with
    it; the next one comes on **New idea** or the next visit to Home.
  - *Off.* With Gemma off, not downloaded or not supported here, the card
    says why and offers **Set up Gemma** (the On-device AI settings in a
    sheet); closing it tries again.
  Home still runs its own `AgentDotsWatch` for the radar and the Agents tile,
  and calls `advance()` on each revision so finished marks are dropped
  without the cards' frame loop.
- **Quick links.** Every carousel and hub destination in its group, six
  across, with the bar's badges; picks go through `ShellBottomBar.run`.
- **Cost.** Only the radar draws per frame (24 fps cap); it and the waveforms
  stop under Reduce Motion and in the background. Gemma runs only on the
  first visit, **New idea**, the thumbs-down, a retry when it repeats itself
  (at most three runs) and the reflection every four decisions. The repo
  reads (issues, history, two tree listings, one batched blob read) use the
  API's caches. The
  telemetry tiles and the uptime chip are their own views, so the 1 Hz
  telemetry tick redraws only them.

### 2.0.8 The agent run page owns the top (2026-10-01, iOS)

The user asked for the agent run page (`AgentRunScreen`) to start at the top
of the screen, with the run's full title visible.

- While the page is on screen the shell's top bar (project, connection line,
  avatar), telemetry strip and agent dots are hidden, so the back button and
  the action buttons sit at the top. The page tells the shell through
  `ShellChrome`: it counts, per tab, the screens that want the bare top, and
  `HomeShell.shellColumn` hides the three rows while the selected tab has
  one. The page registers on appear with the tab that was selected and
  releases on disappear, so a screen pushed over it (Worktrees) brings the
  top bar back, and a page opened from the bottom bar (Home, More, Report a
  bug) still shows it. The connection banner stays. The diff pages keep the
  bare top too (2.0.10).
- The run's title is no longer the navigation title, which truncated it to a
  few words beside the action buttons. It sits under them as a wrapping
  heading (`agent-run-title`), above the transport badge and status line.

### 2.0.9 The project slot is just the project (2026-10-01, iOS)

The user asked for the line under the project name in the top bar
("ForkMesh - Connected - 1 node online") to go, leaving the name centred
beside the project icon.

- `ShellProjectSlot` (`ShellTopBar.swift`) takes no subtitle and draws the
  name alone, so the HStack centres it on the 36 pt icon. `ShellTopBarState`
  dropped `relayLabel`, which only fed that line. The connection state still
  shows as the dot on the avatar and in its VoiceOver label.
- `shellSubtitle` stays in `HomeShellLogic` with its tests; nothing on iOS
  calls it now. Android's top bar never had the line.

### 2.0.10 The diff pages own the top too (2026-10-01, iOS)

The user asked for the diff page to hide the top bar, telemetry strip and
agent dots the way the agent run page does (2.0.8).

- The register/release pair moved out of `AgentRunScreen` into the
  `bareShellTop()` modifier (`BareShellTop`, `ShellBottomBar.swift`), which
  keeps the tab it registered on in its own state. The agent run page and
  the three diff pages use it: `AgentDiffScreen` (desktop agent Changes and
  Merge & clean up), `RunnerDiffScreen` (runner agent changes) and
  `BranchDiffScreen` (branch and PR diffs, from agents, Code and review
  links).
- `ShellChrome` counts registrations, so opening a diff from the run page
  goes from one to two and back, and the top stays bare in both directions.

### 2.0.11 The channel list shows the top bar (2026-10-01, iOS)

The user asked for the room header to go while the channel list is out, with
the regular top bar above the list instead.

- Opening the drawer hides the room header (`ChatTranscript.showsHeader`) and
  shows the shell's top bar, telemetry strip and agent dots; closing it
  brings the room header back. The drawer then starts right under the top
  bar.
- The chat screen tells the shell through `ShellChrome.reveal(on:)` /
  `conceal(on:)`, a per-tab count beside the bare one (2.0.8). It registers
  with the tab that was selected when the drawer opened and releases when the
  drawer closes or the screen disappears, so a page pushed on the Chat stack
  keeps its own navigation bar. `HomeShell.showsTopBar` shows the top bar on
  the Chat tab only while a screen has revealed it and none wants it bare.
- 2026-10-06 (iOS and Android): opening the channel list hides the keyboard.
  Before, a focused composer kept the keyboard up over the bottom half of the
  list. iOS calls `ShellKeyboard.dismiss()` in `openChannels()`; Android's
  `ChannelDrawer` clears focus when it opens.

### 2.0.12 The floating new agent button (2026-10-01)

The user asked for the button next to the avatar to float over the carousel's
**More** (three dots) cell, be visible on every page, look like strong Liquid
Glass, and carry an icon that says what it does.

- `ShellNewAgentButton` replaces `ShellTopPromptButton` (header) and
  `ShellFloatingPromptButton` (iPad). On iPhone `ShellBottomBar` stacks it
  centred on the More cell, 24 pt above the capsule, inside the bar's
  `GlassEffectContainer`; the gap is wider than the container spacing so the
  two shapes stay apart. The bar is outside every tab's `NavigationStack`, so
  the button shows on every tab, pushed page, Home, the Navigator and Report a
  bug, and hides with the bar while the composer or the keyboard is up. The
  iPad keeps it in `floatingButtons`.
- The button first floated in an overlay 16 pt above the capsule. An overlay
  takes no layout space, so the button and its accent glow sat over the More
  cell and over the page's last rows. It is now its own row in the bar's
  `VStack`, above the capsule (`FmSpace.x5`, clear of the glow): the bar grows
  by that row, the page ends above it, and nothing can sit under the button.
- The glyph is `sparkles` (the Agents tab) with a `plus.circle.fill` badge, in
  white. On iOS 26 the circle is interactive `.regular` glass tinted with the
  accent; earlier systems draw ultra-thin material under an accent gradient
  (solid accent with Reduce Transparency). Both add a white specular rim and
  an accent glow.
- The header's prompt button and the Chat header's are gone: the header keeps
  the desktop button and the avatar, and Search on iPad only
  (`ShellTopBar.showsSearch`). A tap still opens the composer, a long press
  the command palette; the label is now "New agent".

### 2.0.13 No gap under the top bar after a bare page (2026-10-01, iOS)

The user reported a blank band, about the height of the status bar, between
the top bar and the Agents tab strip that showed up only some of the time.

- The cause was the bare top (2.0.8, 2.0.10). While the agent run page or a
  diff page is on screen, the tab's `NavigationStack` reaches the top of the
  screen and gets the status-bar safe area (62 pt on a Pro Max). After going
  back, the top bar returns above the stack, but the root screen sometimes
  kept that old top inset. `FmSegmentTabBar`'s raised background reaches into
  safe areas, so the stale inset showed up as a raised band above the strip.
- `HomeShell.tab` makes each tab root ignore the top container safe area
  while the top bar shows on that tab (`showsTopBar`). The root then sits
  under the top bar and never at the screen edge, so the right inset is
  always zero there. Inbox is left out because it shows its own navigation
  bar, which the root's content has to clear. So is a tab whose top is bare,
  where the inset is real.

### 2.0.14 Long press to manage a bar item (2026-10-01, iPhone)

The user asked for a long press on an item in the bottom navigation to manage
it the way the desktop's left rail does (desktop README, Rail folders and
Movable buttons): hide it, put it in a folder, and the rest.

- **The item menu** (`ShellBarItemMenu`, `ShellBarCustomize.swift`) is the
  context menu of every carousel cell, every cell inside a folder and every
  tile of the More grid. It offers, in order: for a package's page,
  **<Name> settings** and **Remove <Name>…**, both opening the package's
  Market page (`Route.package`), where its settings, data export and removal
  per organization live (`PackageRoutes.packageId(forHubItem:)` maps the
  bar id to the package id); **Move** (left, right, to the front, to the
  end; bar and folders only, the grid keeps its fixed order); **Move to
  folder** (each folder, the current one checked, then **New folder…**);
  **Take out of <folder>**; **Hide from bar** or **Show in bar**; and
  **Reset bar layout** once anything has been changed.
- **Hidden items** leave the carousel but stay in the More grid, marked
  with a small `eye.slash`, so the long press there brings them back. The
  Navigator settings (gear on the More page, long press on More) gained
  **Show hidden items** and **Reset bar layout**. When the Agents cell is
  hidden the launch flight lands on More; inside a folder it lands on the
  folder.
- **Folders** are cells with a 2×2 mini grid of their items' symbols, the
  folder's name, and the sum of their counts. A tap opens a popover above
  the cell with the items four across (`ShellBarFolderCell`); picking one
  closes it and opens the page. A folder sits where its first item would be
  and keeps its own order. Its long press: **Open**, **Move**, **Rename
  folder…**, **New folder…**, **Hide folder** (hides its items) and
  **Remove folder (keeps its items)**. Names come from an alert with a text
  field (`ShellBarNamingAlert`), trimmed, at most 40 characters, "Folder"
  when empty. A folder whose items are all hidden is not drawn.
- **Order.** Moving by hand writes the whole visible order, hidden items
  after it, and switches Sort to Usual order, which is now that hand-made
  order; otherwise the recent or frequent ranking would undo the move at
  once. New items with no place in the saved order go to the end.
- **Storage.** `ShellBarLayout` in FMApi (tested in `ShellBarLayoutTests`)
  holds the hidden ids, the folders and the order, saved as one JSON string
  under `shell/bar-layout`; an empty layout removes the key. Like the sort
  keys it is shared by every project.
- Android's fixed bar and the iPad sidebar are unchanged.

### 2.0.15 Every bottom button lands on its first page; the grid is full screen (2026-10-01, iOS)

The user asked for every bottom-bar button to go to its own first page (an
agent's detail plus a tap on Agents shows the agent list), and for the More
grid to fill the screen.

- Tapping Agents from an agent's detail left the detail up. Agent rows, and
  about a hundred other links across the app, push with a closure
  `NavigationLink`, which SwiftUI keeps outside the tab's `[Route]` path. The
  path stayed empty, so `AppRouter.select` treated the tap as a reselect at
  the root and only scrolled.
- `HomeShell` now counts the tracked pages showing on each tab's stack: the
  tab root and every `Route` destination carry `tabStackPage`
  (`TabStackTops`, `Navigation/TabReselect.swift`), counted up on appear and
  down on disappear. A count of zero means an untracked page covers them.
  `HomeShell.landOnTop` then gives that tab's `NavigationStack` a new
  identity (`stackResets`), which drops the untracked pages. Tab picks
  (`selectTab`: the bar, the grid, the iPad sidebar, ⌘1–4, the palette, the
  Home stats), the Code cell and every hub page (`openFromHub`) go through it.
- A hub page that is already on the current tab's stack now pops back to
  itself instead of being pushed again (`AppRouter.openFromHub`), so Notes
  from Notes → a note → Timers returns to Notes.
- While the grid (`ShellPage.menu`) is open, the shell's top bar (with the
  telemetry strip) and the runner sign-in banner are hidden. The grid starts
  at the top of the screen and runs down to the bar, which stays so More can
  close it again.

### 2.0.16 The agent list scrolls under the glass bar (2026-10-01, iPhone)

The user asked for the list of agents to run on past the bottom navigation,
under its Liquid Glass.

- The bar and the version under it (`HomeShell.barStack`) now float over the
  page area as an overlay instead of being the column's last rows. The shell
  measures their height (`barHeight`) and gives it back to the pages as
  `barClearance`: zero while the keyboard has taken the bar away or on iPad.
- Every tab but Agents, and the pages the bar opens in place (Home, More,
  Report a bug), are padded by that height, so they still end at the bar's
  top edge as §2.1 asks (issue #729).
- The Agents tab's `NavigationStack` takes the height as bottom safe area
  instead (`ShellBarClearance`, `safeAreaPadding`). Its list's `ScrollView`
  runs under the glass, which blurs the rows going past, and the last row
  still scrolls clear of the bar. Pages pushed from the Agents tab see the
  same safe area, as under the system tab bar.

### 2.0.17 A still first open, quiet transitions (2026-10-01, iOS, issue #1006)

The user found the home screen jumpy on first open and asked for smooth
loading on every page, with very few transitions unless an effect was asked
for.

- **Launch.** `AppRootView` keeps the launch colour under the app and fades
  the themed app in over it (`FmMotion.normal`), instead of cutting from the
  colour to the first frame.
- **Home from the first frame.** On iPhone `HomeShell.page` starts at
  `.home`, so the first frame already shows Home with the header and the
  carousel on its Home cell. Before, the shell drew the last tab for a frame,
  then `start()` switched to Home and the carousel slid across. `start()` still
  decides: a restored route, a presented sheet or a tapped push clears Home
  as §2.0.7 says. The iPad starts with no page.
- **The offline strip waits.** The relay is `.offline` until it first dials,
  so the strip flashed in and out under the header on every launch and pushed
  the page down and up. The shell now shows it only after the relay has not
  been connected for `HomeShell.offlineGrace` (4 s) and is offline, keeps it
  through reconnect attempts (so it does not flap with the backoff), and
  hides it once connected. It fades in and out (none under Reduce Motion).
  The avatar dot and the Home relay chip still show the live state at once.
- **Next task keeps its size.** The card reserved the height of a typical
  suggestion so the quick links under it did not move when Gemma answered.
  Since the 2026-10-02 compact card shows only a title and a two-line status
  beside fixed-size buttons, the reserve is gone.
- **Loading pages fade, not cut.** The Agents tab cross-fades between the
  skeleton, the failure card, the empty state and the list (`FmSwap`). The
  Inbox shows skeleton rows instead of a spinner over the list and fades the
  first rows in (a thin bar since 2.0.87). Repos already fades its catalog in (`FmSliverFadeIn`).
- Tab switches and bar picks stay instant (§2.0.6). Pushes keep the system
  slide.

### 2.0.18 Watch agents is full screen (2026-10-01, iPhone, issue #1167)

The user asked for the Watch agents screen to run past the Dynamic Island
and under the bottom navigation, with only Liquid Glass over it, the events
on the right instead of underneath, the agents starting at the top so more
fit, and a sky that moves with the phone.

- **Edge to edge.** `AgentWatchScreen` takes the bare top (`bareShellTop()`,
  2.0.10), so the shell's header goes, and hides the navigation bar's
  background, so only the back button, title and glass buttons float over the
  sky. The content reads the safe area once (`GeometryReader` ignoring it)
  and every layer ignores it: the sky fills the screen, and the field and the
  events scroll under the status bar and the bottom bar, padded by the insets
  so their first and last rows still clear the glass. Pushed from the Agents
  tab, the bottom inset includes the bar (2.0.16).
- **Events on the right.** The list of sounds is a column at the right on
  every width, not a panel under the field: 36% of the width on a phone
  (132 to 170 pt), as before on an iPad. In a column narrower than 200 pt the
  rows stack their eye and speaker switches (top half the eye, bottom half
  the speaker) so the name and the latest line keep their room.
- **Agents from the top.** On a phone (`WatchFieldLayout.packsFor`, under
  700 pt) the rows are fixed at 150 pt from the top of the field with a 24 pt
  band header, instead of fractions of a field that is stretched to fit, which
  left a gap above the first row. The room under them is the desktop node's
  (96 pt), and `heightFor` adds up the same pixels. The side margin is 10 pt
  under 420 pt wide, so the field beside the column still seats two a row.
  The iPad keeps the desktop's spread rows.
- **The sky looks around.** `WatchTilt` reads Core Motion's gravity at 30 Hz,
  relative to a rest pose that follows the phone over about eight seconds,
  and `WatchSky.paint` takes it as `look`: the gradient clouds, aurora and
  light shafts move a little, each star more the bigger (nearer) it is, and
  the stars wrap at the edges so no side goes bare. Only the sky's own
  Canvas redraws on a tilt. It stops off screen, in the background and with
  Reduce Motion. Device motion needs no permission prompt.

### 2.0.19 The Agents tool row takes taps again (2026-10-01, iPhone)

The user reported that none of the chips under the top bar on Agents (sort,
list view, queue, usage, terminal, Watch, on-device chat, permissions) did
anything when tapped.

- 2.0.13 let tab roots reach into the top inset that the tab's
  `NavigationStack` keeps after a bare page (2.0.8, 2.0.10) is popped. That
  inset is not empty: the navigation bar the bare page showed can stay laid
  out over the root, invisible, and it takes the touches. With the inset
  ignored, the tool row sat under it.
- `HomeShell.tab` no longer ignores the top safe area. Instead every tab root
  that hides the bar (all but Inbox) carries `ShellRootBarHider`, a
  non-interactive UIKit view that finds its `UINavigationController` and calls
  `setNavigationBarHidden(true)` when the root is the top screen: on joining
  the window, on each layout (the stack resizing as the top bar returns) and
  at the end of a running push or pop. A cancelled swipe back leaves the page
  on top, so its bar stays. With the bar gone the leftover inset, and the
  band 2.0.13 was about, go too.
- Not checked on a device from Linux, which can only parse the Swift. If the
  band comes back without the bar, the inset is a stale status-bar inset and
  needs a fix that keeps the row out of it.

### 2.0.20 Every page runs under the glass bar (2026-10-01, iPhone)

The user asked for the Liquid Glass bar to sit in front of the content on
every screen, as §2.0.16 did for the agent list, without the bar covering a
page's own buttons.

- Every tab's `NavigationStack` and the pages the bar opens in place (Home,
  More, Report a bug) now take `barClearance` as bottom safe area
  (`safeAreaPadding`) instead of padding; `ShellBarClearance` is gone.
  Scroll views run under the glass and their last row still scrolls clear
  of it.
- A page's controls pinned to the bottom stay above the bar, because they
  are laid out inside the safe area: the Chat message field, the
  `safeAreaInset` add buttons (pulls, notes, tasks, vault, repository
  sections), the issue board's notice, the review bar and Report a bug's
  `FmBottomActionBar`. Only their backgrounds, which ignore the bottom safe
  area, reach under the glass.
- The height is still zero while a page's field has the keyboard up or on
  iPad, so nothing moves there.

### 2.0.21 Pages under the glass scroll clear of it with a margin (2026-10-01, iPhone, issue #1172)

The user asked for every page with the Liquid Glass buttons over it to scroll
up all the way past them, leaving a margin at the bottom.

- `HomeShell.barClearance`, the bottom safe area every page takes (§2.0.20),
  is now the bar's height plus `HomeShell.barMargin` (32 pt), so a page's
  last row stops a margin above the new agent button instead of touching it.
  It is still zero while a page's field has the keyboard up or on iPad.
- A page's pinned bottom controls (the Chat message field, the add buttons,
  the review bar) sit the same margin above the bar; their backgrounds still
  reach under the glass.

### 2.0.22 The band under the top bar, again (2026-10-01, iPhone, issue #1177)

The user sent a screenshot of the Agents tab with an empty raised band, about
the height of the status bar, between the top bar and the tool row.

- It is the stale status-bar inset 2.0.13 was about. 2.0.19 dropped that
  fix for `ShellRootBarHider`, expecting the inset to leave with the hidden
  navigation bar; it does not, as 2.0.19's last note warned.
- `HomeShell.tab` again makes each tab root (all but Inbox) ignore the top
  container safe area while the top bar shows on that tab, and keeps
  `ShellRootBarHider` beside it. The hider takes the leftover navigation bar
  away, so the tool row that now sits flush under the top bar is not under
  an invisible bar and still takes taps.
- Not checked on a device from Linux.

### 2.0.23 Inbox filters sit under the top bar, on glass (2026-10-01, iPhone, issue #1179)

The user sent a screenshot of the Inbox tab, where the filter chips sat below
an options button row and a large "Inbox" title, inside a raised band, and
asked for them more compact, higher up and on Liquid Glass.

- The Inbox tab root no longer keeps its own navigation bar: `HomeShell.tab`
  treats it like the other tabs (`ShellRootBarHider`, the top safe area
  ignored while the top bar shows, the bar hidden), so the chips sit flush
  under the top bar. The selected tab already says Inbox.
- The options menu (`InboxToolbarMenu`) moved from the navigation bar to the
  end of the chip row, as a 32 pt glass circle; paused pings still show
  `bell.slash`, now in the warning colour.
- The chips are 30 pt glass capsules in a `GlassEffectContainer`, with the
  selected one tinted with the accent and its count in the accent; the row
  has no list-row background. Before iOS 26 they keep the old flat fills.
- The list uses compact section spacing and a 4 pt top margin.
- An Inbox pushed onto another stack keeps its navigation bar, now with an
  inline title.
- Not checked on a device from Linux, which can only parse the Swift.

### 2.0.24 The Agents tool row takes taps, without the band (2026-10-01, iPhone, issue #1181)

The user sent a screenshot of the Agents tab, with no band now, and reported
that the chips under the top bar did nothing again.

- Every time a tab root reached into the stale top inset (2.0.13, 2.0.22) the
  row stopped taking taps; every time it stayed out of it (2.0.19) the band
  came back. Whatever UIKit keeps over that inset takes the touches, whether
  or not the navigation bar is hidden, so the root may not ignore it.
- `HomeShell.tab` no longer ignores the top safe area on any tab root,
  Inbox included (2.0.23 had it ignore it there too). `ShellRootBarHider`
  now removes the stale inset instead: it finds the root's own view
  controller in the stack and, while the bar is hidden, sets a negative
  `additionalSafeAreaInsets.top` that brings the root's top inset down to
  what the stack's place in the window gives (the window's top inset less
  how far down the stack starts, so zero under the top bar and the status
  bar while a bare page is being popped). It reruns on layout, on a safe
  area change and after a push or pop, and clears the correction once the
  inset is right or the bar shows.
- If UIKit's own inset is right and only SwiftUI's is stale, the correction
  is zero and the band can come back, but the row still takes taps.
- Not checked on a device from Linux, which only parses the Swift.

### 2.0.25 An agent's prompt sits on the bar, the new agent button above it (2026-10-01, iPhone)

The user sent a screenshot of an agent's run page with its follow-up
composer under the Liquid Glass bar (the model chips hidden behind the
capsule) and the new agent button over the transcript, and asked for the
prompt area to move above the bar and the button above the prompt area.

- The composer stayed at the page's own bottom edge: the bar clearance the
  tab's `NavigationStack` takes as bottom safe area (§2.0.20) did not reach
  that pushed page, and had it reached it the composer would have sat above
  the new agent button instead of between the two.
- `ShellDock` (`Shell/ShellDock.swift`) docks a page's bottom control. A page
  wraps itself in `.shellDock { composer }`; inside the shell the page then
  ignores the bottom container safe area and ends `ShellDock.bottom` above
  the bar's bottom edge: the capsule and version line under it
  (`HomeShell.capsuleTop`, measured in the bar's coordinate space) plus
  `ShellDock.gap`. That holds whether or not the stack's safe area reaches
  the page, and the page's background still runs under the glass.
- Each tab gets its own `ShellDock` through the environment; the composer
  reports its height there while it is on screen. `HomeShell.dockLift`, the
  selected tab's docked height, widens the gap between the new agent button
  and the capsule so the button sits a gap above the composer.
- While a page's field has the keyboard or on iPad `ShellDock.bottom` is
  zero, so the composer sits on the keyboard or the page's bottom as before.
- Docked pages: the agent run page (`AgentRunScreen`), the phone's repository
  agent (`AgentScreen`) and a launched session (`LaunchedAgentSessionScreen`).
- Not checked on a device from Linux.

### 2.0.26 Pushed pages scroll clear of the new agent button (2026-10-01, iPhone, issue #1191)

The user asked for the Pedometer to scroll up past the floating new agent
button with a margin under it, and for every other page the button would
otherwise cover to do the same.

- §2.0.25 found that the bar clearance the tab's `NavigationStack` took as
  bottom safe area (§2.0.20, §2.0.21) did not reach pushed pages. Pedometer,
  Notes, Tasks, Vault and every other page opened from a tab are pushed, so
  their last rows and `safeAreaInset` add buttons stayed under the button.
- The clearance now travels with the tab's `ShellDock`, which the
  environment already carries into pushed pages: `HomeShell` copies
  `barClearance` into `ShellDock.clearance` for every tab, and
  `shellPageClearance()` (`Shell/ShellDock.swift`) adds it as bottom safe
  area (`safeAreaPadding`) to each tab's root and to every
  `navigationDestination` page. The stack itself no longer takes it, so no
  page counts it twice.
- Scroll views still run under the glass, and their last row stops
  `HomeShell.barMargin` (32 pt) above the new agent button. A page's pinned
  bottom controls sit the same margin above it. Docked pages (§2.0.25)
  ignore the bottom container safe area and keep their own placement.
- The clearance is zero while a page's field has the keyboard, on iPad and
  in a presented sheet (no `ShellDock` there). Home, More and Report a bug
  keep their own `safeAreaPadding` in `HomeShell.tabs`.
- Not checked on a device from Linux.

### 2.0.27 Desktop, core node and runner lights beside the version (2026-10-01, iOS)

The user asked for the desktop icon to move from the header to the right of
the version number, green while the desktop is connected and red when it is
not. They also asked for every core node, green when it is up and
operational and red (blinking) on any issue, and every connected runner,
green when it has a valid signed-in session and red when it does not.

- The desktop button left the header (`ShellTopBar`). `ShellVersionFooter`
  now draws `ShellFooterDesktopLight` right of the version: the
  `desktopcomputer` glyph, `fm.success` only for `ShellLocalDesktop.connected`,
  `fm.danger` for every other state, including no desktop in reach
  (`.hidden`, labelled "No desktop connected"). A tap opens Desktops, as the
  header button did.
- After it, `ShellFleetLights` draws one glyph per `core:<name>` system
  (`server.rack`) and then one per `runner:<name>` system (`cpu`) from
  `/api/status`, the public status report the Status page reads. A node is
  green only when its state is `operational`; degraded, down or no data is
  red and blinks (no blinking with Reduce Motion). The server's core verdict
  covers the cluster heartbeat, the cluster status, the peers' HTTPS
  readiness votes and the intrusion detector. Its runner verdict lists the
  active pool runners and is green only when the runner has polled within
  three minutes and `runner_auth.status_reason` finds no expired or missing
  Claude/Codex login. A tap opens the Status page, which gives each reason.
  VoiceOver reads every node with its state.
- `FleetLightsModel` (`FMInsights/FleetLights.swift`) loads `/api/status`
  when the shell starts and again when the app becomes active. There is no
  timer (no display polling): a reload waits at least 15 minutes after an
  all-green read and 2 minutes while any node is red. A failed read keeps the
  last lights. With no nodes in the report, nothing is drawn after the
  desktop light.
- Not checked on a device from Linux.

### 2.0.28 Watch agents: activity drops down, words float off the agents, sound follows the phone (2026-10-01, iPhone)

The user sent a screenshot of Watch agents with the list of sounds as a column
on the right and asked for: that column in a drop-down; no "Watch agents"
title and nothing but the glass buttons over the top; each new activity shown
as its icon and one word on the agent, fading out; the busiest agents at the
top; any other information about the agents, such as network activity; a
sound that follows the accelerometer; and the activity to keep streaming
while the screen is open.

- **Drop-down.** The list of sounds is no longer a column (2.0.18). A
  toolbar button (`agent-watch-sounds-toggle`) drops it from the top right
  as a glass panel, 62% of the height and up to 360 pt wide; the field keeps
  the full width. A tap on the field, or the button again, puts it away. It
  starts closed on every visit, so the `agentWatch/sounds` preference is
  gone. While it is open the frames tick once a second, so its figures stay
  current on a quiet field.
- **Bare top.** The navigation title is empty and on iOS 26 every scroll
  view on the screen hides its scroll edge effect (`watchEdgesClear`), which
  was the dark band behind the bar. Only the back button and the glass
  button group stay over the sky.
- **The word for each activity.** `WatchedAgent.tag` and `tagSeconds` hold
  the kind of its latest mark that is not bookkeeping, set when the mark
  lands. `WatchFieldPainter.paintTags` draws its icon and label in a pill
  above the body's ring of words, rising 10 pt and fading out over
  `kTagSeconds` (2.4 s). Kinds hidden with the eye switch are not drawn.
- **Busiest first.** Every `kRankSeconds` (4 s) `AgentWatchController`
  re-ranks its agents by their marks, each weighing `exp(-age / 30 s)`, plus
  0.6 for the place an agent already holds, so two equally busy agents do
  not keep swapping. `WatchFieldLayout` seats each band by that rank (bands
  stay working, waiting, resting), and `glide` eases each body to its new
  seat over about 0.35 s instead of jumping.
- **Network and activity.** The controller counts the text each read brings
  in (`bytesIn`, `bytesPerSecond` over 10 s, `readsDone`,
  `lastReadSeconds`) and the marks of the last minute (`eventsPerMinute`).
  `ControlWatchFeeds` counts the desktop stream's bytes and deltas and how
  many of its streams are live. The desktop node at the foot of the field
  shows the rate and the live streams, glowing as a read lands. The
  drop-down opens with a summary: working, waiting and idle counts, the rate,
  the streams, the bytes in, events per minute, when the last read landed
  and the busiest agent with what it is doing.
- **Sound that follows the phone.** `WatchMotionSound` is a soft drone on
  its own `AVAudioEngine` source node, in the agents' key (`kTonicHz` an
  octave down, `kScale` over two octaves). Tilting the phone left and right
  (gravity x) steps it along the scale with a glide and pans it; holding the
  phone upright rather than flat opens its filter; moving it (Core Motion's
  user acceleration) swells its volume and adds an octave shimmer. Still, it
  is a faint hum. It plays only while "Hear everything" is on, the menu's
  "Sound follows motion" (`agentWatch/motionSound`, on by default) is on and
  the app is in front. `WatchTilt` now hands every reading on, and runs under
  Reduce Motion when the sound needs it, with the sky held still.
- **Streaming stays live.** `AgentWatchController.ring()` restarted its
  debounce on every ring, so a desktop stream that delivered deltas more
  often than the debounce (0.4 s) never let a read through: the field went
  quiet exactly while an agent was busiest. A ring now only starts the timer
  when none is pending, so a steady stream is read at least once per
  debounce. `pause()` drops a pending read; `resume()` rings again.
  `ControlWatchFeeds` opened at most six streams and never more: a stream
  that ends (`unknown_session`) now gives its slot to the next row waiting
  for one.
- Not checked on a device from Linux; the FMAgentWatch tests and
  `ControlWatchFeedsTests` cover the controller, layout and feed changes.

### 2.0.29 Shortcuts moved to the navigator (2026-10-01)

The user asked for Shortcuts to show up in the navigator and to come off
the repository screen.

- Shortcuts is a page of its own, `.shortcuts(RepoRef?)` (title
  "Shortcuts", symbol `command`, WS17), listed in the hub's Operate group
  after Special Agents. Through the hub catalog it reaches the iPhone carousel, the
  More grid, the avatar hub and the iPad sidebar, with the bar's
  recent-first and most-used sorting like any other hub page.
- With no repository in the route the page starts on the repository in use
  (`CodeCurrentRepo.memory`, the Code button's); the page's repository
  picker switches it. With neither, the desktop answers with its app-wide
  skills and repository list, and the page picks the first working tree.
- The repository grid no longer has a Shortcuts tile (`RepoSection` lost
  `shortcuts`). A link to `/<owner>/<repo>/shortcuts` opens the new page on
  that repository, on the current tab. A saved navigation stack that still
  holds the old section fails to decode and starts fresh, as any unknown
  route does.
- Android does the same: `Route.Shortcuts(repo)` (`keyboard_command_key`,
  WS17) after Special Agents in Operate, so it is in the More grid and the hub;
  with no repository it opens on `CodeFeature.currentRepository()`. The
  repository grid dropped its Shortcuts tile and the same link opens the
  page.

### 2.0.30 A docked prompt clears the bar on a home-indicator iPhone (2026-10-01, iPhone)

The user sent a screenshot of an agent's run page with the bottom of the
follow-up composer (§2.0.25) still under the glass capsule, and asked for
the prompt area to move up a bit so the two no longer overlap.

- `ShellDock.bottom` is measured from the bar's bottom edge, but the bar
  sits on the bottom safe area while a docked page ignores it and ends at
  the screen's bottom. On an iPhone with a home indicator the composer
  therefore sat the 34 pt indicator inset lower than intended, about 22 pt
  into the capsule instead of `ShellDock.gap` above it.
- `HomeShell` now copies the bar's bottom edge in global coordinates into
  `ShellDock.barBottom`, and `ShellDocked` measures its own global bottom
  edge and adds whatever of it lies below the bar's to the padding. A page
  that does not reach under the bar gets no extra padding.
- The new agent button already sits `ShellDock.gap` above where the
  composer should be, so it keeps its place and the composer now fits
  between it and the capsule.
- Not checked on a device from Linux.

### 2.0.31 A Fix shortcut beside Send in agent prompts (2026-10-02, iOS and Android)

The user asked for a shortcut button in the prompt area, "Fix", that sends
whatever is typed with a "please fix" ask in one tap.

- Every agent composer draws a **Fix** pill (wrench icon) just left of Send:
  the shell's new-agent composer, the on-device agent, a session's
  follow-up and a run page's prompt on iOS (`PromptComposer(offersFix:)`),
  and the shell, Quick start and every `AgentPromptComposer` on Android
  (`PromptComposer(offersFix = true)`). Chat, the shell's page targets
  (calories and the like) and comment boxes do not show it: "please fix"
  means nothing to them.
- A tap rewrites the field to `Please fix: <typed text>` and sends it the
  same way Send would, so the run, steering and the launch sparkle behave
  as before. With only photos attached it sends `Please fix` alone; with
  nothing attached or typed it is dimmed and does nothing, like Send.
- The wording lives in one rule per platform so the two apps stay alike:
  `FMAgents/PromptFix.swift` and `ComposerRules.fixPrompt`
  (`C/composer/ComposerText.kt`), each with a unit test.
- The field shows the rewritten text until the send clears it, so a failed
  send leaves the full prompt in place to retry.
- Verified: `./gradlew :core:coreTest -Pfilter=ComposerText` (6 passed) and
  `:app:compileDebugKotlin`. The iOS side was not built or run on a device
  from Linux.

### 2.0.31a The prompt row no longer pushes the page sideways while listening (2026-10-02, iOS)

On a desktop agent's run page, dictating into the composer slid the whole
screen left: the title lost its first letters, the trash button and Send were
cut off. The controls row holds the attach button, the mode chip, the model
chip, the red Listening chip, Fix and Send; Listening, Fix and the mode chip
were all `fixedSize`, so with Listening showing their sum outgrew an iPhone's
width and SwiftUI widened the page to fit.

- Fix is hidden while dictation runs; it comes back when Listening stops.
- Fix is a `ViewThatFits`: the labelled pill when there is room, a round
  icon-only button when there is not. The model chip still gives up width
  first (`layoutPriority(-1)`).
- Listening is always the round red waveform button with no word, at the
  user's request; VoiceOver still reads "Listening. Tap to stop dictation".
- Not built or checked on a device from Linux.

### 2.0.32 Counts on every Navigator tile; drag tiles to rearrange (2026-10-01, iPhone)

The user asked for the Navigator (the More page) to show the counts that go
with each tile, and for every tile to be movable by a long press, including
moving the repository's Pull requests or Issues button onto it.

- **Counts.** A tile with something waiting keeps its red badge (accent for
  running timers), now with the full number instead of `99+`. Any other tile
  with a count shows it in a grey pill in the same corner
  (`ShellBarItem.count`, drawn by `ShellBarCellLabel(showsCounts:)`; the
  bottom bar keeps only the red badges). Sources: Repos the network catalog,
  Notes all notes, Tasks open tasks, Calendar today's events, Organizations
  your organizations, Ledger your own ledger records
  (`ShellNavigatorCounts`, fetched at launch and each time the Navigator opens,
  at most once a minute, cleared on a project switch); Desktops the paired
  desktops, Special Agents the teammates once loaded, Market the installed packages,
  Vault the items while unlocked, On-device AI the downloaded models, Network
  & nodes the people online, Hosts & fleet the runners (red: runners needing
  attention), Status the fleet lights (red: unhealthy ones). Calories, Steps,
  Water and Sleep measure rather than count, so they show none; neither do
  Coves (per repository), CRM and Marketing (one request per organization).
  All of these got counts in §2.0.35.
- **Repository tiles.** With a current repository, a section titled with its
  name follows Go to: Pull requests, Issues, Commits, Branches, Releases,
  Actions and Discussions, each opening that section of the repository on
  the Code tab, with the open pull request and issue counts and the commit,
  branch and release counts. The counts come from the repository page when it
  loads (`ShellCounts.adopt`) or are fetched when the Navigator opens. These
  tiles are bar items like any other, so they also sit in the carousel.
- **Rearranging.** A long press on a tile opens its menu as before; dragging
  instead lifts the tile, and the grid moves around it live: over a tile it
  takes that tile's place, over a section heading it goes to the end of that
  section, so a tile can move to any section (Pull requests into Go to, for
  example). The bar's usual order follows the Navigator's. **Reset Navigator
  order** in the Navigator settings puts everything back. This replaces the
  fixed grid order of §2.0.4 and §2.0.14.
- **Storage.** `ShellMenuLayout` in FMApi (tested in `ShellMenuLayoutTests`)
  saves each section's item ids under `shell/menu-layout`. An item whose saved
  section is gone, or that has no saved place, shows in its own section, new
  items at the end. An item that is absent for now (no current repository, a
  package not loaded yet) keeps its place for when it comes back.

### 2.0.33 An Actions run scrolls clear of the bar (2026-10-02, iPhone)

The user sent a screenshot of an Actions run (Run #628) scrolled to the end,
with its log and Share log under the Liquid Glass bar and the version line,
and asked for the bottom margin.

- `shellPageClearance()` (§2.0.26) is added by `navigationDestination`, so it
  reaches route pages only. A page pushed by a `NavigationLink` with a
  destination closure gets no bar clearance, and the run page is opened that
  way from the Actions list (`RepoActionsList`) and a pull's checks
  (`PullChecksList`).
- Both links now add `.shellPageClearance()` to `ActionRunScreen`, so its
  last row stops `HomeShell.barMargin` above the new agent button like the
  other pages. The route form (`ActionRunDestination`) already got it.
- Other closure-pushed pages still have the same gap; each needs the same
  modifier on its link's destination.
- Not checked on a device from Linux.

### 2.0.34 A desktop terminal's keys and input line clear the bar (2026-10-02, iPhone)

The user sent a screenshot of Claude Code running in a desktop terminal
(`DesktopTerminalScreen`) with its key row and its "Type or paste, then
send" line under the Liquid Glass bar and the version line, and asked for
the bottom margin.

- This is the gap §2.0.33 left open: the terminal page is pushed by a
  closure, so it never got the bar clearance. It is opened from the Agents
  tab's terminal button (`AgentsScreen`), and from an agent's run page by the
  Terminal button and by Open terminal (`AgentRunScreen`).
- All three destinations now add `.shellPageClearance()`, so the key row
  and input line sit `HomeShell.barMargin` above the new agent button and
  the terminal grid ends above them.
- The terminal opened from the accounts sheet (`AgentAccountsSheet`) is in
  a sheet over the bar and the sign-in terminal (`AgentLoginTerminalScreen`)
  in a full-screen cover, so neither changed.
- Not checked on a device from Linux.

### 2.0.35 A more compact Navigator with a count on every tile (2026-10-02, iPhone)

The user sent a screenshot of the Navigator (the More page) and asked for it
to be more compact so more fits on the screen, and for counts on everything.

- **Compact.** Tiles are 36 pt tall (from 46) with 15 pt icons (from 17) and
  1 pt between icon and label; labels may shrink to 75%. Section headers sit
  4 pt below the tiles above them (from 8), the grid's side margin is 8 pt
  (from 12), and the Navigator title row is 32 pt tall with no top padding.
  Still five across, so labels such as "Network & nodes" stay readable
  (six across since §2.0.36).
- **A known zero shows.** `ShellBarItem.count` is now optional: `nil` means
  the number is not known (no pill), any known number shows, `0` included.
  `ShellNavigatorCounts.count` and `ShellCounts.repoCount` return `nil` until
  their source has answered.
- **New sources** (on top of §2.0.32's): Home the running agents, Agents
  every agent in its list, Inbox the notifications held, Chat the people
  online, Code the current repository's open pull requests, Search the saved
  searches, Timers every timer (red/accent badge still for running ones),
  Shortcuts the desktop's shortcuts (fetched once if a desktop is paired),
  Coves the current repository's coves (loaded once when the Navigator
  opens), Activity the relay log's events, Tour its pages, Stats the tiles
  on your board, Admin the users (admins only), Calories today's kcal, Steps
  today's steps (server total, no HealthKit sync from here), Water today's
  ml, Sleep last night's hours, CRM the contacts, Marketing the content
  items and Analytics the projects across your first five organizations with
  that package installed. The repository section adds Actions (runs) and
  Discussions; `ShellCounts.adopt` merges into the same repository's counts
  instead of replacing them, so the repository page's refresh keeps them.
- The health tiles reuse their page models with sync and reconcile off, so
  opening the Navigator writes nothing. Packages that are not installed
  answer `plugin_not_installed` and show no pill.
- Still without a number: Settings, Bug and World (an external link), which
  have nothing to count.
- Not checked on a device from Linux.

### 2.0.36 Six across in the Navigator, like the bar (2026-10-02, iPhone)

The user sent a screenshot of the Navigator and asked for it to be more
compact, six across like the bottom bar (five items and More).

- `ShellMenuPage.across` is 6 (from 5). On a 393 pt wide iPhone a tile is
  about 59 pt wide, close to the bar's 64 pt cells.
- Tile labels may shrink to 70% (`ShellBarCellLabel.minimumScale`, still
  75% in the bar), so "Network & nodes", the longest label, fits in one
  line instead of being cut.
- Not checked on a device from Linux.

### 2.0.37 An agent opens from the list faster (2026-10-02, iPhone)

The user asked for the move from the agent list to an agent's page to be
faster.

- **Pages are built when opened.** Each row's link built its destination
  page (`AgentRunScreen`, `AgentSessionDetailScreen`,
  `LaunchedAgentSessionScreen`) and that page's model every time the list
  redrew, for every row, and the list redraws on each running agent's
  update. The links now hold `AgentLazyPage`, which builds the page only
  when it is pushed. The "All agents" list does the same.
- **The list stops after the slide.** Pushing a page makes the list
  disappear, and `AgentsScreen` used to stop its model, the eagle feed and
  the agent dots right then. Dropping the dots redrew every row on the frame
  the push started. The stop now waits `AgentsScreen.leaveDelay` (0.5 s,
  past the push), and coming back within it cancels the stop.
- Not checked on a device from Linux.

### 2.0.38 Back to the agent list after a merge, at once (2026-10-02, iOS)

The user asked for the way back to the agent list after Merge & clean up to
be fast and smooth, without transitions: the top bar jumped in after the
list had already slid into place.

- Back on the "Merged" page used to pop the diff page, show the agent run
  page, wait 350 ms and pop that too: two slides, and the bare top (2.0.8)
  was released only as each page disappeared, after its slide, so the top
  bar, telemetry strip and agent dots dropped in and pushed the list down.
- `AgentDiffScreen` and `RunnerDiffScreen` take an `onLeave`. The run page
  passes its own leave, and the Merged page's Back calls it instead of
  popping itself. `ShellChrome.leave(_:)` clears the selected tab's bare
  count and calls the run page's `dismiss`, which pops the run page and the
  diff page above it, both in one transaction with animations disabled. The
  list and the top bar appear together in one frame, with no slide.
  `ShellChrome.show` stops at zero, so the pages' own releases on disappear
  change nothing after that. Only the top page of a stack holds the bare
  top, since a covered page releases it (2.0.8), so clearing the tab's count
  touches no other page.
- The other ways from the run page back to the list use the same leave:
  closing the runner Clean up sheet after a merge, Delete, and Continue on
  desktop when the runner run is deleted.
- Not checked on a device from Linux.

### 2.0.39 Back returns to the Navigator (2026-10-02, iPhone)

The user asked for Back to respect the navigation menu: Back always goes
back to where you were, even when that was the Navigator (the More grid,
`ShellPage.menu`).

- A pick from the grid closed it (`page = nil`) and then pushed the page on
  the current tab's stack, so Back popped to whatever the grid had covered.
- Every bar and grid pick now runs through `HomeShell.leavePage`, which
  notes the page it left (the grid or Home; not the Bug form, whose draft
  would be lost) and where the pick landed: the tab and its `[Route]` path,
  as a `PageReturn` (`FMApi/Navigation/PageReturn.swift`). Home's stat
  buttons and the grid's package tiles go through it too.
- When the tab's path is popped below that landing (the Back button, the
  swipe back, the Back history menu, ⌘[), `returnToPage` shows the grid (or
  Home) again over the tab, with the keyboard down. Pushing further keeps the
  return, so backing out of a deeper page first lands on the picked page,
  then on the grid. A pick of a repository section returns to the grid when
  popped to the repository.
- The return is dropped by a switch to another tab, a path that no longer
  starts with the landing, a deep link or notification (`router.landings`),
  and a bar pick made with no page open. A pick that pushes nothing (a tab's
  root, a modal route, a guest pick waiting for sign in) keeps none.
- Pages pushed with a closure `NavigationLink` are not in the path (2.0.15),
  so popping them does not bring the grid back; only popping the picked
  route does.
- Not checked on a device from Linux; `PageReturnTests` cover the rule.

### 2.0.40 One usage chip, no permissions chip, chips that look like buttons (2026-10-02, iPhone)

The user asked for the Agents tool row's usage to show only the active
model's session usage, for the row's far-right button to go, and for the
row to look more like buttons. The runner run that started it (session
`6de0979d…`, branch `agent/6-only-show-the-active-model`) hit its weekly
limit before pushing anything; the work was redone on the desktop.

- **Usage.** `AgentUsageBars` drew a bar for every window of every provider
  (Claude Code 5-hour, weekly and Fable weekly, then each Codex window). It
  now draws one bar and its percentage: the first window (the session
  window) of the provider new agents use. The desktop names that provider in
  `agentops.usage` as `activeProvider` (its `defaultAgentProvider()`);
  `UsageSummary.activeSession` picks it, or the first provider with windows
  when the field is missing (an older desktop) or that provider has no
  meter. Before the first reading the chip shows a gauge, so the accounts
  sheet stays one tap away. The sheet still lists every window.
- **Saved permissions.** The far-right chip (`account_tree_outlined`, Saved
  permissions) is gone from the row; the Desktop agents screen
  (`AgentOpsHubScreen`) still links to `CapabilityGrantsScreen`. The first
  version of this change removed the terminal chip instead, from "the icon
  on the right" of the usage chip; the user meant the far-right button and
  asked to keep the terminal.
- **Buttons.** `AgentToolChip` is a filled rounded rectangle (`bgOverlay`,
  `FmRadius.md`, a hairline border), the Navigator tile look, instead of a
  `bgBase` capsule that read as a hole in the bar. The row's buttons use
  `ShellBarButtonStyle`, so a tap dims and shrinks the chip.
- Not compiled or checked on a device from Linux.

### 2.0.41 A search field at the top of the Navigator (2026-10-02, iPhone)

The user sent a screenshot of the Navigator (the More page) and asked for a
search at the top of it.

- A search field sits under the Navigator title, above the first section.
  Typing filters the tiles (`ShellMenuPage.filtered`): a tile stays when its
  title contains the text, a whole section stays when its title does (so
  "business" keeps Market, CRM and the rest), and sections left with no tile
  are hidden. Matching ignores case and accents.
- Return opens the first tile left; with no tile left it opens the global
  search with the text. Below the tiles, **Search everywhere for "…"**
  always opens the global search (`HomeShell.openSearch`), leaving the
  Navigator through `HomeShell.leavePage` as a tile does, so Back from the
  search results returns to the Navigator (2.0.39), with the field empty.
- The field clears with its x button and when the Navigator is left, since
  the page is rebuilt each time it opens. Dragging the list hides the
  keyboard. Drag to rearrange still works on the filtered tiles; the move is
  worked out against the full list.
- Not checked on a device from Linux.

### 2.0.42 An activity ring on every icon with something running (2026-10-02, iPhone)

The user sent a screenshot of the Navigator and asked for every icon that has
something active on it, such as an agent or a shortcut, to show a nice
activity indicator.

- `ShellBarItem.active` marks an item with live work. `ShellBarCellLabel`
  then tints the glyph with the accent and draws `ShellActivityHalo` behind
  it: a soft accent glow that breathes every 2.4 s under a comet arc (70% of
  a circle, fading at its tail) that turns once every 1.6 s. One label serves
  the bar's carousel, its folder pop-over, the Navigator grid and the Home
  links, so the ring shows in all of them. A bar folder
  (`ShellBarFolderLabel`) wears the ring when any member is active.
- The ring is drawn from a `TimelineView` capped at 30 frames a second, not a
  repeating animation, so it keeps turning when a page opens with animations
  off (2.0.6). With Reduce Motion the timeline pauses and the ring is a still,
  full accent circle.
- What counts as active: **Agents** while any agent runs
  (`ShellCounts.agents`), **Timers** while a timer runs (`ShellCounts.timers`),
  **Shortcuts** while a shortcut runs on the desktop (`ShortcutsStore.anyRunning`
  or a catalog entry's `running`), **Brains** while a teammate's status is
  Working, and **On-device AI** while a model downloads
  (`LocalModelManager.downloadProgress`). `ShellNavigatorCounts.active` holds
  the list; it reads state already loaded, with no fetch or timer.
- VoiceOver reads "Active" before the item's count.
- Android has no ring yet. Not checked on a device from Linux.

### 2.0.43 Status dots under the version, from the Status Page package (2026-10-02, iOS)

The user asked for the status page to become a package, on by default, and,
while it is on, for the desktop debug bar's status dots to show under the
version number at the foot of the phone.

- `marketplace/packages/status-page` (bundled, `install.default: true`,
  `dataLifecycle.scope: none`) carries no code. The desktop and the phone gate
  their built-in status lights on it.
- `ShellVersionFooter` is now two rows: the version row of 2.0.27, and under
  it `ShellStatusDots`, one 7pt circle per system in `/api/status` other than
  `core:`, `runner:` and `mirror:` systems and `future` ones (website, status
  page, API, errors, database, email and so on, in the server's order).
  The colours follow the desktop: `fm.success` for operational, `fm.warning`
  for degraded, `fm.danger` for down and `fm.textTertiary` for no data.
  Degraded and down dots blink, except with Reduce Motion. A tap opens the
  Status page, and VoiceOver reads each system with its state.
- `FleetLightsModel` keeps these as `systems` from the same `/api/status`
  read; no new request is made. A degraded or down system brings the next
  reload forward to the 2-minute floor, as a red node does.
- The package has no account data, so the phone keeps its on/off per device
  under `packages/status-page/on` (`SettingsService.statusPageOn`, default
  on). The Market page for Status Page shows an "Installed on this device"
  switch (`PhonePackages`), and the Market's Installed filter follows it.
  Turned off, the phone drops the dots, the core node and runner lights and
  the Status entry in the Navigator, and stops reading `/api/status` from the
  shell. Long-pressing a Status bar item offers the package's settings
  (`nativeDestinations["status-page"] = "status"`).
- Android has no version footer yet, so it has no dots.
- Not checked on a device from Linux.

### 2.0.44 Home is the icon grid; no More button (2026-10-02, iPhone)

The user asked for Home to keep only the icons, with the counts the Navigator
tiles carried and a very thin search on top, and for the three-dot More button
to leave the bottom bar.

- Home (`ShellHome`, `ShellPage.home`) is now the Navigator's grid: six
  tiles across in their sections, each with its count or badge
  (`ShellBarCellLabel` with `showsCounts`), the activity ring (2.0.42), the
  hidden-from-bar mark, the long-press menu and drag to rearrange. The Home
  item itself is left out of the grid (`ShellHome.linkGroups`). Opening Home
  loads the repository's counts and refreshes `ShellNavigatorCounts`, as the
  Navigator did.
- Gone from Home: the hero (greeting, radar, relay, uptime and repository
  chips), the stat tiles (Agents, Pings, memory, requests), the Gemma
  **Next task** card (`ShellHomeNextTask.swift` deleted) and the dotted
  backdrop. `HomeDashboard` and `NextTaskAdvisor` stay in ForkMeshCore with
  their tests but nothing in the app reads them now.
- The search is a 26 pt capsule at the top of Home (`shell-home-search`),
  filtering tiles as 2.0.41 did, with **Search everywhere** under the tiles.
  When the field is empty its right end holds a small sliders menu with the
  bar settings (sort, clear usage, show hidden, reset bar, reset Home order),
  which used to sit on the Navigator's gear and a long press on More.
- The bottom bar has no More cell and no divider: the capsule is the
  carousel alone. `ShellPage.menu`, `ShellMenuPage` and
  `HomeShell.toggleMenu` are removed. Items hidden from the bar are reached
  from Home, and when Agents is hidden the launch flight lands on the Home
  cell (`ShellBottomBar.landingItems`) instead of More.
- Android keeps its More button. Not checked on a device from Linux.

### 2.0.45 The bar as low as it goes, one footer line (2026-10-02, iPhone)

The user sent a screenshot of the bar with the version, desktop and fleet
lights on one row and the status dots on a second row under it, and asked
for the bar to sit as low as it can, with the dots to the right of the
other lights so everything is on one line at the very bottom.

- `ShellVersionFooter` puts the status dots (2.0.43) at the end of the
  version row. A `ViewThatFits` falls back to the old two rows only when the
  line does not fit, such as with the Update capsule showing or in the iPad
  sidebar.
- `HomeShell.barStack` ignores the bottom safe area, so the stack sits on
  the screen's bottom edge instead of above the home indicator. The footer
  is lifted by `footerLift`: the bottom inset less
  `HomeShell.indicatorReach` (24 pt), at least 4 pt, which keeps its lights
  just above the indicator (10 pt on a 34 pt inset). On an iPhone with a
  home indicator the capsule moves down about 48 pt.
- The shell reads the bottom inset (`safeBottom`) from the tabs area, and
  `barClearance` subtracts it, because pages already take that inset as
  safe area and the bar now covers it. `ShellDock.barBottom` is now the
  screen's bottom, so the 2.0.30 correction adds nothing and the docked
  prompt still sits `ShellDock.gap` above the capsule.
- Not checked on a device from Linux.

### 2.0.46 Six icons in the bar, moved in and out from Home (2026-10-02, iPhone)

The user asked to long press the icons on Home to move them in and out of the
bottom bar, and for the bar to hold only six icons.

- **Six slots.** The bar shows at most `ShellBarLayout.barLimit` (6) slots; a
  folder is one slot. Home is always one of them (`ShellBarPrefs.keep`), so the
  grid stays reachable. Cells shrink to fit the capsule (`slotWidth`, 44 to
  64 pt) so all six show without scrolling. With nothing chosen yet the bar
  holds the first six in the usual, recent or frequent order.
- **Long press on a Home icon** (`ShellBarItemMenu`) opens with **Add to bar**,
  or **Remove from bar** when it is in the bar. When the bar is full, **Add to
  bar in place of** lists the bar's icons (not Home) and swaps the picked one
  out. The same menu on a bar icon offers **Remove from bar**; a folder's menu
  offers **Remove folder from bar**. Icons in the bar carry a small accent
  `dock.rectangle` mark on Home (VoiceOver: "In the bar"), replacing the
  `eye.slash` mark for hidden ones.
- **Drag.** Dragging a Home icon onto a bar icon puts it in that place (and,
  when the bar is full, takes that icon out); dropping it elsewhere on the
  capsule adds it if there is room. Bar icons can be dragged along the bar to
  reorder them, or onto Home to take them out. A placing drop switches Sort to
  Usual order, as Move does. The drag lives in `ShellBarPrefs.drag`, shared by
  the grid and the bar.
- **Storage.** `ShellBarLayout.bar` holds the ids allowed in the bar once the
  user edits it (`fit` writes the six on screen first, so removing one never
  pulls the seventh in); nil is the first-six default. Items outside it read
  as hidden, so folders and ordering work unchanged. Older saved layouts
  without the key load as before. The Home settings' **Show hidden items**
  became **Reset bar icons** (clears hidden ids and the bar list). Tested in
  `ShellBarLayoutTests`.
- Android's fixed bar and the iPad sidebar are unchanged. Not checked on a
  device from Linux.

### 2.0.47 Colourful Home icons (2026-10-02, iPhone)

The user sent a mock-up of Home with large glossy icons, each in its own
colour with a soft glow, and asked for the icons to look like that, with good
colour diversity.

- **Colour per icon.** `ShellHomeIcons` (FMApi) gives every Home item one of
  13 hues plus slate (`ShellHomeHue`), chosen by its SF Symbol
  (`bySymbol`, keyed without `.fill`), with `byId` for the few items that share
  a symbol (Discussions is indigo, Chat orange). The hues were picked so
  neighbours in a section differ and each section mixes warm and cool
  colours. Items with no chosen hue (a new package route) get a stable one
  from a hash of their id and symbol. `ShellHomeIconsTests` checks that every
  `Route.Kind` symbol and Hub item has a chosen hue, that neighbours in a
  section never match, and that the sections use at least 12 hues.
- **The glyph.** `ShellHomeGlyph` draws the symbol's `.fill` variant when
  UIKit has one (`ShellHomeIcons.glyph`), in a top-to-bottom gradient of its
  hue, with a white gloss on the upper half (a gradient masked by the glyph),
  a glow in the hue's deep tone and a 1 pt drop shadow.
- **The tile.** Home icons are 24 pt (was 15) in 60 pt tiles (was 36), with
  6 pt between icon and label. The grey tile background is gone: only the
  selected tile keeps its accent tint. Labels are semibold in the primary
  text colour. Count pills take the icon's deep tone with white digits; unread
  badges stay red (or accent while running). Badges sit relative to the icon
  size. The **Search everywhere** row keeps its 36 pt height (`rowHeight`).
- `ShellBarCellLabel` draws the coloured glyph only when given a `hue`, so the
  bottom bar and the bar editor look as before. Not checked on a device from
  Linux: the app sources were only parsed.

### 2.0.48 The build Mac's light beside the version (2026-10-02, iOS)

The user asked for the build Mac's status (the office Mac of
[MAC_BUILD.md](MAC_BUILD.md)) on the phone's bottom bar, next to the version
number with the other status lights.

- `ShellVersionFooter` draws `ShellBuildMacLight` after the core node and
  runner lights: a `laptopcomputer` glyph, `fm.success` while the Mac is
  idle, `fm.accent` while it holds its build lock and `fm.danger`, blinking
  (not with Reduce Motion), when the desktop cannot reach it. VoiceOver reads
  the state and the job ("Build Mac, building the macOS release v1.2.5"). A
  tap opens Desktops, as the desktop light does.
- The phone cannot see the Mac itself: it asks the desktop, which holds the
  Mac's key. `diag.buildMac` on the desktop
  (`MainWindowLocalControlDiagnostics.cpp`) ssh-es to `build@MAC_BUILD_HOST`
  (the Actions variable, else `192.168.1.201`) with
  `~/projects/forkmesh/ssh/mac-build/{id_ed25519,known_hosts}` and the pinned
  `forkmesh-mac` host key, 5-second connect timeout, and reads `~/ci/lock`:
  no lock (or one older than four hours, which the build scripts treat as
  stale) is `idle`, a lock is `building` with its run name (`ci/<commit>-<time>`,
  `desktop-release vX.Y.Z`, `ios-release vX.Y.Z`), and a failed ssh is
  `offline`. The answer is cached for 20 seconds and concurrent requests share
  one ssh. It does not scan the LAN as the build workflow does, so a Mac that
  moved to another DHCP address reads as offline until `MAC_BUILD_HOST` is
  updated. A desktop without the key answers `configured: false` and the
  phone draws nothing; so does a desktop too old to know the method.
- `BuildMacLightModel` (`FMInsights/BuildMacLight.swift`) asks only over an
  already open channel (`DesktopControlHub.currentLink`), so the light never
  dials the relay by itself. It loads when the shell starts, when the app
  becomes active and when the desktop link comes up. No timer: a reload waits
  15 minutes after an idle read and 2 minutes while the Mac is building or
  offline. A failed request keeps the last light. It is not gated on the
  Status Page package, like the desktop light. Tested in `BuildMacLightTests`.
- Android has no version footer yet, so it has no build Mac light. Not checked
  on a device from Linux.

### 2.0.49 Home icons back to the plain look; counts in the accent (2026-10-02, iPhone)

The user asked for the Home icons to go back to the colour and style they had
before 2.0.47, and for the numbers on them to follow the accent colour chosen
in Settings.

- 2.0.47 is reverted: `ShellHomeGlyph`, `ShellHomeIcons` and their tests are
  gone. Home icons are again 15 pt plain SF Symbols in the primary text colour
  (accent when selected or active) on 36 pt grey tiles.
- **Counts.** The count pill on a Home icon is filled with `fm.accent` (the
  user's Settings accent) with white digits, instead of a faint grey pill.
  Unread badges keep red (accent while running).
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.50 The desktop light is green through the relay too (2026-10-02, iOS)

The user, away from the desktop's network, saw the footer's desktop light red
while the phone was still driving the desktop, and asked for it to show that
the desktop is connected.

- The light read only `LocalControlService`, the LAN channel, so a desktop
  reached through the relay (`DesktopControlHub`'s `.connected(.relay, …)`)
  counted as not connected.
- `ShellLocalDesktop` gains `.relay(name)` ("Connected to <name> through the
  relay") and `isConnected`, true for `.connected` and `.relay`.
- `DesktopControlHub` is now the shell's `ShellLocalDesktopBridge`
  (`HostsFeature.register` replaces the one `ChatFeature` put): the LAN state
  when the LAN channel is connected, `.relay` when only the relay channel is
  open, and otherwise the LAN state (in reach, connecting, approval, hidden).
- `ShellFooterDesktopLight` is green for either connection; through the relay
  it adds a small green `globe` at the glyph's lower right, so the two read
  apart. VoiceOver reads the label above.
- Tested in `HomeShellViewModelTests.testLocalDesktopLabels` and
  `DesktopControlHubTests.testWithNoLanTheHubDialsTheRelayAndKeepsTapsAcrossIt`.
  Not checked on a device from Linux: the app sources were only parsed.

### 2.0.51 The Genie button above the new agent button (2026-10-02)

The user asked for a second floating button on top of the new agent button,
for an on-device voice genie.

- `GenieButton` (46 pt, a live camera view of the animated genie at 20 fps,
  paused when the app is inactive, with a lens vignette and a gold rim)
  stacks `FmSpace.x3` above `ShellNewAgentButton`. On iPhone both
  sit in one column of `ShellBottomBar`, centred on the More cell; on iPad
  they stack in `floatingButtons`. It hides with the bar.
- The genie in the button waves every 21 s. It glances at a tab you pick,
  nods when a page opens, cheers when an agent starts, and looks surprised
  at a new ping (`GenieCues`, GENIE.md).
- A tap opens `GenieSheet`, a full-height dark sheet. The Genie's app
  actions are queued and run through `runPalette` once the sheet closes. See
  [GENIE.md](GENIE.md).
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.52 Square selection, glass under the version, a dot on a busy runner (2026-10-02, iOS)

The user asked for the selected bar button to be a rectangle, rounded only
on the side that meets the end of the bar; for a semi-transparent, Liquid
Glass-like background under the version line so it reads better; and for a
tiny blinking dot on the runner icon while an agent runs on that runner.

- **Selection.** `ShellBottomBar.selectionShape` gives the selected cell (and
  a selected or open folder) an `UnevenRoundedRectangle` with 10 pt corners.
  The first cell of the row gets its leading corners, and the last its
  trailing corners, at half the cell height, so it follows the capsule.
  Folder popover tiles keep the capsule.
- **Footer glass.** `ShellFooterGlass` puts the version, the lights and the
  status dots on `glassEffect(.regular)` in a 14 pt rounded rectangle on
  iOS 26, and `ultraThinMaterial` with a hairline border before. It hugs the
  content in both the one-row and the two-row layout, and on the iPad
  sidebar too.
- **Busy runner.** `AgentsScreen` puts the lowercased `runner` of every
  active agent line into `ShellCounts.busyRunners` (cleared on a project
  switch). The server writes the same lowercased `runner_nodes.name` into a
  session's `runner` and the `runner:<name>` status id, so
  `ShellFleetLights` matches them exactly. A matched `cpu` glyph gets
  `ShellRunnerBusyDot`, a 5 pt accent dot at its top right that blinks (it
  stays lit with Reduce Motion), and VoiceOver adds "running an agent".
  Runs on personal runners have no light, so nothing is drawn for them.
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.53 Icon numbers without a coloured background (2026-10-02, iOS)

The user asked that the numbers on icons have no coloured background.

- `ShellBarCellLabel` (bottom bar, home grid, bar editor) and
  `ShellBadgedIcon` (the bell, menu rows) draw the count as bold digits with
  no pill. The digit colour carries the meaning the pill had: red for unread,
  accent while running, accent for plain counts. The 16 pt frame stays, so
  the digits sit where the pill's centre was.
- This supersedes the red and accent pills described above for icon badges.
  The chat channel list's unread pill is a row count, not an icon number, and
  is unchanged.
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.54 The Agents button's running count has a heartbeat (2026-10-02, iOS)

The user asked for the Agents button on mobile to show the number of active
agents too, with a heartbeat.

- `ShellBarItem.live` carries a running count; only the Agents tab sets it,
  from `ShellCounts.agents` (the runs `AgentsScreen` counts as active). The
  red total on the icon's top right stays (2026-10-01).
- `ShellBarCellLabel` draws the running count on the icon's top left as
  accent digits (`ShellHeartbeatCount`) over a faint accent disc. Every
  1.1 s it beats twice, lub then a softer dub (the digits grow by up to 22%
  and the disc brightens), and a thin accent ring spreads out and fades from
  each beat. A bar folder holding Agents shows its members' summed count the
  same way.
- Like the activity ring (2.0.42) it is drawn from a `TimelineView` capped at
  30 frames a second, so it beats on pages opened with animations off. With
  Reduce Motion it is still and the ring is hidden.
- VoiceOver reads "N running" before the item's count, in place of
  "Active".
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.55 A load light on the desktop glyph; no build Mac glyph (2026-10-04, iOS)

The user asked for the red blinking computer in the middle of the footer to
go, and for a light on the top right of the desktop glyph at the start of the
row instead: green, yellow or red by the desktop's load, blinking when red.

- `ShellBuildMacLight`, `BuildMacLightModel` and their tests are gone; the
  footer no longer asks `diag.buildMac` (the desktop still answers it,
  [MAC_BUILD.md](MAC_BUILD.md)).
- `ShellFooterDesktopLight` draws the `desktopcomputer` glyph in
  `fm.textSecondary` (the relay `globe` too) with a 6 pt dot on its top
  right: `fm.success` while the desktop is calm, `fm.warning` while busy and
  `fm.danger` while strained or not connected. A red dot blinks (not with
  Reduce Motion). VoiceOver reads the connection label and the load
  ("Connected to Mac on this network, busy").
- The load is the `level` of the desktop's `diag.health` answer (`ok`,
  `busy`, `strained`; `healthLevel` in `LocalControlDiagnostics.cpp`: memory
  or IO pressure, little free memory or disk, a one-minute load above 1.5x the
  CPUs, open files near the limit). Connected with no reading yet, or a
  desktop too old to answer, is green.
- `DesktopLoadLightModel` (`FMInsights/DesktopLoadLight.swift`) asks only
  over an already open channel, as the build Mac model did. It reads when the
  shell starts, when the app becomes active and every time the desktop link
  changes. No timer: a reload waits 15 minutes after a calm read and 2
  minutes while busy or strained. A failed request keeps the last load.
  Tested in `DesktopLoadLightTests`.
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.56 Design in the App group, from the Design System package (2026-10-04, iOS and Android)

The user asked for a design system at `/design` on the web with buttons that
link to it on desktop and mobile, as a package people can remove.

- `HubCatalog.app` ends with **Design** (`designSystem`, SF Symbol
  `paintpalette` on iOS, Material `palette` on Android) before Sign in/out.
  Its action is `designSystem` / `HubItem.Action.DesignSystem`: the phone opens
  `LegalLinks.designFor(homepage)`, the active project's homepage with the
  path `/design`, in the browser. On iOS it joins the shell bar like World; on
  Android it is a page in the shell menu like World.
- The package has no account data, so the phone keeps its on/off per device
  under `packages/design-system/on` (default on): iOS `SettingsService.designSystemOn`
  through `PhonePackages`, Android `AccountSettings.designSystemOn` through the
  new core `PhonePackages`. The Market page shows "Installed on this device";
  turned off, `HubGroups.current` (iOS) and `PhonePackages.hiddenHubItems`
  passed to `HubCatalog.groups(hidden = …)` (Android) drop the item, and the
  Market's Installed filter follows it (`MarketModel(deviceSwitch)` on Android).
- The Market row's web surface is the `www.page` route (`/design`), so the
  package page offers "Open on the web".
- Not checked on a device from Linux.

### 2.0.57 Run shortcuts on the phone itself (2026-10-05, iOS)

The user asked to be able to run shortcuts directly on the phone. Until now
every Run went to the paired desktop, and with no desktop the Shortcuts page
was empty.

- A prompt shortcut or skill (`.md`, `.txt`, `.prompt`, `.skill`) now has a
  **Run on this iPhone** button on its page, under the desktop's Run. It
  strips the `# name:`, `# description:`, `# agent:` and `# model:` lines the
  same way the desktop's `shortcutPrompt` does (`PhoneShortcuts.prompt`),
  replaces the desktop-only placeholders `{{appLog}}` and `{{stallLog}}` with
  "(only on the desktop)", and opens the shell's composer with that prompt on
  the shortcut's repository, aimed at this iPhone
  (`ComposeTarget.onDevice`, `QuickAgentLauncher.prefer(.device)`, which does
  not change the repository's remembered destination). Send starts the
  on-device agent with the composer's model chip, so on-device Gemma works
  too. The prompt replaces whatever was drafted in the composer, since Run
  is an explicit request. The shortcut's `# model:` line is not applied on
  the phone. A repository not yet on the phone, or no model set up here,
  gets the composer's usual one-tap fix (Mirror to this iPhone, Set up the
  agent, Set up Gemma).
- The composer is the confirmation step: the prompt can be read and edited
  before Send, as the desktop's confirm sheet allows for a desktop run.
- With no desktop answering, the page reads the phone's own copy of the
  repository: `.forkmesh/shortcuts` (scope `shortcut`) and
  `.forkmesh/skills` (scope `repository`), through `ShortcutMirror`, which
  the app implements over `LocalRepoStore` (`PhoneShortcutMirror`). The
  repository picker then lists the mirrored repositories, and a note says the
  list comes from this iPhone. Entries read this way (`ShortcutEntry.onPhone`)
  have no desktop Run; scripts list but do not run, since a phone has no
  shell. App and organization skills live on the desktop and only show when
  it answers.
- A mirrored repository shows its phone list at once and switches to the
  desktop's list (with run counts and script output) when the desktop
  answers. A repository with no copy on the phone waits for the desktop and
  never jumps the picker to another repository.
- Android still runs every shortcut on the desktop.
- Not compiled or checked on a device from Linux.

### 2.0.58 No zero counts on Home (2026-10-05, iPhone)

The user asked for Home not to show the number 0 when a tile has no items.
`ShellBarCellLabel` drew the accent count pill whenever an item had a
`count`, so Home tiles (Agents, Pulls, Saved and the like) showed "0". The
pill now shows only for a count above zero, and VoiceOver's value for the
tile (`ShellHome.value`) leaves a zero count out too. Android's tiles
(`BadgeIcon`) already hid zero.

- Not compiled or checked on a device from Linux.

### 2.0.59 The heartbeat counts each cloud run once, in its current state (2026-10-06, iOS)

The user reported that the Agents button's heartbeat count (2.0.54) was
wrong, mostly for cloud agents. The count is the active lines of
`AgentLines.merge`, which joins three reads: each repository's relay queue
(`.launched` entries), the desktop's pushed snapshots (`.desktop` entries) and
the control center's rows (the desktop in reach plus the account-wide cloud
list). Three things made cloud runs count wrong:

- A queued run took its state from the repository's own read unless the run
  already had a branch or a runner. That read can be older than the
  account-wide one, so a run that had stopped or finished stayed "running".
  `AgentLines.fresher` now takes the status, label and active flag from the
  account row when it is at least as new. A run the pull ledger shows as
  merged keeps that.
- A website run the desktop in reach claimed was shown twice: as the
  desktop's own session and as the repository's queued run, because
  `mergeAgentRows` had already folded its cloud row into the desktop row.
  `AgentLines.claimedByDesktop` drops the queued line when a desktop row has
  the run's `localAgentId`, node (runner, else target node) and repository.
- The same session listed under two repositories (an organization alias and
  the owner) counted twice. Merge keeps the first line per session id.

Tested in `AgentLinesTests`. The relay itself still keeps a run `running`
when its node goes away without reporting; that is not changed here.

### 2.0.60 A docked prompt clears the bar, measured where it is drawn (2026-10-06, iPhone)

The user sent a screenshot from 1.4.0 (170) of an agent's run page with the
follow-up composer's lower edge still under the glass capsule, and the new
agent button floating over the transcript, and asked for the prompt area to
stop overlapping the bar.

- The new agent button sat where the composer should have been one gap
  below it, so the composer was the 34 pt home indicator inset too low:
  2.0.30's correction was always zero.
- `ShellDocked` read its page's bottom edge outside `.ignoresSafeArea`, which
  reports the safe-area frame, not the frame the page draws in. The page
  therefore looked as if it ended above the bar's bottom, and the clamp
  dropped the difference. The bar's own edge is read inside its
  `.ignoresSafeArea`, so the two were measured differently.
- The page's edge is now read inside `.ignoresSafeArea` too, where it is the
  edge the composer is laid out against, so the composer sits
  `ShellDock.gap` above the capsule whether or not the bar reaches the
  screen's bottom.
- Not checked on a device from Linux.

### 2.0.61 Bar on the bottom edge, office MacBook light, runner counts, one machines page (2026-10-06, iOS)

The user sent a screenshot of the bar and footer and asked for them to sit
all the way down, for a light for the office MacBook, for a tiny number on
each runner glyph counting the agents on it, and for the runner, Mac and
core node status to be on the Desktops page that the footer icons open.

- `HomeShell.footerLift` is now a fixed 2 pt (it was the bottom inset less
  `indicatorReach`, 10 pt on a 34 pt inset), so the footer's glass sits on
  the screen's bottom edge and the capsule above it moves down with it. The
  lights now overlap the home indicator.
- `ShellFooterMacLight` comes back after the desktop glyph (2.0.55 removed
  `ShellBuildMacLight`): a `laptopcomputer` glyph in `fm.textSecondary` with
  the desktop's 6 pt dot, green idle, accent building, red when the desktop
  cannot reach the Mac. It does not blink. `BuildMacLightModel`
  (`FMInsights/BuildMacLight.swift`) is restored as it was, read through the
  desktop's `diag.buildMac` ([MAC_BUILD.md](MAC_BUILD.md)) with the same
  floors (15 minutes idle, 2 otherwise) and refreshed with the desktop load.
  It is hidden while there is no answer, when the desktop has no Mac key, or
  when the desktop is too old. VoiceOver says "Office MacBook, idle".
- `ShellCounts.runnerAgents` (was `busyRunners`) maps a lowercased runner
  name to its active agents, from `AgentLines.runnerCounts`. A runner glyph
  with agents shows the count in 7 pt heavy accent digits at its top right
  (`ShellRunnerAgentCount`, replacing the blinking `ShellRunnerBusyDot`);
  VoiceOver adds "running 2 agents".
- The desktop, Mac and fleet glyphs all open Desktops (`Route.desktops`);
  only the status dots still open the Status page. `DesktopsScreen` adds an
  Office MacBook section (state and how long the current build has run),
  Runners (signed in or not, the failure reason, the agent count) and Core
  nodes (operational or not, the reason, a link to the Status page). The page
  reads `/api/status` itself on open and on pull to refresh, and forces a
  fresh `diag.buildMac` read.
- Tested in `BuildMacLightTests` and `AgentLinesTests`. Not checked on a
  device from Linux: the app sources were only parsed.

### 2.0.62 The new agent button moves out of the way like the Genie (2026-10-06, iOS, issue #1462)

Superseded by 2.0.88: the button is now fixed in the header and cannot be dragged.

The user asked to move the floating new agent button the way the Genie
spider moves, so it can get out of the way.

- Drag `ShellNewAgentButton` anywhere on the screen; it lifts (scale 1.1, a
  bigger glow) while held. Drop it within a quarter of its width of the left
  or right screen edge and it tucks, showing 40% of the circle. Drop it
  within three quarters of its width of its usual spot and it snaps back.
  Anywhere else it stays, kept inside the safe area with an 8 pt margin.
- A tap still opens the composer and a long press the command palette, even
  while tucked; a drag starts after 12 pt and cancels the long press, as on
  the spider. Drag a tucked button out to bring it back.
- The spot is an offset from where the layout puts the button
  (`NewAgentButtonSpot` in `FMAgents`), so it follows the bar when an
  agent's prompt docks and is re-clamped on rotation or a size change. It is
  stored under `shell/new_agent_x`, `shell/new_agent_y` and
  `shell/new_agent_tucked`; the usual spot removes the keys.
  `ShellNewAgentPlace.shared` holds it for the iPhone bar and the iPad's
  `floatingButtons`.
- The button only moves visually: the bar keeps its row and pages still end
  above where it would sit. It has `zIndex(1)` in the bar so it draws and
  takes touches over the capsule.
- VoiceOver adds "Tucked away" as its value and the actions "Tuck into the
  edge" / "Come out" and "Put back in its place".
- Tested in `NewAgentButtonSpotTests`. Not checked on a device from Linux:
  the app sources were only parsed.

### 2.0.63 Square Home icons without backgrounds, the most used glow (2026-10-06, iPhone, issue #1463)

The user asked for the Home buttons to be square, without their button
backgrounds, and for the ones opened more often to show it, by how many
times.

- Each Home tile is a square as wide as its column (six across) instead of a
  36 pt high rounded rectangle. The grey tile and the selected tile's accent
  tint are gone; a selected tile still shows its icon and label in the accent.
- **How often it was opened.** Home reads the bar's use counts
  (`ShellBarPrefs.uses`, `shell/bar-uses`, the same counts as the "Most used"
  sort, cleared by "Clear usage history"). `ShellBarOrder.heat` turns a count
  into 0 to 1 on a log scale against the most used Home item. A tile never
  opened looks plain; the more it was opened the bigger its icon (up to 1.45
  times the 15 pt) and the stronger a soft accent glow behind it (radial,
  no edge). VoiceOver adds "Opened once" or "Opened N times" to the tile's
  value.
- The tiles and the search field are also places the Genie spider keeps off
  ([GENIE.md](GENIE.md), "Keeping off buttons").
- Tested in `HomeShellViewModelTests`. Not checked on a device from Linux:
  the app sources were only parsed.

### 2.0.64 Office MacBook build buttons (2026-10-06, iOS, issue #1488)

The user circled two spots on the Office MacBook row of the Desktops page
and asked for one button that runs the existing Mac ad hoc shortcut and one
that builds the macOS desktop app, with a new shortcut for the latter.

- `BuildMacRow` shows two small bordered buttons between the label and the
  dot: **iOS** (`iphone`) runs `deploy-native-ios-ad-hoc-mac.sh` and
  **macOS** (`macwindow`) runs `build-desktop-macos-mac.sh`, both from
  `.forkmesh/shortcuts/` of `forkmesh/forkmesh` on the desktop
  ([MAC_BUILD.md](MAC_BUILD.md)).
- A tap goes through the Shortcuts page's own path (`ShortcutsStore.prepare`,
  `TeamConfirmSheet`, `ShortcutsStore.run`), so the desktop asks for the same
  confirmation, and a second tap while one runs restarts it as on the
  Shortcuts page. After the run starts, the page pushes
  `ShortcutOutputScreen` with the live output. While a run of that shortcut
  is going, its button shows a spinner instead of the glyph. A refusal
  (another shortcut running, the desktop without the repository) shows under
  the row in red.
- The buttons are hidden when the phone has no shortcuts store. VoiceOver
  reads the row's label and then each button by what it builds.
- Not checked on a device from Linux: the app sources were only parsed.
### 2.0.64 Tighter Home, blue counts, a used-often mark and a Recent row (2026-10-06, iPhone, issue #1487)

The user asked for the Home icons to sit closer together, for the red count
numbers to be blue everywhere, for the app to track which sections are opened
most and mark those, and for a top row of the most recently visited sections.

- **Closer together.** Tiles are 42 pt high (scaled with Dynamic Type)
  instead of squares as wide as a column (about 62 pt on an iPhone), with no
  gap between columns or rows and 4 pt side padding. Section headers keep
  their 8 pt indent. 2.0.63's square shape is gone; the tiles have no
  background, so only the spacing changes.
- **Blue counts.** `FmPalette.count` is the blue accent (`FmAccent.blue`,
  light or on-dark), fixed whatever accent the user picked. Every icon number
  uses it: unread badges and plain counts in `ShellBarCellLabel` (bottom bar,
  Home, bar editor), the folder pill in the bar editor, `ShellBadgedIcon`
  (the bell, menu rows) and the Agents heartbeat count. Red no longer means
  unread on icons, and the running/unread colour split is gone; this
  supersedes 2.0.53's colours. Status colours (down, offline, alarms) stay
  red.
- **Tracking.** Taps from Home and the bar counted already
  (`ShellBarPrefs.use`). Sections opened another way now count too:
  `HomeShell.visitedBarIds` is the selected bar items (repository sections
  first, then the tabs, Code and Home) or, with none selected, the hub item
  whose route is on top of the stack (`ShellBarOrder.hubItemId`). When it
  changes, `ShellBarPrefs.visit` records the first id unless one of them is
  already the most recent, so a tap that also changes the selection counts
  once (`ShellBarOrder.counted(visit:recent:)`). Coming back from Home to the
  page underneath counts as a visit to that page.
- **Used-often mark.** A tile opened at least 3 times and with at least half
  the heat of the most used Home item (`ShellBarOrder.frequentMark`) shows a
  small blue `cellularbars` glyph before its label, filled by its heat.
  VoiceOver adds "Used often". The bigger icon and glow from 2.0.63 stay.
- **Recent row.** With the search field empty, a "Recent" section heads
  Home: up to six Home items from `ShellBarPrefs.recent`, newest first
  (`ShellBarOrder.recentRow`; Home itself and items no longer on Home are
  left out). Its tiles open, dock and drag like the others but do not take
  drops, so the menu layout is never moved into it. Identifiers are
  `shell-home-recent-<id>`. "Clear usage history" empties it.
- Android is unchanged.
- Tested in `HomeShellViewModelTests`. Not checked on a device from Linux:
  the app sources were only parsed.

### 2.0.65 The chat room composer is one glass line on the bar (2026-10-06, iPhone, issue #1491)

The user sent a screenshot of a chat room with the "as jett" line, the plus,
a dark text field and a square send button above the new agent button, and
asked for the chat input to be a single Liquid Glass line between the new
agent button and the navigation.

- The room's composer is docked with `.shellDock` (§2.0.25), like the agent
  pages: it sits `ShellDock.gap` plus 8 pt above the capsule and the new
  agent button rises above it. With the keyboard up or on iPad it sits at
  the page's bottom as before.
- `ChatComposerBar(glass: true)` draws the plus menu, the field and a round
  send button inside one rounded glass shape (`chatGlass`, Liquid Glass on
  iOS 26, thin material before). The field still grows to five lines while
  typing. The thread reply composer keeps the old flat style.
- The "as <name>" identity line is gone from the room composer so it stays
  one line; thread replies still show "Replying as".
- The guest sign-in notice and the read-only channel line take the same
  docked place; the read-only line is on glass too.
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.66 Opening the chat input keeps the last message in view (2026-10-06, iPhone, issue #1498)

The user reported that tapping the chat input hid the last message: the
keyboard shrank the transcript but kept its top in place, so the newest
messages ended up under the composer and keyboard.

- `chatKeepsEndInView` (in `ChatScreen.swift`) scrolls to the last message
  when the composer gets focus, and again whenever the transcript gets
  shorter while the composer is focused, which is when the keyboard
  finishes moving in. Scrolling on focus alone ran before the keyboard
  resized the page, so it did not help.
- It is used by the channel room, direct messages and private groups (which
  replaces their old scroll on focus only) and thread replies.
- Not checked on a device from Linux: the app sources were only parsed.
- Its first version used the iOS 18 two-value `onGeometryChange` and
  broke every ad hoc build until it was fixed ([MAC_BUILD.md](MAC_BUILD.md)).

### 2.0.67 Agent sends open in the Agents tab; a model dropdown on Android (2026-10-04, iOS and Android)

The user asked for the prompt's agent send to go right to the queue with no
step in between, and for the agent model dropdown in the prompt.

- **Send.** The native shell composer already queues on send with no form in
  between (the step the user meant was the Flutter start form, gone since
  2026-09-28). The queued run's page (`LaunchedAgentSessionScreen`) stays:
  it is the queue entry, where the run's progress shows. A first pass
  dropped it and landed on the Agents list; that was reverted as a
  regression. iOS is unchanged from main. On Android the shell composer
  pushed that page, or All agents for a desktop run, onto whichever tab was
  open; `ShellPrompt(onStarted =, openRun =)` now pops the Agents tab to its
  root, selects it and pushes the page there, as iOS `HomeShell.handle`
  does. The Agents tab's own quick start keeps pushing on its navigator.
- **Model dropdown on Android.** The composer's model chip opened a bottom
  sheet (`pickQuickModel`, removed). `PromptComposer` now takes
  `modelChoices`, `selectedModel` and `onSelectModel`, as the iOS composer
  takes `modelOptions`; with them the chip and `/model` open an
  `FmDropdownMenu` under the chip, each row the model's name over its
  provider and note, with a check on the chosen one. The shell composer and
  the Agents tab's quick start pass them. Surfaces that only pass
  `onPickModel` (the agent page opens its setup) keep that.
- iOS already had the dropdown (`PromptComposer.modelChip`).
- Checked by compiling the Android app and its core tests.

### 2.0.68 Home fits on one screen (2026-10-06, iPhone)

The user found the Home icons jumpy when scrolling the page up and down and
asked for them to be smaller so no scroll is needed.

- `ShellHome` measures its visible height (less safe-area insets), the
  search field block and one section header. The tiles share what is left
  after the search field, the headers and the 12 pt bottom padding:
  `ShellBarOrder.fittedTile` divides it by the number of tile rows (six per
  row, per section, Recent included), capped at the 42 pt of 2.0.64 and
  floored at 28 pt (both scaled with Dynamic Type).
- The icon, its heat growth and its glow shrink by the same ratio as the
  tile. Labels keep their size and still shrink to 70% to fit the width.
- When even 28 pt tiles do not fit (very large text, a long menu) Home
  scrolls as before; with everything fitting, `.basedOnSize` keeps it from
  bouncing.
- The height is not re-measured while the search field has focus, so the
  keyboard does not shrink the tiles while typing.
- Android is unchanged.
- Tested in `HomeShellViewModelTests`. Not checked on a device from Linux:
  the app sources were only parsed.

### 2.0.69 Home: no holes, no corner glyphs, ruled section headers (2026-10-07, iPhone)

The user sent a Home screenshot with gaps in the grid and asked to drop the
two tiny indicator icons on the left of the tiles and make the section
markers nicer.

- **Holes.** An item in the Recent row was also in its own section, and both
  tiles had the same identity in the one `LazyVGrid`, so the section's copy
  drew blank. Tiles are now `ShellHomeTile`s identified by section and item
  (`recent/inbox`, `main/inbox`), so both copies draw.
- **Glyphs.** The `dock.rectangle` mark (item is in the bar, from 2.0.46)
  and the `cellularbars` used-often mark (2.0.64) are gone from the tiles.
  VoiceOver still says "In the bar" and "Used often"; the heat glow and
  bigger icon still show use.
- **Section headers.** The uppercase title is semibold and followed by a
  hairline in `fm.border` across the row, with 8 pt above and 4 pt below so
  count badges no longer touch the title. The header is one VoiceOver
  element.
- Not checked on a device from Linux: the app sources were only parsed.

### 2.0.70 Home counts sit beside the icon; grown icons leave the label its room (2026-10-07, iPhone)

The user sent another Home screenshot, still with the grid holes, the
`dock.rectangle` glyph on the Recent and Go to tiles and unruled headers:
a build from before 2.0.69, which already removes the glyph from every tile,
the top ones included. Two layout faults in it were still in the code.

- **Counts over the icon.** Badges and counts were centred near the icon's
  top right corner, so a wide one ("1,840" on CRM, "718" on Steps, "92" on
  Marketing) spread left across the glyph. In `ShellBarCellLabel` their
  leading edge is now pinned 2 pt inside the icon's trailing edge and they
  grow to the right; the Agents heartbeat count mirrors this on the left.
  One-digit counts sit where they did. This applies to the bottom bar too.
- **Squeezed labels.** At the 28 pt floor of 2.0.68 the heat growth of
  2.0.63 made the icon taller than the tile allowed, and the label shrank
  instead ("Agents" and "Inbox" smaller than "Chat").
  `ShellBarOrder.tileIcon` caps the grown icon at the tile height less 17 pt
  (`ShellHome.labelRoom`: the 10 pt label, the 1 pt gap and the icon
  frame's 4 pt), never below the tile's base icon.
- Tested in `HomeShellViewModelTests`. Not checked on a device from Linux:
  the app sources were only parsed.

### 2.0.71 A shortcut's output ends above the bar and follows new lines (2026-10-07, iPhone)

The user sent a screenshot of the "Deploy native iOS app (ad hoc, office
Mac)" output page while it ran: the newest lines and the cursor were under
the Liquid Glass bar and the version line. They asked for the end of the
output above the bar and for the page to scroll itself.

- Another closure-pushed page (§2.0.33): `ShortcutOutputScreen` never got
  the bar clearance, so its scroll to the end put the last line behind the
  bar. Its four ways in now add `.shellPageClearance()`: the Desktops page
  after a Build Mac run (`DesktopsScreen`), the Output row on the Shortcuts
  page, and the live run row and the after-run push on a shortcut's page
  (`ShortcutDetailScreen`). The shortcut page itself, also closure-pushed,
  gets it too.
- `ShortcutTerminal` keeps its bottom anchor (`.defaultScrollAnchor(.bottom)`)
  and scrolls to the end on the next main-actor turn after new output, once
  the longer text is laid out, so it keeps following a running script.
- Not checked on a device from Linux.

### 2.0.72 Status Page follows the account; checks, settings and your monitors (2026-10-07, iOS and Android)

The user asked for the green status dots to be a plugin with settings, each
check a dependent plugin, custom monitors, and an uninstall that removes it
from desktop, mobile and web at once. The server side is in
[docs/marketplace/status-page.md](../marketplace/status-page.md).

- The per-device switch of 2.0.43 is gone (`packages/status-page/on` is no
  longer read). Both phones keep the account's answer from
  `/api/plugins/@account` in prefs (`packages/account/state`, with the
  account-synced Market rows in `packages/account/catalog` and the custom
  monitors in `packages/status-page/monitors`). First run, signed out, or a
  failed request: the last answer, or everything installed with defaults.
  The state is reread on launch, on return to the foreground, on sign-in,
  when the Market or a package page opens, and after every install,
  uninstall or settings call (the answer carries the new state). iOS also
  drops the store on a project switch (`AccountPackagesStore`).
- A package page for an account-synced package has one switch, "Installed on
  all your devices". Removing asks first and names what goes with it (all the
  checks, and your monitors, for Status Page); installing a check while
  Status Page is removed says Status Page comes back too. Declared settings
  follow as a toggle (boolean) or a picker (choice). Status Page's page adds
  "Your monitors": name, URL, remove, and an add form.
- Dots: only systems an installed `status-check-*` package claims, plus a
  `custom:<id>` dot per monitor that the phone itself loads (GET, 10 s
  timeout, green below HTTP 400). `onlyProblems` hides green dots;
  `refreshMinutes` is the wait between reads while all are green, and any
  dot that is not green keeps the 2-minute floor. Before the Market rows have
  ever loaded, every system shows. On iOS the core and runner lights are
  filtered the same way (Core Nodes, Runners checks), and mirror dots now
  show when the opt-in Mirrors check is installed.
- Android gets dots for the first time (`StatusDots.kt`,
  `StatusPageFeature.kt`): a row just above the bottom bar on phones and at
  the foot of the side rail on wide screens, since Android has no version
  footer; a tap opens Status. Reads run only in the foreground.
- Removed for the account: no dots, no `/api/status` reads or monitor probes
  from the shell, and no Status entry in the hub or Navigator. On Android it
  also leaves global search, the command palette and the recents lists. The
  `/status` deep link still opens the screen, since alert pings link there.
- Checked with the Linux Swift toolchain (FMPackages, FMInsights, FMAccount:
  393 tests) and Android core tests plus `:app:compileDebugKotlin`; not run
  on a device.

### 2.0.73 Core icons and status dots packed on the footer line (2026-10-08, iPhone)

The user asked for the core icons and status checks on the bottom bar's
version footer to sit side by side on one line so they take less room. On an
iPhone the 2.0.45 line was too wide once the core nodes, runners and dots
were all shown, so the footer fell back to two rows.

- `ShellVersionFooter`'s `ViewThatFits` now tries a compact line between the
  full line and the two rows: 4 pt between groups instead of 8 pt, the fleet
  glyphs (`ShellFleetLights`) 1 pt apart, and the status dots
  (`ShellStatusDots`) overlapping by 2 pt, each ringed in the page colour
  (`ShellStatusDot.ringed`) so they read as a stack.
- The two rows stay as the last fallback, such as with the Update capsule
  showing on a narrow phone.
- Android has no version footer. Not built or checked on a device from Linux.

### Profile diagnostics footer and edge navigation (2026-10-10, iPhone)

The version, desktop, core/runner and service-health footer now belongs to
Settings > Profile instead of the home screen. The Profile card includes the
Diagnostics switch: off hides the footer and avoids its status refreshes;
turning it on loads the current lights. The home navigation capsule is no
longer lifted above that footer, so it reaches the bottom edge of the screen.
After a loaded core, runner or service check reports trouble, the top-right
account-avatar dot is red while the relay itself remains connected.

### The bar touches the bottom of the phone (2026-10-13, iPhone)

The user asked for the bottom bar to move all the way down until it touches
the bottom of the phone.

- `HomeShell.barStack`'s `.ignoresSafeArea(.container, edges: .bottom)` did
  nothing on its own: the bar has a fixed height, so the area it is placed in
  grew into the home indicator inset but the capsule stayed on the safe-area
  line, 34 pt plus its 4 pt bottom padding above the screen's edge. A
  throwaway SwiftUI app on the office Mac's iPhone 17 simulator showed this.
- `barStack` now fills the overlay (`.frame(maxHeight: .infinity, alignment:
  .bottom)`) before ignoring the bottom safe area, so the bar is placed at the
  bottom of the larger area. `ShellBottomBar` drops its bottom padding, so the
  capsule's lower edge is the screen's bottom edge.
- The bar's height and `barBottom` are still read on the bar itself, before
  the frame, so `barClearance`, `ShellDock.bottom` and the docked prompt's
  gap follow the capsule down.
- Checked with that simulator app only. The full app was not built.

### Core icons over the status dots when the packed line is too wide (2026-10-08, iPhone)

The same request as 2.0.73, from a parallel branch: the user asked for the
footer's core icons and status checks to take less room on a phone.

- `ShellVersionFooter`'s `ViewThatFits` adds one more layout after the
  2.0.73 compact line and before the two rows. The version, the desktop
  light and the MacBook light stay in a row. Next to them, the core and
  runner lights sit 2 pt above the status dots (`stackedLights`). The footer
  stays one line, 28 pt tall.
- In this layout `ShellFleetLights` takes `minHeight: 14`, and
  `ShellStatusDots` takes `minHeight: 10` and `inset: 0`. Both are also
  compact. Elsewhere they keep their defaults: 28 pt and 20 pt, and an
  inset that follows `compact`. They remain two buttons: the lights open the
  machines page, and the dots open Status.
- Two rows are now used only when this layout doesn't fit either, for
  example with the Update capsule showing on a narrow phone.
- Not built or checked on a device from Linux.

### 2.0.74 The chat room pins its transcript to the bottom (2026-10-09, iPhone)

The user reported that in #mesh the newest messages still ended up under the
composer and keyboard when the input opened, even with 2.0.66 in place.

- The channel room (`ChatScreen`) and thread replies (`ChatThreadScreen`)
  now set `.defaultScrollAnchor(.bottom)` on the transcript, as direct
  messages (`MessagesConversationView`) already did. SwiftUI then keeps the
  bottom edge fixed whenever the viewport changes size, so the keyboard
  lifts the last message with the composer as it moves in. Before, the room
  depended on the timing of the `chatKeepsEndInView` scroll alone.
- A short transcript now sits at the bottom, just above the composer, as in
  direct messages.
- `chatKeepsEndInView` stays, so that focusing the input from further up the
  history still jumps to the newest message.
- Not built or checked on a device from Linux.

### 2.0.75 The Shortcuts page always lands on a repository with a list (2026-10-09, iPhone)

The user reported that the phone no longer showed the list of shortcuts.
The page asked the desktop for the repository in use and only moved to one
the desktop has (`ShortcutCatalog.pick`) when the catalog's repository list
changed. The store is shared and the navigator preloads it for the
Shortcuts tile's count, so on a second visit, or on the first once the
preload had run, the list was already the same and never changed. When the
repository in use had no working tree on the desktop, or none was in use,
the page stayed on that repository with no entries and an empty picker.

- `ShortcutsStore.load(preferred:)` asks for the preferred repository, or
  straight away for the one the last desktop list picks for it, and fetches
  the picked repository again if the answer does not have it. The page and
  the navigator's preload both use it, so the tile's count matches the page.
  This is what Android's `ShortcutsScreen` already does after each load.
- The page no longer re-picks when the repository list changes; choosing a
  repository in the picker fetches it unless it is the one shown.
- The note "The desktop has no working tree of …" still shows above the
  list of the repository picked instead.
- Not built or checked on a device from Linux.

### 2.0.76 A glass top bar the agents scroll under (2026-10-09, iPhone)

The user marked up a screenshot of the Agents tab: the project logo in a
round Liquid Glass button with no project name, the diagnostics charts at
the top centre, a clear top bar that the agents scroll under up past the
clock and the notch, an oval of glass around the status emoji and the
avatar, the tool row without its band and lines with each chip on glass, the
runner accounts in the usage chip, quieter eagle rows and a narrower footer.

- `ShellTopBar` has no background. `ShellProjectSlot` is only the project
  icon, clipped round, in a 44 pt glass circle (`shellGlass`, a generic
  shape version of the footer's glass: `glassEffect` on iOS 26, thin
  material before); its accessibility label still names the project. The
  avatar and status emoji share one glass capsule. `TelemetryStrip` sits in
  the middle of the bar, centred, with 64 pt kept clear on each side.
- `HomeShell` lays the top bar and the connection and runner banners over
  the tab (`topChrome`) instead of above it, and measures their height. While
  a tab root or a route page is on screen (`TabStackTops`, now observable,
  is not covered) and no shell page is open, the tab is not pushed down: its
  stack reaches the top of the screen as it does under a bare page (2.0.13),
  and `shellPageClearance()` adds the bar's height as top safe area through
  `ShellDock.top`. Scroll views then start under the bar and scroll under it
  and the status bar. Pages pushed with a `NavigationLink`, which may not add
  the clearance, still get the old layout with the tab below the bar.
- The Agents tool row is the list's top `safeAreaInset`, so rows scroll under
  it; the band and its two hairlines are gone and `AgentToolChip` is glass.
- Runner accounts moved into the usage sheet (`RunnerAccountsPanel`, see
  [RUNNER_AGENTS.md](RUNNER_AGENTS.md#runner-accounts-on-ios-2026-10-01)).
  The usage chip is now only three tiny vertical bars (`AgentUsageBar`), no
  percentage: the active provider's 5-hour window, its weekly window
  (`UsageProviderMeter.sessionWindow`, `weeklyWindow`) and Claude's usage
  credits (extra usage). The window bars keep their green, amber and red;
  the credits bar is blue, amber from 80 % of the monthly limit and red at
  it (`UsageCredits.usedPercent`, `state`), and an empty track when credits
  are off or the limit is unknown. Its spoken label reads all three.
- The usage sheet and the desktop usage card show usage credits as a meter
  row like the windows (`UsageCreditsRow`): "$3.50 of $20.00 (18%)", or
  "$4.00 used, no monthly limit". Claude Code always has the row, reading
  "Off, or not reported by the desktop" when the desktop sends no `credits`
  (it sends them, in major units, only while extra usage is on).
- Eagle rows lose their raised background and get a 0.5 pt rule above and
  below, overlapping their neighbour's (`AgentLineRow.ruleWidth`). The line
  under the title drops the host, branch and folder glyphs; the files
  changed count and the green and red churn bars sit small at the title's
  top right (`AgentEagleStats`). The age is gone too; the line holds only
  the dialog strip. Compact, zen and full rows keep their age.
- Footer: the status dots always overlap and are ringed, and the core and
  runner glyphs overlap too, each on a small page coloured tile, with room
  kept for a runner's agent count.
- Not built or checked on a device from Linux, which only parses the Swift.

### 2.0.77 Back buttons clear of the logo; a glass prompt with a close button (2026-10-09, iPhone)

After 2.0.76 the project logo and the avatar capsule covered the back button
on every page pushed through the router (a repository, a run, the agents
list): those pages are tab stack pages, so the top bar floated over them, and
the system navigation bar sits at the top of the stack, where the extra top
safe area from `shellPageClearance()` does not reach.

- `HomeShell.floatsChrome` now also needs the selected tab's path to be
  empty. Only a tab root lets its list scroll under the glass bar; any pushed
  page gets the layout from before 2.0.76, the stack starting below the bar,
  so its navigation bar and back button are never under the logo.
- New Issue no longer paints its navigation bar `bgRaised`; it uses the
  system bar, which is Liquid Glass on iOS 26.
- The prompt card is Liquid Glass on iOS 26 (`PromptCardSurface`,
  `glassEffect(.regular)`); before iOS 26 it keeps the frosted fill, stroke
  and shadow.
- `PromptComposer` takes `onClose`; the shell's composer passes
  `composer.close()`, which draws a small round x at the card's top right,
  beside the header, so it never covers the context line.
- The context line names a linked desktop as "studio (desktop)"
  (`QuickAgentLauncher.destinationLabel`), so it says where the run goes.
- The mode chip ("Auto", "Plan" …) shows only its icon in a round button and
  Fix is always the round wrench, with no word; both keep their VoiceOver
  labels. This replaces the `ViewThatFits` of 2.0.31a.
- Touching and holding the red Listening button stops dictation and turns off
  Settings, Voice, "Listen when the prompt opens"; a grey line in the card
  says so and how to get it back. VoiceOver has a named action for it.
- Not built or checked on a device from Linux, which only parses the Swift.

### 2.0.78 Every screen checked for overlap and glass (2026-10-09, iPhone)

The user asked to go through each screen. Every view in `Features`, `Kit`,
`Navigation`, `Shell`, `Platform` and `App` was checked against four rules:
nothing under the top chrome, a navigation bar, the status bar or the bottom
bar; no opaque navigation bars; chrome bands become glass; sheets show the
system glass sheet on iOS 26. Cards, rows, bubbles, code and diff blocks,
page canvases and full-screen media stay as they were.

- Bottom overlap was the common fault. Only router routes get
  `shellPageClearance()` from `HomeShell`; pages pushed by a `NavigationLink`
  or a view's own `navigationDestination` did not, so their last rows,
  footers and inputs sat under the floating bottom bar (the gap 2.0.33
  describes). Every such push site in the app now adds
  `.shellPageClearance()` to its destination.
- Sheets: `fmSheetStyle` sets its background through `fmSheetBackground`,
  which applies `presentationBackground` only before iOS 26. Sheet roots that
  painted `bgBase`, `bgRaised` or `bgOverlay` use `fmPreGlassBackground`, the
  same colour before iOS 26 and nothing on it.
- Shared bars in Kit are glass: `FmBottomActionBar` is a glass panel with a
  margin, `FmBottomBar` a glass capsule, and `FmSegmentTabBar` loses its two
  hairlines and raised band, its tabs on a glass rounded rectangle.
  `FmPanelHeader` has a minimum height instead of a fixed one.
- Bands that became glass: the Code tab's search and filter header (now the
  list's top inset, so repositories scroll under it), the issue filter bar,
  the diff review toolbar, find bar and health strip, the branch diff and
  review selection bars, the board notice, the CRM header, tab strip and bulk
  bar, the chat room header and topic line, unread banner and mesh debug bar,
  the channel search field, the new message and share composers, the local
  repository banner, breadcrumb, commit composer and editor status bar, the
  tasks people row, the telemetry range bar and log filters, the on-device
  chat approval bar and setup note, the host log status, runner and desktop
  merge failure notes, the Home search field, the sign-in header's back
  button and the status editor's close button.
- Centre titles no longer take the full width (`NewIssueScreen`,
  `PullChangesScreen`, `BranchDiffScreen`) and terminal titles are one line,
  so they cannot crowd a back or Close button.
- The run page's toolbar actions sit in a horizontal scroller at most 184 pt
  wide (`AgentRunActionsCap`); a runner run with every action used to grow
  over the back button.
- The Cove spreadsheet editor's bottom toolbar, which sat under the shell's
  bottom bar, is a row of glass buttons in a bottom inset.
- Left as they are, on purpose: the code editor's tab header, status bar and
  key bar, diff file headers, the DM header and composer `.bar` band (the
  "server copies" line needs it), the chat drawer and iPad channel column,
  the project drawer, the Cove save-error strip and the Mark Up editor's
  dark bar.
- Not built or checked on a device from Linux, which only parses the Swift.

### 2.0.79 Back to the agent list without a jump (2026-10-09, iPhone)

The user saw the navigation bar jump when going back from an agent to the
agent list. The list (a tab root) hides the system navigation bar, and the run
page hides the shell's top bar (`bareShellTop()`, 2.0.8). Both came back
only once the back slide had finished: `ShellRootBarHider` hid the navigation
bar in the transition's completion, and the run page's `onDisappear`, which
gives the top bar back, fires after the pop. So the slide ended with the run
page's bar still over the list, then the bar vanished, the logo and avatar
appeared and the tool row and rows moved up.

- `ShellRootBarHider` now knows its tab. When a navigation transition is
  bringing its own page back (the coordinator's `to` controller is the root
  page), it hides the navigation bar at the start, animated with the
  transition, instead of at the end.
- At the same moment it calls `ShellChrome.returnToRoot(on:)`, inside a
  `withAnimation` as long as the transition, so the top bar fades in during
  the slide while the departing page still counts as bare. `isBare` is false
  for a tab that is returning; the mark clears when the last bare page
  disappears, when a new bare page appears, or in `leave`.
- A back swipe that is cancelled puts both back: `stayOff(_:)` clears the
  mark, so the run page is bare again, and the navigation bar is shown again
  if the hider had hidden it. The completion check of 2.0.19 still runs as a
  backstop.
- Not built or checked on a device from Linux, which only parses the Swift.

### 2.0.80 The channel list opens full screen (2026-10-09, iOS and Android phones)

The user asked for the chat channel list to go all the way full screen when
it opens.

- The phone drawer (2.0.2) takes the whole width instead of about 85% (at
  most 360 pt/dp): iOS `ChatScreenBody.channelDrawer` fills the frame,
  Android `ChannelDrawer` uses `fillMaxSize()`. It still slides in from the
  left and starts under the top bar on iOS (2.0.11).
- With no dimmed strip left to tap, it closes by swiping it left, picking a
  room, or the system back (Android). Wide layouts keep the fixed sidebar.
  Swiping was removed in 2.0.96 for a close button.

### 2.0.81 A thread's reply box docks on the bar (2026-10-09, iPhone)

The user sent a screenshot from 1.4.1 (20) of a chat thread with its
"Reply..." box, the plus and the send button under the Liquid Glass capsule
and the version line, and asked for the chat input to stop hiding behind the
bar.

- That build was cut before 2.0.78. The room pushes a thread with its own
  `navigationDestination(item:)` (`ChatScreenBody.pushed`), a closure push
  that had no bar clearance (the gap 2.0.33 describes), so the reply footer
  sat on the screen's bottom edge. 2.0.78 gave that push the clearance, which
  puts the reply panel above the new agent button like any pinned bottom
  control (2.0.26), while the room's own composer sits below that button, on
  the bar.
- `ChatThreadScreen` now docks the composer with `.shellDock` (2.0.25), like
  the room (2.0.65): it sits `ShellDock.gap` above the capsule, the new agent
  button rises above it and the transcript ends above it. Docking places it
  the same way whichever push opened the thread. With the keyboard up or on
  iPad it sits at the page's bottom as before.
- The reply box is the room's single glass line (`ChatComposerBar(glass:
  true)`) instead of the `FmBottomActionBar` panel, and the "Replying as
  <name>" line is gone so the dock stays one line tall, as in the room. The
  guest sign-in notice takes the same docked place.
- Checked in the iPhone 17 Pro simulator (iOS 26.5) with a hardware keyboard
  attached, so not with the software keyboard up.

### 2.0.82 No toasts: feedback shows inline (2026-10-09, iPhone and iPad)

The user asked for no toasts anywhere in the app, with the information shown
inline where it belongs. The floating snackbar (`FmSnackBarData`,
`.fmSnackBar`, the inverted capsule that sat over the bottom of a page and
went away after four seconds) is gone from `Kit/FmOverlays.swift`, with its
palette colours (`inverseSurface`, `onInverseSurface`, `snackBarAction`).

- `FmNotice` (message, `isError`, optional action such as Undo) and
  `FmNoticeLine` replace it: an icon and a line of text in the page's own
  layout, secondary colour for results and the danger colour for failures,
  an optional action button and a small dismiss button. No overlay, no
  animation, no timer: the line stays until the next action replaces it, it
  is dismissed, or the page goes.
- Each screen puts the line where its action happened: under a form or button
  row, at the top of a list, under a card's controls, above the chat composer.
  The tools (Calories, Water, Steps, Sleep, Stretch, Sugar Break, Speech),
  Notes, Tasks, local repositories, organizations, pings, settings cards,
  agents, terminals and chat screens all do so.
- Core models publish the sentence instead of a toast: `ToolsNotice` and
  `notice` on the tool models, `ToolsOutcome.notice`, `LocalNotice` on the
  local repository models, `actionError` on the organization models,
  `notice`/`noticeIsError` on `NotificationsScreenModel` and
  `LocalNewAgentModel`, `saveError` on `PingSettingsModel`.
- The collaboration screens share `CollabMessenger.shared`; each shows it with
  `CollabNoticeLine()` in its flow (under the header or the pull request's tab
  picker), so a composer that closes still reports on the screen below. The
  issue board's move result moved from a glass panel pinned to the bottom to a
  line under its column picker.
- Messages that belong to no page (signed out, a link that would not open,
  "Spider hidden" with Undo, the report-bug thanks, a prompt target's result,
  the iPad's agent-started line) show in the shell's notice line under the top
  bar, next to the offline banner, in the layout and not over the page.
- The agent delete Undo is `AgentUndoDeleteLine` at the top of the Agents tab
  list and of "All agents" (DESKTOP_AGENTS.md).
- System notifications (push, Live Activities) are not toasts and are
  unchanged.

### 2.0.83 Home's clear search button clears again (2026-10-10, iPhone)

The user reported that the ✕ at the right end of Home's search field
(`shell-home-search-clear`) did nothing on the phone.

- The whole capsule carried the tap that focuses the field, so the ✕ sat
  under a competing tap gesture, and since 2.0.78 also under interactive
  glass, which treats the capsule as one pressable control. The focus tap now
  covers only the magnifier, the field and the leading padding; the ✕ and
  the sliders menu are outside it.
- The capsule's glass is not interactive (`shellGlass(_:interactive:)`, which
  defaults to interactive for every other caller): it holds a text field and
  two controls, not a single control.
- Not built or checked on a device from Linux, which only parses the Swift.

### 2.0.84 Home icons the size of the bar's, count pills, no Recent row, pull-down search (2026-10-10, iPhone, issue 1889)

The user asked for the phone's Home icons to look nice at about the size of
the bottom bar's icons, with proper notification number circles or ovals,
for the most recent icons to go, and for search to open on a pull down.

- **Size.** Home icons are 20 pt like the bar's (`ShellBottomBar.iconSize`)
  in 56 pt tiles, six across, with 3 pt between icon and label. 2.0.68's fit
  to one screen stays but only shrinks tiles to 44 pt, and the icon shrinks
  with the tile, never grows. 2.0.63's heat growth is gone
  (`ShellBarOrder.tileIcon`, `ShellHome.heatGrowth` and `labelRoom` removed)
  so every icon is the same size; the most-used glow stays.
- **Count pills.** `ShellBarCellLabel(pillCounts: true)` (Home and its drag
  preview only; the bar is unchanged) draws an unread badge as a filled blue
  capsule (`fm.count`) with white rounded bold `caption2` digits, at least
  18 pt round, and a plain count as the same capsule in a 16% blue tint with
  blue digits. Both sit on a 1.5 pt ring in `fm.bgBase` over the icon's top
  right corner and grow to the right as before; the bounce on a new badge
  stays.
- **No Recent row.** 2.0.64's Recent section is gone, with
  `ShellBarOrder.recentRow`. `ShellBarPrefs.recent` is still kept: the
  Recently used sort and visit counting read it.
- **Pull-down search.** The search capsule (with the icon settings menu in
  it) is hidden. Pulling Home down 56 pt past its top
  (`ShellBarOrder.searchPull`, `revealsSearch`) slides it in with a
  selection haptic and focuses it; it hides again when it loses focus
  empty, when ✕ clears it without focus, and when Home is left empty. The
  pull is measured as the grid's global top less the scroll view's top plus
  its top safe-area inset, and Home now always bounces vertically
  (`.scrollBounceBehavior(.always, axes: .vertical)`), so the pull works when
  everything fits. With VoiceOver on, the field is always shown.
- Android has no Home grid of this kind and is unchanged.
- Tested in `HomeShellViewModelTests`. Not built or checked on a device from
  Linux, which only parses the Swift.

### 2.0.85 One People row per person in chat (2026-10-10, iOS and Android)

The user reported their own account listed twice under People on the phone:
"jett (you)" with the phone's node id and "jett" with the desktop's, each
with an `id …` line because the names collided. People rows are grouped by
an identity key (`RelayService.identityKey` on iOS, `identityKey` in
`chat/MemberGroups.kt` on Android). Your own node always got the key `self`,
so your other signed-in devices, keyed `owner:<account>`, could never join
your row.

- Your own node now uses `owner:<account>` when you are signed in, as the
  desktop's chat member list already does (`groupKeyFor` in
  `MainWindowMessages.cpp` keys your node by `accountOwner()`). Your desktop,
  laptop and phone show as one "(you)" row with one device icon each. A
  guest device, with no account, keeps `self` and stays its own row.
- The #mesh room built one row per node and did not group at all. It now
  uses the same grouping (`RelayService.groups` on iOS, `memberGroupsOf` on
  Android, which `meshMemberGroups` calls), so a person seen on two devices
  nearby is one row there too.
- Android's grouping moved out of `RelayRoom` into `chat/MemberGroups.kt` so
  the relay roster and the mesh room share it. Tests: `MemberGroupsTest`
  (Android) and `RelayServiceTests` (iOS).

### 2.0.86 Every Home tile is a plugin, off in new projects (2026-10-10, iOS and Android)

The user circled almost every Home tile and asked for each to be a full
plugin, off by default in new projects on desktop, mobile and web, with
dependent plugins nested. Repos, Search, Market, Settings and Workflows stay
core. The package list, the server rule and the desktop and web sides are in
[docs/marketplace/core-plugins.md](../marketplace/core-plugins.md). The
feature code stays in each app; the package is its switch, its Market page
and its install state. Deep links and direct routes keep working, as `/status`
did in 2.0.72: a removed package loses its entry points, not its screens.

**iOS**

- One gate, `PackageGate` (`FMApi/Navigation/PackageGate.swift`), maps every
  Home, bar, repo and hub item id (`Agents`, `Inbox`, `Chat`, `currentRepo`,
  `reportBug`, `repo-pulls` ... `repo-discussions`, `teammates`, `localAI`,
  `orgs`, `world`, `designSystem`, `status`, `signIn`/`signOut`, the personal
  and workspace tiles), each `Route.Kind`, each repository section, the
  Agents/Inbox/Chat tabs and the Chat/Issues/Pull requests/Commits/Code search
  buckets to its package id. The app publishes the set of effectively
  installed gated packages (`PackageGate.publish`, from `PackageGateSync` in
  `HubGroups.swift`) whenever the account answer or catalog changes
  (`AccountPackagesStore.onChange`), the organization installs are reread, a
  device switch flips, or Home draws. Nothing published (widgets, tests)
  shows everything.
- Effective state (`AccountPackagesStore.isInstalled`): the account's row
  from `/api/plugins/@account` wins, unless the entry says `legacy` (the
  server only answered installed because the account predates
  `installedBefore`); otherwise `default`, or installed when
  the Market row has `installedBefore` and this project is legacy; a gated
  package the catalog has never listed is installed only in a legacy
  project; an unknown non-gated package (a status check) still counts as
  installed. Every dependency must be installed too, also when the account
  has a row (pull-requests needs branches, special-agents needs agents).
  `installedBefore` is parsed from the Market rows into `MarketPackage` and
  `AccountPackageInfo` and kept in the cached catalog
  (`packages/account/catalog`). A phone has no project creation date, so any
  `installedBefore` date counts for a legacy project.
- The legacy marker (`ProjectLegacyMarker` in FMAccount) is written once per
  project, under the project's own key (`packages/legacy-project` in the
  legacy namespace, `p/<id>/packages/legacy-project` for added projects), by
  `SettingsService.create` at launch and on first read after a switch, and
  never changes. It says legacy when any of these exists: the per-project
  keys `profile/name`, `server/room`, `profile/solana`, `nodeSync/applied`,
  `node/catalogNodeId`, `agent/branch-records-v1`,
  `localControl/pairedDesktops`; a stored session (`auth/session/<id>`) or
  guest mode (`auth/guestMode/<id>`) for the project; and, for the original
  (legacy-namespace) project only, the shared keys
  `packages/account/state`, `packages/account/catalog`,
  `packages/status-page/monitors`, `packages/design-system/on`,
  `packages/tour/on`, `shell/bar-layout`, `shell/menu-layout`,
  `shell/bar-uses`, `shell/bar-recent`, `shell/bar-sort`,
  `appearance/themeMode`, `appearance/accent`, `bugReport/anchor`, the legacy
  `auth/session` and `auth/guestMode`. Migration keys written on every
  launch (`server/localWorkerUrlMigrated`, `server/urlEnv`) do not count. The
  marker is not one of `ProjectKeyPolicy.projectPreferenceKeys`, so
  `NextProjectRetirement`'s "untouched legacy project" check is unaffected.
- Gated surfaces: the Home grid and the bottom bar (the `main`, repository
  and hub groups of `HomeShell.barGroups`), the iPad sidebar (tabs and hub
  links), the You hub and its Recent rows, global search (destinations and
  the Chat, Issues, Pull requests, Commits and Code buckets), the command
  palette (suggestions, recents, tabs, destinations: "New agent" needs agents,
  "New note" notes, "Start a timer" timers), Genie's routes and app actions
  (`GenieToolbox.destinations`, `entries`), Home Screen quick actions
  (`ShellQuickAction.availableActions`, and the recent-agent item), the
  floating and bar New agent button (agents), the floating bug button
  (bug-reports), the first-run tour (opens only when tour is installed) and
  the footer dots (status-page, unchanged) and the top bar Glasses radar
  (glasses-alert). The Business group shows CRM only
  when an organization you belong to has it installed, like Marketing,
  Analytics and Competitors (`PackageRoutes.hubRoutes`). App Intents and
  App Shortcuts are static and not gated; they open routes, which keep
  working.
- Tour and Design lose their per-phone switches (`packages/tour/on`,
  `packages/design-system/on` are no longer read; `SettingsService.tourOn`
  and `designSystemOn` are gone); both follow the account, like Status Page.
- Sign in is the one per-device package: `PhonePackages.signIn`, stored in
  `packages/sign-in/on`, default from the legacy marker (legacy on, new off).
  Off hides the guest's Sign in chip (`ShellTopBar.showsSignIn`) and the
  hub's Sign in / Sign out items. The top bar avatar (the guest's person
  icon) always shows, since it is the way into the hub (2026-10-13: the user
  reported the avatar missing from the top bar of a new project).
  Settings, Account always keeps Sign in (the guest card) and Sign out (the
  Sign out card); those cards are not gated, so a new user can sign in with
  it off.
- Market: every package of the list shows with "Installed on all your
  devices" (account) or "Installed on this device" (Sign in, with a footer
  pointing at Settings). `MarketPackage.nativeDestinations` now covers the
  new packages, so they say "On this phone" and get "Open" where a route
  exists (Agents, Inbox, Chat, Desktops, Special Agents, Shortcuts, Hosts,
  Network, Coves, On-device AI, Activity, Stats, Organizations). Children
  nest under their first dependency ("Includes N packages", Pull requests
  under Branches, Special Agents under Agents). Removing always asks first,
  also for a nested child, and names what goes with it; installing a child
  names the parent that comes back.
- Tests: `PackageGateTests` (FMPackages: new project hides everything, legacy
  project keeps `installedBefore` packages, the account answer wins, the
  dependency rule, unknown packages before the catalog loads, account +
  device + organization, hub filtering, item and route tables),
  `PackageGateSearchTests` (FMApi: search destinations and buckets, palette,
  quick actions) and `SettingsServiceTests` (the marker and Sign in default).
  Not checked on a device.

**Android**

- Gate: `core/.../market/PackageGate.kt` maps hub ids, route kinds, the
  Agents, Inbox and Chat bar tabs and the repo tiles to package ids; the Code
  bar button follows `code`. Repos, Search, Market, Settings, Workflows,
  Projects, Set status and Ads are never gated (Workflows is gated since
  2.0.90). `PackageSurfaces` turns the
  installed state into the hidden sets.
- `AccountPackagesStore.isInstalled` follows the same order as iOS (account
  row unless it says `legacy`, then `default` or `installedBefore` on a
  legacy project, unknown
  packages by the legacy flag, dependencies required). Its cache now lives in
  each project's own prefs instead of one shared answer.
- Legacy marker `packages/legacyProject`, written once per project by
  `PackageGateFeature` right after `AccountFeature`: legacy when the project's
  prefs already hold `packages/account/state`, `packages/account/catalog`,
  `packages/status-page/monitors`, `nav/tour.v1`, `nav/archive.v1`, the old
  Tour or Design switch, `profile/name`, `server/room`, `node/serve`, a saved
  session or guest mode.
- `PackageGateFeature.surfaces` (a StateFlow fed by the account store, the
  Sign in switch and organization installs) replaces the Status-only boolean
  and filters the hub sheet and its recents, the shell menu, the bottom bar
  and side rail (Repos always stays), Ctrl+1..4 and Ctrl+N, the new agent
  button and running-agent accessory, global search (destinations and the
  issues, pulls, commits and chat results), the command palette, launcher
  shortcuts (republished on change), the repo tiles (hidden ones skip their
  counts) and the repo screen's Agent and Issue buttons. When the open tab
  is removed the shell moves to Agents, or to Repos.
- CRM, Marketing, Analytics and Competitors show only when an organization of
  the user's has them installed (checked at most every 10 minutes in the
  foreground and on sign-in, cached, refreshed by the Market).
- Tour and Design follow the account; the first-run tour shows only with Tour
  installed and is not marked seen otherwise. Sign in is a per-project phone
  switch defaulting from the legacy marker; off hides the Sign in and Sign
  out items and the guest banner; the top bar avatar always shows. Settings always keeps Sign out, and Log in and Create
  an account for guests.
- Market: the account switch with a removal confirmation for every package,
  nested children included; Sign in has "Installed on this phone"; packages
  with a phone screen say "On this phone" with **Open**.
- Not hidden: the Agents and Inbox home-screen widgets and the New agent
  quick-settings tile, which the user places. Routes stay registered.
- `:core:coreTest` (2098 passed, including `PackageGateTest`) and
  `:app:compileDebugKotlin`; not run on a device. Details in
  [ANDROID.md](ANDROID.md) section 13.

### 2.0.87 The Inbox loads under a thin bar (2026-10-12, iPhone, issue 1931)

The user sent a screenshot of the grey skeleton rows that filled the Inbox while it
loaded, sitting above rows that had already arrived, and asked for a fast,
thin horizontal bar instead.

- While the Inbox has not loaded, the list shows one indeterminate
  `FmLinearProgressIndicator` under the filter chips (2pt, one sweep every
  0.8 s, identifier `inbox-loading`) in place of the four `FmSkeletonRow`s.
  This replaces the skeleton rows from 2.0.17.
- `FmLinearProgressIndicator` takes an optional `height` and `period`; the
  defaults (4pt, 1.4 s) keep every other use as it was.
- Not built or checked on a device from Linux.

### 2.0.88 The new agent button sits left of the avatar, chat backgrounds show through (2026-10-13, iPhone, issue 1939)

The user sent a screenshot of a direct message with the floating new agent
button over the bottom bar and the message field a gap above it, and asked
to fix the button to the left of their avatar as a full button with a
sparkle effect driven by the accelerometer, to sit the message field at the
bottom, to make the chat page's background transparent and to allow custom
background pictures and pictures for people and groups.

- **Fixed, labelled button.** `ShellNewAgentButton`
  (`Shell/ShellNewAgentButton.swift`) is now a 44 pt accent capsule with the
  sparkles glyph and "New agent". It sits just left of the avatar in
  `ShellTopBar`, the chat room header and the direct message header
  (`ChatShellActions.onNewAgent`/`onPalette`). Where the label does not fit
  (`ViewThatFits`) it drops to a 44 pt circle. Tap opens the composer, a long
  press the command palette, as before. The bottom bar and the iPad's
  `floatingButtons` no longer carry it, and dragging and tucking it (2.0.62)
  is gone with `NewAgentButtonSpot` and `ShellNewAgentPlace`; the
  `shell/new_agent_*` keys are no longer read.
- **Sparkles.** `ShellSparkleTilt.shared` reads the accelerometer at 30 Hz
  while at least one button is on screen and the app is active (reference
  counted, stopped on disappear or when the scene leaves `.active`). A
  low-pass of the reading is the tilt; what is left over is a jolt that
  briefly brightens and grows the sparkles and bounces the glyph.
  `ShellSparkleField` draws 14 four-point stars in a `Canvas` at up to
  30 fps: they slide with the tilt by depth, drift slowly and twinkle, and a
  white sheen and the fill's gradient lean with the phone. Reduce Motion
  keeps the stars still and the accelerometer off; Reduce Transparency draws
  a flat accent capsule without them.
- **Message field at the bottom.** The direct message view docks its
  composer with `shellDock` like the room view, so it sits `ShellDock.gap`
  above the bar's capsule instead of above the page clearance and the
  button's old row.
- **Transparent chat page.** The direct message header and composer lost
  their `.bar` fills (the header keeps a light top scrim for its buttons),
  so the backdrop runs from the top of the screen to behind the bottom bar.
  The room view draws `MessagesBackdrop` behind the whole page and its
  transcript, intro and notice line no longer paint `fm.bgBase`.
- **Custom backgrounds in direct messages.** Background in the + menu and
  Details now works in end-to-end direct messages too, kept on this phone
  only (see `docs/FMChat.md` in `native/ios`).
- **Pictures for people.** Details of a direct message has Set/Change/Remove
  Picture, and each person in a group's People list has the same in its
  context menu. `MessagesPeoplePictures` keeps them as JPEGs (512 px, at most
  160 KB) in Application Support `chat-people-pictures/`, named by the
  base64url of the lowercased name; `MessagesPersonAvatar` and
  `ChatSenderAvatar` show one over the account avatar. Only this phone sees
  them. Group photos are unchanged (shared `look` photo).
- Not built or checked on a device from Linux: the app sources were only
  parsed; the FMChat tests ran with `swift test`.

### 2.0.89 A starry new agent circle, pocket mode and Diagnostics in the avatar's menu, which drops down (2026-10-13, iPhone)

The user asked for the new agent button to be just a circle whose stars match
the agents running, for it to look more magical, for the slide-to-unlock
(pocket mode) button and the Diagnostics switch to move into the avatar's
menu, and for that menu to slide down from the avatar instead of up from the
bottom.

- **A circle, one star per running agent.** `ShellNewAgentButton` is always a
  44 pt circle (scaled with Dynamic Type) with a plus glyph; the "New agent"
  label and `ViewThatFits` are gone. `ShellSparkleField` takes `stars`, which
  is `ShellCounts.shared.agents` (the running agents the Agents tab reports),
  and draws that many four-point stars, at most 24. The seeds sit in a ring
  outside the glyph (golden-angle spacing) so one star is as visible as
  twenty. No agent running means no stars. VoiceOver reads the count as the
  button's value ("3 agents running").
- **More magical.** The stars orbit slowly by depth, twinkle and carry a soft
  halo; a star that appears because an agent started blooms large for a
  moment. A slowly turning ice-and-rose aurora (screen blend) and a turning
  iridescent rim sit under and around them, a shooting star crosses every
  6.5 s while any agent runs, and the accent glow under the button grows with
  the count (up to 8). Tilt and jolts work as in 2.0.88. Reduce Motion stops
  the motion but keeps the stars; Reduce Transparency drops the aurora, sheen,
  comet and rim and keeps the stars on a flat accent circle, since the stars
  are now information.
- **Pocket mode and Diagnostics in the hub.** The top bar's lock button
  (`ShellPocketButton`) is removed. The You hub has a section under the
  header with "Pocket mode" (closes the hub and opens the lock screen) and
  the Suggestion mode switch. The Diagnostics switch is back in the hub's
  top-leading toolbar slot (`hub-diagnostics-strip`), where it was before
  2026-10-10, and left Settings > Profile; the profile still shows the
  version and status footer while Diagnostics is on.
- **The hub drops down from the avatar.** `HomeShell` no longer presents
  `YouHubSheet` as a sheet. `hubDropdown`, an overlay above the Genie layers,
  shows it as a rounded panel anchored top-trailing just under the avatar
  (`HomeShell.hubDrop`, 54 pt below the safe area; at most 380 x 640 pt)
  over a light scrim that closes it on tap. `ShellHubUnroll` reveals it from
  the top edge down while it grows from the avatar's corner; Reduce Motion
  fades it. `YouHubSheet` takes `onClose` instead of the environment's
  dismiss, and its title is inline.
- Not built or checked on a device from Linux: the app sources were only
  parsed.

### 2.0.90 Plugins choose where they show up; Workflows is a plugin (2026-10-13, iOS and Android)

The user circled Vault, Calories, Steps, Water, Sleep, Sugar Break, Stretch
Break, Food Guide and Workflows on Home and asked that they be plugins, off
in new projects, and installable on mobile, desktop and web or only some of
them. The server side is in
[core-plugins.md](../marketplace/core-plugins.md#2026-10-13-where-each-plugin-shows-up).

**iOS**

- `PackagePlace` (`mobile`, `desktop`, `web`; this phone is `mobile`).
  `AccountPackageEntry` reads and caches `places` and `availablePlaces`;
  missing or unknown values count as all three. Market rows read `places`.
- `AccountPackagesStore.isShownHere`: installed and placed on the phone (a
  package without an account entry counts as placed; a dependency only has
  to be installed). `installedGatedPackages`, which feeds `PackageGate`,
  uses it; `statusPageShown` drives the status dots (`statusPageOn` still
  drives monitor data).
- A `legacy` entry is installed only when the server says installed and the
  project is legacy, whatever `installedBefore` is, so Vault and the other
  set-up-only data packages stay out of new projects.
- `PackagesClient.installForAccount` takes optional places;
  `setAccountPlaces` posts `/places`; the store's `setPlaces` /
  `setPlace(_:_:on:)` refuse an empty list without a call.
- `PackageGate.items["workflows"] = "workflows"`: the Workflows hub item is
  its only entry point (it opens the web page), so it now leaves Home, the
  You hub, the iPad sidebar and the bar with the package.
  `MarketPackage.nativeDestinations` lists it.
- Market: `placesSection` ("Show on", a Phone / Desktop / Web toggle per
  available place; the last one disabled; "Not shown on this phone. Turn on
  Phone to show it here.").
- Tests: `PackageGateTests` (`testALegacyHookInstallFollowsTheProject`,
  `testPlacesParseFromTheAccountAndTheMarket`,
  `testAPackageShowsOnlyWhereItIsPlaced`,
  `testWorkflowsIsGatedLikeTheOtherPackages`), `AccountPackagesTests`
  (`testPlacesChangeThroughTheAccount`). App screens only parsed with
  `swiftc -parse`, not checked on a device.

**Android**

- `PackagePlaces` (`MOBILE`, `DESKTOP`, `WEB`, `HERE = MOBILE`, `parse`,
  `label`); `AccountPackage.places`, `availablePlaces`, `placedHere`;
  `MarketPackage.places`.
- `AccountPackagesStore.shows(id)`: installed and placed on the phone.
  `PackageGateFeature.isInstalled` and `StatusPageFeature` use it. The
  `legacy` rule is the iOS one. `setPlaces` posts `/places`; a 409 reads
  "Install this package first."
- `PackageGate.WORKFLOWS` gates the `workflows` hub item, so Workflows is no
  longer in the never-gated list.
- Package page: `PackagePlacesSection` ("Show on", one switch per available
  place, the last one disabled).
- Tests: `PackageGateTest` (`testPlacesAreParsedAndAnOlderServerMeansEverywhere`,
  `testAPackageShowsHereOnlyWhenItsPlacesHaveMobile`,
  `testALegacyEntryIsInstalledOnlyInALegacyProject`,
  `testWorkflowsIsAGatedPackage`), `AccountPackagesTest`
  (`testSetPlacesPostsToThePlacesRoute`). Not run on a device.

### 2.0.90 Plugins belong to the project (2026-10-13, iOS and Android)

The user created a new project and Home still showed Agents, Inbox,
Shortcuts, the Personal tiles and Workflows; they asked for plugins to be
per project. The rule is in
[core-plugins.md](../marketplace/core-plugins.md#2026-10-13-plugins-belong-to-the-project).

**iOS**

- `ProjectLegacyMarker.markNew`: `RelayDirectory.add` (a new instance) and
  `addNamed` (New project) write the marker as `new` for the added project,
  so the sign-in that follows can no longer settle it as legacy. An already
  written marker is never changed. `NextProjectRetirement.used` skips the
  marker key.
- `AccountPackagesStore` takes `projectNamespace` (the active project's
  namespace, from `SettingsService.scope`) and keeps the project's choices
  under `<namespace>packages/project-installs` (`projectChoices`, cached per
  namespace, `projectRevision` for observation). `ownsInstalls` is true in a
  new project. There `isInstalled` is: chosen and the account still has it;
  removed here never; no choice only for a `default` package the account has
  not removed. A legacy project is unchanged.
- `install` in a new project records the package and the dependencies it
  brought after the account install succeeds. `uninstall` in a new project
  makes no server call and records the package and its installed dependents
  as removed; `uninstall(_:everywhere: true)` (the Steps and Water erase
  panels through `PackagesFeature.removeFromAccount`) also removes it from
  the account.
- Market: in a new project the switch reads "Installed in this project", the
  footer "Your other projects install it on their own. Its settings and where
  it shows stay with your account.", and the confirmation "Remove X from this
  project?" / "Your other projects keep it."
- Tests: `PackageGateTests`, `SettingsServiceTests` (see core-plugins.md).
  The app screens were only parsed with `swiftc -parse`.

**Android**

- `ProjectsService.add` (a project not in the list yet) and `addNamed` call
  `PackageGate.markNewProject` on the project's own scoped preferences
  (`packages/legacyProject = false`), so a stored session cannot make it
  legacy later.
- `AccountPackagesStore` (built per project by `StatusPageFeature` on the
  project's scoped preferences) reads `packages/projectInstalls` into
  `projectChoices`; `perProject` is true in a new project, where
  `isInstalled` follows the iOS rule. `install` records the package and the
  dependencies it brought after the account install; `uninstall` posts
  nothing and records the package and its installed dependents as removed;
  both return those ids as `changed`.
- `AccountPackageGraph.removeNote`, `installNote` and `changedNote` take
  `project` and say "this project"; the Market switch reads "Installed in
  this project" with the shared footer, and the remove dialog "Remove X from
  this project?" / "Your other projects keep it."
- Tests: `PackageGateTest`
  (`testTheMarkerForACreatedProjectIsNewEvenWithAStoredSession`,
  `testANewProjectShowsOnlyWhatItInstalled`,
  `testAnAccountInstallFromAnotherProjectDoesNotShowHere`,
  `testALegacyProjectStillFollowsTheAccount`; five older cases now record
  project choices), `AccountPackagesTest`
  (`testInstallInANewProjectRecordsThePackageAndItsMissingDependencies`,
  `testRemovalInANewProjectMakesNoServerCallAndLeavesTheAccountInstalled`,
  `testProjectNotesNameThisProject`). Not run on a device.

### 2.0.91 Home icons with more care, higher counts, Home screen settings (2026-10-13, iPhone)

The user sent a Home screenshot and asked for nicer icons, slightly bigger
labels, the count pills a little higher at the top right, settings to show
and hide the categories, many sorting options and a way to turn off the
frequent-use highlight.

- **The default look stays plain.** 2.0.47's coloured glossy icons were
  reverted at the user's request (2.0.49) and 2.0.63 removed the tile
  backgrounds, so the default is still a monochrome glyph with no tile. It is
  drawn by `ShellHomeIcon` (Home and its drag preview; the bottom bar keeps
  `ShellBarCellLabel`): a 22 pt glyph (was 20) in medium weight,
  hierarchical rendering, a top-to-bottom gradient of the primary text colour
  and a 1 pt soft shadow, centred in a square box 1.75 times the icon. This
  keeps glyphs of different widths on one optical grid. Labels are 12 pt
  medium (was 10) and may tighten and shrink to 75%. Tiles are 66 pt high
  (was 56), and the fit to one screen of 2.0.68 floors them at 50 pt. A
  press springs the tile to 90% (`ShellHomeButtonStyle`).
- **Five across by default.** At 12 pt, six across shrank the long labels
  ("Organizations", "Stretch Break") back to 10 pt, so Home now shows five
  across. The Home rows in the screenshot take the same number of rows at
  five as at six. Icons per row is a setting (4, 5 or 6).
- **Counts higher, at the top right.** The pills of 2.0.84 hang from a
  zero-size anchor (`ShellHomeIcon.countAnchor`) instead of an alignment
  guide. The screenshot showed the old guide being ignored, so wide counts
  ("1,840", "2,401") spread left over the glyph. The pill's bottom-left
  corner sits 30% of the icon in from the glyph's right edge and 6 pt below
  its top, so it grows to the right and its centre sits about 4 pt above the
  glyph's top. Unread pills have a gentle gradient, a hairline and a soft
  glow in `fm.count`; plain counts have a tinted capsule with a hairline. The
  Agents heartbeat count mirrors this on the left.
- **Icon styles** (setting): Outline (default), Filled (`.symbolVariant(.fill)`
  where a fill exists), Accent (glyphs in the Settings accent) and Tiles (a
  rounded continuous tile with a raised-to-sunk gradient, a top highlight
  hairline and a drop shadow, with the badge on the tile's corner as on
  iOS). The tiles only show when chosen. With Tiles, the activity ring of
  2.0.42 becomes a comet that runs around the tile's border
  (`ShellHomeActivityRing`); otherwise it is the round halo as before.
- **Home screen settings** (`ShellHomeSettingsSheet`/`ShellHomeSettingsForm`).
  They open from the sliders menu in the pull-down search ("Home settings…";
  the bar's own settings follow under "Bottom bar"), from a long press on any
  icon or category title, and from Settings → General → Home screen
  (`HomeScreenSettingsCard`). The sheet starts with a live preview of four
  sample icons in the chosen style, then:
  - **Look**: icon style, icons per row.
  - **Sort icons**: Custom order (the drag order), Name A–Z, Name Z–A, Most
    used, Least used, Recently used, Needs attention (unread badges first,
    biggest first, then icons with something running) and Biggest count.
    Ties keep the custom order. Dragging an icon onto another on Home
    switches back to Custom order, as a placing drop does for the bar.
  - **Categories**: Group by category (off: one grid sorted across
    everything, with no reordering by drag, so an icon cannot silently move
    to another category), Show category titles (off: a small gap between
    categories), Category order (usual, A–Z, most used, most icons) and one
    switch per category with its icon count. A hidden category leaves Home
    but its icons still show in a Home search and stay in the bottom bar.
    A long press on a category title or an icon offers **Hide <category>**.
    When everything is hidden, Home shows "Every category is hidden" with a
    button to the settings.
  - **Highlights**: Highlight frequently used (the glow of 2.0.63, now a
    soft accent circle behind the glyph, or a tint on the tile, instead of a
    blob across the whole tile) and Show counts (plain counts; unread badges
    always show).
  - Clear usage history, Reset Home order, Reset Home settings.
- **Storage.** `ShellHomeStyle` (FMApi) holds the settings as JSON under
  `shell/home-style`. The default writes no key. Each field falls back to
  its default by itself, so an older or partly broken save still loads.
  `ShellHomeStyle.arranged` does the hiding, the category order and the
  sorting on plain `ShellHomeSection`/`ShellHomeEntry` values.
  `ShellHome.arranged` maps them back to bar items. `ShellBarPrefs.home`
  and `styleHome` keep it observable. `ShellBarPrefs.homeGroups` is the list
  of categories Home last drew, which the settings list. Opened from Settings
  before Home has drawn, the list falls back to Go to plus the hub groups.
  Tested in `ShellHomeStyleTests`.
- Android has no Home grid of this kind and is unchanged. Not built or checked
  on a device from Linux, which only parses the Swift.

### 2.0.92 Touch, movement, drawing and a shared background in direct messages (2026-10-13, iPhone)

The user asked to tap the other person so their phone buzzes, to share the
accelerometer and watch the other person move while chatting, to share the
background, and to draw to each other on the background.

- **Touch button.** A hand button sits right of + in the direct message
  composer. It opens a full-screen touch canvas over the conversation
  (`MessagesLiveTouchSurface` in `Features/Chat/Messages/MessagesLiveTouch.swift`)
  with a hint line at the top and a glass toolbar at the bottom: eight ink
  colours, Share my movement, Clear drawing and Done. The button is filled
  with the accent while the canvas is open or movement is shared.
- **Tap.** A touch that moves less than 8 pt is a tap: ripples on both
  screens where it landed and a heavy-then-rigid knock on the other phone
  (`DirectLiveHaptics`, honours the Haptics setting). A light tick confirms
  your own.
- **Draw.** A drag draws a glowing stroke behind the messages on both screens
  (`MessagesLiveInkLayer` in the backdrop). Strokes fade after 45 s; Clear
  wipes both screens.
- **Movement.** Share my movement starts Core Motion (device motion at
  20 Hz, `DirectLiveMotionFeed`). The other person sees a card in the top
  right corner with a phone that tilts, turns and shakes like yours
  (`MessagesLivePartnerMotion`). Sharing stops when the conversation closes,
  the app leaves the foreground or the account signs out.
- **Background.** Background in + and Details is now shared in direct
  messages, end-to-end encrypted; both screens say so.
- Transport, limits and the relay change: `docs/app/chat-private-rooms.md`,
  "Touch, movement and drawing in direct messages". Not built or run on a
  device from Linux: the app files were parsed only; the FMChat tests ran with
  `swift test`.

### 2.0.93 The Market: search above the bar, every sort and filter, usage on flat cards (2026-10-13, iPhone)

The user asked to move up what sat behind the navigation, for every sort and
filter the data allows, usage numbers and a one-tap Install on each card,
flat cards split by hairlines, and an installed state that matches reality.

- **Behind the bar.** `MarketScreen` used `.searchable` with the default
  placement. On iOS 26 an iPhone puts that field in the bottom toolbar, the
  same strip the shell's floating Liquid Glass bar covers (§2.0.16), so the
  search field sat under the capsule; the All / Installed / Not installed
  picker was the first row of an inset `List` and scrolled away under the
  top chrome. Search is now `.navigationBarDrawer(displayMode: .always)`,
  under the title, and the sort and filter chips are the scroll view's top
  `safeAreaInset` (`MarketControls`, on `.bar` material with a hairline), so
  rows scroll under them. The page still takes the bar clearance from
  `shellPageClearance()`, and the list ends with 16 pt of padding above it.
  Not checked in the simulator: a cold Debug build on the office Mac did not
  finish in the time available.
- **Sorts** (`MarketSort`): Featured (catalog order), Name, Top rated, Most
  installed, Most downloaded, Most used, Most opened, Installed first. Ties
  keep catalog order. There are no release dates in the catalog, so no
  Newest.
- **Filters** (`MarketFilter` for installed or not, `MarketFilters` for the
  rest): category, runs on (this phone, web, desktop, World), installs for
  (account, organization, this phone, desktop), and under More: kind, price,
  publisher, verified only, rated only. Clear resets them. Search also
  matches the publisher.
- **Cards** (`MarketCard`): no background or grouped inset; a full-width
  `Divider` between rows. The preview is a fixed 112 pt 16:9 thumbnail (the
  catalog's previews are all 16:9), or a placeholder of the same size, so
  every row lines up. The stats line (`MarketStatsLine`) shows the rating,
  installs (`installs + accounts + organizations` from
  `/api/market/installs`, or downloads when there are none), and people who
  use it (`usage.people` from `/api/market/reviews/summary`), and drops the
  extra tags when the row is too narrow. The listing API changes are in
  [../marketplace/README.md](../marketplace/README.md#market-numbers-and-fields-for-the-phones-2026-10-13).
- **One-tap Install** (`MarketActionButton`, `MarketModel.cardAction`):
  Open when installed with a phone route or web page, Installed otherwise;
  Install for an account package (asks first only when dependencies come
  along), a per-device package, a package hidden on this phone, or the one
  organization you manage; a menu when you manage several; Get (to the
  package page) when you must sign in, create an organization or ask an
  admin; Desktop (or World for World-only packages) for packages only a
  desktop installs, as on Android. A spinner replaces
  the button while it works, and a failure shows in red under the card
  (`MarketModel.installErrors`).
- **Installed state.** Three causes, fixed in `MarketModel`:
  1. A package that is neither account-synced nor per organization counted
     as installed whenever it was `bundled`, and every catalog package is
     bundled, so Winamp, the IDEs and the banner boards read Installed.
     `bundled` only means the App host can activate it; the desktop installs
     it on first run when `install.default` is true, so that is what the
     phone now shows.
  2. The list and the package page each kept their own copy of the
     organizations' packages, so installing from the page left the list
     stale. Both now read `OrganizationPackagesLedger.shared`, which
     `HubPackageState.refresh` also fills and sign-out resets. An
     organization that fails to load keeps its last state and says so.
  3. An account package removed from the phone before it was account-synced
     stayed hidden from Home (`settings.removedPackages`) while the Market
     said Installed, and no screen could undo it. The Market now reads it as
     not installed here; Install, or **Install on this phone** on its page,
     clears the flag.
  Also: package ids with dots (`nemotron-3.5-lightning`) were dropped by
  `PackagesText.isPackageId` and are now kept.
- Tested in `MarketListTests` (sorts, filters, card actions, each state
  bug) and the updated `MarketTests`; all ForkMeshCore tests pass on Linux.
  The app files were only parsed.

### 2.0.94 People: everyone around the campfire (2026-10-13, iOS and Android)

The user asked for a phone page showing members' avatars and names in rings by
engagement, like the World campfire, with a profile that offers Follow and
Message, as a plugin on every platform. Route `.people` (`Route.People` on
Android), URL `/dashboard/people`, menu App group after Organizations, gated
by the `people` package. The screen is a campfire with five rings (Blazing,
Glowing, Kindling, Warm, Embers), a row of ring chips that focus one ring, and
a card per ring listing everyone in it; a sheet shows the profile with Follow
(`PeopleProfileModel.toggleFollow`, `POST`/`DELETE /api/accounts/<name>/follow`)
and Message (`.dm(user:)`, `Route.Dm`), behind `requireAccount` for guests.
Rules, layout and the shared fixture: [People](../marketplace/people.md).

### 2.1 Behaviour of the tab row (WS3 builds it, WS8 checks it against this list)

- **Selecting and reselecting.** Tapping a tab always shows its root
  screen: if pages are pushed on its stack they are popped, whether the tab
  was selected or not (the user asked on 2026-09-30 to go all the way up to
  the top page from any inner page). Tapping the selected tab at its root
  scrolls to the top. On iOS this is `AppRouter.select`, which the bar, the
  iPad sidebar, ⌘1–4 and the command palette all use; deep links go through
  `AppRouter.open` and keep the stack. On Android it is `HomeShell.select`
  with `reselect = true`, used by the bottom bar and the wide-screen rail;
  keyboard shortcuts, the command palette and deep links pass
  `reselect = false` and keep the stack.
- **Minimizing.** The row minimizes while a list scrolls down, the way
  `tabBarMinimizeBehavior(.onScrollDown)` does, and comes back on scrolling up.
  The prompt button never hides.
- **Hiding.** The row stays visible on pushed screens, per HIG for tab-based
  hierarchies. It hides only under a full-screen cover (the Watch field, the
  on-device model download) and while the keyboard is up. Under the prompt
  composer it fades out and the page ignores the keyboard, so nothing moves
  behind the sheet. For a page's own field (the chat composer, a comment box)
  the row leaves the layout (`HomeShell.pageKeyboardUp` on iOS, the IME inset
  in `HomeShell.kt` on Android) and the page ends at the keyboard, so the
  field stays in view.
- **Over the pages on iPhone, below them on Android.** On iPhone the row
  floats over the page area and every page takes its height plus a margin
  as bottom safe area (§2.0.16, §2.0.20, §2.0.21): lists scroll under the
  glass and clear of it, while a page's bottom controls end that margin
  above the row's top edge (issue #729). On Android the row
  is the last item of the shell's column and pages end at its top edge. The
  iPhone's first inset (issue #729) sat on the whole column and content was
  reported running under the tab buttons; the safe area now sits on each
  tab's stack, a page's bottom controls are laid out inside it, and it is
  zero while a page's field has the keyboard. On Android the page box also consumes
  the navigation-bar inset that the bottom menu already pads for, so pages
  that add that inset themselves (the repository page's floating button, a
  field's `imePadding`) no longer count it twice.
- **Chat.** The Chat tab shows its own message field above the row, as the
  channel screens do; the prompt button still aims the composer at the
  channel.
- **Implementation.** Prefer the system `TabView` with four `Tab`s, and put the
  prompt button beside it as a `.glassEffect` button in a shared
  `GlassEffectContainer`. That gives the system bar's accessibility, badges and
  the iPad sidebar adaptation. If the system bar cannot sit left-aligned to
  leave room, keep WS3's custom row but meet everything in §10: the tab trait,
  "Tab, 1 of 4", badges read by VoiceOver, the large content viewer at
  accessibility text sizes, and 44 pt targets.
- **Running-agent accessory.** A compact glass capsule sits above the row
  while at least one run is active, in the manner of
  `tabViewBottomAccessory`. It shows the model face, the run title, the step
  and elapsed time. A tap opens the agent. With several runs it reads
  "3 running" and opens Agents > Running. It disappears when nothing is
  running.
- **Badges.** Counts come from the account event channel (the `agents` and
  `pings` topics). Badges show at most "999+". The app icon badge equals the
  Inbox badge.
- **Launch sparkle.** Starting an agent from the composer shows no snackbar.
  Tapping Send makes the up-arrow button pop, and a glowing star charges
  on the arrow with a ring and orbiting sparks while the run is sent. The
  tap also takes a picture of the whole composer card. When the run is
  accepted, the composer closes and its picture takes its place: it
  shrinks in place to about 120 pt wide (0.32 s), then swings sideways
  and down into the Agents tab (a quadratic curve that starts level),
  shrinking to about 14 pt and rounding into a pill. It has an accent
  glow and a twinkling trail, then bursts at the tab (0.95 s in all). If
  no picture was taken (the composer was not on screen, or the run
  started from somewhere else), the star flies instead: it leaves the
  arrow, which puffs as it goes, climbs in a big arc (a cubic curve,
  peaking about 270 pt above the higher of the two ends and kept below the
  top of the screen), drops into the Agents tab and bursts there. If the
  send fails, the charge fades out and the composer stays. The charge
  starts at the tap so the effect reads as one motion, even though the
  flight has to wait for the server. As it lands, the
  Agents count goes up by one with a pop, and the icon bounces. The run's
  message ("Queued.", "Started agent #12 on studio.") is announced to screen
  readers instead of being shown. The count cannot wait for the run list to
  reload, so the shell holds a floor of (count at launch + 1) for 12
  seconds; the real count replaces it once the list catches up. With reduced
  motion, the count goes up with no flight. iOS:
  `Shell/ShellLaunchSparkles.swift` (`ShellLaunchFlight`), with the Agents
  cell of `ShellBottomBar` as the target. The picture is the window's
  pixels inside the composer's frame (`PromptScreenCapture.region`,
  drawn before the tap's own update), clipped to the card's 28 pt
  corners. The iPad sidebar has no Agents
  count, so the iPad shows the message in the shell's inline notice line
  under the top bar (it was a snackbar until 2026-10-09). When the agent dots bar is
  showing (§4, Agent dots), the target is its next free seat instead
  (2026-10-01, the user asked for the prompt to shrink up to the left into
  the row of agents): the picture shrinks in place, then rises and swings
  left into the seat (the curve's control point is 85% of the way up, 15%
  across), squaring off into a 21 pt circle, and a spinning placeholder
  agent appears there as it lands. The star-only flight follows the same
  curve. If the bar is hidden (switched off, or the Chat tab), the flight
  goes to the Agents tab as before. Android:
  `shell/LaunchSparkles.kt` (`LaunchFlight`); the target is the Agents item
  of the bottom menu or the navigation rail. The shell's agent composer
  draws itself through a graphics layer (`rememberGraphicsLayer`), and
  `LaunchFlight.capture` turns that layer into the picture, so only the
  composer is in it. The Send button's bounds
  come from `PromptComposer(onSendFrame:)` (iOS) and
  `PromptComposer(onSendBounds =)` (Android). Only the shell composer
  passes them, so other composers get no pop. Android's Agents badge counts
  runs waiting on you, in red. When nothing is waiting, it counts running
  runs, in the accent colour. The local desktop screen's "Start on desktop"
  uses the same flight.

### 2.2 The prompt button

| Gesture | Result |
|---|---|
| Tap | The composer sheet opens at once, with the keyboard up, aimed at the current context (see below). This is WS3's requirement |
| Long press | The **command palette** (§5.2), with a light haptic |
| Drag up (optional) | The composer opens at its large detent with dictation started |

Context rules, from the most specific to the least:

1. an open agent session steers that session;
2. an open PR or issue starts an agent on that repository with the item
   attached;
3. an open repository starts an agent on that repository;
4. an open chat channel posts a message there;
5. anything else starts an agent on the last-used repository, with a chip to
   change it.

The composer always shows its target as an editable chip, so the result is
never a surprise.

### 2.0.95 Chat pictures without a flash (2026-10-13, iOS and Android)

The user saw a slight flash on the chat page: each avatar drew its letter
circle, then swapped to the person's picture a moment later, and asked for it
to load faster or not flash.

- **Why it flashed (iOS).** Every avatar took two async hops after it
  appeared: `ChatSenderAvatar` and `MessagesPersonAvatar` read the picture in
  a `.task` (even from `ChatAvatarCache`), then `FmAvatar` decoded it in a
  second `.task`. So each row painted the initial for a frame or more, on
  every visit. The cache lived in memory only, so the first visit after a
  launch also waited on the network for every person.
- **First frame paints the picture.** `FmAvatar` decodes through
  `FmAvatarImages` (an `NSCache` of decoded images keyed by the base64, 256
  entries) inside `body`, so a picture seen before is drawn at once and a new
  one is decoded once. `ChatAvatarCache` is `@Observable`; both avatar views
  read it in `body` and only refresh it in their `.task`.
- **Kept across launches.** `ChatAvatarCache` saves the pictures (newest 200
  people, empty answers not kept) to Caches `chat-avatars.json`, written off
  the main thread a second after a change. `ChatFeature.register` warms it
  off the main thread at launch, so the chat list and transcript open with
  the pictures from the last session; the server's answer then replaces any
  that changed.
- **Offline keeps the saved picture.** `ApiService.accountAvatarPngIfReached`
  returns nil when the account read failed with an `HTTPError` (no route,
  timeout), so a saved picture is not cleared; a missing account or no
  picture is still "". `accountAvatarPng` is unchanged for other callers.
- **Android.** `ApiService.knownAccountAvatarPng` returns a picture already
  fetched this session without blocking; `ChatSenderAvatar` and the DM row's
  `DirectAvatar` start from it, so returning to the chat page no longer
  flashes. Android does not keep the pictures across launches yet.
- Tests: `ApiServiceTests.testAccountAvatarTellsAnUnreachableServerFromNoPicture`
  (iOS core), `ApiServiceTest.testKnownAccountAvatarIsReadWithoutARequest`
  (Android core). The app sources were only parsed on Linux, not run on a
  device.

### 2.0.96 The Chat tab reopens the last conversation; drawers only move by their buttons (2026-10-14, iOS and Android)

The user asked for the Chat button in the bottom bar to land on the last
channel or direct message they had open instead of #general, and for the
slide-out panels (the chat room list and the project list) not to move when
swiped: only the buttons that open and close them may move them.

- **Last conversation.** The open conversation lived only in memory
  (`RelayService.currentConversation` / `RelayRoom.conversation`, starting at
  `#general`), so after the phone dropped the app the Chat tab came back on
  #general. Each switch is now saved per project and restored the first time
  the chat screen appears in a launch.
  - iOS: `ChatHub.select` writes `ChatWorkspace.lastConversation`
    (`chat/workspace/<project>/lastConversation`); `ChatScreenBody.onAppear`
    calls `ChatHub.restoreLastConversation()`, which reopens it through
    `ChatHub.open` (re-adding a `#channel` the relay list no longer has).
  - Android: `RelayRoom.onConversationSwitched` (every switch, including the
    ones push and inbox make on the room directly) writes
    `chat/lastConversation` under the project's `ProjectScope.key`;
    `ChatScreen` calls `ChatDepth.restoreLastConversation()` once.
  - A conversation chosen before the screen appears (a notification tap, a
    deep link) wins: any switch marks the launch as chosen and the restore
    then does nothing.
- **Drawers do not follow a swipe.** The drag gestures are gone from the chat
  channel drawer (iOS `ChatScreenBody.channelDrawer`, Android
  `ChannelDrawer`) and the project drawer (iOS `ProjectDrawer`, Android
  `ProjectDrawerHost`). A swipe over either one now scrolls or does nothing.
  - The project drawer still closes from its scrim, picking a project, its
    actions, VoiceOver escape and Android Back.
  - The iPhone channel drawer is full width and hides the room header (and
    its open button) while out, so it gains an **×** button (`Close
    conversations`) at the start of its search row
    (`ChatChannelSidebar.onClose`). Android's menu button stays visible above
    its drawer and toggles it, as before.
- Tests: `ChatNewMessageTests.testTheChatTabReopensTheLastConversation` (iOS
  core). The app sources were parsed (iOS) and compiled (Android) on Linux,
  not run on a device.

### 2.0.97 Slack-style channel room, channel details, link previews and @here/@everyone (2026-10-14, iOS)

The user sent two Slack screenshots (an OWASP channel on iPhone and its
channel details sheet) and asked for the phone's channels to look like them,
for previews of popular links like Slack's YouTube card (for 50 kinds of site,
plus any site's own social image), for the room header to show the total number
of members instead of who is online, for every feature of the details sheet,
and then for `@everyone` and `@here` tags that work as in Discord and Slack.

- **Header.** `ChatHub.headerDetail` for a channel is now
  `ChatHub.channelDetail`: "346 members • 4 tabs". Members are
  `ChatHub.channelMembers` (below); tabs count Messages and Files & Links, plus
  Bookmarks when there are any and the canvas when there is one. Direct
  messages and #mesh keep their old line. Tapping the pill opens the channel
  details sheet for a channel (`ChatHub.isChannel`); a relay direct message
  still explains its encryption. The back circle, New agent button and avatar
  stay (2.0.2, 2.0.66).
- **Who counts as a member.** Relay `#channels` are labels inside one
  encrypted room, so the relay cannot list or count a channel's members (it
  stores ciphertext only). The phone counts everyone it knows is in the
  channel: the people online in the room (by account, then name) and everyone
  who has posted in it or its threads, without ForkBot, apps or transient
  guests. Office and private D1 rooms use the server's member list
  (`OfficeRoom.members`). The Members page says which rule applies.
  Since 2.0.100 `#general`, `#random` and `#welcome` also count every member
  of the project's account directory.
- **Messages.** 38 pt rounded-square avatars (`FmAvatar(corner:)`), the sender's
  name bold in the text colour with the time beside it, 17 pt body. Reactions
  are larger capsules with an add-reaction button after them
  (`ChatAddReactionButton`). A red **NEW** line sits above the first message
  that was unread when the room was opened (`ChatHub.unreadStart`, set in
  `select` from the unread count before it is cleared; `ChatHub.firstUnreadId`
  counts back over other people's messages). A message that mentions you,
  `@here`, `@channel` or `@everyone` gets a yellow wash and a left bar, and the
  mention itself is highlighted (`ChatSmartMentions.split`,
  `chatRichAttributed(own:mentions:)`, `\.chatMentionNames`); other `@names`
  are bold in the accent.
- **Composer.** "Message #channel", a round **+**, and a microphone when the
  field is empty in a room that takes files: it records a voice message with
  the conversations' `MessagesVoiceRecorder` and sends it as
  `Audio Message.m4a` through `sendAttachment`. The send button returns as soon
  as there is text.
- **Link previews** (`ChatLinkPreviews`, `ChatLinkPreviewCard`): up to three
  cards under a message, in the Slack layout: title, author or description,
  the site's badge and name, then the picture (16:9 with a play button for
  video). Tap opens the link; long-press copies or shares it. Details are in
  `native/ios/docs/FMChat.md` (Link previews).
- **Channel details sheet** (`Channels/ChatChannelSheet.swift`,
  `ChatChannelPages.swift`): × close, the lock or # and the name, the members
  and tabs line, and a ⋯ menu (star, notification level, mark as read, copy
  name, message retention, leave). **Add** invites online people to a private
  relay room, opens the channel settings of a private Office room the account
  manages, or explains that a public channel is already open to everyone (with
  Mention someone and Notify @here). **Move** stars the channel or moves it to a
  sidebar group (`ChatChannelGrouping`, the sidebar's `ChannelGroups`), or a new
  one. **Search** opens the search sheet with `in:#channel`. Rows: Messages
  (closes the sheet), Files & Links (every file and link in the loaded
  history, filterable, with previews and a share button), Bookmarks (links
  saved to the channel plus messages saved for later from it), Add canvas /
  the canvas (a titled note per channel, autosaved, can be posted to the
  channel), Members (count, search, presence, message or view a person),
  Agents & Apps (ForkBot and the project's app commands; tapping puts the
  command in the composer), Notifications (All, Mentions, Nothing) and
  Settings & Details (name, type, member count, topic, the encryption summary,
  retention, the link preview switch, Office channel settings, leave).
- **Notification level.** `ChatNotifyLevel` per channel in `ChatWorkspace`.
  Nothing is the existing mute. Mentions keeps the unread dot off unless an
  unread message mentions you or uses a broadcast tag (`hasUnreadMention`).
  Like mute it is local: the relay pushes every message of a room to its phones
  and cannot read them, so the push banner is not filtered.
- **@here, @channel, @everyone.** Typing `@` offers them first in a channel
  ("@here — notify everyone online … (3 online)"), and the + menu has Notify
  the channel. `@here` reaches the people online, the other two every member.
  Sending one to six people or more asks first ("Notify 12 people?",
  `ChatHub.broadcastWarning`), as Slack does. They count as mentions for the
  Mentions level, the Mentions search and the highlight. Inside backticks they
  are plain text.
- Bookmarks, canvases and notification levels are stored per project on this
  phone (`chat/workspace/<project>/bookmarks|canvases|mentionsOnly`), like
  stars and mutes; other clients do not see them.
- Tests: `ChatLinkPreviewTests`, `ChatSmartMentionTests`,
  `ChatChannelInfoTests`, `ChatHubTests.testTheRoomHeaderNamesTheRoomAndWhoIsIn`
  (iOS core, Linux). The Android app was not changed.

### 2.0.98 Bare logo and avatar, square agent tools, glass run buttons (2026-10-14, iOS)

The user asked for the top-left project logo without its round glass button,
a little larger; the avatar as a rounded rectangle, off its glass capsule,
with the status emoji at its bottom right; the agent sort chip without the
word "Recent"; the desktop queue's "1/10" as a square that shows the slots;
the agent run page's actions as separate small glass buttons; and a
follow-up sent to a running agent to show at once.

- `ShellProjectSlot` draws `ProjectIcon` at 40 pt in its own rounded square,
  with no `shellGlass`, in a 44 pt tap target. The no-settings fallback logo
  is 40 pt with no glass too.
- The top bar's avatar is `AvatarWithDot(size: 36, rounded: true)`: a
  rounded rectangle (corner 28% of the side) with no glass. The status emoji
  is a badge at 42% of the avatar's side over its bottom-right corner, with a
  faint shadow so it reads on any photo. With an emoji the connection dot
  moves to the top right; without one it stays bottom right. The You hub's
  avatar keeps the round style.
- The Agents tool row's sort chip is a 30 pt square: a tiny symbol for the
  current order (`AgentLineSort.symbol`: clock Recent, play Running, half
  circle Status, exclamation mark Priority, text Name, folder Repo, plus-minus
  Changes) above the sort arrows. The menu shows each order's symbol.
- `DesktopQueueMenu`'s label is `DesktopQueueSlots`, a 30 pt square: one small
  rounded cell per slot in a near-square grid (10 slots are 4, 4 and 2), the
  running ones in the accent colour, with an amber dot at the corner while
  agents wait. Above 25 slots it shows "running" over "limit" in small type.
  The accessibility label still reads the queue summary.
- `AgentRunScreen`'s toolbar actions (terminal, retry, continue, stop, move to
  desktop, PR, merge, update, fix conflicts, clean up, delete) each sit on
  their own 32 pt rounded-rectangle glass (`AgentRunActionButtonStyle`):
  accent icons, red for stop and delete, dimmed when disabled. On iOS 26 the
  toolbar's shared capsule is hidden behind them, as it already was for the
  branch diff button.
- A follow-up sent from the run page used to appear only once the desktop or
  runner wrote it into the transcript, which for a running agent can be after
  its current turn. `PendingPrompts` (FMControl) now shows the sent text as a
  prompt block straight away and drops it when the transcript carries it (a
  prompt or steer block containing the text, counted against how many did
  before sending, so a repeated "continue" waits for its own echo). A failed
  send takes it back and returns the text to the composer.
  `PendingPromptsTests` covers it (iOS core, Linux).

### 2.0.99 The Agents tool row is centred (2026-10-14, iPhone)

The user asked for the icons under the top bar on the Agents tab to be
centred. `AgentsScreen.tabBar` used to pack `tools` to the left with a
trailing `Spacer`; it now gives the row the full width
(`frame(maxWidth: .infinity)`), so the chips (stop all, sort, list view,
queue, usage or runner accounts, terminal, recycle bin, watch, on-device
chat) sit centred whichever of them are shown.

### 2.0.100 Every member is in #general, #random and #welcome (2026-10-14, iOS)

The user asked for every ForkMesh member to be in the general chat and the
random chat. The phone's general chat page should show the full member count,
and a new `#welcome` channel should greet each new member and ask everyone to
say hi.

- **Members.** `ChatEveryone.channels` is `#general`, `#random` and
  `#welcome`. In the project's shared room (default room, no room passphrase,
  `ChatHub.sharedRoom`), `ChatHub.channelMembers` adds every account from
  `/api/accounts/users?view=world` (`refreshMemberDirectory`, the same list as
  People and the web chat's people pane). It still adds the people online and
  everyone who posted, so the count, the Members page and the "N members • N
  tabs" header now cover everyone. The directory is read at launch, on sign-in,
  when the Members page opens, on resume (at most every five minutes) and after
  every join. A passphrase room or another room keeps the old rule.
- **No leaving.** The three channels are on every phone (`RelayService.channels`
  starts with all three). The Leave action is hidden for them,
  `ChatExtras.removeChannel` refuses them, and a stored "left #random" from an
  older build is dropped when history loads.
- **Welcome note.** The relay already sends a signed, unencrypted
  `forkmesh-new-member` frame to the room when an account is created (the web
  chat shows confetti for it). The phone used to drop it because it is not a
  cipher envelope. `RelayService` now passes it to `onNewMember`, and the hub
  adds a ForkBot message to `#welcome`: "🎉 Welcome to ForkMesh, @name!
  Everyone, come say hi 👋". It counts as unread and files a "New member joined"
  ping. The frame is live only, so a phone that was offline adds the same note
  from the directory for each account created in the last seven days, dated
  when the account was created and without counting it as unread.
- **Same note on every phone.** The note is built on each phone, not sent to the
  room. Its id is `welcome-member-<name>` on every phone, so restores and
  repeats do not duplicate it, and reactions and thread replies on it line up
  across iPhones. A note posted from a client would have been sent once per
  online phone.
- The new member's own "👋 Just joined ForkMesh — hello!" (`ChatWelcome`)
  is unchanged.
- `ChatEveryoneTests` covers it (iOS core, Linux). Android still lists only
  `#general` and `#random` from the relay roster.

### 2.0.58 Tour can be removed from the phone (2026-10-05, iOS and Android)

Reported: "Was unable to remove tour button in app". On iOS the Tour cell's
**Remove Tour…** opened the `tour` Market page, but `tour` is a desktop-only
package with no per-device switch on the phone, so the page offered nothing
to turn off and the button stayed.

- `tour` joins `PhonePackages` on both phones (`packages/tour/on`, default
  on): iOS `SettingsService.tourOn`, Android `AccountSettings.tourOn`. The
  Market page shows "Installed on this device"; turned off, `HubGroups.current`
  (iOS) and `PhonePackages.hiddenHubItems` (Android) drop the Tour item from
  the menu, the bar and the More grid, and the first-run tour no longer opens
  (`HomeShell` on iOS, `ShellFirstRun` on Android).
- `tour` is a native destination (`MarketPackage.nativeDestinations` /
  `NATIVE_DESTINATIONS` → `.tour`), so its Market page reads "On this phone"
  with **Open Tour** instead of "runs on your desktop", and
  `PackageRoutes.packageId(forHubItem:)` no longer needs a `tour` special case.
- The `/tour` route stays registered, so a link still opens it.
- The desktop already removed Tour from the rail's right-click menu
  (**Remove Tour…**) or the Market; unchanged.
- Not checked on a device from Linux.

## 3. Navigation inside a tab

### 3.1 One `NavigationStack` per tab, with typed routes

- Each tab owns a `NavigationStack(path: $router.paths[tab])` of `Route`
  values. `Route` is one `Codable, Hashable` enum listing every destination in
  §8. Gate 2 fixes it as a contract, and WS8 owns it.
- Feature workstreams register destination views in `RouteRegistry` from their
  installers. A route that nobody has registered yet opens a "Coming soon"
  placeholder that names the owning workstream, so the whole sitemap is
  navigable from the start.
- Screens push values (`NavigationLink(value: Route.pull(repo, 12))`), never
  views. There is one `navigationDestination(for: Route.self)` per stack.
- The existing `ShellPage` and `NotificationDeepLink` map onto `Route`.
  `RepoDetailTab` becomes `Route.repo(_, section:)`.

### 3.2 Titles and toolbars

- Tab roots and the hub root use **large titles**. They collapse to inline on
  scroll.
- Pushed detail screens use **inline titles**: the repository or item name,
  with a subtitle where it helps ("forkmesh/app · #412").
- Keep at most three toolbar items per side. Anything more goes in a trailing
  `ellipsis.circle` menu. Primary actions go in the trailing position.
  Destructive actions go only inside menus or confirmation dialogs, never as a
  bare toolbar button.
- Liquid Glass belongs only to bars and floating controls, never to content.
  Toolbars adopt it automatically. Do not add backgrounds behind toolbar items.

### 3.3 Back navigation

- **Never hide or replace the system Back button.** It gives the swipe from
  the edge, iOS 26's swipe from anywhere in the content, the long-press
  history menu (the desktop's history menu, row 1.2) and VoiceOver's
  two-finger scrub.
- Content that scrolls sideways (the side-by-side diff, the board, the
  timeline) must scroll inside its own `ScrollView`, so the system can tell a
  pan from a back swipe. Nothing may attach a full-width horizontal drag
  gesture to a pushed screen.
- Sheets dismiss by swiping down. A sheet holding unsaved input sets
  `interactiveDismissDisabled(hasChanges)` and offers Save or Discard through
  a confirmation dialog.
- There is no Forward button. The history menu on a long press of Back covers
  the desktop's back and forward history.

### 3.4 Repository screens (the desktop's 19 repository tabs)

A tab strip of 19 segments does not fit a phone. Until 2026-09-29 the iOS app
had two repository screens: the catalog opened one with a scrolling tab strip
(Code, Issues, Pulls, …, eleven pages), while `Route.repo(r, nil)` opened a
second one, a plain list of section rows. The user asked for one compact
screen with every option in view and counts on it, and both
were replaced by `RepoDetailScreen`, which every entry point now opens
(`Route.repo(r, nil)`, the catalog, the desktop's repositories, deep links,
agent and ping screens):

1. a header: icon, `owner/name` with the online dot, two lines of
   description, and a meta line (default branch, language, stars, forks,
   mirrors, Public or Private);
2. an action row: **Agent** (the composer aimed at the repository),
   **Issue** (New issue), **Files** (the Code section) and Clone to device;
   the ⋯ menu in the toolbar keeps Fork, Download ZIP, Code map, Issue board,
   Branches and Settings;
3. a three-column grid of every section, each tile with a count badge when
   one is known: Code, Pull requests, Issues, Commits, Branches, Agents,
   Worktrees, Actions, Releases, Discussions, Mirrors, Projects, Security,
   Insights, Artifacts, About, and Settings with admin rights. Shortcuts
   left the grid for the navigator on 2026-10-01 (§2.0.29).
   Counts come from `RepoOverviewModel` (FMCode): open issues and open pull
   requests from the published ledgers (`publishedIssues`, `publishedPulls`,
   the same reads as the Issues and Pull requests pages; the `/issues` and
   `/pulls` GETs are the owner node's signed inbox drain and refuse a session),
   commits, branches and releases from the served reads, mirrors
   from the catalog record; a read that fails leaves its tile without a
   count;
4. a README row.

Each tile pushes `RepoSectionScreen` for its section, which carries the
New issue button the Issues page had in the tab strip. A deep link that
names a section (`initialTab`) pushes that section once on arrival, and one
that names an issue, pull request, discussion or commit pushes it, as
before; going back lands on the repository screen. Pull to refresh reloads
the counts. On iPad the screen is the content column (§7).

The repository list shows the paired desktop's repositories above the
phone's own and the network catalog ("Desktop · <name>"), from the
desktop's `repos.list` control call (`DesktopReposModel`). Neither app had
asked the desktop before, and the catalog only lists public repositories
to a phone, so repositories that live only on the desktop, or are private,
never appeared. Rows show the branch (or "No checkout"), running agents,
Public or Private and "Not published"; the list reloads on the desktop's
`repos.changed` bell. A desktop that predates `repos.list` falls back to
the repositories of its `agents.list` answer; with neither, the section
says why (not paired, another account, needs an update, offline).

The Actions page (and a pull request's checks) asks the desktop too.
The server's `actions/runs` only reads HTTPS mirror nodes, so a repository
whose runs happen on the desktop showed "No node that hosts this repository
is answering right now" while the phone was connected to that desktop.
`RepoActionsModel` now takes a `DesktopActions` (both apps, `ci/` in core)
and calls the desktop's `repos.actions` first. Desktop runs win when it
has any; otherwise the server answers, and the desktop's answer (its
workflows, "No workflow runs yet") stands in when the server has no node.
An `unknown_repo` or an offline desktop leaves the server's answer as
before, and the empty message then says why the desktop could not help.
The desktop reply is mapped onto the server's v2 summary (`path` to
`workflowPath`, `branch` to `refs/heads/…`, `pull` to `pullNumber`,
`triggers` to `events`, `disabled` to `enabled`), so the list, steps and
checks views are unchanged; the header says "Live from <desktop>". The
run page follows its run's log through `repos.actions.log` with the
`since` offset (appending, or replacing on `reset`, capped at 64K
characters), and both pages reload on the desktop's `repos.changed` bell,
which it rings on every step and status change. Run controls from a
desktop answer go through `repos.prepare` then `repos.run` (cancel is
`run.stop`, dispatch is `workflow.run` with a bare branch name). Since
2026-10-02 a workflow's on/off switch is `workflow.enable` or
`workflow.disable` with its `path`; the desktop updates the repository's
`disabledWorkflows`, as its own Actions tab checkbox does, and rings
`repos.changed`. The repository-wide Actions switch stays desktop-only and
is hidden.

Android matches this (`RepoDetailScreen`, `RepoSectionScreen`,
`RepoOverview.kt` and `DesktopRepos.kt` in core). Its two former views were
the tab pager and the separate "Repository tools" list (`RepoToolsScreen`);
both are gone. The grid has 23 tiles: the eleven former pages, Branches,
the former tools (Issue board, Backlog, Merge queue, Code map, Insights,
Security, Webhooks, Download ZIP, Settings, which Android always showed),
and since 2026-10-01 Projects and Milestones. Projects and Milestones open
the planning screens that were reachable only by link before; they came in
with a Shortcuts tile to keep the three-column grid to full rows. Shortcuts
moved to the navigator the same day (§2.0.29), so the last row holds two
tiles (`RepoOverviewTest` checks the count).
Tools that belong to a section (workflow tools, releases and artifacts,
mirror nodes) are buttons in that section's app bar.

### 3.5 State restoration

- Store the selected tab and each tab's path with `@SceneStorage`, using
  `NavigationPath.CodableRepresentation` of `Route` values. Restore on cold
  launch within 8 hours. After that, open the Agents root, with the other tabs
  still restored.
- A route whose object has disappeared (a deleted issue, a finished run that
  was cleaned up) falls back to its parent and says so in an inline line. Never
  show an error page.
- WS3 restores composer drafts (row 1.20). Scroll positions are restored
  where the list supports `scrollPosition(id:)`.
- The You hub sheet is not restored. The destination it opened lives in a
  tab's stack, so it is.
- A project switch rebuilds the service graph (WS1). Paths are stored per
  project, so switching back returns to where you were.

## 4. The You hub

The avatar button in the top-trailing corner of every tab root drops a panel
down from the avatar (since 2026-10-13, §2.0.89; before, a sheet with medium
and large detents) with its own list. Choosing an item **closes the
panel and pushes the destination onto the current tab's stack**, so Back
returns to where you were. On iPad the same items are sections of the sidebar
(§7).

| Group | Items (the DESKTOP_PARITY sections they hold) |
|---|---|
| Header | Avatar, name, status (#69); project switcher chip (WS1 `ProjectSwitcher`); account switcher (14.1) |
| Workspace | Notes (10.1–10.2), Tasks (5.25), Calendar (10.3), Timers (10.4) |
| Operate | Desktops: paired desktops, the LAN desktop screen, the desktop's files, log and health (4.11, 12.4, 12.12, 13.12); Teammates (§3 of DESKTOP_PARITY); Hosts & fleet (§7 of DESKTOP_PARITY); Network & nodes (§8 of DESKTOP_PARITY) |
| Business | Market (11.1); CRM (11.3); Marketing (11.4); Analytics (11.5); Competitors (11.6); Ad review (11.7, ad reviewers only); Ledger (11.8). Only packages enabled for the account appear; the rest are in Market. CRM is the exception and always appears: its screen installs it for an owner or admin and tells other members whom to ask |
| Personal | Vault (11.11–11.12), Coves (16.1), Calories (11.14), Steps, Water, Sleep, Sugar Break and Stretch Break (Pedometer, Water Intake, Sleep, Sugar Break and Stretch Break packages, Apple Health), Glasses Alert (Glasses Alert package: Bluetooth alarm for Meta smart glasses nearby; iOS and Android), Food Guide (Food Guide package: meal ideas from Apple Health nutrition, the calorie budget and ALDI groceries; iOS only), On-device AI (2.33), Activity (#44) |
| Insights | Stats (12.1), Status (1.18, 12.8), Admin (4.3, 8.9, 12.6–12.7), which only admins see |
| App | Settings (§13 of DESKTOP_PARITY), Organizations (13.10), Report a bug (#73), Tour (1.15), World (9.22), Sign out |

The hub shows at most five **recent destinations** at the top, so a tool used
daily is one tap from the hub. Items are never hidden behind a second "More".

On iPhone the sheet shows only the header, the recents and an **Account**
group, because every other link lives in the bar and on Home (§2.0,
`YouHubSheet.linksInBar`). Since 2026-10-09 that group is Settings, then Sign
in or Sign out: the user sent a screenshot of the sheet with Sign out alone
and asked for Settings there as well. It is the `HubCatalog.app` Settings
item, so it opens the Settings page on the current tab like any hub link, and
it stays in the bar and on Home too. The same user asked for a setting that
turns sounds off; that switch already exists, Settings, General, **Sounds**
(§6.5). Checked in the iPhone 17 Pro simulator (iOS 26.5).

A **Diagnostics** switch sits in the hub's top-leading corner (the sheet's
toolbar on iOS, the first row on Android). It shows or hides the telemetry
strip, the row of metric charts and log lights (on iOS, under the project name
in the top bar), so the strip can be put away without opening Settings. The choice is stored per device
(`diagnostics/showStrip`, on by default) in `SettingsService.showTelemetryStrip`;
hiding the strip does not stop telemetry sampling. The strip is the only way
into the Diagnostics screen, so turn it back on here to open it.

**Send diagnostics** (iOS) lives in the shell prompt, not the hub: it is an
item in the composer's "+" menu and the `/diagnostics` slash command. Until
2026-10-02 it was a clipboard button beside the hub's Diagnostics switch; the
copied report was pasted into an agent task by hand, and that day's paste
arrived cut ("[cut at 4000 chars]") with the newest slow events, the stall log
and the app log lost. Now the report goes straight into the prompt and the
prompt switches to starting an agent (as `/agent` does on a page with its own
target). An empty prompt gets a lead line asking the agent to find and fix what
makes the app slow or crash; text already typed stays as the lead. The report
is budgeted to what the agent task can still take (`ApiService.maxAgentPromptChars`,
8,000 UTF-16 units, minus the lead), and at most `PerformanceReport.maxChars`
(4,000). When the prompt has no room for `PerformanceReport.minChars` (1,000),
the composer says so instead.

The report carries what is needed to chase lag without a debugger: build,
device model, thermal state, low power mode, memory headroom, display rate and
the current screen; each telemetry metric summarised over 5 m and 1 h; the top
slow events by name; the operations in flight or just finished during stalls;
the top HTTP endpoints over the last five minutes; two MetricKit reports, each
with the top eight frames of the attributed thread (`binary+0xoffset`, which a
dSYM symbolicates); the newest slow events; the busiest log lines; the last 10
minutes as a per-minute series; the tail of `diagnostics/stalls.log` (so stalls
from earlier launches are included); the last 10 app log lines; and log volume.
Sections run in that order, most useful first.

When the report runs over its budget it trims itself rather than being cut at
the end. Every section has a rank; the lowest ranked lose lines first, all of
them down to three lines, then to one, then to none, and each trimmed section
ends with "... N lines trimmed to fit" (the stall log and app log drop their
oldest lines and say so first). Every heading survives, so the next reader sees
what was there. A hard cut remains only for a header that alone exceeds the
budget. The text is built by FMTools' `PerformanceReport` and gathered by the
app's `PerformanceReportBuilder`. Request query strings are never logged, so
the report carries no tickets or tokens.

Two measurement faults found in the same report are fixed with it. The frame
source (`DisplayLinkFrameSource`) kept its last display timestamp across
backgrounding, so the first callback after returning to the app counted the
whole time away as one late frame: that report's "frame" total was dominated by
a single 85,667 ms frame. It now forgets the timestamp on resign active, enter
background, enter foreground and become active. And MetricKit's call stack
tree, kept in `CrashReport.detail` up to 16 KB, held every thread
pretty-printed, so a crash's tree was cut mid-JSON and could not be read back;
the sink now keeps only the attributed thread, compact
(`CrashReport.attributedStackJSON`), which `CrashReport.topFrames` reads.

**Agent dots** (2026-10-01, iOS). The user asked for a second switch in the
profile area that puts tiny rings for the agents in the top bar, with the Watch
screen's activity moving inside them. The switch is a row in the hub's profile
header, under the name, with a three-ring preview. It is stored per device
(`agentWatch/showDots`, off by default) in `SettingsService.showAgentDots`.
The switch did not go in the toolbar next to Diagnostics: two labelled switches,
the clipboard button and Done do not fit across an iPhone sheet's toolbar.

When it is on, `AgentDotsBar` sits under the telemetry strip (hidden with the
top bar on the Chat tab). It follows the same agents as the Agents tab's Watch
button (`AgentWatchScreen.forAgentsTab`: the control center's running rows
streamed from the desktop, the running relay sessions, or the last few to replay
when nothing runs). It runs its own `AgentWatchController` with
`SilentAgentWatchOutput`, sound and feel off, so the dots never play notes or
vibrate. The Agents tab hands its `AgentsListModel` to `AgentDotsFeed` instead
of the bar loading the relay lists again.

Each dot is the agent's model face on a disc in its voice colour. Every mark the
Watch would land on the agent's body flies out of the centre as that kind's
`paintWatchIcon` icon, with a glow in the kind's colour, spiralling outwards and
fading. The ring round the disc is a donut of the kinds the agent has done most;
a bright comet spins round it while the agent runs; a waiting agent breathes in
amber; a finished or failed one gets a small green check or red cross badge.
Faint bookkeeping kinds are left out so a tiny dot is not crowded. The summary
next to the dots counts starting, working, waiting and failed agents; tapping
the bar opens the Watch screen in a sheet.

Running dots are portals (2026-10-01, the user asked to look into a running
agent as through a portal at a star field of its activity). A running or
starting agent's disc is dark at the centre and lit in its voice colour at
the rim, and stars stream outwards from the centre, speeding up and growing
as they near the rim. Each star's angle, start and speed come from a hash of
the agent id and the star's number, so the field is stable from frame to
frame. The agent's energy sets how many stars are out (9 idle, up to 22),
how bright they are and how long their streaks get. Every mark it makes
flies out as a bright streak in the kind's colour instead of the kind's
icon. The face is not drawn while it runs; it comes back when the agent
waits or ends. With Reduce Motion the field stands still.

A waiting (paused) agent shows its face with a solid orange ring
(`#e3742f`, the list's waiting colour) instead of the kinds donut; it no
longer breathes, glows amber or carries a badge.

When the composer starts an agent and the bar is showing, the launch
sparkle (§2.1) flies into the bar's next free seat instead of the Agents
tab. As it lands, a spinning placeholder dot appears in that seat: a violet
portal with the running comet. `ShellLaunchFlight.spawns` holds one entry
per landed launch, each with the agent ids that were already in the row at
launch. The bar hands its current ids to `ShellLaunchFlight.claim`, and the
first id that is not in a placeholder's list replaces it. A placeholder
nobody claims goes after 45 seconds.

Frames stop when nothing moves and no agent is live, as on the Watch screen;
with Reduce Motion the comet and breathing are off, so frames stop as soon as
the marks settle. The bar pauses the controller while the app is in the
background, and stops it and its desktop streams when the switch is turned off.

**Dots follow status changes** (2026-10-01, iOS). The user reported that the
dots did not change when an agent's status did. Four causes, all fixed:

- A desktop row streamed over the control link took its status from the last
  `agents.output` frame. An agent that stops writing (waiting on you, or
  finished) sends no more frames, so the dot stayed "working". The status now
  comes from the control center's row, which `agents.changed` refreshes;
  the streamed status is used only for outside agents (their rows always say
  running) and rows that left the list (`controlWatchStatus` in FMAgents).
- `AgentWatchController.ring()` reads only live and unread agents, so a
  finished agent that was prompted again stayed "done". When `add` is handed a
  target it already holds that has ended, the next ring reads it again.
- The bar rang the controller only when the Agents list brought a new agent,
  so a list update that changed a status was not read. `AgentDotsWatch.add`
  (bar and Home) now always rings.
- The bar drew the first twelve agents it had ever seated, and the controller
  never drops one, so after a busy day new agents had no dot. It now seats the
  live ones first, then the most recently seated (`AgentDotsBar.seated`).

No timer was added: every read is still driven by `agents.changed`, the relay
signals and the streams.

**Dots lead the agent rows** (2026-10-01, iOS). The user asked for the dots
to move in front of each entry in the Agents list so the status icon could
come off the row. `AgentDotsBar` is no longer mounted under the top bar, and
the hub's Agent dots switch is gone with it; `SettingsService.showAgentDots`
is no longer read. Every view of `AgentLineRow` (Compact, Zen, Full, Eagle)
now starts with an `AgentLineDot`, and the status glyph is off the row: the
Compact lead was the face with a status badge, and Full's status column now
shows its word only. The dot carries the status instead:

- **Running** is a portal and is meant to look busy: a pulsing glow, a dark
  disc lit at the rim, the face faint underneath, 30 stars streaming and
  swirling out of the centre, four bright sparks orbiting at changing radii,
  the kinds donut, and two comets circling the ring in opposite directions.
  Each mark the agent makes flies out as a streak in its kind's colour, as on
  the bar. The motion runs at 30 frames a second in a `TimelineView`.
- **Waiting** (paused) shows the still face with the solid orange ring and
  draws no frames at all; so do every other status.
- **Queued** gets a dashed grey ring. Done gets a green ring and check,
  merged a purple ring and check, failed a red ring and cross, stopped a grey
  ring and a red stop badge.

With Reduce Motion a running dot stands still. The Agents tab runs one
`AgentDotsWatch` while any of its rows is running (and stops it when none
is, or the tab goes away), pausing it in the background. Each running row
finds its watched agent by key: `AgentLines.watchKeys` gives
`control:<row id>` for a control-center row (or the live row a desktop entry
merged with) and `relay:<owner/name>#<id>` for a relay session. A running
row the watch does not follow (past its twelve) still animates, without the
marks. The launch sparkle no longer has a bar seat, so it flies to the
Agents tab button again.

**Diagnostics under the project name** (2026-10-01, iOS). The user asked for
the live diagnostic charts to lose their numbers and text and become very
tiny charts right under the project name, with the uptime gone and the log
icons in its place. `TelemetryStrip` is no longer a bar under the top bar:
`ShellProjectSlot` stacks the project name over it. The strip is one row,
10 points tall: a 12 by 10 point chart of each metric's last half minute,
with no captions or values. The user later asked for the log icons to go and
only the tiny charts to stay, so the strip shows no log families; their
counts are on the Diagnostics screen. It is one button that
opens the Diagnostics screen; the metric values are in its accessibility
label. Whatever does not fit the width left of the top bar's buttons is
clipped at the end. The project icon and the name are separate buttons that
both open the project drawer.

**Tiny charts in health colours** (2026-10-02, iOS). The user asked for the
tiny charts to be green, orange or red by whether their numbers are good or
bad, and for their backgrounds to go. Each chart is now a bare line with no
box and no area fill. `TelemetryMetric.health` judges the worst reading in
the half minute the chart shows (the lowest for frames drawn, the highest for
the rest) against `TelemetryMetric.limits`, so a spike keeps its colour while
it is on the chart:

| Metric | Orange from | Red from |
| --- | --- | --- |
| Frames drawn | below 45 | below 24 |
| Worst frame | 50 ms | 100 ms |
| Event loop lag | 50 ms | 200 ms |
| Memory | 500 MB | 1000 MB |
| HTTP requests | 10 a second | 30 a second |
| Relay frames | 50 a second | 200 a second |
| Log lines | 20 a second | 100 a second |
| Stalls | — | 1 |

A chart with no sampled second is grey. Android's strip still has the older
captioned squares, so it is unchanged.

**Desktop charts first** (2026-10-02, iOS). The user asked for four tiny
charts at the front of the strip: desktop memory, desktop CPU, desktop disk
and swap, in that order. They are the desktop's own host readings (memory and
swap in use, CPU busy across all cores, used space on the drive holding the
ForkMesh data folder), drawn on a fixed 0–100 % scale, then a 4 point gap,
then the phone's own charts as before. Each one turns orange from 75 % and
red from 90 % (`DesktopVital.limits`, the same cut-offs as the desktop
health bars), judged on the worst reading on the chart. A machine without
swap draws a grey, empty swap chart.

The phone does not poll for them. The desktop already samples its host once
a second for its footer charts; it also keeps the last 30 samples
(`diag::appendVitals`, `kVitalsSeconds`) and pushes each new one as a
`diag.vitals` event to the connections that asked for it:

| Request | Answer |
| --- | --- |
| `diag.watch` | `{memory, cpu, disk, swap}`, each the last 30 percentages, oldest first; `-1` where a reading is missing. The connection then gets `diag.vitals {memory, cpu, disk, swap}` each second. |
| `diag.unwatch` | `{}`; the pushes stop. |

The desktop also drops a watcher when an event to it can no longer be sent
(the connection closed). `DesktopVitals` (`FMDesktopRemote`) asks for
`diag.watch` when the desktop channel comes up (LAN or relay) and the app is
in the foreground, asks for `diag.unwatch` when the app goes to the
background, and forgets the readings when the channel goes away. The desktop
charts are hidden until `diag.watch` answers, so with no desktop, or with a
desktop older than this change (it answers `unsupported`), the strip is as
before. The strip's accessibility label reads the desktop's latest four
values before the phone's.

### 4.x Version, What's new and Send diagnostics (2026-10-13)

The user asked for the version and build number in the avatar hub, opening a
plain-language changelog, and then for a button beside it that sends
diagnostics to a coding agent and turns red when the app is slow.

- **Where.** The last row of the hub (iOS `YouHubSheet.versionRow`, Android
  `HubVersionRow` in `feature/tools/AboutAndDiagnostics.kt`): "Version 1.4.1
  (219)" with "What's new" under it, and the diagnostics capsule at the end.
  iOS reads `CFBundleShortVersionString` and `CFBundleVersion`
  (`AppChangelog.label(infoDictionary:)`); Android reads `versionName` and
  `versionCode` from the package manager.
- **What's new.** iOS pushes `WhatsNewScreen` inside the hub's
  `NavigationStack`; Android swaps the hub sheet's content for
  `WhatsNewPage` (back returns to the hub). Each build is a section headed
  with its version, build number and date, the running one marked
  Installed, with area titles (Market, Calls, Projects...) over bullets. The
  data is the bundled `Changelog.json` (iOS resource) or `changelog.json`
  (Android asset), built per [MAC_BUILD.md](MAC_BUILD.md#whats-new-changelog-2026-10-13).
  `AppChangelog.forRunningBuild` puts the running build first even when the
  file was made for another build (an Xcode or debug build), with "No notes
  were written for this build". No commit hashes are shown.
- **Send diagnostics.** The rule is `PerformanceAlert.evaluate` (iOS
  `FMTools/PerformanceAlert.swift`, Android `tools/PerformanceDiagnostics.kt`),
  checked when the hub opens and every 5 s while it is open. Red ("Slow, send
  report") when, since the last send: an OS diagnostic arrived in the last 24
  h (iOS MetricKit hang, crash, CPU or slow-launch report; Android
  `ApplicationExitInfo` ANR or crash, API 30+); else a main-thread
  (`eventLoop` or `sync`) stall of 250 ms or more in the last 10 min; else 5
  or more dropped-frame (`frame`) stalls in the last 10 min. Otherwise it is
  a neutral "Send diagnostics". The telemetry files were not changed; the
  rule only reads `PerformanceMonitorService.events` and the crash store.
- **The bundle.** iOS: the existing `PerformanceReport` text
  (`PerformanceReportBuilder`: app version and debug or release, device, OS,
  memory, thermal state, metrics, slow events, MetricKit reports with stack
  frames, stall log tail, app log, tab, screen and recent screens) plus the
  build label, commit and alert. Android has no report builder, so
  `DiagnosticsBundle.report` writes a smaller one (version, device, OS,
  metrics, the 8 slowest events with 3 stack frames, exit reasons, last 15 log
  lines, tab, screen and recents). The confirm sheet lists what is sent, has
  an "Include recent log lines" toggle (it drops the log sections) and a
  preview. `DiagnosticsBundle.scrub` removes bearer tokens and `token=`,
  `password`, `api_key`, `cookie`, `session` values, JWTs, `"text"`, `"body"`,
  `"message"`, `"content"`, `"prompt"`, `"title"`, `"note"`, `"subject"` JSON
  values, emails, URL query strings, `+` phone numbers, opaque strings of 32+
  characters and the user folder in file paths.
- **The agent.** Send starts a run on `forkmesh/forkmesh` through the
  composer's quick launcher (iOS `composer.launcher.send(prompt:repository:onDevice:)`,
  Android `ComposerFeature.quick()`), with a prompt asking it to diagnose the
  issue in the native app, fix it on a branch with tests and explain the
  cause, followed by the bundle, cut to the 8000-character prompt limit. The
  relay only takes image attachments (`ORG_AGENT_ATTACHMENT_TYPES`), so the
  bundle travels in the prompt rather than as a file. On success the hub
  closes, the Agents tab opens on the new session, and the red state clears
  until new issues appear (the send time is kept in `UserDefaults` /
  `PrefStore`). A failure stays on the sheet with the launcher's message.

## 5. Search, the command palette and quick actions

### 5.1 Search

- **Scoped search on every tab root.** `.searchable` searches that tab's
  content: agents by title, repo or model; the Inbox; repositories, then code
  within a repository; channels and messages. The first suggestion is always
  "Search everywhere for '…'".
- **Global search** is a route (`Route.search(query, scope)`), reached from
  the command palette, the iPad sidebar, Spotlight and "Search everywhere". Its
  scopes (at most 5, as a segmented control) are All, Code, Issues & PRs,
  Chat and People. Results are grouped in buckets, as on the desktop's results
  page (1.6), each with "See all".
- **Buckets (2026-10-10, iOS and Android).** In order: Repositories, Go to,
  People, Commits, Issues, Pull requests, Code, Chat. The Code scope holds
  Repositories, Commits and Code. Searching for a repository used to show an
  "@<query>" People row whose profile did not exist ("page not found"), while
  the repository itself was missing. The fixes:
  - **Repositories** come first, titled by the repository's name with its owner
    (or "On this device") and description underneath. They cover the network
    catalog (`/api/repositories`) and the phone's on-device repositories
    (`LocalRepoStore`), skipping an on-device copy whose published name is
    already in the catalog. Tapping an on-device repository opens
    `Route.localRepo(id:)`, now registered on both platforms. If the catalog
    can't be read, the on-device ones still show.
  - **People** lists only real accounts: the chat roster plus the server's
    account search (`GET /api/chat/direct-messages/users?query=`, the one the direct
    message picker uses, from two characters on). It no longer adds the typed
    text as a profile.
  - **Commits** match the subject, the author, or a hex prefix of the hash (four
    characters or more), from the repository history read (`history`). A hit
    opens the commit.
  - Issues, pull requests, commits and code are searched in at most three
    repositories: first those whose name or `owner/name` equals or starts with
    the query (`RepositoriesSearchSource.named`), then the composer's
    repository and recent ones. Searching a repository by name therefore also
    lists its commits and issues.
  - Matching ignores spaces and punctuation as a last resort (score 4), so
    "my garden" finds `my-garden`.
- **Saved searches** (1.7) appear above recent searches in global search's
  empty state. Pin a search from the results toolbar.
- **Search within a screen** (1.8): the file viewer, diff and transcript
  support `UIFindInteraction` (⌘F on iPad) with next and previous.
- Search starts as you type, debounced by 250 ms. Scoped results appear first
  from the cache, then the server's results replace them. Offline, only cached
  content is searched, and the screen says so.

### 5.2 The command palette (a long press on the prompt button)

A sheet with the search field focused. It shows:

- **Suggested actions** for the current context: New agent here, New issue,
  Approve 2 pending, Merge #412, Stop all runs, Start timer 25m, New note,
  Switch project. Each is an App Intent (§6), so the palette, Siri and
  Shortcuts share one implementation.
- **Recent destinations.**
- **Results** as you type: actions, destinations (every route in §8 by title,
  "Calendar", "Hosts"…), then content hits from global search.

It is the phone's version of the desktop's Ctrl+K overlay (1.5, 1.10). On iPad
with a keyboard, ⌘K opens it.

### 5.3 Home Screen quick actions

The long-press menu on the app icon (`UIApplicationShortcutItem`) offers New
agent, Inbox, Search and New note. They are static, plus up to two dynamic
items: the most recent repository and the most recent running agent.

### 5.4 Keyboard (iPad, or iPhone with a keyboard)

| Keys | Action |
|---|---|
| ⌘1 to ⌘4 | Switch tab |
| ⌘K | Command palette |
| ⌘F | Find in screen |
| ⌘N | New agent (the composer) |
| ⌘⇧N | New issue |
| ⌘R | Refresh |
| ⌘[ | Back |
| ⌘, | Settings |
| ⌘↩ | Send (composer) |
| Esc | Dismiss |

Every command is declared with `.keyboardShortcut`, so it shows in the
hold-⌘ overlay.

## 6. System surfaces (WS12)

### 6.1 App Intents, Siri and Shortcuts

The user guide for these commands is
[`www/docs/mobile/siri.md`](../../www/docs/mobile/siri.md) (`/docs/mobile/siri/`);
update it when a phrase or an answer changes.

- **Entities.** `RepositoryEntity`, `AgentSessionEntity`, `IssueEntity`,
  `PullEntity`, `NoteEntity`, `ProjectEntity` (the instance). Each has an
  `EntityQuery` backed by the cache. Make them `IndexedEntity`, so Spotlight
  finds repositories, notes and agents.
- **Intents.**
  - `StartAgentIntent`, with parameters repo, prompt and model.
  - `SteerAgentIntent`, `StopAgentIntent` and `ContinueAgentIntent`.
  - `ApproveAgentRequestIntent` and `DenyAgentRequestIntent`, which set
    `authenticationPolicy = .requiresAuthentication`.
  - `CreateIssueIntent`, `CreateNoteIntent` and `StartTimerIntent`.
  - Opening the Inbox, a repository or search, each returning
    `OpensIntent(Route)`.
- **App Shortcuts** with phrases such as "Start a ForkMesh agent on
  ${repo}", "What are my ForkMesh agents doing?" (the answer is a snippet
  view) and "Approve ForkMesh request".
- **Voice commands on iOS** (`native/ios/ForkMesh/Intents/AgentVoiceIntents.swift`,
  phrases in `ForkMeshShortcuts`). A desktop agent is named by its session
  number: the `AgentRunEntity` title is "Agent 754", so "Merge agent 754 in
  ForkMesh" works as said. `FMSurfaces/AgentVoice` reads spoken numbers
  ("754", "seven five four") and matches titles and repositories; the
  coordinator calls `updateAppShortcutParameters()` whenever the agent list
  changes, since Siri only recognises parameter values it was given. Each
  command also has a phrase without the agent ("Merge a ForkMesh agent"), and
  Siri then asks which one.

  | Say | Does |
  |---|---|
  | Merge agent 754 in/for ForkMesh | Merge & clean up on the desktop, after Siri reads back the desktop's summary. Needs the phone unlocked |
  | Stop agent 754 in ForkMesh | Stops it (desktop or cloud) |
  | Continue agent 754 in ForkMesh | Continues a finished desktop agent |
  | Tell agent 754 in ForkMesh | Asks for a message and sends it to the agent |
  | How is agent 754 doing / What did agent 754 change in ForkMesh | Its status, plus files and lines changed from the desktop diff (6 s budget) |
  | What are my ForkMesh agents doing | Summary of all agents |
  | Approve / Deny ForkMesh request | Reads the oldest pending request, confirms, answers it. Needs the phone unlocked |
  | Start a new agent in ForkMesh / New agent in ForkMesh / Start a ForkMesh agent (on a repository) | Opens the app on the composer with the keyboard up, ready for the prompt |
  | Open inbox / approvals / agents / notes / … in ForkMesh | Opens that screen (`ForkMeshScreen`) |
  | Delete agent 754 in ForkMesh | Reads the desktop's delete summary, confirms, deletes. Needs the phone unlocked |
  | Delete all agents / Delete everything in ForkMesh | Counts the desktop's own agents (running, unmerged), confirms once, deletes each. Needs the phone unlocked |
  | Start / Stop / Toggle listening in ForkMesh, Turn on/off ForkMesh listening | The Genie's Always listen (`ListeningIntent`, `ListeningSwitch`; no value toggles). Opens the app, since the recognizer runs only in the foreground, waits up to 8 s for the shell to attach the Genie, then calls `GenieHub.setAlwaysListening`. Start shows a hidden spider again first ([GENIE.md, Hiding the spider](GENIE.md#hiding-the-spider)) |
  | Open a repository in ForkMesh | As before, now a phrase of the Open screen shortcut |

  Apple requires the app's name in every App Shortcut phrase, so plain "Hey
  Siri, start a new agent" cannot be built in; "Hey Siri, start a new agent
  in ForkMesh" is the closest, and a user who wants the bare phrase can name a
  Shortcuts shortcut that runs "New agent" exactly that. The intent sets the
  router's compose request; on a cold launch from Siri that request lands
  before `HomeShell` is listening for it, so `HomeShell.start()` takes any
  pending compose request and opens the composer itself.

  Apple allows ten App Shortcuts per app, so Stop all agents, Start a timer,
  Open agent run, Open inbox and Open approvals are Shortcuts-app actions
  without a built-in phrase (the timer gave up its phrase to listening on
  2026-10-07, when the user asked to turn ForkMesh's speech recognition on
  and off through Siri); a user can add their own Siri phrase for them in
  Shortcuts. To give delete a phrase without dropping one, "Open *repository*
  in ForkMesh" moved into the Open screen shortcut (`OpenScreenIntent` takes
  an optional repository; `OpenRepositoryIntent` stays as a Shortcuts
  action), and one Delete agent shortcut covers one agent and all of them:
  `DeleteAgentIntent` takes an optional agent and an `AgentDeleteScope`
  (`one`, `all`; "all", "everything", "every agent" are synonyms of `all`),
  since a phrase can carry only one parameter. Merge and delete take the
  desktop's confirmation token and send its `typeToConfirm` back, as the
  in-app sheet does; the spoken confirmation and `.requiresAuthentication`
  stand in for the sheet's Face ID prompt.

  Delete all has no desktop bulk op (`bulk` is stop and continue only), so
  the phone asks once, with the counts from the agent list
  (`AgentVoice.deleteAllQuestion`), then runs `prepareDelete` and `delete`
  per agent with each fresh token. It takes only the desktop's own sessions
  (`isDesktop`, not `outside`, no `outsideUser`, `localId > 0`): ending a
  terminal session the user started, or another login's, by voice is too
  broad, and cloud agents have their own delete path. A desktop too old for
  `delete` stops the loop at the first agent with the update message; other
  failures are counted and spoken (`AgentVoice.deletedAll`).
- **Focus filter.** Choose which project the app shows, and whether agent
  pings break through.
- The command palette (§5.2) and the widgets' buttons call the same intents.

### 6.2 Widgets, Live Activities and controls

- **Live Activity per running agent.** It shows the model face, title, current
  step, elapsed time and tokens. It carries interactive **Stop** and, when a
  request is pending, **Approve** buttons, both App Intents that require
  authentication for Approve.
  - **Dynamic Island.** The compact view shows the face and a status glyph.
    The minimal view shows a count of runs. The expanded view shows the last
    step and the buttons.
  - **Updates.** The app updates and ends each activity locally while it
    runs; the activity goes stale after 15 minutes without an update.
    Activities are requested without a push type: the served stack has no
    APNs `liveactivity` sender and no route to register tokens.
  - **Token registration removed (2026-10-08).** `LiveActivityTokenRegistrar`
    posted push-to-start and update tokens to `POST
    /api/accounts/live-activity-token`. That route only ever existed on Next's
    unmerged `agent/native-next-server` branch, so app.forkmesh.com answered
    404 to every post (a ForkMesh ping alert on 2026-10-07 counted 11). The
    registrar, its tests and the coordinator's token tracking are gone.
    Push-updated activities and push-to-start need that server module merged
    first, and the client registration written again with it.
  - **Timers** (10.4) get their own countdown activity.
  - **Walks.** With Pedometer step syncing on, the app notices a walk from
    the motion coprocessor and shows its step count, elapsed time and
    distance; see `native/ios/docs/FMTools.md` (Walks).
- **Widgets.** Running agents (small and medium, with Stop buttons), Inbox
  count and top items, a pinned repository's open PRs, and the timer. Lock
  Screen accessory widgets show the count of running agents and of unread
  items.
- **Control Center and Action button controls.** New agent (opens the
  composer) and Approvals (opens Inbox > Approvals).
- A widget tap deep-links through `Route` (§8.2).

### 6.3 Notification actions (WS9, on WS2's push)

| Category | Actions | Options |
|---|---|---|
| `agent-approval` | **Approve**, **Deny**, Open | Approve requires authentication. Interruption level time-sensitive. Thread: the session |
| `agent-finished` | Open, **Continue**, **Merge & clean up** | Merge requires authentication and is destructive. Thread: the session |
| `agent-failed` | Open, Retry, Send to agent | Thread: the session |
| `pr-review` | Open, **Approve PR** | Approve requires authentication. Thread: the repository |
| `mention`, `dm` | **Reply** (text input), Mark read | Thread: the channel or DM |
| `ci-failed` | Open log, Send to agent | Thread: the repository |
| `ping` (the default) | Open, Mark read | Thread: the kind |

Each action ends in the same App Intent or route as its in-app button.
Summaries group by thread. The relevance score is higher for approvals.

### 6.4 Haptics

Use SwiftUI `.sensoryFeedback`, and let the system's settings turn it off:

| Event | Feedback |
|---|---|
| Send a prompt, post a message | `.impact(weight: .light)` |
| Approve, merge, run succeeded | `.success` |
| Run failed, merge blocked | `.error` |
| Destructive confirmation shown | `.warning` |
| Tab reselect, segment change, pickers | `.selection` |
| Long-press opens the palette | `.impact(weight: .medium)` |

There are no haptics on scroll or on refresh ticks. Listen (WS4) has its own
haptic voice, outside this table.

### 6.5 Interface sounds

Both apps play the desktop's interface cues (`desktop/src/UiSoundCues.cpp`,
[../desktop/agents-ui.md](../desktop/agents-ui.md)): the same fourteen cues (the desktop's fifteenth, merge, belongs to its
Merge & clean up buttons, which the phones do not have),
the same notes, the same celesta two octaves above the agents' tonic, rendered
dry by the Agent Watch synth that is already a sample-for-sample port of the
desktop's. A click on the phone is the note a click is on the desktop.

| Where | Code |
|---|---|
| Cue table, weights, label rule, throttle, WAV writer | iOS `FMAgentWatch/InterfaceCues.swift`, Android `core/.../ui/SoundCues.kt` |
| Player | iOS `Theme/FmSoundCues.swift` (one `AVAudioPlayer` per clip), Android `ui/kit/FmSoundCues.kt` (`SoundPool`, WAVs in the cache dir) |
| Tests | `InterfaceCueTests` (FMAgentWatch), `SoundCuesTest` (core) |

What plays:

| Event | Cue |
|---|---|
| Every `FmHaptics` call | `selection` select, `tap` click, `commit` success, `failure` error, `send` send, `warning` warn, `palette` open; each takes a cue to override (`commit(.send)`, `failure(.delete)`) |
| Button whose label starts with Delete, Remove, Discard... or Send, Post, Publish... | delete or send (`forLabel`; a label ending in an ellipsis keeps the plain cue) |
| Tab change, screen pushed | navigate |
| Modal route (iOS `AppRouter.presented`), kit dialog or bottom sheet (Android) shown | open |
| Screen popped; that modal, dialog or sheet dismissed | close |
| Switch flipped | toggle-on or toggle-off (iOS: a `ToggleStyle` set on the shell, skipped for 0.4 s after the switch appears so a loaded value does not ring; Android: `FmSwitch` / `FmCheckbox`) |
| Pull to refresh (iOS `fmRefreshable`, Android `FmPullToRefreshBox`) | click |
| Pager swiped to another page (Android `FmPagerSounds`), slider moved a step | select |
| Inbox row swiped away (Android) | delete |
| Any other tap that made no sound of its own | see the touch fallback below |
| Typing in the prompt composer | keystroke, shaded by one of six seeds |
| Prompt submitted | send; an agent that starts rings success, a reply from a prompt target is arrive |

**Chat is silent** (2026-10-02). The chat screens make no interface sound at
all: no taps, typing, send, navigate or arrive while one is showing. iOS marks
the chat room, a thread and the Discord screens with `.fmQuietSounds()`;
Android calls `FmQuietSounds()` in `ChatScreen`, `ChatThreadScreen`,
`DiscordScreen` and `DiscordChannelScreen`. Each adds to a counter in the cue
player while the screen is on, and the player drops every cue but a Settings
preview while the counter is above zero. Before, a message from someone else
landing in the open room played arrive on iOS.

One gesture is one sound: cues raised in the same main-loop turn are held and
only the weightiest plays (a row tap that pushes a screen is a navigate), and
within 45 ms a cue only plays if it outweighs the last one.

**Touch fallback.** Since 2026-10-01 every tap sounds, not only the ones that
go through the kit, because most rows, links and plain buttons (an agent in
the Agents list, a `NavigationLink`, a Material `IconButton`) called no cue.
The player remembers when it last raised a cue; a touch that raised none gets
one from the fallback (`InterfaceCueFallback` / `SoundCueFallback`, tested in
`InterfaceCueTests` and `SoundCuesTest`).

- iOS: `FmTouchSoundRecognizer` sits on the shell's window (installed by
  `fmInterfaceSounds`). It never recognises, never delays or cancels touches
  and runs alongside every other recogniser. When the last finger lifts it
  compares the window's view-controller tree with the one at touch-down:
  presented controllers (sheets, covers, alerts, confirmation dialogs) and
  navigation-stack depth. More presented is open, fewer is close, a deeper
  stack is navigate, a shallower one close (`InterfaceCue.afterTouch`), so a
  `NavigationLink(destination:)` push, a local `.sheet` and a swipe-back all
  sound like the router's own. A tap that changed none of that is a click; a
  slide that changed none is silent, so scrolling stays quiet. A tap is
  checked 80 ms after the lift (`kCueFallbackDelayMs`), a slide 450 ms after
  (`kCueSlideSettleMs`, time for the pop or dismiss animation to finish).
  Every tap in the window counts, including one on plain text.
- Android: `Modifier.fmTapSounds()` observes the main pass at the activity
  root and inside kit dialogs, bottom sheets and dropdown menus (each is its
  own window). A tap whose lift a child consumed (a `clickable`, toggle, text
  field or Material button) and that moved less than the touch slop rings a
  click 80 ms later unless something else sounded since the touch went down.
  A tap on a part of the screen that does nothing is silent. Pushes, kit
  dialogs and sheets already sound through `Navigator` and `FmSoundOnShow`;
  the few raw Material `Dialog`, `ModalBottomSheet`, `DropdownMenu` and
  `DatePickerDialog` calls now use both as well.

Sounds are on by default since 2026-09-30 (the desktop's have been since
2026-09-22) and switched off under Settings, General, Sounds. On iOS that
switch writes `device/soundsOff` (true when switched off, removed when switched
back on), and nothing else writes it.
The Settings search finds the switch: "sound", "mute", "silent", "audio",
"volume", "noise", "beep" and "click" all match General (`SettingsCatalog`),
and a Settings route naming the `sounds` section (`dashboard/settings/sounds`)
opens General. iOS filters the Settings page in place, so the switch itself
shows; the Android hub lists matching pages and,
since 2026-10-06, names the matching setting under each one
(`SettingsCatalog.matchedSettings`), so a search for "mute" reads General,
Sounds.
That switch is the only thing that silences them: since 2026-10-01 they play
from the moment the app opens even with the phone on silent, because on silent
(how most phones are carried) the on-by-default setting was inaudible. iOS uses
the `.playback` session category with `.mixWithOthers`, the same as the Watch
screen, so the silent switch does not mute them and other audio keeps playing;
it stays quiet while dictation records. Since 2026-10-02 the session is also
activated (`FmCuePlayer.activateSession()`), not just given that category. This
happens at launch before the cue players are prepared, each time the app becomes
active, and whenever a cue has to put the category back. Before this, only
opening the Agent Watch screen activated it, so the sounds stayed silent until
someone opened the Watcher. Android plays them on the media stream
whatever the ringer mode.

**iOS: on unless the user switched them off** (2026-10-02). Two things kept
the iOS sounds silent for people who never touched the switch:

- The stored value. From 2026-09-24 to 2026-09-30 `device/sounds` defaulted to
  off, and a one-time migration copied the old `sounds.enabled` value (the
  off-by-default system beeps) into it. A `false` stored that way looked like a
  user choice. iOS now ignores and deletes `device/sounds` and `sounds.enabled`
  at launch (`SettingsService.retireStoredSounds`) and reads only
  `device/soundsOff`, so everyone starts with sounds on and only the Settings
  switch turns them off again.
- The audio session. Dictation and a chat voice message set the
  `.playAndRecord` category and leave it there when they stop. The cue player
  treated that category as "recording", so after the first dictation (the
  prompt composer dictates on open by default) every cue was dropped until the
  app was relaunched. It now stays quiet only while `PromptDictation.isListening`
  or `MessagesVoiceRecorder.isRecording` is true, and otherwise puts the
  `.playback` / `.mixWithOthers` category back.

## 7. iPad: a split view with a sidebar

- **Structure.** The same `TabView` with `.tabViewStyle(.sidebarAdaptable)`.
  In a regular width it becomes a **sidebar**. In a compact width (Slide
  Over, narrow Split View) it falls back to the phone layout with the tab row.
- **Sidebar.** The four primary tabs, then `TabSection`s for the hub groups
  (Workspace, Operate, Business, Personal, Insights), then Search and
  Settings. People can customize it (`.tabViewCustomization`): reorder,
  hide, or drag a section item into the tab bar.
- **Columns.** Each list-and-detail destination uses a
  `NavigationSplitView`:
  - Agents: sessions, then the transcript.
  - Inbox: items, then the item.
  - Code: repositories, the repository's sections, then the content (three
    columns).
  - Chat: channels, then the conversation, with threads in an inspector
    (`.inspector`).
  - Notes, Tasks, CRM: list, then detail.
- **Selection.** Selection in the content column drives the detail column.
  The detail keeps its own `NavigationStack` for deeper pushes.
- **Inspectors.** PR metadata, issue sidebar fields and agent details open
  in `.inspector` on iPad and in a sheet on iPhone.
- **Prompt button.** It floats at the bottom trailing corner of the detail
  column, the same control as on the phone. It sits in its own row at the
  bottom of the column, as the phone's tab row does, so every page's scroll
  content ends above it instead of under it.
- **Pointer and drag.** Hover effects on rows. Drag a file or a photo onto
  the composer. Drag a board card between columns (5.5).
- **Multiple windows.** Each scene has its own restoration.
  "Open in New Window" is offered on repositories, PRs and agent sessions.

## 8. Sitemap and routes

### 8.1 Every destination

"Tab" is where the route opens from a deep link. Hub destinations open on the
**current** tab (§4). DESKTOP_PARITY rows are listed, so every capability has a
home. The desktop-only rows (DESKTOP_PARITY's "Desktop-only" verdicts) have no
destination.

| Destination | Route | Tab | DESKTOP_PARITY rows | WS |
|---|---|---|---|---|
| Agents root (Needs you, Running, Queued, Recent) | `.agents(filter)` | Agents | 2.1, 2.2, 2.11, 2.12, 2.35 | WS10, WS7 |
| Agent session (transcript, detail, steer, stop, continue, merge & clean up, move to cloud) | `.agent(id)` | Agents | 2.4–2.10, 2.29, 2.31 | WS10 |
| Approval request | `.approval(id)` | Inbox | 2.13, 2.14, 8.2 | WS10, WS9 |
| New agent (composer), with the agent account picker | sheet, `.compose(target)` | any | 2.3, 2.21 | WS3, WS10 |
| Provider sign-in terminal | `.agentLogin(target)` | current | 2.22 | — |
| Watch field and Listen | `.agentWatch(filter)` | Agents | 2.17–2.18 | WS4 |
| Teammates grid, teammate, briefing, editor | `.teammates`, `.teammate(id)`, `.teammateEditor(id)` | current (hub: Operate) | 3.1–3.6, 9.16 | WS17 |
| Shortcuts and skills, on the repository in use | `.shortcuts(r?)` | current (hub: Operate) | 2.28 | WS17 |
| Inbox (All, Pings, Reviews, Assigned, Mentions) | `.inbox(filter)` | Inbox | 9.1, 9.2, 12.13, 18.5 | WS9, WS2 |
| Ping settings | `.pingSettings` | current (Settings) | 9.3, 13.6 | WS2 |
| Repositories | `.repos(scope)` | Code | 4.1, 4.2, 4.32 | WS11 |
| Repository root (list, README) | `.repo(r, nil)` | Code | 4.4, 4.25, 4.26 | WS11 |
| Code browser, file, file history, code map | `.tree(r, ref, path)`, `.file(r, ref, path)`, `.fileHistory(…)`, `.codeMap(r, ref)` | Code | 4.5–4.9, 4.12, 4.13 | WS11 |
| Commits, commit, range | `.commits(r, ref)`, `.commit(r, sha)`, `.range(r, base, head)` | Code | 4.14–4.16 | WS11 |
| Branches, compare, conflicts | `.repo(r, .branches)`, `.compare(r, base, head)`, `.conflicts(r, branch)` | Code | 4.18, 4.21, 4.22 | WS11 |
| Worktrees | `.repo(r, .worktrees)` | Code | 4.20 | WS10 |
| Issues, issue, new issue, board | `.repo(r, .issues)`, `.issue(r, n)`, `.newIssue(r)`, `.board(r)` | Code | 5.1–5.11 | WS11 |
| Pull requests, PR (Conversation, Commits, Files, Checks), review | `.repo(r, .pulls)`, `.pull(r, n, tab)`, `.review(r, n)` | Code | 5.12–5.19 | WS11 |
| Discussions, discussion | `.repo(r, .discussions)`, `.discussion(r, n)` | Code | 5.22 | WS11 |
| Actions, run, log | `.repo(r, .actions)`, `.run(r, id)` | Code | 5.20, 6.1–6.4 | WS14 |
| Releases, release, artifacts | `.repo(r, .releases)`, `.release(r, tag)`, `.repo(r, .artifacts)` | Code | 6.11–6.13 | WS14 |
| Security | `.repo(r, .security)` | Code | 6.8 | WS14 |
| Mirrors of a repository | `.repo(r, .mirrors)` | Code | 6.15, 6.17 | WS14 |
| Repository projects, milestones | `.repo(r, .projects)`, `.project(r, id)`, `.milestones(r)` | Code | 5.23, 5.24 | WS15 |
| Insights, quality | `.repo(r, .insights)`, `.repo(r, .quality)` | Code | 4.29–4.31, 5.6 | WS20 |
| Repository settings (General, Agents, Automation incl. webhooks, Coves, Danger zone) | `.repoSettings(r, section)` | Code | 4.27, 4.28, 6.10, 16.2 | WS11, WS14, WS21 |
| On-device repositories, local file, quick edit | `.localRepos`, `.localRepo(id)`, `.quickEdit(…)` | Code | 4.17 (phone half), 4.23 | WS5 |
| Chat channels | `.chat` | Chat | 9.4, 9.8, 9.9 | WS13 |
| Conversation, thread | `.channel(id)`, `.thread(channel, id)` | Chat | 9.5–9.7, 9.11–9.15, 9.18, 9.19 | WS13 |
| DM | `.dm(user)` | Chat | 9.10 | WS13 |
| Discord (through the desktop) | `.discord(guild, channel)` | Chat | 9.17 | WS13 |
| Mesh room | `.channel(.mesh)` | Chat | 8.7 | — |
| Profile of a user or organization (`/@name`) | `.profile(name)` | current | 8.5, 9.4 | WS18 |
| People: members in campfire rings, profile sheet with Follow and Message | `.people` | current (hub: App) | 8.5, 9.10 | WS18 |
| Global search | `.search(query, scope)` | current | 1.5–1.7 | WS8 |
| Notes, note | `.notes`, `.note(id)` | current (hub) | 10.1, 10.2 | WS15 |
| Tasks, task | `.tasks(filter)`, `.task(id)` | current (hub) | 2.30, 5.25 | WS15 |
| Calendar, event | `.calendar(date, mode)`, `.event(id)` | current (hub) | 10.3 | WS15 |
| Timers | `.timers` | current (hub) | 10.4 | WS15 |
| Desktops, desktop (files, log, health, working tree, backups) | `.desktops`, `.desktop(id, section)` | current (hub) | 2.34, 4.11, 4.17, 4.24, 12.4, 12.9, 12.12, 13.12, 13.14 | WS16, WS18 |
| Hosts & fleet, host, provisioning | `.hosts`, `.host(name, section)`, `.provision(id)` | current (hub) | 6.16, 7.1–7.11, 7.17 | WS7, WS16 |
| Network, nodes, node | `.network(section)`, `.node(name)` | current (hub) | 7.20, 8.1, 8.3, 8.4 | WS20, WS16 |
| Market, package | `.market(filter)`, `.package(id)` | current (hub) | 11.1, 11.2 | WS19 |
| CRM, contact, organization | `.crm(pipeline)`, `.contact(id)`, `.crmOrg(id)` | current (hub) | 11.3 | WS19 |
| Marketing, analytics, competitors, ads | `.marketing(…)`, `.analytics(…)`, `.competitors`, `.ads` | current (hub) | 11.4–11.7 | WS19 |
| Ledger (public records, rewards, own records) | `.ledger(section)` | current (hub) | 5.27, 11.8 | WS19 |
| Vault, coves, calories, on-device AI, activity | `.vault(…)`, `.coves(r)`, `.cove(id)`, `.calories`, `.localAI`, `.activity` | current (hub) | 2.33, 11.11, 11.12, 11.14, 16.1 | WS21, — |
| Stats, status, admin (users, database, usage, errors, Sentry) | `.stats`, `.status`, `.admin(section)` | current (hub) | 1.18, 4.3, 8.9, 11.16, 12.1, 12.5–12.8, 12.10 | WS20 |
| Settings and its sections | `.settings(section)` | current (hub) | 1.13, 1.16, 1.23, 6.9, 7.12, 7.15, 8.12, 13.x, 14.x | WS18 |
| Installed apps (a Settings section and its screen) | `.settings(section: "apps")` | current (hub) | 13.16 | WS22 |
| Organizations, organization | `.orgs`, `.org(name)` | current (hub) | 13.10 | — |
| Project switcher, projects, a project's agent access | `.projects`, `.projectAccess(id)` | current (hub) | 1.3, 1.4, 2.36, 15.1, 15.2 | WS1, WS10 |
| Tour | `.tour` | current (hub) | 1.15 | WS8 |
| Report a bug | sheet | any | 12.14 | — |
| World | opens the hosted World | — | 9.22, 11.19 | — |
| Share extension "Send to agent" (screenshot), then markup from the composer | extension, then `.compose(target)` | Agents | 11.15 | WS12 |
| Capture screen and markup editor | full-screen cover over the composer | any | 11.15 | — |
| Update required | full-screen cover | — | 1.28 | WS8 |
| Cross-cutting, with no screen of its own: the tab row and hub, keyboard shortcuts, haptics, the prompt button and its spell check, the sponsored card, push, agent record sync, reference links, signed inbox writes, status and avatars, room crypto, the discussion inbox drain | — | — | 1.1, 1.9, 1.14, 1.19–1.22, 1.26, 2.32, 4.34, 5.26, 9.20, 9.21, 18.4 | WS8, WS3, WS2, WS11 |

### 8.2 URLs

**Universal links (canonical).**
`https://app.next.forkmesh.com/...` and `https://next.forkmesh.com/...` open
the app when it is installed and the path is supported. Otherwise they open
the website. The site serves `/.well-known/apple-app-site-association` (Gate
2). Paths are **the website's own paths**, so a link copied from a browser
works:

| Path | Route |
|---|---|
| `/<owner>/<repo>` | `.repo(r, nil)` |
| `/<owner>/<repo>/tree/<ref>/<path>`, `/blob/<ref>/<path>` | `.tree`, `.file` |
| `/<owner>/<repo>/issues[/<n>]`, `/pulls[/<n>]`, `/discussions[/<n>]` | `.repo(r, .issues)`, `.issue`, … (today's `NotificationDeepLink` rules) |
| `/<owner>/<repo>/commit/<sha>`, `/commits/<ref>` | `.commit`, `.commits` |
| `/<owner>/<repo>/actions[/runs/<id>]`, `/releases[/tag/<t>]`, `/mirrors`, `/agents`, `/worktrees`, `/about` | the matching `.repo` section, `.run`, `.release` |
| `/@<name>` | `.profile(name)` |
| `/dashboard/chat`, `/dashboard/notes`, `/dashboard/tasks`, `/dashboard/calendar`, `/dashboard/settings`, `/dashboard/network`, `/dashboard/ads`, `/dashboard/repos`, `/dashboard/profile` | `.chat`, `.notes`, `.tasks`, `.calendar`, `.settings`, `.network`, `.ads`, `.repos`, `.profile(me)` |
| `/dashboard/apps` | `.settings(section: "apps")`: Settings › Installed apps (WS22) |
| `/ledger.html`, `/market.html`, `/status.html` | `.ledger`, `.market`, `.status` |
| anything else | opens in Safari (`openURL` falls through) |

`oauth` is a reserved owner, so `/oauth/authorize` is never read as a
repository: an app's consent always opens in the browser, where the signed-in
web session answers it (docs/APPS.md). `/dashboard/apps/build`, the developer
console, opens in the browser too.

**The custom scheme.** `forkmesh://` takes the same paths
(`forkmesh://forkmesh/app/pulls/12`), plus routes that exist only in the app:

- `forkmesh://agents/<id>` and `forkmesh://approvals/<id>`;
- `forkmesh://compose?repo=o/r&text=…`;
- `forkmesh://search?q=…&scope=…`;
- `forkmesh://ios-udid` (existing).

`RouteParser` returns nothing for `forkmesh://ios-udid` and
`forkmesh://surface/open?route=…`. `AppServices.openURL` handles those two,
and `SurfaceRouter` decodes the surface token. HomeShell's own `onOpenURL`
also sees every URL. Before 2026-10-02 it read the share extension's
`forkmesh://surface/open` as the repository `surface/open`, so sharing an
image pushed a screen that failed with "HTTP 404 for
/api/repo/surface/open/branches".

Add `?project=<id>` to target a project. Without it, the link opens in the
project whose host matches (a universal link), or in the active project.

Any app can claim a custom scheme, so `forkmesh://` is for internal and
developer use only. Anything shared with people uses a universal link.
Widgets, intents and notifications do not use URLs at all. They carry an
encoded `Route`.

**Handling.**

1. `RouteParser.parse(url) -> (project, [Route])` returns the whole stack, for
   example `[.repo(r, nil), .repo(r, .pulls), .pull(r, 12, .conversation)]`,
   so Back walks up naturally.
2. The router selects the route's tab, dismisses sheets, then sets that tab's
   path.
3. An unauthenticated or guest session keeps the route pending and opens it
   after sign-in.

The parser is pure and lives in `NIC/FMApi/Navigation`. It is covered by
Linux XCTests, including every row of the table above.

## 9. States, offline and feedback

### 9.1 Loading, empty and error

| State | Pattern |
|---|---|
| First load | Placeholder rows with `.redacted(reason: .placeholder)`, shown after 300 ms (to avoid a flash). Never a full-screen spinner unless the screen has nothing to show at all |
| Refresh | `.refreshable` on every list. Account event bells refresh silently |
| Empty | `ContentUnavailableView` with an SF Symbol, one line saying why, and the next action as a button ("No agents running" plus "Start an agent") |
| Error, whole screen | `ContentUnavailableView` with the reason in plain words and Retry. The details (status, request id) sit behind a disclosure, for bug reports |
| Error, partial | An inline banner above the content that stays ("Couldn't load checks. Retry"). Content already shown stays visible |
| Action failed | An inline line in the section where the action happened, in the error colour, with Undo or Retry when they apply; never a toast (the user asked on 2026-10-09). A destructive failure uses an alert |
| Long operation (merge, deploy, provisioning) | A progress row that stays on screen, and a Live Activity when it may outlast the app being open |
| Needs the desktop | If DC is unreachable, show "Your desktop '<name>' is offline" with when it was last seen, and the actions that still work (queue on a runner) |
| Needs an admin or owner | Hide the entry point. Deep links show a reason screen, never an empty page |

### 9.2 Offline

- `NWPathMonitor` drives one app-wide connectivity state. When offline, a
  slim banner under the navigation bar says "Offline, showing saved data from
  14:02".
- **Reads.** Lists and details you have seen are cached per project and shown
  read-only, with their age.
- **Writes.** Signed inbox writes (issue comments, reactions, notes) go to an
  outbox and show "Pending" chips. They are sent on reconnect, and the
  failures come back in the Inbox. Destructive and money actions are never
  queued. They are disabled with the reason.
- **On the LAN without internet.** Desktop control (LAN) and the `#mesh` room
  keep working. The Agents tab still shows the LAN desktop's runs.
- The composer keeps its draft offline. Send turns into "Send when online"
  for queued runs.

## 10. Accessibility

- **Dynamic Type.**
  - Support every size up to AX5.
  - At accessibility sizes, rows stack vertically and toolbars move their
    items into the ⋯ menu.
  - The diff viewer switches to unified and wraps lines.
  - Tab and prompt buttons show the large content viewer on a long press.
- **VoiceOver.**
  - Reading order goes from the title to the content, then the toolbar, then
    the tab row; the prompt button is announced last.
  - Every list row is one element whose label combines title, status and
    count ("forkmesh/app, pull request 412, checks failing, 3 comments").
  - Swipe actions are also `accessibilityActions`.
  - Custom rotors list headings, files in a diff, and unresolved review
    threads.
  - Status changes of runs are announced politely, at most one every
    10 seconds.
- **Colour and contrast.** Status is never shown by colour alone; it always has
  an icon or text. Support Increase Contrast and Reduce Transparency, which
  makes glass surfaces solid.
- **Reduce Motion.**
  - The Watch field shows a static summary.
  - Zoom and morph transitions become cross-fades.
  - The running-agent capsule stops animating.
  - Liquid Glass morphing follows the system.
- **Targets and input.**
  - Targets are at least 44×44 pt.
  - Labels are visible for Voice Control ("Tap Approve").
  - iPad has full keyboard navigation (focus) through lists and the sidebar.
  - Dictation is available in every text field.
- **Audio.** Listen and the announcer always have a visual counterpart (the
  Watch field, the transcript) and never carry information that exists only as
  sound.

## 11. Rules for new screens (checklist)

Copy this into the PR description of every new screen.

- [ ] It has a `Route` case (in `NIC/FMApi/Navigation/Route.swift`) and is
      registered in `RouteRegistry` from its workstream's installer.
- [ ] It is in the sitemap (§8.1) with its tab or hub group and its
      DESKTOP_PARITY rows. A URL is added to §8.2 if the website has the same
      page.
- [ ] `RouteParser` tests cover its URL, and a restoration round-trip test
      encodes and decodes its route.
- [ ] It pushes onto a stack or opens a sheet. It never replaces the tab
      root, and it never hides the Back button.
- [ ] Tab roots and the hub root have large titles; details have inline
      titles. There are at most three toolbar items per side, with the rest in
      a ⋯ menu. Destructive actions sit in menus or confirmation dialogs.
- [ ] It has loading, empty, error, partial-error and offline states (§9),
      built from the shared components.
- [ ] Destructive, money and remote-machine actions ask for confirmation,
      and require Face ID where DESKTOP_PARITY says so.
- [ ] If it can target the prompt button's context, it tells the composer
      (repo, item, agent, channel).
- [ ] Its main actions also exist as App Intents (§6.1) where they make sense
      outside the app, and it uses the shared haptics table (§6.4).
- [ ] Accessibility (§10): AX5 layout checked; VoiceOver labels, order and
      actions; no colour-only status; Reduce Motion alternative; 44 pt
      targets.
- [ ] It works on iPad: it sits in the right split-view column, inspectors
      are used instead of sheets where they fit, it has hover and keyboard
      shortcuts, and "Open in New Window" if it is a document-like object.
- [ ] Liquid Glass is used only for bars and floating controls, never behind
      content.
- [ ] A floating action button never covers the last row. On iOS it sits in
      `.safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0)`, not
      an overlay; on Android its list ends with 88dp of bottom padding.
- [ ] Guest mode: its writes are wrapped in `GuestGate`.
- [ ] It is scoped to the active project (WS1): cache keys and restoration
      paths are per project.
- [ ] Its logic lives in `NIC/` with Linux XCTests, and the view in `NI/`.
- [ ] PROGRESS.md: the capability's checkbox is ticked in the same PR.

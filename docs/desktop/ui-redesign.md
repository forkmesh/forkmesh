# Desktop: the interface redesign

Part of [the desktop docs](README.md). The desktop has two designs: **Classic**
(the default) and **New (preview)**, internally `next`. Next is a calm,
neutral interface: near-black or white surfaces, hairline borders, one accent
colour, grouped settings rows, a labelled sidebar, a welcome and sign-in card
and a command palette. No feature may differ in behaviour between the designs.

## Decisions

| Question | Decision |
| --- | --- |
| Technology | Qt Widgets design system. No QML in this project. |
| Sign-in | A welcome screen inside the window. Guest use stays allowed. Account creation still finishes in the browser. |
| Navigation | The left rail gains an expanded, labelled mode and can collapse back to icons. |
| Rollout | Next ships beside Classic, becomes the default when the shell and core pages are done, and Classic stays one release as a fallback. |

### Why not QML

- `MainWindow` is one very large class whose logic reads widget values
  directly; there is no model layer to expose to QML.
- QML does not read QSS, so it would need a second copy of the theme.
- The static Linux Qt build carries only `qtbase` and `qtsvg`; the Windows
  cross-build has no QML host tools or `qml/` deployment.
- Market packages hand the desktop `QWidget` pages
  (`NativePackageWidget : public QWidget`), so the shell must stay widget-based.
- The Qt 6.4 floor, OpenGL widget coexistence and `AUTOMOC OFF` on the app
  target all add friction.

Nothing in the design needs a scene graph; a dim scrim replaces backdrop blur.
Revisit QML only if a screen needs something Widgets cannot draw, and only after
every release pipeline carries Qt Quick.

## Design language

Principles: neutral first (colour means something: blue for selection, focus
and links; green, amber, red only for status); one accent and no gradients;
hairlines, not shadows (only floating layers get a stronger border); seven font
sizes, three weights, five radii, a 4 px grid; quiet neutral count pills (blue
only for unread); text labels over icon-with-caption.

**Colour roles** (`kRoles` in `desktop/src/DesignTokens.cpp`):

| Role | Dark | Light | Used for |
| --- | --- | --- | --- |
| `bg` | `#121214` | `#ffffff` | Window, page |
| `bgSubtle` | `#171719` | `#f3f3f5` | Sidebar, chrome bar |
| `surface` | `#1b1b1f` | `#ffffff` | Cards, inputs, menus |
| `surfaceRaised` | `#222226` | `#f4f4f5` | Hover row, default button |
| `surfaceActive` | `#2a2a2e` | `#eaeaec` | Pressed, active sidebar row |
| `border` | `#26262a` | `#e9e9eb` | Hairlines, row dividers |
| `borderStrong` | `#323237` | `#dcdcdf` | Inputs, buttons, floating layers |
| `fg` | `#ececef` | `#17171a` | Primary text |
| `fgSecondary` | `#c8c8ce` | `#3a3a40` | Secondary text |
| `fgMuted` | `#8d8d97` | `#6c6c75` | Descriptions, icons |
| `fgSubtle` | `#66666f` | `#9a9aa3` | Placeholder, disabled |
| `accent` | `#4d7ef7` | `#2f6fed` | Primary button, selection, focus, toggle on |
| `accentHover`, `accentPressed` | `#6b93fa`, `#3d6be0` | `#2860d6`, `#2254bd` | Button states |
| `accentFg` | `#7ea4ff` | `#2f6fed` | Links, active tab underline |
| `accentSubtle` | `#1a2338` | `#eaf1fe` | Current-item background |
| `onAccent` | `#ffffff` | `#ffffff` | Text on accent, danger and success fills |
| `success` | `#3fb871` | `#1f9d55` | Status only |
| `warning` | `#d9a440` | `#b7791f` | Status only |
| `danger` | `#ef6464` | `#d64545` | Status, destructive actions |

`success`, `warning` and `danger` each have a `Subtle` background. Chat has its
own tone table (`kChatTones`, `@chat*` in QSS, `Theme::chatColor` in code).

**Type.** Base 14 px; the Next sheet caps `font-weight` at 600. Next uses the
bundled Geist and Geist Mono (`desktop/resources/fonts`, SIL OFL, registered by
`Theme::ensureBundledFonts()`). The base `QWidget` rule must set no font family
(a family there beats every monospace `setFont`).

**Shape and space.** Radii 4, 6, 8, 12, 16 and full; spacing 4–32; control
heights 28, 32, 36. Buttons: primary is accent with white text, secondary a
raised neutral, ghost borderless with a hover fill, danger/success coloured text
that fills on hover.

## Architecture

**A second design, not a second theme.** `app/design` (`classic` or `next`) is
independent of `app/theme` (`system`, `dark`, `light`, `halloween`, or a Market
theme id).
`MainWindow::applyTheme()` resolves it with `Theme::designFromSettings()`;
`FORKMESH_DESIGN` overrides it. A Market theme forces Classic because its palette
keys are Classic's hex values. The window's view menu and Settings → Appearance
both switch the interface, and both pick the theme: the view menu's Theme
section lists `MainWindow::appThemeChoices()`, the same list as
`appThemeCombo`, and goes through that combo when Settings has been built.
Below the themes it has two more entries:

- **Match the instance's theme** (`viewMenuMatchInstanceTheme`) toggles the same
  `ui/matchInstanceTheme` setting as the Settings checkbox (see the instance
  theme item below). Turning it on from Halloween or a Market skin switches the
  theme to `system` first, because both of those replace the instance colours
  and the toggle would otherwise seem to do nothing.
- **Reset to default** (`viewMenuThemeReset`,
  `MainWindow::resetAppearanceToDefaults`) removes `app/theme`, `app/design` and
  `ui/matchInstanceTheme`, so the app looks the way a fresh install does:
  Classic, following the system's light or dark setting, in ForkMesh's own
  colours. It leaves density, the Agents layout and the view mode alone. The
  entry is greyed out while those three settings are already at their defaults
  (`appearanceIsDefault`).

**Halloween is a built-in theme, not a Market skin.** `halloween` always uses the
dark base and installs `Theme::halloweenRoleColors()` (pumpkin-orange accent,
bone text on near-black purple, slime-green success, candle-yellow warning)
through `Theme::setRoleOverrides`, the same path an instance theme takes. So it
works in both interfaces, and it replaces the instance theme while it is on.

**Next is Classic recoloured, plus overlays.** `Theme::nextStyleSheet(dark)`
takes the Classic sheet, replaces its neutral greys in one pass
(`kNextDarkPalette`, `kNextLightPalette`), caps weights, and appends every
`desktop/resources/theme/next/*.qss` in name order with `@role` placeholders
expanded. An overlay rule with the same selector wins by order, so every page is
complete in Next and one rule serves dark and light. Exceptions: `95-compact.qss`
only in compact density, `96-edge-indicators.qss` only when edge indicators are
on. `desktop/CMakeLists.txt` generates `theme.qrc` from a glob, so a new sheet
needs no registration.

**The palette reaches code.** `applyTheme()` installs the Next palette with
`Theme::setActivePalette`, so `Theme::remapColor` and `Theme::remapSheet`
recolour icons, the transcript, diffs, `QPalette` and widget-level style sheets.
Both are identity when no palette is active. Colours inside rich-text HTML are
not covered.

**Roles in code** (`desktop/src/DesignTokens.h`). New painting code uses
`Theme::color(role)` / `Theme::hex(role)`, never literals. Existing non-neutral
colours are wrapped per site with `Theme::nextOr(role, classicColor)` /
`Theme::nextOrHex`, which return the classic expression verbatim in Classic:
decoration becomes neutral, selection becomes `SurfaceActive`/`AccentSubtle`/
`AccentFg`, purple states become accent, and status, diff, syntax, chart, avatar
and brand colours stay. These helpers live in `DesignTokens.cpp`, so files also
built into small test programs that do not link it cannot use them.

**What a sheet cannot finish** (`kit::installStockPolish()` in `UiKit.cpp`, an
application event filter applied in Next and undone in Classic):

- *Floating surfaces.* Menus, combo popups and the app's popups get
  `WA_TranslucentBackground`, `FramelessWindowHint` and `NoDropShadowWindowHint`
  before their native window exists (menus on `Polish`, combo popups on
  `ChildAdded`). Property `floatingSurface` tells the sheet whether corners can
  be rounded.
- *Combo rows.* `kit::prepareComboPopup` swaps in a `QStyledItemDelegate` so
  `::item` rules apply.
- *Tables.* Default-centred headers become left aligned; tables with property
  `dataTable` drop grid, row numbers and alternating rows, get 38 px rows,
  right-elided cells, a stretched last column and a delegate that hovers the
  whole row (a cell-level `::item:hover` is all a sheet can do). The admin
  Users and Quarantine tables carry `dataTable`. The table rules in
  `00-base.qss` give every header a 12 px muted caption, 12 px side padding
  and one hairline on a transparent band, and table selection `accentSubtle`
  (`#issueTable` keeps `surfaceActive`, `40-issues.qss`); `91-tables.qss`
  only adds the slim item-view scrollbars and corner, so a rule lives in one
  place instead of being overridden by a later file. The column-menu header
  (`KebabHeaderView`) painted its chevron over the caption: the caption's
  rectangle never left room for it, so a section narrower than its caption
  (dragged, capped, or measured before the sheet applied) ran under the
  glyph. In Next it now elides the caption before the menu button and sort
  arrow, draws its own sort arrow, and shows the menu chevron only on the
  hovered column.

**Hooks instead of header edits.** `MainWindow.h` and `MainWindowInternal.h` are
in the precompiled header. `MainWindow` declares
`friend struct forkmesh::ui::NextAccess` (`MainWindowNextAccess.h`, implemented
in `MainWindowNextShell.cpp`, `MainWindowWelcome.cpp`,
`MainWindowCommandPalette.cpp`). Rail and toggle widgets call hooks in
`NextShellHooks.h` from their paint and size functions; the hooks return false in
Classic.

**Per-page delegates.** Pages restyled in Next use delegates and event-filter
hooks that apply on `Polish`/`StyleChange`/`Show` and defer to stock behaviour in
Classic (notes rows, calendar views, chat rows, files tree, Git changes and
history, docs reader). They read existing item text and roles, so data, drag
and drop and context menus are unchanged.

**Components** (`desktop/src/UiKit.{h,cpp}`, `forkmesh::ui::kit`, no
`Q_OBJECT`): `Toggle` (a `QCheckBox` subclass), `Card`, `SettingsRow`, `Badge`,
`KeyHint`, `InlineNotice`, `EmptyState`, `SegmentedControl`, `Spinner`, and
helpers `groupHeading`, `pageTitle`, `pageLede`, `labeledField`, `tintedIcon`.
The special agents roster's empty state (`#teammateEmpty`) is an `EmptyState`
in Next: its own labels painted the page colour as dark boxes on the card, and
the centred layout sized the wrapped body to its one-line hint, cutting its
descenders.

## Shell features

- **Expanded sidebar** (`ui/railExpanded`): labelled rows; folders open the same
  pop-out as before. Toggled by the top-bar button, Settings, or the palette (no
  window shortcut, to keep `^B` for the built-in terminal).
  Its width is adjustable: drag the thin handle on its right edge
  (`railResizeHandle`, `NextAccess::railResizeHandle`/`setRailWidth`) between
  160 and 380 px; double-click resets it to 196. The width is
  `railExpandedItemWidth()`, saved in `ui/railExpandedWidth` when the drag
  ends. The handle shows only while the sidebar is expanded.
- **Match the instance's theme** (Settings → General → Appearance,
  `ui/matchInstanceTheme`, off): the app wears the colours the connected
  instance's admin picked (Settings → Instance theme on the web). The desktop
  reads `themeColors` from `GET /api/instance` (falling back to `theme.dark` /
  `theme.light`) when the setting is turned on, at launch and when the active
  instance changes (`refreshInstanceTheme`), maps the dashboard tokens onto its
  colour roles (`forkmesh::instancetheme::roleColors`: background → `bg`,
  card → `surface`, primary → `accent`, link → `accentFg`, muted-foreground →
  `fgMuted` and so on, with hover, subtle and selection shades blended from
  them) and keeps the result in `ui/instanceThemeColors`, so the next launch
  starts in those colours offline. `Theme::setRoleOverrides` makes
  `Theme::hex` return them, `nextPalette` and `overridePalette` recolour the
  base stylesheet, so both interfaces follow. A Market skin wins over the
  instance theme. An instance on the default theme clears the overrides.
- **Movable top-bar clock** (both designs): the Timers package's clock and its
  chips sit left beside the agent/node/action indicators, in the middle beside
  search, or right before the view button. Drag the clock and drop it in the
  left, middle or right third of the top bar, or pick a spot under "Clock
  position" in the clock's menu or the top bar's right-click menu. A drag
  swallows the release, so it never opens the clock menu. The spot is
  `ui/timersClockPlacement` (`center` or `right`; removed for the default
  `left`), kept when the package is removed. `installTimersClockSlots` records
  the three rows; `setTimersClockPlacement` moves the strip between them.
- **Welcome and sign-in card**: an overlay on the shell, never a gate
  (`onboarding/welcomeSignInSeenV1`). The sign-in panel is modal over the window.
  Account notices route through `NextAccess::accountNotice`.
- **Command palette**: Ctrl+Shift+P (also Cmd+P on macOS). Lists commands,
  every Settings section (sub-headings once typing), and a "Search settings for
  …" row. Settings is built lazily after the palette opens.
- **Window management** (both designs): `WindowFrameControl` resizes and moves
  the frameless window itself where the platform refuses `startSystemResize`,
  and `fitToScreen` keeps it on the available screen area. The section stack is
  a `SectionViewport` that reports a small minimum width and scrolls a wider
  page sideways. Screen changes are handled one event-loop turn later. The
  fallback resize stores the newest geometry per mouse move and applies it
  once per event-loop turn (and on release), so a drag lays the window out
  once per frame instead of once per move event.
- **App-wide event filters**: `MainWindow::eventFilter` sees every event of
  every object, so it returns at once for event types none of its branches
  handle (timers, meta-calls, paint, layout, polish). A new branch must add
  its event type to that switch.
- **Footer overlays**: `positionGlobalFooterOverlays` runs straight away for
  resizes of the overlay host and the Agents detail pane, but prompt drag and
  grip moves go through `schedulePositionGlobalFooterOverlays`, one queued
  pass per event-loop turn; the button release runs a still-queued pass first
  so the stored size is the dragged one. The pass no longer shows the footer dock
  before deciding whether it is visible (showing a hidden dock re-ran its whole
  height-for-width layout on every drag move while the prompt floated) and
  invalidates the dock row only when its spacer or prompt alignment changed.
- **Stacks sized to the current page** (`src/CurrentPageStack.h`): hidden
  pages get an Ignored size policy. The stack used to re-apply every page's
  policy, with a dynamic-property read per page, on each `LayoutRequest`; it
  now remembers the pages and policies it set and re-applies only when the
  current page, the page list or a page's policy has moved.
- **Density** (`ui/density`: `auto`, `comfortable`, `compact`;
  `FORKMESH_DENSITY`): compact appends `95-compact.qss` and drops font sizes by
  one; auto is compact under 1,500 px available width. Next only.
- **Edge indicators** (`ui/edgeIndicators`, `Theme::edgeIndicators()`, off by
  default): the coloured left stripe on inline notices and conversation cards.
- **Notices** are inline `kit::InlineNotice` rows in `#pageNoticeArea`, not
  floating cards.
- **Alerts** (`QMessageBox`, every call site) are restyled centrally in Next by
  `kit::installStockPolish()` and `13-message-card.qss`: a frameless rounded
  card on `surface` with a hairline border, no icon, the default button in the
  accent, and a dim `#messageCardScrim` over the parent window while it is
  open. The box is marked on its first `ChildAdded` (during construction,
  before its native window exists, which `Polish` is too late for); its
  translucent window skips the stylesheet background, so the filter paints
  `PE_Widget` on `Paint`. The window title becomes a bold heading and the
  message the body when the box has no informative text. macOS discards
  `QMessageBox` window titles (`QMessageBox::setWindowTitle` is a no-op there),
  so on macOS only boxes that set the heading as `text` and the body as
  `informativeText` get a heading; the rest show the message alone. Qt's macOS
  box adds a 24 px widget margin and re-inserts the icon on a style change, so
  the layout is fixed on `Show`, just before `QMessageBox::showEvent` sizes it.
  Frameless alerts cannot be dragged. The scrim follows the parent window
  when it is resized while the alert is open (`fitMessageScrims` on
  `Resize`), and a title moved into the heading is set as plain text unless
  the box is already rich text, so a title containing `<` is never read as
  markup. A title that only repeats the message (`Saved` over `Saved.`) is
  not turned into a heading. Classic is unchanged.
- **Alert layout** is one symmetrical column: 24 px padding on every side, the
  heading over the body, then any checkbox, then the buttons centred and all
  as wide as the widest (at least 96 px). Heading, body and checkbox are
  centred; when either text is a block (a line break, or rich text with
  paragraphs, breaks, lists, tables or `pre`) all of them are left-aligned
  instead, because centred lists are hard to read. Qt's own grid keeps an
  indent column beside the text, and the 340 px button row spans it, so the
  grid split that width in half and pushed the text into the right half with
  the buttons on the right edge (issue 2482); `layoutMessageCard` therefore
  replaces the grid on `Show` with a `QVBoxLayout` holding the grid's widgets
  in row order, minus the icon. `forkmesh-ui-gallery` snapshots four alerts
  (`*-alert-*.png`) in each design.

### Prompt composer

In Next the floating prompt (`#promptWrapper`, built in
`MainWindow::buildNetworkLogDock`) is one `surface` card with a hairline
`borderStrong` edge and 12 px corners, styled by `62-prompt-composer.qss`. Every
rule there is scoped under `#promptWrapper` so it outranks the older composer
rules in `10-shell.qss` and `70-chat.qss`, while the docked workspace rules in
`61-workspace.qss` (`[workspaceDocked="true"]`) still win. The colour images
(model faces, the purple Auto sparkle, the speedometer effort art) are replaced
in Next only: models show monochrome octicons by where they run (`terminal` for
the CLI agents, `server` for the API agents, `cloud` for Cloudflare AI,
`device-desktop` for local models) and the closed model picker shows just the
name and a chevron; modes are `unlock` (Auto), `hand` (Ask), `eye` (Plan) and
`pencil` (Edit); effort is a painted progress ring that fills with the level.
Icons are `fgMuted`: a button opts in with the `nextIconRole` property, which
`applyStoredOcticon` honours only in Next. The bottom bar uses ghost controls
28 px high with 16 px icons on a 4 px grid; the drag strip uses 20 px buttons
with 14 px icons. The send column drops its icons for text labels: the button
Enter triggers (`enterTarget`) is the accent primary, the other is a raised
secondary, and "chat" (sends to the current channel) is a ghost. Object names, layout positions, settings
keys, tooltips and Classic are unchanged.

### Mockup shell (October 2026)

The Next shell follows the desktop mockup (`forkmesh2/desktop-electron`,
`src/index.css` and `src/components/shell/`). Its translucent "glass" tokens
are flattened onto the window colour to give opaque roles, so the role table
above now carries those values.

- **One chrome colour, one content card.** The top bar, sidebar and side rail
  share `bgSubtle` with no hairlines between them. The section stack sits in
  `#shellContentCard` (`bg`, 1 px `border`, 12 px corners), inset 8 px from
  the right and bottom by `refreshShellContentCard`. It is padded 4 px inside
  so square page corners never cover the rounded edge.
- **Sidebar header and footer** (`NextAccess::buildSidebarHeader`,
  `buildSidebarFooter`, refreshed every 2 s by `refreshSidebarChrome`).
  - The header has the workspace button (logo, instance name, this machine's
    node name and the online node count), which opens the account menu, and
    New session (`startNewSessionInSessionsPage`, also Ctrl/⌘+N in Next).
  - The footer has the account button: initials avatar with an online dot,
    the handle, then email or Offline. It opens the same account menu above
    itself; the menu's anchor comes from the `accountMenuAt` /
    `accountMenuAbove` window properties.
  - In Next the account menu also lists Settings, Offline mode and the app
    version.
  - Rows are 8 px rounded; labels use regular weight; counts are plain
    `fgSubtle` text (urgent counts keep the red pill).
- **Top bar** (`NextAccess::installTopBar`).
  - Back/forward move next to the sidebar toggle, followed by the current page
    name.
  - A right-hand cluster holds the online node count (opens Network), the pings
    bell (a dot when pings are waiting), the dark/light toggle (through
    `m_themeCombo` like the view menu), the terminal button
    (`topTerminalButton`, opens the terminal launcher) and the side-rail
    toggle (`topSideRailButton`, `setSideRailEnabled`); see
    [side-rail-and-terminals.md](side-rail-and-terminals.md).
  - The bell opens `#pingsPopover`: the six newest non-quiet local pings. Rows
    open like Home's "Needs you", "Mark all read" moves the local seen
    watermark, and "View all" opens Pings.
  - On macOS, close, minimise and zoom move to the left as traffic lights
    (`trafficLight` property).
  - Every existing chrome control stays.
- **Side rail.** The team rail of special agent chips was replaced on
  2026-10-24 by the generic side rail (terminal, browser, and dragged-in
  workflows, pages and links), in both designs; see
  [side-rail-and-terminals.md](side-rail-and-terminals.md).
- **Home's prompt replaces the footer prompt on Home.**
  `positionGlobalFooterOverlays` hides `m_promptOverlayHost` while Next Home
  (`homeComposer` page property) is current and shows it again on leaving, so
  there is never a second prompt on screen. A detached prompt window is left
  alone.

### Home

In Next, Home (`MainWindow::buildHomeGridSection`, `MainWindowHome.cpp`) is a
centred column instead of a bare icon grid; Classic keeps the old layout. From
the top:

- **Greeting and summary.** "Good evening, Kaif": the first word of
  `topBarUserName()`, capitalised, after the time-of-day greeting. The summary
  counts pings, open tasks and running sessions, then waiting and failed
  sessions, or says "All quiet".
- **Composer** (`#homeComposer`). A second face of the footer prompt, not a
  new launcher. Its pickers read and write the footer composer's own controls:
  repository → `m_issuesRepoCombo`, agent → `m_quickAddAgentProvider`, mode →
  `m_quickAddModeSelector`, model → `m_quickAddClaudeModel`, effort →
  `m_quickAddSpeedSelector`. Changing one on Home therefore changes the footer
  too, and the settings each control already saves stay the source of truth.
  "+" and the microphone click the footer's attach and dictation buttons.
  Dictated text flows back into the Home field.
  Enter (or the send button) copies the text into `m_issueQuickAdd` and calls
  `quickAddIssue()`, so cloud runs, worktree, vibe, create-PR, attachments and
  history behave exactly as in the footer. If the provider is a non-agent one
  (manual, note, task, ActivityPub), it is first switched to the default agent
  provider. The one exception is a branch picked on Home that differs from the
  repository's default: `quickAddIssue()` has no branch input, so Home calls
  `startAdHocAgentForRepo` with the same arguments plus that base branch. When
  a session starts, Home opens it on the Sessions page.
- **Pickers** (`QMenu#homePickerMenu`). Search field, then two-line rows
  (title and path or vendor), with a check on the current one. The repository
  picker ends in "Open folder…" (`promptAddRepository`). The branch picker
  lists `listRepoBranches` with the default branch first. The model picker
  adds an effort segmented control for Claude Code and Codex.
- **Suggestions.** Up to four chips built from real state: pings, open pull
  requests, failed sessions, open tasks, then two general ones. A chip fills
  the composer; it never sends.
- **Apps and Needs you.** The tile grid keeps pages, dots, drag to reorder,
  context menus, pull-down and type-to-search. `HomeBoard::setStyle` paints
  Next tiles as neutral plates with a hairline edge, a muted glyph and an
  accent count pill. The colours are passed in as a `HomeBoardStyle` because
  `HomeBoard.cpp` is also built into `forkmesh-home-board-tests`, which does not
  link the design tokens. "Needs you" lists the five newest non-quiet local
  pings (`m_notifications`): a red dot for a warning, accent for unseen
  (newer than the seen watermark), grey otherwise. A row opens its run or link
  the way the Pings page does; "All N" opens the Pings page.

Home's widgets are built once, the first time the page is shown, so switching
design while running keeps the old Home until restart, like the other widgets
noted under Remaining work.

### Sessions

In Next, the Sessions page (`MainWindowSessions.cpp`, `63-sessions.qss`)
follows the desktop mockup; Classic keeps the old layout.

- **Sidebar** (264 px).
  - A search field and a filter button. Its menu holds the "Between sessions
    only" filter, which toggles the existing, now hidden,
    `sessionsBetweenFilter`, and a Group by Repository or Status choice kept in
    memory.
  - Sessions sit under foldable repository headings, live groups first. A
    heading's "+" opens new-session mode with that repository picked, when it
    has a local checkout.
  - `SessionsNextRowDelegate` paints 32 px rows: status icon, title, age, the
    relay count and unread dot. On hover a "…" opens the existing context
    menu. The old repo · branch · model line moved into the tooltip.
  - Headings use a third sentinel id, so `sessionsHeaderId()` now filters
    them.
- **Centre header** (one 48 px row): title with a chevron menu (the same
  context menu), a monospace branch badge, the status chip, the meta line,
  ghost Stop, Continue and Open in Agents buttons, and a toggle for a 288 px
  details panel.
- **Details panel.** Foldable cards built only from real data:
  - Environment: machine, worktree path, branch with ahead and behind from
    `m_agentDiffStats`, agent, mode.
  - Context: context-window use, tokens, cost.
  - Changes: +/− lines, files, uncommitted, pull request, conflicts.
- **Left out on purpose.** The mockup's remote-control switch, token breakdown,
  file list, Commit and Open PR buttons, task list, and terminal, files and
  browser buttons: nothing on this page backs them.
- **Transcript and composer.**
  - The transcript column is capped at 808 px and centred.
  - The composer is a centred rounded card with ghost pickers, the open
    session's mode and model, and a round send button.
- **New-session mode.** The header is hidden. The composer moves under a
  centred "Start a new session" heading with three starter cards, which fill
  the prompt but never send.

### Transcript

In Next the agent transcript (`ClaudeTranscriptView.cpp`) follows the mockup
on both the Sessions and Agents pages.

- **User turns** are right-aligned `surfaceActive` bubbles: 16 px corners with
  a small bottom-right one, at most 80% of the column wide, with the time below
  in `fgSubtle`.
- **Commands.** Back-to-back shell commands fold into one "Ran N commands
  (M failed)" row. It opens to a bordered card listing each command with a
  status dot.
- **Assistant prose.** Finished prose is 15 px with 165% line height, and
  inline code shows as mono chips.
  - The prose is converted once per message through `QTextDocument` and
    cached, because a markdown `QLabel` cannot take line height or code
    backgrounds.
  - Streaming text is converted only when it completes, so the 60 ms flush
    stays cheap.

### Code

In Next, the repository page (`MainWindowRepoDetail.cpp`, `30-repo.qss`)
reads like a GitHub repository home; Classic keeps the old layout.

- **Header row** (`#repoNextHeader`): a repo tile, the owner and name, and a
  pill saying Public, Private, Local only or Preview.
- **Tabs and actions.** They stay `VerticalIconButton`s with the same object
  names and placement keys. An event filter, `NextRepoButtonStyler`, paints
  them as horizontal underline tabs and ghost buttons with neutral count
  pills, and leaves any button moved to the rail alone. The actions stay in
  the tab row, because button placement owns that row.
- **Code toolbar.**
  - A branch picker that mirrors and opens the status bar's branch menu.
  - Branches, Tags, Worktrees and Remotes as "count label" stats.
  - A blue Code button that opens the existing Source menu.
- **File table.**
  - The latest-commit card becomes the header of the bordered file table
    (`NextOverviewCommitHeader`). This leaves the list's columns and object
    names unchanged.
  - Rows are 36 px with hairlines and a hover fill, with the age right-aligned
    beside the commit message.
- **Right sidebar** (`#repoAboutSidebar`, hidden below 860 px). It shows only
  data the app already has:
  - About: description, website, topics, license file, stars, forks and
    mirrors, with the existing About editor behind the gear.
  - Releases: the tag count.
  - Languages: computed on a worker from the tree listing the overview already
    reads, so no new git call.
  - Contributors: from the repository's info file.
- **Left out on purpose.** No "watching" count, latest-release tag or commit
  total: each would need a new git call.

### CRM

In Next, the CRM page (Market package `crm`,
`marketplace/packages/crm/content/desktop/`) follows the mockup. The package
includes `DesignTokens.h`, as Docs does, and `CrmFeature::applyNextStyle`
sets one `@role` sheet on `crmSection`. Classic is unchanged.

- **Header.**
  - The title sits over a muted summary line.
  - The actions are Import and Mail menus, settings and refresh icons, and one
    inverse "Add contact" button.
  - The import and mailbox buttons live in a hidden holder. The two menus
    mirror them each time they open: each item copies its button's text,
    tooltip, enabled state and visibility, then calls its `click()`. So every
    existing visibility, enable and permission rule still decides what is
    offered.
- **Tabs.** People, Pipelines, History and Extension move into an underline
  tab row, plus a new Overview tab that returns to the open list.
- **Notice.** The Zoho Mail status bar becomes a rounded amber notice. Its real
  steps (Connect, Reconnect, Sync, Guide) stay.
- **Cards and strip.** List cards, the reach-out and follow-up cards and the
  pipeline stage strip use role colours: hairline surfaces, a raised selected
  list, and rounded stage boxes with counts in place of chevrons. Stage and
  avatar colours stay because they carry data.
- **Contacts table.** It drops alternating rows and the grid. It is not
  marked `dataTable`, because that would undo its draggable, resizable
  columns.
- **Live switch.** The header layout is built once, so a live design switch
  shows the new header only after the page is rebuilt.

### Shell: sidebar, top bar, workspace switcher, side rail

This continues "Mockup shell (October 2026)". The code is in
`MainWindowNextShell.cpp`, `MainWindowNavigation.cpp` (group headings) and
`MainWindowSideRail.cpp`, with styles in `10-shell.qss`. Classic is
unchanged.

- **Sidebar rows** (`paintRailButtonHook`, `paintTabButtonHook`).
  - Expanded rows are 32 px tall (`kNextRowHeight` in `MainWindowNextShell.cpp`;
    `kRailRowHeight` in `NextShellHooks.h` is no longer read).
  - The hover and active fill is inset 10 px on each side, so it lines up
    with the header and footer buttons. The 16 px icon sits at 18 px and the
    13 px regular label at 44 px.
  - Counts on rail buttons and on rail tab rows (Code, Git and so on) are
    right-aligned 11 px `fgSubtle` numbers with thousands separators. Tab rows
    used to draw them as a grey pill. Urgent counts keep the red pill, and
    pending-sync counts keep the amber pill.
- **Group headings** (`RailGroupHeadingStyle`, an event filter that
  `refreshRailGroupSeparators` installs on every `railGroupSeparator`).
  - In the expanded Next sidebar, a heading is 36 px tall: 12 px of space,
    then the group name in 11 px medium uppercase `fgSubtle` with a little
    letter spacing, at the row icon's 18 px inset.
  - In the collapsed Next rail it is a centred 20 px hairline, like the
    mockup's collapsed sidebar.
  - The filter repaints the widget and keeps its height. Because
    `followRailCaptionPreference` resets the size, the filter re-fits it on
    `Resize`/`Show`.
  - A custom group being dragged over gets an accent underline instead of
    Classic's green line.
  - Right-click menus, drag and drop, tooltips, group ids and the
    `ui/railGrouped` setting are unchanged.
- **Sidebar header** (`buildSidebarHeader`).
  - The workspace button is 40 px with a 13 px semibold title and an up/down
    chevron (`chevronsUpDownPixmap`). It now opens the workspace switcher
    instead of the account menu. The account menu still opens from the
    footer and keeps every entry.
  - New session's key hints are 18 px `surface` keycaps (`sidebarKbd`).
- **Workspace switcher** (`QMenu#workspaceMenu`, rows `workspaceMenuRow`).
  - **Relay** heading (`workspaceMenuHeading`), then one two-line row per
    configured relay in `m_servers`.
    - The row shows the `broadcast` icon and the relay's name. Under it are
      its host (when that differs from the name) and the last measured
      latency from `m_relayHostLatency`, when one exists. The menu starts no
      probe of its own.
    - The active relay has an accent check.
    - A row calls `switchToServer(i)`, the same path as the relay menu.
      While the project's instance is still unchosen, it calls
      `promptChooseProjectInstance()` instead, as the relay menu does.
  - "Add a relay…" calls `promptAddServer()`.
  - **Organization** lists the organizations in `m_organizations`, once the
    desktop has loaded them (`m_organizationsKnown`), each with a letter
    tile. A row opens Settings → Organizations with that organization
    selected (`m_organizationPendingSelection`, or `selectRow` when the table
    is already built). "New organization" opens the same section and focuses
    its name field.
  - The whole Organization part is left out while the Organizations section
    is hidden by its package surface.
  - The chosen row runs after `exec()` returns, so a dialog it opens never
    runs inside the menu's event loop.
- **Top bar** (`installTopBar`).
  - **Search field.**
    - `#globalSearch` is 28 px tall, at most 440 px wide, and uses 13 px text.
    - In Next its default placeholder reads "Search or jump to…"
      (`createGlobalSearchBox` and `updateRepoActivityRail` in
      `MainWindowRepoDetail.cpp`). The per-page placeholders (tasks,
      issues, agents, specials) are unchanged.
    - While the field is empty it shows ⌘ K keycaps (`topSearchKbd`, Ctrl K
      off macOS) for the existing global search shortcut. Its right text
      margin reserves room for them.
  - **"N agents"** (`topAgentsButton`, accent dot): the number of agent
    sessions that are running and not merged. Clicking it opens Agents. It
    is hidden when none are running.
  - **"N nodes"** now says "N nodes online · open Network" in its tooltip.
  - Both status buttons share the `topStatusButton` style.
- **Side rail.** It replaced the team rail; the chip spacing and dashed
  create tile described here before no longer exist
  ([side-rail-and-terminals.md](side-rail-and-terminals.md)).

**Left out on purpose.**

- *MORE fold chevron.* The rail has no foldable groups. Folding would hide
  rail buttons, and `refreshRailGroupSeparators` hides a heading whose rows
  are all hidden, so the heading would disappear and the group could not be
  opened again. Every feature that toggles rail buttons would also fight it.
- *WORKSPACE / BUSINESS / SYSTEM group names.* The headings show the app's
  own groups (Work, People, Network, Insights, Packages, Tools, Shortcuts,
  More, and custom groups). They appear only when grouping is on in the
  rail's sort menu (`ui/railGrouped`, off by default). Renaming the built-in
  groups or turning grouping on by default would change settings and
  behaviour.
- *Settings row in the sidebar footer.* Settings stays where it is in the
  rail, because rail shortcuts and remembered page buttons address rail
  buttons by position and object name. The account menu also lists
  Settings.
- *Relay version and kind ("1.2.8 · ForkMesh cloud").* The desktop keeps no
  per-relay version or hosting kind, so the row shows the host and the
  measured latency instead.
- *A check on the current organization.* The desktop has no active
  organization to switch between. Organizations are administered in
  Settings → Organizations, so organization rows open that page, and none
  is marked current.
- *Plan line ("Max plan").* There is no subscription plan field
  (`m_accountTier` is only active or free), so the footer keeps the email,
  "Offline" or "Not connected".

### Control

In Next, the Control page (`MainWindow::buildControlNodeSection`,
`MainWindowControlNode.cpp`, `81-control.qss`) follows the desktop mockup;
Classic keeps the old layout. Everything is built only when `Theme::isNext()`
is true at build time, and every rule in the sheet is scoped under
`#controlNodeSection`.

- **Header.** "Control" (15 px semibold, `controlNextTitle`) over a muted
  one-line summary (`controlNextLede`). On the right a status dot and label
  (`controlNextRelayDot`, `controlNextRelayStatus`) say "Relay connected" or
  "Relay offline" from `m_backend`, refreshed by the page's existing 10 s
  `m_controlNodeRefreshTimer`; the tooltip is the mirror-client health line.
  A hairline (`controlNextHeaderRule`) runs under the tab row, full width.
- **Tabs.** The tiles stay `VerticalIconButton`s (`controlNodeTab`,
  `controlNodeHelpToggle`) in the same `QButtonGroup`, so indexes, shortcuts
  and the remembered sub-tab are unchanged. An event filter,
  `ControlTabStyler`, paints them as 36 px underline tabs: a 14 px muted icon,
  a 13 px label and an `accentFg` underline on the current tab; Help, a
  toggle, gets a raised fill instead. In Classic, or when a tile is in the
  rail, it hands painting back and restores the tile's old fixed width. The
  layout order follows the mockup (Deploy, Mirrors, Export, Import,
  Permissions, Keys, Rewards, Relay, Token, Help); only the row's layout
  order moves, not the `QTabWidget` pages or the button ids. Next icons:
  Mirrors `sync`, Relay `broadcast`, the rest unchanged.
- **Tab bodies.** Each tab is padded 24 px and centred at most 1100 px wide.
  Every tab but Deploy gets its tab name as a heading
  (`controlNextSectionTitle`) above the content.
- **Deploy.** `arrangeNextDeployCard` re-lays the two deploy cards (same
  widgets, same parent card) as bordered cards (`controlTabPane[controlNextCard]`):
  - A header row: muted icon, bold title ("Deploy the app to every node",
    "Deploy workers"), and a status line with a status dot
    (`ControlStatusDot`), the existing spinner and the existing status label
    (`m_nodeDeployStatus`, `m_siteDeployStatus`). The dot is hidden while the
    spinner runs, green for "Ready…"/finished/succeeded, amber when Force is
    armed, red when the label shows an error notice or a failure.
  - The action buttons sit on the right: "Deploy app to all nodes" and
    "Deploy changed workers" are the inverse primary.
  - "More targets" (`siteDeployMoreButton`, new) opens a menu built each time
    it opens from the now hidden buttons: Deploy app, Deploy World, Deploy www,
    Force (a checkable item showing the Force button's state), Export complete
    system… and New deployment…. Each item copies its button's text, icon,
    tooltip and enabled state and calls its `click()`, so the run logic, the
    one-run Force and the enable rules are unchanged.
  - Cancel (`siteDeployCancelButton`) shows only while it is enabled, that is
    while a worker deploy runs (`ControlShowWhenEnabled`).
  - A hairline, then the help text and the log.
- **Logs** (`nodeDeployOutput`, `siteDeployOutput`,
  `controlNodeDeploymentOutput`, `controlTokenOutput`): a darker `bgSubtle`
  panel with a hairline and 12 px corners; placeholders unchanged.
- **Other tabs.** Mirrors, Keys and Relay are bordered `surface` cards with
  16 px padding. Export, Import, Permissions, Rewards and Token stay open
  columns so their tables are not a box in a box. Tables get a hairline,
  12 px corners and a quiet header; the permissions, export, import and
  reward tables carry `dataTable` (38 px rows, no grid). The token table does
  not, because it wraps its "Used for" text. Form labels are 12 px muted
  (`controlNextRole="fieldLabel"`), sub-headings 13 px semibold. Buttons in
  the section are 28 px: `buttonSize="primary"` ones become the inverse
  primary (`controlNextButton="primary"`, `fg` fill, `bg` text and icon),
  the rest outline (`controlNextButton="outline"`, `surface` with a hairline;
  a checked one is amber).
- **Unchanged.** Object names, tab texts, button texts and tooltips, the
  help toggle and its hidden hints, settings keys and every connection. The
  reward-pool card (`MainWindowRewardPool.cpp`) needed no code change; its
  buttons and tables are styled by the page pass.
- **Left out on purpose.**
  - "Fleet reachable": the desktop has no fleet reachability probe; the
    header shows the relay connection it does know.
  - The mockup's fake run transcripts, the timestamped coloured log lines and
    the force-deploy confirmation dialog: the logs show deploy.sh and
    next_autodeploy.py output as plain text, and Force keeps its existing
    one-run arming.
  - Export's per-part checklist and progress bar, Import's drop zone and
    "Previous imports", Permissions' person-by-capability grid, Keys' key
    list with fingerprints and rotation, Rewards' stat tiles and per-mirror
    earnings, Relay's relay picker, stat tiles and health checks, Token's
    token list and "New token" dialog, and the Help tab's explainer cards:
    the app has no data behind them, or its real controls differ (the
    Cloudflare bootstrap form, the repository permissions table, the
    Cloudflare token check). The real controls are restyled instead.
  - Button texts keep the app's wording ("Deploy app to all nodes", not
    "Deploy to all nodes"), and the status line keeps the full script path.
- **Live switch.** The header, tab order and card layout are built once, so
  a live design switch shows them only after the page is rebuilt; the tab
  painting and the sheet follow at once.

### Network

In Next, the Network page (`buildNetworkDiagnosticsSection` in
`MainWindowNetworkDiagnostics.cpp`, `buildNodesSection` in
`MainWindowNodes.cpp`, `buildMirrorNodesSection` in `MainWindowReleases.cpp`,
`82-network.qss`) follows the desktop mockup. Classic keeps the old layout.

- **Header.**
  - The title is 15 px semibold, with the "About network diagnostics" help
    button beside it.
  - Under the title is one muted summary line (`networkRole="summary"`):
    "N of M nodes online · E endpoints · c / n websocket connected · X sent,
    Y received". The nodes part comes from the Nodes table and the rest from
    `refreshNetworkDiagnostics`. Each side stores its part in a dynamic
    property (`nextNodesPart`, `nextDiagPart`) on `m_networkDiagnosticsStatus`
    and joins both. The Classic text, with frame counts, becomes the line's
    tooltip.
  - On the right are the page's one Refresh (`repoAction`, ghost through
    `networkRole="ghost"`, same slot) and an inverse "Add node" button
    (`networkAddNodeButton`). Add node opens the Add a host section of the
    Hosts rail beside the node list and focuses its address field. It adds no
    new flow.
- **Tabs.** `#networkTabs` uses underline tabs (accent underline on the
  current tab) in place of the pills from `10-shell.qss`. Tabs that were
  title case are renamed in sentence case in Next ("Web requests",
  "Firewall rules", "Recent firewall decisions"). Every tab stays, in the
  same order and at the same index. Each tab page gets 16 px top padding.
  The five diagnostics tables get 36 px rows and a 12 px rounded hairline
  frame (`networkGrid`).
- **Nodes tab.** It is a left column plus a right detail panel. Every widget
  still belongs to the Nodes page.
  - **Stat cards** (`networkStatOnline`, `networkStatLatency`,
    `networkStatTraffic`, `networkStatRepos`):
    - Nodes online: online and total rows, with an offline breakdown.
    - Median latency: median of the Latency column, with how many nodes were
      measured.
    - Relay traffic: bytes sent plus received over the relay websockets this
      session.
    - Repositories served: distinct repositories in the roster's mirror
      lists, and how many nodes hold them.

    The cards are refreshed at the end of `sortNodesTable` and in
    `refreshNetworkDiagnostics`.
  - **Repository mirrors bar.** `#mirrorNodesSection` becomes a rounded card
    with the repository picker, the count, the Online only switch, Reset
    integrity pin, Refresh nodes and "Update all from this binary".
    - Online only is a `kit::Toggle` (a `QCheckBox`), with the same text,
      default and slot.
    - The update button keeps its text and object name, so a rail shortcut
      to it still resolves by label. It sits in the bar's
      `mirrorNodesHeaderRow`.
  - **Filter bar** (`networkNodesToolbar`).
    - A segmented control built from `nodesRoleFilter`'s items: All, Core,
      Runners, Mirrors, Desktop nodes, Relays. It sets the combo's index, so
      the saved setting and the filtering code are unchanged. The combo and
      its label are hidden, and the two stay in sync both ways.
    - The "Filter nodes" field hides rows whose name, role, owner, status,
      region, platform and endpoint all lack the text. It filters only what
      the role filter already shows, and hides child copy rows with their
      node.
    - The long lede moved into an "About nodes" help button.
    - Sorting lives here, not in the column headers: a "Sort: <field>"
      combo (`networkNodesSort`, every column but Actions, in display order)
      and an ascending/descending button (`networkNodesSortOrder`). Both set
      the header's sort indicator, so `sortNodesTable` is unchanged; the
      choice is saved as `network/nodesSort` (the column's caption) and
      `network/nodesSortOrder` (`asc`/`desc`).
  - **Nodes table.**
    - The model keeps all 44 columns, its actions, context menus and
      double-click behaviour, but the table shows far fewer: the header is
      hidden (`nodePairs` property) and each shown cell holds two different
      fields, one per line (`nextNodePairs`):
      - Status over Activity, Latency over Checked, Health over Integrity,
        Revision over Message and Author, Route over Reach, Repos over
        Mirrors, Size over Git DB, Issues over Pulls and Discussions,
        Commits over Branches, Artifacts over Worktrees, Clones over Web
        hits, Caps over Scope.
      - CPU, RAM and Disk share one cell as three small meters.
      - Each line is its icon, a muted caption (left out where the value
        says what it is: status, activity, latency, revision, message) and
        the value. An empty value is a plain dash.
      - A count's sync-back backlog (`nextNodeBacklogColumn`) follows it in
        the warning colour as "↑N", instead of taking its own column.
    - Role, owner, platform (or region), endpoint and version are folded
      into the Node cell's second line, which absorbs the spare width
      (`makeColumnsResizable` keeps Node flexible). Node id is only in the
      details panel.
    - `nextFitNodeColumns` (run at the end of `applyNodeRoleFilter`) hides a
      cell whose fields are empty in every row the role and text filters
      show, so a role tab only lists the columns its nodes report.
    - Rows are 48 px.
    - `NextNodeCellDelegate` paints:
      - Node: a status dot (green online, grey offline, amber behind, red
        integrity failing), the mono name, and a soft "You" badge or outline
        "Source of truth" and "Active relay" badges.
      - Child copy rows: indented under "└".
      - Muted columns: role, scope, status ("Online · serving"), activity,
        owner, latency, region.
      - Version and Revision: mono.
    - `NextNodesTableChrome` (a viewport event filter) paints the hairlines,
      the whole-row hover fill and the selected-node band. The table stays
      `NoSelection`.
  - **Detail panel** (`networkNodePanel`, 320 px).
    - A click on a row opens it, and × closes it. The node is remembered by
      key (`nextPanelKey`) and refilled after every refresh.
    - Content, all read from that row's cells:
      - Header: a role icon tile, the mono name, "This node", and role ·
        region (or platform).
      - An offline notice made of the status and its tooltip.
      - Latency, Synced and Version tiles.
      - CPU, Memory and Disk bars (`kProgressBarRole`).
      - A Details list: status, activity, owner, platform, revision,
        endpoint, health, node id.
    - Footer: Profile (`showNodeProfile`), the row's own action (Website or
      Logs, through `activateNodeDirectoryRow`), and "…", which offers the
      row's Delete button by clicking it.
  - `m_nodesStatus` stays as a footnote under the table.
- **Left out on purpose.**
  - Topology graph: the app has no node-to-node link data and no graph
    widget.
  - Activity · 24h sparklines and the panel's Load · last 24h: no per-node
    history exists. Activity stays the existing text column.
  - The panel's Uptime tile: no uptime is reported. Synced is shown instead.
  - The Services list with running dots: nodes report no services.
    Capabilities stay in the Caps column tooltip.
  - Open shell and Restart: the Nodes directory has no per-node shell or
    restart. Those stay on saved hosts in the Hosts rail.
  - The Relay traffic delta ("+12%"), and the mockup's mesh invite command
    and dialog: no such data or flow exists.
  - The mockup hides most columns. Here they are paired two to a cell or
    folded into the Node cell instead, so no field is lost.
- **Hosts.** The Hosts tab is gone: saved hosts, fleet servers, runners and
  directory instances are rows of the node list, and the host controls sit in
  a compact rail beside it ([networking.md](networking.md#network--nodes)).
- **Tests.** `test_window_resize.cpp` pins compact two-line node rows (40 to
  52 px) and the "Web Requests" tab label. Both hold in Classic, the test
  default. The Next values are cosmetic.
- **Live switch.** The layout is built once, so a live switch shows it after
  the page is rebuilt.

### Node profile

In Next, the node profile panel (`buildNodeProfilePanel` and
`showNodeProfile` in `MainWindowNodes.cpp`, rules at the end of
`82-network.qss`) follows the mockup's Settings → Profile layout. It looks the
same in Settings → Profile and when opened from the Network page. Classic keeps
the old two-column layout. Every action, object name and visibility rule stays.

- **User profile card** (`profileNextUserCard`).
  - A 64 px rounded avatar beside the name. The name is 15 px semibold. In
    Next it drops the " (you)" suffix and shows a "you" badge instead; the
    admin crown stays.
  - A muted meta line: the existing status dot and text, then "This machine"
    (or "Owned by <owner>"), the platform and the version.
  - The note.
  - One row of outline buttons:
    - Rebuild, Update, Add repo, Edit profile, Settings (hidden in Settings,
      as before) and Disconnect.
    - On other nodes' profiles, Message and Take ownership.

    `layoutProfileActions` puts them on one line in Next instead of the
    three-column grid.
  - "Mirror reward settings" sits under the card as an accent link-style
    button. It keeps its text, tooltip and slot.
- **This machine card** (`profileNextMachineCard`, own profile only).
  - The node name, with a muted line: platform, ForkMesh version, and how
    long this session has been online.
  - An outline "This device" pill.
  - The existing REPOS, MIRRORED, NETWORK and CHATS tiles, with caption over
    number, split by hairlines.
- **Power switch.** The existing section follows the machine card unchanged.
- **Details** (`profileNextDetails`).
  - Key/value rows with hairlines: Status with a dot, Platform, Version
    (mono), Uptime, Total uptime, Channels, Key (mono). The values are the
    ones `m_profileDetails` shows.
  - The rich-text `m_profileDetails` label is still filled the same way, but
    hidden in Next.
- **Below.** Mirrors, local repositories, node ID, nodes, Solana and tip
  wallet stay in the existing columns row. It still stacks in Settings and
  sits side by side on the Network page.
- **Selectors.** The new rules are scoped under `#nodeProfileContent
  #profileNext…`, so they outrank the Settings rules in `20-settings.qss`
  (`#settingsSectionsContent #nodeProfileContent QLabel#statTile` and the
  others) without editing that file.
- **Settings navigation.** The sub-section list still comes from the visible
  `sectionLabel` headings, sorted by position. The title now reads "USER
  PROFILE" and the new "THIS MACHINE" heading sits above the machine card; the
  old one inside the actions block is hidden. So the list reads User profile,
  This machine, This machine's power switch, Details, and so on.
- **Left out on purpose.**
  - "joined August 2026": the panel has no join date.
  - A separate device name and hardware spec ("Apple M4 Pro · 36 GB memory ·
    Pune"): the panel knows only the node name, platform and version.
  - The mockup's "…" overflow menu: every action stays a visible button
    instead.

### Files

In Next, the Files page (`MainWindow::buildFilesSection`, `MainWindowFiles.cpp`,
`32-files.qss`) reads like a file browser with the editor as its preview pane;
Classic keeps the old layout. The page layout is now an outer `QHBoxLayout`
(`frame`) holding the old column, so the places column can sit beside it; in
Classic the column is the only item and the margins are the old 20/18/20/20.
Every new widget is a child of the page or the editor column, built in both
designs and hidden in Classic, and `FilesLookHook` re-arranges on a live
design switch (`arrangeFilesHeader`).

- **Places column** (`#filesPlaces`, 208 px, hairline on the right).
  - A source picker (`#filesSourcePicker`, "This Mac" on macOS, "This
    computer" elsewhere) whose menu clicks the existing Repo, Coves and Network
    buttons; "This Mac" re-reads the filesystem root through
    `setFileExplorerRoot`.
  - PLACES: Home, code, projects (`paths::projectsRoot()`), Downloads and
    Desktop. FORKMESH: Node (`paths::homeDir()`), Node data
    (`paths::dataDir()`) and Primary checkout (`homeDir()/src`). A row shows
    only when its folder exists. The row for the open project clicks the
    existing Home button, so it runs the same code; when the open project is
    not the home project it is a separate "This project" row. The current root
    gets the `surfaceActive` fill (`filesPlaceActive`).
  - A disk card (`#filesDiskCard`) at the bottom: volume name, used bar and
    "N free of M", read with `QStorageInfo` for the current root. Hidden when
    the volume reports no size.
- **Toolbar** (one 48 px row with a hairline under it).
  - Back and forward step through the folders browsed on this page (kept in
    memory, at most 100); Up clicks the existing Up button.
  - A breadcrumb (`#filesCrumbs`): the home folder's name, then each folder
    as a button; deep paths keep the first crumb, an ellipsis and the last
    three. The pencil, or a double-click on the breadcrumb, swaps in the
    existing path field (`filesPathBar`) until editing finishes. The
    repository, Coves and network trees show their source name instead.
  - "Search this folder" (`#filesFolderSearch`) hides top-level rows of the
    filesystem and repository trees whose name does not match; it is hidden
    on Coves and Network, which have their own filter.
  - A view menu (filter icon) holds Show hidden files and Show sizes (they
    set the existing checkbox and Size button, so the settings and the
    re-listing are unchanged), Project folder, Choose a folder…, Refresh and
    Open in the file manager (each clicks the existing button).
  - A panel toggle hides or shows the editor column. Opening a file in a tab
    shows it again.
  - The old title, Show hidden checkbox, Size, Up, Home, Repo, Coves,
    Network, Choose, Refresh and Open buttons are hidden in Next; each has an
    equivalent that calls the same code.
- **Git bar.** The existing editor bar (Preview, Show history, Commit direct,
  Save as PR, Save, commit message, Commit, Push) is the row under the
  toolbar, with a hairline under it.
- **List.** The filesystem tree gains a Kind column (logical column 3, moved
  to visual position 1 in Next, hidden in Classic), filled from
  `QMimeDatabase` by extension in Next only, so Classic listing costs
  nothing more. `FilesTreeDelegate` now paints 36 px rows (32 compact)
  across the full width with a hairline under each, `accentSubtle` for the
  selection and `surfaceRaised` on hover, and tints file icons `fgSubtle` and
  folders `fgMuted`. The trees lose their card border; the header is a 36 px
  `fgSubtle` caption row with Size right-aligned.
- **Preview pane.** The editor column (breadcrumb, find bar, tabs, symbol
  panel) is the right pane, behind a hairline splitter handle, with
  `#filesInfoPanel` added under it: the file name, its kind, Size, Modified
  and Where (home-shortened, mono), then Open (opens a local file with the
  desktop app, like "Open with the desktop app") and Send to agent (puts the
  path at the end of the agent prompt and focuses it; it never sends).
  `updateFilesEditorStatus` refreshes it through `syncFilesInfoPanel`, so it
  follows the current tab. Rows without data (Size and Modified for Cove and
  network files) are hidden.
- **Status bar.** `m_fileExplorerStatus` (its existing text), "· name
  selected" for the current row, and on the right "Double-click a folder to
  open it · double-click the path to type one".
- **Left out on purpose.**
  - The grid view and the list/grid toggle: the app has no grid of the
    explorer, and a second view would need its own context menu, rename,
    trash, paste and drop handling to keep those actions.
  - "N items" for folders in the Size column, and "Today" style dates: the
    app does not count folder contents, and the existing
    `yyyy-MM-dd HH:mm` stamp is kept.
  - The "Protected" badge and notice: nothing on this page knows which
    folders are protected.
  - The line-numbered code card: the Preview tab's text starts with a
    three-line header that the preview-line highlight and tests depend on,
    so it stays a plain preview; opened tabs keep their line numbers.
- **Sizes.** The mockup source's 36 px rows (`h-9`) are used; the
  screenshot is scaled about 1.38x, which is why rows measure 50 px there.

### Memory

In Next, the Memory page (`MainWindow::buildMemorySection`,
`MainWindowMemory.cpp`, `MemoryUsageChart.cpp`, `83-memory.qss`) follows the
mockup. It is built once, so a live design switch keeps the layout it was
built with; the chart and table colours still follow the switch.

- **Header.** "Memory" over the existing status line ("N mappings in M
  groups · read in X ms · time"), then a 24 px gutter and a hairline under
  it (`memoryRule`). The actions keep their order and code: the Filter field
  (160 px, a search icon), the interval combo, Pause (a painted pause/play
  icon that follows its state), Trim heap on Linux, Copy report, Memory map
  and Agent governor as ghost buttons with `fgMuted` octicons
  (`nextIconRole`).
- **Tiles.** The eight `statsTile` frames are 12 px rounded `surface` cards
  in a 4-column grid with 12 px gaps: a 12 px muted label, a 22 px semibold
  value and the existing caption in `fgSubtle` (`memoryRole` property on each
  label).
- **Chart.** `MemoryUsageChart` gains a Next painting mode, switched on by
  colour properties the page sets (`nextAccent`, `nextBorder`, …), because the
  file is also built into `forkmesh-process-memory-tests`, which does not
  link the design tokens. The page re-sets them on `Polish`, `StyleChange`
  and `PaletteChange` (`MemoryLookHook`) and clears them in Classic. In Next
  the chart is a 244 px card: a 40 px header with "Resident memory · last 5
  minutes" and the newest total on the right, dashed gridlines with byte
  labels, HH:mm:ss time labels, and one 2 px accent line of the summed
  bands. Hover draws a rule and a dot; the tooltip still lists every group.
  Classic keeps the stacked bands.
- **Tabs and tables.** "By mapping" and "Inside the app" are underline tabs
  (document mode, accent underline). Both trees are 12 px bordered tables on
  `bg` with 36 px `fgSubtle` headers, 40 px rows with hairlines, no
  alternating rows, `accentSubtle` selection and right-aligned numeric
  headers. Growth in the Change column is `warning` and shrinkage `success`
  in Next (`Theme::nextOr`); Classic keeps its red and green.
- **Left out on purpose.** The mockup's fixed captions ("not shared with
  other processes", "peak 63" threads) are replaced by the captions the page
  already computes, and its feature ranking under "Inside the app" by the
  existing allocator, toolkit and probe tree; nothing else is measured.

### Tasks

In Next, the Tasks page (`MainWindow::buildOrganizationTasksSection`,
`MainWindowTasks.cpp`, `41-tasks.qss`) is a board with a side panel, after
the mockup; Classic keeps the old list layout. The Next layout is built once,
inside the builder, so a live design switch shows it only after restart.

- **Header** (`#tasksNextHeader`).
  - "Tasks" over a muted summary (`#tasksNextSummary`): open, with agents
    (open tasks assigned to the bot), in progress, closed, and "manager" when
    the account can manage tasks. The old summary label is hidden.
  - Filter tasks (`#tasksNextFilter`) writes the same search mirror the top
    bar search feeds (`m_organizationTasksSearchMirror`), so it filters with
    the existing matcher; a top-bar search or a section change still wins and
    the field follows it.
  - The assignee picker (`#tasksNextAssignee`, "Everyone ▾") lists the people
    strip's buttons (Everyone, each member, Bot, Unassigned) with their open
    counts and clicks the chosen button, so the strip's own toggle logic
    decides. The strip itself is hidden in Next.
  - Board / List toggle (`#tasksNextViewSwitch`), Refresh (still showing its
    age), Delete closed, and New task as the one inverse button.
- **Chip row** (`#tasksNextChips`): the Open, Closed and All status tabs and
  Mine are painted as rounded chips with their counts (accent when checked);
  on the right the list density button (list mode only) and the sort menu
  ("Newest ▾").
- **Buttons.** Existing `VerticalIconButton`s keep their object names, menus,
  connections and enable/visible rules. `TasksNextButtonStyler`, an event
  filter, paints them in Next as ghost icon buttons, ghost text buttons,
  chips or the inverse primary, and shows the button's caption as a tooltip
  when it has none. Start shows a stop glyph while its caption is Stop.
- **Board** (`#tasksNextBoardScroll`). Five rounded `bgSubtle` lanes
  (`#tasksBoardColumn`): Ready, Queued, In progress, In QA (QA requested and
  not reviewed) and Done, each with a status icon, title and count; Ready has
  a "+" that clicks New task (managers only). Lanes follow the Open / Closed
  / All filter: Open hides Done, Closed shows only Done. Each lane is a
  `TasksBoardList` (`QListWidget`) whose `TasksBoardCardDelegate` paints
  12 px cards: short task id (mono) and repository or department, the
  priority number (`P12`), the title on up to three lines, then the
  assignee's face (agent face, person face, or a dashed empty avatar), a
  checklist count read from the details' task boxes (or progress percent),
  the work-item branch, attachment count, QA state and age. The card tooltip
  is the list's hover card. The board shows every task the filters match;
  clicking a card selects its row in the (hidden) table, loading further
  pages of rows if needed, so every detail action keeps working from the
  table selection. Double-click edits, as in the list.
- **Drag to change status.** Dropping a card on another lane selects the task
  and clicks the button that already performs that move: Start (to In
  progress), Stop (back to Ready), Mark undone (Done to Ready), Complete (to
  Done, with its completion-note prompt) or Send to QA (with its steps
  prompt). A move none of them offers, or one the account may not make, only
  says so in the footer status. The drop runs after the drag has finished.
  The footer (`#tasksNextFooter`) carries the drag hint (board mode) and the
  task status line.
- **List.** The List toggle shows the existing table with its Full /
  Compact / Zen rows, paging label and spinners unchanged.
- **Detail panel** (`#tasksNextDetailPanel`, splitter right side).
  - Header: the short task id (mono, full id in the tooltip), a "…" menu
    (`#tasksNextDetailMore`) with Edit and Move to trash, which mirror and
    click the Edit and Delete buttons (both hidden from the row), and ×,
    which hides the panel until a card or row is clicked.
  - Icon row (`#tasksNextDetailActions`): every other action button.
  - Body (`#tasksNextDetailScroll`): the title (`#tasksNextDetailTitle`,
    18 px), the property editors (Status, Priority, Progress, Assignee,
    Department, Team, Repository, Follows task) restyled in place, then the
    detail browser grown to its document height: details, How to test and
    Completion note when present, attachments, the fields that used to sit in
    the side browser under DETAILS (Destination, Created by, Agent session
    link, QA, Updated, Last reminder, Work items, Task ID), and ACTIVITY rows
    with an avatar, "@who did what" and a right-aligned age. The side fields
    browser is hidden in Next because its rows and links now render, through
    the same link handler, in the detail browser.
  - Comment bar: the quick comment field and its Comment button in a rounded
    surface box; Ctrl+Enter still posts.
- **Left out on purpose.** Backlog and "In review" (pull request) columns,
  "2 due today", due dates, comment counts on cards, linked PR numbers, the
  priority bars' urgent/high/medium/low buckets, People/Bots/Blocked chips,
  blocked state, Copy link and Duplicate, subtask checklists in the panel,
  the PR card, "Hand this task to an agent" in the comment field, the list
  view grouped by status, and the signed-out lock card: the catalog carries
  no due date, comment count, PR link, blocked flag or task URL, and there is
  no duplicate action, so each would be invented. The board/list choice is
  kept in memory, not in settings.

### Chat

In Next, the Chat page (`buildChatSection` and its builders in
`MainWindowIssues.cpp`, the rows in `MessageRow.cpp`, the data-driven parts in
`MainWindowMessages.cpp`, sheets `70-chat.qss` and `71-chat-sidebar.qss`)
follows the desktop mockup. Classic is unchanged. The chat sheets now use the
standard roles (`@bg`, `@surface`, `@border`, `@fgMuted`, `@accent`…) instead of
the Discord-like `@chat*` tones, so Chat matches the rest of the shell.

- **Icons.** The mockup's lucide icons that the octicon set lacks (hash,
  search, phone, video, smile, at-sign, paperclip, arrow-up, shield-check,
  activity, x) are drawn from inline SVG by `chatGlyphPixmap` in
  `MainWindowIssues.cpp`. `setChatGlyph` puts one on a button, label or line
  edit with a colour role and clears the button's stored octicon;
  `ChatGlyphTinter`, an event filter, redraws it on `StyleChange` and
  `PaletteChange`, so the icons follow a dark/light switch.
- **Conversation column** (`#sidebar`).
  - The header row is `QLineEdit#chatJumpSearch` ("Jump to a conversation")
    and the existing compose button as a "+" (`chatComposeButton`, still
    `showNewMessageDialog`). The "ForkMesh ▾" button (`chatWorkspaceMenu`) is
    hidden: it opened the same dialog.
  - The search hides channel, group and direct-message rows that do not
    match (`setHidden` on the items; a collapsed group keeps its collapse
    state). Enter opens the first match through the lists' own
    `currentItemChanged`, Esc clears. The filter is re-applied after the lists
    are rebuilt. `fitToRows` now skips hidden list rows; nothing is hidden in
    Classic.
  - Section headings read "Channels" and "Direct messages" with the chevron
    before the label; their "+" buttons stay. "All unread" stays as a row; the
    divider under it is hidden.
  - `ChatRoomRowDelegate::paintNext` draws 32 px rows inset 10 px with 8 px
    corners (`surfaceActive` selected, `surfaceRaised` hover), a 16 px `#`
    (a lock for private rooms), 13 px names (medium and `fg` when unread), an
    accent unread pill or dot, and for direct messages a 20 px avatar with a
    presence dot (`chatListAvatar` draws neutral initials in Next; green online,
    amber away, grey offline). User groups are 28 px headings. The gear
    column is unchanged.
  - The last-message preview and its time moved from a second row line into
    the row tooltip (`helpEvent`, escaped so a remote preview never renders
    as markup).
  - The footer (`#chatEncryptionFooter`) replaces the chat user panel
    (`chatUserPanel`, hidden: the shell's account footer shows the same name
    and online state). Its text comes from the current room in
    `updateChatConversationHeading`, the same rules as the header lock:
    "End-to-end encrypted on every device" for private rooms and direct
    messages, "Sealed to this room's devices; the relay cannot read it" for
    public rooms, and plain statements without the shield for server-visible,
    Discord and nearby rooms. Its tooltip is the lock's tooltip.
- **Header** (`#chatHeader`, 48 px). `#` glyph, 14 px title, then the
  encryption lock (moved beside the title, 14 px `fgSubtle`), then the topic in
  12 px muted (no divider). On the right the members toggle
  (`chatHeaderMeta`) shows a pile of up to three member faces and the count,
  built in `refreshChatMembers`; then voice and video call (icon-only, text
  kept as the accessible name, still DM-only), Invite, the seal button, mesh
  diagnostics and channel settings as 28 px ghost icons.
- **Messages** (`MessageRow`).
  - The transcript and composer are capped at 860 px and centred
    (`ChatNextEventHook` on the scroll area's resize).
  - Rows: 32 px avatar (neutral two-letter initials when there is no photo; a
    rounded square for specials), 12 px gap, 8 px rounded hover fill. The
    header is the name in 13 px semibold, a "Special" outline badge for
    `teammate:` senders, and the time in 11 px `fgSubtle`
    (`QLabel#messageTime`, which also carries "(edited)" and the delivery
    note). Recipient counts and receipt marks follow the time.
  - Body text is 14 px with 150% line height; inline code becomes a mono chip
    on `surfaceActive`.
  - Reactions are 22 px pills; the ones you added (`chatSelfName`, the same
    name `hasOwnChatReaction` checks) are accent-tinted (`mine` property).
  - The thread line is the repliers' faces, "N replies" in accent and
    "· last 9:58 AM" (newest reply time from the history) in `fgSubtle`.
  - Non-media files are a 340 px card: icon tile, file name, size, and the
    existing Save button. `fileChip` keeps its object name.
  - Day dividers are centred pills reading Today, Yesterday or the date.
  - The typing line reads "••• Donnie is typing" in plain text.
- **Composer.** One rounded `surface` card (`#composerInputRow`, 16 px
  corners, accent edge while the input has focus): the message input on top,
  then attach (paperclip), emoji, mention and link as 28 px ghost icons, an
  "Enter to send" hint, and a round accent send button with an arrow. Every
  button keeps its slot, tooltip and shortcut.
- **Thread pane and members panel** use the same header height, hairlines,
  roles and a rounded reply box.

**Left out on purpose.**

- Pinned messages and the channel notifications bell: the desktop has
  neither.
- A separate members icon: the face pile and count is the members toggle.
- The composer sticker button: stickers are placed on messages (the hover
  sticker button and picker stay), there is no sticker message to send.
- "Shift + Enter for a new line": the chat input is a single-line field, so
  the hint says "Enter to send" instead.
- "· encrypted" on file cards: a Discord or nearby room's files are not
  sealed, and the card does not know the room's transport.
- The muted-channel dimming and mention-only blue pill: there is no muted or
  mention state per room; every unread count is the accent pill.
- The "Welcome to #channel" banner and the floating quick-reaction toolbar:
  the row keeps its existing hover react, sticker and "⋯" buttons.

**Live switch.** The sidebar, header and composer layouts are built once, so
switching design while running shows the new layout only after the chat page
is rebuilt.

### Settings

In Next, Settings (`MainWindow::buildSettingsSection`, `MainWindowSettings.cpp`;
the page itself is `SettingsSectionsView.cpp`; sheet `20-settings.qss`) follows
the desktop mockup. Classic keeps the old layout: every Next change is behind
`Theme::isNext()` in code, or the `nextLayout` property of the
`SettingsSectionsView`, which `SettingsSectionsView.cpp` reads because that file
is also built into `forkmesh-settings-sections-tests`, which does not link the
design tokens.

- **Page frame.** The page loses its outer margins. Setting `nextLayout` on the
  view (before any section is added) rebuilds its chrome once:
  - A 256 px left column (`#settingsSidebar`, hairline right edge). The search
    field sits in a 12 px band (`#settingsSearchBand`) over a hairline
    (`#settingsSidebarRule`) and gets a leading search glyph (a `QLineEdit`
    action marked `nextSearchIcon`).
  - The scroll area moves into `#settingsMainColumn` above an empty
    `#settingsFooterSlot`. The content column is centred and capped at 824 px
    (760 px plus 32 px padding each side).
- **Section list.** `SettingsNavDelegate` paints Next rows itself when the list
  carries the `nextNavColors` property (role colours passed in from
  `refreshSettingsSectionIcons`, like `HomeBoardStyle`):
  - 32 px rounded rows with a 16 px icon and 13 px text; the lit row is filled
    `surfaceActive`, the hovered one `surfaceRaised`.
  - The section being read is `fg` with an accent icon (the icon's Selected
    mode); the others are muted with an `fgSubtle` icon (Disabled mode).
  - The sub-headings of the current section are 28 px, 12 px rows behind a
    hairline guide, indented 16 px. Row heights stay in step with the hit
    testing (`subLineHeight`), so clicking a sub-heading still jumps to it.
- **Section headings.** Icon and 16 px semibold title, the collapse chevron
  moved to the right of the same row, and the hairline between sections at
  the top of each section instead of under its title (hidden on the first
  visible one). 28 px above and below an open section.
- **Lit heading.** In October 2026 the user asked for the heading on the page
  to light up with the entry lit in the list, so the eye can find where a
  jump landed. `SettingsSectionsView::syncLitHeading` runs whenever the lit
  list row or sub-row changes and sets `settingsLit` on exactly the label the
  list lights: the sub-section's `sectionLabel` when a sub-row is lit, the
  section title otherwise. Next paints it `accentFg` on `accentSubtle`; Classic
  uses the list's selection blue. Titles and sub-headings carry the same
  padding lit or not, so lighting one never moves the page.
- **Outline check.** `forkmesh-window-tests --settings-outline-only` (add
  `--next` for the new interface) opens every section and fails if a group
  card on the page has no heading in the list, if the list and the page
  disagree on the sub-headings, or if a section's heading does not light up.
  It also checks that searching for "halloween" finds the Halloween switch.
- **Groups and rows** (`polishNextSettingsBody`, run on each section body as it
  is built, eager or lazy):
  - In every section card (`sectionCard`) whose column starts with a
    `#sectionLabel` heading, the heading stays on top as a small-caps muted
    label, hint labels right under it stay outside, and everything else moves
    into a bordered `surface` group (`QFrame#settingsGroupBox`, 12 px radius).
  - Each control starts a row (`#settingsGroupRow`, 16/12 px padding). A
    `statusLine` or `modeHint` label stays in the row of the control above it.
    1 px dividers (`#settingsRowDivider`) separate rows.
  - `NextGroupRows` hides a divider and drops the padding of a row whose
    widgets are all hidden, and marks a group with nothing visible `empty`.
    It never hides a container, so a control shown later appears in place.
  - The card itself becomes transparent and unpadded (`settingsGroupCard`).
    `nextMargins` remembers those margins, and they are put back one turn
    after `NextAccess::themeChanged` resets section-card margins.
  - Quick Setup, Secrets and Security, which have no cards, are grouped the
    same way from their column.
  - The heading's 12 px glyph is hidden.
- **Toggles.** A checkbox that is a row of its own is drawn by
  `NextSettingsToggle`, an event filter: label on the left, a 30×18 accent
  switch on the right, with a focus ring after keyboard focus. It is still
  the same `QCheckBox`. Clicking the label toggles it as before, a click on
  the switch calls `click()`, and Space still works. Checkboxes inside
  mixed rows keep the square box.
- **Apps** (`buildSuggestedAppsTab`). The recommended and optional tools are
  two bordered tables in Next instead of a column of cards. Each row shows:
  - the tool name and the Optional badge;
  - its description, its command, and its status pill;
  - compact Test and Install buttons on the right.

  Object names, the status labels and the buttons that
  `refreshSuggestedApps` updates are the same widgets. Rows carry
  `sectionCard` so the tool names stay out of the section list's sub-headings,
  as before.
- **Profile.** The borrowed node profile panel (`MainWindowNodes.cpp`) is
  styled only by sheet rules scoped under `#settingsSectionsContent
  #nodeProfileContent`. The rules cover hairline 12 px cards for the details,
  stat tiles, key and nodes card, outline action buttons, and a 15 px name.
  The Network page's node profile is unchanged.
- **Footer.** The footer row moves into `#settingsFooterBar` in the footer
  slot, under the content column only, with a hairline top edge. Leave node
  and Uninstall (danger outline) sit on the left. Rebuild & restart, Log in
  to a user account… and Log out of account (outline) sit on the right. The
  rebuild status line sits above the bar.
- **Left out on purpose.**
  - The mockup's profile card layout (avatar beside name, "you" badge, a meta
    line, one row of buttons), the device row with platform and memory, and
    key/value Details rows. The panel's layout belongs to `MainWindowNodes.cpp`
    and is shared with the Network page's node profile. Its Details text is
    rich text built there.
  - Sub-headings for every section at once: the list still shows them only
    for the section being read, because the others may not be built yet.
  - Section titles keep their names ("Quick Setup", "SSH Keys"), because
    `settings/sectionsExpanded` and the collapsed defaults key on them.
- **Live switch.** Settings is built once, so switching design while running
  shows the new layout after Settings is rebuilt. The toggles fall back to the
  stock checkbox in Classic straight away.

#### Apps: where each installed app shows up

In Next, the Apps section opens with an "Installed apps" table
(`#settingsPackagePlaces`, built in `buildSuggestedAppsTab`). It sits above
the tool tables, which are unchanged.

- **Which packages.** The same ones Market offers "Show on" for: installed,
  account-synced packages whose manifest lists places.
- **Columns.** A name and summary column, then Show on Phone, Show on Desktop
  and Show on Web. Each place the manifest offers gets an accent `kit::Toggle`
  (`settingsPackagePlace-<place>`); a place it does not offer is a blank cell.
- **Last place.** As on the Market card, the last place left on is disabled,
  with the same tooltip.
- **Saving.** A change takes the same path as Market's `appendPackagePlaces`
  callback: `PackageRegistry::setPlaces`, `syncPackageRuntime`,
  `refreshMarketPage` and `pushAccountPackageChange("places")`. What is saved,
  the settings keys and the Market card are unchanged.
- **Keeping in sync.** `refreshMarketPage` and `syncPackageRuntime` bump the
  table's `placesRevision` property, and a `NextPlacesRefresher` rebuilds it
  one event-loop turn later. So a change made on either side, a registry that
  finishes loading after Settings was built, or an account sync redraws the
  table. The table is hidden while no package qualifies.

### Users

In Next, the admin Users page (`MainWindow::buildUsersSection`,
`MainWindowUsers.cpp`, `84-users.qss`) follows the desktop mockup; Classic
keeps the old layout. The page carries the `usersNextPage` property.

- **Header.** "Users" (15 px, semibold, no `setFont` in Next) over the
  existing status line (`m_usersStatus`, moved into the title column). The
  inline help button stays beside the title. Quarantine, Unquarantine,
  Delete, Send tip and Refresh stay on the right with their object names.
- **Referral code panel** (`#usersSignupReferralCodePanel`) is a hairline card
  on `surface` with 12 px corners.
- **Tabs.** All users and Quarantine stay the `QTabWidget#usersTabs` tabs,
  drawn as underline tabs. A toolbar (`#usersNextToolbar`, the tab widget's
  top-right corner widget) holds "Search people" (`#usersNextSearch`), a role
  filter (`#usersNextRoleFilter`: All roles, Admins, Not admins, read from the
  Admin column) and "N shown" (`#usersNextShown`). Both filter the table of
  either tab by hiding rows; nothing is fetched or written. Search matches the
  user, Discord username, Discord display name and email. A selection never
  keeps a hidden row (`usersNextDropHiddenSelection`), so Quarantine, Delete
  and Send tip act only on rows the admin can see. The filter is re-applied
  in `updateUsersStatus`, which runs after every render and tab change.
- **Table.** Both tables keep `dataTable`, every column, sorting, the column
  menu, extended selection and Shift+Up/Down. Rows are 48 px
  (`UsersNextRowHeight` re-sets the height on `Show`, after the stock data
  table polish sets 38 px).
  - Person (`UsersNextPersonDelegate`, column 0): the uploaded avatar from
    the user directory, else the generated portrait (`forkMeshAvatarPng` of
    the user name, the stand-in Chat and Issues use, so one person keeps one
    face), else an initials circle; then the Discord display name (or the
    user name),
    a "You" badge on the signed-in account (`accountOwner()`), and the muted
    `@user`. Sorting still uses the user name.
  - Email verified and Status get a status dot (`UsersNextDotDelegate`:
    verified green, unverified amber; active green, anything else grey).
  - In Next the visible order starts with User, Admin, Nodes, Email verified,
    Status, Activity recency (the quarantine table: User, Admin, Nodes,
    Quarantine status, Reasons, Quarantined). It is a header move, not saved.
  - Right-click opens a row menu that mirrors the header buttons (Send tip,
    Quarantine, Delete, Refresh; Unquarantine, Refresh on the quarantine
    tab): each item copies its button's text, icon, tooltip and enabled state
    and calls its `click()`.
- **Profile rail** (`#usersDetailPanel`, `buildUsersDetailPanel`,
  `renderUsersDetail`, both designs). Clicking a row in either tab opens a
  360 px rail right of the tabs: the avatar (uploaded, else the generated
  portrait), name and `@user`, the custom status, You / Admin / status badges,
  then every column of that row as label and value, grouped into Account,
  Email, Discord, Activity, Device, Signup, Referrals and Fingerprints
  (Quarantine first on the quarantine tab; any column left out of the groups
  lands in Other). The values are the table's own cell text, so the rail never
  shows a field the table does not. It is opened by a click (`cellClicked`),
  not by selection, so Shift+Up/Down range selection does not pop it open;
  once open it follows the current row. It re-renders after every directory
  refresh, keeping its scroll position, and closes when the user leaves the
  directory, the tab changes, or the close button is pressed. Closing deletes
  the rail's widgets, and losing admin closes it (`updateAdminCrownBadge`), so
  protected fields do not outlive a shared-desktop sign-out.
- **Classic avatars.** Classic keeps its one-line rows, but column 0 of both
  tables gets `UsersAvatarDelegate`: a 22 px round face, the uploaded avatar
  or else the same generated portrait. Both delegates build faces lazily for
  painted rows and cache them, so a directory of a thousand users does not
  render a thousand portraits on every refresh. The rail's Classic look (bold
  name and section titles, muted labels) is set in code, because the rail's
  stylesheet lives in the Next theme only.
- **Avatars after a session restart.** `startSession` empties `m_avatars`;
  it now empties `m_chatDirectoryAvatarDigests` too. The directory only
  decodes an avatar whose digest changed, so keeping the digests left every
  row without its picture until that person uploaded a new one.
- **Left out on purpose.** "Invite people", the Pending invites and Roles
  tabs, role pickers and the Two-factor column: the directory has no invites,
  roles beyond the admin flag, or two-factor state. Tab counts are not added
  to the tab text; the status line carries the counts. The painted "…" row
  button is replaced by the right-click menu, because the table has 59
  columns and scrolls sideways.
- **Live switch.** The header and toolbar are built once, so a live design
  switch shows them after the page is rebuilt.

### Pings

In Next, the Pings page (`MainWindow::buildNotificationsSection`,
`refreshNotificationsTable` and `renderPingDetail` in `MainWindowActions.cpp`,
`85-pings.qss`) is an inbox with a list and a detail pane, as in the mockup;
Classic keeps the grid. The page carries `pingsNextPage`, the table and the
detail pane `pingsNext`.

- **Header** (`#pingsNextHeader`).
  - "Pings" over a summary (`#pingsNextSummary`): "N unread · M in your
    inbox", or "You are all caught up". Unread means an approval waiting, a
    local ping newer than the seen watermark (the same test as
    `unseenLocalPingCount`), or a website ping with no `readAt`.
  - Quiet hours (`#pingsNextQuiet`): a moon icon, "Quiet hours 22:00 to
    07:00" from `forkmesh::pings::loadQuietHours()` ("Quiet hours on until
    07:00" while a window runs) and a `kit::Toggle`. The toggle builds the
    Settings page if needed (`ensureSectionBuilt(1)`) and sets Settings >
    Pings' own `pingQuietHoursCheck`, so the same `saveQuietHours` and relay
    sync run. If that check cannot be found it opens Ping settings instead.
    Days and times stay in Settings. The label refreshes when the page shows.
  - The pause button (`m_pingPauseButton`) stays visible as a ghost button,
    so its pause menu still opens under it.
  - "Mark all read" (`#pingsNextMarkRead`) does what the top bar's pings
    popover does: `markLocalPingsSeen()` and `updateNotificationButton()`.
    It then clears the unread dot on this desktop's rows in place, without a
    rebuild or a network call. Website pings are still marked read one at a
    time when opened, as before, so the button is enabled only while a local
    ping is unread.
  - Settings, Test, Refresh, Delete and Clear move to a hidden holder; the
    "…" button (`#pingsNextMore`) lists them each time it opens, copying text,
    icon, tooltip and enabled state, and calls their `click()`. Del still
    deletes the selection.
  - The "…" menu also has "Show as table" (`pingsNextSetTableMode`). It shows
    the grid's header and columns again, so the `KebabHeaderView` column menu
    (sort, select, hide, show all, move) works exactly as in Classic. The row
    delegate is taken off the Title column while it is on, filters and the
    sort button keep working, and rows are 34 px with no day headings.
    Turning it off remembers which columns were hidden for the next time.
- **Filters.** A segmented control (`#pingsNextFilters`, buttons
  `#pingsNextFilter`) with All, Mentions, Reviews, Errors and Agents and an
  unread count pill each. The category comes from the ping's own kind, link
  and warning flag (`pingNextCategory`, stored in `kPingCategoryRole`):
  errors are warnings and `error` kinds; mentions are mention, thread reply,
  chat, comment and discussion pings; reviews are pull request pings; agents
  are finished agent runs and agent links. Filtering hides rows only, and a
  selection never keeps a hidden row.
- **Sort.** The grid's header is hidden, so a sort button (`#pingsNextSort`)
  offers Newest first, Oldest first, and by title, type, kind, repository,
  from or status, through `sortByColumn`.
- **List.** The grid keeps every column, item, cell widget, role, click,
  double-click, context menu and shortcut; in Next only the Title column is
  shown and `PingsNextRowDelegate` paints the row from the row's own cells:
  an accent unread dot, a 28 px circle with the ping's muted type icon (or the
  sender's initials for mentions and reviews), the title (medium when unread,
  muted when read), a source line (from, or type, plus repository) and a
  short age. Rows are 56 px; when sorted by time the first row of each day
  group gets a TODAY, YESTERDAY or EARLIER heading (`pingsNextLayout`, run
  after each refresh, sort, filter change and page show).
- **Detail.** `renderPingDetail` builds a wider Next pane: a 36 px icon, the
  17 px title, a meta line (source · clock · "Today, 17:03" · a type badge,
  red for errors), then the same actions in a wrapping row (Open …, Send to
  prompt, Copy text, Copy link, Open repository, Quick fixes, Ping settings,
  Delete ping), the ping body in a card headed LOG EXCERPT (errors, monospace),
  MESSAGE (has a sender) or SUMMARY, and the Details facts. With no ping open,
  `#pingsNextEmptyDetail` says "Select a ping", or "Nothing in your inbox" /
  "No pings of this kind" with "Show all pings".
- **Left out on purpose.** Per-ping Snooze and Mark done (pings have no
  snooze or done state; the pause menu is the global snooze and Delete ping
  removes one), "Start a session about this" (Send to prompt is the existing
  equivalent), the quote card's separate author for website pings, and the
  "Errors break through quiet hours" note, which is not true here: quiet
  hours mute every ping kind. A bulk "mark every website ping read": no
  bulk path exists, and one request per ping would be new behaviour.
- **Live switch.** The header, list and pane are set up when the page is
  built; a live design switch shows them after a rebuild.

### Stats

In Next, the Stats page (`MainWindow::buildStatsSection`, `MainWindowStats.cpp`,
`86-stats.qss`) follows the mockup's `pages/Stats.tsx`; Classic keeps the old
layout.

- **Header.**
  - "Stats" sits over the muted summary (`m_statsStatus`, `statsRole="summary"`,
    "N tiles · updated HH:mm:ss"), with a hairline under the header
    (`#statsHeaderDivider`).
  - The actions are on the right in the mockup's order: ghost Reset and
    Refresh (`statsRole="action"`, muted icons through `nextIconRole`), then
    one inverse Add tile (`statsRole="create"`, `@fg` fill, `@bg` text and
    icon).
  - The header row is attached before its buttons are added, and the order on
    screen comes from `insertWidget`. So the buttons keep the creation order
    and parent they have in Classic, and their rail-shortcut paths stay the
    same. Object names, tooltips, menus and connections are unchanged.
- **Board** (`StatsDashboard`).
  - The board sits under a 24 px page padding. Tiles are hairline `@surface`
    cards with 12 px corners and a `@surfaceRaised` hover fill, padded 16 px
    across and 12 px down. Each tile shows a 12 px muted title, a 22 px
    semibold value (28 px on tiles bigger than one square), and a 12 px
    `fgSubtle` note.
  - Time series draw a 1.75 px accent line with no area fill. Bars use the
    accent. Ticks are 10 px `fgSubtle` and the baseline is `@border`.
  - Status shows as a tinted border (`success`, `warning`, `danger`). The
    coloured left edge is drawn only when edge indicators are on.
  - The resize and remove controls are small bordered squares with
    `fgSubtle` glyphs.
  - While a tile is dragged, its landing slot shows as an `accentSubtle` box
    with an accent edge. The tile being dragged gets a `borderStrong` edge
    and no shadow.
  - The empty board is a dashed `borderStrong` box with the heading "No tiles
    on the board" and the hint "Use Add tile, or Reset…".
- **Colours.** `StatsDashboard.cpp` is also built into
  `forkmesh-stats-dashboard-tests`, which does not link the design tokens.
  So the window passes the role colours in as the dynamic property
  `nextColors` (a `QVariantMap`, plus `edges` for edge indicators) instead of
  calling `Theme::` there.
  - The map is set by `applyNextChartColors`: at build, on every refresh, and
    on the board's `StyleChange`, `PaletteChange` and `Show` events, through
    an event filter (`NextChartColorSync`).
  - The board re-lays itself out on `DynamicPropertyChange`.
  - Classic never sets the property, so the board paints exactly as before.
  - The packing, square cells, hit areas and every edit, menu and tooltip are
    unchanged.
- **Left out on purpose.**
  - The mockup's horizontal "breakdown" bars and its fixed-height
    (non-square) tiles: the board's square packing, point hit-testing and
    the Network-tile click-through depend on them.
  - The grip handle: the whole tile already drags.

### Component health

In Next, Component health (`MainWindow::buildComponentHealthSection`,
`MainWindowComponentHealth.cpp`, `ComponentHealthBoard.cpp`, `87-health.qss`)
follows the mockup's `pages/Health.tsx`; Classic keeps the old layout.

- **Header.**
  - "Component health" sits over the muted scan summary
    (`m_componentHealthStatus`, `healthRole="summary"`), with Rescan as a
    ghost action on the right.
  - Source files and Binary size become underline tabs below the summary
    (`healthRole="tab"`): the active one has `@fg` text and an `@accentFg`
    underline. A hairline (`#healthHeaderDivider`) ends the header.
- **Toolbar.**
  - The existing source tools widget (`m_componentHealthSourceTools`,
    `healthRole="toolbar"`) becomes a full-width row with a bottom hairline:
    All files (back), the filter at 224 px with a search icon, then a
    right-aligned "N files" / "N functions" count (`#healthCardCount`) and
    the sort menu.
  - It still hides on the Binary size tab.
- **Cards** (`ComponentHealthBoard`).
  - Cards follow the mockup's `auto-fill, minmax(176px, 1fr)` grid: 92 px
    high, a 10 px gap and 24 px padding.
  - Each card is hairline `@surface` with 12 px corners and a
    `@surfaceRaised` hover fill. It shows:
    - the name, 14 px medium, elided from the left;
    - the component, 12 px muted;
    - "N lines" over the stall and flag facts (or "No flags"), in
      `fgSubtle`;
    - the score, 18 px semibold, in the success, warning or danger role.
      The 80 and 55 band thresholds are unchanged.
  - Cards scoring under 80 get a border tinted in their band's colour.
  - Colours come through the same `nextColors` property as Stats
    (`applyNextHealthColors`, `NextHealthColorSync`), because the board is
    also built into `forkmesh-component-health-board-tests`.
- **Detail panel** (`#healthDetailPanel`, 360 px, Next only).
  - It opens beside the function board when a file is open (the existing
    drill-in) and closes on All files, the panel's ×, or the Binary size
    tab. The × (`#healthDetailClose`) calls the same
    `showComponentHealthOverview()` as All files.
  - It shows only data from `FileHealth`:
    - the name and the path, in mono and selectable;
    - the component and line count;
    - the score with a band word: Healthy, Needs attention or At risk;
    - a facts card: code lines, functions, longest function, stall
      attributed, acknowledged in code, and last changed;
    - "FLAGS · N" with one row per finding kind ("N × kind"), or "Nothing
      flagged in this file".
- **Binary size.**
  - Four stat tiles (`#healthStatTile` inside `#healthBinaryTiles`) are read
    from the existing report: total size, symbols, object files, and the
    largest top-level part with its share. They are hidden while there is no
    report.
  - The breadcrumb line (`healthRole="crumbs"`) and the treemap are
    unchanged.
- **Widget paths.** Every existing widget keeps its parent and creation order.
  The new containers are named `QFrame`s, so the class-index paths of the
  existing `QWidget`, `QPushButton` and `QLabel` siblings do not move. Object
  names, sort modes, the filter, the drill-in, Rescan, the progress row and
  every tooltip are unchanged.
- **Left out on purpose.**
  - Component picker and "Only flagged" switch: the page has no such
    filters, and adding them would be new logic.
  - The four score-part progress bars (Size, Complexity, Stalls, Flags):
    the scanner computes a single score.
  - Open in editor and "Draft a fix with an agent": the page has no such
    actions.
  - The selected-card highlight: a click drills in instead of selecting.
  - "Size by component" ranked list and "Biggest symbols" table: the treemap
    already shows both and zooms. The binary's delta since the last
    release is not recorded.
- **Live switch.** The header, toolbar and panel are built once, so a live
  design switch shows them only after the page is rebuilt. The boards' colours
  do follow dark/light and design switches.

### Calendar

In Next, the Calendar page (`MainWindow::buildCalendarSection`,
`MainWindowCalendar.cpp`, `46-calendar.qss`) closes the remaining gaps to the
mockup; Classic keeps the old layout.

- **Header.** The title sits over a muted line: "Today is Friday, October 23"
  (refreshed on Show) and the existing `m_calendarStatus` (event count and time
  zone), so the time-zone chip is hidden as a duplicate. On the right: an
  outline Today, ghost ‹ › steps, the range label (`m_calendarHeading`, now
  "Oct 19 – 25, 2026" style in Next via `calendarNextRangeLabel`), a segmented
  Day / Week / Month / Agenda control (`#calendarViewTabs`) and New event. The
  segmented buttons only call `setCurrentIndex` on the existing mode combo,
  which stays (hidden) and keeps driving `m_calendarMode`; the combo's own
  changes check the matching segment.
- **Sidebar.** A painted mini month (`CalendarMiniMonth`,
  `#calendarMiniMonth`) above the search field and calendar list: today in
  the accent, the shown day, week or month in `accentSubtle`, its own month
  steps. Picking a date sets `m_calendarAnchor` and runs the same refresh and
  render the ‹ › buttons run. It is built only in Next and hidden if the page
  is shown in Classic.
- **Week view.** `CalendarWeekHeader` (installed in Next only, stock painting
  otherwise) draws each day as a small-caps weekday over a date circle, today
  in the accent; the week start reaches it through the table's
  `calendarWeekStart` property, set by `renderCalendar`. Hours read "09:00",
  rows are 48 px, the view scrolls to 08:00 the first time, weekend columns use
  `surface`, and the current hour cell of today draws the accent now line and
  dot.
- **Month view.** Upper-case weekday captions, weekend cells on `surface`,
  days outside the month keep faint text on the plain background.
- **Event chips** (month and week) are `accentSubtle` with a 25% accent
  hairline and `accentFg` titles, the mockup's "team" block.
- **Left out on purpose.** Per-calendar colour dots and per-calendar block
  styles (events carry no calendar colour the page uses), duration-spanning
  event blocks and lanes (the week view is one cell per hour; spanning blocks
  would need a new view), the event popover (double-click still opens the
  event dialog), attendee avatars, and a time-grid Day view (Day stays the
  list).

### Specials

In Next, the Specials page (`MainWindow::buildTeamSection`,
`buildTeamSectionHeader`, `MainWindowTeammates.cpp`, `88-specials.qss`)
follows the mockup; Classic keeps the old header and the video tiles.

- **Header.** `buildTeamSectionHeader` builds the same buttons into its
  `HeaderWrapLayout`, then in Next replaces that layout on `teammatesTopBar`
  with two rows; every button stays a direct child of `teammatesTopBar`, so
  object names and widget paths are unchanged.
  - Row one: "Specials" over `#teammatesNextSummary` (the existing
    `m_teammatesSummaryText`, refreshed by `loadTeammatesPanel`), the repository
    chooser (its "WORKS ON" caption hidden), icon-only ghost Run special agents,
    Run all in one session, Disable all schedules, Refresh, the List / Grid
    view toggles, and the accent Add Special Agent button.
  - Row two: Dashboard, Briefing and Stats, then All / Working / Scheduled /
    Idle as rounded filter pills with their counts, and a "Search specials"
    field (`#teammatesNextSearch`). The field filters the cards with the same
    match as the window's top search box, which still wins when it has text.
  - `NextTeamButtonStyler` paints the `VerticalIconButton`s as pills, icon
    buttons and the create button through an event filter, the way
    `NextRepoButtonStyler` does on the repository page; the "All" pill is a
    painted label for the "Special agents" metric.
- **Cards.** In grid view `buildTeammateRosterCard` builds a `NextSpecialCard`
  (`QPushButton#teammateCard`, property `nextSpecialCard`) instead of the
  video tile: initials tile, name, specialty, status at top right (accent
  while working, amber for "Needs you" when the mood asks for attention or
  "Done" when a finished run is unseen, red after a failed run, grey
  otherwise), the latest run caption (or the description), the schedule
  (`humanSchedule`), the next run (`humanNextInvocation`, the log monitor
  detail, or the last-run age), and bars for the last seven runs from
  `teammateHomeRuns` (success neutral, failure red, stopped short, running
  accent, empty slots outlined). Click opens the special's page and the
  context menu is `showTeammateRailMenu`, as before; run, mood, menu and
  payouts (`addTeammateRailHud`, same object names) sit in a hover toolbar
  (`#teammateNextCardHud`). The card whose profile is open gets an accent
  border.
- **Profile panel.** `showTeammateDetail` swaps the "Special agent profile"
  title for an initials tile, name, role and status, keeps Save and close, and
  adds underline tabs Instructions / Schedule / Runs / Settings
  (`#teammateNextPanelTabs`). The tabs scroll the one profile column to the
  Instructions, Schedule and Activity sections and the settings form, and
  follow the scroll position; nothing moves out of the column, so every
  existing control stays. The hero's face, name and role are hidden as
  duplicates of the header. The panel is a flat column with a hairline left
  edge and 12 px section cards.
- **Left out on purpose.** The pin mark (specials have no pinned state on
  this page), a "Needs you" filter (no such filter exists; the status shows
  it instead), the "Your AI executive team" tagline and the
  Instructions-only editor with its own Save (instructions are part of the
  one settings form and its Save in the header).

### Calories

In Next, the Calorie Counter package
(`marketplace/packages/calories/content/desktop/CaloriesFeature.{h,cpp}`)
follows the mockup. Like CRM it includes `DesignTokens.h` and
`CaloriesFeature::applyNextStyle` sets one `@role` sheet on `caloriesSection`;
`arrangeNextHeader` rebuilds the header. Classic is unchanged.

- **Header.** "Calorie counter" over the muted summary; on the right the
  status line, a ghost Export my data and a "…" menu (`#caloriesMoreButton`)
  that mirrors Refresh and Uninstall… from a hidden holder
  (`#caloriesMenuHolder`): each item copies its button's text, tooltip, enabled
  state and visibility and calls `click()`, so the install state still decides
  what is offered.
- **Tabs.** The `caloriesTabs` tab bar is styled as underline tabs on the
  header's bottom hairline (document mode); "Goals & reminders" reads "Goals
  and reminders".
- **Toolbar.** Today (shown only off today), a ‹ date › stepper around the
  existing date edit, which reads "Today, Oct 23" (`syncNextDay`; the steps
  set the date like typing it), the food search with a search icon,
  icon-only Photo, Describe, Barcode and Weigh in (same buttons, text moved to
  the accessible name, tooltips added), and an inverse Add food.
- **Cards.** A calorie ring (`CaloriesRing`, accent, amber over goal) with
  Remaining / Over goal and today's entry count; Protein, Carbs and Fat rows
  with 6 px accent bars (the existing progress bars); a streak card (days
  logged in a row, days within goal) and a weight card (latest weigh-in, its
  day, distance to target). The old calorie label, bar, caption and streak
  and weight labels still update but are hidden.
- **Diary table.** No grid or alternating rows, 40 px rows, a bordered 12 px
  container. Consecutive entries of the same day and meal get a heading band
  painted by `CaloriesEntryDelegate` ("Breakfast  196 kcal", the day added for
  past days; the sum covers that meal's whole day), so the Day and Meal
  columns are hidden and macro cells read "6 g". Rows stay one per entry, so
  selection, double-click to edit, Edit / Delete and search hits are
  unchanged.
- **Quick add.** A hairline-separated column: title and Scan receipt, meal
  combo and Filter foods, then the list painted by `CaloriesQuickDelegate`
  (small-caps headings, muted notes, name and kcal rows with a hover fill),
  Log / Save / Remove, and the footer hint.
- **Trends, Goals, Challenges.** 24 px page margins; the charts, the goals
  form and the reminders sit in hairline cards; the reminders and challenges
  tables are `dataTable`.
- **Left out on purpose.** Empty meal groups ("Dinner  Nothing yet": the
  diary lists entries, it has no fixed meal slots), the per-row hover trash
  and star / + buttons (Delete, Save and Log do the same), "−0.6 kg this
  week" (no weekly weight change is computed) and Package settings in the
  "…" menu (the package has no settings page).
- **Live switch.** The layout is built once, so a live design switch shows
  the new layout only after the page is rebuilt.

### Repos

In Next, the Repos page (`MainWindow::buildNetworkReposSection`,
`MainWindowNetworkRepos.cpp`, `31-repos.qss`) follows the mockup's
Repositories page; Classic keeps the old layout. The page carries the
`reposNextPage` property, the tab widget and both tables `reposNext`.

- **Header.**
  - "Repositories" (15 px semibold) with an "About repositories" help button
    (`inlineHelpButton`) that holds the old long description, word for word.
  - Under it one muted line (`mutedLabel`, `reposRole="summary"`): "Grouped
    by owner across the active relay. Mirror hosts fold into one row per
    repository."
  - On the right: a Cards / Table toggle (`reposNextViewToggle`, two
    `reposNextViewButton`s; the grid glyph is painted, the list glyph is
    `list-unordered`), then Simple list (only in Table view), Refresh and
    Import folder as ghost buttons (`reposRole="ghost"`), then New repository
    as the inverse button (`reposRole="inverse"`, `plus` icon). Same buttons,
    object names, tooltips and slots; only their order in the header changes.
    The view choice is kept in memory only (no new setting); Cards is the
    default.
- **Tabs.** "Remote" and "Private and local" (Classic: "Remote repositories",
  "Private / local"), same order and index, underline tabs with an accent
  underline. Each tab carries a count pill (`reposNextTabCount`, a
  `QTabBar::setTabButton` label) filled from the rendered row counts. Each
  tab page gets 16 px top padding.
- **Toolbar per tab.**
  - "Filter repositories" (`reposNextFilter`, `reposNextLocalFilter`) hides
    rows and their cards whose Repository cell (name, access, branch, commit,
    message, author) and owner/name key lack the text. Nothing is fetched.
    The text is kept in the table's `reposNextFilterText` property and
    re-applied after every render. With nothing left, the card list says so
    (`reposNextCardEmpty`).
  - The status line (`m_networkReposStatus`, `m_localReposStatus`,
    `reposRole="status"`, plain text) sits on the right.
  - On Private and local, the "Private & local" heading is dropped (the tab
    says it) and its description moved into an "About private and local
    repositories" help button.
- **Cards view** (`reposNextCards`, `reposNextCardsLocal`, one
  `reposNextCard` per table row, rebuilt by `reposNextRebuildCards` at the
  end of `renderNetworkRepos` and `renderLocalOnlyRepos`). Each card reads
  the row's catalog record (kept on the Repository item under
  `kRepoNextRecordRole`, Next only) and the row's own cells, so both views
  show the same data.
  - Top: a repository tile (`git-branch`; `lock` for a private local copy),
    the owner/name, the Access line as pills (Public / Private outline, Live
    host accent, the local copy label neutral, a terms flag in danger), a
    "Current" accent pill when the row is the repository open in the app
    (`m_repoDetailIndex`, `kRepoNextCurrentRole`), the published description,
    the checkout path on local cards, and the last commit: author initials,
    subject and age.
  - Local cards also show the working-tree state pill (`reposNextCardDirty`:
    "Checking changes...", "Clean", or "Dirty: N files" in warning). It is
    updated when the background `git status` scan returns.
  - Actions: the row's real buttons, mirrored. The first (Switch) is inverse,
    or an outline "Open" on the current repository. The next two (Fork or
    Forked, Mirror or Mirrored; Settings on local cards) are ghost buttons.
    Web and Delete sit in a "…" menu. Each mirror copies the button's text,
    tooltip, icon and enabled state and calls its `click()`; menu items do it
    after the menu closes.
  - Lower band, four hairline-split cells:
    - Issues, Discussions, Copies (worktrees plus artifacts) and Storage,
      with the table's Contents text as the tooltip.
    - Commits and Merges sparklines (`ReposNextSparkCell`) with "N · 52 wk".
      Bars are `fg` at 35%, the last four weeks `accent`, empty weeks a 2 px
      stub, or "Activity not published". The tooltip is the table chart's.
    - Host: machine, mirror count (follows the async `/mirrors` reply through
      `reposNextSyncCardMirrors`), platform and runtime, version, CPU, disk
      use, and "Sync via <source> · synced …", with the table's Host
      tooltip.
  - A value the record lacks prints an em dash; nothing is invented.
- **Table view.** The existing tables with all seven columns, the column
  menu, double-click, tooltips and Simple list. In Cards view a table is
  kept at zero height rather than hidden: `refreshMirrorAdverts` only runs
  while `m_localReposTable->isVisible()`, so hiding it would stop the local
  git figures from refreshing.
  - Next turns the grid and alternating rows off; `NetworkRepoRowDelegate`
    paints a hairline under each row, the whole-row hover fill and the
    selected band (`surfaceActive`). `ReposNextTableChrome` (a viewport event
    filter, also on each actions cell) tracks the hovered row in
    `reposNextHoverRow`.
  - Repository cell: the mono owner/name with the Access line as badges,
    then muted label and secondary value pairs; Contents and Host the same.
  - Simple list rows are 46 px: the mono name, then the rest muted.
  - The activity charts use the same role colours as the cards in Next only;
    Classic keeps green and purple.
  - Row actions: the same buttons in the same slots, 80 by 26 px; Switch
    outline, Fork, Mirror, Web and Settings ghost, Delete ghost with danger
    text, icons tinted through `nextIconRole`.
- **Left out on purpose.**
  - Author avatars: there are no avatar images for commit authors, so the
    card shows initials.
  - "Clone to this Mac" in the "…" menu: this page has no clone flow.
  - The local rows' ahead / behind counts: nothing on this page measures
    them.
- **Live switch.** The header, toolbars, toggle icons and table setup are
  built once, so a live design or scheme switch shows them after the page is
  rebuilt. Cards are rebuilt on every render; the delegate and charts check
  the design when they paint.

### Git

In Next, the Git page (sidebar Git; `MainWindow::buildSourceControlPanel` in
`MainWindowSourceControl.cpp`, `52-git.qss`) follows the desktop mockup: a
page header with underline tabs over three columns. Classic keeps the old
layout. The page around the panel is still built by `buildRepoCommitsTab`
(`MainWindowRepoDetail.cpp`, not edited); the Next chrome is added from the
panel's own `ScmDesignHook`, one event-loop turn after the panel is polished
or shown, so it only runs once the workspace exists. `ScmNextChrome` (a child
of the panel, object name `scmNextChrome`) holds the Next widgets and
re-arranges both ways on a live design switch: Next to Classic puts every
moved widget back where `buildRepoCommitsTab` placed it, in the same order.
A window that starts in Classic is never touched.

- **Header** (`QFrame#gitNextHeader`, inserted above `m_gitWorkspaceStack`
  in the same page layout).
  - "Git" (`#gitNextTitle`) over a muted summary (`#gitNextSummary`): the
    repository name and "N ahead" from the outgoing probe the page already
    runs (`m_scmOutgoingProbePendingCount`, only when it was read for this
    checkout).
  - On the right, the real history-header controls, moved (not copied) out of
    `#gitHistoryHeader`: the branch picker pair (`m_commitsBranchButton` with
    its compare arrow and compare-base button, so its menu, Create new branch
    and switching are unchanged), Fetch (`m_commitsFetchButton`), Pull
    (`m_commitsPullButton`) and the rescan button (`m_scmRefreshButton`).
    Leaderboard stays in the history header.
  - A new Push button with "↑N" calls `pushCurrentRepoUpstream`, the same
    slot as Sync Changes. It is disabled while a push or sync runs, when there
    is no working tree, and when the probe says nothing is ahead.
  - Outline buttons are 28 px on `surface` with a hairline; icons are
    `fgMuted`.
- **Tabs** (`QPushButton#gitNextTab`, `accentFg` underline when checked).
  "Changes (N)" uses the count the Changes header already shows
  (`m_scmCountLabel`). Changes shows the file pane (`m_gitFilesSlot`) and
  History the commit list (`m_gitHistorySlot`) in the left column of
  `gitWorkspaceSplit`, which used to stack them. The tab follows the detail
  stack: opening a commit selects History, the changes or branch-range page
  selects Changes. Clicking Changes leaves an open commit for the changes
  page and runs `refreshSourceControl()`; clicking History runs
  `refreshCommitMarkersIfStale()` and reopens the last commit when it is in
  the loaded list. Both are the refreshes the page runs when it is shown,
  since a hidden pane skips them.
- **Left column** (`GitSidePanel`, `QFrame#gitNextSide`, 240 px, hairline on
  the right), beside `m_gitWorkspaceStack` in a new row layout of the same
  page (the stack keeps its parent). `GitSideRow` paints 28 px rows: foldable
  small-caps headings with a right count, the current branch on
  `accentSubtle` with an `accentFg` check, muted icons, mono `fgSubtle` path
  lines. It shows only data the page already holds, read when the menus are
  rebuilt (`ChildAdded` on their buttons) and on every source-control
  refresh:
  - Branches: the checkable entries of the branch picker's menu; a row
    triggers that entry (`switchToBranch`).
  - Remotes (folded): the entries of the Code toolbar's remotes menu; a row
    triggers it (copies the URL).
  - Worktrees: the rows of the Worktrees table when it was loaded for this
    repository (name and home-shortened path; a row calls
    `switchToWorktree`). Otherwise the toolbar badge's count and an "Open the
    worktree list" link that clicks the Worktrees button.
  - Tags (folded): the toolbar badge's count and an "Open tags and releases"
    link that clicks the Tags button.
- **Change list** (middle column).
  - Groups are `ScmGroupRow`s: "STAGED 2" / "UNSTAGED 5" (11 px medium,
    letter-spaced, `fgSubtle`), with "Unstage all" / "Stage all" text buttons
    (`#scmGroupAction`) that call `scmUnstageAll` / `scmStageAll`. They take
    the place of the "−" and "+" glyphs, which stay in Classic; Open all and
    Discard all stay in both. Range view reads "AGAINST main 3".
  - Rows (`ScmFileRow`, now 32 px) are arranged per design from the same
    widgets (`ScmRowParts`): a checkbox (`#scmStageCheck`, staged = ticked;
    it calls `scmStagePath` / `scmUnstagePath`), the status letter in mono
    (A and ? `success`, D `danger`, M `fgMuted`), the muted directory with a
    trailing slash, then the file name. The hover actions (stage, unstage,
    discard, open) and the context menu are unchanged. Classic keeps icon,
    name, directory, status. The checkbox is created only when the row is
    built in Next, so after a live switch it appears with the next refresh.
  - The tree drops the root chevrons in Next (groups still fold by
    double-click or the keyboard) and indents only in folder mode.
  - The "▾ CHANGES" caption and its count are hidden; the tab carries the
    count. Prev, Next, Folders, All repos and the auto-view eye stay.
- **Commit box** (bottom of the middle column, under a hairline
  `#scmNextCommitRule`).
  - "Summary (required)" (`#scmSummaryInput`) and "Description"
    (`#scmDescriptionInput`) mirror the existing message box
    (`m_scmMessage`, hidden in Next): the message is the summary, a blank
    line and the description; auto-fill and Generate write the message box
    and the fields follow. Commit reads `m_scmMessage` as before. Ctrl+Enter
    commits from either field. The sparkle and "⋯" generation buttons sit
    beside the summary.
  - "Commit to <branch>" is the existing Commit button (accent, commit icon;
    the branch is read from the working tree's HEAD file). Its "▾" half
    (`#scmNextCommitMore`) opens a menu that clicks the existing Commit,
    Commit & push and Stage & push buttons, which are hidden in Next. The
    controls row is a plain row in Next and the original `FlowLayout` in
    Classic. The Sync Changes row and its live status line stay.
- **History rows** (`ScmCommitRowDelegate`, now 36 px): the short hash
  (mono, `fgSubtle`) and the relative age from the list's own hash and date
  columns are painted on the right, before the ref chips.
- **Diff pane.** Unchanged: it already uses the Next diff renderer (status
  glyph, mono path, +/− counts, hunk rows, line gutters and success/danger
  bands).
- **Left out on purpose.**
  - "N behind", "Pull ↓N" and "Last fetched …": the page never computes how
    far the upstream is ahead or when the last fetch ran; each would need a
    new git call.
  - Stashes: the app reads no stash list.
  - Remote branches under Remotes and the tag list under Tags: only remote
    names and the tag count are in memory.
  - The mockup's per-row "⋯" menu, its discard dialog and "Tick a file to
    stage it": the existing hover buttons, context menu and confirmations
    cover these.
  - Commit author avatars in History: author names stay as text.

### Ledger

In Next, the Ledger page (`MainWindow::buildLedgerSection`,
`MainWindowLedger.cpp`, `89-ledger.qss`) follows the desktop mockup
(screenshots 19 and 20, `pages/Ledger.tsx`). Classic keeps the old layout.

The Market package's `LedgerFeature.{h,cpp}` holds no widgets (records,
totals, wallets, the payout plan), so it is unchanged. The page carries the
`ledgerNextPage` property. Every rule in the sheet is scoped to
`#ledgerSection`, which also outranks the older unscoped ledger rules in
`90-data.qss`.

- **Header.**
  - "Ledger" (15 px, semibold) over the muted line "Every relay-owned
    financial record. Amounts are exact; USD is a live spot estimate.", then a
    hairline (`ledgerRule`).
  - The old long description is the tooltip of the title and summary, and the
    title's accessible description. Its text is unchanged.
  - On the right: ghost Refresh and Open in browser, an outline "+ Add record"
    (`#ledgerAddRecordToggle`, new) and an inverse Send payment…
    (paper-airplane icon).
  - Button looks come from the `ledgerVariant` property (`ghost`, `outline`,
    `inverse`, `icon`). Icons use `nextIconRole`.
- **Add record popover.** The entry form (`#ledgerEntryForm`) becomes a popup
  (`Qt::Popup`, still a child of the page; `ledgerPopover` property).
  - Add record opens it under the button, aligned to its right edge and kept
    on screen, and focuses Kind.
  - It holds a "New record" heading with a close button, then every existing
    field in its old order: type, date, kind, what; to/from, amount (now
    right-aligned), currency, Not settled yet, and the existing Add record
    button (inverse). The "stays on this machine" hint follows.
  - Return in any field still adds the record. Esc or a click outside closes
    the popup.
- **Wallet cards.** `makeLedgerNextWalletCard` builds the Next card; Classic
  keeps `makeLedgerWalletCard`. The strip is still one `QHBoxLayout` of
  cards, so the card count and text tests read it the same way. Each card is
  a 12 px rounded `surface` card with a wallet icon and the uppercase label.
  - **Linked:** a 20 px semibold balance, then "≈ $" with the mono,
    explorer-linked short address, and the reading's age on the right. The
    header row holds refresh, remove (tracked wallets only) and a QR button
    (`#ledgerWalletQrButton`).
  - **Not linked:** the empty title ("Not published", "Not linked") at 15 px
    over the existing hint, and an outline button
    (`#ledgerWalletSetUpButton`):
    - "Publish address" opens Settings → Profile (`openAccountManagement`)
      and focuses the Solana payout field (`m_settingsSolanaEdit`).
    - "Link wallet" opens the same section and focuses the profile's
      "Link a wallet…" button (`m_profileTipLinkButton`), which is where the
      hint already points.
  - **QR popover** (`#ledgerWalletQrMenu`): the existing `QrCode` encoder
    draws the real `solana:<address>` code on a white rounded tile
    (`ledgerQrTile`). Under it are the short mono address (the full one in
    its tooltip) and a copy button (`#ledgerWalletQrCopy`) that puts the full
    address on the clipboard.
- **Totals as cards** (`#ledgerNextStat`, labels
  `ledgerStat{Expenses,Income,Records,Awaiting}{Value,Note}`).
  - Expenses and Income show the SOL (or dollar) total with "≈ $" under it;
    Records shows the page count with "N on-chain"; Awaiting payment shows the
    count with the amount owed.
  - `refreshLedgerNextStats` fills them at the end of `rebuildLedgerTable`
    from the same `LedgerTotals` and `LedgerPage` the old labels use. The
    totals tooltip, with its counts, is copied onto the Expenses and Income
    cards.
  - `ledgerTotals` and `ledgerSummary` still update but are hidden.
- **Table card.** A 44 px `surface` toolbar (`#ledgerNextToolbar`) sits on
  the table to make one 12 px bordered block. It holds:
  - An All / Awaiting payment segmented control that checks and follows the
    now hidden `ledgerPendingOnly` checkbox, so it runs the same
    `rebuildLedgerTable`.
  - "Search records" (`#ledgerNextSearch`). It hides rows whose date, record,
    title, person, amount, status or transaction does not match. It is view
    only and is re-applied after every rebuild and sort (`layoutChanged`).
    Totals still cover the whole page.
  - The live SOL price (`#ledgerNextPrice`).
- **Table.** Every column, sort, the column menu, the links and the row
  buttons stay.
  - Headers are 36 px, `fgSubtle`; "To" reads "To / from"; Amount is
    right-aligned. Rows are 48 px.
  - The Title column is hidden in Next and drawn as the muted second line of
    Record. "Show all fields" in the column menu brings it back, and then the
    second line goes away.
  - `LedgerNextRowDelegate` paints hairline rows, a whole-row hover and
    `accentSubtle` selection.
    - Amounts are right-aligned mono with the USD part on a second line
      ("≈ $0.21", or "$x at send"); income is `success`.
    - Status is the relay's text with a capital first letter and a dot from
      `kLedgerStatusToneRole`: accent while awaiting payment, amber while a
      payout is held, green once paid, red for a failed or cancelled status.
    - Signatures are mono `accentFg` links. Rows with an action leave
      Transaction blank instead of "—".
    - The Record tooltip merges the Title tooltip while Title is folded.
    - In Classic the delegate defers to `QStyledItemDelegate`.
  - The row's pay button reads "Pay reward" in Next ("Send payment" in
    Classic). Pay and Settle are small outline buttons; Remind and Remove are
    ghost buttons.
- **Footer.** The status line is 12 px muted, and the "Paying a reward…" note
  is 12 px `fgSubtle`. The payout-hold notice keeps its amber card.
- **Left out on purpose.**
  - The "Missing receipt" filter, its amber status and "Attach receipt": a
    `LedgerRecord` has no receipt field and nothing attaches one.
  - Putting Pay reward inside the Transaction column: the buttons stay in
    their own last column (`kLedgerActionColumn`), which window tests and the
    in-place payment updates read. The empty Transaction cell beside them
    keeps the look close.
  - USDC as a currency.
- **Live switch.** The page is built once, so a live design switch keeps the
  layout it was built with. Wallet cards are rebuilt with each refresh and
  follow the current design.

### Analytics

In Next, the Analytics page (Market package `analytics`,
`marketplace/packages/analytics/content/desktop/AnalyticsFeature.{h,cpp}`)
follows the mockup. Like CRM and Calories it includes `DesignTokens.h`; the
constructor builds the Next layout when `Theme::isNext()` is on, and the
overlay `desktop/resources/theme/next/77-analytics.qss` styles it through
`analyticsRole` properties and object names under `#analyticsSection`.
Classic is unchanged. The Conversions and Experiments pages
(`AnalyticsManage.cpp`) are not restyled; they only get 24 px page margins.

- **Header.** "Analytics" (`pageTitle`) over the muted status line
  (`m_status`, `analyticsRole="summary"`), which reads "1,715 events in the
  last 7 days · updated 9:05 PM" (`rangePhrase`; Classic still says "7d").
  Loading and error messages still land in the same line. On the right: the
  organization and project combos (the project combo is the mockup's site
  picker and still shows only when there is more than one project), the
  24h / 7d / 30d / 90d buttons inside a segmented frame
  (`#analyticsRangeSegment`; `applyState` hides the frame with the buttons),
  and icon-only Refresh (`sync`) and Open in browser (`link-external`)
  buttons. Their text moved to the accessible name; the web button keeps its
  long tooltip after "Open in browser".
- **Tabs.** Overview, Conversions and Experiments (the existing
  `m_sectionGroup` buttons) move into an underline tab row over a full-width
  hairline (`#analyticsHeaderDivider`), active tab underlined in `@accentFg`.
- **Stat cards.** Visitors, Pageviews, Sessions, Bounce rate and Avg. session
  are 12 px hairline cards (`StatCard`, `#analyticsStatCard`): muted label,
  22 px value (`#analyticsKpi`), then the delta (`#analyticsKpiChange`,
  `analyticsTone` up / down / flat: green, red, muted) and a faint "vs
  previous" (`#analyticsKpiHint`). Visitors and Pageviews are clickable
  (mouse, Space, Enter): the selected card gets the accent border, the
  accent-subtle fill and an accent label, and the chart switches to that
  series (`selectChartMetric`). Both series were already loaded; no request
  is added.
- **Chart.** A card (`#analyticsChartCard`) with "Pageviews per day" /
  "Last 7 days · hover a day for its value" ("per hour" / "hover an hour" for
  24h), a line / bar toggle (`#analyticsChartKind`, view-only state) and the
  existing `AnalyticsChart`, which in Next paints one accent series: dashed
  hairline gridlines at nice thirds, a solid zero line, faint 10 px axis
  labels, "Oct 16" dates, a hover guide and dot (line) or dimmed other bars
  (bar). The tooltip text (both counts) is unchanged.
- **Lists.** Top pages, Referrers, Landing pages, Countries, Devices, Events,
  Browsers and Search terms sit in a three-column grid of cards
  (`analyticsRole="card"`), each with a 40 px head: title left, the value
  column's header in small caps right (VIEWS / VISITS / COUNT). The tables
  are the same `QTableWidget`s, header hidden, no grid or alternation, 34 px
  rows, painted by `RankedRowDelegate`: an accent-subtle bar proportional to
  the row's value (stored in `Qt::UserRole` on column 1 in Next), label left,
  value right. A third column moves visually before the value
  (`moveSection`, logical columns unchanged) and reads "13% bounce" or
  "12 visitors".
- **Campaigns, Recordings, setup.** Campaigns is a full-width card with a
  plain hairline table (`analyticsRole="grid"`, 36 px rows). Below it, the
  Recordings panel (`#analyticsRecordingsPanel`, no longer a fixed 380 px
  side column) and an "Add this project to a site" card
  (`#analyticsSetupCard`: Copy snippet with a copy icon, the write key line in
  a monospace box, plain text) share a row. The recordings table shows only
  when there are recordings; the note and Turn on recordings stay.
- **Left out on purpose.** A Recordings tab (recordings are a panel on
  Overview, not a section); the Sessions and Bounce rate chart series (the
  overview query returns only Visitors and Pageviews per bucket, so those
  cards are not selectable); the bounce-rate and average-session deltas and
  "median 4m 12s" (the page computes no previous bounce or session length
  and no median); the chart's Visitors-bars-plus-Pageviews-line legend (one
  series at a time, both values in the tooltip).
- **Live switch.** The layout is built once, so a live design switch shows
  the new layout only after the page is rebuilt. Icons are tinted when built.

### Market

In Next, the Market page (`MainWindow::buildMarketSection`, the card builder in
`refreshMarketPage`, `MainWindowMarket.cpp`, `79-market.qss`) follows the
mockup (`pages/Market.tsx`, screenshot 24). Classic is unchanged: every Next
branch is behind `Theme::isNext()` and the sheet is a Next overlay.

- **Header.** "Market" (`#sectionTitle`, accessible name still "ForkMesh
  Market") over `#marketNextSummary`, "N packages · M installed", refreshed by
  `applyMarketSearch` from the same totals as the tab counts; "from verified
  creators" is added only when every listed package's creator is verified. On
  the right: the search field with a search icon, the sort combo, a ghost
  Install all (N) with a download icon and an inverse Publish (the same
  `marketPublishButton`, text shortened from "Publish an item", upload icon).
  The page has no outer margins; the header and lists carry the 24 px gutter.
- **Tabs.** `marketTabs` is a `MarketNextTabWidget` (a `QTabWidget` that installs
  `MarketNextTabBar`; neither has `Q_OBJECT`, so class names, `findChild` and
  type selectors are unchanged). The bar paints underline tabs on the pane's
  top hairline and turns the "(n)" or "(n of m)" the tab text already carries
  into a count pill, so `tabText()` still reads "All (20)". Tabs, indices,
  current widgets and the five scroll areas are the same.
- **Category pills.** `marketCategoryButtons` stays a child of the page but
  leaves the layout: `MarketNextTabsOverlay` (an event filter on the tab widget,
  its bar and its stack) places it in the 52 px gap the pane's `padding-top`
  opens under the tabs, so the pills sit between the tabs and the grid as in the
  mockup. A new "All" pill (`#marketNextCategoryAllButton`) clicks the checked
  category off (the same handler as before) and follows the others' state. The
  selected pill is filled with the foreground colour, as in the mockup. The same
  overlay centres `marketSearchEmpty` under the pills when a search finds
  nothing.
- **Grid or list.** A two-button toggle after the sort combo
  (`#marketNextViewToggle`: `marketViewGridButton` with the `apps` icon,
  `marketViewListButton` with `list-unordered`) switches the All, Installed and
  Not installed lists between a grid and a list. The choice is saved in
  `market/view` (`grid` or `list`), set on each list host as `marketNextList`,
  and the cards are rebuilt for it. The three hosts use `MarketGridLayout`, a
  `QVBoxLayout` subclass that lays its widget items out in columns (330 px
  minimum, up to four, 16 px gaps, rows as tall as their tallest card), or one
  card per row with 8 px gaps in list mode; any other widget (the package
  settings page) takes a full row. Because it is still the same `QVBoxLayout`
  member, `addWidget`, `takeAt`, `itemAt`, `clearPackageCards`,
  `applyMarketSearch` and the tests' layout walks are unchanged. Packs and
  Guides stay lists of 12 px hairline cards.
- **Cards.** `arrangeMarketNextCard` re-lays the widgets the Classic builder
  made (with the same object names, properties and connections). In the grid:
  the preview on top in a rounded frame (painted cover-fit by the card's event
  filter; Next decodes it once at 640×360 instead of 240×135), name and muted
  version, price on the right, "@author · Category · N downloads" (the full
  Classic line is its tooltip), a description clamped to two lines, and a
  footer with the Install button (`marketPackageAction-*`, accent) or a
  "✓ Installed" badge (plus "Active" for the applied theme), an amber "Not
  reproducible" when the build is not, and "N permissions" on the right. In
  the list (`nextMarketList` on the card): a 112×63 preview, then name, meta
  and a one-line description, then the warning, price, Install or badges and
  "N permissions" in one row.
- **Details pane.** Clicking a card (or "N permissions",
  `#marketNextDetailsToggle`) opens its details in `#marketNextDetailsPane`, a
  380 px pane right of the tabs with the package name
  (`#marketNextPaneTitle`), a close button (`#marketNextPaneClose`) and a
  scroll area. Clicking the open card again, or the close button, closes it;
  the open card has an accent border (`nextMarketSelected`). The pane shows
  the card's own `#marketNextDetails` widget, moved in while it is open and
  handed back when another card opens: the preview (`#marketNextPaneShot`),
  version and price, the meta line and the full description, type, licence
  and install status (`#marketNextFacts`), ratings and usage
  (`marketPackageStats`), the trust checks as icon items (`#marketNextTrust`),
  the permissions as a hairline list (`#marketNextPermissions`, amber for
  anything not low risk, "Needs no permissions." when empty), the native-code
  warning (`marketNativeNotice`), bundled packages (`marketNestedPackages`),
  Show on (`marketPackagePlaces`, its check boxes stacked in a bordered list)
  with `marketPackageNotShownHere`, and Settings, Install from a saved copy and
  Install or Remove. Install in the pane (`#marketNextPaneAction`) clicks the
  card's own `marketPackageAction-*`, so it takes the same path. The pane
  remembers the package, not the card: when an install or a count report
  rebuilds the cards, or the tab changes, `restore()` reattaches the new card
  of the same package from the current list (or All, when the package moved
  out of it) and keeps the scroll position; it hides on Packs and Guides and
  closes when the package is gone or the package settings page opens. A card
  owns its details widget and deletes it with itself. The Classic
  `marketTrust` and permissions labels are hidden duplicates of the new trust
  row and list.
- **Rich text.** The package settings title and guide "from" captions use
  `Theme::nextOrHex(FgMuted, "#8b949e")`, so Classic keeps its grey.
- **Left out on purpose.** The "Extensions" pill (the Market has no such
  category filter; the four existing categories and All are shown), the
  illustrated placeholder thumbnails (each card shows the package's own
  preview). The details used to expand inside the card so every control
  stayed a descendant of it; the window tests run Classic, and the search
  filter walks only the list layouts, so the pane does not affect them.
- **Live switch.** The header, tabs and grid are built once, so a live design
  switch shows them after the page is rebuilt; cards rebuilt in the other
  design still work in either layout (a Next card in a Classic list stacks
  full width and, with no pane on the page, opens its details below itself; a
  Classic card in the grid takes a full row).

## Must not break

- **Object names and class names stay.** Window tests use `findChild`, Market
  overlays address object names, and Python tests read `desktop/src`.
- **Widget paths stay.** Rail shortcuts (`ui/railShortcuts`) and remembered page
  buttons (`ui/pageButtons`) address buttons by object name or class and index.
  Move widgets only between layouts of the same parent; a restructure needs a
  migration and a test.
- **Settings keys stay**: `app/theme`, `ui/railOrder`, `ui/railFolders`,
  `ui/buttonPlacement`, `ui/hideRailCaptions`, `ui/poppedOutPages/*`.
- **Rail behaviour stays**: reorder, folders, drag out to a window, shortcuts,
  movable buttons, badges.
- **The app opens on the shell.** The welcome screen is an overlay.
- **Item views keep a painted selection band**, and the base rule sets no font
  family ([README.md](README.md#traps)).
- **Perf budgets**: startup 1000 ms, tab switch 150 ms, repository open 250 ms
  (`test_perf_budgets.cpp`).
- **Qt 6.4 floor**; newer API behind `QT_VERSION_CHECK`.
- **No logic changes in a styling change** (no edits to `connect`, network
  calls, settings writes or Git calls).
- **Remote strings are plain text** (`Qt::PlainText`) wherever server errors,
  branch names or file names land in a label.
- The embedded browser hands only http(s) addresses to the system.

Tests that pin a purely cosmetic value are updated with the value; behavioural
assertions are never loosened.

## Tools

- `forkmesh-ui-gallery` (`desktop/tests/ui_gallery.cpp`) renders every stock
  widget and component per design and scheme, linking only tokens, kit and
  resources.
- `forkmesh-window-tests --ui-snapshots-only` builds the real window offscreen
  against temporary directories and writes PNGs per design and scheme under
  `ui-snapshots/<design>-<scheme>/` (or `FORKMESH_UI_SNAPSHOT_DIR`). It also
  checks the design switch, that no `@role` placeholder remains, and sidebar
  widths. A review aid, not a pixel gate.
- `FORKMESH_DESIGN=next ./desktop/run.sh` tries Next without changing settings.
  On macOS start it from a terminal, not from an agent's command tool: Bluetooth
  privacy checks are attributed to the parent process.
- `clang++ -fsyntax-only` with the flags from
  `build/CMakeFiles/forkmesh-window-tests.dir/flags.make` checks one
  `MainWindow*.cpp` in seconds.

A change is done when `forkmesh` and `forkmesh-window-tests` build, the focused
window suites, critical tests and Python tests reading `desktop/` match the
baseline, snapshots were reviewed in both designs and schemes, and Classic is
unchanged.

## Remaining work

- Settings rows onto `SettingsRow`; hard-coded colours in files also built into
  test programs; some charts and badges; rich-text colours.
- Widgets styled once at build time do not follow a live design switch until
  rebuilt; the prompt composer's Next sizes and its icon-less send, add and
  send-to buttons are one such case. Its baked mode, effort and model icons do
  follow a dark/light switch (`refreshComposerIcons` from
  `refreshThemedIcons`).
- Overlay coverage for remaining object names and Market package pages; text
  tabs with overflow for the repository tab row.
- Motion, empty and loading states, an interface font-size option, focus rings.
- Then make Next the default (`Theme::designFromSettings()` falls back to
  `classic`), keep Classic one release, delete the Classic sheets and re-key
  Market themes.

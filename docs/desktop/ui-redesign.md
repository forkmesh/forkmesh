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
| `bg` | `#09090b` | `#ffffff` | Window, page |
| `bgSubtle` | `#0c0c0e` | `#fafafa` | Sidebar, chrome bar |
| `surface` | `#111113` | `#ffffff` | Cards, inputs, menus |
| `surfaceRaised` | `#18181b` | `#f4f4f5` | Hover row, default button |
| `surfaceActive` | `#232326` | `#e8e8eb` | Pressed, active sidebar row |
| `border` | `#232326` | `#e4e4e7` | Hairlines, row dividers |
| `borderStrong` | `#303036` | `#d4d4d8` | Inputs, buttons, floating layers |
| `fg` | `#f4f4f5` | `#18181b` | Primary text |
| `fgSecondary` | `#d4d4d8` | `#3f3f46` | Secondary text |
| `fgMuted` | `#a1a1aa` | `#6b6b74` | Descriptions, icons |
| `fgSubtle` | `#71717a` | `#8a8a94` | Placeholder, disabled |
| `accent` | `#1f6feb` | `#0969da` | Primary button, selection, focus, toggle on |
| `accentHover`, `accentPressed` | `#388bfd`, `#1158c7` | `#0550ae`, `#0a5cc7` | Button states |
| `accentFg` | `#58a6ff` | `#0969da` | Links, active tab underline |
| `accentSubtle` | `#12233b` | `#e6f0fd` | Current-item background |
| `onAccent` | `#ffffff` | `#ffffff` | Text on accent, danger and success fills |
| `success` | `#3fb950` | `#1a7f37` | Status only |
| `warning` | `#d29922` | `#9a6700` | Status only |
| `danger` | `#f85149` | `#cf222e` | Status, destructive actions |

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
  page sideways. Screen changes are handled one event-loop turn later.
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
secondary, and "send to: #channel" is a ghost. Object names, layout positions, settings
keys, tooltips and Classic are unchanged.

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

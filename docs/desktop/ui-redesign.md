# Desktop: the interface redesign

Part of [the desktop docs](README.md). Status on 2026-10-04: phases 0 to 4
are built on the branch `refactor/desktop-app-ui`, with two passes over
phase 5, most of phase 6 and a round of fixes from the owner's first review,
all behind the opt-in "New (preview)" interface. Classic is still the default and nothing is on `main` yet.
"Progress" at the bottom says exactly what exists and how it was checked.

The desktop app works but looks dated: a GitHub-dark clone palette with 456
distinct colours, icon-over-caption buttons everywhere, eighteen font sizes,
heavy weights (650 to 850), coloured kicker labels and a few gradient
buttons. The goal is a calm, professional interface in the style of the
reference screenshots (T3 Code): near-black neutral surfaces, hairline
borders, one accent colour, grouped settings rows, a labelled sidebar, a real
welcome and sign-in screen. No existing feature may stop working.

## Decisions

Taken with the owner on 2026-10-03.

| Question | Decision |
| --- | --- |
| Technology | Qt Widgets design system. No QML in this project. |
| Sign-in | A welcome screen inside the window. Guest use stays allowed. Account creation still finishes in the browser. |
| Navigation | The left rail gains an expanded, labelled mode and can collapse back to icons. |
| Rollout | The new look ships as a second design next to the current one ("Classic"), becomes the default when the shell and core pages are done, and Classic stays one release as a fallback. |

### Why not QML

The idea that started this was "keep the C++ core, rewrite one screen in QML,
bind it, migrate gradually". That advice assumes a backend that is separate
from the interface. This app does not have one, and its release toolchain does
not carry Qt Quick. Measured on 2026-10-03:

- `desktop/src` is about 450,000 lines; `MainWindow` is one class in 101
  files and 255,000 lines. Logic reads widget values directly:
  `catalogApiUrl()` returns `m_serverUrlEdit->text()` and has 118 call sites
  in 39 files. There is no model layer to expose to QML, so every migrated
  page would need its logic extracted first.
- QML does not read QSS. It cannot restyle a single existing widget; it only
  helps for screens built new, and needs its own copy of the theme. Two
  theming paths is how an interface ends up inconsistent again.
- The Linux release is a static Qt 6.8.2 built from `qtbase` and `qtsvg` only
  (`tools/package/linux-baseline/stage-qt.sh`). Quick needs `qtdeclarative`,
  `qtshadertools` and static QML plugin import.
- The Windows cross-build has no QML host tools, and `deploy_qt_by_hand`
  (`tools/package/windows.sh`) copies a fixed module list with no `qml/` tree.
- Market packages hand the desktop `QWidget` pages
  (`NativePackageWidget : public QWidget`, 11 bundled contributions), so the
  shell has to stay widget-based.
- Qt 6.4 is the floor; on 6.4 a `QQuickWidget` renders through OpenGL or the
  software scene graph only and must agree with the `agent-watch` package's
  `QOpenGLWidget` when both sit in one window.
- The `forkmesh` target sets `AUTOMOC OFF`; QML type registration needs the
  moc JSON that mode does not produce.

Nothing in the references needs a scene graph. Flat surfaces, hairlines,
rounded cards, toggles, segmented selects, a sidebar and a command palette are
all ordinary QSS plus a dozen painted widgets. The one thing Widgets cannot do
cheaply is a live blur behind a modal; a dim scrim replaces it.

Revisit QML only if a screen needs something Widgets cannot draw, and only
after the three release pipelines carry Qt Quick. That is a separate project.

## What is being replaced

How the look is produced today, so the work replaces it centrally.

- **Two hand-maintained style sheets.** `Theme::kStyleSheet` (dark,
  `Theme.h:169-2386`) and `Theme::kLightStyleSheet` (`:2388-4574`) mirror each
  other, about 517 `#objectName` selectors each. Every rule is written twice.
- **One funnel.** `MainWindow::applyTheme()` (`MainWindow.cpp:1315`) sets the
  palette, the sheet and re-tints icons. `resolveThemeIsDark()`
  (`MainWindowInternal2.cpp:311`) reads `app/theme` (`system`, `dark`,
  `light`, or a Market theme id).
- **A hex-to-hex remap** for Market themes (`Theme::setActivePalette`,
  `styleSheetForPalette`): plain string replacement over the sheet. One
  package uses it, `marketplace/packages/winamp`, with 111 keys that are the
  current hex values and a 461-line overlay that addresses object names.
- **A long tail outside the sheets.** 2,163 hex literals in 84 files, mostly
  `dark ? "#x" : "#y"` ternaries and inline rich text; 198 `setStyleSheet`
  calls (72 in `ClaudeTranscriptView.cpp`); 70 classes that override
  `paintEvent`, about 38 of them charts.
- **No shared components.** There is no Button, Card, Badge or Dialog class.
  Cards are re-declared per page (`#setupCard`, `#homeCard`,
  `#ledgerWalletCard`, `controlCard()`, `rewardCard()`), each repeating the
  same background, border and a radius between 10 and 18 px. `ToggleSwitch`
  exists but is used twice; 138 `QCheckBox` do the real toggling.
- **No bundled interface font.** The family is the first installed of Segoe
  UI, Inter, Cantarell, SF Pro Text, Roboto, Noto Sans, Ubuntu
  (`main.cpp:845-877`), so the app looks different on every machine.
- **No sign-in screen.** The app opens as a guest. Sign-in is a chain of
  modal dialogs (`MainWindowAccounts.cpp:1332`, `MainWindowSetup.cpp:5205`),
  errors are `QMessageBox::warning`, network calls block in nested event
  loops with no spinner, and "Create an account" opens `<relay>/signup` in the
  browser. `buildSetupPage()` still builds a page at stack index 0 that is
  never shown; its hidden fields are the source of truth for the relay URL.

## Design language

Values are proposals until the component gallery is approved (review gate 1).

**Principles**

1. Neutral first. Surfaces, borders and text are greys. Colour means
   something: blue for selection, focus and links; green, amber and red only
   for status.
2. One accent. No purple, cyan or pink as decoration, no gradient fills.
3. Hairlines, not shadows. Panels sit on 1 px borders. Only floating layers
   (menus, popovers, dialogs, the command palette) get a stronger border.
4. Fewer sizes. Seven font sizes, three weights, five radii, a 4 px grid.
5. Quiet counts. A count is a neutral pill; it turns blue only for unread.
6. Text labels over icon-with-caption. Icons accompany labels, they do not
   replace them, except in the collapsed rail.

**Colour roles**

A neutral zinc scale, close to the website homepage's tokens
(`www/public/index.html`), so the desktop and the site read as one product.
Accent and status hues keep the values the code already uses most. The table
is `kRoles` in `desktop/src/DesignTokens.cpp`.

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

`success`, `warning` and `danger` each have a `Subtle` background for badges
and notices.

**Type.** Base stays 14 px so existing layouts and pinned heights hold; the
Next sheet caps every `font-weight` above 600 at 600. The new interface uses
Geist for the interface and Geist Mono for logs and the agent transcript,
the faces the website already uses. Six static TTFs (regular, medium and
semibold of each, about 830 KB, SIL Open Font License, from the
`vercel/geist-font` 1.7.2 release) sit in `desktop/resources/fonts` with
their licence and are registered on first use by
`Theme::ensureBundledFonts()`. Classic keeps picking an installed family in
`main.cpp`. The base `QWidget` rule must still set no family (the documented
trap: a family there beats every monospace `setFont`).

**Shape and space.** Radii 4 (chips, key hints), 6 (buttons, inputs), 8
(rows, menus), 12 (cards, dialogs), 16 (composer, welcome card), full
(pills, toggles). Spacing 4, 8, 12, 16, 24, 32. Control heights 28, 32, 36.

**Buttons.** Primary is the blue accent with white text, as in the reference
screenshots; it also keeps the white icons that `Theme::iconColorForButton`
already gives primary buttons. Secondary is a raised neutral; ghost has no
border and gains a quiet fill on hover; danger and success are coloured text
that fill on hover.

**What each reference contributes**

| Reference | Taken from it | Lands in |
| --- | --- | --- |
| Main thread view | Three quiet panes, list rows, breadcrumb bar with pill buttons, rounded composer with inline selectors | Shell, sidebar, agent prompt composer |
| Pairing card | Centred card: title, one-line explanation, labelled input, primary and secondary button, footnote box | Welcome and sign-in |
| Settings, General | Section list on the left; content in a centred column; heading outside the card; rows of title, description and a trailing control divided by hairlines | Settings |
| Settings, Appearance | Colour-scheme tiles with a miniature preview; slider with a value chip | Settings, Appearance |
| Settings, Source Control | Provider row: icon, name, version in monospace, outline status badge, toggle | Hosts, mirrors, integrations, Market list |
| Command palette | Centred search with grouped results, trailing badges, key hints in the footer, dimmed backdrop | Command palette |

## Architecture of the change

**A second design, not a second theme.** The setting `app/design`
(`classic` or `next`) is independent of `app/theme`, so system, dark and
light work in both. `MainWindow::applyTheme()` resolves it with
`Theme::designFromSettings()`; the environment variable `FORKMESH_DESIGN`
overrides the setting, which is how to try the new look without touching a
profile. A Market theme (Winamp) forces `classic` while it is active, because
its palette keys are Classic's hex values.

The window's view menu (the button left of minimize) has an **Interface**
section, Classic or New (preview), beside the view modes, so the new look is
one click away instead of under Settings, Appearance (2026-10-06). It drives
the Settings `appDesignCombo` when that page has been built, so the two
controls never disagree, and otherwise writes `app/design` and reapplies the
theme itself. `FORKMESH_DESIGN` still wins over both.

**Next is Classic recoloured, plus overlays.** Writing a second 2,200-line
sheet from nothing would have left most of the 517 one-off object names
unstyled for weeks. Instead `Theme::nextStyleSheet(dark)`
(`desktop/src/DesignTokens.cpp`) builds the sheet in three steps:

1. take the Classic sheet for the scheme;
2. replace its neutral greys with the new neutrals in one pass
   (`kNextDarkPalette`, 23 entries; `kNextLightPalette`, 19) and cap
   `font-weight` at 600;
3. append the overlay files, in order, with `@role` placeholders replaced
   from the role table.

A rule in an overlay with the same selector as a Classic rule wins by order,
so an area is refined by restating only what should change. Every page is
therefore complete in Next from the first day, and one overlay rule serves
dark and light.

```
desktop/resources/theme/next/00-base.qss      stock widgets, shared components
desktop/resources/theme/next/10-shell.qss     chrome, rail, tabs, welcome
desktop/resources/theme/next/11-overlays.qss  command palette
desktop/resources/theme/next/20-settings.qss
desktop/resources/theme/next/30-repo.qss
desktop/resources/theme/next/40-issues.qss
desktop/resources/theme/next/45-notes.qss
desktop/resources/theme/next/46-calendar.qss
desktop/resources/theme/next/50-pulls.qss
desktop/resources/theme/next/60-agents.qss
desktop/resources/theme/next/70-chat.qss
desktop/resources/theme/next/80-ops.qss       actions, releases, control, network
desktop/resources/theme/next/90-data.qss      stats, health, logs, ledger
```

Every `*.qss` in that folder is loaded, in name order, so a new area sheet
needs no registration. Two are exceptions: `95-compact.qss` is appended
only in the compact density, and `96-edge-indicators.qss` only when the
edge indicators are on (see 2026-10-07 below). `desktop/CMakeLists.txt` writes the resource
list (`generated/theme.qrc` in the build folder) from a glob of the sheets,
the icons beside them and the Geist fonts; it is part of
`forkmesh-resources`, so all three targets that compile the app see them.
Editing a sheet rebuilds only that resource.

**The same palette reaches code.** `applyTheme()` installs the Next palette
with the existing `Theme::setActivePalette`, so everything already wired to
`Theme::remapColor` follows: tinted icons, the transcript palette, the diff
renderer, `QPalette`. To extend that to the long tail, neutral
`QColor("#rrggbb")` constructions and the argument of every widget-level
`setStyleSheet` are wrapped in `Theme::remapColor` and `Theme::remapSheet`
(156 and 184 sites in 53 files, done by script). Both return their input
untouched when no palette is active, so Classic is unchanged, and the
original text is still in the source for the tests that read it. Colours
inside rich-text HTML strings are not covered.

**Roles for new code** (`desktop/src/DesignTokens.h`). `Theme::color(role)`
and `Theme::hex(role)` resolve against the active design and scheme; in
Classic a role returns today's hex and then passes through `remapColor`, so
Market themes still recolour it. New painting code uses roles, never
literals.

**Roles for existing colours.** A neutral grey can be remapped blindly; a
purple or a green cannot, because whether it is decoration, a selection mark
or a status depends on the site. Those sites are wrapped one by one:
`Theme::nextOr(role, classicColor)` and `Theme::nextOrHex(role, classicHex)`
return the role in Next and the classic expression, untouched, in Classic.
The classic expression stays in the source verbatim, so Classic is the same
by construction (Market themes included) and the tests that read the source
still find their text. The rule applied: decoration becomes a neutral role,
a selection or current mark becomes `SurfaceActive`, `AccentSubtle` or
`AccentFg`, a purple state (merged, closed, scheduled) becomes accent, and
colours that encode success, warning or failure, diff and syntax colours,
chart series, avatars and brand colours are left alone. These helpers live
in `DesignTokens.cpp`, so they may only be used in files that are compiled
into the app alone; `StatsDashboard.cpp`, `ComponentHealthBoard.cpp`,
`CodeMapView.cpp`, `MemoryUsageChart.cpp`, `FleetVisualizer.cpp`,
`AddCoreNodeDialog.cpp`, `PingSettingsPanel.cpp`, `CoveFileEditor.cpp` and
`SettingsSectionsView.cpp` are also built into small test programs that do
not link them.

**What a sheet cannot finish.** Three things about stock widgets are not
reachable from QSS, so one application event filter does them in Next and
undoes them in Classic: `kit::installStockPolish()` in `UiKit.cpp`, installed
by `NextAccess::install` and re-run over every polished widget by
`kit::syncStockWidgets()` when the theme changes.

- *Floating surfaces.* A `QMenu`, a combo box's popup and the app's own
  popups (`agentMetaPopup`, `reactionPicker`, `slashActionsPopup`,
  `globalSearchPopup`, `mentionPopup`) are top-level windows. A sheet can
  round the border it draws inside one, but the window stays an opaque
  rectangle, which shows as a square frame around a rounded one. In Next
  these windows get `WA_TranslucentBackground`, `FramelessWindowHint` and
  `NoDropShadowWindowHint`. That has to happen before the native window
  exists (the Wayland note in [README](README.md) is the same rule), so a
  menu is handled on its `Polish` event, which `QMenu` raises before it
  creates the window, and a combo popup on the `ChildAdded` its combo box
  receives, because `showPopup()` creates the native window before the
  popup is polished. A window that already exists natively is left alone.
  Each handled surface carries the property `floatingSurface`: `true` when
  it is translucent, `false` when it could not be made so, and the sheet
  squares the corners for `false`, so the double frame cannot come back. A
  menu with its own style sheet or `autoFillBackground` (the issue card
  menu) is skipped.
- *Combo box rows.* A combo popup draws rows with a private delegate that
  ignores `::item` rules. `kit::prepareComboPopup` swaps in a
  `QStyledItemDelegate` the first time a combo is pressed or receives a key,
  which is before `showPopup()` measures the list; code that opens a popup
  itself calls it first. `combobox-popup: 0` in the sheet keeps the list
  under the field. The delegate is not swapped back on a switch to Classic;
  a restart does that.
- *Tables.* Horizontal headers that still have the default centred
  alignment become left aligned, like the cells under them. A table that
  sets the property `dataTable` also loses its grid, row numbers and
  alternating rows, gets 38 px rows and fits its interactive columns to
  their contents; the sheet then draws one hairline under each row. It is
  opt-in because a table whose delegate paints the whole cell would lose
  its row lines. The Marketing package's four tables use it.

**Settings sections.** `ColumnFlowWidget::addSection` marks each section
with the property `sectionCard`; in Next it gets a hairline border, 12 px
corners and inner margins, re-applied when the design changes. Section
frames in `SettingsSectionsView` are `Fixed` vertically, which pins them to
a height computed at their preferred width; text that wraps at the real
width was squeezed (the clipped identity-key warning in Classic was this).
`refreshGeometry()` gives each frame its height-for-width at the real width,
in both interfaces since Settings became one column (2026-10-07,
[README.md](README.md#recent-changes)). The cards are also how the section
list finds a section's parts: each card that sits directly in a section body,
named by its `sectionLabel` heading, and each `sectionLabel` outside any card.

**Hooks instead of header edits.** `MainWindow.h` and `MainWindowInternal.h`
are in the precompiled header; here a change to either costs about 50
minutes of rebuilding (14 for the app, 35 for the window tests). They were
edited once:

- `MainWindow` declares `friend struct forkmesh::ui::NextAccess`. The struct
  is defined in `MainWindowNextAccess.h`, which is not in the precompiled
  header, and its static functions are implemented across
  `MainWindowNextShell.cpp`, `MainWindowWelcome.cpp` and
  `MainWindowCommandPalette.cpp`. New shell behaviour is added there without
  touching `MainWindow.h`.
- `ActivityRailButton`, `VerticalIconButton`, `RailFolderPopout` and
  `ToggleSwitch` call hooks declared in `NextShellHooks.h` at the top of
  their `paintEvent` and size functions, passing their private state as a
  plain struct. The hooks live in `MainWindowNextShell.cpp`; they return
  false in Classic, and the original code runs.

**Components** (`desktop/src/UiKit.{h,cpp}`, namespace `forkmesh::ui::kit`),
none with `Q_OBJECT`: `Toggle` (a `QCheckBox` subclass, so `isChecked()`,
`toggled` and `findChild<QCheckBox *>` keep working), `Card`, `SettingsRow`,
`Badge`, `KeyHint`, `InlineNotice`, `EmptyState`, `SegmentedControl` (an
exclusive `QButtonGroup` underneath), `Spinner`, and the helpers
`groupHeading`, `pageTitle`, `pageLede`, `labeledField`, `tintedIcon`.
`SearchField` and `ValueSlider` from the first plan are not built yet.

**Seeing the result without the real app.** Running `forkmesh` from an agent
shell would use the real node's data. Two tools avoid that:

- `forkmesh-ui-gallery` (`desktop/tests/ui_gallery.cpp`) links only the
  tokens, the kit and the resources, and writes one PNG of every stock widget
  and component per design and scheme. It builds in seconds.
- `forkmesh-window-tests --ui-snapshots-only` builds the real window
  offscreen against temporary directories, walks thirteen sections and a
  fixture repository in all four design and scheme combinations, and writes
  PNGs under `ui-snapshots/<design>-<scheme>/` (or `FORKMESH_UI_SNAPSHOT_DIR`).
  It also asserts the design switch, that no `@role` placeholder is left in
  the sheet, and the sidebar widths. It is a review aid, not a pixel gate.

**Launching the real app on macOS.** Start it from a terminal, with
`FORKMESH_DESIGN=next ./desktop/run.sh` to see the new interface without
changing saved settings. Do not start it from an agent's command tool: the
local mesh begins a Bluetooth scan a few minutes after launch
(`LocalMeshNode::startBleScan`), macOS attributes that request to the
process the app descends from, and a parent that declares no Bluetooth use
gets the app aborted with a privacy violation. That happened on 2026-10-03
after about six minutes; the same build started from the owner's terminal
kept running.

**Checking a change without the full build.** `clang++ -fsyntax-only` with
the flags from `build/CMakeFiles/forkmesh-window-tests.dir/flags.make`
parses one `MainWindow*.cpp` in about three seconds. That is how parallel
work was verified before the single combined build.

## Must not break

These are the couplings found in the survey. Every change is checked against
this list.

- **Object names and class names stay.** `test_window_resize.cpp` has 673
  `findChild` calls; the Winamp overlay addresses object names; 18 files in
  `app/tests` read `desktop/src` as text.
- **Widget paths stay.** Rail shortcuts (`ui/railShortcuts`) and remembered
  page buttons (`ui/pageButtons`) address a button by object name, or by class
  name and index among siblings. Re-parenting a page's buttons orphans a
  user's saved shortcut. Where a restructure is unavoidable, it ships with a
  migration and a test.
- **Settings keys stay**: `app/theme`, `ui/railOrder`, `ui/railFolders`,
  `ui/buttonPlacement`, `ui/hideRailCaptions`, `ui/poppedOutPages/*`.
- **Rail behaviour stays**: drag to reorder, folders, drag out to a window,
  shortcuts, movable buttons, badges, the teammate rail.
- **The app opens on the shell.** Tests assert it
  (`test_window_resize.cpp:16247`). The welcome screen is an overlay on the
  shell, never a gate.
- **Item views keep a painted selection band**, and the base rule sets no
  font family (both in `docs/desktop/README.md`, Traps).
- **Perf budgets**: startup 1000 ms, tab switch 150 ms, repository open
  250 ms (`test_perf_budgets.cpp`).
- **Qt 6.4 floor.** Newer API is guarded with `QT_VERSION_CHECK`.
- **Market packages.** The 16 packages with `content/desktop` keep rendering;
  Winamp keeps working on Classic.
- **No logic changes in a styling change.** No edits to `connect`, network
  calls, settings writes or Git calls. A diff that touches them is a different
  change and goes in its own pull request.

Tests that pin a purely cosmetic value (a hex string, a pixel height) are
updated in the same change that moves the value. Behavioural assertions are
never loosened to make a restyle pass.

## Phases

What each phase set out to do, and where it stands. "Progress" has the
detail.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Baseline builds, test results, snapshot tooling | Done |
| 1 | Roles, the Next sheet, components, the design switch | Done, except bundled fonts |
| 2 | Shell: chrome, rail, tabs, notices, menus | Done for colour and shape; tabs keep their icon-over-caption layout |
| 3 | Expanded, labelled sidebar | Done; folders keep their pop-out row |
| 4 | Welcome and sign-in | Done in the new interface |
| 5 | Pages | Two passes: area sheets and mechanical routing of greys, then the decorative and selection colours in C++ site by site. Settings has section cards. Other page layouts are unchanged |
| 6 | Command palette, polish | Command palette, colour-scheme tiles, bundled fonts, contrast check done. Motion, empty states and a font-size option not started |
| 7 | Make Next the default, retire Classic | Not started |

### Departures from the first plan, and why

- **Next is built on the Classic sheet** instead of a new sheet from a
  template (see Architecture). It made every page usable in Next at once.
- **The primary button is blue.** The references use it, and it keeps the
  white icons primary buttons already have.
- **Repository tabs keep icon over caption.** Fourteen tabs and five actions
  do not fit one row as text tabs at common window widths; that needs an
  overflow menu, which is page work.
- **Rail folders are not inline groups.** In the expanded sidebar a folder is
  a row that opens the same pop-out as before. Turning folders into
  collapsible groups means rewriting `RailFolderWidget`'s drawer, drag and
  pin logic, which is exactly the behaviour that must not break.
- **The expanded sidebar, the welcome card, the sign-in panel and the command
  palette exist only in Next.** Classic behaves as before, dialogs included.
- **No Ctrl+B for the sidebar.** A window shortcut would take `^B` away from
  the built-in terminal. The toggle is the top-bar button, the Settings
  check box and the command palette.
- **The command palette is Ctrl+Shift+P**, because Ctrl+K already opens
  global search. In Next that chord no longer reaches the built-in terminal.
  On macOS Cmd+P opens it too (2026-10-13). Linux and Windows keep only
  Ctrl+Shift+P: a window-level Ctrl+P would take `^P` (previous command) away
  from the built-in terminal, the same reason there is no Ctrl+B.
- **The command palette reaches Settings** (2026-10-13). A "Settings" group
  lists every Settings section; a section's sub-headings
  ("General › Appearance") are listed only once something is typed, so the
  empty palette stays short. Choosing one opens Settings and scrolls there
  (`SettingsSectionsView::setCurrentIndex` / `setCurrentSubSection`, the
  sub-heading looked up by title at that moment). Any query also gets a last
  row, "Search settings for “…”", which opens Settings with that text in its
  search box, so a single option is reachable without the palette keeping
  its own index of every control. Settings is a deferred section: when it has
  not been built yet the palette opens at once and builds it 30 ms later,
  then refreshes its rows (`showCommandPalette(window, true)`), so the
  one-time build cost never delays the palette appearing. Sub-headings of a
  lazy Settings section appear only after that section has been built.
- **`verifyTotpLogin` was not split.** Its message boxes are routed through
  `NextAccess::accountNotice`, which shows them inside the sign-in card when
  it is open and otherwise returns false so the original dialog appears. The
  texts are unchanged.
- **No UI kit page behind the debug bar.** The standalone gallery program
  replaced it.

### Phase 5 and later: what is left

Per area, in the order the owner cares about:

- move Settings rows onto `SettingsRow` (title, description, control at the
  right); the sections are cards now but their rows are still the old ones;
- hard-coded colours in the files that are also built into small test
  programs (listed under "Roles for existing colours"), which cannot use
  the helpers until those targets link `DesignTokens.cpp`;
- the charts drawn in `MainWindowInternal.h`, and `PullBadgeWidget`;
- a second blue: `Theme::kRunning` and "checks running" stay the classic
  blue as a state, next to the accent;
- widgets styled once when built (the source-control Generate button, the
  active-file outlines, the tree selection) do not follow a live switch of
  interface until the page is rebuilt;
- object names no overlay covers yet (`MainWindowShortcuts.cpp` pills, plain
  frames such as `leaderboardCard`, `statsTile`);
- the Market packages' own desktop pages;
- sentence-case headings where the source text is upper case;
- text tabs with overflow for the repository tab row;
- Classic rules for the `UiKit` object names, needed before any page uses a
  component in both designs;
- motion for floating layers, empty and loading states, an interface font
  size option, keyboard focus rings.

Phase 7 then makes Next the default, keeps Classic one release, deletes the
two Classic sheets and re-keys Winamp.

## How the work is run

**What a build costs here.** On the owner's Mac (10 cores, 16 GB) a first
full build of `forkmesh-window-tests` took 35 minutes; a rebuild after an
edit to `MainWindowInternal.h` or `MainWindow.h` recompiles 121 files and
took about 7 minutes for the window tests and 4 for the app. A `.cpp` or
`.qss` edit rebuilds in under a minute.

**Roles.** One lead session owns the shared files (tokens, `UiKit`,
`Theme.h`, the two precompiled headers, CMake) and the build. Agents work in
parallel in the same tree on disjoint files, do not build, and report back;
the lead syntax-checks each file in seconds, then runs one combined build.
On 2026-10-03 six agents ran that way: welcome and sign-in, command palette,
Settings, and three writing area sheets.

**Every change is done when**

1. `forkmesh` and `forkmesh-window-tests` build (the small unit targets do
   not compile `MainWindow*.cpp`);
2. the focused window suites, the critical tests and the Python tests that
   read `desktop/` give the same results as before the change;
3. the snapshot suite ran and its images were looked at, in both schemes and
   both designs;
4. Classic is unchanged.

**Landing.** Small independent pull requests, each cut from current
`origin/main` and checked with `git apply --check` against it. The shared
foundation has to be on `main` first; after that the area sheets and page
work are independent. Next is off by default until Phase 7, so an unfinished
design on `main` changes nothing for users, which matters because every push
deploys.

**Left alone.** Uncommitted work already in the tree when this started (two
lines in `desktop/CMakeLists.txt`, `DesktopDisplayReadiness.cpp`, one line in
`forkmesh_autogen.cpp`) belongs to its author. It is a macOS build fix the
tree needs to compile here, and it is not part of these changes.

## Risks

| Risk | Handling |
| --- | --- |
| Edits to `MainWindowInternal.h` or `MainWindow.h` recompile everything and collide between agents | Edited once; hooks and the `NextAccess` friend make later edits unnecessary |
| The app looks half-migrated for weeks | Next is opt-in until Phase 7 |
| A restyle silently changes behaviour in a 12,000-line file | Styling-only rule; behaviour changes are gated on `Theme::isNext()`; tests compared against a baseline |
| Saved rail shortcuts break when a page's widget tree changes | Names and paths are frozen; migration plus test where not |
| Colours inside rich-text strings stay Classic in Next | Listed under "what is left"; they are mostly greys close to the new ones |
| A larger sheet slows startup | Only Next pays for it; not yet measured against the 1000 ms budget |
| Fonts render differently on Windows, macOS and Linux | Open until fonts are bundled |
| Frameless chrome differs per platform (Wayland translucency, macOS) | Chrome work leaves the `WindowFade` and opacity paths untouched |
| Only macOS has been built and run | Linux and Windows builds, and Qt 6.4, are unverified; `tools/qt64-build-check.sh` is Linux only |

## Open points

1. **When to make Next the default.** It is one line
   (`Theme::designFromSettings()` falls back to `classic`). Left alone
   because the window tests run with the default and assume Classic, and
   because only macOS has been built.
2. **Default sidebar groups for a new install.** None; the rail keeps its
   flat order until the user makes folders.
3. **States that now share the accent.** Closed and merged (purple in
   Classic), running, scheduled and "current" are all blue in Next; their
   icons still differ. A neutral pill for closed is the alternative.
4. **Judgement calls from the colour pass** that are easy to reverse: the
   vote count and the confetti are accent rather than green and
   multi-colour; the saved-search bell stays amber when on; ours/theirs
   conflict buttons and search-match highlights keep their colours.

## Progress

### 2026-10-03

Committed on `refactor/desktop-app-ui` as a series of small commits, one per
seam (palette routing, roles, components, design switch, shell, Settings,
sign-in, command palette, area sheets, snapshot suite). No pull request yet.

**New files**

| File | What it is |
| --- | --- |
| `desktop/src/DesignTokens.{h,cpp}` | Roles, the Next palette maps, the Next sheet builder, the design setting |
| `desktop/src/UiKit.{h,cpp}` | Shared components |
| `desktop/src/NextShellHooks.h` | Paint and size hooks called from the shared header |
| `desktop/src/MainWindowNextAccess.h` | The `NextAccess` friend and `railColumnWidth()` |
| `desktop/src/MainWindowNextShell.cpp` | Rail and tab painting, expanded sidebar, the toggle |
| `desktop/src/MainWindowWelcome.cpp` | Welcome card and sign-in panel |
| `desktop/src/MainWindowCommandPalette.cpp` | Command palette |
| `desktop/resources/theme.qrc`, `desktop/resources/theme/next/*` | Eleven overlay sheets (about 700 lines) and two chevron icons |
| `desktop/tests/ui_gallery.cpp` | The component gallery program |

**Changed files, by kind**

- Shared headers, once: `MainWindow.h` (friend), `MainWindowInternal.h`
  (includes, hooks, 85 neutral colours through `remapColor`, green hover and
  selection tints through `nextHoverFill` / `nextAccentTint`), `Theme.h`
  (`remapSheet`, an early return in `remapColor`).
- Theme switch: `MainWindow.cpp` (`applyTheme`, `NextAccess::install`).
- Rail: `MainWindowInternal2.cpp` (`railColumnWidth`),
  `MainWindowNavigation.cpp` (toggle in the chrome row, sign-in entry),
  `MainWindowChat.cpp`.
- Sign-in: `MainWindowSetup.cpp` (`verifyTotpLogin`, `runLoginFlow`),
  `MainWindowAccounts.cpp` (`loginToUserAccount`),
  `MainWindowOnboarding.cpp` (`maybeShowFirstRunWelcome`).
- Settings: `MainWindowSettings.cpp`. Appearance gains "Interface" (Classic,
  New preview; key `app/design`) and "Expanded sidebar with labels" (key
  `ui/railExpanded`).
- 53 files touched by the mechanical `remapColor` / `remapSheet` wrap.
- Build and tests: `CMakeLists.txt`, `tests/test_window_resize.cpp`
  (`--ui-snapshots-only`).

**New settings keys:** `app/design`, `ui/railExpanded`,
`onboarding/welcomeSignInSeenV1`. **New environment variables:**
`FORKMESH_DESIGN`, `FORKMESH_UI_SNAPSHOT_DIR`.

**How to try it.** Settings, Appearance, Interface: New (preview). Or start
with `FORKMESH_DESIGN=next`. The sidebar toggle is the first button after the
project picker; the command palette is Ctrl+Shift+P (Cmd+Shift+P or Cmd+P on macOS);
the top bar's "Log in / Sign up" opens the sign-in card.

**Checked, on macOS with Qt 6.11.1 only**

- `forkmesh` and `forkmesh-window-tests` build and link.
- `--ui-snapshots-only`: every check passes in the four design and scheme
  combinations; 70 images written and looked at. Its only failing line is the
  Linux-only systemd check that fails in every suite on macOS.
- Seven focused window suites (`--onboarding-only`, `--view-modes-only`,
  `--mirror-fleet-only`, `--off-thread-teardown-only`,
  `--agent-efficiency-only`, `--stats-only`, `--diff-stream-only`): the same
  PASS and FAIL lines as before the change. They are not clean on macOS to
  begin with (1, 1, 1, 1, 1, 5 and 9 failures).
- Critical tests (`tools/run_critical_tests.py qt`): 8 of 15 pass. The 7
  failures are the four window suites above plus
  `forkmesh-agent-tool-efficiency-tests`, `forkmesh-vm-runtime-tests` and
  `forkmesh-control-tests`, none of which compiles a file this work changed.
- Python tests that read `desktop/` (133 files, installer tests left out):
  2,327 pass and 54 fail, and every one of the 54 also fails on an untouched
  copy of `HEAD`.
- `desktop/tests/validate_*.py`: the same four fail before and after.
- The full window suite (1,706 checks): 1,611 pass and 95 fail, and an
  untouched copy of `HEAD` built beside it gives the same 1,611 and 95. Two
  checks differ between runs in both builds ("opening the Worktrees tab
  focuses the table" and "a rendered branch range is kept for the next
  visit"); they are flaky, not caused by this work.

**Not checked**

- The real application was never launched; everything visual comes from the
  offscreen test window and the gallery.
- A real sign-in against a relay. The test drives the card, the inline error
  and Escape, not a login.
- Drag-reorder, folder pop-out and drag-out with the sidebar expanded.
- Linux, Windows, Qt 6.4, and the release packaging.
- Startup time in Next.

### 2026-10-04, second round

**Fonts.** Geist and Geist Mono, as described under "Type".

**Colour pass in C++.** 171 calls to `Theme::nextOr` / `nextOrHex` in 30
files, worked from a prepared list of the 588 lines that carried a
non-neutral colour; the rest are states, diffs, charts, avatars and brands
and were left. By area: issues, tasks, calendar and discussions (38); pulls,
branches, source control and the diff chrome (35); repository, releases and
files (17); agents, teammates and the transcript (46, plus the transcript's
bubble and panel fills); actions, nodes and network (6); chat, shortcuts and
the membership dialog (28). `agentRunningFireballColor()` and
four icon tints (`quickAddSendIcon`, `chatAddChannelButton`,
`quickAddGenieButton`, `chatIntegrationButton`) follow the design through
`Theme::iconColorOverride`.

**Settings.** Sections are cards and get their wrapped height; Appearance
has three colour-scheme tiles (System, Light, Dark) above the Theme
dropdown, which stays the source of truth and still lists Market themes;
the Interface dropdown follows a change made elsewhere.

**Snapshot suite.** Now also walks Hosts, Control, Health, Memory, Nodes,
Pings, Settings General, the Agents overview and every repository tab, and
resets the window to 1440 by 900 before each capture: 150 images. Pages
whose own minimum is wider (Health at about 2,200 px, Notes and Users at
about 1,700) stay wider in both interfaces. Notes was brought down to about
800 px in the third round.

**Measured**

- Contrast, text on its background, dark and light: primary 18.1 and 17.7,
  muted 7.8 and 5.3, accent text 7.9 and 5.2, white on the accent fill 4.6
  and 5.2, status text on its subtle background 5.2 to 6.4 and 4.5 to 4.7.
  The light muted and placeholder greys were darkened to reach these.
  Placeholder text is 4.1 and 3.4, below the 4.5 used for body text.
- Start-up cost of the new interface: building the sheet 1 to 5 ms,
  registering fonts 5 to 7 ms, applying the sheet 5 to 9 ms against 3 to 5
  ms for Classic. The main window still shows about 0.6 s after launch.

**Checked, on macOS with Qt 6.11.1 only**

- `forkmesh`, `forkmesh-window-tests` and `forkmesh-ui-gallery` build.
- Snapshot suite: 105 checks pass in the four combinations; the one failing
  line is the Linux-only systemd check.
- Seven focused window suites and the full window suite (1,611 pass, 95
  fail): the same as the untouched copy of `HEAD`, apart from the two flaky
  checks named above.
- Critical tests: the same 8 of 15. `forkmesh-settings-sections-tests`,
  `forkmesh-startup-splash-tests`, `forkmesh-cove-files-tests`,
  `forkmesh-code-map-view-tests` and `forkmesh-stats-dashboard-tests` pass.
- Python tests that read `desktop/`: 2,327 pass and 54 fail, the same 54
  that fail on the untouched copy.
- The app was launched once with `FORKMESH_DESIGN=next` on the owner's
  machine. What the owner saw is not recorded here.

**Not checked:** a real sign-in, drag and folder behaviour with the sidebar
expanded, Linux, Windows, Qt 6.4, release packaging (the font files and
`theme.qrc` are ordinary Qt resources, so they need no packaging change).

### 2026-10-04, third round: the owner's review

The owner ran the new interface and named seven things. The Code page and
the Git page were liked as they are.

| Named | Cause | Change |
| --- | --- | --- |
| Dropdowns look basic and have a square border around a rounded one | Menus and popups are opaque rectangular windows; combo rows are drawn by a delegate that ignores the sheet | Translucent floating surfaces and styled combo rows, under "What a sheet cannot finish". Menus gain a tick for checked items, a chevron for submenus and 12 px item padding |
| The selected row in Repos looks wrong | The selection was the full accent, and the cell widgets (action buttons, activity charts) painted the page colour over it, inset by the 8 px item padding, which left a blue frame around a black box | The selection is `surfaceRaised`, the row delegate keeps its normal text colours in Next, and `repoRowActions` and the two chart cells are transparent |
| Row actions are not aligned | The Actions column was sized for three 96 px slots, but the cell widget is laid out inside the item padding, 16 px narrower, so the third slot was clipped; on a local repository Delete also sat alone on a second line | `kRepoActionsColumnWidth` includes the padding (both interfaces, it was clipped in Classic too). In Next a local row is one line, Switch, Settings, Delete, and the ghost buttons have a hairline border so the three read as one group |
| Tables could look nicer (Marketing) | Centred headers over left-aligned cells, row numbers, a full grid, 100 px columns that cut dates off | Left-aligned headers everywhere; the `dataTable` treatment on the Marketing tables |
| Notes | A status sentence in the title row set the page minimum to about 1,700 px and pushed buttons off screen; list rows were three lines of plain text with a horizontal scrollbar | See below |
| Calendar | Plain text in a grid, nothing marking today | See below |
| The README under the file list | Rendered straight onto the page | `readmeCard`: a hairline card with a header carrying the file name; the file list above it has the same frame |

**Repository overview.** `m_readmeView` now sits inside `QFrame#readmeCard`
with a `readmeCardHeader` (book icon and `readmeCardTitle`). The card is
framed only in Next and only when there is a README (property `framed`); in
Classic the header stays hidden and the frame has no rule, so the page is as
before. The view's height adds its own contents margins, since the sheet
gives it 20 px of padding inside the card. `#overviewList` gets a hairline
frame and 4 px of padding; its height already counted `frameWidth()`. Two
details: a re-polish alone does not make a `QFrame` re-read its frame
widths, so the card's children are also sent `StyleChange` when `framed`
flips, otherwise the padding is ignored; and in Next the scroll body's
margins are 0 and 16 px instead of 6 and 0, so the file list and the README
card share both edges with the commit bar above them.

**Notes** (`MainWindowNotes.cpp`, `45-notes.qss`). The status line moved to
its own wrapped row under the title in both interfaces, which is what lets
the page shrink: its minimum is now about 800 px. In Next the list rows are
painted by `NoteRowDelegate`: the title, then a storage pill, a visibility
pill (accent for a published note) and a short date, with the sharing line
when there is room; it falls back to the stock delegate in Classic. Item
text, tooltips and the id and storage roles are unchanged; the delegate
reads its parts from `Qt::UserRole + 2` to `+ 6`. The title field has the
object name `noteTitleEdit`. No container was named, because
`pathSegment()` turns a uniquely named ancestor into part of a saved
shortcut's path.

**Calendar** (`MainWindowCalendar.cpp`, `46-calendar.qss`). Three delegates
paint the views in Next and defer to the stock delegate in Classic.
`CalendarMonthDelegate` draws the day number, today's in a filled accent
circle, days outside the month in `fgSubtle` on `bgSubtle`, each event as a
chip with a muted time, "+N more", a hover fill and a 1 px accent outline
for the selected cell; it also draws the grid and its 12 px rounded frame,
so the table's own grid is off in Next (`QTableView` paints grid lines after
the delegates and they would overshoot the corners). `CalendarWeekDelegate`
draws one chip and a muted "+N" per hour cell. `CalendarAgendaDelegate`
draws Day and Agenda rows with a time column and shows the date once per
day, so rows are 36 or 50 px instead of a fixed 66. The cell and row text
that `renderCalendar` and `agendaItem` produce is unchanged; the delegates
parse it, plus `Qt::UserRole + 4` (outside the month) and `+ 5` (agenda
parts). `CalendarEventHook`, an event filter on the page, applies the
Next-only state on `Polish`, `StyleChange` and `Show` and restores the
Classic values: "Find events" moves above the calendar list, the list loses
its 240 px maximum width, header highlighting is off, and month rows re-fit
when the viewport is resized. New object names: `calendarMonthTable`,
`calendarWeekTable`, `calendarDayList`, `calendarAgendaList`. The Today and
step buttons keep the name `ghostButton` and are addressed by the property
`calendarNav`; the two sidebar labels by `calendarSidebar`.

**Snapshot suite.** The fixture repository now has a README, a licence and
two folders, and the suite waits for the overview to load before the
picture. It also seeds three local notes and a week of calendar activity,
adds `repos`, `calendar-week` and `calendar-agenda`, and selects the first
row of each repository table. The gallery writes an open combo popup and a
menu beside each page picture, drawn over the page colour so translucent
corners show as they will on screen.

**Checked, on macOS with Qt 6.11.1 only**

- `forkmesh-window-tests` and `forkmesh-ui-gallery` build.
- Gallery, four combinations: the combo popup and the menu are rounded with
  no frame behind them in Next and are as before in Classic.
- Snapshot suite, offscreen in a temporary home: 105 checks pass. Looked at
  in Next dark and light: Repos, the repository overview, Notes, and
  Calendar in month, week and agenda. Classic Repos and the Classic
  repository overview look as they did, apart from the Delete button no
  longer being clipped.
- The Python tests that read the changed sources (33 files, installer tests
  left out): 459 pass and 14 fail, and the same 14 fail on an untouched copy
  of `HEAD`.

**Not checked**

- The real application. A grab of a translucent popup shows what Qt draws,
  not what the window system composites, so the rounded menus and
  dropdowns still need a look on screen; so does the Private / local tab of
  Repos, which the snapshot suite does not open.
- The Marketing page itself (the package is not installed in the test
  window); its tables are covered by the gallery's `dataTable` sample only.
- The full window suite, the critical tests, Linux, Windows and Qt 6.4.

### 2026-10-05, fourth round: the window, and the owner's second review

The owner ran the app from the third round and named four more things: the
alert cards, the Docs page, the selected channel in Chat, and that the
window could not be resized or dragged and was hard to manage on a small
screen. The last one is not about the new interface; its fixes apply to both.

**Why the window could not be resized.** The main window is frameless
(`Qt::FramelessWindowHint`), and its edges were wired to
`QWindow::startSystemResize`. Qt's macOS plugin implements `startSystemMove`
but not `startSystemResize` (the platform default returns false; checked in
`libqcocoa.dylib` of Qt 6.11.1), so on macOS an edge press was swallowed and
did nothing. `WindowFrameControl` (`desktop/src/WindowFrameControl.h`, a
`QObject` child of the window, created in its constructor) is an
application event filter installed after the window's own, so it sees the
press first: it asks for a system resize and, where the system declines,
resizes the window itself from the mouse moves, between the window's minimum
and maximum size. It does the same for a press on `windowChromeBar` if a
system move is refused. `MainWindow::handleFramelessResizeEvent` is left in
place and still sets the edge cursors.

**Why the window could not shrink.** Its minimum width was 1,129 px on every
page, set by the status bar: the commit, Git identity and folder labels had
explicit minimum widths of 340, 240 and 220 px. They are now 96, 48 and 72;
their preferred and maximum widths are unchanged, so a wide window shows
what it showed. Some pages then asked for more (Hosts 1,400, Health 1,575,
Notes about 1,700 before the third round), and a page's minimum became the
window's, which pushed the window past the edge of a laptop screen where its
right side and bottom corner could not be reached.

- The section stack is now a `SectionViewport`
  (`desktop/src/SectionViewport.h`, a `CurrentPageStack`): it reports at
  most 520 px as its minimum width, and when the current page needs more
  than the stack has, it lays the page out at the width it needs and shows a
  horizontal scroll bar (`sectionScrollBar`) along the bottom. A sideways
  wheel or trackpad gesture, or Shift with the wheel, scrolls it. The pages
  stay direct children of the stack, so saved shortcut paths are unchanged;
  a `QScrollArea` around the stack would have added two ancestors to every
  path.
- `WindowFrameControl::fitToScreen` moves and sizes the window to lie inside
  the screen's available area when it is shown, when it changes screen and
  when the screen's available area changes.
- A screen change is handled one event-loop turn later, for the screen the
  window is on by then, and only for a screen still in
  `QGuiApplication::screens()`. On 2026-10-06 the desktop segfaulted inside
  `QObject::connect` when a Wayland output went away: `screenChanged` fires
  from `QWindowSystemInterface::handleScreenRemoved` while the old screen is
  being torn down, and connecting to the screen it handed over crashed.

**Density.** The owner suggested smaller buttons and text on small screens.
The new interface has a compact density: `95-compact.qss` (tighter padding
for buttons, inputs, menu items, header sections and tabs) is appended and
every `font-size` of 13 px or more in the built sheet drops by one.
`ui/density` holds `auto`, `comfortable` or `compact` (Settings, Appearance,
Density, next to Interface); `FORKMESH_DENSITY` overrides it. Automatic is
compact when the primary screen's available width is under 1,500 px. Sizes
set in C++ (the 42 px chrome bar, the rail, icon sizes) do not change, and
Classic has no compact density. The offscreen test platform reports an
800 px screen, so the gallery and the snapshot suite are compact unless
`FORKMESH_DENSITY=comfortable` is set.

A whole-interface zoom was considered and left out: Qt's scale factor has to
be set before `QApplication` exists, which is before the settings layout is
resolved, and a fractional factor on a Retina screen draws hairlines at 1.7
device pixels. `QT_SCALE_FACTOR=0.85` in the environment still works for
anyone who wants to try it.

**Chat sidebar** (`MainWindowIssues.cpp`, `71-chat-sidebar.qss`). The
selected channel was two boxes: the row's fill stopped before the gear, and
the gear cell had its own. `ChatRoomRowDelegate`, on the channel tree and
the direct-message list, paints the row in Next and defers to the stock
delegate in Classic: one 6 px pill across the whole row, gear and indent
included (`surfaceActive` selected, `surfaceRaised` hover), 32 px rows, the
gear drawn only on the hovered or selected row and without a box, an unread
row as a 6 px accent dot at the right with the name in semibold, the padlock
as the `lock` icon, and group headers as quiet 12 px section rows with a
chevron. It reads the existing item text and its prefixes, so text, roles,
drag and drop and the context menu are untouched. The direct-message list
has the object name `chatDmList`. "New channel", "Connect Discord" and "New
message" are quiet bordered buttons; their icons are tinted `fgSecondary`
through `Theme::iconColorOverride`, where `chatAddChannelButton` moved from
the accent to the neutral group. The Discord button keeps its connected and
failed colours.

**Alert cards** (`MainWindowSettings.cpp`, `MainWindowNavigation.cpp`,
`12-alerts.qss`). The cards looked wrong for three reasons: the whole
message was bold in the tone colour (red for an error, underlined when the
card links somewhere); three inner widgets with no rule of their own
(`topMessageBody`, `topMessagePromptImages`, `topMessageQueueActions`)
painted the page colour over the card; and the actions were a mix of
blue-tinted and plain buttons. In Next the card is `surface` with a 12 px
radius and a 2 px left edge in the tone colour, everything inside it is
transparent, and the message is 13 px medium in the primary text colour.
The tone is carried by that edge and by the type badge, which is a subtle
tone background with no outline. Every action is the same 24 px ghost
button; the gear and close buttons are 24 px squares (property
`toastIcon`). `renderTopMessage` and `renderTopMessageQueue` set
`topMessageTone` on the card as well as the badge, use slightly larger
margins and give the body an inset for the border that
`topMessageBubbleRect` does not count; `renderTopMessageCountdown` re-tints
the action icons, because it is the one function that runs after both
`showTopMessage` and `showPromptBubble` have reset them. All of it is gated
on the design, and Classic renders are byte-identical in a mock. The prompt
status line is muted rather than green. Two window-test assertions hold only
in Classic: action buttons at most 18 px tall (they are 24 in Next), and an
alert exactly as tall as its lines (about 7 px more in Next).

Superseded by issue 1867 (2026-10-09): the user asked for no toasts, so the
alert cards were removed. Messages are inline notices in the page layout
(`kit::InlineNotice` rows in `#pageNoticeArea`); `12-alerts.qss` now holds
only their rules, and the Pings row's prompt button is `pingPromptAction`.

One text fix applies to both interfaces: a queued alert that needs
dismissing read "queued Â· needs dismissing", because the middle dot was
written as UTF-8 escapes inside `QStringLiteral`, which takes them as two
UTF-16 units. It is built with `QString::fromUtf8` now.

**Docs page** (`marketplace/packages/docs/content/desktop/DocsFeature.{h,cpp}`,
`47-docs.qss`). The text the owner saw running off the right was not a wrap
problem; `docsView` wraps to its viewport. The two pager buttons carried a
page title each and could not shrink, which held the page at about 1,070 px
before the rail, so the window was wider than the screen and nothing capped
the line length. Fenced code and tables did overflow on their own: at
1,200 px, 57 of the 155 documents showed a horizontal scrollbar.

- Both interfaces: the pager buttons have a 64 px minimum and elide their
  titles to half the footer width, so the page minimum is about 550 px.
- Next: the reader pane (property `docsPane="reader"`) is a card; the page
  content is capped at 260 + 900 + 200 px and anchored left, so a line of
  text is at most about 830 px. The document gets a 28 px margin, semibold
  headings, code runs that wrap inside a padded hairline box (each run in a
  `QTextFrame`, because a block background cannot carry padding), Geist
  Mono for code, and tables as hairline rows. No document shows a
  horizontal scrollbar between 640 and 1,440 px.
- Next: a delegate paints the navigation tree (groups semibold with the
  count at the right, 30 px rows, rounded hover and selection); the
  breadcrumb is `Library / Area / path` with the reading time at the right;
  backlinks wrap; the outline hides when the reader would fall under 560 px.
- `DocsFeature` overrides `event` to re-apply this on `Polish` and
  `StyleChange`, so a live switch of interface works.

**Checked, on macOS with Qt 6.11.1 only**

- `forkmesh-window-tests` and `forkmesh-ui-gallery` build in a separate
  build directory with tests on; `desktop/run.sh` configures `desktop/build`
  with `FORKMESH_BUILD_TESTS=OFF`, which removes the test targets there.
- Snapshot suite, offscreen in a temporary home, `FORKMESH_DENSITY`
  comfortable and compact: 153 checks pass, including the new ones (the
  window shrinks to 1024 by 700 on five pages, a wide page scrolls
  sideways, an edge drag resizes, the two geometry helpers). The window's
  minimum width is 653 px in Next and 551 px in Classic. The one failing
  line is the Linux-only systemd check. The "fits a 1280x720 laptop window"
  checks added in parallel work on this branch now pass on every page.
- Looked at: the alert cards in the real window; Notes, Chat, Calendar and
  Health at 1024 by 700; Repos and Calendar in compact.
- Alert cards, chat sidebar and Docs page were each rendered in a scratch
  program that compiles the real source against mocks (Docs against the
  real `www/docs`): Next dark and light, and Classic byte-identical to
  `HEAD`.
- The Python tests that read the changed sources (55 files, installer tests
  left out): 732 pass and 24 fail, the same 24 as on an untouched copy of
  `HEAD`.

**Not checked**

- Resizing and dragging with a real pointer on macOS. The test drives the
  event filter with synthetic events on the offscreen platform, where the
  system resize is refused exactly as it is on macOS.
- The Docs page inside the real window (the snapshot suite does not open
  Market package pages), a live Classic to Next switch while an alert is
  showing, automatic density on a real small screen.
- The full window suite, the critical tests, Linux, Windows and Qt 6.4.

### 2026-10-06, fifth round: Git, Files and the code view

The owner asked for the Git page, the file explorer and its tabs, and the
code view to look more modern.

**Shared.** Closable tabs had Fusion's grey close box. `00-base.qss` now
gives every `QTabBar::close-button` a 14 px cross (`@closeIcon`, and
`@closeHoverIcon` on `surfaceActive` when hovered; four SVGs beside the
chevrons) and styles the scroll buttons a tab bar shows when its tabs
overflow. The gallery has a closable, scrolling tab bar to look at.
`topMessageAction` buttons outside an alert card (the "send to prompt"
button on each Pings row) lose their blue tint; the rule is at the top of
`12-alerts.qss`, under the card-scoped ones in specificity.

**Files page, its tabs and the code view** (`MainWindowFiles.cpp`,
`32-files.qss`).

- Tabs, both interfaces: with several files open the names were crushed to
  "C…d" while the close boxes kept their size. `FilesTabBar` (a `QTabBar`
  subclass without `Q_OBJECT`, so the class name the shortcut paths record
  is unchanged) never lets a tab shrink below its natural width, so the bar
  scrolls instead; middle elision starts only past 320 px. In Next the
  active tab is `fg` on `surface` with a 2 px accent line.
- Explorer, Next: `FilesTreeDelegate` (on the filesystem, repository, Cove
  and trace trees) paints 28 px rows with one rounded pill for hover and
  selection, folder names in `fg` and file names in `fgSecondary`, its own
  chevrons, and the folder icon tinted `fgMuted` instead of yellow. The
  tree sits in a 12 px card.
- Header, Next: the title row keeps Show hidden and Size; one toolbar row
  holds Repo, Coves, Up, Home, Choose, the path field (mono, 12 px),
  Refresh and Open; the commit row keeps its order. Widgets move between
  layouts of the same parent only, so all 39 saved-shortcut paths are the
  same as on `HEAD`. Open and Save carry the property `filesQuiet` and are
  drawn as neutral buttons, which leaves Push as the one primary; their
  icons are re-tinted on a zero-length timer because `refreshThemedIcons()`
  runs after the style change.
- Editor, Next: the editor and the markdown preview share one 12 px card;
  code is Geist Mono 13 px with 17 px lines and 4-space tab stops; the
  preview has a 24 px margin; the breadcrumb is rich text with the file
  name in `fg`; the editor's scrollbars are hidden while the preview covers
  it (they showed through as a second scrollbar).
- The page minimum went from 1,013 px to 781 px (1,137 to 874 with a
  markdown tab open). Classic renders match `HEAD` except for the tab strip.
- Keyboard focus order still follows creation order, so in Next it no
  longer matches the toolbar's visual order exactly.

**Git page** (`MainWindowSourceControl.cpp`, `MainWindowShared.cpp`,
`52-git.qss`).

- Both interfaces: two buttons read "Commit_push" and "Stage_push". Their
  texts were `"Commit & push"` and `"Stage & push"`, and a single `&` is
  Qt's mnemonic marker. They are `&&` now. Nothing compares the texts, and
  saved shortcuts strip every `&` before matching.
- Buttons, Next: Commit is the one primary. The two push buttons keep the
  object name `primaryButton`, which their saved-shortcut path depends on,
  and carry the property `scmSecondary`, which the sheet draws as a neutral
  button.
- Changes list, Next: `ScmFileRow` paints one rounded pill for hover and
  selection and the tree's own selection fill is transparent; that fill in
  the indent area was the stray grey box, and the green outline is gone
  with it. `ScmTreeRowDelegate` gives 28 px rows. The status letter is
  coloured by meaning: `M` warning, `A` and `?` success, `D` and `U`
  (unmerged) danger, `R` and `C` accent.
- History, Next: `ScmCommitRowDelegate` subclasses `CommitSummaryDelegate`
  and replaces it on `m_commitsTable`. Refs are subtle chips, the author is
  muted, rows have the same pill. Graph line colours are data and are left.
- Diff, Next: `ScmDiffCard`, a mouse-transparent overlay named
  `scmDiffCard`, draws a 12 px hairline card in place of the 2 px accent
  outline; the active-file outline is 1 px `borderStrong`. The colours of
  the hunk band, line numbers, file header, stat cells and pills are chosen
  in `MainWindowShared.cpp` (`diffStyleSheet`, `diffFileHeaderHtml` and
  their helpers) through `Theme::nextOrHex`, with the classic expressions
  kept verbatim. Those functions are shared, so pull-request, commit and
  branch-range diffs get the same colours. Added and removed line colours
  are unchanged.
- `ScmDesignHook`, an event filter on the panel and on the diff view,
  applies and restores this on a live switch. It also restyles the
  branch-range outline that `MainWindowBranches.cpp` creates.
- Left as they are: "Sync Changes" is still an accent button, so a dirty
  tree with outgoing commits shows two blue buttons; the edit and delete
  pills in the diff header are rich text and cannot change colour on hover.

**Code editor** (`MainWindowInternal.h`, the one edit to a shared header
this round). `CodePreviewEditor` drew its current-line band in a bare
`#161b22`, which reads blue against the new neutrals; it is `surfaceRaised`
in Next, and the gutter is filled with `bg`. Both go through
`Theme::nextOr`, so Classic is the same expression as before.

**Checked, on macOS with Qt 6.11.1 only**

- `forkmesh`, and in a separate build directory `forkmesh-window-tests` and
  `forkmesh-ui-gallery`, build.
- Snapshot suite, offscreen in a temporary home: 153 checks pass; the one
  failing line is the Linux-only systemd check. The suite now also changes
  two files in the fixture repository, opens the Git page from the rail and
  takes a `git` picture with a diff showing. Looked at in Next dark: the Git
  page and the Files page in the real window.
- The Git page and the Files page were each rendered in a scratch program
  that compiles the real source against mocks: Next dark and light at two
  widths and in compact, and Classic identical to `HEAD` apart from the two
  button labels and the tab strip.
- The Python tests that read the changed sources (65 files, installer tests
  left out): 809 pass and 29 fail, the same 29 as on an untouched copy of
  `HEAD`.

**Not checked**

- The real application with a real pointer: tab scrolling with many files
  open, hover on tree and history rows, the all-repositories view of the
  Git page, pull-request and branch-range diffs with the new colours.
- The full window suite, the critical tests, Linux, Windows and Qt 6.4.


### 2026-10-06, merge review: security fixes

Pulls #263–#280 were reviewed before they were merged into `main`. None of them
changes a network call, an auth check or a Git path. The review made these
changes on top of them:

- **Plain text where a remote string can land.** The sign-in panel's notice
  (`kit::InlineNotice`) shows the server's own error text, so it is
  `Qt::PlainText`; a hostile instance could otherwise format it as HTML. The
  same goes for the README card title and, in the Agents workspace, the empty
  body and the branch label (Git allows `<` and `>` in branch names).
- **The sign-in panel is modal.** Classic signs in through `QDialog::exec()`,
  which blocks the window. The panel runs its own event loop over the central
  widget, so while it waits it disables the widgets beside it and the menu
  bar, and the command palette does not open over it. The account check in
  `switchToUserAccount` still runs afterwards either way.
- **Delete keeps its gap.** On the Repos list, Delete stays on the second row,
  away from Settings, in both interfaces (`addNetworkRepoAction`).
- **The browser pane stays on the web.** "Open in browser" hands only http(s)
  addresses to the system, because the address follows whatever the embedded
  page navigated to. With WebEngine built in, links with other schemes
  (`mailto:`, app links) are refused instead of launching another program.

### 2026-10-07: alerts under the top bar, edge indicators optional

The owner asked for two things: the alert cards should not cover the top
bar, as they did not in Classic, and the coloured left edge on cards should
be an option, off everywhere by default.

**Alerts under the conversation header.** A deep alert stack may climb to
the top of the content area (`topMessageStackCeiling`). In Classic the
Agents page's action row sits there too, but Classic cards are short
enough that a full stack happened to stop just under it. Next cards are
taller (a 24 px ghost button row), so the same 16 pings climbed over the
Next conversation header (`workspaceHeader`: title, status, Continue, Show
diff, panel toggles). The stack now has a list of top bars,
`MainWindow::m_topMessageCeilingBars`; `topMessageContentTop` lowers the
ceiling to half a margin under any visible bar in it that lies across the
composer's column and above it. `NextAccess::installWorkspace` adds the
conversation header, so the header is respected only while it is on
screen, and Classic geometry is unchanged. The workspace snapshot suite
(`--workspace-snapshots-only`) flashes 16 pings over the running session
in each design, saves `workspace-alerts.png`, and checks in Next that the
stack's top is below the header.

Superseded by issue 1867 (2026-10-09): with the cards gone there is no stack
to climb, the ceiling bars and `topMessage*` geometry were removed, and the
snapshot suite now checks that a burst of 16 notices sits in the layout above
the conversation header.

**Edge indicators.** Settings, Appearance, "Colored left edge on alerts and
cards" (key `ui/edgeIndicators`, read by `Theme::edgeIndicators()`, off by
default) controls every coloured left stripe that marks a card's tone:

- the 2 px tone edge on the page's inline notices (`#pageNoticeArea
  #inlineNotice`, the alert cards' edge until issue 1867), in
  `96-edge-indicators.qss`, which `nextStyleSheet` appends only when the
  setting is on; without it a notice keeps its 1 px outline on all four sides
  and the tone is carried by its icon and colour;
- the 3 px accent edge on an agent's question card
  (`ClaudeTranscriptView::addAskUserQuestion`) and on the command hover card
  (`CommandHoverCard`);
- the 3 px accent edge on pull-request and issue conversation cards
  (`MainWindow::addConversationCard`).

Toggling it reapplies the theme, so the alert sheet changes at once; the
C++ cards pick it up the next time they are built. Left as they are: the
rail's selected-item bar (it marks the current page, not a card), the issue
row's progress rail, and the Marketing package's channel-coloured slots and
quote rule, which belong to the package and are its only channel cue.

### 2026-10-07: Chat laid out like Slack, and unread you can reach

The owner saw "2 unread messages" over an empty direct message and could
not find them, and asked for the channel rail and the conversation to look
like a Slack screenshot they supplied.

**Why the messages could not be found.** The banner added every
conversation's counter together but never said where the messages were,
and its only button marked everything read. The saved state behind the
report had one unread message each in `#mesh` and `#random` while
`#office/@cenzo` was open. A conversation can hold unread while its sidebar
row is missing: `#mesh` is listed only while the local mesh runs, and a
relay channel only while the relay advertises it. That left a count with no
row to click.

- The banner now names the places, newest first ("2 unread messages in
  #mesh and #random", then "and N more" past three), and has an **Open**
  button beside **Mark all read**. Open goes to the conversation whose last
  message is newest (`MainWindow::openNextUnreadConversation`). It is
  hidden when the only unread conversation is the one already open.
- `MainWindow::unlistedChatConversations` returns each unread conversation,
  and the open one, that has messages but no row. `refreshChannelList` adds
  the channels among them to the tree, and `refreshDmList` adds the direct
  messages to the list, so every counted message has a row to click. The
  rows go away again once the conversation is read and left.
- An **All unread** row at the top of the sidebar shows the total as a red
  badge and does the same thing as Open.

**The Slack look (Next design).** In both schemes the sidebar is
aubergine (`#3f0e40`, `71-chat-sidebar.qss`). At its top is a workspace
header: "ForkMesh", a green presence dot with the chat name, and a white
round compose button that starts a direct message. Below it come All
unread, a divider, then **Channels** and **Direct messages** headers. Each
header has a chevron that collapses its list and a `+` (new channel / new
message); the bordered "New channel" and "New message" buttons are gone.
`ChatRoomRowDelegate` paints rows edge to edge at 28 px in 15 px type:
`#` before a channel, a padlock before a private one, a presence dot
before a direct message (filled green when the peer is in the online
roster, `MainWindow::chatPeerOnline`, a ring when not). Text is
`#d1c4d2`, hover `#350d36`, and the selected row is `#1164a3` with white
text. An unread row is white and bold with a red count pill (`#cd2553`).
The count comes from `ChannelGroupTree::kUnreadCountRole`, presence from
`kPresenceRole`. The item text and its prefixes are unchanged, so Classic
painting, drag and drop and the tests that read rows still work. The
sidebar icons are tinted through `Theme::iconColorOverride`, so a design or
scheme switch repaints them.

The conversation header now has the title (18 px, heavy) above a meta line:
the member count and the topic, which used to be joined to the title with
a dash and now has its own label, `channelTopic`. On the right are Invite,
the seal button, the transport icon and channel settings, which is now an
`info` icon. Message rows in Next have rounded-square avatars, the name in
the foreground colour rather than the per-sender colour (the avatar keeps
that colour), 12 px times, 15 px text and a light hover. The composer is
one bordered box: the input on top, a toolbar under it with bold, italic,
strike, code, link and bulleted list on the left and mention, emoji,
attach and a green send button on the right. The toolbar buttons take no
focus, so a selection in the input stays selected for wrapping. They call
the existing `wrapComposerSelection` / `insertLinkIntoComposer`, so the
keyboard shortcuts are unchanged. Classic keeps its colours; its sheets
have plain rules for the new widgets.

Message rows bake their colours in when they are built, so a scheme switch
left the open transcript in the old scheme's colours. With sender names now
in the foreground colour, that turned them white on white after a switch
from dark to light. `MainWindow::applyTheme` now queues a transcript
rebuild (`scheduleConversationRebuild`) for every window that has one.

`forkmesh-window-tests --chat-unread-only` checks the unread behaviour: rows
for unlisted rooms, the banner text, Open, the badge, and an unread direct
message. With `FORKMESH_CHAT_SHOT=<prefix>` it also saves
`<prefix>-light.png` and `<prefix>-dark.png` of the Next chat page.

### 2026-10-12: Chat laid out like Discord

The owner asked for the chat on desktop and web to follow a Discord
screenshot: a channel rail, a one-line channel bar, a member list on the
right and a pill composer. This replaces the Slack look of 2026-10-07 in
the Next design; Classic keeps its colours and gets plain rules for the new
widgets in `Theme.h`.

**Palette.** `DesignTokens.cpp` has a chat tone table (`kChatTones`) with a
dark and a light value per tone: rail, rail footer, pane, row hover,
selected row, message hover, text, strong text, muted, faint, divider,
edge, composer, composer icons, brand, "new" red, online, away. The values
are Discord's. QSS reads them as `@chatRail` and so on (`expandTokens`
falls back to `chatHex` for names that start with `chat`), and painted code
reads them with `Theme::chatColor`. `iconColorOverride` tints the rail,
header and composer icons from the same table, so a scheme switch repaints
them.

**Rail.** A 48 px header holds "ForkMesh" with a chevron (it starts a
direct message, as the compose button beside it does). Section headers read
**Text Channels** and **Direct Messages** in small semi-bold muted type with
the chevron after the name. `ChatRoomRowDelegate` paints 32 px rows with a
rounded 4 px background inset 8 px from the rail edges, a 20 px muted `#`,
bright text on hover, selection and unread, and the red count pill. At the
bottom a user panel shows your avatar with a presence dot (green when the
chat backend runs, grey when not) and your name over "Online" or "Offline"
(`refreshChatWorkspaceUser`; `withPresenceDot` paints the dot with a ring in
the background colour).

**Channel bar.** One 48 px row: a muted `#` (or `@` for a direct message),
the name, a thin divider and the topic, then Invite, the seal button, the
transport icon, channel settings and the members button. The title label
now holds the name without its prefix; `updateChatConversationHeading`
moves the prefix to `channelHash`.

**Member list.** The members pop-up menu is now a 240 px panel docked to
the right of the transcript, under the channel bar (`buildChatMembersPanel`).
The members button is a toggle and the choice is saved in
`chat/memberListVisible`. `syncChatMembersPanel` hides the panel while the
chat body is narrower than 720 px, so a narrow window keeps a usable
transcript. `refreshChatMembers` still builds the cards only while the list
is shown. Members are sorted online first, under **Online — N** and
**Offline — N** headings. Each card has a 32 px round avatar with a presence
dot (green, amber for away, grey); offline faces are drawn at 40 % opacity
and their names in the faint colour. The name label is `chatMemberName`, and
`testChatMemberNames` reads it by that name now that the text has no dot.

**Transcript.** Avatars are 40 px circles with 16 px to the text. The name
is semi-bold in the strong text colour; the time is faint 12 px and reads
"4:22 PM" today, "Yesterday at 11:48 PM" yesterday and "10/1/26, 12:27 AM"
before that (`MessageRow::chatStampLabel`). A message from the same sender
within seven minutes on the same day, with no attachment on either side,
continues the one above (`MessageRow::continues`, `setContinuation`): no
avatar, name or expiry ring, a 1 px gap, the full time as the row tooltip,
and the "⋯" actions button floating at the top right while the pointer is
over the row. `addMessageRow` puts a centred date divider ("October 8,
2026", rules on both sides) before the first message of each day.

**Composer.** One rounded bar in the composer colour: a round `+` attach
button, the input, then link, mention, emoji and send. The bold, italic,
strike, code and list buttons are gone from view; their shortcuts (Ctrl+B,
Ctrl+I, Ctrl+Shift+X, Ctrl+Shift+C) and the syntax tooltip on the input are
unchanged.

`FORKMESH_CHAT_SHOT=<prefix> forkmesh-window-tests --chat-unread-only`
saves the light and dark chat page; the shot now includes a continued
message.

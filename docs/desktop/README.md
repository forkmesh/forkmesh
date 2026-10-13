# Desktop app (Qt)

The desktop app is the ForkMesh workspace: a Qt 6 Widgets program (`forkmesh`)
that keeps local Git data, signs collaboration records (issues, pulls, chat,
reviews), runs coding agents in worktrees, runs `.forkmesh` workflows, and
operates the user's mirror hosts over SSH. It runs on Linux, macOS and Windows.
It does not serve repositories to the network; managed mirrors run the Go
daemon in `server/` (see [../server/README.md](../server/README.md)). The same
binary runs on the deploy box as the "flagship" node, built from the primary
checkout.

User-facing guide: [www/docs/qt-client.md](../../www/docs/qt-client.md).
Topic pages in this folder:

- [build-and-test.md](build-and-test.md): CMake, `run.sh`, Rebuild & restart, ctest, CI
- [diagnostics.md](diagnostics.md): logs, stall watchdog, crash repair, Health and Memory pages
- [networking.md](networking.md): relay transport, HTTP/3 client, local mesh, phone LAN control, hosts
- [agents-ui.md](agents-ui.md): Agents page, activity window, sounds and announcer
- [branch-diffs.md](branch-diffs.md): the branch/pull review pane renders wide diffs on a worker and streams them in
- [tasks-page.md](tasks-page.md): Tasks page layout, top-search filtering, row and detail design, task work items and delete cleanup
- [saved-searches.md](saved-searches.md): saved searches, their pings, and the security-pertinent alarm
- [ui-redesign.md](ui-redesign.md): the interface redesign plan: design tokens, components, welcome and sign-in, expanded sidebar, phases and rules
- [windows.md](windows.md): Windows-only behaviour, installer, the 1.2.4 Actions lock fix
- [git-locking.md](git-locking.md): per-repository queue for index-writing Git commands, `index.lock` recovery, merge timeout
- [lifecycle.md](lifecycle.md): what a quit stops (agent and Actions scopes, escaped children) and what it leaves running
- [genie.md](genie.md): the Genie spider pet, its health from code and marketing velocity, and the marketing coach
- Agent runs across desktop, mirrors, web and phone: [../agents/README.md](../agents/README.md)

## Code map

Everything is in `desktop/`.

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt` | The app target, ~80 test targets, optional modules, job pools, version stamping |
| `run.sh` | Build (incremental) and launch; `test`, `clean`, `rebuild` |
| `install.sh` | User-level XDG install to `~/.local/bin/forkmesh` plus launcher; `--uninstall [--purge]` |
| `cmake/` | Version header template, MinGW toolchain file for the Windows cross-build |
| `scripts/package-macos.sh` | macOS `.app`/`.dmg` packaging |
| `src/main.cpp` | Startup: paths layout, single instance, CLI modes, crash handler, Actions recovery |
| `src/MainWindow.h`, `src/MainWindow*.cpp` | The main window, split by feature into ~79 translation units (one class) |
| `src/MainWindowInternal.h`, `src/MainWindowInternal1-4.cpp` | Shared helper widgets and functions (namespace `forkmesh::ui`); bodies live in the four `.cpp` files |
| `src/MainWindowNavigation.cpp` | The left rail and its sections |
| `src/AppPaths.{h,cpp}` | The `~/projects/<project>/...` layout and migration from legacy paths |
| `src/Theme.h`, `src/DesignTokens.{h,cpp}`, `resources/theme/next/*.qss` | The two Classic style sheets; the colour roles, palette maps and overlay sheets of the opt-in new interface ([ui-redesign](ui-redesign.md)) |
| `src/UiKit.{h,cpp}` | Shared components of the new interface (toggle, card, settings row, badge, notice, segmented control) |
| `src/NextShellHooks.h`, `src/MainWindowNextAccess.h`, `src/MainWindowNextShell.cpp`, `src/MainWindowWelcome.cpp`, `src/MainWindowCommandPalette.cpp` | New-interface shell: rail and tab painting, expanded sidebar, welcome and sign-in card, command palette |
| `src/Agent*.{h,cpp}`, `src/ClaudeStreamSession.*`, `src/CodexAppServerSession.*` | Local agent runtimes, guardrails, worktrees, access gates |
| `src/ControlNode.{h,cpp}` | Host/fleet operations: SSH commands, installers, agent CLI bootstrap/refresh |
| `src/MirrorActionsConfiguration.*`, `src/ActionRunner.*`, `src/ActionStore.*` | `.forkmesh` workflows and the mirror Actions transaction lock/journal |
| `src/ServerNode.*`, `src/ChatBackend.h`, `src/NodeEventSocket.*` | Relay HTTP/WebSocket transport and event topics |
| `src/Http3Get.*` | In-process HTTP/3 (OpenSSL QUIC + nghttp3) |
| `src/LocalMeshNode.*`, `src/LocalControl.*`, `src/MeshFirewall.*` | LAN mesh discovery and the phone control channel |
| `src/StallWatchdog.*`, `src/CrashHandler.*`, `src/CrashLogScan.*` | UI stall sampling, crash records, crash-repair dispatch |
| `src/CodeHealth.*`, `src/ComponentHealthBoard.*` | The Health page (per-file code health) |
| `src/StatsDashboard.*`, `src/MainWindowStats.cpp` | The Stats page |
| `src/HomeBoard.*`, `src/HomeLayout.*`, `src/MainWindowHome.cpp` | The Home page: every rail page as an icon on user-arranged pages, with live counts |
| `src/UiSounds.*`, `src/UiSoundCues.cpp`, `src/AgentSonifier.*`, `src/AgentAnnouncer*.*` | Interface sounds, Listen, Announcer |
| `src/NativePackageExtension.*`, `src/ForkMeshPackageRegistry.*`, `src/MainWindowPackageExtensions.cpp` | Market packages hosted inside the desktop |
| `src/LocalBackupStore.*`, `src/BackupHostFeed.*`, `src/MainWindowData.cpp` | Hourly snapshots, restore, and the host socket the `backups` package page uses |
| `src/CleanupHostFeed.*`, `src/MainWindowCleanup.cpp` | Host socket for the `cleanup` package: sessions, task work items, repositories, worktree removal and Actions sandbox removal |
| `src/MarkdownRender.h`, `src/MarkdownDocument.h` | Markdown prepared for `QTextBrowser` and the README styling any rendered view shares (the Docs package uses it too) |
| `src/Genie*.{h,cpp}`, `src/MainWindowGenie.cpp` | The Genie spider pet and marketing coach |
| `src/PetHostFeed.*` | Host socket that lets a package see and nudge the Genie spider (the `aquarium` package) |
| `tests/` | Qt Test programs (`test_*.cpp`) and Python source validators (`validate_*.py`) |

## Architecture

- **One process per project.** `forkmesh --project <slug>` selects an app-level
  project; each has its own lock and single-instance socket
  (`src/SingleInstance.cpp`, `src/ProjectRegistry.cpp`). The default project is
  `forkmesh`.
- **Project picker.** The top-left dropdown (`src/MainWindowProjectPicker.cpp`)
  lists every folder in the projects root whose name is a valid slug, including
  plain folders with no `project.json`; hidden folders and symlinks are left
  out. Each row is labelled by its folder name (`forkmesh`, `blt`,
  `forkmesh-next`), matching `ls ~/projects`; the `name` in `project.json` is
  kept for the Project settings page. Every row has a folder button that opens the
  project's folder in the system file manager (`QDesktopServices::openUrl`),
  beside the gear for its settings. Its strings use literal `…`, `’` and `✓`:
  `QStringLiteral("\xE2\x80\xA6")` makes one UTF-16 unit per byte and showed
  as `â€¦`. Removing a project sets `project.json` aside as `project.json.removed`
  (a plain folder gets an empty marker), and a folder with that marker and no
  `project.json` is no longer listed. Only a folder ForkMesh created (its
  `project.json` has `created`, written by `projects::create`) can also be
  moved aside to `<slug>.deleted-<stamp>` (never deleted); a plain folder such as `~/projects/forkmesh-backup` can only be
  unlisted, and saving its settings (which writes `project.json`) does not
  change that. A folder that is itself a git checkout is listed but will not
  open, because the project's `data/`, `ssh/` and identity key would land in
  the repository's working tree.
- **A new project has no instance.** "Add project" no longer offers "Connect to
  a relay": `projects::create` always seeds offline mode (not automatic),
  `project/instancePending=true`, and `project/instanceChoices`, the creating
  project's saved instances as `[{name,url}]`. Before this, a new project that
  went online fell back to `kDefaultServerUrl` in `loadServers()` and joined
  app.forkmesh.com without being asked. When the new project's window
  finishes starting, `promptChooseProjectInstance()` asks which instance to use
  (one of the offered ones, or an address typed in). Until the user chooses,
  the relay picker reads "Choose instance" and opens that dialog, and turning
  offline mode off opens it too. `connectProjectInstance()` clears both
  settings, switches to the chosen instance, drops the `loadServers()`
  placeholder, and goes online.
- **A project with no instance lists nothing from the network** (2026-10-13).
  Asked: a non-home project showed `forkmesh/forkmesh` and
  `newnewnode/forkmesh` from app.forkmesh.com although it was attached to no
  instance. While `project/instancePending` is set, `loadServers()` loads and
  synthesizes no instance (`m_servers` stays empty, `m_activeServer` is -1,
  `saveServers()` writes nothing), `catalogApiUrl()` and `activeRelayOrigin()`
  are empty, the relay edit is left blank, `startSession()` keeps the backend
  off the network and does not save `server/url`, and the network
  repositories page, `refreshRelayRepositoryList()`, `fetchCatalogRepos()` and
  `fetchNodesOnline()` send nothing; the page reads "Choose an instance". The
  repository switcher shows only local workspaces (not preview-only network
  records). Projects made before the pending flag existed are caught at
  startup by `projects::markUnattachedProjectPending()`: a non-home project
  without `project/instanceAttached` whose saved instances are none or only
  the default instance is set pending and offline (not automatic), with the
  instance it held offered back as a choice. `connectProjectInstance()` sets
  `project/instanceAttached`, so a chosen instance (app.forkmesh.com
  included) is kept from then on; a project that once picked app.forkmesh.com
  before this change is asked once more. A project that saved any other
  instance is left alone, and the home project (`forkmesh`) never changes.
- **Inviting someone to a project.** The current project's row in the picker
  has a people button, and the menu has "Invite someone to this project…";
  both open `promptInviteToProject()`. A person who already has an account on
  the project's instance is checked with the account lookup
  (`coveInviteAccountValid`) and sent a direct message that names the project
  and the instance to connect to. The message is queued in
  `m_pendingDirectInvites` and sent when `OfficeChannelMirror` reports the
  direct conversation ready, so the window stays on the dialog instead of
  jumping to chat; the result shows in the dialog. Anyone else gets an email:
  "Open email" hands a `mailto:` link to the system mail app, with the
  instance's referral link `https://<instance>/r/<your account>` (it counts the
  click and leads to sign-up), or `/signup` when you are not signed in. A
  project with no instance yet can only send the email, and it has no link.
  When no mail app opens, or Messages are offline, the text is copied instead.
  The text and the link are built in `src/ProjectInvite.cpp`, tested by
  `forkmesh-project-invite-tests`.
- **Paths.** `src/AppPaths.cpp` resolves `~/projects/<project>/` (override with
  `FORKMESH_PROJECTS_DIR` or `FORKMESH_HOME`) into `data/`, `config/`,
  `cache/`, `state/`, `logs/`, `ssh/`, `toolchain/`, with the checkout in
  `src/`. Legacy locations (`~/.local/share/ForkMesh`, `~/.config/ForkMesh`,
  `~/.forkmesh`) are moved by rename with compat symlinks
  (`forkmesh --migrate-layout --dry-run` previews). `tools/forkmesh_paths.py`
  and `tools/forkmesh-paths.sh` implement the same rules and must agree.
  At startup the app exports `FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR` to
  every child process, including agents (see `AGENTS.md` for why tests must
  drop them).
- **Settings.** `QSettings` is re-homed into `config/ForkMesh/ForkMesh.conf`
  on Linux and macOS. On Windows the home project's settings are the registry
  (`HKEY_CURRENT_USER\Software\ForkMesh\ForkMesh`); code must not derive a file
  path from `QSettings::fileName()` there (see [windows.md](windows.md)).
- **Rail sections.** `src/MainWindowNavigation.cpp` builds the rail: Repo,
  Repos, Market, Ledger, Tasks, Calendar, Notes, Pings, Agents, Chat,
  Settings, Tour, Log, Control, Network, Users, Team, Files, Stats, Health,
  Memory, Restart. Section indices are constants in `src/MainWindow.h`
  (`k*SectionIndex`); Health is 24, Memory is 25 and is the last deferred
  section. Every rail count badge is blue (`#1f6feb`), Pings and Chat included:
  they used to set `setBadgeUrgent(true)` and paint red, which made routine
  unread counts look like errors.
- **Rail folders.** Rail icons can be grouped into folders: right-click an
  icon and choose **Move to folder**, or drag it onto a folder; right-click an
  empty part of the rail for **New folder…**. A folder opens when the pointer
  rests on it: its icons pop out to the right of the rail in a horizontal row
  (`RailFolderPopout`), and the row closes shortly after the pointer leaves
  both the folder and the row, so an item inside still launches with one
  click. Clicking the folder keeps it open; clicking an item in the row opens
  its page and closes the folder, even a pinned one. The row slides out from
  behind the rail edge as it opens and slides back in as it closes, a drawer
  effect driven by one `QVariantAnimation` (about 200 ms out, 170 ms in, eased)
  that moves the row and masks off the part still left of the rail, so the
  row's rounded leading edge stays intact. Reversing mid-slide continues from
  where the row is; a folder hidden with the rail closes at once without
  animating. The row is a child of the main
  window rather than the rail, so the rail's scroll area does not clip it; the
  folder moves it when the rail scrolls or the window resizes, deletes it with
  itself, and closes it when the rail is hidden. Dropping a rail icon on the
  row files it like dropping it on the folder. The
  folder takes its first item's place in the rail order, marks itself while
  one of its pages is open, and adds up its items' badges while closed.
  The real rail buttons move into the folder (`RailFolderWidget` in
  `src/MainWindowInternal.h`), so their badges, checked state and menus keep
  working. The folders are saved as JSON under `ui/railFolders`
  (`src/RailFolders.h`), keyed by the same stable rail keys that
  `applyRailOrder` uses for click sorting. Removing a folder puts its items
  back on the rail. For movable buttons (below) an item inside a folder counts
  as in the rail (`isInRail()`), so `placeMovableButtons()` leaves it there
  and its menu offers "Move to repository tab bar". A button moved to the tab
  bar keeps its folder entry and goes back into that folder when it is moved
  back to the rail. Buttons outside the rail keep their own right-click menu;
  dragging one onto the rail moves it there (see Dropping any page button
  into the rail below). Dragging shares one gesture with rail reordering (see
  Dragging rail icons below) and uses its `application/x-forkmesh-rail-item`
  payload. `RailDragReorder` drags icons that sit directly in the rail and
  reorders them live; a folder accepts that drag and files the icon. Icons
  inside a folder are not in the rail layout, so the folder filter
  (`handleRailEvent`) starts their drag. It only claims drops on the open rail
  for icons already in a folder, which takes them out, and leaves every other
  drop to the reorder filter. A folder follows its first item in the
  rail order and is not dragged itself. `~MainWindow` deletes the rail event filter first:
  rail buttons are destroyed after MainWindow's own members, and the filter
  would otherwise read the destroyed folder table from their destroy events.
- **Tour.** The first-run welcome and the feature tour are the bundled `tour`
  package (`marketplace/packages/tour`), installed by default since
  2026-09-27 so a user can remove it once they know the app: from the Market
  or by right-clicking Tour in the rail. The overlay widget lives in the
  package; `src/MainWindowOnboarding.cpp` keeps the step list, because every
  step points at a core rail control. With the package removed the Tour
  button is hidden, the welcome never opens and `startFeatureTour()` does
  nothing; an install from the Market brings all of it back.
- **Movable buttons.** Right-click any left rail button or repository tab bar
  button (a tab such as Pulls or Issues, or Notify, Fork, Mirror, Source, Open)
  and choose "Move to left rail" or "Move to repository tab bar". The Code
  page's toolbar buttons (Overview, Branches, Worktrees, Remotes, Tags,
  Releases) move the same way, between their toolbar and the rail ("Move to
  Code toolbar"); in the rail they go into folders and drag like any rail
  icon. Placement is
  stored per button key in `ui/buttonPlacement` (`repoTab:<label>`,
  `repoAction:<name>`, `repoCode:<name>`, or the rail key); only moved
  buttons are stored, so a
  button moved back home drops out of the map. `placeMovableButtons()` runs at
  the top of `applyRailOrder()`, so every rail rebuild re-applies placement.
  Repository tabs moved into the rail sit after Git and, like Code and Git,
  appear once a repository has been opened (the repository page is built
  lazily and its build re-runs `applyRailOrder()`); clicking one from another
  section opens the repository page first. A tab in the rail marks its open
  state with the rail's left bar instead of the tab bar's underline
  (`VerticalIconButton::setInRail`). Rail buttons moved into the tab bar
  sit after the tabs, before the stretch. The Code toolbar buttons sit in
  their own row inside the toolbar (`m_repoCodeBarLayout`);
  `applyRepoCodeBarOrder()` puts one moved back into its original place, and
  the Code tab's lazy build re-runs `applyRailOrder()`. A Code toolbar button
  clicked in the rail opens the repository page first, Overview included
  (it selects the Code tab itself), and Overview marks itself open in the rail
  only while the file list shows. Settings -> Appearance "Reset moved
  buttons" (or "Reset button placement" in any moved button's menu) clears the
  map and the dragged rail order.
- **Dragging rail icons.** Drag any left rail icon up or down to reorder the
  rail. An event filter (`RailDragReorder` in `src/MainWindowNavigation.cpp`)
  starts a `QDrag` once a left press moves past the drag distance, moves the
  icon live as it passes the other icons, and on drop stores the rail's keys
  in `ui/railOrder`; a drag that ends outside the rail puts the icon back.
  Keys saved earlier for icons not in the rail right now (a repository tab, a
  package page) keep their place after their saved neighbour.
  `applyRailOrder()` sorts by that list; an icon missing from it (a new
  package page, a button moved in later) follows its default neighbour. A
  drop switches the rail back to custom order (and so turns "Sort the left
  rail icons by most clicked" off), since any other sort would undo the drag
  at once; switching a sort back on sorts with the dragged order breaking ties.
  With groups on, an icon dropped under another group's separator joins that
  group (`adoptRailGroupsFromLayout`).
- **Rail sort button and groups.** A tiny sort button (`railSortButton`) sits
  at the top of the rail, above the icons, in its own row of the rail content
  so it is never taken out by `applyRailOrder()`'s rebuild of
  `m_appNavigationRailLayout` (a sub-layout under it). Its menu (also under
  "Sort the left rail" in the rail's right-click menus) picks the sort mode,
  saved as `ui/railSortMode`: custom order (the dragged order above),
  alphabetically by caption, most frequently used (the click counts in
  `ui/railClickCounts`) or by count (the blue badge, highest first; a badge
  change re-sorts after 400 ms, `scheduleRailCountSort`). Every sort is
  stable over the custom order, and a folder sorts as one item: by its name,
  or by the sum of its icons' clicks or badges, with the same sort applied to
  the icons inside it. "Most frequently used" is the same setting as Settings
  -> Appearance "Sort the left rail icons by most clicked": `setRailSortMode`
  keeps `ui/sortRailByClicks` in step, and an older settings file with only
  that key reads as "most used". "Group the icons" (`ui/railGrouped`, off by
  default) splits the rail into groups. A group is only a labelled separator
  line (`RailGroupSeparator`), not a folder: nothing collapses or pops out,
  the icons stay in the rail under it, and the sort applies inside each
  group. The built-in groups (Work, People, Network, Insights, Packages,
  Tools, Shortcuts, More) are the Home page's groups, so `src/RailGroups.h`
  holds the one list both use; a built-in group with no visible icon hides
  its separator. "New group…" adds a custom group at the top; right-click an
  icon or folder for "Move to group", or drop an icon on a custom group's
  separator (an icon in a folder leaves the folder). Right-click a separator
  to rename, move up or down, or remove a custom group (its icons go back to
  their own groups), or to stop grouping. Groups, their order and each
  icon's group live as JSON under `ui/railGroups`, keyed by the rail keys;
  an icon in its default group stores nothing, and "Reset groups" clears the
  key.
- **Dragging a rail icon out of the window** opens its page in a window of its
  own, for instance Issues moved into the rail, Notes or Chat; "Open in new
  window" on the icon's right-click menu does the same. Both rail drag
  starters hold a `RailDragOutWatch` for the length of `QDrag::exec`. It
  watches the drag events sent to the app's `QWindow`s: a `DragLeave` with no
  later enter and no `Drop` means the icon was let go outside every ForkMesh
  window. On Wayland a drop the client never accepted is cancelled and looks
  the same as one outside, so the watch also makes the main window (and a
  folder's popout) accept rail drags everywhere and drop them as a no-op. An
  Escape that cancels a drag after the pointer has left the window counts as
  a drop outside. A drop outside does not bounce back. The compositor (GNOME
  on Wayland) or macOS slides a drag nobody accepted back to where it
  started, so on those platforms the watch opens a `RailDropCatcher` when the
  pointer leaves the window, but only for an icon that can pop out. The
  catcher is a borderless, see-through window over the screen (maximized on
  Wayland, where a window cannot be placed) that accepts the drop. Wayland
  gives no global pointer position, so its first drag event is matched to the
  point where the drag left the window, and the catcher hides again when the
  pointer comes back inside a ForkMesh window, where the rail can take the
  drop. On X11, Windows and macOS without a catch the catcher is made after
  the drop at `QCursor::pos()`. The dropped icon then spins at the drop point
  in a window shaped (`setMask`) to the icon, so clicks pass through. It
  stays until the popped-out window's `QWindow` is exposed, or at most 8 s.
  The pop-out starts 60 ms after the drop so the spin has painted before
  building the page window blocks the event loop.
  `MainWindow::popOutRailItem` clicks the icon, so the page is
  the one a click shows, then moves the current page of the section stack,
  or of the repository tab stack for repository tabs, Code, Git, Agents and
  the Code toolbar buttons, into a `PoppedOutPageWindow`. A placeholder takes
  the page's index, because the rail's `QButtonGroup` ids and every
  `setCurrentIndex` caller address pages by index. Opening the page from the
  main window shows the placeholder and raises the window. Closing the window
  or "Return to main window" puts the page back at the placeholder's index;
  the window's geometry is kept under `ui/poppedOutPages/<rail key>/geometry`
  and, on X11, a drag-out places the window at the pointer. Package pages use
  their own window (`openNativePackageWindow`, see
  [marketplace](../marketplace/README.md)). Buttons that run an action
  (Screenshot, Tour, Fork, Mirror, Notify) have no page and a drag out of the
  window just puts them back. `~MainWindow` docks every popped-out page first
  so teardown sees the usual widget tree.
- **Dragging the debug bar's Log button out of the window** (the one next to
  Restart and Resize, which otherwise toggles the five-line log tail) opens
  the full Log page in a popped-out window; its right-click menu has "Open Log
  in new window" too. `MainWindow::attachPageDragOut` gives any button that
  drag with the same `RailDragOutWatch`, but under its own
  `kPoppedOutPageMimeType`, so dragging across the rail never reorders it or
  drops a shortcut. `MainWindow::popOutLogPage` builds section 4 if it is
  still deferred and moves it out through `popOutStackPage` without switching
  the main window's section, then runs `catchUpLogPage`, the stale-view
  rebuild and live-tail follow that `showSection(4)` runs, because lines that
  arrived while the page was hidden were not rendered. Dragging it out again
  raises the same window; its geometry is kept under
  `ui/poppedOutPages/log/geometry`.
- **Dropping any page button into the rail** (`src/MainWindowRailShortcuts.cpp`,
  `src/RailShortcuts.h`). Any button in the main window (or a popped-out page
  window) that opens a page can be dragged onto the left rail; a green line
  shows where it will land. A movable button (repository tab, Code toolbar
  button, rail button parked in the tab bar) moves into the rail itself, as
  "Move to left rail" does. Every other button becomes a rail shortcut: an
  `ActivityRailButton` with the button's octicon (or `link`) and caption,
  keyed `shortcut:<id>`, so it reorders, goes into folders and follows the
  caption preference like any rail icon. Right-click it for "Remove from left
  rail". Shortcuts are stored as JSON under `ui/railShortcuts`: the button's
  path from the main window, its label, icon, and the section (and repository
  tab) it lives on. A path segment is the page index for a `QStackedWidget`
  child (section pages are built lazily, so their child order changes between
  runs), the object name when it is unique among its siblings, or the class
  name and index otherwise; a popped-out page is addressed through its
  placeholder's index. Clicking a shortcut opens the section and repository
  tab, finds the button by path (or, if the page changed, by label on that
  section), switches any hidden inner stack or tab to the button's page and
  clicks it, so the shortcut does exactly what the button does. A repository
  page shortcut asks for a repository to be open first.
  Which buttons open a page cannot be known without clicking them, and a
  test click could run an action, so only these buttons drag: movable
  buttons, checkable buttons in an exclusive `QButtonGroup` (tab strips), and
  buttons that have been seen opening a page. The app event filter
  (`handlePageButtonEvent`) notes the button a left click released on; if
  within 1.5 s a page of a `QStackedWidget` at least a fifth of the window's
  area is shown, or the Back/Forward trail records a new place, the button's
  path is remembered under `ui/pageButtons` (the 400 most recent). Rail
  items, folder popouts, Back/Forward, placeholders and buttons with a menu
  never drag. Other buttons keep their plain click behaviour.
- **Settings page** is one scrolling page with a section list on the left
  (`src/SettingsSectionsView.*`). Each section folds to its heading (click
  the heading or its arrow). Sections added with `addLazySection` are built
  only when first opened; a search builds and opens every section so it can
  match their controls, then folds them back when cleared. Jumping to a
  section (the list, `setCurrentIndex`, `revealWidget`) opens it. Folded
  sections are not "revealed", so Apps and Organizations refresh only once
  open. The open/folded choice per title is saved in
  `settings/sectionsExpanded` (title -> bool); titles not in it use the
  defaults in `restoreSettingsSectionsExpanded()`.
- **Actions strip in the top chrome** (`ActionRunStrip` in
  `src/MainWindowInternal.h`, fed by `refreshActionRunStrip()`). While no
  run is running it shows the grid of status squares for recent runs. While
  any run is running it shows one icon per running workflow instead, as tall
  as the three-square grid (21 px), with a slow glow-and-scale breathing
  animation. Clicking an icon opens that workflow's newest running run.
  Each workflow (repository plus workflow path) gets its own icon for the
  session (`actionWorkflowIconName()`): a keyword in its name or file picks
  one (deploy → rocket, build → package, test → shield-check, and so on). If
  there is no keyword, or another workflow already has that icon, it takes
  the first unused icon from a fixed list, starting at a hash of its key. The
  icon colour is a hash of the same key.
- **Git work off the GUI thread.** Git is run through helpers in
  `src/MainWindowInternal.h` (`waitForGit`, `runOffThread`); a GUI-thread wait
  pumps the event loop. New code that runs Git on the GUI thread shows up in
  the stall log.
  Index-writing commands for one repository run one at a time through
  `src/GitIndexQueue.*`; see [git-locking.md](git-locking.md).
- **No display polling.** Since 2026-09-19 the desktop has no timers that
  re-read the relay for display; updates arrive over the account event socket
  (`src/MainWindowRepoSync.cpp` handles topics such as `cloud-agents`,
  `tasks`). Catch-up reads are bounded by floors measured in minutes. See
  [www/docs/operations/polling-elimination.md](../../www/docs/operations/polling-elimination.md).

## Protocols and interfaces

- Relay HTTP API and account event socket: `src/ServerNode.*`,
  `src/MainWindowRepoSync.cpp`. Signed requests use the node's Ed25519 key
  (`src/ForkMeshIdentity.*`). The desktop has no browser session token and
  sends no `Origin` header, so it uses key-signed routes, not cookie routes.
- Website-queued agent jobs: `GET /api/repo/<node>/<repo>/org-agent-jobs` and
  `POST .../org-agent-jobs/<id>/result` (`src/MainWindowAgentSync.cpp`);
  a job's images come from `.../org-agent-jobs/<id>/attachments/<id>`
  (`src/OrgAgentJobAttachments.*`, see `docs/agents/README.md`).
- Cloud agent rows: `GET /api/account/agent-sessions`
  (`src/MainWindowAgentCloud.cpp`).
- Cloud notes: `/api/notes`, with a Bearer session or an account-key signed
  request (`src/MainWindowNotes.cpp`). See [notes.md](notes.md).
- Local mesh and phone control: TCP/UDP 48732 (`src/LocalMeshNode.cpp`,
  `src/LocalControl.*`).
- Editor bridge: `src/ClaudeIdeBridge.*`. MCP: `src/McpConnector.*`,
  `src/MainWindowMcp.cpp`; the MCP server tool itself is `tools/forkmesh_mcp_server.py`.
  Its `create_agent` tool reaches the desktop over the agent-intake socket
  (`src/AgentIntake.*`, `src/MainWindowAgentIntake.cpp`,
  [../tools/mcp-create-agent.md](../tools/mcp-create-agent.md)). Its
  `show_window`/`read_window`/`close_window` tools open small agent windows
  over the same socket (`src/AgentWindows.*`,
  [../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md)), which also
  records the 2026-10-01 transcript update (`src/ClaudeTranscriptTools.*`).
  Its `list_releases`/`create_release`/`publish_release` tools cut and publish
  releases over the same socket without the Draft a new release dialog
  (`src/MainWindowRemoteReleases.cpp`, `src/ReleaseDraft.*`,
  [../tools/mcp-releases.md](../tools/mcp-releases.md)).
- Launch targets: `forkmesh <target>` takes a `forkmesh://` link
  (`main.cpp`) or an absolute folder path or `file://` URL
  (`src/SystemOpenFolder.*`). A second launch hands the target to the
  running window over the single-instance socket, and macOS delivers it as a
  `QFileOpenEvent`; both end in the same handler. A folder goes to
  `MainWindow::openFolderFromSystem`, see
  [Open with ForkMesh](#open-with-forkmesh).

### Open with ForkMesh

The file manager's right-click menu has **Open with ForkMesh** on folders.
It launches `forkmesh <folder>`, and the desktop then:

1. opens the repository whose `localPath` is the folder or contains it (the
   innermost one when repositories are nested);
2. otherwise imports it with the same flow as Add repository
   (`importRepositoriesFrom`): the folder itself if it is a Git checkout, its
   immediate child checkouts if it holds some, or else the checkout around it,
   so a right-click on `repo/src` imports `repo`. After a single-repository
   import from the menu the repository opens.

The visibility question (Public, Private, Local only) is always asked, so a
right-click never publishes anything by itself. A path after `--project` is a
project name, never a folder to open.

Where the entry comes from:

| Platform | Written by | Entry |
|---|---|---|
| GNOME Files (Nautilus) | `desktop/install.sh` | `~/.local/share/nautilus/scripts/Open with ForkMesh`, under **Scripts**. Shown on folders, files (their folder opens) and the folder background |
| KDE Dolphin | `desktop/install.sh` | `~/.local/share/kio/servicemenus/forkmesh-open.desktop`, top level, folders only |
| Cinnamon Nemo | `desktop/install.sh` | `~/.local/share/nemo/actions/forkmesh-open.nemo_action`, single folder |
| Windows Explorer | `tools/package/forkmesh.nsi` | `HKCU\Software\Classes\Directory\shell\ForkMesh` (a folder) and `Directory\Background\shell\ForkMesh` (the open folder); on Windows 11 under "Show more options" |

`desktop/install.sh` writes each Linux entry only when that file manager is
installed; `--uninstall`, `www/public/install.sh --uninstall` and
`www/public/uninstall.sh` remove all three, and so does **Settings → Uninstall
ForkMesh**. The launcher keeps
`MimeType=x-scheme-handler/forkmesh;` only: listing `inode/directory` there
would put ForkMesh under "Open With" too, but a user-level entry that claims
`inode/directory` can become the default folder handler on desktops without
an explicit default, so `xdg-open <folder>` would start ForkMesh instead of
the file manager. macOS has no entry yet: Finder needs a document type or a
Services entry in the bundle's `Info.plist`, which CMake generates from its
default template today.

## Configuration

- Environment: `FORKMESH_HOME`, `FORKMESH_PROJECTS_DIR`, `FORKMESH_PROJECT`,
  `FORKMESH_START_DIRECTORY` (set by `run.sh`), `FORKMESH_SONIFY_BACKEND`
  (`silent`, `pipe`, `none`), `QT_QPA_PLATFORM=offscreen` for tests.
- CMake cache options: `FORKMESH_BUILD_TESTS`, `FORKMESH_VERSION_OVERRIDE`,
  `FORKMESH_BUILD_COMMIT_OVERRIDE`, `FORKMESH_HEAVY_JOBS_OVERRIDE`.
- Notable settings keys: `diagnostics/autoAgentOnErrorLog` (crash and error
  repair dispatch), `ui/interfaceSounds`, `agents/listenToNewRuns`, `ui/buttonPlacement`,
  `chat/retentionPreferences` (last known chat message retention).
- Per-project agent access policy: `project.json` key `agentAccess`
  (`src/ProjectAgentAccess.*`).

## Build, run, test

```sh
./desktop/run.sh            # incremental Release build, then launch
./desktop/run.sh test       # build and run the "critical" ctest label
```

Details, manual CMake, test targets and known failures:
[build-and-test.md](build-and-test.md).

## Operations

- The deploy box runs the flagship from `~/projects/forkmesh/src`
  (binary `desktop/build/forkmesh`). **Restart** on the rail (Rebuild &
  restart) rebuilds that checkout and relaunches; it waits for running agents.
- Hosts: **Network → Hosts** installs, updates and removes ForkMesh on SSH
  hosts; every host update also refreshes the host's agent CLIs (see
  [../agents/README.md](../agents/README.md#agent-cli-refresh-on-hosts)).
- Logs and diagnosis: [diagnostics.md](diagnostics.md).

## Security

- The node identity key lives under `data/`; losing it loses the account. Never
  delete `~/projects/forkmesh/{data,config,ssh,state,logs}` (see `AGENTS.md`).
- Bearer tokens are held in memory and never written to `QSettings`
  ([www/docs/qt-client.md](../../www/docs/qt-client.md#multiple-relay-instances)).
- AWS credentials (Settings → Cloud) are sealed with AES-256-GCM under a
  0600 key file in `data/secrets/`. `ForkMesh.conf` holds only the
  ciphertext and the region. The Vultr API key is still a plain-text device
  variable ([networking.md](networking.md#aws-credentials)).
- The fleet SSH key is `ssh/forkmesh_shared_ed25519` (legacy
  `ssh/vultr_mirror_ed25519` is used when it is the one present), resolved by
  `sharedHostKeyPath()` in `src/ControlNode.cpp`.
- Agent sandboxing: Landlock folder lock, external-call gate, optional KVM
  guest. See [../agents/README.md](../agents/README.md#desktop-agent-sandboxing).
- Package extensions hosted in the desktop get settings and data scoped per
  package (`src/NativePackageExtension.*`).

## Traps

- **Qt never draws a tooltip on screen.** `Tooltips::install()`
  (`src/Tooltips.h`, called from the `MainWindow` constructor) marks Qt's
  `QTipLabel` `WA_DontShowOnScreen` and draws its text in its own
  `Tooltips::Window`. Keep using `setToolTip`, `Qt::ToolTipRole` and
  `QToolTip::showText`/`hideText`; `QToolTip::text()` and `isVisible()` still
  answer. Do not restyle tips through `QToolTip { }` rules: the window paints
  `QPalette::ToolTipBase`/`ToolTipText` itself. Why it exists is in Recent
  changes, 2026-10-06 "Tooltips".
- **`MainWindowInternal.h` is in the precompiled header.** Editing it, or
  `MainWindow.h`, recompiles every `MainWindow*.cpp`. New helper bodies go in
  `src/MainWindowInternal1-4.cpp` with only a declaration in the header.
  New shell behaviour for the opt-in interface goes through
  `forkmesh::ui::NextAccess` (a friend of `MainWindow`, defined outside the
  precompiled header) and the paint hooks in `NextShellHooks.h`, so neither
  header needs another edit; see [ui-redesign.md](ui-redesign.md).
- **Namespace `forkmesh::ui`.** Helper widgets in `MainWindowInternal.h` live
  in `forkmesh::ui`; a bare `class Foo;` in `MainWindow.h` declares a different
  class. Forward-declare inside the namespace.
- **Only the `forkmesh` target compiles `src/main.cpp`.** `MainWindow*.cpp`
  are compiled by the app and by `forkmesh-window-tests`/`forkmesh-perf-tests`
  (`FORKMESH_APP_SOURCES`), not by the small unit targets. Build the app before
  calling a change done. A `ninja ... | tail` pipeline hides the build's exit
  status.
- **New `Q_OBJECT` headers** must be listed for moc in `CMakeLists.txt`. Several
  newer classes (`LocalControlServer`, `ComponentHealthBoard`,
  `StatsDashboard`, `Http3Get`) avoid `Q_OBJECT` on purpose and use
  `std::function` callbacks.
- **External and cloud agent rows have negative ids** (`<= -1000`, cloud rows
  `<= -1000000`). A guard like `sessionId < 0` silently excludes them; use
  `isExternalSession()`/`isCloudSession()`.
- **`AgentSession::name` is the repository name**, not the task title (that is
  `issueTitle`). It also seeds the agent's colour and voice.
- **Show a chat row as soon as it is inserted.** `addMessageRow`
  (`src/MainWindowMessages.cpp`) calls `row->show()` right after
  `insertWidget`. Without it the layout shows the row through a queued call,
  so for one pass the new rows count as empty and the message scroll area
  keeps the container at its old height. The next pass lays every row out
  inside that height, and the frame painted then showed only squeezed
  avatars and sender names with no text. Opening a busy channel such as
  `#office/email-log` showed it every time (2026-10-06): 40 rows were laid
  out in a 682px container that needed 10,770px.
- **Registry-backed settings on Windows**: `QSettings::fileName()` is a
  registry path; never build a file path from it (`settingsFilePathFor` in
  `src/AppPaths.h` returns empty for registry names).
- **A relaunch under systemd must not start a child.** The flagship runs as
  `forkmesh-source.service`; a child it starts lives in the unit's cgroup, and
  `closeEvent` stops the unit, so the child dies with it (2026-09-24 10:51).
  Relaunch paths call `requestSupervisingUserServiceRestart(path)` first: under
  a user service it marks a restart, and `stopSupervisingUserServiceAfterWindowClose`
  then runs `systemctl --user restart --no-block <unit>` instead of `stop`.
  See [../tools/source-publisher.md](../tools/source-publisher.md#flagship-guard).
- **Qt 6.4 is the floor.** Guard newer Qt APIs with `QT_VERSION_CHECK`;
  `tools/qt64-build-check.sh` builds against Ubuntu 24.04 / Debian 12 Qt.
- **OpenSSL 3.0 builds; 3.5 is needed at run time** for ML-KEM-768 (private
  mirrors, owner envelopes, Password Vault) and Argon2id (vault). On 3.0 those
  fail closed with a message; `-DFORKMESH_BUNDLED_OPENSSL=ON` builds a pinned
  3.5. See [build-and-test.md](build-and-test.md#openssl).
- **A leftover `.git/rebase-apply` blocks every later `git am`.** A killed or
  timed-out `git am`, or a conflict session the desktop was closed during,
  leaves the directory behind, and the next PR merge failed with "previous
  rebase directory .git/rebase-apply still exists but mbox given" (error group
  521). `PullStore` keeps a process-wide set of work trees with a live conflict
  session (registered when `beginPullBranch` leaves conflicts, cleared by
  `abortConflictMerge` and `finalizeOnPullBranch`). `mergePull` and
  `beginPullBranch` treat any other `rebase-apply` as stale and clear it first:
  `git am --abort`, or `git am --quit` when the tree has tracked edits and no
  unmerged paths so the user's work stays, then removal of the directory if git
  cannot. A `MERGE_HEAD` equal to the PR head being merged (an interrupted
  earlier attempt at the same merge) is aborted the same way.
  `conflictMergeInProgress` reports only live sessions, so a stale directory no
  longer stalls the merge queue.
- **Every item view needs a painted selection band.** The theme's
  `QAbstractItemView::item { border: 0px; }` (it keeps text from jumping when
  a row is selected) hands item painting to the style sheet, which then paints
  no selection background for a list or tree without its own
  `::item:selected` rule, while selected text still switches to the highlight
  text colour: white on white in the light theme, near-black on black in the
  dark one. `QAbstractItemView::item:selected` right after it restores the
  palette highlight. A view-specific `::item:selected` rule with a light or
  transparent background must set `color` too, and a delegate that drops the
  band (`SelectionBorderRowDelegate`) must keep its normal text colours.

- **A branch to merge may not be local.** Merge, Merge & clean up, the
  agent auto-merge, `agentops.merge` and the Merge editor all name the branch,
  and `git merge <name>` only resolves local refs. A branch that another node
  pushed to the mirror, or the head of a patch-based pull (only
  `.forkmesh/pulls/<n>/changes.patch` and `commits.mbox`, no branch), failed
  with "merge: ui/landing-pricing-downloads - not something we can merge"
  (task 573). The merge worker (`prepareWorktreeMerge`) first looks for
  local and remote-tracking refs (`gitrefusal::mergeSourceFromRefs`) and makes
  a tracking branch from a single remote copy. When there is no ref at all, it
  calls `ensureBranchForMerge`, as the Merge editor's worker does, which uses
  `PullStore::ensureLocalBranch`: `refs/heads/<name>` is fetched from the
  repository's mirror; otherwise the newest open pull whose head is that name
  is replayed from its mbox (`materializePullRef`) and the branch is created
  at `refs/pr/<n>/head`, so `reconcileMergedPulls` marks that pull merged
  afterwards. A branch found nowhere, a pull that is already merged or closed,
  or a patch that no longer applies stops the merge with a notice that says
  which, before the checkout is switched. Only a missing ref is created; an
  existing branch is never moved.
- **One `commitResolvedMerge`.** The Merge editor, the worktree conflict
  editor and the AI conflict fix all conclude a merge through
  `commitResolvedMerge` (`MainWindowInternal4.cpp`), so all three quote
  `gitFailureDetail` instead of "Could not commit the merge." (see
  [diagnostics.md](diagnostics.md)).

- **`forkmesh-mesh/*` is not a remote.** A source checkout's mesh converge
  fetches each mirror by URL into `refs/remotes/forkmesh-mesh/*`
  (`UpstreamCheckoutSync.cpp`), so those rows show in Branches like remote
  branches, but `git push forkmesh-mesh --delete` has nothing to push to.
  `deleteRemoteBranch` checks `git remote` first and hands unconfigured
  remotes to `deleteMeshTrackingBranch`: it drops the tracking ref and runs a
  forced converge, whose complete read ends in the pruning fan-out that removes
  the branch from the mirrors. A mesh copy whose local branch still exists is
  refused, because the next fan-out would push it straight back. The real
  `push --delete` runs with stderr merged so git's reason reaches the notice.
- **A deleted branch leaves its tip in `refs/forkmesh/trash/deleted/<b>`.**
  The mesh converge (`UpstreamCheckoutSync.cpp`) re-creates a local branch
  from any mirror copy that is not in main, which is how mirror-side work
  reaches the source. A branch deleted here would come back the same way, so
  `deleteBranchAndCopies` (`MainWindowBranches.cpp`) and the
  `forkmesh-mesh/<b>` delete write that ref first, and the converge skips a
  mirror copy contained in it. A mirror that gained commits after the delete
  still comes back. `propagateBranchDeletion` runs a forced converge for the
  source repo, since only a complete converge pushes with `--prune`; a plain
  `propagateRepoUpdate` never removed anything from the mirrors.
  The other direction (2026-10-07): a branch the upstream deleted through the
  executor is deleted here too. Every refresh also fetches the upstream's
  `refs/forkmesh/trash/branch/*` and `refs/forkmesh/trash/agent-branch/*`
  (`<branch>/<16-hex token>`, `branchTrashPrefixFor` in
  `server/merge_branch.go`) into `refs/forkmesh/upstream-trash/<remote>/`. A
  local branch goes only when the upstream no longer has it, no worktree has
  it checked out, it is not `forkmesh/*`, the local tombstone does not already
  hold its tip (that is a restore), and its tip equals one of those trash tips,
  or is inside one and was either on the upstream before this fetch or has
  commits outside the upstream's main. The tip is written to
  `refs/forkmesh/trash/deleted/<b>` first, so the publisher's tombstone pass
  removes a copy it re-pushed to one core and the converge never re-creates
  it. A never-pushed branch is never inside a trash tip unless it carries no
  work of its own, and then it must sit at exactly that tip.
  **The served cores hide `refs/forkmesh/`** (`uploadpack.hideRefs`,
  `tools/mirror_gateway.py`), and a branch simply missing from one read is
  not a signal: `app.forkmesh.com` answers from n1, n2 or n3 in turn, pushes
  land on one core and the others catch up every 15 s, and the publisher
  re-pushes a deleted branch the checkout still has to one core. So since
  2026-10-07 the source converge asks the app instead: before reading the
  routes it posts a signed `POST /api/git/branch-trash` (the account key,
  `forkmesh-branch-trash-v1`; [../CORE_PUSH.md](../CORE_PUSH.md#listing-the-branches-the-cores-deleted-2026-10-07))
  and hands the listed `branch -> tips` to the route whose host is the
  catalog's (`MirrorRoute::deletedBranches`, `fetchServedBranchTrash` in
  `MainWindowRepoSync.cpp`), where they join the fetched trash under the
  same conditions. Nothing is written under `upstream-trash/` for them: a
  listed tip may be a commit this checkout never had, and then only an exact
  tip match counts. A listing that fails, is not `200`, names another
  repository, has no answering core or has one malformed entry gives an
  empty map (`parseServedBranchTrash`), so the pass deletes nothing; a desktop
  without owner signing never asks.
  A `forkmesh/pull/<n>` branch is force-tracked on a re-pointed origin, but
  since 2026-10-07 a local absence plus a tombstone
  `refs/forkmesh/trash/deleted/forkmesh/pull/<n>` that contains the upstream
  tip keeps it deleted; an upstream tip past the tombstone still comes back.

- **A stop signal reaches the Actions runner's children.** Rebuild & restart
  restarts the systemd user unit, and systemd sends `SIGTERM` to every
  process in the unit, so the runner's `git worktree add`, workspace clone and
  step processes die with the app. The app itself survives the signal while a
  run is active (`CrashHandler.cpp`) and used to read those deaths as run
  failures: on 2026-09-28 01:16:49 runs #620 (Deploy nodes, "Could not create
  the isolated workspace"), #622, #623 and #624 (the Native iOS build on the
  Mac, "Checkout failed", never reaching the Mac) all failed in the second
  before the restart, and the pushes that arrived in that second logged
  "could not bind ... could not archive the repository snapshot" because their
  `git archive` was killed too. Since then a phase process that ends
  unsuccessfully after a stop signal is an interruption
  (`ActionRunner::interruptForShutdown`): the run stays Running on disk, logs
  "INTERRUPTED", emits `interrupted` instead of `finished` (no "Action failed"
  ping), and `MainWindow::onRunInterrupted` re-queues it, starting the queue
  again after 60 s if the app is still alive. A restart re-queues it from
  `restoreActionRuns` as before. A push whose snapshot could not be bound
  after a stop signal is written back to the spool
  (`ActionStore::spoolPushEvent`, the post-receive hook's format) and queued
  on the next start; a pull request scan cannot be spooled and says so.
- **A checkout that loses a race on the mirror's worktree list is retried.**
  Every run checks out with `git worktree add` into the same bare mirror, and
  a push to `main` starts several workflows at once while finished runs, the
  pull and repository pages and `git gc --auto` run `git worktree prune` on
  it. Prune deletes an admin folder that has no `gitdir` yet, which is the
  state between `worktree add`'s `mkdir` and its `locked` file. On 2026-10-04
  run #229 failed with "could not open 'worktrees/source1/locked' for
  writing: No such file or directory" and on 2026-10-07 run #1385 with
  "failed to read worktrees/source/commondir: Success", both "Checkout
  failed." with nothing wrong in the commit. When the checkout output names
  `worktrees/` with "failed to read", "could not open" or "No such file or
  directory" (`ActionRunner::checkoutLostWorktreeRace`), the run logs a retry
  and checks out again after 200 ms per earlier attempt into a fresh
  `source-<n>` folder of the same sandbox (the failed path may still be
  registered), at most three attempts in all. A missing commit or an occupied
  destination still fails at once.
- **`on.push.paths` is matched over the whole pushed range.** The scan used
  to diff only the tip against its first parent. On 2026-09-28 `main` moved
  from `21fe700db0` to `65d76176dc` ("Merge main into integrate/world3") in
  one push; the range changed `world/public/world/*.js`, but the merge's
  first-parent diff held only the desktop files that came from `main`, so
  "Deploy nodes" logged "did not match the workflow's on.push.paths filter"
  and the World release never deployed. A push of several linear commits
  counted only the last one's files the same way. Every push source now
  carries the ref's old value to `queueWorkflowsForCommit` (the post-receive
  spool's `ref <old> <new> <ref>` line, the served-mirror head watcher's
  stored head, the external-source watcher's stored commit, a mirror sync's
  previous head, the runner-ref publish's previous value, and the shutdown
  spool, which now writes the old value instead of zeros).
  `forkmesh::actions::pushRangeChangedPaths` (`src/ActionPushRange.cpp`)
  runs `git diff --name-only <old> <new>` when the old value is a real
  object that is an ancestor of the new one; for a new branch (unknown or
  all-zero old value) or a force push it falls back to the union of the tip's
  diffs against every parent, and `--root` for a root commit. The
  metadata-only skip (`isMetadataOnlyChange`) reads the same list, and the
  "did not match" line now names the range. Tests: the "push range" block in
  `forkmesh-tests` and the spool round trip in
  `forkmesh-action-security-tests`.

## Recent changes

- 2026-10-13: the bundled `NotoColorEmoji.ttf` (4.5 MB in every
  binary and a startup step) is gone; the desktop uses the system colour
  emoji font. macOS always has Apple Color Emoji and Windows 10+ Segoe UI
  Emoji (no flags, as before: Qt drew Segoe there). Linux desktops mostly
  ship Noto Color Emoji; for the rest `www/public/install.sh` installs the
  distribution package (`fonts-noto-color-emoji`,
  `google-noto-color-emoji-fonts`, `noto-fonts-emoji`,
  `noto-coloremoji-fonts`, `font-noto-emoji`) on both the prebuilt and the
  source path, and only warns when it cannot (no package manager, no sudo,
  `FORKMESH_NO_INSTALL_DEPS=1`). Startup logs a warning when none of Noto
  Color Emoji, Apple Color Emoji or Segoe UI Emoji is installed; emoji then
  draw in monochrome from whatever fallback fontconfig finds. Building from
  source by hand on Linux: install the same package.
- 2026-10-10: Core features are Market packages, off in new projects. The
  user asked for the Home grid's features to be "full fledged plugins and
  turned off by default on new projects on desktop, mobile and web, nest any
  that have dependencies"; the package list, the `install.installedBefore`
  field and the rule every host applies are in
  [../marketplace/core-plugins.md](../marketplace/core-plugins.md).
  - *Registry.* `PackageManifest::installedBefore` is parsed from
    `install.installedBefore` (a `yyyy-MM-dd` date; anything else quarantines
    the manifest). With no `market/packages/<id>/state` key a package counts
    as installed when it is external, when it is bundled and
    `install.default` is true, or when it is bundled and this project was
    created before its `installedBefore` date
    (`PackageRegistry::installedWithoutChoice`). The project's creation time
    comes from `project.json` `created` in `forkmesh::paths::layout()`
    (`readProjectCreated`); the home project, a project without `created`
    and a process that never activated the layout (tests, helpers) count as
    old. The account answer still wins: `applyAccountPackages` writes the
    state key. An entry marked `legacy` (the server said installed only
    because the account predates `installedBefore`, or, since 2026-10-13,
    because the account set up Vault, Calories, Steps, Water, Sleep, Sugar
    Break or Stretch through the package's own panel) counts as installed
    only when this project was created before the package's
    `installedBefore`, or before 2026-10-11 for a package without one, so a
    new project of an old account starts with every gated package off.
    *Places* (2026-10-13): the registry reads `install.places` (a bad list
    quarantines the manifest like a bad `installedBefore`) and keeps each
    account package's chosen places under `market/packages/<id>/places`,
    written by `applyAccountPackages`. `PackageRegistry::isShownHere` (installed,
    dependencies installed, and `desktop` among its places) decides
    `packageSurfaceShown`, `syncPackageRuntime`, `syncNativePackagePages`,
    `syncDesktopPackageExtensions`, the Discord check and the Installed
    packages search hits. Installed account packages get a "Show on" row of
    Phone / Desktop / Web checkboxes on their settings page and Market card
    (`/api/plugins/@account/<id>/places`); the last one is disabled. The
    Workflows rail link (`builtin:workflows`) follows the `workflows`
    package, which a new project's "Plugins to start with" list leaves
    unticked ([core-plugins.md](../marketplace/core-plugins.md#2026-10-13-where-each-plugin-shows-up)). Installing a parent brings back a nested child only when the
    child would be installed here without a choice.
    *Per project* (2026-10-13): in a project created on or after 2026-10-11
    (`PackageRegistry::projectOwnsInstalls`) the account answer no longer
    wins. `accountAnswerInstalls` keeps an entry installed only when the
    account has it and this project installed it, so the account can take a
    package away here but never brings one in; a `legacy` entry leaves the
    project's own state alone. A legacy project (and the home project)
    follows the account as before. Installing in a new project still posts
    the account install; removing there posts nothing, so other projects keep
    it, and the confirmation no longer says account records are deleted. The
    Market says "Installed in this project", "Install in this project",
    "Remove from this project" there
    ([core-plugins.md](../marketplace/core-plugins.md#2026-10-13-plugins-belong-to-the-project)).
    Tests: `test_package_registry.cpp`, "an account install does not turn a
    package on in a new project" and the seven checks around it.
  - *Surfaces.* `syncPackageSurfaces` runs after every `syncPackageRuntime`
    and after the lazily built repository page, each of its lazily built
    tabs (the Branches button lives on the Code tab), the Network page and
    the Settings page are made. It shows or hides: rail Agents (`agents`), Code (`code`),
    Git (`commits`), Chat (`chat`), Notes, Calendar, Tasks (`notes`,
    `calendar`, `tasks`), Pings (`inbox`), Network (`network` or `hosts`),
    Specials and the right-hand Special Agents rail (`special-agents`),
    Stats (`stats`); the repository Issues, Pulls, Discussions, Actions and
    Shortcuts tabs and the Releases and Branches buttons (`issues`,
    `pull-requests`, `discussions`, `actions`, `shortcuts`, `releases`,
    `branches`); the Network page's Nodes and Hosts tabs (`network`,
    `hosts`); the Settings Organizations section (`organizations`, through
    the new `SettingsSectionsView::setSectionHidden`). Ledger, Tour, Timers,
    Capture and Status Page keep their own hooks. Users stays core: it is the
    administrators' World profile list, not organizations. The Home grid and
    the command palette already skip hidden buttons; global search drops
    issue, milestone, pull, discussion, agent, action, commit, branch and code
    hits of a hidden package, and the Notes, Tasks and Calendar hits follow
    the same check. A gated package missing from the catalog counts as
    installed only for an old project.
  - *Leaving a page.* When a package goes from shown to hidden while its
    page is open, the window moves to the Market (where it can be put back);
    a hidden repository tab falls back to Code. `showSection` itself is not
    gated, so direct routes keep working.
  - *New project dialog.* The list nests each package under its first
    dependency, ticking a nested package also installs its parent, and every
    unticked package that is on by default or has `installedBefore` is
    seeded `removed`, so a project created before the cutoff date still
    starts with only what was ticked.
  - *Synced packages with data on this device.* Timers is now account
    synced but still keeps its records on this device. Its Install and
    Remove go through the local-data dialogs (import a saved copy, "Save a
    copy and remove") instead of the account-sync confirmations, which would
    otherwise drop the data without asking. Dependencies still use the
    account-sync install dialog.
  - The desktop has no Report a bug, World or Desktops entry points, so
    `bug-reports`, `world`, `desktops`, `coves`, `on-device-ai`, `activity`
    and `sign-in` change nothing here beyond their Market listing.
  - Tests: installedBefore parsing and the old/new project fallback in
    `forkmesh-package-tests`, hidden sections in
    `forkmesh-settings-sections-tests`, and "removing Chat hides its rail
    button…" with its neighbours in `test_window_resize.cpp`.
- 2026-10-10: Home looks and works like an iPhone home screen (issue 1889).
  The user asked for bigger, clearer icons, for right-click to arrange them
  into pages, and for the search bar to stay hidden until the page is pulled
  down.
  - *Icons.* Each tile is 132 × 132 px with a 64 px rounded "app icon" plate
    in its group's colour (Work blue, People green, Network teal, Insights
    purple, Packages orange, Tools grey, Shortcuts indigo, More slate; amber
    when the rail button is tinted) and a 36 px white octicon on it. The
    caption is one step larger than before; badges and pending-push pills sit
    on the plate's corners with a ring in the window colour; a live page
    pulses a ring around the plate. The grid is centred, and the group
    headers are gone: grouping is the plate colour.
  - *Pages.* `HomeLayout` (pure Qt Core) holds the pages as lists of rail
    keys, saved as JSON in `ui/homePages`; no setting means one page in the
    rail's group order. `resolve` drops keys no longer in the rail and puts
    new ones (a new package, a shortcut) at the end of the last page. A page
    emptied by a move disappears, like on the iPhone; Add Page keeps an empty
    one until it is filled or removed.
  - *Arranging.* Right-click an icon: Open, Move to Page (each page, New
    Page), Move Left, Move Right. Right-click anywhere: Add Page, Remove Page
    N (its icons go to the page before, or the next one for page 1), Reset
    Home Screen. Dragging an icon onto another slot of the same page
    reorders it, and the other icons make room while it is dragged.
  - *Turning pages.* Dots under the board (`HomePageDots`, click to jump), a
    sideways touchpad swipe or Shift+wheel (one page per 400 ms), and the
    Left/Right or Page Up/Down keys.
  - *Search.* Hidden until asked for. A wheel or touchpad pull upward while
    the board is already at the top (momentum ignored, one wheel notch is
    enough), dragging the empty space down 56 px, or typing on the board
    slides it open and focuses it (typed letters go into it). It searches
    every page, hides the dots while it has text, and closes on Esc or when
    it loses focus empty. Ctrl+F still belongs to MainWindow's find-in-page.
  - Tests: `forkmesh-home-board-tests` (layout model, menu actions, drag,
    swipe, pull, typing, dots) and `checkHomeGridPage` in the window suite.
- 2026-10-10: Reading a chat on another device clears its unread badge here,
  and reading it here clears it there (issue 1876). The desktop posts a signed
  read mark (`forkmesh-chat-reads-mark-v1`) when you view a conversation.
  It fetches marks on the `chat-read` account event and after every DM-list
  read. See [../app/chat-read-sync.md](../app/chat-read-sync.md).
- 2026-10-09: No toasts (issue 1867). The user asked for no toasts anywhere
  and for information to be shown inline. The floating card stack over the
  prompt is gone: a message is shown in the page it was raised on, in an
  inline notice strip above the page (Open, View output, Copy and clear; no
  countdown), or in the open dialog's or panel's own status line; unsolicited
  pings (chat, agent done, logged errors, usage resets) are only filed on the
  Pings page with its rail badge. Details in
  [diagnostics.md](diagnostics.md#inline-notices-instead-of-toasts-issue-1867-2026-10-09).
- 2026-10-08: Direct messages: the desktop re-lists DMs on the
  `direct-messages` account event and keeps a socket to every DM with unread
  messages and every DM the user opens, not only the 8 most recent. In a DM
  room `ServerNode` no longer acknowledges key advertisements. It asks for a
  re-seal when it cannot open a message, and re-seals its own undelivered
  messages when it learns a new device key
  ([app/chat-private-rooms.md](../app/chat-private-rooms.md#undeliverable-copies-and-re-sealing-2026-10-08)).
- 2026-10-08: Office channels sit in the root of the Chat sidebar under
  their room name. They used to show their conversation key, so a channel
  read `#office/email-log` and an organization room, named
  `office/<org>/<room>` on the server, read `#office/office/forkmesh/coffee`.
  `officeChannelLabel` (`MainWindowNavigation.cpp`) shows `#email-log` and
  `#coffee` in the sidebar, the heading and the composer placeholder; when
  an organization room has the same name as another channel (the forkmesh
  `general` next to World `#general`) it reads `#general · forkmesh`. Only
  the label changed: the `#office/...` key, saved history and the server
  name stay as they were.
- 2026-10-08: **Project settings** is a page of the main window, not a popup
  dialog: the user asked for it to be its own page. The gear beside a project
  in the picker, the "project settings" link in the destructive-command
  prompt and **Project settings…** in the command permissions dialog all call
  `MainWindow::showProjectSettings(slug)`, which loads that project into the
  page and shows section `kProjectSettingsSectionIndex` (28, built on first
  use like the other deferred sections, `src/MainWindowProjectPicker.cpp`).
  The page keeps everything the dialog had: the logo, folder path and **Open
  folder**; name and description; **Follow the global agent rules**,
  **Global agent rules…** and the agent access editor; the hint and the error
  line. **Save** writes the project, refreshes the command approvals and the
  project switcher, shows "Saved X's settings." and stays on the page with
  the saved values, like Settings. **Cancel** and the header's **Back** drop
  unsaved edits and return to the page that was open before, skipping
  Project settings entries of other projects in the back/forward trail
  (`leaveProjectSettingsPage()`). The trail records the project's slug, so
  Back and Forward reopen the right project. Opening the page for another
  project while it is open loads that project. **Remove project…** keeps its
  confirmation box and outcomes and leaves the page once the project is
  unlisted. Test: `forkmesh-project-settings-window-tests`
  (`forkmesh-window-tests --project-settings-only`), run in a temporary
  projects root.
- 2026-10-07: An agent using a skill or shortcut adds a run to its Shortcuts
  & Skills tally, not only a launch from that page. Three ways count, each
  at most once per agent session for each entry, so retries and re-reads do
  not inflate the tally:
  - A special agent run (single, continued or shared) counts its linked
    shortcut and attached skills (`MainWindow::recordTeammateShortcutUses`).
  - A tool call names the file: a Bash command or Read path with
    `.forkmesh/shortcuts/<file>`, `.forkmesh/skills/<file>`, or the app or
    organization skills folder under the data root.
  - A `Skill` tool call names a skill whose file exists in one of the three
    scopes (`MainWindow::recordAgentToolShortcutUses`, called for each
    `tool_use` in `applyTranscriptEvent`).

  Edits do not count. Files that do not exist in the session's repository
  checkout do not count either, and sessions from outside the desktop
  (external ids) are skipped. `skillsDirPath` now takes the repository index,
  because an agent's repository is not always the one open on screen.
- 2026-10-07: Settings is one column, its section list shows each section's
  parts, and the first open is faster. Every section's cards stack in a single
  column at most 920 px wide, centred (`settingsFlowBody()` and the Apps lists
  use one column; they used to flow into two or three). The section being read
  opens in the list on the left to its cards (a card's `sectionLabel` heading,
  in sentence case; hidden cards and cards nested inside another card's list,
  such as each suggested app, are left out, and a section with fewer than two
  has none), the card being read is lit as the page scrolls, and a click on one
  scrolls to it. The other sections stay folded in the list. The rows are
  drawn by a delegate in `SettingsSectionsView.cpp`, so `settingsNav` keeps
  one row per section and the existing row-based callers and tests are
  unchanged. Section heights now follow word wrap in Classic too (it was
  Next-only), since one wide column wraps differently from the preferred
  width. An open section that is built on demand (Apps, Cloud and the rest,
  once opened) is no longer built when Settings opens but when the page comes
  within two screens of it, or on a jump, a search or a reveal that needs it.
  The first open was slow mostly because the finished page was re-parented
  four times (the section list into the page, the page into its scroll area,
  the scroll area into the window, and each body twice), and every re-parent
  under the application style sheet re-resolves style, font and palette for
  all ~920 widgets. The scroll area now goes into the window first and the
  page, the section list and each section frame are attached before they are
  filled, so each widget is parented once. Measured in a sandboxed offscreen
  run (fresh profile, no network, 1440 by 900), the click on Settings to the
  first paint went from a median of ~700 ms to ~510 ms (five runs each).
  Test: `forkmesh-settings-sections-tests`.
- 2026-10-07: Settings opens faster and folds its rarely used sections.
  Quick Setup, Apps, SSH Keys, Cloud, Organizations, MCP, Phone, Background
  timers, Security and Data are built only when first opened, and start
  folded with Secrets; Profile, General, Repos, Pings, Agents and Voice start
  open. A folded Profile no longer moves the node profile panel in on every
  Settings open. Measured offscreen in the window-test binary, the first
  Settings open went from ~1.2 s to ~0.57 s to build (~1.35 s to ~0.64 s to
  first paint); the first search then builds the folded sections (~0.25 s).
  Removed: the General > PR BOUNTIES card (every control was disabled since
  the Worker-held escrow was retired; `autoBountyForMergedPull` still resets
  the old keys) and Quick Setup's WORLD & APPEARANCE group
  (`WORLD_THEME`, `WORLD_ACCENT_COLOR` and `WORLD_NAME` were saved but nothing
  read them; the app theme stays in General > Appearance).
- 2026-10-07: The main window has rounded corners on Wayland. It is
  frameless (`Qt::FramelessWindowHint`), so no compositor rounds it, and its
  corners were square. `WindowFade` (`desktop/src/WindowFade.h`), already the
  topmost child of the translucent Wayland window, now stays shown at full
  opacity with a mask of the four 10 px corner squares and paints
  `CompositionMode_DestinationOut` outside an antialiased rounded rect, the
  same 10 px radius as `paintEdgeGlow`'s borders. Maximized and full-screen
  windows keep square corners (it follows `WindowStateChange`). Below 100 %
  opacity it covers the whole window again and does both. X11, macOS and
  Windows have no alpha surface on the main window and are unchanged. Test:
  `forkmesh-window-fade-tests`.
- 2026-10-07: Chat rows render Slack-style formatting (`ChatFormat.h`)
  instead of removing the markers, the composer takes Ctrl+B, Ctrl+I,
  Ctrl+Shift+X, Ctrl+Shift+C and Ctrl+Shift+U, and chat notifications show
  clean formatted text: no markers, link labels, `•` list items, at most four
  lines, a notice card summarised from its fields, and an escaped body on Linux
  servers that read markup ([../app/chat-formatting.md](../app/chat-formatting.md)).
- 2026-10-07: The Branches page no longer calls a branch "merged" just
  because it has no commits ahead of the default branch. A new agent branch
  that has not committed yet also has nothing ahead, so every fresh worktree
  showed "merged". The page reads each local branch's reflog
  (`<common dir>/logs/refs/heads/<b>`, `readBranchRefHistory`): if the first
  entry is the branch's creation and no later entry is work of its own (a
  commit, merge, cherry-pick, rebase, revert, `am` or pull; fast-forwards,
  `branch:` and `reset:` entries do not count), the status reads "new".
  "Delete merged" skips those branches. A branch with no reflog, or one whose
  creation entry has expired, keeps the old 0-ahead rule. The table gained a
  Created column (reflog creation time and the starting point). Updated is now
  the later of the tip commit time and the last ref move. Both show the clock
  time beside the age (`12:49 AM · 2m`), and the full date is in the tooltip.
- 2026-10-07: The Branches page lists a remote-tracking branch only when it
  says something the local rows do not. One branch showed up to three times
  (`origin/main`, `forkmesh-cores/main`, `forkmesh-mesh/main` under `main`).
  `forkmesh-cores/*` is the source publisher's working copy of the cores'
  `main` (`docs/tools/source-publisher.md`), not a remote anyone uses, so it is
  never listed; and `<remote>/<b>` is hidden while a local `<b>` exists at the
  same commit or contains it (`loadBranchesAndTags`, `MainWindowBranches.cpp`).
  A remote copy that is ahead of the local branch, or one with no local
  branch (an orphan to delete), still shows.
- 2026-10-07: A branch deleted upstream is deleted on the desktop too, and a
  deleted pull record branch stays deleted. Before this the converge kept a
  local branch whose tip was not in main forever ("local-only commits"), and
  the publisher pushed it back to the cores, so a branch deleted on the web
  never went away; and `forkmesh/pull/<n>`, force-tracked on a re-pointed
  origin, skipped the tombstone check, so a pull the desktop deleted came back
  from the cores. The rule is in [Traps](#traps), "A deleted branch leaves
  its tip". The served cores hide their trash refs, so the converge reads
  the owner-only `POST /api/git/branch-trash` listing for the route whose
  host is the catalog's; that is what makes the rule act on
  forkmesh/forkmesh. Test: `desktop/tests/test_upstream_checkout_sync.cpp`
  (trash-core, pull-upstream and served-core blocks).
- 2026-10-06: **Connect Discord** moved from the Chat sidebar, where it sat
  between the channel list and DIRECT MESSAGES and read as a stray list entry,
  to a DISCORD card in Settings > General under Chat
  (`m_discordSettingsCard`, built in `buildSettingsGeneralTab`). The whole card
  is hidden while the Discord package is not installed, and
  `attachDiscordPackage`/`detachDiscordPackage` show and hide it with the
  button. Settings is built on first open, so the card seeds its label from
  the live client. A Discord channel's **Discord connection settings…** menu
  item in Chat is unchanged. From `agent/adhoc-2020-…` (2026-09-09).
- 2026-10-06: Every file header in a diff carries the "⋯" File actions
  pill, not only files with a copy on disk. Binary files, files a branch
  deleted, branch compares whose branch is not checked out here, and
  patch-only diffs (pulls, actions) used to show no pill. A file with no copy
  on disk keeps Edit, Open with default app and Show in folder in the menu,
  disabled with a tooltip; Copy path always works. Each pill adds a cached
  inline PNG, so the streamer's first batch (`kDiffStreamBatchChars`) fits a
  little less on those panes.
  - *Delete from branch.* `DiffFileEntry` now records the diff's repository
    (`repoDir`) and compared branch (`branch`, empty on a working-tree diff,
    which means the branch checked out in `repoDir`). The `filemenu:` anchor
    carries them as `r=` (only when it differs from `d=`) and `b=`. When the
    menu opens, `diffBranchDeleteRequestFor` checks that
    `refs/heads/<branch>:<path>` exists (so commit, tag and remote compares
    get no entry) and finds the worktree holding the branch. "Delete from
    <branch> and commit…" then runs `git rm -f` and a one-path
    `git commit -- <path>` in that worktree (`runScmDeletePathFromBranch`),
    the same way "Revert to base and commit" does, behind the same staged-
    commit ratchet. Anything else staged stays uncommitted. If no worktree
    holds the branch, the entry is disabled and its tooltip says why. Before
    this, a branch compare could not delete a file at all, and a
    working-tree delete only left an uncommitted deletion in the worktree.
  - "Delete file…" (working tree only, uncommitted) is unchanged.
  - *Fold at a batch join.* The streamer appends each batch with its own
    `insertHtml` and always splits at a file start, so the file opening a
    batch has two empty blocks above its header instead of one. Folding
    that file in place (`swapDiffFileViewedState`) replaced only the block
    next to the header, which left the other one holding a 14 px line, and
    the folded header landed 14 px below where the fold aimed. The extra
    pills moved a batch join onto the fold check's fixture and exposed it.
    `diffFileBlockRange` now starts the range at the first of a run of
    empty blocks before the header.
  - Window test: the File actions check and the bottom-fold checks in
    `--diff-stream-only`.
- 2026-10-06: Deleting an agent lowers the Branches count. The delete already
  removed the agent's branch with every copy (local, `origin/<b>`,
  `forkmesh-mesh/<b>`, tombstoned under `refs/forkmesh/trash/deleted/`), but
  the agent paths (`deleteWorktreeBranchAndAgentInBackground`,
  `removeWorktree` and the no-worktree branch of
  `deleteWorktreeBranchAndAgent`, `MainWindowBranches.cpp`) only reloaded the
  Branches table. The rail badge and the "Branches (N)" tab come from
  `loadBranchesAndTags`, which only the branch-row delete called, so the count
  stayed put until something else re-read the refs. Each path now calls it
  once the branch is gone. Window test: `--agent-delete-branch-count-only`.
  A check of the forkmesh/forkmesh sessions that day: every desktop agent in
  the list named a branch that existed, apart from two external Claude Code
  sessions (one with no branch, one whose branch was merged and deleted).
  The ~317 local branches with no session or recycle-bin record behind them
  have no agent to delete them; deleting an agent never touches them.
- 2026-10-06: Tooltips. Every tooltip in the app blinked, ran on one long line
  and on some pages had no background. All three came from Qt's own
  `QTipLabel` (Qt 6.8):
  - Background: Qt parents the tip to the hovered widget and sets it as the
    tip's style sheet parent, so a parent rule such as
    `QLabel { background: transparent; }` or `QWidget { background: transparent; }`
    made the tip see-through.
  - One line: Qt wraps only rich text; plain text wraps only once it is wider
    than the whole screen.
  - Blink: the tip filters every event in the application and hides on a
    Leave, FocusIn, FocusOut or WindowActivate sent to *any* object. Moving
    across a child label inside a card, or a live refresh moving focus, hid
    it; the next mouse move re-created it at once, and a re-show with the same
    text did not cancel the pending hide.

  `Tooltips::Presenter` (an application event filter) now keeps Qt's tip as
  the source of text and timing only: it marks the tip
  `WA_DontShowOnScreen`, removes the tip's application-wide filter, and
  forwards to it only the events that should end a tip: mouse moves (for a
  `showText` rect), clicks, wheel, a deactivation of the hovered widget's
  window, and a Leave once the pointer is really outside the hovered widget.
  A key press other than a modifier also hides it. `Tooltips::Window` draws
  the text: an opaque `ToolTipBase` fill with a 1 px border, a 13 px UI font,
  and wrapping at 440 px (breaking long paths anywhere). It sits beside the
  pointer through `placeBeside()`, ignores input and never takes focus. It
  stays put while the same text stays up for the same widget. When Qt drops
  and re-creates the tip within 150 ms, the window is kept, so a re-show does
  not blink. Qt's tooltip fade and roll effects are turned off, since they
  would show a grab of the hidden label. `desktop/tests/test_tooltips.cpp`
  covers each case.
- 2026-10-04: The amber restart border and the red error / green celebration
  borders now follow the window when it is resized mid-restart. The amber one
  blinks by hiding and showing its overlay, and `MainWindow::resizeEvent` only
  resized overlays that were visible, so a resize during an "off" blink left it
  at the old size. Every existing border overlay is now resized, and the blink
  re-applies `rect()` each time it shows. All three borders share
  `paintEdgeGlow` in `MainWindowActions.cpp` and have 10 px rounded corners to
  match the main window's rounded corners (see the 2026-10-07 entry on
  `WindowFade`).
  `test_window_resize` checks the rounded corner and a resize made while the
  blink is off.
- 2026-10-02: The rail has a **Home** button at the top (octicon `home`,
  rail key `home`, section `kHomeGridSectionIndex` = 26, built on first open
  like every deferred section). The user asked for a home screen like the
  iPhone's ([../native/NAVIGATION.md](../native/NAVIGATION.md) §2.0.7):
  every icon in one place, grouped, with stats and activity on each.
  (The headers, the always-visible search box and click-on-press were
  replaced on 2026-10-10 by pages, a pull-down search and click-on-release;
  see that entry.)
  - *What it shows.* A greeting, a one-line headline (agents working,
    waiting, failed, unread pings and chat, or "All quiet"), a search box,
    and `HomeBoard`: one tile per page in the rail, under the headers Work,
    People, Network, Insights, Packages, Tools, Shortcuts and More
    (`homeGroupSpecs`; a key the table does not name goes to More,
    `package:*` to Packages and `shortcut:*` to Shortcuts). Within a group
    the tiles keep the rail's own order, folders flattened. Hidden rail
    buttons (Users for a non-admin, a removed Ledger) are left out.
  - *Where the numbers come from.* The tiles read the rail buttons
    themselves, so the counts can never disagree with the rail: badge,
    urgency, pending pushes, sync spinner, activity lamp and alert tint
    (`ActivityRailButton::iconName`, `label`, `syncing`, `alertTint` were added for
    this). `homeTileDetail` writes the line under each caption: agents
    failed, waiting, running or the session count; chat unread; pings new;
    git syncing, to push, changed or clean; tasks open; network online;
    memory in MB; otherwise the rail tooltip's text after its dash, up to
    the first punctuation.
  - *Activity.* A tile whose button spins or blinks, and Agents while any
    session runs, gets a green dot and a pulsing ring. The pulse timer runs
    only while a live tile is on screen and repaints only those tiles.
  - *Cost.* The page refreshes once a second while it is the current
    section of a visible window and stops on the first tick after it is
    left; it only reads in-memory button state, and `setGroups` skips the
    repaint when nothing changed.
  - *Clicks.* A tile clicks its rail button, so it does exactly what the
    rail does (Capture captures, Files opens at the repo root). Enter in
    the search box opens the first match.
  - Tests: `forkmesh-home-board-tests` (layout, grouping, filter, hit
    testing, animation gating) and `checkHomePage` in the window suite's
    `--stats-only` mode.
- 2026-10-02: A diff file header's pencil and trash pills are now one "⋯"
  pill (`diffFileMenuPillHtml`, octicon `kebab-horizontal`) that opens a
  menu: Edit file, Open with default app, Show in folder, Copy path, Copy
  full path, and Delete file on a working-tree diff. One pill takes less
  room than two on a narrow window and leaves space for more actions. The
  pill is drawn on the same files the pencil was (those with an `editDir`),
  so commit, pull and patch-only headers carry no extra inline PNG and the
  streamer's first-batch budget is unchanged. The
  `filemenu:<path>?d=<dir>&x=1` anchor (`x=1` = deletable)
  goes through `handleDiffFileActionLink` to `showDiffFileMenu`, which
  re-checks the path the way the old `edit:`/`delete:` anchors did (those
  still resolve). The sticky header on the Git page carries the same pill.
  Window tests: the File actions check in `--diff-stream-only` and the
  sticky header check in the full run.
- 2026-10-02: Chat makes no interface sounds: clicks, keys and sending in the
  Chat section are silent ([agents-ui.md](agents-ui.md#listen-announcer-and-interface-sounds)).
- 2026-10-01: Deleting an agent, a branch row or a branch's worktree deletes
  every copy of the branch: the local branch, `origin/<b>` (pushed as a
  delete) and the `forkmesh-mesh/<b>` tracking copy. "Delete merged" also
  takes the merged remote copies, not only local branches. Before this the
  served repo and the mirrors kept every deleted branch, so Branches filled up
  with `origin/…` and `forkmesh-mesh/…` rows for agents long gone, and an
  unmerged branch came straight back as a local branch on the next mesh sync
  ([Traps](#traps)).
- 2026-10-01: The version is back at the right end of the status bar, and the
  nav-rail Update button is gone; `maybeAutoUpdate()` again checks only when
  both auto-update settings are on and no agent runs. The move was meant for
  the iOS app ([../native/NAVIGATION.md](../native/NAVIGATION.md) §2.0.5).
- 2026-09-28: How long chat messages stay before they disappear is now a
  per-user preference that follows the account to the web, Android and iOS
  clients: a default for all chats (seven days unless the user picks 3 days,
  1 day, 8 hours or 1 hour in Settings > General > Chat) plus per-chat
  overrides from the "Messages disappear after" submenu on a channel's or
  direct message's right-click menu. Seven days is also the most, since the
  relay keeps nothing longer (`CHAT_HISTORY_RETAIN_MS`). The server keeps the
  preference in the account record (`POST /api/accounts/chat-retention`,
  docs/app/README.md); the sign-in answer carries it, else it is read once
  after sign-in, and each change is written back and applied locally at once.
  `src/ChatMessageRetention.h` holds the model and the last answer, cached
  in `chat/retentionPreferences` so an offline start still applies it. Chats
  are keyed the same way on every client: `#name` channels and `@peer` DMs by
  their conversation, office channels by `channel:<server channel id>`
  (resolved through `OfficeChannelMirror`). Loading saved history, the
  expiry sweep (now every five minutes, and at once after a change), dropped
  relay replays in `onMessage` and each message's expiry ring (whose tooltip
  now counts minutes in the last hour) all use the chat's own time. Going
  back to a longer time can bring recent messages back from a room's relay
  replay. `test_crypto.cpp` covers the model.
- 2026-09-29: Actions matches `on.push.paths` over the whole pushed range,
  not the tip's first-parent diff, so a merge or a multi-commit push that
  touches `world/` queues "Deploy nodes" ([Traps](#traps)).
- **One torn ref stopped every Action.** The disposable workspace is cloned
  from the run's detached worktree with `--no-local`, which asks for every
  ref the mirror advertises. An empty loose ref file (a ref write cut short)
  is advertised as `0000...`, and the clone then failed with "upload-pack: not
  our ref 0000..." ("Could not create the isolated workspace", run 331 on
  2026-09-27). The clone is `--single-branch`: it fetches only the detached
  `HEAD` and its history, which is all a run checks out; tags on that history
  still come along, so `release.yml`'s tag check works. The torn ref itself
  still needs repairing in the mirror.
- **A push to an old branch runs that branch's copy of a workflow.** Branches
  cut before `.forkmesh/deploy.yml` became manual-only (2026-09-24) still
  carry the push-triggered "Deploy Cloudflare Worker", and pushes to them
  queued it (2026-09-27 17:04, `3e740366`). The push scan also reads the same
  workflow from the mirror's default branch (`HEAD`): when that copy exists,
  parses, and has no `push` trigger, a push to any other branch skips it and
  logs why (`ActionFile::defaultBranchRetiresPush`). A branch that adds a new
  workflow, or changes one the default branch still runs on push, is
  unaffected; *Run workflow* still runs any of them.
- **Failure diagnostics carry the step command.** "Action failed before UI
  refresh" (network log, `qCritical`, crash context) printed the step's
  command after `${{ vars.* }}` substitution, so a credential in a failing
  step's `run:` text was written in clear. The command is masked with the same
  exposed-variable redaction as the run log.

## Recent changes

- 2026-10-09: Every shortcut shows how long its last run took, and a running
  one shows a progress bar estimated from that time. The user asked for both
  on all shortcuts. The table view has a **Took** column (the last measured
  run; the tooltip adds the average), next to the "took …" the cards already
  had. While a script shortcut or a dedicated-agent shortcut or skill runs,
  its card, its **Took** cell and the Output header show a bar
  (`shortcutRunProgress`) with "1m 05s of about 1m 30s · 25s left" beside or
  under it (`shortcutRunEta`); past the estimate it stays near full and reads
  "… longer than the last run", and with no timed run yet it is a busy bar
  reading "Running for …". A queued agent reads "Waiting to start". A run
  takes its estimate when it starts (`ShortcutRun::expectedMs`, the stats'
  `lastMs`); an agent's elapsed time is its `totalRunMs`. The phones show
  the same bar and "took …" (2026-10-10): local control run rows carry
  `expectedMs` and `elapsedMs`, and `insights.shortcuts` entries carry
  `lastMs`, `expectedMs` and `elapsedMs`, with a running dedicated agent
  counted as `running`
  ([../native/DESKTOP_PARITY.md](../native/DESKTOP_PARITY.md) 2.28). One 1 s timer (`tickShortcutProgress`) moves every bar
  and stops when nothing runs. A stopped run now rebuilds the cards too, so
  its bar goes away. Window test: "a running shortcut shows a progress bar
  estimated from its last run" in `test_window_resize.cpp`.
- 2026-10-09: Script shortcuts run at the same time. The user asked to run
  the Mac ad hoc and App Store shortcuts (and any others) together. Each run
  has its own process, output and id (`m_shortcutProcesses`,
  `m_shortcutRuns`, the last 20 kept); starting one no longer refuses with "A
  shortcut is already running". Running the same script again still restarts
  only that script, and a click within 3 s of its start is still ignored. The
  Output pane shows one run at a time: a picker (`shortcutRunPicker`, shown
  with two or more runs) chooses which, and Stop stops the shown run
  (`stopShortcutRun`). Local control: `insights.run` no longer answers
  `shortcut_running`; `insights.shortcutRun` takes an optional `id` and also
  returns `runs`, every kept run without its output. The phones keep a run
  per id ([../native/DESKTOP_PARITY.md](../native/DESKTOP_PARITY.md) 2.28).
  Window test: "two script shortcuts run at once" in `test_window_resize.cpp`.
- 2026-10-08: **Open with ForkMesh** in the file manager's right-click menu
  (Nautilus, Dolphin, Nemo, Windows Explorer). The user asked for it. A
  folder opens its repository, or goes through the Add repository import
  when ForkMesh does not know it yet
  ([Open with ForkMesh](#open-with-forkmesh)). Add repository's folder scan
  moved to `src/SystemOpenFolder.*`, tested by
  `forkmesh-system-open-folder-tests`.
- 2026-10-07: The launch splash is back, with the ForkMesh logo large. The
  operator asked for the logo "nice and big" on the loading screen and for
  everything to load in the background. `main.cpp` shows
  `forkmesh::ui::showStartupSplash` again, after the single-instance lock (so
  a second launch that only focuses the open window shows nothing). It had
  been off since 2026-08-30. `StartupSplash` now draws the logo at 112 px on a
  white disc, centred under the mesh orbit, with the title and the status line
  centred below it; the card grew from 452 to 620 px tall to fit. It still
  hands over on the main window's first paint, so sign-in, the saved
  repository, agents and package pages keep loading behind the open window
  (the channel rail: [agents-ui.md](agents-ui.md#channel-status-rail-2026-10-06)).
  `ui/startupSplash=false` or `FORKMESH_NO_SPLASH` turns it off.
- 2026-10-07: The Health page stopped blaming stalls on `MainWindow`'s
  app-wide event filter: return addresses it leaves on the stack put 78.7 s
  of other code's stalls on `MainWindowIssues.cpp`. The stall watchdog's
  likely origin skips the same frames
  ([diagnostics.md](diagnostics.md#reading-summed-stall-frames)).
- 2026-10-06: The instance, user and repository rows in the project dropdown
  each carry a **+** (add) and a pencil (edit) button, so they can be added
  and edited without opening the row's own menu. Instance: + is "Add relay"
  (`promptAddServer`), the pencil renames the active instance
  (`renameServer`); while a new project still has no instance both open the
  instance chooser. User: + signs in as another user (`loginToUserAccount`),
  the pencil opens Settings → Profile (`openAccountManagement`). Repository:
  + offers New repository… / Add local repo…, the pencil opens the open
  repository's Settings tab. Signed out, the user row shows the sign-in
  button's text (or "Not signed in" before startup auth resolves), and with no
  repository the row reads "No repository"; both then offer only +.
- 2026-10-06: Splitter dividers are a 1 px line (`QSplitter::handle` in
  `Theme.h`), so a pane's header rule runs into the divider with no gap. The
  grab zone is still 9 px: `SplitterHairline` (installed on the application
  in `main.cpp`) gives each 1 px handle 4 px contents margins and masks its
  paint to the line, the way Qt's own "tiny mode" does with 2 px. The handle
  overlaps 4 px of each neighbour, which QSplitter allows because handles are
  stacked above the panes. Hover and drag paint the line in
  `palette(highlight)`, the theme's highlight colour, so a marketplace theme
  recolours it.
- 2026-10-06: The top bar keeps only the project logo (32 px, was 25). The
  instance picker, the signed-in user's avatar and the active repository moved
  into the project dropdown, listed under the current project
  (`MainWindow::addProjectContextRows`). Each row copies the hidden top-bar
  button's icon, text, tooltip and enabled state, and clicking it closes the
  dropdown and clicks that button. The original buttons sit in a hidden
  holder in `buildBreadcrumb()`, so every existing update to them still runs.
  `topBarMenuAnchor()` opens their menus under the project logo while they
  are hidden. "Log in / Sign up" and the Approve and runner badges stay in the
  top bar.
- 2026-10-06: A shortcut's Stop is now Ctrl-C first. `MainWindow::stopShortcut`
  sends SIGINT, then SIGTERM after 1 s and SIGKILL after 4 s (Windows:
  terminate, then kill). A script can now tell a deliberate Stop (SIGINT) from
  the desktop going away (SIGTERM or SIGKILL when it quits or restarts, or its
  agent scope stopping). The Mac ad hoc shortcut relies on this: Stop ends its
  build, while a restart leaves the build running
  ([../native/MAC_BUILD.md](../native/MAC_BUILD.md#builds-outlive-the-desktop-2026-10-06)).
- 2026-10-06: Running a script shortcut that is already running restarts it:
  the desktop stops the running one (Stop's SIGTERM, then SIGKILL after 3 s)
  and starts it again when it exits (`MainWindow::shortcutRunning`,
  `m_shortcutRestartPath`). A second click within 3 s of the start is taken as
  a double-click and ignored. Stop cancels a pending restart. A different
  shortcut had to wait until 2026-10-09. The phone's Run does the same
  (`insights.run` answers "Stopping … to run it again", with the next run's
  id); other shortcuts still answer `shortcut_running`. This is how the Mac ad
  hoc shortcut cancels its own build ([../native/MAC_BUILD.md](../native/MAC_BUILD.md#the-shortcut-2026-10-04)).
- 2026-10-06: Actions tab workflow list. Each workflow is one line (tick box
  and name, plus a spinner while it runs); selecting it opens the step count,
  status, runs-on and the step graph. The tick box is the row widget's own
  (`ActionWorkflowArea`): the list item is no longer `ItemIsUserCheckable`,
  so there is one box instead of two that disagreed. An unticked box and a
  pending step used to paint solid black because `cond ? QColor : Qt::NoBrush`
  converts `Qt::NoBrush` to `QColor(0)`; both now pass a `QBrush`. The Runs
  table shows a second line under the workflow name with the branch, short
  commit and machine (`ActionRun::machine`, the node name stamped when the run
  starts; older records fall back to this machine).
- 2026-09-30: Settings → Cloud holds AWS credentials, encrypted at rest,
  with a read-only Test (`sts:GetCallerIdentity`) and an explicit "Write
  aws.env for the fleet tools". Add core node has a Vultr/AWS choice. See
  [networking.md](networking.md#aws-credentials) and
  [../deploy/README.md](../deploy/README.md#cloud-providers).
- 2026-09-30: Saved searches can be marked "Security pertinent". While such a
  search has unacknowledged results, a red banner with Open results and
  Acknowledge stays on screen and an alarm tone repeats every 3 seconds; the
  first raise also files a warning Ping. See
  [saved-searches.md](saved-searches.md).
- 2026-09-30: `src/CleanupHostFeed.*` and `MainWindow::installCleanupHost()`
  give the `cleanup` Market package the agent sessions, task work items and
  repositories it classifies worktrees by, remove a confirmed worktree with
  git (retired branches go to `refs/forkmesh/trash/cleanup/`), and remove one
  unclaimed Actions sandbox through `ActionRunner::removeUnclaimedSandbox()`
  ([marketplace/cleanup](../marketplace/cleanup.md)).
- 2026-09-30: The Insights tab's Contributors & Activity table no longer
  scrolls on its own: `fitInsightsContributorsHeight()` sizes it to every row
  (plus the horizontal scrollbar when the columns overflow), so the page's
  own scroll area moves through the list. The git log now also reads `%ae`,
  and the table shows each contributor's emails (most-used first, every
  address with its commit count in the tooltip, "Copy email" on the
  right-click menu) and their first and last commit dates in the selected
  range. The date-range caption and the data-source notes that sat under the
  table now sit between the section header and the table.
- 2026-09-30: Desktop client CI went red on every main push from 8f862fff7
  (runs 2059, 2119, 2125). `checkCollapsedDiffWindowFillsViewport`
  (`--diff-stream-only`) sized its pane at a fixed 1800×2400 and assumed the
  first two pages of 75 folded headers were shorter than that. The 8 px pill
  padding made each header 6 px taller, so the two pages overflowed the pane
  by 148 px, no third page was needed, and the check failed with "third page
  y=-1073741823, scroll max=148". The test now renders the same diff into a
  400 px probe pane first, reads the two-page height from it, and sizes the
  real pane 24 px taller than that, so header height changes no longer break
  it. The failure ping said only `Cause: [critical-tests] qt: 8.94s`:
  `ActionFailureSummary::looksLikeFailure` matched `FAILED` but not the
  window tests' `FAIL:` lines, so it fell back to the step's last line. It
  now matches `FAIL` as well (checked in `forkmesh-tests`).
- 2026-09-30: Every Actions step gets `FORKMESH_RUN_ID`, the run's number.
  A run that a restart or shutdown re-queues keeps its number, so a step can
  recognise work that the same run started before the desktop stopped. The iOS
  ad hoc build uses it to reconnect to a build that kept running on the Mac
  ([../native/MAC_BUILD.md](../native/MAC_BUILD.md), "Surviving a desktop
  restart"). `test_action_security` checks it in the `checkout: false` case.
- 2026-09-30: Git page review aids (#811).
  - The pills in a diff file header (Viewed, the pencil, Preview, the fold
    chevron) pad 8 px above and below instead of 5 px (`diffPillCellHtml`),
    so every file header row and sticky header is 6 px taller and Viewed is
    easier to hit. This applies to every diff pane.
  - A working-tree diff (no `head`) draws a trash pill beside the pencil
    (`DiffFileEntry::deletable`). Its `delete:<path>?d=<dir>` anchor goes
    through `handleDiffFileActionLink` (`registerDiffView` hands every diff
    pane's clicks to it) to `deleteWorkingTreeFile`, the same confirmation
    and single-file `QFile::remove` the file list's "Delete file" uses, then
    the Git page refreshes. Branch compares get no trash pill: deleting there
    would only leave a stray edit in the branch's worktree. Each pill carries
    an inline PNG, and the streamer's first batch is capped at
    `kDiffStreamBatchChars`, so with the pill on every branch header the
    cached repaint of a branch showed one file fewer on its first turn (the
    adhoc #227 window check caught it).
  - The working-tree diff's sticky header carries the pencil and trash
    pills too (`m_scmStickyActionHtml`), so a file scrolled into can be
    edited or deleted without scrolling back to its header.
  - A bar pinned to the bottom of the working-tree diff names the next file
    coming up: the first file whose header is still below the viewport (or
    under the bar itself). Clicking it scrolls there. It hides once the last
    file's header is on screen (`updateScmNextFileBar`). Window tests: the
    "bottom bar names the next file" and "sticky file header carries the
    edit and delete pills" checks in the full run; the delete link
    round-trip is in `--diff-stream-only`.
- 2026-09-30: Diff panes mark the words that changed inside a line (#845).
  A one-attribute edit such as dropping `checked` from a long `<input>` used
  to show as two full-width red and green rows with nothing pointing at the
  difference. The desktop now pairs the n-th removed line of each run with
  the n-th added line, as the phone apps do (`intraLineChanges` in
  `UnifiedDiff.kt` / `IntraLine.swift`), and `DiffIntraLine.cpp` ports that
  algorithm: split into word, space and punctuation tokens, trim the shared
  prefix and suffix, then an LCS over the rest. The changed tokens are
  wrapped in `delx` / `addx` spans, a stronger shade of the row colour
  (`diffStyleSheet`). The split view does this in `flushPairs`. The unified
  view now holds each run of `-`/`+` lines (`pendingRun`) until the next
  context line, hunk or file so it can pair them. A pair is left plain when
  the lines are equal, a side is empty, the token grid is over 40,000 cells,
  more than 70% of both lines changed (a rewrite reads better whole), or the
  two cells' HTML would exceed 4 KB. That last cap keeps the row under
  `kDiffMaxRowHtmlChars`, because `splitOversizeDiffRow` can only chunk cells
  that hold plain escaped text. Tests: `forkmesh-diff-intra-line-tests`, and
  the "diff marks the changed words in a line pair" checks in the window
  tests.
- 2026-09-30: An "Action failed" ping says what broke. Its body was only
  "Deploy nodes · forkmesh/forkmesh", so the reader had to open the run log
  to learn anything. `ActionFailureSummary` (desktop/src) now reads the run
  log and adds up to three lines under that heading:
  - `Step:` the last step header (`==> 🔧 …`) before the `==> ❌ FAILED:`
    footer.
  - `Cause:` the last line of that step that reads as an error (`FAILED`,
    `ERROR`, `error:`, `fatal:`, `…Error:`, `Traceback`), with ANSI colours,
    the `HH:MM:SS` stamp and a leading `FAILED: ` removed. With no such line
    it takes the step's last output line, then the runner's own verdict.
    Clipped to 500 characters.
  - `Fix:` whatever follows ` Fix: ` in that line. Deploy tools write their
    suggested fix that way
    ([auto-deploy](../deploy/auto-deploy.md#errors-carry-a-fix)).

  The same body reaches the Pings list, the OS card and a paired phone.
  Succeeded and stopped runs keep the one-line body.
- 2026-09-29: A left rail icon dragged out of the window no longer bounces
  back when let go. It spins where it was dropped until its window opens
  ([Dragging a rail icon out of the window](#architecture)).
- 2026-09-29: Chat and mention notifications show the sender's avatar. The
  desktop renders the same round face a chat row uses (the initials badge
  when the sender has no picture) to a PNG under
  `<cacheRoot>/notification-avatars/` and passes that path to `notify-send
  -i`; the tray fallback on Windows and macOS gets it as the balloon icon.
  The PNG is only written when a notification is about to show, so a busy
  channel in the focused window writes nothing.
- 2026-09-29: The branch review header has **Delete & clean up** beside
  Merge & clean up. It deletes the branch without merging it, and removes
  everything tied to it: the worktree (found even when its folder is gone),
  the local branch, and the branch's agent sessions. Linked issues are
  closed. `deleteReviewedBranchAndCleanUp` (`MainWindowBranches.cpp`) hands
  off to `deleteWorktreeBranchAndAgent`, the routine behind Worktrees >
  Delete, which asks for confirmation first. The deleted branch's pane then
  moves to the next branch in the list.
- 2026-09-30: Delete & clean up now sits before Merge & clean up in the
  branch review header, so the primary merge action ends the row.
- 2026-10-09: Delete & clean up also deletes the branch's pull request, so it
  leaves the PR list the way Merge & clean up's does. Once the local branch is
  gone, `deleteReviewedBranchAndCleanUp` hands the branch to
  `deletePullsOfMergedBranch`: every PR whose head is the branch (or `pr/<N>`),
  the PR the review pane was showing, and the deleted sessions' `prNumber`s.
  Cancelling the confirmation, or a branch that could not be deleted, leaves
  the PR alone.
- 2026-09-29: `forkmesh-e2e` links again. `AgentStore.cpp` calls into
  `AgentRuntimeSession.cpp` since the provider-neutral runtime contract, and
  every test target that compiles the store lists both except this one, so it
  failed with undefined `agentRuntimeIdForLegacyProvider` and
  `AgentRuntimeDescriptor::isValid`.
- 2026-09-29: Settings → Appearance → **Window opacity** works on Wayland.
  Qt 6.8's Wayland plugin has no window-opacity support (no
  `wp_alpha_modifier_v1`), so `setWindowOpacity` did nothing on GNOME and
  the slider looked broken. On a `wayland*` platform the main window is now
  created with `WA_TranslucentBackground` (keeping `WA_NoSystemBackground`
  off so the root still paints its opaque theme background), and
  `WindowFade` (`desktop/src/WindowFade.h`), a mouse-transparent child kept
  above every sibling, paints `CompositionMode_DestinationIn` at the chosen
  alpha. That scales the whole backing store, including partial repaints,
  the backdrop veil and overlays that raise themselves later (it watches
  their `ZOrderChange`). The attribute has to be set before the native window
  exists: turned on later, Qt keeps the full-surface opaque region it sent
  at creation and the compositor ignores the alpha. Other platforms keep
  `setWindowOpacity`. `MainWindow::applyWindowOpacity` picks the path. Test:
  `forkmesh-window-fade-tests`.
- 2026-09-29: the project picker says **Remove project…**, not Delete. The
  folder was never erased: "Move folder aside" renames `~/projects/<slug>` to
  `<slug>.deleted-<stamp>` (`projects::remove`, which now also returns the new
  path). The confirmation says so and that the identity key, settings and data
  are kept, with selectable text; the notice names the real path and stays up
  for 20 seconds. Test: the project-registry block of
  `forkmesh-app-paths-tests`.
- 2026-09-29: every recursive delete in the desktop goes through
  `forkmesh::safedelete::removeTree` (`desktop/src/SafeDelete.h`), which refuses
  HOME, checkouts, other people's worktrees and the protected project folders.
  The audit also stopped several flows from force-removing any worktree that
  holds a branch, from deleting a project folder (it is moved aside now), and
  from pruning the whole repository after a history rewrite. See
  [../engineering/destructive-actions.md](../engineering/destructive-actions.md).
- 2026-09-29: A Docs rail page (the `docs` Market package, on by default)
  shows the `www/docs` library the website renders, embedded in the binary.
  `fitReadmeImages`, `styleReadmeDocument` and `anchorReadmeHeadings` are now
  declared in `src/MarkdownDocument.h`, so a package can render Markdown the
  way the repository README does without including `MainWindowInternal.h`.
- 2026-09-29: Update, rebuild & restart no longer deletes the source checkout.
  When `git pull --ff-only` from the install mirror failed, for any reason, it
  ran `rm -rf` on the checkout and re-cloned with `--depth 1`. At 12:38 on
  2026-09-29 core n3 answered one pull with a 503, and the update erased
  `~/projects/forkmesh/src`, its `.git` (every local branch) and every agent
  worktree under `.worktrees` and `.claude/worktrees`. A failed pull now stops
  the update, says why, and changes nothing. A fresh clone runs only when there
  is no checkout, and only into a missing or empty folder. Test:
  `app/tests/test_desktop_update_keeps_checkout.py`.
- 2026-09-29: opening a branch compare paints much sooner. A log of one load
  (728, 19 files, clean worktree) read 5.7 s to "Diff on screen": ~0.3 s of
  git probes, 2.8 s of `git add -A` into the snapshot index, then the layout.
  The shell ran the same `add` in ~70 ms; the in-app cost was a cold inode
  cache (the box swaps, so an untouched worktree's ~8000 files are re-read
  from disk on first stat; a second run is ~65 ms), plus the click's other
  git work competing with it. Changes:
  - The committed range (`git diff --binary base...branch`, object reads only)
    is read beside the snapshot and painted first. For a clean worktree it
    is byte-identical to the snapshot's patch (checked on 12 worktrees), so
    the snapshot's arrival redraws nothing; uncommitted files join when it
    lands. The committed-range read now passes `--binary` for that identity.
  - Dropped steps: the checkout-root probe (`git rev-parse --show-toplevel`)
    is a file check (`.git` resolves to an existing gitdir), and the separate
    `git merge-base` is gone: the snapshot diff uses `--merge-base <base>`
    against the worktree's `HEAD` (same bytes, one process), falling back to
    a direct diff when the histories share no base. The cold snapshot seed
    reads `HEAD`'s tree instead of the base's.
  - The read worker decodes the patch and fingerprints it, so the render
    skips its fingerprint hop back to the GUI thread.
  - A click no longer starts the branch-table reload, repo overview and
    commit list reloads alongside the read: `deferUntilBranchDiffRead` runs
    them once the first page is on screen (or after 2 s). The table reload
    keeps the click's read sequence, so it does not trigger a second read.
  - Resting the pointer on a branch row for 120 ms prefetches that branch's
    diff into the patch cache (`prefetchBranchDiff`, at most two at once);
    the click then paints from the cache while the fresh read runs, and a
    prefetch that lands after the click paints if nothing has yet.
  - The log says "First page on screen" when the first page is laid out;
    the old "Diff on screen" line, which fired after the last streamed batch,
    is now "Whole diff laid out".
  - The checkout re-cloned on 2026-09-29 had no commit-graph, so every
    `base...branch` walked commits out of a 490 MB pack: `git merge-base`
    took ~200 ms warm on an older branch (~20 ms with a graph), and a cold
    committed-range read of 728 took 2.1 s (67 ms warm). The branch-stats
    worker now runs `ensureGitCommitGraph` first: once per repository per
    session, `git commit-graph write --reachable --split` when the graph is
    missing or older than a day (~2.7 s the first time, ~60 ms after). The
    per-branch `--numstat` counts get the same gain.
  - After the branch table's change counts land, `warmBranchDiffs` reads the
    committed range of the top six rows (one worker, in order) into the patch
    cache, keyed without a worktree and re-read only when the tip moves. A
    click on a worktree branch with no snapshot cached paints that committed
    copy on the next event-loop turn while the snapshot read runs. The cache
    holds 16 patches (was 8) so warming does not push out branches visited.
- 2026-09-29: ticking Viewed in a branch compare ("Changes against main")
  moves to the next file, whether the pill was on the sticky header or in the
  file's own header row. The advance used to live only in the sticky header's
  handler; it now sits in `onBranchDiffAnchorClicked`, so both pills share it.
  Unticking stays on the file. `scrollBranchDiffToFile` also moves the file
  list's row (`selectScmFileInTree`): before, the green outline in the diff
  moved to the next file but the list kept pointing at the one just marked,
  because the scroll suppresses the scroll-driven tree sync. The file list's
  selected row is now a light green fill with a softer border, drawn by the
  row widget alone (the tree's own `::item:selected` border sat on the same
  edges). The blue square left of the selected row was the tree's selection
  highlight painted into the branch/indent area; `show-decoration-selected:0`
  and a transparent `::branch:selected` remove it.
- 2026-09-29: the green outline around the active file in the branch/PR and
  working-tree diffs closes over the sticky header. Once the file's own header
  row scrolled off the top, the outline's top edge sat above the viewport and
  the sticky header covered the rest, so the box had no top. The outline now
  starts where the sticky header does (`diffStickyTopOffset`, so it follows
  the header out as the next file arrives) and is stacked above it; it is
  transparent to the mouse, so the header's pills still take clicks.
- 2026-09-29: a diff file header has a pencil pill beside the churn bar that
  opens the file in the Files editor (`openWorkingTreeFile`), the same target
  as the right-click "Open in file editor". The renderer stamps
  `DiffFileEntry::editDir` only where the file on disk is the diff's new side:
  a working-tree diff (empty `head`), or a checkout whose `HEAD` is the branch
  being diffed (`diffEditableDir`, read from the `.git` files so worker
  renders spawn no git). Commit, pull-review and patch-only diffs, deleted and
  binary files get no pencil. The `edit:<path>?d=<dir>` anchor is handled in
  `registerDiffView`, so every diff pane gets it; the click re-checks the path
  is relative, stays out of `.git` and still exists.
- 2026-09-29: pings are forwarded to a paired phone as push notifications
  (`MainWindowPingForward.cpp`, `DesktopPingForward.{h,cpp}`), switchable in
  Settings > Pings; see [native/DESKTOP_PINGS.md](../native/DESKTOP_PINGS.md).
- 2026-09-28: Clicking a pull request ping (Pings row, top banner, linked-PR
  chip) lands on that pull request's detail. `openNotificationLink` used to
  call `showPull` right after `openRepoDetail`, which had just emptied the
  list and started a background load, so the number was never found and the
  pane said "Select a pull request". The ping now opens the repository
  straight on the Pull requests tab and calls `openPullFromNotification`
  (`MainWindowPulls.cpp`): a synchronous ledger read selects the row. If the
  pull request has not reached this node yet, it drains the relay inbox and
  reconciles the mirrors (`refreshPullsFromMesh`) and keeps the number as
  `m_pullOpenTarget` for two minutes, so the first reload that carries it
  selects it, scrolls it to the top of the list and switches the
  Open/Closed/Merged filter if that filter hid it. Selecting another row, or
  opening another repository, drops the target.
- 2026-09-28: Salvaged from the 2026-09-27 fix branches: the Actions
  workspace clone survives a torn ref, the failure diagnostic masks variables
  in the step command, a push to an old branch no longer runs a workflow the
  default branch made non-push; the Team room member scan, cached large
  branch diffs, agent branches with nothing to land and unreadable worktree
  status. Verdicts for every
  change, including what was dropped:
  [diagnostics.md](diagnostics.md#salvage-of-the-2026-09-27-fix-branches-2026-09-28).

- 2026-09-28: Deleting a branch on the Branches page also removes its
  worktree (#719). `git branch -D` refuses a branch checked out in another
  worktree ("used by worktree at ..."), so `deleteBranch`
  (`MainWindowBranches.cpp`) now looks the worktree up (including one whose
  folder is already gone), names it in the confirmation, and hands off to
  `removeWorktree`, which removes the worktree and then the branch. Agent
  sessions on the branch are kept; Worktrees > Merge & clean up still removes
  those.
- 2026-09-27: Merging a branch that is only on the mirror or only in a
  patch-based pull fetches or rebuilds it first instead of failing with "not
  something we can merge"; the Merge editor says why a commit failed (task 573,
  see Traps).
- 2026-09-27: Notes created on the web now show on the desktop Notes page.
  The desktop signs `/api/notes` requests with the account key when it has
  no password session, and the relay accepts those signed requests. Before
  this, the page showed local notes only. See [notes.md](notes.md).
- 2026-09-28: Errors no longer leave the desktop unless the operator opts
  in. `reportUserVisibleError` (`MainWindowMessages.cpp`) used to send every
  warning dialog and error card to the relay's error log
  (`POST /api/desktop-errors`) unless `diagnostics/reportErrors` was set to
  false, and nothing in the UI could set it. The setting now defaults to
  false in both places that read it (the sender and the Pings sync label in
  `MainWindowActions.cpp`), and Settings > General has a "Send errors from
  this desktop to ForkMesh" checkbox (`reportErrorsCheck`), unchecked by
  default. With it off, the error still reaches the Log and Pings, and its
  Ping is marked local-only with "sending errors to ForkMesh is turned off
  (Settings > General)". No installed desktop had the key set, since there
  was no control for it, so every existing install stops reporting after
  the update.
- 2026-09-28: A remote issue event for an issue this node has not opened
  (a web claim release that reached the inbox before the opening, or before
  the checkout had the issue) no longer invents an opening event.
  `IssueStore::applyRemoteEvent` used to write an unsigned `open` event
  (`id open-N`, `sig ""`, title `titleIfNew`, often empty) into
  `.forkmesh/issues/open/N/issue-N.json`; that is what the desktop wrote for
  #729 during the 2026-09-28 core divergence, next to the cores'
  `relay-placeholder` copies. It now returns false with a reason: the inbox
  row is not drained and is applied once the opening event exists. When the
  signed opening event arrives for a slot that holds such a fabricated
  opening (a core's `sig "relay-placeholder"`, or an unsigned `open-N` with no
  body or attachments whose title is empty or whose timestamp equals the next
  event's), it replaces that opening in place and keeps every later event,
  instead of moving the real issue to a new number. Tests in
  `test_crypto.cpp` (IssueStore section).
- 2026-09-28: One bad ref no longer fails a whole mirror sync or every pull
  request head publish. A mirror branch whose tip object is missing or corrupt
  (`refs/heads/608-add-crm-mobile`, 156 lines on 2026-09-27; earlier
  `429-sure-watch-feature` and `refs/forkmesh/upstream/heads/405-repair-crashes-sigsegv-sigabrt` on the cores) makes every fetch into
  the mirror fail its connectivity check (`fatal: bad object refs/heads/<b>`,
  "did not send all necessary objects") and every push into it fail in
  receive-pack (`remote: fatal: bad object refs/heads/<b>`, "! [remote
  rejected] ... (missing necessary objects)"). The Actions "could not publish
  pull request #N's head" lines were that push. The earlier fix only matched
  `refs/heads|tags`, used one attempt of the three-attempt ref-race budget per
  ref, and the Actions publish had no repair at all; its error was cut to the
  last 200 characters, which dropped the ref name. Now
  `forkmesh::upstream::dropUnreadableRefs` (`UpstreamCheckoutSync.cpp`) takes
  every ref named in `bad object refs/...`, deletes it from the mirror only if
  `git cat-file -t` cannot read its tip (a readable ref named in error text is
  never touched), with the ref's value as the old-value guard. The sync
  (`MainWindowRepoFetch.cpp`) runs it off the GUI thread with up to eight
  repairs per sync and fetches again, so the source restores the branch. If
  the named ref is readable in the mirror, the bad copy is the source's: the
  sync logs a warning and fetches with a `^ref` negative refspec for 30
  minutes, so every other branch keeps syncing. `resolvePullCheckHeads`
  (`MainWindowActions.cpp`) keeps the push's full stderr, drops the unreadable
  refs it names and retries the push once.
  `tests/test_upstream_checkout_sync.cpp` reproduces both failures with a
  missing object and an empty (corrupt) loose object. How the refs broke was
  not recoverable: the ref and its source branch were gone by 2026-09-28 and
  bare mirrors keep no reflog; the same day's "ORIG_HEAD ... reference broken"
  in the 608 worktree and a disk at 92% point at truncated writes.
- 2026-09-28: Actions runs killed by a stop signal to ForkMesh (rebuild &
  restart) are re-queued instead of failing, and pushes that arrive during
  the stop are spooled for the next start ([Traps](#traps)).
- 2026-09-28: "Couldn't create the requested PR for Agent #N" names the
  reason. `maybeCreatePullForStreamSession` read a failed `git diff` (the 8 s
  git wait running out under load: the notice came exactly 8 s after
  "Merging ..." each time, followed by "QProcess: Destroyed while process
  ("git") is still running") as an empty diff and logged "No code changes; no
  pull request created." for agents 574, 576, 605, 632 and 635, whose branches
  had commits; the notice then only said "See the agent log". A git failure is
  now logged as "!! Could not create pull request: git could not read the
  branch's changes in <worktree>: <git's error>", and every refusal (no base
  commit, git failure, empty diff, the pull store's error, a pull sent to the
  owner's inbox) is kept in `m_agentPullFailures` and quoted in the notice.
  Window test: `forkmesh-agent-landing-window-tests`.
- 2026-09-28: Desktop client CI: the agent-efficiency window test ("the
  running Sonnet 5 probe counts as a live run", failing since run 684) opened
  the Models board after the probe's `result` event. `result` starts the
  off-thread subprocess scan, which marks a session with no process Success,
  so the board counted it as OK rather than Live whenever the scan finished
  first. The test now starts a new turn (`system`/`init`) before opening
  Models, so the probe is running when it is counted. Taken from the unmerged
  `fix/actions-failures` branch.

- 2026-09-28: "New private room" creates a relay-enforced room with a people
  picker instead of a mesh label on World #general, which every connected
  device could read ([app/chat-private-rooms.md](../app/chat-private-rooms.md)).
  "Invite" and the Settings dialog's member list use the signed members
  endpoint. Direct messages are server conversations (`#office/@<user>` in
  `OfficeChannelMirror`): "New message" in the sidebar and the New channel
  menu, the profile card, a node profile and an @mention click open or reuse
  one, and they reach people who are offline. A legacy mesh private room no
  longer lists every account as its members, and the desktop stops sending
  and accepting mesh `invite` frames.
- 2026-09-28: Ticking Viewed (or reopening a file) in any diff pane — Git,
  pull request, branch compare — keeps the file's header where it was. The
  in-place fold (`swapDiffFileViewedState`, `MainWindowShared.cpp`) used to
  keep the old scroll value, which is right while the header is on screen but
  not when the pill was clicked on the sticky header of a file scrolled
  into: the body above the viewport vanished and the pane landed somewhere
  in the following files, further the longer the folded file was. The fold
  now scrolls to the smaller of the old value and the header's own top
  (`diffFileRowRect`), so a file folded from its sticky header stays at the
  top of the pane with the next file right below it. The window test
  `testScmViewedClickKeepsViewport` checks both cases.
  Follow-up the same day: fold then reopen drifted 14 px (scroll 430 after
  the fold, 416 after the reopen). The header's top was read with
  `diffFileRowRect(view, start)`, but `start` is the empty block the file's
  `<div>` opens, not a cell of the header table, so that call always returned
  a null rect and the fallback measured the cursor at `start`. That cursor
  rect is 14 px higher in the folded form than in the open one (measured:
  430 open, 416 folded), while the header row itself is at the same y in
  both. The reopen therefore landed
  14 px above the row the fold had put at the top. The swap now measures the
  first one-row table in the file's range, i.e. the header row itself, so a
  fold and its reopen agree on one position: nothing moves on screen, and
  the reopen scroll equals the fold scroll. The older bottom-of-diff check in
  `checkDiffViewedInPlaceSwap` (`--diff-stream-only`) now asserts that
  invariant (landing = min(scroll, header top), folded and reopened header
  at the same screen y, maximum still reaching it) instead of an unchanged
  raw scroll value.
- 2026-09-28: A new project is no longer connected to app.forkmesh.com. It
  starts offline, and when it opens the user picks its instance
  ([A new project has no instance](#architecture)).
- 2026-09-28: Any button that opens a page can be dropped into the left rail
  from anywhere in the app. Repository tabs and Code toolbar buttons move
  there; every other page button becomes a rail shortcut that opens the same
  page ([Dropping any page button into the rail](#architecture)).
- 2026-09-28: Dragging a left rail icon out of the window (or "Open in new
  window" on its menu) opens its page in its own window: repository tabs such
  as Issues, Code, Git, Agents and every section page
  ([Dragging a rail icon out of the window](#architecture)).
- 2026-09-28: A `.forkmesh` workflow can say `checkout: false` at its top
  level. The runner then makes no worktree, clone or checkout: the steps start
  in an empty `/workspace` and `/source`, and only the approved workflow text
  runs on this node. Approval still binds to the commit's tree. Every step
  now gets `FORKMESH_COMMIT` and `FORKMESH_REF`, so a workflow like this can
  fetch the commit somewhere else. The iOS ad hoc build uses it so the Mac
  fetches the source itself
  ([../native/MAC_BUILD.md](../native/MAC_BUILD.md)). The workflow parser
  drops blank lines and lines that start with `#`, even inside `run: |`
  blocks, so a heredoc in a step cannot hold a `#` line.
- 2026-09-28: A mirror sync that fails with `fatal: bad object refs/heads/<b>`
  / "did not send all necessary objects" heals itself. The message means the
  mirror's own copy of that branch points at an object it no longer has (seen
  for `608-add-crm-mobile` in `forkmesh/forkmesh`): `fetch --prune` only drops
  branches the source no longer has, and the connectivity check after the
  transfer walks every local ref, so each later sync failed the same way while
  the source kept the branch. `syncRepository`'s failure path
  (`MainWindowRepoFetch.cpp`) now deletes the named `refs/heads/*` or
  `refs/tags/*` ref from the mirror with `git update-ref -d` and fetches again,
  sharing the three-attempt budget of the ref-race retry; the fetch restores
  the branch from the source. `tests/validate_error_log_regressions.py` guards
  it.
- 2026-09-28: Every chat channel row has a gear (a second column in
  `ChannelGroupTree`), and the row's right-click menu has **Channel
  settings…**. Both call `MainWindow::openChannelSettingsFor`, which picks
  the dialog for the channel:
  - an office channel you manage gets the full editor (name, topic,
    visibility, webhooks, delete);
  - any other office channel, #email-log included, opens the same dialog read-only, with
    a line that says why;
  - a mesh room gets a small dialog with its visibility, **Invite…** for a
    private room and **Delete room**;
  - a Discord channel opens the Discord connection settings.

  The header **Settings** button now shows on every channel, not only the
  ones you manage. The web chat has the same gear on each room it can open
  settings for: the channels you manage, and #email-log, where it opens the
  server-visible panel.
- 2026-09-28: The desktop shows #email-log notes. The server writes them with
  the room key only, because it cannot seal to devices. The desktop treated
  the channel like any private room and dropped every unsealed frame, both in
  history (`OfficeChannelMirror::applyHistory`) and on the live socket (its
  `ServerNode` was built with `createPrivateChannel`). So the channel stayed
  empty while the server had posted 41 batches.

  The mirror now reads `serverVisible` and `readOnly` from the channel list:
  - a server-visible room takes room-layer frames;
  - a read-only room refuses sends, and its composer says "Server notices are
    read-only";
  - the header shows **Server-visible** instead of **End-to-end**.

  Every other private channel still refuses unsealed frames.
  `testOfficeNoticesChannelShowsServerFrames` in `test_chat_transport.cpp`
  covers both cases. See [../MAIL_LOG.md](../MAIL_LOG.md).
- 2026-09-28: Pull main and Merge & clean up no longer run git merges in the
  same worktree at once. Background git calls skip optional locks, and a
  dirty-checkout refusal is a notice naming the branch, not an error
  ([agents-ui.md](agents-ui.md)).
- 2026-09-28: Node access lines show the real visitor behind Cloudflare or
  the forkmesh.com Worker, with place, network and a one-time whois line;
  expected-degraded 503s are warnings ([../NODE_LOGS.md](../NODE_LOGS.md)).
- 2026-09-28: Merge & clean up no longer refuses over main-checkout edits the
  merge doesn't touch, and a refusal names the checkout and its dirty files
  (#654; [../agents/README.md](../agents/README.md)).
- 2026-09-28: Notes created on the web now show on the desktop Notes page.
  The desktop signs `/api/notes` requests with the account key when it has
  no password session, and the relay accepts those signed requests. Before
  this, the page showed local notes only. See [notes.md](notes.md).
- 2026-09-27: Website merges. `WebBranchOps` applies the new `merge` op
  (`applyMerge`, then `cleanUpMergedBranch` in a second pass), and
  `MainWindow::applyWebBranchOps` refuses a merge while an agent is still
  running on the branch. It runs the desktop merge's bookkeeping (pull
  reconciliation, merge note, linked issues, agent sessions) and posts every
  outcome to the relay (`reportWebBranchOpResults`).
  `markAgentSessionsMerged` and `closeIssuesForMerge` gained repository-scoped
  variants for a repository that is not the one on screen. See
  [../app/branch-ops.md](../app/branch-ops.md).
- 2026-09-27: A phone signed in to the same ForkMesh account as the desktop
  controls it without the six-digit pairing code, on the LAN and over the
  relay. The desktop checks the phone's key with `POST
  /api/accounts/devices/check`; see
  [networking.md](networking.md#phone-control-over-the-lan).
- 2026-09-27: The repository Shortcuts & Skills page has a Cards / Table
  switch (`shortcutsView`, remembered in the `shortcuts/view` setting). The
  table (`shortcutsTable`) keeps every entry to one fixed-height, non-wrapping
  row and sorts by any column: name, type, scope, runs, success, last run,
  added, modified, file and description. It starts sorted by runs, most first,
  like the cards. Added is the file's creation time; a file system that does
  not record one shows the last modification instead and says so in the
  cell's tooltip. Double-click (or Enter on) a row does what clicking its card
  does; right-click offers edit and delete.
- 2026-09-27: Chat ping cards (the one on screen and each queued card) carry
  a row of quick reactions (Like, Love, Laugh, Celebrate, Eyes, Done). A
  click toggles that reaction on the message the ping is about, through the
  same path a chat row's chips use (`MainWindow::toggleChatReaction`), so
  Discord channels get the literal emoji and office channels, which take no
  reactions, show no row. The chat ping now files the message id
  (`AppNotification::chatMessageId`) so the card knows which message to react
  to; it is not written to the Pings journal, so a ping restored after a
  restart has no row. A button shows as pressed when this node already
  reacted with it. The card stays up, and reacting does not acknowledge a
  must-dismiss card.
- 2026-09-27: Deleting a `forkmesh-mesh/<branch>` row in Branches works
  instead of failing with a bare "Could not delete X on forkmesh-mesh.", and a
  failed remote delete shows git's error ([Traps](#traps)).

- 2026-09-27: Selected rows no longer vanish. Lists and trees without their
  own selection rule paint the highlight band again instead of white text on
  a white row, the teammate model picker's selected row sets its own text
  colour, and the pull request list's age and file count keep their normal
  colour when selected ([Traps](#traps)).
- 2026-09-27: Every alert card (the one on screen and each queued card) has a
  destination button beside Copy. A "Firewall blocked" ping says "Manage
  firewall" and opens Network → Firewall Rules with the blocked rule
  (or `host:port`) pre-filled and its row selected, so Allow or Deny is one
  click. The ping files a `firewall` link, so its Pings row opens the same
  place. Other cards open their issue, pull request, run, agent, chat and so
  on. A ping or error with nowhere specific to go says "View in Pings". The
  button is left off when View output or View agent already opens the same
  target, and it never acknowledges a must-dismiss card.
- 2026-09-27: Left rail icons reorder by dragging; the order is saved
  ([Dragging rail icons](#architecture)).
- 2026-09-27: Fleet node lines get their own log groups (App, Cron, Web,
  Daemon, IDS, Cert, Ops, Job, Mail, System, with DB for rqlite; Deploy for
  deploy output) instead of all landing under Cloud; autoscaled (`aN`) and
  burst (`bN`) nodes parse as node lines; daemon request records read as text;
  node lines are coloured by node, group, severity, status, duration and SQL
  ([networking.md](networking.md#fleet-node-logs)).
- 2026-09-27: `src/BackupHostFeed.*` exposes back up now, restore, hourly
  on/off and the rotation size to the `backups` Market package, and every
  Settings → Data status line reaches its page
  ([marketplace/packages/backups](../../marketplace/packages/backups/README.md)).
- 2026-09-27: Search results (Ctrl+K overlay, header dropdown, results page)
  show an Open / Closed / Merged badge and status icon on issues, pull
  requests, discussions, projects and milestones, and highlight the query in
  each row. `SearchHitDelegate` paints the rows; `statusBadge` and
  `highlightRuns` in `GlobalSearchMatch` stay GUI-free so the tests cover
  them. The dropdown now shows where a body or comment matched, and snippets
  mark text cut after the match with an ellipsis.
- 2026-09-28: Network > Nodes is one table: each node's row also shows its
  copy of the repository chosen under Repository mirrors, with every former
  mirrors column beside its counterpart, and several copies on one node are
  grouped as lines under it (issue 718,
  [networking.md](networking.md#one-table-issue-718)).
- 2026-09-28: Network > Nodes lists runners (fleet files, the `forkmesh-next`
  Vultr listing and `/api/runners`, with last seen, job leases, scope and a
  Logs action) and relays (every former Relays tab column and the website
  action) as roles Runner and Relay; the Relays tab is gone and its links open
  Nodes filtered to Relay. Repository mirrors keeps a core the relay reports
  live under Online only even when its endpoint lease lapsed because its refs
  diverged (n1, 2026-09-28)
  ([networking.md](networking.md#network--nodes)).
- 2026-09-27: Buttons move between the left rail and the repository tab bar
  from their right-click menu, for instance Pulls out of the repository page
  into the rail ([Movable buttons](#architecture)).
- 2026-09-27: The Code toolbar's Overview, Branches, Worktrees, Remotes, Tags
  and Releases buttons move to the left rail, into rail folders and back
  ([Movable buttons](#architecture)). A repository tab inside a rail folder
  now also opens the repository page from another section.
- 2026-09-27: Actions run detail: the primary pane lays out like each watched
  column, with the title alone on its row and Stop, Skip, the icon-only copy
  log, copy path and Send to agent buttons, then Rerun at the right, on a row
  under it. The primary pane's title row keeps an empty slot the width of a
  column's close button, and its right margin drops from 24 to a column's 12,
  so the rows line up across panes. A running run shows a spinner beside its
  title in the pane and in every column (`src/MainWindowActions.cpp`).
- 2026-09-27: A stale `rebase-apply` or interrupted merge of the same PR is
  cleared before a PR merge instead of failing it ([Traps](#traps)).
- 2026-09-25: Log fixes: no SIGSEGV when a relay or event-socket frame drops
  the link ahead of a queued close frame; room peers persist only when new;
  fleet log cursors save off the GUI thread; the Worker tail stops for good on
  "Worker does not exist" and the Node probe runs off the GUI thread; the
  `link-external` icon ships as a codicon; fleet node lines replayed from
  more than 15 minutes ago, and the boot-time vsock generator error, no
  longer raise error pings
  ([networking.md](networking.md#relay-transport),
  [networking.md](networking.md#cloudflare-worker-log-tail)).
- 2026-09-24: The Log page follows the Next core nodes' journals over SSH;
  their rqlite statements land under DB and in a live ranking on the D1 tab
  ([networking.md](networking.md#fleet-node-logs)).
- 2026-09-24: Fleet node logs: cores and runners (autoscaled ones too) through the read-only `fmnext-logread` forced command, the cursor saved on disk, the Nodes filter, and Caddy access lines feeding Web Requests (networking.md, Fleet node logs).
- 2026-09-24: Rebuild & restart and the data-wipe relaunch restart the systemd
  user unit instead of starting a child the unit stop then killed; publishing no
  longer needs the flagship ([../tools/source-publisher.md](../tools/source-publisher.md)).

- 2026-09-24: 1.2.4. Windows mirror Actions lock no longer lives at
  `C:\HKEY_CURRENT_USER`; recovery failure is non-fatal; the NSIS uninstaller
  deletes only its own payload (#727; `f08182c96`, `aef0332af`).
- 2026-09-24: Hosts → Update refreshes the host's agent CLIs; cloud agent rows
  show a run's error once (`de8481362`).
- 2026-09-23: Web visitors' flags, agents and threats in Agent activity.
- 2026-09-22: `MainWindowInternal.h` helper bodies moved into
  `MainWindowInternal1-4.cpp`; build commit id moved to its own generated
  header; `run.sh` configures `FORKMESH_BUILD_TESTS=OFF`.
- 2026-09-22: Interface sounds on by default; Merge & clean up removes only the
  merged agent; Windows installer cross-builds OpenSSL 3.5.
- 2026-09-21: Phone control over the local network (`0599b91c0`); HTTP/3
  client for host logs; per-project agent access (Landlock lock and
  external-call gate); projects layout under `~/projects/forkmesh`.
- 2026-09-19: Client display polling removed.
- 2026-09-18 onward: Password Vault, Voice Changer and other Market packages
  hosted in the desktop (see [../marketplace/README.md](../marketplace/README.md)).
- 2026-09-16: Health page (component health).
- 2026-09-13: Haiku safety preflight for website agent jobs removed.

## Related docs

- [www/docs/qt-client.md](../../www/docs/qt-client.md): user guide
- [www/docs/operations/desktop-control-node.md](../../www/docs/operations/desktop-control-node.md): Control page
- [www/docs/engineering/definition-of-done.md](../../www/docs/engineering/definition-of-done.md)
- [www/docs/engineering/network-log-badge-audit.md](../../www/docs/engineering/network-log-badge-audit.md)
- [docs/native/DESKTOP_PARITY.md](../native/DESKTOP_PARITY.md): native app parity with the desktop

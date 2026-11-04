# Desktop package modules and the empty shell

The goal is a completely modular ForkMesh: every feature a plugin, and an
empty shell that takes almost no memory, with everything added from the
Market. This page covers how optional packages are built as shared modules and
loaded only when installed, how plugin pages stay apart from the window code,
the window services they use, the Empty project and the core services that
start and stop with their packages, and what is still core. The marketplace
side is in [../marketplace/plugins.md](../marketplace/plugins.md).

Part of [Desktop](README.md).

## Code

| Part | Code | Tests |
| --- | --- | --- |
| Modules | `desktop/cmake/ForkMeshPackageModules.cmake`, `desktop/src/PackageModules.{h,cpp}`, `desktop/scripts/check_package_modules.py` | `forkmesh-package-modules-resolve`, `forkmesh-plugin-build-isolation`, `marketplace/tests/test_plugin_build_isolation.py` |
| Core services | `desktop/src/CorePackageServices.*`, `desktop/src/MainWindowCorePackageServices.cpp` | `forkmesh-core-package-services-tests` |
| Web engine module | `desktop/src/WebEngine.*`, `desktop/src/WebEngineQt.cpp` | `forkmesh-web-engine-module-tests` |
| Empty pack | `projects::StarterPack::Empty` (`desktop/src/ProjectRegistry.*`, `MainWindowProjectPicker.cpp`) | `forkmesh-app-paths-tests` |

## Modules

Optional marketplace packages that only talk to the window through
`NativePackageRegistration` are built as separate shared modules instead of
being linked into `forkmesh`.

- **Build.** Every plugin listed for the build ([plugins.md](../marketplace/plugins.md))
  that has `content/desktop/build.json` and is not `coreLinked` becomes
  `build/packages/<id>.so` with a sidecar `<id>.json` (`api`, `abi`,
  `package`, `file`, `entrypoints`) when `FORKMESH_PACKAGE_MODULES` is on
  (default on for Linux, off elsewhere, where listed plugins are linked into
  the app). The list comes from the generator
  (`FORKMESH_PLUGIN_MODULE_CANDIDATES`); there is no hand-kept list.
  The module also carries the plugin's own files (`plugins/<id>.qrc`); the
  core keeps only its listing files. Configure removes module files the
  configuration no longer builds. Modules link Qt Multimedia, OpenGL, WebP
  and Qt WebEngine (for the Video Mix DJ Studio's YouTube decks) when the
  build has them. The linker records only the libraries a module uses, so most
  modules need nothing the app has not loaded. Qt WebEngine is the exception:
  the app does not link it on Linux
  ([browser](browser.md#the-engine-is-a-module)), so Face Changer and the
  Video Mix DJ Studio map it when they load, like the `web-engine` module
  does. Modules resolve the app's symbols through
  the `-rdynamic` export the stall watchdog already needs, so the app binary
  does not grow. Installed-by-default plugins (agent-watch, analytics, docs)
  are modules too and load at start (about 30 ms and 3 MB together, see
  [Measurements](#measurements)).
- **Include folders.** A plugin's sources see `desktop/src` and their own
  `content/desktop` folder only (a source property from the generator's
  `FORKMESH_PACKAGE_<ID>_INCLUDE_DIRS`); the app target sees `desktop/src` and
  the core-linked plugins' folders, so adding or removing a plugin changes no
  other compile command (see [Measurements](#measurements)). No plugin includes another plugin's headers, and the core includes only the
  core-linked ones; a plugin that needs another's header has to say so here.
- **Loading.** At start the app reads only the sidecars (from `packages/` beside
  the binary, `../lib/forkmesh/packages`, the install prefix's
  `lib/forkmesh/packages`, and `../PlugIns/forkmesh-packages` on macOS). A
  module is loaded the first time `nativePackageContribution()` asks for its
  entrypoint, which happens only for packages installed on this desktop: a
  package that is not installed is never mapped.
- **Versioned API.** The sidecar's `api` must equal the app's
  `FORKMESH_PACKAGE_MODULE_API` (1) and its `abi` the app's build identity
  (version, compiler, Qt, build type), checked before `dlopen`. After loading,
  the exported `forkmesh_package_module_api/abi/id` must match again, or the
  module's entrypoints are unregistered. A refused or broken module only
  removes that package's page; the log says why (`[package module] …`).
- **Trust.** Modules load only from the application's own folders, never from
  user-installed marketplace content. A module file or its folder that another
  user (or a group other than the user's own) can write is refused.
- **Install.** `install.sh` copies the modules to `~/.local/lib/forkmesh/packages`
  and removes ones the build no longer produces; Rebuild & restart and
  auto-update do the same beside the installed app (`modules::installBeside`);
  `cmake --install` puts them in `<libdir>/forkmesh/packages`.
- **Test.** `ctest -R forkmesh-package-modules-resolve` checks that every
  module's undefined symbols are provided by the app or its own libraries, so a
  new window-to-package reference fails the build's tests instead of the user's
  start.

## Plugins built apart from the window

`MainWindow.h` and `MainWindowInternal.h` are the app's precompiled header and
change many times a day; every file that includes them recompiles on each
change. So a plugin's sources include only
`NativePackageExtension.h` and small, stable core headers (`OcticonIcons.h`,
`Theme.h`, `DesignTokens.h`, ...), never `MainWindow.h` or
`MainWindowInternal.h`, and reach the window only through `NativePackageHost`.
Then a window change rebuilds none of them; a change to
`NativePackageExtension.h` rebuilds every module, which is rare.

What moved out of the window:

| Plugin | Was | Now |
| --- | --- | --- |
| Timers | `MainWindowTimers.cpp`, core-linked | `TimersClock` top-bar widget in `timers.so` |
| Calendar | `MainWindowCalendar.cpp`, `RepositoryCalendar.cpp`, section 19 | `CalendarPage` and `RepositoryCalendar` in `calendar.so` |
| Notes | `MainWindowNotes.cpp`, section 16 | `NotesPage` in `notes.so` |
| Tasks | `MainWindowTasks.cpp` (about 8,000 lines), section 15 | `TasksPage` in `tasks.so`; the catalog stays in core as `OrganizationTaskStore` |

The pages are ordinary native package pages: they appear on the rail
when the package is installed here, under their old rail keys (`notes`,
`calendar`, `tasks`) so a saved rail order or folder still finds them, and
their search hits come from the page. Section indexes 15, 16 and 19 stay
unused rather than renumbering every later section. The window keeps the few things other
window code calls: the quick-add composer's "take a note" (`note.add`), note
links in bodies and from the system (`note.openLink`, `note.openSystemLink`),
and the note-to-task and note-to-issue conversions, which run as window
commands (`task.composeFromText`, `issue.composeFromText`) because the task
and issue composers belong to the window. Calendar reads repositories through
`NativePackageHost::repositories` (its manifest's `repository.activity.read`
permission grants it) and Git through `GitCapture.h`, and its Git walk runs on
the thread pool (`QtConcurrent`) instead of the window's `runOffThread`. Its
"Fill event" asks the chosen CLI through `askAgentWith` and stops it when the
dialog closes.

A page that claims the composer (`hostPromptTarget()` with a placeholder) gets
Enter and the **new** button, as Calorie Counter and Water do. One that sets
`appliesToOpen` (Notes, while a note is open: [notes.md](notes.md#editing-a-note-with-ai))
edits what is open instead: Enter and **add** go to `submitHostPrompt()`, and
**new** does what it does on any other page.

### Keeping them apart

Two checks hold the line, so a plugin recompiles only when its own files or the
plugin API it includes change:

- `marketplace/tests/test_plugin_build_isolation.py` (no build needed, runs with
  the Python suite) follows every module plugin's `#include "..."` tree through
  its own folder and `desktop/src`. It fails when the tree reaches a
  `MainWindow*.h`, `ForkMeshVersion.h` or `ForkMeshBuildCommit.h` (rewritten on
  every commit), or another plugin's folder, and when a `desktop/src` file
  includes a module plugin's header.
- `ctest -R forkmesh-plugin-build-isolation`
  (`desktop/scripts/check_plugin_build_isolation.py`) reads the header
  dependencies Ninja recorded in a real build, so it also sees system and
  generated headers. It fails when a plugin object depends on a window header,
  the per-commit headers, the app's precompiled header or its `forkmesh.dir` /
  `forkmesh_autogen` output, or another plugin's files, and when a core object
  depends on a module plugin's files. Other generators print SKIP.

Core-linked plugins (`coreLinked`) are part of the app and exempt until they
move out.

### Window services

`NativePackageHost` carries generic services a plugin page or widget needs
from the window. None of them names a package:

| Service | What it does |
| --- | --- |
| `flashMessage(text, error)` | the window's toast; `error` marks it a local-operation notice, kept out of error reports |
| `reportError(text)` | an error toast that is filed as a client error report |
| `logActivity(text)` | a line in the activity log |
| `openReference(href)` | open an issue, pull request, discussion or other ForkMesh reference link in the window |
| `showPage()` | show the plugin's own page |
| `windowCommand(command, args)` | ask the window to do something it owns (`issue.composeFromText`, `prompt.append`, `agent.show`, `issue.show`, and the Tasks page's `task.*` commands) |
| `windowQuery(command, args)` | read something the window holds, answered at once as JSON (`task.agentSessions`, `task.repositoryLogos`) |
| `windowRequest(command, args, done)` | ask the window for work that answers later (`task.startAgent`, `task.fileAsIssue`, `task.workPlan`); `done` gets `ok`, a JSON reply and an error |
| `accountPicture(account)` | an account's profile picture, as the chat shows it; empty when none is loaded |
| `notify(title, body, fileLocally)` | the OS notification card and, when `fileLocally`, a Pings row |
| `recordQuietNotification(title, body)` | a quiet Pings row with no card |
| `setting` / `setSetting` | the package's own settings (`market/packages/<id>/data/...`), removed with the package |
| `attachPackageMenu(button)` | the right-click package menu (settings, export, remove) on a plugin's own button |
| `accountApiUrl(path)` | the relay URL for a path under the contribution's `apiPrefix` or one of its `accountPaths`; invalid otherwise |
| `accountSignedIn()` | whether the desktop is signed in to its account: the one sign-in check (see below) |
| `accountSessionToken(url)` | this account's session token for a relay URL; empty while the desktop is still getting one, which it then starts |
| `requestAccountSession(done)` | get a session for the account if there is none yet; `done` gets an empty error once `accountSessionToken` answers, else the reason to show |
| `signProof(proof, resource, ...)` | the account key's `signer/ts/sig` over a proof; only the contribution's `requestProof` and `resourceProofs` |
| `askAgentWith(provider, prompt, timeoutMs, done)` | one prompt to the local Codex or Claude Code CLI; returns a function that stops it |

#### Checking sign-in

Ask `accountSignedIn()` whether to show a "Sign in" state; never infer it from
an empty `accountSessionToken`. A desktop signed in by its key (the normal
launch, no password typed) has no session at first: it gets one from
`POST /api/accounts/desktop-session` ([sessions](../app/auth-sessions.md#desktop-sessions-from-the-account-key))
right after signing in, and again when a page asks. Until it arrives, a request
either signs with `signProof` (Notes, Calendar, CRM) or waits on
`requestAccountSession` (Spreadsheets, whose web editor needs a real session).
Window code does the same through `ensureAccountSession` and
`waitForAccountSession` (`MainWindowAccounts.cpp`). Checking the token instead
once told signed-in people to sign in to Spreadsheets, runner accounts and
their status.

The window calls a plugin through `NativePackageWidget` virtuals:
`refresh`, `searchHits` / `activateSearchHit`, `activate` (open the plugin's
own surface), and `acceptsHostCommand` / `hostCommand` for the few things the
core asks of whichever plugin offers them by a generic command name (the agent
usage rows add a countdown through `countdown.add`, which Timers accepts; task
links open through `task.open`). A `windowCommand` the window does not own
itself, such as `task.composeFromText` from Notes, goes on to the plugin that
accepts it. Three broadcasts reach every package page: `agentSessionsChanged()`
when a local agent starts, stops or is linked to a task,
`accountPicturesChanged()` when profile pictures arrive, and `themeChanged()`
when the theme changes (for pages that bake colours into rich text).

A plugin may also use core data classes that do not reach the window headers.
Tasks reads and writes the organization task catalog through
`OrganizationTaskStore` (`desktop/src/OrganizationTaskStore.h`), which the
window owns and the page finds with `OrganizationTaskStore::current()`; the
same store answers the agents, quick add and Stats, so they keep working
without the page.

### Top-bar widgets

A contribution with `chromeFactory` instead of `factory` adds a widget to the
window's top bar rather than a page (`MainWindowPackageChrome.cpp`). The
window keeps three slots (left beside the indicators, middle beside search,
right before the view button); the widget says which one it wants
(`chromeSlot`), asks to be moved with `chromeSlotChanged`, maps a drop point
to a slot with `chromeSlotAt`, and adds its entries to the top bar's
right-click menu (`addChromeMenuActions`). The widget is created when the
package is installed here and deleted when it is removed. Its search hits go
to the top search box like a page's. Timers is the first: `TimersClock` is the
clock and its chips; the package's model (`TimersFeature`) was already in the
plugin.

### Pages that draw on the GPU

A page holding a `QWebEngineView` (a `QQuickWidget` inside), a
`QOpenGLWidget` or a `QRhiWidget` needs its top-level window composed by
Qt's RHI. Qt 6 decides a window's surface type when it first creates the
native window, so adding such a child to a window already on screen makes
Qt destroy the native window and create it again (on Wayland the window
vanishes and comes back, because package pages are built after the first
paint). A contribution that puts
one on its page while the page is built sets `gpuSurface` (Face Changer,
Synapse, Video Mix DJ Studio). Before its first show the window calls
`MainWindow::prepareStartupComposition()` (`MainWindowMarket.cpp`): when a
package installed here sets the flag, it adds a hidden `QRhiWidget`, so the
first native window already has the GPU surface and the page joins it later
without a rebuild. With no such package the window stays raster: composing
on the GPU loads the GL driver, which is not worth paying for a window that
never shows GPU content. It is skipped on the `offscreen` and `minimal`
platforms and before Qt 6.7, which has no `QRhiWidget`. A GPU child added
later by a user action (opening the workspace Browser tab in a raster
window) still rebuilds the window once. The side browser restored at start
does not, because the window builds it in its constructor, before the first
show.

To check it, run the window in a sandbox `HOME` on Wayland with
`WAYLAND_DEBUG=client`: count `xdg_surface#N.get_toplevel` lines whose
surface gets a `wl_surface.attach`; the main window ("ForkMesh v<version>")
should have one, not two.

## The empty shell

### Starting empty

The new-project dialog offers a fourth plugin pack, **Empty**: "Only the shell,
the Market and Settings. Nothing runs until you add it from the Market."
`projects::starterPackages(StarterPack::Empty, ...)` returns nothing, and the
dialog seeds every default-installed package (`install.default` and
`installedBefore`) as removed, as it does for the other packs. Everything else
is added from the Market, one plugin at a time.

The home project always counts as a legacy project
([core-plugins](../marketplace/core-plugins.md#projects)), so it keeps every
feature it had; an empty shell is a new project (`forkmesh --project <slug>`,
or the project switcher).

### Core services follow their packages

Removing a switch package stops its timers, sockets and feeds, not just its
buttons. `CorePackageServices` is a small registry of the core's background services. Each service names the
packages that need it and has a start and a stop hook; it runs while at least
one of those packages is shown on this desktop (`packageSurfaceShown`: installed,
with its dependencies, and placed here). `syncPackageRuntime`, which runs on
every install, removal and account sync, calls `syncCorePackageServices`, so a
service starts the moment its package is added and stops the moment the last
one is removed, without a restart. The log says which:
`[core services] started: …; stopped: …; running: …`.

| Service | Packages | Runs |
| --- | --- | --- |
| `agent-watchers` | `agents`, `desktops` | the 2 s machine-agent scan and the 1 s cloud-agent watch |
| `agent-housekeeping` | `agents` | the hourly recycle-bin sweep and the agent/branch/worktree parity watch |
| `local-mesh` | `chat`, `desktops` | the local mesh room (UDP beacons, TCP links, Bluetooth) and with it the phone's LAN control channel |
| `organization-tasks` | `tasks`, `agents` | the one task-catalog read 25 s after start |
| `host-log-feeds` | `hosts`, `logs` | a log stream from every saved host, 15 s after start |
| `fleet-log-feeds` | `logs` | the fleet nodes' log feeds, the fleet inventory and the deploy log follower, 15 s after start |

`startLocalMesh` refuses to start the mesh while its service is off, so the
reachability watch and the session start cannot bring it back behind the
package's back. Stopping it is the same silenced stop the window uses when it
closes. A phone can still reach the desktop through the relay socket
(`onNodeControlFrame`); only the LAN path needs Chat or Desktops.

The World voice bridge is created the first time someone asks for a pairing
code (`createWorldSpeechPairing`), not at start.

#### Left running on purpose

- **Chat retention.** Expired chat history is still pruned every five minutes
  with Chat removed: removing a plugin must not keep messages past their
  expiry.
- **Repositories, mirrors, relay sync and auto-update.** Repos is core, so the
  mirror safety sync, the relay inbox sync and the update check stay.
- **Diagnostics.** The stall watchdog and the footer's diagnostics tick stay;
  they are how a freeze or crash gets reported.

### Qt WebEngine loads on first use

Qt WebEngine is a module on Linux (`packages/web-engine.so`), loaded the first
time the Browser tab or an agent opens a page; linking it cost about 36 MB of
resident memory in every process ([browser](browser.md#the-engine-is-a-module)).

### What is still core

The shell is not empty yet. In order of what it would save:

1. **The session.** `startSession` (5 s after start) builds the whole Chat page
   and connects the relay `ServerNode` even without Chat, because the chat
   models (`m_channelList`, `m_dmList`) are widgets on that page and the relay
   socket also carries presence, pings and agent prompts. Splitting the chat
   models out of the page and starting the relay room only with `chat` is the
   next step; the session start adds about 22 MB.
2. **Feature code in the binary.** About 48 MB of the `forkmesh` binary is
   resident at start. The switch packages (Agents, Chat, Issues, Pull requests,
   Network, Hosts, Stats and the rest) are still window code in `MainWindow*.cpp`;
   each moves out as a module page the way Tasks, Notes and Calendar did
   ([above](#plugins-built-apart-from-the-window)).
3. **Core-linked plugins.** `capture`, `discord`, `ledger` and `tour` are still
   linked into the app ([plugins](../marketplace/plugins.md#plugins-the-core-still-links)).
4. **macOS and Windows** link every plugin and the web engine into the app;
   modules there need a shared plugin SDK library.
5. **Phones** compile every package into the app and hide what is not
   installed ([plugins](../marketplace/plugins.md#phones)).

## Measurements

### Module build

Debug builds with `-O0 -g0`, run offscreen in a fresh sandboxed `HOME` (no
packages installed) on a loaded build box, interleaved A/B: "compiled in" is
`-DFORKMESH_PACKAGE_MODULES=OFF`, "modules" the default.

| | Compiled in | Modules | Change |
| --- | --- | --- | --- |
| `forkmesh` file | 206.4 MB | 188.8 MB (+ 29.7 MB in 15 modules, loaded only when installed) | −8.5 % |
| `forkmesh` text segment | 129.2 MB | 118.6 MB | −8.2 % |
| Resident memory 8 s after start, median of 12 | 178.5 MB | 175.2 MB | −3.3 MB |
| Window shown (`[startup +Nms]`), median of 12 | 608 ms | 632 ms | within noise at this load |
| Editing a module package's source | recompiles the file, relinks the app (2.7 s link) | recompiles the file, relinks only its module | no app relink |

Adding or removing a plugin (configuring with and without `crm` and comparing
the compile commands both builds share) recompiles no app or plugin object,
modules on or off; with every plugin folder on every command line it
recompiled all of them.

### Shell memory

Release build, offscreen, a fresh sandboxed `HOME` and `XDG_*_HOME`
(`env -i`), resident memory (`VmRSS`, `smaps_rollup`) 10 s after start, medians
of three runs:

| Profile | Services always on | Services follow packages | + Qt WebEngine as a module |
| --- | --- | --- | --- |
| Home project (legacy: every feature installed) | 207 MB (PSS 154) | 207 MB (PSS 154) | 179 MB (PSS 142) |
| New project, Standard pack | | 185 MB (PSS 132) | 158 MB (PSS 121) |
| New project, Empty pack | | 183 MB (PSS 130) | 155 MB (PSS 117) |

Gating the services saves work (no 1 s and 2 s agent timers, no mesh beacons,
no log feeds or task reads) more than memory. With Qt WebEngine linked in, the
Empty project holds 66 MB heap and anonymous memory, 48 MB of the `forkmesh`
binary, 18 MB of `libQt6WebEngineCore` plus Qt Quick and QML, and the rest Qt,
OpenSSL and system libraries. A Qt Widgets program that only shows a window is
24 MB, about the floor for a Qt shell.

# Desktop package modules

Part of [the desktop docs](README.md). Code: `desktop/cmake/ForkMeshPackageModules.cmake`,
`desktop/src/PackageModules.{h,cpp}`, `desktop/scripts/check_package_modules.py`.

Optional marketplace packages that only talk to the window through
`NativePackageRegistration` are built as separate shared modules instead of
being linked into `forkmesh`. The goal is a desktop that is an empty shell
plus plugins ([shell.md](shell.md)); the marketplace side is in
[../marketplace/plugins.md](../marketplace/plugins.md).

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
| `accountSessionToken(url)` | this account's session token for a relay URL, empty when signed in by key only |
| `signProof(proof, resource, ...)` | the account key's `signer/ts/sig` over a proof; only the contribution's `requestProof` and `resourceProofs` |
| `askAgentWith(provider, prompt, timeoutMs, done)` | one prompt to the local Codex or Claude Code CLI; returns a function that stops it |

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

## Measurements

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

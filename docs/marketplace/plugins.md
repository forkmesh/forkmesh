# Plugins and the core

Part of [the marketplace docs](README.md). Code: `marketplace/plugins.json`,
`marketplace/tools/marketplace_lib.py` (`plugin_selection` and the
`selected_*` helpers), `marketplace/tools/generate_desktop_packages.py`,
`desktop/cmake/ForkMeshPackageModules.cmake`, `app/tools/build_app_plugins.py`,
`marketplace/tools/generate_world_packages.py`.

Until October 2026 every catalogued package was built into every host: the
desktop compiled all of them into the `forkmesh` binary (17 were split into
modules on Linux only), the App staged every package's Worker module into its
own source tree, and the desktop binary embedded every package's files. A
package was an on/off switch over code that was always there. The owner asked
for real plugins: separate parts with their own setup, and a core that holds
none of them unless a build explicitly adds them.

## Plugins and switches

A package is a **plugin** when it has a `content/` folder: code or material
that some host builds or ships (desktop sources from `content/desktop/build.json`,
a Worker module, dashboard or `www` pages, World modules, themes, agent
descriptors, Workflows packs). There are 90.

A package without `content/` is a **switch**: it only turns a core feature on
or off (Chat, Issues, Agents, the repository tabs, see
[core-plugins.md](core-plugins.md)). There are 38. Switches are part of the
core catalog in every build, because the code they gate is core code.

## The plugin list

`marketplace/plugins.json` lists the plugins a ForkMesh build adds to the core.
A plugin that is not listed is not part of any host: no code, no files, no
catalog entry, no Market listing. Adding a package to the catalog does not add
it to a build; listing it here does. Today the list holds every plugin, so the
shipped product is unchanged.

Every host takes the same override:

| Value | Plugins added |
| --- | --- |
| `default` (or empty) | `marketplace/plugins.json` |
| `none` | none: the core alone |
| `all` | every plugin in the catalog |
| `crm;water` (`,`, `;` or spaces) | exactly those |

- Desktop: `cmake -DFORKMESH_PLUGINS=…` (cached, default `default`).
- App, World: the `FORKMESH_PLUGINS` environment variable, plus
  `FORKMESH_EXCLUDED_PLUGINS` (the instance installer's list) which takes
  plugins out afterwards.

Rules, enforced by `plugin_selection`:

- A listed id must be a plugin in the catalog; naming a switch or an unknown
  id fails the build.
- A plugin's plugin dependencies must be listed too (`status-check-*` need
  `status-page`). Switch dependencies are always there.
- An excluded plugin also takes out every listed plugin that depends on it.

Packs and guides keep only the packages a build contains; a pack left empty is
dropped, and so is a guide whose pack is gone.

## Desktop

- The generator writes the build's own `catalog.json`, `packs.json` and
  `guides.json` into the build folder and embeds those, not the source files.
- The core's `marketplace.qrc` holds the catalog, the switches, the
  core-linked plugins and only the **listing files** (`manifest.json`,
  `preview/`, `README.md`, `LICENSE`) of module plugins, so the Market can
  list them. Everything else a module plugin ships (its `content/`, `data/`,
  `repositoryResources`) is compiled into its own module from
  `generated/marketplace/plugins/<id>.qrc` and registered when the module
  loads.
- Every listed plugin with `content/desktop/build.json` that the core does not
  call is built as its own module (`packages/<id>.so` and sidecar,
  [package-modules](../desktop/package-modules.md)): 37 today, against 17
  before. The hand-kept module list is gone; the generator emits
  `FORKMESH_PLUGIN_MODULE_CANDIDATES`.
- Desktop extensions (Sentry, VSCodium) register themselves with
  `DesktopPackageExtensionRegistration`, like page plugins do with
  `NativePackageRegistration`. The generated factory table that made the core
  call `createSentryDesktopExtension()` by name is gone, so both are modules
  now.
- `AgentNetworkSampler` moved from agent-watch to `desktop/src`: the core's
  local control insights use it, so it is core code. Agent Watch is a module.
- Agent Watch, Analytics and Docs are installed by default, so their modules
  load at start (the 2026-10-15 measurement put that at about 30 ms and 3 MB
  together). They were compiled in to save that; being real plugins was the
  owner's call.
- Qt WebEngine is a core module the same way (`packages/web-engine.so`, not a
  Market package), loaded the first time a page opens
  ([browser](../desktop/browser.md#the-engine-is-a-module)); the empty shell
  and what is still core are on [shell](../desktop/shell.md).
- macOS and Windows still link listed plugins into the app
  (`FORKMESH_PACKAGE_MODULES` defaults off there): a module resolves the
  window's symbols from the executable, which works through `-rdynamic` on
  Linux but needs a shared plugin SDK library on Windows. Unlisted plugins
  are left out on every platform.
- **The Market card shows it** (2026-10-30). Every package with desktop code
  has a line under its byline: **Separate from the core · 6.4 MB in memory**
  (green) for a loaded module, **· not loaded yet** / **· no memory until
  installed** for one that is not mapped, **· not loaded** with the reason in
  the tooltip for a refused one, and **Built into the core** (muted) for a
  core-linked plugin or any plugin on macOS and Windows. The figure is the
  `Rss` of the module file's own mappings in `/proc/self/smaps`
  (`modules::residentBytesByPackage`), read once per Market refresh: its code
  and static data. Heap the plugin allocates while running cannot be told
  apart from the app's, so the tooltip says it is counted with the app;
  Linux only, elsewhere a loaded module just says "loaded". An installed
  package with a page also gets an **Open** button beside Settings (in the
  Next card's status row and its details pane too) that shows the page, as
  its rail button does.

### Plugins the core still links

`build.json` `"coreLinked": true` marks a plugin the window calls directly. The
generator always adds these, even with `FORKMESH_PLUGINS=none`, and configure
prints them. `marketplace/tests/test_plugin_selection.py` pins the list, so it
can only shrink.

| Plugin | Why the core links it |
| --- | --- |
| `capture` | `MainWindowMarket.cpp` creates `CaptureFeature` |
| `discord` | `MainWindowSetup.cpp` runs `DiscordClient` and shows Discord rooms in Chat |
| `ledger` | `MainWindowLedger.cpp` (about 3,700 lines, payouts and tip transfers) uses `LedgerFeature` |
| `tour` | `MainWindowOnboarding.cpp` creates `OnboardingTour` |

Each one moves out by turning the window code that drives it into the plugin's
own page or a socket in `NativePackageHost`, then dropping `coreLinked`.
Timers moved out on 2026-10-25: its clock is a top-bar widget the plugin
registers (`chromeFactory`), so it is a module now
([package-modules](../desktop/package-modules.md#top-bar-widgets)).

Tasks, Notes and Calendar were never `coreLinked`, but their desktop pages
were window code (`MainWindowTasks.cpp`, `MainWindowNotes.cpp`,
`MainWindowCalendar.cpp`) that every change to `MainWindow.h` recompiled. They
are module pages now (`tasks.so`, `notes.so`, `calendar.so`); for Tasks the
catalog and its signed requests stayed in the core as `OrganizationTaskStore`
because agents, search, quick add and Stats read it
([tasks-page](../desktop/tasks-page.md#page-and-window)).

## App

- `build_app_plugins` uses the list for everything it stages: dashboard and
  `www` pages, `worker.http` routes, the generated `PLUGINS` registry,
  `MARKET_PACKAGES`, and the Worker modules. Before, `FORKMESH_EXCLUDED_PLUGINS`
  only hid pages: an instance built without CRM still shipped `crm.py` and
  answered `/api/crm`.
- Worker modules are staged into `app/src_build/plugins/`, not beside the core
  modules. `forkmesh_host.paths.ensure_import_paths` puts that folder last on
  `sys.path`, after the core, and `plugin_data._module` adds it when missing.
  A plugin module named like a core module fails the build.
- A plugin that is not in the registry is never loaded: `plugin_data` only
  loads modules the registry names.

## World

`generate_world_packages.py` writes only listed plugins into
`world-marketplace-packages.js`.

## Phones

Not changed yet. The iPhone and Android apps compile every package's screens
into the app (Swift area modules under `ForkMeshCore`, Android `:app` and
`:core`) and `PackageGate` hides what is not installed. Apps on the stores
cannot load code at run time, so plugins there have to be chosen at build time:
one Swift package target and one Gradle module per plugin, assembled from
`plugins.json`.

## Still in the core

- The App's `workflows.py` is the Workflows plugin's server code living in
  core, and `marketplace_reviews.FIRST_PARTY_PACKAGES` names every package by
  hand.
- `pitch_deck_runtime.py` links to `/dashboard/crm`.

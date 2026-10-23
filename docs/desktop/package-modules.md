# Desktop package modules

Part of [the desktop docs](README.md). Code: `desktop/cmake/ForkMeshPackageModules.cmake`,
`desktop/src/PackageModules.{h,cpp}`, `desktop/scripts/check_package_modules.py`.

Optional marketplace packages that only talk to the window through
`NativePackageRegistration` are built as separate shared modules instead of
being linked into `forkmesh` (`cmake/ForkMeshPackageModules.cmake`,
`src/PackageModules.{h,cpp}`).

- **Build.** Every plugin listed for the build ([plugins.md](../marketplace/plugins.md))
  that has `content/desktop/build.json` and is not `coreLinked` becomes
  `build/packages/<id>.so` with a sidecar `<id>.json` (`api`, `abi`,
  `package`, `file`, `entrypoints`) when `FORKMESH_PACKAGE_MODULES` is on
  (default on for Linux, off elsewhere, where listed plugins are linked into
  the app). The list comes from the generator
  (`FORKMESH_PLUGIN_MODULE_CANDIDATES`); there is no hand-kept list any more.
  The module also carries the plugin's own files (`plugins/<id>.qrc`); the
  core keeps only its listing files. Configure removes module files the
  configuration no longer builds. Modules resolve the app's symbols through
  the `-rdynamic` export the stall watchdog already needs, so the app binary
  does not grow. Installed-by-default plugins (agent-watch, analytics, docs)
  are modules too and load at start (about 30 ms and 3 MB together, see
  [Measurements](#measurements)).
- **Include folders.** A plugin's sources see `desktop/src` and their own
  `content/desktop` folder only (a source property from the generator's
  `FORKMESH_PACKAGE_<ID>_INCLUDE_DIRS`); the app target sees `desktop/src` and
  the core-linked plugins' folders. Until 2026-10-23 the app and every module
  got all 42 plugin folders on their command lines, so adding or removing any
  plugin (about once a day that month) changed every compile command and
  rebuilt the whole app and every plugin, modules on or off. Now it changes
  none: the configure comparison in [Measurements](#measurements) shows it.
  No plugin includes another plugin's headers, and the core includes only the
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
  and removes ones the build no longer produces; Rebuild & restart and auto-update do the same beside the installed app (`modules::installBeside`); `cmake --install`
  puts them in `<libdir>/forkmesh/packages`.
- **Test.** `ctest -R forkmesh-package-modules-resolve` checks that every
  module's undefined symbols are provided by the app or its own libraries, so a
  new window-to-package reference fails the build's tests instead of the user's
  start.

## Measurements

Taken 2026-10-15 on the 36-core Linux build box while other agents were
building (load average 9–33). Debug builds with `-O0 -g0`, the same stage, run
offscreen in a fresh sandboxed `HOME` (no packages installed), interleaved
A/B: "compiled in" is `-DFORKMESH_PACKAGE_MODULES=OFF`, "modules" the default.

| | Compiled in | Modules | Change |
| --- | --- | --- | --- |
| `forkmesh` file | 206.4 MB | 188.8 MB (+ 29.7 MB in 15 modules, loaded only when installed) | −8.5 % |
| `forkmesh` text segment | 129.2 MB | 118.6 MB | −8.2 % |
| Resident memory 8 s after start, median of 12 | 178.5 MB | 175.2 MB | −3.3 MB |
| Window shown (`[startup +Nms]`), median of 12 | 608 ms | 632 ms | within noise at this load |
| Editing a module package's source | recompiles the file, relinks the app (2.7 s link) | recompiles the file, relinks only its module | no app relink |

Adding or removing a plugin, measured 2026-10-23 by configuring the same
checkout with the default plugin list and with `crm` left out, then comparing
the compile commands of every object both builds share (a changed command is a
recompile under Ninja):

| | App objects recompiled | Plugin objects recompiled |
| --- | --- | --- |
| Modules on, before | 411 of 411 | 139 of 139 |
| Modules on, after | 0 of 411 | 0 of 139 |
| Modules off (macOS, Windows), before | 492 of 492 | (in the app) |
| Modules off, after | 0 of 492 | (in the app) |

An earlier list that also modularised the three packages installed by default

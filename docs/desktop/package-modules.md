# Desktop package modules

Part of [the desktop docs](README.md). Code: `desktop/cmake/ForkMeshPackageModules.cmake`,
`desktop/src/PackageModules.{h,cpp}`, `desktop/scripts/check_package_modules.py`.

Optional marketplace packages that only talk to the window through
`NativePackageRegistration` are built as separate shared modules instead of
being linked into `forkmesh` (`cmake/ForkMeshPackageModules.cmake`,
`src/PackageModules.{h,cpp}`).

- **Build.** `FORKMESH_PACKAGE_MODULES` (default on for Linux, off elsewhere)
  and `FORKMESH_PACKAGE_MODULE_IDS` (default: app-store-status, aquarium,
  backups, calories, cleanup, computer-security, crm, marketing, people,
  rss-reader, sleep, steps, stretch, sugar-break, voice-changer, water). Disk
  Cleanup reaches the window only through its host socket and
  `NativePackageRegistration`, and loads in under a millisecond when
  installed. Each becomes
  `build/packages/<id>.so` with a sidecar `<id>.json` (`api`, `abi`, `package`,
  `file`, `entrypoints`); configure removes module files the configuration no
  longer builds. Modules resolve the app's symbols through the `-rdynamic`
  export the stall watchdog already needs, so the app binary does not grow.
  Stay compiled in: packages installed by default (agent-watch, analytics,
  docs: as modules they were loaded at every start and cost about 30 ms and
  3 MB, see [Measurements](#measurements)), packages with a `desktopFactory`
  (sentry, vscodium), and the ones the window references directly. Sources the
  window calls from a modular package stay in the app through
  `FORKMESH_PACKAGE_HOST_SHARED_SOURCES` (agent-watch's
  `AgentNetworkSampler.cpp`, for builds that list agent-watch).
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

An earlier list that also modularised the three packages installed by default

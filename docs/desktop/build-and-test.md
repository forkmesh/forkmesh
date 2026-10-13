# Desktop: build, rebuild and test

Part of [the desktop docs](README.md). The user-facing build guide with
per-distribution package commands is
[www/docs/qt-client.md](../../www/docs/qt-client.md#build-and-run-from-source).

## Requirements

From `desktop/CMakeLists.txt`:

- Qt 6.4 or newer: Widgets, Network, Svg, Concurrent (required); DBus on Linux
  (required); Multimedia/MultimediaWidgets, OpenGL/OpenGLWidgets, Bluetooth,
  WebEngineWidgets (optional, found `QUIET`). WebEngine (`qt6-webengine-dev`)
  gives agents in-app browser windows
  ([../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md)); without it
  those pages open in the system browser.
- OpenSSL 3.0 or newer; 3.5 for every feature. See [OpenSSL](#openssl).
- Python 3 (the build generates marketplace sources from `marketplace/`).
- Optional: `libnghttp3` via pkg-config plus OpenSSL QUIC for the HTTP/3 client
  (`FORKMESH_HAVE_HTTP3`), `libwebp`, `ninja`, `mold`, `ccache`.

## OpenSSL

The build compiles against OpenSSL 3.0 headers. Two features need algorithms
that only OpenSSL 3.5 provides, fetched at run time:

| Feature | Needs | Code |
| --- | --- | --- |
| Private mirrors and owner-sealed envelopes (agent sync to the owner's other devices) | ML-KEM-768 (3.5), hybrid with X25519 | `src/MirrorCrypto.cpp` |
| Password Vault (suite `fmv1`, shared with the browser and phone) | Argon2id (3.2) and ML-KEM-768 (3.5) | `marketplace/packages/vault/content/desktop/VaultCrypto.cpp` |
| HTTP/3 host log feeds (optional) | QUIC client (3.2) and nghttp3 | `src/Http3Get.cpp` |

There is no weaker fallback: the envelopes and vault records are a
cross-client format, so a node without ML-KEM cannot read or write them.
Ubuntu 24.04 and Debian 12 ship OpenSSL 3.0, so there are three ways to build:

- **System OpenSSL 3.0.** Configure prints a warning and the build runs. Private
  mirrors and the Password Vault fail closed: `MirrorCrypto::postQuantumAvailable()`
  and `forkmesh::vault::cryptoAvailable()` report the missing algorithm and the
  loaded OpenSSL version, and nothing is generated, sealed, opened or unlocked.
  Everything else (public repositories, chat, agents, rooms) works. The same
  check covers a binary built against 3.5 that loads an older `libcrypto.so.3`
  at run time (same soname).
- **`-DFORKMESH_BUNDLED_OPENSSL=ON`.** Configure runs `tools/openssl-local.sh`,
  which downloads OpenSSL 3.5.8 (pinned by SHA-256, the same tarball the
  Windows toolchain uses), builds it shared once into
  `<toolchain>/openssl/openssl-<version>-<os>-<arch>` with the system
  `OPENSSLDIR` (so the system CA store and `openssl.cnf` still apply), and links
  it with an RPATH. The option is cached, so run it once and `desktop/run.sh`
  keeps it:
  `cmake -S desktop -B desktop/build -DFORKMESH_BUNDLED_OPENSSL=ON`.
  `FORKMESH_OPENSSL_VERSION`/`FORKMESH_OPENSSL_SHA256` pin another release,
  `FORKMESH_OPENSSL_DIR` moves the output, `FORKMESH_OPENSSL_TARBALL` supplies
  the tarball offline (still hash-checked). The binary depends on that
  directory staying in place.
- **`-DOPENSSL_ROOT_DIR=<prefix>`** for an OpenSSL 3.5 you installed yourself
  (Homebrew's `openssl@3` on macOS).

`-DFORKMESH_REQUIRE_OPENSSL_3_5=ON` turns the warning into a configure error.
Every release configure passes it (`.forkmesh/release.yml`, the Windows
cross-build in `tools/forkmesh-release-host.py`, the `macos-desktop` workflow
in `codemagic.yaml`), so a published binary is never built without ML-KEM.

Prebuilt Linux binaries (1.2.5 and later) are built in a Debian 12 container
with OpenSSL 3.5.8 and Qt 6.8.2 linked statically, so neither the plain
`forkmesh-linux-x86_64` nor the AppImage uses the system `libcrypto` or Qt;
they run on Ubuntu 22.04+ and Debian 12+. See
[`docs/release/linux-baseline.md`](../release/linux-baseline.md). Releases up
to 1.2.4 were built on Debian 13 and need `GLIBC_2.38` and the system Qt 6.8.

## `desktop/run.sh`

```sh
./desktop/run.sh            # (or "run") configure Release, build incrementally, launch
./desktop/run.sh test       # configure with tests ON, build critical-tests, run tools/run_critical_tests.py qt
./desktop/run.sh clean      # rm -rf desktop/build
./desktop/run.sh rebuild    # clean, build, launch
```

What it does (`desktop/run.sh`):

- Records the caller's directory in `FORKMESH_START_DIRECTORY` (the KVM
  workspace mounts exactly that directory).
- Picks Ninja when installed; migrates a Make tree to Ninja in place by
  deleting only generator files.
- Sizes `--parallel` from free memory: 6 GiB reserved, 2 GiB per job, at most
  12 (same figures as `ramCappedBuildJobs()` in `src/MainWindowInternal.h`).
  CMake also declares a heavy-compile job pool so Ninja caps the giant
  `MainWindow*.cpp` units itself (`FORKMESH_HEAVY_JOBS`, override with
  `FORKMESH_HEAVY_JOBS_OVERRIDE`).
- Detects a `CMakeCache.txt` generated at another path and wipes `build/`.
- Configures `-DFORKMESH_BUILD_TESTS=OFF` for `run`; the option defaults to ON,
  and a cached ON used to make every run compile all test programs.

## Machine-wide compile gate

Every compile and link of the desktop goes through
`desktop/cmake/forkmesh-compile-gate.sh` (Linux with `flock`; CMake puts it in
front of ccache in `CMAKE_CXX_COMPILER_LAUNCHER` and, on CMake 3.21+, in
`CMAKE_CXX_LINKER_LAUNCHER`). Test: `forkmesh-compile-gate`
(`desktop/tests/validate_compile_gate.py`).

- **Why.** `run.sh`, `ramCappedBuildJobs()` and the `forkmesh_heavy` pool each
  size one build. With several agents building their worktrees at once, every
  build saw free memory at its start and took its full share: on 2026-09-27 the
  stall log showed load 57 with 1 GB of 31 GB available and 5.6 GB in swap, and
  the OOM killer ran three times that morning. An agent's plain `ninja` or
  `cmake --build` used Ninja's default of cores + 2 jobs on top of that. Ninja
  1.12 has no jobserver, so the builds cannot share a budget any other way.
- **What it does.** A job takes one of N slot files under
  `$XDG_RUNTIME_DIR/forkmesh-compile-gate-<uid>/` with `flock -n` and holds it
  until the compiler or linker exits (the locked descriptor is inherited by the
  `exec`ed command). N is the same budget as the heavy pool: (RAM - 6 GiB) /
  2 GiB, at most the core count and 12. Any slot but slot 0 also waits until
  `MemAvailable` is at least 2 GiB, so under pressure builds drop to one job
  at a time instead of pushing the machine into swap; slot 0 always runs, so a
  build never stalls outright. Waiting jobs poll every 0.2 s.
- **Effect.** Builds from any number of worktrees share one set of 12 slots on
  a 32 GiB box, so more agents can build at once without paging; each build
  just gets a share. A job's `-j` no longer matters for memory.
- **Knobs.** `FORKMESH_COMPILE_SLOTS` (slot count),
  `FORKMESH_COMPILE_MIN_FREE_MB` (default 2048), `FORKMESH_COMPILE_GATE_DIR`,
  and `FORKMESH_COMPILE_GATE_OFF=1` to bypass it. macOS, Windows and machines
  without `flock` build ungated. A build tree picks the gate up on its next
  build, because Ninja reconfigures when `CMakeLists.txt` changes.
- **Agents.** The gate limits desktop compiles only. Agent memory containment
  ([../agents/memory.md](../agents/memory.md)) puts every agent and the
  `systemd-run --user` builds it starts under one `forkmesh.slice` memory
  ceiling, which also covers Gradle, Python and other projects' builds.

## Rebuild & restart (in the app)

The rail's **Restart** button and **Settings → Rebuild & restart**
(`MainWindow::quickRebuildRestart()`, `src/MainWindowSettings.cpp`) rebuild the
running checkout with `RelWithDebInfo` (`-g1` line tables for `addr2line`) and
relaunch. It queues behind running agents (`anyAgentRunning()`,
`maybeStartQueuedRebuild()`), checks the sources after each pass and runs the
pass again if a merge changed them mid-build (up to three passes), and installs
only a build that finished on an unchanged tree.

**Update, rebuild & restart** (`MainWindow::updateRebuildRestart()`,
`src/MainWindowSetup.cpp`) pulls the source from the mirror that
`/api/install-source` names before it builds. It runs `git pull --ff-only`.
If that fails (a busy mirror, a 503, local commits), the update stops, says
why and changes nothing. It clones only when there is no checkout, and only
into a missing or empty folder. It never deletes the checkout, because
`src/.git` holds every local branch and `src/.worktrees` holds the agents'
worktrees. `app/tests/test_desktop_update_keeps_checkout.py` guards this.

## Why rebuilds are slow and what keeps them down

- The precompiled header includes `src/MainWindowInternal.h` and
  `src/MainWindow.h`. Touching either recompiles every `MainWindow*.cpp`
  (~79 units). Cost is optimizer/codegen, not parsing.
- `MainWindowInternal.h` helper bodies were moved into
  `src/MainWindowInternal1.cpp` … `src/MainWindowInternal4.cpp` (2026-09-22).
  Add new helpers there with a declaration in the header.
- The build commit id is in a generated `ForkMeshBuildCommit.h` included only by
  `src/main.cpp` and `src/MainWindowHostInstall.cpp`, so a new commit does not
  recompile the heavy units. `FORKMESH_VERSION` is deliberately not a cache
  variable and not on compile definitions.
- Install `ccache` (give it room: `ccache -M 30G`), `ninja`, and `mold`
  (a copy in `~/.local/bin` is enough; CMake uses it only after a test link
  succeeds).

## Manual CMake and ctest

```sh
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=ON
cmake --build desktop/build --target forkmesh            # the app (the only target that compiles src/main.cpp)
cmake --build desktop/build --target critical-tests      # what CI builds
ctest --test-dir desktop/build -L critical --output-on-failure
cmake --build desktop/build --target check               # builds the test set and runs all of ctest
```

Test targets are `EXCLUDE_FROM_ALL`; ctest reports "Not Run" for any that were
never built. The `critical` label (timeout 20 s) is the set in
`FORKMESH_CRITICAL_TESTS`; `critical-tests` is the aggregate target that builds
their binaries. Focused window suites (`forkmesh-mirror-fleet-window-tests`,
`forkmesh-diff-stream-window-tests`, …) are flag-selected runs of the
`forkmesh-window-tests` binary.

Tests run offscreen (`QT_QPA_PLATFORM=offscreen`). `forkmesh-e2e` drives real
Git with the stub agent `forkmesh-e2e-stub-agent`
(`FORKMESH_E2E_STUB_AGENT`). `tests/validate_*.py` are source-level contract
checks registered as ctest cases. The HTTP/3 probe:
`cmake --build desktop/build --target forkmesh-http3-probe` then
`desktop/build/forkmesh-http3-probe https://www.cloudflare.com/cdn-cgi/trace`.

Many unit targets link only Qt Core or Widgets and build in seconds; iterate on
those before building the app.

## CI

- `.forkmesh/desktop-ci.yml` (push, pull request) builds and runs the critical
  Qt contracts. It is separate from `.forkmesh/ci.yml` so the Worker deploy
  does not wait on a desktop compile, and it gates nothing.
- CI never builds the `forkmesh` app target. `forkmesh-window-tests` (a
  critical target) compiles every `MainWindow*.cpp`, but `src/main.cpp` and the
  app link are only checked by building the app locally.
- `.forkmesh/release.yml` and `tools/package/*.sh` produce release binaries
  (see [../release/README.md](../release/README.md) and
  [windows.md](windows.md)).
- `tools/qt64-build-check.sh [ubuntu-24.04|debian-12]` builds against those
  distributions' Qt 6.4.2 and OpenSSL 3.0 in a private sysroot under the
  toolchain directory.

## Known failures and traps

- `forkmesh-window-tests`, `forkmesh-perf-tests` and `forkmesh-e2e` have failed
  on unmodified main in agent sandboxes (timing under load, missing identity,
  offscreen sizing). Compare against main before treating a failure as yours.
- Under parallel agent load, high `-j` gets `cc1plus` OOM-killed ("Terminated
  signal"), which reads like a compiler crash. Use one `ninja -j2`/`-j3` with
  all wanted targets; never run two builds in one build dir.
- A build that started before a worktree was reset compiled the old tree; a
  green result proves nothing until `git status` shows your diff.
- The agent sandbox can hit the PCH "redefinition" error when a merge rewrites
  a header mid-build; Rebuild & restart handles this with its extra passes, a
  manual build should just be re-run.

## macOS build: no SOCK_CLOEXEC (2026-09-30)

`desktopEndpointAcceptsConnections` (`src/DesktopDisplayReadiness.cpp`)
created its probe socket with `SOCK_CLOEXEC | SOCK_NONBLOCK`, which exist only
on Linux, so the v1.2.6 macOS build stopped there. It now sets both with
`fcntl` after `socket()`, which every Unix supports. Code under `Q_OS_UNIX`
must stay POSIX; Linux-only calls go under `Q_OS_LINUX`.

## macOS package: forkmesh-term not built (2026-09-30)

`tools/package/macos_universal_on_mac.sh` built only the `forkmesh` target,
but the bundle install rule also installs `forkmesh-term` into
`ForkMesh.app/Contents/MacOS`, so `cmake --install` stopped with "file INSTALL
cannot find .../forkmesh-term". The script now builds both targets. Any new
target with a macOS `install()` rule must be added to that ninja line.

## Actions workspace clone: CPU budget (2026-10-01)

Runs 146-148 failed with "Isolated workspace exceeded the process time limit"
about 6 minutes in: `git clone --no-local` re-ran pack-objects' delta search
over the whole history on every thread, and the step's `RLIMIT_CPU`
(`ActionSandboxLimits::maxCpuSeconds`, 20 CPU minutes summed over threads)
killed it. After the 2026-09-30 history rewrite the source repository holds
11 packs (3.2 GiB) whose objects are not deltified against each other, which
made the search far longer. The workspace clone now passes
`--upload-pack=git -c pack.window=0 -c pack.threads=2 upload-pack`: existing
deltas are still reused, no new delta search runs, and the local transfer is
slightly larger. Measured under load average ~50: 24 s wall, ~2 CPU minutes.

## Actions workspaces are shallow by default (2026-10-01)

A workflow's isolated workspace is cloned `--depth=2` unless the workflow sets a
top-level `fetch-depth`: a positive number clones that many commits and `0`
clones the full history. Depth 2 covers a release-metadata commit sitting on
its code commit. Only `.forkmesh/release.yml` sets `fetch-depth: 0`, because it
stamps the build with `git log -1 -- . ':(exclude).forkmesh/releases/**'`, the
newest code commit, which may lie further back on a re-run. The clone keeps
`pack.window=0` so a full-history clone also stays inside the step's CPU budget.

## Windows: `environ` is a macro (2026-10-01)

The v1.2.6 Windows installer build stopped in `MachineAgentScan.cpp`:
"cannot declare pointer to `const QByteArrayList&`". MinGW's `stdlib.h`
defines `environ` as a macro (`(* __p__environ())`), so a parameter or local
named `environ` breaks the Windows build while Linux and macOS compile it. The
parameter is now `environment` and the local `processEnvironment`. Never name
a C++ identifier `environ`.

The same night `ProcessReaper` (`411dd83d789`, after the tag) added
`instanceFromEnviron(const QByteArray &environ)`, which broke 17 Windows
translation units through `ProcessReaper.h`. It and its test are renamed, and
`tests/validate_no_environ_identifier.py` (ctest
`forkmesh-no-environ-identifier`) now fails on any C++ variable or parameter
declared as `environ` under `desktop/src`, `desktop/tests` or
`desktop/packages`. It skips string literals, raw strings and comments, and it
allows the real POSIX global (`extern char **environ;` and its uses). Linux
and macOS builds can't see this mistake, so the check is the only warning
before the Windows release build.

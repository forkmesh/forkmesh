# Desktop: build, rebuild and test

Part of [the desktop docs](README.md). The user-facing build guide with
per-distribution package commands is
[www/docs/qt-client.md](../../www/docs/qt-client.md#build-and-run-from-source).

## Requirements

From `desktop/CMakeLists.txt`:

- Qt 6.4 or newer: Widgets, Network, Svg, Concurrent (required); DBus on Linux
  (required); Multimedia/MultimediaWidgets, OpenGL/OpenGLWidgets, Bluetooth,
  WebEngineWidgets (optional, found `QUIET`). Without WebEngine, agent browser
  windows ([../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md)) open
  in the system browser.
- OpenSSL 3.0 or newer; 3.5 for every feature. See [OpenSSL](#openssl).
- Python 3 (the build generates marketplace sources from `marketplace/`).
- Optional: `libnghttp3` plus OpenSSL QUIC for the HTTP/3 client
  (`FORKMESH_HAVE_HTTP3`), `libwebp`, `ninja`, `mold`, `ccache`.

## OpenSSL

The build compiles against OpenSSL 3.0 headers. Some features need algorithms
fetched at run time:

| Feature | Needs | Code |
| --- | --- | --- |
| Private mirrors and owner-sealed envelopes | ML-KEM-768 (3.5), hybrid with X25519 | `src/MirrorCrypto.cpp` |
| Password Vault (suite `fmv1`) | Argon2id (3.2) and ML-KEM-768 (3.5) | `marketplace/packages/vault/content/desktop/VaultCrypto.cpp` |
| HTTP/3 host log feeds (optional) | QUIC client (3.2) and nghttp3 | `src/Http3Get.cpp` |

There is no weaker fallback: these are cross-client formats. Three ways to build:

- **System OpenSSL 3.0.** Configure warns; private mirrors and the Password
  Vault fail closed (`MirrorCrypto::postQuantumAvailable()`,
  `forkmesh::vault::cryptoAvailable()` report the missing algorithm). The same
  check covers a binary that loads an older `libcrypto.so.3` at run time.
- **`-DFORKMESH_BUNDLED_OPENSSL=ON`.** `tools/openssl-local.sh` downloads a
  pinned, hash-checked OpenSSL 3.5 release, builds it shared once into the
  toolchain directory with the system `OPENSSLDIR`, and links it with an RPATH.
  `FORKMESH_OPENSSL_VERSION`/`FORKMESH_OPENSSL_SHA256` pin another release,
  `FORKMESH_OPENSSL_DIR` moves the output, `FORKMESH_OPENSSL_TARBALL` supplies
  the tarball offline.
- **`-DOPENSSL_ROOT_DIR=<prefix>`** for a self-installed OpenSSL 3.5.

`-DFORKMESH_REQUIRE_OPENSSL_3_5=ON` turns the warning into a configure error;
every release configure passes it. Prebuilt Linux binaries link OpenSSL 3.5 and
Qt statically in an older-distribution container
([../release/linux-baseline.md](../release/linux-baseline.md)).

## `desktop/run.sh`

```sh
./desktop/run.sh            # (or "run") configure Release, build incrementally, launch
./desktop/run.sh test       # configure with tests ON, build critical-tests, run tools/run_critical_tests.py qt
./desktop/run.sh clean      # remove desktop/build
./desktop/run.sh rebuild    # clean, build, launch
./desktop/run.sh --quiet    # any mode without the step and timing log (also FORKMESH_RUN_QUIET=1)
```

It records the caller's directory in `FORKMESH_START_DIRECTORY`, prefers Ninja,
sizes `--parallel` from free memory (6 GiB reserved, 2 GiB per job, at most 12;
same as `ramCappedBuildJobs()`), and configures `-DFORKMESH_BUILD_TESTS=OFF` for
`run`. CMake also declares a heavy-compile job pool for the `MainWindow*.cpp`
units (`FORKMESH_HEAVY_JOBS`, override `FORKMESH_HEAVY_JOBS_OVERRIDE`).

### Step and timing log

`run`, `rebuild` and `test` print the same log as the desktop's Rebuild &
restart (`buildAndRelaunch`, `runUpdateStep` and `setUpdateStatus` in
`MainWindowSetup.cpp`, `logRestart` in `MainWindowShared.cpp`), so a terminal
build reads like the update log:

- on stderr, `[restart +NNNNNms] ...` lines timed from the start of the script:
  the mode started, each status, `run: <command>`, `done in Nms (exit N):
  <command>`, `build pass 1: N parallel jobs, HEAD <sha>`, a `summary:` line
  and `launching <binary>`;
- on stdout, `==> Configuring...` / `==> Rebuilding...` / `==> Launching...`,
  `$ <command>` with `(in <dir>)` before each step, the step's own output, and
  `— finished in Nms (exit N)` after it;
- `==> Built in <total> (configure <t>, build <t>)` before the launch (or
  `==> Failed in ...`).

A failed step prints `!! Build failed: <reason>` on stderr and exits with the
step's code. The reason is picked like `updateFailureSummary()`: a full disk,
else the first line containing `error:` or starting `fatal:` (300 characters),
else the output's tail. Each step's output is also kept in
`desktop/build/run-sh-last-step.log`.

The desktop's log is C++ in the running app and run.sh is a shell script, so the
two cannot share one implementation; `app/tests/test_desktop_run_sh_restart_log.py`
pins the shared wording against the C++ strings and runs run.sh with a fake
`cmake` to check the order, the quiet mode and the failure line. run.sh does not
copy the parts of Rebuild & restart that it never did: it still configures every
time, builds once (no extra pass when the sources move), and does not build the
Go mirror-node companion, install, or record restart history for the countdown.

## Machine-wide compile gate

Every compile and link goes through `desktop/cmake/forkmesh-compile-gate.sh`
(Linux with `flock`; placed in `CMAKE_CXX_COMPILER_LAUNCHER` and
`CMAKE_CXX_LINKER_LAUNCHER`). Test: `forkmesh-compile-gate`
(`desktop/tests/validate_compile_gate.py`).

Concurrent builds in several worktrees would each size themselves from free
memory and together exhaust it. The gate makes them share one budget: a job
takes one of N slot files under `$XDG_RUNTIME_DIR/forkmesh-compile-gate-<uid>/`
with `flock -n` and holds it until the compiler exits. N is
(RAM - 6 GiB) / 2 GiB, at most the core count and 12. Slots other than 0 also
wait for `MemAvailable` of at least 2 GiB; slot 0 always runs.

Knobs: `FORKMESH_COMPILE_SLOTS`, `FORKMESH_COMPILE_MIN_FREE_MB` (default 2048),
`FORKMESH_COMPILE_GATE_DIR`, `FORKMESH_COMPILE_GATE_OFF=1`. macOS, Windows and
machines without `flock` build ungated. Agent memory containment is separate
([../agents/memory.md](../agents/memory.md)).

## Rebuild & restart (in the app)

The rail's **Restart** button and **Settings → Rebuild & restart**
(`MainWindow::quickRebuildRestart()`, `src/MainWindowSettings.cpp`) rebuild the
running checkout as `RelWithDebInfo` and relaunch. It queues behind running
agents, re-runs the pass if sources changed mid-build (up to three passes), and
installs only a build of an unchanged tree.

**Update, rebuild & restart** (`MainWindow::updateRebuildRestart()`,
`src/MainWindowSetup.cpp`) runs `git pull --ff-only` from the mirror named by
`/api/install-source`. If the pull fails, the update stops, reports why and
changes nothing. It clones only into a missing or empty folder and never
deletes the checkout, whose `.git` holds every local branch and agent worktree.
`app/tests/test_desktop_update_keeps_checkout.py` guards this.

## Keeping rebuilds fast

- The precompiled header includes `src/MainWindowInternal.h` and
  `src/MainWindow.h`; touching either recompiles every `MainWindow*.cpp`. Put
  helper bodies in `src/MainWindowInternal1.cpp` … `4.cpp` with only a
  declaration in the header.
- The build commit id is in a generated `ForkMeshBuildCommit.h` included only by
  `src/main.cpp` and `src/MainWindowHostInstall.cpp`; `FORKMESH_VERSION` is not a
  cache variable or compile definition.
- Install `ccache` (with room, e.g. `ccache -M 30G`), `ninja` and `mold`.
- The generated `marketplace.qrc` embeds every package (about 47 MB of C++
  through AUTORCC by October 2026). Compiling that one file took 1.3 GiB, and
  the app library and `forkmesh-package-tests` each compiled their own copy, so
  on the 4 GiB CI runner the compiler was killed once CRM 1.37.0 added its
  grants CSV. It goes through `qt_add_big_resources` now: rcc writes the data
  straight into an object file, built once by `forkmesh-marketplace-resources`
  and linked by the app, the window and perf tests (through
  `forkmesh-resources`) and `forkmesh-package-tests`. Bundle other large
  resource sets the same way.

## Manual CMake and ctest

```sh
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=ON
cmake --build desktop/build --target forkmesh            # the app (the only target that compiles src/main.cpp)
cmake --build desktop/build --target critical-tests      # what CI builds
ctest --test-dir desktop/build -L critical --output-on-failure
cmake --build desktop/build --target check               # builds the test set and runs all of ctest
```

Test targets are `EXCLUDE_FROM_ALL`; ctest reports "Not Run" for unbuilt ones.
The `critical` label (timeout 20 s) is `FORKMESH_CRITICAL_TESTS`. Focused window
suites are flag-selected runs of `forkmesh-window-tests`. Tests run offscreen
(`QT_QPA_PLATFORM=offscreen`); `forkmesh-e2e` drives real Git with a stub agent
(`FORKMESH_E2E_STUB_AGENT`); `tests/validate_*.py` are source-level contract
checks registered as ctest cases. Many unit targets link only Qt Core or
Widgets; iterate on those before building the app.

## CI

- `.forkmesh/desktop-ci.yml` builds and runs the critical Qt contracts; it is
  separate from `.forkmesh/ci.yml` and gates nothing.
- Qt builds use at most two compiler jobs. The desktop workflow keeps its
  cache independently from the Worker and deployment workflows, so parallel
  runs do not force a cold Qt rebuild.
- CI never builds the `forkmesh` app target; `src/main.cpp` and the app link
  are only checked by building locally.
- `.forkmesh/release.yml` and `tools/package/*.sh` produce release binaries
  ([../release/README.md](../release/README.md), [windows.md](windows.md)).
- `tools/qt64-build-check.sh [ubuntu-24.04|debian-12]` builds against those
  distributions' Qt 6.4 and OpenSSL 3.0 in a private sysroot.

## Actions workspaces

A workflow's isolated workspace is cloned `--depth=2` unless the workflow sets
a top-level `fetch-depth` (`0` = full history). The clone passes
`--upload-pack=git -c pack.window=0 -c pack.threads=2 upload-pack` so no new
delta search runs and the step stays within its CPU limit
(`ActionSandboxLimits::maxCpuSeconds`).

## Portability rules

- Code under `Q_OS_UNIX` must stay POSIX (e.g. set `O_CLOEXEC`/`O_NONBLOCK`
  with `fcntl`, not `SOCK_CLOEXEC`); Linux-only calls go under `Q_OS_LINUX`.
- Any new target with a macOS `install()` rule must also be built by
  `tools/package/macos_universal_on_mac.sh`.
- Never name a C++ identifier `environ`: MinGW defines it as a macro.
  `tests/validate_no_environ_identifier.py` (ctest
  `forkmesh-no-environ-identifier`) fails on such declarations under
  `desktop/src`, `desktop/tests` and `desktop/packages`, allowing the POSIX
  global (`extern char **environ;`).

## Known failures and traps

- Window, perf and e2e suites can fail in loaded agent sandboxes (timing,
  missing identity, offscreen sizing). Compare against main first.
- High `-j` under load gets `cc1plus` OOM-killed, which reads like a compiler
  crash. Use one modest `ninja -j` with all wanted targets; never run two
  builds in one build directory.
- A build that started before a worktree reset compiled the old tree.
- A merge rewriting a header mid-build can cause a PCH "redefinition" error;
  re-run the build.

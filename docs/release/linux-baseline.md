# Linux release baseline (Debian 12 container build)

The Linux release assets — `forkmesh-linux-x86_64`, `forkmesh-linux-x86_64.AppImage`
and its `.zsync` — are built inside a pinned Debian 12 (bookworm, glibc 2.36)
image so they run on **Ubuntu 22.04+ and Debian 12+**. Before 1.2.5 they were
built natively on the release host (Debian 13): the binary needed `GLIBC_2.38`
and the system's Qt 6.8, and the AppImage bundled host libraries needing up to
`GLIBC_2.39` (`libsystemd`), so neither started on Ubuntu 24.04 or Debian 12.

The Go mirror node is built with `CGO_ENABLED=0` and is fully static, so it is
unchanged and still built by the Actions job. Development builds on the deploy
box (`desktop/run.sh`, the app's Rebuild & restart) are unchanged.

## Code map

| Path | Responsibility |
| --- | --- |
| `tools/package/linux-baseline.sh` | Driver: `engine`, `prepare`, `build`, `check`, `smoke`. |
| `tools/package/linux-baseline/image.env` | Every pin: base image digest, apt snapshot, Qt/OpenSSL/nghttp3/linuxdeploy/appimagetool versions and SHA-256s, the glibc ceiling. |
| `tools/package/linux-baseline/Containerfile` | The same stages as image layers, for podman/docker. |
| `tools/package/linux-baseline/stage-*.sh` | Image stages: `packages`, `openssl`, `qt`, `nghttp3`, `appimage-tools`. |
| `tools/package/linux-baseline/build-forkmesh.sh` | Runs inside the image: configure, build, install, provenance checks, AppImage. |
| `tools/package/linux-baseline/check_glibc.py` | Fails when any ELF (the binary, or any file inside the AppImage) needs `GLIBC_` above the ceiling or `GLIBCXX_` above 3.4.30. |
| `tools/package/linux-baseline/smoke-test.sh` | Launch test run inside Ubuntu 22.04, Ubuntu 24.04 and Debian 12. |
| `tools/package/linux-baseline/oci_rootfs.py` | Pulls an image by digest from Docker Hub into a directory (bubblewrap engine). |
| `tools/forkmesh-release-host.py` (`build_linux_assets`) | Release host: builds, checks and smoke-tests, then publishes into the candidate. |
| `app/tests/test_linux_release_baseline.py` | Checker, pins and recipe contracts. |

## What the image contains

- `debian:bookworm-20260918@sha256:704583db…` with apt pinned to
  `snapshot.debian.org/archive/debian/20260918T000000Z` (and the matching
  security snapshot), so a rebuild installs the same packages.
- **OpenSSL 3.5.8**, static (`no-shared`, `--openssldir=/usr/lib/ssl`, the
  Debian/Ubuntu CA and config location), same tarball and SHA-256 as
  `tools/openssl-local.sh`. The release still configures with
  `-DFORKMESH_REQUIRE_OPENSSL_3_5=ON`, and links with
  `-DOPENSSL_ROOT_DIR=/opt/forkmesh/openssl -DOPENSSL_USE_STATIC_LIBS=ON`
  instead of `-DFORKMESH_BUNDLED_OPENSSL=ON`, whose shared build and absolute
  RPATH only work on the machine that built it.
- **Qt 6.8.2, static** (`qtbase` + `qtsvg`, the version the app uses today),
  checked against Qt's published md5sums. Only what the app links is built:
  Core, Gui, Widgets, Network, Svg, Concurrent, DBus (loaded at run time,
  `INPUT_dbus=runtime`), OpenGL and OpenGLWidgets. Platform plugins `xcb`,
  `offscreen`, `minimal`, with the xcb GLX/EGL integrations; TLS through
  OpenSSL linked in. Bundled 3rd-party copies of PCRE2, HarfBuzz, libpng,
  libjpeg, double-conversion, md4c and libb2; ICU, glib, libproxy, GSSAPI,
  brotli, zstd, SQL and GTK3 are off. Multimedia and Bluetooth stay off, as in
  the 1.2.4 release binary.
- **nghttp3 1.8.0**, static, so the HTTP/3 log stream (`src/Http3Get.cpp`) is in
  the release; `build-forkmesh.sh` fails if it is missing.
- `libwebp.a` for WebP diff previews; xcb-util, -cursor, -icccm, -image,
  -keysyms and -render-util linked statically (their `.so` dev links are
  removed in the image) because `libxcb-cursor0` is missing on many desktops
  and Qt 6.5+ needs it.
- linuxdeploy `1-alpha-20251107-1` and appimagetool `1.9.1` (static runtime),
  pinned by SHA-256. No linuxdeploy Qt plugin: `tools/package/appimage.sh`
  skips it when the binary does not link `libQt6Core`
  (`FORKMESH_PKG_QT_LINKAGE=shared|static` overrides the check).

The resulting binary needs only system libraries present on any X11/Wayland
desktop (and installed by `install.sh`'s Qt runtime step on servers):
`libX11`, `libX11-xcb`, `libxcb` and its glx/randr/render/shape/shm/sync/
xfixes/xkb extensions, `libxkbcommon(-x11)`, `libfontconfig`, `libfreetype`,
`libEGL`/`libGLX`/`libOpenGL`, `libz`, `libstdc++` (GLIBCXX ≤ 3.4.30, GCC 12)
and glibc. It never loads Qt, OpenSSL, nghttp3, libwebp or ICU from the system
(`build-forkmesh.sh` checks `NEEDED`). Wayland sessions run it through XWayland.

## Engines

`linux-baseline.sh` picks `podman`, then `docker`, when the daemon answers for
the current user, else **bubblewrap** (`FORKMESH_LINUX_BASELINE_ENGINE`
overrides). The deploy box has no usable podman/docker as user `f`: podman is
not installed, the docker socket is `root:docker`, and rootless docker needs
`newuidmap` from the `uidmap` package (`sudo apt install uidmap`, then
`dockerd-rootless-setuptool.sh install`). Until then the bubblewrap engine runs
the same pinned image (pulled by digest, every blob hash-checked) in a
single-uid user namespace; it binds a `chown`/`chgrp` shim and
`DPkg::Path` so package scripts that chown to other ids do not fail, and
extracts tarballs with `--no-same-owner`. Stages are cached like layers: a stage
reruns only when its script, an earlier stage or `image.env` changes
(`<cache>/rootfs/builder/.forkmesh-stages`).

Cache: `~/projects/forkmesh/toolchain-linux` (`FORKMESH_LINUX_BASELINE_CACHE`):
`downloads/`, `rootfs/builder` (~3 GB), `rootfs/smoke-*`, `work/build` and
`work/ccache`, stage logs.

## Commands

```sh
tools/package/linux-baseline.sh prepare
tools/package/linux-baseline.sh build --source <clean tree> --out <dir> \
    --version X.Y.Z --commit <build commit> [--channel candidate-X.Y.Z]
tools/package/linux-baseline.sh check <dir>/forkmesh-linux-x86_64 <dir>/forkmesh-linux-x86_64.AppImage
tools/package/linux-baseline.sh smoke --out <dir> --version X.Y.Z
python3 -m pytest app/tests/test_linux_release_baseline.py
```

`build` runs `check` itself. The smoke test pulls `ubuntu:22.04`,
`ubuntu:24.04` and `debian:12`, installs the X11/GL/fontconfig runtime list in
`linux-baseline.sh` (`smoke_packages`), and for both assets requires: every
shared library resolves, `--version` prints the release, and the app started
with `QT_QPA_PLATFORM=offscreen` is still running after 25 s. HOME and
`XDG_*_HOME` point into a temp dir and the environment is cleared (`env -i`),
so no `FORKMESH_HOME`/`FORKMESH_PROJECTS_DIR` reaches it.

## Release flow

- `.forkmesh/release.yml` on Linux now publishes only
  `forkmesh-mirror-node-linux-x86_64` into `candidate-<version>`; the Actions
  sandbox cannot run containers.
- `tools/forkmesh-release-host.py` lists `forkmesh-linux-x86_64` and
  `forkmesh-linux-x86_64.AppImage` in `HOST_ASSETS`. They are required by the
  gate, so every new candidate is picked up by the host timer: it builds both in
  a pristine worktree at the build commit, runs `check` and `smoke`, and
  publishes the binary, the AppImage and the `.zsync` before the APK, Windows
  and macOS steps. A candidate without the mirror node is still treated as an
  incomplete sandbox matrix and left alone.
- `app/deploy.sh republish-release-binary` builds the Linux binary through the
  same container.

Timings on the deploy box (36 cores, jobs capped by free memory): packages
271 s, OpenSSL 54 s, Qt 856 s at `-j8`, nghttp3 3 s — about 20 minutes once per
recipe change. The app itself: about 13 minutes cold at `-j6`, seconds to relink
from `work/ccache`, plus ~1 minute for the AppImage and ~4.5 minutes for the
first smoke run (distro images and packages are cached afterwards).

## Traps

- **glibc ceiling is 2.35, not 2.36.** Ubuntu 22.04 ships glibc 2.35; a
  Debian 12 build can still pick up `GLIBC_2.36` symbols (e.g. `arc4random`).
  1.2.5's binary tops out at exactly `GLIBC_2.35`.
- **Static xcb-util link order.** `libxcb-image.a` needs `xcb_aux_*` from
  `libxcb-util.a`, which Qt's targets list earlier; `build-forkmesh.sh` appends
  it with `CMAKE_CXX_STANDARD_LIBRARIES`.
- **ccache 4.7 (Debian 12) rejects `ccache sloppiness=… c++`**, the launcher
  `desktop/CMakeLists.txt` sets whenever ccache is found. The container passes
  `-DFORKMESH_CCACHE=OFF` and sets `CCACHE_SLOPPINESS` instead. A source build on
  stock Debian 12 or Ubuntu 22.04 with ccache installed hits the same error.
- **No accessibility bridge.** Qt's AT-SPI bridge needs `libatspi`, which is not
  in the image; linking it would add a `NEEDED` entry absent on servers.
- **Dropped AppImage libraries.** linuxdeploy bundles fewer libraries now (no Qt,
  ICU, glib, gnutls, krb5, systemd); the AppImage went from ~100 MB to ~80 MB.

## Related docs

- [README.md](README.md) — the release flow this plugs into.
- [`docs/desktop/build-and-test.md`](../desktop/build-and-test.md) — OpenSSL options.

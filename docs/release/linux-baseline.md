# Linux release baseline (Debian 12 container build)

The Linux release assets (`forkmesh-linux-x86_64`,
`forkmesh-linux-x86_64.AppImage` and its `.zsync`, and
`forkmesh-linux-x86_64.deb` for the [apt repository](apt.md)) are built inside a pinned
Debian 12 (glibc 2.36) image so they run on Ubuntu 22.04+ and Debian 12+. The
Go mirror daemon is fully static (`CGO_ENABLED=0`) and is built by the Actions
job. Development builds are unaffected.

## Code map

| Path | Responsibility |
| --- | --- |
| `tools/package/linux-baseline.sh` | driver: `engine`, `prepare`, `build`, `check`, `smoke` |
| `tools/package/linux-baseline/image.env` | every pin: base image digest, apt snapshot, toolchain versions and SHA-256s, the glibc ceiling |
| `tools/package/linux-baseline/Containerfile` | the same stages as image layers for podman or docker |
| `tools/package/linux-baseline/stage-*.sh` | image stages: packages, OpenSSL, Qt, nghttp3, AppImage tools |
| `tools/package/linux-baseline/build-forkmesh.sh` | configure, build, install, provenance checks, AppImage, `.deb` (`--build N` sets the package's `+N`) |
| `tools/package/linux-baseline/check_glibc.py` | fails when any ELF needs `GLIBC_` above the ceiling or `GLIBCXX_` above 3.4.30 |
| `tools/package/linux-baseline/smoke-test.sh` | launch test inside Ubuntu 22.04, Ubuntu 24.04 and Debian 12; the `.deb`'s dependencies and binary |
| `tools/package/linux-baseline/oci_rootfs.py` | pulls an image by digest into a directory (bubblewrap engine), refusing unsafe layer members |
| `tools/forkmesh-release-host.py` (`build_linux_assets`) | builds, checks, smoke-tests and publishes into the candidate |
| `app/tests/test_linux_release_baseline.py` | checker, pins and recipe contracts |

## Image contents

- A Debian 12 base image pinned by digest, with apt pinned to a
  `snapshot.debian.org` date, so rebuilds install identical packages.
- Static OpenSSL 3.5 (CA and config in the distribution's standard
  locations), linked with `-DOPENSSL_USE_STATIC_LIBS=ON`.
- Static Qt 6 (`qtbase`, `qtsvg`) with only the modules the app links, xcb,
  offscreen and minimal platform plugins, bundled third-party copies, and
  ICU, glib, GTK and SQL off.
- Static nghttp3 (HTTP/3), libwebp and xcb utility libraries.
- linuxdeploy and appimagetool pinned by SHA-256.

The binary needs only common desktop system libraries (X11/xcb, xkbcommon,
fontconfig, freetype, EGL/GLX, zlib, libstdc++ from GCC 12, glibc); it never
loads Qt, OpenSSL, nghttp3, libwebp or ICU from the system
(`build-forkmesh.sh` checks `NEEDED`). Wayland sessions run it through
XWayland.

## Engines

`linux-baseline.sh` uses podman, then docker, when available to the current
user, else bubblewrap (`FORKMESH_LINUX_BASELINE_ENGINE` overrides). The
bubblewrap engine runs the same pinned image (pulled by digest, every blob
hash-checked) in a single-uid user namespace. Stages are cached and rerun only
when their script, an earlier stage or `image.env` changes. Cache:
`~/projects/forkmesh/toolchain-linux` (`FORKMESH_LINUX_BASELINE_CACHE`).

## Commands

```sh
tools/package/linux-baseline.sh prepare
tools/package/linux-baseline.sh build --source <clean tree> --out <dir> \
    --version X.Y.Z --commit <build commit> [--channel candidate-X.Y.Z]
tools/package/linux-baseline.sh check <dir>/forkmesh-linux-x86_64 <dir>/forkmesh-linux-x86_64.AppImage
tools/package/linux-baseline.sh smoke --out <dir> --version X.Y.Z
python3 -m pytest app/tests/test_linux_release_baseline.py
```

`build` runs `check` itself. The smoke test requires, for both assets on each
target distribution, that every shared library resolves, `--version` prints
the release, and the app started with `QT_QPA_PLATFORM=offscreen` is still
running after 25 s. It runs with a temporary `HOME` and `XDG_*_HOME` and a
cleared environment.

## Release flow

The Linux assets are listed in the release host's `HOST_ASSETS` and required
by the gate, so every new candidate is built by the host timer in a pristine
worktree at the build commit, checked, smoke-tested and published into the
candidate.

The release build passes `-DFORKMESH_PACKAGE_MODULES=OFF`, so Market packages
are compiled into the one `forkmesh-linux-x86_64` binary as they are on macOS
and Windows. Modules are the Linux default since the empty-shell work, but a
module is a shared object, and the static `libxcb-*.a` archives in the image
are not built with `-fPIC`: v1.5.0's first build (2026-10-28) failed linking
`packages/timers.so` with "relocation R_X86_64_PC32 against symbol
`xcb_render_id` can not be used when making a shared object". The release
asset is a single file, so loose module files would not have shipped anyway.
The recipe comes from the release host's checkout, not the tag, so a fix here
applies to the candidate being built.

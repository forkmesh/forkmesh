# Desktop: Windows

Part of [the desktop docs](README.md).

## How the Windows build is made

The installer `forkmesh-windows-x86_64-setup.exe` is cross-built on the Linux
release host:

- `tools/package/install-windows-toolchain.sh` installs, without root, a MinGW
  cross compiler, NSIS, `osslsigncode` and a Qt `win64_mingw` kit into a private
  cache; `tools/package/windows-toolchain.sh` (sourced) exports the
  `FORKMESH_WIN_*` roots, `QT_HOST_PATH` and `NSISDIR`.
- CMake toolchain file: `desktop/cmake/mingw-w64-x86_64.cmake`.
- OpenSSL 3.5 is cross-built because the desktop requires it
  (`find_package(OpenSSL 3.5 REQUIRED)`).
- `tools/package/windows.sh` deploys Qt and the MinGW runtime DLLs, builds the
  NSIS installer from `tools/package/forkmesh.nsi`, and Authenticode-signs it
  (`FORKMESH_WIN_CERT_PFX`, `FORKMESH_WIN_CERT_PASSWORD`,
  `FORKMESH_WIN_TIMESTAMP_URL`; mandatory when `FORKMESH_REQUIRE_NATIVE_SIGNING=1`).
- Users update by running a newer `setup.exe`; the per-user install
  (`RequestExecutionLevel user`) upgrades in place without elevation. In-app
  auto-update only runs where a source checkout exists.

### Qt DLL deployment

Without `windeployqt`, `deploy_qt_by_hand` copies a base module list, then reads
the PE import tables (`x86_64-w64-mingw32-objdump -p`) of `forkmesh.exe`, every
plugin and every copied DLL, and copies each import the Qt kit holds until
nothing new appears. `verify_payload_imports` then requires every `Qt6*` or
`lib*` import of any payload binary to be in the payload, or packaging fails.
Test: `app/tests/test_native_installer_packaging.py`.

### `environ` is a macro under MinGW

MinGW's `<stdlib.h>` defines `environ` as a macro, so any identifier named
`environ` breaks only the Windows build. The toolchain file force-includes
`desktop/cmake/mingw-no-environ-macro.h`, which undefines it; this also means
the MainWindow units do not use the precompiled header on Windows. Avoid the
identifier anyway (`desktop/tests/validate_no_environ_identifier.py`).

## Settings live in the registry

On Windows the home project's `QSettings` is the registry key
`HKEY_CURRENT_USER\Software\ForkMesh\ForkMesh`, and `QSettings::fileName()`
returns that key name, which `QFileInfo` would treat as a rooted path. Never
derive a file or folder from it: `forkmesh::paths::settingsFilePathFor()` /
`settingsFolderFor()` in `desktop/src/AppPaths.h` return nothing for a registry
name, and every caller goes through them.

## Mirror Actions lock

- With registry-backed settings, the mirror Actions lock and journal live under
  `<data root>/actions` (`desktop/src/MirrorActionsConfiguration.*`).
- The journal's group/other permission check is POSIX-only.
- A failed startup recovery only suspends mirror Actions. The window opens with
  a non-modal warning offering **Retry** and **Reset Actions configuration…**
  (`desktop/src/main.cpp`), which deletes exactly the lock and journal files and
  refuses while a live helper holds the lock.

Tests: `forkmesh-control-tests` and `forkmesh-app-paths-tests`.

## Uninstaller

The NSIS uninstaller never deletes the install directory recursively.
`windows.sh` generates an explicit manifest
(`tools/package/nsis-uninstall-manifest.sh`): one `Delete` per payload file,
then a non-recursive `RMDir` per payload folder, deepest first. Install and
upgrade delete nothing.

## Traps

- Windows reports every file as world-readable without an NTFS ACL lookup;
  POSIX mode checks must be skipped there.
- Anything that deletes on uninstall must be an explicit list of what the
  installer wrote. A recursive delete of the install directory can reach user
  data.

# Desktop: Windows

Part of [the desktop docs](README.md).

## How the Windows build is made

There is no Windows machine in the fleet. The installer
`forkmesh-windows-x86_64-setup.exe` is cross-built on the Linux release host:

- `tools/package/install-windows-toolchain.sh` installs, without root, a MinGW
  cross compiler, NSIS, `osslsigncode`, and a Qt `win64_mingw` kit into a
  private cache; `tools/package/windows-toolchain.sh` (sourced) exports the
  `FORKMESH_WIN_*` roots, `QT_HOST_PATH` and `NSISDIR`.
- CMake toolchain file: `desktop/cmake/mingw-w64-x86_64.cmake`.
- OpenSSL 3.5 is cross-built for the installer because the Qt kit ships 3.0 and
  the desktop requires 3.5 (`find_package(OpenSSL 3.5 REQUIRED)`).
- `tools/package/windows.sh` deploys Qt and the MinGW runtime DLLs, builds the
  NSIS installer from `tools/package/forkmesh.nsi`, and Authenticode-signs it
  (`FORKMESH_WIN_CERT_PFX`, `FORKMESH_WIN_CERT_PASSWORD`,
  `FORKMESH_WIN_TIMESTAMP_URL`; signing is mandatory when
  `FORKMESH_REQUIRE_NATIVE_SIGNING=1`). The certificate is self-issued, so
  Windows shows "Unknown publisher".
- Windows users update by running a newer `setup.exe`; the per-user install
  (`RequestExecutionLevel user`) upgrades in place without elevation.
  WinSparkle was planned in issue #370 but never linked into the desktop. The
  in-app auto-update (`MainWindow::maybeAutoUpdate`) only runs where a source
  checkout exists (`<data root>/src/desktop`), which an installer install does
  not have. `tools/package/generate-appcast.sh` still writes `appcast.xml`,
  which nothing reads yet.

## Settings live in the registry

On Windows the home project's `QSettings` is the registry key
`HKEY_CURRENT_USER\Software\ForkMesh\ForkMesh`, and `QSettings::fileName()`
returns that key name. `QFileInfo` treats `\HKEY_CURRENT_USER\...` as a rooted
path on the current drive. Never derive a file or folder from it:
`forkmesh::paths::settingsFilePathFor()` / `settingsFolderFor()` in
`desktop/src/AppPaths.h` return nothing for a registry name, and every caller
(settings folder, config root, uninstall list, config restore, Actions lock)
goes through them.

## The 1.2.4 mirror Actions lock fix (#727)

Symptom: on every launch the desktop stopped with
`configuration_lock_unavailable`. Cause: the mirror Actions transaction lock
and journal were placed beside `QSettings::fileName()`, which resolved to
`<drive>:\HKEY_CURRENT_USER\Software\ForkMesh`; a standard user cannot create a
folder at a drive root, and `main()` treated the failure as fatal.
Reinstalling could not help.

Fix (`f08182c96`, `aef0332af`, released in 1.2.4):

- Registry-backed settings keep the Actions lock and journal under
  `<data root>/actions` (`desktop/src/MirrorActionsConfiguration.*`).
- The journal's group/other permission check is POSIX-only.
- A failed startup recovery only suspends mirror Actions. The window opens with
  a non-modal warning offering **Retry** and **Reset Actions configuration…**
  (`desktop/src/main.cpp`), which deletes exactly the lock and journal files
  (and their pre-fix location) and refuses while a live helper holds the lock.
- The NSIS uninstaller no longer runs `RMDir /r "$INSTDIR"`, which also erased
  the legacy data root `%LOCALAPPDATA%\ForkMesh\ForkMesh`. `windows.sh` now
  generates an explicit manifest (`tools/package/nsis-uninstall-manifest.sh`):
  one `Delete` per payload file, then a non-recursive `RMDir` per payload
  folder, deepest first. It also removes the stray lock/journal and the empty
  `C:\HKEY_CURRENT_USER` folders left by earlier versions. Install and upgrade
  delete nothing.

Tests: `forkmesh-control-tests` (registry-name simulation, uncreatable lock
directory, stale and live locks, reset) and `forkmesh-app-paths-tests`.

## The 1.2.5 missing Qt6OpenGLWidgets.dll fix (#712)

Symptom: on a clean install, 1.2.5 stopped at launch with "The code
execution cannot proceed because Qt6OpenGLWidgets.dll was not found".
Cause: the release host has no `windeployqt`, so `deploy_qt_by_hand` in
`tools/package/windows.sh` copied a fixed module list (Core, Gui, Widgets,
Network, Svg, Concurrent). The Watch field's GPU painter
(`forkmesh_link_opengl` in `desktop/CMakeLists.txt`) added Qt6OpenGLWidgets
to `forkmesh.exe`'s imports and the list was never updated.

Fix:

- After the fixed list, `deploy_qt_by_hand` reads the PE import table of
  `forkmesh.exe`, every plugin and every DLL it copies
  (`x86_64-w64-mingw32-objdump -p`), and copies each import the Qt kit's
  `bin/` holds, repeating until nothing new appears (Qt6OpenGLWidgets pulls
  in Qt6OpenGL). A new Qt module now ships without touching the packager.
- `verify_payload_imports` runs after OpenSSL is copied and before `makensis`:
  every import named `Qt6*` or `lib*` in any payload `.exe`/`.dll` must be in
  the payload, or packaging fails. Windows ships no DLL with those prefixes;
  other imports (`KERNEL32.dll`, `msvcrt.dll`, ...) come from the system. On a
  Windows runner without an objdump the check is skipped; a cross build
  requires one.
- `pe_objdump` checks its probe with `grep >/dev/null`, not `grep -q`: under
  `pipefail`, `grep -q` exits at the first match, objdump dies of SIGPIPE, and
  the probe reports no objdump.

Test: `app/tests/test_native_installer_packaging.py`
(`test_windows_packager_bundles_every_imported_qt_dll`). Verified by
repackaging the 1.2.5 `forkmesh.exe`: the log shows Qt6OpenGLWidgets.dll and
Qt6OpenGL.dll bundled and the import check passing.

## `environ` is not a macro in Windows builds (1.2.7)

MinGW's `<stdlib.h>` has `#define environ _environ`, and `_environ` expands
to `(* __p__environ())`. Any identifier named `environ` (a parameter, a
local) then fails to compile, and only on Windows: 1.2.6 lost its installer
to `MachineAgentScan.cpp` (841ef1b), and 1.2.7 was tagged with
`ProcessReaper.h` declaring `instanceFromEnviron(const QByteArray &environ)`,
which broke every unit that includes it.

Fix: `desktop/cmake/mingw-w64-x86_64.cmake` force-includes
`desktop/cmake/mingw-no-environ-macro.h` into every C++ unit; it includes
`<stdlib.h>` and undefines `environ`. No Windows code uses the C `environ`
variable (the `extern char **environ` users are Unix-only). The release host
takes the toolchain file from the primary checkout's main, not from the
tagged tree, so the fix also builds releases tagged before it, at their
own build commit, with unchanged provenance.

The force include comes before the CMake precompiled header, so GCC does not
use the PCH for the MainWindow units in Windows builds; it parses
`MainWindowInternal.h` as text instead. Slower, but the same output.

## Traps

- Windows reports every file as world-readable without an NTFS ACL lookup;
  POSIX mode checks must be skipped there.
- Anything that deletes on uninstall must be an explicit list of what the
  installer wrote. A recursive delete of the install directory can reach user
  data.

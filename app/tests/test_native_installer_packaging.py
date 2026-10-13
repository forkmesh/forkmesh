#!/usr/bin/env python3
"""Contracts that keep release packaging binary-first and self-contained."""

import os
import struct
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "tools" / "package"
WORKFLOW = ROOT / ".forkmesh" / "release.yml"
INSTALLER = ROOT / "www" / "public" / "install.sh"
ACTION_RUNNER = ROOT / "desktop" / "src" / "ActionRunner.cpp"
APPIMAGE_TOOLCHAIN = PACKAGE / "install-appimage-toolchain.sh"


def test_linux_packager_requires_a_real_appimage_toolchain():
    source = (PACKAGE / "appimage.sh").read_text()
    assert 'required_tools="linuxdeploy appimagetool"' in source
    assert 'required_tools="linuxdeploy linuxdeploy-plugin-qt appimagetool"' in source
    assert "for tool in $required_tools" in source
    assert "qt_plugin=(--plugin qt)" in source
    assert "NEEDED.*libQt6Core" in source
    assert "AppDir.tar.gz" not in source
    assert "APPIMAGE_EXTRACT_AND_RUN=1" in source


def test_linux_packager_uses_qt6_qmake_for_qt_deployment():
    source = (PACKAGE / "appimage.sh").read_text()

    assert 'command -v qmake6' in source
    assert 'QMAKE="$(command -v qmake6' in source
    assert '"$QMAKE" -query QT_VERSION' in source
    assert 'QMAKE must select Qt 6' in source
    assert 'export QMAKE' in source


def test_linux_packager_uses_a_linuxdeploy_compatible_icon():
    source = (PACKAGE / "appimage.sh").read_text()
    icon = ROOT / "www/public/favicon/android-chrome-512x512.png"
    header = icon.read_bytes()[:24]

    assert header[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", header[16:24]) == (512, 512)
    assert 'icon_source="www/public/favicon/android-chrome-512x512.png"' in source
    assert "hicolor/512x512/apps" in source
    assert "desktop/resources/icons/forkmesh.png" not in source


def test_linux_packager_keeps_appimage_and_zsync_in_the_output_directory():
    source = (PACKAGE / "appimage.sh").read_text()

    assert 'outdir="$(cd "$outdir" && pwd -P)"' in source
    assert '( cd "$outdir" &&' in source
    assert '"$appdir" "$name"' in source


def test_linux_packager_prefers_qt6_qmake_over_unversioned_qmake(tmp_path):
    fake_bin = tmp_path / "fake-bin"
    fake_bin.mkdir()
    for tool in ("linuxdeploy", "linuxdeploy-plugin-qt", "appimagetool"):
        executable = fake_bin / tool
        executable.write_text("#!/bin/sh\nexit 0\n")
        executable.chmod(0o755)

    qmake = fake_bin / "qmake"
    qmake.write_text("#!/bin/sh\necho unexpected Qt 5 qmake >&2\nexit 31\n")
    qmake.chmod(0o755)
    qmake6 = fake_bin / "qmake6"
    qmake6.write_text(
        "#!/bin/sh\n"
        '[ "$1" = "-query" ] && [ "$2" = "QT_VERSION" ] && echo 6.8.2\n'
    )
    qmake6.chmod(0o755)

    env = {
        **os.environ,
        "PATH": f"{fake_bin}:/usr/bin:/bin",
    }
    env.pop("QMAKE", None)
    env.pop("FORKMESH_PKG_INSTALL_ROOT", None)
    env["FORKMESH_PKG_QT_LINKAGE"] = "shared"
    result = subprocess.run(
        [str(PACKAGE / "appimage.sh"), "/bin/true"],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
    )

    # The missing install tree stops packaging after qmake selection; the Qt 5
    # sentinel would instead terminate at qmake validation.
    assert result.returncode == 1
    assert "CMake-installed ForkMesh deployment resources are incomplete" in result.stderr
    assert "unexpected Qt 5 qmake" not in result.stderr


def test_linux_release_builds_desktop_assets_in_the_baseline_container():
    workflow = WORKFLOW.read_text()
    host = (ROOT / "tools" / "forkmesh-release-host.py").read_text()

    assert ". tools/package/appimage-toolchain.sh" not in workflow
    assert "install-appimage-toolchain.sh" not in workflow
    assert 'if [ "$os" = "linux" ]; then\n            desktop_native=0' in workflow
    assert 'publish_assets=("$mirror_asset")' in workflow
    assert "tools/package/linux-baseline.sh" in workflow
    assert '"$FORKMESH_LINUX_DRIVER" build --source .' in host
    assert '"$FORKMESH_LINUX_DRIVER" smoke --out' in host
    assert 'ROOT / "tools" / "package" / "linux-baseline.sh"' in host


def test_appimage_toolchain_installer_stays_pinned():
    toolchain = APPIMAGE_TOOLCHAIN.read_text()
    assert "linuxdeploy_tag=\"1-alpha-20251107-1\"" in toolchain
    assert "qt_plugin_tag=\"1-alpha-20250213-1\"" in toolchain
    assert "appimagetool_tag=\"1.9.1\"" in toolchain
    assert "linuxdeploy-${tool_arch}.AppImage" in toolchain
    assert "linuxdeploy-plugin-qt-${tool_arch}.AppImage" in toolchain
    assert "appimagetool-${tool_arch}.AppImage" in toolchain


def test_appimage_installer_downloads_each_tool_to_its_own_path(tmp_path):
    fake_bin = tmp_path / "fake-bin"
    fake_bin.mkdir()
    curl = fake_bin / "curl"
    curl.write_text(
        "#!/bin/sh\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = --output ]; then shift; output=$1; fi\n"
        "  shift\n"
        "done\n"
        "printf appimage-fixture > \"$output\"\n"
    )
    curl.chmod(0o755)
    destination = tmp_path / "toolchain"
    env = {
        **os.environ,
        "FORKMESH_APPIMAGE_TOOL_DIR": str(destination),
        "PATH": f"{fake_bin}:/usr/bin:/bin",
    }

    subprocess.run(
        [str(APPIMAGE_TOOLCHAIN), "x86_64"],
        cwd=ROOT,
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )

    assert {path.name for path in destination.iterdir()} == {
        "linuxdeploy",
        "linuxdeploy-plugin-qt",
        "appimagetool",
    }
    assert all(path.stat().st_mode & 0o111 for path in destination.iterdir())


def test_windows_packager_deploys_qt_before_nsis():
    source = (PACKAGE / "windows.sh").read_text()
    nsis = (PACKAGE / "forkmesh.nsi").read_text()
    assert "windeployqt --release" in source
    assert source.index("windeployqt --release") < source.index('log "makensis:')
    assert "-DFORKMESH_PAYLOAD" in source
    assert 'File /r "${FORKMESH_PAYLOAD}\\*"' in nsis
    assert "FORKMESH_SRCEXE" not in nsis
    assert "FORKMESH_REQUIRE_NATIVE_SIGNING" in source
    assert "signtool verify /pa" in source
    assert "osslsigncode verify" in source
    assert "copy_openssl_runtime libcrypto-3-x64.dll" in source
    assert "copy_openssl_runtime libssl-3-x64.dll" in source
    assert '[ ! -f "$resource_root/app/deploy.sh" ]' in source


def test_windows_packager_bundles_every_imported_qt_dll():
    source = (PACKAGE / "windows.sh").read_text()
    hand = source[source.index("deploy_qt_by_hand() {"):source.index("\npe_objdump() {")]
    assert 'pe_imports "$objdump" "$file"' in hand
    assert 'cp "$qt/bin/$dll" "$payload/$dll"' in hand
    assert "grep -q" not in source[source.index("pe_objdump() {"):source.index("pe_imports() {")]
    verify = source.index("\nverify_payload_imports\n")
    assert source.index("copy_openssl_runtime libssl-3-x64.dll") < verify
    assert verify < source.index('log "makensis:')


def test_macos_packager_deploys_runtime_before_signing():
    source = (PACKAGE / "macos.sh").read_text()
    assert "macdeployqt is required" in source
    assert source.index('"$macdeployqt" "$app"') < source.index("codesign --force")
    assert "otool -L" in source
    assert "install_name_tool -change" in source
    assert "FORKMESH_REQUIRE_NATIVE_SIGNING" in source
    assert "codesign --verify --deep --strict" in source


def test_release_cannot_publish_only_a_bare_desktop_binary():
    source = WORKFLOW.read_text()
    assert "native packager produced no installer" in source
    assert "unexpected native installer name" in source
    assert "export FORKMESH_REQUIRE_NATIVE_SIGNING=1" in source
    assert "publishing the bare binary only" not in source
    assert "| tail -n1 || true" not in source
    assert 'channel="${RELEASE_CHANNEL:-candidate-${version}}"' in source
    # Promotion to the public channel is gated on the required asset set and
    # on not dropping assets latest already offers (tools/forkmesh-release-
    # promote.py, covered by test_release_promotion_gate.py). The gate runs
    # only after the candidate is published, and its config is checked before
    # the expensive build.
    gate = "python3 tools/forkmesh-release-promote.py"
    assert f'{gate} \\\n            --candidate "$channel" --channel "$promote_channel"' in source
    assert source.index("tools/forkmesh-release-publish.sh") < source.index(
        "--candidate \"$channel\""
    )
    assert source.index(f"{gate} --check-config") < source.index("cmake -S desktop")
    assert "3) echo \"Candidate $channel remains unpublished" in source
    assert "FORKMESH_RELEASE_REQUIRED_ASSETS" in source
    assert "FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS" in source
    assert "complete_matrix" not in source
    assert 'cp "$candidate_manifest"' not in source
    runner = ACTION_RUNNER.read_text()
    release_run = runner[
        runner.index("bool ActionRunner::isReleaseRun() const"):
        runner.index("bool ActionRunner::landReleaseMetadata()")
    ]
    assert 'm_run.workflowPath == QLatin1String(".forkmesh/release.yml")' in release_run


def test_end_user_installer_is_binary_only_by_default():
    source = INSTALLER.read_text()
    default = source[
        source.index('if [ -z "${FORKMESH_NO_SOURCE_FALLBACK:-}" ]'):
        source.index("# Anchor to a directory", source.index("FORKMESH_NO_SOURCE_FALLBACK"))
    ]
    assert "FORKMESH_NO_SOURCE_FALLBACK=1" in default
    assert "DISPLAY" not in default
    assert "WAYLAND_DISPLAY" not in default
    assert "Download a native installer" in source

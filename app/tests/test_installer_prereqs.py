#!/usr/bin/env python3
"""Installer prerequisite and dependency-install checks."""

from pathlib import Path
import os
import shutil
import subprocess


INSTALLER = Path(__file__).resolve().parents[2] / "www" / "public" / "install.sh"

# The hard-coded prefix list inside have_qt6_cmake_package. Tests swap it for a
# fixture root so a Qt 6 install on the machine running the suite cannot mask a
# probe that is actually broken.
SYSTEM_ROOTS = "/usr /usr/local /opt/homebrew/opt/qt /usr/local/opt/qt"


def _function(name):
    source = INSTALLER.read_text(encoding="utf-8")
    start = source.index(f"{name}() {{")
    end = source.index("\n}\n", start) + len("\n}\n")
    return source[start:end]


def _qt_probe(pinned_root=None):
    package_probe = _function("have_qt6_cmake_package")
    if pinned_root is not None:
        assert SYSTEM_ROOTS in package_probe, "prefix list moved; update SYSTEM_ROOTS"
        package_probe = package_probe.replace(SYSTEM_ROOTS, f'"{pinned_root}"')
    return (
        "set -uo pipefail\n"
        + _function("pkg_config_probe")
        + package_probe
        + _function("have_qt6_dev")
    )


def _qt_layout(root, *, multiarch=False, svg=True, lib="lib"):
    """Create the Qt6 CMake package files a dev install ships under <root>."""
    cmake_dir = Path(root) / lib
    if multiarch:
        cmake_dir /= "x86_64-linux-gnu"
    cmake_dir /= "cmake"
    (cmake_dir / "Qt6").mkdir(parents=True)
    (cmake_dir / "Qt6" / "Qt6Config.cmake").write_text("", encoding="utf-8")
    (cmake_dir / "Qt6Widgets").mkdir()
    (cmake_dir / "Qt6Widgets" / "Qt6WidgetsConfig.cmake").write_text("", encoding="utf-8")
    if svg:
        (cmake_dir / "Qt6Svg").mkdir()
        (cmake_dir / "Qt6Svg" / "Qt6SvgConfig.cmake").write_text("", encoding="utf-8")


def _run(script, tmp_path, *, path_tools=(), extra_env=None):
    # A PATH holding only the named tools, so "pkg-config is not installed" can
    # be reproduced without touching the machine running the suite.
    bindir = tmp_path / "stubbin"
    bindir.mkdir(exist_ok=True)
    for tool in ("bash", "dirname", "uname"):
        real = shutil.which(tool)
        assert real, f"{tool} is required to run this test"
        link = bindir / tool
        if not link.exists():
            link.symlink_to(real)
    for tool, body in path_tools:
        stub = bindir / tool
        stub.write_text(body, encoding="utf-8")
        stub.chmod(0o755)
    env = dict(os.environ, PATH=str(bindir))
    env.pop("CMAKE_PREFIX_PATH", None)
    env.pop("Qt6_DIR", None)
    env.update(extra_env or {})
    return subprocess.run(
        [str(bindir / "bash"), "-c", script], env=env,
        text=True, capture_output=True, check=False,
    )


def _probe_call(fn):
    return f'\nif {fn}; then echo FOUND; else echo MISSING; fi\n'


def test_apt_qt_packages_include_build_tools_and_pkg_config(tmp_path):
    script = (
        "set -u\n"
        + _function("pkg_for")
        + '\nPM=apt\nprintf "%s\\n" "$(pkg_for qt)"\n'
    )
    result = _run(script, tmp_path)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip().split() == [
        "qt6-base-dev",
        "qt6-svg-dev",
        "qt6-base-dev-tools",
        "pkg-config",
    ]


def test_qt_probe_survives_a_host_without_pkg_config(tmp_path):
    # The regression: minimal images ship no pkg-config, so the old probe
    # reported Qt 6 as missing, the installer reinstalled the dev packages the
    # package manager already had, re-ran the same failing check and died with
    # "Installed qt6-base-dev qt6-svg-dev but 'qt' is still unavailable".
    root = tmp_path / "prefix"
    _qt_layout(root)
    result = _run(
        _qt_probe(pinned_root=root) + _probe_call("have_qt6_dev"),
        tmp_path,
    )
    assert result.returncode == 0, result.stderr
    assert "FOUND" in result.stdout


def test_qt_probe_finds_debian_multiarch_layout(tmp_path):
    # Debian/Ubuntu install the packages under lib/<triplet>/cmake.
    root = tmp_path / "prefix"
    _qt_layout(root, multiarch=True)
    result = _run(
        _qt_probe(pinned_root=root) + _probe_call("have_qt6_cmake_package"),
        tmp_path,
    )
    assert result.returncode == 0, result.stderr
    assert "FOUND" in result.stdout


def test_qt_probe_finds_lib64_layout(tmp_path):
    # Fedora/openSUSE install the packages under lib64/cmake.
    root = tmp_path / "prefix"
    _qt_layout(root, lib="lib64")
    result = _run(
        _qt_probe(pinned_root=root) + _probe_call("have_qt6_cmake_package"),
        tmp_path,
    )
    assert result.returncode == 0, result.stderr
    assert "FOUND" in result.stdout


def test_qt_probe_honours_cmake_prefix_path(tmp_path):
    # A Qt in a custom prefix counts when the operator exported the hint CMake
    # itself reads, which is what the failure message now tells them to do.
    root = tmp_path / "opt" / "qt"
    _qt_layout(root)
    result = _run(
        _qt_probe(pinned_root=tmp_path / "empty") + _probe_call("have_qt6_cmake_package"),
        tmp_path,
        extra_env={"CMAKE_PREFIX_PATH": f"/nonexistent:{root}"},
    )
    assert result.returncode == 0, result.stderr
    assert "FOUND" in result.stdout


def test_qt_probe_reports_missing_svg_package(tmp_path):
    # qt6-svg-dev is a separate package: base-only must still install it, not
    # sail past into a build that fails at find_package(Qt6 ... Svg).
    root = tmp_path / "prefix"
    _qt_layout(root, svg=False)
    result = _run(
        _qt_probe(pinned_root=root) + _probe_call("have_qt6_dev"),
        tmp_path,
    )
    assert result.returncode == 0, result.stderr
    assert "MISSING" in result.stdout


def test_qt_probe_reports_missing_qt(tmp_path):
    result = _run(
        _qt_probe(pinned_root=tmp_path / "empty") + _probe_call("have_qt6_dev"),
        tmp_path,
    )
    assert result.returncode == 0, result.stderr
    assert "MISSING" in result.stdout


def test_pkg_config_probe_falls_back_to_pkgconf(tmp_path):
    # Debian renamed the binary to pkgconf; pkg-config is only a wrapper package
    # that need not be installed.
    script = (
        "set -uo pipefail\n"
        + _function("pkg_config_probe")
        + '\nif pkg_config_probe --exists openssl; then echo OK; else echo NO; fi\n'
    )
    result = _run(
        script, tmp_path,
        path_tools=[("pkgconf", '#!/bin/sh\ntest "$2" = openssl\n')],
    )
    assert result.returncode == 0, result.stderr
    assert "OK" in result.stdout


def test_pkg_config_probe_reports_unknown_without_any_pkg_config(tmp_path):
    script = (
        "set -uo pipefail\n"
        + _function("pkg_config_probe")
        + '\nif pkg_config_probe --exists openssl; then echo OK; else echo NO; fi\n'
    )
    result = _run(script, tmp_path)
    assert result.returncode == 0, result.stderr
    assert "NO" in result.stdout


def test_missing_age_without_package_manager_uses_macos_fallback(tmp_path):
    script = (
        "set -uo pipefail\n"
        "say() { :; }\n"
        "die() { echo \"DIED: $1\"; return 90; }\n"
        "PM=\nDIAG_MISSING=\nFORKMESH_NO_INSTALL_DEPS=0\nINSTALLED=0\n"
        + _function("fallback_installer_for")
        + _function("install_macos_age").replace(
            'install_macos_age() {',
            'real_install_macos_age() {',
            1,
        )
        + "install_macos_age() { INSTALLED=1; }\n"
        + _function("ensure")
        + "have_age_tools() { [ \"$INSTALLED\" = 1 ]; }\n"
        + "uname() { [ \"${1:-}\" = -s ] && echo Darwin || echo arm64; }\n"
        + "ensure age have_age_tools\n"
        + "printf '%s|%s\\n' \"$INSTALLED\" \"$DIAG_MISSING\"\n"
    )
    result = _run(script, tmp_path)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "1|age"


def test_macos_age_fallback_is_version_and_checksum_pinned():
    source = INSTALLER.read_text(encoding="utf-8")
    fallback = _function("install_macos_age")
    assert 'local version="1.3.1"' in fallback
    assert "age-v${version}-darwin-${arch}.tar.gz" in fallback
    assert "github.com/FiloSottile/age/releases/download/v${version}" in fallback
    assert "01120ea2cbf0463d4c6bd767f99f3271bbed1cdc8a9aa718a76ba1fe4f01998b" in fallback
    assert "2b233301ad21ab7b1eabd9ae1198a164005fa4928fcdd745d47c39f8593209d7" in fallback
    assert 'got="$(_sha256_file "$archive")"' in fallback
    assert '[ "$got" != "$expected" ]' in fallback
    assert source.index('got="$(_sha256_file "$archive")"') < source.index(
        'tar -xzf "$archive"'
    )


def _webengine_probe(pinned_root):
    package_probe = _function("have_qt6_cmake_package")
    assert SYSTEM_ROOTS in package_probe, "prefix list moved; update SYSTEM_ROOTS"
    package_probe = package_probe.replace(SYSTEM_ROOTS, f'"{pinned_root}"')
    return (
        "set -uo pipefail\n"
        + _function("pkg_config_probe")
        + package_probe
        + _function("have_qt6_webengine")
        + _probe_call("have_qt6_webengine")
    )


def test_webengine_probe_finds_the_cmake_package(tmp_path):
    root = tmp_path / "prefix"
    _qt_layout(root, multiarch=True)
    module = root / "lib" / "x86_64-linux-gnu" / "cmake" / "Qt6WebEngineWidgets"
    module.mkdir()
    (module / "Qt6WebEngineWidgetsConfig.cmake").write_text("", encoding="utf-8")
    result = _run(_webengine_probe(root), tmp_path)
    assert result.returncode == 0, result.stderr
    assert "FOUND" in result.stdout


def test_webengine_probe_reports_a_qt_without_webengine(tmp_path):
    root = tmp_path / "prefix"
    _qt_layout(root, multiarch=True)
    result = _run(_webengine_probe(root), tmp_path)
    assert result.returncode == 0, result.stderr
    assert "MISSING" in result.stdout


def test_webengine_packages_per_package_manager(tmp_path):
    script = "set -u\n" + _function("webengine_package") + (
        'for PM in apt dnf pacman zypper apk brew; do '
        'printf "%s=%s\\n" "$PM" "$(webengine_package)"; done\n'
    )
    result = _run(script, tmp_path)
    assert result.returncode == 0, result.stderr
    assert result.stdout.split() == [
        "apt=qt6-webengine-dev",
        "dnf=qt6-qtwebengine-devel",
        "pacman=qt6-webengine",
        "zypper=qt6-webengine-devel",
        "apk=qt6-qtwebengine-dev",
        "brew=",
    ]


def test_failed_webengine_install_only_warns(tmp_path):
    script = (
        "set -uo pipefail\n"
        "say() { echo \"SAY: $1\"; }\n"
        "warn() { echo \"WARN: $1\"; }\n"
        "die() { echo \"DIED: $1\"; exit 90; }\n"
        "need_sudo() { return 0; }\n"
        "have_qt6_webengine() { return 1; }\n"
        "pm_install() { echo \"INSTALL: $*\"; return 1; }\n"
        "uname() { echo Linux; }\n"
        "PM=apt\nFORKMESH_NO_INSTALL_DEPS=0\n"
        + _function("webengine_package")
        + _function("ensure_webengine")
        + "ensure_webengine\necho DONE\n"
    )
    result = _run(script, tmp_path, extra_env={"DISPLAY": ":0"})
    assert result.returncode == 0, result.stderr
    assert "INSTALL: qt6-webengine-dev" in result.stdout
    assert "WARN: Could not install qt6-webengine-dev" in result.stdout
    assert "DIED" not in result.stdout
    assert "DONE" in result.stdout


def test_source_build_installs_webengine_with_the_other_build_deps():
    source = INSTALLER.read_text(encoding="utf-8")
    deps = source.index("ensure qt have_qt6_dev\n")
    assert deps < source.index("ensure_webengine\n", deps) < source.index('diag deps 1 "', deps)

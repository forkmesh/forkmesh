#!/usr/bin/env python3
"""Installation support matrix and data-preserving lifecycle (issue #500).

The published matrix must match what the latest signed release actually ships,
and no installer path short of an explicit purge may remove an identity key or
a repository.
"""

import json
import os
from pathlib import Path
import re
import subprocess


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
MATRIX_PATH = ROOT / "www" / "docs" / "install-support-matrix.json"
MATRIX_DOC = ROOT / "www" / "docs" / "install-support-matrix.md"
LATEST = ROOT / ".forkmesh" / "releases" / "latest"
INSTALLER = ROOT / "www" / "public" / "install.sh"
UNINSTALLER = ROOT / "www" / "public" / "uninstall.sh"
TIERS = {"supported", "preview", "source-only", "planned"}
OPERATORS = {
    "desktop", "cli", "server", "container", "home-server", "cluster",
    "small-device",
}
ARTIFACT = re.compile(
    r"^forkmesh-(mirror-node-)?(linux|macos|windows|android)-"
    r"(x86_64|arm64|universal)(\.exe|\.AppImage|\.dmg|-setup\.exe|\.apk)?$"
)


def _matrix():
    return json.loads(MATRIX_PATH.read_text(encoding="utf-8"))


def _latest_assets():
    names = set()
    for line in (LATEST / "SHASUMS256.txt").read_text().splitlines():
        if line.strip():
            names.add(line.split()[-1])
    return names


def test_matrix_covers_every_operator_profile_with_a_known_tier():
    matrix = _matrix()
    assert set(matrix["tiers"]) == TIERS
    ids = [p["id"] for p in matrix["profiles"]]
    assert len(ids) == len(set(ids))
    assert {p["operator"] for p in matrix["profiles"]} == OPERATORS
    for profile in matrix["profiles"]:
        assert profile["tier"] in TIERS, profile["id"]
        assert profile["install"] and profile["signing"], profile["id"]
        for name in profile["artifacts"]:
            assert ARTIFACT.match(name), name
        for path in profile["evidence"]:
            assert (ROOT / path).is_file(), (profile["id"], path)


def test_supported_profiles_ship_in_the_latest_signed_release():
    assets = _latest_assets()
    manifest = json.loads((LATEST / "release.json").read_text())
    assert manifest["schema"] == "forkmesh-release-v2"
    assert (LATEST / "release.json.sig").is_file()
    manifest_assets = {a["name"] for a in manifest["assets"]}
    for profile in _matrix()["profiles"]:
        if profile["tier"] != "supported":
            continue
        assert profile["artifacts"], profile["id"]
        assert profile["evidence"], profile["id"]
        for name in profile["artifacts"]:
            assert name in assets, (profile["id"], name)
            assert name in manifest_assets, (profile["id"], name)


def test_matrix_document_lists_every_profile_and_lifecycle_command():
    doc = MATRIX_DOC.read_text(encoding="utf-8")
    matrix = _matrix()
    for profile in matrix["profiles"]:
        assert f"`{profile['id']}`" in doc, profile["id"]
    assert "FORKMESH_RELEASE=candidate-<version>" in doc
    assert "--purge --yes" in doc
    assert 'RELEASE_CHANNEL="${FORKMESH_RELEASE:-latest}"' in INSTALLER.read_text()


def _sandbox(tmp_path):
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    # Never signal or reach anything on the machine running the test.
    for tool in ("pgrep", "pkill", "sudo", "systemctl", "curl"):
        (stubs / tool).write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        (stubs / tool).chmod(0o755)
    home = tmp_path / "home"
    identity = home / ".local/share/ForkMesh/identity/node.key"
    repo = home / ".local/share/ForkMesh/mirrors/alice/project/HEAD"
    settings = home / ".config/ForkMesh/ForkMesh.conf"
    diagnostics = home / ".forkmesh/diagnostics/actions.jsonl"
    binary = home / ".local/bin/forkmesh"
    launcher = home / ".local/share/applications/forkmesh.desktop"
    cache = home / ".cache/ForkMesh/catalog.json"
    for path, text in (
        (identity, "ed25519-secret"), (repo, "ref: refs/heads/main\n"),
        (settings, "[node]\nname=alice\n"), (diagnostics, "{}\n"),
        (binary, "old binary"), (launcher, "[Desktop Entry]\n"),
        (cache, "{}"),
    ):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    env = {
        "PATH": f"{stubs}{os.pathsep}/usr/bin{os.pathsep}/bin",
        "HOME": str(home),
    }
    data = {identity: "ed25519-secret", repo: "ref: refs/heads/main\n",
            settings: "[node]\nname=alice\n", diagnostics: "{}\n"}
    return env, data, (binary, launcher, cache)


def _run(script, args, env):
    return subprocess.run(
        ["bash", str(script), *args], env=env, text=True,
        capture_output=True, stdin=subprocess.DEVNULL, check=False,
    )


def _assert_uninstall_preserves_data(tmp_path, script, args):
    env, data, program = _sandbox(tmp_path)
    result = _run(script, args, env)
    assert result.returncode == 0, result.stderr
    for path, text in data.items():
        assert path.read_text(encoding="utf-8") == text, path
    for path in program:
        assert not path.exists(), path
    assert "identity and repositories were kept" in result.stdout


def test_standalone_uninstall_keeps_identity_and_repositories(tmp_path):
    _assert_uninstall_preserves_data(tmp_path, UNINSTALLER, [])


def test_installer_uninstall_keeps_identity_and_repositories(tmp_path):
    _assert_uninstall_preserves_data(tmp_path, INSTALLER, ["--uninstall"])


def test_purge_refuses_without_confirmation_and_changes_nothing(tmp_path):
    for script, args in ((UNINSTALLER, ["--purge"]),
                         (INSTALLER, ["--uninstall", "--purge"])):
        sandbox = tmp_path / script.stem
        sandbox.mkdir()
        env, data, program = _sandbox(sandbox)
        result = _run(script, args, env)
        assert result.returncode != 0, script
        assert "without confirmation" in result.stderr
        for path in [*data, *program]:
            assert path.exists(), path


def test_confirmed_purge_erases_user_data(tmp_path):
    for script, args in ((UNINSTALLER, ["--purge", "--yes"]),
                         (INSTALLER, ["--purge", "--uninstall", "--yes"])):
        sandbox = tmp_path / script.stem
        sandbox.mkdir()
        env, data, program = _sandbox(sandbox)
        result = _run(script, args, env)
        assert result.returncode == 0, result.stderr
        for path in [*data, *program]:
            assert not path.exists(), path
        assert "completely removed" in result.stdout


def test_install_and_upgrade_paths_never_touch_user_data_directories():
    # Only uninstall_forkmesh (purge or confirmed reinstall) may name the
    # ForkMesh settings/data directories; every install, upgrade and rollback
    # path replaces program files alone.
    source = INSTALLER.read_text(encoding="utf-8")
    start = source.index("uninstall_forkmesh() {")
    end = source.index("\n}\n", start)
    outside = source[:start] + source[end:]
    for needle in ('$config_home/ForkMesh', '$data_home/ForkMesh',
                   '$cache_home/ForkMesh', "$HOME/.forkmesh"):
        assert needle not in outside, needle
    # Known exception, still open under issue #500: the one-time migration off
    # the retired installer-owned Qt server service erases that service's
    # identity under /var/lib/forkmesh. Pin it so no second one appears.
    data_erasures = re.findall(r'rm -rf -- [^\n]*\.config/ForkMesh', outside)
    assert data_erasures == ['rm -rf -- "$state_dir/.config/ForkMesh'], data_erasures
    legacy = outside.index('rm -rf -- "$state_dir/.config/ForkMesh')
    assert outside.rfind('local legacy_unit="/etc/systemd/system/forkmesh-node.service"',
                         0, legacy) != -1
    body = source[start:end]
    assert 'if [ "$purge" = "1" ]; then dirs+=("${data_dirs[@]}"); fi' in body
    assert 'if [ "$purge" != "1" ]; then' in body
    assert body.index('if [ "$purge" != "1" ]; then') < body.index(
        'rm -rf -- "$SYSTEMD_STATE_DIR"')


def test_hosts_panel_uninstall_opts_into_purge():
    # Its confirmation promises the identity key and repositories are removed.
    source = "".join(
        (ROOT / "desktop" / "src" / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    start = source.index("void MainWindow::runHostUninstall()")
    body = source[start:source.index("\n}\n", start)]
    assert "FORKMESH_ASSUME_YES=1 FORKMESH_PURGE=1 bash" in body


# Every destructive mode of both scripts. A reinstall purges before it installs.
WIPES = ((UNINSTALLER, ["--purge", "--yes"]),
         (INSTALLER, ["--purge", "--uninstall", "--yes"]),
         (INSTALLER, ["--reinstall"]))


def _layout_elsewhere(root):
    # A projects-layout tree the sandboxed HOME does not own. Everything a
    # broken guard could erase lives under tmp_path, never the real machine.
    layout = root / "projects" / "forkmesh"
    kept = (layout / "data" / "identity" / "ed25519.pem",
            layout / "config" / "ForkMesh" / "ForkMesh.conf",
            layout / "ssh" / "vultr_mirror_ed25519")
    for path in kept:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("keep\n", encoding="utf-8")
    return layout, kept


def test_wipe_refuses_a_forkmesh_home_outside_home(tmp_path):
    # 2026-09-22: a test sandboxed HOME but inherited the desktop's exported
    # FORKMESH_HOME, and a reinstall erased the owner's real data/config/ssh.
    for index, (script, args) in enumerate(WIPES):
        sandbox = tmp_path / f"{script.stem}-{index}"
        sandbox.mkdir()
        env, data, program = _sandbox(sandbox)
        layout, kept = _layout_elsewhere(sandbox / "elsewhere")
        env["FORKMESH_HOME"] = str(layout)
        result = _run(script, args, env)
        assert result.returncode != 0, (script, args)
        assert "outside HOME" in result.stderr, result.stderr
        for path in [*kept, *data, *program]:
            assert path.exists(), path


def test_agent_shell_cannot_wipe_the_account_home(tmp_path):
    for index, (script, args) in enumerate(WIPES):
        sandbox = tmp_path / f"{script.stem}-{index}"
        sandbox.mkdir()
        env, data, program = _sandbox(sandbox)
        # The account's passwd home IS this HOME, as in a real agent shell.
        getent = sandbox / "stubs" / "getent"
        getent.write_text(
            f"#!/bin/sh\necho 'u:x:1001:1001::{sandbox / 'home'}:/bin/sh'\n",
            encoding="utf-8")
        getent.chmod(0o755)
        env["CLAUDECODE"] = "1"
        result = _run(script, args, env)
        assert result.returncode != 0, (script, args)
        assert "AI agent" in result.stderr, result.stderr
        for path in [*data, *program]:
            assert path.exists(), path


def test_agent_shell_may_still_wipe_a_sandboxed_home(tmp_path):
    # Tests run by agents keep working: a HOME that is not the account's own
    # passwd home is a sandbox, and purging it is the point of the test.
    env, data, program = _sandbox(tmp_path)
    env["CLAUDECODE"] = "1"
    result = _run(UNINSTALLER, ["--purge", "--yes"], env)
    assert result.returncode == 0, result.stderr
    for path in [*data, *program]:
        assert not path.exists(), path

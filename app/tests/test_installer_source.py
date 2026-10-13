#!/usr/bin/env python3
"""Installer source-selection regression checks (stdlib only)."""

import os
from pathlib import Path
import subprocess
import tempfile


INSTALLER = Path(__file__).resolve().parents[2] / "www" / "public" / "install.sh"


def _base_env():
    env = os.environ.copy()
    # ForkMesh action runs set FORKMESH_REPO for release metadata. install.sh
    # intentionally treats it as an explicit source override, so source-selection
    # unit tests must not inherit it accidentally.
    env.pop("FORKMESH_REPO", None)
    # Never let the owner's real projects layout reach a script under test
    # (conftest scrubs these too; this helper is also imported standalone).
    env.pop("FORKMESH_HOME", None)
    env.pop("FORKMESH_PROJECTS_DIR", None)
    return env


def test_source_build_parallelism_preserves_live_host_memory():
    source = INSTALLER.read_text(encoding="utf-8")
    start = source.index("build_client() {")
    end = source.index("\n}\n\ninstall_client()", start)
    build = source[start:end]

    assert '[ "$jobs" -gt 4 ] && jobs=4' in build
    assert "MemAvailable:" in build
    assert "available_kb - reserve_kb" in build
    assert build.index("MemAvailable:") < build.index(
        'cmake --build "$build_dir" -j"$jobs"'
    )


def _selection_prefix():
    script = INSTALLER.read_text(encoding="utf-8")
    marker = "# --- privilege escalation"
    assert marker in script
    # install.sh is intentionally wrapped in a top-level brace group so a
    # piped ``curl | bash`` cannot have its input consumed by a child process.
    # This harness cuts it before the group closes, therefore it must close the
    # otherwise complete prefix before Bash can parse or execute the assertions.
    return script.split(marker, 1)[0] + (
        # install.sh deliberately wraps its body in one outer brace group so
        # curl-piped child processes cannot consume the remaining script from
        # stdin. Close that group in this sliced harness before appending probes.
        '\n}\nprintf "REPO=%s\\n" "$REPO"\n'
        'printf "CANDIDATES=%s\\n" "${REPO_CANDIDATES[*]}"\n'
    )


def _run_with_response(response):
    with tempfile.TemporaryDirectory() as tmp:
        bindir = Path(tmp) / "bin"
        bindir.mkdir()
        curl = bindir / "curl"
        curl.write_text(
            """#!/bin/sh
printf '%s\\n' "$FORKMESH_TEST_RESPONSE"
""",
            encoding="utf-8",
        )
        curl.chmod(0o755)
        env = _base_env()
        env["FORKMESH_TEST_RESPONSE"] = response
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        return subprocess.run(
            ["bash", "-c", _selection_prefix()], env=env,
            text=True, capture_output=True, check=False,
        )


def test_installer_uses_selected_online_node():
    result = _run_with_response(
        '{"ok":true,"node":"forkmesh","repo":"forkmesh","totalMinutes":486}'
    )
    assert result.returncode == 0, result.stderr
    assert "REPO=https://forkmesh.com/forkmesh/forkmesh" in result.stdout


def test_installer_uses_first_of_ranked_node_list():
    # Newer mainnode returns ranked nodes for display; cloning uses the stable
    # flagship route, whose Worker router applies that live endpoint failover.
    result = _run_with_response(
        '{"ok":true,"node":"alpha","nodes":["alpha","bravo","charlie"],'
        '"repo":"forkmesh","totalMinutes":500}'
    )
    assert result.returncode == 0, result.stderr
    assert "REPO=https://forkmesh.com/forkmesh/forkmesh" in result.stdout
    assert "CANDIDATES=https://forkmesh.com/forkmesh/forkmesh" in result.stdout


def test_installer_drops_malformed_and_duplicate_nodes():
    # A bad node id (slashes) is rejected and a repeated id collapses, but the
    # server-ranked order of the survivors is preserved.
    result = _run_with_response(
        '{"ok":true,"node":"alpha","nodes":["alpha","bad/../id","alpha","bravo"],'
        '"repo":"forkmesh","totalMinutes":1}'
    )
    assert result.returncode == 0, result.stderr
    assert "CANDIDATES=https://forkmesh.com/forkmesh/forkmesh" in result.stdout


def test_installer_falls_back_to_single_node_field():
    # An older mainnode without a "nodes" array still resolves via "node", and
    # that single mirror becomes the only clone candidate.
    result = _run_with_response(
        '{"ok":true,"node":"solo","repo":"forkmesh","totalMinutes":7}'
    )
    assert result.returncode == 0, result.stderr
    assert "REPO=https://forkmesh.com/forkmesh/forkmesh" in result.stdout
    assert "CANDIDATES=https://forkmesh.com/forkmesh/forkmesh" in result.stdout


def _run_prefix(response, extra_env):
    # Like _run_with_response but sandboxes HOME/XDG so uninstall_forkmesh can be
    # exercised (it rm's under $HOME) without touching the real machine, and lets
    # a test set extra env (FORKMESH_REINSTALL, FORKMESH_OWNER, ...).
    with tempfile.TemporaryDirectory() as tmp:
        bindir = Path(tmp) / "bin"
        bindir.mkdir()
        curl = bindir / "curl"
        curl.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$FORKMESH_TEST_RESPONSE\"\n",
            encoding="utf-8",
        )
        curl.chmod(0o755)
        # Reinstall exercises uninstall_forkmesh(), which may otherwise call the
        # host's real pgrep/pkill and signal the ForkMesh app that is running
        # this action. In this unit harness there is no daemon to stop.
        pgrep = bindir / "pgrep"
        pgrep.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        pgrep.chmod(0o755)
        pkill = bindir / "pkill"
        pkill.write_text(
            "#!/bin/sh\necho 'unexpected pkill in installer unit test' >&2\nexit 99\n",
            encoding="utf-8",
        )
        pkill.chmod(0o755)
        home = Path(tmp) / "home"
        home.mkdir()
        env = _base_env()
        env["FORKMESH_TEST_RESPONSE"] = response
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        env["HOME"] = str(home)
        env["XDG_DATA_HOME"] = str(home / ".local/share")
        env["XDG_CONFIG_HOME"] = str(home / ".config")
        env["XDG_CACHE_HOME"] = str(home / ".cache")
        env.update(extra_env)
        return subprocess.run(
            ["bash", "-c", _selection_prefix()], env=env,
            text=True, capture_output=True, check=False,
        )


def test_installer_reinstall_wipes_then_continues():
    # --reinstall / FORKMESH_REINSTALL=1 runs the uninstall wipe first, then falls
    # through to the normal install (the source resolution still happens), rather
    # than exiting after the uninstall (adhoc #258).
    result = _run_prefix(
        '{"ok":true,"node":"forkmesh","repo":"forkmesh","totalMinutes":9}',
        {"FORKMESH_REINSTALL": "1", "FORKMESH_NO_LAUNCH": "1"},
    )
    assert result.returncode == 0, result.stderr
    assert "Reinstall requested" in result.stdout
    # It carried on into the install (resolved a source) instead of exiting.
    assert "REPO=https://forkmesh.com/forkmesh/forkmesh" in result.stdout


def test_installer_echoes_owner_when_attached():
    # The Hosts panel passes FORKMESH_OWNER so the operator can confirm which
    # account the fresh node is attached to (adhoc #258); the installer echoes it.
    result = _run_prefix(
        '{"ok":true,"node":"newnewnode","repo":"forkmesh","totalMinutes":9}',
        {"FORKMESH_OWNER": "alice", "FORKMESH_NODE_NAME": "vps-1"},
    )
    assert result.returncode == 0, result.stderr
    assert "Owner:  alice" in result.stdout
    assert "Node:   vps-1" in result.stdout


def test_installer_rejects_dangerous_source_targets():
    result = _run_prefix(
        '{"ok":true,"node":"forkmesh","repo":"forkmesh"}',
        {"FORKMESH_DIR": "/"},
    )
    assert result.returncode != 0
    assert "unsafe FORKMESH_DIR target" in result.stderr


def test_installer_refuses_unmanaged_existing_custom_source(tmp_path):
    source = tmp_path / "forkmesh" / "src"
    source.mkdir(parents=True)
    sentinel = source / "operator-data"
    sentinel.write_text("preserve me", encoding="utf-8")
    result = _run_prefix(
        '{"ok":true,"node":"forkmesh","repo":"forkmesh"}',
        {"FORKMESH_DIR": str(source)},
    )
    assert result.returncode != 0
    assert "not marked as an owner-matched ForkMesh install" in result.stderr
    assert sentinel.read_text(encoding="utf-8") == "preserve me"


def test_installer_refuses_symlink_source_target(tmp_path):
    real = tmp_path / "real"
    real.mkdir()
    source = tmp_path / "forkmesh" / "src"
    source.parent.mkdir()
    source.symlink_to(real, target_is_directory=True)
    result = _run_prefix(
        '{"ok":true,"node":"forkmesh","repo":"forkmesh"}',
        {"FORKMESH_DIR": str(source)},
    )
    assert result.returncode != 0
    assert "non-symlink path" in result.stderr or "may not be a symlink" in result.stderr


def test_installer_stops_when_no_mirror_is_online():
    result = _run_with_response(
        '{"ok":false,"error":"no_online_install_source"}'
    )
    assert result.returncode != 0
    assert "No online ForkMesh node is currently mirroring 'forkmesh'" in result.stderr
    assert "REPO=" not in result.stdout


def test_installer_skips_mirror_lookup_when_uploading_a_binary():
    # Direct-upload installs (adhoc #67/#70) stream the binary over the same SSH
    # session, so a missing/offline mirror must not block them: resolve_install_node
    # (and its curl call to the mainnode) must not run at all when
    # FORKMESH_LOCAL_BINARY is set, even if no mirror is actually online.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        marker = tmp / "resolve_curl_called"
        curl = bindir / "curl"
        curl.write_text(
            f"#!/bin/sh\ntouch {marker}\n"
            'printf \'%s\\n\' "$FORKMESH_TEST_RESPONSE"\n',
            encoding="utf-8",
        )
        curl.chmod(0o755)
        env = _base_env()
        env["FORKMESH_TEST_RESPONSE"] = '{"ok":false,"error":"no_online_install_source"}'
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        env["FORKMESH_NO_DIAG"] = "1"  # isolate resolve_install_node's curl use
        env["FORKMESH_LOCAL_BINARY"] = "/tmp/fake-forkmesh-upload"
        result = subprocess.run(
            ["bash", "-c", _selection_prefix()], env=env,
            text=True, capture_output=True, check=False,
        )
        assert result.returncode == 0, result.stderr
        assert "REPO=\n" in result.stdout
        assert "CANDIDATES=\n" in result.stdout
        assert not marker.exists()


def test_installer_still_resolves_a_mirror_lazily_if_upload_is_unusable():
    # install_prebuilt_release calls ensure_mirror_candidates itself, so a local
    # upload that turns out to be unusable (bad platform, empty file, ...) still
    # falls back to a real mirror lookup instead of leaving REPO_CANDIDATES empty.
    prefix = _selection_prefix() + (
        "\nensure_mirror_candidates\n"
        'printf "REPO2=%s\\n" "$REPO"\n'
        'printf "CANDIDATES2=%s\\n" "${REPO_CANDIDATES[*]}"\n'
    )
    with tempfile.TemporaryDirectory() as tmp:
        bindir = Path(tmp) / "bin"
        bindir.mkdir()
        curl = bindir / "curl"
        curl.write_text(
            """#!/bin/sh
printf '%s\\n' "$FORKMESH_TEST_RESPONSE"
""",
            encoding="utf-8",
        )
        curl.chmod(0o755)
        env = _base_env()
        env["FORKMESH_TEST_RESPONSE"] = (
            '{"ok":true,"node":"fallback-node","repo":"forkmesh","totalMinutes":1}'
        )
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        env["FORKMESH_LOCAL_BINARY"] = "/tmp/fake-forkmesh-upload"
        result = subprocess.run(
            ["bash", "-c", prefix], env=env, text=True,
            capture_output=True, check=False,
        )
        assert result.returncode == 0, result.stderr
        # Skipped up front (matches the previous test) ...
        assert "REPO=\n" in result.stdout
        assert "CANDIDATES=\n" in result.stdout
        # ... but resolves once something actually needs a mirror.
        assert "REPO2=https://forkmesh.com/forkmesh/forkmesh" in result.stdout
        assert (
            "CANDIDATES2=https://forkmesh.com/forkmesh/forkmesh"
            in result.stdout
        )


def _clone_functions():
    # repo_node + classify_clone_failure + clean_clone, sliced out so the
    # multi-mirror fallback can be driven directly with a fake `git`.
    script = INSTALLER.read_text(encoding="utf-8")
    start = script.index("# Extract the node segment")
    end = script.index("fetch_source() {")
    helper = script.index("fm_safe_rm_tree() {")
    helper_end = script.index("\n}\n", helper) + 3
    marker = script.index("_write_managed_marker() {")
    marker_end = script.index("\n}\n", marker) + 3
    return script[helper:helper_end] + script[marker:marker_end] + script[start:end]


def test_clean_clone_falls_back_past_unreachable_mirror():
    # The whole point of the fix: when the best mirror's git tunnel times out
    # (504), the installer must try the next online mirror instead of failing.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        # Fake git: the first mirror (alpha) returns the relay's 504; the second
        # (bravo) "clones" by creating the destination directory.
        git = bindir / "git"
        git.write_text(
            "#!/bin/sh\n"
            'url=$4; dest=$5\n'
            'case "$url" in\n'
            '  *alpha*)\n'
            '    echo "remote: Host timed out."\n'
            "    echo \"fatal: unable to access '$url': The requested URL returned error: 504\"\n"
            '    exit 1 ;;\n'
            '  *) mkdir -p "$dest"; exit 0 ;;\n'
            'esac\n',
            encoding="utf-8",
        )
        git.chmod(0o755)
        src = tmp / "share" / "forkmesh" / "src"
        harness = (
            "set -euo pipefail\n"
            'say()  { printf "==> %s\\n" "$1"; }\n'
            'warn() { printf "Warning: %s\\n" "$1" >&2; }\n'
            'dbg()  { :; }\n'
            'MANAGED_MARKER=".forkmesh-managed"\n'
            "PIN_FAILURE=0\n"
            'CLONE_FAIL_REASON=""\n'
            f'SRC="{src}"\n'
            'REPO=""\n'
            'REPO_CANDIDATES=("https://forkmesh.com/alpha/forkmesh" '
            '"https://forkmesh.com/bravo/forkmesh")\n'
            + _clone_functions()
            + 'mkdir -p "$(dirname "$SRC")"\n'
            # Call via `if` like the real fetch_source does, so `set -e` is
            # disabled inside clean_clone (a failing clone is handled, not fatal).
            "if clone_ok=1 && clean_clone; then :; else clone_ok=0; fi\n"
            'printf "RESULT_REPO=%s\\n" "$REPO"\n'
            'printf "CLONE_OK=%s\\n" "$clone_ok"\n'
        )
        env = _base_env()
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        result = subprocess.run(
            ["bash", "-c", harness], env=env, text=True,
            capture_output=True, check=False,
        )
        assert result.returncode == 0, result.stderr + result.stdout
        assert "CLONE_OK=1" in result.stdout
        # Fell through to the second mirror and recorded it as the working repo.
        assert "RESULT_REPO=https://forkmesh.com/bravo/forkmesh" in result.stdout
        # Named the unreachable mirror and the 504 reason for the operator.
        assert ("Mirror 'alpha' could not be cloned: "
                "mirror endpoint timed out (HTTP 504)") in result.stderr
        assert src.is_dir()


def test_clean_clone_reports_when_all_mirrors_time_out():
    # Every mirror down: clean_clone fails and leaves a 504 reason for the final
    # error/diagnostics, without falsely flagging an integrity-pin failure.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        git = bindir / "git"
        git.write_text(
            "#!/bin/sh\n"
            'echo "remote: Host timed out."\n'
            'echo "fatal: ... The requested URL returned error: 504"\n'
            "exit 1\n",
            encoding="utf-8",
        )
        git.chmod(0o755)
        src = tmp / "src"
        harness = (
            "set -euo pipefail\n"
            'say()  { :; }\n'
            'warn() { :; }\n'
            'dbg()  { :; }\n'
            'MANAGED_MARKER=".forkmesh-managed"\n'
            "PIN_FAILURE=0\n"
            'CLONE_FAIL_REASON=""\n'
            f'SRC="{src}"\n'
            'REPO=""\n'
            'REPO_CANDIDATES=("https://forkmesh.com/a/forkmesh" '
            '"https://forkmesh.com/b/forkmesh")\n'
            + _clone_functions()
            + 'mkdir -p "$(dirname "$SRC")"\n'
            "if clean_clone; then echo UNEXPECTED_OK; else echo CLONE_FAILED; fi\n"
            'printf "REASON=%s\\n" "$CLONE_FAIL_REASON"\n'
            'printf "PIN=%s\\n" "$PIN_FAILURE"\n'
        )
        env = _base_env()
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        result = subprocess.run(
            ["bash", "-c", harness], env=env, text=True,
            capture_output=True, check=False,
        )
        assert result.returncode == 0, result.stderr + result.stdout
        assert "CLONE_FAILED" in result.stdout
        assert "REASON=mirror endpoint timed out (HTTP 504)" in result.stdout
        assert "PIN=0" in result.stdout


def _function_text(script, name):
    start = script.index(name + "() {")
    return script[start:script.index("\n}\n", start) + 3]


def _fetch_harness(src, candidates):
    script = INSTALLER.read_text(encoding="utf-8")
    functions = "".join(_function_text(script, name) for name in (
        "fm_safe_rm_tree", "_path_owner_uid", "_marker_is_valid",
        "_write_managed_marker", "_move_src_aside"))
    start = script.index("# Extract the node segment")
    end = script.index("build_client() {")
    return (
        "set -uo pipefail\n"
        'say()  { printf "==> %s\\n" "$1"; }\n'
        'warn() { printf "Warning: %s\\n" "$1" >&2; }\n'
        'die()  { printf "Error: %s\\n" "$1" >&2; exit 1; }\n'
        'dbg()  { :; }\n'
        'MANAGED_MARKER=".forkmesh-managed"\n'
        "PIN_FAILURE=0\n"
        'CLONE_FAIL_REASON=""\n'
        f'SRC="{src}"\n'
        'REPO=""\n'
        "REPO_CANDIDATES=(" + " ".join(f'"{c}"' for c in candidates) + ")\n"
        + functions
        + 'owns_src() { _marker_is_valid "$SRC"; }\n'
        + script[start:end]
        + "if fetch_source; then echo FETCH_OK; else echo FETCH_FAILED; fi\n"
    )


def _sandboxed_env(tmp, bindir):
    env = _base_env()
    home = tmp / "home"
    home.mkdir(exist_ok=True)
    env["HOME"] = str(home)
    for name in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        env[name] = str(home / name.lower())
    env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
    return env


def _fake_git(bindir, body):
    git = bindir / "git"
    git.write_text("#!/bin/sh\n" + body, encoding="utf-8")
    git.chmod(0o755)


def test_failed_pull_leaves_the_managed_checkout_untouched():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        calls = tmp / "calls.log"
        _fake_git(bindir,
                  f'echo "$*" >> "{calls}"\n'
                  'case "$*" in *pull*) echo "fatal: The requested URL returned error: 503" >&2; exit 1 ;; esac\n'
                  'exit 0\n')
        src = tmp / "work" / "forkmesh" / "src"
        (src / ".git").mkdir(parents=True)
        (src / "desktop").mkdir()
        (src / "desktop" / "CMakeLists.txt").write_text("x", encoding="utf-8")
        (src / "my-branch-work.txt").write_text("keep", encoding="utf-8")
        uid = os.getuid()
        (src / ".forkmesh-managed").write_text(
            f"forkmesh-managed-v1\nuid={uid}\npath={src}\n", encoding="utf-8")
        result = subprocess.run(
            ["bash", "-c", _fetch_harness(src, ["https://m/a/forkmesh", "https://m/b/forkmesh"])],
            env=_sandboxed_env(tmp, bindir), text=True, capture_output=True, check=False)
        assert "FETCH_FAILED" in result.stdout, result.stdout + result.stderr
        assert (src / "my-branch-work.txt").read_text() == "keep"
        assert (src / ".git").is_dir()
        log = calls.read_text()
        assert "clone" not in log
        assert log.count("pull --ff-only") == 2
        assert "nothing was deleted or re-cloned" in result.stderr
        assert not list(src.parent.glob("src.replaced-*"))


def test_foreign_checkout_stops_before_any_clone():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        calls = tmp / "calls.log"
        _fake_git(bindir, f'echo "$*" >> "{calls}"\nexit 0\n')
        src = tmp / "work" / "src"
        (src / ".git").mkdir(parents=True)
        (src / "notes.txt").write_text("mine", encoding="utf-8")
        result = subprocess.run(
            ["bash", "-c", _fetch_harness(src, ["https://m/a/forkmesh"])],
            env=_sandboxed_env(tmp, bindir), text=True, capture_output=True, check=False)
        assert "FETCH_FAILED" in result.stdout, result.stdout + result.stderr
        assert (src / "notes.txt").read_text() == "mine"
        assert not calls.exists()
        assert "FORKMESH_DIR" in result.stderr


def test_incomplete_managed_checkout_is_moved_aside_not_deleted():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        _fake_git(bindir,
                  'case "$1" in clone) mkdir -p "$5/desktop" "$5/.git"; '
                  'echo x > "$5/desktop/CMakeLists.txt" ;; esac\nexit 0\n')
        src = tmp / "work" / "src"
        (src / ".git").mkdir(parents=True)
        (src / "local-only.txt").write_text("keep", encoding="utf-8")
        uid = os.getuid()
        (src / ".forkmesh-managed").write_text(
            f"forkmesh-managed-v1\nuid={uid}\npath={src}\n", encoding="utf-8")
        result = subprocess.run(
            ["bash", "-c", _fetch_harness(src, ["https://m/a/forkmesh"])],
            env=_sandboxed_env(tmp, bindir), text=True, capture_output=True, check=False)
        assert "FETCH_OK" in result.stdout, result.stdout + result.stderr
        aside = list(src.parent.glob("src.replaced-*"))
        assert len(aside) == 1
        assert (aside[0] / "local-only.txt").read_text() == "keep"
        assert (src / "desktop" / "CMakeLists.txt").is_file()
        assert not (src.parent / "tmp").exists()


def test_clone_refuses_a_non_empty_target():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        calls = tmp / "calls.log"
        _fake_git(bindir, f'echo "$*" >> "{calls}"\nexit 0\n')
        src = tmp / "work" / "src"
        src.mkdir(parents=True)
        (src / "stray.txt").write_text("x", encoding="utf-8")
        script = _fetch_harness(src, ["https://m/a/forkmesh"]).replace(
            "if fetch_source; then", "if clean_clone; then")
        result = subprocess.run(["bash", "-c", script], env=_sandboxed_env(tmp, bindir),
                                text=True, capture_output=True, check=False)
        assert "FETCH_FAILED" in result.stdout
        assert (src / "stray.txt").read_text() == "x"
        assert not calls.exists()


def test_installer_never_deletes_the_source_checkout():
    script = INSTALLER.read_text(encoding="utf-8")
    assert "_remove_managed_src" not in script
    assert 'rm -rf -- "$SRC"' not in script
    assert 'rm -rf "$SRC"' not in script
    retry = script[script.index("if ! attempt_install; then"):]
    retry = retry[:retry.index("fi  # end source-build pipeline")]
    assert "rm " not in retry and "_move_src_aside" not in retry
    uninstall = _function_text(script, "uninstall_forkmesh")
    assert "Kept the source checkout" in uninstall
    uninstaller = (INSTALLER.parent / "uninstall.sh").read_text(encoding="utf-8")
    assert '"$data_home/forkmesh"' not in uninstaller
    assert "Kept the source checkout" in uninstaller

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _fake_node(path: Path, version: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!/usr/bin/env sh\nprintf '{version}\\n'\n")
    path.chmod(0o755)
    return path


def _bootstrap_node(tmp_path: Path, script_body: str, home: Path):
    script = f"""
        set -eu
        export HOME='{home.as_posix()}'
        unset NVM_DIR FORKMESH_NODE_BIN || true
        export PYWRANGLER_VENV='{(tmp_path / ".pywrangler").as_posix()}'
        PATH='{(tmp_path / "sysbin").as_posix()}:/usr/bin:/bin'
        . ./pywrangler.sh
        {script_body}
    """
    return subprocess.run(
        ["bash", "-c", script],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def test_pywrangler_wrapper_does_not_detect_itself_as_path_binary():
    script = """
        set -eu
        PATH='/usr/bin:/bin'
        . ./pywrangler.sh
        if type -P pywrangler >/dev/null 2>&1; then
            exit 77
        fi
        command -v pywrangler
    """
    result = subprocess.run(
        ["bash", "-c", script],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "pywrangler"


def test_relative_venv_npx_wrapper_does_not_reexec_itself(tmp_path):
    sysbin = tmp_path / "sysbin"
    _fake_node(sysbin / "node", "v22.0.0")
    npx = sysbin / "npx"
    npx.write_text("#!/bin/sh\nprintf 'system-npx:%s\\n' \"$*\"\n")
    npx.chmod(0o755)

    script = f"""
        set -eu
        cd '{tmp_path.as_posix()}'
        export PYWRANGLER_VENV='.pywrangler'
        PATH='{sysbin.as_posix()}:/usr/bin:/bin'
        . '{(ROOT / "pywrangler.sh").as_posix()}'
        pywrangler secret list --env "" --name forkmesh-app
    """
    result = subprocess.run(
        ["bash", "-c", script],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == (
        "system-npx:--yes --package wrangler@4.136.2 -- "
        "wrangler secret list --env  --name forkmesh-app"
    )


def test_pywrangler_uses_local_venv_uvx_without_system_uv(tmp_path):
    venv = tmp_path / ".pywrangler"
    uvx = venv / "bin" / "uvx"
    uvx.parent.mkdir(parents=True)
    uvx.write_text(
        "#!/usr/bin/env sh\n"
        "printf 'uvx-call:%s\\n' \"$*\"\n"
    )
    uvx.chmod(0o755)

    script = f"""
        set -eu
        export PYWRANGLER_VENV='{venv.as_posix()}'
        PATH='/usr/bin:/bin'
        . ./pywrangler.sh
        pywrangler deploy --env \"\"
    """
    result = subprocess.run(
        ["bash", "-c", script],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "uvx-call:--from workers-py<1.18.0 pywrangler deploy --env"


def test_pywrangler_rejects_moved_venv_entrypoint(tmp_path):
    venv = tmp_path / ".pywrangler"
    stale = venv / "bin" / "pywrangler"
    stale.parent.mkdir(parents=True)
    stale.write_text(
        "#!/path/from/before/repository-move/.pywrangler/bin/python\n"
        "exit 88\n"
    )
    stale.chmod(0o755)

    sysbin = tmp_path / "sysbin"
    _fake_node(sysbin / "node", "v22.0.0")
    uvx = sysbin / "uvx"
    uvx.write_text("#!/bin/sh\nprintf 'safe-uvx:%s\\n' \"$*\"\n")
    uvx.chmod(0o755)

    script = f"""
        set -eu
        export PYWRANGLER_VENV='{venv.as_posix()}'
        PATH='{sysbin.as_posix()}:/usr/bin:/bin'
        . ./pywrangler.sh
        pywrangler whoami
    """
    result = subprocess.run(
        ["bash", "-c", script],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "safe-uvx:--from workers-py<1.18.0 pywrangler whoami"
    assert "ignoring moved or broken pywrangler environment" in result.stderr


def test_no_node_pipeline_refuses_to_auto_install_pywrangler(tmp_path):
    venv = tmp_path / ".pywrangler"
    script = f"""
        set -eu
        export FORKMESH_NO_NODE_PACKAGES=1
        export PYWRANGLER_VENV='{venv.as_posix()}'
        PATH='/usr/bin:/bin'
        . ./pywrangler.sh
        pywrangler deploy --env ""
    """
    result = subprocess.run(
        ["bash", "-c", script],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 1
    assert "FORKMESH_NO_NODE_PACKAGES=1" in result.stderr
    assert not (venv / "bin" / "pywrangler").exists()


def test_node_too_old_for_wrangler_falls_back_to_a_version_manager(tmp_path):
    # Wrangler 4.x aborts on Node < 22, and distro packages sit on 20 for
    # years — the newer runtime is installed but only on PATH in a login shell.
    home = tmp_path / "home"
    _fake_node(tmp_path / "sysbin" / "node", "v20.19.2")
    _fake_node(home / ".nvm" / "versions" / "node" / "v22.9.0" / "bin" / "node", "v22.9.0")
    newest = _fake_node(
        home / ".nvm" / "versions" / "node" / "v22.23.1" / "bin" / "node", "v22.23.1"
    )

    result = _bootstrap_node(tmp_path, "command -v node", home)

    assert result.returncode == 0, result.stderr
    # Newest qualifying runtime wins, by numeric version — the glob lists
    # v22.23.1 *before* v22.9.0, so neither "first" nor "last" match works.
    assert result.stdout.strip() == newest.as_posix()
    assert "v22.23.1" in result.stderr


def test_node_already_new_enough_leaves_path_alone(tmp_path):
    home = tmp_path / "home"
    current = _fake_node(tmp_path / "sysbin" / "node", "v22.0.0")
    _fake_node(home / ".nvm" / "versions" / "node" / "v24.1.0" / "bin" / "node", "v24.1.0")

    result = _bootstrap_node(tmp_path, "command -v node", home)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == current.as_posix()
    assert result.stderr.strip() == ""


def test_missing_modern_node_warns_without_aborting_the_deploy(tmp_path):
    home = tmp_path / "home"
    _fake_node(tmp_path / "sysbin" / "node", "v20.19.2")

    # `set -eu` stands in for deploy.sh's `set -euo pipefail`: sourcing must not
    # kill the script, because a FORKMESH_NO_NODE_PACKAGES pipeline never needs
    # Node at all.
    result = _bootstrap_node(tmp_path, "echo sourced-ok", home)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "sourced-ok"
    assert "no Node >= v22 found" in result.stderr
    assert "FORKMESH_NODE_BIN" in result.stderr

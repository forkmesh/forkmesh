#!/usr/bin/env python3
"""Installer process-supervision and privilege-boundary checks."""

from pathlib import Path
import subprocess


INSTALLER = Path(__file__).resolve().parents[2] / "www" / "public" / "install.sh"


def _function(name):
    source = INSTALLER.read_text(encoding="utf-8")
    start = source.index(f"{name}() {{")
    end = source.index("\n}\n", start) + len("\n}\n")
    return source[start:end]


def _supervision_preamble(tmp_path):
    return f"""
set -u
say() {{ :; }}
warn() {{ printf 'WARN:%s\\n' "$1" >&2; }}
die() {{ printf 'ERROR:%s\\n' "$1" >&2; exit 1; }}
_canonical_path() {{ readlink -f -- "$1"; }}
BIN="{tmp_path}/forkmesh"
SRC="{tmp_path}/forkmesh/src"
PID_FILE="{tmp_path}/forkmesh.pid"
SYSTEMD_UNIT="{tmp_path}/forkmesh-node.service"
SYSTEMD_BIN="/usr/local/bin/forkmesh"
"""


def test_stop_targets_only_pidfile_instance(tmp_path):
    subprocess.run(["cp", "/bin/sleep", tmp_path / "forkmesh"], check=True)
    script = (
        _supervision_preamble(tmp_path)
        + _function("stop_forkmesh_daemons")
        + r"""
"$BIN" 30 &
target=$!
"$BIN" 30 &
other=$!
trap 'kill "$target" "$other" 2>/dev/null || true' EXIT
printf '%s\n' "$target" > "$PID_FILE"
stop_forkmesh_daemons test
if kill -0 "$target" 2>/dev/null; then exit 10; fi
kill -0 "$other" 2>/dev/null || exit 11
"""
    )
    result = subprocess.run(
        ["bash", "-c", script], text=True, capture_output=True, timeout=10
    )
    assert result.returncode == 0, result.stderr


def test_stop_rejects_pid_for_different_executable(tmp_path):
    subprocess.run(["cp", "/bin/true", tmp_path / "forkmesh"], check=True)
    script = (
        _supervision_preamble(tmp_path)
        + _function("stop_forkmesh_daemons")
        + r"""
/bin/sleep 30 &
other=$!
trap 'kill "$other" 2>/dev/null || true' EXIT
printf '%s\n' "$other" > "$PID_FILE"
if (stop_forkmesh_daemons test); then exit 12; fi
kill -0 "$other" 2>/dev/null || exit 13
"""
    )
    result = subprocess.run(
        ["bash", "-c", script], text=True, capture_output=True, timeout=10
    )
    assert result.returncode == 0, result.stderr
    assert "Refusing to signal" in result.stderr


def test_installer_has_no_global_process_kill_or_root_bypass():
    source = INSTALLER.read_text(encoding="utf-8")
    assert "pkill -x forkmesh" not in source
    assert "pkill -f" not in source
    assert 'args="--allow-root"' not in source
    assert "export FORKMESH_ALLOW_ROOT=1" not in source
    assert "User=%s" in source
    assert "NoNewPrivileges=true" in source

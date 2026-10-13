#!/usr/bin/env python3
"""Installer anonymous-diagnostics reporting checks (stdlib only).

Extracts the self-contained diagnostics block from install.sh (the `diag`
function and its setup) and drives it with a fake `curl` that records the POST
body, so we can assert the installer emits a well-formed, anonymous event and
honours the FORKMESH_NO_DIAG opt-out — without running a real install.
"""

import os
from pathlib import Path
import subprocess
import tempfile


INSTALLER = Path(__file__).resolve().parents[2] / "www" / "public" / "install.sh"


def _diag_block():
    script = INSTALLER.read_text(encoding="utf-8")
    start = script.index("# --- anonymous diagnostics")
    marker = "trap on_diag_exit EXIT"
    end = script.index(marker)
    return script[start:end] + marker + "\n"


def _run(diag_calls, no_diag=False):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        log = tmp / "diag.log"
        curl = bindir / "curl"
        # Fake curl: record only the JSON passed after --data, one line per call.
        curl.write_text(
            "#!/bin/sh\n"
            "data=\n"
            "while [ $# -gt 0 ]; do\n"
            '  if [ "$1" = --data ]; then shift; data=$1; fi\n'
            "  shift\n"
            "done\n"
            'printf "%s\\n" "$data" >> "$DIAG_LOG"\n',
            encoding="utf-8",
        )
        curl.chmod(0o755)
        harness = (
            "set -euo pipefail\n"
            'INSTALLER_VERSION="9.9.9 (test)"\n'
            'FORKMESH_DIAG_URL="https://example.test/api/install-diag"\n'
            + _diag_block()
            + diag_calls
            + "\nwait\n"  # let the backgrounded fake curl finish before exit
        )
        env = os.environ.copy()
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        env["DIAG_LOG"] = str(log)
        if no_diag:
            env["FORKMESH_NO_DIAG"] = "1"
        result = subprocess.run(
            ["bash", "-c", harness], env=env, text=True,
            capture_output=True, check=False,
        )
        assert result.returncode == 0, result.stderr
        return log.read_text(encoding="utf-8") if log.exists() else ""


def test_diag_emits_anonymous_event():
    out = _run('diag start 1 "none"')
    assert '"step":"start"' in out
    assert '"ok":1' in out
    assert '"run":"' in out
    assert '"version":"9.9.9 (test)"' in out
    assert '"detail":"none"' in out
    # Coarse platform facts are present; nothing account/email/IP-identifying is.
    assert '"os":"' in out and '"arch":"' in out


def test_diag_reports_failure_with_zero_ok():
    out = _run('diag build 0 "exit2"')
    assert '"step":"build"' in out
    assert '"ok":0' in out
    assert '"detail":"exit2"' in out


def test_opt_out_suppresses_all_reports():
    out = _run('diag start 1; diag done 1', no_diag=True)
    assert out == ""


def test_run_id_is_present_and_nonempty():
    out = _run("diag start 1").strip()
    # The run id is a random per-run token, not blank.
    assert '"run":""' not in out


def test_die_diag_reports_synchronously_before_exit():
    # A backgrounded POST can die with the shell; the fatal-path report must
    # already be written when the script exits (no `wait` here).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "bin"
        bindir.mkdir()
        log = tmp / "diag.log"
        curl = bindir / "curl"
        curl.write_text(
            "#!/bin/sh\n"
            "sleep 1\n"
            "data=\n"
            "while [ $# -gt 0 ]; do\n"
            '  if [ "$1" = --data ]; then shift; data=$1; fi\n'
            "  shift\n"
            "done\n"
            'printf "%s\\n" "$data" >> "$DIAG_LOG"\n',
            encoding="utf-8",
        )
        curl.chmod(0o755)
        harness = (
            "set -euo pipefail\n"
            'INSTALLER_VERSION="9.9.9 (test)"\n'
            'FORKMESH_DIAG_URL="https://example.test/api/install-diag"\n'
            + _diag_block()
            + "trap - EXIT\n"
            + 'die_diag prebuilt metadata-unreachable "Could not reach a mirror"\n'
        )
        env = os.environ.copy()
        env["PATH"] = str(bindir) + os.pathsep + env.get("PATH", "")
        env["DIAG_LOG"] = str(log)
        result = subprocess.run(
            ["bash", "-c", harness], env=env, text=True,
            capture_output=True, check=False,
        )
        assert result.returncode == 1
        assert "Could not reach a mirror" in result.stderr
        out = log.read_text(encoding="utf-8")
        assert '"step":"prebuilt"' in out
        assert '"ok":0' in out
        assert '"detail":"metadata-unreachable"' in out

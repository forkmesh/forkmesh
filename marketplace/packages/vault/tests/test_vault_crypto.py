"""The vault's JavaScript cryptography, checked against OpenSSL's answers."""

from pathlib import Path
import shutil
import subprocess
import sys

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
NODE = shutil.which("node")
needs_node = pytest.mark.skipif(NODE is None, reason="node is not installed")


def _node(script):
    return subprocess.run(
        [NODE, "--no-warnings", str(PACKAGE / "tests" / script)],
        capture_output=True, text=True, timeout=600,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})


@needs_node
def test_primitives_and_protocol_match_openssl_and_enforce_every_binding():
    result = _node("crypto_vectors.js")
    assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


@needs_node
def test_real_client_against_the_real_worker_module():
    result = _node("e2e_client.js")
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


@needs_node
def test_the_browser_opens_what_every_other_client_writes():
    """interop-from-*.json are committed by each port of suite fmv1."""
    result = _node("interop_check.js")
    assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
    for made_by in ("js", "cpp", "dart"):
        assert f"interop-from-{made_by}.json" in result.stdout, made_by


def test_every_copy_of_the_cryptography_is_the_source():
    result = subprocess.run(
        [sys.executable, str(PACKAGE / "tools" / "sync_crypto.py"), "--check"],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr

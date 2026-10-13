from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from forkmesh_host import request_guard


WWW = Path(__file__).resolve().parents[3] / "www"


def _vectors(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    out = tmp_path / "vectors.json"
    env = {
        key: value for key, value in os.environ.items()
        if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")
    }
    env.update({
        "HOME": str(tmp_path),
        "XDG_DATA_HOME": str(tmp_path / "data"),
        "XDG_CONFIG_HOME": str(tmp_path / "config"),
        "XDG_CACHE_HOME": str(tmp_path / "cache"),
        "FORKMESH_EDGE_VECTORS_OUT": str(out),
    })
    result = subprocess.run(
        [node, "--test", "--test-name-pattern", "cross-check vectors",
         "tests/app_origin.test.mjs"],
        cwd=WWW, env=env, text=True, capture_output=True, timeout=120,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(out.read_text())


def test_www_worker_header_verifies_with_the_host(tmp_path):
    vectors = _vectors(tmp_path)
    assert len(vectors) == 3
    for vector in vectors:
        header = vector["header"]
        secret = vector["secret"]
        method = vector["method"]
        path = vector["path"]
        assert header and header.startswith("v1,")
        verifier = request_guard.EdgeClientVerifier()
        assert verifier.verify(header, secret, method, path) == (
            vector["ip"], vector["country"]), vector
        assert verifier.verify(header, secret, method, path) is None
        other = request_guard.EdgeClientVerifier()
        assert other.verify(header, secret, "DELETE", path) is None
        assert other.verify(header, secret, method, "/api/other") is None
        assert other.verify(header, secret + "x", method, path) is None

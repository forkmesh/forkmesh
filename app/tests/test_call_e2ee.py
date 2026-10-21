import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app"
SERVER = ROOT / "server"
SUITE = "tests/call_e2ee.test.mjs"


def _env(**extra):
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    env["GOTOOLCHAIN"] = "local"
    env.update(extra)
    return env


def _node(*args, **extra):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    done = subprocess.run(
        [node, "--test", *args, SUITE], cwd=str(APP), env=_env(**extra),
        capture_output=True, text=True, timeout=600)
    assert done.returncode == 0, done.stdout[-6000:] + done.stderr[-2000:]
    assert "# fail 0" in done.stdout
    return done


def _go_test(pattern, **extra):
    if not shutil.which("go"):
        pytest.skip("go is not installed")
    done = subprocess.run(
        ["go", "test", "./rtc/e2ee/", "-count=1", "-run", pattern, "-v"],
        cwd=str(SERVER), env=_env(**extra), capture_output=True, text=True,
        timeout=600)
    assert done.returncode == 0, done.stdout[-6000:] + done.stderr[-2000:]
    return done


def test_call_e2ee_web_suite():
    _node()


def test_go_vectors_open_in_js_live(tmp_path):
    out = tmp_path / "go.json"
    _go_test("^TestGoVectors$", E2EE_WRITE_GO_VECTORS=str(out))
    assert out.stat().st_size > 10000
    _node("--test-name-pattern", "JS opens the vectors Go wrote",
          CALL_E2EE_GO_VECTORS=str(out))


def test_js_vectors_open_in_go_live(tmp_path):
    out = tmp_path / "js.json"
    _node("--test-name-pattern", "JS writes vectors",
          CALL_E2EE_WRITE_JS_VECTORS=str(out))
    assert out.stat().st_size > 10000
    done = _go_test("^TestJSVectors$", E2EE_JS_VECTORS=str(out))
    assert "--- PASS: TestJSVectors" in done.stdout

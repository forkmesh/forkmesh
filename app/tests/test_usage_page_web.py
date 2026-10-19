import shutil
import subprocess
from pathlib import Path

import pytest

APP = Path(__file__).resolve().parents[1]


def test_usage_page_web_suite():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    done = subprocess.run(
        [node, "--test", "tests/usage_page_web.test.mjs"], cwd=str(APP),
        capture_output=True, text=True, timeout=120)
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-2000:]

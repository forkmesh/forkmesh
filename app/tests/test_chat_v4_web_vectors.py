import shutil
import subprocess
from pathlib import Path

import pytest

APP = Path(__file__).resolve().parents[1]


def test_web_chat_v4_suite():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    done = subprocess.run(
        [node, "--test", "tests/chat_v4_web.test.mjs"], cwd=str(APP),
        capture_output=True, text=True, timeout=300)
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-2000:]

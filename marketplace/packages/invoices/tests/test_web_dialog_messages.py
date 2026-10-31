from pathlib import Path
import shutil
import subprocess

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
NODE = shutil.which("node")


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_dialog_errors_show_inside_the_open_dialog():
    result = subprocess.run(
        [NODE, "--no-warnings", str(PACKAGE / "tests" / "dialog_message_checks.js")],
        capture_output=True, text=True, timeout=120,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "invoice dialog message checks passed" in result.stdout

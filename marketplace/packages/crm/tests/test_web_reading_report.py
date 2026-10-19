import json
from pathlib import Path
import shutil
import subprocess

import pytest


WEB_SCRIPT = (Path(__file__).resolve().parents[1] / "content" / "web" / "page.js").read_text(
    encoding="utf-8")
NODE = shutil.which("node")


def _reading(email):
    source = "function crmReadingParts(" + WEB_SCRIPT.split(
        "function crmReadingParts(", 1)[1].split("\n  }\n", 1)[0] + "\n}\n"
    script = source + "console.log(JSON.stringify(crmReadingParts(%s)));" % json.dumps(email)
    result = subprocess.run([NODE, "-e", script], capture_output=True, text=True, timeout=60,
                            env={"PATH": "/usr/local/bin:/usr/bin:/bin"})
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_a_deck_reader_shows_time_reach_and_mail_app():
    assert _reading({"opens": 1, "readSeconds": 240, "scrollDepth": 64, "slideCount": 14,
                     "slideReached": 9, "slideTitle": "Market",
                     "mailClient": "iPhone · Apple Mail", "readingReported": "yes"}) == [
        "read 4 min+", "reached slide 9/14 · Market", "iPhone · Apple Mail"]
    assert _reading({"opens": 1, "readSeconds": 75, "scrollDepth": 100, "slideCount": 14,
                     "slideReached": 14, "slideTitle": "Sources"}) == [
        "read 1 min+", "read all 14 slides"]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_a_mail_app_that_cannot_report_says_so_instead_of_a_blank():
    assert _reading({"opens": 2, "mailClient": "Gmail", "readingReported": "no"}) == [
        "Gmail doesn't report read time"]
    assert _reading({"opens": 0, "mailClient": "Gmail", "readingReported": "no"}) == []
    assert _reading({"opens": 1, "readSeconds": 30, "scrollDepth": 50}) == [
        "read 30s+", "scrolled 50%"]


def test_the_activity_line_uses_the_reading_parts():
    assert "...crmReadingParts(email)].filter(Boolean)" in WEB_SCRIPT

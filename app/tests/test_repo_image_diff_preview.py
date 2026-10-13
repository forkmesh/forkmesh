#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess

FRAGMENT = (
    Path(__file__).resolve().parents[1]
    / "public" / "dashboard" / "js" / "06-repo-content.js"
)


def _render(image):
    text = FRAGMENT.read_text(encoding="utf-8")
    start = text.index("  function renderDiffImagePreview(")
    end = text.index("\n  }\n", start) + 4
    script = (
        "function escapeHtml(value) { return String(value ?? '').replace("
        "/[&<>\"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;',"
        "'\"':'&quot;',\"'\":'&#039;'})[c]); }\n"
        + text[start:end]
        + "\nprocess.stdout.write(JSON.stringify(renderDiffImagePreview("
        + json.dumps(image)
        + ")));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_mirror_supplied_image_bytes_cannot_break_out_of_the_src_attribute():
    html = _render({
        "path": "a.png",
        "mime": "image/png",
        "old": 'x" onerror="alert(document.cookie)',
        "new": "iVBORw0KGgo=",
    })
    assert "onerror" not in html
    assert 'src="data:image/png;base64,iVBORw0KGgo="' in html
    assert html.count("<img") == 1
    assert "Not present" in html


def test_fork_rows_only_link_same_origin_paths():
    text = FRAGMENT.read_text(encoding="utf-8")
    start = text.index("  function repoForkPath(")
    end = text.index("\n  }\n", start) + 4
    cases = [
        "/alice/fork",
        "/\\evil.example",
        "/\t/evil.example",
        "//evil.example",
        "/%0a/x",
        "javascript:alert(1)",
    ]
    script = (
        text[start:end]
        + "\nprocess.stdout.write(JSON.stringify("
        + json.dumps(cases)
        + ".map((url) => repoForkPath(url, 'alice', 'fork'))));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    assert json.loads(result.stdout) == [
        "/alice/fork",
        "/alice/fork",
        "/alice/fork",
        "/alice/fork",
        "/%0a/x",
        "/alice/fork",
    ]

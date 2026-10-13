#!/usr/bin/env python3
"""The web New issue draft survives a background Issues refresh (issue #562)."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


PUBLIC = Path(__file__).resolve().parents[1] / "public"
CONTENT = (PUBLIC / "dashboard" / "js" / "06-repo-content.js").read_text(
    encoding="utf-8"
)


def _region(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


LOAD_ISSUES = _region(
    CONTENT,
    "  async function loadRepoIssues(repo)",
    "  // Reads and parses the issue-N.json blobs",
)


HARNESS = r'''
const form = {
  title: "Keep this title",
  body: "Keep this description",
  attachments: ["screenshot.png"],
};
const container = {
  innerHTML: "<form data-repo-issue-form>draft</form>",
  querySelector: (selector) => selector === "[data-repo-issue-form]" ? form : null,
};
const $ = (selector) => selector === "[data-repo-issues]" ? container : null;
const beforeHtml = container.innerHTML;
const beforeDraft = { ...form, attachments: [...form.attachments] };

__LOAD_ISSUES__

await loadRepoIssues({ owner: "source", name: "forkmesh" });
process.stdout.write(JSON.stringify({
  sameForm: container.querySelector("[data-repo-issue-form]") === form,
  sameHtml: container.innerHTML === beforeHtml,
  draft: form,
  beforeDraft,
}));
'''


pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable"
)


def test_background_issue_refresh_leaves_the_active_web_composer_untouched():
    """Refreshing metadata must not turn a partially written draft into a list."""
    completed = subprocess.run(
        ["node", "--input-type=module", "-"],
        input=HARNESS.replace("__LOAD_ISSUES__", LOAD_ISSUES),
        text=True,
        capture_output=True,
        check=True,
    )
    result = json.loads(completed.stdout)
    assert result["sameForm"] and result["sameHtml"]
    assert result["draft"] == result["beforeDraft"] == {
        "title": "Keep this title",
        "body": "Keep this description",
        "attachments": ["screenshot.png"],
    }

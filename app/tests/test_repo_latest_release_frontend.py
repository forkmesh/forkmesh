#!/usr/bin/env python3
import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
DASHBOARD_JS = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")

HELPERS = (
    "repoReleaseTimestampMs",
    "repoReleaseVersionParts",
    "compareRepoReleases",
    "pickLatestRepoRelease",
)


def _function_source(name: str) -> str:
    start = DASHBOARD_JS.index(f"  function {name}(")
    end = DASHBOARD_JS.index("\n  }\n", start) + len("\n  }\n")
    return DASHBOARD_JS[start:end]


CHANNELS = [
    ("candidate-0.7.95", "v0.7.95", 1788043569000),
    ("candidate-0.8.10", "v0.8.10", 1789399871000),
    ("candidate-0.8.5", "v0.8.5", 1789246850000),
    ("candidate-0.8.9", "v0.8.9", 1789352347000),
    ("candidate-1.2.0", "v1.2.0", 1789604419000),
    ("candidate-1.2.2", "v1.2.2", 1789769869000),
    ("candidate-1.2.3", "v1.2.3", 1790123499000),
    ("latest", "v1.2.3", 1790123499000),
]


def _run(script: str):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    source = "\n".join(_function_source(name) for name in HELPERS)
    out = subprocess.run(
        [node, "-e", source + "\n" + script],
        check=True, capture_output=True, text=True, timeout=30,
    )
    return json.loads(out.stdout)


def _entries(channels):
    return [
        {"channel": c, "manifest": {"tag": t, "created_at": ts}}
        for c, t, ts in channels
    ]


def test_about_widget_uses_picker_not_published_at():
    body = DASHBOARD_JS[DASHBOARD_JS.index("async function loadRepoAboutRelease"):]
    body = body[: body.index("\n  }\n")]
    assert "pickLatestRepoRelease(manifests)" in body
    assert "repoReleaseTimestampMs(latest)" in body
    assert "published_at" not in body
    releases_tab = DASHBOARD_JS[DASHBOARD_JS.index("async function loadRepoReleases"):]
    assert "releases.sort(compareRepoReleases)" in releases_tab[:4000]


def test_latest_channel_wins():
    result = _run(
        "const e = " + json.dumps(_entries(CHANNELS)) + ";"
        "console.log(JSON.stringify(pickLatestRepoRelease(e)));"
    )
    assert result["tag"] == "v1.2.3"


def test_newest_candidate_by_created_at_without_latest_channel():
    candidates = [c for c in CHANNELS if c[0] != "latest"]
    result = _run(
        "const e = " + json.dumps(_entries(candidates)) + ";"
        "console.log(JSON.stringify(pickLatestRepoRelease(e)));"
    )
    assert result["tag"] == "v1.2.3"


def test_semver_breaks_timestamp_ties_and_seconds_normalise():
    entries = [
        {"channel": "a", "manifest": {"tag": "v0.8.9", "published_at": 1789352347}},
        {"channel": "b", "manifest": {"tag": "v0.8.10", "published_at": 1789352347}},
        {"channel": "c", "manifest": {"tag": "v0.8.2"}},
        {"channel": "d", "manifest": {"name": "untagged", "created_at": 9e12}},
    ]
    result = _run(
        "const e = " + json.dumps(entries) + ";"
        "const p = pickLatestRepoRelease(e);"
        "console.log(JSON.stringify({tag: p.tag, ms: repoReleaseTimestampMs(p),"
        " none: pickLatestRepoRelease([])}));"
    )
    assert result == {"tag": "v0.8.10", "ms": 1789352347000, "none": None}

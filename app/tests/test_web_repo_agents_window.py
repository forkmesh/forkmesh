#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()


def _function(source, name):
    start = source.index(f"function {name}(")
    cursor = source.index("{", start)
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _keep(previous, fresh, finished_before):
    constant = REPO_UI[REPO_UI.index("const REPO_AGENT_ACTIVE_STATUSES"):]
    constant = constant[:constant.index(";") + 1]
    script = (
        constant + "\n" + _function(REPO_UI, "keepRepoAgentsBeyondWindow")
        + "\nconsole.log(JSON.stringify(keepRepoAgentsBeyondWindow("
        + json.dumps(previous) + "," + json.dumps(fresh) + ","
        + json.dumps(finished_before) + ").map((s) => s.id)));"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def _session(session_id, status, updated):
    return {"id": session_id, "status": status, "updatedAt": updated}


def test_a_finished_run_past_the_cut_off_stays_listed():
    previous = [_session("new", "completed", 300),
                _session("old", "completed", 100)]
    fresh = [_session("run", "running", 400), _session("new", "completed", 300)]
    assert _keep(previous, fresh, 200) == ["run", "new", "old"]


def test_an_uncut_answer_is_the_whole_list():
    previous = [_session("old", "completed", 100)]
    fresh = [_session("new", "completed", 300)]
    assert _keep(previous, fresh, 0) == ["new"]


def test_rows_the_answer_should_have_listed_are_gone():
    previous = [
        _session("deleted", "completed", 250),
        _session("stale-active", "running", 100),
        _session("old", "failed", 150),
    ]
    fresh = [_session("new", "completed", 300)]
    assert _keep(previous, fresh, 200) == ["new", "old"]


def test_the_live_and_full_reads_both_keep_the_window():
    assert REPO_UI.count("keepRepoAgentsBeyondWindow(") == 3
    assert "payload.finishedBefore" in REPO_UI
    assert "payload?.finishedBefore" in REPO_UI

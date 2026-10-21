#!/usr/bin/env python3

import json
import re
import subprocess
import sys
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import repository_projects  # noqa: E402
from test_event_repo_binding import _ed25519_verify  # noqa: E402

DASHBOARD_JS = assembled_dashboard_js()


def _function(name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), DASHBOARD_JS)
    assert match, name
    return DASHBOARD_JS[match.start():DASHBOARD_JS.index("\n  }\n", match.start()) + 4]


def _const(name):
    return re.search(r"\n  const %s = [^\n]*;" % re.escape(name), DASHBOARD_JS).group(0)


def test_browser_signed_project_events_verify_at_the_relay():
    script = "\n".join([
        "const store = {};",
        "globalThis.localStorage = {getItem: (k) => store[k] ?? null, setItem: (k, v) => { store[k] = String(v); }};",
        "const state = {session: {nodeName: 'alice'}};",
        _const("ISSUE_TEXT_ENCODER"), _const("WEB_ISSUE_KEY_STORAGE"),
    ] + [_function(n) for n in ("bytesToB64url", "sha256HexLower", "getWebIssueKey",
                                "webDiscussionEventId", "webProjectEventContent",
                                "signWebProjectEvents")] + ["""
      (async () => {
        const events = await signWebProjectEvents(4, [
          {type: 'open', title: 'Roadmap', body: 'Q4'},
          {type: 'dates', startDate: 1, endDate: 2},
          {type: 'issues', issues: [9, 3, 9]},
          {type: 'status', status: 'closed'},
          {type: 'delete', target: 'self'}]);
        console.log(JSON.stringify(events));
      })();"""])
    events = json.loads(subprocess.run(["node", "-e", script], check=True,
                                       capture_output=True, text=True).stdout)
    import asyncio
    for raw in events:
        ev = repository_projects.clean_event(raw)
        assert ev is not None, raw
        assert asyncio.run(_ed25519_verify(
            ev["author"], ev["sig"], repository_projects.canonical_string(4, ev)))
    assert events[2]["issues"] == [3, 9]
    assert len({e["id"] for e in events}) == 5


def test_the_form_sends_only_what_changed():
    script = _function("projectFormEvents") + """
      const before = {title: 'A', body: 'x', startDate: 0, endDate: 0, milestone: '', issues: [1]};
      console.log(JSON.stringify({
        fresh: projectFormEvents(null, {title: 'A', body: '', startDate: 0, endDate: 0, milestone: '', issues: []}),
        edit: projectFormEvents(before, {title: 'B', body: 'x', startDate: 5, endDate: 0, milestone: 'v1', issues: [1]}),
        same: projectFormEvents(before, {...before}),
      }));"""
    out = json.loads(subprocess.run(["node", "-e", script], check=True,
                                    capture_output=True, text=True).stdout)
    assert [e["type"] for e in out["fresh"]] == ["open"]
    assert [e["type"] for e in out["edit"]] == ["title", "dates", "milestone"]
    assert out["same"] == []


def test_writers_get_project_controls_and_tombstones_are_hidden():
    assert '<button type="button" data-repo-project-new hidden' in DASHBOARD_JS
    assert "state.projectsView.manageable === true" in _function("renderRepoProjectsList")
    assert "deleted: (Array.isArray(project.events)" in _function("parseProjectJson")
    assert ".filter((project) => project && !project.deleted)" in _function("loadRepoProjects")
    assert "/project-edit" in _function("submitWebProjectEvents")
    assert "error.code === \"number_taken\"" in _function("handleProjectFormSubmit")
    assert "Create them from the desktop client" not in DASHBOARD_JS
    for selector in ('"[data-repo-project-form]"', '"[data-repo-project-new], [data-repo-project-edit]"',
                     '"[data-repo-project-status]"', '"[data-repo-project-delete]"'):
        assert "event.target.closest(%s)" % selector in DASHBOARD_JS
    assert 'active === "projects"' in _function("loadRepoFeaturePanels")

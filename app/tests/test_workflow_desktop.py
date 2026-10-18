#!/usr/bin/env python3

import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_workflows import NOW, FakeDate, Harness, _node  # noqa: E402

import workflow_desktop as desktop  # noqa: E402
import workflows as engine  # noqa: E402

SEEDED = "Free disk space"


@pytest.fixture(autouse=True)
def _clock():
    FakeDate.now_ms = NOW
    yield
    FakeDate.now_ms = NOW


@pytest.fixture
def harness():
    h = Harness()
    h.user("alice")
    h.user("bob")
    return h


def _seeded(harness, user="alice"):
    listed = harness.call("/api/workflows", user=user).data["workflows"]
    return [item for item in listed if item["name"] == SEEDED]


def _report(workflow, freed=3 * 1024 ** 3, free=120 * 1024 ** 3):
    nodes = {}
    for node in workflow["nodes"]:
        if node["type"] == "core.manual":
            nodes[node["id"]] = {"status": "success", "ms": 0,
                                 "outputs": {"main": {"count": 1, "items": [{}]}}}
            continue
        nodes[node["id"]] = {
            "status": "success", "ms": 1500,
            "outputs": {"main": {"count": 1, "items": [{"step": node["id"]}]}},
            "logs": ["del 1.0 GB /tmp/fm123"]}
    return {"startedAt": NOW - 20_000, "finishedAt": NOW - 1_000, "nodes": nodes,
            "output": {"freedBytes": freed, "freeBytes": free,
                       "totalBytes": 400 * 1024 ** 3}}


def test_desktop_steps_are_in_the_catalog_with_icons():
    core = {spec["type"]: spec for spec in engine.catalog()["core"]}
    steps = [core[node_type] for node_type in desktop.NODES]
    assert [spec["desktopStep"] for spec in steps] == [
        "usage", "agents", "tests", "versions", "caches", "report"]
    assert all(spec["group"] == "Desktop" and spec["runsOn"] == "desktop"
               for spec in steps)
    assert len({spec["icon"] for spec in steps}) == len(steps)


def test_desktop_steps_only_run_by_hand_and_alone():
    assert engine.clean_workflow(desktop.seed_workflow())[1] == ""
    scheduled = desktop.seed_workflow()
    scheduled["nodes"][0] = _node("run", "core.schedule", "Every hour",
                                  {"every": "60"})
    assert engine.clean_workflow(scheduled)[1] == "desktop_needs_manual"
    mixed = desktop.seed_workflow()
    mixed["nodes"].append(_node("h", "core.http", "Call",
                                {"method": "GET", "url": "https://example.com"}))
    assert engine.clean_workflow(mixed)[1] == "desktop_steps_only"


def test_the_server_engine_refuses_a_desktop_step():
    workflow, _ = engine.clean_workflow(desktop.seed_workflow())

    async def http(*_args):
        return 200, "{}"

    report = asyncio.run(engine.execute(
        workflow, {}, http, {}, 5, NOW + 10_000, lambda: NOW, NOW))
    assert report["status"] == "error"
    assert desktop.REFUSED in report["error"]


def test_every_account_gets_the_cleanup_once_and_keeps_its_delete(harness):
    seeded = _seeded(harness)
    assert len(seeded) == 1
    assert _seeded(harness) == seeded
    assert seeded[0]["id"] == desktop.seed_id("alice-bi")
    assert seeded[0]["active"] is False and seeded[0]["trigger"] == "core.manual"
    detail = harness.call("/api/workflows/" + seeded[0]["id"],
                          user="alice").data["workflow"]
    assert [node["type"] for node in detail["nodes"]][1:] == list(desktop.NODES)
    assert len(detail["connections"]) == len(desktop.NODES)
    assert len(_seeded(harness, "bob")) == 1
    path = "/api/workflows/" + seeded[0]["id"]
    assert harness.call(path + "/delete", "POST", {}, user="alice").status == 200
    assert _seeded(harness) == []
    assert harness.call(path, user="alice").status == 404


def test_a_server_run_of_a_desktop_workflow_is_refused(harness):
    workflow_id = _seeded(harness)[0]["id"]
    response = harness.call("/api/workflows/" + workflow_id + "/run", "POST",
                            {}, user="alice")
    assert response.status == 409
    assert response.data["error"] == "runs_on_desktop"


def test_a_desktop_run_is_recorded_with_its_output(harness):
    workflow_id = _seeded(harness)[0]["id"]
    path = "/api/workflows/" + workflow_id
    workflow = harness.call(path, user="alice").data["workflow"]
    response = harness.call(path + "/record", "POST", _report(workflow),
                            user="alice")
    assert response.status == 200
    run = response.data["run"]
    assert run["status"] == "success" and run["trigger"] == "desktop"
    assert run["output"]["text"] == "Freed 3.0 GB · 120.0 GB free"
    assert [m["value"] for m in run["output"]["metrics"]] == ["3.0 GB", "120.0 GB"]
    assert run["finishedAt"] - run["startedAt"] == 19_000
    runs = harness.call(path + "/runs", user="alice").data["runs"]
    assert runs[0]["id"] == run["id"]
    assert runs[0]["output"]["freedBytes"] == 3 * 1024 ** 3
    detail = harness.call(path + "/runs/" + run["id"], user="alice").data["run"]
    assert detail["nodes"]["agents"]["logs"] == ["del 1.0 GB /tmp/fm123"]
    assert harness.call(path + "/record", "POST", _report(workflow),
                        user="bob").status == 404


def test_a_failed_desktop_step_fails_the_run(harness):
    workflow_id = _seeded(harness)[0]["id"]
    path = "/api/workflows/" + workflow_id
    workflow = harness.call(path, user="alice").data["workflow"]
    body = _report(workflow)
    body["nodes"]["tests"] = {"status": "error", "ms": 10, "error": "busy"}
    body["nodes"]["versions"] = {"status": "skipped"}
    run = harness.call(path + "/record", "POST", body, user="alice").data["run"]
    assert run["status"] == "error"
    assert run["error"] == "Remove test leftovers: busy"
    assert run["output"]["text"] == "Remove test leftovers: busy"


def test_a_desktop_record_is_checked(harness):
    workflow_id = _seeded(harness)[0]["id"]
    path = "/api/workflows/" + workflow_id
    workflow = harness.call(path, user="alice").data["workflow"]
    stranger = _report(workflow)
    stranger["nodes"]["nobody"] = {"status": "success", "ms": 1}
    assert harness.call(path + "/record", "POST", stranger,
                        user="alice").data["error"] == "invalid_run"
    backwards = dict(_report(workflow), finishedAt=NOW - 30_000)
    assert harness.call(path + "/record", "POST", backwards,
                        user="alice").data["error"] == "invalid_run"
    created = harness.call("/api/workflows", "POST", {
        "name": "Plain", "nodes": [_node("t", "core.manual", "Go")],
        "connections": []}, user="alice").data["workflow"]
    assert harness.call("/api/workflows/" + created["id"] + "/record", "POST",
                        _report(workflow), user="alice").data["error"] == \
        "not_a_desktop_workflow"


def test_server_runs_carry_an_output_summary(harness):
    created = harness.call("/api/workflows", "POST", {
        "name": "Echo", "nodes": [
            _node("t", "core.manual", "Go", {"payload": '{"status": "ready"}'}),
            _node("s", "core.set", "Mark", {"values": '{"message": "done"}'})],
        "connections": [{"from": "t", "to": "s"}]}, user="alice").data["workflow"]
    run = harness.call("/api/workflows/" + created["id"] + "/run", "POST", {},
                       user="alice").data["run"]
    assert run["output"] == {"text": "done",
                             "metrics": [{"label": "Output", "value": "1 item"}]}
    runs = harness.call("/api/workflows/" + created["id"] + "/runs",
                        user="alice").data["runs"]
    assert runs[0]["output"]["text"] == "done"


def _page(tmp_path, mode="ok"):
    import json
    import shutil
    import subprocess
    import dashboard_bundle
    import workflow_files
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    workflow, _ = engine.clean_workflow(desktop.seed_workflow())
    workflow = dict(workflow, id="a" * 32, active=False, hookPath="")
    summary = {"id": workflow["id"], "name": SEEDED, "active": False,
               "trigger": "core.manual", "nodes": len(workflow["nodes"])}
    runs = [{"id": "b" * 32, "trigger": "desktop", "status": "success",
             "startedAt": NOW - 7_200_000, "finishedAt": NOW - 7_080_000, "output": None},
            {"id": "c" * 32, "trigger": "desktop", "status": "success",
             "startedAt": NOW - 3_600_000, "finishedAt": NOW - 3_540_000, "output": None}]
    fixture = tmp_path / "fixture.json"
    fixture.write_text(json.dumps({
        "catalog": dict(engine.catalog(), files=workflow_files.catalog()),
        "workflow": workflow, "summary": summary, "runs": runs}))
    root = Path(__file__).resolve().parents[1]
    result = subprocess.run(
        ["node", str(Path(__file__).with_name("run_workflow_desktop_page_js.js")),
         str(root / "public" / "dashboard-page-workflows.js"), str(fixture),
         dashboard_bundle.CHUNK_GLOBAL, mode],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr[-3000:]
    return json.loads(result.stdout)


def test_the_page_runs_each_step_on_the_desktop_and_records_the_run(tmp_path):
    out = _page(tmp_path)
    steps = [step for _type, step, *_rest in desktop.STEPS]
    assert [call[2] for call in out["calls"] if call[0] == "diag.run"] == steps
    assert all(call[1] == "freeDiskSpace" for call in out["calls"])
    assert [call[3] for call in out["calls"] if call[0] == "diag.run"] == [
        "tok-" + step for step in steps]
    record = out["recorded"]
    assert set(record["nodes"]) == {"run", *steps}
    assert all(entry["status"] == "success" for entry in record["nodes"].values())
    assert record["nodes"]["agents"]["logs"] == ["ran agents"]
    assert record["nodes"]["agents"]["outputs"]["main"]["items"][0]["freed"] == "2.0 GB"
    assert record["output"] == {"freedBytes": 2 * 1024 ** 3, "freeBytes": 50 * 1024 ** 3,
                                "totalBytes": 400 * 1024 ** 3}
    assert record["finishedAt"] >= record["startedAt"]


def test_the_rail_moves_the_running_step_against_the_longest_run(tmp_path):
    out = _page(tmp_path)
    rail = out["railWhileRunning"]
    assert 'class="wf-rail-step" data-status="running"' in rail
    assert 'class="wf-rail-step" data-status="success"' in rail
    assert rail.count('class="wf-rail-tick"') == 1
    assert rail.count('class="wf-rail-tick is-longest"') == 1
    assert "2 m 00 s" in rail and "1 m 00 s" in rail
    assert "Full bar: longest run, 2 m 00 s" in rail
    assert "Remove agent leftovers" in rail
    assert 'data-lucide="bot"' in rail
    assert 'data-status="running"' in out["canvasWhileRunning"]
    assert out["railHidden"] is False
    assert "Finished in" in out["railAfter"]
    assert not out["timer"]


def test_the_recent_runs_list_shows_each_runs_output_window(tmp_path):
    out = _page(tmp_path)
    assert out["runsHidden"] is False
    assert out["runsHtml"].count('class="wf-run"') == 3
    assert '<span class="wf-run-output" title="Freed 2.0 GB · 50.0 GB free">' in out["runsHtml"]
    assert "<strong>2.0 GB</strong> cleaned" in out["runsHtml"]
    assert "<strong>50.0 GB</strong> free" in out["runsHtml"]
    assert "on your desktop" in out["runsHtml"]
    assert out["notice"].startswith("Done in")


def test_a_failed_desktop_step_stops_the_rest(tmp_path):
    out = _page(tmp_path, "fail")
    nodes = out["recorded"]["nodes"]
    assert nodes["tests"] == {"status": "error", "ms": nodes["tests"]["ms"],
                              "error": "Another disk cleanup step is still running on the desktop."}
    assert [nodes[step]["status"] for step in ("versions", "caches", "report")] == ["skipped"] * 3
    assert nodes["agents"]["status"] == "success"

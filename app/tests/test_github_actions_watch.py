import asyncio
import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MODULE = SRC / "github_actions_watch.py"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
FOOTPRINT = (ROOT.parent / "world" / "tools" / "build_worker_footprint.py").read_text(
    encoding="utf-8")
REPO = "forkmesh/forkmesh"
QUALITY = ".github/workflows/quality-gates.yml"
WORLD = ".github/workflows/world-browser-e2e.yml"


def _run(number, path, conclusion, event="push", branch="main",
         status="completed", name=None):
    return {
        "id": 1000 + number, "run_number": number, "path": path,
        "name": name or path.rsplit("/", 1)[-1], "event": event,
        "head_branch": branch, "status": status, "conclusion": conclusion,
        "head_sha": "%040x" % number,
        "head_commit": {"message": "Commit %d\n\nbody" % number},
        "html_url": "https://github.com/%s/actions/runs/%d" % (
            REPO, 1000 + number),
        "created_at": "2026-10-22T00:%02d:00Z" % number,
    }


class _Env:
    def __init__(self, **values):
        for name, value in values.items():
            setattr(self, name, value)


class _Response:
    def __init__(self, status, payload):
        self.status = status
        self._text = json.dumps(payload) if payload is not None else ""

    async def text(self):
        return self._text


class _Runtime:
    def __init__(self, admins=("alice", "bob")):
        self.now = 1_790_000_000_000
        self.state = {}
        self.pings = []
        self.fetches = []
        self.responses = {}
        self.admins = list(admins)

    def respond(self, fragment, status, payload):
        self.responses[fragment] = (status, payload)

    def namespace(self):
        runtime = self

        class _Date:
            @staticmethod
            def now():
                return runtime.now

        async def ensure_schema(env):
            return None

        async def js_fetch_with_timeout(url, init, timeout):
            runtime.fetches.append((url, dict(init["headers"])))
            for fragment, (status, payload) in runtime.responses.items():
                if fragment in url:
                    return _Response(status, payload)
            return _Response(404, {"message": "Not Found"})

        async def d1_all(env, sql, *params):
            if sql.startswith("SELECT data FROM users"):
                return [{"data": name} for name in runtime.admins]
            if "FROM repository_monitor_state" in sql:
                return [dict(runtime.state[key], monitor_id=key)
                        for key in params if key in runtime.state]
            raise AssertionError(sql)

        async def d1_run(env, sql, *params):
            if sql.lstrip().startswith("INSERT INTO repository_monitor_state"):
                (key, is_up, changed_at, outage, checked, reason,
                 _notified, pinged) = params
                row = runtime.state.get(key)
                if row is None:
                    runtime.state[key] = {
                        "is_up": is_up, "changed_at": changed_at,
                        "outage_started_at": outage, "checked_at": checked,
                        "reason": reason, "pinged_state": pinged}
                    return
                flipped = row["is_up"] != is_up
                row["changed_at"] = changed_at if flipped else row["changed_at"]
                row["outage_started_at"] = (
                    0 if is_up else outage if flipped
                    else row["outage_started_at"])
                row.update(is_up=is_up, checked_at=checked, reason=reason)
                return
            if sql.startswith("UPDATE repository_monitor_state SET pinged_state"):
                pinged, key, is_up = params
                row = runtime.state.get(key)
                if row and row["is_up"] == is_up:
                    row["pinged_state"] = pinged
                return
            raise AssertionError(sql)

        async def decrypt_row(env, data):
            return {"name": data}

        async def enqueue_notification(env, recipient, kind, title, **kwargs):
            runtime.pings.append(dict(kwargs, recipient=recipient, kind=kind,
                                      title=title))
            return True

        return {
            "Date": _Date, "ensure_schema": ensure_schema,
            "js_fetch_with_timeout": js_fetch_with_timeout,
            "d1_all": d1_all, "d1_run": d1_run, "decrypt_row": decrypt_row,
            "enqueue_notification": enqueue_notification,
            "clean_string": lambda value, maximum=0: str(value)[:maximum],
            "MAX_NODE_NAME": 39,
            "valid_node_name": lambda name: bool(name) and name.isalnum(),
        }


def _load(runtime=None):
    namespace = {"__name__": "github_actions_watch"}
    exec(compile(MODULE.read_text(encoding="utf-8"), str(MODULE), "exec"),
         namespace)
    if runtime is not None:
        namespace["_bind_runtime"](runtime.namespace())
    return namespace


def _tick(module, env=None):
    return asyncio.run(module["watch_github_actions"](env or _Env()))


def test_verdict_skips_cancelled_runs_and_counts_the_failing_streak():
    module = _load()
    runs = [
        _run(9, QUALITY, "cancelled"),
        _run(8, QUALITY, "failure"),
        _run(7, QUALITY, None, status="in_progress"),
        _run(6, QUALITY, "failure"),
        _run(5, QUALITY, "success"),
        _run(4, QUALITY, "failure"),
        _run(3, WORLD, "success"),
        _run(2, WORLD, "failure", event="pull_request"),
        _run(1, WORLD, "failure", branch="feature"),
    ]
    verdicts = module["workflow_verdicts"](runs, "main")
    quality = verdicts[QUALITY]
    assert quality["failing"] is True
    assert quality["runNumber"] == 8
    assert quality["streak"] == 2
    assert quality["failingSince"]["runId"] == 1006
    assert quality["streakComplete"] is True
    world = verdicts[WORLD]
    assert world["failing"] is False
    assert world["runNumber"] == 3
    assert world["failingSince"] is None


def test_skipped_only_workflows_are_not_watched():
    module = _load()
    runs = [_run(3, ".github/workflows/sync.yml", "skipped"),
            _run(2, ".github/workflows/sync.yml", "neutral")]
    assert module["workflow_verdicts"](runs, "main") == {}


def test_failed_steps_name_the_job_and_step():
    module = _load()
    jobs = {"jobs": [
        {"name": "core", "conclusion": "failure", "steps": [
            {"name": "Check out source", "conclusion": "success"},
            {"name": "Run gates", "conclusion": "failure"}]},
        {"name": "lint", "conclusion": "success", "steps": []},
        {"name": "world-browser", "conclusion": "timed_out", "steps": []},
    ]}
    assert module["failed_steps"](jobs) == [
        "core › Run gates", "world-browser"]
    assert module["failed_steps"]({"message": "rate limited"}) == []


def test_annotation_detail_skips_the_runner_exit_line():
    module = _load()
    annotations = [
        {"annotation_level": "notice", "message": "ubuntu-latest moves"},
        {"annotation_level": "failure",
         "message": "Process completed with exit code 2."},
        {"annotation_level": "failure",
         "title": "Quality gate mirror-server-go-tests failed",
         "message": "[critical-tests] go: 900s budget\n"
                    "--- FAIL: TestThreePeople (4.26s)\n"
                    "    call_test.go:63: p2 received 0 video frames\n"
                    "FAIL\tforkmesh.com/mirror-node/rtc/fmmserver"},
    ]
    assert module["annotation_detail"](annotations) == (
        "Quality gate mirror-server-go-tests failed: "
        "--- FAIL: TestThreePeople (4.26s)")
    assert module["annotation_detail"]([
        {"annotation_level": "failure",
         "title": "[chromium] › daily-interface.spec.js:12:3 › daily interface",
         "message": "Error: expect(received).toBeLessThan(expected)\n\n"
                    "Expected: < 4\nReceived: 6"}]) == (
        "[chromium] › daily-interface.spec.js:12:3 › daily interface: "
        "Error: expect(received).toBeLessThan(expected)")
    assert module["annotation_detail"]([
        {"annotation_level": "failure",
         "message": "Process completed with exit code 1."}]) == ""
    assert module["annotation_detail"]({"message": "Forbidden"}) == ""


def test_ping_text_names_the_failure_and_links_the_run():
    module = _load()
    verdict = module["workflow_verdicts"](
        [_run(8, QUALITY, "failure", name="ForkMesh quality gates"),
         _run(7, QUALITY, "failure")], "main")[QUALITY]
    title, body = module["ping_text"](
        REPO, "main", verdict, ["core › Run gates"])
    assert title == "GitHub Actions: ForkMesh quality gates is failing"
    assert body.startswith("core › Run gates failed on main at 0000000 (Commit 8).")
    assert "It has failed at least 2 runs in a row." in body
    assert body.endswith("https://github.com/forkmesh/forkmesh/actions/runs/1008")
    passing = module["workflow_verdicts"](
        [_run(9, QUALITY, "success", name="ForkMesh quality gates")],
        "main")[QUALITY]
    title, body = module["ping_text"](REPO, "main", passing)
    assert title == "GitHub Actions: ForkMesh quality gates passes again"
    assert body.startswith("Run #9 passed on main")


def test_failing_workflow_pings_every_admin_once_then_recovery_once():
    runtime = _Runtime()
    module = _load(runtime)
    runtime.respond("/actions/runs?", 200, {"workflow_runs": [
        _run(8, QUALITY, "failure"), _run(5, WORLD, "success")]})
    runtime.respond("/actions/runs/1008/jobs", 200, {"jobs": [
        {"id": 77, "name": "core", "conclusion": "failure",
         "steps": [{"name": "Run gates", "conclusion": "failure"}]}]})
    runtime.respond("/check-runs/77/annotations", 200, [
        {"annotation_level": "failure", "title": "Quality gate go failed",
         "message": "--- FAIL: TestCall (1s)\nFAIL\tpkg"}])
    result = _tick(module)
    assert result == {"ok": True, "checked": 2, "failing": 1, "alerted": 1}
    assert [ping["recipient"] for ping in runtime.pings] == ["alice", "bob"]
    ping = runtime.pings[0]
    assert ping["kind"] == "operational_alert"
    assert ping["source"] == "github-actions"
    assert ping["href"] == "/status"
    assert ping["body"].startswith(
        "core › Run gates failed on main at 0000000 (Commit 8). "
        "Quality gate go failed: --- FAIL: TestCall (1s). https://")
    assert ping["meta"]["runId"] == 1008
    key = "github-actions:forkmesh/forkmesh:" + QUALITY
    assert runtime.state[key]["pinged_state"] == "down"
    assert runtime.state[key]["is_up"] == 0
    world_key = "github-actions:forkmesh/forkmesh:" + WORLD
    assert runtime.state[world_key]["pinged_state"] == "up"

    runtime.pings.clear()
    runtime.now += 300_000
    runtime.respond("/actions/runs?", 200, {"workflow_runs": [
        _run(9, QUALITY, "failure"), _run(8, QUALITY, "failure"),
        _run(5, WORLD, "success")]})
    assert _tick(module)["alerted"] == 0
    assert runtime.pings == []

    runtime.now += 300_000
    runtime.respond("/actions/runs?", 200, {"workflow_runs": [
        _run(10, QUALITY, "success"), _run(9, QUALITY, "failure"),
        _run(5, WORLD, "success")]})
    assert _tick(module)["alerted"] == 1
    assert {ping["title"] for ping in runtime.pings} == {
        "GitHub Actions: quality-gates.yml passes again"}
    assert runtime.state[key]["pinged_state"] == "up"
    assert runtime.state[key]["changed_at"] == runtime.now
    assert runtime.pings[0]["dedupe"] == "github-actions:forkmesh/forkmesh:%s:up:%d" % (
        QUALITY, runtime.now)


def test_undelivered_alert_is_retried_with_the_same_dedupe_key():
    runtime = _Runtime(admins=())
    module = _load(runtime)
    runtime.respond("/actions/runs?", 200, {"workflow_runs": [
        _run(8, QUALITY, "failure")]})
    assert _tick(module)["alerted"] == 0
    first_changed = runtime.state[
        "github-actions:forkmesh/forkmesh:" + QUALITY]["changed_at"]
    runtime.admins = ["alice"]
    runtime.now += 300_000
    assert _tick(module)["alerted"] == 1
    assert runtime.pings[0]["dedupe"].endswith(":down:%d" % first_changed)


def test_hourly_sweep_pings_workflows_missing_from_the_run_page():
    runtime = _Runtime()
    module = _load(runtime)
    sync = ".github/workflows/github-issue-sync.yml"
    runtime.respond("/actions/runs?", 200, {"workflow_runs": [
        _run(8, QUALITY, "success"),
        _run(7, WORLD, "cancelled"),
        _run(6, ".github/workflows/bridge.yml", "skipped")]})
    runtime.respond("/actions/workflows?", 200, {"workflows": [
        {"id": 11, "path": QUALITY, "state": "active"},
        {"id": 12, "path": WORLD, "state": "active"},
        {"id": 13, "path": sync, "state": "active"},
        {"id": 14, "path": ".github/workflows/old.yml",
         "state": "disabled_manually"}]})
    runtime.respond("/actions/workflows/12/runs?", 200, {"workflow_runs": [
        _run(7, WORLD, "cancelled"), _run(5, WORLD, "failure")]})
    runtime.respond("/actions/workflows/13/runs?", 200, {"workflow_runs": [
        _run(3, sync, "failure", event="schedule"),
        _run(2, sync, "failure", event="schedule")]})

    runtime.now = 1_790_002_800_000 + 60_000
    assert not module["sweep_due"](runtime.now - 360_000)
    assert module["sweep_due"](runtime.now)
    result = _tick(module)
    assert result == {"ok": True, "checked": 3, "failing": 2, "alerted": 2}
    assert {ping["title"] for ping in runtime.pings} == {
        "GitHub Actions: world-browser-e2e.yml is failing",
        "GitHub Actions: github-issue-sync.yml is failing"}
    swept = [url for url, _ in runtime.fetches if "/actions/workflows/" in url]
    assert len(swept) == 2
    assert all("branch=main" in url for url in swept)
    assert not any("/workflows/14/" in url for url, _ in runtime.fetches)
    assert not any("/workflows/11/" in url for url, _ in runtime.fetches)

    runtime.pings.clear()
    runtime.fetches.clear()
    runtime.now += 300_000
    assert _tick(module) == {
        "ok": True, "checked": 1, "failing": 0, "alerted": 0}
    assert len(runtime.fetches) == 1
    assert runtime.state["github-actions:forkmesh/forkmesh:" + sync][
        "pinged_state"] == "down"


def test_github_errors_write_nothing():
    runtime = _Runtime()
    module = _load(runtime)
    runtime.respond("/actions/runs?", 403, {"message": "rate limit"})
    assert _tick(module) == {"ok": False, "checked": 0, "status": 403}
    assert runtime.state == {} and runtime.pings == []


def test_config_defaults_overrides_token_and_off_switch():
    runtime = _Runtime()
    module = _load(runtime)
    assert module["watch_config"](_Env()) == {
        "repo": REPO, "branch": "main"}
    assert module["watch_config"](_Env(GITHUB_ACTIONS_WATCH_REPO="off")) is None
    assert module["watch_config"](
        _Env(GITHUB_ACTIONS_WATCH_REPO="bad repo")) is None
    runtime.respond("/actions/runs?", 200, {"workflow_runs": []})
    _tick(module, _Env(GITHUB_ACTIONS_WATCH_REPO="acme/widgets",
                       GITHUB_ACTIONS_WATCH_BRANCH="release/1",
                       GITHUB_ACTIONS_WATCH_TOKEN="secret"))
    url, headers = runtime.fetches[0]
    assert url.startswith(
        "https://api.github.com/repos/acme/widgets/actions/runs?branch=release%2F1")
    assert headers["authorization"] == "Bearer secret"
    assert headers["user-agent"]
    assert _tick(module, _Env(GITHUB_ACTIONS_WATCH_REPO="off"))[
        "reason"] == "watch_not_configured"


def test_cron_phase_is_wired_every_five_minutes():
    assert 'github_actions_watch = _LazyModule("github_actions_watch")' in ENTRY_TEXT
    assert ('    if minute % 5 == 1:\n        phases.append("github-actions")\n'
            in ENTRY_TEXT)
    assert ('    if phase == "github-actions":\n'
            '        return await watch_github_actions(env)\n') in ENTRY_TEXT
    assert '"github_actions_watch.py",' in FOOTPRINT


def test_status_cli_reports_failing_workflows_with_steps_and_detail():
    spec = importlib.util.spec_from_file_location(
        "github_actions_status", ROOT.parent / "tools" / "github_actions_status.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    answers = {
        "/actions/runs?": {"workflow_runs": [
            _run(8, QUALITY, "failure", name="ForkMesh quality gates"),
            _run(7, QUALITY, "failure"), _run(6, QUALITY, "success"),
            _run(5, WORLD, "success", name="World browser E2E")]},
        "/actions/runs/1008/jobs": {"jobs": [
            {"id": 5, "name": "core", "conclusion": "failure",
             "steps": [{"name": "Run gates", "conclusion": "failure"}]}]},
        "/check-runs/5/annotations": [
            {"annotation_level": "failure", "title": "Quality gate go failed",
             "message": "--- FAIL: TestCall (1s)"}],
    }
    asked = []

    def fetch(path):
        asked.append(path)
        for fragment, payload in answers.items():
            if fragment in path:
                return 200, payload
        return 404, None

    report = tool.collect(REPO, "main", fetch=fetch)
    assert report["failing"] == 1
    assert tool.render(report).splitlines() == [
        "GitHub Actions on forkmesh/forkmesh main: 1 of 2 workflows failing",
        "FAIL  ForkMesh quality gates  #8 0000000 failure, 2 runs in a row",
        "      core › Run gates",
        "      Quality gate go failed: --- FAIL: TestCall (1s)",
        "      https://github.com/forkmesh/forkmesh/actions/runs/1008",
        "ok    World browser E2E  #5 0000000 success",
    ]
    assert len(asked) == 4
    with pytest.raises(RuntimeError):
        tool.collect(REPO, "main", fetch=lambda path: (403, None))

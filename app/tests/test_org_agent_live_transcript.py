import ast
import asyncio
import copy
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import agent_live_relay  # noqa: E402

ROUTES = ROOT / "src" / "route_handlers.py"
DASHBOARD = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
HELPERS = (
    ROOT / "public" / "dashboard" / "js" / "02-helpers.js"
).read_text(encoding="utf-8")
GO_AGENTS = (ROOT.parent / "server" / "agents.go").read_text(encoding="utf-8")

NAMES = [
    "_apply_org_agent_job_result",
    "_apply_org_agent_live_transcript",
    "_org_agent_live_projection",
]


class _Store:
    def __init__(self, session_status="running", job_status="running"):
        self.job = {
            "id": 7, "session_id": "sess1", "org_bi": "org-bi",
            "provider": "claude-code", "status": job_status,
            "data": {"kind": "start"},
        }
        self.session = {
            "session_id": "sess1", "status": session_status,
            "created_by_bi": "bi", "updated_at": 1_000,
            "data": {
                "createdBy": "alice", "scope": "owner", "org": "alice",
                "history": [{"role": "user", "text": "Fix it", "at": 900}],
            },
        }
        self.runs = []

    async def first(self, _env, sql, *args):
        if "FROM org_agent_jobs" in sql:
            return dict(self.job)
        if "FROM org_agent_sessions" in sql:
            row = dict(self.session)
            row["data"] = copy.deepcopy(row["data"])
            return row
        return None

    async def run(self, _env, sql, *args):
        self.runs.append((sql, args))
        if sql.startswith("UPDATE org_agent_sessions") and "AND updated_at=?" in sql:
            data, updated_at, _sid, _org, expected = args
            if (self.session["status"] == "running"
                    and self.session["updated_at"] == expected):
                self.session["data"] = data
                self.session["updated_at"] = updated_at
        elif sql.startswith("UPDATE org_agent_sessions"):
            status, data, updated_at = args[0], args[1], args[2]
            self.session.update(
                status=status, data=data, updated_at=updated_at)


def _load(store, now=5_000, watchers=0):
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"), filename=str(ROUTES))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in NAMES
    ]
    assert len(selected) == len(NAMES)
    rung = []
    published = []

    class _Date:
        @staticmethod
        def now():
            return now

    async def passthrough(_env, value):
        return copy.deepcopy(value)

    async def ring(_env, record, topic="cloud-agents"):
        rung.append(topic)

    async def publish(_env, record, session_id, payload):
        published.append((record.get("createdBy"), session_id, dict(payload)))
        return watchers

    async def noop(*_args, **_kwargs):
        return None

    def response(payload, status=200, **_kwargs):
        return {"payload": payload, "status": status}

    def error(code, message, status, **_kwargs):
        return {"payload": {"error": code, "message": message},
                "status": status}

    namespace = {
        "re": re,
        "Date": _Date,
        "d1_first": store.first,
        "d1_run": store.run,
        "decrypt_row": passthrough,
        "encrypt_row": passthrough,
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
        "json_response": response,
        "_org_agent_error": error,
        "_ring_cloud_agent_readers": ring,
        "_publish_org_agent_live": publish,
        "_agent_live_relay": agent_live_relay,
        "_org_agent_delete_attachments": noop,
        "_org_agent_attachment_ids": lambda _job: [],
        "_audit_sensitive_action": noop,
        "_org_agent_info_projection": lambda value: dict(value),
        "_org_agent_availability_projection": lambda value: dict(value),
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
        "MAX_AGENT_TRANSCRIPT": 16000,
        "ORG_AGENT_MAX_HISTORY": 40,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ROUTES), "exec"),
         namespace)
    namespace["_published"] = published
    return namespace, rung


def _report(ns, **data):
    body = {"leaseId": "lease-1", "securityVerdict": "approved",
            "status": "running"}
    body.update(data)
    return asyncio.run(ns["_apply_org_agent_job_result"](
        None, "mirror35", "widgets", 7, body))


def test_a_live_report_stores_the_transcript_without_touching_history():
    store = _Store()
    ns, rung = _load(store)
    reply = _report(ns, live=True, transcript="Reading the README.")
    assert reply["status"] == 200 and reply["payload"]["live"] is True
    assert reply["payload"]["watched"] is False
    assert ns["_published"] == [
        ("alice", "sess1", {"at": 5_000, "text": "Reading the README."})]
    data = store.session["data"]
    assert data["live"] == {"text": "Reading the README.", "at": 5_000}
    assert data["history"] == [{"role": "user", "text": "Fix it", "at": 900}]
    assert store.session["status"] == "running"
    assert rung == ["cloud-agents-live"]
    assert not any("org_agent_jobs" in sql for sql, _ in store.runs)


def test_a_live_report_never_overwrites_a_concurrent_write():
    store = _Store()
    ns, _rung = _load(store)
    original = store.first

    async def racing_first(env, sql, *args):
        row = await original(env, sql, *args)
        if "FROM org_agent_sessions" in sql:
            store.session["updated_at"] = 1_500
            store.session["data"]["history"].append(
                {"role": "user", "text": "Also add tests", "at": 1_500})
        return row

    store.first = racing_first
    ns["d1_first"] = racing_first
    _report(ns, live=True, transcript="Reading the README.")
    history = store.session["data"]["history"]
    assert history[-1]["text"] == "Also add tests"
    assert "live" not in store.session["data"]


def test_a_live_report_for_a_run_that_is_not_running_is_ignored():
    for session_status, job_status in (("completed", "running"),
                                       ("running", "leased")):
        store = _Store(session_status=session_status, job_status=job_status)
        ns, rung = _load(store)
        reply = _report(ns, live=True, transcript="late words")
        assert reply["payload"]["live"] is False
        assert store.runs == [] and rung == []


def test_the_closing_report_replaces_the_live_transcript_with_history():
    store = _Store()
    ns, _rung = _load(store)
    _report(ns, live=True, transcript="Working on it")
    reply = _report(ns, status="completed", transcript="Working on it\n\nDone.")
    assert reply["payload"] == {"ok": True, "status": "completed"}
    data = store.session["data"]
    assert "live" not in data
    assert data["history"][-1]["text"] == "Working on it\n\nDone."
    assert store.session["status"] == "completed"


def test_the_live_transcript_is_only_projected_while_running():
    ns, _rung = _load(_Store())
    project = ns["_org_agent_live_projection"]
    record = {"live": {"text": "Step one", "at": 12}}
    assert project(record, "running") == {"text": "Step one", "at": 12}
    assert project(record, "completed") is None
    assert project({"live": {"text": "  "}}, "running") is None
    assert project({}, "running") is None
    source = ROUTES.read_text(encoding="utf-8")
    assert '"live": _org_agent_live_projection(record, status),' in source


def test_the_node_and_the_web_are_wired_for_live_transcripts():
    assert "newAgentLiveReporter(" in GO_AGENTS
    assert "onChange: live" in GO_AGENTS
    assert 'topic === "cloud-agents-live"' in HELPERS
    assert '"forkmesh:cloud-agents"' in HELPERS
    assert 'addEventListener("forkmesh:cloud-agents"' in DASHBOARD
    assert "data-org-agent-live-entry" in DASHBOARD


def test_a_watched_run_streams_every_report_but_persists_at_the_old_pace():
    store = _Store()
    store.session["data"]["live"] = {"text": "Reading", "at": 4_000}
    ns, rung = _load(store, now=5_000, watchers=1)
    reply = _report(ns, live=True, transcript="Reading the README.")
    assert reply["payload"] == {"ok": True, "status": "running",
                                "live": True, "watched": True}
    assert ns["_published"][-1][2] == {"at": 5_000,
                                       "text": "Reading the README."}
    assert store.runs == [] and rung == []
    assert store.session["data"]["live"] == {"text": "Reading", "at": 4_000}
    ns, rung = _load(store, now=4_000 + agent_live_relay.PERSIST_MS,
                     watchers=1)
    reply = _report(ns, live=True, transcript="Reading the README. Done.")
    assert reply["payload"]["watched"] is True
    assert store.session["data"]["live"]["text"] == "Reading the README. Done."
    assert rung == ["cloud-agents-live"]


def test_the_closing_report_ends_the_live_stream():
    store = _Store()
    ns, _rung = _load(store)
    _report(ns, status="completed", transcript="Done.")
    assert ns["_published"][-1] == ("alice", "sess1", {"status": "completed"})
    store = _Store()
    ns, _rung = _load(store)
    _report(ns, status="running")
    assert ns["_published"] == []

"""Stopping a cloud agent run from the account's own devices.

POST .../agent-sessions/<sid> with {"action": "stop"} withdraws a run no node
has started, and queues a ``kind:"stop"`` job for a run that is going, ahead
of any follow-up still waiting. The node (desktop or mirror) that runs the
session kills the provider and reports how it ended over the signed result
route; the relay never reaches into a node.
"""

import ast
import asyncio
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
ROUTES = ROOT / "src" / "route_handlers.py"
GO_AGENTS = (REPOSITORY / "server" / "agents.go").read_text(encoding="utf-8")
DESKTOP_SYNC = (
    REPOSITORY / "desktop" / "src" / "MainWindowAgentSync.cpp"
).read_text(encoding="utf-8")


def _load(names, extra):
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"), filename=str(ROUTES))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert len(selected) == len(names)
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ROUTES), "exec"),
         namespace)
    return namespace


def _harness(status, local_agent_id, signed=False):
    calls = {"batches": [], "audits": [], "notified": [], "rung": [],
             "contexts": []}

    async def member_context(*_args, **kwargs):
        calls["contexts"].append(kwargs)
        return ({
            "org": "alice", "orgBi": "org-bi", "repo": "widgets",
            "actor": "alice",
        }, None)

    async def session_row(*_args):
        return ({
            "session_id": "sess1", "provider": "claude-code",
            "status": status, "target_node": "mirror35",
        }, {"localAgentId": local_agent_id, "history": []})

    async def request_json(_request):
        return {"action": "stop"}, None

    async def batch(_env, statements):
        calls["batches"].append(statements)

    async def audit(*args):
        calls["audits"].append(args)

    async def notify(_env, node, repo, topic):
        calls["notified"].append((node, repo, topic))

    async def ring(_env, record):
        calls["rung"].append(record)

    async def encrypt_row(_env, record):
        return dict(record)

    async def signed_session(_env, _request, data=None):
        calls["signedBodies"] = [data]
        return ("bi:alice", {"name": "alice"})

    class _Date:
        @staticmethod
        def now():
            return 5_000

    ns = _load(["org_agent_bot_handler", "_org_agent_stop_session"], {
        "ensure_schema": lambda _env: asyncio.sleep(0),
        "method_name": lambda _request: "POST",
        "_owner_agent_signed_triple": lambda _request: signed,
        "_owner_agent_signed_session": signed_session,
        "_request_origin_allowed": lambda _env, _request: True,
        "_org_agent_request_json": request_json,
        "_org_agent_member_context": member_context,
        "_org_agent_session_row": session_row,
        "_contribution_run_batch": batch,
        "_audit_sensitive_action": audit,
        "notify_repo_host": notify,
        "_ring_cloud_agent_readers": ring,
        "encrypt_row": encrypt_row,
        "Date": _Date,
        "ORG_AGENT_MAX_HISTORY": 50,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "json_response": lambda payload, **kwargs: {
            "payload": payload, "status": kwargs.get("status", 200)},
        "_org_agent_error": lambda code, *_args, **_kwargs: {"error": code},
    })
    return ns["org_agent_bot_handler"], calls


def test_a_running_session_gets_a_stop_job_for_its_node():
    handler, calls = _harness("running", 42)
    response = asyncio.run(handler(
        object(), object(), "alice", "widgets", "sess1", owner_scope=True))
    assert response["status"] == 202
    assert response["payload"]["outcome"] == "stop_requested"
    (statements,) = calls["batches"]
    withdraw, session_update, insert = statements
    assert "status='queued'" in withdraw[0] and "SET status='failed'" in \
        withdraw[0]
    assert "INSERT INTO org_agent_jobs" in insert[0]
    job = insert[1][5]
    assert job["kind"] == "stop" and job["localAgentId"] == 42
    assert job["repositoryOwner"] == "mirror35"
    assert session_update[1][0]["stopRequestedAt"] == 5_000
    assert calls["notified"] == [("mirror35", "widgets", "org-agents")]
    assert calls["audits"][0][2] == "owner.agent_stop"
    assert calls["rung"], "the account's devices hear the change"


def test_a_session_no_node_started_is_withdrawn_here():
    handler, calls = _harness("queued", 0)
    response = asyncio.run(handler(
        object(), object(), "alice", "widgets", "sess1", owner_scope=True))
    assert response["payload"]["outcome"] == "withdrawn"
    assert response["payload"]["stopped"] is True
    (statements,) = calls["batches"]
    assert not any("INSERT" in sql for sql, _ in statements)
    assert any("UPDATE org_agent_sessions SET status='failed'" in sql
               for sql, _ in statements)
    assert calls["notified"] == []


def test_a_finished_session_has_nothing_to_stop():
    handler, calls = _harness("completed", 42)
    response = asyncio.run(handler(
        object(), object(), "alice", "widgets", "sess1", owner_scope=True))
    assert response == {"error": "session_not_running"}
    assert calls["batches"] == []


def test_a_key_signed_desktop_stop_is_the_web_stop():
    handler, calls = _harness("queued", 42, signed=True)
    response = asyncio.run(handler(
        object(), object(), "alice", "widgets", "sess1", owner_scope=True))
    assert response["payload"]["outcome"] == "stop_requested"
    assert calls["signedBodies"] == [{"action": "stop"}]
    (context,) = calls["contexts"]
    assert context["signed_account"] == ("bi:alice", {"name": "alice"})
    assert context["own_session"] is True
    assert context["owner_scope"] is True
    (statements,) = calls["batches"]
    assert "INSERT INTO org_agent_jobs" in statements[-1][0]
    assert statements[-1][1][5]["kind"] == "stop"
    assert [audit[2] for audit in calls["audits"]] == ["owner.agent_stop"]
    assert calls["notified"] == [("mirror35", "widgets", "org-agents")]


def test_both_node_implementations_honour_the_stop_job():
    # Go mirror node: cancels the run's own context, never starts anything.
    assert 'job.Kind == "stop"' in GO_AGENTS
    assert "r.stops[job.SessionID]" in GO_AGENTS
    # Qt desktop: stops the bound local session through the normal stop path.
    assert 'kind == QLatin1String("stop")' in DESKTOP_SYNC
    assert "stopAgentSessionById(" in DESKTOP_SYNC.split(
        'kind == QLatin1String("stop")', 1)[1][:1500]

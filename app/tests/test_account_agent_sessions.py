#!/usr/bin/env python3
"""The account-wide cloud agent list, GET /api/account/agent-sessions.

A mirror runs the account's agent work for whichever repository the job named,
which is rarely the repository the reader happens to have open. The desktop and
the phone must not poll (www/docs/operations/polling-elimination.md), so asking
repository by repository is not an option either: this route answers for the
whole account in one indexed read, and the account event doorbell tells the
clients when to ask again.

These checks pin:

  * GET only, and the account's own blind index is the only scope read;
  * an organization's sessions join only for a member the org route would let
    read them;
  * a URL signature is the only credential once it is on the URL;
  * no session, no list;
  * the read is bounded.

AST-extraction harness in the style of test_agent_cloud_handoff.py.

Run: python3 -m pytest app/tests/test_account_agent_sessions.py
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
HANDLERS = ROOT / "src" / "route_handlers.py"


class Recorder:
    def __init__(self, rows=(), signed=None, token=None, orgs=(),
                 org_rows=None, engineering=()):
        self.rows = list(rows)
        self.signed = signed
        self.token = token
        self.orgs = list(orgs)
        self.org_rows = dict(org_rows or {})
        self.engineering = set(engineering)
        self.queries = []
        self.account_events = []

    async def d1_all(self, _env, sql, *args):
        self.queries.append((sql, args))
        if "FROM org_members" in sql:
            return self.orgs
        rows = self.rows
        if args and args[0] in self.org_rows:
            rows = self.org_rows[args[0]]
        if "status NOT IN" in sql:
            rows = [row for row in rows if row["status"] not in args[1:-1]]
        elif "status IN" in sql:
            rows = [row for row in rows if row["status"] in args[1:-1]]
        if "ORDER BY updated_at DESC LIMIT ?" in sql:
            rows = sorted(rows, key=lambda row: -row["updated_at"])[:args[-1]]
        return rows

    async def floor_rows(self, _env, org_bi, floor, member_bi=""):
        assert floor == "engineering" and member_bi == "bi:jett"
        return [{"team": "eng"}] if org_bi in self.engineering else []

    async def signed_session(self, _env, _request, _data=None):
        return self.signed or ("", None)

    async def account_session(self, _env, _request, _data=None):
        return self.token or ("", None)

    async def notify_account(self, _env, owner, topic):
        self.account_events.append((owner, topic))


def _load(recorder, triple=False):
    tree = ast.parse(HANDLERS.read_text(encoding="utf-8"),
                     filename=str(HANDLERS))
    wanted = {
        "account_agent_sessions_handler",
        "_account_org_agent_rows",
        "_account_agent_scope_rows",
        "_ring_cloud_agent_readers",
    }
    constants = {"ACCOUNT_AGENT_MAX_SESSIONS", "ACCOUNT_AGENT_HISTORY_TURNS",
                 "ACCOUNT_AGENT_MAX_ORGS", "ACCOUNT_AGENT_MAX_ACTIVE",
                 "ACCOUNT_AGENT_ACTIVE_STATUSES"}
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id in constants
                    for target in node.targets))
    ]
    found = {
        node.name if hasattr(node, "name") else node.targets[0].id
        for node in selected
    }
    assert found == wanted | constants, "missing: %s" % sorted(
        (wanted | constants) - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))

    async def noop(*_args, **_kwargs):
        return None

    def response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    namespace = {
        "MAX_NODE_NAME": 64,
        "ensure_schema": noop,
        "method_name": lambda request: request["method"],
        "clean_string": lambda value, maximum: str(
            "" if value is None else value)[:maximum],
        "d1_all": recorder.d1_all,
        "blind_index": lambda _env, value: asyncio.sleep(
            0, result="bi:" + value),
        "_org_floor_team_rows": recorder.floor_rows,
        "_org_rbac": SimpleNamespace(
            org_role_allows=lambda role, capability: (
                capability == "organization.agents.read"
                and role in ("owner", "admin", "member"))),
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "notify_account_event": recorder.notify_account,
        "_owner_agent_signed_triple": lambda _request: triple,
        "_owner_agent_signed_session": recorder.signed_session,
        "_account_session_record": recorder.account_session,
        # The projection has its own coverage; here it only has to be the
        # thing the route hands back, unchanged.
        "_org_agent_session_projection": lambda row, record: {
            "id": row["session_id"],
            "org": record.get("org"),
            "repo": row["repo"],
            "status": row["status"],
            "targetNode": row["target_node"],
            "history": list(record.get("history") or []),
        },
        "_org_agent_hide_private_runner": (
            lambda projection, row, record, viewer: projection),
        "_org_agent_error": lambda code, message, status, **kwargs: {
            "status": status,
            "data": {"ok": False, "error": code, "message": message, **kwargs},
        },
        "json_response": response,
    }
    exec(compile(module, str(HANDLERS), "exec"), namespace)
    return namespace


def _row(session_id, repo, status="running", node="mirror9", scope="owner",
         org="jett", turns=0, updated=2):
    return {
        "session_id": session_id,
        "org_bi": "bi:jett",
        "repo": repo,
        "target_node": node,
        "provider": "claude-code",
        "status": status,
        "data": {
            "org": org,
            "repo": repo,
            "scope": scope,
            "history": [{"text": "turn %d" % n} for n in range(turns)],
        },
        "created_at": 1,
        "updated_at": updated,
        "completed_at": 0,
    }


ACCOUNT = ("bi:jett", {"name": "Jett"})


def _get(recorder, triple=False, method="GET"):
    namespace = _load(recorder, triple=triple)
    return asyncio.run(namespace["account_agent_sessions_handler"](
        object(), {"method": method}))


def test_every_repository_answers_in_one_read_scoped_to_the_account():
    recorder = Recorder(
        rows=[_row("s1", "forkmesh"), _row("s2", "notes", status="queued")],
        token=ACCOUNT,
    )
    response = _get(recorder)
    assert response["status"] == 200
    assert response["data"]["account"] == "jett"
    assert response["data"]["activeCount"] == 2
    assert [item["repo"] for item in response["data"]["sessions"]] == [
        "forkmesh", "notes"]
    assert len(recorder.queries) == 3
    active_sql, active_args = recorder.queries[0]
    assert "WHERE org_bi=? AND status IN (?,?,?)" in active_sql
    assert active_args[0] == "bi:jett"
    loaded = _load(Recorder())
    assert active_args[-1] == loaded["ACCOUNT_AGENT_MAX_ACTIVE"] <= 1000
    sql, args = recorder.queries[1]
    # Scoped by the account's own blind index, finished runs only, newest
    # first, and bounded one past the list so the route knows it cut some.
    assert ("WHERE org_bi=? AND status NOT IN (?,?,?) "
            "ORDER BY updated_at DESC LIMIT ?") in sql
    assert args[0] == "bi:jett"
    limit = loaded["ACCOUNT_AGENT_MAX_SESSIONS"]
    assert args[-1] == limit + 1 and 0 < limit <= 100
    assert response["data"]["finishedBefore"] == 0


def test_an_organization_row_under_the_accounts_own_index_is_ignored():
    recorder = Recorder(
        rows=[
            _row("s1", "forkmesh"),
            _row("s2", "forkmesh", scope="organization", org="acme"),
            _row("s3", "forkmesh", scope=None),
        ],
        token=ACCOUNT,
    )
    response = _get(recorder)
    assert [item["id"] for item in response["data"]["sessions"]] == ["s1"]
    assert response["data"]["scope"] == "owner"
    assert response["data"]["sessions"][0]["scope"] == "owner"


def test_an_organizations_web_runs_join_for_an_engineering_member():
    recorder = Recorder(
        rows=[_row("own", "notes", updated=5)],
        token=ACCOUNT,
        orgs=[
            {"org_bi": "org:forkmesh", "name": "ForkMesh", "role": "member"},
            {"org_bi": "org:acme", "name": "acme", "role": "member"},
            {"org_bi": "org:guests", "name": "guests", "role": "guest"},
        ],
        org_rows={
            "org:forkmesh": [
                _row("web", "forkmesh", scope="organization",
                     org="forkmesh", updated=9),
                _row("stray", "forkmesh", scope="organization",
                     org="acme", updated=8),
                _row("owned", "forkmesh", scope="owner",
                     org="forkmesh", updated=7),
            ],
            "org:acme": [
                _row("acme", "site", scope="organization", org="acme"),
            ],
            "org:guests": [
                _row("guest", "site", scope="organization", org="guests"),
            ],
        },
        engineering={"org:forkmesh", "org:guests"},
    )
    response = _get(recorder)
    sessions = response["data"]["sessions"]
    assert [item["id"] for item in sessions] == ["web", "own"]
    assert [item["scope"] for item in sessions] == ["organization", "owner"]
    assert response["data"]["activeCount"] == 2
    member_sql, member_args = recorder.queries[2]
    assert "WHERE m.member_bi=?" in member_sql
    assert member_args[0] == "bi:jett"
    read = [args[0] for sql, args in recorder.queries
            if "FROM org_agent_sessions" in sql]
    assert read == ["bi:jett", "bi:jett", "org:forkmesh", "org:forkmesh"]


def test_the_merged_list_is_newest_first_and_bounded():
    limit = _load(Recorder())["ACCOUNT_AGENT_MAX_SESSIONS"]
    recorder = Recorder(
        rows=[_row("own%d" % n, "notes", status="completed", updated=2 * n)
              for n in range(limit)],
        token=ACCOUNT,
        orgs=[{"org_bi": "org:forkmesh", "name": "forkmesh",
               "role": "member"}],
        org_rows={"org:forkmesh": [
            _row("web%d" % n, "forkmesh", scope="organization",
                 org="forkmesh", status="completed", updated=2 * n + 1)
            for n in range(limit)]},
        engineering={"org:forkmesh"},
    )
    sessions = _get(recorder)["data"]["sessions"]
    assert len(sessions) == limit
    assert sessions[0]["id"] == "web%d" % (limit - 1)
    assert sessions[1]["id"] == "own%d" % (limit - 1)


def test_every_active_run_is_listed_beyond_the_finished_bound():
    loaded = _load(Recorder())
    limit = loaded["ACCOUNT_AGENT_MAX_SESSIONS"]
    active = [_row("q%d" % n, "forkmesh", status="queued", updated=1000 + n)
              for n in range(limit + 43)]
    done = [_row("d%d" % n, "forkmesh", status="completed", updated=n)
            for n in range(limit + 10)]
    recorder = Recorder(rows=active + done, token=ACCOUNT)
    sessions = _get(recorder)["data"]["sessions"]
    ids = {s["id"] for s in sessions}
    assert all("q%d" % n in ids for n in range(limit + 43))
    assert sum(1 for s in sessions if s["id"].startswith("d")) == limit
    assert len(ids) == len(sessions)
    active_sql = [sql for sql, _ in recorder.queries if "status IN" in sql]
    assert active_sql and "LIMIT ?" in active_sql[0]


def test_finished_runs_are_listed_while_more_runs_are_active_than_the_bound():
    limit = _load(Recorder())["ACCOUNT_AGENT_MAX_SESSIONS"]
    active = [_row("q%d" % n, "forkmesh", status="queued", updated=1000 + n)
              for n in range(limit + 5)]
    done = [_row("d%d" % n, "forkmesh", status="failed", updated=10 + n)
            for n in range(3)]
    sessions = _get(Recorder(rows=active + done, token=ACCOUNT))["data"][
        "sessions"]
    assert {"d0", "d1", "d2"} <= {s["id"] for s in sessions}


def test_a_cut_finished_list_says_where_it_stopped():
    limit = _load(Recorder())["ACCOUNT_AGENT_MAX_SESSIONS"]
    done = [_row("d%d" % n, "forkmesh", status="completed", updated=100 + n)
            for n in range(limit + 7)]
    data = _get(Recorder(rows=done, token=ACCOUNT))["data"]
    assert len(data["sessions"]) == limit
    assert data["finishedBefore"] == 100 + 7
    whole = _get(Recorder(rows=done[:limit], token=ACCOUNT))["data"]
    assert whole["finishedBefore"] == 0


def test_a_signature_on_the_url_is_the_only_credential_considered():
    # A desktop that launched silently has no session token, and a request
    # that carries a triple must never fall back to somebody else's cookie.
    recorder = Recorder(rows=[_row("s1", "forkmesh")], signed=ACCOUNT,
                        token=("bi:mallory", {"name": "mallory"}))
    assert _get(recorder, triple=True)["data"]["account"] == "jett"
    junk = Recorder(rows=[_row("s1", "forkmesh")],
                    token=("bi:mallory", {"name": "mallory"}))
    refused = _get(junk, triple=True)
    assert refused["status"] == 401
    assert refused["data"]["error"] == "invalid_session"
    assert junk.queries == []


def test_no_session_reads_nothing_and_only_get_is_answered():
    recorder = Recorder(rows=[_row("s1", "forkmesh")])
    assert _get(recorder)["status"] == 401
    assert recorder.queries == []
    for method in ("POST", "DELETE", "PUT"):
        refused = _get(Recorder(token=ACCOUNT), method=method)
        assert refused["status"] == 405
        assert refused["data"]["error"] == "method_not_allowed"


def test_a_long_conversation_travels_as_its_tail_and_says_so():
    # Every repository at once means every history at once, and one session
    # may hold 80 turns of 16 KB. The list carries the end of it; the session's
    # own route still has the whole thing.
    limit = _load(Recorder())["ACCOUNT_AGENT_HISTORY_TURNS"]
    recorder = Recorder(
        rows=[
            _row("long", "forkmesh", turns=limit + 4),
            _row("short", "notes", turns=2),
        ],
        token=ACCOUNT,
    )
    sessions = _get(recorder)["data"]["sessions"]
    assert len(sessions[0]["history"]) == limit
    assert sessions[0]["history"][0]["text"] == "turn 4"
    assert sessions[0]["historyTruncated"] is True
    assert len(sessions[1]["history"]) == 2
    assert "historyTruncated" not in sessions[1]


def test_the_doorbell_names_the_account_that_owns_the_run():
    recorder = Recorder()
    namespace = _load(recorder)
    ring = namespace["_ring_cloud_agent_readers"]
    asyncio.run(ring(object(), {
        "scope": "owner", "org": "jett", "createdBy": "jett"}))
    assert recorder.account_events == [("jett", "cloud-agents")]
    # A node namespace the account owns is worth its own frame; an
    # organization's is not an account and only the member is told.
    recorder = Recorder()
    ring = _load(recorder)["_ring_cloud_agent_readers"]
    asyncio.run(ring(object(), {
        "scope": "owner", "org": "workshop", "createdBy": "jett"}))
    assert recorder.account_events == [("jett", "cloud-agents"),
                                       ("workshop", "cloud-agents")]
    recorder = Recorder()
    ring = _load(recorder)["_ring_cloud_agent_readers"]
    asyncio.run(ring(object(), {
        "scope": "organization", "org": "acme", "createdBy": "jett"}))
    assert recorder.account_events == [("jett", "cloud-agents")]
    recorder = Recorder()
    ring = _load(recorder)["_ring_cloud_agent_readers"]
    asyncio.run(ring(object(), None))
    assert recorder.account_events == []

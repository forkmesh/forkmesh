#!/usr/bin/env python3
"""Authorization contracts for the login-gated spatial Office."""

import ast
import asyncio
import base64
import hmac
from pathlib import Path
import re
import sqlite3
import tomllib
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
WORLD_DURABLE_OBJECTS = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
)
WRANGLER = ROOT / "wrangler.toml"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
PARSED_ENTRY = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
PARSED_WORLD_DURABLE_OBJECTS = ast.parse(
    WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8"),
    filename=str(WORLD_DURABLE_OBJECTS),
)
SESSION_ID = "session_binding_0123456789abcdef"


def _route_table():
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    for node in ast.parse(runtime).body:
        if isinstance(node, ast.ClassDef) and node.name == "Default":
            return ast.unparse(node)
    raise AssertionError("Default not found in app_runtime.py")


def _top_level_node(name):
    for tree in (PARSED_ENTRY, PARSED_WORLD_DURABLE_OBJECTS):
        for node in tree.body:
            if isinstance(
                node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
            ) and node.name == name:
                return node
    raise AssertionError(f"{name} not found in Worker sources")


def _top_level_assignment(name):
    for node in PARSED_ENTRY.body:
        if not isinstance(node, (ast.Assign, ast.AnnAssign)):
            continue
        targets = node.targets if isinstance(node, ast.Assign) else [node.target]
        if any(isinstance(target, ast.Name) and target.id == name for target in targets):
            return node
    raise AssertionError(f"{name} assignment not found in entry.py")


def _compile(nodes, namespace):
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _ticket_helpers():
    names = {
        "_require_data_secret",
        "_office_entry_ticket",
        "_office_entry_ticket_claims",
        "_office_meeting_ticket",
        "_office_meeting_ticket_claims",
    }
    nodes = [
        node for node in PARSED_ENTRY.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == names
    clock = [1_800_000_000_000]

    class _Date:
        @staticmethod
        def now():
            return clock[0]

    namespace = {
        "OFFICE_ENTRY_TICKET_TTL_MS": 5 * 60 * 1000,
        "OFFICE_MEETING_TICKET_TTL_MS": 60 * 1000,
        "Date": _Date,
        "_INSECURE_DATA_KEYS": frozenset({"", "forkmesh-dev-data-key"}),
        "base64": base64,
        "hmac": hmac,
        "new_world_peer_id": lambda: "0123456789abcdef",
        "re": re,
    }
    return _compile(nodes, namespace), clock


class _Env:
    DATA_KEY = "private-production-data-key"


def test_entry_ticket_is_account_bound_short_lived_and_tamper_evident():
    namespace, clock = _ticket_helpers()
    account_bi = "a" * 64
    token = namespace["_office_entry_ticket"](_Env(), account_bi)
    claims = namespace["_office_entry_ticket_claims"](_Env(), token)

    assert claims == {
        "scope": "world-general",
        "accountBi": account_bi,
        "expires": clock[0] + 5 * 60 * 1000,
        "nonce": "0123456789abcdef",
    }
    assert token.startswith(f"v2.world-general.{account_bi}.")
    replacement = "0" if token[-1] != "0" else "1"
    assert namespace["_office_entry_ticket_claims"](
        _Env(), token[:-1] + replacement) is None
    clock[0] += 5 * 60 * 1000
    assert namespace["_office_entry_ticket_claims"](_Env(), token) is None


def test_entry_ticket_rejects_guests_and_malformed_account_bindings():
    namespace, _clock = _ticket_helpers()
    for account_bi in ("", "guest", "a" * 63, "A" * 64):
        try:
            namespace["_office_entry_ticket"](_Env(), account_bi)
        except ValueError:
            continue
        raise AssertionError("Office entry tickets must require a blind-index account")


def _entry_handler():
    state = {
        "account_bi": "",
        "account": None,
    }

    class _Response(dict):
        def __init__(self, data, status=200):
            super().__init__(status=status, data=data)
            self.status = status

    async def account_session(_env, _request):
        return state["account_bi"], state["account"]

    def json_response(data, status=200, **_kwargs):
        return _Response(data, status)

    namespace = {
        "_account_kind": lambda record: record.get("kind", ""),
        "_account_session_record": account_session,
        "_office_entry_ticket": lambda _env, _account_bi: "entry-proof",
        "_office_entry_ticket_claims": lambda _env, _token: {
            "expires": 1_800_000_060_000,
        },
        "_request_same_origin": lambda request: request.same_origin,
        "_request_origin_allowed": (
            lambda _env, request: request.same_origin
        ),
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "re": re,
    }
    _compile([_top_level_node("office_general_entry_handler")], namespace)
    return namespace["office_general_entry_handler"], state


class _EntryRequest:
    method = "POST"
    same_origin = True

    def __init__(self, data=None):
        self.data = data or {}


def test_entry_requires_same_origin_active_registered_user_and_has_no_code_body():
    handler, state = _entry_handler()

    cross_origin = _EntryRequest()
    cross_origin.same_origin = False
    assert asyncio.run(handler(_Env(), cross_origin)) == {
        "status": 403,
        "data": {"error": "origin_not_allowed"},
    }

    assert asyncio.run(handler(_Env(), _EntryRequest())) == {
        "status": 401,
        "data": {"error": "login_required"},
    }

    state["account_bi"] = "a" * 64
    state["account"] = {"name": "node", "status": "active", "kind": "node"}
    assert asyncio.run(handler(_Env(), _EntryRequest()))["status"] == 401

    state["account"] = {"name": "alice", "status": "active", "kind": "user"}
    admitted = asyncio.run(handler(_Env(), _EntryRequest({"code": "2468"})))
    assert admitted == {
        "status": 200,
        "data": {
            "ok": True,
            "room": "general",
            "entryTicket": "entry-proof",
            "expiresAt": 1_800_000_060_000,
            "requiresLogin": True,
        },
    }
    assert "2468" not in repr(admitted)

    entry_source = ast.unparse(_top_level_node("office_general_entry_handler"))
    assert "bounded_json_request" not in entry_source
    assert "OFFICE_ENTRY_CODE" not in entry_source
    assert "code" not in entry_source.lower()


def test_status_reports_only_the_login_gate_not_room_or_secret_state():
    node = _top_level_node("office_general_status_handler")
    state = {"account": None}

    async def account_session(_env, _request):
        return "a" * 64, state["account"]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    namespace = {
        "_account_kind": lambda record: record.get("kind", ""),
        "_account_session_record": account_session,
        "json_response": json_response,
        "method_name": lambda request: request.method,
    }
    _compile([node], namespace)
    request = SimpleNamespace(method="GET")

    guest = asyncio.run(namespace["office_general_status_handler"](
        _Env(), request))
    assert guest == {
        "status": 200,
        "data": {
            "ok": True,
            "authenticated": False,
            "requiresLogin": True,
        },
    }

    state["account"] = {
        "name": "alice",
        "status": "active",
        "kind": "user",
    }
    member = asyncio.run(namespace["office_general_status_handler"](
        _Env(), request))
    assert member["data"]["authenticated"] is True
    assert "occupied" not in repr(member).lower()
    assert "participant" not in repr(member).lower()
    assert "code" not in repr(member).lower()


def _floor_access_handler():
    state = {
        "account_bi": "",
        "account": None,
        "office_org_bi": "forkmesh-org-bi",
        "office_org_row": {"name": "forkmesh"},
        "organization_reads": [],
        "rows": [],
        "membership": None,
        "queries": [],
    }

    async def account_session(_env, _request):
        return state["account_bi"], state["account"]

    async def ensure_schema(_env):
        return None

    async def org_row(_env, org):
        state["organization_reads"].append(org)
        return state["office_org_bi"], state["office_org_row"]

    async def d1_all(_env, query, *params):
        state["queries"].append((query, params))
        return state["rows"]

    async def d1_first(_env, query, *params):
        state["queries"].append((query, params))
        return state["membership"]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    nodes = [
        _top_level_assignment("OFFICE_FLOOR_TEAM_ALIASES"),
        _top_level_node("_office_team_slug"),
        _top_level_node("office_floor_access_handler"),
    ]
    namespace = {
        "MAX_NODE_NAME": 64,
        "_account_kind": lambda record: record.get("kind", ""),
        "_account_session_record": account_session,
        "_org_row": org_row,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "d1_all": d1_all,
        "d1_first": d1_first,
        "ensure_schema": ensure_schema,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "re": re,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", str(value or ""))
        ),
    }
    _compile(nodes, namespace)
    return namespace["office_floor_access_handler"], state


def test_floor_projection_grants_defaults_plus_server_derived_team_floors():
    handler, state = _floor_access_handler()
    request = SimpleNamespace(method="GET")

    denied = asyncio.run(handler(_Env(), request))
    assert denied == {
        "status": 401,
        "data": {
            "authenticated": False,
            "allowedFloorIds": [],
            "teams": [],
        },
    }

    state["account_bi"] = "b" * 64
    state["account"] = {
        "name": "Alice",
        "status": "active",
        "kind": "user",
    }
    state["rows"] = [
        {"team": "Frontend"},
        {"team": "Trust & Safety"},
        {"team": "Unmapped Team"},
    ]
    admitted = asyncio.run(handler(_Env(), request))
    assert admitted["status"] == 200
    assert admitted["data"]["authenticated"] is True
    assert admitted["data"]["account"] == "alice"
    assert admitted["data"]["teams"] == [
        "frontend",
        "trust-safety",
        "unmapped-team",
    ]
    # An account on no marketing team is not admitted to the Marketing floor,
    # and Trust & Safety no longer maps anywhere: Security is not a floor.
    assert admitted["data"]["allowedFloorIds"] == [
        "lobby",
        "rooftop",
        "engineering",
    ]
    query, params = state["queries"][-1]
    assert "FROM org_team_members tm" in query
    assert "INNER JOIN orgs o" in query
    assert "INNER JOIN org_members om" in query
    assert "INNER JOIN org_teams ot" in query
    assert "tm.org_bi=?" in query
    assert state["organization_reads"] == ["forkmesh"]
    assert params == ("forkmesh-org-bi", "forkmesh", "b" * 64)


def _database_floor_access_handler(database, state):
    async def account_session(_env, _request):
        return state["account_bi"], state["account"]

    async def ensure_schema(_env):
        return None

    async def org_row(_env, org):
        row = database.execute(
            "SELECT org_bi,name FROM orgs WHERE name=?",
            (org,),
        ).fetchone()
        return (
            (str(row["org_bi"]), dict(row))
            if row
            else ("missing-org-bi", None)
        )

    async def d1_all(_env, query, *params):
        return [
            dict(row)
            for row in database.execute(query, params).fetchall()
        ]

    async def d1_first(_env, query, *params):
        row = database.execute(query, params).fetchone()
        return dict(row) if row else None

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    namespace = {
        "MAX_NODE_NAME": 64,
        "_account_kind": lambda record: record.get("kind", ""),
        "_account_session_record": account_session,
        "_org_row": org_row,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "d1_all": d1_all,
        "d1_first": d1_first,
        "ensure_schema": ensure_schema,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "re": re,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", str(value or ""))
        ),
    }
    _compile([
        _top_level_assignment("OFFICE_FLOOR_TEAM_ALIASES"),
        _top_level_node("_office_team_slug"),
        _top_level_node("office_floor_access_handler"),
    ], namespace)
    return namespace["office_floor_access_handler"]


def test_floor_projection_rejects_other_org_aliases_and_orphan_grants():
    database = sqlite3.connect(":memory:")
    database.row_factory = sqlite3.Row
    database.executescript(
        """
        CREATE TABLE orgs (
            org_bi TEXT PRIMARY KEY, name TEXT NOT NULL
        );
        CREATE TABLE org_members (
            org_bi TEXT NOT NULL, member_bi TEXT NOT NULL,
            role TEXT NOT NULL
        );
        CREATE TABLE org_teams (
            org_bi TEXT NOT NULL, team TEXT NOT NULL,
            permission TEXT NOT NULL
        );
        CREATE TABLE org_team_members (
            org_bi TEXT NOT NULL, team TEXT NOT NULL,
            member_bi TEXT NOT NULL
        );
        """
    )
    alice_bi = "a" * 64
    bob_bi = "b" * 64
    database.executemany(
        "INSERT INTO orgs (org_bi,name) VALUES (?,?)",
        (("official", "forkmesh"), ("self-created", "alice-labs")),
    )
    database.executemany(
        "INSERT INTO org_members (org_bi,member_bi,role) VALUES (?,?,?)",
        (
            ("official", alice_bi, "member"),
            ("self-created", alice_bi, "owner"),
        ),
    )
    database.executemany(
        "INSERT INTO org_teams (org_bi,team,permission) VALUES (?,?,?)",
        (
            ("official", "frontend", "read"),
            ("official", "security", "read"),
            ("self-created", "operations", "admin"),
        ),
    )
    database.executemany(
        "INSERT INTO org_team_members (org_bi,team,member_bi) VALUES (?,?,?)",
        (
            # The only valid authoritative grant.
            ("official", "frontend", alice_bi),
            # A fully live lookalike team in Alice's own organization.
            ("self-created", "operations", alice_bi),
            # Missing org_teams parent in the authoritative organization.
            ("official", "community", alice_bi),
            # Live team but missing org_members parent for Bob.
            ("official", "security", bob_bi),
        ),
    )
    state = {
        "account_bi": alice_bi,
        "account": {"name": "Alice", "status": "active", "kind": "user"},
    }
    handler = _database_floor_access_handler(database, state)
    request = SimpleNamespace(method="GET")

    alice = asyncio.run(handler(_Env(), request))
    assert alice["data"]["teams"] == ["frontend"]
    assert alice["data"]["allowedFloorIds"] == [
        "lobby",
        "rooftop",
        "engineering",
    ]
    # Membership of the authoritative org, not of Alice's own 'alice-labs'.
    assert alice["data"]["organizationRole"] == "member"

    state["account_bi"] = bob_bi
    state["account"] = {"name": "Bob", "status": "active", "kind": "user"}
    bob = asyncio.run(handler(_Env(), request))
    assert bob["data"]["teams"] == []
    assert bob["data"]["allowedFloorIds"] == [
        "lobby",
        "rooftop",
    ]
    # A team row without the org_members parent is not membership.
    assert bob["data"]["organizationRole"] == ""
    database.close()


def test_floor_projection_reports_office_organization_membership():
    # The Office chat embed opens channel creation on this role, so it must
    # come from org_members in the authoritative org — not from holding a
    # team floor, and not from any org the viewer created themselves.
    handler, state = _floor_access_handler()
    request = SimpleNamespace(method="GET")
    state["account_bi"] = "d" * 64
    state["account"] = {"name": "Dana", "status": "active", "kind": "user"}

    state["rows"] = [{"team": "Frontend"}]
    outsider = asyncio.run(handler(_Env(), request))
    assert outsider["data"]["organizationRole"] == ""

    for role in ("member", "admin", "owner"):
        state["membership"] = {"role": role}
        state["rows"] = []
        insider = asyncio.run(handler(_Env(), request))
        assert insider["data"]["organizationRole"] == role
        # Membership alone grants no team floor.
        assert insider["data"]["allowedFloorIds"] == ["lobby", "rooftop"]

    membership_query, membership_params = next(
        entry for entry in state["queries"] if "FROM org_members om" in entry[0]
    )
    assert "INNER JOIN orgs o" in membership_query
    assert "om.org_bi=?" in membership_query
    assert "o.name=?" in membership_query
    assert "om.member_bi=?" in membership_query
    # Insider roles from the organization authorization matrix (issue #487):
    # guests are outside collaborators and never hold a floor.
    assert "om.role IN ('owner','admin','maintainer','member','auditor')" in membership_query
    assert membership_params == ("forkmesh-org-bi", "forkmesh", "d" * 64)


def test_marketing_floor_is_granted_only_to_the_marketing_team():
    handler, state = _floor_access_handler()
    request = SimpleNamespace(method="GET")
    state["account_bi"] = "c" * 64
    state["account"] = {"name": "Mallory", "status": "active", "kind": "user"}

    state["rows"] = [{"team": "Community"}]
    outsider = asyncio.run(handler(_Env(), request))
    assert "marketing" not in outsider["data"]["allowedFloorIds"]

    for team in ("Marketing", "growth", "Brand"):
        state["rows"] = [{"team": team}]
        member = asyncio.run(handler(_Env(), request))
        assert "marketing" in member["data"]["allowedFloorIds"]


def _general_access_handler():
    state = {
        "account_bi": "a" * 64,
        "record": {
            "name": "Alice",
            "status": "active",
            "kind": "user",
        },
        "claim_account_bi": "a" * 64,
    }

    class _Headers(dict):
        def get(self, name, default=None):
            return super().get(str(name).lower(), default)

    class _Request:
        method = "GET"

        def __init__(self, entry=""):
            self.headers = _Headers({"x-forkmesh-office-entry": entry})

    async def account_session(_env, _request):
        return state["account_bi"], state["record"]

    def entry_claims(_env, token):
        if token != "valid-entry":
            return None
        return {
            "scope": "world-general",
            "accountBi": state["claim_account_bi"],
        }

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    namespace = {
        "_account_kind": lambda record: record.get("kind", ""),
        "_account_session_record": account_session,
        "_request_account_session_id": lambda _request: SESSION_ID,
        "_office_entry_ticket_claims": entry_claims,
        "_office_meeting_ticket": (
            lambda _env, _scope, _version, _account, _name, _session:
                "meeting-proof"
        ),
        "_office_meeting_ticket_claims": (
            lambda _env, _token: {"expires": 1_800_000_060_000}
        ),
        "hmac": hmac,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "quote": lambda value, safe="": value,
        "re": re,
        "world_protocol": type("_World", (), {
            "clean_display_name": staticmethod(
                lambda value, fallback: value or fallback),
        }),
    }
    _compile([_top_level_node("office_general_access_handler")], namespace)
    return namespace["office_general_access_handler"], _Request, state


def test_meeting_access_requires_entry_proof_bound_to_the_logged_in_account():
    handler, request_type, state = _general_access_handler()
    for entry in ("", "tampered"):
        denied = asyncio.run(handler(_Env(), request_type(entry)))
        assert denied == {
            "status": 401,
            "data": {"error": "registered_office_entry_required"},
        }

    state["claim_account_bi"] = "b" * 64
    mismatched = asyncio.run(handler(_Env(), request_type("valid-entry")))
    assert mismatched["status"] == 401

    state["claim_account_bi"] = "a" * 64
    admitted = asyncio.run(handler(_Env(), request_type("valid-entry")))
    assert admitted["status"] == 200
    assert admitted["data"]["meetingWebSocketUrl"].endswith("meeting-proof")
    assert admitted["data"]["accountStatus"] == "Registered"
    assert "valid-entry" not in repr(admitted)


def test_meeting_ticket_is_short_lived_tamper_evident_and_account_bound():
    namespace, clock = _ticket_helpers()
    account_bi = "c" * 64
    token = namespace["_office_meeting_ticket"](
        _Env(), "world-general", 1, account_bi, "Alice", SESSION_ID)
    claims = namespace["_office_meeting_ticket_claims"](_Env(), token)

    assert claims == {
        "scope": "world-general",
        "version": 1,
        "account_bi": account_bi,
        "session_id": SESSION_ID,
        "name": "Alice",
        "expires": clock[0] + 60 * 1000,
        "nonce": "0123456789abcdef",
    }
    replacement = "0" if token[-1] != "0" else "1"
    assert namespace["_office_meeting_ticket_claims"](
        _Env(), token[:-1] + replacement) is None
    clock[0] += 60 * 1000
    assert namespace["_office_meeting_ticket_claims"](_Env(), token) is None


def test_meeting_ticket_rejects_unbounded_or_malformed_claims():
    namespace, _clock = _ticket_helpers()
    for scope, version, account_bi, name, session_id in (
        ("private-name", 1, "a" * 64, "Alice", SESSION_ID),
        ("world-general", 0, "a" * 64, "Alice", SESSION_ID),
        ("a" * 32, 1, "not-a-blind-index", "Alice", SESSION_ID),
        ("world-general", 1, "a" * 64, "x" * 80, SESSION_ID),
        ("world-general", 1, "a" * 64, "Alice", "short"),
    ):
        try:
            namespace["_office_meeting_ticket"](
                _Env(), scope, version, account_bi, name, session_id)
        except ValueError:
            continue
        raise AssertionError("invalid Office ticket input must fail closed")


def test_office_routes_register_floor_access_and_remove_the_code_endpoint():
    route_source = _route_table()
    for route in (
        "/api/world/office/general/status",
        "/api/world/office/general/entry",
        "/api/world/office/floors",
        "/api/world/office/general/access",
        "/api/world/office/general/ws",
    ):
        assert route in route_source
    assert "/api/world/office/general/code" not in route_source
    assert "office_general_code_handler" not in ENTRY_TEXT
    assert "_office_entry_code_digest" not in ENTRY_TEXT
    assert "_office_socket_code_state" not in ENTRY_TEXT
    assert "_office_general_room_state" not in ENTRY_TEXT
    assert "office_floor_access_handler" in route_source


def test_socket_routes_authorize_before_durable_object_lookup():
    general = ast.unparse(_top_level_node("_office_general_socket_handler"))
    assert "_office_meeting_ticket_claims" in general
    assert "world_websocket_origin_allowed" in general
    assert "_request_account_session_id" in general
    assert "claims.get('session_id')" in general
    assert "[0-9a-f]{64}" in general
    assert general.index("_office_meeting_ticket_claims") < general.index(
        "_forward_office_socket")

    channel = ast.unparse(_top_level_node("_office_channel_socket_handler"))
    forward_at = channel.index("_forward_office_socket")
    assert channel.index("FROM chat_channels") < forward_at
    assert channel.index("FROM users") < forward_at
    assert channel.index("chat_channel_members") < forward_at
    assert "_office_meeting_ticket_claims" in channel
    assert "world_websocket_origin_allowed" in channel
    assert "_request_account_session_id" in channel
    assert "claims.get('session_id')" in channel

    forward = ast.unparse(_top_level_node("_forward_office_socket"))
    assert "FORKMESH_OFFICE_ROOM.idFromName" in forward


def test_office_internal_upgrade_strips_browser_credentials_and_query():
    source = ast.unparse(
        _top_level_node("office_durable_object_request")).lower()
    assert "x-forkmesh-office-claim" in source
    assert "'session_id'" in source
    assert "source_url.path" in source
    assert "source_url.query" not in source
    for forbidden in (
        "authorization",
        "cookie",
        "user-agent",
        "cf-connecting-ip",
        "x-forwarded-for",
        "referer",
    ):
        assert forbidden not in source


def test_wrangler_registers_office_room_in_prod_dev_and_migrations():
    parsed = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
    prod = {
        item["name"]: item["class_name"]
        for item in parsed["durable_objects"]["bindings"]
    }
    dev = {
        item["name"]: item["class_name"]
        for item in parsed["env"]["dev"]["durable_objects"]["bindings"]
    }
    assert prod["FORKMESH_OFFICE_ROOM"] == "ForkMeshOfficeRoom"
    assert dev["FORKMESH_OFFICE_ROOM"] == "ForkMeshOfficeRoom"
    assert {
        "tag": "v11",
        "new_sqlite_classes": ["ForkMeshOfficeRoom"],
    } in parsed["migrations"]


def test_office_room_uses_hibernation_attachments_and_revalidates_accounts():
    source = ast.unparse(_top_level_node("ForkMeshOfficeRoom"))
    for contract in (
        "acceptWebSocket",
        "getWebSockets",
        "serializeAttachment",
        "OFFICE_MESSAGE_MAX_BYTES",
        "allocate_office_seat",
        "seat-denied",
        "Seat just taken.",
        "webSocketClose",
        "webSocketError",
        "account_bi",
        "session_id",
        "auth_checked_at",
        "OFFICE_ACCESS_RECHECK_MS",
        "FROM account_sessions s",
        "JOIN users u ON u.user_bi=s.account_bi",
        "s.revoked_at=0",
        "s.expires_at>?",
        "_account_kind(account_record) != 'user'",
        "room access revoked",
    ):
        assert contract in source
    assert "re.fullmatch('[0-9a-f]{64}', account_bi)" in source
    for forbidden in (
        "ctx.storage.put",
        "ctx.storage.get",
        "chat_history",
        "roomKey",
        "passphrase",
        "entry_code",
        "office-code",
        "codeConfigured",
    ):
        assert forbidden not in source


def test_office_access_revalidation_skips_frames_inside_cadence_and_persists():
    namespace = {
        "DurableObject": object,
        "OFFICE_ACCESS_RECHECK_MS": 30_000,
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.attachment, name, default),
    }
    _compile([_top_level_node("ForkMeshOfficeRoom")], namespace)
    room = namespace["ForkMeshOfficeRoom"]()
    socket = SimpleNamespace(
        attachment=SimpleNamespace(
            auth_checked_at=1_000,
            departed=False,
            last=1_000,
        ),
    )
    checks = []
    departures = []

    async def access_current(_ws, now=None):
        checks.append(now)
        return True

    def save_socket(ws, _state, **values):
        if "auth_checked_at" in values:
            ws.attachment.auth_checked_at = values["auth_checked_at"]

    room._access_current = access_current
    room._save_socket = save_socket
    room._depart = lambda *_args: departures.append(_args)

    assert asyncio.run(room._access_current_if_due(
        socket, {}, 30_999, 30_000, 1)) is True
    assert checks == []
    assert socket.attachment.auth_checked_at == 1_000

    assert asyncio.run(room._access_current_if_due(
        socket, {}, 31_000, 31_000, 1)) is True
    assert checks == [31_000]
    assert socket.attachment.auth_checked_at == 31_000
    assert departures == []

    # The persisted timestamp survives subsequent attachment reads and keeps
    # every movement frame inside the next interval off D1.
    assert asyncio.run(room._access_current_if_due(
        socket, {}, 60_999, 60_000, 2)) is True
    assert checks == [31_000]


def test_office_access_revalidation_disconnects_when_due_session_is_revoked():
    namespace = {
        "DurableObject": object,
        "OFFICE_ACCESS_RECHECK_MS": 30_000,
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.attachment, name, default),
    }
    _compile([_top_level_node("ForkMeshOfficeRoom")], namespace)
    room = namespace["ForkMeshOfficeRoom"]()
    socket = SimpleNamespace(
        attachment=SimpleNamespace(
            auth_checked_at=1_000,
            departed=False,
            last=1_000,
        ),
    )
    checks = []
    departures = []

    async def revoked_session(_ws, now=None):
        checks.append(now)
        return False

    room._access_current = revoked_session
    room._save_socket = lambda *_args, **_kwargs: None
    room._depart = lambda *_args: departures.append(_args)

    assert asyncio.run(room._access_current_if_due(
        socket, {}, 31_000, 31_000, 1)) is False
    assert checks == [31_000]
    assert len(departures) == 1
    assert departures[0][1:] == (1008, "room access revoked")


def test_office_access_query_binds_exact_live_session_and_expiry():
    queries = []
    active = {"value": True}

    async def d1_first(_env, sql, *args):
        queries.append((sql, args))
        if "FROM account_sessions s" in sql and active["value"]:
            return {
                "data": {
                    "status": "active",
                    "kind": "user",
                },
                "is_admin": 0,
            }
        return None

    async def decrypt_row(_env, value):
        return value

    namespace = {
        "DurableObject": object,
        "Date": SimpleNamespace(now=lambda: 90_000),
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.attachment, name, default),
        "_account_kind": lambda record: record.get("kind", ""),
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "re": re,
    }
    _compile([_top_level_node("ForkMeshOfficeRoom")], namespace)
    room = namespace["ForkMeshOfficeRoom"]()
    room.env = object()
    socket = SimpleNamespace(
        attachment=SimpleNamespace(
            account_bi="a" * 64,
            session_id=SESSION_ID,
            scope="world-general",
            version=1,
        ),
    )

    assert asyncio.run(room._access_current(socket, 90_000)) is True
    assert len(queries) == 1
    sql, params = queries[-1]
    assert "s.session_id=?" in sql
    assert "s.account_bi=?" in sql
    assert "s.revoked_at=0" in sql
    assert "s.expires_at>?" in sql
    assert params == (SESSION_ID, "a" * 64, 90_000)

    active["value"] = False
    assert asyncio.run(room._access_current(socket, 120_000)) is False
    assert queries[-1][1] == (SESSION_ID, "a" * 64, 120_000)


def test_office_access_revalidates_signed_desktop_accounts_without_browser_session():
    queries = []
    active = {"value": True}

    async def d1_first(_env, sql, *args):
        queries.append((sql, args))
        if "FROM users WHERE user_bi=?" in sql and active["value"]:
            return {
                "data": {"status": "active", "kind": "user"},
                "is_admin": 0,
            }
        return None

    namespace = {
        "DurableObject": object,
        "Date": SimpleNamespace(now=lambda: 90_000),
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.attachment, name, default),
        "_account_kind": lambda record: record.get("kind", ""),
        "d1_first": d1_first,
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "re": re,
    }
    _compile([_top_level_node("ForkMeshOfficeRoom")], namespace)
    room = namespace["ForkMeshOfficeRoom"]()
    room.env = object()
    socket = SimpleNamespace(
        attachment=SimpleNamespace(
            account_bi="a" * 64,
            session_id="desktop_" + "b" * 32,
            scope="world-general",
            version=1,
        ),
    )

    assert asyncio.run(room._access_current(socket, 90_000)) is True
    assert len(queries) == 1
    assert "FROM users WHERE user_bi=?" in queries[0][0]
    assert "account_sessions" not in queries[0][0]
    assert queries[0][1] == ("a" * 64,)

    active["value"] = False
    assert asyncio.run(room._access_current(socket, 120_000)) is False


def _office_channel_access(account_bi, *, admin, member, visibility="private",
                           kind="", creator="c" * 64):
    import hmac

    async def d1_first(_env, sql, *args):
        if "FROM users WHERE user_bi=?" in sql:
            return {
                "data": {"status": "active", "kind": "user"},
                "is_admin": 1 if admin else 0,
            }
        if "FROM chat_channels" in sql:
            record = {"visibility": visibility}
            if kind:
                record["kind"] = kind
            return {"data": record, "created_by_bi": creator, "key_version": 1}
        if "FROM chat_channel_members" in sql:
            return {"allowed": 1} if member else None
        return None

    namespace = {
        "DurableObject": object,
        "Date": SimpleNamespace(now=lambda: 90_000),
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.attachment, name, default),
        "_account_kind": lambda record: record.get("kind", ""),
        "d1_first": d1_first,
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "hmac": hmac,
        "re": re,
    }
    _compile([_top_level_node("ForkMeshOfficeRoom")], namespace)
    room = namespace["ForkMeshOfficeRoom"]()
    room.env = object()
    socket = SimpleNamespace(
        attachment=SimpleNamespace(
            account_bi=account_bi,
            session_id="desktop_" + "b" * 32,
            scope="d" * 32,
            version=1,
        ),
    )
    return asyncio.run(room._access_current(socket, 90_000))


def test_office_private_channel_access_is_creator_or_invited_member_only():
    outsider = "a" * 64
    creator = "c" * 64
    assert _office_channel_access(outsider, admin=True, member=False) is False
    assert _office_channel_access(outsider, admin=False, member=False) is False
    assert _office_channel_access(outsider, admin=False, member=True) is True
    assert _office_channel_access(creator, admin=False, member=False) is True
    assert _office_channel_access(
        outsider, admin=False, member=False, visibility="public") is True
    assert _office_channel_access(
        outsider, admin=True, member=True, kind="notices") is False
    assert _office_channel_access(
        creator, admin=False, member=False, kind="notices") is True


def test_office_channel_socket_has_no_admin_bypass_for_private_channels():
    channel = ast.unparse(_top_level_node("_office_channel_socket_handler"))
    assert "is_admin" not in channel.split("FROM chat_channels")[1]


def test_durable_object_internal_actions_are_only_websocket_and_revoke():
    assignment = _top_level_assignment("OFFICE_INTERNAL_RE")
    regex_source = ast.unparse(assignment)
    assert "(ws|revoke)" in regex_source
    for retired in ("status", "entry", "code"):
        assert f"|{retired}" not in regex_source
        assert f"{retired}|" not in regex_source


def _takeover_room():
    """A room instance with only the collaborators the join path needs."""
    import importlib.util

    world_spec = importlib.util.spec_from_file_location(
        "forkmesh_world_protocol_office", ROOT.parent / "world" / "src" / "world.py")
    world_protocol = importlib.util.module_from_spec(world_spec)
    world_spec.loader.exec_module(world_protocol)

    counter = {"n": 0}

    def new_world_peer_id():
        counter["n"] += 1
        return "part%04d" % counter["n"]

    class _Socket:
        def __init__(self):
            self.sent = []

    class _Pair:
        @staticmethod
        def new():
            client, server = _Socket(), _Socket()
            return SimpleNamespace(object_values=lambda: (client, server))

    live = []
    namespace = {
        "DurableObject": object,
        "OFFICE_INTERNAL_RE": re.compile(
            r"^/api/world/office/(world-general|[0-9a-f]{32})/"
            r"v([1-9][0-9]*)/(ws|revoke)$"),
        "WORLD_ACCOUNT_TAKEOVER_CODE": 4009,
        "world_protocol": world_protocol,
        "hmac": hmac,
        "re": re,
        "Date": SimpleNamespace(now=lambda: 50_000),
        "urlparse": urlparse,
        "method_name": lambda request: "GET",
        "json_response": lambda data, status=200, **_kw: SimpleNamespace(
            status=status, data=data),
        "new_world_peer_id": new_world_peer_id,
        "WebSocketPair": _Pair,
        "to_js": lambda value: value,
        "JsResponse": SimpleNamespace(
            new=lambda *_a, **_kw: SimpleNamespace(status=101)),
        "_ws_attr": lambda ws, name, default=None: getattr(
            ws.attachment, name, default),
    }
    _compile([_top_level_node("ForkMeshOfficeRoom")], namespace)
    room = namespace["ForkMeshOfficeRoom"]()
    departures = []

    def depart(ws, code, reason):
        departures.append((ws, code, reason))
        live.remove(ws)

    room._live_sockets = lambda cleanup=False: list(live)
    room._depart = depart
    room._save_socket = lambda ws, state, **values: setattr(
        ws, "attachment", SimpleNamespace(**{**values, "id": state["id"]}))
    room._safe_send = lambda ws, frame: ws.sent.append(frame)
    room._broadcast = lambda *_a, **_kw: None
    room._socket_state = lambda ws: world_protocol.default_office_presence(
        getattr(ws.attachment, "id", "peer"), 1_000)
    room.ctx = SimpleNamespace(
        acceptWebSocket=lambda ws, _tags=None: live.append(ws))
    room._claim = lambda request: request.claim
    room.departures = departures
    room.live = live
    return room


def _office_ws_request(account_bi, nonce):
    return SimpleNamespace(
        url="https://forkmesh.example/api/world/office/world-general/v1/ws",
        headers=SimpleNamespace(get=lambda name: "websocket"),
        claim={
            "scope": "world-general",
            "version": 1,
            "nonce": nonce,
            "account_bi": account_bi,
            "session_id": SESSION_ID,
            "name": "jett",
            "accountStatus": "Registered",
        },
    )


def test_second_device_replaces_the_same_member_in_a_meeting_room():
    jett = "a" * 64
    nova = "b" * 64
    room = _takeover_room()

    asyncio.run(room.fetch(_office_ws_request(nova, "nonce-nova-1111")))
    other_member = room.live[-1]
    asyncio.run(room.fetch(_office_ws_request(jett, "nonce-jett-1111")))
    first_device = room.live[-1]

    asyncio.run(room.fetch(_office_ws_request(jett, "nonce-jett-2222")))
    second_device = room.live[-1]

    # The member's earlier device is retired with the shared handover code,
    # and nobody else in the room is disturbed.
    assert [(ws, code) for ws, code, _reason in room.departures] == [
        (first_device, 4009)]
    assert other_member in room.live
    assert first_device not in room.live

    # The newest device sees one row per person, not a twin of itself.
    welcome = second_device.sent[0]
    assert welcome["type"] == "welcome"
    assert len(welcome["participants"]) == 1


def test_meeting_takeover_uses_the_account_binding_not_a_typed_name():
    source = ast.unparse(_top_level_node("ForkMeshOfficeRoom"))
    assert "WORLD_ACCOUNT_TAKEOVER_CODE" in source
    assert (
        "hmac.compare_digest(str(_ws_attr(peer, 'account_bi', '') or ''), "
        "account_bi)") in source
    meeting_js = (
        ROOT.parent / "world" / "public" / "world" / "world-office-meeting.js"
    ).read_text(encoding="utf-8")
    assert "const OFFICE_ACCOUNT_TAKEOVER_CODE = 4009;" in meeting_js
    assert "event?.code === OFFICE_ACCOUNT_TAKEOVER_CODE" in meeting_js

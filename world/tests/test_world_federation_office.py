import ast
import asyncio
import re
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
OFFICE = ROOT / "world" / "src" / "world_office_routes.py"
APP = (ROOT / "world" / "public" / "world" / "world.js").read_text(
    encoding="utf-8")
OFFICE_JS = (ROOT / "world" / "public" / "world" / "world-office.js").read_text(
    encoding="utf-8")

VISITOR = {"name": "alice", "home": "code.example.org", "nonce": "nonce1234",
           "expiresAt": 1}


def handlers(visitor):
    tree = ast.parse(OFFICE.read_text(encoding="utf-8"))
    names = {"office_floor_access_handler", "office_attendance_handler",
             "office_general_entry_handler", "_office_visitor"}
    nodes = [node for node in tree.body
             if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
             and node.name in names]

    async def no_account(_env, _request):
        return "", None

    def json_response(data, status=200, cache_control=None,
                      extra_headers=None, **_):
        return {"status": status, "data": data}

    async def must_not_parse(*_args, **_kwargs):
        raise AssertionError("a visitor's punch must not be parsed")

    namespace = {
        "re": re,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "_account_session_record": no_account,
        "world_bearer_visitor": lambda env, request: visitor,
        "_request_origin_allowed": lambda env, request: False,
        "bounded_json_request": must_not_parse,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(OFFICE), "exec"),
        namespace)
    return namespace


def call(namespace, name, method):
    return asyncio.run(namespace[name](
        SimpleNamespace(), SimpleNamespace(method=method, headers={})))


def test_a_visitor_has_the_common_floors_under_their_home_name():
    reply = call(handlers(VISITOR), "office_floor_access_handler", "GET")
    assert reply["status"] == 200
    assert reply["data"] == {
        "authenticated": True, "visitor": True,
        "account": "alice@code.example.org",
        "allowedFloorIds": ["lobby", "rooftop"], "teams": [],
        "organizationRole": ""}


def test_a_visitors_attendance_is_accepted_but_not_recorded():
    reply = call(handlers(VISITOR), "office_attendance_handler", "POST")
    assert reply == {"status": 200, "data": {
        "ok": True, "recorded": False, "visitor": True}}


def test_meetings_stay_with_members_and_say_so():
    reply = call(handlers(VISITOR), "office_general_entry_handler", "POST")
    assert reply["status"] == 403
    assert reply["data"]["error"] == "members_only"
    # Without a visitor token nothing changes: no account is still refused.
    floors = call(handlers(None), "office_floor_access_handler", "GET")
    assert floors["status"] == 401
    assert floors["data"]["authenticated"] is False


def test_the_office_runs_against_its_owner_with_the_visitor_identity():
    assert "setOfficeOwner(owner = null) {" in APP
    assert "federation.fetchJSON(owner, path, options)" in APP
    assert 'return visitor ? { ...visitor, sessionToken: "visitor" } : null;' \
        in APP
    assert "if (owner) await this.worldFederation().token(owner);" in APP
    assert "getSession: readSession" not in APP.split(
        "async ensureOfficeRuntime(")[1].split("controller.setProximity(")[0]
    assert "if (activeSession?.visitor) {" in OFFICE_JS
    assert "Office meetings are for members of this World." in OFFICE_JS

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

import world_mesh

ROOT = Path(__file__).resolve().parents[2]
ROUTES = ROOT / "world" / "src" / "world_routes.py"
RUNTIME = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")

SECRET = b"s" * 32
NOW = 1_800_000_000_000
HUB_ENV = SimpleNamespace(
    WORLD_ORIGIN="https://world.forkmesh.com",
    APP_ORIGIN="https://app.forkmesh.com",
)
WORLDS = world_mesh.build_directory(
    world_mesh.hub_entry(HUB_ENV),
    [{"id": "a" * 32, "name": "Example Code",
      "url": "https://code.example.org/", "slot": 0}],
    "https://app.forkmesh.com")


def route_functions(*names, extra=None):
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"))
    nodes = [node for node in tree.body
             if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
             and node.name in names]
    assert {node.name for node in nodes} == set(names)

    async def directory(_env):
        return WORLDS

    def json_response(data, status=200, cache_control=None,
                      extra_headers=None, **_):
        return {"status": status, "data": data, "cache": cache_control,
                "headers": dict(extra_headers or {})}

    namespace = {
        "world_mesh": world_mesh,
        "world_mesh_directory": directory,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "_account_session_secret": lambda env: SECRET,
        "Date": SimpleNamespace(now=lambda: NOW),
        "WORLD_MESH_BODY_MAX_BYTES": 4096,
    }
    namespace.update(extra or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ROUTES), "exec"),
        namespace)
    return namespace


def request(method="GET", origin="", body=None, authorization=""):
    headers = {}
    if origin:
        headers["origin"] = origin
    if authorization:
        headers["authorization"] = authorization
    return SimpleNamespace(method=method, headers=headers, body=body)


def token(**overrides):
    claim = world_mesh.visitor_for(
        "alice", "code.example.org", "https://world.forkmesh.com",
        "nonce1234", NOW)
    claim.update(overrides)
    return world_mesh.visitor_encode(SECRET, claim)


def test_a_visitor_token_is_signed_scoped_bound_and_short_lived():
    good = token()
    assert good.startswith("fmwv1.")
    assert world_mesh.visitor_verify(
        SECRET, good, NOW + 1000, "https://world.forkmesh.com") == {
            "name": "alice", "home": "code.example.org", "nonce": "nonce1234",
            "expiresAt": NOW + world_mesh.VISITOR_TTL_MS}
    assert world_mesh.visitor_verify(
        SECRET, good, NOW + 1000, "https://other.example.org") is None
    assert world_mesh.visitor_verify(
        b"x" * 32, good, NOW + 1000, "https://world.forkmesh.com") is None
    assert world_mesh.visitor_verify(
        SECRET, good, NOW + world_mesh.VISITOR_TTL_MS + 1,
        "https://world.forkmesh.com") is None
    assert world_mesh.visitor_verify(
        SECRET, token(scope="admin"), NOW, "https://world.forkmesh.com") is None
    assert world_mesh.visitor_verify(
        SECRET, token(exp=NOW + 10 * world_mesh.VISITOR_TTL_MS), NOW,
        "https://world.forkmesh.com") is None
    assert world_mesh.visitor_verify(
        SECRET, token(home="evil example"), NOW,
        "https://world.forkmesh.com") is None
    # A visa is not a visitor token, and a visitor token is not a visa.
    visa = world_mesh.visa_encode(SECRET, world_mesh.visa_for(
        "alice", "https://code.example.org", "https://world.forkmesh.com",
        "nonce1234", NOW))
    assert world_mesh.visitor_verify(
        SECRET, "fmwv1." + visa, NOW, "https://world.forkmesh.com") is None
    assert world_mesh.visa_verify(
        SECRET, good[len("fmwv1."):], NOW, "https://code.example.org",
        "https://world.forkmesh.com") is None
    assert world_mesh.bearer_visitor_token("Bearer " + good) == good
    assert world_mesh.bearer_visitor_token("Bearer fmat_x") == ""
    assert world_mesh.bearer_visitor_token(good) == ""


def test_only_listed_world_endpoints_and_neighbour_pages_get_cors():
    ns = route_functions("world_federation_cors")
    cors = lambda method, origin, path: asyncio.run(ns["world_federation_cors"](
        HUB_ENV, request(method, origin), path))
    assert cors("GET", "https://code.example.org", "/api/world/events") == {
        "access-control-allow-origin": "https://code.example.org",
        "vary": "Origin"}
    preflight = cors("OPTIONS", "https://code.example.org",
                     "/api/world/visa/session/")
    assert preflight["access-control-allow-headers"] == \
        "authorization, content-type, accept"
    assert "access-control-allow-credentials" not in preflight
    assert cors("GET", "https://evil.example.net", "/api/world/events") == {}
    assert cors("GET", "https://world.forkmesh.com", "/api/world/events") == {}
    assert cors("GET", "https://code.example.org", "/api/accounts/me") == {}
    assert cors("GET", "", "/api/world/events") == {}
    assert world_mesh.federated_path("/api/world/office/general/entry/")
    assert not world_mesh.federated_path("/api/world/visa")


def test_neighbour_town_reads_get_cors_for_reading_only():
    ns = route_functions("world_federation_cors")

    def cors(method, path, requested=""):
        sent = request(method, "https://code.example.org")
        if requested:
            sent.headers["access-control-request-method"] = requested
        return asyncio.run(ns["world_federation_cors"](HUB_ENV, sent, path))

    assert cors("GET", "/api/accounts/users")[
        "access-control-allow-origin"] == "https://code.example.org"
    assert cors("POST", "/api/accounts/users") == {}
    assert cors("OPTIONS", "/api/accounts/users", "POST") == {}
    assert cors("OPTIONS", "/api/accounts/users", "GET")[
        "access-control-allow-methods"] == "GET, OPTIONS"
    assert cors("OPTIONS", "/api/world/office/general/entry", "POST")[
        "access-control-allow-methods"] == "GET, POST, OPTIONS"


def test_a_visa_is_exchanged_once_for_a_visitor_token():
    async def visitor(_env, _request, visa, arrival=False):
        assert arrival is True
        if visa != "good-visa":
            return {"error": "invalid_visa"}
        return {"home": "code.example.org",
                "claim": {"name": "alice", "ticketNonce": "nonce1234"}}

    async def body(req):
        return req.body

    ns = route_functions(
        "world_visa_session_handler", "world_visitor_claim",
        "world_bearer_visitor",
        extra={"world_mesh_visitor": visitor,
               "bounded_json_request": lambda req, limit: body(req)})
    handler = ns["world_visa_session_handler"]
    reply = asyncio.run(handler(HUB_ENV, request(
        "POST", "https://code.example.org", {"visa": "good-visa"})))
    assert reply["status"] == 200 and reply["cache"].startswith("no-store")
    data = reply["data"]
    assert (data["name"], data["home"]) == ("alice", "code.example.org")
    assert data["expiresAt"] == NOW + world_mesh.VISITOR_TTL_MS
    assert world_mesh.visitor_verify(
        SECRET, data["token"], NOW, "https://world.forkmesh.com")
    # The token works as a bearer and on the visit socket, for its home only.
    assert ns["world_bearer_visitor"](HUB_ENV, request(
        authorization="Bearer " + data["token"]))["name"] == "alice"
    assert ns["world_visitor_claim"](
        HUB_ENV, data["token"], "code.example.org")["accountStatus"] == \
        world_mesh.VISITOR_STATUS
    assert ns["world_visitor_claim"](
        HUB_ENV, data["token"], "other.example.org") is None
    refused = [
        asyncio.run(handler(HUB_ENV, request(
            "POST", "https://code.example.org", {"visa": "forged"}))),
        asyncio.run(handler(HUB_ENV, request(
            "POST", "https://evil.example.net", {"visa": "good-visa"}))),
        asyncio.run(handler(HUB_ENV, request("GET"))),
    ]
    assert [item["status"] for item in refused] == [401, 403, 405]


def test_the_app_routes_the_exchange_cors_and_visitor_sockets():
    assert '"/api/world/visa/session"' in RUNTIME
    assert "await world_federation_cors(self.env, request, url.path)" in RUNTIME
    assert 'Response("", status=204, headers=fed_cors)' in RUNTIME
    assert "response = self._with_headers(response, fed_cors)" in RUNTIME
    assert 'visitor_values = query.get("visitor") or []' in RUNTIME
    assert "world_visitor_claim(" in RUNTIME
    for name in ("world_visa_session_handler", "world_federation_cors",
                 "world_federated_path", "world_visitor_claim",
                 "world_bearer_visitor"):
        assert '_world_routes.export("%s")' % name in ENTRY
    json.dumps(sorted(world_mesh.FEDERATED_PATHS))

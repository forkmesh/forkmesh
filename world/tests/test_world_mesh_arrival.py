import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

import world as world_protocol
import world_mesh
import world_routes

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")
APP = (ROOT / "world" / "public" / "world" / "world.js").read_text(
    encoding="utf-8")

HUB_ENV = SimpleNamespace(
    WORLD_ORIGIN="https://world.forkmesh.com",
    APP_ORIGIN="https://app.forkmesh.com",
)
INSTANCE_ID = "a" * 32
WORLDS = world_mesh.build_directory(
    world_mesh.hub_entry(HUB_ENV),
    [{"id": INSTANCE_ID, "name": "Example Code",
      "url": "https://code.example.org/", "slot": 0}],
    "https://app.forkmesh.com")


def visitor_function(monkeypatch, verify_reply):
    calls = []

    async def directory(_env):
        return WORLDS

    async def fetch(url, options, _timeout):
        calls.append((url, json.loads(options["body"])))
        status, payload = verify_reply

        async def text():
            return json.dumps(payload)

        return SimpleNamespace(
            status=status, text=text)

    monkeypatch.setattr(world_routes, "json", json, raising=False)
    monkeypatch.setattr(world_routes, "world_mesh", world_mesh, raising=False)
    monkeypatch.setattr(world_routes, "world_protocol", world_protocol, raising=False)
    monkeypatch.setattr(world_routes, "world_mesh_directory", directory, raising=False)
    monkeypatch.setattr(world_routes, "js_fetch_with_timeout", fetch, raising=False)
    return world_routes.world_mesh_visitor, calls


def visa_from(home):
    return world_mesh.visa_encode(b"s" * 32, world_mesh.visa_for(
        "alice", home, "https://world.forkmesh.com", "nonce1234",
        1_800_000_000_000))


def same_origin_request():
    return SimpleNamespace(headers={"origin": "https://world.forkmesh.com"})


def test_an_arrival_visa_is_vouched_for_by_the_home_it_claims(monkeypatch):
    visit, calls = visitor_function(monkeypatch,
        (200, {"ok": True, "name": "alice", "nonce": "nonce1234"}))
    visitor = asyncio.run(visit(
        HUB_ENV, same_origin_request(), visa_from("https://code.example.org"),
        arrival=True))
    assert visitor["home"] == "code.example.org"
    assert visitor["claim"]["name"] == "alice"
    assert visitor["claim"]["accountStatus"] == world_mesh.VISITOR_STATUS
    assert calls == [("https://code.example.org/api/world/visa/verify",
                      {"visa": visa_from("https://code.example.org"),
                       "destination": "https://world.forkmesh.com"})]


def test_an_arrival_from_an_unlisted_or_refusing_world_is_not_signed_in(monkeypatch):
    visit, calls = visitor_function(monkeypatch, (401, {"ok": False}))
    request = same_origin_request()
    assert asyncio.run(visit(
        HUB_ENV, request, visa_from("https://evil.example.net"),
        arrival=True)) is None
    assert calls == []
    assert asyncio.run(visit(HUB_ENV, request, "", arrival=True)) is None
    refused = asyncio.run(visit(
        HUB_ENV, request, visa_from("https://code.example.org"),
        arrival=True))
    assert refused == {"error": "invalid_visa"}


def test_cross_world_sockets_still_take_their_home_from_the_origin(monkeypatch):
    visit, _calls = visitor_function(monkeypatch,
        (200, {"ok": True, "name": "alice", "nonce": "nonce1234"}))
    request = SimpleNamespace(headers={"origin": "https://code.example.org"})
    assert asyncio.run(visit(HUB_ENV, request))["home"] == "code.example.org"


def test_the_socket_and_page_spend_an_arrival_visa_once():
    assert "if len(visa_values) == 1 and not query.get(\"ticket\"):" in RUNTIME
    assert "self.env, request, visa_values[0], arrival=True)" in RUNTIME
    assert "watch=bool(mesh_visitor) and not arrival and (" in RUNTIME
    assert "this.arrivalVisa = takeArrivalVisa();" in APP
    assert 'socketURL.searchParams.set("visa", this.arrivalVisa);' in APP
    assert APP.count('this.arrivalVisa = "";') == 1
    assert "history.replaceState(history.state, \"\", url.href);" in APP

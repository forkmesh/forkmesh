from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

import pytest

httpx = pytest.importorskip("httpx")
pytest.importorskip("uvicorn")
websockets_sync = pytest.importorskip("websockets.sync.client")
websockets_exceptions = pytest.importorskip("websockets.exceptions")

from forkmesh_host import paths  # noqa: E402

TURN_SECRET = "e2e-turn-secret-0123456789"


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture(scope="module")
def server(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("org-office-e2e")
    assets = tmp / "assets"
    shutil.copytree(paths.APP_DIR / "public", assets)
    (assets / "index.html").write_text("<!doctype html><title>home</title>")
    (assets / "404.html").write_text("<!doctype html><title>404</title>")
    port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    env = sandbox_env(tmp)
    env.update({
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_ASSETS_DIR": str(assets),
        "FORKMESH_HOST_OFFLINE": "1",
        "PYTHONUNBUFFERED": "1",
        "FORKMESH_VAR_DATA_KEY": "office-e2e-" + os.urandom(16).hex(),
        "FORKMESH_VAR_ADMIN_PASS": "office-admin-" + os.urandom(8).hex(),
        "FORKMESH_VAR_APP_ORIGIN": origin,
        "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin,
        "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_SECRET_KEY":
            "1x0000000000000000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
        "FORKMESH_VAR_OFFICE_STUN_URLS": "stun:stun.example.test:3478",
        "FORKMESH_VAR_OFFICE_TURN_URLS": "turn:turn.example.test:3478",
        "FORKMESH_VAR_OFFICE_TURN_SECRET": TURN_SECRET,
    })
    log = open(tmp / "server.log", "w+")
    process = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--no-cron", "--migrate", "--no-build", "--log-level", "warning"],
        cwd=str(paths.HOST_DIR), env=env, stdout=log,
        stderr=subprocess.STDOUT)
    deadline = time.time() + 90
    while time.time() < deadline:
        if process.poll() is not None:
            log.seek(0)
            pytest.fail("server exited: " + log.read()[-4000:])
        try:
            if httpx.get(origin + "/health", timeout=1).status_code == 200:
                break
        except httpx.HTTPError:
            time.sleep(0.2)
    else:
        process.kill()
        pytest.fail("server did not start")
    client = httpx.Client(base_url=origin, timeout=60,
                          headers={"origin": origin})

    def ops(*args):
        done = subprocess.run(
            [sys.executable, "-m", "forkmesh_host", "ops", *args],
            cwd=str(paths.HOST_DIR), env=env, capture_output=True, text=True,
            timeout=180)
        assert done.returncode == 0, done.stderr[-2000:]

    users = {}
    for name in ("alice", "bob", "carol"):
        page = client.get("/signup")
        field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
        signup = client.post("/api/accounts/signup", json={
            "nodeName": name, field: name + "@example.com",
            "password": "correct-horse-battery",
            "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
        assert signup.status_code == 201, signup.text[:300]
        ops("verify-email", name)
        login = client.post("/api/accounts/login", json={
            "nodeName": name, "password": "correct-horse-battery"})
        assert login.status_code == 200, login.text[:300]
        users[name] = login.json()["sessionToken"]

    def call(user, method, path, body=None):
        response = client.request(
            method, path, json=body,
            headers={"authorization": "Bearer " + users[user]})
        assert response.status_code < 300, response.text[:300]

    call("alice", "POST", "/api/orgs", {"name": "acme", "displayName": "A"})
    call("alice", "POST", "/api/orgs/acme/members",
         {"member": "bob", "role": "member"})
    call("alice", "POST", "/api/orgs/acme/teams",
         {"team": "engineering", "permission": "write"})
    call("alice", "POST", "/api/orgs/acme/teams/engineering/members",
         {"member": "bob"})
    call("carol", "POST", "/api/orgs", {"name": "globex", "displayName": "G"})
    yield {"origin": origin, "port": port, "client": client, "users": users,
           "log": log}
    client.close()
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()
    log.close()


def _cookies(server, user):
    return {"cookie": "forkmesh_account=" + server["users"][user]}


def _entry(server, user, org, origin=True):
    headers = dict(_cookies(server, user))
    headers["content-type"] = "application/json"
    if not origin:
        headers["origin"] = "https://evil.example"
    return server["client"].post("/api/office/entry", headers=headers,
                                 content=json.dumps({"org": org}))


def _socket(server, user, ticket_path, origin=None):
    url = "ws://127.0.0.1:%d%s" % (server["port"], ticket_path)
    return websockets_sync.connect(
        url, origin=origin or server["origin"],
        additional_headers=_cookies(server, user), open_timeout=20)


def _recv(ws, kind, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = ws.recv(timeout=max(0.1, deadline - time.time()))
        if raw == "pong":
            continue
        message = json.loads(raw)
        if message.get("t") == kind:
            return message
    raise AssertionError("no %s message" % kind)


def _join(server, user, org, **extra):
    entry = _entry(server, user, org)
    assert entry.status_code == 200, entry.text[:300]
    ws = _socket(server, user, entry.json()["ws"])
    ws.send(json.dumps({"t": "join", **extra}))
    return ws, _recv(ws, "init")


def test_session_lists_only_the_callers_organizations(server):
    client = server["client"]
    anonymous = client.get("/api/office/session").json()
    assert anonymous["signedIn"] is False
    alice = client.get("/api/office/session",
                       headers=_cookies(server, "alice")).json()
    assert alice["org"] == "acme" and alice["role"] == "owner"
    assert [o["name"] for o in alice["orgs"]] == ["acme"]
    carol = client.get("/api/office/session?org=acme",
                       headers=_cookies(server, "carol")).json()
    assert carol["org"] == "globex"
    bob = client.get("/api/office/session",
                     headers=_cookies(server, "bob")).json()
    assert bob["teams"] == ["engineering"] and bob["unlocked"] == ["a"]


def test_signed_in_member_always_gets_an_office_to_enter(server):
    client = server["client"]
    for query in ("", "?org=globex", "?org=left-org"):
        alice = client.get("/api/office/session" + query,
                           headers=_cookies(server, "alice")).json()
        assert alice["signedIn"] is True
        assert alice["org"] == "acme"
        assert alice["role"] == "owner"
    assert _entry(server, "alice", "acme").status_code == 200


def test_entry_requires_membership_origin_and_a_session(server):
    assert _entry(server, "carol", "acme").status_code == 403
    assert _entry(server, "alice", "acme", origin=False).status_code == 403
    no_session = server["client"].post(
        "/api/office/entry", content=json.dumps({"org": "acme"}),
        headers={"content-type": "application/json"})
    assert no_session.status_code == 401
    ok = _entry(server, "alice", "acme").json()
    turn = [s for s in ok["iceServers"] if "username" in s][0]
    expiry = int(turn["username"].split(":")[0])
    assert 0 < expiry - time.time() <= 600 + 5
    assert turn["credential"] == base64.b64encode(hmac.new(
        TURN_SECRET.encode(), turn["username"].encode(),
        hashlib.sha1).digest()).decode()
    assert turn["urls"] == ["turn:turn.example.test:3478"]
    for denied in (no_session, _entry(server, "carol", "acme")):
        assert "iceServers" not in denied.json()
    assert TURN_SECRET not in json.dumps(ok)
    assert ok["iceServers"][0]["urls"] == ["stun:stun.example.test:3478"]


def test_tickets_are_bound_to_the_session_origin_and_single_socket(server):
    ticket = _entry(server, "alice", "acme").json()["ws"]
    with pytest.raises(websockets_exceptions.InvalidStatus):
        _socket(server, "carol", ticket)
    with pytest.raises(websockets_exceptions.InvalidStatus):
        _socket(server, "alice", ticket, origin="https://evil.example")
    with pytest.raises(websockets_exceptions.InvalidStatus):
        _socket(server, "alice", "/api/office/ws?ticket=v1.forged")
    with _socket(server, "alice", ticket) as ws:
        ws.send(json.dumps({"t": "join"}))
        assert _recv(ws, "init")["office"] == "acme"
        with pytest.raises(websockets_exceptions.InvalidStatus):
            _socket(server, "alice", ticket)


def test_members_meet_and_other_orgs_stay_out(server):
    alice, init_a = _join(server, "alice", "acme", x=30, y=20)
    bob, init_b = _join(server, "bob", "acme", x=31, y=20)
    carol, init_c = _join(server, "carol", "globex", x=30, y=20)
    try:
        assert init_b["peers"][0][1] == "alice"
        assert init_c["peers"] == [] and init_c["office"] == "globex"
        assert _recv(alice, "j")["p"][1] == "bob"
        offer = {"sdp": {"type": "offer", "sdp": "v=0"}}
        alice.send(json.dumps({"t": "sig", "to": init_b["you"][0],
                               "d": offer}))
        assert _recv(bob, "sig")["d"] == offer
        carol.send(json.dumps({"t": "sig", "to": init_a["you"][0],
                               "d": offer}))
        with pytest.raises(TimeoutError):
            _recv(alice, "sig", timeout=1.5)
    finally:
        for ws in (alice, bob, carol):
            ws.close()


def test_office_chat_is_the_orgs_site_channels(server):
    client = server["client"]

    def session(user):
        return client.get("/api/office/session",
                          headers=_cookies(server, user)).json()

    def channels(user):
        body = client.get("/api/chat/channels",
                          headers=_cookies(server, user)).json()
        return {c["id"]: c for c in body["channels"]}

    alice = session("alice")["chat"]
    bob = session("bob")["chat"]
    carol = session("carol")["chat"]
    assert sorted(alice) == sorted(["general", "engineering", "design",
                                    "coffee", "lobby", "garden"])
    assert "engineering" in bob and "design" not in bob
    assert not set(carol.values()) & set(alice.values())
    listed = channels("bob")
    general = listed[alice["general"]]
    assert general["kind"] == "office" and general["canManage"] is False
    assert general["name"] == "office/acme/general"
    assert alice["design"] not in listed
    assert alice["general"] not in channels("carol")
    denied = client.get(
        "/api/chat/channels/%s/room-access" % alice["general"],
        headers=_cookies(server, "carol"))
    assert denied.status_code == 404
    access = client.get(
        "/api/chat/channels/%s/room-access" % alice["engineering"],
        headers=_cookies(server, "bob")).json()
    assert access["room"].startswith("chat-channel:" + alice["engineering"])
    managed = client.post(
        "/api/chat/channels/%s/members" % alice["general"],
        headers=dict(_cookies(server, "alice"),
                     **{"content-type": "application/json"}),
        content=json.dumps({"username": "carol"}))
    assert managed.status_code == 403
    removed = client.request(
        "DELETE", "/api/orgs/acme/teams/engineering/members",
        headers={"authorization": "Bearer " + server["users"]["alice"],
                 "content-type": "application/json"},
        content=json.dumps({"member": "bob"}))
    assert removed.status_code < 300, removed.text[:300]
    assert alice["engineering"] not in channels("bob")

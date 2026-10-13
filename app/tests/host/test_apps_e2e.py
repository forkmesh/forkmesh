from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import struct
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import httpx
import pytest

from forkmesh_host import paths

HELLO = paths.REPO_DIR / "examples" / "apps" / "hello-app" / "server.py"


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _wait(url, process, log, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            log.seek(0)
            pytest.fail("process exited: " + log.read()[-4000:])
        try:
            if httpx.get(url, timeout=1).status_code == 200:
                return
        except httpx.HTTPError:
            time.sleep(0.2)
    process.kill()
    pytest.fail("did not start: " + url)


def _stop(process):
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()


@pytest.fixture(scope="module")
def stack(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("apps-e2e")
    assets = tmp / "assets"
    shutil.copytree(paths.APP_DIR / "public", assets)
    (assets / "index.html").write_text("<!doctype html><title>home</title>")
    (assets / "404.html").write_text("<!doctype html><title>404</title>")
    port = _free_port()
    hello_port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    hello_origin = "http://127.0.0.1:%d" % hello_port
    env = sandbox_env(tmp)
    env.update({
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_ASSETS_DIR": str(assets),
        "FORKMESH_EGRESS_ALLOW": "127.0.0.1:%d" % hello_port,
        "FORKMESH_VAR_APPS_DEV_ENDPOINT_ORIGINS": hello_origin,
        "FORKMESH_VAR_DATA_KEY": "e2e-data-key-" + os.urandom(16).hex(),
        "FORKMESH_VAR_ADMIN_PASS": "e2e-admin-" + os.urandom(8).hex(),
        "FORKMESH_VAR_APP_ORIGIN": origin,
        "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin,
        "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_SECRET_KEY":
            "1x0000000000000000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
    })
    log = open(tmp / "server.log", "w+")
    server = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--no-cron", "--migrate", "--no-build", "--log-level", "warning"],
        cwd=str(paths.HOST_DIR), env=env, stdout=log, stderr=subprocess.STDOUT)
    _wait(origin + "/health", server, log)
    info = {"origin": origin, "env": env, "tmp": tmp, "log": log,
            "hello_port": hello_port, "hello_origin": hello_origin,
            "server": server}
    yield info
    hello = info.get("hello")
    if hello is not None:
        _stop(hello)
    _stop(server)
    log.close()


@pytest.fixture(scope="module")
def client(stack):
    with httpx.Client(base_url=stack["origin"], timeout=30,
                      headers={"origin": stack["origin"]}) as http:
        yield http


def _ops(stack, *args):
    return subprocess.run(
        [sys.executable, "-m", "forkmesh_host", "ops", *args],
        cwd=str(paths.HOST_DIR), env=stack["env"], capture_output=True,
        text=True, timeout=120)


def _account(client, stack, name):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    signup = client.post("/api/accounts/signup", json={
        "nodeName": name, field: name + "@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert signup.status_code == 201, signup.text[:300]
    done = _ops(stack, "verify-email", name)
    assert done.returncode == 0, done.stderr[-2000:]
    login = client.post("/api/accounts/login", json={
        "nodeName": name, "password": "correct-horse-battery"})
    assert login.status_code == 200, login.text[:300]
    client.cookies.clear()
    return {"authorization": "Bearer " + login.json()["sessionToken"]}


def _make_admin(stack, name):
    database = Path(stack["tmp"], "data", "d1.sqlite")
    assert database.is_file()
    with sqlite3.connect(str(database), timeout=30) as db:
        rows = db.execute("SELECT user_bi, username FROM users").fetchall()
        target = [bi for bi, username in rows if username == name]
        assert target, rows
        db.execute("UPDATE users SET is_admin=1 WHERE user_bi=?", (target[0],))


def _totp(secret):
    key = base64.b32decode(secret + "=" * ((8 - len(secret) % 8) % 8))
    mac = hmac.new(key, struct.pack(">Q", int(time.time()) // 30),
                   hashlib.sha1).digest()
    offset = mac[-1] & 0x0F
    return "%06d" % (
        (struct.unpack(">I", mac[offset:offset + 4])[0] & 0x7FFFFFFF)
        % 1000000)


def _enroll_admin(client, name):
    credentials = {"nodeName": name, "password": "correct-horse-battery"}
    setup = client.post("/api/accounts/login", json=credentials)
    assert setup.status_code == 401, setup.text[:300]
    assert setup.json()["error"] == "totp_setup_required"
    assert "sessionToken" not in setup.json()
    secret = setup.json()["totpSecret"]
    assert setup.json()["otpauthUri"].startswith("otpauth://totp/ForkMesh:")
    login = client.post("/api/accounts/login",
                        json={**credentials, "totp": _totp(secret)})
    assert login.status_code == 200, login.text[:300]
    client.cookies.clear()
    without_code = client.post("/api/accounts/login", json=credentials)
    assert without_code.json()["error"] == "bad_totp"
    client.cookies.clear()
    return {"authorization": "Bearer " + login.json()["sessionToken"]}


@pytest.fixture(scope="module")
def people(client, stack):
    return {
        "alice": _account(client, stack, "appalice"),
        "bob": _account(client, stack, "appbob"),
        "root": _account(client, stack, "approot"),
    }


@pytest.fixture(scope="module")
def registered(client, stack, people):
    hello = stack["hello_origin"]
    created = client.post("/api/apps", headers=people["alice"], json={
        "name": "Hello App",
        "description": "The ForkMesh example app.",
        "icon": {"glyph": "H", "color": "#16a34a"},
        "redirectUris": [hello + "/oauth/callback"],
        "scopes": ["repos:read", "chat:write", "pings:read", "pings:write",
                   "commands"],
        "events": ["ping.created"],
        "eventUrl": hello + "/events",
        "commands": [{"command": "hello", "url": hello + "/commands",
                      "description": "Say hello"}],
        "interactivityUrl": hello + "/interactive",
        "bot": {"enabled": True},
    })
    assert created.status_code == 201, created.text[:500]
    return created.json()


def _start_hello(stack, registered):
    if stack.get("hello") is not None:
        return
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    env.update({
        "HELLO_PORT": str(stack["hello_port"]),
        "HELLO_PUBLIC_URL": stack["hello_origin"],
        "FORKMESH_ORIGIN": stack["origin"],
        "FORKMESH_CLIENT_ID": registered["app"]["clientId"],
        "FORKMESH_CLIENT_SECRET": registered["clientSecret"],
        "FORKMESH_SIGNING_SECRET": registered["signingSecret"],
        "HELLO_STATE_FILE": str(stack["tmp"] / "hello-state.json"),
    })
    log = open(stack["tmp"] / "hello.log", "w+")
    process = subprocess.Popen([sys.executable, str(HELLO)], env=env,
                               stdout=log, stderr=subprocess.STDOUT)
    _wait(stack["hello_origin"] + "/health", process, log)
    stack["hello"] = process
    stack["hello_log"] = log


def _hello_state(stack):
    return httpx.get(stack["hello_origin"] + "/state", timeout=5).json()


def test_consent_page_and_scope_catalog_are_served(client, stack):
    page = client.get("/oauth/authorize?client_id=x")
    assert page.status_code == 200
    assert "data-consent-scopes" in page.text
    assert "frame-ancestors 'none'" in page.headers["content-security-policy"]
    assert client.get("/oauth-authorize.js").status_code == 200
    scopes = client.get("/api/apps/scopes").json()
    assert "agents:run" in [s["scope"] for s in scopes["scopes"]]
    assert client.get("/dashboard/apps").status_code == 200
    assert client.get("/dashboard/apps/build").status_code == 200


def test_oauth_install_with_pkce_through_the_example_app(
        client, stack, people, registered):
    _start_hello(stack, registered)
    begin = httpx.get(stack["hello_origin"] + "/install",
                      follow_redirects=False, timeout=5)
    assert begin.status_code == 302
    authorize_url = urlparse(begin.headers["location"])
    assert authorize_url.path == "/oauth/authorize"
    params = {k: v[0] for k, v in parse_qs(authorize_url.query).items()}
    assert params["code_challenge_method"] == "S256"

    preview = client.get("/api/apps/oauth/authorize", params=params,
                         headers=people["alice"])
    assert preview.status_code == 200, preview.text[:300]
    assert preview.json()["app"]["name"] == "Hello App"

    mismatch = client.post("/api/apps/oauth/authorize",
                           headers=people["alice"], json=dict(
                               params, redirect_uri="https://evil.example/cb",
                               decision="allow", target="user"))
    assert mismatch.status_code == 400
    assert mismatch.json()["error"] == "redirect_uri_mismatch"

    allowed = client.post("/api/apps/oauth/authorize",
                          headers=people["alice"],
                          json=dict(params, decision="allow", target="user"))
    assert allowed.status_code == 200, allowed.text[:300]
    callback = httpx.get(allowed.json()["redirect"], timeout=30)
    assert callback.status_code == 200, callback.text[:500]
    state = _hello_state(stack)
    assert state["installed"] is True
    assert state["bot"]["name"] == "Hello App (app)"
    assert set(state["scopes"]) >= {"repos:read", "commands"}

    installed = client.get("/api/apps/installed", headers=people["alice"])
    assert [i["app"]["slug"] for i in installed.json()["installs"]] == [
        "hello-app"]


def test_event_url_verification_and_signed_events(client, stack, people,
                                                  registered):
    _start_hello(stack, registered)
    app_id = registered["app"]["id"]
    verified = client.post("/api/apps/%s/verify" % app_id,
                           headers=people["alice"], json={})
    assert verified.status_code == 200, verified.text[:300]
    assert verified.json()["app"]["eventUrlVerified"] is True
    log = client.get("/api/apps/%s/deliveries" % app_id,
                     headers=people["alice"]).json()
    assert log["deliveries"][0]["kind"] == "verify"
    assert log["deliveries"][0]["outcome"] == "delivered"
    assert client.get("/api/apps/%s/deliveries" % app_id,
                      headers=people["bob"]).status_code == 404


@pytest.fixture(scope="module")
def channel(client, people):
    created = client.post("/api/chat/channels", headers=people["alice"],
                          json={"name": "apps-lab", "visibility": "public"})
    assert created.status_code in (200, 201), created.text[:300]
    return created.json()["channel"]["id"]


def test_slash_command_round_trip_and_bot_post(client, stack, people,
                                               registered, channel):
    _start_hello(stack, registered)
    listed = client.get("/api/apps/commands", headers=people["alice"])
    assert [c["command"] for c in listed.json()["commands"]] == ["hello"]
    assert client.get("/api/apps/commands",
                      headers=people["bob"]).json()["commands"] == []

    reply = client.post("/api/apps/commands/run", headers=people["alice"],
                        json={"text": "/hello", "channelId": channel})
    assert reply.status_code == 200, reply.text[:500]
    body = reply.json()
    assert body["responseType"] == "ephemeral"
    assert body["text"] == "Hello, @appalice!"
    assert body["app"]["botName"] == "Hello App (app)"
    assert _hello_state(stack)["commands"][-1] == {
        "command": "hello", "text": "", "user": "appalice"}

    announced = client.post("/api/apps/commands/run", headers=people["alice"],
                            json={"text": "/hello announce",
                                  "channelId": channel})
    assert announced.json()["responseType"] == "in_channel", announced.text
    history = client.get("/api/chat/channels/%s/history" % channel,
                         headers=people["alice"])
    assert history.status_code == 200
    assert history.json()["messages"], history.text[:300]


def test_interactive_callback_and_bot_web_api(client, stack, people,
                                              registered, channel):
    _start_hello(stack, registered)
    posted = httpx.get(stack["hello_origin"] + "/post",
                       params={"channel": channel, "text": "Hi from the bot"},
                       timeout=30)
    assert posted.status_code == 200, posted.text[:500]
    message_id = posted.json()["messageId"]
    wave = client.post("/api/apps/interact", headers=people["bob"], json={
        "appMessageId": message_id, "channelId": channel,
        "actionId": "wave", "value": "wave"})
    assert wave.status_code == 200, wave.text[:500]
    assert wave.json()["responseType"] == "in_channel"
    assert _hello_state(stack)["actions"][-1] == {
        "actionId": "wave", "value": "wave", "user": "appbob"}
    forged = client.post("/api/apps/interact", headers=people["bob"], json={
        "appMessageId": message_id, "channelId": channel,
        "actionId": "wave", "value": "rm -rf"})
    assert forged.status_code == 400
    mood = client.post("/api/apps/interact", headers=people["bob"], json={
        "appMessageId": message_id, "channelId": channel,
        "actionId": "mood", "value": "great"})
    assert mood.json()["text"] == "Noted: great."
    log = client.get("/api/apps/%s/deliveries" % registered["app"]["id"],
                     headers=people["alice"]).json()["deliveries"]
    kinds = {entry["kind"] for entry in log}
    assert {"verify", "command", "interaction"} <= kinds


def test_revocation_sends_uninstalled_and_kills_the_token(
        client, stack, people, registered):
    _start_hello(stack, registered)
    installed = client.get("/api/apps/installed", headers=people["alice"])
    install_id = installed.json()["installs"][0]["installId"]
    events_before = len(_hello_state(stack)["events"])
    removed = client.delete("/api/apps/installs/" + install_id,
                            headers=people["alice"])
    assert removed.status_code == 200
    deadline = time.time() + 10
    state = _hello_state(stack)
    while len(state["events"]) == events_before and time.time() < deadline:
        time.sleep(0.2)
        state = _hello_state(stack)
    assert state["events"][-1]["type"] == "app.uninstalled"
    after = httpx.get(stack["hello_origin"] + "/post",
                      params={"channel": "c" * 32}, timeout=30)
    assert after.status_code == 401


def test_directory_review_and_star_ratings(client, stack, people):
    _make_admin(stack, "approot")
    created = client.post("/api/apps", headers=people["alice"], json={
        "name": "Rate Me", "description": "Directory test.",
        "redirectUris": ["https://rate.example.org/cb"],
        "homepageUrl": "https://rate.example.org/",
        "scopes": ["repos:read"]})
    assert created.status_code == 201, created.text[:300]
    app = created.json()["app"]
    submitted = client.post("/api/apps/%s/submit" % app["id"],
                            headers=people["alice"], json={})
    assert submitted.json()["listing"] == "pending"
    assert client.get("/api/apps/review",
                      headers=people["bob"]).status_code == 403
    assert client.get("/api/apps/review",
                      headers=people["root"]).status_code == 403
    root = _enroll_admin(client, "approot")
    queue = client.get("/api/apps/review", headers=root)
    assert queue.status_code == 200, queue.text[:300]
    item = next(a for a in queue.json()["apps"] if a["id"] == app["id"])
    approved = client.post("/api/apps/%s/review" % app["id"],
                           headers=root,
                           json={"action": "approve",
                                 "contentHash": item["contentHash"]})
    assert approved.json()["listing"] == "listed", approved.text[:300]
    directory = client.get("/api/apps/directory").json()["apps"]
    assert [a["slug"] for a in directory] == ["rate-me"]

    url = "/api/market/reviews/forkmesh-apps/rate-me"
    refused = client.post(url, headers=people["bob"], json={"stars": 5})
    assert refused.status_code == 403
    assert refused.json()["error"] == "package_use_required"
    installed = client.post("/api/apps/oauth/authorize",
                            headers=people["bob"], json={
                                "response_type": "code",
                                "client_id": app["clientId"],
                                "redirect_uri": "https://rate.example.org/cb",
                                "scope": "repos:read", "state": "s",
                                "code_challenge": "A" * 43,
                                "code_challenge_method": "S256",
                                "target": "user", "decision": "allow"})
    assert installed.status_code == 200, installed.text[:300]
    rated = client.post(url, headers=people["bob"], json={"stars": 5})
    assert rated.status_code == 201, rated.text[:300]
    own = client.post(url, headers=people["alice"], json={"stars": 5})
    assert own.json()["error"] == "maintainer_cannot_rate"
    listed = client.get("/api/apps/directory").json()["apps"][0]
    assert listed["rating"] == {"average": 5.0, "count": 1}

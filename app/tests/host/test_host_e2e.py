"""End to end: the unchanged Worker app under uvicorn on CPython.

Starts ``python -m forkmesh_host serve`` in a subprocess on a free port with
the sqlite3 D1 fallback (all app/migrations applied), no cron, and outbound
fetch disabled (``FORKMESH_HOST_OFFLINE=1``), then drives real HTTP and
WebSocket routes through it.

WebSocket route used: ``/api/status/ws`` -- the public receive-only status
signal socket. It needs no ticket, crosses the app-ingress Durable Object
(ForkMeshCronRunner, authorization + handoff descriptor) and is accepted by
the hibernating ForkMeshWorld object, which sends ``{"type":"ready"}`` and
answers ``{"type":"ping"}`` with a pong.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

import httpx
import pytest

from forkmesh_host import paths

websockets_sync = pytest.importorskip("websockets.sync.client")


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _vm_rss_kib(pid):
    try:
        with open("/proc/%d/status" % pid, encoding="ascii") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return 0
    return 0


@pytest.fixture(scope="module")
def server(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("host-e2e")
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
        "FORKMESH_HOST_D1_LOG": "1",
        "PYTHONUNBUFFERED": "1",
        "FORKMESH_VAR_DATA_KEY": "e2e-data-key-" + os.urandom(16).hex(),
        "FORKMESH_VAR_ADMIN_PASS": "e2e-admin-" + os.urandom(8).hex(),
        "FORKMESH_VAR_APP_ORIGIN": origin,
        "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin,
        "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        # Cloudflare's always-pass Turnstile test pair, answered in process
        # (turnstile.py); the host runs as FORKMESH_PLATFORM=vps, where the
        # test pair also needs TURNSTILE_TEST_MODE=1.
        "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_SECRET_KEY":
            "1x0000000000000000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
    })
    log = open(tmp / "server.log", "w+")
    process = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--no-cron", "--migrate", "--no-build", "--log-level", "warning"],
        cwd=str(paths.HOST_DIR), env=env, stdout=log,
        stderr=subprocess.STDOUT)
    deadline = time.time() + 60
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
    info = {"origin": origin, "port": port, "pid": process.pid, "env": env,
            "rss_startup_kib": _vm_rss_kib(process.pid), "log": log}
    yield info
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()
    log.close()


@pytest.fixture(scope="module")
def client(server):
    with httpx.Client(base_url=server["origin"], timeout=30,
                      headers={"origin": server["origin"]}) as http:
        yield http


def test_health_and_version(client, server):
    health = client.get("/health")
    assert health.status_code == 200
    assert health.json()["ok"] is True
    version = client.get("/api/version")
    assert version.status_code == 200
    body = version.json()
    assert body["ok"] is True and "accounts" in body["features"]
    assert isinstance(body["now"], int)
    print("serve RSS after startup: %d KiB" % server["rss_startup_kib"])


def test_public_read_routes(client):
    status = client.get("/api/status")
    assert status.status_code == 200
    assert "systems" in status.json()
    for path in ("/api/repositories", "/api/network/overview",
                 "/api/openapi.json", "/api/ledger"):
        response = client.get(path)
        assert response.status_code == 200, (path, response.text[:300])
        json.loads(response.text)


def test_static_assets_routing(client):
    login = client.get("/login")
    assert login.status_code == 200
    assert "text/html" in login.headers["content-type"]
    # "!/login/" in run_worker_first: assets answer, drop-trailing-slash.
    redirect = client.get("/login/")
    assert redirect.status_code == 307
    assert redirect.headers["location"] == "/login"
    # run_worker_first: /api/* never falls through to assets.
    assert client.get("/api/definitely-missing").status_code == 404


def test_signup_login_sessions_logout(client):
    page = client.get("/signup")
    assert page.status_code == 200, page.text[:200]
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    assert "forkmesh_signup_intent" in page.headers.get("set-cookie", "")
    # Turnstile widget + a CSP that lets it load (on this page only).
    assert 'data-sitekey="1x00000000000000000000AA"' in page.text
    assert "challenges.cloudflare.com/turnstile/v0/api.js" in page.text
    csp = page.headers["content-security-policy"]
    assert "script-src 'self' https://challenges.cloudflare.com" in csp
    assert "challenges.cloudflare.com" not in client.get(
        "/api/turnstile").headers.get("content-security-policy", "")
    assert client.get("/api/turnstile").json() == {
        "siteKey": "1x00000000000000000000AA", "enabled": True}
    no_token = client.post("/api/accounts/signup", json={
        "nodeName": "e2euser", field: "e2e@example.com",
        "password": "correct-horse-battery"})
    assert no_token.status_code == 403, no_token.text[:300]
    assert no_token.json()["error"] == "turnstile_required"
    signup = client.post("/api/accounts/signup", json={
        "nodeName": "e2euser", field: "e2e@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert signup.status_code == 201, signup.text[:300]
    assert signup.json()["nodeName"] == "e2euser"

    login = client.post("/api/accounts/login", json={
        "nodeName": "e2euser", "password": "correct-horse-battery"})
    assert login.status_code == 200, login.text[:300]
    token = login.json()["sessionToken"]
    assert "forkmesh_account=" in login.headers.get("set-cookie", "")
    bad = client.post("/api/accounts/login", json={
        "nodeName": "e2euser", "password": "wrong-password"})
    assert bad.status_code in (401, 403)

    auth = {"authorization": "Bearer " + token}
    sessions = client.get("/api/accounts/sessions", headers=auth)
    assert sessions.status_code == 200
    assert any(s["current"] for s in sessions.json()["sessions"])
    profile = client.get("/api/accounts/e2euser", headers=auth)
    assert profile.status_code == 200 and profile.json()["exists"] is True

    logout = client.post("/api/accounts/logout", headers=auth, json={})
    assert logout.status_code == 200
    after = client.get("/api/accounts/sessions", headers=auth)
    assert after.status_code == 401


def test_d1_log_lines_name_the_request_user_and_session(client, server):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    signup = client.post("/api/accounts/signup", json={
        "nodeName": "e2ecaller", field: "caller@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert signup.status_code == 201, signup.text[:300]
    login = client.post("/api/accounts/login", json={
        "nodeName": "e2ecaller", "password": "correct-horse-battery"})
    token = login.json()["sessionToken"]
    session = token.split(".")[1]
    sessions = client.get("/api/accounts/sessions",
                          headers={"authorization": "Bearer " + token})
    assert sessions.status_code == 200
    server["log"].flush()
    server["log"].seek(0)
    lines = [line for line in server["log"].read().splitlines()
             if line.startswith("D1 ")
             and "(for GET /api/accounts/sessions, IP 127.0.0.1" in line]
    assert lines
    named = [line for line in lines
             if ", user e2ecaller, session %s)" % session[:8] in line]
    assert named
    assert all(token.split(".")[2] not in line for line in lines)
    assert all(session not in line for line in lines)


def test_password_reset_request_needs_the_turnstile_token(client):
    refused = client.post("/api/accounts/forgot-password",
                          json={"identifier": "nobody@example.com"})
    assert refused.status_code == 403
    assert refused.json()["error"] == "turnstile_required"
    assert refused.json()["challengeUrl"] == "/forgot-password"
    ok = client.post("/api/accounts/forgot-password", json={
        "identifier": "nobody@example.com",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert ok.status_code == 200 and ok.json() == {"ok": True}


def test_status_websocket_through_durable_objects(server):
    url = "ws://127.0.0.1:%d/api/status/ws" % server["port"]
    with websockets_sync.connect(url, origin=server["origin"],
                                 open_timeout=10) as ws:
        assert json.loads(ws.recv(timeout=10)) == {"type": "ready"}
        ws.send(json.dumps({"type": "ping"}))
        pong = json.loads(ws.recv(timeout=10))
        assert pong["type"] == "pong" and pong["serverTimeMs"] > 0


def test_world_websocket_welcomes_a_guest(server):
    url = "ws://127.0.0.1:%d/api/world/ws" % server["port"]
    with websockets_sync.connect(url, origin=server["origin"],
                                 open_timeout=10) as ws:
        welcome = json.loads(ws.recv(timeout=10))
        assert welcome["type"] == "welcome" and welcome["id"]


def test_websocket_denial_is_an_http_response(server):
    from websockets.exceptions import InvalidStatus
    url = "ws://127.0.0.1:%d/api/version" % server["port"]
    with pytest.raises(InvalidStatus) as caught:
        with websockets_sync.connect(url, origin=server["origin"],
                                     open_timeout=10):
            pass
    assert caught.value.response.status_code == 426


def test_server_log_has_no_tracebacks(client, server):
    client.get("/health")
    server["log"].flush()
    server["log"].seek(0)
    text = server["log"].read()
    assert "Traceback" not in text, text[-4000:]
    print("serve RSS after e2e: %d KiB" % _vm_rss_kib(server["pid"]))


def _stock_git_push(server, tmp_path, username, password, repo="demo"):
    from urllib.parse import quote
    env = dict(server["env"])
    env.update({"GIT_TERMINAL_PROMPT": "0", "GIT_ASKPASS": "/usr/bin/false",
                "GIT_CONFIG_NOSYSTEM": "1"})
    work = tmp_path / ("push-" + os.urandom(4).hex())
    work.mkdir()

    def git(*args):
        return subprocess.run(
            ["git", *args], cwd=str(work), env=env, capture_output=True,
            text=True, timeout=60)

    git("init", "-q", "-b", "main")
    (work / "README.md").write_text("hello\n")
    git("add", "README.md")
    git("-c", "user.name=E2E", "-c", "user.email=e2e@example.com",
        "commit", "-q", "-m", "first")
    remote = "http://%s:%s@127.0.0.1:%d/%s/%s.git" % (
        quote(username, safe=""), quote(password, safe=""), server["port"],
        "gituser", repo)
    return git("push", remote, "main")


def test_stock_git_push_signs_in_with_email_and_password(
        server, client, tmp_path):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    assert client.post("/api/accounts/signup", json={
        "nodeName": "gituser", field: "git@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"}).status_code == 201

    wrong = _stock_git_push(server, tmp_path, "git@example.com", "nope-nope")
    assert wrong.returncode != 0
    assert "Authentication failed" in wrong.stderr, wrong.stderr

    for username in ("git@example.com", "gituser", "Git@Example.com"):
        pushed = _stock_git_push(
            server, tmp_path, username, "correct-horse-battery")
        assert "Authentication failed" not in pushed.stderr, pushed.stderr
        assert "push credential" not in pushed.stderr, pushed.stderr
        assert "could not reach the node that accepts pushes" in (
            pushed.stderr), pushed.stderr


def test_ops_verify_email_unblocks_an_unverified_account(server, client):
    """Next has no outbound mail: `ops verify-email` is the operator's way to
    finish a signup, through the app's own record helpers."""
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    assert client.post("/api/accounts/signup", json={
        "nodeName": "opsuser", field: "ops@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"}).status_code == 201
    token = client.post("/api/accounts/login", json={
        "nodeName": "opsuser", "password": "correct-horse-battery"}
    ).json()["sessionToken"]
    auth = {"authorization": "Bearer " + token}
    blocked = client.post("/api/orgs", headers=auth,
                          json={"name": "opsorg", "displayName": "Ops"})
    assert blocked.status_code == 403
    assert blocked.json()["error"] == "email_not_verified"

    def ops(*args):
        return subprocess.run(
            [sys.executable, "-m", "forkmesh_host", "ops", *args],
            cwd=str(paths.HOST_DIR), env=server["env"], capture_output=True,
            text=True, timeout=120)

    done = ops("verify-email", "opsuser")
    assert done.returncode == 0, done.stderr[-2000:]
    assert '"verified": true' in done.stdout
    again = ops("verify-email", "opsuser")
    assert '"alreadyVerified": true' in again.stdout
    assert ops("verify-email", "nobody-here").returncode == 1
    shown = ops("show-account", "opsuser")
    assert '"emailVerified": true' in shown.stdout
    assert '"serviceAccount": false' in shown.stdout
    marked = ops("service-account", "opsuser")
    assert '"serviceAccount": true' in marked.stdout, marked.stderr[-2000:]
    assert '"alreadyServiceAccount": true' in ops(
        "service-account", "opsuser").stdout
    assert '"serviceAccount": true' in ops("show-account", "opsuser").stdout
    assert ops("service-account", "nobody-here").returncode == 1

    allowed = client.post("/api/orgs", headers=auth,
                          json={"name": "opsorg", "displayName": "Ops"})
    assert allowed.json().get("error") != "email_not_verified", allowed.text

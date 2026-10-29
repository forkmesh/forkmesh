from __future__ import annotations

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

PASSWORD = "correct-horse-battery"
TURNSTILE = "XXXX.DUMMY.TOKEN.XXXX"


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture(scope="module")
def server(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("host-read-only")
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
        "FORKMESH_HOST_DEBUG": "1",
        "PYTHONUNBUFFERED": "1",
        "FORKMESH_VAR_DATA_KEY": "ro-data-key-" + os.urandom(16).hex(),
        "FORKMESH_VAR_ADMIN_PASS": "ro-admin-" + os.urandom(8).hex(),
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
    yield {"origin": origin, "port": port, "log": log}
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


def _signup(client, name, email):
    page = client.get("/signup")
    assert page.status_code == 200, page.text[:200]
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    return client.post("/api/accounts/signup", json={
        "nodeName": name, field: email, "password": PASSWORD,
        "turnstileToken": TURNSTILE})


def _read_only(client, on):
    answer = client.get("/_host/read-only", params={"on": on})
    assert answer.status_code == 200
    return answer.json()


def _assert_read_only_answer(response):
    assert response.status_code == 503, response.text[:300]
    assert response.headers["retry-after"] == "30"
    body = response.json()
    assert body["code"] == "read_only" and body["readOnly"] is True
    assert "read-only while servers recover" in body["error"]


def test_read_only_serves_reads_refuses_writes_and_recovers(client, server):
    assert _signup(client, "rouser", "ro@example.com").status_code == 201
    login = client.post("/api/accounts/login", json={
        "nodeName": "rouser", "password": PASSWORD})
    assert login.status_code == 200, login.text[:300]
    auth = {"authorization": "Bearer " + login.json()["sessionToken"]}
    assert client.get("/readyz").status_code == 200

    assert _read_only(client, "1")["readOnly"] is True
    ready = client.get("/readyz")
    assert ready.status_code == 503
    assert ready.json()["readOnly"] is True
    assert "read_only" in ready.json()["reasons"]

    status = client.get("/api/status")
    assert status.status_code == 200
    assert status.headers["x-forkmesh-read-only"] == "1"
    assert client.get("/api/version").status_code == 200
    sessions = client.get("/api/accounts/sessions", headers=auth)
    assert sessions.status_code == 200, sessions.text[:300]
    assert any(s["current"] for s in sessions.json()["sessions"])
    profile = client.get("/api/accounts/rouser", headers=auth)
    assert profile.status_code == 200 and profile.json()["exists"] is True
    assert client.get("/signup").status_code == 200
    assert client.get("/login").status_code == 200

    _assert_read_only_answer(_signup(client, "rolate", "late@example.com"))
    _assert_read_only_answer(client.post("/api/accounts/logout",
                                         headers=auth, json={}))
    stats = _read_only(client, "1")["stats"]
    assert stats["refused"] >= 2
    assert stats["answered"] >= 2

    assert _read_only(client, "auto")["readOnly"] is False
    assert client.get("/readyz").status_code == 200
    assert "x-forkmesh-read-only" not in client.get("/api/status").headers
    assert client.get("/api/accounts/sessions",
                      headers=auth).status_code == 200
    assert _signup(client, "rolate", "late@example.com").status_code == 201
    assert client.post("/api/accounts/logout", headers=auth,
                       json={}).status_code == 200
    assert client.get("/api/accounts/sessions",
                      headers=auth).status_code == 401


def test_read_only_login_and_verification_link_get_the_answer(client):
    _read_only(client, "1")
    try:
        _assert_read_only_answer(client.post("/api/accounts/login", json={
            "nodeName": "rouser", "password": PASSWORD}))
        link = client.get("/api/accounts/verify-email",
                          params={"token": "x" * 40},
                          headers={"accept": "text/html,*/*"})
        assert link.status_code == 503
        assert link.headers["content-type"].startswith("text/html")
        assert "ForkMesh is read-only" in link.text
        assert client.get("/api/status").status_code == 200
    finally:
        _read_only(client, "auto")


def test_server_log_has_no_tracebacks(client, server):
    del client
    log = server["log"]
    log.flush()
    log.seek(0)
    text = log.read()
    assert "Traceback" not in text, text[-4000:]
    assert "read-only mode ON" in text and "read-only mode OFF" in text

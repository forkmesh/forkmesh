from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import time

import httpx
import pytest

from forkmesh_host import paths

AESGCM = pytest.importorskip(
    "cryptography.hazmat.primitives.ciphers.aead").AESGCM


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture(scope="module")
def server(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("mail-log-e2e")
    assets = tmp / "assets"
    shutil.copytree(paths.APP_DIR / "public", assets)
    (assets / "index.html").write_text("<!doctype html><title>home</title>")
    (assets / "404.html").write_text("<!doctype html><title>404</title>")
    port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    env = sandbox_env(tmp)
    spool = tmp / "mail-spool"
    spool.mkdir()
    env.update({
        "FORKMESH_MAIL_SPOOL": str(spool),
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_ASSETS_DIR": str(assets),
        "FORKMESH_HOST_OFFLINE": "1",
        "FORKMESH_HOST_CRON_MINUTE_MS": "1500",
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
        "FORKMESH_VAR_MAIL_LOG_OWNER": "mailowner",
        "FORKMESH_VAR_MAILTRAP_API_TOKEN": "e2e-mailtrap-" + os.urandom(8).hex(),
        "FORKMESH_VAR_MAIL_TRANSPORT": "mailtrap",
    })
    log = open(tmp / "server.log", "w+")
    process = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--migrate", "--no-build", "--log-level", "warning"],
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
    yield {"origin": origin, "db": tmp / "data" / "d1.sqlite", "log": log,
           "token": env["FORKMESH_VAR_MAILTRAP_API_TOKEN"], "spool": spool}
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()
    log.close()


def _account(client, name):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    created = client.post("/api/accounts/signup", json={
        "nodeName": name, field: name + "@mail-log-e2e.dev",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert created.status_code == 201, created.text
    token = client.post("/api/accounts/login", json={
        "nodeName": name, "password": "correct-horse-battery"}
    ).json()["sessionToken"]
    return {"authorization": "Bearer " + token}


def _open(passphrase, room, envelope):
    salt = hashlib.sha256(("ForkMesh room:" + room).encode()).digest()[:16]
    key = hashlib.pbkdf2_hmac("sha256", passphrase.encode(), salt, 210000, 32)
    sealed = (base64.b64decode(envelope["body"])
              + base64.b64decode(envelope["tag"]))
    return json.loads(AESGCM(key).decrypt(
        base64.b64decode(envelope["nonce"]), sealed, None))


def test_every_send_lands_in_the_owners_private_notices_channel(server):
    with httpx.Client(base_url=server["origin"], timeout=30,
                      headers={"origin": server["origin"]}) as client:
        owner = _account(client, "mailowner")
        snoop = _account(client, "snoop")
        db = sqlite3.connect(server["db"], timeout=30)
        db.execute("UPDATE users SET is_admin=1 WHERE username='snoop'")
        db.commit()
        db.close()

        channel = None
        deadline = time.time() + 90
        while time.time() < deadline and channel is None:
            listed = client.get("/api/chat/channels", headers=owner).json()
            for item in listed.get("channels", []):
                if item.get("name") == "email-log":
                    channel = item
            if channel is None:
                time.sleep(1)
        assert channel is not None, "no #email-log channel appeared"
        assert channel["serverVisible"] is True
        assert channel["readOnly"] is True
        assert channel["visibility"] == "private"

        texts = []
        deadline = time.time() + 60
        while time.time() < deadline:
            history = client.get(
                "/api/chat/channels/%s/history" % channel["id"],
                headers=owner).json()
            texts = [_open(history["passphrase"], history["room"],
                           json.loads(message["body"]))["text"]
                     for message in history["messages"]]
            if "\n".join(texts).count("Verification") >= 2:
                break
            time.sleep(1)
        body = "\n".join(texts)
        assert "Verification" in body
        assert "mailowner@mail-log-e2e.dev" in body
        assert "\n📨 To: snoop@mail-log-e2e.dev\n" in body
        assert "\n🛤️ Route: Mailtrap API" in body
        assert "\n📄 Body: " in body
        assert "FAILED" in body
        assert server["token"] not in body
        assert "http" not in body

        admin_list = client.get("/api/chat/channels", headers=snoop).json()
        assert all(item.get("name") != "email-log"
                   for item in admin_list["channels"])
        for suffix in ("/history", "/room-access", "/members"):
            denied = client.get(
                "/api/chat/channels/%s%s" % (channel["id"], suffix),
                headers=snoop)
            assert denied.status_code == 404, suffix
        closed = client.patch(
            "/api/chat/channels/%s" % channel["id"], headers=owner,
            json={"visibility": "public"})
        assert closed.status_code == 403


def test_a_broken_mail_log_never_fails_the_send(staged_src, tmp_path):
    del staged_src
    import asyncio

    from forkmesh_host.env import HostConfig
    from forkmesh_host.runtime import HostRuntime

    runtime = HostRuntime(HostConfig(data_dir=tmp_path, environ={
        "FORKMESH_VAR_DATA_KEY": "unit-" + os.urandom(8).hex(),
        "FORKMESH_VAR_MAILTRAP_API_TOKEN": "unit-token",
        "FORKMESH_VAR_MAIL_TRANSPORT": "mailtrap",
        "FORKMESH_VAR_MAIL_LOG_OWNER": "mailowner",
    }))
    entry = runtime.entry
    sent = []

    class Response:
        status = 200

        async def text(self):
            return '{"success":true,"message_ids":["m-1"]}'

    async def fake_fetch(url, init, timeout):
        sent.append(url)
        return Response()

    class BrokenLog:
        def note(self, *args, **kwargs):
            raise RuntimeError("log down")

        async def enqueue(self, *args, **kwargs):
            raise RuntimeError("log down")

    original_fetch, original_log = entry.js_fetch_with_timeout, entry.mail_log
    entry.js_fetch_with_timeout = fake_fetch
    entry.mail_log = BrokenLog()
    try:
        accepted = asyncio.run(entry._send_email(
            runtime.env, "someone@mail-log-e2e.dev", "Subject", "Body",
            email_kind="verification"))
    finally:
        entry.js_fetch_with_timeout = original_fetch
        entry.mail_log = original_log
    assert accepted is True
    assert len(sent) == 1


def _email_log_texts(client, headers, until, timeout=90):
    channel = None
    texts = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        if channel is None:
            for item in client.get("/api/chat/channels",
                                   headers=headers).json().get("channels", []):
                if item.get("name") == "email-log":
                    channel = item
        if channel is not None:
            history = client.get(
                "/api/chat/channels/%s/history" % channel["id"],
                headers=headers).json()
            texts = [_open(history["passphrase"], history["room"],
                           json.loads(message["body"]))["text"]
                     for message in history["messages"]]
            if until("\n".join(texts)):
                break
        time.sleep(1)
    return "\n".join(texts)


def test_mail_sent_outside_the_app_on_any_node_lands_in_email_log(server):
    with httpx.Client(base_url=server["origin"], timeout=30,
                      headers={"origin": server["origin"]}) as client:
        login = client.post("/api/accounts/login", json={
            "nodeName": "mailowner", "password": "correct-horse-battery"})
        owner = ({"authorization": "Bearer " + login.json()["sessionToken"]}
                 if login.status_code == 200 else _account(client, "mailowner"))
        event = {"event_id": "smtp:" + "ab" * 20, "event": "delivery",
                 "message_id": "cron-mail@n2.next.forkmesh.com",
                 "email": "ops-person@example.org", "reason": "",
                 "response_code": 250, "dsn": "2.0.0", "node": "n2",
                 "sender": "root@n2.next.forkmesh.com"}
        name = "%020d-%s.json" % (time.time_ns(), "ab" * 20)
        (server["spool"] / name).write_text(json.dumps(event))
        db = sqlite3.connect(server["db"], timeout=30)
        db.execute(
            "INSERT INTO mail_log_node_notes (created_at,node,data) "
            "VALUES (?,?,?)", (int(time.time() * 1000), "r1", json.dumps({
                "kind": "ids_direct", "to": "o***@example.org",
                "subject": "[fmnext-ids lockdown] r1: test",
                "body": '{ "detector": "binaries" }',
                "result": "accepted", "status": 200,
                "from": "no-reply@forkmesh.com"})))
        db.commit()
        db.close()
        body = _email_log_texts(
            client, owner, lambda text: "Not sent by the ForkMesh app" in text
            and "IDS direct mail" in text)
        assert ("📮 Not sent by the ForkMesh app · "
                "📥 delivered · ") in body
        assert ("\n📨 To: ops-person@example.org\n"
                "📤 From: root@n2.next.forkmesh.com\n"
                "🛤️ Route: local mail (Postfix on the core) "
                "on n2\n"
                "🧰 Source: no app send record: a tool, a cron job "
                "or a bounce\n"
                "🆔 Message ID: cron-mail@n2.next.forkmesh.com") in body
        assert "🛰️ IDS direct mail · ✅ accepted · " in body
        assert ("\n📨 To: o***@example.org\n"
                "📤 From: no-reply@forkmesh.com\n"
                "📝 Subject: [fmnext-ids lockdown] r1: test\n"
                "📄 Body: { \"detector\": \"binaries\" }\n"
                "⚠️ Reason: HTTP 200\n"
                "🛤️ Route: Mailtrap API, IDS direct on r1") in body


def test_an_open_of_the_tracked_footer_lands_in_email_log(server):
    with httpx.Client(base_url=server["origin"], timeout=30,
                      headers={"origin": server["origin"]}) as client:
        login = client.post("/api/accounts/login", json={
            "nodeName": "mailowner", "password": "correct-horse-battery"})
        owner = ({"authorization": "Bearer " + login.json()["sessionToken"]}
                 if login.status_code == 200 else _account(client, "mailowner"))
        assert client.get("/api/mail/footer").status_code == 401
        footer = client.get("/api/mail/footer", headers=owner).json()
        assert footer["ok"] is True and footer["opens"] == 0
        assert footer["imageUrl"] in footer["html"]
        assert footer["imageUrl"] not in footer["previewHtml"]
        assert footer == {**client.get(
            "/api/mail/footer", headers=owner).json(),
            "opens": 0, "lastOpenedAt": 0}
        path = footer["imageUrl"][len(server["origin"]):]
        image = client.get(path, follow_redirects=False)
        assert image.status_code == 302
        assert image.headers["location"].endswith("/assets/logo-96.png")
        assert image.headers["cache-control"].startswith("no-store")
        forged = client.get(re.sub(r"\.[0-9a-f]{24}\.png$",
                                   "." + "0" * 24 + ".png", path),
                            follow_redirects=False)
        assert forged.status_code == 302
        assert client.get("/api/mail/footer",
                          headers=owner).json()["opens"] == 1
        body = _email_log_texts(
            client, owner, lambda text: "Own mail (tracked footer)" in text)
        assert ("🖼️ Own mail (tracked footer) · "
                "👀 opened · ") in body
        assert ("🔍 Detail: @mailowner's tracked footer was opened in "
                "an email sent from mailowner@mail-log-e2e.dev") in body

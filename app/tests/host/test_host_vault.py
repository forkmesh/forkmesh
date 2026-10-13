"""End to end: a vault-mode host starts sealed and is unsealed over its socket.

Same harness as test_host_e2e.py (sqlite D1 fallback, all migrations, no
cron, offline), with ``FORKMESH_VAULT=1`` and a socket in the test's temp
dir. The vault rows are written into the sqlite file the way
tools/next_vault.py writes them into rqlite (encrypted with vault_crypto).
"""

from __future__ import annotations

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

from forkmesh_host import paths, vault_crypto
from forkmesh_host.env import HostConfig, collect_vars

DATA_KEY = "vault-e2e-key-" + os.urandom(20).hex()
WRONG_KEY = "vault-e2e-wrong-" + os.urandom(20).hex()
ADMIN_PASS = "vault-e2e-admin-" + os.urandom(8).hex()
LATE_VALUE = "vault-e2e-late-" + os.urandom(8).hex()


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def talk(path, *lines):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(30)
        client.connect(str(path))
        client.sendall("".join(line + "\n" for line in lines).encode())
        client.shutdown(socket.SHUT_WR)
        data = b""
        while not data.endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data)


def upsert(db_path, rows):
    connection = sqlite3.connect(str(db_path), timeout=30)
    try:
        with connection:
            for row in rows:
                connection.execute(
                    "INSERT INTO host_secrets(name, nonce, ciphertext, key_id, "
                    "updated_at) VALUES(?,?,?,?,?) ON CONFLICT(name) DO UPDATE SET "
                    "nonce=excluded.nonce, ciphertext=excluded.ciphertext, "
                    "key_id=excluded.key_id", (row["name"], row["nonce"],
                                               row["ciphertext"], row["key_id"], 1))
    finally:
        connection.close()


@pytest.fixture(scope="module")
def sealed_server(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("host-vault")
    assets = tmp / "assets"
    shutil.copytree(paths.APP_DIR / "public", assets)
    (assets / "index.html").write_text("<!doctype html><title>home</title>")
    (assets / "404.html").write_text("<!doctype html><title>404</title>")
    port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    sock = tmp / "run" / "unseal.sock"
    sock.parent.mkdir()
    # Credentials directory as systemd would populate it: with the vault on,
    # only the bootstrap names may be read from it.
    creds = tmp / "creds"
    creds.mkdir()
    (creds / "DATA_KEY").write_text("cred-file-key-must-be-ignored-" + "z" * 20)
    (creds / "RQLITE_PASSWORD").write_text("bootstrap-pw")
    env = sandbox_env(tmp)
    env.update({
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_ASSETS_DIR": str(assets),
        "FORKMESH_HOST_OFFLINE": "1",
        "FORKMESH_VAULT": "1",
        "FORKMESH_VAULT_SOCKET": str(sock),
        "CREDENTIALS_DIRECTORY": str(creds),
        # Ignored under the vault: DATA_KEY only arrives over the socket.
        "FORKMESH_VAR_DATA_KEY": "env-key-must-be-ignored-" + "w" * 20,
        "FORKMESH_VAR_APP_ORIGIN": origin,
        "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin,
        "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        # Cloudflare's always-pass Turnstile test pair (turnstile.py).
        "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_SECRET_KEY":
            "1x0000000000000000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
    })
    log = open(tmp / "server.log", "w+")
    process = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--no-cron", "--migrate", "--no-build", "--log-level", "warning"],
        cwd=str(paths.HOST_DIR), env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.time() + 60
    while time.time() < deadline:
        if process.poll() is not None:
            log.seek(0)
            pytest.fail("server exited: " + log.read()[-4000:])
        try:
            if httpx.get(origin + "/health", timeout=1).status_code == 200 \
                    and sock.exists():
                break
        except httpx.HTTPError:
            pass
        time.sleep(0.2)
    else:
        process.kill()
        pytest.fail("server did not start")
    key = vault_crypto.VaultKey(DATA_KEY)
    upsert(tmp / "data" / "d1.sqlite", [key.check_row(),
                                        key.encrypt("ADMIN_PASS", ADMIN_PASS)])
    info = {"origin": origin, "port": port, "sock": sock, "log": log,
            "db": tmp / "data" / "d1.sqlite", "pid": process.pid}
    yield info
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()
    log.close()


@pytest.fixture(scope="module")
def client(sealed_server):
    with httpx.Client(base_url=sealed_server["origin"], timeout=30,
                      headers={"origin": sealed_server["origin"]}) as http:
        yield http


def test_socket_is_private(sealed_server):
    assert (sealed_server["sock"].stat().st_mode & 0o777) == 0o600


def test_sealed_answers_503_except_health_and_readyz(client, sealed_server):
    assert talk(sealed_server["sock"], "status")["sealed"] is True
    for path in ("/api/version", "/api/status", "/login", "/"):
        response = client.get(path)
        assert response.status_code == 503, path
        assert response.json() == {"error": "sealed"}
    assert client.post("/api/accounts/login", json={}).status_code == 503
    health = client.get("/health")
    assert health.status_code == 200 and health.json()["sealed"] is True
    ready = client.get("/readyz")
    assert ready.status_code == 503 and "sealed" in ready.json()["reasons"]


def test_sealed_websocket_is_refused(sealed_server):
    from websockets.exceptions import InvalidStatus
    from websockets.sync.client import connect
    url = "ws://127.0.0.1:%d/api/status/ws" % sealed_server["port"]
    with pytest.raises(InvalidStatus) as caught:
        connect(url, origin=sealed_server["origin"], open_timeout=10)
    assert caught.value.response.status_code == 503
    time.sleep(0.5)
    sealed_server["log"].flush()
    sealed_server["log"].seek(0)
    assert "without completing handshake" not in sealed_server["log"].read()


def test_ops_is_refused_while_sealed(sealed_server):
    assert talk(sealed_server["sock"], "ops show-account nobody")["error"] == "sealed"


def test_wrong_key_is_rejected(client, sealed_server):
    reply = talk(sealed_server["sock"], "unseal", WRONG_KEY)
    assert reply == {"ok": False, "error": "wrong_key",
                     "detail": "wrong_key: key_id does not match the vault"}
    assert client.get("/api/version").status_code == 503


def test_unseal_with_the_right_key_serves_the_app(client, sealed_server):
    reply = talk(sealed_server["sock"], "unseal", DATA_KEY)
    assert reply["ok"] is True and reply["sealed"] is False
    assert reply["secrets"] == ["ADMIN_PASS"]
    assert reply["keyId"] == vault_crypto.VaultKey(DATA_KEY).key_id
    assert client.get("/readyz").status_code == 200
    version = client.get("/api/version")
    assert version.status_code == 200 and version.json()["ok"] is True
    # Signup encrypts and blind-indexes under DATA_KEY: the key is live.
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    signup = client.post("/api/accounts/signup", json={
        "nodeName": "vaultuser", field: "vault@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert signup.status_code == 201, signup.text[:300]
    login = client.post("/api/accounts/login", json={
        "nodeName": "vaultuser", "password": "correct-horse-battery"})
    assert login.status_code == 200
    # The same key again is a no-op; ops now run inside the app.
    assert talk(sealed_server["sock"], "unseal", DATA_KEY)["already"] is True
    shown = talk(sealed_server["sock"], "ops show-account vaultuser")
    assert shown["ok"] is True and shown["account"] == "vaultuser"
    assert talk(sealed_server["sock"], "ops drop-tables x")["error"] == \
        "unknown_ops_action"


def test_reload_picks_up_new_rows(sealed_server):
    key = vault_crypto.VaultKey(DATA_KEY)
    upsert(sealed_server["db"], [key.encrypt("PRODUCT_ANALYSIS_USER", LATE_VALUE)])
    reply = talk(sealed_server["sock"], "reload")
    assert reply["ok"] is True
    assert reply["secrets"] == ["ADMIN_PASS", "PRODUCT_ANALYSIS_USER"]


def test_node_helper_script(sealed_server, tmp_path):
    """deploy/core/templates/fmnext-unseal against the live socket."""
    template = (paths.REPO_DIR / "deploy" / "core" / "templates" /
                "fmnext-unseal").read_text()
    helper = tmp_path / "fmnext-unseal"
    helper.write_text(template.replace("@@UNSEAL_SOCKET@@", str(sealed_server["sock"]))
                      .replace("@@NODE@@", "n1"))

    def run(*args, stdin=b""):
        done = subprocess.run([sys.executable, str(helper), *args], input=stdin,
                              capture_output=True, timeout=60)
        return done.returncode, json.loads(done.stdout.decode().strip().splitlines()[-1])
    code, reply = run(stdin=(DATA_KEY + "\n").encode())
    assert code == 0 and reply["already"] is True
    code, reply = run("--status")
    assert code == 0 and reply["sealed"] is False
    code, reply = run("--ops", "show-account", "vaultuser")
    assert code == 0 and reply["account"] == "vaultuser"
    code, reply = run()
    assert code == 2 and reply["error"] == "no_key_on_stdin"
    missing = tmp_path / "other"
    missing.write_text(template.replace("@@UNSEAL_SOCKET@@", str(tmp_path / "nope.sock")))
    done = subprocess.run([sys.executable, str(missing), "--status"],
                          capture_output=True, timeout=60)
    assert done.returncode == 1 and b"no_socket" in done.stdout


def test_seal_drops_back_to_503(client, sealed_server):
    reply = talk(sealed_server["sock"], "seal")
    assert reply["ok"] is True and reply["sealed"] is True and reply["secrets"] == []
    assert client.get("/api/version").status_code == 503
    assert talk(sealed_server["sock"], "reload")["error"] == "sealed"


def test_failed_unseals_are_rate_limited(client, sealed_server):
    # The successful unseal above cleared the earlier failure: five now.
    for _ in range(5):
        assert talk(sealed_server["sock"], "unseal", WRONG_KEY)["error"] == "wrong_key"
    limited = talk(sealed_server["sock"], "unseal", DATA_KEY)
    assert limited["error"] == "rate_limited" and limited["retryAfter"] >= 1
    assert client.get("/api/version").status_code == 503


def test_protocol_errors(sealed_server):
    assert talk(sealed_server["sock"], "bogus")["error"] == "unknown_command"
    assert talk(sealed_server["sock"], "unseal", "x" * 10000)["error"] == \
        "line_too_long"


def test_no_secret_reaches_the_log(sealed_server):
    log = sealed_server["log"]
    log.flush()
    log.seek(0)
    text = log.read()
    for value in (DATA_KEY, WRONG_KEY, ADMIN_PASS, LATE_VALUE):
        assert value not in text
    assert "vault: unsealed (1 secrets" in text
    assert "Traceback" not in text


def test_collect_vars_under_the_vault(tmp_path):
    creds = tmp_path / "creds"
    creds.mkdir()
    for name in ("DATA_KEY", "ADMIN_PASS", "RQLITE_PASSWORD", "FORKMESH_MESH_SECRET"):
        (creds / name).write_text("value-of-" + name)
    environ = {"CREDENTIALS_DIRECTORY": str(creds),
               "FORKMESH_VAR_DATA_KEY": "from-env"}
    plain = collect_vars(HostConfig(environ=dict(environ)))
    assert plain["DATA_KEY"] == "value-of-DATA_KEY"
    assert plain["ADMIN_PASS"] == "value-of-ADMIN_PASS"
    sealed = collect_vars(HostConfig(environ=dict(environ, FORKMESH_VAULT="1")))
    assert "DATA_KEY" not in sealed and "ADMIN_PASS" not in sealed
    assert sealed["RQLITE_PASSWORD"] == "value-of-RQLITE_PASSWORD"
    assert sealed["FORKMESH_MESH_SECRET"] == "value-of-FORKMESH_MESH_SECRET"

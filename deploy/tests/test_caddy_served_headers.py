import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "deploy" / "core"))
import caddy_routes  # noqa: E402

HEADERS = (REPO / "www" / "public" / "_headers").read_text(encoding="utf-8") \
    + "\n/*\n  x-forkmesh-worker: app\n"


def _caddy():
    found = os.environ.get("FORKMESH_CADDY") or shutil.which("caddy")
    return found if found and os.access(found, os.X_OK) else None


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def test_rules_that_replace_a_header_do_not_also_delete_it():
    routes = caddy_routes.app_routes(
        caddy_routes.load_worker_first(REPO / "app" / "wrangler.toml"),
        HEADERS, "/srv/app/dist").lower()
    for index, (_rule, ops) in enumerate(
            caddy_routes.parse_headers_file(HEADERS)):
        for op, name, _value in ops:
            if op == "set":
                assert "header @app_h%d -%s\n" % (index, name.lower()) \
                    not in routes


@pytest.fixture(scope="module")
def served(tmp_path_factory):
    caddy = _caddy()
    if not caddy:
        pytest.skip("caddy is not installed (set FORKMESH_CADDY)")
    tmp = tmp_path_factory.mktemp("caddy")
    dist = tmp / "dist"
    (dist / "office").mkdir(parents=True)
    (dist / "office" / "index.html").write_text("<!doctype html><title>o</title>")
    (dist / "office" / "app.js").write_text("export {};")
    (dist / "login.html").write_text("<!doctype html><title>l</title>")
    (dist / "_headers").write_text(HEADERS)
    routes = caddy_routes.app_routes(
        caddy_routes.load_worker_first(REPO / "app" / "wrangler.toml"),
        HEADERS, str(dist))
    (tmp / "app-routes.caddy").write_text(routes)
    port = _free_port()
    (tmp / "Caddyfile").write_text(
        "{\n\tadmin off\n\tauto_https off\n\tpersist_config off\n}\n"
        "(app_proxy) {\n\trespond \"app\" 200\n}\n"
        "http://127.0.0.1:%d {\n\timport %s\n}\n" % (port, tmp / "app-routes.caddy"))
    env = {**os.environ, "HOME": str(tmp), "XDG_DATA_HOME": str(tmp / "d"),
           "XDG_CONFIG_HOME": str(tmp / "c")}
    process = subprocess.Popen(
        [caddy, "run", "--config", str(tmp / "Caddyfile"), "--adapter", "caddyfile"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    base = "http://127.0.0.1:%d" % port
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            urllib.request.urlopen(base + "/login", timeout=1)
            break
        except OSError:
            time.sleep(0.2)
    yield base
    process.terminate()
    process.wait(10)


def _headers(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return {k.lower(): v for k, v in response.headers.items()}


@pytest.mark.parametrize("path", ["/office", "/office/app.js?v=1"])
def test_office_is_served_with_its_csp_and_media_permissions(served, path):
    headers = _headers(served + path)
    csp = headers.get("content-security-policy", "")
    assert "require-trusted-types-for 'script'" in csp
    assert "trusted-types office" in csp
    assert "script-src 'self';" in csp
    permissions = headers.get("permissions-policy", "")
    for feature in ("camera=(self)", "microphone=(self)",
                    "display-capture=(self)"):
        assert feature in permissions


def test_the_rest_of_the_app_keeps_its_own_policy(served):
    headers = _headers(served + "/login")
    assert "camera=()" in headers.get("permissions-policy", "")
    assert "trusted-types" not in headers.get("content-security-policy", "")
    assert headers.get("content-security-policy")

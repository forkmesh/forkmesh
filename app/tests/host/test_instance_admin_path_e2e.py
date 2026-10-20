from __future__ import annotations

import os
import sqlite3
import shutil
import socket
import subprocess
import sys
import time

import httpx
import pytest

from forkmesh_host import paths

PRIVATE = "/manage-x7k2q9"
NOT_FOUND_PAGE = "<!doctype html><title>instance 404</title>"
LEGACY = ("/admin", "/admin/", "/admin/theme", "/admin/acquisition",
          "/admin/acquisition/", "/admin/hackathon", "/admin/anything/else")


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture(scope="module")
def serve(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    started = []

    def start(name, **variables):
        tmp = tmp_path_factory.mktemp("admin-path-" + name)
        assets = tmp / "assets"
        shutil.copytree(paths.APP_DIR / "public", assets)
        (assets / "404.html").write_text(NOT_FOUND_PAGE)
        port = _free_port()
        origin = "http://127.0.0.1:%d" % port
        env = sandbox_env(tmp)
        env.update({
            "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
            "FORKMESH_HOST_ASSETS_DIR": str(assets),
            "FORKMESH_HOST_OFFLINE": "1",
            "FORKMESH_VAR_DATA_KEY": "admin-path-key-" + os.urandom(16).hex(),
            "FORKMESH_VAR_ADMIN_PASS": "admin-path-" + os.urandom(8).hex(),
            "FORKMESH_VAR_APP_ORIGIN": origin,
            "FORKMESH_VAR_API_ORIGIN": origin,
            "FORKMESH_VAR_WORLD_ORIGIN": origin,
            "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
            "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        })
        for key, value in variables.items():
            env["FORKMESH_VAR_" + key] = value
        log = open(tmp / "server.log", "w+")
        process = subprocess.Popen(
            [sys.executable, "-m", "forkmesh_host", "serve", "--port",
             str(port), "--migrate", "--no-build", "--no-cron",
             "--log-level", "warning"],
            cwd=str(paths.HOST_DIR), env=env, stdout=log,
            stderr=subprocess.STDOUT)
        started.append((process, log))
        deadline = time.time() + 90
        while time.time() < deadline:
            if process.poll() is not None:
                log.seek(0)
                pytest.fail("server exited: " + log.read()[-4000:])
            try:
                if httpx.get(origin + "/health", timeout=1).status_code == 200:
                    client = httpx.Client(base_url=origin,
                                          follow_redirects=False, timeout=30)
                    client.db = tmp / "data" / "d1.sqlite"
                    return client
            except httpx.HTTPError:
                time.sleep(0.2)
        pytest.fail("server did not start")

    yield start
    for process, log in started:
        process.terminate()
        try:
            process.wait(15)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()


@pytest.fixture(scope="module")
def private(serve):
    return serve("private", INSTANCE_NAME="Acme Code", ADMIN_PATH=PRIVATE,
                 INSTANCE_PLUGINS="hackathon")


@pytest.fixture(scope="module")
def unset(serve):
    return serve("unset", INSTANCE_NAME="Acme Code")


@pytest.fixture(scope="module")
def core(serve):
    return serve("core", ADMIN_PATH=PRIVATE)


def _is_console_login(response, path):
    assert response.status_code == 302, (path, response.status_code)
    assert response.headers["location"].startswith("/login?next=")


def test_the_private_path_serves_the_console_and_admin_is_not_found(private):
    console = private.get(PRIVATE)
    _is_console_login(console, PRIVATE)
    assert console.headers["cache-control"] == "no-store"
    assert console.headers["x-robots-tag"] == "noindex, nofollow"
    assert console.headers["referrer-policy"] == "no-referrer"
    _is_console_login(private.get(PRIVATE + "/acquisition"), "acquisition")
    _is_console_login(private.get(PRIVATE + "/hackathon"), "hackathon")
    theme = private.get(PRIVATE + "/theme")
    assert theme.status_code == 405
    assert theme.headers["cache-control"] == "no-store"
    moved = private.post(PRIVATE + "/theme", content="theme=dark&next=/",
                         headers={"content-type":
                                  "application/x-www-form-urlencoded"})
    assert moved.status_code == 303
    for path in LEGACY:
        for method in ("GET", "POST"):
            answer = private.request(method, path)
            assert answer.status_code == 404, (method, path)
            assert answer.text == NOT_FOUND_PAGE, (method, path)
            assert "location" not in answer.headers
            assert PRIVATE not in answer.text


def test_anonymous_visitors_never_see_the_private_path(private):
    for path in ("/api/instance", "/robots.txt", "/", "/login", "/signup",
                 "/does-not-exist", "/admin", "/api/admin/affiliates",
                 "/api/accounts/admin-pending"):
        answer = private.get(path)
        assert PRIVATE not in answer.text, path
        assert PRIVATE[1:] not in answer.text, path
        assert PRIVATE[1:] not in str(answer.headers), path
    details = private.get("/api/instance").json()
    assert not any("admin" in key.lower() for key in details)


def test_an_instance_without_the_setting_is_unchanged(unset):
    for path in ("/admin", "/admin/", PRIVATE, PRIVATE + "/acquisition"):
        answer = unset.get(path)
        assert answer.status_code in (200, 404), path
        assert "location" not in answer.headers, path
    _is_console_login(unset.get("/admin/acquisition"), "acquisition")
    assert unset.get("/admin/theme").status_code == 405
    assert unset.get("/admin/hackathon").status_code == 404


def test_a_switched_off_plugin_page_stays_off_under_the_private_path(serve):
    off = serve("plugin-off", INSTANCE_NAME="Acme Code", ADMIN_PATH=PRIVATE)
    assert off.get(PRIVATE + "/hackathon").status_code == 404
    _is_console_login(off.get(PRIVATE + "/acquisition"), "acquisition")


def test_cores_ignore_the_instance_layout(core):
    _is_console_login(core.get(PRIVATE), PRIVATE)
    _is_console_login(core.get("/admin/acquisition"), "acquisition")
    assert core.get("/admin/theme").status_code == 405
    assert core.get(PRIVATE + "/acquisition").status_code != 302
    console = core.get(PRIVATE)
    assert console.headers.get("referrer-policy") != "no-referrer"


def test_short_repository_addresses_never_capture_the_console(serve):
    short = serve("short", INSTANCE_NAME="Acme Code", ADMIN_PATH=PRIVATE)
    with sqlite3.connect(short.db) as connection:
        connection.execute(
            "INSERT INTO instance_settings (k, doc, updated_at) VALUES "
            "('repo_paths', ?, 0)",
            ('{"canonical":true,"owner":"acme","short":true}',))
    _is_console_login(short.get(PRIVATE), PRIVATE)
    _is_console_login(short.get(PRIVATE + "/acquisition"), "acquisition")
    assert short.get("/admin").status_code == 404
    assert short.get("/admin").text == NOT_FOUND_PAGE

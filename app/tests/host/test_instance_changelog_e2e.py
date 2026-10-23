from __future__ import annotations

import html
import json
import os
import shutil
import socket
import subprocess
import sys
import time

import httpx
import pytest

from forkmesh_host import paths

WWW_PUBLIC = paths.APP_DIR.parent / "www" / "public"


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture(scope="module")
def instance(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    sys.path.insert(0, str(paths.APP_DIR / "tools"))
    import build_site_assets

    tmp = tmp_path_factory.mktemp("instance-changelog")
    assets = tmp / "assets"
    shutil.copytree(paths.APP_DIR / "public", assets)
    for name in build_site_assets.BACKEND_ASSETS:
        if name.endswith(".html"):
            shutil.copyfile(WWW_PUBLIC / name, assets / name)
    port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    env = sandbox_env(tmp)
    env.update({
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_ASSETS_DIR": str(assets),
        "FORKMESH_HOST_OFFLINE": "1",
        "FORKMESH_VAR_DATA_KEY": "changelog-key-" + os.urandom(16).hex(),
        "FORKMESH_VAR_ADMIN_PASS": "changelog-" + os.urandom(8).hex(),
        "FORKMESH_VAR_APP_ORIGIN": origin,
        "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin,
        "FORKMESH_VAR_WWW_ORIGIN": origin,
        "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        "FORKMESH_VAR_INSTANCE_NAME": "Acme Code",
    })
    log = open(tmp / "server.log", "w+")
    process = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--migrate", "--no-build", "--no-cron", "--log-level", "warning"],
        cwd=str(paths.HOST_DIR), env=env, stdout=log,
        stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + 90
        while True:
            if process.poll() is not None:
                log.seek(0)
                pytest.fail("server exited: " + log.read()[-4000:])
            try:
                if httpx.get(origin + "/health", timeout=1).status_code == 200:
                    break
            except httpx.HTTPError:
                pass
            if time.time() > deadline:
                pytest.fail("server did not start")
            time.sleep(0.2)
        with httpx.Client(base_url=origin, follow_redirects=False,
                          timeout=30) as client:
            yield client
    finally:
        process.terminate()
        try:
            process.wait(15)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()


def test_an_instance_serves_its_own_changelog(instance):
    page = instance.get("/changelog")
    assert page.status_code == 200
    assert page.headers["content-type"].startswith("text/html")
    entries = json.loads((paths.APP_DIR.parent / "changelog" / "changelog.json")
                         .read_text(encoding="utf-8"))["entries"]
    assert html.escape(entries[0]["title"], quote=False) in page.text
    assert "Changelog" in page.text
    assert instance.get("/about").status_code == 404
    assert instance.get("/careers").status_code == 404


def test_the_instance_lists_the_changelog_as_on_and_website_as_off(instance):
    info = instance.get("/api/instance").json()
    plugins = {item["id"]: item for item in info["plugins"]}
    assert plugins["changelog"]["enabled"] is True
    assert plugins["changelog"]["scope"] == "instance"
    assert plugins["website"]["enabled"] is False
    assert "/changelog" not in plugins["website"]["paths"]
    sheet = instance.get("/instance-theme.css")
    assert sheet.status_code == 200
    assert '[data-instance-plugin~="website"]' in sheet.text
    assert 'a[href="/changelog"]' not in sheet.text
    assert 'a[href="/about"]' in sheet.text

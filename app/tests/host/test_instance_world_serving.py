from __future__ import annotations

import asyncio
import os
import sys

import pytest

ORIGIN = "https://code.example.org"
INDEX = ("<!doctype html><html><head><title>Acme Code World</title></head>"
         "<body><script type=\"module\" src=\"/world/world.js?v=0123456789ab\">"
         "</script></body></html>")
FILES = {
    "index.html": "<!doctype html><title>home</title>",
    "404.html": "<!doctype html><title>Not found</title>",
    "world/index.html": INDEX,
    "world/world.js": "export const loader = 1;\n",
    "world/world-runtime.js": "export const runtime = 1;\n",
    "world/world.css": "body{margin:0}\n",
    "world/vendor/three-0.184.0/three.module.min.js": "export const T = 1;\n",
    "world/assets/ground.webp": "RIFF0000WEBP",
}


def _assets(root):
    for relative, text in FILES.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


def _runtime(staged_src, tmp, plugins):
    del staged_src
    from forkmesh_host.env import HostConfig
    from forkmesh_host.runtime import HostRuntime
    from forkmesh_host.serve import _migrate

    environ = {
        "FORKMESH_VAR_DATA_KEY": "unit-" + os.urandom(8).hex(),
        "FORKMESH_HOST_OFFLINE": "1",
        "FORKMESH_VAR_INSTANCE_NAME": "Acme Code",
        "FORKMESH_VAR_INSTANCE_PLUGINS": plugins,
    }
    for name in ("APP_ORIGIN", "API_ORIGIN", "WORLD_ORIGIN", "WWW_ORIGIN",
                 "PUBLIC_BASE_URL"):
        environ["FORKMESH_VAR_" + name] = ORIGIN
    host = HostRuntime(HostConfig(data_dir=tmp / "data",
                                  assets_dir=_assets(tmp / "assets"),
                                  environ=environ))
    _migrate(host)
    return host


@pytest.fixture(scope="module")
def world_on(staged_src, tmp_path_factory):
    return _runtime(staged_src, tmp_path_factory.mktemp("world-on"), "world")


@pytest.fixture(scope="module")
def world_off(staged_src, tmp_path_factory):
    return _runtime(staged_src, tmp_path_factory.mktemp("world-off"), "")


@pytest.fixture(autouse=True)
def fresh_plugin_cache():
    yield
    module = sys.modules.get("instance_features")
    if module is not None:
        module.forget()


def _get(runtime, path, method="GET"):
    from forkmesh_host import js as _js

    async def main():
        response = await runtime.fetch(
            _js.Request.new(ORIGIN + path, {"method": method}))
        return (response.status, dict(response.headers.items()),
                str(await response.text()))
    return asyncio.run(main())


def _forget():
    module = sys.modules.get("instance_features")
    if module is not None:
        module.forget()


@pytest.mark.parametrize("path", ["/world/", "/world"])
def test_the_world_document_is_served_by_the_instance(world_on, path):
    status, headers, body = _get(world_on, path)
    assert status == 200, body[:300]
    assert body == INDEX
    assert headers["content-type"].startswith("text/html")
    assert headers["cache-control"] == "no-store, max-age=0, must-revalidate"
    assert "location" not in headers
    policy = headers["content-security-policy"]
    assert "script-src 'self' 'unsafe-inline';" in policy
    assert "cdn.jsdelivr.net" not in policy
    assert "connect-src 'self' https: wss:" in policy
    assert headers["x-content-type-options"] == "nosniff"


def test_world_modules_carry_a_script_type_and_the_shell_policy(world_on):
    status, headers, body = _get(world_on, "/world/world.js")
    assert status == 200 and "loader" in body
    assert headers["content-type"].startswith("text/javascript")
    assert headers["cache-control"] == "no-store, max-age=0, must-revalidate"
    status, headers, _ = _get(world_on, "/world/world.css")
    assert status == 200 and headers["content-type"].startswith("text/css")


def test_versioned_and_vendored_files_are_immutable(world_on):
    status, headers, _ = _get(
        world_on, "/world/world-runtime.js?v=0123456789ab")
    assert status == 200
    assert headers["cache-control"] == "public, max-age=31536000, immutable"
    status, headers, body = _get(
        world_on, "/world/vendor/three-0.184.0/three.module.min.js")
    assert status == 200 and "T = 1" in body
    assert headers["cache-control"] == "public, max-age=31536000, immutable"
    status, headers, _ = _get(world_on, "/world/assets/ground.webp")
    assert status == 200 and headers["content-type"] == "image/webp"


def test_head_answers_without_a_body(world_on):
    status, headers, body = _get(world_on, "/world/", method="HEAD")
    assert status == 200 and body == ""
    assert headers["content-type"].startswith("text/html")


@pytest.mark.parametrize("path", [
    "/world/missing.js", "/world/vendor/", "/world/%2e%2e/index.html",
])
def test_unknown_world_paths_are_not_found(world_on, path):
    status, _, body = _get(world_on, path)
    assert status == 404
    assert "home" not in body


@pytest.mark.parametrize("path", [
    "/world/", "/world", "/world/world.js",
    "/world/vendor/three-0.184.0/three.module.min.js",
])
def test_with_the_plugin_off_the_world_is_not_found(world_off, path):
    _forget()
    status, _, body = _get(world_off, path)
    assert status == 404
    assert "export" not in body and "Acme Code World" not in body


def test_world_api_is_switched_with_the_plugin(world_on, world_off):
    _forget()
    status, _, body = _get(world_off, "/api/world/visitors")
    assert status == 404 and "plugin_disabled" in body
    _forget()
    status, _, body = _get(world_on, "/api/world/visitors")
    assert status == 200, body[:300]


def test_rewards_and_ads_stay_off_inside_an_enabled_world(world_on):
    for path, plugin in (("/api/world/store/catalog", "rewards"),
                         ("/api/world/wallet", "rewards"),
                         ("/api/world/community-ads/placements", "ads")):
        _forget()
        status, _, body = _get(world_on, path)
        assert status == 404 and '"plugin": "%s"' % plugin in body.replace(
            '":"', '": "'), (path, body[:200])


def test_a_saved_plugin_choice_turns_the_world_on(world_off):
    async def save():
        await world_off.env.DB.prepare(
            "INSERT INTO instance_settings (k, doc, updated_at) "
            "VALUES ('plugins', ?, 1) ON CONFLICT(k) DO UPDATE SET "
            "doc=excluded.doc").bind('{"enabled":["world"]}').run()
    asyncio.run(save())
    _forget()
    try:
        status, _, body = _get(world_off, "/world/")
        assert status == 200 and body == INDEX
    finally:
        async def clear():
            await world_off.env.DB.prepare(
                "DELETE FROM instance_settings WHERE k = 'plugins'").run()
        asyncio.run(clear())
        _forget()

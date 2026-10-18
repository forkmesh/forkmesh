import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
for folder in (ROOT / "src", ROOT / "tools"):
    sys.path.insert(0, str(folder))

import apply_instance_brand  # noqa: E402
import build_site_assets  # noqa: E402
import instance_features  # noqa: E402
import static_routes  # noqa: E402

INSTALLER = (REPO / "www" / "public" / "install-instance.sh").read_text(
    encoding="utf-8")
WORLD_INDEX = (
    '<!doctype html>\n<html><head>\n    <title>ForkMesh World</title>\n'
    '    <link rel="canonical" href="https://forkmesh.com/world/" />\n'
    '</head><body><strong>Opening ForkMesh World</strong></body></html>\n')


def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


@pytest.fixture
def staged(tmp_path, monkeypatch):
    app = tmp_path / "app"
    _write(app / "dist" / "index.html", "<!doctype html><title>ForkMesh</title>")
    _write(app / "dist" / "qr.js", "app qr\n")
    _write(app / "dist" / "world" / "stale.js", "old\n")
    world = tmp_path / "world-dist"
    _write(world / "_headers", "/*\n  x-forkmesh-worker: world\n")
    _write(world / "qr.js", "world qr\n")
    _write(world / "assets" / "bot-avatars" / "luna-face.webp", "RIFF")
    _write(world / "world" / "index.html", WORLD_INDEX)
    _write(world / "world" / "world.js", "export {};\n")
    _write(world / "world" / "vendor" / "three-0.184.0" / "three.core.min.js",
           "export const T = 1;\n")
    monkeypatch.setattr(build_site_assets, "APP", app)
    monkeypatch.setattr(build_site_assets, "stage_world", lambda: world)
    return app / "dist"


def test_the_instance_bundle_adds_the_world_to_the_app(staged):
    target = build_site_assets.stage_instance_world()
    assert target == staged / "world"
    assert (staged / "world" / "world.js").is_file()
    assert (staged / "world" / "vendor" / "three-0.184.0"
            / "three.core.min.js").is_file()
    assert not (staged / "world" / "stale.js").exists()
    assert (staged / "assets" / "bot-avatars" / "luna-face.webp").is_file()
    assert (staged / "qr.js").read_text(encoding="utf-8") == "app qr\n"
    assert "x-forkmesh-worker: world" not in (
        (staged / "_headers").read_text(encoding="utf-8")
        if (staged / "_headers").exists() else "")
    index = (staged / "world" / "index.html").read_text(encoding="utf-8")
    assert "canonical" not in index and "<title>ForkMesh World</title>" in index


def test_the_instance_bundle_needs_the_app_first(tmp_path, monkeypatch):
    monkeypatch.setattr(build_site_assets, "APP", tmp_path / "app")
    monkeypatch.setattr(build_site_assets, "stage_world",
                        lambda: pytest.fail("built the World without an app"))
    with pytest.raises(RuntimeError):
        build_site_assets.stage_instance_world()


def test_the_instance_bundle_is_only_built_when_asked(monkeypatch):
    called = []
    for name in ("stage_app", "stage_cutover", "stage_single", "stage_world",
                 "stage_www", "stage_instance_world"):
        monkeypatch.setattr(build_site_assets, name,
                            lambda name=name: called.append(name) or REPO)
    monkeypatch.setattr(build_site_assets, "REPOSITORY", REPO)
    assert build_site_assets.main(["build"]) == 0
    assert "stage_instance_world" not in called


def test_the_brand_renames_the_bundled_world(staged):
    build_site_assets.stage_instance_world()
    apply_instance_brand.apply(staged, "Acme Code")
    index = (staged / "world" / "index.html").read_text(encoding="utf-8")
    assert "<title>Acme Code World</title>" in index
    assert 'data-instance-name="Acme Code"' in index


def test_the_installer_bundles_the_world_before_branding():
    build = INSTALLER.index("-m forkmesh_host build")
    world = INSTALLER.index("build_site_assets.py instance-world")
    brand = INSTALLER.index("apply_instance_brand.py")
    assert build < world < brand


@pytest.mark.parametrize("path, asset", [
    ("/world", "/world/index.html"),
    ("/world/", "/world/index.html"),
    ("/world/world.js", "/world/world.js"),
    ("/world/scene/chunks/a.js", "/world/scene/chunks/a.js"),
    ("/world/../index.html", ""),
    ("/world/%2e%2e/index.html", ""),
    ("/worlds", ""),
    ("/", ""),
])
def test_world_paths_map_to_bundled_files(path, asset):
    assert static_routes.world_asset_path(path) == asset


@pytest.mark.parametrize("path, query, cache", [
    ("/world/", "", static_routes.WORLD_SHELL_CACHE),
    ("/world/world.js", "", static_routes.WORLD_SHELL_CACHE),
    ("/world/world.js", "v=", static_routes.WORLD_SHELL_CACHE),
    ("/world/world-runtime.js", "v=0123456789ab",
     static_routes.WORLD_IMMUTABLE_CACHE),
    ("/world/scene/a.js", "retry=1&v=0123456789ab",
     static_routes.WORLD_IMMUTABLE_CACHE),
    ("/world/vendor/three-0.184.0/three.core.min.js", "",
     static_routes.WORLD_IMMUTABLE_CACHE),
])
def test_world_cache_policy(path, query, cache):
    assert static_routes.world_asset_cache_control(path, query) == cache


def test_on_a_one_origin_instance_the_world_is_served_locally():
    origin = "https://code.example.org"
    assert static_routes.external_site_delivery(
        "/world/", origin, origin, [origin]) == ("local", "")


@pytest.mark.parametrize("path, plugin", [
    ("/world/", "world"),
    ("/api/world/ws", "world"),
    ("/api/world/visitors", "world"),
    ("/office", "world"),
    ("/api/world/store/catalog", "rewards"),
    ("/api/world/wallet", "rewards"),
    ("/api/accounts/central-fund", "rewards"),
    ("/api/accounts/sessions", ""),
    ("/api/world/community-ads/placements", "ads"),
    ("/api/orgs/acme/discord", "discord"),
    ("/dashboard", ""),
])
def test_the_most_specific_plugin_owns_a_path(path, plugin):
    assert instance_features.feature_for(path) == plugin


def test_world_shell_policy_keeps_three_local():
    source = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    block = re.search(r"WORLD_DOCUMENT_SECURITY_HEADERS = \{(.*?)\n\}",
                      source, re.S).group(1)
    assert "script-src 'self' 'unsafe-inline' \"\n        \"https://static.cloudflareinsights.com;" in block
    assert "jsdelivr" not in block

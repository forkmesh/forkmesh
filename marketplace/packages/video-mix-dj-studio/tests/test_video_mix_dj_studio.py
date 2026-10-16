import json
import shutil
import subprocess
from pathlib import Path


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
IOS = REPO / "native" / "ios"
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
CATALOG = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
PAGE = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
VIEW = (PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")


def test_manifest_catalog_and_places_describe_the_cross_platform_studio():
    assert MANIFEST["id"] == "video-mix-dj-studio"
    assert MANIFEST["install"] == {"bundled": True, "default": False, "sync": "account", "places": ["mobile", "desktop", "web"]}
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:video-mix-dj-studio"
    assert MANIFEST["entrypoints"]["ios"] == "builtin:video-mix-dj-studio"
    assert MANIFEST["contributions"][0]["config"]["route"] == "/dashboard/video-mix-dj-studio"
    row = next(item for item in CATALOG["packages"] if item["id"] == MANIFEST["id"])
    assert row == {"id": MANIFEST["id"], "version": MANIFEST["version"], "manifest": "packages/video-mix-dj-studio/manifest.json"}


def test_dashboard_page_has_a_fixed_mobile_two_deck_surface_and_overlay_controls():
    for marker in ("data-vmdj-deck=\"a\"", "data-vmdj-deck=\"b\"", "data-vmdj=\"crossfader\"", "data-vmdj=\"sync\"", "data-vmdj=\"loop-a\"", "data-vmdj=\"capture-a\"", "data-vmdj=\"platter-a\"", "data-vmdj=\"art-a\"", "data-vmdj=\"section-0\"", "data-vmdj=\"save\"", "data-vmdj=\"modal\"", "data-vmdj=\"open-search\"", "data-vmdj-pad=\"kick\"", "data-vmdj-pad=\"riser\""):
        assert marker in VIEW
    assert "overflow:hidden" in VIEW
    assert "orientation:landscape" in VIEW
    assert "vmdj-platter.is-spinning" in VIEW
    assert "playsinline=1" in PAGE
    assert "vmdj-fullscreen" in VIEW
    assert "registerPage(\"video-mix-dj-studio\"" in PAGE


def test_mix_uses_youtube_embed_messages_local_storage_and_public_search_only():
    assert "www.youtube-nocookie.com/embed" in PAGE
    assert '"setVolume"' in PAGE
    assert '"setPlaybackRate"' in PAGE
    assert "AudioContext" in PAGE
    assert "triggerPad" in PAGE
    assert "captureSection" in PAGE
    assert "scratchStart" in PAGE
    assert "localStorage.setItem" in PAGE
    assert "https://api.invidious.io/instances.json" in PAGE
    assert "/api/v1/search" in PAGE
    assert "canonicalUrl" in PAGE
    assert "attribution_link" in PAGE
    assert "XMLHttpRequest" not in PAGE


def test_desktop_link_and_preview_are_packaged():
    link = (PACKAGE / "content" / "desktop" / "VideoMixDjStudioLink.cpp").read_text(encoding="utf-8")
    build = json.loads((PACKAGE / "content" / "desktop" / "build.json").read_text(encoding="utf-8"))
    assert 'QStringLiteral("builtin:video-mix-dj-studio")' in link
    assert 'QStringLiteral("/dashboard/video-mix-dj-studio")' in link
    assert 'QStringLiteral("play")' in link
    assert build == {"schemaVersion": 1, "sources": ["VideoMixDjStudioLink.cpp"], "mocHeaders": []}
    assert (PACKAGE / "preview" / "market.svg").is_file()
    assert (PACKAGE / "preview" / "market.webp").stat().st_size > 1000


def test_ios_opens_the_native_full_screen_inline_media_studio():
    screen = (IOS / "ForkMesh" / "Features" / "Packages" / "VideoMixDjStudioScreen.swift").read_text(encoding="utf-8")
    routes = (IOS / "ForkMeshCore" / "Sources" / "FMPackages" / "PackageRoutes.swift").read_text(encoding="utf-8")
    parser = (IOS / "ForkMeshCore" / "Sources" / "FMApi" / "Navigation" / "RouteParser.swift").read_text(encoding="utf-8")
    assert "allowsInlineMediaPlayback = true" in screen
    assert '.toolbar(.hidden, for: .navigationBar)' in screen
    assert '.toolbar(.hidden, for: .tabBar)' in screen
    assert 'URLQueryItem(name: "presentation", value: "studio")' in screen
    assert 'target.host == "www.youtube-nocookie.com"' in screen
    assert 'case "videoMixDjStudio": return .videoMixDjStudio' in routes
    assert 'case "video-mix-dj-studio": return [.videoMixDjStudio]' in parser


def test_page_is_valid_javascript_when_node_is_available():
    node = shutil.which("node")
    if node:
        subprocess.run([node, "--check", str(PACKAGE / "content" / "web" / "page.js")], check=True)

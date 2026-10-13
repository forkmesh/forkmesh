import importlib.util
import json
import re
from pathlib import Path

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPOSITORY = PACKAGE.parents[2]
WWW = PACKAGE / "content" / "www"


def _inventory_module():
    spec = importlib.util.spec_from_file_location("design_system_inventory", WWW / "inventory.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    destination = tmp_path_factory.mktemp("design")
    target = _inventory_module().build(REPOSITORY, destination)
    return destination, json.loads(target.read_text(encoding="utf-8"))


def test_every_desktop_icon_in_the_resource_file_is_listed_and_copied(built):
    destination, data = built
    qrc = (REPOSITORY / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    octicons = set(re.findall(r'alias="([^"/]+)\.svg">icons/(?:octicons|forkmesh)/', qrc))
    listed = {icon["name"] for icon in data["desktop"]["icons"] if icon["family"] == "octicons"}
    assert octicons and octicons == listed
    assert "palette" in listed
    for icon in data["desktop"]["icons"]:
        assert (destination / icon["file"]).is_file(), icon["file"]


def test_desktop_buttons_cover_the_named_styles_in_both_themes(built):
    _, data = built
    buttons = {(row["name"], row["attributes"]): row for row in data["desktop"]["buttons"]}
    assert ("", "") == (data["desktop"]["buttons"][0]["name"], data["desktop"]["buttons"][0]["attributes"])
    for name in ("primaryButton", "dangerButton", "successButton", "ghostButton"):
        states = buttons[(name, "")]["states"]
        assert states["default"]["dark"] and states["default"]["light"], name
    assert buttons[("primaryButton", "")]["uses"] > 0
    assert len(data["desktop"]["buttons"]) >= 80
    gradient = buttons[("agentsMagicButton", "")]["states"]["default"]["dark"]["background"]
    assert gradient.startswith("linear-gradient(135deg,")


def test_desktop_palette_reads_every_role_in_dark_and_light(built):
    _, data = built
    roles = {row["role"]: row for row in data["desktop"]["roles"]}
    assert roles["Window"] == {"role": "Window", "dark": "#0d1117", "light": "#f6f8fa"}
    assert "Disabled Text" in roles and "PlaceholderText" in roles
    assert len(data["desktop"]["senders"]) == 8
    assert data["desktop"]["baseFontSize"] == 14


def test_web_tokens_pair_dark_and_light_values(built):
    _, data = built
    tokens = {row["name"]: row for row in data["web"]["tokens"]}
    assert tokens["--background"]["dark"] != tokens["--background"]["light"]
    assert {"primary-button", "secondary-button", "kit-btn"} <= set(data["web"]["buttonClasses"])
    assert len(data["web"]["icons"]) >= 100
    script = data["web"]["script"]
    assert (REPOSITORY / "www" / "public" / script.lstrip("/")).is_file()


@pytest.mark.parametrize("platform", ["ios", "android"])
def test_phone_tokens_and_icons_come_from_the_theme_sources(built, platform):
    destination, data = built
    spec = data[platform]
    assert [row["value"] for row in spec["space"]] == [0, 4, 8, 12, 16, 24, 32, 48, 64]
    assert {row["name"] for row in spec["radius"]} == {"sm", "md", "lg", "full"}
    colors = {row["name"]: row["value"] for row in spec["colors"]}
    assert colors["bgBase"] == "#f7f7f7" and colors["darkBgBase"] == "#0b0b0c"
    assert {"FmFilledButton", "FmOutlinedButton", "FmTextButton", "FmToneButton"} <= {
        row["name"] for row in spec["buttons"]}
    assert len(spec["materialIcons"]) >= 100
    assert all(re.fullmatch(r"[0-9a-f]{4,5}", icon["codepoint"]) for icon in spec["materialIcons"])
    assert set(spec["materialGlyphs"]) == {"add", "more_vert"}
    assert (destination / data["materialFont"]).is_file()


def test_ios_lists_its_text_roles_accents_and_symbols(built):
    _, data = built
    ios = data["ios"]
    assert [row["role"] for row in ios["textRoles"]][:2] == ["displayLarge", "displaySmall"]
    assert {row["name"] for row in ios["accents"]} == {"blue", "purple", "teal"}
    assert len(ios["sfSymbols"]) >= 100
    assert all("." in row["name"] or row["name"].isalpha() for row in ios["sfSymbols"])


def test_the_page_follows_the_website_page_conventions():
    page = (WWW / "design.html").read_text(encoding="utf-8")
    scripts = re.findall(r'<script src="([^"]+)"', page)
    assert scripts[0].startswith("/api-client.js")
    assert "/analytics.js" in scripts
    assert '<meta name="color-scheme" content="light dark"' in page
    assert 'href="/site-header.css"' in page and 'src="/site-header.js?v=${version}"' in page
    assert '<div data-forkmesh-header="simple"></div>' in page
    assert '<link rel="canonical" href="https://forkmesh.com/design" />' in page
    assert 'id="theme-toggle"' not in page
    for asset in ("design.css", "design.js"):
        assert f"/design/{asset}?v=${{version}}" in page
    assert "/design/inventory.json?v=${version}" in (WWW / "design.js").read_text(encoding="utf-8")


def test_the_manifest_mounts_the_page_and_the_desktop_link():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    contribution = manifest["contributions"][0]
    assert contribution["socket"] == "www.page"
    assert contribution["config"]["route"] == "/design"
    assert manifest["entrypoints"]["desktop"] == "builtin:design-system"
    assert manifest["install"] == {
        "bundled": True, "default": False, "sync": "account", "installedBefore": "2026-10-11"}
    source = (PACKAGE / "content" / "desktop" / "DesignSystemLink.cpp").read_text(encoding="utf-8")
    assert 'QStringLiteral("builtin:design-system")' in source
    assert 'QStringLiteral("/design")' in source

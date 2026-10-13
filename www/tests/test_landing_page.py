import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WWW = ROOT / "www" / "public"


def read(path):
    return path.read_text(encoding="utf-8")

DARK_PRIMARY_FILL = "linear-gradient(180deg, #ffffff 0%, #e8e8ea 100%)"
LIGHT_PRIMARY_FILL = "linear-gradient(180deg, #2c2c2c 0%, #222222 100%)"
LIGHT_PRIMARY_SHADOW = (
    "inset 0 1px 0 rgba(255, 255, 255, 0.17), 0 1px 2px rgba(0, 0, 0, 0.16)"
)
LIGHT_SECONDARY_FILL = "linear-gradient(180deg, #ffffff 0%, #fcfcfc 100%)"
LIGHT_SECONDARY_BORDER = "#e1e2e3"


def squash(text):
    return re.sub(r"\s+", "", text)


def test_landing_sections_carry_no_green_kicker():
    index = read(WWW / "index.html")
    for gone in (
        "cap-kicker",
        "home-downloads-kicker",
        "Direct downloads",
        ">Distributed Git<",
        "--cap-accent",
    ):
        assert gone not in index, gone


def test_homepage_downloads_show_platform_icons_and_the_visitor_device():
    script = read(WWW / "homepage-downloads.js")
    for key in ("linux", "android", "windows", "macos"):
        assert f"    {key}:\n" in script, key
    assert 'here.textContent = "This device";' in script
    assert "function visitorPlatform()" in script
    assert "function formatSize(bytes)" in script
    assert 'status.dataset.state = "verified";' in script
    assert 'status.dataset.state = "unavailable";' in script
    index = read(WWW / "index.html")
    assert 'data-home-download-status data-state="checking"' in index
    assert index.count(
        'class="home-download is-loading" aria-hidden="true"') == 4
    assert "#downloads .home-download[data-current=\"true\"]" in index


def test_release_metadata_is_fetched_without_credentials():
    script = read(WWW / "homepage-downloads.js")
    fetches = script.count("fetch(")
    assert fetches == 4
    assert "async function earlierAsset(assetName, tag)" in script
    assert "candidate.tag !== `v${version}`" in script
    assert script.count('credentials: "omit"') == fetches
    client = read(WWW / "api-client.js")
    assert 'options.credentials = "include";' in client


def test_landing_buttons_wear_the_textured_recipe():
    landing = squash(read(WWW / "index.html"))
    assert squash(f"--btn-primary-fill: {DARK_PRIMARY_FILL};") in landing
    assert squash(f"--btn-primary-fill: {LIGHT_PRIMARY_FILL};") in landing
    assert squash(f"--btn-primary-shadow: {LIGHT_PRIMARY_SHADOW};") in landing
    assert squash(f"--btn-secondary-fill: {LIGHT_SECONDARY_FILL};") in landing
    assert squash(
        f"--btn-secondary-border: {LIGHT_SECONDARY_BORDER};") in landing
    for selector in (
        ".button-primary{gap:0.4rem;border:1pxsolidvar(--btn-primary-border);",
        ".button-outline{gap:0.4rem;border:1pxsolidvar(--btn-secondary-border);",
        "#support-forkmesh.support-mission-cta{",
        "#ecosystem.eco-button.is-primary{border-color:var(--btn-primary-border);",
        "#pricing.plan.p-btn{",
    ):
        assert selector in landing, selector
    assert "box-shadow:var(--btn-primary-shadow);" in landing
    assert "box-shadow:var(--btn-secondary-shadow);" in landing

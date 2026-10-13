from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest


MARKETPLACE = Path(__file__).resolve().parents[1]
TOOLS = MARKETPLACE / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from marketplace_lib import (  # noqa: E402
    MarketplaceError,
    load_catalog,
    load_guides,
    load_onboarding,
    load_packs,
)
import generate_desktop_packages  # noqa: E402


CATALOG = load_catalog(MARKETPLACE / "catalog.json")
PACKAGE_IDS = [entry.package_id for entry in CATALOG.packages]


def _write(tmp_path: Path, name: str, payload: dict) -> Path:
    path = tmp_path / name
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def _pack(**overrides) -> dict:
    pack = {
        "id": "starter",
        "name": "Starter",
        "tagline": "A few things.",
        "summary": "Some packages.",
        "packages": ["chat"],
    }
    pack.update(overrides)
    return pack


def _guide(**overrides) -> dict:
    guide = {
        "id": "somewhere",
        "title": "Move from somewhere",
        "from": "Somewhere",
        "summary": "Bring it over.",
        "pack": "starter",
        "steps": ["One.", "Two."],
    }
    guide.update(overrides)
    return guide


def test_shipped_packs_and_guides_validate_against_the_catalog():
    packs, guides = load_onboarding(CATALOG)
    by_id = {pack["id"]: pack for pack in packs}
    assert by_id["whole-cutlery-drawer"]["packages"] == "*"
    assert "chat" in by_id["chat-essentials"]["packages"]
    assert {guide["id"] for guide in guides} >= {
        "cloudflare-pages-github", "vercel-github", "netlify-github",
        "aws-s3-cloudfront", "github-pages", "fly-io-app",
    }
    assert all(guide["pack"] in by_id for guide in guides)


def test_desktop_resources_carry_the_packs_and_guides():
    qrc, _, _ = generate_desktop_packages.generate(MARKETPLACE / "catalog.json")
    assert 'alias="packs.json"' in qrc
    assert 'alias="guides.json"' in qrc


def test_a_pack_naming_a_missing_package_is_refused(tmp_path):
    path = _write(tmp_path, "packs.json", {
        "schemaVersion": 1, "packs": [_pack(packages=["chat", "no-such-package"])]})
    with pytest.raises(MarketplaceError, match="missing from the catalog"):
        load_packs(path, PACKAGE_IDS)


def test_a_pack_id_may_not_shadow_a_package(tmp_path):
    path = _write(tmp_path, "packs.json", {
        "schemaVersion": 1, "packs": [_pack(id="chat")]})
    with pytest.raises(MarketplaceError, match="already taken"):
        load_packs(path, PACKAGE_IDS)


def test_a_guide_must_name_a_known_pack_and_https_links(tmp_path):
    path = _write(tmp_path, "guides.json", {
        "schemaVersion": 1, "guides": [_guide(pack="missing")]})
    with pytest.raises(MarketplaceError, match="unknown pack"):
        load_guides(path, PACKAGE_IDS, ["starter"])
    path = _write(tmp_path, "guides.json", {
        "schemaVersion": 1,
        "guides": [_guide(links=[{"label": "Docs", "url": "http://example.com"}])]})
    with pytest.raises(MarketplaceError, match="https"):
        load_guides(path, PACKAGE_IDS, ["starter"])


def test_a_guide_needs_at_least_two_steps(tmp_path):
    path = _write(tmp_path, "guides.json", {
        "schemaVersion": 1, "guides": [_guide(steps=["Only one."])]})
    with pytest.raises(MarketplaceError, match="2-12 steps"):
        load_guides(path, PACKAGE_IDS, ["starter"])

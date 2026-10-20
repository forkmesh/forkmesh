import json
from pathlib import Path
import sys

import pytest

MARKETPLACE = Path(__file__).resolve().parents[1]
REPOSITORY = MARKETPLACE.parent
sys.path.insert(0, str(MARKETPLACE / "tools"))

import generate_desktop_packages  # noqa: E402
from marketplace_lib import (  # noqa: E402
    MarketplaceError, load_catalog, plugin_ids, plugin_selection,
    selected_catalog_document, selected_onboarding_documents,
)

CATALOG = load_catalog(MARKETPLACE / "catalog.json")
CORE_LINKED = {"capture", "discord", "ledger", "timers", "tour"}


def test_plugins_json_lists_known_plugins_once_and_sorted():
    listed = json.loads((MARKETPLACE / "plugins.json").read_text())["plugins"]
    assert listed == sorted(set(listed))
    assert set(listed) <= set(plugin_ids(CATALOG))
    assert plugin_selection(CATALOG) == frozenset(listed)


def test_a_switch_is_never_a_plugin():
    plugins = set(plugin_ids(CATALOG))
    for entry in CATALOG.packages:
        has_content = (MARKETPLACE / entry.manifest).parent.joinpath("content").is_dir()
        assert (entry.package_id in plugins) == has_content
    with pytest.raises(MarketplaceError, match="core feature switch"):
        plugin_selection(CATALOG, "chat")


def test_selection_keywords_and_lists():
    assert plugin_selection(CATALOG, "none") == frozenset()
    assert plugin_selection(CATALOG, "all") == frozenset(plugin_ids(CATALOG))
    assert plugin_selection(CATALOG, "crm;water, vault") == {"crm", "water", "vault"}
    with pytest.raises(MarketplaceError, match="unknown package"):
        plugin_selection(CATALOG, "no-such-plugin")


def test_a_plugin_needs_its_plugin_dependencies_selected():
    with pytest.raises(MarketplaceError, match="needs plugin status-page"):
        plugin_selection(CATALOG, "status-check-api")
    assert plugin_selection(CATALOG, "status-page;status-check-api") == {
        "status-page", "status-check-api"}


def test_excluding_a_plugin_also_removes_the_plugins_that_need_it():
    selected = plugin_selection(CATALOG, "all", "status-page")
    assert "status-page" not in selected
    assert not any(package_id.startswith("status-check-") for package_id in selected)
    assert plugin_selection(CATALOG, "crm", "a-package-since-removed") == {"crm"}


def test_a_core_only_catalog_keeps_switches_and_drops_plugins():
    catalog = selected_catalog_document(CATALOG, frozenset())
    kept = {item["id"] for item in catalog["packages"]}
    assert "chat" in kept and "crm" not in kept
    assert kept == set(item.package_id for item in CATALOG.packages) - set(plugin_ids(CATALOG))
    packs, guides = selected_onboarding_documents(CATALOG, frozenset())
    for pack in packs["packs"]:
        assert pack["packages"] == "*" or set(pack["packages"]) <= kept
    for guide in (guides or {}).get("guides", []):
        assert set(guide.get("packages", [])) <= kept


def test_the_desktop_core_links_only_the_known_plugins():
    assert set(generate_desktop_packages.core_linked_plugins(MARKETPLACE / "catalog.json")) == CORE_LINKED


def test_a_core_only_desktop_build_carries_no_other_plugin(tmp_path):
    qrc, cmake, documents, plugin_qrcs = generate_desktop_packages.generate(
        MARKETPLACE / "catalog.json", "none", tmp_path)
    kept = {item["id"] for item in json.loads(documents["catalog.json"])["packages"]}
    assert kept & set(plugin_ids(CATALOG)) == CORE_LINKED
    assert plugin_qrcs == {}
    assert "packages/crm/" not in qrc and "packages/winamp/" not in qrc
    assert "FORKMESH_PACKAGE_CRM_SOURCES" not in cmake


def test_module_plugins_carry_their_own_content():
    qrc, cmake, _, plugin_qrcs = generate_desktop_packages.generate(
        MARKETPLACE / "catalog.json", "crm", REPOSITORY / "unused")
    assert 'alias="packages/crm/manifest.json"' in qrc
    assert "packages/crm/content/" not in qrc
    assert "packages/crm/content/" in plugin_qrcs["crm"]
    assert "FORKMESH_PACKAGE_CRM_QRC" in cmake

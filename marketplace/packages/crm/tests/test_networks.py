import re
from pathlib import Path


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
NETWORKS = [
    ("mastodon", "Mastodon"), ("bluesky", "Bluesky"), ("twitter", "X (Twitter)"),
    ("linkedin", "LinkedIn"), ("github", "GitHub"),
]


def _block(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


def test_web_and_desktop_offer_the_same_networks_in_the_same_order():
    web = _block(WEB_SCRIPT, "const CRM_NETWORKS = [", "];")
    assert re.findall(r'\{ id: "([a-z]+)", name: "([^"]+)" \}', web) == NETWORKS
    desktop = _block(FEATURE, "constexpr CrmNetwork kNetworks[] = {", "};")
    assert re.findall(r'\{"([a-z]+)", "([^"]+)"\}', desktop) == NETWORKS


def test_a_network_holds_everyone_whose_profile_field_is_filled_in():
    assert 'return Boolean(String(contact?.[network] || "").trim());' in WEB_SCRIPT
    membership = _block(FEATURE, "bool CrmFeature::contactInSheet(", "\n}\n")
    assert membership.index("if (networkSheet(sheet))") < membership.index('contact.value(QStringLiteral("views"))')
    assert 'return !text(contact, text(sheet, "network")).isEmpty();' in membership


def test_a_network_tab_is_client_side_and_never_acts_as_a_list():
    assert "if (sheet.isEmpty() || networkSheet(sheet))\n        return;" in _block(
        FEATURE, "void CrmFeature::showSheetTabMenu(", "\nvoid CrmFeature::")
    for function in ("void CrmFeature::renameSheet(", "void CrmFeature::addSheetMembers("):
        assert "!sheetIsList(" in _block(FEATURE, function, "\nvoid CrmFeature::")
    assert "if (sheetIsList(sheetById(id)))\n            renameSheet(id);" in FEATURE
    assert "if (sheetIsList(sheet) || sheetContactCount(sheet) > 0)" in FEATURE
    assert 'QStringLiteral("Networks")' in FEATURE
    assert "if (view.network) return crmOnNetwork(contact, view.network);" in WEB_SCRIPT
    assert "const network = event.target.closest(\"[data-crm-network-tab]\");" in WEB_SCRIPT


def test_opening_a_network_shows_every_stage():
    first = _block(FEATURE, "void CrmFeature::selectFirstStage()", "\n}\n")
    assert first.index("networkSheet(sheetById(sheetId))") < first.index("stripStages(nullptr)")
    web = _block(WEB_SCRIPT, "function selectCrmFirstStage()", "\n  }\n")
    assert web.index("if (view.network)") < web.index("crmStripStages(")

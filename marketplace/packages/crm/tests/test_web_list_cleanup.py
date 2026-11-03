from pathlib import Path
import os
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
NODE = shutil.which("node")


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_list_cleanup_finds_sorts_and_moves_people_when_run():
    result = subprocess.run(
        [NODE, "--no-warnings", str(CRM_PACKAGE / "tests" / "list_cleanup_checks.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"), "TZ": "UTC"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "list cleanup checks passed" in result.stdout


def test_list_cleanup_is_a_more_page_with_its_own_panel():
    sections = WEB_VIEW.split("<div data-crm-sections", 1)[1].split("</div>", 1)[0]
    tabs = [chunk.split('"', 1)[0] for chunk in sections.split('data-crm-section-tab="')[1:]]
    assert tabs == ["contacts", "inbox", "outreach", "team", "cleanup", "zoho"]
    assert 'const CRM_SECTIONS = ["contacts", "inbox", "outreach", "team", "cleanup", "zoho"];' in WEB_SCRIPT
    panel = WEB_VIEW.split('<div data-crm-section="cleanup"', 1)[1].split('<div data-crm-section="zoho"', 1)[0]
    for hook in ("data-crm-cleanup-search", "data-crm-cleanup-list", "data-crm-cleanup-keep-all",
                 "data-crm-cleanup-head", "data-crm-cleanup-rows", "data-crm-cleanup-empty"):
        assert hook in panel, hook
    assert "bg-cta" not in panel


def test_list_cleanup_moves_through_the_merge_and_bulk_endpoints():
    apply = WEB_SCRIPT.split("async function crmCleanupApply(plan) {", 1)[1].split("\n  }\n", 1)[0]
    assert '"/contacts/merge"' in apply and '"/contacts/bulk"' in apply
    assert apply.index('"/contacts/merge"') < apply.index('"/contacts/bulk"')
    assert "renderCrmCleanup();" in WEB_SCRIPT.split("function renderCrmSections() {", 1)[1].split("\n  }\n", 1)[0]


DESKTOP = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")


def test_the_desktop_offers_the_same_list_cleanup():
    more = DESKTOP.split("const QList<QToolButton *> moreButtons{", 1)[1].split("};", 1)[0]
    assert "m_cleanupButton" in more
    assert "connect(m_cleanupButton, &QToolButton::clicked, this, [this] { showListCleanup(); });" in DESKTOP
    keep = DESKTOP.split("void CrmFeature::keepPeopleOnList(", 1)[1].split("\n}\n", 1)[0]
    assert 'QStringLiteral("/contacts/merge")' in keep and 'QStringLiteral("/contacts/bulk")' in keep
    assert "if (m_cleanupBusy || !m_pagedSummary.isEmpty())" in keep
    fill = DESKTOP.split("void CrmFeature::fillListCleanup()", 1)[1].split("\n}\n", 1)[0]
    assert "keepPeopleOnList({personId}, id); },\n                    Qt::QueuedConnection);" in fill
    snapshot = DESKTOP.split("void CrmFeature::refreshContactSnapshot()", 1)[1].split("\n}\n", 1)[0]
    assert "fillListCleanup();" in snapshot

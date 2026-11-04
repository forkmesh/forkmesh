from pathlib import Path
import os
import re
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
NODE = shutil.which("node")


def _function(name):
    return WEB_SCRIPT.split(f"function {name}(", 1)[1].split("\n  }\n", 1)[0]


def _panel():
    return WEB_VIEW.split('<div data-crm-section="mastodon"', 1)[1].split("<div data-crm-section=", 1)[0]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_mastodon_page_and_leaderboard_strip_hold_up_when_run():
    result = subprocess.run(
        [NODE, "--no-warnings", str(CRM_PACKAGE / "tests" / "mastodon_page_checks.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"), "TZ": "UTC"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "mastodon page checks passed" in result.stdout


def test_mastodon_is_a_more_page_after_outreach():
    sections = WEB_VIEW.split("<div data-crm-sections", 1)[1].split("</div>", 1)[0]
    tab = re.search(r'<button data-crm-section-tab="mastodon" id="crm-section-tab-mastodon" type="button" role="tab" aria-controls="crm-section-mastodon" aria-selected="false" tabindex="-1"[^>]*class="crm-side-item">', sections)
    assert tab
    assert sections.index('data-crm-section-tab="outreach"') < sections.index('data-crm-section-tab="mastodon"') < sections.index('data-crm-section-tab="team"')
    assert '<div data-crm-section="mastodon" id="crm-section-mastodon" role="tabpanel" aria-labelledby="crm-section-tab-mastodon" hidden>' in WEB_VIEW
    keys = WEB_SCRIPT.split("sections?.addEventListener(\"keydown\"", 1)[1].split("\n    });", 1)[0]
    assert 'CRM_SECTIONS.filter((section) => section !== "contacts")' in keys


def test_the_page_reads_its_own_route_when_opened():
    sections = _function("renderCrmSections")
    assert 'current === "mastodon"' in sections and "void loadCrmMastodon();" in sections
    assert 'if (next === "mastodon") crmMastodon.stale = true;' in _function("selectCrmSection")
    load = _function("loadCrmMastodon")
    assert 'crmOrganizationPath(`/mastodon?${query}`)' in load
    assert "tz: String(-new Date().getTimezoneOffset())" in load
    assert "error.status === 404" in load
    assert "wireCrmMastodon();" in WEB_SCRIPT


def test_the_mastodon_panel_uses_the_dashboard_roles():
    panel = _panel()
    for hook in ("data-crm-mastodon-stats", "data-crm-mastodon-prepare", "data-crm-mastodon-unavailable",
                 "data-crm-mastodon-queue", "data-crm-mastodon-more", "data-crm-mastodon-strategy"):
        assert hook in panel, hook
    card = _function("crmMastodonCardHtml")
    assert "bg-cta" not in panel and "bg-cta" not in card
    assert card.count("fm-btn fm-btn-sm") >= 3
    assert "text-control" in card
    assert "rounded-lg" not in card and "border-border bg-card" not in card
    assert 'target="_blank" rel="noopener noreferrer"' in card


def test_the_leaderboard_strip_sits_under_the_header_and_opens_the_team_page():
    after = WEB_VIEW.split("</header>", 1)[1]
    assert after.lstrip().startswith("<button data-crm-leaderboard hidden")
    strip = re.search(r"<button data-crm-leaderboard[^>]*>", after).group(0)
    assert "text-xs text-muted-foreground" in strip
    assert 'const CRM_LEADERBOARD_KINDS = ["emails", "outreach", "toots"];' in WEB_SCRIPT
    assert "renderCrmLeaderboardStrip();" in _function("renderCrm")
    assert 'selectCrmSection("team")' in _function("openCrmLeaderboard")
    assert '$(`[data-crm-leaderboard]`)?.addEventListener("click", openCrmLeaderboard);' in WEB_SCRIPT

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import badges  # noqa: E402
import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402

SOURCE = ROOT / "public" / "dashboard" / "js" / "09-achievements.js"
VIEW = ROOT / "public" / "dashboard" / "partials" / "views" / "achievements.html"
SIDEBAR = ROOT / "public" / "dashboard" / "partials" / "sidebar.html"


def test_the_page_is_registered_everywhere_a_dashboard_page_must_be():
    page = dashboard_shell.PAGES["achievements"]
    assert page["route"] == "/dashboard/achievements"
    assert page["asset"] == "dashboard/achievements/index.html"
    assert dashboard_shell.CHUNK_PAGES["achievements"] == "achievements"
    assert "dashboard-page-achievements.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    assert dashboard_bundle.LAZY_CHUNKS["achievements"] == "09-achievements.js"
    assert "09-achievements.js" in dashboard_bundle.FRAGMENTS
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/achievements"] == (
        "dashboard/achievements/index.html")
    assert "/dashboard/achievements/index.html" in (
        static_routes.BLOCKED_STATIC_HTML_PATHS)
    boot = (ROOT / "public" / "dashboard" / "js" / "10-boot.js").read_text()
    assert '"achievements": initAchievementsPage' in boot
    built = ROOT / "public" / "dashboard" / "achievements" / "index.html"
    assert 'data-view="achievements"' in built.read_text()


def test_the_sidebar_links_achievements_right_after_profile():
    sidebar = SIDEBAR.read_text()
    profile = sidebar.index('data-nav="profile"')
    achievements = sidebar.index('data-nav="achievements"')
    repos = sidebar.index('data-nav="repos"')
    assert profile < achievements < repos


def test_the_view_has_a_plain_header_and_separate_earned_and_locked_lists():
    view = VIEW.read_text()
    assert '<h1 class="text-base font-semibold text-foreground">Achievements</h1>' in view
    assert "data-achievements-earned" in view and "data-achievements-locked" in view
    assert "cta" not in view


def test_event_only_badges_stay_hidden_until_earned():
    catalog = {entry["slug"]: entry for entry in badges.public_catalog()}
    for slug in ("first_100_users", "hackathon_builder", "hackathon_winner"):
        assert catalog[slug]["hiddenUntilEarned"] is True
    assert "hiddenUntilEarned" not in catalog["stargazer"]


def _functions(names):
    text = SOURCE.read_text()
    out = []
    for name in names:
        match = re.search(r"  function %s\(.*?\n  }\n" % name, text, re.S)
        assert match, name
        out.append(match.group(0))
    return "\n".join(out)


@pytest.mark.skipif(shutil.which("node") is None, reason="Node.js is unavailable")
def test_entries_split_earned_and_locked_and_sort_locked_by_closeness():
    script = _functions(["achievementsShare", "achievementsEntries"]) + """
const catalog = [
  {slug: "first_100_users", hiddenUntilEarned: true},
  {slug: "qa"},
  {slug: "stargazer", unit: "stars", tiers: [{tier: "default", threshold: 16}]},
  {slug: "fan_club", unit: "followers", tiers: [{tier: "default", threshold: 10}]},
  {slug: "coder"},
];
const mine = [
  {slug: "coder", awardedAt: 5},
  {slug: "qa", awardedAt: 9},
];
const progress = [{slug: "fan_club", progress: 8, nextThreshold: 10, unit: "followers"},
                  {slug: "stargazer", progress: 4, nextThreshold: 16, unit: "stars"}];
const out = achievementsEntries(catalog, mine, progress);
console.log(JSON.stringify({
  earned: out.earned.map((item) => item.slug),
  locked: out.locked.map((item) => [item.slug, item.progress, item.nextThreshold]),
}));
"""
    completed = subprocess.run(
        ["node", "-e", script], capture_output=True, text=True, check=True,
        timeout=60)
    assert json.loads(completed.stdout) == {
        "earned": ["qa", "coder"],
        "locked": [["fan_club", 8, 10], ["stargazer", 4, 16]],
    }


def test_locked_tiles_are_greyed_with_a_lock_and_show_first_level_progress():
    text = SOURCE.read_text()
    tile = _functions(["achievementsTile"])
    assert 'medal.style.filter = "grayscale(1)"' in tile
    assert 'data-lucide="lock"' in tile
    assert '"First level"' in tile
    assert 'fetchJson("/api/badges")' in text
    assert "bg-primary" in text and "bg-cta" not in text and "text-success" not in text

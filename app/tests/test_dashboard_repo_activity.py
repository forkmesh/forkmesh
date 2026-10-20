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

JS = ROOT / "public" / "dashboard" / "js"
SOURCE = JS / "09-repo-activity.js"
COMPOSE = JS / "07-repo-compose-branch.js"
DETAIL = JS / "08-repo-detail-network.js"
NODE = shutil.which("node")


def test_the_activity_view_is_a_lazy_chunk_the_insights_tab_loads():
    assert dashboard_bundle.LAZY_CHUNKS["repo-activity"] == "09-repo-activity.js"
    assert "09-repo-activity.js" in dashboard_bundle.FRAGMENTS
    assert "dashboard-page-repo-activity.js" in (
        dashboard_shell.CACHE_BUSTED_ASSETS)
    built = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert 'loadDashboardChunk("repo-activity")' in built
    assert "function loadRepoActivity(...args)" in built
    assert (ROOT / "public" / "dashboard-page-repo-activity.js").is_file()
    compose = COMPOSE.read_text(encoding="utf-8")
    assert re.search(
        r"function loadRepoInsights\(repo\) \{\n\s+return loadRepoActivity\(repo\);",
        compose)


def test_the_insights_panel_is_not_a_box_around_boxes():
    detail = DETAIL.read_text(encoding="utf-8")
    assert ('<section data-dashboard-repo-tab-panel="insights" class="hidden">'
            '<div data-repo-insights class="min-w-0"></div></section>') in detail
    assert "repoInsightsRouteQuery()" in detail
    source = SOURCE.read_text(encoding="utf-8")
    assert "data-repo-insights-refresh" in source
    assert 'role="tooltip"' in source and "aria-describedby" in source
    assert 'role="progressbar"' in source
    assert "aria-current" in source and "aria-expanded" in source


def test_the_view_has_no_inline_handlers_and_keeps_the_colour_roles():
    source = SOURCE.read_text(encoding="utf-8")
    assert not re.search(r"\son[a-z]+\s*=", source)
    assert "innerHTML = data" not in source
    assert "bg-cta" not in source and "text-cta" not in source
    assert "prefers-reduced-motion" in source
    for token in ("--contribution-pulls", "--dashboard-open"):
        assert token in source


def test_milestones_reuse_platform_badges_that_exist():
    source = SOURCE.read_text(encoding="utf-8")
    slugs = set(re.findall(r'badge: "([a-z_]+)"', source))
    catalog = {entry["slug"]: entry for entry in badges.public_catalog()}
    assert slugs == {"coder", "web_weaver", "six_eyes", "bug_catcher",
                     "daily_spinner"}
    for slug in slugs - {"coder"}:
        assert catalog[slug]["tiers"], slug
    assert 'fetchJson("/api/badges")' in source
    assert "profileBadgeMedal(" in source


def _functions(path, names):
    text = path.read_text(encoding="utf-8")
    out = []
    for name in names:
        match = re.search(r"  (?:async )?function %s\(.*?\n  }\n" % name,
                          text, re.S)
        assert match, name
        out.append(match.group(0))
    return "\n".join(out)


def _constants():
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("  const ACTIVITY_VIEWS")
    end = text.index("  const ACTIVITY_STYLE_ID")
    return text[start:end] + text[text.index("  const activityView = {"):text.index("  function activityEnsureStyle")]


HELPERS = r"""
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;"})[c]);
const formatCount = (value) => String(Number(value) || 0);
const repoLinkUrl = (repo) => `/${repo.owner}/${repo.name}`;
const repoRecordUrl = (repo, kind, number) => `/${repo.owner}/${repo.name}/${kind}/${number}`;
const relativeTimeLabel = () => "1 day ago";
const platformAccount = () => false;
const platformName = () => "Platform";
const userNameHtml = (name) => escapeHtml(name);
const repoContributorAvatar = (person) => `<span data-avatar="${escapeHtml(person.name)}"></span>`;
let location = { search: "" };
"""


def _run(script):
    completed = subprocess.run(["node", "-e", script], capture_output=True,
                               text=True, check=True, timeout=60)
    return json.loads(completed.stdout)


@pytest.mark.skipif(NODE is None, reason="Node.js is unavailable")
def test_url_state_round_trips_and_ignores_unknown_values():
    script = HELPERS + _constants() + _functions(SOURCE, [
        "activityReadLocation", "activityHref"]) + """
const repo = {owner: "acme", name: "space"};
const out = [];
for (const search of ["", "?view=leaderboard&period=1m&scope=all", "?view=nope&period=9y&scope=x"]) {
  location.search = search;
  const read = activityReadLocation();
  out.push([read, activityHref(repo, read.view, read.period, read.scope)]);
}
console.log(JSON.stringify(out));
"""
    assert _run(script) == [
        [{"view": "pulse", "period": "1w", "scope": "period"},
         "/acme/space/insights"],
        [{"view": "leaderboard", "period": "1m", "scope": "all"},
         "/acme/space/insights?view=leaderboard&period=1m&scope=all"],
        [{"view": "pulse", "period": "1w", "scope": "period"},
         "/acme/space/insights"],
    ]
    compose = _functions(COMPOSE, ["repoInsightsRouteQuery"])
    script = "let location = {search: '?view=commits&period=3d&viewer=x&scope=<b>'};\n" + compose + """
console.log(JSON.stringify(repoInsightsRouteQuery()));
"""
    assert _run(script) == "?view=commits&period=3d"


@pytest.mark.skipif(NODE is None, reason="Node.js is unavailable")
def test_milestone_tiers_come_from_the_catalog():
    catalog = json.dumps(badges.public_catalog())
    script = HELPERS + _constants() + _functions(SOURCE, [
        "activityTiers", "activityLevel", "activityEarned"]) + """
activityView.catalog = %s;
const earned = (values) => activityEarned(values).map((item) => [item.milestone.id, item.tier]);
console.log(JSON.stringify({
  none: earned({mergedPulls: 0, reviews: 0, bugsClosed: 0, longestStreak: 0}),
  first: earned({mergedPulls: 1, reviews: 0, bugsClosed: 0, longestStreak: 0}),
  more: earned({mergedPulls: 20, reviews: 70, bugsClosed: 1, longestStreak: 30}),
  level: activityLevel(activityTiers(ACTIVITY_MILESTONES[2]), 16),
}));
""" % catalog
    assert _run(script) == {
        "none": [],
        "first": [["first-merge", ""]],
        "more": [["merged", "bronze"], ["reviews", "silver"],
                 ["bugs", "default"], ["streak", "bronze"]],
        "level": 1,
    }


@pytest.mark.skipif(NODE is None, reason="Node.js is unavailable")
def test_lists_collapse_after_ten_rows_and_escape_titles():
    script = HELPERS + _constants() + _functions(SOURCE, [
        "activityPlural", "activityWhen", "activityNameHtml", "activitySection",
        "activityStateLabel", "activityListHtml"]) + """
const items = Array.from({length: 12}, (_, i) => ({number: 12 - i, title: i ? `Pull ${i}` : "<img src=x onerror=alert(1)>", author: "alice", status: i % 2 ? "merged" : "open", at: 1700000000000}));
const html = activityListHtml({owner: "acme", name: "space"}, "activity-merged", "pulls", {count: 12, authors: 1, items}, "12 merged", "", "none");
const empty = activityListHtml({owner: "acme", name: "space"}, "activity-closed", "issues", {count: 0, items: []}, "0 closed", "", "No issues were closed.");
console.log(JSON.stringify({
  hidden: (html.match(/ hidden" data-activity-item/g) || []).length,
  rows: (html.match(/data-activity-item/g) || []).length,
  more: html.includes('data-activity-more="activity-merged"') && html.includes("Show all 12"),
  escaped: html.includes("&lt;img src=x onerror=alert(1)&gt;") && !html.includes("<img src=x"),
  merged: html.includes("fm-activity-done") && html.includes(">Merged<"),
  link: html.includes('href="/acme/space/pulls/12"') && html.includes('data-activity-record="pulls"'),
  empty: empty.includes("No issues were closed.") && !empty.includes("data-activity-more"),
}));
"""
    assert _run(script) == {
        "hidden": 2, "rows": 12, "more": True, "escaped": True,
        "merged": True, "link": True, "empty": True,
    }


@pytest.mark.skipif(NODE is None, reason="Node.js is unavailable")
def test_rank_movement_is_text_and_colour():
    script = HELPERS + _constants() + _functions(SOURCE, [
        "activityPlural", "activityMoveHtml"]) + """
console.log(JSON.stringify([
  activityMoveHtml({rank: 1, previousRank: 4}, false),
  activityMoveHtml({rank: 5, previousRank: 2}, false),
  activityMoveHtml({rank: 3, previousRank: 3}, true),
  activityMoveHtml({rank: 2, previousRank: null}, false),
]));
"""
    up, down, same, new = _run(script)
    assert "▲" in up and "text-success" in up and "Up 3 places" in up
    assert "▼" in down and "text-destructive" in down and "Down 3 places" in down
    assert "Same rank as a week ago" in same
    assert ">New<" in new


@pytest.mark.skipif(NODE is None, reason="Node.js is unavailable")
def test_score_tooltip_explains_the_breakdown():
    script = HELPERS + _constants() + _functions(SOURCE, [
        "activityBreakdownLines", "activityScoreHtml"]) + """
const weights = {merged: 10, commits: 1, issuesClosed: 3, reviews: 4, comments: 1};
console.log(JSON.stringify(activityScoreHtml({score: 25, breakdown: {merged: 2, commits: 1, issuesClosed: 0, reviews: 1, comments: 0}}, weights, 3)));
"""
    html = _run(script)
    assert 'aria-describedby="activity-score-3"' in html
    assert 'id="activity-score-3"' in html and 'role="tooltip"' in html
    assert "2 merged pull requests × 10 = 20" in html
    assert "1 commit to the default branch × 1 = 1" in html
    assert "review of someone else&#039;s pull request × 4 = 4" in html
    assert "issues closed" not in html

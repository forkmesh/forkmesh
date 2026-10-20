"""The About rail's Contributors section reads a committed contributor roster.

`.forkmesh/contributors/contributors.json` (with `<slug>.png` avatars beside
it) outranks the git authors from `/stats`; without it the authors are used,
minus bots. The functions run under node against a tiny DOM stand-in.
"""

import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DASHBOARD = ROOT / "public" / "dashboard"
HELPERS = (DASHBOARD / "js" / "02-helpers.js").read_text(encoding="utf-8")
EXPLORER = (DASHBOARD / "js" / "05-repo-list-explorer.js").read_text(encoding="utf-8")
CONTENT = (DASHBOARD / "js" / "06-repo-content.js").read_text(encoding="utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
DOCS = (ROOT.parent / "docs" / "app" / "repo-views.md").read_text(encoding="utf-8")


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    start = match.start()
    cursor = text.index("{", text.index(")", start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _constants(text, prefix):
    return "\n".join(line for line in text.splitlines()
                     if line.startswith("  const %s" % prefix))


def _node(body):
    script = """
globalThis.location = { search: "", origin: "https://code.acme.test" };
const nodes = {};
function element(name) {
  return nodes[name] ||= {
    textContent: "", innerHTML: "", hidden: true, pending: true,
    classList: { remove() { nodes[name].hidden = false; }, add() {} },
    removeAttribute() { nodes[name].pending = false; },
    querySelectorAll() { return []; },
  };
}
function $(selector) { return element(selector.replace(/^\\[|\\]$/g, "")); }
function repoPublicOwner(repo) { return repo.owner; }
function repoSelectedBranch(repo) { return repo.branch || "main"; }
function repoDataVersion() { return ""; }
function repoLinkUrl(repo, kind, path) { return `/${repo.owner}/${repo.name}/${kind}/${path}`; }
function platformName() { return "Acme Code"; }
function platformAccount() { return false; }
function validNodeName() { return false; }
function repoAboutStillCurrent() { return true; }
let countFormatter = null;
%s
%s
%s
%s
""" % (
        "\n".join(_function(HELPERS, name) for name in ("escapeHtml", "formatCount", "repoApiBase")),
        "\n".join(_function(EXPLORER, name) for name in ("repoRawUrl", "repoLiveUrl")),
        _constants(CONTENT, "REPO_CONTRIBUTOR_ROSTER_") + "\n" + "\n".join(
            _function(CONTENT, name) for name in (
                "repoContributorAvatar", "repoContributorAvatarUrl", "dropBrokenContributorAvatars",
                "renderRepoAboutContributors", "repoContributorIsBot", "repoContributorProfileUrl",
                "parseRepoContributorRoster", "repoRosterContributorAvatar", "renderRepoAboutRoster",
                "renderRepoAboutAuthors")),
        body,
    )
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


def _roster(count=None, rows=12):
    data = {"about": "Everyone with a commit.", "contributors": [
        {"slug": "user-%02d" % index, "name": "User %02d" % index, "login": "user%02d" % index,
         "profile": "https://example.com/@user%02d" % index, "source": "github",
         "github": "user%02d" % index, "gitlab": None, "commits": index * 10,
         "first": "2024-01-01", "last": "2026-10-01", "projects": ["acme-api"]}
        for index in range(rows)]}
    if count is not None:
        data["count"] = count
    return json.dumps(data)


def test_the_roster_supplies_the_count_avatars_and_raw_urls():
    out = _node("""
const roster = parseRepoContributorRoster(%s);
const repo = { owner: "acme", name: "space", branch: "dev" };
const drawn = renderRepoAboutRoster(repo, roster);
console.log(JSON.stringify({
  drawn, count: roster.count, first: roster.rows[0].name,
  text: element("data-repo-about-contribs-count").textContent,
  html: element("data-repo-about-contribs-list").innerHTML,
  shown: !element("data-repo-about-contribs").hidden,
}));
""" % json.dumps(_roster(count=40)))
    assert out["drawn"] and out["shown"]
    assert out["count"] == 40 and out["text"] == "40"
    assert out["first"] == "User 11"  # sorted by commits, not file order
    html = out["html"]
    srcs = re.findall(r'<img src="([^"]+)"', html)
    assert len(srcs) == 8
    assert srcs[0] == ("/api/repo/acme/space/raw?path=.forkmesh%2Fcontributors%2Fuser-11.png&amp;ref=dev")
    assert srcs[-1].startswith("/api/repo/acme/space/raw?path=.forkmesh%2Fcontributors%2Fuser-04.png")
    assert 'title="User 11 — 110 commits"' in html
    assert '<a href="https://example.com/@user11" target="_blank" rel="noopener noreferrer nofollow ugc"' in html
    assert html.count("data-repo-contributor-avatar") == 8
    # "+N" counts everyone the roster names and opens the roster file.
    assert '<a href="/acme/space/blob/.forkmesh/contributors/contributors.json"' in html
    assert ">+32</a>" in html


def test_the_count_falls_back_to_the_number_of_rows():
    out = _node("""
const roster = parseRepoContributorRoster(%s);
console.log(JSON.stringify({ count: roster.count, rows: roster.rows.length }));
""" % json.dumps(_roster(count="many", rows=3)))
    assert out == {"count": 3, "rows": 3}


def test_invalid_rosters_are_rejected():
    big = json.dumps({"contributors": [{"name": "x" * 50}] * 50000})  # over 2 MiB
    many = json.dumps({"contributors": [{"name": "x"}] * 10001})  # over the row cap
    out = _node("""
console.log(JSON.stringify([%s].map((text) => parseRepoContributorRoster(text))));
""" % ", ".join(json.dumps(value) for value in (
        "{not json", "[]", json.dumps({"contributors": []}), json.dumps({"contributors": "alice"}),
        json.dumps({"contributors": [None, 3, {"commits": 4}]}), big, many)))
    assert out == [None] * 7


def test_malicious_roster_values_are_escaped_and_unsafe_links_dropped():
    roster = json.dumps({"count": "<b>", "contributors": [
        {"slug": "../../secret", "name": '"><img src=x onerror=alert(1)>', "commits": 9,
         "profile": "javascript:alert(1)"},
        {"slug": "bob", "name": "Bob", "commits": 5, "profile": "http://example.com/bob"},
        {"slug": "carol", "name": "Carol", "commits": 3, "profile": "https://u:p@example.com/"},
        {"slug": "dave\" onerror=\"x", "name": "Dave", "commits": 2,
         "profile": "https://example.com/\"><script>"},
    ]})
    out = _node("""
const roster = parseRepoContributorRoster(%s);
renderRepoAboutRoster({ owner: "acme", name: "space" }, roster);
console.log(JSON.stringify({ html: element("data-repo-about-contribs-list").innerHTML,
                             count: element("data-repo-about-contribs-count").textContent }));
""" % json.dumps(roster))
    html = out["html"]
    assert out["count"] == "4"
    assert "<img src=x" not in html and "&quot;&gt;&lt;img src=x onerror=alert(1)&gt;" in html
    assert "javascript:" not in html and "http://example.com/bob" not in html and "u:p@" not in html
    assert "<script>" not in html and "%22%3E%3Cscript%3E" in html
    assert "secret" not in html and "onerror=\\\"x" not in html and 'onerror="x' not in html
    # Only bob and carol have safe slugs, so only they get a picture.
    assert re.findall(r"contributors%2F([a-z]+)\.png", html) == ["bob", "carol"]


def test_without_a_roster_the_stats_authors_are_shown_without_bots():
    contributors = [
        {"name": "GitHub Action", "commits": 900},
        {"name": "github-actions[bot]", "commits": 400},
        {"name": "dependabot[bot]", "commits": 300},
        {"name": "Alice", "commits": 200},
        {"name": "acme-actions", "commits": 100},
        {"name": "Acme Code Actions", "commits": 90},
        {"name": "ForkMesh Actions", "commits": 80},
        {"name": "ForkMesh Deploy", "commits": 75},
        {"name": "Tool Bot", "commits": 70},
        {"name": "helper-bot", "commits": 60},
        {"name": "Bob <b>", "commits": 50},
        {"name": "Abbot", "commits": 40},
    ]
    out = _node("""
await renderRepoAboutAuthors({ owner: "acme", name: "space" }, Promise.resolve(null), %s, 20);
console.log(JSON.stringify({ html: element("data-repo-about-contribs-list").innerHTML,
                             count: element("data-repo-about-contribs-count").textContent }));
""" % json.dumps(contributors))
    titles = re.findall(r'title="([^"]+)"', out["html"])
    assert titles == ["Alice — 200 commits", "Bob &lt;b&gt; — 50 commits", "Abbot — 40 commits"]
    assert out["count"] == "11"  # 20 authors minus the 9 bots in the list
    assert "<b>" not in out["html"]


def test_a_roster_suppresses_the_author_fallback():
    out = _node("""
await renderRepoAboutAuthors({ owner: "acme", name: "space" },
  Promise.resolve({ count: 1, rows: [{ name: "Alice", commits: 1 }] }), [{ name: "Mallory", commits: 9 }], 1);
console.log(JSON.stringify({ html: element("data-repo-about-contribs-list").innerHTML }));
""")
    assert out["html"] == ""


def test_the_roster_is_read_once_through_the_optional_folder_machinery():
    load = _function(CONTENT, "loadRepoContributorRoster")
    assert "repoOptionalTreeMissed(repo, REPO_CONTRIBUTOR_ROSTER_PATH)" in load
    assert "repoOptionalTreeParentHas(repo, REPO_CONTRIBUTOR_ROSTER_DIR)" in load
    assert load.count("rememberRepoOptionalTreeMiss(") == 2
    assert "fetchRepoBlobs(repo, [REPO_CONTRIBUTOR_ROSTER_PATH])" in load
    insights = _function(CONTENT, "loadRepoAboutInsights")
    assert "const roster = loadRepoContributorRoster(repo);" in insights
    assert "loadRepoAboutStats(repo, roster)" in insights
    assert "loadRepoAboutContributors(repo, roster)" in insights
    assert "renderRepoAboutRoster(repo, committed)" in insights
    assert "await renderRepoAboutAuthors(repo, roster, data.contributors" in _function(CONTENT, "loadRepoAboutStats")
    assert "function loadRepoContributorRoster(" in BUNDLE and "function repoContributorIsBot(" in BUNDLE
    assert ".forkmesh/contributors/contributors.json" in DOCS

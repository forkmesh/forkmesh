"""Server-rendered issue page (forkmesh_host.repo_ssr_issue).

The first view is only steady if dashboard.js, taking the page over, draws
the same markup in the same place. The parity tests run the dashboard's own
renderRepoIssueDetail (extracted from the sources, under node) on an issue
and compare it with the server's article: same elements, same classes, same
text, same whitespace. Icons are compared by name (the server draws the
<svg> lucide.createIcons() puts in place of <i data-lucide>).
"""

from __future__ import annotations

import asyncio
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from forkmesh_host import repo_ssr, repo_ssr_issue, repo_views

ROOT = Path(__file__).resolve().parents[2]
JS = ROOT / "public" / "dashboard" / "js"
DASHBOARD = ROOT / "public" / "dashboard"
NOW_MS = 1792480000000
OWNER, NAME = "acme-bolt", "Bolt"
LINK_BASE, API_BASE = "/Bolt", "/api/repo/acme-bolt/Bolt"
PAGE_PATH = "/Bolt/issues/27"
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

SOURCES = {name: (JS / name).read_text("utf-8") for name in (
    "02-helpers.js", "03-issues-profile-io.js", "04-account.js", "06-repo-content.js")}
FUNCTIONS = {
    "02-helpers.js": (
        "escapeHtml", "userNameKey", "userDisplayName", "userNameHtml", "issueActorAvatarHtml",
        "parseFlexibleDate", "formatDate", "formatTimeAgo", "formatCount", "loadingHtml",
        "issuePersonHandle", "issueActorIsBot", "issueActorPixels", "issuePersonProfileHref",
        "issuePersonNameHtml"),
    "03-issues-profile-io.js": (
        "webIssueClaimApplied", "webIssueAssigneesWithClaims", "issueSubtaskCandidates"),
    "04-account.js": ("validNodeName",),
    "06-repo-content.js": (
        "mdSafeUrl", "mdImageSource", "mdHeadingSlug", "mdLinkHtml", "mdImageHtml",
        "mdTrimAutolink", "renderMarkdownInline", "htmlTagAttr",
        "decodeHtmlEntities", "flattenMarkdownHtml", "flattenOutsideCode", "takeWrappedLines",
        "joinWrappedLines", "mdLoneImage", "renderMarkdown", "renderRecordMarkdown",
        "parseFrontMatterList", "parseIssueJson", "formatRecordDate", "issueDetailParsed",
        "issueEventIcon", "foldIssueReactions", "renderIssueReactionChips",
        "issueEventDescription", "renderIssueTimelineComment",
        "renderIssueTimelineEvent", "issueEventDescriptionHtml", "renderIssueTimeline",
        "formatRecordDateTime", "issueLabelHue", "renderIssueLabelChip",
        "issueRelationStatusPill", "renderRepoIssueParents", "renderRepoIssueSubIssues",
        "issueDescriptionWithoutDependencies", "issueParticipants", "issueCommentCount",
        "renderRepoIssueDetail", "renderIssueCommentForm", "repoContributorIsBot",
        "issueMentionsOf", "issueReferenceNumbers", "issueDependencyNumbers",
        "issueSubIssueNumbers", "renderRecordRewardEstimate", "formatRewardSol",
        "issueDateInputValue", "mdMentionHtml", "issueCardAuthorHtml"),
}
CONSTANTS = {
    "02-helpers.js": ("USER_NAME_RE", "ISSUE_PERSON_HANDLE_RE"),
    "06-repo-content.js": (
        "MD_DATA_IMAGE_URL", "MD_BARE_URL", "MD_ENTITIES", "MD_HARD_BREAK",
        "MD_BLOCK_START", "MD_LONE_IMAGE", "MD_BODY_CLASS", "ISSUE_REACTION_EMOJI",
        "rewardStateLabels", "MD_MENTION"),
}


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


def _const(text, name):
    start = text.index("  const %s =" % name)
    match = re.compile(r";\n").search(text, start)
    return text[start:match.end()]


HARNESS = """
const NOW = %(now)d;
Date.now = () => NOW;
const VIEWER = %(viewer)s;
const state = { session: VIEWER === "member" ? { nodeName: "me" } : null,
  userNameCases: new Map(), issuesView: { items: [] }, repoCollectionItems: {} };
let countFormatter = null;
function watchIssueActorAvatars() {}
function platformName() { return "Acme Code"; }
function publicSiteOrigin() { return ""; }
function publicProfileUrl(name, suffix = "") {
  return `/@${encodeURIComponent(String(name || "").trim().toLowerCase())}${suffix}`;
}
function repoLinkUrl(repo, kind = "tree", path = "") {
  return `%(link)s${path ? `/${kind}/${path.split("/").map(encodeURIComponent).join("/")}` : ""}`;
}
function repoRecordUrl(repo, kind, number) { return `%(link)s/${kind}/${number}`; }
function repoRawUrl(repo, path) { return `%(api)s/raw?path=${encodeURIComponent(path)}&ref=main`; }
const navigator = { platform: "Linux" };
const location = { pathname: %(path)s, search: "" };
%(code)s
const input = JSON.parse(%(input)s);
const repo = { owner: %(owner)s, name: %(name)s, isPrivate: false };
state.selectedRepo = repo;
const index = input.index.map((text, i) => parseIssueJson(text, 0));
const parsed = issueDetailParsed(input.issue, input.number);
const byNumber = new Map(index.map((issue) => [Number(issue.number), issue]));
parsed.issueDependencies = issueDependencyNumbers(parsed.body)
  .filter((n) => n !== input.number).map((n) => byNumber.get(n)).filter(Boolean);
parsed.issueSubIssues = issueSubIssueNumbers(index, input.number, parsed.body)
  .map((n) => byNumber.get(n) || { number: n, title: "", status: "unknown" })
  .filter((issue) => !issue.deleted);
parsed.issueMentions = issueMentionsOf(index, input.number);
parsed.issueMutationAuthorized = false;
parsed.issueSubscribed = false;
state.repoRecordDetail = { repo, kind: "issues", number: input.number, parsed };
process.stdout.write(renderRepoIssueDetail(repo, input.number, parsed));
"""


def js_render(issue, number, index=(), viewer="guest"):
    code = "\n".join(
        [_const(SOURCES[name], const) for name, consts in CONSTANTS.items() for const in consts]
        + [_function(SOURCES[name], fn) for name, fns in FUNCTIONS.items() for fn in fns])
    script = HARNESS % {
        "now": NOW_MS, "viewer": json.dumps(viewer), "link": LINK_BASE, "api": API_BASE,
        "path": json.dumps(PAGE_PATH), "code": code, "owner": json.dumps(OWNER),
        "name": json.dumps(NAME), "input": json.dumps(json.dumps({
            "issue": json.dumps(issue), "number": number,
            "index": [json.dumps(row) for row in index]}))}
    return subprocess.run(["node", "-"], input=script, check=True, capture_output=True,
                          text=True, env={"TZ": "UTC", "LANG": "en_US.UTF-8",
                                          "PATH": "/usr/bin:/bin:" + str(Path(shutil.which("node")).parent)}
                          ).stdout


def py_render(issue, number, index=(), viewer="guest"):
    ctx = repo_ssr_issue.Context(LINK_BASE, API_BASE, "main")
    rows = [repo_ssr_issue.parse_issue(json.dumps(row), 0) for row in index]
    return repo_ssr_issue.render_issue(
        repo_ssr_issue.parse_issue(json.dumps(issue), number), ctx, OWNER, PAGE_PATH,
        index=rows, now=NOW_MS / 1000, viewer=viewer)


ICON_RE = re.compile(r'<svg xmlns="http://www.w3.org/2000/svg"[^>]*class="lucide lucide-'
                     r'([a-z0-9-]+) ?([^"]*)">.*?</svg>')


def normalize(markup):
    markup = ICON_RE.sub(r'<i data-lucide="\1" class="\2"></i>', markup)
    markup = re.sub(r' data-repo-ssr-issue="\d+"', "", markup)
    # The comment box's avatar is the signed-in visitor's, unknown to the server.
    markup = re.sub(r'<span class="hidden sm:block">(?:(?!<span class="hidden sm:block">).)*?'
                    r'</span>(\s*<form)', r'<span class="hidden sm:block">AVATAR</span>\1',
                    markup, flags=re.S)
    return re.sub(r"\s+", " ", markup).strip()


def issue27():
    return {
        "number": 27, "title": "Leaderboard <b>shows</b> wrong info", "status": "open",
        "authorName": "import[bot]", "author": "k-bot", "createdAt": NOW_MS - 5 * 3600_000,
        "labels": ["bolt-next", "ui & ux"], "assignees": ["riley"], "milestone": "M1",
        "priority": 2, "progress": 40, "votes": 3,
        "events": [
            {"type": "open", "id": "o1", "author": "k-bot", "authorName": "import[bot]",
             "ts": NOW_MS - 5 * 3600_000, "title": "Leaderboard",
             "body": "![image.png](https://img.example/a.png)\n\n---\n\nMoved from "
                     "https://git.example/x/-/issues/1 (opened by @Morgan on 2026-05-30)."
                     "\n\n**Discussion summary**\n\n- The rows render outside the table, "
                     "see #12 and pr #3.\n- `/api/leaderboard` serves `LEADERBOARD` "
                     "(main.py:134)\n  wrapped *line*\n\n- [ ] first task\n- [x] done "
                     "(sub-issue #31)\n\n| a | b |\n|---|---|\n| <i>x</i> | y |\n\n"
                     "> quoted <script>alert(1)</script>\n\n```\n<b>code</b>\n```\n"
                     "<p align=\"center\"><img src=\"logo.png\" width=\"80\"></p>\n"
                     "[link](docs/a.md#Intro) [bad](javascript:alert(1))"},
            {"type": "labels", "id": "l1", "author": "k-bot", "authorName": "import[bot]",
             "labels": ["bolt-next", "ui & ux"], "ts": NOW_MS - 5 * 3600_000 + 1},
            {"type": "edit", "id": "e1", "author": "k-bot", "authorName": "import[bot]",
             "target": "o1", "ts": NOW_MS - 4 * 3600_000,
             "body": "![image.png](https://img.example/a.png)\n\nEdited **body** with "
                     "@riley and #31.\n\n- [ ] first task\n- [x] done (sub-issue #31)"},
            {"type": "comment", "id": "c1", "author": "k2", "authorName": "riley",
             "body": "/assign", "ts": NOW_MS - 3 * 3600_000},
            {"type": "assignees", "id": "a1", "author": "k3", "authorName": "tasks[bot]",
             "assignees": ["riley"], "ts": NOW_MS - 3 * 3600_000 + 5},
            {"type": "comment", "id": "c2", "author": "k3", "authorName": "tasks[bot]",
             "body": "Assigned to @riley.\n\n<details><summary>Rules</summary>Be "
                     "kind</details>", "ts": NOW_MS - 2 * 3600_000},
            {"type": "reaction", "id": "r1", "author": "k4", "authorName": "erin",
             "target": "c2", "reaction": "like", "ts": NOW_MS - 3600_000},
            {"type": "reaction", "id": "r2", "author": "k4", "authorName": "erin",
             "target": "o1", "reaction": "rocket", "ts": NOW_MS - 3500_000},
            {"type": "comment", "id": "c3", "author": "k5", "authorName": "mallory",
             "body": "spam", "ts": NOW_MS - 1000_000},
            {"type": "delete", "id": "d1", "author": "k5", "target": "c3",
             "ts": NOW_MS - 900_000},
            {"type": "priority", "id": "p1", "author": "k2", "authorName": "riley",
             "priority": 2, "ts": NOW_MS - 800_000},
            {"type": "milestone", "id": "m1", "author": "k2", "authorName": "riley",
             "milestone": "M1", "ts": NOW_MS - 700_000},
            {"type": "claim", "id": "cl1", "author": "k6", "authorName": "zed",
             "assignee": "zed", "ts": NOW_MS - 600_000},
            {"type": "agent", "id": "ag1", "author": "k2", "authorName": "riley",
             "agentSessionId": 9, "agentProvider": "Claude", "agentStatus": "running",
             "ts": NOW_MS - 500_000},
            {"type": "status", "id": "s1", "author": "k2", "authorName": "riley",
             "status": "open", "ts": NOW_MS - 400_000},
        ]}


def index_rows():
    return [
        {"number": 31, "title": "Child", "status": "open", "authorName": "zed",
         "events": [{"type": "open", "id": "x", "author": "z", "body": "Parent issue: #27"}]},
        {"number": 12, "title": "Mentions it", "status": "open", "authorName": "ann",
         "events": [{"type": "open", "id": "y", "author": "a", "ts": NOW_MS - 100_000,
                     "body": "Related to #27."}]},
    ]


@needs_node
@pytest.mark.parametrize("viewer", ["guest", "member"])
def test_first_view_is_the_dashboards_issue_markup(viewer):
    issue = issue27()
    expected = normalize(js_render(issue, 27, index_rows(), viewer))
    got = normalize(py_render(issue, 27, index_rows(), viewer))
    assert got == expected


@needs_node
def test_a_bare_closed_issue_matches_too():
    issue = {"number": 5, "title": "", "status": "closed", "events": [
        {"type": "open", "id": "o", "author": "someone", "ts": "2026-01-02T03:04:05Z"}]}
    assert normalize(py_render(issue, 5)) == normalize(js_render(issue, 5))


def test_untrusted_text_is_escaped():
    issue = issue27()
    issue["title"] = '"><script>alert(1)</script>'
    issue["labels"] = ['<img src=x onerror=y>']
    issue["status"] = '<script>alert(2)</script><img src=x onerror=y>'
    issue["events"][0]["authorName"] = "<b>x</b>"
    issue["events"][3]["body"] = ('<script>alert(1)</script> <img src=x onerror="y"> '
                                  '[x](javascript:alert(1)) <a href="vbscript:z">z</a>')
    html = py_render(issue, 27, viewer="both")
    assert "<script>" not in html and not re.search(r"<[^>]*onerror", html)
    assert "javascript:" not in html and "vbscript:" not in html
    assert "&lt;script&gt;alert(1)&lt;/script&gt;" in html


def test_both_viewers_are_drawn_and_picked_by_the_guest_flag():
    html = py_render(issue27(), 27, index_rows(), viewer="both")
    assert re.search(r"<div data-ssr-guest [^>]*>.*Sign in</a> to join", html, re.S)
    assert re.search(r"<div data-ssr-member [^>]*>.*data-repo-issue-comment-form", html, re.S)
    # The sub-issue list is everyone's; only its Add button is a member's.
    assert "<section data-repo-issue-subissues" in html
    assert "<button data-ssr-member type=\"button\" data-repo-issue-add-subissue" in html
    assert "html[data-guest] [data-ssr-member]" in repo_ssr_issue.VIEWER_STYLE
    shell = (DASHBOARD / "shell.html").read_text("utf-8")
    assert 'if (!signedIn) document.documentElement.dataset.guest = "";' in shell


def test_a_lone_picture_of_unknown_size_gets_a_fixed_frame():
    ctx = repo_ssr_issue.Context(LINK_BASE, API_BASE)
    html = repo_ssr_issue.record_markdown("![a](https://x.test/a.png)\n\nText", ctx)
    assert "data-md-figure" in html
    for source in ("![a](https://x.test/a.png) and text", '<img src="a.png" width="20">',
                   "![a](https://x.test/a.png) ![b](https://x.test/b.png)"):
        assert "data-md-figure" not in repo_ssr_issue.record_markdown(source, ctx), source
    shell = (DASHBOARD / "shell.html").read_text("utf-8")
    assert re.search(r"img\[data-md-figure\]\s*\{[^}]*aspect-ratio", shell)


def _tailwind_selector(token):
    escaped = re.sub(r"([^a-zA-Z0-9_-])", r"\\\1", token).replace("\\,", "\\2c ")
    return "." + escaped


def test_first_view_uses_classes_the_stylesheet_has():
    html = py_render(issue27(), 27, index_rows(), viewer="both") + repo_ssr_issue.FRAME
    css = (DASHBOARD / "tailwind.css").read_text("utf-8")
    shell = (DASHBOARD / "shell.html").read_text("utf-8")
    tokens = {token for match in re.finditer(r'class="([^"]*)"', html)
              for token in match.group(1).split()}
    own = ("lucide", "dashboard-accent-link", "issue-", "copy-", "fm-spinner", "group")
    missing = sorted(token for token in tokens if not token.startswith(own)
                     and _tailwind_selector(token) not in css and "." + token not in shell)
    assert missing == []


def test_the_frame_is_the_dashboards():
    detail = (JS / "08-repo-detail-network.js").read_text("utf-8")
    panels = (JS / "07-repo-compose-branch.js").read_text("utf-8")
    # The content grid (one column on an Issues route) and the Issues panel.
    assert "mx-auto grid w-full max-w-7xl min-w-0 gap-5 px-3 pb-3 pt-4 sm:p-5" in detail
    assert "mx-auto grid w-full max-w-7xl min-w-0 gap-5 px-3 pb-3 pt-4 sm:p-5 " \
        "lg:grid-cols-1" in repo_ssr_issue.FRAME
    line = next(line for line in panels.splitlines()
                if 'data-repo-collection-layout="issues"' in line)
    assert 'class="mt-2 grid min-w-0 gap-6"' in line
    assert 'lg:grid-cols-1", !conversation' in detail
    assert 'class="mt-2 grid min-w-0 gap-6"' in repo_ssr_issue.FRAME


# -- the page ----------------------------------------------------------------

def repo_page():
    return (DASHBOARD / "repo.html").read_text("utf-8")


def bundle_for(issue, index=()):
    blobs = {".forkmesh/issues/open/27/issue-27.json": {"content": json.dumps(issue)}}
    for row in index:
        blobs[".forkmesh/issues/open/%d/issue-%d.json" % (row["number"], row["number"])] = {
            "content": json.dumps(row)}
    raw = {"blobs?path=x": json.dumps({"blobs": blobs}).encode()}
    model = {"issue": repo_views.issue_summary(json.dumps(issue), 27),
             "issueJson": json.dumps(issue), "index": blobs}
    return repo_views.Bundle(OWNER, NAME, "issue", 27, raw, model, "node-1", "main")


DATA = {"owner": OWNER, "repo": NAME, "ref": "main", "commit": "a" * 40, "servedBy": "n",
        "tree": {"ok": True, "commit": "a" * 40, "counts": {"openIssues": 39, "openPulls": 0}}}


def test_inject_issue_draws_header_tabs_and_the_issue():
    bundle = bundle_for(issue27(), index_rows())
    page = repo_ssr.inject_issue(repo_page(), DATA, bundle, short=True)
    assert page is not None
    view = page[page.index("<div data-repo-ssr"):]
    assert 'href="/Bolt/issues" role="tab" aria-selected="true"' in view
    assert ">39</span>" in view
    assert 'data-repo-ssr-issue="27"' in view
    assert "Leaderboard &lt;b&gt;shows&lt;/b&gt; wrong info" in view
    assert "data-repo-content-grid" in view
    assert "window.__forkmeshView=" in page
    assert bundle.url().replace("&", "\\u0026") in page
    assert repo_ssr_issue.VIEWER_STYLE in page
    assert '<a href="/Bolt" class="hover:underline">Bolt</a>' in page
    assert repo_ssr.inject_issue("<html></html>", DATA, bundle) is None
    empty = repo_views.Bundle(OWNER, NAME, "issue", 27, {}, {"issue": None}, "n", "main")
    assert repo_ssr.inject_issue(repo_page(), DATA, empty) is None


def test_the_dashboard_keeps_the_first_view_until_its_own_render():
    content = (JS / "06-repo-content.js").read_text("utf-8")
    detail = (JS / "08-repo-detail-network.js").read_text("utf-8")
    assert "takeRepoFirstViewIssue(repo, number)" in content
    assert "keepRepoFirstViewIssue(detail)" in detail
    assert detail.index("keepRepoFirstViewIssue(detail)") < detail.index(
        "detail.innerHTML = `\n      <div data-repo-layout=\"github-like\"")


def test_a_long_address_issue_page_uses_the_same_first_view():
    from forkmesh_host import js

    bundle = bundle_for(issue27(), index_rows())

    async def main():
        loop = asyncio.get_running_loop()
        task, root = loop.create_future(), loop.create_future()
        task.set_result(bundle)
        root.set_result(DATA)
        ticket = repo_ssr.ViewTicket(task, "", (OWNER, NAME, "issue", 27), root)
        response = js.Response(repo_page().encode("utf-8"), 200, "",
                               {"content-type": "text/html; charset=utf-8"})
        answer = await repo_ssr.finish(ticket, response)
        return (await answer._read()).decode("utf-8")
    html = asyncio.run(main())
    view = html[html.index("<div data-repo-ssr"):]
    assert 'data-repo-ssr-issue="27"' in view
    assert 'href="/acme-bolt/Bolt/issues" role="tab" aria-selected="true"' in view
    assert 'href="/acme-bolt/Bolt/issues/31"' in view

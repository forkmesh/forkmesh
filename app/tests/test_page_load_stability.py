import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DASHBOARD = ROOT / "public" / "dashboard"
SHELL = (DASHBOARD / "shell.html").read_text(encoding="utf-8")
HEADER = (DASHBOARD / "partials" / "header.html").read_text(encoding="utf-8")
PROFILE = (DASHBOARD / "partials" / "views" / "profile-overview.html").read_text(encoding="utf-8")
HELPERS = (DASHBOARD / "js" / "02-helpers.js").read_text(encoding="utf-8")
CONTENT = (DASHBOARD / "js" / "06-repo-content.js").read_text(encoding="utf-8")
DETAIL = (DASHBOARD / "js" / "08-repo-detail-network.js").read_text(encoding="utf-8")


def _node(script):
    result = subprocess.run(["node", "--input-type=module", "-e", script],
                            check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def _head_bootstrap():
    head = SHELL.split("</head>", 1)[0]
    script = re.search(r"<script>\s*\(\(\) => \{\s*const key = \"forkmesh\.dashboard\.theme\";.*?\}\)\(\);",
                       head, re.S)
    return script.group(0)[len("<script>"):]


def _guest(cookie, stored):
    script = """
const dataset = {};
globalThis.document = {
  cookie: %s,
  documentElement: { dataset, style: {} },
  querySelector: () => null,
};
globalThis.location = { search: "" };
globalThis.localStorage = { getItem: (key) => (key === "forkmesh.session" ? %s : null), setItem() {} };
%s
console.log(JSON.stringify({ guest: "guest" in dataset, theme: dataset.dashboardTheme }));
""" % (json.dumps(cookie), json.dumps(stored), _head_bootstrap())
    return _node(script)


def test_head_marks_signed_out_visitors_before_the_first_paint():
    assert _guest("", None) == {"guest": True, "theme": "dark"}
    assert _guest("other=1; forkmesh_session=1", None)["guest"] is False
    assert _guest("", json.dumps({"nodeName": "ada"}))["guest"] is False
    assert _guest("", json.dumps({"email": "a@example.test"}))["guest"] is False
    assert _guest("forkmesh_session=; x=1", json.dumps({}))["guest"] is True
    head = SHELL.split("</head>", 1)[0]
    assert head.index("dataset.guest") < head.index('href="/dashboard/tailwind.css"')


def test_signed_out_header_and_icon_space_are_styled_from_the_first_paint():
    style = SHELL.split("<style>", 1)[1]
    assert ":where(i[data-lucide]) {\n        display: inline-block;" in style
    assert "html[data-guest] [data-profile-account-controls]," in style
    assert "html[data-guest] [data-guest-auth-link] {\n        display: inline-flex !important;" in style
    assert 'document.documentElement?.toggleAttribute?.("data-guest", guest);' in DETAIL
    assert 'document.documentElement?.removeAttribute?.("data-guest")' in HELPERS.split(
        "function writeSession(", 1)[1].split("\n  }\n", 1)[0]


def test_always_visible_header_icons_are_drawn_in_the_page():
    for name in ("menu", "search", "command", "globe-2", "bell", "bot"):
        assert f'<i data-lucide="{name}"' not in HEADER, name
        assert re.search(r'<svg [^>]*data-lucide="%s"[^>]* class="lucide lucide-%s' % (name, name),
                         HEADER), name
    skip = HELPERS.split("function installIncrementalIcons()", 1)[1].split("installIncrementalIcons();", 1)[0]
    assert 'element.classList.contains(`lucide-${element.getAttribute("data-lucide")}`)) continue;' in skip


def test_profile_period_buttons_have_their_final_width_in_the_page():
    periods = PROFILE.split("data-profile-contribution-periods", 1)[1].split("</script>", 1)[0]
    assert periods.count("<button ") == 6
    assert periods.count("&#8199;&#8199;&#8199;&#8199;</button>") == 5
    assert "button.textContent = String(year - index);" in periods
    assert 'style="min-height:3.5rem"' in PROFILE
    assert 'data-profile-about-edit type="button" aria-label="Edit about yourself" class="-my-1 flex' in PROFILE


def test_about_rail_reserves_its_late_sections():
    rail = DETAIL.split("<aside data-repo-about data-repo-about-rail", 1)[1].split("</aside>", 1)[0]
    assert '<div data-repo-about-files class="grid' in rail
    assert '<div data-repo-about-sizemap>' in rail
    assert '<div data-repo-about-langs data-repo-about-pending class="mt-3 border-t' in rail
    assert '<div data-repo-about-contribs data-repo-about-pending class="mt-3 border-t' in rail
    assert "data-repo-about-sizemap-marker" not in CONTENT
    insights = CONTENT.split("async function loadRepoAboutInsights(repo) {", 1)[1].split("\n  }\n", 1)[0]
    assert "Promise.allSettled" in insights and "settleRepoAboutPending()" in insights
    assert 'section.removeAttribute("data-repo-about-pending");' in CONTENT.split(
        "function renderRepoAboutLanguages(", 1)[1].split("\n  }\n", 1)[0]


def _function(text, name):
    start = text.index("  function %s(" % name)
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


def test_dashboard_takes_the_first_view_about_before_painting():
    assert "applyRepoFirstViewAbout(repo);\n    // Restore whichever tab" in DETAIL
    script = """
const calls = [];
let seedText = %s;
globalThis.document = { getElementById: () => (seedText === null ? null : { textContent: seedText }) };
function repoApiBase(repo) { return `/api/repo/${repo.owner}/${repo.name}`; }
function applyRepoFeatures(value) { calls.push(["features", value]); }
function applyRepoAboutDescription(repo, value) { calls.push(["about", value]); }
function applyRepoAboutWebsite(value) { calls.push(["website", value]); }
%s
%s
applyRepoFirstViewAbout({ owner: "Acme", name: "Tool" });
const first = calls.splice(0);
seedText = JSON.stringify({ owner: "other", repo: "tool", features: { issues: false } });
applyRepoFirstViewAbout({ owner: "Acme", name: "Tool" });
const other = calls.splice(0);
seedText = "{not json";
applyRepoFirstViewAbout({ owner: "Acme", name: "Tool" });
console.log(JSON.stringify({ first, other, broken: calls }));
""" % (json.dumps(json.dumps({"owner": "acme", "repo": "tool", "features": {"issues": False},
                               "info": {"about": "Hello", "website": "https://e.test"}})),
       _function(CONTENT, "repoFirstViewSeed"), _function(CONTENT, "applyRepoFirstViewAbout"))
    out = _node(script)
    assert out["first"] == [["features", {"issues": False}], ["about", "Hello"],
                            ["website", "https://e.test"]]
    assert out["other"] == [] and out["broken"] == []


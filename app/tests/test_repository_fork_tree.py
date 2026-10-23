import json
import re
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
EXPLORER = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text(encoding="utf-8")
DOCS = (ROOT.parent / "docs" / "app" / "repo-views.md").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"
EXPORTS = (
    "nestRepositoryForks, homeRepositoryEntries, renderHomeRepositories, "
    "renderSidebarRepositories, toggleRepositoryForks, "
    "applyRepositoryForkCounts, hydrateRepositoryForkCounts, state")

STUBS = """
const vm = require("vm");
const calls = [];
let countsReply = {};
global.fetch = async (url, init) => {
  calls.push({ url: String(url), body: init && init.body ? JSON.parse(init.body) : null });
  if (String(url) === "/api/repo-fork-counts") {
    return { ok: true, json: async () => ({ ok: true, counts: countsReply }) };
  }
  return { ok: true, json: async () => ({ ok: true, views: {} }) };
};
global.location = { origin: "https://code.example.org", hostname: "code.example.org", pathname: "/dashboard", search: "", hash: "" };
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
const home = { innerHTML: "", querySelectorAll() { return []; } };
const sidebar = { innerHTML: "", querySelectorAll() { return []; } };
const sidebarCount = { textContent: "" };
let countNodes = [];
function countNode(key, parent) {
  const attrs = { "data-repo-fork-counts": key, "data-repo-fork-parent": parent };
  const classes = new Set(["hidden"]);
  return {
    attrs, classes, textContent: "",
    getAttribute: (name) => (name in attrs ? attrs[name] : null),
    setAttribute: (name, value) => { attrs[name] = String(value); },
    removeAttribute: (name) => { delete attrs[name]; },
    classList: {
      add: (name) => classes.add(name),
      remove: (name) => classes.delete(name),
      contains: (name) => classes.has(name),
    },
  };
}
global.document = {
  body: { classList: emptyClassList, dataset: { page: "home" } },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector(sel) {
    if (sel === "[data-home-top-repositories]") return home;
    if (sel === "[data-sidebar-repo-list]") return sidebar;
    if (sel === "[data-sidebar-repo-count]") return sidebarCount;
    return null;
  },
  querySelectorAll(sel) { return sel === "[data-repo-fork-counts]" ? countNodes : []; },
  addEventListener() {},
};
global.CustomEvent = class { constructor(type) { this.type = type; } };
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  dispatchEvent() { return true; },
  history: { state: null, pushState() {}, replaceState() {} },
  lucide: { createIcons() {} },
  setTimeout() {}, setInterval() {}, clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const api = global.__dashboardExports;
const out = {};
api.state.repositories = [
  { owner: "alice", name: "widget", rootCommit: "r1", cloneOnline: true, description: "The widget" },
  { owner: "bob", name: "widget", rootCommit: "r1", cloneOnline: true, forkedFrom: "alice/widget" },
  { owner: "carol", name: "widget", rootCommit: "r1", cloneOnline: true, forkedFrom: "alice/widget" },
  { owner: "dave", name: "widget", rootCommit: "r1", cloneOnline: true, forkedFrom: "alice/widget" },
  { owner: "erin", name: "gizmo", rootCommit: "r1", cloneOnline: true, forkedFrom: "alice/widget" },
  { owner: "frank", name: "widget", rootCommit: "r1", cloneOnline: true, forkedFrom: "bob/widget", isPrivate: true },
  { owner: "zed", name: "tool", rootCommit: "z1", cloneOnline: true, forkedFrom: "nobody/tool" },
];
api.state.filteredGroups = null;
api.state.homeOrganizationRepositories = [];
api.state.homeRepositoryViewsSignature = "seen";
api.state.homeRepositoryViews = {
  "bob/widget": { views: 2 }, "carol/widget": { views: 9 },
  "dave/widget": { views: 5 }, "erin/gizmo": { views: 1 },
};
"""


def run(body):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + EXPORTS + " };")
    assert transformed != DASHBOARD_JS
    script = ("const SOURCE = " + json.dumps(transformed) + ";\n" + STUBS
              + "(async () => {\n" + body
              + "\nconsole.log(JSON.stringify(out));\n})();")
    result = subprocess.run(["node"], input=script, check=True,
                            capture_output=True, text=True)
    assert result.stderr == ""
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_forks_nest_under_their_parent_ordered_by_views():
    out = run("""
const views = (entry) => api.state.homeRepositoryViews[`${entry.repo.owner}/${entry.repo.name}`]?.views;
out.tree = api.nestRepositoryForks(api.homeRepositoryEntries(), views).map((group) => ({
  top: `${group.entry.repo.owner}/${group.entry.repo.name}`,
  forks: group.forks.map((fork) => `${fork.repo.owner}/${fork.repo.name}`),
}));
""")
    tree = {group["top"]: group["forks"] for group in out["tree"]}
    assert set(tree) == {"alice/widget", "zed/tool"}
    assert tree["alice/widget"] == [
        "carol/widget", "dave/widget", "bob/widget", "erin/gizmo",
        "frank/widget"]
    assert tree["zed/tool"] == []


def test_the_home_list_shows_three_forks_then_a_toggle_that_expands_inline():
    out = run("""
api.renderHomeRepositories();
out.collapsed = home.innerHTML;
const toggle = out.collapsed.match(/data-repo-forks-toggle="([^"]+)"/)[1];
out.toggled = api.toggleRepositoryForks({ dataset: { repoForksToggle: toggle } });
api.renderHomeRepositories();
out.expanded = home.innerHTML;
""")
    collapsed = out["collapsed"]
    assert collapsed.count("data-home-repo-group") == 1
    assert collapsed.count("data-home-repo-fork ") == 3
    assert "+2 more forks" in collapsed
    assert collapsed.count("data-home-repo-row") == 2
    group = collapsed[collapsed.index("data-home-repo-group"):]
    owners = re.findall(r'<span class="min-w-0 flex-1 truncate text-foreground">([^<]+)</span>', group)
    assert owners == ["carol", "dave", "bob"]
    assert "<a" not in re.sub(r"<a [^>]*>.*?</a>", "", group, flags=re.S)
    assert out["toggled"] is True
    expanded = out["expanded"]
    assert expanded.count("data-home-repo-fork ") == 5
    assert "erin/gizmo" in expanded and "Show fewer forks" in expanded
    assert 'aria-expanded="true"' in expanded


def test_fork_rows_ask_for_counts_of_public_forks_only():
    out = run("""
api.renderHomeRepositories();
out.html = home.innerHTML;
api.state.repositoryForksExpanded = new Set([out.html.match(/data-repo-forks-toggle="([^"]+)"/)[1]]);
api.renderHomeRepositories();
out.expanded = home.innerHTML;
""")
    keys = re.findall(r'data-repo-fork-counts="([^"]+)"', out["expanded"])
    assert keys == ["carol/widget", "dave/widget", "bob/widget", "erin/gizmo"]
    assert 'data-repo-fork-parent="alice/widget"' in out["expanded"]


def test_counts_load_once_fill_the_badge_and_stay_hidden_when_unknown():
    out = run("""
countsReply = {
  "carol/widget": { ahead: 3, behind: 12, parent: "alice/widget" },
  "dave/widget": { ahead: 0, behind: 0, parent: "alice/widget" },
  "bob/widget": { ahead: 1, behind: 0, parent: "alice/widget" },
  "erin/gizmo": { ahead: "x", behind: 2 },
};
countNodes = ["carol/widget", "dave/widget", "bob/widget", "erin/gizmo", "zed/tool"]
  .map((key) => countNode(key, "alice/widget"));
await api.hydrateRepositoryForkCounts(document);
out.shown = countNodes.map((node) => [node.textContent, node.attrs.title || "", node.classes.has("hidden")]);
out.calls = calls.filter((call) => call.url === "/api/repo-fork-counts").map((call) => call.body);
await api.hydrateRepositoryForkCounts(document);
out.again = calls.filter((call) => call.url === "/api/repo-fork-counts").length;
""")
    assert out["calls"] == [{"forks": [
        "carol/widget", "dave/widget", "bob/widget", "erin/gizmo", "zed/tool"]}]
    assert out["again"] == 1
    assert out["shown"] == [
        ["↑3 ↓12", "3 commits ahead, 12 behind alice/widget", False],
        ["", "", True],
        ["↑1 ↓0", "1 commit ahead, 0 behind alice/widget", False],
        ["", "", True],
        ["", "", True],
    ]


def test_the_sidebar_nests_forks_with_its_own_compact_rows():
    out = run("""
api.renderSidebarRepositories(api.state.session);
out.html = sidebar.innerHTML;
out.count = sidebarCount.textContent;
""")
    html = out["html"]
    assert html.count("data-sidebar-repo-group") == 1
    assert html.count("data-sidebar-repo-fork ") == 3
    assert "dashboard-nav-repo-forks-toggle" in html and "+2 more forks" in html
    assert 'data-repo-fork-counts="carol/widget"' in html
    assert ">zed/<" in html


def test_toggles_rerender_both_lists_and_the_styles_and_docs_exist():
    handler = NETWORK[NETWORK.index('const toggle = event.target?.closest?.("[data-repo-forks-toggle]");'):]
    handler = handler[:handler.index("});")]
    assert "if (!toggleRepositoryForks(toggle)) return;" in handler
    assert 'toggle.closest("[data-home-top-repositories]")) renderHomeRepositories();' in handler
    assert 'toggle.closest("[data-sidebar-repo-list]")) renderSidebarRepositories(state.session);' in handler
    assert 'fetch("/api/repo-fork-counts"' in EXPLORER
    for selector in (".dashboard-nav-repo-forks {", ".dashboard-nav-repo-fork,",
                     ".dashboard-nav-repo-fork-counts {"):
        assert selector in SHELL
    assert "/api/repo-fork-counts" in DOCS and "counts=1" in DOCS

import json
import re
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
BOOT_JS = (JS / "10-boot.js").read_text(encoding="utf-8")
EXPLORER = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"
EXPORTS = (
    "repoDisplayName, repoReferenceLabel, repoPublicText, repoHasShortAddress, "
    "groupDisplayName, groupRepositories, renderSidebarRepositories, "
    "applyRepoAddresses, repoAddresses, repoAddressesKnown, repoPathUrl, "
    "refreshRepoLinks, replaceLongRepoLocation, refreshRepoAddressViews, state")
DOC = {"owner": "acme", "short": True, "canonical": True, "keep": [],
       "reserved": ["api", "dashboard", "docs"],
       "aliases": {"repo": "alice", "forkmesh": "forkmesh"}}
STUBS = """
const vm = require("vm");
const store = new Map(STORE);
const sidebar = { innerHTML: "" };
const sidebarCount = { textContent: "" };
const events = [];
const replaced = [];
const anchors = LINKS.map((href) => {
  const attrs = { href };
  return { getAttribute: (k) => attrs[k], setAttribute: (k, v) => { attrs[k] = v; } };
});
global.location = {
  origin: "https://code.example.org",
  hostname: "code.example.org",
  pathname: PATH,
  search: "",
  hash: "",
};
global.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, String(v)),
  removeItem: (k) => store.delete(k),
};
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList, dataset: { page: "home" } },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  title: "Repository - ForkMesh",
  querySelector(sel) {
    if (sel === "[data-sidebar-repo-list]") return sidebar;
    if (sel === "[data-sidebar-repo-count]") return sidebarCount;
    return null;
  },
  querySelectorAll(sel) { return sel === "a[href^='/']" ? anchors : []; },
  addEventListener() {},
};
global.CustomEvent = class { constructor(type) { this.type = type; } };
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  dispatchEvent(event) { events.push(event.type); return true; },
  history: {
    state: null,
    pushState() {},
    replaceState(_s, _t, url) { replaced.push(url); },
  },
  lucide: { createIcons() {} },
  setTimeout() {},
  setInterval() {},
  clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const api = global.__dashboardExports;
const out = {};
"""


def run(body, store=None, links=(), path="/dashboard"):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + EXPORTS + " };")
    assert transformed != DASHBOARD_JS
    stubs = (STUBS.replace("STORE", json.dumps(list((store or {}).items())))
             .replace("LINKS", json.dumps(list(links)))
             .replace("PATH", json.dumps(path)))
    script = ("const SOURCE = " + json.dumps(transformed) + ";\n" + stubs
              + body + "\nconsole.log(JSON.stringify(out));")
    result = subprocess.run(["node"], input=script, check=True,
                            capture_output=True, text=True)
    assert result.stderr == ""
    return json.loads(result.stdout.strip().splitlines()[-1])


def _text(html):
    return re.sub(r"<[^>]+>", "", html)


def fresh(doc):
    return {"forkmesh.repoAddresses": json.dumps(doc),
            "forkmesh.repoAddressesSavedAt": "NOW"}


def stored(doc, age_ms=0):
    return {"forkmesh.repoAddresses": json.dumps(doc),
            "forkmesh.repoAddressesSavedAt": "AGE:%d" % age_ms}


SEED = """
for (const [k, v] of store) {
  if (v === "NOW") store.set(k, String(Date.now()));
  if (String(v).startsWith("AGE:")) store.set(k, String(Date.now() - Number(String(v).slice(4))));
}
"""

REPOS = """
api.state.repositories = [
  { owner: "alice", name: "Repo", rootCommit: "r1", cloneOnline: true },
  { owner: "forkmesh", name: "forkmesh", rootCommit: "f1", cloneOnline: true },
  { owner: "bob", name: "tools", rootCommit: "t1", cloneOnline: true },
];
api.state.filteredGroups = null;
api.state.homeOrganizationRepositories = [
  { owner: "acme", name: "Repo", organizationOwned: true, cloneOnline: true },
];
"""


def test_canonical_labels_name_only_the_repository():
    out = run(SEED + """
out.creator = api.repoDisplayName({ owner: "alice", name: "Repo" });
out.org = api.repoDisplayName({ owner: "acme", name: "Repo" });
out.node = api.repoDisplayName({ owner: "forkmesh", name: "forkmesh" });
out.other = api.repoDisplayName({ owner: "bob", name: "tools" });
out.otherWithFallback = api.repoDisplayName({ owner: "bob", name: "tools" }, "bob/tools");
out.key = api.repoDisplayName("acme/Repo");
out.ref = api.repoReferenceLabel({ owner: "acme", name: "Repo" }, 12);
out.text = api.repoPublicText("Bounty funded on acme/Repo#12, see bob/tools and docs/README.md");
out.short = api.repoHasShortAddress({ owner: "alice", name: "Repo" });
""", store=fresh(DOC))
    assert out["creator"] == "Repo" and out["org"] == "Repo"
    assert out["node"] == "forkmesh"
    assert out["other"] == "bob/tools"
    assert out["otherWithFallback"] == "bob/tools"
    assert out["key"] == "Repo"
    assert out["ref"] == "Repo#12"
    assert out["text"] == "Bounty funded on Repo#12, see bob/tools and docs/README.md"
    assert out["short"] is True


def test_labels_keep_the_owner_without_canonical_mode():
    out = run(SEED + """
out.creator = api.repoDisplayName({ owner: "alice", name: "Repo" });
out.org = api.repoDisplayName({ owner: "acme", name: "Repo" });
out.fallback = api.repoDisplayName({ owner: "acme", name: "Repo" }, "acme/Repo");
out.key = api.repoDisplayName("acme/Repo");
out.ref = api.repoReferenceLabel({ owner: "acme", name: "Repo" }, 12);
out.text = api.repoPublicText("Bounty funded on acme/Repo#12");
""", store=fresh({"owner": "acme", "short": True, "canonical": False}))
    assert out["creator"] == "alice/Repo"
    assert out["org"] == "acme/Repo" and out["fallback"] == "acme/Repo"
    assert out["key"] == "acme/Repo"
    assert out["ref"] == "acme/Repo#12"
    assert out["text"] == "Bounty funded on acme/Repo#12"


def test_the_sidebar_lists_each_repository_once_by_name():
    out = run(SEED + REPOS + """
api.renderSidebarRepositories(api.state.session);
out.html = sidebar.innerHTML;
out.count = sidebarCount.textContent;
out.hrefs = [...sidebar.innerHTML.matchAll(/href="([^"]+)"/g)].map((m) => m[1]);
out.labels = [...sidebar.innerHTML.matchAll(/data-sidebar-repo="([^"]+)"/g)].map((m) => m[1]);
""", store=fresh(DOC))
    assert sorted(out["labels"]) == ["Repo", "bob/tools", "forkmesh"]
    assert sorted(out["hrefs"]) == ["/Repo", "/bob/tools", "/forkmesh"]
    text = _text(out["html"])
    assert "alice/" not in text and "acme/" not in text
    assert "forkmesh/" not in text
    assert "bob/tools" in text
    assert "data-issue-person" not in out["html"]
    assert out["count"] == "3"


def test_the_sidebar_is_unchanged_without_canonical_mode():
    out = run(SEED + REPOS + """
api.renderSidebarRepositories(api.state.session);
out.html = sidebar.innerHTML;
out.hrefs = [...sidebar.innerHTML.matchAll(/href="([^"]+)"/g)].map((m) => m[1]);
""", store=fresh({"owner": "acme", "short": True, "canonical": False}))
    assert "/acme/Repo" in out["hrefs"] and "/alice/Repo" in out["hrefs"]
    assert "alice/Repo" in _text(out["html"]) and "acme/Repo" in _text(out["html"])


def test_changed_addresses_rerender_links_and_the_address_bar():
    out = run(SEED + REPOS + """
out.before = api.repoPathUrl({ owner: "alice", name: "Repo" });
api.renderSidebarRepositories(api.state.session);
out.sidebarBefore = sidebar.innerHTML.includes('href="/alice/Repo"');
api.applyRepoAddresses(DOC);
out.events = events.slice();
out.changed = api.state.repoAddressesChanged === true;
api.refreshRepoAddressViews();
out.sidebarAfter = [...sidebar.innerHTML.matchAll(/href="([^"]+)"/g)].map((m) => m[1]);
out.links = anchors.map((a) => a.getAttribute("href"));
out.replaced = replaced.slice();
""".replace("DOC", json.dumps(DOC)),
        store=fresh({"owner": "acme", "short": True, "canonical": False}),
        links=["/alice/Repo", "/alice/Repo/issues/3?x=1", "/bob/tools",
               "/dashboard/repos", "/@alice"],
        path="/alice/Repo/issues")
    assert out["before"] == "/alice/Repo" and out["sidebarBefore"] is True
    assert out["events"] == ["forkmesh:repo-addresses"] and out["changed"]
    assert "/Repo" in out["sidebarAfter"] and "/alice/Repo" not in out["sidebarAfter"]
    assert out["links"] == ["/Repo", "/Repo/issues/3?x=1", "/bob/tools",
                            "/dashboard/repos", "/@alice"]
    assert out["replaced"] == ["/Repo/issues"]


def test_turning_canonical_off_restores_long_links():
    out = run(SEED + """
api.repoPathUrl({ owner: "acme", name: "Repo" });
api.applyRepoAddresses({ owner: "acme", short: true, canonical: false });
api.refreshRepoLinks(document);
api.replaceLongRepoLocation();
out.links = anchors.map((a) => a.getAttribute("href"));
out.replaced = replaced.slice();
""", store=fresh(DOC), links=["/Repo", "/Repo/pulls/2"], path="/Repo/issues")
    assert out["links"] == ["/acme/Repo", "/acme/Repo/pulls/2"]
    assert out["replaced"] == ["/acme/Repo/issues"]


def test_an_unchanged_answer_triggers_no_rerender():
    out = run(SEED + """
api.repoPathUrl({ owner: "acme", name: "Repo" });
api.applyRepoAddresses(DOC);
out.events = events.slice();
""".replace("DOC", json.dumps(DOC)), store=fresh(DOC))
    assert out["events"] == []


def test_the_cached_answer_expires_so_boot_waits_again():
    young = run(SEED + "out.known = api.repoAddressesKnown();",
                store=stored(DOC, 60 * 1000))
    old = run(SEED + "out.known = api.repoAddressesKnown();",
              store=stored(DOC, 11 * 60 * 1000))
    legacy = run("out.known = api.repoAddressesKnown();",
                 store={"forkmesh.repoAddresses": json.dumps(DOC)})
    assert young["known"] is True
    assert old["known"] is False
    assert legacy["known"] is False


def test_boot_listens_for_changed_addresses_before_the_first_render():
    assert BOOT_JS.index(
        'window.addEventListener("forkmesh:repo-addresses", refreshRepoAddressViews);'
    ) < BOOT_JS.index("const repoAddressesLoaded = loadRepoAddresses();")
    assert "Promise.race([repoAddressesLoaded" in BOOT_JS
    assert "1500" in BOOT_JS


def test_lists_use_the_one_label_helper():
    for needle in ("groupDisplayName(group)", "repoLabelParts(entry.key)",
                   "repoPublicText(item.title)"):
        assert needle in EXPLORER
    assert "repoDisplayOwner(repo) || \"owner\"" not in EXPLORER
    assert "function repoDisplayName(" in HELPERS

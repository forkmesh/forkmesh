#!/usr/bin/env python3
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
FRAGMENT = JS_DIR / "09-recent-pushes.js"
HELPERS = JS_DIR / "02-helpers.js"
BOOT = JS_DIR / "10-boot.js"
MAIN = ROOT / "public" / "dashboard" / "partials" / "main.html"
NOW = 1_800_000_000_000


def _escape_html_source():
    text = HELPERS.read_text(encoding="utf-8")
    start = text.index("  function escapeHtml(")
    end = text.index("\n  }\n", start) + 4
    return text[start:end]


PRELUDE = r"""
const NOW = %d;
Date.now = () => NOW;
const store = new Map();
globalThis.localStorage = {
  getItem: (key) => (store.has(key) ? store.get(key) : null),
  setItem: (key, value) => { store.set(key, String(value)); },
  removeItem: (key) => { store.delete(key); },
};
globalThis.location = { pathname: "/dashboard", protocol: "https:" };
const state = { session: null };
let page = "home";
function currentPage() { return page; }
function repoKey(repo) { return `${repo.owner}/${repo.name}`; }
function repoDisplayKey(repo) { return `${repo.owner}/${repo.name}`; }
function sessionHeaders() { return { accept: "application/json" }; }
const calls = [];
let reply = { ok: true, windowMinutes: 60, pushes: [] };
globalThis.fetch = async (url, init = {}) => {
  calls.push({ url, method: init.method || "GET", body: init.body || "" });
  return { ok: true, json: async () => reply };
};
let idleCallback = null;
globalThis.window = {
  lucide: { createIcons() {} },
  requestIdleCallback: (callback) => { idleCallback = callback; },
  setTimeout,
};
const slot = {
  hidden: true, innerHTML: "", dataset: {}, listeners: {},
  addEventListener(name, handler) { this.listeners[name] = handler; },
};
function $(selector) { return selector === "[data-recent-pushes]" ? slot : null; }
function click(key) {
  slot.listeners.click({ target: { closest: (selector) => (
    selector === "[data-recent-push-dismiss]" ? { dataset: { recentPushDismiss: key } } : null) } });
}
function push(branch, minutesAgo, extra = {}) {
  return {
    id: `r1:${branch}`, repo: "alice/app", repoUrl: "/alice/app", branch,
    branchUrl: `/alice/app/tree/${branch}`, head: "a".repeat(40),
    pushedAt: NOW - minutesAgo * 60000, base: "main", fork: false,
    target: "alice/app", targetUrl: "/alice/app",
    compareUrl: `/alice/app/compare/main...${branch}`, ...extra,
  };
}
async function flush() {
  if (idleCallback) { const run = idleCallback; idleCallback = null; run(); }
  for (let i = 0; i < 5; i += 1) await new Promise((resolve) => setTimeout(resolve, 0));
}
function signIn(name = "alice") { state.session = { nodeName: name, sessionToken: "cookie" }; }
function banners() {
  return [...slot.innerHTML.matchAll(/data-recent-push="([^"]*)"/g)].map((match) => match[1]);
}
""" % NOW


def _run(body):
    script = (
        _escape_html_source() + PRELUDE + FRAGMENT.read_text(encoding="utf-8")
        + "\nconst out = await (async () => {\n" + body + "\n})();\n"
        + "console.log(JSON.stringify(out));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_signed_out_renders_nothing_and_never_fetches():
    out = _run("""
      localStorage.setItem("forkmesh.recentPushes", JSON.stringify({ account: "alice", at: NOW, items: [push("x", 1)] }));
      initRecentPushes();
      await flush();
      return { hidden: slot.hidden, html: slot.innerHTML, calls, cache: localStorage.getItem("forkmesh.recentPushes") };
    """)
    assert out == {"hidden": True, "html": "", "calls": [], "cache": None}


def test_the_banner_renders_newest_first_up_to_three_and_escapes():
    out = _run("""
      signIn();
      reply.pushes = [push("one", 50), push("two", 2), push("<img src=x onerror=alert(1)>", 5), push("four", 1), push("stale", 61)];
      initRecentPushes();
      await flush();
      return { hidden: slot.hidden, html: slot.innerHTML, calls: calls.map((call) => call.url), keys: banners() };
    """)
    assert out["hidden"] is False
    assert out["calls"] == ["/api/me/recent-pushes"]
    assert [key.split("@")[0] for key in out["keys"]] == ["r1:four", "r1:two", "r1:&lt;img src=x onerror=alert(1)&gt;"]
    html = out["html"]
    assert "<img" not in html
    assert "stale" not in html and "one" not in re.sub(r"<[^>]+>", " ", html).split()
    assert 'href="/alice/app/compare/main...four?expand=1"' in html
    assert "Compare &amp; pull request" in html
    assert "1 minute ago" in html and "2 minutes ago" in html
    assert "fm-flash-warning" in html
    assert "fm-btn-cta" not in html
    assert "onclick" not in html.lower()


def test_dismiss_is_remembered_per_branch_and_head():
    out = _run("""
      signIn();
      reply.pushes = [push("one", 3), push("two", 2)];
      initRecentPushes();
      await flush();
      const before = banners();
      click(before[0]);
      const afterDismiss = banners();
      const dismissed = Object.keys(JSON.parse(localStorage.getItem("forkmesh.recentPushes.dismissed")));
      reply.pushes = [push("one", 3), push("two", 1, { head: "b".repeat(40) })];
      await loadRecentPushes();
      return { before, afterDismiss, dismissed, afterNewPush: banners() };
    """)
    assert [key.split("@")[0] for key in out["before"]] == ["r1:two", "r1:one"]
    assert [key.split("@")[0] for key in out["afterDismiss"]] == ["r1:one"]
    assert out["dismissed"] == ["r1:two@" + "a" * 40]
    assert sorted(key.split("@")[1][:1] for key in out["afterNewPush"]) == ["a", "b"]


def test_a_fresh_cache_paints_without_a_request_and_storage_errors_are_ignored():
    out = _run("""
      signIn();
      localStorage.setItem("forkmesh.recentPushes", JSON.stringify({ account: "alice", at: NOW - 1000, items: [push("cached", 4)] }));
      initRecentPushes();
      await flush();
      const cached = { keys: banners(), calls: calls.length };
      localStorage.getItem = () => { throw new Error("blocked"); };
      localStorage.setItem = () => { throw new Error("blocked"); };
      click(cached.keys[0]);
      return { cached, after: banners() };
    """)
    assert [key.split("@")[0] for key in out["cached"]["keys"]] == ["r1:cached"]
    assert out["cached"]["calls"] == 0
    assert [key.split("@")[0] for key in out["after"]] == ["r1:cached"]


def test_another_accounts_cache_is_ignored():
    out = _run("""
      signIn("bob");
      localStorage.setItem("forkmesh.recentPushes", JSON.stringify({ account: "alice", at: NOW, items: [push("theirs", 1)] }));
      initRecentPushes();
      const painted = banners();
      await flush();
      return { painted, calls: calls.length, after: banners() };
    """)
    assert out == {"painted": [], "calls": 1, "after": []}


def test_repo_pages_show_only_that_repos_or_its_parents_branches():
    out = _run("""
      signIn();
      page = "repo";
      location.pathname = "/upstream/lib/pulls";
      reply.pushes = [
        push("here", 1),
        push("fork", 2, { id: "r2:fork", repo: "alice/lib", repoUrl: "/alice/lib", target: "upstream/lib", targetUrl: "/upstream/lib", fork: true, compareUrl: "/alice/lib/compare/main...fork" }),
      ];
      initRecentPushes();
      await flush();
      const onParent = { keys: banners(), html: slot.innerHTML };
      location.pathname = "/alice/lib/compare/main...fork";
      renderRecentPushes();
      const onCompare = banners();
      page = "settings";
      renderRecentPushes();
      return { onParent, onCompare, onSettings: banners(), hidden: slot.hidden };
    """)
    assert [key.split("@")[0] for key in out["onParent"]["keys"]] == ["r2:fork"]
    assert "alice/lib" in out["onParent"]["html"]
    assert out["onCompare"] == []
    assert out["onSettings"] == [] and out["hidden"] is True


def test_an_opened_pull_hides_the_branch_and_tells_the_server():
    out = _run("""
      signIn();
      reply.pushes = [push("one", 3), push("two", 2), push("fork", 1, { id: "r2:fork", repo: "alice/lib", target: "upstream/lib" })];
      initRecentPushes();
      await flush();
      noteRecentPushPullOpened({ owner: "alice", name: "app" }, "one");
      noteRecentPushOpenPulls({ owner: "upstream", name: "lib" }, [{ state: "open", head: "alice:fork" }, { state: "closed", head: "two" }]);
      noteRecentPushOpenPulls({ owner: "alice", name: "app" }, [{ state: "closed", head: "two" }]);
      await flush();
      return { keys: banners(), posts: calls.filter((call) => call.method === "POST").map((call) => JSON.parse(call.body).id) };
    """)
    assert [key.split("@")[0] for key in out["keys"]] == ["r1:two"]
    assert out["posts"] == ["r1:one", "r2:fork"]


def test_the_banner_is_wired_into_every_dashboard_page():
    assert '<div data-recent-pushes hidden class="flex-none"></div>' in MAIN.read_text(encoding="utf-8")
    assert "initRecentPushes();" in BOOT.read_text(encoding="utf-8")
    built = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert "function initRecentPushes()" in built
    for page in ("dashboard/index.html", "dashboard/repo.html", "dashboard/repos/index.html"):
        assert "data-recent-pushes" in (ROOT / "public" / page).read_text(encoding="utf-8")

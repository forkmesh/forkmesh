import subprocess
from pathlib import Path

JS = Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")

HELPER = CONTENT[
    CONTENT.index("  const REPO_RECONNECT_DELAYS_MS")
    : CONTENT.index("  // Reads the node assembled for a pull request")
]
FETCH = CONTENT[
    CONTENT.index("  async function fetchRepoJson(")
    : CONTENT.index("  const REPO_RECONNECT_DELAYS_MS")
]

PRELUDE = r"""
const assert = require("assert");
let now = 1000000;
Date.now = () => now;
const timers = new Map();
let nextTimer = 1;
const cleared = [];
const window = {
  setTimeout(callback, delay) { const id = nextTimer++; timers.set(id, { callback, delay }); return id; },
  clearTimeout(id) { cleared.push(id); timers.delete(id); },
};
const location = { pathname: "/acme/site/pulls/3", origin: "https://code.example.com" };
const store = new Map();
let storageBroken = false;
const sessionStorage = {
  getItem(key) { if (storageBroken) throw new Error("denied"); return store.has(key) ? store.get(key) : null; },
  setItem(key, value) { if (storageBroken) throw new Error("denied"); store.set(key, String(value)); },
  removeItem(key) { store.delete(key); },
};
const state = { session: { nodeName: "Ada" } };
function repoKey(repo) { return `${repo.owner}/${repo.name}`; }
function escapeHtml(value) { return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;"); }
function loadingHtml(label) { return `<span class="fm-spinner"></span>${label}`; }
function makeContainer() {
  const container = {
    innerHTML: "",
    isConnected: true,
    listeners: [],
    querySelector(selector) {
      if (selector.includes("data-repo-reconnect-note") && this.innerHTML.includes("data-repo-reconnect-note")) {
        return { remove: () => { this.innerHTML = this.innerHTML.replace(/<div data-repo-reconnect-note[^>]*>.*?<\/div>/, ""); }, setAttribute() {} };
      }
      return null;
    },
    querySelectorAll(selector) {
      if (!this.innerHTML.includes("data-repo-reconnect-retry")) return [];
      return [{ addEventListener: (type, handler) => this.listeners.push(handler) }];
    },
    insertAdjacentHTML(position, html) { this.innerHTML = html + this.innerHTML; },
  };
  return container;
}
function runDue() {
  const due = Array.from(timers.entries());
  timers.clear();
  due.forEach(([, timer]) => { now += timer.delay; timer.callback(); });
  return due.map(([, timer]) => timer.delay);
}
"""


def run_node(body):
    script = PRELUDE + HELPER + FETCH + body
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr + result.stdout


def test_only_outage_answers_count_as_transient():
    run_node(r"""
assert.equal(repoReadIsTransient({ status: 503, code: "mirror_unavailable" }), true);
assert.equal(repoReadIsTransient({ status: 503, code: "" }), true);
assert.equal(repoReadIsTransient({ status: 502 }), true);
assert.equal(repoReadIsTransient({ status: 504 }), true);
assert.equal(repoReadIsTransient(Object.assign(new TypeError("Failed to fetch"), { network: true })), true);
assert.equal(repoReadIsTransient({ status: 404, code: "not_found" }), false);
assert.equal(repoReadIsTransient({ status: 503, code: "maintenance" }), false);
assert.equal(repoReadIsTransient(new Error("render bug")), false);
assert.equal(repoReadIsTransient(null), false);
""")


def test_backoff_retries_for_two_minutes_then_offers_retry():
    run_node(r"""
const container = makeContainer();
const repo = { owner: "acme", name: "site" };
const key = repoViewKey(repo, "pulls/3");
let retries = 0;
const outage = { status: 503, code: "mirror_unavailable" };
const reconnect = { key, container, retry: () => { retries += 1; handleRepoReadFailure(outage, reconnect); } };
assert.equal(handleRepoReadFailure(outage, reconnect), true);
assert.ok(container.innerHTML.includes("Reconnecting to the repository host…"));
assert.ok(!container.innerHTML.includes("unavailable until"));
assert.equal(repoReconnectActive(key), true);
const delays = [];
for (let step = 0; step < 12 && timers.size; step += 1) delays.push(...runDue());
assert.deepEqual(delays, [2000, 4000, 8000, 16000, 30000, 30000, 30000]);
assert.equal(retries, 7);
assert.equal(timers.size, 0);
assert.equal(repoReconnectActive(key), false);
assert.ok(container.innerHTML.includes("The repository host is not answering right now."));
assert.ok(container.innerHTML.includes("data-repo-reconnect-retry"));
assert.equal(container.listeners.length, 1);
container.listeners[0]();
assert.equal(retries, 8);
assert.equal(timers.size, 1);
assert.ok(container.innerHTML.includes("Reconnecting to the repository host…"));
""")


def test_last_good_view_stays_visible_with_a_note():
    run_node(r"""
const container = makeContainer();
const repo = { owner: "acme", name: "site" };
const key = repoViewKey(repo, "pulls");
container.innerHTML = "<ul><li>#3 Fix login</li></ul>";
finishRepoRead(repoStaleReadMark(), { key, container, retry() {} });
assert.equal(timers.size, 0);
container.innerHTML = "loading";
assert.equal(handleRepoReadFailure({ network: true }, { key, container, retry() {} }), true);
assert.ok(container.innerHTML.includes("<li>#3 Fix login</li>"));
assert.ok(container.innerHTML.includes("Reconnecting to the repository host…"));
const other = repoViewKey(repo, "issues");
storageBroken = true;
const plain = makeContainer();
assert.equal(handleRepoReadFailure({ status: 502 }, { key: other, container: plain, retry() {} }), true);
assert.ok(plain.innerHTML.includes("Reconnecting to the repository host…"));
saveRepoViewSnapshot(other, "<p>x</p>");
storageBroken = false;
assert.equal(loadRepoViewSnapshot(other), "");
state.session = { nodeName: "Grace" };
assert.notEqual(repoViewKey(repo, "pulls"), key);
""")


def test_real_absence_and_other_errors_are_left_to_the_caller():
    run_node(r"""
const container = makeContainer();
container.innerHTML = "before";
const key = "k";
assert.equal(handleRepoReadFailure({ status: 404, code: "not_found" }, { key, container, retry() {} }), false);
assert.equal(container.innerHTML, "before");
assert.equal(timers.size, 0);
""")


def test_route_change_cancels_pending_retries():
    run_node(r"""
const container = makeContainer();
let retries = 0;
handleRepoReadFailure({ status: 504 }, { key: "a", container, retry: () => { retries += 1; } });
location.pathname = "/acme/site/issues";
runDue();
assert.equal(retries, 0);
assert.equal(repoReconnectActive("a"), false);
location.pathname = "/acme/site/pulls/3";
handleRepoReadFailure({ status: 504 }, { key: "b", container, retry: () => { retries += 1; } });
handleRepoReadFailure({ status: 504 }, { key: "c", container, retry: () => { retries += 1; } });
assert.equal(timers.size, 2);
stopRepoReconnects();
assert.equal(timers.size, 0);
assert.equal(repoReconnectActive("b") || repoReconnectActive("c"), false);
const detached = makeContainer();
handleRepoReadFailure({ status: 504 }, { key: "d", container: detached, retry: () => { retries += 1; } });
detached.isConnected = false;
runDue();
assert.equal(retries, 0);
""")


def test_stale_answer_shows_updating_and_refreshes():
    run_node(r"""
const container = makeContainer();
const mark = repoStaleReadMark();
repoReconnect.staleReads += 1;
container.innerHTML = "<ul><li>#3</li></ul>";
let retries = 0;
finishRepoRead(mark, { key: "s", container, retry: () => { retries += 1; } });
assert.ok(container.innerHTML.includes("Updating…"));
assert.equal(timers.size, 1);
runDue();
assert.equal(retries, 1);
container.innerHTML = "<ul><li>#3</li></ul>";
finishRepoRead(repoStaleReadMark(), { key: "s", container, retry() {} });
assert.equal(repoReconnectActive("s"), false);
assert.ok(!container.innerHTML.includes("Updating…"));
""")


def test_fetch_marks_network_failures_and_stale_answers():
    run_node(r"""
async function seededRepoRead() { return null; }
async function earlyRepoRead() { return null; }
function directRepoRequestCandidates() { return []; }
let mode = "network";
global.fetch = async () => {
  if (mode === "network") throw new TypeError("Failed to fetch");
  return {
    ok: true, status: 200,
    headers: { get: (name) => (name === "X-ForkMesh-Stale" ? "1" : "") },
    json: async () => ({ ok: true, entries: [] }),
  };
};
(async () => {
  await assert.rejects(fetchRepoJson("/api/repo/acme/site/tree"), (error) => repoReadIsTransient(error));
  mode = "stale";
  const before = repoStaleReadMark();
  await fetchRepoJson("/api/repo/acme/site/tree");
  assert.equal(repoStaleReadMark(), before + 1);
})().catch((error) => { console.error(error); process.exit(1); });
""")


def test_views_use_the_shared_helper_instead_of_permanent_outage_text():
    for text in (CONTENT, COMPOSE):
        assert "is unavailable until a reachable mirror host serves ${" not in text
        assert "are unavailable until a reachable mirror host serves the ${" not in text
        assert "Releases are unavailable until" not in text
        assert "Projects are unavailable until" not in text
    assert CONTENT.count("handleRepoReadFailure(error, reconnect)") >= 5
    assert "retry: () => loadRepoProjects(repo)" in CONTENT
    assert COMPOSE.count("handleRepoReadFailure(error, reconnect)") == 3
    assert "function handleRepoReadFailure" not in COMPOSE
    assert "stopRepoReconnects();" in HELPERS[HELPERS.index("function navigateHistory"):]
    assert "stopRepoReconnects();" in NETWORK[NETWORK.index("function initPageHistory"):]
    assert "Reconnecting to the repository host…" in CONTENT

#!/usr/bin/env python3
import json
import subprocess

from _dashboard_bundle import assembled_dashboard_js

DASHBOARD_JS = assembled_dashboard_js()
EXPORTS = [
    "state", "splitRepoCodeRef", "resolveRepoCodeRoute", "repoCodeLinkUrl",
    "repoRouteParts", "openRepoCodeRoute", "openRepoCodeRefFallback",
    "applyRepoAddresses", "repoKey",
]


def run(body, pathname="/alice/tools", meta=None):
    transformed = DASHBOARD_JS.replace(
        "  void bootDashboard();",
        "  globalThis.__dashboardExports = { %s };" % ", ".join(EXPORTS),
    )
    assert "__dashboardExports" in transformed
    script = "const SOURCE = " + json.dumps(transformed) + ";\n" + """
const assert = require("assert");
const vm = require("vm");
const META = """ + json.dumps(meta or {}) + """;
global.location = {
  origin: "https://forge.test", hostname: "forge.test", host: "forge.test",
  pathname: """ + json.dumps(pathname) + """, search: "", hash: "",
};
const replaced = [];
const pushed = [];
const fetched = [];
const browserStorage = new Map();
global.localStorage = {
  getItem(key) { return browserStorage.has(key) ? browserStorage.get(key) : null; },
  setItem(key, value) { browserStorage.set(key, String(value)); },
  removeItem(key) { browserStorage.delete(key); },
};
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector(selector) {
    const name = /meta\\[name="([^"]+)"\\]/.exec(selector);
    return name && META[name[1]] ? { content: META[name[1]] } : null;
  },
  querySelectorAll() { return []; },
  addEventListener() {},
};
function moveTo(url) {
  const [path, query = ""] = url.split("#")[0].split("?");
  location.pathname = path;
  location.search = query ? `?${query}` : "";
}
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  history: {
    state: null,
    pushState(_s, _t, url) { pushed.push(url); moveTo(url); },
    replaceState(_s, _t, url) { replaced.push(url); moveTo(url); },
  },
  lucide: { createIcons() {} },
  setTimeout() {},
  setInterval() {},
  clearInterval() {},
};
global.history = global.window.history;
global.navigator = {};
let answers = {};
global.fetch = async (url) => {
  fetched.push(String(url));
  const hit = Object.keys(answers).find((key) => String(url).includes(key));
  const body = hit ? answers[hit] : null;
  return {
    ok: Boolean(body),
    status: body ? 200 : 404,
    headers: { get() { return ""; } },
    async json() { return body || { ok: false, error: "not_found" }; },
  };
};
vm.runInThisContext(SOURCE);
const X = global.__dashboardExports;
const repo = { owner: "alice", name: "tools", defaultBranch: "main" };
X.state.selectedRepo = repo;
function listBranches(names, defaultBranch = "main") {
  X.state.repoBranches[X.repoKey(repo)] = {
    loaded: true, defaultBranch, branches: names.map((name) => ({ name })),
  };
}
(async () => {
""" + body + """
})().catch((error) => { console.error(error); process.exit(1); });
"""
    result = subprocess.run(["node"], input=script, capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stderr + result.stdout
    assert result.stderr == ""


def test_the_longest_known_ref_prefix_wins():
    run("""
const refs = ["main", "feature/x", "dev"];
assert.deepStrictEqual(X.splitRepoCodeRef("blob", ["main", "README.md"], refs),
  { ref: "main", path: "README.md" });
assert.deepStrictEqual(X.splitRepoCodeRef("tree", ["feature", "x", "src"], refs),
  { ref: "feature/x", path: "src" });
assert.deepStrictEqual(X.splitRepoCodeRef("tree", ["dev"], refs), { ref: "dev", path: "" });
assert.strictEqual(X.splitRepoCodeRef("blob", ["main"], refs), null);
assert.strictEqual(X.splitRepoCodeRef("blob", ["docs", "a.md"], refs), null);
assert.strictEqual(X.splitRepoCodeRef("tree", ["feature", "y"], refs), null);
""")


def test_the_default_branch_resolves_without_a_branch_listing():
    run("""
const hit = await X.resolveRepoCodeRoute(repo, "blob", "main/docs/README.md");
assert.deepStrictEqual(hit, { ref: "main", path: "docs/README.md", fallback: null });
assert.deepStrictEqual(fetched, []);
const plain = await X.resolveRepoCodeRoute(repo, "blob", "README.md");
assert.deepStrictEqual(plain, { ref: "", path: "README.md", fallback: null });
assert.deepStrictEqual(fetched, []);
""")


def test_slash_branches_and_ambiguous_folders():
    run("""
listBranches(["main", "feature/x"]);
assert.deepStrictEqual(await X.resolveRepoCodeRoute(repo, "tree", "feature/x/src/lib"),
  { ref: "feature/x", path: "src/lib", fallback: null });
assert.deepStrictEqual(await X.resolveRepoCodeRoute(repo, "tree", "main"),
  { ref: "main", path: "", fallback: null });
const folder = await X.resolveRepoCodeRoute(repo, "tree", "docs/guide");
assert.deepStrictEqual(folder, {
  ref: "", path: "docs/guide", fallback: { ref: "docs", path: "guide" } });
""")


def test_canonical_addresses_drop_the_default_ref_and_keep_others():
    run("""
listBranches(["main", "feature/x"]);
assert.strictEqual(X.repoCodeLinkUrl(repo, "blob", "README.md"), "/alice/tools/blob/README.md");
X.state.selectedBranches[X.repoKey(repo)] = "feature/x";
assert.strictEqual(X.repoCodeLinkUrl(repo, "blob", "src/a.py"),
  "/alice/tools/blob/src/a.py?ref=feature%2Fx");
assert.strictEqual(X.repoCodeLinkUrl(repo), "/alice/tools?ref=feature%2Fx");
""")


def test_opening_a_github_link_replaces_the_address():
    run("""
listBranches(["main", "feature/x"]);
location.pathname = "/alice/tools/blob/main/README.md";
await X.openRepoCodeRoute(repo, "blob", "main/README.md");
assert.deepStrictEqual(replaced, ["/alice/tools/blob/README.md"]);
assert.strictEqual(X.state.selectedBranches[X.repoKey(repo)] || "main", "main");
location.pathname = "/alice/tools/tree/feature/x/src";
await X.openRepoCodeRoute(repo, "tree", "feature/x/src");
assert.strictEqual(replaced.at(-1), "/alice/tools/tree/src?ref=feature%2Fx");
assert.strictEqual(X.state.selectedBranches[X.repoKey(repo)], "feature/x");
X.state.selectedBranches[X.repoKey(repo)] = "main";
location.pathname = "/alice/tools/blob/main/README.md";
location.search = "?ref=dev";
const before = replaced.length;
await X.openRepoCodeRoute(repo, "blob", "main/README.md", { explicitRef: true });
assert.strictEqual(replaced.length, before);
""", pathname="/alice/tools/blob/main/README.md")


def test_short_addresses_parse_and_canonicalise():
    run("""
X.applyRepoAddresses({ owner: "alice", short: true, canonical: true });
assert.deepStrictEqual(X.repoRouteParts(), ["alice", "tools", "blob", "main", "README.md"]);
listBranches(["main"]);
await X.openRepoCodeRoute(repo, "blob", "main/README.md");
assert.deepStrictEqual(replaced, ["/tools/blob/README.md"]);
""", pathname="/tools/blob/main/README.md",
        meta={"forkmesh-repo-short": "tools", "forkmesh-repo-owner": "alice",
              "forkmesh-repo-canonical": "alice"})


def test_unknown_first_segments_try_the_ref_only_when_it_resolves():
    run("""
listBranches(["main"]);
assert.strictEqual(await X.openRepoCodeRefFallback(repo, "blob", { ref: "nope", path: "README.md" }), false);
assert.strictEqual(X.state.selectedBranches[X.repoKey(repo)], undefined);
assert.deepStrictEqual(replaced, []);
answers = { "ref=v1.0": { ok: true, commit: "a".repeat(40), entries: [] } };
location.pathname = "/alice/tools/blob/v1.0/README.md";
assert.strictEqual(await X.openRepoCodeRefFallback(repo, "blob", { ref: "v1.0", path: "README.md" }), true);
assert.strictEqual(X.state.selectedBranches[X.repoKey(repo)], "v1.0");
assert.deepStrictEqual(replaced, ["/alice/tools/blob/README.md?ref=v1.0"]);
""")

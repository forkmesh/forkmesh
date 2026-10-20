"""A page asks for each shared public read once, and as early as it can.

* GET /api/instance: the dashboard helpers, instance-brand.js and
  genie-spider.js share one promise (window.__forkmeshInstanceInfo), which an
  instance page starts from its first script (tools/apply_instance_brand.py).
* The repository page asks for the organization's repository list with the
  HTML when the catalog does not hold owner/name (partials/repo-early-reads.html)
  and fetchJson takes that answer over once.
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import apply_instance_brand  # noqa: E402

PUBLIC = ROOT / "public"
HELPERS = (PUBLIC / "dashboard" / "js" / "02-helpers.js").read_text(encoding="utf-8")
EARLY_READS = (PUBLIC / "dashboard" / "partials" / "repo-early-reads.html").read_text(encoding="utf-8")
BRAND_JS = (PUBLIC / "instance-brand.js").read_text(encoding="utf-8")
GENIE_JS = (ROOT.parent / "www" / "public" / "genie-spider.js").read_text(encoding="utf-8")


def _node(script):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    out = subprocess.run([node, "-e", script], check=True, capture_output=True,
                         text=True, timeout=30)
    return json.loads(out.stdout.strip().splitlines()[-1])


def _function(name, source=HELPERS):
    start = source.index("  function %s(" % name) if ("  function %s(" % name) in source \
        else source.index("  async function %s(" % name)
    end = source.index("\n  }\n", start)
    return source[start:end + 4]


def _instance_helpers():
    start = HELPERS.index("  let instanceInfoRequest = null;")
    end = HELPERS.index("\n  }\n", HELPERS.index("  function instanceInfo("))
    return HELPERS[start:end + 4]


INSTANCE_PRELUDE = """
const calls = [];
globalThis.window = globalThis;
globalThis.fetch = async (path, options) => {
  calls.push([path, options && options.credentials]);
  await new Promise((resolve) => setImmediate(resolve));
  return { ok: true, json: async () => ({ ok: true, gitNode: "Flagship", repoAddresses: null }) };
};
const dashboardOnInstance = () => true;
"""


def test_every_dashboard_reader_shares_one_instance_request():
    result = _node(INSTANCE_PRELUDE + _instance_helpers() + """
Promise.all([instanceInfo(), sharedInstanceDocument(), instanceInfo(), sharedInstanceDocument()])
  .then((docs) => console.log(JSON.stringify({
    calls, same: docs.every((doc) => doc && doc.gitNode === "Flagship"), node: instanceGitNode })));
""")
    assert result == {"calls": [["/api/instance", "omit"]], "same": True, "node": "flagship"}


def test_the_early_instance_request_is_taken_over():
    early = re.search(r"<script data-instance-info-early>(.*?)</script>",
                      apply_instance_brand.INFO_EARLY, re.S).group(1)
    result = _node(INSTANCE_PRELUDE + early + "\n" + _instance_helpers() + """
Promise.all([instanceInfo(), sharedInstanceDocument()])
  .then(() => console.log(JSON.stringify({ calls })));
""")
    assert result == {"calls": [["/api/instance", "omit"]]}


def test_brand_and_genie_scripts_reuse_the_shared_instance_request():
    for source in (BRAND_JS, GENIE_JS):
        assert "window.__forkmeshInstanceInfo" in source
        assert source.count('fetch("/api/instance"') == 1
        guard = source.index("let shared = window.__forkmeshInstanceInfo;")
        assert source.index('fetch("/api/instance"') > guard
    assert 'fetch("/api/instance"' not in HELPERS.replace(
        _function("sharedInstanceDocument"), "")


def test_brand_tool_starts_the_instance_read_first_on_dashboard_pages(tmp_path):
    page = ('<!doctype html><html><head>\n<meta charset="UTF-8" />\n'
            '<script src="/api-client.js?v=1"></script>\n'
            '<script src="/dashboard.js?v=abc" defer></script>\n'
            '</head><body>ForkMesh</body></html>\n')
    other = '<!doctype html><html><head><script src="/x.js"></script></head><body></body></html>\n'
    (tmp_path / "dashboard.html").write_text(page)
    (tmp_path / "other.html").write_text(other)
    assert apply_instance_brand.apply(tmp_path, "Acme Code") == 2
    branded = (tmp_path / "dashboard.html").read_text()
    assert branded.count("data-instance-info-early") == 1
    assert branded.index("data-instance-info-early") < branded.index('src="/api-client.js')
    assert "data-instance-info-early" not in (tmp_path / "other.html").read_text()
    # Re-branding (an update, a rename) keeps exactly one copy.
    assert apply_instance_brand.apply(tmp_path, "Acme Code") == 0
    apply_instance_brand.apply(tmp_path, "Beta Git")
    assert (tmp_path / "dashboard.html").read_text().count("data-instance-info-early") == 1


def _fetch_json_source():
    start = HELPERS.index("  const FETCH_JSON_RETRY_DELAYS_MS")
    end = HELPERS.index("\n  }\n", HELPERS.index("async function fetchJson("))
    return HELPERS[start:end + 4]


def _fetch_json_run(session, body):
    script = """
const calls = [];
globalThis.setTimeout = (fn) => fn();
globalThis.fetch = async (path) => {
  calls.push(path);
  return { ok: true, status: 200, json: async () => ({ ok: true, repos: [{ repo: "live", node: "n" }] }) };
};
const state = { session: %s };
const cloneJson = (value) => JSON.parse(JSON.stringify(value));
const fetchJsonCacheTtl = () => 0;
const cacheBustedPath = (path) => path;
globalThis.__forkmeshEarly = { json: {
  "/api/orgs/acme/repos": Promise.resolve({ ok: true, repos: [{ repo: "early", node: "n" }] }),
} };
%s
%s
""" % (json.dumps(session), _fetch_json_source(), body)
    return _node(script)


def test_fetch_json_takes_the_early_org_read_over_once():
    result = _fetch_json_run(None, """
(async () => {
  const first = await fetchJson("/api/orgs/acme/repos");
  const second = await fetchJson("/api/orgs/acme/repos");
  console.log(JSON.stringify({ calls, first: first.repos[0].repo, second: second.repos[0].repo }));
})();
""")
    assert result == {"calls": ["/api/orgs/acme/repos"], "first": "early", "second": "live"}


def test_cookie_session_takes_the_early_read_but_a_bearer_session_does_not():
    body = """
fetchJson("/api/orgs/acme/repos").then((data) =>
  console.log(JSON.stringify({ calls, repo: data.repos[0].repo })));
"""
    cookie = _fetch_json_run({"nodeName": "a", "sessionToken": "cookie"}, body)
    assert cookie == {"calls": [], "repo": "early"}
    bearer = _fetch_json_run({"nodeName": "a", "sessionToken": "tok"}, body)
    assert bearer == {"calls": ["/api/orgs/acme/repos"], "repo": "live"}


def test_a_failed_early_read_falls_back_to_a_real_request():
    result = _fetch_json_run(None, """
globalThis.__forkmeshEarly.json["/api/orgs/acme/repos"] = Promise.resolve(null);
fetchJson("/api/orgs/acme/repos").then((data) =>
  console.log(JSON.stringify({ calls, repo: data.repos[0].repo })));
""")
    assert result == {"calls": ["/api/orgs/acme/repos"], "repo": "live"}


def _early_reads_run(pathname, catalog, meta=None):
    source = re.search(r"<script>(.*?)</script>", EARLY_READS, re.S).group(1)
    script = """
const calls = [];
const meta = %s;
globalThis.window = globalThis;
globalThis.location = { pathname: %s, search: "" };
globalThis.document = {
  querySelector: (selector) => {
    const name = (selector.match(/name="([^"]+)"/) || [])[1];
    return name in meta ? { content: meta[name] } : null;
  },
  getElementById: () => null,
};
globalThis.fetch = (url) => {
  calls.push(url);
  const body = url === "/api/repositories" ? %s : { ok: true, repos: [] };
  return Promise.resolve({ ok: true, headers: { get: () => "" }, json: async () => body });
};
%s
setTimeout(async () => {
  const orgs = await (window.__forkmeshEarly.json || {})["/api/orgs/acme/widgets/repos".replace("/widgets", "")];
  console.log(JSON.stringify({ calls, orgs }));
}, 20);
""" % (json.dumps(meta or {}), json.dumps(pathname), json.dumps(catalog), source)
    return _node(script)


def test_repo_page_asks_for_the_org_list_with_the_html_when_the_catalog_lacks_it():
    result = _early_reads_run("/acme/widgets", {"ok": True, "repositories": [
        {"owner": "someone", "name": "widgets"}]})
    assert "/api/orgs/acme/repos" in result["calls"]
    assert result["calls"].index("/api/repositories") < result["calls"].index("/api/orgs/acme/repos")
    assert result["orgs"] == {"ok": True, "repos": []}


def test_repo_page_skips_the_org_list_when_the_catalog_holds_the_repository():
    result = _early_reads_run("/acme/widgets", {"ok": True, "repositories": [
        {"owner": "Acme", "name": "Widgets"}]})
    assert "/api/orgs/acme/repos" not in result["calls"]
    assert result["orgs"] is None


def test_the_short_address_asks_for_the_org_list_without_waiting_for_the_catalog():
    result = _early_reads_run("/widgets/issues/3", {"ok": True, "repositories": [
        {"owner": "acme", "name": "widgets"}]},
        {"forkmesh-repo-short": "widgets", "forkmesh-repo-owner": "acme"})
    assert result["calls"][:2] == ["/api/repositories", "/api/orgs/acme/repos"]
    assert result["orgs"] == {"ok": True, "repos": []}

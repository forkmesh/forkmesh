#!/usr/bin/env python3
"""Static contracts for the dashboard.js fragment build.

``public/dashboard.js`` (~5450 lines) is split into ordered fragment files under
``public/dashboard/js/``. ``tools/build_dashboard_assets.py`` builds the static
``public/dashboard.js`` served by Cloudflare, mirroring how the HTML shell is
composed from partials (see ``src/dashboard_bundle.py`` / ``src/dashboard_shell.py``).
"""

import json
import sys
import tomllib
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

_ROOT = Path(__file__).resolve().parents[1]
_SRC = _ROOT / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

import dashboard_bundle  # noqa: E402

PUBLIC = _ROOT / "public"
JS_DIR = PUBLIC / "dashboard" / "js"
ENTRY_TEXT = (_SRC / "entry.py").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((_ROOT / "wrangler.toml").read_text(encoding="utf-8"))


def _split():
    sys.path.insert(0, str(_SRC))
    import dashboard_shell
    return dashboard_bundle.compose_split_from_reader(
        lambda rel: (PUBLIC / rel).read_text(encoding="utf-8"),
        dashboard_shell.content_version)


def test_static_dashboard_js_is_built_from_fragments():
    core, chunks = _split()
    assert (PUBLIC / "dashboard.js").is_file()
    assert (PUBLIC / "dashboard.js").read_text(encoding="utf-8") == core
    for name, text in chunks.items():
        assert (PUBLIC / name).read_text(encoding="utf-8") == text, name
    on_disk = sorted(p.name for p in PUBLIC.glob("dashboard-page-*.js"))
    assert on_disk == sorted(chunks)


def test_page_modules_are_chunks_the_core_loads_on_demand():
    core, chunks = _split()
    full = assembled_dashboard_js()
    assert len(core) < len(full) - 200_000
    for chunk, fragment in dashboard_bundle.LAZY_CHUNKS.items():
        asset = dashboard_bundle.chunk_asset(chunk)
        # The core carries only a stub for what other fragments call.
        body = (JS_DIR / fragment).read_text(encoding="utf-8")
        assert body not in core
        assert "loadDashboardChunk(%s)" % json.dumps(chunk) in core
        assert '"/%s?v=' % asset in core
        assert chunks[asset].count(body.strip()[:200]) == 1
        assert dashboard_bundle.CHUNK_GLOBAL in chunks[asset]


def test_chunks_run_against_the_core_scope():
    import shutil
    import subprocess
    if shutil.which("node") is None:
        return
    _core, chunks = _split()
    sources = {name: (JS_DIR / name).read_text(encoding="utf-8")
               for name in dashboard_bundle.FRAGMENTS}
    plans = dashboard_bundle.plan_chunks(sources)
    for asset, text in chunks.items():
        chunk = asset[len("dashboard-page-"):-3]
        imports, _lets, exports = plans[chunk]
        script = ("globalThis.window = globalThis;\n" + text
                  + "\nconst scope = {};\n"
                  + "".join("scope[%s] = function () {};\n" % json.dumps(n)
                            for n in imports)
                  + "const out = window.%s[%s](scope);\n"
                    % (dashboard_bundle.CHUNK_GLOBAL, json.dumps(chunk))
                  + "console.log(JSON.stringify(Object.keys(out).map((k) => [k, typeof out[k]])));")
        result = subprocess.run(["node", "-e", script], check=True,
                                capture_output=True, text=True)
        assert json.loads(result.stdout) == [[n, "function"] for n in exports]


_LOADER_HARNESS = r"""
globalThis.window = globalThis;
const outcomes = JSON.parse(process.argv[1]);
const appended = [];
const notices = [];
globalThis.setTimeout = (fn) => { fn(); return 0; };
globalThis.location = { reload() {} };
const element = (tag) => ({
  tag, attrs: {}, children: [],
  setAttribute(k, v) { this.attrs[k] = v; },
  addEventListener() {}, appendChild(c) { this.children.push(c); },
  remove() {}, prepend(c) { notices.push(c); },
});
const body = element("body");
globalThis.document = {
  createElement: element,
  querySelector: (sel) => sel === "main" ? null
    : (notices.length ? notices[0] : null),
  body,
  head: { appendChild(script) {
    appended.push(script.src);
    const ok = outcomes.shift();
    queueMicrotask(() => {
      if (ok) {
        window.%(glob)s = { demo: () => ({ answer: 42 }) };
        script.onload();
      } else script.onerror();
    });
  } },
};
%(loader)s
loadDashboardChunk("demo").then(
  (chunk) => console.log(JSON.stringify({ ok: chunk.answer, appended, notices: notices.length })),
  (error) => console.log(JSON.stringify({ error: error.message, appended, notices: notices.length })));
"""


def _run_loader(outcomes):
    import shutil
    import subprocess
    import pytest
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    loader = dashboard_bundle._core_loader({"demo": ((), (), ())}, {"demo": "abc"})
    script = _LOADER_HARNESS % {"glob": dashboard_bundle.CHUNK_GLOBAL, "loader": loader}
    result = subprocess.run(["node", "-e", script, json.dumps(outcomes)], check=True,
                            capture_output=True, text=True)
    return json.loads(result.stdout)


def test_a_chunk_that_fails_once_is_fetched_again():
    out = _run_loader([False, True])
    assert out["ok"] == 42
    assert out["appended"][0] == "/dashboard-page-demo.js?v=abc"
    assert out["appended"][1].startswith("/dashboard-page-demo.js?v=abc&retry=")
    assert out["notices"] == 0


def test_a_chunk_that_keeps_failing_shows_a_reload_notice_and_still_rejects():
    out = _run_loader([False, False])
    assert out["error"] == "dashboard_chunk_failed"
    assert len(out["appended"]) == 2
    assert out["notices"] == 1


def test_chunk_planner_refuses_unsafe_splits():
    import pytest
    sources = {name: (JS_DIR / name).read_text(encoding="utf-8")
               for name in dashboard_bundle.FRAGMENTS}
    fragment = dashboard_bundle.LAZY_CHUNKS["calendar"]
    broken = dict(sources)
    broken[fragment] += "\n  function poke() { repositoriesReady = null; }\n"
    with pytest.raises(dashboard_bundle.ChunkError):
        dashboard_bundle.plan_chunks(broken)
    broken = dict(sources)
    broken[fragment] += "\n  const CALENDAR_ONLY = 1;\n"
    broken["10-boot.js"] += "\n  void CALENDAR_ONLY;\n"
    with pytest.raises(dashboard_bundle.ChunkError):
        dashboard_bundle.plan_chunks(broken)


def test_every_fragment_listed_in_order_exists_on_disk():
    assert dashboard_bundle.FRAGMENTS, "no fragments declared"
    for name in dashboard_bundle.FRAGMENTS:
        assert (JS_DIR / name).is_file(), "missing fragment %s" % name
    # Names are numeric-prefixed so lexical order == source order; the declared
    # order must match a sorted listing so nothing is silently reordered.
    assert list(dashboard_bundle.FRAGMENTS) == sorted(dashboard_bundle.FRAGMENTS)


def test_no_orphan_fragments_on_disk():
    # Every *.js under dashboard/js/ must be declared in FRAGMENTS, or it would be
    # a dead file the Worker never serves (and never re-couples into the bundle).
    on_disk = sorted(p.name for p in JS_DIR.glob("*.js"))
    assert on_disk == sorted(dashboard_bundle.FRAGMENTS)


def test_composed_bundle_is_the_single_iife():
    js = assembled_dashboard_js()
    # The fragments are contiguous slices of ONE closure, so the concatenation
    # must open and close exactly one IIFE and still call init() at the end.
    assert js.startswith("(() => {")
    assert js.rstrip().endswith("})();")
    assert js.count("(() => {") >= 1
    assert "async function bootDashboard()" in js
    assert "await hydratePortableMirrorSession();" in js
    assert "\n    if (initSharedChrome()) {\n" in js
    assert "pendingPluginRegistrations" in js
    assert "registerPluginPage" in js
    assert "PAGE_INITS[currentPage()]" in js
    # A couple of load-bearing symbols that must survive the split intact.
    for marker in ("const state = {", "function renderRepoDetail(", "function initSharedChrome("):
        assert marker in js


def test_composition_matches_on_disk_fragments():
    expected = dashboard_bundle.assemble_bundle(
        (JS_DIR / name).read_text(encoding="utf-8")
        for name in dashboard_bundle.FRAGMENTS)
    assert assembled_dashboard_js() == expected


def test_dashboard_js_is_served_as_a_static_asset():
    assert "from dashboard_bundle import" not in ENTRY_TEXT
    assert 'if url.path == "/dashboard.js":' not in ENTRY_TEXT
    assert "async def _serve_dashboard_bundle" not in ENTRY_TEXT
    assert "/dashboard.js" not in WRANGLER["assets"]["run_worker_first"]


def test_shell_still_references_dashboard_js():
    shell = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")
    # The checked-in shell names the shared variable; deploy staging resolves it
    # to the bundle's content-derived version in its copy.
    assert 'src="/dashboard.js?v=${version}" defer' in shell

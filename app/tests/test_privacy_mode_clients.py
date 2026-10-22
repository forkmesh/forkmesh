import json
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(REPO / "tools"))

import mirror_gateway as gateway  # noqa: E402

ANALYTICS = REPO / "www" / "public" / "analytics.js"
BRAND = (ROOT / "public" / "instance-brand.js").read_text(encoding="utf-8")

HARNESS = r"""
const vm = require("node:vm");
const source = require("node:fs").readFileSync(process.argv[1], "utf8");
const privacyMode = process.argv[2] === "1";
const listeners = {};
const posted = [];
const window = {
  addEventListener(type, handler) { (listeners[type] ||= []).push(handler); },
  dispatchEvent() {},
  __forkmeshInstanceInfo: Promise.resolve({ privacyMode }),
};
const context = vm.createContext({
  window,
  location: new URL("https://code.example.org/dashboard/"),
  navigator: {},
  document: { addEventListener() {}, readyState: "complete", body: null,
              getElementById: () => null, head: { appendChild() {} },
              createElement: () => ({}) },
  localStorage: { getItem: () => "granted", setItem() {} },
  fetch: (url, init) => { posted.push(String(url)); return Promise.resolve({}); },
  Event, EventTarget, Response,
  CustomEvent: class extends Event {},
  Map, Set, Date, JSON, Promise, Error, TypeError, String, Number, Math, Object,
  setTimeout,
});
vm.runInContext(source, context);
setTimeout(() => {
  for (const handler of listeners.unhandledrejection || []) {
    handler({ reason: new Error("boom") });
  }
  setTimeout(() => process.stdout.write(JSON.stringify(posted)), 10);
}, 5);
"""


def _posted(privacy):
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    result = subprocess.run(
        ["node", "-e", HARNESS, str(ANALYTICS), "1" if privacy else "0"],
        check=True, capture_output=True, text=True, timeout=60)
    return json.loads(result.stdout)


def test_the_browser_sends_no_reports_or_analytics_in_privacy_mode():
    assert _posted(True) == []
    posted = _posted(False)
    assert "/api/client-errors" in posted


def test_the_footer_says_privacy_mode_is_on():
    assert 'const PRIVACY_NOTE = "Privacy mode: no visitor logs";' in BRAND
    assert "data.privacyMode === true" in BRAND


def test_the_git_node_counts_no_clones_in_privacy_mode(monkeypatch):
    calls = []
    application = SimpleNamespace(service_counters=SimpleNamespace(
        record=lambda *args: calls.append(args) or True))
    record = gateway.GatewayApplication.record_service_counter
    target = "/v1/repositories/mirror6/forkmesh/tree?path=src"
    monkeypatch.setenv("FORKMESH_PRIVACY_MODE", "1")
    assert not record(application, target, "tree", 200, delivered=True)
    assert calls == []
    monkeypatch.delenv("FORKMESH_PRIVACY_MODE")
    assert record(application, target, "tree", 200, delivered=True)
    assert len(calls) == 1

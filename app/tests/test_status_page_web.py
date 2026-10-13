import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


APP = Path(__file__).resolve().parents[1]
REPO = APP.parent
for root in (APP / "src", APP / "tools", REPO / "marketplace" / "tools"):
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

import build_app_plugins  # noqa: E402
import dashboard_shell  # noqa: E402


PACKAGE = REPO / "marketplace" / "packages" / "status-page"
PAGE = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
VIEW = (PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WWW = REPO / "www" / "public"
API_CLIENT = (WWW / "api-client.js").read_text(encoding="utf-8")
STATUS_LINK = re.compile(r"<a\b[^>]*\bhref=\"/status\"[^>]*>")


def test_manifest_declares_the_dashboard_page_and_its_entrypoints():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    page = next(item for item in manifest["contributions"]
                if item["socket"] == "dashboard.page")
    assert page["config"]["route"] == "/dashboard/status-monitors"
    assert manifest["entrypoints"][page["entrypoint"]] == "content/web/view.html"
    assert manifest["entrypoints"][page["config"]["script"]] == "content/web/page.js"
    entries = {row["item"].package_id: row for row in build_app_plugins._web_entries()}
    assert entries["status-page"]["route"] == "/dashboard/status-monitors"
    assert entries["status-page"]["script_path"].name == "page.js"


def test_status_page_stages_its_view_script_and_gated_navigation(tmp_path):
    destination = tmp_path / "dist"
    shutil.copytree(APP / "public", destination)
    pages = build_app_plugins.stage_web(destination)
    asset = pages["/dashboard/status-monitors"]
    assert asset == "dashboard-plugins/status-page/default/index.html"
    document = (destination / asset).read_text(encoding="utf-8")
    assert 'data-view="status-page"' in document
    assert 'data-nav="status-page" data-nav-link aria-current="page"' in document
    assert "/dashboard-plugins/status-page/default/page.js" in document
    staged = (destination / "dashboard-plugins" / "status-page" / "default"
              / "page.js").read_text(encoding="utf-8")
    assert staged == PAGE
    home = (destination / "dashboard" / "index.html").read_text(encoding="utf-8")
    nav = re.search(r'<a data-nav="status-page"[^>]*>', home).group(0)
    assert 'data-plugin-nav="status-page"' in nav and 'class="hidden"' in nav


def test_page_registers_and_uses_the_account_install_api():
    assert PAGE.startswith('window.ForkMeshDashboard.registerPage("status-page",')
    assert "escapeHtml" in PAGE
    assert 'request("/api/plugins/@account")' in PAGE
    assert "/api/plugins/@account/${encodeURIComponent(id)}/${action}" in PAGE
    assert 'lifecycle(PACKAGE, "install")' in PAGE
    assert 'lifecycle(PACKAGE, "uninstall")' in PAGE
    assert 'on ? "uninstall" : "install"' in PAGE
    assert "/api/plugins/@account/${PACKAGE}/settings" in PAGE
    assert "{ settings: { [key]: value } }" in PAGE
    assert 'request("/api/market/packages")' in PAGE
    assert "dependencyIds(row).includes(PACKAGE)" in PAGE
    assert "window.ForkMeshAPI?.rememberStatusPage?.(account, state.installed)" in PAGE


def test_page_filters_dots_by_installed_checks_and_probes_custom_monitors():
    assert 'fetch("/api/status"' in PAGE
    assert "(monitors.systems || []).includes(id)" in PAGE
    assert "(monitors.prefixes || []).some((prefix) => prefix && id.startsWith(prefix))" in PAGE
    assert "if (!installed(row.id)) return false;" in PAGE
    assert "const PROBE_TIMEOUT_MS = 10000;" in PAGE
    assert 'mode: "no-cors", cache: "no-store"' in PAGE
    assert "new AbortController()" in PAGE
    assert "settings().onlyProblems === true" in PAGE
    assert "!hideGreen || !isGreen(" in PAGE
    assert 'request("/api/status-monitors")' in PAGE
    assert 'request("/api/status-monitors/monitors", "POST", { name, url })' in PAGE
    assert "/api/status-monitors/monitors/${encodeURIComponent(id)}`, \"DELETE\"" in PAGE
    for code in ("invalid_name", "invalid_url", "too_many_monitors",
                 "monitor_not_found", "status_page_not_installed"):
        assert code + ":" in PAGE


def test_page_never_polls():
    assert "setInterval" not in PAGE
    assert "refreshMinutes" not in PAGE
    assert PAGE.count("setTimeout(") == 1
    assert "setTimeout(() => controller.abort(), PROBE_TIMEOUT_MS)" in PAGE
    assert "visibilitychange" not in PAGE


def test_page_renders_declared_settings_generically():
    assert 'field.type === "boolean"' in PAGE
    assert 'field.type === "choice"' in PAGE
    assert 'type="checkbox" data-status-setting=' in PAGE
    assert "<select data-status-setting=" in PAGE
    assert "onlyProblems" not in VIEW and "refreshMinutes" not in VIEW


def test_view_has_every_section():
    for marker in ("data-status-install-panel", "data-status-install", "data-status-app",
                   "data-status-dots", "data-status-custom-dots", "data-status-checks",
                   "data-status-settings", "data-status-monitors", "data-status-monitor-form",
                   "data-status-monitor-name", "data-status-monitor-url",
                   "data-status-monitor-error", "data-status-remove", "data-status-refresh"):
        assert marker in VIEW, marker
    assert 'href="/status"' in VIEW
    assert 'maxlength="40"' in VIEW and 'maxlength="300"' in VIEW
    assert "every device" in PAGE and "your monitors" in PAGE


def test_every_signed_in_status_link_is_gated():
    sources = [
        APP / "public" / "dashboard" / "partials" / "sidebar.html",
        APP / "public" / "dashboard" / "partials" / "views" / "profile-overview.html",
        WWW / "index.html", WWW / "mobile.html", WWW / "hackathon.html",
        WWW / "site-header.js",
    ]
    sources += [APP / "public" / meta["asset"] for meta in dashboard_shell.PAGES.values()]
    sources += [APP / "public" / "dashboard" / name / "index.html"
                for name in ("tasks", "notes", "calendar")]
    for path in sources:
        links = STATUS_LINK.findall(path.read_text(encoding="utf-8"))
        assert links, path
        for link in links:
            assert "data-status-entry" in link, (path, link)
    footer = (WWW / "site-footer.js").read_text(encoding="utf-8")
    assert '{ href: "/status", label: "System status", statusEntry: true }' in footer
    assert '<li${link.statusEntry ? " data-status-entry" : ""}${pluginAttr(link)}>' in footer
    assert 'class="site-footer-status-pill" data-status-entry>' in footer


def test_headers_and_dashboard_feed_the_shared_gate():
    for name in ("site-header.js", "home-header-auth.js"):
        source = (WWW / name).read_text(encoding="utf-8")
        assert source.count("window.ForkMeshAPI?.refreshStatusPage?.();") == 1, name
    for path in (APP / "public" / "dashboard" / "js" / "04-account.js",
                 APP / "public" / "dashboard.js"):
        source = path.read_text(encoding="utf-8")
        assert 'account, installed.includes("status-page"));' in source, path
    assert "html[data-status-page-off] [data-status-entry]{display:none!important}" in API_CLIENT
    assert 'routedFetch("/api/plugins/"' in API_CLIENT
    assert "STATUS_PAGE_TTL_MS = 5 * 60 * 1000" in API_CLIENT


GATE_HARNESS = r"""
const vm = require("node:vm");
const fs = require("node:fs");
const store = new Map();
const storage = {
  getItem: (key) => store.has(key) ? store.get(key) : null,
  setItem: (key, value) => store.set(key, String(value)),
  removeItem: (key) => store.delete(key),
};
const attributes = new Set();
const listeners = {};
const requests = [];
let plugins = ["status-page"];
const documentElement = {
  toggleAttribute: (name, on) => { if (on) attributes.add(name); else attributes.delete(name); },
  append: () => {},
};
const window = {
  addEventListener: (type, fn) => { (listeners[type] ||= []).push(fn); },
  dispatchEvent: () => true,
  fetch: async (url, init) => {
    requests.push({ url: String(url), auth: init?.headers?.authorization || "" });
    return { ok: true, status: 200, json: async () => ({ ok: true, plugins }) };
  },
  WebSocket: function () {},
};
const context = {
  window, localStorage: storage, sessionStorage: storage,
  document: { documentElement, head: { append: () => {} }, createElement: () => ({}), querySelector: () => null, referrer: "" },
  location: new URL("https://app.forkmesh.com/dashboard"),
  URL, Request: class {}, CustomEvent: class { constructor(type) { this.type = type; } },
  performance: { getEntriesByType: () => [] },
  console,
};
window.window = window;
vm.createContext(context);
vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), context);
const api = window.ForkMeshAPI;
(async () => {
  const out = {};
  out.signedOut = attributes.has("data-status-page-off");
  out.signedOutShown = await api.refreshStatusPage();
  out.signedOutRequests = requests.length;
  storage.setItem("forkmesh.session", JSON.stringify({ kind: "user", nodeName: "alice", sessionToken: "tok" }));
  plugins = ["water"];
  out.removed = await api.refreshStatusPage();
  out.removedHidden = attributes.has("data-status-page-off");
  out.auth = requests[0]?.auth || "";
  out.cached = await api.refreshStatusPage();
  out.cachedRequests = requests.length;
  api.rememberStatusPage("alice", true);
  out.reinstalledHidden = attributes.has("data-status-page-off");
  api.rememberStatusPage("bob", false);
  out.otherAccountHidden = attributes.has("data-status-page-off");
  out.shownForBob = api.statusPageShown("bob");
  console.log(JSON.stringify(out));
})();
"""


def test_shared_gate_hides_status_links_only_for_an_account_without_status_page(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path / "gate.js"
    harness.write_text(GATE_HARNESS, encoding="utf-8")
    result = subprocess.run(
        [node, str(harness), str(WWW / "api-client.js")],
        capture_output=True, text=True, timeout=30, check=True,
    )
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out == {
        "signedOut": False,
        "signedOutShown": True,
        "signedOutRequests": 0,
        "removed": False,
        "removedHidden": True,
        "auth": "Bearer tok",
        "cached": False,
        "cachedRequests": 1,
        "reinstalledHidden": False,
        "otherAccountHidden": False,
        "shownForBob": False,
    }


PLUGIN_ENTRY = re.compile(r'data-plugin-entry="([a-z0-9-]+)"')


def test_plugin_entries_name_real_packages():
    known = {path.name for path in (REPO / "marketplace" / "packages").iterdir()
             if (path / "manifest.json").exists()}
    sources = [
        APP / "public" / "dashboard" / "partials" / "sidebar.html",
        APP / "public" / "dashboard" / "partials" / "views" / "home.html",
        APP / "public" / "dashboard" / "partials" / "views" / "profile-overview.html",
        APP / "public" / "dashboard" / "partials" / "views" / "profile-repositories.html",
        APP / "public" / "dashboard" / "js" / "07-repo-compose-branch.js",
        APP / "public" / "dashboard" / "js" / "08-repo-detail-network.js",
        WWW / "site-header.js", WWW / "site-footer.js",
        WWW / "index.html", WWW / "mobile.html", WWW / "hackathon.html",
    ]
    sources += [APP / "public" / "dashboard" / name / "index.html"
                for name in ("tasks", "notes", "calendar")]
    found = set()
    for path in sources:
        ids = set(PLUGIN_ENTRY.findall(path.read_text(encoding="utf-8")))
        assert ids, path
        found |= ids
    assert found <= known, found - known
    sidebar = (APP / "public" / "dashboard" / "partials" / "sidebar.html").read_text(encoding="utf-8")
    for package in ("agents", "chat", "world", "network", "ledger"):
        assert f'data-plugin-entry="{package}"' in sidebar, package
    header = (WWW / "site-header.js").read_text(encoding="utf-8")
    assert '<a class="fm-header-world" href="https://world.forkmesh.com/" data-plugin-entry="world"' in header
    assert '<a class="fm-header-chat" href="/chat" data-plugin-entry="chat"' in header
    repo = (APP / "public" / "dashboard" / "js" / "08-repo-detail-network.js").read_text(encoding="utf-8")
    assert 'pulls: "pull-requests"' in repo
    assert '${pluginEntry ? ` data-plugin-entry="${pluginEntry}"` : ""}' in repo
    for name in dashboard_shell.PAGES:
        page = (APP / "public" / dashboard_shell.PAGES[name]["asset"]).read_text(encoding="utf-8")
        assert 'data-plugin-entry="chat"' in page, name


def test_apps_page_installs_and_removes_account_synced_plugins():
    view = (APP / "public" / "dashboard" / "partials" / "views" / "apps.html").read_text(encoding="utf-8")
    for marker in ("data-apps-plugins-panel", "data-apps-plugins-refresh",
                   "data-apps-plugins-notice", "data-apps-plugins"):
        assert marker in view, marker
    source = (APP / "public" / "dashboard" / "js" / "09-apps.js").read_text(encoding="utf-8")
    assert 'appsRequest("GET", "/api/market/packages")' in source
    assert 'appsRequest("GET", "/api/plugins/@account")' in source
    assert '`/api/plugins/@account/${encodeURIComponent(row.id)}/${on ? "install" : "uninstall"}`' in source
    assert 'row?.accountSync === true' in source
    assert 'appsPluginDependencies(row)[0] === id' in source
    assert "This also removes" in source and "window.confirm(" in source
    assert 'window.ForkMeshAPI?.refreshPlugins?.(true)' in source
    assert '"forkmesh:plugins-changed"' in source
    assert "setInterval" not in source and "setTimeout" not in source
    account = (APP / "public" / "dashboard" / "js" / "04-account.js").read_text(encoding="utf-8")
    assert 'window.addEventListener("forkmesh:plugins-changed"' in account
    assert "window.ForkMeshAPI?.rememberPlugins?.(account, installed);" in account


PLUGIN_HARNESS = r"""
const vm = require("node:vm");
const fs = require("node:fs");
const store = new Map();
const storage = {
  getItem: (key) => store.has(key) ? store.get(key) : null,
  setItem: (key, value) => store.set(key, String(value)),
  removeItem: (key) => store.delete(key),
};
const styles = [];
const requests = [];
let plugins = ["agents", "status-page"];
const documentElement = { toggleAttribute: () => {}, append: () => {} };
const window = {
  addEventListener: () => {},
  dispatchEvent: () => true,
  fetch: async (url, init) => {
    requests.push(String(url));
    return { ok: true, status: 200, json: async () => ({ ok: true, plugins }) };
  },
  WebSocket: function () {},
};
const context = {
  window, localStorage: storage, sessionStorage: storage,
  document: {
    documentElement, head: { append: () => {} },
    createElement: () => { const node = { textContent: "", setAttribute: () => {} }; styles.push(node); return node; },
    querySelector: () => null, referrer: "",
  },
  location: new URL("https://app.forkmesh.com/dashboard"),
  URL, Request: class {}, CustomEvent: class { constructor(type) { this.type = type; } },
  performance: { getEntriesByType: () => [] },
  console,
};
window.window = window;
vm.createContext(context);
vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), context);
const api = window.ForkMeshAPI;
const rule = () => styles[1].textContent;
(async () => {
  const out = {};
  out.signedOutRule = rule();
  out.signedOutShown = api.pluginShown("chat");
  out.signedOutList = await api.refreshPlugins();
  storage.setItem("forkmesh.session", JSON.stringify({ kind: "user", nodeName: "alice", sessionToken: "tok" }));
  out.list = await api.refreshPlugins();
  out.rule = rule();
  out.chat = api.pluginShown("chat");
  out.agents = api.pluginShown("agents");
  out.status = api.statusPageShown();
  api.rememberPlugin("alice", "chat", true);
  out.chatAfterInstall = api.pluginShown("chat");
  api.rememberPlugin("alice", "status-page", false);
  out.statusAfterRemove = api.statusPageShown();
  out.cached = await api.refreshPlugins();
  out.requests = requests.length;
  out.bob = api.pluginShown("chat", "bob");
  console.log(JSON.stringify(out));
})();
"""


def test_plugin_gate_hides_entries_missing_from_the_account_list(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path / "plugins.js"
    harness.write_text(PLUGIN_HARNESS, encoding="utf-8")
    result = subprocess.run(
        [node, str(harness), str(WWW / "api-client.js")],
        capture_output=True, text=True, timeout=30, check=True,
    )
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out == {
        "signedOutRule": "",
        "signedOutShown": True,
        "signedOutList": None,
        "list": ["agents", "status-page"],
        "rule": '[data-plugin-entry]:not([data-plugin-entry="agents"])'
                ':not([data-plugin-entry="status-page"]){display:none!important}',
        "chat": False,
        "agents": True,
        "status": True,
        "chatAfterInstall": True,
        "statusAfterRemove": False,
        "cached": ["agents", "chat"],
        "requests": 1,
        "bob": True,
    }

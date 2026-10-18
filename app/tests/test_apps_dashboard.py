import json
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402

PUBLIC = ROOT / "public"
DASHBOARD = PUBLIC / "dashboard"
APPS_JS = (DASHBOARD / "js" / "09-apps.js").read_text(encoding="utf-8")
BOOT_JS = (DASHBOARD / "js" / "10-boot.js").read_text(encoding="utf-8")
SIDEBAR = (DASHBOARD / "partials" / "sidebar.html").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
CONSENT_HTML = (PUBLIC / "oauth-authorize.html").read_text(encoding="utf-8")
CONSENT_JS = (PUBLIC / "oauth-authorize.js").read_text(encoding="utf-8")


def test_apps_pages_are_composed_dashboard_pages():
    first = WRANGLER["assets"]["run_worker_first"]
    for page_id, route, marker in (
            ("apps", "/dashboard/apps", "data-apps-installed"),
            ("apps-build", "/dashboard/apps/build", "data-app-form")):
        page = dashboard_shell.PAGES[page_id]
        assert page["route"] == route
        assert static_routes.DASHBOARD_PAGE_ASSETS[route] == page["asset"]
        assert "/" + page["asset"] in static_routes.BLOCKED_STATIC_HTML_PATHS
        assert "!" + route in first and "!" + route + "/" in first
        built = (PUBLIC / page["asset"]).read_text(encoding="utf-8")
        assert built.count("<section data-view=") == 1
        assert marker in built
        assert 'data-page="%s"' % page_id in built


def test_fragment_is_a_lazy_chunk_wired_to_boot():
    assert "09-apps.js" in dashboard_bundle.FRAGMENTS
    assert dashboard_bundle.LAZY_CHUNKS["apps"] == "09-apps.js"
    assert '"apps": initAppsPage,' in BOOT_JS
    assert '"apps-build": initAppsBuildPage,' in BOOT_JS
    assert (PUBLIC / "dashboard-page-apps.js").is_file()
    assert re.search(r'<a data-nav="apps"[^>]*href="/dashboard/apps"', SIDEBAR)


def test_pages_render_untrusted_text_safely():
    for source in (APPS_JS, CONSENT_JS):
        assert "innerHTML" not in source
        assert "insertAdjacentHTML" not in source
    assert 'credentials: "same-origin"' in APPS_JS
    assert "/^data:image\\/(png|webp);base64,/" in APPS_JS
    assert 'parsed.protocol === "https:"' in CONSENT_JS
    assert '<meta name="referrer" content="no-referrer" />' in CONSENT_HTML
    assert "/oauth-authorize.js?v=${version}" in CONSENT_HTML


def test_chat_clients_route_app_commands_blocks_and_mentions():
    chat = (PUBLIC / "chat.js").read_text(encoding="utf-8")
    chat_html = (PUBLIC / "chat.html").read_text(encoding="utf-8")
    dashboard_chat = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
    apps_js = (PUBLIC / "chat-apps.js").read_text(encoding="utf-8")
    assert '<script src="/chat-apps.js?v=${version}" defer></script>' in chat_html
    assert "window.ForkMeshChatApps.run(source, appsChannelContext())" in chat
    assert "forwardMentions(text, appsContext.channelId, plain.id)" in chat
    assert "window.ForkMeshChatApps.renderBlocks(container, record.appMessage" in chat
    assert 'senderId.startsWith("app:") && !notice' in chat
    assert 'script.src = "/chat-apps.js";' in dashboard_chat
    assert "innerHTML" not in apps_js
    assert "if (!named.some((slug) => slugs.has(slug))) return;" in apps_js
    assert "/api/apps/mentions" in apps_js
    assert 'record && record.visibility === "public" ? record.id : ""' in chat


PLACES_HARNESS = r"""
const fs = require("node:fs");
const source = fs.readFileSync(process.argv[2], "utf8");
class Node {
  constructor(tag) { this.tag = tag; this.children = []; this.dataset = {}; this.attributes = {}; this.listeners = {}; this.className = ""; this.text = ""; this.disabled = false; this.checked = false; this.hidden = false; this.title = ""; this.type = ""; this.classList = { toggle: () => {} }; }
  get textContent() { return this.text + this.children.map((child) => child.textContent).join(""); }
  set textContent(value) { this.text = String(value); this.children = []; }
  append(...nodes) { this.children.push(...nodes); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  addEventListener(type, listener) { this.listeners[type] = listener; }
  all() { return [this, ...this.children.flatMap((child) => child.all())]; }
}
const box = new Node("div");
const notice = new Node("p");
const requests = [];
const remembered = [];
const events = [];
let accountPackages = [
  { id: "workflows", installed: true, places: ["mobile", "desktop", "web"], availablePlaces: ["mobile", "desktop", "web"] },
  { id: "food-guide", installed: true, places: ["mobile"], availablePlaces: ["mobile"] },
  { id: "notes", installed: false },
];
const market = [
  { id: "workflows", name: "Workflows", accountSync: true },
  { id: "food-guide", name: "Food Guide", accountSync: true, places: ["mobile"] },
  { id: "notes", name: "Notes", accountSync: true },
];
const fetch = async (path, init) => {
  requests.push({ path, method: init.method, credentials: init.credentials, body: init.body ? JSON.parse(init.body) : null });
  if (path === "/api/market/packages") return { ok: true, status: 200, json: async () => ({ ok: true, packages: market }) };
  if (path.endsWith("/places")) {
    const body = JSON.parse(init.body);
    accountPackages = accountPackages.map((row) => row.id === "workflows" ? { ...row, places: body.places } : row);
  }
  return { ok: true, status: 200, json: async () => ({ ok: true, packages: accountPackages }) };
};
const $ = (selector) => selector === "[data-apps-plugins]" ? box : selector === "[data-apps-plugins-notice]" ? notice : null;
const window = {
  ForkMeshAPI: { rememberPlugin: (account, id, shown) => remembered.push([id, shown]), refreshPlugins: async () => null },
  dispatchEvent: (event) => events.push(event.type),
  confirm: () => true,
};
const document = { createElement: (tag) => new Node(tag) };
class CustomEvent { constructor(type, init) { this.type = type; this.detail = init?.detail; } }
const state = { session: { nodeName: "alice", sessionToken: "cookie" } };
const api = new Function("$", "state", "window", "document", "fetch", "CustomEvent",
  source + "\nreturn { loadAppsPlugins, setAppsPluginPlaces };")($, state, window, document, fetch, CustomEvent);
const card = (id) => box.all().find((node) => node.dataset.appsPlugin === id);
const places = (id) => {
  const node = card(id);
  const group = node && node.all().find((item) => item.dataset.appsPluginPlaces === id);
  if (!group) return null;
  return group.all().filter((item) => item.dataset.appsPluginPlace).map((item) => [item.dataset.appsPluginPlace, item.checked, item.disabled]);
};
const note = (id) => {
  const node = card(id);
  const hint = node && node.all().find((item) => item.dataset.appsPluginNotOnWeb === id);
  return hint ? hint.textContent : "";
};
(async () => {
  const out = {};
  await api.loadAppsPlugins();
  out.workflows = places("workflows");
  out.foodGuide = places("food-guide");
  out.foodGuideNote = note("food-guide");
  out.workflowsNote = note("workflows");
  const web = card("workflows").all().find((item) => item.dataset.appsPluginPlace === "web");
  web.checked = false;
  web.listeners.change();
  await new Promise((resolve) => setImmediate(resolve));
  await new Promise((resolve) => setImmediate(resolve));
  out.request = requests.find((item) => item.path.endsWith("/places"));
  out.afterWeb = places("workflows");
  out.afterNote = note("workflows");
  out.notice = notice.textContent;
  out.remembered = remembered;
  out.events = events;
  await api.setAppsPluginPlaces({ id: "workflows", name: "Workflows" }, []);
  out.placesRequests = requests.filter((item) => item.path.endsWith("/places")).length;
  console.log(JSON.stringify(out));
})();
"""


def test_plugins_list_lets_the_user_choose_where_a_plugin_shows(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path / "places.js"
    harness.write_text(PLACES_HARNESS, encoding="utf-8")
    result = subprocess.run(
        [node, str(harness), str(DASHBOARD / "js" / "09-apps.js")],
        capture_output=True, text=True, timeout=30, check=True,
    )
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out["workflows"] == [["mobile", True, False], ["desktop", True, False], ["web", True, False]]
    assert out["foodGuide"] == [["mobile", True, True]]
    assert out["foodGuideNote"] == "Not available on the web: it shows on Phone only."
    assert out["workflowsNote"] == ""
    assert out["request"] == {
        "path": "/api/plugins/@account/workflows/places", "method": "POST",
        "credentials": "same-origin", "body": {"places": ["mobile", "desktop"]},
    }
    assert out["afterWeb"] == [["mobile", True, False], ["desktop", True, False], ["web", False, False]]
    assert out["afterNote"] == "Not shown on the web. Turn on Web to show it here."
    assert out["notice"] == "Workflows now shows on Phone, Desktop."
    assert out["remembered"] == [["workflows", False]]
    assert out["events"] == ["forkmesh:plugins-changed"]
    assert out["placesRequests"] == 1


INSTALL_ALL_HARNESS = r"""
const fs = require("node:fs");
const source = fs.readFileSync(process.argv[2], "utf8");
class Node {
  constructor(tag) { this.tag = tag; this.children = []; this.dataset = {}; this.attributes = {}; this.listeners = {}; this.className = ""; this.text = ""; this.disabled = false; this.hidden = false; this.checked = false; }
  get textContent() { return this.text + this.children.map((child) => child.textContent).join(""); }
  set textContent(value) { this.text = String(value); this.children = []; }
  append(...nodes) { this.children.push(...nodes); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  addEventListener(type, listener) { this.listeners[type] = listener; }
  querySelector() { return label; }
  all() { return [this, ...this.children.flatMap((child) => child.all())]; }
}
const box = new Node("div");
const notice = new Node("p");
const button = new Node("button");
const label = new Node("span");
const requests = [];
const confirms = [];
const events = [];
let installed = { workflows: true, notes: false, tasks: false, crm: false, "notes-ai": false, gemma: false, skin: false };
const market = [
  { id: "workflows", name: "Workflows", accountSync: true, type: "extension" },
  { id: "notes", name: "Notes", accountSync: true, type: "extension" },
  { id: "tasks", name: "Tasks", accountSync: true, type: "extension" },
  { id: "crm", name: "CRM", accountSync: true, type: "extension" },
  { id: "notes-ai", name: "Notes AI", accountSync: true, type: "extension", dependencies: ["notes"] },
  { id: "gemma", name: "Gemma", accountSync: true, type: "agent" },
  { id: "skin", name: "Skin", accountSync: true, type: "theme" },
  { id: "desktop-only", name: "Desktop only", accountSync: false, type: "extension" },
];
const rows = () => ({ ok: true, packages: Object.entries(installed).map(([id, on]) => ({ id, installed: on })) });
const fetch = async (path, init) => {
  requests.push({ path, method: init.method });
  const id = path.split("/")[4];
  if (path.endsWith("/install")) {
    if (id === "crm") return { ok: false, status: 500, json: async () => ({ ok: false, error: "boom" }) };
    installed[id] = true;
    return { ok: true, status: 200, json: async () => ({ ...rows(), changed: id === "tasks" ? ["tasks", "notes"] : [id] }) };
  }
  if (path === "/api/market/packages") return { ok: true, status: 200, json: async () => ({ ok: true, packages: market }) };
  return { ok: true, status: 200, json: async () => rows() };
};
const $ = (selector) => ({
  "[data-apps-plugins]": box,
  "[data-apps-plugins-notice]": notice,
  "[data-apps-plugins-install-all]": button,
})[selector] || null;
const window = {
  ForkMeshAPI: { rememberPlugin: () => null, refreshPlugins: async () => null },
  dispatchEvent: (event) => events.push(event.type),
  confirm: (message) => { confirms.push(message); return true; },
};
const document = { createElement: (tag) => new Node(tag) };
class CustomEvent { constructor(type, init) { this.type = type; this.detail = init?.detail; } }
const state = { session: { nodeName: "alice", sessionToken: "cookie" } };
const location = { pathname: "/dashboard/apps", search: "?installAll=1&tab=x", hash: "#plugins" };
const replaced = [];
const history = { replaceState: (_s, _t, url) => replaced.push(url) };
const api = new Function("$", "state", "window", "document", "fetch", "CustomEvent", "location", "history",
  source + "\nreturn { loadAppsPlugins, installAllAppsPlugins, runAppsInstallAllRequest };")($, state, window, document, fetch, CustomEvent, location, history);
(async () => {
  const out = {};
  await api.loadAppsPlugins();
  out.before = [label.textContent, button.disabled];
  await api.runAppsInstallAllRequest();
  out.replaced = replaced;
  out.installs = requests.filter((item) => item.path.endsWith("/install")).map((item) => item.path.split("/")[4]);
  out.confirm = confirms;
  out.notice = notice.textContent;
  out.events = events;
  out.after = [label.textContent, button.disabled];
  installed.crm = true;
  await api.loadAppsPlugins();
  out.done = [label.textContent, button.disabled];
  console.log(JSON.stringify(out));
})();
"""


def test_plugins_list_installs_every_account_plugin_in_one_click(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path / "install_all.js"
    harness.write_text(INSTALL_ALL_HARNESS, encoding="utf-8")
    result = subprocess.run(
        [node, str(harness), str(DASHBOARD / "js" / "09-apps.js")],
        capture_output=True, text=True, timeout=30, check=True,
    )
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out["before"] == ["Install all (3)", False]
    assert out["confirm"] == [
        "Install 3 plugins for your account? They show on the web, the desktop app and the mobile app, and you can remove any of them later."
    ]
    assert out["replaced"] == ["/dashboard/apps?tab=x#plugins"]
    assert out["installs"] == ["notes", "tasks", "crm"]
    assert out["notice"] == "Installed 2, 1 failed: CRM."
    assert out["events"] == ["forkmesh:plugins-changed"]
    assert out["after"] == ["Install all (1)", False]
    assert out["done"] == ["All installed", True]


def test_install_all_button_sits_in_the_plugins_toolbar():
    view = (DASHBOARD / "partials" / "views" / "apps.html").read_text(encoding="utf-8")
    assert "data-apps-plugins-install-all" in view
    assert "cta" not in view[view.index("data-apps-plugins-install-all"):view.index("data-apps-plugins-refresh")]


def test_plugins_list_disables_the_last_place_and_keeps_installs_plain():
    assert 'input.disabled = appsPlugins.busy || (input.checked && chosen.length === 1);' in APPS_JS
    assert '`/api/plugins/@account/${encodeURIComponent(row.id)}/places`, { places }' in APPS_JS
    assert '`/api/plugins/@account/${encodeURIComponent(row.id)}/${on ? "install" : "uninstall"}`, {})' in APPS_JS
    assert 'if (on) card.append(appsPluginPlacesControl(row));' in APPS_JS
    assert 'window.ForkMeshAPI?.rememberPlugin?.(account, id, appsPluginOnWeb(id))' in APPS_JS


def test_workflows_sidebar_link_comes_only_from_the_plugin_list():
    for path in (DASHBOARD / "partials").rglob("*.html"):
        assert "/dashboard/workflows" not in path.read_text(encoding="utf-8"), path
    for path in (DASHBOARD / "js").glob("*.js"):
        assert 'href="/dashboard/workflows"' not in path.read_text(encoding="utf-8"), path
    manifest = json.loads((ROOT.parent / "marketplace" / "packages" / "workflows" / "manifest.json").read_text(encoding="utf-8"))
    page = next(item for item in manifest["contributions"] if item["socket"] == "dashboard.page")
    assert page["config"]["route"] == "/dashboard/workflows"
    assert page["config"]["navigation"]["label"] == "Workflows"
    assert manifest["install"]["default"] is False
    tools = str(ROOT / "tools")
    marketplace_tools = str(ROOT.parent / "marketplace" / "tools")
    for extra in (tools, marketplace_tools):
        if extra not in sys.path:
            sys.path.insert(0, extra)
    import build_app_plugins
    navigation = build_app_plugins._navigation(build_app_plugins._web_entries(build_app_plugins.CATALOG))
    assert re.search(r'<a data-nav="workflows" data-nav-link data-plugin-nav="workflows" href="/dashboard/workflows" class="hidden">', navigation)

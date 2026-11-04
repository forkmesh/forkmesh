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
PINGS_JS = (DASHBOARD / "js" / "09-pings.js").read_text(encoding="utf-8")
NETWORK_JS = (DASHBOARD / "js" / "08-repo-detail-network.js").read_text(encoding="utf-8")
BOOT_JS = (DASHBOARD / "js" / "10-boot.js").read_text(encoding="utf-8")
HEADER = (DASHBOARD / "partials" / "header.html").read_text(encoding="utf-8")
MODALS = (DASHBOARD / "partials" / "modals.html").read_text(encoding="utf-8")
VIEW = (DASHBOARD / "partials" / "views" / "pings.html").read_text(encoding="utf-8")
BUNDLE = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))


def function_source(source, name):
    match = re.search(
        r"^  (?:async )?function %s\(.*?(?=^  (?:async )?function |^  const |\Z)" % re.escape(name),
        source, re.M | re.S)
    assert match, name
    return match.group(0)


def test_pings_page_is_a_composed_dashboard_page():
    page = dashboard_shell.PAGES["pings"]
    assert page["route"] == "/dashboard/pings"
    assert page["view"] == "pings"
    assert page["asset"] == "dashboard/pings/index.html"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/pings"] == page["asset"]
    assert "/dashboard/pings/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    first = WRANGLER["assets"]["run_worker_first"]
    assert "!/dashboard/pings" in first and "!/dashboard/pings/" in first
    built = (PUBLIC / page["asset"]).read_text(encoding="utf-8")
    read = lambda rel: (PUBLIC / rel).read_text(encoding="utf-8")
    assert built == dashboard_shell.compose_page_from_reader(read, "pings")
    assert built.count("<section data-view=") == 1
    assert 'data-page="pings"' in built
    for marker in ("data-pings-page", "data-pings-list", 'data-pings-filter="all"',
                   'data-pings-filter="unread"', "data-pings-mark-all",
                   "data-pings-load-more", "data-ping-pause-form",
                   "data-notification-count", 'data-ping-settings-link=""'):
        assert marker in built, marker


def test_fragment_and_boot_are_wired_and_bundled():
    assert "09-pings.js" in dashboard_bundle.FRAGMENTS
    assert "09-pings.js" not in dashboard_bundle.LAZY_CHUNKS.values()
    assert '"pings": initPingsPage,' in BOOT_JS
    assert PINGS_JS in BUNDLE


def test_bell_is_a_link_to_the_pings_page_and_keeps_its_badge():
    bell = HEADER[HEADER.rindex("<a", 0, HEADER.index('id="notificationToggle"')):]
    bell = bell[:bell.index("</a>")]
    assert 'href="/dashboard/pings"' in bell
    assert 'aria-label="Pings"' in bell
    assert "data-notification-badge" in bell
    assert "data-notification-bell-on" in bell and "data-notification-bell-off" in bell
    assert "notificationDropdown" not in HEADER
    assert "data-show-all-notifications" not in HEADER
    assert "notificationModal" not in MODALS
    for name in ("setNotificationDropdownOpen", "setNotificationModalOpen",
                 "renderNotificationModal", "renderNotificationPreview"):
        assert "function %s(" % name not in BUNDLE, name
    badge = function_source(NETWORK_JS, "renderNotificationBadge")
    assert '$("[data-notification-badge]")?.classList.toggle("hidden", unread <= 0);' in badge
    assert "`Pings, ${unread} unread`" in badge
    for page in dashboard_shell.PAGES.values():
        html = (PUBLIC / page["asset"]).read_text(encoding="utf-8")
        assert 'id="notificationToggle"\n            href="/dashboard/pings"' in html, page["asset"]
    assert 'bell?.setAttribute("aria-current", "page");' in function_source(PINGS_JS, "initPingsPage")


def test_the_page_reuses_the_ping_renderer_and_the_live_load():
    render = function_source(PINGS_JS, "renderPingsPage")
    assert "notificationRowHtml(item, {" in render
    assert "notificationRowHtml" not in PINGS_JS.replace("notificationRowHtml(item, {", "", 1)
    row = function_source(NETWORK_JS, "notificationRowHtml")
    for helper in ("notificationLead(item)", "notificationItemIcon(item)", "notificationAvatarHtml(item, lead)",
                   "notificationInstanceLogoHtml()", "notificationTimeLabel(item.ts)",
                   "pingSettingsHref(item.kind)", "notificationDetailHtml(item)",
                   "notificationTargetHref(item)"):
        assert helper in row or helper in function_source(NETWORK_JS, "notificationAvatarHtml"), helper
    assert "homeFeedHref(item)" in function_source(NETWORK_JS, "notificationTargetHref")
    assert "pingTurnOffLinkHtml(item)" in function_source(NETWORK_JS, "notificationDetailHtml")
    load = function_source(NETWORK_JS, "loadNotifications")
    assert "renderPingsPage();" in load and "renderNotificationBadge();" in load
    assert "state.notificationTotal = Number(data.total)" in load
    more = function_source(PINGS_JS, "loadMorePings")
    assert "&limit=${PINGS_PAGE_SIZE}&offset=${offset}" in more


def _run_node(script):
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


@pytest.mark.skipif(shutil.which("node") is None, reason="Node.js is unavailable")
def test_mark_one_and_mark_all_read_update_the_list_and_the_relay():
    sources = "\n".join([
        "const PINGS_PAGE_SIZE = 40;",
        function_source(PINGS_JS, "pingsPageItems"),
        function_source(PINGS_JS, "pingsPageVisible"),
        function_source(PINGS_JS, "markPingsReadLocally"),
        function_source(PINGS_JS, "markPingsRead"),
        function_source(NETWORK_JS, "markNotificationsRead"),
    ])
    script = """
const posts = [];
let loads = 0;
const state = {
  session: { nodeName: "alice", sessionToken: "tok" },
  notifications: [
    { id: "a", ts: 3, readAt: 0 },
    { id: "b", ts: 2, readAt: 0 },
  ],
  notificationUnread: 3,
};
const pingsView = { filter: "unread", older: [{ id: "c", ts: 1, readAt: 0 }] };
const fetch = async (url, init) => { posts.push({ url, init, body: JSON.parse(init.body) }); return { ok: true }; };
const loadNotifications = async () => { loads += 1; };
const renderNotificationBadge = () => {};
const renderPingsPage = () => {};
%s
(async () => {
  await markPingsRead(["b"]);
  const afterOne = { unread: state.notificationUnread, visible: pingsPageVisible(pingsPageItems()).map((i) => i.id) };
  await markPingsRead([], true);
  const afterAll = { unread: state.notificationUnread, visible: pingsPageVisible(pingsPageItems()).map((i) => i.id) };
  console.log(JSON.stringify({ afterOne, afterAll, posts: posts.map((p) => ({ url: p.url, body: p.body, keepalive: p.init.keepalive })), loads }));
})();
""" % sources
    out = _run_node(script)
    assert out["afterOne"] == {"unread": 2, "visible": ["a", "c"]}
    assert out["afterAll"] == {"unread": 0, "visible": []}
    assert out["posts"] == [
        {"url": "/api/notifications", "keepalive": True,
         "body": {"node": "alice", "ids": ["b"], "all": False, "sessionToken": "tok"}},
        {"url": "/api/notifications", "keepalive": True,
         "body": {"node": "alice", "ids": [], "all": True, "sessionToken": "tok"}},
    ]
    assert out["loads"] == 2


def test_page_uses_theme_roles_and_existing_controls():
    assert "fm-segmented" in VIEW and 'aria-pressed="true"' in VIEW
    assert "bg-cta" not in VIEW and "green" not in VIEW and "emerald" not in VIEW
    assert "fm-blankslate" in PINGS_JS
    assert "Sign in to see your pings" in PINGS_JS
    assert "/login?next=" in PINGS_JS


def test_good_news_pings_show_a_green_check():
    good = function_source(NETWORK_JS, "pingIsGoodNews")
    assert '=== "operational_alert"' in good and '=== "up"' in good
    assert '"circle-check"' in function_source(NETWORK_JS, "notificationItemIcon")
    assert '"text-success"' in function_source(NETWORK_JS, "notificationItemIconTone")

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HOME_AUTH = (ROOT / "www" / "public" / "home-header-auth.js").read_text(encoding="utf-8")
INDEX_HTML = (ROOT / "www" / "public" / "index.html").read_text(encoding="utf-8")
HELPERS = (ROOT / "app" / "public" / "dashboard" / "js" / "02-helpers.js").read_text(encoding="utf-8")
CLICKS = (ROOT / "app" / "public" / "dashboard" / "js" / "08-repo-detail-network.js").read_text(encoding="utf-8")
BOOT = (ROOT / "app" / "public" / "dashboard" / "js" / "10-boot.js").read_text(encoding="utf-8")
SETTINGS = (ROOT / "app" / "public" / "dashboard" / "partials" / "views" / "settings.html").read_text(encoding="utf-8")
BUILT_SETTINGS = (ROOT / "app" / "public" / "dashboard" / "settings" / "index.html").read_text(encoding="utf-8")
BUNDLE = (ROOT / "app" / "public" / "dashboard.js").read_text(encoding="utf-8")


def test_settings_switch_defaults_on_and_is_wired():
    for html in (SETTINGS, BUILT_SETTINGS):
        switch = re.search(r"<button data-home-redirect [^>]*>", html)
        assert switch, "settings page lost the switch"
        assert 'role="switch"' in switch.group(0)
        assert 'aria-checked="true"' in switch.group(0)
    assert 'closest("[data-home-redirect]")' in CLICKS
    assert "watchHomeRedirectSetting();" in BOOT
    assert "function saveHomeRedirectSetting(enabled)" in BUNDLE


def test_choice_is_a_shared_cookie_absent_means_on():
    assert 'const HOME_REDIRECT_COOKIE = "forkmesh_home";' in HELPERS
    assert 'const HOME_REDIRECT_COOKIE = "forkmesh_home";' in HOME_AUTH
    assert '"dashboard" : "stay"' in HELPERS
    assert "Domain=.forkmesh.com" in HELPERS
    for source in (HELPERS, HOME_AUTH):
        assert '!entry || entry.slice(HOME_REDIRECT_COOKIE.length + 1) !== "stay"' in source


def test_homepage_redirect_lives_in_the_header_script_not_index_html():
    assert "forkmesh.session" not in INDEX_HTML
    assert "location.replace(DASHBOARD_HREF)" in HOME_AUTH
    assert '"https://app.forkmesh.com/dashboard"' in HOME_AUTH
    allowed = HOME_AUTH[HOME_AUTH.index("const homeRedirectAllowed"):]
    allowed = allowed[:allowed.index(";")]
    for guard in ('location.pathname === "/"', "!location.hash", "!location.search",
                  "homeRedirectEnabled()", "!arrivedFromForkMesh()"):
        assert guard in allowed


def _run_home(tmp_path, cookie, referrer, stored, path="/", hash_=""):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path / "harness.mjs"
    harness.write_text(
        """
import fs from "node:fs";
const [script, cookie, referrer, stored, path, hash] = process.argv.slice(2);
const replaced = [];
const store = new Map(stored === "1" ? [["forkmesh.session", JSON.stringify({ nodeName: "ada", kind: "user" })]] : []);
const storage = (map) => ({
  getItem: (key) => (map.has(key) ? map.get(key) : null),
  setItem: (key, value) => map.set(key, String(value)),
  removeItem: (key) => map.delete(key),
});
globalThis.localStorage = storage(store);
globalThis.sessionStorage = storage(new Map([["forkmesh.membershipProjection.v1", JSON.stringify({ account: "ada", membership: null, ts: Date.now() })]]));
globalThis.location = { hostname: "forkmesh.com", pathname: path, hash, search: "", replace: (href) => replaced.push(href) };
globalThis.document = { cookie, referrer, hidden: false, querySelector: () => null, addEventListener() {} };
globalThis.window = { addEventListener() {} };
globalThis.BroadcastChannel = class { addEventListener() {} };
globalThis.fetch = async () => ({ status: 200, ok: true, json: async () => ({}) });
new Function(fs.readFileSync(script, "utf8"))();
await new Promise((resolve) => setTimeout(resolve, 10));
console.log(JSON.stringify(replaced));
""",
        encoding="utf-8",
    )
    result = subprocess.run(
        [node, str(harness), str(ROOT / "www" / "public" / "home-header-auth.js"),
         cookie, referrer, stored, path, hash_],
        capture_output=True, text=True, timeout=30, check=True,
    )
    return result.stdout.strip()


def test_signed_in_visit_opens_dashboard_by_default(tmp_path):
    assert _run_home(tmp_path, "", "", "1") == '["https://app.forkmesh.com/dashboard"]'
    assert _run_home(tmp_path, "forkmesh_home=dashboard", "https://news.example/", "1") == (
        '["https://app.forkmesh.com/dashboard"]'
    )


def test_redirect_is_skipped_when_off_signed_out_or_internal(tmp_path):
    assert _run_home(tmp_path, "forkmesh_home=stay", "", "1") == "[]"
    assert _run_home(tmp_path, "", "", "0") == "[]"
    assert _run_home(tmp_path, "", "https://app.forkmesh.com/dashboard", "1") == "[]"
    assert _run_home(tmp_path, "", "https://forkmesh.com/docs", "1") == "[]"
    assert _run_home(tmp_path, "", "", "1", hash_="#pricing") == "[]"

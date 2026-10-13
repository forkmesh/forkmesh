"""The dashboard rebuilds a missing login marker from the App account cookie.

The account session is an HttpOnly cookie on app.forkmesh.com; the
``forkmesh.session`` localStorage entry the dashboard renders from is only a
copy. When the copy went missing while the cookie was live, the dashboard showed
a signed-in visitor as a guest (and sent Settings to /login) while the homepage
and World still knew the account. The boot now restores the copy through the
shared api-client before the chrome decides who is signed in, and Settings and
the account menu can sign out every device at once.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
HELPERS = (PUBLIC / "dashboard" / "js" / "02-helpers.js").read_text(encoding="utf-8")
ACCOUNT = (PUBLIC / "dashboard" / "js" / "04-account.js").read_text(encoding="utf-8")
NETWORK = (PUBLIC / "dashboard" / "js" / "08-repo-detail-network.js").read_text(
    encoding="utf-8")
BOOT = (PUBLIC / "dashboard" / "js" / "10-boot.js").read_text(encoding="utf-8")


def _restore_source():
    start = HELPERS.index("const CONTROLLER_SESSION_RESTORE_BUDGET_MS")
    end = HELPERS.index("function clearForkMeshBrowserState()")
    # A short budget keeps the late-answer case fast.
    return HELPERS[start:end].replace(
        "CONTROLLER_SESSION_RESTORE_BUDGET_MS = 1500",
        "CONTROLLER_SESSION_RESTORE_BUDGET_MS = 30",
    )


HARNESS = r'''
const [source, casesJson] = process.argv.slice(1);
const cases = JSON.parse(casesJson);
const results = {};
for (const c of cases) {
  let reloads = 0;
  let restoreCalls = 0;
  const location = { hostname: c.hostname, reload: () => { reloads += 1; } };
  const window = {
    setTimeout, clearTimeout,
    ForkMeshAPI: {
      restoreSession: () => {
        restoreCalls += 1;
        return new Promise((resolve) => setTimeout(() => resolve(c.restored), c.delay));
      },
    },
  };
  const OFFICIAL_SERVICE_HOSTS = new Set(["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"]);
  const readSession = () => c.marker;
  const restoreControllerSession = new Function(
    "location", "window", "OFFICIAL_SERVICE_HOSTS", "readSession",
    source + "\nreturn restoreControllerSession;",
  )(location, window, OFFICIAL_SERVICE_HOSTS, readSession);
  const value = await restoreControllerSession();
  await new Promise((r) => setTimeout(r, (c.delay || 0) + 50));
  results[c.name] = { value, reloads, restoreCalls };
}
console.log(JSON.stringify(results));
'''


def _run(cases):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [node, "--input-type=module", "-e", HARNESS, "--",
         _restore_source(), json.dumps(cases)],
        capture_output=True, text=True, timeout=30, check=True,
    )
    return json.loads(completed.stdout)


def test_missing_marker_is_restored_from_the_app_cookie():
    session = {"nodeName": "theo", "kind": "user", "sessionToken": "cookie"}
    results = _run([
        {"name": "restored", "hostname": "app.forkmesh.com", "marker": None,
         "restored": session, "delay": 0},
        {"name": "guest", "hostname": "app.forkmesh.com", "marker": None,
         "restored": None, "delay": 0},
        {"name": "marker", "hostname": "app.forkmesh.com",
         "marker": {"nodeName": "theo"}, "restored": session, "delay": 0},
        # Mirrors hydrate from their own portable cookie instead.
        {"name": "mirror", "hostname": "mirror35.forkmesh.com", "marker": None,
         "restored": session, "delay": 0},
        {"name": "late", "hostname": "app.forkmesh.com", "marker": None,
         "restored": session, "delay": 120},
        {"name": "late-guest", "hostname": "app.forkmesh.com", "marker": None,
         "restored": None, "delay": 120},
    ])
    assert results["restored"] == {"value": session, "reloads": 0, "restoreCalls": 1}
    assert results["guest"] == {"value": None, "reloads": 0, "restoreCalls": 1}
    assert results["marker"]["restoreCalls"] == 0
    assert results["mirror"]["restoreCalls"] == 0
    # A slow answer does not hold the page; it reloads once into the signed-in
    # view, and a slow 401 changes nothing.
    assert results["late"] == {"value": None, "reloads": 1, "restoreCalls": 1}
    assert results["late-guest"] == {"value": None, "reloads": 0, "restoreCalls": 1}


def test_boot_restores_before_the_chrome_reads_the_session():
    restore = BOOT.index("await restoreControllerSession();")
    assert BOOT.index("await hydratePortableMirrorSession();") < restore
    assert restore < BOOT.index("initSharedChrome()")


def test_every_device_can_be_signed_out_from_settings_and_the_account_menu():
    assert 'accountSessionApi("DELETE", "/all")' in ACCOUNT
    revoke_all = ACCOUNT[ACCOUNT.index("async function revokeAllAccountSessions()"):]
    revoke_all = revoke_all[:revoke_all.index("\n  }\n") + 4]
    assert "logout();" in revoke_all
    assert "[data-account-sessions-revoke-all]" in ACCOUNT
    assert "[data-account-menu-logout-all]" in NETWORK

    settings = (PUBLIC / "dashboard" / "settings" / "index.html").read_text(
        encoding="utf-8")
    assert "data-account-sessions-revoke-all" in settings
    menus = [
        path for path in PUBLIC.rglob("*.html")
        if "data-account-menu-logout " in path.read_text(encoding="utf-8")
    ]
    assert menus
    missing = [
        str(path.relative_to(PUBLIC)) for path in menus
        if "data-account-menu-logout-all" not in path.read_text(encoding="utf-8")
    ]
    assert missing == []

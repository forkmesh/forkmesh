"""The /dashboard/runners page: web sign-in of a runner's provider CLIs.

Source-text contracts for the page wiring and the admin-only nav link; the
endpoints themselves are covered by test_runner_logins.py.
"""

import re
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402


PUBLIC = ROOT / "public"
DASHBOARD = PUBLIC / "dashboard"
RUNNERS_JS = (DASHBOARD / "js" / "09-runners.js").read_text(encoding="utf-8")
ACCOUNT_JS = (DASHBOARD / "js" / "04-account.js").read_text(encoding="utf-8")
BOOT_JS = (DASHBOARD / "js" / "10-boot.js").read_text(encoding="utf-8")
VIEW = (DASHBOARD / "partials" / "views" / "runners.html").read_text(encoding="utf-8")
SIDEBAR = (DASHBOARD / "partials" / "sidebar.html").read_text(encoding="utf-8")
BUNDLE = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))


def function_source(name):
    match = re.search(
        r"^  (?:async )?function %s\(.*?(?=^  (?:async )?function |\Z)" % re.escape(name),
        RUNNERS_JS, re.M | re.S)
    assert match, name
    return match.group(0)


def test_runners_page_is_a_composed_dashboard_page():
    page = dashboard_shell.PAGES["runners"]
    assert page["route"] == "/dashboard/runners"
    assert page["view"] == "runners" and page["nav"] == "runners"
    assert page["asset"] == "dashboard/runners/index.html"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/runners"] == page["asset"]
    assert "/dashboard/runners/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    first = WRANGLER["assets"]["run_worker_first"]
    assert "!/dashboard/runners" in first and "!/dashboard/runners/" in first
    built = (PUBLIC / page["asset"]).read_text(encoding="utf-8")
    assert built.count("<section data-view=") == 1
    assert 'data-page="runners"' in built
    for marker in ("data-runners-list", "data-runner-login-output",
                   "data-runner-login-input", "data-runner-login-cancel",
                   "data-runner-login-links", 'maxlength="4000"'):
        assert marker in built, marker
    assert re.search(r"<div data-runner-login hidden", built)


def test_fragment_and_boot_are_wired_and_bundled():
    assert "09-runners.js" in dashboard_bundle.FRAGMENTS
    assert '"runners": initRunnersPage,' in BOOT_JS
    assert "function initRunnersPage()" in RUNNERS_JS
    assert RUNNERS_JS in BUNDLE


def test_nav_link_is_shown_to_every_account():
    link = SIDEBAR[SIDEBAR.index('data-nav="runners"'):]
    link_tag = link[:link.index(">")]
    link = link[:link.index("</a>")]
    assert 'href="/dashboard/runners"' in link_tag
    assert "data-admin-only" not in link_tag
    assert "hidden" not in link_tag
    assert link.rstrip().endswith("Runners")
    gate = function_source("renderAdminOnlyNavigation")
    assert 'classList.toggle("hidden", session?.isAdmin !== true)' in gate
    assert "startRunnerAttention(session);" in gate
    assert "renderAdminOnlyNavigation(session);" in ACCOUNT_JS
    home = (PUBLIC / dashboard_shell.PAGES["home"]["asset"]).read_text(encoding="utf-8")
    assert re.search(r'<a data-nav="runners" data-nav-link href="/dashboard/runners">', home)


def test_page_uses_the_login_endpoints_safely():
    for endpoint in ('"/api/runners"', "/login`",
                     "/output?after=${login.seq}`", "/input`", "/cancel`"):
        assert endpoint in RUNNERS_JS, endpoint
    request = function_source("runnersRequest")
    assert 'credentials: "same-origin"' in request
    assert "headers.authorization = `Bearer ${token}`" in request
    # Terminal text and URLs only ever reach the DOM as text / href.
    assert "innerHTML" not in RUNNERS_JS
    assert "insertAdjacentHTML" not in RUNNERS_JS
    render = function_source("renderRunnerLogin")
    assert "output.textContent = text" in render
    assert 'anchor.rel = "noopener noreferrer"' in render
    urls = function_source("runnerLoginUrls")
    assert 'url.protocol === "https:"' in urls
    # The pasted code is sent with a carriage return, like Enter in the pty.
    assert '{ data: value + "\\r" }' in function_source("sendRunnerLoginInput")
    # Polling is bounded to an open panel and stops on a final state.
    poll = function_source("pollRunnerLogin")
    assert "runnersView.login !== login" in poll
    assert "!RUNNER_LOGIN_ACTIVE.has(login.state)" in poll
    assert "setInterval" not in RUNNERS_JS


def test_a_failed_agent_session_link_focuses_its_runner_without_starting_a_sign_in():
    init = function_source("initRunnersPage")
    assert "runnersView.focus = requestedRunnerFocus();" in init
    requested = function_source("requestedRunnerFocus")
    assert 'new URLSearchParams(window.location.search)' in requested
    assert "RUNNER_PROVIDERS.some((entry) => entry.id === provider)" in requested
    pick = function_source("runnerForTarget")
    assert 'runner.status === "active"' in pick
    assert "active.length === 1" in pick
    focus = function_source("focusRequestedRunner")
    assert "runnersView.focus = null;" in focus
    assert "button.focus()" in focus
    assert "is not a runner you can manage" in focus
    before_connect = focus[:focus.index("if (focus.connect) {")]
    assert "startRunnerLogin" not in before_connect
    connect = focus[focus.index("if (focus.connect) {"):]
    assert connect.index("dropConnectParam();") < connect.index("startRunnerLogin")
    assert "attachRunnerLogin(name, provider.id, active.id)" in connect
    assert 'connect: params.get("connect") === "1"' in requested
    load = function_source("loadRunners")
    assert load.index("renderRunners();\n    focusRequestedRunner();") > load.index("return;")

#!/usr/bin/env python3
"""Issue #479: anonymous public browsing never redirects to registration.

A signed-out visitor may be asked to sign in only at a write boundary (star,
fork, comment, new issue or pull request) or on a page that is about their
own account. Every public read (repository, code, issues, pull requests,
releases, availability/mirrors) and the stable HTTPS clone URL stay open.
The browser half of this contract is
``app/browser_tests/tests/anonymous-public-reading.spec.js``.
"""

import re
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
SRC = APP / "src"
PUBLIC = APP / "public"
DOCS = APP.parent / "www" / "docs"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import static_routes  # noqa: E402

DASHBOARD_JS = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
AUTH_TARGET = re.compile(r"""["'`]/(?:login|signup)(?:\?|["'`])""")
NAVIGATION = re.compile(r"location\.(?:href\s*=|assign\(|replace\()")

PUBLIC_READ_VIEWS = (
    "issues", "pulls", "releases", "mirrors", "branches", "commits",
    "discussions", "insights", "projects",
)

# Each client-side bounce to /login, keyed by the guard that must sit just
# above it. A new entry needs a reason it is a write or account boundary.
ALLOWED_BOUNCE_GUARDS = (
    "!state.session?.nodeName",     # issue/pull compose, issue actions
    "!state.session?.sessionToken", # star, fork, MCP prompt, move-to-initiative
    "response.status === 401",      # write request the relay refused
    "grant && !session?.nodeName",  # node link grant must attach to an account
    "if (accountPage)",             # settings and the signed-in profile only
)


def test_every_public_repository_view_is_a_worker_served_repo_document():
    for view in PUBLIC_READ_VIEWS:
        path = "/acme/widgets/" + view
        assert static_routes.looks_like_repo_route(path), path
        assert static_routes.external_site_route(path) is None, path
    assert static_routes.looks_like_repo_route("/acme/widgets")
    assert static_routes.looks_like_repo_route("/acme/widgets/pulls/93")
    assert static_routes.looks_like_repo_route("/acme/widgets/blob/main/README.md")
    # The alias table sends familiar forge URLs to the readable page, never to
    # an auth page.
    assert static_routes.repo_alias_redirect("/acme/widgets/pull/7", "") == (
        "/acme/widgets/pulls/7")


def test_worker_login_redirects_are_admin_only():
    offenders = []
    for source in sorted(SRC.glob("*.py")):
        lines = source.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            if not re.search(r"""["']location["']\s*[:=,]?.*["']/(login|signup)""",
                             line) and not re.search(
                                 r"location=\(?\s*$", line):
                continue
            window = "\n".join(lines[max(0, index - 30):index + 3])
            if not re.search(r"/(login|signup)", window):
                continue
            if "/signup" in line and "referral" in window.lower():
                continue  # /r/<name> share links are an explicit signup CTA
            if "admin" not in window.lower():
                offenders.append(f"{source.name}:{index + 1}")
    assert offenders == []


def test_client_login_bounces_sit_behind_a_write_or_account_boundary():
    lines = DASHBOARD_JS.splitlines()
    unguarded = []
    count = 0
    for index, line in enumerate(lines):
        if not (NAVIGATION.search(line) and (
                AUTH_TARGET.search(line)
                or AUTH_TARGET.search(lines[index + 1] if index + 1 < len(lines) else ""))):
            continue
        count += 1
        above = "\n".join(lines[max(0, index - 6):index])
        if not any(guard in above for guard in ALLOWED_BOUNCE_GUARDS):
            unguarded.append(f"dashboard.js:{index + 1}: {line.strip()}")
    assert count > 0, "bounce scan found nothing; the pattern has drifted"
    assert unguarded == []


def test_guest_boot_keeps_public_pages_open():
    start = DASHBOARD_JS.index("function initSharedChrome()")
    boot = DASHBOARD_JS[start:DASHBOARD_JS.index("function initHomePage()", start)]
    guest = boot[boot.index("const guest ="):]
    account_page = guest[guest.index("const accountPage"):guest.index("if (accountPage)")]
    # Only the settings document and the viewer's own profile are account
    # pages; a /@name public profile and every repository route are not.
    assert 'page === "settings"' in account_page
    assert "!publicProfileNameFromPath()" in account_page
    assert re.findall(r'page === "([a-z-]+)"', account_page) == [
        "settings", "profile", "profile-repositories"]
    # Guests get a sign-in link in the header instead of a redirect, and the
    # repository catalog still loads for them.
    assert "[data-guest-auth-link]" in guest
    assert "repositoriesReady = loadRepositories();" in guest


def test_accountless_reading_is_documented_and_linked():
    doc = (DOCS / "accountless-reading.md").read_text(encoding="utf-8")
    lowered = doc.lower()
    for phrase in (
        "without an account", "git clone", "releases", "mirrors",
        "never redirect", "sign in", "browser identity", "not yet",
        "anonymous-public-reading.spec.js", "test_anonymous_public_reading.py",
    ):
        assert phrase in lowered or phrase in doc, phrase
    readme = (DOCS / "README.md").read_text(encoding="utf-8")
    assert "(accountless-reading.md)" in readme

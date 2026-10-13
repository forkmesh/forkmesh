"""The landing-page header must not outlive the session it is showing.

The account session is an HttpOnly cookie on the App origin, while the
marker these pages read lives in this origin's localStorage. Signing out on
app.forkmesh.com therefore cannot reach the copy, so the copy has to expire
itself: the header asks the session API, and a 401 - and only a 401 - means
signed out. A 5xx or a dead network means we could not ask, and network loss
is not a logout.

This regressed once as a stale account chip that survived every reload on
forkmesh.com until browser storage was cleared by hand.
"""

from pathlib import Path

WWW = Path(__file__).resolve().parents[1] / "public"
HOME_AUTH = (WWW / "home-header-auth.js").read_text(encoding="utf-8")
SITE_HEADER = (WWW / "site-header.js").read_text(encoding="utf-8")


def _without_comments(source):
    """Source with // comments dropped, so a claim cannot match its own prose."""
    lines = []
    for line in source.splitlines():
        marker = line.find("//")
        if marker >= 0 and line[:marker].count('"') % 2 == 0 and (
            line[:marker].count("'") % 2 == 0
        ):
            line = line[:marker]
        lines.append(line)
    return "\n".join(lines)


HOME_CODE = _without_comments(HOME_AUTH)


def test_a_revoked_session_clears_the_local_marker():
    assert "function clearLocalSession()" in HOME_CODE
    assert 'localStorage.removeItem("forkmesh.session")' in HOME_CODE
    assert "sessionStorage.removeItem(MEMBERSHIP_CACHE_KEY)" in HOME_CODE
    # The clear hangs off the 401 branch, not off any failure.
    unauthorized = HOME_CODE.index("response.status === 401")
    following = HOME_CODE[unauthorized:unauthorized + 200]
    assert "clearLocalSession()" in following


def test_a_server_error_is_not_a_logout():
    # The two outcomes are separate branches: a 401 signs out, anything else
    # only drops the cached membership and leaves the marker for a retry.
    assert "response.status === 401 || !response.ok" not in HOME_CODE
    not_ok = HOME_CODE.index("!response.ok")
    branch = HOME_CODE[not_ok:not_ok + 160]
    assert "clearLocalSession" not in branch
    # The network-failure path must not sign out either.
    catch = HOME_CODE.rindex(".catch(")
    assert "clearLocalSession" not in HOME_CODE[catch:catch + 160]


def test_an_open_page_revalidates_on_focus_not_on_a_timer():
    assert "function startSessionWatch()" in HOME_CODE
    assert "startSessionWatch();" in HOME_CODE
    assert 'window.addEventListener("focus", validate)' in HOME_CODE
    assert 'document.addEventListener("visibilitychange"' in HOME_CODE
    # A landing page left open must not spend an authenticated read on a
    # timer, and an alt-tab burst must not become one.
    assert "SESSION_REVALIDATE_MIN_MS" in HOME_CODE
    assert "setInterval" not in HOME_CODE


def test_both_headers_apply_the_same_rule():
    # site-header.js covers docs, blog and the other www pages;
    # home-header-auth.js covers the homepage and the hackathon pages. A
    # visitor must not get a different answer depending on which page they
    # happen to land on.
    for source in (HOME_CODE, _without_comments(SITE_HEADER)):
        assert "response.status === 401" in source
        assert "SESSION_REVALIDATE_MIN_MS" in source
        assert 'BroadcastChannel("forkmesh.session")' in source

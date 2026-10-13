#!/usr/bin/env python3
"""The root keeps the regular site and embeds ForkMesh World."""

import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WWW_PUBLIC = ROOT.parent / "www" / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from static_routes import dashboard_section_redirect  # noqa: E402

WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
ENTRY_TEXT = (SRC / "app_runtime.py").read_text(encoding="utf-8")
INDEX_HTML = (WWW_PUBLIC / "index.html").read_text(encoding="utf-8")
WORLD_HTML = (WORLD_PUBLIC / "world" / "index.html").read_text(encoding="utf-8")

COOKIE_SET = 'forkmesh_session=1; Path=/; Max-Age=2592000; SameSite=Lax'
COOKIE_CLEAR = 'forkmesh_session=; Path=/; Max-Age=0; SameSite=Lax'


def _read(rel):
    source = PUBLIC / rel
    if not source.is_file():
        source = WWW_PUBLIC / rel
    return source.read_text(encoding="utf-8")


def test_homepage_no_longer_client_redirects():
    assert 'location.replace("/dashboard")' not in INDEX_HTML
    assert "forkmesh.session" not in INDEX_HTML
    assert 'data-world-mode="public"' in WORLD_HTML
    # The World embed moved from a mid-page index.html section to a
    # site-footer.js band pinned to the bottom of every page (adhoc #280).
    footer_js = _read("site-footer.js")
    assert 'src="/world/"' not in INDEX_HTML
    assert (
        'const WORLD_EMBED_URL = "https://world.forkmesh.com/world/"'
        in footer_js
    )
    assert 'src="${WORLD_EMBED_URL}"' in footer_js
    assert 'title="Interactive ForkMesh World"' in footer_js
    assert "Open World full screen" in footer_js
    assert "repositories, source code, documentation" in footer_js
    assert 'src="/site-footer.js?v=${version}"' in INDEX_HTML


def test_worker_owns_root_without_cookie_routing():
    assert "/" in WRANGLER["assets"]["run_worker_first"]
    root_branch = ENTRY_TEXT[
        ENTRY_TEXT.index('if url.path == "/" and method_name(request)'):
        ENTRY_TEXT.index("if url.path in BLOCKED_STATIC_HTML_PATHS")
    ]
    assert '_cookie_value(request, "forkmesh_session")' not in root_branch
    assert '"location": "/dashboard"' not in root_branch
    assert "return await self._serve_homepage(url)" in root_branch


def test_regular_homepage_revalidates_without_varying_on_cookie():
    serve = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _serve_homepage"):
        ENTRY_TEXT.index("async def _serve_homepage") + 1500
    ]
    assert '"cache-control": "no-cache"' in serve
    assert '"vary": "cookie"' not in serve
    assert 'base + "index.html"' in serve
    assert 'base + "world/index.html"' not in serve


def test_presence_cookie_lifecycle_is_complete():
    # Set on login/signup and on dashboard session writes, with Secure on https.
    for rel in ("login.js", "signup.js", "dashboard.js"):
        text = _read(rel)
        assert COOKIE_SET in text, rel
        assert '(location.protocol === "https:" ? "; Secure" : "")' in text, rel
    # Cleared on dashboard logout, on the marketing-page header logout (the
    # pre-refactor gap: it only removed localStorage, leaving / redirecting),
    # and by the dashboard guest branch as a self-heal when localStorage says
    # logged out but the cookie survived.
    assert COOKIE_CLEAR in _read("site-header.js")
    assert _read("dashboard.js").count(COOKIE_CLEAR) >= 2


def test_legacy_section_urls_redirect_permanently():
    assert dashboard_section_redirect("/dashboard", "section=repos") == "/dashboard/repos"
    assert dashboard_section_redirect("/dashboard", "section=network") == "/dashboard/network"
    assert dashboard_section_redirect("/dashboard", "section=chat") == "/dashboard/chat"
    assert dashboard_section_redirect("/dashboard", "section=home") == "/dashboard"
    assert dashboard_section_redirect("/dashboard", "section=profile") == "/dashboard/settings"
    assert dashboard_section_redirect("/dashboard", "section=profile-overview") == "/dashboard/profile"
    assert (dashboard_section_redirect("/dashboard", "section=profile-repositories")
            == "/dashboard/profile/repositories")
    # Leftover params survive the redirect — ?repo=, ?mock=, link grants.
    assert (dashboard_section_redirect("/dashboard", "section=repos&mock=1")
            == "/dashboard/repos?mock=1")
    assert (dashboard_section_redirect(
        "/dashboard", "link_node=x&link_ts=1&link_sig=s&section=profile")
        == "/dashboard/settings?link_node=x&link_ts=1&link_sig=s")
    # No redirect without a recognized section, and never off /dashboard.
    assert dashboard_section_redirect("/dashboard", "section=bogus") is None
    assert dashboard_section_redirect("/dashboard", "") is None
    assert dashboard_section_redirect("/dashboard", "mock=1") is None
    assert dashboard_section_redirect("/dashboard/repos", "section=chat") is None


def test_desktop_link_grant_urls_still_reach_the_nodes_panel():
    # The desktop app hard-codes /dashboard?link_node=... — boot forwards the
    # grant (params untouched) to the settings document where the panel lives.
    js = _read("dashboard.js")
    assert 'location.replace("/dashboard/settings" + location.search)' in js
    assert "pendingLinkGrant()" in js

"""Static URL ownership for the ForkMesh public site.

Clean marketing/auth routes are served by Workers Static Assets from
``public/_redirects``. Repo shortcuts are Worker-owned so hard-refreshing
``/owner/repo`` and tab/tree/blob deep links can serve the repo-detail page
without relying on a client-side 404 bounce. Direct implementation-file URLs
such as ``/login.html`` are intentionally blocked.

The dashboard is true separate pages (one built document per page, see
``dashboard_shell.PAGES``). Known page shells are asset-served; the Worker owns
unrecognized dashboard and repository deep links.
"""

import re
from urllib.parse import parse_qs, unquote, urlencode, urlsplit

from plugin_registry import (
    PLUGIN_DASHBOARD_PAGE_ASSETS,
    PLUGIN_STATIC_HTML_PATHS,
    PLUGIN_WWW_SITE_ROUTES,
)

ECOSYSTEM_SLUG_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")
MARKET_PACKAGE_RE = re.compile(
    r"^(?=.{1,96}$)[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
MARKET_ASSET_FOLDERS = frozenset({"previews"})


REPO_TAB_ROUTES = frozenset({
    "branches",
    "commits",
    "releases",
    "issues",
    "projects",
    "pulls",
    "actions",
    "discussions",
    "mirrors",
    "agents",
    "insights",
    "sizemap",
    "settings",
    "security",
})

# Repository deep links that are not tabs but still belong to the repo
# document: code tree/blob paths and the branch comparison page
# (/owner/repo/compare/<base>...<head>, the web diff viewer an agent's branch
# links to before any pull request exists). A path family missing here is
# answered with the Worker's JSON 404 on a hard refresh or a pasted link.
REPO_DOCUMENT_ROUTES = frozenset({
    "tree",
    "blob",
    "compare",
    "pull",
})

CORE_RESERVED_ROUTE_PREFIXES = frozenset({
    "api",
    "assets",
    "favicon",
    "dashboard",
    "docs",
    "blogs",
    "blog",
    "login",
    "signup",
    "forgot-password",
    "reset-password",
    # The pending-verification hold-out screen (issue #592).
    "verify-pending",
    "mirror-payouts",
    "security-report",
    "advisories",
    "security-researchers",
    "network",
    "world",
    "chat",
    "desktop",
    "mobile",
    "about",
    "features",
    "market",
    "pricing",
    "press",
    "careers",
    "changelog",
    "privacy",
    "terms",
    "status",
    "referral",
    # The retired plural URL still 308s to /referral, so it stays reserved.
    "referrals",
    "microjobs",
    "hackathon",
    "ecosystem",
    "leaderboards",
    "ledger",
    "notes",
    "office",
    "start",
    # Referral share links (/r/<name>) are worker-owned counters.
    "r",
    "admin",
    "ap",
    "client-error",
    "cron-runner",
    "desktop-error",
    "developers",
    "health",
    "healthz",
    "readyz",
    "mcp",
    "nodeinfo",
    "oauth",
    "oauth2",
    "pitch-deck",
    "v1",
    "ws",
    "profile",
})

RESERVED_ROUTE_PREFIXES = CORE_RESERVED_ROUTE_PREFIXES | PLUGIN_WWW_SITE_ROUTES

# Pages of the hackathon campaign that are plain marketing documents: no
# account, no live per-visitor data, so they are static assets on the
# marketing origin like /features. Everything else under /hackathon (the
# personalised invite and squad pages, and the participant HQ) needs D1 and
# is owned by the App Worker.
WWW_HACKATHON_PAGES = frozenset({"archive", "leaderboard", "projects", "rules"})
WWW_HACKATHON_ARCHIVE_PAGES = frozenset({
    "revenue-contest-2026", "revenue-contest-2026-rules"})

WWW_SITE_ROUTES = frozenset({
    "about",
    "affiliates",
    "blog",
    "careers",
    "changelog",
    "desktop",
    "docs",
    "ecosystem",
    "features",
    "hackathon",
    "leaderboards",
    "market",
    "mobile",
    "network",
    "press",
    "pricing",
    "privacy",
    "security-report",
    "status",
    "terms",
}) | PLUGIN_WWW_SITE_ROUTES

# Clean page URL -> the built per-page document that serves it. Nearly all of
# these are /dashboard pages; /referral and /microjobs are composed the same
# way but keep top-level public URLs the marketing chrome links directly.
DASHBOARD_PAGE_ASSETS = {
    "/referral": "referral/index.html",
    "/hackathon/me": "hackathon/me/index.html",
    "/microjobs": "microjobs/index.html",
    "/dashboard": "dashboard/index.html",
    "/dashboard/repos": "dashboard/repos/index.html",
    "/dashboard/repos/new": "dashboard/repos/new/index.html",
    "/dashboard/network": "dashboard/network/index.html",
    "/dashboard/ads": "dashboard/ads/index.html",
    "/dashboard/challenges": "dashboard/challenges/index.html",
    "/dashboard/agents": "dashboard/agents/index.html",
    "/dashboard/bounties": "dashboard/bounties/index.html",
    "/dashboard/links": "dashboard/links/index.html",
    "/dashboard/achievements": "dashboard/achievements/index.html",
    "/dashboard/runners": "dashboard/runners/index.html",
    "/dashboard/docs": "dashboard/docs/index.html",
    "/dashboard/apps": "dashboard/apps/index.html",
    "/dashboard/apps/build": "dashboard/apps/build/index.html",
    "/dashboard/settings": "dashboard/settings/index.html",
    "/dashboard/profile": "dashboard/profile/index.html",
    "/dashboard/profile/repositories": "dashboard/profile/repositories/index.html",
    **PLUGIN_DASHBOARD_PAGE_ASSETS,
}

DASHBOARD_PAGE_SEGMENTS = frozenset(
    route.split("/")[2] for route in DASHBOARD_PAGE_ASSETS
    if route.startswith("/dashboard/")) | frozenset({
        "repos", "settings", "profile", "workflows", "tasks", "calendar",
        "notes"})

SHORT_NAME_RESERVED = (
    RESERVED_ROUTE_PREFIXES | DASHBOARD_PAGE_SEGMENTS
    | frozenset(route.strip("/").split("/")[0] for route in DASHBOARD_PAGE_ASSETS)
    | frozenset({"explore", "forgot", "profile"}))

# The repo-detail document, fetched by the Worker for every /owner/repo[...]
# route. Deliberately not a navigable asset path of its own.
DASHBOARD_REPO_ASSET = "dashboard/repo.html"

# Legacy /dashboard?section=X names -> the clean per-page paths. 308s are
# browser-cached permanently, so this mapping is a one-way door — keep it in
# sync with dashboard_shell.PAGES routes.
DASHBOARD_SECTION_PATHS = {
    "home": "/dashboard",
    "repos": "/dashboard/repos",
    "network": "/dashboard/network",
    "chat": "/chat",
    "profile": "/dashboard/settings",
    "profile-overview": "/dashboard/profile",
    "profile-repositories": "/dashboard/profile/repositories",
}


def dashboard_section_redirect(path, query):
    """308 target for legacy ``/dashboard?section=X`` URLs, else ``None``.

    Leftover query params are preserved — ``?repo=``, ``?mock=`` and the
    desktop app's ``?link_node=...`` grant params are live features.
    """
    if path != "/dashboard" or not query:
        return None
    params = parse_qs(query, keep_blank_values=True)
    section = (params.get("section") or [""])[0]
    target = DASHBOARD_SECTION_PATHS.get(section)
    if not target:
        return None
    rest = [(key, value) for key, values in params.items() if key != "section" for value in values]
    return target + ("?" + urlencode(rest) if rest else "")


# Retired page URLs -> their current path. Like the section 308s above this is
# a one-way door (permanent redirects are browser-cached), so an entry stays
# forever: the plural /referrals is still linked from posts, chat and old docs.
LEGACY_PAGE_PATHS = {
    "/referrals": "/referral",
    "/referrals/": "/referral",
    "/explore": "/dashboard/repos",
    "/explore/": "/dashboard/repos",
    "/forgot": "/forgot-password",
    "/forgot/": "/forgot-password",
    "/dashboard/chat": "/chat",
    "/dashboard/chat/": "/chat",
}

PROFILE_NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
PROFILE_PATH_RE = re.compile(
    r"^(?:/dashboard)?/profile/([^/]+)(/repositories)?/?$")


def profile_page_redirect(path, query):
    match = PROFILE_PATH_RE.match(str(path or ""))
    if not match:
        return None
    name = unquote(match.group(1)).lower().lstrip("@")
    if not PROFILE_NAME_RE.match(name) or name == "repositories":
        return None
    target = "/@" + name + (match.group(2) or "")
    return target + ("?" + query if query else "")


def account_page_name(path):
    parts = path_parts(path)
    if len(parts) != 1:
        return ""
    name = unquote(parts[0]).lower()
    if name in SHORT_NAME_RESERVED or not PROFILE_NAME_RE.match(name):
        return ""
    return name


def legacy_page_redirect(path, query):
    """308 target for a retired page URL, else ``None``. Keeps the query."""
    target = LEGACY_PAGE_PATHS.get(path)
    if not target:
        return None
    return target + ("?" + query if query else "")


# Familiar repository URLs from other forges -> the canonical repo route
# (issue #508). One pull request lives at /owner/repo/pull/12, GitHub style,
# and the list at /owner/repo/pulls; /pulls/12, /pull-requests/12 and GitLab's
# /-/merge_requests/12 land on /pull/12. Like the page redirects above these
# are permanent 308s, so an alias is never removed; see
# www/docs/terminology.json for the full compatibility table.
REPO_ROUTE_ALIASES = {
    "commit": "commits",
    "pull": "pulls",
    "pulls": "pulls",
    "pull-requests": "pulls",
    "merge_requests": "pulls",
    "merge-requests": "pulls",
}
PULL_RECORD_ROUTE = "pull"
PULL_NUMBER_RE = re.compile(r"^[1-9][0-9]{0,8}$")


def repo_alias_redirect(path, query):
    """308 target for an aliased repository deep link, else ``None``."""
    parts = path_parts(path)
    if len(parts) < 3 or unquote(parts[0]).lower() in RESERVED_ROUTE_PREFIXES:
        return None
    rest = parts[2:]
    gitlab_scope = rest[0] == "-" and len(rest) > 1
    if gitlab_scope:
        rest = rest[1:]
    kind = unquote(rest[0])
    canonical = REPO_ROUTE_ALIASES.get(kind)
    if canonical == "pulls" and len(rest) > 1 and PULL_NUMBER_RE.match(
            unquote(rest[1])):
        canonical = PULL_RECORD_ROUTE
    if canonical is None and gitlab_scope and (
            kind in REPO_TAB_ROUTES or kind in REPO_DOCUMENT_ROUTES):
        canonical = kind
    if canonical is None or (canonical == kind and not gitlab_scope):
        return None
    target = "/" + "/".join([parts[0], parts[1], canonical, *rest[1:]])
    return target + ("?" + query if query else "")


BLOCKED_STATIC_HTML_PATHS = frozenset({
    "/index.html",
    "/world/index.html",
    "/dashboard.html",
    "/dashboard/index.html",
    "/dashboard/repos/index.html",
    "/dashboard/repos/new/index.html",
    "/dashboard/network/index.html",
    "/dashboard/chat/index.html",
    "/dashboard/ads/index.html",
    "/dashboard/challenges/index.html",
    "/dashboard/workflows/index.html",
    "/dashboard/agents/index.html",
    "/dashboard/bounties/index.html",
    "/dashboard/links/index.html",
    "/dashboard/achievements/index.html",
    "/dashboard/runners/index.html",
    "/dashboard/docs/index.html",
    "/dashboard/apps/index.html",
    "/dashboard/apps/build/index.html",
    "/dashboard/tasks/index.html",
    "/dashboard/calendar/index.html",
    "/dashboard/notes/index.html",
    "/dashboard/settings/index.html",
    "/dashboard/profile/index.html",
    "/dashboard/profile/repositories/index.html",
    "/dashboard/repo.html",
    "/blog.html",
    "/login.html",
    "/signup.html",
    "/forgot-password.html",
    "/reset-password.html",
    "/verify-pending.html",
    "/mirror-payouts.html",
    "/start.html",
    "/referral/index.html",
    "/hackathon/me/index.html",
    "/microjobs/index.html",
    "/leaderboards.html",
    "/ledger.html",
    "/security-report.html",
    "/docs.html",
    "/network.html",
    "/chat.html",
    "/desktop.html",
    "/mobile.html",
    "/about.html",
    "/features.html",
    "/market.html",
    "/pricing.html",
    "/press.html",
    "/careers.html",
    "/changelog.html",
    "/ecosystem.html",
    "/privacy.html",
    "/terms.html",
    "/status.html",
}) | PLUGIN_STATIC_HTML_PATHS

def path_parts(pathname):
    return [part for part in (pathname or "").split("/") if part]


def external_site_route(pathname):
    parts = path_parts(pathname)
    if not parts:
        return None
    if parts[0] == "world":
        return "world"
    # /hackathon/leaderboard and /hackathon/rules are static marketing
    # documents; /hackathon/me, /hackathon/join/<name> and
    # /hackathon/squad/<code> stay with the Worker that has the data.
    if (parts[0] == "hackathon" and len(parts) == 2
            and unquote(parts[1]) in WWW_HACKATHON_PAGES):
        return "www"
    if (parts[0] == "hackathon" and len(parts) == 3 and parts[1] == "archive"
            and unquote(parts[2]) in WWW_HACKATHON_ARCHIVE_PAGES):
        return "www"
    if parts[0] in WWW_SITE_ROUTES and (
        len(parts) == 1 or parts[0] in {"blog", "docs"}
    ):
        return "www"
    if is_ecosystem_project_path(pathname):
        return "www"
    if is_market_package_path(pathname):
        return "www"
    return None


def is_ecosystem_project_path(pathname):
    parts = path_parts(pathname)
    return (len(parts) == 2 and parts[0] == "ecosystem"
            and ECOSYSTEM_SLUG_RE.match(parts[1]) is not None)


def is_market_package_path(pathname):
    parts = path_parts(pathname)
    return (len(parts) == 2 and parts[0] == "market"
            and parts[1] not in MARKET_ASSET_FOLDERS
            and MARKET_PACKAGE_RE.match(parts[1]) is not None)


def site_document_asset_path(pathname):
    if is_ecosystem_project_path(pathname):
        return "/ecosystem"
    if is_market_package_path(pathname):
        return "/market/" + path_parts(pathname)[1]
    return pathname


WORLD_IMMUTABLE_CACHE = "public, max-age=31536000, immutable"
WORLD_SHELL_CACHE = "no-store, max-age=0, must-revalidate"


def world_asset_path(pathname):
    parts = path_parts(pathname)
    if (not parts or parts[0] != "world"
            or any(unquote(part) in (".", "..") for part in parts)):
        return ""
    if len(parts) == 1:
        return "/world/index.html"
    return "/" + "/".join(parts)


def world_asset_cache_control(pathname, query):
    parts = path_parts(pathname)
    versioned = any(
        item.startswith("v=") and len(item) > 2
        for item in str(query or "").split("&"))
    if len(parts) > 1 and (versioned or parts[1] == "vendor"):
        return WORLD_IMMUTABLE_CACHE
    return WORLD_SHELL_CACHE


def looks_like_repo_route(pathname):
    parts = path_parts(pathname)
    if len(parts) < 2:
        return False

    owner = unquote(parts[0]).lower()
    if owner in RESERVED_ROUTE_PREFIXES:
        return False

    if len(parts) == 2:
        return True

    kind = unquote(parts[2])
    return kind in REPO_DOCUMENT_ROUTES or kind in REPO_TAB_ROUTES


def dashboard_repo_redirect(path):
    if not str(path or "").startswith("/dashboard/"):
        return None
    rest = path[len("/dashboard"):]
    if path_parts(rest)[:1] and unquote(
            path_parts(rest)[0]).lower() in DASHBOARD_PAGE_SEGMENTS:
        return None
    if (rest.startswith("//") or "\\" in rest
            or any(ord(ch) < 32 or ord(ch) == 127 for ch in rest)):
        return None
    return rest if looks_like_repo_route(rest) else None


# ---------------------------------------------------------------------------
# Origin policy for www-owned documents (issue #625)
# ---------------------------------------------------------------------------
#
# The account session is origin-scoped in both halves that matter: the
# HttpOnly session cookie is host-only (no Domain attribute) and the
# ``forkmesh.session`` marker lives in localStorage. A relay reached on its
# own hostname — a node subdomain such as ``pp.forkmesh.com``, a tunnel host,
# a preview host — therefore holds a session that exists on that host and
# nowhere else. Sending such a visitor to the canonical marketing origin
# presents the shared product header as signed out, and signing in again on
# the destination cannot help: the two origins hold different sessions.
#
# So a request that arrives on a host this deployment does not publish as one
# of its own origins keeps its documents: the in-product Docs and Blog links
# are served in place instead of redirected away. Requests that do arrive on a
# published origin keep the canonical split (app.forkmesh.com/docs still 308s
# to the www origin), which is what keeps one indexable URL per document.
SAME_ORIGIN_SITE_PREFIXES = ("/blog", "/docs")


def normalized_site_origin(value):
    """``scheme://host[:port]`` for an origin string, or ``""``."""
    origin = str(value or "").strip()
    if not origin:
        return ""
    try:
        parsed = urlsplit(origin)
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    scheme = parsed.scheme.lower()
    host = str(parsed.hostname or "").lower().rstrip(".")
    if (
        scheme not in ("http", "https")
        or not host
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
    ):
        return ""
    if ":" in host and not host.startswith("["):
        host = "[" + host + "]"
    default_port = (scheme == "https" and port == 443) or (
        scheme == "http" and port == 80)
    return scheme + "://" + host + (
        (":" + str(port)) if port is not None and not default_port else ""
    )


def in_product_site_document(pathname):
    """True for the www-owned documents the dashboard itself links to.

    Only these keep their visitor's origin. The marketing pages are entered
    from search and public links rather than from a signed-in session, so they
    stay on the one canonical origin that owns them.
    """
    parts = path_parts(pathname)
    # A fetch implementation may normalize dot segments after we have chosen
    # a delivery mode. Reject them here (including URL-escaped spellings) so a
    # /docs prefix cannot be used to relay another public page.
    if not parts or any(unquote(part) in (".", "..") for part in parts):
        return False
    path = "/" + "/".join(parts)
    return any(
        path == prefix or path.startswith(prefix + "/")
        for prefix in SAME_ORIGIN_SITE_PREFIXES
    )


def external_site_delivery(
        pathname, request_origin, owner_origin, published_origins):
    """How this Worker delivers a www/world-owned document.

    Returns ``(mode, origin)`` where mode is ``"redirect"`` (308 to ``origin``,
    the canonical split), ``"proxy"`` (serve ``origin``'s copy under the
    request's own origin), or ``"local"`` (serve whatever this deployment
    bundles, ``origin`` empty). ``None`` when the path is not externally
    owned.
    """
    owner = external_site_route(pathname)
    if not owner:
        return None
    request_origin = normalized_site_origin(request_origin)
    owner_origin = normalized_site_origin(owner_origin)
    if not owner_origin or owner_origin == request_origin:
        return ("local", "")
    published = {
        normalized_site_origin(item) for item in published_origins or ()
    }
    published.discard("")
    if request_origin in published or not request_origin:
        return ("redirect", owner_origin)
    if owner != "www" or not in_product_site_document(pathname):
        return ("redirect", owner_origin)
    return ("proxy", owner_origin)

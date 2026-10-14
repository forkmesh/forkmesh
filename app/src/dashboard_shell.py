"""Composition of the per-page dashboard documents from HTML partials.

The dashboard used to be one SPA shell: every ``/dashboard*`` URL served the
same ``dashboard/index.html`` with all views stacked as hidden ``[data-view]``
sections, and dashboard.js swapped them client-side after boot (a visible
flash-then-swap on deep links). It is now a set of true separate pages: one
document per page, each containing the shared chrome (header, sidebar, modals),
optional page-specific extras, and exactly one view from
``public/dashboard/partials/views/``.

``dashboard/shell.html`` keeps the ``<head>`` and outer body scaffold with

    <!--#include partial="name"-->      pulls in a shared partial
    <!--#include view-->                (inside the "main" partial) pulls in
                                        the page's single view file
    <!--#page token-->                  per-page metadata substitution
                                        (id, title, description, section,
                                        page-specific extras)

``tools/build_dashboard_assets.py`` composes one document per ``PAGES`` entry
before deploy so Cloudflare can serve them as static assets.

This module is intentionally js-free — no ``js``/``workers`` imports — so both
the Worker (on Cloudflare) and the test suite can import it directly, the same
way they import ``urls.py``. The composition is a pure string substitution, so
it is trivially unit-testable against the on-disk partials.
"""

import hashlib
import json
import re

# Placeholder the shell uses to pull in a partial: <!--#include partial="name"-->
INCLUDE_RE = re.compile(r'<!--#include partial="([a-z0-9-]+)"-->')

# Placeholder inside the "main" partial for the page's single view.
VIEW_INCLUDE_RE = re.compile(r"<!--#include view-->")

# Stable package-neutral insertion point. Marketplace package markup is added
# only to the ignored deployment stage; checked-in dashboard documents retain
# this marker and never acquire a package id, label, or route.
PLUGIN_NAV_MARKER = "<!-- forkmesh:plugin-navigation -->"

# Per-page metadata tokens in the shell: <!--#page id-->, <!--#page title-->, ...
PAGE_TOKEN_RE = re.compile(r"<!--#page ([a-z]+)-->")

THEME_TOKEN_RE = re.compile(r"/\*#theme (dark|light)\*/")
THEME_PATH = "dashboard/theme.json"

# Root-relative first-party script and stylesheet references, with an optional
# existing ``?v=`` cache-busting query. Source documents keep a visible
# ``?v=${version}`` placeholder; staging resolves copied counterparts to
# content-addressed URLs.
ASSET_REFERENCE_RE = re.compile(
    r'((?:src|href)="/([\w.-]+\.(?:js|css)))(?:\?v=[^"]*)?"'
)

# JavaScript loaders use an assignment rather than an HTML ``src`` attribute.
# Keep the same version map usable for those dynamic first-party loads too.
ASSET_ASSIGNMENT_RE = re.compile(
    r'(=\s*"/([\w.-]+\.js))(?:\?v=[^"]*)?"'
)

# Dynamic loaders keep their cache-busted URL readable in authored and staged
# JavaScript. The source placeholder is valid for local development; staging
# replaces only the adjacent ``version`` value with the target asset's hash.
ASSET_TEMPLATE_ASSIGNMENT_RE = re.compile(
    r'(const\s+version\s*=\s*)"__FORKMESH_ASSET_VERSION__"'
    r'(;\s*[\w$.]+\.src\s*=\s*`/([\w.-]+\.js)\?v=\$\{version\}`)'
)

# The dashboard's pages. Keys are page ids (also the <body data-page> value the
# JS boot dispatch keys off). "view" names the partials/views/<view>.html file;
# optional "extras" names partials that only that page needs; "section" is the
# legacy data-dashboard-section name (CSS hooks + old JS state names); "nav" is
# the sidebar data-nav link to mark active (None = no sidebar item); "route" is
# the public clean URL (None = worker-only, the repo detail document is fetched
# by the Worker for every /owner/repo[...] path); "asset" is the built
# public/-relative file.
PAGES = {
    "home": {
        "view": "home",
        "section": "home",
        "nav": "home",
        "route": "/dashboard",
        "asset": "dashboard/index.html",
        "title": "ForkMesh Dashboard",
        "description": "Manage ForkMesh repositories, mirrors, signed collaboration, and account activity from your dashboard.",
    },
    "repos": {
        "view": "repos",
        "extras": ("new-repo-modal",),
        "section": "repos",
        "nav": "repos",
        "route": "/dashboard/repos",
        "asset": "dashboard/repos/index.html",
        "title": "Repositories - ForkMesh",
        "description": "Browse repositories mirrored by ForkMesh desktop nodes and open them live from their hosts.",
    },
    "new-repo": {
        "view": "new-repo",
        "section": "new-repo",
        "nav": "repos",
        "route": "/dashboard/repos/new",
        "asset": "dashboard/repos/new/index.html",
        "title": "New repository - ForkMesh",
        "description": "Create a repository hosted by ForkMesh, import one from a git URL or a forge, or mirror one from your desktop node.",
    },
    "network": {
        "view": "network",
        "section": "network",
        "nav": "network",
        "route": "/dashboard/network",
        "asset": "dashboard/network/index.html",
        "title": "Network - ForkMesh",
        "description": "Relay health, connected nodes, and live network activity on ForkMesh.",
    },
    "chat": {
        "view": "chat",
        "section": "chat",
        "nav": "chat",
        "route": "/dashboard/chat",
        "asset": "dashboard/chat/index.html",
        "title": "Chat - ForkMesh",
        "description": "ForkMesh public #general chat is controller-readable so ForkBot and MCP can participate.",
    },
    # The referral program keeps its own top-level public URL (/referral is
    # linked from the site header, the site footer and the privacy page) and is
    # composed like any other dashboard page, so the asset sits outside
    # ``dashboard/`` — Cloudflare maps a clean URL onto the file at that path.
    # The plural /referrals it shipped as first 308s here (static_routes).
    "referrals": {
        "view": "referrals",
        "section": "referrals",
        "nav": "referrals",
        "route": "/referral",
        "asset": "referral/index.html",
        "title": "Referral - ForkMesh",
        "description": "Share your ForkMesh referral link, follow the clicks and signups it earns, and climb the public community leaderboard.",
    },
    # The hackathon participant HQ. Composed like every other dashboard page
    # but on a top-level URL, because every public hackathon page links
    # straight to it and a participant arriving from an invite must not be
    # bounced through /dashboard first.
    "challenges": {
        "view": "challenges",
        "section": "challenges",
        "nav": "challenges",
        "route": "/dashboard/challenges",
        "asset": "dashboard/challenges/index.html",
        "title": "Challenges - ForkMesh",
        "description": "Challenge other ForkMesh users to see who ships the most code in a set number of days. Start the default week in one click or set your own rules.",
    },
    "bounties": {
        "view": "bounties",
        "section": "bounties",
        "nav": "bounties",
        "route": "/dashboard/bounties",
        "asset": "dashboard/bounties/index.html",
        "title": "Bounties - ForkMesh",
        "description": "Propose organization work, name the SOL you would want for it, and follow every open and completed bounty bid.",
    },
    "links": {
        "view": "links",
        "section": "links",
        "nav": "links",
        "route": "/dashboard/links",
        "asset": "dashboard/links/index.html",
        "title": "Link rewards - ForkMesh",
        "description": "Share a public link you made about ForkMesh, see its explainable reach estimate, and follow the small SOL appreciation it can earn.",
    },
    "hackathon": {
        "view": "hackathon",
        "section": "hackathon",
        "nav": "hackathon",
        "route": "/hackathon/me",
        "asset": "hackathon/me/index.html",
        "title": "Your hackathon HQ - ForkMesh",
        "description": "Your ForkMesh Hackathon standing: points, rank, missions, squad and the share kit that brings the next builder in.",
    },
    # Paid microjobs keep a top-level public URL for the same reason /referral
    # does: the marketing chrome links straight to it, and a signed-out visitor
    # must be able to read the job list and the amounts before signing up.
    "microjobs": {
        "view": "microjobs",
        "section": "microjobs",
        "nav": "microjobs",
        "route": "/microjobs",
        "asset": "microjobs/index.html",
        "title": "Microjobs - ForkMesh",
        "description": "Submit useful issues and pull requests through the normal repository workflow; owners may attach and directly pay small SOL reward estimates.",
    },
    # ForkMesh Ads: the first-party, human-reviewed text ad network. Owners
    # write and submit ads here, and platform reviewers read the queue on the
    # same page. The `web` slot (partials/main.html) is skipped on this page.
    "ads": {
        "view": "ads",
        "section": "ads",
        "nav": "ads",
        "route": "/dashboard/ads",
        "asset": "dashboard/ads/index.html",
        "title": "Ads - ForkMesh",
        "description": "Write a short text ad for ForkMesh, send it to a human reviewer, and follow where it runs. Contextual only: no tracking, no images, no scripts.",
    },
    # Agent runners and the web sign-in of their provider CLIs
    # (runner_logins.py). The sidebar link is admin-only; the registrant of a
    # personal runner may still open the page directly.
    "agents": {
        "view": "agents",
        "section": "agents",
        "nav": "agents",
        "route": "/dashboard/agents",
        "asset": "dashboard/agents/index.html",
        "title": "Agents - ForkMesh",
        "description": "Every agent session your account can see: cloud runs in all your repositories, organization agent runs, and agents your desktops report.",
    },
    "runners": {
        "view": "runners",
        "section": "runners",
        "nav": "runners",
        "route": "/dashboard/runners",
        "asset": "dashboard/runners/index.html",
        "title": "Runners - ForkMesh",
        "description": "Agent runners and the sign-in of their Claude Code and Codex CLIs.",
    },
    "docs": {
        "view": "docs",
        "section": "docs",
        "nav": "docs",
        "route": "/dashboard/docs",
        "asset": "dashboard/docs/index.html",
        "title": "Developer docs - ForkMesh",
        "description": "Every Markdown file in the docs folder of forkmesh/forkmesh as one page, with the last update of each file.",
    },
    "apps": {
        "view": "apps",
        "section": "apps",
        "nav": "apps",
        "route": "/dashboard/apps",
        "asset": "dashboard/apps/index.html",
        "title": "Apps - ForkMesh",
        "description": "Apps installed for you and your organizations, the App Directory, and install approvals.",
    },
    "apps-build": {
        "view": "apps-build",
        "section": "apps-build",
        "nav": "apps",
        "route": "/dashboard/apps/build",
        "asset": "dashboard/apps/build/index.html",
        "title": "Build an app - ForkMesh",
        "description": "Register a ForkMesh app: OAuth credentials, scopes, signed events, slash commands and interactive messages.",
    },
    "settings": {
        "view": "settings",
        "section": "profile",
        "nav": None,
        "route": "/dashboard/settings",
        "asset": "dashboard/settings/index.html",
        "title": "Settings - ForkMesh",
        "description": "Manage your ForkMesh account: public profile, appearance, notifications, payout, and linked nodes.",
    },
    "profile": {
        "view": "profile-overview",
        "section": "profile-overview",
        "nav": "profile",
        "route": "/dashboard/profile",
        "asset": "dashboard/profile/index.html",
        "title": "Profile - ForkMesh",
        "description": "Your public ForkMesh profile overview and contribution activity.",
    },
    "profile-repositories": {
        "view": "profile-repositories",
        "section": "profile-repositories",
        "nav": "profile",
        "route": "/dashboard/profile/repositories",
        "asset": "dashboard/profile/repositories/index.html",
        "title": "Your repositories - ForkMesh",
        "description": "Repositories on your public ForkMesh profile.",
    },
    "repo": {
        "view": "repo",
        "section": "explore",
        "nav": "repos",
        "route": None,
        "asset": "dashboard/repo.html",
        "title": "Repository - ForkMesh",
        "description": "Inspect a repository mirrored by ForkMesh desktop nodes: files, issues, pulls, and mirrors.",
    },
}

PACKAGED_PAGE_COPIES = {
    page_id: {
        "view": page_id,
        "section": page_id,
        "nav": page_id,
        "route": "/dashboard/" + page_id,
        "asset": "dashboard/%s/index.html" % page_id,
        "title": title + " - ForkMesh",
        "description": description,
    }
    for page_id, title, description in (
        ("tasks", "Tasks", "The organization-private ForkMesh task catalog: status, routing, and assignment for every task the desktop app and the World office board share."),
        ("calendar", "Calendar", "Private personal and organization calendars with invitations, agenda views, time zones, and reminders."),
        ("notes", "Notes", "Collaborative Markdown notes with sharing, publishing, snapshots, and repository conversation links."),
    )
}


def partial_path(name):
    """public/-relative path to a partial by its include name."""
    return "dashboard/partials/%s.html" % name


def view_path(name):
    """public/-relative path to a per-page view partial."""
    return "dashboard/partials/views/%s.html" % name


def included_partials(shell):
    """Ordered, de-duplicated list of partial names the shell pulls in."""
    seen = []
    for name in INCLUDE_RE.findall(shell):
        if name not in seen:
            seen.append(name)
    return seen


def assemble_shell(shell, partials):
    """Substitute each ``<!--#include-->`` with ``partials[name]``.

    ``partials`` maps a partial name to its raw HTML. A missing name raises
    ``KeyError`` rather than silently leaving the placeholder in the page, so a
    composition bug surfaces loudly instead of shipping a broken shell.
    """
    return INCLUDE_RE.sub(lambda m: partials[m.group(1)], shell)


def mark_active_nav(html, page_id, page=None):
    """Bake the active sidebar link for ``page_id`` into the composed page.

    Zero JS and zero flash: the right link carries ``aria-current="page"`` and
    active text color at parse time. Pages without a sidebar item (settings)
    are returned unchanged.
    """
    nav = (page or PAGES[page_id]).get("nav")
    if not nav:
        return html
    tag_re = re.compile(r'<a data-nav="%s"[^>]*>' % re.escape(nav))

    def _activate(match):
        tag = match.group(0)
        tag = tag.replace("data-nav-link", 'data-nav-link aria-current="page"', 1)
        return tag.replace("text-muted-foreground", "text-foreground", 1)

    return tag_re.sub(_activate, html)


def content_version(text):
    """Short, deterministic content fingerprint for a client bundle.

    Only the bytes matter (not the deploy time), so an unchanged bundle keeps
    the same ``?v=`` and warm caches survive redeploys; any change rotates it.
    """
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]


# Single source of truth for cache-busting: first-party client assets that
# receive a content-hash ``?v=`` query in staged deploy output. Adding an asset
# is one edit here (plus its placeholder HTML/loader reference); every worker derives
# the version from the exact file bytes it copied into ``dist``.
#
# Checked-in documents contain only the shared ``${version}`` placeholder, so
# changing one bundle no longer rewrites pages with generated hash conflicts.
CACHE_BUSTED_ASSETS = (
    "api-client.js",
    # Dashboard.
    "dashboard.js",
    "dashboard-chat.js",
    "dashboard-plugin-host.js",
    # Shared site chrome, on nearly every marketing/auth page.
    "site-header.js",
    "site-footer.js",
    "genie-spider.js",
    "home-header-auth.js",
    "home-nav.css",
    "home-nav.js",
    "home-theme.js",
    "homepage-downloads.js",
    "static-page.js",
    "blog-social.js",
    # Rendered documentation library pages (app/tools/docs_site.py).
    "docs-page.js",
    "docs-page.css",
    # Per-page site bundles.
    "auth-mesh.js",
    "login.js",
    "signup.js",
    "forgot-password.js",
    "reset-password.js",
    "verify-pending.js",
    "security-report.js",
    "mirror-payouts.js",
    "start.js",
    "leaderboards.js",
    "ledger.js",
    "hackathon.js",
    "membership-prompt.js",
    "oauth-authorize.js",
    "chat-apps.js",
    # Standalone chat shell styling.
    "chat.css",
    # Away + custom status helper (chat page and dashboard).
    "user-status.js",
    "dashboard-composer.js",
    "dashboard-composer.css",
    # Per-page dashboard chunks (dashboard_bundle.LAZY_CHUNKS).
    "dashboard-page-agents.js",
    "dashboard-page-apps.js",
    "dashboard-page-bounties.js",
    "dashboard-page-calendar.js",
    "dashboard-page-challenges.js",
    "dashboard-page-workflows.js",
    "dashboard-page-docs.js",
    "dashboard-page-hackathon.js",
    "dashboard-page-links.js",
    "dashboard-page-notes.js",
    "dashboard-page-referrals.js",
    "dashboard-page-tasks.js",
)

# Pages whose module is a per-page chunk of dashboard.js
# (dashboard_bundle.LAZY_CHUNKS): the page preloads it with the HTML, so it
# is ready when dashboard.js calls the page's init.
CHUNK_PAGES = {
    "agents": "agents",
    "apps": "apps",
    "apps-build": "apps",
    "bounties": "bounties",
    "calendar": "calendar",
    "challenges": "challenges",
    "docs": "docs",
    "hackathon": "hackathon",
    "links": "links",
    "notes": "notes",
    "referrals": "referrals",
    "tasks": "tasks",
    "workflows": "workflows",
}


def asset_versions(read, composed=None, names=CACHE_BUSTED_ASSETS):
    """Content-hash ``?v=`` map for the requested cache-busted assets.

    ``read(public_relative_path) -> str`` loads authored bundles off disk;
    ``composed`` optionally supplies the text of bundles the caller built in
    memory (e.g. ``dashboard.js``) so they are hashed without a disk round-trip.
    Keyed by served filename. Shared by the build tool and the frontend tests so
    the ``?v=`` stamped into the built documents is reproduced identically.
    """
    composed = composed or {}
    return {
        name: content_version(composed[name] if name in composed else read(name))
        for name in names
    }


def staged_asset_versions(root):
    """Hash cache-busted assets that are present in one staged site tree."""
    present = tuple(
        name for name in CACHE_BUSTED_ASSETS if (root / name).is_file()
    )
    return asset_versions(
        lambda name: (root / name).read_text(encoding="utf-8"),
        names=present,
    )


def stamp_asset_versions(html, versions):
    """Version first-party script and stylesheet references in staged HTML.

    ``versions`` maps a bundle filename (``dashboard.js``) to a per-build
    content hash. Only listed assets are touched; any existing ``?v=`` query is
    replaced, and a version is added where none was present. Third-party scripts
    (tailwind, lucide) and unlisted assets pass through unchanged.

    This is the deploy-time cache-buster: because the query string tracks the
    bundle's content, every changed deploy yields a fresh URL, so no edge or
    browser cache can serve the previous build's JS against the new HTML.
    """
    def _stamp(match):
        base, name = match.group(1), match.group(2)
        version = versions.get(name)
        if not version:
            return match.group(0)
        return '%s?v=%s"' % (base, version)

    return ASSET_REFERENCE_RE.sub(_stamp, html)


def stamp_js_asset_versions(source, versions):
    """Rewrite quoted first-party JS URLs assigned by a runtime loader.

    This is intentionally separate from :func:`stamp_asset_versions`: the
    latter handles authored HTML, while this function handles assignments such
    as ``script.src = `/dashboard-chat.js?v=${version}``` in the World bundle.
    Template-based loaders retain that readable URL and receive their adjacent
    ``version`` value from the same content-hash map as static HTML references.
    """
    def _stamp_template(match):
        version = versions.get(match.group(3))
        if not version:
            return match.group(0)
        return '%s"%s"%s' % (match.group(1), version, match.group(2))

    def _stamp(match):
        base, name = match.group(1), match.group(2)
        version = versions.get(name)
        if not version:
            return match.group(0)
        return '%s?v=%s"' % (base, version)

    source = ASSET_TEMPLATE_ASSIGNMENT_RE.sub(_stamp_template, source)
    return ASSET_ASSIGNMENT_RE.sub(_stamp, source)


def composed_page_assets():
    """The ``public/``-relative documents :data:`PAGES` composes."""
    return {meta["asset"] for meta in PAGES.values()}


def theme_declarations(theme, mode):
    lines = [
        "--dashboard-%s-rgb: %s;" % (name, values[mode])
        for name, values in theme["colors"].items()
    ]
    if theme.get("density"):
        lines += [
            "--dashboard-%s: %s;" % (name, value)
            for name, value in theme["density"].items()
        ]
    return "\n        ".join(lines)


def apply_theme(shell, read):
    theme = json.loads(read(THEME_PATH))
    return THEME_TOKEN_RE.sub(
        lambda m: theme_declarations(theme, m.group(1)), shell)


def compose_page_from_reader(
        read, page_id, *, page=None, view=None, plugin_script=""):
    """Assemble one page document using ``read(public_relative_path) -> str``.

    Shared by the build tool and the test suite (``read`` loads files off
    disk). Reads the shell, substitutes the page tokens, pulls in each shared
    partial, injects the page's single view, and marks the active sidebar link.
    A bad page id or token raises ``KeyError`` loudly.
    """
    meta = page or PAGES[page_id]
    tokens = {
        "id": page_id,
        "title": meta["title"],
        "description": meta["description"],
        "section": meta["section"],
        "chatscript": (
            '<script src="/dashboard-chat.js?v=${version}" defer></script>'
            if page_id == "chat" else ""
        ),
        "pluginscript": plugin_script,
        "geniescript": (
            "" if page_id == "chat"
            else '    <script src="/genie-spider.js?v=${version}" defer></script>'
        ),
        # The repository page starts its first reads from the top of <head>
        # (partials/repo-early-reads.html, docs/PERF_REPO_PAGE.md).
        "earlyreads": (
            read(partial_path("repo-early-reads")) if page_id == "repo"
            else ('\n    <link rel="preload" as="script" '
                  'href="/dashboard-page-%s.js?v=${version}" />'
                  % CHUNK_PAGES[page_id]) if page_id in CHUNK_PAGES
            else ""
        ),
        "extras": "".join(
            read(partial_path(name)) for name in meta.get("extras", ())),
    }
    shell = apply_theme(
        PAGE_TOKEN_RE.sub(lambda m: tokens[m.group(1)], read("dashboard/shell.html")),
        read)
    partials = {name: read(partial_path(name)) for name in included_partials(shell)}
    view_source = view if view is not None else read(view_path(meta["view"]))
    partials["main"] = VIEW_INCLUDE_RE.sub(lambda m: view_source, partials["main"])
    return mark_active_nav(assemble_shell(shell, partials), page_id, meta)

#!/usr/bin/env python3
"""Render the ``docs`` Markdown library into the hosted ``/docs/`` tree.

Every ``*.md`` under ``docs`` becomes ``/docs/<path>/`` with a shared shell
(site header, search, area sidebar, on-page outline, previous/next, related
documents and backlinks). Sibling JSON schemas are copied verbatim so the
``schema.json`` links keep working, and the hand-written ``/docs/`` landing
page receives a generated library section listing every document.

This runs at deploy staging time (``build_site_assets.py www``); the checked-in
``www/public/docs`` keeps only its hand-authored pages plus placeholders.
"""

from __future__ import annotations

import datetime
import html
import json
import math
import posixpath
import re
from dataclasses import dataclass, field
from pathlib import Path

import docs_art
import docs_history
import docs_markdown


REPOSITORY = Path(__file__).resolve().parents[2]
WWW = REPOSITORY / "www"
DOCS_ROOT = REPOSITORY / "docs"
PUBLIC_DOCS = WWW / "public" / "docs"
DOCS_URL_PREFIX = "/docs/"
LIBRARY_URL = "/docs/library/"
HISTORY_URL = "/docs/history/"
DOCUMENTS_URL = "/docs/documents/"
# Generated pages own their slug: a Markdown file called history.md renders at
# /docs/history/reference/ rather than overwriting the history page.
GENERATED_SLUGS = frozenset({"history", "documents"})
HISTORY_LIMIT = 150
RECENT_WINDOW_DAYS = 30
SOURCE_TREE_URL = "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/"
COMPARE_URL = "https://app.forkmesh.com/forkmesh/forkmesh/compare/"
HISTORY_SOURCE = "app/tools/docs_history.py"
DOCUMENTS_SOURCE = "app/tools/docs_site.py"
LIBRARY_PLACEHOLDER = "<!-- forkmesh-docs-library -->"
LIBRARY_NAV_PLACEHOLDER = "<!-- forkmesh-docs-library-nav -->"
LIBRARY_TOC_PLACEHOLDER = "<!-- forkmesh-docs-library-toc -->"
DESCRIPTION_LIMIT = 180
READER_INDEX_URL = "/docs/reader-index.json"
MAP_VISIBLE = 6
KEYWORDS_PER_PAGE = 12
WORD_RE = re.compile(r"[a-z][a-z0-9]+")
STOPWORDS = frozenset(
    """
    about above after again against all also and any are because been before being below
    between both but can cannot could did does doing down during each few for from further
    had has have having her here hers him his how into its itself just more most must
    not now off once only other our ours out over own same she should some such than that
    the their theirs them then there these they this those through too under until upon
    use used uses using very was way were what when where which while who whom why will
    with within without would you your yours one two three first second new set get gets
    see page pages forkmesh docs doc file files line lines run runs see need needs make
    makes like still even every many much may might shows show keep keeps without per
    """.split()
)


@dataclass(frozen=True)
class Area:
    key: str
    label: str
    blurb: str


AREAS: tuple[Area, ...] = (
    Area("pillars", "Core documentation", "Architecture, encryption, mirroring, self-hosting, censorship resilience and the threat model."),
    Area("product", "Product pages", "Hand-written overviews of the desktop app, protocol and hosted service."),
    Area("guides", "Guides", "Install, use and run ForkMesh: repositories, sync, chat, calls, identities and imports."),
    Area("protocol", "Protocol & design", "The normative protocol suite, its conformance vectors and design decisions."),
    Area("security", "Security", "Trust boundaries, keys, private repositories, chat encryption and reviews."),
    Area("operations", "Operations", "The served stack, fleet tooling, releases, recovery, gateways and headless nodes."),
    Area("mobile", "Phone apps", "Using ForkMesh on iPhone, Apple Watch and Android."),
    Area("project", "Project", "Governance, funding and the definition of done."),
    Area("desktop", "Desktop internals", "How the Qt desktop app and its coding agents are built."),
    Area("app", "App & website internals", "The Python app, its CPython host, the website and the Go daemon."),
    Area("native", "Phone app internals", "How the iOS and Android apps are built and kept alike."),
    Area("marketplace", "Marketplace internals", "Packages, plugins and the Market."),
    Area("world", "World & Office internals", "The 3D World and the 2D Office."),
    Area("tools", "Tools & engineering", "Repository tools, conformance suites and engineering rules."),
    Area("schemas", "Schemas & data", "Machine-readable configuration and protocol schemas served verbatim."),
)
AREA_BY_KEY = {area.key: area for area in AREAS}
# Folders that hold internal working notes; they stay in the repository but are
# not published on the site or in the app bundles.
UNPUBLISHED_DIRECTORIES = frozenset({"finance", "marketing", "community"})
# Areas written for people building ForkMesh; the phone bundle leaves them out.
DEVELOPER_AREAS = frozenset({"desktop", "app", "native", "marketplace", "world", "tools"})
DIRECTORY_AREAS = {
    "": "guides",
    "protocol": "protocol",
    "design": "protocol",
    "conformance": "protocol",
    "security": "security",
    "operations": "operations",
    "disaster-recovery": "operations",
    "release": "operations",
    "mobile": "mobile",
    "governance": "project",
    "funding": "project",
    "desktop": "desktop",
    "agents": "desktop",
    "app": "app",
    "host": "app",
    "server": "app",
    "www": "app",
    "native": "native",
    "marketplace": "marketplace",
    "plugins": "marketplace",
    "world": "world",
    "office": "world",
    "tools": "tools",
    "engineering": "tools",
}
# The core documentation, in reading order.
PILLARS = (
    "architecture.md",
    "encryption.md",
    "mirroring.md",
    "self-hosting.md",
    "censorship-resilience.md",
    "protocol/threat-model.md",
)
PILLAR_LABELS: dict[str, tuple[str, str]] = {
    "architecture.md": ("Architecture", "network"),
    "encryption.md": ("Encryption", "lock"),
    "mirroring.md": ("Mirroring", "mirror"),
    "self-hosting.md": ("Self-hosting", "server"),
    "censorship-resilience.md": ("Censorship resilience", "shield"),
    "protocol/threat-model.md": ("Threat model", "radar"),
}
AREA_STYLE: dict[str, tuple[str, str]] = {
    "pillars": ("layers", "shield"),
    "product": ("cube", "logo"),
    "guides": ("compass", "compass"),
    "protocol": ("doc", "doc"),
    "security": ("shield-check", "key"),
    "operations": ("gauge", "server"),
    "mobile": ("phone", "phone"),
    "project": ("flag", "flag"),
    "desktop": ("monitor", "monitor"),
    "app": ("browser", "browser"),
    "native": ("phone", "phone"),
    "marketplace": ("package", "package"),
    "world": ("globe", "globe"),
    "tools": ("sliders", "sliders"),
    "schemas": ("braces", "braces"),
}
DOCS_TABS: tuple[tuple[str, str], ...] = (
    ("/docs/", "Overview"),
    ("/docs/#pillars", "Pillars"),
    ("/docs/#quickstart", "Get started"),
    ("/docs/#repository", "Concepts"),
    ("/docs/#api-websockets", "APIs"),
    ("/docs/protocol/", "Protocol"),
    (LIBRARY_URL, "Library"),
    (HISTORY_URL, "Updates"),
    ("/blog", "Blog"),
)
PLATFORMS: tuple[tuple[str, str], ...] = (
    ("/docs/", "Desktop"),
    ("/docs/browser-extension/", "Extension"),
    ("/docs/mobile/", "Mobile"),
)
SIDEBAR_PLACEHOLDER = "<!-- forkmesh-docs-sidebar -->"
# Curated order for the top-level guides; everything else sorts by title.
GUIDE_ORDER = (
    "README.md",
    "getting-started.md",
    "product-promise.md",
    "category-comparisons.md",
    "terminology.md",
    "business-model.md",
    "incentive-independence.md",
    "git-compatibility.md",
    "qt-client.md",
    "capability-matrix.md",
    "install-support-matrix.md",
    "portable-identities.md",
    "social-signals.md",
    "hosted-repositories.md",
    "repository-imports.md",
    "admin-dashboard.md",
    "cloudflare-workers-ai-coding-agent.md",
)
# Hand-authored pages that already live under www/public/docs. They are listed
# in the library so the sidebar is a complete map of /docs/, and a Markdown
# file whose slug collides with one of them renders under ``<slug>/reference/``.
PRODUCT_PAGES: tuple[tuple[str, str, str], ...] = (
    ("/docs/", "Desktop overview", "Install, quickstart, the ForkMesh mental model, APIs and self-hosting."),
    ("/docs/onboarding/", "Team onboarding", "Bring a team onto ForkMesh."),
    ("/docs/product-promise/", "Product promise proof", "See the promise work: collaborate, mirror, take the source offline, clone again."),
    ("/docs/business-model/", "Free protocol / paid services", "What is always free and which hosted services are paid."),
    ("/docs/contributions/", "Contribution counting", "How ForkMesh counts commits, issues, pull requests, reviews and repositories."),
    ("/docs/protocol/", "Protocol spec", "Signed collaboration objects, routing and the wire format."),
    ("/docs/git-compatibility/", "Git compatibility", "Generated stock-Git support matrix."),
    ("/docs/cloudflare-worker/", "App & relay", "The app on the core nodes: rooms, the repository catalog and room encryption."),
    ("/docs/qt-client/", "Desktop client", "The Qt desktop application."),
)


@dataclass
class Entry:
    url: str
    title: str
    description: str
    area: str
    kind: str = "doc"  # doc | product | asset
    source: str = ""  # repository-relative path for docs and assets
    html: str = ""
    headings: list[docs_markdown.Heading] = field(default_factory=list)
    plain_text: str = ""
    outgoing: set[str] = field(default_factory=set)
    backlinks: list["Entry"] = field(default_factory=list)
    # Repository path whose Git history dates this page. Markdown documents and
    # schemas use their source; hand-written pages use their checked-in HTML.
    history_path: str = ""
    updated: int = 0
    added: int = 0
    added_known: bool = True
    revisions: int = 0

    @property
    def words(self) -> int:
        return len(self.plain_text.split())


@dataclass
class Catalog:
    entries: list[Entry]
    history: docs_history.History = docs_history.EMPTY

    @property
    def documents(self) -> list[Entry]:
        return [entry for entry in self.entries if entry.kind == "doc"]

    @property
    def assets(self) -> list[Entry]:
        return [entry for entry in self.entries if entry.kind == "asset"]

    def by_url(self) -> dict[str, Entry]:
        return {entry.url: entry for entry in self.entries}

    def grouped(self) -> list[tuple[Area, list[Entry]]]:
        groups: list[tuple[Area, list[Entry]]] = []
        for area in AREAS:
            members = [entry for entry in self.entries if entry.area == area.key]
            if members:
                groups.append((area, members))
        return groups


# ---------------------------------------------------------------- catalog

def reserved_slugs() -> set[str]:
    return {path.name for path in PUBLIC_DOCS.iterdir() if path.is_dir()} | set(GENERATED_SLUGS)


def document_url(relative: str, reserved: set[str] | None = None) -> str:
    """Hosted URL for a ``docs``-relative Markdown path."""
    reserved = reserved_slugs() if reserved is None else reserved
    if relative == "README.md":
        return LIBRARY_URL
    parts = relative.split("/")
    if parts[-1] == "README.md":
        parts = parts[:-1]
    else:
        parts[-1] = parts[-1][: -len(".md")]
    if len(parts) == 1 and parts[0] in reserved:
        parts.append("reference")
    return DOCS_URL_PREFIX + "/".join(parts) + "/"


def asset_url(relative: str) -> str:
    return DOCS_URL_PREFIX + relative


def area_for(relative: str) -> str:
    if relative in PILLARS:
        return "pillars"
    directory = relative.split("/")[0] if "/" in relative else ""
    return DIRECTORY_AREAS.get(directory, "guides")


def is_published(relative: str) -> bool:
    return relative.split("/")[0] not in UNPUBLISHED_DIRECTORIES


def _guide_rank(relative: str) -> tuple[int, str]:
    try:
        return (GUIDE_ORDER.index(relative), "")
    except ValueError:
        return (len(GUIDE_ORDER), relative)


def _truncate(text: str, limit: int = DESCRIPTION_LIMIT) -> str:
    text = " ".join(text.split()).rstrip(":")
    if len(text) <= limit:
        return text
    cut = text[: limit - 1].rsplit(" ", 1)[0]
    return cut.rstrip(",;:") + "…"


def _asset_description(path: Path) -> str:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "Machine-readable data file."
    if isinstance(payload, dict):
        for key in ("description", "title", "$comment"):
            value = payload.get(key)
            if isinstance(value, str) and value.strip():
                return _truncate(value)
    return "Machine-readable data file."


ACRONYMS = {"ssh": "SSH", "https": "HTTPS", "api": "API", "apns": "APNs"}


def _asset_title(relative: str) -> str:
    name = relative.rsplit("/", 1)[-1]
    if name.endswith(".schema.json"):
        words = name[: -len(".schema.json")].split("-")
        words = [ACRONYMS.get(word, word) for word in words]
        if words[0] not in ACRONYMS.values():
            words[0] = words[0].capitalize()
        return " ".join(words) + " schema"
    return name


def product_history_path(url: str) -> str:
    """Checked-in HTML behind a hand-written ``/docs/`` page."""
    return "www/public/docs/" + url[len(DOCS_URL_PREFIX):] + "index.html"


def load_catalog(docs_root: Path = DOCS_ROOT) -> Catalog:
    reserved = reserved_slugs()
    entries: list[Entry] = [
        Entry(
            url=url,
            title=title,
            description=description,
            area="product",
            kind="product",
            history_path=product_history_path(url),
        )
        for url, title, description in PRODUCT_PAGES
    ]
    sources = sorted(
        path for path in docs_root.rglob("*.md")
        if path.is_file() and is_published(path.relative_to(docs_root).as_posix())
    )
    documents: dict[str, Entry] = {}
    for path in sources:
        relative = path.relative_to(docs_root).as_posix()
        documents[relative] = Entry(
            url=document_url(relative, reserved),
            title="",
            description="",
            area=area_for(relative),
            source=f"docs/{relative}",
            history_path=f"docs/{relative}",
        )
    assets: dict[str, Entry] = {}
    for path in sorted(docs_root.rglob("*.json")):
        if not path.is_file() or not is_published(path.relative_to(docs_root).as_posix()):
            continue
        relative = path.relative_to(docs_root).as_posix()
        assets[relative] = Entry(
            url=asset_url(relative),
            title=_asset_title(relative),
            description=_asset_description(path),
            area="schemas",
            kind="asset",
            source=f"docs/{relative}",
            history_path=f"docs/{relative}",
        )

    for relative, entry in documents.items():
        rewriter = _link_rewriter(relative, documents, assets, entry)
        rendered = docs_markdown.render(
            (docs_root / relative).read_text(encoding="utf-8"),
            link_rewriter=rewriter,
            rich=True,
        )
        entry.title = rendered.title or relative.rsplit("/", 1)[-1][: -len(".md")]
        entry.description = _truncate(rendered.description)
        entry.html = rendered.html
        entry.headings = rendered.headings
        entry.plain_text = rendered.plain_text

    def sort_key(item: tuple[str, Entry]):
        relative, entry = item
        order = [area.key for area in AREAS].index(entry.area)
        if entry.area == "pillars":
            return (order, PILLARS.index(relative), "", entry.title.lower())
        if entry.area == "guides":
            return (order, *_guide_rank(relative), entry.title.lower())
        return (order, len(GUIDE_ORDER), "", entry.title.lower())

    ordered_documents = [entry for _, entry in sorted(documents.items(), key=sort_key)]
    entries.extend(ordered_documents)
    entries.extend(entry for _, entry in sorted(assets.items(), key=lambda item: item[1].title.lower()))
    catalog = Catalog(entries)
    stamp_history(catalog)
    by_url = catalog.by_url()
    for entry in ordered_documents:
        for url in sorted(entry.outgoing):
            target = by_url.get(url)
            if target is not None and target is not entry and entry not in target.backlinks:
                target.backlinks.append(entry)
    return catalog


def stamp_history(catalog: Catalog) -> Catalog:
    """Date every entry from the repository history, or from its file mtime."""
    history = docs_history.load_history()
    catalog.history = history
    for entry in catalog.entries:
        if not entry.history_path:
            continue
        record = history.file(entry.history_path)
        if record is None and history.available:
            record = docs_history.file_history(entry.history_path)
        if record is not None:
            entry.updated = record.updated
            entry.added = record.added
            entry.added_known = record.added_known
            entry.revisions = record.revisions
            continue
        # No history for this path: an untracked working copy, a shallow clone,
        # or a build without git. The file's own timestamp still orders it.
        try:
            entry.updated = int((REPOSITORY / entry.history_path).stat().st_mtime)
        except OSError:
            entry.updated = 0
        entry.added = entry.updated
        entry.added_known = False
        entry.revisions = 0
    return catalog


# ---------------------------------------------------------------- dates

def _utc(timestamp: int) -> datetime.datetime:
    return datetime.datetime.fromtimestamp(timestamp, tz=datetime.timezone.utc)


def iso_date(timestamp: int) -> str:
    return _utc(timestamp).strftime("%Y-%m-%d") if timestamp else ""


def short_date(timestamp: int) -> str:
    """``17 Sep 2026`` — stable width, unambiguous between locales."""
    if not timestamp:
        return "—"
    moment = _utc(timestamp)
    return f"{moment.day} {moment.strftime('%b %Y')}"


def long_date(timestamp: int) -> str:
    if not timestamp:
        return "Undated"
    moment = _utc(timestamp)
    return f"{moment.strftime('%A')} {moment.day} {moment.strftime('%B %Y')}"


def month_label(timestamp: int) -> str:
    return _utc(timestamp).strftime("%B %Y") if timestamp else "Undated"


def reading_minutes(words: int) -> int:
    return max(1, round(words / 220)) if words else 0


def _link_rewriter(relative: str, documents: dict[str, Entry], assets: dict[str, Entry], entry: Entry):
    directory = posixpath.dirname(relative)

    def rewrite(target: str, is_image: bool) -> str:
        if not target or target.startswith(("#", "/", "//")) or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
            return target
        path, _, fragment = target.partition("#")
        suffix = f"#{fragment}" if fragment else ""
        repo_relative = posixpath.normpath(posixpath.join("docs", directory, path))
        if repo_relative.startswith("docs/"):
            inner = repo_relative[len("docs/"):]
            if inner in documents:
                url = documents[inner].url
                entry.outgoing.add(url)
                return url + suffix
            if inner in assets:
                entry.outgoing.add(assets[inner].url)
                return assets[inner].url
        if repo_relative.startswith("www/public/"):
            return "/" + repo_relative[len("www/public/"):] + suffix
        if repo_relative.startswith("../"):
            return target
        return SOURCE_TREE_URL + repo_relative + suffix

    return rewrite


# ---------------------------------------------------------------- rendering

def _e(text: str) -> str:
    return html.escape(text, quote=True)


LIBRARY_PAGES: tuple[tuple[str, str, str], ...] = (
    ("/docs/", "Overview", "home"),
    ("/docs/#pillars", "The six pillars", "layers"),
    (LIBRARY_URL, "Library overview", "grid"),
    (HISTORY_URL, "Recent updates", "clock"),
    (DOCUMENTS_URL, "All documents", "list"),
)


def home_nav_html(current: str) -> str:
    """The library-wide entry points above the per-area sidebar."""
    parts: list[str] = []
    for url, label, glyph in LIBRARY_PAGES:
        active = " active" if url == current else ""
        aria = ' aria-current="page"' if url == current else ""
        parts.append(
            f'<a class="dp-nav-link dp-nav-home{active}" href="{_e(url)}"{aria}>'
            f"{docs_art.icon(glyph)}<span>{_e(label)}</span></a>"
        )
    return "\n".join(parts)


def platforms_html(current: str = "/docs/") -> str:
    links = "".join(
        f'<a class="{"active" if url == current else ""}" href="{_e(url)}">{_e(label)}</a>'
        for url, label in PLATFORMS
    )
    return f'<nav class="dp-platforms" aria-label="Platform">{links}</nav>'


def promo_html(uid: str = "promo") -> str:
    return (
        '<a class="dp-promo" href="/desktop">'
        f'{docs_art.art("logo", uid)}'
        "<strong>Run ForkMesh</strong>"
        "<span>Get the desktop app: host, mirror and collaborate from your own machine.</span>"
        f'<em>Download {docs_art.icon("download")}</em></a>'
    )


def tabs_html(current: str) -> str:
    if current in {HISTORY_URL, DOCUMENTS_URL}:
        active_url = HISTORY_URL
    elif current == "/docs/":
        active_url = "/docs/"
    else:
        active_url = LIBRARY_URL
    return "\n".join(
        f'<a class="{"active" if url == active_url else ""}" href="{_e(url)}">{_e(label)}</a>'
        for url, label in DOCS_TABS
    )


def meta_html(entry: Entry) -> str:
    """The dateline under a page's breadcrumbs."""
    items: list[str] = []
    if entry.updated:
        items.append(
            f'<span>Updated <time datetime="{iso_date(entry.updated)}">'
            f"{short_date(entry.updated)}</time></span>"
        )
    if entry.revisions:
        plural = "" if entry.revisions == 1 else "s"
        items.append(
            f'<a href="{HISTORY_URL}">{entry.revisions} revision{plural}</a>'
        )
    minutes = reading_minutes(entry.words)
    if minutes:
        items.append(f"<span>{minutes} min read</span>")
    return (
        '<p class="dp-meta" data-docs-meta>'
        + '<span aria-hidden="true">·</span>'.join(items)
        + "</p>"
    )


def sidebar_html(catalog: Catalog, current: str) -> str:
    parts: list[str] = []
    for area, members in catalog.grouped():
        here = any(entry.url == current for entry in members)
        expanded = area.key == "pillars" or here
        glyph = AREA_STYLE.get(area.key, ("cube", "logo"))[0]
        parts.append(
            f'<details class="dp-nav-group{" current" if here else ""}"{" open" if expanded else ""}>'
            f'<summary>{docs_art.icon(glyph)}<h2>{_e(area.label)}</h2></summary>'
        )
        for entry in members:
            active = " active" if entry.url == current else ""
            aria = ' aria-current="page"' if entry.url == current else ""
            parts.append(
                f'<a class="dp-nav-link{active}" href="{_e(entry.url)}"{aria}'
                f' data-search-title="{_e(entry.title)}" data-search-body="{_e(entry.description)}"'
                f' data-search-href="{_e(entry.url)}">{_e(entry.title)}</a>'
            )
        parts.append("</details>")
    return "\n".join(parts)


def landing_sidebar_html(catalog: Catalog) -> str:
    return home_nav_html("/docs/") + sidebar_html(catalog, "/docs/") + promo_html()


def library_html(catalog: Catalog, *, heading_level: int = 3, include_product: bool = True) -> str:
    parts: list[str] = []
    for area, members in catalog.grouped():
        if area.key == "product" and not include_product:
            continue
        tag = f"h{heading_level}"
        parts.append(
            f'<div class="library-area" id="library-{area.key}">'
            f'<{tag} class="library-title">{_e(area.label)}</{tag}>'
            f'<p class="library-blurb">{_e(area.blurb)}</p>'
            f'<div class="library-grid">'
        )
        for entry in members:
            kind = {"asset": "JSON", "product": "Page", "doc": ""}[entry.kind]
            badge = f'<span class="library-kind">{kind}</span>' if kind else ""
            parts.append(
                f'<a class="doc-card library-card" href="{_e(entry.url)}"'
                f' data-search-title="{_e(entry.title)}" data-search-body="{_e(entry.description)}"'
                f' data-search-href="{_e(entry.url)}">'
                f'<span class="library-card-title">{_e(entry.title)}{badge}</span>'
                f'<span class="library-card-body">{_e(entry.description)}</span></a>'
            )
        parts.append("</div></div>")
    return "\n".join(parts)


def library_nav_html(catalog: Catalog, *, include_product: bool = True) -> str:
    return "\n".join(
        f'<a class="nav-link" href="#library-{area.key}">{_e(area.label)}</a>'
        for area, _ in catalog.grouped()
        if include_product or area.key != "product"
    )


def library_toc_html(catalog: Catalog, *, include_product: bool = True) -> str:
    return "\n".join(
        f'<a class="dp-outline-link level-3" href="#library-{area.key}">{_e(area.label)}</a>'
        for area, _ in catalog.grouped()
        if include_product or area.key != "product"
    )


def outline_nav_html(items: list[tuple[str, str, int]]) -> str:
    """``(id, text, level)`` triples as the right-rail outline."""
    if not items:
        return ""
    parts = ['<nav class="dp-outline" aria-label="On this page"><h2>On this page</h2>']
    for identifier, text, level in items:
        parts.append(
            f'<a class="dp-outline-link level-{level}" href="#{_e(identifier)}">{_e(text)}</a>'
        )
    parts.append("</nav>")
    return "\n".join(parts)


def outline_html(entry: Entry) -> str:
    return outline_nav_html(
        [
            (heading.id, heading.text, heading.level)
            for heading in entry.headings
            if heading.level in (2, 3)
        ]
    )


def _heading(level: int, identifier: str, text: str) -> str:
    return (
        f'<h{level} id="{_e(identifier)}">{_e(text)}'
        f'<a class="heading-anchor" href="#{_e(identifier)}" aria-label="Link to this section">#</a>'
        f"</h{level}>"
    )


def _stat(value: str, label: str) -> str:
    return (
        f'<div class="dp-stat"><span class="dp-stat-value">{_e(value)}</span>'
        f'<span class="dp-stat-label">{_e(label)}</span></div>'
    )


def _card_list(title: str, entries: list[Entry], current: Entry) -> str:
    entries = [entry for entry in entries if entry is not current]
    if not entries:
        return ""
    parts = [f'<section class="dp-related"><h2>{_e(title)}</h2><ul>']
    for entry in entries:
        parts.append(
            f'<li><a href="{_e(entry.url)}">{_e(entry.title)}</a>'
            f'<span>{_e(entry.description)}</span></li>'
        )
    parts.append("</ul></section>")
    return "\n".join(parts)


def _map_column(kind: str, title: str, entries: list[Entry]) -> str:
    if not entries:
        return (
            f'<div class="dp-map-col dp-map-{kind} dp-map-empty"><h3>{_e(title)}</h3>'
            '<p>No pages yet.</p></div>'
        )
    nodes = [
        f'<a class="dp-map-node" href="{_e(entry.url)}" data-map-node>'
        f'<strong>{_e(entry.title)}</strong><span>{_e(entry.description)}</span></a>'
        for entry in entries
    ]
    visible, hidden = nodes[:MAP_VISIBLE], nodes[MAP_VISIBLE:]
    more = (
        f'<details class="dp-map-more"><summary>Show {len(hidden)} more</summary>{"".join(hidden)}</details>'
        if hidden
        else ""
    )
    return (
        f'<div class="dp-map-col dp-map-{kind}"><h3>{_e(title)}'
        f' <span class="dp-map-count">{len(entries)}</span></h3>{"".join(visible)}{more}</div>'
    )


def page_map_html(entry: Entry, references: list[Entry]) -> str:
    incoming = [other for other in entry.backlinks if other is not entry]
    outgoing = [other for other in references if other is not entry]
    if not incoming and not outgoing:
        return ""
    area = AREA_BY_KEY[entry.area]
    return (
        '<section class="dp-map" aria-labelledby="dp-page-map">'
        '<h2 id="dp-page-map">Page map</h2>'
        '<p class="dp-map-lead">How this page connects to the rest of the library.</p>'
        '<div class="dp-map-grid" data-map>'
        + _map_column("in", "Pages that link here", incoming)
        + '<div class="dp-map-hub" data-map-hub><span>You are here</span>'
        f"<strong>{_e(entry.title)}</strong><em>{_e(area.label)}</em></div>"
        + _map_column("out", "Pages this one links to", outgoing)
        + "</div></section>"
    )


def _words(text: str) -> list[str]:
    return [word for word in WORD_RE.findall(text.lower()) if word not in STOPWORDS and len(word) > 2]


def reader_index(catalog: Catalog) -> dict:
    pages = [entry for entry in catalog.entries if entry.kind in ("doc", "product")]
    position = {entry.url: index for index, entry in enumerate(pages)}
    counts: list[dict[str, float]] = []
    frequency: dict[str, int] = {}
    for entry in pages:
        weights: dict[str, float] = {}
        for word in _words(entry.title):
            weights[word] = weights.get(word, 0.0) + 4.0
        for heading in entry.headings:
            for word in _words(heading.text):
                weights[word] = weights.get(word, 0.0) + 2.0
        for word in _words(entry.description + " " + entry.plain_text):
            weights[word] = weights.get(word, 0.0) + 1.0
        counts.append(weights)
        for word in weights:
            frequency[word] = frequency.get(word, 0) + 1
    total = len(pages) or 1
    records = []
    for entry, weights in zip(pages, counts):
        scored = sorted(
            (
                count / (count + 4.0) * math.log(total / frequency[word]),
                word,
            )
            for word, count in weights.items()
            if 3 <= frequency[word] < total * 0.7
        )[::-1]
        peak = scored[0][0] if scored else 1.0
        title_words = list(dict.fromkeys(word for word in _words(entry.title) if word in frequency))[:4]
        keywords = [[word, 1.0] for word in title_words]
        keywords += [
            [word, round(score / peak, 2)] for score, word in scored if word not in title_words
        ][: KEYWORDS_PER_PAGE - len(keywords)]
        records.append(
            {
                "u": entry.url,
                "t": entry.title,
                "d": entry.description,
                "a": entry.area,
                "m": reading_minutes(entry.words),
                "o": sorted(position[url] for url in entry.outgoing if url in position),
                "k": keywords,
            }
        )
    return {
        "version": 1,
        "areas": {area.key: area.label for area in AREAS},
        "pages": records,
    }


def breadcrumbs_html(entry: Entry) -> str:
    area = AREA_BY_KEY[entry.area]
    crumbs = [
        f'<a href="/docs/" aria-label="Docs home">{docs_art.icon("home")}</a>',
        '<a href="/docs/">Docs</a>',
        f'<a href="{LIBRARY_URL}#library-{area.key}">{_e(area.label)}</a>',
    ]
    if entry.url != LIBRARY_URL:
        crumbs.append(f"<span>{_e(entry.title)}</span>")
    return '<nav class="dp-crumbs" aria-label="Breadcrumb">' + " <span aria-hidden=\"true\">/</span> ".join(crumbs) + "</nav>"


HERO_RE = re.compile(r"^\s*(<h1\b.*?</h1>)\s*(?:<p>(.*?)</p>)?", re.DOTALL)
CORE_PARAGRAPH_RE = re.compile(r"<p>Part of the core documentation:.*?</p>\s*", re.DOTALL)
AUDIENCE_RE = re.compile(r'(<h2 id="who-it-is-for">.*?</h2>)\s*(<p>.*?</p>)', re.DOTALL)


def pillar_entries(catalog: Catalog) -> list[tuple[Entry, str, str]]:
    by_source = {entry.source: entry for entry in catalog.documents}
    return [
        (by_source["docs/" + path], label, art)
        for path, (label, art) in PILLAR_LABELS.items()
        if "docs/" + path in by_source
    ]


def pillar_strip_html(catalog: Catalog, current: str) -> str:
    links = []
    for entry, label, _ in pillar_entries(catalog):
        aria = ' aria-current="page"' if entry.url == current else ""
        links.append(f'<a href="{_e(entry.url)}"{aria}>{_e(label)}{docs_art.icon("arrow")}</a>')
    return (
        '<nav class="dp-pillar-strip" aria-label="Core documentation">'
        "<span>Part of the core documentation</span>" + "".join(links) + "</nav>"
    )


def art_key(entry: Entry) -> str:
    pillar = PILLAR_LABELS.get(entry.source[len("docs/"):])
    if pillar:
        return pillar[1]
    return AREA_STYLE.get(entry.area, ("cube", "logo"))[1]


def hero_html(catalog: Catalog, entry: Entry) -> str:
    body = entry.html
    is_pillar = entry.source[len("docs/"):] in PILLAR_LABELS
    if is_pillar:
        body = CORE_PARAGRAPH_RE.sub("", body, count=1)
    body = AUDIENCE_RE.sub(
        lambda match: '<section class="dp-callout"><span class="dp-callout-icon">'
        + docs_art.icon("users")
        + "</span>"
        + match.group(1)
        + match.group(2)
        + "</section>",
        body,
        count=1,
    )
    match = HERO_RE.match(body)
    if not match:
        return meta_html(entry) + body
    area = AREA_BY_KEY[entry.area]
    glyph = AREA_STYLE.get(area.key, ("cube", "logo"))[0]
    kicker = (
        f'<a class="dp-kicker" href="{LIBRARY_URL}#library-{area.key}">'
        f"{docs_art.icon(glyph)}{_e(area.label)}</a>"
    )
    lead = f'<p class="dp-hero-lead">{match.group(2)}</p>' if match.group(2) else ""
    strip = pillar_strip_html(catalog, entry.url) if is_pillar else ""
    hero = (
        '<header class="dp-hero"><div class="dp-hero-text">'
        + kicker
        + match.group(1)
        + lead
        + meta_html(entry)
        + strip
        + '</div><div class="dp-hero-art">'
        + docs_art.art(art_key(entry), "hero-art")
        + "</div></header>"
    )
    return hero + body[match.end():]


def core_path_html(catalog: Catalog, current: str) -> str:
    items = []
    for index, (entry, label, _) in enumerate(pillar_entries(catalog), start=1):
        aria = ' aria-current="page"' if entry.url == current else ""
        items.append(
            f'<li><a href="{_e(entry.url)}"{aria}><span class="dp-step">{index}</span>{_e(label)}</a></li>'
        )
    return (
        '<section class="dp-rail-card dp-rail-path" aria-label="Core documentation">'
        f'<h2>{docs_art.icon("layers")}Core documentation</h2><ol>' + "".join(items) + "</ol></section>"
    )


def related_rail_html(entry: Entry, references: list[Entry], siblings: list[Entry]) -> str:
    pillar_urls = {item.url for item in references if item.source[len("docs/"):] in PILLAR_LABELS}
    picks = [item for item in references if item.url not in pillar_urls and item is not entry]
    if len(picks) < 5:
        index = siblings.index(entry) if entry in siblings else 0
        nearby = siblings[index + 1:] + siblings[:index]
        picks += [item for item in nearby if item not in picks and item is not entry]
    picks = picks[:5]
    if not picks:
        return ""
    items = "".join(
        f'<li><a href="{_e(item.url)}">{docs_art.icon("chevron")}{_e(item.title)}</a></li>'
        for item in picks
    )
    return (
        '<section class="dp-rail-card dp-rail-related" aria-label="Related">'
        f'<h2>{docs_art.icon("network")}Related</h2><ul>{items}</ul></section>'
    )


def page_html(catalog: Catalog, entry: Entry) -> str:
    documents = catalog.documents
    index = documents.index(entry)
    previous = documents[index - 1] if index > 0 else None
    following = documents[index + 1] if index + 1 < len(documents) else None
    by_url = catalog.by_url()
    area = AREA_BY_KEY[entry.area]
    siblings = [member for member in documents if member.area == entry.area]
    references = [by_url[url] for url in sorted(entry.outgoing) if url in by_url]
    library_section = ""
    if entry.url == LIBRARY_URL:
        library_section = (
            '<section class="dp-library" id="library">'
            + _heading(2, "every-document", "Every document")
            + f'<p class="dp-lead-links"><a href="{DOCUMENTS_URL}">Sort every document by date</a>'
            f' <span aria-hidden="true">·</span> <a href="{HISTORY_URL}">See what changed recently</a></p>'
            + library_html(catalog, heading_level=3)
            + "</section>"
        )

    pager = ['<nav class="dp-pager" aria-label="Previous and next">']
    if previous:
        pager.append(
            f'<a class="dp-pager-link prev" href="{_e(previous.url)}"><span>Previous</span>{_e(previous.title)}</a>'
        )
    else:
        pager.append("<span></span>")
    if following:
        pager.append(
            f'<a class="dp-pager-link next" href="{_e(following.url)}"><span>Next</span>{_e(following.title)}</a>'
        )
    pager.append("</nav>")

    description = entry.description or f"{entry.title} — ForkMesh documentation."
    return render_shell(
        catalog,
        title=entry.title,
        description=description,
        url=entry.url,
        source=SOURCE_TREE_URL + entry.source,
        crumbs=breadcrumbs_html(entry),
        body=hero_html(catalog, entry),
        library=library_section,
        pager="".join(pager),
        related=_card_list(f"More in {area.label}", siblings, entry),
        page_map=page_map_html(entry, references),
        outline=outline_html(entry),
        rail_extra=core_path_html(catalog, entry.url) + related_rail_html(entry, references, siblings),
    )


def render_shell(
    catalog: Catalog,
    *,
    title: str,
    description: str,
    url: str,
    source: str,
    crumbs: str,
    body: str,
    meta: str = "",
    library: str = "",
    pager: str = "",
    related: str = "",
    page_map: str = "",
    outline: str = "",
    rail_extra: str = "",
    wide: bool = False,
) -> str:
    """The shared documentation shell around already-rendered body HTML."""
    return PAGE_TEMPLATE.format(
        # A wide page (the sortable table) trades the outline rail for width.
        shell_class="dp-shell dp-shell-wide" if wide else "dp-shell",
        title=_e(title),
        description=_e(description),
        url=_e(url),
        platforms=platforms_html(),
        home=home_nav_html(url),
        tabs=tabs_html(url),
        sidebar=sidebar_html(catalog, url),
        promo=promo_html(),
        crumbs=crumbs,
        meta=meta,
        source=_e(source),
        body=body,
        library=library,
        pager=pager,
        related=related,
        page_map=page_map,
        outline=outline,
        rail_extra=rail_extra,
    )


# ------------------------------------------------- generated library pages

def _generated_crumbs(title: str) -> str:
    return (
        '<nav class="dp-crumbs" aria-label="Breadcrumb">'
        '<a href="/docs/">Docs</a> <span aria-hidden="true">/</span> '
        f'<a href="{LIBRARY_URL}">Library</a> <span aria-hidden="true">/</span> '
        f"<span>{_e(title)}</span></nav>"
    )


def _entry_link(entry: Entry) -> str:
    kind = {"asset": "JSON", "product": "Page", "doc": ""}[entry.kind]
    badge = f'<span class="library-kind">{kind}</span>' if kind else ""
    return f'<a href="{_e(entry.url)}">{_e(entry.title)}</a>{badge}'


def history_page_html(catalog: Catalog) -> str:
    """``/docs/history/`` — what changed in the documentation, newest first."""
    history = catalog.history
    by_path = {entry.history_path: entry for entry in catalog.entries if entry.history_path}
    documents = catalog.documents
    latest = max((entry.updated for entry in catalog.entries), default=0)
    cutoff = latest - RECENT_WINDOW_DAYS * 86400 if latest else 0
    recent = sum(1 for entry in catalog.entries if entry.updated >= cutoff > 0)

    body = [
        _heading(1, "recent-updates", "Recent documentation updates"),
        '<p class="dp-lead">Every commit that touched a documentation page, newest first, '
        "straight from the repository history. Each entry links the pages it changed and the "
        "commit that changed them.</p>",
        '<div class="dp-stats">',
        _stat(str(len(documents)), "Documents"),
        _stat(str(len(catalog.assets)), "Schemas"),
        _stat(str(recent), f"Changed in {RECENT_WINDOW_DAYS} days"),
        _stat(short_date(latest), "Last update"),
        "</div>",
        f'<p class="dp-lead-links"><a href="{DOCUMENTS_URL}">Browse all documents by date</a>'
        f' <span aria-hidden="true">·</span> <a href="{LIBRARY_URL}">Library overview</a></p>',
    ]

    outline: list[tuple[str, str, int]] = []
    if not history.available:
        body.append(
            '<p class="dp-empty">Documentation history is unavailable in this build: it is '
            "read from the repository at deploy time, and this site was staged without one.</p>"
        )
        return render_shell(
            catalog,
            title="Recent documentation updates",
            description="Every change to the ForkMesh documentation, newest first.",
            url=HISTORY_URL,
            source=SOURCE_TREE_URL + HISTORY_SOURCE,
            crumbs=_generated_crumbs("Recent updates"),
            body="\n".join(body),
        )

    month = ""
    open_list = False
    for commit in history.commits[:HISTORY_LIMIT]:
        label = month_label(commit.timestamp)
        if label != month:
            if open_list:
                body.append("</ol>")
            month = label
            identifier = "month-" + iso_date(commit.timestamp)[:7]
            outline.append((identifier, label, 2))
            body.append(_heading(2, identifier, label))
            body.append('<ol class="dp-history">')
            open_list = True
        changes = []
        for change in commit.changes:
            entry = by_path.get(change.path)
            name = _entry_link(entry) if entry else f"<span>{_e(change.path)}</span>"
            changes.append(
                f'<li><span class="dp-change {_e(change.status)}">{_e(change.status.capitalize())}'
                f"</span>{name}</li>"
            )
        if commit.parent:
            compare = f"{COMPARE_URL}{commit.parent}...{commit.sha}"
            sha = f'<a class="dp-history-sha" href="{_e(compare)}">{_e(commit.short_sha)}</a>'
        else:
            sha = f'<span class="dp-history-sha">{_e(commit.short_sha)}</span>'
        body.append(
            '<li class="dp-history-entry">'
            '<div class="dp-history-head">'
            f'<time datetime="{iso_date(commit.timestamp)}">{_e(long_date(commit.timestamp))}</time>'
            f"{sha}"
            "</div>"
            f'<p class="dp-history-subject">{_e(commit.subject)}</p>'
            f'<p class="dp-history-author">{_e(commit.author)}</p>'
            f'<ul class="dp-history-changes">{"".join(changes)}</ul>'
            "</li>"
        )
    if open_list:
        body.append("</ol>")
    if len(history.commits) > HISTORY_LIMIT:
        body.append(
            f'<p class="dp-note">Showing the {HISTORY_LIMIT} most recent changes of '
            f'{len(history.commits)}. <a href="{_e(SOURCE_TREE_URL + "docs")}">Read the rest '
            "in the repository.</a></p>"
        )

    return render_shell(
        catalog,
        title="Recent documentation updates",
        description="Every change to the ForkMesh documentation, newest first.",
        url=HISTORY_URL,
        source=SOURCE_TREE_URL + HISTORY_SOURCE,
        crumbs=_generated_crumbs("Recent updates"),
        body="\n".join(body),
        outline=outline_nav_html(outline),
    )


def _sortable_header(label: str, sort: str, *, state: str = "none") -> str:
    return (
        f'<th scope="col" data-sort="{_e(sort)}" aria-sort="{_e(state)}">'
        f'<button type="button">{_e(label)}<span class="dp-sort-arrow" aria-hidden="true"></span>'
        "</button></th>"
    )


def documents_page_html(catalog: Catalog) -> str:
    """``/docs/documents/`` — every page with its dates, sortable in place."""
    rows: list[str] = []
    ordered = sorted(
        catalog.entries,
        key=lambda entry: (-entry.updated, entry.title.lower()),
    )
    for entry in ordered:
        area = AREA_BY_KEY[entry.area]
        added_title = "" if entry.added_known else ' title="Earliest recorded change"'
        rows.append(
            f'<tr data-filter-text="{_e((entry.title + " " + entry.description + " " + area.label).lower())}">'
            f'<th scope="row" data-sort-value="{_e(entry.title.lower())}">'
            f"{_entry_link(entry)}"
            f'<span class="dp-row-note">{_e(entry.description)}</span></th>'
            f'<td data-sort-value="{_e(area.label)}">{_e(area.label)}</td>'
            f'<td data-sort-value="{entry.updated}">'
            f'<time datetime="{iso_date(entry.updated)}">{_e(short_date(entry.updated))}</time></td>'
            f'<td data-sort-value="{entry.added}"{added_title}>'
            f'<time datetime="{iso_date(entry.added)}">{_e(short_date(entry.added))}</time></td>'
            f'<td class="dp-numeric" data-sort-value="{entry.revisions}">'
            f'{entry.revisions or "—"}</td>'
            "</tr>"
        )

    body = [
        _heading(1, "all-documents", "All documents"),
        '<p class="dp-lead">Every page in the documentation with the date it last changed. '
        "Sort by any column, or filter by title, area or summary.</p>",
        '<div class="dp-stats">',
        _stat(str(len(catalog.documents)), "Documents"),
        _stat(str(len(catalog.assets)), "Schemas"),
        _stat(str(len([e for e in catalog.entries if e.kind == "product"])), "Hand-written pages"),
        _stat(short_date(max((entry.updated for entry in catalog.entries), default=0)), "Last update"),
        "</div>",
        f'<p class="dp-lead-links"><a href="{HISTORY_URL}">See what changed recently</a>'
        f' <span aria-hidden="true">·</span> <a href="{LIBRARY_URL}">Library overview</a></p>',
        '<div class="dp-table-tools">',
        '<label class="dp-filter"><span class="dp-visually-hidden">Filter documents</span>'
        '<input type="search" data-table-filter="documents-table" placeholder="Filter documents…" '
        'autocomplete="off" /></label>',
        f'<p class="dp-filter-count" data-table-count="documents-table" role="status">'
        f"Showing {len(catalog.entries)} of {len(catalog.entries)}</p>",
        "</div>",
        '<div class="table-wrap">',
        '<table class="dp-table" id="documents-table" data-sortable>',
        "<thead><tr>",
        _sortable_header("Document", "text"),
        _sortable_header("Area", "text"),
        _sortable_header("Updated", "number", state="descending"),
        _sortable_header("Added", "number"),
        _sortable_header("Changes", "number"),
        "</tr></thead>",
        "<tbody>",
        *rows,
        "</tbody></table></div>",
    ]
    if not catalog.history.available:
        body.append(
            '<p class="dp-note">Dates fall back to file timestamps: this site was staged '
            "without the repository history.</p>"
        )

    return render_shell(
        catalog,
        title="All documents",
        description="Every ForkMesh documentation page with the date it last changed, sortable by column.",
        url=DOCUMENTS_URL,
        source=SOURCE_TREE_URL + DOCUMENTS_SOURCE,
        crumbs=_generated_crumbs("All documents"),
        body="\n".join(body),
        wide=True,
    )


PAGE_TEMPLATE = """<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <script src="/api-client.js?v=${{version}}"></script>
    <script src="/analytics.js"></script>
    <script src="/safe-input.js" defer></script>
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>{title} · Docs · ForkMesh</title>
    <meta name="description" content="{description}" />
    <link rel="canonical" href="https://forkmesh.com{url}" />
    <meta name="application-name" content="ForkMesh" />
    <meta name="theme-color" content="#090909" />
    <meta name="color-scheme" content="light dark" />
    <link rel="icon" href="/favicon/favicon.ico" sizes="any" />
    <link rel="icon" href="/favicon/favicon.svg" type="image/svg+xml" />
    <link rel="icon" type="image/png" sizes="32x32" href="/favicon/favicon-32x32.png" />
    <link rel="apple-touch-icon" sizes="180x180" href="/favicon/apple-touch-icon.png" />
    <link rel="manifest" href="/favicon/site.webmanifest" />
    <link rel="stylesheet" href="/site-header.css" />
    <link rel="stylesheet" href="/docs-page.css?v=${{version}}" />
    <script src="/site-header.js?v=${{version}}" defer></script>
    <script src="/docs-page.js?v=${{version}}" defer></script>
    <script src="/docs-explore.js?v=${{version}}" defer></script>
    <script src="/genie-spider.js?v=${{version}}" defer></script>
  </head>
  <body class="dp-body" data-reader-index="{reader_index}">
    <div data-forkmesh-header="simple"></div>
    <header class="dp-toolbar">
      <div class="dp-toolbar-inner">
        <div class="dp-search">
          <label class="dp-search-box" aria-label="Search documentation">
            <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="7"></circle><path d="m20 20-3.5-3.5"></path></svg>
            <input id="docs-search" type="search" placeholder="Search docs..." autocomplete="off" aria-controls="docs-search-panel" aria-expanded="false" />
            <span class="kbd">⌘K</span>
          </label>
          <div id="docs-search-panel" class="dp-search-panel" hidden role="listbox" aria-label="Documentation search results">
            <div id="docs-search-results"></div>
            <div id="docs-search-empty" hidden>No results found.</div>
          </div>
        </div>
        <nav class="dp-tabs" aria-label="Documentation areas">
{tabs}
        </nav>
      </div>
    </header>

    <main class="{shell_class}">
      <aside class="dp-sidebar">
        <details class="dp-nav" open>
          <summary>All documentation</summary>
          <nav aria-label="Documentation library">
{platforms}
{home}
{sidebar}
{promo}
          </nav>
        </details>
      </aside>

      <article class="dp-article">
        <div class="dp-topline">
        {crumbs}
        <div class="dp-actions">
          <button id="copy-page" class="dp-button" type="button">
            <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="8" y="8" width="12" height="12" rx="2"></rect><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"></path></svg>
            <span>Copy link</span>
          </button>
          <a class="dp-button" href="{source}">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M14 3v4a1 1 0 0 0 1 1h4"></path><path d="M17 21H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h7l5 5v11a2 2 0 0 1-2 2z"></path></svg>
            <span>View source</span>
          </a>
        </div>
        </div>
        {meta}
        <div class="dp-prose">
{body}
        </div>
        {library}
        {pager}
        <section class="dp-suggest" data-suggest hidden aria-labelledby="dp-suggest-title"></section>
        {related}
        {page_map}
        <section class="dp-feedback">
          <p data-feedback-status>Was this page helpful?</p>
          <div>
            <button class="dp-button" type="button" data-feedback-like>
              <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 10v11"></path><path d="M15 5.5 14 10h5.7a1.8 1.8 0 0 1 1.7 2.3l-2 6.4A3 3 0 0 1 16.5 21H7"></path><path d="M7 10H4a2 2 0 0 0-2 2v7a2 2 0 0 0 2 2h3"></path></svg>
              Yes
            </button>
            <button class="dp-button" type="button" data-feedback-dislike>
              <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M17 14V3"></path><path d="M9 18.5 10 14H4.3a1.8 1.8 0 0 1-1.7-2.3l2-6.4A3 3 0 0 1 7.5 3H17"></path><path d="M17 14h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2h-3"></path></svg>
              No
            </button>
          </div>
        </section>
        <footer class="dp-footer">
          <a href="/docs/">Docs home</a>
          <a href="/docs/#pillars">Pillars</a>
          <a href="{library_url}">Library</a>
          <a href="https://app.forkmesh.com/chat">Chat</a>
          <a href="/network">Network</a>
          <p>Powered by <strong>ForkMesh</strong></p>
        </footer>
      </article>

      <aside class="dp-rail">
        <div class="dp-rail-stack">
        {outline}
        {rail_extra}
        </div>
      </aside>
    </main>

    <div class="dp-modal" data-feedback-modal hidden role="dialog" aria-modal="true" aria-labelledby="feedback-modal-title">
      <div class="dp-modal-backdrop" data-feedback-close aria-hidden="true"></div>
      <form class="dp-modal-card" data-feedback-form>
        <h2 id="feedback-modal-title">What was confusing, missing, or could be improved?</h2>
        <p>Optional, but useful for improving the docs.</p>
        <textarea data-feedback-message maxlength="2000" placeholder="Tell us what would have helped."></textarea>
        <div class="dp-modal-actions">
          <button class="dp-button" type="button" data-feedback-close>Cancel</button>
          <button class="dp-button primary" type="submit">Send feedback</button>
        </div>
      </form>
    </div>
  </body>
</html>
""".replace("{library_url}", LIBRARY_URL).replace("{reader_index}", READER_INDEX_URL)


# ---------------------------------------------------------------- staging

def _inject(document: str, placeholder: str, replacement: str) -> str:
    if placeholder not in document:
        raise RuntimeError(f"docs index is missing the {placeholder} placeholder")
    return document.replace(placeholder, replacement, 1)


def inject_library(index_html: str, catalog: Catalog) -> str:
    index_html = _inject(index_html, SIDEBAR_PLACEHOLDER, landing_sidebar_html(catalog))
    index_html = _inject(index_html, LIBRARY_PLACEHOLDER, library_html(catalog, include_product=False))
    # The landing page already lists the hand-written product pages in its
    # own sidebar, so its library section only carries the Markdown tree.
    index_html = _inject(
        index_html, LIBRARY_NAV_PLACEHOLDER, library_nav_html(catalog, include_product=False)
    )
    return _inject(
        index_html, LIBRARY_TOC_PLACEHOLDER, library_toc_html(catalog, include_product=False)
    )


def stage_docs(destination: Path, docs_root: Path = DOCS_ROOT) -> Catalog:
    """Write the rendered library under ``destination/docs``."""
    catalog = load_catalog(docs_root)
    docs_destination = destination / "docs"
    for entry in catalog.documents:
        target = docs_destination / entry.url[len(DOCS_URL_PREFIX):] / "index.html"
        if target.exists():
            raise RuntimeError(f"rendered document {entry.url} would overwrite {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(page_html(catalog, entry), encoding="utf-8")
    for entry in catalog.assets:
        target = docs_destination / entry.url[len(DOCS_URL_PREFIX):]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((REPOSITORY / entry.source).read_bytes())
    for url, render in ((HISTORY_URL, history_page_html), (DOCUMENTS_URL, documents_page_html)):
        target = docs_destination / url[len(DOCS_URL_PREFIX):] / "index.html"
        if target.exists():
            raise RuntimeError(f"generated page {url} would overwrite {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(render(catalog), encoding="utf-8")
    reader_target = destination / READER_INDEX_URL.lstrip("/")
    if reader_target.exists():
        raise RuntimeError(f"the reader index would overwrite {reader_target}")
    reader_target.write_text(
        json.dumps(reader_index(catalog), ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    index_path = docs_destination / "index.html"
    index_path.write_text(
        inject_library(index_path.read_text(encoding="utf-8"), catalog),
        encoding="utf-8",
    )
    return catalog


def main() -> int:
    catalog = load_catalog()
    for area, members in catalog.grouped():
        print(f"{area.label}:")
        for entry in members:
            print(f"  {entry.url:60} {entry.title}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

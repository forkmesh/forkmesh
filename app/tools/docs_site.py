#!/usr/bin/env python3
"""Render the ``www/docs`` Markdown library into the hosted ``/docs/`` tree.

Every ``*.md`` under ``www/docs`` becomes ``/docs/<path>/`` with a shared shell
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
import posixpath
import re
from dataclasses import dataclass, field
from pathlib import Path

import docs_history
import docs_markdown


REPOSITORY = Path(__file__).resolve().parents[2]
WWW = REPOSITORY / "www"
DOCS_ROOT = WWW / "docs"
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


@dataclass(frozen=True)
class Area:
    key: str
    label: str
    blurb: str


AREAS: tuple[Area, ...] = (
    Area("product", "Product docs", "Hand-written guides for the desktop app, protocol and hosted service."),
    Area("guides", "Guides", "Getting started, the product promise, business model, identities, hosted repositories and imports."),
    Area("design", "Design", "Protocol and federation design decisions."),
    Area("engineering", "Engineering", "Quality gates, performance budgets and the definition of done."),
    Area("security", "Security", "Security boundaries and local integration contracts."),
    Area("operations", "Operations", "Deployment, recovery, gateways, headless nodes and service runbooks."),
    Area("mobile", "Mobile", "Mobile node contract, the owned Git engine and Siri on iPhone."),
    Area("strategy", "Strategy & research", "Launch readiness, weekly briefs and pricing research."),
    Area("superpowers", "Plans & specs", "Implementation plans and design specs for shipped features."),
    Area("schemas", "Schemas & data", "Machine-readable configuration and protocol schemas served verbatim."),
)
AREA_BY_KEY = {area.key: area for area in AREAS}
DIRECTORY_AREAS = {
    "": "guides",
    "design": "design",
    "engineering": "engineering",
    "security": "security",
    "operations": "operations",
    "mobile": "mobile",
    "research": "strategy",
    "strategy": "strategy",
    "launch": "strategy",
    "superpowers": "superpowers",
}
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
    ("/docs/cloudflare-worker/", "Relay & website", "The Cloudflare Worker that coordinates discovery and routing."),
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
    """Hosted URL for a ``www/docs``-relative Markdown path."""
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
    directory = relative.split("/")[0] if "/" in relative else ""
    return DIRECTORY_AREAS.get(directory, "guides")


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
    sources = sorted(path for path in docs_root.rglob("*.md") if path.is_file())
    documents: dict[str, Entry] = {}
    for path in sources:
        relative = path.relative_to(docs_root).as_posix()
        documents[relative] = Entry(
            url=document_url(relative, reserved),
            title="",
            description="",
            area=area_for(relative),
            source=f"www/docs/{relative}",
            history_path=f"www/docs/{relative}",
        )
    assets: dict[str, Entry] = {}
    for path in sorted(docs_root.rglob("*.json")):
        if not path.is_file():
            continue
        relative = path.relative_to(docs_root).as_posix()
        assets[relative] = Entry(
            url=asset_url(relative),
            title=_asset_title(relative),
            description=_asset_description(path),
            area="schemas",
            kind="asset",
            source=f"www/docs/{relative}",
            history_path=f"www/docs/{relative}",
        )

    for relative, entry in documents.items():
        rewriter = _link_rewriter(relative, documents, assets, entry)
        rendered = docs_markdown.render(
            (docs_root / relative).read_text(encoding="utf-8"),
            link_rewriter=rewriter,
        )
        entry.title = rendered.title or relative.rsplit("/", 1)[-1][: -len(".md")]
        entry.description = _truncate(rendered.description)
        entry.html = rendered.html
        entry.headings = rendered.headings
        entry.plain_text = rendered.plain_text

    def sort_key(item: tuple[str, Entry]):
        relative, entry = item
        order = [area.key for area in AREAS].index(entry.area)
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
        repo_relative = posixpath.normpath(posixpath.join("www/docs", directory, path))
        if repo_relative.startswith("www/docs/"):
            inner = repo_relative[len("www/docs/"):]
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


LIBRARY_PAGES: tuple[tuple[str, str], ...] = (
    (LIBRARY_URL, "Library overview"),
    (HISTORY_URL, "Recent updates"),
    (DOCUMENTS_URL, "All documents"),
)


def home_nav_html(current: str) -> str:
    """The three library-wide entry points above the per-area sidebar."""
    parts: list[str] = []
    for url, label in LIBRARY_PAGES:
        active = " active" if url == current else ""
        aria = ' aria-current="page"' if url == current else ""
        parts.append(
            f'<a class="dp-nav-link dp-nav-home{active}" href="{_e(url)}"{aria}>{_e(label)}</a>'
        )
    return "\n".join(parts)


def tabs_html(current: str) -> str:
    updates = current in {HISTORY_URL, DOCUMENTS_URL}
    tabs = (
        ("/docs/", "Desktop", False),
        (LIBRARY_URL, "Library", not updates),
        (HISTORY_URL, "Updates", updates),
        ("/blog", "Blog", False),
    )
    return "\n".join(
        f'<a class="{"active" if active else ""}" href="{_e(url)}">{_e(label)}</a>'
        for url, label, active in tabs
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
        parts.append(f'<section class="dp-nav-group"><h2>{_e(area.label)}</h2>')
        for entry in members:
            active = " active" if entry.url == current else ""
            aria = ' aria-current="page"' if entry.url == current else ""
            parts.append(
                f'<a class="dp-nav-link{active}" href="{_e(entry.url)}"{aria}'
                f' data-search-title="{_e(entry.title)}" data-search-body="{_e(entry.description)}"'
                f' data-search-href="{_e(entry.url)}">{_e(entry.title)}</a>'
            )
        parts.append("</section>")
    return "\n".join(parts)


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
        f'<a class="mb-4 block font-bold text-[var(--docs-muted)] no-underline" href="#library-{area.key}">{_e(area.label)}</a>'
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


def breadcrumbs_html(entry: Entry) -> str:
    area = AREA_BY_KEY[entry.area]
    crumbs = [f'<a href="/docs/">Docs</a>', f'<a href="{LIBRARY_URL}#library-{area.key}">{_e(area.label)}</a>']
    if entry.url != LIBRARY_URL:
        crumbs.append(f"<span>{_e(entry.title)}</span>")
    return '<nav class="dp-crumbs" aria-label="Breadcrumb">' + " <span aria-hidden=\"true\">/</span> ".join(crumbs) + "</nav>"


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
        meta=meta_html(entry),
        body=entry.html,
        library=library_section,
        pager="".join(pager),
        related=_card_list(f"More in {area.label}", siblings, entry),
        references=_card_list("Referenced from this page", references, entry),
        backlinks=_card_list("Pages that link here", entry.backlinks, entry),
        outline=outline_html(entry),
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
    references: str = "",
    backlinks: str = "",
    outline: str = "",
    wide: bool = False,
) -> str:
    """The shared documentation shell around already-rendered body HTML."""
    return PAGE_TEMPLATE.format(
        # A wide page (the sortable table) trades the outline rail for width.
        shell_class="dp-shell dp-shell-wide" if wide else "dp-shell",
        title=_e(title),
        description=_e(description),
        url=_e(url),
        home=home_nav_html(url),
        tabs=tabs_html(url),
        sidebar=sidebar_html(catalog, url),
        crumbs=crumbs,
        meta=meta,
        source=_e(source),
        body=body,
        library=library,
        pager=pager,
        related=related,
        references=references,
        backlinks=backlinks,
        outline=outline,
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
            f'{len(history.commits)}. <a href="{_e(SOURCE_TREE_URL + "www/docs")}">Read the rest '
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
    <script src="/genie-spider.js?v=${{version}}" defer></script>
  </head>
  <body class="dp-body">
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
{home}
{sidebar}
          </nav>
        </details>
      </aside>

      <article class="dp-article">
        {crumbs}
        {meta}
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
        <div class="dp-prose">
{body}
        </div>
        {library}
        {pager}
        {related}
        {references}
        {backlinks}
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
          <a href="{library_url}">Library</a>
          <a href="https://app.forkmesh.com/chat">Chat</a>
          <a href="/network">Network</a>
          <p>Powered by <strong>ForkMesh</strong></p>
        </footer>
      </article>

      <aside class="dp-rail">
        {outline}
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
""".replace("{library_url}", LIBRARY_URL)


# ---------------------------------------------------------------- staging

def _inject(document: str, placeholder: str, replacement: str) -> str:
    if placeholder not in document:
        raise RuntimeError(f"docs index is missing the {placeholder} placeholder")
    return document.replace(placeholder, replacement, 1)


def inject_library(index_html: str, catalog: Catalog) -> str:
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

"""The Markdown tree under docs is rendered into the hosted /docs/ tree.

app/tools/docs_site.py runs during ``build_site_assets.py www``; these tests
pin the contract: every document and schema gets a URL, hand-written pages are
never overwritten, cross-document links resolve, and the landing page lists
the whole library.
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


WWW = Path(__file__).resolve().parents[1]
REPOSITORY = WWW.parent
PUBLIC = WWW / "public"
DOCS = WWW.parent / "docs"
APP_TOOLS = REPOSITORY / "app" / "tools"
if str(APP_TOOLS) not in sys.path:
    sys.path.insert(0, str(APP_TOOLS))

import docs_history  # noqa: E402
import docs_markdown  # noqa: E402
import docs_site  # noqa: E402


def test_every_markdown_document_and_json_file_has_a_hosted_url():
    catalog = docs_site.load_catalog()
    sources = {entry.source for entry in catalog.entries if entry.source}
    expected = {
        f"docs/{path.relative_to(DOCS).as_posix()}"
        for path in DOCS.rglob("*")
        if path.is_file() and path.suffix in {".md", ".json"}
        and docs_site.is_published(path.relative_to(DOCS).as_posix())
    }
    assert sources == expected
    urls = [entry.url for entry in catalog.entries]
    assert len(urls) == len(set(urls))
    for entry in catalog.documents:
        assert entry.url.startswith("/docs/") and entry.url.endswith("/"), entry.url
        assert entry.title, entry.source
        assert entry.html, entry.source


def test_documents_never_shadow_hand_written_pages():
    reserved = docs_site.reserved_slugs()
    assert {"product-promise", "business-model", "qt-client", "git-compatibility"} <= reserved
    assert docs_site.document_url("product-promise.md", reserved) == "/docs/product-promise/reference/"
    assert docs_site.document_url("getting-started.md", reserved) == "/docs/getting-started/"
    assert docs_site.document_url("operations/mirror-node.md", reserved) == "/docs/operations/mirror-node/"
    assert docs_site.document_url("README.md", reserved) == docs_site.LIBRARY_URL
    assert (
        docs_site.document_url("engineering/quality-gate-waivers/README.md", reserved)
        == "/docs/engineering/quality-gate-waivers/"
    )


def test_relative_links_resolve_to_hosted_pages_schemas_or_source_tree():
    catalog = docs_site.load_catalog()
    by_url = catalog.by_url()
    relative = re.compile(r'href="(?!https?://|mailto:|#|/)([^"]+)"')
    for entry in catalog.documents:
        assert not relative.search(entry.html), (entry.source, relative.search(entry.html).group(0))
        for href in re.findall(r'href="([^"]+)"', entry.html):
            if href.startswith("/docs/"):
                target = href.split("#", 1)[0]
                assert target in by_url or (PUBLIC / target.lstrip("/")).exists(), (entry.source, href)
    readme = by_url[docs_site.LIBRARY_URL]
    assert 'href="/docs/getting-started/"' in readme.html
    assert f'href="{docs_site.SOURCE_TREE_URL}CONTRIBUTING.md"' in readme.html
    qt_client = by_url["/docs/qt-client/reference/"]
    assert "/docs/qt-client/reference/" in {
        link.url for link in by_url["/docs/getting-started/"].backlinks
    } or qt_client.backlinks


def test_cross_document_anchor_links_point_at_rendered_heading_ids():
    catalog = docs_site.load_catalog()
    by_url = catalog.by_url()
    checked = 0
    for entry in catalog.documents:
        for href in re.findall(r'href="(/docs/[^"#]+/)#([^"]+)"', entry.html):
            url, fragment = href
            target = by_url.get(url)
            if target is None:
                continue
            ids = {heading.id for heading in target.headings}
            ids.update(re.findall(r'<a id="([^"]+)"></a>', target.html))
            assert fragment in ids, (entry.source, url, fragment)
            checked += 1
    assert checked > 0


def test_mobile_desktop_control_docs_do_not_restore_the_lan_only_claim():
    catalog = docs_site.load_catalog()
    by_url = catalog.by_url()
    access = by_url["/docs/mobile/mobile-agent-access/"]
    contract = by_url["/docs/mobile/mobile-node-contract/"]
    assert "Controlling a desktop on the LAN or through the relay" in access.html
    assert "automatic same-account desktop discovery" in access.html
    assert "desktop-local pairing" in access.html
    assert "control of owner-sealed sessions through the relay is not" not in access.html
    assert "superseded for optional desktop control" in contract.html


def test_pages_cross_link_each_other():
    catalog = docs_site.load_catalog()
    entry = catalog.by_url()["/docs/operations/mirror-node/"]
    page = docs_site.page_html(catalog, entry)
    assert 'aria-current="page"' in page
    for other in catalog.entries:
        assert f'href="{other.url}"' in page, other.url
    assert "Pages that link here" in page
    assert "More in Operations" in page
    assert "Previous</span>" in page and "Next</span>" in page
    assert 'class="dp-outline"' in page
    assert f'href="{docs_site.SOURCE_TREE_URL}docs/operations/mirror-node.md"' in page
    assert 'href="/docs-page.css?v=${version}"' in page
    assert 'src="/docs-page.js?v=${version}"' in page
    assert 'src="/site-header.js?v=${version}"' in page
    assert '<div data-forkmesh-header="simple"></div>' in page
    assert 'fetch("/api/feedback"' in (PUBLIC / "docs-page.js").read_text(encoding="utf-8")


def test_landing_page_carries_library_placeholders_and_build_fills_them(tmp_path):
    index = (PUBLIC / "docs" / "index.html").read_text(encoding="utf-8")
    for placeholder in (
        docs_site.LIBRARY_PLACEHOLDER,
        docs_site.LIBRARY_NAV_PLACEHOLDER,
        docs_site.LIBRARY_TOC_PLACEHOLDER,
    ):
        assert placeholder in index
    assert 'href="/docs/library/"' in index

    destination = tmp_path / "dist"
    (destination / "docs").mkdir(parents=True)
    for path in (PUBLIC / "docs").rglob("*"):
        target = destination / "docs" / path.relative_to(PUBLIC / "docs")
        if path.is_dir():
            target.mkdir(exist_ok=True)
        else:
            target.write_bytes(path.read_bytes())
    catalog = docs_site.stage_docs(destination)

    built = (destination / "docs" / "index.html").read_text(encoding="utf-8")
    assert "forkmesh-docs-library" not in built
    for entry in catalog.entries:
        if entry.kind != "product":
            assert f'href="{entry.url}"' in built, entry.url
    assert 'href="#library-operations"' in built
    assert 'href="#library-product"' not in built
    for entry in catalog.documents:
        page = destination / "docs" / entry.url[len("/docs/"):] / "index.html"
        assert page.is_file(), entry.url
    for entry in catalog.assets:
        assert (destination / "docs" / entry.url[len("/docs/"):]).is_file(), entry.url
    # Hand-written pages survive untouched next to their rendered references.
    assert "See the promise work." in (destination / "docs/product-promise/index.html").read_text(encoding="utf-8")
    assert (destination / "docs/product-promise/reference/index.html").is_file()
    library = (destination / "docs/library/index.html").read_text(encoding="utf-8")
    assert 'id="every-document"' in library
    assert 'href="/docs/"' in library


def test_markdown_renderer_covers_the_syntax_the_docs_use():
    doc = docs_markdown.render(
        "# Title (`/x`)\n\n"
        "Intro with **bold**, *em*, `a | b`, snake_case, [link](other.md#sec) "
        "and <https://x.test/z> plus https://forkmesh.com/docs/.\n\n"
        "<!-- hidden -->\n\n"
        "| Goal | Guide |\n| --- | :---: |\n| Pipe `a \\| b` | [Go](go.md) |\n\n"
        "- one\n  - nested\n- two\n\n"
        "1. first\n2. second\n\n"
        "- [ ] todo\n- [x] done\n\n"
        "> quoted\n\n"
        "```json\n{\"a\": <b>}\n```\n\n"
        '<a id="anchor"></a>\n\n---\n\n## Section two\n',
        link_rewriter=lambda target, is_image: f"R:{target}",
    )
    assert doc.title == "Title (/x)"
    assert doc.description.startswith("Intro with bold, em, a | b, snake_case, link and")
    assert [heading.id for heading in doc.headings] == ["title-x", "section-two"]
    html = doc.html
    assert '<h1 id="title-x">' in html
    assert "<strong>bold</strong>" in html and "<em>em</em>" in html
    assert "snake_case" in html and "<em>case" not in html
    assert '<a href="R:other.md#sec">link</a>' in html
    assert '<a href="https://x.test/z">https://x.test/z</a>' in html
    assert '<a href="https://forkmesh.com/docs/">https://forkmesh.com/docs/</a>' in html
    assert "hidden" not in html
    assert '<td style="text-align:center"><a href="R:go.md">Go</a></td>' in html
    assert "<td>Pipe <code>a | b</code></td>" in html
    assert "<ul>\n<li>one\n<ul>\n<li>nested</li>\n</ul></li>\n<li>two</li>\n</ul>" in html
    assert "<ol>\n<li>first</li>\n<li>second</li>\n</ol>" in html
    assert '<input type="checkbox" disabled checked />' in html
    assert "<blockquote>\n<p>quoted</p>\n</blockquote>" in html
    assert '<pre><code class="language-json" data-language="json">{&quot;a&quot;: &lt;b&gt;}' not in html
    assert '<pre><code class="language-json" data-language="json">{"a": &lt;b&gt;}\n</code></pre>' in html
    assert '<a id="anchor"></a>' in html
    assert "<hr />" in html
    assert doc.links == ["other.md#sec", "https://x.test/z", "https://forkmesh.com/docs/", "go.md"]


def test_markdown_renderer_escapes_untrusted_html():
    doc = docs_markdown.render(
        "Placeholders like <owner>/<repo> and <script>alert(1)</script> stay text, "
        '<a href="javascript:alert(1)">x</a> loses its href.'
    )
    assert "&lt;owner&gt;/&lt;repo&gt;" in doc.html
    assert "<script>" not in doc.html and "&lt;script&gt;" in doc.html
    assert "javascript:" not in doc.html and "<a>x</a>" in doc.html


def test_markdown_renderer_keeps_a_bare_scheme_as_text():
    doc = docs_markdown.render('Links must start with https://").')
    assert "https://&quot;)." in doc.html and "<a " not in doc.html


def test_www_stage_renders_the_library(tmp_path, monkeypatch):
    import build_site_assets

    assert "docs_site.stage_docs(destination)" in (APP_TOOLS / "build_site_assets.py").read_text(encoding="utf-8")
    from dashboard_shell import CACHE_BUSTED_ASSETS

    assert "docs-page.js" in CACHE_BUSTED_ASSETS
    assert "docs-page.css" in CACHE_BUSTED_ASSETS
    assert (PUBLIC / "docs-page.js").is_file()
    assert (PUBLIC / "docs-page.css").is_file()
    assert build_site_assets  # imported for the side-effect assertion above


def test_generated_pages_own_their_slugs():
    reserved = docs_site.reserved_slugs()
    assert docs_site.GENERATED_SLUGS <= reserved
    assert docs_site.document_url("history.md", reserved) == "/docs/history/reference/"
    assert docs_site.document_url("documents.md", reserved) == "/docs/documents/reference/"
    assert docs_site.document_url("operations/history.md", reserved) == "/docs/operations/history/"


def test_history_log_parser_follows_renames_and_additions():
    record, field = docs_history.RECORD, docs_history.FIELD
    output = "\n".join(
        [
            f"{record}c3{field}300{field}Ada{field}Rename the guide{field}c2",
            "",
            "R087\tdocs/old.md\tdocs/new.md",
            "M\tdocs/other.md",
            f"{record}c2{field}200{field}Ada{field}Edit the guide",
            "",
            "M\tdocs/old.md",
            "D\tdocs/gone.md",
            f"{record}c1{field}100{field}Bob{field}Add the guide",
            "",
            "A\tdocs/old.md",
            f"{record}c0{field}50{field}Bob{field}Older edit",
            "",
            "M\tdocs/other.md",
        ]
    )
    commits, files = docs_history.parse_log(output)
    assert [commit.sha for commit in commits] == ["c3", "c2", "c1", "c0"]
    assert [commit.parent for commit in commits] == ["c2", "", "", ""]
    assert commits[0].changes[0] == docs_history.Change("renamed", "docs/new.md", "docs/old.md")
    # The older commits that named old.md are the same document as new.md.
    assert commits[1].changes[0].path == "docs/new.md"
    assert commits[1].changes[1].status == "removed"
    guide = files["docs/new.md"]
    assert (guide.added, guide.updated, guide.revisions, guide.added_known) == (100, 300, 3, True)
    other = files["docs/other.md"]
    assert (other.added, other.updated, other.revisions, other.added_known) == (50, 300, 2, False)


def test_history_degrades_without_a_repository(tmp_path):
    history = docs_history.load_history(tmp_path, ("docs",))
    assert history is docs_history.EMPTY and not history.available


def test_history_and_documents_pages_date_every_entry(tmp_path):
    destination = tmp_path / "dist"
    (destination / "docs").mkdir(parents=True)
    for path in (PUBLIC / "docs").rglob("*"):
        target = destination / "docs" / path.relative_to(PUBLIC / "docs")
        if path.is_dir():
            target.mkdir(exist_ok=True)
        else:
            target.write_bytes(path.read_bytes())
    catalog = docs_site.stage_docs(destination)
    for entry in catalog.entries:
        assert entry.updated > 0, entry.url
        # Files that only arrived through a merge still get their Git dates.
        if catalog.history.available:
            assert entry.revisions > 0, entry.url

    documents = (destination / "docs/documents/index.html").read_text(encoding="utf-8")
    assert "data-sortable" in documents and 'data-table-filter="documents-table"' in documents
    assert 'class="dp-shell dp-shell-wide"' in documents
    assert documents.count("<tr data-filter-text=") == len(catalog.entries)
    for entry in catalog.entries:
        assert f'href="{entry.url}"' in documents, entry.url
        assert f'data-sort-value="{entry.updated}"' in documents, entry.url
    assert 'aria-sort="descending"' in documents
    assert '<a class="active" href="/docs/history/">Updates</a>' in documents
    assert 'aria-current="page">All documents</a>' in documents

    history = (destination / "docs/history/index.html").read_text(encoding="utf-8")
    assert "Recent documentation updates" in history
    assert 'aria-current="page">Recent updates</a>' in history
    if catalog.history.available:
        newest = catalog.history.commits[0]
        assert newest.parent
        assert f'href="{docs_site.COMPARE_URL}{newest.parent}...{newest.sha}"' in history
        assert "/commit/" not in history
        assert 'class="dp-history"' in history and 'class="dp-outline"' in history
        # Every change to a page that still exists links to that page.
        by_path = {entry.history_path: entry for entry in catalog.entries if entry.history_path}
        for change in newest.changes:
            if change.path in by_path:
                assert f'href="{by_path[change.path].url}"' in history

    mirror_node = (destination / "docs/operations/mirror-node/index.html").read_text(encoding="utf-8")
    assert 'class="dp-meta" data-docs-meta' in mirror_node and "min read" in mirror_node
    assert 'href="/docs/documents/"' in mirror_node and 'href="/docs/history/"' in mirror_node
    library = (destination / "docs/library/index.html").read_text(encoding="utf-8")
    assert 'href="/docs/documents/"' in library
    landing = (destination / "docs/index.html").read_text(encoding="utf-8")
    assert 'href="/docs/history/"' in landing and 'href="/docs/documents/"' in landing


def test_docs_page_script_sorts_and_filters_tables():
    script = (PUBLIC / "docs-page.js").read_text(encoding="utf-8")
    assert 'table[data-sortable]' in script and "data-sort-value" in script
    assert "input[data-table-filter]" in script and "aria-sort" in script
    css = (PUBLIC / "docs-page.css").read_text(encoding="utf-8")
    for selector in (".dp-meta", ".dp-history", ".dp-table", ".dp-shell-wide", ".dp-stats"):
        assert selector in css, selector


VIEWS_HARNESS = """
import { readFileSync } from "node:fs";
const [source, visitors, dated] = [readFileSync(process.argv[2], "utf8"),
  Number(process.argv[3]), process.argv[4] === "dated"];
function element(tag) {
  return { tag, children: [], attributes: {}, textContent: "", title: "",
    setAttribute(name, value) { this.attributes[name] = value; },
    append(child) { this.children.push(child); }, addEventListener() {} };
}
const meta = element("p");
if (dated) meta.children.push(element("time"));
const requests = [];
globalThis.window = { location: { pathname: "/docs/intro/", hash: "" }, addEventListener() {} };
globalThis.document = {
  querySelector: (selector) => (selector === "[data-docs-meta]" ? meta : null),
  querySelectorAll: () => [], addEventListener() {}, createElement: element,
};
globalThis.fetch = async (url) => {
  requests.push(url);
  return { ok: visitors >= 0, json: async () => ({ ok: true, visitors }) };
};
new Function(source)();
await new Promise((resolve) => setTimeout(resolve, 10));
const last = meta.children.at(-1);
console.log(JSON.stringify({ requests, items: meta.children.map((child) => child.textContent),
  views: Boolean(last && "data-docs-views" in last.attributes), title: last ? last.title : "" }));
"""


def test_docs_page_shows_unique_views_from_analytics(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path / "views.mjs"
    harness.write_text(VIEWS_HARNESS, encoding="utf-8")

    def run(visitors, dated=True):
        output = subprocess.run(
            [node, str(harness), str(PUBLIC / "docs-page.js"), str(visitors),
             "dated" if dated else ""],
            check=True, capture_output=True, text=True, timeout=30).stdout
        return json.loads(output)

    shown = run(1234)
    assert shown["requests"] == ["/api/analytics/_/pages?path=%2Fdocs%2Fintro%2F"]
    assert shown["items"] == ["", "·", "1,234 views"] and shown["views"]
    assert "Unique visitors" in shown["title"]
    assert run(1)["items"][-1] == "1 view"
    assert run(3, dated=False)["items"] == ["3 views"]
    assert run(0)["items"] == [""]
    assert run(-1)["items"] == [""]
    css = (PUBLIC / "docs-page.css").read_text(encoding="utf-8")
    assert ".dp-meta:empty" in css


def _cpp_strings(source: str, name: str) -> list[str]:
    block = re.search(name + r"[^{]*=\s*\{(.*?)\n\s*\};", source, re.DOTALL)
    assert block, name
    return re.findall(r'QStringLiteral\("([^"]*)"\)', block.group(1))


def test_desktop_docs_package_mirrors_the_web_library():
    source = (
        REPOSITORY / "marketplace" / "packages" / "docs" / "content" / "desktop" / "DocsLibrary.cpp"
    ).read_text(encoding="utf-8")
    assert _cpp_strings(source, "kGuideOrder") == list(docs_site.GUIDE_ORDER)
    assert set(_cpp_strings(source, "kReservedSlugs")) == docs_site.reserved_slugs()
    directories = _cpp_strings(source, "QHash<QString, QString> map")
    assert dict(zip(directories[::2], directories[1::2])) == {
        key: value for key, value in docs_site.DIRECTORY_AREAS.items() if key
    }
    block = re.search(r"QList<DocsArea> list = \{(.*?)\n\s*\};", source, re.DOTALL)
    areas = re.findall(r'\{QStringLiteral\("([^"]*)"\), QStringLiteral\("([^"]*)"\)', block.group(1))
    assert areas == [(area.key, area.label) for area in docs_site.AREAS if area.key != "product"]


def test_rich_rendering_turns_data_blocks_into_views():
    source = (
        "```json\n{\"name\": \"node\", \"port\": 8080, \"tls\": true, \"owner\": null,\n"
        " \"tags\": [\"a\", \"b\"], \"peers\": [{\"host\": \"n1\", \"up\": true}, {\"host\": \"n2\", \"up\": false}]}\n```\n\n"
        "```json\n\"fragment\": {\"key\": \"<value>\"}\n```\n\n"
        "```json\n{\"a\": [...]}\n```\n\n"
        "```toml\n[calls]\nenabled = true\nport = 3479  # the UDP port\n# extra = 1\n```\n\n"
        "```dotenv\nTOKEN=<secret>\n```\n"
    )
    plain = docs_markdown.render(source).html
    assert "dp-figure" not in plain and plain.count("<pre>") == 5
    html = docs_markdown.render(source, rich=True).html
    assert html.count('class="dp-figure dp-data"') == 4
    assert "Object · 6 fields" in html
    assert '<span class="dp-v dp-v-num">8080</span>' in html
    assert '<span class="dp-v dp-v-bool dp-v-true">true</span>' in html
    assert '<span class="dp-v dp-v-null">null</span>' in html
    assert '<ul class="dp-chips">' in html
    assert '<td data-label="host"><span class="dp-v dp-v-str">n1</span></td>' in html
    assert '<span class="dp-v dp-v-hole">&lt;value&gt;</span>' in html
    assert '<pre><code class="language-json" data-language="json">{&quot;a&quot;: [...]}' not in html
    assert '<pre><code class="language-json" data-language="json">{"a": [...]}\n</code></pre>' in html
    assert '<p class="dp-conf-name"><code>[calls]</code></p>' in html
    assert '<small class="dp-kv-note">the UDP port</small>' in html
    assert '<small class="dp-kv-note">extra = 1</small>' in html
    assert "1 section · 2 settings" in html
    assert '<span class="dp-v dp-v-hole">&lt;secret&gt;</span>' in html
    assert html.count('<details class="dp-data-source"><summary>Show ') == 4


def test_rich_rendering_frames_diagrams_and_draws_flows():
    html = docs_markdown.render(
        "```text\n a ──► b\n │\n c\n```\n\n"
        "```text\nplain\nlines\nonly\n```\n\n"
        "```flow\ntitle: Request path\nClient -> DNS -> Core\nCore -> DNS\nProbe -> DNS\n```\n\n"
        "```flow\nnot a flow\n```\n",
        rich=True,
    ).html
    assert html.count('class="dp-figure dp-diagram"') == 1
    assert "--dp-diagram-columns:8" in html
    assert html.count('class="dp-figure dp-flow"') == 1
    assert '<span class="dp-figure-summary">Request path</span>' in html
    ranks = re.findall(r'<div class="dp-flow-rank">(.*?)</div>', html)
    assert [re.findall(r">([^<]+)</span>", rank) for rank in ranks] == [["Client", "Probe"], ["DNS"], ["Core"]]
    assert '<li data-from="2" data-to="1">Core → DNS</li>' in html
    assert '<pre><code class="language-flow" data-language="flow">not a flow\n</code></pre>' in html


def test_rich_tables_carry_labels_for_stacked_phone_cards():
    source = "| Part | Does | Code |\n| --- | --- | --- |\n| **Node** | Holds `repos` | `desktop/` |\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n"
    html = docs_markdown.render(source, rich=True).html
    assert '<div class="table-wrap table-stack" data-columns="3"><table>' in html
    assert '<td data-label="Does">Holds <code>repos</code></td>' in html
    assert "<td>1</td>" in html
    assert "table-stack" not in docs_markdown.render(source).html


def test_pages_carry_the_page_map_suggestions_and_reader_script():
    catalog = docs_site.load_catalog()
    entry = catalog.by_url()["/docs/architecture/"]
    page = docs_site.page_html(catalog, entry)
    assert '<section class="dp-map"' in page and "data-map-hub" in page
    assert "Pages this one links to" in page
    assert page.count('class="dp-map-node"') >= docs_site.MAP_VISIBLE
    assert 'class="dp-map-more"' in page
    assert '<section class="dp-suggest" data-suggest hidden' in page
    assert f'data-reader-index="{docs_site.READER_INDEX_URL}"' in page
    assert 'src="/docs-explore.js?v=${version}"' in page
    assert 'class="dp-figure dp-flow"' in page
    lonely = docs_site.Entry(url="/docs/lonely/", title="Lonely", description="", area="guides")
    assert docs_site.page_map_html(lonely, []) == ""


def test_reader_index_lists_pages_links_and_keywords(tmp_path):
    catalog = docs_site.load_catalog()
    index = docs_site.reader_index(catalog)
    pages = index["pages"]
    assert {page["u"] for page in pages} == {
        entry.url for entry in catalog.entries if entry.kind in ("doc", "product")
    }
    by_url = {page["u"]: page for page in pages}
    node = by_url["/docs/operations/mirror-node/"]
    assert {"mirror", "node"} <= {word for word, _ in node["k"]}
    assert 0 < len(node["k"]) <= docs_site.KEYWORDS_PER_PAGE
    assert all(0 < weight <= 1 for _, weight in node["k"])
    linked = {pages[position]["u"] for position in node["o"]}
    assert linked == {url for url in catalog.by_url()["/docs/operations/mirror-node/"].outgoing if url in by_url}
    assert set(index["areas"]) == {area.key for area in docs_site.AREAS}


def test_reader_script_keeps_reading_history_on_the_device():
    script = (PUBLIC / "docs-explore.js").read_text(encoding="utf-8")
    node = shutil.which("node")
    if node:
        subprocess.run([node, "--check", str(PUBLIC / "docs-explore.js")], check=True, timeout=30)
    assert "forkmesh.docs.reader.v1" in script and "localStorage" in script
    assert re.findall(r"fetch\(([^,)]+)", script) == ["source"]
    assert 'getAttribute("data-reader-index")' in script
    assert "Nothing is sent anywhere." in script
    assert "Forget my reading" in script
    css = (PUBLIC / "docs-page.css").read_text(encoding="utf-8")
    for selector in (".dp-dock", ".dp-sheet", ".dp-peek", ".dp-figure", ".table-stack", ".dp-map-grid", ".dp-viewer"):
        assert selector in css, selector


def test_docs_art_partials_match_the_generator():
    import docs_art

    assert docs_art.stale_partials() == []
    for name in docs_art.partials():
        assert f'partials/docs-art/{name}' in (
            REPOSITORY / "www" / "templates" / "pages" / "docs" / "index.html"
        ).read_text(encoding="utf-8")


def test_pages_open_with_an_illustrated_hero_and_interwoven_rail():
    catalog = docs_site.load_catalog()
    entry = catalog.by_url()["/docs/censorship-resilience/"]
    page = docs_site.page_html(catalog, entry)
    hero = page[page.index('<header class="dp-hero">'):page.index("</header>", page.index('<header class="dp-hero">'))]
    assert "<h1 " in hero and 'class="dp-hero-lead"' in hero and "data-docs-meta" in hero
    assert 'class="dp-art"' in hero and 'class="dp-pillar-strip"' in hero
    assert '<a href="/docs/censorship-resilience/" aria-current="page">' in hero
    assert "<p>Part of the core documentation:" not in page
    assert '<section class="dp-callout">' in page
    assert 'class="dp-rail-card dp-rail-path"' in page and 'class="dp-rail-card dp-rail-related"' in page
    assert page.count('class="dp-art"') == 2
    for url, label in docs_site.DOCS_TABS:
        assert f'href="{url}">{label}</a>' in page
    other = docs_site.page_html(catalog, catalog.by_url()["/docs/operations/mirror-node/"])
    assert 'class="dp-hero"' in other and 'class="dp-pillar-strip"' not in other
    assert 'class="dp-nav-group current" open' in other


def test_every_area_has_an_icon_and_an_illustration():
    import docs_art

    for area in docs_site.AREAS:
        glyph, art = docs_site.AREA_STYLE[area.key]
        assert glyph in docs_art.ICONS
        assert 'class="dp-art"' in docs_art.art(art, "check")
    for _, art in docs_site.PILLAR_LABELS.values():
        assert 'class="dp-art"' in docs_art.art(art, "check")

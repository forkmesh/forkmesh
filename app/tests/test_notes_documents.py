import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
NOTES = ROOT / "public" / "dashboard" / "js" / "09-notes.js"
VIEW = ROOT / "public" / "dashboard" / "partials" / "views" / "notes.html"
PACKAGED = ROOT.parent / "marketplace" / "packages" / "notes" / "content" / "web" / "view.html"
PUBLIC = ROOT / "public" / "notes" / "view.html"

STUBS = """
function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;",
  })[char]);
}
function repoKey(repo) { return `${repo.owner || ""}/${repo.name || ""}`; }
function repoRawUrl(repo, path) { return `/raw/${path}`; }
function repoLinkUrl(repo, kind, path) { return `/${kind}/${path}`; }
function repoRecordUrl(repo, kind, number) { return `/${kind}/${number}`; }
function publicProfileUrl(name) { return `/@${name}`; }
"""


def _block(text, start):
    cursor = text.index("{", text.index(start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return cursor + 1
        cursor += 1


def _source():
    renderer = RENDERER.read_text(encoding="utf-8")
    start = renderer.index("  function mdSafeUrl(")
    end = _block(renderer, "  function renderMarkdown(raw")
    notes = NOTES.read_text(encoding="utf-8")
    helpers = notes[notes.index("  const NOTE_LAYOUT_DEFAULTS"):notes.index("  function printNoteDocument(")]
    return STUBS + renderer[start:end] + "\n" + helpers


def _run(expression):
    script = _source() + "\nconsole.log(JSON.stringify(" + expression + "));\n"
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def _layout(markdown):
    return _run("noteLayoutRead(%s)" % json.dumps(markdown))


def _render(markdown):
    return _run("renderMarkdown(%s, { noteDocument: true })" % json.dumps(markdown))


DEFAULTS = {"size": "a4", "orientation": "portrait", "margins": "normal",
            "cover": "no", "contents": "no", "numbers": "yes"}
SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect width="10" height="10"/></svg>'


def test_layout_line_parsing_follows_the_contract():
    assert _layout("Just text") == DEFAULTS
    assert _layout("<!-- page: size=letter; cover=yes -->\nBody") == {
        **DEFAULTS, "size": "letter", "cover": "yes"}
    assert _layout("<!--page:size = LEGAL;orientation=landscape;margins=wide;numbers=no-->") == {
        **DEFAULTS, "size": "legal", "orientation": "landscape", "margins": "wide", "numbers": "no"}
    assert _layout("<!-- page: size=a5; margins=huge; colour=red -->") == DEFAULTS
    assert _layout("<!-- page: size=letter -->\n<!-- page: size=legal -->")["size"] == "letter"
    assert _layout("```\n<!-- page: size=letter -->\n```\n") == DEFAULTS
    assert _layout("~~~\n```\n<!-- page: size=letter -->\n~~~\n")["size"] == "a4"
    assert _layout("<!-- page: __proto__=yes; toString=no -->") == DEFAULTS


def test_writing_the_layout_replaces_the_first_line_and_puts_it_on_top():
    written = _run('noteWithLayout("Intro\\n<!-- page: size=a4 -->\\nBody", '
                   '{ size: "letter", orientation: "landscape", margins: "wide", '
                   'cover: "yes", contents: "no", numbers: "yes" })')
    assert written == ("<!-- page: size=letter; orientation=landscape; margins=wide; "
                       "cover=yes; contents=no; numbers=yes -->\n\nIntro\nBody")
    defaults = _run('noteWithLayout("Body", %s)' % json.dumps(DEFAULTS))
    assert defaults == ("<!-- page: size=a4; orientation=portrait; margins=normal; "
                        "cover=no; contents=no; numbers=yes -->\n\nBody")
    assert _run('noteWithLayout(%s, %s)' % (json.dumps(defaults), json.dumps(DEFAULTS))) == defaults
    junk = _run('noteLayoutLine({ size: "a5", cover: "maybe" })')
    assert junk == ("<!-- page: size=a4; orientation=portrait; margins=normal; "
                    "cover=no; contents=no; numbers=yes -->")


def test_svg_blocks_become_image_figures_only_in_notes():
    html = _render("```svg Figure 1\n" + SVG + "\n```\n")
    assert '<figure class="note-figure"><img src="data:image/svg+xml;base64,' in html
    assert "<figcaption>Figure 1</figcaption>" in html
    assert "<svg" not in html and "&lt;svg" not in html
    prolog = _render('~~~SVG\n<?xml version="1.0"?>\n<!-- c --><svg viewBox="0 0 1 1"></svg>\n~~~\n')
    assert "note-figure" in prolog and "figcaption" not in prolog
    assert "note-figure" not in _render("```svg\n<div>no</div>\n```\n")
    assert "note-figure" not in _render("```svgfoo\n" + SVG + "\n```\n")
    big = "<svg>" + "x" * (262145 - len("<svg></svg>")) + "</svg>"
    assert len(big.encode()) == 262145
    assert "note-figure" not in _render("```svg\n" + big + "\n```\n")
    fits = "<svg>" + "x" * (262144 - len("<svg></svg>")) + "</svg>"
    assert "note-figure" in _render("```svg\n" + fits + "\n```\n")
    readme = _run("renderMarkdown(%s)" % json.dumps("```svg\n" + SVG + "\n```\n"))
    assert "note-figure" not in readme and "&lt;svg" in readme


def test_caption_is_escaped_and_the_svg_never_reaches_the_page_as_markup():
    html = _render('```svg <img src=x onerror=alert(1)>\n<svg><script>alert(1)</script></svg>\n```\n')
    assert "<script" not in html and "onerror=alert" not in html.replace("&lt;img src=x onerror=alert(1)&gt;", "")
    assert "&lt;img src=x onerror=alert(1)&gt;" in html


def test_page_break_and_layout_lines():
    for line in ("<!-- pagebreak -->", "<!--PAGEBREAK-->", "  <!--  pagebreak  -->  "):
        html = _render("One\n\n" + line + "\n\nTwo")
        assert html.count('class="note-page-break"') == 1, line
    assert "note-page-break" not in _render("<!-- pagebreak --> text")
    hidden = _render("<!-- page: size=letter; cover=yes -->\n\nBody")
    assert "page:" not in hidden and "size=letter" not in hidden and "Body" in hidden


def test_pdf_file_name_uses_the_markdown_slug():
    assert _run('["Project Proposal", "", "  ", "Ünïcode / x"].map(notePdfFileName)') == [
        "project-proposal.pdf", "note.pdf", "note.pdf", "n-code-x.pdf"]


def test_print_document_lays_out_cover_contents_body_and_numbers():
    markdown = ("# Summary\n\nText\n\n## Detail\n\n```\n# not a heading\n```\n\n"
                "<!-- pagebreak -->\n\n### Deep\n\n```svg Figure 1\n" + SVG + "\n```\n")
    layout = {**DEFAULTS, "size": "letter", "orientation": "landscape", "margins": "narrow",
              "cover": "yes", "contents": "yes"}
    html = _run("notePrintDocument({ ...%s, date: new Date(2026, 10, 1) })" % json.dumps({
        "title": "Project <Proposal>", "emoji": "📄", "markdown": markdown, "layout": layout,
    }))
    assert "@page { size: 279.4mm 215.9mm; margin: 12.7mm;" in html
    assert 'counter(page) " / " counter(pages)' in html
    assert "@page :first { @bottom-center { content: none; } }" in html
    assert '<section class="note-cover"><div class="note-cover-emoji">📄</div><h1>Project &lt;Proposal&gt;</h1>' in html
    assert "November 1, 2026" in html
    contents = html[html.index('<section class="note-contents">'):html.index("</section>", html.index('<section class="note-contents">'))]
    assert '<li class="note-contents-1"><a href="#note-summary">Summary</a></li>' in contents
    assert '<li class="note-contents-2"><a href="#note-detail">Detail</a></li>' in contents
    assert "Deep" not in contents and "not a heading" not in contents
    assert '<h1 id="note-summary"' in html
    assert 'class="note-page-break"' in html and "data:image/svg+xml;base64," in html
    assert "<title>project-proposal</title>" in html
    assert "<script" not in html
    plain = _run("notePrintDocument(%s)" % json.dumps({
        "title": "", "markdown": "Body", "layout": {**DEFAULTS, "numbers": "no"}}))
    assert "<article><h1>Untitled note</h1>" in plain
    assert "counter(page)" not in plain and "note-cover" not in plain.split("</style>")[1]
    assert "note-contents" not in plain.split("</style>")[1]


def test_page_markup_dialog_and_packaged_copy():
    view = VIEW.read_text(encoding="utf-8")
    assert PACKAGED.read_text(encoding="utf-8") == view
    assert "data-note-export-pdf" in view and "data-note-pdf-dialog" in view
    for field in ("size", "orientation", "margins", "cover", "contents", "numbers"):
        assert 'data-note-pdf-field="%s"' % field in view
    assert "Save as this note's page layout" in view
    notes = NOTES.read_text(encoding="utf-8")
    assert notes.count("renderMarkdown(markdown, { noteDocument: true })") >= 2
    assert 'frame.setAttribute("sandbox", "allow-same-origin allow-modals")' in notes


def test_published_note_page_draws_figures_and_hides_layout_lines():
    page = PUBLIC.read_text(encoding="utf-8")
    script = page[page.index("const esc="):page.index("(async()=>")]
    markdown = ("<!-- page: size=letter -->\n# T\n```svg Fig\n" + SVG + "\n```\n<!-- pagebreak -->\n"
                "```svg\n<div>x</div>\n```\n")
    result = subprocess.run(
        ["node", "--input-type=module"],
        input=script + "\nconsole.log(JSON.stringify(md(%s)));" % json.dumps(markdown),
        check=True, text=True, capture_output=True)
    html = json.loads(result.stdout)
    assert html.count('<figure class="figure">') == 1 and "<figcaption>Fig</figcaption>" in html
    assert 'class="pagebreak"' in html and "size=letter" not in html
    assert "<pre><code>&lt;div&gt;x&lt;/div&gt;</code></pre>" in html

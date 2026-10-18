#!/usr/bin/env python3
"""Executable contract for the README markdown renderer in the repo view.

The renderer lives in ``public/dashboard/js/06-repo-content.js`` and is the
only thing standing between a mirror-served README and ``innerHTML``, so these
run the real functions in node rather than asserting on their source text.
"""

import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRAGMENT = ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"

# The renderer, its HTML flattener and their helpers are one contiguous region
# of the fragment: from mdSafeUrl() to the end of renderMarkdown().
STUBS = """
function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;",
  })[char]);
}
function repoRawUrl(repo, path) {
  return `/api/repo/${repo.owner}/${repo.name}/raw?path=${encodeURIComponent(path)}`;
}
function repoLinkUrl(repo, kind, path) {
  return `/${repo.owner}/${repo.name}/${kind}/${path.split("/").map(encodeURIComponent).join("/")}`;
}
function repoRecordUrl(repo, kind, number) {
  return `/${repo.owner}/${repo.name}/${kind}/${number}`;
}
"""

REPO = '{ repo: { owner: "forkmesh", name: "forkmesh" }, basePath: "README.md" }'


def _renderer_source():
    text = FRAGMENT.read_text(encoding="utf-8")
    start = text.index("  function mdSafeUrl(")
    cursor = text.index("  function renderMarkdown(raw", start)
    cursor = text.index("{", cursor)
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                break
        cursor += 1
    return text[start:cursor + 1]


def _render(markdown, context=REPO):
    script = (
        STUBS
        + _renderer_source()
        + "\nconsole.log(JSON.stringify(renderMarkdown("
        + json.dumps(markdown)
        + f", {context})));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_wrapped_lines_stay_in_the_block_they_belong_to():
    # README prose is hard-wrapped; every wrapped line used to become its own
    # paragraph, so each bullet rendered as a bullet plus a stray paragraph.
    html = _render(
        "- Browse and clone public repositories through a stable URL backed by\n"
        "  live, integrity-checked mirrors.\n"
        "- Run repository Actions on labeled nodes.\n\n"
        "ForkMesh is actively developed and its public network is real, but it\n"
        "is not a finished replacement.\n"
    )
    assert "<li>Browse and clone public repositories through a stable URL backed by" \
        " live, integrity-checked mirrors.</li>" in html
    assert html.count("<li>") == 2
    assert html.count("<ul") == 1
    assert html.count("<p ") == 1


def test_numbered_record_references_link_to_the_named_item_type():
    html = _render(
        "Compare issue #12, PR #12, pull #13, and pull request #14 with #15."
    )
    link_class = (
        ' class="dashboard-accent-link underline underline-offset-2 '
        'hover:text-foreground">'
    )
    assert f'href="/forkmesh/forkmesh/issues/12"{link_class}issue #12</a>' in html
    assert f'href="/forkmesh/forkmesh/pulls/12"{link_class}PR #12</a>' in html
    assert f'href="/forkmesh/forkmesh/pulls/13"{link_class}pull #13</a>' in html
    assert (
        f'href="/forkmesh/forkmesh/pulls/14"{link_class}pull request #14</a>'
        in html
    )
    assert f'href="/forkmesh/forkmesh/issues/15"{link_class}#15</a>' in html


def test_html_header_block_renders_as_markdown_not_tag_soup():
    html = _render(
        '<p align="center">\n'
        '  <img src="logo.png" alt="ForkMesh" width="120">\n'
        "</p>\n\n"
        '<p align="center"><strong>Own your Git home.</strong></p>\n\n'
        '<p align="center">\n'
        '  <a href="https://app.forkmesh.com">App</a> &middot;\n'
        '  <a href="https://forkmesh.com/status">Status</a>\n'
        "</p>\n"
    )
    assert "&lt;p align" not in html and "&lt;img" not in html
    assert 'class="text-center"' in html
    assert 'src="/api/repo/forkmesh/forkmesh/raw?path=logo.png"' in html
    assert 'width="120"' in html
    assert "<strong>Own your Git home.</strong>" in html
    assert 'href="https://app.forkmesh.com"' in html and "·" in html


def test_relative_readme_links_open_repository_files():
    html = _render(
        "See [the compatibility matrix](www/docs/git-compatibility.md#limits).\n"
    )
    assert 'href="/forkmesh/forkmesh/blob/www/docs/git-compatibility.md#limits"' in html
    assert 'data-repo-markdown-link="blob"' in html
    assert 'data-repo-markdown-path="www/docs/git-compatibility.md"' in html
    assert 'data-repo-markdown-anchor="limits"' in html
    # In-repository links open in place instead of a new dashboard tab.
    assert 'target="_blank"' not in html


def test_relative_links_resolve_against_the_rendered_file_directory():
    html = _render(
        "[up](../README.md) [guide](./guide.md) [folder](operations/)\n",
        context='{ repo: { owner: "forkmesh", name: "forkmesh" }, basePath: "www/docs/intro.md" }',
    )
    assert 'href="/forkmesh/forkmesh/blob/www/README.md"' in html
    assert 'href="/forkmesh/forkmesh/blob/www/docs/guide.md"' in html
    assert 'href="/forkmesh/forkmesh/tree/www/docs/operations"' in html
    assert 'data-repo-markdown-link="tree"' in html


def test_headings_carry_github_style_anchors_for_section_links():
    html = _render(
        "See [step two](#2-add-an-independent-mirror).\n\n"
        "## 2. Add an independent mirror\n\n"
        "## Setup\n\n## Setup\n\n"
        "### [Clone `again`](https://example.test)\n"
    )
    assert 'href="#2-add-an-independent-mirror" data-repo-markdown-anchor="2-add-an-independent-mirror"' in html
    assert 'data-repo-markdown-heading="2-add-an-independent-mirror"' in html
    assert 'data-repo-markdown-heading="setup"' in html
    assert 'data-repo-markdown-heading="setup-1"' in html
    assert 'data-repo-markdown-heading="clone-again"' in html


def test_two_links_in_one_paragraph_keep_their_attributes():
    # The emphasis passes used to run over the anchor HTML emitted before
    # them, turning the `target="_blank" … "_blank` between two links into an
    # <em> and dropping both attributes.
    html = _render("See [projects](https://a.test) and [issues](https://b.test).\n")
    assert html.count('target="_blank"') == 2
    assert "<em>" not in html
    assert ">a_b_c</code> keeps its underscores" in _render("`a_b_c` keeps its underscores\n")


def test_issue_references_link_without_nesting_inside_markdown_links():
    html = _render(
        "Parent #12 and [external #34](https://example.test/thread#56).\n"
    )
    assert 'href="/forkmesh/forkmesh/issues/12"' in html
    assert 'href="https://example.test/thread#56"' in html
    assert ">external #34</a>" in html
    assert "/issues/34" not in html and "/issues/56" not in html
    assert html.count("<a ") == 2


def test_tables_nested_lists_and_hard_breaks():
    html = _render(
        "| Host | Role |\n| --- | --- |\n| app | API |\n\n"
        "- outer item\n  - inner item\n\n"
        "first<br>second\n"
    )
    assert "<table" in html and "<th" in html and "<td" in html and "app" in html
    assert html.count("<ul") == 2 and "<li>inner item</li></ul></li>" in html
    assert "<br />" in html


def test_untrusted_markup_never_reaches_innerhtml_intact():
    html = _render(
        "<script>alert(1)</script>\n\n"
        '<img src="x" onerror="alert(1)">\n\n'
        "[click](javascript:alert(1))\n\n"
        "&lt;script&gt;alert(2)&lt;/script&gt;\n"
    )
    assert "<script" not in html
    assert "onerror" not in html
    assert "javascript:" not in html
    assert "&lt;script&gt;alert(2)" in html


def test_control_characters_cannot_smuggle_a_script_scheme_past_the_link_filter():
    import re
    for markdown in (
        "[x](&#1;javascript:alert%281%29)",
        "[x](\x01javascript:alert%281%29)",
        '<a href="&#1;javascript:alert%281%29">x</a>',
        '<a href="java&#9;script:alert%281%29">x</a>',
    ):
        for context in ("null", REPO):
            html = _render(markdown, context)
            for href in re.findall(r'href="([^"]*)"', html):
                assert not re.search(r"[\x00-\x1f\x7f]", href), html
                parsed = re.sub(r"[\t\n\r]", "", href).strip("\x00 ")
                assert not re.match(r"(?i)javascript:", parsed), html


def test_images_attached_on_the_web_render_from_their_data_urls():
    # Issue #684: the web issue form stores an attached or pasted screenshot
    # inside the signed body as a data: URL. The renderer used to rewrite it to
    # src="#", a broken image icon.
    png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwAD/wH+Aq8m1QAAAABJRU5ErkJggg=="
    jpeg = "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsL+f/9k="
    html = _render(
        f"Steps:\n\n![image.png](data:image/png;base64,{png})\n\n"
        f"![screenshot.png](data:image/jpeg;base64,{jpeg})\n",
        context='{ repo: { owner: "forkmesh", name: "forkmesh" } }',
    )
    assert f'<img src="data:image/png;base64,{png}" alt="image.png"' in html
    assert f'<img src="data:image/jpeg;base64,{jpeg}" alt="screenshot.png"' in html
    assert 'src="#"' not in html


def test_data_urls_stay_limited_to_raster_image_sources():
    svg = "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciLz4="
    html = _render(
        f"![vector](data:image/svg+xml;base64,{svg})\n\n"
        "![page](data:text/html;base64,PHNjcmlwdD4=)\n\n"
        '![quote](data:image/png;base64,iVBO"onerror="x)\n\n'
        "[open](data:image/png;base64,iVBORw0KGgo=)\n"
    )
    assert "data:image/svg+xml" not in html
    assert "data:text/html" not in html
    assert "onerror" not in html
    # A data: URL is only ever an image source, never a link target.
    assert 'href="data:' not in html


def test_notes_preview_without_a_repository_falls_back_to_alt_text():
    # 09-notes.js renders without repo context: a repo-relative image has no
    # URL this origin could serve, so its alt text stands in for it.
    html = _render("![Logo](logo.png)\n", context="undefined")
    assert "<img" not in html
    assert "Logo" in html


def test_task_lists_render_as_checkboxes_not_bracket_literals():
    # Pull request descriptions carry checklists; "[x]"/"[ ]" used to survive
    # into the conversation view as the literal characters.
    html = _render("- [x] shipped\n- [ ] pending\n- plain bullet\n")
    assert html.count('type="checkbox"') == 2
    assert html.count("disabled checked") == 1
    assert "[x]" not in html and "[ ]" not in html
    # Only the task items lose their bullet; a plain item in the same list keeps
    # the marker it always had.
    assert html.count("list-none") == 2
    assert "<li>plain bullet</li>" in html


def test_editable_issue_tasks_render_as_large_row_controls_with_relationships():
    context = (
        '{ repo: { owner: "forkmesh", name: "forkmesh" }, '
        'issueTasks: { enabled: true, editable: true } }'
    )
    html = _render(
        "- [ ] Ship it\n- [x] Already split (sub-issue #42)\n",
        context=context,
    )
    assert 'data-repo-issue-task-toggle="0"' in html
    assert 'data-repo-issue-task-drag' in html
    assert 'data-repo-issue-task-subissue="0"' in html
    assert "issue-task-box" in html and "issue-task-checkmark" in html
    assert 'class="issue-task-row -ml-5 list-none is-complete"' in html
    assert 'href="/forkmesh/forkmesh/issues/42"' in html
    assert "Sub-issue #42" in html
    # The relationship becomes one attached badge instead of duplicated prose.
    assert "Already split (sub-issue" not in html


def test_read_only_issue_tasks_keep_the_large_treatment_without_edit_actions():
    context = (
        '{ repo: { owner: "forkmesh", name: "forkmesh" }, '
        'issueTasks: { enabled: true, editable: false } }'
    )
    html = _render("- [ ] Visible task\n", context=context)
    assert "issue-task-box" in html
    assert 'type="checkbox" disabled' in html
    assert "data-repo-issue-task-toggle" not in html
    assert "data-repo-issue-task-drag" not in html
    assert "data-repo-issue-task-subissue" not in html


def test_a_list_nested_under_a_task_item_stays_inside_it():
    # The nesting pass appends the child list to the parent item, which is an
    # object once task state has to travel with it.
    html = _render("- [ ] outer\n  - inner\n")
    assert html.count("<ul") == 2
    assert "outer<ul" in html
    assert html.index("<li>inner</li>") < html.index("</ul></div></div></li></ul>")


def test_strikethrough_renders_from_both_spellings():
    # flattenMarkdownHtml() rewrites <del>/<s>/<strike> to "~~", which had no
    # inline rule of its own, so both spellings printed the tildes.
    html = _render("~~gone~~ and <del>also gone</del> and `~~kept~~`\n")
    assert "<del>gone</del>" in html
    assert "<del>also gone</del>" in html
    assert ">~~kept~~</code>" in html


def test_repository_svg_images_wait_for_the_safe_hydrator():
    html = _render('<p align="center"><img src="logo.svg" alt="Acme" width="120"></p>\n\n'
                   "![chart](docs/chart.png) ![badge](https://img.example.org/b.svg)\n")
    assert 'data-repo-svg-src="/api/repo/forkmesh/forkmesh/raw?path=logo.svg"' in html
    assert 'src="/api/repo/forkmesh/forkmesh/raw?path=logo.svg"' not in html.replace(
        "data-repo-svg-src=", "")
    assert 'src="/api/repo/forkmesh/forkmesh/raw?path=docs%2Fchart.png"' in html
    assert 'src="https://img.example.org/b.svg"' in html

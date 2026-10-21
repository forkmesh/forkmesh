import json
from pathlib import Path
import subprocess

from test_repo_readme_markdown import STUBS, _renderer_source

ROOT = Path(__file__).resolve().parents[1]
TASKS = ROOT / "public" / "dashboard" / "js" / "09-tasks.js"
BUILT = ROOT / "public" / "dashboard-page-tasks.js"


def _function_source(text, signature):
    start = text.index(signature)
    cursor = text.index("{", start)
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _task_markdown_source():
    text = TASKS.read_text(encoding="utf-8")
    start = text.index("  const TASK_MENTION_SOURCE")
    mention = text[start:text.index("\n", start)]
    return mention + "\n" + _function_source(
        text, "  function taskMentionLinkHtml(name)") + "\n" + _function_source(
        text, "  function taskMarkdownHtml(value)")


def _render(markdown, actor="bob"):
    script = (
        STUBS
        + "const state = { selectedRepo: null, tasksView: { actor: "
        + json.dumps(actor) + " } };\n"
        + 'const MD_BODY_CLASS = "md-body";\n'
        + _renderer_source()
        + "\n" + _task_markdown_source()
        + "\nconsole.log(JSON.stringify(taskMarkdownHtml("
        + json.dumps(markdown) + ")));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_task_details_render_headings_tables_lists_and_code():
    html = _render(
        "## Why\n"
        "The cores run **hot** and *slow* on `/api/tasks`.\n\n"
        "| Route | Queries |\n|---|---:|\n| /api/tasks | 12 |\n\n"
        "1. Measure\n2. Cut\n\n- bullet\n\n"
        "> quoted\n\n"
        "```\nif (a < b && c > d) {}\n```\n"
    )
    assert html.startswith('<div class="task-markdown md-body">')
    assert "<h2" in html and ">Why</h2>" in html
    assert "<strong>hot</strong>" in html and "<em>slow</em>" in html
    assert ">/api/tasks</code>" in html
    assert "<table" in html and ">Route</th>" in html and ">12</td>" in html
    assert '<ol class="list-decimal' in html and "<li>Measure</li>" in html
    assert '<ul class="list-disc' in html
    assert "<blockquote" in html
    assert "<pre" in html and "if (a &lt; b &amp;&amp; c &gt; d) {}" in html
    assert "| Route |" not in html


def test_task_links_keep_safe_schemes_only():
    html = _render(
        "[docs](https://forkmesh.com/docs) [mail](mailto:ops@forkmesh.com) "
        "[bad](javascript:alert(1)) [data](data:text/html,x) "
        "[vb](vbscript:msgbox)"
    )
    assert 'href="https://forkmesh.com/docs"' in html
    assert 'rel="noopener noreferrer nofollow ugc"' in html
    assert 'href="mailto:ops@forkmesh.com"' in html
    assert "javascript:" not in html
    assert "data:text" not in html
    assert "vbscript:" not in html
    assert html.count('href="#"') == 3


def test_task_markdown_never_injects_raw_html():
    html = _render(
        "<script>alert(1)</script>\n\n"
        '<img src=x onerror="alert(1)">\n\n'
        "Hello <b onclick=alert(1)>there</b>\n\n"
        '[x"><img src=x onerror=alert(1)>](https://ok.test)\n'
    )
    assert "<script" not in html
    assert "<img" not in html
    assert "<b " not in html
    assert " onclick=" not in html and " onerror=" not in html.replace(
        "&lt;img src=x onerror=", "")


def test_task_mentions_highlight_outside_code_and_links():
    html = _render(
        "Ping @bob and @carol.\n\n`@bob in code`\n\n"
        "[see @bob](https://example.test/@bob)"
    )
    assert (
        '<a class="task-mention" href="https://code.example.test/@bob" '
        'data-issue-person="bob" data-me="true">@bob</a>'
    ) in html
    assert (
        '<a class="task-mention" href="https://code.example.test/@carol" '
        'data-issue-person="carol">@carol</a>'
    ) in html
    assert ">@bob in code</code>" in html
    assert 'href="https://example.test/@bob"' in html
    assert html.count("task-mention") == 2


def test_task_detail_uses_markdown_for_every_multiline_field():
    text = TASKS.read_text(encoding="utf-8")
    assert "taskMarkdownHtml(task.details)" in text
    assert "taskMarkdownHtml(qa.howToTest)" in text
    assert "taskMarkdownHtml(task.completionNote)" in text
    assert "taskMarkdownHtml(entry.body)" in text
    assert "taskMarkdownHtml(entry.note)" in text
    assert "function taskMarkdownHtml(value)" in BUILT.read_text(
        encoding="utf-8")

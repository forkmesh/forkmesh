import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
JS = ROOT / "app/public/dashboard/js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
SHELL = (ROOT / "app/public/dashboard/shell.html").read_text(encoding="utf-8")
BUNDLE = (ROOT / "app/public/dashboard.js").read_text(encoding="utf-8")
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")


def _slice(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def _css_rule(selector):
    match = re.search(re.escape(selector) + r"\s*\{([^}]*)\}", SHELL)
    assert match, selector
    return match.group(1)


SECTION = _slice(
    CONTENT,
    "function renderRepoPullFilesSection(pullPatch, viewed, options = {})",
    "function diffPathParts(path)",
)


def test_header_has_exactly_two_rows_checks_then_current_file():
    head = _slice(SECTION, "data-repo-diff-head", "data-repo-pull-review-batch")
    rows = re.findall(r'class="fm-diff-row[^"]*"', head)
    assert len(rows) == 2
    assert head.index('data-repo-diff-bar="checks"') < head.index("data-repo-diff-current")
    for marker in (
        "data-repo-diff-ci",
        "data-repo-diff-quality",
        'data-repo-pull-diff-mode="unified"',
        'data-repo-pull-diff-mode="split"',
        "data-repo-pull-viewed-summary",
        "data-repo-diff-collapse-all",
        "data-repo-diff-settings",
        "data-repo-diff-explorer-toggle",
    ):
        assert marker in head


def test_header_rows_are_thin_and_sticky():
    head = _css_rule(".fm-diff-head")
    assert "position: sticky" in head and "top: 0" in head
    assert "--fm-diff-row: 30px" in _css_rule(".fm-diff")
    row = _css_rule(".fm-diff-row")
    assert "height: var(--fm-diff-row)" in row
    assert "white-space: nowrap" in row and "overflow: hidden" in row
    assert "min-height: 0" in SHELL[SHELL.index(".fm-diff-head :where(button, input, summary)"):][:200]


def test_per_file_headers_no_longer_stick_and_files_clear_the_header():
    block = _slice(CONTENT, "function renderDiffFileBlock(", "function pullToolbarToggle(")
    assert "sticky" not in block
    assert "rounded-lg border" not in block
    assert 'class="fm-diff-file-head"' in block
    assert "scroll-margin-top: var(--fm-diff-head)" in _css_rule(".fm-diff-file")
    assert "scroll-margin-top" in _css_rule(".fm-diff [data-repo-pull-diff-row]")
    blocks = _slice(CONTENT, "function renderDiffFiles(", "function renderRepoPullPatch(")
    assert "gap-3 p-3" not in blocks


def test_current_file_row_follows_the_scroll_spy():
    active = _slice(CONTENT, "function setRepoPullActiveFile(", "function scrollRepoPullDiffBlockIntoView(")
    assert "updateRepoDiffCurrent(section, path)" in active
    spy = _slice(CONTENT, "function syncRepoPullActiveFileFromScroll()", "function renderDiffFiles(")
    assert 'section.querySelector("[data-repo-diff-head]")' in spy
    assert "head.getBoundingClientRect().bottom" in spy
    assert "syncRepoPullActiveFileFromScroll()" in NETWORK
    toggle = _slice(CONTENT, "function toggleRepoPullViewed(", "function swapLucideIcon(")
    assert "checkbox.checked = selected" in toggle


def test_full_width_applies_only_while_a_diff_pane_is_visible():
    selector = "[data-repo-content-grid]:has([data-repo-diff-fullscreen]:not(.hidden *))"
    rule = _css_rule(selector)
    assert "max-width: none" in rule and "padding: 0" in rule
    every = re.findall(r"data-repo-diff-fullscreen", CONTENT + COMPOSE + NETWORK)
    assert len(every) == 1
    assert "data-repo-diff-fullscreen" in SECTION
    tab = _slice(
        (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8"),
        "function setRepoTab(tab)",
        "function activateRepoTab(tab)",
    )
    assert 'const fullBleed = tab === "mirrors";' in tab


def test_compare_and_commit_diffs_use_the_same_pane():
    compare = _slice(COMPOSE, "async function openRepoCompare(", "async function fillRepoDiffCiPills(")
    assert "renderRepoPullFilesSection({ patch, files }, viewed, { reviewable: false })" in compare
    assert "<details data-repo-compare-commits-box" in compare
    commit = _slice(COMPOSE, "function renderRepoCommitDiff(diff, imageDiffs)", "function renderRepoCommitDetail(")
    assert "renderRepoPullFilesSection({ patch: diff, files: parsePatchStats(diff), imageDiffs }" in commit
    context = _slice(CONTENT, "function repoPullFilesContext()", "function refreshRepoPullExplorer()")
    assert 'section.closest("[data-repo-commit-detail]")' in context
    assert "loadRepoCommitViewed(commit)" in context
    assert "`forkmesh.commit-viewed.v1:${owner}/${name}@${hash}`" in CONTENT
    css = _css_rule('[data-dashboard-repo-tab-panel="commits"] > div:has([data-repo-commit-detail])')
    assert "overflow: visible" in css


def test_long_lines_scroll_inside_the_file_and_never_widen_the_page():
    assert "white-space: pre" in _css_rule(
        '.fm-diff[data-diff-wrap="false"] [data-diff-mode="unified"] [data-repo-pull-diff-grid] > span:last-child')
    assert "width: max-content" in _css_rule(
        '.fm-diff[data-diff-wrap="false"] [data-diff-mode="unified"] .fm-diff-code')
    block = _slice(CONTENT, "function renderDiffFileBlock(", "function pullToolbarToggle(")
    assert 'data-repo-pull-diff-body class="${isViewed ? "hidden " : ""}overflow-x-auto"' in block
    assert "if (!pullDiffWraps(mode)) return 0;" in CONTENT


def test_keyboard_and_header_controls_are_wired_without_inline_handlers():
    keys = _slice(NETWORK, 'document.addEventListener("keydown", (event) => {', "function initPageHistory()")
    assert 'stepRepoDiffFile(event.key === "j" ? 1 : -1)' in keys
    assert 'event.key.toLowerCase() === "t"' in keys
    for marker in (
        '"[data-repo-diff-explorer-toggle]"',
        '"[data-repo-diff-step]"',
        '"[data-repo-diff-collapse-all]"',
        '"[data-repo-diff-current-viewed]"',
        '"[data-repo-diff-flag]"',
        '"[data-repo-diff-wrap]"',
    ):
        assert marker in NETWORK
    added = SECTION + _slice(CONTENT, "function diffPathParts(path)", "// The review surface on the page")
    assert not re.search(r"\son[a-z]+=", added)


def _run_node(source):
    if shutil.which("node") is None:
        pytest.skip("Node.js is unavailable")
    result = subprocess.run(["node", "-e", source], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def test_current_file_row_escapes_and_keeps_the_filename_visible():
    script = "\n".join([
        _slice(HELPERS, "  function escapeHtml(value) {", "  // Loading placeholder"),
        "let countFormatter = null;",
        _slice(HELPERS, "  function formatCount(value) {", "  // Compose-identity chip"),
        _slice(CONTENT, "  function diffPathParts(path) {", "  function updateRepoDiffCurrent("),
        """
        const files = [
          { path: "a.txt", adds: 1, dels: 0 },
          { path: "src/<img src=x>/very/long/dir/name.js", adds: 3, dels: 2 },
        ];
        const html = renderRepoDiffCurrent(files, files[1].path, new Set([files[1].path]));
        const first = renderRepoDiffCurrent(files, "a.txt", new Set());
        console.log(JSON.stringify({ html, first, none: renderRepoDiffCurrent([], "", new Set()) }));
        """,
    ])
    out = _run_node(script)
    html = out["html"]
    assert "<img" not in html and "&lt;img src=x&gt;" in html
    assert '<span class="fm-diff-dir"><span dir="ltr">src/&lt;img src=x&gt;/very/long/dir/</span></span>' in html
    assert '<span class="fm-diff-name">name.js</span>' in html
    assert " checked" in html and "2/2" in html
    assert re.search(r'data-repo-diff-step="1"[^>]*disabled', html)
    assert re.search(r'data-repo-diff-step="-1"[^>]*disabled', out["first"])
    assert "No changed files" in out["none"]


def test_quality_pills_count_added_lines_only():
    script = "\n".join([
        _slice(HELPERS, "  function escapeHtml(value) {", "  // Loading placeholder"),
        "let countFormatter = null;",
        _slice(HELPERS, "  function formatCount(value) {", "  // Compose-identity chip"),
        _slice(CONTENT, "  const DIFF_QUALITY_RULES = [", "  // The review surface on the page"),
        r"""
        const patch = [
          "diff --git a/app.js b/app.js",
          "--- a/app.js",
          "+++ b/app.js",
          "@@ -1,2 +1,4 @@",
          "-console.log('removed');",
          "+console.log('added');",
          "+// TODO: tidy",
          "+eval(code);",
          " unchanged();",
          "diff --git a/b.py b/b.py",
          "+++ b/b.py",
          "+requests.get(url, verify=False)",
        ].join("\n");
        const flags = diffQualityFlags(patch);
        console.log(JSON.stringify({ flags, pills: renderRepoDiffQualityPills(flags), clean: renderRepoDiffQualityPills(diffQualityFlags("")) }));
        """,
    ])
    out = _run_node(script)
    flags = out["flags"]
    assert flags["debug"] == {"count": 1, "path": "app.js"}
    assert flags["todo"] == {"count": 1, "path": "app.js"}
    assert flags["security"] == {"count": 2, "path": "app.js"}
    assert flags["conflict"]["count"] == 0
    assert 'data-repo-diff-flag="app.js"' in out["pills"]
    assert "Conflict" not in out["clean"]
    assert out["clean"].count("fm-diff-pill-success") == 2
    assert out["clean"].count("fm-diff-pill-muted") == 1
    assert "not a security review" in out["clean"]


def test_ci_pills_say_nothing_when_checks_cannot_be_read():
    pills = _slice(CONTENT, "function renderRepoDiffCiPills(", "const DIFF_QUALITY_RULES")
    assert 'if (!checks?.available) return "";' in pills
    assert 'pullCheckOutcomeCount(runs, "failed")' in pills
    assert "data-repo-diff-open-checks" in pills
    assert 'diffCi.innerHTML = renderRepoDiffCiPills(parsed.pullChecks, true)' in CONTENT
    assert "renderRepoPullFilesSection(pullPatch, pullViewed, { checks: parsed.pullChecks })" in CONTENT


def test_bundle_is_rebuilt():
    for marker in ("function renderRepoDiffCurrent(", "function stepRepoDiffFile(", "data-repo-diff-fullscreen"):
        assert marker in BUNDLE

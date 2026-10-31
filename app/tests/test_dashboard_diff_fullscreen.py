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
BLOCK = _slice(CONTENT, "function renderDiffFileBlock(", "function pullToolbarToggle(")
TOGGLE = _slice(CONTENT, "function toggleRepoPullViewed(", "function swapLucideIcon(")


def test_the_only_sticky_header_is_the_file_being_read():
    assert "fm-diff-head" not in SECTION.replace("fm-diff-side-head", "")
    head = _css_rule(".fm-diff-file-head")
    assert "position: sticky" in head and "top: 0" in head
    assert "height: var(--fm-diff-head)" in head
    assert "--fm-diff-head: 32px" in _css_rule(".fm-diff")
    for marker in (
        'class="fm-diff-file-head"',
        "data-repo-diff-explorer-toggle",
        'class="fm-diff-path"',
        "renderRepoDiffFileMarks(path, file.flagged)",
        "data-repo-pull-viewed=",
    ):
        assert marker in BLOCK
    assert "rounded-lg border" not in BLOCK
    assert "scroll-margin-top" in _css_rule(".fm-diff [data-repo-pull-diff-row]")


def test_changed_files_are_a_left_column_open_by_default():
    aside = _slice(SECTION, "<aside data-repo-diff-explorer", "</aside>")
    order = [
        "data-repo-diff-maximize",
        "data-repo-diff-settings",
        "data-repo-diff-explorer-toggle",
        "data-repo-diff-progress",
        "data-repo-pull-viewed-summary",
        "data-repo-diff-health",
        "data-repo-diff-ci",
        "data-repo-pull-file-filter",
        "data-repo-pull-file-list",
    ]
    positions = [aside.index(marker) for marker in order]
    assert positions == sorted(positions)
    for marker in ('data-repo-pull-diff-mode="unified"', 'data-repo-pull-diff-mode="split"', "data-repo-diff-collapse-all", "data-repo-diff-wrap"):
        assert marker in aside
    assert "explorerOpen: stored?.explorerOpen !== false" in CONTENT
    assert 'data-explorer-open="${pullExplorerShownOpen() ? "true" : "false"}"' in SECTION
    wide = SHELL[SHELL.index("@media (min-width: 1024px) {\n        .fm-diff[data-explorer-open"):][:700]
    assert "position: sticky" in wide and "top: 0" in wide
    assert "height: var(--fm-diff-viewport" in wide
    assert "grid-template-columns: var(--fm-diff-explorer) minmax(0, 1fr)" in wide
    drawer = SHELL[SHELL.index("@media (max-width: 1023px) {\n        .fm-diff[data-explorer-open"):][:1200]
    assert "position: fixed" in drawer and "fm-diff-drawer" in drawer
    assert "fm-diff-scrim" in drawer


def test_scroll_spy_reads_the_scrolling_view_and_sizes_the_column():
    spy = _slice(CONTENT, "function syncRepoPullActiveFileFromScroll()", "function renderDiffFiles(")
    assert "const edge = fitRepoDiffViewport(section) + 6;" in spy
    assert "repoDiffShown(section)" in spy
    fit = _slice(CONTENT, "function fitRepoDiffViewport(section)", "function toggleRepoDiffMaximize()")
    assert '"--fm-diff-viewport"' in fit
    assert "syncRepoPullActiveFileFromScroll()" in NETWORK
    assert 'document.addEventListener("fullscreenchange", syncRepoDiffMaximize);' in NETWORK
    assert "document.fullscreenEnabled ?" in SECTION
    assert "height: 100dvh" in _css_rule(".fm-diff:fullscreen")


def test_marking_viewed_animates_then_moves_to_the_next_unviewed_file():
    assert "const motion = repoDiffMotion();" in TOGGLE
    for marker in (
        'playRepoDiffAnimation(button, "pop")',
        "burstRepoDiffSparks(button, done ? 18 : 10)",
        'playRepoDiffAnimation(block.firstElementChild, "sweep")',
        'playRepoDiffAnimation(body, "leave")',
        "celebrateRepoDiffReview(section)",
        "window.setTimeout(finish, 280)",
        "repoDiffFoldTokens.get(block) !== token",
        "advanceRepoDiffReview(section, block)",
        "updateRepoDiffProgress(section, seen, context.files.length)",
    ):
        assert marker in TOGGLE
    advance = _slice(CONTENT, "function advanceRepoDiffReview(", "// Lucide swaps")
    assert 'item.dataset.viewed !== "true"' in advance
    assert "blocks.indexOf(block) + 1" in advance
    for keyframes in ("fm-diff-pop", "fm-diff-check", "fm-diff-ring", "fm-diff-spark", "fm-diff-sweep", "fm-diff-leave"):
        assert f"@keyframes {keyframes}" in SHELL
    reduced = SHELL[SHELL.index("@media (prefers-reduced-motion: reduce) {\n        .fm-diff [data-anim]"):][:600]
    assert "animation: none !important" in reduced
    assert "background: rgb(var(--dashboard-success-rgb) / .12)" in _css_rule('.fm-diff-viewed[data-viewed="true"]')
    assert "width: var(--fm-diff-progress, 0%)" in _css_rule(".fm-diff-progress > span")
    assert "background-size: 100% 1px" in _css_rule(
        '.fm-diff-explorer [data-repo-pull-file][data-viewed="true"] [data-repo-pull-file-name]')


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
    assert 'data-repo-pull-diff-body class="${isViewed ? "hidden " : ""}overflow-x-auto"' in BLOCK
    assert "if (!pullDiffWraps(mode)) return 0;" in CONTENT


def test_keyboard_and_controls_are_wired_without_inline_handlers():
    keys = _slice(NETWORK, 'document.addEventListener("keydown", (event) => {', "function initPageHistory()")
    assert 'stepRepoDiffFile(event.key === "j" ? 1 : -1)' in keys
    assert 'event.key === "v" && toggleRepoDiffCurrentViewed(state.selectedRepo)' in keys
    assert 'event.key === "Escape" && pullExplorerIsDrawer()' in keys
    assert 'event.key.toLowerCase() === "t"' in keys
    for marker in (
        '"[data-repo-diff-explorer-toggle]"',
        '"[data-repo-diff-explorer-scrim]"',
        '"[data-repo-diff-maximize]"',
        '"[data-repo-diff-collapse-all]"',
        '"[data-repo-diff-mark-next]"',
        '"[data-repo-diff-wrap]"',
        '"[data-repo-diff-settings][open], [data-repo-diff-health-pill][open]"',
    ):
        assert marker in NETWORK
    for gone in ("data-repo-diff-current", "data-repo-diff-step", "data-repo-diff-flag"):
        assert gone not in CONTENT + NETWORK
    health = _slice(CONTENT, "  const DIFF_LINE_FLAGS = {", "  // The review surface on the page")
    assert not re.search(r"\son[a-z]+=", SECTION + BLOCK + health)


def _run_node(source):
    if shutil.which("node") is None:
        pytest.skip("Node.js is unavailable")
    result = subprocess.run(["node", "-e", source], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def _health_script(body):
    return "\n".join([
        _slice(HELPERS, "  function escapeHtml(value) {", "  // Loading placeholder"),
        "let countFormatter = null;",
        "function platformName() { return \"ForkMesh\"; }",
        _slice(HELPERS, "  function formatCount(value) {", "  // Compose-identity chip"),
        _slice(CONTENT, "  function parseDiffFiles(rawText) {", "  function parsePatchStats("),
        _slice(CONTENT, "  const DIFF_LINE_FLAGS = {", "  // The review surface on the page"),
        body,
    ])


PATCH = "\n".join([
    "diff --git a/app/src/sync_client.py b/app/src/sync_client.py",
    "new file mode 100644",
    "--- /dev/null",
    "+++ b/app/src/sync_client.py",
    "@@ -0,0 +1,7 @@",
    "+import requests",
    "+def pull(url):",
    "+    # TODO: tidy",
    '+    response = requests.get("http://mirror.forkmesh.net/" + url, verify=False)',
    "+    try:",
    "+        return eval(response.text)",
    "+    except Exception:",
    "diff --git a/notes.md b/notes.md",
    "--- a/notes.md",
    "+++ b/notes.md",
    "@@ -1 +1 @@",
    "-console.log('removed');",
    "+<b>plain</b>",
])


def test_health_scores_match_the_phone_app():
    out = _run_node(_health_script(f"""
        const parsed = parseDiffFiles({json.dumps(PATCH)});
        const marks = annotateDiffLineFlags(parsed.files);
        const report = diffHealthReport(parsed.files);
        const rows = parsed.files[0].rows.filter((row) => row.type === "add").map((row) => row.flags || []);
        console.log(JSON.stringify({{
          report: report.map(({{ key, score, tone }}) => ({{ key, score, tone }})),
          rows,
          marked: [...marks.keys()],
          metrics: [...marks.get("app/src/sync_client.py").metrics.entries()],
          severe: [...marks.get("app/src/sync_client.py").severeMetrics],
          clean: diffHealthReport(parseDiffFiles("").files).map(({{ score }}) => score),
        }}));
    """))
    assert out["report"] == [
        {"key": "merge", "score": 55, "tone": "red"},
        {"key": "complexity", "score": 100, "tone": "green"},
        {"key": "quality", "score": 72, "tone": "orange"},
        {"key": "security", "score": 45, "tone": "red"},
        {"key": "network", "score": 36, "tone": "red"},
        {"key": "practices", "score": 97, "tone": "green"},
    ]
    assert out["rows"] == [
        [],
        [],
        ["todo"],
        ["tlsBypass", "insecureUrl", "request"],
        [],
        ["unsafeApi"],
        ["broadCatch"],
    ]
    assert out["marked"] == ["app/src/sync_client.py"]
    assert dict(out["metrics"]) == {"quality": 2, "security": 2, "network": 2}
    assert sorted(out["severe"]) == ["network", "security"]
    assert out["clean"] == [84, 100, 100, 100, 100, 100]


def test_secret_scan_skips_placeholders_and_the_ignore_marker():
    out = _run_node(_health_script("""
        console.log(JSON.stringify([
          diffLineFlags("a.py", 'api_key = "AbCdEfGhIjKlMnOpQrStUvWxYz012345"'),  // forkmesh-secret-scan:ignore-line
          diffLineFlags("a.py", 'api_key = "your-api-key-goes-here-please"'),
          diffLineFlags("a.py", 'token = "ghp_' + "a".repeat(36) + '"  # forkmesh-secret-scan:ignore-line'),
          diffLineFlags("a.txt", "x".repeat(150) + " "),
          diffLineFlags("a.js", "                if (ready) { go(); }"),
        ]));
    """))
    assert out[0] == ["secret"]
    assert out[1] == []
    assert out[2] == []
    assert out[3] == []
    assert out[4] == ["deepNesting"]


def test_indicators_render_escaped_and_jump_to_flagged_lines():
    out = _run_node(_health_script(f"""
        const parsed = parseDiffFiles({json.dumps(PATCH)});
        const marks = annotateDiffLineFlags(parsed.files);
        const summary = marks.get("app/src/sync_client.py");
        console.log(JSON.stringify({{
          pills: renderRepoDiffHealthPills(diffHealthReport(parsed.files), marks),
          fileMarks: renderRepoDiffFileMarks("<x>.py", summary),
          dot: renderRepoDiffFileDot(summary),
          none: renderRepoDiffFileDot(undefined),
          tag: renderDiffLineFlagTag(["tlsBypass", "request"]),
          attrs: diffLineFlagAttributes(["todo", "request"]),
          clean: renderRepoDiffHealthPills(diffHealthReport([]), new Map()),
        }}));
    """))
    pills = out["pills"]
    assert pills.count("<details data-repo-diff-health-pill=") == 6
    assert "fm-diff-pill-danger" in pills and "fm-diff-pill-attention" in pills and "fm-diff-pill-success" in pills
    assert 'data-repo-diff-mark-next="security"' in pills
    assert 'data-repo-diff-mark-next="complexity"' not in pills
    assert "Green 85–100 · orange 60–84 · red 0–59" in pills
    assert pills.count("not a security review") == 1
    clean = out["clean"]
    assert re.search(r'data-repo-diff-health-pill="security" class="fm-diff-menu">\s*<summary class="fm-diff-pill fm-diff-pill-muted"', clean)
    assert "fm-diff-pill-success" in clean
    assert 'data-repo-diff-mark-file="&lt;x&gt;.py"' in out["fileMarks"]
    assert '<button type="button" data-repo-diff-mark-next="security"' in out["fileMarks"]
    assert re.search(r'data-repo-diff-mark-next="quality"[^>]*data-tone="attention"', out["fileMarks"])
    assert re.search(r'data-repo-diff-mark-next="security"[^>]*data-tone="danger"', out["fileMarks"])
    assert 'data-tone="danger"' in out["dot"] and out["none"] == ""
    assert out["tag"] == '<span class="fm-diff-flag" data-tone="danger">⚑ TLS bypass · network call</span>'
    assert out["attrs"] == ' data-flag="attention" data-flag-metrics="quality network"'
    jump = _slice(CONTENT, "function jumpToRepoDiffMark(", "// The review surface on the page")
    assert 'playRepoDiffAnimation(row, "flash")' in jump
    assert "foldRepoDiffBlock(block, false)" in jump


def test_ci_pills_say_nothing_when_checks_cannot_be_read():
    pills = _slice(CONTENT, "function renderRepoDiffCiPills(", "const DIFF_LINE_FLAGS")
    assert 'if (!checks?.available) return "";' in pills
    assert 'pullCheckOutcomeCount(runs, "failed")' in pills
    assert "data-repo-diff-open-checks" in pills
    assert 'diffCi.innerHTML = renderRepoDiffCiPills(parsed.pullChecks, true)' in CONTENT
    assert "renderRepoPullFilesSection(pullPatch, pullViewed, { checks: parsed.pullChecks })" in CONTENT


def test_bundle_is_rebuilt():
    for marker in ("function diffHealthReport(", "function advanceRepoDiffReview(", "data-repo-diff-fullscreen"):
        assert marker in BUNDLE
    assert "@keyframes fm-diff-spark" in (ROOT / "app/public/dashboard/repo.html").read_text(encoding="utf-8")

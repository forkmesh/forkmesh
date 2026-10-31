import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
AI = (JS_DIR / "09-ai-review.js").read_text("utf-8")
CONTENT = (JS_DIR / "06-repo-content.js").read_text("utf-8")
HELPERS = (JS_DIR / "02-helpers.js").read_text("utf-8")
ACCOUNT = (JS_DIR / "04-account.js").read_text("utf-8")
BOOT = (JS_DIR / "10-boot.js").read_text("utf-8")
PARTIAL = (ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html").read_text("utf-8")
BUILT = (ROOT / "public" / "dashboard" / "settings" / "index.html").read_text("utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text("utf-8")
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

sys.path.insert(0, str(ROOT / "src"))
import ai_review  # noqa: E402
import dashboard_bundle  # noqa: E402


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    assert match, name
    cursor = text.index("{", text.index(")", match.start()))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():cursor + 1]
        cursor += 1


def _section(html):
    start = html.index('data-settings-section="ai-review"')
    return html[start:html.index("</section>", start)]


def test_settings_section_is_admin_only_and_never_holds_the_token():
    for html in (PARTIAL, BUILT):
        link = re.search(r"<button[^>]*data-settings-section-link=\"ai-review\"[^>]*>", html).group(0)
        assert "data-ai-review-settings-link" in link and " hidden" in link
        section = _section(html)
        token = re.search(r"<input data-ai-review-token[^>]*>", section).group(0)
        assert 'type="password"' in token and 'autocomplete="off"' in token
        assert "value=" not in token
        for hook in ("data-ai-review-enabled", "data-ai-review-default-on",
                     "data-ai-review-account", "data-ai-review-model",
                     "data-ai-review-daily-cap", "data-ai-review-max-diff",
                     "data-ai-review-repo-daily", "data-ai-review-author-daily",
                     "data-ai-review-clear-token", "data-ai-review-settings-status"):
            assert hook in section
        assert "approved list" in section and "Workers AI" in section
    assert '"ai-review"]' in ACCOUNT
    assert "void initAiReviewSettings();" in BOOT
    init = _function(AI, "initAiReviewSettings")
    assert 'fetchJson("/api/ai-review/settings", { fresh: true })' in init
    assert "link.hidden = false" in init.split("fillAiReviewSettings")[0]
    fill = _function(AI, "fillAiReviewSettings")
    assert '[data-ai-review-token]").value = ""' in fill


def test_bundle_carries_the_fragment():
    assert "09-ai-review.js" in dashboard_bundle.FRAGMENTS
    names = list(dashboard_bundle.FRAGMENTS)
    assert names.index("09-agents.js") < names.index("09-ai-review.js") < names.index("09-apps.js")
    for name in ("renderPullAiReviewComment", "renderPullAiReviewSidebar",
                 "loadPullAiReview", "initAiReviewSettings"):
        assert "function %s(" % name in BUNDLE


def test_pull_page_wires_the_sidebar_chip_and_the_comment_renderer():
    assert "${isPulls ? renderPullAiReviewSidebar(repo, number) : \"\"}" in CONTENT
    assert 'if (kind === "pulls") void loadPullAiReview(repo, number);' in CONTENT
    conversation = _function(CONTENT, "renderRepoPullConversation")
    assert "isPullAiReviewEvent(ev)" in conversation and "latestPullAiReviewEvent(rows)" in conversation
    assert "/api/ai-review/pulls/" in _function(AI, "pullAiReviewApiPath")
    assert "encodeURIComponent" in _function(AI, "pullAiReviewApiPath")
    assert 'AI_REVIEW_TITLE = "### 🤖 Automated AI review"' in AI
    assert ai_review.comment_title("m", "abc").startswith("### " + ai_review.TITLE_PREFIX)


def test_ai_review_markup_uses_theme_tokens_only():
    classes = " ".join(re.findall(r'class="([^"]*)"', AI))
    assert not re.search(r"\b(?:text|bg|border)-(?:green|emerald|red|blue|gray|slate)-\d", classes)
    assert "text-success" in _function(AI, "pullAiReviewChipClass")
    assert "motion-reduce:animate-none" in AI


def _run(script):
    code = "\n".join([
        _function(HELPERS, "escapeHtml"),
        "const MD_BODY_CLASS = 'md';",
        "const state = { session: {} };",
        "const document = { addEventListener() {} };",
        "const window = {};",
        "function renderRecordMarkdown(text) { return '<md>' + escapeHtml(text) + '</md>'; }",
        "function formatRecordDateTime() { return { iso: 'iso', label: 'label' }; }",
        "function formatTimeAgo() { return 'now'; }",
        "function issueCardAuthorHtml(name) { return '<b>' + escapeHtml(name) + '</b>'; }",
        "function issueActorAvatarHtml(name, author, size, bot) { return bot ? '<bot>' : '<person>'; }",
        "function commentReactionEventBar() { return ''; }",
        AI,
        script,
    ])
    result = subprocess.run(["node", "-"], input=code, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@needs_node
def test_newest_ai_review_is_open_and_older_ones_are_folded():
    old = ai_review.comment_body("@cf/openai/gpt-oss-120b", "a" * 40,
                                 "### Summary\nold\n### Suggested verdict\ncomment\n### Blockers\nNone",
                                 ai_review.prepare_diff(""))
    new = ai_review.comment_body("@cf/openai/gpt-oss-120b", "b" * 40,
                                 "### Summary\nnew <script>x</script>\n### Suggested verdict\napprove\n"
                                 "### Blockers\nNone\n### Security\nNone",
                                 ai_review.prepare_diff(""))
    events = [
        {"type": "comment", "author": "forkbot", "authorName": "ForkBot", "ts": 1, "id": "1", "body": old},
        {"type": "comment", "author": "bob", "ts": 2, "id": "2", "body": old},
        {"type": "comment", "author": "forkbot", "authorName": "ForkBot", "ts": 3, "id": "3", "body": new},
    ]
    out = _run("""
      const events = %s;
      const latest = latestPullAiReviewEvent(events);
      console.log(JSON.stringify({
        flags: events.map(isPullAiReviewEvent),
        latest: latest.id,
        newest: renderPullAiReviewComment(events[2], true),
        older: renderPullAiReviewComment(events[0], false),
        raw: renderPullAiReviewComment({ type: "comment", author: "forkbot", ts: 4,
          body: "### 🤖 Automated AI review <img src=x onerror=alert(1)>\\n<script>x</script>\\n### Blockers\\n<b>y</b>" }, true),
      }));
    """ % json.dumps(events))
    assert out["flags"] == [True, False, True] and out["latest"] == "3"
    newest, older = out["newest"], out["older"]
    assert "data-ai-review-superseded" not in newest and "<bot>" in newest
    assert "AI review</span>" in newest and "data-ai-review-more" in newest
    assert newest.index("approve") < newest.index("Show the full report")
    assert "<script>" not in newest and "new x" in newest
    assert "data-ai-review-superseded" in older and "superseded" in older
    raw = out["raw"]
    assert "<script>" not in raw and "<img" not in raw and "<b>y" not in raw
    assert "&lt;script&gt;" in raw and "&lt;img" in raw
    assert '<span class="font-mono text-[12px]">aaaaaaa</span>' in older


@needs_node
def test_status_chip_and_run_button():
    out = _run("""
      console.log(JSON.stringify({
        maintainer: renderPullAiReviewSlotBody({ status: "reviewing", canRun: true, head: "%s" }),
        visitor: renderPullAiReviewSlotBody({ status: "skipped", reason: "draft", canRun: false }),
        done: renderPullAiReviewSlotBody({ status: "done", verdict: "comment", reviewedHead: "%s", canRun: true }),
        hidden: renderPullAiReviewSidebar({ owner: "a", name: "b" }, 7),
      }));
    """ % ("c" * 40, "d" * 40))
    assert 'data-status="reviewing"' in out["maintainer"] and "Reviewing" in out["maintainer"]
    assert "data-ai-review-run" in out["maintainer"] and " disabled" in out["maintainer"]
    assert "data-ai-review-run" not in out["visitor"] and "Skipped" in out["visitor"]
    assert "draft" in out["visitor"]
    assert "suggests comment · head ddddddd" in out["done"] and "text-success" in out["done"]
    assert 'data-repo-pull-ai-review="7"' in out["hidden"] and " hidden>" in out["hidden"]

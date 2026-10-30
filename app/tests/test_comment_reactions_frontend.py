#!/usr/bin/env python3
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
FRAGMENT = (JS_DIR / "09-comment-reactions.js").read_text("utf-8")
CONTENT = (JS_DIR / "06-repo-content.js").read_text("utf-8")
HELPERS = (JS_DIR / "02-helpers.js").read_text("utf-8")
BOOT = (JS_DIR / "10-boot.js").read_text("utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text("utf-8")
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

sys.path.insert(0, str(ROOT / "src"))
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


def _const(text, name):
    start = text.index("  const %s =" % name)
    return text[start:re.compile(r";\n").search(text, start).end()]


def _run(body):
    code = "\n".join([
        _const(CONTENT, "ISSUE_REACTION_EMOJI"), _const(CONTENT, "COMMENT_REACTIONS"),
        _const(CONTENT, "COMMENT_REACTION_QUICK"), _const(CONTENT, "COMMENT_REACTION_KINDS"),
        _const(CONTENT, "COMMENT_REACTION_GRATITUDE"), _const(CONTENT, "COMMENT_REACTION_STICKERS"),
        _const(CONTENT, "COMMENT_REACTION_CUSTOM_RE"),
        _function(HELPERS, "escapeHtml"), _function(HELPERS, "formatCount"),
    ] + [_function(CONTENT, name) for name in (
        "commentReactionSubject", "commentReactionLabel", "commentReactionKnown",
        "commentReactionSafeSrc", "commentReactionGlyph", "commentReactionTallies",
        "commentReactionWho", "renderCommentReactionChip", "renderCommentReactionExtras",
        "renderCommentReactionBar")]
        + [_function(FRAGMENT, name) for name in (
            "commentReactionParts", "commentReactionBase", "prepareCommentReactions",
            "seedCommentReactions", "commentReactionApplyLocal", "commentReactionAuthor",
            "commentReactionOptionHtml", "commentReactionSectionHtml",
            "renderCommentReactionPicker", "commentReactionErrorText")])
    script = """
let countFormatter = null;
const state = { session: { nodeName: "bob", sessionToken: "cookie" } };
function repoApiBase(repo) { return `/api/repo/${repo.owner}/${repo.name}`; }
let seedText = "";
globalThis.document = { getElementById: (id) => id === "forkmesh-comment-reactions" && seedText ? { textContent: seedText } : null };
%s
const out = {};
%s
process.stdout.write(JSON.stringify(out));
""" % (code, body)
    result = subprocess.run(["node", "-"], input=script, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


@needs_node
def test_optimistic_toggle_adds_and_takes_back_the_viewers_reaction():
    out = _run("""
state.repoRecordDetail = { repo: { owner: "alice", name: "app" } };
state.commentReactions = { "issue:7": { base: "/api/repo/alice/app", reactions: {
  c1: [{ emoji: "like", count: 2, mine: false, people: [{ name: "carol" }, { name: "dave" }] }] } } };
commentReactionApplyLocal("issue:7", "c1", "like", true);
out.added = state.commentReactions["issue:7"].reactions.c1[0];
commentReactionApplyLocal("issue:7", "c1", "like", true);
out.twice = state.commentReactions["issue:7"].reactions.c1[0].count;
commentReactionApplyLocal("issue:7", "c1", "rocket", true);
commentReactionApplyLocal("issue:7", "c1", "rocket", false);
commentReactionApplyLocal("issue:7", "c1", "like", false);
out.after = state.commentReactions["issue:7"].reactions.c1;
out.bar = renderCommentReactionBar("issue:7", "c1");
""")
    assert out["added"]["count"] == 3 and out["added"]["mine"] is True
    assert out["added"]["people"][0] == {"name": "bob", "agent": False}
    assert out["twice"] == 3
    assert out["after"] == [{"emoji": "like", "count": 2, "mine": False,
                             "people": [{"name": "carol"}, {"name": "dave"}]}]
    assert 'aria-pressed="false"' in out["bar"]
    assert 'aria-label="Like 2: carol and dave reacted with Like"' in out["bar"]


@needs_node
def test_bars_escape_and_merge_ledger_reactions():
    out = _run("""
state.commentReactions = { "pull:3": { reactions: {
  "x\\"y": [{ emoji: "like", count: 1, mine: true, people: [{ name: "<img src=x>", agent: true }] }] } } };
out.bar = renderCommentReactionBar("pull:3", "x\\"y", [{ value: "like", names: ["erin"] }, { value: "eyes", names: ["fay"] }], "px-4");
out.none = renderCommentReactionBar("", "c1");
out.subjects = [commentReactionSubject("issues", 3), commentReactionSubject("pulls", "4"),
  commentReactionSubject("discussions", 5), commentReactionSubject("wiki", 1), commentReactionSubject("issues", 0)];
""")
    bar = out["bar"]
    assert 'data-comment-id="x&quot;y"' in bar
    assert "<img src=x>" not in bar and "&lt;img src=x&gt; (agent)" in bar
    assert 'aria-pressed="true"' in bar and "fm-reaction-agent" in bar
    assert '<span class="fm-reaction-count">2</span>' in bar
    assert 'data-reaction="eyes"' in bar
    assert 'class="fm-reactions px-4"' in bar
    assert out["none"] == ""
    assert out["subjects"] == ["issue:3", "pull:4", "discussion:5", "", ""]


@needs_node
def test_a_seed_is_kept_only_for_the_record_it_was_rendered_for():
    out = _run("""
seedText = JSON.stringify({ base: "/api/repo/alice/app", subject: "issue:7", reactions: { body: [] } });
seedCommentReactions();
out.seeded = Object.keys(state.commentReactions);
prepareCommentReactions({ owner: "alice", name: "app" }, "issues", 7);
out.kept = Boolean(state.commentReactions["issue:7"]);
prepareCommentReactions({ owner: "bob", name: "app" }, "issues", 7);
out.dropped = Object.keys(state.commentReactions);
seedText = "{not json";
state.commentReactions = {};
seedCommentReactions();
out.bad = Object.keys(state.commentReactions);
""")
    assert out == {"seeded": ["issue:7"], "kept": True, "dropped": [], "bad": []}


@needs_node
def test_the_picker_offers_quick_reactions_and_searches_the_catalog():
    out = _run("""
const bar = { getAttribute: (name) => ({ "data-comment-reactions": "issue:1", "data-comment-id": "c" })[name] };
state.commentReactions = { "issue:1": { reactions: { c: [{ emoji: "rocket", count: 1, mine: true, people: [] }] } } };
const full = renderCommentReactionPicker(bar, "");
out.options = [...full.matchAll(/data-reaction-option="(\\w+)"/g)].map((m) => m[1]);
out.pressed = [...full.matchAll(/data-reaction-option="(\\w+)" aria-pressed="true"/g)].map((m) => m[1]);
out.party = [...renderCommentReactionPicker(bar, "party").matchAll(/data-reaction-option="(\\w+)"/g)].map((m) => m[1]);
out.thumbs = [...renderCommentReactionPicker(bar, "THUMBS").matchAll(/data-reaction-option="(\\w+)"/g)].map((m) => m[1]);
out.empty = renderCommentReactionPicker(bar, "<zzz>");
out.errors = ["rate_limited", "too_many_reactions", "daily_limit", "boom"].map((code) => commentReactionErrorText(new Error(code)));
""")
    assert out["options"][:6] == ["like", "love", "laugh", "celebrate", "eyes", "rocket"]
    assert sorted(out["options"]) == sorted(set(out["options"])) and len(out["options"]) == 24
    assert out["options"][6:13] == ["thanks", "hearthands", "sparkleheart", "handshake",
                                    "glowstar", "trophy", "thankyou"]
    assert out["pressed"] == ["rocket"]
    assert out["party"] == ["celebrate"]
    assert out["thumbs"] == ["like", "dislike"]
    assert "&lt;zzz&gt;" in out["empty"] and "<zzz>" not in out["empty"]
    assert len(set(out["errors"])) == 4


@needs_node
def test_notifications_name_the_comment_author_from_the_loaded_record():
    out = _run("""
state.repoRecordDetail = { parsed: { values: { authorName: "alice" },
  issueEvents: [{ id: "c1", authorName: "carol" }],
  pullConversation: [{ id: "r1", authorName: "dave" }] } };
out.names = [commentReactionAuthor("issue:1", "body"), commentReactionAuthor("issue:1", "c1"),
  commentReactionAuthor("pull:1", "r1"), commentReactionAuthor("pull:1", "zz")];
""")
    assert out["names"] == ["alice", "carol", "dave", ""]


def test_fragment_is_bundled_and_booted():
    assert "09-comment-reactions.js" in dashboard_bundle.FRAGMENTS
    assert "09-comment-reactions.js" not in dashboard_bundle.LAZY_CHUNKS.values()
    assert "initCommentReactions();" in BOOT
    bundle = (ROOT / "public" / "dashboard.js").read_text("utf-8")
    assert "function initCommentReactions()" in bundle
    assert "function renderCommentReactionBar(" in bundle


def test_every_comment_surface_draws_a_bar():
    assert 'renderCommentReactionBar(commentReactionSubject(kind, number), "body", null, "px-4 pb-3")' in CONTENT
    assert '${commentReactionEventBar(ev, "px-4 pb-3")}' in CONTENT
    assert '${commentReactionEventBar(event, "mt-2")}' in CONTENT
    assert 'renderCommentReactionBar(commentReactionSubject("issues", number), "body"' in CONTENT
    events = _const(CONTENT, "COMMENT_REACTION_EVENT_TYPES")
    for kind in ("comment", "review", "line-comment", "thread-comment", "thread-reply"):
        assert '"%s"' % kind in events
    bar = _function(CONTENT, "commentReactionEventBar")
    assert "ev.draft" in bar
    load = _function(CONTENT, "loadRepoRecordDetail")
    assert "prepareCommentReactions(repo, kind, number);" in load
    assert "loadCommentReactions(repo, kind, number);" in load


def test_writes_carry_the_session_and_one_read_serves_the_page():
    toggle = _function(FRAGMENT, "toggleCommentReaction")
    assert "`${repoApiBase(repo)}/reactions`" in toggle
    assert 'method: "POST"' in toggle
    for field in ("kind: parts.kind", "number: parts.number", "comment,", "emoji,", "on,",
                  "sessionToken: state.session?.sessionToken"):
        assert field in toggle
    assert 'showCommentReactionNotice(bar, "signin")' in toggle
    assert "commentReactionUi.inflight" in toggle
    load = _function(FRAGMENT, "loadCommentReactions")
    assert "/reactions?kind=${parts.kind}&number=${parts.number}" in load
    assert "commentReactionUi.loads" in load


def test_motion_is_optional_and_celebrate_bursts_once():
    celebrate = _function(FRAGMENT, "celebrateCommentReaction")
    assert celebrate.index("if (!commentReactionMotionOk()) return;") < celebrate.index("classList.add")
    assert 'emoji !== "celebrate" || commentReactionUi.confetti.has(once)' in celebrate
    assert "prefers-reduced-motion: reduce" in _function(FRAGMENT, "commentReactionMotionOk")
    css = SHELL[SHELL.index(":where(.fm-reactions)"):]
    reduced = css[css.index("@media (prefers-reduced-motion: reduce)"):]
    reduced = reduced[:reduced.index("\n      }\n")]
    assert ".fm-reaction-pop { animation: none; }" in reduced
    assert ".fm-reaction-burst, .fm-reaction-confetti { display: none; }" in reduced


def test_long_press_hover_and_keyboard_paths_exist():
    init = _function(FRAGMENT, "initCommentReactions")
    assert "COMMENT_REACTION_LONG_PRESS_MS" in init and "suppressClick" in init
    assert '"pointerover"' in init and '"focusin"' in init and '"contextmenu"' in init
    keys = _function(FRAGMENT, "commentReactionPickerKey")
    for key in ("Escape", "ArrowRight", "ArrowLeft", "ArrowDown", "ArrowUp", "Home", "End", "Enter"):
        assert key in keys
    picker = _function(FRAGMENT, "openCommentReactionPicker")
    assert 'setAttribute("role", "dialog")' in picker
    assert 'button.setAttribute("aria-expanded", "true")' in picker


def test_selection_uses_the_accent_never_green():
    css = SHELL[SHELL.index(":where(.fm-reactions)"):SHELL.index("@media (prefers-reduced-motion: reduce) {\n        .fm-reaction,")]
    pressed = re.search(r'\.fm-reaction\[aria-pressed="true"\] \{([^}]*)\}', css).group(1)
    assert "--dashboard-accent-rgb" in pressed and "success" not in pressed and "cta" not in pressed
    assert "--dashboard-ring-rgb" in css

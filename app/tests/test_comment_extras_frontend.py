#!/usr/bin/env python3
"""Front-end contracts for gratitude reactions, custom reactions, animated
chips, GIFs, stickers and tips (dashboard fragments, shell CSS, first view)."""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
CONTENT = (JS_DIR / "06-repo-content.js").read_text("utf-8")
REACTIONS = (JS_DIR / "09-comment-reactions.js").read_text("utf-8")
GIFS = (JS_DIR / "09-comment-gifs.js").read_text("utf-8")
TIPS = (JS_DIR / "09-comment-tips.js").read_text("utf-8")
SETTINGS = (JS_DIR / "09-reaction-settings.js").read_text("utf-8")
HELPERS = (JS_DIR / "02-helpers.js").read_text("utf-8")
BOOT = (JS_DIR / "10-boot.js").read_text("utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text("utf-8")
STICKER = ROOT / "public" / "assets" / "reactions" / "thank-you.svg"
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))
import custom_reactions  # noqa: E402
import dashboard_bundle  # noqa: E402
from forkmesh_host import repo_ssr_issue  # noqa: E402


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


def _run(body, tip_author="", motion=True):
    code = "\n".join([
        _const(CONTENT, name) for name in (
            "ISSUE_REACTION_EMOJI", "COMMENT_REACTIONS", "COMMENT_REACTION_QUICK",
            "COMMENT_REACTION_KINDS", "COMMENT_REACTION_GRATITUDE",
            "COMMENT_REACTION_STICKERS", "COMMENT_REACTION_CUSTOM_RE", "MD_GIPHY_GIF",
            "MD_DATA_IMAGE_URL", "MD_VIDEO_URL", "MD_VIDEO_CLASS", "MD_GITHUB_ATTACHMENT_URL",
            "MD_ATTACHMENT_PATH", "MD_ATTACHMENT_ENTITIES", "repoAttachmentFiles")
    ] + [_function(HELPERS, "escapeHtml"), _function(HELPERS, "formatCount"),
         _function(HELPERS, "repoKey")]
        + [_function(CONTENT, name) for name in (
            "commentReactionSubject", "commentReactionLabel", "commentReactionKnown",
            "commentReactionSafeSrc", "commentReactionGlyph", "commentReactionTallies",
            "commentReactionWho", "renderCommentReactionChip", "renderCommentReactionExtras",
            "renderCommentReactionBar", "mdSafeUrl", "mdImageSource", "mdGifHtml",
            "mdAttachmentKey", "mdAttachmentFile", "mdAttachmentHref", "mdGithubImageHtml",
            "mdImageHtml")]
        + [_function(REACTIONS, name) for name in (
            "commentReactionOptionHtml", "commentReactionSectionHtml",
            "renderCommentReactionPicker", "commentReactionApplyLocal",
            "commentReactionBase")]
        + [_function(GIFS, name) for name in ("gifMarkdown", "stickerMarkdown",
                                               "gifPreviewHtml", "commentStickers")])
    script = """
let countFormatter = null;
const state = { session: { nodeName: "bob", sessionToken: "cookie" } };
function commentTipAuthor() { return %s; }
function commentReactionMotionOk() { return %s; }
function repoApiBase(repo) { return `/api/repo/${repo.owner}/${repo.name}`; }
%s
const out = {};
%s
process.stdout.write(JSON.stringify(out));
""" % (json.dumps(tip_author), "true" if motion else "false", code, body)
    result = subprocess.run(["node", "-"], input=script, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


SRC = "/api/reactions/custom/asset/%s.svg" % ("a" * 40)


@needs_node
def test_bars_draw_stickers_custom_images_and_fallbacks_like_the_first_view():
    stored = {"c1": [
        {"emoji": "thankyou", "count": 2, "mine": False, "people": [{"name": "dave"}]},
        {"emoji": "custom:wasp", "count": 1, "mine": False, "people": [{"name": "erin"}],
         "custom": {"label": "Wasp", "src": SRC, "animatedSrc": ""}},
        {"emoji": "custom:gone", "count": 1, "mine": False, "people": [{"name": "fay"}], "custom": None},
        {"emoji": "custom:evil", "count": 1, "mine": False, "people": [{"name": "gus"}],
         "custom": {"label": "Evil", "src": 'https://evil.example/x.svg" onerror="x', "animatedSrc": ""}},
        {"emoji": "not-a-slug", "count": 1, "mine": False, "people": []},
    ]}
    out = _run("""
state.commentReactions = { "issue:7": { reactions: %s } };
out.bar = renderCommentReactionBar("issue:7", "c1");
""" % json.dumps(stored))
    python = repo_ssr_issue.reaction_bar("issue:7", "c1", stored["c1"]).replace(
        repo_ssr_issue.icon("smile-plus"), '<i data-lucide="smile-plus" class="h-4 w-4"></i>')
    assert out["bar"] == python
    bar = out["bar"]
    assert '<img src="/assets/reactions/thank-you.svg"' in bar
    assert '<img src="%s"' % SRC in bar and 'aria-label="Wasp 1: erin reacted with Wasp"' in bar
    assert '<span class="fm-reaction-fallback">:gone:</span>' in bar
    assert "evil.example" not in bar and ":evil:" in bar
    assert "not-a-slug" not in bar
    assert bar.count("data-reaction-thanks") == 1 and "Say thanks" in bar


@needs_node
def test_say_thanks_disappears_once_given_and_tips_show_for_wallets():
    out = _run("""
state.commentReactions = { "issue:7": { reactions: { c1: [{ emoji: "thankyou", count: 1, mine: true, people: [] }] } } };
state.commentTips = { "issue:7": { c1: 3 } };
out.bar = renderCommentReactionBar("issue:7", "c1");
""", tip_author="alice")
    bar = out["bar"]
    assert "data-reaction-thanks" not in bar
    assert 'data-comment-tip="alice"' in bar and 'aria-label="Tip @alice in Solana"' in bar
    assert "data-comment-tipped" in bar and '<span class="fm-reaction-count">3</span>' in bar
    plain = _run('out.bar = renderCommentReactionBar("issue:7", "c1");')["bar"]
    assert "data-comment-tip" not in plain and "data-comment-tipped" not in plain


@needs_node
def test_the_picker_groups_gratitude_and_merges_custom_reactions():
    out = _run("""
state.customReactions = { owner: "acme", loaded: true, items: {
  "custom:wasp": { emoji: "custom:wasp", slug: "wasp", label: "Wasp", src: "%s", animatedSrc: "" } } };
const bar = { getAttribute: (name) => ({ "data-comment-reactions": "issue:1", "data-comment-id": "c" })[name] };
state.commentReactions = { "issue:1": { reactions: { c: [] } } };
const full = renderCommentReactionPicker(bar, "");
out.sections = [...full.matchAll(/fm-reaction-section" role="presentation">([^<]+)</g)].map((m) => m[1]);
out.options = [...full.matchAll(/data-reaction-option="([^"]+)"/g)].map((m) => m[1]);
out.thanks = [...renderCommentReactionPicker(bar, "thanks").matchAll(/data-reaction-option="([^"]+)"/g)].map((m) => m[1]);
out.wasp = [...renderCommentReactionPicker(bar, "WASP").matchAll(/data-reaction-option="([^"]+)"/g)].map((m) => m[1]);
commentReactionApplyLocal("issue:1", "c", "custom:wasp", true);
out.local = state.commentReactions["issue:1"].reactions.c;
""" % SRC)
    assert out["sections"] == ["Gratitude", "This community", "More"]
    assert out["options"][6:13] == ["thanks", "hearthands", "sparkleheart", "handshake",
                                    "glowstar", "trophy", "thankyou"]
    assert out["options"][13] == "custom:wasp" and len(out["options"]) == 25
    assert out["thanks"][:7] == ["thanks", "hearthands", "sparkleheart", "handshake",
                                 "glowstar", "trophy", "thankyou"]
    assert out["wasp"] == ["custom:wasp"]
    assert out["local"][0]["custom"] == {"label": "Wasp", "src": SRC, "animatedSrc": ""}


@needs_node
def test_giphy_gifs_render_with_a_still_frame_on_both_sides_and_other_images_do_not():
    gif = "https://media.giphy.com/media/abc123XYZ/200.gif"
    out = _run("""
out.gif = mdImageHtml("Thank you", "%s", null);
out.other = mdImageHtml("x", "https://media.giphy.com.evil.example/media/abc/200.gif", null);
out.sized = mdImageHtml("x", "%s#w=50", null);
out.markdown = gifMarkdown({ title: "Thanks [so] much", url: "%s" });
out.sticker = stickerMarkdown({ label: "Thank you", src: "/assets/reactions/thank-you.svg" });
out.stickers = commentStickers();
out.preview = gifPreviewHtml({ id: "abc", title: "t", preview: "P.gif", still: "S.gif", width: 200, height: 100 });
""" % (gif, gif, gif))
    assert out["gif"] == repo_ssr_issue.image_html("Thank you", gif, None)
    assert 'srcset="https://media.giphy.com/media/abc123XYZ/200_s.gif" media="(prefers-reduced-motion: reduce)"' in out["gif"]
    assert 'loading="lazy"' in out["gif"] and 'referrerpolicy="no-referrer"' in out["gif"]
    assert 'role="button" tabindex="0"' in out["gif"]
    assert "data-fm-gif" not in out["other"] and "data-fm-gif" not in out["sized"]
    assert out["markdown"] == "![Thanks  so  much](%s)" % gif
    assert out["sticker"] == "![Thank you](/assets/reactions/thank-you.svg#w=96)"
    assert out["stickers"] == [{"id": "thankyou", "label": "Thank you", "src": "/assets/reactions/thank-you.svg"}]
    assert 'src="P.gif"' in out["preview"]
    still = _run('out.preview = gifPreviewHtml({ id: "abc", title: "t", preview: "P.gif", still: "S.gif" });',
                 motion=False)
    assert 'src="S.gif"' in still["preview"]


def test_first_view_renders_giphy_gifs_too():
    from forkmesh_host import repo_ssr
    html = repo_ssr.markdown_html("![Party](https://media2.giphy.com/media/abc123/giphy.gif)", "/alice/app")
    assert "data-fm-gif" in html and "giphy_s.gif" in html


def test_motion_is_off_under_reduced_motion():
    no_pref = re.findall(r"@media \(prefers-reduced-motion: no-preference\) \{(.*?)\n      \}", SHELL, re.S)
    assert no_pref, "hover loops live in a no-preference block"
    loops = "".join(no_pref)
    for name in ("fm-reaction-bob", "fm-reaction-beat", "fm-reaction-wave", "fm-reaction-twinkle",
                 "fm-reaction-bounce", "fm-reaction-wobble"):
        assert "animation: %s" % name in loops
        outside = SHELL.replace(loops, "")
        assert "animation: %s" % name not in outside
    reduced = re.search(r"@media \(prefers-reduced-motion: reduce\) \{(.*?)\n      \}", SHELL, re.S).group(1)
    assert "animation: none !important" in reduced and ".fm-reaction-thanks" in reduced
    assert "commentReactionMotionOk()" in _function(REACTIONS, "commentReactionAnimate")
    assert "commentReactionMotionOk()" in _function(GIFS, "gifPreviewHtml")
    assert "commentReactionMotionOk()" in _function(GIFS, "commentGifPlaying")


def test_fragments_are_bundled_and_booted():
    for name in ("09-comment-gifs.js", "09-comment-tips.js", "09-reaction-settings.js"):
        assert name in dashboard_bundle.FRAGMENTS
    for call in ("initCommentGifs();", "initCommentTips();", "void initReactionSettings();"):
        assert call in BOOT
    bundle = (ROOT / "public" / "dashboard.js").read_text("utf-8")
    for name in ("function openGifPicker(", "function openCommentTipSheet(",
                 "function initReactionSettings(", "function sayCommentThanks("):
        assert name in bundle


def test_gif_searches_and_tip_requests_go_through_this_server():
    assert "/api/gifs/search?q=" in GIFS and "/api/gifs/trending" in GIFS
    assert "api.giphy.com" not in GIFS and "api_key" not in GIFS
    assert "Powered by GIPHY" in GIFS
    assert "MD_GIPHY_GIF.test" in _function(GIFS, "loadGifPage")
    assert "loadQrGenerator()" in TIPS and "ForkMeshQR.toCanvas" in TIPS
    assert "/tips`" in TIPS and "/api/tips/verify" in TIPS
    assert 'setAttribute("role", "dialog")' in GIFS and 'setAttribute("aria-modal", "true")' in TIPS
    assert "never holds funds" in TIPS


def test_the_csp_allows_giphy_media_on_pages_that_render_comments():
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    runtime = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    csp = re.search(r'_BROWSER_CSP = \((.*?)\)\n', entry, re.S).group(1)
    assert re.search(r"img-src 'self' data: blob: https:;", csp)
    assert "img-src 'self' data: blob: https:;" in runtime


def test_the_thank_you_sticker_is_small_drawing_only_and_original():
    data = STICKER.read_bytes()
    assert len(data) < 8 * 1024
    text = data.decode("utf-8")
    assert "<title>Thank you</title>" in text and "THANK" in text and "YOU" in text
    assert "#e8437b" in text and "#79c3f2" in text
    for forbidden in ("<script", "href=", "<image", "<foreignObject", "data:"):
        assert forbidden not in text
    clean = custom_reactions.sanitize_svg(text)
    assert clean.count("<text") == text.count("<text")


def test_upstream_code_names_no_instance():
    for path in [JS_DIR / name for name in ("09-comment-gifs.js", "09-comment-tips.js",
                                             "09-reaction-settings.js", "09-comment-reactions.js")] + [
            ROOT / "src" / name for name in ("custom_reactions.py", "comment_gifs.py",
                                              "comment_tips.py")] + [STICKER]:
        text = path.read_text("utf-8")
        for word in ("OWASP", "BLT", "BACON", "wasp.svg", "mntjJ"):
            assert word not in text, (path.name, word)


def test_settings_page_has_the_section_for_admins_and_owners():
    view = (ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html").read_text("utf-8")
    assert 'data-settings-section="reactions"' in view
    assert 'data-settings-section-link="reactions" data-reaction-settings-link hidden' in view
    assert '"reactions"]' in (JS_DIR / "04-account.js").read_text("utf-8")
    assert "/api/reactions/custom/manage" in SETTINGS
    assert 'type="password"' in view and "data-gif-settings-key" in view


QR = ROOT.parent / "world" / "public" / "qr.js"
# sha256 of the module rows ("0"/"1", one line per row, no quiet zone) that an
# independent encoder (python-qrcode, byte mode, level L, fixed mask) draws.
QR_REFERENCE = [
    ("solana:6sbzC1eH4FTujJXWj51eQe25cYvr4xfXbJ1vAj7j2k5J?amount=12.5&spl-token="
     "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v&reference="
     "7aXo4rY3rZ8PcUyM5GzXK3Jp1qyWN2hS8vDqJe1v1Fmn", 8, 3,
     "bff05981558d6c8012daf596f6ecbf8743b1d33440aa1110a30f9d2102609595"),
    ("hello", 1, 0, "d9425dfc02fbbe47f123cf0ca192905819d46b3bfcfe88341ea76c41267db2f3"),
    ("u" * 150, 7, 5, "f6331fc9fd644d17ee4a40cb94868f93c8636a74b1e0e62349f1f49961bef340"),
]


@needs_node
def test_the_shared_qr_generator_fits_a_token_tip_request_and_matches_a_reference():
    import hashlib
    script = """
require(%s);
const Q = globalThis.ForkMeshQR._test;
const cases = %s;
const out = cases.map(([text, version, mask]) => {
  const base = Q.makeMatrix(version, Q.buildCodewords(Array.from(new TextEncoder().encode(text)), version));
  const m = base.m.map((row) => row.slice());
  for (let r = 0; r < base.size; r++) for (let c = 0; c < base.size; c++)
    if (!base.reserved[r][c] && Q.MASKS[mask](r, c)) m[r][c] = !m[r][c];
  Q.applyFormat(m, base.size, mask);
  return m.map((row) => row.map((v) => (v ? "1" : "0")).join("")).join("\\n");
});
out.push(Q.chooseVersion(189), Q.versionBits(7), globalThis.ForkMeshQR.generate("x".repeat(230)).size);
let tooLong = "";
try { globalThis.ForkMeshQR.generate("x".repeat(231)); } catch (error) { tooLong = error.message; }
out.push(tooLong);
process.stdout.write(JSON.stringify(out));
""" % (json.dumps(str(QR)), json.dumps([case[:3] for case in QR_REFERENCE]))
    result = subprocess.run(["node", "-"], input=script, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr[-2000:]
    out = json.loads(result.stdout)
    for case, rows in zip(QR_REFERENCE, out):
        assert hashlib.sha256(rows.encode()).hexdigest() == case[3], case[1:3]
    assert out[3] == 8 and out[4] == 0x07C94 and out[5] == 17 + 4 * 9
    assert "too long" in out[6]
    assert "commentTipQrText(data.uri)" in TIPS


def test_tip_buttons_wait_for_the_record_authors():
    detail = _function(CONTENT, "loadRepoRecordDetail")
    assert "loadCommentTipWallets(commentReactionSubject(kind, number))" in detail
    assert "loadCommentTipWallets(commentReactionSubject(kind, number))" in _function(
        CONTENT, "showOptimisticRepoRecord")

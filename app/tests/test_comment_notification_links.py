import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
EXPLORER = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
REACTIONS = (JS / "09-comment-reactions.js").read_text(encoding="utf-8")
BOOT = (JS / "10-boot.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
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


def _hrefs(items):
    script = _function(EXPLORER, "homeFeedHref") + "\nconsole.log(JSON.stringify(%s.map(homeFeedHref)));" % (
        json.dumps(items))
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def test_comment_notifications_open_the_comment_in_its_thread():
    root = "/acme/app"
    assert _hrefs([
        {"kind": "mention", "href": root, "meta": {"number": 4, "source": "pull", "comment": "c-1"}},
        {"kind": "subscribed", "href": root + "/", "meta": {"number": 7, "source": "discussion", "comment": "a b"}},
        {"kind": "subscribed", "href": root, "meta": {"number": 9, "source": "issue"}},
        {"kind": "mention", "href": root, "meta": {"number": 2}},
        {"kind": "mention", "href": root, "meta": {}},
    ]) == [
        "/acme/app/pull/4#comment-c-1",
        "/acme/app/discussions/7#comment-a%20b",
        "/acme/app/issues/9",
        "/acme/app/issues/2",
        "/acme/app",
    ]


def test_the_feed_detail_and_anchor_reveal_are_wired():
    assert "repoPublicHref(homeFeedHref(item))" in NETWORK
    assert "initCommentAnchor();" in BOOT
    reveal = _function(REACTIONS, "revealAnchoredComment")
    assert "CSS.escape(id)" in reveal
    assert "observer?.disconnect()" in reveal


def test_comment_ids_reach_the_notification_meta():
    assert 'number=0, comment=""):' in ENTRY
    assert 'body, href, skip=(), comment=""):' in ENTRY
    assert ENTRY.count('{"comment": clean_string(str(comment), 80)} if comment else {}') == 2
    assert ENTRY.count('comment=str(event.get("id", "") or "")') >= 3

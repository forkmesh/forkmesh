from __future__ import annotations

import asyncio
import json

from hidden_unicode import clean_value
from test_event_repo_binding import EV, _Signer

LS, PS = chr(0x2028), chr(0x2029)
VS16, ZWSP, RLO, ZWJ = chr(0xFE0F), chr(0x200B), chr(0x202E), chr(0x200D)
TAG_A = chr(0xE0041)
CODE = (
    'assert "' + LS + '" not in CONTENT and "' + PS + '" not in CONTENT\n'
    "label = '✅ done' + ' " + VS16 + "' + '❤" + VS16 + "'\n"
    "family = '\U0001F468" + ZWJ + "\U0001F469" + ZWJ + "\U0001F467'\n"
    "hidden = '" + ZWSP + RLO + TAG_A + "'\n"
)
PATCH = (
    "diff --git a/t.py b/t.py\n--- a/t.py\n+++ b/t.py\n@@ -0,0 +1,4 @@\n"
    + "".join("+" + line + "\n" for line in CODE.split("\n")
              if line)
)


class _TextRequest:
    url = "https://forkmesh.com/api/repo/forkmesh/forkmesh/pulls"
    headers = {}
    body = None

    def __init__(self, text):
        self._text = text

    async def text(self):
        return self._text


def _signed_pull(signer, repo="forkmesh/forkmesh"):
    import hashlib

    title = "Pull requests: quick review replies"
    base = "main"
    head = "git.owaspblt.org/owasp-blt/forkmesh-blt:fm53/quick-review-replies"
    commits = "From abc Mon Sep 17 00:00:00 2001\nSubject: test\n\n" + PATCH
    ts = 1790000000
    content = "\x00".join([title, base, head, PATCH, commits])
    digest = hashlib.sha256(content.encode()).hexdigest()
    sig = signer.sign(
        "forkmesh-pull-event-v2\n" + repo + "\n"
        + signer.pub + "\n" + str(ts) + "\n" + digest)
    return {"title": title, "description": "", "base": base, "head": head,
            "patch": PATCH, "commits": commits, "author": signer.pub,
            "authorName": "", "ts": ts, "sig": sig}


def _bounded(payload):
    import request_bodies

    request_bodies.RequestBodyTooLarge = type("TooLarge", (BaseException,), {})
    request_bodies.urlparse = __import__("urllib.parse").parse.urlparse
    request_bodies.json = json
    return asyncio.run(request_bodies.bounded_json_request(
        _TextRequest(json.dumps(payload, ensure_ascii=False))))


def test_signed_pull_with_separators_and_emoji_still_verifies():
    signer = _Signer()
    pull = _signed_pull(signer)
    data = _bounded({"owner": "forkmesh", "repo": "forkmesh", "pull": pull})
    received = data["pull"]
    assert received["patch"] == PATCH
    assert received["commits"] == pull["commits"]
    assert LS in received["patch"] and PS in received["commits"]
    assert asyncio.run(EV["verify_pull_event"](
        received, ("forkmesh/forkmesh",), False))


def test_signed_code_fields_stay_exact_but_prose_is_still_cleaned():
    body = {
        "title": "Fix" + ZWSP,
        "body": "a" + LS + "b",
        "patch": PATCH,
        "commits": PATCH,
        "suggestionPatch": PATCH,
        "anchorLines": CODE,
        "event": {"suggestionPatch": PATCH, "anchorLines": CODE,
                  "body": "x" + TAG_A},
    }
    assert clean_value(body) == {
        "title": "Fix",
        "body": "a\nb",
        "patch": PATCH,
        "commits": PATCH,
        "suggestionPatch": PATCH,
        "anchorLines": CODE,
        "event": {"suggestionPatch": PATCH, "anchorLines": CODE, "body": "x"},
    }

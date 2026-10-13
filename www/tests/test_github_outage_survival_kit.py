"""The GitHub outage survival kit (issue #522) stays postable as written.

The thread is copied verbatim into X, so every post must fit the 280-character
limit, and the video script must add up to exactly 60 seconds.
"""

import re
from pathlib import Path


KIT = Path(__file__).resolve().parents[1] / "docs" / "launch" / "github-outage-survival-kit.md"
X_POST_LIMIT = 280
X_LINK_LENGTH = 23
# X shortens anything it autolinks, bare domains included, to a 23-character link.
LINK = re.compile(r"(?:https?://)?(?:[\w-]+\.)+(?:com|org|io|dev)(?:[/:][^\s\"]*)?")


def _text():
    return KIT.read_text(encoding="utf-8")


def _thread_posts():
    thread = _text().split("<!-- outage-thread:start -->", 1)[1].split("<!-- outage-thread:end -->", 1)[0]
    return re.findall(r"```text\n(.*?)\n```", thread, re.S)


def _x_length(post):
    # Conservative: a link counts as whichever is longer, its text or the t.co length.
    return len(LINK.sub(lambda m: "x" * max(len(m.group(0)), X_LINK_LENGTH), post))


def test_every_thread_post_fits_an_x_post():
    posts = _thread_posts()
    assert 3 <= len(posts) <= 8
    for post in posts:
        assert _x_length(post) <= X_POST_LIMIT, (_x_length(post), post)


def test_thread_outlines_three_ways_and_links_the_bounty_terms():
    posts = _thread_posts()
    for number in ("1. ", "2. ", "3. "):
        assert sum(post.startswith(number) for post in posts) == 1, number
    assert "https://forkmesh.com/docs/launch/github-outage-survival-kit/" in posts[-1]
    assert "$5" in posts[-1]


def test_video_timeline_is_contiguous_and_sixty_seconds():
    spans = re.findall(r"^\| (\d):(\d\d)–(\d):(\d\d) \|", _text(), re.M)
    assert spans
    cursor = 0
    for start_m, start_s, end_m, end_s in spans:
        start = int(start_m) * 60 + int(start_s)
        end = int(end_m) * 60 + int(end_s)
        assert start == cursor and end > start
        cursor = end
    assert cursor == 60


def test_bounty_requires_disclosure_and_never_pays_for_engagement():
    text = _text()
    assert "discloses the payment" in text
    assert "We never pay for likes, reposts, follows, or replies" in text
    assert "merge-base --is-ancestor HEAD FETCH_HEAD" in text

import re
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PAGE = ROOT / "www" / "public" / "mirror.html"
BRIEF = ROOT / "docs" / "marketing" / "campaigns" / "repo-mirrored-2026-10-09.md"
CAMPAIGN = "repo_mirrored"
X_POST_LIMIT = 280
X_LINK_LENGTH = 23
LINK = re.compile(r"https?://\S+")


def _page():
    return PAGE.read_text(encoding="utf-8")


def _text():
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", _page()))


def _x_post():
    brief = BRIEF.read_text(encoding="utf-8")
    block = brief.split("<!-- repo-mirrored-x:start -->", 1)[1].split("<!-- repo-mirrored-x:end -->", 1)[0]
    return re.search(r"```text\n(.*?)\n```", block, re.S).group(1)


def test_page_is_canonical_and_sends_people_to_the_copy_form():
    page = _page()
    assert '<link rel="canonical" href="https://forkmesh.com/mirror" />' in page
    assert 'href="/signup"' in page
    assert 'href="https://app.forkmesh.com/dashboard/repos/new?source=remote"' in page
    assert "utm_" not in page


def test_page_keeps_every_limit():
    text = _text()
    for limit in (
        "It is a copy, not a live sync.",
        "Push to it to keep it current.",
        "Issues, pull requests, releases, wikis and LFS files stay where they are.",
        "Public sources only,",
        "200 MiB",
        "10 repositories per account",
        "Copying from a host that is down fails.",
        "not an independent network",
        "Remote clone URL",
    ):
        assert limit in text, limit


def test_page_never_promises_a_live_mirror():
    text = _text().lower()
    for claim in ("automatically", "stays in sync", "keeps in sync", "real-time", "never down", "free forever"):
        assert claim not in text, claim


def test_campaign_is_on_both_allowlists():
    config = tomllib.loads((ROOT / "app" / "wrangler.toml").read_text(encoding="utf-8"))
    assert CAMPAIGN in config["vars"]["ATTRIBUTION_ALLOWED_CAMPAIGNS"].split(",")
    source = (ROOT / "app" / "src" / "attribution.py").read_text(encoding="utf-8")
    defaults = re.search(r"^DEFAULT_ALLOWED_CAMPAIGNS = \((.*)\)$", source, re.M).group(1)
    assert f'"{CAMPAIGN}"' in defaults


def test_x_draft_fits_and_links_the_tagged_page():
    post = _x_post()
    assert post.startswith("Is your repo mirrored?")
    links = LINK.findall(post)
    assert links == [
        "https://forkmesh.com/mirror?utm_source=x&utm_medium=social"
        f"&utm_campaign={CAMPAIGN}&utm_content=is-your-repo-mirrored"
    ]
    assert len(LINK.sub("x" * X_LINK_LENGTH, post)) <= X_POST_LIMIT
    for claim in ("sync", "automatic", "up to date"):
        assert claim not in post.lower(), claim

"""The hackathon leaderboard, projects and rules pages.

They sit beside main's landing page at /hackathon and share its chrome, but
they do not load Tailwind, so the stylesheet they do load has to supply the
rules Tailwind's preflight would have.
"""

import re
from pathlib import Path

WWW = Path(__file__).resolve().parents[1] / "public"
HACKATHON_CSS = (WWW / "hackathon.css").read_text(encoding="utf-8")
LANDING = (WWW / "hackathon.html").read_text(encoding="utf-8")
SUBPAGES = ("hackathon/leaderboard.html", "hackathon/projects.html",
            "hackathon/rules.html")


def assert_one_sun_and_one_moon(page, html):
    toggles = []
    start = html.find("data-home-theme")
    while start >= 0:
        end = html.index("</button>", start)
        toggles.append(html[start:end])
        start = html.find("data-home-theme", end)
    assert toggles, page
    for markup in toggles:
        assert markup.count("home-theme-sun") == 1, page
        assert markup.count("home-theme-moon") == 1, page
        # The moon ships hidden so the page paints a single icon before the
        # script runs, with no flash of both.
        moon = markup.index("home-theme-moon")
        assert "hidden" in markup[moon:markup.index("</svg>", moon)], page


def test_subpages_enforce_the_hidden_attribute_without_tailwind():
    # The theme toggle icons, the milestone meter and the QR panel all hide
    # themselves with the attribute, which an <svg> otherwise ignores.
    assert "[hidden] { display: none !important; }" in HACKATHON_CSS
    for page in SUBPAGES:
        html = (WWW / page).read_text(encoding="utf-8")
        assert 'href="/tailwind.css"' not in html, page
        assert 'href="/hackathon.css"' in html, page


def test_each_subpage_toggle_carries_exactly_one_sun_and_one_moon():
    for page in SUBPAGES:
        assert_one_sun_and_one_moon(page, (WWW / page).read_text(encoding="utf-8"))


def test_subpage_navigation_reaches_the_landing_page_sections():
    # The section anchors live on /hackathon. On the subpages a bare #tracks
    # points at nothing, so every section link names the landing page.
    landing_ids = set(re.findall(r'id="([a-z-]+)"', LANDING))
    for page in SUBPAGES:
        html = (WWW / page).read_text(encoding="utf-8")
        own_ids = set(re.findall(r'id="([a-z-]+)"', html))
        for anchor in re.findall(r'href="#([a-z-]+)"', html):
            assert anchor in own_ids, (page, anchor)
        for anchor in re.findall(r'href="/hackathon#([a-z-]+)"', html):
            assert anchor in landing_ids, (page, anchor)

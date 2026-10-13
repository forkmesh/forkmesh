"""The theme toggle shows one icon, and it is the one you are switching to.

The control carries a sun and a moon and hides one of them with the `hidden`
attribute. Two things have to hold for that to work, and both of them broke:

* `hidden` is an IDL property of HTMLElement, and these are <svg> elements.
  Assigning `svg.hidden = true` sets a plain expando and never touches the
  attribute, so the icon that ships hidden in the markup stayed hidden in
  both themes and the other stayed visible.
* The attribute only hides anything if a stylesheet says so. The homepage
  gets that from tailwind.css's preflight, and the hackathon landing page
  from the shared pill navigation stylesheet.
"""

from pathlib import Path

WWW = Path(__file__).resolve().parents[1] / "public"
HOME_THEME = (WWW / "home-theme.js").read_text(encoding="utf-8")
SITE_HEADER = (WWW / "site-header.js").read_text(encoding="utf-8")
HOME_NAV_CSS = (WWW / "home-nav.css").read_text(encoding="utf-8")
LANDING = (WWW / "hackathon.html").read_text(encoding="utf-8")


def toggles(html):
    """The markup of each theme toggle button on a page."""
    out = []
    start = html.find("data-home-theme")
    while start >= 0:
        end = html.index("</button>", start)
        out.append(html[start:end])
        start = html.find("data-home-theme", end)
    return out


def assert_one_sun_and_one_moon(page, html):
    found = toggles(html)
    assert found, page
    for markup in found:
        assert markup.count("home-theme-sun") == 1, page
        assert markup.count("home-theme-moon") == 1, page
        # The moon ships hidden so the page paints a single icon before
        # the script runs, with no flash of both.
        moon = markup.index("home-theme-moon")
        assert "hidden" in markup[moon:markup.index("</svg>", moon)], page


def test_the_icon_swap_toggles_the_attribute_not_the_property():
    for name, source in (("home-theme.js", HOME_THEME),
                         ("site-header.js", SITE_HEADER)):
        assert 'sun?.toggleAttribute("hidden", light)' in source, name
        assert 'moon?.toggleAttribute("hidden", !light)' in source, name
        # The property assignment is what did nothing on an SVG.
        assert "sun.hidden =" not in source, name
        assert "moon.hidden =" not in source, name


def test_the_landing_page_enforces_the_hidden_attribute():
    assert 'href="/home-nav.css' in LANDING
    assert ".home-theme-toggle svg[hidden]" in HOME_NAV_CSS


def test_the_landing_toggle_carries_exactly_one_sun_and_one_moon():
    assert_one_sun_and_one_moon("hackathon.html", LANDING)

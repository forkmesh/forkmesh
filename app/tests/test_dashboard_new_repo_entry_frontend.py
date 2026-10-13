#!/usr/bin/env python3
"""The dashboard home rail's "New" control (issue #626).

The green New button above Top repositories looked actionable but was inert
markup: no data hook, no href, no listener. Clicking it produced no visible
response, so the repository-creation flow was undiscoverable from the home
page.

"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_shell  # noqa: E402

from _dashboard_bundle import assembled_dashboard_js  # noqa: E402

HOME_VIEW = PUBLIC / "dashboard" / "partials" / "views" / "home.html"
HOME_PAGE = PUBLIC / dashboard_shell.PAGES["home"]["asset"]
REPOS_VIEW = PUBLIC / "dashboard" / "partials" / "views" / "repos.html"

BUNDLE = assembled_dashboard_js()

NEW_CONTROL_RE = re.compile(
    r"<a\b[^>]*\bdata-home-new-repo\b[^>]*>(.*?)</a>", re.S)


def _read(path):
    return path.read_text(encoding="utf-8")


def _new_control(html):
    match = NEW_CONTROL_RE.search(html)
    assert match, "home rail has no data-home-new-repo control"
    return match.group(0), match.group(1)


def test_home_new_control_is_a_real_link_into_the_new_repository_flow():
    for html in (_read(HOME_VIEW), _read(HOME_PAGE)):
        tag, label = _new_control(html)
        assert label.strip().endswith("New")
        assert 'href="/dashboard/repos/new"' in tag
        # A title so the control says what it does before it is clicked.
        assert "title=" in tag


def test_home_rail_no_longer_ships_an_inert_button():
    # The regression this fixes: a <button> with no data hook and no listener.
    html = _read(HOME_VIEW)
    rail = html[html.index("Top repositories"):]
    rail = rail[:rail.index("</div>")]
    assert "<button" not in rail, "the rail control is a link, not a bare button"


def test_the_flow_is_not_hooked_to_a_modal_that_the_home_page_never_ships():
    # setNewRepoModalOpen returns early with no [data-new-repo-modal] in the
    # document, so a click handler here would be silently inert again.
    home = dashboard_shell.PAGES["home"]
    assert "new-repo-modal" not in home.get("extras", ())
    assert "data-new-repo-modal" not in _read(HOME_PAGE)


def test_new_repository_is_its_own_page_and_the_old_marker_redirects_there():
    assert 'href="/dashboard/repos/new"' in _read(REPOS_VIEW)
    page = dashboard_shell.PAGES["new-repo"]
    assert page["route"] == "/dashboard/repos/new"
    html = _read(PUBLIC / page["asset"])
    assert "data-new-repo-page" in html and "data-new-repo-form" in html
    assert "data-new-repo-modal" not in html
    start = BUNDLE.index("function consumeNewRepositoryRequest()")
    body = BUNDLE[start:start + 400]
    assert 'params.get("new") !== "1"' in body
    assert 'location.replace("/dashboard/repos/new")' in body
    init = BUNDLE.index("function initNewRepoPage()")
    assert "prepareNewRepoForm();" in BUNDLE[init:init + 200]
    assert '"new-repo": initNewRepoPage' in BUNDLE

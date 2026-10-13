#!/usr/bin/env python3
"""Contracts for the shared admin shell (app/src/admin_shell.py).

Every administrator page renders inside this chrome, so these tests pin what
all of them rely on: the section list, the script-free drawer and theme
switch, escaping, the redirect guard on /admin/theme, and the house style
(GitHub palettes, solid colours, no shadows).
"""

import asyncio
import importlib.util
import re
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
NOW = 1_789_560_000_000  # 2026-09-16T12:00:00Z


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class FakeRequest:
    def __init__(self, method="POST", body="", cookie=""):
        self.method = method
        self._body = body
        self.headers = {"cookie": cookie} if cookie else {}

    async def text(self):
        return self._body


def _load(d1_first=None):
    spec = importlib.util.spec_from_file_location(
        "admin_shell_under_test", SRC / "admin_shell.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    async def no_rows(env, sql, *args):
        return None

    module._bind_runtime({
        "Response": FakeResponse,
        "d1_first": d1_first or no_rows,
        "Date": SimpleNamespace(now=lambda: NOW),
    })
    return module


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_page_draws_every_section_with_the_active_one_marked():
    shell = _load()
    page = shell.render_page(
        "Errors", "<p>body</p>", active="errors", console="/ops-console",
        badges={"bugs": ("9", "bad")}, account="ada-lovelace",
        next_path="/ops-console?view=errors")

    assert page.startswith("<!doctype html>")
    assert page.count('<nav class="ab-nav"') == 1
    for label in ("Overview", "Product analytics", "Acquisition", "Hackathon",
                  "Users", "Bug reports", "Docs &amp; World feedback",
                  "Errors", "Install diagnostics", "Alert delivery",
                  "Controls", "Database"):
        assert "<span>%s</span>" % label in page, label
    for group in ("Growth", "People", "Support", "Reliability", "Platform"):
        assert '<div class="ab-group">%s</div>' % group in page
    body = page.split("</head>", 1)[1]
    assert body.count('aria-current="page"') == 1
    assert ('<a class="ab-item on" href="/ops-console?view=errors" '
            'aria-current="page">') in page
    assert 'href="/product-analysis"' in page
    assert 'href="/admin/acquisition"' in page
    assert 'href="/admin/hackathon"' in page
    assert '<span class="ab-count bad">9</span>' in page
    assert "/assets/logo.svg" in page
    assert 'title="Signed in as ada-lovelace"' in page
    assert '<meta name="robots" content="noindex, nofollow">' in page


def test_console_links_and_search_need_a_known_console_path():
    shell = _load()
    page = shell.render_page("Product analytics", "", active="analytics")

    assert "?view=" not in page
    assert 'role="search"' not in page
    assert "<span>Product analytics</span>" in page
    assert "<span>Users</span>" not in page


def test_allowed_limits_the_sections_a_page_may_show():
    shell = _load()
    page = shell.render_page("Product analytics", "", active="analytics",
                             console="/ops", allowed={"analytics"})

    assert "<span>Product analytics</span>" in page
    assert "<span>Hackathon</span>" not in page
    assert "/ops" not in page.replace('action="/admin/theme"', "")


def test_shell_is_script_free_with_a_css_drawer_and_a_form_theme_switch():
    shell = _load()
    page = shell.render_page("Overview", "", active="overview", console="/ops",
                             theme="dark", next_path="/ops?view=overview")

    assert "<script" not in page.lower()
    assert '<html lang="en" class="theme-dark">' in page
    # The checkbox is a sibling of the sidebar, scrim and main column, so the
    # general-sibling selectors that open the drawer can reach them.
    assert re.search(r'<div class="ab"><input type="checkbox" id="ab-nav"[^>]*>'
                     r'<aside class="ab-side"', page)
    assert "#ab-nav:checked~.ab-side{transform:none}" in shell.STYLE
    assert page.count('name="theme"') == 3
    assert '<form class="seg" method="post" action="/admin/theme"' in page
    assert '<input type="hidden" name="next" value="/ops?view=overview">' in page
    assert 'value="dark" title="Dark theme" aria-label="Dark theme" aria-pressed="true"' in page


def test_house_style_solid_github_palettes_without_shadows():
    shell = _load()
    style = shell.STYLE

    for banned in ("gradient", "box-shadow", "letter-spacing",
                   "text-transform"):
        assert banned not in style, banned
    assert "--bg:#ffffff" in style and "--fg:#1f2328" in style
    assert "--bg:#0d1117" in style and "--fg:#e6edf3" in style
    assert "@media (prefers-color-scheme:dark){html:not(.theme-light){" in style
    assert "prefers-reduced-motion" in style
    assert ":focus-visible" in style
    assert "tabular-nums" in style
    assert "@media (max-width:767px)" in style


def test_everything_a_page_passes_in_is_escaped():
    shell = _load()
    hostile = '<script>alert(1)</script>"'
    page = shell.render_page(hostile, "", console="/ops", account=hostile,
                             search_value=hostile, next_path="/x" + hostile)
    fragments = (
        shell.page_head(hostile, hostile, actions=""),
        shell.pill(hostile, "bad"),
        shell.who(hostile, hostile),
        shell.card("", title=hostile, description=hostile),
        shell.kpi(hostile, hostile, window=hostile),
        shell.banner(hostile, "bad"),
        shell.tabs([("a", hostile, "#", hostile)], "a"),
        shell.line_chart([hostile], [([1], "accent")], title=hostile),
    )

    for html in (page,) + fragments:
        assert "<script>alert" not in html
    assert "&lt;script&gt;alert(1)&lt;/script&gt;" in page


def test_theme_route_sets_the_cookie_and_returns_to_a_same_site_path():
    shell = _load()
    response = _run(shell.theme_handler(None, FakeRequest(
        body="theme=dark&next=%2Fops%3Fview%3Dusers%26q%3Dada")))

    assert response.status == 303
    assert response.headers["location"] == "/ops?view=users&q=ada"
    cookie = response.headers["set-cookie"]
    assert cookie.startswith("forkmesh_admin_theme=dark; Path=/;")
    assert "HttpOnly" in cookie and "Secure" in cookie

    cleared = _run(shell.theme_handler(None, FakeRequest(
        body="theme=system&next=/ops")))
    assert cleared.headers["set-cookie"].startswith(
        "forkmesh_admin_theme=; Path=/; Max-Age=0")


def test_theme_route_refuses_to_redirect_off_site():
    shell = _load()
    for target in ("https://evil.example/", "//evil.example", "/\\evil",
                   "javascript:alert(1)", "/ok\r\nset-cookie: x=1", ""):
        assert shell.safe_next(target) == "/", target
    assert shell.safe_next("/admin/hackathon?slug=a") == "/admin/hackathon?slug=a"

    response = _run(shell.theme_handler(None, FakeRequest(
        body="theme=light&next=https%3A%2F%2Fevil.example%2F")))
    assert response.headers["location"] == "/"

    bogus = _run(shell.theme_handler(None, FakeRequest(
        body="theme=neon&next=/ops")))
    assert bogus.status == 303 and "set-cookie" not in bogus.headers

    get = _run(shell.theme_handler(None, FakeRequest(method="GET")))
    assert get.status == 405


def test_theme_cookie_is_read_back_and_only_light_or_dark_pin():
    shell = _load()
    assert shell.theme_from_request(
        FakeRequest(cookie="a=1; forkmesh_admin_theme=light")) == "light"
    assert shell.theme_from_request(
        FakeRequest(cookie="forkmesh_admin_theme=<x>")) == "system"
    assert shell.theme_from_request(FakeRequest()) == "system"


def test_nav_badges_count_attention_and_survive_missing_tables():
    seen = []

    async def d1_first(env, sql, *args):
        seen.append(sql)
        if "bug_reports" in sql:
            return {"n": 9}
        if "agent_note_queue" in sql:
            return {"n": 2}
        if "error_log" in sql:
            assert args == (NOW - 86_400_000,)
            return {"n": 0}
        if "not_found_paths" in sql:
            assert args == (NOW - 86_400_000,)
            return {"n": 3}
        raise RuntimeError("no such table: hackathons")

    shell = _load(d1_first)
    badges = _run(shell.nav_badges(None))

    assert badges == {"bugs": ("9", "bad"), "agent-notes": ("2", "warn"),
                      "not-found": ("3", "warn")}
    assert len(seen) == 6


def test_line_chart_puts_every_mark_on_one_labelled_scale():
    shell = _load()
    svg = shell.line_chart(["Sep 1", "Sep 2", "Sep 3"],
                           [([1, 7, 3], "accent"), ([0, 2, 1], "second")])

    # 7 rounds up to a ceiling of 10, labelled on the top grid line.
    assert '<text class="g-axis" x="30" y="16.0" text-anchor="end">10</text>' in svg
    assert 'class="s-second"' in svg and 'stroke-dasharray="4 4"' in svg
    assert 'class="f-area"' in svg
    assert shell.line_chart([], []) == ""
    bars = shell.line_chart(["a", "b"], [([2, 4], "accent")], bars=True)
    assert bars.count("<rect") == 2


def test_time_labels_are_utc_and_relative_to_the_request():
    shell = _load()
    assert shell.utc_label(NOW) == "Sep 16, 2026 12:00 UTC"
    assert shell.utc_label(NOW // 1000) == "Sep 16, 2026 12:00 UTC"
    assert shell.relative_label(NOW - 3 * 3_600_000, NOW) == "3 hours ago"
    assert shell.relative_label(NOW - 61_000, NOW) == "1 minute ago"
    assert shell.relative_label(NOW, NOW) == "just now"
    assert shell.relative_label(0, NOW) == "never"
    assert 'datetime="2026-09-16T12:00:00Z"' in shell.time_html(NOW, NOW)



def test_theme_route_is_dispatched_before_the_repository_router():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    entry = (SRC / "entry.py").read_text(encoding="utf-8")

    theme = runtime.index('if url.path == "/admin/theme":')
    assert runtime.index('if url.path in ("/admin/hackathon"') < theme
    assert theme < runtime.index(
        "external_owner = external_site_route(url.path)", theme)
    assert '_admin_shell = _LazyModule("admin_shell")' in entry
    assert 'admin_theme_handler = _admin_shell.export("theme_handler")' in entry

"""deploy/core/caddy_routes.py: Workers glob semantics and _headers."""

import re
import tomllib
from pathlib import Path

import pytest

import caddy_routes

REPO = Path(__file__).resolve().parents[2]
APP_FIRST = tomllib.loads((REPO / "app/wrangler.toml").read_text())["assets"]["run_worker_first"]
WORLD_FIRST = tomllib.loads((REPO / "world/wrangler.toml").read_text())["assets"]["run_worker_first"]


@pytest.mark.parametrize("path, expected", [
    ("/", True), ("/api", True), ("/api/v1/x", True), ("/@alice", True),
    ("/%40alice", True), ("/alice/repo", True), ("/alice/repo/tree/main/a", True),
    ("/dashboard/repos", False), ("/dashboard/chat/", False), ("/dashboard/other", True),
    ("/dashboard/js/app.js", False), ("/assets/logo.png", False), ("/login/", False),
    ("/favicon/x.ico", False), ("/login.html", True), ("/chat.js", False),
    ("/dashboard-plugins/p/x.js", False), ("/.well-known/webfinger", True),
])
def test_app_worker_first_matches_workers_semantics(path, expected):
    assert caddy_routes.worker_first(APP_FIRST, path) is expected


def test_world_worker_first():
    assert caddy_routes.worker_first(WORLD_FIRST, "/assets/x.png") is True
    assert caddy_routes.worker_first(WORLD_FIRST, "/assets/world/x.png") is False
    assert caddy_routes.worker_first(WORLD_FIRST, "/world/world.js") is False


def test_generated_regexes_match_the_reference():
    snippet = caddy_routes.app_routes(APP_FIRST, "", "/srv/dist")
    positive = re.search(r"path_regexp app_wf_pos (\S+)", snippet).group(1)
    negative = re.search(r"path_regexp app_wf_neg (\S+)", snippet).group(1)
    for path in ("/", "/api/x", "/alice/repo", "/dashboard/repos", "/assets/a.png",
                 "/@bob", "/notes/1", "/login/"):
        caddy = bool(re.fullmatch(positive, path)) and not re.fullmatch(negative, path)
        assert caddy == caddy_routes.worker_first(APP_FIRST, path), path


def test_regex_escapes_literals():
    assert caddy_routes.glob_to_regex("/rss.xml") == r"/rss\.xml"
    assert caddy_routes.glob_to_regex("/%40*") == "/@.*"
    with pytest.raises(caddy_routes.RoutesError):
        caddy_routes.glob_to_regex("no-slash")
    with pytest.raises(caddy_routes.RoutesError):
        caddy_routes.glob_to_regex("/{bad}")


def test_headers_file_parsing_and_order():
    rules = caddy_routes.parse_headers_file(
        "# comment\n/*\n  X-A: 1\n  Cache-Control: max-age=0\n\n/chat\n"
        "  ! X-A\n  X-A: 2\n")
    assert rules == [("/*", [("set", "X-A", "1"), ("set", "Cache-Control", "max-age=0")]),
                     ("/chat", [("unset", "X-A", ""), ("set", "X-A", "2")])]
    matchers, directives = caddy_routes.header_block(rules, "app")
    assert matchers[0].startswith("@app_h0 path_regexp app_h0 ")
    assert directives == ['\theader @app_h0 X-A "1"',
                          '\theader @app_h0 Cache-Control "max-age=0"',
                          '\theader @app_h1 X-A "2"']
    _, only_unset = caddy_routes.header_block(
        caddy_routes.parse_headers_file("/w\n  ! X-Frame-Options\n"), "app")
    assert only_unset == ["\theader @app_h0 -X-Frame-Options"]


def test_headers_refuse_unsafe_input():
    with pytest.raises(caddy_routes.RoutesError):
        caddy_routes.parse_headers_file("/*\n  Bad Header: x\n")
    with pytest.raises(caddy_routes.RoutesError):
        caddy_routes.parse_headers_file("  X-A: before any path\n")


def test_placeholders_match_one_segment():
    regex = caddy_routes.alternation(["/:owner/:repo/issues/:number"])
    assert re.fullmatch(regex, "/alice/app/issues/12")
    assert not re.fullmatch(regex, "/alice/app/x/issues/12")
    assert not re.fullmatch(regex, "/alice/app/issues/")


def test_real_www_headers_file_converts():
    text = (REPO / "www/public/_headers").read_text()
    matchers, directives = caddy_routes.header_block(
        caddy_routes.parse_headers_file(text), "app")
    assert matchers and directives
    assert any("Content-Security-Policy" in d for d in directives)


def test_world_versioned_files_stay_immutable_after_headers():
    headers = "/world/*\n  Cache-Control: no-store, max-age=0, must-revalidate\n"
    routes = caddy_routes.world_routes(WORLD_FIRST, headers, "/srv/world",
                                       "/srv/app")
    block = routes[routes.index("handle @world_file {"):]
    block = block[:block.index("\n}\n")]
    lines = [line.strip() for line in block.splitlines()]
    immutable = 'header @world_versioned Cache-Control "public, max-age=31536000, immutable"'
    no_store = [index for index, line in enumerate(lines)
                if "no-store, max-age=0, must-revalidate" in line]
    assert no_store
    assert lines.index(immutable) < no_store[0]
    assert len(lines) - 1 - lines[::-1].index(immutable) > no_store[-1]
    app = caddy_routes.app_routes(APP_FIRST, headers, "/srv/app")
    app_block = app[app.index("handle @app_file {"):]
    app_block = app_block[:app_block.index("\n}\n")]
    assert app_block.count("_versioned Cache-Control") == 1

from __future__ import annotations

import asyncio
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from forkmesh_host import repo_ssr, repo_ssr_mirrors

ROOT = Path(__file__).resolve().parents[2]
JS = ROOT / "public" / "dashboard" / "js"
NOW_MS = 1792480000000
needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")

DATA = {
    "ok": True, "target": 10, "healthy": 3, "total": 6, "registered": 4, "max": 100,
    "maxTarget": 100, "cloneUrl": "https://forum.example.org/example/forum.git",
    "mirrors": [
        {"id": "self", "label": "This instance", "kind": "forkmesh-instance",
         "url": "https://forum.example.org/example/forum", "host": "forum.example.org",
         "builtin": True, "status": "green", "statusLabel": "Serving this page",
         "lastCheck": {"at": NOW_MS, "ok": True, "headCommit": "a" * 40}},
        {"id": "source", "label": "GitHub (original)", "kind": "github",
         "url": "https://github.com/example/forum", "host": "github.com", "builtin": True,
         "status": "green", "statusLabel": "Reachable",
         "lastCheck": {"at": NOW_MS - 5 * 60 * 1000, "ok": True}},
        {"id": "0123456789abcdef", "label": "Bob's <mirror>", "kind": "other",
         "url": "https://m.example.org/bbs.git", "host": "m.example.org", "builtin": False,
         "status": "yellow", "statusLabel": "Behind or different",
         "lastCheck": {"at": NOW_MS - 3 * 3600 * 1000, "ok": True}, "canRemove": True},
        {"id": "1123456789abcdef", "label": "codeberg.org", "kind": "codeberg",
         "url": "git@codeberg.org:example/forum.git", "host": "codeberg.org",
         "builtin": False, "status": "unknown", "statusLabel": "Not checked (no https address)",
         "lastCheck": None},
        {"id": "2123456789abcdef", "label": "down", "kind": "other",
         "url": "https://down.example.org/bbs\"onmouseover=x", "host": "down.example.org",
         "builtin": False, "status": "red", "statusLabel": "Unreachable",
         "lastCheck": {"at": NOW_MS - 2 * 86400 * 1000, "ok": False}},
        {"id": "3123456789abcdef", "label": "odd", "status": "purple", "lastCheck": {}},
    ],
}



@pytest.fixture(autouse=True)
def mirror_plugin_on(monkeypatch):
    from forkmesh_host import repo_ssr_issue
    monkeypatch.setattr(repo_ssr_issue, "plugins_off", lambda: frozenset())

def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    start = match.start()
    cursor = text.index("{", text.index(")", start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _const(text, name):
    start = text.index("  const %s =" % name)
    match = re.compile(r";\n").search(text, start)
    return text[start:match.end()]


def js_render(data, viewer=None, help_href=""):
    helpers = (JS / "02-helpers.js").read_text("utf-8")
    source = (JS / "09-mirror-quota.js").read_text("utf-8")
    code = "\n".join(
        [_function(helpers, name) for name in ("escapeHtml", "parseFlexibleDate", "formatTimeAgo")]
        + [_const(source, name) for name in ("MIRROR_QUOTA_DOT", "MIRROR_QUOTA_WORD",
                                              "MIRROR_QUOTA_OVERALL_WORD")]
        + [_function(source, name) for name in ("mirrorQuotaOverall", "mirrorQuotaRowHtml",
                                                "renderRepoMirrorQuotaCard")])
    script = ("Date.now = () => %d;\n%s\nprocess.stdout.write(renderRepoMirrorQuotaCard(%s, %s, %s));"
              % (NOW_MS, code, json.dumps(data), json.dumps(viewer), json.dumps(help_href)))
    return subprocess.run(["node", "-"], input=script, check=True, capture_output=True,
                          text=True, env={"TZ": "UTC", "PATH": "/usr/bin:/bin:" + str(
                              Path(shutil.which("node")).parent)}).stdout


def py_render(data, viewer=None, help_href=""):
    return repo_ssr_mirrors.card_html(data, viewer, NOW_MS / 1000, help_href)


@needs_node
@pytest.mark.parametrize("viewer", [None, {"canSetTarget": True}])
def test_the_card_matches_the_dashboard_render(viewer):
    assert py_render(DATA, viewer) == js_render(DATA, viewer)


@needs_node
@pytest.mark.parametrize("change", [
    {"target": 0},
    {"target": 0, "registered": 0},
    {"target": 3, "healthy": 5},
    {"target": 7, "healthy": 1},
    {"mirrors": []},
])
def test_variants_match_the_dashboard_render(change):
    data = dict(DATA, **change)
    for viewer in (None, {"canSetTarget": True}):
        assert py_render(data, viewer) == js_render(data, viewer)


def test_the_card_is_hidden_until_there_is_something_to_show():
    empty = dict(DATA, target=0, registered=0)
    assert repo_ssr_mirrors.card_html(empty) == ""
    assert 'data-mirror-quota-target-form' in repo_ssr_mirrors.card_html(
        empty, {"canSetTarget": True})
    assert "Set a mirror goal" in repo_ssr_mirrors.card_html(empty, {"canSetTarget": True})
    assert repo_ssr_mirrors.section_html(None).endswith(" hidden></section>")


def test_the_card_draws_status_progress_and_escapes():
    html = py_render(DATA)
    assert "3 / 10 healthy" in html and "width:30%" in html
    assert 'aria-label="Behind"' in html and "bg-attention" in html
    assert 'aria-label="Unreachable"' in html and "bg-destructive" in html
    assert "Bob&#039;s &lt;mirror&gt;" in html
    assert "onmouseover" not in html.split('href="')[-1].split('"')[0]
    assert 'href="https://down.example.org' not in html
    assert "checked 3 hours ago" in html and "checked 5 minutes ago" in html
    assert html.count("data-mirror-quota-remove") == 1
    assert "github.com" in html and "<img" not in html
    assert "bg-cta" not in html and "text-success" not in html


def test_the_first_view_rail_draws_a_remembered_card_and_seeds_it():
    async def main():
        loop = asyncio.get_running_loop()
        repo_ssr._quota_memo.clear()
        body = json.dumps(DATA).encode()
        repo_ssr._remember_quota("/api/repo/forkmesh/forkmesh/mirror-quota", body, loop.time())
        signed = dict(DATA, viewer={"signedIn": True})
        repo_ssr._remember_quota("/api/repo/o/r/mirror-quota", json.dumps(signed).encode(),
                                 loop.time())
        assert repo_ssr.cached_quota("o", "r") is None
        view = repo_ssr.render_view(_ssr_data())
        page = repo_ssr.inject(_page(), _ssr_data())
        repo_ssr._quota_memo.clear()
        return view, page
    view, page = asyncio.run(main())
    rail = view[view.index("<aside"):]
    assert "<section data-repo-mirror-quota" in rail and "3 / 10 healthy" in rail
    assert rail.index("data-repo-mirror-quota") > rail.index("Repository logo")
    seed = json.loads(re.search(
        r'<script type="application/json" id="forkmesh-repo-ssr">(.*?)</script>', page,
        re.S).group(1))
    assert seed["mirrorQuota"]["target"] == 10


def test_without_a_remembered_card_the_section_is_an_empty_placeholder():
    repo_ssr._quota_memo.clear()
    view = repo_ssr.render_view(_ssr_data())
    assert '<section data-repo-mirror-quota aria-label="Mirrors" class="mt-3 border-t ' \
        'border-border pt-3" hidden></section>' in view


def test_mirror_quota_reads_are_micro_cached_and_writes_drop_them():
    def scope(method="GET"):
        return {"method": method, "path": "/api/repo/o/r/mirror-quota",
                "query_string": b"", "headers": []}
    assert isinstance(repo_ssr.micro_cache_key(scope()), repo_ssr.MicroKey)
    repo_ssr._quota_memo["/api/repo/o/r/mirror-quota"] = (0, DATA)
    assert repo_ssr.micro_cache_key(scope("POST")) is None
    assert "/api/repo/o/r/mirror-quota" not in repo_ssr._quota_memo


def test_the_dashboard_rail_and_the_first_view_share_the_section():
    detail = (JS / "08-repo-detail-network.js").read_text("utf-8")
    source = (JS / "09-mirror-quota.js").read_text("utf-8")
    assert "repoMirrorQuotaSection(repo)" in detail
    assert ('<section data-repo-mirror-quota aria-label="Mirrors" class="mt-3 border-t '
            'border-border pt-3"') in source


def _ssr_data():
    from test_repo_ssr import DATA as BASE
    return dict(BASE)


def _page():
    from test_repo_ssr import repo_page
    return repo_page()


HELP = "/example/forum/mirror"


@needs_node
@pytest.mark.parametrize("change", [{}, {"target": 7, "healthy": 1}, {"target": 3, "healthy": 5},
                                    {"target": 0}, {"target": 4, "healthy": 3}])
def test_the_help_line_matches_the_dashboard_render(change):
    data = dict(DATA, **change)
    for viewer in (None, {"canSetTarget": True}):
        assert py_render(data, viewer, HELP) == js_render(data, viewer, HELP)


def test_a_card_below_its_goal_links_to_the_how_to_help_page():
    html = py_render(DATA, None, HELP)
    assert '<p data-mirror-quota-help class="mt-1.5 text-[11px] text-muted-foreground">' \
        'Needs 7 more mirrors &middot; <a href="/example/forum/mirror"' in html
    assert html.index("data-mirror-quota-help") < html.index("<ul")
    assert "Needs 1 more mirror &middot;" in py_render(dict(DATA, target=4, healthy=3), None, HELP)
    assert "data-mirror-quota-help" not in py_render(dict(DATA, target=3, healthy=5), None, HELP)
    assert "data-mirror-quota-help" not in py_render(DATA)
    assert 'href="/example/forum/mirror"' in repo_ssr_mirrors.section_html(DATA, None, HELP)


def test_the_first_view_rail_links_its_card_to_the_help_page():
    async def main():
        loop = asyncio.get_running_loop()
        repo_ssr._quota_memo.clear()
        repo_ssr._remember_quota("/api/repo/forkmesh/forkmesh/mirror-quota",
                                 json.dumps(DATA).encode(), loop.time())
        view = repo_ssr.render_view(_ssr_data())
        repo_ssr._quota_memo.clear()
        return view
    assert 'href="/forkmesh/forkmesh/mirror" class="dashboard-accent-link">How to help' in \
        asyncio.run(main())


def test_the_dashboard_card_and_dialog_point_at_the_help_page():
    source = (JS / "09-mirror-quota.js").read_text("utf-8")
    assert source.count("renderRepoMirrorQuotaCard(data, data?.viewer || null, "
                        "repoMirrorHelpHref(repo))") == 2
    assert "Read the full guide: run a mirror, join the federation, register it" in source

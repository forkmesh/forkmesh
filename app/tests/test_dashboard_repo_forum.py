import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402

JS = ROOT / "public" / "dashboard" / "js"
SOURCE = JS / "09-repo-forum.js"
DETAIL = JS / "08-repo-detail-network.js"
NODE = shutil.which("node")


def test_the_forum_is_a_lazy_chunk_the_forum_tab_loads():
    assert dashboard_bundle.LAZY_CHUNKS["repo-forum"] == "09-repo-forum.js"
    assert "09-repo-forum.js" in dashboard_bundle.FRAGMENTS
    assert "dashboard-page-repo-forum.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    built = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert 'loadDashboardChunk("repo-forum")' in built
    assert "function loadRepoForum(...args)" in built
    chunk = (ROOT / "public" / "dashboard-page-repo-forum.js").read_text(encoding="utf-8")
    assert "function forumCookedHtml(" in chunk


def test_forum_is_a_repository_route_everywhere():
    assert "forum" in static_routes.REPO_TAB_ROUTES
    helpers = (JS / "02-helpers.js").read_text(encoding="utf-8")
    assert re.search(r'const REPO_TAB_ROUTES = \[[^\]]*"discussions", "forum"', helpers)
    detail = DETAIL.read_text(encoding="utf-8")
    assert '<section data-dashboard-repo-tab-panel="forum" class="hidden"><div data-repo-forum' in detail
    assert 'forum: "forum"' in detail
    assert 'routeKind === "security" || routeKind === "forum"' in detail
    assert 'if (kind === "forum") loadRepoForum(repo);' in detail
    assert 'data-repo-feature="forum"' in detail
    assert 'if (routeKind !== "forum") detail.querySelector(\'[data-dashboard-repo-tab="forum"]\')' in detail
    content = (JS / "06-repo-content.js").read_text(encoding="utf-8")
    assert 'const REPO_FEATURE_TABS = ["issues", "pulls", "discussions", "forum", "actions"];' in content
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    assert '"forum": False' in entry.split("REPO_FEATURE_DEFAULTS = ", 1)[1].split("}", 1)[0]


def test_the_page_keeps_the_colour_roles_and_never_loads_remote_content():
    source = SOURCE.read_text(encoding="utf-8")
    assert not re.search(r"\son[a-z]+\s*=", source.replace("onerror", ""))
    assert "bg-cta" not in source and "text-cta" not in source and "--dashboard-success" not in source
    assert "new DOMParser()" in source
    assert "innerHTML = cooked" not in source and ".cooked}" not in source
    assert "srcset" not in source
    assert "source/discourse.json" not in source
    assert 'rel="nofollow noopener noreferrer"' in source
    assert 'referrerpolicy="no-referrer"' in source
    for tag in ("script", "iframe", "svg", "object", "embed", "style", "form"):
        assert '"%s"' % tag in source.split("FORUM_DROPPED_TAGS", 1)[1].split("]);", 1)[0]


def test_the_package_switch_exists_and_is_web_only():
    manifest = json.loads((REPO / "marketplace" / "packages" / "forum" / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["entrypoints"] == {"web": "builtin:forum", "settings": "builtin:package"}
    assert manifest["install"] == {"bundled": True, "default": True, "sync": "account", "places": ["web"]}
    assert not (REPO / "marketplace" / "packages" / "forum" / "content").exists()


NODE_HARNESS = r"""
const fs = require("fs");
const source = fs.readFileSync(process.argv[2], "utf8");
const stubs = `
  const safeDecodeURIComponent = (v) => { try { return decodeURIComponent(v); } catch (_) { return v; } };
  const repoLinkUrl = () => "/alice/proj";
  const escapeHtml = (v) => String(v ?? "").replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#039;"})[c]);
  const formatDate = (v) => String(v);
`;
const api = new Function(stubs + source + "; return { forumRoute, forumHref, forumAgo, forumCount, forumColor, forumSlug, forumLink, forumOriginUrl };")();
const now = Date.UTC(2026, 9, 31, 12, 0, 0);
const origin = api.forumOriginUrl("https://forum.example.org/community");
const ctx = { origin, topicHref: (id, post) => `/t/${id}/${post}` };
const out = {
  routes: [
    [], ["categories"], ["latest"], ["top"], ["c", "general", "4"], ["c", "news", "sub", "9", "l", "latest"], ["c", "x"],
    ["tag", "dpi"], ["t", "a-title", "123"], ["t", "a-title", "123", "7"], ["t", "123"], ["t", "2024", "55"], ["t", "n12"],
    ["t", "bad", "../etc"],
  ].map((parts) => api.forumRoute(parts, "")),
  top: [api.forumRoute(["top"], "?period=weekly"), api.forumRoute(["top"], "?period=<script>")],
  hrefs: [
    api.forumHref({}, { view: "categories" }),
    api.forumHref({}, { view: "top", period: "monthly" }),
    api.forumHref({}, { view: "category", id: 4, slug: "General Talk", list: "latest" }),
    api.forumHref({}, { view: "topic", id: "123", slug: "Блокировка сайтов?", post: 7 }),
    api.forumHref({}, { view: "topic", id: "5", slug: "", post: 1 }),
    api.forumHref({}, { view: "tag", key: "a b" }),
  ],
  ago: [now - 30e3, now - 5 * 60e3, now - 3 * 3600e3, now - 6 * 86400e3, Date.UTC(2026, 7, 17), Date.UTC(2025, 5, 2), 0]
    .map((ms) => api.forumAgo(ms, now)),
  counts: [0, 7, 999, 1000, 3050, 1250000, -4, "x"].map(api.forumCount),
  colors: ["0088CC", "#e45735", "fff", "red", "bf1e2e\"><script>", "", null].map(api.forumColor),
  links: [
    "https://forum.example.org/community/t/slug/42/3", "/community/t/42", "https://forum.example.org/t/slug/42",
    "/community/u/bob", "javascript:alert(1)", " JaVa\tScRiPt:alert(1)", "data:text/html,x", "mailto:a@example.org",
    "#heading-1", "https://elsewhere.example/x", "/community/c/news/4",
  ].map((value) => api.forumLink(value, ctx)),
};
process.stdout.write(JSON.stringify(out));
"""


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_routes_times_counts_colours_and_links(tmp_path):
    harness = tmp_path / "harness.js"
    harness.write_text(NODE_HARNESS, encoding="utf-8")
    result = subprocess.run([NODE, str(harness), str(SOURCE)], capture_output=True, text=True, check=True)
    out = json.loads(result.stdout)
    assert out["routes"] == [
        {"view": "categories"}, {"view": "categories"}, {"view": "latest"}, {"view": "top", "period": "all"},
        {"view": "category", "id": 4, "list": ""}, {"view": "category", "id": 9, "list": "latest"},
        {"view": "categories"}, {"view": "tag", "key": "dpi"}, {"view": "topic", "id": "123", "post": 0},
        {"view": "topic", "id": "123", "post": 7}, {"view": "topic", "id": "123", "post": 0},
        {"view": "topic", "id": "55", "post": 0}, {"view": "topic", "id": "n12", "post": 0},
        {"view": "categories"},
    ]
    assert out["top"] == [{"view": "top", "period": "weekly"}, {"view": "top", "period": "all"}]
    assert out["hrefs"] == [
        "/alice/proj/forum", "/alice/proj/forum/top?period=monthly", "/alice/proj/forum/c/general-talk/4/l/latest",
        "/alice/proj/forum/t/%D0%B1%D0%BB%D0%BE%D0%BA%D0%B8%D1%80%D0%BE%D0%B2%D0%BA%D0%B0-%D1%81%D0%B0%D0%B9%D1%82%D0%BE%D0%B2/123/7",
        "/alice/proj/forum/t/topic/5", "/alice/proj/forum/tag/a%20b",
    ]
    assert out["ago"] == ["1m", "5m", "3h", "6d", "Aug 17", "Jun 2025", ""]
    assert out["counts"] == ["0", "7", "999", "1.0k", "3.0k", "1.3m", "0", "0"]
    assert out["colors"] == ["#0088cc", "#e45735", "#fff", "", "", "", ""]
    links = out["links"]
    assert links[0] == {"href": "/t/42/3", "internal": True}
    assert links[1] == {"href": "/t/42/0", "internal": True}
    assert links[2] == {"href": "https://forum.example.org/t/slug/42", "external": True}
    assert links[3]["plain"] is True
    assert links[4] is None and links[5] is None and links[6] is None
    assert links[7] == {"href": "mailto:a@example.org", "external": True}
    assert links[8] == {"href": "#heading-1"}
    assert links[9] == {"href": "https://elsewhere.example/x", "external": True}
    assert links[10]["plain"] is True


def test_the_browser_spec_covers_hostile_posts_and_remote_requests():
    spec = (ROOT / "browser_tests" / "tests" / "dashboard-repo-forum.spec.js").read_text(encoding="utf-8")
    for needle in ("<script>window.__pwned", "onerror=", "srcset=", "javascript:", "other.example",
                   "expect(outside", "window.__pwned)).toBeUndefined()"):
        assert needle in spec


HOSTILE_SOURCE_HARNESS = r"""
const fs = require("fs");
const source = fs.readFileSync(process.argv[2], "utf8");
const body = source.slice(source.indexOf("function forumOriginUrl"), source.indexOf("function forumLink"));
const forumOriginUrl = new Function(body + "; return forumOriginUrl;")();
process.stdout.write(JSON.stringify(
  ['x"><img src=x onerror=alert(1)>', "javascript:alert(1)", "https://forum.example.org/t/a/1"]
    .map((value) => { const url = forumOriginUrl(value); return url ? url.href : null; })));
"""


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_a_committed_topic_source_cannot_break_out_of_its_links(tmp_path):
    harness = tmp_path / "harness.js"
    harness.write_text(HOSTILE_SOURCE_HARNESS, encoding="utf-8")
    result = subprocess.run([NODE, str(harness), str(SOURCE)], capture_output=True, text=True, check=True)
    assert json.loads(result.stdout) == [None, None, "https://forum.example.org/t/a/1"]
    code = SOURCE.read_text(encoding="utf-8")
    assert "`${topic.source}/" not in code and 'href="${escapeHtml(topic.source)}"' not in code
    assert code.count("forumOriginUrl(topic.source)") == 2

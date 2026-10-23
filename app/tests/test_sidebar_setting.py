import asyncio
import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import app_runtime  # noqa: E402
from test_repository_fork_tree import run  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
DOCS = (ROOT.parent / "docs" / "app" / "repo-views.md").read_text(encoding="utf-8")

MORE_REPOSITORIES = """
api.state.repositories.push(
  { owner: "gina", name: "lamp", rootCommit: "g1", cloneOnline: true },
  { owner: "hal", name: "kite", rootCommit: "h1", cloneOnline: true },
  { owner: "ivy", name: "drum", rootCommit: "i1", cloneOnline: true },
);
api.state.sidebarSettingRequested = true;
"""


def _sidebar_tops(html):
    return re.findall(r'data-sidebar-repo="([^"]+)"(?! data-sidebar-repo-fork)', html)


def _render(setting):
    return run(MORE_REPOSITORIES + "api.state.sidebarSetting = %s;\n" % json.dumps(setting) + """
api.renderSidebarRepositories(api.state.session);
api.renderHomeRepositories();
out.sidebar = sidebar.innerHTML;
out.home = home.innerHTML;
""")


def test_a_pinned_list_shows_only_those_repositories_in_its_order():
    out = _render({"pinned": ["hal/kite", "alice/widget", "missing/repo"], "forks": 3})
    assert _sidebar_tops(out["sidebar"]) == ["hal/kite", "alice/widget"]
    assert "gina/lamp" not in out["sidebar"] and "zed/tool" not in out["sidebar"]
    assert out["sidebar"].count("data-sidebar-repo-fork ") == 3
    assert out["home"].count("data-home-repo-row") == 2
    assert "gina/lamp" not in out["home"] and "zed/tool" not in out["home"]
    assert out["home"].index("hal/kite") < out["home"].index("alice/widget")


def test_a_bare_name_pins_the_repository_with_that_name():
    out = _render({"pinned": ["lamp"], "forks": 3})
    assert _sidebar_tops(out["sidebar"]) == ["gina/lamp"]


def test_the_fork_limit_applies_to_both_lists():
    out = _render({"pinned": [], "forks": 1})
    assert out["sidebar"].count("data-sidebar-repo-fork ") == 1
    assert "+4 more forks" in out["sidebar"]
    assert out["home"].count("data-home-repo-fork ") == 1
    assert "+4 more forks" in out["home"]
    none = _render({"pinned": [], "forks": 0})
    assert none["sidebar"].count("data-sidebar-repo-fork ") == 0
    assert "+5 more forks" in none["sidebar"]


def test_an_empty_or_missing_setting_keeps_the_default_lists():
    default = _render(None)
    assert len(_sidebar_tops(default["sidebar"])) == 5
    assert default["sidebar"].count("data-sidebar-repo-fork ") == 3
    assert "+2 more forks" in default["sidebar"]
    for setting in ({"pinned": [], "forks": 3}, {"pinned": ["nothing/here"], "forks": 3},
                    {"pinned": "alice/widget", "forks": "many"}):
        out = _render(setting)
        assert out["sidebar"] == default["sidebar"]
        assert out["home"] == default["home"]


def _setting(monkeypatch, doc):
    async def d1_first(_env, sql, *args):
        assert args == ("sidebar",)
        return {"doc": doc} if doc is not None else None
    monkeypatch.setattr(app_runtime, "d1_first", d1_first, raising=False)
    monkeypatch.setattr(app_runtime, "json", json, raising=False)
    return asyncio.run(app_runtime.Default._instance_sidebar_setting(
        SimpleNamespace(env=SimpleNamespace())))


def test_the_instance_document_carries_a_cleaned_setting(monkeypatch):
    assert _setting(monkeypatch, None) == {"pinned": [], "forks": 3}
    assert _setting(monkeypatch, "not json") == {"pinned": [], "forks": 3}
    assert _setting(monkeypatch, "[1, 2]") == {"pinned": [], "forks": 3}
    pinned = ["Alice/Widget", "alice/widget", " lamp ", "", "a/b/c", "x" * 162]
    pinned += ["r%d" % n for n in range(20)]
    cleaned = _setting(monkeypatch, json.dumps({"pinned": pinned, "forks": 99}))
    assert cleaned["pinned"][:2] == ["alice/widget", "lamp"]
    assert len(cleaned["pinned"]) == 10
    assert _setting(monkeypatch, json.dumps({"pinned": "acme/app"})) == {
        "pinned": [], "forks": 3}
    assert cleaned["forks"] == 50
    assert _setting(monkeypatch, json.dumps({"forks": -4}))["forks"] == 0
    assert _setting(monkeypatch, json.dumps({"forks": "two"}))["forks"] == 3
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    assert '"sidebar": await self._instance_sidebar_setting(),' in route
    assert "## Pinned repositories and fork count" in DOCS

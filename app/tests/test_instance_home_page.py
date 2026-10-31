import asyncio
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import admin_shell  # noqa: E402
import app_runtime  # noqa: E402
import instance_console as console_module  # noqa: E402
import instance_home  # noqa: E402
from test_instance_registry import FakeRequest, bind_console  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


def test_home_repository_is_cleaned():
    assert instance_home.clean(" Example/forum/ ") == "example/forum"
    assert instance_home.clean("") == ""
    assert instance_home.parse(instance_home.dump("example/forum")) == "example/forum"
    assert instance_home.parse("not json") == ""
    assert instance_home.parse('{"repository": "../x"}') == ""
    assert instance_home.public("") is None
    assert instance_home.public("a/b") == {"repository": "a/b"}


@pytest.mark.parametrize("value", [
    "forum", "a/b/c", "a/..", "a/b.git", "-x/b", "a/b c", "https://x.org/a/b", "a/" + "b" * 101])
def test_bad_home_repositories_are_refused(value):
    with pytest.raises(instance_home.SettingError):
        instance_home.clean(value)


def home_console(monkeypatch, form=None, rows=None, branded=True):
    rows, _directory = bind_console(monkeypatch, branded=branded, rows=rows)
    env = SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="example",
                          PUBLIC_BASE_URL="https://example.example.org")
    if form is None:
        request = FakeRequest("https://example.example.org/secret?view=home-page")
    else:
        request = FakeRequest("https://example.example.org/secret", "POST",
                              dict(form, action="save_home"))
    answer = asyncio.run(console_module.admin_handler(
        env, request, {"view": ["home-page"]}, "<csrf>", "", "ada"))
    return answer, rows


def test_the_console_sets_the_home_repository(monkeypatch):
    answer, rows = home_console(monkeypatch)
    assert 'name="repository"' in answer.body and "save_home" in answer.body
    answer, rows = home_console(monkeypatch, {"csrf": "ok", "repository": "example/forum"})
    assert answer.headers["location"] == "/secret?view=home-page&notice=home_saved"
    assert json.loads(rows["home"]) == {"repository": "example/forum"}
    answer, rows = home_console(monkeypatch, rows=rows)
    assert 'value="example/forum"' in answer.body
    answer, rows = home_console(monkeypatch, {"csrf": "ok", "repository": "nope"}, rows=rows)
    assert answer.headers["location"].endswith("notice=home_invalid")
    assert json.loads(rows["home"]) == {"repository": "example/forum"}
    answer, rows = home_console(monkeypatch, {"csrf": "ok", "repository": ""}, rows=rows)
    assert json.loads(rows["home"]) == {"repository": ""}
    answer, rows = home_console(monkeypatch, {"csrf": "bad", "repository": "a/b"}, rows=rows)
    assert answer.headers["location"].endswith("notice=csrf")
    answer, rows = home_console(monkeypatch, {"csrf": "ok", "repository": "a/b"},
                                branded=False)
    assert answer.headers["location"].endswith("notice=home_core") and rows == {}
    assert ("home-page", "Home page", "globe", ("console", "home-page")) in dict(
        admin_shell.NAV)["Platform"]


def test_api_instance_reports_the_home_repository(monkeypatch):
    stored = {"doc": instance_home.dump("example/forum")}

    async def d1_first(_env, sql, *args):
        assert args == ("home",)
        return {"doc": stored["doc"]} if stored["doc"] else None

    for name, value in {
        "d1_first": d1_first, "instance_home": instance_home,
        "instance_branded": lambda _env: True,
    }.items():
        monkeypatch.setattr(app_runtime, name, value, raising=False)
    me = SimpleNamespace(env=SimpleNamespace())
    assert asyncio.run(app_runtime.Default._instance_home_public(me)) == {
        "repository": "example/forum"}
    stored["doc"] = None
    assert asyncio.run(app_runtime.Default._instance_home_public(me)) is None
    monkeypatch.setattr(app_runtime, "instance_branded", lambda _env: False)
    stored["doc"] = instance_home.dump("example/forum")
    assert asyncio.run(app_runtime.Default._instance_home_public(me)) is None
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    assert '"home": await self._instance_home_public(),' in route
    assert 'instance_home = _LazyModule("instance_home")' in ENTRY

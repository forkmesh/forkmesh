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
import instance_world  # noqa: E402
from test_instance_registry import FakeRequest, bind_console  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
BLT_LIKE = {
    "style": "security",
    "accent": "#E74C3C",
    "objects": [{"type": "house", "label": "Hacker House",
                 "url": "https://house.example.org",
                 "position": [-60, -10]}],
    "bugs": {"repo": "acme/website", "path": "./.forkmesh/world/bugs.json",
             "limit": 500, "title": "Bugs we found"},
}


def test_world_settings_are_cleaned():
    setting = instance_world.clean(BLT_LIKE)
    assert setting == {
        "style": "security", "accent": "#e74c3c",
        "objects": [{"type": "house", "label": "Hacker House",
                     "url": "https://house.example.org",
                     "position": [-60.0, -10.0], "color": ""}],
        "bugs": {"repo": "acme/website", "ref": "main",
                 "path": ".forkmesh/world/bugs.json",
                 "title": "Bugs we found", "limit": 60,
                 "position": [70, 40]}}
    assert instance_world.parse(instance_world.dump(setting)) == setting
    assert instance_world.parse("not json") == instance_world.default()
    assert instance_world.parse("") == instance_world.default()


@pytest.mark.parametrize("value", [
    [],
    {"style": "neon"},
    {"extra": 1},
    {"accent": "red"},
    {"objects": [{"label": "A", "url": "http://a.example.org"}]},
    {"objects": [{"label": "A", "url": "https://user:pw@a.example.org"}]},
    {"objects": [{"label": "A", "url": "javascript:alert(1)"}]},
    {"objects": [{"label": "", "url": "https://a.example.org"}]},
    {"objects": [{"type": "tower", "label": "A",
                  "url": "https://a.example.org"}]},
    {"objects": [{"label": "A", "url": "https://a.example.org",
                  "position": [999, 0]}]},
    {"objects": [{"label": "A", "url": "https://a.example.org",
                  "position": [1]}]},
    {"objects": [{"label": "A", "url": "https://a.example.org",
                  "position": [0, 300]}]},
    {"bugs": {"repo": "acme/site", "path": "b.json", "position": [250, 200]}},
    {"objects": [{"label": "A", "url": "https://a.example.org"}] * 13},
    {"bugs": {"repo": "acme", "path": "bugs.json"}},
    {"bugs": {"repo": "acme/site", "path": "../../etc/passwd"}},
    {"bugs": {"repo": "acme/site", "ref": "../main", "path": "b.json"}},
    {"bugs": {"repo": "acme/site", "path": ""}},
    {"bugs": "yes"},
])
def test_bad_world_settings_are_refused(value):
    with pytest.raises(instance_world.SettingError):
        instance_world.clean(value)


def test_the_public_payload_carries_only_safe_fields():
    setting = instance_world.clean(BLT_LIKE)
    payload = instance_world.public(setting, "/instance-logo.svg")
    assert payload["logo"] == "/instance-logo.svg"
    assert set(payload) == {"style", "accent", "objects", "bugs", "logo"}
    assert "logo" not in instance_world.public(setting, "https://x/l.svg")


def world_console(monkeypatch, form=None, rows=None, branded=True):
    rows, _directory = bind_console(monkeypatch, branded=branded, rows=rows)
    env = SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="Acme Code",
                          INSTANCE_PLUGINS="world",
                          PUBLIC_BASE_URL="https://code.example.org")
    if form is None:
        request = FakeRequest("https://code.example.org/secret"
                              "?view=world-territory")
    else:
        request = FakeRequest("https://code.example.org/secret", "POST",
                              dict(form, action="save_world"))
    answer = asyncio.run(console_module.admin_handler(
        env, request, {"view": ["world-territory"]}, "<csrf>", "", "ada"))
    return answer, rows


def test_the_console_edits_the_world(monkeypatch):
    answer, rows = world_console(monkeypatch)
    assert 'name="world"' in answer.body and "save_world" in answer.body
    assert "&quot;style&quot;: &quot;meadow&quot;" in answer.body
    answer, rows = world_console(
        monkeypatch, {"csrf": "ok", "world": json.dumps(BLT_LIKE)})
    assert answer.headers["location"] == (
        "/secret?view=world-territory&notice=world_saved")
    assert json.loads(rows["world"])["style"] == "security"
    answer, rows = world_console(
        monkeypatch, {"csrf": "ok", "world": '{"style": "neon"}'}, rows=rows)
    assert answer.headers["location"].endswith("notice=world_invalid")
    assert json.loads(rows["world"])["style"] == "security"
    answer, rows = world_console(
        monkeypatch, {"csrf": "bad", "world": "{}"}, rows=rows)
    assert answer.headers["location"].endswith("notice=csrf")
    answer, rows = world_console(
        monkeypatch, {"csrf": "ok", "world": "{}"}, branded=False)
    assert answer.headers["location"].endswith("notice=world_core")
    assert rows == {}
    assert ("world-territory", "World", "grid",
            ("console", "world-territory")) in dict(admin_shell.NAV)[
                "Platform"]


def test_api_instance_reports_the_world(monkeypatch):
    stored = {"doc": instance_world.dump(instance_world.clean(BLT_LIKE))}

    async def d1_first(_env, sql, *args):
        assert args == ("world",)
        return {"doc": stored["doc"]} if stored["doc"] else None

    for name, value in {
        "d1_first": d1_first, "instance_world": instance_world,
        "instance_branded": lambda _env: True,
    }.items():
        monkeypatch.setattr(app_runtime, name, value, raising=False)
    me = SimpleNamespace(env=SimpleNamespace(INSTANCE_LOGO="/instance-logo.svg"))
    world = asyncio.run(app_runtime.Default._instance_world_public(me))
    assert world["style"] == "security" and world["logo"] == "/instance-logo.svg"
    assert world["bugs"]["repo"] == "acme/website"
    stored["doc"] = None
    world = asyncio.run(app_runtime.Default._instance_world_public(me))
    assert world == {"style": "meadow", "accent": "", "objects": [],
                     "bugs": None, "logo": "/instance-logo.svg"}
    monkeypatch.setattr(app_runtime, "instance_branded", lambda _env: False)
    assert asyncio.run(app_runtime.Default._instance_world_public(me)) is None
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    assert '"world": await self._instance_world_public(),' in route
    assert 'instance_world = _LazyModule("instance_world")' in ENTRY

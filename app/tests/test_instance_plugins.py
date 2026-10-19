import asyncio
import html
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlencode

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))

import admin_shell  # noqa: E402
import app_runtime  # noqa: E402
import instance_console as console_module  # noqa: E402
import instance_features  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
INSTALLER = (REPO / "www" / "public" / "install-instance.sh").read_text(
    encoding="utf-8")
NOW = 1_800_000_000_000


@pytest.fixture(autouse=True)
def fresh_cache():
    instance_features.forget()
    yield
    instance_features.forget()


def env(plugins="", name="Acme Code"):
    return SimpleNamespace(INSTANCE_NAME=name, INSTANCE_PLUGINS=plugins)


def test_every_plugin_is_either_offered_or_flagged_as_forkmesh_only():
    offered = set(instance_features.CHOOSABLE)
    fixed = set(instance_features.FORKMESH_ONLY)
    assert offered | fixed == set(instance_features.IDS)
    assert not offered & fixed
    assert set(instance_features.INSTANCE_NOTES) == offered
    assert {"world", "genie"} <= offered
    assert {"rewards", "ads", "website", "blog", "hackathon"} <= fixed
    described = {item["id"]: item for item in instance_features.describe(
        env())}
    assert described["world"]["scope"] == "instance"
    assert described["rewards"]["scope"] == "forkmesh"
    assert all(item["note"] for item in described.values())


def test_without_a_saved_choice_the_install_setting_decides():
    assert instance_features.enabled_ids(env("world,blog")) == {
        "world", "blog"}
    assert instance_features.enabled_ids(env("")) == set()
    assert instance_features.enabled_ids(env("", name="")) == set(
        instance_features.IDS)


def test_a_saved_choice_wins_over_the_install_setting():
    instance_features.remember({"world"}, NOW)
    assert instance_features.enabled_ids(env("")) == {"world"}
    assert instance_features.enabled_ids(env("blog,genie")) == {"world"}
    assert instance_features.blocked(env("blog"), "/api/blog") == "blog"
    assert instance_features.blocked(env(""), "/world/") == ""
    instance_features.remember(None, NOW)
    assert instance_features.enabled_ids(env("blog")) == {"blog"}
    assert instance_features.enabled_ids(env("", name="")) == set(
        instance_features.IDS)


def test_a_stored_row_is_read_strictly():
    assert instance_features.parse("") is None
    assert instance_features.parse("{") is None
    assert instance_features.parse("[]") is None
    assert instance_features.parse('{"enabled": "world"}') is None
    assert instance_features.parse(
        '{"enabled": ["world", "nope", 3, "blog"]}') == {"world", "blog"}
    assert instance_features.parse('{"enabled": []}') == set()
    doc = instance_features.dump({"world", "genie", "unknown"})
    assert json.loads(doc) == {"enabled": ["genie", "world"]}


def test_the_console_only_turns_offered_plugins_on_or_off():
    current = {"world", "blog"}
    assert instance_features.choose(current, {"genie", "rewards"}) == {
        "genie", "blog"}
    assert instance_features.choose(set(), {"rewards", "website"}) == set()


class Answer:
    def __init__(self, body, status=200, headers=None, **_extra):
        self.body, self.status = body, status
        self.headers = dict(headers or {})


def loader(monkeypatch, rows, clock, fail=False):
    reads = []

    async def d1_first(_env, sql, *args):
        reads.append(args)
        if fail:
            raise RuntimeError("database is locked")
        assert "FROM instance_settings" in sql
        doc = rows.get(args[0])
        return {"doc": doc} if doc is not None else None

    monkeypatch.setattr(app_runtime, "d1_first", d1_first, raising=False)
    monkeypatch.setattr(app_runtime, "Date",
                        SimpleNamespace(now=lambda: clock[0]), raising=False)
    monkeypatch.setattr(app_runtime, "instance_features", instance_features,
                        raising=False)
    me = SimpleNamespace(env=env("blog"))
    return reads, lambda: asyncio.run(
        app_runtime.Default._instance_plugins_load(me))


def test_the_server_reads_the_choice_at_most_every_thirty_seconds(
        monkeypatch):
    rows = {"plugins": instance_features.dump({"world"})}
    clock = [NOW]
    reads, load = loader(monkeypatch, rows, clock)
    load()
    load()
    assert len(reads) == 1
    assert instance_features.enabled_ids(env("blog")) == {"world"}
    rows["plugins"] = instance_features.dump({"genie"})
    clock[0] += instance_features.CACHE_MS
    load()
    assert len(reads) == 2
    assert instance_features.enabled_ids(env("blog")) == {"genie"}
    del rows["plugins"]
    clock[0] += instance_features.CACHE_MS
    load()
    assert instance_features.enabled_ids(env("blog")) == {"blog"}


def test_a_failed_read_keeps_the_last_choice(monkeypatch):
    instance_features.remember({"world"}, NOW - instance_features.CACHE_MS)
    reads, load = loader(monkeypatch, {}, [NOW], fail=True)
    load()
    assert len(reads) == 1
    assert instance_features.enabled_ids(env("blog")) == {"world"}
    load()
    assert len(reads) == 1


def test_every_request_on_an_instance_loads_the_choice_before_the_gate():
    fetch = RUNTIME.split("    async def fetch(self, request):", 1)[1]
    gate = fetch.split("upgrade = str(", 1)[0]
    assert gate.index("await self._instance_plugins_load()") < gate.index(
        "instance_features.blocked(self.env, plugin_path)")
    assert gate.index("instance_features.branded(self.env)") < gate.index(
        "await self._instance_plugins_load()")


def test_the_installer_never_writes_the_saved_choice():
    assert "'plugins'" not in INSTALLER
    assert "instance_settings" not in INSTALLER.split(
        "write_settings() {", 1)[1].split("\n}\n", 1)[0]


class FakeRequest:
    def __init__(self, url, method="GET", form=None):
        self.url, self.method = url, method
        self.form = urlencode(form or {}, doseq=True)

    async def text(self):
        return self.form


def bind_console(monkeypatch, branded=True, rows=None, audits=None):
    rows = rows if rows is not None else {}

    async def d1_first(_env, sql, *args):
        return {"doc": rows[args[0]]} if args and args[0] in rows else None

    async def d1_run(_env, sql, *args):
        assert "INSERT INTO instance_settings" in sql
        rows[args[0]] = args[1]

    async def audit(_env, account, action, kind, target, outcome, details):
        if audits is not None:
            audits.append((account, action, kind, outcome, details))

    async def nav_badges(_env):
        return {}

    monkeypatch.setattr(admin_shell, "nav_badges", nav_badges)
    for name, value in {
        "_admin_shell": admin_shell, "_html_escape": html.escape,
        "_admin_href": lambda _query, **params: "?" + "&amp;".join(
            "%s=%s" % item for item in params.items()),
        "Date": SimpleNamespace(now=lambda: NOW),
        "Response": Answer,
        "method_name": lambda request: request.method,
        "_admin_csrf_ok": lambda _env, form: form.get("csrf") == ["ok"],
        "instance_branded": lambda _env: branded,
        "d1_first": d1_first, "d1_run": d1_run,
        "_audit_sensitive_action": audit,
    }.items():
        monkeypatch.setattr(console_module, name, value, raising=False)
    return rows


def console(form=None, plugins="blog"):
    if form is None:
        request = FakeRequest("https://git.example.org/secret?view=plugins")
    else:
        request = FakeRequest("https://git.example.org/secret", "POST",
                              dict(form, action="save_plugins"))
    return asyncio.run(console_module.admin_handler(
        SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="Acme Code",
                        INSTANCE_PLUGINS=plugins), request,
        {"view": ["plugins"]}, "<csrf>", "", "ada"))


def test_the_console_offers_a_switch_per_instance_plugin(monkeypatch):
    bind_console(monkeypatch)
    page = console().body
    for plugin_id in instance_features.CHOOSABLE:
        assert 'value="%s"' % plugin_id in page
    for plugin_id in instance_features.FORKMESH_ONLY:
        assert 'name="plugin" value="%s"' % plugin_id not in page
    assert "forkmesh.com services" in page
    assert "Set by the install settings" in page
    assert ">Plugins<" in page and 'name="action" value="save_plugins"' in page


def test_an_admin_saves_plugins_and_they_apply_at_once(monkeypatch):
    audits = []
    rows = bind_console(monkeypatch, audits=audits)
    answer = console({"csrf": "ok", "plugin": ["world", "genie", "rewards"]})
    assert answer.status == 303
    assert answer.headers["location"] == (
        "/secret?view=plugins&notice=plugins_saved")
    assert instance_features.parse(rows["plugins"]) == {
        "world", "genie", "blog"}
    assert instance_features.enabled_ids(env("")) == {
        "world", "genie", "blog"}
    assert audits[-1] == (
        "ada", "admin.console_save_plugins", "instance_settings", "success",
        {"enabled": ["blog", "genie", "world"],
         "turnedOn": ["genie", "world"], "turnedOff": [],
         "notice": "plugins_saved"})
    console({"csrf": "ok", "plugin": ["genie"]})
    assert instance_features.parse(rows["plugins"]) == {"genie", "blog"}
    assert audits[-1][4]["turnedOff"] == ["world"]
    page = console().body
    assert "Saved from this page." in page


def test_a_forged_or_unknown_post_changes_nothing(monkeypatch):
    audits = []
    rows = bind_console(monkeypatch, audits=audits)
    answer = console({"csrf": "bad", "plugin": ["world"]})
    assert answer.headers["location"].endswith("notice=csrf")
    request = FakeRequest("https://git.example.org/secret", "POST",
                          {"csrf": "ok", "action": "drop_tables"})
    answer = asyncio.run(console_module.admin_handler(
        SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="Acme Code"),
        request, {"view": ["plugins"]}, "<csrf>", "", "ada"))
    assert answer.headers["location"].endswith("notice=invalid")
    assert rows == {} and audits == []
    assert instance_features.enabled_ids(env("")) == set()


def test_forkmesh_com_shows_no_form_and_refuses_a_write(monkeypatch):
    rows = bind_console(monkeypatch, branded=False)
    assert "save_plugins" not in console().body
    answer = console({"csrf": "ok", "plugin": ["world"]})
    assert answer.headers["location"].endswith("notice=plugins_core")
    assert rows == {}


def test_the_plugins_view_is_in_the_platform_menu_and_routed():
    keys = [item[0] for group, items in admin_shell.NAV for item in items
            if group == "Platform"]
    assert "plugins" in keys
    assert 'instance_console = _LazyModule("instance_console")' in ENTRY
    admin_route = RUNTIME.split(
        "if not await _check_admin_page_auth(self.env, request):", 1)[1]
    assert "in _instance_console.VIEWS:" in admin_route

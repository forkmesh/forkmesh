import ast
import asyncio
import hashlib
import ipaddress
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import attribution  # noqa: E402
import instance_console as console_module  # noqa: E402
import instance_privacy  # noqa: E402
import mail_activity  # noqa: E402
import mail_tracking  # noqa: E402
import not_found_log  # noqa: E402
import probe_ban  # noqa: E402
from test_instance_registry import FakeRequest, bind_console  # noqa: E402
from test_turnstile import FakeSiteverify, live_env, verify  # noqa: E402

ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
RUNTIME_TEXT = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
STRICT = SimpleNamespace(INSTANCE_PRIVACY_MODE="1")
OFF = SimpleNamespace()


@pytest.fixture(autouse=True)
def fresh_cache():
    instance_privacy.forget()
    yield
    instance_privacy.forget()


def _run(coro):
    return asyncio.run(coro)


def _entry_functions(*names, **namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in names]
    assert {node.name for node in selected} == set(names)
    namespace.setdefault("instance_privacy", instance_privacy)
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY),
                 "exec"), namespace)
    return namespace


class Headers:
    def __init__(self, values):
        self.values = {key.lower(): value for key, value in values.items()}

    def get(self, name, default=None):
        return self.values.get(name.lower(), default)


def test_the_setting_is_one_small_document():
    assert instance_privacy.SETTING_KEY == "privacy"
    assert instance_privacy.parse(instance_privacy.dump("strict")) == "strict"
    assert instance_privacy.parse(instance_privacy.dump("off")) == "off"
    assert json.loads(instance_privacy.dump("strict")) == {"mode": "strict"}
    for doc in ("", "nope", "[]", '{"mode": "loud"}'):
        assert instance_privacy.parse(doc) is None


def test_the_install_setting_forces_it_on_and_the_console_setting_adds_to_it():
    assert not instance_privacy.active(OFF)
    assert instance_privacy.active(STRICT)
    for value in ("1", "true", "on", "strict", " Yes "):
        assert instance_privacy.forced(
            SimpleNamespace(INSTANCE_PRIVACY_MODE=value))
    for value in ("", "0", "false", "off"):
        assert not instance_privacy.forced(
            SimpleNamespace(INSTANCE_PRIVACY_MODE=value))
    instance_privacy.remember("strict", 1000)
    assert instance_privacy.active(OFF)
    instance_privacy.remember("off", 1000)
    assert not instance_privacy.active(OFF)
    assert instance_privacy.active(STRICT)


def test_the_stored_setting_is_read_from_the_database_and_cached():
    calls = []

    class Statement:
        def __init__(self, sql):
            self.sql = sql

        def bind(self, *args):
            calls.append((self.sql, args))
            return self

        async def first(self):
            return {"doc": '{"mode":"strict"}'}

    env = SimpleNamespace(DB=SimpleNamespace(prepare=Statement))
    assert _run(instance_privacy.load(env, now_ms=10_000)) is True
    assert _run(instance_privacy.load(env, now_ms=20_000)) is True
    assert len(calls) == 1
    assert calls[0][1] == ("privacy",)
    _run(instance_privacy.load(env, now_ms=10_000 + instance_privacy.CACHE_MS))
    assert len(calls) == 2

    class Broken:
        def __init__(self, sql):
            raise RuntimeError("no table")

    instance_privacy.forget()
    assert _run(instance_privacy.load(
        SimpleNamespace(DB=SimpleNamespace(prepare=Broken)), 1)) is False


def test_visitor_addresses_become_daily_pseudonyms_outside_the_public_internet():
    day = 3 * instance_privacy.SALT_ROTATE_S + 5
    first = instance_privacy.pseudonym("203.0.113.9", now_s=day)
    assert first == instance_privacy.pseudonym("203.0.113.9", now_s=day + 60)
    assert first != instance_privacy.pseudonym("203.0.113.10", now_s=day)
    parsed = ipaddress.ip_address(first)
    assert parsed in instance_privacy.PSEUDONYM_NETWORK
    assert not parsed.is_global
    assert instance_privacy.is_pseudonym(first)
    assert not instance_privacy.is_pseudonym("203.0.113.9")
    assert "203.0.113.9" not in first
    assert instance_privacy.pseudonym(first, now_s=day) == first
    later = instance_privacy.pseudonym(
        "203.0.113.9", now_s=day + instance_privacy.SALT_ROTATE_S)
    assert later != first


def test_one_ipv6_network_keeps_one_network_prefix():
    day = 7 * instance_privacy.SALT_ROTATE_S
    one = ipaddress.ip_address(instance_privacy.pseudonym(
        "2001:db8:1:2::10", now_s=day))
    two = ipaddress.ip_address(instance_privacy.pseudonym(
        "2001:db8:1:2::20", now_s=day))
    other = ipaddress.ip_address(instance_privacy.pseudonym(
        "2001:db8:1:3::10", now_s=day))
    assert one != two
    assert one.packed[:8] == two.packed[:8]
    assert one.packed[:8] != other.packed[:8]


def test_loopback_and_the_ssh_gateway_pin_keep_their_address():
    env = SimpleNamespace(SSH_GATEWAY_ALLOWED_ADDRESSES="198.51.100.0/24")
    assert instance_privacy.pseudonym("127.0.0.1") == "127.0.0.1"
    assert instance_privacy.pseudonym("::1") == "::1"
    assert instance_privacy.pseudonym("198.51.100.7", env) == "198.51.100.7"
    assert instance_privacy.pseudonym("198.51.101.7", env) != "198.51.101.7"
    assert instance_privacy.pseudonym("not an address") == ""


def test_browser_reports_and_analytics_beacons_are_discarded():
    for path in ("/api/client-errors", "/api/client-errors/",
                 "/api/desktop-errors", "/api/attribution/capture",
                 "/api/attribution/events", "/api/not-found",
                 "/api/analytics/_/capture", "/api/analytics/_/decide"):
        assert instance_privacy.discards(STRICT, "POST", path), path
        assert not instance_privacy.discards(OFF, "POST", path), path
    assert not instance_privacy.discards(STRICT, "GET", "/api/client-errors")
    assert not instance_privacy.discards(STRICT, "POST", "/api/repo/a/b")
    assert "instance_privacy.discards(self.env, method_name(request), " \
        "url.path)" in RUNTIME_TEXT


def test_api_instance_says_whether_privacy_mode_is_on():
    assert '"privacyMode": instance_privacy.active(self.env)' in RUNTIME_TEXT
    assert "await instance_privacy.load(self.env, int(Date.now()))" \
        in RUNTIME_TEXT


def test_page_view_recorders_are_skipped_in_the_response_tail():
    for guarded in (
            "if not read_only and not private_console and not private_mode:\n"
            "            response = await _attribution.capture_page_view(",
            "if not read_only and not private_console and not private_mode:\n"
            "            await record_site_referral(",
            "if not read_only and not private_mode:\n"
            "            await record_blog_visit(",
            "                not private_mode:\n"
            "            await _not_found_log.record_response("):
        assert guarded in RUNTIME_TEXT, guarded


def test_the_404_log_probe_bans_and_attribution_record_nothing():
    calls = []

    async def ensure_schema(_env):
        calls.append("schema")

    not_found_log.ensure_schema = ensure_schema
    assert _run(not_found_log.record(
        STRICT, "code.example.org", "GET", "/wp-login.php")) is False
    assert calls == []
    assert probe_ban.mode(STRICT) == "off"
    assert probe_ban.mode(OFF) == "ban"
    request = SimpleNamespace(headers=Headers({}))
    assert attribution.tracking_declined(request, STRICT) is True


def test_email_opens_and_clicks_are_not_tracked():
    assert _run(mail_activity.tracked(
        STRICT, "0123abcd-0123-4567-89ab-0123456789ab", "open", 1)) is False

    async def blind_index(_env, value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    namespace = _entry_functions(
        "_mail_tracked_html", blind_index=blind_index,
        mail_tracking=mail_tracking, mail_activity=mail_activity)
    env = SimpleNamespace(API_ORIGIN="https://code.example.org",
                          INSTANCE_PRIVACY_MODE="1")
    body = '<p><a href="https://example.org/x">x</a></p>'
    html, unsubscribe = _run(namespace["_mail_tracked_html"](
        env, "0123abcd-0123-4567-89ab-0123456789ab", body))
    assert html == body
    assert unsubscribe.startswith("https://code.example.org/api/mail/t/u/")
    html, _ = _run(namespace["_mail_tracked_html"](
        SimpleNamespace(API_ORIGIN="https://code.example.org"),
        "0123abcd-0123-4567-89ab-0123456789ab", body))
    assert "/api/mail/t/o/" in html


def test_sessions_and_signups_keep_no_address_user_agent_or_country():
    namespace = _entry_functions(
        "_account_session_client_ip", "_signup_metadata",
        "_transient_client_address", "_visitor_count_address",
        ipaddress=ipaddress,
        Date=SimpleNamespace(now=lambda: 5),
        _referral_user_agent=lambda request: "Mozilla/5.0 raw",
        world_request_country=lambda request: "NL",
        _generalized_client_category=lambda request: "browser",
        world_visitor_metrics=SimpleNamespace(
            canonical_edge_address=lambda value: str(value or "")))
    pseudonym = instance_privacy.pseudonym("203.0.113.9")
    hidden = SimpleNamespace(headers=Headers(
        {"cf-connecting-ip": pseudonym, "user-agent": "Mozilla/5.0 raw"}))
    plain = SimpleNamespace(headers=Headers(
        {"cf-connecting-ip": "203.0.113.9", "user-agent": "Mozilla/5.0"}))
    assert namespace["_account_session_client_ip"](hidden) == ""
    assert namespace["_account_session_client_ip"](plain) == "203.0.113.9"
    assert namespace["_signup_metadata"](hidden) == {
        "clientCategory": "browser", "at": 5}
    assert namespace["_signup_metadata"](plain)["ip"] == "203.0.113.9"
    assert namespace["_visitor_count_address"](pseudonym) == ""
    assert namespace["_visitor_count_address"]("203.0.113.9") == "203.0.113.9"
    assert "not instance_privacy.is_pseudonym(signup_ip) else None" \
        in ENTRY_TEXT


def test_view_counters_downloads_sentry_and_error_actors_follow_the_mode():
    namespace = _entry_functions("_repo_viewer_key")
    assert _run(namespace["_repo_viewer_key"](STRICT, None, {}, "a/b")) == ""
    rows = []

    async def ensure_schema(_env):
        return None

    async def d1_run(_env, sql, *args):
        rows.append(args)

    namespace = _entry_functions(
        "record_release_download", ensure_schema=ensure_schema,
        d1_run=d1_run, Date=SimpleNamespace(now=lambda: 9))
    _run(namespace["record_release_download"](STRICT, "bi", "sha", "curl/8"))
    _run(namespace["record_release_download"](OFF, "bi", "sha", "curl/8"))
    assert rows == [("bi", "sha", 9, ""), ("bi", "sha", 9, "curl/8")]
    sentry = _entry_functions("capture_sentry_error",
                              _sentry_dsn_parts=lambda value: 1 / 0)
    assert _run(sentry["capture_sentry_error"](
        STRICT, 500, "GET", "/x", "boom")) is False
    for marker in (
            '        if instance_privacy.active(env):\n            actor = ""',
            "        if self.request is None or instance_privacy.active("
            "self.env):",
            "    if instance_privacy.active(env):\n        return\n"
            "    now = int(Date.now())\n    await ensure_schema(env)\n\n"
            "    address = _visitor_count_address("):
        assert marker in ENTRY_TEXT


def test_turnstile_is_not_sent_a_pseudonym():
    fake = FakeSiteverify()
    pseudonym = instance_privacy.pseudonym("203.0.113.9")
    assert verify(live_env(), "tok-1", fetcher=fake,
                  remoteip=pseudonym) == ("", "")
    assert "remoteip" not in json.loads(fake.calls[0][1]["body"])
    fake = FakeSiteverify()
    assert verify(live_env(), "tok-2", fetcher=fake,
                  remoteip="203.0.113.9") == ("", "")
    assert json.loads(fake.calls[0][1]["body"])["remoteip"] == "203.0.113.9"


def privacy_console(monkeypatch, form=None, rows=None, branded=True,
                    forced=False, audits=None):
    rows, _directory = bind_console(monkeypatch, branded=branded, rows=rows,
                                    audits=audits)
    env = SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="Acme Code",
                          INSTANCE_PLUGINS="",
                          PUBLIC_BASE_URL="https://code.example.org")
    if forced:
        env.INSTANCE_PRIVACY_MODE = "1"
    if form is None:
        request = FakeRequest("https://code.example.org/secret?view=privacy")
    else:
        request = FakeRequest("https://code.example.org/secret", "POST",
                              dict(form, action="save_privacy"))
    answer = asyncio.run(console_module.admin_handler(
        env, request, {"view": ["privacy"]}, "<csrf>", "", "ada"))
    return answer, rows, env


def test_the_console_turns_privacy_mode_on_and_off(monkeypatch):
    audits = []
    answer, rows, env = privacy_console(monkeypatch)
    assert "save_privacy" in answer.body
    assert "Privacy mode: no visitor logs" in answer.body
    assert "privacyMode" in answer.body
    answer, rows, env = privacy_console(
        monkeypatch, {"csrf": "ok", "privacy": "1"}, audits=audits)
    assert answer.headers["location"] == (
        "/secret?view=privacy&notice=privacy_on")
    assert json.loads(rows["privacy"]) == {"mode": "strict"}
    assert instance_privacy.active(env)
    assert audits[-1][1] == "admin.console_save_privacy"
    assert audits[-1][2] == "success"
    answer, rows, env = privacy_console(
        monkeypatch, {"csrf": "ok"}, rows=rows)
    assert answer.headers["location"].endswith("notice=privacy_off")
    assert json.loads(rows["privacy"]) == {"mode": "off"}
    assert not instance_privacy.active(env)
    answer, rows, env = privacy_console(
        monkeypatch, {"csrf": "bad", "privacy": "1"}, rows=rows)
    assert answer.headers["location"].endswith("notice=csrf")
    assert json.loads(rows["privacy"]) == {"mode": "off"}


def test_the_install_setting_keeps_it_on(monkeypatch):
    answer, rows, env = privacy_console(monkeypatch, forced=True)
    assert "INSTANCE_PRIVACY_MODE=1" in answer.body
    answer, rows, env = privacy_console(
        monkeypatch, {"csrf": "ok"}, forced=True)
    assert answer.headers["location"].endswith("notice=privacy_forced")
    assert instance_privacy.active(env)


def test_forkmesh_com_does_not_offer_it(monkeypatch):
    answer, rows, env = privacy_console(monkeypatch, branded=False)
    assert "save_privacy" not in answer.body
    answer, rows, env = privacy_console(
        monkeypatch, {"csrf": "ok", "privacy": "1"}, branded=False)
    assert answer.headers["location"].endswith("notice=privacy_core")
    assert "privacy" not in rows


def test_the_admin_menu_lists_it():
    import admin_shell
    assert "privacy" in admin_shell.NAV_KEYS
    assert console_module.PRIVACY_VIEW in console_module.VIEWS

#!/usr/bin/env python3
"""Ping settings (issue #347): per-kind switches, quiet hours, one page."""
import ast
import asyncio
import datetime
import json
import re
from pathlib import Path

import marketing_email

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
PING_SETTINGS_TEXT = (ROOT / "src" / "ping_settings.py").read_text(
    encoding="utf-8")

HELPERS = (
    "_clean_profile_timezone", "ping_pause_until", "pings_paused",
    "_apply_ping_pause", "ping_kind_catalog", "ping_muted_kinds",
    "_apply_ping_muted_kinds", "_quiet_minute", "ping_quiet_hours",
    "_apply_ping_quiet_hours", "ping_quiet_until", "_forget_ping_settings",
    "_ping_delivery_settings", "notification_email_preferences",
    "_clean_notification_preferences", "notification_delivery_mode",
    "_apply_notification_delivery", "ping_settings_payload",
    "_apply_ping_settings", "enqueue_notification", "_notification_trim_due",
)
CONSTANTS = (
    "NOTIFICATION_KINDS", "NOTIFICATION_EMAIL_KINDS",
    "NOTIFICATION_EMAIL_DEFAULTS", "NOTIFICATION_DELIVERY_MODES",
    "NOTIFICATION_DELIVERY_DEFAULT", "PING_PAUSE_MAX_MS", "PING_KIND_GROUPS",
    "PING_KIND_CATALOG", "PING_QUIET_DAYS", "PING_QUIET_DEFAULT_START",
    "PING_QUIET_DEFAULT_END", "_PING_SETTINGS_MEMO",
    "PING_SETTINGS_MEMO_TTL_MS", "PING_SETTINGS_MEMO_MAX",
    "MAX_PROFILE_TIMEZONE", "MAX_NOTIFICATIONS_PER_RECIPIENT",
    "NOTIFICATION_RETAIN_MS",
)

# 2026-09-22 12:00 UTC, a Tuesday.
NOW = int(datetime.datetime(
    2026, 9, 22, 12, 0, tzinfo=datetime.timezone.utc).timestamp() * 1000)


class Clock:
    value = NOW

    @classmethod
    def now(cls):
        return cls.value


def _load(accounts=None, runs=None, pushes=None):
    body = []
    nodes = (ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body
             + ast.parse(PING_SETTINGS_TEXT).body)
    for node in nodes:
        if isinstance(node, ast.Assign):
            names = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if names & set(CONSTANTS):
                body.append(node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                and node.name in HELPERS:
            body.append(node)
    accounts = accounts if accounts is not None else {}
    runs = runs if runs is not None else []
    pushes = pushes if pushes is not None else []

    async def account_row(_env, name):
        return "bi:" + name, accounts.get(name)

    async def d1_run(_env, sql, *args):
        runs.append(" ".join(sql.split()))

    async def notify_account_event(_env, recipient, topic, record=None):
        pushes.append((recipient, topic, record))

    async def noop(*_args, **_kwargs):
        return True

    async def blind_index(_env, value):
        return "bi:" + value

    async def encrypt_row(_env, payload):
        return payload

    ns = {
        "re": re,
        "json": json,
        "marketing_email": marketing_email,
        "Date": Clock,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, size=240: (
            str(value)[:size] if isinstance(value, (str, int)) else ""),
        "valid_node_name": lambda value: bool(value),
        "_account_row": account_row,
        "ensure_schema": noop,
        "notification_payload": lambda kind, title, **kw: dict(
            kw, kind=kind, title=title),
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "d1_run": d1_run,
        "notify_account_event": notify_account_event,
        "_queue_instant_notification": noop,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=body, type_ignores=[])), str(ENTRY), "exec"), ns)
    return ns


def test_catalog_names_every_ping_kind_once_and_hides_admin_rows():
    ns = _load()
    catalog_kinds = [kind for kind, _group, _label in ns["PING_KIND_CATALOG"]]
    assert sorted(catalog_kinds) == sorted(ns["NOTIFICATION_KINDS"])
    assert len(catalog_kinds) == len(set(catalog_kinds))
    groups = {key for key, _label in ns["PING_KIND_GROUPS"]}
    assert {group for _kind, group, _label in ns["PING_KIND_CATALOG"]} <= groups

    member = ns["ping_kind_catalog"](False)
    admin = ns["ping_kind_catalog"](True)
    member_kinds = {row["kind"] for group in member for row in group["kinds"]}
    admin_kinds = {row["kind"] for group in admin for row in group["kinds"]}
    assert "error_group" not in member_kinds
    assert {"error_group", "operational_alert",
            "account_email_sent"} <= admin_kinds
    assert admin_kinds == set(ns["NOTIFICATION_KINDS"])
    mention = next(row for group in member for row in group["kinds"]
                   if row["kind"] == "mention")
    assert mention["email"] is True
    timer = next(row for group in member for row in group["kinds"]
                 if row["kind"] == "timer_finished")
    assert timer["email"] is False


def test_muted_kinds_accept_a_list_or_a_partial_map_and_store_absence():
    ns = _load()
    apply_muted = ns["_apply_ping_muted_kinds"]
    rec = {"name": "ada"}
    assert apply_muted(rec, {"mention": False, "nonsense": False}) is True
    assert rec["ping_muted_kinds"] == ["mention"]
    assert apply_muted(rec, {"mention": False}) is False
    assert apply_muted(rec, {"host_online": False}) is True
    assert ns["ping_muted_kinds"](rec) == ["host_online", "mention"]
    assert apply_muted(rec, {"mention": True, "host_online": True}) is True
    assert "ping_muted_kinds" not in rec
    assert apply_muted(rec, ["tip_sent", "bogus"]) is True
    assert rec["ping_muted_kinds"] == ["tip_sent"]
    assert apply_muted(rec, "tip_sent") is False


def test_quiet_hours_window_crosses_midnight_in_the_account_zone():
    ns = _load()
    rec = {"notification_quiet_hours": {
        "enabled": True, "start": 22 * 60, "end": 7 * 60,
        "days": [0, 1, 2, 3, 4, 5, 6], "timezone": "Europe/Paris"}}
    until = ns["ping_quiet_until"]
    # 12:00 UTC is 14:00 in Paris: not quiet.
    assert until(rec, NOW) == 0
    assert ns["pings_paused"](rec, NOW) is False
    # 21:30 UTC is 23:30 in Paris: quiet until 07:00 Paris = 05:00 UTC.
    late = NOW + int(9.5 * 3600 * 1000)
    expected = int(datetime.datetime(
        2026, 9, 23, 5, 0, tzinfo=datetime.timezone.utc).timestamp() * 1000)
    assert until(rec, late) == expected
    assert ns["pings_paused"](rec, late) is True
    # 04:00 UTC next day is 06:00 Paris: still inside the same window.
    early = NOW + 16 * 3600 * 1000
    assert until(rec, early) == expected


def test_quiet_hours_days_name_the_night_a_window_starts_on():
    ns = _load()
    # Weeknights only (Mon-Thu starts); Tuesday 12:00 UTC is 12:00 in UTC.
    rec = {"notification_quiet_hours": {
        "enabled": True, "start": 9 * 60, "end": 17 * 60, "days": [5, 6],
        "timezone": "UTC"}}
    assert ns["ping_quiet_until"](rec, NOW) == 0  # Tuesday is not a quiet day
    rec["notification_quiet_hours"]["days"] = [1]
    assert ns["ping_quiet_until"](rec, NOW) == NOW + 5 * 3600 * 1000
    # A window that started Monday night (day 0) still covers Tuesday 03:00.
    rec = {"notification_quiet_hours": {
        "enabled": True, "start": 23 * 60, "end": 6 * 60, "days": [0],
        "timezone": "UTC"}}
    tuesday_3am = NOW - 9 * 3600 * 1000
    assert ns["ping_quiet_until"](rec, tuesday_3am) == NOW - 6 * 3600 * 1000
    rec["notification_quiet_hours"]["days"] = [1]
    assert ns["ping_quiet_until"](rec, tuesday_3am) == 0


def test_quiet_hours_are_cleaned_merged_and_stored_as_absence_by_default():
    ns = _load()
    apply_quiet = ns["_apply_ping_quiet_hours"]
    rec = {"name": "ada", "profile_timezone": "America/New_York"}
    assert ns["ping_quiet_hours"](rec) == {
        "enabled": False, "start": 22 * 60, "end": 7 * 60,
        "days": [0, 1, 2, 3, 4, 5, 6], "timezone": "America/New_York"}
    assert apply_quiet(rec, {"enabled": True}) is True
    assert rec["notification_quiet_hours"]["enabled"] is True
    assert apply_quiet(rec, {"enabled": True}) is False
    assert apply_quiet(rec, {"start": 5000, "days": [9, 1, 1, "x"]}) is True
    stored = rec["notification_quiet_hours"]
    assert stored["start"] == 22 * 60  # out of range keeps the old value
    assert stored["days"] == [1]
    assert apply_quiet(rec, {"timezone": "bad zone!"}) is False
    # An empty window is no window.
    assert apply_quiet(rec, {"start": 60, "end": 60}) is True
    assert rec["notification_quiet_hours"]["enabled"] is False
    assert apply_quiet(rec, {"start": 22 * 60, "end": 7 * 60,
                             "days": list(range(7))}) is True
    assert "notification_quiet_hours" not in rec


def test_a_switched_off_kind_is_never_filed_and_a_quiet_one_never_buzzes():
    accounts = {"ada": {"name": "ada", "status": "active",
                        "ping_muted_kinds": ["mention"]}}
    runs, pushes = [], []
    ns = _load(accounts, runs, pushes)
    enqueue = ns["enqueue_notification"]
    assert asyncio.run(enqueue(object(), "ada", "mention", "hi")) is False
    assert runs == [] and pushes == []

    assert asyncio.run(enqueue(object(), "ada", "subscribed", "hi")) is True
    assert pushes[-1][2]["title"] == "hi"  # a banner record for APNs

    ns["_PING_SETTINGS_MEMO"].clear()
    accounts["ada"]["notification_pause_until"] = NOW + 3600 * 1000
    assert asyncio.run(enqueue(object(), "ada", "subscribed", "later")) is True
    assert any("INSERT INTO notifications" in sql for sql in runs)
    assert pushes[-1] == ("ada", "pings", None)  # filed, bell moves, no banner


def test_a_machine_node_follows_its_owner_and_a_failed_read_delivers():
    accounts = {
        "ada-laptop": {"name": "ada-laptop", "owner": "ada"},
        "ada": {"name": "ada", "status": "active",
                "ping_muted_kinds": ["host_online"]},
    }
    ns = _load(accounts)
    muted, paused = asyncio.run(
        ns["_ping_delivery_settings"](object(), "ada-laptop", NOW))
    assert muted == {"host_online"} and paused is False

    async def broken(_env, _name):
        raise RuntimeError("d1 down")

    ns["_account_row"] = broken
    ns["_PING_SETTINGS_MEMO"].clear()
    assert asyncio.run(
        ns["_ping_delivery_settings"](object(), "ada", NOW)) == (set(), False)
    assert ns["_PING_SETTINGS_MEMO"] == {}


def test_settings_payload_and_apply_cover_every_switch_on_the_page():
    ns = _load()
    rec = {"name": "ada"}
    assert ns["_apply_ping_settings"](rec, {
        "emailNotifications": False,
        "notificationPreferences": {"mention": False},
        "notificationDelivery": "instant",
        "notificationPauseUntil": NOW + 60_000,
        "pingMutedKinds": {"tip_sent": False},
        "notificationQuietHours": {"enabled": True},
        "marketingEmail": True,
    }) is True
    payload = ns["ping_settings_payload"](rec, False)
    assert payload["marketingEmail"] is True
    assert payload["marketingEmailSince"] == NOW
    assert payload["emailNotifications"] is False
    assert payload["notificationPreferences"]["mention"] is False
    assert payload["notificationDelivery"] == "instant"
    assert payload["notificationPauseUntil"] == NOW + 60_000
    assert payload["pingMutedKinds"] == ["tip_sent"]
    assert payload["notificationQuietHours"]["enabled"] is True
    assert isinstance(payload["pingKindCatalog"], list)
    assert ns["_apply_ping_settings"](rec, {"pingMutedKinds": ["tip_sent"]}) \
        is False


def test_every_write_rail_and_read_carries_the_new_settings():
    profile = ENTRY_TEXT[ENTRY_TEXT.index("async def _account_profile"):]
    heartbeat = ENTRY_TEXT[ENTRY_TEXT.index("async def _account_heartbeat"):
                           ENTRY_TEXT.index("async def _mirroring_owners")]
    handler = ENTRY_TEXT[ENTRY_TEXT.index("async def notifications_handler"):]
    for body in (profile, heartbeat):
        assert '"pingMutedKinds" in data' in body
        assert '"notificationQuietHours" in data' in body
    assert 'response["pingMutedKinds"]' in heartbeat
    assert 'response["notificationQuietUntil"]' in heartbeat
    # The heartbeat signature binds only name and timestamp, so the
    # platform-wide admin matrix never rides it.
    assert "statusMonitorAlerts" not in heartbeat
    assert 'params.get("settings", [""])[0] == "1"' in handler
    assert 'isinstance(ping_settings, dict)' in handler
    # Admin matrix writes are refused to everyone else.
    settings_post = handler[handler.index('data.get("pingSettings")'):]
    settings_post = settings_post[:settings_post.index('if "pauseUntil" in data')]
    assert 'monitor_alerts is not None and not is_admin' in settings_post
    assert '"forbidden"' in settings_post
    # Session-only, like the pause: a replayed signed mark-read never mutes.
    assert "_authed_account_name(env, request, data) != node" in settings_post
    assert "payload.update(ping_settings_payload(rec, is_admin))" in ENTRY_TEXT


def test_desktop_catalog_mirrors_the_relay_catalog():
    ns = _load()
    header = (ROOT.parent / "desktop" / "src" / "PingSettings.h").read_text(
        encoding="utf-8")
    rows = re.findall(
        r'\{"([a-z_]+)", "([a-z]+)",\s*"([^"]+)",\s*(true|false),\s*'
        r'(true|false)\}', header)
    desktop = {kind: (group, label, email == "true", default == "true")
               for kind, group, label, email, default in rows}
    relay = {kind: (group, label, kind in ns["NOTIFICATION_EMAIL_DEFAULTS"],
                    bool(ns["NOTIFICATION_EMAIL_DEFAULTS"].get(kind)))
             for kind, group, label in ns["PING_KIND_CATALOG"]}
    assert desktop == relay
    for key, label in ns["PING_KIND_GROUPS"]:
        assert '{"%s", "%s"}' % (key, label) in header


if __name__ == "__main__":
    for name, test in list(globals().items()):
        if name.startswith("test_") and callable(test):
            test()
            print("PASS", name)

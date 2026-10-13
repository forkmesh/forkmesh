#!/usr/bin/env python3
"""Operational status alerts are available in the encrypted Pings inbox."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("admin_console.py").read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("status_monitoring.py").read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
)


def test_operational_pings_are_a_first_class_default_on_channel():
    defaults = ENTRY_TEXT.split("REPO_ALERT_SETTING_DEFAULTS = {", 1)[1] \
        .split("}", 1)[0]
    kinds = ENTRY_TEXT.split("NOTIFICATION_KINDS = frozenset({", 1)[1] \
        .split("})", 1)[0]
    assert '"statusPings": True' in defaults
    assert '"statusEmails": False' in defaults
    assert '"operational_alert"' in kinds
    assert "async def _enqueue_operational_alert_pings" in ENTRY_TEXT


def test_both_operational_monitor_paths_enqueue_pings():
    watchdog = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _send_cron_watchdog_email"):
        ENTRY_TEXT.index("async def _cron_watchdog_completion")
    ]
    transitions = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _record_status_monitor_transitions"):
        ENTRY_TEXT.index("\n\nasync def record_status_sample")
    ]
    assert "await _enqueue_operational_alert_pings(env, [alert], now)" in watchdog
    assert (
        "await _enqueue_operational_alert_pings(env, ping_pending, now)"
        in transitions)
    assert 'href="/status"' in ENTRY_TEXT
    assert 'source="operational-status"' in ENTRY_TEXT


def test_ping_channel_tracks_its_own_delivered_transition():
    """Outage pings must not be crowded out by repeated recovery pings.

    Pings default on and mail defaults off, so a ping-only deployment gets no
    notified_state updates. Without an independent marker every later sample
    re-announced the last transition, and recoveries keyed off a stamp that
    moved each minute, so duplicated "recovered" rows trimmed the outage pings
    out of the capped inbox.
    """
    transitions = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _record_status_monitor_transitions"):
        ENTRY_TEXT.index("\n\nasync def record_status_sample")
    ]
    enqueue = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _enqueue_operational_alert_pings"):
        ENTRY_TEXT.index("async def _repository_monitor_admin_emails")
    ]
    assert "pinged_state TEXT NOT NULL DEFAULT ''" in ENTRY_TEXT
    assert 'prior_pinged != "down"' in transitions
    assert "should_ping = pinged != state" in transitions
    assert "await _record_operational_alert_pings_sent(env, ping_pending)" \
        in transitions
    assert 'alert.get("changed_at")' in enqueue
    assert "UPDATE repository_monitor_state SET pinged_state=?" in ENTRY_TEXT


def test_monitor_transition_upserts_stay_under_the_d1_parameter_limit():
    transitions = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _record_status_monitor_transitions"):
        ENTRY_TEXT.index("\n\nasync def record_status_sample")
    ]
    upsert = transitions[
        transitions.index("INSERT INTO repository_monitor_state"):
        transitions.index("# A fleet-wide transition")
    ]
    # JSON1 carries the full supported roster in one bind, instead of either
    # crossing D1's 100-parameter limit or issuing several write statements.
    assert "FROM json_each(?) WHERE 1" in upsert
    assert "json.dumps(changed_rows" in upsert
    assert 'separators=(",", ":")' in upsert
    assert upsert.count("?") == 1
    assert "*batch" not in upsert


def test_admin_setting_exposes_independent_ping_and_email_controls():
    # The matrix moved from the admin console to the Pings settings page
    # (#347): one saver merges per-monitor, per-channel switches.
    module = (ROOT / "src" / "ping_settings.py").read_text(encoding="utf-8")
    saver = module[module.index("async def _save_status_monitor_alerts"):]
    assert 'STATUS_MONITOR_ALERT_CHANNELS = ("pings", "emails", "continual")' \
        in module
    assert 'updated["statusMonitors"] = monitors' in saver
    assert "_audit_sensitive_action(" in saver

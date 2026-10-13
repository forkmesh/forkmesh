#!/usr/bin/env python3
"""Consolidated /api/poll status digest remains available for compatibility,
but the dashboard no longer runs a periodic client poll."""
from pathlib import Path
import re

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
ENTRY_TEXT = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8") + "\n"
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
# dashboard.js is built from ordered public/dashboard/js/*.js fragments before
# deploy (see src/dashboard_bundle.py).
DASHBOARD_JS = assembled_dashboard_js()


def test_worker_registers_poll_route_and_handler():
    assert 'url.path in ("/api/poll", "/api/poll/")' in ENTRY_TEXT
    assert "return await poll_handler(self.env, request)" in ENTRY_TEXT
    assert "async def poll_handler(env, request):" in ENTRY_TEXT


def test_poll_handler_returns_cheap_change_tokens():
    # Grab just the handler body.
    start = ENTRY_TEXT.index("async def poll_handler(env, request):")
    body = ENTRY_TEXT[start:start + 2600]
    # Build rev + profile + notification digests, all in one response.
    assert '"rev": _build_rev(env)' in body
    assert '"profile"' in body
    assert '"notif"' in body
    # Notifications are summarised with an aggregate query, NOT by decrypting
    # every row (that was the per-request cost we are removing from the tick).
    assert "COUNT(*)" in body and "MAX(ts)" in body
    assert "decrypt_row" not in body


def test_version_endpoint_shares_build_rev_helper():
    assert "def _build_rev(env):" in ENTRY_TEXT
    assert '"rev": _build_rev(self.env)' in ENTRY_TEXT


def test_dashboard_does_not_run_periodic_profile_or_notification_polling():
    assert "async function pollStatus(" not in DASHBOARD_JS
    assert "`/api/poll?node=${encodeURIComponent(node)}`" not in DASHBOARD_JS
    assert "function startProfileSync()" not in DASHBOARD_JS
    assert not re.search(
        r"(?:window\.)?setInterval\(\s*(?:loadNotifications|"
        r"hydrateCanonicalProfile|refreshPublicProfile)\b",
        DASHBOARD_JS,
    )
    assert "canonicalProfileReady = hydrateCanonicalProfile(session)" in DASHBOARD_JS
    assert (
        "if (!force && Number(session?.accountSnapshotVersion) >= 1) return session;"
        in DASHBOARD_JS
    )
    assert "return loadNotifications();" in DASHBOARD_JS


def test_session_validation_is_event_driven_not_periodic():
    """The 20s session poll is gone; the account event socket owns liveness.

    It was the most-requested endpoint on the relay (370 hits in a 21.5h
    network-log window, 46% of all browser traffic) purely to learn "still
    signed in". Revocation now surfaces on the event channel's ticket
    handshake, and the only reads left are floor-limited focus checks.
    """
    session_watch = DASHBOARD_JS.split(
        "function startAccountSessionWatch()", 1
    )[1].split("function escapeHtml", 1)[0]
    assert 'fetch("/api/accounts/sessions"' in session_watch
    assert "if (response.status === 401) logout();" in session_watch
    # No timer of any period may reintroduce the poll.
    assert not re.search(r"setInterval\(\s*validate\b", session_watch)
    assert "startAccountEventChannel(" in session_watch
    assert "hydrateCanonicalProfile" not in session_watch
    assert "/api/poll" not in session_watch


def test_focus_revalidation_is_floor_limited():
    """Focus/visibilitychange bursts must not become the poll again."""
    session_watch = DASHBOARD_JS.split(
        "function startAccountSessionWatch()", 1
    )[1].split("function escapeHtml", 1)[0]
    assert "SESSION_REVALIDATE_MIN_MS" in DASHBOARD_JS
    assert "const SESSION_REVALIDATE_MIN_MS = 5 * 60 * 1000;" in DASHBOARD_JS
    assert "Date.now() - lastValidatedMs < SESSION_REVALIDATE_MIN_MS" in (
        session_watch
    )
    # A failed request must stay eligible instead of arming the floor.
    assert "else lastValidatedMs = Date.now();" in session_watch


def test_dashboard_joins_the_account_push_channel():
    """The dashboard was the last signed-in surface with no socket."""
    channel = DASHBOARD_JS.split(
        "function startAccountEventChannel(", 1
    )[1].split("\n  }\n", 1)[0]
    assert 'import("/account-events.js")' in channel
    assert "createAccountEventChannel({" in channel
    assert "onSignedOut: revalidate," in channel
    # Pushes refresh the ping surface, so no notification timer is ever needed.
    assert 'topic === "pings"' in channel
    assert "loadNotifications()" in channel

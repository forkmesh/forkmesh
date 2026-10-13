#!/usr/bin/env python3
"""Static contracts for the settings "Nodes & devices" panel.

The web dashboard is the source of truth for every node key bound to an
account (``www/docs/mobile/mobile-node-contract.md`` section 4). These
assertions pin the DOM hooks and the API calls the panel makes so the
Worker routes and the dashboard script cannot drift apart silently.
"""

from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js, built_dashboard_js
from _dashboard_shell import assembled_dashboard_page


ROOT = Path(__file__).resolve().parents[1]
SETTINGS_VIEW = ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
ACCOUNT_FRAGMENT = ROOT / "public" / "dashboard" / "js" / "04-account.js"


def _settings_panel():
    html = SETTINGS_VIEW.read_text(encoding="utf-8")
    start = html.index("Nodes &amp; devices")
    end = html.index("Active sessions", start)
    return html[start:end]


def test_settings_view_exposes_devices_panel_hooks():
    panel = _settings_panel()
    for hook in (
        "data-account-devices-revoke-others",
        "data-account-devices-status",
        "data-account-devices-list",
        "data-account-devices-reauth",
        "data-account-devices-reauth-title",
        "data-account-devices-reauth-password",
        "data-account-devices-reauth-totp",
        "data-account-devices-reauth-confirm",
        "data-account-devices-reauth-cancel",
    ):
        assert hook in panel, hook
    # Loading state is authored so the card is never blank before the fetch.
    assert "Loading nodes and devices" in panel
    # Accessibility: status text is announced; buttons never submit by accident.
    assert 'data-account-devices-status role="status" aria-live="polite"' in panel
    assert 'data-account-devices-revoke-others type="button"' in panel
    assert 'data-account-devices-reauth-cancel type="button"' in panel
    # Re-auth form stays hidden until a revoke is requested and collects the
    # sudo credentials without autofilling a stale password.
    assert "data-account-devices-reauth hidden" in panel
    assert 'data-account-devices-reauth-password type="password" autocomplete="current-password" required' in panel
    assert 'data-account-devices-reauth-totp type="text" inputmode="numeric" autocomplete="one-time-code"' in panel
    # Copy explains the sudo gate and what revocation does.
    assert "Revoking asks for your password again" in panel


def test_devices_panel_ships_in_composed_settings_page_above_sessions():
    page = assembled_dashboard_page("settings")
    devices_at = page.index("data-account-devices-list")
    sessions_at = page.index("data-account-session-list")
    assert devices_at < sessions_at
    # The built static asset mirrors the partial.
    built = (ROOT / "public" / "dashboard" / "settings" / "index.html").read_text(
        encoding="utf-8")
    assert "data-account-devices-list" in built
    assert "data-account-devices-reauth-password" in built


def test_account_fragment_calls_device_registry_routes():
    fragment = ACCOUNT_FRAGMENT.read_text(encoding="utf-8")
    assert 'fetch("/api/accounts/devices" + path' in fragment
    assert 'accountDeviceApi("GET")' in fragment
    assert 'accountDeviceApi("POST", "/revoke-all", body)' in fragment
    assert '"DELETE", "/" + encodeURIComponent(action.id), body' in fragment
    # Sudo re-auth body: password always, TOTP only when entered.
    assert "data-account-devices-reauth-password" in fragment
    assert "data-account-devices-reauth-totp" in fragment
    assert "const body = { password };" in fragment
    assert "if (totp) body.totp = totp;" in fragment
    assert 'code === "reauth_required"' in fragment
    # Revoking the node bound to this session signs the browser out like the
    # sessions UI does; revoke-all also refreshes the sessions list.
    assert "result?.currentRevoked" in fragment
    assert "loadAccountSessions({ force: true })" in fragment
    # Per-row rendering contract.
    for marker in (
        "data-account-device-revoke",
        "data-account-device-row",
        '"Unnamed device"',
        "This device",
        ">Revoked</span>",
        'desktop_node: "Desktop node"',
        'mobile_node: "Mobile node"',
        '"signs as owner"',
        "formatTimeAgo(lastSeen)",
        # The device id is shown so a row can be matched to the id the
        # mobile app lists under Settings -> This device (issue #721).
        "Device ID <span data-account-device-id",
        "${escapeHtml(device.id)}</span>",
        "function setAccountDevicesStatus(",
    ):
        assert marker in fragment, marker
    # Panel loads with the rest of the Account tab.
    assert "loadAccountDevices({ force: true });" in fragment


def test_assembled_dashboard_bundle_includes_devices_panel():
    js = assembled_dashboard_js()
    assert "/api/accounts/devices" in js
    assert "async function loadAccountDevices(" in js
    # The built static bundle is regenerated from the fragments.
    built = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert built == built_dashboard_js()
    assert "async function loadAccountDevices(" in built

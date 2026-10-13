"""Revoked devices are booted and account-scoped browser state is cleared."""

from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
DASHBOARD = assembled_dashboard_js()
HEADER = (ROOT.parent / "www/public/site-header.js").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
ENTRY = (ROOT / "src/entry.py").read_text(encoding="utf-8")


def test_logout_revokes_the_server_session_and_clears_client_storage():
    for source in (DASHBOARD, HEADER, WORLD):
        assert '"/api/accounts/logout"' in source
        assert "forkmesh_session=; Path=/; Max-Age=0" in source
        assert "forkmesh_account=; Path=/; Max-Age=0" in source
        assert "forkmesh_admin=; Path=/; Max-Age=0" in source
        assert "localStorage.length" in source
        assert "sessionStorage.length" in source
        assert "caches.keys()" in source
    assert "state.fetchJsonCache = {}" in DASHBOARD
    assert "this.responseCache.clear()" in WORLD
    assert "_account_revoke_sessions(env, account_bi, session_id)" in ENTRY
    assert "_clear_account_session_cookie()" in ENTRY


def test_revoked_devices_poll_authoritative_session_state_and_boot_on_401():
    for source in (DASHBOARD, HEADER, WORLD):
        assert 'fetch("/api/accounts/sessions"' in source
        assert 'cache: "no-store"' in source
        assert "response.status === 401" in source
    assert "startAccountSessionWatch()" in DASHBOARD
    assert "startSessionWatch(renderAccountAreas)" in HEADER
    assert "validateActiveWorldSession()" in WORLD
    assert 'new BroadcastChannel("forkmesh.session")' in DASHBOARD
    assert 'new BroadcastChannel("forkmesh.session")' in HEADER

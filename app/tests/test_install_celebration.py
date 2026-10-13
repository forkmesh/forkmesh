"""Installer-completion celebration contracts (adhoc: fireworks + Ping).

A successful one-line-installer run (the `done` diag with ok=1) must Ping the
platform admins and surface on /api/world/installs so the World runs its
firework show — without leaking anything beyond the anonymous coarse platform
tokens install_diag already stores.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src/entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src/app_runtime.py").read_text(encoding="utf-8")
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src/entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
APP = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")


def test_successful_done_diag_triggers_admin_pings():
    handler = ENTRY.split("async def install_diag_handler", 1)[1][:4000]
    assert 'fields[1] == "done" and fields[2] == 1' in handler
    assert "_enqueue_install_celebration_pings" in handler

    pings = ENTRY.split(
        "async def _enqueue_install_celebration_pings", 1)[1][:2500]
    # One Ping per admin per installer run, deduplicated on the run id.
    assert "SELECT data FROM users WHERE is_admin=1 LIMIT 20" in pings
    assert '"operational_alert"' in pings
    assert '"install-celebration:" + run' in pings
    assert 'href="/world/"' in pings


def test_world_installs_endpoint_serves_only_fresh_anonymous_installs():
    assert '"/api/world/installs"' in APP_RUNTIME
    assert "world_recent_installs_handler" in ENTRY
    handler = ENTRY.split(
        "async def world_recent_installs_handler", 1)[1]
    handler = handler.split("\nasync def ", 1)[0]
    # Only whole-install successes, bounded to the ten-minute window.
    assert "WHERE step='done' AND ok=1 AND ts>=?" in handler
    assert "WORLD_INSTALL_CELEBRATION_MS = 10 * 60 * 1000" in ENTRY
    # The installer's random run id is hashed before leaving the Worker.
    assert '"forkmesh-world-install-v1\\0" + run' in handler
    # Coarse platform tokens only — never pm/distro/version/detail columns.
    assert "MAX(os) AS os, MAX(arch) AS arch" in handler
    assert "distro" not in handler
    assert '"public, max-age=30"' in handler


def test_world_runs_the_firework_show_for_a_fresh_desktop_install():
    assert "/api/world/installs?refresh=" in APP
    celebration = APP.split("celebrateRecentInstall(installs = [])", 1)[1][:4000]
    # Keep a long enough discovery window for polling, but end the install
    # fireworks after ten seconds and do not replay them on the next poll.
    assert "const installFreshnessMs = 10 * 60 * 1000;" in celebration
    assert "const fireworksDurationMs = 10 * 1000;" in celebration
    assert "this.lastCelebratedInstallId === newest.id" in celebration
    assert "this.lastCelebratedInstallId = newest.id;" in celebration
    assert "}, fireworksDurationMs);" in celebration
    # Install and federation celebrations continue to share the same styles.
    assert "world-instance-fireworks" in celebration
    assert "world-instance-celebration" in celebration
    assert "data-world-install-celebration" in celebration
    assert "prefers-reduced-motion" in celebration
    assert "world-instance-announcement" not in celebration
    assert 'layer.setAttribute("aria-hidden", "true");' in celebration
    assert "A new ForkMesh desktop was just installed" in celebration
    assert "this.notifyChatArea(" in celebration
    # The federated-instance celebration keeps priority over the overlay.
    assert "data-world-instance-celebration" in celebration
    # Timer hygiene: reset in the constructor, cleared on teardown.
    assert "this.installCelebrationTimer = 0;" in APP
    assert 'this.lastCelebratedInstallId = "";' in APP
    assert "window.clearTimeout(this.installCelebrationTimer);" in APP

"""`GET /api/accounts/sessions` is asked once per reason, not once per tick.

The route is the one every signed-in surface shares, so any client that asks it
on a timer, or again on every navigation, multiplies straight into Worker
requests and D1 reads on a plan where an exhausted isolate fails whoever shares
it. These pins keep the 2026-09-18 pass from regressing; the inventory lives in
docs/operations/polling-elimination.md.
"""

from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORLD = (REPO / "world" / "public" / "world" / "world.js").read_text(
    encoding="utf-8")
API_CLIENT = (REPO / "www" / "public" / "api-client.js").read_text(
    encoding="utf-8")
SITE_HEADER = (REPO / "www" / "public" / "site-header.js").read_text(
    encoding="utf-8")
HOME_AUTH = (REPO / "www" / "public" / "home-header-auth.js").read_text(
    encoding="utf-8")
QT_MAIN = (REPO / "desktop" / "src" / "MainWindow.cpp").read_text(
    encoding="utf-8")
QT_CHAT = (REPO / "desktop" / "src" / "MainWindowChat.cpp").read_text(
    encoding="utf-8")
QT_NOTES = (REPO / "marketplace" / "packages" / "notes" / "content" / "desktop"
            / "NotesPage.cpp").read_text(
    encoding="utf-8")
DOC = (REPO / "www" / "docs" / "operations" / "polling-elimination.md").read_text(
    encoding="utf-8")


def test_world_has_no_session_timer():
    assert "sessionWatchTimer" not in WORLD
    watch = WORLD[
        WORLD.index("  async validateActiveWorldSession("):
        WORLD.index("\n  toggleSettings(")
    ]
    # Floor-limited on the way back to a hidden tab; forced only by the
    # account event channel's authoritative refusal.
    assert "WORLD_SESSION_REVALIDATE_MIN_MS" in watch
    assert "const WORLD_SESSION_REVALIDATE_MIN_MS = 5 * 60 * 1000;" in WORLD
    assert "this.sessionValidatedAt = Date.now();" in watch
    assert (
        "onSignedOut: () => void this.validateActiveWorldSession({ force: true })"
        in WORLD
    )


def test_world_short_pollers_stay_slow_and_visible_only():
    assert "ADMIN_ERROR_POLL_MS" not in WORLD
    assert "const WORLD_AGENT_BOT_POLL_MS = 30 * 1000;" in WORLD
    deposit = WORLD[
        WORLD.index("  startElementDepositPolling() {"):
        WORLD.index("  stopElementDepositPolling() {")
    ]
    assert "if (document.hidden) return;" in deposit


def test_a_signed_out_visitor_is_not_re_asked_on_every_page():
    restore = API_CLIENT[
        API_CLIENT.index("  function restoreSession() {"):
        API_CLIENT.index("  const api = {")
    ]
    assert (
        "recentRestoreMiss() && !arrivedFromAnotherOrigin() && !explicitReload()"
        in restore
    )
    # Only the relay's own 401 is remembered, never an outage.
    missed = restore.index("response.status === 401")
    assert "sessionStorage.setItem(RESTORE_MISS_KEY" in restore[missed:missed + 320]
    assert restore.index("RESTORE_MISS_KEY") < restore.index("if (!response.ok)")
    # A successful restore files the relay's projection for the headers.
    assert '"forkmesh.membershipProjection.v1"' in restore
    assert "membership: account.membership || null" in restore


def test_headers_do_not_force_the_session_read_on_load():
    for source in (SITE_HEADER, HOME_AUTH):
        # The focus/visibility watch is the one forced caller left.
        assert source.count("fetchMembershipProjection(true)") == 1
        assert "function adoptRestoredSession(" in source
    watch = SITE_HEADER[
        SITE_HEADER.index("  function startSessionWatch("):
        SITE_HEADER.index('window.addEventListener("focus", validate);')
    ]
    assert "fetchMembershipProjection(true)" in watch


def test_desktop_status_and_note_reads_are_not_timed():
    # The minute tick repaints local state only: no status read or page probe
    # rides it, and the latency probe only while the desktop has switched
    # itself offline (so it notices the relay coming back).
    tick = QT_MAIN[
        QT_MAIN.index("m_relayLatencyTimer = new QTimer(this);"):
        QT_MAIN.index("m_relayLatencyTimer->start(")
    ]
    for network_call in (
        "refreshFooterWebsiteStatus",
        "refreshDesktopWebsiteProbes();",
    ):
        assert network_call not in tick
    assert tick.count("probeRelayLatency();") <= 1
    if "probeRelayLatency();" in tick:
        guarded = "if (m_offlineMode && m_offlineModeAuto)\n            probeRelayLatency();"
        assert guarded in tick, "an online desktop must not probe the relay every minute"
    # Activation re-reads behind a freshness floor instead.
    assert "void MainWindow::refreshFooterStatusOnActivation()" in QT_CHAT
    assert "kFooterStatusHealthyFloorMs = 15 * 60 * 1000;" in QT_CHAT
    assert "OnTick" not in QT_CHAT
    activation = QT_MAIN[QT_MAIN.index("void MainWindow::changeEvent("):]
    assert "refreshFooterStatusOnActivation();" in activation[:1500]
    # Notes: read on open and on Refresh.
    assert "m_noteRefreshTimer" not in QT_NOTES
    assert 'new QPushButton(QStringLiteral("Refresh"))' in QT_NOTES


def test_the_inventory_records_this_pass():
    for named in (
        "validateActiveWorldSession",
        "forkmesh.sessionRestoreMiss",
        "PORTABLE_SESSION_REISSUE_MS",
        "refreshFooterStatusOnActivation",
        "ADMIN_ERROR_POLL_MS",
        "WORLD_AGENT_BOT_POLL_MS",
    ):
        assert named in DOC

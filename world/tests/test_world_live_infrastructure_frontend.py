#!/usr/bin/env python3
"""Regressions for truthful, low-impact World infrastructure displays."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
APP = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _between(start, end):
    return APP[APP.index(start):APP.index(end, APP.index(start))]


def test_system_capacity_uses_current_service_limits_and_admin_table_counts():
    assert "worldConnections = Number(context?.worldConnections)" in APP
    assert (
        "worldMessagesPerSecond = Number(context?.worldMessagesPerSecond)"
        in APP
    )
    assert "chatConnections = Number(context?.chatConnections)" in APP
    metrics = _between(
        "  updateSystemCapacityMetrics() {",
        "  async moderateWorldPeer(action) {",
    )
    assert "this.worldLimits" in metrics
    assert "this.network?.stats?.clients" in metrics
    assert "Boolean(this.serverPeerId)" in metrics
    assert "1 + this.remotePlayers.size" in metrics
    assert "limits.worldConnections" in metrics
    assert "limits.chatConnections" in metrics
    assert "this.systemCapacityTables" in metrics
    assert "this.systemCapacityDatabase" in metrics
    assert "this.world.updateSystemCapacity({" in metrics
    assert "tables: this.systemCapacityTables" in metrics
    assert "database: this.systemCapacityDatabase" in metrics
    assert "Math.random" not in metrics
    assert "fetch(" not in metrics
    assert "fetchJSON" not in metrics


def test_d1_table_display_shows_storage_against_free_and_paid_limits():
    assert "ticket?.systemCapacity?.d1Storage" in APP
    assert "freeDatabaseLimitBytes" in APP
    assert "paidDatabaseLimitBytes" in APP
    assert "Free per-database limit" in APP
    assert "Paid per-database limit" in APP
    assert 'storageLabel.name = "system-capacity-d1-storage"' in SCENE
    assert "`D1 STORAGE ${formatCapacityBytes(databaseBytes)}`" in SCENE


def test_infrastructure_display_adds_no_durable_object_polling():
    # Context is a small global read. The cached network overview has one
    # shared fetch helper, reached only through the fountain's proximity load
    # or an explicit full refresh; rendering values creates no other poll.
    assert APP.count('this.fetchJSON("/api/world/context"') == 1
    assert APP.count('this.fetchJSON("/api/network/overview"') == 1
    for method in (
        "startEventPolling",
        # Pings are pushed now, not polled (adhoc #1604), but the starter is
        # still the place a stray bootstrap read would get bolted on.
        "startNotificationChannel",
        "startMediaPlaybackPolling",
    ):
        section = _between(f"  {method}(", "\n  }")
        assert "/api/world/context" not in section
        assert "/api/network/overview" not in section


def test_treasury_balance_refreshes_on_hover_instead_of_on_a_timer():
    # Every /api/accounts/central-fund view costs a public Solana RPC round
    # trip, so the SOL board keeps the balance it is already painted with
    # until somebody points at it. No timer may fetch it.
    assert "WORLD_REWARD_POLL_MS" not in APP
    hover = _between("  async refreshRewardStateOnHover() {", "\n  }")
    assert "now - this.rewardHoverRefreshedAt < WORLD_REWARD_HOVER_MS" in hover
    assert "this.refreshRewardState({ force: true })" in hover
    assert "onRewardBoardHover: () =>" in APP
    # The hover itself is reported by the scene, from the treasury sign only.
    assert "onRewardBoardHover = () => {}," in SCENE
    assert "function updateRewardBoardHover(event, now) {" in SCENE
    assert 'landmarkObjects.get("fountain")?.userData?.treasurySign' in SCENE
    assert "if (hovered) onRewardBoardHover();" in SCENE


def test_system_status_has_no_builtin_world_runtime():
    css = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(
        encoding="utf-8"
    )
    assert 'this.fetchJSON("/api/status/world"' not in APP
    assert "refreshSystemStatusBoard" not in APP
    assert "statusBoardTimer" not in APP
    assert 'message.type === "status-update"' not in APP
    assert "data-world-admin-status-light" not in APP
    assert "world-admin-status-light" not in css
    assert "systemStatusBannerTexture" not in SCENE
    assert "updateSystemStatusBoard" not in SCENE

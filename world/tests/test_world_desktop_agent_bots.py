"""Contracts for Qt-launched agent droids inside the playable World."""

from pathlib import Path
from world_scene_source import world_scene_source


# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
TASKS = (WORLD / "world-office-tasks.js").read_text(encoding="utf-8")
QT_AGENTS = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)


def test_all_six_model_symbols_are_world_assets():
    for model in ("sol", "luna", "terra", "opus", "fable", "sonnet"):
        asset = WORLD / "assets" / f"agent-model-{model}.webp"
        assert asset.is_file() and asset.stat().st_size > 1000
        assert f'agent-model-{model}.webp' in SCENE
    for label in ("Sol", "Luna", "Terra", "Opus", "Fable", "Sonnet"):
        assert f'label: "{label}"' in SCENE


def test_task_sync_carries_model_effort_and_live_qt_status_into_world():
    # One batched write for the whole fleet, not one request per session: the
    # per-session burst on restart was rate-limited by the relay (adhoc #1618).
    assert 'QStringLiteral("/api/tasks/agent-status")' in QT_AGENTS
    assert 'kOrgTaskAgentStatusBatchProof' in QT_AGENTS
    assert 'QStringLiteral("statuses"), statuses' in QT_AGENTS
    assert '{QStringLiteral("status"), session.status}' in QT_AGENTS
    assert 'world.updateDesktopAgentBots?.(' in TASKS
    assert 'async refreshDesktopAgentBots()' in APP
    assert 'this.fetchJSON("/api/tasks"' in APP
    assert 'WORLD_AGENT_BOT_POLL_MS' in APP
    for field in ("model", "strength", "status"):
        assert f"task.agent.{field}" in TASKS


def test_world_droids_drop_blink_and_show_status_labels():
    assert "function createDesktopAgentRobot" in SCENE
    assert "DESKTOP_AGENT_DROP_HEIGHT" in SCENE
    assert "DESKTOP_AGENT_DROP_MS" in SCENE
    assert "Math.pow(1 - progress, 3)" in SCENE
    assert '`${effort} effort · ${status.label}`' in SCENE
    assert "light.material.emissiveIntensity" in SCENE
    for state, label in (
        ("queued", "Queued"),
        ("running", "Working"),
        ("waiting", "Waiting"),
        ("success", "Done"),
        ("failed", "Failed"),
        ("stopped", "Stopped"),
    ):
        assert f'{state}: Object.freeze({{ label: "{label}"' in SCENE


def test_world_summon_button_drops_the_fleet_into_a_grid():
    assert "data-world-agent-summon" in APP
    assert "summonDesktopAgentBots" in APP
    assert "function summonDesktopAgentBots" in SCENE
    assert "DESKTOP_AGENT_GRID_SPACING" in SCENE
    assert "desktopAgentBotCount" in SCENE

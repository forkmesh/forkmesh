from pathlib import Path

from world_scene_source import world_scene_source


SCENE = world_scene_source()
WORLD = (
    Path(__file__).resolve().parents[1] / "public/world/world.js"
).read_text(encoding="utf-8")


def test_what_we_are_building_sign_is_not_built():
    for removed in (
        "FORKMESH · WHAT WE'RE BUILDING",
        "DONE + DEPLOYED",
        "WORLD_TASK_BULLETIN_ITEMS",
        "worldTaskBulletinTexture",
        "worldTaskDoneTexture",
        "forkmesh-office-task-bulletin",
        "forkmesh-build-board-video-link",
        "office-task-bulletin",
        "officeTaskBulletin",
        "updateBuildBoard",
        "onBuildBoardNearby",
    ):
        assert removed not in SCENE, removed
    for removed in (
        "/api/world/build-board",
        "refreshBuildBoard",
        "WORLD_BUILD_BOARD_POLL_MS",
        "What we're building",
    ):
        assert removed not in WORLD, removed


def test_human_todo_sign_is_not_built():
    for removed in (
        "HUMAN TODO · FROM CLAUDE + CODEX",
        "worldHumanTodoTexture",
        "humanTodoItemsFromAgentSessions",
        "forkmesh-human-todo-board",
        "repaintHumanTodoBoard",
        "syncXPackageTodo",
    ):
        assert removed not in SCENE, removed
    assert "function updateMirrorAgentTasks" in SCENE
    assert "reconcileSessionAgentBots(sessions)" in SCENE


def test_qa_board_stays():
    assert 'worldQaBoard.name = "forkmesh-world-qa-board"' in SCENE
    assert "function qaBoardHitAction(" in SCENE
    assert '"qa-board", "QA board", "Boards & kiosks"' in SCENE


PUBLIC = Path(__file__).resolve().parents[1] / "public/world"
LOADER = (PUBLIC / "world-loader.js").read_text(encoding="utf-8")
TOWER = (PUBLIC / "world-office-tower.js").read_text(encoding="utf-8")


def test_local_console_board_and_capture_are_gone():
    for removed in (
        "LOCAL CONSOLE",
        "infrastructure-console-switch",
        "infrastructureConsoleTexture",
        "setInfrastructureConsoleLogs",
    ):
        assert removed not in SCENE, removed
    for removed in (
        "createInfrastructureConsoleCapture",
        "setInfrastructureConsoleEnabled",
        "Local console capture enabled",
        "sanitizeInfrastructureConsoleText",
    ):
        assert removed not in WORLD, removed
        assert removed not in LOADER, removed


def test_worker_footprint_boards_and_chart_asset_are_gone():
    assert not (PUBLIC / "worker-footprint.js").exists()
    for removed in (
        "WORKER STARTUP FOOTPRINT",
        "WORKER COMPONENT + FREE PLAN MAP",
        "worker-footprint.js",
        "WORKER_FOOTPRINT",
    ):
        assert removed not in SCENE, removed
        assert removed not in LOADER, removed


def test_infrastructure_floor_is_gone_and_the_roof_moved_down():
    assert 'id: "infrastructure"' not in TOWER
    assert "export const OFFICE_FLOOR_COUNT = 4;" in TOWER
    assert 'id: "rooftop",\n    level: 3,' in TOWER
    assert 'officeFloorGroups.get("infrastructure")' not in SCENE
    assert '"office-infrastructure"' not in SCENE
    assert 'systemCapacityPlatform.userData.officeFloorId = "engineering"' in SCENE


def test_lobby_kiosks_moved_to_the_website_are_gone():
    for removed in (
        "forkmesh-office-feedback-kiosk",
        "forkmesh-office-task-bid-kiosk",
        "forkmesh-office-link-kiosk",
        "forkmesh-office-link-rewards",
        "office-feedback-kiosk",
        "office-task-bid-kiosk",
        "office-link-kiosk",
        "FEEDBACK KIOSK",
        "TASK BOUNTY DESK",
        "LINK LAB",
        "LINK REWARDS",
        "linkSubmissionRewardTexture",
        "updateLobbyLinkBoard",
        "onLobbyLinkKioskNearby",
        "onLobbyFeedbackKioskSelect",
        "onLobbyTaskBidKioskSelect",
        "onLobbyLinkKioskSelect",
    ):
        assert removed not in SCENE, removed
        assert removed not in LOADER, removed
    for removed in (
        "/api/world/link-kiosk",
        '"/api/feedback"',
        "openLobbyFeedbackKiosk",
        "openLobbyTaskBidKiosk",
        "openLobbyLinkKiosk",
        "loadLobbyLinkBoard",
        "data-world-feedback-kiosk",
        "data-world-task-bid",
        "data-world-link-kiosk",
        "openFeedbackKioskOnLoad",
    ):
        assert removed not in WORLD, removed
    assert "/dashboard?feedback=1" in WORLD

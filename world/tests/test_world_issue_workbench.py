"""World issue workbench, mirror task panel, and smooth Office exit contracts."""

from pathlib import Path
import re
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
STYLE = (ROOT.parent / "world/public/world/world.css").read_text(encoding="utf-8")
HEADERS = (ROOT.parent / "www/public/_headers").read_text(encoding="utf-8")


def _function_body(source, name):
    match = re.search(
        rf"\n(?:  )?(?:async )?(?:function )?{re.escape(name)}"
        rf"\([^)]*\)\s*\{{",
        source,
    )
    assert match, f"missing function {name}"
    start = match.start()
    opening = match.end() - 1
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function {name}")


def test_repo_issue_cards_and_board_open_the_full_web_workbench():
    assert "this.openRepositoryIssueWorkbench(meta.repositoryIssuePage)" in WORLD
    workbench = _function_body(WORLD, "openRepositoryIssueWorkbench")
    assert 'this.openRepositoryRecordWebWorkbench("issue", page)' in workbench
    generic = _function_body(WORLD, "openRepositoryRecordWebWorkbench")
    assert '"pulls" : "issues"' in generic
    assert "data-world-repository-web-workbench" in generic
    assert "<iframe" in generic
    assert "loading=\"eager\"" in generic
    assert "referrerpolicy=\"same-origin\"" in generic
    assert "Open tab ↗" in generic
    assert ".world-issue-workbench iframe" in STYLE
    assert '--world-detail-base-width: 920px' in STYLE


def test_record_detail_frame_exceptions_are_narrow_and_same_origin_only():
    for marker in (
        "/:owner/:repo/issues/:number\n",
        "/:owner/:repo/pulls/:number\n",
    ):
        start = HEADERS.index(marker)
        rule = HEADERS[start:].split("\n\n", 1)[0]
        assert "! Content-Security-Policy" in rule
        assert "frame-ancestors 'self'" in rule
        assert "frame-ancestors 'none'" not in rule
        assert "! X-Frame-Options" in rule
        assert "X-Frame-Options: SAMEORIGIN" in rule


def test_mirror_cabinet_sides_show_authenticated_agent_task_states():
    for contract in (
        "MIRROR_AGENT_TASK_STATUS",
        "RUNNING",
        "STOPPED",
        "MERGED",
        "ATTENTION",
        "mirrorAgentTaskPanelTexture",
        '"mirror-server-agent-panel-left"',
        '"mirror-server-agent-panel-right"',
        "function updateMirrorAgentTasks(sessions = [])",
        "this.world?.updateMirrorAgentTasks?.(sessions)",
    ):
        assert contract in SCENE or contract in WORLD
    refresh = _function_body(WORLD, "refreshOrgAgentBots")
    assert refresh.index("this.sessionAuthenticated") < refresh.index(
        "this.world?.updateMirrorAgentTasks?.(sessions)"
    )


def test_office_exit_preserves_heading_instead_of_reversing_the_visitor():
    begin = _function_body(SCENE, "beginOfficeExit")
    leave = _function_body(SCENE, "leaveOfficeInterior")
    assert "avatar.rotation.y =" not in begin
    assert "player.rotation.y = 0" not in begin
    assert "Math.sin(player.rotation.y)" in leave
    assert "Math.cos(player.rotation.y)" in leave
    assert "cameraYaw = 0" not in leave
    assert "cameraZoom = OFFICE_EXIT_CAMERA_ZOOM" in leave
    assert "setCameraMode(\"third-person\", \"office-exit\")" in leave


def test_repository_circle_uses_avatar_orbit_in_the_open_district():
    assert 'icon.scale.set(1.14, 1.14, 1)' in SCENE
    assert "const orbitRadius = 4.25" in SCENE
    assert 'content.name = "repository-district-content"' in SCENE
    assert "function createRepositoryGeodesicDome" not in SCENE
    assert "repository-geodesic-dome" not in SCENE
    assert '"repository-create-button"' not in SCENE
    follower_texture = _function_body(SCENE, "repositoryFollowerIconTexture")
    assert "256, 256" in follower_texture
    assert "drawRepositoryFollowerAvatar" in follower_texture
    assert "fillText" not in follower_texture


def test_repository_records_stay_bounded_in_the_web_workbench_not_the_world_scene():
    assert "const selected = numbered.slice(0, 250)" in WORLD
    assert ".slice(0, 250);" in _function_body(WORLD, "repositoryPullRecords")
    assert "offset += 60" in _function_body(WORLD, "loadRepositoryBlobBatches")
    refresh = _function_body(WORLD, "syncRepositoryScene")
    assert "updateRepositoryRecordDesk" not in refresh

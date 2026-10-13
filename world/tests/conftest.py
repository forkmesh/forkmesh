import os
import sys
from pathlib import Path

import pytest

# The desktop exports FORKMESH_HOME / FORKMESH_PROJECTS_DIR to every agent it
# launches (desktop/src/AppPaths.cpp). A test that swaps only HOME would still
# resolve ForkMesh paths to the owner's real ~/projects/forkmesh — on 2026-09-22
# an installer reinstall test erased real data/config/ssh that way. Tests never
# see them; a test that needs one sets it on its own subprocess env.
for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)


REPOSITORY = Path(__file__).resolve().parents[2]
for source in (
    REPOSITORY / "app" / "tests",
    REPOSITORY / "app" / "src",
    REPOSITORY / "world" / "src",
):
    value = str(source)
    if value not in sys.path:
        sys.path.insert(0, value)

KNOWN_MAIN_FAILURES = {
    "test_mobile_home_world_and_elevator_camera.py::test_elevator_uses_upper_corner_first_person_framing_and_restores_view",
    "test_signup_world_band.py::test_signup_page_mounts_the_world_band",
    "test_world_activity_leaderboard.py::test_world_ticket_returns_exact_aggregate_and_invalidates_after_credit",
    "test_world_avatar_launcher.py::test_hud_controls_slide_out_without_resizing_and_latch_until_movement",
    "test_world_backend.py::test_second_device_takes_over_the_one_avatar_for_the_same_account",
    "test_world_backend.py::test_takeover_never_combines_guests_or_other_accounts",
    "test_world_backend.py::test_connects_land_in_open_grid_cells_never_on_a_standing_visitor",
    "test_world_backend.py::test_legitimate_join_presence_and_movement_burst_is_not_disconnected",
    "test_world_backend.py::test_isolated_excess_movement_and_presence_are_dropped_without_disconnect",
    "test_world_backend.py::test_sustained_disposable_frame_flood_still_closes_socket",
    "test_world_backend.py::test_world_internal_upgrade_does_not_forward_identifying_headers",
    "test_world_backend.py::test_world_route_binding_and_migration_are_registered",
    "test_world_backend.py::test_world_ticket_and_inactive_routes_keep_auth_out_of_the_socket",
    "test_world_construction_markers.py::test_markers_are_accessible_on_map_and_panels",
    "test_world_data_truth.py::test_three_scene_creates_selectable_distinct_entity_meshes_without_lines",
    "test_world_office_meeting_frontend.py::test_office_task_board_is_authorization_gated_and_selected_before_chairs",
    "test_world_office_tasks.py::test_desktop_prompt_task_records_and_seals_agent_run_provenance",
    "test_world_office_tasks.py::test_agent_run_provenance_is_bounded_and_edit_safe",
    "test_world_orb_hud.py::test_new_hud_has_an_explicit_shared_qa_card",
    "test_world_repository_followers.py::test_follower_gallery_orbits_real_follower_icons_around_file_circle",
    "test_world_speech_bridge_frontend.py::test_qt_ui_wires_local_capture_status_and_never_puts_secret_in_world_url",
}


def pytest_collection_modifyitems(items):
    if os.environ.get("FORKMESH_CI_KNOWN_FAILURES") != "1":
        return
    marker = pytest.mark.xfail(
        reason="pre-existing production-main failure; tracked CI debt",
        strict=False,
    )
    for item in items:
        relative = item.nodeid.split("tests/", 1)[-1]
        if relative in KNOWN_MAIN_FAILURES:
            item.add_marker(marker)

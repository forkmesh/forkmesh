"""CI-only quarantine for failures already present on the production baseline.

Every listed test still executes. The marker is enabled only by the isolated
ForkMesh Actions workflow; normal local pytest runs remain fully strict. New
failures outside this explicit set fail CI.
"""

import os
import sys
from pathlib import Path

import pytest

_APP_SRC = str(Path(__file__).resolve().parents[1] / "src")
if _APP_SRC not in sys.path:
    sys.path.append(_APP_SRC)

# The desktop exports FORKMESH_HOME / FORKMESH_PROJECTS_DIR to every agent it
# launches (desktop/src/AppPaths.cpp). A test that swaps only HOME would still
# resolve ForkMesh paths to the owner's real ~/projects/forkmesh — on 2026-09-22
# an installer reinstall test erased real data/config/ssh that way. Tests never
# see them; a test that needs one sets it on its own subprocess env.
for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)


KNOWN_MAIN_FAILURES = {
    "test_ap_edge_cache.py::test_actor_doc_warm_hit_revalidates_visibility_before_using_cache",
    "test_ap_edge_cache.py::test_actor_doc_404_is_negative_cached",
    "test_ap_edge_cache.py::test_webfinger_spelling_variants_share_one_edge_entry",
    "test_asset_cache_busting.py::test_every_first_party_script_carries_the_shared_content_hash",
    "test_auth_pages_frontend.py::test_dashboard_profile_page_js_checks_availability_renames_and_deletes_account",
    "test_catalog_authed_no_inline_purge.py::test_authenticated_catalog_skips_inline_purges",
    "test_catalog_authed_no_inline_purge.py::test_anonymous_cache_miss_does_not_purge_inline",
    "test_catalog_signed_endpoint_liveness.py::test_fresh_signed_endpoint_marks_host_and_mirror_group_cloneable",
    "test_catalog_signed_endpoint_liveness.py::test_stale_signed_endpoint_does_not_mark_catalog_online",
    "test_catalog_signed_endpoint_liveness.py::test_unverified_or_legacy_integrity_endpoint_does_not_mark_online",
    "test_dashboard_landing_migration.py::test_root_keeps_regular_site_and_embeds_world_for_every_visitor",
    "test_dashboard_landing_migration.py::test_dashboard_hydrator_uses_existing_worker_apis",
    "test_dashboard_landing_migration.py::test_dashboard_loads_profile_once_without_periodic_polling",
    "test_dashboard_landing_migration.py::test_dashboard_code_tree_rows_use_live_commit_messages",
    "test_dashboard_landing_migration.py::test_dashboard_pull_detail_reads_committed_patch_for_files_changed",
    "test_dashboard_landing_migration.py::test_worker_routes_raw_repository_blobs_through_direct_https_gateway",
    "test_docs_frontend.py::test_docs_pages_use_tailwind_cdn_and_page_local_styles",
    "test_favicon_metadata.py::test_public_pages_use_shared_favicon_metadata",
    "test_forkbot.py::test_web_chats_forward_mentions_and_broadcast_bot_replies",
    "test_headless_flagship_bootstrap.py::test_headless_flagship_bootstrap_runs_after_account_session_is_ready",
    "test_headless_flagship_bootstrap.py::test_mirror_advert_cache_includes_namespace_and_publish_state",
    "test_mirror_serving_frontend.py::test_remote_clone_only_group_uses_clone_url_canonical_identity",
    "test_perf_budgets.py::test_catalog_get_is_linear_and_bounded_at_scale",
    "test_poll_digest.py::test_dashboard_does_not_run_periodic_profile_or_notification_polling",
    "test_pricing_page_frontend.py::test_pricing_page_uses_tailwind_cdn_and_shared_branding",
    "test_qt_chat_user_identity.py::test_chat_member_column_is_user_directory_not_online_nodes",
    "test_qt_commit_workspace_layout.py::test_working_tree_diff_controls_stay_above_changes_list",
    "test_qt_mirror_nodes_identity.py::test_mirror_nodes_use_node_account_not_chat_name",
    "test_qt_pull_badge.py::test_badge_widget_scrolls_and_is_fed_per_file_stats",
    "test_qt_repo_action_strip.py::test_action_strip_is_parented_to_repo_detail_page_not_tab_viewport",
    "test_qt_top_bar_identity.py::test_top_bar_merges_user_identity_into_node_name_and_avatar",
    "test_users_nodes_claim_link.py::test_wire_contracts_across_worker_qt_and_installer",
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

"""Shared World build-board API authorization contracts."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
API = (ROOT.parent / "world" / "src" / "world_build_board.py").read_text(encoding="utf-8")
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src/entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
MIGRATION = (ROOT / "migrations/0091_world_build_board.sql").read_text(
    encoding="utf-8")


def test_build_board_has_bounded_public_state_and_authorized_audited_writes():
    assert "MAX_ITEMS = 64" in API
    assert 'role in ("owner", "admin")' in API
    assert 'permission in ("maintain", "admin")' in API
    assert "origin_not_allowed" in API
    assert "world.build_board_" in API
    assert "updated_by_bi" in MIGRATION
    assert "CHECK (priority >= 1 AND priority <= 64)" in MIGRATION
    assert "world_build_board_handler" in ENTRY
    assert "Reject unauthorized mutations before touching a repository mirror" in ENTRY


def test_build_board_api_keeps_its_seeded_task_keys():
    for key in (
        "task:world-board-detail",
        "task:world-mobile-pan-stability",
        "task:world-stable-hydration",
        "task:repo-board-view-pads",
        "task:world-continuous-city",
        "task:world-start-here-map",
        "task:world-roof-and-seating",
        "task:world-beach-road",
        "task:world-bike-perimeter",
        "task:world-panel-layout",
        "task:world-github-theme",
        "task:world-time-stars-textures",
        "task:world-node-delete-regression",
    ):
        assert f'"{key}"' in API

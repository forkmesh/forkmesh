#!/usr/bin/env python3
"""Contracts for the authenticated World profile social controls."""

import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)


def _handler_block():
    """The handler's own source, wherever the Worker keeps it."""
    for node in ast.parse(WORLD_ROUTE_SOURCE).body:
        if (isinstance(node, ast.AsyncFunctionDef)
                and node.name == "world_profile_social_handler"):
            return ast.get_source_segment(WORLD_ROUTE_SOURCE, node)
    raise AssertionError("world_profile_social_handler not found")


def _profile_feed_handler_block():
    for node in ast.parse(WORLD_ROUTE_SOURCE).body:
        if (isinstance(node, ast.AsyncFunctionDef)
                and node.name == "world_profile_activity_feed_handler"):
            return ast.get_source_segment(WORLD_ROUTE_SOURCE, node)
    raise AssertionError("world_profile_activity_feed_handler not found")


def test_profile_social_route_is_authenticated_and_self_scoped():
    handler = _handler_block()
    assert '"/api/world/profile-social"' in ENTRY
    assert "_account_session_record(env, request, data)" in handler
    assert '"invalid_session"' in handler
    assert "WHERE target_bi=?" in handler
    assert "account_bi)" in handler
    assert '"avatarPng": follower.get("avatar_png", "")' in handler
    assert '"avatarUrl": ap.public_media_url(' in handler
    assert "LIMIT 24" in handler


def test_profile_update_is_durable_before_delivery_and_carries_alt_text():
    handler = _handler_block()
    object_insert = handler.index("INSERT INTO ap_objects")
    outbox_insert = handler.index("INSERT INTO ap_outbox")
    drain = handler.index("_ap_drain_outbox")
    assert object_insert < outbox_insert < drain
    assert "ap.extract_body_images(" in handler
    assert 'images[0]["name"] = alt_text' in handler
    assert 'item["name"] = image.get("name", "")' in handler
    assert '"kind": "user-update"' in handler
    assert '"world.activitypub.publish"' in handler
    assert "published>=?" in handler
    assert "publish_rate_limited" in handler


def test_profile_update_uses_the_authenticated_user_actor_only():
    handler = _handler_block()
    assert "_ap_user_federates(env, name)" in handler
    assert "_ap_local_actor(env, AP_ACTOR_USER, name, create=True)" in handler
    assert '"actorKind": AP_ACTOR_USER' in handler
    assert '"actorHandle": name' in handler
    assert "data.get(\"name\"" not in handler


def test_profile_feed_exposes_only_public_user_updates():
    handler = _profile_feed_handler_block()
    assert '"/api/world/profile-feed"' in ENTRY
    assert "WHERE a.kind=?" in handler
    assert "AP_ACTOR_USER" in handler
    assert "account.get(\"profile_private\")" in handler
    assert '"access-control-allow-origin": "*"' in handler
    assert '"actor_data"' in handler
    assert "decrypt_row(env, row.get(\"data\"))" in handler

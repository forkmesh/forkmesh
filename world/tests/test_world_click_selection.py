#!/usr/bin/env python3
"""Contracts for click-selected World member avatars."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")


def test_avatar_click_uses_public_identity_and_opens_the_side_panel():
    assert "const avatarSelections = new WeakMap();" in SCENE
    assert "forkmesh-avatar-selection-highlight" in SCENE
    assert "avatarSelections.set(mesh" in SCENE
    assert "onAvatarSelect(avatarSelectionPayload(avatarSelection));" in SCENE
    assert "onAvatarSelect: (member) => this.openWorldMemberDetail(member)" in WORLD
    assert "openWorldMemberDetail(member = {}" in WORLD
    assert 'detail.dataset.openLandmark = "world-member";' in WORLD
    assert "privacy-filtered member, presence, and public profile fields" in WORLD
    # The general member panel must not inherit the administrator-only guest
    # transport details exposed by a different, explicitly privileged control.
    start = WORLD.index("openWorldMemberDetail(member = {}")
    member_panel = WORLD[start:WORLD.index("mirrorNodeActionsHTML(node)", start)]
    assert "ipAddress" not in member_panel
    assert "userAgent" not in member_panel


def test_member_panel_shows_email_verification_and_admin_user_detail_link():
    start = WORLD.index("openWorldMemberDetail(member = {}")
    member_panel = WORLD[start:WORLD.index("mirrorNodeActionsHTML(node)", start)]
    assert "✓ VERIFIED EMAIL" in member_panel
    assert "✕ EMAIL NOT VERIFIED" in member_panel
    assert "world-status-pill-danger" in member_panel
    assert "validWorldSession()?.adminUrl" in member_panel
    assert 'target.searchParams.set("table", "users")' in member_panel
    assert 'target.searchParams.set("user", name)' in member_panel
    assert "Open admin user detail" in member_panel
    assert 'target="_blank" rel="noopener noreferrer"' in member_panel

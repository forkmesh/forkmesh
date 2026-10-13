#!/usr/bin/env python3
"""Activity achievement patches worn by World avatars."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2]
SCENE = world_scene_source()
APP = (ROOT / "world" / "public" / "world" / "world.js").read_text(
    encoding="utf-8")


def test_activity_badges_are_fixed_right_arm_patches_for_real_members():
    for contract in (
        "const AVATAR_ACHIEVEMENT_BADGE_DEFINITIONS = Object.freeze({",
        'qa: { label: "QA"',
        'coder: { label: "CODE"',
        'discord: { label: "DISCORD"',
        'poster: { label: "POST"',
        'chat: { label: "CHAT"',
        "function avatarAchievementBadgeSlugs(identity)",
        'String(identity?.accountStatus || "Guest") === "Guest"',
        "forkmesh-avatar-right-arm-achievement-badges",
        "badge.position.set(0.148, 0.47 - index * 0.105, 0)",
        "badge.rotation.y = Math.PI / 2",
        "function syncAvatarAchievementBadges(THREE, avatar, identity)",
        "avatar.userData.rightArm.add(badges)",
    ):
        assert contract in SCENE, contract


def test_activity_patches_follow_avatar_creation_and_identity_refreshes():
    create_avatar = SCENE[
        SCENE.index("function createAvatar(THREE, identity, options = {})"):
        SCENE.index("function applyAvatarLookDirection")
    ]
    assert "syncAvatarAchievementBadges(THREE, group, identity)" in create_avatar

    update_badge = SCENE[
        SCENE.index("function updateAvatarBadge(THREE, avatar, identity, remote = false)"):
        SCENE.index("function startAvatarWave")
    ]
    assert "syncAvatarAchievementBadges(THREE, avatar, identity)" in update_badge


def test_public_directory_badges_reach_live_office_and_member_circle_avatars():
    assert "function memberAchievementBadges(value)" in APP
    assert "WORLD_AVATAR_ACHIEVEMENT_BADGE_SLUGS" in APP
    assert "achievementBadges: memberAchievementBadges(user?.achievementBadges)" in APP

    for contract in (
        "achievementBadges: Array.isArray(facts.achievementBadges)",
        "achievementBadges: Array.isArray(member?.achievementBadges)",
        "achievementBadges: Array.isArray(member.achievementBadges)",
        "const refreshAvatarMemberFacts = (",
        "refreshAvatarMemberFacts(player, identity, false)",
        "refreshAvatarMemberFacts(officeLobbyPlayer, identity, false)",
        "officeParticipants.forEach((avatar) => refreshAvatarMemberFacts(avatar))",
        "const participantIdentity = withMemberFacts({",
        "const badgeIdentity = withMemberFacts({",
    ):
        assert contract in SCENE, contract

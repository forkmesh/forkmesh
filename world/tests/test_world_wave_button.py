#!/usr/bin/env python3
"""HUD wave button and the avatar arm pose it plays (issue #532)."""

from pathlib import Path
import re
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
STYLES = (WORLD / "world.css").read_text(encoding="utf-8")


def test_avatar_reactions_live_in_the_world_chat_dock():
    for emote in ("wave", "jump", "spin", "backflip", "dance"):
        assert f'data-dashboard-chat-emote="{emote}"' in APP
    assert "data-world-wave" not in APP


def test_the_phone_launcher_reveals_controls_without_overflowing_the_row():
    # Touch starts with the circular avatar launcher only.  The controls fan
    # out in the same fixed-size row after a tap, where the row can scroll
    # rather than pushing the avatar past the viewport edge.
    compact_blocks = STYLES.split("@media (max-width: 720px) {")[1:]
    assert compact_blocks, "compact top-bar media query is missing"
    compact_block = next(
        block
        for block in compact_blocks
        if '.world-hud[data-hud-expanded="true"] .world-top-actions' in block
    )
    assert "width: 0;" in compact_block
    assert ".world-hud[data-hud-expanded=\"true\"] .world-top-actions" in compact_block
    assert "overflow-x: auto;" in compact_block
    assert ".world-hud[data-hud-expanded=\"true\"] .world-top-link" in compact_block


def test_chat_reactions_play_locally_and_broadcast_one_emote():
    wave = APP.split("  sendWorldEmote(rawEmote) {", 1)[1].split(
        "\n  }\n", 1
    )[0]
    # The local arm plays without waiting on the relay.
    assert 'this.world?.playEmote?.(this.identity?.id || "", emote, true);' in wave
    # And everybody else sees it through the existing text-free emote frame.
    assert '{ type: "interaction", kind: "emote", emote }' in wave
    # Holding the button down must not turn one gesture into a frame stream.
    assert "WORLD_WAVE_COOLDOWN_MS" in wave
    assert "const WORLD_WAVE_COOLDOWN_MS = 2000;" in APP


def test_super_jump_is_a_real_building_height_movement_action():
    assert 'data-dashboard-chat-emote="superjump"' in APP
    wave = APP.split("  sendWorldEmote(rawEmote) {", 1)[1].split(
        "\n  }\n", 1
    )[0]
    assert '"superjump",' in wave
    emote = SCENE.split("  function playEmote(", 1)[1].split("\n  }\n", 1)[0]
    assert "jumpVelocity = PLAYER_SUPER_JUMP_VELOCITY;" in emote
    assert "superJumping = true;" in emote
    assert "const PLAYER_SUPER_JUMP_VELOCITY = 82;" in SCENE
    assert "if (!superJumping)" in SCENE


def test_the_wave_lifts_the_right_arm_and_returns_it_to_rest():
    assert "function startAvatarWave(avatar" in SCENE
    # Local clicks and relayed frames both arrive through playEmote.
    emote = SCENE.split("  function playEmote(", 1)[1].split("\n  }\n", 1)[0]
    assert 'if (emote === "wave") startAvatarWave(avatar);' in emote

    activity = SCENE.split("function animateAvatarActivity(", 1)[1].split(
        "\nfunction ", 1
    )[0]
    # The pose is replayed inside animateAvatarActivity, which every walk,
    # sit, and ride path calls after it has written the frame's arm rotations,
    # so the gait cannot overwrite the raised arm.
    assert "avatar.userData.waveStartedAt" in activity
    assert "poseWavingArm(waveArm, rest, lift" in activity
    # It ends by putting the arm back exactly where it hung.
    assert "waveArm.rotation.z = 0;" in activity
    assert "waveArm.position.copy(rest);" in activity

    duration = re.search(r"const AVATAR_WAVE_DURATION_MS = (\d+);", SCENE)
    assert duration and 800 <= int(duration.group(1)) <= 5000


def test_the_shoulder_stays_pinned_while_the_arm_swings():
    pose = SCENE.split("function poseWavingArm(", 1)[1].split("\n}\n", 1)[0]
    # Arm meshes rotate about their own centre, so the mesh has to travel the
    # arc that keeps its shoulder end on the torso.
    assert "halfLength * Math.sin(angle)" in pose
    assert "halfLength * (1 - Math.cos(angle))" in pose


def test_the_button_hand_animation_respects_reduced_motion():
    # The scene-side pose drops its shake under the same preference.
    assert "const shake = reducedMotion" in SCENE

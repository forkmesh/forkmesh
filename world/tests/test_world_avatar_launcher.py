"""Regression contracts for the compact circular World avatar launcher."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
CSS = (ROOT.parent / "world/public/world/world.css").read_text(encoding="utf-8")
SCENE = world_scene_source()
FACE = (ROOT.parent / "world/public/world/world-avatar-face.js").read_text(encoding="utf-8")
BUILD_BOARD = (ROOT.parent / "world" / "src" / "world_build_board.py").read_text(encoding="utf-8")


def test_avatar_launcher_keeps_actionable_counts_on_their_related_tools():
    assert 'data-world-hud data-hud-expanded="false"' in WORLD
    assert 'data-world-top-actions aria-label="World tools"' in WORLD
    assert 'data-world-shirt-badge' in WORLD
    assert 'aria-expanded="false"' in WORLD
    for removed in (
        "data-world-notification-count",
        "data-world-admin-error-count",
        "data-world-task-count",
        "data-world-notifications-open",
        "data-world-tasks-open",
    ):
        assert removed not in WORLD
    avatar_markup = WORLD.split('class="world-shirt-badge"', 1)[1].split(
        "</button>", 1
    )[0]
    assert "data-world-notification-count" not in avatar_markup
    assert "data-world-admin-error-count" not in avatar_markup
    assert "data-world-task-count" not in avatar_markup
    assert ".world-shirt-badge {" in CSS
    badge_css = CSS.split(".world-shirt-badge {", 1)[1].split(
        ".world-shirt-badge:hover", 1
    )[0]
    assert "border-radius: 50%;" in badge_css
    final_launcher_css = CSS.rsplit("/* Fixed launcher geometry.", 1)[1]
    assert ".fm-world .world-shirt-badge" in final_launcher_css
    assert "border-radius: 50%;" in final_launcher_css
    assert "clip-path: circle(50%);" in final_launcher_css
    assert ".world-shirt-avatar" in CSS
    assert "world-shirt-initial" not in WORLD
    assert ".world-shirt-initial" not in CSS
    assert ".world-top-link:has(" not in CSS
    assert 'content: attr(data-world-tooltip);' not in CSS


def test_launcher_wears_a_painted_face_when_no_photo_was_uploaded():
    # Guests can never have an uploaded account photo, so the badge used to
    # render as an empty disc in the top-right corner. The deterministic
    # portrait the 3D head already paints stands in, keyed to the same
    # identity so the badge and the avatar in the scene match.
    assert "export function paintProceduralAvatarFace(context, identityKey)" in FACE
    assert "export function proceduralAvatarFaceDataURL(identityKey, size = 128)" in FACE
    assert "skin = paintProceduralAvatarFace(context, identityKey);" in SCENE
    assert (
        'import { proceduralAvatarFaceDataURL } from "./world-avatar-face.js";'
        in WORLD
    )
    assert "function hudAvatarFaceSource(session, identity)" in WORLD
    assert (
        'hudFacePortrait = { key, url: proceduralAvatarFaceDataURL(key) };'
        in WORLD
    )
    assert (
        "const shirtAvatarSource = hudAvatarFaceSource(accountSession, identity);"
        in WORLD
    )
    avatar_markup = WORLD.split("data-world-shirt-avatar", 1)[1].split("/>", 1)[0]
    assert 'src="${escapeHTML(shirtAvatarSource)}"' in avatar_markup
    assert "${shirtAvatarSource ? \"\" : \"hidden\"}" in avatar_markup
    identity_sync = WORLD.split("const avatar = this.$(\"[data-world-shirt-avatar]\");", 1)[1]
    assert "const source = hudAvatarFaceSource(session, this.identity);" in identity_sync
    assert "avatar.hidden = !source;" in identity_sync


def test_hud_controls_slide_out_without_resizing_and_latch_until_movement():
    assert '.world-hud[data-hud-expanded="true"] .world-top-link' in CSS
    assert "width: 40px;" in CSS
    assert "transform: translateX(18px) scale(0.84);" in CSS
    assert "const hudHoverTargets = [topActions, rightRail].filter(Boolean);" in WORLD
    assert "target.addEventListener(\"pointerenter\", openHud);" in WORLD
    assert "this.setWorldRightRailExpanded(false);" in WORLD
    assert "if (movement?.moving === true) {" in WORLD


def test_touch_first_tap_reveals_the_launcher_before_opening_settings():
    assert 'const avatarLauncher = event.target.closest("[data-world-shirt-badge]");' in WORLD
    assert "!hoverCapable" in WORLD
    assert "Touch has no hover: the first press exposes the same launcher" in WORLD


def test_phone_launcher_tray_and_right_rail_have_a_readable_safe_layout():
    phone = CSS.split("/* Phone HUD:", 1)[1]
    assert 'top: calc(var(--safe-top) + 64px);' in phone
    assert '.world-hud .world-top-actions [data-world-agent-summon]' in phone
    assert 'top: calc(var(--safe-top) + 6px);' in phone
    assert 'right: calc(var(--safe-right) + 6px);' in phone
    assert 'clip-path: circle(50% at 50% 50%);' in phone
    assert '.world-map:not(.is-expanded) .world-map-label-copy' in phone
    assert 'grid-template-columns: 22px minmax(0, 1fr) auto;' in phone
    assert 'width: 132px;' in phone
    assert 'background: transparent;' in phone
    assert 'box-shadow: none;' in phone


def test_avatar_actions_are_one_compact_toolbar_in_the_island():
    island = WORLD[
        WORLD.index('class="world-hud-island"'):
        WORLD.index("</section>", WORLD.index('class="world-hud-island"'))
    ]
    actions = island.split("data-world-avatar-actions", 1)[1].split("</div>", 1)[0]
    assert 'role="toolbar"' in actions
    assert actions.count('data-dashboard-chat-emote="') == 9
    assert "data-world-jetpack-toggle" in actions
    assert "data-world-avatar-actions-toggle" not in WORLD
    assert "setAvatarActionsExpanded" not in WORLD
    assert "world-avatar-action-menu" not in CSS


def test_recovered_launcher_is_recorded_in_the_build_board_api():
    assert '"task:avatar-hud-launcher"' in BUILD_BOARD

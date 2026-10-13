#!/usr/bin/env python3
"""The avatar chest card has to be readable, not just present.

Every card is painted in one 512-unit layout, so the only thing that decides
whether its fourteen rows arrive as text or as mush is how many device pixels
its backing store holds while a visitor stands in front of it. The memory
budget alone answered that with 128px — 92px on a phone — because an oversized
desktop surface, a crowd figure's plate scale and the compact renderer each
halved it. These contracts pin the ladder that hands the cards being read a
resolution of their own, and the painted contrast that survives to the screen.
"""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[2] / "world" / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
SCENE = world_scene_source()


def run_scene_script(body):
    script = f"""
      import * as scene from {json.dumps(SCENE_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_a_card_being_read_is_painted_above_its_layout():
    scales = run_scene_script(
        """
        console.log(JSON.stringify({
          farDesktop: scene.badgeTextTextureScale("far", 1),
          farCompact: scene.badgeTextTextureScale("far", 0.18),
          readingDesktop: scene.badgeTextTextureScale("reading", 1),
          readingOversized: scene.badgeTextTextureScale("reading", 0.5),
          readingCompact: scene.badgeTextTextureScale("reading", 0.25),
          readingLowTier: scene.badgeTextTextureScale("reading", 0.18),
        }));
        """
    )
    # A distant card keeps whatever the memory budget chose, which is what the
    # zero means: no override.
    assert scales["farDesktop"] == 0
    assert scales["farCompact"] == 0
    # A 512-unit layout needs a 1024px store to stay sharp at the closest a
    # visitor can stand, and the reduction the surface size asked for must not
    # follow the text into the reading level.
    assert scales["readingDesktop"] == 2
    assert scales["readingOversized"] == 2
    # A phone gets its layout at 1:1 and no more: it has the least memory to
    # spend and the fewest screen pixels to spend it on.
    assert scales["readingCompact"] == 1
    assert scales["readingLowTier"] == 1


def test_drawn_size_not_distance_decides_what_is_readable():
    sizes = run_scene_script(
        """
        const at = (options) => scene.badgeCardDrawnPixels(options);
        console.log(JSON.stringify({
          close: at({ distance: 1.5, fov: 60, viewportPixels: 1080 }),
          halfAgain: at({ distance: 3, fov: 60, viewportPixels: 1080 }),
          thirdPersonDefault: at({ distance: 33, fov: 60, viewportPixels: 1080 }),
          retina: at({ distance: 1.5, fov: 60, viewportPixels: 2160 }),
          zoomedLens: at({ distance: 1.5, fov: 30, viewportPixels: 1080 }),
          shrunkFigure: at({ distance: 1.5, fov: 60, viewportPixels: 1080, scale: 0.5 }),
          degenerate: at({ distance: 0, fov: 60, viewportPixels: 1080 }),
        }));
        """
    )
    # Standing in front of a card fills a third of a 1080p viewport, which is
    # more than a 512px store can cover, while the World's own third-person
    # camera stands some thirty metres back and needs nothing.
    assert 350 < sizes["close"] < 800
    assert sizes["thirdPersonDefault"] < 40
    # Distance, pixel ratio and field of view all move the answer, and only
    # together do they say how many texels the card actually needs.
    assert abs(sizes["halfAgain"] * 2 - sizes["close"]) < 0.01
    assert abs(sizes["retina"] - sizes["close"] * 2) < 0.01
    assert sizes["zoomedLens"] > sizes["close"]
    assert abs(sizes["shrunkFigure"] * 2 - sizes["close"]) < 0.01
    # A card at the camera's own position cannot divide by zero.
    assert sizes["degenerate"] > 0


def test_the_reading_resolution_goes_to_the_largest_cards_on_screen():
    picked = run_scene_script(
        """
        const card = (key, extra) => ({
          key,
          visible: true,
          reading: false,
          farPixels: 128,
          ...extra,
        });
        const selection = (cards, limit) =>
          scene.badgeReadingSelection(cards, limit);
        console.log(JSON.stringify({
          // Drawn past the store the budget gave it, so its text is being
          // magnified rather than sampled.
          magnified: selection([card("a", { drawnPixels: 480 })]),
          // Drawn small: the store is already finer than the screen.
          sampled: selection([card("a", { drawnPixels: 90 })]),
          // A tiny store does not make a scenery-sized card worth 1 MB.
          tinyStore: selection([card("a", { drawnPixels: 140, farPixels: 92 })]),
          // A card already being read holds through the hysteresis band and
          // only drops once the visitor has clearly walked away.
          holding: selection([
            card("a", { drawnPixels: 190, reading: true }),
          ]),
          released: selection([
            card("a", { drawnPixels: 120, reading: true }),
          ]),
          hidden: selection([
            card("a", { drawnPixels: 900, visible: false }),
          ]),
          crowd: selection(
            [
              card("near", { drawnPixels: 900 }),
              card("mid", { drawnPixels: 600 }),
              card("far", { drawnPixels: 300 }),
            ],
            2,
          ),
          none: selection([], 6),
        }));
        """
    )
    assert picked["magnified"] == ["a"]
    assert picked["sampled"] == []
    assert picked["tinyStore"] == []
    assert picked["holding"] == ["a"]
    assert picked["released"] == []
    # A card nobody is drawing cannot take a reading slot from one on screen.
    assert picked["hidden"] == []
    assert picked["crowd"] == ["near", "mid"]
    assert picked["none"] == []


def test_the_card_ladder_is_bounded_by_headcount_and_repaint_cost():
    assert "const BADGE_READING_LIMIT = 6;" in SCENE
    assert "const BADGE_DETAIL_SAMPLE_MS = 250;" in SCENE
    assert "const BADGE_DETAIL_UPGRADES_PER_SAMPLE = 2;" in SCENE
    # The sample is a loop cadence like the scene LOD and selection highlight,
    # not per-frame work.
    assert (
        "if (time >= nextBadgeDetailAt) {\n"
        "      nextBadgeDetailAt = time + BADGE_DETAIL_SAMPLE_MS;\n"
        "      updateAvatarBadgeDetail();\n"
        "    }"
    ) in SCENE
    sweep = SCENE.split("function updateAvatarBadgeDetail() {", 1)[1].split(
        "\n  }", 1
    )[0]
    # Screen size, from the drawing buffer rather than CSS pixels, so a device
    # pixel ratio is already counted.
    assert "Number(renderer.domElement?.height) || 0," in sweep
    assert "fov: camera.fov," in sweep
    assert "scale: avatar.scale.y," in sweep
    assert "visible: objectIsEffectivelyVisible(avatar)," in sweep
    assert "farPixels: avatar.userData.badgeFarPixels," in sweep
    assert "const reading = new Set(badgeReadingSelection(cards));" in sweep
    assert "if (upgrades >= BADGE_DETAIL_UPGRADES_PER_SAMPLE) return;" in sweep
    assert 'setAvatarBadgeDetail(THREE, card.key, "far");' in sweep


def test_every_badged_avatar_is_swept_and_released_with_its_controls():
    assert "const badgedAvatars = new Set();" in SCENE
    register = SCENE.split("function registerAvatarChestControls(avatar, peerId) {", 1)[
        1
    ].split("\n  }", 1)[0]
    assert "badgedAvatars.add(avatar);" in register
    unregister = SCENE.split("function unregisterAvatarChestControls(avatar) {", 1)[
        1
    ].split("\n  }", 1)[0]
    assert "badgedAvatars.delete(avatar);" in unregister


def test_a_repaint_carries_the_detail_level_and_records_its_far_size():
    assert "  detail = \"far\",\n) {" in SCENE
    assert "}, plateScale, badgeTextureScale(detail));" in SCENE
    render = SCENE.split("function renderAvatarBadge(THREE, avatar, remote = false) {", 1)[
        1
    ].split("\n}", 1)[0]
    assert "avatar.userData.badgeDetail," in render
    assert 'if (avatar.userData.badgeDetail !== "reading") {' in render
    assert (
        "avatar.userData.badgeFarPixels = badge.material.map.image.height;" in render
    )
    detail = SCENE.split("function setAvatarBadgeDetail(THREE, avatar, detail) {", 1)[
        1
    ].split("\n}", 1)[0]
    # One canvas per card at a time: the repaint path disposes the old texture,
    # so a card that walks away gives its megabytes back.
    assert "if (avatar.userData.badgeDetail === next) return false;" in detail
    assert "renderAvatarBadge(THREE, avatar," in detail
    assert "old?.dispose?.();" in render
    avatar = SCENE.split(
        "function createAvatar(THREE, identity, options = {})", 1
    )[1].split("\nfunction applyAvatarLookDirection", 1)[0]
    assert 'badgeDetail: "far",' in avatar
    assert "badgeFarPixels: badge.material.map.image.height," in avatar


def test_card_text_keeps_its_painted_contrast_and_mip_sharpening():
    avatar = SCENE.split(
        "function createAvatar(THREE, identity, options = {})", 1
    )[1].split("\nfunction applyAvatarLookDirection", 1)[0]
    badge = avatar.split("const badge = cloneSharedPlane(", 1)[1].split(
        "torso.add(badge);", 1
    )[0]
    # ACES plus the live daylight exposure pulled white type off white and
    # dimmed every row after dusk. The card is a document, not a lit surface.
    assert "toneMapped: false," in badge
    assert "return sharpenCanvasTextTexture(THREE, texture);" in SCENE

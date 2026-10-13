#!/usr/bin/env python3
"""Contracts for the World "node served somebody" launch effect.

When a node's signed mirror payload reports that it has answered more clones
or repository web requests than the scene last showed, the cabinet launches a
small figure for the agent it served, shooting up out of the rack and away
into the sky. Like the push surge, it is driven only by the verified payload
the scene already renders — never by an unauthenticated relay frame.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD_DIR = ROOT.parent / "world" / "public" / "world"
APP = (WORLD_DIR / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _block(text, start_marker, end_marker):
    section = text[text.index(start_marker):]
    return section[:section.index(end_marker)]


def test_served_total_counts_clones_and_web_requests_without_estimating():
    total = _block(
        SCENE, "function mirrorServedTotal", "function compactMirrorCount")
    # Both ways a node answers for the repository ride the same trigger.
    assert "mirrorMetric(node?.clonesServed, 1_000_000_000)" in total
    assert "mirrorMetric(node?.websiteServed, 1_000_000_000)" in total
    # A node that reports neither counter has no served history to compare —
    # unreported values are never treated as zero traffic.
    assert "if (clones === null && website === null) return null;" in total


def test_launch_fires_only_on_a_verified_increase_between_snapshots():
    update = _block(
        SCENE, "function updateNetworkNodes", "function focusNetworkNode")
    # The prior total is read off the cabinet ahead of any rebuild, exactly as
    # the push surge captures the prior commit — the cabinet is recreated when
    # the served counters change, so reading it afterwards would see the new
    # value and never fire.
    assert "mirrorServedTotal(cabinet?.userData?.nodeRecord)" in update
    assert update.index("const priorServed") < update.index(
        "removeCabinet(cabinet);")
    assert "const nextServed = mirrorServedTotal(node);" in update
    assert "nextServed > priorServed" in update
    assert "priorServed !== null" in update and "nextServed !== null" in update
    assert (
        "spawnServeFlights(cabinet.position, nextServed - priorServed);"
        in update
    )


def test_no_unauthenticated_frame_can_launch_the_effect():
    # The room frames stay doorbells: nothing in the client app plays the
    # effect directly, it only ever refreshes the signed payload.
    assert "spawnServeFlights" not in APP
    assert "createServedVisitorFigure" not in APP


def test_figure_shoots_up_into_the_sky_and_shrinks_away():
    spawn = _block(
        SCENE, "function spawnServeFlights", "function playRewardEvent")
    # Launched from the top of the cabinet that served the request.
    assert "position.y + 3.3" in spawn
    assert "climb: 34 + Math.random() * 12" in spawn
    animate = _block(
        SCENE,
        "for (let index = serveFlights.length - 1;",
        "updateCamera(delta)",
    )
    assert (
        "flight.group.position.y = flight.baseY + eased * flight.climb;"
        in animate
    )
    # Small and getting smaller as it climbs, fading out on the way.
    assert "flight.group.scale.setScalar(1 - eased * 0.6);" in animate
    assert "material.opacity = Math.max(0, fade);" in animate


def test_effect_stays_bounded_and_frees_everything_it_built():
    spawn = _block(
        SCENE, "function spawnServeFlights", "function playRewardEvent")
    # A burst of served requests stays within a fixed effect budget, both per
    # update and across the whole yard.
    assert "Math.min(\n      3," in spawn
    assert "if (serveFlights.length >= 12) return;" in spawn
    animate = _block(
        SCENE,
        "for (let index = serveFlights.length - 1;",
        "updateCamera(delta)",
    )
    assert "world.remove(flight.group);" in animate
    assert "child.geometry?.dispose?.();" in animate
    assert "child.material?.dispose?.();" in animate
    assert "serveFlights.splice(index, 1);" in animate


def test_the_figure_owns_its_own_materials_so_fading_is_local():
    figure = _block(
        SCENE, "function createServedVisitorFigure",
        "function createMirrorServerCabinet")
    assert "new THREE.MeshBasicMaterial({" in figure
    assert "transparent: true," in figure
    # Both materials are handed to the flight so the fade never reaches into a
    # shared avatar palette.
    assert "group.userData.figureMaterials = [skin, suit];" in figure
    assert "materials: figure.userData.figureMaterials," in SCENE

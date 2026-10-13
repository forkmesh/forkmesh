#!/usr/bin/env python3
"""Frontend contracts for wearable jetpack flight and the World Gym."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")


def test_jetpack_is_wearable_and_flies_on_all_three_axes():
    for contract in (
        'data-world-jetpack-toggle',
        'data-world-jetpack-direction="up"',
        'data-world-jetpack-direction="down"',
        "onJetpackChange: (state) => this.renderJetpackState(state)",
        "toggleJetpack()",
        "setJetpackVertical",
    ):
        assert contract in WORLD
    for contract in (
        'group.name = "forkmesh-avatar-jetpack"',
        "JETPACK_HORIZONTAL_SPEED",
        "JETPACK_VERTICAL_SPEED",
        "JETPACK_MAX_ALTITUDE",
        "function updateJetpackFlight(",
        'Number(keys.has("Space"))',
        'keys.has("KeyC")',
        "jetpackVerticalControl",
        "JETPACK_FLYING_ACTIVITY",
        "toggleJetpack,",
        "setJetpackEquipped,",
        "setJetpackVertical,",
        '"forkmesh-jetpack-control-hint"',
        '"WASD MOVE · SPACE UP · C DOWN · SHIFT BOOST"',
    ):
        assert contract in SCENE
    assert '.world-hud-island .world-avatar-actions button:is([aria-pressed="true"], [data-active="true"])' in CSS
    assert ".world-jetpack-controls" in CSS


def test_gym_has_multiple_clickable_exercise_stations():
    for station in ("bench", "treadmill", "bike", "rower", "punch"):
        assert f'"{station}"' in SCENE
    for contract in (
        'gym.name = "forkmesh-world-gym"',
        "markGymInteractive(",
        "beginGymExercise(String(hit.object.userData.gymEquipment))",
        "function updateGymExercise(",
        "world.add(gym);",
    ):
        assert contract in SCENE


def test_gym_wayfinding_uses_nearby_physical_plaques_not_floating_labels():
    gym_source = SCENE[
        SCENE.index('gym.name = "forkmesh-world-gym"'):
        SCENE.index("\n  const gymState = {")
    ]
    for contract in (
        'gymSign.name = "world-gym-entrance-plaque"',
        "const gymSign = makeGroundPlaque(",
        "const plaque = makeGroundPlaque(",
        "world-gym-station-plaque:",
        "plaque.position.set(x, y, z)",
    ):
        assert contract in gym_source
    assert "makeLabelSprite(" not in gym_source
    assert "label.scale.set(5.1, 1.7, 1)" not in gym_source


def test_bench_press_supports_weight_manual_reps_auto_and_heavy_bar_flex():
    for contract in (
        "data-world-gym-weight-range",
        "data-world-gym-weight-number",
        "data-world-gym-rep",
        "data-world-gym-auto",
        "Auto reps · unlocks at 315 lb",
    ):
        assert contract in WORLD
    for contract in (
        "GYM_HEAVY_WEIGHT_LB = 315",
        "GYM_MAX_WEIGHT_LB = 1200",
        "function setGymWeight(",
        "function performGymRep(",
        "function setGymAuto(",
        "gymState.benchBarSleeves.forEach",
        "pivot.rotation.z = side * bend",
        "gymState.benchBarRig.position.x",
    ):
        assert contract in SCENE
    assert ".world-gym-console" in CSS
    assert '.world-gym-actions button[aria-pressed="true"]' in CSS

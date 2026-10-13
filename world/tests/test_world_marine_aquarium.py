#!/usr/bin/env python3
"""Contracts for the ambient marine aquarium in the Office lobby."""

from pathlib import Path
from world_scene_source import world_scene_module, world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")


def _aquarium_block():
    start = SCENE.index("function* officeMarineAquariumSteps")
    end = SCENE.index("\n}\n", start)
    return SCENE[start:end]


def test_office_lobby_has_a_bioluminescent_left_wall_marine_aquarium():
    aquarium = _aquarium_block()
    assert 'group.name = "forkmesh-office-marine-aquarium"' in aquarium
    assert "group.position.set(...OFFICE_AQUARIUM_POSITION);" in aquarium
    for name in (
        "forkmesh-office-aquarium-frame",
        "forkmesh-office-aquarium-water",
        "forkmesh-office-aquarium-sand",
        "forkmesh-office-aquarium-coral",
        "forkmesh-office-aquarium-anemone",
        "forkmesh-office-aquarium-fish",
        "forkmesh-office-aquarium-bubbles",
        "forkmesh-office-aquarium-light",
    ):
        assert name in aquarium
    assert "function createOfficeMarineAquarium(THREE, animated)" in SCENE
    assert "createInstancedUserFish(THREE, population)" in aquarium


def test_marine_aquarium_stays_ambient_and_reuses_the_scene_animation_queue():
    aquarium = _aquarium_block()
    assert "interactive.push" not in aquarium
    assert "userData.interactive" not in aquarium
    assert "userData.officeFloorId" not in aquarium
    assert "animated.push((time) =>" in aquarium
    assert "Math.sin(time" in aquarium
    assert "fish.position" in aquarium
    assert "bubbleInstances.setMatrixAt(index, bubblePose.matrix);" in aquarium
    assert "coral.rotation.z" in aquarium
    assert "anemone.rotation.z" in aquarium


def test_marine_fish_have_compact_species_detail_and_individual_curved_paths():
    aquarium = _aquarium_block()
    assert "new THREE.LatheGeometry(profile, 12)" in aquarium
    assert "forkmesh-office-aquarium-fish-eye" in aquarium
    assert "forkmesh-office-aquarium-fish-dorsal-fin" in aquarium
    assert "forkmesh-office-aquarium-fish-pectoral-fin" in aquarium
    assert "createAquariumFishTexture" in aquarium
    assert "new THREE.CatmullRomCurve3" in aquarium
    assert "Math.atan2(state.tangent.x, state.tangent.z)" in aquarium


def test_cinematic_reef_builds_recognizable_fish_species():
    aquarium = _aquarium_block()
    for species in (
        "clownfish",
        "blue-tang",
        "yellow-tang",
        "royal-gramma",
        "butterflyfish",
        "chromis",
    ):
        assert f'id: "{species}"' in aquarium
        assert f"forkmesh-office-aquarium-fish-{species}" in aquarium
    for detail in (
        "createAquariumFishTexture",
        "createReefFishBodyGeometry",
        "forkmesh-office-aquarium-fish-gill",
        "forkmesh-office-aquarium-fish-mouth",
        "forkmesh-office-aquarium-fish-iris",
        "forkmesh-office-aquarium-fish-highlight",
    ):
        assert detail in aquarium


def test_cinematic_reef_contains_layered_habitat_families():
    aquarium = _aquarium_block()
    for helper in (
        "createAquariumLiveRock",
        "createBranchingCoral",
        "createPlateCoral",
        "createBrainCoral",
        "createSeaFan",
        "createSoftCoral",
        "createAquariumAnemone",
        "createSeaGrass",
    ):
        assert helper in aquarium
    for name in (
        "forkmesh-office-aquarium-live-rock",
        "forkmesh-office-aquarium-staghorn",
        "forkmesh-office-aquarium-plate-coral",
        "forkmesh-office-aquarium-brain-coral",
        "forkmesh-office-aquarium-sea-fan",
        "forkmesh-office-aquarium-soft-coral",
        "forkmesh-office-aquarium-seagrass",
    ):
        assert name in aquarium


def test_cinematic_reef_uses_compact_habitat_geometry():
    aquarium = _aquarium_block()
    # The reef is viewed as a whole across the lobby, so the habitat shapes
    # keep their silhouettes with deliberately modest segment counts.
    for geometry in (
        'sharedGeometry(THREE, "IcosahedronGeometry", 1, 0)',
        "new THREE.SphereGeometry(0.145 * scale, 8, 6)",
        "new THREE.SphereGeometry(scale, 12, 8)",
        "new THREE.CapsuleGeometry(0.12 * scale, length, 4, 6)",
        "new THREE.CapsuleGeometry(0.038 * scale, length, 4, 5)",
    ):
        assert geometry in aquarium
    assert "for (let index = 0; index < 16; index += 1)" in aquarium


def test_cinematic_reef_uses_curved_motion_and_initialized_atmosphere():
    aquarium = _aquarium_block()
    assert "new THREE.CatmullRomCurve3" in aquarium
    assert "curve.getPointAt" in aquarium
    assert "curve.getTangentAt" not in aquarium
    assert ".sub(state.point)" in aquarium
    assert "initializeAquariumState(0)" in aquarium
    assert "createAquariumCausticTexture" in aquarium
    assert "forkmesh-office-aquarium-particles" in aquarium
    assert "forkmesh-office-aquarium-water-surface" in aquarium
    assert "forkmesh-office-aquarium-caustics" in aquarium


def test_aquarium_registers_a_lobby_only_collision_footprint():
    assert "OFFICE_AQUARIUM_BOUNDS" in SCENE
    assert "function officeScenePointIsWalkable" in SCENE
    assert 'floorId !== "lobby"' in SCENE
    assert "officeInteriorPointIsWalkable(" in SCENE
    assert "pointHitsOfficeAquarium(" in SCENE


def test_aquarium_feeding_is_timed_and_reuses_scene_animation():
    aquarium = _aquarium_block()
    assert "AQUARIUM_FEEDING_DURATION_MS = 60_000" in aquarium
    assert 'food.name = "forkmesh-office-aquarium-food"' in aquarium
    assert "function feed(time = performance.now())" in aquarium
    assert "function updateFeeding(time, animate = true)" in aquarium
    assert "feedingBlend" in aquarium
    assert "feed," in aquarium
    assert "updateFeeding," in aquarium
    assert "getFeedingState," in aquarium
    assert "setTimeout(" not in aquarium
    assert "setInterval(" not in aquarium


def test_aquarium_feeding_action_is_persistently_available_and_accessible():
    panel = world_scene_module("aquarium-panel.js")
    assert 'addButton("feed"' in panel
    assert "result = aquarium.feed(time)" in panel
    assert "feedOfficeAquarium" in SCENE
    assert "updateOfficeAquariumProximity" in SCENE
    assert "syncAquariumPanel(time)" in SCENE


def test_aquarium_physical_panel_combines_every_reef_action():
    panel = world_scene_module("aquarium-panel.js")
    for control in ("feed", "tap", "follow"):
        assert f'addButton("{control}"' in panel
    assert 'addDial("school"' in panel
    assert 'addToggle("backdrop"' in panel
    assert 'addToggle("glass-response"' in panel
    assert 'addLever("light"' in panel
    assert "result = aquarium.tapGlass(time)" in panel
    assert "aquarium.setBackdropOpaque(!aquarium.getControlState(time).backdropOpaque)" in panel
    assert "aquarium.setLightEnabled(!aquarium.getControlState(time).lightEnabled, time)" in panel
    assert "aquarium.setSchoolMode(next, time)" in panel
    assert '"forkmesh-office-aquarium-control-anchor"' not in SCENE
    assert "aquariumDistance / 9" in SCENE
    assert ".world-aquarium-control-panel" not in CSS


def test_aquarium_control_panel_reports_live_school_mode():
    aquarium = _aquarium_block()
    for constant in (
        "AQUARIUM_SCHOOL_INITIAL_DELAY_MS",
        "AQUARIUM_SCHOOL_MIN_IDLE_MS",
        "AQUARIUM_SCHOOL_MAX_IDLE_MS",
        "AQUARIUM_SCHOOL_DURATION_MS",
        "AQUARIUM_SCHOOL_TRANSITION_MS",
        "AQUARIUM_SCHOOL_MODES",
    ):
        assert f"const {constant}" in aquarium
    panel = world_scene_module("aquarium-panel.js")
    assert '"SCHOOL · AUTO · STANDBY"' in panel
    assert "`SCHOOL MODE · ${schooling.label} ACTIVE`" in panel
    assert "schoolLine(current.schooling)" in panel
    assert "schooling: controlState.schooling" in panel


def test_aquarium_fish_approach_a_visitor_gently_without_a_second_loop():
    aquarium = _aquarium_block()
    assert "let visitorReaction = 0" in aquarium
    assert "let visitorReactionTarget = 0" in aquarium
    assert "function selectVisitorFish()" in aquarium
    assert "visitorApproachCount" in aquarium
    assert "aquariumFishApproachesVisitor(index)" in aquarium
    assert "setVisitorProximity(value, target = {})" in aquarium
    assert "1 - Math.exp(-elapsed / 720)" in aquarium
    assert "fish.position.lerp(" in aquarium
    assert "state.visitorPoint" in aquarium
    assert "reaction * 0.68" not in aquarium
    assert "fish.position.x -= reaction" not in aquarium
    assert "setVisitorProximity(" in SCENE


def test_tap_reverses_routes_continuously_and_one_focus_fish_swims_closer():
    aquarium = _aquarium_block()
    assert "function tapGlass(time = performance.now())" in aquarium
    assert "state.direction *= -1" in aquarium
    assert "progress - now * state.speed * state.direction" in aquarium
    assert "AQUARIUM_GLASS_TAP_REACTION_MS = 2_600" in aquarium
    assert "const focus = index === visitorFocusIndex" in aquarium
    assert "approachBlend * (focus ? 0.86 : 0.62)" in aquarium
    assert "tapGlass," in aquarium


def test_aquarium_background_and_light_are_real_scene_toggles():
    aquarium = _aquarium_block()
    assert "function setBackdropOpaque(value)" in aquarium
    assert "backdropMaterial.opacity = backdropOpaque ? 1 : 0.12" in aquarium
    assert "function setLightEnabled(value" in aquarium
    assert "waterLight.intensity = lightAmount *" in aquarium
    assert "spillLight.intensity = lightAmount *" in aquarium
    assert "sandLight.intensity = lightAmount *" in aquarium
    assert "topLight.visible = aquariumLightEnabled" in aquarium


def test_public_users_seed_an_activity_first_school_for_every_member():
    aquarium = _aquarium_block()
    assert "function aquariumUserPalette(name)" in aquarium
    assert '"outfit:" + String(name || "guest")' in aquarium
    assert "OUTFIT_COLORWAYS[rng.int(OUTFIT_COLORWAYS.length)]" in aquarium
    assert "function setUsers(users = [])" in aquarium
    assert "visibleUsers.forEach((user, index) =>" in aquarium
    assert "group.userData.accountPopulation = normalized.length" in aquarium
    assert "group.userData.visibleFish = visibleUsers.length" in aquarium
    assert "const population = visibleUsers.length" in aquarium
    assert "createInstancedUserFish(THREE, population)" in aquarium
    assert "fishLimit" not in aquarium
    assert "fishStates.push({" in aquarium
    assert '"active-recent"' in aquarium
    assert '"inactive"' in aquarium
    assert "const laneMin = recent ? 6.8 : 1.65" in aquarium
    assert "Math.log2(Math.max(1, population) + 1)" in aquarium
    assert "officeAquarium.setUsers(members)" in SCENE
    # Names seed appearance but are not drawn or attached as visible labels.
    assert "makeLabelSprite" not in aquarium
    assert "fillText(user.name" not in aquarium


def test_aquarium_animation_is_lobby_only_and_capped_at_twenty_hertz():
    aquarium = _aquarium_block()
    assert "const AQUARIUM_ANIMATION_MS = 1000 / 20" in aquarium
    assert "if (!animationActive || !aquariumInView || time < nextAnimationAt) return" in aquarium
    assert "nextAnimationAt + AQUARIUM_ANIMATION_MS" in aquarium
    assert "setAnimationActive(value)" in aquarium
    assert 'officeSceneMode === "lobby"' in SCENE
    assert 'officeCurrentFloorId === "lobby"' in SCENE
    assert "officeAquarium.setAnimationActive(aquariumAnimationActive)" in SCENE


def test_aquarium_fish_draw_batches_are_hidden_outside_the_office():
    aquarium = _aquarium_block()
    assert "let fishBodiesVisible = false" in aquarium
    assert "function setFishBodiesVisible(value)" in aquarium
    assert "school.visible = fishBodiesVisible" in aquarium
    assert "body.visible = fishBodiesVisible" in aquarium
    assert "tail.visible = fishBodiesVisible" in aquarium
    assert "fins.visible = fishBodiesVisible" in aquarium
    assert "setFishBodiesVisible(fishBodiesVisible)" in aquarium
    assert "setFishBodiesVisible," in aquarium
    assert 'officeAquarium.setFishBodiesVisible(officeSceneMode !== "town")' in SCENE

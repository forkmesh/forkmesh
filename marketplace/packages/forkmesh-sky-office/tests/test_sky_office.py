#!/usr/bin/env python3
"""Contracts for the ForkMesh Aquarium Sky Office marketplace package.

The package is a World district: everything it needs lives under this
package directory, the core World only offers the generic district host, and
the package ships switched off until a visitor installs it.

Run explicitly: ``python3 -m pytest marketplace/packages/forkmesh-sky-office/tests``.
"""

import json
from pathlib import Path
import re
import subprocess


PACKAGE = Path(__file__).resolve().parents[1]
ROOT = PACKAGE.parents[2]
MODULE = PACKAGE / "content" / "world" / "index.js"
MANIFEST = PACKAGE / "manifest.json"
ORIGINAL_TOWER = ROOT / "world" / "public" / "world" / "world-office-tower.js"
OFFICE_CONTROLLER = ROOT / "world" / "public" / "world" / "world-office.js"
SCENE = ROOT / "world" / "public" / "world" / "world-scene.js"
WORLD = ROOT / "world" / "public" / "world" / "world.js"
GENERATED = ROOT / "world" / "public" / "world" / "world-marketplace-packages.js"


def source(path):
    return path.read_text(encoding="utf-8")


def run_module_script(body):
    script = f"""
      import * as sky from {json.dumps(MODULE.as_uri())};
      import * as original from {json.dumps(ORIGINAL_TOWER.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_package_ships_off_and_stays_out_of_core():
    manifest = json.loads(source(MANIFEST))
    assert manifest["id"] == "forkmesh-sky-office"
    assert manifest["type"] == "virtual.world"
    assert manifest["entrypoints"] == {
        "world": "content/world/index.js",
        "settings": "builtin:package",
    }
    assert manifest["install"] == {"bundled": True, "default": False}
    assert manifest["lifecycle"] == {"migrations": []}
    assert (PACKAGE / "LICENSE").is_file()
    assert (PACKAGE / "preview" / "market.png").is_file()
    # Lazy entry: the catalog lists it, but its code is a dynamic import.
    generated = source(GENERATED)
    assert 'id: "forkmesh-sky-office",' in generated
    assert "installedByDefault: false," in generated
    assert (
        'load: () => import("../../../marketplace/packages/forkmesh-sky-office/content/world/index.js")'
        in generated
    )
    assert "packages/forkmesh-sky-office" not in generated.split("BUNDLED_WORLD_PACKAGES", 1)[0]
    # Nothing sky-office-specific remains in the core World.
    for path in (SCENE, WORLD, ROOT / "world" / "src" / "world.py"):
        assert "sky-office" not in source(path).replace("aquarium Sky Office", "")
        assert "SkyOffice" not in source(path)
    assert not (ROOT / "world" / "public" / "world" / "world-forkmesh-sky-office.js").exists()
    module = source(MODULE)
    assert not re.search(r"^import ", module, re.MULTILINE)
    assert "fetch(" not in module


def test_install_returns_the_district_contract():
    result = run_module_script(
        """
        const interactive = [];
        const animated = [];
        const sounds = [];
        const access = [];
        const host = {
          THREE: null,
          interactive,
          animated,
          disposeObject3D: () => {},
          emitSound: (stage, detail) => sounds.push(stage),
          requestAccess: () => access.push("request"),
          releaseAccess: () => access.push("release"),
          setFloorY: () => {},
          setSpace: () => {},
        };
        let threw = "";
        try {
          sky.install({});
        } catch (error) {
          threw = String(error.message);
        }
        process.stdout.write(JSON.stringify({
          threw,
          manifest: sky.manifest,
          spaces: Object.keys(sky.FORKMESH_SKY_OFFICE_SPACES),
          spaceSpec: sky.FORKMESH_SKY_OFFICE_SPACES["pkg-forkmesh-sky-office-glass-lounge"],
          radius: sky.FORKMESH_SKY_OFFICE_WORLD_RADIUS,
          soundCount: Object.keys(sky.soundProfiles).length,
        }));
        """
    )
    assert "district host" in result["threw"]
    assert result["manifest"]["id"] == "forkmesh-sky-office"
    assert result["manifest"]["label"] == "ForkMesh Aquarium Sky Office"
    assert result["spaces"] == [
        "pkg-forkmesh-sky-office-campus",
        "pkg-forkmesh-sky-office-aquarium-lobby",
        "pkg-forkmesh-sky-office-glass-lounge",
        "pkg-forkmesh-sky-office-summit-office",
        "pkg-forkmesh-sky-office-drone-roof",
    ]
    assert result["spaceSpec"]["floorY"] == 16 + 0.38
    assert result["spaceSpec"]["remoteHorizon"] is True
    assert result["spaceSpec"]["label"] == "Forkmesh Office"
    assert result["radius"] == 14_400 + 112 + 32 + 4  # ceil(hypot(330, 14400)) + 112 + 32
    assert result["soundCount"] == 35
    module = source(MODULE)
    for contract in (
        "export function install(host)",
        "walkSurfaceContains: forkmeshSkyOfficeSurfaceContains,",
        "constrainPlayer: (previous, player) =>",
        "onWorldReady: ({ officeInterior } = {}) =>",
        "cameraDistanceLimit: (target, direction, requested, player) =>",
        "office.setSoundEmitter((stage, detail) => host.emitSound?.(stage, detail));",
        "host.disposeObject3D?.(office.group);",
        'setSpace("pkg-forkmesh-sky-office-aquarium-lobby")',
        'setSpace("town-square")',
    ):
        assert contract in module, contract
    # The host only ever sees namespaced spaces.
    assert 'setSpace("forkmesh-sky-office-' not in module
    assert "setSpace(`forkmesh-sky-office-${" not in module


def test_core_offers_the_generic_district_host():
    scene = source(SCENE)
    for contract in (
        "function districtPackageHost(packageId, module, packageAnimated)",
        "function installDistrictPackage(packageId, entry, module)",
        "const packageSpaces = new Map();",
        "function districtWalkSurfaceContains(x, z, radius)",
        "function districtCameraDistanceLimit(target, direction, requestedDistance)",
        "function interactWithDistrictPackage(object)",
        "districtPackages[index].instance.constrainPlayer?.(",
        "const state = record.instance.update?.(record.frame);",
        "districtPackages[index].instance.setAccess?.(lastOfficeAccessPayload);",
        "districtPackages[index].instance.onWorldReady?.({ officeInterior });",
        "animated.splice(trampoline, 1);",
        "record.spaces.forEach((spaceId) => packageSpaces.delete(spaceId));",
        "getWorldPackageState: (id) =>",
        "onWorldPackageAccessRequest = () => {},",
    ):
        assert contract in scene, contract
    world = source(WORLD)
    assert "onWorldPackageAccessRequest: () => {" in world
    assert "controller?.hydrateFloorAccess?.()" in world
    assert "playOfficeElevatorSound(stage, trip = {}, profile = null)" in world
    assert 'INSTALLED_WORLD_PACKAGES_KEY = "forkmesh.world.installedPackages.v1"' in world
    assert "async function hydrateFloorAccess()" in source(OFFICE_CONTROLLER)


def test_sky_office_is_an_additional_destination_and_original_office_remains():
    result = run_module_script(
        """
        process.stdout.write(JSON.stringify({
          originalCenter: original.OFFICE_ISLAND_CENTER,
          originalFloors: original.OFFICE_FLOORS.map((floor) => floor.id),
          distanceMultiplier: sky.FORKMESH_SKY_OFFICE_DISTANCE_MULTIPLIER,
          skyCenter: sky.FORKMESH_SKY_OFFICE_CENTER,
          skyFloors: sky.FORKMESH_SKY_OFFICE_FLOORS.map((floor) => floor.id),
        }));
        """
    )

    assert result["originalCenter"] != result["skyCenter"]
    assert result["distanceMultiplier"] == 20
    assert result["skyCenter"] == [-330, 0, -720 * 20]
    assert result["originalFloors"] == [
        "lobby",
        "marketing",
        "engineering",
        "rooftop",
    ]
    assert result["skyFloors"] == [
        "aquarium-lobby",
        "glass-lounge",
        "summit-office",
        "drone-roof",
    ]
    scene = source(SCENE)
    assert "function createForkMeshOffice(" in scene
    assert "office: createForkMeshOffice" in scene


def test_public_lounge_and_server_role_gated_summit_routing():
    result = run_module_script(
        """
        const guest = sky.normalizeForkmeshSkyOfficeAccess({
          authenticated: false,
          organizationRole: "owner",
        });
        const signedOutsider = sky.normalizeForkmeshSkyOfficeAccess({
          authenticated: true,
          organizationRole: "",
          allowedFloorIds: ["summit-office"],
        });
        const member = sky.normalizeForkmeshSkyOfficeAccess({
          authenticated: true,
          organizationRole: "member",
          account: "Alice",
        });
        process.stdout.write(JSON.stringify({ guest, signedOutsider, member }));
        """
    )

    public = ["aquarium-lobby", "glass-lounge"]
    assert result["guest"]["allowedFloorIds"] == public
    assert result["signedOutsider"]["allowedFloorIds"] == public
    assert result["member"]["allowedFloorIds"] == public + [
        "summit-office",
        "drone-roof",
    ]
    assert result["member"]["account"] == "alice"

    module = source(MODULE)
    controller = source(OFFICE_CONTROLLER)
    assert 'access.organizationRole\n        ? "summit-office"\n        : "glass-lounge"' in module
    assert "async function hydrateFloorAccess()" in controller
    assert "officeAccess = await loadFloorAccess(activeSession);" in controller


def test_aquarium_tower_glass_lounge_summit_roof_and_lod_are_explicit():
    module = source(MODULE)
    dimensions = run_module_script(
        """
        process.stdout.write(JSON.stringify({
          floorHeight: sky.FORKMESH_SKY_OFFICE_FLOOR_HEIGHT,
          lounge: sky.FORKMESH_SKY_OFFICE_LOUNGE_Y,
          rise: sky.FORKMESH_SKY_OFFICE_ELEVATOR_RISE,
          summit: sky.FORKMESH_SKY_OFFICE_SUMMIT_Y,
        }));
        """
    )
    assert dimensions["rise"] == dimensions["floorHeight"] * 20
    assert dimensions["summit"] == (
        dimensions["lounge"] + dimensions["floorHeight"] + dimensions["rise"]
    )
    for contract in (
        "export const FORKMESH_SKY_OFFICE_AQUARIUM_DEPTH = 6",
        '"forkmesh-sky-office-six-foot-water-wall"',
        '"forkmesh-sky-office-aquarium-inner-glass"',
        "const fishCount = 72",
        "const coralCount = 48",
        '"forkmesh-sky-office-floor-glass-lounge"',
        "addCouch(THREE, lounge",
        '"forkmesh-sky-office-glass-tube-elevator"',
        "export const FORKMESH_SKY_OFFICE_ELEVATOR_RISE = 320",
        '"forkmesh-sky-office-floor-summit-office"',
        '"forkmesh-sky-office-summit-widget-copies"',
        '"forkmesh-sky-office-floor-drone-roof"',
        '"forkmesh-sky-office-drone-landing-pad"',
        "const lod = new THREE.LOD()",
        "lod.addLevel(high, 0, 0.08)",
        "lod.addLevel(low, 260, 0.12)",
        "export const FORKMESH_SKY_OFFICE_DISTANT_TRIANGLE_BUDGET = 900",
        'lowIsland.name = "forkmesh-sky-office-low-island"',
        "const hyperloopCollarFractions = Object.freeze([0, 0.004, 0.996, 1])",
        "const collars = new THREE.InstancedMesh(",
        "forkmeshSkyOfficeDistantTriangleCount",
        "office.populateFromExisting(officeInterior)",
    ):
        assert contract in module, contract
    assert "new THREE.CylinderGeometry(3.05, 3.05, tubeLength, 24" not in module
    assert "new THREE.CapsuleGeometry(2.35, 6.8, 8, 16)" not in module


def test_sky_office_defers_full_construction_and_avoids_transmission_passes():
    module = source(MODULE)

    for contract in (
        "function createForkmeshSkyOfficeFull(",
        "function createForkmeshSkyOfficeShell(",
        "const shell = createForkmeshSkyOfficeShell(THREE, interactive);",
        "const ensureFull = () => {",
        "return ensureFull().interactNearest(player);",
        "forkmeshSkyOfficeLoaded = false",
        "const handle = globalThis.requestIdleCallback(populate, {",
        "timeout: 1500,",
        "isLoaded: () => full !== null",
        "cancelPopulation();",
        "child.castShadow = false",
        "broadSurface && child.material?.transparent !== true",
    ):
        assert contract in module

    # Refraction/transmission asks Three.js for another full-scene render. The
    # office uses ordinary transparent materials so entering it does not halve
    # the frame rate, and the always-resident route uses an unlit material.
    assert "new THREE.MeshPhysicalMaterial" not in module
    assert "transmission:" not in module
    assert "const hyperloopGlass = new THREE.MeshBasicMaterial({" in module
    assert "office.dispose();" in module


def test_cube_elevator_reception_and_bidirectional_hyperloop_layout():
    module = source(MODULE)
    result = run_module_script(
        """
        process.stdout.write(JSON.stringify({
          center: sky.FORKMESH_SKY_OFFICE_CENTER,
          elevatorZ: sky.FORKMESH_SKY_OFFICE_ELEVATOR_Z,
          entranceZ: sky.FORKMESH_SKY_OFFICE_FRONT_Z,
          startZ: sky.FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z,
          endZ: sky.FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z,
          island: sky.forkmeshSkyOfficeSurfaceContains(
            sky.FORKMESH_SKY_OFFICE_CENTER[0],
            sky.FORKMESH_SKY_OFFICE_CENTER[2],
          ),
          route: sky.forkmeshSkyOfficeSurfaceContains(
            sky.FORKMESH_SKY_OFFICE_CENTER[0],
            (sky.FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z +
              sky.FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z) / 2,
          ),
          water: sky.forkmeshSkyOfficeSurfaceContains(-100, -500),
        }));
        """
    )

    assert result["island"] is True
    assert result["route"] is True
    assert result["water"] is False
    assert result["startZ"] - result["endZ"] > 450
    assert result["entranceZ"] > result["elevatorZ"] > 28 > -24
    for contract in (
        'cubeMount.name = "forkmesh-sky-office-spinning-cube"',
        "cubeMount.position.set(0, 6.6, 28)",
        'reception.name = "forkmesh-sky-office-reception"',
        "reception.position.set(0, 0, -24)",
        'hyperloop.name = "forkmesh-sky-office-twin-hyperloop"',
        '"forkmesh-sky-office-hyperloop-car"',
        '"forkmesh-office"',
        '"mainland"',
        "const boarded = office.interactNearest(player) === true;",
    ):
        assert contract in module, contract


def test_every_sky_office_sound_event_is_generated_and_played():
    result = run_module_script(
        """
        process.stdout.write(JSON.stringify({
          sounds: sky.FORKMESH_SKY_OFFICE_SOUND_EVENTS,
          profiles: Object.keys(sky.soundProfiles),
        }));
        """
    )
    sounds = result["sounds"]
    module = source(MODULE)

    assert len(sounds) == 35
    assert len(sounds) == len(set(sounds))
    assert set(sounds) == set(result["profiles"])
    for event in sounds:
        # Once in the exported contract, once in the profile table, and at
        # least once where geometry or runtime behavior emits it.
        assert module.count(f'"{event}"') >= 3
    # The host plays whatever profile the package hands it.
    world = source(WORLD)
    assert "sky-office-" not in world
    assert "if (!Array.isArray(profile) || profile.length < 6) return;" in world
    assert "module.soundProfiles?.[stage] ?? null" in source(SCENE)


def test_sky_office_polish_keeps_the_tank_readable_and_camera_inside_floors():
    result = run_module_script(
        """
        process.stdout.write(JSON.stringify({
          lightSettings: sky.FORKMESH_SKY_OFFICE_LIGHT_SETTINGS,
        }));
        """
    )
    settings = result["lightSettings"]
    assert [setting["id"] for setting in settings] == [
        "daylight",
        "moonlight",
        "sunset",
        "rgb-slow-fade",
    ]
    assert settings[-1]["label"] == "RGB SLOW FADE"

    module = source(MODULE)
    assert "new THREE.ConeGeometry" not in module
    for contract in (
        'fishTail.name = "forkmesh-sky-office-aquarium-fish-tails"',
        'fishFoodPellets.name = "forkmesh-sky-office-aquarium-food-pellets"',
        'fishFoodPulse.name = "forkmesh-sky-office-aquarium-food-bloom"',
        'receptionist.name = "forkmesh-sky-office-receptionist"',
        "function createSkyOfficeForkmeshCube(",
        '"forkmesh-reflective-fm-cube-fixed-tilt"',
        "let aquariumLightSettingIndex",
        'aquariumLightSetting.id === "rgb-slow-fade"',
        "-terrazzo-finish",
        "function skyOfficeCameraDistanceLimit(",
    ):
        assert contract in module, contract

import re
from pathlib import Path


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = WORLD / "scene"


def read(path):
    return path.read_text(encoding="utf-8")


def test_aquarium_builds_in_yielding_steps_during_the_scene_build():
    aquarium = read(SCENE / "office-aquarium.js")
    assert "export function* officeMarineAquariumSteps(THREE, animated)" in aquarium
    assert "export async function createOfficeMarineAquariumSliced(" in aquarium
    body = aquarium[aquarium.index("export function* officeMarineAquariumSteps"):]
    assert len(re.findall(r"^  yield;$", body, re.M)) >= 8
    floors = read(SCENE / "districts" / "office-floors.js")
    assert "await createOfficeMarineAquariumSliced(" in floors
    assert "createOfficeMarineAquarium(THREE, animated)" not in floors


def test_far_proxy_merge_yields_per_batch_mesh():
    batching = read(SCENE / "batching.js")
    steps = batching[batching.index("export function* mergeObjectBatchesSteps"):]
    steps = steps[: steps.index("export function mergeMeshesByMaterial")]
    assert steps.count("yield;") >= 3
    assert "for (const [key, batch] of batches)" in steps


def test_logo_fountain_is_sliced_and_idle_when_hidden():
    displays = read(SCENE / "districts" / "office-displays.js")
    start = displays.index('await sceneSlice("office-boards"')
    end = displays.index('await sceneSlice("office-logo-fountain"')
    fountain = displays[start:end]
    assert fountain.count("await sceneSlice();") >= 3
    assert "for (let node = logoFountain; node; node = node.parent)" in fountain
    assert "if (!node.visible) return;" in fountain


def test_asset_textures_decode_as_image_bitmaps():
    town = read(SCENE / "town.js")
    assert "new THREE.ImageBitmapLoader()" in town
    assert 'loader.setOptions({ imageOrientation: "flipY" })' in town
    assert "texture.flipY = false;" in town


def test_own_account_profile_is_fetched_once():
    app = read(WORLD / "world.js")
    assert "const WORLD_ACCOUNT_PROFILE_MAX_AGE_MS = 60_000;" in app
    helper = app[app.index("  fetchAccountProfile(account, { fresh = false } = {}) {"):]
    helper = helper[: helper.index("\n  }\n")]
    assert "?viewer=${encodeURIComponent(viewer)}" in helper
    assert "maxAge: WORLD_ACCOUNT_PROFILE_MAX_AGE_MS" in helper
    assert "this.fetchAccountProfile(account, {" in app
    assert "? this.fetchAccountProfile(account)" in app
    assert "fresh: true," in app

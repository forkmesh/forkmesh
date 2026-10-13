#!/usr/bin/env python3
"""Every avatar's repeated, untextured parts draw as shared instanced batches.

Legs, shoes and the shoulder antenna are identical for everybody — same
geometry, same material, only the transform differs — so they belong in one
batch for the whole crowd rather than eight meshes per person. The runtime
behaviour is covered by world/browser_tests/tests/world-avatar-instancing.spec.js;
these are the source contracts that keep the wiring in place.
"""

from pathlib import Path
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = world_scene_source()


def avatar_body() -> str:
    return SCENE.split("function createAvatar(THREE, identity", 1)[1].split(
        "function applyAvatarLookDirection", 1
    )[0]


def test_repeated_avatar_parts_are_one_instanced_batch_each():
    batch = SCENE.split("function createAvatarPartInstances(THREE, scene)", 1)[
        1
    ].split("function avatarDetailParent(", 1)[0]
    assert 'const AVATAR_INSTANCED_PART_KINDS = [' in SCENE
    for kind, slots in (
        ("leg", 4),
        ("shoe", 2),
        ("antennaStalk", 1),
        ("antennaBulb", 1),
    ):
        assert f"  {kind}: {slots}," in SCENE
        assert f"{kind}: (THREE) =>" in SCENE
    assert "new THREE.InstancedMesh(" in batch
    assert 'mesh.name = `forkmesh-avatar-${batch.kind}-instances`;' in batch
    assert "mesh.frustumCulled = false;" not in batch
    assert "batch.mesh.computeBoundingSphere();" in SCENE
    assert "mesh.instanceMatrix.setUsage?.(THREE.DynamicDrawUsage);" in batch
    # Geometry and material outlive every avatar built from them.
    assert "geometry.userData.forkmeshSharedResource = true;" in batch
    assert "material.userData.forkmeshSharedResource = true;" in batch


def test_batch_capacity_is_claimed_before_any_matrix_is_written():
    sync = SCENE.split("  function sync() {", 1)[1].split(
        "\n  return {", 1
    )[0]
    # Growing part-way through the crowd would abandon the matrices already
    # written into the smaller buffer, so every batch is sized first.
    grow_at = sync.index("grow(batch,")
    write_at = sync.index("batch.mesh.setMatrixAt(")
    assert grow_at < write_at
    assert "avatars.size * AVATAR_INSTANCED_PART_SLOTS[kind]" in sync
    assert "if (batch.count >= batch.capacity) break;" in sync
    assert "batch.mesh.count = batch.count;" in sync
    assert "matrices.needsUpdate = true;" in sync
    assert "matrices.addUpdateRange(" in sync


def test_hidden_and_departed_avatars_leave_no_instances_behind():
    sync = SCENE.split("  function sync() {", 1)[1].split(
        "\n  return {", 1
    )[0]
    # A detached avatar is gone for good; districts and the Office hide their
    # contents instead of removing them, so both cases are covered.
    assert "if (!avatar.parent) {" in sync
    assert "avatars.delete(avatar);" in sync
    assert "objectIsEffectivelyVisible(avatar)" in sync
    assert "if (!branchVisible(node, avatar)) continue;" in sync


def test_scene_fills_the_batches_after_matrices_settle():
    assert (
        "const avatarPartInstances = createAvatarPartInstances(THREE, avatarCrowdBatches);"
        in SCENE
    )
    # onBeforeRender runs after the renderer refreshes every world matrix and
    # before it builds the render list, so poses are settled and the shadow
    # pass sees the same instances.
    before_render = SCENE.split(
        "scene.onBeforeRender = (_renderer, _scene, renderCamera) => {", 1
    )[1].split("};", 1)[0]
    assert "avatarPartInstances.sync();" in before_render
    # Every avatar in the World joins the crowd batch.
    assert SCENE.count("instances: avatarPartInstances") == 6
    assert SCENE.count("createAvatar(") == 7
    assert "avatarPartInstances: avatarPartInstances.stats()," in SCENE


def test_an_element_root_put_back_rejoins_the_crowd_batch():
    attach = SCENE.split("  function attachElementRoot(element, root) {", 1)[
        1
    ].split("\n  }", 1)[0]
    # The Elements panel detaches a root from the Scene rather than hiding it,
    # so the sweep drops that avatar; switching the element back on has to hand
    # its legs, shoes and antenna back.
    assert "avatarPartInstances.register(root);" in attach
    assert attach.index("record.parent?.add?.(root);") < attach.index(
        "avatarPartInstances.register(root);"
    )


def test_avatar_legs_shoes_and_antenna_are_transform_only_rigs():
    avatar = avatar_body()
    assert "const instances = options.instances || null;" in avatar
    assert "instancedPart(\"leg\")" in avatar
    assert "instancedPart(\"shoe\")" in avatar
    assert "instancedPart(\"antennaStalk\")" in avatar
    assert "instancedPart(\"antennaBulb\")" in avatar
    assert "instances?.register(group);" in avatar
    # Legs and shoes no longer need a material of their own; the batch has one.
    assert "const dark = instancedParts\n    ? null" in avatar
    assert "const shoe = instancedParts\n    ? null" in avatar
    # A bare createAvatar — a test, or a scene built before the batch — still
    # gets a complete avatar.
    assert "cloneSharedMesh(\n        THREE,\n        \"avatar-leg-segment\"," in avatar
    assert "cloneSharedMesh(\n        THREE,\n        \"avatar-shoe\"," in avatar


def test_lower_body_keeps_one_invisible_selection_proxy():
    avatar = avatar_body()
    proxy = avatar.split("if (instancedParts) {", 1)[1].split("}", 1)[0]
    assert '"avatar-leg-hit-proxy"' in proxy
    assert "new THREE.BoxGeometry(1.02, AVATAR_HIP_Y, 0.56)" in proxy
    assert "legProxy.position.y = AVATAR_HIP_Y / 2;" in proxy
    # Never drawn, always hit-tested.
    assert "legProxy.visible = false;" in proxy
    assert "legProxy.userData.raycastProxy = true;" in proxy
    assert "function avatarHitProxyMaterial(THREE)" in SCENE
    assert "new THREE.MeshBasicMaterial({ visible: false })" in SCENE


def test_antenna_blink_rides_in_the_per_instance_colour():
    blink = SCENE.split("const lit = reducedMotion || cycle < 0.5;", 1)[1].split(
        "\n  }", 1
    )[0]
    assert "bulb.userData.partColor = glow;" in blink
    # The fallback bulb owns its material and still paints itself.
    assert "bulb.material?.color?.set(glow);" in blink
    # White batch material, so the per-instance colour survives the multiply.
    assert (
        'define("antennaBulb", resources.material(THREE, { color: "#ffffff" }, "MeshBasicMaterial"), true)'
        in SCENE
    )

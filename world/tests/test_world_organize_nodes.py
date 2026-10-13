#!/usr/bin/env python3
"""Contracts for automatic World node placement and the pool rim title.

Every live mirror-node cabinet is evenly placed around the centred SOL board
as part of the existing catalog update. Membership changes deliberately reflow
the live ring so deleted nodes cannot leave permanent gaps.
"""

import json
import math
import subprocess
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _function_source(name):
    start = SCENE.index(f"function {name}(")
    end = SCENE.index("\n  }\n", start)
    return SCENE[start:end] + "\n  }\n"


def _reward_circle_slots(count, centre_x=0.0, centre_z=0.0, options=None):
    # The real landmark table decides the campfire keep-out, so the slot math
    # is exercised against the shipped world data rather than a stand-in.
    data_uri = json.dumps((WORLD / "world-data.js").as_uri())
    world_radius = SCENE[SCENE.index("const WORLD_RADIUS = "):]
    world_radius = world_radius[: world_radius.index(";") + 1]
    source = _function_source("rewardCircleSlots")
    completed = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            f"import {{ landmarkById }} from {data_uri};\n"
            + world_radius
            + "\n"
            + source
            + (
                "process.stdout.write(JSON.stringify("
                f"rewardCircleSlots({centre_x}, {centre_z}, {count}, "
                f"{json.dumps(options or {})})));"
            ),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(completed.stdout)


def test_manual_organize_nodes_control_is_removed():
    assert "data-world-organize-nodes" not in APP
    assert "Organize nodes" not in APP
    assert "toggleOrganizedNodes" not in APP
    assert "syncOrganizeNodesButton" not in APP


def test_manual_node_layout_mode_and_scene_controls_are_removed():
    assert "organizeNetworkNodes" not in SCENE
    assert "getNodeLayoutState" not in SCENE
    assert "nodeLayoutMode" not in SCENE
    assert "lastNetworkNodes" not in SCENE


def test_live_nodes_are_automatically_ringed_and_face_the_reward_pool():
    assert 'const fountain = landmarkObjects.get("fountain")' in SCENE
    assert "const circleSlots = rewardCircleSlots(" in SCENE
    assert "usableNodes.length" in SCENE
    assert "const slot = circleSlots[nodeIndex]" in SCENE
    assert "cabinet.position.set(slot.x, 0.38, slot.z)" in SCENE
    assert "routingX - slot.x" in SCENE
    assert "routingZ - slot.z" in SCENE
    assert "serverSlots" not in SCENE


def test_node_order_is_stable_but_membership_changes_reflow_the_ring():
    block = _function_source("updateNetworkNodes")
    assert ".sort((left, right) => {" in block
    assert "left.name.toLowerCase()" in block
    assert "right.name.toLowerCase()" in block
    assert ".filter(" in block
    assert ".slice(0, 64)" in block
    assert "nodeSlotAssignments" not in block
    assert "usableNodes.forEach(({ node, name: nodeName }, nodeIndex)" in block
    assert "networkNodeSnapshot" in block
    assert 'worldLayoutId("node-"' not in block
    # Automatic placement piggybacks on the catalog update: it does not add
    # another timer, request, or listener.
    assert "fetch(" not in block
    assert "setInterval(" not in block
    assert "addEventListener(" not in block


def test_reward_circle_slots_clear_the_pool_and_never_collide():
    for count in (1, 3, 8, 17, 64):
        slots = _reward_circle_slots(count)
        assert len(slots) == count
        for slot in slots:
            radius = math.hypot(slot["x"], slot["z"])
            # Outside the 5.25 pool rim and the 6.4–8.5 tree circle.
            assert radius >= 10.7
            # The member fire now occupies its own south island, safely clear
            # of every central service-yard slot.
            assert math.hypot(slot["x"], slot["z"] - 130) >= 8.2
        for index, left in enumerate(slots):
            for right in slots[index + 1:]:
                assert math.dist(
                    (left["x"], left["z"]), (right["x"], right["z"])
                ) >= 3.6, count


def test_reward_circle_slots_are_deterministic():
    assert _reward_circle_slots(64) == _reward_circle_slots(64)


def test_each_populated_node_ring_has_equal_angular_spacing():
    for count in (2, 3, 8, 17):
        slots = _reward_circle_slots(count)
        angles = sorted(
            math.atan2(slot["z"], slot["x"]) % (math.pi * 2)
            for slot in slots
        )
        gaps = [
            (angles[(index + 1) % count] - angles[index]) % (math.pi * 2)
            for index in range(count)
        ]
        expected = math.pi * 2 / count
        assert all(abs(gap - expected) < 1e-9 for gap in gaps)
        assert abs(sum(slot["x"] for slot in slots) / count) < 1e-9
        assert abs(sum(slot["z"] for slot in slots) / count) < 1e-9


def test_node_deletion_immediately_reflows_surviving_cabinets():
    block = _function_source("deleteNetworkNode")
    assert "nodeInfrastructure.delete(matchId)" in block
    assert "if (matches.length) relayoutNetworkNodes();" in block


def test_member_growth_no_longer_reanchors_nodes_around_roster_benches():
    assert "function relayoutNetworkNodes()" in SCENE
    member_start = SCENE.index("function updateMemberLounge(")
    member_end = SCENE.index("\n  }\n", member_start)
    lounge = SCENE[member_start:member_end]
    assert "previousSeatRadius" not in lounge
    assert "relayoutNetworkNodes();" not in lounge
    assert "const slot = layout.memberSlots[index]?.position || seat;" in lounge


def test_reward_circle_slots_follow_a_relocated_pool():
    slots = _reward_circle_slots(
        6,
        centre_x=12.0,
        centre_z=-7.0,
    )
    assert len(slots) == 6
    for slot in slots:
        assert math.hypot(slot["x"] - 12.0, slot["z"] + 7.0) >= 10.7
    # Evenly spread around the relocated pool, not bunched on one side.
    assert abs(sum(slot["x"] for slot in slots) / 6 - 12.0) < 1.5
    assert abs(sum(slot["z"] for slot in slots) / 6 + 7.0) < 1.5


def test_reward_pool_rim_title_is_measured_so_wraps_do_not_overlap():
    start = SCENE.index("function rewardPoolRimTexture(")
    block = SCENE[start: SCENE.index("\n}\n", start)]
    assert "const repeats = 3;" in block
    assert 'const title = "GLOBAL REWARD POOL";' in block
    # The type shrinks to the measured slot rather than trusting a fixed size.
    assert "const available = slot * 0.72;" in block
    assert "context.measureText(title).width > available" in block
    assert "context.fillText(title, centre, height / 2 + 4);" in block


def test_reward_pool_shows_a_live_mirror_count_above_the_orb():
    # The cabinets already ring this basin, so their live total hangs over the
    # pool orb instead of on another sign.
    assert "function rewardPoolMirrorCountTexture(THREE, total, online)" in SCENE
    fountain_start = SCENE.index("function createFountain(")
    fountain = SCENE[fountain_start: SCENE.index("\n}\n", fountain_start)]
    assert 'mirrorCountSprite.name = "reward-pool-mirror-count";' in fountain
    # The online count is painted directly into the raised fireball orb.
    assert "mirrorCountSprite.position.y = 6.7;" in fountain
    assert "depthTest: false" in fountain
    assert "group.add(mirrorCountSprite);" in fountain
    # Hidden until the first signed catalog lands, so no placeholder zero.
    assert "mirrorCountSprite.visible = false;" in fountain
    texture_start = SCENE.index("function rewardPoolMirrorCountTexture(")
    texture = SCENE[texture_start: SCENE.index("\n}\n", texture_start)]
    assert 'context.fillText(live.toLocaleString("en-US"), 256, 92);' in texture
    assert 'context.fillText("ONLINE", 256, 168);' in texture
    # Offline cabinets stay in the ring, and the second line states the total.
    assert '`${count.toLocaleString("en-US")} TOTAL`' in texture


def test_mirror_count_is_repainted_by_the_live_node_update():
    start = SCENE.index("function updateNetworkNodes(")
    update = SCENE[start: SCENE.index("\n  }\n", start)]
    # Counted from the cabinets actually placed in the ring, not the raw
    # payload, and only the nodes the signed record reports as answering.
    assert "setRewardPoolMirrorCount(" in update
    assert "usableNodes.length," in update
    assert 'String(node?.status || "").toLowerCase() === "online",' in update
    setter_start = SCENE.index("function setRewardPoolMirrorCount(")
    setter = SCENE[setter_start: SCENE.index("\n  }\n", setter_start)]
    # An empty catalog stays hidden rather than claiming zero mirrors.
    assert "sprite.visible = count > 0;" in setter
    # The catalog refresh is on a timer, so repaint only when a number moves.
    assert "if (mirrorCountShown === key) return;" in setter
    assert "sprite.material.map?.dispose?.();" in setter

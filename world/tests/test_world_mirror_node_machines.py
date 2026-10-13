import json
import re
import shutil
import subprocess
import types
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SRC = Path(__file__).resolve().parents[1] / "src"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
MIRROR_NODES = WORLD / "scene" / "mirror-nodes.js"
NETWORK = (WORLD / "scene" / "runtime" / "network.js").read_text(encoding="utf-8")
ANIMATE = (WORLD / "scene" / "runtime" / "animate.js").read_text(encoding="utf-8")
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CABINETS = MIRROR_NODES.read_text(encoding="utf-8")
ROUTES = (SRC / "world_routes.py").read_text(encoding="utf-8")
DURABLE = (SRC / "world_durable_objects.py").read_text(encoding="utf-8")
ENTRY = (
    Path(__file__).resolve().parents[2] / "app" / "src" / "entry.py"
).read_text(encoding="utf-8")
NODE = shutil.which("node")


def run_node(script):
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=60,
    )
    return json.loads(result.stdout)


def test_cabinets_are_black_and_share_their_materials():
    cabinet = CABINETS.split("export function createMirrorServerCabinet", 1)[1]
    assert 'makeSharedMaterial(THREE, online ? "#060707" : "#0e0f10", {' in cabinet
    assert 'makeSharedMaterial(THREE, "#1c1f21", {' in cabinet
    assert "group.userData.repeatedParts = repeatedParts;" in cabinet
    assert "#17221e" not in cabinet


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_gears_follow_services_and_sparks_come_from_a_fixed_pool():
    script = """
      const THREE = await import(%s);
      const nodes = await import(%s);
      const parent = new THREE.Group();
      const machines = nodes.createMirrorNodeMachines(THREE, parent);
      const healthy = new THREE.Group();
      healthy.position.set(3, 0.38, 4);
      healthy.rotation.y = 0.7;
      healthy.userData.nodeRecord = {
        online: true,
        endpoint: "https://node.example",
        endpointHealthy: true,
        operations: ["git-upload-pack", "tree", "merge-pull"],
        agentTasks: [{ id: "a" }],
      };
      const broken = new THREE.Group();
      broken.userData.nodeRecord = {
        online: true,
        endpoint: "https://node.example",
        endpointHealthy: false,
        operations: ["git-info-refs"],
      };
      const offline = new THREE.Group();
      offline.userData.nodeRecord = { online: false, operations: ["tree"] };
      parent.add(healthy, broken, offline);
      const cabinets = new Map([["a", healthy], ["b", broken], ["c", offline]]);
      machines.layout(cabinets);
      const services = [...cabinets.values()].map((cabinet) =>
        nodes.mirrorNodeServices(cabinet.userData.nodeRecord));
      const before = new THREE.Matrix4();
      machines.gears.getMatrixAt(0, before);
      machines.update(500);
      const after = new THREE.Matrix4();
      machines.gears.getMatrixAt(0, after);
      const still = new THREE.Matrix4();
      const stillIndex = services[0].length + 1;
      machines.gears.getMatrixAt(stillIndex, still);
      machines.update(900);
      const stillAfter = new THREE.Matrix4();
      machines.gears.getMatrixAt(stillIndex, stillAfter);
      const color = new THREE.Color();
      machines.gears.getColorAt(stillIndex, color);
      machines.queueSparks(healthy, 40, 1000, 0);
      let peak = 0;
      for (let time = 0; time < 9000; time += 16) {
        machines.update(time);
        peak = Math.max(peak, machines.stats().sparks);
      }
      console.log(JSON.stringify({
        services: services.map((list) => list.map((s) => [s.id, s.healthy])),
        stats: machines.stats(),
        count: machines.gears.count,
        spun: !before.equals(after),
        stopped: still.equals(stillAfter),
        red: color.r > color.g * 2,
        peak,
        pool: machines.sparks.count,
        sparksVisible: machines.sparks.visible,
        children: parent.children.length,
      }));
    """ % (json.dumps(THREE.as_uri()), json.dumps(MIRROR_NODES.as_uri()))
    data = run_node(script)
    assert data["services"] == [
        [["sync", True], ["git", True], ["browse", True], ["merge", True], ["agents", True]],
        [["sync", True], ["git", False]],
        [["sync", False], ["browse", False]],
    ]
    assert data["count"] == 9
    assert data["stats"]["spinning"] == 6
    assert data["spun"] and data["stopped"] and data["red"]
    assert 1 <= data["peak"] <= 12
    assert data["pool"] == 60
    assert data["stats"]["sparks"] == 0 and not data["sparksVisible"]
    assert data["stats"]["queued"] == 0
    assert data["children"] == 5


def test_the_yard_drives_gears_and_sparks_from_one_shared_update():
    assert "ensureNodeMachines()?.layout(ctx.nodeInfrastructure);" in NETWORK
    assert "fns.updateNodeMachines(time);" in ANIMATE
    assert "function noteMirrorTraffic(entries)" in NETWORK
    assert "MIRROR_POLL_SPARK_WINDOW_MS" in NETWORK
    assert 'message.type === "mirror-traffic"' in APP
    assert "this.world?.noteMirrorTraffic?.(" in APP
    assert "new THREE.InstancedMesh(" in CABINETS
    assert 'resources.geometry(THREE, "mirror-node-gear:v1"' in CABINETS


def test_the_router_batches_served_requests_into_one_signed_frame():
    assert "await world_note_mirror_traffic(env, served_node)" in ENTRY
    assert 'world_note_mirror_traffic = _world_routes.export("world_note_mirror_traffic")' in ENTRY
    assert "WORLD_MIRROR_TRAFFIC_FLUSH_MS = 10_000" in ROUTES
    assert '"/api/world/mirror-traffic"' in DURABLE
    assert '"type": "mirror-traffic"' in DURABLE
    assert "hmac.compare_digest(" in DURABLE.split("async def _mirror_traffic", 1)[1]
    assert "WORLD_MIRROR_TRAFFIC_MAX_AGE_MS" in DURABLE


def load_traffic_helpers():
    source = ROUTES.split("\nWORLD_MIRROR_TRAFFIC_FLUSH_MS", 1)[1]
    source = "WORLD_MIRROR_TRAFFIC_FLUSH_MS" + source.split(
        "\nasync def world_note_mirror_traffic", 1)[0]
    module = types.ModuleType("traffic")
    module.re = re
    exec(compile(source, "world_routes_traffic", "exec"), module.__dict__)
    return module


def test_traffic_counts_aggregate_and_flush_at_most_every_ten_seconds():
    traffic = load_traffic_helpers()
    state = {"counts": {}, "flushed_at": 0}
    first = traffic._world_mirror_traffic_take("Node-A", 20_000, state)
    assert first == [("node-a", 1)]
    for when in range(20_001, 20_050):
        assert traffic._world_mirror_traffic_take("node-a", when, state) == []
    assert traffic._world_mirror_traffic_take("node-b", 25_000, state) == []
    assert traffic._world_mirror_traffic_take("bad name!", 26_000, state) == []
    assert traffic._world_mirror_traffic_take("node-b", 30_000, state) == [
        ("node-a", 49), ("node-b", 2)]
    assert state["counts"] == {}


def test_traffic_entries_are_cleaned_bounded_and_canonical():
    traffic = load_traffic_helpers()
    entries = traffic._world_mirror_traffic_entries(
        [["b-node", 3], ["A-NODE", 2], ["x y", 5], ["c", -1], ["b-node", 10**9]])
    assert entries == [("a-node", 2), ("b-node", 10_000)]
    assert traffic._world_mirror_traffic_entries({"n1": 4}) == [("n1", 4)]
    assert traffic._world_mirror_traffic_entries("nope") == []
    many = [["n%d" % index, 1] for index in range(200)]
    assert len(traffic._world_mirror_traffic_entries(many)) == 64


def test_members_circle_lettering_is_fixed_and_the_sign_reads_from_both_sides():
    circle = (WORLD / "scene" / "districts" / "members-circle.js").read_text(
        encoding="utf-8")
    assert "orientMemberCircleLabels" not in circle
    assert "orientMemberCircleLabels" not in ANIMATE
    sign = circle.split("const memberCircleSignMaterial", 1)[1].split(
        "campfire.add(memberCircleInfo);", 1)[0]
    assert "THREE.DoubleSide" not in sign
    assert 'memberCircleInfoBack.name = "members-circle-member-info-back";' in sign

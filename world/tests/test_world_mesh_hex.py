import json
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

import world_mesh

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
HEXES = (WORLD / "scene" / "instance-hexes.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

HUB_ENV = SimpleNamespace(
    WORLD_ORIGIN="https://world.forkmesh.com",
    APP_ORIGIN="https://app.forkmesh.com",
    WORLD_MESH_HEX_SLOTS="0, 4,7,x",
)

SCRIPT = r"""
import * as l from "./scene/layout.js";
import * as m from "./world-mesh.js";
import {
  desertWalkSurfaceContains, setWorldMeshFlowers, worldMeshOwnerAt,
} from "./scene/movement.js";

const pitch = l.CONTINUOUS_CITY_RADIUS * 2 + l.WORLD_ROAD_WIDTH;
const near = (a, b) => Math.hypot(a.x - b.x, a.z - b.z);
const absolute = (offset) => ({
  x: l.WORLD_BIKE_LANE_CENTER_X + offset.x,
  z: l.CONTINUOUS_CITY_CENTER_Z + offset.z,
});
const out = {};
out.hexes = Array.from({ length: 6 }, (_, slot) => {
  const hex = l.worldMeshSlotOffset(slot, "hex");
  const angle = Math.PI / 6 + (slot * Math.PI) / 3;
  const flower = l.worldMeshSlotOffset(slot);
  const touching = l.WORLD_HEX_TILES.filter((tile) =>
    Math.abs(near(tile, absolute(hex)) - pitch) < 1e-6).length;
  const overlaps = Array.from({ length: 12 }, (_, other) => other)
    .filter((other) => other !== slot)
    .some((other) => {
      const center = l.worldMeshSlotOffset(other);
      return l.WORLD_MESH_FLOWER_HEXES.some((tile) =>
        near({ x: center.x + tile.x, z: center.z + tile.z }, hex) < pitch - 1e-6);
    });
  return {
    reach: Math.abs(near(hex, { x: 0, z: 0 }) - pitch * Math.sqrt(3)) < 1e-6,
    angle: Math.abs(Math.atan2(hex.z, hex.x) - Math.atan2(Math.sin(angle), Math.cos(angle))) < 1e-9,
    insideOwnFlower: l.WORLD_MESH_FLOWER_HEXES.some((tile) =>
      near({ x: flower.x + tile.x, z: flower.z + tile.z }, hex) < 1e-6),
    touching,
    overlaps,
    town: near(l.WORLD_HEX_TILES[0], absolute(hex)) > pitch,
  };
});
out.shapes = [
  l.worldMeshShape("hex", 0), l.worldMeshShape("hex", 5), l.worldMeshShape("hex", 6),
  l.worldMeshShape("flower", 0), l.worldMeshShape("hex", -1),
];
out.flowerUnchanged = JSON.stringify(l.worldMeshSlotOffset(7)) ===
  JSON.stringify(l.worldMeshSlotOffset(7, "hex"));
out.sizes = m.WORLD_MESH_HEX_APOTHEM === l.CONTINUOUS_CITY_RADIUS &&
  m.WORLD_MESH_ROAD_WIDTH === l.WORLD_ROAD_WIDTH;
out.hexCount = l.worldMeshHexesFor("hex").length;
out.flowerCount = l.worldMeshHexesFor("flower").length;

out.normalized = m.normalizeWorldMesh({
  self: "forkmesh",
  worlds: [
    { id: "forkmesh", world: "https://world.example.com", api: "https://app.example.com", slot: -1 },
    { id: "a".repeat(32), world: "https://code.example.org", api: "https://code.example.org", slot: 0, shape: "hex" },
    { id: "b".repeat(32), world: "https://far.example.org", api: "https://far.example.org", slot: 8, shape: "hex" },
  ],
}).worlds.map((world) => world.shape);

const hex = absolute(l.worldMeshSlotOffset(0, "hex"));
setWorldMeshFlowers([{ id: "a".repeat(32), x: hex.x, z: hex.z, shape: "hex", slot: 0 }]);
const flowerZero = absolute(l.worldMeshSlotOffset(0));
const desert = l.WORLD_HEX_TILES[1];
const toward = { x: (desert.x - hex.x) / pitch, z: (desert.z - hex.z) / pitch };
const road = (step) => ({ x: hex.x + toward.x * (pitch / 2 + step), z: hex.z + toward.z * (pitch / 2 + step) });
out.walk = {
  land: desertWalkSurfaceContains(hex.x, hex.z),
  owner: worldMeshOwnerAt(hex.x, hex.z)?.slot,
  noDesert: desertWalkSurfaceContains(flowerZero.x, flowerZero.z),
  sharedRoad: worldMeshOwnerAt(road(1).x, road(1).z) === null,
  hexRoad: worldMeshOwnerAt(road(-3).x, road(-3).z)?.slot,
  roadWalkable: desertWalkSurfaceContains(road(-3).x, road(-3).z),
};
setWorldMeshFlowers([]);

const visa = "eyJ2IjoxfQ." + "a".repeat(64);
out.urls = [
  m.worldMeshCrossingURL("https://code.example.org", { from: "forkmesh", slot: 0, local: { x: -371.04, z: 12 } }),
  m.worldMeshCrossingURL("https://code.example.org/", { from: "forkmesh", slot: 0, local: { x: 1, z: 2 }, visa }),
  m.worldMeshCrossingURL("https://code.example.org", { from: "forkmesh", slot: 0, local: { x: 900, z: -900 }, visa: "bad" }),
  m.worldMeshCrossingURL("http://code.example.org", { from: "forkmesh", slot: 0, local: { x: 1, z: 2 } }),
  m.worldMeshCrossingURL("https://code.example.org", { from: "<x>", slot: 0, local: { x: 1, z: 2 } }),
  m.worldMeshCrossingURL("https://code.example.org", { from: "forkmesh", slot: 0, local: { x: NaN, z: 2 } }),
];
out.arrivals = [
  "?from=code.example.org&slot=0&pos=-371,12.5",
  "?from=code.example.org&pos=1,2",
  "?from=code.example.org&slot=12&pos=1,2",
  "?from=code.example.org&slot=0&pos=401,2",
  "?from=<script>&slot=0&pos=1,2",
  "?portal=3",
].map(m.worldMeshArrival);
out.cleaned = [
  m.withoutWorldMeshArrival("https://world.example.com/world/?from=code.example.org&slot=0&pos=1,2&view=x#visa=" + visa),
  m.withoutWorldMeshArrival("https://world.example.com/world/?view=x#tab"),
];
const center = { x: 1000, z: 500 };
out.edge = [
  m.worldMeshHexEdgeArrival(center, { x: -371, z: 12 }),
  m.worldMeshHexEdgeArrival(center, { x: 10, z: 380 }),
  m.worldMeshHexEdgeArrival(center, { x: -371, z: 200 }),
  m.worldMeshHexEdgeArrival(center, { x: "a", z: 1 }),
];
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    run = subprocess.run([NODE, "--input-type=module", "-e", SCRIPT],
                         cwd=WORLD, capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    return json.loads(run.stdout)


def test_a_single_hexagon_sits_in_the_notch_beside_the_town(result):
    for slot, item in enumerate(result["hexes"]):
        assert item == {"reach": True, "angle": True, "insideOwnFlower": True,
                        "touching": 2, "overlaps": False, "town": True}, slot
    assert result["shapes"] == ["hex", "hex", "flower", "flower", "flower"]
    assert result["flowerUnchanged"] is True
    assert (result["hexCount"], result["flowerCount"]) == (1, 7)
    assert result["sizes"] is True


def test_only_slots_zero_to_five_can_be_single_hexagons(result):
    assert result["normalized"] == ["flower", "hex", "flower"]
    assert world_mesh.hex_slots(HUB_ENV) == frozenset({0, 4})
    worlds = world_mesh.build_directory(
        world_mesh.hub_entry(HUB_ENV),
        [{"id": "a" * 32, "name": "Example Code",
          "url": "https://code.example.org/", "slot": 0},
         {"id": "b" * 32, "name": "Other",
          "url": "https://other.example.org/", "slot": 3}],
        "https://app.forkmesh.com", world_mesh.hex_slots(HUB_ENV))
    assert [entry["shape"] for entry in worlds] == ["flower", "hex", "flower"]
    payload = world_mesh.directory_payload(HUB_ENV, worlds)
    assert [entry["shape"] for entry in
            world_mesh.normalize_directory(payload)] == [
                "flower", "hex", "flower"]
    forged = dict(worlds[2], shape="hex", slot=9)
    assert world_mesh.normalize_world(forged)["shape"] == "flower"
    assert world_mesh.hex_slots(SimpleNamespace()) == frozenset()


def test_the_hexagon_is_walkable_only_on_its_own_land(result):
    assert result["walk"] == {"land": True, "owner": 0, "noDesert": False,
                              "sharedRoad": True, "hexRoad": 0,
                              "roadWalkable": True}


def test_walking_on_carries_origin_slot_position_and_visa(result):
    urls = result["urls"]
    assert urls[0] == ("https://code.example.org/world/?from=forkmesh&slot=0"
                       "&pos=-371%2C12")
    assert urls[1] == ("https://code.example.org/world/?from=forkmesh&slot=0"
                       "&pos=1%2C2#visa=eyJ2IjoxfQ." + "a" * 64)
    assert urls[2] == ("https://code.example.org/world/?from=forkmesh&slot=0"
                       "&pos=400%2C-400")
    assert urls[3:] == ["", "", ""]


def test_arrivals_are_sanitized_and_land_outside_the_crossed_edge(result):
    arrivals = result["arrivals"]
    assert arrivals[0] == {"from": "code.example.org", "slot": 0,
                           "pos": {"x": -371, "z": 12.5}}
    assert arrivals[1:] == [None, None, None, None, None]
    assert result["cleaned"] == [
        "https://world.example.com/world/?view=x",
        "https://world.example.com/world/?view=x#tab"]
    west, south, clamped, bad = result["edge"]
    assert (round(west["x"]), round(west["z"])) == (1000 - 389, 512)
    assert abs(west["heading"] - 1.5707963267948966) < 1e-9
    assert west["space"] == "town-square"
    # (10, 380) is nearest the 60-degree edge: the walker lands 389 out
    # along that edge's normal.
    normal = (0.5, 3 ** 0.5 / 2)
    depth = (south["x"] - 1000) * normal[0] + (south["z"] - 500) * normal[1]
    assert round(depth) == 389
    assert round(clamped["z"]) == 500 + 150
    assert bad is None


def test_the_world_draws_and_crosses_single_hexagons():
    assert "WORLD_MESH_HEX_SHAPE" in HEXES
    assert 'single ? "world-mesh-hex-road" : "world-mesh-flower-road"' in HEXES
    assert 'placed.filter((world) => world.shape !== "hex")' in APP
    assert "if (owner.onLand) void this.crossIntoWorldMeshHex(owner);" in APP
    assert "worldMeshCrossingURL(owner.world, { from, slot: owner.slot, local, visa })" in APP
    assert "this.pendingMeshArrival = worldMeshArrival(location.search);" in APP
    assert "worldMeshHexEdgeArrival(hex, arrival.pos)" in APP


def test_the_mesh_layer_is_not_redrawn_from_the_legacy_portal_list():
    # Only the no-directory fallback still draws the legacy portal list; after
    # a mesh payload, placeWorldMesh() owns the neighbour layer.
    assert APP.count("this.world?.updateInstanceHexes?.(this.instanceHexes);") == 1
    assert "if (!arrival || !this.world?.setSpawn || !this.worldMesh?.self) return;" in APP

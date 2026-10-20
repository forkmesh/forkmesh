import json
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

import world_mesh

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
HEXES = (WORLD / "scene" / "instance-hexes.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

SCRIPT = r"""
import * as s from "./world-neighbour-scene.js";
import * as m from "./world-mesh.js";

const good = {
  v: 1,
  ground: { color: "#3A4550", accent: "#e74c3c", pattern: "circuit" },
  objects: [
    { type: "plaza", x: 0, z: 85, radius: 26, color: "#46525d" },
    { type: "sign", x: 0, z: 63, title: "Acme Code", text: "> hello_" },
    { type: "house", x: -60, z: 75, facing: 1.2, label: "Club <b>House</b>",
      url: "https://club.example.org/", color: "#e74c3c" },
    { type: "house", x: 10, z: 10, label: "Bad link", url: "javascript:alert(1)" },
    { type: "terrarium", x: 60, z: 75, facing: -1.4, label: "Acme bugs", bugs: [
      { label: "shop.example.com", icon: "https://code.example.org/i.png", url: "https://code.example.org/d" },
      { label: "x.example.com", icon: "http://code.example.org/i.png" },
      { label: "y.example.com", icon: "https://tracker.example.net/i.png" },
      { label: "", icon: "https://code.example.org/i.png" },
      ...Array.from({ length: 40 }, (_, i) => ({ label: `b${i}.example.com` })),
    ] },
    { type: "lamp", x: 20, z: 99, color: "red" },
    { type: "tree", x: 100, z: 100, scale: 9 },
    { type: "script", x: 0, z: 0 },
    { type: "house", x: 900, z: 0, label: "Far" },
    { type: "house", x: "a", z: 0, label: "NaN" },
    null,
  ],
};
const out = {};
out.good = s.normalizeNeighbourScene(good, "https://code.example.org");
out.noOrigin = s.normalizeNeighbourScene(good).objects[4].bugs[0].icon;
out.bad = [
  s.normalizeNeighbourScene(null),
  s.normalizeNeighbourScene({ v: 2, objects: [] }),
  s.normalizeNeighbourScene("<script>"),
];
out.many = s.normalizeNeighbourScene({ v: 1, objects: Array.from({ length: 99 }, () => ({ type: "lamp", x: 0, z: 0 })) }).objects.length;
out.url = [
  s.neighbourSceneURL("https://code.example.org"),
  s.neighbourSceneURL("http://code.example.org"),
  s.neighbourSceneURL("https://user:pw@code.example.org"),
];
const reply = (status, body, length = "") => async () => ({
  ok: status === 200, status, headers: new Headers(length ? { "content-length": length } : {}),
  text: async () => body,
});
out.loaded = await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene",
  { fetchImpl: reply(200, JSON.stringify(good)) });
out.refused = [
  await s.loadNeighbourScene("https://code.example.org/other", { fetchImpl: reply(200, JSON.stringify(good)) }),
  await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene", { fetchImpl: reply(404, "") }),
  await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene", { fetchImpl: reply(200, "x".repeat(70000)) }),
  await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene", { fetchImpl: reply(200, "not json") }),
  await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene", { fetchImpl: reply(200, "{}", "70000") }),
  await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene", { fetchImpl: reply(200, `"${"é".repeat(40000)}"`) }),
  await s.loadNeighbourScene("https://code.example.org/api/world/neighbour-scene", { fetchImpl: async () => { throw new Error("cors"); } }),
];
out.mesh = m.normalizeWorldMesh({ self: "forkmesh", worlds: [
  { id: "forkmesh", world: "https://world.example.com", api: "https://app.example.com", slot: -1 },
  { id: "a".repeat(32), world: "https://code.example.org", api: "https://code.example.org", slot: 0, shape: "hex",
    scene: "https://code.example.org/api/world/neighbour-scene" },
  { id: "b".repeat(32), world: "https://two.example.org", api: "https://two.example.org", slot: 1, shape: "hex",
    scene: "https://evil.example.net/api/world/neighbour-scene" },
] }).worlds.map((world) => world.scene);
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


def test_a_neighbour_scene_is_plain_checked_data(result):
    scene = result["good"]
    assert scene["ground"] == {"color": "#3a4550", "accent": "#e74c3c",
                               "pattern": "circuit"}
    types = [item["type"] for item in scene["objects"]]
    assert types == ["plaza", "sign", "house", "house", "terrarium", "lamp",
                     "tree"]
    house = scene["objects"][2]
    assert house["label"] == "Club bHouse/b"
    assert house["url"] == "https://club.example.org/"
    assert scene["objects"][3]["url"] == ""
    bugs = scene["objects"][4]["bugs"]
    assert len(bugs) == 32
    assert bugs[0] == {"label": "shop.example.com",
                       "icon": "https://code.example.org/i.png",
                       "url": "https://code.example.org/d"}
    assert bugs[1]["icon"] == ""
    assert bugs[2] == {"label": "y.example.com", "icon": "", "url": ""}
    assert result["noOrigin"] == ""
    assert scene["objects"][5]["color"] == "#fff3c4"
    assert scene["objects"][6]["scale"] == 2
    assert result["bad"] == [None, None, None]
    assert result["many"] == 48


def test_only_the_fixed_https_path_is_fetched_and_failures_fall_back(result):
    assert result["url"] == [
        "https://code.example.org/api/world/neighbour-scene", "", ""]
    assert result["loaded"] == result["good"]
    assert result["refused"] == [None] * 7
    assert result["mesh"] == [
        "", "https://code.example.org/api/world/neighbour-scene", ""]


def test_the_hub_directory_names_the_scene_of_single_hexagons():
    env = SimpleNamespace(WORLD_ORIGIN="https://world.forkmesh.com",
                          APP_ORIGIN="https://app.forkmesh.com",
                          WORLD_MESH_HEX_SLOTS="0")
    worlds = world_mesh.build_directory(
        world_mesh.hub_entry(env),
        [{"id": "a" * 32, "name": "Acme", "url": "https://code.example.org/",
          "slot": 0},
         {"id": "b" * 32, "name": "Two", "url": "https://two.example.org/",
          "slot": 2}],
        "https://app.forkmesh.com", world_mesh.hex_slots(env))
    assert [entry.get("scene") for entry in worlds] == [
        None, "https://code.example.org/api/world/neighbour-scene", None]
    relayed = world_mesh.normalize_directory(
        world_mesh.directory_payload(env, worlds))
    assert relayed[1]["scene"] == worlds[1]["scene"]
    forged = dict(worlds[1], scene="https://evil.example.net/x")
    assert "scene" not in world_mesh.normalize_world(forged)


def test_the_scene_is_drawn_on_the_hexagon_or_the_logo_stays():
    assert "if (single && instance.scene) {" in HEXES
    assert "if (!scene || !group.parent?.parent) return;" in HEXES
    assert "group.add(buildNeighbourScene(THREE, scene));" in HEXES
    assert 'image.crossOrigin = "anonymous";' in HEXES
    assert "innerHTML" not in HEXES

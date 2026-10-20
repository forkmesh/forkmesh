import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
POINTER = (WORLD / "scene" / "runtime" / "pointer.js").read_text(encoding="utf-8")
WALK = (WORLD / "scene" / "runtime" / "walk-player.js").read_text(encoding="utf-8")
HEXES = (WORLD / "scene" / "instance-hexes.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

SCRIPT = r"""
import * as f from "./scene/neighbour-footprints.js";
import { neighbourSceneFootprints } from "./scene/instance-hexes.js";
import { normalizeNeighbourScene } from "./world-neighbour-scene.js";

const scene = normalizeNeighbourScene({ v: 1, ground: {}, objects: [
  { type: "house", x: -60, z: 75, facing: 0, label: "Club House", url: "https://club.example.org/" },
  { type: "terrarium", x: 60, z: 75, facing: 0, label: "Bugs", bugs: [] },
  { type: "lamp", x: 0, z: 0 },
] });
const center = { x: 1000, z: 500 };
const placed = neighbourSceneFootprints(scene).map((item) =>
  f.neighbourFootprint({ ...item, x: center.x + item.x, z: center.z + item.z }));
f.setNeighbourFootprints("a".repeat(32), placed);
const all = f.neighbourFootprints();
const at = (x, z) => ({ x: center.x + x, z: center.z + z });
const move = (from, to) => {
  const result = f.constrainToFootprints(all, at(...from), at(...to), 0.46);
  return { blocked: result.blocked, door: result.door?.link || null,
    x: Math.round((result.x - center.x) * 100) / 100, z: Math.round((result.z - center.z) * 100) / 100 };
};
const out = {
  kinds: neighbourSceneFootprints(scene).map((item) => [item.hollow, item.link?.url || "", item.door]),
  count: all.length,
  houseDoor: move([-60, 75 + 6.6], [-60, 75 + 6.2]),
  houseWall: move([-55, 75 + 6.6], [-55, 75 + 6.2]),
  tankIn: move([60, 75 + 9.5], [60, 75 + 6]),
  tankWall: move([60, 75 - 9], [60, 75 - 7]),
};
f.setNeighbourFootprints("a".repeat(32), []);
out.cleared = f.neighbourFootprints().length;
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


def test_a_scenes_buildings_have_walls_and_doors(result):
    assert result["kinds"] == [[False, "https://club.example.org/", 3.4],
                               [True, "", 4.4]]
    assert result["count"] == 2
    assert result["cleared"] == 0


def test_a_portal_houses_door_opens_and_its_walls_stop_walkers(result):
    assert result["houseDoor"]["blocked"] is True
    assert result["houseDoor"]["door"] == {"url": "https://club.example.org/",
                                           "label": "Club House"}
    assert result["houseWall"]["blocked"] is True
    assert result["houseWall"]["door"] is None


def test_the_terrarium_is_entered_only_through_its_doorway(result):
    assert result["tankIn"] == {"blocked": False, "door": None, "x": 60,
                                "z": 81}
    assert result["tankWall"]["blocked"] is True
    assert result["tankWall"]["z"] == 75 - 9


def test_the_hub_page_wires_doors_walls_and_bug_cards():
    assert "constrainNeighbourWalls(previousHorizontalPosition);" in WALK
    assert "fns.onNeighbourDoor?.({ ...result.door.link });" in WALK
    assert '"neighbour-door"' in POINTER and '"neighbour-bug"' in POINTER
    assert "setNeighbourFootprints(instance.id, neighbourSceneFootprints(scene)" \
        in HEXES
    door = APP.split("  openNeighbourDoor({ url, label } = {}) {")[1].split(
        "\n  }\n")[0]
    assert "neighbourSceneLink(url)" in door
    assert "window.location.assign(destination);" in door
    card = APP.split("  showNeighbourBug(bug = {}) {")[1].split("\n  }\n")[0]
    assert "innerHTML" not in card
    assert 'link.rel = "noopener noreferrer";' in card
    assert "neighbourSceneLink(bug.url)" in card

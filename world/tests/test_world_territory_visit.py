import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
NODE = shutil.which("node")
BLT = "f455c98c739e5b0f7348dfd5914d2eac"

SCRIPT = r"""
const context = new Proxy({}, {
  get: (target, key) => {
    if (key === "measureText") return () => ({ width: 10 });
    if (key === "createLinearGradient" || key === "createRadialGradient" || key === "createPattern") {
      return () => ({ addColorStop() {} });
    }
    if (key === "getImageData") return () => ({ data: new Uint8ClampedArray(4) });
    if (key in target) return target[key];
    return () => {};
  },
  set: (target, key, value) => {
    target[key] = value;
    return true;
  },
});
globalThis.document = {
  createElement: () => ({ width: 0, height: 0, style: {}, getContext: () => context }),
};
globalThis.Image = class {
  set src(value) {
    images.push({ src: value, crossOrigin: this.crossOrigin || "", referrer: this.referrerPolicy || "" });
  }
};
const images = [];
globalThis.performance ||= { now: () => Date.now() };

const THREE = await import(__THREE__);
const territoryRuntime = await import("./scene/instance-territory.js");
const geometry = await import("./scene/territory-geometry.js");
const { territoryWalkActive, desertWalkSurfaceContains } = await import("./scene/movement.js");
const { territoryForNeighbour, TERRITORY_HIDDEN_ELEMENTS } = await import("./world-territory.js");

const world = new THREE.Group();
const town = new THREE.Group();
town.add(new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1)));
const townButton = town.children[0];
const forkbot = new THREE.Group();
const disabled = new Set(["forkbot"]);
const elements = new Map([
  ["city-terrain", { roots: new Set([town]) }],
  ["leaderboard-district", { roots: new Set() }],
]);
const petals = [];
const ctx = {
  world,
  interactive: [townButton],
  animated: [],
  worldElements: elements,
  desertPlots: { userData: { replacePetals: (list) => petals.push([...list]) } },
  groundSurfaces: [],
  player: { position: { x: 0, y: 0, z: 0 } },
};
const toggles = [];
const moves = [];
const fns = {
  registerWorldElement() {},
  moveCompactDistrictsWithin(root, dx, dz) {
    moves.push([root === town, dx, dz]);
  },
  worldElementEnabled: (id) => !disabled.has(id),
  setWorldElementEnabled(id, on) {
    toggles.push([id, on]);
    if (on) disabled.delete(id);
    else disabled.add(id);
    return true;
  },
};
const runtime = territoryRuntime.functions({ THREE, ctx, fns, territory: null, onTerritoryCross() {} });

const territory = territoryForNeighbour(
  { id: __BLT__, shape: "hex", slot: 0, host: "git.owaspblt.org", world: "https://git.owaspblt.org", logo: "" },
  {
    branded: true, name: "OWASP BLT",
    world: {
      style: "security",
      objects: [{ type: "house", label: "Hacker House", url: "https://hackerhouse.owaspblt.org/", position: [-60, -10] }],
      bugs: { repo: "owasp-blt/BLT", path: ".forkmesh/world/bugs.json", position: [60, -10] },
    },
  },
  { objects: [{ type: "terrarium", bugs: [{ label: "aaa.com", icon: "https://git.owaspblt.org/f/aaa.png", url: "" }] }] },
  (icon) => `/api/world/neighbour-icon?path=${encodeURIComponent(new URL(icon).pathname)}`,
);

const out = {};
const shift = geometry.forkmeshShiftAround(geometry.territoryPetalTile(1));
out.prepared = runtime.prepareTerritoryVisit(territory);
const standby = world.children.find((child) => child.name === "instance-territory");
out.standby = {
  inWorld: Boolean(standby),
  shown: standby?.visible,
  ground: standby?.userData.groundStandby,
  clickable: ctx.interactive.some((item) => item.userData?.interactive === "territory-link"),
  town: { x: town.position.x, z: town.position.z },
  walk: territoryWalkActive(),
  visiting: runtime.territoryVisitId(),
};
out.entered = runtime.enterTerritoryVisit(territory);
runtime.updateTerritoryVisitBugs(territory.id, territory.visitBugs);
const root = world.children.find((child) => child.name === "instance-territory");
out.visit = {
  built: Boolean(root),
  reused: root === standby,
  shown: root?.visible,
  ground: root?.userData.groundStandby,
  moves: moves.slice(),
  houses: root ? root.getObjectsByProperty("name", "territory-house").length : 0,
  terrarium: root ? root.getObjectsByProperty("name", "territory-terrarium").length : 0,
  shield: root ? root.getObjectsByProperty("name", "territory-shield").length : 0,
  town: { x: town.position.x, z: town.position.z },
  shift,
  townClickable: ctx.interactive.includes(townButton),
  territoryClickable: ctx.interactive.some((item) => item.userData?.interactive === "territory-link"),
  walk: territoryWalkActive(),
  onTerritory: desertWalkSurfaceContains(geometry.TERRITORY_CENTER.x, geometry.TERRITORY_CENTER.z),
  owner: runtime.territoryOwnerAt(geometry.TERRITORY_CENTER.x + shift.x, geometry.TERRITORY_CENTER.z + shift.z),
  ownerHere: runtime.territoryOwnerAt(geometry.TERRITORY_CENTER.x, geometry.TERRITORY_CENTER.z),
  shiftApi: runtime.territoryForkmeshShift(),
  petals: petals.at(-1),
  hidden: toggles.filter(([, on]) => !on).map(([id]) => id),
  visiting: runtime.territoryVisitId(),
  images: images.map((image) => [image.src, image.crossOrigin, image.referrer]),
};
out.again = runtime.enterTerritoryVisit(territory);
out.againPetals = petals.at(-1);
out.left = runtime.leaveTerritoryVisit();
out.home = {
  inWorld: world.children.includes(root),
  shown: root.visible,
  ground: root.userData.groundStandby,
  moves: moves.slice(),
  town: { x: town.position.x, z: town.position.z },
  townClickable: ctx.interactive.includes(townButton),
  territoryClickable: ctx.interactive.some((item) => item.userData?.interactive === "territory-link"),
  walk: territoryWalkActive(),
  restored: toggles.filter(([, on]) => on).map(([id]) => id),
  forkbotStillOff: disabled.has("forkbot"),
  owner: runtime.territoryOwnerAt(0, 0),
  shiftApi: runtime.territoryForkmeshShift(),
  visiting: runtime.territoryVisitId(),
};
runtime.enterTerritoryVisit(territory);
out.reentered = {
  same: world.children.includes(root),
  town: { x: town.position.x, z: town.position.z },
  territoryClickable: ctx.interactive.some((item) => item.userData?.interactive === "territory-link"),
};
runtime.leaveTerritoryVisit();
out.leftTwice = runtime.leaveTerritoryVisit();
const instancePage = territoryRuntime.functions({ THREE, ctx, fns, territory, onTerritoryCross() {} });
out.instancePage = instancePage.enterTerritoryVisit(territory);
TERRITORY_HIDDEN_ELEMENTS.forEach((id) => disabled.add(id));
const ownPage = territoryRuntime.functions({ THREE, ctx, fns, territory: { ...territory }, onTerritoryCross() {} });
const ownBuilt = ownPage.installInstanceTerritory();
const forkmeshTown = { x: geometry.TERRITORY_CENTER.x + shift.x, z: geometry.TERRITORY_CENTER.z + shift.z };
const ownView = () => ({
  town: { x: town.position.x, z: town.position.z },
  shown: ownBuilt.root.visible,
  clickable: ctx.interactive.some((item) => item.userData?.interactive === "territory-link"),
  walk: territoryWalkActive(),
  owner: ownPage.territoryOwnerAt(forkmeshTown.x, forkmeshTown.z),
  off: TERRITORY_HIDDEN_ELEMENTS.filter((id) => disabled.has(id)),
});
out.own = { start: ownView() };
out.own.left = ownPage.leaveOwnTerritory(["forkbot"]);
out.own.away = ownView();
out.own.leftAgain = ownPage.leaveOwnTerritory(["forkbot"]);
out.own.entered = ownPage.enterOwnTerritory();
out.own.back = ownView();
out.own.visitPage = runtime.leaveOwnTerritory([]);
out.hiddenList = TERRITORY_HIDDEN_ELEMENTS;
process.stdout.write(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def result():
    if NODE is None:
        pytest.skip("node is required")
    script = (SCRIPT.replace("__THREE__", json.dumps(THREE.as_uri()))
              .replace("__BLT__", json.dumps(BLT)))
    run = subprocess.run([NODE, "--input-type=module", "-e", script],
                         cwd=WORLD, capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    return json.loads(run.stdout)


def test_the_hexagon_is_built_hidden_before_anyone_steps_in(result):
    standby = result["standby"]
    assert result["prepared"] is True
    assert standby["inWorld"] is True
    assert standby["shown"] is False
    assert standby["ground"] is True
    assert standby["clickable"] is False
    assert standby["town"] == {"x": 0, "z": 0}
    assert standby["walk"] is False
    assert standby["visiting"] == ""
    assert result["visit"]["reused"] is True
    assert result["visit"]["shown"] is True
    assert result["visit"]["ground"] is False


def test_compact_districts_follow_the_town_next_door_and_back(result):
    shift = result["visit"]["shift"]
    assert result["visit"]["moves"] == [[True, pytest.approx(shift["x"]), pytest.approx(shift["z"])]]
    assert result["home"]["moves"][-1] == [True, pytest.approx(-shift["x"]), pytest.approx(-shift["z"])]


def test_standing_in_a_neighbour_builds_its_own_hexagon(result):
    visit = result["visit"]
    assert result["entered"] is True
    assert visit["built"] is True
    assert visit["houses"] == 1
    assert visit["terrarium"] == 1
    assert visit["shield"] == 1
    assert visit["territoryClickable"] is True
    assert visit["visiting"] == BLT


def test_forkmesh_moves_next_door_and_its_own_pieces_step_aside(result):
    visit = result["visit"]
    assert visit["town"] == pytest.approx(visit["shift"])
    assert visit["townClickable"] is False
    assert visit["walk"] is True
    assert visit["onTerritory"] is True
    assert visit["owner"] == "forkmesh"
    assert visit["ownerHere"] == ""
    assert visit["shiftApi"]["x"] == pytest.approx(visit["shift"]["x"])
    assert visit["petals"] == [1]
    assert result["againPetals"] == [1]
    assert "forkbot" not in visit["hidden"]
    assert set(visit["hidden"]) == set(result["hiddenList"]) - {"forkbot"}


def test_the_logo_and_bug_icons_load_through_cors(result):
    images = result["visit"]["images"]
    assert ["/api/world/neighbour-icon?path=%2Ff%2Faaa.png", "anonymous", "no-referrer"] in images
    assert all(crossorigin == "anonymous" for _, crossorigin, _ in images)


def test_walking_home_puts_everything_back(result):
    home = result["home"]
    assert result["left"] is True
    assert home["inWorld"] is True
    assert home["shown"] is False
    assert home["ground"] is True
    assert home["town"] == {"x": 0, "z": 0}
    assert home["townClickable"] is True
    assert home["territoryClickable"] is False
    assert home["walk"] is False
    assert set(home["restored"]) == set(result["visit"]["hidden"])
    assert home["forkbotStillOff"] is True
    assert home["owner"] == ""
    assert home["shiftApi"] is None
    assert home["visiting"] == ""
    assert result["leftTwice"] is False


def test_coming_back_reuses_the_built_hexagon(result):
    again = result["reentered"]
    assert again["same"] is True
    assert again["town"] == pytest.approx(result["visit"]["shift"])
    assert again["territoryClickable"] is True


def test_an_instance_page_keeps_its_own_territory(result):
    assert result["instancePage"] is False


def test_an_instance_page_hands_its_ground_to_forkmesh_and_takes_it_back(result):
    own = result["own"]
    shift = result["visit"]["shift"]
    start, away, back = own["start"], own["away"], own["back"]
    assert start["town"] == pytest.approx(shift)
    assert start["shown"] is True and start["clickable"] is True and start["walk"] is True
    assert start["owner"] == "forkmesh"
    assert set(start["off"]) == set(result["hiddenList"])
    assert own["left"] is True
    assert away["town"] == {"x": 0, "z": 0}
    assert away["shown"] is False and away["clickable"] is False and away["walk"] is False
    assert away["owner"] == ""
    assert away["off"] == ["forkbot"]
    assert own["leftAgain"] is False
    assert own["entered"] is True
    assert back == start
    assert own["visitPage"] is False

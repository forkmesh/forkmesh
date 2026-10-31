import json
import shutil
import subprocess
from pathlib import Path

import pytest

import world_mesh

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
NETWORK = (WORLD / "scene" / "runtime" / "network.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

HUB = "forkmesh"
BLT = "f455c98c739e5b0f7348dfd5914d2eac"
ACME = "0a1b2c3d4e5f60718293a4b5c6d7e8f9"
WORLDS = [
    {"id": HUB, "name": "ForkMesh", "world": "https://world.forkmesh.com",
     "api": "https://app.forkmesh.com", "slot": -1, "shape": "flower"},
    {"id": BLT, "name": "OWASP BLT", "world": "https://git.owaspblt.org",
     "api": "https://git.owaspblt.org", "slot": 0, "shape": "hex",
     "scene": "https://git.owaspblt.org/api/world/neighbour-scene"},
    {"id": ACME, "name": "Acme", "world": "https://world.acme.dev",
     "api": "https://app.acme.dev", "slot": 3, "shape": "flower"},
]


def block(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def scene_function(name):
    start = f"  function {name}("
    begin = NETWORK.index(start)
    return NETWORK[begin:NETWORK.index("\n  }\n", begin) + 4]


METHODS = "\n".join([
    block(APP, "  async fetchMeshRootJSON(", "\n  async postJSON("),
    block(APP, "  placeTerritoryNeighbour() {", "\n  applyWorldMeshArrival() {"),
    block(APP, "  syncWorldMeshPosition(", "\n  openNeighbourDoor("),
    block(APP, "  fetchJSONAt(", "\n  async requestWorldVisa("),
    block(APP, "  meshPeerPlayers() {", "\n  celebrateRecentInstance("),
])

SCENE = scene_function("updateInstanceHexes") + scene_function("worldMeshOwner")

SCRIPT = r"""
import * as l from "./scene/layout.js";
import {
  WORLD_MESH_OUTDOOR_SPACES, WORLD_MESH_SPACE, WorldMeshLinks,
  normalizeWorldMesh, worldMeshHost,
} from "./world-mesh.js";
import {
  roundedHexagonContains, setWorldMeshFlowers, worldMeshOwnerAt,
  worldMeshReplacedPetals,
} from "./scene/movement.js";
import { meshRootRoute } from "./world-federation.js";
import { territoryForNeighbour } from "./world-territory.js";
import { neighbourIconURL } from "./world-neighbour-scene.js";
import {
  forkmeshShiftAround, forkmeshTilesAround, hexRoadContains, territoryPetalTile,
} from "./scene/territory-geometry.js";

globalThis.window = globalThis;
globalThis.document = { hidden: false };
globalThis.location = { host: "world.forkmesh.com" };
const {
  CONTINUOUS_CITY_CENTER_Z, CONTINUOUS_CITY_CORNER_RADIUS, CONTINUOUS_CITY_RADIUS,
  WORLD_BIKE_LANE_CENTER_X, WORLD_MESH_HEX_SHAPE, worldMeshSlotOffset,
} = l;
const WORLD_MESH_SWITCH_MARGIN = 8;
const FRESH_ARRIVAL_CAMPFIRE_PREVIEW = { x: 0, y: 0.38, z: 114.8, heading: Math.PI, space: "town-square" };
const THREE = { Group: class { add() {} } };
const createInstanceHex = () => null;
const clearNeighbourFootprints = () => {};
const removeInteractiveObject = () => {};
const disposeObject3D = () => {};
const ctx = { world: { userData: {}, add() {}, remove() {} }, interactive: [] };
const fns = { registerWorldElement() {} };
const BLT_SCENE = {
  v: 1,
  ground: { color: "#3a4550", accent: "#e74c3c", pattern: "circuit" },
  objects: [{
    type: "terrarium", x: 60, z: 75, facing: 0, label: "OWASP BLT bugs",
    bugs: [
      { label: "aaa.com", icon: "https://git.owaspblt.org/api/repo/owasp-blt/BLT/raw?path=f%2Faaa.com.png&ref=main", url: "https://legacy.owaspblt.org/domain/aaa.com/" },
      { label: "aao.org", icon: "", url: "" },
    ],
  }],
};
const BLT_INSTANCE = {
  ok: true, name: "OWASP BLT", branded: true, logo: "/instance-logo.png",
  directory: { listed: true, world: true, portal: { home: "https://world.forkmesh.com/world/", slot: 0 } },
  world: {
    style: "security", accent: "#e74c3c", logo: "/instance-logo.png",
    objects: [{ type: "house", label: "Hacker House", url: "https://hackerhouse.owaspblt.org/", position: [-60, -10] }],
    bugs: { repo: "owasp-blt/BLT", ref: "main", path: ".forkmesh/world/bugs.json", title: "OWASP BLT bugs", limit: 60, position: [60, -10] },
  },
};
const scenes = [];
globalThis.fetch = async (url, options) => {
  territoryReads.push({ url, options });
  return { ok: true, json: async () => BLT_INSTANCE };
};
const loadNeighbourScene = async (url) => {
  scenes.push(url);
  return BLT_SCENE;
};
const remotePlayer = (peer) => ({
  id: String(peer.id), name: String(peer.name), activity: peer.activity || "online",
  category: peer.category || "", x: 0, y: 0, z: 0,
});
__SCENE__

class View {
__METHODS__
}

const sockets = [];
const federated = [];
const territoryReads = [];
const own = [];
function view(self, x, z) {
  const player = { x, z };
  const v = Object.assign(new View(), {
    destroyed: false,
    meshRoot: null,
    meshRootGeneration: 0,
    meshPlaced: [],
    meshSwitching: false,
    meshLinks: null,
    meshVisitId: "",
    officeOwner: null,
    notices: [],
    reloads: 0,
    saved: [],
    restoredPosition: null,
    meshRestoreChecked: false,
    meshTerritories: new Map(),
    player,
    lastMovement: { x, z, space: "town-square" },
    worldMesh: normalizeWorldMesh({ self, worlds: __WORLDS__ }),
    world: {
      updateInstanceHexes, worldMeshOwner,
      getPosition: () => ({ x: player.x, z: player.z, space: "town-square" }),
      setSpawn(spawn) {
        player.x = spawn.x;
        player.z = spawn.z;
        return true;
      },
      shiftPlayer(dx, dz) {
        player.x += dx;
        player.z += dz;
        return true;
      },
      visiting: null,
      visitBugs: null,
      enterTerritoryVisit(territory) {
        this.visiting = territory;
        return true;
      },
      leaveTerritoryVisit() {
        this.visiting = null;
        return true;
      },
      updateTerritoryVisitBugs(id, list) {
        this.visitBugs = { id, count: list.length, first: list[0] };
      },
      territoryForkmeshShift() {
        const shift = this.visiting
          ? forkmeshShiftAround(territoryPetalTile(this.visiting.petal))
          : null;
        return shift
          ? { ...shift, townX: home.x + shift.x, townZ: home.z + shift.z }
          : null;
      },
      territoryOwnerAt(x, z) {
        if (!this.visiting) return "";
        return forkmeshTilesAround(territoryPetalTile(this.visiting.petal))
          .some((tile) => hexRoadContains(x, z, tile, 5)) ? "forkmesh" : "";
      },
    },
  });
  v.worldMeshLinks = function () {
    this.meshLinks ||= new WorldMeshLinks({
      onVisitChanged: (world, previous) => this.handleWorldMeshVisit(world, previous),
      requestVisa: async () => ({ visitor: "fmwv1.token" }),
      createSocket: (url) => {
        const socket = { url, readyState: 0, addEventListener() {}, close() {}, send() {} };
        sockets.push(socket);
        return socket;
      },
    });
    return this.meshLinks;
  };
  v.applyWorldMeshArrival = () => {};
  v.rememberWorldPosition = (position) => { v.saved.push({ x: position.x, z: position.z }); };
  v.hudNotice = (text) => v.notices.push(text);
  v.setOfficeOwner = (owner) => { v.officeOwner = owner?.id || null; };
  v.reloadMeshRootData = () => { v.reloads += 1; };
  v.sendPresenceNow = () => {};
  v.schedulePeerRender = () => {};
  v.worldFederation = () => ({
    fetchJSON: async (world, path, options) => {
      federated.push({ world: world.id, path, options });
      return { from: world.id };
    },
  });
  v.fetchJSON = async (path, options = {}) => {
    own.push({ path, options });
    return { from: "home" };
  };
  return v;
}

const walk = (v, x, z) => {
  v.lastMovement = { ...v.lastMovement, x, z };
  v.syncWorldMeshPosition();
};
const links = (v) => [...v.meshLinks.links.values()].map((link) => [link.id, link.mode]);
const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

const out = {};
const home = { x: WORLD_BIKE_LANE_CENTER_X, z: CONTINUOUS_CITY_CENTER_Z };
const hub = view("forkmesh", home.x, home.z);
hub.placeWorldMesh();
await flush();
const blt = hub.meshPlaced.find((world) => world.id === "__BLT__");
out.atHome = { shape: blt.shape, dx: blt.dx, dz: blt.dz, petals: worldMeshReplacedPetals() };
const territory = hub.meshTerritories.get("__BLT__")?.territory || null;
out.territory = territory && {
  id: territory.id, name: territory.name, style: territory.style, petal: territory.petal,
  slot: territory.slot, logo: territory.logo, objects: territory.objects.map((item) => item.label),
  bugs: territory.bugs && territory.bugs.title, visitBugs: territory.visitBugs,
};
out.territoryReads = territoryReads.map((item) => [
  item.url, item.options.credentials, item.options.mode, item.options.redirect]);
out.scenes = [...scenes];

const into = { x: blt.x - blt.dx * 0.4, z: blt.z - blt.dz * 0.4 };
out.land = {
  onLand: hub.neighbourHexLandAt(into.x, into.z)?.id || "",
  onRoad: hub.neighbourHexLandAt((home.x + blt.x) / 2, (home.z + blt.z) / 2)?.id || "",
  atHome: hub.neighbourHexLandAt(home.x, home.z)?.id || "",
};
walk(hub, (home.x + blt.x) / 2, (home.z + blt.z) / 2);
out.onRoad = { root: hub.meshRoot?.id || "", reloads: hub.reloads };
walk(hub, into.x, into.z);
await flush();
out.visited = {
  root: hub.meshRoot?.id || "",
  player: { ...hub.player },
  lastMovement: { x: hub.lastMovement.x, z: hub.lastMovement.z },
  petals: worldMeshReplacedPetals(),
  visit: hub.meshLinks.visitId,
  links: links(hub),
  socket: sockets.at(-1)?.url || "",
  office: hub.officeOwner,
  notices: [...hub.notices],
  reloads: hub.reloads,
  shift: hub.meshHomeShift(),
  standing: hub.world.visiting?.id || "",
  bugs: hub.world.visitBugs,
  homePeer: hub.homeFramePlayer({ id: "a", x: home.x, z: home.z }),
};
walk(hub, hub.lastMovement.x + 2, hub.lastMovement.z + 2);
const bltRoad = { x: home.x - blt.dx * (367 / 740), z: home.z - blt.dz * (367 / 740) };
walk(hub, bltRoad.x, bltRoad.z);
out.inside = { root: hub.meshRoot?.id || "", reloads: hub.reloads, standing: hub.world.visiting?.id || "" };
walk(hub, home.x - blt.dx, home.z - blt.dz);
await flush();
out.left = {
  root: hub.meshRoot?.id || "",
  player: { ...hub.player },
  visit: hub.meshLinks.visitId,
  links: links(hub),
  notices: [...hub.notices],
  standing: hub.world.visiting?.id || "",
  petals: worldMeshReplacedPetals(),
  office: hub.officeOwner,
  reloads: hub.reloads,
  shift: hub.meshHomeShift(),
};

const acme = hub.meshPlaced.find((world) => world.id === "__ACME__");
const startPlayer = { ...hub.player };
walk(hub, acme.x, acme.z);
await flush();
const hubInAcme = hub.meshPlaced.find((world) => world.id === "forkmesh");
out.entered = {
  acme: { dx: acme.dx, dz: acme.dz },
  start: startPlayer,
  root: hub.meshRoot?.id || "",
  player: { ...hub.player },
  lastMovement: { x: hub.lastMovement.x, z: hub.lastMovement.z },
  hub: { shape: hubInAcme.shape, dx: hubInAcme.dx, dz: hubInAcme.dz },
  visit: hub.meshLinks.visitId,
  links: links(hub),
  socket: sockets.at(-1)?.url || "",
  office: hub.officeOwner,
  notices: [...hub.notices],
  reloads: hub.reloads,
  shift: hub.meshHomeShift(),
  homePeer: hub.homeFramePlayer({ id: "a", x: home.x, z: home.z }),
};
walk(hub, hub.lastMovement.x + 1, hub.lastMovement.z + 1);
out.stays = { root: hub.meshRoot?.id || "", reloads: hub.reloads };

hub.meshLinks.links.get("__ACME__").peers.set("p1", {
  id: "p1", name: "resident", activity: "coding", category: "work", x: 3, z: 4,
  space: "town-square", home: "world.acme.dev",
});
out.resident = hub.meshPeerPlayers()[0];

out.reads = {};
out.reads.users = await hub.fetchMeshRootJSON(
  "/api/accounts/users?view=world", { auth: false, maxAge: 5 }, meshRootRoute("/api/accounts/users?view=world"));
out.reads.hidden = await hub.fetchMeshRootJSON(
  "/api/repository-imports", {}, meshRootRoute("/api/repository-imports"))
  .then(() => "loaded", (error) => error.message);
out.reads.write = await hub.fetchMeshRootJSON(
  "/api/world/inactive", { method: "DELETE" }, meshRootRoute("/api/world/inactive"));
const late = hub.fetchMeshRootJSON("/api/leaderboards", {}, "read");
hub.meshRootGeneration += 1;
out.reads.late = await late.then(() => "applied", (error) => error.message);
out.reads.at = await hub.fetchJSONAt(hubInAcme.x, hubInAcme.z, "/api/world/events");
out.federated = federated;
out.own = own;

walk(hub, hubInAcme.x, hubInAcme.z);
out.back = {
  root: hub.meshRoot?.id || "",
  player: { ...hub.player },
  visit: hub.meshLinks.visitId,
  links: links(hub),
  office: hub.officeOwner,
  notices: [...hub.notices],
  reloads: hub.reloads,
  shift: hub.meshHomeShift(),
};

const restored = view("forkmesh", into.x, into.z);
restored.restoredPosition = { x: into.x, z: into.z, space: "town-square" };
const loadedMesh = restored.worldMesh;
restored.worldMesh = { self: null, worlds: [] };
restored.placeWorldMesh();
restored.worldMesh = loadedMesh;
restored.placeWorldMesh();
out.restoredOnBlt = {
  player: { ...restored.player },
  restored: restored.restoredPosition,
  saved: restored.saved,
  root: restored.meshRoot?.id || "",
};
const kept = view("forkmesh", home.x, home.z);
kept.restoredPosition = { x: home.x, z: home.z, space: "town-square" };
kept.placeWorldMesh();
out.restoredAtHome = { player: { ...kept.player }, saved: kept.saved };

const own_page = view("__BLT__", home.x, home.z);
own_page.placeWorldMesh();
const seen = own_page.meshPlaced.find((world) => world.id === "forkmesh");
out.fromBlt = { shape: seen.shape, dx: seen.dx, dz: seen.dz, petals: worldMeshReplacedPetals() };

const ownShift = forkmeshShiftAround(territoryPetalTile(1));
const ownPage = view("__BLT__", home.x, home.z);
ownPage.territory = { petal: 1, slot: 0 };
ownPage.disabledWorldElements = ["forkbot"];
Object.assign(ownPage.world, {
  away: false,
  calls: [],
  territoryForkmeshShift: () => ({
    ...ownShift, townX: home.x + ownShift.x, townZ: home.z + ownShift.z,
  }),
  territoryOwnerAt(x, z) {
    if (this.away) return "";
    return forkmeshTilesAround(territoryPetalTile(1))
      .some((tile) => hexRoadContains(x, z, tile, 5)) ? "forkmesh" : "";
  },
  leaveOwnTerritory(saved) {
    this.calls.push(["leave", saved]);
    this.away = true;
    return true;
  },
  enterOwnTerritory() {
    this.calls.push(["enter", this.away]);
    this.away = false;
    return true;
  },
});
ownPage.placeWorldMesh();
const ownState = (v) => ({
  root: v.meshRoot?.id || "",
  at: { x: v.lastMovement.x, z: v.lastMovement.z },
  away: v.world.away,
  calls: [...v.world.calls],
  office: v.officeOwner,
  notices: [...v.notices],
  reloads: v.reloads,
  links: links(v),
  placed: v.meshPlaced.map((world) => [world.id, world.dx, world.dz]),
  shift: v.meshHomeShift(),
});
out.ownStart = ownState(ownPage);
walk(ownPage, home.x + ownShift.x * 0.47, home.z + ownShift.z * 0.47);
out.ownRoad = ownState(ownPage);
walk(ownPage, home.x + ownShift.x * 0.6, home.z + ownShift.z * 0.6);
await flush();
out.ownInForkmesh = ownState(ownPage);
walk(ownPage, home.x - ownShift.x * 0.47, home.z - ownShift.z * 0.47);
out.ownForkmeshRoad = ownState(ownPage);
walk(ownPage, home.x - ownShift.x * 0.7, home.z - ownShift.z * 0.7);
await flush();
out.ownBack = ownState(ownPage);

out.routes = [
  "/api/accounts/users?view=world", "/api/world/inactive", "/api/repositories",
  "/api/repo/acme/app/size-map", "/api/repository-imports", "/api/world/context",
  "/api/world/ticket", "/api/notifications?node=a",
].map((path) => meshRootRoute(path));
process.stdout.write(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def walk():
    if not NODE:
        pytest.skip("node is not installed")
    script = (SCRIPT.replace("__SCENE__", SCENE)
              .replace("__METHODS__", METHODS)
              .replace("__WORLDS__", json.dumps(WORLDS))
              .replace("__BLT__", BLT)
              .replace("__ACME__", ACME))
    result = subprocess.run([NODE, "--input-type=module", "-e", script],
                            cwd=WORLD, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


PITCH = 365 * 2 + 10
PETAL1 = (PITCH * 0.5, PITCH * 3 ** 0.5 / 2)


def close(a, b):
    return abs(a - b) < 1e-6


def test_the_hub_draws_blt_on_its_first_petal(walk):
    home = walk["atHome"]
    assert home["shape"] == "hex"
    assert close(home["dx"], PETAL1[0]) and close(home["dz"], PETAL1[1])
    assert home["petals"] == [1]


def test_blt_draws_the_hub_as_one_hexagon_on_its_opposite_petal(walk):
    seen = walk["fromBlt"]
    assert seen["shape"] == "hex"
    assert close(seen["dx"], -PETAL1[0]) and close(seen["dz"], -PETAL1[1])
    assert seen["petals"] == [4]


def test_the_shared_road_does_not_switch_worlds(walk):
    assert walk["onRoad"] == {"root": "", "reloads": 0}


def test_the_hub_loads_blts_own_territory_before_anyone_steps_in(walk):
    territory = walk["territory"]
    assert territory["id"] == BLT
    assert territory["name"] == "OWASP BLT"
    assert territory["style"] == "security"
    assert (territory["slot"], territory["petal"]) == (0, 1)
    assert territory["objects"] == ["Hacker House"]
    assert territory["bugs"] == "OWASP BLT bugs"
    assert territory["logo"] == (
        f"/api/world/neighbour-icon?world={BLT}&path=%2Finstance-logo.png")
    first, second = territory["visitBugs"]
    assert first["domain"] == "aaa.com"
    assert first["url"] == "https://legacy.owaspblt.org/domain/aaa.com/"
    assert first["favicon"].startswith(f"/api/world/neighbour-icon?world={BLT}&path=")
    assert second == {"domain": "aao.org", "name": "aao.org", "count": 0,
                      "url": "", "favicon": ""}
    assert walk["territoryReads"] == [
        ["https://git.owaspblt.org/api/instance", "omit", "cors", "error"]]
    assert walk["scenes"] == ["https://git.owaspblt.org/api/world/neighbour-scene"]


def test_stepping_onto_blt_puts_the_page_in_its_own_hexagon(walk):
    visited = walk["visited"]
    assert visited["root"] == BLT
    assert visited["standing"] == BLT
    assert close(visited["player"]["x"], -PETAL1[0])
    assert close(visited["player"]["z"], -45 - PETAL1[1])
    assert visited["petals"] == []
    assert visited["visit"] == BLT
    assert visited["links"] == [[BLT, "visit"]]
    assert visited["socket"] == (
        "wss://git.owaspblt.org/api/world/ws?visitor=fmwv1.token")
    assert visited["office"] is None
    assert visited["notices"] == ["Entering OWASP BLT · git.owaspblt.org"]
    assert visited["reloads"] == 1
    assert close(visited["shift"]["dx"], -PETAL1[0])
    assert close(visited["shift"]["dz"], -PETAL1[1])
    assert close(visited["homePeer"]["x"], -PETAL1[0])
    assert close(visited["homePeer"]["z"], -45 - PETAL1[1])
    assert visited["bugs"]["id"] == BLT and visited["bugs"]["count"] == 2


def test_only_blt_land_counts_as_its_hexagon(walk):
    land = walk["land"]
    assert land["onLand"] == BLT
    assert land["onRoad"] == ""
    assert land["atHome"] == ""


def test_walking_inside_blt_and_on_its_side_of_the_road_stays_there(walk):
    assert walk["inside"] == {"root": BLT, "reloads": 1, "standing": BLT}


def test_walking_off_blt_returns_home(walk):
    left = walk["left"]
    assert left["root"] == ""
    assert left["standing"] == ""
    assert close(left["player"]["x"], 0) and close(left["player"]["z"], -45)
    assert left["visit"] == ""
    assert left["links"] == [[BLT, "watch"]]
    assert left["petals"] == [1]
    assert left["office"] is None
    assert left["reloads"] == 2
    assert left["shift"] is None
    assert left["notices"][-1] == "Back in ForkMesh"
    assert left["notices"].count("Entering OWASP BLT · git.owaspblt.org") == 1


def test_a_saved_spot_on_blt_starts_at_the_campfire(walk):
    restored = walk["restoredOnBlt"]
    assert restored["root"] == ""
    assert restored["player"] == {"x": 0, "z": 114.8}
    assert restored["restored"] is None
    assert restored["saved"] == [{"x": 0, "z": 114.8}]


def test_a_saved_spot_in_forkmesh_is_kept(walk):
    assert walk["restoredAtHome"] == {"player": {"x": 0, "z": -45}, "saved": []}


def test_stepping_onto_a_flower_puts_the_page_in_that_world(walk):
    entered = walk["entered"]
    acme = entered["acme"]
    assert entered["root"] == ACME
    assert close(entered["player"]["x"], entered["start"]["x"] - acme["dx"])
    assert close(entered["player"]["z"], entered["start"]["z"] - acme["dz"])
    assert entered["hub"]["shape"] == "flower"
    assert close(entered["hub"]["dx"], -acme["dx"])
    assert close(entered["hub"]["dz"], -acme["dz"])
    assert entered["visit"] == ACME
    assert entered["links"] == [[ACME, "visit"]]
    assert entered["socket"] == (
        "wss://app.acme.dev/api/world/ws?visitor=fmwv1.token")
    assert entered["office"] == ACME
    assert entered["notices"][-1] == "Entering Acme · world.acme.dev"
    assert entered["reloads"] == 3
    assert close(entered["shift"]["dx"], -acme["dx"])
    assert close(entered["homePeer"]["x"], -acme["dx"])
    assert close(entered["homePeer"]["z"], -45 - acme["dz"])


def test_walking_inside_the_flower_keeps_the_page_there(walk):
    assert walk["stays"] == {"root": ACME, "reloads": 3}


def test_residents_keep_their_activity_and_stand_where_their_world_puts_them(walk):
    resident = walk["resident"]
    assert resident["id"] == f"mesh:{ACME}:p1"
    assert resident["name"] == "resident@world.acme.dev"
    assert resident["activity"] == "coding"
    assert resident["category"] == "work"
    assert (resident["x"], resident["z"]) == (3, 4)


def test_town_reads_follow_the_world_the_page_stands_in(walk):
    reads = walk["reads"]
    assert reads["users"] == {"from": ACME}
    assert reads["hidden"] == "not_in_this_world"
    assert reads["write"] == {"from": "home"}
    assert reads["late"] == "world_changed"
    assert reads["at"] == {"from": "home"}
    assert {"world": ACME, "path": "/api/accounts/users?view=world",
            "options": {"maxAge": 5, "auth": False}} in walk["federated"]
    assert all(item["world"] == ACME for item in walk["federated"])
    assert [item["path"] for item in walk["own"]] == [
        "/api/world/inactive", "/api/world/events"]
    assert all(item["options"]["meshHome"] is True for item in walk["own"])


def test_stepping_back_onto_the_hub_returns_home(walk):
    back = walk["back"]
    assert back["root"] == ""
    assert close(back["player"]["x"], 0) and close(back["player"]["z"], -45)
    assert back["visit"] == ""
    assert back["links"] == [[BLT, "watch"]]
    assert back["office"] is None
    assert back["notices"][-1] == "Back in ForkMesh"
    assert back["reloads"] == 4
    assert back["shift"] is None


def test_only_town_content_follows_the_world(walk):
    assert walk["routes"] == [
        "read", "read", "read", "hidden", "hidden", "", "", ""]


def test_neighbour_town_reads_are_federated_for_reading_only():
    for path in ("/api/accounts/users", "/api/world/inactive",
                 "/api/world/visitors", "/api/network/overview"):
        assert world_mesh.federated_path(path)
        assert world_mesh.federated_method_allowed(path, "GET")
        assert world_mesh.federated_method_allowed(path, "OPTIONS", "GET")
        assert not world_mesh.federated_method_allowed(path, "POST")
        assert not world_mesh.federated_method_allowed(path, "OPTIONS", "POST")
        assert not world_mesh.federated_method_allowed(path, "DELETE")
    assert world_mesh.federated_method_allowed(
        "/api/world/office/general/entry", "POST")
    preflight = world_mesh.federation_cors_headers(
        "https://git.owaspblt.org", preflight=True, read_only=True)
    assert preflight["access-control-allow-methods"] == "GET, OPTIONS"



def test_an_instance_page_starts_in_its_own_hexagon_with_forkmesh_next_door(walk):
    start = walk["ownStart"]
    assert start["root"] == ""
    assert start["calls"] == [["enter", False]]
    assert start["office"] == HUB
    assert start["placed"] == [[HUB, pytest.approx(-PETAL1[0]), pytest.approx(-PETAL1[1])]]
    assert start["shift"] is None
    assert walk["ownRoad"]["root"] == ""


def test_an_instance_page_steps_into_forkmesh_without_leaving_the_page(walk):
    inside = walk["ownInForkmesh"]
    assert inside["root"] == HUB
    assert inside["away"] is True
    assert inside["calls"][-1] == ["leave", ["forkbot"]]
    assert close(inside["at"]["x"], 0.4 * PETAL1[0])
    assert close(inside["at"]["z"], -45 + 0.4 * PETAL1[1])
    assert inside["notices"][-1] == "Entering ForkMesh · world.forkmesh.com"
    assert inside["office"] == HUB
    assert inside["reloads"] == 1
    assert [BLT, pytest.approx(PETAL1[0]), pytest.approx(PETAL1[1])] in inside["placed"]
    assert inside["shift"]["dx"] == pytest.approx(PETAL1[0])
    assert [HUB, "visit"] in inside["links"]
    assert walk["ownForkmeshRoad"]["root"] == HUB


def test_an_instance_page_walks_back_into_its_own_hexagon(walk):
    back = walk["ownBack"]
    assert back["root"] == ""
    assert back["away"] is False
    assert back["calls"][-1] == ["enter", True]
    assert close(back["at"]["x"], -0.3 * PETAL1[0])
    assert close(back["at"]["z"], -45 - 0.3 * PETAL1[1])
    assert back["notices"][-1] == "Back in OWASP BLT"
    assert back["office"] == HUB
    assert back["reloads"] == 2
    assert back["shift"] is None
    assert [HUB, "visit"] not in back["links"]

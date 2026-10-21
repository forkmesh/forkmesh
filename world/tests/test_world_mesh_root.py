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
WORLDS = [
    {"id": HUB, "name": "ForkMesh", "world": "https://world.forkmesh.com",
     "api": "https://app.forkmesh.com", "slot": -1, "shape": "flower"},
    {"id": BLT, "name": "OWASP BLT", "world": "https://git.owaspblt.org",
     "api": "https://git.owaspblt.org", "slot": 0, "shape": "hex",
     "scene": "https://git.owaspblt.org/api/world/neighbour-scene"},
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
    block(APP, "  placeWorldMesh() {", "\n  applyWorldMeshArrival() {"),
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

globalThis.window = globalThis;
globalThis.document = { hidden: false };
globalThis.location = { host: "world.forkmesh.com" };
const {
  CONTINUOUS_CITY_CENTER_Z, CONTINUOUS_CITY_CORNER_RADIUS, CONTINUOUS_CITY_RADIUS,
  WORLD_BIKE_LANE_CENTER_X, WORLD_MESH_HEX_SHAPE, worldMeshSlotOffset,
} = l;
const WORLD_MESH_SWITCH_MARGIN = 8;
const THREE = { Group: class { add() {} } };
const createInstanceHex = () => null;
const clearNeighbourFootprints = () => {};
const removeInteractiveObject = () => {};
const disposeObject3D = () => {};
const ctx = { world: { userData: {}, add() {}, remove() {} }, interactive: [] };
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
    player,
    lastMovement: { x, z, space: "town-square" },
    worldMesh: normalizeWorldMesh({ self, worlds: __WORLDS__ }),
    world: {
      updateInstanceHexes, worldMeshOwner,
      shiftPlayer(dx, dz) {
        player.x += dx;
        player.z += dz;
        return true;
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
const blt = hub.meshPlaced.find((world) => world.id === "__BLT__");
out.atHome = { shape: blt.shape, dx: blt.dx, dz: blt.dz, petals: worldMeshReplacedPetals() };

const into = { x: blt.x - blt.dx * 0.4, z: blt.z - blt.dz * 0.4 };
walk(hub, (home.x + blt.x) / 2, (home.z + blt.z) / 2);
out.onRoad = { root: hub.meshRoot?.id || "", reloads: hub.reloads };
walk(hub, into.x, into.z);
await flush();
const hubInBlt = hub.meshPlaced.find((world) => world.id === "forkmesh");
out.entered = {
  root: hub.meshRoot?.id || "",
  player: { ...hub.player },
  lastMovement: { x: hub.lastMovement.x, z: hub.lastMovement.z },
  hub: { shape: hubInBlt.shape, dx: hubInBlt.dx, dz: hubInBlt.dz },
  petals: worldMeshReplacedPetals(),
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

hub.meshLinks.links.get("__BLT__").peers.set("p1", {
  id: "p1", name: "resident", activity: "coding", category: "work", x: 3, z: 4,
  space: "town-square", home: "git.owaspblt.org",
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
out.reads.at = await hub.fetchJSONAt(hubInBlt.x, hubInBlt.z, "/api/world/events");
out.federated = federated;
out.own = own;

const back = { x: hubInBlt.x - hubInBlt.dx * 0.4, z: hubInBlt.z - hubInBlt.dz * 0.4 };
walk(hub, back.x, back.z);
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

const own_page = view("__BLT__", home.x, home.z);
own_page.placeWorldMesh();
const seen = own_page.meshPlaced.find((world) => world.id === "forkmesh");
out.fromBlt = { shape: seen.shape, dx: seen.dx, dz: seen.dz, petals: worldMeshReplacedPetals() };

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
              .replace("__BLT__", BLT))
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


def test_stepping_onto_blt_puts_the_page_in_blt(walk):
    entered = walk["entered"]
    assert entered["root"] == BLT
    assert close(entered["player"]["x"], -PETAL1[0])
    assert close(entered["player"]["z"], -45 - PETAL1[1])
    assert close(entered["lastMovement"]["x"], -PETAL1[0] * 0.4)
    assert close(entered["lastMovement"]["z"], -45 - PETAL1[1] * 0.4)
    assert entered["hub"]["shape"] == "hex"
    assert close(entered["hub"]["dx"], -PETAL1[0])
    assert entered["petals"] == [4]
    assert entered["visit"] == BLT
    assert entered["links"] == [[BLT, "visit"]]
    assert entered["socket"] == (
        "wss://git.owaspblt.org/api/world/ws?visitor=fmwv1.token")
    assert entered["office"] == BLT
    assert entered["notices"] == ["Entering OWASP BLT · git.owaspblt.org"]
    assert entered["reloads"] == 1
    assert close(entered["shift"]["dx"], -PETAL1[0])
    assert close(entered["homePeer"]["x"], -PETAL1[0])
    assert close(entered["homePeer"]["z"], -45 - PETAL1[1])


def test_walking_inside_blt_keeps_the_page_there(walk):
    assert walk["stays"] == {"root": BLT, "reloads": 1}


def test_blt_residents_keep_their_activity_and_stand_where_blt_puts_them(walk):
    resident = walk["resident"]
    assert resident["id"] == f"mesh:{BLT}:p1"
    assert resident["name"] == "resident@git.owaspblt.org"
    assert resident["activity"] == "coding"
    assert resident["category"] == "work"
    assert (resident["x"], resident["z"]) == (3, 4)


def test_town_reads_follow_the_world_the_page_stands_in(walk):
    reads = walk["reads"]
    assert reads["users"] == {"from": BLT}
    assert reads["hidden"] == "not_in_this_world"
    assert reads["write"] == {"from": "home"}
    assert reads["late"] == "world_changed"
    assert reads["at"] == {"from": "home"}
    assert {"world": BLT, "path": "/api/accounts/users?view=world",
            "options": {"maxAge": 5, "auth": False}} in walk["federated"]
    assert all(item["world"] == BLT for item in walk["federated"])
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
    assert back["reloads"] == 2
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


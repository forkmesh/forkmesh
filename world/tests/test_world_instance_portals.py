import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
HEXES = (WORLD / "scene" / "instance-hexes.js").read_text(encoding="utf-8")
POINTER = (WORLD / "scene" / "runtime" / "pointer.js").read_text(
    encoding="utf-8")
CORE = (WORLD / "scene" / "core.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

SCRIPT = r"""
import {
  homePortalHex, portalDestination, requestedPortalSlot, withHomePortal,
  worldPortalURL,
} from "./world-instance-portals.js";
import { instanceHexArrival, instanceHexSlot } from "./scene/instance-hexes.js";

const out = {};
out.urls = [
  worldPortalURL("https://code.example.org/world/", "code.example.org"),
  worldPortalURL("https://code.example.org/world/", "other.example.org"),
  worldPortalURL("http://code.example.org/world/"),
  worldPortalURL("https://code.example.org/"),
  worldPortalURL("https://code.example.org/world/?x=1"),
  worldPortalURL("javascript:alert(1)"),
  worldPortalURL("https://user:pw@code.example.org/world/"),
];
out.destinations = [
  portalDestination("https://code.example.org/world/", 3),
  portalDestination("https://code.example.org/world/", -1),
  portalDestination("https://code.example.org/world/", 12),
  portalDestination("https://evil.example.org/", 3),
];
out.requested = ["?portal=4", "?portal=11", "?portal=12", "?portal=-1",
  "?portal=x", "?view=world", ""].map(requestedPortalSlot);
const payload = { branded: true, directory: { portal: {
  home: "https://world.example.com/world/", slot: 5 } } };
out.home = homePortalHex(payload);
out.homeWithoutSlot = homePortalHex({ branded: true, directory: { portal: {
  home: "https://world.example.com/world/", slot: -1 } } });
out.none = [homePortalHex({ branded: false, directory: payload.directory }),
  homePortalHex({ branded: true, directory: null }),
  homePortalHex({ branded: true, directory: { portal: {
    home: "https://world.example.com/" } } }),
  homePortalHex(null)];
out.merged = withHomePortal([{ slot: 0 }], out.home).length;
out.taken = withHomePortal([{ slot: 5 }], out.home).length;
out.empty = withHomePortal([], null).length;
out.arrivals = Array.from({ length: 12 }, (_, slot) => {
  const tile = instanceHexSlot(slot);
  const spot = instanceHexArrival(slot);
  return { slot, tile, spot,
    fromTile: Math.hypot(spot.x - tile.x, spot.z - tile.z) };
});
out.badArrival = [instanceHexArrival(-1), instanceHexArrival(12)];
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    run = subprocess.run([NODE, "--input-type=module", "-e", SCRIPT],
                         cwd=WORLD, capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 0, run.stderr
    return json.loads(run.stdout)


def test_only_an_https_world_address_on_the_same_host_is_a_portal(result):
    assert result["urls"] == ["https://code.example.org/world/", "", "", "",
                              "", "", ""]


def test_a_portal_names_the_hexagon_to_arrive_at(result):
    assert result["destinations"] == [
        "https://code.example.org/world/?portal=3",
        "https://code.example.org/world/",
        "https://code.example.org/world/", ""]
    assert result["requested"] == [4, 11, -1, -1, -1, -1, -1]


def test_an_instance_world_gets_one_portal_home(result):
    assert result["home"] == {
        "id": "forkmesh-world-home", "name": "ForkMesh World",
        "url": "https://world.example.com/world/",
        "host": "world.example.com", "logo": "", "slot": 5,
        "worldUrl": "https://world.example.com/world/", "arrivalSlot": 5,
        "home": True}
    assert result["homeWithoutSlot"]["slot"] == 0
    assert result["homeWithoutSlot"]["arrivalSlot"] == -1
    assert result["none"] == [None, None, None, None]
    assert (result["merged"], result["taken"], result["empty"]) == (2, 1, 0)


def test_arrival_is_on_the_hexagon_in_front_of_its_landmark(result):
    for item in result["arrivals"]:
        assert 200 < item["fromTile"] < 260, item
        assert abs(item["spot"]["x"]) < 1900 and abs(item["spot"]["z"]) < 1900
        assert item["spot"]["space"] == "town-square"
    assert result["badArrival"] == [None, None]


def test_the_world_wires_portals_both_ways():
    assert 'from "./world-instance-portals.js"' in APP
    assert "worldUrl: worldPortalURL(item?.worldUrl, new URL(url).hostname)" \
        in APP
    assert "withHomePortal(\n        normalizeInstanceHexes(payload)," in APP
    assert 'this.fetchJSON("/api/instance", {' in APP
    select = APP.split("onInstanceHexagonSelect: (", 1)[1].split(
        "onFederatedWorldTravel", 1)[0]
    assert "portalDestination(worldUrl, arrivalSlot)" in select
    assert "window.location.assign(portal)" in select
    assert 'window.open(destination, "_blank", "noopener,noreferrer")' in select
    arrival = APP.split("  applyPortalArrival() {", 1)[1].split("\n  }\n", 1)[0]
    assert "this.world.instanceHexArrival?.(slot)" in arrival
    assert "this.world.setSpawn(spawn)" in arrival
    assert "url.searchParams.delete(PORTAL_PARAM)" in arrival
    assert "this.pendingPortalSlot = requestedPortalSlot(location.search);" in APP
    assert "object.userData.worldUrl = instance.worldUrl" in HEXES
    assert "worldUrl: String(hit.object.userData.worldUrl" in POINTER
    assert "arrivalSlot: Number(hit.object.userData.arrivalSlot)" in POINTER
    assert 'import { instanceHexArrival } from "./instance-hexes.js";' in CORE
    assert "    instanceHexArrival,\n" in CORE


PLUGIN_SCRIPT = r"""
import { instanceKeepsDirectory, instancePluginOff } from "./world-instance-portals.js";
const off = { branded: true, plugins: [{ id: "rewards", enabled: false },
  { id: "world", enabled: true }] };
console.log(JSON.stringify({
  off: [instancePluginOff(off, "rewards"), instancePluginOff(off, "world"),
    instancePluginOff(off, "ads"),
    instancePluginOff({ branded: false, plugins: off.plugins }, "rewards"),
    instancePluginOff(null, "rewards")],
  directory: [instanceKeepsDirectory(off), instanceKeepsDirectory({ branded: false }),
    instanceKeepsDirectory(null)],
}));
"""


def test_an_instance_world_skips_what_its_plugins_switch_off():
    if not NODE:
        pytest.skip("node is not installed")
    run = subprocess.run([NODE, "--input-type=module", "-e", PLUGIN_SCRIPT],
                         cwd=WORLD, capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 0, run.stderr
    result = json.loads(run.stdout)
    assert result["off"] == [True, False, False, False, False]
    assert result["directory"] == [False, True, True]


def test_rewards_ads_and_the_directory_are_not_requested_when_off():
    rewards = APP.split("  async refreshRewardState(", 1)[1].split(
        "\n  }\n", 1)[0]
    assert 'instancePluginOff(await this.instanceInfo(), "rewards")' in rewards
    assert rewards.index("instancePluginOff") < rewards.index(
        '"/api/accounts/central-fund"')
    for path in ('fetchJSON("/api/accounts/central-fund"',
                 'fetchJSON("/api/rewards/pending"',
                 '"/api/world/community-ads/placements?context=town-square"',
                 "fetchJSON(NETWORK_AD_SERVE_PATH, {"):
        for chunk in APP.split(path)[:-1]:
            tail = chunk[-700:]
            assert ("whenInstancePlugin(" in tail
                    or "instancePluginOff(" in tail
                    or "const NETWORK_AD_SERVE_PATH" in tail), (path, tail)
    directory = APP.split("  async refreshInstanceDirectory() {", 1)[1].split(
        "\n  }\n", 1)[0]
    assert "if (!instanceKeepsDirectory(instance)) {" in directory
    assert directory.index("instanceKeepsDirectory") < directory.index(
        "/api/start/instances")
    assert APP.count('this.fetchJSON("/api/instance", {') == 1

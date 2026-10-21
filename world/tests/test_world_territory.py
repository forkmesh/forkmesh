import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CORE = (WORLD / "scene" / "core.js").read_text(encoding="utf-8")
TERRITORY = (WORLD / "scene" / "instance-territory.js").read_text(
    encoding="utf-8")
WALK = (WORLD / "scene" / "runtime" / "walk-player.js").read_text(
    encoding="utf-8")
POINTER = (WORLD / "scene" / "runtime" / "pointer.js").read_text(
    encoding="utf-8")
NETWORK = (WORLD / "scene" / "runtime" / "network.js").read_text(
    encoding="utf-8")
OFFICE = (WORLD / "scene" / "runtime" / "office-navigation.js").read_text(
    encoding="utf-8")
NODE = shutil.which("node")

SCRIPT = r"""
import * as t from "./world-territory.js";
import * as g from "./scene/territory-geometry.js";
import {
  beachWalkSurfaceContains, cityWalkSurfaceContains, desertWalkSurfaceContains,
  setTerritoryTiles, territoryWalkActive,
} from "./scene/movement.js";
import { WORLD_HEX_TILES } from "./scene/layout.js";
import * as b from "./scene/territory-bugs.js";

const out = {};
out.slots = Array.from({ length: 12 }, (_, slot) => {
  const tiles = g.forkmeshTilesAround(slot);
  const home = g.TERRITORY_SLOTS[slot];
  return {
    slot,
    count: tiles.length,
    adjacent: tiles.filter((tile) => tile.adjacent).length,
    distances: tiles.filter((tile) => tile.adjacent).map((tile) =>
      Math.round(Math.hypot(tile.x - g.TERRITORY_CENTER.x, tile.z - g.TERRITORY_CENTER.z))),
    mapped: tiles.every((tile) =>
      Math.abs(tile.x - g.TERRITORY_CENTER.x - (WORLD_HEX_TILES[tile.index].x - home.x)) < 1e-6 &&
      Math.abs(tile.z - g.TERRITORY_CENTER.z - (WORLD_HEX_TILES[tile.index].z - home.z)) < 1e-6),
    replaces: WORLD_HEX_TILES.findIndex((tile) => Math.hypot(tile.x - home.x, tile.z - home.z) < 1),
    town: tiles.some((tile) => tile.center && tile.adjacent),
    border: g.portalBorderLocal(slot),
  };
});
out.badSlots = [g.forkmeshTilesAround(-1).length, g.forkmeshTilesAround(12).length,
  g.portalBorderLocal(99)];

const home = { x: 0, z: 0 };
out.crossings = [
  g.crossedInto({ x: 360, z: 0 }, { x: 368, z: 0 }, [{ x: 740, z: 0 }]),
  g.crossedInto({ x: 372, z: 0 }, { x: 376, z: 0 }, [{ x: 740, z: 0 }]),
  g.crossedInto({ x: 380, z: 0 }, { x: 382, z: 0 }, [{ x: 740, z: 0 }]),
  g.crossedInto(null, { x: 382, z: 0 }, [{ x: 740, z: 0 }]),
].map((tile) => (tile ? tile.x : null));
out.inside = g.edgeArrival({ x: 380, z: 12 }, "inside");
out.outside = g.edgeArrival({ x: 360, z: 12 }, "outside");
out.clamped = g.edgeArrival({ x: 380, z: 200 }, "inside");
out.badArrival = [g.edgeArrival({ x: "a", z: 1 }), g.forkmeshArrivalSpawn(12, { x: 1, z: 1 })];
out.instanceSpawn = g.territoryArrivalSpawn({ x: 380, z: 0 });
out.forkmeshSpawn = g.forkmeshArrivalSpawn(6, { x: -380, z: 0 });
out.slot6 = g.TERRITORY_SLOTS[6];

out.urls = [
  t.territoryCrossingURL("https://world.example.com/world/", {
    from: "code.example.org", slot: 3, local: { x: -193.16, z: -321.77 } }),
  t.territoryCrossingURL("https://code.example.org/world/", {
    from: "forkmesh", local: { x: 12, z: 900 } }),
  t.territoryCrossingURL("https://code.example.org/world/?token=x", {
    from: "forkmesh", local: { x: 1, z: 1 } }),
  t.territoryCrossingURL("http://code.example.org/world/", {
    from: "forkmesh", local: { x: 1, z: 1 } }),
  t.territoryCrossingURL("https://code.example.org/world/", {
    from: "javascript:alert(1)", local: { x: 1, z: 1 } }),
  t.territoryCrossingURL("https://code.example.org/world/", {
    from: "127.0.0.1", local: { x: 1, z: 1 } }),
  t.territoryCrossingURL("https://code.example.org/world/", {
    from: "forkmesh", local: { x: NaN, z: 1 } }),
];
out.arrivals = [
  "?from=code.example.org&slot=3&pos=-193.2,-321.8",
  "?from=forkmesh&pos=12,400",
  "?from=FORKMESH&pos=1.5,-2",
  "?from=forkmesh&pos=401,0",
  "?from=forkmesh&pos=1e3,0",
  "?from=<script>&pos=1,1",
  "?from=code.example.org&slot=12&pos=1,1",
  "?from=code.example.org",
  "?portal=3",
].map(t.territoryArrival);
out.cleaned = t.withoutTerritoryArrival(
  "https://code.example.org/world/?from=forkmesh&slot=1&pos=1,2&view=x#a");

const payload = {
  branded: true,
  name: "Acme Code",
  theme: { dark: { primary: "#ff0000" } },
  directory: { portal: { home: "https://world.example.com/world/", slot: 7 } },
  world: {
    style: "security",
    accent: "#E74C3C",
    logo: "/instance-logo.svg",
    objects: [
      { type: "house", label: "Hacker House", url: "https://house.example.org", position: [-60, -10] },
      { type: "house", label: "Bad", url: "http://house.example.org" },
      { type: "house", label: "", url: "https://house.example.org" },
      { type: "tower", label: "Tower", url: "https://house.example.org" },
      { type: "house", label: "Far", url: "https://far.example.org", position: [9999, -9999] },
    ],
    bugs: { repo: "acme/website", path: ".forkmesh/world/bugs.json", limit: 500 },
  },
};
out.territory = t.territoryFromInstance(payload);
out.unlisted = t.territoryFromInstance({ branded: true, name: "Acme", directory: null });
out.themeAccent = t.territoryFromInstance({ branded: true, name: "Acme",
  theme: { dark: { primary: "#123456" } }, world: { style: "meadow" } }).palette.accent;
out.notInstance = [t.territoryFromInstance({ branded: false }), t.territoryFromInstance(null)];
out.badLogo = t.territoryFromInstance({ branded: true, world: { logo: "https://evil.example/x.svg" } }).logo;
out.badBugs = [
  t.territoryFromInstance({ branded: true, world: { bugs: { repo: "acme", path: "x.json" } } }).bugs,
  t.territoryFromInstance({ branded: true, world: { bugs: { repo: "a/b", path: "../../x.json" } } }).bugs,
  t.territoryFromInstance({ branded: true, world: { bugs: { repo: "a/b", ref: "../x", path: "x.json" } } }).bugs,
];
out.disabled = t.territoryDisabledElements(["weather"], out.territory);
out.disabledOff = t.territoryDisabledElements(["weather"], null);
out.hidden = t.TERRITORY_HIDDEN_ELEMENTS;
out.neighbour = t.TERRITORY_NEIGHBOUR_ELEMENTS;
const mesh = (self) => ({ self: "a".repeat(32), worlds: [{ id: "a".repeat(32), ...self }] });
out.petals = [
  t.territoryPetal(0),
  t.territoryPetal(0, mesh({ shape: "hex" })),
  t.territoryPetal(0, mesh({ shape: "hex", petal: 3 })),
  t.territoryPetal(4, mesh({ shape: "flower", petal: 2 })),
  t.territoryPetal(5),
  t.territoryPetal(0, mesh({ shape: "hex", petal: 4 })),
  t.territoryPetal(-1, { self: "x", worlds: [] }),
];
out.kept = t.TERRITORY_KEPT_ELEMENTS;

const source = t.territoryBugSource({ repo: "acme/website", ref: "main", path: ".forkmesh/world/bugs.json" });
out.raw = t.territoryRawURL(source, source.path);
out.bugs = t.territoryBugs([
  { domain: "Shop.Example.COM", name: "Shop", url: "https://bugs.example.org/domain/shop.example.com", favicon: "favicons/shop.png", count: 4 },
  { domain: "shop.example.com" },
  "plain.example.net",
  { domain: "javascript:alert(1)" },
  { domain: "bad..example" },
  { domain: "x.example.org", favicon: "../../secret.png" },
  { domain: "y.example.org", favicon: "/etc/passwd.png" },
  { domain: "z.example.org", favicon: "https://tracker.example/z.png" },
  { domain: "w.example.org", favicon: "icon.svg", url: "javascript:alert(1)" },
], 60, source);
out.capped = t.territoryBugs(Array.from({ length: 100 }, (_, index) => `d${index}.example.com`), 500).length;
out.wrapped = t.territoryBugs({ bugs: ["a.example.com"] }, 5).length;

const center = g.TERRITORY_CENTER;
const tiles = g.forkmeshTilesAround(6).filter((tile) => tile.adjacent);
setTerritoryTiles([center, ...tiles]);
out.walk = {
  active: territoryWalkActive(),
  home: cityWalkSurfaceContains(center.x, center.z + 100) || desertWalkSurfaceContains(center.x, center.z + 100),
  road: desertWalkSurfaceContains(center.x - 370, center.z),
  forkmesh: desertWalkSurfaceContains(tiles[0].x, tiles[0].z),
  otherDesert: desertWalkSurfaceContains(center.x + 740, center.z),
  beach: beachWalkSurfaceContains(520, -80),
};
setTerritoryTiles(null);
out.walkOff = {
  active: territoryWalkActive(),
  otherDesert: desertWalkSurfaceContains(center.x + 740, center.z),
  beach: beachWalkSurfaceContains(520, -80),
};

const tank = g.territoryFootprint({ x: 60, z: 30, rotation: Math.atan2(-60, 10),
  width: 26, depth: 16, hollow: true, door: 4.4 });
const house = g.territoryFootprint({ x: -60, z: 30, rotation: 0.4, width: 14, depth: 12,
  door: 3.4, link: { url: "https://house.example.org/", label: "House" } });
const at = (footprint, x, z) => g.footprintWorld(footprint, x, z);
const local = (footprint, point) => {
  const spot = g.footprintLocal(footprint, point.x, point.z);
  return [Math.round(spot.x * 100) / 100, Math.round(spot.z * 100) / 100];
};
const move = (footprint, from, to) => {
  const result = g.constrainToFootprints([footprint], at(footprint, ...from), at(footprint, ...to), 0.46);
  return { at: local(footprint, result), blocked: result.blocked, door: result.door ? result.door.link?.label || "tank" : null };
};
out.collide = {
  roundTrip: local(tank, at(tank, 3.25, -4.5)),
  intoBackWall: move(tank, [0, -9], [0, -7]),
  outOfSideWall: move(tank, [12, 0], [14, 0]),
  throughDoorIn: move(tank, [0, 9.5], [0, 6]),
  throughDoorOut: move(tank, [0, 6], [0, 9.5]),
  beside: move(tank, [6, 9.5], [6, 7]),
  slide: move(tank, [6, 9.5], [6.4, 8.5]),
  tunnel: move(tank, [0, -20], [0, 20]),
  stuck: move(tank, [13, 0], [13.2, 0]),
  houseWall: move(house, [5, 7], [5, 5]),
  houseDoor: move(house, [0, 6.6], [0, 6.2]),
  houseDoorLeaving: move(house, [0, 6.6], [0, 7]),
  houseBack: move(house, [0, -7], [0, -5]),
  doorDistance: Math.round(g.footprintDoorDistance(house, at(house, 0, 9).x, at(house, 0, 9).z) * 100) / 100,
};
const domains = Array.from({ length: 200 }, (_, index) => `site${index}.example.com`);
const traits = domains.map((domain) => b.bugTraits(domain));
out.bugKinds = [...new Set(traits.map((item) => item.kind))].sort();
out.bugColors = new Set(traits.map((item) => item.color)).size;
out.bugStable = JSON.stringify(b.bugTraits("shop.example.com")) === JSON.stringify(b.bugTraits("shop.example.com"));
out.bugRanges = traits.every((item) => {
  const kind = b.BUG_KINDS[item.kind];
  return kind.colors.includes(item.color) && item.scale >= kind.size[0] && item.scale <= kind.size[1] &&
    item.speed >= kind.speed[0] && item.speed <= kind.speed[1] &&
    (item.motion === "fly") === (item.altitude > 0);
});
out.monograms = ["www.example.com", "91mobiles.com", "m.shop.example.org", ""].map(b.bugMonogram);
out.badgeSlots = b.BUG_BADGE_SLOTS;
const visa = "eyJ2IjoxfQ." + "a".repeat(64);
out.visa = [
  t.withTerritoryVisa("https://world.example.com/world/?from=code.example.org&pos=1,2", visa),
  t.withTerritoryVisa("https://world.example.com/world/?from=code.example.org&pos=1,2", "not a visa"),
  t.withTerritoryVisa("https://world.example.com/world/?from=code.example.org&pos=1,2", ""),
  t.territoryArrivalVisa("#visa=" + visa),
  t.territoryArrivalVisa("#visa=<script>"),
  t.withoutTerritoryArrival("https://code.example.org/world/?from=forkmesh&pos=1,2#visa=" + visa),
];
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


def test_the_forkmesh_world_sits_next_door_at_the_directory_slot(result):
    for item in result["slots"]:
        petal = item["slot"] < 5
        # A petal slot is one of the town's petals: the desert petal it
        # replaces is not drawn, and it borders the town and two petals.
        assert item["count"] == (6 if petal else 7), item
        assert item["replaces"] == (item["slot"] + 2 if petal else -1), item
        assert item["town"] is petal, item
        assert item["mapped"] is True, item
        assert item["adjacent"] == (3 if petal else 2 if item["slot"] < 6
                                    else 1), item
        assert all(distance == 740 for distance in item["distances"]), item
        border = item["border"]
        assert abs((border["x"] ** 2 + border["z"] ** 2) ** 0.5 - 370) < 1e-6
    assert result["badSlots"] == [0, 0, None]


def test_crossing_the_road_is_entering_the_neighbouring_land(result):
    assert result["crossings"] == [None, 740, None, None]


def test_arrival_is_on_the_matching_road_edge_facing_in(result):
    inside = result["inside"]
    assert (round(inside["x"]), round(inside["z"])) == (351, 12)
    assert (inside["normalX"], inside["normalZ"]) == (1, 0)
    assert abs(inside["heading"] - 1.5707963267948966) < 1e-9
    outside = result["outside"]
    assert (round(outside["x"]), round(outside["z"])) == (389, 12)
    assert abs(outside["heading"] + 1.5707963267948966) < 1e-9
    assert abs(result["clamped"]["z"] - 150) < 1e-9
    assert result["badArrival"] == [None, None]
    assert result["instanceSpawn"]["space"] == "town-square"
    # Arrivals turn the camera to face into the World entered.
    assert result["instanceSpawn"]["faceCamera"] is True
    assert result["forkmeshSpawn"]["faceCamera"] is True
    assert round(result["instanceSpawn"]["x"]) == 351
    spawn = result["forkmeshSpawn"]
    slot6 = result["slot6"]
    assert round(spawn["x"] - slot6["x"]) == -389
    assert round(spawn["z"] - slot6["z"]) == 0


def test_crossing_urls_carry_only_origin_slot_and_position(result):
    urls = result["urls"]
    assert urls[0] == ("https://world.example.com/world/?from=code.example.org"
                       "&slot=3&pos=-193.2%2C-321.8")
    assert urls[1] == ("https://code.example.org/world/?from=forkmesh"
                       "&pos=12%2C400")
    assert urls[2:] == ["", "", "", "", ""]


def test_arrival_parameters_are_sanitized(result):
    arrivals = result["arrivals"]
    assert arrivals[0] == {"from": "code.example.org", "slot": 3,
                           "pos": {"x": -193.2, "z": -321.8}}
    assert arrivals[1] == {"from": "forkmesh", "slot": -1,
                           "pos": {"x": 12, "z": 400}}
    assert arrivals[2] == {"from": "forkmesh", "slot": -1,
                           "pos": {"x": 1.5, "z": -2}}
    assert arrivals[3:6] == [None, None, None]
    assert arrivals[6]["slot"] == -1
    assert arrivals[7:] == [None, None]
    assert result["cleaned"] == "https://code.example.org/world/?view=x#a"


def test_an_instance_world_is_its_own_branded_territory(result):
    territory = result["territory"]
    assert territory["name"] == "Acme Code"
    assert territory["style"] == "security"
    assert territory["palette"]["accent"] == "#e74c3c"
    assert territory["logo"] == "/instance-logo.svg"
    assert territory["home"] == "https://world.example.com/world/"
    assert (territory["slot"], territory["listed"]) == (7, True)
    assert territory["objects"] == [
        {"type": "house", "label": "Hacker House",
         "url": "https://house.example.org/", "host": "house.example.org",
         "x": -60, "z": -10, "color": ""},
        {"type": "house", "label": "Far", "url": "https://far.example.org/",
         "host": "far.example.org", "x": 300, "z": -300, "color": ""}]
    assert territory["bugs"]["limit"] == 60
    assert territory["bugs"]["source"] == {
        "repo": "acme/website", "ref": "main",
        "path": ".forkmesh/world/bugs.json"}
    unlisted = result["unlisted"]
    assert (unlisted["slot"], unlisted["listed"]) == (0, False)
    assert unlisted["home"] == "https://world.forkmesh.com/world/"
    assert unlisted["style"] == "meadow" and unlisted["bugs"] is None
    assert result["themeAccent"] == "#123456"
    assert result["notInstance"] == [None, None]
    assert result["badLogo"] == "/assets/logo.svg"
    assert result["badBugs"] == [None, None, None]


def test_forkmesh_world_is_shown_beside_and_its_live_pieces_are_hidden(result):
    hidden = set(result["hidden"])
    shown = set(result["neighbour"])
    # ForkMesh's own town, petals and buildings are drawn beside the
    # instance; boards, bots and districts that would show the instance's
    # own data inside ForkMesh's town stay off.
    for element in ("landmark-office", "city-terrain", "desert-plots",
                    "coaster", "beach", "town-landscape", "bike-lane"):
        assert element in shown
    for element in ("leaderboard-district", "forkbot", "node-district",
                    "start-here-board", "social-banners", "mastodon-kiosk",
                    "discord-board", "world-bulletin"):
        assert element in hidden
    assert not hidden & set(result["kept"])
    assert not hidden & shown and not shown & set(result["kept"])
    assert set(result["disabled"]) == hidden | {"weather"}
    assert result["disabledOff"] == ["weather"]
    assert result["petals"] == [1, 1, 3, 5, -1, 4, -1]


def test_the_bug_list_is_capped_and_sanitized(result):
    assert result["raw"] == ("/api/repo/acme/website/raw?path=.forkmesh%2Fworld"
                             "%2Fbugs.json&ref=main")
    bugs = result["bugs"]
    assert [bug["domain"] for bug in bugs] == [
        "shop.example.com", "plain.example.net", "x.example.org",
        "y.example.org", "z.example.org", "w.example.org"]
    assert bugs[0] == {
        "domain": "shop.example.com", "name": "Shop", "count": 4,
        "url": "https://bugs.example.org/domain/shop.example.com",
        "favicon": "/api/repo/acme/website/raw?path=.forkmesh%2Fworld%2F"
                   "favicons%2Fshop.png&ref=main"}
    assert bugs[2]["favicon"] == ("/api/repo/acme/website/raw?path=secret.png"
                                  "&ref=main")
    assert [bug["favicon"] for bug in bugs[3:]] == ["", "", ""]
    assert bugs[5]["url"] == ""
    assert result["capped"] == 60
    assert result["wrapped"] == 1


def test_the_territory_walks_only_its_hex_and_the_forkmesh_side(result):
    assert result["walk"] == {"active": True, "home": True, "road": True,
                              "forkmesh": True, "otherDesert": False,
                              "beach": False}
    assert result["walkOff"] == {"active": False, "otherDesert": True,
                                 "beach": True}


def test_the_world_wires_the_territory():
    assert 'from "./world-territory.js"' in APP
    assert ("this.territory = territoryFromInstance(\n"
            "        await this.instanceInfo(),\n"
            "        await this.worldMeshDirectory(),\n      );") in APP
    assert "await this.placeTerritoryMesh();" in APP
    assert "{ ...hub, dx: shift.x, dz: shift.z, x: shift.townX, z: shift.townZ }," \
        in APP
    assert "territoryForkmeshShift: fns.territoryForkmeshShift," in CORE
    assert "TERRITORY_NEIGHBOUR_ELEMENTS.forEach((id) => {" in TERRITORY
    assert "ctx.desertPlots?.userData?.replacePetals?.(" in TERRITORY
    assert "territory: this.territory," in APP
    assert "territoryDisabledElements(\n          this.disabledWorldElements," \
        in APP
    assert "window.location.assign(destination);" in APP
    assert "fns.constrainTerritoryObstacles?.(previousHorizontalPosition);" in WALK
    assert "fns.enterNearbyTerritoryDoor?.() ||" in POINTER
    assert "onTerritoryLinkSelect: (link) => this.openTerritoryLink(link)," in APP
    assert "this.leaveTerritory(destination, `Entering ${name}…`);" in APP
    assert "withTerritoryVisa(destination, visa)" in APP
    assert '"/api/world/visa",' in APP
    assert "territoryRawURL(source, source.path)" in APP
    assert "fns.installInstanceTerritory();" in CORE
    assert "fns.checkTerritoryCrossing?.(previousHorizontalPosition);" in WALK
    assert '"territory-link"' in POINTER and '"territory-bug"' in POINTER
    assert "fns.setForkmeshCrossings?.(" not in NETWORK
    assert "if (!this.world || this.destroyed || this.territory) return;" in APP
    assert "territoryWalkActive()" not in OFFICE
    # The portal obelisk no longer reloads the page.
    assert 'part.userData.interactive = "instance-hexagon";' not in TERRITORY


def test_the_terrarium_and_houses_are_walled_with_one_way_in(result):
    collide = result["collide"]
    assert collide["roundTrip"] == [3.25, -4.5]
    assert collide["intoBackWall"]["blocked"] is True
    assert collide["intoBackWall"]["at"] == [0, -9]
    assert collide["outOfSideWall"]["blocked"] is True
    assert collide["outOfSideWall"]["at"][0] < 13
    assert collide["throughDoorIn"] == {"at": [0, 6], "blocked": False,
                                        "door": None}
    assert collide["throughDoorOut"] == {"at": [0, 9.5], "blocked": False,
                                         "door": None}
    assert collide["beside"]["blocked"] is True
    assert collide["beside"]["at"][1] > 8
    # A diagonal step into the glass slides along it instead of stopping.
    assert collide["slide"]["at"] == [6.4, 9.5]
    assert collide["tunnel"]["blocked"] is True
    assert collide["tunnel"]["at"] == [0, -20]
    # A walker already inside a wall is let out, never pinned.
    assert collide["stuck"]["blocked"] is False
    assert collide["houseWall"]["blocked"] is True
    assert collide["houseWall"]["door"] is None
    assert collide["houseDoor"] == {"at": [0, 6.6], "blocked": True,
                                    "door": "House"}
    assert collide["houseDoorLeaving"]["blocked"] is False
    assert collide["houseBack"]["door"] is None
    assert collide["doorDistance"] == 3


def test_each_site_gets_its_own_steady_bug(result):
    assert result["bugKinds"] == ["ant", "beetle", "ladybug", "moth",
                                  "spider", "stag"]
    assert result["bugColors"] >= 15
    assert result["bugStable"] is True
    assert result["bugRanges"] is True
    assert result["monograms"] == ["E", "9", "S", "?"]
    assert result["badgeSlots"] >= 60


def test_the_visa_rides_in_the_fragment_and_is_removed_on_arrival(result):
    visa = result["visa"]
    token = "eyJ2IjoxfQ." + "a" * 64
    assert visa[0] == ("https://world.example.com/world/?from=code.example.org"
                       "&pos=1,2#visa=" + token)
    assert visa[1] == visa[2] == ("https://world.example.com/world/"
                                  "?from=code.example.org&pos=1,2")
    assert visa[3] == token
    assert visa[4] == ""
    assert visa[5] == "https://code.example.org/world/"


def test_the_bug_badges_fall_back_and_stay_sharp():
    bugs = (WORLD / "scene" / "territory-bugs.js").read_text(encoding="utf-8")
    assert "image.onload" in bugs and "faviconUsable(image)" in bugs
    assert "bugMonogram(domain)" in bugs
    assert "context.imageSmoothingEnabled = !small;" in bugs
    assert "camera.quaternion" in bugs


def test_forkmesh_beside_us_is_live_and_walkable_without_a_reload():
    assert "walkTiles: [TERRITORY_CENTER, ...neighbours.tiles]," in TERRITORY
    assert "crossings: []," in TERRITORY
    assert 'if (hexRoadContains(x, z, tile, WORLD_ROAD_WIDTH / 2)) return "forkmesh";' \
        in TERRITORY
    assert "this.meshLinks.update(x, z, this.world?.territoryOwnerAt?.(x, z) || \"\");" \
        in APP
    assert "this.setOfficeOwner(hub);" in APP
    assert "forkmeshViewURL" not in APP
    assert '"territory-forkmesh"' not in POINTER
    assert 'if (ctx.officeSceneMode !== "town") return false;' in OFFICE


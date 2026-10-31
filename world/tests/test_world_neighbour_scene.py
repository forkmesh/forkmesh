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
out.icons = [
  s.neighbourIconURL("a".repeat(32), "https://code.example.org/api/repo/o/r/raw?path=a%2Fb.png&ref=main"),
  s.neighbourIconURL("forkmesh", "https://code.example.org/i.png"),
  s.neighbourIconURL("a".repeat(32), "http://code.example.org/i.png"),
  s.neighbourIconURL("a".repeat(32), ""),
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
    assert "group.add(buildNeighbourScene(THREE, scene, {" in HEXES
    assert "icon: (url) => neighbourIconURL(instance.id, url)," in HEXES
    assert 'image.crossOrigin = "anonymous";' in HEXES
    assert "innerHTML" not in HEXES


def test_bugs_show_through_the_glass_and_links_are_clickable():
    tank = HEXES[HEXES.index("function sceneTerrarium("):
                 HEXES.index("export function buildNeighbourScene(")]
    assert "depthWrite: false,\n  });" in tank
    assert "const url = entry.icon ? icon(entry.icon) : \"\";" in HEXES
    # A bug opens its card (neighbour-bug); its link is on the card.
    assert "bug(sprite, entry);" in tank
    assert "sprite.visible = false;" in tank
    assert "sprite.userData.raycastProxy = true;" in tank
    assert "tank.add(bugBoard(THREE, centers, bugIconAtlas(THREE, item.bugs, icon, accent)));" in tank
    assert "pick(object, item.url, item.label)" in HEXES
    assert 'object.userData.interactive = "instance-hexagon";' in HEXES
    assert 'if (ground.pattern !== "circuit") {' in HEXES


def test_bug_icons_go_through_the_home_worlds_icon_route(result):
    assert result["icons"] == [
        "/api/world/neighbour-icon?world=" + "a" * 32
        + "&path=%2Fapi%2Frepo%2Fo%2Fr%2Fraw%3Fpath%3Da%252Fb.png%26ref%3Dmain",
        "", "", ""]


MESH_WORLDS = [
    {"id": "forkmesh", "world": "https://world.forkmesh.com",
     "host": "world.forkmesh.com", "api": "https://app.forkmesh.com"},
    {"id": "a" * 32, "world": "https://code.example.org",
     "host": "code.example.org", "api": "https://code.example.org",
     "scene": "https://code.example.org/api/world/neighbour-scene"},
    {"id": "b" * 32, "world": "https://two.example.org",
     "host": "two.example.org", "api": "https://two.example.org"},
]


def test_an_icon_source_is_a_path_on_a_listed_hexagon_neighbour():
    hub = SimpleNamespace(WORLD_ORIGIN="https://world.forkmesh.com",
                          APP_ORIGIN="https://app.forkmesh.com")
    source = world_mesh.neighbour_icon_source
    assert source(hub, MESH_WORLDS, "a" * 32, "/i.png?v=1") == \
        "https://code.example.org/i.png?v=1"
    for world_id, path in (
            ("b" * 32, "/i.png"), ("forkmesh", "/i.png"), ("c" * 32, "/i.png"),
            ("a" * 32, "//evil.example.net/i.png"), ("a" * 32, "i.png"),
            ("a" * 32, "/i.png#x"), ("a" * 32, "/a\\b"), ("a" * 32, "/a b"),
            ("a" * 32, "/" + "x" * 300), ("a" * 32, "")):
        assert source(hub, MESH_WORLDS, world_id, path) == "", path
    own = SimpleNamespace(WORLD_ORIGIN="https://code.example.org",
                          APP_ORIGIN="https://code.example.org",
                          INSTANCE_NAME="Acme")
    assert source(own, MESH_WORLDS, "a" * 32, "/i.png") == ""


def test_only_raster_images_are_relayed():
    kind = world_mesh.icon_type
    assert kind(b"\x89PNG\r\n\x1a\n....") == "image/png"
    assert kind(b"\xff\xd8\xff\xe0") == "image/jpeg"
    assert kind(b"GIF89a..") == "image/gif"
    assert kind(b"RIFF\0\0\0\0WEBPVP8 ") == "image/webp"
    assert kind(b"\0\0\1\0\1\0\x10\x10") == "image/x-icon"
    for raw in (b"", b"<svg xmlns='http://www.w3.org/2000/svg'/>",
                b"<html>", b"\0\0\1\0"):
        assert kind(raw) == ""


INSTANCE_ENV = SimpleNamespace(INSTANCE_NAME="Acme <Code>",
                               WORLD_ORIGIN="https://code.example.org",
                               APP_ORIGIN="https://code.example.org")
HUB_ENV = SimpleNamespace(WORLD_ORIGIN="https://world.forkmesh.com",
                          APP_ORIGIN="https://app.forkmesh.com")
CHECK = r"""
import * as s from "./world-neighbour-scene.js";
const scene = JSON.parse(process.argv[1]);
console.log(JSON.stringify(s.normalizeNeighbourScene(scene, "https://code.example.org")));
"""


def checked_by_the_hub(scene):
    if not NODE:
        pytest.skip("node is not installed")
    run = subprocess.run([NODE, "--input-type=module", "-e", CHECK,
                          json.dumps(scene)],
                         cwd=WORLD, capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    return json.loads(run.stdout)


def test_an_instance_publishes_a_scene_the_hub_accepts_whole():
    scene = world_mesh.neighbour_scene(INSTANCE_ENV)
    assert scene["v"] == 1
    sign = next(item for item in scene["objects"] if item["type"] == "sign")
    assert sign["title"] == "Acme Code"
    assert sign["text"] == "code.example.org"
    assert len(json.dumps(scene)) < world_mesh.SCENE_MAX_BYTES
    checked = checked_by_the_hub(scene)
    assert len(checked["objects"]) == len(scene["objects"])
    assert [item["type"] for item in checked["objects"]] == [
        item["type"] for item in scene["objects"]]
    assert world_mesh.neighbour_scene(HUB_ENV) is None
    assert world_mesh.neighbour_scene(
        SimpleNamespace(INSTANCE_NAME="Acme")) is None


def test_an_operator_scene_replaces_the_default_only_when_it_fits():
    own = {"v": 1, "objects": [{"type": "lamp", "x": 1, "z": 2}]}
    env = SimpleNamespace(**vars(INSTANCE_ENV),
                          WORLD_NEIGHBOUR_SCENE=json.dumps(own))
    assert world_mesh.neighbour_scene(env) == own
    default = world_mesh.neighbour_scene(INSTANCE_ENV)
    for raw in ("not json", json.dumps({"v": 2, "objects": own["objects"]}),
                json.dumps({"v": 1, "objects": []}),
                json.dumps({"v": 1, "objects": ["x"]}),
                json.dumps({"v": 1, "objects": own["objects"] * 49}),
                json.dumps({"v": 1, "objects": own["objects"],
                            "pad": "x" * world_mesh.SCENE_MAX_BYTES})):
        env = SimpleNamespace(**vars(INSTANCE_ENV), WORLD_NEIGHBOUR_SCENE=raw)
        assert world_mesh.neighbour_scene(env) == default, raw[:40]


def test_the_scene_route_is_public_cacheable_and_get_only(monkeypatch):
    import world_routes

    def json_response(data, status=200, cache_seconds=None,
                      extra_headers=None, **_):
        return SimpleNamespace(data=data, status=status,
                               cache_seconds=cache_seconds,
                               headers=extra_headers or {})

    monkeypatch.setattr(world_routes, "world_mesh", world_mesh, raising=False)
    monkeypatch.setattr(world_routes, "json_response", json_response,
                        raising=False)
    monkeypatch.setattr(world_routes, "method_name",
                        lambda request: request.method, raising=False)
    handler = world_routes.world_neighbour_scene_handler
    reply = handler(INSTANCE_ENV, SimpleNamespace(method="GET"))
    assert reply.status == 200
    assert reply.data == world_mesh.neighbour_scene(INSTANCE_ENV)
    assert reply.cache_seconds == world_mesh.SCENE_MAX_AGE
    assert reply.headers == {"access-control-allow-origin": "*"}
    assert handler(HUB_ENV, SimpleNamespace(method="GET")).status == 404
    assert handler(INSTANCE_ENV, SimpleNamespace(method="POST")).status == 405
    runtime = (ROOT / "app" / "src" / "app_runtime.py").read_text(
        encoding="utf-8")
    assert '"/api/world/neighbour-scene", "/api/world/neighbour-scene/"' \
        in runtime
    assert "world_neighbour_scene_handler(self.env, request)" in runtime


class _Buffer:
    def __init__(self, raw):
        self.raw = raw

    def to_py(self):
        return self.raw


def test_the_icon_route_fetches_once_and_answers_cors_open(monkeypatch):
    import asyncio
    from urllib.parse import parse_qs, urlparse

    import world_routes

    png = b"\x89PNG\r\n\x1a\n" + b"x" * 40
    calls = []

    async def fetch(url, init, timeout):
        calls.append((url, init))
        body = {"https://code.example.org/i.png": png,
                "https://code.example.org/page.html": b"<html>"}.get(url)

        async def array_buffer():
            return body
        return SimpleNamespace(
            status=200 if body else 404,
            headers={"content-length": str(len(body or b""))},
            arrayBuffer=array_buffer)

    async def directory(env):
        return MESH_WORLDS

    def json_response(data, status=200, extra_headers=None, **_):
        return SimpleNamespace(data=data, status=status,
                               headers=extra_headers or {})

    def response(body, init):
        return SimpleNamespace(status=init["status"], body=body,
                               headers=init["headers"])

    values = {
        "world_mesh": world_mesh, "world_mesh_directory": directory,
        "js_fetch_with_timeout": fetch, "json_response": json_response,
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs, "urlparse": urlparse,
        "Date": SimpleNamespace(now=lambda: 1000),
        "Uint8Array": SimpleNamespace(new=lambda value: _Buffer(bytes(value))
                                      if not isinstance(value, _Buffer)
                                      else value),
        "JsResponse": SimpleNamespace(new=response),
        "_to_js": lambda value: value, "to_js": lambda value: value,
    }
    for name, value in values.items():
        monkeypatch.setattr(world_routes, name, value, raising=False)
    monkeypatch.setattr(world_routes, "_WORLD_ICON_CACHE", {})
    hub = SimpleNamespace(WORLD_ORIGIN="https://world.forkmesh.com",
                          APP_ORIGIN="https://app.forkmesh.com")

    def get(query, method="GET"):
        request = SimpleNamespace(
            method=method,
            url="https://app.forkmesh.com/api/world/neighbour-icon?" + query)
        return asyncio.run(
            world_routes.world_neighbour_icon_handler(hub, request))

    ok = "world=%s&path=%%2Fi.png" % ("a" * 32)
    first = get(ok)
    assert first.status == 200
    assert first.body.raw == png
    assert first.headers["content-type"] == "image/png"
    assert first.headers["access-control-allow-origin"] == "*"
    assert first.headers["x-content-type-options"] == "nosniff"
    assert get(ok).status == 200
    assert [url for url, _ in calls] == ["https://code.example.org/i.png"]
    assert calls[0][1]["redirect"] == "error"
    assert get("world=%s&path=%%2Fpage.html" % ("a" * 32)).status == 404
    assert get("world=%s&path=%%2Fi.png" % ("b" * 32)).status == 404
    assert get("world=%s&path=%%2F%%2Fevil.example.net" % ("a" * 32)).status \
        == 404
    assert get(ok, method="POST").status == 405
    assert len(calls) == 2
    runtime = (ROOT / "app" / "src" / "app_runtime.py").read_text(
        encoding="utf-8")
    assert '"/api/world/neighbour-icon", "/api/world/neighbour-icon/"' \
        in runtime
    assert "await world_neighbour_icon_handler(self.env, request)" in runtime


def test_a_terrarium_draws_its_bugs_in_one_call_and_merges_its_decor():
    board = HEXES[HEXES.index("function bugBoardMaterial("):HEXES.index("function sceneHouse(")]
    assert '"MeshBasicMaterial"' in board and "alphaTest: 0.5" in board
    assert "mvPosition.xy += bugCorner;" in board
    assert 'material.customProgramCacheKey = () => "forkmesh-neighbour-bug-board";' in board
    assert "new THREE.Sprite(" not in HEXES[HEXES.index("function bugIconAtlas("):HEXES.index("function sceneHouse(")]
    built = HEXES[HEXES.index("export function buildNeighbourScene("):]
    assert "if (child.userData.neighbourBugBoard) child.castShadow = false;" in built
    assert "mergeMeshesByMaterial(THREE, group, { skip: (object) => Boolean(object.userData.interactive) });" in built

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import instance_world_scene as scene_module  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
WORLD = {
    "style": "security",
    "accent": "#e74c3c",
    "objects": [{"type": "house", "label": "Club House",
                 "url": "https://club.example.org/", "position": [-60, -10],
                 "color": ""}],
    "bugs": {"repo": "acme/website", "ref": "main",
             "path": ".forkmesh/world/bugs.json", "title": "Acme bugs",
             "limit": 60, "position": [60, -10]},
}
LISTING = [
    {"domain": "shop.example.com", "favicon": "favicons/shop.png",
     "url": "https://bugs.example.org/domain/shop.example.com/"},
    {"domain": "plain.example.net", "url": "http://plain.example.net/"},
    {"domain": "x.example.org", "favicon": "../../secret.png"},
    {"domain": ""},
    "not a bug",
] + [{"domain": "d%d.example.com" % i} for i in range(50)]


def test_the_scene_places_our_hexagon_as_our_world_draws_it():
    scene = scene_module.build("Acme Code", WORLD, "https://code.example.org",
                               "#123456", LISTING)
    assert scene["v"] == 1
    assert scene["ground"] == {"color": "#3a4550", "accent": "#e74c3c",
                               "pattern": "circuit"}
    kinds = [item["type"] for item in scene["objects"]]
    assert kinds == ["plaza", "sign", "lamp", "lamp", "lamp", "lamp",
                     "house", "terrarium"]
    plaza, sign = scene["objects"][:2]
    assert (plaza["x"], plaza["z"], plaza["radius"]) == (0.0, 85.0, 26)
    assert (sign["x"], sign["z"], sign["title"]) == (0.0, 63.0, "Acme Code")
    house = scene["objects"][6]
    assert (house["x"], house["z"]) == (-60.0, 75.0)
    assert house["url"] == "https://club.example.org/"
    assert house["color"] == "#e74c3c"
    assert abs(house["facing"] - 1.4056) < 1e-4
    tank = scene["objects"][7]
    assert (tank["x"], tank["z"], tank["label"]) == (60.0, 75.0, "Acme bugs")
    bugs = tank["bugs"]
    assert len(bugs) == scene_module.MAX_BUGS
    assert bugs[0] == {
        "label": "shop.example.com",
        "icon": "https://code.example.org/api/repo/acme/website/raw?path="
                ".forkmesh%2Fworld%2Ffavicons%2Fshop.png&ref=main",
        "url": "https://bugs.example.org/domain/shop.example.com/"}
    assert bugs[1] == {"label": "plain.example.net", "icon": "", "url": ""}
    assert bugs[2]["icon"] == ""
    assert len(scene_module.dumps(scene)) < 64 * 1024


def test_a_plain_world_without_bugs_still_has_a_scene():
    scene = scene_module.build("Acme", {}, "https://code.example.org")
    assert scene["ground"]["pattern"] == "plain"
    assert [item["type"] for item in scene["objects"]][:2] == ["plaza", "sign"]
    assert not any(item["type"] == "terrarium" for item in scene["objects"])
    json.dumps(scene)


def test_the_scene_is_served_publicly_and_cached():
    assert 'url.path.rstrip("/") == \\\n                "/api/world/neighbour-scene"' \
        in RUNTIME
    assert '"access-control-allow-origin": "*",' in RUNTIME
    assert "cache_seconds=instance_world_scene.CACHE_SECONDS" in RUNTIME
    assert "await edge_cache_put(cache_key, response)" in RUNTIME
    assert "cache_seconds=instance_world_scene.FAILED_CACHE_SECONDS" in RUNTIME


def test_the_scene_needs_a_configured_origin_and_a_saved_world():
    scene = RUNTIME.split("    async def _instance_world_scene(", 1)[1]
    scene = scene.split("\n    async def ", 1)[0]
    assert '"url.hostname' not in scene and "url.hostname" not in scene
    assert 'return json_response({"error": "not_found"}, status=404)' in scene
    assert "if not await self._instance_world_saved():\n            return None" in scene
    assert "if scene is not None:\n                return scene" in RUNTIME
    assert 'instance_world_scene = _LazyModule("instance_world_scene")' \
        in ENTRY

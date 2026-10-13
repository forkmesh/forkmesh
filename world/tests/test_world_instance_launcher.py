"""Start-your-world plots, travel, mirror launch and celebration contracts."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
APP = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (ROOT.parent / "world/public/world/world.css").read_text(encoding="utf-8")
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src/entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
QT_CONTROL = (
    ROOT.parent / "desktop/src/MainWindowControlNode.cpp"
).read_text(encoding="utf-8")


def test_instance_booth_and_in_world_launchers_are_gone():
    for retired in (
        "forkmesh-instance-launch-booth",
        "instance-launch-booth",
        "instanceBooth",
        "INSTANCE_GARDEN_POSITION",
        "onInstanceBoothSelect",
        "LAUNCH INSTANCE",
        "CREATE A NEW WORLD",
        "data-world-instance-launcher",
        "data-world-start-world",
        "toggleInstanceLauncher",
        "toggleStartWorld",
        "continueInstanceLaunch",
        "downloadInstanceSetupCard",
        "startWorldSetupScript",
        "continueStartWorldServer",
    ):
        assert retired not in SCENE
        assert retired not in APP
    assert "PRIVATE INSTANCE BOOTH" not in APP
    assert "forkmesh-instance-world-portal" not in SCENE
    assert ".world-instance-launcher" not in CSS
    assert ".world-start-world" not in CSS


def test_start_world_signs_open_the_start_instance_page():
    assert "onStartWorldSignSelect: (plot) => this.openStartInstance(plot)," in APP
    assert "onStartWorldSignSelect(hit.object.userData.plot);" in SCENE
    assert "board.userData.plot = index + 1;" in SCENE
    opener = APP.split("  openStartInstance(plot = \"\") {", 1)[1].split("\n  }\n", 1)[0]
    assert 'new URL("/start", WORLD_PUBLIC_SITE_ORIGIN)' in opener
    assert 'url.searchParams.set("plot"' in opener
    assert '"noopener,noreferrer"' in opener
    assert "password" not in opener.lower()


def test_front_garden_water_and_recreation_are_interactive():
    assert 'officeFrontGarden.name = "forkmesh-office-front-garden"' in SCENE
    # The rock waterfall beside the gym was retired.
    assert "forkmesh-interactive-waterfall" not in SCENE
    assert "garden-waterfall" not in SCENE
    assert "waterfallSplashUntil" not in SCENE
    assert 'drinkingFountain.name = "forkmesh-drinking-fountain"' in SCENE
    assert 'userData.interactive = "drinking-fountain"' in SCENE
    assert "onDrinkWater();" in SCENE
    assert "const GYM_POSITION = Object.freeze([66, 0.14, -137])" in SCENE
    assert "const SWING_SET_POSITION = Object.freeze([43, 0, -137])" in SCENE
    assert 'name: "forkmesh-swing-park-bench-west"' in SCENE
    assert 'name: "forkmesh-swing-park-bench-east"' in SCENE
    assert "x: 46" in SCENE
    assert "z: -129" in SCENE
    assert "z: -145" in SCENE


def test_node_plaza_grows_and_sol_is_a_floating_fireball():
    assert 'nodeYardConcrete.name = "reward-node-concrete-plaza"' in SCENE
    assert "farthestRadius + 3.2" in SCENE
    assert "const NODE_PLAZA_MAX_RADIUS = 28;" in SCENE
    assert "Math.min(\n        NODE_PLAZA_MAX_RADIUS," in SCENE
    assert 'sun.name = "reward-pool-fireball-sun"' in SCENE
    assert 'fireShell.name = "reward-pool-fireball-corona"' in SCENE
    fountain = SCENE.split("function createFountain(", 1)[1].split(
        "\n}\n", 1
    )[0]
    assert "const stem =" not in fountain


def test_launch_mirror_hands_token_to_desktop_without_a_secret_url():
    assert 'mirrorButton.userData.interactive = "launch-vultr-mirror"' in SCENE
    assert "launchVultrMirrorFromWorld" in APP
    launch = APP.split("async launchVultrMirrorFromWorld()", 1)[1].split(
        "\n  }\n", 1
    )[0]
    assert "navigator.clipboard.writeText(token)" in launch
    assert 'launchURL.searchParams.set("mode", "vultr")' in launch
    assert 'launchURL.searchParams.set("token"' not in launch
    assert 'bounded(QStringLiteral("mode"), 16)' in QT_CONTROL
    assert "QApplication::clipboard()->clear()" in QT_CONTROL
    assert "createVultrMirrorFromForm();" in QT_CONTROL


def test_approved_instances_become_world_portals_and_celebrate_for_ten_minutes():
    assert '"joinedAt": max(0, int(row.get("approved_at") or 0))' in ENTRY
    assert 'tower.userData.interactive = "federated-world-portal"' in SCENE
    assert 'label.userData.interactive = "federated-world-portal"' in SCENE
    assert "onFederatedWorldTravel" in SCENE
    assert "10 * 60 * 1000" in APP
    assert "startInstanceDirectoryPolling" in APP
    assert "world-instance-fireworks" in APP
    assert "A new independent World joined the federation" in APP
    assert "@keyframes world-instance-firework" in CSS
    assert "@media (prefers-reduced-motion: reduce)" in CSS

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
PUBLIC = ROOT / "public"
HOME = (ROOT.parent / "www" / "public" / "index.html").read_text(encoding="utf-8")
SCENE = world_scene_source()
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
WORLD_DURABLE_OBJECTS = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
).read_text(encoding="utf-8")


def test_home_navigation_reaches_the_world_before_the_app():
    brand = HOME.index('class="home-brand"')
    menu = HOME.index('id="mobile-menu-toggle"')
    panel = HOME.index('id="home-mobile-panel"')
    assert brand < menu < panel
    mobile_world = HOME.index('<a href="https://world.forkmesh.com/">World</a>', panel)
    mobile_app = HOME.index('<a href="https://app.forkmesh.com/">App</a>', panel)
    assert mobile_world < mobile_app
    desktop_world = HOME.index(
        'href="https://world.forkmesh.com/"', HOME.index('class="home-nav-links"'))
    desktop_app = HOME.index('href="https://app.forkmesh.com/"', desktop_world)
    assert desktop_world < desktop_app < menu
    assert HOME.count('href="https://app.forkmesh.com/"') >= 2
    cta = HOME.index('class="cap-world-cta" href="https://world.forkmesh.com/"')
    assert "Join World" in HOME[cta:HOME.index("</a", cta)]
    assert "@media (max-width: 1023px)" in HOME


def test_live_world_count_comes_from_current_durable_object_sockets():
    assert 'path.rstrip("/") == "/api/world/online"' in WORLD_DURABLE_OBJECTS
    assert '"online": len(self._live_sockets(cleanup=True))' in WORLD_DURABLE_OBJECTS
    assert '("/api/world/online", "/api/world/online/")' in ENTRY


def test_elevator_uses_upper_corner_first_person_framing_and_restores_view():
    assert "function lockOfficeElevatorCamera()" in SCENE
    assert 'setCameraMode("first-person", "office-elevator-enter")' in SCENE
    assert "cameraEye.set(-3.72, 6.46, 2.7)" in SCENE
    assert "cameraLookTarget.set(3.4, 3.15, -5.4)" in SCENE
    assert "elevatorPanel.position.set(4.34, 3.3, -0.6)" in SCENE
    assert "elevatorPanel.rotation.y = -Math.PI / 2" in SCENE
    assert "camera.fov = 58" in SCENE
    assert "function releaseOfficeElevatorCamera(released = true)" in SCENE
    assert 'setCameraMode(prior, "office-elevator-exit")' in SCENE

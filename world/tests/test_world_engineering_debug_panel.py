from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()


def test_engineering_floor_has_live_bounded_optimization_telemetry():
    for contract in (
        "function engineeringDebugTexture(THREE, snapshot = {})",
        "function createEngineeringDebugPanel(THREE)",
        '"forkmesh-engineering-live-debug-panel"',
        '"ENGINEERING · LIVE DEBUG CONTROL"',
        "GREEN HEALTHY · ORANGE OPTIMIZE · RED ACT",
        "addOfficeDeferredFloorDisplay(\n"
        '    engineeringFloor,\n'
        '    engineeringDebugPanel,\n'
        '    "engineering",\n'
        "  )",
        '"FRAMES / SECOND"',
        '"LONGEST FRAME"',
        '"DRAW CALLS"',
        '"TRIANGLES"',
        '"GEOMETRIES"',
        '"TEXTURES"',
        '"GPU PROGRAMS"',
        '"ANIMATION CALLBACKS"',
        '"INTERACTIVE TARGETS"',
        '"JS HEAP"',
        "renderer.info?.render?.calls",
        "renderer.info?.memory?.textures",
        "previous?.dispose?.()",
    ):
        assert contract in SCENE


def test_debug_panel_uses_green_orange_red_thresholds():
    assert 'status === "critical"' in SCENE
    assert 'status === "warning"' in SCENE
    assert 'statusHigh(engineeringDebugLongestFrameMs, 24, 40)' in SCENE
    assert "statusLow(fps, 50, 30)" in SCENE


def test_compact_debug_panel_can_toggle_a_mesh_only_triangle_view():
    world = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(
        encoding="utf-8"
    )
    css = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(
        encoding="utf-8"
    )

    for contract in (
        "data-world-triangle-view",
        "View triangles",
        "this.world?.setTriangleView?.(enabled)",
    ):
        assert contract in world
    for contract in (
        "function setTriangleView(enabled = false)",
        "if (child.isMesh) child.layers.enable(TRIANGLE_VIEW_LAYER)",
        "if (child.isMesh && child.userData.ground)",
        "child.layers.disable(TRIANGLE_VIEW_LAYER)",
        "camera.layers.set(TRIANGLE_VIEW_LAYER)",
        "scene.overrideMaterial = triangleViewMaterial",
        "labelLayer.hidden = true",
        "scene.overrideMaterial = null",
        "setTriangleView,",
    ):
        assert contract in SCENE
    assert ".world-diagnostics-triangle-control" in css

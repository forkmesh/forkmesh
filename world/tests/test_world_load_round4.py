import json
import shutil
import subprocess
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
SCENE_DIR = WORLD / "scene"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
NODE = shutil.which("node")


def node(script: str) -> dict:
    if not NODE:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        timeout=60,
    )
    return json.loads(result.stdout)


def block(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin)]


def module_text(relative: str) -> str:
    return (SCENE_DIR / relative).read_text(encoding="utf-8")


def test_fresh_arrival_waits_for_the_benches_before_sitting():
    rides = SCENE_DIR / "runtime" / "rides.js"
    result = node(
        f"""
      globalThis.window = globalThis;
      const {{ functions }} = await import({json.dumps(rides.as_uri())});
      const ctx = {{ campfire: {{ userData: {{}} }} }};
      const api = functions({{ THREE: {{}}, ctx, fns: {{}}, onMovement() {{}} }});
      const calls = [];
      const before = api.campfireSeatsReady();
      api.whenCampfireSeatsReady(() => calls.push("first"));
      api.notifyCampfireSeatsReady();
      const early = calls.length;
      ctx.campfire.userData.seatBenches = [{{ seat: {{}} }}];
      ctx.campfire.userData.seatByName = new Map();
      api.notifyCampfireSeatsReady();
      api.whenCampfireSeatsReady(() => calls.push("late"));
      api.notifyCampfireSeatsReady();
      process.stdout.write(JSON.stringify({{ before, early, calls, after: api.campfireSeatsReady() }}));
    """
    )
    assert result == {"before": False, "early": 0, "calls": ["first", "late"], "after": True}
    seat = block(APP, "  seatFreshArrivalAtCampfire() {", "\n  returnToCampfireBench() {")
    assert "this.world?.campfireSeatsReady?.() === false" in seat
    assert "this.world.whenCampfireSeatsReady?.(() => {" in seat
    assert "this.freshArrivalMoved ||" in seat
    movement = block(APP, "  handleMovement(movement) {", "this.setWorldRightRailExpanded(false);")
    assert "this.freshArrivalMoved = true;" in movement
    lounge = module_text("runtime/remote-players.js")
    assert lounge.index("ctx.campfire.userData.seatByName = seatByName;") < lounge.index(
        "queueMicrotask(() => fns.notifyCampfireSeatsReady());"
    )


def test_enclosure_cost_counts_the_current_triangles():
    utils = SCENE_DIR / "object-utils.js"
    result = node(
        f"""
      const {{ countTriangles, triangleWeight }} = await import({json.dumps(utils.as_uri())});
      const mesh = (count) => ({{ isMesh: true, geometry: {{ attributes: {{ position: {{ count }} }} }} }});
      const children = [mesh(30)];
      const root = {{ traverse(visit) {{ children.forEach(visit); }} }};
      const first = [countTriangles(root), triangleWeight(root)];
      children.push(mesh(3000));
      const second = [countTriangles(root), triangleWeight(root)];
      process.stdout.write(JSON.stringify({{ first, second }}));
    """
    )
    assert result == {"first": [10, 10], "second": [1010, 10]}
    compact = module_text("districts/compact-districts.js")
    verdict = block(compact, "  function enclosureExteriorCostVerdict(", "\n  function enclosureWalkUnchanged(")
    assert "countTriangles(root) <= ENCLOSURE_EXTERIOR_TRIANGLE_LIMIT" in verdict
    assert "triangleWeight" not in verdict


def test_populate_leaves_distant_data_for_after_the_first_interactive_frame():
    populate = block(APP, "      await this.nextBootPaint();\n      if (this.destroyed) return;\n      this.world.updateIdentity(", "const firstView = await this.world.prepareFirstView")
    deferred = block(populate, "      [\n        () => this.pushLiveMirrorNodes(),", "].forEach((step) => {")
    for call in (
        "this.pushLiveMirrorNodes()",
        "this.world.updateBots(this.botDirectory)",
        "this.world.updateFediverseDirectory(this.fediverseDirectory)",
        "this.startMastodonRefresh();",
        "this.startSocialBannersRefresh();",
        "this.syncRepositoryScene()",
    ):
        assert call in deferred
        assert populate.count(call) == 1
    each = block(populate, "].forEach((step) => {", "\n      });")
    assert "void this.afterWorldInteractive().then(() => {" in each
    assert "step();" in each
    assert "this.seatFreshArrivalAtCampfire();" not in deferred


def test_only_nearby_first_view_plates_paint_before_the_reveal():
    diagnostics = module_text("runtime/diagnostics.js")
    note = block(diagnostics, "  const noteFirstViewTextures = (material, object) => {", "\n  };")
    assert "FIRST_VIEW_PAINT_DISTANCE ** 2" in note
    prepare = block(diagnostics, "  async function prepareFirstView(", "\n  function scheduleGpuWarmup() {")
    assert prepare.index("warmFirstView();") < prepare.index("await yieldSceneBuild();") < prepare.index("paintFirstViewCanvases(sliceMs)")

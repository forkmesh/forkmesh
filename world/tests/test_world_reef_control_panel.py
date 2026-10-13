import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = WORLD / "scene"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
PANEL = (SCENE / "aquarium-panel.js").read_text(encoding="utf-8")
STEERING = (SCENE / "aquarium-schooling.js").read_text(encoding="utf-8")
AQUARIUM = (SCENE / "office-aquarium.js").read_text(encoding="utf-8")
FLOORS = (SCENE / "districts" / "office-floors.js").read_text(encoding="utf-8")
POINTER = (SCENE / "runtime" / "pointer.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
NODE = shutil.which("node")

CANVAS_STUB = """
const gradient = { addColorStop() {} };
const noop = () => gradient;
const context = new Proxy({}, {
  get: (target, key) => (key in target ? target[key] : noop),
  set: (target, key, value) => ((target[key] = value), true),
});
globalThis.document = {
  createElement: () => ({ width: 1, height: 1, dataset: {}, style: {}, getContext: () => context }),
};
"""

BUILD = CANVAS_STUB + """
const THREE = await import(%(three)s);
const { createOfficeMarineAquarium } = await import(%(aquarium)s);
const panelModule = await import(%(panel)s);
const constants = await import(%(constants)s);
const tower = await import(%(tower)s);
const animated = [];
const interactive = [];
const aquarium = createOfficeMarineAquarium(THREE, animated);
const users = [];
for (let index = 0; index < 96; index += 1) {
  users.push({
    name: `reef-member-${index}`,
    activityBucket: index %% 2 ? "hour" : "",
    countryCode: ["US", "DE", "BR"][index %% 3],
    browser: index %% 2 ? "CHROME" : "FIREFOX",
    os: "LINUX",
  });
}
aquarium.setUsers(users);
aquarium.setFishBodiesVisible(true);
aquarium.setAnimationActive(true);
let clock = 1000;
const panel = panelModule.createAquariumControlPanel(THREE, aquarium, {
  animated,
  interactive,
  now: () => clock,
});
const office = new THREE.Group();
office.add(aquarium.group);
office.updateMatrixWorld(true);
const tick = (count) => {
  for (let index = 0; index < count; index += 1) {
    clock += 50;
    for (const callback of animated) callback(clock);
  }
};
"""


def run_node(script):
    result = subprocess.run(
        [NODE, "--expose-gc", "--input-type=module", "-e", script],
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    return json.loads(result.stdout.strip().splitlines()[-1])


def build(body):
    return BUILD % {
        "three": json.dumps(THREE.as_uri()),
        "aquarium": json.dumps((SCENE / "office-aquarium.js").as_uri()),
        "panel": json.dumps((SCENE / "aquarium-panel.js").as_uri()),
        "constants": json.dumps((SCENE / "gameplay-constants.js").as_uri()),
        "tower": json.dumps((WORLD / "world-office-tower.js").as_uri()),
    } + body


def block(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_panel_is_mounted_on_the_tank_and_nothing_floats():
    report = run_node(build("""
aquarium.group.position.set(0, 0, 0);
aquarium.group.rotation.set(0, 0, 0);
office.updateMatrixWorld(true);
const box = (object) => new THREE.Box3().setFromObject(object, true);
const meshes = [];
panel.group.traverse((child) => {
  if (child.isMesh && child.visible) meshes.push(child);
});
const frame = aquarium.group.getObjectByName("forkmesh-office-aquarium-frame");
const glass = aquarium.frontGlass;
const housing = panel.group.getObjectByName("forkmesh-office-aquarium-reef-panel-housing");
const nodes = [...frame.children, glass, ...meshes];
const boxes = nodes.map((node) => box(node).expandByScalar(0.004));
const reached = new Set(frame.children.map((_, index) => index));
let grew = true;
while (grew) {
  grew = false;
  for (let index = 0; index < nodes.length; index += 1) {
    if (reached.has(index)) continue;
    for (const other of reached) {
      if (boxes[index].intersectsBox(boxes[other])) {
        reached.add(index);
        grew = true;
        break;
      }
    }
  }
}
const floating = nodes
  .filter((_, index) => !reached.has(index))
  .map((node) => node.name || node.geometry?.type);
const whole = new THREE.Box3();
for (const mesh of meshes) whole.union(box(mesh));
const housingBox = box(housing);
const glassBox = box(glass);
const bounds = constants.OFFICE_AQUARIUM_BOUNDS;
const anchors = [];
office.traverse((child) => {
  if (/control-anchor/.test(child.name)) anchors.push(child.name);
});
console.log(JSON.stringify({
  parent: panel.group.parent === aquarium.group,
  floating,
  meshes: meshes.length,
  min: whole.min.toArray(),
  max: whole.max.toArray(),
  housingBottom: housingBox.min.y,
  housingBack: housingBox.min.x,
  glassFront: glassBox.max.x,
  bounds,
  avatarRadius: tower.OFFICE_AVATAR_RADIUS,
  tankHeight: aquarium.dimensions.height,
  anchors,
  panelWidth: panelModule.AQUARIUM_PANEL_WIDTH,
  panelHeight: panelModule.AQUARIUM_PANEL_HEIGHT,
}));
"""))
    assert report["parent"] is True
    assert report["meshes"] > 20
    assert report["floating"] == []
    assert report["anchors"] == []
    bounds = report["bounds"]
    assert abs(report["housingBottom"] - 0.5) < 0.01
    assert 0 <= report["housingBack"] - report["glassFront"] < 0.05
    assert report["min"][0] >= bounds["minX"]
    assert report["max"][0] <= bounds["maxX"] + report["avatarRadius"]
    assert report["min"][2] >= bounds["minZ"]
    assert report["max"][2] <= bounds["maxZ"]
    assert report["min"][1] >= 0
    assert report["max"][1] <= report["tankHeight"] / 3
    assert report["panelWidth"] <= 3 and report["panelHeight"] <= 2


def test_floating_html_reef_controls_are_gone():
    for token in (
        "world-aquarium-control-panel",
        "aquariumControlToggle",
        "aquariumFeedAction",
        "updateScreenLabel(",
        "controlAnchor",
    ):
        assert token not in FLOORS, token
    assert "world-aquarium" not in CSS
    assert "controlAnchor" not in AQUARIUM
    assert "createAquariumControlPanel(THREE, officeAquarium" in FLOORS


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_every_old_reef_function_is_reachable_from_a_physical_control():
    report = run_node(build("""
const steps = {};
const read = () => ({ ...panel.state(clock), screen: panel.screenKey });
tick(4);
steps.start = read();
steps.feed = panel.press("feed", clock);
tick(8);
steps.afterFeed = read();
const directions = () => aquarium.group
  .getObjectByName("forkmesh-office-aquarium-user-fish-anchors").children.length;
steps.tap = panel.press("tap", clock);
steps.afterTap = aquarium.getControlState(clock).glassTappedAt;
steps.backdrop = panel.press("backdrop", clock);
tick(8);
steps.afterBackdrop = read();
steps.light = panel.press("light", clock);
tick(20);
steps.afterLight = read();
const modes = [];
for (let index = 0; index < 4; index += 1) {
  modes.push(panel.press("school", clock));
  tick(8);
}
steps.modes = modes;
steps.afterSchool = read();
steps.follow = panel.press("follow", clock);
steps.glass = panel.press("glass-response", clock);
tick(8);
steps.end = read();
steps.pads = [...panel.controls.keys()];
steps.targets = panel.targets.map((target) => ({
  interactive: target.userData.interactive,
  control: target.userData.aquariumControl || "",
  floor: target.userData.officeFloorId,
  size: new THREE.Box3().setFromObject(target, true).getSize(new THREE.Vector3()).toArray(),
}));
steps.registered = panel.targets.every((target) => interactive.includes(target));
steps.hover = panel.hover(panel.targets[0], null, clock);
steps.hoverGlass = panel.hover(aquarium.frontGlass, null, clock);
steps.hoverNone = panel.hover(null, null, clock);
steps.touch = panel.touchGlass(aquarium.group.localToWorld(new THREE.Vector3(1.5, 5, -2)), clock);
steps.touchedAt = aquarium.getControlState(clock).glassTouchedAt;
console.log(JSON.stringify(steps));
"""))
    assert report["start"]["feeding"] is False
    assert report["feed"] is True
    assert report["afterFeed"]["feeding"] is True
    assert "FEEDING" in report["afterFeed"]["screen"]
    assert report["tap"] == 96
    assert report["afterTap"] is not None
    assert report["start"]["backdropOpaque"] is True
    assert report["backdrop"] is False
    assert report["afterBackdrop"]["backdropOpaque"] is False
    assert report["light"] is False
    assert report["afterLight"]["lightEnabled"] is False
    assert report["afterLight"]["lever"] > 2.5
    assert report["start"]["lever"] < 0.5
    assert "LIGHT OFF" in report["afterLight"]["screen"]
    assert report["modes"] == ["species", "colour", "scatter", "auto"]
    assert "SCHOOL" in report["afterSchool"]["screen"]
    assert report["follow"] is True
    assert report["glass"] == "shy"
    assert report["end"]["followEnabled"] is True
    assert report["end"]["glassResponse"] == "shy"
    assert set(report["pads"]) == {
        "feed", "tap", "follow", "school", "backdrop", "glass-response", "light",
    }
    assert report["registered"] is True
    for target in report["targets"]:
        assert target["floor"] == "lobby"
        if target["interactive"] == "aquarium-control":
            assert min(target["size"][1], target["size"][2]) >= 0.43, target
    assert report["hover"] == "control"
    assert report["hoverGlass"] == "glass"
    assert report["hoverNone"] == ""
    assert report["touchedAt"] is not None


def test_panel_clicks_and_hover_go_through_the_world_pointer():
    assert 'hit?.object?.userData?.interactive === "aquarium-control"' in POINTER
    assert "ctx.aquariumPanel.press(" in POINTER
    assert 'hit?.object?.userData?.interactive === "aquarium-glass"' in POINTER
    assert "ctx.aquariumPanel.touchGlass(hit.point)" in POINTER
    hover = block(POINTER, "  function updateAquariumPanelHover(event) {", "\n  function runHoverPick() {")
    assert "intersectObjects(panel.targets, false)" in hover
    assert "canvas.dataset.hoverTarget = target" in hover
    assert '.world-canvas[data-hover-target="control"] {\n  cursor: pointer;' in CSS
    assert "updateAquariumPanelHover(hoverPickEvent);" in POINTER
    assert 'pad.userData.raycastProxy = true' in PANEL
    assert "control.hovered ? 0.45 : 0" in PANEL


def test_reef_controls_stay_local_to_the_visitor():
    for source in (PANEL, FLOORS):
        assert "sendWorld" not in source
        assert "socket" not in source.lower()
    assert 'saveAquariumToggle("forkmesh-world-aquarium-backdrop"' in FLOORS
    assert 'saveAquariumToggle("forkmesh-world-aquarium-light"' in FLOORS


STEERING_RUN = """
const steering = await import(%(steering)s);
const reef = steering.createReefSteering(0);
const bounds = [-1.08, 1.12, 1.35, 10.7, -12.6, 12.6];
reef.setBounds(...bounds);
const count = 180;
const groups = 6;
reef.resize(count);
let seed = 7;
const random = () => ((seed = (seed * 16807) %% 2147483647) / 2147483647);
for (let index = 0; index < count; index += 1) {
  reef.place(
    index,
    bounds[0] + random() * (bounds[1] - bounds[0]),
    bounds[2] + random() * (bounds[3] - bounds[2]),
    bounds[4] + random() * (bounds[5] - bounds[4]),
    0, 0, 0,
  );
  reef.setGroup(index, index %% groups);
}
reef.setGroupCount(groups);
const radii = () => {
  const out = [];
  for (let group = 0; group < groups; group += 1) {
    let cx = 0, cy = 0, cz = 0, members = 0;
    for (let index = 0; index < count; index += 1) {
      if (reef.group[index] !== group) continue;
      cx += reef.position[index * 3];
      cy += reef.position[index * 3 + 1];
      cz += reef.position[index * 3 + 2];
      members += 1;
    }
    cx /= members; cy /= members; cz /= members;
    let radius = 0;
    for (let index = 0; index < count; index += 1) {
      if (reef.group[index] !== group) continue;
      radius = Math.max(radius, Math.hypot(
        reef.position[index * 3] - cx,
        reef.position[index * 3 + 1] - cy,
        reef.position[index * 3 + 2] - cz,
      ));
    }
    out.push(radius);
  }
  return out;
};
const outside = () => {
  let escaped = 0;
  for (let index = 0; index < count; index += 1) {
    const x = reef.position[index * 3];
    const y = reef.position[index * 3 + 1];
    const z = reef.position[index * 3 + 2];
    const slack = 1e-4;
    if (
      x < bounds[0] - slack || x > bounds[1] + slack ||
      y < bounds[2] - slack || y > bounds[3] + slack ||
      z < bounds[4] - slack || z > bounds[5] + slack
    ) escaped += 1;
  }
  return escaped;
};
let time = 0;
const run = (steps, mode) => {
  let escaped = 0;
  for (let step = 0; step < steps; step += 1) {
    time += 50;
    reef.step(0.05, mode, time);
    escaped += outside();
  }
  return escaped;
};
const before = radii();
const schoolEscapes = run(500, steering.REEF_STEER_SCHOOL);
const schooled = radii();
const holdEscapes = run(300, steering.REEF_STEER_SCHOOL);
const held = radii();
const scatterEscapes = run(300, steering.REEF_STEER_SCATTER);
const scattered = radii();
reef.setTarget(1.1, 6, 0, 1, 6.5);
const nearBefore = (() => { let n = 0; for (let i = 0; i < count; i += 1) if (Math.hypot(reef.position[i*3]-1.1, reef.position[i*3+1]-6, reef.position[i*3+2]) < 2.5) n += 1; return n; })();
run(120, steering.REEF_STEER_TRACK);
const nearAfter = (() => { let n = 0; for (let i = 0; i < count; i += 1) if (Math.hypot(reef.position[i*3]-1.1, reef.position[i*3+1]-6, reef.position[i*3+2]) < 2.5) n += 1; return n; })();
reef.clearTarget();
run(2000, steering.REEF_STEER_SCHOOL);
globalThis.gc();
const heapBefore = process.memoryUsage().heapUsed;
for (let step = 0; step < 4000; step += 1) {
  time += 50;
  reef.step(0.05, step %% 2 ? steering.REEF_STEER_SCHOOL : steering.REEF_STEER_SCATTER, time);
}
const heapGrowth = process.memoryUsage().heapUsed - heapBefore;
console.log(JSON.stringify({
  before, schooled, held, scattered,
  schoolEscapes, holdEscapes, scatterEscapes,
  nearBefore, nearAfter, heapGrowth,
}));
"""


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_schooling_step_keeps_groups_together_inside_the_tank():
    report = run_node(STEERING_RUN % {"steering": json.dumps((SCENE / "aquarium-schooling.js").as_uri())})
    assert min(report["before"]) > 6
    assert max(report["schooled"]) < 2.2
    assert max(report["held"]) < 2.2
    assert min(report["scattered"]) > 4
    assert report["schoolEscapes"] == 0
    assert report["holdEscapes"] == 0
    assert report["scatterEscapes"] == 0
    assert report["nearAfter"] > report["nearBefore"] + 5


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_schooling_step_does_not_allocate_per_frame():
    report = run_node(STEERING_RUN % {"steering": json.dumps((SCENE / "aquarium-schooling.js").as_uri())})
    assert report["heapGrowth"] < 64 * 1024, report["heapGrowth"]


def test_fish_loop_steering_code_is_allocation_free():
    step = block(STEERING, "  function step(deltaSeconds, mode, time) {", "\n  setBounds(bounds[0]")
    grid = block(STEERING, "  function buildGrid() {", "\n  function step(")
    steering = block(AQUARIUM, "  function updateReefSteering(time, deltaMs, animate) {", "\n  function tapGlass(")
    loop = block(AQUARIUM, "    const steerBlend = updateReefSteering(", "\n    updateInstancedUserFish();")
    for body in (step, grid, steering, loop):
        assert "new " not in body
        assert "=>" not in body
        assert ".map(" not in body and ".filter(" not in body and "..." not in body
    assert "Float32Array" in STEERING and "Int32Array" in STEERING
    assert "curve.getTangentAt" not in AQUARIUM
    assert "if (!animate || !fishBodiesVisible || !fishStates.length)" in steering
    assert "if (!animationActive || !aquariumInView || time < nextAnimationAt) return;" in AQUARIUM
    assert "ctx.officeAquarium.setInView(" in FLOORS


def test_feeding_gathers_the_fish_at_the_surface():
    assert "tankHeight - 0.62 - visualProgress * state.drift" in AQUARIUM
    assert "feedingCenter.set(0.68, 10.1 - visualProgress * 2.2, -1.8)" in AQUARIUM
    assert "tankHeight - 1.25," in AQUARIUM

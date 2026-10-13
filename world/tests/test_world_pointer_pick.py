import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
POINTER_MODULE = WORLD / "scene" / "runtime" / "pointer.js"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
SCENE = world_scene_source()
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


HARNESS = f"""
  const THREE = await import({json.dumps(THREE_MODULE.as_uri())});
  const {{ functions }} = await import({json.dumps(POINTER_MODULE.as_uri())});
  let clock = 1000;
  Object.defineProperty(globalThis, "performance", {{ value: {{ now: () => clock }}, configurable: true }});
  const frames = [];
  globalThis.window = {{
    requestAnimationFrame(callback) {{ frames.push(callback); return frames.length; }},
    cancelAnimationFrame(id) {{ frames[id - 1] = null; }},
  }};
  const runFrames = () => {{
    const pending = frames.splice(0);
    pending.forEach((callback) => callback && callback(clock));
  }};
  const scene = new THREE.Scene();
  const sign = new THREE.Group();
  const board = new THREE.Mesh(new THREE.BoxGeometry(2, 2, 0.2), new THREE.MeshBasicMaterial());
  sign.add(board);
  scene.add(sign);
  scene.updateMatrixWorld(true);
  let raycasts = 0;
  const raycast = board.raycast.bind(board);
  board.raycast = (raycaster, hits) => {{ raycasts += 1; return raycast(raycaster, hits); }};
  const camera = new THREE.PerspectiveCamera(50, 800 / 600, 0.1, 100);
  camera.position.set(0, 0, 10);
  camera.lookAt(0, 0, 0);
  camera.updateMatrixWorld(true);
  let rectReads = 0;
  const ctx = {{
    canvasRectStale: true,
    renderer: {{ domElement: {{
      dataset: {{}},
      getBoundingClientRect() {{ rectReads += 1; return {{ left: 0, top: 0, width: 800, height: 600 }}; }},
      setPointerCapture() {{}},
      releasePointerCapture() {{}},
    }} }},
    pointer: new THREE.Vector2(),
    raycaster: new THREE.Raycaster(),
    camera,
    landmarkObjects: new Map([["fountain", {{ userData: {{ treasurySign: sign }} }}]]),
    officeSceneMode: "town",
    primaryPointerId: null,
    diagnosticsPointerWorstGapMs: 0,
    diagnosticsPointerMoves: 0,
    touchPointers: new Map(),
    pointerStart: new THREE.Vector2(),
    pointerLast: new THREE.Vector2(),
    pointerCurrent: new THREE.Vector2(),
    interactive: [],
    disposed: false,
  }};
  let hovers = 0;
  const api = functions({{
    THREE,
    onRewardBoardHover: () => {{ hovers += 1; }},
    onCameraLook: () => {{}},
    ctx,
    fns: {{}},
  }});
  const move = (x, y) => api.handlePointerMove({{
    pointerType: "mouse", pointerId: 1, clientX: x, clientY: y, preventDefault() {{}},
  }});
"""


def test_mouse_moves_between_frames_share_one_hover_pick():
    result = node(
        HARNESS
        + """
  for (let index = 0; index < 40; index += 1) move(400 + (index % 3), 300);
  const beforeFrame = { raycasts, hovers, rectReads, frames: frames.length };
  runFrames();
  process.stdout.write(JSON.stringify({ beforeFrame, raycasts, hovers, rectReads }));
"""
    )
    assert result["beforeFrame"] == {"raycasts": 0, "hovers": 0, "rectReads": 0, "frames": 1}
    assert result["raycasts"] == 1
    assert result["hovers"] == 1
    assert result["rectReads"] == 1


def test_a_ray_outside_the_sign_bounds_never_reaches_mesh_intersection():
    result = node(
        HARNESS
        + """
  move(4, 4);
  runFrames();
  const miss = { raycasts, hovers };
  clock += 500;
  move(400, 300);
  runFrames();
  const hit = { raycasts, hovers };
  clock += 500;
  move(400, 300);
  runFrames();
  process.stdout.write(JSON.stringify({ miss, hit, held: { raycasts, hovers } }));
"""
    )
    assert result["miss"] == {"raycasts": 0, "hovers": 0}
    assert result["hit"] == {"raycasts": 1, "hovers": 1}
    assert result["held"]["hovers"] == 1


def test_a_press_cancels_the_pending_hover_pick_and_drags_do_not_hover():
    result = node(
        HARNESS
        + """
  move(400, 300);
  api.handlePointerDown({ pointerType: "mouse", button: 0, pointerId: 1, clientX: 400, clientY: 300 });
  runFrames();
  const pressed = { raycasts, hovers, primary: ctx.primaryPointerId };
  clock += 500;
  move(420, 300);
  const dragFrames = frames.length;
  runFrames();
  ctx.disposed = true;
  ctx.primaryPointerId = null;
  clock += 500;
  move(400, 300);
  runFrames();
  process.stdout.write(JSON.stringify({ pressed, dragFrames, hovers }));
"""
    )
    assert result["pressed"]["hovers"] == 0
    assert result["pressed"]["primary"] == 1
    assert result["dragFrames"] == 0
    assert result["hovers"] == 0


def test_pointer_move_source_contract():
    move = SCENE[SCENE.index("function handlePointerMove(event) {"):]
    move = move[: move.index("\n  }\n")]
    assert "scheduleHoverPick(event);" in move
    assert "updateRewardBoardHover(" not in move
    assert "intersectObject" not in move.split("if (draggedBuildCard) {", 1)[0]
    assert "raycaster.ray.intersectsBox(hoverPickBoxFor(sign))" in SCENE
    assert "hoverPickFrame = window.requestAnimationFrame(runHoverPick);" in SCENE
    assert "if (disposed || primaryPointerId !== null) return;" in SCENE

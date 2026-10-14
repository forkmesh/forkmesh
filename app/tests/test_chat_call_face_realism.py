import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
RENDER = (PUBLIC / "chat-call-face-render.js").read_text(encoding="utf-8")
CALLS = (PUBLIC / "chat-call-faces.js").read_text(encoding="utf-8")
SYNC = (ROOT / "tools" / "sync_native_call_faces.py").read_text(encoding="utf-8")
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
WEB_CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[2].replace(/chat-call-faces\.js$/, "");
const load = (name) => import(pathToFileURL(base + name).href);
const faces = await load("chat-call-faces.js");
const geo = await load("chat-call-face-geometry.js");
const render = await load("chat-call-face-render.js");
const ex = await load("chat-call-face-expression.js");
const custom = await load("chat-call-face-custom.js");

const out = {};
const maya = faces.faceHead(faces.CALL_FACES[0]);
const tmpl = faces.faceTemplate();
const turn = (deg) => tmpl.map(([x, y, z]) => {
  const t = deg * Math.PI / 180;
  return [300 + 100 * (x * Math.cos(t) + z * Math.sin(t)), 300 + 100 * y];
});

out.turns = [-40, -20, 20, 40].map((deg) => {
  const live = turn(deg);
  const head = geo.liveHead(maya.model, live);
  const depths = geo.vertexDepths(maya.model, head.pose);
  const vertices = render.warpVertices(maya.mesh, maya.layout, head.points, 512, depths);
  return {
    deg,
    yaw: head.pose.yaw * 180 / Math.PI,
    noseTurn: geo.noseTurn(live),
    noseFront: depths[30] > depths[39] && depths[30] > depths[42] && depths[30] > depths[0] && depths[30] > depths[16],
    eyesFrontOfJaw: depths[36] > depths[0] && depths[45] > depths[16],
    ringBehind: depths.slice(78).every((d) => d < depths[33]),
    stride: vertices.length / (maya.mesh.triangles.length * 3),
    depthSlot: vertices[6],
  };
});
const front = geo.liveHead(maya.model, turn(0));
const flipped = front.pose;
out.frontDepthNose = geo.vertexDepths(maya.model, flipped)[30];

const image = (w, h, paint) => {
  const data = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const [r, g, b] = paint(x, y);
      const at = (y * w + x) * 4;
      data[at] = r; data[at + 1] = g; data[at + 2] = b; data[at + 3] = 255;
    }
  }
  return { data, width: w, height: h };
};
const pts = faces.facePoints(faces.CALL_FACES[1]);
const eye = ex.eyeBox(pts, 0);
const inEye = (x, y, box, sx = 1, sy = 1) => {
  const dx = x - box.center[0];
  const dy = y - box.center[1];
  const u = (dx * box.ux[0] + dy * box.ux[1]) / (box.half[0] * sx);
  const v = (dx * box.uy[0] + dy * box.uy[1]) / (box.half[1] * sy);
  return u * u + v * v;
};
const skinColor = [205, 160, 135];
const eyes = [ex.eyeBox(pts, 0), ex.eyeBox(pts, 1)];
const openFace = image(512, 512, (x, y) => {
  for (const box of eyes) {
    const d = inEye(x, y, box, 0.85, 0.75);
    if (d < 1) {
      const iris = inEye(x, y, { ...box, half: [box.half[1] * 0.7, box.half[1] * 0.7] });
      return iris < 1 ? [60, 40, 30] : [235, 235, 230];
    }
  }
  return skinColor;
});
const closedFace = image(512, 512, (x, y) => {
  for (const box of eyes) {
    const dx = x - box.center[0];
    const dy = y - box.center[1] - box.half[1] * 0.3;
    if (Math.abs(dy) < 1 && Math.abs(dx) < box.half[0] * 0.7) return [70, 50, 40];
  }
  return skinColor;
});
const mouth = ex.tongueWindow(pts);
const tongueFace = image(512, 512, (x, y) => (inEye(x, y, { ...mouth, half: [mouth.half[0] * 0.6, mouth.half[1] * 0.7] }) < 1 ? [200, 80, 95] : skinColor));
const skin = ex.skinReference(openFace, pts);
out.skin = skin;
out.open = ex.measureEye(openFace, eye, skin);
out.closed = ex.measureEye(closedFace, eye, skin);
out.tongueOpen = ex.measureTongue(openFace, mouth, skin);
out.tongueOut = ex.measureTongue(tongueFace, mouth, skin);
const lids = ex.closeLids(pts, [1, 0]);
out.lids = {
  leftGap: Math.abs(lids[37][1] - lids[41][1]) / Math.abs(pts[37][1] - pts[41][1]),
  rightGap: Math.abs(lids[43][1] - lids[47][1]) / Math.abs(pts[43][1] - pts[47][1]),
  untouched: ex.closeLids(pts, [0, 0]) === pts,
};
out.lookUniform = render.lookUniform([[0.2, -0.1], [0.3, 0.4]]);
out.photoEyes = render.photoEyes(maya.layout, 512);

const transform = custom.customFaceTransform(pts);
const placed = pts.map((point) => custom.applyTransform(transform.matrix, point));
const frame = geo.faceFrame(placed);
out.custom = { span: frame.span, mid: frame.mid, roll: frame.roll };
const disc = new Float32Array(128 * 128);
for (let y = 0; y < 128; y += 1) for (let x = 0; x < 128; x += 1) disc[y * 128 + x] = Math.hypot(x - 64, y - 64) < 30 ? 1 : 0;
const ring = custom.headRing(disc, 128, [64, 74], 20);
out.ring = { count: ring.length / 2, inside: ring.every((v) => v >= 0 && v <= 128), minRadius: Math.min(...Array.from({ length: ring.length / 2 }, (_, i) => Math.hypot(ring[i * 2] - 64, ring[i * 2 + 1] - 67))) };
const blurred = custom.boxBlur(disc, 128, 3, 2);
out.blur = { center: blurred[64 * 128 + 64], edge: blurred[64 * 128 + 94], far: blurred[0] };
out.choices = faces.callFaceChoices().map((face) => face.id);
out.isCustom = [faces.isCustomFace("custom"), faces.isCustomFace("maya")];
out.pupils = faces.pupilsFromRegions({ leftPupil: [[300, 100]], rightPupil: [[100, 100]] }, 0.5);
out.noPupils = faces.pupilsFromRegions({ leftPupil: [] }, 1);
let stored = null;
const storage = { getItem: () => stored, setItem: (_, value) => { stored = value; }, removeItem: () => { stored = null; } };
custom.rememberCustomFaceImage("data:image/jpeg;base64,AA", storage);
out.saved = custom.savedCustomFaceImage(storage);
custom.rememberCustomFaceImage("", storage);
out.cleared = custom.savedCustomFaceImage(storage);
let threw = "";
try { custom.buildCustomFace({ image: null, width: 1, height: 1, points: null }); } catch (error) { threw = error.message; }
out.noFace = threw;
console.log(JSON.stringify(out));
'''


def _node():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / "chat-call-faces.js")],
        input=HARNESS, capture_output=True, text=True, timeout=60, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


@pytest.fixture(scope="module")
def data():
    return _node()


def test_turning_keeps_the_nose_in_front_of_the_far_cheek(data):
    for turn in data["turns"]:
        assert turn["noseFront"] is True, turn
        assert turn["eyesFrontOfJaw"] is True, turn
        assert turn["ringBehind"] is True, turn
        assert (turn["yaw"] > 0) == (turn["deg"] > 0), turn
        assert (turn["noseTurn"] > 0) == (turn["deg"] > 0), turn
        assert turn["stride"] == 7
    assert data["frontDepthNose"] > 0.3


def test_head_mesh_is_drawn_with_a_depth_test():
    assert "gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, width, height);" in RENDER
    assert "warp: target(gl, w, h, gl.UNSIGNED_BYTE, true)," in RENDER
    assert "gl.enable(gl.DEPTH_TEST);" in RENDER
    assert "attribute float aDepth;" in RENDER
    assert "vertexDepths(face.model, head.pose)" in RENDER


def test_blink_shows_as_closed_lids_and_tongue_as_a_red_patch(data):
    assert data["open"]["edge"] > 2.5 * data["closed"]["edge"]
    assert data["open"]["dark"] > data["closed"]["dark"]
    assert data["tongueOut"] > data["tongueOpen"] + 0.2
    assert data["lids"]["leftGap"] < 0.25
    assert data["lids"]["rightGap"] == pytest.approx(1)
    assert data["lids"]["untouched"] is True


def test_gaze_and_eyelid_uniforms_follow_the_stand_in_eyes(data):
    assert data["lookUniform"] == pytest.approx([0.11, -0.055, 0.165, 0.22])
    left, right = data["photoEyes"]
    assert 0.3 < left[0] < 0.5 < right[0] < 0.7
    assert left[2] > left[3] > 0
    assert "uniform vec2 uClose;" in RENDER
    assert "tongueAt(vUv, cam)" in RENDER


def test_custom_photo_is_framed_like_the_built_in_faces(data):
    assert data["custom"]["span"] == pytest.approx(118, abs=0.5)
    assert data["custom"]["mid"] == pytest.approx([256, 285], abs=0.5)
    assert abs(data["custom"]["roll"]) < 1e-6
    assert data["ring"]["count"] == 28
    assert data["ring"]["inside"] is True
    assert data["ring"]["minRadius"] > 30
    assert data["blur"]["center"] == pytest.approx(1)
    assert 0 < data["blur"]["edge"] < 1
    assert data["blur"]["far"] == 0
    assert data["noFace"] == "face_not_found"


def test_your_photo_is_a_face_choice_everywhere(data):
    assert data["choices"] == ["maya", "theo", "kofi", "ingrid", "ren", "custom"]
    assert data["isCustom"] == [True, False]
    assert data["pupils"] == [[200, 200], [600, 200]]
    assert data["noPupils"] is None
    assert data["saved"] == "data:image/jpeg;base64,AA"
    assert data["cleared"] == ""
    assert '"isCustomFace",' in SYNC and '"callFaceChoices",' in SYNC
    assert 'PUBLIC / "chat-call-face-expression.js",' in SYNC
    assert 'PUBLIC / "chat-call-face-custom.js",' in SYNC
    assert 'id="chat-call-face-file"' in HTML
    assert '[CUSTOM_FACE_ID, "Your photo…"]' in WEB_CALLS
    assert "faceImage: call.faceImage" in WEB_CALLS
    assert "{ landmarks: true, mask: true, accurate: true }" in CALLS

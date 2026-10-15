import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
FACES = (PUBLIC / "chat-call-faces.js").read_text(encoding="utf-8")
CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
DESKTOP = ROOT.parent / "desktop" / "src"

FACES_HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[2].replace(/chat-call-faces\.js$/, "");
const load = (name) => import(pathToFileURL(base + name).href);
const faces = await load("chat-call-faces.js");
const geo = await load("chat-call-face-geometry.js");
const tracker = await load("chat-call-face-tracker.js");

const loop = (points, count, rotate) => {
  const out = geo.resamplePolyline([...points, points[0]], count + 1).slice(0, count);
  return out.slice(rotate).concat(out.slice(0, rotate));
};
const vision = (p) => {
  const pick = (idx) => idx.map((i) => p[i]);
  const band = (idx) => loop([...pick(idx).map(([x, y]) => [x, y - 3]), ...pick(idx).map(([x, y]) => [x, y + 3]).reverse()], 6, 2);
  return {
    faceContour: geo.resamplePolyline(pick([...Array(17).keys()]), 11).reverse(),
    leftEyebrow: band([22, 23, 24, 25, 26]),
    rightEyebrow: band([17, 18, 19, 20, 21]),
    leftEye: loop(pick([42, 43, 44, 45, 46, 47]), 8, 3),
    rightEye: loop(pick([36, 37, 38, 39, 40, 41]), 8, 5),
    noseCrest: geo.resamplePolyline(pick([27, 28, 29, 30]), 6),
    nose: pick([31, 32, 33, 34, 35]),
    outerLips: loop(pick([48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59]), 14, 4),
    innerLips: loop(pick([60, 61, 62, 63, 64, 65, 66, 67]), 6, 1),
  };
};
const range = (a, b) => [...Array(b - a).keys()].map((i) => i + a);
const worst = (one, two, idx, span) => Math.max(...idx.map((i) => Math.hypot(one[i][0] - two[i][0], one[i][1] - two[i][1]))) / span;

const out = { faces: [], vision: [] };
for (const face of faces.CALL_FACES) {
  const raw = geo.pointsFromFlat(face.points);
  out.faces.push({
    id: face.id,
    label: face.label,
    count: raw.length,
    inside: raw.every(([x, y]) => x >= 0 && y >= 0 && x <= 512 && y <= 512),
    span: geo.faceFrame(raw).span,
  });
  const canon = faces.facePoints(face);
  const back = geo.landmarksFromRegions(vision(raw));
  const span = geo.faceFrame(canon).span;
  out.vision.push({
    jaw: worst(back, canon, range(0, 17), span),
    eyes: worst(back, canon, range(36, 48), span),
    brows: worst(back, canon, range(17, 27), span),
    lips: worst(back, canon, range(48, 68), span),
  });
}
out.missingRegions = geo.landmarksFromRegions({ faceContour: [[0, 0]] });
out.unknown = faces.callFaceById("nobody");

const heads = faces.CALL_FACES.map((face) => faces.faceHead(face));
out.mesh = heads.map((head, i) => ({
  triangles: head.mesh.triangles.length,
  mouth: head.mesh.triangles.filter((t) => t.mouth).length,
  mouthOnlyInnerLips: head.mesh.triangles.filter((t) => t.mouth).every((t) => t.v.every((v) => (v >= 60 && v < 68) || v >= head.mesh.extraStart)),
  extras: head.layout.length - head.mesh.extraStart,
  vertices: head.layout.length,
  valid: head.mesh.triangles.every((t) => t.v.every((v) => v >= 0 && v < head.layout.length)),
  ring: faces.CALL_FACES[i].head.length / 2,
  ringInside: head.layout.slice(78, head.mesh.extraStart).every(([x, y]) => x >= 0 && y >= 0 && x <= 512 && y <= 512),
  photoArea: head.mesh.triangles.reduce((sum, t) => {
    const [a, b, c] = t.v.map((v) => head.layout[v]);
    return sum + Math.abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2;
  }, 0),
  flipped: head.mesh.triangles.filter((t) => {
    if (t.mouth) return false;
    const [a, b, c] = t.v.map((v) => head.layout[v]);
    const [p, q, r] = t.v.map((v) => head.model[v]);
    const photo = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    const model = (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]);
    return Math.sign(photo) !== Math.sign(model) && Math.abs(photo) > 1;
  }).length,
}));
const theo = heads[1];
const rotate = (yaw, pitch) => ([x, y, z]) => {
  const X = x * Math.cos(yaw) + z * Math.sin(yaw);
  let Z = -x * Math.sin(yaw) + z * Math.cos(yaw);
  const Y = y * Math.cos(pitch) - Z * Math.sin(pitch);
  return [320 + 60 * X, 200 + 60 * Y];
};
out.pose = [[0, 0], [0.4, 0], [0, 0.3], [-0.3, -0.2]].map(([yaw, pitch]) => {
  const project = rotate(yaw, pitch);
  const live = theo.model.slice(0, 68).map(project);
  const result = geo.liveHead(theo.model, live);
  const outline = (i) => i >= 78 && i < 78 + geo.ringCount(theo.model.length);
  const expected = (i) => (outline(i) ? geo.projectOutline(result.pose, theo.model[i]) : project(theo.model[i]));
  const error = Math.max(...result.points.map((point, i) => Math.hypot(point[0] - expected(i)[0], point[1] - expected(i)[1])));
  const ring = result.points.filter((_, i) => outline(i));
  const width = (Math.max(...ring.map((p) => p[0])) - Math.min(...ring.map((p) => p[0]))) / result.pose.scale;
  return { yaw: result.pose.yaw, pitch: result.pose.pitch, scale: result.pose.scale, error, width };
});
const template = faces.faceTemplate();
const anchors = { eyeL: [300, 200], eyeR: [380, 210], mouth: [336, 300] };
const fitted = geo.fitTemplate(template, anchors);
const frame = geo.faceFrame(fitted);
const opened = geo.fitTemplate(template, { ...anchors, open: 1 });
const turned = geo.fitTemplate(template, { ...anchors, yaw: 0.5 });
out.fit = {
  left: Math.hypot(frame.left[0] - 300, frame.left[1] - 200),
  right: Math.hypot(frame.right[0] - 380, frame.right[1] - 210),
  mouth: Math.hypot((fitted[48][0] + fitted[54][0]) / 2 - 336, (fitted[48][1] + fitted[54][1]) / 2 - 300),
  closedGap: geo.mouthGap(fitted),
  openGap: geo.mouthGap(opened),
  chinDrop: opened[8][1] - fitted[8][1],
  noseShift: turned[30][0] - fitted[30][0],
};

const size = 128;
const data = new Uint8ClampedArray(size * size * 4);
const paint = (test, rgb) => {
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) if (test(x, y)) data.set([...rgb, 255], (y * size + x) * 4);
  }
};
paint(() => true, [205, 150, 120]);
for (const cx of [44, 86]) {
  paint((x, y) => ((x - cx) / 9) ** 2 + ((y - 45) / 4) ** 2 <= 1, [235, 232, 228]);
  paint((x, y) => (x - cx) ** 2 + (y - 45) ** 2 <= 9, [35, 25, 20]);
}
paint((x, y) => Math.abs(y - 90) <= 4 && x >= 49 && x <= 81, [190, 70, 80]);
paint((x, y) => Math.abs(y - 90) <= 0 && x >= 50 && x <= 80, [60, 20, 25]);
const map = tracker.eyeMap({ width: size, height: size, data });
out.eye = tracker.refineEyeCenter(map.y, size, size, [47, 48], 6);
out.mouthFound = tracker.findMouth(map, size, size, [44, 45], [86, 45]);
delete out.mouthFound?.samples;
delete out.mouthFound?.skin;

const skin = tracker.createSkinModel();
tracker.learnSkin(skin, Array.from({ length: 40 }, (_, i) => [100 + (i % 5), 155 + (i % 7)]));
out.skin = {
  ready: skin.ready,
  match: tracker.isSkin(skin, 140, 102, 158),
  blue: tracker.isSkin(skin, 140, 150, 110),
};

const filter = new geo.OneEuroFilter(1, 0);
filter.filter(0, 0);
const moved = filter.filter(10, 40);
out.euro = { moved, settled: Array.from({ length: 60 }, (_, i) => filter.filter(10, 80 + i * 40)).pop() };
out.size = faces.outputSize(1920, 1080);
console.log(JSON.stringify(out));
'''

CALLS_HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createChatCalls } = await import(pathToFileURL(process.argv[2]).href);

class FakeStream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(track) { this.tracks.push(track); }
}
globalThis.MediaStream = FakeStream;
const track = (kind, id) => ({ kind, id, enabled: true, stopped: false,
  stop() { this.stopped = true; } });

function element(id) {
  const listeners = {};
  const attributes = {};
  return {
    id, hidden: true, textContent: "", value: "", options: [], children: [], srcObject: null, style: {},
    classList: { toggle() {} },
    setAttribute(name, value) { attributes[name] = String(value); },
    getAttribute(name) { return attributes[name] ?? null; },
    addEventListener(type, fn) { listeners[type] = fn; },
    append(...items) { this.options.push(...items); this.children.push(...items); },
    querySelector() { return null; },
    querySelectorAll() { return this.children; },
    fire(type) { return listeners[type]?.(); },
    click() { return listeners.click?.(); },
  };
}
const elements = {};
const documentRef = {
  querySelector(selector) {
    const id = selector.replace(/^#/, "");
    elements[id] ||= element(id);
    return elements[id];
  },
  createElement(tag) { return element(tag); },
};

const camera = track("video", "camera");
const mic = track("audio", "mic");
const stages = [];
function createFaceStageImpl({ cameraStream, faceId }) {
  const stage = {
    track: track("video", "stage-" + stages.length),
    current: faceId,
    stopped: false,
    source: cameraStream,
    face() { return this.current; },
    setFace(id) { this.current = id; },
    stop() { this.stopped = true; },
  };
  stages.push(stage);
  return stage;
}

class FakePeer {
  constructor() {
    this.transceivers = [];
    this.localDescription = { type: "offer", sdp: "x" };
  }
  addTransceiver(trackOrKind) {
    const kind = typeof trackOrKind === "string" ? trackOrKind : trackOrKind.kind;
    const sender = {
      track: typeof trackOrKind === "string" ? null : trackOrKind,
      async replaceTrack(next) { this.track = next; },
    };
    this.transceivers.push({ sender, receiver: { track: { kind } } });
  }
  getTransceivers() { return this.transceivers; }
  async setLocalDescription() {}
  close() { this.closed = true; }
}

const stored = new Map([["forkmesh.chat.callFace", "kofi"]]);
const storage = {
  getItem: (key) => stored.get(key) ?? null,
  setItem: (key, value) => stored.set(key, value),
  removeItem: (key) => stored.delete(key),
};

const calls = createChatCalls({
  selfId: "me",
  sendSignal: async () => true,
  getUserMediaImpl: async () => new FakeStream([mic, camera]),
  RTCPeerConnectionImpl: FakePeer,
  documentRef,
  storage,
  facesSupported: () => true,
  createFaceStageImpl,
  setTimeoutImpl: () => 1,
  clearTimeoutImpl: () => {},
  setIntervalImpl: () => 1,
  clearIntervalImpl: () => {},
});

const out = {};
out.options = elements["chat-call-face"].options.map((o) => o.value);
await calls.start("dm:bob", "bob", "video");
out.savedFace = calls.face();
out.pickerShown = !elements["chat-call-face-row"].hidden;
out.previewIsStage = elements["chat-call-local"].srcObject.getTracks()[0] === stages[0].track;
out.stageSeesMic = stages[0].source.getAudioTracks()[0] === mic;
elements["chat-call-face"].value = "ren";
elements["chat-call-face"].fire("change");
out.switched = calls.face();
out.reusedStage = stages.length === 1;
out.remembered = stored.get("forkmesh.chat.callFace");
elements["chat-call-face"].value = "";
elements["chat-call-face"].fire("change");
out.cleared = calls.face();
out.stageStopped = stages[0].stopped;
out.previewIsCamera = elements["chat-call-local"].srcObject.getTracks()[0] === camera;
out.forgotten = stored.has("forkmesh.chat.callFace");
calls.setFace("maya");
out.secondStage = stages.length === 2 && calls.face() === "maya";
calls.hangUp();
out.stoppedOnHangup = stages[1].stopped && camera.stopped;
console.log(JSON.stringify(out));
'''

SENDER_HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createChatCalls } = await import(pathToFileURL(process.argv[2]).href);
class FakeStream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(track) { this.tracks.push(track); }
}
globalThis.MediaStream = FakeStream;
const track = (kind, id) => ({ kind, id, enabled: true, stop() {} });
const documentRef = {
  querySelector: () => null,
  createElement: () => ({}),
};
const camera = track("video", "camera");
const stage = { track: track("video", "stage"), id: "theo",
  face() { return this.id; }, setFace(id) { this.id = id; }, stop() {} };
let pc = null;
class FakePeer {
  constructor() { this.transceivers = []; pc = this; }
  addTransceiver(value) {
    const kind = typeof value === "string" ? value : value.kind;
    this.transceivers.push({
      sender: { track: typeof value === "string" ? null : value,
        async replaceTrack(next) { this.track = next; } },
      receiver: { track: { kind } },
    });
  }
  getTransceivers() { return this.transceivers; }
  async setLocalDescription() { this.localDescription = {}; }
  close() {}
}
const sent = [];
const calls = createChatCalls({
  selfId: "me",
  sendSignal: async (channel, type, extra) => { sent.push({ type, ...extra }); return true; },
  getUserMediaImpl: async () => new FakeStream([track("audio", "mic"), camera]),
  RTCPeerConnectionImpl: FakePeer,
  documentRef,
  storage: { getItem: () => "theo", setItem() {}, removeItem() {} },
  facesSupported: () => true,
  createFaceStageImpl: () => stage,
  setTimeoutImpl: () => 1, clearTimeoutImpl() {},
  setIntervalImpl: () => 1, clearIntervalImpl() {},
});
await calls.start("dm:bob", "bob", "video");
const callId = sent.find((s) => s.type === "call-invite").call;
await calls.handleFrame({ type: "call-accept", call: callId, senderId: "peer", to: "me" }, "dm:bob");
const video = () => pc.transceivers.find((t) => t.receiver.track.kind === "video").sender.track.id;
const out = { offered: video() };
calls.setFace("");
await new Promise((r) => setImmediate(r));
out.afterClear = video();
console.log(JSON.stringify(out));
'''

def _node(harness, module):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / module)],
        input=harness, capture_output=True, text=True, timeout=60, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])

def _faces():
    return _node(FACES_HARNESS, "chat-call-faces.js")


def test_there_are_five_distinct_photo_faces_with_landmarks():
    data = _faces()
    ids = [face["id"] for face in data["faces"]]
    assert ids == ["maya", "theo", "kofi", "ingrid", "ren"]
    assert len({face["label"] for face in data["faces"]}) == 5
    for face in data["faces"]:
        assert face["count"] == 68
        assert face["inside"] is True
        assert 90 < face["span"] < 160
        photo = PUBLIC / "assets" / "call-faces" / f"{face['id']}.jpg"
        assert photo.read_bytes()[:3] == b"\xff\xd8\xff"
    assert data["unknown"] is None


def test_vision_regions_become_the_same_68_points_as_the_photos():
    data = _faces()
    for errors in data["vision"]:
        assert errors["jaw"] < 0.06
        assert errors["eyes"] < 0.05
        assert errors["brows"] < 0.08
        assert errors["lips"] < 0.12
    assert data["missingRegions"] is None


def test_head_mesh_covers_hair_and_leaves_the_mouth_open():
    for mesh in _faces()["mesh"]:
        assert mesh["triangles"] > 150
        assert mesh["valid"] is True
        assert mesh["vertices"] == 68 + 10 + 2 * 28 + mesh["extras"]
        assert mesh["extras"] >= 40
        assert mesh["ring"] == 28
        assert mesh["ringInside"] is True
        assert mesh["photoArea"] > 0.45 * 512 * 512
        assert mesh["mouth"] >= 4
        assert mesh["mouthOnlyInnerLips"] is True
        assert mesh["flipped"] == 0


def test_pose_solver_recovers_turns_and_places_the_hair_ring():
    cases = _faces()["pose"]
    for case, (yaw, pitch) in zip(cases, [(0, 0), (0.4, 0), (0, 0.3), (-0.3, -0.2)]):
        assert abs(case["scale"] - 60) < 0.5
        assert abs(abs(case["yaw"]) - abs(yaw)) < 0.03
        assert abs(abs(case["pitch"]) - abs(pitch)) < 0.03
        assert case["error"] < 0.05
        assert case["width"] == pytest.approx(cases[0]["width"], rel=0.01)


def test_template_fit_puts_the_face_on_the_tracked_eyes_and_mouth():
    fit = _faces()["fit"]
    assert fit["left"] < 0.5
    assert fit["right"] < 0.5
    assert fit["mouth"] < 3
    assert abs(fit["closedGap"]) < 0.02
    assert fit["openGap"] > 0.3
    assert fit["chinDrop"] > 20
    assert fit["noseShift"] > 5


def test_tracker_finds_eye_centres_and_the_mouth_line():
    data = _faces()
    assert abs(data["eye"][0] - 44) <= 2 and abs(data["eye"][1] - 45) <= 2
    mouth = data["mouthFound"]
    assert mouth is not None
    assert abs(mouth["center"][1] - 90) <= 4
    assert abs(mouth["center"][0] - 65) <= 6
    assert mouth["evidence"] > 0.5


def test_skin_model_learns_the_callers_skin():
    skin = _faces()["skin"]
    assert skin == {"ready": True, "match": True, "blue": False}


def test_landmark_smoothing_settles_on_the_new_position():
    data = _faces()
    assert 0 < data["euro"]["moved"] < 10
    assert abs(data["euro"]["settled"] - 10) < 0.05
    assert data["size"] == [960, 540]


def test_call_face_picker_swaps_the_stage_and_remembers_the_choice():
    data = _node(CALLS_HARNESS, "chat-calls.js")
    assert data["options"] == ["", "maya", "theo", "kofi", "ingrid", "ren", "custom"]
    assert data["savedFace"] == "kofi"
    assert data["pickerShown"] is True
    assert data["previewIsStage"] is True
    assert data["stageSeesMic"] is True
    assert data["switched"] == "ren"
    assert data["reusedStage"] is True
    assert data["remembered"] == "ren"
    assert data["cleared"] == ""
    assert data["stageStopped"] is True
    assert data["previewIsCamera"] is True
    assert data["forgotten"] is False
    assert data["secondStage"] is True
    assert data["stoppedOnHangup"] is True

def test_the_peer_receives_the_disguised_track_until_the_face_is_cleared():
    data = _node(SENDER_HARNESS, "chat-calls.js")
    assert data == {"offered": "stage", "afterClear": "camera"}

def test_call_panel_has_a_face_picker():
    assert 'id="chat-call-face-row"' in HTML
    assert 'id="chat-call-face"' in HTML
    assert ".chat-call-face[hidden] { display: none; }" in CSS
    assert 'from "./chat-call-faces.js"' in CALLS

def test_call_query_asks_before_ringing():
    assert "offerRequestedCall(requestedDirect);" in CHAT
    start = CHAT.index("function offerRequestedCall(")
    body = CHAT[start:CHAT.index("\nfunction ", start + 1)]
    assert 'chatQuery.get("call")' in body
    assert "globalThis.confirm(" in body
    assert "callsSupported()" in body

def test_desktop_direct_message_header_offers_calls():
    header = (DESKTOP / "MainWindowIssues.cpp").read_text(encoding="utf-8")
    messages = (DESKTOP / "MainWindowMessages.cpp").read_text(encoding="utf-8")
    assert "m_callAudioButton = makeCallButton(" in header
    assert "m_callVideoButton = makeCallButton(" in header
    start = messages.index("void MainWindow::startDirectCallOnWeb(")
    body = messages[start:messages.index("\n}\n", start)]
    assert 'QStringLiteral("/chat")' in body
    assert 'QStringLiteral("dm")' in body
    assert 'QStringLiteral("call")' in body
    assert "m_callVideoButton->setVisible(directCall);" in messages


NATIVE = ROOT.parent / "native"
IOS_PAGE = NATIVE / "ios" / "ForkMesh" / "Features" / "Chat" / "Calls" / "direct-call.html"
ANDROID_PAGE = NATIVE / "android" / "app" / "src" / "main" / "assets" / "direct-call.html"


def test_native_call_pages_inline_the_current_faces_module():
    result = subprocess.run(
        ["python3", str(ROOT / "tools" / "sync_native_call_faces.py"), "--check"],
        capture_output=True, text=True, timeout=60, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert IOS_PAGE.read_text(encoding="utf-8") == ANDROID_PAGE.read_text(encoding="utf-8")


def test_native_call_page_exposes_face_operations():
    page = IOS_PAGE.read_text(encoding="utf-8")
    assert "window.FmCallFaces = (() => {" in page
    assert "async setup(media, iceServers, face) {" in page
    assert "async face(id, image) {" in page
    assert "async faceImage(image) {" in page
    assert "async faceState() {" in page
    assert "async faceList() {" in page
    assert "faces.callFaceChoices()" in page
    assert "videoSender.replaceTrack(track)" in page
    assert "if (stage) stage.stop();" in page
    assert "img-src data:;" in page
    assert '<script id="fm-call-face-photos">' in page
    for face_id in ("maya", "theo", "kofi", "ingrid", "ren"):
        assert f'"{face_id}": "data:image/jpeg;base64,' in page
    assert "window.FmCallFaceHost = {" in page
    assert 'post({ event: "landmarks", id, image, width, height, landmarks: wanted.landmarks !== false, mask: Boolean(wanted.mask), accurate: Boolean(wanted.accurate) });' in page
    assert "async background(id, image) {" in page
    assert "async backgroundList() {" in page
    assert '<script id="fm-call-models">' in page
    assert '<script id="fm-call-backgrounds">' in page
    for name in ("landscape", "square", "detector", "mesh", "hair"):
        assert f'  "{name}": "' in page
    for face_id in ("maya", "theo", "kofi", "ingrid", "ren"):
        assert f'"{face_id}-matte": "data:image/png;base64,' in page
    assert "window.fmCallLandmarks = (id, regions) => {" in page
    assert "window.FmCallDepthCamera && window.FmCallDepthCamera.available" in page
    assert "window.fmCallFrame = (payload) => {" in page
    assert "owner.pushFrame(image, { regions: payload.regions || null, mask: payload.mask || null }, performance.now())" in page
    assert 'const frameDone = () => post({ event: "frame-done", ...frameWant() });' in page
    assert 'post({ event: "camera", id, op, ...frameWant() });' in page
    assert "window.fmCallCamera = (id, reply) => {" in page
    assert "window.fmCallCameraFailed = () => {" in page
    assert "faces.createFaceStage({ cameraStream: null, manual: true," in page
    assert "navigator.mediaDevices.getUserMedia({ audio: true, video: false })" in page
    assert "wait(DEPTH_FRAME_MS)" in page
    assert "const DEPTH_FRAME_MS = 1500;" in page
    assert "navigator.mediaDevices.getUserMedia({ video: true })" in page
    assert "stream = previewed || await openStream(media);" in page
    assert "if (!stream) stream = await openStream(media);" in page
    assert "stopPushed();\n      if (stage) stage.stop();" in page


def test_ios_wires_the_call_engine_screen_and_buttons():
    app = NATIVE / "ios" / "ForkMesh"
    install = (app / "App" / "AppServices+Chat.swift").read_text(encoding="utf-8")
    assert "installCalls(hub: hub, direct: direct, relay: relay, auth: auth)" in install
    assert "calls.peerFactory = WebCallPeerFactory()" in install
    assert "direct.onCallFrame = {" in install
    assert "hub.calls = calls" in install
    header = (app / "Features" / "Chat" / "Messages" / "MessagesConversationView.swift").read_text(encoding="utf-8")
    assert "DirectCallHeaderButtons(hub: hub, dark: palette.dark)" in header
    screen = (app / "Features" / "Chat" / "ChatScreen.swift").read_text(encoding="utf-8")
    assert ".directCalls(hub)" in screen
    views = (app / "Features" / "Chat" / "Calls" / "DirectCallViews.swift").read_text(encoding="utf-8")
    assert "engine.setFace($0)" in views
    assert "WebCallPeerFactory.requestAccess(for:" in views
    peer = (app / "Features" / "Chat" / "Calls" / "WebCallPeer.swift").read_text(encoding="utf-8")
    assert 'configuration.userContentController.add(proxy, name: "fmCall")' in peer
    assert 'run("face", [.string(id)])' in peer
    assert 'case "landmarks":' in peer
    assert "await CallFaceLandmarks.detect(dataURL: image, landmarks: landmarks, mask: mask, accurate: accurate)" in peer
    assert 'run("background", [.string(id), .string(image)])' in peer
    assert "if let previewed = current, previewed.previewing, media == .video {" in peer
    assert "window.fmCallLandmarks && window.fmCallLandmarks(" in peer
    vision = (app / "Features" / "Chat" / "Calls" / "CallFaceLandmarks.swift").read_text(encoding="utf-8")
    assert "VNDetectFaceLandmarksRequest()" in vision
    assert "faceRequest.constellation = .constellation76Points" in vision
    assert "region.pointsInImage(imageSize: size)" in vision
    assert "size.height - point.y" in vision
    assert "VNGeneratePersonSegmentationRequest()" in vision
    assert "personRequest.qualityLevel = accurate ? .accurate : .balanced" in vision
    assert "static func analyze(image: CGImage, landmarks wantLandmarks: Bool" in vision
    assert 'source: "window.FmCallDepthCamera = { available: true };", injectionTime: .atDocumentStart' in peer
    assert 'case "camera":' in peer
    assert 'case "frame-done":' in peer
    assert "window.fmCallFrame && window.fmCallFrame(" in peer
    assert "window.fmCallCamera && window.fmCallCamera(" in peer
    assert "window.fmCallCameraFailed && window.fmCallCameraFailed(" in peer
    assert "depthCamera?.stop()" in peer
    depth = (app / "Features" / "Chat" / "Calls" / "CallDepthCamera.swift").read_text(encoding="utf-8")
    assert "AVCaptureDevice.default(.builtInTrueDepthCamera, for: .video, position: .front)" in depth
    assert "AVCaptureDataOutputSynchronizer(dataOutputs: [video, depth])" in depth
    assert "depth.isFilteringEnabled = true" in depth
    assert "video.alwaysDiscardsLateVideoFrames = true" in depth
    assert "kCVPixelFormatType_DepthFloat32" in depth
    assert "CallFaceLandmarks.analyze(image: image, landmarks: landmarks, mask: mask, accurate: false)" in depth
    assert "CallFaceDepth.relief(regions, field: field" in depth
    assert "calibration.intrinsicMatrixReferenceDimensions.width" in depth
    assert "device.activeFormat.videoFieldOfView" in depth
    assert "session.automaticallyConfiguresApplicationAudioSession = false" in depth
    core = NATIVE / "ios" / "ForkMeshCore" / "Sources" / "FMChat" / "Direct" / "CallFaceDepth.swift"
    assert "(faceDepth - depth) * focal / faceDepth" in core.read_text(encoding="utf-8")
    preview = (app / "Features" / "Chat" / "Calls" / "DirectCallPreview.swift").read_text(encoding="utf-8")
    assert "face: engine.face, background: engine.background, image: image, faceImage: faceImage)" in preview
    assert "PhotosPicker(selection: $photo, matching: .images)" in preview
    assert "DirectCallPreviewButton(hub: hub, engine: engine, dark: dark)" in views
    assert "calls.onBackgroundChanged = {" in install


def test_android_wires_the_call_engine_screen_and_buttons():
    chat = NATIVE / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "feature"
    assert "DirectCallButtons(conversation)" in (chat / "chat" / "ChatScreen.kt").read_text(encoding="utf-8")
    assert "DirectCallOverlay()" in (chat / "Features.kt").read_text(encoding="utf-8")
    assert "chat.calls = DirectCallFeature.install(" in (chat / "chat" / "ChatDepth.kt").read_text(encoding="utf-8")
    peer = (chat / "chat" / "WebCallPeer.kt").read_text(encoding="utf-8")
    assert 'const val BASE_URL = "https://$ORIGIN_HOST/"' in peer
    assert "settings.allowFileAccess = false" in peer
    assert "request.origin?.host == ORIGIN_HOST" in peer
    core = NATIVE / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "chat" / "DirectCalls.kt"
    faces = core.read_text(encoding="utf-8")
    for face_id in ("maya", "theo", "kofi", "ingrid", "ren"):
        assert f'DirectCallFace("{face_id}"' in faces

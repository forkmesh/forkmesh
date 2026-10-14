import json
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
MODELS = PUBLIC / "assets" / "call-segmentation"
BACKGROUNDS = PUBLIC / "assets" / "call-backgrounds"
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
SUPPORTED = {"conv", "dw", "tconv", "add", "mul", "mean", "maxpool", "argmax", "unpool", "concat", "pad", "resize", "act"}

MODULES_HARNESS = r'''
import { pathToFileURL } from "node:url";
import { readFileSync } from "node:fs";
const base = process.argv[2];
const load = (name) => import(pathToFileURL(base + "/" + name).href);
const seg = await load("chat-call-segment.js");
const mesh = await load("chat-call-face-mesh.js");
const render = await load("chat-call-face-render.js");
const bg = await load("chat-call-backgrounds.js");
const out = { models: {} };
for (const [name, file] of Object.entries(seg.SEGMENT_MODELS)) {
  const model = seg.parseSegmentModel(readFileSync(base + "/assets/call-segmentation/" + file));
  const { layers, shapes } = seg.expandLayers(model);
  let shaders = 0;
  for (const layer of layers) {
    const second = layer.in[1] !== undefined ? shapes[layer.in[1]] : null;
    const packed = layer.w ? seg.packWeights(layer, model.weights) : null;
    if (packed && packed.data.length !== packed.width * packed.height * 4) throw new Error("bad packing");
    seg.layerShader(layer, shapes[layer.in[0]], shapes[layer.out], second, packed?.alphaRow || 0);
    shaders += 1;
  }
  out.models[name] = { input: shapes[model.input], outputs: model.outputs.map((group) => group.map((i) => shapes[i])), layers: shaders };
}
out.half = [0, 1, -2.5, 65504, 0.0001].map((v) => seg.halfToFloat(seg.floatToHalf(v)));
out.anchors = mesh.blazeAnchors().length;
out.meshIndices = Object.values(mesh.MESH_REGIONS).flat().every((i) => i >= 0 && i < 468);
const scores = new Float32Array(896).fill(-10);
const boxes = new Float32Array(896 * 16);
scores[100] = 5;
boxes.set([4, -2, 40, 40, -10, -8, 10, -8, 0, 4, 0, 14, -30, -4, 30, -4], 100 * 16);
const found = mesh.decodeDetections(scores, boxes, mesh.blazeAnchors());
out.detection = { cx: found.cx, cy: found.cy, w: found.w, score: found.score };
const roi = mesh.roiFromDetection(found, { scale: 640, x: 0, y: 0 });
out.roi = roi;
out.cover = [render.coverRect(1920, 1080, 640, 360), render.coverRect(1920, 1080, 360, 640)];
const pose = { scale: 50, rows: [[1, 0, 0], [0, 1, 0], [0, 0, 1]], t: [320, 180], roll: 0 };
out.hair = [render.hairRegion(pose, 640, 360), render.hairRegion({ ...pose, t: [20, 20] }, 640, 360)];
out.backgrounds = bg.CALL_BACKGROUNDS.map((b) => [b.id, b.mode, Boolean(b.custom)]);
const stored = new Map();
const storage = { getItem: (k) => stored.get(k) ?? null, setItem: (k, v) => stored.set(k, v), removeItem: (k) => stored.delete(k) };
out.saved = [];
out.saved.push(bg.savedCallBackground(storage));
out.saved.push(bg.rememberCallBackground("custom", "", storage));
bg.rememberCallBackground("world", "", storage);
out.saved.push(bg.savedCallBackground(storage));
bg.rememberCallBackground("custom", "data:image/jpeg;base64,AAAA", storage);
out.saved.push(bg.savedCallBackground(storage));
bg.rememberCallBackground("", "", storage);
out.saved.push(bg.savedCallBackground(storage));
out.fit = [bg.fitSide(4000, 3000), bg.fitSide(800, 600)];
const office = bg.callBackgroundById("office");
out.urls = [bg.callBackgroundImageUrl(office), bg.callBackgroundImageUrl(office, true), bg.callBackgroundImageUrl(bg.callBackgroundById("blur"), true)];
out.portrait = [bg.isPortraitFrame(360, 640), bg.isPortraitFrame(640, 360), bg.isPortraitFrame(480, 480), bg.isPortraitFrame(0, 0)];
console.log(JSON.stringify(out));
'''

PREVIEW_HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createChatCalls } = await import(pathToFileURL(process.argv[2]).href);
class FakeStream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
}
globalThis.MediaStream = FakeStream;
const track = (kind, id) => ({ kind, id, enabled: true, stopped: false, stop() { this.stopped = true; } });
function element(id) {
  const listeners = {};
  const attributes = {};
  return {
    id, hidden: true, textContent: "", value: "", options: [], children: [], srcObject: null, style: {},
    classList: { on: new Set(), toggle(name, on) { if (on) this.on.add(name); else this.on.delete(name); } },
    setAttribute(name, value) { attributes[name] = String(value); },
    getAttribute(name) { return attributes[name] ?? null; },
    addEventListener(type, fn) { listeners[type] = fn; },
    append(...items) { this.options.push(...items); this.children.push(...items); },
    remove() {},
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
let mediaCalls = 0;
const camera = track("video", "camera");
const mic = track("audio", "mic");
const stages = [];
function createFaceStageImpl({ faceId, backgroundId }) {
  const stage = {
    track: track("video", "stage-" + stages.length), current: faceId, scene: backgroundId, stopped: false,
    face() { return this.current; }, setFace(id) { this.current = id; },
    background() { return this.scene; }, setBackground(id) { this.scene = id; },
    stop() { this.stopped = true; },
  };
  stages.push(stage);
  return stage;
}
const previews = [];
async function createVoicePreviewImpl({ micStream }) {
  const preview = { stream: micStream, stopped: null, supported: false, headphones: false,
    choice: () => "face", voice: () => "", monitoring: () => false, setFace() {}, setChoice() {}, monitor() { return false; },
    stop(options) { this.stopped = options || {}; } };
  previews.push(preview);
  return preview;
}
const sent = [];
const stored = new Map([["forkmesh.chat.callFace", "theo"], ["forkmesh.chat.callBackground", "blur"]]);
const storage = { getItem: (k) => stored.get(k) ?? null, setItem: (k, v) => stored.set(k, v), removeItem: (k) => stored.delete(k) };
class FakePeer {
  constructor() { this.transceivers = []; }
  addTransceiver(value) {
    const kind = typeof value === "string" ? value : value.kind;
    this.transceivers.push({ sender: { track: typeof value === "string" ? null : value, async replaceTrack(next) { this.track = next; } }, receiver: { track: { kind } } });
  }
  getTransceivers() { return this.transceivers; }
  async setLocalDescription() { this.localDescription = {}; }
  close() {}
}
const calls = createChatCalls({
  selfId: "me",
  sendSignal: async (channel, type) => { sent.push(type); return true; },
  getUserMediaImpl: async () => { mediaCalls += 1; return new FakeStream([mic, camera]); },
  RTCPeerConnectionImpl: FakePeer,
  documentRef, storage,
  facesSupported: () => true,
  backgroundsSupported: () => true,
  voicesSupported: () => false,
  createFaceStageImpl, createVoicePreviewImpl,
  setTimeoutImpl: () => 1, clearTimeoutImpl() {}, setIntervalImpl: () => 1, clearIntervalImpl() {},
});
const out = {};
out.buttons = elements["chat-call-background-options"].children.map((b) => b.getAttribute("data-background"));
await calls.preview("dm:bob", "bob");
await new Promise((r) => setImmediate(r));
out.previewing = calls.previewing();
out.stageFace = stages[0]?.face();
out.stageBackground = stages[0]?.background();
out.startShown = !elements["chat-call-start"].hidden;
out.previewSlotShown = !elements["chat-call-preview-slot"].hidden;
out.previewClass = elements["chat-call-panel"].classList.on.has("is-preview");
out.localIsStage = elements["chat-call-local"].srcObject.getTracks()[0] === stages[0].track;
out.voicePreviewMic = previews[0]?.stream.getAudioTracks()[0] === mic;
calls.setFace("");
out.keptForBackground = stages.length === 1 && !stages[0].stopped;
calls.setBackground("world");
out.background = stages[0].background();
out.rememberedBackground = stored.get("forkmesh.chat.callBackground");
out.signalsBeforeStart = sent.length;
elements["chat-call-start"].click();
await new Promise((r) => setImmediate(r));
out.afterStart = { previewing: calls.previewing(), active: calls.active(), sameStage: stages.length === 1 && !stages[0].stopped,
  mediaCalls, invite: sent.includes("call-invite"), voicePreviewStopped: previews[0]?.stopped?.keepStream === true,
  startHidden: elements["chat-call-start"].hidden };
calls.setBackground("");
out.clearedBackground = stages[0].stopped;
calls.hangUp();
out.hungUp = !calls.active() && camera.stopped;
await calls.preview("dm:bob", "bob");
calls.hangUp();
out.closedPreviewSignals = sent.filter((type) => type === "call-bye").length;
console.log(JSON.stringify(out));
'''


def _node(harness, *args):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", *map(str, args)],
        input=harness, capture_output=True, text=True, timeout=120, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


def _header(path):
    data = path.read_bytes()
    assert data[:4] == b"FMNN"
    length = struct.unpack_from("<I", data, 8)[0]
    return json.loads(data[12:12 + length]), len(data) - 12 - length


def test_bundled_models_use_only_ops_the_webgl_runner_implements():
    for name, expected in {
        "selfie-landscape.fmnn": [144, 256, 3],
        "selfie-square.fmnn": [256, 256, 3],
        "face-detector.fmnn": [128, 128, 3],
        "face-mesh.fmnn": [192, 192, 3],
        "hair-192.fmnn": [192, 192, 4],
    }.items():
        header, blob = _header(MODELS / name)
        assert header["shapes"][str(header["input"])] == expected
        assert {op["op"] for op in header["ops"]} <= SUPPORTED
        assert blob % 2 == 0
        for op in header["ops"]:
            for key in ("w", "b", "alpha"):
                if key in op:
                    assert op[key][0] + op[key][1] <= blob // 2
    total = sum(path.stat().st_size for path in MODELS.glob("*.fmnn"))
    assert total < 2_500_000
    assert "Apache License" in (MODELS / "LICENSE").read_text(encoding="utf-8")


def test_model_runner_builds_every_layer_and_round_trips_half_floats():
    data = _node(MODULES_HARNESS, PUBLIC)
    assert data["models"]["landscape"]["outputs"] == [[[144, 256, 1]]]
    assert data["models"]["square"]["outputs"] == [[[256, 256, 1]]]
    assert data["models"]["mesh"]["outputs"] == [[[1, 1, 1404]], [[1, 1, 1]]]
    assert data["models"]["detector"]["outputs"] == [[[16, 16, 32], [8, 8, 96]], [[16, 16, 2], [8, 8, 6]]]
    assert data["models"]["hair"]["outputs"] == [[[192, 192, 2]]]
    assert all(model["layers"] > 30 for model in data["models"].values())
    assert data["half"][:4] == [0, 1, -2.5, 65504]
    assert abs(data["half"][4] - 0.0001) < 1e-6


def test_face_detector_decoding_and_mesh_regions():
    data = _node(MODULES_HARNESS, PUBLIC)
    assert data["anchors"] == 896
    assert data["meshIndices"] is True
    detection = data["detection"]
    assert detection["score"] > 0.99
    assert abs(detection["w"] - 40 / 128) < 1e-6
    roi = data["roi"]
    assert roi["angle"] == pytest.approx(0)
    assert roi["size"] == pytest.approx(40 / 128 * 640 * 1.5)


def test_background_fitting_and_hair_crop_stay_inside_the_frame():
    data = _node(MODULES_HARNESS, PUBLIC)
    assert data["cover"][0] == [0, 0, 1, 1]
    span = (360 / 640) / (1920 / 1080)
    assert data["cover"][1] == pytest.approx([(1 - span) / 2, 0, span, 1])
    centered, corner = data["hair"]
    assert centered["size"] == [320, 320]
    assert corner["center"] == [160, 160]


def test_background_choices_persist_and_custom_needs_a_photo():
    data = _node(MODULES_HARNESS, PUBLIC)
    ids = [entry[0] for entry in data["backgrounds"]]
    assert ids == ["", "blur", "blur-strong", "world", "office", "meeting", "library", "skyline", "custom"]
    assert data["saved"][0] == {"id": "", "image": ""}
    assert data["saved"][1] is False
    assert data["saved"][2] == {"id": "world", "image": ""}
    assert data["saved"][3] == {"id": "custom", "image": "data:image/jpeg;base64,AAAA"}
    assert data["saved"][4] == {"id": "", "image": ""}
    assert data["fit"] == [[1920, 1440], [800, 600]]


def test_portrait_frames_use_the_portrait_render():
    data = _node(MODULES_HARNESS, PUBLIC)
    assert data["urls"] == ["/assets/call-backgrounds/office.jpg", "/assets/call-backgrounds/office-portrait.jpg", ""]
    assert data["portrait"] == [True, False, False, False]
    faces = (PUBLIC / "chat-call-faces.js").read_text(encoding="utf-8")
    assert "callBackgroundImageUrl(next, portrait)" in faces
    assert 'if (background.mode === "image" && !background.custom) loadBackground(background.id, \"\", true);' in faces
    assert "if (!keep) renderer.setBackground({ mode: \"off\" });" in faces


def test_built_in_backgrounds_are_bundled_with_thumbnails():
    for stem in ("world", "office", "meeting", "library", "skyline"):
        full = (BACKGROUNDS / f"{stem}.jpg").read_bytes()
        thumb = (BACKGROUNDS / f"{stem}-thumb.jpg").read_bytes()
        portrait = (BACKGROUNDS / f"{stem}-portrait.jpg").read_bytes()
        assert full[:3] == b"\xff\xd8\xff" and thumb[:3] == b"\xff\xd8\xff" and portrait[:3] == b"\xff\xd8\xff"
        height, width = _jpeg_size(full)
        assert (width, height) == (1920, 1080)
        height, width = _jpeg_size(portrait)
        assert (width, height) == (720, 1280)
    total = sum(path.stat().st_size for path in BACKGROUNDS.glob("*.jpg"))
    assert total < 1_500_000


def _jpeg_size(data):
    at = 2
    while at < len(data):
        marker = data[at + 1]
        length = struct.unpack(">H", data[at + 2:at + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            return struct.unpack(">HH", data[at + 5:at + 9])
        at += 2 + length
    return (0, 0)


def test_preview_runs_the_call_pipeline_and_start_keeps_it():
    data = _node(PREVIEW_HARNESS, PUBLIC / "chat-calls.js")
    assert data["buttons"] == ["", "blur", "blur-strong", "world", "office", "meeting", "library", "skyline", "custom"]
    assert data["previewing"] is True
    assert data["stageFace"] == "theo"
    assert data["stageBackground"] == "blur"
    assert data["startShown"] is True
    assert data["previewSlotShown"] is True
    assert data["previewClass"] is True
    assert data["localIsStage"] is True
    assert data["voicePreviewMic"] is True
    assert data["keptForBackground"] is True
    assert data["background"] == "world"
    assert data["rememberedBackground"] == "world"
    assert data["signalsBeforeStart"] == 0
    assert data["afterStart"] == {
        "previewing": False, "active": True, "sameStage": True, "mediaCalls": 1, "invite": True,
        "voicePreviewStopped": True, "startHidden": True,
    }
    assert data["clearedBackground"] is True
    assert data["hungUp"] is True
    assert data["closedPreviewSignals"] == 1


def test_chat_page_has_preview_and_background_picker():
    for marker in (
        'id="chat-call-preview"', 'id="chat-call-background-row"', 'id="chat-call-background-options"',
        'id="chat-call-background-file"', 'id="chat-call-preview-slot"', 'id="chat-call-start"',
    ):
        assert marker in HTML
    assert ".chat-call-backgrounds[hidden] { display: none; }" in CSS
    assert ".chat-call-panel.is-preview #chat-call-local" in CSS
    assert "callPreviewBtn?.addEventListener(\"click\", previewDirectCall);" in CHAT


def test_android_wires_backgrounds_and_the_preview():
    feature = ROOT.parent / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "feature" / "chat"
    views = (feature / "DirectCallViews.kt").read_text(encoding="utf-8")
    peer = (feature / "WebCallPeer.kt").read_text(encoding="utf-8")
    installer = (feature / "DirectCallFeature.kt").read_text(encoding="utf-8")
    assert "previewing != null -> DirectCallPreviewScreen(previewing, state)" in views
    assert "ActivityResultContracts.PickVisualMedia()" in views
    assert 'request("background", listOf(id, image))' in peer
    assert "if (previewed != null && video) {" in peer
    assert 'const val BACKGROUND_PREF = "chat.callBackground"' in installer
    core = (ROOT.parent / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "chat" / "DirectCalls.kt").read_text(encoding="utf-8")
    for background_id in ("blur", "blur-strong", "world", "office", "meeting", "library", "skyline", "custom"):
        assert f'DirectCallFace("{background_id}"' in core or (background_id == "custom" and "DirectCallFace(CUSTOM_BACKGROUND" in core)

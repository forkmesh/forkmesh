import functools
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
HANDS = PUBLIC / "assets" / "hand-tracking"
CONVERTER = (ROOT / "tools" / "build_call_segmentation_model.py").read_text(encoding="utf-8")
SEGMENT = (PUBLIC / "chat-call-segment.js").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[1];
const load = (name) => import(pathToFileURL(base + "/" + name).href);
const wire = await load("call-avatar-rig-wire.js");
const rig = await load("call-avatar-rig.js");
const hands = await load("chat-call-hand-tracker.js");
const speech = await load("call-speech-meter.js");
const puppet = await load("call-avatar-puppet.js");
const out = {};

const deltas = new Float32Array(136).map((_, i) => ((i % 9) - 4) / 32);
const full = { yaw: 0.3, pitch: -0.2, roll: 0.05, deltas, lidLeft: 1, lidRight: 0.2, gazeX: -0.4, gazeY: 0.1, mouth: 0.6, smile: 0.3, browLeft: -0.5, browRight: 0.5, round: 0.1, tongue: 0.7,
  hands: [{ side: "right", x: -3, y: 1.5, curl: [0, 0.5, 1, 1, 1], roll: 0.3 }], talking: true, level: 0.4, words: 321,
  anchors: Array.from({ length: 68 }, (_, i) => [i / 67, 1 - i / 67]) };
const encoded = wire.encodeRig(full);
const back = wire.decodeRig(JSON.parse(JSON.stringify(encoded)));
out.size = JSON.stringify({ type: "rig", ...encoded }).length;
out.sizeWithoutAnchors = JSON.stringify({ type: "rig", ...wire.encodeRig({ ...full, anchors: null }) }).length;
out.firstKey = Object.keys({ type: "rig", ...encoded })[0];
out.round = [back.yaw, back.pitch, back.roll, back.mouth, back.tongue, back.browLeft, back.words, back.talking, back.hands[0].side, back.hands[0].curl[2]];
out.deltaError = Math.max(...Array.from(deltas).map((v, i) => Math.abs(v - back.deltas[i])));
out.anchorError = Math.max(...full.anchors.flatMap((p, i) => [Math.abs(p[0] - back.anchors[i][0]), Math.abs(p[1] - back.anchors[i][1])]));
out.audioOnly = wire.decodeRig(wire.encodeRig({ yaw: 0, pitch: 0, roll: 0, hands: [], talking: true, words: 4 }));
out.rejected = [wire.decodeRig({ v: 2 }), wire.decodeRig(null)];
out.reactions = Object.keys(wire.RIG_REACTIONS);

const open = new Float32Array(136);
open[62 * 2 + 1] = -0.2;
open[66 * 2 + 1] = 0.2;
const closed = rig.closedMouth(open);
out.closed = [closed[62 * 2 + 1], closed[66 * 2 + 1]];

function hand(extended, thumb, thumbUp = 0) {
  const p = Array.from({ length: 21 }, () => [0, 0]);
  p[0] = [0, 0];
  const bases = { index: [-0.6, -2], middle: [-0.2, -2.1], ring: [0.2, -2], pinky: [0.6, -1.8] };
  const chain = { index: [5, 6, 7, 8], middle: [9, 10, 11, 12], ring: [13, 14, 15, 16], pinky: [17, 18, 19, 20] };
  for (const [name, [bx, by]] of Object.entries(bases)) {
    const [mcp, pip, dip, tip] = chain[name];
    p[mcp] = [bx, by];
    if (extended.includes(name)) {
      p[pip] = [bx, by - 0.8];
      p[dip] = [bx, by - 1.4];
      p[tip] = [bx, by - 1.9];
    } else {
      p[pip] = [bx, by - 0.6];
      p[dip] = [bx, by - 0.2];
      p[tip] = [bx * 0.8, by + 0.3];
    }
  }
  p[1] = [-0.7, -0.5];
  p[2] = [-1.1, -0.9];
  if (thumb) {
    p[3] = [-1.0, -3.0];
    p[4] = [-1.0, -4.0];
    if (!thumbUp) {
      p[3] = [-1.8, -1.1];
      p[4] = [-2.5, -1.3];
    }
  } else {
    p[3] = [-0.9, -1.2];
    p[4] = [-0.5, -1.3];
  }
  return p;
}
out.gestures = {
  palm: hands.classifyHand(hand(["index", "middle", "ring", "pinky"], true)),
  victory: hands.classifyHand(hand(["index", "middle"], false)),
  rock: hands.classifyHand(hand(["index", "pinky"], false)),
  point: hands.classifyHand(hand(["index"], false)),
  fist: hands.classifyHand(hand([], false)),
  thumbsUp: hands.classifyHand(hand([], true, 1)),
};
out.curls = hands.fingerCurls(hand(["index"], false)).map((v) => Math.round(v * 10) / 10);
out.anchorsAt = hands.palmAnchors().length;

const detector = hands.createGestureDetector({ holdMs: 400, cooldownMs: 2000 });
const fired = [];
for (let t = 0; t <= 1200; t += 100) fired.push(detector.update("thumbs_up", t));
for (let t = 1300; t <= 1600; t += 100) fired.push(detector.update(null, t));
for (let t = 1700; t <= 2600; t += 100) fired.push(detector.update("thumbs_up", t));
for (let t = 2700; t <= 5000; t += 100) fired.push(detector.update("victory", t));
out.fired = fired.filter(Boolean);

const counter = speech.createSyllableCounter();
for (let t = 0; t < 4000; t += 20) {
  const voiced = t >= 500 && t < 3500;
  const db = voiced ? -30 + 12 * Math.sin((2 * Math.PI * (t - 500)) / 250) : -75;
  counter.push(db, t);
}
out.syllables = counter.syllables;
out.states = [speech.speakerState({ talking: true }), speech.speakerState({ lastSpokeAt: Date.now() - 1000 }), speech.speakerState({ lastSpokeAt: Date.now() - 60000 })];

const anchors = wire.RIG_TEMPLATE.map(([x, y]) => [64 + x * 30, 50 + y * 30]);
const moved = puppet.puppetTargets(anchors, Float32Array.from({ length: 136 }, (_, i) => (i === 66 * 2 + 1 ? 0.5 : 0)));
out.puppet = [moved[66][1] - anchors[66][1], moved[0][1] - anchors[0][1]];

const pair = rig.assignHandSides([{ side: "right", x: 2, y: 1, curl: [0, 0, 0, 0, 0], roll: 0 }, { side: "right", x: -1.5, y: 1, curl: [0, 0, 0, 0, 0], roll: 0 }]);
out.pairSides = pair.map((hand) => [hand.side, hand.x]);
out.stickySide = rig.assignHandSides([{ side: "right", x: 0.3, y: 1, curl: [0, 0, 0, 0, 0], roll: 0 }], [{ side: "left", x: -0.2, y: 1 }])[0].side;
const smoother = rig.createHandSmoother({ holdMs: 300 });
const still = { side: "left", x: 0, y: 0, z: 0, curl: [0, 0, 0, 0, 0], roll: 0 };
smoother.smooth([still], 0);
const jitter = smoother.smooth([{ ...still, x: 0.2 }], 40);
const held = smoother.smooth([], 200);
const dropped = smoother.smooth([], 600);
out.smoothing = [Math.round(jitter[0].x * 1000) / 1000, held.length, dropped.length];
const frame = rig.imageFrame(640, 360);
out.imageFrame = [frame.mid, Math.round(frame.span * 10) / 10];
const span = 60;
const near = { points: Array.from({ length: 21 }, () => [0, 0]), palm: span * 1.5 * 1.6 };
const far = { points: Array.from({ length: 21 }, () => [0, 0]), palm: span * 1.5 * 0.7 };
out.depth = [rig.handDepth(near, { span }), rig.handDepth(far, { span })].map((v) => Math.round(v * 100) / 100);
const deep = wire.decodeRig(wire.encodeRig({ yaw: 0, pitch: 0, roll: 0, hands: [{ side: "left", x: 1, y: 2, z: 0.6, curl: [0, 0, 0, 0, 0], roll: 0 }] }));
const legacy = wire.decodeRig({ v: 1, p: [0, 0, 0], h: [[0, 20, 40, 0, 0, 0, 0, 0, 0]] });
out.wireDepth = [deep.hands[0].z, legacy.hands[0].z, wire.encodeRig({ yaw: 0, pitch: 0, roll: 0, hands: [{ side: "left", x: 1, y: 2, z: 0.6, curl: [0, 0, 0, 0, 0], roll: 0 }] }).h[0].length];
console.log(JSON.stringify(out));
'''


@functools.cache
def _run():
    done = subprocess.run(["node", "--input-type=module", "-e", HARNESS, str(PUBLIC)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    return json.loads(done.stdout)


def test_rig_frames_round_trip_and_fit_the_world_socket():
    data = _run()
    assert data["firstKey"] == "type"
    assert data["size"] < 1024 and data["sizeWithoutAnchors"] < 400
    assert data["round"] == [0.3, -0.2, 0.05, 0.6, 0.7, -0.5, 321, True, "right", 1]
    assert data["deltaError"] <= 1 / 128 + 1e-6
    assert data["anchorError"] <= 1 / 510 + 1e-6
    assert data["audioOnly"]["deltas"] is None and data["audioOnly"]["talking"] is True and data["audioOnly"]["words"] == 4
    assert data["rejected"] == [None, None]
    assert {"thumbs_up", "victory", "heart", "clap", "laugh", "fire"} <= set(data["reactions"])


def test_the_neutral_face_always_has_a_closed_mouth():
    assert _run()["closed"] == [0, 0]


def test_hand_shapes_become_gestures_and_fire_once_per_hold():
    data = _run()
    assert data["gestures"] == {"palm": "open_palm", "victory": "victory", "rock": "rock", "point": "point_up", "fist": "fist", "thumbsUp": "thumbs_up"}
    assert data["curls"][1] < 0.2 and min(data["curls"][2:]) > 0.5
    assert data["anchorsAt"] == 2016
    assert data["fired"] == ["thumbs_up", "thumbs_up", "victory"]


def test_words_are_counted_from_syllables_without_recognising_speech():
    data = _run()
    assert 10 <= data["syllables"] <= 13
    assert data["states"] == ["talking", "recent", "quiet"]
    source = (PUBLIC / "call-speech-meter.js").read_text(encoding="utf-8")
    assert "SpeechRecognition" not in source


def test_the_photo_puppet_moves_only_the_points_that_moved():
    assert _run()["puppet"][1] == 0
    assert abs(_run()["puppet"][0] - 15) < 0.01


def test_two_hands_take_opposite_sides_and_one_hand_keeps_its_side():
    data = _run()
    assert data["pairSides"] == [["right", -1.5], ["left", 2]]
    assert data["stickySide"] == "left"


def test_hands_are_smoothed_held_briefly_and_then_dropped():
    x, held, dropped = _run()["smoothing"]
    assert 0 < x < 0.2
    assert held == 1 and dropped == 0


def test_hands_without_a_face_use_the_image_frame_and_carry_depth():
    data = _run()
    assert data["imageFrame"] == [[320, 144], 70.4]
    near, far = data["depth"]
    assert near == 0.6 and far == -0.3
    assert data["wireDepth"] == [0.6, 0, 10]
    tracker = (PUBLIC / "chat-call-hand-tracker.js").read_text(encoding="utf-8")
    assert "roi.followed ? HAND_KEEP_SCORE : HAND_SCORE" in tracker
    assert "rois.length ? REDETECT_MS : SEARCH_MS" in tracker


def _header(path):
    data = path.read_bytes()
    assert data[:4] == b"FMNN"
    length = struct.unpack_from("<I", data, 8)[0]
    return json.loads(data[12:12 + length]), len(data) - 12 - length


def test_hand_models_are_bundled_with_their_licence_and_use_supported_ops():
    expected = {"palm-detection-lite.fmnn": [192, 192, 3], "hand-landmark-lite.fmnn": [224, 224, 3]}
    for name, shape in expected.items():
        header, blob = _header(HANDS / name)
        assert header["shapes"][str(header["input"])] == shape
        assert {op["op"] for op in header["ops"]} <= {"conv", "dw", "add", "mean", "maxpool", "pad", "resize", "act"}
        for op in header["ops"]:
            for key in ("w", "b", "alpha"):
                if key in op:
                    assert op[key][0] + op[key][1] <= blob // 2
    landmark, _ = _header(HANDS / "hand-landmark-lite.fmnn")
    assert [[landmark["shapes"][str(i)] for i in group] for group in landmark["outputs"]] == [[[1, 1, 63]], [[1, 1, 1]], [[1, 1, 1]], [[1, 1, 63]]]
    assert "Apache License" in (HANDS / "LICENSE").read_text(encoding="utf-8")
    assert '9: "FULLY_CONNECTED"' in CONVERTER and '3: "relu6"' in CONVERTER
    assert 'if (name === "relu6") return "v = clamp(v, 0.0, 6.0);";' in SEGMENT

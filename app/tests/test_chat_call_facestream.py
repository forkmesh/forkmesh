import json
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
PUBLIC = ROOT / "public"
MODULE = PUBLIC / "chat-call-facestream.js"
FIXTURE = REPOSITORY / "server" / "rtc" / "facestream" / "testdata" / "face-stream-parity.json"
FIXTURE_COPIES = (
    REPOSITORY / "native" / "ios" / "ForkMeshCore" / "Tests" / "FMChatTests" / "Fixtures" / "face-stream-parity.json",
    REPOSITORY / "native" / "android" / "core" / "src" / "test" / "resources" / "chat" / "face-stream-parity.json",
)
SWIFT = REPOSITORY / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMChat" / "FaceStream" / "FaceStreamCodec.swift"
KOTLIN = REPOSITORY / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "chat" / "facestream" / "FaceStreamCodec.kt"
EVAL = REPOSITORY / "tools" / "face_stream_eval.mjs"

PARITY_HARNESS = r'''
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
const fs = await import(pathToFileURL(process.argv[2]).href);
const fixture = JSON.parse(readFileSync(process.argv[3], "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const unhex = (h) => new Uint8Array(Buffer.from(h, "hex"));
const pts = (flat) => { const o = []; for (let i = 0; i < flat.length; i += 2) o.push([flat[i], flat[i + 1]]); return o; };
const names = ["lidLeft", "lidRight", "gazeX", "gazeY", "mouth", "smile", "tongue"];
let worst = 0;
let packets = 0;
const mismatches = [];
const close = (a, b, label) => {
  const e = Math.abs(a - b) / Math.max(1, Math.abs(b));
  worst = Math.max(worst, e);
  if (!(e <= 1e-9)) mismatches.push(`${label}: ${a} vs ${b}`);
};
for (const s of fixture.streams) {
  const enc = fs.createFaceStreamEncoder(s.config);
  const dec = fs.createFaceStreamDecoder();
  let d = 0;
  s.inputs.forEach((inp, i) => {
    if (s.requestKeyAt?.includes(i)) enc.requestKey();
    const packet = enc.encode({
      time: inp.time,
      points: pts(inp.points),
      pose: { rotation: inp.pose.slice(0, 4), x: inp.pose[4], y: inp.pose[5], scale: inp.pose[6] },
      expression: Object.fromEntries(names.map((n, k) => [n, inp.expression[k]])),
    });
    packets += 1;
    if (hex(packet) !== s.packets[i]) mismatches.push(`${s.name} packet ${i}`);
    if (i < s.decodeFrom || s.drop.includes(i)) return;
    const want = s.decoded[d++];
    try {
      const got = dec.decode(unhex(s.packets[i]));
      if (want.error) { mismatches.push(`${s.name} ${i} should fail with ${want.error}`); return; }
      for (const k of ["key", "stale", "seq", "keyId", "time", "width", "height", "precision"]) if (got[k] !== want.frame[k]) mismatches.push(`${s.name} ${i}.${k}`);
      [...got.pose.rotation, got.pose.x, got.pose.y, got.pose.scale].forEach((v, k) => close(v, want.frame.pose[k], `${s.name} ${i} pose`));
      names.forEach((n, k) => close(got.expression[n], want.frame.expression[k], `${s.name} ${i} ${n}`));
      got.points.flat().forEach((v, k) => close(v, want.frame.points[k], `${s.name} ${i} point`));
    } catch (error) {
      if (error.code !== want.error) mismatches.push(`${s.name} ${i}: ${error.code} vs ${want.error}`);
    }
  });
}
for (const c of fixture.references) {
  const i = c.input;
  const packet = fs.encodeFaceStreamReference({
    id: i.id, format: i.format, time: i.time, cropX: i.crop[0], cropY: i.crop[1], cropW: i.crop[2], cropH: i.crop[3],
    width: i.width, height: i.height, points: pts(i.points), maskW: i.maskW, maskH: i.maskH,
    mask: i.mask ? Uint8Array.from(i.mask, (ch) => (ch === "1" ? 1 : 0)) : null, image: unhex(i.image),
  });
  if (hex(packet) !== c.packet) mismatches.push("reference packet");
  const got = fs.decodeFaceStreamReference(unhex(c.packet));
  if ((got.mask ? Array.from(got.mask).join("") : "") !== c.decoded.mask) mismatches.push("reference mask");
  got.points.flat().forEach((v, k) => close(v, c.decoded.points[k], "reference point"));
  if (hex(got.image) !== c.decoded.image) mismatches.push("reference image");
}
console.log(JSON.stringify({ packets, worst, mismatches: mismatches.slice(0, 10), count: mismatches.length }));
'''

BEHAVIOUR_HARNESS = r'''
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
const fs = await import(pathToFileURL(process.argv[2]).href);
const geo = await import(pathToFileURL(process.argv[2].replace(/chat-call-facestream\.js$/, "chat-call-face-geometry.js")).href);
const fixture = JSON.parse(readFileSync(process.argv[3], "utf8"));
const unhex = (h) => new Uint8Array(Buffer.from(h, "hex"));
const out = {};

const norm = (q) => { const n = Math.hypot(...q); return q.map((v) => v / n); };
const q = norm([0.97, 0.12, -0.18, 0.06]);
const back = fs.quatFromRows(fs.rotationRows(q));
out.quatError = Math.max(...q.map((v, i) => Math.abs(v - back[i])));
const pose = { rotation: q, x: 600, y: 320, scale: 90 };
const r3 = fs.rotationRows(q)[2];
const projected = fs.projectFaceStreamPose(pose).map(([x, y], i) => [x, y, 90 * (r3[0] * fs.FACE_STREAM_TEMPLATE[i][0] + r3[1] * fs.FACE_STREAM_TEMPLATE[i][1] + r3[2] * fs.FACE_STREAM_TEMPLATE[i][2])]);
const solved = fs.faceStreamPose(projected);
out.poseError = { scale: Math.abs(solved.scale - 90), x: Math.abs(solved.x - 600), rotation: Math.max(...q.map((v, i) => Math.abs(v - solved.rotation[i]))) };

const frontal = { yaw: 0.02, pitch: 0.01, roll: 0 };
const open = { lidLeft: 0, lidRight: 0, mouth: 0, tongue: 0 };
const light = { mean: 0.5, contrast: 0.1 };
out.scores = {
  good: fs.referenceScore({ pose: frontal, expression: open, light, span: 90 }),
  turned: fs.referenceScore({ pose: { ...frontal, yaw: 0.4 }, expression: open, light, span: 90 }),
  blinking: fs.referenceScore({ pose: frontal, expression: { ...open, lidLeft: 0.5 }, light, span: 90 }),
  talking: fs.referenceScore({ pose: frontal, expression: { ...open, mouth: 0.3 }, light, span: 90 }),
  dark: fs.referenceScore({ pose: frontal, expression: open, light: { mean: 0.1, contrast: 0.1 }, span: 90 }),
  flat: fs.referenceScore({ pose: frontal, expression: open, light: { mean: 0.5, contrast: 0.01 }, span: 90 }),
  tiny: fs.referenceScore({ pose: frontal, expression: open, light, span: 20 }),
};
out.due = [
  fs.referenceDue(null, 0, 0.9),
  fs.referenceDue({ time: 0, score: 0.5 }, 1500, 1),
  fs.referenceDue({ time: 0, score: 0.5 }, 2500, 0.9),
  fs.referenceDue({ time: 0, score: 0.85 }, 2500, 0.9),
  fs.referenceDue({ time: 0, score: 0.95 }, 10000, 0.5),
  fs.referenceDue(null, 0, 0),
];
const face = fs.projectFaceStreamPose({ rotation: [1, 0, 0, 0], x: 640, y: 330, scale: 96 });
const crop = fs.referenceCrop(face, 1280, 720);
const placed = fs.referencePoints(face, crop, 256, 320);
const edge = fs.referenceCrop(fs.projectFaceStreamPose({ rotation: [1, 0, 0, 0], x: 40, y: 700, scale: 300 }), 1280, 720);
out.crop = { crop, aspect: crop.w / crop.h, inside: placed.every(([x, y]) => x > 0 && x < 256 && y > 0 && y < 320), edge, edgeInside: edge.x >= 0 && edge.y >= 0 && edge.x + edge.w <= 1280 && edge.y + edge.h <= 720 };
const maskBytes = new Uint8Array(1280 * 720);
for (let y = 0; y < 720; y += 1) for (let x = 0; x < 1280; x += 1) maskBytes[y * 1280 + x] = Math.hypot(x - 640, y - 330) < 200 ? 255 : 0;
const bits = fs.referenceMaskBits({ bytes: maskBytes, width: 1280, height: 720 }, crop, 64, 80);
out.mask = { ones: bits.reduce((s, v) => s + v, 0), centre: bits[40 * 64 + 32], corner: bits[0] };

const stream = fixture.streams[0];
const receiver = fs.createFaceStreamReceiver();
const reference = receiver.receive(unhex(fixture.references[0].packet));
const beforeFrame = receiver.livePoints(320, 400);
const first = receiver.receive(unhex(stream.packets[0]));
const live = receiver.livePoints(320, 400);
receiver.receive(unhex(stream.packets[1]));
const skipped = receiver.receive(unhex(stream.packets[3]));
const bad = receiver.receive(new Uint8Array([0x50]));
out.receiver = {
  reference: reference.type,
  beforeFrame,
  first: first.type,
  liveCount: live.length,
  depth: live.every((p) => Number.isFinite(p[2])),
  noseNearer: live[30][2] > live[0][2],
  inTile: live.every(([x, y]) => x > 0 && x < 320 && y > 0 && y < 400),
  skippedStale: skipped.frame.stale,
  needsKey: skipped.needsKey,
  bad: bad.error,
  errors: receiver.errors,
};

const decoder = fs.createFaceStreamDecoder();
decoder.decode(unhex(stream.packets[0]));
const good = decoder.decode(unhex(stream.packets[1]));
decoder.decode(unhex(stream.packets[3]));
const later = decoder.decode(unhex(stream.packets[4]));
const opened = fs.faceStreamConceal({ ...later, expression: { ...later.expression, mouth: good.expression.mouth + 0.4 } }, good.expression.mouth);
out.conceal = { lowered: opened.points[66][1] - later.points[66][1], upperStill: opened.points[62][1] - later.points[62][1], freshUntouched: fs.faceStreamConceal(good, 0) === good };

let state = 0x1234;
const next = () => { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; return (state >>> 0) & 255; };
let thrown = 0;
let foreign = 0;
for (let round = 0; round < 600; round += 1) {
  const d = fs.createFaceStreamDecoder();
  d.decode(unhex(stream.packets[0]));
  const packet = Uint8Array.from({ length: 2 + (round % 90) }, next);
  if (round % 3 === 0) packet[0] = (packet[0] & 0x4f) | 0x10;
  for (const run of [() => d.decode(packet), () => fs.decodeFaceStreamReference(Uint8Array.from({ length: 300 + round }, next))]) {
    try {
      const frame = run();
      if (frame.points && !frame.points.flat().every(Number.isFinite)) foreign += 1;
    } catch (error) {
      if (error instanceof fs.FaceStreamError) thrown += 1;
      else foreign += 1;
    }
  }
}
out.fuzz = { thrown, foreign };
out.smile = { neutral: fs.faceStreamSmile(face), wide: fs.faceStreamSmile(face.map(([x, y], i) => (i === 48 ? [x - 20, y] : i === 54 ? [x + 20, y] : [x, y]))) };
out.labels = { label: fs.FACE_STREAM_LABEL, detail: fs.FACE_STREAM_DETAIL, stale: fs.FACE_STREAM_STALE_DETAIL };
out.encodeErrors = [];
for (const bad of [{ width: 0, height: 10 }, { width: 10, height: 10, precision: 8 }]) {
  try { fs.createFaceStreamEncoder(bad); out.encodeErrors.push("accepted"); } catch (error) { out.encodeErrors.push(error.code); }
}
try {
  fs.createFaceStreamEncoder({ width: 10, height: 10 }).encode({ time: 0, points: face.map(([x]) => [x, NaN]), pose: { rotation: [1, 0, 0, 0], x: 0, y: 0, scale: 1 } });
  out.encodeErrors.push("accepted");
} catch (error) {
  out.encodeErrors.push(error.code);
}
console.log(JSON.stringify(out));
'''


def _node(harness):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(MODULE), str(FIXTURE)],
        input=harness, capture_output=True, text=True, timeout=120, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


@pytest.fixture(scope="module")
def behaviour():
    return _node(BEHAVIOUR_HARNESS)


def test_js_codec_is_byte_identical_to_the_go_reference():
    data = _node(PARITY_HARNESS)
    assert data["count"] == 0, data["mismatches"]
    assert data["packets"] == 115
    assert data["worst"] <= 1e-12


def test_fixture_copies_for_the_phones_match_the_go_testdata():
    reference = FIXTURE.read_bytes()
    for copy in FIXTURE_COPIES:
        assert copy.read_bytes() == reference, copy


def test_pose_round_trips_through_the_quaternion(behaviour):
    assert behaviour["quatError"] < 1e-12
    assert behaviour["poseError"]["scale"] < 0.5
    assert behaviour["poseError"]["x"] < 0.5
    assert behaviour["poseError"]["rotation"] < 1e-3


def test_reference_picture_needs_a_frontal_open_well_lit_face(behaviour):
    scores = behaviour["scores"]
    assert scores["good"] > 0.8
    for gated in ("turned", "blinking", "talking", "dark", "flat", "tiny"):
        assert scores[gated] == 0, gated
    assert behaviour["due"] == [True, False, True, False, True, False]


def test_reference_crop_frames_the_head_at_four_by_five(behaviour):
    crop = behaviour["crop"]
    assert abs(crop["aspect"] - 0.8) < 0.01
    assert crop["inside"]
    assert crop["edgeInside"]
    assert behaviour["mask"]["centre"] == 1
    assert behaviour["mask"]["corner"] == 0
    assert 0 < behaviour["mask"]["ones"] < 64 * 80


def test_receiver_maps_points_into_the_tile_with_depth(behaviour):
    r = behaviour["receiver"]
    assert r["reference"] == "reference"
    assert r["beforeFrame"] is None
    assert r["first"] == "frame"
    assert r["liveCount"] == 68
    assert r["depth"] and r["noseNearer"] and r["inTile"]
    assert r["skippedStale"] and r["needsKey"]
    assert r["bad"] == "short"
    assert r["errors"] == 1


def test_a_lost_frame_keeps_the_mouth_moving(behaviour):
    c = behaviour["conceal"]
    assert c["lowered"] > 5
    assert abs(c["upperStill"]) < 1e-9
    assert c["freshUntouched"]


def test_decoders_only_throw_face_stream_errors(behaviour):
    assert behaviour["fuzz"]["foreign"] == 0
    assert behaviour["fuzz"]["thrown"] > 500


def test_bad_configs_and_inputs_are_refused(behaviour):
    assert behaviour["encodeErrors"] == ["config", "config", "input"]
    assert behaviour["smile"]["neutral"] < 0.02
    assert behaviour["smile"]["wide"] > 0.5


def test_tile_label_is_the_same_on_every_platform(behaviour):
    labels = behaviour["labels"]
    swift = SWIFT.read_text(encoding="utf-8")
    kotlin = KOTLIN.read_text(encoding="utf-8")
    for text in (labels["label"], labels["detail"], labels["stale"]):
        assert f'"{text}"' in swift
        assert f'"{text}"' in kotlin
    assert labels["label"] == "Face stream"


def test_face_changer_still_exports_its_renderer_and_warp():
    render = (PUBLIC / "chat-call-face-render.js").read_text(encoding="utf-8")
    assert "export function createFaceRenderer" in render
    assert re.search(r"export const FACE_WARP = Object\.freeze\(\{ vertex: WARP_VERTEX, fragment: WARP_FRAGMENT, compile, texture, target \}\);", render)
    module = MODULE.read_text(encoding="utf-8")
    assert "FACE_WARP.vertex, FACE_WARP.fragment" in module


def test_face_stream_renders_a_live_face_in_headless_chromium(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    env = {k: v for k, v in os.environ.items() if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    result = subprocess.run(
        [node, str(EVAL), str(tmp_path), "--frames", "75", "--every", "15", "--face", "theo"],
        capture_output=True, text=True, timeout=600, check=False, env=env,
    )
    if result.returncode != 0 and "playwright-core not found" in result.stderr:
        pytest.skip("playwright-core is not installed")
    assert result.returncode == 0, result.stderr[-2000:]
    summary = json.loads(result.stdout.strip().splitlines()[-1])
    assert summary["bytesPerFrame"] <= 60
    assert summary["codecRmsPx"] <= 0.6
    assert summary["references"] >= 1
    assert summary["psnrCodecOnly"] > 35
    assert summary["psnrFace"] > summary["psnrFaceFrozen"] + 3
    assert summary["ssimFace"] > 0.6
    assert (tmp_path / "face-stream-000.png").exists()

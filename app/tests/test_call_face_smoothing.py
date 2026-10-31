import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
STAGE = (PUBLIC / "chat-call-faces.js").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv.at(-1);
const G = await import(pathToFileURL(base + "chat-call-face-geometry.js").href);
const F = await import(pathToFileURL(base + "chat-call-faces.js").href);
const face = F.faceTemplate().map(([x, y]) => [x * 100 + 320, y * 100 + 240]);
const LOWER = new Set([55, 56, 57, 58, 59, 65, 66, 67]);
let seed = 11;
const noise = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647 - 0.5; };
const legacy = () => {
  const smoother = G.createPointSmoother(68, 1.5, 1.5);
  return { smooth: (points, time) => smoother.smooth(points, time, G.faceFrame(points).span) };
};
function error(make, { talk = 0, move = 0, jitter = 1.5 }) {
  seed = 11;
  const smoother = make();
  let sum = 0;
  let count = 0;
  for (let k = 0; k < 300; k += 1) {
    const time = 1000 + k * 33.3;
    const open = talk * Math.max(0, Math.sin((2 * Math.PI * 4 * time) / 1000));
    const dx = move * Math.sin((2 * Math.PI * 0.7 * time) / 1000);
    const truth = face.map(([x, y], i) => [x + dx, y + (LOWER.has(i) ? open : 0)]);
    const seen = truth.map(([x, y]) => [x + jitter * noise(), y + jitter * noise()]);
    const out = smoother.smooth(seen, time);
    if (k < 30) continue;
    for (const i of [8, 30, 39, 48, 62, 66]) {
      sum += (out[i][0] - truth[i][0]) ** 2 + (out[i][1] - truth[i][1]) ** 2;
      count += 1;
    }
  }
  return Math.sqrt(sum / count);
}
const out = {};
for (const [name, motion] of Object.entries({ still: {}, talking: { talk: 12 }, moving: { move: 60 } })) {
  out[name] = { legacy: error(legacy, motion), landmarks: error(() => G.createLandmarkSmoother(), motion) };
}
const smoother = G.createLandmarkSmoother();
const deep = face.map(([x, y]) => [x, y, 7]);
smoother.smooth(deep, 0);
const moved = smoother.smooth(deep.map(([x, y, z]) => [x + 5, y, z]), 33);
out.depth = moved.every((point) => point.length === 3);
out.shape = moved.slice(48, 68).map(([x, y], i) => [x - moved[48][0], y - moved[48][1]]).every(([x, y], i) => {
  const [ax, ay] = [face[48 + i][0] - face[48][0], face[48 + i][1] - face[48][1]];
  return Math.abs(x - ax) < 1e-6 && Math.abs(y - ay) < 1e-6;
});
smoother.reset();
out.reset = smoother.smooth(face, 66)[30][0] === face[30][0];
out.short = smoother.smooth(face.slice(0, 10), 99).length === 10;
console.log(JSON.stringify(out));
'''


@pytest.fixture(scope="module")
def result():
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    run = subprocess.run(
        ["node", "--input-type=module", "-e", HARNESS, str(PUBLIC) + "/"],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert run.returncode == 0, run.stderr[-2000:]
    return json.loads(run.stdout)


def test_lips_follow_speech_closely(result):
    talking = result["talking"]
    assert talking["landmarks"] < talking["legacy"] * 0.6


def test_head_follows_movement_with_less_lag(result):
    moving = result["moving"]
    assert moving["landmarks"] < moving["legacy"] * 0.5


def test_still_face_stays_steady(result):
    assert result["still"]["landmarks"] < 0.45


def test_rigid_moves_keep_the_lip_shape_and_depth(result):
    assert result["depth"] is True
    assert result["shape"] is True
    assert result["reset"] is True
    assert result["short"] is True


def test_stage_runs_on_camera_frames():
    assert "const FRAME_RATE = 30;" in STAGE
    assert "video.requestVideoFrameCallback(onCameraFrame)" in STAGE
    assert "if (now() - frameAt < FRAME_MS * 3) return;" in STAGE
    assert "createLandmarkSmoother()" in STAGE
    assert "createPointSmoother" not in STAGE

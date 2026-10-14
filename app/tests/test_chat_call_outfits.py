import json
import math
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
OUTFITS = (PUBLIC / "chat-call-outfits.js").read_text(encoding="utf-8")
RENDER = (PUBLIC / "chat-call-face-render.js").read_text(encoding="utf-8")
FACES = (PUBLIC / "chat-call-faces.js").read_text(encoding="utf-8")
SYNC = (ROOT / "tools" / "sync_native_call_faces.py").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[1];
const load = (name) => import(pathToFileURL(base + "/" + name).href);
const outfits = await load("chat-call-outfits.js");
const out = {};
out.ids = outfits.CALL_OUTFITS.map((o) => o.id);
out.labels = outfits.CALL_OUTFITS.map((o) => o.label);
const stored = new Map();
const storage = { getItem: (k) => stored.get(k) ?? null, setItem: (k, v) => stored.set(k, v), removeItem: (k) => stored.delete(k) };
out.saved = [outfits.savedCallOutfit(storage)];
outfits.rememberCallOutfit("hoodie", storage);
out.saved.push(outfits.savedCallOutfit(storage), stored.get(outfits.CALL_OUTFIT_STORAGE_KEY));
outfits.rememberCallOutfit("nope", storage);
out.saved.push(outfits.savedCallOutfit(storage));
stored.set(outfits.CALL_OUTFIT_STORAGE_KEY, "gone");
out.saved.push(outfits.savedCallOutfit(storage));
function face(cx, cy, span, roll = 0) {
  const points = Array.from({ length: 68 }, () => [cx, cy]);
  const c = Math.cos(roll), s = Math.sin(roll);
  const put = (dx, dy) => [cx + dx * c - dy * s, cy + dx * s + dy * c];
  for (let i = 36; i <= 41; i += 1) points[i] = put(-span / 2, 0);
  for (let i = 42; i <= 47; i += 1) points[i] = put(span / 2, 0);
  points[8] = put(0, 1.9 * span);
  return points;
}
out.short = outfits.outfitFrame([[0, 0]]);
const level = outfits.outfitFrame(face(320, 200, 60));
out.level = level;
out.levelUv = outfits.outfitUv(level, level.center);
out.belowUv = outfits.outfitUv(level, [level.center[0], level.center[1] + 2 * level.unit]);
out.tilted = outfits.outfitFrame(face(320, 200, 60, 0.4));
const smoother = outfits.createOutfitSmoother(0.5);
smoother.smooth(level);
out.smoothed = smoother.smooth(outfits.outfitFrame(face(360, 200, 60)));
out.held = smoother.smooth(null);
smoother.reset();
out.reset = smoother.smooth(null);
out.noDocument = outfits.drawCallOutfit("suit", null);
out.unknown = outfits.drawCallOutfit("cape", {});
console.log(JSON.stringify(out));
'''


def _node():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run([node, "--input-type=module", "-e", HARNESS, str(PUBLIC)], capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_outfit_list_starts_with_your_own_clothes():
    data = _node()
    assert data["ids"] == ["", "suit", "hoodie", "sweater", "denim", "flannel", "turtleneck"]
    assert data["labels"][0] == "Your own clothes"
    assert len(set(data["labels"])) == len(data["labels"])


def test_outfit_choice_is_remembered_and_unknown_ids_clear_it():
    data = _node()
    assert data["saved"] == ["", "hoodie", "hoodie", "", ""]


def test_outfit_sits_below_the_chin_and_scales_with_the_face():
    data = _node()
    assert data["short"] is None
    level = data["level"]
    assert level["unit"] == pytest.approx(60)
    assert level["center"][0] == pytest.approx(320)
    assert level["center"][1] == pytest.approx(200 + 1.9 * 60 + 1.35 * 60)
    assert data["levelUv"] == pytest.approx([512 / 1024, 176 / 1024])
    assert data["belowUv"] == pytest.approx([512 / 1024, (176 + 2 * 96) / 1024])


def test_shoulders_turn_less_than_the_head():
    tilted = _node()["tilted"]
    assert tilted["roll"] == pytest.approx(0.4 * 0.3)
    assert math.hypot(*tilted["x"]) * tilted["unit"] == pytest.approx(1)


def test_outfit_smoother_eases_and_resets():
    data = _node()
    assert data["smoothed"]["center"][0] == pytest.approx(340)
    assert data["held"]["center"][0] == pytest.approx(340)
    assert data["reset"] is None


def test_drawing_needs_a_document_and_a_known_outfit():
    data = _node()
    assert data["noDocument"] is None and data["unknown"] is None


def test_composite_dresses_only_the_matted_body_and_fits_the_texture_units():
    composite = RENDER.split("const COMPOSITE_FRAGMENT = `", 1)[1].split("`;", 1)[0]
    assert "uniform sampler2D uOutfit;" in composite
    assert "if (uOutfitOn > 0.001 && uHasMatte > 0.5) front = dress(front, cam, alpha, zone);" in composite
    zone = RENDER.split("const ZONE = `", 1)[1].split("`;", 1)[0]
    matte = (PUBLIC / "chat-call-segment.js").read_text(encoding="utf-8").split("export const MATTE_SAMPLE = `", 1)[1].split("`;", 1)[0]
    samplers = set(re.findall(r"uniform sampler2D (\w+);", composite + zone + matte))
    minimum = int(re.search(r"const MIN_UNITS = (\d+);", RENDER).group(1))
    assert len(samplers) <= minimum
    assert '["uOutfit", outfitTexture],' in RENDER
    assert "gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);" in RENDER


def test_stage_tracks_the_face_for_an_outfit_without_a_stand_in_face():
    assert "function wantsPoints() {\n    return Boolean(face || outfit);\n  }" in FACES
    assert "return Boolean(face || outfit) || background.mode !== \"off\";" in FACES
    assert "if (wantsPoints() && !hostLocate) groups.push(\"face\");" in FACES
    assert "setOutfit(id) {\n      loadOutfit(id);\n    }," in FACES
    assert "outfitId = \"\"," in FACES


def test_native_pages_inline_the_outfit_module_before_the_renderer():
    assert SYNC.index('"chat-call-outfits.js"') < SYNC.index('"chat-call-face-render.js"')
    assert not re.search(r"^\s*//", OUTFITS, re.M) and "/*" not in OUTFITS

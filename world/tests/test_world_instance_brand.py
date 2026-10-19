import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

SCRIPT = r"""
import {
  brandCanvasText, installCanvasBrand, instanceBrandName,
} from "./world-brand.js";

const doc = (name) => ({ querySelector: (selector) =>
  selector === "script[data-instance-brand]" && name !== null
    ? { dataset: { instanceName: name } } : null });
const out = {};
out.names = [doc("Acme Code"), doc("  Acme   Code "), doc("ForkMesh"),
  doc(""), doc(null), doc("<b>x</b>")].map(instanceBrandName);
out.texts = [
  "FORKMESH OFFICE", "WELCOME TO THE FORKMESH OFFICE", "ForkMesh coaster",
  "ForkMesh World", "FORKMESH WORLD", "ForkMesh Desktop",
  "RSS: FORKMESH.COM/BLOG/RSS.XML", "OPENS @FORKMESH ON MASTODON",
  "forkmesh/forkmesh", "github.com/ForkMesh", "ForkMesh-Bot", "Town Square",
].map((text) => brandCanvasText(text, "Acme Code"));
out.untouched = brandCanvasText("FORKMESH OFFICE", "");
out.number = brandCanvasText(42, "Acme Code");

class Context { fillText(text) { return "fill:" + text; }
  strokeText(text) { return "stroke:" + text; }
  measureText(text) { return { text }; } }
const scope = { CanvasRenderingContext2D: Context };
out.installed = installCanvasBrand("Acme Code", scope);
out.again = installCanvasBrand("Acme Code", scope);
const context = new Context();
out.drawn = [context.fillText("FORKMESH OFFICE"),
  context.strokeText("ForkMesh World"),
  context.measureText("ForkMesh beach").text];
out.skipped = installCanvasBrand("", { CanvasRenderingContext2D: class {} });
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    run = subprocess.run([NODE, "--input-type=module", "-e", SCRIPT],
                         cwd=WORLD, capture_output=True, text=True,
                         timeout=60)
    assert run.returncode == 0, run.stderr
    return json.loads(run.stdout)


def test_the_instance_name_comes_from_the_brand_script(result):
    assert result["names"] == ["Acme Code", "Acme Code", "", "", "", ""]


def test_scene_text_names_the_instance(result):
    assert result["texts"] == [
        "ACME CODE OFFICE", "WELCOME TO THE ACME CODE OFFICE",
        "Acme Code coaster",
        "ForkMesh World", "FORKMESH WORLD", "ForkMesh Desktop",
        "RSS: FORKMESH.COM/BLOG/RSS.XML", "OPENS @FORKMESH ON MASTODON",
        "forkmesh/forkmesh", "github.com/ForkMesh", "ForkMesh-Bot",
        "Town Square",
    ]
    assert result["untouched"] == "FORKMESH OFFICE"
    assert result["number"] == 42


def test_canvas_text_is_branded_once_and_only_on_an_instance(result):
    assert result["installed"] is True
    assert result["again"] is False
    assert result["drawn"] == ["fill:ACME CODE OFFICE",
                               "stroke:ForkMesh World", "Acme Code beach"]
    assert result["skipped"] is False


def test_the_runtime_brands_canvas_text_before_the_scene_draws():
    assert 'import { installCanvasBrand } from "./world-brand.js";' in APP
    install = APP.index("installCanvasBrand();")
    assert install < APP.index("startWorldBootPrefetch(window);")
    assert install < APP.index('import("./world-scene.js")')

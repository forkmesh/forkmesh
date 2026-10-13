import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
TEXTURES = WORLD / "scene" / "members-circle.js"
DISTRICT = (WORLD / "scene" / "districts" / "members-circle.js").read_text(
    encoding="utf-8")
NODE = shutil.which("node")


def test_band_lettering_is_baked_into_the_band_material():
    bands = DISTRICT.split("function rebuildMemberEngagementBands", 1)[1].split(
        "function setMemberCircleInfo", 1)[0]
    assert bands.count("new THREE.Mesh(") == 2
    assert "memberBandLetteringTexture(" in bands
    assert "emissiveMap: letters," in bands
    assert "bumpMap: letters," in bands
    assert "PlaneGeometry" not in bands
    texture = TEXTURES.read_text(encoding="utf-8")
    assert "memberCircleBandLabelTexture" not in texture
    assert "canvasArtTexture(THREE, width, height" in texture
    assert "texture.colorSpace = THREE.NoColorSpace;" in texture
    assert "context.rotate(middle - Math.PI / 2);" in texture
    lettering = texture.split("export function memberBandLetteringTexture", 1)[1]
    assert "[0, Math.PI].forEach((turn) => {" in lettering
    assert "layout.placed.forEach(letter);" in lettering
    assert "yMin: -top," in texture
    assert "height: top * 2," in texture
    assert "Math.min(1, 2048 / width, 2048 / height)" in lettering


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_each_band_maps_the_shared_lettering_onto_its_own_planar_uvs():
    script = """
      const THREE = await import(%s);
      const circle = await import(%s);
      const base = new THREE.Texture();
      base.userData.letteringRect = { xMin: -6, width: 12, yMin: -20, height: 40 };
      const band = circle.memberBandLetteringFor(base, 10);
      const uv = (x, y, outer) => [(x / outer + 1) / 2, (y / outer + 1) / 2];
      const mapped = ([u, v]) => [
        u * band.repeat.x + band.offset.x,
        v * band.repeat.y + band.offset.y,
      ];
      console.log(JSON.stringify({
        centre: mapped(uv(0, 0, 10)),
        north: mapped(uv(0, 10, 10)),
        south: mapped(uv(0, -10, 10)),
        east: mapped(uv(6, 0, 10)),
        shared: band.source === base.source,
        text: circle.memberBandLetteringText({ label: "Dormant" }, 48.2),
        height: circle.memberBandLetteringHeight({ innerRadius: 10, outerRadius: 12.2 }),
      }));
    """ % (json.dumps(THREE.as_uri()), json.dumps(TEXTURES.as_uri()))
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=60,
    )
    data = json.loads(result.stdout)
    assert data["centre"] == pytest.approx([0.5, 0.5])
    assert data["north"] == pytest.approx([0.5, 0.75])
    assert data["south"] == pytest.approx([0.5, 0.25])
    assert data["east"] == pytest.approx([1.0, 0.5])
    assert data["shared"] is True
    assert data["text"] == "DORMANT  48"
    assert data["height"] == pytest.approx(0.78)

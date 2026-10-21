import collections
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
RESOURCES = WORLD / "scene" / "resources.js"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
NODE = shutil.which("node")
CONSTRUCTION = re.compile(r"new THREE\.(\w+(?:Geometry|Material))\(")

ALLOWED = {
    "elements/aurora-beacon.js": {
        "CylinderGeometry": 2,
        "MeshBasicMaterial": 1,
        "MeshStandardMaterial": 2,
        "TorusGeometry": 1,
    },
    "elements/orbit-sculpture.js": {
        "CylinderGeometry": 1,
        "IcosahedronGeometry": 1,
        "MeshStandardMaterial": 3,
        "TorusGeometry": 1,
    },
    "elements/status-board.js": {
        "BoxGeometry": 1,
        "CylinderGeometry": 1,
        "MeshBasicMaterial": 1,
        "MeshStandardMaterial": 2,
        "PlaneGeometry": 1,
    },
    "elements/status-obelisk.js": {
        "BoxGeometry": 1,
        "ConeGeometry": 1,
        "MeshBasicMaterial": 1,
        "MeshStandardMaterial": 2,
        "PlaneGeometry": 1,
    },
    "scene/agent-bots.js": {"SpriteMaterial": 1},
    "scene/avatar-controls.js": {"PlaneGeometry": 1},
    "scene/avatar-instancing.js": {
        "BoxGeometry": 2,
        "CylinderGeometry": 1,
        "SphereGeometry": 1,
    },
    "scene/avatars.js": {
        "BoxGeometry": 4,
        "CircleGeometry": 2,
        "RingGeometry": 1,
        "SphereGeometry": 2,
    },
    "scene/batching.js": {"BufferGeometry": 2},
    "scene/districts/members-circle.js": {
        "BoxGeometry": 1,
        "BufferGeometry": 1,
        "RingGeometry": 1,
        "TorusGeometry": 1,
    },
    "scene/districts/office-displays.js": {"ExtrudeGeometry": 1},
    "scene/districts/office-floors.js": {"SpriteMaterial": 1},
    "scene/districts/office-rooftop.js": {
        "BoxGeometry": 1,
        "CylinderGeometry": 1,
        "TubeGeometry": 1,
    },
    "scene/districts/quadcopters.js": {
        "BoxGeometry": 3,
        "CylinderGeometry": 2,
        "SphereGeometry": 1,
        "TorusGeometry": 2,
    },
    "scene/districts/coaster.js": {"BufferGeometry": 1},
    "scene/districts/roads.js": {"BufferGeometry": 1},
    "scene/environment.js": {"SphereGeometry": 1},
    "scene/geometry-dedupe.js": {"BufferGeometry": 1},
    "scene/labels.js": {"SpriteMaterial": 1},
    "scene/members-circle.js": {
        "BufferGeometry": 2,
        "InstancedBufferGeometry": 1,
        "PlaneGeometry": 1,
        "ShaderMaterial": 4,
    },
    "scene/office-aquarium.js": {
        "BufferGeometry": 5,
        "LatheGeometry": 1,
        "PlaneGeometry": 1,
        "PointsMaterial": 1,
    },
    "scene/repositories.js": {
        "BufferGeometry": 1,
        "ExtrudeGeometry": 1,
        "PointsMaterial": 1,
        "SpriteMaterial": 3,
        "TorusGeometry": 1,
    },
    "scene/runtime/effects.js": {
        "BufferGeometry": 1,
        "PointsMaterial": 1,
        "SpriteMaterial": 1,
    },
    "scene/instance-hexes.js": {"BufferGeometry": 1},
    "scene/instance-territory.js": {
        "BoxGeometry": 1,
        "BufferGeometry": 1,
        "ConeGeometry": 2,
        "CylinderGeometry": 1,
        "DodecahedronGeometry": 1,
        "ExtrudeGeometry": 1,
    },
    "scene/runtime/network.js": {"BufferGeometry": 1},
    "scene/runtime/office-exits.js": {"BufferGeometry": 1, "LineBasicMaterial": 1},
    "scene/sol-fountain.js": {
        "BufferGeometry": 1,
        "PointsMaterial": 1,
        "SpriteMaterial": 1,
    },
    "scene/territory-bugs.js": {
        "BoxGeometry": 1,
        "BufferGeometry": 1,
        "CircleGeometry": 1,
        "PlaneGeometry": 1,
        "ShapeGeometry": 1,
        "SphereGeometry": 2,
    },
    "scene/town.js": {"BufferGeometry": 1},
    "scene/weather.js": {"BufferGeometry": 1, "PointsMaterial": 2},
    "world-floor-notes.js": {"SpriteMaterial": 1},
    "world-sky.js": {
        "BufferGeometry": 1,
        "MeshBasicMaterial": 3,
        "OctahedronGeometry": 1,
        "PointsMaterial": 1,
        "RingGeometry": 1,
    },
}


def direct_constructions():
    found = {}
    for path in sorted(WORLD.rglob("*.js")):
        if "vendor" in path.parts or path == RESOURCES:
            continue
        counts = collections.Counter(CONSTRUCTION.findall(path.read_text(encoding="utf-8")))
        if counts:
            found[path.relative_to(WORLD).as_posix()] = dict(counts)
    return found


def test_geometries_and_materials_go_through_the_resource_authority():
    over = []
    for path, counts in direct_constructions().items():
        allowed = ALLOWED.get(path, {})
        for kind, count in counts.items():
            if count > allowed.get(kind, 0):
                over.append(f"{path}: {count} x new THREE.{kind} (allowed {allowed.get(kind, 0)})")
    assert not over, "use scene/resources.js (primitive, geometry, material, uniqueMaterial):\n" + "\n".join(over)


def test_the_allow_list_only_shrinks():
    found = direct_constructions()
    stale = [
        f"{path}: {kind} allowed {count}, found {found.get(path, {}).get(kind, 0)}"
        for path, counts in ALLOWED.items()
        for kind, count in counts.items()
        if found.get(path, {}).get(kind, 0) < count
    ]
    assert not stale, "lower the allow-list:\n" + "\n".join(stale)


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_the_authority_shares_what_is_equal_and_defaults_surfaces_to_lambert():
    script = """
      const THREE = await import(%s);
      const resources = await import(%s);
      const a = resources.primitive(THREE, "BoxGeometry", 1, 2, 3);
      const b = resources.primitive(THREE, "BoxGeometry", 1, 2, 3);
      const c = resources.primitive(THREE, "BoxGeometry", 1, 2, 4);
      const keyed = resources.geometry(THREE, "custom", () => new THREE.BufferGeometry());
      const again = resources.geometry(THREE, "custom", () => new THREE.BufferGeometry());
      const m1 = resources.material(THREE, { color: "#ff0000", roughness: 0.8 });
      const m2 = resources.material(THREE, { color: "#ff0000", roughness: 0.8 });
      const hero = resources.material(THREE, { color: "#ff0000", metalness: 0.9, roughness: 0.2 });
      const u1 = resources.uniqueMaterial(THREE, { color: "#ff0000" });
      const u2 = resources.uniqueMaterial(THREE, { color: "#ff0000" });
      a.dispose();
      m1.color.set("#00ff00");
      console.log(JSON.stringify({
        sameBox: a === b,
        otherBox: a !== c,
        keyed: keyed === again,
        sameMaterial: m1 === m2,
        surfaceType: m1.type,
        heroType: hero.type,
        unique: u1 !== u2,
        drift: resources.sharedMaterialDrift(THREE).length,
        stats: resources.resourceStats(THREE),
      }));
    """ % (json.dumps(THREE.as_uri()), json.dumps(RESOURCES.as_uri()))
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=60,
    )
    data = json.loads(result.stdout)
    assert data["sameBox"] and data["otherBox"] and data["keyed"]
    assert data["sameMaterial"]
    assert data["surfaceType"] == "MeshLambertMaterial"
    assert data["heroType"] == "MeshStandardMaterial"
    assert data["unique"]
    assert data["drift"] == 1
    assert data["stats"]["sharedGeometries"] == 3

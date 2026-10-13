#!/usr/bin/env python3
"""Focused deterministic/runtime contracts for the isolated World sky."""

import hashlib
import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SKY_MODULE = ROOT.parent / "world" / "public" / "world" / "world-sky.js"
WORLD_APP = ROOT.parent / "world" / "public" / "world" / "world.js"
SGP4_MODULE = (
    ROOT.parent
    / "world"
    / "public"
    / "world"
    / "vendor"
    / "satellite-js-7.1.0.esm.js"
)
SGP4_LICENSE = SGP4_MODULE.with_name(
    "satellite-js-7.1.0.LICENSE.md"
)


def run_sky_script(body):
    script = f"""
      import * as sky from {json.dumps(SKY_MODULE.as_uri())};
      import * as sgp4Engine from {json.dumps(SGP4_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


# Just enough of the Three.js surface that createWorldSky() touches to observe
# its visibility bookkeeping in Node, with no GPU and no WebGL context.
THREE_STUB = """
class Vec {
  constructor() { this.x = 0; this.y = 0; this.z = 0; }
  set(x, y, z) { this.x = x; this.y = y; this.z = z; return this; }
  setScalar(value) { return this.set(value, value, value); }
}
class Obj {
  constructor() {
    this.children = [];
    this.parent = null;
    this.visible = true;
    this.position = new Vec();
    this.scale = new Vec();
    this.matrix = { x: 0, y: 0, z: 0, scale: 0 };
  }
  add(child) { this.children.push(child); child.parent = this; }
  remove(child) {
    const index = this.children.indexOf(child);
    if (index >= 0) this.children.splice(index, 1);
    child.parent = null;
  }
  clear() { this.children = []; }
  lookAt() {}
  updateMatrix() {
    this.matrix = {
      x: this.position.x,
      y: this.position.y,
      z: this.position.z,
      scale: this.scale.x,
    };
  }
}
class Geometry {
  constructor() { this.attributes = {}; this.disposed = false; }
  setAttribute(name, value) { this.attributes[name] = value; }
  dispose() { this.disposed = true; }
}
class Material {
  constructor(params = {}) { Object.assign(this, params); this.disposed = false; }
  dispose() { this.disposed = true; }
}
class Points extends Obj {
  constructor(geometry, material) {
    super();
    this.geometry = geometry;
    this.material = material;
  }
}
class InstancedMesh extends Obj {
  constructor(geometry, material, capacity) {
    super();
    this.geometry = geometry;
    this.material = material;
    this.capacity = capacity;
    this.count = capacity;
    this.matrices = new Array(capacity).fill(null);
    this.colors = new Array(capacity).fill(null);
    this.instanceMatrix = { needsUpdate: false, setUsage() {} };
    this.instanceColor = { needsUpdate: false };
  }
  setMatrixAt(index, matrix) { this.matrices[index] = { ...matrix }; }
  setColorAt(index, color) { this.colors[index] = color.value; }
}
const THREE = {
  Group: Obj,
  Object3D: Obj,
  BufferGeometry: Geometry,
  BufferAttribute: class {
    constructor(array, itemSize) { this.array = array; this.itemSize = itemSize; }
  },
  Points,
  PointsMaterial: Material,
  InstancedMesh,
  MeshBasicMaterial: Material,
  SphereGeometry: Geometry,
  RingGeometry: Geometry,
  OctahedronGeometry: Geometry,
  Color: class {
    constructor() { this.value = ""; }
    set(value) { this.value = String(value); return this; }
  },
  Vector3: class {
    constructor(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; }
  },
  Sphere: class {
    constructor(center, radius) { this.center = center; this.radius = radius; }
  },
  DynamicDrawUsage: 35048,
  DoubleSide: 2,
  AdditiveBlending: 2,
};
"""


def test_deep_sky_is_drawn_only_while_a_night_sky_is_looked_at():
    result = run_sky_script(
        THREE_STUB
        + """
        const NOW = Date.parse("2026-07-26T00:00:00Z");
        const engineCalls = { propagations: 0 };
        const countedEngine = {
          json2satrec: (record) => sgp4Engine.json2satrec(record),
          sgp4(satrec, minutes) {
            engineCalls.propagations += 1;
            return sgp4Engine.sgp4(satrec, minutes);
          },
        };
        const parent = new THREE.Group();
        const layer = sky.createWorldSky({ THREE, parent, starCount: 32 });
        layer.update({
          ok: true,
          schemaVersion: 1,
          fetchedAt: NOW,
          satellites: [{
            OBJECT_NAME: "ISS (ZARYA)",
            OBJECT_ID: "1998-067A",
            EPOCH: "2026-07-26T00:00:00Z",
            MEAN_MOTION: 15.5,
            ECCENTRICITY: 0.0004,
            INCLINATION: 51.64,
            RA_OF_ASC_NODE: 123.4,
            ARG_OF_PERICENTER: 24.5,
            MEAN_ANOMALY: 335.5,
            NORAD_CAT_ID: "25544",
            BSTAR: 0.0002,
            MEAN_MOTION_DOT: 0.00001,
            MEAN_MOTION_DDOT: 0,
          }],
        }, countedEngine);

        const sample = (label) => ({
          ...layer.getState(),
          label,
          starsVisible: layer.stars.visible,
          satellitesVisible: layer.satellites.visible,
          planetCount: layer.planets.count,
          propagations: engineCalls.propagations,
          sunY: layer.planets.matrices[0].y,
          moonY: layer.planets.matrices[1].y,
        });

        // Noon, view level with the horizon.
        layer.setDaylightMinute(12 * 60, 1);
        layer.tick(NOW, { x: 0, y: 2, z: 0 }, { x: 0, y: 0, z: -1 });
        const noon = sample("noon");
        // Still daytime, but now craning at the sun.
        layer.tick(NOW + 5_000, null, { x: 0, y: 1, z: -0.2 });
        const noonLookingUp = sample("noonLookingUp");

        // Midnight, still looking at the horizon.
        layer.setDaylightMinute(0, 0);
        layer.tick(NOW + 10_000, null, { x: 0, y: 0, z: -1 });
        const midnight = sample("midnight");
        // Midnight, looking up.
        layer.tick(NOW + 20_000, null, { x: 0, y: 1, z: -0.2 });
        const midnightLookingUp = sample("midnightLookingUp");
        // Hysteresis: a shallow rise keeps the layer that is already up.
        layer.tick(NOW + 30_000, null, { x: 0, y: 0.09, z: -1 });
        const midnightShallowUp = sample("midnightShallowUp");
        // …and does not turn it on once it has dropped below the exit angle.
        layer.tick(NOW + 40_000, null, { x: 0, y: -0.5, z: -1 });
        layer.tick(NOW + 50_000, null, { x: 0, y: 0.09, z: -1 });
        const midnightShallowDown = sample("midnightShallowDown");

        process.stdout.write(JSON.stringify({
          samples: [
            noon,
            noonLookingUp,
            midnight,
            midnightLookingUp,
            midnightShallowUp,
            midnightShallowDown,
          ],
          sunColor: layer.planets.colors[0],
          moonColor: layer.planets.colors[1],
          planetCapacity: layer.planets.capacity,
          attached: parent.children.includes(layer.group),
        }));
        """
    )

    samples = {sample["label"]: sample for sample in result["samples"]}
    assert result["sunColor"] == "#ffd45f"
    assert result["moonColor"] == "#dff5ff"
    assert result["planetCapacity"] == 8
    assert result["attached"] is True
    # The two surviving instances are the ones that belong to their half of the
    # day: the sun is up at noon, the moon is up at midnight.
    assert samples["noon"]["sunY"] > 0 > samples["noon"]["moonY"]
    assert samples["midnight"]["moonY"] > 0 > samples["midnight"]["sunY"]

    # Daylight draws the sun and the moon and nothing else, whichever way the
    # visitor is facing.
    for label in ("noon", "noonLookingUp", "midnight"):
        assert samples[label]["starsVisible"] is False, label
        assert samples[label]["satellitesVisible"] is False, label
        assert samples[label]["planetCount"] == 2, label
        assert samples[label]["deepSkyVisible"] is False, label
        assert samples[label]["drawCalls"] == 2, label
    assert samples["noon"]["night"] is False
    assert samples["noonLookingUp"]["lookingUp"] is True
    assert samples["midnight"]["night"] is True
    assert samples["midnight"]["lookingUp"] is False

    # Looking up at a night sky is the only state that pays for the full layer.
    for label in ("midnightLookingUp", "midnightShallowUp"):
        assert samples[label]["starsVisible"] is True, label
        assert samples[label]["satellitesVisible"] is True, label
        assert samples[label]["planetCount"] == 8, label
        assert samples[label]["drawCalls"] == 4, label
    assert samples["midnightShallowDown"]["deepSkyVisible"] is False

    # SGP4 runs only while the satellites are on screen, and resumes on the
    # first tick after they return.
    assert samples["midnight"]["propagations"] == 0
    assert samples["midnightLookingUp"]["propagations"] == 1
    assert samples["midnightShallowUp"]["propagations"] == 2
    assert samples["midnightShallowDown"]["propagations"] == 2


def test_look_up_gate_has_hysteresis_and_ignores_malformed_directions():
    result = run_sky_script(
        """
        const check = (direction, previous) =>
          sky.worldSkyLookingUp(direction, previous);
        process.stdout.write(JSON.stringify({
          enter: sky.WORLD_SKY_LOOK_UP_ENTER,
          exit: sky.WORLD_SKY_LOOK_UP_EXIT,
          steepFromDown: check({ x: 0, y: 1, z: 0 }, false),
          levelFromDown: check({ x: 0, y: 0, z: -1 }, false),
          bandFromDown: check({ x: 0, y: 0.1, z: -1 }, false),
          bandFromUp: check({ x: 0, y: 0.1, z: -1 }, true),
          belowExitFromUp: check({ x: 0, y: 0.01, z: -1 }, true),
          // Length must not change the answer.
          scaled: check({ x: 0, y: 40, z: -8 }, false),
          zeroLength: check({ x: 0, y: 0, z: 0 }, true),
          missing: check(null, true),
          nonFinite: check({ x: 0, y: Number.NaN, z: -1 }, true),
          nonFiniteFromDown: check({ x: 0, y: Number.NaN, z: -1 }, false),
        }));
        """
    )

    assert result["exit"] < result["enter"]
    assert result["steepFromDown"] is True
    assert result["levelFromDown"] is False
    # Inside the band the previous answer wins, in both directions.
    assert result["bandFromDown"] is False
    assert result["bandFromUp"] is True
    assert result["belowExitFromUp"] is False
    assert result["scaled"] is True
    # Unusable input never flips the layer.
    assert result["zeroLength"] is True
    assert result["missing"] is True
    assert result["nonFinite"] is True
    assert result["nonFiniteFromDown"] is False


def test_environment_tint_updates_sky_bodies_without_stacking():
    result = run_sky_script(
        THREE_STUB
        + """
        const layer = sky.createWorldSky({ THREE, starCount: 8 });
        layer.setDaylightMinute(0, 0);
        const baseline = layer.planets.colors[0];
        const changed = layer.setEnvironmentTint({
          tint: "#5f7785",
          strength: 0.2,
          brightness: 0.88,
        });
        const rain = layer.planets.colors[0];
        const repeated = layer.setEnvironmentTint({
          tint: "#5f7785",
          strength: 0.2,
          brightness: 0.88,
        });
        const rainState = layer.getState();
        const reset = layer.setEnvironmentTint();
        process.stdout.write(JSON.stringify({
          baseline,
          rain,
          changed,
          repeated,
          reset,
          rainState,
          restored: layer.planets.colors[0],
          restoredStars: layer.stars.material.opacity,
        }));
        """
    )

    assert result["baseline"] == "#ffd45f"
    assert result["rain"] == "#dfc167"
    assert result["changed"] is True
    assert result["repeated"] is False
    assert result["reset"] is True
    assert result["rainState"]["environmentTint"] == "#5f7785"
    assert result["rainState"]["environmentTintStrength"] == 0.2
    assert result["rainState"]["environmentBrightness"] == 0.88
    assert result["restored"] == result["baseline"]
    assert result["restoredStars"] == 0.94


def test_seeded_star_field_is_deterministic_bounded_and_above_the_horizon():
    result = run_sky_script(
        """
        const first = sky.generateWorldStarField(64, 12345, 760);
        const repeated = sky.generateWorldStarField(64, 12345, 760);
        const changed = sky.generateWorldStarField(64, 54321, 760);
        const capped = sky.generateWorldStarField(999999, 12345, 760);
        let minimumRadius = Infinity;
        let maximumRadius = 0;
        let minimumY = Infinity;
        for (let index = 0; index < first.count; index += 1) {
          const offset = index * 3;
          const x = first.positions[offset];
          const y = first.positions[offset + 1];
          const z = first.positions[offset + 2];
          minimumRadius = Math.min(minimumRadius, Math.hypot(x, y, z));
          maximumRadius = Math.max(maximumRadius, Math.hypot(x, y, z));
          minimumY = Math.min(minimumY, y);
        }
        process.stdout.write(JSON.stringify({
          count: first.count,
          cappedCount: capped.count,
          repeated:
            Buffer.from(first.positions.buffer).equals(
              Buffer.from(repeated.positions.buffer),
            ) &&
            Buffer.from(first.colors.buffer).equals(
              Buffer.from(repeated.colors.buffer),
            ),
          changed:
            !Buffer.from(first.positions.buffer).equals(
              Buffer.from(changed.positions.buffer),
            ),
          typed:
            first.positions instanceof Float32Array &&
            first.colors instanceof Float32Array,
          minimumRadius,
          maximumRadius,
          minimumY,
        }));
        """
    )

    assert result["count"] == 64
    assert result["cappedCount"] == 1600
    assert result["repeated"] is True
    assert result["changed"] is True
    assert result["typed"] is True
    assert 760 * 0.964 < result["minimumRadius"] <= 760
    assert 760 * 0.964 < result["maximumRadius"] <= 760.001
    assert result["minimumY"] > 0


def test_omm_snapshot_validation_deduplicates_caps_and_fails_closed():
    result = run_sky_script(
        """
        const base = {
          OBJECT_NAME: "ISS (ZARYA)",
          OBJECT_ID: "1998-067A",
          EPOCH: "2026-07-26T12:00:00Z",
          MEAN_MOTION: 15.5,
          ECCENTRICITY: 0.0004,
          INCLINATION: 51.64,
          RA_OF_ASC_NODE: 123.4,
          ARG_OF_PERICENTER: 24.5,
          MEAN_ANOMALY: 335.5,
          NORAD_CAT_ID: "25544",
          BSTAR: 0.0002,
          MEAN_MOTION_DOT: 0.00001,
          MEAN_MOTION_DDOT: 0,
        };
        const records = [
          { ...base, NORAD_CAT_ID: "bad" },
          base,
          { ...base, OBJECT_NAME: "duplicate" },
        ];
        for (let id = 30000; id < 30180; id += 1) {
          records.push({
            ...base,
            OBJECT_NAME: `SAT ${id}`,
            NORAD_CAT_ID: String(id),
          });
        }
        const normalized = sky.normalizeWorldSatelliteSnapshot({
          ok: true,
          schemaVersion: 1,
          fetchedAt: 1800000000000,
          satellites: records,
        }, sky.WORLD_SKY_MAX_SATELLITES, sgp4Engine);
        process.stdout.write(JSON.stringify({
          count: normalized.length,
          firstId: normalized[0]?.catalogId,
          duplicateCount:
            normalized.filter((record) => record.catalogId === "25544").length,
          invalidTopLevel:
            sky.normalizeWorldSatelliteSnapshot(
              { satellites: records },
              sky.WORLD_SKY_MAX_SATELLITES,
              sgp4Engine,
            ).length,
          invalidRecord: sky.prepareWorldSatelliteOmm({
            ...base,
            ECCENTRICITY: 1,
          }, sgp4Engine),
          invalidInclination: sky.prepareWorldSatelliteOmm({
            ...base,
            INCLINATION: 181,
          }, sgp4Engine),
          invalidBstar: sky.prepareWorldSatelliteOmm({
            ...base,
            BSTAR: Number.NaN,
          }, sgp4Engine),
          missingEngine:
            sky.normalizeWorldSatelliteSnapshot({
              ok: true,
              schemaVersion: 1,
              satellites: [base],
            }).length,
          max: sky.WORLD_SKY_MAX_SATELLITES,
        }));
        """
    )

    assert result["count"] == result["max"] == 160
    assert result["firstId"] == "25544"
    assert result["duplicateCount"] == 1
    assert result["invalidTopLevel"] == 0
    assert result["invalidRecord"] is None
    assert result["invalidInclination"] is None
    assert result["invalidBstar"] is None
    assert result["missingEngine"] == 0


def test_sgp4_matches_published_celestrak_vanguard_reference_vectors():
    result = run_sky_script(
        """
        // CelesTrak AIAA-2006-6753 Rev 2, Appendix D/E, satellite 00005.
        // https://celestrak.org/publications/AIAA/2006-6753/
        let initializationCalls = 0;
        let propagationCalls = 0;
        const countedEngine = {
          json2satrec(record) {
            initializationCalls += 1;
            return sgp4Engine.json2satrec(record);
          },
          sgp4(satrec, minutesSinceEpoch) {
            propagationCalls += 1;
            return sgp4Engine.sgp4(satrec, minutesSinceEpoch);
          },
        };
        const prepared = sky.prepareWorldSatelliteOmm({
          OBJECT_NAME: "VANGUARD 1",
          OBJECT_ID: "1958-002B",
          EPOCH: "2000-06-27T18:50:19.733568Z",
          MEAN_MOTION: 10.82419157,
          ECCENTRICITY: 0.1859667,
          INCLINATION: 34.2682,
          RA_OF_ASC_NODE: 348.7242,
          ARG_OF_PERICENTER: 331.7664,
          MEAN_ANOMALY: 19.3264,
          NORAD_CAT_ID: "5",
          BSTAR: 0.000028098,
          MEAN_MOTION_DOT: 0.00000023,
          MEAN_MOTION_DDOT: 0,
        }, countedEngine);
        const epoch = Date.parse("2000-06-27T18:50:19.733568Z");
        const output = { x: -1, y: -1, z: -1 };
        const atEpoch = sky.propagateWorldSatelliteOmm(
          prepared,
          epoch,
          output,
        );
        const epochPosition = { ...output };
        const afterSixHours = sky.propagateWorldSatelliteOmm(
          prepared,
          epoch + 360 * 60 * 1000,
          output,
        );
        const sixHourPosition = { ...output };
        const stale = sky.propagateWorldSatelliteOmm(
          prepared,
          epoch + 15 * 24 * 60 * 60 * 1000,
          output,
        );
        process.stdout.write(JSON.stringify({
          atEpoch,
          afterSixHours,
          stale,
          epochPosition,
          sixHourPosition,
          initializationCalls,
          propagationCalls,
        }));
        """
    )

    assert result["atEpoch"] is True
    assert result["afterSixHours"] is True
    assert result["stale"] is False
    assert result["initializationCalls"] == 1
    assert result["propagationCalls"] == 2
    expected_epoch = {
        "x": 7022.46529266,
        "y": -1400.08296755,
        "z": 0.03995155,
    }
    expected_six_hours = {
        "x": -7154.03120202,
        "y": -3783.17682504,
        "z": -3536.19412294,
    }
    for axis, expected in expected_epoch.items():
        assert abs(result["epochPosition"][axis] - expected) < 1e-5
    for axis, expected in expected_six_hours.items():
        assert abs(result["sixHourPosition"][axis] - expected) < 1e-5


def test_sky_module_keeps_rendering_and_lifecycle_work_bounded():
    source = SKY_MODULE.read_text(encoding="utf-8")
    assert "export function createWorldSky(" in source
    assert "export function generateWorldStarField(" in source
    assert "export function normalizeWorldSatelliteSnapshot(" in source
    assert "export function propagateWorldSatelliteOmm(" in source
    assert "const stars = new THREE.Points(" in source
    assert source.count("new THREE.InstancedMesh(") == 3
    assert "satellites.instanceMatrix.setUsage?.(THREE.DynamicDrawUsage)" in source
    assert "now - lastSatelliteTick < safeTickInterval" in source
    assert "const propagated = { x: 0, y: 0, z: 0 };" in source
    assert "sgp4Engine.json2satrec(" in source
    assert "prepared.sgp4(" in source
    assert "solveEccentricAnomaly" not in source
    assert "EARTH_MU_KM3_S2" not in source
    assert "transform.updateMatrix();" in source
    # The sun and moon hold the leading instance slots so daylight can drop the
    # six planets by shortening `count` alone.
    assert "const sunInstanceIndex = 0;" in source
    assert "const moonInstanceIndex = 1;" in source
    assert "const planetInstanceOffset = moonInstanceIndex + 1;" in source
    assert "function setDaylightMinute(" in source
    assert "function applyDeepSkyVisibility(" in source
    assert "sunAndMoon: 2" in source
    assert "forkmesh-world-sun-moon-glows" in source
    assert "new THREE.RingGeometry(1.08, 1.62, 28)" in source
    assert "THREE.AdditiveBlending" in source
    for method in (
        "setDaylightMinute,",
        "setViewDirection,",
        "update,",
        "tick,",
        "dispose,",
        "getState,",
    ):
        assert method in source

    # Fetch ownership stays in world.js. This rendering module has no timers,
    # animation loop, or third-party request path of its own.
    for forbidden in (
        "fetch(",
        "XMLHttpRequest",
        "setInterval(",
        "setTimeout(",
        "requestAnimationFrame(",
        "Math.random(",
    ):
        assert forbidden not in source


def test_pinned_local_sgp4_bundle_and_license_are_integrity_locked():
    bundle = SGP4_MODULE.read_bytes()
    license_text = SGP4_LICENSE.read_text(encoding="utf-8")
    assert len(bundle) < 25_000
    assert hashlib.sha256(bundle).hexdigest() == (
        "e9b6d7c5b01611701fe567d97b498794"
        "1714e3476d38a2901bd9b43eba70ba23"
    )
    assert "satellite.js 7.1.0" in license_text
    assert "MIT License" in license_text
    assert "Copyright (C) 2013 Shashwat Kandadai" in license_text
    assert "sourceMappingURL" not in bundle.decode("utf-8")
    assert "http://" not in bundle.decode("utf-8")
    assert "https://" not in bundle.decode("utf-8")


def test_sgp4_bundle_load_is_local_deferred_and_not_world_boot_blocking():
    source = WORLD_APP.read_text(encoding="utf-8")
    index_source = (
        ROOT.parent / "world" / "public" / "world" / "index.html"
    ).read_text(encoding="utf-8")
    scene_source = world_scene_source()

    assert (
        'SATELLITE_SGP4_MODULE_URL =\n'
        '  "./vendor/satellite-js-7.1.0.esm.js";'
    ) in source
    assert "import(SATELLITE_SGP4_MODULE_URL)" in source
    assert 'from "./vendor/satellite-js' not in source
    assert "satellite-js-7.1.0.esm.js" not in index_source

    load_start = source.index("  async loadSatelliteSky() {")
    load_end = source.index("\n  async loadWorldData(", load_start)
    load_body = source[load_start:load_end]
    assert load_body.index(
        'await this.fetchJSON("/api/world/satellites"'
    ) < load_body.index("await loadSatelliteSgp4Module()")
    assert "snapshot?.ok !== true" in load_body
    assert "snapshot.satellites.length === 0" in load_body
    assert "updateSatelliteSky?.(snapshot, sgp4Engine)" in load_body
    assert (
        "updateSatelliteSky: (snapshot, sgp4Engine) =>"
        in scene_source
    )
    assert "worldSky.update(snapshot, sgp4Engine)" in scene_source


def test_celestial_glows_are_occluded_and_hidden_below_the_horizon():
    source = (Path(__file__).resolve().parents[1] / "public" / "world" / "world-sky.js").read_text(encoding="utf-8")
    glow = source.split("const celestialGlowMaterial", 1)[1].split("});", 1)[0]
    assert "depthTest: true," in glow
    assert "Math.sin(angle) > -0.02 ? scale : 0" in source


def test_sky_bodies_take_their_instance_colors_not_missing_vertex_colors():
    result = run_sky_script(
        THREE_STUB
        + """
        const parent = new THREE.Group();
        const layer = sky.createWorldSky({ THREE, parent, starCount: 8 });
        const all = [];
        const walk = (node) => { all.push(node); (node.children || []).forEach(walk); };
        walk(parent);
        const byName = Object.fromEntries(all.filter((n) => n.name).map((n) => [n.name, n]));
        const broken = all
          .filter((n) => n.material && n.material.vertexColors && !(n.geometry && n.geometry.attributes.color))
          .map((n) => n.name);
        const planets = byName["forkmesh-world-planets"];
        const glows = byName["forkmesh-world-sun-moon-glows"];
        console.log(JSON.stringify({
          broken,
          planetsVertexColors: Boolean(planets.material.vertexColors),
          glowsVertexColors: Boolean(glows.material.vertexColors),
          sun: planets.colors[0],
          moon: planets.colors[1],
        }));
        layer.dispose?.();
        """
    )
    assert result["broken"] == []
    assert result["planetsVertexColors"] is False
    assert result["glowsVertexColors"] is False
    assert result["sun"] is not None and result["moon"] is not None

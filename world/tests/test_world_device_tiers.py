#!/usr/bin/env python3
"""Device-tier detection, the adaptive quality ladder, and its live knobs.

The World has to be usable from a workstation down to a low-tier Android, so
every cost the renderer can lower hangs off one ordered tier table. These
contracts exercise the three pure decisions in Node (no GPU, no WebGL) and
then pin the wiring that connects them to the scene, the sky, and the panel
that reports which tier a visitor actually got.
"""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
SKY_MODULE = WORLD / "world-sky.js"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
SKY = SKY_MODULE.read_text(encoding="utf-8")

TIERS = ["desktop-high", "laptop-mid", "mobile-mid", "mobile-low"]


def run_scene_script(body):
    script = f"""
      import * as scene from {json.dumps(SCENE_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def run_sky_script(body):
    script = f"""
      import * as sky from {json.dumps(SKY_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


# Enough of the Three.js surface for createWorldSky() to build and for a draw
# range to be observable, with no GPU and no WebGL context.
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
  constructor() {
    this.attributes = {};
    this.disposed = false;
    this.drawRange = { start: 0, count: Infinity };
  }
  setAttribute(name, value) { this.attributes[name] = value; }
  setDrawRange(start, count) { this.drawRange = { start, count }; }
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


def test_tier_table_is_ordered_strongest_first_and_never_raises_a_cost():
    profiles = run_scene_script(
        """
        console.log(JSON.stringify({
          tiers: scene.WORLD_QUALITY_TIERS,
          profiles: scene.WORLD_QUALITY_TIERS.map(
            (tier) => scene.worldQualityProfile(tier),
          ),
          unknown: scene.worldQualityProfile("no-such-tier").tier,
        }));
        """
    )
    assert profiles["tiers"] == TIERS
    # An unrecognised tier resolves to the strongest profile rather than
    # throwing, so a bad ?quality= value cannot break the World.
    assert profiles["unknown"] == TIERS[0]
    # Every knob is a cost. Walking the table from strongest to weakest may
    # only ever lower them, or a "step down" could make a device slower.
    descending = (
        "pixelRatioCap",
        "pixelBudget",
        "canvasTextureScale",
        "starCount",
        "maxSatellites",
        "weatherParticles",
        "pointLightBudget",
        "lodDistanceScale",
    )
    for index, profile in enumerate(profiles["profiles"]):
        assert profile["tier"] == TIERS[index]
        assert profile["label"]
        assert profile["visualAnimationMs"] > 0
        if index == 0:
            continue
        stronger = profiles["profiles"][index - 1]
        for knob in descending:
            assert profile[knob] <= stronger[knob], (knob, profile["tier"])
        # A slower ambient-animation cadence is a larger interval.
        assert profile["visualAnimationMs"] >= stronger["visualAnimationMs"]
        # Antialiasing and shadows are booleans: once dropped, they stay off.
        assert profile["antialias"] <= stronger["antialias"]
        assert profile["shadows"] <= stronger["shadows"]
    # Only the two phone tiers take compact mode's reduced populations.
    assert [profile["compact"] for profile in profiles["profiles"]] == [
        False,
        False,
        True,
        True,
    ]


def test_detection_separates_four_device_classes_from_cheap_signals():
    observed = run_scene_script(
        """
        const cases = {
          workstation: { coarsePointer: false, cores: 16, memoryGb: 8,
            gpu: "ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 Direct3D11)" },
          integratedLaptop: { coarsePointer: false, cores: 8, memoryGb: 8,
            gpu: "ANGLE (Intel, Intel(R) UHD Graphics 620 Direct3D11)" },
          fourCoreLaptop: { coarsePointer: false, cores: 4, memoryGb: 8,
            gpu: "ANGLE (Apple, Apple M1)" },
          softwareRasteriser: { coarsePointer: false, cores: 16, memoryGb: 16,
            gpu: "Google SwiftShader" },
          flagshipPhone: { coarsePointer: true, cores: 8, memoryGb: 8,
            gpu: "Adreno (TM) 740" },
          smallMemoryPhone: { coarsePointer: true, cores: 8, memoryGb: 3,
            gpu: "Adreno (TM) 740" },
          oldMaliPhone: { coarsePointer: true, cores: 8, memoryGb: 6,
            gpu: "Mali-G52" },
          twoCorePhone: { coarsePointer: true, cores: 2 },
          saveDataDesktop: { coarsePointer: false, cores: 16, memoryGb: 16,
            gpu: "NVIDIA GeForce RTX 4090", saveData: true },
          crashedLastSession: { coarsePointer: false, cores: 16, memoryGb: 16,
            gpu: "NVIDIA GeForce RTX 4090", forceLowest: true },
          // A hardened browser withholds the GPU name and deviceMemory; the
          // pointer type is then the only signal that votes.
          withheldSignalsTouch: { coarsePointer: true },
          withheldSignalsMouse: { coarsePointer: false },
          nothingKnown: {},
        };
        const observed = {};
        for (const [name, input] of Object.entries(cases)) {
          observed[name] = scene.classifyWorldDeviceTier(input);
        }
        console.log(JSON.stringify(observed));
        """
    )
    assert observed == {
        "workstation": "desktop-high",
        "integratedLaptop": "laptop-mid",
        "fourCoreLaptop": "laptop-mid",
        # A software rasteriser is slower than any real GPU in the table,
        # whatever machine happens to be around it.
        "softwareRasteriser": "mobile-low",
        "flagshipPhone": "mobile-mid",
        "smallMemoryPhone": "mobile-low",
        "oldMaliPhone": "mobile-low",
        "twoCorePhone": "mobile-low",
        "saveDataDesktop": "laptop-mid",
        "crashedLastSession": "mobile-low",
        "withheldSignalsTouch": "mobile-mid",
        "withheldSignalsMouse": "desktop-high",
        "nothingKnown": "desktop-high",
    }


def test_resolution_falls_with_the_tier_and_never_exceeds_the_device():
    measured = run_scene_script(
        """
        const surfaces = {
          desktop1600x900at1: [1600, 900, 1],
          desktop4Kat1: [3840, 2160, 1],
          laptop1440x900at2: [1440, 900, 2],
          phone412x915at3: [412, 915, 3],
          phone360x740at3: [360, 740, 3.5],
        };
        const measured = {};
        for (const [name, [width, height, dpr]] of Object.entries(surfaces)) {
          measured[name] = scene.WORLD_QUALITY_TIERS.map((tier) =>
            Number(
              scene
                .worldRendererPixelRatio(
                  width,
                  height,
                  dpr,
                  scene.worldQualityProfile(tier),
                )
                .toFixed(4),
            ),
          );
        }
        console.log(JSON.stringify({
          measured,
          // A device that asks for less than any cap gets exactly what it
          // asked for: resolution scaling never invents pixels.
          modestRequest: scene.worldRendererPixelRatio(
            800,
            600,
            0.5,
            scene.worldQualityProfile("desktop-high"),
          ),
        }));
        """
    )
    for name, ratios in measured["measured"].items():
        assert ratios == sorted(ratios, reverse=True), (name, ratios)
        assert ratios[-1] < ratios[0], (name, ratios)
        assert all(ratio > 0 for ratio in ratios), (name, ratios)
    assert measured["modestRequest"] == 0.5
    # A low-tier phone shades under one device pixel per CSS pixel; that is
    # the single largest lever this scene has on that hardware.
    assert measured["measured"]["phone360x740at3"][-1] <= 0.75
    # The area budget bites before the density cap on an oversized surface.
    assert measured["measured"]["desktop4Kat1"][0] < 1


def test_the_ladder_steps_down_on_evidence_and_back_up_only_slowly():
    decisions = run_scene_script(
        """
        const call = (input) => scene.adaptiveQualityDecision(input);
        console.log(JSON.stringify({
          slowAndSettled: call({ fps: 22, tier: "desktop-high",
            bootTier: "desktop-high", msSinceChange: 9_000 }),
          slowButJustChanged: call({ fps: 22, tier: "desktop-high",
            bootTier: "desktop-high", msSinceChange: 1_000 }),
          twoStepsAlready: call({ fps: 18, tier: "mobile-mid",
            bootTier: "desktop-high", msSinceChange: 60_000 }),
          floorTier: call({ fps: 12, tier: "mobile-low",
            bootTier: "mobile-low", msSinceChange: 60_000 }),
          healthyAtBoot: call({ fps: 59, tier: "desktop-high",
            bootTier: "desktop-high", goodWindows: 9, msSinceChange: 90_000 }),
          recovering: call({ fps: 58, tier: "mobile-mid",
            bootTier: "laptop-mid", goodWindows: 2, msSinceChange: 40_000 }),
          recoveringTooSoon: call({ fps: 58, tier: "mobile-mid",
            bootTier: "laptop-mid", goodWindows: 2, msSinceChange: 12_000 }),
          recoveringTooFewWindows: call({ fps: 58, tier: "mobile-mid",
            bootTier: "laptop-mid", goodWindows: 0, msSinceChange: 90_000 }),
          previewCadenceHealthy: call({ fps: 29, targetFps: 30,
            tier: "laptop-mid", bootTier: "laptop-mid", msSinceChange: 9_000 }),
          previewCadenceSlow: call({ fps: 14, targetFps: 30,
            tier: "laptop-mid", bootTier: "laptop-mid", msSinceChange: 9_000 }),
          unknownTier: call({ fps: 5, tier: "potato", bootTier: "potato",
            msSinceChange: 60_000 }),
          zeroFps: call({ fps: 0, tier: "laptop-mid", bootTier: "laptop-mid",
            msSinceChange: 60_000 }),
        }));
        """
    )
    assert decisions["slowAndSettled"]["action"] == "downgrade"
    assert decisions["slowAndSettled"]["tier"] == "laptop-mid"
    # A window that starts right after a change measures the change, not the
    # device, so nothing moves until the world has settled.
    assert decisions["slowButJustChanged"]["action"] == "hold"
    # Two steps from the boot tier is as far as detection can be wrong before
    # dropping further would hide the problem instead of reporting it.
    assert decisions["twoStepsAlready"]["action"] == "hold"
    assert decisions["floorTier"]["action"] == "hold"
    # Nothing to recover to: the world is already at the tier it booted with.
    assert decisions["healthyAtBoot"]["action"] == "hold"
    assert decisions["recovering"]["action"] == "upgrade"
    assert decisions["recovering"]["tier"] == "laptop-mid"
    assert decisions["recoveringTooSoon"]["action"] == "hold"
    assert decisions["recoveringTooFewWindows"]["action"] == "hold"
    # The judgement is a ratio of the cadence the loop asked for, so the
    # marketing band's 30 Hz preview mode is not read as a slow device.
    assert decisions["previewCadenceHealthy"]["action"] == "hold"
    assert decisions["previewCadenceHealthy"]["good"] is True
    assert decisions["previewCadenceSlow"]["action"] == "downgrade"
    assert decisions["unknownTier"]["action"] == "hold"
    assert decisions["zeroFps"]["action"] == "downgrade"


def test_the_sky_lowers_drawn_stars_and_propagated_satellites_in_place():
    measured = run_sky_script(
        THREE_STUB
        + """
        const layer = sky.createWorldSky({
          THREE,
          parent: new THREE.Group(),
          starCount: 900,
          maxSatellites: 64,
        });
        const range = () => layer.stars.geometry.drawRange.count;
        const before = { ...layer.getState(), drawRange: range() };
        const lowered = {
          ...layer.setDetailBudget({ stars: 260, satellites: 24 }),
          drawRange: range(),
        };
        const restored = {
          ...layer.setDetailBudget({ stars: 900, satellites: 64 }),
          drawRange: range(),
        };
        // Neither buffer may be reallocated: the pool keeps its boot size so a
        // step back up needs no new snapshot and no new GPU allocation.
        const clamped = {
          ...layer.setDetailBudget({ stars: 5_000, satellites: 500 }),
          drawRange: range(),
          allocated: layer.satellites.capacity,
        };
        // An omitted field means "leave this alone", not "draw nothing".
        const untouched = {
          ...layer.setDetailBudget({}),
          drawRange: range(),
        };
        const shape = (state) => ({
          drawn: state.starsDrawn,
          budget: state.satelliteBudget,
          drawRange: state.drawRange,
          ...(state.allocated === undefined ? {} : { allocated: state.allocated }),
        });
        console.log(JSON.stringify({
          before: { stars: before.stars, ...shape(before) },
          lowered: shape(lowered),
          restored: shape(restored),
          clamped: shape(clamped),
          untouched: shape(untouched),
        }));
        """
    )
    assert measured["before"]["stars"] == 900
    assert measured["before"]["drawn"] == 900
    assert measured["before"]["budget"] == 64
    assert measured["lowered"] == {
        "drawn": 260,
        "budget": 24,
        "drawRange": 260,
    }
    assert measured["restored"] == {
        "drawn": 900,
        "budget": 64,
        "drawRange": 900,
    }
    # Asking for more than was allocated clamps instead of overrunning.
    assert measured["clamped"] == {
        "drawn": 900,
        "budget": 64,
        "drawRange": 900,
        "allocated": 64,
    }
    assert measured["untouched"] == {
        "drawn": 900,
        "budget": 64,
        "drawRange": 900,
    }


def test_boot_detection_runs_before_the_first_allocation():
    # The unmasked GPU name needs a live context, so it is read from a 1x1
    # probe that is handed straight back rather than from the scene's own
    # renderer, which does not exist yet when the tier has to be known.
    assert "function probeGpuRenderer()" in SCENE
    assert 'canvas.getContext("webgl2", { powerPreference: "low-power" })' in SCENE
    assert 'gl.getExtension?.("WEBGL_lose_context")?.loseContext?.()' in SCENE
    assert "const bootQualityTier = WORLD_QUALITY_PROFILES[qualityTier]" in SCENE
    assert "cores: win?.navigator?.hardwareConcurrency," in SCENE
    assert "memoryGb: win?.navigator?.deviceMemory," in SCENE
    assert "gpu: probeGpuRenderer()," in SCENE
    assert "saveData: win?.navigator?.connection?.saveData === true," in SCENE
    assert "forceLowest: forceCompactRenderer === true," in SCENE
    assert "const rendererPlan = planWorldRenderer({" in SCENE
    # Construction-time knobs read the boot tier and are never re-derived.
    assert "const compactRenderer = qualityProfile.compact;" in SCENE
    assert "antialias: !memoryConstrainedRenderer" in SCENE


def test_every_live_knob_is_re_applied_when_the_tier_changes():
    tier_change = SCENE[
        SCENE.index("  function applyQualityTier(") : SCENE.index(
            "  function sampleAdaptiveQuality("
        )
    ]
    assert "applyRendererResolution();" in tier_change
    assert "applyShadowQuality();" in tier_change
    assert "applyWeatherParticleBudget(qualityProfile.weatherParticles);" in tier_change
    assert "worldSky.setDetailBudget({" in tier_change
    assert "updateSceneLevelOfDetail(true);" in tier_change
    assert "nextVisualAnimationAt = 0;" in tier_change
    # A step up may never exceed what the boot tier allocated.
    assert "Math.min(bootProfile.starCount, qualityProfile.starCount)" in tier_change
    assert "bootProfile.maxSatellites," in tier_change
    # setSize cleared the drawing buffer, so a step must repaint rather than
    # leave the canvas blank until the next callback - and must not do it in
    # the middle of a touch gesture.
    assert "renderer.render(scene, camera);" in tier_change
    assert "primaryPointerId === null && touchPointers.size === 0" in tier_change
    # Shadows have three independent authorities and one derivation.
    shadows = SCENE[
        SCENE.index("  function applyShadowQuality(") : SCENE.index(
            "\n  // The World contains hundreds of casters"
        )
    ]
    assert "qualityProfile.shadows && shadowsElementEnabled && !shadowsFarCamera" in (
        shadows
    )
    # Only the drawn range of the weather buffer is integrated each frame.
    assert "const range = points.geometry.drawRange?.count;" in SCENE
    assert "for (let index = 0; index < drawn; index += 1) {" in SCENE
    assert "weather.rain.geometry.setDrawRange(0, budget);" in SCENE


def test_the_ladder_only_judges_windows_the_loop_actually_presented():
    sampler = SCENE[
        SCENE.index("  function sampleAdaptiveQuality(") : SCENE.index(
            "  function qualityState("
        )
    ]
    assert "if (elapsedMs < QUALITY_SAMPLE_WINDOW_MS) return;" in sampler
    assert "if (elapsedMs > QUALITY_SAMPLE_WINDOW_MS * 3) {" in sampler
    assert "targetFps: 1000 / renderFrameIntervalMs," in sampler
    assert "bootTier: qualityBaseTier," in sampler
    # A pause and a cadence change both invalidate the window in flight.
    paused = SCENE[
        SCENE.index("  function setPaused(") : SCENE.index(
            "  function setPreviewCadence("
        )
    ]
    assert "qualitySampleStartedAt = 0;" in paused
    cadence = SCENE[
        SCENE.index("  function setPreviewCadence(") : SCENE.index(
            "  function rendererContextInfo("
        )
    ]
    assert "qualitySampleStartedAt = 0;" in cadence
    # The render loop is already paused while the canvas is off-screen or the
    # tab is hidden, so an idle world costs nothing on any tier.
    assert "this.embedViewportObserver = new IntersectionObserver(" in APP
    assert "this.world?.setPaused?.(paused);" in APP
    assert 'document.addEventListener("visibilitychange", this.handleVisibility);' in APP


def test_the_tier_is_reportable_overridable_and_recorded_when_a_tab_dies():
    # QA drives the supported matrix from one machine, and support can hand a
    # visitor the same link when detection got their device wrong.
    assert 'worldQuery.get("quality")' in APP
    assert 'this.adaptiveQualityRequested = worldQuery.get("adaptive") !== "0";' in APP
    assert "qualityTier: this.requestedQualityTier," in APP
    assert "adaptiveQuality: this.adaptiveQualityRequested," in APP
    assert "setQualityTier," in SCENE
    assert "getQualityState: qualityState," in SCENE
    assert "quality: qualityState()," in SCENE
    # The panel says which tier the visitor got, whether it was detected or
    # pinned, and every step the ladder has taken since boot.
    assert "data-world-debug-device-tier" in APP
    assert '"device-tier": readouts.quality,' in APP
    assert "quality: scene?.quality" in APP
    # A tab that died at desktop-high on a phone says something different from
    # one that died after the ladder had already stepped twice.
    assert "qualityBootTier: String(quality?.bootTier" in APP
    assert "quality tier ${String(record.qualityTier" in APP

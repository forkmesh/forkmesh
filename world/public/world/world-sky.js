import * as resources from "./scene/resources.js";

// A dependency-injected, network-free sky layer for the World scene.
//
// The caller owns fetching the bounded /api/world/satellites snapshot and
// injects the deferred, same-origin SGP4 engine only after that snapshot is
// valid. This module initializes each compact OMM record once, then propagates
// it locally between refreshes. Stars, planets, and satellites use three total
// draw calls: one Points object and two InstancedMesh objects, and all three
// are gated on a night sky being looked at — the daytime or horizon-level view
// draws only the sun, the moon, and their shared glow ring.

export const WORLD_SKY_SEED = 0x464d534b;
export const WORLD_SKY_MAX_STARS = 1600;
export const WORLD_SKY_DEFAULT_STARS = 1100;
export const WORLD_SKY_MAX_SATELLITES = 160;
export const WORLD_SKY_RADIUS = 760;
export const WORLD_SKY_SATELLITE_RADIUS = 690;
export const WORLD_SKY_TICK_MS = 1000;
export const WORLD_SKY_MAX_PROPAGATION_DAYS = 14;
// Deep-sky detail (stars, planets, satellites) is only worth its triangles and
// its SGP4 propagation while the view is actually tilted upward. The enter and
// exit thresholds are the sine of the view pitch, and the gap between them
// keeps a camera resting near the horizon from flickering the layer on and off.
export const WORLD_SKY_LOOK_UP_ENTER = 0.14;
export const WORLD_SKY_LOOK_UP_EXIT = 0.045;
// Above this daylight strength the sky is simply blue: the deep-sky layer is
// hidden and only the sun (and the moon, which is below the horizon anyway)
// keeps its instance.
export const WORLD_SKY_NIGHT_DAYLIGHT_MAX = 0.35;

const TAU = Math.PI * 2;
const DAY_MS = 86_400_000;
const MINUTES_PER_DAY = 1_440;
const EARTH_RADIUS_KM = 6_378.137;
const OMM_CATALOG_PATTERN = /^[1-9][0-9]{0,8}$/;

// These are deliberately stable visual anchors, not an astronomical ephemeris.
// The public OMM snapshot drives the moving satellite layer independently.
export const WORLD_SKY_PLANETS = Object.freeze([
  Object.freeze({
    id: "mercury",
    color: "#b9b3a8",
    azimuth: 0.12,
    elevation: 0.25,
    size: 4.5,
  }),
  Object.freeze({
    id: "venus",
    color: "#ffe0a3",
    azimuth: 0.28,
    elevation: 0.43,
    size: 7.5,
  }),
  Object.freeze({
    id: "mars",
    color: "#f37b5d",
    azimuth: 0.45,
    elevation: 0.3,
    size: 6.2,
  }),
  Object.freeze({
    id: "jupiter",
    color: "#e8c59d",
    azimuth: 0.61,
    elevation: 0.5,
    size: 13,
  }),
  Object.freeze({
    id: "saturn",
    color: "#e7d28e",
    azimuth: 0.77,
    elevation: 0.35,
    size: 10.5,
  }),
  Object.freeze({
    id: "neptune",
    color: "#719bff",
    azimuth: 0.92,
    elevation: 0.56,
    size: 8,
  }),
]);

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

function finiteNumber(value) {
  if (typeof value === "boolean") return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function boundedNumber(value, minimum, maximum, maximumExclusive = false) {
  const number = finiteNumber(value);
  if (
    number === null ||
    number < minimum ||
    (maximumExclusive ? number >= maximum : number > maximum)
  ) {
    return null;
  }
  return number;
}

function xorshift32(state) {
  let next = state | 0;
  next ^= next << 13;
  next ^= next >>> 17;
  next ^= next << 5;
  return next | 0;
}

function seededUnit(state) {
  const next = xorshift32(state);
  return [next, (next >>> 0) / 4_294_967_296];
}

/**
 * Build deterministic typed star attributes once at scene construction time.
 * No ambient randomness, DOM state, clock, or network input participates.
 */
export function generateWorldStarField(
  requestedCount = WORLD_SKY_DEFAULT_STARS,
  seed = WORLD_SKY_SEED,
  radius = WORLD_SKY_RADIUS,
) {
  const count = clamp(
    Math.trunc(finiteNumber(requestedCount) ?? WORLD_SKY_DEFAULT_STARS),
    0,
    WORLD_SKY_MAX_STARS,
  );
  const safeRadius = clamp(
    finiteNumber(radius) ?? WORLD_SKY_RADIUS,
    100,
    10_000,
  );
  const positions = new Float32Array(count * 3);
  const colors = new Float32Array(count * 3);
  let state = (Math.trunc(finiteNumber(seed) ?? WORLD_SKY_SEED) | 0) || 1;

  for (let index = 0; index < count; index += 1) {
    let random;
    [state, random] = seededUnit(state);
    const y = 0.04 + random * 0.96;
    [state, random] = seededUnit(state);
    const theta = random * TAU;
    [state, random] = seededUnit(state);
    const shell = safeRadius * (0.965 + random * 0.035);
    const horizontal = Math.sqrt(Math.max(0, 1 - y * y));
    const offset = index * 3;
    positions[offset] = Math.cos(theta) * horizontal * shell;
    positions[offset + 1] = y * shell;
    positions[offset + 2] = Math.sin(theta) * horizontal * shell;

    [state, random] = seededUnit(state);
    const brightness = 0.58 + Math.pow(random, 2.4) * 0.42;
    [state, random] = seededUnit(state);
    if (random < 0.14) {
      colors[offset] = brightness;
      colors[offset + 1] = brightness * 0.82;
      colors[offset + 2] = brightness * 0.68;
    } else if (random > 0.83) {
      colors[offset] = brightness * 0.7;
      colors[offset + 1] = brightness * 0.84;
      colors[offset + 2] = brightness;
    } else {
      colors[offset] = brightness * 0.92;
      colors[offset + 1] = brightness * 0.96;
      colors[offset + 2] = brightness;
    }
  }
  return { count, positions, colors };
}

/**
 * Decide whether a view direction counts as "looking up", with hysteresis so a
 * camera hovering on the threshold does not toggle the deep-sky layer every
 * frame. A malformed or zero-length direction keeps the previous answer.
 */
export function worldSkyLookingUp(direction, previous = false) {
  const wasLookingUp = previous === true;
  const x = finiteNumber(direction?.x);
  const y = finiteNumber(direction?.y);
  const z = finiteNumber(direction?.z);
  if (x === null || y === null || z === null) return wasLookingUp;
  const length = Math.hypot(x, y, z);
  if (!(length > 0)) return wasLookingUp;
  const rise = y / length;
  return wasLookingUp
    ? rise > WORLD_SKY_LOOK_UP_EXIT
    : rise > WORLD_SKY_LOOK_UP_ENTER;
}

/**
 * Validate and initialize one compact CelesTrak OMM record. The expensive
 * json2satrec conversion happens only when a new snapshot arrives, never in
 * the animation tick.
 */
export function prepareWorldSatelliteOmm(record, sgp4Engine) {
  if (!record || typeof record !== "object" || Array.isArray(record)) {
    return null;
  }
  if (
    typeof sgp4Engine?.json2satrec !== "function" ||
    typeof sgp4Engine?.sgp4 !== "function"
  ) {
    return null;
  }
  const catalogId = String(record.NORAD_CAT_ID ?? "").trim();
  const epochMs = Date.parse(String(record.EPOCH || ""));
  const meanMotion = boundedNumber(record.MEAN_MOTION, 0.000001, 20);
  const eccentricity = boundedNumber(
    record.ECCENTRICITY,
    0,
    1,
    true,
  );
  const inclination = boundedNumber(record.INCLINATION, 0, 180);
  const ascendingNode = boundedNumber(record.RA_OF_ASC_NODE, 0, 360);
  const argumentOfPericenter = boundedNumber(
    record.ARG_OF_PERICENTER,
    0,
    360,
  );
  const meanAnomaly = boundedNumber(record.MEAN_ANOMALY, 0, 360);
  const bstar = boundedNumber(record.BSTAR, -100, 100);
  const meanMotionDot = boundedNumber(
    record.MEAN_MOTION_DOT ?? 0,
    -10,
    10,
  );
  const meanMotionDdot = boundedNumber(
    record.MEAN_MOTION_DDOT ?? 0,
    -10,
    10,
  );
  if (
    !OMM_CATALOG_PATTERN.test(catalogId) ||
    !Number.isFinite(epochMs) ||
    meanMotion === null ||
    eccentricity === null ||
    inclination === null ||
    ascendingNode === null ||
    argumentOfPericenter === null ||
    meanAnomaly === null ||
    bstar === null ||
    meanMotionDot === null ||
    meanMotionDdot === null
  ) {
    return null;
  }

  const name = String(record.OBJECT_NAME || catalogId).trim().slice(0, 80);
  let satrec;
  try {
    satrec = sgp4Engine.json2satrec({
      OBJECT_NAME: name,
      OBJECT_ID: String(record.OBJECT_ID || "").trim().slice(0, 32),
      EPOCH: new Date(epochMs).toISOString(),
      MEAN_MOTION: meanMotion,
      ECCENTRICITY: eccentricity,
      INCLINATION: inclination,
      RA_OF_ASC_NODE: ascendingNode,
      ARG_OF_PERICENTER: argumentOfPericenter,
      MEAN_ANOMALY: meanAnomaly,
      NORAD_CAT_ID: catalogId,
      BSTAR: bstar,
      MEAN_MOTION_DOT: meanMotionDot,
      MEAN_MOTION_DDOT: meanMotionDdot,
    });
  } catch (_) {
    return null;
  }
  if (!satrec || Number(satrec.error) !== 0) return null;

  return {
    catalogId,
    name,
    epochMs,
    satrec,
    sgp4: sgp4Engine.sgp4,
  };
}

/**
 * Fail closed on malformed snapshots, deduplicate catalog ids, and cap work.
 */
export function normalizeWorldSatelliteSnapshot(
  snapshot,
  requestedLimit = WORLD_SKY_MAX_SATELLITES,
  sgp4Engine = null,
) {
  const limit = clamp(
    Math.trunc(finiteNumber(requestedLimit) ?? WORLD_SKY_MAX_SATELLITES),
    0,
    WORLD_SKY_MAX_SATELLITES,
  );
  if (
    !snapshot ||
    snapshot.ok !== true ||
    snapshot.schemaVersion !== 1 ||
    !Array.isArray(snapshot.satellites) ||
    typeof sgp4Engine?.json2satrec !== "function" ||
    typeof sgp4Engine?.sgp4 !== "function" ||
    limit === 0
  ) {
    return [];
  }
  const normalized = [];
  const seen = new Set();
  // A hostile direct caller cannot make this browser helper scan an unbounded
  // array even if it bypasses the Worker's stricter response validation.
  const scanLimit = Math.min(snapshot.satellites.length, limit * 2);
  for (
    let index = 0;
    index < scanLimit && normalized.length < limit;
    index += 1
  ) {
    const prepared = prepareWorldSatelliteOmm(
      snapshot.satellites[index],
      sgp4Engine,
    );
    if (!prepared || seen.has(prepared.catalogId)) continue;
    seen.add(prepared.catalogId);
    normalized.push(prepared);
  }
  return normalized;
}

/**
 * Propagate one initialized OMM record into TEME kilometres with SGP4.
 *
 * The public CelesTrak mean elements are built for SGP4. This remains a
 * bounded visualization rather than a flight-dynamics or conjunction-analysis
 * product, and stale records fail closed.
 */
export function propagateWorldSatelliteOmm(prepared, timestampMs, out) {
  const target = out || { x: 0, y: 0, z: 0 };
  const now = finiteNumber(timestampMs);
  if (!prepared || now === null) return false;
  const elapsedDays = (now - prepared.epochMs) / DAY_MS;
  if (
    !Number.isFinite(elapsedDays) ||
    Math.abs(elapsedDays) > WORLD_SKY_MAX_PROPAGATION_DAYS
  ) {
    return false;
  }

  if (
    typeof prepared.sgp4 !== "function" ||
    !prepared.satrec
  ) {
    return false;
  }
  let state;
  try {
    state = prepared.sgp4(
      prepared.satrec,
      elapsedDays * MINUTES_PER_DAY,
    );
  } catch (_) {
    return false;
  }
  const position = state?.position;
  if (
    !position ||
    Number(prepared.satrec.error) !== 0
  ) {
    return false;
  }
  target.x = Number(position.x);
  target.y = Number(position.y);
  target.z = Number(position.z);
  return (
    Number.isFinite(target.x) &&
    Number.isFinite(target.y) &&
    Number.isFinite(target.z)
  );
}

// Blend two "#rrggbb" strings and return a third. The sky bodies are drawn
// with toneMapped/fog disabled so they stay legible against the background,
// which also means the renderer exposure and the scene lights a personal
// environment preset dims never reach them. Mixing the preset's tint into the
// colours here is what lets a rain, snow, or winter sky read as one.
export function mixWorldSkyColor(base, tint, strength) {
  const from = parseHexColor(base);
  const to = parseHexColor(tint);
  const amount = clamp(finiteNumber(strength) ?? 0, 0, 1);
  if (!from || !to || amount <= 0) return base;
  const channel = (index) =>
    Math.round(from[index] + (to[index] - from[index]) * amount)
      .toString(16)
      .padStart(2, "0");
  return `#${channel(0)}${channel(1)}${channel(2)}`;
}

function parseHexColor(value) {
  const match = /^#([0-9a-f]{6})$/i.exec(String(value || "").trim());
  if (!match) return null;
  const packed = Number.parseInt(match[1], 16);
  return [(packed >> 16) & 255, (packed >> 8) & 255, packed & 255];
}

function disposeMaterial(material) {
  if (Array.isArray(material)) {
    material.forEach((entry) => entry?.dispose?.());
  } else {
    material?.dispose?.();
  }
}

/**
 * Create a three-draw-call sky. Pass the scene (or a world group) as parent.
 *
 * API:
 *   update(snapshot, sgp4Engine)     accept a bounded OMM response
 *   tick(epochMs, cameraPosition, viewDirection)
 *                                    move sky origin, gate the deep-sky layer
 *                                    on the view, and advance satellites
 *   setViewDirection(direction)      gate the deep-sky layer on its own
 *   setDetailBudget({stars, satellites})
 *                                    lower or restore drawn stars and
 *                                    propagated satellites without
 *                                    reallocating either buffer
 *   setEnvironmentTint({ tint, strength, brightness })
 *                                    follow a personal environment preset
 *   dispose()                        remove and release GPU resources
 */
export function createWorldSky({
  THREE,
  parent = null,
  compact = false,
  seed = WORLD_SKY_SEED,
  starCount = WORLD_SKY_DEFAULT_STARS,
  maxSatellites = WORLD_SKY_MAX_SATELLITES,
  radius = WORLD_SKY_RADIUS,
  satelliteRadius = WORLD_SKY_SATELLITE_RADIUS,
  tickIntervalMs = WORLD_SKY_TICK_MS,
} = {}) {
  if (
    !THREE?.Group ||
    !THREE?.BufferGeometry ||
    !THREE?.BufferAttribute ||
    !THREE?.Points ||
    !THREE?.PointsMaterial ||
    !THREE?.InstancedMesh ||
    !THREE?.MeshBasicMaterial ||
    !THREE?.SphereGeometry ||
    !THREE?.RingGeometry ||
    !THREE?.OctahedronGeometry ||
    !THREE?.Object3D ||
    !THREE?.Color
  ) {
    throw new TypeError("createWorldSky requires a compatible Three.js module");
  }

  const safeRadius = clamp(finiteNumber(radius) ?? WORLD_SKY_RADIUS, 300, 950);
  const safeSatelliteRadius = clamp(
    finiteNumber(satelliteRadius) ?? WORLD_SKY_SATELLITE_RADIUS,
    250,
    safeRadius - 20,
  );
  const safeSatelliteLimit = clamp(
    Math.trunc(finiteNumber(maxSatellites) ?? WORLD_SKY_MAX_SATELLITES),
    1,
    WORLD_SKY_MAX_SATELLITES,
  );
  const safeTickInterval = clamp(
    Math.trunc(finiteNumber(tickIntervalMs) ?? WORLD_SKY_TICK_MS),
    250,
    60_000,
  );

  const group = new THREE.Group();
  group.name = "forkmesh-world-sky";
  group.frustumCulled = false;

  const starsData = generateWorldStarField(starCount, seed, safeRadius);
  const starGeometry = new THREE.BufferGeometry();
  starGeometry.setAttribute(
    "position",
    new THREE.BufferAttribute(starsData.positions, 3),
  );
  starGeometry.setAttribute(
    "color",
    new THREE.BufferAttribute(starsData.colors, 3),
  );
  const starMaterial = new THREE.PointsMaterial({
    size: compact ? 1 : 1.35,
    sizeAttenuation: false,
    vertexColors: true,
    transparent: true,
    opacity: 0.94,
    depthWrite: false,
    toneMapped: false,
    fog: false,
  });
  const stars = new THREE.Points(starGeometry, starMaterial);
  stars.name = "forkmesh-world-stars";
  stars.renderOrder = -30;
  group.add(stars);

  const planetGeometry = resources.primitive(
    THREE,
    "SphereGeometry",
    1,
    compact ? 8 : 12,
    compact ? 6 : 8,
  );
  const planetMaterial = new THREE.MeshBasicMaterial({
    toneMapped: false,
    fog: false,
  });
  // The sun and moon share the existing planet draw call. They move with
  // local civil time, but do not add two more meshes to every frame. They take
  // the first two instance slots so the daytime sky can drop the planets by
  // shortening `count` instead of paying for six invisible spheres.
  const sunInstanceIndex = 0;
  const moonInstanceIndex = 1;
  const planetInstanceOffset = moonInstanceIndex + 1;
  const planetInstanceCount = planetInstanceOffset + WORLD_SKY_PLANETS.length;
  const planets = new THREE.InstancedMesh(
    planetGeometry,
    planetMaterial,
    planetInstanceCount,
  );
  planets.name = "forkmesh-world-planets";
  planets.boundingSphere = new THREE.Sphere(new THREE.Vector3(), safeRadius * 1.25);
  planets.renderOrder = -20;

  const transform = new THREE.Object3D();
  const color = new THREE.Color();
  // The active personal environment preset, as a colour to blend the sky
  // bodies toward and a factor to scale the star field's opacity by. "world"
  // (and every unrecognised value) leaves the sky exactly as it was.
  let environmentTint = "";
  let environmentTintStrength = 0;
  let environmentBrightness = 1;

  function tintedSkyColor(hex) {
    return environmentTintStrength > 0
      ? mixWorldSkyColor(hex, environmentTint, environmentTintStrength)
      : hex;
  }

  function applyPlanetColors() {
    WORLD_SKY_PLANETS.forEach((planet, index) => {
      color.set(tintedSkyColor(planet.color));
      planets.setColorAt(planetInstanceOffset + index, color);
    });
    if (planets.instanceColor) planets.instanceColor.needsUpdate = true;
  }

  WORLD_SKY_PLANETS.forEach((planet, index) => {
    const azimuth = planet.azimuth * TAU;
    const elevation = planet.elevation * (Math.PI / 2);
    const horizontal = Math.cos(elevation);
    const planetRadius = safeRadius - 45 - index * 2;
    transform.position.set(
      Math.cos(azimuth) * horizontal * planetRadius,
      Math.sin(elevation) * planetRadius,
      Math.sin(azimuth) * horizontal * planetRadius,
    );
    transform.scale.setScalar(planet.size);
    transform.updateMatrix();
    planets.setMatrixAt(planetInstanceOffset + index, transform.matrix);
  });
  applyPlanetColors();
  planets.instanceMatrix.needsUpdate = true;
  group.add(planets);

  // One instanced, additive draw gives both bodies a readable corona without
  // introducing per-frame canvas work or one mesh per body. The warm outer
  // ring makes the sun read as a sun; the cool ring keeps the moon bright
  // against the night sky.
  const celestialGlowGeometry = new THREE.RingGeometry(1.08, 1.62, 28);
  const celestialGlowMaterial = new THREE.MeshBasicMaterial({
    transparent: true,
    opacity: 0.72,
    depthTest: true,
    depthWrite: false,
    toneMapped: false,
    fog: false,
    side: THREE.DoubleSide,
    forceSinglePass: true,
    blending: THREE.AdditiveBlending,
  });
  const celestialGlows = new THREE.InstancedMesh(
    celestialGlowGeometry,
    celestialGlowMaterial,
    2,
  );
  celestialGlows.name = "forkmesh-world-sun-moon-glows";
  celestialGlows.boundingSphere = new THREE.Sphere(new THREE.Vector3(), safeRadius * 1.25);
  celestialGlows.renderOrder = -19;
  group.add(celestialGlows);

  // Until a caller passes a view direction the sky behaves as it always did.
  let lookingUp = true;
  let nightStrength = 0;
  // Matches the freshly constructed Three.js objects; the construction-time
  // setDaylightMinute() call below reconciles them with the real time of day.
  let deepSkyVisible = true;
  let deepSkyStale = true;

  // Stars, planets, and satellites are the expensive half of this layer, and
  // they are only legible at night with the view tilted upward. Gate all three
  // on that so the daytime and horizon-level World pays for the sun, the moon,
  // and their shared glow ring — nothing else.
  function applyDeepSkyVisibility() {
    const visible = lookingUp && nightStrength > 0;
    if (visible === deepSkyVisible) return;
    deepSkyVisible = visible;
    stars.visible = visible;
    satellites.visible = visible;
    planets.count = visible ? planetInstanceCount : planetInstanceOffset;
    // Orbits kept moving while nothing was propagating them, so the first tick
    // after the layer returns re-propagates instead of showing a stale frame.
    if (visible) deepSkyStale = true;
  }

  let lastDaylightMinute = 12 * 60;
  let lastDaylightStrength = 1;

  function setDaylightMinute(value, daylightStrength = null) {
    lastDaylightMinute = value;
    lastDaylightStrength = daylightStrength;
    const minute = (
      (finiteNumber(value) ?? 12 * 60) % (24 * 60) +
      24 * 60
    ) % (24 * 60);
    const solarAngle = (minute / (24 * 60)) * TAU - Math.PI / 2;
    const solarRadius = safeRadius - 105;
    const horizontalRadius = solarRadius * 0.76;
    const verticalRadius = solarRadius * 0.62;
    const depth = solarRadius * 0.38;
    const setBody = (instanceIndex, angle, scale, bodyColor) => {
      transform.position.set(
        Math.cos(angle) * horizontalRadius,
        Math.sin(angle) * verticalRadius,
        -Math.cos(angle) * depth,
      );
      transform.scale.setScalar(scale);
      transform.updateMatrix();
      planets.setMatrixAt(instanceIndex, transform.matrix);
      color.set(tintedSkyColor(bodyColor));
      planets.setColorAt(instanceIndex, color);
      transform.lookAt(0, 0, 0);
      transform.scale.setScalar(Math.sin(angle) > -0.02 ? scale : 0);
      transform.updateMatrix();
      celestialGlows.setMatrixAt(
        instanceIndex === sunInstanceIndex ? 0 : 1,
        transform.matrix,
      );
      celestialGlows.setColorAt(
        instanceIndex === sunInstanceIndex ? 0 : 1,
        color,
      );
    };
    setBody(
      sunInstanceIndex,
      solarAngle,
      compact ? 13 : 18,
      "#ffd45f",
    );
    setBody(
      moonInstanceIndex,
      solarAngle + Math.PI,
      compact ? 9 : 12,
      "#dff5ff",
    );
    const visibleDaylight = clamp(
      finiteNumber(daylightStrength) ??
        Math.max(0, Math.sin(solarAngle)),
      0,
      1,
    );
    // Fade the stars out exactly as dawn reaches the cutoff that hides them, so
    // the layer switching off is never a visible pop.
    nightStrength = clamp(
      1 - visibleDaylight / WORLD_SKY_NIGHT_DAYLIGHT_MAX,
      0,
      1,
    );
    starMaterial.opacity = nightStrength * 0.94 * environmentBrightness;
    applyDeepSkyVisibility();
    planets.instanceMatrix.needsUpdate = true;
    if (planets.instanceColor) planets.instanceColor.needsUpdate = true;
    celestialGlows.instanceMatrix.needsUpdate = true;
    if (celestialGlows.instanceColor) {
      celestialGlows.instanceColor.needsUpdate = true;
    }
    return minute;
  }

  const satelliteGeometry = new THREE.OctahedronGeometry(
    compact ? 0.72 : 0.9,
    0,
  );
  const satelliteMaterial = new THREE.MeshBasicMaterial({
    color: "#b8ffe0",
    toneMapped: false,
    fog: false,
  });
  const satellites = new THREE.InstancedMesh(
    satelliteGeometry,
    satelliteMaterial,
    safeSatelliteLimit,
  );
  satellites.name = "forkmesh-world-satellites";
  satellites.count = 0;
  satellites.boundingSphere = new THREE.Sphere(new THREE.Vector3(), safeRadius * 1.25);
  satellites.renderOrder = -10;
  satellites.instanceMatrix.setUsage?.(THREE.DynamicDrawUsage);
  group.add(satellites);

  // Runs only now that every gated object exists, so the very first frame is
  // already in the correct day/night state instead of flashing a full sky.
  setDaylightMinute(12 * 60, 1);

  if (parent?.add) parent.add(group);

  let records = [];
  let disposed = false;
  let lastSatelliteTick = -Infinity;
  let acceptedAt = 0;
  let sourceEpoch = "";
  // Live detail budget. The star buffer and the satellite instance pool keep
  // the size the boot device tier asked for; lowering these draws a prefix of
  // the stars and propagates a prefix of the tracked satellites instead, so a
  // quality step down is two integer writes rather than a reallocation, and a
  // step back up needs no new snapshot fetch.
  let starDrawCount = starsData.count;
  let satelliteBudget = safeSatelliteLimit;
  // One output object and one Object3D are reused for all instances and ticks.
  const propagated = { x: 0, y: 0, z: 0 };

  function activeSatelliteCount() {
    return Math.min(records.length, satelliteBudget);
  }

  /**
   * Lower - or restore, never past the allocated maxima - the drawn star count
   * and the number of satellites propagated per tick. Omitted fields are left
   * as they are and out-of-range numbers clamp, because the caller is a live
   * quality ladder reacting to frame rate, not validated input.
   */
  function setDetailBudget({ stars = null, satellites: satelliteLimit = null } = {}) {
    if (disposed) return getState();
    // Number(null) is 0, so an omitted field has to be recognised before it is
    // coerced or `setDetailBudget({})` would draw no stars at all.
    const nextStars = stars === null || stars === undefined
      ? null
      : finiteNumber(stars);
    if (nextStars !== null) {
      starDrawCount = clamp(Math.trunc(nextStars), 0, starsData.count);
      starGeometry.setDrawRange(0, starDrawCount);
    }
    const nextSatellites =
      satelliteLimit === null || satelliteLimit === undefined
        ? null
        : finiteNumber(satelliteLimit);
    if (nextSatellites !== null) {
      satelliteBudget = clamp(
        Math.trunc(nextSatellites),
        0,
        safeSatelliteLimit,
      );
      satellites.count = activeSatelliteCount();
      // Instances inside a newly widened prefix still hold matrices from an
      // older tick, so the next tick must run rather than wait out the
      // interval and leave stale positions on screen.
      deepSkyStale = true;
    }
    return getState();
  }

  function update(snapshot, sgp4Engine) {
    if (disposed) return 0;
    records = normalizeWorldSatelliteSnapshot(
      snapshot,
      safeSatelliteLimit,
      sgp4Engine,
    );
    satellites.count = activeSatelliteCount();
    const accepted = snapshot?.ok === true && snapshot?.schemaVersion === 1;
    acceptedAt = accepted ? finiteNumber(snapshot?.fetchedAt) ?? 0 : 0;
    sourceEpoch = accepted
      ? String(snapshot?.sourceEpoch || "").slice(0, 40)
      : "";
    lastSatelliteTick = -Infinity;
    return records.length;
  }

  // Follow a personal environment preset. `tint` is a "#rrggbb" string, and
  // `strength`/`brightness` are the same 0..1 blend and dimming the town-level
  // overlay applies to the background and the lights, so an overcast rain sky
  // and a bright snow sky match the ground beneath them.
  function setEnvironmentTint({
    tint = "",
    strength = 0,
    brightness = 1,
  } = {}) {
    if (disposed) return false;
    const hex = parseHexColor(tint) ? String(tint).trim() : "";
    const nextStrength = hex
      ? clamp(finiteNumber(strength) ?? 0, 0, 1)
      : 0;
    const nextBrightness = clamp(finiteNumber(brightness) ?? 1, 0, 2);
    if (
      hex === environmentTint &&
      nextStrength === environmentTintStrength &&
      nextBrightness === environmentBrightness
    ) {
      return false;
    }
    environmentTint = hex;
    environmentTintStrength = nextStrength;
    environmentBrightness = nextBrightness;
    applyPlanetColors();
    // Re-run the last daylight placement so the sun, the moon, and their glow
    // ring pick the new colours up without waiting for the next minute.
    setDaylightMinute(lastDaylightMinute, lastDaylightStrength);
    return true;
  }

  function setViewDirection(direction) {
    if (disposed) return lookingUp;
    lookingUp = worldSkyLookingUp(direction, lookingUp);
    applyDeepSkyVisibility();
    return lookingUp;
  }

  function tick(
    timestampMs = Date.now(),
    cameraPosition = null,
    viewDirection = null,
  ) {
    if (disposed) return false;
    if (viewDirection) setViewDirection(viewDirection);
    if (
      cameraPosition &&
      Number.isFinite(cameraPosition.x) &&
      Number.isFinite(cameraPosition.y) &&
      Number.isFinite(cameraPosition.z)
    ) {
      group.position.set(
        cameraPosition.x,
        cameraPosition.y,
        cameraPosition.z,
      );
    }
    const now = finiteNumber(timestampMs);
    // A hidden satellite layer costs nothing: SGP4 runs again on the first tick
    // after it becomes visible, not while nobody can see the result.
    if (
      now === null ||
      !deepSkyVisible ||
      (!deepSkyStale && now - lastSatelliteTick < safeTickInterval)
    ) {
      return false;
    }
    deepSkyStale = false;
    lastSatelliteTick = now;
    const propagateCount = activeSatelliteCount();
    for (let index = 0; index < propagateCount; index += 1) {
      const valid = propagateWorldSatelliteOmm(
        records[index],
        now,
        propagated,
      );
      if (!valid) {
        transform.position.set(0, 0, 0);
        transform.scale.setScalar(0);
      } else {
        const distance = Math.hypot(
          propagated.x,
          propagated.y,
          propagated.z,
        );
        const altitude = Math.max(0, distance - EARTH_RADIUS_KM);
        const displayRadius =
          safeSatelliteRadius + clamp(altitude / 36_000, 0, 1) * 18;
        const inverseDistance = distance > 0 ? displayRadius / distance : 0;
        // ECI Z is north; map it to Three's vertical axis. Negating ECI Y
        // preserves a right-handed coordinate system for the World camera.
        transform.position.set(
          propagated.x * inverseDistance,
          propagated.z * inverseDistance,
          -propagated.y * inverseDistance,
        );
        transform.scale.setScalar(compact ? 0.68 : 0.9);
      }
      transform.updateMatrix();
      satellites.setMatrixAt(index, transform.matrix);
    }
    if (propagateCount) satellites.instanceMatrix.needsUpdate = true;
    return true;
  }

  function getState() {
    return {
      stars: starsData.count,
      starsDrawn: starDrawCount,
      planets: WORLD_SKY_PLANETS.length,
      satellites: records.length,
      satellitesDrawn: activeSatelliteCount(),
      satelliteLimit: safeSatelliteLimit,
      satelliteBudget,
      fetchedAt: acceptedAt,
      sourceEpoch,
      disposed,
      sunAndMoon: 2,
      environmentTint,
      environmentTintStrength,
      environmentBrightness,
      lookingUp,
      night: nightStrength > 0,
      deepSkyVisible,
      // The sun, the moon, and their glow ring are always drawn. Stars and
      // satellites only join them while the night sky is being looked at.
      drawCalls:
        2 +
        (deepSkyVisible && starDrawCount > 0 ? 1 : 0) +
        (deepSkyVisible && activeSatelliteCount() > 0 ? 1 : 0),
    };
  }

  function dispose() {
    if (disposed) return;
    disposed = true;
    records = [];
    satellites.count = 0;
    group.parent?.remove?.(group);
    starGeometry.dispose?.();
    planetGeometry.dispose?.();
    celestialGlowGeometry.dispose?.();
    satelliteGeometry.dispose?.();
    disposeMaterial(starMaterial);
    disposeMaterial(planetMaterial);
    disposeMaterial(celestialGlowMaterial);
    disposeMaterial(satelliteMaterial);
    group.clear?.();
  }

  return {
    group,
    stars,
    planets,
    celestialGlows,
    satellites,
    setDaylightMinute,
    setEnvironmentTint,
    setViewDirection,
    setDetailBudget,
    update,
    tick,
    dispose,
    getState,
  };
}

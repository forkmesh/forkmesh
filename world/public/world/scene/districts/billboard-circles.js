import { sharedGeometry, sharedMaterial } from "../../world-resource-cache.js";
import {
  BILLBOARD_CIRCLE_GROUND_Y,
  BILLBOARD_CIRCLE_SURFACE_Y,
  BILLBOARD_RING,
} from "../layout.js";
import { districtGroundMaterial } from "../town.js";
import * as resources from "../resources.js";

const TAU = Math.PI * 2;
const RIM_Y = BILLBOARD_CIRCLE_GROUND_Y + 0.01;
const PAD_Y = BILLBOARD_CIRCLE_GROUND_Y + 0.02;
const PAD_SEGMENTS = 24;
const MIN_PAD_CAPACITY = 32;
const DECAL = Object.freeze({
  toneMapped: false,
  polygonOffset: true,
  polygonOffsetFactor: -2,
  polygonOffsetUnits: -4,
});

export function functions({
  THREE,
  ctx,
  onBillboardNearby = () => {},
  onBillboardAway = () => {},
}) {
  const ring = {
    group: null,
    apron: null,
    rim: null,
    pads: null,
    radius: 0,
    records: [],
    padded: [],
    padReachMin: 0,
    padReachMax: 0,
  };
  const feedRecords = new Map();
  const loadingFeeds = new Set();
  const spinnerHolders = new Map();
  const spinning = new Set();
  const bounds = new THREE.Box3();
  const center = new THREE.Vector3();
  const padMatrix = new THREE.Matrix4();
  const padPosition = new THREE.Vector3();
  const padRotation = new THREE.Quaternion();
  const padYaw = new THREE.Quaternion();
  const padFlat = new THREE.Quaternion().setFromAxisAngle(
    new THREE.Vector3(1, 0, 0),
    -Math.PI / 2,
  );
  const padScale = new THREE.Vector3(1, 1, 1);
  const up = new THREE.Vector3(0, 1, 0);
  const idlePadColor = new THREE.Color(BILLBOARD_RING.muted);
  const activePadColor = new THREE.Color(BILLBOARD_RING.accent);
  const padGeometry = resources.primitive(
    THREE,
    "CircleGeometry",
    BILLBOARD_RING.padRadius,
    PAD_SEGMENTS,
    Math.PI,
    Math.PI,
  );
  const padMaterial = resources.uniqueMaterial(THREE, DECAL, "MeshBasicMaterial");
  let placementSequence = 0;
  let activePad = null;

  function ensureRing() {
    if (ring.group) return ring;
    const group = new THREE.Group();
    group.name = "forkmesh-billboard-ring";
    const apronMaterial = districtGroundMaterial(THREE, "leaderboards");
    apronMaterial.color.set(BILLBOARD_RING.color);
    const apron = new THREE.Mesh(
      resources.primitive(THREE, "CircleGeometry", 1, 96),
      apronMaterial,
    );
    apron.name = "forkmesh-billboard-ring-ground";
    apron.rotation.x = -Math.PI / 2;
    apron.position.y = BILLBOARD_CIRCLE_GROUND_Y;
    apron.receiveShadow = true;
    apron.userData.ground = true;
    const rim = new THREE.Mesh(
      rimGeometry(BILLBOARD_RING.minRadius),
      sharedMaterial(THREE, "MeshBasicMaterial", {
        ...DECAL,
        color: BILLBOARD_RING.muted,
      }),
    );
    rim.name = "forkmesh-billboard-ring-rim";
    rim.rotation.x = -Math.PI / 2;
    rim.position.y = RIM_Y;
    group.add(apron, rim);
    ctx.leaderboardInterior.add(group);
    Object.assign(ring, { group, apron, rim });
    sizeRing(BILLBOARD_RING.minRadius);
    return ring;
  }

  function sectionRank(section) {
    return Math.max(0, BILLBOARD_RING.sections.indexOf(section));
  }

  function ringOrder(left, right) {
    return left.rank - right.rank || left.order - right.order;
  }

  function rimGeometry(radius) {
    const outer = radius + BILLBOARD_RING.groundMargin;
    return resources.primitive(
      THREE,
      "RingGeometry",
      outer - BILLBOARD_RING.rimWidth,
      outer,
      128,
    );
  }

  function sizeRing(radius) {
    if (radius === ring.radius) return;
    ring.radius = radius;
    const outer = radius + BILLBOARD_RING.groundMargin;
    ring.apron.scale.set(outer, outer, 1);
    ring.rim.geometry = rimGeometry(radius);
  }

  function ensurePadCapacity(count) {
    if (ring.pads && ring.pads.instanceMatrix.count >= count) return;
    const capacity = Math.max(
      MIN_PAD_CAPACITY,
      2 ** Math.ceil(Math.log2(Math.max(1, count))),
    );
    const pads = new THREE.InstancedMesh(padGeometry, padMaterial, capacity);
    pads.name = "forkmesh-billboard-refresh-pads";
    pads.count = 0;
    if (ring.pads) {
      ring.group.remove(ring.pads);
      ring.pads.dispose();
    }
    ring.group.add(pads);
    ring.pads = pads;
  }

  function paintPad(record) {
    if (!ring.pads || record.padIndex < 0) return;
    ring.pads.setColorAt(
      record.padIndex,
      record === activePad ? activePadColor : idlePadColor,
    );
    ring.pads.instanceColor.needsUpdate = true;
  }

  function writePads() {
    ensurePadCapacity(ring.padded.length);
    const pads = ring.pads;
    for (const record of ring.padded) {
      padPosition.set(record.padX, PAD_Y, record.padZ);
      padYaw.setFromAxisAngle(up, record.object.rotation.y);
      padRotation.multiplyQuaternions(padYaw, padFlat);
      pads.setMatrixAt(
        record.padIndex,
        padMatrix.compose(padPosition, padRotation, padScale),
      );
      paintPad(record);
    }
    pads.count = ring.padded.length;
    pads.instanceMatrix.needsUpdate = true;
    pads.computeBoundingSphere();
  }

  function relayout() {
    const records = ring.records.sort(ringOrder);
    let span = 0;
    let widest = 0;
    for (const record of records) {
      span += record.width + BILLBOARD_RING.gap;
      widest = Math.max(widest, record.width);
    }
    const radius = Math.max(
      BILLBOARD_RING.minRadius,
      (span + BILLBOARD_RING.entryGap) / TAU,
      widest * 0.75,
    );
    sizeRing(radius);
    const entry = BILLBOARD_RING.entryGap / radius;
    const step = span ? (TAU - entry) / span : 0;
    let cursor = entry / 2;
    ring.padded.length = 0;
    ring.padReachMin = Infinity;
    ring.padReachMax = 0;
    for (const record of records) {
      const slice = (record.width + BILLBOARD_RING.gap) * step;
      const angle = cursor + slice / 2;
      cursor += slice;
      const outX = Math.cos(angle);
      const outZ = Math.sin(angle);
      const object = record.object;
      object.position.set(
        outX * radius,
        BILLBOARD_CIRCLE_SURFACE_Y - record.baseY,
        outZ * radius,
      );
      object.rotation.y = Math.atan2(-outX, -outZ);
      record.padIndex = -1;
      if (!record.feed) continue;
      const reach = radius - record.frontZ - BILLBOARD_RING.padGap;
      record.padX = outX * reach;
      record.padZ = outZ * reach;
      record.inX = -outX;
      record.inZ = -outZ;
      record.padIndex = ring.padded.push(record) - 1;
      ring.padReachMin = Math.min(ring.padReachMin, reach);
      ring.padReachMax = Math.max(ring.padReachMax, reach);
    }
    writePads();
  }

  function syncSpinners(feed) {
    const loading = loadingFeeds.has(feed);
    const holder = spinnerHolders.get(feed);
    for (const record of feedRecords.get(feed) || []) {
      const show = loading && (!holder || holder === record);
      record.spinner.visible = show;
      if (show) spinning.add(record.spinner);
      else spinning.delete(record.spinner);
    }
  }

  function placeBillboardOnIsland(object, options = {}) {
    if (!object) return null;
    ensureRing();
    object.parent?.remove(object);
    object.position.set(0, 0, 0);
    object.rotation.y = 0;
    object.updateMatrixWorld(true);
    bounds.setFromObject(object);
    const feed = String(options.feed || "");
    const order = Number(options.order);
    const record = {
      object,
      rank: sectionRank(String(options.circle || "")),
      order: Number.isFinite(order) ? order : placementSequence++,
      feed,
      cooldownMs: Number(options.cooldownMs) || BILLBOARD_RING.cooldownMs,
      refreshedAt: -Infinity,
      width: Math.max(1, bounds.max.x - bounds.min.x),
      baseY: bounds.min.y,
      topY: bounds.max.y,
      frontZ: Math.max(0, bounds.max.z),
      padIndex: -1,
      padX: 0,
      padZ: 0,
      inX: 0,
      inZ: 0,
      spinner: null,
    };
    ring.group.add(object);
    ring.records.push(record);
    if (feed) {
      const spinner = new THREE.Mesh(
        sharedGeometry(THREE, "TorusGeometry", 0.42, 0.1, 10, 30, Math.PI * 1.55),
        sharedMaterial(THREE, "MeshBasicMaterial", {
          color: BILLBOARD_RING.accent,
          toneMapped: false,
        }),
      );
      spinner.name = `forkmesh-billboard-refresh-spinner-${feed}-${ring.records.length}`;
      spinner.position.set(0, record.topY + 1.1, 0.4);
      spinner.visible = false;
      spinner.renderOrder = 46;
      object.add(spinner);
      record.spinner = spinner;
      const shared = feedRecords.get(feed) || [];
      shared.push(record);
      feedRecords.set(feed, shared);
      syncSpinners(feed);
    }
    relayout();
    return record;
  }

  function removeBillboardFromIsland(record) {
    const index = ring.records.indexOf(record);
    if (index < 0) return false;
    ring.records.splice(index, 1);
    if (record === activePad) leavePad();
    if (record.feed) {
      const shared = feedRecords.get(record.feed) || [];
      shared.splice(shared.indexOf(record), 1);
      if (!shared.length) feedRecords.delete(record.feed);
      if (spinnerHolders.get(record.feed) === record) {
        spinnerHolders.delete(record.feed);
      }
      spinning.delete(record.spinner);
      record.spinner.removeFromParent();
    }
    record.object.removeFromParent();
    relayout();
    return true;
  }

  function setBillboardRefreshing(feed, loading) {
    const key = String(feed || "");
    if (loading === true) {
      loadingFeeds.add(key);
    } else {
      loadingFeeds.delete(key);
      spinnerHolders.delete(key);
    }
    syncSpinners(key);
  }

  function enterPad(record) {
    activePad = record;
    paintPad(record);
    const now = performance.now();
    const refetch = now - record.refreshedAt >= record.cooldownMs;
    if (refetch) {
      record.refreshedAt = now;
      spinnerHolders.set(record.feed, record);
      syncSpinners(record.feed);
    }
    onBillboardNearby({ feed: record.feed, refetch });
  }

  function leavePad() {
    const record = activePad;
    if (!record) return;
    activePad = null;
    paintPad(record);
    onBillboardAway({ feed: record.feed });
  }

  function padUnderPlayer() {
    if (ctx.officeSceneMode !== "town" || !ring.padded.length) return null;
    ring.group.getWorldPosition(center);
    const x = ctx.player.position.x - center.x;
    const z = ctx.player.position.z - center.z;
    const slack = BILLBOARD_RING.padRadius + BILLBOARD_RING.padExit;
    const distance = Math.hypot(x, z);
    if (
      distance < ring.padReachMin - slack ||
      distance > ring.padReachMax + slack
    ) {
      return null;
    }
    for (const record of ring.padded) {
      const dx = x - record.padX;
      const dz = z - record.padZ;
      const reach = record === activePad ? slack : BILLBOARD_RING.padRadius;
      if (
        dx * dx + dz * dz <= reach * reach &&
        dx * record.inX + dz * record.inZ >= -BILLBOARD_RING.padGap
      ) {
        return record;
      }
    }
    return null;
  }

  function updateBillboardPads() {
    const record = padUnderPlayer();
    if (record === activePad) return;
    leavePad();
    if (record) enterPad(record);
  }

  function tickBillboardSpinners(time) {
    for (const spinner of spinning) spinner.rotation.z = -time * 0.006;
  }

  return {
    placeBillboardOnIsland,
    removeBillboardFromIsland,
    setBillboardRefreshing,
    tickBillboardSpinners,
    updateBillboardPads,
  };
}

export function build({
  animated,
  placeBillboardOnIsland,
  tickBillboardSpinners,
  worldBulletin,
  worldDiscordBoard,
  worldGeneralChatBoard,
}) {
  animated.push(tickBillboardSpinners);
  placeBillboardOnIsland(worldBulletin, { circle: "hub" });
  placeBillboardOnIsland(worldGeneralChatBoard, { circle: "hub" });
  placeBillboardOnIsland(worldDiscordBoard, { circle: "social" });
  return {};
}

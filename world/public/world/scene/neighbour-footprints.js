export const NEIGHBOUR_WALL_THICKNESS = 0.3;
const COLLISION_STEP = 0.35;

export function neighbourFootprint({
  x = 0, z = 0, rotation = 0, width = 1, depth = 1,
  hollow = false, door = 0, link = null,
} = {}) {
  return {
    x: Number(x) || 0,
    z: Number(z) || 0,
    rotation: Number(rotation) || 0,
    cos: Math.cos(Number(rotation) || 0),
    sin: Math.sin(Number(rotation) || 0),
    halfWidth: Math.max(0.5, Number(width) || 1) / 2,
    halfDepth: Math.max(0.5, Number(depth) || 1) / 2,
    hollow: Boolean(hollow),
    doorHalf: Math.max(0, Number(door) || 0) / 2,
    link,
  };
}

export function footprintLocal(footprint, x, z) {
  const dx = x - footprint.x;
  const dz = z - footprint.z;
  return {
    x: dx * footprint.cos - dz * footprint.sin,
    z: dx * footprint.sin + dz * footprint.cos,
  };
}

export function footprintWorld(footprint, localX, localZ) {
  return {
    x: footprint.x + localX * footprint.cos + localZ * footprint.sin,
    z: footprint.z - localX * footprint.sin + localZ * footprint.cos,
  };
}

function boxHit(localX, localZ, minX, maxX, minZ, maxZ, radius) {
  return (
    localX > minX - radius && localX < maxX + radius &&
    localZ > minZ - radius && localZ < maxZ + radius
  );
}

export function footprintBlocks(footprint, localX, localZ, radius = 0) {
  const w = footprint.halfWidth;
  const d = footprint.halfDepth;
  if (!footprint.hollow) return boxHit(localX, localZ, -w, w, -d, d, radius);
  const t = NEIGHBOUR_WALL_THICKNESS / 2;
  const door = footprint.doorHalf;
  return (
    boxHit(localX, localZ, -w, w, -d - t, -d + t, radius) ||
    boxHit(localX, localZ, -w - t, -w + t, -d, d, radius) ||
    boxHit(localX, localZ, w - t, w + t, -d, d, radius) ||
    boxHit(localX, localZ, -w, -door, d - t, d + t, radius) ||
    boxHit(localX, localZ, door, w, d - t, d + t, radius)
  );
}

function pathBlocked(footprint, from, to, radius) {
  const length = Math.hypot(to.x - from.x, to.z - from.z);
  const steps = Math.max(1, Math.ceil(length / COLLISION_STEP));
  for (let step = 1; step <= steps; step += 1) {
    const t = step / steps;
    const x = from.x + (to.x - from.x) * t;
    const z = from.z + (to.z - from.z) * t;
    if (footprintBlocks(footprint, x, z, radius)) return { x, z };
  }
  return null;
}

export function constrainToFootprints(footprints, previous, current, radius = 0) {
  let x = Number(current?.x);
  let z = Number(current?.z);
  let door = null;
  let blocked = false;
  if (!Number.isFinite(x) || !Number.isFinite(z) || !previous) {
    return { x, z, blocked, door };
  }
  for (const footprint of footprints || []) {
    if (!footprint) continue;
    const from = footprintLocal(footprint, previous.x, previous.z);
    if (Math.max(Math.abs(from.x) - footprint.halfWidth,
      Math.abs(from.z) - footprint.halfDepth) > 40) continue;
    const to = footprintLocal(footprint, x, z);
    if (footprintBlocks(footprint, from.x, from.z, radius)) continue;
    const hit = pathBlocked(footprint, from, to, radius);
    if (!hit) continue;
    blocked = true;
    if (
      !footprint.hollow &&
      footprint.doorHalf > 0 &&
      Math.abs(hit.x) <= footprint.doorHalf &&
      from.z >= footprint.halfDepth + radius - 1e-6 &&
      to.z < from.z
    ) {
      door = footprint;
    }
    let local = { x: to.x, z: from.z };
    if (pathBlocked(footprint, from, local, radius)) {
      local = { x: from.x, z: to.z };
      if (pathBlocked(footprint, from, local, radius)) local = from;
    }
    ({ x, z } = footprintWorld(footprint, local.x, local.z));
  }
  return { x, z, blocked, door };
}

export function footprintDoorDistance(footprint, x, z) {
  const local = footprintLocal(footprint, x, z);
  return Math.hypot(
    Math.max(0, Math.abs(local.x) - footprint.doorHalf),
    local.z - footprint.halfDepth,
  );
}

const registry = new Map();

export function setNeighbourFootprints(owner, footprints = []) {
  const id = String(owner || "");
  const list = (Array.isArray(footprints) ? footprints : []).filter(Boolean);
  if (!id) return;
  if (list.length) registry.set(id, list);
  else registry.delete(id);
}

export function clearNeighbourFootprints() {
  registry.clear();
}

export function neighbourFootprints() {
  const all = [];
  registry.forEach((list) => all.push(...list));
  return all;
}

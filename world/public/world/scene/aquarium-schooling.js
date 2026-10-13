export const REEF_STEER_TRACK = 0;
export const REEF_STEER_SCHOOL = 1;
export const REEF_STEER_SCATTER = 2;
export const REEF_STEER_GROUP_LIMIT = 16;

const CELL_SIZE = 1;
const SEPARATION_RADIUS = 0.62;
const SCATTER_SEPARATION_RADIUS = 1.35;
const NEIGHBOUR_RADIUS = 1.6;
const WALL_MARGIN = 0.7;
const MAX_SPEED = 2.6;
const SCHOOL_MIN_SPEED = 0.55;
const MAX_ACCELERATION = 5.5;
const STAT_STRIDE = 7;

export function reefColourGroup(hex) {
  const value = parseInt(String(hex || "").replace("#", ""), 16);
  if (!Number.isFinite(value)) return 0;
  const r = ((value >> 16) & 255) / 255;
  const g = ((value >> 8) & 255) / 255;
  const b = (value & 255) / 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const delta = max - min;
  if (delta < 0.04) return 0;
  let hue;
  if (max === r) hue = ((g - b) / delta) % 6;
  else if (max === g) hue = (b - r) / delta + 2;
  else hue = (r - g) / delta + 4;
  hue = (hue * 60 + 360) % 360;
  return Math.floor(((hue + 30) % 360) / 60);
}

export function createReefSteering(initialCapacity = 0) {
  let capacity = 0;
  let count = 0;
  let position = new Float32Array(0);
  let velocity = new Float32Array(0);
  let home = new Float32Array(0);
  let group = new Int16Array(0);
  let cellOf = new Int32Array(0);
  let cellItems = new Int32Array(0);
  const bounds = new Float32Array([-1, 1, 1, 10, -12, 12]);
  let cellsX = 1;
  let cellsY = 1;
  let cellsZ = 1;
  let cellStart = new Int32Array(2);
  let cellFill = new Int32Array(1);
  const stats = new Float32Array(REEF_STEER_GROUP_LIMIT * STAT_STRIDE);
  const anchors = new Float32Array(REEF_STEER_GROUP_LIMIT * 3);
  const target = new Float32Array(5);
  let groupCount = 1;

  function reserve(next) {
    if (next <= capacity) return;
    capacity = Math.max(next, Math.ceil(capacity * 1.5), 8);
    const grow = (source, Kind, stride) => {
      const out = new Kind(capacity * stride);
      out.set(source.subarray(0, Math.min(source.length, out.length)));
      return out;
    };
    position = grow(position, Float32Array, 3);
    velocity = grow(velocity, Float32Array, 3);
    home = grow(home, Float32Array, 3);
    group = grow(group, Int16Array, 1);
    cellOf = new Int32Array(capacity);
    cellItems = new Int32Array(capacity);
  }

  function resize(next) {
    const size = Math.max(0, Math.floor(Number(next) || 0));
    reserve(size);
    count = size;
    return count;
  }

  function setBounds(minX, maxX, minY, maxY, minZ, maxZ) {
    bounds[0] = minX;
    bounds[1] = maxX;
    bounds[2] = minY;
    bounds[3] = maxY;
    bounds[4] = minZ;
    bounds[5] = maxZ;
    cellsX = Math.max(1, Math.ceil((maxX - minX) / CELL_SIZE));
    cellsY = Math.max(1, Math.ceil((maxY - minY) / CELL_SIZE));
    cellsZ = Math.max(1, Math.ceil((maxZ - minZ) / CELL_SIZE));
    const cells = cellsX * cellsY * cellsZ;
    cellStart = new Int32Array(cells + 1);
    cellFill = new Int32Array(cells);
  }

  function setGroup(index, value) {
    group[index] = Math.max(0, Math.min(REEF_STEER_GROUP_LIMIT - 1, value | 0));
  }

  function setGroupCount(value) {
    groupCount = Math.max(1, Math.min(REEF_STEER_GROUP_LIMIT, value | 0));
    return groupCount;
  }

  function setHome(index, x, y, z) {
    const at = index * 3;
    home[at] = x;
    home[at + 1] = y;
    home[at + 2] = z;
  }

  function place(index, x, y, z, vx, vy, vz) {
    const at = index * 3;
    position[at] = x;
    position[at + 1] = y;
    position[at + 2] = z;
    velocity[at] = vx;
    velocity[at + 1] = vy;
    velocity[at + 2] = vz;
    home[at] = x;
    home[at + 1] = y;
    home[at + 2] = z;
  }

  function setTarget(x, y, z, sign, radius) {
    target[0] = x;
    target[1] = y;
    target[2] = z;
    target[3] = sign < 0 ? -1 : 1;
    target[4] = Math.max(0, radius);
  }

  function clearTarget() {
    target[4] = 0;
  }

  function cellIndex(x, y, z) {
    let cx = Math.floor((x - bounds[0]) / CELL_SIZE);
    let cy = Math.floor((y - bounds[2]) / CELL_SIZE);
    let cz = Math.floor((z - bounds[4]) / CELL_SIZE);
    if (cx < 0) cx = 0;
    else if (cx >= cellsX) cx = cellsX - 1;
    if (cy < 0) cy = 0;
    else if (cy >= cellsY) cy = cellsY - 1;
    if (cz < 0) cz = 0;
    else if (cz >= cellsZ) cz = cellsZ - 1;
    return (cz * cellsY + cy) * cellsX + cx;
  }

  function buildGrid() {
    cellStart.fill(0);
    for (let index = 0; index < count; index += 1) {
      const at = index * 3;
      const cell = cellIndex(position[at], position[at + 1], position[at + 2]);
      cellOf[index] = cell;
      cellStart[cell + 1] += 1;
    }
    for (let cell = 0; cell < cellFill.length; cell += 1) {
      cellStart[cell + 1] += cellStart[cell];
      cellFill[cell] = cellStart[cell];
    }
    for (let index = 0; index < count; index += 1) {
      const cell = cellOf[index];
      cellItems[cellFill[cell]] = index;
      cellFill[cell] += 1;
    }
  }

  function gatherStats() {
    stats.fill(0);
    for (let index = 0; index < count; index += 1) {
      const at = index * 3;
      const slot = group[index] * STAT_STRIDE;
      stats[slot] += position[at];
      stats[slot + 1] += position[at + 1];
      stats[slot + 2] += position[at + 2];
      stats[slot + 3] += velocity[at];
      stats[slot + 4] += velocity[at + 1];
      stats[slot + 5] += velocity[at + 2];
      stats[slot + 6] += 1;
    }
    for (let slot = 0; slot < stats.length; slot += STAT_STRIDE) {
      const members = stats[slot + 6];
      if (members <= 0) continue;
      stats[slot] /= members;
      stats[slot + 1] /= members;
      stats[slot + 2] /= members;
      stats[slot + 3] /= members;
      stats[slot + 4] /= members;
      stats[slot + 5] /= members;
    }
  }

  function placeAnchors(time) {
    const spanZ = bounds[5] - bounds[4];
    const spanY = bounds[3] - bounds[2];
    const lane = spanZ / groupCount;
    for (let index = 0; index < groupCount; index += 1) {
      const at = index * 3;
      anchors[at] = (bounds[0] + bounds[1]) * 0.5;
      anchors[at + 1] =
        bounds[2] +
        spanY * (0.3 + 0.4 * ((index * 0.618) % 1)) +
        Math.sin(time * 0.00031 + index * 1.7) * spanY * 0.14;
      anchors[at + 2] =
        bounds[4] +
        lane * (index + 0.5) +
        Math.sin(time * 0.00019 + index * 2.3) * lane * 0.28;
    }
  }

  function step(deltaSeconds, mode, time) {
    if (count === 0) return 0;
    const dt = deltaSeconds > 0.1 ? 0.1 : deltaSeconds > 0 ? deltaSeconds : 0;
    if (dt === 0) return 0;
    buildGrid();
    gatherStats();
    placeAnchors(time);
    const schooling = mode === REEF_STEER_SCHOOL;
    const scattering = mode === REEF_STEER_SCATTER;
    const separationRadius = scattering
      ? SCATTER_SEPARATION_RADIUS
      : SEPARATION_RADIUS;
    const separationSquared = separationRadius * separationRadius;
    const neighbourSquared = NEIGHBOUR_RADIUS * NEIGHBOUR_RADIUS;
    const targetRadius = target[4];
    const minX = bounds[0];
    const maxX = bounds[1];
    const minY = bounds[2];
    const maxY = bounds[3];
    const minZ = bounds[4];
    const maxZ = bounds[5];
    for (let index = 0; index < count; index += 1) {
      const at = index * 3;
      const x = position[at];
      const y = position[at + 1];
      const z = position[at + 2];
      const ownGroup = group[index];
      let ax = 0;
      let ay = 0;
      let az = 0;
      let sx = 0;
      let sy = 0;
      let sz = 0;
      let alignX = 0;
      let alignY = 0;
      let alignZ = 0;
      let aligned = 0;
      const cell = cellOf[index];
      const cx = cell % cellsX;
      const cy = Math.floor(cell / cellsX) % cellsY;
      const cz = Math.floor(cell / (cellsX * cellsY));
      const reach = scattering ? 2 : 1;
      for (let oz = cz - reach; oz <= cz + reach; oz += 1) {
        if (oz < 0 || oz >= cellsZ) continue;
        for (let oy = cy - reach; oy <= cy + reach; oy += 1) {
          if (oy < 0 || oy >= cellsY) continue;
          for (let ox = cx - reach; ox <= cx + reach; ox += 1) {
            if (ox < 0 || ox >= cellsX) continue;
            const neighbourCell = (oz * cellsY + oy) * cellsX + ox;
            const end = cellStart[neighbourCell + 1];
            for (let slot = cellStart[neighbourCell]; slot < end; slot += 1) {
              const other = cellItems[slot];
              if (other === index) continue;
              const otherAt = other * 3;
              const dx = x - position[otherAt];
              const dy = y - position[otherAt + 1];
              const dz = z - position[otherAt + 2];
              const distanceSquared = dx * dx + dy * dy + dz * dz;
              if (distanceSquared < separationSquared && distanceSquared > 1e-6) {
                const distance = Math.sqrt(distanceSquared);
                const push = (separationRadius - distance) / (separationRadius * distance);
                sx += dx * push;
                sy += dy * push;
                sz += dz * push;
              }
              if (
                schooling &&
                group[other] === ownGroup &&
                distanceSquared < neighbourSquared
              ) {
                alignX += velocity[otherAt];
                alignY += velocity[otherAt + 1];
                alignZ += velocity[otherAt + 2];
                aligned += 1;
              }
            }
          }
        }
      }
      const vx = velocity[at];
      const vy = velocity[at + 1];
      const vz = velocity[at + 2];
      if (schooling) {
        const slotStats = ownGroup * STAT_STRIDE;
        const anchorAt = (ownGroup % groupCount) * 3;
        const centreX = stats[slotStats] * 0.55 + anchors[anchorAt] * 0.45;
        const centreY = stats[slotStats + 1] * 0.55 + anchors[anchorAt + 1] * 0.45;
        const centreZ = stats[slotStats + 2] * 0.55 + anchors[anchorAt + 2] * 0.45;
        ax += (centreX - x) * 0.9;
        ay += (centreY - y) * 0.9;
        az += (centreZ - z) * 0.9;
        let meanX = stats[slotStats + 3];
        let meanY = stats[slotStats + 4];
        let meanZ = stats[slotStats + 5];
        if (aligned > 0) {
          meanX = meanX * 0.4 + (alignX / aligned) * 0.6;
          meanY = meanY * 0.4 + (alignY / aligned) * 0.6;
          meanZ = meanZ * 0.4 + (alignZ / aligned) * 0.6;
        }
        ax += (meanX - vx) * 1.4;
        ay += (meanY - vy) * 1.4;
        az += (meanZ - vz) * 1.4;
        ax += sx * 3.2;
        ay += sy * 3.2;
        az += sz * 3.2;
      } else if (scattering) {
        ax += (home[at] - x) * 0.35 - vx * 0.25;
        ay += (home[at + 1] - y) * 0.35 - vy * 0.25;
        az += (home[at + 2] - z) * 0.35 - vz * 0.25;
        ax += sx * 6;
        ay += sy * 6;
        az += sz * 6;
      } else {
        ax += (home[at] - x) * 2.4 - vx * 1.9;
        ay += (home[at + 1] - y) * 2.4 - vy * 1.9;
        az += (home[at + 2] - z) * 2.4 - vz * 1.9;
        ax += sx * 1.2;
        ay += sy * 1.2;
        az += sz * 1.2;
      }
      if (targetRadius > 0) {
        const dx = target[0] - x;
        const dy = target[1] - y;
        const dz = target[2] - z;
        const distance = Math.sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < targetRadius && distance > 1e-4) {
          const pull = ((1 - distance / targetRadius) * 7 * target[3]) / distance;
          const settle = target[3] > 0 && distance < 0.9 ? distance / 0.9 : 1;
          ax += dx * pull * settle;
          ay += dy * pull * settle;
          az += dz * pull * settle;
        }
      }
      if (x < minX + WALL_MARGIN) ax += (minX + WALL_MARGIN - x) * 12;
      else if (x > maxX - WALL_MARGIN) ax -= (x - maxX + WALL_MARGIN) * 12;
      if (y < minY + WALL_MARGIN) ay += (minY + WALL_MARGIN - y) * 12;
      else if (y > maxY - WALL_MARGIN) ay -= (y - maxY + WALL_MARGIN) * 12;
      if (z < minZ + WALL_MARGIN) az += (minZ + WALL_MARGIN - z) * 12;
      else if (z > maxZ - WALL_MARGIN) az -= (z - maxZ + WALL_MARGIN) * 12;
      const acceleration = Math.sqrt(ax * ax + ay * ay + az * az);
      if (acceleration > MAX_ACCELERATION) {
        const scale = MAX_ACCELERATION / acceleration;
        ax *= scale;
        ay *= scale;
        az *= scale;
      }
      let nx = vx + ax * dt;
      let ny = (vy + ay * dt) * 0.96;
      let nz = vz + az * dt;
      const speed = Math.sqrt(nx * nx + ny * ny + nz * nz);
      if (speed > MAX_SPEED) {
        const scale = MAX_SPEED / speed;
        nx *= scale;
        ny *= scale;
        nz *= scale;
      } else if (schooling && speed < SCHOOL_MIN_SPEED) {
        if (speed > 1e-4) {
          const scale = SCHOOL_MIN_SPEED / speed;
          nx *= scale;
          ny *= scale;
          nz *= scale;
        } else {
          nz = ownGroup % 2 ? -SCHOOL_MIN_SPEED : SCHOOL_MIN_SPEED;
        }
      }
      let px = x + nx * dt;
      let py = y + ny * dt;
      let pz = z + nz * dt;
      if (px < minX) {
        px = minX;
        if (nx < 0) nx = -nx * 0.3;
      } else if (px > maxX) {
        px = maxX;
        if (nx > 0) nx = -nx * 0.3;
      }
      if (py < minY) {
        py = minY;
        if (ny < 0) ny = -ny * 0.3;
      } else if (py > maxY) {
        py = maxY;
        if (ny > 0) ny = -ny * 0.3;
      }
      if (pz < minZ) {
        pz = minZ;
        if (nz < 0) nz = -nz * 0.3;
      } else if (pz > maxZ) {
        pz = maxZ;
        if (nz > 0) nz = -nz * 0.3;
      }
      position[at] = px;
      position[at + 1] = py;
      position[at + 2] = pz;
      velocity[at] = nx;
      velocity[at + 1] = ny;
      velocity[at + 2] = nz;
    }
    return count;
  }

  setBounds(bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
  resize(initialCapacity);
  count = 0;

  return {
    get count() {
      return count;
    },
    get position() {
      return position;
    },
    get velocity() {
      return velocity;
    },
    get group() {
      return group;
    },
    get groupCount() {
      return groupCount;
    },
    bounds,
    resize,
    setBounds,
    setGroup,
    setGroupCount,
    setHome,
    place,
    setTarget,
    clearTarget,
    step,
  };
}

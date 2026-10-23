import { sharedMaterial } from "../../world-resource-cache.js";
import {
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  WORLD_BIKE_LANE_APOTHEM,
  WORLD_BIKE_LANE_CORNER_RADIUS,
  WORLD_HEX_TILES,
  WORLD_ROAD_CORNER_SEGMENTS,
  WORLD_ROAD_JUNCTIONS,
  WORLD_ROAD_JUNCTION_REACH,
  WORLD_ROAD_MARKING_Y,
  WORLD_ROAD_SURFACE_Y,
  WORLD_ROAD_WIDTH,
} from "../layout.js";
import {
  makeSharedMaterial,
  standardMaterialParameters,
} from "../materials.js";
import { roundedHexagonOutline } from "../movement.js";

function hexOutlineRingPositions(tiles, innerApothem, width, positions, indices) {
  const inner = roundedHexagonOutline(
    innerApothem,
    CONTINUOUS_CITY_CORNER_RADIUS + innerApothem - CONTINUOUS_CITY_RADIUS,
    WORLD_ROAD_CORNER_SEGMENTS,
  );
  const count = inner.length;
  tiles.forEach((tile) => {
    const base = positions.length / 3;
    inner.forEach(([x, y]) => positions.push(tile.x + x, 0, tile.z - y));
    const outer = roundedHexagonOutline(
      innerApothem + width,
      CONTINUOUS_CITY_CORNER_RADIUS +
        innerApothem +
        width -
        CONTINUOUS_CITY_RADIUS,
      WORLD_ROAD_CORNER_SEGMENTS,
    );
    outer.forEach(([x, y]) => positions.push(tile.x + x, 0, tile.z - y));
    for (let index = 0; index < count; index += 1) {
      const next = (index + 1) % count;
      indices.push(
        base + index,
        base + count + index,
        base + next,
        base + next,
        base + count + index,
        base + count + next,
      );
    }
  });
}

function roadBendPositions(junctions, positions, indices) {
  const segments = WORLD_ROAD_CORNER_SEGMENTS;
  const inner = roundedHexagonOutline(
    CONTINUOUS_CITY_RADIUS,
    CONTINUOUS_CITY_CORNER_RADIUS,
    segments,
  );
  const outer = roundedHexagonOutline(
    CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH,
    CONTINUOUS_CITY_CORNER_RADIUS + WORLD_ROAD_WIDTH,
    segments,
  );
  junctions.forEach(({ bend }) => {
    if (!bend) return;
    const base = positions.length / 3;
    const first = bend.vertex * (segments + 1);
    for (let step = 0; step <= segments; step += 1) {
      const [ix, iy] = inner[first + step];
      const [ox, oy] = outer[first + step];
      positions.push(bend.x + ix, 0, bend.z + iy, bend.x + ox, 0, bend.z + oy);
    }
    for (let step = 0; step < segments; step += 1) {
      const a = base + step * 2;
      indices.push(a, a + 1, a + 2, a + 2, a + 1, a + 3);
    }
  });
}

function flatRoadGeometry(THREE, positions, indices) {
  for (let index = 0; index < indices.length; index += 3) {
    const a = indices[index] * 3;
    const b = indices[index + 1] * 3;
    const c = indices[index + 2] * 3;
    const cross =
      (positions[c] - positions[a]) * (positions[b + 2] - positions[a + 2]) -
      (positions[b] - positions[a]) * (positions[c + 2] - positions[a + 2]);
    if (cross < 0) {
      indices[index + 1] = c / 3;
      indices[index + 2] = b / 3;
    }
  }
  const normals = new Float32Array(positions.length);
  for (let index = 1; index < normals.length; index += 3) normals[index] = 1;
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute(
    "position",
    new THREE.Float32BufferAttribute(positions, 3),
  );
  geometry.setAttribute("normal", new THREE.BufferAttribute(normals, 3));
  geometry.setIndex(indices);
  geometry.computeBoundingSphere();
  return geometry;
}

function hexRoadDashCenters() {
  const period = 6.4;
  const dash = 3.2;
  const apothem = WORLD_BIKE_LANE_APOTHEM;
  const corner = WORLD_BIKE_LANE_CORNER_RADIUS;
  const edge = ((apothem - corner) * 2) / Math.sqrt(3);
  const arcCount = Math.max(1, Math.floor((corner * Math.PI) / 3 / period));
  const seen = new Set();
  const dashes = [];
  const clear = WORLD_ROAD_JUNCTION_REACH + dash / 2;
  const add = (x, z, heading, through = false) => {
    const key = `${Math.round(x * 4)}:${Math.round(z * 4)}`;
    if (seen.has(key)) return;
    if (
      !through &&
      WORLD_ROAD_JUNCTIONS.some(
        (junction) => Math.hypot(x - junction.x, z - junction.z) < clear,
      )
    ) {
      return;
    }
    seen.add(key);
    dashes.push([x, z, heading]);
  };
  const circumradius = (apothem * 2) / Math.sqrt(3);
  const bendAt = (x, z) =>
    WORLD_ROAD_JUNCTIONS.some(
      (junction) =>
        junction.bend && Math.hypot(x - junction.x, z - junction.z) < 1,
    );
  const addArc = (tile, normal, through) => {
    const cornerAngle = normal + Math.PI / 6;
    const cornerX = tile.x + Math.cos(cornerAngle) * edge;
    const cornerZ = tile.z + Math.sin(cornerAngle) * edge;
    for (let step = 0; step < arcCount; step += 1) {
      const angle = normal + ((step + 0.5) * Math.PI) / 3 / arcCount;
      add(
        cornerX + Math.cos(angle) * corner,
        cornerZ + Math.sin(angle) * corner,
        Math.atan2(Math.sin(angle), -Math.cos(angle)),
        through,
      );
    }
  };
  WORLD_ROAD_JUNCTIONS.forEach(({ bend }) => {
    if (bend) addArc(bend, (bend.vertex * Math.PI) / 3, true);
  });
  WORLD_HEX_TILES.forEach((tile) => {
    for (let side = 0; side < 6; side += 1) {
      const normal = (side * Math.PI) / 3;
      const midX = tile.x + Math.cos(normal) * apothem;
      const midZ = tile.z + Math.sin(normal) * apothem;
      const tangentX = -Math.sin(normal);
      const tangentZ = Math.cos(normal);
      const heading = Math.atan2(-tangentX, -tangentZ);
      const reach = Math.floor((edge / 2 - dash / 2) / period);
      for (let step = -reach; step <= reach; step += 1) {
        add(
          midX + tangentX * step * period,
          midZ + tangentZ * step * period,
          heading,
        );
      }
      const vertexAngle = normal + Math.PI / 6;
      if (
        !bendAt(
          tile.x + Math.cos(vertexAngle) * circumradius,
          tile.z + Math.sin(vertexAngle) * circumradius,
        )
      ) {
        addArc(tile, normal, false);
      }
    }
  });
  return dashes;
}

export const ROAD_TURN_CHOICE_DISTANCE = 60;
export const ROAD_TURN_STEER_THRESHOLD = 0.3;

function roadTurnArc(graph, incoming, outgoing, hex) {
  const node = graph.nodes[incoming.to];
  const cross = incoming.ux * outgoing.uz - incoming.uz * outgoing.ux;
  const side = cross > 0 ? 1 : -1;
  const radius = graph.cornerRadius;
  const startX = node.x - incoming.ux * graph.cornerInset;
  const startZ = node.z - incoming.uz * graph.cornerInset;
  const endX = node.x + outgoing.ux * graph.cornerInset;
  const endZ = node.z + outgoing.uz * graph.cornerInset;
  const centerX = startX - incoming.uz * side * radius;
  const centerZ = startZ + incoming.ux * side * radius;
  const startAngle = Math.atan2(startZ - centerZ, startX - centerX);
  let sweep = graph.arcLength / radius;
  const probe = startAngle + sweep;
  const miss = Math.hypot(
    centerX + Math.cos(probe) * radius - endX,
    centerZ + Math.sin(probe) * radius - endZ,
  );
  if (miss > 1e-6 * radius) sweep = -sweep;
  return {
    from: incoming.id,
    edge: outgoing.id,
    side,
    hex,
    centerX,
    centerZ,
    startAngle,
    sweep,
  };
}

export function createRoadGraph(
  tiles = WORLD_HEX_TILES,
  apothem = WORLD_BIKE_LANE_APOTHEM,
  cornerRadius = WORLD_BIKE_LANE_CORNER_RADIUS,
) {
  const circumradius = (apothem * 2) / Math.sqrt(3);
  const cornerInset = cornerRadius / Math.sqrt(3);
  const graph = {
    nodes: [],
    edges: [],
    turns: [],
    cornerRadius,
    cornerInset,
    straightLength: circumradius - 2 * cornerInset,
    arcLength: (cornerRadius * Math.PI) / 3,
    pieceLength: circumradius - 2 * cornerInset + (cornerRadius * Math.PI) / 3,
  };
  const nodeIds = new Map();
  const edgeIds = new Map();
  const nodeAt = (x, z) => {
    const key = `${Math.round(x * 100)}:${Math.round(z * 100)}`;
    let id = nodeIds.get(key);
    if (id === undefined) {
      id = graph.nodes.length;
      nodeIds.set(key, id);
      graph.nodes.push({ id, x, z, out: [] });
    }
    return id;
  };
  const directed = (from, to, hex) => {
    const key = `${from}>${to}`;
    let id = edgeIds.get(key);
    if (id === undefined) {
      const a = graph.nodes[from];
      const b = graph.nodes[to];
      const length = Math.hypot(b.x - a.x, b.z - a.z);
      id = graph.edges.length;
      edgeIds.set(key, id);
      graph.edges.push({
        id,
        from,
        to,
        ux: (b.x - a.x) / length,
        uz: (b.z - a.z) / length,
        length,
        reverse: -1,
        hexes: [],
        turns: [],
        incoming: [],
      });
      a.out.push(id);
    }
    const edge = graph.edges[id];
    if (!edge.hexes.includes(hex)) edge.hexes.push(hex);
    return id;
  };
  tiles.forEach((tile, hex) => {
    const corners = [];
    for (let vertex = 0; vertex < 6; vertex += 1) {
      const angle = Math.PI / 6 + (vertex * Math.PI) / 3;
      corners.push(
        nodeAt(
          tile.x + Math.cos(angle) * circumradius,
          tile.z + Math.sin(angle) * circumradius,
        ),
      );
    }
    for (let vertex = 0; vertex < 6; vertex += 1) {
      const a = corners[vertex];
      const b = corners[(vertex + 1) % 6];
      const forward = directed(a, b, hex);
      const backward = directed(b, a, hex);
      graph.edges[forward].reverse = backward;
      graph.edges[backward].reverse = forward;
    }
  });
  graph.edges.forEach((incoming) => {
    graph.nodes[incoming.to].out.forEach((outId) => {
      if (outId === incoming.reverse) return;
      const outgoing = graph.edges[outId];
      const hex = incoming.hexes.find((tile) => outgoing.hexes.includes(tile));
      if (hex === undefined) return;
      const turn = roadTurnArc(graph, incoming, outgoing, hex);
      turn.id = graph.turns.length;
      graph.turns.push(turn);
      incoming.turns.push(turn);
      outgoing.incoming.push(turn);
    });
  });
  graph.edges.forEach((incoming) => {
    const node = graph.nodes[incoming.to];
    if (node.out.length !== 3) return;
    node.out.forEach((outId) => {
      if (outId === incoming.reverse) return;
      const outgoing = graph.edges[outId];
      if (incoming.hexes.some((tile) => outgoing.hexes.includes(tile))) return;
      const turn = roadTurnArc(graph, incoming, outgoing, outgoing.hexes[0]);
      turn.id = graph.turns.length;
      graph.turns.push(turn);
      incoming.turns.push(turn);
      outgoing.incoming.push(turn);
    });
  });
  graph.edges.forEach((edge) => {
    edge.outerTurn = null;
    edge.outerIncoming = null;
    if (edge.hexes.length !== 1) return;
    edge.outerTurn =
      edge.turns.find((turn) => graph.edges[turn.edge].hexes.length === 1) || null;
    edge.outerIncoming =
      edge.incoming.find((turn) => graph.edges[turn.from].hexes.length === 1) || null;
  });
  return graph;
}

let worldRoadGraphCache = null;

export function worldRoadGraph() {
  if (!worldRoadGraphCache) worldRoadGraphCache = createRoadGraph();
  return worldRoadGraphCache;
}

export function roadDefaultTurn(edge, hex) {
  if (edge.outerTurn) return edge.outerTurn;
  const turns = edge.turns;
  for (let index = 0; index < turns.length; index += 1) {
    if (turns[index].hex === hex) return turns[index];
  }
  return turns[0];
}

export function roadDefaultIncoming(edge, hex) {
  if (edge.outerIncoming) return edge.outerIncoming;
  const turns = edge.incoming;
  for (let index = 0; index < turns.length; index += 1) {
    if (turns[index].hex === hex) return turns[index];
  }
  return turns[0];
}

export function roadTurnForSteer(edge, hex, steer) {
  if (!(Math.abs(steer) >= ROAD_TURN_STEER_THRESHOLD)) {
    return roadDefaultTurn(edge, hex);
  }
  const side = steer > 0 ? 1 : -1;
  const turns = edge.turns;
  for (let index = 0; index < turns.length; index += 1) {
    if (turns[index].side === side) return turns[index];
  }
  return roadDefaultTurn(edge, hex);
}

export function createRoadRoute(graph = worldRoadGraph()) {
  const edge = graph.edges[0];
  return {
    graph,
    edge: edge.id,
    hex: edge.hexes[0],
    next: roadDefaultTurn(edge, edge.hexes[0]),
    s: 0,
    choice: 0,
  };
}

export function roadRoutePlaceOnLoop(route, distance, hex = 0) {
  const graph = route.graph;
  const tile = WORLD_HEX_TILES[hex];
  const circumradius = graph.straightLength + 2 * graph.cornerInset;
  const total = 6 * graph.pieceLength;
  const along =
    ((((Number(distance) || 0) + graph.straightLength / 2) % total) + total) %
    total;
  const side = Math.min(5, Math.floor(along / graph.pieceLength));
  const fromAngle = Math.PI / 6 + ((side - 1) * Math.PI) / 3;
  const toAngle = fromAngle + Math.PI / 3;
  const fromX = tile.x + Math.cos(fromAngle) * circumradius;
  const fromZ = tile.z + Math.sin(fromAngle) * circumradius;
  const toX = tile.x + Math.cos(toAngle) * circumradius;
  const toZ = tile.z + Math.sin(toAngle) * circumradius;
  let best = graph.edges[0];
  let bestMiss = Infinity;
  graph.edges.forEach((edge) => {
    const a = graph.nodes[edge.from];
    const b = graph.nodes[edge.to];
    const miss =
      Math.hypot(a.x - fromX, a.z - fromZ) + Math.hypot(b.x - toX, b.z - toZ);
    if (miss < bestMiss) {
      bestMiss = miss;
      best = edge;
    }
  });
  route.edge = best.id;
  route.hex = hex;
  route.next = roadDefaultTurn(best, hex);
  route.s = along - side * graph.pieceLength;
  route.choice = 0;
  return route;
}

export function roadRouteAdvance(route, distance, steer = 0) {
  const graph = route.graph;
  let edge = graph.edges[route.edge];
  if (
    route.s < graph.straightLength &&
    graph.straightLength - route.s <= ROAD_TURN_CHOICE_DISTANCE &&
    Math.abs(steer) >= ROAD_TURN_STEER_THRESHOLD
  ) {
    const chosen = roadTurnForSteer(edge, route.hex, steer);
    route.next = chosen;
    route.choice = chosen.side;
  }
  route.s += Number(distance) || 0;
  while (route.s >= graph.pieceLength) {
    route.s -= graph.pieceLength;
    const turn = route.next;
    route.hex = turn.hex;
    route.edge = turn.edge;
    edge = graph.edges[turn.edge];
    route.next = roadDefaultTurn(edge, route.hex);
    route.choice = 0;
  }
  while (route.s < 0) {
    route.s += graph.pieceLength;
    const turn = roadDefaultIncoming(edge, route.hex);
    route.next = turn;
    route.edge = turn.from;
    route.hex = turn.hex;
    edge = graph.edges[turn.from];
    route.choice = 0;
  }
  return route;
}

export function roadRouteTurnPending(route) {
  const graph = route.graph;
  if (route.s >= graph.straightLength) return route.next.side;
  if (graph.straightLength - route.s > ROAD_TURN_CHOICE_DISTANCE) return 0;
  return route.choice || route.next.side;
}

export function roadRoutePoint(route, target) {
  const graph = route.graph;
  const edge = graph.edges[route.edge];
  let directionX = edge.ux;
  let directionZ = edge.uz;
  if (route.s < graph.straightLength) {
    const node = graph.nodes[edge.from];
    const along = graph.cornerInset + route.s;
    target.x = node.x + directionX * along;
    target.z = node.z + directionZ * along;
  } else {
    const turn = route.next;
    const angle =
      turn.startAngle +
      (turn.sweep * (route.s - graph.straightLength)) / graph.arcLength;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    target.x = turn.centerX + cos * graph.cornerRadius;
    target.z = turn.centerZ + sin * graph.cornerRadius;
    const sign = turn.sweep > 0 ? 1 : -1;
    directionX = -sin * sign;
    directionZ = cos * sign;
  }
  target.heading = Math.atan2(-directionX, -directionZ);
  return target;
}

export const ROAD_SNAP_DISTANCE = 3;
export const ROAD_SNAP_ANGLE = (50 * Math.PI) / 180;
export const ROAD_SNAP_CELL = 32;
const ROAD_SNAP_COS = Math.cos(ROAD_SNAP_ANGLE);

function roadSnapItemBounds(graph, item, bounds) {
  const edgeCount = graph.edges.length;
  if (item < edgeCount) {
    const edge = graph.edges[item];
    const node = graph.nodes[edge.from];
    const ax = node.x + edge.ux * graph.cornerInset;
    const az = node.z + edge.uz * graph.cornerInset;
    const bx = ax + edge.ux * graph.straightLength;
    const bz = az + edge.uz * graph.straightLength;
    bounds[0] = Math.min(ax, bx);
    bounds[1] = Math.min(az, bz);
    bounds[2] = Math.max(ax, bx);
    bounds[3] = Math.max(az, bz);
  } else {
    const turn = graph.turns[item - edgeCount];
    bounds[0] = turn.centerX - graph.cornerRadius;
    bounds[1] = turn.centerZ - graph.cornerRadius;
    bounds[2] = turn.centerX + graph.cornerRadius;
    bounds[3] = turn.centerZ + graph.cornerRadius;
  }
  bounds[0] -= ROAD_SNAP_DISTANCE;
  bounds[1] -= ROAD_SNAP_DISTANCE;
  bounds[2] += ROAD_SNAP_DISTANCE;
  bounds[3] += ROAD_SNAP_DISTANCE;
  return bounds;
}

export function createRoadSnapGrid(graph = worldRoadGraph(), cell = ROAD_SNAP_CELL) {
  const itemCount = graph.edges.length + graph.turns.length;
  const bounds = [0, 0, 0, 0];
  let minX = Infinity;
  let minZ = Infinity;
  let maxX = -Infinity;
  let maxZ = -Infinity;
  for (let item = 0; item < itemCount; item += 1) {
    roadSnapItemBounds(graph, item, bounds);
    minX = Math.min(minX, bounds[0]);
    minZ = Math.min(minZ, bounds[1]);
    maxX = Math.max(maxX, bounds[2]);
    maxZ = Math.max(maxZ, bounds[3]);
  }
  const cols = Math.max(1, Math.ceil((maxX - minX) / cell));
  const rows = Math.max(1, Math.ceil((maxZ - minZ) / cell));
  const buckets = Array.from({ length: cols * rows }, () => []);
  for (let item = 0; item < itemCount; item += 1) {
    roadSnapItemBounds(graph, item, bounds);
    const c0 = Math.max(0, Math.floor((bounds[0] - minX) / cell));
    const r0 = Math.max(0, Math.floor((bounds[1] - minZ) / cell));
    const c1 = Math.min(cols - 1, Math.floor((bounds[2] - minX) / cell));
    const r1 = Math.min(rows - 1, Math.floor((bounds[3] - minZ) / cell));
    for (let row = r0; row <= r1; row += 1) {
      for (let col = c0; col <= c1; col += 1) {
        const x0 = minX + col * cell;
        const z0 = minZ + row * cell;
        if (roadSnapItemDistance(graph, item, x0 + cell / 2, z0 + cell / 2) <=
          ROAD_SNAP_DISTANCE + cell * Math.SQRT1_2) {
          buckets[row * cols + col].push(item);
        }
      }
    }
  }
  const start = new Int32Array(cols * rows + 1);
  let total = 0;
  buckets.forEach((bucket, index) => {
    start[index] = total;
    total += bucket.length;
  });
  start[cols * rows] = total;
  const items = new Int32Array(total);
  buckets.forEach((bucket, index) => items.set(bucket, start[index]));
  return { graph, cell, minX, minZ, cols, rows, start, items };
}

function roadSnapItemDistance(graph, item, x, z) {
  const edgeCount = graph.edges.length;
  if (item < edgeCount) {
    const edge = graph.edges[item];
    const node = graph.nodes[edge.from];
    const px = x - node.x;
    const pz = z - node.z;
    const along = Math.max(
      graph.cornerInset,
      Math.min(graph.cornerInset + graph.straightLength, px * edge.ux + pz * edge.uz),
    );
    return Math.hypot(px - edge.ux * along, pz - edge.uz * along);
  }
  const turn = graph.turns[item - edgeCount];
  return Math.abs(Math.hypot(x - turn.centerX, z - turn.centerZ) - graph.cornerRadius);
}

let worldRoadSnapGridCache = null;

export function worldRoadSnapGrid() {
  if (!worldRoadSnapGridCache) worldRoadSnapGridCache = createRoadSnapGrid();
  return worldRoadSnapGridCache;
}

export function createRoadSnapHit() {
  return { found: false, near: false, item: -1, s: 0, distance: 0 };
}

export function roadSnapFind(grid, x, z, heading, hit) {
  hit.found = false;
  hit.near = false;
  hit.item = -1;
  hit.s = 0;
  hit.distance = Infinity;
  const col = Math.floor((x - grid.minX) / grid.cell);
  const row = Math.floor((z - grid.minZ) / grid.cell);
  if (col < 0 || row < 0 || col >= grid.cols || row >= grid.rows) return false;
  const graph = grid.graph;
  const edgeCount = graph.edges.length;
  const forwardX = -Math.sin(heading);
  const forwardZ = -Math.cos(heading);
  const cellIndex = row * grid.cols + col;
  const end = grid.start[cellIndex + 1];
  for (let index = grid.start[cellIndex]; index < end; index += 1) {
    const item = grid.items[index];
    let distance = Infinity;
    let s = 0;
    let directionX = 0;
    let directionZ = 0;
    if (item < edgeCount) {
      const edge = graph.edges[item];
      const node = graph.nodes[edge.from];
      const px = x - node.x;
      const pz = z - node.z;
      const along = px * edge.ux + pz * edge.uz - graph.cornerInset;
      if (along < 0 || along >= graph.straightLength) continue;
      distance = Math.abs(px * edge.uz - pz * edge.ux);
      s = along;
      directionX = edge.ux;
      directionZ = edge.uz;
    } else {
      const turn = graph.turns[item - edgeCount];
      const dx = x - turn.centerX;
      const dz = z - turn.centerZ;
      const angle = Math.atan2(dz, dx);
      let offset = angle - turn.startAngle;
      offset = Math.atan2(Math.sin(offset), Math.cos(offset));
      const fraction = offset / turn.sweep;
      if (fraction < 0 || fraction > 1) continue;
      distance = Math.abs(Math.hypot(dx, dz) - graph.cornerRadius);
      s = graph.straightLength + fraction * graph.arcLength;
      const sign = turn.sweep > 0 ? 1 : -1;
      directionX = -Math.sin(angle) * sign;
      directionZ = Math.cos(angle) * sign;
    }
    if (distance > ROAD_SNAP_DISTANCE) continue;
    hit.near = true;
    if (forwardX * directionX + forwardZ * directionZ < ROAD_SNAP_COS) continue;
    if (distance >= hit.distance) continue;
    hit.found = true;
    hit.item = item;
    hit.s = s;
    hit.distance = distance;
  }
  return hit.found;
}

export function roadRouteSnap(route, hit) {
  const graph = route.graph;
  const edgeCount = graph.edges.length;
  if (hit.item < edgeCount) {
    const edge = graph.edges[hit.item];
    route.edge = edge.id;
    route.hex = edge.hexes[0];
    route.next = roadDefaultTurn(edge, route.hex);
  } else {
    const turn = graph.turns[hit.item - edgeCount];
    route.edge = turn.from;
    route.hex = turn.hex;
    route.next = turn;
  }
  route.s = hit.s;
  route.choice = 0;
  return route;
}

export function createHexRoadNetwork(THREE) {
  const group = new THREE.Group();
  group.name = "forkmesh-hex-roads";
  const positions = [];
  const indices = [];
  hexOutlineRingPositions(
    WORLD_HEX_TILES,
    CONTINUOUS_CITY_RADIUS,
    WORLD_ROAD_WIDTH,
    positions,
    indices,
  );
  roadBendPositions(WORLD_ROAD_JUNCTIONS, positions, indices);
  WORLD_ROAD_JUNCTIONS.forEach((junction) => {
    const base = positions.length / 3;
    for (let arm = 0; arm < 3; arm += 1) {
      const angle = junction.angle + (arm * Math.PI * 2) / 3;
      positions.push(
        junction.x + Math.cos(angle) * WORLD_ROAD_JUNCTION_REACH,
        0,
        junction.z + Math.sin(angle) * WORLD_ROAD_JUNCTION_REACH,
      );
    }
    indices.push(base, base + 1, base + 2);
  });
  const asphalt = new THREE.Mesh(
    flatRoadGeometry(THREE, positions, indices),
    makeSharedMaterial(THREE, "#2b3433", { roughness: 0.92, metalness: 0 }),
  );
  asphalt.name = "forkmesh-world-bike-lane";
  asphalt.position.y = WORLD_ROAD_SURFACE_Y;
  asphalt.receiveShadow = true;
  asphalt.userData.ground = true;
  group.add(asphalt);

  const markingMaterial = (color) =>
    sharedMaterial(THREE, "MeshStandardMaterial", {
      ...standardMaterialParameters(THREE, color, {
        roughness: 0.86,
        metalness: 0,
      }),
      polygonOffset: true,
      polygonOffsetFactor: -2,
      polygonOffsetUnits: -2,
    });
  const edgePositions = [];
  const edgeIndices = [];
  hexOutlineRingPositions(
    WORLD_HEX_TILES,
    CONTINUOUS_CITY_RADIUS + 0.45,
    0.4,
    edgePositions,
    edgeIndices,
  );
  const edges = new THREE.Mesh(
    flatRoadGeometry(THREE, edgePositions, edgeIndices),
    markingMaterial("#e6e0a5"),
  );
  edges.name = "forkmesh-world-road-edge-lines";
  edges.position.y = WORLD_ROAD_MARKING_Y;
  group.add(edges);

  const dashes = hexRoadDashCenters();
  const dashGeometry = flatRoadGeometry(
    THREE,
    [-0.16, 0, -1.6, 0.16, 0, -1.6, 0.16, 0, 1.6, -0.16, 0, 1.6],
    [0, 1, 2, 0, 2, 3],
  );
  const centerLine = new THREE.InstancedMesh(
    dashGeometry,
    markingMaterial("#f1eee3"),
    dashes.length,
  );
  centerLine.name = "forkmesh-world-bike-center-groove";
  const transform = new THREE.Object3D();
  dashes.forEach(([x, z, heading], index) => {
    transform.position.set(x, WORLD_ROAD_MARKING_Y, z);
    transform.rotation.set(0, heading, 0);
    transform.updateMatrix();
    centerLine.setMatrixAt(index, transform.matrix);
  });
  centerLine.instanceMatrix.needsUpdate = true;
  centerLine.computeBoundingSphere();
  group.add(centerLine);
  return group;
}

export function build({
  THREE,
  registerWorldElement,
  world,
}) {
  const worldRoads = createHexRoadNetwork(THREE);
  world.add(worldRoads);
  registerWorldElement(
    "bike-lane", "Hex roads & bike loop", "Vehicles & rides",
    worldRoads,
  );
  return {
  };
}

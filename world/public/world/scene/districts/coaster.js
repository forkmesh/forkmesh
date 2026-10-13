import {
  coasterDronePoint,
  coasterPlanFrame,
  coasterSample,
  coasterStationDistance,
  coasterTrack,
} from "../coaster.js";
import {
  COASTER_CAR_COUNT,
  COASTER_CAR_SPACING,
  COASTER_DETAIL_DISTANCE,
  COASTER_RAIL_GAUGE,
} from "../gameplay-constants.js";
import { makeLabelSprite } from "../labels.js";
import {
  BEACH_CENTER_Z,
  BEACH_ROAD_HALF_WIDTH,
  BEACH_ROAD_MIN_X,
  BEACH_TOWN_PORTAL_X,
} from "../layout.js";
import { material } from "../resources.js";

const RAIL_COLOR = "#e6edf3";
const SPINE_COLOR = "#1f6feb";
const ACCENT_COLOR = "#f7c96b";
const TIE_COLOR = "#30363d";
const SUPPORT_COLOR = "#8fb8ff";
const FOOTING_COLOR = "#6e7681";
const TUNNEL_COLOR = "#2d333b";
const TUNNEL_INNER_COLOR = "#0d1117";
const CHUNK_LENGTH = 220;
const DETAIL_STEP = 3;
const COARSE_STEP = 7;
const TIE_SPACING = 3;
const SUPPORT_SPACING = 15;
const STATION_START_F = 6;
const STATION_END_F = 66;

function createMeshBuilder() {
  const positions = [];
  const normals = [];
  const colors = [];
  const indices = [];
  const tint = [0, 0, 0];
  function setTint(hex) {
    const value = parseInt(hex.slice(1), 16);
    tint[0] = ((value >> 16) & 255) / 255;
    tint[1] = ((value >> 8) & 255) / 255;
    tint[2] = (value & 255) / 255;
  }
  function vertex(x, y, z, nx, ny, nz) {
    positions.push(x, y, z);
    normals.push(nx, ny, nz);
    colors.push(tint[0], tint[1], tint[2]);
    return positions.length / 3 - 1;
  }
  function quad(a, b, c, d) {
    indices.push(a, b, c, a, c, d);
  }
  function box(center, axes, half, hex) {
    setTint(hex);
    for (let axis = 0; axis < 3; axis += 1) {
      for (const sign of [-1, 1]) {
        const normal = axes[axis].map((value) => value * sign);
        const u = axes[(axis + 1) % 3];
        const v = axes[(axis + 2) % 3];
        const hu = half[(axis + 1) % 3];
        const hv = half[(axis + 2) % 3];
        const face = center.map((value, k) => value + normal[k] * half[axis]);
        const corners = [[-1, -1], [1, -1], [1, 1], [-1, 1]].map(([su, sv]) =>
          vertex(
            face[0] + u[0] * hu * su + v[0] * hv * sv,
            face[1] + u[1] * hu * su + v[1] * hv * sv,
            face[2] + u[2] * hu * su + v[2] * hv * sv,
            normal[0],
            normal[1],
            normal[2],
          ),
        );
        if (sign > 0) quad(corners[0], corners[1], corners[2], corners[3]);
        else quad(corners[0], corners[3], corners[2], corners[1]);
      }
    }
  }
  function tube(frames, offsetSide, offsetUp, half, hex) {
    setTint(hex);
    let previous = null;
    const corners = [[-1, -1], [1, -1], [1, 1], [-1, 1]];
    for (const frame of frames) {
      if (!frame) {
        previous = null;
        continue;
      }
      const ring = corners.map(([cs, cu]) => {
        const ns = cs * frame.side[0] + cu * frame.up[0];
        const nu = cs * frame.side[1] + cu * frame.up[1];
        const nz = cs * frame.side[2] + cu * frame.up[2];
        const length = Math.hypot(ns, nu, nz) || 1;
        return vertex(
          frame.point[0] + frame.side[0] * (offsetSide + cs * half) + frame.up[0] * (offsetUp + cu * half),
          frame.point[1] + frame.side[1] * (offsetSide + cs * half) + frame.up[1] * (offsetUp + cu * half),
          frame.point[2] + frame.side[2] * (offsetSide + cs * half) + frame.up[2] * (offsetUp + cu * half),
          ns / length,
          nu / length,
          nz / length,
        );
      });
      if (previous) {
        for (let corner = 0; corner < 4; corner += 1) {
          const next = (corner + 1) % 4;
          quad(previous[corner], ring[corner], ring[next], previous[next]);
        }
      }
      previous = ring;
    }
  }
  function arch(frames, lift, radius, segments, outer, inner) {
    let previous = null;
    for (const frame of frames) {
      if (!frame) {
        previous = null;
        continue;
      }
      const ring = [];
      for (let step = 0; step <= segments; step += 1) {
        const angle = (Math.PI * step) / segments;
        const cs = Math.cos(angle);
        const su = Math.sin(angle);
        const n = [0, 1, 2].map((k) => frame.side[k] * cs + frame.up[k] * su);
        const p = [0, 1, 2].map(
          (k) => frame.point[k] + frame.up[k] * lift + n[k] * radius,
        );
        setTint(outer);
        const out = vertex(p[0], p[1], p[2], n[0], n[1], n[2]);
        setTint(inner);
        const inside = vertex(p[0], p[1], p[2], -n[0], -n[1], -n[2]);
        ring.push([out, inside]);
      }
      if (previous) {
        for (let step = 0; step < segments; step += 1) {
          quad(previous[step][0], previous[step + 1][0], ring[step + 1][0], ring[step][0]);
          quad(previous[step][1], ring[step][1], ring[step + 1][1], previous[step + 1][1]);
        }
      }
      previous = ring;
    }
  }
  function geometry(THREE, origin) {
    const shifted = new Float32Array(positions.length);
    for (let index = 0; index < positions.length; index += 3) {
      shifted[index] = positions[index] - origin[0];
      shifted[index + 1] = positions[index + 1] - origin[1];
      shifted[index + 2] = positions[index + 2] - origin[2];
    }
    const built = new THREE.BufferGeometry();
    built.setAttribute("position", new THREE.BufferAttribute(shifted, 3));
    built.setAttribute("normal", new THREE.Float32BufferAttribute(normals, 3));
    built.setAttribute("color", new THREE.Float32BufferAttribute(colors, 3));
    built.setIndex(indices);
    built.computeBoundingSphere();
    return built;
  }
  return { box, tube, arch, geometry, empty: () => indices.length === 0 };
}

function frameAt(track, index) {
  const point = [track.x[index], track.y[index], track.z[index]];
  const forward = [track.tx[index], track.ty[index], track.tz[index]];
  const up = [track.ux[index], track.uy[index], track.uz[index]];
  const side = [
    forward[1] * up[2] - forward[2] * up[1],
    forward[2] * up[0] - forward[0] * up[2],
    forward[0] * up[1] - forward[1] * up[0],
  ];
  return { index, point, forward, up, side };
}

function pickFrames(track, from, to, step) {
  const frames = [];
  let last = null;
  for (let index = from; index <= to; index += 1) {
    const section = track.sections[track.piece[index]];
    if (!section.rails) {
      if (frames.length && frames[frames.length - 1]) frames.push(null);
      last = null;
      continue;
    }
    const frame = frameAt(track, index);
    if (
      last &&
      index !== to &&
      (index - last.index) * track.step < step &&
      frame.forward[0] * last.forward[0] +
        frame.forward[1] * last.forward[1] +
        frame.forward[2] * last.forward[2] >
        0.994 &&
      frame.up[0] * last.up[0] + frame.up[1] * last.up[1] + frame.up[2] * last.up[2] >
        0.994
    ) {
      continue;
    }
    frames.push(frame);
    last = frame;
  }
  return frames;
}

function accentAt(track, index) {
  const section = track.sections[track.piece[index]];
  const next = track.sections[section.index + 1];
  const previous = track.sections[section.index - 1];
  return section.landing || (next && !next.rails) || (previous && !previous.rails);
}

function dronePath() {
  const points = [];
  for (let angle = 0; angle < Math.PI * 2; angle += 0.002) {
    points.push(coasterDronePoint(angle));
  }
  return points;
}

function supportAllowed(track, index, top, drone) {
  const [x, y, z] = top;
  if (
    x > BEACH_ROAD_MIN_X - 4 &&
    Math.abs(z - BEACH_CENTER_Z) < BEACH_ROAD_HALF_WIDTH + 3
  ) {
    return false;
  }
  if (Math.hypot(x - BEACH_TOWN_PORTAL_X, z - BEACH_CENTER_Z) < 9) return false;
  for (let other = 0; other < track.count; other += 2) {
    const gap = Math.abs(other - index) * track.step;
    if (Math.min(gap, track.length - gap) < 12) continue;
    if (track.y[other] > y) continue;
    if (Math.hypot(track.x[other] - x, track.z[other] - z) < 3.2) return false;
  }
  for (const point of drone) {
    if (point.y > y + 3) continue;
    if (Math.hypot(point.x - x, point.z - z) < 6) return false;
  }
  return true;
}

function supportTops(track, from, to, drone) {
  const tops = [];
  let last = -Infinity;
  let retry = -Infinity;
  for (let index = from; index <= to; index += 1) {
    const d = index * track.step;
    if (d - last < SUPPORT_SPACING || d < retry) continue;
    const section = track.sections[track.piece[index]];
    if (!section.rails || section.inversion || section.station) continue;
    if (track.uy[index] < 0.8 || track.y[index] < 3) continue;
    const frame = frameAt(track, index);
    const top = frame.point.map((value, k) => value - frame.up[k] * 0.95);
    if (!supportAllowed(track, index, top, drone)) {
      retry = d + 2;
      continue;
    }
    last = d;
    tops.push(top);
  }
  return tops;
}

function addSupports(builder, tops) {
  for (const top of tops) {
    const height = top[1];
    builder.box(
      [top[0], height / 2, top[2]],
      [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
      [0.42, height / 2, 0.42],
      SUPPORT_COLOR,
    );
    builder.box(
      [top[0], 0.16, top[2]],
      [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
      [1, 0.16, 1],
      FOOTING_COLOR,
    );
  }
}

function addTrackDetail(builder, track, frames) {
  const gauge = COASTER_RAIL_GAUGE / 2;
  builder.tube(frames, -gauge, 0, 0.17, RAIL_COLOR);
  builder.tube(frames, gauge, 0, 0.17, RAIL_COLOR);
  const runs = [[]];
  for (const frame of frames) {
    if (!frame) runs.push([]);
    else runs[runs.length - 1].push(frame);
  }
  for (const run of runs) {
    let group = [];
    let accent = null;
    for (const frame of run) {
      const next = accentAt(track, frame.index);
      group.push(frame);
      if (accent !== null && next !== accent) {
        builder.tube(group, 0, -0.78, 0.32, accent ? ACCENT_COLOR : SPINE_COLOR);
        group = [frame];
      }
      accent = next;
    }
    if (group.length > 1) {
      builder.tube(group, 0, -0.78, 0.32, accent ? ACCENT_COLOR : SPINE_COLOR);
    }
  }
  let lastTie = -Infinity;
  for (const frame of frames) {
    if (!frame) continue;
    const d = frame.index * track.step;
    if (d - lastTie < TIE_SPACING) continue;
    lastTie = d;
    const center = frame.point.map((value, k) => value - frame.up[k] * 0.4);
    builder.box(
      center,
      [frame.side, frame.up, frame.forward],
      [gauge + 0.12, 0.42, 0.13],
      TIE_COLOR,
    );
  }
}

function tunnelFrames(track, from, to) {
  const frames = [];
  for (let index = from; index <= to; index += 1) {
    if (!track.sections[track.piece[index]].tunnel) continue;
    if (frames.length && (index - frames[frames.length - 1].index) * track.step < 2.5) {
      continue;
    }
    frames.push(frameAt(track, index));
  }
  return frames;
}

export function functions({
  THREE,
}) {
  const sample = {};
  const matrix = new THREE.Matrix4();
  const right = new THREE.Vector3();
  const up = new THREE.Vector3();
  const back = new THREE.Vector3();

  function coasterMaterial() {
    return material(THREE, { color: "#ffffff", vertexColors: true });
  }

  function createCoasterTrack() {
    const track = coasterTrack();
    const drone = dronePath();
    const group = new THREE.Group();
    group.name = "forkmesh-coaster-track";
    const shared = coasterMaterial();
    const chunkSamples = Math.round(CHUNK_LENGTH / track.step);
    let triangles = 0;
    for (let from = 0, chunk = 0; from < track.count - 1; from += chunkSamples, chunk += 1) {
      const to = Math.min(track.count - 1, from + chunkSamples);
      const detail = createMeshBuilder();
      addTrackDetail(detail, track, pickFrames(track, from, to, DETAIL_STEP));
      const tops = supportTops(track, from, to, drone);
      addSupports(detail, tops);
      const tunnel = tunnelFrames(track, from, to);
      if (tunnel.length > 1) detail.arch(tunnel, 0.9, 3.6, 8, TUNNEL_COLOR, TUNNEL_INNER_COLOR);
      const coarse = createMeshBuilder();
      coarse.tube(pickFrames(track, from, to, COARSE_STEP), 0, -0.4, 0.62, SPINE_COLOR);
      addSupports(coarse, tops.filter((_, k) => k % 2 === 0));
      if (tunnel.length > 1) coarse.arch(tunnel.filter((_, k) => k % 2 === 0), 0.9, 3.6, 4, TUNNEL_COLOR, TUNNEL_INNER_COLOR);
      const middle = from + Math.floor((to - from) / 2);
      const origin = [track.x[middle], 0, track.z[middle]];
      const lod = new THREE.LOD();
      lod.name = `forkmesh-coaster-track-${chunk + 1}`;
      lod.position.set(...origin);
      const near = new THREE.Mesh(detail.geometry(THREE, origin), shared);
      near.name = `forkmesh-coaster-track-${chunk + 1}-detailed`;
      const far = new THREE.Mesh(coarse.geometry(THREE, origin), shared);
      far.name = `forkmesh-coaster-track-${chunk + 1}-coarse`;
      for (const mesh of [near, far]) {
        mesh.castShadow = false;
        mesh.receiveShadow = false;
      }
      triangles += near.geometry.index.count / 3;
      lod.addLevel(near, 0, 0.1);
      lod.addLevel(far, COASTER_DETAIL_DISTANCE, 0.1);
      group.add(lod);
    }
    group.userData.detailedTriangles = triangles;
    return group;
  }

  function createCoasterStation() {
    const track = coasterTrack();
    const middle = (STATION_START_F + STATION_END_F) / 2;
    const plan = coasterPlanFrame(middle, {});
    const station = new THREE.Group();
    station.name = "forkmesh-coaster-station";
    station.position.set(plan.x, 0, plan.z);
    station.rotation.y = Math.atan2(-plan.tx, -plan.tz);
    const builder = createMeshBuilder();
    const length = STATION_END_F - STATION_START_F;
    const axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
    const platformX = COASTER_RAIL_GAUGE / 2 + 3.4;
    builder.box([platformX, 0.18, 0], axes, [2.6, 0.18, length / 2], "#c9d1d9");
    builder.box([platformX - 2.45, 0.38, 0], axes, [0.15, 0.04, length / 2], ACCENT_COLOR);
    for (let post = -2; post <= 2; post += 1) {
      const z = (post * length) / 4.4;
      for (const x of [platformX + 2.3, -(COASTER_RAIL_GAUGE / 2 + 2.2)]) {
        builder.box([x, 3.3, z], axes, [0.25, 3.3, 0.25], "#8b949e");
      }
    }
    builder.box([1.5, 6.75, 0], axes, [6.4, 0.18, length / 2 + 2], SPINE_COLOR);
    builder.box([1.5, 7.05, 0], axes, [5.6, 0.14, length / 2 + 1], "#79c0ff");
    const mesh = new THREE.Mesh(builder.geometry(THREE, [0, 0, 0]), coasterMaterial());
    mesh.name = "forkmesh-coaster-station-structure";
    mesh.castShadow = true;
    mesh.receiveShadow = true;
    station.add(mesh);
    const sign = makeLabelSprite(
      THREE,
      "FORKMESH COASTER",
      "LOOPS · JUMP · CORKSCREW · HELIX",
      "#79c0ff",
    );
    sign.name = "forkmesh-coaster-station-sign";
    sign.position.set(1.5, 10.6, 0);
    sign.scale.set(16, 5.3, 1);
    station.add(sign);
    const hint = makeLabelSprite(
      THREE,
      "RIDE THE COASTER",
      "CLICK THE TRAIN OR PRESS E",
      "#f7c96b",
    );
    hint.name = "forkmesh-coaster-control-hint";
    const board = coasterSample(coasterStationDistance(), {});
    const local = station.worldToLocal(new THREE.Vector3(board.x, board.y, board.z));
    hint.position.set(local.x, 4.6, local.z);
    hint.scale.set(7.2, 2.4, 1);
    station.add(hint);
    station.userData.track = track;
    return { station, hint };
  }

  function createCoasterTrain() {
    const builder = createMeshBuilder();
    const axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
    builder.box([0, 0.12, 0], axes, [1.35, 0.2, 1.9], "#6e7681");
    builder.box([0, 0.55, 0], axes, [1.18, 0.25, 1.95], SPINE_COLOR);
    builder.box([-1.1, 1.0, 0.1], axes, [0.09, 0.3, 1.75], "#79c0ff");
    builder.box([1.1, 1.0, 0.1], axes, [0.09, 0.3, 1.75], "#79c0ff");
    builder.box([0, 0.95, -1.85], axes, [1.18, 0.4, 0.22], ACCENT_COLOR);
    for (const z of [-0.45, 1.15]) {
      builder.box([0, 0.86, z], axes, [0.98, 0.07, 0.45], "#161b22");
      builder.box([0, 1.32, z + 0.48], axes, [0.98, 0.42, 0.08], "#161b22");
      builder.box([0, 1.42, z - 0.32], axes, [0.9, 0.05, 0.05], "#f0f6fc");
    }
    const train = new THREE.InstancedMesh(
      builder.geometry(THREE, [0, 0, 0]),
      coasterMaterial(),
      COASTER_CAR_COUNT,
    );
    train.name = "forkmesh-coaster-train";
    train.frustumCulled = false;
    train.castShadow = true;
    train.receiveShadow = false;
    train.userData.coasterTrain = true;
    return train;
  }

  function placeCoasterTrain(train, leadDistance) {
    for (let car = 0; car < train.count; car += 1) {
      coasterSample(leadDistance - car * COASTER_CAR_SPACING, sample);
      back.set(-sample.tx, -sample.ty, -sample.tz);
      up.set(sample.ux, sample.uy, sample.uz);
      right.crossVectors(up, back).normalize();
      matrix.makeBasis(right, up, back);
      matrix.setPosition(sample.x, sample.y, sample.z);
      train.setMatrixAt(car, matrix);
    }
    train.instanceMatrix.needsUpdate = true;
  }

  return {
    createCoasterStation,
    createCoasterTrack,
    createCoasterTrain,
    placeCoasterTrain,
  };
}

export async function build({
  THREE,
  createCoasterStation,
  createCoasterTrack,
  createCoasterTrain,
  ctx,
  placeCoasterTrain,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  const track = createCoasterTrack();
  world.add(track);
  await sceneSlice();
  const { station, hint } = createCoasterStation();
  const train = createCoasterTrain();
  world.add(station, train);
  ctx.interactive.push(train);
  const coaster = {
    track,
    station,
    hint,
    train,
    stationDistance: coasterStationDistance(),
  };
  ctx.coaster = coaster;
  placeCoasterTrain(train, coaster.stationDistance);
  registerWorldElement(
    "coaster",
    "ForkMesh coaster",
    "Vehicles & rides",
    [track, station, train],
  );
  return { coaster };
}

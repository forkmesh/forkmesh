import {
  COASTER_APOTHEM,
  COASTER_CHAIN_SPEED,
  COASTER_CORNER_RADIUS,
  COASTER_GRAVITY,
  COASTER_LIFT_HEIGHT,
  COASTER_SAMPLE_STEP,
  COASTER_STATION_SPEED,
  DRONE_COURSE_FLOOR,
  DRONE_COURSE_RING_COUNT,
  DRONE_COURSE_START,
  DRONE_COURSE_WEAVE_HEIGHT,
  DRONE_COURSE_WEAVE_WIDTH,
} from "./gameplay-constants.js";
import { WORLD_HEX_TILES } from "./layout.js";
import {
  roundedHexagonLoopLength,
  roundedHexagonLoopPoint,
} from "./movement.js";

const TAU = Math.PI * 2;
const DENSE_STEP = 0.2;
const DRONE_COURSE_RING_SIDES = Object.freeze([1, -1, 1, -1, 1, 1, -1, 1, -1, 1]);
const MODES = Object.freeze({ station: 0, lift: 1, free: 2, brake: 3 });

export const COASTER_PLAN_LENGTH = roundedHexagonLoopLength(
  COASTER_APOTHEM,
  COASTER_CORNER_RADIUS,
);
const COASTER_EDGE =
  ((COASTER_APOTHEM - COASTER_CORNER_RADIUS) * 2) / Math.sqrt(3);
const COASTER_SIDE = COASTER_EDGE + (COASTER_CORNER_RADIUS * Math.PI) / 3;
const COASTER_PLAN_START = 3 * COASTER_SIDE - COASTER_EDGE / 2;
export const DRONE_COURSE_UNITS_PER_RADIAN = COASTER_PLAN_LENGTH / TAU;

function smooth(p) {
  return p * p * (3 - 2 * p);
}

function hermite(y0, y1, m0, m1, p) {
  const p2 = p * p;
  const p3 = p2 * p;
  return (
    (2 * p3 - 3 * p2 + 1) * y0 +
    (p3 - 2 * p2 + p) * m0 +
    (-2 * p3 + 3 * p2) * y1 +
    (p3 - p2) * m1
  );
}

function wrapPlan(f) {
  return ((f % COASTER_PLAN_LENGTH) + COASTER_PLAN_LENGTH) % COASTER_PLAN_LENGTH;
}

export function coasterPlanFrame(f, target = {}) {
  const [town] = WORLD_HEX_TILES;
  const point = roundedHexagonLoopPoint(COASTER_PLAN_START + wrapPlan(f), {}, {
    centerX: town.x,
    centerZ: town.z,
    apothem: COASTER_APOTHEM,
    cornerRadius: COASTER_CORNER_RADIUS,
  });
  target.x = point.x;
  target.z = point.z;
  target.tx = -Math.sin(point.heading);
  target.tz = -Math.cos(point.heading);
  target.nx = -target.tz;
  target.nz = target.tx;
  return target;
}

function spline(f0, f1, y0, y1, slope0, slope1, options = {}) {
  const span = f1 - f0;
  const l0 = options.l0 || 0;
  const l1 = options.l1 ?? l0;
  const bank = options.bank || 0;
  return {
    ...options,
    f0,
    f1,
    length: Math.hypot(span, y1 - y0) * 1.3 + 1,
    at(p, out) {
      const b = bank * Math.sin(Math.PI * p);
      out.f = f0 + span * p;
      out.base = out.f;
      out.y = hermite(y0, y1, slope0 * span, slope1 * span, p);
      out.l = l0 + (l1 - l0) * smooth(p);
      out.uf = 0;
      out.ul = Math.sin(b);
      out.uy = Math.cos(b);
      out.env = out.y;
    },
  };
}

function loop(f0, y0, radius, l0, shift, options = {}) {
  return {
    ...options,
    f0,
    f1: f0,
    inversion: true,
    length: TAU * radius,
    at(p, out) {
      const angle = TAU * p;
      out.f = f0 + radius * Math.sin(angle);
      out.base = f0;
      out.y = y0 + radius * (1 - Math.cos(angle));
      out.l = l0 + (shift * (angle - Math.sin(angle))) / TAU;
      out.uf = -Math.sin(angle);
      out.ul = 0;
      out.uy = Math.cos(angle);
      out.env = y0;
    },
  };
}

function corkscrew(f0, f1, y0, radius, options = {}) {
  const span = f1 - f0;
  return {
    ...options,
    f0,
    f1,
    inversion: true,
    length: Math.hypot(span, TAU * radius) * 1.2,
    at(p, out) {
      const angle = TAU * p - Math.sin(TAU * p);
      out.f = f0 + span * p;
      out.base = out.f;
      out.y = y0 + radius * (1 - Math.cos(angle));
      out.l = -radius * Math.sin(angle);
      out.uf = 0;
      out.ul = Math.sin(angle);
      out.uy = Math.cos(angle);
      out.env = y0;
    },
  };
}

function helix(f0, y0, y1, radius, advance, bank, options = {}) {
  return {
    ...options,
    f0,
    f1: f0 + TAU * advance,
    length: TAU * (radius + advance) * 1.2,
    at(p, out) {
      const angle = TAU * p;
      const ramp =
        smooth(Math.min(1, p / 0.12)) * smooth(Math.min(1, (1 - p) / 0.12));
      const b = bank * ramp;
      out.f = f0 + radius * Math.sin(angle) + advance * angle;
      out.base = f0 + advance * angle;
      out.y = y0 + (y1 - y0) * smooth(p);
      out.l = radius * (1 - Math.cos(angle));
      out.uf = -Math.sin(b) * Math.sin(angle);
      out.ul = Math.sin(b) * Math.cos(angle);
      out.uy = Math.cos(b);
      out.env = out.y;
    },
  };
}

export function coasterSpeedAtHeight(y) {
  return Math.sqrt(
    Math.max(
      0,
      COASTER_CHAIN_SPEED ** 2 + 2 * COASTER_GRAVITY * (COASTER_LIFT_HEIGHT - y),
    ),
  );
}

function jump(fLip, yLip, slope, yLand, options = {}) {
  const speed = coasterSpeedAtHeight(yLip);
  const pitch = Math.atan(slope);
  const vf = speed * Math.cos(pitch);
  const vy = speed * Math.sin(pitch);
  const g = COASTER_GRAVITY;
  const time = (vy + Math.sqrt(vy * vy + 2 * g * (yLip - yLand))) / g;
  return {
    ...options,
    f0: fLip,
    f1: fLip + vf * time,
    rails: false,
    landingSlope: (vy - g * time) / vf,
    flightTime: time,
    apex: yLip + (vy * vy) / (2 * g),
    length: vf * time * 1.4,
    at(p, out) {
      const t = time * p;
      const climb = Math.atan2(vy - g * t, vf);
      out.f = fLip + vf * t;
      out.base = out.f;
      out.y = yLip + vy * t - 0.5 * g * t * t;
      out.l = 0;
      out.uf = -Math.sin(climb);
      out.ul = 0;
      out.uy = Math.cos(climb);
      out.env = out.y;
    },
  };
}

export function coasterLayout() {
  const end = COASTER_PLAN_LENGTH;
  const top = COASTER_LIFT_HEIGHT;
  const station = 1.2;
  const kick = Math.tan(Math.PI / 6);
  const leap = jump(1115, 22, kick, 10, { mark: "Freefall" });
  const landed = leap.f1;
  return [
    spline(0, 70, station, station, 0, 0, { mode: "station", station: true }),
    spline(70, 250, station, top, 0, 0, { mode: "lift", mark: "Lift hill" }),
    spline(250, 350, top, 6, 0, 0, { bank: 0.35, mark: "First drop" }),
    spline(350, 400, 6, 6, 0, 0),
    loop(400, 6, 15, 0, -7, { mark: "Double loop" }),
    spline(400, 440, 6, 6, 0, 0, { l0: -7 }),
    loop(440, 6, 15, -7, 7),
    spline(440, 620, 6, 30, 0, 0),
    spline(620, 740, 30, 30, 0, 0, { bank: 0.8, mark: "Banked turn" }),
    spline(740, 785, 30, 9, 0, 0, { mark: "Airtime hills" }),
    spline(785, 815, 9, 34, 0, 0),
    spline(815, 845, 34, 9, 0, 0),
    spline(845, 872, 9, 30, 0, 0),
    spline(872, 899, 30, 9, 0, 0),
    spline(899, 926, 9, 26, 0, 0),
    spline(926, 955, 26, 9, 0, 0),
    spline(955, 995, 9, 14, 0, 0),
    spline(995, 1080, 14, 7, 0, 0, { bank: 0.6 }),
    spline(1080, 1115, 7, 22, 0, kick, { mark: "Ramp jump" }),
    leap,
    spline(landed, landed + 25, 10, 4, leap.landingSlope, 0, { landing: true }),
    spline(landed + 25, 1340, 4, 30, 0, 0),
    spline(1340, 1460, 30, 30, 0, 0, { bank: 0.6 }),
    spline(1460, 1490, 30, 20, 0, 0),
    spline(1490, 1590, 20, 20, 0, 0),
    corkscrew(1590, 1660, 20, 7, { mark: "Corkscrew" }),
    spline(1660, 1700, 20, 30, 0, 0),
    spline(1700, 1830, 30, 36, 0, 0, { bank: 0.8 }),
    helix(1830, 36, 24, 24, 4, 0.9, { mark: "Helix" }),
    spline(1830 + TAU * 4, 1990, 24, 28, 0, 0),
    spline(1990, 2080, 28, 4, 0, 0),
    spline(2080, end, 4, station, 0, 0, {
      mode: "brake",
      tunnel: true,
      mark: "Tunnel",
    }),
  ];
}

function buildTrack() {
  const pieces = coasterLayout();
  const dense = [];
  const point = {};
  const plan = {};
  let distance = 0;
  let previous = null;
  const marks = [];
  const ranges = [];
  pieces.forEach((piece, index) => {
    const steps = Math.max(4, Math.ceil(piece.length / DENSE_STEP));
    const startDistance = distance;
    for (let step = index === 0 ? 0 : 1; step <= steps; step += 1) {
      piece.at(step / steps, point);
      coasterPlanFrame(point.f, plan);
      const sample = {
        x: plan.x + plan.nx * point.l,
        y: point.y,
        z: plan.z + plan.nz * point.l,
        ux: plan.tx * point.uf + plan.nx * point.ul,
        uy: point.uy,
        uz: plan.tz * point.uf + plan.nz * point.ul,
        f: point.f,
        base: point.base,
        env: point.env,
        piece: index,
      };
      if (previous) {
        distance += Math.hypot(
          sample.x - previous.x,
          sample.y - previous.y,
          sample.z - previous.z,
        );
      }
      sample.d = distance;
      dense.push(sample);
      previous = sample;
    }
    ranges.push({ start: startDistance, end: distance });
    if (piece.mark) marks.push({ name: piece.mark, d: startDistance });
  });
  const length = distance;
  const count = Math.floor(length / COASTER_SAMPLE_STEP) + 1;
  const arrays = {
    x: new Float32Array(count),
    y: new Float32Array(count),
    z: new Float32Array(count),
    tx: new Float32Array(count),
    ty: new Float32Array(count),
    tz: new Float32Array(count),
    ux: new Float32Array(count),
    uy: new Float32Array(count),
    uz: new Float32Array(count),
    f: new Float32Array(count),
    speed: new Float32Array(count),
    piece: new Uint8Array(count),
  };
  let cursor = 0;
  for (let index = 0; index < count; index += 1) {
    const d = index * COASTER_SAMPLE_STEP;
    while (cursor < dense.length - 2 && dense[cursor + 1].d < d) cursor += 1;
    const a = dense[cursor];
    const b = dense[cursor + 1];
    const t = b.d > a.d ? Math.min(1, Math.max(0, (d - a.d) / (b.d - a.d))) : 0;
    arrays.x[index] = a.x + (b.x - a.x) * t;
    arrays.y[index] = a.y + (b.y - a.y) * t;
    arrays.z[index] = a.z + (b.z - a.z) * t;
    arrays.ux[index] = a.ux + (b.ux - a.ux) * t;
    arrays.uy[index] = a.uy + (b.uy - a.uy) * t;
    arrays.uz[index] = a.uz + (b.uz - a.uz) * t;
    arrays.f[index] = a.f + (b.f - a.f) * t;
    arrays.piece[index] = t < 0.5 ? a.piece : b.piece;
  }
  for (let index = 0; index < count; index += 1) {
    const before = Math.max(0, index - 1);
    const after = Math.min(count - 1, index + 1);
    let tx = arrays.x[after] - arrays.x[before];
    let ty = arrays.y[after] - arrays.y[before];
    let tz = arrays.z[after] - arrays.z[before];
    const tl = Math.hypot(tx, ty, tz) || 1;
    tx /= tl;
    ty /= tl;
    tz /= tl;
    let ux = arrays.ux[index];
    let uy = arrays.uy[index];
    let uz = arrays.uz[index];
    const along = ux * tx + uy * ty + uz * tz;
    ux -= tx * along;
    uy -= ty * along;
    uz -= tz * along;
    const ul = Math.hypot(ux, uy, uz) || 1;
    arrays.tx[index] = tx;
    arrays.ty[index] = ty;
    arrays.tz[index] = tz;
    arrays.ux[index] = ux / ul;
    arrays.uy[index] = uy / ul;
    arrays.uz[index] = uz / ul;
  }
  const brakeIndex = pieces.findIndex((piece) => piece.mode === "brake");
  const brakeStart = ranges[brakeIndex].start;
  const brakeEntry = coasterSpeedAtHeight(
    arrays.y[Math.floor(brakeStart / COASTER_SAMPLE_STEP)],
  );
  for (let index = 0; index < count; index += 1) {
    const piece = pieces[arrays.piece[index]];
    const mode = MODES[piece.mode || "free"];
    if (mode === MODES.station) arrays.speed[index] = COASTER_STATION_SPEED;
    else if (mode === MODES.lift) arrays.speed[index] = COASTER_CHAIN_SPEED;
    else if (mode === MODES.brake) {
      const range = ranges[arrays.piece[index]];
      const progress = Math.min(
        1,
        Math.max(0, (index * COASTER_SAMPLE_STEP - range.start) / (range.end - range.start)),
      );
      arrays.speed[index] = Math.sqrt(
        brakeEntry ** 2 + (COASTER_STATION_SPEED ** 2 - brakeEntry ** 2) * progress,
      );
    } else arrays.speed[index] = coasterSpeedAtHeight(arrays.y[index]);
  }
  const envelope = new Float32Array(Math.ceil(COASTER_PLAN_LENGTH) + 2).fill(NaN);
  dense.forEach((sample) => {
    const slot = Math.round(wrapPlan(sample.base));
    if (slot < envelope.length && Number.isNaN(envelope[slot])) envelope[slot] = sample.env;
  });
  let last = envelope.findIndex((value) => !Number.isNaN(value));
  for (let slot = 0; slot < envelope.length; slot += 1) {
    if (!Number.isNaN(envelope[slot])) {
      last = slot;
      continue;
    }
    let next = slot + 1;
    while (next < envelope.length && Number.isNaN(envelope[next])) next += 1;
    const from = envelope[last];
    const to = next < envelope.length ? envelope[next] : envelope[0];
    const span = next - last;
    for (let fill = slot; fill < next && fill < envelope.length; fill += 1) {
      envelope[fill] = from + ((to - from) * (fill - last)) / span;
    }
    slot = next - 1;
  }
  const sections = pieces.map((piece, index) => ({
    index,
    start: ranges[index].start,
    end: ranges[index].end,
    f0: piece.f0,
    f1: piece.f1,
    mode: piece.mode || "free",
    rails: piece.rails !== false,
    inversion: piece.inversion === true,
    station: piece.station === true,
    tunnel: piece.tunnel === true,
    landing: piece.landing === true,
    mark: piece.mark || "",
    flightTime: piece.flightTime || 0,
    apex: piece.apex || 0,
  }));
  return {
    length,
    step: COASTER_SAMPLE_STEP,
    count,
    ...arrays,
    envelope,
    sections,
    marks,
  };
}

let cachedTrack = null;

export function coasterTrack() {
  if (!cachedTrack) cachedTrack = buildTrack();
  return cachedTrack;
}

export function coasterSample(distance, target = {}) {
  const track = coasterTrack();
  const wrapped = ((distance % track.length) + track.length) % track.length;
  const position = wrapped / track.step;
  const index = Math.min(track.count - 2, Math.floor(position));
  const t = Math.min(1, position - index);
  const next = index + 1;
  const mix = (array) => array[index] + (array[next] - array[index]) * t;
  target.x = mix(track.x);
  target.y = mix(track.y);
  target.z = mix(track.z);
  target.tx = mix(track.tx);
  target.ty = mix(track.ty);
  target.tz = mix(track.tz);
  target.ux = mix(track.ux);
  target.uy = mix(track.uy);
  target.uz = mix(track.uz);
  target.speed = mix(track.speed);
  target.section = track.piece[t < 0.5 ? index : next];
  target.distance = wrapped;
  return target;
}

export function coasterStationDistance() {
  const track = coasterTrack();
  const station = track.sections.find((section) => section.station);
  return station.start + (station.end - station.start) * 0.62;
}

export function coasterSectionAt(distance) {
  const track = coasterTrack();
  const wrapped = ((distance % track.length) + track.length) % track.length;
  return track.sections[track.piece[Math.min(track.count - 1, Math.round(wrapped / track.step))]];
}

export function coasterEnvelope(f) {
  const { envelope } = coasterTrack();
  const position = wrapPlan(f);
  const index = Math.floor(position);
  const t = position - index;
  const next = index + 1 < envelope.length ? index + 1 : 0;
  return envelope[index] + (envelope[next] - envelope[index]) * t;
}

function softMax(a, b, softness) {
  return 0.5 * (a + b + Math.sqrt((a - b) ** 2 + softness * softness));
}

export function droneCourseRingSide(index) {
  const count = DRONE_COURSE_RING_SIDES.length;
  return DRONE_COURSE_RING_SIDES[((index % count) + count) % count];
}

export function coasterDronePoint(angle, target = {}) {
  const f = DRONE_COURSE_START + angle * DRONE_COURSE_UNITS_PER_RADIAN;
  const plan = coasterPlanFrame(f, {});
  const phase = (angle * DRONE_COURSE_RING_COUNT) / 2;
  const across = Math.cos(phase);
  const lateral = -DRONE_COURSE_WEAVE_WIDTH * Math.sin(phase);
  const base = softMax(coasterEnvelope(f), DRONE_COURSE_FLOOR, 6);
  const side = droneCourseRingSide(Math.round(phase / Math.PI));
  target.x = plan.x + plan.nx * lateral;
  target.y = base + DRONE_COURSE_WEAVE_HEIGHT * side * across * across;
  target.z = plan.z + plan.nz * lateral;
  return target;
}

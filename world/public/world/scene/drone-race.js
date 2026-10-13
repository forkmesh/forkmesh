import {
  DRONE_RACE_GHOST_MAX_SAMPLES,
  DRONE_RACE_GHOST_SAMPLE_MS,
  DRONE_RACE_MAX_LAP_MS,
} from "./gameplay-constants.js";

export function gateCrossing(gate, from, to) {
  const [cx, cy, cz] = gate.center;
  const [nx, ny, nz] = gate.normal;
  const before = (from.x - cx) * nx + (from.y - cy) * ny + (from.z - cz) * nz;
  const after = (to.x - cx) * nx + (to.y - cy) * ny + (to.z - cz) * nz;
  const forward = before < 0 && after >= 0;
  const backward = before >= 0 && after < 0;
  if (!forward && !backward) return 0;
  const t = before / (before - after);
  const px = from.x + (to.x - from.x) * t - cx;
  const py = from.y + (to.y - from.y) * t - cy;
  const pz = from.z + (to.z - from.z) * t - cz;
  if (px * px + py * py + pz * pz > gate.radius * gate.radius) return 0;
  return forward ? 1 : -1;
}

export function createRaceState(ringCount, bestMs = 0) {
  return {
    ringCount,
    running: false,
    nextRing: 0,
    lapStart: 0,
    lastLapMs: 0,
    bestMs: validLapMs(bestMs),
    laps: 0,
    newBest: false,
  };
}

export function validLapMs(value) {
  const number = Math.round(Number(value) || 0);
  return number > 0 && number <= DRONE_RACE_MAX_LAP_MS ? number : 0;
}

export function resetRace(race) {
  race.running = false;
  race.nextRing = 0;
  race.lapStart = 0;
  race.newBest = false;
}

function beginLap(race, now) {
  race.running = true;
  race.nextRing = 0;
  race.lapStart = now;
}

export function advanceRace(race, rings, start, from, to, now) {
  const startCrossing = gateCrossing(start, from, to);
  if (!race.running) {
    if (startCrossing > 0) {
      beginLap(race, now);
      return "start";
    }
    return startCrossing < 0 ? "wrong-way" : "";
  }
  if (startCrossing > 0) {
    if (race.nextRing >= race.ringCount) {
      const lap = Math.round(now - race.lapStart);
      race.lastLapMs = lap;
      race.laps += 1;
      race.newBest = !race.bestMs || lap < race.bestMs;
      if (race.newBest) race.bestMs = lap;
      beginLap(race, now);
      return "finish";
    }
    beginLap(race, now);
    return "restart";
  }
  if (startCrossing < 0) return "wrong-way";
  for (let index = 0; index < rings.length; index += 1) {
    const crossing = gateCrossing(rings[index], from, to);
    if (crossing === 0) continue;
    if (crossing < 0) return "wrong-way";
    if (index === race.nextRing) {
      race.nextRing += 1;
      return "ring";
    }
    return "out-of-order";
  }
  return "";
}

export function raceElapsedMs(race, now) {
  return race.running ? Math.max(0, now - race.lapStart) : 0;
}

export function racePresence(race, now) {
  return {
    ring: race.running ? race.nextRing : 0,
    lap: race.running ? Math.min(DRONE_RACE_MAX_LAP_MS, Math.round(now - race.lapStart)) : 0,
    best: race.bestMs,
  };
}

export function remoteRace(value, ringCount) {
  if (!value || typeof value !== "object") return null;
  const ring = Math.round(Number(value.ring) || 0);
  return {
    ring: Math.max(0, Math.min(ringCount, ring)),
    lap: validLapMs(value.lap),
    best: validLapMs(value.best),
  };
}

export function formatLapTime(ms) {
  const total = Math.max(0, Math.round(Number(ms) || 0));
  const minutes = Math.floor(total / 60_000);
  const seconds = Math.floor((total % 60_000) / 1000);
  const hundredths = Math.floor((total % 1000) / 10);
  return `${minutes}:${String(seconds).padStart(2, "0")}.${String(hundredths).padStart(2, "0")}`;
}

export function createGhostTrack(capacity = DRONE_RACE_GHOST_MAX_SAMPLES) {
  return {
    samples: new Float32Array(capacity * 4),
    capacity,
    count: 0,
    stepMs: DRONE_RACE_GHOST_SAMPLE_MS,
  };
}

export function clearGhostTrack(track) {
  track.count = 0;
}

export function recordGhostSample(track, elapsedMs, x, y, z, yaw) {
  while (track.count < track.capacity && track.count * track.stepMs <= elapsedMs) {
    const offset = track.count * 4;
    track.samples[offset] = x;
    track.samples[offset + 1] = y;
    track.samples[offset + 2] = z;
    track.samples[offset + 3] = yaw;
    track.count += 1;
  }
}

export function copyGhostTrack(target, source) {
  const count = Math.min(target.capacity, source.count);
  target.samples.set(source.samples.subarray(0, count * 4));
  target.count = count;
  target.stepMs = source.stepMs;
}

export function sampleGhostTrack(track, elapsedMs, out) {
  if (track.count < 2 || elapsedMs < 0) return false;
  const position = elapsedMs / track.stepMs;
  const index = Math.floor(position);
  if (index >= track.count - 1) return false;
  const t = position - index;
  const a = index * 4;
  const b = a + 4;
  const s = track.samples;
  out.x = s[a] + (s[b] - s[a]) * t;
  out.y = s[a + 1] + (s[b + 1] - s[a + 1]) * t;
  out.z = s[a + 2] + (s[b + 2] - s[a + 2]) * t;
  let turn = s[b + 3] - s[a + 3];
  if (turn > Math.PI) turn -= Math.PI * 2;
  if (turn < -Math.PI) turn += Math.PI * 2;
  out.yaw = s[a + 3] + turn * t;
  return true;
}

export function ghostTrackRecord(track) {
  const samples = [];
  for (let index = 0; index < track.count * 4; index += 1) {
    samples.push(Math.round(track.samples[index] * 10) / 10);
  }
  return { stepMs: track.stepMs, samples };
}

export function loadGhostTrack(track, record) {
  const samples = Array.isArray(record?.samples) ? record.samples : [];
  const stepMs = Number(record?.stepMs);
  if (stepMs !== DRONE_RACE_GHOST_SAMPLE_MS || samples.length % 4 !== 0) {
    track.count = 0;
    return false;
  }
  const count = Math.min(track.capacity, samples.length / 4);
  for (let index = 0; index < count * 4; index += 1) {
    const value = Number(samples[index]);
    if (!Number.isFinite(value)) {
      track.count = 0;
      return false;
    }
    track.samples[index] = value;
  }
  track.count = count;
  track.stepMs = stepMs;
  return count > 1;
}

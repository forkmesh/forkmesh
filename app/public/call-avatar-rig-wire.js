import { FACE_TEMPLATE_3D } from "./chat-call-face-template.js";

export const RIG_VERSION = 1;
export const RIG_SEND_HZ = 12;
export const RIG_POINTS = 68;
export const RIG_TEMPLATE = Object.freeze(FACE_TEMPLATE_3D.map(([x, y]) => Object.freeze([x, y])));
export const HAND_GESTURES = Object.freeze({
  thumbs_up: { emoji: "👍", label: "Thumbs up" },
  thumbs_down: { emoji: "👎", label: "Thumbs down" },
  victory: { emoji: "✌️", label: "Peace" },
  ok: { emoji: "👌", label: "OK" },
  rock: { emoji: "🤘", label: "Rock on" },
  point_up: { emoji: "☝️", label: "Point up" },
  open_palm: { emoji: "✋", label: "Raise hand" },
  wave: { emoji: "👋", label: "Wave" },
  fist: { emoji: "✊", label: "Fist" },
  heart: { emoji: "🫶", label: "Heart" },
  clap: { emoji: "👏", label: "Clap" },
});
export const RIG_REACTIONS = Object.freeze({
  ...HAND_GESTURES,
  laugh: { emoji: "😂", label: "Laugh" },
  fire: { emoji: "🔥", label: "Fire" },
});
export const RIG_CHANNELS = Object.freeze(["lidLeft", "lidRight", "gazeX", "gazeY", "mouth", "smile", "browLeft", "browRight", "round", "tongue"]);

const DELTA_STEPS = 64;
const ANCHOR_STEPS = 255;
const SIGNED_CHANNELS = new Set(["gazeX", "gazeY", "browLeft", "browRight"]);

const clampInt = (value, low, high) => Math.max(low, Math.min(high, Math.round(Number.isFinite(value) ? value : 0)));

function bytesToBase64(bytes) {
  let text = "";
  for (const byte of bytes) text += String.fromCharCode(byte);
  return globalThis.btoa(text);
}

function base64ToBytes(text) {
  const raw = globalThis.atob(String(text || ""));
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) out[i] = raw.charCodeAt(i);
  return out;
}

export function encodeRig(rig) {
  if (!rig) return null;
  const wire = { v: RIG_VERSION };
  wire.p = [rig.yaw, rig.pitch, rig.roll].map((angle) => clampInt(angle * 1000, -1600, 1600));
  if (rig.deltas?.length === RIG_POINTS * 2) {
    const bytes = new Uint8Array(RIG_POINTS * 2);
    for (let i = 0; i < bytes.length; i += 1) bytes[i] = clampInt(rig.deltas[i] * DELTA_STEPS, -127, 127) & 255;
    wire.d = bytesToBase64(bytes);
  }
  wire.x = RIG_CHANNELS.map((name) => clampInt((rig[name] || 0) * 100, SIGNED_CHANNELS.has(name) ? -100 : 0, 100));
  wire.h = (rig.hands || []).slice(0, 2).map((hand) => [
    hand.side === "right" ? 1 : 0,
    clampInt(hand.x * 20, -255, 255),
    clampInt(hand.y * 20, -255, 255),
    ...[0, 1, 2, 3, 4].map((i) => clampInt((hand.curl?.[i] || 0) * 100, 0, 100)),
    clampInt((hand.roll || 0) * 40, -255, 255),
    clampInt((hand.z || 0) * 100, -255, 255),
  ]);
  wire.t = rig.talking ? 1 : 0;
  wire.l = clampInt((rig.level || 0) * 100, 0, 100);
  wire.w = clampInt(rig.words || 0, 0, 9999999);
  if (rig.anchors?.length === RIG_POINTS) wire.a = encodeAnchors(rig.anchors);
  return wire;
}

export function decodeRig(wire) {
  if (!wire || typeof wire !== "object" || Number(wire.v) !== RIG_VERSION) return null;
  const p = Array.isArray(wire.p) ? wire.p : [];
  const rig = {
    yaw: clampInt(p[0], -1600, 1600) / 1000,
    pitch: clampInt(p[1], -1600, 1600) / 1000,
    roll: clampInt(p[2], -1600, 1600) / 1000,
    deltas: null,
    hands: [],
    talking: Number(wire.t) === 1 || wire.t === true,
    level: clampInt(wire.l, 0, 100) / 100,
    words: clampInt(wire.w, 0, 9999999),
    anchors: null,
  };
  const x = Array.isArray(wire.x) ? wire.x : [];
  RIG_CHANNELS.forEach((name, i) => {
    rig[name] = clampInt(x[i], SIGNED_CHANNELS.has(name) ? -100 : 0, 100) / 100;
  });
  if (typeof wire.d === "string") {
    try {
      const bytes = base64ToBytes(wire.d);
      if (bytes.length === RIG_POINTS * 2) {
        rig.deltas = new Float32Array(bytes.length);
        for (let i = 0; i < bytes.length; i += 1) rig.deltas[i] = ((bytes[i] << 24) >> 24) / DELTA_STEPS;
      }
    } catch (_) {
      rig.deltas = null;
    }
  }
  if (Array.isArray(wire.h)) {
    for (const hand of wire.h.slice(0, 2)) {
      if (!Array.isArray(hand) || hand.length < 9) continue;
      rig.hands.push({
        side: Number(hand[0]) === 1 ? "right" : "left",
        x: clampInt(hand[1], -255, 255) / 20,
        y: clampInt(hand[2], -255, 255) / 20,
        curl: hand.slice(3, 8).map((value) => clampInt(value, 0, 100) / 100),
        roll: clampInt(hand[8], -255, 255) / 40,
        z: hand.length > 9 ? clampInt(hand[9], -255, 255) / 100 : 0,
      });
    }
  }
  if (typeof wire.a === "string") rig.anchors = decodeAnchors(wire.a);
  return rig;
}

export function encodeAnchors(anchors) {
  const bytes = new Uint8Array(RIG_POINTS * 2);
  anchors.forEach(([x, y], i) => {
    bytes[i * 2] = clampInt(x * ANCHOR_STEPS, 0, ANCHOR_STEPS);
    bytes[i * 2 + 1] = clampInt(y * ANCHOR_STEPS, 0, ANCHOR_STEPS);
  });
  return bytesToBase64(bytes);
}

export function decodeAnchors(text) {
  try {
    const bytes = base64ToBytes(text);
    if (bytes.length !== RIG_POINTS * 2) return null;
    return Array.from({ length: RIG_POINTS }, (_, i) => [bytes[i * 2] / ANCHOR_STEPS, bytes[i * 2 + 1] / ANCHOR_STEPS]);
  } catch (_) {
    return null;
  }
}

export function rigTemplatePoints(rig) {
  return RIG_TEMPLATE.map(([x, y], i) => [x + (rig?.deltas ? rig.deltas[i * 2] : 0), y + (rig?.deltas ? rig.deltas[i * 2 + 1] : 0)]);
}

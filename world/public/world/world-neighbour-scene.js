// A neighbouring World may publish what stands on its hexagon so the hub can
// draw it there instead of only a logo. It is third-party data: it is read as
// JSON, every field is checked against a small allow-list, text is plain
// (never HTML), links and icons must be https, and anything that does not fit
// is dropped. A scene that fails as a whole leaves the logo hexagon as it was.
export const NEIGHBOUR_SCENE_PATH = "/api/world/neighbour-scene";
export const NEIGHBOUR_SCENE_VERSION = 1;
export const NEIGHBOUR_SCENE_MAX_BYTES = 64 * 1024;
export const NEIGHBOUR_SCENE_MAX_OBJECTS = 48;
export const NEIGHBOUR_SCENE_MAX_BUGS = 32;
export const NEIGHBOUR_SCENE_REACH = 330;
export const NEIGHBOUR_SCENE_TYPES = Object.freeze([
  "house",
  "lamp",
  "plaza",
  "sign",
  "terrarium",
  "tree",
]);
const COLOR_RE = /^#[0-9a-f]{6}$/;
const HOST_RE = /^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z][a-z0-9-]{0,61}[a-z0-9]$/;

function text(value, limit) {
  return String(value ?? "")
    .replace(/[\u0000-\u001f\u007f<>"'`]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, limit);
}

function color(value, fallback) {
  const hex = String(value || "").trim().toLowerCase();
  return COLOR_RE.test(hex) ? hex : fallback;
}

function number(value, low, high, fallback = 0) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? Math.max(low, Math.min(high, parsed)) : fallback;
}

export function neighbourSceneLink(value) {
  try {
    const url = new URL(String(value || ""));
    if (url.protocol !== "https:" || url.username || url.password) return "";
    if (!HOST_RE.test(url.hostname) || url.href.length > 300) return "";
    return url.href;
  } catch (_) {
    return "";
  }
}

export function neighbourSceneURL(world) {
  const origin = neighbourSceneLink(world);
  return origin ? new URL(NEIGHBOUR_SCENE_PATH, origin).href : "";
}

function placed(item) {
  const x = Number(item?.x);
  const z = Number(item?.z);
  if (!Number.isFinite(x) || !Number.isFinite(z)) return null;
  if (Math.hypot(x, z) > NEIGHBOUR_SCENE_REACH) return null;
  return {
    type: String(item.type),
    x: Math.round(x * 10) / 10,
    z: Math.round(z * 10) / 10,
    facing: number(item.facing, -Math.PI * 2, Math.PI * 2),
  };
}

function bug(item) {
  const label = text(item?.label, 40);
  if (!label) return null;
  return {
    label,
    icon: neighbourSceneLink(item?.icon),
    url: neighbourSceneLink(item?.url),
  };
}

function sceneObject(item) {
  if (!item || typeof item !== "object") return null;
  if (!NEIGHBOUR_SCENE_TYPES.includes(item.type)) return null;
  const base = placed(item);
  if (!base) return null;
  if (base.type === "house") {
    const label = text(item.label, 40);
    if (!label) return null;
    return { ...base, label, url: neighbourSceneLink(item.url), color: color(item.color, "#e74c3c") };
  }
  if (base.type === "terrarium") {
    const bugs = (Array.isArray(item.bugs) ? item.bugs : [])
      .slice(0, NEIGHBOUR_SCENE_MAX_BUGS * 2)
      .map(bug)
      .filter(Boolean)
      .slice(0, NEIGHBOUR_SCENE_MAX_BUGS);
    return { ...base, label: text(item.label, 40), bugs };
  }
  if (base.type === "sign") {
    const title = text(item.title, 40);
    if (!title) return null;
    return { ...base, title, text: text(item.text, 60) };
  }
  if (base.type === "plaza") {
    return { ...base, radius: number(item.radius, 4, 60, 26), color: color(item.color, "#d8d0bf") };
  }
  if (base.type === "lamp") return { ...base, color: color(item.color, "#fff3c4") };
  return { ...base, scale: number(item.scale, 0.5, 2, 1) };
}

export function normalizeNeighbourScene(payload) {
  if (!payload || typeof payload !== "object") return null;
  if (Number(payload.v) !== NEIGHBOUR_SCENE_VERSION) return null;
  const ground = payload.ground && typeof payload.ground === "object" ? payload.ground : {};
  const objects = (Array.isArray(payload.objects) ? payload.objects : [])
    .slice(0, NEIGHBOUR_SCENE_MAX_OBJECTS)
    .map(sceneObject)
    .filter(Boolean);
  return {
    v: NEIGHBOUR_SCENE_VERSION,
    ground: {
      color: color(ground.color, "#9fc584"),
      accent: color(ground.accent, "#9ef7c6"),
      pattern: ground.pattern === "circuit" ? "circuit" : "plain",
    },
    objects,
  };
}

// Fetched without credentials and read as text first, so an oversized or
// non-JSON answer is refused before it is parsed.
export async function loadNeighbourScene(url, { timeoutMs = 6000, fetchImpl = globalThis.fetch } = {}) {
  const target = neighbourSceneLink(url);
  if (!target || !target.endsWith(NEIGHBOUR_SCENE_PATH)) return null;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetchImpl(target, {
      mode: "cors",
      credentials: "omit",
      redirect: "error",
      signal: controller.signal,
      headers: { accept: "application/json" },
    });
    if (!response.ok) return null;
    const body = await response.text();
    if (body.length > NEIGHBOUR_SCENE_MAX_BYTES) return null;
    return normalizeNeighbourScene(JSON.parse(body));
  } catch (_) {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

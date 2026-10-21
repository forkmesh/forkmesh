export const FORKMESH_WORLD_HOME = "https://world.forkmesh.com/world/";
export const TERRITORY_FROM_PARAM = "from";
export const TERRITORY_SLOT_PARAM = "slot";
export const TERRITORY_POS_PARAM = "pos";
export const TERRITORY_VISA_PARAM = "visa";
export const FORKMESH_ORIGIN_NAME = "forkmesh";
export const DEFAULT_TERRITORY_SLOT = 0;
export const TERRITORY_SLOTS = 12;
export const TERRITORY_POS_LIMIT = 400;
export const TERRITORY_OBJECT_LIMIT = 12;
export const TERRITORY_BUG_LIMIT = 60;
export const TERRITORY_OBJECT_REACH = 300;
export const TERRITORY_PLAZA = Object.freeze({ x: 0, z: 40 });
export const TERRITORY_FRESH_SPAWN = Object.freeze({
  x: 0,
  y: 0.38,
  z: 70,
  heading: 0,
  space: "town-square",
});

export const TERRITORY_KEPT_ELEMENTS = Object.freeze([
  "lighting",
  "sky",
  "shadows",
  "screen-labels",
  "animations",
  "effects",
  "chat-bubbles",
  "weather",
  "player-avatar",
  "avatar-crowd-batches",
  "remote-avatars",
  "campfire",
  "member-circle-figures",
  "member-figures",
  "repository-portals",
  "landmark-repositories",
  "instance-territory",
]);

export const TERRITORY_NEIGHBOUR_ELEMENTS = Object.freeze([
  "beach",
  "benches",
  "bike-lane",
  "causeways",
  "city-terrain",
  "coaster",
  "desert-plots",
  "drone-race-course",
  "gym",
  "landmark-fountain",
  "landmark-office",
  "office-garden",
  "office-landscaping",
  "swing-set",
  "town-landscape",
  "zip-line",
]);

export const TERRITORY_HIDDEN_ELEMENTS = Object.freeze([
  "agent-npcs",
  "arrival-box",
  "beach-car",
  "beach-portals",
  "bikes",
  "directory-bots",
  "discord-board",
  "forkbot",
  "leaderboard-district",
  "mainland-distance-lod",
  "mastodon-kiosk",
  "node-cabinets",
  "node-district",
  "qa-board",
  "quadcopter",
  "social-banners",
  "start-here-board",
  "world-bulletin",
  "world-chat-board",
]);

export const TERRITORY_STYLES = Object.freeze({
  meadow: Object.freeze({
    id: "meadow",
    ground: "#8fbf72",
    plaza: "#d8d0bf",
    road: "#2b3433",
    trim: "#f4f1e8",
    accent: "#3b82f6",
    sign: "#111113",
    signText: "#f4f4f5",
    foliage: "#4f8f3a",
    trunk: "#6b4a2f",
    lamp: "#fff3c4",
  }),
  security: Object.freeze({
    id: "security",
    ground: "#3a4550",
    plaza: "#46525d",
    road: "#1b2126",
    trim: "#5a6670",
    accent: "#e74c3c",
    sign: "#0b0f12",
    signText: "#3ee6a0",
    foliage: "#2f5e46",
    trunk: "#4a3a30",
    lamp: "#9ff5e1",
  }),
});

const HEX_COLOR_RE = /^#[0-9a-f]{6}$/;
const DOMAIN_RE =
  /^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z][a-z0-9-]{0,61}[a-z0-9]$/;
const VISA_RE = /^[A-Za-z0-9_-]{1,1950}\.[0-9a-f]{64}$/;
const POS_RE = /^-?\d{1,3}(?:\.\d)?,-?\d{1,3}(?:\.\d)?$/;
const LOGO_RE = /^\/(?:assets\/logo\.svg|instance-logo\.(?:png|jpg|jpeg|webp|svg))$/;

function text(value, limit) {
  return String(value ?? "")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, limit);
}

export function territoryColor(value, fallback = "") {
  const color = String(value || "").trim().toLowerCase();
  return HEX_COLOR_RE.test(color) ? color : fallback;
}

export function territoryDomain(value) {
  const domain = String(value || "").trim().toLowerCase().replace(/\.$/, "");
  return DOMAIN_RE.test(domain) ? domain : "";
}

export function territorySlot(value) {
  const slot = Number(value);
  return Number.isInteger(slot) && slot >= 0 && slot < TERRITORY_SLOTS
    ? slot
    : -1;
}

export function territoryLinkURL(value) {
  try {
    const url = new URL(String(value || ""));
    if (url.protocol !== "https:" || url.username || url.password) return "";
    if (!territoryDomain(url.hostname)) return "";
    return url.href.length <= 300 ? url.href : "";
  } catch (_) {
    return "";
  }
}

export function territoryWorldHome(value) {
  try {
    const url = new URL(String(value || ""));
    if (
      url.protocol !== "https:" ||
      url.pathname !== "/world/" ||
      url.search ||
      url.hash ||
      url.username ||
      url.password ||
      !territoryDomain(url.hostname)
    ) {
      return "";
    }
    return url.href;
  } catch (_) {
    return "";
  }
}

function coordinate(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return 0;
  return Math.max(-TERRITORY_OBJECT_REACH, Math.min(TERRITORY_OBJECT_REACH, number));
}

export function territoryObjects(list) {
  return (Array.isArray(list) ? list : [])
    .map((item) => {
      if (!item || typeof item !== "object") return null;
      const type = String(item.type || "house");
      const url = territoryLinkURL(item.url);
      const label = text(item.label, 40);
      if (type !== "house" || !url || !label) return null;
      const position = Array.isArray(item.position) ? item.position : [];
      return {
        type,
        label,
        url,
        host: new URL(url).hostname,
        x: coordinate(position[0]),
        z: coordinate(position[1]),
        color: territoryColor(item.color),
      };
    })
    .filter(Boolean)
    .slice(0, TERRITORY_OBJECT_LIMIT);
}

const REPO_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,99}\/[A-Za-z0-9][A-Za-z0-9._-]{0,99}$/;
const REF_RE = /^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$/;
const FILE_RE = /^[A-Za-z0-9._-][A-Za-z0-9._/-]{0,199}$/;
const FAVICON_RE = /\.(?:png|ico|gif|jpe?g|webp)$/i;

export function territoryRepoPath(value) {
  const parts = [];
  for (const part of String(value || "").split("/")) {
    if (!part || part === ".") continue;
    if (part === "..") {
      if (!parts.length) return "";
      parts.pop();
      continue;
    }
    parts.push(part);
  }
  const path = parts.join("/");
  return FILE_RE.test(path) ? path : "";
}

export function territoryBugSource(bugs) {
  const repo = String(bugs?.repo || "").trim();
  const ref = String(bugs?.ref || "main").trim();
  const path = territoryRepoPath(bugs?.path);
  if (!REPO_RE.test(repo) || !REF_RE.test(ref) || ref.includes("..") || !path) return null;
  return { repo, ref, path };
}

export function territoryRawURL(source, path) {
  const file = territoryRepoPath(path);
  if (!source || !file) return "";
  const [owner, name] = source.repo.split("/");
  const query = new URLSearchParams({ path: file, ref: source.ref });
  return `/api/repo/${encodeURIComponent(owner)}/${encodeURIComponent(name)}/raw?${query}`;
}

export function territoryFaviconURL(source, favicon) {
  const value = String(favicon || "");
  if (!source || !value || value.includes(":") || value.startsWith("/")) return "";
  const folder = source.path.split("/").slice(0, -1).join("/");
  const path = territoryRepoPath(folder ? `${folder}/${value}` : value);
  return path && FAVICON_RE.test(path) ? territoryRawURL(source, path) : "";
}

export function territoryBugs(list, limit = TERRITORY_BUG_LIMIT, source = null) {
  const cap = Math.max(0, Math.min(TERRITORY_BUG_LIMIT, Number(limit) || 0));
  const items = Array.isArray(list) ? list : Array.isArray(list?.bugs) ? list.bugs : [];
  const seen = new Set();
  const bugs = [];
  items.forEach((item) => {
    if (bugs.length >= cap) return;
    const domain = territoryDomain(typeof item === "string" ? item : item?.domain);
    if (!domain || seen.has(domain)) return;
    seen.add(domain);
    const count = Number(item?.count);
    bugs.push({
      domain,
      name: text(item?.name, 60) || domain,
      count: Number.isInteger(count) && count > 0 ? count : 0,
      url: territoryLinkURL(item?.url),
      favicon: territoryFaviconURL(source, item?.favicon),
    });
  });
  return bugs;
}

export function territoryPetal(slot, mesh = null) {
  const worlds = Array.isArray(mesh?.worlds) ? mesh.worlds : [];
  const self = worlds.find((world) => world?.id && world.id === mesh?.self);
  const published = Number(self?.petal);
  if (self?.shape === "hex" && Number.isInteger(published) && published >= 1 && published <= 5) {
    return published;
  }
  return Number.isInteger(slot) && slot >= 0 && slot < 5 ? slot + 1 : -1;
}

export function territoryFromInstance(payload, mesh = null) {
  if (payload?.branded !== true) return null;
  const world = payload.world && typeof payload.world === "object" ? payload.world : {};
  const style = TERRITORY_STYLES[world.style] || TERRITORY_STYLES.meadow;
  const themePrimary = territoryColor(payload?.theme?.dark?.primary);
  const accent = territoryColor(world.accent) || themePrimary || style.accent;
  const portal = payload?.directory?.portal;
  const slot = territorySlot(portal?.slot);
  const listed = slot >= 0;
  const home =
    territoryWorldHome(portal?.home) ||
    territoryWorldHome(world.home) ||
    FORKMESH_WORLD_HOME;
  const bugs = world.bugs && typeof world.bugs === "object" ? world.bugs : null;
  return {
    name: text(payload.name, 40) || "This instance",
    logo: LOGO_RE.test(String(world.logo || "")) ? world.logo : "/assets/logo.svg",
    style: style.id,
    palette: { ...style, accent },
    home,
    slot: listed ? slot : territorySlot(world.slot) >= 0
      ? territorySlot(world.slot)
      : DEFAULT_TERRITORY_SLOT,
    listed,
    petal: territoryPetal(listed ? slot : territorySlot(world.slot) >= 0
      ? territorySlot(world.slot)
      : DEFAULT_TERRITORY_SLOT, mesh),
    objects: territoryObjects(world.objects),
    bugs: bugs && territoryBugSource(bugs)
      ? {
        source: territoryBugSource(bugs),
        title: text(bugs.title, 40) || "Bug terrarium",
        limit: Math.max(1, Math.min(TERRITORY_BUG_LIMIT, Number(bugs.limit) || TERRITORY_BUG_LIMIT)),
        x: coordinate(Array.isArray(bugs.position) ? bugs.position[0] : 70),
        z: coordinate(Array.isArray(bugs.position) ? bugs.position[1] : 40),
      }
      : null,
  };
}

export function territoryDisabledElements(saved = [], territory = null) {
  const list = Array.isArray(saved) ? saved : [];
  if (!territory) return list;
  return [...new Set([...list, ...TERRITORY_HIDDEN_ELEMENTS])];
}

function roundedPos(value) {
  const number = Math.round(Number(value) * 10) / 10;
  if (!Number.isFinite(number)) return null;
  return Math.max(-TERRITORY_POS_LIMIT, Math.min(TERRITORY_POS_LIMIT, number));
}

export function formatTerritoryPos(local) {
  const x = roundedPos(local?.x);
  const z = roundedPos(local?.z);
  if (x === null || z === null) return "";
  return `${x.toFixed(1).replace(/\.0$/, "")},${z.toFixed(1).replace(/\.0$/, "")}`;
}

export function parseTerritoryPos(value) {
  const raw = String(value || "");
  if (!POS_RE.test(raw)) return null;
  const [x, z] = raw.split(",").map(Number);
  if (Math.abs(x) > TERRITORY_POS_LIMIT || Math.abs(z) > TERRITORY_POS_LIMIT) return null;
  return { x, z };
}

export function territoryCrossingURL(base, { from, slot = -1, local } = {}) {
  const home = territoryWorldHome(base);
  const origin = from === FORKMESH_ORIGIN_NAME ? from : territoryDomain(from);
  const pos = formatTerritoryPos(local);
  if (!home || !origin || !pos) return "";
  const url = new URL(home);
  url.searchParams.set(TERRITORY_FROM_PARAM, origin);
  const arrival = territorySlot(slot);
  if (arrival >= 0) url.searchParams.set(TERRITORY_SLOT_PARAM, String(arrival));
  url.searchParams.set(TERRITORY_POS_PARAM, pos);
  return url.href;
}

export function withTerritoryVisa(href, visa) {
  const token = String(visa || "");
  if (!href || !VISA_RE.test(token)) return href;
  try {
    const url = new URL(href);
    url.hash = new URLSearchParams({ [TERRITORY_VISA_PARAM]: token }).toString();
    return url.href;
  } catch (_) {
    return href;
  }
}

export function territoryArrivalVisa(hash = "") {
  const params = new URLSearchParams(String(hash || "").replace(/^#/, ""));
  const token = String(params.get(TERRITORY_VISA_PARAM) || "");
  return VISA_RE.test(token) ? token : "";
}

export function territoryArrival(search = "") {
  const params = new URLSearchParams(String(search || ""));
  const rawFrom = String(params.get(TERRITORY_FROM_PARAM) || "").toLowerCase();
  const from = rawFrom === FORKMESH_ORIGIN_NAME ? rawFrom : territoryDomain(rawFrom);
  const pos = parseTerritoryPos(params.get(TERRITORY_POS_PARAM));
  if (!from || !pos) return null;
  const slotText = params.get(TERRITORY_SLOT_PARAM);
  const slot = slotText !== null && /^\d{1,2}$/.test(slotText) ? territorySlot(slotText) : -1;
  return { from, slot, pos };
}

export function withoutTerritoryArrival(href) {
  try {
    const url = new URL(String(href || ""));
    [TERRITORY_FROM_PARAM, TERRITORY_SLOT_PARAM, TERRITORY_POS_PARAM].forEach((key) =>
      url.searchParams.delete(key),
    );
    if (territoryArrivalVisa(url.hash) || /(?:^#|&)visa=/.test(url.hash)) url.hash = "";
    return url.href;
  } catch (_) {
    return "";
  }
}

export function territoryVisitorLabel(arrival) {
  if (!arrival) return "";
  return arrival.from === FORKMESH_ORIGIN_NAME ? "the ForkMesh World" : arrival.from;
}

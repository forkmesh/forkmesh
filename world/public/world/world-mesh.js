export const WORLD_MESH_SPACE = "world-mesh";
export const WORLD_MESH_VISITING_SPACE = "visiting";
export const WORLD_MESH_HUB_ID = "forkmesh";
export const WORLD_MESH_SLOT_COUNT = 12;
export const WORLD_MESH_OUTDOOR_SPACES = new Set(["town-square", "east", "central", "west"]);

const WORLD_ID_RE = /^(?:forkmesh|[0-9a-f]{32})$/;
const LOGO_RE = /^https:\/\/[a-z0-9.-]+(?::\d{1,5})?\/api\/start\/instances\/[0-9a-f]{32}\/logo\?v=\d+$/;
const PEER_ID_RE = /^[A-Za-z0-9_-]{1,32}$/;
const WATCH_REACH = 1700;
const WATCH_RELEASE = 1900;
const MAX_LINKS = 4;
const MAX_PEERS_PER_WORLD = 32;
const PING_MS = 40_000;
const RETRY_BASE_MS = 1_000;
const RETRY_MAX_MS = 30_000;
const COORD_LIMIT = 16_000;

function httpsOrigin(value) {
  try {
    const url = new URL(String(value || ""));
    if (url.protocol !== "https:" || url.username || url.password) return "";
    return url.origin;
  } catch (_) {
    return "";
  }
}

function cleanText(value, fallback, limit) {
  const text = String(value ?? "")
    .replace(/[\u0000-\u001f\u007f<>"'`]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, limit);
  return text || fallback;
}

function coordinate(value) {
  const number = Number(value);
  return Number.isFinite(number) ? Math.max(-COORD_LIMIT, Math.min(COORD_LIMIT, number)) : 0;
}

export function worldMeshHost(value) {
  const host = String(value || "").toLowerCase();
  return /^[a-z0-9.-]{1,253}(?::\d{1,5})?$/.test(host) ? host : "";
}

export function normalizeWorldMesh(payload) {
  const worlds = [];
  const ids = new Set();
  const slots = new Set();
  const hosts = new Set();
  for (const item of Array.isArray(payload?.worlds) ? payload.worlds : []) {
    const id = String(item?.id || "");
    const slot = Number(item?.slot);
    const world = httpsOrigin(item?.world);
    const api = httpsOrigin(item?.api);
    if (!WORLD_ID_RE.test(id) || ids.has(id) || !world || !api) continue;
    if (!Number.isInteger(slot) || slot < -1 || slot >= WORLD_MESH_SLOT_COUNT) continue;
    if ((id === WORLD_MESH_HUB_ID) !== (slot === -1) || slots.has(slot)) continue;
    const host = new URL(world).host;
    if (hosts.has(host)) continue;
    ids.add(id);
    slots.add(slot);
    hosts.add(host);
    worlds.push({
      id,
      name: cleanText(item?.name, host, 40),
      host,
      world,
      api,
      logo: LOGO_RE.test(String(item?.logo || "")) ? String(item.logo) : "",
      slot,
      // A single-hexagon neighbour is drawn beside the town and entered by
      // walking onto it, which loads its own World page.
      shape: item?.shape === "hex" && slot >= 0 && slot < 5 ? "hex" : "flower",
    });
    if (worlds.length > WORLD_MESH_SLOT_COUNT) break;
  }
  const self = worlds.find((world) => world.id === String(payload?.self || "")) || null;
  return { self, worlds: self ? worlds : [] };
}

export function worldMeshSocketURL(api, { watch = false, visa = "" } = {}) {
  const origin = httpsOrigin(api);
  if (!origin) return "";
  const url = new URL("/api/world/ws", origin);
  url.protocol = "wss:";
  if (watch) url.searchParams.set("watch", "1");
  else if (visa) url.searchParams.set("visa", visa);
  return url.href;
}

export class WorldMeshLinks {
  constructor({
    onPeersChanged = () => {},
    onPeerMoved = () => {},
    onVisitChanged = () => {},
    requestVisa = async () => "",
    presenceFrame = () => null,
    moveIntervalMs = 1000,
    createSocket = (url) => new WebSocket(url),
  } = {}) {
    this.onPeersChanged = onPeersChanged;
    this.onPeerMoved = onPeerMoved;
    this.onVisitChanged = onVisitChanged;
    this.requestVisa = requestVisa;
    this.presenceFrame = presenceFrame;
    this.moveIntervalMs = moveIntervalMs;
    this.createSocket = createSocket;
    this.neighbors = new Map();
    this.links = new Map();
    this.visitId = "";
    this.pendingMove = null;
    this.moveTimer = 0;
    this.lastMoveAt = 0;
    this.destroyed = false;
  }

  setNeighbors(placed = []) {
    this.neighbors = new Map(
      (Array.isArray(placed) ? placed : [])
        .filter((world) => world?.id && httpsOrigin(world.api))
        .map((world) => [world.id, world]),
    );
    for (const id of [...this.links.keys()]) {
      if (!this.neighbors.has(id)) this.closeLink(id);
    }
    if (this.visitId && !this.neighbors.has(this.visitId)) this.setVisit("");
  }

  update(x, z, ownerId = "") {
    if (this.destroyed) return;
    const located = Number.isFinite(x) && Number.isFinite(z);
    const visit = located && this.neighbors.has(ownerId) ? ownerId : "";
    const wanted = new Map();
    if (visit) wanted.set(visit, "visit");
    if (located) {
      [...this.neighbors.values()]
        .map((world) => ({ world, distance: Math.hypot(x - world.x, z - world.z) }))
        .sort((left, right) => left.distance - right.distance)
        .forEach(({ world, distance }) => {
          if (wanted.size >= MAX_LINKS || wanted.has(world.id)) return;
          const reach = this.links.has(world.id) ? WATCH_RELEASE : WATCH_REACH;
          if (distance <= reach) wanted.set(world.id, "watch");
        });
    }
    for (const id of [...this.links.keys()]) {
      if (!wanted.has(id)) this.closeLink(id);
    }
    wanted.forEach((mode, id) => this.ensureLink(id, mode));
    this.setVisit(visit);
  }

  setVisit(id) {
    if (this.visitId === id) return;
    const previous = this.neighbors.get(this.visitId) || null;
    this.visitId = id;
    this.pendingMove = null;
    window.clearTimeout(this.moveTimer);
    this.moveTimer = 0;
    this.onVisitChanged(this.neighbors.get(id) || null, previous);
  }

  get visiting() {
    return this.neighbors.get(this.visitId) || null;
  }

  ensureLink(id, mode) {
    let link = this.links.get(id);
    if (link?.mode === mode) return;
    if (link) {
      this.retireSocket(link, "switching");
      link.mode = mode;
      link.retry = RETRY_BASE_MS;
    } else {
      link = {
        id,
        mode,
        socket: null,
        selfId: "",
        peers: new Map(),
        retry: RETRY_BASE_MS,
        retryTimer: 0,
        pingTimer: 0,
        generation: 0,
      };
      this.links.set(id, link);
    }
    void this.openLink(link);
  }

  async openLink(link) {
    const generation = (link.generation += 1);
    window.clearTimeout(link.retryTimer);
    link.retryTimer = 0;
    const world = this.neighbors.get(link.id);
    if (!world || this.destroyed) return;
    let visa = "";
    if (link.mode === "visit") {
      try {
        visa = String((await this.requestVisa(world)) || "");
      } catch (_) {
        visa = "";
      }
    }
    if (this.destroyed || link.generation !== generation || this.links.get(link.id) !== link) return;
    const url = worldMeshSocketURL(world.api, { watch: link.mode === "watch", visa });
    if (!url) return;
    let socket;
    try {
      socket = this.createSocket(url);
    } catch (_) {
      this.scheduleRetry(link);
      return;
    }
    link.socket = socket;
    socket.addEventListener("open", () => {
      if (link.socket !== socket) return;
      link.retry = RETRY_BASE_MS;
      window.clearInterval(link.pingTimer);
      link.pingTimer = window.setInterval(() => this.sendFrame(link, { type: "ping" }), PING_MS);
      if (link.mode === "visit") {
        this.sendPresence();
        if (this.pendingMove) this.flushMove();
      }
    });
    socket.addEventListener("message", (event) => {
      if (link.socket !== socket) return;
      let message = null;
      try {
        message = JSON.parse(event.data);
      } catch (_) {
        return;
      }
      this.receive(link, message);
    });
    socket.addEventListener("close", () => {
      if (link.socket !== socket) return;
      link.socket = null;
      link.selfId = "";
      window.clearInterval(link.pingTimer);
      link.pingTimer = 0;
      if (this.links.get(link.id) === link && !this.destroyed) this.scheduleRetry(link);
    });
  }

  scheduleRetry(link) {
    window.clearTimeout(link.retryTimer);
    const delay = link.retry * (0.75 + Math.random() * 0.5);
    link.retry = Math.min(RETRY_MAX_MS, link.retry * 2);
    link.retryTimer = window.setTimeout(() => {
      if (this.links.get(link.id) === link && !link.socket) void this.openLink(link);
    }, delay);
  }

  receive(link, message) {
    if (!message || typeof message !== "object") return;
    if (message.type === "welcome" && Array.isArray(message.peers)) {
      link.selfId = PEER_ID_RE.test(String(message.id || "")) ? String(message.id) : "";
      link.peers = new Map();
      message.peers.slice(0, MAX_PEERS_PER_WORLD).forEach((peer) => this.storePeer(link, peer));
      this.onPeersChanged();
      return;
    }
    if ((message.type === "join" || message.type === "presence") && message.peer) {
      if (this.storePeer(link, message.peer)) this.onPeersChanged();
      return;
    }
    if (message.type === "move") {
      const id = String(message.id || "");
      const peer = link.peers.get(id);
      if (!peer) return;
      for (const field of ["x", "y", "z"]) {
        if (field in message) peer[field] = coordinate(message[field]);
      }
      for (const field of ["yaw", "lookYaw", "lookPitch", "moving", "updatedAt"]) {
        if (field in message) peer[field] = message[field];
      }
      this.onPeerMoved(this.entry(link, peer));
      return;
    }
    if (message.type === "leave") {
      if (link.peers.delete(String(message.id || ""))) this.onPeersChanged();
    }
  }

  storePeer(link, peer) {
    const id = String(peer?.id || "");
    if (!PEER_ID_RE.test(id) || id === link.selfId) return false;
    if (!link.peers.has(id) && link.peers.size >= MAX_PEERS_PER_WORLD) return false;
    link.peers.set(id, {
      ...peer,
      id,
      x: coordinate(peer.x),
      y: coordinate(peer.y),
      z: coordinate(peer.z),
    });
    return true;
  }

  entry(link, peer) {
    const world = this.neighbors.get(link.id);
    return world
      ? { worldId: link.id, host: world.host, name: world.name, dx: world.dx, dz: world.dz, peer }
      : null;
  }

  peers() {
    const entries = [];
    this.links.forEach((link) => {
      link.peers.forEach((peer) => {
        const entry = this.entry(link, peer);
        if (entry) entries.push(entry);
      });
    });
    return entries;
  }

  sendFrame(link, frame) {
    const socket = link?.socket;
    if (!socket || socket.readyState !== 1) return false;
    try {
      socket.send(JSON.stringify(frame));
      return true;
    } catch (_) {
      return false;
    }
  }

  sendPresence() {
    const link = this.links.get(this.visitId);
    if (link?.mode !== "visit") return false;
    const frame = this.presenceFrame();
    return frame ? this.sendFrame(link, { ...frame, type: "presence", space: "town-square" }) : false;
  }

  queueMove(movement) {
    const world = this.visiting;
    if (!world || !movement) return;
    this.pendingMove = {
      type: "move",
      x: coordinate(Number(movement.x) - world.dx),
      y: coordinate(movement.y),
      z: coordinate(Number(movement.z) - world.dz),
      yaw: Number(movement.yaw ?? movement.heading) || 0,
      lookYaw: Number(movement.lookYaw ?? movement.yaw ?? movement.heading) || 0,
      lookPitch: Number(movement.lookPitch) || 0,
      moving: movement.moving !== false,
    };
    const elapsed = performance.now() - this.lastMoveAt;
    if (!this.pendingMove.moving || elapsed >= this.moveIntervalMs) {
      this.flushMove();
      return;
    }
    if (!this.moveTimer) {
      this.moveTimer = window.setTimeout(() => this.flushMove(), this.moveIntervalMs - elapsed);
    }
  }

  flushMove() {
    window.clearTimeout(this.moveTimer);
    this.moveTimer = 0;
    const link = this.links.get(this.visitId);
    if (!this.pendingMove || link?.mode !== "visit") return;
    if (this.sendFrame(link, this.pendingMove)) {
      this.pendingMove = null;
      this.lastMoveAt = performance.now();
    }
  }

  retireSocket(link, reason) {
    const socket = link.socket;
    link.socket = null;
    link.selfId = "";
    link.generation += 1;
    window.clearInterval(link.pingTimer);
    window.clearTimeout(link.retryTimer);
    link.pingTimer = 0;
    link.retryTimer = 0;
    if (!socket) return;
    try {
      socket.close(1000, reason);
    } catch (_) {}
  }

  closeLink(id) {
    const link = this.links.get(id);
    if (!link) return;
    this.retireSocket(link, "out of range");
    this.links.delete(id);
    if (link.peers.size) this.onPeersChanged();
  }

  destroy() {
    this.destroyed = true;
    for (const id of [...this.links.keys()]) this.closeLink(id);
    this.setVisit("");
  }
}

// Single-hexagon neighbours run their own World page, so walking onto one is
// a page load. The address names where the walker came from, the slot, and
// where they stepped off relative to the hexagon's centre; a signed-in
// walker's visa rides in the fragment, which no server or Referer sees.
export const WORLD_MESH_FROM_PARAM = "from";
export const WORLD_MESH_SLOT_PARAM = "slot";
export const WORLD_MESH_POS_PARAM = "pos";
export const WORLD_MESH_VISA_PARAM = "visa";
export const WORLD_MESH_POS_LIMIT = 400;
const POS_RE = /^-?\d{1,3}(?:\.\d)?,-?\d{1,3}(?:\.\d)?$/;
const VISA_RE = /^[A-Za-z0-9_-]{1,1950}\.[0-9a-f]{64}$/;
const FROM_RE = /^(?:forkmesh|[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?)$/;
// scene/layout.js is bundled with the scene, so this module keeps its own
// copy of the two sizes it needs (a test holds them equal).
export const WORLD_MESH_HEX_APOTHEM = 365;
export const WORLD_MESH_ROAD_WIDTH = 10;
const ARRIVAL_EDGE_REACH = 150;
const ARRIVAL_DEPTH = 14;

function roundedPos(value) {
  const number = Math.round(Number(value) * 10) / 10;
  if (!Number.isFinite(number)) return null;
  return Math.max(-WORLD_MESH_POS_LIMIT, Math.min(WORLD_MESH_POS_LIMIT, number));
}

export function worldMeshCrossingURL(world, { from, slot, local, visa = "" } = {}) {
  const origin = httpsOrigin(world);
  const source = String(from || "").toLowerCase();
  const x = roundedPos(local?.x);
  const z = roundedPos(local?.z);
  if (!origin || !FROM_RE.test(source) || x === null || z === null) return "";
  const url = new URL("/world/", origin);
  url.searchParams.set(WORLD_MESH_FROM_PARAM, source);
  const index = Number(slot);
  if (Number.isInteger(index) && index >= 0 && index < WORLD_MESH_SLOT_COUNT) {
    url.searchParams.set(WORLD_MESH_SLOT_PARAM, String(index));
  }
  const format = (value) => value.toFixed(1).replace(/\.0$/, "");
  url.searchParams.set(WORLD_MESH_POS_PARAM, `${format(x)},${format(z)}`);
  if (VISA_RE.test(String(visa || ""))) {
    url.hash = new URLSearchParams({ [WORLD_MESH_VISA_PARAM]: visa }).toString();
  }
  return url.href;
}

export function worldMeshArrival(search = "") {
  const params = new URLSearchParams(String(search || ""));
  const from = String(params.get(WORLD_MESH_FROM_PARAM) || "").toLowerCase();
  const rawPos = String(params.get(WORLD_MESH_POS_PARAM) || "");
  const slotText = String(params.get(WORLD_MESH_SLOT_PARAM) ?? "");
  if (!FROM_RE.test(from) || !POS_RE.test(rawPos) || !/^\d{1,2}$/.test(slotText)) return null;
  const slot = Number(slotText);
  const [x, z] = rawPos.split(",").map(Number);
  if (slot >= WORLD_MESH_SLOT_COUNT) return null;
  if (Math.abs(x) > WORLD_MESH_POS_LIMIT || Math.abs(z) > WORLD_MESH_POS_LIMIT) return null;
  return { from, slot, pos: { x, z } };
}

export function withoutWorldMeshArrival(href) {
  try {
    const url = new URL(String(href || ""));
    [WORLD_MESH_FROM_PARAM, WORLD_MESH_SLOT_PARAM, WORLD_MESH_POS_PARAM].forEach((key) =>
      url.searchParams.delete(key));
    if (new URLSearchParams(url.hash.replace(/^#/, "")).has(WORLD_MESH_VISA_PARAM)) url.hash = "";
    return url.href;
  } catch (_) {
    return "";
  }
}

// Where a walker coming back off a single-hexagon neighbour lands: just
// outside the edge they crossed, on the road side, facing away from it.
export function worldMeshHexEdgeArrival(center, pos) {
  const x = Number(pos?.x);
  const z = Number(pos?.z);
  if (!center || !Number.isFinite(x) || !Number.isFinite(z)) return null;
  let normal = { x: 1, z: 0 };
  let best = -Infinity;
  for (let side = 0; side < 6; side += 1) {
    const candidate = { x: Math.cos((side * Math.PI) / 3), z: Math.sin((side * Math.PI) / 3) };
    const dot = x * candidate.x + z * candidate.z;
    if (dot > best) {
      best = dot;
      normal = candidate;
    }
  }
  const along = Math.max(-ARRIVAL_EDGE_REACH, Math.min(ARRIVAL_EDGE_REACH, -x * normal.z + z * normal.x));
  const depth = WORLD_MESH_HEX_APOTHEM + WORLD_MESH_ROAD_WIDTH + ARRIVAL_DEPTH;
  return {
    x: center.x + normal.x * depth - normal.z * along,
    z: center.z + normal.z * depth + normal.x * along,
    heading: Math.atan2(-normal.x, -normal.z),
    space: "town-square",
  };
}

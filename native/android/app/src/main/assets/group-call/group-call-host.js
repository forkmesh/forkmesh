import { createGroupCalls } from "./chat-group-calls.js";
import { createCallQualityMeter, fmmStatsReader, qualityEvent, rtcStatsReader } from "./call-quality-meter.js";

const TICKET_TIMEOUT_MS = 15000;
const QUALITY_POST_MS = 1000;
const FACE_LABEL = "Face stream";
const FACE_DETAIL = "Face stream. Animated from a recent photo while the connection is weak";
const asset = (name) => new URL(name, window.location.href).href;
const grid = document.getElementById("grid");
const tiles = new Map();
const ticketWaiters = new Map();
let ticketSeq = 0;
let account = "";
let calls = null;
let lastState = "";
let quality = null;
let joinRoom = "";
let identity = null;

const post = (message) => {
  if (window.FmGroupBridge) window.FmGroupBridge.post(JSON.stringify(message));
};

function toBase64(bytes) {
  let text = "";
  for (const byte of bytes) text += String.fromCharCode(byte);
  return btoa(text);
}

function fromBase64(text) {
  const plain = String(text || "").replace(/-/g, "+").replace(/_/g, "/");
  const raw = atob(plain + "===".slice((plain.length + 3) % 4));
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
  return bytes;
}

function useIdentity(value) {
  const call = value && typeof value.call === "string" ? value.call : "";
  const acct = value && typeof value.acct === "string" ? value.acct : "";
  const idk = value && typeof value.idk === "string" ? value.idk : "";
  identity = call && acct && idk ? { call, acct, idk } : null;
}

async function callSigner() {
  const current = identity;
  const bridge = window.FmGroupBridge;
  if (!current || !bridge || typeof bridge.sign !== "function") return null;
  const idk = fromBase64(current.idk);
  if (idk.length !== 32) return null;
  return {
    idk,
    async sign(bytes) {
      const signature = bridge.sign(current.call, toBase64(bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || [])));
      if (!signature) throw new Error("signing refused");
      return fromBase64(signature);
    },
  };
}

const initial = (name) => (String(name || "").trim().charAt(0) || "?").toUpperCase();

function fetchTicket(channel, body = {}) {
  const id = ++ticketSeq;
  return new Promise((resolve, reject) => {
    ticketWaiters.set(id, { resolve, reject });
    post({ event: "ticket", id, channel, body: body || {} });
    setTimeout(() => {
      const waiter = ticketWaiters.get(id);
      if (!waiter) return;
      ticketWaiters.delete(id);
      waiter.reject(new Error("ticket timed out"));
    }, TICKET_TIMEOUT_MS);
  });
}

window.fmGroupTicket = (id, ok, value) => {
  const waiter = ticketWaiters.get(id);
  if (!waiter) return;
  ticketWaiters.delete(id);
  if (ok && value) waiter.resolve(value);
  else waiter.reject(new Error(String(value || "ticket failed")));
};

function sendFrame(channel, type, body) {
  post({ event: "send", channel, type, body: body || {} });
  return Promise.resolve(true);
}

function tileFor(key, self) {
  let entry = tiles.get(key);
  if (entry) return entry;
  const element = document.createElement("div");
  element.className = "tile" + (self ? " self" : "");
  element.setAttribute("role", "listitem");
  const video = document.createElement("video");
  video.autoplay = true;
  video.playsInline = true;
  video.muted = self;
  const avatar = document.createElement("div");
  avatar.className = "avatar";
  const name = document.createElement("span");
  name.className = "name";
  const badge = document.createElement("span");
  badge.className = "badge";
  badge.setAttribute("role", "note");
  badge.hidden = true;
  const warning = document.createElement("span");
  warning.className = "warning";
  warning.setAttribute("role", "alert");
  warning.hidden = true;
  element.append(video, avatar, name, badge, warning);
  entry = { element, video, avatar, name, badge, warning };
  tiles.set(key, entry);
  return entry;
}

function showTile(entry, stream, label, blank, state = {}) {
  if (entry.video.srcObject !== stream) entry.video.srcObject = stream || null;
  if (entry.name.textContent !== label) entry.name.textContent = label;
  const letter = initial(label === "You" ? account : label);
  if (entry.avatar.textContent !== letter) entry.avatar.textContent = letter;
  entry.element.classList.toggle("blank", blank);
  entry.element.setAttribute("aria-label", label);
  const face = state.rep === "face";
  entry.badge.hidden = !face;
  entry.badge.textContent = face ? FACE_LABEL : "";
  entry.badge.setAttribute("aria-label", face ? FACE_DETAIL : "");
  const auth = state.auth || "";
  const warn = auth === "forged"
    ? `${label}'s video failed a check. Someone in this call may be faking it.`
    : auth === "missing" ? `Can't confirm this is ${label} right now` : "";
  entry.element.classList.toggle("forged", auth === "forged");
  entry.warning.hidden = !warn;
  if (entry.warning.textContent !== warn) entry.warning.textContent = warn;
}

const hasPicture = (stream) => Boolean(stream && stream.getVideoTracks().some((track) => track.readyState === "live"));

function render() {
  const active = calls && calls.active;
  const order = [];
  if (active) {
    const self = tileFor("self", true);
    showTile(self, active.local, "You", active.media !== "video" || active.cameraOff || !hasPicture(active.local));
    order.push("self");
    for (const [peer, tile] of active.tiles) {
      const entry = tileFor(peer, false);
      showTile(entry, tile.stream, tile.name || "Someone", !hasPicture(tile.stream), { rep: tile.rep, auth: tile.auth });
      order.push(peer);
    }
  }
  for (const [key, entry] of tiles) {
    if (order.includes(key)) continue;
    entry.video.srcObject = null;
    entry.element.remove();
    tiles.delete(key);
  }
  order.forEach((key, index) => {
    const element = tiles.get(key).element;
    if (grid.children[index] !== element) grid.insertBefore(element, grid.children[index] || null);
  });
}

function syncQuality(active) {
  if (quality && quality.call === active) return;
  if (quality) {
    clearInterval(quality.timer);
    quality.meter.stop();
  }
  quality = null;
  if (!active) return;
  const current = () => calls.active === active;
  const rtc = rtcStatsReader(() => (current() && active.pc ? [active.pc] : []));
  const fmm = fmmStatsReader(() => (current() ? active.fmm : null));
  const meter = createCallQualityMeter({
    readStats: () => (active.fmm ? fmm() : rtc()),
    tracks: () => ({
      mic: current() ? active.local.getAudioTracks()[0] || null : null,
      peers: current() ? [...active.tiles.values()].flatMap((tile) => (tile.stream ? tile.stream.getAudioTracks() : [])) : [],
    }),
  });
  const timer = setInterval(() => {
    if (!current()) return;
    post({ ...qualityEvent(meter.latest, meter.drain()), live: active.state === "live" });
  }, QUALITY_POST_MS);
  quality = { call: active, meter, timer };
}

function names(active, peers) {
  return (Array.isArray(peers) ? peers : []).map((peer) => String(active.tiles.get(peer)?.name || "someone"));
}

function verification(active) {
  const view = active && active.engine === "fmm" ? active.verification : null;
  if (!view) return null;
  const code = view.code;
  return {
    code: code ? { emoji: code.emojiText.split(" "), emojiNames: code.emojiNames.slice(), digits: String(code.digits || "") } : null,
    waiting: names(active, view.waiting),
    late: Boolean(view.late),
    verified: Boolean(view.verified),
    newcomers: names(active, view.newcomers),
    unfinished: Number(view.unfinished) || 0,
    mismatch: Boolean(view.mismatch),
  };
}

function changed() {
  const active = calls.active;
  render();
  syncQuality(active || null);
  const state = {
    event: "state",
    active: Boolean(active),
    state: active ? String(active.state || "") : "",
    others: active ? active.tiles.size : 0,
    muted: Boolean(active && active.muted),
    cameraOff: Boolean(active && active.cameraOff),
    error: active ? String(active.error || "") : String(calls.lastError || ""),
    engine: active ? String(active.engine || "") : "",
    verify: verification(active),
  };
  const text = JSON.stringify(state);
  if (text === lastState) return;
  lastState = text;
  post(state);
}

const ops = {
  async join(channel, media, name, accent, chatRoom, callIdentity) {
    account = String(name || "");
    useIdentity(callIdentity);
    joinRoom = identity ? identity.call : String(chatRoom || "");
    if (accent) document.documentElement.style.setProperty("--fm-accent", String(accent));
    return calls.join(String(channel), String(channel), media === "video" ? "video" : "audio");
  },
  async leave() {
    calls.leave();
    return true;
  },
  async mute() {
    calls.toggleMute();
    return true;
  },
  async camera() {
    calls.toggleCamera();
    return true;
  },
  async rekeyed(channel, chatRoom, callIdentity) {
    if (callIdentity !== undefined) useIdentity(callIdentity);
    if (chatRoom) joinRoom = identity ? identity.call : String(chatRoom);
    return calls.channelRekeyed(String(channel), String(chatRoom || ""));
  },
  async confirm(match) {
    return calls.confirmCode(Boolean(match));
  },
  async frame(plain, channel) {
    calls.handleFrame(plain, String(channel));
    return true;
  },
};

window.fmGroup = (id, name, args) => {
  const op = Object.prototype.hasOwnProperty.call(ops, name) ? ops[name] : null;
  Promise.resolve()
    .then(() => {
      if (!op || !calls) throw new Error("unknown group call operation");
      return op(...(Array.isArray(args) ? args : []));
    })
    .then((value) => post({ event: "result", id, ok: true, value: value === undefined ? null : value }))
    .catch((error) => post({ event: "result", id, ok: false, error: String((error && error.message) || error) }));
  return true;
};

async function start() {
  const source = await (await fetch("chat-group-call-crypto.js")).text();
  const workerUrl = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
  calls = createGroupCalls({
    sendFrame,
    fetchTicket,
    accountName: () => account.trim().toLowerCase(),
    onChange: changed,
    workerUrl,
    scope: window,
    callSigner,
    callAccount: () => (identity ? identity.acct : ""),
    callId: () => joinRoom,
    fmm: {
      workerUrl: asset("fmm/fmm-worker.js"),
      workletUrl: asset("fmm/fmm-playout-worklet.js"),
      faceStreamUrl: asset("chat-call-facestream.js"),
    },
  });
  post({ event: "ready" });
}

start().catch((error) => post({ event: "state", active: false, state: "", others: 0, error: String((error && error.message) || error) }));

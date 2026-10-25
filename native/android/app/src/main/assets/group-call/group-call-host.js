import { createGroupCalls } from "./chat-group-calls.js";
import { createCallQualityMeter, qualityEvent, rtcStatsReader } from "./call-quality-meter.js";

const TICKET_TIMEOUT_MS = 15000;
const QUALITY_POST_MS = 1000;
const grid = document.getElementById("grid");
const tiles = new Map();
const ticketWaiters = new Map();
let ticketSeq = 0;
let account = "";
let calls = null;
let lastState = "";
let quality = null;

const post = (message) => {
  if (window.FmGroupBridge) window.FmGroupBridge.post(JSON.stringify(message));
};

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
  element.append(video, avatar, name);
  entry = { element, video, avatar, name };
  tiles.set(key, entry);
  return entry;
}

function showTile(entry, stream, label, blank) {
  if (entry.video.srcObject !== stream) entry.video.srcObject = stream || null;
  if (entry.name.textContent !== label) entry.name.textContent = label;
  const letter = initial(label === "You" ? account : label);
  if (entry.avatar.textContent !== letter) entry.avatar.textContent = letter;
  entry.element.classList.toggle("blank", blank);
  entry.element.setAttribute("aria-label", label);
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
      showTile(entry, tile.stream, tile.name || "Someone", !hasPicture(tile.stream));
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
  const meter = createCallQualityMeter({
    readStats: rtcStatsReader(() => (current() && active.pc ? [active.pc] : [])),
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
  };
  const text = JSON.stringify(state);
  if (text === lastState) return;
  lastState = text;
  post(state);
}

const ops = {
  async join(channel, media, name, accent) {
    account = String(name || "");
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
    callSigner: null,
    fmm: { enabled: false },
  });
  post({ event: "ready" });
}

start().catch((error) => post({ event: "state", active: false, state: "", others: 0, error: String((error && error.message) || error) }));

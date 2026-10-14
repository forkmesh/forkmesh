// The "someone just joined ForkMesh" celebration, shared by every websocket
// client that holds the public #general room: the /chat page (chat.js), the
// dashboard and the World's embedded chat panel (dashboard-chat.js).
//
// The relay is the only sender of the frame behind this. It signs the notice
// with a secret no browser holds, and the room Durable Object drops every
// non-cipher frame a peer tries to send, so a visitor cannot forge a welcome
// for a name that never signed up.
//
// The World's 3D client runs its own scene-styled copy of this celebration
// (world.js celebrateNewMember) off its presence socket. Both sockets carry
// the same news inside the World, so both mark the overlay with
// data-forkmesh-member-welcome and stand down when the other got there first.

const WELCOME_MS = 12000;
export const NEW_MEMBER_GREETING = (name) =>
  `🎉 ${name} just joined ForkMesh. Say hello in #welcome.`;
const STYLE_ID = "forkmesh-member-welcome-style";
// Matches valid_node_name in the Worker: lowercase, digits, inner hyphens.
const HANDLE_RE = /^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$/;

let welcomeTimer = 0;

const STYLE_TEXT = `
.fm-member-welcome{position:fixed;inset:0;z-index:2147483000;overflow:hidden;
pointer-events:none;font-family:inherit}
.fm-member-welcome i{position:absolute;opacity:0}
.fm-member-welcome .fm-spark{top:var(--y);left:var(--x);width:7px;height:7px;
border-radius:50%;background:hsl(var(--hue) 100% 68%);box-shadow:
0 -34px 0 hsl(var(--hue) 100% 68%),
24px -24px 0 hsl(calc(var(--hue) + 22) 100% 68%),
34px 0 0 hsl(calc(var(--hue) + 44) 100% 68%),
24px 24px 0 hsl(calc(var(--hue) + 66) 100% 68%),
0 34px 0 hsl(calc(var(--hue) + 88) 100% 68%),
-24px 24px 0 hsl(calc(var(--hue) + 110) 100% 68%),
-34px 0 0 hsl(calc(var(--hue) + 132) 100% 68%),
-24px -24px 0 hsl(calc(var(--hue) + 154) 100% 68%);
animation:fm-member-firework var(--duration) var(--delay) ease-out infinite}
.fm-member-welcome .fm-flake{top:-24px;left:var(--x);width:8px;height:14px;
border-radius:2px;background:hsl(var(--hue) 90% 62%);
animation:fm-member-confetti var(--duration) var(--delay) linear infinite}
@keyframes fm-member-firework{0%,68%{opacity:0;transform:scale(.05)}
72%{opacity:1}92%{opacity:.85;transform:scale(1)}
100%{opacity:0;transform:scale(1.25) translateY(14px)}}
@keyframes fm-member-confetti{0%{opacity:0;transform:translate3d(0,0,0) rotate(0)}
8%,92%{opacity:1}
100%{opacity:0;transform:translate3d(var(--drift),100vh,0) rotate(720deg)}}
.fm-member-welcome .fm-cheer{position:absolute;left:50%;top:38%;
transform:translate(-50%,-50%);max-width:min(90vw,480px);padding:16px 20px;
border:1px solid #30363d;border-radius:12px;background:#161b22;color:#f0f6fc;
text-align:center;box-shadow:0 16px 48px rgb(1 4 9/.5);
animation:fm-member-cheer 6s ease-out forwards}
[data-dashboard-theme] .fm-member-welcome .fm-cheer{
border-color:rgb(var(--dashboard-border-rgb));background:rgb(var(--dashboard-popover-rgb));
color:rgb(var(--dashboard-foreground-rgb))}
.fm-member-welcome .fm-cheer b{display:block;font-size:20px;font-weight:600;line-height:1.3}
.fm-member-welcome .fm-cheer span{display:block;margin-top:4px;font-size:14px;opacity:.75}
@keyframes fm-member-cheer{0%{opacity:0;transform:translate(-50%,calc(-50% + 8px))}
6%{opacity:1;transform:translate(-50%,-50%)}80%{opacity:1}
100%{opacity:0;transform:translate(-50%,-50%)}}
@media (prefers-reduced-motion:reduce){.fm-member-welcome i{display:none}}`;

function ensureStyle() {
  if (document.getElementById(STYLE_ID)) return;
  const style = document.createElement("style");
  style.id = STYLE_ID;
  style.textContent = STYLE_TEXT;
  document.head.append(style);
}

function appendParticles(layer, scale = 1) {
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  for (let index = 0; index < 40 * scale; index += 1) {
    const spark = document.createElement("i");
    spark.className = "fm-spark";
    spark.style.setProperty("--x", `${5 + Math.random() * 90}%`);
    spark.style.setProperty("--y", `${5 + Math.random() * 62}%`);
    spark.style.setProperty("--delay", `${Math.random() * -4}s`);
    spark.style.setProperty("--duration", `${1.8 + Math.random() * 2.7}s`);
    spark.style.setProperty("--hue", `${Math.floor(Math.random() * 360)}`);
    layer.append(spark);
  }
  for (let index = 0; index < 60 * scale; index += 1) {
    const flake = document.createElement("i");
    flake.className = "fm-flake";
    flake.style.setProperty("--x", `${Math.random() * 100}%`);
    flake.style.setProperty("--delay", `${Math.random() * -3.5}s`);
    flake.style.setProperty("--duration", `${2.6 + Math.random() * 2.4}s`);
    flake.style.setProperty("--drift", `${-60 + Math.random() * 120}px`);
    flake.style.setProperty("--hue", `${Math.floor(Math.random() * 360)}`);
    layer.append(flake);
  }
}

export function celebrateNewMember(rawName) {
  const name = String(rawName || "").trim().toLowerCase();
  if (!HANDLE_RE.test(name)) return false;
  const existing = document.querySelector("[data-forkmesh-member-welcome]");
  // The World's own celebration (or an earlier frame for the same person)
  // already owns the screen.
  if (existing?.dataset.forkmeshMemberWelcome === name) return false;
  existing?.remove();
  clearTimeout(welcomeTimer);
  ensureStyle();

  const layer = document.createElement("section");
  layer.className = "fm-member-welcome";
  layer.dataset.forkmeshMemberWelcome = name;
  layer.setAttribute("aria-hidden", "true");
  appendParticles(layer);
  document.body.append(layer);

  welcomeTimer = window.setTimeout(() => {
    layer.remove();
    welcomeTimer = 0;
  }, WELCOME_MS);
  return true;
}

// A relayed frame is only a new-member announcement when the relay said so.
export function isNewMemberFrame(envelope) {
  return Boolean(envelope) && envelope.kind === "forkmesh-new-member";
}

const CHEER_MS = 9000;
const CHEER_FRESH_MS = 5 * 60 * 1000;
const CHEER_SEEN_KEY = "forkmesh:celebrated-pings";
const CHEER_SEEN_MAX = 40;
let cheerTimer = 0;

export function freshCelebrations(notifications, seenIds, now = Date.now()) {
  const seen = new Set(seenIds || []);
  return (Array.isArray(notifications) ? notifications : []).filter((item) => {
    const id = String(item?.id || "");
    const ts = Number(item?.ts) || 0;
    return Boolean(id) && !seen.has(id) && !(Number(item?.readAt) > 0) &&
      Boolean(String(item?.meta?.celebrate || "").trim()) &&
      Math.abs(now - ts) <= CHEER_FRESH_MS;
  });
}

function readSeen() {
  try {
    const list = JSON.parse(localStorage.getItem(CHEER_SEEN_KEY) || "[]");
    return Array.isArray(list) ? list.map(String) : [];
  } catch (_) {
    return [];
  }
}

function writeSeen(ids) {
  try {
    localStorage.setItem(
      CHEER_SEEN_KEY, JSON.stringify(ids.slice(-CHEER_SEEN_MAX)));
  } catch (_) {}
}

export function celebrateMoment(title, body = "") {
  document.querySelector("[data-forkmesh-celebration]")?.remove();
  clearTimeout(cheerTimer);
  ensureStyle();
  const layer = document.createElement("section");
  layer.className = "fm-member-welcome";
  layer.dataset.forkmeshCelebration = "1";
  layer.setAttribute("aria-hidden", "true");
  appendParticles(layer, 2);
  const cheer = document.createElement("div");
  cheer.className = "fm-cheer";
  const headline = document.createElement("b");
  headline.textContent = String(title || "🎉");
  cheer.append(headline);
  if (body) {
    const detail = document.createElement("span");
    detail.textContent = String(body);
    cheer.append(detail);
  }
  layer.append(cheer);
  document.body.append(layer);
  cheerTimer = window.setTimeout(() => {
    layer.remove();
    cheerTimer = 0;
  }, CHEER_MS);
  return true;
}

export function celebrateFreshPings(notifications) {
  const seen = readSeen();
  const fresh = freshCelebrations(notifications, seen);
  if (!fresh.length) return false;
  writeSeen([...seen, ...fresh.map((item) => String(item.id))]);
  const newest = fresh.reduce((best, item) =>
    (Number(item.ts) || 0) > (Number(best.ts) || 0) ? item : best);
  return celebrateMoment(newest.title, newest.body);
}

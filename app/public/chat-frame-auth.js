import { FRAME_AUTH, isNodeKeyId, withoutFrameAuth } from "./chat-seal.js";

export const NODE_OWNER_ENDPOINT = "/api/chat/node-owner";
export const NODE_BIND_ENDPOINT = "/api/chat/node-bind";
export const NODE_OWNER_TTL_MS = 60 * 60 * 1000;
const NODE_OWNER_RETRY_MS = 60 * 1000;
const NODE_OWNER_KEPT = 512;
const NODE_OWNER_STORE = "forkmesh.chat.nodeOwners.v1";
const NODE_BIND_STORE = "forkmesh.chat.nodeBound.v1";
const NODE_BIND_REFRESH_MS = 24 * 60 * 60 * 1000;
const LOOKUP_TIMEOUT_MS = 6000;
const LOOKUPS_AT_ONCE = 6;
const GUEST_RECHECKS = 10;
const GUEST_PENDING_NODES = 256;
const GUEST_PENDING_LINES = 200;
const LEGACY_SELF_ID_KEY = "forkmesh.chat.selfId";
const PRIVATE_RELAY_TYPES = new Set(["dm", "invite", "cove-invite", "cove-open"]);
const SERVER_SENDER_RE = /^(?:app|system|webhook|srv):/;
const ACCOUNT_RE = /^[^\s\p{C}]{1,128}$/u;

export { FRAME_AUTH };

function safeStorage(storage) {
  if (storage !== undefined) return storage;
  try {
    return globalThis.localStorage || null;
  } catch (_) {
    return null;
  }
}

function readJson(storage, key) {
  try {
    const parsed = JSON.parse(storage?.getItem(key) || "null");
    return parsed && typeof parsed === "object" && !Array.isArray(parsed) ? parsed : null;
  } catch (_) {
    return null;
  }
}

function writeJson(storage, key, value) {
  try {
    storage?.setItem(key, JSON.stringify(value));
  } catch (_) {}
}

export function normalAccount(value) {
  const name = String(value || "").trim().toLowerCase();
  return ACCOUNT_RE.test(name) ? name : "";
}

export function chatProfileFor(account) {
  const name = normalAccount(account);
  return name ? "acct:" + name : "guest";
}

export function forgetLegacySelfId(storage) {
  try {
    safeStorage(storage)?.removeItem(LEGACY_SELF_ID_KEY);
  } catch (_) {}
}

export function frameAuth(plain) {
  const auth = plain && typeof plain === "object" ? plain[FRAME_AUTH] : null;
  return auth && typeof auth === "object" ? auth : null;
}

export function isSealedPlain(plain) {
  const auth = frameAuth(plain);
  return Boolean(auth?.sealed === true && auth.node);
}

export function isServerPlain(plain) {
  return frameAuth(plain)?.srv === true;
}

export function serverPlain(framed) {
  const clean = withoutFrameAuth(framed);
  if (!clean || typeof clean !== "object" || Array.isArray(clean)) return null;
  return { ...clean, [FRAME_AUTH]: Object.freeze({ srv: true }) };
}

export function isPrivateRelayFrame(plain) {
  if (!plain || typeof plain !== "object") return false;
  if (PRIVATE_RELAY_TYPES.has(String(plain.type || ""))) return true;
  if (plain.private === true) return true;
  return typeof plain.conversation === "string" && plain.conversation.startsWith("@");
}

export function mayChangeMessage(plain, target) {
  const auth = frameAuth(plain);
  return Boolean(
    auth?.sealed === true && auth.node && target &&
      String(target.senderId || "") === auth.node,
  );
}

const SHA256_K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

function sha256Head(text) {
  const bytes = new TextEncoder().encode(text);
  const length = bytes.length;
  const words = new Uint32Array((((length + 8) >> 6) + 1) * 16);
  for (let i = 0; i < length; i += 1) words[i >> 2] |= bytes[i] << (24 - (i & 3) * 8);
  words[length >> 2] |= 0x80 << (24 - (length & 3) * 8);
  words[words.length - 1] = length * 8;
  const W = new Uint32Array(64);
  let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
  let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
  for (let base = 0; base < words.length; base += 16) {
    for (let t = 0; t < 64; t += 1) {
      if (t < 16) {
        W[t] = words[base + t];
      } else {
        const x = W[t - 15];
        const y = W[t - 2];
        W[t] = W[t - 16] + W[t - 7]
          + (((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3))
          + (((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10));
      }
    }
    let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
    for (let t = 0; t < 64; t += 1) {
      const t1 = (h + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7)))
        + ((e & f) ^ (~e & g)) + SHA256_K[t] + W[t]) | 0;
      const t2 = ((((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10)))
        + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
    h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + h) | 0;
  }
  return h0 >>> 0;
}

export function guestLabel(node) {
  const key = String(node || "");
  return key ? "Guest " + (1000 + (sha256Head(key) % 9000)) : "Guest";
}

export function integrationSenderId(value) {
  const id = String(value || "").trim();
  if (!id) return "srv:integration";
  return SERVER_SENDER_RE.test(id) ? id : "srv:" + id;
}

export function isIntegrationSenderId(value) {
  return SERVER_SENDER_RE.test(String(value || ""));
}

export function integrationName(plain) {
  const name = String(plain?.sender || plain?.bot || "").replace(/[\u0000-\u001f\u007f]/g, "").trim();
  return name.slice(0, 32) || "Integration";
}

export const BOT_NAMES = Object.freeze(["forkbot", "claude", "codex", "agent"]);
const VIA_RE = /\s·\svia\s/;

export function botViaName(plain, names = BOT_NAMES) {
  const bot = String(plain?.bot || "").trim().toLowerCase();
  if (!names.includes(bot) || !isSealedPlain(plain)) return "";
  const poster = String(plain.sender || "").trim() || guestLabel(frameAuth(plain).node);
  return `${bot} · via ${poster}`;
}

export function isBotViaName(value) {
  return VIA_RE.test(String(value || ""));
}

export function botLine(plain, names = BOT_NAMES) {
  const bot = String(plain?.bot || "").trim().toLowerCase();
  if (!names.includes(bot) || String(plain?.senderId || "") === bot) return plain;
  const sender = botViaName(plain, names);
  if (!sender) return plain;
  return { ...plain, sender, senderId: bot, postedBy: plain.senderId, botVia: true };
}

export function historyEntries(plain) {
  const auth = frameAuth(plain);
  if (!auth || !Array.isArray(plain?.entries)) return [];
  const author = String(plain.senderId || "");
  return plain.entries
    .filter((entry) => entry && typeof entry === "object" && !Array.isArray(entry))
    .filter((entry) => auth.srv === true || String(entry.senderId || "") === author)
    .filter((entry) => !isPrivateRelayFrame(entry))
    .map((entry) => ({
      ...withoutFrameAuth(entry),
      senderId: author,
      sender: plain.sender,
      accountKind: plain.accountKind,
      verifiedAccount: plain.verifiedAccount || "",
      [FRAME_AUTH]: auth,
    }));
}

export function attributeFrame(plain, options = {}) {
  const auth = frameAuth(plain);
  if (!plain || !auth) return null;
  if (auth.srv === true) {
    return {
      ...plain,
      senderId: integrationSenderId(plain.senderId),
      sender: integrationName(plain),
      accountKind: "integration",
      verifiedAccount: "",
    };
  }
  if (auth.sealed !== true || !auth.node) return null;
  const node = auth.node;
  if (auth.self === true || (options.selfNode && node === options.selfNode)) {
    const account = normalAccount(options.selfAccount);
    return {
      ...plain,
      sender: account || String(options.selfName || plain.sender || guestLabel(node)),
      accountKind: account ? "user" : "guest",
      verifiedAccount: account,
    };
  }
  const claimed = auth.acct !== node ? normalAccount(auth.acct) : "";
  const confirmed = claimed && (options.direct === true || options.owners?.cached(node) === claimed);
  const account = confirmed ? claimed : "";
  return {
    ...plain,
    sender: account || guestLabel(node),
    accountKind: account ? "user" : "guest",
    verifiedAccount: account,
  };
}

export function createNodeOwners({
  fetchImpl = (...args) => globalThis.fetch(...args),
  storage,
  now = () => Date.now(),
  ttlMs = NODE_OWNER_TTL_MS,
  retryMs = NODE_OWNER_RETRY_MS,
  endpoint = NODE_OWNER_ENDPOINT,
} = {}) {
  const store = safeStorage(storage);
  const cache = new Map();
  const inflight = new Map();
  const listeners = new Set();
  const saved = readJson(store, NODE_OWNER_STORE);
  if (saved) {
    for (const [node, entry] of Object.entries(saved)) {
      const at = Number(entry?.at);
      if (isNodeKeyId(node) && Number.isFinite(at) && entry?.ok === true && normalAccount(entry.account)) {
        cache.set(node, { account: normalAccount(entry.account), at, ok: true });
      }
    }
  }

  function fresh(entry) {
    if (!entry) return false;
    const age = now() - entry.at;
    return age >= 0 && age < (entry.ok && entry.account ? ttlMs : retryMs);
  }

  function persist() {
    const out = {};
    const kept = [...cache.entries()].filter(([, entry]) => entry.ok && entry.account && fresh(entry)).slice(-NODE_OWNER_KEPT);
    for (const [node, entry] of kept) out[node] = { account: entry.account, at: entry.at, ok: true };
    writeJson(store, NODE_OWNER_STORE, out);
  }

  function remember(node, account, ok = true) {
    cache.delete(node);
    cache.set(node, { account: normalAccount(account), at: now(), ok });
    while (cache.size > NODE_OWNER_KEPT) cache.delete(cache.keys().next().value);
    if (ok) persist();
    for (const listener of [...listeners]) {
      try { listener(node); } catch (_) {}
    }
  }

  async function ask(node) {
    const controller = typeof AbortController === "function" ? new AbortController() : null;
    const timer = controller ? setTimeout(() => controller.abort(), LOOKUP_TIMEOUT_MS) : null;
    try {
      const response = await fetchImpl(`${endpoint}?node=${encodeURIComponent(node)}`, {
        headers: { accept: "application/json" },
        credentials: "same-origin",
        cache: "no-store",
        signal: controller?.signal,
      });
      if (!response?.ok) {
        remember(node, "", false);
        return "";
      }
      const body = await response.json().catch(() => null);
      if (!body || typeof body !== "object" || typeof body.account !== "string") {
        remember(node, "", false);
        return "";
      }
      const account = normalAccount(body.account);
      remember(node, account, true);
      return account;
    } catch (_) {
      remember(node, "", false);
      return "";
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  function lookup(node) {
    const id = String(node || "");
    if (!isNodeKeyId(id)) return Promise.resolve("");
    const entry = cache.get(id);
    if (fresh(entry)) return Promise.resolve(entry.ok ? entry.account : "");
    if (inflight.has(id)) return inflight.get(id);
    const pending = ask(id).finally(() => inflight.delete(id));
    inflight.set(id, pending);
    return pending;
  }

  function cached(node) {
    const entry = cache.get(String(node || ""));
    return fresh(entry) && entry.ok ? entry.account : "";
  }

  function needsLookup(plain, selfNode) {
    const auth = frameAuth(plain);
    if (!auth || auth.sealed !== true || auth.self === true || !auth.node) return "";
    if (auth.node === selfNode || auth.acct === auth.node || !normalAccount(auth.acct)) return "";
    return fresh(cache.get(auth.node)) ? "" : auth.node;
  }

  async function prefetch(plains, { selfNode = "" } = {}) {
    const nodes = [...new Set((plains || []).map((plain) => needsLookup(plain, selfNode)).filter(Boolean))];
    let index = 0;
    const worker = async () => {
      while (index < nodes.length) {
        const node = nodes[index];
        index += 1;
        await lookup(node);
      }
    };
    await Promise.all(Array.from({ length: Math.min(LOOKUPS_AT_ONCE, nodes.length) }, worker));
  }

  return {
    lookup,
    cached,
    prefetch,
    remember: (node, account) => remember(String(node || ""), account, true),
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}

const bindsInFlight = new Map();

export function bindNodeKey(options = {}) {
  const key = normalAccount(options.account) + "\n" + String(options.store?.node || "");
  if (bindsInFlight.has(key)) return bindsInFlight.get(key);
  const pending = bindNodeKeyOnce(options).finally(() => bindsInFlight.delete(key));
  bindsInFlight.set(key, pending);
  return pending;
}

async function bindNodeKeyOnce({
  store,
  account,
  headers = {},
  fetchImpl = (...args) => globalThis.fetch(...args),
  storage,
  now = () => Date.now(),
  owners = null,
  force = false,
} = {}) {
  const name = normalAccount(account);
  if (!name || typeof store?.nodeBindProof !== "function") return false;
  const node = await store.ready();
  if (!isNodeKeyId(node)) return false;
  const memory = safeStorage(storage);
  const marker = name + "\n" + node;
  const bound = readJson(memory, NODE_BIND_STORE) || {};
  const at = Number(bound[marker]) || 0;
  if (!force && at && now() - at >= 0 && now() - at < NODE_BIND_REFRESH_MS) {
    owners?.remember(node, name);
    return true;
  }
  const proof = await store.nodeBindProof(name);
  let response;
  try {
    response = await fetchImpl(NODE_BIND_ENDPOINT, {
      method: "POST",
      headers: { accept: "application/json", "content-type": "application/json", ...headers },
      credentials: "same-origin",
      cache: "no-store",
      body: JSON.stringify(proof),
    });
  } catch (_) {
    return false;
  }
  if (!response?.ok) return false;
  const answer = await response.json().catch(() => null);
  if (answer?.ok !== true || normalAccount(answer.account) !== name || answer.node !== node) return false;
  writeJson(memory, NODE_BIND_STORE, { [marker]: now() });
  owners?.remember(node, name);
  return true;
}

export function unconfirmedClaim(plain) {
  const auth = frameAuth(plain);
  if (!auth || auth.sealed !== true || auth.self === true || !auth.node) return "";
  if (plain.accountKind !== "guest" || plain.botVia) return "";
  return auth.acct !== auth.node ? normalAccount(auth.acct) : "";
}

export function createGuestRelabeler({
  owners,
  onConfirmed,
  retryMs = NODE_OWNER_RETRY_MS,
  tries = GUEST_RECHECKS,
  setTimer = (...args) => globalThis.setTimeout(...args),
  clearTimer = (...args) => globalThis.clearTimeout(...args),
} = {}) {
  const pending = new Map();

  function drop(node) {
    const entry = pending.get(node);
    if (!entry) return;
    if (entry.timer) clearTimer(entry.timer);
    pending.delete(node);
  }

  function settle(node) {
    const entry = pending.get(node);
    if (!entry) return;
    const account = owners?.cached(node) || "";
    if (!account) return;
    const ids = [];
    for (const [id, claim] of entry.lines) {
      if (claim !== account) continue;
      ids.push(id);
      entry.lines.delete(id);
    }
    if (!entry.lines.size) drop(node);
    if (ids.length) {
      try { onConfirmed?.(node, account, ids); } catch (_) {}
    }
  }

  function schedule(node) {
    const entry = pending.get(node);
    if (!entry || entry.timer || entry.tries >= tries) return;
    entry.timer = setTimer(() => {
      entry.timer = null;
      entry.tries += 1;
      if (pending.get(node) !== entry) return;
      Promise.resolve(owners?.lookup(node)).catch(() => "").then(() => {
        settle(node);
        schedule(node);
      });
    }, retryMs + 1000);
  }

  function noteLine(node, id, claim) {
    let entry = pending.get(node);
    if (!entry) {
      if (pending.size >= GUEST_PENDING_NODES) drop(pending.keys().next().value);
      entry = { lines: new Map(), tries: 0, timer: null };
      pending.set(node, entry);
    }
    entry.lines.delete(id);
    entry.lines.set(id, claim);
    while (entry.lines.size > GUEST_PENDING_LINES) entry.lines.delete(entry.lines.keys().next().value);
  }

  function note(plain) {
    const claim = unconfirmedClaim(plain);
    if (!claim) return;
    const node = frameAuth(plain).node;
    const lines = plain.type === "history" ? historyEntries(plain) : [plain];
    let added = false;
    for (const line of lines) {
      const id = String(line?.id || "");
      if (!id || (line.type && line.type !== "chat" && line.type !== "thread-reply")) continue;
      noteLine(node, id, claim);
      added = true;
    }
    if (added) schedule(node);
  }

  const unsubscribe = owners?.subscribe?.(settle);

  return {
    note,
    settle,
    pending: () => [...pending.entries()].map(([node, entry]) => [node, [...entry.lines.keys()]]),
    clear() {
      for (const node of [...pending.keys()]) drop(node);
    },
    stop() {
      for (const node of [...pending.keys()]) drop(node);
      unsubscribe?.();
    },
  };
}

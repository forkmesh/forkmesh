const encoder = new TextEncoder();
const decoder = new TextDecoder();

export const PROTOCOL_LABEL = "forkmesh-local-control-v1";
export const PROTOCOL_VERSION = 1;
export const BIND_ENDPOINT = "/api/accounts/devices/browser-control";
export const BIND_PROOF = "forkmesh-browser-control-v1";
export const CHUNK_CHARS = 9000;
export const MAX_FRAME_BYTES = 1024 * 1024;
const HELLO_TAG = "fm-control-hello";
const WELCOME_TAG = "fm-control-welcome";
const AUTH_TAG = "fm-control-auth";
const DB_NAME = "forkmesh-desktop-control";
const DB_STORE = "identities";
const DEFAULT_TIMEOUT_MS = 20000;

export class DesktopControlError extends Error {
  constructor(code, message = "") {
    super(message || DESKTOP_CONTROL_MESSAGES[code] || code);
    this.code = code;
  }
}

export const DESKTOP_CONTROL_MESSAGES = Object.freeze({
  unsupported: "This browser cannot open an encrypted channel to the desktop (it needs Ed25519 and X25519 in WebCrypto).",
  not_signed_in: "Sign in to reach your desktop.",
  timeout: "The desktop did not answer. It is reachable only while the ForkMesh desktop app is running and signed in.",
  closed: "The channel to the desktop closed.",
  wrong_desktop: "The desktop answered with a key that is not on your account.",
  bad_welcome: "The desktop's answer was malformed.",
  bad_signature: "The desktop's answer did not verify.",
  pair_on_lan: "The desktop did not recognise this browser as yours. Update the ForkMesh desktop app and try again.",
  relay_unavailable: "The relay is not reachable.",
  relay_reconnected: "The relay connection dropped; reconnecting.",
  relay_out_of_order: "A relay message went missing; reconnecting.",
  frame_did_not_authenticate: "A message from the desktop did not authenticate.",
  device_revoked: "This browser's desktop access was revoked.",
  not_owner: "Only the desktop's owner can do that.",
  unknown_session: "The desktop no longer has that agent.",
  not_running: "That agent is not running.",
  not_continuable: "That agent is still running.",
  no_checkout: "The desktop has no checkout of this repository.",
  agent_running: "An agent is still working on that branch.",
  conflicts: "The branch conflicts with its base. Ask the agent to merge the base first.",
  merge_failed: "The desktop could not merge the branch. Its log has the details.",
  name_mismatch: "The typed branch name does not match.",
  confirmation_expired: "The confirmation expired. Try again.",
  confirmation_required: "The desktop asked for a fresh confirmation. Try again.",
  confirmation_mismatch: "The confirmation did not match this action.",
  unsupported_method: "The desktop app is too old for this. Update ForkMesh on the desktop.",
});

export function bytesToB64Url(bytes) {
  let binary = "";
  for (const value of new Uint8Array(bytes)) binary += String.fromCharCode(value);
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/g, "");
}

export function b64UrlToBytes(value, length = 0) {
  const text = String(value || "");
  if (!text || text.length > 200 || !/^[A-Za-z0-9_-]+$/.test(text)) return null;
  let padded = text.replaceAll("-", "+").replaceAll("_", "/");
  while (padded.length % 4) padded += "=";
  let binary;
  try {
    binary = atob(padded);
  } catch (_) {
    return null;
  }
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
  return length && out.length !== length ? null : out;
}

export function bytesToB64(bytes) {
  let binary = "";
  const view = new Uint8Array(bytes);
  for (let i = 0; i < view.length; i += 0x8000) {
    binary += String.fromCharCode(...view.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

export function b64ToBytes(text) {
  let binary;
  try {
    binary = atob(String(text || ""));
  } catch (_) {
    return null;
  }
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
  return out;
}

function concat(...parts) {
  const total = parts.reduce((sum, part) => sum + part.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

function subtle() {
  const value = globalThis.crypto?.subtle;
  if (!value) throw new DesktopControlError("unsupported");
  return value;
}

export function transcript(clientKey, clientEph, clientNonce, serverKey, serverEph) {
  return encoder.encode(
    [PROTOCOL_LABEL, clientKey, clientEph, clientNonce, serverKey, serverEph].join("\n"),
  );
}

export function serverSignedBytes(transcriptBytes) {
  return concat(encoder.encode(`${PROTOCOL_LABEL} server\n`), transcriptBytes);
}

export function clientSignedBytes(transcriptBytes) {
  return concat(encoder.encode(`${PROTOCOL_LABEL} client\n`), transcriptBytes);
}

async function hmac(key, message) {
  const imported = await subtle().importKey(
    "raw", key, { name: "HMAC", hash: "SHA-256" }, false, ["sign"],
  );
  return new Uint8Array(await subtle().sign("HMAC", imported, message));
}

export async function deriveKeys(sharedSecret, transcriptBytes) {
  const salt = new Uint8Array(await subtle().digest("SHA-256", transcriptBytes));
  const prk = await hmac(salt, sharedSecret);
  const expand = (direction) =>
    hmac(prk, concat(encoder.encode(`${PROTOCOL_LABEL} ${direction}`), new Uint8Array([1])));
  return {
    clientToServer: await expand("c2s"),
    serverToClient: await expand("s2c"),
  };
}

function nonceFor(counter) {
  const nonce = new Uint8Array(12);
  new DataView(nonce.buffer).setBigUint64(4, BigInt(counter), false);
  return nonce;
}

async function aesKey(raw, usage) {
  return subtle().importKey("raw", raw, { name: "AES-GCM" }, false, [usage]);
}

export async function sealFrame(key, counter, plain) {
  const sealed = await subtle().encrypt(
    { name: "AES-GCM", iv: nonceFor(counter), additionalData: encoder.encode(PROTOCOL_LABEL), tagLength: 128 },
    await aesKey(key, "encrypt"),
    plain,
  );
  return new Uint8Array(sealed);
}

export async function openFrame(key, counter, frame) {
  if (!frame || frame.length < 16) return null;
  try {
    const plain = await subtle().decrypt(
      { name: "AES-GCM", iv: nonceFor(counter), additionalData: encoder.encode(PROTOCOL_LABEL), tagLength: 128 },
      await aesKey(key, "decrypt"),
      frame,
    );
    const out = new Uint8Array(plain);
    return out.length ? out : null;
  } catch (_) {
    return null;
  }
}

async function verifyEd25519(publicKeyB64Url, signatureB64Url, data) {
  const key = b64UrlToBytes(publicKeyB64Url, 32);
  const sig = b64UrlToBytes(signatureB64Url, 64);
  if (!key || !sig) return false;
  try {
    const imported = await subtle().importKey("raw", key, { name: "Ed25519" }, false, ["verify"]);
    return await subtle().verify({ name: "Ed25519" }, imported, sig, data);
  } catch (_) {
    return false;
  }
}

export async function newEphemeral() {
  const pair = await subtle().generateKey({ name: "X25519" }, true, ["deriveBits"]);
  const publicKey = new Uint8Array(await subtle().exportKey("raw", pair.publicKey));
  return { privateKey: pair.privateKey, publicKey };
}

export async function x25519(privateKey, peerPublic) {
  const peer = await subtle().importKey("raw", peerPublic, { name: "X25519" }, true, []);
  const bits = new Uint8Array(await subtle().deriveBits({ name: "X25519", public: peer }, privateKey, 256));
  if (bits.every((value) => value === 0)) throw new DesktopControlError("bad_welcome");
  return bits;
}

function openIdentityDb(indexedDb) {
  return new Promise((resolve, reject) => {
    const request = indexedDb.open(DB_NAME, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(DB_STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

function dbCall(db, mode, run) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(DB_STORE, mode);
    const request = run(tx.objectStore(DB_STORE));
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

const memoryIdentities = new Map();

export async function loadIdentity(account, { fresh = false, indexedDb = globalThis.indexedDB } = {}) {
  const key = String(account || "").toLowerCase();
  let db = null;
  if (indexedDb) {
    try {
      db = await openIdentityDb(indexedDb);
    } catch (_) {
      db = null;
    }
  }
  if (!fresh) {
    const cached = memoryIdentities.get(key)
      || (db ? await dbCall(db, "readonly", (store) => store.get(key)).catch(() => null) : null);
    if (cached?.privateKey && cached?.publicKey) {
      memoryIdentities.set(key, cached);
      return cached;
    }
  }
  let pair;
  try {
    pair = await subtle().generateKey({ name: "Ed25519" }, false, ["sign", "verify"]);
  } catch (_) {
    throw new DesktopControlError("unsupported");
  }
  const publicKey = bytesToB64Url(await subtle().exportKey("raw", pair.publicKey));
  const identity = { privateKey: pair.privateKey, publicKey, createdAt: Date.now() };
  memoryIdentities.set(key, identity);
  if (db) await dbCall(db, "readwrite", (store) => store.put(identity, key)).catch(() => {});
  return identity;
}

export async function signWith(identity, bytes) {
  return bytesToB64Url(await subtle().sign({ name: "Ed25519" }, identity.privateKey, bytes));
}

export function browserLabel(userAgent = globalThis.navigator?.userAgent || "") {
  const ua = String(userAgent);
  const browser = /Edg\//.test(ua) ? "Edge"
    : /Firefox\//.test(ua) ? "Firefox"
      : /Chrome\//.test(ua) ? "Chrome"
        : /Safari\//.test(ua) ? "Safari" : "Browser";
  const os = /iPhone|iPad/.test(ua) ? "iOS"
    : /Android/.test(ua) ? "Android"
      : /Mac OS X/.test(ua) ? "macOS"
        : /Windows/.test(ua) ? "Windows"
          : /Linux/.test(ua) ? "Linux" : "";
  return `${browser}${os ? ` on ${os}` : ""}`;
}

export async function bindIdentity({ account, sessionToken, fetchImpl = globalThis.fetch?.bind(globalThis), indexedDb } = {}) {
  const name = String(account || "").trim().toLowerCase();
  if (!name || !fetchImpl) throw new DesktopControlError("not_signed_in");
  let identity = await loadIdentity(name, { indexedDb });
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const ts = String(Date.now());
    const sig = await signWith(identity, encoder.encode(`${BIND_PROOF}\n${name}\n${identity.publicKey}\n${ts}`));
    const token = typeof sessionToken === "function" ? String(sessionToken() || "") : "";
    const headers = { accept: "application/json", "content-type": "application/json" };
    const body = { pubkey: identity.publicKey, ts, sig, label: browserLabel() };
    if (token && token !== "cookie") {
      headers.authorization = `Bearer ${token}`;
      body.sessionToken = token;
    }
    const response = await fetchImpl(BIND_ENDPOINT, {
      method: "POST",
      cache: "no-store",
      credentials: "same-origin",
      headers,
      body: JSON.stringify(body),
    });
    const payload = await response.json().catch(() => ({}));
    if (response.ok && payload?.ok) {
      return {
        identity,
        account: String(payload.account || name),
        desktopKeys: Array.isArray(payload.desktopKeys) ? payload.desktopKeys.map(String) : [],
        desktops: (Array.isArray(payload.desktops) ? payload.desktops : [])
          .map((item) => ({ key: String(item?.key || ""), node: String(item?.node || "").trim().toLowerCase() }))
          .filter((item) => item.key && /^[a-z0-9._:-]{1,63}$/.test(item.node)),
      };
    }
    const code = String(payload?.error || "");
    if (attempt === 0 && ["device_revoked", "device_key_conflict"].includes(code)) {
      identity = await loadIdentity(name, { fresh: true, indexedDb });
      continue;
    }
    if (response.status === 401 && code !== "device_revoked") throw new DesktopControlError("not_signed_in");
    throw new DesktopControlError(code || "relay_unavailable");
  }
  throw new DesktopControlError("device_revoked");
}

function newSessionId() {
  const bytes = new Uint8Array(16);
  globalThis.crypto.getRandomValues(bytes);
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
}

export function createDesktopConnection({
  node,
  identity,
  account,
  desktopKeys = [],
  channelFactory,
  timeoutMs = DEFAULT_TIMEOUT_MS,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
}) {
  const target = String(node || "").trim().toLowerCase();
  const session = newSessionId();
  const allowedKeys = new Set(desktopKeys.map(String));
  const listeners = new Map();
  const closeListeners = new Set();
  const pending = new Map();
  let stage = "connecting";
  let channel = null;
  let sendSeq = 0;
  let receiveSeq = 0;
  let sendCounter = 0;
  let receiveCounter = 0;
  let chunks = [];
  let chunkBytes = 0;
  let keys = null;
  let handshake = null;
  let nextId = 1;
  let closeReason = "";
  let inbound = Promise.resolve();
  let outbound = Promise.resolve();
  let resolveOpen;
  let rejectOpen;
  const opened = new Promise((resolve, reject) => {
    resolveOpen = resolve;
    rejectOpen = reject;
  });
  opened.catch(() => {});
  const info = { node: target, name: "", user: "", key: "", transport: "relay" };

  function emit(event, data) {
    for (const listener of listeners.get(event) || []) {
      try {
        listener(data);
      } catch (_) {}
    }
    for (const listener of listeners.get("*") || []) {
      try {
        listener(event, data);
      } catch (_) {}
    }
  }

  function sendRaw(action, payloadBytes) {
    const encoded = bytesToB64(payloadBytes);
    let at = 0;
    do {
      const end = Math.min(encoded.length, at + CHUNK_CHARS);
      const frame = {
        type: "control",
        action,
        session,
        node: target,
        seq: sendSeq,
        data: encoded.slice(at, end),
        more: end < encoded.length,
      };
      sendSeq += 1;
      if (!channel || channel.send(frame) === false) return false;
      at = end;
    } while (at < encoded.length);
    return true;
  }

  function finish(reason, notifyDesktop) {
    if (stage === "closed") return;
    const started = stage !== "connecting" || sendSeq > 0;
    stage = "closed";
    closeReason = reason || "closed";
    if (notifyDesktop && started && channel) {
      try {
        channel.send({ type: "control", action: "close", session, node: target });
      } catch (_) {}
    }
    try {
      channel?.dispose?.();
    } catch (_) {}
    channel = null;
    const error = new DesktopControlError(closeReason);
    rejectOpen(error);
    for (const entry of pending.values()) {
      clearTimeoutImpl(entry.timer);
      entry.reject(error);
    }
    pending.clear();
    for (const listener of closeListeners) {
      try {
        listener(closeReason);
      } catch (_) {}
    }
  }

  async function onWelcome(payload) {
    let welcome;
    try {
      welcome = JSON.parse(decoder.decode(payload));
    } catch (_) {
      return;
    }
    if (welcome?.t !== WELCOME_TAG) return;
    const serverKey = String(welcome.key || "");
    const serverEph = String(welcome.eph || "");
    const serverEphBytes = b64UrlToBytes(serverEph, 32);
    if (welcome.v !== PROTOCOL_VERSION || !b64UrlToBytes(serverKey, 32) || !serverEphBytes) {
      finish("bad_welcome", true);
      return;
    }
    if (!allowedKeys.has(serverKey)) {
      finish("wrong_desktop", true);
      return;
    }
    const t = transcript(identity.publicKey, handshake.eph, handshake.nonce, serverKey, serverEph);
    if (!(await verifyEd25519(serverKey, String(welcome.sig || ""), serverSignedBytes(t)))) {
      finish("bad_signature", true);
      return;
    }
    const shared = await x25519(handshake.ephemeral.privateKey, serverEphBytes);
    keys = await deriveKeys(shared, t);
    info.name = String(welcome.name || "");
    info.user = String(welcome.user || "");
    info.key = serverKey;
    stage = "authenticating";
    const auth = { t: AUTH_TAG, sig: await signWith(identity, clientSignedBytes(t)) };
    if (!sendRaw("data", encoder.encode(JSON.stringify(auth)))) finish("relay_unavailable", false);
  }

  async function onSealed(payload) {
    const plain = await openFrame(keys.serverToClient, receiveCounter, payload);
    if (!plain) {
      finish("frame_did_not_authenticate", true);
      return;
    }
    receiveCounter += 1;
    let message;
    try {
      message = JSON.parse(decoder.decode(plain));
    } catch (_) {
      return;
    }
    if (!message || typeof message !== "object") return;
    if (typeof message.event === "string") {
      const data = message.data && typeof message.data === "object" ? message.data : {};
      if (stage === "authenticating") {
        if (message.event === "pairing-denied") {
          finish(String(data.reason || "pair_on_lan"), true);
          return;
        }
        if (message.event === "session") {
          if (data.paired === true) {
            stage = "open";
            resolveOpen(info);
          } else if (data.code) {
            finish("pair_on_lan", true);
          }
          return;
        }
      }
      emit(message.event, data);
      return;
    }
    const entry = pending.get(Number(message.id));
    if (!entry) return;
    pending.delete(Number(message.id));
    clearTimeoutImpl(entry.timer);
    if (message.ok === true) {
      entry.resolve(message.result && typeof message.result === "object" ? message.result : {});
    } else {
      const code = String(message.error || "failed");
      const text = typeof message.message === "string" ? message.message.trim() : "";
      entry.reject(new DesktopControlError(code === "unsupported" ? "unsupported_method" : code, code === "unsupported" ? "" : text));
    }
  }

  function onFrame(frame) {
    if (stage === "closed" || !frame || frame.type !== "control") return;
    if (frame.session !== session || frame.from !== "node") return;
    if (frame.action === "close" || frame.action === "error") {
      const reason = String(frame.reason || "closed");
      finish(/pair_on_lan|Approve this phone/i.test(reason) ? "pair_on_lan" : reason, false);
      return;
    }
    if (frame.action !== "data") return;
    if (Number(frame.seq) !== receiveSeq) {
      finish("relay_out_of_order", true);
      return;
    }
    receiveSeq += 1;
    const chunk = b64ToBytes(frame.data);
    if (!chunk || !chunk.length) {
      finish("bad_frame", true);
      return;
    }
    chunks.push(chunk);
    chunkBytes += chunk.length;
    if (chunkBytes > MAX_FRAME_BYTES) {
      finish("frame_too_large", true);
      return;
    }
    if (frame.more === true) return;
    const payload = concat(...chunks);
    chunks = [];
    chunkBytes = 0;
    inbound = inbound.then(async () => {
      if (stage === "closed") return;
      if (stage === "welcome") await onWelcome(payload);
      else if (stage === "authenticating" || stage === "open") await onSealed(payload);
    }).catch(() => finish("bad_frame", true));
  }

  async function onConnected() {
    if (stage === "closed") return;
    if (stage !== "connecting") {
      finish("relay_reconnected", false);
      return;
    }
    stage = "welcome";
    const ephemeral = await newEphemeral();
    const nonceBytes = new Uint8Array(16);
    globalThis.crypto.getRandomValues(nonceBytes);
    handshake = { ephemeral, eph: bytesToB64Url(ephemeral.publicKey), nonce: bytesToB64Url(nonceBytes) };
    const hello = {
      t: HELLO_TAG,
      v: PROTOCOL_VERSION,
      key: identity.publicKey,
      eph: handshake.eph,
      nonce: handshake.nonce,
      name: `Web · ${browserLabel()}`.slice(0, 80),
      user: String(account || "").slice(0, 80),
      platform: "web",
    };
    if (!sendRaw("open", encoder.encode(JSON.stringify(hello)))) finish("relay_unavailable", false);
  }

  const openTimer = setTimeoutImpl(() => {
    if (stage !== "open") finish("timeout", true);
  }, timeoutMs);
  opened.then(() => clearTimeoutImpl(openTimer), () => clearTimeoutImpl(openTimer));

  Promise.resolve()
    .then(() => channelFactory({ onFrame, onConnected: () => void onConnected().catch(() => finish("unsupported", true)) }))
    .then((created) => {
      if (stage === "closed") {
        created?.dispose?.();
        return;
      }
      channel = created;
      channel.start?.();
    })
    .catch(() => finish("relay_unavailable", false));

  function request(method, params = {}, requestTimeoutMs = DEFAULT_TIMEOUT_MS) {
    if (stage !== "open") return Promise.reject(new DesktopControlError("closed"));
    const id = nextId;
    nextId += 1;
    return new Promise((resolve, reject) => {
      const timer = setTimeoutImpl(() => {
        pending.delete(id);
        reject(new DesktopControlError("timeout"));
      }, requestTimeoutMs);
      pending.set(id, { resolve, reject, timer });
      const body = encoder.encode(JSON.stringify({ id, method, params }));
      outbound = outbound.then(async () => {
        if (stage !== "open") throw new DesktopControlError("closed");
        const counter = sendCounter;
        sendCounter += 1;
        const sealed = await sealFrame(keys.clientToServer, counter, body);
        if (!sendRaw("data", sealed)) {
          finish("relay_unavailable", false);
          throw new DesktopControlError("relay_unavailable");
        }
      }).catch((error) => {
        const entry = pending.get(id);
        if (entry) {
          pending.delete(id);
          clearTimeoutImpl(entry.timer);
          entry.reject(error instanceof DesktopControlError ? error : new DesktopControlError("closed"));
        }
      });
    });
  }

  return {
    session,
    info,
    opened,
    request,
    on(event, listener) {
      if (!listeners.has(event)) listeners.set(event, new Set());
      listeners.get(event).add(listener);
      return () => listeners.get(event)?.delete(listener);
    },
    onClose(listener) {
      closeListeners.add(listener);
      if (stage === "closed") listener(closeReason);
      return () => closeListeners.delete(listener);
    },
    get isOpen() {
      return stage === "open";
    },
    get closeReason() {
      return closeReason;
    },
    close() {
      finish("closed", true);
    },
    _onFrame: onFrame,
    _onConnected: onConnected,
  };
}

export function createDesktopControlHub({
  account,
  sessionToken,
  fetchImpl = globalThis.fetch?.bind(globalThis),
  channelFactory = null,
  timeoutMs = DEFAULT_TIMEOUT_MS,
} = {}) {
  const connections = new Map();
  const subscribers = new Set();
  let binding = null;
  let shared = null;
  let sharedConnected = false;

  async function defaultChannelFactory(handlers) {
    const { createAccountEventChannel } = await import("/account-events.js");
    return createAccountEventChannel({
      sessionToken,
      onFrame: handlers.onFrame,
      onConnected: handlers.onConnected,
    });
  }

  function sharedChannel() {
    if (!shared) {
      shared = Promise.resolve((channelFactory || defaultChannelFactory)({
        onFrame: (frame) => {
          for (const handlers of Array.from(subscribers)) handlers.onFrame(frame);
        },
        onConnected: () => {
          sharedConnected = true;
          for (const handlers of Array.from(subscribers)) handlers.onConnected();
        },
      })).then((channel) => {
        channel.start?.();
        return channel;
      });
      shared.catch(() => {
        shared = null;
      });
    }
    return shared;
  }

  async function multiplexedChannel(handlers) {
    const channel = await sharedChannel();
    subscribers.add(handlers);
    return {
      start() {
        if (sharedConnected) queueMicrotask(() => handlers.onConnected());
      },
      send: (frame) => channel.send(frame),
      dispose() {
        subscribers.delete(handlers);
      },
    };
  }

  function bind() {
    if (!binding) {
      binding = bindIdentity({ account, sessionToken, fetchImpl }).catch((error) => {
        binding = null;
        throw error;
      });
    }
    return binding;
  }

  function connect(node) {
    const key = String(node || "").trim().toLowerCase();
    const existing = connections.get(key);
    if (existing && (!existing.connection || existing.connection.closeReason === "")) return existing.ready;
    const entry = { connection: null, ready: null };
    entry.ready = bind().then(({ identity, account: bound, desktopKeys }) => {
      const connection = createDesktopConnection({
        node: key,
        identity,
        account: bound,
        desktopKeys,
        channelFactory: multiplexedChannel,
        timeoutMs,
      });
      entry.connection = connection;
      connection.onClose(() => {
        if (connections.get(key) === entry) connections.delete(key);
      });
      return connection.opened.then(() => connection);
    });
    entry.ready.catch(() => {
      if (connections.get(key) === entry) connections.delete(key);
    });
    connections.set(key, entry);
    return entry.ready;
  }

  return {
    connect,
    async desktops() {
      return (await bind()).desktops;
    },
    connection(node) {
      const entry = connections.get(String(node || "").trim().toLowerCase());
      return entry?.connection?.isOpen ? entry.connection : null;
    },
    closeAll() {
      for (const entry of connections.values()) entry.connection?.close();
      connections.clear();
      const current = shared;
      shared = null;
      sharedConnected = false;
      subscribers.clear();
      if (current) void current.then((channel) => channel.dispose?.(), () => {});
    },
  };
}

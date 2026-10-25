const TRAILER = 9;
const TAG = 16;
const KEEP_KEYS = 3;
const REPLAY_WINDOW = 1024n;
const REPLAY_MASK = (1n << REPLAY_WINDOW) - 1n;

const peerKeys = new Map();
let sendKey = null;
let sendRaw = null;
let sendKeyId = 0;
let counter = 0n;

function sameBytes(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i += 1) diff |= a[i] ^ b[i];
  return diff === 0;
}

function fresh(entry, value) {
  if (!entry.started || value > entry.top) return true;
  const age = entry.top - value;
  return age < REPLAY_WINDOW && ((entry.seen >> age) & 1n) === 0n;
}

function mark(entry, value) {
  if (!entry.started) {
    entry.started = true;
    entry.top = value;
    entry.seen = 1n;
  } else if (value > entry.top) {
    const shift = value - entry.top;
    entry.seen = shift >= REPLAY_WINDOW ? 1n : ((entry.seen << shift) | 1n) & REPLAY_MASK;
    entry.top = value;
  } else {
    entry.seen |= 1n << (entry.top - value);
  }
}

function readCounter(data) {
  const start = data.length - TRAILER;
  let value = 0n;
  for (let i = 0; i < 8; i += 1) value = (value << 8n) | BigInt(data[start + i]);
  return value;
}

function clearBytes(kind, frame, length) {
  if (kind === "audio") return Math.min(1, length);
  return Math.min(frame.type === "key" ? 10 : 3, length);
}

async function importKey(raw) {
  return crypto.subtle.importKey("raw", raw, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
}

function iv(value) {
  const bytes = new Uint8Array(12);
  new DataView(bytes.buffer).setBigUint64(4, value);
  return bytes;
}

async function encrypt(kind, frame, controller) {
  if (!sendKey) return;
  const key = sendKey;
  const keyId = sendKeyId;
  const data = new Uint8Array(frame.data);
  const clear = clearBytes(kind, frame, data.length);
  counter += 1n;
  const nonce = iv(counter);
  const sealed = new Uint8Array(await crypto.subtle.encrypt(
    { name: "AES-GCM", iv: nonce, additionalData: data.subarray(0, clear), tagLength: 128 },
    key,
    data.subarray(clear),
  ));
  const out = new Uint8Array(clear + sealed.length + TRAILER);
  out.set(data.subarray(0, clear), 0);
  out.set(sealed, clear);
  out.set(nonce.subarray(4), clear + sealed.length);
  out[out.length - 1] = keyId;
  frame.data = out.buffer;
  controller.enqueue(frame);
}

async function decrypt(kind, peer, frame, controller) {
  const data = new Uint8Array(frame.data);
  if (data.length < TRAILER + TAG + 1) return;
  const keyId = data[data.length - 1];
  const entry = peerKeys.get(peer)?.get(keyId);
  if (!entry) return;
  const value = readCounter(data);
  if (!fresh(entry, value)) return;
  const clear = clearBytes(kind, frame, data.length - TRAILER - TAG);
  const nonce = new Uint8Array(12);
  nonce.set(data.subarray(data.length - TRAILER, data.length - 1), 4);
  let plain;
  try {
    plain = new Uint8Array(await crypto.subtle.decrypt(
      { name: "AES-GCM", iv: nonce, additionalData: data.subarray(0, clear), tagLength: 128 },
      entry.key,
      data.subarray(clear, data.length - TRAILER),
    ));
  } catch (_) {
    return;
  }
  if (peerKeys.get(peer)?.get(keyId) !== entry || !fresh(entry, value)) return;
  mark(entry, value);
  const out = new Uint8Array(clear + plain.length);
  out.set(data.subarray(0, clear), 0);
  out.set(plain, clear);
  frame.data = out.buffer;
  controller.enqueue(frame);
}

function pipe(readable, writable, options) {
  const transform = new TransformStream({
    transform(frame, controller) {
      if (options.direction === "encrypt") return encrypt(options.kind, frame, controller);
      return decrypt(options.kind, options.peer, frame, controller);
    },
  });
  readable.pipeThrough(transform).pipeTo(writable).catch(() => {});
}

let pending = Promise.resolve();

async function handle(message) {
  if (message.op === "send-key") {
    const raw = new Uint8Array(message.raw.slice(0));
    const keyId = message.keyId & 0xff;
    if (sendKey && sameBytes(raw, sendRaw)) {
      sendKeyId = keyId;
      return;
    }
    const key = await importKey(raw);
    sendKey = key;
    sendRaw = raw;
    sendKeyId = keyId;
    counter = 0n;
  } else if (message.op === "key") {
    const raw = new Uint8Array(message.raw.slice(0));
    const keyId = message.keyId & 0xff;
    const known = peerKeys.get(message.peer)?.get(keyId);
    if (known && sameBytes(raw, known.raw)) return;
    const key = await importKey(raw);
    const keys = peerKeys.get(message.peer) || new Map();
    keys.delete(keyId);
    keys.set(keyId, { key, raw, started: false, top: 0n, seen: 0n });
    while (keys.size > KEEP_KEYS) keys.delete(keys.keys().next().value);
    peerKeys.set(message.peer, keys);
  } else if (message.op === "forget") {
    peerKeys.delete(message.peer);
  } else if (message.op === "stream") {
    pipe(message.readable, message.writable, message);
  }
}

self.onmessage = (event) => {
  const message = event.data || {};
  pending = pending.then(() => handle(message)).catch(() => {});
  return pending;
};

self.onrtctransform = (event) => {
  const transformer = event.transformer;
  pipe(transformer.readable, transformer.writable, transformer.options || {});
};

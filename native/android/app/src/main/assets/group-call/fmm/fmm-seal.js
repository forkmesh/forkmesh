import { createMediaSigner, createMediaVerifier } from "../chat-call-e2ee.js";

export const FRAME_TRAILER = 9;
export const FRAME_TAG = 16;
export const KEEP_KEYS = 3;
export const CLEAR_AUDIO = 1;
export const CLEAR_VIDEO = 0;
const WINDOW = 1024n;
const MASK = (1n << WINDOW) - 1n;

function sameBytes(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i += 1) diff |= a[i] ^ b[i];
  return diff === 0;
}

function nonce(counter) {
  const bytes = new Uint8Array(12);
  new DataView(bytes.buffer).setBigUint64(4, counter);
  return bytes;
}

function importKey(subtle, raw) {
  return subtle.importKey("raw", raw, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
}

function serial() {
  let queue = Promise.resolve();
  return (task) => {
    const run = queue.then(task);
    queue = run.catch(() => {});
    return run;
  };
}

export function readTrailer(sealed) {
  if (!(sealed instanceof Uint8Array) || sealed.length < FRAME_TRAILER + FRAME_TAG + 1) return null;
  const view = new DataView(sealed.buffer, sealed.byteOffset, sealed.byteLength);
  return { counter: view.getBigUint64(sealed.length - FRAME_TRAILER), keyId: sealed[sealed.length - 1] };
}

export function createFrameSealer({ subtle = globalThis.crypto.subtle } = {}) {
  const exclusive = serial();
  let key = null;
  let raw = null;
  let keyId = 0;
  let counter = 0n;
  let signer = null;
  const pending = [];

  async function retire() {
    if (signer) pending.push(...await signer.flush());
  }

  return {
    get ready() { return key !== null; },
    get counter() { return counter; },
    setKey(frameKey) {
      return exclusive(async () => {
        if (key && sameBytes(raw, frameKey.raw)) return;
        if (!(frameKey.raw instanceof Uint8Array) || frameKey.raw.length !== 32) throw new TypeError("A 32-byte frame key is required.");
        const nextSigner = await createMediaSigner(frameKey);
        const next = await importKey(subtle, frameKey.raw);
        await retire();
        key = next;
        raw = frameKey.raw.slice();
        keyId = frameKey.keyId & 0xff;
        counter = 0n;
        signer = nextSigner;
      });
    },
    seal(frame, clear) {
      return exclusive(async () => {
        if (!key) return null;
        const data = frame instanceof Uint8Array ? frame : new Uint8Array(frame);
        const head = Math.max(0, Math.min(clear, data.length));
        counter += 1n;
        const iv = nonce(counter);
        const body = new Uint8Array(await subtle.encrypt(
          { name: "AES-GCM", iv, additionalData: data.subarray(0, head), tagLength: 128 },
          key,
          data.subarray(head),
        ));
        const out = new Uint8Array(head + body.length + FRAME_TRAILER);
        out.set(data.subarray(0, head), 0);
        out.set(body, head);
        out.set(iv.subarray(4), head + body.length);
        out[out.length - 1] = keyId;
        await signer.add(out);
        return out;
      });
    },
    flush() {
      return exclusive(async () => {
        await retire();
        return pending.splice(0);
      });
    },
  };
}

function fresh(entry, value) {
  if (!entry.started || value > entry.top) return true;
  const age = entry.top - value;
  return age < WINDOW && ((entry.seen >> age) & 1n) === 0n;
}

function mark(entry, value) {
  if (!entry.started) {
    entry.started = true;
    entry.top = value;
    entry.seen = 1n;
  } else if (value > entry.top) {
    const shift = value - entry.top;
    entry.seen = shift >= WINDOW ? 1n : ((entry.seen << shift) | 1n) & MASK;
    entry.top = value;
  } else {
    entry.seen |= 1n << (entry.top - value);
  }
}

export function createFrameOpener({ subtle = globalThis.crypto.subtle, now = () => Date.now(), onState = null } = {}) {
  const peers = new Map();
  const verifier = createMediaVerifier({ now, onState });
  const exclusive = serial();

  return {
    setKey(frameKey) {
      return exclusive(async () => {
        if (!(frameKey.raw instanceof Uint8Array) || frameKey.raw.length !== 32) throw new TypeError("A 32-byte frame key is required.");
        const keyId = frameKey.keyId & 0xff;
        const keys = peers.get(frameKey.peer) || new Map();
        const known = keys.get(keyId);
        if (known && sameBytes(known.raw, frameKey.raw)) return;
        verifier.setKey(frameKey);
        const cryptoKey = await importKey(subtle, frameKey.raw);
        keys.delete(keyId);
        keys.set(keyId, { key: cryptoKey, raw: frameKey.raw.slice(), started: false, top: 0n, seen: 0n });
        while (keys.size > KEEP_KEYS) keys.delete(keys.keys().next().value);
        peers.set(frameKey.peer, keys);
      });
    },
    forget(peer) {
      peers.delete(peer);
      verifier.forget(peer);
    },
    has(peer) {
      return Boolean(peers.get(peer)?.size);
    },
    async open(peer, sealed, clear) {
      const trailer = readTrailer(sealed);
      if (!trailer) return { plain: null, drop: "invalid" };
      const entry = peers.get(peer)?.get(trailer.keyId);
      if (!entry) return { plain: null, drop: "no-key" };
      if (!fresh(entry, trailer.counter)) return { plain: null, drop: "replay" };
      const body = sealed.length - FRAME_TRAILER - FRAME_TAG;
      const head = Math.max(0, Math.min(clear, body));
      let plain;
      try {
        plain = new Uint8Array(await subtle.decrypt(
          { name: "AES-GCM", iv: nonce(trailer.counter), additionalData: sealed.subarray(0, head), tagLength: 128 },
          entry.key,
          sealed.subarray(head, sealed.length - FRAME_TRAILER),
        ));
      } catch (_) {
        return { plain: null, drop: "tag" };
      }
      if (peers.get(peer)?.get(trailer.keyId) !== entry || !fresh(entry, trailer.counter)) return { plain: null, drop: "replay" };
      mark(entry, trailer.counter);
      let drop = "";
      try {
        drop = await verifier.frame(peer, sealed);
      } catch (error) {
        drop = error?.code || "unknown-peer";
      }
      if (drop) return { plain: null, drop };
      const out = new Uint8Array(head + plain.length);
      out.set(sealed.subarray(0, head), 0);
      out.set(plain, head);
      return { plain: out, drop: "" };
    },
    async accept(peer, bytes) {
      try {
        await verifier.accept(peer, bytes);
        return "";
      } catch (error) {
        return error?.code || "invalid";
      }
    },
    status(peer) {
      return verifier.status(peer);
    },
  };
}

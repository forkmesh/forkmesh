import { mlkemDecapsulate, mlkemEncapsulate, mlkemKeyPair } from "./chat-mlkem.js";

export const CALL_E2EE_VERSION = 1;
export const MAX_PARTICIPANTS = 16;
export const MAX_RECIPIENTS = 31;
export const CLOCK_SKEW_MS = 5 * 60 * 1000;
export const PRESENCE_TIMEOUT_MS = 60 * 1000;
export const PRESENCE_GRACE_MS = 30 * 1000;
export const KEEP_PRESENCE_KEYS = 2;
export const SIGNING_PREFIX = "forkmesh-call-e2ee-";

const SECRET_BYTES = 32;
const FRAME_KEY_BYTES = 32;
const X_BYTES = 32;
const PQ_BYTES = 1184;
const CT_BYTES = 1088;
const WRAPPED_BYTES = SECRET_BYTES + 16;
const SIG_BYTES = 64;
const IDK_BYTES = 32;
const MLKEM_SEED = 64;
const MAX_CALL = 256;
const MAX_ACCT = 128;
const MAX_EPOCH = 0xffffffff;
const CODE_ITERATIONS = 5200;
const CODE_GROUPS = 6;

const PRESENCE_LABEL = "forkmesh-call-e2ee-presence-v1";
const KEYS_LABEL = "forkmesh-call-e2ee-keys-v1";
const WRAP_LABEL = "forkmesh-call-e2ee-wrap-v1";
const FRAME_LABEL = "forkmesh-call-e2ee-frame-v1";
const SALT_LABEL = "forkmesh-call-e2ee-salt-v1";
const CODE_LABEL = "forkmesh-call-e2ee-code-v1";

const X25519_PKCS8 = Uint8Array.from([
  0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x6e, 0x04, 0x22, 0x04, 0x20,
]);
const ED25519_PKCS8 = Uint8Array.from([
  0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
]);
const PEER_RE = /^[A-Za-z0-9_-]{1,64}$/;
const B64U_RE = /^[A-Za-z0-9_-]*$/;

export class CallE2eeError extends Error {
  constructor(code) {
    super("call e2ee: " + code);
    this.name = "CallE2eeError";
    this.code = code;
  }
}

function fail(code) {
  throw new CallE2eeError(code);
}

export function errorCode(error) {
  if (!error) return "";
  return error instanceof CallE2eeError ? error.code : "invalid";
}

const subtle = () => globalThis.crypto.subtle;
const encoder = new TextEncoder();
const utf8 = (text) => encoder.encode(text);

function randomBytes(count) {
  return globalThis.crypto.getRandomValues(new Uint8Array(count));
}

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

function wipe(...arrays) {
  for (const array of arrays) if (array) array.fill(0);
}

function sameBytes(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i += 1) diff |= a[i] ^ b[i];
  return diff === 0;
}

function compareBytes(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i += 1) if (a[i] !== b[i]) return a[i] - b[i];
  return a.length - b.length;
}

export function b64uEncode(bytes) {
  let text = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(text).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function b64uDecode(text, size = -1) {
  if (typeof text !== "string" || !B64U_RE.test(text) || text.length % 4 === 1) fail("invalid");
  if (size >= 0 && text.length !== Math.ceil((size * 4) / 3)) fail("invalid");
  const padded = text.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((text.length + 3) % 4);
  let binary;
  try {
    binary = atob(padded);
  } catch (_) {
    fail("invalid");
  }
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
  if ((size >= 0 && out.length !== size) || b64uEncode(out) !== text) fail("invalid");
  return out;
}

async function sha256(bytes) {
  return new Uint8Array(await subtle().digest("SHA-256", bytes));
}

async function digest(bytes) {
  return b64uEncode(await sha256(bytes));
}

export async function keyId(idk) {
  return (await digest(idk)).slice(0, 16);
}

function validCall(call) {
  if (typeof call !== "string" || call.length < 1 || call.length > MAX_CALL) return false;
  for (let i = 0; i < call.length; i += 1) {
    const c = call.charCodeAt(i);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

function validPeer(peer) {
  return typeof peer === "string" && PEER_RE.test(peer);
}

function validAcct(acct) {
  if (typeof acct !== "string") return false;
  let count = 0;
  for (const ch of acct) {
    const cp = ch.codePointAt(0);
    if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp === 0xfffd || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    count += 1;
  }
  return count >= 1 && count <= MAX_ACCT;
}

function asObject(input) {
  let value = input;
  if (typeof value === "string") {
    try {
      value = JSON.parse(value);
    } catch (_) {
      fail("invalid");
    }
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) fail("invalid");
  return value;
}

function own(object, key) {
  return Object.prototype.hasOwnProperty.call(object, key) ? object[key] : undefined;
}

function str(object, key) {
  const value = own(object, key);
  if (typeof value !== "string") fail("invalid");
  return value;
}

function integer(object, key, lo, hi) {
  const value = own(object, key);
  if (!Number.isSafeInteger(value) || value < lo || value > hi) fail("invalid");
  return value === 0 ? 0 : value;
}

function bytesField(object, key, size) {
  return b64uDecode(str(object, key), size);
}

export async function parsePresence(input) {
  const o = asObject(input);
  if (integer(o, "v", 0, Number.MAX_SAFE_INTEGER) !== CALL_E2EE_VERSION) fail("invalid");
  const call = str(o, "call");
  if (!validCall(call)) fail("invalid");
  const peer = str(o, "peer");
  if (!validPeer(peer)) fail("invalid");
  const acct = str(o, "acct");
  if (!validAcct(acct)) fail("invalid");
  const kid = str(o, "kid");
  const idk = bytesField(o, "idk", IDK_BYTES);
  if (kid !== await keyId(idk)) fail("invalid");
  const epoch = integer(o, "epoch", 1, MAX_EPOCH);
  const ts = integer(o, "ts", 1, Number.MAX_SAFE_INTEGER);
  const x = bytesField(o, "x", X_BYTES);
  const pq = bytesField(o, "pq", PQ_BYTES);
  const sig = bytesField(o, "sig", SIG_BYTES);
  return { call, peer, acct, kid, idk, epoch, ts, x, pq, sig };
}

export function marshalPresence(p) {
  return {
    v: CALL_E2EE_VERSION, call: p.call, peer: p.peer, acct: p.acct, kid: p.kid, idk: b64uEncode(p.idk),
    epoch: p.epoch, ts: p.ts, x: b64uEncode(p.x), pq: b64uEncode(p.pq), sig: b64uEncode(p.sig),
  };
}

export async function presenceMessage(p) {
  return utf8([
    PRESENCE_LABEL, p.call, p.peer, p.acct, await keyId(p.idk), b64uEncode(p.idk),
    String(p.epoch), String(p.ts), b64uEncode(p.x), await digest(p.pq),
  ].join("\n"));
}

export async function parseKeyMessage(input) {
  const o = asObject(input);
  if (integer(o, "v", 0, Number.MAX_SAFE_INTEGER) !== CALL_E2EE_VERSION) fail("invalid");
  const call = str(o, "call");
  if (!validCall(call)) fail("invalid");
  const peer = str(o, "peer");
  if (!validPeer(peer)) fail("invalid");
  const kid = str(o, "kid");
  const idk = bytesField(o, "idk", IDK_BYTES);
  if (kid !== await keyId(idk)) fail("invalid");
  const epoch = integer(o, "epoch", 1, MAX_EPOCH);
  const ts = integer(o, "ts", 1, Number.MAX_SAFE_INTEGER);
  const eph = bytesField(o, "eph", X_BYTES);
  const sig = bytesField(o, "sig", SIG_BYTES);
  const list = own(o, "keys");
  if (!Array.isArray(list) || list.length < 1 || list.length > MAX_RECIPIENTS) fail("invalid");
  const keys = [];
  for (const item of list) {
    if (!item || typeof item !== "object" || Array.isArray(item)) fail("invalid");
    const to = str(item, "peer");
    if (!validPeer(to) || to === peer) fail("invalid");
    if (keys.length && !(to > keys[keys.length - 1].peer)) fail("invalid");
    keys.push({
      peer: to,
      epoch: integer(item, "epoch", 1, MAX_EPOCH),
      ct: bytesField(item, "ct", CT_BYTES),
      key: bytesField(item, "key", WRAPPED_BYTES),
    });
  }
  return { call, peer, kid, idk, epoch, ts, eph, keys, sig };
}

export function marshalKeyMessage(m) {
  return {
    v: CALL_E2EE_VERSION, call: m.call, peer: m.peer, kid: m.kid, idk: b64uEncode(m.idk), epoch: m.epoch, ts: m.ts,
    eph: b64uEncode(m.eph),
    keys: m.keys.map((k) => ({ peer: k.peer, epoch: k.epoch, ct: b64uEncode(k.ct), key: b64uEncode(k.key) })),
    sig: b64uEncode(m.sig),
  };
}

export async function keyMessageBytes(m) {
  let text = [
    KEYS_LABEL, m.call, m.peer, await keyId(m.idk), b64uEncode(m.idk), String(m.epoch), String(m.ts), b64uEncode(m.eph),
  ].join("\n");
  for (const k of m.keys) {
    text += "\n" + k.peer + "|" + String(k.epoch) + "|" + await digest(k.ct) + "|" + b64uEncode(k.key);
  }
  return utf8(text);
}

const verifyKeys = new Map();

async function edVerify(idk, signature, message) {
  try {
    const cacheKey = b64uEncode(idk);
    let key = verifyKeys.get(cacheKey);
    if (!key) {
      key = await subtle().importKey("raw", idk, { name: "Ed25519" }, false, ["verify"]);
      verifyKeys.set(cacheKey, key);
      while (verifyKeys.size > 256) verifyKeys.delete(verifyKeys.keys().next().value);
    }
    return await subtle().verify({ name: "Ed25519" }, key, signature, message);
  } catch (_) {
    return false;
  }
}

export async function verifyPresence(p) {
  if (!await edVerify(p.idk, p.sig, await presenceMessage(p))) fail("signature");
}

export async function verifyKeyMessage(m) {
  if (!await edVerify(m.idk, m.sig, await keyMessageBytes(m))) fail("signature");
}

export async function signerFromSeed(seed) {
  if (!(seed instanceof Uint8Array) || seed.length !== 32) fail("invalid");
  const pkcs8 = concat(ED25519_PKCS8, seed);
  const exportable = await subtle().importKey("pkcs8", pkcs8, { name: "Ed25519" }, true, ["sign"]);
  wipe(pkcs8);
  const jwk = await subtle().exportKey("jwk", exportable);
  const privateKey = await subtle().importKey(
    "jwk", { kty: "OKP", crv: "Ed25519", d: jwk.d, x: jwk.x }, { name: "Ed25519" }, false, ["sign"]);
  jwk.d = "";
  const idk = b64uDecode(jwk.x, IDK_BYTES);
  return {
    idk,
    async sign(message) {
      if (!(message instanceof Uint8Array) || !new TextDecoder().decode(message.subarray(0, SIGNING_PREFIX.length)).startsWith(SIGNING_PREFIX)) {
        fail("invalid");
      }
      return new Uint8Array(await subtle().sign({ name: "Ed25519" }, privateKey, message));
    },
  };
}

export async function x25519FromPrivate(raw) {
  if (!(raw instanceof Uint8Array) || raw.length !== X_BYTES) fail("invalid");
  const pkcs8 = concat(X25519_PKCS8, raw);
  const exportable = await subtle().importKey("pkcs8", pkcs8, { name: "X25519" }, true, ["deriveBits"]);
  wipe(pkcs8);
  const jwk = await subtle().exportKey("jwk", exportable);
  const privateKey = await subtle().importKey(
    "jwk", { kty: "OKP", crv: "X25519", d: jwk.d, x: jwk.x }, { name: "X25519" }, false, ["deriveBits"]);
  jwk.d = "";
  return { privateKey, publicBytes: b64uDecode(jwk.x, X_BYTES) };
}

async function x25519(privateKey, publicBytes) {
  let shared;
  try {
    const publicKey = await subtle().importKey("raw", publicBytes, { name: "X25519" }, false, []);
    shared = new Uint8Array(await subtle().deriveBits({ name: "X25519", public: publicKey }, privateKey, 256));
  } catch (_) {
    fail("invalid");
  }
  if (!shared.some((byte) => byte !== 0)) fail("invalid");
  return shared;
}

async function callSalt(call) {
  return sha256(utf8(SALT_LABEL + "\n" + call));
}

export async function wrapInfo(call, sender, recipient, eph, ct) {
  return [
    WRAP_LABEL, call,
    sender.peer, b64uEncode(sender.idk), String(sender.epoch),
    recipient.peer, b64uEncode(recipient.idk), String(recipient.epoch),
    b64uEncode(recipient.x), await digest(recipient.pq), b64uEncode(eph), await digest(ct),
  ].join("\n");
}

async function hkdf(ikm, salt, info, length) {
  const material = await subtle().importKey("raw", ikm, "HKDF", false, ["deriveBits"]);
  return new Uint8Array(await subtle().deriveBits(
    { name: "HKDF", hash: "SHA-256", salt, info: utf8(info) }, material, length * 8));
}

export async function kekFrom(call, info, ss1, ss2) {
  const ikm = concat(ss1, ss2);
  const okm = await hkdf(ikm, await callSalt(call), info, 44);
  wipe(ikm);
  return { kek: okm.slice(0, 32), nonce: okm.slice(32, 44) };
}

async function aesKey(raw, usages) {
  return subtle().importKey("raw", raw, { name: "AES-GCM" }, false, usages);
}

export async function wrapFrameSecret({ call, sender, recipient, eph, secret, mlkemMessage }) {
  const ss1 = await x25519(eph.privateKey, recipient.x);
  let encapsulated;
  try {
    encapsulated = mlkemMessage ? mlkemEncapsulate(recipient.pq, mlkemMessage) : mlkemEncapsulate(recipient.pq);
  } catch (_) {
    wipe(ss1);
    fail("invalid");
  }
  const ct = encapsulated.ciphertext;
  const ss2 = encapsulated.sharedSecret;
  const info = await wrapInfo(call, sender, recipient, eph.publicBytes, ct);
  const { kek, nonce } = await kekFrom(call, info, ss1, ss2);
  const key = new Uint8Array(await subtle().encrypt(
    { name: "AES-GCM", iv: nonce, additionalData: utf8(info), tagLength: 128 },
    await aesKey(kek, ["encrypt"]),
    secret,
  ));
  return { entry: { peer: recipient.peer, epoch: recipient.epoch, ct, key }, trace: { ss1, ss2, kek, nonce, info } };
}

export async function unwrapFrameSecret({ call, sender, eph, entry, recipient, xPrivate, pqSecret }) {
  if (entry.peer !== recipient.peer || entry.epoch !== recipient.epoch) fail("invalid");
  const ss1 = await x25519(xPrivate, eph);
  let ss2;
  try {
    ss2 = mlkemDecapsulate(pqSecret, entry.ct);
  } catch (_) {
    wipe(ss1);
    fail("invalid");
  }
  const info = await wrapInfo(call, sender, recipient, eph, entry.ct);
  const { kek, nonce } = await kekFrom(call, info, ss1, ss2);
  wipe(ss1, ss2);
  let secret;
  try {
    secret = new Uint8Array(await subtle().decrypt(
      { name: "AES-GCM", iv: nonce, additionalData: utf8(info), tagLength: 128 },
      await aesKey(kek, ["decrypt"]),
      entry.key,
    ));
  } catch (_) {
    fail("invalid");
  } finally {
    wipe(kek);
  }
  if (secret.length !== SECRET_BYTES) fail("invalid");
  return secret;
}

export async function deriveFrameKey(call, sender, secret) {
  if (!(secret instanceof Uint8Array) || secret.length !== SECRET_BYTES) fail("invalid");
  const info = [FRAME_LABEL, call, sender.peer, b64uEncode(sender.idk), String(sender.epoch)].join("\n");
  const raw = await hkdf(secret, await callSalt(call), info, FRAME_KEY_BYTES);
  return { peer: sender.peer, epoch: sender.epoch, keyId: sender.epoch & 0xff, raw };
}

export async function verificationCode(call, members) {
  if (!validCall(call) || !Array.isArray(members) || members.length === 0) fail("invalid");
  const list = members.map((member) => {
    if (!(member.idk instanceof Uint8Array) || member.idk.length !== IDK_BYTES || !validAcct(member.acct)) fail("invalid");
    return { idk: member.idk, acct: utf8(member.acct) };
  });
  list.sort((a, b) => compareBytes(a.idk, b.idk) || compareBytes(a.acct, b.acct));
  const parts = [];
  list.forEach((member, i) => {
    if (i > 0 && sameBytes(member.idk, list[i - 1].idk) && sameBytes(member.acct, list[i - 1].acct)) return;
    parts.push(member.idk, Uint8Array.of(member.acct.length >> 8, member.acct.length & 0xff), member.acct);
  });
  const entries = concat(...parts);
  let h = new Uint8Array(await subtle().digest("SHA-512", concat(utf8(CODE_LABEL + "\n" + call + "\n"), entries)));
  const buffer = new Uint8Array(64 + entries.length);
  buffer.set(entries, 64);
  for (let i = 1; i < CODE_ITERATIONS; i += 1) {
    buffer.set(h, 0);
    h = new Uint8Array(await subtle().digest("SHA-512", buffer));
  }
  const groups = [];
  for (let g = 0; g < CODE_GROUPS; g += 1) {
    let value = 0;
    for (let i = 0; i < 5; i += 1) value = value * 256 + h[g * 5 + i];
    groups.push(String(value % 100000).padStart(5, "0"));
  }
  return groups.join(" ");
}

export async function createCallSession({
  call,
  peer,
  acct,
  signer,
  now = () => Date.now(),
  random = randomBytes,
} = {}) {
  if (!validCall(call) || !validPeer(peer) || !validAcct(acct) || !signer ||
      !(signer.idk instanceof Uint8Array) || signer.idk.length !== IDK_BYTES || typeof signer.sign !== "function") {
    fail("invalid");
  }
  const idk = signer.idk.slice();
  const kid = await keyId(idk);
  const keys = [];
  const members = new Map();
  let sendEpoch = 0;
  let closed = false;
  let queue = Promise.resolve();

  function exclusive(task) {
    const run = queue.then(task);
    queue = run.catch(() => {});
    return run;
  }

  function ensureOpen() {
    if (closed) fail("closed");
  }

  function draw(count) {
    const bytes = random(count);
    if (!(bytes instanceof Uint8Array) || bytes.length !== count) fail("invalid");
    return bytes;
  }

  async function newPresenceKey(epoch) {
    const xs = draw(X_BYTES);
    const seed = draw(MLKEM_SEED);
    const x = await x25519FromPrivate(xs);
    const pq = mlkemKeyPair(seed);
    wipe(xs, seed);
    return { epoch, xPrivate: x.privateKey, xPublic: x.publicBytes, pqPublic: pq.publicKey, pqSecret: pq.secretKey, public: null, retired: 0 };
  }

  async function signPresence(key) {
    const p = { call, peer, acct, kid, idk, epoch: key.epoch, ts: now(), x: key.xPublic, pq: key.pqPublic, sig: null };
    p.sig = await signer.sign(await presenceMessage(p));
    key.public = p;
    return marshalPresence(p);
  }

  async function rotateLocked() {
    ensureOpen();
    let epoch = 1;
    if (keys.length) {
      if (keys[keys.length - 1].epoch === MAX_EPOCH) fail("exhausted");
      epoch = keys[keys.length - 1].epoch + 1;
    }
    const key = await newPresenceKey(epoch);
    if (keys.length) keys[keys.length - 1].retired = now();
    keys.push(key);
    while (keys.length > KEEP_PRESENCE_KEYS) wipe(keys.shift().pqSecret);
    return signPresence(key);
  }

  function fresh(ts) {
    const t = now();
    return ts >= t - CLOCK_SKEW_MS && ts <= t + CLOCK_SKEW_MS;
  }

  function sortedPeers() {
    return [...members.keys()].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  }

  await exclusive(rotateLocked);

  return {
    get call() { return call; },
    get peer() { return peer; },
    get idk() { return idk.slice(); },

    presence() {
      return exclusive(() => {
        ensureOpen();
        return signPresence(keys[keys.length - 1]);
      });
    },

    rotate() {
      return exclusive(rotateLocked);
    },

    presenceEpoch() {
      return keys.length ? keys[keys.length - 1].epoch : 0;
    },

    acceptPresence(input) {
      return exclusive(async () => {
        const p = await parsePresence(input);
        ensureOpen();
        if (p.call !== call) fail("call");
        if (p.peer === peer) fail(sameBytes(p.idk, idk) ? "own" : "peer-bound");
        if (!fresh(p.ts)) fail("clock");
        const m = members.get(p.peer);
        if (m && !sameBytes(m.presence.idk, p.idk)) fail("peer-bound");
        if (!m && members.size + 1 >= MAX_PARTICIPANTS) fail("full");
        await verifyPresence(p);
        if (!m) {
          members.set(p.peer, { presence: p, heard: now(), opened: 0 });
          return { kind: "joined", peer: p.peer };
        }
        const old = m.presence;
        if (p.acct !== old.acct) fail("peer-bound");
        if (p.epoch < old.epoch) fail("stale");
        if (p.epoch === old.epoch) {
          if (!sameBytes(p.x, old.x) || !sameBytes(p.pq, old.pq)) fail("equivocation");
          if (p.ts < old.ts) fail("stale");
          m.presence = p;
          m.heard = now();
          return { kind: "same", peer: p.peer };
        }
        m.presence = p;
        m.heard = now();
        return { kind: "updated", peer: p.peer };
      });
    },

    leave(who) {
      return members.delete(who);
    },

    expire() {
      const t = now();
      const gone = [];
      for (const [who, m] of members) {
        if (t - m.heard > PRESENCE_TIMEOUT_MS) {
          gone.push(who);
          members.delete(who);
        }
      }
      return gone.sort();
    },

    prune() {
      const t = now();
      for (let i = keys.length - 1; i >= 0; i -= 1) {
        if (keys[i].retired && t - keys[i].retired > PRESENCE_GRACE_MS) wipe(keys.splice(i, 1)[0].pqSecret);
      }
    },

    peers() {
      return sortedPeers();
    },

    member(who) {
      const m = members.get(who);
      return m ? { ...m.presence } : null;
    },

    rekey() {
      return exclusive(async () => {
        ensureOpen();
        if (sendEpoch === MAX_EPOCH) fail("exhausted");
        const epoch = sendEpoch + 1;
        const secret = draw(SECRET_BYTES);
        const sender = { peer, idk, epoch };
        let message = null;
        try {
          const recipients = sortedPeers().map((who) => members.get(who).presence);
          if (recipients.length) {
            if (recipients.length > MAX_RECIPIENTS) fail("full");
            const xs = draw(X_BYTES);
            const eph = await x25519FromPrivate(xs);
            wipe(xs);
            const m = { call, peer, kid, idk, epoch, ts: now(), eph: eph.publicBytes, keys: [], sig: null };
            for (const recipient of recipients) {
              const { entry, trace } = await wrapFrameSecret({ call, sender, recipient, eph, secret });
              wipe(trace.ss1, trace.ss2, trace.kek);
              m.keys.push(entry);
            }
            m.sig = await signer.sign(await keyMessageBytes(m));
            message = marshalKeyMessage(m);
          }
          const key = await deriveFrameKey(call, sender, secret);
          sendEpoch = epoch;
          return { message, key };
        } finally {
          wipe(secret);
        }
      });
    },

    openKeys(input) {
      return exclusive(async () => {
        const m = await parseKeyMessage(input);
        ensureOpen();
        if (m.call !== call) fail("call");
        if (m.peer === peer) fail("own");
        const member = members.get(m.peer);
        if (!member) fail("unknown-peer");
        if (!sameBytes(member.presence.idk, m.idk)) fail("peer-bound");
        if (!fresh(m.ts)) fail("clock");
        await verifyKeyMessage(m);
        if (m.epoch < member.opened) fail("stale");
        if (m.epoch === member.opened) fail("duplicate");
        const entry = m.keys.find((k) => k.peer === peer);
        if (!entry) fail("not-for-me");
        const key = keys.find((candidate) => candidate.epoch === entry.epoch);
        if (!key) fail("recipient-epoch");
        const sender = { peer: m.peer, idk: m.idk, epoch: m.epoch };
        const secret = await unwrapFrameSecret({
          call, sender, eph: m.eph, entry, recipient: key.public, xPrivate: key.xPrivate, pqSecret: key.pqSecret,
        });
        try {
          const frameKey = await deriveFrameKey(call, sender, secret);
          member.opened = m.epoch;
          return frameKey;
        } finally {
          wipe(secret);
        }
      });
    },

    code() {
      const list = [{ idk, acct }];
      for (const m of members.values()) list.push({ idk: m.presence.idk, acct: m.presence.acct });
      return verificationCode(call, list);
    },

    close() {
      closed = true;
      for (const key of keys.splice(0)) wipe(key.pqSecret);
      members.clear();
    },
  };
}

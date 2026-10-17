import {
  b64ToBytes,
  b64UrlToBytes,
  bytesToB64,
  bytesToB64Url,
} from "./chat-crypto.js";
import {
  MLKEM_CIPHERTEXT_BYTES,
  MLKEM_PUBLIC_KEY_BYTES,
  mlkemDecapsulate,
  mlkemEncapsulate,
  mlkemKeyPair,
} from "./chat-mlkem.js";

const SEAL_TYPE = "sealed";
const KEY_TYPE = "e2ee-key";
const SEAL_VERSION = 3;
const SEAL_V4 = 4;
const MAX_SKIP = 256;
const SKIPPED_KEPT = 512;
const SESSIONS_KEPT = 4;
const OPENED_LIFE_MS = 10 * 60 * 1000;
const OPENED_KEPT = 256;
const OPENED_ENTRY_BYTES = 16384;
const OPENED_TOTAL_BYTES = 262144;
const OPENED_HERE_KEPT = 1024;
const RATCHET_LOCK = "forkmesh-chat-ratchet";
export const E2EE_RESET_TYPE = "e2ee-reset";
const DAY_MS = 24 * 60 * 60 * 1000;
const PREKEY_LIFE_MS = 8 * DAY_MS;
const SKIPPED_LIFE_MS = 8 * DAY_MS;
const PREKEY_ROTATE_MS = DAY_MS;
const PREKEYS_KEPT = 10;
const MAX_RECIPIENTS = 64;
const REPLAY_WINDOW = 1024;
const HELD_PER_ROOM = 64;
const PENDING_PER_SCOPE = 16;
const OPEN_RECORDS_PER_SCOPE = 128;
const SAFETY_ITERATIONS = 5199;
const ANNOUNCE_DURABLE_MS = DAY_MS;
const STORE_PREFIX = "forkmesh.chat/e2ee/";
const IDENTITY_KEY = STORE_PREFIX + "ed25519IdentityV3";
const PREKEYS_KEY = STORE_PREFIX + "prekeysV3";
const LEGACY_KEYS = [
  "forkmesh.chat.e2ee.x25519.v2",
];
const LEGACY_PREFIXES = [
  "forkmesh.chat.e2ee.peers.v2.",
  "forkmesh.chat.e2ee.announced.v2.",
  "forkmesh.chat.seal.",
];
const WRAP_DB = "forkmesh-chat-e2ee";
const WRAP_STORE = "keys";
const WRAP_ID = "storage-wrap-v3";

const KID_RE = /^[A-Za-z0-9_-]{16}$/;
const KEY32_RE = /^[A-Za-z0-9_-]{43}$/;
const SIG_RE = /^[A-Za-z0-9_-]{86}$/;
const PQ_RE = /^[A-Za-z0-9_-]{1579}$/;
const B64_RE = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;
const ACCT_RE = /^(?:[^\p{C}\p{Z}]| )+$/u;

const ED25519_PKCS8 = Uint8Array.from([
  0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
]);
const X25519_PKCS8 = Uint8Array.from([
  0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x6e, 0x04, 0x22, 0x04, 0x20,
]);

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const subtle = () => globalThis.crypto.subtle;

function utf8(text) {
  return encoder.encode(String(text));
}

function concatBytes(...parts) {
  const total = parts.reduce((sum, part) => sum + part.length, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

function randomBytes(count) {
  return globalThis.crypto.getRandomValues(new Uint8Array(count));
}

export async function sha256(bytes) {
  return new Uint8Array(await subtle().digest("SHA-256", bytes));
}

export async function id16(bytes) {
  return bytesToB64Url(await sha256(bytes)).slice(0, 16);
}

function strictB64(text, length = -1) {
  if (typeof text !== "string" || !B64_RE.test(text)) throw new TypeError("Invalid base64");
  const bytes = b64ToBytes(text);
  if (length >= 0 && bytes.length !== length) throw new TypeError("Invalid length");
  return bytes;
}

function strictB64u(text, pattern) {
  if (typeof text !== "string" || !pattern.test(text)) throw new TypeError("Invalid base64url");
  return b64UrlToBytes(text);
}

function safeJson(value) {
  try {
    return JSON.parse(String(value ?? ""));
  } catch (_) {
    return null;
  }
}

function browserStorage() {
  try {
    return globalThis.localStorage || null;
  } catch (_) {
    return null;
  }
}

function storageKey(kind, name) {
  return STORE_PREFIX + kind + "/" + bytesToB64Url(utf8(name));
}

function readItem(storage, key) {
  try {
    return storage?.getItem(key) ?? null;
  } catch (_) {
    return null;
  }
}

function writeItem(storage, key, value) {
  try {
    storage?.setItem(key, value);
    return true;
  } catch (_) {
    return false;
  }
}

function removeItem(storage, key) {
  try {
    storage?.removeItem(key);
  } catch (_) {}
}

function forgetLegacy(storage) {
  for (const key of LEGACY_KEYS) removeItem(storage, key);
  try {
    const length = Number(storage?.length) || 0;
    const doomed = [];
    for (let index = 0; index < length; index += 1) {
      const key = storage.key(index);
      if (key && LEGACY_PREFIXES.some((prefix) => key.startsWith(prefix))) doomed.push(key);
    }
    for (const key of doomed) removeItem(storage, key);
  } catch (_) {}
}

export async function browserWrappingKey() {
  const idb = globalThis.indexedDB;
  if (!idb || !globalThis.crypto?.subtle) return null;
  const request = (make) => new Promise((resolve, reject) => {
    const req = make();
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
  try {
    const db = await new Promise((resolve, reject) => {
      const open = idb.open(WRAP_DB, 1);
      open.onupgradeneeded = () => open.result.createObjectStore(WRAP_STORE);
      open.onsuccess = () => resolve(open.result);
      open.onerror = () => reject(open.error);
    });
    const store = (mode) => db.transaction(WRAP_STORE, mode).objectStore(WRAP_STORE);
    let key = await request(() => store("readonly").get(WRAP_ID));
    if (!key) {
      key = await subtle().generateKey({ name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
      await request(() => store("readwrite").put(key, WRAP_ID));
      key = (await request(() => store("readonly").get(WRAP_ID))) || key;
    }
    db.close();
    return key && key.extractable === false ? key : null;
  } catch (_) {
    return null;
  }
}

async function aesSeal(key, plain, aad, nonce = randomBytes(12)) {
  const combined = new Uint8Array(await subtle().encrypt(
    { name: "AES-GCM", iv: nonce, tagLength: 128, additionalData: aad },
    key,
    plain,
  ));
  return {
    nonce,
    body: combined.slice(0, combined.length - 16),
    tag: combined.slice(combined.length - 16),
  };
}

async function aesOpen(key, nonce, body, tag, aad) {
  return new Uint8Array(await subtle().decrypt(
    { name: "AES-GCM", iv: nonce, tagLength: 128, additionalData: aad },
    key,
    concatBytes(body, tag),
  ));
}

async function aesKey(raw, usages) {
  return subtle().importKey("raw", raw, { name: "AES-GCM", length: 256 }, false, usages);
}

async function readSecret(ctx, key) {
  const raw = readItem(ctx.storage, key);
  if (raw === null) return null;
  const parsed = safeJson(raw);
  if (parsed && typeof parsed === "object" && parsed.wrapped === 1) {
    const wrap = await ctx.wrapKey();
    if (!wrap) return null;
    try {
      const clear = await aesOpen(
        wrap,
        strictB64(parsed.nonce, 12),
        strictB64(parsed.body),
        strictB64(parsed.tag, 16),
        utf8("forkmesh-chat-e2ee-store-v3\n" + key),
      );
      return decoder.decode(clear);
    } catch (_) {
      return null;
    }
  }
  return raw;
}

async function writeSecret(ctx, key, value) {
  const wrap = await ctx.wrapKey();
  if (!wrap) return writeItem(ctx.storage, key, value);
  const sealed = await aesSeal(wrap, utf8(value), utf8("forkmesh-chat-e2ee-store-v3\n" + key));
  return writeItem(ctx.storage, key, JSON.stringify({
    wrapped: 1,
    nonce: bytesToB64(sealed.nonce),
    tag: bytesToB64(sealed.tag),
    body: bytesToB64(sealed.body),
  }));
}

async function edKeysFromSeed(seed) {
  const pkcs8 = concatBytes(ED25519_PKCS8, seed);
  const exportable = await subtle().importKey("pkcs8", pkcs8, { name: "Ed25519" }, true, ["sign"]);
  const jwk = await subtle().exportKey("jwk", exportable);
  pkcs8.fill(0);
  const privateKey = await subtle().importKey(
    "jwk",
    { kty: "OKP", crv: "Ed25519", d: jwk.d, x: jwk.x },
    { name: "Ed25519" },
    false,
    ["sign"],
  );
  jwk.d = "";
  return { privateKey, publicBytes: b64UrlToBytes(jwk.x) };
}

async function xKeysFromPrivate(priv) {
  const pkcs8 = concatBytes(X25519_PKCS8, priv);
  const exportable = await subtle().importKey("pkcs8", pkcs8, { name: "X25519" }, true, ["deriveBits"]);
  const jwk = await subtle().exportKey("jwk", exportable);
  pkcs8.fill(0);
  const privateKey = await subtle().importKey(
    "jwk",
    { kty: "OKP", crv: "X25519", d: jwk.d, x: jwk.x },
    { name: "X25519" },
    false,
    ["deriveBits"],
  );
  jwk.d = "";
  return { privateKey, publicBytes: b64UrlToBytes(jwk.x) };
}

const verifyKeys = new Map();

async function edVerify(publicBytes, signature, message) {
  try {
    const cacheKey = bytesToB64Url(publicBytes);
    let key = verifyKeys.get(cacheKey);
    if (!key) {
      key = await subtle().importKey("raw", publicBytes, { name: "Ed25519" }, false, ["verify"]);
      verifyKeys.set(cacheKey, key);
      while (verifyKeys.size > 512) verifyKeys.delete(verifyKeys.keys().next().value);
    }
    return await subtle().verify({ name: "Ed25519" }, key, signature, message);
  } catch (_) {
    return false;
  }
}

async function x25519(privateKey, publicBytes) {
  const publicKey = await subtle().importKey("raw", publicBytes, { name: "X25519" }, true, []);
  const shared = new Uint8Array(await subtle().deriveBits(
    { name: "X25519", public: publicKey },
    privateKey,
    256,
  ));
  if (!shared.some((byte) => byte !== 0)) throw new TypeError("All-zero X25519 shared secret");
  return shared;
}

export async function x25519Shared(priv, publicBytes) {
  const { privateKey } = await xKeysFromPrivate(priv);
  return x25519(privateKey, publicBytes);
}

export async function deriveKek(room, sender, recipient, spkId, ephPub, ct, ss1, ss2) {
  const ikm = concatBytes(ss1, ss2);
  const material = await subtle().importKey("raw", ikm, "HKDF", false, ["deriveBits"]);
  ikm.fill(0);
  const info = [
    "forkmesh-chat-e2ee-kek-v3", room, sender, recipient, spkId,
    bytesToB64Url(ephPub), bytesToB64Url(await sha256(ct)),
  ].join("\n");
  return new Uint8Array(await subtle().deriveBits(
    {
      name: "HKDF",
      hash: "SHA-256",
      salt: await sha256(utf8("ForkMesh chat e2ee salt v3:" + room)),
      info: utf8(info),
    },
    material,
    256,
  ));
}

function contentAad(room, sender, ctr, ts, version = SEAL_VERSION) {
  return utf8(["forkmesh-chat-e2ee-content-v" + version, room, sender, String(ctr), String(ts)].join("\n"));
}

async function hashB64(text) {
  return bytesToB64Url(await sha256(b64ToBytes(text)));
}

async function wrapAad4(room, sender, entry, nonce, tag) {
  return utf8([
    "forkmesh-chat-e2ee-wrap-v4", room, sender, entry.kid, entry.sid, entry.spk, await hashB64(entry.ct),
    entry.dh, String(entry.pn), String(entry.n), nonce, tag,
  ].join("\n"));
}

async function hkdf64(ikm, salt, info) {
  const material = await subtle().importKey("raw", ikm, "HKDF", false, ["deriveBits"]);
  const out = new Uint8Array(await subtle().deriveBits(
    { name: "HKDF", hash: "SHA-256", salt, info },
    material,
    512,
  ));
  return [out.slice(0, 32), out.slice(32)];
}

async function hmacByte(key, byte) {
  const hmacKey = await subtle().importKey("raw", key, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return new Uint8Array(await subtle().sign("HMAC", hmacKey, Uint8Array.of(byte)));
}

export async function kdfInit(room, sender, recipient, spkId, sid, ss1, ss2) {
  return hkdf64(
    concatBytes(ss1, ss2),
    await sha256(utf8("ForkMesh chat e2ee salt v4:" + room)),
    utf8(["forkmesh-chat-e2ee-init-v4", room, sender, recipient, spkId, sid].join("\n")),
  );
}

export async function kdfRk(rk, dhOut, ss) {
  return hkdf64(concatBytes(dhOut, ss), rk, utf8("forkmesh-chat-e2ee-ratchet-v4"));
}

export async function kdfCk(ck) {
  return [await hmacByte(ck, 1), await hmacByte(ck, 2)];
}

export async function sessionId(dhPub, ct) {
  return id16(concatBytes(dhPub, ct));
}

async function newX25519() {
  const priv = randomBytes(32);
  const { publicBytes } = await xKeysFromPrivate(priv);
  const pair = { priv: bytesToB64(priv), pub: bytesToB64Url(publicBytes) };
  priv.fill(0);
  return pair;
}

function ratchetFail(status) {
  return Object.assign(new Error(status), { ratchet: status });
}

async function skipTo(session, until, now) {
  if (until < session.nr) throw ratchetFail("replay");
  if (until - session.nr > MAX_SKIP) throw ratchetFail("noSession");
  let ck = b64ToBytes(session.ckr);
  while (session.nr < until) {
    const [mk, next] = await kdfCk(ck);
    session.skipped.push({ dh: session.dhr, n: session.nr, mk: bytesToB64(mk), ts: now });
    ck = next;
    session.nr += 1;
  }
  session.ckr = bytesToB64(ck);
}

function prunePeer(peer, now) {
  const sessions = [...peer.sessions].sort((a, b) => b.used - a.used);
  const current = sessions.find((session) => session.sid === peer.current);
  const kept = current ? [current] : [];
  for (const session of sessions) {
    if (session !== current && kept.length < SESSIONS_KEPT) kept.push(session);
  }
  for (const session of kept) {
    session.skipped = session.skipped.filter((key) => key.ts >= now - SKIPPED_LIFE_MS).slice(-SKIPPED_KEPT);
  }
  return { current: peer.current, sessions: kept };
}

function withRatchetLock(task) {
  const locks = globalThis.navigator?.locks;
  if (locks && typeof locks.request === "function") return locks.request(RATCHET_LOCK, task);
  return task();
}

async function frameDigest(frame) {
  return bytesToB64Url(await sha256(utf8(JSON.stringify(frame))));
}

function wrapAad(room, sender, recipient, nonce, tag) {
  return utf8(["forkmesh-chat-e2ee-wrap-v3", room, sender, recipient, nonce, tag].join("\n"));
}

export function bundleMessage(room, kid, idk, acct, node, spk) {
  return utf8([
    "forkmesh-chat-e2ee-bundle-v3", room, kid, idk, acct, node, spk.id, spk.x, spk.pq,
    String(spk.ts), String(spk.exp),
  ].join("\n"));
}

export async function sealedMessage(room, frame) {
  const v4 = frame.v === SEAL_V4;
  const head = [
    "forkmesh-chat-e2ee-sealed-v" + (v4 ? SEAL_V4 : SEAL_VERSION), room, frame.kid, frame.idk, String(frame.ctr), String(frame.ts),
    frame.nonce, frame.tag, bytesToB64Url(await sha256(b64ToBytes(frame.body))),
  ].join("\n");
  let tail = "";
  for (const entry of frame.keys) {
    tail += "\n" + (v4
      ? [
        entry.kid, entry.sid, entry.spk, await hashB64(entry.ct), entry.dh, String(entry.pn), String(entry.n),
        entry.nonce, entry.tag, entry.key,
      ]
      : [entry.kid, entry.spk, entry.eph, await hashB64(entry.ct), entry.nonce, entry.tag, entry.key]
    ).join("|");
  }
  return utf8(head + tail);
}

export function validAcct(acct) {
  if (typeof acct !== "string" || acct.includes("\n")) return false;
  const length = [...acct].length;
  return length >= 1 && length <= 128 && ACCT_RE.test(acct);
}

function validTimes(ts, exp) {
  return Number.isSafeInteger(ts) && Number.isSafeInteger(exp) &&
    exp - ts > 0 && exp - ts <= 9 * DAY_MS;
}

export async function verifyBundle(bundle, room, now) {
  try {
    if (!bundle || typeof bundle !== "object" || bundle.type !== KEY_TYPE || bundle.v !== SEAL_VERSION) {
      return false;
    }
    const { kid, idk, acct, node, spk, sig } = bundle;
    if (!spk || typeof spk !== "object" || !KID_RE.test(kid || "") || !KID_RE.test(spk.id || "")) {
      return false;
    }
    const idkBytes = strictB64u(idk, KEY32_RE);
    const x = strictB64u(spk.x, KEY32_RE);
    const pq = strictB64u(spk.pq, PQ_RE);
    if (idkBytes.length !== 32 || x.length !== 32 || pq.length !== MLKEM_PUBLIC_KEY_BYTES) return false;
    if (kid !== await id16(idkBytes) || spk.id !== await id16(concatBytes(x, pq)) ||
        !validAcct(acct) || !validAcct(node)) {
      return false;
    }
    if (!validTimes(spk.ts, spk.exp) || spk.ts > now + DAY_MS || spk.exp <= now) return false;
    return await edVerify(idkBytes, strictB64u(sig, SIG_RE), bundleMessage(room, kid, idk, acct, node, spk));
  } catch (_) {
    return false;
  }
}

function digitsFrom(h) {
  let digits = "";
  for (let i = 0; i < 60; i += 5) {
    let value = 0;
    for (let j = 0; j < 5; j += 1) value = value * 256 + h[i + j];
    digits += String(value % 100000).padStart(5, "0");
  }
  return digits;
}

function compareBytes(left, right) {
  const length = Math.min(left.length, right.length);
  for (let i = 0; i < length; i += 1) {
    if (left[i] !== right[i]) return left[i] - right[i];
  }
  return left.length - right.length;
}

export async function safetyNumber(scope, idks) {
  const keys = concatBytes(...[...idks].sort(compareBytes));
  let h = new Uint8Array(await subtle().digest(
    "SHA-512",
    concatBytes(utf8("forkmesh-safety-v3\n" + scope + "\n"), keys),
  ));
  const buffer = new Uint8Array(64 + keys.length);
  buffer.set(keys, 64);
  for (let i = 0; i < SAFETY_ITERATIONS; i += 1) {
    buffer.set(h, 0);
    h = new Uint8Array(await subtle().digest("SHA-512", buffer));
  }
  const digits = digitsFrom(h);
  return {
    digits,
    groups: digits.match(/.{5}/g),
    qr: "forkmesh-safety:3:" + scope + ":" + bytesToB64Url(h.slice(0, 32)),
  };
}

function publicPrekey(spk) {
  return { id: spk.id, x: bytesToB64Url(spk.x), pq: bytesToB64Url(spk.pq), ts: spk.ts, exp: spk.exp };
}

function serialPrekey(spk) {
  return {
    id: spk.id,
    xPriv: bytesToB64(spk.xPriv),
    x: bytesToB64Url(spk.x),
    pqSeed: bytesToB64(spk.pqSeed),
    pq: bytesToB64Url(spk.pq),
    ts: spk.ts,
    exp: spk.exp,
  };
}

async function buildPrekey(xPriv, pqSeed, ts) {
  const { privateKey, publicBytes } = await xKeysFromPrivate(xPriv);
  const pair = mlkemKeyPair(pqSeed);
  return {
    id: await id16(concatBytes(publicBytes, pair.publicKey)),
    x: publicBytes,
    pq: pair.publicKey,
    ts,
    exp: ts + PREKEY_LIFE_MS,
    xPriv,
    pqSeed,
    xKey: privateKey,
    pqSecret: pair.secretKey,
  };
}

export async function prekeyFromSecrets(xPriv, pqSeed, ts) {
  return buildPrekey(xPriv, pqSeed, ts);
}

async function restorePrekey(entry) {
  try {
    const xPriv = strictB64(entry?.xPriv, 32);
    const pqSeed = strictB64(entry?.pqSeed, 64);
    if (!Number.isSafeInteger(entry.ts)) return null;
    const spk = await buildPrekey(xPriv, pqSeed, entry.ts);
    return spk.id === entry.id ? spk : null;
  } catch (_) {
    return null;
  }
}

function wipePrekey(spk) {
  spk.xPriv?.fill(0);
  spk.pqSeed?.fill(0);
  spk.pqSecret?.fill(0);
  spk.xKey = null;
}

function emptyTrust() {
  return { records: {}, approvedKey: "" };
}

function cloneRecord(record) {
  return {
    kid: record.kid,
    idk: record.idk,
    acct: record.acct,
    node: record.node,
    state: record.state,
    firstSeen: record.firstSeen,
    lastSeen: record.lastSeen,
    verified: Boolean(record.verified),
    spk: { ...record.spk },
  };
}

function seenFromJson(value) {
  const out = new Map();
  if (!value || typeof value !== "object") return out;
  for (const [kid, entry] of Object.entries(value)) {
    const max = Number(entry?.m);
    if (!KID_RE.test(kid) || !Number.isSafeInteger(max) || max < 0) continue;
    const set = new Set();
    try {
      const bits = b64ToBytes(String(entry.w || ""));
      for (let i = 0; i < REPLAY_WINDOW && (i >>> 3) < bits.length; i += 1) {
        if ((bits[i >>> 3] >>> (i & 7)) & 1) set.add(max - i);
      }
    } catch (_) {}
    out.set(kid, { max, set });
  }
  return out;
}

function seenToJson(seen) {
  const out = {};
  for (const [kid, entry] of seen) {
    const bits = new Uint8Array(REPLAY_WINDOW / 8);
    for (const ctr of entry.set) {
      const i = entry.max - ctr;
      if (i >= 0 && i < REPLAY_WINDOW) bits[i >>> 3] |= 1 << (i & 7);
    }
    out[kid] = { m: entry.max, w: bytesToB64(bits) };
  }
  return JSON.stringify(out);
}

export function isSealKeyFrame(frame) {
  return Boolean(
    frame && typeof frame === "object" && frame.type === KEY_TYPE && frame.v === SEAL_VERSION &&
      KID_RE.test(frame.kid || "") && KEY32_RE.test(frame.idk || "") &&
      frame.spk && typeof frame.spk === "object",
  );
}

export function isSealedFrame(frame) {
  return Boolean(
    frame && typeof frame === "object" && frame.type === SEAL_TYPE &&
      (frame.v === SEAL_VERSION || frame.v === SEAL_V4) &&
      KID_RE.test(frame.kid || "") && Array.isArray(frame.keys) &&
      frame.keys.length >= (frame.v === SEAL_V4 ? 0 : 1) && frame.keys.length <= MAX_RECIPIENTS,
  );
}

export function frameKeyId(frame) {
  return isSealedFrame(frame) ? frame.kid : "";
}

function isKid(value) {
  return typeof value === "string" && KID_RE.test(value);
}

function isCount(value) {
  return Number.isInteger(value) && value >= 0 && value < 2 ** 31;
}

function checkEntryShape(entry, v4) {
  if (!entry || typeof entry !== "object" || !isKid(entry.kid) || !isKid(entry.spk)) return false;
  if (v4) {
    if (!isKid(entry.sid) || typeof entry.dh !== "string" || !KEY32_RE.test(entry.dh)) return false;
    if (!isCount(entry.pn) || !isCount(entry.n)) return false;
  } else if (!KEY32_RE.test(entry.eph || "")) {
    return false;
  }
  strictB64(entry.ct, MLKEM_CIPHERTEXT_BYTES);
  strictB64(entry.nonce, 12);
  strictB64(entry.tag, 16);
  strictB64(entry.key, 32);
  return true;
}

function sealedShape(frame) {
  try {
    if (!frame || typeof frame !== "object" || frame.type !== SEAL_TYPE) return false;
    const v4 = frame.v === SEAL_V4;
    if (!v4 && frame.v !== SEAL_VERSION) return false;
    if (!KID_RE.test(frame.kid || "") || !KEY32_RE.test(frame.idk || "") || !SIG_RE.test(frame.sig || "")) {
      return false;
    }
    if (!Number.isSafeInteger(frame.ctr) || frame.ctr < 1 || !Number.isSafeInteger(frame.ts)) return false;
    strictB64(frame.nonce, 12);
    strictB64(frame.tag, 16);
    if (!strictB64(frame.body).length) return false;
    const keys = frame.keys;
    if (!Array.isArray(keys) || keys.length < (v4 ? 0 : 1) || keys.length > MAX_RECIPIENTS) return false;
    for (let i = 0; i < keys.length; i += 1) {
      if (!checkEntryShape(keys[i], v4)) return false;
      if (i > 0 && !(keys[i - 1].kid < keys[i].kid)) return false;
      if (v4 && keys[i].kid === frame.kid) return false;
    }
    return true;
  } catch (_) {
    return false;
  }
}

function createContext({ storage, acct, node, now, wrappingKey }) {
  let wrapPromise = null;
  let chain = Promise.resolve();
  const ctx = {
    storage,
    acctSource: acct,
    node: String(node || ""),
    now,
    identity: null,
    prekeys: [],
    rotation: 0,
    trusts: new Map(),
    openedHere: new Set(),
    listeners: new Set(),
    wrapKey() {
      if (!wrapPromise) {
        wrapPromise = Promise.resolve()
          .then(() => (typeof wrappingKey === "function" ? wrappingKey() : wrappingKey || null))
          .catch(() => null);
      }
      return wrapPromise;
    },
    exclusive(task) {
      const run = chain.then(task, task);
      chain = run.catch(() => {});
      return run;
    },
    emit(event) {
      for (const listener of [...ctx.listeners]) {
        try { listener(event); } catch (_) {}
      }
    },
  };
  return ctx;
}

async function loadIdentity(ctx) {
  if (ctx.identity) return ctx.identity;
  let seed = null;
  const stored = await readSecret(ctx, IDENTITY_KEY);
  if (stored) {
    try { seed = strictB64(stored, 32); } catch (_) { seed = null; }
  }
  if (!seed) {
    seed = randomBytes(32);
    await writeSecret(ctx, IDENTITY_KEY, bytesToB64(seed));
  } else if (!safeJson(readItem(ctx.storage, IDENTITY_KEY))?.wrapped && await ctx.wrapKey()) {
    await writeSecret(ctx, IDENTITY_KEY, bytesToB64(seed));
  }
  const { privateKey, publicBytes } = await edKeysFromSeed(seed);
  seed.fill(0);
  ctx.identity = {
    privateKey,
    idkBytes: publicBytes,
    idk: bytesToB64Url(publicBytes),
    kid: await id16(publicBytes),
  };
  return ctx.identity;
}

async function readStoredPrekeys(ctx) {
  const list = safeJson(await readSecret(ctx, PREKEYS_KEY));
  if (!Array.isArray(list)) return [];
  const out = [];
  for (const entry of list.slice(-PREKEYS_KEPT * 2)) {
    const known = ctx.prekeys.find((spk) => spk.id === entry?.id);
    if (known) {
      out.push(known);
      continue;
    }
    const spk = await restorePrekey(entry);
    if (spk) out.push(spk);
  }
  return out;
}

async function syncPrekeys(ctx, extra = []) {
  const merged = new Map();
  for (const spk of [...await readStoredPrekeys(ctx), ...ctx.prekeys, ...extra]) {
    if (!merged.has(spk.id)) merged.set(spk.id, spk);
  }
  const now = ctx.now();
  const keep = [];
  for (const spk of [...merged.values()].sort((a, b) => a.ts - b.ts)) {
    if (now > spk.exp) wipePrekey(spk);
    else keep.push(spk);
  }
  while (keep.length > PREKEYS_KEPT) wipePrekey(keep.shift());
  for (const old of ctx.prekeys) if (!keep.includes(old)) wipePrekey(old);
  ctx.prekeys = keep;
  return keep;
}

async function persistPrekeys(ctx) {
  await writeSecret(ctx, PREKEYS_KEY, JSON.stringify(ctx.prekeys.map(serialPrekey)));
}

async function ensurePrekey(ctx) {
  const before = ctx.prekeys.map((spk) => spk.id).join(",");
  const newestBefore = ctx.prekeys[ctx.prekeys.length - 1]?.id || "";
  await syncPrekeys(ctx);
  const now = ctx.now();
  const newest = ctx.prekeys[ctx.prekeys.length - 1];
  let rotated = false;
  if (!newest || now - newest.ts >= PREKEY_ROTATE_MS) {
    const spk = await buildPrekey(randomBytes(32), randomBytes(64), now);
    await syncPrekeys(ctx, [spk]);
    rotated = true;
  }
  if (rotated || before !== ctx.prekeys.map((spk) => spk.id).join(",")) await persistPrekeys(ctx);
  const newestAfter = ctx.prekeys[ctx.prekeys.length - 1]?.id || "";
  if (newestBefore && newestAfter !== newestBefore) ctx.rotation += 1;
  return ctx.prekeys[ctx.prekeys.length - 1];
}

function trustFor(ctx, localScope) {
  let trust = ctx.trusts.get(localScope);
  if (!trust) {
    trust = { key: storageKey("trustV3", localScope), data: emptyTrust(), version: 0 };
    ctx.trusts.set(localScope, trust);
  }
  const raw = readItem(ctx.storage, trust.key);
  if (raw === null || raw === trust.raw) return trust;
  trust.raw = raw;
  const parsed = safeJson(raw);
  if (parsed && typeof parsed === "object" && parsed.records && typeof parsed.records === "object") {
    const records = {};
    for (const [kid, record] of Object.entries(parsed.records)) {
      if (record && record.kid === kid && KID_RE.test(kid) && typeof record.idk === "string" &&
          typeof record.acct === "string" && typeof record.node === "string" && record.spk && typeof record.spk === "object" &&
          ["approved", "pending", "blocked"].includes(record.state)) {
        records[kid] = record;
      }
    }
    trust.data = { records, approvedKey: String(parsed.approvedKey || "") };
  }
  return trust;
}

function persistTrust(ctx, trust) {
  trust.version += 1;
  const raw = JSON.stringify(trust.data);
  if (writeItem(ctx.storage, trust.key, raw)) trust.raw = raw;
}

class RoomSeal {
  constructor(ctx, { scope, roomName, controllerScope, acct, participants, version }) {
    this.ctx = ctx;
    this.acct = acct;
    this.node = ctx.node;
    this.participants = participants;
    this.roomName = roomName;
    this.controllerScope = controllerScope;
    this.channelScope = scope;
    this.policy = /^direct:/.test(scope) ? "strict" : "open";
    this.trustScope = this.policy === "strict" ? scope : roomName;
    this.version = version === SEAL_VERSION || version === SEAL_V4
      ? version
      : this.policy === "strict" ? SEAL_V4 : SEAL_VERSION;
    this.scope = `${controllerScope}\n${scope}`;
    this.localTrust = `${controllerScope}\n${this.trustScope}`;
    this.localRoom = `${controllerScope}\n${roomName}`;
    this.held = null;
    this.counter = 0;
    this.trustVersion = 0;
  }

  get keyId() {
    return this.ctx.identity?.kid || "";
  }

  get idk() {
    return this.ctx.identity?.idk || "";
  }

  get rotation() {
    return this.ctx.rotation;
  }

  get peerCount() {
    return this.recipientKeyIds().length;
  }

  trust() {
    return trustFor(this.ctx, this.localTrust);
  }

  records() {
    return Object.values(this.trust().data.records);
  }

  devices() {
    return this.records().map(cloneRecord);
  }

  pendingDevices() {
    return this.devices().filter((record) => record.state === "pending");
  }

  recipientKeyIds(now = this.ctx.now()) {
    return this.records()
      .filter((record) => record.state === "approved" && record.kid !== this.keyId && record.spk.exp > now)
      .map((record) => record.kid);
  }

  async bundle() {
    return this.ctx.exclusive(async () => {
      const identity = await loadIdentity(this.ctx);
      const spk = publicPrekey(await ensurePrekey(this.ctx));
      const signature = new Uint8Array(await subtle().sign(
        { name: "Ed25519" },
        identity.privateKey,
        bundleMessage(this.roomName, identity.kid, identity.idk, this.acct, this.node, spk),
      ));
      return {
        type: KEY_TYPE,
        v: SEAL_VERSION,
        kid: identity.kid,
        idk: identity.idk,
        acct: this.acct,
        node: this.node,
        spk,
        sig: bytesToB64Url(signature),
      };
    });
  }

  async accept(bundle) {
    const result = await this.ctx.exclusive(() => this.acceptLocked(bundle));
    if (result?.event) this.ctx.emit(result.event);
    if (result?.safety) this.ctx.emit(result.safety);
    return result;
  }

  async acceptLocked(bundle) {
    const now = this.ctx.now();
    const identity = await loadIdentity(this.ctx);
    if (!await verifyBundle(bundle, this.roomName, now)) return null;
    if (this.policy === "strict" && this.participants && !this.participants.includes(bundle.acct)) return null;
    if (bundle.kid === identity.kid) return { state: "approved", kid: bundle.kid, isNew: false, released: [] };
    const trust = this.trust();
    const records = trust.data.records;
    const known = records[bundle.kid];
    const spk = { id: bundle.spk.id, x: bundle.spk.x, pq: bundle.spk.pq, ts: bundle.spk.ts, exp: bundle.spk.exp };
    if (known) {
      if (known.idk !== bundle.idk || known.acct !== bundle.acct || known.node !== bundle.node) return null;
      let refreshed = false;
      if (spk.ts > known.spk.ts) {
        known.spk = spk;
        refreshed = true;
      }
      known.lastSeen = now;
      persistTrust(this.ctx, trust);
      if (refreshed) this.trustVersion += 1;
      const released = known.state === "approved" ? await this.releaseLocked(bundle.kid) : [];
      return {
        state: known.state, kid: bundle.kid, acct: known.acct, node: known.node, isNew: false, refreshed, released,
      };
    }
    let state = "approved";
    if (this.policy === "strict") {
      const taken = bundle.acct === this.acct || Object.values(records).some(
        (record) => record.acct === bundle.acct && record.state === "approved",
      );
      if (taken) {
        state = "pending";
        const pending = Object.values(records).filter((record) => record.state === "pending").length;
        if (pending >= PENDING_PER_SCOPE) return null;
      }
    }
    records[bundle.kid] = {
      kid: bundle.kid,
      idk: bundle.idk,
      acct: bundle.acct,
      node: bundle.node,
      spk,
      state,
      firstSeen: now,
      lastSeen: now,
      verified: false,
    };
    if (this.policy === "open") this.trimOpenRecords(records);
    const safety = state === "approved" ? await this.noteApprovedSet(trust) : null;
    persistTrust(this.ctx, trust);
    this.trustVersion += 1;
    const released = state === "approved" ? await this.releaseLocked(bundle.kid) : [];
    const event = state === "pending"
      ? this.event("pending", { kid: bundle.kid, acct: bundle.acct, node: bundle.node })
      : null;
    return { state, kid: bundle.kid, acct: bundle.acct, node: bundle.node, isNew: true, released, event, safety };
  }

  trimOpenRecords(records) {
    const all = Object.values(records);
    if (all.length <= OPEN_RECORDS_PER_SCOPE) return;
    all.sort((a, b) => (a.lastSeen || 0) - (b.lastSeen || 0));
    for (const record of all.slice(0, all.length - OPEN_RECORDS_PER_SCOPE)) delete records[record.kid];
  }

  event(type, extra = {}) {
    return {
      type,
      trustScope: this.trustScope,
      channelScope: this.channelScope,
      controllerScope: this.controllerScope,
      roomName: this.roomName,
      ...extra,
    };
  }

  approvedIdks(trust = this.trust()) {
    const idks = Object.values(trust.data.records)
      .filter((record) => record.state === "approved")
      .map((record) => record.idk);
    if (this.ctx.identity) idks.push(this.ctx.identity.idk);
    return [...new Set(idks)].sort();
  }

  async noteApprovedSet(trust) {
    if (this.policy !== "strict") return null;
    const own = this.ctx.identity?.idk || "";
    const key = this.approvedIdks(trust).join(",");
    const previous = trust.data.approvedKey;
    trust.data.approvedKey = key;
    if (previous === key) return null;
    const hadOthers = previous.split(",").some((idk) => idk && idk !== own);
    if (!hadOthers) return null;
    for (const record of Object.values(trust.data.records)) record.verified = false;
    return this.event("safety-changed");
  }

  async approve(kid) {
    const result = await this.ctx.exclusive(async () => {
      await loadIdentity(this.ctx);
      const trust = this.trust();
      const record = trust.data.records[kid];
      if (!record) return { released: [] };
      record.state = "approved";
      const safety = await this.noteApprovedSet(trust);
      persistTrust(this.ctx, trust);
      this.trustVersion += 1;
      return { released: await this.releaseLocked(kid), safety };
    });
    if (result.safety) this.ctx.emit(result.safety);
    this.ctx.emit(this.event("approved", { kid }));
    return result.released;
  }

  async block(kid) {
    const safety = await this.ctx.exclusive(async () => {
      await loadIdentity(this.ctx);
      const trust = this.trust();
      const record = trust.data.records[kid];
      if (!record) return null;
      record.state = "blocked";
      const changed = await this.noteApprovedSet(trust);
      persistTrust(this.ctx, trust);
      this.trustVersion += 1;
      this.dropHeld(kid);
      return changed;
    });
    if (safety) this.ctx.emit(safety);
    this.ctx.emit(this.event("blocked", { kid }));
  }

  async safetyNumber() {
    await loadIdentity(this.ctx);
    const trust = this.trust();
    const idks = this.approvedIdks(trust).map((idk) => b64UrlToBytes(idk));
    const number = await safetyNumber(this.trustScope, idks);
    const others = Object.values(trust.data.records).filter((record) => record.state === "approved");
    return {
      ...number,
      verified: others.length > 0 && others.every((record) => record.verified),
      devices: others.length + 1,
    };
  }

  async markVerified() {
    await this.ctx.exclusive(async () => {
      const trust = this.trust();
      for (const record of Object.values(trust.data.records)) {
        if (record.state === "approved") record.verified = true;
      }
      persistTrust(this.ctx, trust);
    });
    this.ctx.emit(this.event("verified"));
  }

  heldFrames() {
    if (!this.held) {
      const parsed = safeJson(readItem(this.ctx.storage, storageKey("heldV3", this.localRoom)));
      this.held = Array.isArray(parsed) ? parsed.filter((frame) => frame && typeof frame === "object") : [];
    }
    return this.held;
  }

  persistHeld() {
    const key = storageKey("heldV3", this.localRoom);
    if (!this.held?.length) {
      removeItem(this.ctx.storage, key);
      return;
    }
    writeItem(this.ctx.storage, key, JSON.stringify(this.held));
  }

  hold(frame) {
    const held = this.heldFrames();
    if (held.some((entry) => entry.sig === frame.sig)) return;
    held.push(frame);
    while (held.length > HELD_PER_ROOM) held.shift();
    this.persistHeld();
  }

  dropHeld(kid) {
    const held = this.heldFrames();
    const kept = held.filter((frame) => frame.kid !== kid);
    if (kept.length === held.length) return;
    this.held = kept;
    this.persistHeld();
  }

  async releaseLocked(kid) {
    const held = this.heldFrames();
    const mine = held.filter((frame) => frame.kid === kid);
    if (!mine.length) return [];
    this.held = held.filter((frame) => frame.kid !== kid);
    this.persistHeld();
    const opened = [];
    for (const frame of mine) {
      const result = await this.openLocked(frame, { history: false });
      if (result.status === "opened") opened.push(result);
    }
    return opened;
  }

  nextCounter() {
    const key = storageKey("ctrV3", this.localRoom);
    const previous = Number(readItem(this.ctx.storage, key)) || 0;
    const ctr = Math.max(previous, this.counter) + 1;
    this.counter = ctr;
    writeItem(this.ctx.storage, key, String(ctr));
    return ctr;
  }

  ratchetKey() {
    return storageKey("ratchetV4", this.localRoom);
  }

  async readRatchet() {
    const parsed = safeJson(await readSecret(this.ctx, this.ratchetKey()));
    const peers = parsed && parsed.v === SEAL_V4 && parsed.peers && typeof parsed.peers === "object" &&
      !Array.isArray(parsed.peers) ? parsed.peers : {};
    return { v: SEAL_V4, peers };
  }

  async writeRatchet(state) {
    if (!await writeSecret(this.ctx, this.ratchetKey(), JSON.stringify({ v: SEAL_V4, peers: state.peers }))) {
      throw new Error("The chat ratchet could not be stored.");
    }
  }

  async exportRatchets() {
    return this.ctx.exclusive(() => withRatchetLock(() => this.readRatchet()));
  }

  async importRatchets(state) {
    return this.ctx.exclusive(() => withRatchetLock(() => this.writeRatchet({ peers: state?.peers || {} })));
  }

  async resetPeer(kid) {
    return this.ctx.exclusive(() => withRatchetLock(async () => {
      const state = await this.readRatchet();
      if (!Object.hasOwn(state.peers, kid)) return;
      delete state.peers[kid];
      await this.writeRatchet(state);
    }));
  }

  openedKey() {
    return storageKey("openedV4", this.localRoom);
  }

  async readOpened(now) {
    const list = safeJson(await readSecret(this.ctx, this.openedKey()));
    return Array.isArray(list)
      ? list.filter((item) => item && typeof item.d === "string" && item.ts > now - OPENED_LIFE_MS && item.ts <= now + DAY_MS)
      : [];
  }

  async rememberOpened(digest, result, now) {
    this.noteOpenedHere(digest);
    const item = {
      d: digest, ts: now, own: Boolean(result.own), kid: result.kid, acct: result.acct, node: result.node, plain: result.plain,
    };
    if (JSON.stringify(item).length > OPENED_ENTRY_BYTES) return;
    const list = (await this.readOpened(now)).filter((entry) => entry.d !== digest);
    list.push(item);
    let total = 0;
    const kept = [];
    for (const entry of list.reverse()) {
      total += JSON.stringify(entry).length;
      if (kept.length >= OPENED_KEPT || total > OPENED_TOTAL_BYTES) break;
      kept.push(entry);
    }
    await writeSecret(this.ctx, this.openedKey(), JSON.stringify(kept.reverse()));
  }

  noteOpenedHere(digest) {
    const here = this.ctx.openedHere;
    here.delete(digest);
    here.add(digest);
    while (here.size > OPENED_HERE_KEPT) here.delete(here.values().next().value);
  }

  async openedElsewhere(frame, own) {
    const digest = await frameDigest(frame);
    if (this.ctx.openedHere.has(digest)) return null;
    const item = (await this.readOpened(this.ctx.now())).find((entry) => entry.d === digest && entry.own === own);
    if (!item || !item.plain || typeof item.plain !== "object") return null;
    this.noteOpenedHere(digest);
    return { plain: item.plain, kid: item.kid, acct: item.acct, node: item.node };
  }

  async prekeyFor(id, now) {
    let spk = this.ctx.prekeys.find((candidate) => candidate.id === id);
    if (!spk) {
      await syncPrekeys(this.ctx);
      spk = this.ctx.prekeys.find((candidate) => candidate.id === id);
    }
    return spk && spk.xKey && now <= spk.exp ? spk : null;
  }

  async sendEntry(peers, recipient, spk, now) {
    const peer = peers[recipient] || { current: null, sessions: [] };
    peers[recipient] = peer;
    let session = peer.sessions.find((candidate) => candidate.sid === peer.current) || null;
    if (session && session.cks === null && session.dhr === null) session = null;
    if (!session) {
      const eph = await newX25519();
      const ss1 = await x25519Shared(b64ToBytes(eph.priv), b64UrlToBytes(spk.x));
      const { ciphertext, sharedSecret } = mlkemEncapsulate(b64UrlToBytes(spk.pq));
      const sid = await sessionId(b64UrlToBytes(eph.pub), ciphertext);
      const [rk, ck] = await kdfInit(this.roomName, this.keyId, recipient, spk.id, sid, ss1, sharedSecret);
      ss1.fill(0);
      sharedSecret.fill(0);
      session = {
        sid, rk: bytesToB64(rk), dhs: eph, dhr: null, cks: bytesToB64(ck), ns: 0, pn: 0,
        sq: { spk: spk.id, ct: bytesToB64(ciphertext) }, ckr: null, nr: 0, skipped: [], created: now, used: now,
      };
      peer.sessions.push(session);
      peer.current = sid;
    } else if (session.cks === null) {
      const dhs = await newX25519();
      const { ciphertext, sharedSecret } = mlkemEncapsulate(b64UrlToBytes(spk.pq));
      const dhOut = await x25519Shared(b64ToBytes(dhs.priv), b64UrlToBytes(session.dhr));
      const [rk, cks] = await kdfRk(b64ToBytes(session.rk), dhOut, sharedSecret);
      dhOut.fill(0);
      sharedSecret.fill(0);
      Object.assign(session, {
        rk: bytesToB64(rk), cks: bytesToB64(cks), dhs, sq: { spk: spk.id, ct: bytesToB64(ciphertext) },
      });
    }
    const [mk, cks] = await kdfCk(b64ToBytes(session.cks));
    const entry = {
      kid: recipient, sid: session.sid, spk: session.sq.spk, ct: session.sq.ct, dh: session.dhs.pub,
      pn: session.pn, n: session.ns,
    };
    session.cks = bytesToB64(cks);
    session.ns += 1;
    session.used = now;
    return { entry, mk };
  }

  async receive(session, entry, now) {
    const index = session.skipped.findIndex((key) => key.dh === entry.dh && key.n === entry.n);
    if (index >= 0) return b64ToBytes(session.skipped.splice(index, 1)[0].mk);
    if (entry.dh !== session.dhr) {
      if (!session.dhs) throw ratchetFail("noSession");
      if (session.ckr !== null) await skipTo(session, entry.pn, now);
      const spk = await this.prekeyFor(entry.spk, now);
      if (!spk) throw ratchetFail("noSession");
      const ss = mlkemDecapsulate(spk.pqSecret, b64ToBytes(entry.ct));
      const dhOut = await x25519Shared(b64ToBytes(session.dhs.priv), b64UrlToBytes(entry.dh));
      const [rk, ckr] = await kdfRk(b64ToBytes(session.rk), dhOut, ss);
      ss.fill(0);
      dhOut.fill(0);
      Object.assign(session, {
        rk: bytesToB64(rk), ckr: bytesToB64(ckr), nr: 0, dhr: entry.dh, pn: session.ns, ns: 0, cks: null, sq: null,
      });
    }
    if (session.ckr === null) throw ratchetFail("noSession");
    await skipTo(session, entry.n, now);
    const [mk, ckr] = await kdfCk(b64ToBytes(session.ckr));
    session.ckr = bytesToB64(ckr);
    session.nr += 1;
    return mk;
  }

  async openEntry(state, sender, entry, now) {
    const peer = JSON.parse(JSON.stringify(state.peers[sender] || { current: null, sessions: [] }));
    let session = peer.sessions.find((candidate) => candidate.sid === entry.sid);
    if (!session) {
      const dh = b64UrlToBytes(entry.dh);
      const ct = b64ToBytes(entry.ct);
      if (entry.sid !== await sessionId(dh, ct) || entry.pn !== 0) throw ratchetFail("noSession");
      const spk = await this.prekeyFor(entry.spk, now);
      if (!spk) throw ratchetFail("noSession");
      const ss1 = await x25519(spk.xKey, dh);
      const ss2 = mlkemDecapsulate(spk.pqSecret, ct);
      const [rk, ck] = await kdfInit(this.roomName, sender, this.keyId, spk.id, entry.sid, ss1, ss2);
      ss1.fill(0);
      ss2.fill(0);
      session = {
        sid: entry.sid, rk: bytesToB64(rk), dhs: null, dhr: entry.dh, cks: null, ns: 0, pn: 0, sq: null,
        ckr: bytesToB64(ck), nr: 0, skipped: [], created: now, used: now,
      };
      peer.sessions.push(session);
    }
    const mk = await this.receive(session, entry, now);
    session.used = now;
    peer.current = session.sid;
    return { mk, peer };
  }

  sealTargets(identity, now, to, only) {
    return this.records()
      .filter((record) => record.state === "approved" && record.kid !== identity.kid && record.spk.exp > now)
      .filter((record) => !to || record.node === to || record.acct === this.acct)
      .filter((record) => !only || only.includes(record.kid))
      .sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0));
  }

  async seal4(plain, identity, now, to, only) {
    const targets = new Map(
      this.sealTargets(identity, now, to, only).slice(0, MAX_RECIPIENTS).map((record) => [record.kid, record.spk]),
    );
    return withRatchetLock(async () => {
      const state = await this.readRatchet();
      const ctr = this.nextCounter();
      const contentBytes = randomBytes(32);
      const content = await aesSeal(
        await aesKey(contentBytes, ["encrypt"]),
        utf8(JSON.stringify(plain)),
        contentAad(this.roomName, identity.kid, ctr, now, SEAL_V4),
      );
      const frame = {
        type: SEAL_TYPE,
        v: SEAL_V4,
        kid: identity.kid,
        idk: identity.idk,
        ctr,
        ts: now,
        nonce: bytesToB64(content.nonce),
        tag: bytesToB64(content.tag),
        body: bytesToB64(content.body),
        keys: [],
      };
      const touched = new Set();
      for (const recipient of [...targets.keys()].sort()) {
        const { entry, mk } = await this.sendEntry(state.peers, recipient, targets.get(recipient), now);
        touched.add(recipient);
        const wrapped = await aesSeal(
          await aesKey(mk, ["encrypt"]),
          contentBytes,
          await wrapAad4(this.roomName, identity.kid, entry, frame.nonce, frame.tag),
        );
        mk.fill(0);
        frame.keys.push({
          ...entry, nonce: bytesToB64(wrapped.nonce), tag: bytesToB64(wrapped.tag), key: bytesToB64(wrapped.body),
        });
      }
      contentBytes.fill(0);
      for (const kid of touched) state.peers[kid] = prunePeer(state.peers[kid], now);
      await this.writeRatchet(state);
      frame.sig = bytesToB64Url(new Uint8Array(await subtle().sign(
        { name: "Ed25519" },
        identity.privateKey,
        await sealedMessage(this.roomName, frame),
      )));
      await this.rememberOpened(await frameDigest(frame), {
        own: true, kid: identity.kid, acct: this.acct, node: this.node, plain,
      }, now).catch(() => {});
      return frame;
    });
  }

  async open4(frame, identity, record, commit) {
    const seenKey = storageKey("seenV3", this.localRoom);
    const seenAll = seenFromJson(safeJson(readItem(this.ctx.storage, seenKey)));
    const seen = seenAll.get(frame.kid) || { max: 0, set: new Set() };
    const opened = (found) => ({ status: "opened", ...found, self: false, replayed: true });
    const replay = async () => {
      const found = await this.openedElsewhere(frame, false);
      return found ? opened(found) : { status: "replay" };
    };
    if (frame.ctr <= seen.max - REPLAY_WINDOW || seen.set.has(frame.ctr)) return replay();
    const now = this.ctx.now();
    if (!(now - PREKEY_LIFE_MS <= frame.ts && frame.ts <= now + DAY_MS)) return { status: "invalid" };
    const entry = frame.keys.find((key) => key.kid === identity.kid);
    if (!entry) return { status: "notForMe", kid: frame.kid };
    const state = await this.readRatchet();
    let peer;
    let contentBytes;
    try {
      const result = await this.openEntry(state, frame.kid, entry, now);
      peer = result.peer;
      contentBytes = await aesOpen(
        await aesKey(result.mk, ["decrypt"]),
        b64ToBytes(entry.nonce),
        b64ToBytes(entry.key),
        b64ToBytes(entry.tag),
        await wrapAad4(this.roomName, frame.kid, entry, frame.nonce, frame.tag),
      );
      result.mk.fill(0);
    } catch (error) {
      if (error?.ratchet === "replay") return replay();
      return { status: "noSession", kid: frame.kid, later: frame.ctr < seen.max };
    }
    if (contentBytes.length !== 32) return { status: "invalid" };
    const contentKey = await aesKey(contentBytes, ["decrypt"]);
    contentBytes.fill(0);
    const clear = await aesOpen(
      contentKey,
      b64ToBytes(frame.nonce),
      b64ToBytes(frame.body),
      b64ToBytes(frame.tag),
      contentAad(this.roomName, frame.kid, frame.ctr, frame.ts, SEAL_V4),
    );
    const plain = JSON.parse(decoder.decode(clear));
    if (!plain || typeof plain !== "object" || Array.isArray(plain)) return { status: "invalid" };
    if (Object.hasOwn(plain, "senderId") && plain.senderId !== record.node) return { status: "invalid" };
    const result = { kid: frame.kid, acct: record.acct, node: record.node, plain };
    if (commit) {
      state.peers[frame.kid] = prunePeer(peer, now);
      await this.writeRatchet(state);
      seen.set.add(frame.ctr);
      seen.max = Math.max(seen.max, frame.ctr);
      for (const ctr of seen.set) if (ctr <= seen.max - REPLAY_WINDOW) seen.set.delete(ctr);
      seenAll.set(frame.kid, seen);
      writeItem(this.ctx.storage, seenKey, seenToJson(seenAll));
      await this.rememberOpened(await frameDigest(frame), result, now).catch(() => {});
    }
    return { status: "opened", ...result, self: false, replayed: false };
  }

  async seal(plain, { to = "", only = null } = {}) {
    return this.ctx.exclusive(async () => {
      const identity = await loadIdentity(this.ctx);
      const own = await ensurePrekey(this.ctx);
      const now = this.ctx.now();
      if (this.version === SEAL_V4) return this.seal4(plain, identity, now, to, Array.isArray(only) ? only : null);
      const targets = new Map([[identity.kid, publicPrekey(own)]]);
      const candidates = this.sealTargets(identity, now, to, Array.isArray(only) ? only : null);
      for (const record of candidates.slice(0, MAX_RECIPIENTS - 1)) targets.set(record.kid, record.spk);
      const ctr = this.nextCounter();
      const contentBytes = randomBytes(32);
      const contentKey = await aesKey(contentBytes, ["encrypt"]);
      const content = await aesSeal(
        contentKey,
        utf8(JSON.stringify(plain)),
        contentAad(this.roomName, identity.kid, ctr, now),
      );
      const frame = {
        type: SEAL_TYPE,
        v: SEAL_VERSION,
        kid: identity.kid,
        idk: identity.idk,
        ctr,
        ts: now,
        nonce: bytesToB64(content.nonce),
        tag: bytesToB64(content.tag),
        body: bytesToB64(content.body),
        keys: [],
      };
      for (const recipient of [...targets.keys()].sort()) {
        const spk = targets.get(recipient);
        const eph = await subtle().generateKey({ name: "X25519" }, false, ["deriveBits"]);
        const ephPub = new Uint8Array(await subtle().exportKey("raw", eph.publicKey));
        const ss1 = await x25519(eph.privateKey, b64UrlToBytes(spk.x));
        const { ciphertext, sharedSecret } = mlkemEncapsulate(b64UrlToBytes(spk.pq));
        const kekBytes = await deriveKek(this.roomName, identity.kid, recipient, spk.id, ephPub, ciphertext, ss1, sharedSecret);
        ss1.fill(0);
        sharedSecret.fill(0);
        const kek = await aesKey(kekBytes, ["encrypt"]);
        kekBytes.fill(0);
        const wrapped = await aesSeal(
          kek,
          contentBytes,
          wrapAad(this.roomName, identity.kid, recipient, frame.nonce, frame.tag),
        );
        frame.keys.push({
          kid: recipient,
          spk: spk.id,
          eph: bytesToB64Url(ephPub),
          ct: bytesToB64(ciphertext),
          nonce: bytesToB64(wrapped.nonce),
          tag: bytesToB64(wrapped.tag),
          key: bytesToB64(wrapped.body),
        });
      }
      contentBytes.fill(0);
      frame.sig = bytesToB64Url(new Uint8Array(await subtle().sign(
        { name: "Ed25519" },
        identity.privateKey,
        await sealedMessage(this.roomName, frame),
      )));
      return frame;
    });
  }

  async open(frame, options = {}) {
    return this.ctx.exclusive(() => this.openLocked(frame, options));
  }

  async openLocked(frame, { history = false, commit = true } = {}) {
    try {
      if (!sealedShape(frame) || frame.v !== this.version) return { status: "invalid" };
      const identity = await loadIdentity(this.ctx);
      const idkBytes = b64UrlToBytes(frame.idk);
      if (frame.kid !== await id16(idkBytes)) return { status: "invalid" };
      const self = frame.kid === identity.kid;
      if (self && frame.v === SEAL_V4) {
        const found = await this.openedElsewhere(frame, true);
        return found ? { status: "own", ...found, self: true } : { status: "own" };
      }
      let record = self
        ? { kid: identity.kid, idk: identity.idk, state: "approved", acct: null, node: null }
        : this.trust().data.records[frame.kid];
      if (!record || record.state !== "approved") {
        const blocked = record?.state === "blocked";
        if (!blocked && commit) this.hold(frame);
        return { status: "held", kid: frame.kid, blocked, pending: record?.state === "pending" };
      }
      if (record.idk !== frame.idk) return { status: "invalid" };
      if (!await edVerify(idkBytes, b64UrlToBytes(frame.sig), await sealedMessage(this.roomName, frame))) {
        return { status: "invalid" };
      }
      if (frame.v === SEAL_V4) return await withRatchetLock(() => this.open4(frame, identity, record, commit));
      const seenKey = storageKey("seenV3", this.localRoom);
      const seenAll = seenFromJson(safeJson(readItem(this.ctx.storage, seenKey)));
      const seen = seenAll.get(frame.kid) || { max: 0, set: new Set() };
      const replayed = frame.ctr <= seen.max - REPLAY_WINDOW || seen.set.has(frame.ctr);
      if (replayed && !history) return { status: "replay" };
      const now = this.ctx.now();
      if (!(now - PREKEY_LIFE_MS <= frame.ts && frame.ts <= now + DAY_MS)) return { status: "invalid" };
      const entry = frame.keys.find((key) => key.kid === identity.kid);
      if (!entry) return { status: "notForMe", kid: frame.kid };
      let spk = this.ctx.prekeys.find((candidate) => candidate.id === entry.spk);
      if (!spk) {
        await syncPrekeys(this.ctx);
        spk = this.ctx.prekeys.find((candidate) => candidate.id === entry.spk);
      }
      if (!spk || !spk.xKey || now > spk.exp) return { status: "notForMe", kid: frame.kid };
      const ct = b64ToBytes(entry.ct);
      const ephPub = b64UrlToBytes(entry.eph);
      const ss1 = await x25519(spk.xKey, ephPub);
      const ss2 = mlkemDecapsulate(spk.pqSecret, ct);
      const kekBytes = await deriveKek(this.roomName, frame.kid, identity.kid, spk.id, ephPub, ct, ss1, ss2);
      ss1.fill(0);
      ss2.fill(0);
      const kek = await aesKey(kekBytes, ["decrypt"]);
      kekBytes.fill(0);
      const contentBytes = await aesOpen(
        kek,
        b64ToBytes(entry.nonce),
        b64ToBytes(entry.key),
        b64ToBytes(entry.tag),
        wrapAad(this.roomName, frame.kid, identity.kid, frame.nonce, frame.tag),
      );
      if (contentBytes.length !== 32) return { status: "invalid" };
      const contentKey = await aesKey(contentBytes, ["decrypt"]);
      contentBytes.fill(0);
      const clear = await aesOpen(
        contentKey,
        b64ToBytes(frame.nonce),
        b64ToBytes(frame.body),
        b64ToBytes(frame.tag),
        contentAad(this.roomName, frame.kid, frame.ctr, frame.ts),
      );
      const plain = JSON.parse(decoder.decode(clear));
      if (!plain || typeof plain !== "object" || Array.isArray(plain)) return { status: "invalid" };
      if (record.node !== null && Object.hasOwn(plain, "senderId") && plain.senderId !== record.node) {
        return { status: "invalid" };
      }
      if (!replayed && commit) {
        seen.set.add(frame.ctr);
        seen.max = Math.max(seen.max, frame.ctr);
        for (const ctr of seen.set) if (ctr <= seen.max - REPLAY_WINDOW) seen.set.delete(ctr);
        seenAll.set(frame.kid, seen);
        writeItem(this.ctx.storage, seenKey, seenToJson(seenAll));
      }
      return {
        status: "opened",
        plain,
        kid: frame.kid,
        acct: self ? this.acct : record.acct,
        node: self ? this.node : record.node,
        self,
        replayed,
      };
    } catch (_) {
      return { status: "invalid" };
    }
  }

  shouldPersistAdvertisement(now = this.ctx.now()) {
    const key = storageKey("announcedV3", this.localRoom);
    const previous = Number(readItem(this.ctx.storage, key)) || 0;
    const rotation = String(this.ctx.rotation);
    const lastRotation = readItem(this.ctx.storage, key + "/rotation");
    if (now - previous < ANNOUNCE_DURABLE_MS && lastRotation === rotation) return false;
    writeItem(this.ctx.storage, key, String(now));
    writeItem(this.ctx.storage, key + "/rotation", rotation);
    return true;
  }
}

export function authenticatedPlain(result) {
  const plain = result?.plain;
  if (!plain || typeof plain !== "object") return null;
  const node = String(result.node || "");
  if (!node) return plain;
  if (Object.hasOwn(plain, "reactorId") && plain.reactorId !== node) return null;
  return Object.hasOwn(plain, "senderId") ? plain : { ...plain, senderId: node };
}

export async function sealKeyFrame(seal) {
  if (!(seal instanceof RoomSeal)) throw new TypeError("A room encryption identity is required.");
  return seal.bundle();
}

export async function acceptSealKey(frame, seal) {
  if (!isSealKeyFrame(frame) || !(seal instanceof RoomSeal)) return null;
  return seal.accept(frame);
}

export async function sealFrame(plain, seal, roomName = seal?.roomName, options = {}) {
  const room = String(roomName || "").trim();
  if (!(seal instanceof RoomSeal) || !room || room !== seal.roomName) {
    throw new TypeError("A room encryption identity is required.");
  }
  return seal.seal(plain, options);
}

export async function unsealFrame(frame, seal, roomName = seal?.roomName, options = {}) {
  const room = String(roomName || "").trim();
  if (!(seal instanceof RoomSeal) || room !== seal.roomName) return { status: "invalid" };
  return seal.open(frame, options);
}

function accountFor(ctx) {
  let value = ctx.acctSource;
  try {
    if (typeof value === "function") value = value();
  } catch (_) {
    value = "";
  }
  return String(value || "").trim().toLowerCase() || ctx.node;
}

export function createRoomSealStore({
  storage = browserStorage(),
  acct = "",
  node = "",
  now = () => Date.now(),
  wrappingKey = browserWrappingKey,
} = {}) {
  const ctx = createContext({ storage, acct, node, now, wrappingKey });
  const rooms = new Map();
  forgetLegacy(storage);

  return {
    has(scope) {
      return Boolean(scope);
    },
    get node() {
      return ctx.node;
    },
    account() {
      return accountFor(ctx);
    },
    async identity() {
      const identity = await loadIdentity(ctx);
      return { kid: identity.kid, idk: identity.idk };
    },
    subscribe(listener) {
      ctx.listeners.add(listener);
      return () => ctx.listeners.delete(listener);
    },
    wipeExpired() {
      return ctx.exclusive(async () => {
        await syncPrekeys(ctx);
        await persistPrekeys(ctx);
      });
    },
    async resolve(scope, roomName, controllerScope = "", { participants = null, version = 0 } = {}) {
      const room = String(roomName || "").trim();
      const channel = String(scope || "").trim();
      if (!channel || !room) throw new TypeError("A chat room is required.");
      const acct = accountFor(ctx);
      if (!validAcct(ctx.node) || !validAcct(acct)) throw new TypeError("A chat sender id is required.");
      removeItem(storage, "forkmesh.chat.seal." + channel);
      const controller = String(controllerScope || "").trim();
      const members = Array.isArray(participants)
        ? [...new Set(participants.map((name) => String(name || "").trim().toLowerCase()).filter(Boolean))].sort()
        : null;
      const cacheKey = `${controller}\n${channel}\n${room}\n${acct}\n${members ? members.join(",") : ""}\n${version}`;
      if (rooms.has(cacheKey)) return rooms.get(cacheKey);
      await ctx.exclusive(async () => {
        await loadIdentity(ctx);
        await ensurePrekey(ctx);
      });
      const seal = new RoomSeal(ctx, {
        scope: channel, roomName: room, controllerScope: controller, acct, participants: members, version,
      });
      rooms.set(cacheKey, seal);
      return seal;
    },
  };
}

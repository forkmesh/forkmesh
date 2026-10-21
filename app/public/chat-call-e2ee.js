import { mlkemDecapsulate, mlkemEncapsulate, mlkemKeyPair } from "./chat-mlkem.js";

export const CALL_E2EE_VERSION = 2;
export const MAX_PARTICIPANTS = 16;
export const MAX_RECIPIENTS = 31;
export const CLOCK_SKEW_MS = 5 * 60 * 1000;
export const PRESENCE_TIMEOUT_MS = 60 * 1000;
export const PRESENCE_GRACE_MS = 30 * 1000;
export const KEEP_PRESENCE_KEYS = 2;
export const SIGNING_PREFIX = "forkmesh-call-e2ee-";
export const NONCE_BYTES = 32;
export const ROUND_BYTES = 32;
export const SAS_EMOJI = 4;
export const REVEAL_TIMEOUT_MS = 10 * 1000;
export const ROUND_TIMEOUT_MS = 30 * 1000;
export const MAX_UNFINISHED = 3;
export const MAX_ROUNDS = 32;
export const MAX_GEN_JUMP = 1 << 16;
export const QR_PREFIX = "FMCALL2:";
export const AUTH_DIGEST_BYTES = 10;
export const MAX_AUTH_FRAMES = 100;
export const AUTH_HEADER_BYTES = 15;
export const MAX_AUTH_BYTES = AUTH_HEADER_BYTES + MAX_AUTH_FRAMES * AUTH_DIGEST_BYTES + 64;
export const AUTH_INTERVAL_MS = 200;
export const AUTH_DEADLINE_MS = 1000;
export const AUTH_HOLD_MS = 10 * 1000;
export const MISSING_GRACE_MS = 2 * 1000;
export const AUTH_RECENT_MS = 2 * 1000;
export const KEEP_FRAME_KEYS = 3;
export const REPLAY_WINDOW = 1024;

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
const FRAME_TRAILER = 9;
const FRAME_TAG = 16;
const AUTH_TYPE = (2 << 4) | 1;
const MAX_AUTH_WINDOWS = 64;
const SWEEP_EVERY_MS = 50;
const U64_MAX = (1n << 64n) - 1n;

const PRESENCE_LABEL = "forkmesh-call-e2ee-presence-v2";
const KEYS_LABEL = "forkmesh-call-e2ee-keys-v2";
const WRAP_LABEL = "forkmesh-call-e2ee-wrap-v2";
const FRAME_LABEL = "forkmesh-call-e2ee-frame-v2";
const SALT_LABEL = "forkmesh-call-e2ee-salt-v2";
const COMMIT_LABEL = "forkmesh-call-e2ee-commit-v2";
const NONCE_LABEL = "forkmesh-call-e2ee-nonce-v2";
const ROUND_LABEL = "forkmesh-call-e2ee-round-v2";
const SAS_LABEL = "forkmesh-call-e2ee-sas-v2";
const AUTH_LABEL = "forkmesh-call-e2ee-media-v2";
const DIGEST_LABEL = "forkmesh-call-e2ee-digest-v2";

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
  const auth = bytesField(o, "ak", IDK_BYTES);
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
  return { call, peer, kid, idk, epoch, ts, eph, auth, keys, sig };
}

export function marshalKeyMessage(m) {
  return {
    v: CALL_E2EE_VERSION, call: m.call, peer: m.peer, kid: m.kid, idk: b64uEncode(m.idk), epoch: m.epoch, ts: m.ts,
    eph: b64uEncode(m.eph), ak: b64uEncode(m.auth),
    keys: m.keys.map((k) => ({ peer: k.peer, epoch: k.epoch, ct: b64uEncode(k.ct), key: b64uEncode(k.key) })),
    sig: b64uEncode(m.sig),
  };
}

export async function keyMessageBytes(m) {
  let text = [
    KEYS_LABEL, m.call, m.peer, await keyId(m.idk), b64uEncode(m.idk), String(m.epoch), String(m.ts), b64uEncode(m.eph),
    b64uEncode(m.auth),
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
  return { call, peer: sender.peer, epoch: sender.epoch, keyId: sender.epoch & 0xff, raw };
}

export const EMOJI_TABLE = Object.freeze([
  [[0x1f436], "Dog"], [[0x1f431], "Cat"], [[0x1f981], "Lion"], [[0x1f40e], "Horse"],
  [[0x1f984], "Unicorn"], [[0x1f437], "Pig"], [[0x1f418], "Elephant"], [[0x1f430], "Rabbit"],
  [[0x1f43c], "Panda"], [[0x1f413], "Rooster"], [[0x1f427], "Penguin"], [[0x1f422], "Turtle"],
  [[0x1f41f], "Fish"], [[0x1f419], "Octopus"], [[0x1f98b], "Butterfly"], [[0x1f337], "Flower"],
  [[0x1f333], "Tree"], [[0x1f335], "Cactus"], [[0x1f344], "Mushroom"], [[0x1f30f], "Globe"],
  [[0x1f319], "Moon"], [[0x2601, 0xfe0f], "Cloud"], [[0x1f525], "Fire"], [[0x1f34c], "Banana"],
  [[0x1f34e], "Apple"], [[0x1f353], "Strawberry"], [[0x1f33d], "Corn"], [[0x1f355], "Pizza"],
  [[0x1f382], "Cake"], [[0x2764, 0xfe0f], "Heart"], [[0x1f600], "Smiley"], [[0x1f916], "Robot"],
  [[0x1f3a9], "Hat"], [[0x1f453], "Glasses"], [[0x1f527], "Spanner"], [[0x1f385], "Santa"],
  [[0x1f44d], "Thumbs up"], [[0x2602, 0xfe0f], "Umbrella"], [[0x231b], "Hourglass"], [[0x23f0], "Clock"],
  [[0x1f381], "Gift"], [[0x1f4a1], "Light bulb"], [[0x1f4d5], "Book"], [[0x270f, 0xfe0f], "Pencil"],
  [[0x1f4ce], "Paperclip"], [[0x2702, 0xfe0f], "Scissors"], [[0x1f512], "Lock"], [[0x1f511], "Key"],
  [[0x1f528], "Hammer"], [[0x260e, 0xfe0f], "Telephone"], [[0x1f3c1], "Flag"], [[0x1f682], "Train"],
  [[0x1f6b2], "Bicycle"], [[0x2708, 0xfe0f], "Aeroplane"], [[0x1f680], "Rocket"], [[0x1f3c6], "Trophy"],
  [[0x26bd], "Ball"], [[0x1f3b8], "Guitar"], [[0x1f3ba], "Trumpet"], [[0x1f514], "Bell"],
  [[0x2693], "Anchor"], [[0x1f3a7], "Headphones"], [[0x1f4c1], "Folder"], [[0x1f4cc], "Pin"],
].map(([codePoints, name]) => Object.freeze({ text: String.fromCodePoint(...codePoints), name })));

const BASE32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

function base32Encode(bytes) {
  let out = "";
  let bits = 0;
  let value = 0;
  for (const byte of bytes) {
    value = (value << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out += BASE32[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
    value &= (1 << bits) - 1;
  }
  if (bits > 0) out += BASE32[(value << (5 - bits)) & 31];
  return out;
}

function base32Decode(text) {
  let bits = 0;
  let value = 0;
  const out = [];
  for (const ch of text) {
    const v = BASE32.indexOf(ch);
    if (v < 0) fail("invalid");
    value = (value << 5) | v;
    bits += 5;
    if (bits >= 8) {
      out.push((value >>> (bits - 8)) & 0xff);
      bits -= 8;
    }
    value &= (1 << bits) - 1;
  }
  return Uint8Array.from(out);
}

function u32(n) {
  return Uint8Array.of((n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff);
}

function rosterEntries(members) {
  if (!Array.isArray(members) || members.length === 0 || members.length > MAX_PARTICIPANTS) fail("invalid");
  const peers = new Set();
  const entries = members.map((m, index) => {
    if (!(m.idk instanceof Uint8Array) || m.idk.length !== IDK_BYTES || !validAcct(m.acct) || !validPeer(m.peer) || peers.has(m.peer)) {
      fail("invalid");
    }
    peers.add(m.peer);
    const acct = utf8(m.acct);
    const peerBytes = utf8(m.peer);
    return { index, bytes: concat(m.idk, Uint8Array.of(acct.length >> 8, acct.length & 0xff), acct, Uint8Array.of(peerBytes.length), peerBytes) };
  });
  return entries.sort((a, b) => compareBytes(a.bytes, b.bytes));
}

export async function roundId(call, gen, members) {
  if (!validCall(call) || !Number.isInteger(gen) || gen < 1 || gen > MAX_EPOCH) fail("invalid");
  const entries = rosterEntries(members);
  return sha256(concat(utf8(ROUND_LABEL + "\n" + call + "\n"), u32(gen), Uint8Array.of(entries.length), ...entries.map((e) => e.bytes)));
}

export function sasFromTranscript(round, transcript) {
  const v = (transcript[0] << 16) | (transcript[1] << 8) | transcript[2];
  const emoji = [];
  for (let i = 0; i < SAS_EMOJI; i += 1) emoji.push((v >>> (18 - 6 * i)) & 63);
  const d = (((transcript[3] << 24) >>> 0) + (transcript[4] << 16) + (transcript[5] << 8) + transcript[6]) % 1000000;
  const digits = String(Math.floor(d / 1000)).padStart(3, "0") + " " + String(d % 1000).padStart(3, "0");
  return {
    round, transcript, emoji, digits, qr: QR_PREFIX + base32Encode(transcript),
    emojiText: emoji.map((i) => EMOJI_TABLE[i].text).join(" "),
    emojiNames: emoji.map((i) => EMOJI_TABLE[i].name).join(", "),
  };
}

export async function computeSas(call, gen, members) {
  const round = await roundId(call, gen, members);
  const entries = rosterEntries(members);
  const nonces = entries.map((e) => {
    const nonce = members[e.index].nonce;
    if (!(nonce instanceof Uint8Array) || nonce.length !== NONCE_BYTES) fail("invalid");
    return nonce;
  });
  return sasFromTranscript(round, await sha256(concat(utf8(SAS_LABEL + "\n"), round, ...nonces)));
}

export function parseQr(text) {
  if (typeof text !== "string" || !text.startsWith(QR_PREFIX) || text.length !== QR_PREFIX.length + 52) fail("invalid");
  const body = text.slice(QR_PREFIX.length);
  const bytes = base32Decode(body);
  if (bytes.length !== 32 || base32Encode(bytes) !== body) fail("invalid");
  return bytes;
}

export async function commitHash(call, peer, idk, round, gen, nonce) {
  return sha256(concat(utf8([NONCE_LABEL, call, peer, b64uEncode(idk), b64uEncode(round), String(gen)].join("\n") + "\n"), nonce));
}

function messageHead(input, kind) {
  const o = asObject(input);
  if (integer(o, "v", 0, Number.MAX_SAFE_INTEGER) !== CALL_E2EE_VERSION) fail("invalid");
  if (str(o, "t") !== kind) fail("invalid");
  return o;
}

export async function parseCommit(input) {
  const o = messageHead(input, "commit");
  const call = str(o, "call");
  if (!validCall(call)) fail("invalid");
  const peer = str(o, "peer");
  if (!validPeer(peer)) fail("invalid");
  const kid = str(o, "kid");
  const idk = bytesField(o, "idk", IDK_BYTES);
  if (kid !== await keyId(idk)) fail("invalid");
  const round = bytesField(o, "round", ROUND_BYTES);
  const gen = integer(o, "gen", 1, MAX_EPOCH);
  const ts = integer(o, "ts", 1, Number.MAX_SAFE_INTEGER);
  const c = bytesField(o, "c", 32);
  const sig = bytesField(o, "sig", SIG_BYTES);
  return { call, peer, kid, idk, round, gen, ts, c, sig };
}

export function marshalCommit(c) {
  return {
    v: CALL_E2EE_VERSION, t: "commit", call: c.call, peer: c.peer, kid: c.kid, idk: b64uEncode(c.idk),
    round: b64uEncode(c.round), gen: c.gen, ts: c.ts, c: b64uEncode(c.c), sig: b64uEncode(c.sig),
  };
}

export async function commitMessage(c) {
  return utf8([
    COMMIT_LABEL, c.call, c.peer, await keyId(c.idk), b64uEncode(c.idk), b64uEncode(c.round), String(c.gen), String(c.ts), b64uEncode(c.c),
  ].join("\n"));
}

export async function verifyCommit(c) {
  if (!await edVerify(c.idk, c.sig, await commitMessage(c))) fail("signature");
}

export async function commitOpens(c, nonce) {
  return nonce instanceof Uint8Array && nonce.length === NONCE_BYTES &&
    sameBytes(await commitHash(c.call, c.peer, c.idk, c.round, c.gen, nonce), c.c);
}

export async function parseReveal(input) {
  const o = messageHead(input, "reveal");
  const call = str(o, "call");
  if (!validCall(call)) fail("invalid");
  const peer = str(o, "peer");
  if (!validPeer(peer)) fail("invalid");
  const round = bytesField(o, "round", ROUND_BYTES);
  const gen = integer(o, "gen", 1, MAX_EPOCH);
  const nonce = bytesField(o, "nonce", NONCE_BYTES);
  return { call, peer, round, gen, nonce };
}

export function marshalReveal(r) {
  return { v: CALL_E2EE_VERSION, t: "reveal", call: r.call, peer: r.peer, round: b64uEncode(r.round), gen: r.gen, nonce: b64uEncode(r.nonce) };
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
  const verified = new Set();
  let sendEpoch = 0;
  let closed = false;
  let queue = Promise.resolve();
  let gen = 0;
  let round = null;
  let unfinished = 0;
  let rounds = 0;

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

  function rosterNow() {
    const roster = [{ idk, acct, peer, nonce: round ? round.nonce : null }];
    for (const who of sortedPeers()) {
      const m = members.get(who);
      roster.push({ idk: m.presence.idk, acct: m.presence.acct, peer: who, nonce: m.nonce });
    }
    return roster;
  }

  async function liveRound() {
    if (!round || round.stale) return null;
    return sameBytes(await roundId(call, round.gen, rosterNow()), round.id) ? round : null;
  }

  function retireRound() {
    if (!round) return;
    if (round.reveal && !round.code && !round.counted) unfinished += 1;
    wipe(round.nonce);
    round = null;
  }

  function identityKey(m) {
    return b64uEncode(m.idk) + "\u0000" + m.acct;
  }

  function markRoster() {
    for (const m of rosterNow()) verified.add(identityKey(m));
  }

  async function verificationNow() {
    const st = { round: null, gen: 0, needCommit: true, code: null, waiting: [], late: false, rounds, unfinished, verified: false, newcomers: [] };
    if (closed) return st;
    const roster = rosterNow();
    const r = await liveRound();
    st.needCommit = !r;
    if (r) {
      st.round = r.id;
      st.gen = r.gen;
      for (const who of sortedPeers()) {
        const m = members.get(who);
        if (!m.commit || m.commit.gen !== r.gen || !sameBytes(m.commit.round, r.id) || (r.reveal && !m.nonce)) st.waiting.push(who);
      }
      if (r.reveal && st.waiting.length === 0 && !r.code) r.code = await computeSas(call, r.gen, roster);
      st.code = r.code;
      const t = now();
      st.late = !st.code && t - r.started > REVEAL_TIMEOUT_MS;
      if (r.reveal && !r.code && !r.counted && t - r.revealedAt > ROUND_TIMEOUT_MS) {
        r.counted = true;
        unfinished += 1;
      }
    }
    st.rounds = rounds;
    st.unfinished = unfinished;
    if (verified.size) {
      st.newcomers = roster.filter((m) => !verified.has(identityKey(m))).map((m) => m.peer).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
      st.verified = st.newcomers.length === 0;
    }
    return st;
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
        const authSeed = draw(32);
        const auth = (await signerFromSeed(authSeed)).idk;
        const sender = { peer, idk, epoch };
        let message = null;
        try {
          const recipients = sortedPeers().map((who) => members.get(who).presence);
          if (recipients.length) {
            if (recipients.length > MAX_RECIPIENTS) fail("full");
            const xs = draw(X_BYTES);
            const eph = await x25519FromPrivate(xs);
            wipe(xs);
            const m = { call, peer, kid, idk, epoch, ts: now(), eph: eph.publicBytes, auth, keys: [], sig: null };
            for (const recipient of recipients) {
              const { entry, trace } = await wrapFrameSecret({ call, sender, recipient, eph, secret });
              wipe(trace.ss1, trace.ss2, trace.kek);
              m.keys.push(entry);
            }
            m.sig = await signer.sign(await keyMessageBytes(m));
            message = marshalKeyMessage(m);
          }
          const key = await deriveFrameKey(call, sender, secret);
          key.auth = auth;
          key.authSeed = authSeed;
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
          frameKey.auth = m.auth.slice();
          member.opened = m.epoch;
          return frameKey;
        } finally {
          wipe(secret);
        }
      });
    },

    commit() {
      return exclusive(async () => {
        ensureOpen();
        if (await liveRound()) return round.commit;
        if (gen === MAX_EPOCH) fail("exhausted");
        const roster = rosterNow();
        let next = gen + 1;
        for (const m of members.values()) {
          const c = m.commit;
          if (c && c.gen > next && sameBytes(await roundId(call, c.gen, roster), c.round)) next = c.gen;
        }
        const id = await roundId(call, next, roster);
        const nonce = draw(NONCE_BYTES);
        retireRound();
        gen = next;
        const c = { call, peer, kid, idk, round: id, gen: next, ts: now(), c: null, sig: null };
        c.c = await commitHash(call, peer, idk, id, next, nonce);
        c.sig = await signer.sign(await commitMessage(c));
        round = { id, gen: next, nonce, commit: marshalCommit(c), reveal: null, started: now(), revealedAt: 0, code: null, counted: false, stale: false };
        return round.commit;
      });
    },

    acceptCommit(input) {
      return exclusive(async () => {
        const c = await parseCommit(input);
        ensureOpen();
        if (c.call !== call) fail("call");
        if (c.peer === peer) fail(sameBytes(c.idk, idk) ? "own" : "peer-bound");
        const m = members.get(c.peer);
        if (!m) fail("unknown-peer");
        if (!sameBytes(m.presence.idk, c.idk)) fail("peer-bound");
        if (!fresh(c.ts)) fail("clock");
        if (c.gen > gen + MAX_GEN_JUMP) fail("invalid");
        await verifyCommit(c);
        const old = m.commit;
        if (old) {
          if (c.gen < old.gen) fail("stale");
          if (c.gen === old.gen) fail(sameBytes(c.round, old.round) && sameBytes(c.c, old.c) ? "duplicate" : "equivocation");
        }
        m.commit = c;
        m.nonce = null;
        const early = m.early;
        if (early && early.gen <= c.gen) {
          m.early = null;
          if (early.gen === c.gen && sameBytes(early.round, c.round) && await commitOpens(c, early.nonce)) m.nonce = early.nonce;
        }
        if (round && c.gen > round.gen && sameBytes(await roundId(call, c.gen, rosterNow()), c.round)) round.stale = true;
      });
    },

    reveal() {
      return exclusive(async () => {
        ensureOpen();
        if (!await liveRound()) fail("waiting");
        if (round.reveal) return round.reveal;
        for (const m of members.values()) {
          if (!m.commit || m.commit.gen !== round.gen || !sameBytes(m.commit.round, round.id)) fail("waiting");
        }
        round.reveal = marshalReveal({ call, peer, round: round.id, gen: round.gen, nonce: round.nonce });
        round.revealedAt = now();
        rounds += 1;
        return round.reveal;
      });
    },

    acceptReveal(input) {
      return exclusive(async () => {
        const r = await parseReveal(input);
        ensureOpen();
        if (r.call !== call) fail("call");
        if (r.peer === peer) fail("own");
        const m = members.get(r.peer);
        if (!m) fail("unknown-peer");
        if (!m.commit || r.gen > m.commit.gen) {
          if (!m.early || r.gen >= m.early.gen) m.early = r;
          fail("waiting");
        }
        if (r.gen < m.commit.gen) fail("stale");
        if (!sameBytes(r.round, m.commit.round) || !await commitOpens(m.commit, r.nonce)) fail("commitment");
        if (m.nonce) fail("duplicate");
        m.nonce = r.nonce;
      });
    },

    verification() {
      return exclusive(verificationNow);
    },

    markVerified(id) {
      return exclusive(async () => {
        const st = await verificationNow();
        if (!st.code || !sameBytes(id, st.code.round)) fail("waiting");
        markRoster();
      });
    },

    checkQr(text) {
      return exclusive(async () => {
        const t = parseQr(text);
        const st = await verificationNow();
        if (!st.code) fail("waiting");
        if (!sameBytes(t, st.code.transcript)) return false;
        markRoster();
        return true;
      });
    },

    unverify() {
      verified.clear();
    },

    close() {
      closed = true;
      for (const key of keys.splice(0)) wipe(key.pqSecret);
      members.clear();
      if (round) wipe(round.nonce);
      round = null;
    },
  };
}

export function parseFrameTrailer(sealed) {
  if (!(sealed instanceof Uint8Array) || sealed.length < FRAME_TRAILER + FRAME_TAG + 1) fail("invalid");
  const view = new DataView(sealed.buffer, sealed.byteOffset, sealed.byteLength);
  return { counter: view.getBigUint64(sealed.length - FRAME_TRAILER), keyId: sealed[sealed.length - 1] };
}

export async function frameDigest(sealed) {
  return (await sha256(concat(utf8(DIGEST_LABEL), sealed))).slice(0, AUTH_DIGEST_BYTES);
}

function authBody(m) {
  const head = new Uint8Array(AUTH_HEADER_BYTES);
  const view = new DataView(head.buffer);
  head[0] = AUTH_TYPE;
  head[1] = m.keyId;
  view.setUint32(2, m.epoch);
  view.setBigUint64(6, m.first);
  head[14] = m.digests.length / AUTH_DIGEST_BYTES;
  return concat(head, m.digests);
}

function authSigned(call, peer, body) {
  return concat(utf8(AUTH_LABEL + "\n" + call + "\n" + peer + "\n"), body);
}

export function marshalAuthMessage(m) {
  return concat(authBody(m), m.sig);
}

export function parseAuthMessage(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < AUTH_HEADER_BYTES + AUTH_DIGEST_BYTES + SIG_BYTES ||
      bytes.length > MAX_AUTH_BYTES || bytes[0] !== AUTH_TYPE) fail("invalid");
  const n = bytes[14];
  if (n < 1 || n > MAX_AUTH_FRAMES || bytes.length !== AUTH_HEADER_BYTES + n * AUTH_DIGEST_BYTES + SIG_BYTES) fail("invalid");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const m = { keyId: bytes[1], epoch: view.getUint32(2), first: view.getBigUint64(6) };
  if (m.epoch === 0 || m.keyId !== (m.epoch & 0xff) || m.first === 0n || m.first > U64_MAX - BigInt(n - 1)) fail("invalid");
  m.digests = bytes.slice(AUTH_HEADER_BYTES, AUTH_HEADER_BYTES + n * AUTH_DIGEST_BYTES);
  m.sig = bytes.slice(AUTH_HEADER_BYTES + n * AUTH_DIGEST_BYTES);
  return m;
}

export async function signAuthMessage(call, peer, signer, m) {
  m.sig = await signer.sign(authSigned(call, peer, authBody(m)));
  return marshalAuthMessage(m);
}

export async function verifyAuthMessage(call, peer, auth, m) {
  if (!(auth instanceof Uint8Array) || auth.length !== IDK_BYTES || !await edVerify(auth, m.sig, authSigned(call, peer, authBody(m)))) {
    fail("signature");
  }
}

function digestAt(m, counter) {
  const count = BigInt(m.digests.length / AUTH_DIGEST_BYTES);
  if (counter < m.first || counter - m.first >= count) return null;
  const i = Number(counter - m.first) * AUTH_DIGEST_BYTES;
  return m.digests.subarray(i, i + AUTH_DIGEST_BYTES);
}

function validMediaKey(key) {
  return key && validCall(key.call) && validPeer(key.peer) && Number.isInteger(key.epoch) && key.epoch >= 1 &&
    key.epoch <= MAX_EPOCH && key.keyId === (key.epoch & 0xff);
}

export async function createMediaSigner(key) {
  if (!validMediaKey(key) || !(key.authSeed instanceof Uint8Array) || key.authSeed.length !== 32) fail("invalid");
  const signer = await signerFromSeed(key.authSeed);
  if (key.auth && !sameBytes(key.auth, signer.idk)) fail("invalid");
  const { call, peer, epoch, keyId } = key;
  let first = 0n;
  let digests = [];
  const pending = [];
  let queue = Promise.resolve();

  function exclusive(task) {
    const run = queue.then(task);
    queue = run.catch(() => {});
    return run;
  }

  async function close() {
    if (!digests.length) return;
    const m = { keyId, epoch, first, digests: concat(...digests), sig: null };
    digests = [];
    pending.push(await signAuthMessage(call, peer, signer, m));
  }

  return {
    get auth() { return signer.idk.slice(); },
    add(sealed) {
      return exclusive(async () => {
        const t = parseFrameTrailer(sealed);
        if (t.keyId !== keyId) fail("invalid");
        if (digests.length && t.counter !== first + BigInt(digests.length)) await close();
        if (!digests.length) first = t.counter;
        digests.push(await frameDigest(sealed));
        if (digests.length === MAX_AUTH_FRAMES) await close();
      });
    },
    flush() {
      return exclusive(async () => {
        await close();
        return pending.splice(0);
      });
    },
  };
}

function newPeerAuth() {
  return { verified: 0, missing: 0, forged: 0, lastVerified: null, lastMissing: null, lastForged: null, missingRun: null, lastSweep: null, state: "pending" };
}

function record(a, verdict, at) {
  if (verdict === "verified") {
    a.verified += 1;
    a.lastVerified = at;
    a.missingRun = null;
  } else if (verdict === "forged") {
    a.forged += 1;
    a.lastForged = at;
  } else if (verdict === "missing") {
    a.missing += 1;
    a.lastMissing = at;
    if (a.missingRun === null) a.missingRun = at;
  }
}

function evaluate(a, t) {
  if (a.lastForged !== null && t - a.lastForged < AUTH_HOLD_MS) return "forged";
  if (a.lastMissing !== null && t - a.lastMissing < AUTH_RECENT_MS) return "missing";
  if (a.lastVerified !== null) return "verified";
  return "pending";
}

function dropCode(a, t) {
  const state = evaluate(a, t);
  if (state === "forged") return "forged";
  if (state === "missing" && a.missingRun !== null && t - a.missingRun >= MISSING_GRACE_MS) return "unverified";
  return "";
}

export function createMediaVerifier({ now = () => Date.now(), onState = null } = {}) {
  const peers = new Map();

  function sweepEntry(a, entry, t, force) {
    for (const [counter, pf] of entry.pending) {
      if (force || t - pf.at > AUTH_DEADLINE_MS) {
        entry.pending.delete(counter);
        record(a, "missing", Math.min(pf.at + AUTH_DEADLINE_MS, t));
      }
    }
    entry.windows = entry.windows.filter((w) => t - w.at <= AUTH_DEADLINE_MS);
  }

  function sweep(p, t, force) {
    if (!force && p.auth.lastSweep !== null && t - p.auth.lastSweep < SWEEP_EVERY_MS) return;
    p.auth.lastSweep = t;
    for (const entry of p.keys.values()) sweepEntry(p.auth, entry, t, false);
  }

  function notify(who, p, t) {
    const state = evaluate(p.auth, t);
    if (state === p.auth.state) return;
    p.auth.state = state;
    if (onState) onState(who, state);
  }

  function status(p, t) {
    return { state: evaluate(p.auth, t), dropping: dropCode(p.auth, t) !== "", verified: p.auth.verified, missing: p.auth.missing, forged: p.auth.forged };
  }

  return {
    setKey(key) {
      if (!validMediaKey(key) || !(key.auth instanceof Uint8Array) || key.auth.length !== IDK_BYTES) fail("invalid");
      let p = peers.get(key.peer);
      if (!p) {
        p = { keys: new Map(), order: [], newest: 0, auth: newPeerAuth() };
        peers.set(key.peer, p);
      }
      const known = p.keys.get(key.keyId);
      if (known && known.epoch === key.epoch && sameBytes(known.auth, key.auth)) return;
      if (p.order.length && key.epoch <= p.newest) fail("stale");
      const t = now();
      const drop = (id) => {
        sweepEntry(p.auth, p.keys.get(id), t, true);
        p.keys.delete(id);
      };
      if (known) {
        drop(key.keyId);
        p.order = p.order.filter((id) => id !== key.keyId);
      }
      p.keys.set(key.keyId, { epoch: key.epoch, call: key.call, auth: key.auth.slice(), pending: new Map(), windows: [] });
      p.order.push(key.keyId);
      p.newest = key.epoch;
      while (p.order.length > KEEP_FRAME_KEYS) drop(p.order.shift());
    },

    forget(who) {
      peers.delete(who);
    },

    async frame(who, sealed) {
      const tr = parseFrameTrailer(sealed);
      const digest = await frameDigest(sealed);
      const t = now();
      const p = peers.get(who);
      const entry = p && p.keys.get(tr.keyId);
      if (!entry) fail("unknown-peer");
      let verdict = "pending";
      for (let i = entry.windows.length - 1; i >= 0; i -= 1) {
        const want = digestAt(entry.windows[i].msg, tr.counter);
        if (want) {
          verdict = sameBytes(want, digest) ? "verified" : "forged";
          break;
        }
      }
      if (verdict === "pending") entry.pending.set(tr.counter, { digest, at: t });
      record(p.auth, verdict, t);
      while (entry.pending.size > REPLAY_WINDOW) {
        let oldest = null;
        for (const c of entry.pending.keys()) if (oldest === null || c < oldest) oldest = c;
        entry.pending.delete(oldest);
        record(p.auth, "missing", t);
      }
      sweep(p, t, false);
      const code = verdict === "forged" ? "forged" : dropCode(p.auth, t);
      notify(who, p, t);
      return code;
    },

    async accept(who, bytes) {
      const m = parseAuthMessage(bytes);
      const p = peers.get(who);
      const entry = p && p.keys.get(m.keyId);
      if (!entry) fail("unknown-peer");
      if (entry.epoch !== m.epoch) fail("stale");
      await verifyAuthMessage(entry.call, who, entry.auth, m);
      const t = now();
      sweep(p, t, true);
      const count = BigInt(m.digests.length / AUTH_DIGEST_BYTES);
      const last = m.first + count - 1n;
      let error = "";
      for (const w of entry.windows) {
        const o = w.msg;
        const oCount = BigInt(o.digests.length / AUTH_DIGEST_BYTES);
        const lo = o.first > m.first ? o.first : m.first;
        const oLast = o.first + oCount - 1n;
        const hi = oLast < last ? oLast : last;
        if (lo > hi) continue;
        const oa = Number(lo - o.first) * AUTH_DIGEST_BYTES;
        const ma = Number(lo - m.first) * AUTH_DIGEST_BYTES;
        const n = Number(hi - lo + 1n) * AUTH_DIGEST_BYTES;
        if (!sameBytes(o.digests.subarray(oa, oa + n), m.digests.subarray(ma, ma + n))) {
          record(p.auth, "forged", t);
          error = "equivocation";
          break;
        }
        if (o.first === m.first && oCount === count) {
          error = "duplicate";
          break;
        }
      }
      if (!error) {
        entry.windows.push({ msg: m, at: t });
        if (entry.windows.length > MAX_AUTH_WINDOWS) entry.windows.shift();
        for (const [counter, pf] of entry.pending) {
          const want = digestAt(m, counter);
          if (!want) continue;
          entry.pending.delete(counter);
          record(p.auth, sameBytes(want, pf.digest) ? "verified" : "forged", t);
        }
      }
      notify(who, p, t);
      if (error) fail(error);
    },

    status(who) {
      const p = peers.get(who);
      if (!p) return { state: "pending", dropping: false, verified: 0, missing: 0, forged: 0 };
      const t = now();
      sweep(p, t, true);
      const st = status(p, t);
      notify(who, p, t);
      return st;
    },
  };
}

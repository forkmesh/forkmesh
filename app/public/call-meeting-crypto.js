export const MEETING_ROOM_RE = /^[0-9a-f]{32}$/;
export const MEETING_KEY_RE = /^[0-9a-f]{64}$/;
export const MEETING_BOX_MAX_AGE_MS = 5 * 60 * 1000;
export const MEETING_CODE_WORDS = Object.freeze([
  "apple", "bridge", "cactus", "dolphin", "ember", "falcon", "glacier", "harbor",
  "island", "jasmine", "kettle", "lantern", "meadow", "nectar", "orchid", "pepper",
  "quartz", "river", "saddle", "tiger", "umbrella", "violet", "walnut", "yellow",
  "anchor", "bamboo", "canyon", "desert", "engine", "forest", "garden", "helmet",
]);

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export function parseMeetingLink(search, hash) {
  const room = String(search || "").replace(/^\?/, "").split(/[&=#]/)[0].toLowerCase();
  const key = String(hash || "").replace(/^#/, "").split(/[&=]/)[0].toLowerCase();
  if (!MEETING_ROOM_RE.test(room)) return { room: "", key: "", error: "room" };
  if (!MEETING_KEY_RE.test(key)) return { room, key: "", error: "key" };
  return { room, key, error: "" };
}

function hexBytes(hex) {
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; i += 1) bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return bytes;
}

export function base64url(bytes) {
  let text = "";
  for (const byte of bytes) text += String.fromCharCode(byte);
  return btoa(text).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function fromBase64url(text) {
  const clean = String(text || "").replace(/-/g, "+").replace(/_/g, "/");
  const raw = atob(clean + "===".slice((clean.length + 3) % 4));
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
  return bytes;
}

export async function deriveMeetingKeys(keyHex, room, subtle = globalThis.crypto.subtle) {
  if (!MEETING_KEY_RE.test(String(keyHex || "")) || !MEETING_ROOM_RE.test(String(room || ""))) {
    throw new Error("invalid meeting link");
  }
  const base = await subtle.importKey("raw", hexBytes(keyHex), "HKDF", false, ["deriveKey", "deriveBits"]);
  const salt = encoder.encode("forkmesh-meeting-v1\n" + room);
  const seal = await subtle.deriveKey(
    { name: "HKDF", hash: "SHA-256", salt, info: encoder.encode("seal") },
    base,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"],
  );
  const code = new Uint8Array(await subtle.deriveBits(
    { name: "HKDF", hash: "SHA-256", salt, info: encoder.encode("code") },
    base,
    64,
  ));
  return { room, seal, code: meetingCode(code), subtle };
}

export function meetingCode(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const digits = String(view.getUint32(0) % 1000000).padStart(6, "0");
  const words = [bytes[4], bytes[5], bytes[6]].map((value) => MEETING_CODE_WORDS[value % MEETING_CODE_WORDS.length]);
  return { digits: digits.slice(0, 3) + " " + digits.slice(3), words };
}

export async function sealMeetingBox(keys, plain, { now = Date.now(), scope = globalThis } = {}) {
  const iv = new Uint8Array(12);
  scope.crypto.getRandomValues(iv);
  const body = encoder.encode(JSON.stringify({ ...plain, at: now }));
  const sealed = new Uint8Array(await keys.subtle.encrypt(
    { name: "AES-GCM", iv, additionalData: encoder.encode(keys.room), tagLength: 128 },
    keys.seal,
    body,
  ));
  const out = new Uint8Array(iv.length + sealed.length);
  out.set(iv, 0);
  out.set(sealed, iv.length);
  return base64url(out);
}

export async function openMeetingBox(keys, box, { now = Date.now(), maxAgeMs = MEETING_BOX_MAX_AGE_MS } = {}) {
  let bytes;
  try {
    bytes = fromBase64url(box);
  } catch (_) {
    return null;
  }
  if (bytes.length < 12 + 16 + 2) return null;
  let plain;
  try {
    plain = new Uint8Array(await keys.subtle.decrypt(
      { name: "AES-GCM", iv: bytes.subarray(0, 12), additionalData: encoder.encode(keys.room), tagLength: 128 },
      keys.seal,
      bytes.subarray(12),
    ));
  } catch (_) {
    return null;
  }
  let value;
  try {
    value = JSON.parse(decoder.decode(plain));
  } catch (_) {
    return null;
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const at = Number(value.at);
  if (!Number.isFinite(at) || Math.abs(now - at) > maxAgeMs) return null;
  return value;
}

export function cleanMeetingName(value) {
  return String(value || "")
    .replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁦-⁩]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 40);
}

const PEER_RE = /^[0-9a-f]{32}$/;

export function meetingEnvelope(type, body, { name = "", transcribing = false, camera = false, microphone = false } = {}) {
  return { t: String(type || ""), f: body && typeof body === "object" ? body : {}, n: cleanMeetingName(name), tr: Boolean(transcribing), cam: Boolean(camera), mic: Boolean(microphone) };
}

export function meetingFrame(plain) {
  if (!plain || typeof plain !== "object") return null;
  const type = String(plain.t || "");
  const body = plain.f && typeof plain.f === "object" && !Array.isArray(plain.f) ? plain.f : {};
  const peer = String(body.peer || "");
  if (!type || !PEER_RE.test(peer)) return null;
  const name = cleanMeetingName(plain.n) || "Guest";
  return {
    type,
    peer,
    name,
    transcribing: Boolean(plain.tr),
    camera: plain.cam !== false,
    microphone: plain.mic !== false,
    leaving: type === "group-call-presence" && body.call === "out",
    frame: type === "meeting-status" ? null : { ...body, type, sender: name.toLowerCase() },
  };
}

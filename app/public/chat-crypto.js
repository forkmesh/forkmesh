const encoder = new TextEncoder();
const decoder = new TextDecoder();
const MEETING_ID_RE = /^[A-Za-z0-9_-]{1,64}$/;
const P256_COORDINATE_RE = /^[A-Za-z0-9_-]{43}$/;

export function bytesToB64(bytes) {
  const array = new Uint8Array(bytes);
  let binary = "";
  for (let index = 0; index < array.length; index += 1) {
    binary += String.fromCharCode(array[index]);
  }
  return btoa(binary);
}

export function b64ToBytes(value) {
  const binary = atob(String(value || ""));
  const output = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    output[index] = binary.charCodeAt(index);
  }
  return output;
}

export function bytesToB64Url(bytes) {
  return bytesToB64(bytes)
    .replaceAll("+", "-")
    .replaceAll("/", "_")
    .replace(/=+$/g, "");
}

export function b64UrlToBytes(value) {
  let encoded = String(value || "").replaceAll("-", "+").replaceAll("_", "/");
  while (encoded.length % 4) encoded += "=";
  return b64ToBytes(encoded);
}

export async function sha256Base64Url(value) {
  const bytes = value instanceof Uint8Array
    ? value
    : value instanceof ArrayBuffer
      ? new Uint8Array(value)
      : encoder.encode(String(value ?? ""));
  const digest = await globalThis.crypto.subtle.digest("SHA-256", bytes);
  return bytesToB64Url(digest);
}

export async function derivePassphraseKey(passphrase, roomName) {
  const saltDigest = new Uint8Array(
    await globalThis.crypto.subtle.digest(
      "SHA-256",
      encoder.encode("ForkMesh room:" + String(roomName || "")),
    ),
  );
  const baseKey = await globalThis.crypto.subtle.importKey(
    "raw",
    encoder.encode(String(passphrase || "")),
    "PBKDF2",
    false,
    ["deriveKey"],
  );
  return globalThis.crypto.subtle.deriveKey(
    {
      name: "PBKDF2",
      salt: saltDigest.slice(0, 16),
      iterations: 210000,
      hash: "SHA-256",
    },
    baseKey,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"],
  );
}

export async function encryptObject(value, key) {
  if (!key) throw new TypeError("An encryption key is required.");
  const nonce = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const combined = new Uint8Array(
    await globalThis.crypto.subtle.encrypt(
      { name: "AES-GCM", iv: nonce, tagLength: 128 },
      key,
      encoder.encode(JSON.stringify(value)),
    ),
  );
  const body = combined.slice(0, combined.length - 16);
  const tag = combined.slice(combined.length - 16);
  return {
    kind: "cipher",
    v: 1,
    nonce: bytesToB64(nonce),
    tag: bytesToB64(tag),
    body: bytesToB64(body),
  };
}

export async function decryptObject(envelope, key) {
  if (!key || !envelope || envelope.kind !== "cipher" || envelope.v !== 1) {
    return null;
  }
  try {
    const nonce = b64ToBytes(envelope.nonce);
    const body = b64ToBytes(envelope.body);
    const tag = b64ToBytes(envelope.tag);
    if (nonce.length !== 12 || tag.length !== 16 || !body.length) return null;
    const combined = new Uint8Array(body.length + tag.length);
    combined.set(body, 0);
    combined.set(tag, body.length);
    const plain = await globalThis.crypto.subtle.decrypt(
      { name: "AES-GCM", iv: nonce, tagLength: 128 },
      key,
      combined,
    );
    return JSON.parse(decoder.decode(plain));
  } catch (_) {
    return null;
  }
}

function safeMeetingId(value, label) {
  const result = String(value || "");
  if (!MEETING_ID_RE.test(result)) {
    throw new TypeError(`Invalid ${label}.`);
  }
  return result;
}

function safeMeetingTimestamp(value) {
  const result = Number(value);
  if (!Number.isSafeInteger(result) || result < 1) {
    throw new TypeError("Invalid meeting timestamp.");
  }
  return result;
}

function safePublicJwk(value) {
  if (!value || typeof value !== "object") return null;
  const keys = Object.keys(value).sort();
  if (JSON.stringify(keys) !== JSON.stringify(["crv", "kty", "x", "y"])) {
    return null;
  }
  if (value.kty !== "EC" || value.crv !== "P-256") return null;
  if (!P256_COORDINATE_RE.test(value.x) || !P256_COORDINATE_RE.test(value.y)) {
    return null;
  }
  return { kty: "EC", crv: "P-256", x: value.x, y: value.y };
}

async function normalizedMeetingContent(input) {
  const text = String(input?.text || "").slice(0, 16000);
  const source = input?.attachment;
  let attachment = null;
  if (source && typeof source === "object" && typeof source.file === "string") {
    const bytes = b64ToBytes(source.file);
    attachment = {
      fileName: String(source.fileName || "file").slice(0, 180),
      fileMime: String(source.fileMime || "application/octet-stream").slice(0, 100),
      size: bytes.byteLength,
      digest: await sha256Base64Url(bytes),
    };
  }
  return { text, attachment };
}

async function meetingCanonical(input) {
  const participantId = safeMeetingId(input?.participantId, "participant id");
  const messageId = safeMeetingId(input?.messageId, "message id");
  const senderId = safeMeetingId(input?.senderId, "sender id");
  const timestamp = safeMeetingTimestamp(input?.ts);
  const content = await normalizedMeetingContent(input);
  const contentDigest = await sha256Base64Url(JSON.stringify(content));
  return [
    "forkmesh-office-bubble-v1",
    participantId,
    messageId,
    senderId,
    String(timestamp),
    contentDigest,
  ].join("\n");
}

export async function createMeetingBinding() {
  const pair = await globalThis.crypto.subtle.generateKey(
    { name: "ECDSA", namedCurve: "P-256" },
    false,
    ["sign", "verify"],
  );
  const exported = await globalThis.crypto.subtle.exportKey("jwk", pair.publicKey);
  const publicJwk = safePublicJwk({
    kty: exported.kty,
    crv: exported.crv,
    x: exported.x,
    y: exported.y,
  });
  if (!publicJwk) throw new Error("Could not create a meeting identity.");
  return { privateKey: pair.privateKey, publicJwk };
}

export async function signMeetingProof(privateKey, input) {
  if (!privateKey || privateKey.type !== "private" || privateKey.extractable) {
    throw new TypeError("A non-exportable meeting private key is required.");
  }
  const canonical = await meetingCanonical(input);
  const signature = await globalThis.crypto.subtle.sign(
    { name: "ECDSA", hash: "SHA-256" },
    privateKey,
    encoder.encode(canonical),
  );
  return {
    v: 1,
    participantId: safeMeetingId(input?.participantId, "participant id"),
    ts: safeMeetingTimestamp(input?.ts),
    signature: bytesToB64Url(signature),
  };
}

export async function verifyMeetingProof(publicJwk, proof, input) {
  try {
    const safeJwk = safePublicJwk(publicJwk);
    if (!safeJwk || !proof || proof.v !== 1) return false;
    if (proof.participantId !== input?.participantId || proof.ts !== input?.ts) {
      return false;
    }
    const canonical = await meetingCanonical(input);
    const key = await globalThis.crypto.subtle.importKey(
      "jwk",
      safeJwk,
      { name: "ECDSA", namedCurve: "P-256" },
      false,
      ["verify"],
    );
    return globalThis.crypto.subtle.verify(
      { name: "ECDSA", hash: "SHA-256" },
      key,
      b64UrlToBytes(proof.signature),
      encoder.encode(canonical),
    );
  } catch (_) {
    return false;
  }
}

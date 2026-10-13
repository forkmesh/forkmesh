// Provider-neutral, client-side encrypted portable identity container.
//
// The relay never receives the passphrase or this container's plaintext.  The
// format is deliberately based on standard WebCrypto primitives so mobile,
// desktop, and independent clients can restore it without ForkMesh hosting.

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export const PORTABLE_IDENTITY_TYPE = "forkmesh.portable-identity";
export const PORTABLE_IDENTITY_VERSION = 1;
export const PORTABLE_IDENTITY_AAD = "forkmesh.portable-identity/v1";
export const PORTABLE_IDENTITY_PBKDF2_ITERATIONS = 600000;
const MAX_PLAINTEXT_BYTES = 128 * 1024;

function bytesToB64Url(bytes) {
  let binary = "";
  for (const value of new Uint8Array(bytes)) binary += String.fromCharCode(value);
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/g, "");
}

function b64UrlToBytes(value) {
  const text = String(value || "");
  if (!/^[A-Za-z0-9_-]+$/.test(text) || text.includes("=")) throw new TypeError("Invalid base64url.");
  let padded = text.replaceAll("-", "+").replaceAll("_", "/");
  while (padded.length % 4) padded += "=";
  const binary = atob(padded);
  const out = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) out[index] = binary.charCodeAt(index);
  if (bytesToB64Url(out) !== text) throw new TypeError("Non-canonical base64url.");
  return out;
}

function assertPassphrase(passphrase) {
  if (typeof passphrase !== "string" || passphrase.length < 12) {
    throw new TypeError("Use a recovery passphrase of at least 12 characters.");
  }
}

function parseContainer(value) {
  const container = typeof value === "string" ? JSON.parse(value) : value;
  if (!container || typeof container !== "object" || Array.isArray(container)) throw new TypeError("Invalid portable identity.");
  const { type, schemaVersion, kdf, encryption } = container;
  if (type !== PORTABLE_IDENTITY_TYPE || schemaVersion !== PORTABLE_IDENTITY_VERSION || !kdf || !encryption) {
    throw new TypeError("Unsupported portable identity format.");
  }
  if (kdf.name !== "PBKDF2-HMAC-SHA-256" || kdf.iterations !== PORTABLE_IDENTITY_PBKDF2_ITERATIONS ||
      encryption.name !== "AES-256-GCM") throw new TypeError("Unsupported portable identity encryption.");
  const salt = b64UrlToBytes(kdf.salt);
  const iv = b64UrlToBytes(encryption.iv);
  const ciphertext = b64UrlToBytes(encryption.ciphertext);
  if (salt.length !== 16 || iv.length !== 12 || ciphertext.length < 17 || ciphertext.length > MAX_PLAINTEXT_BYTES + 16) {
    throw new TypeError("Invalid portable identity encryption parameters.");
  }
  return { container, salt, iv, ciphertext };
}

async function deriveKey(passphrase, salt) {
  const baseKey = await globalThis.crypto.subtle.importKey(
    "raw", encoder.encode(passphrase), "PBKDF2", false, ["deriveKey"],
  );
  return globalThis.crypto.subtle.deriveKey(
    { name: "PBKDF2", hash: "SHA-256", salt, iterations: PORTABLE_IDENTITY_PBKDF2_ITERATIONS },
    baseKey, { name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"],
  );
}

/** Encrypt identity material for an offline recovery kit or another client. */
export async function exportPortableIdentity(identity, passphrase, createdAt = Date.now()) {
  assertPassphrase(passphrase);
  if (!identity || typeof identity !== "object" || Array.isArray(identity)) throw new TypeError("Identity payload must be an object.");
  const plaintext = encoder.encode(JSON.stringify(identity));
  if (!plaintext.length || plaintext.length > MAX_PLAINTEXT_BYTES) throw new TypeError("Identity payload is empty or too large.");
  const salt = globalThis.crypto.getRandomValues(new Uint8Array(16));
  const iv = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const key = await deriveKey(passphrase, salt);
  const ciphertext = await globalThis.crypto.subtle.encrypt(
    { name: "AES-GCM", iv, tagLength: 128, additionalData: encoder.encode(PORTABLE_IDENTITY_AAD) }, key, plaintext,
  );
  return {
    type: PORTABLE_IDENTITY_TYPE,
    schemaVersion: PORTABLE_IDENTITY_VERSION,
    createdAt: Number.isSafeInteger(createdAt) && createdAt > 0 ? createdAt : Date.now(),
    kdf: { name: "PBKDF2-HMAC-SHA-256", iterations: PORTABLE_IDENTITY_PBKDF2_ITERATIONS, salt: bytesToB64Url(salt) },
    encryption: { name: "AES-256-GCM", iv: bytesToB64Url(iv), ciphertext: bytesToB64Url(ciphertext) },
  };
}

/** Decrypt and validate an offline portable identity container. */
export async function importPortableIdentity(container, passphrase) {
  assertPassphrase(passphrase);
  const parsed = parseContainer(container);
  const key = await deriveKey(passphrase, parsed.salt);
  let plaintext;
  try {
    plaintext = await globalThis.crypto.subtle.decrypt(
      { name: "AES-GCM", iv: parsed.iv, tagLength: 128, additionalData: encoder.encode(PORTABLE_IDENTITY_AAD) }, key, parsed.ciphertext,
    );
  } catch (_) {
    throw new TypeError("Recovery passphrase is incorrect or the file was changed.");
  }
  const raw = new Uint8Array(plaintext);
  if (!raw.length || raw.length > MAX_PLAINTEXT_BYTES) throw new TypeError("Invalid portable identity payload.");
  const identity = JSON.parse(decoder.decode(raw));
  if (!identity || typeof identity !== "object" || Array.isArray(identity)) throw new TypeError("Invalid portable identity payload.");
  return identity;
}

export function validatePortableIdentity(container) {
  parseContainer(container);
  return true;
}

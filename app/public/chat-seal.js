import {
  b64ToBytes,
  b64UrlToBytes,
  bytesToB64,
  bytesToB64Url,
} from "./chat-crypto.js";

// Automatic end-to-end encryption for room chat.
//
// The ordinary room cipher is still useful on the wire, but its passphrase is
// derived by the relay from DATA_KEY. This layer uses a persistent X25519 key
// created on each participant's device and never sent anywhere. Room peers
// exchange only public keys. Every protected application frame is encrypted under a
// fresh random AES-256-GCM key; that key is wrapped once for each known device
// using an X25519/HKDF pairwise key.
//
// The relay sees and retains only these two inner frame shapes:
//
//   { type:"e2ee-key", v:2, kid, pub }
//   { type:"sealed", v:2, kid, pub, nonce, tag, body, keys:[...] }
//
// A newly seen device can read messages addressed after its public key is
// learned. It cannot retroactively open history never addressed to it.
const SEAL_TYPE = "sealed";
const KEY_TYPE = "e2ee-key";
const SEAL_VERSION = 2;
const KEY_ID_CHARS = 16;
const KEY_ID_RE = new RegExp(`^[A-Za-z0-9_-]{${KEY_ID_CHARS}}$`);
const PUBLIC_KEY_RE = /^[A-Za-z0-9_-]{43}$/;
const IDENTITY_STORAGE_KEY = "forkmesh.chat.e2ee.x25519.v2";
const PEER_STORAGE_PREFIX = "forkmesh.chat.e2ee.peers.v2.";
const ANNOUNCED_STORAGE_PREFIX = "forkmesh.chat.e2ee.announced.v2.";
const LEGACY_PHRASE_STORAGE_PREFIX = "forkmesh.chat.seal.";
const MAX_RECIPIENTS = 64;
const ANNOUNCE_DURABLE_MS = 24 * 60 * 60 * 1000;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

async function sha256(bytes) {
  return new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", bytes));
}

function randomBytes(count) {
  return globalThis.crypto.getRandomValues(new Uint8Array(count));
}

function browserStorage() {
  try {
    return globalThis.localStorage || null;
  } catch (_) {
    return null;
  }
}

function safeJson(value) {
  try {
    return JSON.parse(String(value || ""));
  } catch (_) {
    return null;
  }
}

function validPrivateJwk(value) {
  return Boolean(
    value &&
      typeof value === "object" &&
      value.kty === "OKP" &&
      value.crv === "X25519" &&
      PUBLIC_KEY_RE.test(value.x || "") &&
      PUBLIC_KEY_RE.test(value.d || ""),
  );
}

async function keyIdForPublic(publicBytes) {
  return bytesToB64Url(await sha256(publicBytes)).slice(0, KEY_ID_CHARS);
}

async function importPublic(publicBytes) {
  if (!(publicBytes instanceof Uint8Array) || publicBytes.length !== 32) {
    throw new TypeError("Invalid X25519 public key.");
  }
  return globalThis.crypto.subtle.importKey(
    "raw",
    publicBytes,
    { name: "X25519" },
    false,
    [],
  );
}

async function identityFromJwk(jwk) {
  if (!validPrivateJwk(jwk)) throw new TypeError("Invalid X25519 identity.");
  const privateKey = await globalThis.crypto.subtle.importKey(
    "jwk",
    jwk,
    { name: "X25519" },
    false,
    ["deriveBits"],
  );
  const publicBytes = b64UrlToBytes(jwk.x);
  if (publicBytes.length !== 32) throw new TypeError("Invalid X25519 identity.");
  return {
    privateKey,
    publicBytes,
    publicKey: await importPublic(publicBytes),
    publicValue: bytesToB64Url(publicBytes),
    keyId: await keyIdForPublic(publicBytes),
  };
}

async function createIdentity(storage) {
  try {
    const stored = safeJson(storage?.getItem(IDENTITY_STORAGE_KEY));
    if (validPrivateJwk(stored)) return await identityFromJwk(stored);
  } catch (_) {
    // Replace a malformed/unsupported identity below. Never downgrade to
    // plaintext merely because local storage was unavailable.
  }

  const generated = await globalThis.crypto.subtle.generateKey(
    { name: "X25519" },
    true,
    ["deriveBits"],
  );
  const jwk = await globalThis.crypto.subtle.exportKey("jwk", generated.privateKey);
  if (!validPrivateJwk(jwk)) {
    throw new Error("Could not create chat encryption identity.");
  }
  try {
    storage?.setItem(IDENTITY_STORAGE_KEY, JSON.stringify(jwk));
  } catch (_) {
    // Private browsing keeps it for this page lifetime. Closing the page then
    // loses old-history access; encryption still never falls back to plaintext.
  }
  return identityFromJwk(jwk);
}

function contentAdditionalData(roomName, senderKeyId) {
  return encoder.encode(
    `forkmesh-chat-e2ee-content-v${SEAL_VERSION}\n${roomName}\n${senderKeyId}`,
  );
}

function wrapAdditionalData(roomName, senderKeyId, recipientKeyId, bodyNonce) {
  return encoder.encode(
    `forkmesh-chat-e2ee-wrap-v${SEAL_VERSION}\n${roomName}\n${senderKeyId}\n` +
      `${recipientKeyId}\n${bytesToB64Url(bodyNonce)}`,
  );
}

async function pairwiseKey(seal, peer, senderKeyId, recipientKeyId) {
  const shared = new Uint8Array(
    await globalThis.crypto.subtle.deriveBits(
      { name: "X25519", public: peer.publicKey },
      seal.identity.privateKey,
      256,
    ),
  );
  if (!shared.some((byte) => byte !== 0)) {
    throw new TypeError("Invalid X25519 shared key.");
  }
  const material = await globalThis.crypto.subtle.importKey(
    "raw",
    shared,
    "HKDF",
    false,
    ["deriveKey"],
  );
  shared.fill(0);
  return globalThis.crypto.subtle.deriveKey(
    {
      name: "HKDF",
      hash: "SHA-256",
      salt: await sha256(
        encoder.encode(`ForkMesh chat e2ee salt:${seal.roomName}`),
      ),
      info: encoder.encode(
        `forkmesh-chat-e2ee-kek-v${SEAL_VERSION}\n${seal.roomName}\n` +
          `${senderKeyId}\n${recipientKeyId}`,
      ),
    },
    material,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"],
  );
}

async function encryptAes(bytes, key, aad, nonce = randomBytes(12)) {
  const combined = new Uint8Array(
    await globalThis.crypto.subtle.encrypt(
      {
        name: "AES-GCM",
        iv: nonce,
        tagLength: 128,
        additionalData: aad,
      },
      key,
      bytes,
    ),
  );
  return {
    nonce,
    body: combined.slice(0, combined.length - 16),
    tag: combined.slice(combined.length - 16),
  };
}

async function decryptAes(body, tag, nonce, key, aad) {
  const combined = new Uint8Array(body.length + tag.length);
  combined.set(body, 0);
  combined.set(tag, body.length);
  return new Uint8Array(
    await globalThis.crypto.subtle.decrypt(
      {
        name: "AES-GCM",
        iv: nonce,
        tagLength: 128,
        additionalData: aad,
      },
      key,
      combined,
    ),
  );
}

function peerRecord(publicBytes, publicKey, keyId) {
  return {
    publicBytes,
    publicKey,
    publicValue: bytesToB64Url(publicBytes),
    keyId,
  };
}

async function validatedPeer(keyId, publicValue) {
  if (!KEY_ID_RE.test(String(keyId || "")) ||
      !PUBLIC_KEY_RE.test(String(publicValue || ""))) {
    return null;
  }
  try {
    const publicBytes = b64UrlToBytes(publicValue);
    if (publicBytes.length !== 32 ||
        (await keyIdForPublic(publicBytes)) !== keyId) {
      return null;
    }
    return peerRecord(publicBytes, await importPublic(publicBytes), keyId);
  } catch (_) {
    return null;
  }
}

function peerStorageKey(scope, roomName) {
  return (
    PEER_STORAGE_PREFIX +
    encodeURIComponent(String(scope || "")) +
    "." +
    encodeURIComponent(String(roomName || ""))
  );
}

async function loadPeers(storage, scope, roomName, identity) {
  const peers = new Map();
  let records = [];
  try {
    const parsed = safeJson(storage?.getItem(peerStorageKey(scope, roomName)));
    if (Array.isArray(parsed)) records = parsed.slice(-MAX_RECIPIENTS + 1);
  } catch (_) {}
  for (const record of records) {
    const peer = await validatedPeer(record?.kid, record?.pub);
    if (peer && peer.keyId !== identity.keyId) peers.set(peer.keyId, peer);
  }
  return peers;
}

function persistPeers(seal) {
  try {
    seal.storage?.setItem(
      peerStorageKey(seal.scope, seal.roomName),
      JSON.stringify(
        [...seal.peers.values()]
          .slice(-MAX_RECIPIENTS + 1)
          .map((peer) => ({ kid: peer.keyId, pub: peer.publicValue })),
      ),
    );
  } catch (_) {}
}

export function isSealKeyFrame(frame) {
  return Boolean(
    frame &&
      typeof frame === "object" &&
      frame.type === KEY_TYPE &&
      frame.v === SEAL_VERSION &&
      KEY_ID_RE.test(frame.kid || "") &&
      PUBLIC_KEY_RE.test(frame.pub || ""),
  );
}

export function sealKeyFrame(seal) {
  if (!seal?.identity) {
    throw new TypeError("A room encryption identity is required.");
  }
  return {
    type: KEY_TYPE,
    v: SEAL_VERSION,
    kid: seal.identity.keyId,
    pub: seal.identity.publicValue,
  };
}

function rememberPeer(seal, peer) {
  if (!peer || peer.keyId === seal.identity.keyId) return false;
  const isNew = !seal.peers.has(peer.keyId);
  // Refresh insertion order so a bounded public-room recipient set favors
  // devices that are still announcing.
  seal.peers.delete(peer.keyId);
  seal.peers.set(peer.keyId, peer);
  while (seal.peers.size >= MAX_RECIPIENTS) {
    seal.peers.delete(seal.peers.keys().next().value);
  }
  persistPeers(seal);
  return isNew;
}

export async function acceptSealKey(frame, seal) {
  if (!isSealKeyFrame(frame) || !seal?.identity) return false;
  const peer = await validatedPeer(frame.kid, frame.pub);
  return peer ? rememberPeer(seal, peer) : false;
}

export function isSealedFrame(frame) {
  return Boolean(
    frame &&
      typeof frame === "object" &&
      frame.type === SEAL_TYPE &&
      frame.v === SEAL_VERSION &&
      KEY_ID_RE.test(frame.kid || "") &&
      PUBLIC_KEY_RE.test(frame.pub || "") &&
      Array.isArray(frame.keys) &&
      frame.keys.length > 0 &&
      frame.keys.length <= MAX_RECIPIENTS,
  );
}

export function frameKeyId(frame) {
  return isSealedFrame(frame) ? frame.kid : "";
}

export async function sealFrame(plain, seal, roomName = seal?.roomName) {
  const room = String(roomName || "").trim();
  if (!seal?.identity || !room || room !== seal.roomName) {
    throw new TypeError("A room encryption identity is required.");
  }
  const contentBytes = randomBytes(32);
  const contentKey = await globalThis.crypto.subtle.importKey(
    "raw",
    contentBytes,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"],
  );
  const content = await encryptAes(
    encoder.encode(JSON.stringify(plain)),
    contentKey,
    contentAdditionalData(room, seal.identity.keyId),
  );

  const recipients = new Map(seal.peers);
  recipients.set(
    seal.identity.keyId,
    peerRecord(
      seal.identity.publicBytes,
      seal.identity.publicKey,
      seal.identity.keyId,
    ),
  );
  const selected = [...recipients.values()]
    .slice(-MAX_RECIPIENTS)
    .sort((left, right) => left.keyId.localeCompare(right.keyId));
  const keys = await Promise.all(
    selected.map(async (recipient) => {
      const kek = await pairwiseKey(
        seal,
        recipient,
        seal.identity.keyId,
        recipient.keyId,
      );
      const wrapped = await encryptAes(
        contentBytes,
        kek,
        wrapAdditionalData(
          room,
          seal.identity.keyId,
          recipient.keyId,
          content.nonce,
        ),
      );
      return {
        kid: recipient.keyId,
        nonce: bytesToB64(wrapped.nonce),
        tag: bytesToB64(wrapped.tag),
        key: bytesToB64(wrapped.body),
      };
    }),
  );
  contentBytes.fill(0);
  return {
    type: SEAL_TYPE,
    v: SEAL_VERSION,
    kid: seal.identity.keyId,
    pub: seal.identity.publicValue,
    nonce: bytesToB64(content.nonce),
    tag: bytesToB64(content.tag),
    body: bytesToB64(content.body),
    keys,
  };
}

export async function unsealFrame(frame, seal, roomName = seal?.roomName) {
  const room = String(roomName || "").trim();
  if (!isSealedFrame(frame) || !seal?.identity || room !== seal.roomName) {
    return null;
  }
  try {
    const sender = await validatedPeer(frame.kid, frame.pub);
    if (!sender) return null;
    const wrapped = frame.keys.find(
      (entry) => entry && entry.kid === seal.identity.keyId,
    );
    if (!wrapped) return null;
    const bodyNonce = b64ToBytes(frame.nonce);
    const bodyTag = b64ToBytes(frame.tag);
    const body = b64ToBytes(frame.body);
    const keyNonce = b64ToBytes(wrapped.nonce);
    const keyTag = b64ToBytes(wrapped.tag);
    const keyBody = b64ToBytes(wrapped.key);
    if (bodyNonce.length !== 12 || bodyTag.length !== 16 || !body.length ||
        keyNonce.length !== 12 || keyTag.length !== 16 || keyBody.length !== 32) {
      return null;
    }
    const kek = await pairwiseKey(
      seal,
      sender,
      frame.kid,
      seal.identity.keyId,
    );
    const contentBytes = await decryptAes(
      keyBody,
      keyTag,
      keyNonce,
      kek,
      wrapAdditionalData(
        room,
        frame.kid,
        seal.identity.keyId,
        bodyNonce,
      ),
    );
    if (contentBytes.length !== 32) return null;
    const contentKey = await globalThis.crypto.subtle.importKey(
      "raw",
      contentBytes,
      { name: "AES-GCM", length: 256 },
      false,
      ["decrypt"],
    );
    contentBytes.fill(0);
    const clear = await decryptAes(
      body,
      bodyTag,
      bodyNonce,
      contentKey,
      contentAdditionalData(room, frame.kid),
    );
    const value = JSON.parse(decoder.decode(clear));
    // A valid sender envelope doubles as an announcement for future messages.
    rememberPeer(seal, sender);
    return value && typeof value === "object" && !Array.isArray(value)
      ? value
      : null;
  } catch (_) {
    return null;
  }
}

// One device identity and a controller-and-room-local peer set. There is
// deliberately no enable switch and no phrase: resolve() either supplies
// automatic E2EE or throws, causing the transport to fail closed.
export function createRoomSealStore({ storage = browserStorage() } = {}) {
  let identityPromise = null;
  const rooms = new Map();

  async function identity() {
    if (!identityPromise) identityPromise = createIdentity(storage);
    return identityPromise;
  }

  return {
    has(scope) {
      return Boolean(scope);
    },
    async resolve(scope, roomName, controllerScope = "") {
      const room = String(roomName || "").trim();
      if (!scope || !room) throw new TypeError("A chat room is required.");
      // v1 asked the user for a shared phrase. It is obsolete in v2 and should
      // not linger in local storage after this room upgrades.
      try { storage?.removeItem(LEGACY_PHRASE_STORAGE_PREFIX + scope); } catch (_) {}
      const localScope = `${String(controllerScope || "").trim()}\n${scope}`;
      const cacheKey = `${localScope}\n${room}`;
      if (rooms.has(cacheKey)) return rooms.get(cacheKey);
      const mine = await identity();
      const seal = {
        identity: mine,
        roomName: room,
        // Public-key registries are local metadata. Include the controller so
        // switching between deployments that both call a room "world-general"
        // never wraps a key for a device from the other deployment.
        scope: localScope,
        storage,
        peers: await loadPeers(storage, localScope, room, mine),
        get keyId() {
          return mine.keyId;
        },
        get peerCount() {
          return this.peers.size;
        },
        shouldPersistAdvertisement(now = Date.now()) {
          const key =
            ANNOUNCED_STORAGE_PREFIX +
            encodeURIComponent(localScope) +
            "." +
            encodeURIComponent(room);
          let previous = 0;
          try { previous = Number(storage?.getItem(key)) || 0; } catch (_) {}
          if (now - previous < ANNOUNCE_DURABLE_MS) return false;
          try { storage?.setItem(key, String(now)); } catch (_) {}
          return true;
        },
      };
      rooms.set(cacheKey, seal);
      return seal;
    },
  };
}

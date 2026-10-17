import {
  decryptObject,
  derivePassphraseKey,
  encryptObject,
} from "./chat-crypto.js";
import {
  E2EE_RESET_TYPE,
  acceptSealKey,
  authenticatedPlain,
  isSealKeyFrame,
  isSealedFrame,
  sealFrame,
  sealKeyFrame,
  unsealFrame,
} from "./chat-seal.js";
import { moderateChatPlain } from "./chat-moderation.js";

const INITIAL_RECONNECT_MS = 2000;
const MAX_RECONNECT_MS = 30000;
// The relay replays a room's retained backlog to every joining socket: one
// cipher envelope per message, each tagged historyReplay, closed by a
// "forkmesh-history-end" marker. Handed to the consumer frame by frame that
// backlog repaints the log once per message; buffered here it becomes a single
// onHistory batch. FLUSH is the inter-frame gap that stands in for a marker an
// older relay never sends, IDLE the deadline after open for a room that
// replays nothing at all — between them, onHistory fires exactly once per
// connection, so a consumer can hold its loading state until the backlog lands.
const HISTORY_FLUSH_MS = 180;
const HISTORY_IDLE_MS = 2500;
// Retained frame types the relay should wake the room's phones for.
const NOTIFY_TYPES = new Set(["chat", "thread-reply"]);
const RECIPIENT_REFS_MAX = 256;
const RECIPIENT_NAMES_MAX = 20;
const RECIPIENT_NAME = /^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$/;
const RECEIVED_REFS_MAX = 64;
const RECEIVED_FLUSH_MS = 150;
const OUTBOX_MAX = 64;
const OUTBOX_RETAIN_MS = 7 * 24 * 60 * 60 * 1000;
const RESET_INTERVAL_MS = 60 * 1000;
const resetSentAt = new Map();

export function relayServerCopies(envelope) {
  const value = envelope?.stored;
  return Number.isSafeInteger(value) && value >= 0 ? value : null;
}

export function relayFrameRef(envelope) {
  const value = String(envelope?.ref || "");
  return /^[0-9a-f]{40}$/.test(value) ? value : "";
}

export function relayRefList(envelope) {
  if (!Array.isArray(envelope?.refs)) return [];
  return envelope.refs
    .filter((ref) => typeof ref === "string" && /^[0-9a-f]{40}$/.test(ref))
    .slice(0, RECEIVED_REFS_MAX);
}

export function relayRecipientCount(envelope) {
  const value = envelope?.recipients;
  return Number.isSafeInteger(value) && value >= 0 ? value : null;
}

export function relayRecipientNames(envelope) {
  const value = envelope?.recipientNames;
  if (!Array.isArray(value)) return [];
  return value
    .filter((name) => typeof name === "string" && RECIPIENT_NAME.test(name))
    .slice(0, RECIPIENT_NAMES_MAX);
}

export function withRelayRecipients(plain, envelope) {
  if (!plain || typeof plain !== "object") return plain;
  const {
    relayRecipients: _forged,
    relayRecipientNames: _forgedNames,
    ...rest
  } = plain;
  const count = relayRecipientCount(envelope);
  if (count === null) return rest;
  const names = relayRecipientNames(envelope);
  return names.length
    ? { ...rest, relayRecipients: count, relayRecipientNames: names }
    : { ...rest, relayRecipients: count };
}

export async function frameRef(text) {
  const digest = await globalThis.crypto.subtle.digest(
    "SHA-256",
    new TextEncoder().encode(String(text)),
  );
  return Array.from(new Uint8Array(digest), (byte) =>
    byte.toString(16).padStart(2, "0")).join("").slice(0, 40);
}

function websocketUrl(value, locationLike) {
  const path = String(value || "");
  if (/^wss?:\/\//i.test(path)) return path;
  const scheme = locationLike.protocol === "https:" ? "wss:" : "ws:";
  return new URL(path, `${scheme}//${locationLike.host}`).href;
}

function roomKeyForAccess(access) {
  if (access?.key) return Promise.resolve(access.key);
  if (!access?.passphrase || !access?.roomName) {
    return Promise.reject(new Error("Room access did not include encryption data"));
  }
  return derivePassphraseKey(access.passphrase, access.roomName);
}

export function createChatRoomTransport({
  authorize,
  // Public channels intentionally let controller-side integrations read and
  // author application frames. Everything else remains fail-closed behind the
  // device-to-device seal. This decision comes from authenticated channel
  // metadata, never from a frame supplied by the relay.
  allowsControllerAccess = () => false,
  isDirectRoom = () => false,
  // Resolves this device's automatic E2EE identity and the public keys it has
  // learned for the room. It is deliberately separate from authorize(): the
  // relay answers authorize(), but no relay response may ever produce a local
  // private key. A missing identity fails closed instead of sending plaintext.
  resolveSeal = () => null,
  onPlain = () => {},
  onHistory = () => {},
  onState = () => {},
  // Called with the key id of a sealed frame this client cannot open, so a
  // consumer can say why a sealed room looks empty instead of dropping it in
  // silence.
  onSealed = () => {},
  onTrust = () => {},
  onRecipients = () => {},
  onServerCopies = () => {},
  onDelivered = () => {},
  onRead = () => {},
  onMedia = () => {},
  holdReceipt = () => false,
  WebSocketImpl = globalThis.WebSocket,
  locationLike = globalThis.location,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
} = {}) {
  if (typeof authorize !== "function") {
    throw new TypeError("createChatRoomTransport requires authorize(room)");
  }
  if (typeof WebSocketImpl !== "function") {
    throw new TypeError("WebSocket is unavailable");
  }

  let currentRoom = null;
  let currentAccess = null;
  let currentKey = null;
  let currentSeal = null;
  let currentControllerReadable = false;
  let socket = null;
  let timer = null;
  let generation = 0;
  let reconnectDelayMs = INITIAL_RECONNECT_MS;
  let transportState = "idle";
  let suspended = false;
  let disposed = false;
  const recipientRefs = new Map();
  let outbox = [];
  let outboxRoom = null;
  let releaseHeldReceipt = () => false;
  let deliverReleased = async () => {};
  let afterTrustChange = () => {};

  function rememberOutbox(ref, plain, sealed, notify) {
    if (!ref || !plain || !Array.isArray(sealed?.keys)) return;
    const id = plain.id ? String(plain.id) : "";
    outbox = outbox.filter(
      (entry) => entry.ref !== ref && !(id && entry.id === id),
    );
    outbox.push({
      ref,
      id,
      plain,
      notify: Boolean(notify),
      keyIds: new Set(sealed.keys.map((entry) => entry?.kid)),
      ts: Number.isFinite(plain.ts) && plain.ts > 0 ? plain.ts : Date.now(),
    });
    while (outbox.length > OUTBOX_MAX) outbox.shift();
  }

  function rememberRecipientRef(ref, id) {
    recipientRefs.delete(ref);
    recipientRefs.set(ref, id);
    while (recipientRefs.size > RECIPIENT_REFS_MAX) {
      recipientRefs.delete(recipientRefs.keys().next().value);
    }
  }

  function refsForIds(ids) {
    const wanted = new Set(ids.map((id) => String(id || "")).filter(Boolean));
    const refs = [];
    for (const [ref, id] of recipientRefs) {
      if (wanted.has(id)) refs.push(ref);
    }
    for (const entry of outbox) {
      if (wanted.has(entry.id) && !refs.includes(entry.ref)) refs.push(entry.ref);
    }
    return { wanted, refs };
  }

  function transition(next, detail = {}) {
    transportState = next;
    onState(next, { ...detail, room: currentRoom });
  }

  function clearReconnect() {
    if (timer === null) return;
    clearTimeoutImpl(timer);
    timer = null;
  }

  function detachSocket(code, reason) {
    const previous = socket;
    socket = null;
    currentKey = null;
    currentSeal = null;
    currentControllerReadable = false;
    currentAccess = null;
    if (!previous) return;
    try {
      previous.close(code, reason);
    } catch (_) {}
  }

  function scheduleReconnect() {
    if (disposed || suspended || timer !== null || !currentRoom) return;
    transition("reconnecting", { delayMs: reconnectDelayMs });
    const delay = reconnectDelayMs;
    reconnectDelayMs = Math.min(reconnectDelayMs * 2, MAX_RECONNECT_MS);
    timer = setTimeoutImpl(() => {
      timer = null;
      openCurrentRoom(true);
    }, delay);
  }

  async function openCurrentRoom(isReconnect = false) {
    if (disposed || suspended || !currentRoom) return false;
    clearReconnect();
    const attempt = ++generation;
    const room = currentRoom;
    if (room !== outboxRoom) {
      outbox = [];
      outboxRoom = room;
    }
    const direct = Boolean(isDirectRoom(room));
    detachSocket(1000, isReconnect ? "reconnecting" : "room changed");
    transition("authorizing");

    let access;
    let key;
    let seal;
    let controllerReadable = false;
    try {
      access = await authorize(room);
      if (attempt !== generation || disposed || suspended || room !== currentRoom) {
        return false;
      }
      key = await roomKeyForAccess(access);
      controllerReadable = Boolean(await allowsControllerAccess(room, access));
      // Resolved per connection, from the room name the relay just named, so
      // re-keying a protected channel binds the automatic E2EE layer to the
      // new room. Public rooms still resolve it when possible so clients can
      // open retained frames written before a channel became public.
      try {
        seal = (await resolveSeal(room, access?.roomName)) || null;
      } catch (error) {
        if (!controllerReadable) throw error;
        seal = null;
      }
      if (!seal && !controllerReadable) {
        throw new Error("End-to-end chat encryption is unavailable.");
      }
      if (attempt !== generation || disposed || suspended || room !== currentRoom) {
        return false;
      }
    } catch (error) {
      if (attempt !== generation || disposed || suspended) return false;
      transition(error?.code === "auth" ? "unauthorized" : "unavailable", { error });
      if (error?.code !== "auth") scheduleReconnect();
      return false;
    }

    currentAccess = access;
    currentKey = key;
    currentSeal = seal;
    currentControllerReadable = controllerReadable;
    transition("connecting");
    let roomSocket;
    try {
      roomSocket = new WebSocketImpl(
        websocketUrl(access.webSocketUrl, locationLike),
      );
    } catch (error) {
      currentAccess = null;
      currentKey = null;
      currentSeal = null;
      currentControllerReadable = false;
      transition("unavailable", { error });
      scheduleReconnect();
      return false;
    }
    socket = roomSocket;

    // Backlog buffer and inbound queue are per-connection: a reconnect replays
    // the room's history again and starts its own batch.
    let replayEnvelopes = [];
    let replayTimer = null;
    let replayDelivered = false;
    let inboundQueue = Promise.resolve();
    let keyReplyPending = false;
    let receivedRefs = [];
    let receivedTimer = null;
    const heldReceipts = new Map();
    let mismatchAdvertised = false;
    let resealChain = Promise.resolve();

    function stale() {
      return attempt !== generation || roomSocket !== socket;
    }

    function flushReceived() {
      receivedTimer = null;
      const refs = receivedRefs;
      receivedRefs = [];
      if (stale() || roomSocket.readyState !== WebSocketImpl.OPEN) return;
      for (let start = 0; start < refs.length; start += RECEIVED_REFS_MAX) {
        roomSocket.send(JSON.stringify({
          kind: "forkmesh-received",
          v: 1,
          refs: refs.slice(start, start + RECEIVED_REFS_MAX),
        }));
      }
    }

    function noteReceived(envelope) {
      const ref = relayFrameRef(envelope);
      if (!ref || receivedRefs.includes(ref)) return;
      receivedRefs.push(ref);
      if (receivedTimer === null) {
        receivedTimer = setTimeoutImpl(flushReceived, RECEIVED_FLUSH_MS);
      }
    }

    async function sendKeyAdvertisement(persist = false) {
      if (!seal || stale() || roomSocket.readyState !== WebSocketImpl.OPEN) {
        return false;
      }
      const rotationBefore = seal.rotation;
      const bundle = await sealKeyFrame(seal);
      const envelope = await encryptObject(bundle, key);
      if (persist || seal.rotation !== rotationBefore) envelope.persist = true;
      if (stale() || roomSocket.readyState !== WebSocketImpl.OPEN) return false;
      roomSocket.send(JSON.stringify(envelope));
      return true;
    }

    async function resealNow() {
      if (!direct || !seal || stale() ||
          roomSocket.readyState !== WebSocketImpl.OPEN) {
        return;
      }
      const cutoff = Date.now() - OUTBOX_RETAIN_MS;
      const known = seal.recipientKeyIds();
      const pending = [];
      const kept = [];
      for (const entry of outbox) {
        if (entry.ts < cutoff) continue;
        if (known.some((keyId) => !entry.keyIds.has(keyId))) {
          pending.push(entry);
        } else {
          kept.push(entry);
        }
      }
      outbox = kept;
      const superseded = [];
      for (const entry of pending) {
        const sealed = await sealFrame(entry.plain, seal, access?.roomName);
        const envelope = await encryptObject(sealed, key);
        envelope.persist = true;
        if (entry.notify) envelope.notify = true;
        envelope.resend = true;
        const text = JSON.stringify(envelope);
        const ref = await frameRef(text);
        if (stale() || roomSocket.readyState !== WebSocketImpl.OPEN) return;
        roomSocket.send(text);
        if (entry.notify && entry.id) rememberRecipientRef(ref, entry.id);
        rememberOutbox(ref, entry.plain, sealed, entry.notify);
        superseded.push(entry.ref);
      }
      for (let start = 0; start < superseded.length; start += RECEIVED_REFS_MAX) {
        roomSocket.send(JSON.stringify({
          kind: "forkmesh-superseded",
          v: 1,
          refs: superseded.slice(start, start + RECEIVED_REFS_MAX),
        }));
      }
    }

    function scheduleReseal() {
      if (!direct) return resealChain;
      resealChain = resealChain.then(resealNow).catch(() => {});
      return resealChain;
    }

    async function resealFor(kid) {
      if (!direct || !seal || stale() ||
          roomSocket.readyState !== WebSocketImpl.OPEN) {
        return;
      }
      const cutoff = Date.now() - OUTBOX_RETAIN_MS;
      for (const entry of [...outbox]) {
        if (entry.ts < cutoff) continue;
        const sealed = await sealFrame(entry.plain, seal, access?.roomName, { only: [kid] });
        if (!sealed.keys.length) continue;
        const envelope = await encryptObject(sealed, key);
        envelope.persist = true;
        if (entry.notify) envelope.notify = true;
        envelope.resend = true;
        const text = JSON.stringify(envelope);
        const ref = await frameRef(text);
        if (stale() || roomSocket.readyState !== WebSocketImpl.OPEN) return;
        roomSocket.send(text);
        if (entry.notify && entry.id) rememberRecipientRef(ref, entry.id);
      }
    }

    function scheduleResealFor(kid) {
      if (!direct) return resealChain;
      resealChain = resealChain.then(() => resealFor(kid)).catch(() => {});
      return resealChain;
    }

    async function resetSession(kid) {
      if (!direct || !seal || stale() || !kid) return;
      const tag = `${access?.roomName || ""}\n${kid}`;
      const now = Date.now();
      if (now - (resetSentAt.get(tag) || 0) < RESET_INTERVAL_MS) return;
      for (const [old, at] of resetSentAt) if (now - at >= RESET_INTERVAL_MS) resetSentAt.delete(old);
      resetSentAt.set(tag, now);
      await seal.resetPeer(kid);
      const sealed = await sealFrame(
        { type: E2EE_RESET_TYPE, senderId: seal.node },
        seal,
        access?.roomName,
        { only: [kid] },
      );
      if (sealed.keys.length) {
        const envelope = await encryptObject(sealed, key);
        envelope.persist = true;
        if (stale() || roomSocket.readyState !== WebSocketImpl.OPEN) return;
        roomSocket.send(JSON.stringify(envelope));
      }
      await sendKeyAdvertisement(true);
    }

    function replyWithKeySoon() {
      if (keyReplyPending || stale()) return;
      keyReplyPending = true;
      setTimeoutImpl(() => {
        keyReplyPending = false;
        sendKeyAdvertisement(false).catch(() => {});
      }, 25);
    }

    // The one place a room-layer plaintext becomes an application frame.
    // Protected rooms accept only recipient-sealed frames; public rooms also
    // accept ordinary frames so ForkBot, MCP, and webhooks can participate.
    //
    // Every frame this drops takes onSealed with it, because all three ways a
    // room can be out of step look identical to a reader — an empty room:
    //   kid  -> the content key was not wrapped for this device
    //   ""   -> the frame was not recipient-sealed at all
    releaseHeldReceipt = (id) => {
      const envelope = heldReceipts.get(String(id || ""));
      if (!envelope || stale()) return false;
      heldReceipts.delete(String(id));
      noteReceived(envelope);
      return true;
    };

    deliverReleased = async (results) => {
      if (stale() || !results?.length) return;
      const plains = [];
      for (const result of results) {
        const plain = authenticatedPlain(result);
        if (plain?.type === E2EE_RESET_TYPE) {
          if (direct) scheduleResealFor(result.kid);
        } else if (plain) {
          plains.push(await moderateChatPlain(plain));
        }
      }
      if (plains.length && !stale()) onHistory(plains, room, access);
    };

    afterTrustChange = () => {
      if (stale()) return;
      scheduleReseal();
    };

    async function openFrame(envelope, history = false) {
      const { plain = null, ack = Boolean(plain) } = await openFramePlain(envelope, history) || {};
      if (plain && ack && direct && plain.id && relayFrameRef(envelope) &&
          holdReceipt(plain, room)) {
        heldReceipts.set(String(plain.id), envelope);
      } else if (ack) {
        noteReceived(envelope);
      }
      if (plain?.id && direct && relayFrameRef(envelope)) {
        rememberRecipientRef(relayFrameRef(envelope), String(plain.id));
      }
      return withRelayRecipients(plain, envelope);
    }

    async function openFramePlain(envelope, history = false) {
      const framed = await decryptObject(envelope, key);
      if (!framed) return null;
      if (isSealKeyFrame(framed)) {
        const accepted = seal ? await acceptSealKey(framed, seal) : null;
        if (accepted?.isNew) replyWithKeySoon();
        if (accepted?.state === "pending") {
          onTrust({ type: "pending", kid: accepted.kid, acct: accepted.acct }, room);
        }
        if (accepted && (accepted.isNew || accepted.refreshed) && accepted.state === "approved") {
          scheduleReseal();
        }
        if (accepted?.released?.length) await deliverReleased(accepted.released);
        return null;
      }
      if (framed.type === "sealed" || framed.type === "e2ee-key") {
        if (!isSealedFrame(framed)) return null;
      } else if (controllerReadable) {
        return { plain: framed };
      } else {
        onSealed("", room);
        return null;
      }
      const trustBefore = seal ? seal.trustVersion : 0;
      const result = seal
        ? await unsealFrame(framed, seal, access?.roomName, { history })
        : { status: "notForMe" };
      if (result.status === "held") {
        if (result.pending) onTrust({ type: "held", kid: result.kid }, room);
        return null;
      }
      if (result.status === "own") {
        const own = result.plain ? authenticatedPlain(result) : null;
        return own ? { plain: own, ack: false } : null;
      }
      if (result.status === "noSession") {
        if (!result.later) resetSession(result.kid).catch(() => {});
        return null;
      }
      if (result.status === "replay" || result.status === "invalid") return null;
      const plain = result.status === "opened" ? authenticatedPlain(result) : null;
      if (!plain) {
        if (result.status !== "notForMe") return null;
        onSealed(framed.kid, room);
        if (direct && seal && !mismatchAdvertised) {
          mismatchAdvertised = true;
          sendKeyAdvertisement(true).catch(() => {});
        }
        return null;
      }
      if (plain.type === E2EE_RESET_TYPE) {
        if (direct) scheduleResealFor(result.kid);
        return { plain: null, ack: true };
      }
      if (direct) {
        if (framed.kid === seal.keyId && envelope.persist === true) {
          rememberOutbox(
            relayFrameRef(envelope),
            plain,
            framed,
            envelope.notify === true,
          );
        }
        if (seal.trustVersion !== trustBefore) scheduleReseal();
      }
      return { plain };
    }

    function cancelReplayTimer() {
      if (replayTimer === null) return;
      clearTimeoutImpl(replayTimer);
      replayTimer = null;
    }

    async function flushReplay() {
      cancelReplayTimer();
      if (replayDelivered) return;
      replayDelivered = true;
      const envelopes = replayEnvelopes;
      replayEnvelopes = [];
      const frames = await Promise.all(
        envelopes.map((envelope) => openFrame(envelope, true)),
      );
      if (stale()) return;
      const plains = [];
      for (const frame of frames) {
        if (frame) plains.push(await moderateChatPlain(frame));
      }
      if (stale()) return;
      onHistory(plains, room, access);
    }

    function scheduleReplayFlush(delayMs) {
      cancelReplayTimer();
      if (replayDelivered) return;
      replayTimer = setTimeoutImpl(() => {
        replayTimer = null;
        inboundQueue = inboundQueue.then(flushReplay).catch(() => {});
      }, delayMs);
    }

    async function handleFrame(data) {
      if (typeof data !== "string") return;
      let envelope;
      try {
        envelope = JSON.parse(data);
      } catch (_) {
        return;
      }
      if (envelope?.kind === "forkmesh-history-end") {
        await flushReplay();
        await scheduleReseal();
        return;
      }
      if (envelope?.kind === "forkmesh-server-copies") {
        const stored = relayServerCopies(envelope);
        if (stored === 0) outbox = [];
        if (stored !== null) onServerCopies(stored, room);
        return;
      }
      if (envelope?.kind === "forkmesh-recipients") {
        const id = recipientRefs.get(String(envelope.ref || ""));
        const count = relayRecipientCount(envelope);
        if (id && count !== null) {
          onRecipients(id, count, room, relayRecipientNames(envelope));
        }
        return;
      }
      if (envelope?.kind === "forkmesh-delivered") {
        const ids = relayRefList(envelope)
          .map((ref) => recipientRefs.get(ref))
          .filter(Boolean);
        if (direct && ids.length) onDelivered([...new Set(ids)], room);
        return;
      }
      if (envelope?.kind === "forkmesh-read") {
        if (direct) onRead(room);
        return;
      }
      if (envelope?.kind === "forkmesh-media") {
        const id = String(envelope.id || "");
        const state = String(envelope.state || "");
        if (direct && /^[0-9a-f]{32}$/.test(id) &&
            ["opened", "expired", "withdrawn"].includes(state)) {
          onMedia(id, state, room);
        }
        return;
      }
      if (envelope?.historyReplay === true && !replayDelivered) {
        replayEnvelopes.push(envelope);
        scheduleReplayFlush(HISTORY_FLUSH_MS);
        return;
      }
      // A live frame that overtook the end-of-backlog marker must not paint
      // ahead of the backlog it belongs after.
      if (!replayDelivered) await flushReplay();
      const plain = await openFrame(envelope);
      if (!plain || stale()) return;
      onPlain(await moderateChatPlain(plain), room, access);
    }

    roomSocket.addEventListener("open", () => {
      if (stale()) {
        try {
          roomSocket.close(1000, "stale room connection");
        } catch (_) {}
        return;
      }
      reconnectDelayMs = INITIAL_RECONNECT_MS;
      scheduleReplayFlush(HISTORY_IDLE_MS);
      transition("connected", { access });
      // Public-key discovery is automatic. One small announcement per connect
      // reaches live peers; at most once a day it is retained so an offline
      // room member can learn this device before writing its next message.
      if (seal) {
        sendKeyAdvertisement(
          Boolean(seal.shouldPersistAdvertisement?.()),
        ).catch(() => {});
      }
    });

    // Frames are processed strictly in arrival order. Each one decrypts
    // asynchronously, so without this queue a short frame can overtake a long
    // one and land a message out of order — which costs the consumer a full
    // re-render to put the timeline back in sequence.
    roomSocket.addEventListener("message", (event) => {
      if (stale()) return;
      const data = event.data;
      inboundQueue = inboundQueue.then(() => handleFrame(data)).catch(() => {});
    });

    roomSocket.addEventListener("close", (event = {}) => {
      if (stale()) return;
      cancelReplayTimer();
      socket = null;
      currentAccess = null;
      currentKey = null;
      if (disposed || suspended) return;
      if (Number(event.code) === 1008) {
        transition("unauthorized", { closeCode: 1008 });
        return;
      }
      scheduleReconnect();
    });

    roomSocket.addEventListener("error", (error) => {
      if (stale()) return;
      cancelReplayTimer();
      transition("unavailable", { error });
      try {
        roomSocket.close();
      } catch (_) {}
    });
    return true;
  }

  return {
    get state() {
      return transportState;
    },
    get room() {
      return currentRoom;
    },
    get access() {
      return currentAccess;
    },
    get connected() {
      return Boolean(
        socket && socket.readyState === WebSocketImpl.OPEN && currentKey,
      );
    },
    // Whether this connection is writing through device-to-device encryption.
    // Consumers use it for the lock they show; no private/content key is ever
    // exposed through this API.
    get sealed() {
      return Boolean(currentSeal && !currentControllerReadable);
    },
    get controllerReadable() {
      return currentControllerReadable;
    },
    get sealKeyId() {
      return currentSeal?.keyId || "";
    },
    get trustPolicy() {
      return currentSeal?.policy || "";
    },
    pendingDevices() {
      return currentSeal ? currentSeal.pendingDevices() : [];
    },
    devices() {
      return currentSeal ? currentSeal.devices() : [];
    },
    async approveDevice(kid) {
      const seal = currentSeal;
      if (!seal) return false;
      const released = await seal.approve(String(kid || ""));
      if (seal !== currentSeal) return true;
      await deliverReleased(released);
      afterTrustChange();
      return true;
    },
    async blockDevice(kid) {
      const seal = currentSeal;
      if (!seal) return false;
      await seal.block(String(kid || ""));
      return true;
    },
    async safetyNumber() {
      return currentSeal && currentSeal.policy === "strict"
        ? currentSeal.safetyNumber()
        : null;
    },
    async markVerified() {
      if (!currentSeal) return false;
      await currentSeal.markVerified();
      return true;
    },
    releaseReceipt(id) {
      return releaseHeldReceipt(id);
    },
    async connect(room) {
      if (!room || typeof room !== "object") {
        throw new TypeError("connect(room) requires a room descriptor");
      }
      disposed = false;
      suspended = false;
      currentRoom = room;
      reconnectDelayMs = INITIAL_RECONNECT_MS;
      return openCurrentRoom(false);
    },
    async send(plain, { persist = false } = {}) {
      if (!this.connected) return false;
      const activeSocket = socket;
      const seal = currentSeal;
      const room = currentRoom;
      const moderated = await moderateChatPlain(plain);
      const rotationBefore = seal ? seal.rotation : 0;
      const sealed = currentControllerReadable
        ? null
        : await sealFrame(moderated, seal, currentAccess?.roomName);
      if (sealed && seal.rotation !== rotationBefore) {
        const advertiseKey = currentKey;
        const advertiseSocket = socket;
        encryptObject(await sealKeyFrame(seal), advertiseKey).then((advert) => {
          advert.persist = true;
          if (advertiseSocket === socket && advertiseSocket?.readyState === WebSocketImpl.OPEN) {
            advertiseSocket.send(JSON.stringify(advert));
          }
        }).catch(() => {});
      }
      // persist is decided from the frame the caller wrote, not the sealed
      // wrapper, so retention keeps working in a room whose types the relay
      // can no longer read.
      const envelope = await encryptObject(sealed || moderated, currentKey);
      if (persist) envelope.persist = true;
      // The relay only sees ciphertext, and edits, reactions and key
      // advertisements are retained too. Marking the frames a person would
      // want to hear about is what lets it wake the room's phones for a
      // message and for nothing else.
      if (persist && NOTIFY_TYPES.has(plain && plain.type)) {
        envelope.notify = true;
      }
      const text = JSON.stringify(envelope);
      const ref = persist ? await frameRef(text) : "";
      if (persist && plain?.id) rememberRecipientRef(ref, String(plain.id));
      if (activeSocket !== socket || !this.connected) return false;
      activeSocket.send(text);
      if (persist && sealed && room === outboxRoom && isDirectRoom(room)) {
        rememberOutbox(ref, moderated, sealed, envelope.notify === true);
      }
      return true;
    },
    refsFor(ids) {
      return refsForIds(Array.isArray(ids) ? ids : [ids]).refs;
    },
    supersede(ids) {
      const { wanted, refs } = refsForIds(Array.isArray(ids) ? ids : [ids]);
      outbox = outbox.filter((entry) => !wanted.has(entry.id));
      if (!refs.length || !this.connected || !isDirectRoom(currentRoom)) {
        return false;
      }
      for (let start = 0; start < refs.length; start += RECEIVED_REFS_MAX) {
        socket.send(JSON.stringify({
          kind: "forkmesh-superseded",
          v: 1,
          refs: refs.slice(start, start + RECEIVED_REFS_MAX),
        }));
      }
      return true;
    },
    suspend() {
      if (disposed) return;
      suspended = true;
      generation += 1;
      clearReconnect();
      detachSocket(1000, "room suspended");
      transition("suspended");
    },
    resume() {
      if (disposed || !currentRoom || !suspended) return Promise.resolve(false);
      suspended = false;
      return openCurrentRoom(false);
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      suspended = false;
      generation += 1;
      clearReconnect();
      detachSocket(1000, "transport disposed");
      currentRoom = null;
      transition("disposed");
    },
  };
}

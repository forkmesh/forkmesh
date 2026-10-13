import {
  b64ToBytes,
  b64UrlToBytes,
  bytesToB64,
  bytesToB64Url,
} from "./chat-crypto.js";

export const VIEW_ONCE_PART_BYTES = 512 * 1024;
export const VIEW_ONCE_TAG_BYTES = 16;
export const VIEW_ONCE_TTL_MS = 7 * 24 * 60 * 60 * 1000;
export const VIEW_ONCE_KINDS = Object.freeze({
  video: Object.freeze({
    maxBytes: 12 * 1024 * 1024,
    maxDurationMs: 60 * 1000,
    label: "Video message",
    text: "🎥 Video message",
  }),
  voice: Object.freeze({
    maxBytes: 4 * 1024 * 1024,
    maxDurationMs: 120 * 1000,
    label: "Voice message",
    text: "🎤 Voice message",
  }),
});
export const VIEW_ONCE_STATES = Object.freeze([
  "uploading", "sent", "ready", "opening", "playing", "opened", "expired",
  "withdrawn", "failed",
]);

const MEDIA_ID_RE = /^[0-9a-f]{32}$/;
const KEY_RE = /^[A-Za-z0-9_-]{43}=?$/;
const IV_RE = /^[A-Za-z0-9_-]{16}$/;
const MIME_RE =
  /^(?:video\/(?:webm|mp4|quicktime)|audio\/(?:webm|ogg|mp4|mpeg|aac|x-m4a))(?:;[a-z0-9=.,+ "-]{0,80})?$/;
const FINAL_STATES = new Set(["opened", "expired", "withdrawn"]);

export function viewOnceParts(size) {
  return Math.ceil(Math.max(0, Number(size) || 0) / VIEW_ONCE_PART_BYTES);
}

export function viewOnceMime(value) {
  const mime = String(value || "").trim().toLowerCase();
  return MIME_RE.test(mime) ? mime : "";
}

export function viewOnceFromEntry(entry) {
  const value = entry && entry.viewOnce;
  if (!value || typeof value !== "object" || Number(value.v) !== 1) return null;
  const kind = String(value.kind || "");
  const limits = VIEW_ONCE_KINDS[kind];
  if (!limits) return null;
  const media = String(value.media || "").toLowerCase();
  const key = String(value.key || "");
  const iv = String(value.iv || "");
  const mime = viewOnceMime(value.mime);
  const size = Number(value.size);
  const parts = Number(value.parts);
  const durationMs = Math.max(0, Math.round(Number(value.durationMs) || 0));
  if (!MEDIA_ID_RE.test(media) || !KEY_RE.test(key) || !IV_RE.test(iv)) {
    return null;
  }
  if (!mime || mime.startsWith(kind === "video" ? "audio/" : "video/")) {
    return null;
  }
  if (!Number.isInteger(size) || size <= VIEW_ONCE_TAG_BYTES ||
      size > limits.maxBytes + 1024) {
    return null;
  }
  if (parts !== viewOnceParts(size)) return null;
  return {
    v: 1,
    kind,
    media,
    key,
    iv,
    mime,
    durationMs: Math.min(durationMs, limits.maxDurationMs + 1000),
    size,
    parts,
  };
}

export function viewOnceLabel(kind) {
  return VIEW_ONCE_KINDS[kind]?.label || "Message";
}

export function viewOnceDuration(ms) {
  const seconds = Math.max(0, Math.round((Number(ms) || 0) / 1000));
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
}

export async function encryptViewOnceMedia(bytes, cryptoImpl = globalThis.crypto) {
  const keyBytes = cryptoImpl.getRandomValues(new Uint8Array(32));
  const iv = cryptoImpl.getRandomValues(new Uint8Array(12));
  const key = await cryptoImpl.subtle.importKey(
    "raw", keyBytes, { name: "AES-GCM" }, false, ["encrypt"]);
  const ciphertext = new Uint8Array(await cryptoImpl.subtle.encrypt(
    { name: "AES-GCM", iv, tagLength: 128 }, key, bytes));
  const result = {
    ciphertext,
    key: bytesToB64Url(keyBytes),
    iv: bytesToB64Url(iv),
  };
  keyBytes.fill(0);
  return result;
}

export async function decryptViewOnceMedia(
    ciphertext, keyText, ivText, cryptoImpl = globalThis.crypto) {
  const keyBytes = b64UrlToBytes(keyText);
  const iv = b64UrlToBytes(ivText);
  if (keyBytes.length !== 32 || iv.length !== 12) {
    throw new Error("view_once_key_invalid");
  }
  const key = await cryptoImpl.subtle.importKey(
    "raw", keyBytes, { name: "AES-GCM" }, false, ["decrypt"]);
  keyBytes.fill(0);
  return new Uint8Array(await cryptoImpl.subtle.decrypt(
    { name: "AES-GCM", iv, tagLength: 128 }, key, ciphertext));
}

export function splitViewOnceParts(bytes) {
  const array = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const parts = [];
  for (let start = 0; start < array.length; start += VIEW_ONCE_PART_BYTES) {
    parts.push(array.subarray(start, start + VIEW_ONCE_PART_BYTES));
  }
  return parts;
}

export function createViewOnceClient({
  request,
  endpointFor = (conversationId) =>
    `/api/chat/direct-messages/${conversationId}/media`,
  cryptoImpl = globalThis.crypto,
} = {}) {
  if (typeof request !== "function") {
    throw new TypeError("createViewOnceClient requires request(path, options)");
  }

  function json(path, method, body) {
    const options = { method };
    if (body !== undefined) options.body = JSON.stringify(body);
    return request(path, options);
  }

  async function upload(conversationId, kind, blob, durationMs, {
    mime = "",
    onProgress = () => {},
  } = {}) {
    const limits = VIEW_ONCE_KINDS[kind];
    if (!limits) throw new Error("invalid_kind");
    const type = viewOnceMime(mime || blob?.type);
    if (!type) throw new Error("unsupported_media_type");
    const plain = new Uint8Array(await blob.arrayBuffer());
    if (!plain.length) throw new Error("empty_media");
    if (plain.length > limits.maxBytes) throw new Error("media_too_large");
    const sealed = await encryptViewOnceMedia(plain, cryptoImpl);
    plain.fill(0);
    const base = endpointFor(conversationId);
    const parts = splitViewOnceParts(sealed.ciphertext);
    const duration = Math.min(
      Math.max(0, Math.round(Number(durationMs) || 0)), limits.maxDurationMs);
    const created = await json(base, "POST", {
      kind,
      size: sealed.ciphertext.length,
      parts: parts.length,
      durationMs: duration,
    });
    const mediaId = String(created?.media?.id || "");
    if (!MEDIA_ID_RE.test(mediaId)) throw new Error("invalid_media_id");
    try {
      for (let index = 0; index < parts.length; index += 1) {
        await json(`${base}/${mediaId}/parts/${index}`, "PUT", {
          data: bytesToB64(parts[index]),
        });
        onProgress((index + 1) / parts.length);
      }
      await json(`${base}/${mediaId}/finish`, "POST", {});
    } catch (error) {
      json(`${base}/${mediaId}`, "DELETE").catch(() => {});
      throw error;
    } finally {
      sealed.ciphertext.fill(0);
    }
    return {
      v: 1,
      kind,
      media: mediaId,
      key: sealed.key,
      iv: sealed.iv,
      mime: type,
      durationMs: duration,
      size: sealed.ciphertext.length,
      parts: parts.length,
    };
  }

  async function download(conversationId, viewOnce, { onProgress = () => {} } = {}) {
    const base = `${endpointFor(conversationId)}/${viewOnce.media}/parts/`;
    const output = new Uint8Array(viewOnce.size);
    let offset = 0;
    for (let index = 0; index < viewOnce.parts; index += 1) {
      const part = await request(base + index, { method: "GET" });
      const bytes = b64ToBytes(String(part?.data || ""));
      if (offset + bytes.length > output.length) {
        throw new Error("view_once_size_mismatch");
      }
      output.set(bytes, offset);
      offset += bytes.length;
      bytes.fill(0);
      onProgress((index + 1) / viewOnce.parts);
    }
    if (offset !== output.length) throw new Error("view_once_size_mismatch");
    try {
      return await decryptViewOnceMedia(
        output, viewOnce.key, viewOnce.iv, cryptoImpl);
    } finally {
      output.fill(0);
    }
  }

  function consume(conversationId, mediaId) {
    return json(`${endpointFor(conversationId)}/${mediaId}/consume`, "POST", {});
  }

  function withdraw(conversationId, mediaId) {
    return json(`${endpointFor(conversationId)}/${mediaId}`, "DELETE");
  }

  async function status(conversationId, ids) {
    const clean = [...new Set((ids || []).map(String))]
      .filter((id) => MEDIA_ID_RE.test(id))
      .slice(0, 50);
    if (!clean.length) return new Map();
    const result = await request(
      `${endpointFor(conversationId)}?ids=${clean.join(",")}`, { method: "GET" });
    const states = new Map();
    for (const item of result?.media || []) {
      if (MEDIA_ID_RE.test(String(item?.id || ""))) {
        states.set(String(item.id), item);
      }
    }
    return states;
  }

  return { upload, download, consume, withdraw, status };
}

export function viewOnceStateAfterStatus(record, item) {
  const state = String(item?.state || "");
  if (FINAL_STATES.has(state)) return state;
  if (state === "ready") return record.self ? "sent" : "ready";
  if (state === "uploading") return record.self ? "uploading" : "expired";
  return "";
}

export function forgetViewOnceKey(record) {
  if (record?.viewOnce) {
    record.viewOnce = { ...record.viewOnce, key: "", iv: "" };
  }
}

export function isFinalViewOnceState(state) {
  return FINAL_STATES.has(String(state || ""));
}

const ICONS = {
  video: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="6" width="13" height="12" rx="2"></rect><path d="m16 10 5-3v10l-5-3z"></path></svg>',
  voice: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="3" width="6" height="11" rx="3"></rect><path d="M5 11a7 7 0 0 0 14 0M12 18v3"></path></svg>',
};

function stateText(record) {
  const state = record.viewOnceState || (record.self ? "sent" : "ready");
  if (record.self) {
    if (state === "uploading") return "Sending…";
    if (state === "opened") return "Opened";
    if (state === "expired") return "Expired unopened";
    if (state === "withdrawn") return "Unsent";
    if (state === "failed") return "Not sent";
    return "Sent · disappears once opened";
  }
  if (state === "opening") return "Opening…";
  if (state === "playing") return "Playing";
  if (state === "opened") return "Opened";
  if (state === "expired") return "Expired";
  if (state === "withdrawn") return "Unsent";
  if (state === "failed") return "Could not open. Tap to try again";
  return "Tap to play once";
}

export function paintViewOnceBubble(record) {
  const element = record?.viewOnceEl;
  if (!element || !record.viewOnce) return;
  const kind = record.viewOnce.kind;
  const state = record.viewOnceState || (record.self ? "sent" : "ready");
  const playable = !record.self && (state === "ready" || state === "failed");
  element.dataset.state = state;
  element.disabled = !playable;
  element.setAttribute("aria-disabled", playable ? "false" : "true");
  const label = viewOnceLabel(kind);
  const duration = viewOnceDuration(record.viewOnce.durationMs);
  const status = stateText(record);
  element.querySelector(".chat-view-once-status").textContent = status;
  element.setAttribute(
    "aria-label",
    playable
      ? `Play ${label.toLowerCase()}, ${duration}. It can be played once.`
      : `${label}, ${duration}. ${status}.`,
  );
}

export function renderViewOnceBubble(record, { onOpen = () => {} } = {}) {
  if (!record?.viewOnce) return null;
  const kind = record.viewOnce.kind;
  const button = document.createElement("button");
  button.type = "button";
  button.className = `chat-view-once chat-view-once-${kind}`;
  button.dataset.media = record.viewOnce.media;
  const icon = document.createElement("span");
  icon.className = "chat-view-once-icon";
  icon.innerHTML = ICONS[kind] || ICONS.video;
  const text = document.createElement("span");
  text.className = "chat-view-once-text";
  const title = document.createElement("span");
  title.className = "chat-view-once-title";
  title.textContent =
    `${viewOnceLabel(kind)} · ${viewOnceDuration(record.viewOnce.durationMs)}`;
  const status = document.createElement("span");
  status.className = "chat-view-once-status";
  text.append(title, status);
  button.append(icon, text);
  button.addEventListener("click", () => {
    if (!button.disabled) onOpen(record);
  });
  button.addEventListener("contextmenu", (event) => event.preventDefault());
  record.viewOnceEl = button;
  paintViewOnceBubble(record);
  return button;
}

export function openViewOnceViewer({
  kind,
  bytes,
  mime,
  label = viewOnceLabel(kind),
  onStart = () => {},
  onClose = () => {},
  documentRef = globalThis.document,
  urlApi = globalThis.URL,
}) {
  const overlay = documentRef.createElement("div");
  overlay.className = "chat-view-once-viewer";
  overlay.setAttribute("role", "dialog");
  overlay.setAttribute("aria-modal", "true");
  overlay.setAttribute("aria-label", `${label}, plays once`);
  const panel = documentRef.createElement("div");
  panel.className = "chat-view-once-panel";
  const media = documentRef.createElement(kind === "video" ? "video" : "audio");
  media.className = "chat-view-once-media";
  media.controls = true;
  media.setAttribute("controlsList", "nodownload noplaybackrate noremoteplayback");
  media.disableRemotePlayback = true;
  if (kind === "video") {
    media.playsInline = true;
    media.setAttribute("playsinline", "");
    media.disablePictureInPicture = true;
  }
  media.setAttribute("aria-label", label);
  const blob = new Blob([bytes], { type: mime });
  bytes.fill(0);
  let url = urlApi.createObjectURL(blob);
  media.src = url;
  const note = documentRef.createElement("p");
  note.className = "chat-view-once-note";
  note.textContent =
    "Plays once, then it is deleted for both of you. ForkMesh cannot stop screenshots or screen recording.";
  const close = documentRef.createElement("button");
  close.type = "button";
  close.className = "chat-view-once-close";
  close.textContent = "Close";
  close.setAttribute("aria-label", `Close ${label.toLowerCase()}`);
  panel.append(media, note, close);
  overlay.append(panel);
  overlay.addEventListener("contextmenu", (event) => event.preventDefault());

  let started = false;
  let closed = false;
  function finish() {
    if (closed) return;
    closed = true;
    try { media.pause(); } catch (_) {}
    media.removeAttribute("src");
    try { media.load(); } catch (_) {}
    if (url) urlApi.revokeObjectURL(url);
    url = "";
    overlay.remove();
    documentRef.removeEventListener("keydown", onKey, true);
    onClose({ started });
  }
  function onKey(event) {
    if (event.key === "Escape") {
      event.preventDefault();
      finish();
    }
  }
  media.addEventListener("playing", () => {
    if (started) return;
    started = true;
    onStart();
  });
  media.addEventListener("ended", finish);
  media.addEventListener("error", () => {
    note.textContent = "This browser cannot play this message.";
  });
  close.addEventListener("click", finish);
  documentRef.addEventListener("keydown", onKey, true);
  documentRef.body.append(overlay);
  const playing = media.play();
  if (playing && typeof playing.catch === "function") playing.catch(() => {});
  close.focus();
  return { element: overlay, media, close: finish, get started() { return started; } };
}

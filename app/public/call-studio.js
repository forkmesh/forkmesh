import {
  CALL_FACES,
  CALL_FACE_STORAGE_KEY,
  callBackgroundsSupported,
  callFaceById,
  callFacesSupported,
} from "./chat-call-faces.js";
import {
  CUSTOM_FACE_ID,
  customFaceFromFile,
  rememberCustomFaceImage,
  savedCustomFaceImage,
} from "./chat-call-face-custom.js";
import {
  CUSTOM_BACKGROUND_ID,
  customBackgroundFromFile,
  rememberCallBackground,
  savedCallBackground,
} from "./chat-call-backgrounds.js";
import {
  callVoicesSupported,
  createVoicePreview,
  createVoiceStage,
} from "./chat-call-voice.js";
import { recordViewOnce } from "./chat-view-once-recorder.js";
import { CALL_AUDIO_CONSTRAINTS, CALL_VIDEO_CONSTRAINTS } from "./chat-call-quality.js";

export const STUDIO_AUDIO = CALL_AUDIO_CONSTRAINTS;
export const STUDIO_VIDEO = CALL_VIDEO_CONSTRAINTS;
export const STUDIO_CLIP_MAX_MS = 5 * 60 * 1000;
export const STUDIO_VOICE_WORKLET_URL = new URL("./chat-call-voice-worklet.js", import.meta.url).href;
export const STUDIO_FACE_WATCH_MS = 30000;

export function studioSupport(globalRef = globalThis) {
  const documentRef = globalRef.document;
  return {
    media: Boolean(globalRef.isSecureContext !== false && globalRef.navigator?.mediaDevices?.getUserMedia),
    faces: callFacesSupported(documentRef),
    backgrounds: callBackgroundsSupported(documentRef),
    voices: callVoicesSupported(globalRef),
    recording: Boolean(globalRef.MediaRecorder),
  };
}

export function openStudioMedia({ video = false, getUserMediaImpl = null } = {}) {
  const request = getUserMediaImpl || ((constraints) => globalThis.navigator.mediaDevices.getUserMedia(constraints));
  return request({ audio: { ...STUDIO_AUDIO }, video: video ? { ...STUDIO_VIDEO } : false });
}

export function stopStudioStream(stream) {
  for (const track of stream?.getTracks?.() || []) {
    try {
      track.stop();
    } catch (_) {}
  }
}

export function studioMediaError(error, { video = false } = {}) {
  const name = String(error?.name || "");
  const device = video ? "camera and microphone" : "microphone";
  if (name === "NotAllowedError" || name === "SecurityError") {
    return `Allow the ${device} for this page in your browser, then try again.`;
  }
  if (name === "NotFoundError" || name === "OverconstrainedError") {
    return video ? "No camera was found on this device." : "No microphone was found on this device.";
  }
  if (name === "NotReadableError") {
    return `The ${device} is busy in another app. Close it there and try again.`;
  }
  return `The ${device} could not start here.`;
}

export function studioFaceChoices() {
  return [["", "Your own face"], ...CALL_FACES.map((face) => [face.id, face.label]), [CUSTOM_FACE_ID, "Your photo…"]];
}

export function knownStudioFace(id, storage = globalThis.localStorage) {
  if (id === CUSTOM_FACE_ID) return savedCustomFaceImage(storage) ? CUSTOM_FACE_ID : "";
  return callFaceById(id)?.id || "";
}

export function savedStudioFace(storage = globalThis.localStorage) {
  try {
    return knownStudioFace(storage?.getItem(CALL_FACE_STORAGE_KEY) || "", storage);
  } catch (_) {
    return "";
  }
}

export function rememberStudioFace(id, storage = globalThis.localStorage) {
  const face = knownStudioFace(id, storage);
  try {
    if (face) storage?.setItem(CALL_FACE_STORAGE_KEY, face);
    else storage?.removeItem(CALL_FACE_STORAGE_KEY);
  } catch (_) {}
  return face;
}

export function studioFaceImage(id, storage = globalThis.localStorage) {
  return id === CUSTOM_FACE_ID ? savedCustomFaceImage(storage) : "";
}

export async function studioFaceFromFile(file, { storage = globalThis.localStorage, documentRef = globalThis.document } = {}) {
  const image = await customFaceFromFile(file, documentRef);
  rememberCustomFaceImage(image, storage);
  rememberStudioFace(CUSTOM_FACE_ID, storage);
  return image;
}

export async function studioBackgroundFromFile(file, { storage = globalThis.localStorage, documentRef = globalThis.document } = {}) {
  const image = await customBackgroundFromFile(file, documentRef);
  rememberCallBackground(CUSTOM_BACKGROUND_ID, image, storage);
  return image;
}

export function savedStudioBackground(storage = globalThis.localStorage) {
  return savedCallBackground(storage);
}

export function watchStudioFace(stage, {
  onFailed = () => {},
  limitMs = STUDIO_FACE_WATCH_MS,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  now = () => Date.now(),
} = {}) {
  const started = now();
  let cancelled = false;
  const check = () => {
    if (cancelled || !stage?.faceState) return;
    const state = stage.faceState();
    if (state === "failed") {
      onFailed();
      return;
    }
    if (state === "loading" && now() - started < limitMs) setTimeoutImpl(check, 250);
  };
  setTimeoutImpl(check, 250);
  return () => {
    cancelled = true;
  };
}

export function startStudioVoice({
  micStream,
  faceId = "",
  choice = null,
  storage = globalThis.localStorage,
  onStats = () => {},
  createVoicePreviewImpl = createVoicePreview,
  createVoiceStageImpl = createVoiceStage,
} = {}) {
  return createVoicePreviewImpl({
    micStream,
    faceId,
    choice,
    storage,
    moduleUrl: STUDIO_VOICE_WORKLET_URL,
    createVoiceStageImpl: (options) => createVoiceStageImpl({
      ...options,
      onStats: (stats) => {
        try {
          options.onStats?.(stats);
        } catch (_) {}
        try {
          onStats(stats);
        } catch (_) {}
      },
    }),
  });
}

export function startStudioClip(tracks, kind, {
  maxMs = STUDIO_CLIP_MAX_MS,
  onTick = () => {},
  MediaStreamImpl = globalThis.MediaStream,
  recordImpl = recordViewOnce,
} = {}) {
  const live = (tracks || []).filter((track) => track && track.readyState !== "ended");
  if (!live.length) throw new Error("nothing_to_record");
  const stream = new MediaStreamImpl(live);
  return recordImpl(stream, kind === "video" ? "video" : "voice", { maxMs, onTick });
}

export function studioClipExtension(mime) {
  const type = String(mime || "").toLowerCase();
  if (type.startsWith("video/mp4")) return "mp4";
  if (type.startsWith("audio/mp4")) return "m4a";
  if (type.includes("ogg")) return "ogg";
  return "webm";
}

export function studioFileName(prefix, extension, date = new Date()) {
  const pad = (value) => String(value).padStart(2, "0");
  const stamp = `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}-${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
  const safe = String(prefix || "clip").toLowerCase().replace(/[^a-z0-9-]+/g, "-").replace(/^-+|-+$/g, "") || "clip";
  return `${safe}-${stamp}.${extension}`;
}

export function formatStudioTime(ms) {
  const seconds = Math.max(0, Math.floor((Number(ms) || 0) / 1000));
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
}

export function formatStudioSize(bytes) {
  const size = Math.max(0, Number(bytes) || 0);
  if (size >= 1024 * 1024) return `${(size / (1024 * 1024)).toFixed(1)} MB`;
  return `${Math.max(1, Math.round(size / 1024))} KB`;
}

export function downloadStudioBlob(blob, fileName, documentRef = globalThis.document) {
  const url = globalThis.URL.createObjectURL(blob);
  const link = documentRef.createElement("a");
  link.href = url;
  link.download = fileName;
  link.rel = "noopener";
  link.hidden = true;
  documentRef.body.append(link);
  link.click();
  link.remove();
  globalThis.setTimeout(() => globalThis.URL.revokeObjectURL(url), 60000);
}

export const STUDIO_SHARE_CHUNK = 384 * 1024;

export function studioShareBridge(globalRef = globalThis) {
  const ios = globalRef.webkit?.messageHandlers?.fmShare;
  if (ios && typeof ios.postMessage === "function") return (message) => ios.postMessage(JSON.stringify(message));
  const android = globalRef.FmShareBridge;
  if (android && typeof android.post === "function") return (message) => android.post(JSON.stringify(message));
  return null;
}

export function canShareStudioFiles(type, globalRef = globalThis) {
  if (studioShareBridge(globalRef)) return true;
  const navigatorRef = globalRef.navigator;
  if (typeof navigatorRef?.canShare !== "function" || typeof globalRef.File !== "function") return false;
  try {
    return navigatorRef.canShare({ files: [new globalRef.File([], studioFileName("clip", studioClipExtension(type)), { type })] });
  } catch {
    return false;
  }
}

export function studioBase64(bytes) {
  let text = "";
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    text += String.fromCharCode.apply(null, bytes.subarray(offset, offset + 0x8000));
  }
  return globalThis.btoa(text);
}

export async function shareStudioBlob(blob, fileName, { title = "", globalRef = globalThis } = {}) {
  const bridge = studioShareBridge(globalRef);
  if (bridge) {
    const id = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;
    bridge({ event: "begin", id, name: fileName, mime: blob.type || "application/octet-stream", size: blob.size, title });
    for (let offset = 0; offset < blob.size; offset += STUDIO_SHARE_CHUNK) {
      const part = new Uint8Array(await blob.slice(offset, offset + STUDIO_SHARE_CHUNK).arrayBuffer());
      bridge({ event: "chunk", id, data: studioBase64(part) });
    }
    bridge({ event: "end", id });
    return "native";
  }
  const navigatorRef = globalRef.navigator;
  if (typeof navigatorRef?.share !== "function" || typeof globalRef.File !== "function") return "unsupported";
  const file = new globalRef.File([blob], fileName, { type: blob.type });
  if (typeof navigatorRef.canShare === "function" && !navigatorRef.canShare({ files: [file] })) return "unsupported";
  try {
    await navigatorRef.share(title ? { files: [file], title } : { files: [file] });
    return "shared";
  } catch (error) {
    if (error?.name === "AbortError") return "cancelled";
    throw error;
  }
}

export function studioSnapshot(canvas, type = "image/png") {
  return new Promise((resolve, reject) => {
    if (!canvas?.toBlob) {
      reject(new Error("snapshot_unavailable"));
      return;
    }
    canvas.toBlob((blob) => (blob ? resolve(blob) : reject(new Error("snapshot_unavailable"))), type);
  });
}

export function popOutStudioTrack(track, {
  title = "ForkMesh",
  width = 640,
  height = 360,
  name = "forkmesh-studio",
  globalRef = globalThis,
  onClose = () => {},
} = {}) {
  if (!track) return null;
  const popup = globalRef.open("", name, `popup,width=${Math.round(width)},height=${Math.round(height)}`);
  if (!popup) return null;
  const doc = popup.document;
  doc.title = title;
  doc.body.replaceChildren();
  doc.documentElement.style.cssText = "height:100%;background:#000";
  doc.body.style.cssText = "margin:0;height:100%;background:#000;display:flex;align-items:center;justify-content:center;overflow:hidden";
  const video = doc.createElement("video");
  video.muted = true;
  video.autoplay = true;
  video.playsInline = true;
  video.setAttribute("playsinline", "");
  video.setAttribute("aria-label", title);
  video.style.cssText = "width:100%;height:100%;object-fit:contain;background:#000";
  const StreamImpl = popup.MediaStream || globalRef.MediaStream;
  video.srcObject = new StreamImpl([track]);
  doc.body.append(video);
  video.play?.()?.catch?.(() => {});
  let closed = false;
  const finish = () => {
    if (closed) return;
    closed = true;
    try {
      video.srcObject = null;
    } catch (_) {}
    onClose();
  };
  popup.addEventListener("pagehide", finish);
  popup.addEventListener("beforeunload", finish);
  popup.focus?.();
  return {
    window: popup,
    closed: () => closed || popup.closed,
    close() {
      finish();
      try {
        popup.close();
      } catch (_) {}
    },
  };
}

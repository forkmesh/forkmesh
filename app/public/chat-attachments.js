import { b64ToBytes, bytesToB64, sha256Base64Url } from "./chat-crypto.js";

export const MAX_ATTACHMENT_BYTES = 1024 * 1024;
export const MAX_ATTACHMENT_NAME = 180;
export const MAX_ATTACHMENT_MIME = 100;

export function attachmentFiles(value, limit = 4) {
  const maximum = Math.max(0, Number(limit) || 0);
  return Array.from(value || [])
    .filter((file) => typeof File !== "undefined" && file instanceof File)
    .slice(0, maximum);
}

export function safeAttachmentName(value) {
  const parts = String(value || "")
    .replace(/\\/g, "/")
    .split("/");
  const name = String(parts.pop() || "")
    .replace(/\0/g, "")
    .trim()
    .slice(0, MAX_ATTACHMENT_NAME);
  return name || "file";
}

export function safeAttachmentMime(value) {
  const mime = String(value || "").trim().toLowerCase();
  return /^[a-z0-9][a-z0-9.+-]*\/[a-z0-9][a-z0-9.+-]*$/.test(mime) &&
    mime.length <= MAX_ATTACHMENT_MIME
    ? mime
    : "application/octet-stream";
}

export function attachmentBlobType(value) {
  const mime = safeAttachmentMime(value);
  return /^(?:image\/(?:png|jpeg|gif|webp|avif|bmp)|video\/(?:mp4|webm|ogg|quicktime)|audio\/(?:mpeg|mp4|ogg|webm|wav|aac|flac)|text\/plain|application\/pdf)$/.test(mime)
    ? mime
    : "application/octet-stream";
}

export function attachmentFromEntry(entry) {
  if (!entry || !entry.fileName || typeof entry.file !== "string") return null;
  const encodedLimit = Math.ceil(MAX_ATTACHMENT_BYTES * 4 / 3) + 4;
  if (!entry.file || entry.file.length > encodedLimit) return null;
  try {
    const bytes = b64ToBytes(entry.file);
    if (!bytes.length || bytes.byteLength > MAX_ATTACHMENT_BYTES) return null;
    return {
      fileName: safeAttachmentName(entry.fileName),
      fileMime: safeAttachmentMime(entry.fileMime),
      file: entry.file,
      size: bytes.byteLength,
    };
  } catch (_) {
    return null;
  }
}

export function attachmentBytes(attachment) {
  const normalized = attachmentFromEntry(attachment);
  return normalized ? b64ToBytes(normalized.file) : null;
}

export async function fileToAttachment(file) {
  if (!file || !Number.isFinite(file.size) || file.size < 1) {
    throw new TypeError("That file is empty.");
  }
  if (file.size > MAX_ATTACHMENT_BYTES) {
    throw new RangeError("Attachments must be 1 MiB or smaller.");
  }
  const buffer = await file.arrayBuffer();
  const entry = {
    fileName: safeAttachmentName(file.name),
    fileMime: safeAttachmentMime(file.type),
    file: bytesToB64(buffer),
  };
  const attachment = attachmentFromEntry(entry);
  if (!attachment) throw new TypeError("Could not prepare that attachment.");
  return attachment;
}

export async function normalizeAttachmentForProof(attachment) {
  const normalized = attachmentFromEntry(attachment);
  if (!normalized) return null;
  const bytes = b64ToBytes(normalized.file);
  return {
    fileName: normalized.fileName,
    fileMime: normalized.fileMime,
    size: bytes.byteLength,
    digest: await sha256Base64Url(bytes),
  };
}

export function formatAttachmentSize(size) {
  const bytes = Math.max(0, Number(size) || 0);
  if (bytes < 1024) return `${bytes} B`;
  return `${(bytes / 1024).toFixed(bytes < 10240 ? 1 : 0)} KiB`;
}

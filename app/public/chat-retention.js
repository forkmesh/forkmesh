export const CHAT_RETENTION_ENDPOINT = "/api/accounts/chat-retention";
export const CHAT_RETENTION_STORAGE_KEY = "forkmesh.chat.retention";
export const HOUR_MS = 60 * 60 * 1000;
export const DAY_MS = 24 * HOUR_MS;
export const CHAT_RETENTION_MAX_MS = 7 * DAY_MS;
export const CHAT_RETENTION_MIN_MS = HOUR_MS;
export const CHAT_RETENTION_PRESETS = Object.freeze([
  Object.freeze({ ms: CHAT_RETENTION_MAX_MS, label: "7 days" }),
  Object.freeze({ ms: 3 * DAY_MS, label: "3 days" }),
  Object.freeze({ ms: DAY_MS, label: "1 day" }),
  Object.freeze({ ms: 8 * HOUR_MS, label: "8 hours" }),
  Object.freeze({ ms: HOUR_MS, label: "1 hour" }),
]);

export function clampChatRetentionMs(value) {
  if (typeof value === "boolean" || value === null || value === "") return 0;
  const ms = Math.trunc(Number(value));
  if (!Number.isFinite(ms) || ms <= 0) return 0;
  return Math.max(CHAT_RETENTION_MIN_MS, Math.min(CHAT_RETENTION_MAX_MS, ms));
}

export function normalizeChatRetention(raw) {
  const source = raw && typeof raw === "object" ? raw : {};
  const chats = {};
  if (source.chats && typeof source.chats === "object") {
    for (const [key, value] of Object.entries(source.chats)) {
      const ms = clampChatRetentionMs(value);
      if (key && ms) chats[key] = ms;
    }
  }
  return {
    defaultMs: clampChatRetentionMs(source.defaultMs) || CHAT_RETENTION_MAX_MS,
    chats,
    updatedAt: Math.max(0, Number(source.updatedAt) || 0),
  };
}

export function chatRetentionLabel(ms) {
  const value = clampChatRetentionMs(ms) || CHAT_RETENTION_MAX_MS;
  const preset = CHAT_RETENTION_PRESETS.find((item) => item.ms === value);
  if (preset) return preset.label;
  if (value % DAY_MS === 0) {
    const days = value / DAY_MS;
    return `${days} day${days === 1 ? "" : "s"}`;
  }
  const hours = Math.max(1, Math.round(value / HOUR_MS));
  return `${hours} hour${hours === 1 ? "" : "s"}`;
}

export function chatRetentionKey(channelKey) {
  const value = String(channelKey || "").trim();
  const privateMatch = /^private:([0-9a-f]{32})$/.exec(value);
  if (privateMatch) return "channel:" + privateMatch[1];
  return value;
}

export function createChatRetention({ request, storage = null, now = () => Date.now() } = {}) {
  const listeners = new Set();
  let state = normalizeChatRetention(readCache());

  function readCache() {
    try {
      const raw = storage?.getItem(CHAT_RETENTION_STORAGE_KEY);
      return raw ? JSON.parse(raw) : null;
    } catch (_) {
      return null;
    }
  }

  function writeCache() {
    try {
      storage?.setItem(CHAT_RETENTION_STORAGE_KEY, JSON.stringify(state));
    } catch (_) {}
  }

  function apply(raw) {
    if (!raw || typeof raw !== "object") return false;
    const next = normalizeChatRetention(raw);
    const changed = JSON.stringify(next) !== JSON.stringify(state);
    state = next;
    writeCache();
    if (changed) for (const listener of listeners) listener(state);
    return changed;
  }

  function overrideMs(channelKey) {
    return state.chats[chatRetentionKey(channelKey)] || 0;
  }

  function effectiveMs(channelKey) {
    return overrideMs(channelKey) || state.defaultMs;
  }

  function cutoff(channelKey, nowMs = now()) {
    return nowMs - effectiveMs(channelKey);
  }

  function isExpired(ts, channelKey, nowMs = now()) {
    return Number(ts) <= cutoff(channelKey, nowMs);
  }

  async function send(body) {
    if (typeof request !== "function") return state;
    const data = await request(body);
    apply(data?.chatRetention);
    return state;
  }

  return {
    apply,
    cutoff,
    effectiveMs,
    isExpired,
    overrideMs,
    defaultMs: () => state.defaultMs,
    snapshot: () => normalizeChatRetention(state),
    onChange(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    refresh: () => send({}),
    setChat(channelKey, ms) {
      const key = chatRetentionKey(channelKey);
      return send({ chat: key, retentionMs: clampChatRetentionMs(ms) || null });
    },
    setDefault(ms) {
      return send({ defaultMs: clampChatRetentionMs(ms) || null });
    },
  };
}

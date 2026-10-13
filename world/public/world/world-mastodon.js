export const MASTODON_PROFILE_URL = "https://mastodon.social/@forkmesh";
export const MASTODON_LOOKUP_URL =
  "https://mastodon.social/api/v1/accounts/lookup?acct=forkmesh";
export const MASTODON_STATUS_LIMIT = 30;

const NAMED_ENTITIES = Object.freeze({
  nbsp: " ",
  lt: "<",
  gt: ">",
  quot: '"',
  apos: "'",
  amp: "&",
});

const UNSAFE_TEXT_PATTERN =
  /[\u0000-\u0008\u000b-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/g;

function httpsURL(value) {
  try {
    const raw = String(value || "").trim();
    if (!raw) return "";
    const url = new URL(raw);
    if (url.protocol !== "https:" || url.username || url.password) return "";
    return url.href;
  } catch (_) {
    return "";
  }
}

function oneLine(value, limit = 120) {
  return String(value ?? "")
    .replace(UNSAFE_TEXT_PATTERN, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, limit);
}

function knownCount(value) {
  const number = Number(value);
  return Number.isSafeInteger(number) && number >= 0
    ? Math.min(number, 1_000_000_000)
    : 0;
}

function timestamp(value) {
  const parsed = Date.parse(String(value || ""));
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

function decodeEntity(name) {
  const lower = String(name || "").toLowerCase();
  if (lower in NAMED_ENTITIES) return NAMED_ENTITIES[lower];
  const hex = /^#x([0-9a-f]{1,6})$/i.exec(name);
  const decimal = /^#(\d{1,7})$/.exec(name);
  const code = hex
    ? Number.parseInt(hex[1], 16)
    : decimal
      ? Number(decimal[1])
      : NaN;
  if (!Number.isFinite(code) || code < 32 || code > 0x10ffff) return " ";
  try {
    return String.fromCodePoint(code);
  } catch (_) {
    return " ";
  }
}

/**
 * Reduce Mastodon's sanitized HTML payloads to plain text. Output is inert
 * data: renderers must still HTML-escape it before injection.
 */
export function mastodonPlainText(value, limit = 2400) {
  const text = String(value ?? "")
    .replace(/<br\s*\/?>/gi, "\n")
    .replace(/<\/(?:p|div|blockquote|li)>/gi, "\n\n")
    .replace(/<[^>]*>/g, "")
    .replace(/&([a-z]+|#x?[0-9a-f]+);/gi, (_, name) => decodeEntity(name))
    .replace(UNSAFE_TEXT_PATTERN, " ")
    .replace(/[ \t]+/g, " ")
    .replace(/ ?\n ?/g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
  return text.slice(0, limit).trim();
}

function firstHTTPSLink(value) {
  const match = /href="(https:\/\/[^"]+)"/i.exec(String(value || ""));
  return match ? httpsURL(match[1].replace(/&amp;/gi, "&")) : "";
}

export function formatMastodonCount(value) {
  const number = knownCount(value);
  const compact = (scaled, suffix) => {
    const rounded = Math.round(scaled * 10) / 10;
    const label =
      rounded >= 100 || Number.isInteger(rounded)
        ? String(Math.round(rounded))
        : rounded.toFixed(1);
    return `${label}${suffix}`;
  };
  if (number >= 1_000_000) return compact(number / 1_000_000, "M");
  if (number >= 1_000) return compact(number / 1_000, "K");
  return String(number);
}

/**
 * Bound one public Mastodon account payload to the exact fields the World
 * panel renders. Returns null when the payload has no usable identity.
 */
export function normalizeMastodonAccount(payload) {
  if (!payload || typeof payload !== "object") return null;
  const id = String(payload.id ?? "").trim().slice(0, 64);
  const acct = oneLine(payload.acct || payload.username, 120);
  if (!id || !acct) return null;
  const fields = (Array.isArray(payload.fields) ? payload.fields : [])
    .slice(0, 6)
    .map((field) => ({
      name: oneLine(field?.name, 60),
      value: mastodonPlainText(field?.value, 160),
      url: firstHTTPSLink(field?.value),
      verified: Boolean(timestamp(field?.verified_at)),
    }))
    .filter((field) => field.name && field.value);
  return {
    id,
    acct,
    displayName: oneLine(payload.display_name, 120) || acct,
    url: httpsURL(payload.url) || MASTODON_PROFILE_URL,
    avatar: httpsURL(payload.avatar_static || payload.avatar),
    header: httpsURL(payload.header_static || payload.header),
    note: mastodonPlainText(payload.note, 1200),
    followersCount: knownCount(payload.followers_count),
    followingCount: knownCount(payload.following_count),
    statusesCount: knownCount(payload.statuses_count),
    createdAt: timestamp(payload.created_at),
    fields,
  };
}

/**
 * Bound one public status (toot) payload, unwrapping boosts to the original
 * author. Returns null for statuses with nothing renderable.
 */
export function normalizeMastodonStatus(payload) {
  if (!payload || typeof payload !== "object") return null;
  const boost =
    payload.reblog && typeof payload.reblog === "object" ? payload.reblog : null;
  const source = boost || payload;
  const account =
    source.account && typeof source.account === "object" ? source.account : {};
  const text = mastodonPlainText(source.content, 2400);
  const images = (Array.isArray(source.media_attachments)
    ? source.media_attachments
    : [])
    .filter((media) => String(media?.type || "") === "image")
    .slice(0, 4)
    .map((media) => ({
      url: httpsURL(media?.preview_url || media?.url),
      alt: oneLine(media?.description, 200),
    }))
    .filter((media) => media.url);
  const url = httpsURL(source.url || payload.url);
  if (!text && !images.length && !url) return null;
  return {
    id: String(payload.id ?? "").trim().slice(0, 64),
    url,
    createdAt: timestamp(source.created_at || payload.created_at),
    pinned: payload.pinned === true,
    boostedFrom: boost ? oneLine(account.acct, 120) : "",
    authorName:
      oneLine(account.display_name, 120) ||
      oneLine(account.acct, 120) ||
      "Fediverse account",
    authorAcct: oneLine(account.acct, 120),
    authorAvatar: httpsURL(account.avatar_static || account.avatar),
    spoiler: oneLine(source.spoiler_text, 200),
    text,
    images,
    repliesCount: knownCount(source.replies_count),
    reblogsCount: knownCount(source.reblogs_count),
    favouritesCount: knownCount(source.favourites_count),
  };
}

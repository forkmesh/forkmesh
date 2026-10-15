// ForkMesh CRM extension: the one place that talks to the ForkMesh Worker.
//
// Content scripts and the popup send messages here; this worker holds the
// session token (chrome.storage.local) and calls the CRM package API with a
// bearer header. Cookies are never sent, so nothing the mail site or the
// ForkMesh site set in the browser is reused by accident.
//
// It also keeps what x.js reads off the organization's X profile and records
// it in Marketing when the popup's "Sync now" is pressed (see "X profile for
// Marketing" below).
//
// It also keeps the list of Mastodon instances the reader runs on. Gmail and
// Zoho Mail are in the manifest, but a fediverse instance is any host at all,
// so each one is granted by the user in the popup and registered here.

// The Password Vault shares this worker: its cryptography, its client, and
// the handlers that tie them to the popup (see vault.js).
importScripts("vault-crypto.js", "vault-client.js", "vault.js");

const DEFAULT_BASE_URL = "https://app.forkmesh.com";
const STORED = ["baseUrl", "sessionToken", "account", "organization"];

class ApiError extends Error {
  constructor(code, status = 0, payload = {}) {
    super(String(code || "request_failed").replaceAll("_", " "));
    this.code = String(code || "request_failed");
    this.status = status;
    this.payload = payload;
  }
}

function normalizeBaseUrl(value) {
  try {
    const url = new URL(String(value || "").trim());
    const local = ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname);
    if (url.protocol !== "https:" && !(url.protocol === "http:" && local)) return "";
    return url.origin;
  } catch {
    return "";
  }
}

async function settings() {
  const stored = await chrome.storage.local.get(STORED);
  return {
    baseUrl: normalizeBaseUrl(stored.baseUrl) || DEFAULT_BASE_URL,
    sessionToken: String(stored.sessionToken || ""),
    account: String(stored.account || ""),
    organization: String(stored.organization || ""),
  };
}

async function api(path, method = "GET", body = null, { auth = true } = {}) {
  const current = await settings();
  const headers = { accept: "application/json" };
  if (auth) {
    if (!current.sessionToken) throw new ApiError("not_signed_in", 401);
    headers.authorization = `Bearer ${current.sessionToken}`;
  }
  if (body !== null) headers["content-type"] = "application/json";
  let response;
  try {
    response = await fetch(current.baseUrl + path, {
      method, headers, cache: "no-store", credentials: "omit",
      body: body === null ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new ApiError("forkmesh_unreachable", 0);
  }
  const payload = await response.json().catch(() => ({}));
  if (!response.ok || payload.ok === false) {
    if (auth && response.status === 401) {
      // The session was revoked or expired: forget it so the UI asks again.
      await chrome.storage.local.remove(["sessionToken", "account"]);
    }
    throw new ApiError(payload.error || `http_${response.status}`, response.status, payload);
  }
  return payload;
}

function orgPath(organization, suffix = "") {
  return `/api/crm/${encodeURIComponent(organization)}${suffix}`;
}

async function requireOrganization(requested = "") {
  const current = await settings();
  const organization = String(requested || current.organization || "");
  if (!organization) throw new ApiError("organization_required", 400);
  return organization;
}

async function status() {
  const current = await settings();
  return {
    ok: true,
    connected: Boolean(current.sessionToken),
    account: current.account,
    organization: current.organization,
    baseUrl: current.baseUrl,
  };
}

async function organizations() {
  const payload = await api("/api/crm");
  const list = (payload.organizations || []).map((org) => ({
    name: String(org.name || ""), role: String(org.role || "member"),
    installed: Boolean(org.installed),
  }));
  const current = await settings();
  // Keep a sensible default: the chosen organization if it still has the
  // CRM, otherwise the first one that does.
  const chosen = list.find((org) => org.name === current.organization && org.installed)
    || list.find((org) => org.installed) || null;
  if (chosen && chosen.name !== current.organization) {
    await chrome.storage.local.set({ organization: chosen.name });
  }
  if (payload.actor && payload.actor !== current.account) {
    await chrome.storage.local.set({ account: payload.actor });
  }
  return { ok: true, organizations: list, organization: chosen?.name || "" };
}

async function signIn({ baseUrl, identifier, password, totp }) {
  const origin = normalizeBaseUrl(baseUrl) || DEFAULT_BASE_URL;
  await chrome.storage.local.set({ baseUrl: origin });
  const payload = await api("/api/accounts/login", "POST", {
    identifier: String(identifier || "").trim().toLowerCase(),
    password: String(password || ""),
    totp: String(totp || "").trim(),
  }, { auth: false });
  if (!payload.sessionToken) throw new ApiError("session_token_missing", 502);
  await chrome.storage.local.set({
    sessionToken: String(payload.sessionToken),
    account: String(payload.nodeName || identifier || "").toLowerCase(),
  });
  const orgs = await organizations().catch(() => ({ organizations: [], organization: "" }));
  return { ...(await status()), organizations: orgs.organizations, organization: orgs.organization };
}

async function signOut() {
  const current = await settings();
  if (current.sessionToken) {
    // Best effort: the local copy is dropped even if the server is away.
    await api("/api/accounts/logout", "POST", {}).catch(() => null);
  }
  await chrome.storage.local.remove(["sessionToken", "account", "organization"]);
  // Signing out of ForkMesh locks the vault too.
  await vaultBackground.forget();
  return status();
}

// The name and details ride along so a contact filed under another address,
// or under a name alone, is still found.
async function lookup({ email, handle, name, contact, organization }) {
  const org = await requireOrganization(organization);
  const payload = await api(orgPath(org, "/contacts/lookup"), "POST", {
    email, handle, name: String(name || "").trim(), contact: contactDetails(contact),
  });
  return {
    ok: true, organization: org, contact: payload.contact || null,
    matchedBy: String(payload.matchedBy || ""),
    emails: payload.emails || [], flows: payload.flows || [],
  };
}

// Contact details the content script read off the email (company domain,
// signature lines). The CRM only fills blanks with them.
const CONTACT_DETAILS = ["company", "title", "phone", "website", "linkedin", "mastodon", "twitter", "github"];

function contactDetails(details) {
  const result = {};
  for (const key of CONTACT_DETAILS) {
    const value = String(details?.[key] || "").trim().slice(0, 320);
    if (value) result[key] = value;
  }
  return result;
}

// The message the content script read from the page, trimmed to what the
// CRM keeps: ids, addresses or handles, subject, direction, the contact's
// details, and the pipeline stage to put them in. Never the body.
function linkBody(message) {
  const counterpart = message.direction === "outbound"
    ? (message.to?.[0] || {}) : (message.from || {});
  return {
    provider: message.provider,
    messageId: String(message.messageId || ""),
    threadId: String(message.threadId || ""),
    accountIndex: Number(message.accountIndex || 0),
    direction: message.direction === "outbound" ? "outbound" : "inbound",
    contactEmail: String(message.contactEmail || counterpart.email || "").trim(),
    contactName: String(message.contactName || counterpart.name || "").trim(),
    mailbox: String(message.mailbox || "").trim(),
    // A Mastodon conversation names its people by handle: the contact's and,
    // as the mailbox's counterpart, the account reading it.
    handle: String(message.handle || "").trim(),
    account: String(message.account || "").trim(),
    subject: String(message.subject || "").slice(0, 300),
    sentAt: Number.isFinite(message.sentAt) && message.sentAt > 0 ? Math.round(message.sentAt) : 0,
    providerUrl: String(message.providerUrl || ""),
    contactId: String(message.contactId || ""),
    contact: contactDetails(message.contact),
    flow: String(message.flow || ""),
    stage: String(message.stage || ""),
    createContact: message.createContact !== false,
  };
}

async function link({ message, organization }) {
  const org = await requireOrganization(organization);
  const payload = await api(orgPath(org, "/emails/link"), "POST", linkBody(message || {}));
  return {
    ok: true, organization: org, contact: payload.contact || null,
    email: payload.email || null, emails: payload.emails || [], flows: payload.flows || [],
    contactCreated: Boolean(payload.contactCreated), relinked: Boolean(payload.relinked),
  };
}

const SENT_KEY = "sentMail";
const SENT_RECIPIENTS = 5;

async function sentState() {
  const stored = (await chrome.storage.local.get([SENT_KEY]))[SENT_KEY] || {};
  return { track: stored.track !== false, last: stored.last || null };
}

async function sentSettings() {
  return { ok: true, ...(await sentState()) };
}

async function setTrackSent({ track }) {
  const state = await sentState();
  await chrome.storage.local.set({ [SENT_KEY]: { ...state, track: Boolean(track) } });
  return sentSettings();
}

async function rememberSent(last) {
  const state = await sentState();
  await chrome.storage.local.set({ [SENT_KEY]: { ...state, last } });
}

async function trackSent({ message }) {
  const current = await settings();
  const state = await sentState();
  const sent = message && typeof message === "object" ? message : {};
  if (!state.track || !current.sessionToken || !current.organization) return { ok: true, tracked: false };
  if (!["gmail", "zoho"].includes(sent.provider)) throw new ApiError("invalid_provider", 400);
  const subject = String(sent.subject || "").slice(0, 300);
  const recipients = (Array.isArray(sent.recipients) ? sent.recipients : []).slice(0, SENT_RECIPIENTS);
  for (const person of recipients) {
    try {
      const result = await link({
        organization: current.organization,
        message: {
          ...sent, direction: "outbound", threadId: "", createContact: false,
          contactEmail: String(person?.email || ""), contactName: String(person?.name || ""),
        },
      });
      const contact = result.contact || {};
      await rememberSent({
        at: Date.now(), subject, organization: result.organization, error: "",
        contactId: String(contact.id || ""), contact: String(contact.name || contact.email || person.email || ""),
      });
      return { ok: true, tracked: true, contact: result.contact, email: result.email };
    } catch (error) {
      if (error?.code === "contact_not_found" || error?.code === "invalid_email") continue;
      await rememberSent({ at: Date.now(), subject, organization: current.organization, contact: "", error: error?.message || "request failed" });
      throw error;
    }
  }
  return { ok: true, tracked: false };
}

const CONTACT_ID_RE = /^[0-9a-f]{32}$/;

// The CRM keeps notes as one text field, so a new line is appended to what
// the record holds right now rather than sent as its own row.
const NOTES_LIMIT = 20000;

async function contactPatch({ contactId, patch, organization }) {
  const org = await requireOrganization(organization);
  if (!CONTACT_ID_RE.test(String(contactId || ""))) throw new ApiError("invalid_contact_id", 400);
  const payload = await api(orgPath(org, `/contacts/${contactId}`), "PATCH", patch || {});
  return { ok: true, organization: org, contact: payload.contact || null };
}

// The organization's contacts, trimmed to what a picker shows.
async function contacts({ organization }) {
  const org = await requireOrganization(organization);
  const payload = await api(orgPath(org, "/contacts"));
  return {
    ok: true,
    organization: org,
    contacts: (payload.contacts || []).map((contact) => ({
      id: String(contact.id || ""), name: String(contact.name || ""),
      email: String(contact.email || ""), company: String(contact.company || ""),
      mastodon: String(contact.mastodon || ""),
      flow: String(contact.flow || ""), stage: String(contact.stage || ""),
    })),
  };
}

// Any page, on any site, filed under a contact: the address bar's link and
// the page's title are appended to that contact's notes. Nothing else about
// the page is read, and the CRM is the only place it goes.
async function noteLink({ contactId, url, title, organization }) {
  const org = await requireOrganization(organization);
  if (!CONTACT_ID_RE.test(String(contactId || ""))) throw new ApiError("invalid_contact_id", 400);
  let link;
  try {
    link = new URL(String(url || "").trim());
  } catch {
    throw new ApiError("invalid_url", 400);
  }
  if (!["https:", "http:"].includes(link.protocol)) throw new ApiError("invalid_url", 400);
  const current = await api(orgPath(org, `/contacts/${contactId}`));
  const existing = String(current.contact?.notes || "").trimEnd();
  const heading = String(title || "").replace(/\s+/g, " ").trim().slice(0, 160);
  const line = `${new Date().toISOString().slice(0, 10)} · ${heading || link.hostname} — ${link.href}`;
  const notes = existing ? `${existing}\n${line}` : line;
  // The CRM truncates a longer note, which would silently drop this line.
  if (notes.length > NOTES_LIMIT) throw new ApiError("notes_full", 400);
  const payload = await api(orgPath(org, `/contacts/${contactId}`), "PATCH", { notes });
  return { ok: true, organization: org, contact: payload.contact || null, line };
}

async function unlink({ emailId, organization }) {
  const org = await requireOrganization(organization);
  if (!CONTACT_ID_RE.test(String(emailId || ""))) throw new ApiError("invalid_email_id", 400);
  await api(orgPath(org, `/emails/${emailId}`), "DELETE", {});
  return { ok: true };
}

// --- Mastodon instances --------------------------------------------------
// One registered content script per instance the user added, each behind the
// host permission Chrome granted for it.

const MASTODON_SCRIPT = "fm-mastodon-";
const MASTODON_FILES = { js: ["panel.js", "mastodon.js"], css: ["content.css"] };

// "mastodon.social", "https://mastodon.social/", or the instance half of a
// handle typed in whole ("@grace@mastodon.social").
function instanceHost(value) {
  const raw = String(value || "").trim().toLowerCase().replace(/\/+$/, "");
  const host = raw.includes("@") ? raw.split("@").pop() : raw;
  try {
    const url = new URL(host.includes("://") ? host : `https://${host}`);
    if (url.protocol !== "https:" || url.pathname !== "/") return "";
    return /^[a-z0-9-]+(\.[a-z0-9-]+)+$/.test(url.hostname) ? url.hostname : "";
  } catch {
    return "";
  }
}

const hostPattern = (host) => `https://${host}/*`;

async function storedHosts() {
  const stored = await chrome.storage.local.get(["mastodonHosts"]);
  return [...new Set((stored.mastodonHosts || []).map(instanceHost).filter(Boolean))];
}

// Keep the registered scripts equal to the instances still granted: a
// permission taken back in Chrome's own settings stops its reader too.
async function syncMastodon() {
  const wanted = [];
  for (const host of await storedHosts()) {
    if (await chrome.permissions.contains({ origins: [hostPattern(host)] })) wanted.push(host);
  }
  const registered = await chrome.scripting.getRegisteredContentScripts().catch(() => []);
  const ours = registered.filter((script) => script.id.startsWith(MASTODON_SCRIPT));
  const keep = new Set(wanted.map((host) => MASTODON_SCRIPT + host));
  const stale = ours.filter((script) => !keep.has(script.id)).map((script) => script.id);
  if (stale.length) await chrome.scripting.unregisterContentScripts({ ids: stale }).catch(() => null);
  const have = new Set(ours.map((script) => script.id));
  const missing = wanted.filter((host) => !have.has(MASTODON_SCRIPT + host));
  if (missing.length) {
    await chrome.scripting.registerContentScripts(missing.map((host) => ({
      id: MASTODON_SCRIPT + host, matches: [hostPattern(host)],
      ...MASTODON_FILES, runAt: "document_idle",
    }))).catch(() => null);
  }
  await chrome.storage.local.set({ mastodonHosts: wanted });
  return { ok: true, hosts: wanted };
}

// Chrome grants a host only inside the click that asked for it, so the popup
// makes the request and this says the instance is in once it holds. The tab
// the popup's "Record this toot" was pressed on is already loaded, so it gets
// the reader now instead of on its next load.
async function addMastodon({ host, tabId }) {
  const clean = instanceHost(host);
  if (!clean) throw new ApiError("invalid_instance", 400);
  if (!await chrome.permissions.contains({ origins: [hostPattern(clean)] })) {
    throw new ApiError("permission_denied", 403);
  }
  await chrome.storage.local.set({ mastodonHosts: [...await storedHosts(), clean] });
  const result = await syncMastodon();
  const tab = Number.isInteger(tabId) ? await chrome.tabs.get(tabId).catch(() => null) : null;
  if (tab && instanceHost(tab.url ? new URL(tab.url).hostname : "") === clean) {
    const target = { tabId };
    await chrome.scripting.insertCSS({ target, files: MASTODON_FILES.css }).catch(() => null);
    await chrome.scripting.executeScript({ target, files: MASTODON_FILES.js }).catch(() => null);
  }
  return result;
}

async function removeMastodon({ host }) {
  const clean = instanceHost(host);
  const kept = (await storedHosts()).filter((item) => item !== clean);
  await chrome.storage.local.set({ mastodonHosts: kept });
  if (clean) await chrome.permissions.remove({ origins: [hostPattern(clean)] }).catch(() => null);
  return syncMastodon();
}

chrome.runtime.onInstalled.addListener(() => void syncMastodon());
chrome.runtime.onStartup.addListener(() => void syncMastodon());
chrome.permissions.onRemoved.addListener(() => void syncMastodon());

const TOOT_KEY = "mastodonToots";
const TOOT_ACCOUNT_KEY = "mastodonTootAccount";
const TOOT_ACCOUNT_MS = 10 * 60 * 1000;
const TOOT_PEOPLE = 10;
const TOOT_SEEN = 500;
const TOOT_ID = /^\d{5,25}$/;
const TOOT_HANDLE = /^[a-z0-9_.-]+@[a-z0-9-]+(\.[a-z0-9-]+)+$/;
const TOOT_RETRY = new Set(["not_signed_in", "forkmesh_unreachable", "rate_limited"]);

let tootQueue = Promise.resolve();
const tootSerial = (work) => (tootQueue = tootQueue.then(work, work));

const tootHandle = (value) => {
  const handle = String(value || "").trim().toLowerCase().replace(/^@/, "");
  return TOOT_HANDLE.test(handle) ? handle : "";
};

async function tootState() {
  const stored = (await chrome.storage.local.get([TOOT_KEY]))[TOOT_KEY] || {};
  return { track: stored.track !== false, seen: stored.seen || {}, last: stored.last || null };
}

async function tootSettings() {
  const { track, last } = await tootState();
  return { ok: true, track, last };
}

async function setTrackToots({ track }) {
  const state = await tootState();
  await chrome.storage.local.set({ [TOOT_KEY]: { ...state, track: Boolean(track) } });
  return tootSettings();
}

async function tootOrgAccount(organization) {
  const cached = (await chrome.storage.session.get([TOOT_ACCOUNT_KEY]))[TOOT_ACCOUNT_KEY];
  if (cached?.organization === organization && Date.now() - cached.at < TOOT_ACCOUNT_MS) return cached.account;
  const payload = await api(orgPath(organization, "/settings")).catch(() => null);
  const account = tootHandle(payload?.settings?.mastodon?.account);
  if (payload) {
    await chrome.storage.session.set({ [TOOT_ACCOUNT_KEY]: { organization, account, at: Date.now() } });
  }
  return account;
}

function tootPeople(toot, ...ours) {
  const people = [];
  for (const person of Array.isArray(toot.people) ? toot.people : []) {
    const handle = tootHandle(person?.handle);
    if (handle && !ours.includes(handle) && !people.some((item) => item.handle === handle)) {
      people.push({ handle, name: String(person?.name || "").trim().slice(0, 120) });
    }
  }
  return people.slice(0, TOOT_PEOPLE);
}

async function tootRemember(state, key, last) {
  const seen = { ...state.seen, [key]: Date.now() };
  const kept = Object.entries(seen).sort((a, b) => b[1] - a[1]).slice(0, TOOT_SEEN);
  await chrome.storage.local.set({ [TOOT_KEY]: { track: state.track, seen: Object.fromEntries(kept), last } });
}

const trackToot = ({ toot }) => tootSerial(async () => {
  const current = await settings();
  const state = await tootState();
  const post = toot && typeof toot === "object" ? toot : {};
  if (!state.track || !current.sessionToken || !current.organization) return { ok: true, tracked: false };
  const statusId = String(post.statusId || "");
  let permalink;
  try { permalink = new URL(String(post.providerUrl || "")); } catch { permalink = null; }
  if (!TOOT_ID.test(statusId) || permalink?.protocol !== "https:") throw new ApiError("invalid_toot", 400);
  const key = `${permalink.hostname}/${statusId}`;
  if (state.seen[key]) return { ok: true, tracked: false, seen: true };
  const author = tootHandle(post.author?.handle);
  const mine = tootHandle(post.mine);
  if (!author) return { ok: true, tracked: false };
  if (author !== mine && author !== await tootOrgAccount(current.organization)) {
    return { ok: true, tracked: false, mine: false };
  }
  const people = tootPeople(post, author, mine);
  const sentAt = Number(post.sentAt);
  const logged = [];
  const failed = [];
  let created = 0;
  for (const [index, person] of people.entries()) {
    try {
      const result = await link({
        organization: current.organization,
        message: {
          provider: "mastodon", direction: "outbound", createContact: true,
          messageId: index ? `${statusId}/${person.handle}` : statusId,
          threadId: TOOT_ID.test(String(post.threadId || "")) ? String(post.threadId) : statusId,
          handle: person.handle, contactName: person.name, account: author,
          subject: String(post.subject || ""), sentAt, providerUrl: permalink.href,
        },
      });
      if (result.contactCreated) created += 1;
      logged.push(String(result.contact?.name || `@${person.handle}`));
    } catch (error) {
      if (TOOT_RETRY.has(error?.code) || !error?.status || error.status >= 500) {
        await chrome.storage.local.set({ [TOOT_KEY]: { ...state, last: {
          at: Date.now(), providerUrl: permalink.href, contacts: logged, created, error: error?.message || "request failed",
        } } });
        throw error;
      }
      failed.push(`@${person.handle}`);
    }
  }
  await tootRemember(state, key, {
    at: Date.now(), providerUrl: permalink.href, contacts: logged, created,
    error: failed.length ? `not logged: ${failed.join(", ")}` : "",
  });
  return { ok: true, tracked: logged.length > 0, logged: logged.length, created, contacts: logged };
});


// --- X profile for Marketing ---------------------------------------------
// x.js reads the tracked profile's pages as they are opened in a tab and
// hands the numbers here. They are kept in extension storage and shown in the
// popup's X widget; the popup's "Sync now" or "Update X stats from this page"
// records them as a Marketing snapshot for the organization's X channel.
// Nothing is sent to X and no failure reaches the page: errors land in the
// widget only.

const X_KEY = "xStats";
const X_DEFAULT_HANDLE = "forkmesh";
const X_WINDOW = 30 * 24 * 60 * 60 * 1000;
const X_MAX_POSTS = 1000;
const X_POSTS_BATCH = 250;
const X_MAX_MENTIONS = 1000;
const X_MAX_ERRORS = 8;
const X_PROFILE = ["followers", "following", "posts", "media"];
const X_RECENT = 6;
const X_MAX_IMAGES = 4;
const X_IMAGE_PATHS = /^\/(media|ext_tw_video_thumb|amplify_video_thumb|tweet_video_thumb|card_img)\//;
const X_LIVE_KEY = "xLive";
const X_LIVE_DELAY_MS = 10 * 1000;
const X_LIVE_RECORD_MS = 60 * 1000;

// One observation at a time: messages from several tabs would otherwise
// overwrite each other's read of storage.
let xQueue = Promise.resolve();
const xSerial = (work) => (xQueue = xQueue.then(work, work));

const xHandleOf = (value) => {
  const clean = String(value || "").trim().replace(/^@/, "").toLowerCase();
  return /^[a-z0-9_]{1,15}$/.test(clean) ? clean : "";
};
const xNumber = (value) => (Number.isFinite(value) && value >= 0 ? Math.round(value) : 0);

function xImages(value) {
  const images = [];
  for (const item of Array.isArray(value) ? value : []) {
    let url;
    try {
      url = new URL(String(item || ""));
    } catch {
      continue;
    }
    if (url.protocol !== "https:" || url.hostname !== "pbs.twimg.com" || !X_IMAGE_PATHS.test(url.pathname)) continue;
    if (url.href.length <= 500 && !images.includes(url.href)) images.push(url.href);
    if (images.length >= X_MAX_IMAGES) break;
  }
  return images;
}

function xRecent(state) {
  return Object.entries(state.posts)
    .sort((a, b) => (b[1].at || b[1].seenAt || 0) - (a[1].at || a[1].seenAt || 0))
    .slice(0, X_RECENT)
    .map(([id, post]) => ({
      id, url: `https://x.com/${state.handle}/status/${id}`, text: post.text || "",
      at: xNumber(post.at), images: xImages(post.images), views: xNumber(post.views),
      likes: xNumber(post.likes), replies: xNumber(post.replies), reposts: xNumber(post.reposts),
    }));
}

async function xState() {
  const stored = (await chrome.storage.local.get([X_KEY]))[X_KEY] || {};
  return {
    handle: xHandleOf(stored.handle) || X_DEFAULT_HANDLE,
    profile: stored.profile || {}, readAt: Number(stored.readAt || 0),
    posts: stored.posts || {}, mentions: stored.mentions || {},
    errors: Array.isArray(stored.errors) ? stored.errors : [],
    lastSync: stored.lastSync || null,
  };
}

const xSave = (state) => chrome.storage.local.set({ [X_KEY]: state });

function xRemember(state, message) {
  const at = Date.now();
  const text = String(message || "unknown error").slice(0, 200);
  state.errors = [{ at, message: text },
    ...state.errors.filter((item) => item.message !== text)].slice(0, X_MAX_ERRORS);
}

// Keep the newest entries of an id-keyed map.
function xTrim(map, limit) {
  const entries = Object.entries(map).sort((a, b) => (b[1].at || 0) - (a[1].at || 0));
  return Object.fromEntries(entries.slice(0, limit));
}

// Posts and mentions from the last thirty days; the header counts as read.
function xTotals(state, now = Date.now()) {
  const recent = (item) => (item.at || item.seenAt || 0) >= now - X_WINDOW;
  const posts = Object.values(state.posts).filter(recent);
  const sum = (key) => posts.reduce((total, post) => total + xNumber(post[key]), 0);
  const totals = Object.fromEntries(X_PROFILE.map((key) => [key, xNumber(state.profile[key])]));
  Object.assign(totals, {
    replies: sum("replies"), reposts: sum("reposts"), likes: sum("likes"), views: sum("views"),
    mentions: Object.values(state.mentions).filter(recent).length,
    postsSeen: posts.length,
  });
  totals.engagements = totals.replies + totals.reposts + totals.likes;
  return totals;
}

async function xStats() {
  const state = await xState();
  const current = await settings();
  return {
    ok: true, handle: state.handle, totals: xTotals(state), recent: xRecent(state), readAt: state.readAt,
    lastSync: state.lastSync, errors: state.errors, connected: Boolean(current.sessionToken),
    live: await xLive(),
  };
}

// The Marketing organization: the one the CRM uses when Marketing is
// installed there, otherwise the first that has it.
async function xOrganization() {
  const payload = await api("/api/marketing");
  const list = (payload.organizations || []).filter((org) => org.installed);
  const current = await settings();
  const chosen = list.find((org) => org.name === current.organization) || list[0];
  if (!chosen) throw new ApiError("marketing_not_installed", 409);
  return String(chosen.name);
}

// Records the numbers now. Only "Sync now" in the popup calls this: the
// snapshot is never posted on a schedule or because a profile page was read.
async function xSync(state, { organization = "", record = true, recorded = record } = {}) {
  const current = await settings();
  if (!current.sessionToken || !state.readAt) return false;
  const last = state.lastSync || {};
  try {
    const org = organization || await xOrganization();
    if (record) await xRecord(state, org);
    const links = await xSendPosts(state, org, Number(last.postsAt || 0));
    const at = Date.now();
    state.lastSync = {
      at, organization: org, error: "", ...links,
      profile: recorded ? { ...state.profile } : last.profile || {},
      recordedAt: recorded ? at : Number(last.recordedAt || 0),
    };
  } catch (error) {
    state.lastSync = { ...last, error: error?.code || "request_failed" };
    xRemember(state, `Marketing sync: ${error?.message || "request failed"}`);
  }
  return true;
}

async function xRecord(state, organization) {
  const totals = xTotals(state);
  const path = `/api/marketing/${encodeURIComponent(organization)}/metrics`;
  const history = await api(path);
  const previous = (history.metrics || []).find((metric) => metric.channel === "x") || {};
  const { postsSeen, ...counters } = totals;
  await api(path, "POST", {
    channel: "x", ...counters, impressions: totals.views,
    reach: xNumber(previous.reach), clicks: xNumber(previous.clicks),
    source: "x-profile", note: `Read from x.com/${state.handle} (${postsSeen} recent posts seen)`,
  });
}

async function xSendPosts(state, organization, since) {
  const postsAt = Date.now();
  const posts = Object.entries(state.posts).filter(([, post]) => xNumber(post.seenAt) > since)
    .map(([id, post]) => ({
      url: `https://x.com/${state.handle}/status/${id}`, channel: "x", title: post.text || "",
      postedAt: xNumber(post.at), checkedAt: xNumber(post.seenAt), views: xNumber(post.views),
      likes: xNumber(post.likes), replies: xNumber(post.replies), reposts: xNumber(post.reposts),
      images: xImages(post.images),
    }));
  const links = { postsAt, linksAdded: 0, linksUpdated: 0 };
  for (let start = 0; start < posts.length; start += X_POSTS_BATCH) {
    const result = await api(`/api/marketing/${encodeURIComponent(organization)}/posts`, "POST",
      { posts: posts.slice(start, start + X_POSTS_BATCH) });
    links.linksAdded += xNumber(result.created);
    links.linksUpdated += xNumber(result.updated);
  }
  return links;
}

function xActivity(state, seen, url) {
  const posts = Array.isArray(seen.posts) ? seen.posts.length : 0;
  const mentions = Array.isArray(seen.mentions) ? seen.mentions.length : 0;
  const read = [posts && activityCount(posts, "post", "posts"), mentions && activityCount(mentions, "mention", "mentions"),
    Number.isFinite(seen.profile?.followers) && activityCount(seen.profile.followers, "follower", "followers")].filter(Boolean);
  return {
    kind: "read", site: "x", page: `@${state.handle} ${seen.page}`, url,
    text: `Read @${state.handle} on X (${seen.page})${read.length ? `: ${read.join(", ")}` : ""}.`,
  };
}

async function xObserve({ observation, fresh }, sender) {
  return xSerial(async () => {
    const state = await xState();
    const seen = observation && typeof observation === "object" ? observation : {};
    if (xHandleOf(seen.handle) !== state.handle) return { ok: true, handle: state.handle };
    if (["profile", "media", "mentions", "search"].includes(seen.page)) {
      await refreshMark([`x:${seen.page}`], sender?.tab?.id, fresh === true);
      if (fresh === true) void activityReport(xActivity(state, seen, sender?.tab?.url), `x:${seen.page}`);
    }
    const now = Date.now();
    let changed = false;
    for (const key of X_PROFILE) {
      const value = seen.profile?.[key];
      if (Number.isFinite(value) && value >= 0) {
        state.profile[key] = Math.round(value);
        changed = true;
      }
    }
    for (const post of Array.isArray(seen.posts) ? seen.posts.slice(0, 200) : []) {
      if (!/^\d{5,25}$/.test(String(post?.id || ""))) continue;
      const text = String(post.text || state.posts[post.id]?.text || "").replace(/\s+/g, " ").trim().slice(0, 280);
      const seenImages = xImages(post.images);
      const images = seenImages.length ? seenImages : xImages(state.posts[post.id]?.images);
      state.posts[post.id] = {
        at: xNumber(post.at), seenAt: now, text, images, replies: xNumber(post.replies),
        reposts: xNumber(post.reposts), likes: xNumber(post.likes), views: xNumber(post.views),
      };
      changed = true;
    }
    for (const mention of Array.isArray(seen.mentions) ? seen.mentions.slice(0, 200) : []) {
      if (!/^\d{5,25}$/.test(String(mention?.id || ""))) continue;
      state.mentions[mention.id] = { at: xNumber(mention.at) || state.mentions[mention.id]?.at || now };
      changed = true;
    }
    if (!changed) return { ok: true, handle: state.handle };
    state.readAt = now;
    state.posts = xTrim(state.posts, X_MAX_POSTS);
    state.mentions = xTrim(state.mentions, X_MAX_MENTIONS);
    await xSave(state);
    const live = await xLive();
    if (live && live.tabId === sender?.tab?.id) xLiveSchedule();
    return { ok: true, handle: state.handle };
  });
}

const xError = ({ message, page }) => xSerial(async () => {
  const state = await xState();
  xRemember(state, `${String(page || "").slice(0, 60)} ${String(message || "")}`.trim());
  await xSave(state);
  return { ok: true };
});

const xSyncNow = () => xSerial(async () => {
  const state = await xState();
  if (!state.readAt) throw new ApiError("x_profile_not_read", 400);
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  await xSync(state);
  await xSave(state);
  return xStats();
});

const xSetHandle = ({ handle }) => xSerial(async () => {
  const clean = xHandleOf(handle);
  if (!clean) throw new ApiError("invalid_x_handle", 400);
  const state = await xState();
  // Another account's numbers must not be mixed into this one's.
  if (clean !== state.handle) {
    await xLiveEnd();
    await xSave({ handle: clean, profile: {}, readAt: 0, posts: {}, mentions: {}, errors: [], lastSync: null });
  }
  return xStats();
});

const xClearErrors = () => xSerial(async () => {
  const state = await xState();
  state.errors = [];
  await xSave(state);
  return xStats();
});

let xLiveTimer = 0;

const xLive = async () => (await chrome.storage.session.get([X_LIVE_KEY]))[X_LIVE_KEY] || null;

function xLiveSchedule(delay = X_LIVE_DELAY_MS) {
  if (xLiveTimer) return;
  xLiveTimer = setTimeout(() => {
    xLiveTimer = 0;
    void xLivePush();
  }, delay);
}

async function xLiveEnd() {
  clearTimeout(xLiveTimer);
  xLiveTimer = 0;
  await chrome.storage.session.remove(X_LIVE_KEY);
}

async function xLiveSend(live, record, recorded = record) {
  const state = await xState();
  if (!(await xSync(state, { organization: live.organization, record, recorded }))) return live;
  await xSave(state);
  if (state.lastSync.error) {
    await xLiveEnd();
    return null;
  }
  return { ...live, at: state.lastSync.at,
    linksAdded: live.linksAdded + xNumber(state.lastSync.linksAdded),
    linksUpdated: live.linksUpdated + xNumber(state.lastSync.linksUpdated) };
}

const xCapturePosts = (organization, tabId) => xSerial(async () => {
  const start = { tabId, organization, startedAt: Date.now(), at: 0, linksAdded: 0, linksUpdated: 0 };
  const live = await xLiveSend(start, false, true);
  if (!live || !Number.isInteger(tabId)) return live;
  await chrome.storage.session.set({ [X_LIVE_KEY]: live });
  return live;
});

const xLivePush = () => xSerial(async () => {
  const live = await xLive();
  if (!live) return;
  const state = await xState();
  const last = state.lastSync || {};
  const moved = X_PROFILE.some((key) => key in state.profile && state.profile[key] !== last.profile?.[key]);
  const wait = Number(last.recordedAt || 0) + X_LIVE_RECORD_MS - Date.now();
  const next = await xLiveSend(live, moved && wait <= 0);
  if (!next) return;
  await chrome.storage.session.set({ [X_LIVE_KEY]: next });
  if (moved && wait > 0) xLiveSchedule(wait);
});

const xLiveStop = () => xSerial(async () => {
  await xLiveEnd();
  return xStats();
});

async function openX() {
  const state = await xState();
  await chrome.tabs.create({ url: `https://x.com/${state.handle}` });
  return { ok: true };
}


const REDDIT_KEY = "redditDesk";
const REDDIT_MAX_POSTS = 500;
const REDDIT_MAX_SENT = 3000;
const REDDIT_MAX_ERRORS = 8;
const REDDIT_MAX_RULE_SETS = 60;
const REDDIT_SYNC_LIMIT = 100;
const REDDIT_WATCH_STALE = 6 * 60 * 60 * 1000;
const REDDIT_SUBREDDIT = /^[a-z0-9_]{2,21}$/i;
const REDDIT_POST_ID = /^[a-z0-9]{1,12}$/;
const REDDIT_AUTHOR = /^[A-Za-z0-9_-]{1,20}$/;
const REDDIT_MONITOR_LIMIT = 50;
const REDDIT_CHECK_LIMIT = 20;
const REDDIT_CHECK_TRIES = 6;
const REDDIT_CHECK_RETRY_MS = 1500;
const REDDIT_CHECK_ALARM = "fm-reddit-monitor-check";
const REDDIT_CHECK_EVERY_MINUTES = 6 * 60;
const REDDIT_MONITOR_COUNTS = [["views", 0], ["score", -1e9], ["comments", 0], ["commentViews", 0], ["commentScore", -1e9]];
const REDDIT_ACCOUNT_LIMIT = 10;
const REDDIT_MAX_MINE = 400;
const REDDIT_MINE_CHECK_LIMIT = 25;
const REDDIT_MINE_FRESH_MS = 30 * 60 * 1000;
const REDDIT_MINE_FIELDS = ["title", "postedAt", "views", "score", "comments"];

let redditQueue = Promise.resolve();
const redditSerial = (work) => (redditQueue = redditQueue.then(work, work));

const redditQuery = (value) => String(value || "").replace(/\s+/g, " ").trim().toLowerCase().slice(0, 120);
const redditCount = (value) => (Number.isInteger(value) && Math.abs(value) <= 1e9 ? value : null);

async function redditState() {
  const stored = (await chrome.storage.local.get([REDDIT_KEY]))[REDDIT_KEY] || {};
  const watch = stored.watch || {};
  return {
    watch: {
      subreddits: Array.isArray(watch.subreddits) ? watch.subreddits : [],
      queries: Array.isArray(watch.queries) ? watch.queries : [],
      rulesOnFile: watch.rulesOnFile || {},
      fetchedAt: Number(watch.fetchedAt || 0),
      organization: String(watch.organization || ""),
      monitored: Array.isArray(watch.monitored) ? watch.monitored : [],
      accounts: Array.isArray(watch.accounts) ? watch.accounts : [],
    },
    posts: stored.posts || {}, rules: stored.rules || {},
    mine: stored.mine || {}, mineSent: stored.mineSent || {}, mineCheck: stored.mineCheck || null,
    sent: stored.sent || {}, rulesSent: stored.rulesSent || {},
    readings: stored.readings || {}, readingsSent: stored.readingsSent || {},
    errors: Array.isArray(stored.errors) ? stored.errors : [],
    lastSync: stored.lastSync || null, lastCheck: stored.lastCheck || null,
    autoCheck: stored.autoCheck === true,
  };
}

const redditSave = (state) => chrome.storage.local.set({ [REDDIT_KEY]: state });

function redditRemember(state, message) {
  const text = String(message || "unknown error").slice(0, 200);
  state.errors = [{ at: Date.now(), message: text },
    ...state.errors.filter((item) => item.message !== text)].slice(0, REDDIT_MAX_ERRORS);
}

function redditTrim(map, limit, stampOf = (value) => value?.at) {
  const stamp = (value) => Number(stampOf(value) || 0);
  const entries = Object.entries(map).sort((a, b) => stamp(b[1]) - stamp(a[1]));
  return Object.fromEntries(entries.slice(0, limit));
}

const redditWatchedSubreddit = (state, name) =>
  state.watch.subreddits.some((item) => item.toLowerCase() === String(name || "").toLowerCase());
const redditWatchedQuery = (state, query) => state.watch.queries.includes(redditQuery(query));

function redditTitleMatches(state, title) {
  const lower = String(title || "").toLowerCase();
  return state.watch.queries.some((query) => {
    const words = query.split(" ").filter(Boolean);
    return words.length && words.every((word) => lower.includes(word));
  });
}

function redditCandidates(state) {
  return Object.values(state.posts).filter((post) => {
    const watched = redditWatchedSubreddit(state, post.subreddit);
    return (post.opened && watched)
      || (post.queries || []).some((query) => redditWatchedQuery(state, query))
      || (post.listed && watched && redditTitleMatches(state, post.title));
  }).sort((a, b) => (b.seenAt || 0) - (a.seenAt || 0));
}

const redditUnsent = (state) => redditCandidates(state).filter((post) => !state.sent[post.id]);

const redditMonitoredPost = (state, postId) =>
  state.watch.monitored.find((entry) => entry.postId === String(postId || "").toLowerCase()) || null;

function redditReadingsUnsent(state) {
  return state.watch.monitored.filter((entry) => {
    const reading = state.readings[entry.postId];
    return reading && reading.at > Number(state.readingsSent[entry.postId] || 0) && reading.at > entry.checkedAt;
  });
}

const redditMineUnsent = (state) =>
  Object.values(state.mine).filter((item) => item.seenAt > Number(state.mineSent[item.key] || 0));

const redditAccount = (state, name) =>
  state.watch.accounts.find((item) => item.toLowerCase() === String(name || "").toLowerCase()) || "";

function redditRulesUnsent(state) {
  return Object.values(state.rules).filter((entry) => redditWatchedSubreddit(state, entry.subreddit)
    && entry.readAt > Number(state.rulesSent[entry.subreddit.toLowerCase()] || 0));
}

async function redditStats() {
  const state = await redditState();
  const current = await settings();
  const rulesRead = (name) => Boolean(state.rules[name.toLowerCase()] || state.watch.rulesOnFile[name.toLowerCase()]);
  return {
    ok: true, connected: Boolean(current.sessionToken),
    organization: state.watch.organization, fetchedAt: state.watch.fetchedAt,
    subreddits: state.watch.subreddits.length, queries: state.watch.queries.length,
    waiting: redditUnsent(state).length, seen: Object.keys(state.posts).length,
    ruleSets: Object.keys(state.rules).length, rulesWaiting: redditRulesUnsent(state).length,
    watched: state.watch.subreddits.map((name) => name.toLowerCase()), watchedQueries: state.watch.queries,
    rulesMissing: state.watch.subreddits.filter((name) => !rulesRead(name)).slice(0, 40)
      .map((name) => ({ name, url: `https://old.reddit.com/r/${name}/about/rules` })),
    monitored: state.watch.monitored.map((entry) => {
      const reading = state.readings[entry.postId];
      const latest = reading && reading.at > entry.checkedAt ? reading : entry;
      return {
        deskId: entry.deskId, subreddit: entry.subreddit, url: entry.url, commentUrl: entry.commentUrl,
        title: entry.title || reading?.title || "", checkedAt: latest.checkedAt || latest.at || 0,
        views: latest.views ?? null, score: latest.score ?? null, comments: latest.comments ?? null,
        commentViews: latest.commentViews ?? null, commentScore: latest.commentScore ?? null,
        waiting: Boolean(reading && reading.at > entry.checkedAt && reading.at > Number(state.readingsSent[entry.postId] || 0)),
      };
    }),
    readingsWaiting: redditReadingsUnsent(state).length,
    accounts: state.watch.accounts, mine: Object.keys(state.mine).length, mineWaiting: redditMineUnsent(state).length,
    mineCheck: state.mineCheck, mineChecking: redditMineChecking,
    checking: redditChecking, autoCheck: state.autoCheck, lastCheck: state.lastCheck,
    lastSync: state.lastSync, errors: state.errors,
  };
}

async function redditFetchWatch(state) {
  const organization = await xOrganization();
  const payload = await api(`/api/marketing/${encodeURIComponent(organization)}/reddit`);
  const desk = payload.redditDesk || {};
  const subreddits = (desk.subreddits || []).map((item) => String(item?.name || ""))
    .filter((name) => REDDIT_SUBREDDIT.test(name)).slice(0, 40);
  const queries = [...new Set((desk.queries || []).map((item) => redditQuery(item?.query)).filter(Boolean))].slice(0, 40);
  const rulesOnFile = {};
  for (const item of desk.subreddits || []) {
    if (item?.rulesRead && REDDIT_SUBREDDIT.test(String(item.name || ""))) {
      rulesOnFile[String(item.name).toLowerCase()] = Number(item.rulesReadAt || 0);
    }
  }
  const accounts = (desk.accounts || []).map((item) => String(item?.name || ""))
    .filter((name) => REDDIT_AUTHOR.test(name)).slice(0, REDDIT_ACCOUNT_LIMIT);
  if (state.watch.organization && state.watch.organization !== organization) {
    state.sent = {};
    state.rulesSent = {};
    state.readings = {};
    state.readingsSent = {};
    state.mineSent = {};
  }
  const monitored = (payload.redditThreads || []).filter((item) => item?.monitored)
    .map(redditMonitorEntry).filter(Boolean).slice(0, REDDIT_MONITOR_LIMIT);
  const kept = new Set(monitored.map((entry) => entry.postId));
  state.readings = Object.fromEntries(Object.entries(state.readings).filter(([id]) => kept.has(id)));
  state.readingsSent = Object.fromEntries(Object.entries(state.readingsSent).filter(([id]) => kept.has(id)));
  state.watch = { subreddits, queries, rulesOnFile, fetchedAt: Date.now(), organization, monitored, accounts };
}

function redditMonitorEntry(item) {
  const thread = redditLink(item.url);
  if (!thread || !/^[A-Za-z0-9_-]{1,64}$/.test(String(item.id || ""))) return null;
  const comment = redditLink(item.monitorCommentUrl);
  const counts = Object.fromEntries(REDDIT_MONITOR_COUNTS.map(([key]) => [key, redditCount(item[key])]));
  return {
    deskId: String(item.id), postId: thread.postId, subreddit: thread.subreddit, url: thread.url,
    commentId: comment?.postId === thread.postId ? comment.commentId : "",
    commentUrl: comment?.postId === thread.postId ? comment.url : "",
    title: String(item.title || "").slice(0, 300),
    checkedAt: Number.isInteger(item.checkedAt) && item.checkedAt > 0 ? item.checkedAt : 0,
    ...counts,
  };
}

function redditLink(value) {
  try {
    const parsed = new URL(String(value || ""));
    const parts = parsed.pathname.split("/").filter(Boolean);
    if (parsed.protocol !== "https:" || !/^(?:www\.|old\.|new\.)?reddit\.com$/i.test(parsed.hostname)) return null;
    if (parts[0]?.toLowerCase() !== "r" || !REDDIT_SUBREDDIT.test(parts[1] || "")) return null;
    const postId = String(parts[3] || "").toLowerCase();
    if (parts[2]?.toLowerCase() !== "comments" || !REDDIT_POST_ID.test(postId)) return null;
    const rest = parts.slice(4);
    const commentId = rest.length === 2 && /^[A-Za-z0-9_-]{1,200}$/.test(rest[0]) && REDDIT_POST_ID.test(rest[1].toLowerCase())
      ? rest[1].toLowerCase() : "";
    const base = `https://www.reddit.com/r/${parts[1]}/comments/${postId}/`;
    return {
      subreddit: parts[1], postId, commentId,
      url: commentId ? `${base}${rest[0]}/${commentId}/` : base,
    };
  } catch {
    return null;
  }
}

const redditWatch = async () => {
  const state = await redditState();
  return {
    ok: true,
    subreddits: state.watch.subreddits.map((name) => name.toLowerCase()),
    queries: state.watch.queries,
    monitored: state.watch.monitored.map((entry) => ({ postId: entry.postId, commentId: entry.commentId })),
    accounts: state.watch.accounts.map((name) => name.toLowerCase()),
  };
};

function redditMineItem(raw, account) {
  const link = redditLink(raw?.url);
  const comment = raw?.kind === "comment";
  if (!link || comment !== Boolean(link.commentId) || String(raw?.postId || "").toLowerCase() !== link.postId) return null;
  const count = (value, low) => {
    const number = redditCount(value);
    return number !== null && number >= low ? number : null;
  };
  return {
    key: link.commentId ? `${link.postId}_${link.commentId}` : link.postId,
    kind: comment ? "comment" : "post", account, postId: link.postId, commentId: link.commentId,
    subreddit: link.subreddit, url: link.url,
    title: String(raw.title || "").replace(/\s+/g, " ").trim().slice(0, 300),
    postedAt: Number.isInteger(raw.postedAt) && raw.postedAt > 0 ? raw.postedAt : 0,
    views: count(raw.views, 0), score: count(raw.score, -1e9), comments: comment ? null : count(raw.comments, 0),
  };
}

function redditMineKeep(state, item, now) {
  const previous = state.mine[item.key] || null;
  const next = {
    ...previous, ...item,
    title: item.title || previous?.title || "", postedAt: item.postedAt || previous?.postedAt || 0,
    views: item.views ?? previous?.views ?? null, score: item.score ?? previous?.score ?? null,
    comments: item.comments ?? previous?.comments ?? null,
    readAt: Number(item.readAt || previous?.readAt || 0), seenAt: Number(previous?.seenAt || 0),
  };
  const moved = !previous || REDDIT_MINE_FIELDS.some((key) => next[key] !== previous[key]);
  if (moved) next.seenAt = now;
  state.mine[item.key] = next;
  return moved || next.readAt !== Number(previous?.readAt || 0);
}

function redditMineTrim(state) {
  state.mine = redditTrim(state.mine, REDDIT_MAX_MINE, (item) => Math.max(Number(item?.seenAt || 0), Number(item?.readAt || 0)));
  state.mineSent = Object.fromEntries(Object.entries(state.mineSent).filter(([key]) => key in state.mine));
}

function redditMinePost(item) {
  const title = item.kind === "comment"
    ? `Comment in r/${item.subreddit}${item.title ? `: ${item.title}` : ""}`
    : item.title || `Post in r/${item.subreddit}`;
  const post = { url: item.url, channel: "reddit", title: title.slice(0, 300), checkedAt: item.seenAt };
  if (item.postedAt) post.postedAt = item.postedAt;
  if (item.views !== null) post.views = item.views;
  if (item.score !== null && item.score >= 0) post.likes = item.score;
  if (item.comments !== null) post.replies = item.comments;
  return post;
}

async function redditSendMine(state, base, result) {
  const items = redditMineUnsent(state).sort((one, two) => one.seenAt - two.seenAt);
  for (let start = 0; start < items.length; start += X_POSTS_BATCH) {
    const batch = items.slice(start, start + X_POSTS_BATCH);
    const reply = await api(`${base}/posts`, "POST", { posts: batch.map(redditMinePost) });
    result.linksAdded = Number(result.linksAdded || 0) + Number(reply.created || 0);
    result.linksUpdated = Number(result.linksUpdated || 0) + Number(reply.updated || 0);
    for (const item of batch) state.mineSent[item.key] = item.seenAt;
  }
}

function redditReading(raw, entry) {
  const reading = { at: Date.now() };
  for (const [key, low] of REDDIT_MONITOR_COUNTS) {
    const value = redditCount(raw?.[key]);
    reading[key] = value !== null && value >= low ? value : null;
  }
  if (!entry.commentId || String(raw?.commentId || "").toLowerCase() !== entry.commentId) {
    reading.commentViews = null;
    reading.commentScore = null;
  }
  reading.title = String(raw?.title || "").replace(/\s+/g, " ").trim().slice(0, 300);
  const found = REDDIT_MONITOR_COUNTS.some(([key]) => reading[key] !== null);
  return found ? reading : null;
}

function redditKeepReading(state, raw) {
  const entry = redditMonitoredPost(state, raw?.postId);
  const reading = entry && redditReading(raw, entry);
  if (!reading) return false;
  const previous = state.readings[entry.postId];
  const same = previous && REDDIT_MONITOR_COUNTS.every(([key]) => previous[key] === reading[key]);
  if (same && reading.at - previous.at < 10 * 60 * 1000) return false;
  state.readings[entry.postId] = reading;
  return true;
}

function redditPost(raw) {
  const id = String(raw?.id || "").toLowerCase();
  const subreddit = String(raw?.subreddit || "");
  if (!REDDIT_POST_ID.test(id) || !REDDIT_SUBREDDIT.test(subreddit)) return null;
  let url = "";
  try {
    const parsed = new URL(String(raw.url || ""));
    const parts = parsed.pathname.split("/").filter(Boolean);
    if (parsed.protocol === "https:" && /(^|\.)reddit\.com$/.test(parsed.hostname)
      && parts[0] === "r" && parts[2] === "comments" && parts[3]?.toLowerCase() === id) {
      url = `https://www.reddit.com${parsed.pathname}`;
    }
  } catch {
    url = "";
  }
  const author = String(raw.author || "");
  return {
    id, subreddit, url: url || `https://www.reddit.com/r/${subreddit}/comments/${id}/`,
    title: String(raw.title || "").replace(/\s+/g, " ").trim().slice(0, 300),
    author: REDDIT_AUTHOR.test(author) ? author : "",
    comments: redditCount(raw.comments) !== null && raw.comments >= 0 ? raw.comments : null,
    score: redditCount(raw.score),
    postedAt: Number.isInteger(raw.postedAt) && raw.postedAt > 0 ? raw.postedAt : 0,
  };
}

function redditRules(raw) {
  return (Array.isArray(raw) ? raw : []).slice(0, 30).map((rule) => ({
    title: String(rule?.title || "").replace(/\s+/g, " ").trim().slice(0, 200),
    description: String(rule?.description || "").trim().slice(0, 2000),
  })).filter((rule) => rule.title || rule.description);
}

function redditWaiting(state) {
  return redditUnsent(state).length + redditRulesUnsent(state).length + redditReadingsUnsent(state).length
    + redditMineUnsent(state).length;
}

function redditActivity(state, seen, url) {
  const kind = String(seen.page || seen.kind || "");
  const posts = Array.isArray(seen.posts) ? seen.posts : [];
  const title = String(posts[0]?.title || seen.monitor?.title || "").replace(/\s+/g, " ").trim().slice(0, 80);
  const where = kind === "search" ? `the search “${redditQuery(seen.query)}”`
    : kind === "profile" ? `u/${seen.account}`
    : kind === "rules" ? `r/${seen.subreddit} rules`
      : kind === "thread" ? (title ? `“${title}” in r/${seen.subreddit}` : `a thread in r/${seen.subreddit}`)
        : `r/${seen.subreddit}`;
  const read = kind === "rules" ? activityCount(Array.isArray(seen.rules) ? seen.rules.length : 0, "rule", "rules")
    : kind === "thread" ? [Number.isInteger(seen.monitor?.views) && activityCount(seen.monitor.views, "view", "views"),
      Number.isInteger(seen.monitor?.score) && activityCount(seen.monitor.score, "point", "points")].filter(Boolean).join(", ")
      : kind === "profile" ? activityCount(posts.length, "post or comment", "posts and comments")
      : activityCount(posts.length, "post", "posts");
  const waiting = redditWaiting(state);
  return {
    kind: "read", site: "reddit", page: where, url,
    text: `Read ${where}${read ? `: ${read}` : ""}. ${waiting ? `${waiting} waiting for the Reddit desk.` : "Nothing new for the Reddit desk."}`,
  };
}

async function redditObserve({ observation, fresh }, sender) {
  return redditSerial(async () => {
    const state = await redditState();
    const seen = observation && typeof observation === "object" ? observation : {};
    const now = Date.now();
    const kind = String(seen.page || seen.kind || "");
    const subreddit = String(seen.subreddit || "");
    let changed = false;
    if (kind === "thread" && seen.monitor && redditKeepReading(state, seen.monitor)) changed = true;
    if (kind === "rules" && Array.isArray(seen.rules) && redditWatchedSubreddit(state, subreddit)) {
      const name = state.watch.subreddits.find((item) => item.toLowerCase() === subreddit.toLowerCase());
      const rules = redditRules(seen.rules);
      const previous = state.rules[name.toLowerCase()];
      if (!previous || JSON.stringify(previous.rules) !== JSON.stringify(rules)) {
        state.rules[name.toLowerCase()] = { subreddit: name, rules, readAt: now };
        state.rules = redditTrim(state.rules, REDDIT_MAX_RULE_SETS, (entry) => entry?.readAt);
        changed = true;
      }
    }
    const searched = kind === "search" && redditWatchedQuery(state, seen.query) ? redditQuery(seen.query) : "";
    const listing = kind === "listing" && redditWatchedSubreddit(state, subreddit);
    const thread = kind === "thread" && redditWatchedSubreddit(state, subreddit);
    const postId = String(seen.postId || "").toLowerCase();
    const account = kind === "profile" ? redditAccount(state, seen.account) : "";
    if (account) {
      for (const raw of Array.isArray(seen.posts) ? seen.posts.slice(0, 100) : []) {
        const item = redditMineItem(raw, account);
        if (item && redditMineKeep(state, item, now)) changed = true;
      }
    }
    const marked = [
      account && `reddit:user:${account.toLowerCase()}`,
      listing && `reddit:sub:${subreddit.toLowerCase()}`,
      searched && `reddit:search:${searched}`,
      kind === "rules" && redditWatchedSubreddit(state, subreddit) && `reddit:rules:${subreddit.toLowerCase()}`,
      (thread || (kind === "thread" && redditMonitoredPost(state, postId))) && REDDIT_POST_ID.test(postId) && `reddit:thread:${postId}`,
    ].filter(Boolean);
    await refreshMark(marked, sender?.tab?.id, fresh === true);
    if (searched || listing || thread) {
      for (const raw of Array.isArray(seen.posts) ? seen.posts.slice(0, 100) : []) {
        const post = redditPost(raw);
        if (!post) continue;
        if (listing && post.subreddit.toLowerCase() !== subreddit.toLowerCase()) continue;
        if (thread && post.id !== String(seen.postId || "").toLowerCase()) continue;
        const previous = state.posts[post.id] || {};
        state.posts[post.id] = {
          ...previous, ...post,
          title: post.title || previous.title || "",
          author: post.author || previous.author || "",
          comments: post.comments ?? previous.comments ?? null,
          score: post.score ?? previous.score ?? null,
          postedAt: post.postedAt || previous.postedAt || 0,
          opened: Boolean(previous.opened || thread),
          listed: Boolean(previous.listed || listing),
          queries: [...new Set([...(previous.queries || []), ...(searched ? [searched] : [])])].slice(0, 10),
          at: now, seenAt: now,
        };
        changed = true;
      }
    }
    if (changed) {
      state.posts = redditTrim(state.posts, REDDIT_MAX_POSTS);
      redditMineTrim(state);
      await redditSave(state);
    }
    if (fresh === true && marked.length) void activityReport(redditActivity(state, seen, sender?.tab?.url), marked[0]);
    return { ok: true };
  });
}

const redditError = ({ message, page }) => redditSerial(async () => {
  const state = await redditState();
  redditRemember(state, `${String(page || "").slice(0, 60)} ${String(message || "")}`.trim());
  await redditSave(state);
  return { ok: true };
});

const redditStatsNow = () => redditSerial(async () => {
  const state = await redditState();
  const current = await settings();
  if (current.sessionToken && Date.now() - state.watch.fetchedAt > REDDIT_WATCH_STALE) {
    try {
      await redditFetchWatch(state);
    } catch (error) {
      redditRemember(state, `Watch list: ${error?.message || "request failed"}`);
    }
    await redditSave(state);
  }
  return redditStats();
});

const redditRefresh = () => redditSerial(async () => {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const state = await redditState();
  try {
    await redditFetchWatch(state);
  } catch (error) {
    redditRemember(state, `Watch list: ${error?.message || "request failed"}`);
    await redditSave(state);
    throw error;
  }
  await redditSave(state);
  return redditStats();
});

async function redditSendReadings(state, base, result) {
  for (const entry of redditReadingsUnsent(state)) {
    const reading = state.readings[entry.postId];
    const body = { checkedAt: reading.at };
    for (const [key] of REDDIT_MONITOR_COUNTS) if (reading[key] !== null) body[key] = reading[key];
    if (!entry.title && reading.title) body.title = reading.title;
    try {
      await api(`${base}/reddit/${encodeURIComponent(entry.deskId)}`, "PATCH", body);
      result.readings += 1;
    } catch (error) {
      if (!error?.status || error.status === 401 || error.status >= 500) throw error;
      result.failed += 1;
      redditRemember(state, `r/${entry.subreddit} ${entry.postId} views: ${error.message || "request failed"}`);
    }
    state.readingsSent[entry.postId] = reading.at;
  }
}

async function redditSync(state) {
  const result = { at: Date.now(), organization: "", threads: 0, already: 0, rules: 0, readings: 0, failed: 0, error: "", sentBefore: 0, unmatched: 0, linksAdded: 0, linksUpdated: 0 };
  try {
    await redditFetchWatch(state);
    const candidates = redditCandidates(state);
    result.sentBefore = candidates.filter((post) => state.sent[post.id]).length;
    result.unmatched = Object.keys(state.posts).length - candidates.length;
    const organization = state.watch.organization;
    result.organization = organization;
    const base = `/api/marketing/${encodeURIComponent(organization)}`;
    for (const post of redditUnsent(state).slice(0, REDDIT_SYNC_LIMIT)) {
      const body = {
        url: post.url, title: post.title, status: "to_review", source: "extension",
        author: post.author, comments: post.comments, score: post.score,
        postedAt: post.postedAt || null,
      };
      try {
        await api(`${base}/reddit`, "POST", body);
        result.threads += 1;
      } catch (error) {
        if (!error?.status || error.status === 401 || error.status >= 500) throw error;
        if (error.code === "reddit_thread_exists") result.already += 1;
        else {
          result.failed += 1;
          redditRemember(state, `r/${post.subreddit} ${post.id}: ${error.message || "request failed"}`);
        }
      }
      state.sent[post.id] = Date.now();
    }
    for (const entry of redditRulesUnsent(state)) {
      await api(`${base}/reddit-rules`, "POST", { subreddit: entry.subreddit, rules: entry.rules });
      state.rulesSent[entry.subreddit.toLowerCase()] = entry.readAt;
      state.watch.rulesOnFile[entry.subreddit.toLowerCase()] = Date.now();
      result.rules += 1;
    }
    await redditSendReadings(state, base, result);
    await redditSendMine(state, base, result);
  } catch (error) {
    result.error = error?.code || "request_failed";
    redditRemember(state, `Reddit desk sync: ${error?.message || "request failed"}`);
  }
  state.sent = redditTrim(state.sent, REDDIT_MAX_SENT, (at) => at);
  state.lastSync = result;
}

const redditSyncNow = () => redditSerial(async () => {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const state = await redditState();
  await redditSync(state);
  await redditSave(state);
  return redditStats();
});

const redditClearErrors = () => redditSerial(async () => {
  const state = await redditState();
  state.errors = [];
  await redditSave(state);
  return redditStats();
});

let redditChecking = false;
let redditMineChecking = false;

async function redditBase() {
  return `/api/marketing/${encodeURIComponent(await xOrganization())}`;
}

const redditMonitorAdd = ({ url }) => redditSerial(async () => {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const link = redditLink(url);
  if (!link) throw new ApiError("invalid_reddit_thread_url", 400);
  const state = await redditState();
  const base = await redditBase();
  try {
    await api(`${base}/reddit`, "POST", { url: link.url, status: "to_review", source: "extension", monitored: true });
  } catch (error) {
    if (error?.code !== "reddit_thread_exists" || !error.payload?.threadId) throw error;
    await api(`${base}/reddit/${encodeURIComponent(error.payload.threadId)}`, "PATCH",
      { monitored: true, ...(link.commentId ? { commentUrl: link.url } : {}) });
  }
  await redditFetchWatch(state);
  await redditSave(state);
  return redditStats();
});

const redditMonitorRemove = ({ deskId }) => redditSerial(async () => {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const state = await redditState();
  const entry = state.watch.monitored.find((item) => item.deskId === String(deskId || ""));
  if (!entry) throw new ApiError("reddit_thread_not_found", 404);
  await api(`${await redditBase()}/reddit/${encodeURIComponent(entry.deskId)}`, "PATCH", { monitored: false });
  await redditFetchWatch(state);
  await redditSave(state);
  return redditStats();
});

async function redditWaitForTab(tabId) {
  const deadline = Date.now() + CAPTURE_LOAD_MS;
  while (Date.now() < deadline) {
    const tab = await chrome.tabs.get(tabId).catch(() => null);
    if (!tab) return false;
    if (tab.status === "complete") return true;
    await captureSleep(400);
  }
  return false;
}

async function redditReadTab(entry) {
  const tab = await chrome.tabs.create({ url: entry.commentUrl || entry.url, active: false });
  try {
    if (!tab?.id || !(await redditWaitForTab(tab.id))) return null;
    let last = null;
    for (let attempt = 0; attempt < REDDIT_CHECK_TRIES; attempt += 1) {
      await captureSleep(REDDIT_CHECK_RETRY_MS);
      const reply = await chrome.tabs.sendMessage(tab.id, { type: "fm-reddit-read", postId: entry.postId, commentId: entry.commentId || "" }).catch(() => null);
      if (!reply?.ok || reply.monitor?.postId !== entry.postId) continue;
      last = reply.monitor;
      if (last.views !== null && (!entry.commentId || last.commentScore !== null)) break;
    }
    return last;
  } finally {
    if (tab?.id) await chrome.tabs.remove(tab.id).catch(() => null);
  }
}

async function redditCheck() {
  if (redditChecking) throw new ApiError("reddit_check_running", 409);
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  redditChecking = true;
  try {
    const entries = await redditSerial(async () => {
      const state = await redditState();
      await redditFetchWatch(state);
      await redditSave(state);
      return state.watch.monitored.slice(0, REDDIT_CHECK_LIMIT);
    });
    const read = [];
    let missed = 0;
    for (const entry of entries) {
      const monitor = await redditReadTab(entry).catch(() => null);
      if (monitor) read.push(monitor);
      else missed += 1;
    }
    return await redditSerial(async () => {
      const state = await redditState();
      for (const monitor of read) redditKeepReading(state, monitor);
      const result = { at: Date.now(), organization: state.watch.organization, threads: 0, already: 0, rules: 0, readings: 0, failed: 0, error: "" };
      try {
        await redditSendReadings(state, await redditBase(), result);
      } catch (error) {
        result.error = error?.code || "request_failed";
        redditRemember(state, `Reddit check: ${error?.message || "request failed"}`);
      }
      state.lastCheck = { at: result.at, checked: entries.length, read: read.length, missed, sent: result.readings, error: result.error };
      await redditSave(state);
      return redditStats();
    });
  } finally {
    redditChecking = false;
    void redditMineCheck().catch(() => null);
  }
}

async function redditMineCheck(account = "") {
  if (redditMineChecking) return 0;
  redditMineChecking = true;
  try {
    if (!(await settings()).sessionToken) return 0;
    const due = await redditSerial(async () => {
      const state = await redditState();
      const now = Date.now();
      const lower = account.toLowerCase();
      const items = Object.values(state.mine)
        .filter((item) => (!lower || item.account.toLowerCase() === lower) && now - Number(item.readAt || 0) >= REDDIT_MINE_FRESH_MS)
        .sort((one, two) => Number(one.readAt || 0) - Number(two.readAt || 0) || two.postedAt - one.postedAt)
        .slice(0, REDDIT_MINE_CHECK_LIMIT);
      if (!items.length) return [];
      state.mineCheck = { at: now, account, total: items.length, read: 0, missed: 0, linksUpdated: 0, error: "", done: false };
      await redditSave(state);
      return items;
    });
    for (const item of due) {
      const monitor = await redditReadTab({ url: item.url, postId: item.postId, commentId: item.commentId }).catch(() => null);
      await redditSerial(async () => {
        const state = await redditState();
        const check = state.mineCheck || {};
        const current = state.mine[item.key];
        if (!current) return;
        const views = item.commentId ? monitor?.commentViews : monitor?.views;
        const read = redditMineItem({
          ...current, views: views ?? null,
          score: item.commentId ? monitor?.commentScore ?? null : monitor?.score ?? null,
          comments: item.commentId ? null : monitor?.comments ?? null,
        }, current.account);
        if (read) redditMineKeep(state, { ...read, readAt: Date.now() }, Date.now());
        if (Number.isInteger(views)) check.read = Number(check.read || 0) + 1;
        else check.missed = Number(check.missed || 0) + 1;
        try {
          const result = {};
          await redditSendMine(state, await redditBase(), result);
          check.linksUpdated = Number(check.linksUpdated || 0) + Number(result.linksUpdated || 0) + Number(result.linksAdded || 0);
        } catch (error) {
          check.error = error?.code || "request_failed";
          redditRemember(state, `Reddit views: ${error?.message || "request failed"}`);
        }
        state.mineCheck = check;
        await redditSave(state);
        await redditPostsBroadcast(check);
      });
    }
    if (due.length) {
      await redditSerial(async () => {
        const state = await redditState();
        state.mineCheck = { ...(state.mineCheck || {}), done: true, finishedAt: Date.now() };
        await redditSave(state);
        await redditPostsBroadcast(state.mineCheck);
      });
    }
    return due.length;
  } finally {
    redditMineChecking = false;
  }
}

async function redditPostsBroadcast(check) {
  for (const tab of await chrome.tabs.query({ url: FORKMESH_ORIGINS.map((origin) => `${origin}/dashboard/marketing*`) }).catch(() => [])) {
    if (Number.isInteger(tab.id)) await chrome.tabs.sendMessage(tab.id, { type: "fm-reddit-posts-progress", check }).catch(() => null);
  }
}

async function redditMineRecord(state, base, account, items, result) {
  const now = Date.now();
  for (const raw of Array.isArray(items) ? items.slice(0, 100) : []) {
    const item = redditMineItem(raw, account);
    if (!item) continue;
    result.found += 1;
    redditMineKeep(state, item, now);
  }
  redditMineTrim(state);
  await redditSendMine(state, base, result);
}

async function redditProfileRead(tabId) {
  const ask = () => chrome.tabs.sendMessage(tabId, { type: "fm-reddit-profile-read" }).catch(() => null);
  let reply = await ask();
  if (!reply?.ok) {
    await chrome.scripting.executeScript({ target: { tabId }, files: ["reddit.js"] }).catch(() => null);
    reply = await ask();
  }
  return reply?.ok && REDDIT_AUTHOR.test(String(reply.account || "")) ? reply : null;
}

function redditProfileText(result) {
  const name = `u/${result.account}`;
  const lines = [];
  if (result.added) lines.push(`${name} is now one of the Reddit desk's accounts.`);
  if (result.addError) {
    lines.push(result.addError === "organization_admin_required"
      ? `Only organization owners and admins can add ${name} to the Reddit desk's accounts; its links are recorded anyway.`
      : `${name} could not be added to the Reddit desk's accounts (${refreshError(result.addError)}); its links are recorded anyway.`);
  }
  lines.push(result.found
    ? `Read ${activityCount(result.found, "post or comment", "posts and comments")} by ${name}: ${activityCount(result.linksAdded, "new link", "new links")} and ${result.linksUpdated} updated in Marketing posts.`
    : `No posts or comments by ${name} were found on this page yet: let it load, or open its Posts or Comments tab, then try again.`);
  if (result.checking) lines.push(`Reading the views of ${activityCount(result.checking, "link", "links")} in background tabs; Marketing updates as each is read.`);
  else if (result.busy) lines.push("A views check is already running; press Update again when it ends to read the rest.");
  else if (result.found) lines.push("Every link's views were read in the last 30 minutes.");
  return lines.join(" ");
}

async function redditProfileUpdate({ tabId }) {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const read = await redditProfileRead(Number(tabId));
  if (!read) throw new ApiError("reddit_profile_not_read", 409);
  const result = { at: Date.now(), account: String(read.account), found: 0, added: false, addError: "", linksAdded: 0, linksUpdated: 0, checking: 0, busy: false };
  await redditSerial(async () => {
    const state = await redditState();
    await redditFetchWatch(state);
    const base = `/api/marketing/${encodeURIComponent(state.watch.organization)}`;
    if (!redditAccount(state, result.account)) {
      try {
        await api(`${base}/settings`, "PUT", { reddit: { accounts: [...state.watch.accounts, result.account] } });
        result.added = true;
        await redditFetchWatch(state);
      } catch (error) {
        if (!error?.status || error.status === 401 || error.status >= 500) throw error;
        result.addError = error.code || "request_failed";
      }
    }
    const account = redditAccount(state, result.account) || result.account;
    await redditMineRecord(state, base, account, read.items, result);
    await redditSave(state);
  });
  const state = await redditState();
  const lower = result.account.toLowerCase();
  result.busy = redditMineChecking;
  result.checking = redditMineChecking ? 0 : Math.min(REDDIT_MINE_CHECK_LIMIT, Object.values(state.mine).filter((item) =>
    item.account.toLowerCase() === lower && Date.now() - Number(item.readAt || 0) >= REDDIT_MINE_FRESH_MS).length);
  if (result.checking) void redditMineCheck(result.account).catch(() => null);
  await refreshMark([`reddit:user:${lower}`], Number(tabId), true);
  return { ...(await redditStats()), profile: result, text: redditProfileText(result) };
}

const REDDIT_PROFILE_TABS = ["submitted", "comments"];
let redditAccountsUpdating = false;

async function redditProfileTab(url) {
  const tab = await chrome.tabs.create({ url, active: false });
  try {
    if (!tab?.id || !(await redditWaitForTab(tab.id))) return null;
    let last = null;
    for (let attempt = 0; attempt < REDDIT_CHECK_TRIES; attempt += 1) {
      await captureSleep(REDDIT_CHECK_RETRY_MS);
      const reply = await redditProfileRead(tab.id);
      if (!reply) continue;
      last = reply;
      if (Array.isArray(reply.items) && reply.items.length) break;
    }
    return last;
  } finally {
    if (tab?.id) await chrome.tabs.remove(tab.id).catch(() => null);
  }
}

function redditAccountsText(result) {
  const lines = result.accounts.map((entry) => {
    const name = `u/${entry.account}`;
    if (entry.error) return `${name}: Marketing refused the links (${refreshError(entry.error)}).`;
    if (!entry.read) return `${name}: the profile did not load; open it once in a tab, then try again.`;
    return entry.found
      ? `${name}: ${activityCount(entry.found, "post or comment", "posts and comments")} read, ${activityCount(entry.linksAdded, "new link", "new links")} and ${entry.linksUpdated} updated.`
      : `${name}: no posts or comments were found on the profile.`;
  });
  if (result.checking) lines.push(`Reading the views of ${activityCount(result.checking, "link", "links")} in background tabs; this page updates as they are read.`);
  else if (result.busy) lines.push("A views check is already running; press Update again when it ends to read the rest.");
  return lines.join(" ");
}

async function redditAccountsUpdate({ organization }) {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  if (redditAccountsUpdating) throw new ApiError("reddit_accounts_running", 409);
  redditAccountsUpdating = true;
  try {
    if (organization && String(organization) !== await xOrganization()) throw new ApiError("reddit_organization_mismatch", 409);
    const accounts = await redditSerial(async () => {
      const state = await redditState();
      await redditFetchWatch(state);
      await redditSave(state);
      return [...state.watch.accounts];
    });
    if (!accounts.length) throw new ApiError("reddit_accounts_empty", 409);
    const result = { at: Date.now(), accounts: [], checking: 0, busy: false };
    for (const account of accounts) {
      const entry = { account, read: false, found: 0, linksAdded: 0, linksUpdated: 0, error: "" };
      result.accounts.push(entry);
      for (const tab of REDDIT_PROFILE_TABS) {
        const read = await redditProfileTab(`https://www.reddit.com/user/${encodeURIComponent(account)}/${tab}/`).catch(() => null);
        if (!read || read.account.toLowerCase() !== account.toLowerCase()) continue;
        entry.read = true;
        try {
          await redditSerial(async () => {
            const state = await redditState();
            await redditMineRecord(state, await redditBase(), account, read.items, entry);
            await redditSave(state);
          });
        } catch (error) {
          if (!error?.status || error.status === 401) throw error;
          entry.error = error.code || "request_failed";
          break;
        }
      }
      await refreshMark([`reddit:user:${account.toLowerCase()}`], null, entry.read);
    }
    const state = await redditState();
    result.busy = redditMineChecking;
    result.checking = redditMineChecking ? 0 : Math.min(REDDIT_MINE_CHECK_LIMIT, Object.values(state.mine).filter((item) =>
      Date.now() - Number(item.readAt || 0) >= REDDIT_MINE_FRESH_MS).length);
    if (result.checking) void redditMineCheck("").catch(() => null);
    return { ok: true, ...result, text: redditAccountsText(result) };
  } finally {
    redditAccountsUpdating = false;
  }
}

const redditAutoCheck = ({ on }) => redditSerial(async () => {
  const state = await redditState();
  state.autoCheck = on === true;
  await redditSave(state);
  if (state.autoCheck) await chrome.alarms.create(REDDIT_CHECK_ALARM, { periodInMinutes: REDDIT_CHECK_EVERY_MINUTES });
  else await chrome.alarms.clear(REDDIT_CHECK_ALARM);
  return redditStats();
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === REDDIT_CHECK_ALARM) void redditCheck().catch(() => null);
});

const WALK_KEY = "redditWalk";
const WALK_MAX_STOPS = 24;
const WALK_PAGE_MS = 9000;
const WALK_SETTLE_MS = 1500;
const WALK_GAP_MS = [2500, 6000];
const WALK_TTL = 60 * 60 * 1000;

let redditWalking = false;
let redditWalkStopped = false;

const walkWait = (low, high) => captureSleep(Math.round(low + Math.random() * (high - low)));

function redditWalkPlan(state) {
  const subreddits = state.watch.subreddits.map((name) => ({
    kind: "subreddit", label: `r/${name}`,
    url: `https://www.reddit.com/r/${encodeURIComponent(name)}/new/`,
  }));
  const searches = state.watch.queries.map((query) => ({
    kind: "search", label: query,
    url: `https://www.reddit.com/search/?q=${encodeURIComponent(query)}&sort=new&t=week`,
  }));
  const stops = [];
  while ((subreddits.length || searches.length) && stops.length < WALK_MAX_STOPS) {
    if (subreddits.length) stops.push(subreddits.shift());
    if (searches.length && stops.length < WALK_MAX_STOPS) stops.push(searches.shift());
  }
  for (const name of state.watch.subreddits) {
    if (stops.length >= WALK_MAX_STOPS) break;
    if (state.rules[name.toLowerCase()] || state.watch.rulesOnFile[name.toLowerCase()]) continue;
    stops.push({ kind: "rules", label: `r/${name} rules`,
      url: `https://old.reddit.com/r/${encodeURIComponent(name)}/about/rules` });
  }
  return stops.map((stop, index) => ({ ...stop, index, state: "waiting", posts: 0 }));
}

async function redditWalkState() {
  const stored = (await chrome.storage.session.get([WALK_KEY]))[WALK_KEY] || null;
  if (!stored || Date.now() - Number(stored.startedAt || 0) > WALK_TTL) return null;
  return stored;
}

async function redditWalkSave(walk) {
  await chrome.storage.session.set({ [WALK_KEY]: walk });
  for (const tab of await chrome.tabs.query({ url: FORKMESH_ORIGINS.map((origin) => `${origin}/dashboard/marketing*`) }).catch(() => [])) {
    if (Number.isInteger(tab.id) && tab.id !== walk.tabId) {
      await chrome.tabs.sendMessage(tab.id, { type: "fm-reddit-walk-update", walk }).catch(() => null);
    }
  }
}

async function redditWalkStop() {
  redditWalkStopped = true;
  const walk = await redditWalkState();
  return { ok: true, walk: walk ? { ...walk, state: walk.state === "running" ? "stopping" : walk.state } : null };
}

async function redditWalkStatus() {
  return { ok: true, walk: await redditWalkState(), walking: redditWalking };
}

async function redditWalkPace(tabId) {
  await chrome.scripting.executeScript({ target: { tabId }, files: ["reddit-walk.js"] });
  const [first] = await chrome.scripting.executeScript({
    target: { tabId },
    func: (ms) => (globalThis.__forkmeshRedditWalk ? globalThis.__forkmeshRedditWalk({ ms }) : null),
    args: [WALK_PAGE_MS],
  });
  return first?.result || null;
}

async function redditWalkVisit(walk, stop) {
  await chrome.tabs.update(walk.tabId, { url: stop.url, active: true });
  if (!(await redditWaitForTab(walk.tabId))) return { ok: false, posts: 0 };
  await captureSleep(WALK_SETTLE_MS);
  await redditWalkPace(walk.tabId).catch(() => null);
  const reply = await chrome.tabs.sendMessage(walk.tabId, { type: "fm-reddit-scan" }).catch(() => null);
  return reply || { ok: false, posts: 0 };
}

async function redditWalkRun(walk) {
  try {
    for (const stop of walk.stops) {
      if (redditWalkStopped) break;
      stop.state = "reading";
      walk.index = stop.index;
      await redditWalkSave(walk);
      const reply = await redditWalkVisit(walk, stop).catch(() => null);
      stop.posts = Number(reply?.posts || 0);
      stop.state = reply?.ok ? "read" : "missed";
      walk.posts += stop.posts;
      await redditWalkSave(walk);
      if (redditWalkStopped) break;
      await walkWait(WALK_GAP_MS[0], WALK_GAP_MS[1]);
    }
    walk.synced = await redditSerial(async () => {
      const state = await redditState();
      await redditSync(state);
      await redditSave(state);
      return state.lastSync;
    });
    walk.state = redditWalkStopped ? "stopped" : "done";
  } catch (error) {
    walk.state = "error";
    walk.error = error?.code || "reddit_walk_failed";
    walk.message = String(error?.message || "The walk stopped").slice(0, 200);
  } finally {
    walk.finishedAt = Date.now();
    redditWalking = false;
    if (Number.isInteger(walk.tabId)) await chrome.tabs.remove(walk.tabId).catch(() => null);
    if (Number.isInteger(walk.sourceTabId)) await chrome.tabs.update(walk.sourceTabId, { active: true }).catch(() => null);
    await redditWalkSave(walk);
  }
}

async function redditWalkStart(request, sender) {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  if (redditWalking) throw new ApiError("reddit_walk_running", 409);
  const state = await redditSerial(async () => {
    const current = await redditState();
    await redditFetchWatch(current);
    await redditSave(current);
    return current;
  });
  const stops = redditWalkPlan(state);
  if (!stops.length) throw new ApiError("reddit_watch_empty", 400);
  redditWalking = true;
  redditWalkStopped = false;
  try {
    const tab = await chrome.tabs.create({ url: stops[0].url, active: true });
    const walk = {
      id: crypto.randomUUID(), organization: state.watch.organization,
      startedAt: Date.now(), finishedAt: 0, state: "running", stops, index: 0, posts: 0,
      tabId: tab.id, sourceTabId: Number.isInteger(sender?.tab?.id) ? sender.tab.id : null,
      error: "", message: "", synced: null,
    };
    await redditWalkSave(walk);
    void redditWalkRun(walk);
    return { ok: true, walk };
  } catch (error) {
    redditWalking = false;
    throw error;
  }
}

async function openRedditDesk() {
  const current = await settings();
  const state = await redditState();
  const org = state.watch.organization || current.organization;
  const query = new URLSearchParams({ ...(org ? { org } : {}), page: "reddit" });
  const url = `${current.baseUrl}/dashboard/marketing?${query}`;
  await chrome.tabs.create({ url });
  return { ok: true, url };
}

const CAPTURE_HISTORY_KEY = "marketingCaptures";
const CAPTURE_JOBS_KEY = "marketingCaptureJobs";
const CAPTURE_HISTORY = 20;
const CAPTURE_JOB_TTL = 60 * 60 * 1000;
const CAPTURE_SETTLE_MS = 1500;
const CAPTURE_RETRY_MS = 2000;
const CAPTURE_TRIES = 5;
const CAPTURE_LOAD_MS = 30000;
const CAPTURE_PAGE = "capture.html";
const CAPTURE_SERVER_CHANNELS = new Set(["discord"]);
const FORKMESH_ORIGINS = ["https://app.forkmesh.com", "https://forkmesh.com"];

const captureSleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function captureUrl(value) {
  try {
    const url = new URL(String(value || ""));
    return url.protocol === "https:" ? url.href : "";
  } catch {
    return "";
  }
}

const captureOrigin = (url) => `${new URL(url).origin}/*`;

function captureHostMatches(url, domains) {
  try {
    const host = new URL(url).hostname.toLowerCase();
    return domains.some((domain) => host === domain || host.endsWith(`.${domain}`));
  } catch {
    return false;
  }
}

async function captureOrganization(requested) {
  const clean = String(requested || "").trim();
  if (/^[a-z][a-z0-9-]{0,62}$/.test(clean)) return clean;
  return xOrganization();
}

async function captureChannels(organization) {
  const payload = await api(`/api/marketing/${encodeURIComponent(organization)}/settings`);
  const catalog = new Map((payload.channelCatalog || []).map((item) => [item.id, item]));
  const refreshable = new Set(payload.refreshableChannels || []);
  return {
    catalog,
    channels: (payload.settings?.channels || []).filter((item) => catalog.has(item.id)).map((item) => ({
      id: item.id,
      name: item.displayName || catalog.get(item.id).name,
      category: catalog.get(item.id).category || "",
      url: captureUrl(item.profileUrl),
      enabled: item.enabled !== false,
      hosts: catalog.get(item.id).hosts || [],
      capturedAt: Number(item.capture?.at || 0),
      refreshedAt: Number(item.feed?.lastReadAt || 0),
      refreshable: refreshable.has(item.id),
      server: CAPTURE_SERVER_CHANNELS.has(item.id),
    })),
  };
}

async function captureJobs() {
  const stored = (await chrome.storage.session.get([CAPTURE_JOBS_KEY]))[CAPTURE_JOBS_KEY] || {};
  const now = Date.now();
  return Object.fromEntries(Object.entries(stored).filter(([, job]) => now - Number(job?.startedAt || 0) < CAPTURE_JOB_TTL));
}

async function captureSave(job) {
  const jobs = await captureJobs();
  jobs[job.id] = job;
  await chrome.storage.session.set({ [CAPTURE_JOBS_KEY]: jobs });
}

const capturePublic = (job) => ({
  id: job.id, organization: job.organization, channel: job.channel, name: job.name,
  url: job.url, state: job.state, error: job.error || "", message: job.message || "",
  startedAt: job.startedAt, finishedAt: job.finishedAt || 0, result: job.result || null,
});

async function captureNotify(job) {
  if (!Number.isInteger(job.sourceTabId)) return;
  try {
    await chrome.tabs.sendMessage(job.sourceTabId, { type: "fm-marketing-capture-update", job: capturePublic(job) });
  } catch {
  }
}

async function captureRemember(job) {
  const stored = (await chrome.storage.local.get([CAPTURE_HISTORY_KEY]))[CAPTURE_HISTORY_KEY];
  const history = Array.isArray(stored) ? stored : [];
  const entry = capturePublic(job);
  await chrome.storage.local.set({
    [CAPTURE_HISTORY_KEY]: [entry, ...history.filter((item) => item.id !== job.id)].slice(0, CAPTURE_HISTORY),
  });
}

async function captureUpdate(job, changes) {
  Object.assign(job, changes);
  await captureSave(job);
  await captureNotify(job);
  if (job.state === "done" || job.state === "error") await captureRemember(job);
  if (job.state === "done") await refreshMark([`channel:${job.channel}`]);
  if (!job.panel && (job.state === "done" || job.state === "error")) {
    void activityReport({
      kind: job.state === "done" ? "sent" : "failed", site: refreshBareHost(refreshUrl(job.url)?.hostname), page: job.name, url: job.url,
      text: job.state === "done" ? `${job.name} read and recorded in ${job.organization} Marketing.` : `${job.name}: ${refreshError(job.error, job.message)}`,
    });
  }
}

async function captureWaitForTab(tabId) {
  const deadline = Date.now() + CAPTURE_LOAD_MS;
  while (Date.now() < deadline) {
    const tab = await chrome.tabs.get(tabId).catch(() => null);
    if (!tab) throw new ApiError("capture_tab_closed", 0);
    if (tab.status === "complete" && captureUrl(tab.url)) return tab;
    await captureSleep(400);
  }
  throw new ApiError("capture_page_timeout", 0);
}

async function captureRead(tabId) {
  await chrome.scripting.executeScript({ target: { tabId }, files: ["marketing-capture.js"] });
  const [first] = await chrome.scripting.executeScript({
    target: { tabId },
    func: () => (globalThis.__forkmeshMarketingRead ? globalThis.__forkmeshMarketingRead() : null),
  });
  return first?.result || null;
}

const captureHasNumbers = (observation) =>
  Boolean(observation && (Object.keys(observation.metrics || {}).length || observation.rating));

async function captureObserve(tabId) {
  let observation = null;
  for (let attempt = 0; attempt < CAPTURE_TRIES; attempt += 1) {
    await captureSleep(attempt ? CAPTURE_RETRY_MS : CAPTURE_SETTLE_MS);
    await captureWaitForTab(tabId);
    observation = await captureRead(tabId);
    if (captureHasNumbers(observation)) return observation;
  }
  if (observation?.signInNeeded) throw new ApiError("capture_sign_in_needed", 0);
  throw new ApiError("capture_found_no_numbers", 0);
}

async function capturePost(organization, channel, observation) {
  const body = {
    channel, url: observation.url, title: observation.title, description: observation.description,
    image: observation.image, handle: observation.handle, feedUrl: observation.feedUrl,
    metrics: observation.metrics || {}, rating: observation.rating || 0,
    lastPostAt: observation.lastPostAt || 0, postsLast30d: observation.postsLast30d || 0,
    recent: observation.recent || [], signals: observation.signals || [],
  };
  return api(`/api/marketing/${encodeURIComponent(organization)}/capture`, "POST", body);
}

async function captureRun(job) {
  try {
    const observation = await captureObserve(job.tabId);
    const result = await capturePost(job.organization, job.channel, observation);
    const posts = job.channel === "x" ? await xCapturePosts(job.organization, null) : null;
    await captureUpdate(job, {
      state: "done", finishedAt: Date.now(),
      result: { metrics: result.metrics || {}, capture: result.capture || {}, posts },
    });
    if (job.openedTab && job.closeWhenDone) {
      if (Number.isInteger(job.sourceTabId)) await chrome.tabs.update(job.sourceTabId, { active: true }).catch(() => null);
      await chrome.tabs.remove(job.tabId).catch(() => null);
    }
  } catch (error) {
    await captureUpdate(job, {
      state: "error", finishedAt: Date.now(),
      error: error?.code || "capture_failed", message: String(error?.message || "capture failed").slice(0, 200),
    });
  }
}

async function captureRefresh(job) {
  try {
    const payload = await api(`/api/marketing/${encodeURIComponent(job.organization)}/refresh`, "POST", { channel: job.channel });
    await captureUpdate(job, {
      state: "done", finishedAt: Date.now(),
      result: { metrics: payload.refresh?.metrics || {}, capture: {} },
    });
  } catch (error) {
    await captureUpdate(job, {
      state: "error", finishedAt: Date.now(),
      error: error?.code || "capture_failed", message: String(error?.message || "capture failed").slice(0, 200),
    });
  }
}

async function captureStart({ organization, channel }, sender) {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const org = await captureOrganization(organization);
  const { channels } = await captureChannels(org);
  const target = channels.find((item) => item.id === String(channel || ""));
  if (!target) throw new ApiError("channel_not_configured", 404);
  if (!target.url) throw new ApiError("channel_url_required", 400);
  const job = {
    id: crypto.randomUUID(), organization: org, channel: target.id, name: target.name, url: target.url,
    sourceTabId: Number.isInteger(sender?.tab?.id) ? sender.tab.id : null,
    startedAt: Date.now(), openedTab: true, closeWhenDone: true, state: "opening",
  };
  if (target.server) {
    Object.assign(job, { openedTab: false, closeWhenDone: false });
    await captureUpdate(job, { state: "reading" });
    void captureRefresh(job);
    return { ok: true, job: capturePublic(job) };
  }
  if (!(await chrome.permissions.contains({ origins: [captureOrigin(target.url)] }))) {
    const tab = await chrome.tabs.create({ url: chrome.runtime.getURL(`${CAPTURE_PAGE}?job=${job.id}`) });
    await captureUpdate(job, { tabId: tab.id, state: "permission" });
    return { ok: true, job: capturePublic(job) };
  }
  const tab = await chrome.tabs.create({ url: target.url, active: true });
  await captureUpdate(job, { tabId: tab.id, state: "reading" });
  void captureRun(job);
  return { ok: true, job: capturePublic(job) };
}

async function captureJob({ job: id }) {
  const job = (await captureJobs())[String(id || "")];
  if (!job) throw new ApiError("capture_job_expired", 404);
  return { ok: true, job: capturePublic(job), origin: captureOrigin(job.url) };
}

async function captureContinue({ job: id, granted }, sender) {
  const job = (await captureJobs())[String(id || "")];
  if (!job || job.state !== "permission") throw new ApiError("capture_job_expired", 404);
  if (!granted || !(await chrome.permissions.contains({ origins: [captureOrigin(job.url)] }))) {
    await captureUpdate(job, { state: "error", finishedAt: Date.now(), error: "permission_denied", message: "Chrome did not grant access to that site." });
    return { ok: true, job: capturePublic(job) };
  }
  const tabId = Number.isInteger(sender?.tab?.id) ? sender.tab.id : job.tabId;
  await chrome.tabs.update(tabId, { url: job.url, active: true });
  await captureUpdate(job, { tabId, state: "reading" });
  void captureRun(job);
  return { ok: true, job: capturePublic(job) };
}

async function captureTab({ organization, channel, tabId }) {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const tab = await chrome.tabs.get(Number(tabId)).catch(() => null);
  const url = captureUrl(tab?.url);
  if (!tab || !url) throw new ApiError("invalid_url", 400);
  const org = await captureOrganization(organization);
  const { channels, catalog } = await captureChannels(org);
  const target = channels.find((item) => item.id === String(channel || ""));
  const entry = catalog.get(String(channel || ""));
  if (!entry) throw new ApiError("invalid_channel", 400);
  const job = {
    id: crypto.randomUUID(), organization: org, channel: entry.id, name: target?.name || entry.name, url,
    sourceTabId: null, tabId: tab.id, startedAt: Date.now(), openedTab: false, closeWhenDone: false, state: "reading",
  };
  await captureSave(job);
  if (CAPTURE_SERVER_CHANNELS.has(entry.id)) {
    await captureRefresh(job);
    return { ok: true, job: capturePublic(job) };
  }
  try {
    const observation = await captureRead(tab.id);
    if (!captureHasNumbers(observation)) {
      throw new ApiError(observation?.signInNeeded ? "capture_sign_in_needed" : "capture_found_no_numbers", 0);
    }
    const result = await capturePost(org, entry.id, observation);
    const posts = entry.id === "x" ? await xCapturePosts(org, tab.id) : null;
    await captureUpdate(job, { state: "done", finishedAt: Date.now(), result: { metrics: result.metrics || {}, capture: result.capture || {}, posts } });
  } catch (error) {
    await captureUpdate(job, { state: "error", finishedAt: Date.now(), error: error?.code || "capture_failed", message: String(error?.message || "capture failed").slice(0, 200) });
  }
  return { ok: true, job: capturePublic(job) };
}

async function captureSetup({ organization }) {
  const org = await captureOrganization(organization);
  const { channels, catalog } = await captureChannels(org);
  const history = (await chrome.storage.local.get([CAPTURE_HISTORY_KEY]))[CAPTURE_HISTORY_KEY];
  return {
    ok: true, organization: org, channels,
    catalog: [...catalog.values()].map((item) => ({ id: item.id, name: item.name, hosts: item.hosts || [] })),
    history: Array.isArray(history) ? history : [],
  };
}

function captureFromDashboard(sender) {
  try {
    const url = new URL(String(sender?.url || ""));
    return Boolean(sender?.tab) && sender.id === chrome.runtime.id
      && FORKMESH_ORIGINS.includes(url.origin) && url.pathname.startsWith("/dashboard/marketing");
  } catch {
    return false;
  }
}

function captureFromPage(sender) {
  return sender?.id === chrome.runtime.id
    && String(sender?.url || "").startsWith(chrome.runtime.getURL(CAPTURE_PAGE));
}

const REFRESH_SETUP_KEY = "refreshSetup";
const REFRESH_READ_KEY = "refreshRead";
const REFRESH_SETUP_MS = 5 * 60 * 1000;
const REFRESH_RETRY_MS = 60 * 1000;
const REFRESH_FRESH_MS = 12 * 60 * 60 * 1000;
const REFRESH_NEXT_MS = 60 * 60 * 1000;
const REFRESH_UNREAD_COLOR = "#2f81f7";
const REFRESH_READ_COLOR = "#1a7f37";
const REFRESH_X_HOST = /^(?:x|twitter)\.com$/;
const REFRESH_REDDIT_HOST = /^(?:www|old)\.reddit\.com$/;
const REFRESH_REDDIT_LISTINGS = new Set(["new", "hot", "top", "rising", "controversial", "best"]);
const REFRESH_X_PROFILE_TABS = new Set(["", "with_replies", "following", "followers", "verified_followers"]);
const REFRESH_ERRORS = {
  not_signed_in: "Sign in to ForkMesh in the extension first.",
  marketing_not_installed: "Install the Marketing package first.",
  forkmesh_unreachable: "Could not reach the ForkMesh server.",
  x_profile_not_read: "Nothing has been read on X yet: let the page load, then try again.",
  channel_url_required: "Save the channel's link in Marketing first.",
  channel_refresh_unsupported: "Marketing cannot refresh this channel from its server.",
  channel_not_configured: "Add that channel in Marketing first.",
  capture_found_no_numbers: "No follower, post, view or review counts were found on this page.",
  capture_sign_in_needed: "The site asked you to sign in. Sign in there, then try again.",
  reddit_profile_not_read: "This Reddit page could not be read: reload it, then try again.",
};

const refreshError = (code, message = "") => REFRESH_ERRORS[code] || String(message || code || "request failed").replaceAll("_", " ");
const refreshFresh = (at) => Date.now() - Number(at || 0) < REFRESH_FRESH_MS;
const refreshBareHost = (host) => String(host || "").toLowerCase().replace(/^(?:www|old|new|m|mobile)\./, "");

function refreshUrl(value) {
  try {
    const url = new URL(String(value || ""));
    return url.protocol === "https:" ? url : null;
  } catch {
    return null;
  }
}

function refreshSamePage(url, saved) {
  const other = refreshUrl(saved);
  if (!url || !other) return false;
  const path = (item) => item.pathname.toLowerCase().replace(/\/+$/, "");
  return refreshBareHost(url.hostname) === refreshBareHost(other.hostname)
    && (path(url) === path(other) || path(url).startsWith(`${path(other)}/`));
}

async function refreshGet(key) {
  try {
    return (await chrome.storage.session.get([key]))[key] || null;
  } catch {
    return null;
  }
}

async function refreshMark(keys, tabId, fresh = false) {
  if (!keys.length || !chrome.storage.session) return;
  try {
    const read = await refreshGet(REFRESH_READ_KEY) || {};
    const now = Date.now();
    const changed = fresh || keys.some((key) => !refreshFresh(read[key]));
    for (const key of keys) read[key] = now;
    await chrome.storage.session.set({ [REFRESH_READ_KEY]: read });
    if (!changed) return;
    if (Number.isInteger(tabId)) void refreshBadge(tabId, false);
    chrome.runtime.sendMessage({ type: "fm-refresh-changed" }).catch(() => null);
  } catch {
  }
}

const ACTIVITY_GAP_MS = 5000;
const activityLast = new Map();
const activityCount = (n, one, many) => `${n} ${n === 1 ? one : many}`;

async function activityReport(entry, key = "") {
  try {
    if (!(await settings()).sessionToken) return;
    const now = Date.now();
    if (key && now - (activityLast.get(key) || 0) < ACTIVITY_GAP_MS) return;
    if (key) activityLast.set(key, now);
    const setup = await refreshSetup(true);
    if (!setup?.organization) return;
    await api(`/api/marketing/${encodeURIComponent(setup.organization)}/activity`, "POST", entry);
  } catch {
  }
}

async function refreshSetup(network) {
  const current = await settings();
  const cached = await refreshGet(REFRESH_SETUP_KEY);
  const same = cached && cached.account === current.account && cached.preferred === current.organization;
  const age = Date.now() - Number(cached?.at || 0);
  if (same && (!network || age < (cached.organization ? REFRESH_SETUP_MS : REFRESH_RETRY_MS))) return cached;
  if (!network) return null;
  const setup = { at: Date.now(), account: current.account, preferred: current.organization, organization: "", channels: [] };
  try {
    setup.organization = await xOrganization();
    setup.channels = (await captureChannels(setup.organization)).channels
      .filter((item) => item.enabled && item.url)
      .map(({ id, name, url, refreshable, capturedAt, refreshedAt }) => ({ id, name, url, refreshable, capturedAt, refreshedAt }));
  } catch {
    setup.organization = "";
  }
  if (chrome.storage.session) await chrome.storage.session.set({ [REFRESH_SETUP_KEY]: setup });
  return setup;
}

function refreshChannelPages(url, channels) {
  return channels.filter((channel) => refreshBareHost(refreshUrl(channel.url)?.hostname) === refreshBareHost(url.hostname))
    .map((channel) => ({
      key: `channel:${channel.id}`, label: channel.name, url: channel.url, channel: channel.id,
      current: refreshSamePage(url, channel.url), anywhere: Boolean(channel.refreshable),
      readAt: Math.max(Number(channel.capturedAt || 0), Number(channel.refreshedAt || 0)),
      steps: channel.refreshable ? [{ refresh: channel.id }] : [],
    }));
}

function refreshXPage(url, handle) {
  if (!url || !REFRESH_X_HOST.test(url.hostname)) return "";
  const path = url.pathname.toLowerCase().replace(/\/+$/, "");
  if (path === "/notifications/mentions") return "mentions";
  if (path === "/search") return String(url.searchParams.get("q") || "").toLowerCase().includes(`@${handle}`) ? "search" : "";
  const parts = path.split("/").filter(Boolean);
  if (parts[0] !== handle) return "";
  if (REFRESH_X_PROFILE_TABS.has(parts[1] || "")) return "profile";
  return parts[1] === "media" ? "media" : "";
}

async function refreshXSite(url, setup) {
  const { handle } = await xState();
  const kind = refreshXPage(url, handle);
  const saved = setup.channels.find((item) => item.id === "x");
  const sync = { x: true };
  const pages = [
    { key: "x:profile", label: `@${handle}`, url: `https://x.com/${handle}`, channel: saved ? "x" : "",
      readAt: saved ? Number(saved.capturedAt || 0) : 0, steps: [sync] },
    { key: "x:media", label: "Media", url: `https://x.com/${handle}/media`, steps: [sync] },
    { key: "x:mentions", label: "Mentions", url: "https://x.com/notifications/mentions", steps: [sync] },
    { key: "x:search", label: `Search @${handle}`, url: `https://x.com/search?q=${encodeURIComponent(`@${handle}`)}&f=live`, steps: [sync] },
  ].map((page) => ({ ...page, current: page.key === `x:${kind}` }));
  return { site: "x", title: `X · @${handle}`, pages };
}

function refreshRedditWhere(url) {
  const parts = url.pathname.split("/").filter(Boolean).map((part) => {
    try { return decodeURIComponent(part); } catch { return part; }
  });
  const query = redditQuery(url.searchParams.get("q"));
  const first = String(parts[0] || "").toLowerCase();
  if (first === "search") return { kind: "search", query };
  if ((first === "user" || first === "u") && REDDIT_AUTHOR.test(parts[1] || "")) return { kind: "profile", account: parts[1] };
  if (first !== "r" || !REDDIT_SUBREDDIT.test(parts[1] || "")) return { kind: "" };
  const subreddit = parts[1].toLowerCase();
  const section = String(parts[2] || "").toLowerCase();
  if (section === "search") return { kind: "search", query, subreddit };
  if (section === "comments") return { kind: "thread", subreddit, postId: String(parts[3] || "").toLowerCase() };
  if (section === "about" && String(parts[3] || "").toLowerCase() === "rules") return { kind: "rules", subreddit };
  return { kind: !section || REFRESH_REDDIT_LISTINGS.has(section) ? "listing" : "", subreddit };
}

async function refreshRedditSite(url, network) {
  const stats = network ? await redditStatsNow() : await redditStats();
  const state = await redditState();
  const where = refreshRedditWhere(url);
  const sync = { reddit: true };
  const pages = [];
  for (const name of state.watch.subreddits) {
    const lower = name.toLowerCase();
    pages.push({ key: `reddit:sub:${lower}`, label: `r/${name}`, url: `https://www.reddit.com/r/${name}/new/`,
      current: where.kind === "listing" && where.subreddit === lower, steps: [sync] });
  }
  for (const query of state.watch.queries) {
    pages.push({ key: `reddit:search:${query}`, label: `Search “${query}”`,
      url: `https://www.reddit.com/search/?q=${encodeURIComponent(query)}&sort=new`,
      current: where.kind === "search" && where.query === query, steps: [sync] });
  }
  for (const name of state.watch.subreddits) {
    const lower = name.toLowerCase();
    if (state.watch.rulesOnFile[lower]) continue;
    pages.push({ key: `reddit:rules:${lower}`, label: `r/${name} rules`, url: `https://old.reddit.com/r/${name}/about/rules`,
      current: where.kind === "rules" && where.subreddit === lower, done: Boolean(state.rules[lower]), steps: [sync] });
  }
  for (const name of state.watch.accounts) {
    const lower = name.toLowerCase();
    pages.push({ key: `reddit:user:${lower}`, label: `u/${name}`, url: `https://www.reddit.com/user/${name}/submitted/`,
      current: where.kind === "profile" && where.account.toLowerCase() === lower, steps: [{ redditProfile: name }] });
  }
  if (where.kind === "profile" && !redditAccount(state, where.account)) {
    pages.push({ key: `reddit:user:${where.account.toLowerCase()}`, label: `u/${where.account}`, url: url.href,
      current: true, steps: [{ redditProfile: where.account }] });
  }
  for (const entry of state.watch.monitored) {
    const reading = state.readings[entry.postId];
    const title = entry.title || reading?.title || `r/${entry.subreddit} ${entry.postId}`;
    pages.push({ key: `reddit:thread:${entry.postId}`, label: `Thread: ${title.length > 40 ? `${title.slice(0, 39)}…` : title}`,
      url: entry.commentUrl || entry.url, current: where.kind === "thread" && where.postId === entry.postId,
      readAt: Math.max(Number(entry.checkedAt || 0), Number(reading?.at || 0)), steps: [sync] });
  }
  if (where.kind === "thread" && REDDIT_POST_ID.test(where.postId) && redditWatchedSubreddit(state, where.subreddit)
    && !pages.some((page) => page.current)) {
    pages.push({ key: `reddit:thread:${where.postId}`, label: "This thread", url: url.href, current: true, steps: [sync] });
  }
  const waiting = Number(stats?.waiting || 0) + Number(stats?.rulesWaiting || 0) + Number(stats?.readingsWaiting || 0);
  return { site: "reddit", title: "Reddit desk", pages, waiting };
}

async function refreshSite(url, network = true) {
  const current = await settings();
  if (!url || !current.sessionToken) return null;
  const setup = await refreshSetup(network);
  if (!setup) return undefined;
  if (!setup.organization) return null;
  const host = url.hostname.toLowerCase();
  const site = REFRESH_X_HOST.test(host) ? await refreshXSite(url, setup)
    : REFRESH_REDDIT_HOST.test(host) ? await refreshRedditSite(url, network)
      : { site: refreshBareHost(host), title: "", pages: refreshChannelPages(url, setup.channels) };
  if (!site.pages.length) return null;
  if (!site.title) site.title = site.pages.map((page) => page.label).join(", ");
  const read = await refreshGet(REFRESH_READ_KEY) || {};
  for (const page of site.pages) {
    page.done = Boolean(page.done || refreshFresh(read[page.key]) || refreshFresh(page.readAt)
      || (page.channel && refreshFresh(read[`channel:${page.channel}`])));
  }
  return { ...site, organization: setup.organization };
}

function refreshSteps(site) {
  const steps = [];
  for (const page of site.pages) {
    if (!page.current && !page.anywhere) continue;
    for (const step of page.steps) {
      if (!steps.some((item) => JSON.stringify(item) === JSON.stringify(step))) steps.push(step);
    }
  }
  return steps;
}

function refreshAction(site, steps) {
  const names = steps.filter((step) => step.refresh)
    .map((step) => site.pages.find((page) => page.channel === step.refresh)?.label || step.refresh);
  const labels = [];
  if (names.length) labels.push(`Refresh ${names.join(", ")}`);
  if (steps.some((step) => step.x)) labels.push("Sync X stats");
  for (const step of steps.filter((item) => item.redditProfile)) labels.push(`Update u/${step.redditProfile} posts and views`);
  if (steps.some((step) => step.reddit)) labels.push(`Send to Reddit desk${site.waiting ? ` (${site.waiting})` : ""}`);
  return labels.join(" · ");
}

async function refreshNext(url) {
  if (!(await settings()).sessionToken) return null;
  const setup = await refreshSetup(true);
  if (!setup?.organization) return null;
  const read = await refreshGet(REFRESH_READ_KEY) || {};
  const now = Date.now();
  const due = setup.channels.map((channel) => ({ channel,
    at: Math.max(Number(channel.capturedAt || 0), Number(channel.refreshedAt || 0), Number(read[`channel:${channel.id}`] || 0)) }))
    .filter((item) => now - item.at >= REFRESH_NEXT_MS)
    .sort((one, two) => one.at - two.at);
  if (!due.length) return null;
  const { channel, at } = due[0];
  return { channel: channel.id, label: channel.name, url: channel.url, at,
    current: refreshSamePage(url, channel.url), organization: setup.organization };
}

function refreshPublic(site, due = null) {
  const suggest = due && { channel: due.channel, label: due.label, url: due.url, at: due.at, current: due.current,
    covered: Boolean(site && refreshSteps(site).some((step) => step.refresh === due.channel)) };
  if (!site) {
    return suggest ? { ok: true, show: true, site: "", title: "", organization: due.organization, here: false, onPage: false,
      action: "", hint: "", next: null, read: 0, total: 0, unread: 0, refreshes: [], pages: [], suggest } : { ok: true, show: false };
  }
  const steps = refreshSteps(site);
  const here = site.pages.some((page) => page.current);
  const next = site.pages.find((page) => !page.current && !page.done) || null;
  const manual = site.pages.filter((page) => !page.steps.length).map((page) => page.label);
  const done = site.pages.filter((page) => page.done).length;
  let hint = "";
  if (manual.length && !steps.length) hint = `Marketing cannot refresh ${manual.join(", ")} from its server: Update reads it from its page.`;
  else if (!steps.length) hint = next ? "ForkMesh does not read this page." : "Every page was read in the last 12 hours.";
  return {
    ok: true, show: true, site: site.site, title: site.title, organization: site.organization,
    here: Boolean(steps.length), onPage: here, action: refreshAction(site, steps), hint,
    next: next ? { label: next.label, url: next.url } : null,
    read: done, total: site.pages.length, unread: site.pages.length - done,
    refreshes: steps.filter((step) => step.refresh).map((step) => step.refresh),
    pages: site.pages.map((page) => ({ key: page.key, label: page.label, url: page.url, current: Boolean(page.current), done: page.done })),
    suggest,
  };
}

async function refreshPlan({ url }) {
  const target = refreshUrl(url);
  const site = await refreshSite(target);
  return refreshPublic(site, site === undefined ? null : await refreshNext(target));
}

function refreshRedditText(sent, where) {
  const said = [
    sent.threads && activityCount(sent.threads, "new thread", "new threads"),
    sent.already && `${sent.already} already on the desk`,
    sent.rules && activityCount(sent.rules, "rule set", "rule sets"),
    sent.readings && activityCount(sent.readings, "view reading", "view readings"),
    sent.failed && `${sent.failed} refused (see the popup)`,
  ].filter(Boolean);
  if (said.length) return `Sent to the Reddit desk: ${said.join(", ")}.`;
  if (where?.kind === "listing" && Number(sent.unmatched || 0)) {
    return "Nothing new for the Reddit desk: the posts read here are not threads you opened and none has every word of a watched search in its title. Open a thread to log it.";
  }
  if (Number(sent.sentBefore || 0)) return `Nothing new for the Reddit desk: ${activityCount(sent.sentBefore, "thread", "threads")} read so far ${sent.sentBefore === 1 ? "is" : "are"} already on it.`;
  return "Nothing new for the Reddit desk: let the page load, then try again.";
}

async function refreshStep(step, site, url, tabId) {
  try {
    if (step.redditProfile) {
      if (!Number.isInteger(tabId)) return { error: "Open the profile in a tab, then try again." };
      return { text: (await redditProfileUpdate({ tabId })).text };
    }
    if (step.refresh) {
      const page = site.pages.find((item) => item.channel === step.refresh);
      const job = {
        id: crypto.randomUUID(), organization: site.organization, channel: step.refresh, name: page?.label || step.refresh,
        url: page?.url || "", sourceTabId: null, startedAt: Date.now(), openedTab: false, closeWhenDone: false, state: "reading", panel: true,
      };
      await captureSave(job);
      await captureRefresh(job);
      if (job.state === "error") return { error: `${job.name}: ${refreshError(job.error, job.message)}` };
      return { text: `${job.name} refreshed in ${job.organization} Marketing.` };
    }
    if (step.x) {
      const stats = await xSyncNow();
      if (stats.lastSync?.error) return { error: `X: ${refreshError(stats.lastSync.error)}` };
      await refreshMark(["channel:x"]);
      return { text: `X stats recorded in ${stats.lastSync?.organization || site.organization} Marketing.` };
    }
    const stats = await redditSyncNow();
    const sent = stats.lastSync || {};
    if (sent.error) return { error: `Reddit desk: ${refreshError(sent.error)}` };
    return { text: refreshRedditText(sent, refreshUrl(url) && refreshRedditWhere(refreshUrl(url))) };
  } catch (error) {
    return { error: refreshError(error?.code, error?.message) };
  }
}

async function refreshRun({ url, tabId }) {
  const site = await refreshSite(refreshUrl(url));
  const steps = site ? refreshSteps(site) : [];
  if (!steps.length) throw new ApiError("refresh_page_not_read", 400);
  const said = [];
  for (const step of steps) said.push(await refreshStep(step, site, url, tabId));
  if (Number.isInteger(tabId)) void refreshBadge(tabId);
  const failed = said.some((item) => item.error);
  const message = said.map((item) => item.error || item.text).join(" ");
  void activityReport({ kind: failed ? "failed" : "sent", site: site.site, page: site.title, url: String(url || ""), text: message });
  return { ok: true, failed, message, plan: await refreshPlan({ url }) };
}

async function refreshChannel({ channel, url, tabId }) {
  if (!(await settings()).sessionToken) throw new ApiError("not_signed_in", 401);
  const setup = await refreshSetup(true);
  if (!setup?.organization) throw new ApiError("marketing_not_installed", 404);
  const target = setup.channels.find((item) => item.id === String(channel || ""));
  if (!target) throw new ApiError("channel_not_configured", 404);
  const onPage = refreshSamePage(refreshUrl(url), target.url);
  if (!target.refreshable && !onPage) throw new ApiError("refresh_page_not_read", 400);
  let said;
  if (target.refreshable) {
    said = await refreshStep({ refresh: target.id }, { organization: setup.organization, pages: [{ channel: target.id, label: target.name, url: target.url }] }, url);
  } else {
    const { job } = await captureTab({ organization: setup.organization, channel: target.id, tabId });
    said = job.state === "done" ? { text: `${job.name} recorded in ${job.organization} Marketing.` }
      : { error: `${job.name}: ${refreshError(job.error, job.message)}` };
  }
  if (Number.isInteger(tabId)) void refreshBadge(tabId);
  const message = said.error || said.text;
  if (target.refreshable) {
    void activityReport({ kind: said.error ? "failed" : "sent", site: refreshBareHost(refreshUrl(target.url)?.hostname), page: target.name, url: target.url, text: message });
  }
  return { ok: true, failed: Boolean(said.error), message, plan: await refreshPlan({ url }) };
}

async function refreshBadge(tabId, network = true) {
  try {
    const tab = await chrome.tabs.get(tabId);
    const site = await refreshSite(refreshUrl(tab?.url), network);
    if (site === undefined) return;
    const unread = site ? site.pages.filter((page) => !page.done).length : 0;
    await chrome.action.setBadgeText({ tabId, text: site ? (unread ? String(unread) : "✓") : "" });
    if (site) await chrome.action.setBadgeBackgroundColor({ tabId, color: unread ? REFRESH_UNREAD_COLOR : REFRESH_READ_COLOR });
  } catch {
  }
}

chrome.tabs.onActivated?.addListener(({ tabId }) => void refreshBadge(tabId));
chrome.tabs.onUpdated?.addListener((tabId, change) => {
  if (change.url || change.status === "complete") void refreshBadge(tabId);
});

const X_SETUP_KEY = "xSetup";
const X_SETUP_TTL = 2 * 60 * 60 * 1000;
const X_SETUP_PORTAL = "https://console.x.com/";
const X_SETUP_ORIGINS = ["https://console.x.com", "https://developer.x.com", "https://developer.twitter.com"];
const X_SETUP_FIELDS = {
  apiKey: /^[A-Za-z0-9]{15,64}$/,
  apiSecret: /^[A-Za-z0-9]{30,80}$/,
  accessToken: /^\d{1,20}-[A-Za-z0-9]{10,64}$/,
  accessSecret: /^[A-Za-z0-9]{30,80}$/,
};
let xSetupQueue = Promise.resolve();
const xSetupSerial = (work) => (xSetupQueue = xSetupQueue.then(work, work));

async function xSetupLoad() {
  const stored = (await chrome.storage.session.get([X_SETUP_KEY]))[X_SETUP_KEY];
  if (!stored?.id) return null;
  if (Date.now() - Number(stored.startedAt || 0) > X_SETUP_TTL) {
    await chrome.storage.session.remove(X_SETUP_KEY);
    return null;
  }
  return stored;
}

function xSetupPublic(setup) {
  if (!setup) return { active: false };
  const keys = Object.fromEntries(Object.keys(X_SETUP_FIELDS).map((name) => {
    const value = String(setup.keys?.[name] || "");
    return [name, value ? `…${value.slice(-4)}` : ""];
  }));
  return {
    active: true, id: setup.id, organization: setup.organization, state: setup.state,
    permission: Boolean(setup.permission), keys, error: setup.error || "", message: setup.message || "",
    detail: setup.detail || "", username: setup.username || "", startedAt: setup.startedAt,
    finishedAt: setup.finishedAt || 0, marketingUrl: setup.marketingUrl || "",
  };
}

async function xSetupSave(setup) {
  await chrome.storage.session.set({ [X_SETUP_KEY]: setup });
  const update = xSetupPublic(setup);
  for (const tabId of [setup.sourceTabId, ...(setup.portalTabs || [])]) {
    if (!Number.isInteger(tabId)) continue;
    const type = tabId === setup.sourceTabId ? "fm-x-setup-update" : "fm-x-setup-changed";
    await chrome.tabs.sendMessage(tabId, { type, setup: update }).catch(() => null);
  }
  return update;
}

function xSetupFromPortal(sender) {
  try {
    return Boolean(sender?.tab) && sender.id === chrome.runtime.id
      && X_SETUP_ORIGINS.includes(new URL(String(sender.url || "")).origin);
  } catch {
    return false;
  }
}

function xSetupRemember(setup, sender) {
  const tabId = sender?.tab?.id;
  if (!Number.isInteger(tabId) || !xSetupFromPortal(sender)) return;
  setup.portalTabs = [tabId, ...(setup.portalTabs || []).filter((item) => item !== tabId)].slice(0, 4);
}

const xSetupStart = ({ organization }, sender) => xSetupSerial(async () => {
  const current = await settings();
  if (!current.sessionToken) throw new ApiError("not_signed_in", 401);
  const org = await captureOrganization(organization);
  const payload = await api(`/api/marketing/${encodeURIComponent(org)}/settings`);
  if (!payload.canManageSettings) throw new ApiError("organization_admin_required", 403);
  const previous = await xSetupLoad();
  const keep = previous?.organization === org && previous.state !== "done";
  const setup = {
    id: crypto.randomUUID(), organization: org, startedAt: Date.now(), state: "guiding",
    permission: keep ? Boolean(previous.permission) : false, keys: keep ? { ...previous.keys } : {},
    sourceTabId: Number.isInteger(sender?.tab?.id) ? sender.tab.id : null,
    marketingUrl: `${current.baseUrl}/dashboard/marketing?org=${encodeURIComponent(org)}`,
    portalTabs: [],
  };
  const tab = await chrome.tabs.create({ url: X_SETUP_PORTAL, active: true });
  setup.portalTabs = [tab.id];
  return { ok: true, setup: await xSetupSave(setup) };
});

const xSetupState = (_request, sender) => xSetupSerial(async () => {
  const setup = await xSetupLoad();
  if (setup && xSetupFromPortal(sender) && !(setup.portalTabs || []).includes(sender.tab.id)) {
    xSetupRemember(setup, sender);
    await chrome.storage.session.set({ [X_SETUP_KEY]: setup });
  }
  return { ok: true, setup: xSetupPublic(setup) };
});

async function xSetupSubmit(setup) {
  setup.state = "checking";
  Object.assign(setup, { error: "", message: "", detail: "" });
  await xSetupSave(setup);
  try {
    const result = await api(`/api/marketing/${encodeURIComponent(setup.organization)}/x-connection`, "POST", { ...setup.keys });
    Object.assign(setup, { state: "done", keys: {}, username: String(result.x?.username || ""), finishedAt: Date.now() });
  } catch (error) {
    Object.assign(setup, {
      state: "error", error: error?.code || "request_failed", finishedAt: Date.now(),
      message: String(error?.message || "request failed").slice(0, 200),
      detail: String(error?.payload?.detail || "").slice(0, 300),
    });
    if (error?.code === "x_not_authorized") setup.keys = { apiKey: setup.keys.apiKey, apiSecret: setup.keys.apiSecret };
  }
  return xSetupSave(setup);
}

const xSetupComplete = (setup) => Object.keys(X_SETUP_FIELDS).every((name) => setup.keys?.[name]);

const xSetupKeys = ({ keys, submit }, sender) => xSetupSerial(async () => {
  const setup = await xSetupLoad();
  if (!setup) throw new ApiError("x_setup_expired", 404);
  xSetupRemember(setup, sender);
  let changed = false;
  for (const [name, pattern] of Object.entries(X_SETUP_FIELDS)) {
    const value = String(keys?.[name] || "").trim();
    if (!value || !pattern.test(value) || setup.keys?.[name] === value) continue;
    setup.keys = { ...setup.keys, [name]: value };
    changed = true;
  }
  if (changed && setup.state !== "checking") Object.assign(setup, { state: "guiding", error: "", message: "", detail: "" });
  if (xSetupComplete(setup) && setup.state !== "checking" && ((changed && setup.permission) || submit)) {
    return { ok: true, setup: await xSetupSubmit(setup) };
  }
  return { ok: true, setup: await xSetupSave(setup) };
});

const xSetupMark = ({ permission }, sender) => xSetupSerial(async () => {
  const setup = await xSetupLoad();
  if (!setup) throw new ApiError("x_setup_expired", 404);
  xSetupRemember(setup, sender);
  if (permission && !setup.permission) {
    setup.keys = { apiKey: setup.keys?.apiKey, apiSecret: setup.keys?.apiSecret };
  }
  setup.permission = Boolean(permission);
  return { ok: true, setup: await xSetupSave(setup) };
});

const xSetupForget = ({ field }, sender) => xSetupSerial(async () => {
  const setup = await xSetupLoad();
  if (!setup) throw new ApiError("x_setup_expired", 404);
  xSetupRemember(setup, sender);
  const names = field === "access" ? ["accessToken", "accessSecret"] : field === "api" ? ["apiKey", "apiSecret"] : [];
  setup.keys = Object.fromEntries(Object.entries(setup.keys || {}).filter(([name]) => !names.includes(name)));
  if (setup.state === "error") Object.assign(setup, { state: "guiding", error: "", message: "", detail: "" });
  return { ok: true, setup: await xSetupSave(setup) };
});

const xSetupStop = () => xSetupSerial(async () => {
  const setup = await xSetupLoad();
  await chrome.storage.session.remove(X_SETUP_KEY);
  if (setup) {
    for (const tabId of setup.portalTabs || []) {
      await chrome.tabs.sendMessage(tabId, { type: "fm-x-setup-changed", setup: { active: false } }).catch(() => null);
    }
    if (Number.isInteger(setup.sourceTabId)) {
      await chrome.tabs.sendMessage(setup.sourceTabId, { type: "fm-x-setup-update", setup: { ...xSetupPublic(setup), state: "stopped" } }).catch(() => null);
    }
  }
  return { ok: true, setup: { active: false } };
});

async function xSetupBack() {
  const setup = await xSetupLoad();
  if (!setup) throw new ApiError("x_setup_expired", 404);
  const source = Number.isInteger(setup.sourceTabId) ? await chrome.tabs.get(setup.sourceTabId).catch(() => null) : null;
  if (source) {
    await chrome.tabs.update(source.id, { active: true });
    await chrome.windows.update(source.windowId, { focused: true }).catch(() => null);
  } else {
    await chrome.tabs.create({ url: setup.marketingUrl || `${(await settings()).baseUrl}/dashboard/marketing` });
  }
  if (setup.state === "done") await chrome.storage.session.remove(X_SETUP_KEY);
  return { ok: true };
}

const X_SETUP_PORTAL_ONLY = new Set(["fm-x-setup-keys", "fm-x-setup-mark", "fm-x-setup-forget", "fm-x-setup-back"]);

const REDDIT_WALK_SENDERS = new Set(["fm-reddit-walk", "fm-reddit-walk-state", "fm-reddit-walk-stop", "fm-reddit-accounts-update"]);

const POPUP_ONLY = new Set(["fm-reddit-stats", "fm-reddit-refresh", "fm-reddit-sync",
  "fm-reddit-clear-errors", "fm-reddit-monitor-add", "fm-reddit-monitor-remove", "fm-reddit-check", "fm-reddit-profile-update", "fm-reddit-auto-check", "fm-open-reddit-desk", "fm-marketing-capture-tab", "fm-marketing-setup", "fm-x-live-stop",
  "fm-refresh-plan", "fm-refresh-run", "fm-refresh-channel"]);
POPUP_ONLY.add("fm-toot-set-track");
const CAPTURE_PAGE_ONLY = new Set(["fm-marketing-capture-job", "fm-marketing-capture-continue"]);


async function openCrm({ organization }) {
  const current = await settings();
  const org = String(organization || current.organization || "");
  const url = `${current.baseUrl}/dashboard/crm${org ? `?org=${encodeURIComponent(org)}` : ""}`;
  await chrome.tabs.create({ url });
  return { ok: true, url };
}

// The CRM dashboard's "Extensions page" button. A web page cannot open
// chrome:// addresses; the extension can, which is where an unpacked copy is
// reloaded after unzipping a newer version over it.
async function openExtensions() {
  await chrome.tabs.create({ url: "chrome://extensions" });
  return { ok: true };
}

async function grantHost({ baseUrl }) {
  const origin = normalizeBaseUrl(baseUrl);
  if (!origin) throw new ApiError("invalid_server_url", 400);
  return { ok: true, origin, pattern: `${origin}/*` };
}

const HANDLERS = {
  "fm-status": status,
  "fm-organizations": organizations,
  "fm-sign-in": signIn,
  "fm-sign-out": signOut,
  "fm-set-organization": async ({ organization }) => {
    await chrome.storage.local.set({ organization: String(organization || "") });
    return status();
  },
  "fm-lookup": lookup,
  "fm-contacts": contacts,
  "fm-contact-update": contactPatch,
  "fm-note-link": noteLink,
  "fm-link": link,
  "fm-unlink": unlink,
  "fm-track-sent": trackSent,
  "fm-sent-settings": sentSettings,
  "fm-sent-set-track": setTrackSent,
  "fm-open-crm": openCrm,
  "fm-mastodon-hosts": syncMastodon,
  "fm-mastodon-add": addMastodon,
  "fm-mastodon-remove": removeMastodon,
  "fm-track-toot": trackToot,
  "fm-toot-settings": tootSettings,
  "fm-toot-set-track": setTrackToots,
  "fm-open-extensions": openExtensions,
  "fm-host-pattern": grantHost,
  "fm-x-handle": async () => ({ ok: true, handle: (await xState()).handle }),
  "fm-x-observe": xObserve,
  "fm-x-error": xError,
  "fm-x-stats": xStats,
  "fm-x-sync": xSyncNow,
  "fm-x-set-handle": xSetHandle,
  "fm-x-clear-errors": xClearErrors,
  "fm-x-live-stop": xLiveStop,
  "fm-open-x": openX,
  "fm-reddit-watch": redditWatch,
  "fm-reddit-observe": redditObserve,
  "fm-reddit-error": redditError,
  "fm-reddit-stats": redditStatsNow,
  "fm-reddit-refresh": redditRefresh,
  "fm-reddit-sync": redditSyncNow,
  "fm-reddit-clear-errors": redditClearErrors,
  "fm-reddit-monitor-add": redditMonitorAdd,
  "fm-reddit-monitor-remove": redditMonitorRemove,
  "fm-reddit-check": redditCheck,
  "fm-reddit-profile-update": redditProfileUpdate,
  "fm-reddit-accounts-update": redditAccountsUpdate,
  "fm-reddit-auto-check": redditAutoCheck,
  "fm-reddit-walk": redditWalkStart,
  "fm-reddit-walk-state": redditWalkStatus,
  "fm-reddit-walk-stop": redditWalkStop,
  "fm-open-reddit-desk": openRedditDesk,
  "fm-marketing-capture": captureStart,
  "fm-marketing-capture-job": captureJob,
  "fm-marketing-capture-continue": captureContinue,
  "fm-marketing-capture-tab": captureTab,
  "fm-marketing-setup": captureSetup,
  "fm-x-setup-start": xSetupStart,
  "fm-x-setup-state": xSetupState,
  "fm-x-setup-keys": xSetupKeys,
  "fm-x-setup-mark": xSetupMark,
  "fm-x-setup-forget": xSetupForget,
  "fm-x-setup-stop": xSetupStop,
  "fm-x-setup-back": xSetupBack,
  "fm-refresh-plan": refreshPlan,
  "fm-refresh-run": refreshRun,
  "fm-refresh-channel": refreshChannel,
};

const vaultBackground = self.ForkMeshVaultBackground.install({ api, settings, handlers: HANDLERS });

// The vault answers this extension's own pages only (the popup). A content
// script has a tab behind it; whatever happens inside Gmail, Zoho Mail, a
// Mastodon instance or X, nothing running there can ask for a login.
function mayUseVault(sender) {
  return sender?.id === chrome.runtime.id && !sender.tab
    && String(sender.url || "").startsWith(chrome.runtime.getURL(""));
}

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  const handler = HANDLERS[request?.type];
  if (!handler) return false;
  const forbidden = ((String(request.type).includes("vault") || POPUP_ONLY.has(request.type)) && !mayUseVault(sender))
    || (CAPTURE_PAGE_ONLY.has(request.type) && !captureFromPage(sender))
    || (request.type === "fm-marketing-capture" && !mayUseVault(sender) && !captureFromDashboard(sender))
    || (request.type === "fm-x-setup-start" && !mayUseVault(sender) && !captureFromDashboard(sender))
    || (REDDIT_WALK_SENDERS.has(request.type) && !mayUseVault(sender) && !captureFromDashboard(sender))
    || (["fm-x-setup-state", "fm-x-setup-stop"].includes(request.type) && !mayUseVault(sender)
      && !captureFromDashboard(sender) && !xSetupFromPortal(sender))
    || (X_SETUP_PORTAL_ONLY.has(request.type) && !xSetupFromPortal(sender));
  if (forbidden) {
    sendResponse({ ok: false, error: "forbidden_sender", message: "Forbidden", status: 403 });
    return false;
  }
  handler(request, sender)
    .then((result) => sendResponse(result))
    .catch((error) => sendResponse({
      ok: false, error: error?.code || "request_failed",
      message: error?.message || "Request failed", status: error?.status || 0,
    }));
  return true;
});

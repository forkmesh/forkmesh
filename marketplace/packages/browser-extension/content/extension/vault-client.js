// ForkMesh Vault client: everything between the package API and a screen.
//
// One vault per ForkMesh account. This file owns the unlocked state (the
// keyset, the item keys, the decrypted items) and turns user intents into
// ciphertext for /api/vault; it has no DOM and no fetch of its own, so the
// dashboard page and the browser extension share it byte for byte
// (tools/sync_crypto.py). The caller supplies `request(path, method, body)`.
//
// Trust is pinned on first use: the first time a person's public keys are
// seen their fingerprint is remembered in an encrypted record only this
// account can open. If the server later presents different keys for the same
// name, nothing is sealed to them and nothing from them is opened until the
// owner confirms the new fingerprint out of band.

(function (root) {
  const factory = forkMeshVaultClientFactory;
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.ForkMeshVaultClient = factory();

function forkMeshVaultClientFactory() {
  "use strict";

  const API = "/api/vault";
  const FIELD_LIMITS = Object.freeze({ title: 200, username: 320, password: 4096, url: 2048, notes: 8000 });

  class VaultError extends Error {
    constructor(code, detail = {}) {
      super(String(code).replaceAll("_", " "));
      this.name = "VaultError";
      this.code = String(code);
      this.detail = detail;
    }
  }

  function cleanFields(fields) {
    const out = {};
    for (const [name, limit] of Object.entries(FIELD_LIMITS)) {
      out[name] = String((fields || {})[name] ?? "").slice(0, limit);
    }
    out.title = out.title.trim();
    out.url = out.url.trim();
    if (!out.title) throw new VaultError("title_required");
    return out;
  }

  // The host a saved login belongs to. Only https pages (and localhost) can be
  // filled, so anything else has no host to match.
  function loginUrl(url) {
    try {
      const parsed = new URL(/^[a-z][a-z0-9+.-]*:\/\//i.test(url) ? url : "https://" + url);
      const local = ["localhost", "127.0.0.1", "[::1]"].includes(parsed.hostname);
      if (parsed.protocol !== "https:" && !(parsed.protocol === "http:" && local)) return null;
      return parsed;
    } catch {
      return null;
    }
  }
  function loginHost(url) {
    const parsed = loginUrl(String(url || ""));
    return parsed ? parsed.hostname.toLowerCase().replace(/^www\./, "") : "";
  }
  // The address the Open button visits: never a javascript: or data: URL.
  function loginHref(url) {
    const parsed = loginUrl(String(url || ""));
    return parsed ? parsed.href : "";
  }
  // Exact host only. "Ends with" would hand a login saved for one tenant of a
  // shared suffix (github.io, a co.uk) to every other tenant of it.
  function matchesPage(item, pageUrl) {
    const saved = loginHost((item.fields || {}).url || "");
    return Boolean(saved) && saved === loginHost(pageUrl);
  }

  const IMPORT_MAX_BYTES = 16 * 1024 * 1024;
  const IMPORT_UNTITLED = "Imported login";
  const IMPORT_SOURCES = Object.freeze({
    applePasswords: "Apple Passwords", duckDuckGo: "DuckDuckGo", chrome: "Chrome", firefox: "Firefox",
    bitwarden: "Bitwarden", onePassword: "1Password", other: "a password manager",
  });
  const IMPORT_COLUMNS = [
    ["title", ["title", "name", "account", "item name", "login name", "display name"]],
    ["url", ["url", "login_uri", "uri", "website", "web site", "web address", "site", "hostname", "login url", "urls"]],
    ["username", ["username", "login_username", "user name", "user", "login", "email", "email address", "user id"]],
    ["password", ["password", "login_password", "pass"]],
    ["notes", ["notes", "note", "comments", "comment", "extra"]],
    ["otpauth", ["otpauth", "login_totp", "totp", "otp", "one-time password", "otp auth", "otp url"]],
  ];

  function parseCsv(source) {
    const input = String(source || "").replace(/^﻿/, "");
    const rows = [];
    let row = [], field = "", quoted = false, fieldStart = true;
    const endField = () => { row.push(field); field = ""; fieldStart = true; };
    const endRow = () => {
      endField();
      if (!(row.length === 1 && row[0] === "")) rows.push(row);
      row = [];
    };
    for (let i = 0; i < input.length; i += 1) {
      const c = input[i];
      if (quoted) {
        if (c !== '"') field += c;
        else if (input[i + 1] === '"') { field += '"'; i += 1; }
        else quoted = false;
      } else if (c === '"' && fieldStart) { quoted = true; fieldStart = false; }
      else if (c === ",") endField();
      else if (c === "\r") { if (input[i + 1] === "\n") i += 1; endRow(); }
      else if (c === "\n") endRow();
      else { field += c; fieldStart = false; }
    }
    if (field || row.length || quoted) endRow();
    return rows;
  }

  function csvSource(header) {
    const names = new Set(header.map((name) => String(name).trim().toLowerCase()));
    const all = (...wanted) => wanted.every((name) => names.has(name));
    if (names.has("login_uri") || names.has("login_password")) return "bitwarden";
    if (names.has("httprealm") || names.has("formactionorigin") || names.has("guid")) return "firefox";
    if (names.has("archived") || names.has("one-time password")) return "onePassword";
    if (names.has("otpauth")) return "applePasswords";
    if (all("name", "url", "username", "password", "note")) return "chrome";
    if (all("title", "url", "username", "password", "notes")) return "duckDuckGo";
    return "other";
  }

  function readLoginsCsv(source) {
    const rows = parseCsv(source);
    if (!rows.length) throw new VaultError("csv_empty");
    const names = rows[0].map((name) => String(name).trim().toLowerCase());
    const index = {};
    for (const [field, aliases] of IMPORT_COLUMNS) {
      const alias = aliases.find((name) => names.includes(name));
      if (alias) index[field] = names.indexOf(alias);
    }
    if (index.password === undefined) throw new VaultError("csv_unknown_columns");
    const logins = [];
    let skippedRows = 0;
    for (const row of rows.slice(1)) {
      const value = (field) => (index[field] === undefined ? "" : String(row[index[field]] ?? ""));
      const login = {
        title: value("title").trim(), url: value("url").trim(), username: value("username"),
        password: value("password"), notes: value("notes"), otpauth: value("otpauth").trim(),
      };
      if (!login.password && !login.username.trim()) {
        if (row.some((cell) => cell.trim())) skippedRows += 1;
        continue;
      }
      logins.push(login);
    }
    if (!logins.length) throw new VaultError("csv_no_logins");
    const app = csvSource(rows[0]);
    return { source: app, sourceLabel: IMPORT_SOURCES[app], logins, skippedRows };
  }

  function u16(bytes, at) { return bytes[at] | (bytes[at + 1] << 8); }
  function u32(bytes, at) { return (u16(bytes, at) | (u16(bytes, at + 2) << 16)) >>> 0; }
  function isZip(bytes) { return bytes.length >= 4 && u32(bytes, 0) === 0x04034b50; }

  let crcTable = null;
  function crc32(bytes) {
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (let n = 0; n < 256; n += 1) {
        let c = n;
        for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crcTable[n] = c >>> 0;
      }
    }
    let crc = 0xffffffff;
    for (let i = 0; i < bytes.length; i += 1) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
  }

  async function inflateRaw(data) {
    const reader = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw")).getReader();
    const chunks = [];
    let total = 0;
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        total += value.length;
        if (total > IMPORT_MAX_BYTES) { reader.cancel().catch(() => {}); throw new VaultError("csv_too_large"); }
        chunks.push(value);
      }
    } catch (error) {
      throw error instanceof VaultError ? error : new VaultError("zip_unreadable");
    }
    const out = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) { out.set(chunk, offset); offset += chunk.length; }
    return out;
  }

  async function firstCsvInZip(bytes) {
    let end = -1;
    for (let at = bytes.length - 22; at >= Math.max(0, bytes.length - 22 - 0xffff); at -= 1) {
      if (u32(bytes, at) === 0x06054b50) { end = at; break; }
    }
    if (end < 0) throw new VaultError("zip_unreadable");
    let at = u32(bytes, end + 16);
    for (let entry = u16(bytes, end + 10); entry > 0; entry -= 1) {
      if (at + 46 > bytes.length || u32(bytes, at) !== 0x02014b50) throw new VaultError("zip_unreadable");
      const flags = u16(bytes, at + 8), method = u16(bytes, at + 10), crc = u32(bytes, at + 16);
      const packed = u32(bytes, at + 20), size = u32(bytes, at + 24);
      const nameLength = u16(bytes, at + 28), local = u32(bytes, at + 42);
      const name = new TextDecoder().decode(bytes.subarray(at + 46, at + 46 + nameLength));
      at += 46 + nameLength + u16(bytes, at + 30) + u16(bytes, at + 32);
      if (!/\.csv$/i.test(name) || name.startsWith("__MACOSX/") || name.split("/").pop().startsWith(".")) continue;
      if (flags & 1) throw new VaultError("zip_encrypted");
      if (size > IMPORT_MAX_BYTES) throw new VaultError("csv_too_large");
      if (local + 30 > bytes.length || u32(bytes, local) !== 0x04034b50) throw new VaultError("zip_unreadable");
      const start = local + 30 + u16(bytes, local + 26) + u16(bytes, local + 28);
      if (start + packed > bytes.length) throw new VaultError("zip_unreadable");
      const data = bytes.subarray(start, start + packed);
      if (method !== 0 && (method !== 8 || typeof DecompressionStream !== "function")) throw new VaultError("zip_unsupported");
      const out = method === 0 ? data : await inflateRaw(data);
      if (out.length !== size || crc32(out) !== crc) throw new VaultError("zip_unreadable");
      return out;
    }
    throw new VaultError("zip_no_csv");
  }

  async function readLoginsFile(data) {
    let bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    if (bytes.length > IMPORT_MAX_BYTES) throw new VaultError("csv_too_large");
    if (isZip(bytes)) bytes = await firstCsvInZip(bytes);
    let source;
    try { source = new TextDecoder("utf-8", { fatal: true }).decode(bytes); }
    catch { source = new TextDecoder("iso-8859-1").decode(bytes); }
    return readLoginsCsv(source);
  }

  function importFields(login) {
    const title = String(login.title || "").trim(), url = String(login.url || "").trim();
    const username = String(login.username || ""), code = String(login.otpauth || "").trim();
    let notes = String(login.notes || "").trim();
    if (code && !notes.includes(code)) notes = notes ? notes + "\n\n" + code : code;
    return {
      title: title || loginHost(url) || username.trim() || IMPORT_UNTITLED,
      url, username, password: String(login.password || ""), notes,
    };
  }

  function importIdentity(fields) {
    const place = loginHost(fields.url || "") || String(fields.title || "").trim().toLowerCase();
    return [place, String(fields.username || "").trim().toLowerCase(), String(fields.password || "")].join("\u0000");
  }

  function planImport(logins, items) {
    const seen = new Set((items || []).filter((item) => item.fields).map((item) => importIdentity(item.fields)));
    const fresh = [];
    let duplicates = 0;
    for (const login of logins || []) {
      const fields = importFields(login), key = importIdentity(fields);
      if (seen.has(key)) { duplicates += 1; continue; }
      seen.add(key);
      fresh.push(fields);
    }
    return { fresh, duplicates };
  }

  function create({ crypto: C, request, workerUrl = "" }) {
    let account = "";
    let server = null;          // the account record from GET /api/vault
    let keyset = null;
    let trust = { pins: {} };
    let items = [];
    const itemKeys = new Map();

    const unlocked = () => Boolean(keyset);
    function requireUnlocked() {
      if (!keyset) throw new VaultError("vault_locked");
    }
    function lock() {
      for (const key of itemKeys.values()) C.wipe(key);
      itemKeys.clear();
      C.forgetKeys();
      keyset = null; items = []; trust = { pins: {} };
    }

    async function status() {
      const payload = await request(API);
      account = String(payload.actor || "");
      server = payload.installed ? payload.account : null;
      return { installed: Boolean(payload.installed), account, unlocked: unlocked() };
    }

    async function openTrust() {
      trust = { pins: {} };
      if (!server || !server.trust) return;
      const stored = await C.openPrivate(keyset, "trust", server.trust);
      if (stored && typeof stored.pins === "object" && stored.pins) trust = { pins: { ...stored.pins } };
    }
    async function saveTrust() {
      const blob = await C.sealPrivate(keyset, "trust", trust);
      await request(API + "/trust", "PUT", { trust: blob });
      if (server) server.trust = blob;
    }
    // "new" on first sight, "pinned" when it matches, "changed" when it does not.
    async function trustState(bundle) {
      const name = String((bundle || {}).account || "");
      const fingerprint = await C.fingerprint(bundle);
      const pinned = trust.pins[name];
      return { name, fingerprint, state: !pinned ? "new" : pinned === fingerprint ? "pinned" : "changed" };
    }

    async function setUp(masterPassword) {
      if (!account) await status();
      if (server) throw new VaultError("vault_exists");
      const kdf = C.newKdf();
      const masterKey = await C.stretch(masterPassword, kdf, { workerUrl });
      const fresh = await C.newKeyset();
      const recoveryCode = C.newRecoveryCode();
      const recoveryKey = C.recoverySecret(recoveryCode);
      const payload = await request(API + "/install", "POST", {
        kdf,
        wrappedKeyset: await C.wrapKeyset(masterKey, "master", account, fresh),
        wrappedRecovery: await C.wrapKeyset(recoveryKey, "recovery", account, fresh),
        bundle: await C.publicBundle(fresh, account),
      });
      C.wipe(masterKey, recoveryKey);
      server = payload.account; keyset = fresh; trust = { pins: {} }; items = [];
      return { recoveryCode };
    }

    async function adopt(opened) {
      // The keyset must be the one whose public half the server publishes:
      // otherwise shares sealed to this account could never be opened.
      const mine = await C.publicBundle(opened, account);
      if (!server.bundle || mine.x25519 !== server.bundle.x25519
          || mine.mlkem768 !== server.bundle.mlkem768 || mine.ed25519 !== server.bundle.ed25519) {
        throw new VaultError("keyset_mismatch");
      }
      keyset = opened;
      await openTrust();
    }
    async function unlock(masterPassword) {
      await status();
      if (!server) throw new VaultError("vault_not_set_up");
      const masterKey = await C.stretch(masterPassword, server.kdf, { workerUrl });
      try {
        await adopt(await C.unwrapKeyset(masterKey, "master", account, server.wrappedKeyset));
      } finally {
        C.wipe(masterKey);
      }
    }
    // The extension's background worker restarts often; it re-enters an
    // unlocked vault from the keyset it kept in memory-only session storage.
    async function resume(savedKeyset) {
      await status();
      if (!server) throw new VaultError("vault_not_set_up");
      await adopt(C.checkKeyset(savedKeyset));
    }
    function exportKeyset() {
      requireUnlocked();
      return { ...keyset };
    }

    async function rewrap(masterPassword, withNewRecovery) {
      requireUnlocked();
      const body = { revision: server.revision };
      let recoveryCode = "";
      if (masterPassword !== null) {
        const kdf = C.newKdf();
        const masterKey = await C.stretch(masterPassword, kdf, { workerUrl });
        body.kdf = kdf;
        body.wrappedKeyset = await C.wrapKeyset(masterKey, "master", account, keyset);
        C.wipe(masterKey);
      }
      if (withNewRecovery) {
        recoveryCode = C.newRecoveryCode();
        const recoveryKey = C.recoverySecret(recoveryCode);
        body.wrappedRecovery = await C.wrapKeyset(recoveryKey, "recovery", account, keyset);
        C.wipe(recoveryKey);
      }
      server = (await request(API + "/keys", "PUT", body)).account;
      return { recoveryCode };
    }
    const changePassword = (masterPassword) => rewrap(masterPassword, false);
    const renewRecoveryCode = () => rewrap(null, true);
    // A used recovery code is spent: recovering sets a new master password
    // and a new code in one step.
    async function recover(recoveryCode, newMasterPassword) {
      await status();
      if (!server) throw new VaultError("vault_not_set_up");
      const blob = (await request(API + "/recovery")).wrappedRecovery;
      const recoveryKey = C.recoverySecret(recoveryCode);
      try {
        await adopt(await C.unwrapKeyset(recoveryKey, "recovery", account, blob));
      } catch (error) {
        throw new VaultError(error.code === "wrong_password" ? "wrong_recovery_code" : error.code || "recovery_failed");
      } finally {
        C.wipe(recoveryKey);
      }
      return rewrap(newMasterPassword, true);
    }

    // ---- reading ----------------------------------------------------------------

    async function load() {
      requireUnlocked();
      const payload = await request(API + "/items");
      const next = [];
      for (const row of payload.items || []) {
        const entry = { ...row, fields: null, error: "" };
        try {
          const itemKey = await C.unwrapItemKey(keyset, row.id, row.wrappedKey);
          itemKeys.set(row.id, itemKey);
          entry.fields = await C.openItem(itemKey, row.id, row.body);
        } catch (error) {
          entry.error = error.code || "decryption_failed";
        }
        next.push(entry);
      }
      let pinsChanged = false;
      for (const row of payload.shared || []) {
        const entry = { ...row, fields: null, error: "" };
        const bundle = (payload.bundles || {})[row.owner];
        try {
          if (!bundle) throw new VaultError("sender_keys_missing");
          const seen = await trustState(bundle);
          if (seen.state === "changed") throw new VaultError("sender_keys_changed");
          const itemKey = await C.openGrant({
            keyset, recipient: account, senderBundle: bundle, itemId: row.id,
            grant: row.grant, permission: row.permission,
          });
          // Pinned only after the share's signature held under these keys.
          if (seen.state === "new") { trust.pins[seen.name] = seen.fingerprint; pinsChanged = true; }
          itemKeys.set(row.id, itemKey);
          entry.fields = await C.openItem(itemKey, row.id, row.body);
        } catch (error) {
          entry.error = error.code || "decryption_failed";
        }
        next.push(entry);
      }
      if (pinsChanged) await saveTrust();
      items = next.sort((a, b) => String((a.fields || {}).title || "￿")
        .localeCompare(String((b.fields || {}).title || "￿"), undefined, { sensitivity: "base" }));
      return items;
    }
    const list = () => items;
    const find = (id) => items.find((item) => item.id === id) || null;

    // ---- writing ----------------------------------------------------------------

    async function save(fields, id = "") {
      requireUnlocked();
      const clean = cleanFields(fields);
      if (!id) {
        const itemId = C.newItemId(), itemKey = C.newItemKey();
        await request(API + "/items", "POST", {
          id: itemId, body: await C.sealItem(itemKey, itemId, clean),
          wrappedKey: await C.wrapItemKey(keyset, itemId, itemKey),
        });
        itemKeys.set(itemId, itemKey);
        return itemId;
      }
      const current = find(id), itemKey = itemKeys.get(id);
      if (!current || !itemKey) throw new VaultError("item_not_found");
      if (current.permission === "read") throw new VaultError("read_only");
      await request(`${API}/items/${id}`, "PUT", {
        revision: current.revision, body: await C.sealItem(itemKey, id, clean),
      });
      return id;
    }
    async function remove(id) {
      requireUnlocked();
      await request(`${API}/items/${id}`, "DELETE");
      const key = itemKeys.get(id);
      if (key) { C.wipe(key); itemKeys.delete(id); }
    }

    // ---- sharing ------------------------------------------------------------------

    // Who a share would reach, with each person's fingerprint and whether it
    // is new, pinned, or changed since it was pinned. Nothing is sent yet.
    async function previewShare({ kind, name }) {
      requireUnlocked();
      const clean = String(name || "").trim().toLowerCase();
      let members;
      if (kind === "user") {
        const payload = await request(`${API}/users/${encodeURIComponent(clean)}`);
        members = [{ name: payload.name, bundle: payload.bundle }];
      } else {
        members = (await request(`${API}/organizations/${encodeURIComponent(clean)}/members`)).members || [];
      }
      const out = [];
      for (const member of members) {
        if (!member.bundle) { out.push({ name: member.name, state: "no_vault", fingerprint: "" }); continue; }
        try {
          out.push({ ...(await trustState(member.bundle)), name: member.name, bundle: member.bundle });
        } catch {
          out.push({ name: member.name, state: "invalid_keys", fingerprint: "" });
        }
      }
      return out;
    }
    // Grants for everyone in `members` who may be sealed to. `accepted` names
    // people whose changed fingerprint the owner has just re-verified.
    async function grantsFor(id, itemKey, via, permission, members, accepted = []) {
      const grants = [], skipped = [];
      for (const member of members) {
        if (!member.bundle || member.name === account) continue;
        let seen;
        try { seen = await trustState(member.bundle); } catch { skipped.push(member.name); continue; }
        if (seen.state === "changed" && !accepted.includes(member.name)) { skipped.push(member.name); continue; }
        grants.push({
          recipient: member.name, via,
          grant: await C.sealGrant({
            keyset, sender: account, recipientBundle: member.bundle, itemId: id, itemKey, permission,
          }),
        });
        trust.pins[member.name] = seen.fingerprint;
      }
      return { grants, skipped };
    }
    async function share(id, { kind, name, permission = "read", accepted = [] }) {
      requireUnlocked();
      const current = find(id), itemKey = itemKeys.get(id);
      if (!current || !current.owned || !itemKey) throw new VaultError("owner_required");
      const members = await previewShare({ kind, name });
      const target = { kind, name: String(name).trim().toLowerCase() };
      if (kind === "user") {
        const only = members[0];
        if (!only || !only.bundle) throw new VaultError("recipient_has_no_vault");
        if (only.state === "changed" && !accepted.includes(only.name)) {
          throw new VaultError("recipient_keys_changed", { fingerprint: only.fingerprint });
        }
        target.name = only.name;
      }
      const sealed = await grantsFor(id, itemKey, target, permission, members, accepted);
      await request(`${API}/items/${id}/shares`, "POST", { target, permission, grants: sealed.grants });
      await saveTrust();
      return { sealedTo: sealed.grants.map((grant) => grant.recipient), skipped: sealed.skipped,
               withoutVault: members.filter((m) => m.state === "no_vault").map((m) => m.name) };
    }

    // Re-encrypt an item under a fresh key and seal that key to everyone its
    // remaining shares cover. Whoever was removed keeps only a dead key.
    async function rotate(id) {
      const current = find(id);
      if (!current || !current.owned || !current.fields) throw new VaultError("owner_required");
      const fresh = (await request(API + "/items")).items.find((row) => row.id === id);
      const recipients = (await request(`${API}/items/${id}/recipients`)).recipients || [];
      // Someone with write access may have saved since this list was loaded:
      // what gets re-encrypted is the server's current body, not a stale copy.
      const oldKey = itemKeys.get(id);
      const fields = fresh && oldKey ? await C.openItem(oldKey, id, fresh.body) : current.fields;
      const itemKey = C.newItemKey();
      const grants = [];
      for (const group of recipients) {
        const via = { kind: group.kind, name: group.name };
        grants.push(...(await grantsFor(id, itemKey, via, group.permission, group.members)).grants);
      }
      await request(`${API}/items/${id}`, "PUT", {
        revision: fresh ? fresh.revision : current.revision,
        body: await C.sealItem(itemKey, id, fields),
        wrappedKey: await C.wrapItemKey(keyset, id, itemKey), grants,
      });
      if (oldKey) C.wipe(oldKey);
      itemKeys.set(id, itemKey);
    }
    async function unshare(id, { kind, name }) {
      requireUnlocked();
      await request(`${API}/items/${id}/unshare`, "POST", { kind, name });
      await rotate(id);
    }

    // Members who joined an organization, or made their vault, after an item
    // was shared with it. Only a holder of the item key can seal to them, so
    // the owner's client catches up each time it unlocks.
    async function syncPending() {
      requireUnlocked();
      const pending = (await request(API + "/pending")).pending || [];
      let sealedCount = 0;
      for (const entry of pending) {
        const itemKey = itemKeys.get(entry.itemId);
        if (!itemKey) continue;
        const via = { kind: "organization", name: entry.organization };
        const { grants } = await grantsFor(entry.itemId, itemKey, via, entry.permission, entry.members);
        if (!grants.length) continue;
        await request(`${API}/items/${entry.itemId}/grants`, "POST", { grants });
        sealedCount += grants.length;
      }
      if (sealedCount) await saveTrust();
      return sealedCount;
    }

    async function organizations() {
      return (await request(API + "/organizations")).organizations || [];
    }
    async function myFingerprint() {
      requireUnlocked();
      return C.fingerprint(server.bundle);
    }
    async function acceptKeys(name, fingerprint) {
      requireUnlocked();
      trust.pins[String(name)] = String(fingerprint);
      await saveTrust();
    }
    async function importLogins(logins, { onProgress = null } = {}) {
      requireUnlocked();
      const plan = planImport(logins, items);
      let added = 0, stoppedBy = "";
      for (const fields of plan.fresh) {
        try { await save(fields); } catch (error) { stoppedBy = String(error?.code || error?.message || "error"); break; }
        added += 1;
        onProgress?.(added, plan.fresh.length);
      }
      return { added, duplicates: plan.duplicates, remaining: plan.fresh.length - added, stoppedBy };
    }
    async function exportData() { return request(API + "/export"); }
    async function destroy() {
      const payload = await request(API + "/uninstall", "POST", {});
      lock(); server = null;
      return payload;
    }

    return {
      status, setUp, unlock, resume, exportKeyset, lock, unlocked, recover, changePassword,
      renewRecoveryCode, load, list, find, save, remove, previewShare, share, unshare, rotate,
      syncPending, organizations, myFingerprint, acceptKeys, importLogins, exportData, destroy,
      account: () => account,
      matching: (pageUrl) => items.filter((item) => item.fields && matchesPage(item, pageUrl)),
    };
  }

  return {
    create, VaultError, loginHost, loginHref, matchesPage, cleanFields, FIELD_LIMITS,
    parseCsv, readLoginsCsv, readLoginsFile, importFields, planImport, IMPORT_MAX_BYTES, IMPORT_SOURCES,
  };
}
})(typeof globalThis !== "undefined" ? globalThis : this);

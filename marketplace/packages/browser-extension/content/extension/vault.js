// ForkMesh extension: the Password Vault half of the background worker.
//
// The vault is end-to-end encrypted; vault-crypto.js and vault-client.js are
// the same files the dashboard page runs (copied from the vault package by
// its tools/sync_crypto.py). This file wires them to the extension:
//
// * The unlocked keyset lives in this worker's memory and, because Chrome
//   stops idle service workers, in chrome.storage.session: memory only,
//   never written to disk, gone when the browser closes. It is dropped after
//   fifteen idle minutes, on Lock, and on sign-out.
// * The popup never receives a list with passwords in it. It gets titles and
//   usernames; a password leaves this worker only into the page the user
//   pressed Fill on, or to the popup for one explicit Copy.
// * Filling is always a click in the popup, in the top frame of the active
//   tab, and only when the tab's host is exactly the login's host. No script
//   sits on other sites waiting: the page is touched through activeTab.

(() => {
  const IDLE_MINUTES = 15;
  const ALARM = "forkmesh-vault-lock";
  const SESSION_KEYS = ["vaultKeyset", "vaultAccount", "vaultExpiresAt"];
  let client = null;
  let clientAccount = "";

  class VaultApiError extends Error {
    constructor(code, status = 0) {
      super(String(code || "request_failed").replaceAll("_", " "));
      this.code = String(code || "request_failed"); this.status = status;
    }
  }

  function install({ api, settings, handlers }) {
    const C = self.ForkMeshVaultCrypto, Vault = self.ForkMeshVaultClient;

    async function forget() {
      if (client) client.lock();
      client = null; clientAccount = "";
      await chrome.storage.session.remove(SESSION_KEYS);
      await chrome.alarms.clear(ALARM);
    }
    async function touch() {
      const expires = Date.now() + IDLE_MINUTES * 60000;
      await chrome.storage.session.set({ vaultExpiresAt: expires });
      await chrome.alarms.create(ALARM, { when: expires });
    }
    // The client for the signed-in account, re-entered from session storage
    // when this worker was restarted while the vault was unlocked.
    async function current() {
      const account = (await settings()).account;
      if (!account) { await forget(); throw new VaultApiError("not_signed_in", 401); }
      if (client && clientAccount !== account) await forget();
      if (!client) {
        client = Vault.create({ crypto: C, request: (path, method, body) => api(path, method, body ?? null) });
        clientAccount = account;
      }
      if (!client.unlocked()) {
        const saved = await chrome.storage.session.get(SESSION_KEYS);
        if (saved.vaultKeyset && saved.vaultAccount === account && Number(saved.vaultExpiresAt) > Date.now()) {
          try { await client.resume(saved.vaultKeyset); await client.load(); }
          catch { await forget(); return current(); }
        } else if (saved.vaultKeyset) {
          await chrome.storage.session.remove(SESSION_KEYS);
        }
      }
      return client;
    }
    async function unlockedClient() {
      const vault = await current();
      if (!vault.unlocked()) throw new VaultApiError("vault_locked", 423);
      await touch();
      return vault;
    }

    function summary(item) {
      const fields = item.fields || {};
      return {
        id: item.id, title: String(fields.title || ""), username: String(fields.username || ""),
        host: Vault.loginHost(fields.url || ""), owned: Boolean(item.owned), owner: String(item.owner || ""),
        canEdit: item.permission !== "read", hasPassword: Boolean(fields.password),
      };
    }

    async function vaultStatus() {
      const vault = await current();
      const state = await vault.status();
      return { ok: true, installed: state.installed, unlocked: vault.unlocked(), account: state.account };
    }
    async function vaultUnlock({ password }) {
      const vault = await current();
      await vault.unlock(String(password || ""));
      await vault.load();
      await chrome.storage.session.set({ vaultKeyset: vault.exportKeyset(), vaultAccount: clientAccount });
      await touch();
      // Catch up organization members who joined since the last unlock.
      await vault.syncPending().catch(() => 0);
      return { ok: true, unlocked: true };
    }
    async function vaultLock() { await forget(); return { ok: true, unlocked: false }; }

    async function vaultItems({ url, query, refresh }) {
      const vault = await unlockedClient();
      if (refresh) await vault.load();
      const needle = String(query || "").trim().toLowerCase();
      const matches = vault.matching(String(url || "")).map(summary);
      const matched = new Set(matches.map((item) => item.id));
      const others = vault.list().filter((item) => item.fields && !matched.has(item.id)).map(summary)
        .filter((item) => !needle || [item.title, item.username, item.host].some((text) => text.toLowerCase().includes(needle)));
      return { ok: true, host: Vault.loginHost(String(url || "")), matches, others: needle ? others.slice(0, 30) : [] };
    }

    // Runs inside the page. Self-contained: it is serialized into the tab.
    function fillInPage(username, password, host) {
      if (location.hostname.toLowerCase().replace(/^www\./, "") !== host) {
        return { filledUsername: false, filledPassword: false };
      }
      const usable = (node) => node instanceof HTMLInputElement && !node.disabled && !node.readOnly
        && node.getClientRects().length > 0 && getComputedStyle(node).visibility !== "hidden";
      const write = (node, value) => {
        const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set;
        node.focus(); setter.call(node, value);
        node.dispatchEvent(new Event("input", { bubbles: true }));
        node.dispatchEvent(new Event("change", { bubbles: true }));
      };
      const inputs = [...document.querySelectorAll("input")].filter(usable);
      const passwordField = inputs.find((node) => node.type === "password");
      const textLike = (node) => ["text", "email", "tel", ""].includes(node.type);
      let userField = null;
      if (passwordField) {
        const scope = passwordField.form ? [...passwordField.form.elements].filter(usable) : inputs;
        const before = scope.slice(0, scope.indexOf(passwordField)).filter(textLike);
        userField = before[before.length - 1] || null;
      } else {
        userField = inputs.find((node) => textLike(node)
          && /user|login|email|identifier/i.test(`${node.autocomplete} ${node.name} ${node.id}`)) || null;
      }
      if (userField && username) write(userField, username);
      if (passwordField && password) write(passwordField, password);
      return { filledUsername: Boolean(userField && username), filledPassword: Boolean(passwordField && password) };
    }
    function captureInPage() {
      const usable = (node) => node instanceof HTMLInputElement && !node.disabled
        && node.getClientRects().length > 0;
      const inputs = [...document.querySelectorAll("input")].filter(usable);
      const passwordField = inputs.find((node) => node.type === "password" && node.value);
      if (!passwordField) return { found: false };
      const scope = passwordField.form ? [...passwordField.form.elements].filter(usable) : inputs;
      const before = scope.slice(0, scope.indexOf(passwordField))
        .filter((node) => ["text", "email", "tel", ""].includes(node.type) && node.value);
      return { found: true, username: before.length ? before[before.length - 1].value : "", password: passwordField.value, title: document.title };
    }

    async function activeTab() {
      const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
      if (!tab?.id || !tab.url) throw new VaultApiError("no_active_tab", 400);
      return tab;
    }
    async function vaultFill({ id }) {
      const vault = await unlockedClient();
      const item = vault.find(String(id || ""));
      if (!item?.fields) throw new VaultApiError("item_not_found", 404);
      const tab = await activeTab();
      // Checked here, against the tab Chrome reports, not against anything the
      // popup or the page claims: a login only ever goes to its own host.
      if (!Vault.matchesPage(item, tab.url)) throw new VaultApiError("site_mismatch", 409);
      const [result] = await chrome.scripting.executeScript({
        target: { tabId: tab.id }, func: fillInPage,
        args: [String(item.fields.username || ""), String(item.fields.password || ""), Vault.loginHost(tab.url)],
      });
      return { ok: true, ...(result?.result || {}) };
    }
    async function vaultCopy({ id, field }) {
      const vault = await unlockedClient();
      const item = vault.find(String(id || ""));
      if (!item?.fields || !["username", "password"].includes(field)) throw new VaultApiError("item_not_found", 404);
      return { ok: true, value: String(item.fields[field] || "") };
    }
    async function vaultCapture() {
      await unlockedClient();
      const tab = await activeTab();
      if (!Vault.loginHost(tab.url)) throw new VaultApiError("page_not_secure", 409);
      const [result] = await chrome.scripting.executeScript({ target: { tabId: tab.id }, func: captureInPage });
      const found = result?.result || { found: false };
      if (!found.found) throw new VaultApiError("no_login_on_page", 404);
      const page = new URL(tab.url);
      return { ok: true, login: { title: found.title || page.hostname, url: page.origin, username: found.username, password: found.password } };
    }
    async function vaultSave({ fields, id }) {
      const vault = await unlockedClient();
      const saved = await vault.save(fields || {}, String(id || ""));
      await vault.load();
      return { ok: true, id: saved };
    }
    async function openVault() {
      await chrome.tabs.create({ url: `${(await settings()).baseUrl}/dashboard/vault` });
      return { ok: true };
    }

    Object.assign(handlers, {
      "fm-vault-status": vaultStatus,
      "fm-vault-unlock": vaultUnlock,
      "fm-vault-lock": vaultLock,
      "fm-vault-items": vaultItems,
      "fm-vault-fill": vaultFill,
      "fm-vault-copy": vaultCopy,
      "fm-vault-capture": vaultCapture,
      "fm-vault-save": vaultSave,
      "fm-vault-generate": async (options) => ({ ok: true, password: C.generatePassword(options || {}) }),
      "fm-open-vault": openVault,
    });
    chrome.alarms.onAlarm.addListener((alarm) => { if (alarm.name === ALARM) void forget(); });
    // Session storage is already closed to content scripts by default; say so
    // outright, so the scripts this extension runs inside Gmail, Zoho Mail,
    // Mastodon and X can never be given a way to read the keyset.
    void chrome.storage.session.setAccessLevel?.({ accessLevel: "TRUSTED_CONTEXTS" });
    return { forget };
  }

  self.ForkMeshVaultBackground = { install };
})();

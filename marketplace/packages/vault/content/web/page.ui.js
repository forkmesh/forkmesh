// Password Vault dashboard page. The cryptography and the vault client above
// this line are embedded copies of content/crypto/*.js (tools/sync_crypto.py
// writes them; the page may load only this one script). Everything below is
// screen logic: it never sees a key, only what the client hands back.
window.ForkMeshDashboard.registerPage("vault", ({
  select: $, selectAll: $$, session, escapeHtml,
}) => {
  const C = globalThis.ForkMeshVaultCrypto;
  const Vault = globalThis.ForkMeshVaultClient;
  const AUTOLOCK_KEY = "forkmesh.vault.autolockMinutes";
  const CLIPBOARD_CLEAR_MS = 30000;
  const state = { client: null, editing: "", sharing: "", preview: null, idleTimer: 0, clipboardTimer: 0, toastTimer: 0, afterRecovery: null };

  // --- helpers ----------------------------------------------------------------
  function message(kind, value = "") {
    const node = $(`[data-vault-${kind}]`); if (!node) return;
    (node.querySelector("[data-vault-message-text]") || node).textContent = value; node.classList.toggle("hidden", !value);
    if (value) node.scrollIntoView?.({ block: "nearest" });
  }
  function toast(value) {
    const node = $(`[data-vault-toast]`); if (!node) { message("notice", value); return; }
    node.querySelector("[data-vault-toast-text]").textContent = value;
    node.classList.remove("hidden");
    try { node.hidePopover?.(); node.showPopover?.(); } catch {}
    clearTimeout(state.toastTimer);
    state.toastTimer = setTimeout(() => { node.classList.add("hidden"); try { node.hidePopover?.(); } catch {} }, 4000);
  }
  function ask(text, { title = "Are you sure?", action = "Continue", danger = false } = {}) {
    const dialog = $(`[data-vault-confirm-dialog]`);
    if (!dialog) return Promise.resolve(confirm(text));
    $(`[data-vault-confirm-title]`).textContent = title;
    $(`[data-vault-confirm-text]`).textContent = text;
    const accept = $(`[data-vault-confirm-accept]`);
    accept.textContent = action;
    accept.classList.toggle("fm-btn-danger", danger);
    accept.classList.toggle("fm-btn-cta", !danger);
    dialog.returnValue = "";
    return new Promise((resolve) => {
      dialog.addEventListener("close", () => resolve(dialog.returnValue === "confirm"), { once: true });
      dialog.showModal();
    });
  }
  function clearMessages() { message("error"); message("notice"); }
  function icons() { window.lucide?.createIcons?.(); }
  const WORDS = {
    wrong_password: "That is not the master password.",
    wrong_recovery_code: "That recovery code does not open this vault.",
    invalid_recovery_code: "A recovery code is 13 groups of 4 letters and digits.",
    unsafe_kdf_parameters: "The server returned key-stretching settings weaker than this vault allows. Nothing was unlocked.",
    keyset_mismatch: "The keys stored for this account do not match its published keys. Nothing was unlocked.",
    recipient_has_no_vault: "That person has not created a vault yet, so there are no keys to seal to.",
    recipient_not_found: "No ForkMesh user has that name.",
    recipient_keys_changed: "That person's keys changed since you last shared with them. Compare fingerprints before sharing.",
    organization_not_found: "You are not a member of that organization.",
    revision_conflict: "This login changed somewhere else. It was reloaded; make the change again.",
    read_only: "This login was shared with you to view, not to edit.",
    vault_locked: "The vault is locked.",
    vault_full: "This vault is full.",
    title_required: "Give the login a title.",
    csv_too_large: "That file is too large to be a password export.",
    csv_empty: "That file is empty.",
    csv_unknown_columns: "That file has no Password column. Export a CSV file from your password manager and try again.",
    csv_no_logins: "That file has no logins in it.",
    zip_unreadable: "That ZIP file is damaged or uses a format this browser cannot open.",
    zip_encrypted: "That ZIP file is password-protected. Export it again without a password.",
    zip_no_csv: "That ZIP file has no CSV file of passwords in it.",
  };
  function explain(error) {
    const code = String(error?.code || error?.payload?.error || "");
    return WORDS[code] || String(error?.message || "Something went wrong.").replaceAll("_", " ");
  }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = session()?.sessionToken;
    // The website session is an HttpOnly cookie ("cookie" is only its marker);
    // a bearer is sent just for the clients that really hold one.
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", credentials: "same-origin", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`));
      error.code = String(payload.error || ""); error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }
  async function busy(node, work) {
    node?.classList.add("vault-busy");
    try { return await work(); } finally { node?.classList.remove("vault-busy"); }
  }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  // A rough floor, not a promise: length and variety, nothing more.
  function strength(password) {
    const value = String(password || "");
    const variety = [/[a-z]/, /[A-Z]/, /[0-9]/, /[^A-Za-z0-9]/].filter((re) => re.test(value)).length;
    const score = Math.min(100, Math.round(value.length * (3 + variety)));
    return { score, level: value.length < 12 || score < 60 ? "weak" : "ok" };
  }
  function checkNewPassword(password, confirmation) {
    if (String(password).length < 12) throw Object.assign(new Error("Use at least 12 characters for the master password."), { code: "" });
    if (password !== confirmation) throw Object.assign(new Error("The two passwords do not match."), { code: "" });
  }
  async function copy(value, label) {
    try {
      await navigator.clipboard.writeText(value);
    } catch {
      message("error", "This browser did not allow copying."); return;
    }
    toast(`${label} copied. The clipboard is cleared in 30 seconds.`);
    clearTimeout(state.clipboardTimer);
    state.clipboardTimer = setTimeout(() => { navigator.clipboard.writeText("").catch(() => {}); }, CLIPBOARD_CLEAR_MS);
  }

  // --- lock state -----------------------------------------------------------------
  function show(panel) {
    for (const name of ["unsupported", "setup", "unlock", "app"]) {
      $(`[data-vault-${name}]`)?.classList.toggle("hidden", name !== panel);
    }
    $$(`[data-vault-requires-unlock]`).forEach((node) => node.classList.toggle("hidden", panel !== "app"));
  }
  function autolockMinutes() {
    const stored = Number(localStorage.getItem(AUTOLOCK_KEY));
    return [1, 5, 10, 30].includes(stored) ? stored : 10;
  }
  function touch() {
    clearTimeout(state.idleTimer);
    if (state.client?.unlocked()) state.idleTimer = setTimeout(() => lock("Locked after being idle."), autolockMinutes() * 60000);
  }
  function lock(note = "") {
    clearTimeout(state.idleTimer);
    state.client?.lock();
    state.editing = ""; state.sharing = ""; state.preview = null;
    $$("dialog[open]").forEach((dialog) => { if (dialog.closest("[data-view='vault']")) dialog.close(); });
    $$(`[data-vault-field], [data-vault-unlock-password], [data-vault-recovery-code]`).forEach((node) => { if ("value" in node) node.value = ""; else node.textContent = ""; });
    const list = $(`[data-vault-list]`); if (list) list.innerHTML = "";
    show("unlock"); clearMessages(); if (note) message("notice", note);
  }
  async function enter() {
    show("app");
    await reload();
    const sealed = await state.client.syncPending().catch(() => 0);
    if (sealed) message("notice", `Sealed shared logins to ${sealed} new organization member${sealed === 1 ? "" : "s"}.`);
    touch();
  }
  function showRecoveryCode(code, then) {
    $(`[data-vault-recovery-code]`).textContent = code;
    $(`[data-vault-recovery-saved]`).checked = false;
    $(`[data-vault-recovery-done]`).disabled = true;
    state.afterRecovery = then;
    $(`[data-vault-recovery-dialog]`).showModal();
  }

  // --- list -----------------------------------------------------------------------
  function visibleItems() {
    const query = String($(`[data-vault-search]`)?.value || "").trim().toLowerCase();
    const filter = $(`[data-vault-filter]`)?.value || "all";
    return state.client.list().filter((item) => {
      if (filter === "mine" && !item.owned) return false;
      if (filter === "shared" && item.owned) return false;
      if (!query) return true;
      const fields = item.fields || {};
      return [fields.title, fields.username, fields.url, item.owner].some((text) => String(text || "").toLowerCase().includes(query));
    });
  }
  function shareBadges(item) {
    if (!item.owned) {
      const via = item.via?.kind === "organization" ? ` via ${escapeHtml(item.via.name)}` : "";
      return `<span class="fm-label">from ${escapeHtml(item.owner)}${via}</span> <span class="fm-label">${item.permission === "write" ? "can edit" : "view only"}</span>`;
    }
    return (item.shares || []).map((share) => `<span class="fm-label">${share.kind === "organization" ? "org " : ""}${escapeHtml(share.name)}</span>`).join(" ");
  }
  function renderList() {
    const items = visibleItems();
    const host = $(`[data-vault-list]`); if (!host) return;
    host.classList.toggle("hidden", !items.length);
    $(`[data-vault-empty]`)?.classList.toggle("hidden", Boolean(items.length));
    host.innerHTML = items.map((item) => {
      if (!item.fields) {
        const why = item.error === "sender_keys_changed"
          ? `${escapeHtml(item.owner)}'s keys changed since you first received a share from them. It stays closed until you compare fingerprints with them.`
          : "This item could not be opened with your keys.";
        return `<div class="vault-row p-3"><div><p class="text-sm font-semibold text-foreground">Unreadable item <span class="fm-label fm-label-danger" data-tone="warn">${escapeHtml(item.error || "error")}</span></p><p class="text-xs text-muted-foreground">${why}</p></div><div></div></div>`;
      }
      const fields = item.fields, site = Vault.loginHost(fields.url || "");
      return `<div class="vault-row p-3" data-vault-item="${escapeHtml(item.id)}">
        <button type="button" data-vault-open class="min-w-0 text-left"><p class="truncate text-sm font-semibold text-foreground">${escapeHtml(fields.title)}</p><p class="truncate text-xs text-muted-foreground">${escapeHtml(fields.username || "")}${fields.username && site ? " · " : ""}${escapeHtml(site)}</p><p class="mt-1 flex flex-wrap gap-1">${shareBadges(item)}</p></button>
        <div class="flex flex-wrap items-center justify-end gap-1">
          <button type="button" data-vault-copy="username" title="Copy username" class="fm-btn fm-btn-sm"><i data-lucide="copy"></i>User</button>
          <button type="button" data-vault-copy="password" title="Copy password" class="fm-btn fm-btn-sm"><i data-lucide="copy"></i>Password</button>
          ${site ? `<button type="button" data-vault-visit title="Open the website" class="fm-btn fm-btn-sm"><i data-lucide="external-link"></i>Open</button>` : ""}
          ${item.owned ? `<button type="button" data-vault-share title="Share" class="fm-btn fm-btn-sm"><i data-lucide="share-2"></i>Share</button>` : ""}
        </div></div>`;
    }).join("");
    icons();
  }
  async function reload() {
    await state.client.load();
    renderList();
  }

  // --- item editor ------------------------------------------------------------------
  function openEditor(id = "") {
    const item = id ? state.client.find(id) : null;
    state.editing = id;
    const readOnly = Boolean(item && item.permission === "read");
    $(`[data-vault-item-heading]`).textContent = item ? (readOnly ? "Login" : "Edit login") : "New login";
    for (const node of $$(`[data-vault-field]`)) {
      node.value = item ? String(item.fields[node.dataset.vaultField] || "") : "";
      node.readOnly = readOnly;
    }
    const password = $(`[data-vault-field="password"]`); password.type = "password";
    $(`[data-vault-reveal]`).textContent = "Show";
    $(`[data-vault-generate]`).classList.toggle("hidden", readOnly);
    $(`[data-vault-item-save]`).classList.toggle("hidden", readOnly);
    const remove = $(`[data-vault-item-delete]`);
    remove.classList.toggle("hidden", !item || (!item.owned && item.via?.kind !== "user"));
    remove.textContent = item && !item.owned ? "Remove from my vault" : "Delete";
    const origin = $(`[data-vault-item-origin]`);
    origin.classList.toggle("hidden", !item || item.owned);
    origin.textContent = item && !item.owned ? `Shared by ${item.owner}${item.updatedBy && item.updatedBy !== item.owner ? `, last changed by ${item.updatedBy}` : ""}.` : "";
    $(`[data-vault-item-dialog]`).showModal();
  }
  async function saveItem(event) {
    event.preventDefault(); clearMessages();
    const fields = {};
    for (const node of $$(`[data-vault-field]`)) fields[node.dataset.vaultField] = node.value;
    try {
      await busy(event.target, async () => { await state.client.save(fields, state.editing); await reload(); });
      $(`[data-vault-item-dialog]`).close();
    } catch (error) {
      if (error.code === "revision_conflict") await reload().catch(() => {});
      $(`[data-vault-item-dialog]`).close(); message("error", explain(error));
    }
  }
  async function deleteItem() {
    const item = state.client.find(state.editing); if (!item) return;
    const question = item.owned
      ? `Delete "${item.fields.title}"? Everyone it is shared with loses it too. This cannot be undone.`
      : `Remove "${item.fields.title}" from your vault? ${item.owner} keeps it.`;
    if (!(await ask(question, item.owned ? { title: "Delete login", action: "Delete", danger: true } : { title: "Remove login", action: "Remove", danger: true }))) return;
    try { await state.client.remove(item.id); await reload(); $(`[data-vault-item-dialog]`).close(); }
    catch (error) { $(`[data-vault-item-dialog]`).close(); message("error", explain(error)); }
  }

  // --- sharing ----------------------------------------------------------------------
  function renderShares() {
    const item = state.client.find(state.sharing); if (!item) return;
    $(`[data-vault-share-title]`).textContent = item.fields.title;
    const host = $(`[data-vault-share-current]`);
    const shares = item.shares || [];
    host.innerHTML = shares.length ? shares.map((share) => `<div class="vault-row p-3"><div><p class="text-sm text-foreground">${share.kind === "organization" ? "Organization " : ""}<b>${escapeHtml(share.name)}</b></p><p class="text-xs text-muted-foreground">${share.permission === "write" ? "Can view and edit" : "Can view and use"}</p></div><button type="button" data-vault-unshare-kind="${escapeHtml(share.kind)}" data-vault-unshare-name="${escapeHtml(share.name)}" class="fm-btn fm-btn-sm fm-btn-danger">Stop sharing</button></div>`).join("")
      : `<div class="fm-blankslate vault-compact"><p class="fm-blankslate-desc">Not shared with anyone.</p></div>`;
  }
  async function openShare(id) {
    state.sharing = id; state.preview = null; clearMessages();
    $(`[data-vault-share-preview]`).classList.add("hidden");
    $(`[data-vault-share-name]`).value = "";
    renderShares(); syncShareKind();
    const select = $(`[data-vault-share-org]`);
    const organizations = await state.client.organizations().catch(() => []);
    select.innerHTML = organizations.map((org) => `<option value="${escapeHtml(org.name)}">${escapeHtml(org.name)}</option>`).join("");
    const kind = $(`[data-vault-share-kind]`);
    kind.querySelector(`option[value="organization"]`).disabled = !organizations.length;
    $(`[data-vault-share-dialog]`).showModal();
  }
  function syncShareKind() {
    const organization = $(`[data-vault-share-kind]`).value === "organization";
    $(`[data-vault-share-name]`).classList.toggle("hidden", organization);
    $(`[data-vault-share-org]`).classList.toggle("hidden", !organization);
    $(`label[for="vaultShareName"]`).textContent = organization ? "Organization" : "Username";
    $(`[data-vault-share-preview]`).classList.add("hidden"); state.preview = null;
  }
  function shareTarget() {
    const kind = $(`[data-vault-share-kind]`).value;
    const name = (kind === "organization" ? $(`[data-vault-share-org]`).value : $(`[data-vault-share-name]`).value).trim().toLowerCase();
    return { kind, name, permission: $(`[data-vault-share-permission]`).value };
  }
  const STATE_LABEL = { new: "first share", pinned: "known keys", changed: "keys changed", no_vault: "no vault yet", invalid_keys: "invalid keys" };
  const STATE_TONE = { pinned: "fm-label-success", changed: "fm-label-danger", invalid_keys: "fm-label-danger", new: "fm-label-accent" };
  async function checkShare(event) {
    event.preventDefault(); clearMessages();
    const target = shareTarget(); if (!target.name) return;
    try {
      const members = await busy(event.target, () => state.client.previewShare(target));
      state.preview = { target, members };
      $(`[data-vault-share-members]`).innerHTML = members.map((member) => `<div class="vault-row p-3"><div class="min-w-0"><p class="text-sm text-foreground"><b>${escapeHtml(member.name)}</b> <span class="fm-label ${STATE_TONE[member.state] || ""}" ${member.state === "changed" || member.state === "invalid_keys" ? `data-tone="warn"` : ""}>${STATE_LABEL[member.state] || member.state}</span></p>${member.fingerprint && member.state !== "pinned" ? `<p class="vault-fingerprint text-muted-foreground">${escapeHtml(member.fingerprint)}</p>` : ""}</div><div></div></div>`).join("")
        || `<div class="fm-blankslate vault-compact"><p class="fm-blankslate-desc">Nobody else is in this organization yet.</p></div>`;
      const changed = members.filter((member) => member.state === "changed");
      const fresh = members.filter((member) => member.state === "new");
      const without = members.filter((member) => member.state === "no_vault");
      const advice = [];
      if (changed.length) advice.push(`${changed.map((m) => m.name).join(", ")} now ${changed.length === 1 ? "has" : "have"} different keys than you pinned. That happens when someone deletes and recreates their vault, and it is also what an attack looks like. Compare the fingerprint with them before going on.`);
      if (fresh.length && target.kind === "user") advice.push("First share with this person. To be certain these keys are theirs, ask them to read their fingerprint (Vault settings) to you.");
      if (without.length) advice.push(`${without.length} member${without.length === 1 ? " has" : "s have"} no vault yet. They get access the next time you unlock after they create one.`);
      const adviceNode = $(`[data-vault-share-advice]`);
      adviceNode.textContent = advice.join(" ");
      adviceNode.classList.toggle("fm-flash-warning", Boolean(changed.length));
      adviceNode.classList.toggle("fm-flash-neutral", !changed.length);
      $(`[data-vault-share-confirm-row]`).classList.toggle("hidden", !changed.length);
      $(`[data-vault-share-confirm]`).checked = false;
      $(`[data-vault-share-preview]`).classList.remove("hidden");
    } catch (error) {
      $(`[data-vault-share-preview]`).classList.add("hidden");
      $(`[data-vault-share-advice]`).textContent = "";
      alertInDialog(explain(error));
    }
  }
  function alertInDialog(text) { $(`[data-vault-share-dialog]`).close(); message("error", text); }
  async function submitShare() {
    if (!state.preview) return;
    const { target, members } = state.preview;
    const changed = members.filter((member) => member.state === "changed").map((member) => member.name);
    const accepted = $(`[data-vault-share-confirm]`).checked ? changed : [];
    if (target.kind === "user" && changed.length && !accepted.length) { $(`[data-vault-share-advice]`).textContent = "Tick the box once you have compared the fingerprint with them."; return; }
    try {
      const result = await busy($(`[data-vault-share-submit]`), () => state.client.share(state.sharing, { ...target, accepted }));
      await reload(); renderShares(); $(`[data-vault-share-preview]`).classList.add("hidden"); state.preview = null;
      const skipped = result.skipped.length ? ` Skipped (keys changed): ${result.skipped.join(", ")}.` : "";
      $(`[data-vault-share-dialog]`).close();
      message("notice", `Shared with ${target.kind === "organization" ? `${target.name} (${result.sealedTo.length} member${result.sealedTo.length === 1 ? "" : "s"} sealed)` : target.name}.${skipped}`);
    } catch (error) { alertInDialog(explain(error)); }
  }
  async function stopSharing(button) {
    const target = { kind: button.dataset.vaultUnshareKind, name: button.dataset.vaultUnshareName };
    if (!(await ask(`Stop sharing with ${target.name}? The login is re-encrypted under a new key. If they already saw the password, change it on the website too.`, { title: "Stop sharing", action: "Stop sharing", danger: true }))) return;
    try { await busy(button, () => state.client.unshare(state.sharing, target)); await reload(); renderShares(); }
    catch (error) { alertInDialog(explain(error)); }
  }

  // --- forms ------------------------------------------------------------------------
  async function createVault(event) {
    event.preventDefault(); clearMessages();
    const password = $(`[data-vault-setup-password]`).value, confirmation = $(`[data-vault-setup-confirm]`).value;
    try {
      checkNewPassword(password, confirmation);
      const { recoveryCode } = await busy(event.target, () => state.client.setUp(password));
      event.target.reset();
      showRecoveryCode(recoveryCode, () => void enter().catch((error) => message("error", explain(error))));
    } catch (error) { message("error", explain(error)); }
  }
  async function unlockVault(event) {
    event.preventDefault(); clearMessages();
    const field = $(`[data-vault-unlock-password]`);
    try {
      await busy(event.target, () => state.client.unlock(field.value));
      field.value = ""; await enter();
    } catch (error) { field.select?.(); message("error", explain(error)); }
  }
  async function recoverVault(event) {
    event.preventDefault(); clearMessages();
    const password = $(`[data-vault-recover-password]`).value;
    try {
      checkNewPassword(password, $(`[data-vault-recover-confirm]`).value);
      const { recoveryCode } = await busy(event.target, () => state.client.recover($(`[data-vault-recover-code]`).value, password));
      event.target.reset(); $(`[data-vault-recover-dialog]`).close();
      showRecoveryCode(recoveryCode, () => void enter().catch((error) => message("error", explain(error))));
    } catch (error) { $(`[data-vault-recover-dialog]`).close(); message("error", explain(error)); }
  }
  async function changePassword(event) {
    event.preventDefault(); clearMessages();
    const password = $(`[data-vault-new-password]`).value;
    try {
      checkNewPassword(password, $(`[data-vault-new-confirm]`).value);
      await busy(event.target, () => state.client.changePassword(password));
      event.target.reset(); $(`[data-vault-settings-dialog]`).close();
      message("notice", "Master password changed. Other devices need the new one at their next unlock.");
    } catch (error) { $(`[data-vault-settings-dialog]`).close(); message("error", explain(error)); }
  }
  async function renewRecovery() {
    if (!(await ask("Issue a new recovery code? The current one stops working.", { title: "New recovery code", action: "Issue new code" }))) return;
    try {
      const { recoveryCode } = await state.client.renewRecoveryCode();
      $(`[data-vault-settings-dialog]`).close(); showRecoveryCode(recoveryCode, null);
    } catch (error) { $(`[data-vault-settings-dialog]`).close(); message("error", explain(error)); }
  }
  async function destroyVault() {
    if (!(await ask("Delete your vault? Every login in it is erased here and for everyone you shared it with. An encrypted export downloads first; it opens only with your master password or recovery code.", { title: "Delete vault", action: "Delete vault", danger: true }))) return;
    try {
      const payload = await state.client.destroy();
      downloadJson(payload.export || {}, payload.filename || "forkmesh-vault.json");
      $(`[data-vault-settings-dialog]`).close(); show("setup"); message("notice", "Vault deleted.");
    } catch (error) { $(`[data-vault-settings-dialog]`).close(); message("error", explain(error)); }
  }
  async function importFile(file) {
    clearMessages(); $(`[data-vault-settings-dialog]`).close();
    if (!file) return;
    let read;
    try { read = await Vault.readImportFile(await file.arrayBuffer()); }
    catch (error) { message("error", explain(error)); return; }
    const plan = Vault.planImport(read.logins, state.client.list());
    const source = read.sourceLabel;
    if (!plan.fresh.length) { message("notice", `Every login from ${source} is already in your vault.`); return; }
    const parts = [`Import ${plan.fresh.length} login${plan.fresh.length === 1 ? "" : "s"} from ${source}? Each one is encrypted in this browser before it is uploaded.`];
    if (plan.duplicates) parts.push(`${plan.duplicates} already in the vault ${plan.duplicates === 1 ? "is" : "are"} skipped.`);
    if (read.skippedRows) parts.push(`${read.skippedRows} row${read.skippedRows === 1 ? " has" : "s have"} no username or password and ${read.skippedRows === 1 ? "is" : "are"} left out.`);
    if (!(await ask(parts.join(" "), { title: "Import passwords", action: "Import" }))) return;
    const button = $(`[data-vault-import]`);
    try {
      const report = await busy(button, () => state.client.importLogins(read.logins, (done, total) => {
        message("notice", `Importing ${done} of ${total}…`); touch();
      }));
      renderList();
      const summary = [`Imported ${report.added} login${report.added === 1 ? "" : "s"} from ${source}.`];
      if (report.duplicates) summary.push(`Skipped ${report.duplicates} already in the vault.`);
      if (report.stoppedBy) summary.push(`Stopped with ${report.remaining} left: ${explain({ code: report.stoppedBy, message: report.stoppedBy })}`);
      summary.push("Delete the exported file now: it holds your passwords in plain text.");
      clearMessages();
      message(report.stoppedBy ? "error" : "notice", summary.join(" "));
    } catch (error) { renderList(); clearMessages(); message("error", explain(error)); }
  }
  async function openSettings() {
    $(`[data-vault-my-fingerprint]`).textContent = await state.client.myFingerprint().catch(() => "");
    $(`[data-vault-autolock]`).value = String(autolockMinutes());
    $(`[data-vault-settings-dialog]`).showModal();
  }

  // --- start ------------------------------------------------------------------------
  async function supported() {
    try {
      await crypto.subtle.generateKey({ name: "X25519" }, false, ["deriveBits"]);
      await crypto.subtle.generateKey({ name: "Ed25519" }, false, ["sign", "verify"]);
      return true;
    } catch { return false; }
  }
  async function init() {
    bind(); icons();
    if (!C || !Vault || !(await supported())) { show("unsupported"); return; }
    state.client = Vault.create({ crypto: C, request });
    try {
      const status = await state.client.status();
      $$(`[data-vault-setup-account], [data-vault-unlock-account], [data-vault-recover-account], [data-vault-password-account]`).forEach((node) => { node.value = `${status.account} (ForkMesh vault)`; });
      show(status.installed ? "unlock" : "setup");
      if (status.installed) $(`[data-vault-unlock-password]`)?.focus();
    } catch (error) {
      show("unlock"); message("error", error.status === 401 ? "Sign in to ForkMesh to open your vault." : explain(error));
    }
  }
  function bind() {
    const view = $(`[data-view="vault"]`);
    for (const type of ["pointerdown", "keydown"]) view?.addEventListener(type, touch, { passive: true });
    // A vault left open in a background tab is locked when the page goes away.
    window.addEventListener("pagehide", () => state.client?.lock());
    $(`[data-vault-setup-form]`)?.addEventListener("submit", (event) => void createVault(event));
    $(`[data-vault-setup-password]`)?.addEventListener("input", (event) => {
      const { score, level } = strength(event.target.value);
      const meter = $(`[data-vault-setup-meter]`); meter.dataset.level = level; meter.firstElementChild.style.width = `${score}%`;
      $(`[data-vault-setup-hint]`).textContent = level === "weak" ? "Too easy to guess. Make it longer: several unrelated words." : "Long enough. Make sure you can remember it.";
    });
    $(`[data-vault-unlock-form]`)?.addEventListener("submit", (event) => void unlockVault(event));
    $(`[data-vault-recover-open]`)?.addEventListener("click", () => $(`[data-vault-recover-dialog]`).showModal());
    $(`[data-vault-recover-form]`)?.addEventListener("submit", (event) => void recoverVault(event));
    $(`[data-vault-lock]`)?.addEventListener("click", () => lock());
    $(`[data-vault-settings-open]`)?.addEventListener("click", () => void openSettings());
    $(`[data-vault-search]`)?.addEventListener("input", renderList);
    $(`[data-vault-filter]`)?.addEventListener("change", renderList);
    $(`[data-vault-new]`)?.addEventListener("click", () => openEditor());
    $$(`[data-vault-import]`).forEach((button) => button.addEventListener("click", () => $(`[data-vault-import-file]`)?.click()));
    $(`[data-vault-import-file]`)?.addEventListener("change", (event) => {
      const file = event.target.files?.[0]; event.target.value = "";
      void importFile(file);
    });
    $(`[data-vault-list]`)?.addEventListener("click", (event) => {
      const row = event.target.closest("[data-vault-item]"); if (!row) return;
      const item = state.client.find(row.dataset.vaultItem); if (!item?.fields) return;
      const copyButton = event.target.closest("[data-vault-copy]");
      if (copyButton) { void copy(item.fields[copyButton.dataset.vaultCopy] || "", copyButton.dataset.vaultCopy === "password" ? "Password" : "Username"); return; }
      if (event.target.closest("[data-vault-visit]")) {
        const href = Vault.loginHref(item.fields.url);
        if (href) window.open(href, "_blank", "noopener,noreferrer");
        return;
      }
      if (event.target.closest("[data-vault-share]")) { void openShare(item.id); return; }
      if (event.target.closest("[data-vault-open]")) openEditor(item.id);
    });
    $(`[data-vault-item-form]`)?.addEventListener("submit", (event) => void saveItem(event));
    $(`[data-vault-item-delete]`)?.addEventListener("click", () => void deleteItem());
    $(`[data-vault-reveal]`)?.addEventListener("click", (event) => {
      const field = $(`[data-vault-field="password"]`); const hidden = field.type === "password";
      field.type = hidden ? "text" : "password"; event.target.textContent = hidden ? "Hide" : "Show";
    });
    $(`[data-vault-generate]`)?.addEventListener("click", async () => {
      const field = $(`[data-vault-field="password"]`);
      if (field.value && !(await ask("Replace the current password with a generated one?", { title: "Replace password", action: "Replace" }))) return;
      field.value = C.generatePassword({ length: 24 }); field.type = "text"; $(`[data-vault-reveal]`).textContent = "Hide";
    });
    $(`[data-vault-share-kind]`)?.addEventListener("change", syncShareKind);
    $(`[data-vault-share-form]`)?.addEventListener("submit", (event) => void checkShare(event));
    $(`[data-vault-share-submit]`)?.addEventListener("click", () => void submitShare());
    $(`[data-vault-share-current]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-vault-unshare-kind]"); if (button) void stopSharing(button); });
    $(`[data-vault-recovery-saved]`)?.addEventListener("change", (event) => { $(`[data-vault-recovery-done]`).disabled = !event.target.checked; });
    $(`[data-vault-recovery-copy]`)?.addEventListener("click", () => void copy($(`[data-vault-recovery-code]`).textContent, "Recovery code"));
    $(`[data-vault-recovery-print]`)?.addEventListener("click", () => window.print());
    $(`[data-vault-recovery-dialog]`)?.addEventListener("cancel", (event) => event.preventDefault());
    $(`[data-vault-recovery-done]`)?.addEventListener("click", () => {
      $(`[data-vault-recovery-code]`).textContent = ""; $(`[data-vault-recovery-dialog]`).close();
      const then = state.afterRecovery; state.afterRecovery = null; then?.();
    });
    $(`[data-vault-password-form]`)?.addEventListener("submit", (event) => void changePassword(event));
    $(`[data-vault-renew-recovery]`)?.addEventListener("click", () => void renewRecovery());
    $(`[data-vault-export]`)?.addEventListener("click", async () => {
      try { const payload = await state.client.exportData(); downloadJson(payload.export || {}, payload.filename || "forkmesh-vault.json"); }
      catch (error) { $(`[data-vault-settings-dialog]`).close(); message("error", explain(error)); }
    });
    $(`[data-vault-destroy]`)?.addEventListener("click", () => void destroyVault());
    $(`[data-vault-autolock]`)?.addEventListener("change", (event) => { localStorage.setItem(AUTOLOCK_KEY, event.target.value); touch(); });
    $$(`[data-vault-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    $$(`[data-vault-message-close]`).forEach((button) => button.addEventListener("click", () => button.closest("[data-vault-error], [data-vault-notice]")?.classList.add("hidden")));
  }
  return init();
});

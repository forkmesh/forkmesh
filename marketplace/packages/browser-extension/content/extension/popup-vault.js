// Toolbar popup: the Password Vault section. Unlock with the master password,
// fill the login saved for the site in the current tab, copy a username or
// password, save the login typed into the page, or generate a password.
//
// The popup holds no keys: every action is a message to the background
// worker (vault.js), which answers with titles and usernames only. A password
// reaches this script for exactly one thing, an explicit Copy.
(() => {
  const $ = (selector) => document.querySelector(selector);
  const CLIPBOARD_CLEAR_MS = 30000;
  let send = null;
  let tabUrl = "";

  const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[char]));
  const WORDS = {
    wrong_password: "That is not the master password.",
    vault_locked: "The vault locked itself. Unlock it again.",
    site_mismatch: "That login belongs to a different site, so it was not filled here.",
    no_login_on_page: "No filled-in password field was found on this page.",
    page_not_secure: "Logins are only saved from https pages.",
    unsafe_kdf_parameters: "The server returned key-stretching settings weaker than the vault allows. Nothing was unlocked.",
    keyset_mismatch: "The stored keys do not match this account's published keys. Nothing was unlocked.",
    read_only: "This login was shared with you to view, not to edit.",
    forkmesh_unreachable: "Could not reach the ForkMesh server.",
  };
  const explain = (result) => WORDS[result?.error] || result?.message || "Something went wrong.";
  function say(kind, text) {
    const ok = $("[data-vault-ok]"), error = $("[data-vault-error]");
    ok.hidden = kind !== "ok" || !text; error.hidden = kind !== "error" || !text;
    (kind === "ok" ? ok : error).textContent = text || "";
  }
  function show(state) {
    for (const node of document.querySelectorAll("[data-vault-state]")) node.hidden = node.dataset.vaultState !== state;
  }

  function row(item, fillable) {
    const where = item.owned ? "" : ` · from ${escapeHtml(item.owner)}`;
    return `<li data-vault-id="${escapeHtml(item.id)}"><span class="vault-name"><b>${escapeHtml(item.title)}</b><span class="muted">${escapeHtml(item.username || item.host)}${where}</span></span><span class="actions">`
      + (fillable ? `<button class="primary" type="button" data-vault-do="fill">Fill</button>` : "")
      + `<button class="secondary" type="button" data-vault-do="username" title="Copy username">User</button>`
      + `<button class="secondary" type="button" data-vault-do="password" title="Copy password">Pass</button></span></li>`;
  }
  async function renderItems(refresh = false) {
    const query = $("[data-vault-search]").value;
    const result = await send({ type: "fm-vault-items", url: tabUrl, query, refresh });
    if (!result.ok) { if (result.error === "vault_locked") return render(); say("error", explain(result)); return; }
    $("[data-vault-site]").textContent = result.host ? `Logins for ${result.host}` : "This page cannot be filled (not an https site).";
    $("[data-vault-matches]").innerHTML = result.matches.map((item) => row(item, true)).join("")
      || (result.host ? `<li class="muted">Nothing saved for this site.</li>` : "");
    $("[data-vault-others]").innerHTML = result.others.map((item) => row(item, false)).join("");
    $("[data-vault-capture]").disabled = !result.host;
  }

  async function render() {
    const status = await send({ type: "fm-vault-status" });
    say("", "");
    if (!status.ok) { show("none"); say("error", explain(status)); return; }
    if (!status.installed) { show("none"); return; }
    if (!status.unlocked) { show("locked"); $("[data-vault-account]").value = `${status.account} (ForkMesh vault)`; return; }
    show("open");
    await renderItems();
  }

  async function unlock(event) {
    event.preventDefault();
    const field = $("[data-vault-password]"), button = $("[data-vault-unlock]");
    button.disabled = true; say("ok", "Unlocking. Stretching the master password takes a few seconds.");
    const result = await send({ type: "fm-vault-unlock", password: field.value });
    button.disabled = false; field.value = "";
    if (!result.ok) { say("error", explain(result)); field.focus(); return; }
    await render();
  }
  async function copy(value, label) {
    try { await navigator.clipboard.writeText(value); } catch { say("error", "Chrome did not allow copying."); return; }
    say("ok", `${label} copied. The clipboard is cleared in 30 seconds while this popup stays open.`);
    setTimeout(() => { navigator.clipboard.writeText("").catch(() => {}); }, CLIPBOARD_CLEAR_MS);
  }
  async function act(event) {
    const button = event.target.closest("[data-vault-do]"); if (!button) return;
    const id = button.closest("[data-vault-id]").dataset.vaultId, what = button.dataset.vaultDo;
    if (what === "fill") {
      const result = await send({ type: "fm-vault-fill", id });
      if (!result.ok) { say("error", explain(result)); return; }
      if (!result.filledPassword && !result.filledUsername) { say("error", "No login form was found on this page."); return; }
      window.close();
      return;
    }
    const result = await send({ type: "fm-vault-copy", id, field: what });
    if (!result.ok) { say("error", explain(result)); return; }
    await copy(result.value, what === "password" ? "Password" : "Username");
  }
  async function capture() {
    const result = await send({ type: "fm-vault-capture" });
    if (!result.ok) { say("error", explain(result)); return; }
    const form = $("[data-vault-save-form]");
    form.hidden = false;
    $("[data-vault-save-title]").value = result.login.title.slice(0, 200);
    $("[data-vault-save-url]").value = result.login.url;
    $("[data-vault-save-username]").value = result.login.username;
    $("[data-vault-save-password]").value = result.login.password;
    say("", "");
  }
  async function save(event) {
    event.preventDefault();
    const fields = {
      title: $("[data-vault-save-title]").value, url: $("[data-vault-save-url]").value,
      username: $("[data-vault-save-username]").value, password: $("[data-vault-save-password]").value, notes: "",
    };
    const result = await send({ type: "fm-vault-save", fields });
    if (!result.ok) { say("error", explain(result)); return; }
    event.target.reset(); event.target.hidden = true;
    say("ok", "Saved to your vault, encrypted.");
    await renderItems();
  }
  async function generate() {
    const result = await send({ type: "fm-vault-generate", length: 24 });
    if (result.ok) await copy(result.password, "A new 24-character password");
  }

  function bind(sender) {
    send = sender;
    $("[data-vault-unlock-form]").addEventListener("submit", (event) => void unlock(event));
    $("[data-vault-lists]").addEventListener("click", (event) => void act(event));
    $("[data-vault-search]").addEventListener("input", () => void renderItems());
    $("[data-vault-capture]").addEventListener("click", () => void capture());
    $("[data-vault-save-form]").addEventListener("submit", (event) => void save(event));
    $("[data-vault-save-cancel]").addEventListener("click", () => { const form = $("[data-vault-save-form]"); form.reset(); form.hidden = true; });
    $("[data-vault-generate]").addEventListener("click", () => void generate());
    $("[data-vault-lock]").addEventListener("click", async () => { await send({ type: "fm-vault-lock" }); await render(); });
    for (const node of document.querySelectorAll("[data-vault-open]")) node.addEventListener("click", () => void send({ type: "fm-open-vault" }));
  }

  globalThis.ForkMeshVaultPopup = {
    bind,
    async render(url) { tabUrl = String(url || ""); await render(); },
  };
})();

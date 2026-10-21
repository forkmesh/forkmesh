  // Settings -> Reactions, GIFs & tips. Shown to administrators (custom
  // reactions for the instance, the GIPHY key, tip tokens) and to
  // organization owners (their organization's custom reactions).
  const reactionSettingsUi = { scopes: [], scope: "", reactions: [], editing: "", loaded: false };

  async function reactionSettingsPost(path, body) {
    const response = await fetch(path, {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ ...body, sessionToken: state.session?.sessionToken || "" }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) throw new Error(String(data.error || `HTTP ${response.status}`));
    return data;
  }

  function reactionSettingsErrorText(error) {
    return ({
      invalid_slug: "Use lowercase letters, digits and dashes (up to 32).",
      invalid_label: "Give it a label.",
      image_required: "Choose an image.",
      image_too_large: "That image is too large.",
      image_dimensions: "Images can be at most 256×256.",
      unsupported_image: "Use SVG, PNG, GIF or WebP.",
      invalid_svg: "That SVG could not be read.",
      svg_doctype: "SVGs with a DOCTYPE or entities are not accepted.",
      svg_size: "The SVG needs a viewBox or a width and height.",
      animate_target: "That SVG animates something other than shapes or colours.",
      scope_full: "This scope already has the most custom reactions it can.",
      forbidden: "You can't manage this scope.",
      rate_limited: "Too many changes - wait a moment.",
      invalid_key: "That does not look like a GIPHY key.",
      invalid_token: "Check the mint address, symbol and decimals.",
    })[String(error?.message || "")] || "That didn't save. Try again.";
  }

  function readReactionImage(file) {
    return new Promise((resolve, reject) => {
      if (!file) {
        resolve(null);
        return;
      }
      const reader = new FileReader();
      const svg = file.type === "image/svg+xml" || /\.svg$/i.test(file.name);
      reader.onload = () => resolve(svg ? { svg: String(reader.result || "") } : { data: String(reader.result || "") });
      reader.onerror = () => reject(new Error("invalid_image"));
      if (svg) reader.readAsText(file);
      else reader.readAsDataURL(file);
    });
  }

  function renderCustomReactionRows() {
    const list = $("[data-custom-reactions-list]");
    if (!list) return;
    if (!reactionSettingsUi.reactions.length) {
      list.innerHTML = '<li class="px-4 py-3 text-sm text-muted-foreground">No custom reactions yet.</li>';
      return;
    }
    list.innerHTML = reactionSettingsUi.reactions.map((item) => `<li class="flex flex-wrap items-center gap-3 px-4 py-2.5">
        <img src="${escapeHtml(commentReactionSafeSrc(item.src))}" alt="" width="28" height="28" class="h-7 w-7 object-contain">
        <span class="min-w-0 flex-1"><span class="block truncate text-sm font-medium text-foreground">${escapeHtml(item.label)}</span><span class="block truncate font-mono text-xs text-muted-foreground">:${escapeHtml(item.slug)}:${item.animatedSrc ? " · animated" : ""} · order ${escapeHtml(String(item.position))}</span></span>
        <label class="flex items-center gap-1.5 text-xs text-muted-foreground"><input type="checkbox" data-custom-reaction-toggle="${escapeHtml(item.slug)}" ${item.enabled ? "checked" : ""}> Enabled</label>
        <button type="button" data-custom-reaction-edit="${escapeHtml(item.slug)}" class="fm-btn fm-btn-sm">Edit</button>
        <button type="button" data-custom-reaction-delete="${escapeHtml(item.slug)}" class="fm-btn fm-btn-sm fm-btn-danger" aria-label="Delete ${escapeHtml(item.label)}">Delete</button>
      </li>`).join("");
  }

  async function loadCustomReactionScope(scope) {
    reactionSettingsUi.scope = scope;
    const list = $("[data-custom-reactions-list]");
    if (list) list.innerHTML = '<li class="px-4 py-3 text-sm text-muted-foreground">Loading…</li>';
    try {
      const data = await fetchJson(`/api/reactions/custom/manage?scope=${encodeURIComponent(scope)}`, { fresh: true });
      reactionSettingsUi.reactions = Array.isArray(data?.reactions) ? data.reactions : [];
    } catch (_) {
      reactionSettingsUi.reactions = [];
    }
    renderCustomReactionRows();
  }

  function resetCustomReactionForm() {
    const form = $("[data-custom-reaction-form]");
    if (!form) return;
    form.reset();
    reactionSettingsUi.editing = "";
    form.querySelector("[data-custom-reaction-slug]").readOnly = false;
    form.querySelector("[data-custom-reaction-form-title]").textContent = "Add a reaction";
    form.querySelector("[data-custom-reaction-preview]").replaceChildren();
  }

  async function initReactionSettings() {
    const section = $("[data-reaction-settings]");
    if (!section || reactionSettingsUi.loaded || !state.session?.sessionToken) return;
    reactionSettingsUi.loaded = true;
    let data = null;
    try {
      data = await fetchJson("/api/reactions/custom/manage", { fresh: true });
    } catch (_) {
      data = null;
    }
    reactionSettingsUi.scopes = Array.isArray(data?.scopes) ? data.scopes : [];
    const admin = reactionSettingsUi.scopes.some((scope) => scope.scope === "instance");
    $$("[data-reaction-settings-link]").forEach((link) => { link.hidden = !reactionSettingsUi.scopes.length; });
    if (!reactionSettingsUi.scopes.length) return;
    const select = section.querySelector("[data-custom-reactions-scope]");
    select.innerHTML = reactionSettingsUi.scopes.map((scope) => `<option value="${escapeHtml(scope.scope)}">${escapeHtml(scope.label)}</option>`).join("");
    void loadCustomReactionScope(reactionSettingsUi.scopes[0].scope);
    select.addEventListener("change", () => {
      resetCustomReactionForm();
      void loadCustomReactionScope(select.value);
    });
    const form = section.querySelector("[data-custom-reaction-form]");
    const status = form.querySelector("[data-custom-reaction-status]");
    form.querySelector("[data-custom-reaction-image]").addEventListener("change", (event) => {
      const file = event.target.files?.[0];
      const preview = form.querySelector("[data-custom-reaction-preview]");
      preview.replaceChildren();
      if (!file) return;
      const image = document.createElement("img");
      image.width = 32;
      image.height = 32;
      image.alt = "";
      image.src = URL.createObjectURL(file);
      preview.append(image);
    });
    form.querySelector("[data-custom-reaction-reset]").addEventListener("click", resetCustomReactionForm);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      status.textContent = "Saving…";
      try {
        const image = await readReactionImage(form.querySelector("[data-custom-reaction-image]").files?.[0]);
        const animated = await readReactionImage(form.querySelector("[data-custom-reaction-animated]").files?.[0]);
        const body = {
          scope: reactionSettingsUi.scope,
          action: "save",
          slug: form.querySelector("[data-custom-reaction-slug]").value.trim().toLowerCase(),
          label: form.querySelector("[data-custom-reaction-label]").value.trim(),
          position: Number(form.querySelector("[data-custom-reaction-position]").value) || 0,
          enabled: form.querySelector("[data-custom-reaction-enabled]").checked,
        };
        if (image) body.image = image;
        if (animated) body.animated = animated;
        await reactionSettingsPost("/api/reactions/custom/manage", body);
        status.textContent = "Saved.";
        resetCustomReactionForm();
        state.customReactions = null;
        await loadCustomReactionScope(reactionSettingsUi.scope);
      } catch (error) {
        status.textContent = reactionSettingsErrorText(error);
      }
    });
    section.querySelector("[data-custom-reactions-list]").addEventListener("click", async (event) => {
      const edit = event.target.closest("[data-custom-reaction-edit]");
      const remove = event.target.closest("[data-custom-reaction-delete]");
      if (edit) {
        const item = reactionSettingsUi.reactions.find((row) => row.slug === edit.getAttribute("data-custom-reaction-edit"));
        if (!item) return;
        reactionSettingsUi.editing = item.slug;
        form.querySelector("[data-custom-reaction-form-title]").textContent = `Edit :${item.slug}:`;
        form.querySelector("[data-custom-reaction-slug]").value = item.slug;
        form.querySelector("[data-custom-reaction-slug]").readOnly = true;
        form.querySelector("[data-custom-reaction-label]").value = item.label;
        form.querySelector("[data-custom-reaction-position]").value = String(item.position || 0);
        form.querySelector("[data-custom-reaction-enabled]").checked = item.enabled !== false;
        form.querySelector("[data-custom-reaction-label]").focus();
        return;
      }
      if (remove) {
        const slug = remove.getAttribute("data-custom-reaction-delete");
        if (!window.confirm(`Delete :${slug}:? Reactions already made with it stay, shown by name.`)) return;
        try {
          await reactionSettingsPost("/api/reactions/custom/manage", { scope: reactionSettingsUi.scope, action: "delete", slug });
          await loadCustomReactionScope(reactionSettingsUi.scope);
        } catch (error) {
          status.textContent = reactionSettingsErrorText(error);
        }
      }
    });
    section.querySelector("[data-custom-reactions-list]").addEventListener("change", async (event) => {
      const toggle = event.target.closest("[data-custom-reaction-toggle]");
      if (!toggle) return;
      try {
        await reactionSettingsPost("/api/reactions/custom/manage", { scope: reactionSettingsUi.scope, action: "save", slug: toggle.getAttribute("data-custom-reaction-toggle"), enabled: toggle.checked });
      } catch (error) {
        toggle.checked = !toggle.checked;
        status.textContent = reactionSettingsErrorText(error);
      }
    });
    if (admin) initInstanceCommentSettings(section);
  }

  async function initInstanceCommentSettings(section) {
    const gifCard = section.querySelector("[data-gif-settings-card]");
    const tipCard = section.querySelector("[data-tip-settings-card]");
    gifCard.hidden = false;
    tipCard.hidden = false;
    const gifForm = gifCard.querySelector("[data-gif-settings-form]");
    const gifStatus = gifCard.querySelector("[data-gif-settings-status]");
    try {
      const gifs = await fetchJson("/api/gifs/settings", { fresh: true });
      gifCard.querySelector("[data-gif-settings-rating]").value = gifs?.rating || "pg";
      gifStatus.textContent = gifs?.configured ? "A key is set." : "No key: the GIF button is hidden.";
    } catch (_) {
      gifStatus.textContent = "";
    }
    const saveGifs = async (body) => {
      gifStatus.textContent = "Saving…";
      try {
        const reply = await reactionSettingsPost("/api/gifs/settings", body);
        gifCard.querySelector("[data-gif-settings-key]").value = "";
        gifStatus.textContent = reply.configured ? "Saved. A key is set." : "Saved. No key: the GIF button is hidden.";
      } catch (error) {
        gifStatus.textContent = reactionSettingsErrorText(error);
      }
    };
    gifForm.addEventListener("submit", (event) => {
      event.preventDefault();
      void saveGifs({ key: gifCard.querySelector("[data-gif-settings-key]").value.trim(), rating: gifCard.querySelector("[data-gif-settings-rating]").value });
    });
    gifCard.querySelector("[data-gif-settings-clear]").addEventListener("click", () => {
      void saveGifs({ clear: true, rating: gifCard.querySelector("[data-gif-settings-rating]").value });
    });
    const tipForm = tipCard.querySelector("[data-tip-settings-form]");
    const tipStatus = tipCard.querySelector("[data-tip-settings-status]");
    try {
      const tips = await fetchJson("/api/tips/settings", { fresh: true });
      tipCard.querySelector("[data-tip-settings-enabled]").checked = tips?.enabled !== false;
      tipCard.querySelector("[data-tip-settings-usdc]").checked = tips?.usdc !== false;
      tipCard.querySelector("[data-tip-settings-mint]").value = tips?.token?.mint || "";
      tipCard.querySelector("[data-tip-settings-symbol]").value = tips?.token?.symbol || "";
      tipCard.querySelector("[data-tip-settings-decimals]").value = tips?.token ? String(tips.token.decimals) : "";
    } catch (_) {
      tipStatus.textContent = "";
    }
    tipForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      tipStatus.textContent = "Saving…";
      const mint = tipCard.querySelector("[data-tip-settings-mint]").value.trim();
      try {
        await reactionSettingsPost("/api/tips/settings", {
          enabled: tipCard.querySelector("[data-tip-settings-enabled]").checked,
          usdc: tipCard.querySelector("[data-tip-settings-usdc]").checked,
          token: mint ? { mint, symbol: tipCard.querySelector("[data-tip-settings-symbol]").value.trim(), decimals: Number(tipCard.querySelector("[data-tip-settings-decimals]").value) } : null,
        });
        tipStatus.textContent = "Saved.";
      } catch (error) {
        tipStatus.textContent = reactionSettingsErrorText(error);
      }
    });
  }

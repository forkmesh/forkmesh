  const pluginMenuState = { plugin: null, account: null, loading: null };

  function currentPluginPage() {
    const id = String(document.body?.dataset?.pluginPackage || "");
    if (!/^[a-z0-9][a-z0-9._-]{0,79}$/.test(id)) return null;
    return { id, name: String(document.body.dataset.pluginName || id) };
  }

  function pluginReviewUrl(id) {
    return `/market-reviews?market=forkmesh&package=${encodeURIComponent(id)}`;
  }

  function pluginShareUrl(id) {
    return `https://forkmesh.com/market/${encodeURIComponent(id)}`;
  }

  async function copyPluginLink(plugin) {
    const button = $("[data-plugin-menu-copy-link]");
    const copied = await copyTextToClipboard(pluginShareUrl(plugin.id));
    if (!button) return;
    button.textContent = copied ? "Link copied" : "Copy failed";
    window.setTimeout(() => {
      button.textContent = "Copy link";
      setPluginMenuOpen(false);
    }, 1200);
  }

  async function sharePlugin(plugin) {
    setPluginMenuOpen(false);
    const url = pluginShareUrl(plugin.id);
    try {
      await navigator.share({ title: plugin.name, text: `${plugin.name} on the ForkMesh Market`, url });
    } catch (error) {
      if (error?.name === "AbortError") return;
      if (!(await copyTextToClipboard(url))) showErrorNotice(`The link to ${plugin.name} could not be shared or copied.`);
    }
  }

  function setPluginMenuOpen(open) {
    const dropdown = $("[data-plugin-menu-dropdown]");
    if (!dropdown) return;
    dropdown.classList.toggle("hidden", !open);
    $("[data-plugin-menu-toggle]")?.setAttribute("aria-expanded", open ? "true" : "false");
    if (open) void refreshPluginMenuUninstall();
  }

  async function loadPluginAccountState() {
    if (pluginMenuState.account) return pluginMenuState.account;
    if (!pluginMenuState.loading) {
      const headers = { accept: "application/json" };
      const token = state.session?.sessionToken || "";
      if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
      pluginMenuState.loading = fetch("/api/plugins/@account", { headers, cache: "no-store", credentials: "same-origin" })
        .then(async (response) => {
          const payload = await response.json().catch(() => ({}));
          if (!response.ok || payload.ok === false) return { signedOut: response.status === 401, packages: [] };
          return { signedOut: false, packages: Array.isArray(payload.packages) ? payload.packages : [] };
        })
        .catch(() => ({ signedOut: false, packages: [], failed: true }))
        .then((result) => {
          pluginMenuState.loading = null;
          if (!result.failed) pluginMenuState.account = result;
          return result;
        });
    }
    return pluginMenuState.loading;
  }

  function pluginUninstallAvailability(plugin, account) {
    if (!plugin) return { enabled: false, reason: "" };
    if (!account) return { enabled: false, reason: "Checking…" };
    if (account.signedOut) return { enabled: false, reason: "Sign in to uninstall plugins." };
    const row = account.packages.find((item) => String(item?.id || "") === plugin.id);
    if (!row) return { enabled: false, reason: "Built in or installed by your organization. Manage it from Apps." };
    if (!row.installed) return { enabled: false, reason: "Not installed on your account." };
    return { enabled: true, reason: "" };
  }

  function renderPluginMenuUninstall(availability) {
    const button = $("[data-plugin-menu-uninstall]");
    if (!button) return;
    button.disabled = !availability.enabled;
    button.setAttribute("aria-disabled", availability.enabled ? "false" : "true");
    button.title = availability.reason;
    button.classList.toggle("opacity-50", !availability.enabled);
    const hint = $("[data-plugin-menu-uninstall-hint]");
    if (hint) {
      hint.textContent = availability.reason;
      hint.hidden = !availability.reason;
    }
  }

  async function refreshPluginMenuUninstall() {
    const plugin = pluginMenuState.plugin;
    if (!plugin) return;
    if (!pluginMenuState.account) renderPluginMenuUninstall(pluginUninstallAvailability(plugin, null));
    renderPluginMenuUninstall(pluginUninstallAvailability(plugin, await loadPluginAccountState()));
  }

  async function uninstallCurrentPlugin() {
    const plugin = pluginMenuState.plugin;
    if (!plugin) return;
    const availability = pluginUninstallAvailability(plugin, pluginMenuState.account);
    if (!availability.enabled) return;
    setPluginMenuOpen(false);
    if (!window.confirm(`Uninstall ${plugin.name}? Its links and screens disappear from the web, the desktop app and the mobile app on your account, and anything it stored for your account is deleted. Plugins that need it are removed too.`)) return;
    const headers = { accept: "application/json", "content-type": "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    try {
      const response = await fetch(`/api/plugins/@account/${encodeURIComponent(plugin.id)}/uninstall`, {
        method: "POST",
        headers,
        cache: "no-store",
        credentials: "same-origin",
        body: "{}",
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok || payload.ok === false) throw new Error(payload.error || `HTTP ${response.status}`);
      const account = String(state.session?.nodeName || "");
      const changed = Array.isArray(payload.changed) && payload.changed.length ? payload.changed.map(String) : [plugin.id];
      for (const id of changed) window.ForkMeshAPI?.rememberPlugin?.(account, id, false);
      void window.ForkMeshAPI?.refreshPlugins?.(true);
      window.dispatchEvent(new CustomEvent("forkmesh:plugins-changed", { detail: { changed } }));
      location.assign("/dashboard/apps");
    } catch (error) {
      showErrorNotice(`${plugin.name} could not be uninstalled: ${error.message}`);
    }
  }

  function initPluginMenu() {
    const root = $("[data-plugin-menu]");
    const plugin = currentPluginPage();
    if (!root || !plugin) return;
    pluginMenuState.plugin = plugin;
    root.classList.remove("hidden");
    root.classList.add("flex");
    const toggle = $("[data-plugin-menu-toggle]");
    if (toggle) {
      toggle.setAttribute("aria-label", `${plugin.name} options`);
      toggle.title = `${plugin.name} options`;
    }
    const rate = $("[data-plugin-menu-rate]");
    if (rate) {
      rate.href = pluginReviewUrl(plugin.id);
      rate.textContent = `Rate ${plugin.name}`;
    }
    const share = $("[data-plugin-menu-share]");
    if (share) share.hidden = typeof navigator.share !== "function";
    const uninstall = $("[data-plugin-menu-uninstall-label]");
    if (uninstall) uninstall.textContent = `Uninstall ${plugin.name}…`;
    document.addEventListener("click", (event) => {
      if (event.target.closest("[data-plugin-menu-toggle]")) {
        event.preventDefault();
        setAccountMenuOpen(false);
        setPluginMenuOpen($("[data-plugin-menu-dropdown]")?.classList.contains("hidden"));
        return;
      }
      if (event.target.closest("[data-plugin-menu-feedback]")) {
        event.preventDefault();
        setPluginMenuOpen(false);
        setFeedbackModalOpen(true, plugin);
        return;
      }
      if (event.target.closest("[data-plugin-menu-share]")) {
        event.preventDefault();
        void sharePlugin(plugin);
        return;
      }
      if (event.target.closest("[data-plugin-menu-copy-link]")) {
        event.preventDefault();
        void copyPluginLink(plugin);
        return;
      }
      if (event.target.closest("[data-plugin-menu-uninstall]")) {
        event.preventDefault();
        void uninstallCurrentPlugin();
        return;
      }
      if (!event.target.closest("[data-plugin-menu]")) setPluginMenuOpen(false);
    });
    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape" || $("[data-plugin-menu-dropdown]")?.classList.contains("hidden")) return;
      setPluginMenuOpen(false);
      toggle?.focus();
    });
  }

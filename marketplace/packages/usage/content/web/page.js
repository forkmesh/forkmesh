const usagePageResetClock = (resetAtMs, nowMs = Date.now(), timeZone = undefined) => {
  const reset = Number(resetAtMs) || 0;
  if (!reset || reset <= nowMs) return "";
  const zone = timeZone ? { timeZone } : {};
  const format = (options, ms) => new Intl.DateTimeFormat("en-US", { ...options, ...zone }).format(ms).replace(/\s/g, " ");
  const dayOf = (ms) => Date.parse(new Intl.DateTimeFormat("en-CA", { year: "numeric", month: "2-digit", day: "2-digit", ...zone }).format(ms));
  const clock = format({ hour: "numeric", minute: "2-digit", hour12: true }, reset);
  const days = Math.round((dayOf(reset) - dayOf(nowMs)) / 86400000);
  if (days === 0) return `Resets ${clock}`;
  if (days < 7) return `Resets ${format({ weekday: "short" }, reset)} ${clock}`;
  return `Resets ${format({ month: "short", day: "numeric" }, reset)}, ${clock}`;
};

window.ForkMeshDashboard.registerPage("usage", ({ select: $, desktopUsage, escapeHtml, relativeTimeLabel }) => {
  const state = { loading: false, desktops: [], observedAt: 0, error: "" };
  const percent = (value) => Math.max(0, Math.min(100, Number(value) || 0));
  const usageWindows = (provider, account) => {
    const own = Array.isArray(account?.windows) ? account.windows : [];
    return own.length || !account?.active ? own : (Array.isArray(provider?.windows) ? provider.windows : []);
  };
  const meter = (window) => {
    const used = percent(window?.usedPercent);
    const label = escapeHtml(String(window?.label || window?.id || "Usage window"));
    const reset = Number(window?.resetAtMs) || 0;
    const resetLabel = reset > Date.now() ? escapeHtml(usagePageResetClock(reset)) : reset ? "Ready" : "Reset time unavailable";
    return `<div class="grid gap-1.5"><div class="flex items-baseline justify-between gap-3 text-xs"><span class="min-w-0 truncate text-foreground">${label}</span><span class="shrink-0 font-mono text-muted-foreground">${used.toFixed(0)}%</span></div><div class="h-1.5 overflow-hidden rounded-full bg-secondary" role="progressbar" aria-label="${label}" aria-valuenow="${used}" aria-valuemin="0" aria-valuemax="100"><span class="block h-full bg-primary" style="width:${used}%"></span></div><span class="text-[11px] text-muted-foreground">${resetLabel}</span></div>`;
  };
  const account = (provider, item) => {
    const name = escapeHtml(String(item?.label || item?.identity || "Account"));
    const stateLabel = item?.signedIn === false ? "Not signed in" : item?.active ? "In use" : "Available";
    const windows = usageWindows(provider, item);
    return `<div class="grid gap-3 border-t border-border px-4 py-3 sm:px-5"><div class="flex flex-wrap items-center justify-between gap-2"><span class="text-sm text-foreground">${name}</span><span class="fm-label">${stateLabel}</span></div>${windows.length ? `<div class="grid gap-3">${windows.map(meter).join("")}</div>` : '<p class="text-xs text-muted-foreground">No current usage reading. Run the account on this desktop, then refresh.</p>'}</div>`;
  };
  const provider = (item) => {
    const label = escapeHtml(String(item?.label || item?.id || "Model provider"));
    const accounts = Array.isArray(item?.accounts) ? item.accounts : [];
    const fallback = !accounts.length && Array.isArray(item?.windows) && item.windows.length
      ? `<div class="grid gap-3 border-t border-border px-4 py-3 sm:px-5">${item.windows.map(meter).join("")}</div>` : "";
    return `<section><header class="flex items-center gap-2 px-4 py-3 sm:px-5"><i data-lucide="bot" class="h-4 w-4 text-muted-foreground"></i><h2 class="text-sm font-semibold text-foreground">${label}</h2></header>${accounts.map((entry) => account(item, entry)).join("")}${fallback}</section>`;
  };
  function render() {
    const box = $("[data-usage-desktops]");
    const notice = $("[data-usage-notice]");
    if (!box || !notice) return;
    notice.textContent = state.error || (state.observedAt ? `Updated ${relativeTimeLabel(state.observedAt)}.` : "");
    notice.className = `mb-4 text-sm ${state.error ? "text-destructive" : "text-muted-foreground"}`;
    if (state.loading && !state.desktops.length) {
      box.innerHTML = '<div class="px-4 py-8 text-sm text-muted-foreground"><span class="fm-spinner"></span>Reading connected desktops…</div>';
    } else if (!state.desktops.length) {
      box.innerHTML = '<div class="px-4 py-10 text-center"><i data-lucide="monitor-off" class="mx-auto mb-3 h-6 w-6 text-muted-foreground"></i><p class="text-sm font-medium text-foreground">No connected desktops</p><p class="mt-1 text-sm text-muted-foreground">Open ForkMesh Desktop and sign in to see your model usage.</p></div>';
    } else {
      box.innerHTML = state.desktops.map((desktop, index) => {
        const name = escapeHtml(String(desktop?.name || desktop?.node || "Desktop"));
        const providers = Array.isArray(desktop?.usage?.providers) ? desktop.usage.providers : [];
        const content = desktop?.error ? `<p class="border-t border-border px-4 py-3 text-sm text-destructive sm:px-5">${escapeHtml(desktop.error)}</p>`
          : providers.length ? providers.map(provider).join("")
            : '<p class="border-t border-border px-4 py-5 text-sm text-muted-foreground sm:px-5">This desktop has no Claude Code or Codex account usage yet.</p>';
        return `<section${index ? ' class="border-t border-border"' : ""}><header class="flex items-center gap-2 bg-muted/30 px-4 py-3 sm:px-5"><i data-lucide="monitor" class="h-4 w-4 text-muted-foreground"></i><h2 class="text-sm font-semibold text-foreground">${name}</h2></header>${content}</section>`;
      }).join("");
    }
    const refreshButton = $("[data-usage-refresh]");
    if (refreshButton) {
      refreshButton.disabled = state.loading;
      refreshButton.setAttribute("aria-busy", state.loading ? "true" : "false");
    }
    window.lucide?.createIcons?.();
  }
  async function load(refresh = false) {
    state.loading = true;
    state.error = "";
    render();
    try {
      const result = await desktopUsage({ refresh });
      state.desktops = Array.isArray(result?.desktops) ? result.desktops : [];
      state.observedAt = Number(result?.observedAt) || Date.now();
    } catch (error) {
      state.error = error?.message === "desktop_sign_in_required" ? "Sign in to read model usage from your desktops." : "Could not read model usage. Check that a desktop is online and try again.";
    } finally {
      state.loading = false;
      render();
    }
  }
  $("[data-usage-refresh]")?.addEventListener("click", () => { void load(true); });
  void load();
});

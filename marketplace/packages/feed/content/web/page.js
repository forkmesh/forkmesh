window.ForkMeshDashboard.registerPage("feed", ({ select: $, escapeHtml, relativeTimeLabel }) => {
  const FEED_URLS = [
    "https://world.forkmesh.com/api/world/profile-feed",
    "https://world.forkmesh.com/api/world/fediverse-mentions",
  ];
  const state = { loading: false, items: [], error: "", cachedAt: 0 };
  const safeUrl = (value) => {
    try { const url = new URL(String(value || "")); return url.protocol === "https:" ? url.href : ""; } catch (_) { return ""; }
  };
  const kindLabel = (item) => String(item?.kind || "mention").replace(/[-_]/g, " ");
  function card(item) {
    const name = escapeHtml(String(item?.authorName || item?.author || "Fediverse member"));
    const handle = String(item?.author || "").trim();
    const byline = handle && handle !== item?.authorName ? ` <span class="text-muted-foreground">@${escapeHtml(handle)}</span>` : "";
    const excerpt = escapeHtml(String(item?.excerpt || "")).replace(/\n/g, "<br>");
    const published = Number(item?.published) || 0;
    const remoteUrl = safeUrl(item?.remoteUrl);
    const issueUrl = safeUrl(item?.issueUrl);
    const repo = String(item?.repository || "").trim();
    const actions = [remoteUrl ? `<a class="text-xs text-accent hover:underline" href="${escapeHtml(remoteUrl)}" target="_blank" rel="noopener noreferrer">Open post</a>` : "", issueUrl ? `<a class="text-xs text-accent hover:underline" href="${escapeHtml(issueUrl)}" target="_blank" rel="noopener noreferrer">Open work item</a>` : ""].filter(Boolean).join('<span class="text-muted-foreground">·</span>');
    return `<article class="px-4 py-4 sm:px-5"><div class="flex flex-wrap items-baseline gap-x-2 gap-y-1"><strong class="text-sm text-foreground">${name}</strong>${byline}<span class="fm-label">${escapeHtml(kindLabel(item))}</span><time class="ml-auto text-xs text-muted-foreground" title="${published ? new Date(published).toLocaleString() : ""}">${published ? escapeHtml(relativeTimeLabel(published)) : ""}</time></div>${repo ? `<p class="mt-2 font-mono text-[11px] text-muted-foreground">${escapeHtml(repo)}</p>` : ""}<p class="mt-2 whitespace-pre-wrap text-sm leading-6 text-foreground">${excerpt || "Public ActivityPub activity"}</p>${actions ? `<footer class="mt-3 flex flex-wrap items-center gap-2">${actions}</footer>` : ""}</article>`;
  }
  function render() {
    const list = $("[data-feed-list]");
    const notice = $("[data-feed-notice]");
    if (!list || !notice) return;
    notice.textContent = state.error || (state.cachedAt ? `Updated ${relativeTimeLabel(state.cachedAt)}.` : "");
    notice.className = `mb-4 text-sm ${state.error ? "text-destructive" : "text-muted-foreground"}`;
    if (state.loading && !state.items.length) list.innerHTML = '<div class="px-4 py-8 text-sm text-muted-foreground"><span class="fm-spinner"></span>Loading the network feed…</div>';
    else if (!state.items.length) list.innerHTML = '<div class="px-4 py-10 text-center"><i data-lucide="messages-square" class="mx-auto mb-3 h-6 w-6 text-muted-foreground"></i><p class="text-sm font-medium text-foreground">No public activity yet</p><p class="mt-1 text-sm text-muted-foreground">Verified public replies and mentions will appear here.</p></div>';
    else list.innerHTML = state.items.map((item, index) => `<div${index ? ' class="border-t border-border"' : ""}>${card(item)}</div>`).join("");
    $("[data-feed-refresh]").disabled = state.loading;
    window.lucide?.createIcons?.();
  }
  async function load() {
    state.loading = true;
    state.error = "";
    render();
    try {
      const results = await Promise.allSettled(FEED_URLS.map(async (url) => {
        const response = await fetch(url, { headers: { accept: "application/json" }, cache: "no-store", credentials: "omit" });
        if (!response.ok) throw new Error("feed_unavailable");
        return response.json().catch(() => ({}));
      }));
      const payloads = results.filter((result) => result.status === "fulfilled").map((result) => result.value);
      if (!payloads.length) throw new Error("feed_unavailable");
      state.items = payloads.flatMap((payload) => Array.isArray(payload?.items) ? payload.items : [])
        .sort((a, b) => (Number(b?.published) || 0) - (Number(a?.published) || 0));
      state.cachedAt = Date.now();
    } catch (_) {
      state.error = "The network feed is unavailable. Try again in a moment.";
    } finally {
      state.loading = false;
      render();
    }
  }
  $("[data-feed-refresh]")?.addEventListener("click", () => { void load(); });
  void load();
});

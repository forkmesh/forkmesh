
  function repoReadSurfaceBase(repo) {
    const owner = String(repoPublicOwner(repo) || repo?.owner || "").trim().toLowerCase();
    const name = String(repo?.name || "").trim();
    if (!owner || !name) return "";
    return ["", owner, name].map(encodeURIComponent).join("/");
  }

  function repoIssueFeedLinks(repo) {
    if (!instancePluginOn("read-surfaces")) return "";
    const base = repoReadSurfaceBase(repo);
    if (!base) return "";
    const link = "text-[var(--dashboard-link)] hover:underline";
    return `<p data-repo-issue-feeds class="border-t border-border px-4 py-2 text-[12px] text-muted-foreground"><a href="${escapeHtml(`${base}/issues.atom`)}" type="application/atom+xml" class="${link}">Feed</a> &middot; <a href="${escapeHtml(`${base}/lite`)}" class="${link}">Lite view</a></p>`;
  }

  function liteViewHref() {
    const base = document.body?.dataset.page === "repo" ? repoReadSurfaceBase(state.selectedRepo) : "";
    if (!base) return "/lite";
    const issue = location.pathname.match(/\/issues\/([1-9]\d{0,8})\/?$/);
    return issue ? `${base}/lite/issues/${issue[1]}` : `${base}/lite`;
  }

  document.addEventListener("click", (event) => {
    const link = event.target instanceof Element ? event.target.closest("a[data-lite-link]") : null;
    if (link) link.setAttribute("href", liteViewHref());
  }, true);

  void instanceInfo().then(() => {
    const on = instancePluginOn("read-surfaces");
    document.querySelectorAll("a[data-lite-link]").forEach((link) => { link.hidden = !on; });
  }).catch(() => {});
  function repoMirrorHelpHref(repo) {
    const base = repoReadSurfaceBase(repo);
    return base ? `${base}/mirror` : "";
  }

  function repoMirrorHelpBanner(base, healthy, target) {
    return `<div data-repo-mirror-help class="flex min-w-0 items-center gap-2 rounded-t-md border-b border-attention/40 bg-attention/10 px-3 py-1.5 text-xs text-foreground"><span aria-hidden="true" class="h-2 w-2 shrink-0 rounded-full bg-attention"></span><span class="min-w-0 flex-1 truncate"><b class="font-semibold">This forum needs your help</b> &mdash; ${healthy} of ${target} mirrors running. <a href="${escapeHtml(`${base}/mirror`)}" class="dashboard-accent-link font-semibold">Run a mirror or join the federation &rarr;</a></span><button type="button" data-repo-mirror-help-dismiss aria-label="Dismiss" class="shrink-0 text-muted-foreground hover:text-foreground">&times;</button></div>`;
  }

  function repoMirrorHelpDismissKey(base) {
    return `forkmesh.mirrorHelpDismissed:${base}`;
  }

  function repoMirrorHelpDismissed(base) {
    try {
      return localStorage.getItem(repoMirrorHelpDismissKey(base)) === "1";
    } catch (_) {
      return false;
    }
  }

  function repoMirrorHelpFor(repo) {
    const base = repoReadSurfaceBase(repo);
    if (!base || repo?.isPrivate || repoMirrorHelpDismissed(base)) return "";
    const data = mirrorQuotaKnown(repo);
    const target = Math.max(0, Math.floor(Number(data?.target) || 0));
    const healthy = Math.max(0, Math.floor(Number(data?.healthy) || 0));
    if (!data || !target || healthy >= target) return "";
    return repoMirrorHelpBanner(base, healthy, target);
  }

  function repoMirrorHelpSlot(repo) {
    if (repo && !repo.isPrivate && !mirrorQuotaKnown(repo)) {
      void loadRepoMirrorQuota(repo).then(() => {
        const slot = document.querySelector("[data-repo-mirror-help-slot]");
        if (slot && mirrorQuotaCurrent(repo)) slot.innerHTML = repoMirrorHelpFor(repo);
      });
    }
    return `<div data-repo-mirror-help-slot>${repoMirrorHelpFor(repo)}</div>`;
  }

  document.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest("[data-repo-mirror-help-dismiss]") : null;
    if (!button) return;
    event.preventDefault();
    const base = repoReadSurfaceBase(state.selectedRepo);
    try {
      if (base) localStorage.setItem(repoMirrorHelpDismissKey(base), "1");
    } catch (_) {}
    button.closest("[data-repo-mirror-help]")?.remove();
  });

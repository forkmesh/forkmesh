
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

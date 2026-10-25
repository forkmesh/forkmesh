  const MIRROR_QUOTA_DOT = Object.freeze({
    green: "bg-success",
    yellow: "bg-attention",
    red: "bg-destructive",
    unknown: "bg-muted-foreground/40",
  });
  const MIRROR_QUOTA_WORD = Object.freeze({
    green: "Healthy",
    yellow: "Behind",
    red: "Unreachable",
    unknown: "Not checked",
  });
  const MIRROR_QUOTA_START_URL = "https://forkmesh.com/start";
  const mirrorQuotaUi = { data: new Map(), loads: new Map(), dialog: null };

  function mirrorQuotaRowHtml(row) {
    const status = MIRROR_QUOTA_DOT[row?.status] ? row.status : "unknown";
    const label = String(row?.label || row?.host || "");
    const url = String(row?.url || "");
    const href = /^https:\/\/[^\s"'<>]+$/.test(url) ? url : "";
    const word = MIRROR_QUOTA_WORD[status];
    const detail = String(row?.statusLabel || word);
    const at = Number(row?.lastCheck?.at) || 0;
    const checked = row?.id !== "self" && at > 0 ? ` · checked ${formatTimeAgo(at)}` : "";
    const name = href
      ? `<a href="${escapeHtml(href)}" target="_blank" rel="noopener noreferrer nofollow" class="dashboard-accent-link block min-w-0 truncate">${escapeHtml(label)}</a>`
      : `<span class="block min-w-0 truncate text-foreground" title="${escapeHtml(url)}">${escapeHtml(label)}</span>`;
    const host = row?.host && row.host !== label ? `${escapeHtml(row.host)} · ` : "";
    const remove = row?.canRemove && !row?.builtin
      ? `<button type="button" data-mirror-quota-remove="${escapeHtml(row.id)}" aria-label="Remove ${escapeHtml(label)}" class="shrink-0 text-[11px] text-muted-foreground hover:text-destructive">Remove</button>`
      : "";
    return `<li data-mirror-quota-row="${escapeHtml(row?.id || "")}" data-mirror-status="${status}" class="flex items-center gap-2 rounded-md px-2 py-1.5 text-xs"><span role="img" aria-label="${word}" title="${word}" class="h-2 w-2 shrink-0 rounded-full ${MIRROR_QUOTA_DOT[status]}"></span><span class="min-w-0 flex-1">${name}<span class="block truncate text-[11px] text-muted-foreground">${host}${escapeHtml(detail)}${escapeHtml(checked)}</span></span>${remove}</li>`;
  }

  const MIRROR_QUOTA_OVERALL_WORD = Object.freeze({
    green: "Mirror goal met",
    yellow: "At least half of the mirror goal",
    red: "Below half of the mirror goal",
    unknown: "No mirrors checked yet",
  });

  function mirrorQuotaOverall(healthy, target) {
    if (target <= 0) return healthy > 0 ? "green" : "unknown";
    if (healthy >= target) return "green";
    return healthy * 2 >= target ? "yellow" : "red";
  }

  function renderRepoMirrorQuotaCard(data, viewer = null, helpHref = "") {
    if (!data || typeof data !== "object" || !Array.isArray(data.mirrors)) return "";
    const target = Math.max(0, Number(data.target) || 0);
    const healthy = Math.max(0, Number(data.healthy) || 0);
    const registered = Math.max(0, Number(data.registered) || 0);
    const canSet = Boolean(viewer?.canSetTarget);
    if (!target && !registered && !canSet) return "";
    const count = target ? `${healthy} / ${target} healthy` : `${healthy} healthy`;
    const overall = mirrorQuotaOverall(healthy, target);
    const percent = target ? Math.min(100, Math.round((healthy * 100) / target)) : 0;
    const missing = Math.max(0, target - healthy);
    const help = helpHref && target && overall !== "green"
      ? `<p data-mirror-quota-help class="mt-1.5 text-[11px] text-muted-foreground">Needs ${missing} more mirror${missing === 1 ? "" : "s"} &middot; <a href="${escapeHtml(helpHref)}" class="dashboard-accent-link">How to help &rarr;</a></p>`
      : "";
    const bar = (target
      ? `<div role="progressbar" aria-label="Healthy mirrors" aria-valuemin="0" aria-valuemax="${target}" aria-valuenow="${Math.min(healthy, target)}" class="mt-2 h-1.5 w-full overflow-hidden rounded-full bg-muted-foreground/20"><span class="block h-full rounded-full ${MIRROR_QUOTA_DOT[overall]}" style="width:${percent}%"></span></div>`
      : "") + help;
    const rows = data.mirrors.map((row) => mirrorQuotaRowHtml(row)).join("");
    const goal = canSet
      ? `<form data-mirror-quota-target-form class="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-muted-foreground"><label class="inline-flex items-center gap-1.5">${target ? "Goal" : "Set a mirror goal"}<input name="target" type="number" inputmode="numeric" min="0" max="${Number(data.maxTarget) || 100}" value="${target || 10}" aria-label="Healthy mirrors wanted" class="h-control-sm w-14 rounded-md border border-border bg-background px-2 font-mono text-control text-foreground outline-none focus:border-primary" /></label><button type="submit" class="fm-btn fm-btn-sm">Set target</button><span data-mirror-quota-target-status role="status" class="min-w-0 truncate"></span></form>`
      : "";
    const summary = MIRROR_QUOTA_OVERALL_WORD[overall];
    return `<div data-mirror-quota-overall="${overall}" class="flex items-center justify-between gap-3"><h4 class="inline-flex items-center gap-1.5 text-xs font-semibold text-foreground"><span role="img" aria-label="${summary}" title="${summary}" class="h-2 w-2 shrink-0 rounded-full ${MIRROR_QUOTA_DOT[overall]}"></span>Mirrors</h4><span data-mirror-quota-count class="font-mono text-[11px] text-muted-foreground">${count}</span></div>${bar}<ul class="-mx-2 mt-2 grid">${rows}</ul><div class="mt-2 flex flex-wrap items-center gap-2"><button type="button" data-mirror-quota-add class="fm-btn fm-btn-sm">Add a mirror</button><span data-mirror-quota-status role="status" class="min-w-0 truncate text-[11px] text-destructive"></span></div>${goal}`;
  }

  function mirrorQuotaKey(repo) {
    return repo ? repoApiBase(repo).toLowerCase() : "";
  }

  function mirrorQuotaKnown(repo) {
    const key = mirrorQuotaKey(repo);
    if (!key) return null;
    if (mirrorQuotaUi.data.has(key)) return mirrorQuotaUi.data.get(key);
    const seeded = repoFirstViewSeed(repo)?.mirrorQuota;
    return seeded && typeof seeded === "object" && Array.isArray(seeded.mirrors) ? seeded : null;
  }

  function repoMirrorQuotaSection(repo) {
    if (!instancePluginOn("mirror-quota")) return "";
    const data = mirrorQuotaKnown(repo);
    const inner = renderRepoMirrorQuotaCard(data, data?.viewer || null, repoMirrorHelpHref(repo));
    return `<section data-repo-mirror-quota aria-label="Mirrors" class="mt-3 border-t border-border pt-3"${inner ? "" : " hidden"}>${inner}</section>`;
  }

  function paintRepoMirrorQuota(repo) {
    const section = document.querySelector("[data-repo-mirror-quota]");
    if (!section || !repo) return;
    const data = mirrorQuotaUi.data.get(mirrorQuotaKey(repo)) || null;
    const inner = renderRepoMirrorQuotaCard(data, data?.viewer || null, repoMirrorHelpHref(repo));
    section.innerHTML = inner;
    section.hidden = !inner;
  }

  function mirrorQuotaCurrent(repo) {
    const selected = state.selectedRepo;
    return Boolean(selected && repo && mirrorQuotaKey(selected) === mirrorQuotaKey(repo));
  }

  async function loadRepoMirrorQuota(repo) {
    const key = mirrorQuotaKey(repo);
    if (!key || repo.isPrivate || !instancePluginOn("mirror-quota")) return null;
    if (mirrorQuotaUi.loads.has(key)) return mirrorQuotaUi.loads.get(key);
    const signedIn = Boolean(state.session?.sessionToken);
    const load = (async () => {
      try {
        const data = await fetchJson(`${repoApiBase(repo)}/mirror-quota`, { fresh: signedIn });
        if (!data || data.ok === false || !Array.isArray(data.mirrors)) return null;
        mirrorQuotaUi.data.set(key, data);
        if (mirrorQuotaCurrent(repo)) paintRepoMirrorQuota(repo);
        return data;
      } catch (_) {
        return null;
      } finally {
        mirrorQuotaUi.loads.delete(key);
      }
    })();
    mirrorQuotaUi.loads.set(key, load);
    return load;
  }

  function mirrorQuotaErrorText(data, status) {
    if (data?.message) return String(data.message);
    if (status === 401) return "Sign in first.";
    if (status === 403) return "You can't change this repository's mirrors.";
    if (status === 429) return "Too many changes. Try again in a minute.";
    return String(data?.error || `HTTP ${status}`).replace(/_/g, " ");
  }

  async function sendRepoMirrorQuota(repo, method, body = {}) {
    const response = await fetch(`${repoApiBase(repo)}/mirror-quota`, {
      method,
      credentials: "same-origin",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ ...body, sessionToken: state.session?.sessionToken || "" }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) throw new Error(mirrorQuotaErrorText(data, response.status));
    mirrorQuotaUi.data.set(mirrorQuotaKey(repo), data);
    if (mirrorQuotaCurrent(repo)) paintRepoMirrorQuota(repo);
    return data;
  }

  function mirrorQuotaRunHtml(data, repo) {
    const clone = String(data?.cloneUrl || "") || `${publicSiteOrigin()}${repoLinkUrl(repo)}.git`;
    const folder = `${String(repo?.name || "repository").replace(/[^A-Za-z0-9._-]/g, "")}.git`;
    const commands = `git clone --mirror ${clone}\ncd ${folder} && git update-server-info`;
    const cron = `*/15 * * * * cd /srv/${folder} && git remote update --prune && git update-server-info`;
    const block = (text, label) => `<div class="mt-2 flex items-start gap-2"><pre class="min-w-0 flex-1 overflow-x-auto rounded-md border border-border bg-secondary px-2 py-1.5 font-mono text-[11px] leading-5 text-foreground">${escapeHtml(text)}</pre><button type="button" data-mirror-quota-copy="${escapeHtml(text)}" aria-label="${escapeHtml(label)}" class="fm-btn fm-btn-sm">Copy</button></div>`;
    return `<p class="text-xs leading-5 text-muted-foreground">Every copy keeps this project reachable if this site or the original is blocked or taken down. Issues live in <code class="font-mono">.forkmesh/issues</code>, so a clone carries the whole forum, not only the code.</p>${repoMirrorHelpHref(repo) ? `<p class="mt-2 text-xs"><a href="${escapeHtml(repoMirrorHelpHref(repo))}" class="dashboard-accent-link font-semibold">Read the full guide: run a mirror, join the federation, register it &rarr;</a></p>` : ""}<h3 class="mt-3 text-xs font-semibold text-foreground">Run a mirror</h3>${block(commands, "Copy the clone commands")}<p class="mt-2 text-[11px] text-muted-foreground">Keep it fresh every 15 minutes with cron:</p>${block(cron, "Copy the cron line")}<p class="mt-2 text-[11px] leading-5 text-muted-foreground">Serve the folder over HTTPS with any web server, or push it to another forge. Or <a href="${MIRROR_QUOTA_START_URL}" target="_blank" rel="noopener noreferrer" class="dashboard-accent-link">start your own ForkMesh instance</a>.</p>`;
  }

  function mirrorQuotaFormHtml() {
    if (!state.session?.sessionToken) {
      return `<p class="mt-3 text-xs text-muted-foreground"><a href="/login?next=${encodeURIComponent(location.pathname)}" class="dashboard-accent-link">Sign in</a> to list your mirror here so it is checked and counted.</p>`;
    }
    return `<form data-mirror-quota-add-form class="mt-3 grid gap-2"><h3 class="text-xs font-semibold text-foreground">List your mirror</h3><input name="url" type="text" inputmode="url" autocomplete="off" spellcheck="false" required maxlength="300" placeholder="https://example.org/example/forum.git" aria-label="Mirror address" class="h-control rounded-md border border-border bg-background px-2 font-mono text-control text-foreground outline-none focus:border-primary" /><input name="label" type="text" autocomplete="off" maxlength="60" placeholder="Name (optional)" aria-label="Mirror name" class="h-control rounded-md border border-border bg-background px-2 text-control text-foreground outline-none focus:border-primary" /><p class="text-[11px] text-muted-foreground">https addresses are checked every 15 minutes. git:// and ssh addresses are listed but not checked.</p><span data-mirror-quota-add-status role="status" class="min-h-4 text-[11px] text-muted-foreground"></span></form>`;
  }

  function closeMirrorQuotaDialog() {
    const dialog = mirrorQuotaUi.dialog;
    if (!dialog) return;
    mirrorQuotaUi.dialog = null;
    document.removeEventListener("keydown", dialog.onKey, true);
    dialog.overlay.remove();
    if (dialog.previous?.isConnected && typeof dialog.previous.focus === "function") dialog.previous.focus();
  }

  function openMirrorQuotaDialog(repo) {
    closeMirrorQuotaDialog();
    const data = mirrorQuotaKnown(repo);
    const overlay = document.createElement("div");
    overlay.dataset.mirrorQuotaDialog = "";
    overlay.className = "fixed inset-0 z-[90] flex items-start justify-center bg-background/70 p-4 pt-[10vh]";
    const signedIn = Boolean(state.session?.sessionToken);
    overlay.innerHTML = `<div role="dialog" aria-modal="true" aria-labelledby="mirrorQuotaDialogTitle" class="fm-dialog w-full max-w-lg"><div class="fm-dialog-header"><h2 id="mirrorQuotaDialogTitle" class="fm-dialog-title">Add a mirror</h2><button type="button" data-mirror-quota-close aria-label="Close" class="fm-btn fm-btn-sm fm-btn-icon fm-btn-invisible"><i data-lucide="x"></i></button></div><div class="fm-dialog-body">${mirrorQuotaRunHtml(data, repo)}${mirrorQuotaFormHtml()}</div><div class="fm-dialog-footer"><button type="button" data-mirror-quota-close class="fm-btn fm-btn-sm">${signedIn ? "Cancel" : "Close"}</button>${signedIn ? `<button type="button" data-mirror-quota-submit class="fm-btn fm-btn-sm">Add mirror</button>` : ""}</div></div>`;
    const onKey = (event) => {
      if (event.key !== "Escape") return;
      event.preventDefault();
      event.stopPropagation();
      closeMirrorQuotaDialog();
    };
    mirrorQuotaUi.dialog = { overlay, onKey, repo, previous: document.activeElement };
    document.addEventListener("keydown", onKey, true);
    document.body.appendChild(overlay);
    window.lucide?.createIcons();
    (overlay.querySelector("[data-mirror-quota-add-form] input[name=url]") || overlay.querySelector("[data-mirror-quota-close]"))?.focus();
  }

  async function submitMirrorQuotaDialog() {
    const dialog = mirrorQuotaUi.dialog;
    const form = dialog?.overlay.querySelector("[data-mirror-quota-add-form]");
    if (!dialog || !form) return;
    const status = form.querySelector("[data-mirror-quota-add-status]");
    const button = dialog.overlay.querySelector("[data-mirror-quota-submit]");
    const url = String(form.elements.url?.value || "").trim();
    if (!url) {
      if (status) status.textContent = "Enter the mirror's address.";
      form.elements.url?.focus();
      return;
    }
    if (button) button.disabled = true;
    if (status) {
      status.className = "min-h-4 text-[11px] text-muted-foreground";
      status.textContent = "Adding...";
    }
    try {
      await sendRepoMirrorQuota(dialog.repo, "POST", { url, label: String(form.elements.label?.value || "").trim() });
      closeMirrorQuotaDialog();
    } catch (error) {
      if (status) {
        status.className = "min-h-4 text-[11px] text-destructive";
        status.textContent = error.message;
      }
    } finally {
      if (button) button.disabled = false;
    }
  }

  async function submitMirrorQuotaTarget(form) {
    const repo = state.selectedRepo;
    const status = form.querySelector("[data-mirror-quota-target-status]");
    const target = Number(form.elements.target?.value);
    if (!repo || !Number.isInteger(target) || target < 0) {
      if (status) status.textContent = "Enter a whole number.";
      return;
    }
    if (status) status.textContent = "Saving...";
    try {
      await sendRepoMirrorQuota(repo, "PUT", { target });
    } catch (error) {
      const fresh = document.querySelector("[data-mirror-quota-target-status]");
      if (fresh) {
        fresh.className = "min-w-0 truncate text-destructive";
        fresh.textContent = error.message;
      }
    }
  }

  async function removeRepoMirror(button) {
    const repo = state.selectedRepo;
    const id = button.getAttribute("data-mirror-quota-remove") || "";
    if (!repo || !id) return;
    const ok = await confirmAccountAction({
      title: "Remove this mirror?",
      body: "It stops being checked and counted. The copy itself is not touched.",
      confirmLabel: "Remove",
    });
    if (!ok) return;
    try {
      await sendRepoMirrorQuota(repo, "DELETE", { id });
    } catch (error) {
      const status = document.querySelector("[data-mirror-quota-status]");
      if (status) status.textContent = error.message;
    }
  }

  function initRepoMirrorQuota() {
    document.addEventListener("click", async (event) => {
      const target = event.target;
      if (!target?.closest) return;
      if (target.closest("[data-mirror-quota-add]")) {
        event.preventDefault();
        if (state.selectedRepo) openMirrorQuotaDialog(state.selectedRepo);
        return;
      }
      const remove = target.closest("[data-mirror-quota-remove]");
      if (remove) {
        event.preventDefault();
        removeRepoMirror(remove);
        return;
      }
      const dialog = mirrorQuotaUi.dialog;
      if (!dialog || !dialog.overlay.contains(target)) return;
      if (target === dialog.overlay || target.closest("[data-mirror-quota-close]")) {
        closeMirrorQuotaDialog();
        return;
      }
      if (target.closest("[data-mirror-quota-submit]")) {
        event.preventDefault();
        submitMirrorQuotaDialog();
        return;
      }
      const copy = target.closest("[data-mirror-quota-copy]");
      if (copy) {
        event.preventDefault();
        if (await copyTextToClipboard(copy.getAttribute("data-mirror-quota-copy") || "")) copy.textContent = "Copied";
      }
    });
    document.addEventListener("submit", (event) => {
      const goal = event.target?.closest?.("[data-mirror-quota-target-form]");
      if (goal) {
        event.preventDefault();
        submitMirrorQuotaTarget(goal);
        return;
      }
      if (event.target?.closest?.("[data-mirror-quota-add-form]")) {
        event.preventDefault();
        submitMirrorQuotaDialog();
      }
    });
  }

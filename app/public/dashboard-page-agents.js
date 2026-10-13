// 09-agents.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["agents"] = (core) => {
  const { $, escapeHtml, formatCount, formatTimeAgo, loadRunnerAccountsPicker, loadingHtml, orgAgentRequest, orgAgentStatusDotClass, parseFlexibleDate, repoAgentStatusTone, state } = core;
  const AGENTS_ACTIVE_STATUSES = new Set(["queued", "running", "security_pending"]);
  const AGENTS_FAILED_STATUSES = new Set(["failed", "rejected", "stopped"]);
  const AGENTS_FILTER_KEYS = Object.freeze(["status", "repo", "node"]);
  const AGENTS_STATUS_LABELS = Object.freeze({
    running: "Working",
    queued: "Queued",
    security_pending: "Security review",
    completed: "Completed",
    failed: "Failed",
    rejected: "Rejected",
    stopped: "Stopped",
    attention: "Needs attention",
    stopping: "Stopping",
    desktop: "On desktop",
  });
  const AGENTS_PROVIDER_LABELS = Object.freeze({
    "claude-code": "Claude Code",
    codex: "Codex",
    "local-llama": "Local model",
  });
  const agentsPageView = {
    cloud: [],
    desktop: [],
    loading: true,
    signedOut: false,
    cloudError: "",
    desktopError: "",
    finishedBefore: 0,
    desktopTruncated: false,
    filters: { status: "", repo: "", node: "" },
    reads: { cloud: { busy: false, again: false }, desktop: { busy: false, again: false } },
  };

  function agentsSessionHref(owner, repo, id) {
    const base = `/${encodeURIComponent(owner)}/${encodeURIComponent(repo)}/agents`;
    return id ? `${base}?session=${encodeURIComponent(id)}` : base;
  }

  function agentsCloudRow(session) {
    const status = String(session?.status || "");
    const info = session?.agentInfo && typeof session.agentInfo === "object" ? session.agentInfo : {};
    const owner = String(session?.org || "");
    const repo = String(session?.repo || "");
    const id = String(session?.id || "");
    const provider = String(session?.provider || "");
    const attention = String(session?.displayStatus || "") === "attention";
    const stopping = AGENTS_ACTIVE_STATUSES.has(status) && Number(session?.stopRequestedAt) > 0;
    return {
      key: `cloud:${id}`,
      kind: session?.scope === "organization" ? "organization" : "cloud",
      id,
      title: String(session?.title || "").trim() || `${AGENTS_PROVIDER_LABELS[provider] || "Agent"} session`,
      owner,
      repo,
      provider: AGENTS_PROVIDER_LABELS[provider] || provider,
      model: String(info.model || session?.requestedModel || ""),
      node: String(session?.targetNode || ""),
      status,
      badge: stopping ? "stopping" : attention ? "attention" : status,
      active: AGENTS_ACTIVE_STATUSES.has(status),
      live: status === "running",
      attention,
      createdAt: Number(session?.createdAt) || 0,
      updatedAt: Number(session?.updatedAt) || 0,
      createdBy: String(session?.createdBy || ""),
      message: attention ? String(session?.diagnostic?.message || "") : "",
      href: owner && repo && id ? agentsSessionHref(owner, repo, id) : "",
    };
  }

  function agentsDesktopRow(agent) {
    const owner = String(agent?.owner || "");
    const repo = String(agent?.repo || "");
    const id = String(agent?.id || "");
    return {
      key: `desktop:${owner}/${repo}:${id}`,
      kind: "desktop",
      id,
      title: `Desktop session ${id}`,
      owner,
      repo,
      provider: "",
      model: "",
      node: String(agent?.node || owner),
      status: "desktop",
      badge: "desktop",
      active: false,
      live: false,
      attention: false,
      createdAt: 0,
      updatedAt: Number(agent?.reportedAt) || 0,
      createdBy: "",
      message: "",
      href: owner && repo ? agentsSessionHref(owner, repo, "") : "",
    };
  }

  function agentsRows() {
    return [...agentsPageView.cloud, ...agentsPageView.desktop]
      .sort((a, b) => (Number(b.live) - Number(a.live))
        || (Number(b.active) - Number(a.active))
        || (b.updatedAt - a.updatedAt));
  }

  function agentsRepoLabel(row) {
    return row.owner && row.repo ? `${row.owner}/${row.repo}` : row.repo || "";
  }

  function agentsMatchStatus(row, wanted) {
    if (!wanted) return true;
    if (wanted === "active") return row.active;
    if (wanted === "attention") return row.attention;
    if (wanted === "completed") return row.status === "completed";
    if (wanted === "failed") return AGENTS_FAILED_STATUSES.has(row.status);
    if (wanted === "desktop") return row.kind === "desktop";
    return row.status === wanted;
  }

  function agentsFilteredRows(rows = agentsRows()) {
    const { status, repo, node } = agentsPageView.filters;
    return rows.filter((row) => agentsMatchStatus(row, status)
      && (!repo || agentsRepoLabel(row).toLowerCase() === repo)
      && (!node || row.node.toLowerCase() === node));
  }

  function agentsClockHtml(label, value) {
    const date = Number(value) > 0 ? parseFlexibleDate(value) : null;
    if (!date) return "";
    return `<span>${escapeHtml(label)} <time datetime="${escapeHtml(date.toISOString())}" title="${escapeHtml(date.toLocaleString())}">${escapeHtml(formatTimeAgo(value))}</time></span>`;
  }

  function agentsDotClass(row) {
    if (row.kind === "desktop") return "bg-muted-foreground";
    if (row.attention) return "bg-amber-500";
    if (row.live) return "bg-emerald-500 animate-pulse";
    return orgAgentStatusDotClass(row.status);
  }

  function agentsBadgeTone(row) {
    if (row.kind === "desktop") return "text-muted-foreground";
    if (row.attention) return "text-amber-500";
    if (row.live) return "text-emerald-500";
    return repoAgentStatusTone(row.status);
  }

  function agentsKindLabel(row) {
    if (row.kind === "organization") return "Organization run";
    if (row.kind === "desktop") return "Desktop";
    return "Cloud run";
  }

  function renderAgentsRow(row) {
    const repoLabel = agentsRepoLabel(row);
    const runtime = [row.provider, row.model].filter(Boolean).join(" · ");
    const badge = AGENTS_STATUS_LABELS[row.badge] || row.badge || "Unknown";
    const head = `
        <div class="flex min-w-0 items-center gap-2">
          <span class="h-2 w-2 shrink-0 rounded-full ${agentsDotClass(row)}" aria-hidden="true"></span>
          <span class="min-w-0 flex-1 truncate font-medium text-foreground">${escapeHtml(row.title)}</span>
          <span data-agents-status="${escapeHtml(row.badge)}" class="shrink-0 rounded-full border border-border px-2 py-0.5 font-mono text-[11px] ${agentsBadgeTone(row)}">${escapeHtml(badge)}</span>
        </div>
        <div class="flex flex-wrap items-center gap-x-3 gap-y-1 pl-4 text-xs text-muted-foreground">
          ${repoLabel ? `<span class="font-mono text-foreground">${escapeHtml(repoLabel)}</span>` : ""}
          <span>${escapeHtml(agentsKindLabel(row))}</span>
          ${runtime ? `<span>${escapeHtml(runtime)}</span>` : ""}
          ${row.node ? `<span class="inline-flex items-center gap-1"><i data-lucide="server" class="h-3 w-3"></i><span class="font-mono">${escapeHtml(row.node)}</span></span>` : ""}
          ${row.createdBy ? `<span>by ${escapeHtml(row.createdBy)}</span>` : ""}
          ${agentsClockHtml("Started", row.createdAt)}
          ${agentsClockHtml(row.kind === "desktop" ? "Reported" : "Updated", row.updatedAt)}
        </div>
        ${row.message ? `<p class="pl-4 text-xs text-amber-500">${escapeHtml(row.message)}</p>` : ""}
        ${row.kind === "desktop" ? `<p class="pl-4 text-xs text-muted-foreground">End-to-end encrypted: the relay holds only a sealed copy. Open the repository's Agents tab while this desktop is running to follow and manage it.</p>` : ""}`;
    const attrs = `data-agents-row="${escapeHtml(row.key)}" data-agents-kind="${escapeHtml(row.kind)}"`;
    if (row.href) {
      return `<a ${attrs} href="${escapeHtml(row.href)}" class="grid gap-1.5 px-4 sm:px-5 py-3 text-sm transition-colors hover:bg-secondary/50">${head}</a>`;
    }
    return `<div ${attrs} class="grid gap-1.5 px-4 sm:px-5 py-3 text-sm">${head}</div>`;
  }

  function renderAgentsFilterOptions(rows) {
    const choices = {
      repo: [...new Set(rows.map((row) => agentsRepoLabel(row).toLowerCase()).filter(Boolean))].sort(),
      node: [...new Set(rows.map((row) => row.node.toLowerCase()).filter(Boolean))].sort(),
    };
    for (const key of ["repo", "node"]) {
      const select = $(`[data-agents-filter="${key}"]`);
      if (!select) continue;
      const wanted = agentsPageView.filters[key];
      const values = wanted && !choices[key].includes(wanted) ? [wanted, ...choices[key]] : choices[key];
      select.innerHTML = `<option value="">All</option>${values.map((value) => `<option value="${escapeHtml(value)}">${escapeHtml(value)}</option>`).join("")}`;
      select.value = wanted;
    }
    const status = $('[data-agents-filter="status"]');
    if (status) status.value = agentsPageView.filters.status;
  }

  function setAgentsNotice(message, isError) {
    const notice = $("[data-agents-notice]");
    if (!notice) return;
    notice.hidden = !message;
    notice.textContent = message || "";
    notice.classList.toggle("text-destructive", Boolean(isError));
    notice.classList.toggle("text-muted-foreground", !isError);
  }

  function renderAgentsPage() {
    const list = $("[data-agents-list]");
    if (!list) return;
    const count = $("[data-agents-count]");
    if (agentsPageView.signedOut) {
      if (count) count.textContent = "Signed out";
      setAgentsNotice("");
      list.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">Sign in to see your agents. <a href="/login?next=${encodeURIComponent("/dashboard/agents")}" class="underline hover:text-foreground">Sign in</a></div>`;
      return;
    }
    if (agentsPageView.loading && !agentsPageView.cloud.length && !agentsPageView.desktop.length) {
      if (count) count.textContent = "Loading";
      list.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading agents...")}</div>`;
      return;
    }
    const rows = agentsRows();
    const visible = agentsFilteredRows(rows);
    const working = rows.filter((row) => row.active).length;
    if (count) {
      count.textContent = working
        ? `${formatCount(working)} working · ${formatCount(rows.length)} total`
        : `${formatCount(rows.length)} total`;
    }
    renderAgentsFilterOptions(rows);
    const problems = [agentsPageView.cloudError, agentsPageView.desktopError].filter(Boolean);
    const notes = [];
    if (agentsPageView.finishedBefore > 0) {
      notes.push("Older finished cloud runs are listed in each repository's Agents tab.");
    }
    if (agentsPageView.desktopTruncated) {
      notes.push("Only the most recently reported desktop sessions are shown.");
    }
    setAgentsNotice(problems.length ? problems.join(" ") : notes.join(" "), problems.length > 0);
    const filtered = AGENTS_FILTER_KEYS.some((key) => agentsPageView.filters[key]);
    list.innerHTML = visible.length
      ? visible.map(renderAgentsRow).join("")
      : `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${escapeHtml(filtered
        ? "No agents match these filters."
        : "No agent sessions yet. Start one from the robot button in the header, from a repository's Agents tab, or from the ForkMesh desktop.")}</div>`;
    window.lucide?.createIcons();
  }

  function agentsErrorText(error, fallback) {
    if (Number(error?.status) === 401) return "Your session has expired. Sign in again to see your agents.";
    return String(error?.message || "") || fallback;
  }

  async function agentsRead(which, read) {
    const guard = agentsPageView.reads[which];
    if (guard.busy) {
      guard.again = true;
      return;
    }
    guard.busy = true;
    try {
      await read();
    } finally {
      guard.busy = false;
      if (guard.again) {
        guard.again = false;
        void agentsRead(which, read);
      }
    }
  }

  function loadAgentsCloud() {
    return agentsRead("cloud", async () => {
      try {
        const payload = await orgAgentRequest("GET", "/api/account/agent-sessions");
        agentsPageView.cloud = (Array.isArray(payload?.sessions) ? payload.sessions : []).map(agentsCloudRow);
        agentsPageView.finishedBefore = Number(payload?.finishedBefore) || 0;
        agentsPageView.cloudError = "";
      } catch (error) {
        agentsPageView.cloudError = agentsErrorText(error, "Cloud agent runs are unavailable right now.");
      }
      renderAgentsPage();
    });
  }

  function loadAgentsDesktop() {
    return agentsRead("desktop", async () => {
      try {
        const payload = await orgAgentRequest("GET", "/api/account/desktop-agents");
        agentsPageView.desktop = (Array.isArray(payload?.agents) ? payload.agents : []).map(agentsDesktopRow);
        agentsPageView.desktopTruncated = payload?.truncated === true;
        agentsPageView.desktopError = "";
      } catch (error) {
        agentsPageView.desktopError = agentsErrorText(error, "Desktop agent sessions are unavailable right now.");
      }
      renderAgentsPage();
    });
  }

  async function loadAgentsPage() {
    if (!state.session?.sessionToken) {
      agentsPageView.signedOut = true;
      agentsPageView.loading = false;
      renderAgentsPage();
      return;
    }
    agentsPageView.signedOut = false;
    agentsPageView.loading = true;
    renderAgentsPage();
    await Promise.all([loadAgentsCloud(), loadAgentsDesktop(), loadRunnerAccountsPicker(true)]);
    agentsPageView.loading = false;
    renderAgentsPage();
  }

  function syncAgentsFilterUrl() {
    const params = new URLSearchParams(location.search);
    for (const key of AGENTS_FILTER_KEYS) {
      if (agentsPageView.filters[key]) params.set(key, agentsPageView.filters[key]);
      else params.delete(key);
    }
    const query = params.toString();
    try {
      history.replaceState(history.state, "", `${location.pathname}${query ? `?${query}` : ""}${location.hash}`);
    } catch (_) {}
  }

  function initAgentsPage() {
    if (!$("[data-agents-panel]")) return;
    const params = new URLSearchParams(location.search);
    for (const key of AGENTS_FILTER_KEYS) {
      agentsPageView.filters[key] = String(params.get(key) || "").trim().toLowerCase();
    }
    $("[data-agents-refresh]")?.addEventListener("click", () => { void loadAgentsPage(); });
    $("[data-agents-filters]")?.addEventListener("change", (event) => {
      const key = event.target?.dataset?.agentsFilter;
      if (!AGENTS_FILTER_KEYS.includes(key)) return;
      agentsPageView.filters[key] = String(event.target.value || "").toLowerCase();
      syncAgentsFilterUrl();
      renderAgentsPage();
    });
    window.addEventListener("forkmesh:cloud-agents", () => {
      if (!agentsPageView.signedOut) void loadAgentsCloud();
    });
    window.addEventListener("forkmesh:desktop-agents", () => {
      if (!agentsPageView.signedOut) void loadAgentsDesktop();
    });
    void loadAgentsPage();
  }
  return { initAgentsPage };
  };
})();

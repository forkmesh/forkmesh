  const BOUNTY_AMOUNT_RE = /^(?:0|[1-9][0-9]{0,6})(?:\.[0-9]{1,9})?$/;
  const BOUNTY_ERROR_TEXT = {
    invalid_session: "Your session expired. Log in again to bid.",
    org_member_required: "Bounties are organization work. Ask an organization owner to add your account.",
    invalid_bounty_request: "Enter a positive SOL amount with at most 9 decimals.",
    invalid_department: "Pick one of the listed departments.",
    invalid_task_routing: "Enter a task title of at least 3 characters.",
    origin_not_allowed: "Open this page on forkmesh.com and try again.",
  };
  const BOUNTY_DEPARTMENT_LABELS = {
    general: "General",
    engineering: "Engineering",
    "product-design": "Product + design",
    marketing: "Marketing",
    security: "Security",
    infrastructure: "Infrastructure",
    community: "Community",
    partnerships: "Partnerships",
    operations: "Operations",
    executive: "Executive",
    "quality-assurance": "Quality assurance",
  };

  function bountySignedIn() {
    return Boolean(state.session?.sessionToken && state.session?.nodeName);
  }

  function bountyErrorText(error, fallback) {
    return BOUNTY_ERROR_TEXT[String(error?.message || "")] || fallback;
  }

  async function bountyRequest(method, path, body = null) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, {
      method,
      cache: "no-store",
      credentials: "same-origin",
      headers,
      body: body === null ? undefined : JSON.stringify(body),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      throw error;
    }
    return payload;
  }

  function bountyIsBid(task) {
    return task?.kind === "bid" && Boolean(task?.bountyRequest?.amountSol);
  }

  function bountyIsDone(task) {
    return task?.status === "done" || Number(task?.completedAt) > 0;
  }

  function bountyStatusBadge(task) {
    if (bountyIsDone(task)) {
      return '<span class="fm-label fm-label-success shrink-0"><i data-lucide="circle-check" class="h-3 w-3"></i>Completed</span>';
    }
    if (task?.status === "active") {
      return '<span class="fm-label fm-label-accent shrink-0"><i data-lucide="loader-circle" class="h-3 w-3"></i>In progress</span>';
    }
    return '<span class="fm-label shrink-0"><i data-lucide="circle-dot" class="h-3 w-3"></i>Open</span>';
  }

  function bountyVisibleItems() {
    const view = state.bountiesView;
    const bids = view.items.filter(bountyIsBid);
    if (view.filter === "done") return bids.filter(bountyIsDone);
    if (view.filter === "all") return bids;
    return bids.filter((task) => !bountyIsDone(task));
  }

  function bountyRowHtml(task) {
    const bidder = String(task.assignee || task.createdBy || "").toLowerCase();
    const you = bidder && bidder === String(state.session?.nodeName || "").toLowerCase();
    const department = BOUNTY_DEPARTMENT_LABELS[task.department] || task.department || "General";
    const when = relativeTimeLabel(task.createdAt);
    const amount = `${task.bountyRequest.amountSol} ${task.bountyRequest.currency || "SOL"}`;
    const details = String(task.details || "").trim();
    return `<article class="px-4 sm:px-5 py-4 flex flex-col gap-3 sm:flex-row sm:items-start">
      <div class="min-w-0 flex-1">
        <div class="flex flex-wrap items-center gap-2">
          ${bountyStatusBadge(task)}
          ${you ? '<span class="fm-label shrink-0">your bid</span>' : ""}
        </div>
        <a class="mt-2 block text-sm font-semibold text-foreground hover:underline" href="/dashboard/tasks?task=${encodeURIComponent(task.id)}">${escapeHtml(task.title || "Organization task")}</a>
        ${details ? `<p class="mt-1 text-xs text-muted-foreground line-clamp-2">${escapeHtml(details)}</p>` : ""}
        <p class="mt-1.5 text-xs text-muted-foreground">
          ${bidder ? `Bid by <a class="text-foreground hover:underline" href="${escapeHtml(publicProfileUrl(bidder))}">@${escapeHtml(bidder)}</a>` : "Bid"}
          · ${escapeHtml(department)}${when ? ` · ${escapeHtml(when)}` : ""}
        </p>
      </div>
      <div class="shrink-0 flex items-center justify-between gap-3 sm:flex-col sm:items-end">
        <span class="font-mono text-sm font-semibold tabular-nums text-foreground">${escapeHtml(amount)}</span>
        <span class="text-[11px] capitalize text-muted-foreground">${escapeHtml(task.bountyRequest.status || "requested")}</span>
      </div>
    </article>`;
  }

  function renderBountiesNotice() {
    const root = $("[data-bounties-notice]");
    if (!root) return;
    const view = state.bountiesView;
    if (!view.notice) {
      root.innerHTML = "";
      return;
    }
    const bad = view.noticeTone === "bad";
    root.innerHTML = `<div class="px-4 sm:px-5 py-3 border-b border-border">${bountyFlashHtml(bad ? "danger" : "success", escapeHtml(view.notice))}</div>`;
    window.lucide?.createIcons();
  }

  function bountyFlashHtml(tone, html) {
    const icon = { danger: "circle-alert", success: "circle-check", warning: "triangle-alert", info: "info" }[tone] || "info";
    const variant = tone === "info" ? "" : ` fm-flash-${tone}`;
    return `<div class="fm-flash${variant}"><i data-lucide="${icon}" aria-hidden="true"></i><div class="fm-flash-body">${html}</div></div>`;
  }

  function bountyBlankslateHtml(icon, title, html) {
    return `<div class="fm-blankslate"><i data-lucide="${icon}" aria-hidden="true"></i><p class="fm-blankslate-title">${title}</p>${html ? `<p class="fm-blankslate-desc">${html}</p>` : ""}</div>`;
  }

  function renderBountiesStart() {
    const root = $("[data-bounties-start]");
    const form = $("[data-bounties-form]");
    if (!root || !form) return;
    const view = state.bountiesView;
    if (!bountySignedIn()) {
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      root.hidden = false;
      root.innerHTML = bountyFlashHtml("info", `<a class="text-accent underline" href="/login?next=${next}">Log in</a>
        with your organization account to see bounty bids and submit your own.`);
      form.hidden = true;
      window.lucide?.createIcons();
      return;
    }
    if (view.access === "member") {
      root.hidden = true;
      root.innerHTML = "";
      form.hidden = false;
      const submit = form.querySelector("[data-bounties-submit]");
      if (submit) submit.disabled = view.busy;
      return;
    }
    root.hidden = false;
    form.hidden = true;
    root.innerHTML = view.access === "denied"
      ? bountyFlashHtml("warning", escapeHtml(BOUNTY_ERROR_TEXT.org_member_required))
      : view.loading
        ? `<p class="text-sm text-muted-foreground">${loadingHtml("Checking your organization membership...")}</p>`
        : bountyFlashHtml("danger", escapeHtml(view.error));
    window.lucide?.createIcons();
  }

  function renderBountiesList() {
    const root = $("[data-bounties-list]");
    const count = $("[data-bounties-count]");
    if (!root) return;
    const view = state.bountiesView;
    document.querySelectorAll("[data-bounties-filter]").forEach((button) => {
      button.setAttribute("aria-pressed", String(button.dataset.bountiesFilter === view.filter));
    });
    const openCount = view.items.filter((task) => bountyIsBid(task) && !bountyIsDone(task)).length;
    if (count) {
      count.textContent = view.loading ? "Loading" : view.access === "member" ? `${formatCount(openCount)} open` : "Members only";
    }
    let body = "";
    if (!bountySignedIn()) {
      body = bountyBlankslateHtml("lock", "Members only", "Bounty bids are organization-private. Log in to see them.");
    } else if (view.loading) {
      body = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading bids...")}</div>`;
    } else if (view.access === "denied") {
      body = bountyBlankslateHtml("lock", "Members only", "Only organization members can see bounty bids.");
    } else if (view.error) {
      body = bountyBlankslateHtml("circle-alert", "Bids did not load", escapeHtml(view.error));
    }
    if (body) {
      root.innerHTML = body;
      window.lucide?.createIcons();
      return;
    }
    const items = bountyVisibleItems().sort((a, b) =>
      Number(bountyIsDone(a)) - Number(bountyIsDone(b)) || Number(b.createdAt || 0) - Number(a.createdAt || 0));
    root.innerHTML = items.length
      ? items.map(bountyRowHtml).join("")
      : view.filter === "done"
        ? bountyBlankslateHtml("badge-dollar-sign", "No completed bids yet", "")
        : bountyBlankslateHtml("badge-dollar-sign", "No open bids yet", "Propose the first piece of work above.");
    window.lucide?.createIcons();
  }

  function renderBountiesPage() {
    renderBountiesNotice();
    renderBountiesStart();
    renderBountiesList();
  }

  async function loadBounties() {
    const view = state.bountiesView;
    if (!bountySignedIn()) {
      view.loading = false;
      renderBountiesPage();
      return;
    }
    view.loading = true;
    view.error = "";
    renderBountiesPage();
    try {
      const tasks = [];
      let offset = 0;
      let limit = 2000;
      do {
        const page = await bountyRequest("GET", offset ? `/api/tasks?offset=${offset}` : "/api/tasks");
        tasks.push(...(Array.isArray(page?.tasks) ? page.tasks : []));
        limit = Number(page?.taskLimit) || limit;
        const next = Number(page?.nextOffset);
        if (page?.hasMore !== true || !Number.isInteger(next) || next <= offset) break;
        offset = next;
      } while (tasks.length < limit);
      view.items = tasks;
      view.access = "member";
    } catch (error) {
      view.items = [];
      if (error?.message === "org_member_required") {
        view.access = "denied";
      } else {
        view.access = "";
        view.error = bountyErrorText(error, "Bounty bids are unavailable right now. Try Refresh in a moment.");
      }
    } finally {
      view.loading = false;
      renderBountiesPage();
    }
  }

  async function submitBountyBid(form) {
    const view = state.bountiesView;
    const values = new FormData(form);
    const title = String(values.get("title") || "").trim();
    const details = String(values.get("details") || "").trim();
    const amountSol = String(values.get("amountSol") || "").trim();
    const department = String(values.get("department") || "general");
    if (title.length < 3) {
      view.notice = "Enter a task title of at least 3 characters.";
      view.noticeTone = "bad";
      renderBountiesNotice();
      form.elements.title?.focus();
      return;
    }
    if (!BOUNTY_AMOUNT_RE.test(amountSol) || Number(amountSol) <= 0) {
      view.notice = BOUNTY_ERROR_TEXT.invalid_bounty_request;
      view.noticeTone = "bad";
      renderBountiesNotice();
      form.elements.amountSol?.focus();
      return;
    }
    view.busy = true;
    view.notice = "";
    renderBountiesPage();
    try {
      await bountyRequest("POST", "/api/tasks", {
        kind: "bid",
        title,
        details,
        bountyAmountSol: amountSol,
        department,
        destination: "department",
        assigneeKind: "user",
      });
      form.reset();
      view.notice = `Bid added to the organization task list with a ${amountSol} SOL request.`;
      view.noticeTone = "good";
      view.filter = "open";
      await loadBounties();
    } catch (error) {
      view.notice = bountyErrorText(error, "The bid could not be submitted. Please try again.");
      view.noticeTone = "bad";
      if (error?.message === "org_member_required") view.access = "denied";
    } finally {
      view.busy = false;
      renderBountiesPage();
    }
  }

  function initBountiesPage() {
    const panel = $("[data-bounties-panel]");
    if (!panel) return;
    panel.addEventListener("click", (event) => {
      if (event.target.closest("[data-bounties-refresh]")) {
        state.bountiesView.notice = "";
        void loadBounties();
        return;
      }
      const filter = event.target.closest("[data-bounties-filter]");
      if (filter) {
        state.bountiesView.filter = filter.dataset.bountiesFilter || "open";
        renderBountiesList();
      }
    });
    $("[data-bounties-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      if (state.bountiesView.busy) return;
      void submitBountyBid(event.currentTarget);
    });
    void loadBounties();
  }

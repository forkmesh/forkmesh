  const aiReviewUi = { settingsLoaded: false, pulls: {}, timers: {} };
  const AI_REVIEW_TITLE = "### 🤖 Automated AI review";
  const AI_REVIEW_POLL_MS = 20000;
  const AI_REVIEW_STATUS_LABELS = {
    queued: "Queued",
    reviewing: "Reviewing",
    done: "Done",
    skipped: "Skipped",
    failed: "Failed",
  };
  const AI_REVIEW_REASONS = {
    already_reviewed: "this head commit was already reviewed",
    draft: "draft",
    closed: "closed",
    merged: "merged",
    "closed-merged": "merged",
    deleted: "deleted",
    too_large: "diff too large",
    nothing_to_review: "only binary, lockfile or generated files",
    private_repository: "private repository",
    repository_opted_out: "not enabled for this repository",
    repository_limit: "daily limit for this repository",
    author_limit: "daily limit for this author",
    daily_cap: "instance daily limit, runs tomorrow",
    pending: "waiting for the pull request record",
    retrying: "retrying",
    repository_not_found: "repository not found",
  };
  const AI_REVIEW_ERRORS = {
    invalid_account_id: "The account ID is 32 hexadecimal characters.",
    invalid_token: "That does not look like a Cloudflare API token.",
    model_not_allowed: "Pick a model from the list.",
    forbidden: "Only administrators can change this.",
    rate_limited: "Too many requests - try again later.",
    repository_opted_out: "This repository has not turned AI review on.",
    ai_review_off: "AI review is off on this instance.",
  };

  function aiReviewErrorText(error) {
    return AI_REVIEW_ERRORS[String(error?.message || "")] || "That didn't work. Try again.";
  }

  async function aiReviewPost(path, body) {
    const response = await fetch(path, {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ ...body, sessionToken: state.session?.sessionToken || "" }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) throw new Error(String(data.error || `HTTP ${response.status}`));
    return data;
  }

  function aiReviewSettingsStatus(data) {
    if (!data?.enabled) return "Off.";
    if (!data.ready) return "On, but no Cloudflare account ID and token are set: nothing is reviewed.";
    const model = (data.models || []).find((item) => item.id === data.model);
    return `On. Reviews run on Cloudflare Workers AI with ${model ? model.label : data.model}.${data.configured ? " A token is set." : ""}`;
  }

  function fillAiReviewSettings(section, data) {
    const form = section.querySelector("[data-ai-review-settings-form]");
    form.querySelector("[data-ai-review-enabled]").checked = data.enabled === true;
    form.querySelector("[data-ai-review-default-on]").checked = data.defaultOn === true;
    form.querySelector("[data-ai-review-account]").value = data.accountId || "";
    const select = form.querySelector("[data-ai-review-model]");
    select.innerHTML = (data.models || []).map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.label)}${item.default ? " (default)" : ""}</option>`).join("");
    select.value = data.model || "";
    form.querySelector("[data-ai-review-daily-cap]").value = String(data.dailyCap ?? "");
    form.querySelector("[data-ai-review-max-diff]").value = String(data.maxDiffKb ?? "");
    form.querySelector("[data-ai-review-repo-daily]").value = String(data.repoDaily ?? "");
    form.querySelector("[data-ai-review-author-daily]").value = String(data.authorDaily ?? "");
    const bindingRow = form.querySelector("[data-ai-review-binding-row]");
    bindingRow.hidden = data.bindingAvailable !== true;
    form.querySelector("[data-ai-review-binding]").checked = data.useBinding === true;
    form.querySelector("[data-ai-review-token]").value = "";
    form.querySelector("[data-ai-review-settings-status]").textContent = aiReviewSettingsStatus(data);
  }

  async function initAiReviewSettings() {
    const section = $("[data-ai-review-settings]");
    if (!section || aiReviewUi.settingsLoaded || !state.session?.sessionToken) return;
    aiReviewUi.settingsLoaded = true;
    let data = null;
    try {
      data = await fetchJson("/api/ai-review/settings", { fresh: true });
    } catch (_) {
      data = null;
    }
    if (!data?.ok) return;
    $$("[data-ai-review-settings-link]").forEach((link) => { link.hidden = false; });
    fillAiReviewSettings(section, data);
    const form = section.querySelector("[data-ai-review-settings-form]");
    const status = form.querySelector("[data-ai-review-settings-status]");
    const number = (selector) => Number(form.querySelector(selector).value);
    const save = async (extra) => {
      status.textContent = "Saving…";
      try {
        const reply = await aiReviewPost("/api/ai-review/settings", {
          enabled: form.querySelector("[data-ai-review-enabled]").checked,
          defaultOn: form.querySelector("[data-ai-review-default-on]").checked,
          accountId: form.querySelector("[data-ai-review-account]").value.trim(),
          model: form.querySelector("[data-ai-review-model]").value,
          dailyCap: number("[data-ai-review-daily-cap]"),
          maxDiffKb: number("[data-ai-review-max-diff]"),
          repoDaily: number("[data-ai-review-repo-daily]"),
          authorDaily: number("[data-ai-review-author-daily]"),
          useBinding: form.querySelector("[data-ai-review-binding]").checked,
          ...extra,
        });
        fillAiReviewSettings(section, reply);
        status.textContent = `Saved. ${aiReviewSettingsStatus(reply)}`;
      } catch (error) {
        status.textContent = aiReviewErrorText(error);
      }
    };
    form.addEventListener("submit", (event) => {
      event.preventDefault();
      const token = form.querySelector("[data-ai-review-token]").value.trim();
      void save(token ? { token } : {});
    });
    form.querySelector("[data-ai-review-clear-token]").addEventListener("click", () => {
      void save({ clearToken: true });
    });
  }

  function pullAiReviewKey(repo, number) {
    return `${String(repo?.owner || "").toLowerCase()}/${repo?.name || ""}#${number}`;
  }

  function pullAiReviewApiPath(repo, number) {
    return `/api/ai-review/pulls/${encodeURIComponent(repo?.owner || "")}/${encodeURIComponent(repo?.name || "")}/${encodeURIComponent(String(number))}`;
  }

  function pullAiReviewChipClass(status) {
    if (status === "done") return "border-success/40 bg-success/10 text-success";
    if (status === "failed") return "border-destructive/40 bg-destructive/10 text-destructive";
    if (status === "skipped") return "border-border text-muted-foreground";
    return "border-border text-foreground";
  }

  function pullAiReviewDetail(view) {
    const reason = AI_REVIEW_REASONS[view.reason] || "";
    const head = String((view.status === "done" ? view.reviewedHead || view.head : view.head || view.reviewedHead) || "").slice(0, 7);
    const parts = [];
    if (view.status === "done" && view.verdict) parts.push(`suggests ${view.verdict}`);
    if (reason && !(view.status === "done" && view.reason === "already_reviewed")) parts.push(reason);
    if (head) parts.push(`head ${head}`);
    return parts.join(" · ");
  }

  function renderPullAiReviewSlotBody(view) {
    const status = String(view.status || "none");
    const label = AI_REVIEW_STATUS_LABELS[status] || "Not reviewed yet";
    const busy = status === "queued" || status === "reviewing";
    const detail = pullAiReviewDetail(view);
    const chip = `<span data-ai-review-chip data-status="${escapeHtml(status)}" class="inline-flex h-6 w-fit items-center gap-1 rounded-full border px-2 text-[12px] font-medium ${pullAiReviewChipClass(status)}">${busy ? '<i data-lucide="loader-circle" class="h-3.5 w-3.5 animate-spin motion-reduce:animate-none"></i>' : '<i data-lucide="sparkles" class="h-3.5 w-3.5"></i>'}${escapeHtml(label)}</span>`;
    const button = view.canRun
      ? `<button type="button" data-ai-review-run class="fm-btn fm-btn-sm w-fit"${busy ? " disabled" : ""}>Run AI review</button>`
      : "";
    return `${chip}${detail ? `<span data-ai-review-detail class="text-muted-foreground [overflow-wrap:anywhere]">${escapeHtml(detail)}</span>` : ""}${button}<span data-ai-review-message role="status" class="text-muted-foreground"></span>`;
  }

  function renderPullAiReviewSidebar(repo, number) {
    const view = aiReviewUi.pulls[pullAiReviewKey(repo, number)];
    const hidden = view?.enabled ? "" : " hidden";
    return `
      <div data-repo-pull-ai-review="${escapeHtml(String(number))}" class="border-b border-border py-4 first:pt-0"${hidden}>
        <h4 class="mb-2 text-[12px] font-semibold text-muted-foreground">AI review</h4>
        <div data-ai-review-slot class="grid gap-2 text-[12px] text-foreground">${view?.enabled ? renderPullAiReviewSlotBody(view) : ""}</div>
      </div>`;
  }

  function paintPullAiReview(repo, number) {
    const view = aiReviewUi.pulls[pullAiReviewKey(repo, number)];
    const box = document.querySelector(`[data-repo-pull-ai-review="${CSS.escape(String(number))}"]`);
    if (!box) return;
    box.hidden = !view?.enabled;
    const slot = box.querySelector("[data-ai-review-slot]");
    if (slot) slot.innerHTML = view?.enabled ? renderPullAiReviewSlotBody(view) : "";
    window.lucide?.createIcons();
  }

  function schedulePullAiReviewPoll(repo, number) {
    const key = pullAiReviewKey(repo, number);
    clearTimeout(aiReviewUi.timers[key]);
    const view = aiReviewUi.pulls[key];
    if (!view || !["queued", "reviewing"].includes(view.status)) return;
    aiReviewUi.timers[key] = setTimeout(() => {
      if (!document.querySelector(`[data-repo-pull-ai-review="${CSS.escape(String(number))}"]`)) return;
      void loadPullAiReview(repo, number);
    }, AI_REVIEW_POLL_MS);
  }

  async function loadPullAiReview(repo, number) {
    if (!repo?.owner || !repo?.name || !number) return;
    let view = null;
    try {
      view = await fetchJson(pullAiReviewApiPath(repo, number), { fresh: true });
    } catch (_) {
      view = null;
    }
    aiReviewUi.pulls[pullAiReviewKey(repo, number)] = view?.ok ? view : { enabled: false };
    paintPullAiReview(repo, number);
    schedulePullAiReviewPoll(repo, number);
  }

  async function runPullAiReview(button) {
    const detail = state.repoRecordDetail;
    if (detail?.kind !== "pulls" || !detail.repo) return;
    const box = button.closest("[data-repo-pull-ai-review]");
    const message = box?.querySelector("[data-ai-review-message]");
    button.disabled = true;
    if (message) message.textContent = "Queuing…";
    try {
      const view = await aiReviewPost(pullAiReviewApiPath(detail.repo, detail.number), {});
      aiReviewUi.pulls[pullAiReviewKey(detail.repo, detail.number)] = view;
      paintPullAiReview(detail.repo, detail.number);
      schedulePullAiReviewPoll(detail.repo, detail.number);
    } catch (error) {
      button.disabled = false;
      if (message) message.textContent = aiReviewErrorText(error);
    }
  }

  document.addEventListener("click", (event) => {
    const button = event.target.closest?.("[data-ai-review-run]");
    if (button) void runPullAiReview(button);
  });

  function isPullAiReviewEvent(ev) {
    return String(ev?.author || "").toLowerCase() === "forkbot"
      && String(ev?.type || "comment") === "comment"
      && String(ev?.body || "").trimStart().startsWith(AI_REVIEW_TITLE);
  }

  function latestPullAiReviewEvent(events) {
    let latest = null;
    for (const ev of events) {
      if (isPullAiReviewEvent(ev) && (!latest || Number(ev.ts || 0) >= Number(latest.ts || 0))) latest = ev;
    }
    return latest;
  }

  function splitPullAiReviewBody(body) {
    const text = String(body || "").trim();
    const newline = text.indexOf("\n");
    const title = (newline < 0 ? text : text.slice(0, newline)).replace(/^###\s*/, "");
    const rest = newline < 0 ? "" : text.slice(newline + 1).trim();
    const match = /^###\s*Blockers\s*$/m.exec(rest);
    if (!match) return { title, lead: rest, more: "" };
    return { title, lead: rest.slice(0, match.index).trim(), more: rest.slice(match.index).trim() };
  }

  function renderPullAiReviewComment(ev, latest) {
    const parts = splitPullAiReviewBody(ev.body);
    const head = (/head `([0-9a-f]{7})`/.exec(parts.title) || [])[1] || "";
    const fullDate = formatRecordDateTime(ev.ts);
    const header = `
          <div class="flex min-w-0 flex-wrap items-center gap-1.5 border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] py-1.5 pl-4 pr-2 text-[14px] text-muted-foreground">
            ${issueCardAuthorHtml(ev.authorName || "ForkBot", ev.author, true)}
            <span class="rounded-full border border-border px-2 text-[12px] font-medium leading-5 text-foreground">AI review</span>
            <span>commented</span>
            <time datetime="${escapeHtml(fullDate.iso)}" title="${escapeHtml(fullDate.label)}">${escapeHtml(formatTimeAgo(ev.ts))}</time>
          </div>`;
    const reportHtml = `
          <div class="px-4 pt-3 text-[13px] font-semibold text-foreground [overflow-wrap:anywhere]">${escapeHtml(parts.title.replace(/`/g, ""))}</div>
          <div class="px-4 py-3 ${MD_BODY_CLASS}">${renderRecordMarkdown(parts.lead)}</div>
          ${parts.more ? `<details data-ai-review-more class="group border-t border-border">
            <summary class="flex cursor-pointer list-none items-center gap-1 px-4 py-2 text-[12px] font-medium text-foreground"><i data-lucide="chevron-down" class="h-3.5 w-3.5 transition-transform group-open:rotate-180"></i>Show the full report</summary>
            <div class="px-4 pb-3 ${MD_BODY_CLASS}">${renderRecordMarkdown(parts.more)}</div>
          </details>` : ""}
          ${typeof commentReactionEventBar === "function" ? commentReactionEventBar(ev, "px-4 pb-3") : ""}`;
    const card = latest
      ? `<div class="min-w-0 overflow-hidden rounded-md border border-border bg-background">${header}${reportHtml}</div>`
      : `<details data-ai-review-superseded class="group min-w-0 overflow-hidden rounded-md border border-border bg-background">
          <summary class="flex cursor-pointer list-none flex-wrap items-center gap-1.5 px-4 py-2 text-[13px] text-muted-foreground"><i data-lucide="chevron-right" class="h-3.5 w-3.5 shrink-0 transition-transform group-open:rotate-90"></i><span class="min-w-0">Earlier automated AI review${head ? ` of head <span class="font-mono text-[12px]">${escapeHtml(head)}</span>` : ""} · superseded</span></summary>
          ${header}${reportHtml}
        </details>`;
    return `
      <div data-repo-issue-comment data-ai-review-comment class="relative grid min-w-0 gap-4 sm:grid-cols-[40px_minmax(0,1fr)]">
        <span class="hidden sm:block">${issueActorAvatarHtml(ev.authorName || "ForkBot", ev.author, "h-10 w-10", true)}</span>
        ${card}
      </div>`;
  }

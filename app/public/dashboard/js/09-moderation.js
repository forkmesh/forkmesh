  const MODERATION_HIDDEN_TEXT = "*This post was hidden by the moderators.*";
  const MODERATION_WAIT_MS = 2000;
  const MODERATION_EMPTY = Object.freeze({
    hidden: [], hiddenAuthors: [], locked: false, canModerate: false, pendingMine: 0,
  });

  function moderationHeaders(json = false) {
    const headers = { accept: "application/json" };
    if (json) headers["content-type"] = "application/json";
    const token = String(state.session?.sessionToken || "");
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    return headers;
  }

  async function fetchIssueModeration(repo, number) {
    if (!repo || !Number(number)) return MODERATION_EMPTY;
    try {
      const response = await fetch(
        `${repoApiBase(repo)}/moderation?kind=issue&number=${encodeURIComponent(number)}`,
        { credentials: "same-origin", cache: "no-store", headers: moderationHeaders() });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data.ok === false) return MODERATION_EMPTY;
      return {
        hidden: Array.isArray(data.hidden) ? data.hidden.map(String) : [],
        hiddenAuthors: Array.isArray(data.hiddenAuthors)
          ? data.hiddenAuthors.map((name) => String(name).toLowerCase()) : [],
        locked: data.locked === true,
        canModerate: data.canModerate === true,
        pendingMine: Number(data.pendingMine || 0) || 0,
      };
    } catch (_) {
      return MODERATION_EMPTY;
    }
  }

  async function issueModerationWithin(pending) {
    if (!pending) return MODERATION_EMPTY;
    return Promise.race([
      pending,
      new Promise((resolve) => setTimeout(() => resolve(MODERATION_EMPTY), MODERATION_WAIT_MS)),
    ]);
  }

  function applyIssueModeration(text, moderation) {
    const hidden = new Set((moderation?.hidden || []).map(String));
    const authors = new Set((moderation?.hiddenAuthors || []).map((name) => String(name).toLowerCase()));
    if (!hidden.size && !authors.size) return text;
    let record;
    try {
      record = JSON.parse(String(text || ""));
    } catch (_) {
      return text;
    }
    if (!record || typeof record !== "object" || !Array.isArray(record.events)) return text;
    const opened = record.events.find((event) => event && event.type === "open") || {};
    const openId = String(opened.id || "");
    const covered = new Set();
    for (const event of record.events) {
      if (!event || (event.type !== "open" && event.type !== "comment")) continue;
      const id = String(event.id || "");
      const author = String(event.authorName || "").toLowerCase();
      if ((id && hidden.has(id)) || (event.type === "open" && hidden.has("body"))
          || (author && authors.has(author))) covered.add(id);
    }
    if (hidden.has("body") && openId) covered.add(openId);
    if (!covered.size) return text;
    const originals = {};
    record.events = record.events.map((event) => {
      if (!event || typeof event !== "object") return event;
      const id = String(event.id || "");
      const target = String(event.target || "");
      const hit = ((event.type === "open" || event.type === "comment") && covered.has(id))
        || (event.type === "edit" && (covered.has(target) || (!target && covered.has(openId))));
      if (!hit) return event;
      if (event.type !== "edit") originals[id] = String(event.body || "");
      else originals[target || openId] = String(event.body || "");
      return { ...event, body: MODERATION_HIDDEN_TEXT, attachments: [], moderationHidden: true };
    });
    if (covered.has(openId) && "body" in record) record.body = MODERATION_HIDDEN_TEXT;
    if (moderation?.canModerate) moderation.originals = originals;
    return JSON.stringify(record);
  }

  async function postIssueModeration(repo, number, action, target = "") {
    const response = await fetch(`${repoApiBase(repo)}/moderation`, {
      method: "POST",
      credentials: "same-origin",
      cache: "no-store",
      headers: moderationHeaders(true),
      body: JSON.stringify({ action, kind: "issue", number: Number(number), target }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) throw new Error(data.message || data.error || `HTTP ${response.status}`);
    return data;
  }

  function moderationButtonHtml(attr, value, icon, label) {
    return `<button type="button" ${attr}="${escapeHtml(value)}" class="inline-flex h-7 items-center gap-1 rounded-md px-1.5 text-[12px] text-muted-foreground hover:bg-secondary hover:text-foreground" title="${escapeHtml(label)}"><i data-lucide="${icon}" class="h-4 w-4"></i><span class="hidden sm:inline">${escapeHtml(label)}</span></button>`;
  }

  function moderationNoticeHtml(icon, text) {
    return `<div data-repo-issue-moderation-notice class="flex items-start gap-2 rounded-md border border-border bg-secondary/20 px-4 py-3 text-[14px] text-muted-foreground sm:ml-14"><i data-lucide="${icon}" class="mt-0.5 h-4 w-4 shrink-0"></i><span>${escapeHtml(text)}</span></div>`;
  }

  function decorateIssueModeration(container, repo, number, parsed) {
    const moderation = parsed?.issueModeration;
    if (!container || !moderation) return;
    const hidden = new Set(moderation.hidden || []);
    const form = container.querySelector("[data-repo-issue-comment-form]");
    const formRow = form?.parentElement;
    if (moderation.pendingMine > 0 && formRow) {
      formRow.insertAdjacentHTML("beforebegin", moderationNoticeHtml("hourglass",
        moderation.pendingMine === 1
          ? "One of your posts here is waiting for a moderator. It appears once it is approved."
          : `${moderation.pendingMine} of your posts here are waiting for a moderator.`));
    }
    if (moderation.locked) {
      const text = moderation.canModerate
        ? "This thread is locked. Only moderators can reply."
        : "This thread is locked. New comments are turned off.";
      if (!moderation.canModerate && formRow) {
        formRow.outerHTML = moderationNoticeHtml("lock", text);
      } else if (formRow) {
        formRow.insertAdjacentHTML("beforebegin", moderationNoticeHtml("lock", text));
      }
    }
    if (!moderation.canModerate) {
      window.lucide?.createIcons();
      return;
    }
    const actions = container.querySelector("[data-repo-issue-sidebar-actions]");
    if (actions) {
      actions.insertAdjacentHTML("beforeend", moderationButtonHtml(
        "data-repo-issue-moderation-lock", moderation.locked ? "unlock" : "lock",
        moderation.locked ? "lock-open" : "lock",
        moderation.locked ? "Unlock thread" : "Lock thread"));
    }
    container.querySelectorAll("[data-repo-issue-comment-delete]").forEach((button) => {
      const id = button.getAttribute("data-repo-issue-comment-delete") || "";
      if (!id) return;
      button.insertAdjacentHTML("beforebegin", moderationButtonHtml(
        "data-repo-issue-moderation-hide", id, hidden.has(id) ? "eye" : "eye-off",
        hidden.has(id) ? "Unhide" : "Hide"));
    });
    const opening = container.querySelector("[data-repo-issue-opening] .border-b");
    if (opening) {
      opening.insertAdjacentHTML("beforeend", moderationButtonHtml(
        "data-repo-issue-moderation-hide", "body", hidden.has("body") ? "eye" : "eye-off",
        hidden.has("body") ? "Unhide" : "Hide"));
    }
    const originals = moderation.originals || {};
    container.querySelectorAll("[data-repo-issue-comment-delete]").forEach((button) => {
      const id = button.getAttribute("data-repo-issue-comment-delete") || "";
      const card = button.closest("[data-repo-issue-comment]");
      if (!originals[id] || !card) return;
      revealToggle(card.querySelector("[data-repo-issue-comment-text]"), originals[id]);
    });
    const openId = String(parsed.issueOpenEventId || "");
    if (originals[openId]) {
      revealToggle(container.querySelector("[data-repo-record-body]"), originals[openId]);
    }
    container.querySelectorAll("[data-repo-issue-moderation-hide], [data-repo-issue-moderation-lock]")
      .forEach((button) => button.addEventListener("click", async () => {
        const lock = button.getAttribute("data-repo-issue-moderation-lock");
        const target = button.getAttribute("data-repo-issue-moderation-hide") || "";
        const action = lock || (hidden.has(target) ? "unhide" : "hide");
        button.disabled = true;
        try {
          await postIssueModeration(repo, number, action, target);
          await refreshIssueModeration(container, repo, number);
        } catch (error) {
          button.disabled = false;
          button.title = error.message || "Not saved";
        }
      }));
    window.lucide?.createIcons();
  }

  function revealToggle(holder, original) {
    if (!holder || holder.querySelector("[data-repo-issue-moderation-original]")) return;
    holder.insertAdjacentHTML("beforeend", `<details data-repo-issue-moderation-original class="mt-2 text-[13px]"><summary class="cursor-pointer text-muted-foreground">Show the hidden text (moderators only)</summary><div class="mt-2 border-l-2 border-border pl-3">${renderRecordMarkdown(original)}</div></details>`);
  }

  async function refreshIssueModeration(container, repo, number) {
    if (!container?.isConnected) return;
    await loadRepoRecordDetail(repo, "issues", number);
  }

  function moderationRefusalText(code) {
    return ({
      account_required: "Sign in to post here.",
      account_blocked: "A moderator has blocked this account from posting here.",
      thread_locked: "This thread is locked. Only moderators can reply.",
      new_account_limit: "New accounts can only post a few times a day. Try again tomorrow.",
      review_queue_full: "Too many of your posts are already waiting for review.",
    })[String(code || "")] || "";
  }

  function moderationPendingMessage(result) {
    return String(result?.message || "Sent to the moderators. It appears once a moderator approves it.");
  }

  async function loadOrgModeration(root, name) {
    const holder = root.querySelector("[data-org-moderation]");
    const section = root.querySelector("[data-org-moderation-section]");
    if (!holder || !section) return;
    let data;
    try {
      data = await orgApiRequest("GET", `/api/orgs/${encodeURIComponent(name)}/moderation`);
    } catch (error) {
      if (["forbidden", "unauthorized", "http_403", "http_401"].includes(error.code)) {
        section.hidden = true;
        return;
      }
      holder.textContent = error.message;
      return;
    }
    section.hidden = false;
    holder.innerHTML = renderOrgModeration(data);
    window.lucide?.createIcons();
    wireOrgModeration(root, name, holder);
  }

  function moderationWhen(ms) {
    return ms ? formatTimeAgo(ms) : "";
  }

  function renderOrgModeration(data) {
    const row = (left, right) => `<div class="flex min-w-0 items-center gap-2 rounded-md border border-border bg-card px-3 py-2">${left}<span class="flex-1"></span>${right}</div>`;
    const empty = (text) => `<p class="text-xs text-muted-foreground">${escapeHtml(text)}</p>`;
    const queue = (data.queue || []).map((item) => `
      <div class="grid gap-2 rounded-md border border-border bg-card px-3 py-2">
        <div class="flex min-w-0 flex-wrap items-center gap-2 text-xs text-muted-foreground">
          <span class="font-medium text-foreground">${issuePersonNameHtml(item.author || "anonymous", "")}</span>
          <span>${item.kind === "thread" ? "new thread in" : `comment on #${escapeHtml(item.number)} in`}</span>
          <span class="font-mono">${escapeHtml(item.repo)}</span>
          <span>${escapeHtml(moderationWhen(item.at))}</span>
        </div>
        ${item.title ? `<p class="text-sm font-semibold text-foreground">${escapeHtml(item.title)}</p>` : ""}
        <div class="max-h-48 overflow-auto text-sm ${MD_BODY_CLASS}">${renderRecordMarkdown(item.body || "")}</div>
        <div class="flex flex-wrap gap-2">
          <button type="button" data-org-moderation-approve="${escapeHtml(item.id)}" class="fm-btn fm-btn-sm">Approve</button>
          <button type="button" data-org-moderation-reject="${escapeHtml(item.id)}" class="fm-btn fm-btn-sm fm-btn-danger">Reject</button>
        </div>
      </div>`).join("");
    const moderators = (data.moderators || []).map((item) => row(
      `<span class="truncate text-sm font-medium text-foreground">${issuePersonNameHtml(item.name, "")}</span><span class="text-[11px] text-muted-foreground">added by ${escapeHtml(item.by || "an admin")}</span>`,
      data.canManageModerators ? `<button type="button" data-org-moderation-unmod="${escapeHtml(item.name)}" class="fm-btn fm-btn-sm fm-btn-danger">Remove</button>` : "")).join("");
    const blocked = (data.blocked || []).map((item) => row(
      `<span class="truncate text-sm font-medium text-foreground">${issuePersonNameHtml(item.name, "")}</span><span class="text-[11px] text-muted-foreground">${item.hideContent ? "posts hidden" : "posts shown"}${item.reason ? ` · ${escapeHtml(item.reason)}` : ""}</span>`,
      `<button type="button" data-org-moderation-unblock="${escapeHtml(item.name)}" class="fm-btn fm-btn-sm">Unblock</button>`)).join("");
    const log = (data.log || []).slice(0, 50).map((item) => `<li><span class="text-foreground">${escapeHtml(item.actor || "")}</span> ${escapeHtml(String(item.action || "").replace(/_/g, " "))}${item.target ? ` <span class="text-foreground">${escapeHtml(item.target)}</span>` : ""}${item.detail?.repo ? ` · ${escapeHtml(item.detail.repo)}` : ""}${item.detail?.subject ? ` ${escapeHtml(item.detail.subject)}` : ""} <span>${escapeHtml(moderationWhen(item.at))}</span></li>`).join("");
    const policy = data.policy || {};
    const rules = [
      policy.firstPostApproval ? "a new account's first post waits for approval" : "",
      policy.accountRequired ? "only signed-in accounts can post" : "",
      Number(policy.newThreadsPerDay) ? `new accounts open at most ${Number(policy.newThreadsPerDay)} threads a day` : "",
      Number(policy.newCommentsPerDay) ? `and write at most ${Number(policy.newCommentsPerDay)} comments a day for ${Number(policy.newAccountDays)} days` : "",
    ].filter(Boolean).join(", ");
    return `
      ${rules ? `<p class="text-xs text-muted-foreground">On this server ${escapeHtml(rules)}.</p>` : ""}
      <p data-org-moderation-status class="mt-1 min-h-4 text-xs text-muted-foreground"></p>
      <h5 class="mt-3 text-xs font-semibold text-foreground">Waiting for review (${(data.queue || []).length})</h5>
      <div class="mt-2 grid gap-2">${queue || empty("Nothing is waiting.")}</div>
      <h5 class="mt-4 text-xs font-semibold text-foreground">Moderators</h5>
      <p class="mt-1 text-[11px] text-muted-foreground">Owners and admins always moderate. Moderators can approve posts, hide or delete any post, lock threads and block accounts here, but cannot change settings or members.</p>
      <div class="mt-2 grid gap-2">${moderators || empty("No extra moderators.")}</div>
      ${data.canManageModerators ? `<form data-org-moderation-add class="mt-2 flex flex-wrap gap-2"><input data-org-moderation-name placeholder="account name" autocomplete="off" class="${ORG_INPUT_CLASS} min-w-[12rem] flex-1" /><button type="submit" class="${ORG_BTN_SECONDARY}">Add moderator</button></form>` : ""}
      <h5 class="mt-4 text-xs font-semibold text-foreground">Blocked accounts</h5>
      <p class="mt-1 text-[11px] text-muted-foreground">A blocked account cannot post, comment, vote or react on this organization's repositories. Mirrored GitHub authors are not accounts: hide their posts on the issue instead.</p>
      <div class="mt-2 grid gap-2">${blocked || empty("Nobody is blocked.")}</div>
      <form data-org-moderation-block class="mt-2 flex flex-wrap items-center gap-2">
        <input data-org-moderation-block-name placeholder="account name" autocomplete="off" class="${ORG_INPUT_CLASS} min-w-[10rem] flex-1" />
        <input data-org-moderation-block-reason placeholder="reason (moderators only)" maxlength="280" autocomplete="off" class="${ORG_INPUT_CLASS} min-w-[10rem] flex-1" />
        <label class="inline-flex items-center gap-2 text-xs text-muted-foreground"><input type="checkbox" data-org-moderation-block-hide /> Hide their posts</label>
        <button type="submit" class="${ORG_BTN_SECONDARY}">Block</button>
      </form>
      <details class="mt-4"><summary class="cursor-pointer text-xs font-semibold text-foreground">Moderation log</summary>
        <ul class="mt-2 grid gap-1 text-xs text-muted-foreground">${log || "<li>No actions yet.</li>"}</ul>
      </details>`;
  }

  function wireOrgModeration(root, name, holder) {
    const path = `/api/orgs/${encodeURIComponent(name)}/moderation`;
    const status = (text, ok) => {
      const node = holder.querySelector("[data-org-moderation-status]");
      if (!node) return;
      node.textContent = text || "";
      node.className = "mt-1 min-h-4 text-xs " + (ok ? "text-muted-foreground" : "text-destructive");
    };
    const act = async (body) => {
      try {
        const data = await orgApiRequest("POST", path, body);
        holder.innerHTML = renderOrgModeration(data);
        window.lucide?.createIcons();
        wireOrgModeration(root, name, holder);
        status("Done.", true);
      } catch (error) {
        status(error.data?.message || error.message, false);
      }
    };
    const on = (selector, fn) => holder.querySelectorAll(selector).forEach((node) =>
      node.addEventListener("click", () => fn(node)));
    on("[data-org-moderation-approve]", (node) =>
      act({ action: "approve", id: Number(node.getAttribute("data-org-moderation-approve")) }));
    on("[data-org-moderation-reject]", (node) =>
      act({ action: "reject", id: Number(node.getAttribute("data-org-moderation-reject")) }));
    on("[data-org-moderation-unmod]", (node) =>
      act({ action: "remove_moderator", name: node.getAttribute("data-org-moderation-unmod") }));
    on("[data-org-moderation-unblock]", (node) =>
      act({ action: "unblock", name: node.getAttribute("data-org-moderation-unblock") }));
    holder.querySelector("[data-org-moderation-add]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const value = holder.querySelector("[data-org-moderation-name]")?.value.trim().toLowerCase();
      if (value) act({ action: "add_moderator", name: value });
    });
    holder.querySelector("[data-org-moderation-block]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const value = holder.querySelector("[data-org-moderation-block-name]")?.value.trim().toLowerCase();
      if (!value) return;
      act({
        action: "block",
        name: value,
        reason: holder.querySelector("[data-org-moderation-block-reason]")?.value.trim() || "",
        hideContent: Boolean(holder.querySelector("[data-org-moderation-block-hide]")?.checked),
      });
    });
  }

  const PINGS_PAGE_SIZE = 40;
  const pingsView = {
    filter: "all",
    older: [],
    expanded: new Set(),
    loadingMore: false,
    moreFailed: false,
    wired: false,
  };

  function pingsPageItems() {
    const seen = new Set();
    const items = [];
    for (const item of [...state.notifications, ...pingsView.older]) {
      const id = String(item?.id || "");
      if (!id || seen.has(id)) continue;
      seen.add(id);
      items.push(item);
    }
    return items.sort((a, b) => (Number(b.ts) || 0) - (Number(a.ts) || 0));
  }

  function pingsPageVisible(items) {
    return pingsView.filter === "unread" ? items.filter((item) => !item.readAt) : items;
  }

  function pingsPageHasMore(items) {
    const total = Number(state.notificationTotal) || 0;
    return items.length < total;
  }

  function pingsBlankslateHtml(icon, title, desc, action = "") {
    return `
      <div class="fm-blankslate">
        <i data-lucide="${icon}" aria-hidden="true"></i>
        <p class="fm-blankslate-title">${escapeHtml(title)}</p>
        <p class="fm-blankslate-desc">${escapeHtml(desc)}</p>
        ${action}
      </div>
    `;
  }

  function renderPingsFilter() {
    $$("[data-pings-filter]").forEach((button) => {
      button.setAttribute("aria-pressed", button.dataset.pingsFilter === pingsView.filter ? "true" : "false");
    });
  }

  function renderPingsPage() {
    const list = $("[data-pings-list]");
    if (!list) return;
    renderPingsFilter();
    const signedIn = Boolean(state.session?.nodeName);
    const controls = $("[data-pings-account-controls]");
    const toolbar = $("[data-pings-toolbar]");
    const more = $("[data-pings-more]");
    if (controls) controls.hidden = !signedIn;
    if (toolbar) toolbar.hidden = !signedIn;
    if (!signedIn) {
      if (more) more.hidden = true;
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      list.innerHTML = pingsBlankslateHtml(
        "bell",
        "Sign in to see your pings",
        "Mentions, pull requests, assignments, shares and host status changes for your account show up here.",
        `<a href="/login?next=${next}" class="fm-btn fm-btn-sm mt-3">Log in</a>`,
      );
      window.lucide?.createIcons();
      return;
    }
    if (!state.notificationsLoaded) {
      if (more) more.hidden = true;
      list.innerHTML = '<p class="px-4 py-6 text-sm text-muted-foreground"><span class="fm-spinner" aria-hidden="true"></span>Loading pings...</p>';
      return;
    }
    const items = pingsPageItems();
    const visible = pingsPageVisible(items);
    const markAll = $("[data-pings-mark-all]");
    if (markAll) markAll.disabled = !((Number(state.notificationUnread) || 0) > 0 || items.some((item) => !item.readAt));
    if (state.notificationsFailed && !items.length) {
      list.innerHTML = pingsBlankslateHtml("triangle-alert", "Pings could not be loaded", "Check your connection and reload the page.");
    } else if (!visible.length) {
      list.innerHTML = pingsView.filter === "unread"
        ? pingsBlankslateHtml("check-check", "You're all caught up", "No unread pings. Switch to All to see the ones you have read.")
        : pingsBlankslateHtml("bell", "No pings yet", "Mentions, pull requests, assignments, shares, bounties, releases and host status changes will appear here.");
    } else {
      list.innerHTML = `<ul class="divide-y divide-border">${visible.map((item) => notificationRowHtml(item, {
        expanded: pingsView.expanded.has(String(item.id || "")),
      })).join("")}</ul>`;
    }
    if (more) {
      more.hidden = !pingsPageHasMore(items);
      const button = more.querySelector("[data-pings-load-more]");
      if (button) {
        button.disabled = pingsView.loadingMore;
        button.textContent = pingsView.loadingMore
          ? "Loading..."
          : pingsView.moreFailed ? "Could not load more. Try again" : "Load more";
      }
    }
    window.lucide?.createIcons();
  }

  async function loadMorePings() {
    const node = state.session?.nodeName || "";
    if (!node || pingsView.loadingMore) return;
    pingsView.loadingMore = true;
    pingsView.moreFailed = false;
    renderPingsPage();
    try {
      const offset = pingsPageItems().length;
      const data = await fetchJson(`/api/notifications?node=${encodeURIComponent(node)}&limit=${PINGS_PAGE_SIZE}&offset=${offset}`);
      const page = Array.isArray(data.notifications) ? data.notifications : [];
      pingsView.older = pingsView.older.concat(page);
      if (Number(data.total)) state.notificationTotal = Number(data.total);
      if (!page.length) state.notificationTotal = pingsPageItems().length;
    } catch (_) {
      pingsView.moreFailed = true;
    }
    pingsView.loadingMore = false;
    renderPingsPage();
  }

  function markPingsReadLocally(ids, all = false) {
    const now = Date.now();
    const wanted = new Set(ids.map(String));
    for (const item of [...state.notifications, ...pingsView.older]) {
      if (!item.readAt && (all || wanted.has(String(item.id || "")))) item.readAt = now;
    }
  }

  async function markPingsRead(ids, all = false) {
    const fresh = all
      ? (Number(state.notificationUnread) || 0)
      : pingsPageItems().filter((item) => ids.includes(String(item.id || "")) && !item.readAt).length;
    markPingsReadLocally(ids, all);
    state.notificationUnread = all ? 0 : Math.max(0, (Number(state.notificationUnread) || 0) - fresh);
    renderNotificationBadge();
    renderPingsPage();
    await markNotificationsRead(all ? [] : ids, all);
    markPingsReadLocally(ids, all);
    renderPingsPage();
  }

  function setPingsFilter(filter) {
    pingsView.filter = filter === "unread" ? "unread" : "all";
    const url = new URL(location.href);
    if (pingsView.filter === "unread") url.searchParams.set("filter", "unread");
    else url.searchParams.delete("filter");
    history.replaceState(history.state, "", `${url.pathname}${url.search}${url.hash}`);
    renderPingsPage();
  }

  function focusPingControl(selector) {
    $(selector)?.focus();
  }

  async function handlePingsPageClick(event) {
    const page = event.target.closest("[data-pings-page]");
    if (!page) return;
    const filter = event.target.closest("[data-pings-filter]");
    if (filter) {
      setPingsFilter(filter.dataset.pingsFilter);
      return;
    }
    if (event.target.closest("[data-pings-mark-all]")) {
      await markPingsRead([], true);
      return;
    }
    if (event.target.closest("[data-pings-load-more]")) {
      await loadMorePings();
      return;
    }
    const markRead = event.target.closest("[data-ping-mark-read]");
    if (markRead) {
      const id = markRead.dataset.pingMarkRead || "";
      await markPingsRead([id]);
      focusPingControl(`[data-ping-row-toggle="${CSS.escape(id)}"]`);
      return;
    }
    const toggle = event.target.closest("[data-ping-row-toggle]");
    if (toggle) {
      const id = toggle.dataset.pingRowToggle || "";
      if (pingsView.expanded.has(id)) pingsView.expanded.delete(id);
      else pingsView.expanded.add(id);
      renderPingsPage();
      focusPingControl(`[data-ping-row-toggle="${CSS.escape(id)}"]`);
      return;
    }
    const open = event.target.closest("[data-notification-open]");
    if (open) {
      const id = open.dataset.notificationOpen || "";
      const item = pingsPageItems().find((candidate) => String(candidate.id || "") === id);
      if (!item) return;
      if (open.tagName === "BUTTON") {
        if (pingsView.expanded.has(id)) pingsView.expanded.delete(id);
        else pingsView.expanded.add(id);
        renderPingsPage();
        focusPingControl(`button[data-notification-open="${CSS.escape(id)}"]`);
        if (!item.readAt) await markPingsRead([id]);
        return;
      }
      if (!item.readAt) setTimeout(() => { void markPingsRead([id]); }, 0);
    }
  }

  function initPingsPage() {
    if (!$("[data-pings-page]")) return;
    const bell = $("#notificationToggle");
    bell?.setAttribute("aria-current", "page");
    pingsView.filter = new URLSearchParams(location.search).get("filter") === "unread" ? "unread" : "all";
    if (!pingsView.wired) {
      pingsView.wired = true;
      document.addEventListener("click", (event) => { void handlePingsPageClick(event); });
    }
    renderPingsPage();
    renderPingPause();
  }

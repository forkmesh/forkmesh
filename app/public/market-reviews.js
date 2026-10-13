// Market package ratings and reviews page (/market-reviews).
//
// ?market=<forkmesh|forkmesh-community>&package=<id>[&view=feedback]
// Without a package it lists the packages the signed-in account maintains.
// API: docs/MARKETPLACE_RATINGS.md. Every untrusted string is written with
// textContent; nothing from a review becomes markup.
(() => {
  "use strict";
  const $ = (selector, root = document) => root.querySelector(selector);
  const params = new URLSearchParams(location.search);
  const market = (params.get("market") || "forkmesh").toLowerCase();
  const pkg = (params.get("package") || "").toLowerCase();
  const CATEGORY_LABELS = {
    bug: "Bug",
    "missing-feature": "Missing feature",
    docs: "Docs",
    performance: "Performance",
    ux: "UX",
    other: "Other",
    uncategorized: "Uncategorized",
  };
  const GATE_COPY = {
    sign_in_required: "Sign in to rate this package.",
    email_not_verified: "Confirm your email address to rate packages.",
    maintainer_cannot_rate: "You maintain this package, so you can't rate it. You can reply to reviews below.",
    package_use_required:
      "Only people who installed or used this package can rate it. Install it while signed in, then come back.",
  };
  const ERROR_COPY = {
    review_required: "Ratings under 5 stars need a short review of what to improve.",
    review_invalid: "Please fix the highlighted fields.",
    rating_rate_limited: "You've changed ratings a lot recently. Please try again later.",
    email_not_verified: GATE_COPY.email_not_verified,
    package_use_required: GATE_COPY.package_use_required,
    maintainer_cannot_rate: GATE_COPY.maintainer_cannot_rate,
    sign_in_required: GATE_COPY.sign_in_required,
  };

  let state = {
    data: null,
    sort: "newest",
    stars: 0,
    page: 1,
    reviews: [],
    form: null,
  };

  function readSession() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!session?.nodeName || session.kind === "node") return null;
      return session;
    } catch (_) {
      return null;
    }
  }

  async function api(path, options = {}) {
    const session = readSession();
    const headers = { accept: "application/json", ...(options.headers || {}) };
    if (session?.sessionToken && session.sessionToken !== "cookie") {
      headers.authorization = `Bearer ${session.sessionToken}`;
    }
    if (options.body !== undefined) headers["content-type"] = "application/json";
    const response = await fetch(path, {
      method: options.method || "GET",
      headers,
      credentials: "same-origin",
      cache: "no-store",
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
    });
    let body = {};
    try { body = await response.json(); } catch (_) {}
    return { ok: response.ok, status: response.status, body };
  }

  const base = () => `/api/market/reviews/${encodeURIComponent(market)}/${encodeURIComponent(pkg)}`;
  const itemPath = (id, action) => `/api/market/reviews/items/${encodeURIComponent(id)}/${action}`;

  function el(tag, attrs = {}, text) {
    const node = document.createElement(tag);
    Object.entries(attrs).forEach(([key, value]) => {
      if (value === false || value === undefined || value === null) return;
      if (key === "className") node.className = value;
      else node.setAttribute(key, value === true ? "" : String(value));
    });
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function setStatus(node, text, error = false) {
    if (!node) return;
    node.textContent = text || "";
    node.dataset.error = error ? "true" : "false";
  }

  function date(ms) {
    if (!ms) return "";
    try {
      return new Date(ms).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" });
    } catch (_) {
      return "";
    }
  }

  function starsText(value) {
    const count = Math.max(0, Math.min(5, Number(value) || 0));
    return "★".repeat(count) + "☆".repeat(5 - count);
  }

  function starsBadge(value) {
    const node = el("span", { className: "mr-stars", role: "img", "aria-label": `${value} out of 5 stars` });
    node.style.setProperty("--rating", String(value));
    node.append(
      el("span", { "aria-hidden": "true" }, "★★★★★"),
      el("span", { className: "mr-stars-fill", "aria-hidden": "true" }, "★★★★★"),
    );
    return node;
  }

  // -- Overview -----------------------------------------------------------------

  function renderSummary(summary) {
    const count = Number(summary?.count) || 0;
    const average = Number(summary?.average) || 0;
    $("[data-average]").textContent = count ? average.toFixed(1) : "–";
    const stars = $("[data-average-stars]");
    stars.style.setProperty("--rating", String(count ? average : 0));
    stars.setAttribute(
      "aria-label",
      count ? `${average.toFixed(1)} out of 5 stars` : "No ratings yet",
    );
    $("[data-count]").textContent = count
      ? `${count.toLocaleString()} rating${count === 1 ? "" : "s"}`
      : "No ratings yet";
    const histogram = $("[data-histogram]");
    histogram.replaceChildren();
    for (let star = 5; star >= 1; star -= 1) {
      const n = Number(summary?.histogram?.[String(star)]) || 0;
      const share = count ? Math.round((n / count) * 100) : 0;
      const button = el("button", {
        type: "button",
        "aria-pressed": state.stars === star ? "true" : "false",
        "aria-label": `${star} star${star === 1 ? "" : "s"}: ${n} rating${n === 1 ? "" : "s"} (${share}%). Show only these reviews.`,
      });
      const bar = el("span", { className: "mr-bar", "aria-hidden": "true" });
      const fill = el("span");
      fill.style.width = `${share}%`;
      bar.append(fill);
      button.append(
        el("span", { "aria-hidden": "true" }, `${star} ★`),
        bar,
        el("span", { "aria-hidden": "true" }, String(n)),
      );
      button.addEventListener("click", () => {
        state.stars = state.stars === star ? 0 : star;
        loadReviews(true);
      });
      histogram.append(button);
    }
    $("[data-clear-filter]").hidden = !state.stars;
  }

  // -- Rating form ------------------------------------------------------------

  function renderRateCard(data) {
    const viewer = data.viewer || {};
    const gate = $("[data-rate-gate]");
    const form = $("[data-review-form]");
    if (!viewer.canRate) {
      form.hidden = true;
      gate.hidden = false;
      gate.replaceChildren(document.createTextNode(GATE_COPY[viewer.reason] || "Rating is unavailable right now."));
      if (viewer.reason === "sign_in_required") {
        gate.append(" ", el("a", { href: `/login?next=${encodeURIComponent(location.pathname + location.search)}` }, "Sign in"));
      } else if (viewer.reason === "email_not_verified") {
        gate.append(" ", el("a", { href: "/verify-pending" }, "Confirm your email"));
      }
      return;
    }
    gate.hidden = true;
    form.hidden = false;
    if (!state.form) {
      state.form = window.ForkMeshReviewForm.mount(form, {
        rules: data.rules,
        onSubmit: submitRating,
        onChange: (stars, expanding) => {
          if (expanding) $("[data-field=\"problem\"]")?.scrollIntoView?.({ block: "nearest" });
        },
      });
    }
    if (viewer.review) {
      state.form.fill(viewer.review);
      $("[data-review-submit]").textContent = "Update rating";
      $("[data-history-toggle]").hidden = !(viewer.review.revision > 1);
      if (viewer.review.hidden) {
        setStatus($("[data-review-status]"), "An administrator hid your review from the public list.", true);
      }
    }
  }

  async function submitRating(payload) {
    const status = $("[data-review-status]");
    const button = $("[data-review-submit]");
    button.disabled = true;
    setStatus(status, "Saving…");
    const result = await api(base(), { method: "POST", body: payload });
    button.disabled = false;
    if (!result.ok) {
      if (result.body?.fields) state.form.showServerErrors(result.body.fields);
      setStatus(status, ERROR_COPY[result.body?.error] || `Could not save your rating (${result.body?.error || result.status}).`, true);
      return null;
    }
    setStatus(status, result.body.unchanged ? "No changes to save." : "Thanks! Your rating is saved.");
    button.textContent = "Update rating";
    if (result.body.summary) renderSummary(result.body.summary);
    await loadReviews(true);
    return result.body;
  }

  async function toggleHistory() {
    const list = $("[data-history]");
    const own = state.data?.viewer?.review;
    if (!own) return;
    if (!list.hidden) {
      list.hidden = true;
      return;
    }
    const result = await api(itemPath(own.id, "history"));
    list.replaceChildren();
    (result.body?.revisions || []).forEach((revision) => {
      list.append(el("li", {}, `${date(revision.at)} · ${starsText(revision.stars)}${revision.improvement ? ` · ${revision.improvement.slice(0, 140)}` : ""}`));
    });
    list.hidden = false;
  }

  // -- Reviews list -----------------------------------------------------------

  function section(parent, heading, text) {
    if (!text) return;
    parent.append(el("h3", {}, heading), el("p", {}, text));
  }

  function renderReview(review, viewer, { reportsView = false } = {}) {
    const item = el("li", { className: "mr-review", id: `review-${review.id}` });
    const head = el("div", { className: "mr-review-head" });
    head.append(
      starsBadge(review.stars),
      el("strong", {}, review.reviewer.displayName || review.reviewer.name),
      el("span", { className: "mr-muted" }, `@${review.reviewer.name}`),
      el("time", { className: "mr-muted", datetime: new Date(review.updatedAt || review.createdAt).toISOString() }, date(review.updatedAt || review.createdAt)),
    );
    if (review.edited) head.append(el("span", { className: "mr-chip" }, "Edited"));
    if (review.verifiedUse) head.append(el("span", { className: "mr-chip mr-chip-good" }, "Verified install"));
    if (review.category) head.append(el("span", { className: "mr-chip" }, CATEGORY_LABELS[review.category] || review.category));
    if (review.version) head.append(el("span", { className: "mr-chip" }, review.version));
    if (review.addressedIn) head.append(el("span", { className: "mr-chip mr-chip-good" }, `Addressed in ${review.addressedIn}`));
    if (review.hidden) head.append(el("span", { className: "mr-chip mr-chip-warn" }, "Hidden"));
    item.append(head);
    section(item, "What didn't work", review.problem);
    section(item, "How to improve", review.improvement);
    section(item, "Context", review.context);
    section(item, review.stars < 5 ? "Also" : "Review", review.comment);
    if (review.reply) {
      const reply = el("div", { className: "mr-reply-block" });
      reply.append(
        el("strong", {}, `Reply from @${review.reply.by}`),
        el("span", { className: "mr-muted" }, ` · ${date(review.reply.at)}`),
        el("p", {}, review.reply.text),
      );
      if (review.editedAfterReply) reply.append(el("p", { className: "mr-muted" }, "The review was edited after this reply."));
      item.append(reply);
    }
    if (reportsView && Array.isArray(review.reports)) {
      const reports = el("ul", { className: "mr-muted" });
      review.reports.forEach((report) => reports.append(el("li", {}, `@${report.by}: ${report.reason}`)));
      item.append(el("h3", {}, `Reports (${review.reportCount || review.reports.length})`), reports);
    }
    item.append(reviewActions(review, viewer));
    return item;
  }

  function reviewActions(review, viewer) {
    const actions = el("div", { className: "mr-review-actions" });
    const own = viewer?.name && viewer.name === review.reviewer.name;
    const signedIn = Boolean(viewer?.signedIn);
    if (signedIn && !own) {
      const helpful = el("button", {
        type: "button",
        className: "mr-link-button",
        "aria-pressed": review.viewerFoundHelpful ? "true" : "false",
      }, `Helpful (${review.helpful})`);
      helpful.addEventListener("click", async () => {
        const result = await api(itemPath(review.id, "helpful"), {
          method: review.viewerFoundHelpful ? "DELETE" : "POST",
          body: {},
        });
        if (result.ok) {
          review.viewerFoundHelpful = result.body.viewerFoundHelpful;
          review.helpful = result.body.helpful;
          helpful.textContent = `Helpful (${review.helpful})`;
          helpful.setAttribute("aria-pressed", review.viewerFoundHelpful ? "true" : "false");
        }
      });
      actions.append(helpful);
      const report = el("button", { type: "button", className: "mr-link-button", "aria-expanded": "false" }, "Report");
      report.addEventListener("click", () => toggleInline(actions, report, () => reportForm(review)));
      actions.append(report);
    } else if (!signedIn) {
      actions.append(el("span", { className: "mr-muted" }, `${review.helpful} found this helpful`));
    }
    if (viewer?.isMaintainer || viewer?.isAdmin) {
      const reply = el("button", { type: "button", className: "mr-link-button", "aria-expanded": "false" }, review.reply ? "Edit reply" : "Reply");
      reply.addEventListener("click", () => toggleInline(actions, reply, () => replyForm(review)));
      actions.append(reply);
      if (review.addressedIn) {
        const reopen = el("button", { type: "button", className: "mr-link-button" }, "Mark not addressed");
        reopen.addEventListener("click", async () => {
          const result = await api(itemPath(review.id, "addressed"), { method: "DELETE", body: {} });
          if (result.ok) refreshAfterAction();
        });
        actions.append(reopen);
      }
    }
    if (viewer?.isAdmin) {
      const hide = el("button", { type: "button", className: "mr-link-button" }, review.hidden ? "Unhide" : "Hide");
      hide.addEventListener("click", async () => {
        let reason = "";
        if (!review.hidden) {
          reason = window.prompt("Why hide this review? (kept in the moderation record)") || "";
          if (!reason.trim()) return;
        }
        const result = await api(itemPath(review.id, "moderation"), {
          method: "POST",
          body: { hidden: !review.hidden, reason },
        });
        if (result.ok) refreshAfterAction();
      });
      actions.append(hide);
    }
    return actions;
  }

  function toggleInline(actions, trigger, build) {
    const open = actions.nextElementSibling?.classList?.contains("mr-inline-form");
    if (open) actions.nextElementSibling.remove();
    if (open && trigger.getAttribute("aria-expanded") === "true") {
      trigger.setAttribute("aria-expanded", "false");
      return;
    }
    actions.querySelectorAll("[aria-expanded]").forEach((node) => node.setAttribute("aria-expanded", "false"));
    trigger.setAttribute("aria-expanded", "true");
    const form = build();
    actions.after(form);
    form.querySelector("textarea, input")?.focus();
  }

  function replyForm(review) {
    const form = el("form", { className: "mr-inline-form mr-reply" });
    const id = `reply-${review.id}`;
    const text = el("textarea", { id, maxlength: "2000", required: true });
    text.value = review.reply?.text || "";
    const version = el("input", { id: `${id}-version`, type: "text", placeholder: "e.g. 1.4", maxlength: "64" });
    version.value = review.addressedIn || "";
    const status = el("p", { className: "mr-status", role: "status" });
    form.append(
      el("label", { for: id }, "Public reply"),
      text,
      el("label", { for: `${id}-version` }, "Addressed in version (optional)"),
      version,
      el("button", { type: "submit", className: "mr-button mr-button-primary" }, "Post reply"),
      status,
    );
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const result = await api(itemPath(review.id, "reply"), {
        method: "POST",
        body: { text: text.value, addressedIn: version.value.trim() },
      });
      if (!result.ok) {
        setStatus(status, `Could not post the reply (${result.body?.error || result.status}).`, true);
        return;
      }
      refreshAfterAction();
    });
    return form;
  }

  function reportForm(review) {
    const form = el("form", { className: "mr-inline-form mr-report" });
    const id = `report-${review.id}`;
    const reason = el("textarea", { id, maxlength: "500", required: true });
    const status = el("p", { className: "mr-status", role: "status" });
    form.append(
      el("label", { for: id }, "Why should an administrator look at this review?"),
      reason,
      el("button", { type: "submit", className: "mr-button" }, "Send report"),
      status,
    );
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const result = await api(itemPath(review.id, "report"), { method: "POST", body: { reason: reason.value } });
      setStatus(status, result.ok ? "Thanks, an administrator will review it." : `Could not send the report (${result.body?.error || result.status}).`, !result.ok);
      if (result.ok) reason.disabled = true;
    });
    return form;
  }

  async function loadReviews(reset) {
    if (reset) {
      state.page = 1;
      state.reviews = [];
    }
    const query = new URLSearchParams({ sort: state.sort, page: String(state.page), limit: "10" });
    if (state.stars) query.set("stars", String(state.stars));
    const status = $("[data-reviews-status]");
    setStatus(status, "Loading reviews…");
    const result = await api(`${base()}?${query}`);
    if (!result.ok) {
      setStatus(status, result.status === 404 ? "This package is not in the Market." : "Could not load reviews.", true);
      return null;
    }
    const data = result.body;
    state.data = data;
    state.reviews = reset ? data.reviews : state.reviews.concat(data.reviews);
    renderSummary(data.summary);
    const list = $("[data-reviews]");
    list.replaceChildren(...state.reviews.map((review) => renderReview(review, data.viewer)));
    setStatus(status, state.reviews.length ? "" : state.stars ? `No ${state.stars}-star reviews yet.` : "No reviews yet. Be the first to rate it.");
    $("[data-load-more]").hidden = !data.hasMore;
    return data;
  }

  function refreshAfterAction() {
    loadReviews(true);
    if (!$("[data-panel=\"feedback\"]").hidden) loadFeedback();
    if (!$("[data-panel=\"reports\"]").hidden) loadReports();
  }

  // -- Owner feedback view -------------------------------------------------------

  async function loadFeedback() {
    const query = new URLSearchParams({
      category: $("[data-feedback-category]").value,
      status: $("[data-feedback-status]").value,
      sort: $("[data-feedback-sort]").value,
      limit: "200",
    });
    const status = $("[data-feedback-status-text]");
    setStatus(status, "Loading feedback…");
    const result = await api(`${base()}/feedback?${query}`);
    if (!result.ok) {
      setStatus(status, `Could not load feedback (${result.body?.error || result.status}).`, true);
      return;
    }
    const data = result.body;
    const counts = $("[data-category-counts]");
    counts.replaceChildren(
      el("span", { className: "mr-chip mr-chip-warn" }, `Open: ${data.statuses.open}`),
      el("span", { className: "mr-chip mr-chip-good" }, `Addressed: ${data.statuses.addressed}`),
      ...Object.entries(data.categories)
        .filter(([, n]) => n > 0)
        .map(([key, n]) => el("span", { className: "mr-chip" }, `${CATEGORY_LABELS[key] || key}: ${n}`)),
    );
    const rows = $("[data-feedback-rows]");
    rows.replaceChildren(...data.items.map((item) => {
      const row = el("tr", { id: `feedback-${item.id}` });
      const stars = el("td");
      stars.append(starsBadge(item.stars));
      const text = el("td");
      text.append(
        el("p", {}, item.improvement),
        el("p", { className: "mr-muted" }, `Problem: ${item.problem}`),
      );
      if (item.context) text.append(el("p", { className: "mr-muted" }, `Context: ${item.context}`));
      text.append(el("p", { className: "mr-muted" }, `@${item.reviewer.name} · ${date(item.updatedAt)}${item.version ? ` · ${item.version}` : ""}`));
      const category = el("td", {}, CATEGORY_LABELS[item.category] || "Uncategorized");
      const statusCell = el("td");
      if (item.addressedIn) {
        statusCell.append(el("span", { className: "mr-chip mr-chip-good" }, `Addressed in ${item.addressedIn}`));
      } else {
        const form = el("form", { className: "mr-inline-form" });
        const input = el("input", { type: "text", placeholder: "e.g. 1.4", "aria-label": "Addressed in version", maxlength: "64" });
        form.append(input, el("button", { type: "submit", className: "mr-button" }, "Mark addressed"));
        form.addEventListener("submit", async (event) => {
          event.preventDefault();
          const result = await api(itemPath(item.id, "addressed"), { method: "POST", body: { version: input.value.trim() } });
          if (result.ok) loadFeedback();
          else input.setAttribute("aria-invalid", "true");
        });
        statusCell.append(form);
      }
      row.append(stars, text, category, statusCell);
      return row;
    }));
    setStatus(status, data.items.length ? "" : "Nothing matches these filters.");
  }

  async function loadReports() {
    const status = $("[data-reports-status]");
    setStatus(status, "Loading reports…");
    const result = await api("/api/market/reviews/reports");
    if (!result.ok) {
      setStatus(status, "Could not load reports.", true);
      return;
    }
    const list = $("[data-reports]");
    list.replaceChildren(...(result.body.items || []).map((review) =>
      renderReview(review, state.data?.viewer, { reportsView: true })));
    setStatus(status, result.body.items?.length ? "" : "No open reports.");
  }

  function selectTab(name) {
    document.querySelectorAll("[data-tab]").forEach((tab) => {
      const active = tab.dataset.tab === name;
      tab.setAttribute("aria-selected", active ? "true" : "false");
      tab.setAttribute("tabindex", active ? "0" : "-1");
    });
    document.querySelectorAll("[data-panel]").forEach((panel) => {
      panel.hidden = panel.dataset.panel !== name;
    });
    if (name === "feedback") loadFeedback();
    if (name === "reports") loadReports();
  }

  function wireTabs() {
    const tabs = Array.from(document.querySelectorAll("[data-tab]"));
    tabs.forEach((tab) => {
      tab.addEventListener("click", () => selectTab(tab.dataset.tab));
      tab.addEventListener("keydown", (event) => {
        const visible = tabs.filter((node) => !node.hidden);
        const index = visible.indexOf(tab);
        let next = null;
        if (event.key === "ArrowRight") next = visible[(index + 1) % visible.length];
        if (event.key === "ArrowLeft") next = visible[(index - 1 + visible.length) % visible.length];
        if (event.key === "Home") next = visible[0];
        if (event.key === "End") next = visible[visible.length - 1];
        if (!next) return;
        event.preventDefault();
        next.focus();
        selectTab(next.dataset.tab);
      });
    });
  }

  // -- Maintained packages (no ?package=) ----------------------------------------

  async function loadMaintained() {
    const card = $("[data-maintained]");
    card.hidden = false;
    $("[data-package-title]").textContent = "Package reviews";
    const status = $("[data-maintained-status]");
    if (!readSession()) {
      $("[data-package-subtitle]").textContent = "Pick a package in the Market to read and write reviews.";
      setStatus(status, "Sign in to see feedback for the packages you maintain.");
      return;
    }
    $("[data-package-subtitle]").textContent = "Improvement feedback for the packages you maintain.";
    const result = await api("/api/market/reviews/maintained");
    if (!result.ok) {
      setStatus(status, "Could not load your packages.", true);
      return;
    }
    const list = $("[data-maintained-list]");
    const packages = result.body.packages || [];
    list.replaceChildren(...packages.map((entry) => {
      const item = el("li");
      const link = el("a", {
        href: `/market-reviews?market=${encodeURIComponent(entry.market)}&package=${encodeURIComponent(entry.package)}&view=feedback`,
      }, entry.packageName);
      const count = Number(entry.summary?.count) || 0;
      item.append(
        link,
        el("span", { className: "mr-muted" }, count
          ? ` · ${Number(entry.summary.average).toFixed(1)} ★ (${count}) · ${entry.openFeedback} open`
          : " · no ratings yet"),
      );
      return item;
    }));
    setStatus(status, packages.length ? "" : "You don't maintain any Market packages.");
  }

  async function boot() {
    const marketLink = $("[data-market-link]");
    if (marketLink && location.hostname.endsWith("localhost")) marketLink.href = "/market";
    if (!pkg) {
      await loadMaintained();
      return;
    }
    $("[data-package-view]").hidden = false;
    $("[data-sort]").addEventListener("change", (event) => {
      state.sort = event.target.value;
      loadReviews(true);
    });
    $("[data-clear-filter]").addEventListener("click", () => {
      state.stars = 0;
      loadReviews(true);
    });
    $("[data-load-more]").addEventListener("click", () => {
      state.page += 1;
      loadReviews(false);
    });
    $("[data-history-toggle]").addEventListener("click", toggleHistory);
    ["[data-feedback-category]", "[data-feedback-status]", "[data-feedback-sort]"].forEach((selector) =>
      $(selector).addEventListener("change", loadFeedback));
    wireTabs();
    const data = await loadReviews(true);
    if (!data) {
      $("[data-package-title]").textContent = pkg;
      $("[data-package-subtitle]").textContent = "This package is not in the Market.";
      return;
    }
    document.title = `${data.packageName} ratings and reviews · ForkMesh Market`;
    $("[data-package-title]").textContent = data.packageName;
    $("[data-package-subtitle]").textContent = market === "forkmesh"
      ? "ForkMesh Market package"
      : market === "forkmesh-apps" ? "App Directory app" : "Community Market package";
    renderRateCard(data);
    const viewer = data.viewer || {};
    const owner = viewer.isMaintainer || viewer.isAdmin;
    $("[data-tab=\"feedback\"]").hidden = !owner;
    $("[data-tab=\"reports\"]").hidden = !viewer.isAdmin;
    if (owner && params.get("view") === "feedback") selectTab("feedback");
    if (location.hash.startsWith("#review-")) {
      document.getElementById(location.hash.slice(1))?.scrollIntoView?.();
    }
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();

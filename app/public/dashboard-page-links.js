// 09-links.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["links"] = (core) => {
  const { $, copyTextToClipboard, escapeHtml, fetchJson, formatCount, loadingHtml, publicProfileUrl, relativeTimeLabel, state } = core;
  const LINK_SOLANA_ADDRESS_RE = /^[1-9A-HJ-NP-Za-km-z]{32,44}$/;
  const LINK_CHANNEL_LABELS = {
    article: "Article",
    community: "Community",
    social: "Social post",
    video: "Video",
    other: "Other",
  };
  const LINK_ERROR_TEXT = {
    invalid_session: "Your session expired. Log in again to submit a link.",
    consent_required: "Tick the consent box before publishing.",
    invalid_public_url: "Use a public HTTPS URL without credentials or a custom port.",
    invalid_channel: "Pick one of the listed channels.",
    link_already_submitted: "You already submitted this link.",
    account_link_limit: "This account has reached the limit of 20 links.",
    origin_not_allowed: "Open this page on forkmesh.com and try again.",
  };

  function linkAccountName() {
    const session = state.session;
    if (!session?.sessionToken || !session?.nodeName) return "";
    return String(session.nodeName).toLowerCase();
  }

  function linkErrorText(error, fallback) {
    return LINK_ERROR_TEXT[String(error?.message || "")] || fallback;
  }

  function linkSafeUrl(value) {
    try {
      const url = new URL(String(value || ""));
      if (url.protocol !== "https:" || url.username || url.password) return "";
      return url.href;
    } catch (_) {
      return "";
    }
  }

  function linkRewardText(value) {
    return `${Math.max(0, Number(value) || 0).toFixed(5)} SOL`;
  }

  function linkRowHtml(link, index) {
    const url = linkSafeUrl(link.url);
    const title = String(link.title || link.host || "Public link");
    const submitter = String(link.submittedBy || "member").toLowerCase();
    const you = linkAccountName() && submitter === linkAccountName();
    const score = Math.max(0, Math.min(100, Math.round(Number(link.score) || 0)));
    const low = Math.max(0, Number(link.potentialTraffic?.low) || 0);
    const high = Math.max(low, Number(link.potentialTraffic?.high) || 0);
    const reward = Math.max(0, Number(link.rewardSol) || 0);
    const wallet = LINK_SOLANA_ADDRESS_RE.test(String(link.walletAddress || "")) ? String(link.walletAddress) : "";
    const paid = String(link.paymentStatus || "") === "paid";
    const channel = LINK_CHANNEL_LABELS[link.channel] || "Other";
    const when = relativeTimeLabel(link.createdAt);
    const payUri = wallet
      ? `solana:${wallet}?amount=${reward.toFixed(8)}&label=${encodeURIComponent("ForkMesh link reward")}`
      : "";
    const payAction = paid
      ? ""
      : wallet
        ? `<div class="flex items-stretch gap-1.5">
            <a href="${escapeHtml(payUri)}" class="fm-btn fm-btn-sm"><i data-lucide="wallet" class="h-3 w-3"></i>Pay in wallet</a>
            <button data-links-copy-wallet="${index}" type="button" title="Copy ${escapeHtml(submitter)}'s Solana address" aria-label="Copy ${escapeHtml(submitter)}'s Solana address" class="fm-btn fm-btn-sm fm-btn-icon"><i data-lucide="copy" class="h-3 w-3"></i></button>
          </div>`
        : you
          ? '<a href="/dashboard/settings" class="text-[11px] text-accent underline">Add a Solana address</a>'
          : '<span class="text-[11px] text-muted-foreground">No Solana address yet</span>';
    return `<article class="px-4 sm:px-5 py-4 flex flex-col gap-3 sm:flex-row sm:items-start">
      <div class="min-w-0 flex-1">
        <div class="flex items-center gap-2 min-w-0">
          ${url
            ? `<a href="${escapeHtml(url)}" target="_blank" rel="nofollow ugc noopener noreferrer" class="min-w-0 truncate text-sm font-semibold text-foreground hover:underline">${escapeHtml(title)}</a>`
            : `<span class="min-w-0 truncate text-sm font-semibold text-foreground">${escapeHtml(title)}</span>`}
          ${you ? '<span class="fm-label shrink-0">you</span>' : ""}
        </div>
        <p class="mt-0.5 truncate font-mono text-[11px] text-muted-foreground">${escapeHtml(String(link.host || ""))}</p>
        <p class="mt-1.5 text-xs text-muted-foreground">
          by <a class="text-foreground hover:underline" href="${escapeHtml(publicProfileUrl(submitter))}">@${escapeHtml(submitter)}</a>
          · ${escapeHtml(channel)}${when ? ` · ${escapeHtml(when)}` : ""}
        </p>
        <div class="mt-2 flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground">
          <span class="inline-flex items-center gap-1.5" title="Estimated reach score">
            <span class="h-1.5 w-16 overflow-hidden rounded-full bg-secondary" aria-hidden="true"><span class="block h-full rounded-full bg-primary" style="width: ${score}%"></span></span>
            <span><span class="font-mono tabular-nums text-foreground">${score}</span>/100 reach</span>
          </span>
          <span class="font-mono tabular-nums">${escapeHtml(formatCount(low))}–${escapeHtml(formatCount(high))} potential visits</span>
        </div>
      </div>
      <div class="shrink-0 flex items-center justify-between gap-3 sm:flex-col sm:items-end">
        <div class="flex items-center gap-2 sm:flex-col sm:items-end sm:gap-1">
          <span class="font-mono text-sm font-semibold tabular-nums text-foreground">${escapeHtml(linkRewardText(reward))}</span>
          ${paid
            ? '<span class="fm-label fm-label-success"><i data-lucide="check" class="h-3 w-3"></i>Paid</span>'
            : '<span class="fm-label">Unpaid</span>'}
        </div>
        ${payAction}
      </div>
    </article>`;
  }

  function renderLinksNotice() {
    const root = $("[data-links-notice]");
    if (!root) return;
    const view = state.linksView;
    if (!view.notice) {
      root.innerHTML = "";
      return;
    }
    const bad = view.noticeTone === "bad";
    root.innerHTML = `<div class="px-4 sm:px-5 py-3 border-b border-border">${linkFlashHtml(bad ? "danger" : "success", escapeHtml(view.notice))}</div>`;
    window.lucide?.createIcons();
  }

  function linkFlashHtml(tone, html) {
    const icon = { danger: "circle-alert", success: "circle-check" }[tone] || "info";
    const variant = tone === "danger" || tone === "success" ? ` fm-flash-${tone}` : "";
    return `<div class="fm-flash${variant}"><i data-lucide="${icon}" aria-hidden="true"></i><div class="fm-flash-body">${html}</div></div>`;
  }

  function linkBlankslateHtml(icon, title, html) {
    return `<div class="fm-blankslate"><i data-lucide="${icon}" aria-hidden="true"></i><p class="fm-blankslate-title">${title}</p>${html ? `<p class="fm-blankslate-desc">${html}</p>` : ""}</div>`;
  }

  function renderLinksStart() {
    const root = $("[data-links-start]");
    const form = $("[data-links-form]");
    if (!root || !form) return;
    if (!linkAccountName()) {
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      root.hidden = false;
      root.innerHTML = linkFlashHtml("info", `<a class="text-accent underline" href="/login?next=${next}">Log in</a>
        or <a class="text-accent underline" href="/signup">create an account</a>
        to submit a link. The list below is public.`);
      form.hidden = true;
      window.lucide?.createIcons();
      return;
    }
    root.hidden = true;
    root.innerHTML = "";
    form.hidden = false;
    const submit = form.querySelector("[data-links-submit]");
    if (submit) submit.disabled = state.linksView.busy;
  }

  function renderLinksList() {
    const root = $("[data-links-list]");
    const count = $("[data-links-count]");
    if (!root) return;
    const view = state.linksView;
    if (count) {
      count.textContent = view.loading ? "Loading" : view.error ? "Unavailable" : `${formatCount(view.items.length)} links`;
    }
    if (view.loading || view.error) {
      root.innerHTML = view.loading
        ? `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading links...")}</div>`
        : linkBlankslateHtml("circle-alert", "Links did not load", escapeHtml(view.error));
      window.lucide?.createIcons();
      return;
    }
    root.innerHTML = view.items.length
      ? view.items.map(linkRowHtml).join("")
      : linkBlankslateHtml("link", "No links yet", "No links have been submitted yet. Share something you made about ForkMesh and be the first.");
    window.lucide?.createIcons();
  }

  function renderLinksPage() {
    renderLinksNotice();
    renderLinksStart();
    renderLinksList();
  }

  function applyLinksPayload(payload) {
    state.linksView.items = Array.isArray(payload?.links) ? payload.links.slice(0, 25) : [];
  }

  async function loadLinks(options = {}) {
    const view = state.linksView;
    view.loading = true;
    view.error = "";
    renderLinksList();
    try {
      applyLinksPayload(await fetchJson("/api/world/link-kiosk", { fresh: options.fresh === true }));
    } catch (_) {
      view.items = [];
      view.error = "Links are unavailable right now. Try Refresh in a moment.";
    } finally {
      view.loading = false;
      renderLinksList();
    }
  }

  async function submitLink(form) {
    const view = state.linksView;
    const values = new FormData(form);
    const result = form.querySelector("[data-links-result]");
    if (!values.get("consent")) {
      view.notice = LINK_ERROR_TEXT.consent_required;
      view.noticeTone = "bad";
      renderLinksNotice();
      return;
    }
    if (!linkSafeUrl(values.get("url"))) {
      view.notice = LINK_ERROR_TEXT.invalid_public_url;
      view.noticeTone = "bad";
      renderLinksNotice();
      form.elements.url?.focus();
      return;
    }
    view.busy = true;
    view.notice = "";
    renderLinksPage();
    if (result) result.textContent = "Analyzing server-side signals...";
    try {
      const headers = { accept: "application/json", "content-type": "application/json" };
      const token = state.session?.sessionToken || "";
      if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
      const response = await fetch("/api/world/link-kiosk", {
        method: "POST",
        cache: "no-store",
        credentials: "same-origin",
        headers,
        body: JSON.stringify({
          url: String(values.get("url") || "").trim(),
          title: String(values.get("title") || "").trim(),
          channel: String(values.get("channel") || "other"),
          consent: true,
        }),
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok || payload.ok === false) {
        throw new Error(payload.error || `HTTP ${response.status}`);
      }
      applyLinksPayload(payload);
      const submission = payload.submission || {};
      const low = Math.max(0, Number(submission.potentialTraffic?.low) || 0);
      const high = Math.max(low, Number(submission.potentialTraffic?.high) || 0);
      form.reset();
      if (result) result.textContent = "";
      view.notice = `Link published: reach ${Number(submission.score) || 0}/100, an estimated ${formatCount(low)}–${formatCount(high)} visits and ${linkRewardText(submission.rewardSol)} appreciation.`;
      view.noticeTone = "good";
    } catch (error) {
      if (result) result.textContent = "";
      view.notice = linkErrorText(error, "The link could not be submitted. Please try again.");
      view.noticeTone = "bad";
    } finally {
      view.busy = false;
      renderLinksPage();
    }
  }

  function initLinksPage() {
    const panel = $("[data-links-panel]");
    if (!panel) return;
    panel.addEventListener("click", async (event) => {
      if (event.target.closest("[data-links-refresh]")) {
        state.linksView.notice = "";
        renderLinksNotice();
        void loadLinks({ fresh: true });
        return;
      }
      const copy = event.target.closest("[data-links-copy-wallet]");
      if (!copy) return;
      const link = state.linksView.items[Number(copy.dataset.linksCopyWallet)];
      const wallet = String(link?.walletAddress || "");
      if (!LINK_SOLANA_ADDRESS_RE.test(wallet)) return;
      const copied = await copyTextToClipboard(wallet);
      copy.innerHTML = `<i data-lucide="${copied ? "check" : "x"}" class="h-3 w-3"></i>`;
      window.lucide?.createIcons();
      window.setTimeout(() => {
        if (!document.body.contains(copy)) return;
        copy.innerHTML = '<i data-lucide="copy" class="h-3 w-3"></i>';
        window.lucide?.createIcons();
      }, 1600);
    });
    $("[data-links-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      if (state.linksView.busy) return;
      void submitLink(event.currentTarget);
    });
    renderLinksStart();
    void loadLinks();
  }
  return { initLinksPage };
  };
})();

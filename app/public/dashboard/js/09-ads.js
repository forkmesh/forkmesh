  // ---------------------------------------------------------------------------
  // ForkMesh Ads: the first-party, human-reviewed, text-only contextual ad
  // network (Worker side: app/src/ad_network.py). Two surfaces live here:
  //
  //   1. initAdSlot() - the `web` slot in the shared chrome (partials/main.html),
  //      on every composed page including the public /@name and /owner/repo
  //      documents. Those documents are edge-cached and identical for every
  //      viewer, so the ad is fetched client-side, after the page is idle.
  //   2. initAdsPage() - /dashboard/ads, where an owner writes, submits and
  //      follows their ads, and where a platform reviewer reads the queue.
  //
  // Privacy rules this file must keep: the serve and click calls carry no
  // viewer input at all (no authorization header, credentials "omit"); ad
  // fields are plain text and only ever reach the DOM through textContent; a
  // missing or failed ad renders nothing.
  const ADS_PLACEMENT = "web";
  const ADS_POLICY_DOC_URL = "https://forkmesh.com/docs/operations/ad-network/";
  const ADS_HIDE_KEY = "forkmesh.ads.hiddenUntil";
  const ADS_HIDE_MS = 7 * 24 * 60 * 60 * 1000;
  // The ads page previews ads already, and the chat document gives its whole
  // height to the transcript and composer.
  const ADS_SLOT_SKIPPED_PAGES = new Set(["ads", "chat"]);
  const ADS_ID_RE = /^[a-f0-9]{32}$/;
  // policy.reportReasons, fixed by the client contract. Hard-coded so the slot
  // never spends a second request on the policy document.
  const ADS_REPORT_REASONS = [
    { id: "prohibited", label: "It advertises something the policy prohibits" },
    { id: "misleading", label: "It is misleading or deceptive" },
    { id: "offensive", label: "It is offensive or hateful" },
    { id: "broken", label: "The link is broken or goes somewhere else" },
    { id: "other", label: "Something else" },
  ];
  // Placement ids are exact protocol values; the labels are only a fallback
  // for the moments before (or without) the policy document.
  const ADS_FALLBACK_PLACEMENTS = [
    { id: "web", label: "Web app and public repository and profile pages" },
    { id: "www", label: "forkmesh.com site" },
    { id: "desktop", label: "Desktop app" },
    { id: "mobile", label: "Mobile apps" },
    { id: "world", label: "ForkMesh World" },
  ];
  const ADS_FALLBACK_LIMITS = { headline: 60, body: 160, cta: 24, sponsor: 48, note: 500 };
  const ADS_CREATIVE_FIELDS = ["headline", "body", "cta", "sponsor", "url", "category"];

  const ADS_TONE_CLASSES = {
    good: "border-emerald-500/40 bg-emerald-500/10 text-emerald-500",
    warn: "border-amber-500/40 bg-amber-500/10 text-amber-500",
    bad: "border-red-500/40 bg-red-500/10 text-red-500",
    neutral: "border-border bg-secondary text-muted-foreground",
  };

  // One plain sentence per delivery word, so an owner never has to guess why
  // an approved ad is not on screen.
  const ADS_DELIVERY = {
    "running": { label: "Running", tone: "good", text: "Approved and running now." },
    "paused": { label: "Paused", tone: "neutral", text: "Approved, and paused by you. Resume it whenever you like." },
    "scheduled": { label: "Scheduled", tone: "neutral", text: "Approved. It starts running at its scheduled start time." },
    "ended": { label: "Ended", tone: "neutral", text: "Its schedule has ended. Edit the end date to run it again." },
    "needs-membership": { label: "Needs membership", tone: "warn", text: "Approved. It will start running when you have an active membership." },
    "over-allowance": { label: "Waiting for a slot", tone: "warn", text: "Approved, but your level's live slots are in use by older ads." },
    "review-expired": { label: "Review expired", tone: "warn", text: "The approval is older than 180 days; resubmit it for a fresh review." },
    "draft": { label: "Draft", tone: "neutral", text: "Only you can see it. Submit it when it is ready for review." },
    "pending": { label: "In review", tone: "neutral", text: "Waiting for a person to review it. Nothing runs until then." },
    "rejected": { label: "Rejected", tone: "bad", text: "A reviewer declined it. Edit it to address the reason, then submit it again." },
    "suspended": { label: "Suspended", tone: "bad", text: "A reviewer or several reports put it on hold. It cannot be edited until a reviewer reinstates it." },
    "withdrawn": { label: "Withdrawn", tone: "neutral", text: "You withdrew it. Submit it again to send it back to review." },
  };

  const ADS_STATUS_LABELS = {
    draft: "Draft", pending: "In review", approved: "Approved",
    rejected: "Rejected", suspended: "Suspended", withdrawn: "Withdrawn",
  };

  const ADS_ERRORS = {
    unauthorized: "Your session has ended. Log in again to manage ads.",
    forbidden: "Only platform reviewers can do that.",
    not_found: "That ad no longer exists. Refresh the page to see the current list.",
    invalid_transition: "That action does not apply to the ad's current state. The list has been refreshed.",
    ad_suspended: "This ad is suspended. It cannot be changed until a reviewer reinstates it.",
    ad_limit_reached: "You have reached the limit of ads per account. Delete one you no longer need first.",
    pending_limit_reached: "You already have the maximum number of ads waiting for review. Wait for a decision or withdraw one first.",
    attestation_required: "Tick the confirmation that the ad follows the ForkMesh Ads policy before submitting it for review.",
    headline_required: "Write a headline of at least 4 characters.",
    sponsor_required: "Add the sponsor name (at least 2 characters) so readers know who is behind the ad.",
    invalid_category: "Pick one of the listed categories.",
    invalid_url: "The destination must be a public https:// address.",
    url_shortener_not_allowed: "Link shorteners are not allowed. Use the real destination address.",
    tracking_parameter_not_allowed: "Remove click identifiers (fbclid, gclid and the like) from the destination address. Campaign tags such as utm_source are fine.",
    invalid_placements: "Pick at least one place for the ad to run.",
    invalid_schedule: "Check the schedule: the end must be in the future, after the start, and within a year.",
    unsupported_ad_field: "This page sent something the server does not accept. Reload the page and try again.",
    invalid_reason: "Pick one of the listed reasons.",
    cannot_report_own_ad: "This is your own ad, so you cannot report it.",
    reason_required: "Give a reason or tick at least one policy rule.",
    invalid_codes: "One of the ticked policy rules is not recognised. Reload the page and try again.",
    creative_changed: "The owner changed this ad while you were reading it. The queue has been reloaded - read the new version before deciding.",
    payload_too_large: "That is too much text. Shorten the ad and try again.",
  };

  function adsErrorMessage(error) {
    const code = String(error?.code || "");
    const limit = Number(error?.payload?.limit) || 0;
    if (code === "ad_limit_reached" && limit) {
      return `You have reached the limit of ${limit} ads per account. Delete one you no longer need first.`;
    }
    if (code === "pending_limit_reached" && limit) {
      return `You already have ${limit} ads waiting for review. Wait for a decision or withdraw one first.`;
    }
    if (ADS_ERRORS[code]) return ADS_ERRORS[code];
    if (error?.status === 401) return ADS_ERRORS.unauthorized;
    if (error?.status === 403) return ADS_ERRORS.forbidden;
    if (!error?.status) return "Could not reach ForkMesh. Check your connection and try again.";
    return "That did not work. Try again in a moment.";
  }

  function adsEl(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function adsIcon(name, className) {
    const icon = document.createElement("i");
    icon.setAttribute("data-lucide", name);
    icon.className = className || "w-3.5 h-3.5";
    return icon;
  }

  function adsBadge(label, tone) {
    return adsEl("span",
      `shrink-0 rounded-full border px-2 py-0.5 text-[10px] font-semibold ${ADS_TONE_CLASSES[tone] || ADS_TONE_CLASSES.neutral}`,
      label);
  }

  function adsButton(label, options = {}) {
    const tone = options.tone || "plain";
    const classes = {
      primary: "inline-flex items-center gap-1.5 rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90 transition-colors",
      plain: "inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary transition-colors",
      danger: "inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-muted-foreground hover:bg-red-500/10 hover:border-red-500/50 hover:text-red-500 transition-colors",
    };
    const button = adsEl("button", classes[tone] || classes.plain);
    button.type = "button";
    if (options.icon) button.append(adsIcon(options.icon));
    button.append(document.createTextNode(label));
    return button;
  }

  // Only a plain https address may become a link. The Worker validates the
  // same thing before storing; this is the second half of that rule, at the
  // point the anchor is built, so no javascript:/data: string can reach href.
  function adsSafeDestination(value) {
    try {
      const url = new URL(String(value || ""));
      if (url.protocol !== "https:" || url.username || url.password) return "";
      return url.href;
    } catch (_) {
      return "";
    }
  }

  // Anonymous public read: no authorization header and no cookies, ever. The
  // serve call must not carry a single bit about the viewer, so it does not go
  // through fetchJson (which attaches the session token).
  async function adsPublicRead(path) {
    const response = await fetch(path, {
      method: "GET",
      credentials: "omit",
      headers: { accept: "application/json" },
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(data.error || `HTTP ${response.status}`);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  // Signed-in calls (owner, reviewer, report): bearer session token, same
  // conventions as the other first-party pages, never cached.
  async function adsRequest(method, path, body) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const init = { method, headers, cache: "no-store", credentials: "same-origin" };
    if (body !== undefined) {
      headers["content-type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    const response = await fetch(path, init);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = String(payload.error || "");
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  function adsSignedIn() {
    const session = state.session;
    return Boolean(session && (session.nodeName || session.email));
  }

  // ---- The ad card ---------------------------------------------------------
  // One builder for the live slot, the form preview, the owner's list and the
  // reviewer queue, so what a reviewer approves is what a viewer sees.
  // `options.live` wires the click report and the Hide/Report controls.
  function adCardElement(ad, options = {}) {
    const live = options.live === true;
    const root = adsEl("div", "grid gap-2");
    const row = adsEl("div", "flex flex-wrap items-center gap-x-3 gap-y-2");
    row.append(adsEl("span",
      "shrink-0 rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold uppercase tracking-wide text-muted-foreground",
      "Sponsored"));

    const copy = adsEl("div", "min-w-0");
    copy.style.flex = "1 1 16rem";
    const line = adsEl("p", "text-sm leading-5 text-foreground");
    line.style.overflowWrap = "anywhere";
    line.append(adsEl("span", "font-semibold", ad.headline || ""));
    if (ad.body) {
      line.append(document.createTextNode(" "));
      line.append(adsEl("span", "text-muted-foreground", ad.body));
    }
    copy.append(line);
    const byline = [ad.sponsor, ad.categoryLabel].filter(Boolean).join(" · ");
    if (byline) copy.append(adsEl("p", "mt-0.5 truncate text-[11px] text-muted-foreground", byline));
    row.append(copy);

    const cta = adsEl("a",
      "shrink-0 inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary transition-colors");
    cta.append(document.createTextNode(ad.cta || "Learn more"));
    cta.append(adsIcon("external-link", "w-3 h-3"));
    const href = adsSafeDestination(ad.url);
    if (href) {
      cta.href = href;
      cta.target = "_blank";
      cta.rel = "sponsored noopener noreferrer";
      cta.setAttribute("referrerpolicy", "no-referrer");
      if (live) {
        const report = () => reportAdClick(ad.id);
        cta.addEventListener("click", report);
        cta.addEventListener("auxclick", report);
      }
    } else {
      cta.setAttribute("aria-disabled", "true");
    }
    row.append(cta);

    const tools = adsEl("div", "shrink-0 flex items-center gap-3 text-[11px] text-muted-foreground");
    const why = adsEl("a", "hover:text-foreground hover:underline", "Why this ad?");
    why.href = ADS_POLICY_DOC_URL;
    why.target = "_blank";
    why.rel = "noopener noreferrer";
    tools.append(why);
    let reportButton = null;
    if (live && adsSignedIn()) {
      reportButton = adsEl("button", "hover:text-foreground hover:underline", "Report");
      reportButton.type = "button";
      tools.append(reportButton);
    }
    const hide = adsEl("button", "hover:text-foreground hover:underline", "Hide");
    hide.type = "button";
    if (live) {
      hide.title = "Hide sponsored messages for 7 days";
      hide.addEventListener("click", () => options.onHide?.());
    } else {
      hide.disabled = true;
      hide.title = "Viewers can hide sponsored messages for 7 days";
    }
    tools.append(hide);
    row.append(tools);
    root.append(row);

    if (reportButton) {
      const picker = adReportPickerElement(ad, options);
      picker.hidden = true;
      reportButton.addEventListener("click", () => { picker.hidden = !picker.hidden; });
      root.append(picker);
    }
    return root;
  }

  function adReportPickerElement(ad, options) {
    const picker = adsEl("div", "flex flex-wrap items-center gap-2 text-xs text-muted-foreground");
    const label = adsEl("label", "flex flex-wrap items-center gap-2", "What is wrong with this ad?");
    const select = adsEl("select", "rounded-md border border-border bg-background px-2 py-1 text-xs text-foreground focus:outline-none");
    for (const reason of ADS_REPORT_REASONS) {
      const option = adsEl("option", "", reason.label);
      option.value = reason.id;
      select.append(option);
    }
    label.append(select);
    const send = adsButton("Send report");
    const cancel = adsEl("button", "hover:text-foreground hover:underline", "Cancel");
    cancel.type = "button";
    const status = adsEl("span", "");
    status.setAttribute("role", "status");
    cancel.addEventListener("click", () => { picker.hidden = true; });
    send.addEventListener("click", async () => {
      if (!ADS_ID_RE.test(String(ad.id || ""))) return;
      send.disabled = true;
      status.textContent = "Sending...";
      try {
        await adsRequest("POST", `/api/ads/${ad.id}/report`, { reason: select.value });
        // A reported ad is not something this viewer wants to keep seeing.
        picker.replaceChildren(adsEl("span", "", "Thank you. A reviewer will look at this ad."));
        window.setTimeout(() => options.onReported?.(), 2500);
      } catch (error) {
        send.disabled = false;
        status.textContent = adsErrorMessage(error);
      }
    });
    picker.append(label, send, cancel, status);
    return picker;
  }

  // Fire-and-forget on purpose: following the link must never wait on, or be
  // blocked by, the aggregate click count. keepalive lets it outlive the tab
  // switch; like the serve call it carries no credentials and no session.
  function reportAdClick(adId) {
    if (!ADS_ID_RE.test(String(adId || ""))) return;
    try {
      void fetch(`/api/ads/${adId}/click`, {
        method: "POST",
        keepalive: true,
        credentials: "omit",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ placement: ADS_PLACEMENT }),
      }).catch(() => {});
    } catch (_) {}
  }

  // ---- The `web` slot ------------------------------------------------------
  function adSlotHiddenUntil() {
    try {
      return Number(localStorage.getItem(ADS_HIDE_KEY)) || 0;
    } catch (_) {
      return 0;
    }
  }

  function collapseAdSlot(slot) {
    slot.hidden = true;
    slot.replaceChildren();
  }

  async function loadAdSlot(slot) {
    let ad = null;
    try {
      const data = await adsPublicRead("/api/ads/serve?placement=web");
      ad = data && data.ad && typeof data.ad === "object" ? data.ad : null;
    } catch (_) {
      ad = null;
    }
    // No ad, a failed request or a malformed answer all look the same: the
    // slot stays collapsed. No placeholder, no error text, no retry.
    if (!ad || !ad.headline || !adsSafeDestination(ad.url)) return;
    if (adSlotHiddenUntil() > Date.now()) return;
    slot.replaceChildren(adCardElement(ad, {
      live: true,
      onHide: () => {
        try {
          localStorage.setItem(ADS_HIDE_KEY, String(Date.now() + ADS_HIDE_MS));
        } catch (_) {}
        collapseAdSlot(slot);
      },
      onReported: () => collapseAdSlot(slot),
    }));
    slot.hidden = false;
    window.lucide?.createIcons();
  }

  // Called once per page load from boot. The serve read waits for an idle
  // moment so it never competes with the page's own data; the slot ships
  // `hidden` and reserves no space, so a late or absent ad shifts nothing
  // above it.
  function initAdSlot() {
    const slot = $('[data-forkmesh-ad-slot="web"]');
    if (!slot || ADS_SLOT_SKIPPED_PAGES.has(currentPage())) return;
    if (adSlotHiddenUntil() > Date.now()) return;
    const run = () => { void loadAdSlot(slot); };
    if (typeof window.requestIdleCallback === "function") {
      window.requestIdleCallback(run, { timeout: 5000 });
    } else {
      window.setTimeout(run, 1500);
    }
  }

  // ---- /dashboard/ads: shared lookups --------------------------------------
  function adsPolicyList(key, fallback) {
    const list = state.adsView.policy?.[key];
    return Array.isArray(list) && list.length ? list : (fallback || []);
  }

  function adsLimits() {
    return { ...ADS_FALLBACK_LIMITS, ...(state.adsView.policy?.limits || {}) };
  }

  function adsLabelFor(list, id) {
    const found = list.find((item) => item.id === id);
    return found ? String(found.label || id) : String(id);
  }

  function adsProhibitedList() {
    const fromQueue = state.adsView.review.prohibited;
    return adsPolicyList("prohibited", Array.isArray(fromQueue) ? fromQueue : []);
  }

  function adsPlacementLabels(ids) {
    const list = adsPolicyList("placements", ADS_FALLBACK_PLACEMENTS);
    return (Array.isArray(ids) ? ids : []).map((id) => adsLabelFor(list, id));
  }

  // Screen flags are prompts for the human reviewer, never a decision. They
  // are worded so an owner reads them as "this will be looked at", not "this
  // is wrong".
  function adsFlagLabel(flag) {
    const text = String(flag || "");
    const split = text.indexOf(":");
    const kind = split < 0 ? "" : text.slice(0, split);
    const value = split < 0 ? text : text.slice(split + 1);
    if (kind === "term") return `wording close to "${adsLabelFor(adsProhibitedList(), value)}"`;
    if (text === "style:shouting") return "a headline mostly in capitals";
    if (text === "style:punctuation") return "repeated ! or ? marks";
    if (text === "claim:price-or-rate") return "a price, discount or rate claim";
    if (text === "url:query-string") return "a query string in the destination address";
    return text;
  }

  function adsDateTimeLabel(ms) {
    const value = Number(ms) || 0;
    if (!value) return "";
    return new Date(value).toLocaleString(undefined, {
      year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit",
    });
  }

  function adsScheduleLabel(ad) {
    const start = adsDateTimeLabel(ad.startsAt);
    const end = adsDateTimeLabel(ad.endsAt);
    if (start && end) return `${start} to ${end}`;
    if (start) return `From ${start}, open-ended`;
    if (end) return `Until ${end}`;
    return "No schedule: runs while approved";
  }

  // <input type="datetime-local"> speaks local wall-clock time without a zone.
  function adsDateTimeInputValue(ms) {
    const value = Number(ms) || 0;
    if (!value) return "";
    const date = new Date(value);
    const pad = (number) => String(number).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`;
  }

  function adsDateTimeInputMs(value) {
    if (!value) return 0;
    const ms = new Date(value).getTime();
    return Number.isFinite(ms) && ms > 0 ? ms : NaN;
  }

  function setAdsNotice(message, tone = "good") {
    state.adsView.notice = message || "";
    state.adsView.noticeTone = tone;
    const box = $("[data-ads-notice]");
    if (!box) return;
    box.textContent = state.adsView.notice;
    box.hidden = !state.adsView.notice;
    box.classList.toggle("text-red-500", tone === "bad");
    box.classList.toggle("text-foreground", tone !== "bad");
  }

  // ---- /dashboard/ads: policy summary --------------------------------------
  function renderAdsPolicy() {
    const policy = state.adsView.policy;
    const tiers = $("[data-ads-tiers]");
    if (tiers) {
      tiers.replaceChildren();
      const rows = adsPolicyList("tiers");
      const currentTier = state.adsView.mine?.tier?.id || "";
      if (!rows.length) {
        const row = adsEl("tr");
        const cell = adsEl("td", "py-2 text-muted-foreground",
          state.adsView.policyError || "Loading levels...");
        cell.colSpan = 4;
        row.append(cell);
        tiers.append(row);
      }
      for (const tier of rows) {
        const row = adsEl("tr", tier.id === currentTier ? "bg-secondary" : "");
        const name = adsEl("td", "py-1.5 pr-3 font-semibold text-foreground", tier.label || tier.id);
        if (tier.id === currentTier) {
          name.append(document.createTextNode(" "));
          name.append(adsEl("span", "rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold text-muted-foreground", "your level"));
        }
        const price = Number(tier.priceUsdMonthly) || 0;
        const live = Number(tier.liveAds) || 0;
        row.append(
          name,
          adsEl("td", "py-1.5 pr-3 text-muted-foreground tabular-nums", price ? `$${formatCount(price)}` : "Free"),
          adsEl("td", "py-1.5 pr-3 text-muted-foreground tabular-nums", live ? formatCount(live) : "None - write and get approved, run later"),
          adsEl("td", "py-1.5 text-muted-foreground tabular-nums", live ? `${formatCount(Number(tier.weight) || 0)}x` : "-"));
        tiers.append(row);
      }
    }
    const fillList = (selector, items) => {
      const root = $(selector);
      if (!root) return;
      root.replaceChildren();
      if (!items.length) {
        root.append(adsEl("li", "", state.adsView.policyError || "Loading..."));
        return;
      }
      for (const item of items) root.append(adsEl("li", "", item.label || item.id));
    };
    fillList("[data-ads-prohibited]", adsPolicyList("prohibited"));
    fillList("[data-ads-categories]", adsPolicyList("categories"));
    if (policy?.summary) {
      const summary = $("[data-ads-summary]");
      if (summary) summary.textContent = String(policy.summary);
    }
    if (policy?.payment?.note) {
      const note = $("[data-ads-payment-note]");
      if (note) {
        // The card's heading already says "There is no ad billing yet".
        const text = String(policy.payment.note).replace(/^There is no ad billing yet\.\s*/, "");
        note.textContent = `${text} Anyone can write and submit an ad; it runs while its owner has an active membership.`;
      }
    }
    renderAdsFormChoices();
  }

  function renderAdsFormChoices() {
    const select = $('[data-ads-field="category"]');
    if (select) {
      const chosen = select.value || select.dataset.pending || "";
      select.replaceChildren();
      const blank = adsEl("option", "", "Choose a category");
      blank.value = "";
      select.append(blank);
      for (const category of adsPolicyList("categories")) {
        const option = adsEl("option", "", category.label || category.id);
        option.value = category.id;
        select.append(option);
      }
      select.value = chosen;
    }
    const placements = $("[data-ads-placements]");
    if (placements) {
      const previous = new Map(
        $$("[data-ads-placement]").map((input) => [input.value, input.checked]));
      placements.replaceChildren();
      for (const placement of adsPolicyList("placements", ADS_FALLBACK_PLACEMENTS)) {
        const label = adsEl("label", "flex items-start gap-2 rounded-md border border-border bg-background p-3 text-xs text-muted-foreground");
        const input = document.createElement("input");
        input.type = "checkbox";
        input.value = placement.id;
        input.setAttribute("data-ads-placement", "");
        input.className = "mt-0.5 h-4 w-4 shrink-0 accent-primary";
        // Every surface is on by default; an owner opts out, not in.
        input.checked = previous.has(placement.id) ? previous.get(placement.id) : true;
        label.append(input, adsEl("span", "", placement.label || placement.id));
        placements.append(label);
      }
    }
    const limits = adsLimits();
    for (const field of ["headline", "body", "cta", "sponsor", "note"]) {
      const input = $(`[data-ads-field="${field}"]`);
      if (input && Number(limits[field]) > 0) input.maxLength = Number(limits[field]);
    }
    updateAdsCounters();
    updateAdsPreview();
  }

  // ---- /dashboard/ads: session, level and usage ----------------------------
  function adsStatTile(label, value, hint) {
    const tile = adsEl("div", "rounded-md border border-border bg-background p-3");
    tile.append(
      adsEl("p", "text-xs text-muted-foreground", label),
      adsEl("p", "mt-1 text-2xl font-semibold text-foreground tabular-nums", value),
      adsEl("p", "mt-1 text-xs text-muted-foreground", hint));
    return tile;
  }

  function renderAdsSession() {
    const root = $("[data-ads-session]");
    const level = $("[data-ads-level]");
    const usageLine = $("[data-ads-usage]");
    const owner = $("[data-ads-owner]");
    if (!root) return;
    const view = state.adsView;
    if (!adsSignedIn() || view.error === "unauthorized") {
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      root.innerHTML = `<p class="text-sm text-muted-foreground">
        <a class="text-accent underline" href="/login?next=${next}">Log in</a>
        or <a class="text-accent underline" href="/signup">create an account</a>
        to write an ad and send it to a reviewer. Everything on this page about how
        the network works is public.
      </p>`;
      if (level) level.textContent = "Signed out";
      if (usageLine) usageLine.textContent = "";
      if (owner) owner.hidden = true;
      return;
    }
    if (owner) owner.hidden = false;
    if (view.loading && !view.mine) {
      root.innerHTML = `<p class="text-sm text-muted-foreground">${loadingHtml("Loading your ads...")}</p>`;
      if (level) level.textContent = "Loading";
      return;
    }
    if (!view.mine) {
      root.replaceChildren(adsEl("p", "text-sm text-muted-foreground",
        "Your ads are unavailable right now. Refresh to try again."));
      if (level) level.textContent = "Unavailable";
      return;
    }
    const tier = view.mine.tier || {};
    const usage = view.mine.usage || {};
    const liveAds = Number(usage.liveAds) || 0;
    if (level) level.textContent = `${tier.label || "Free"} level`;
    const grid = adsEl("div", "grid gap-3 sm:grid-cols-3");
    grid.append(
      adsStatTile("Running now", `${formatCount(usage.running)} / ${formatCount(liveAds)}`,
        liveAds
          ? `Live slots your ${tier.label || ""} level funds`
          : "No live slots yet - a membership level funds them"),
      adsStatTile("Waiting for review", `${formatCount(usage.pending)} / ${formatCount(usage.pendingLimit)}`,
        "Ads a person has yet to read"),
      adsStatTile("Ads written", `${formatCount(usage.total)} / ${formatCount(usage.totalLimit)}`,
        "Every ad on this account, any state"));
    root.replaceChildren(grid);
    if (usageLine) {
      usageLine.textContent = `Your level: ${tier.label || "Free"} - ${formatCount(usage.running)} of ${formatCount(liveAds)} live slots in use.`;
    }
  }

  // ---- /dashboard/ads: the form --------------------------------------------
  function adsEditingAd() {
    const id = state.adsView.editingId;
    if (!id) return null;
    return (state.adsView.mine?.ads || []).find((ad) => ad.id === id) || null;
  }

  function adsFormValues() {
    const read = (field) => String($(`[data-ads-field="${field}"]`)?.value || "");
    return {
      headline: read("headline").trim(),
      body: read("body").trim(),
      cta: read("cta").trim(),
      sponsor: read("sponsor").trim(),
      url: read("url").trim(),
      category: read("category"),
      placements: $$("[data-ads-placement]")
        .filter((input) => input.checked).map((input) => input.value).sort(),
      startsAt: adsDateTimeInputMs(read("startsAt")),
      endsAt: adsDateTimeInputMs(read("endsAt")),
      note: read("note").trim(),
    };
  }

  // Whether the reviewed part of the ad differs from what is stored: the
  // same fields the Worker hashes (words, link, category and surfaces - not
  // the schedule or the note).
  function adsCreativeChanged(ad, values) {
    if (ADS_CREATIVE_FIELDS.some((field) =>
      String(values[field] || "") !== String(ad[field] || "") &&
      !(field === "cta" && !values.cta && ad.cta === "Learn more"))) {
      return true;
    }
    const stored = (Array.isArray(ad.placements) ? ad.placements.slice() : []).sort();
    return stored.join(",") !== values.placements.join(",");
  }

  function updateAdsCounters() {
    const limits = adsLimits();
    for (const counter of $$("[data-ads-count]")) {
      const field = counter.getAttribute("data-ads-count");
      const input = $(`[data-ads-field="${field}"]`);
      const limit = Number(limits[field]) || 0;
      const used = Array.from(String(input?.value || "")).length;
      counter.textContent = limit ? `${used} / ${limit}` : String(used);
      counter.classList.toggle("text-amber-500", Boolean(limit) && used >= limit);
      counter.classList.toggle("text-muted-foreground", !limit || used < limit);
    }
  }

  function updateAdsPreview() {
    const root = $("[data-ads-preview]");
    if (!root) return;
    const values = adsFormValues();
    root.replaceChildren(adCardElement({
      headline: values.headline || "Your headline appears here",
      body: values.body,
      cta: values.cta || "Learn more",
      sponsor: values.sponsor || "Sponsor name",
      url: values.url,
      categoryLabel: values.category
        ? adsLabelFor(adsPolicyList("categories"), values.category) : "",
    }));
    window.lucide?.createIcons();
    updateAdsEditWarning(values);
  }

  function updateAdsEditWarning(values) {
    const box = $("[data-ads-edit-warning]");
    if (!box) return;
    const ad = adsEditingAd();
    const warn = Boolean(ad && ad.status === "approved" &&
      adsCreativeChanged(ad, values || adsFormValues()));
    box.hidden = !warn;
    box.textContent = warn
      ? "This ad is approved. Changing its words, link, category or surfaces sends it back to review, and it stops running until a reviewer approves the new version. The schedule and the note can change freely."
      : "";
  }

  function setAdsFormError(message) {
    const box = $("[data-ads-form-error]");
    if (!box) return;
    box.textContent = message || "";
    box.hidden = !message;
  }

  function fillAdsForm(ad) {
    state.adsView.editingId = ad ? ad.id : "";
    const set = (field, value) => {
      const input = $(`[data-ads-field="${field}"]`);
      if (input) input.value = value;
    };
    set("headline", ad?.headline || "");
    set("body", ad?.body || "");
    set("cta", ad?.cta || "");
    set("sponsor", ad?.sponsor || "");
    set("url", ad?.url || "");
    const category = $('[data-ads-field="category"]');
    if (category) {
      category.dataset.pending = ad?.category || "";
      category.value = ad?.category || "";
    }
    set("startsAt", adsDateTimeInputValue(ad?.startsAt));
    set("endsAt", adsDateTimeInputValue(ad?.endsAt));
    set("note", ad?.note || "");
    const chosen = ad && Array.isArray(ad.placements) ? new Set(ad.placements) : null;
    for (const input of $$("[data-ads-placement]")) {
      input.checked = chosen ? chosen.has(input.value) : true;
    }
    const attestation = $("[data-ads-attestation]");
    if (attestation) attestation.checked = false;
    const title = $("[data-ads-form-title]");
    if (title) title.textContent = ad ? "Edit your ad" : "Write an ad";
    const reset = $("[data-ads-form-reset]");
    if (reset) reset.hidden = !ad;
    // Submit applies to an ad that is not already with a reviewer or approved.
    const submit = $("[data-ads-submit]");
    const canSubmit = !ad || ["draft", "rejected", "withdrawn"].includes(ad.status);
    if (submit) submit.hidden = !canSubmit;
    const draft = $("[data-ads-save-draft]");
    if (draft) {
      draft.lastChild.textContent = !ad || ad.status === "draft" ? "Save draft" : "Save changes";
    }
    setAdsFormError("");
    updateAdsCounters();
    updateAdsPreview();
  }

  function adsClientValidation(values) {
    if (Array.from(values.headline).length < 4) return { code: "headline_required" };
    if (Array.from(values.sponsor).length < 2) return { code: "sponsor_required" };
    if (!values.category) return { code: "invalid_category" };
    if (!adsSafeDestination(values.url)) return { code: "invalid_url" };
    if (!values.placements.length) return { code: "invalid_placements" };
    if (Number.isNaN(values.startsAt) || Number.isNaN(values.endsAt)) return { code: "invalid_schedule" };
    if (values.endsAt && values.endsAt <= Math.max(values.startsAt, Date.now())) {
      return { code: "invalid_schedule" };
    }
    return null;
  }

  async function saveAdsForm(submit) {
    setAdsFormError("");
    const values = adsFormValues();
    const invalid = adsClientValidation(values);
    if (invalid) {
      setAdsFormError(adsErrorMessage({ code: invalid.code, status: 400 }));
      return;
    }
    const editing = adsEditingAd();
    const attestation = $("[data-ads-attestation]")?.checked === true;
    const backToReview = Boolean(editing && editing.status === "approved" &&
      adsCreativeChanged(editing, values));
    if ((submit || backToReview) && !attestation) {
      setAdsFormError(ADS_ERRORS.attestation_required);
      $("[data-ads-attestation]")?.focus();
      return;
    }
    if (backToReview && !window.confirm(
      "This ad is approved. Saving these changes sends it back to review, and it stops running until a reviewer approves the new version. Save anyway?")) {
      return;
    }
    const button = submit ? $("[data-ads-submit]") : $("[data-ads-save-draft]");
    setSubmitBusy(button, true, submit ? "Submitting..." : "Saving...");
    try {
      const body = { ...values, submit: submit === true, attestation };
      const result = editing
        ? await adsRequest("PUT", `/api/ads/${editing.id}`, body)
        : await adsRequest("POST", "/api/ads", body);
      const status = result.ad?.status || "";
      setSubmitBusy(button, false);
      fillAdsForm(null);
      setAdsNotice(
        status === "pending" && backToReview
          ? "Saved. The new version is back in review and the ad is paused until a person approves it."
          : status === "pending"
            ? "Submitted. A person will read it before it runs - nothing is approved automatically."
            : status === "draft" ? "Draft saved. Only you can see it until you submit it."
              : "Saved.");
      await loadAdsMine();
      if (state.adsView.mine?.isReviewer) void loadAdsReviewQueue();
    } catch (error) {
      setSubmitBusy(button, false);
      setAdsFormError(adsErrorMessage(error));
      if (error.code === "invalid_transition" || error.code === "not_found") void loadAdsMine();
    }
  }

  // ---- /dashboard/ads: "Your ads" ------------------------------------------
  function adsReviewReasonElement(ad) {
    const codes = Array.isArray(ad.reviewCodes) ? ad.reviewCodes : [];
    if (!["rejected", "suspended"].includes(ad.status) || (!ad.reviewReason && !codes.length)) {
      return null;
    }
    const box = adsEl("div", "rounded-md border border-red-500/40 bg-red-500/10 p-3 text-xs text-foreground");
    box.append(adsEl("p", "font-semibold", ad.status === "suspended" ? "Why it was suspended" : "Why it was rejected"));
    if (ad.reviewReason) {
      const reason = adsEl("p", "mt-1", ad.reviewReason);
      reason.style.overflowWrap = "anywhere";
      box.append(reason);
    }
    if (codes.length) {
      box.append(adsEl("p", "mt-2 text-muted-foreground", "Policy rules the reviewer cited:"));
      const list = adsEl("ul", "mt-1 grid gap-1");
      for (const code of codes) list.append(adsEl("li", "", adsLabelFor(adsProhibitedList(), code)));
      box.append(list);
    }
    return box;
  }

  function adsClicksLabel(ad) {
    const clicks = ad.clicks || {};
    const days = Number(clicks.windowDays) || 30;
    const total = Number(clicks.total) || 0;
    const parts = Object.entries(clicks.byPlacement || {})
      .filter(([, count]) => Number(count) > 0)
      .map(([placement, count]) => `${placement} ${formatCount(count)}`);
    return `Clicks, last ${days} days: ${formatCount(total)}${parts.length ? ` (${parts.join(" · ")})` : ""}`;
  }

  async function runAdsOwnerAction(ad, action, body, done) {
    try {
      if (action === "delete") await adsRequest("DELETE", `/api/ads/${ad.id}`);
      else await adsRequest("POST", `/api/ads/${ad.id}/${action}`, body || {});
      if (state.adsView.editingId === ad.id && (action === "delete" || action === "withdraw")) {
        fillAdsForm(null);
      }
      setAdsNotice(done);
    } catch (error) {
      setAdsNotice(adsErrorMessage(error), "bad");
    }
    await loadAdsMine();
    if (state.adsView.mine?.isReviewer) void loadAdsReviewQueue();
  }

  function adsOwnerItemElement(ad) {
    const item = adsEl("div", "px-4 sm:px-5 py-4 grid gap-3");
    const frame = adsEl("div", "rounded-md border border-border bg-background px-4 py-2");
    frame.append(adCardElement(ad));
    item.append(frame);

    const delivery = ADS_DELIVERY[ad.delivery] || { label: ad.delivery || "Unknown", tone: "neutral", text: "" };
    const stateRow = adsEl("div", "flex flex-wrap items-center gap-2");
    if (ad.status && ad.status !== ad.delivery) {
      stateRow.append(adsBadge(ADS_STATUS_LABELS[ad.status] || ad.status,
        ad.status === "approved" ? "good" : "neutral"));
    }
    stateRow.append(adsBadge(delivery.label, delivery.tone));
    stateRow.append(adsEl("span", "min-w-0 text-xs text-muted-foreground", delivery.text));
    item.append(stateRow);

    const reason = adsReviewReasonElement(ad);
    if (reason) item.append(reason);

    const flags = Array.isArray(ad.screenFlags) ? ad.screenFlags : [];
    if (flags.length && ["draft", "pending", "rejected", "withdrawn"].includes(ad.status)) {
      item.append(adsEl("p", "text-xs text-muted-foreground",
        `A reviewer will look at: ${flags.map(adsFlagLabel).join("; ")}. This is a prompt for the reviewer, not a decision.`));
    }

    const meta = adsEl("p", "text-xs text-muted-foreground");
    meta.style.overflowWrap = "anywhere";
    const approvedUntil = ad.status === "approved" && Number(ad.approvedUntil)
      ? ` · Approval valid until ${adsDateTimeLabel(ad.approvedUntil)}` : "";
    meta.textContent = `Runs on: ${adsPlacementLabels(ad.placements).join(", ") || "every surface"} · ${adsScheduleLabel(ad)}${approvedUntil}`;
    item.append(meta);
    const destination = adsEl("p", "font-mono text-[11px] text-muted-foreground", ad.url || "");
    destination.style.overflowWrap = "anywhere";
    item.append(destination);
    item.append(adsEl("p", "text-xs text-muted-foreground tabular-nums", adsClicksLabel(ad)));

    const actions = adsEl("div", "flex flex-wrap items-center gap-2");
    if (ad.status !== "suspended") {
      const edit = adsButton("Edit", { icon: "pencil" });
      edit.addEventListener("click", () => {
        fillAdsForm(ad);
        $("[data-ads-form]")?.scrollIntoView({ behavior: "smooth", block: "start" });
      });
      actions.append(edit);
    }
    // An approval past its 180 days is resubmitted like a draft.
    if (["draft", "rejected", "withdrawn"].includes(ad.status) || ad.delivery === "review-expired") {
      const submit = adsButton("Submit", { icon: "send", tone: "primary" });
      submit.addEventListener("click", () => {
        if (!window.confirm(
          "I confirm this ad follows the ForkMesh Ads policy and is honest about what it promotes.\n\nSubmit it for review?")) return;
        void runAdsOwnerAction(ad, "submit", { attestation: true },
          "Submitted. A person will read it before it runs.");
      });
      actions.append(submit);
    }
    if (ad.status === "approved") {
      const toggle = ad.paused
        ? adsButton("Resume", { icon: "play" })
        : adsButton("Pause", { icon: "pause" });
      toggle.addEventListener("click", () => {
        void runAdsOwnerAction(ad, ad.paused ? "resume" : "pause", {},
          ad.paused ? "Resumed." : "Paused. It keeps its approval; resume it whenever you like.");
      });
      actions.append(toggle);
    }
    if (["pending", "approved", "rejected"].includes(ad.status)) {
      const withdraw = adsButton("Withdraw", { icon: "undo-2" });
      withdraw.addEventListener("click", () => {
        if (!window.confirm(ad.status === "approved"
          ? "Withdraw this ad? It stops running and loses its approval; running it again needs a new review."
          : "Withdraw this ad from review?")) return;
        void runAdsOwnerAction(ad, "withdraw", {}, "Withdrawn.");
      });
      actions.append(withdraw);
    }
    const remove = adsButton("Delete", { icon: "trash-2", tone: "danger" });
    remove.addEventListener("click", () => {
      if (!window.confirm("Delete this ad and its click totals? This cannot be undone.")) return;
      void runAdsOwnerAction(ad, "delete", undefined, "Deleted.");
    });
    actions.append(remove);
    item.append(actions);
    return item;
  }

  function renderAdsList() {
    const root = $("[data-ads-list]");
    const summary = $("[data-ads-list-summary]");
    if (!root) return;
    const view = state.adsView;
    if (view.loading && !view.mine) {
      root.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading your ads...")}</div>`;
      return;
    }
    if (!view.mine) {
      root.replaceChildren(adsEl("div", "px-4 sm:px-5 py-8 text-sm text-muted-foreground",
        "Your ads are unavailable right now."));
      if (summary) summary.textContent = "";
      return;
    }
    const ads = Array.isArray(view.mine.ads) ? view.mine.ads : [];
    if (summary) summary.textContent = ads.length === 1 ? "1 ad" : `${formatCount(ads.length)} ads`;
    if (!ads.length) {
      root.replaceChildren(adsEl("div", "px-4 sm:px-5 py-8 text-sm text-muted-foreground",
        "No ads yet. Write one above - saving a draft costs nothing and shows it to nobody."));
      return;
    }
    root.replaceChildren(...ads.map(adsOwnerItemElement));
    window.lucide?.createIcons();
  }

  // ---- /dashboard/ads: reviewer queue --------------------------------------
  const ADS_REVIEW_TABS = [
    { id: "pending", label: "Pending" },
    { id: "reported", label: "Reported" },
    { id: "approved", label: "Approved" },
    { id: "suspended", label: "Suspended" },
    { id: "rejected", label: "Rejected" },
  ];

  function setAdsReviewNotice(message) {
    state.adsView.review.notice = message || "";
    const box = $("[data-ads-review-notice]");
    if (!box) return;
    box.textContent = state.adsView.review.notice;
    box.hidden = !state.adsView.review.notice;
  }

  function renderAdsReviewTabs() {
    const root = $("[data-ads-review-tabs]");
    if (!root) return;
    const review = state.adsView.review;
    root.replaceChildren();
    for (const tab of ADS_REVIEW_TABS) {
      const active = review.status === tab.id;
      const button = adsEl("button",
        `inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold transition-colors ${active ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-secondary hover:text-foreground"}`,
        tab.label);
      button.type = "button";
      button.setAttribute("role", "tab");
      button.setAttribute("aria-selected", active ? "true" : "false");
      // The Worker counts by stored status; "reported" is a cross-status
      // filter, so its count is only known while that tab is loaded.
      const count = tab.id === "reported"
        ? (active && !review.loading ? review.ads.length : null)
        : Number(review.counts?.[tab.id]) || 0;
      if (count !== null) {
        button.append(adsEl("span", "rounded-full border border-border px-1.5 font-mono text-[10px] text-muted-foreground tabular-nums", formatCount(count)));
      }
      button.addEventListener("click", () => {
        if (review.status === tab.id && !review.error) return;
        review.status = tab.id;
        setAdsReviewNotice("");
        void loadAdsReviewQueue();
      });
      root.append(button);
    }
  }

  function adsReviewDetailElement(ad) {
    const details = adsEl("details", "rounded-md border border-border bg-background p-3");
    details.append(adsEl("summary", "cursor-pointer text-xs font-semibold text-foreground", "History & reports"));
    const body = adsEl("div", "mt-2 grid gap-2 text-xs text-muted-foreground");
    details.append(body);
    let loaded = false;
    details.addEventListener("toggle", async () => {
      if (!details.open || loaded) return;
      loaded = true;
      body.innerHTML = loadingHtml("Loading history...");
      try {
        const data = await adsRequest("GET", `/api/ads/${ad.id}/review`);
        body.replaceChildren();
        const history = Array.isArray(data.history) ? data.history : [];
        const reports = Array.isArray(data.reports) ? data.reports : [];
        body.append(adsEl("p", "font-semibold text-foreground", "Decisions"));
        if (!history.length) body.append(adsEl("p", "", "No decisions recorded yet."));
        for (const entry of history) {
          const codes = (Array.isArray(entry.codes) ? entry.codes : [])
            .map((code) => adsLabelFor(adsProhibitedList(), code));
          const sameCreative = entry.contentHash && entry.contentHash === ad.contentHash;
          const line = adsEl("p", "",
            `${adsDateTimeLabel(entry.createdAt)} - ${entry.action || "decision"}${entry.selfReview ? " (self-review)" : ""}${entry.reason ? `: ${entry.reason}` : ""}${codes.length ? ` [${codes.join("; ")}]` : ""} - ${sameCreative ? "this version" : "an earlier version"}`);
          line.style.overflowWrap = "anywhere";
          body.append(line);
        }
        body.append(adsEl("p", "mt-1 font-semibold text-foreground", "Reports"));
        if (!reports.length) body.append(adsEl("p", "", "No reports."));
        for (const report of reports) {
          const line = adsEl("p", "",
            `${adsDateTimeLabel(report.createdAt)} - ${adsLabelFor(ADS_REPORT_REASONS, report.reason)}${report.note ? `: ${report.note}` : ""}${report.resolved ? " (resolved)" : " (open)"}`);
          line.style.overflowWrap = "anywhere";
          body.append(line);
        }
      } catch (error) {
        loaded = false;
        body.replaceChildren(adsEl("p", "", adsErrorMessage(error)));
      }
    });
    return details;
  }

  async function submitAdsReview(ad, action, fields, status) {
    const body = { action };
    if (action === "approve") body.contentHash = ad.contentHash || "";
    if (action === "reject" || action === "suspend") {
      body.reason = String(fields?.reason || "").trim();
      body.codes = fields?.codes || [];
      if (!body.reason && !body.codes.length) {
        if (status) status.textContent = ADS_ERRORS.reason_required;
        return;
      }
    }
    if (status) status.textContent = "Recording...";
    try {
      const result = await adsRequest("POST", `/api/ads/${ad.id}/review`, body);
      const verbs = { approve: "Approved", reject: "Rejected", suspend: "Suspended", reinstate: "Reinstated to the pending queue" };
      setAdsReviewNotice(`${verbs[action] || "Recorded"}: "${ad.headline || ad.id}".${result.selfReview ? " Recorded as a self-review." : ""}`);
    } catch (error) {
      // A stale hash means the owner edited the ad mid-review: never approve
      // words nobody read. Reload and say so.
      setAdsReviewNotice(adsErrorMessage(error));
    }
    await loadAdsReviewQueue();
    void loadAdsMine();
  }

  function adsReviewDecisionElement(ad) {
    const box = adsEl("div", "grid gap-2");
    const status = adsEl("p", "text-xs text-muted-foreground");
    status.setAttribute("role", "status");
    const row = adsEl("div", "flex flex-wrap items-center gap-2");
    box.append(row);

    const negative = ad.status === "pending" ? "reject" : ad.status === "approved" ? "suspend" : "";
    if (ad.status === "pending") {
      const approve = adsButton("Approve", { icon: "check", tone: "primary" });
      approve.addEventListener("click", () => { void submitAdsReview(ad, "approve", null, status); });
      row.append(approve);
    }
    if (ad.status === "suspended") {
      const reinstate = adsButton("Reinstate", { icon: "rotate-ccw" });
      reinstate.title = "Send it back to the pending queue for a fresh decision";
      reinstate.addEventListener("click", () => { void submitAdsReview(ad, "reinstate", null, status); });
      row.append(reinstate);
    }
    if (negative) {
      const label = negative === "reject" ? "Reject" : "Suspend";
      const details = adsEl("details", "rounded-md border border-border bg-background p-3");
      details.append(adsEl("summary", "cursor-pointer text-xs font-semibold text-foreground", `${label}...`));
      const form = adsEl("div", "mt-2 grid gap-2");
      const reason = adsEl("textarea", "min-h-24 resize-y rounded-md border border-border bg-card px-3 py-2 text-sm font-normal text-foreground placeholder:text-muted-foreground focus:outline-none focus:ring-1 focus:ring-[#2f81f7]");
      reason.rows = 2;
      reason.maxLength = 500;
      reason.placeholder = "Reason the owner will read";
      reason.setAttribute("aria-label", `${label} reason`);
      form.append(reason);
      form.append(adsEl("p", "text-xs text-muted-foreground", "Policy rules this ad breaks (the owner sees them):"));
      const grid = adsEl("div", "grid gap-1 sm:grid-cols-2");
      const boxes = [];
      for (const rule of adsProhibitedList()) {
        const ruleLabel = adsEl("label", "flex items-start gap-2 text-xs text-muted-foreground");
        const input = document.createElement("input");
        input.type = "checkbox";
        input.value = rule.id;
        input.className = "mt-0.5 h-4 w-4 shrink-0 accent-primary";
        boxes.push(input);
        ruleLabel.append(input, adsEl("span", "", rule.label || rule.id));
        grid.append(ruleLabel);
      }
      form.append(grid);
      const confirm = adsButton(`${label} this ad`, { icon: "ban", tone: "danger" });
      confirm.addEventListener("click", () => {
        void submitAdsReview(ad, negative, {
          reason: reason.value,
          codes: boxes.filter((input) => input.checked).map((input) => input.value),
        }, status);
      });
      const confirmRow = adsEl("div", "flex flex-wrap items-center gap-2");
      confirmRow.append(confirm);
      form.append(confirmRow);
      details.append(form);
      box.append(details);
    }
    if (!negative && ad.status !== "suspended") {
      row.append(adsEl("span", "text-xs text-muted-foreground", "No reviewer action applies in this state."));
    }
    box.append(status);
    return box;
  }

  function adsReviewItemElement(ad) {
    const item = adsEl("div", "px-4 sm:px-5 py-4 grid gap-3");
    const viewer = String(state.session?.nodeName || "").toLowerCase();
    if (viewer && String(ad.owner || "").toLowerCase() === viewer) {
      item.append(adsEl("p", "rounded-md border border-amber-500/40 bg-amber-500/10 p-3 text-xs text-foreground",
        "This is your own ad - the decision will be recorded as a self-review."));
    }
    const frame = adsEl("div", "rounded-md border border-border bg-background px-4 py-2");
    frame.append(adCardElement(ad));
    item.append(frame);

    const badges = adsEl("div", "flex flex-wrap items-center gap-2 text-xs text-muted-foreground");
    badges.append(adsBadge(ADS_STATUS_LABELS[ad.status] || ad.status || "Unknown",
      ad.status === "approved" ? "good" : ["rejected", "suspended"].includes(ad.status) ? "bad" : "neutral"));
    const owner = adsEl("a", "font-semibold text-foreground hover:underline", `@${ad.owner || "unknown"}`);
    if (ad.owner && validNodeName(String(ad.owner).toLowerCase())) owner.href = publicProfileUrl(ad.owner);
    badges.append(owner);
    badges.append(adsEl("span", "", `${ad.ownerTier?.label || "Free"} level (${formatCount(ad.ownerTier?.liveAds)} live ads)`));
    const reports = Number(ad.openReports) || 0;
    badges.append(adsBadge(reports === 1 ? "1 open report" : `${formatCount(reports)} open reports`,
      reports ? "bad" : "neutral"));
    item.append(badges);

    const destination = adsEl("div", "grid gap-1");
    destination.append(adsEl("p", "text-[10px] font-semibold uppercase tracking-wide text-muted-foreground", "Destination"));
    const urlText = adsEl("p", "font-mono text-xs text-foreground", ad.url || "");
    urlText.style.overflowWrap = "anywhere";
    destination.append(urlText);
    const href = adsSafeDestination(ad.url);
    if (href) {
      const open = adsEl("a", "inline-flex w-fit items-center gap-1.5 text-xs text-accent underline", "Open destination");
      open.href = href;
      open.target = "_blank";
      open.rel = "noopener noreferrer nofollow";
      open.setAttribute("referrerpolicy", "no-referrer");
      destination.append(open);
    }
    item.append(destination);

    const meta = adsEl("p", "text-xs text-muted-foreground");
    meta.style.overflowWrap = "anywhere";
    meta.textContent = `Runs on: ${adsPlacementLabels(ad.placements).join(", ") || "every surface"} · ${adsScheduleLabel(ad)} · Category: ${ad.categoryLabel || ad.category || "none"}${Number(ad.submittedAt) ? ` · Submitted ${adsDateTimeLabel(ad.submittedAt)}` : ""}`;
    item.append(meta);

    if (ad.note) {
      const note = adsEl("p", "rounded-md border border-border bg-background p-3 text-xs text-foreground", `Owner's note: ${ad.note}`);
      note.style.overflowWrap = "anywhere";
      item.append(note);
    }
    const flags = Array.isArray(ad.screenFlags) ? ad.screenFlags : [];
    if (flags.length) {
      item.append(adsEl("p", "rounded-md border border-amber-500/40 bg-amber-500/10 p-3 text-xs text-foreground",
        `Screening flagged: ${flags.map(adsFlagLabel).join("; ")}. These are prompts to read closely, not decisions.`));
    }
    if (["rejected", "suspended"].includes(ad.status)) {
      const reason = adsReviewReasonElement(ad);
      if (reason) item.append(reason);
    }
    item.append(adsReviewDetailElement(ad));
    item.append(adsReviewDecisionElement(ad));
    return item;
  }

  function renderAdsReview() {
    const section = $("[data-ads-review]");
    if (!section) return;
    const reviewer = state.adsView.mine?.isReviewer === true;
    section.hidden = !reviewer;
    if (!reviewer) return;
    renderAdsReviewTabs();
    setAdsReviewNotice(state.adsView.review.notice);
    const root = $("[data-ads-review-list]");
    if (!root) return;
    const review = state.adsView.review;
    if (review.loading) {
      root.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading the queue...")}</div>`;
      return;
    }
    if (review.error) {
      root.replaceChildren(adsEl("div", "px-4 sm:px-5 py-8 text-sm text-muted-foreground", review.error));
      return;
    }
    if (!review.ads.length) {
      root.replaceChildren(adsEl("div", "px-4 sm:px-5 py-8 text-sm text-muted-foreground",
        review.status === "pending" ? "Nothing is waiting for review." : "Nothing here."));
      return;
    }
    root.replaceChildren(...review.ads.map(adsReviewItemElement));
    window.lucide?.createIcons();
  }

  let adsReviewLoadSequence = 0;
  async function loadAdsReviewQueue() {
    const review = state.adsView.review;
    const sequence = ++adsReviewLoadSequence;
    review.loading = true;
    review.error = "";
    renderAdsReview();
    try {
      const data = await adsRequest("GET",
        `/api/ads/review?status=${encodeURIComponent(review.status)}`);
      if (sequence !== adsReviewLoadSequence) return;
      review.ads = Array.isArray(data.ads) ? data.ads : [];
      review.counts = data.counts && typeof data.counts === "object" ? data.counts : {};
      review.prohibited = Array.isArray(data.prohibited) ? data.prohibited : [];
    } catch (error) {
      if (sequence !== adsReviewLoadSequence) return;
      review.ads = [];
      review.error = adsErrorMessage(error);
    }
    review.loading = false;
    renderAdsReview();
  }

  // ---- /dashboard/ads: loading and wiring ----------------------------------
  function renderAdsPage() {
    renderAdsSession();
    renderAdsPolicy();
    renderAdsList();
    renderAdsReview();
    window.lucide?.createIcons();
  }

  async function loadAdsPolicy() {
    try {
      state.adsView.policy = await adsPublicRead("/api/ads/policy");
      state.adsView.policyError = "";
    } catch (_) {
      state.adsView.policyError = "The ad policy is unavailable right now. Refresh to try again.";
    }
    renderAdsPolicy();
    renderAdsList();
  }

  async function loadAdsMine() {
    if (!adsSignedIn()) {
      state.adsView.loading = false;
      state.adsView.mine = null;
      renderAdsPage();
      return;
    }
    state.adsView.loading = true;
    state.adsView.error = "";
    renderAdsSession();
    try {
      state.adsView.mine = await adsRequest("GET", "/api/ads/mine");
    } catch (error) {
      state.adsView.mine = null;
      state.adsView.error = error.status === 401 ? "unauthorized" : adsErrorMessage(error);
    }
    state.adsView.loading = false;
    renderAdsPage();
  }

  function initAdsPage() {
    const panel = $("[data-ads-panel]");
    if (!panel) return;
    const form = $("[data-ads-form]");
    form?.addEventListener("input", () => {
      updateAdsCounters();
      updateAdsPreview();
    });
    form?.addEventListener("change", (event) => {
      // A category the owner picked by hand outranks the one an edit loaded.
      if (event.target?.matches?.('[data-ads-field="category"]')) {
        delete event.target.dataset.pending;
      }
      updateAdsPreview();
    });
    form?.addEventListener("submit", (event) => {
      event.preventDefault();
      // Enter in a field follows the visible primary action: submitting when
      // that applies, otherwise saving the edit.
      void saveAdsForm($("[data-ads-submit]")?.hidden !== true);
    });
    $("[data-ads-save-draft]")?.addEventListener("click", () => { void saveAdsForm(false); });
    $("[data-ads-form-reset]")?.addEventListener("click", () => fillAdsForm(null));
    $("[data-ads-refresh]")?.addEventListener("click", async () => {
      setAdsNotice("");
      void loadAdsPolicy();
      await loadAdsMine();
      if (state.adsView.mine?.isReviewer) void loadAdsReviewQueue();
    });
    renderAdsPage();
    fillAdsForm(null);
    void loadAdsPolicy();
    void loadAdsMine().then(() => {
      if (state.adsView.mine?.isReviewer) void loadAdsReviewQueue();
    });
  }

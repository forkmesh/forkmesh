// 09-referrals.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["referrals"] = (core) => {
  const { $, copyTextToClipboard, escapeHtml, fetchJson, formatCount, loadingHtml, publicProfileUrl, publicSiteOrigin, state, validNodeName } = core;
  // ---------------------------------------------------------------------------
  // Referrals page (/referrals). The community referral program the World
  // leaderboard island already shows, as a composed dashboard page on the URL
  // the site header, footer and privacy page have always linked: your share
  // link, three-step instructions, the public top-10 board, and the inbound
  // referring-site board. Both boards are public reads, so signed-out visitors
  // arriving from the marketing chrome see the accepted account names too.
  function referralAccountName() {
    // Same acceptance rule as the website and the World: only real user
    // accounts own a referral link, never node sessions.
    const session = state.session;
    if (!session?.nodeName || session.kind === "node") return "";
    if (session.kind !== "user" && !session.email) return "";
    return String(session.nodeName).toLowerCase();
  }

  function referralShareLink() {
    const name = referralAccountName();
    // Spelled on the apex: the microjobs verifier looks for
    // forkmesh.com/r/<name> in the posted page, so handing the user an
    // app.forkmesh.com link made their own share fail the referral claim.
    return name ? `${publicSiteOrigin()}/r/${encodeURIComponent(name)}` : "";
  }

  function referralPlural(value, word) {
    const count = Number(value) || 0;
    return `${formatCount(count)} ${word}${count === 1 ? "" : "s"}`;
  }

  // The viewer's own row, when the public board carries one. The board is the
  // top 10, so an absent row means "not ranked yet" rather than "zero" — the
  // two are rendered differently so nobody reads an unranked account as idle.
  function referralViewerEntry() {
    const name = referralAccountName();
    if (!name) return null;
    const index = state.referralsView.board.findIndex(
      (row) => String(row.name || "").toLowerCase() === name);
    if (index < 0) return null;
    return { rank: index + 1, row: state.referralsView.board[index] };
  }

  function referralStatTileHtml(label, value, hint) {
    return `<div class="rounded-md border border-border bg-background p-3">
      <p class="text-xs text-muted-foreground">${escapeHtml(label)}</p>
      <p class="mt-1 text-2xl font-semibold text-foreground tabular-nums">${escapeHtml(value)}</p>
      <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(hint)}</p>
    </div>`;
  }

  function renderReferralStats() {
    const root = $("[data-referrals-stats]");
    if (!root) return;
    if (!referralAccountName() || state.referralsView.loading ||
        state.referralsView.error) {
      root.innerHTML = "";
      return;
    }
    const entry = referralViewerEntry();
    root.innerHTML = [
      referralStatTileHtml(
        "Your clicks", entry ? formatCount(entry.row.clicks) : "0",
        "Visits your link has sent here"),
      referralStatTileHtml(
        "Your signups", entry ? formatCount(entry.row.signups) : "0",
        "Accounts created through it"),
      referralStatTileHtml(
        "Your rank", entry ? `#${entry.rank}` : "Unranked",
        entry ? "On the public top 10" : "One signup puts you on the board"),
    ].join("");
  }

  function renderReferralLinkArea() {
    const root = $("[data-referrals-link-area]");
    if (!root) return;
    const link = referralShareLink();
    if (!link) {
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      root.innerHTML = `<p class="text-sm text-muted-foreground">
        <a class="text-accent underline" href="/login?next=${next}">Log in</a>
        or <a class="text-accent underline" href="/signup">create an account</a>
        to get your personal referral link. The leaderboard below is public.
      </p>`;
      return;
    }
    root.innerHTML = `<div class="flex flex-wrap items-center gap-2">
      <input
        data-referrals-link
        type="text"
        readonly
        aria-label="Your referral link"
        value="${escapeHtml(link)}"
        class="min-w-0 flex-1 rounded-md border border-border bg-background px-3 py-2 font-mono text-sm text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20"
      />
      <button
        data-referrals-copy
        type="button"
        class="shrink-0 inline-flex items-center gap-1.5 rounded-md bg-primary px-3 py-2 text-xs font-semibold text-primary-foreground hover:opacity-90 transition-colors"
      >
        <i data-lucide="copy" class="w-3.5 h-3.5"></i>
        Copy link
      </button>
    </div>`;
    window.lucide?.createIcons();
  }

  function referralRowHtml(row, index) {
    const rank = index + 1;
    const name = String(row.name || "user");
    const you = referralAccountName() &&
      name.toLowerCase() === referralAccountName();
    const rankClass = rank === 1
      ? "text-amber-500"
      : rank <= 3 ? "text-foreground" : "text-muted-foreground";
    const referredUsers = Array.from(new Set(
      (Array.isArray(row.referredUsers) ? row.referredUsers : [])
        .map((value) => String(value || "").trim().toLowerCase())
        .filter(validNodeName)
    ));
    const referredLinks = referredUsers.map((referredName) =>
      `<a class="text-muted-foreground hover:text-foreground hover:underline" href="${escapeHtml(publicProfileUrl(referredName))}">${escapeHtml(referredName)}</a>`
    ).join('<span class="text-muted-foreground">, </span>');
    return `<div class="px-4 sm:px-5 py-3 flex items-center gap-3${you ? " bg-secondary" : ""}">
      <span class="w-6 shrink-0 font-mono text-sm font-semibold tabular-nums ${rankClass}">${rank}</span>
      <div class="min-w-0 flex-1">
        <a class="block truncate text-sm font-semibold text-foreground hover:underline" href="${escapeHtml(publicProfileUrl(name))}">${escapeHtml(name)}</a>
        ${referredLinks ? `<p class="mt-0.5 truncate text-[11px]">${referredLinks}</p>` : ""}
      </div>
      ${you ? '<span class="shrink-0 rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold text-muted-foreground">you</span>' : ""}
      <span class="shrink-0 font-mono text-xs text-muted-foreground tabular-nums">${escapeHtml(referralPlural(row.clicks, "click"))} · ${escapeHtml(referralPlural(row.signups, "signup"))}</span>
    </div>`;
  }

  function renderReferralsPage() {
    const board = $("[data-referrals-board]");
    const count = $("[data-referrals-count]");
    if (!board) return;
    renderReferralLinkArea();
    if (state.referralsView.loading || state.referralsView.error) {
      board.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${
        state.referralsView.loading
          ? loadingHtml("Loading leaderboard...")
          : escapeHtml(state.referralsView.error)
      }</div>`;
      if (count) count.textContent = state.referralsView.loading ? "Loading" : "Unavailable";
      renderReferralStats();
      return;
    }
    board.innerHTML = state.referralsView.board.length
      ? state.referralsView.board.map(referralRowHtml).join("")
      : '<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">No referrals counted yet - share your link and be the first on the board.</div>';
    if (count) {
      count.textContent = state.referralsView.board.length
        ? `${state.referralsView.board.length} ranked`
        : "0 ranked";
    }
    renderReferralStats();
  }

  // How deep the public referring-site board runs. Matches
  // SITE_REFERRER_LEADERBOARD_LIMIT in the Worker; only the wording here
  // depends on it, so a drift shows up as a stale count and never as a
  // truncated board.
  const REFERRAL_SITE_BOARD_LIMIT = 25;

  // The latest referring page for one row, re-checked here before it becomes
  // a link. Referer is attacker-controlled, so a row may only hand out an
  // http(s) URL on the very hostname it is counted under — never one pointing
  // somewhere else, and never a javascript:/data: string escapeHtml would
  // otherwise pass straight into href. The Worker applies the same rule
  // before storing; this is the second half of it, at the point the anchor is
  // built.
  function referralSiteUrl(row) {
    const host = String(row?.host || "").trim().toLowerCase();
    if (!host) return "";
    try {
      const candidate = new URL(String(row?.url || ""));
      const candidateHost =
        candidate.hostname.toLowerCase().replace(/^www\./, "");
      if (!["http:", "https:"].includes(candidate.protocol) ||
          candidateHost !== host ||
          candidate.username || candidate.password || candidate.hash) {
        return "";
      }
      return candidate.href.slice(0, 1024);
    } catch (_) {
      return "";
    }
  }

  // The hostname itself stays plain text — it is other people's header data,
  // not a destination this board vouches for. The one link a row carries is
  // its own latest referring page, marked nofollow/ugc so ranking a site here
  // is worth nothing to a referrer spammer.
  function referralSiteRowHtml(row, index) {
    const href = referralSiteUrl(row);
    return `<div class="px-4 sm:px-5 py-3 flex items-center gap-3">
      <span class="w-6 shrink-0 font-mono text-sm font-semibold tabular-nums text-muted-foreground">${index + 1}</span>
      <div class="min-w-0 flex-1">
        <p class="truncate text-sm text-foreground">${escapeHtml(String(row.host || ""))}</p>
        ${href
          ? `<a href="${escapeHtml(href)}" target="_blank" rel="nofollow ugc noopener noreferrer" title="${escapeHtml(href)}" class="mt-0.5 block truncate font-mono text-[11px] text-muted-foreground hover:text-foreground hover:underline">${escapeHtml(href)}</a>`
          : '<p class="mt-0.5 truncate font-mono text-[11px] text-muted-foreground">Full page URL captures on the next referral</p>'}
      </div>
      <span class="shrink-0 font-mono text-xs text-muted-foreground tabular-nums">${escapeHtml(referralPlural(row.visits, "visit"))}</span>
    </div>`;
  }

  function renderReferralSites() {
    const root = $("[data-referrals-sites]");
    const summary = $("[data-referrals-sites-summary]");
    if (!root) return;
    if (state.referralsView.sitesLoading || state.referralsView.sitesError) {
      root.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${
        state.referralsView.sitesLoading
          ? loadingHtml("Loading referring sites...")
          : escapeHtml(state.referralsView.sitesError)
      }</div>`;
      return;
    }
    root.innerHTML = state.referralsView.sites.length
      ? state.referralsView.sites.map(referralSiteRowHtml).join("")
      : '<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">No website referrals counted yet.</div>';
    if (summary) {
      const { sites, visits } = state.referralsView.sitesTotals;
      summary.textContent = sites
        ? `${referralPlural(sites, "site")} · ${referralPlural(visits, "visit")} · top ${REFERRAL_SITE_BOARD_LIMIT} by visits`
        : `Which sites link visitors to ForkMesh - top ${REFERRAL_SITE_BOARD_LIMIT} by visits`;
    }
  }

  async function loadReferralSites(options = {}) {
    state.referralsView.sitesLoading = true;
    state.referralsView.sitesError = "";
    renderReferralSites();
    try {
      const data = await fetchJson("/api/referrals/sites", {
        fresh: options.fresh === true,
      });
      state.referralsView.sites = Array.isArray(data.board) ? data.board : [];
      state.referralsView.sitesTotals = {
        sites: Number(data.sites) || 0,
        visits: Number(data.visits) || 0,
      };
    } catch (_) {
      state.referralsView.sites = [];
      state.referralsView.sitesError =
        "The referring-site board is unavailable right now.";
    } finally {
      state.referralsView.sitesLoading = false;
      renderReferralSites();
    }
  }

  async function loadReferralBoard(options = {}) {
    state.referralsView.loading = true;
    state.referralsView.error = "";
    renderReferralsPage();
    try {
      const data = await fetchJson("/api/referrals/leaderboard", {
        fresh: options.fresh === true,
      });
      state.referralsView.board = Array.isArray(data.board) ? data.board : [];
    } catch (_) {
      state.referralsView.board = [];
      state.referralsView.error = "The leaderboard is unavailable right now.";
    } finally {
      state.referralsView.loading = false;
      renderReferralsPage();
    }
  }

  function initReferralsPage() {
    const panel = $("[data-referrals-panel]");
    panel?.addEventListener("click", async (event) => {
      if (event.target.closest("[data-referrals-refresh]")) {
        void loadReferralBoard({ fresh: true });
        void loadReferralSites({ fresh: true });
        return;
      }
      const copy = event.target.closest("[data-referrals-copy]");
      if (!copy) return;
      const input = $("[data-referrals-link]");
      const copied = await copyTextToClipboard(input?.value || referralShareLink());
      copy.innerHTML = copied
        ? '<i data-lucide="check" class="w-3.5 h-3.5"></i>Copied'
        : '<i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy failed';
      window.lucide?.createIcons();
      window.setTimeout(() => {
        if (!document.body.contains(copy)) return;
        copy.innerHTML = '<i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy link';
        window.lucide?.createIcons();
      }, 1600);
    });
    void loadReferralBoard();
    void loadReferralSites();
  }
  return { initReferralsPage };
  };
})();

(() => {
  const grid = document.querySelector("[data-leaderboards-grid]");
  if (!grid) return;

  let snapshot = null;
  let activeCategory = "all";
  let searchTerm = "";
  let refreshTimer = 0;
  let mineOnly = false;
  const MEMBERLESS_BOARDS = new Set(["referring-sites"]);

  function signedInAccount() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!session?.nodeName || session.kind === "node") return "";
      return String(session.nodeName).trim().toLowerCase();
    } catch (_) {
      return "";
    }
  }

  function isMine(board, row, account) {
    if (!account || MEMBERLESS_BOARDS.has(board.id)) return false;
    const name = String(row?.name || "").trim().toLowerCase();
    return name === account || name.startsWith(`${account}/`);
  }

  const plural = (value, word) => {
    const number = Math.max(0, Number(value) || 0);
    return `${number.toLocaleString()} ${word}${number === 1 ? "" : "s"}`;
  };

  function formatBytes(value) {
    let bytes = Math.max(0, Number(value) || 0);
    const units = ["B", "KB", "MB", "GB", "TB", "PB"];
    let unit = 0;
    while (bytes >= 1024 && unit < units.length - 1) {
      bytes /= 1024;
      unit += 1;
    }
    return `${unit ? bytes.toFixed(bytes >= 100 ? 0 : 1) : Math.round(bytes)} ${units[unit]}`;
  }

  function formatDuration(value) {
    let seconds = Math.floor(Math.max(0, Number(value) || 0) / 1000);
    const days = Math.floor(seconds / 86400);
    seconds %= 86400;
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    if (days) return `${days}d ${hours}h`;
    if (hours) return `${hours}h ${minutes}m`;
    return `${minutes}m`;
  }

  function formatAge(value) {
    const milliseconds = Math.max(0, Number(value) || 0);
    const days = Math.floor(milliseconds / 86400000);
    if (days) return plural(days, "day");
    return plural(Math.floor(milliseconds / 3600000), "hour");
  }

  function formatValue(board, row) {
    switch (board.valueKind) {
      case "duration":
        return formatDuration(row.totalActiveMs);
      case "minutes": {
        const minutes = Math.max(0, Number(row.minutes) || 0);
        return minutes >= 60
          ? `${Math.floor(minutes / 60)}h ${minutes % 60}m`
          : `${minutes}m`;
      }
      case "bytes":
        return formatBytes(row.bytes ?? row.sizeBytes);
      case "repos":
        return plural(row.repos, "repo");
      case "mirrors":
        return plural(row.mirrors, "owner");
      case "age":
        return formatAge(row.ageMs);
      case "contributions":
        return plural(row.total, "contribution");
      case "issues":
        return plural(row.issues, "issue");
      case "referrals":
        return `${plural(row.signups, "signup")} · ${plural(row.clicks, "click")}`;
      case "visits":
        return plural(row.visits, "visit");
      case "sol": {
        const sol = Number(row.sol) || (Number(row.lamports) || 0) / 1e9;
        return `${sol.toFixed(sol >= 1 ? 2 : 4)} SOL`;
      }
      default:
        return String(row.value ?? "—");
    }
  }

  function rowDetail(board, row) {
    if (board.id === "contributors") {
      return `${plural(row.issues, "issue")} · ${plural(row.pulls, "PR")} · ${plural(row.commits, "commit")}`;
    }
    if (board.id === "node-storage") {
      return [
        row.platform,
        row.version,
        row.commit ? String(row.commit).slice(0, 10) : "",
      ].filter(Boolean).join(" · ");
    }
    if (board.id === "referring-sites") {
      return "Aggregate HTTP referrals";
    }
    if (board.id === "tip-recipients") {
      // How the total was reached: many small tips and one large one are very
      // different stories behind the same SOL figure.
      return `${plural(row.tips, "tip")} · ${plural(row.senders, "sender")}`;
    }
    if (board.id === "wallets") {
      // The published payout address itself, abbreviated the way the rest of
      // the site shows one. Public profile data, not a claim on any funds.
      const wallet = String(row.wallet || "");
      return wallet.length > 10
        ? `${wallet.slice(0, 4)}…${wallet.slice(-4)}`
        : wallet;
    }
    return "";
  }

  // Re-read the published payout balances from Solana, then repaint the board
  // from the fresh rows. The relay floors this per address, so a repeated press
  // costs nothing extra.
  async function refreshWalletBalances() {
    const response = await fetch("/api/leaderboards/wallets?refresh=1", {
      headers: { accept: "application/json" },
      cache: "no-store",
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const body = await response.json();
    const rows = Array.isArray(body.board) ? body.board : [];
    const boards = Array.isArray(snapshot?.boards) ? snapshot.boards : [];
    const wallets = boards.find((entry) => entry.id === "wallets");
    if (!wallets) return;
    wallets.rows = rows;
    wallets.degraded = false;
    render(snapshot);
  }

  function makeWalletsRefresh() {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "leaderboard-board-refresh";
    button.textContent = "Refresh balances";
    button.title = "Read these payout balances from Solana again";
    button.addEventListener("click", async () => {
      if (button.disabled) return;
      button.disabled = true;
      button.textContent = "Refreshing…";
      try {
        await refreshWalletBalances();
      } catch (_) {
        button.textContent = "Refresh failed";
        button.disabled = false;
      }
    });
    return button;
  }

  function makeCard(board) {
    const account = signedInAccount();
    const card = document.createElement("article");
    card.className = "leaderboard-card";
    card.dataset.mine = String(
      Array.isArray(board.rows) && board.rows.some((row) => isMine(board, row, account)),
    );
    card.dataset.category = String(board.category || "");
    card.dataset.search = [
      board.title,
      board.subtitle,
      ...(Array.isArray(board.rows)
        ? board.rows.flatMap((row) => [row.name, row.host])
        : []),
    ].join(" ").toLowerCase();

    const header = document.createElement("header");
    header.className = "leaderboard-card-header";
    const category = document.createElement("p");
    category.className = "leaderboard-category";
    category.textContent = String(board.category || "public");
    const title = document.createElement("h2");
    title.textContent = String(board.title || "Leaderboard");
    const subtitle = document.createElement("p");
    subtitle.className = "leaderboard-card-subtitle";
    subtitle.textContent = String(board.subtitle || "");
    header.append(category, title, subtitle);
    // The SOL board is the only one whose numbers cost a public-RPC read, so it
    // is the only one that carries its own refresh: the hub (and its background
    // five-minute reload) always answers from the relay's hourly balance cache.
    if (board.id === "wallets") header.appendChild(makeWalletsRefresh());
    card.appendChild(header);

    const rows = Array.isArray(board.rows) ? board.rows : [];
    if (!rows.length) {
      const empty = document.createElement("p");
      empty.className = "leaderboard-empty";
      // A board whose source could not be read this rebuild is empty for a
      // reason the visitor should not have to guess at: the endpoint marks
      // it, so say "unavailable" rather than "nobody has entered yet".
      empty.textContent = board.degraded
        ? "Temporarily unavailable."
        : "No public data yet.";
      card.appendChild(empty);
      return card;
    }
    rows.forEach((row, index) => {
      const item = document.createElement("div");
      item.className = "leaderboard-row";
      if (isMine(board, row, account)) item.classList.add("is-mine");
      const rank = document.createElement("span");
      rank.className = "leaderboard-rank";
      rank.textContent = String(index + 1).padStart(2, "0");
      const identity = document.createElement("span");
      identity.className = "leaderboard-name";
      const displayName = String(row.name || row.host || "participant");
      if (row.profileUrl) {
        const profile = document.createElement("a");
        profile.className = "leaderboard-profile-link";
        profile.href = String(row.profileUrl);
        profile.textContent = displayName;
        identity.appendChild(profile);
      } else {
        identity.appendChild(document.createTextNode(displayName));
      }
      if (row.mastodon && row.mastodonUrl) {
        const mastodon = document.createElement("a");
        mastodon.className = "leaderboard-detail leaderboard-mastodon-link";
        mastodon.href = String(row.mastodonUrl);
        mastodon.target = "_blank";
        mastodon.rel = "noopener noreferrer";
        mastodon.textContent = String(row.mastodon);
        identity.appendChild(mastodon);
      }
      const detailText = rowDetail(board, row);
      if (detailText) {
        const detail = document.createElement("small");
        detail.className = "leaderboard-detail";
        detail.textContent = detailText;
        identity.appendChild(detail);
      }
      const value = document.createElement("strong");
      value.className = "leaderboard-value";
      value.textContent = formatValue(board, row);
      item.append(rank, identity, value);
      card.appendChild(item);
    });
    return card;
  }

  function applyFilters() {
    let visible = 0;
    grid.querySelectorAll(".leaderboard-card").forEach((card) => {
      const categoryMatches =
        activeCategory === "all" || card.dataset.category === activeCategory;
      const searchMatches =
        !searchTerm || String(card.dataset.search || "").includes(searchTerm);
      const mineMatches = !mineOnly || card.dataset.mine === "true";
      card.hidden = !(categoryMatches && searchMatches && mineMatches);
      if (!card.hidden) visible += 1;
    });
    let empty = grid.querySelector("[data-leaderboards-filter-empty]");
    if (!visible) {
      if (!empty) {
        empty = document.createElement("p");
        empty.className = "leaderboards-empty";
        empty.dataset.leaderboardsFilterEmpty = "true";
        empty.textContent = "No boards match those filters.";
        grid.appendChild(empty);
      }
    } else {
      empty?.remove();
    }
  }

  function myPlacements(account) {
    const boards = Array.isArray(snapshot?.boards) ? snapshot.boards : [];
    return boards.flatMap((board) => {
      const rows = Array.isArray(board.rows) ? board.rows : [];
      const index = rows.findIndex((row) => isMine(board, row, account));
      if (index < 0) return [];
      const entries = rows.filter((row) => isMine(board, row, account)).length;
      return [{ board, rank: index + 1, row: rows[index], entries }];
    });
  }

  function renderMine() {
    const panel = document.querySelector("[data-leaderboards-mine-panel]");
    const button = document.querySelector("[data-leaderboards-mine]");
    button?.setAttribute("aria-pressed", String(mineOnly));
    if (!panel) return;
    panel.hidden = !mineOnly;
    if (!mineOnly) return;
    const account = signedInAccount();
    const heading = document.createElement("h2");
    heading.id = "leaderboards-mine-title";
    heading.textContent = `Your stats · ${account}`;
    const summary = document.createElement("p");
    const actions = document.createElement("div");
    actions.className = "leaderboards-mine-actions";
    const profile = document.createElement("a");
    profile.className = "kit-btn";
    profile.href = `/@${encodeURIComponent(account)}`;
    profile.textContent = "Open your profile";
    const close = document.createElement("button");
    close.type = "button";
    close.className = "leaderboards-filter";
    close.textContent = "Show every board";
    close.addEventListener("click", () => setMineOnly(false));
    actions.append(profile, close);
    if (!snapshot) {
      summary.textContent = "Loading your stats…";
      panel.replaceChildren(heading, summary, actions);
      return;
    }
    const placements = myPlacements(account);
    const total = Array.isArray(snapshot.boards) ? snapshot.boards.length : 0;
    if (!placements.length) {
      summary.textContent =
        `You are not on any of the ${plural(total, "board")} yet. Each board lists only its top entries.`;
      panel.replaceChildren(heading, summary, actions);
      return;
    }
    const best = Math.min(...placements.map((placement) => placement.rank));
    summary.textContent =
      `Ranked on ${placements.length.toLocaleString()} of ${plural(total, "board")} · best rank #${best}.`;
    const list = document.createElement("ul");
    list.className = "leaderboards-mine-list";
    placements.forEach(({ board, rank, row, entries }) => {
      const item = document.createElement("li");
      const place = document.createElement("strong");
      place.textContent = `#${rank}`;
      const title = document.createElement("span");
      title.textContent = String(board.title || "Leaderboard");
      const value = document.createElement("span");
      value.textContent = entries > 1
        ? `${formatValue(board, row)} · ${entries.toLocaleString()} entries`
        : formatValue(board, row);
      item.append(place, title, value);
      list.appendChild(item);
    });
    panel.replaceChildren(heading, summary, list, actions);
  }

  function setMineOnly(enabled) {
    mineOnly = enabled;
    renderMine();
    applyFilters();
  }

  function render(data) {
    snapshot = data;
    const boards = Array.isArray(data?.boards) ? data.boards : [];
    grid.replaceChildren(...boards.map(makeCard));
    document.querySelector("[data-leaderboards-count]").textContent =
      boards.length.toLocaleString();
    document.querySelector("[data-leaderboards-summary]").textContent =
      `${boards.reduce((sum, board) => sum + (Array.isArray(board.rows) ? board.rows.length : 0), 0).toLocaleString()} ranked public entries across the network and community.`;
    const disclosure = document.querySelector("[data-leaderboards-disclosure]");
    if (disclosure && data?.fundsNotice) disclosure.textContent = data.fundsNotice;
    renderMine();
    applyFilters();
  }

  async function load({ quiet = false } = {}) {
    const status = document.querySelector("[data-leaderboards-status]");
    if (!quiet) status.textContent = "Refreshing…";
    try {
      const response = await fetch("/api/leaderboards", {
        headers: { accept: "application/json" },
        cache: "no-store",
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      render(data);
      const observedAt = Number(data.observedAt) || Date.now();
      status.textContent = `Updated ${new Date(observedAt).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}`;
      return true;
    } catch (_) {
      status.textContent = snapshot ? "Refresh failed · showing last result" : "Unavailable";
      if (!snapshot) {
        grid.innerHTML =
          '<p class="leaderboards-empty">Leaderboard data is temporarily unavailable.</p>';
      }
      return false;
    }
  }

  document.querySelector("[data-leaderboards-search]")?.addEventListener(
    "input",
    (event) => {
      searchTerm = String(event.currentTarget.value || "").trim().toLowerCase();
      applyFilters();
    },
  );
  const categoryFilters = document.querySelectorAll(".leaderboards-filter");
  categoryFilters.forEach((button) => {
    button.addEventListener("click", () => {
      activeCategory = button.dataset.category || "all";
      categoryFilters.forEach((candidate) => {
        candidate.setAttribute(
          "aria-pressed",
          String(candidate === button),
        );
      });
      applyFilters();
    });
  });
  document.querySelector("[data-leaderboards-mine]")?.addEventListener(
    "click",
    () => {
      if (!signedInAccount()) {
        location.href = `/login?next=${encodeURIComponent("/leaderboards?mine=1")}`;
        return;
      }
      setMineOnly(!mineOnly);
      if (mineOnly) {
        document.querySelector("[data-leaderboards-mine-panel]")?.scrollIntoView({
          behavior: "smooth",
          block: "nearest",
        });
      }
    },
  );
  if (new URLSearchParams(location.search).get("mine") === "1" && signedInAccount()) {
    setMineOnly(true);
  }
  document.querySelector("[data-leaderboards-refresh]")?.addEventListener(
    "click",
    () => void load(),
  );

  function scheduleRefresh() {
    window.clearTimeout(refreshTimer);
    if (document.hidden) return;
    refreshTimer = window.setTimeout(async () => {
      await load({ quiet: true });
      scheduleRefresh();
    }, 5 * 60_000);
  }
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) {
      void load({ quiet: true });
      scheduleRefresh();
    } else {
      window.clearTimeout(refreshTimer);
    }
  });
  void load().then(scheduleRefresh);
})();

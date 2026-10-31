(() => {
  function applyTheme(mode) {
    const stored = mode ||
      localStorage.getItem("forkmesh.dashboard.theme") ||
      localStorage.getItem("forkmesh.theme") || "system";
    const chosen = stored === "system"
      ? (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light")
      : stored;
    // styles.css palettes key off BOTH classes now: html.dark (the default
    // palette) and html.light (the light palette added with site-wide theme
    // support) — stamp them together so every styles.css page responds.
    document.documentElement.classList.toggle("dark", chosen === "dark");
    document.documentElement.classList.toggle("light", chosen !== "dark");
    document.documentElement.style.colorScheme =
      chosen === "dark" ? "dark" : "light";
    if (stored === "system" && !mode) return;
    localStorage.setItem("forkmesh.theme", chosen);
    localStorage.setItem("forkmesh.dashboard.theme", chosen);
  }

  async function copyText(text, button) {
    try {
      await navigator.clipboard.writeText(text);
    } catch (error) {
      const area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.left = "-9999px";
      document.body.append(area);
      area.select();
      document.execCommand("copy");
      area.remove();
    }
    if (button) {
      const original = button.textContent;
      button.textContent = "Copied";
      setTimeout(() => (button.textContent = original), 1500);
    }
  }

  function setText(selector, value) {
    document.querySelectorAll(selector).forEach((el) => {
      el.textContent = value;
    });
  }

  // The last-known counts are cached so the header pill and stat cards render
  // instantly on a cold load instead of sitting at "Checking…"/"—" until the
  // first poll returns (stale-while-revalidate). Values are non-negative numbers.
  function readCachedStat(key) {
    try {
      const raw = localStorage.getItem(`forkmesh.stats.${key}`);
      if (raw === null || raw === "") return null;
      const n = Number(raw);
      return Number.isFinite(n) && n >= 0 ? n : null;
    } catch (error) {
      return null;
    }
  }
  function writeCachedStat(key, value) {
    try {
      localStorage.setItem(`forkmesh.stats.${key}`, String(value));
    } catch (error) {
      /* storage disabled or quota exceeded — caching is best-effort */
    }
  }

  function shortWallet(address) {
    if (!address) return "No payout wallet";
    return address.length > 16 ? `${address.slice(0, 8)}…${address.slice(-6)}` : address;
  }

  function eligibilityLabel(node) {
    if (node.payoutEligible) return "Eligible";
    if (node.eligibilityReason === "offline") return "Offline";
    if (node.eligibilityReason === "duplicate_wallet") return "Duplicate wallet";
    if (node.eligibilityReason === "no_mirrors") return "Not mirroring";
    return "No wallet";
  }

  const PAYOUT_ROWS_SHOWN = 6;

  function payoutRank(node) {
    if (node.payoutEligible) return 0;
    if (node.wallet) return 1;
    return node.online ? 2 : 3;
  }

  // Public balances are served from an hourly cache, so say how old the number
  // is rather than implying it is live. Anything under a minute reads as "just
  // now" — the exact second is noise next to an hour-wide window.
  function balanceAgeLabel(checkedAt) {
    const stamp = Number(checkedAt) || 0;
    if (!stamp) return "";
    const minutes = Math.floor((Date.now() - stamp) / 60000);
    if (minutes < 1) return "just now";
    if (minutes < 60) return `${minutes} min ago`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return hours === 1 ? "1 hour ago" : `${hours} hours ago`;
    const days = Math.floor(hours / 24);
    return days === 1 ? "1 day ago" : `${days} days ago`;
  }

  function renderPayoutNodes(nodes) {
    const root = document.querySelector("#network-wallets");
    if (!root) return;
    const list = (Array.isArray(nodes) ? nodes : []).slice().sort((left, right) => (
      payoutRank(left) - payoutRank(right)
      || String(left.name || "").localeCompare(String(right.name || ""))
    ));
    if (!list.length) {
      root.innerHTML = '<div class="online-graph-empty">No user payout wallets are listed yet.</div>';
      return;
    }
    root.innerHTML = "";
    const headings = ["User", "Wallet", "Balance", "Payouts"];
    const heading = document.createElement("div");
    heading.className = "wallet-row wallet-heading";
    headings.forEach((text) => {
      const cell = document.createElement("div");
      cell.textContent = text;
      heading.appendChild(cell);
    });
    root.appendChild(heading);
    const overflow = [];
    list.forEach((node, index) => {
      const row = document.createElement("div");
      row.className = "wallet-row";
      if (index >= PAYOUT_ROWS_SHOWN) {
        row.hidden = true;
        overflow.push(row);
      }

      const name = document.createElement("div");
      name.className = "wallet-node";
      name.textContent = node.name || "node";
      name.title = node.name || "node";
      row.appendChild(name);

      const wallet = document.createElement("div");
      wallet.className = node.wallet ? "wallet-address" : "wallet-address is-empty";
      wallet.textContent = shortWallet(node.wallet || "");
      if (node.wallet) wallet.title = node.wallet;
      row.appendChild(wallet);

      const balance = document.createElement("div");
      balance.className = node.balanceSol ? "wallet-balance" : "wallet-balance is-empty";
      balance.textContent = node.balanceSol
        ? `${node.balanceSol} SOL`
        : node.wallet ? "Unavailable" : "—";
      const age = balanceAgeLabel(node.balanceCheckedAt);
      if (age) balance.title = `Balance last checked ${age}`;
      row.appendChild(balance);

      const status = document.createElement("div");
      status.className = "wallet-status";
      status.dataset.eligible = node.payoutEligible ? "true" : "false";
      status.dataset.reason = String(node.eligibilityReason || "");
      status.textContent = eligibilityLabel(node);
      row.appendChild(status);

      root.appendChild(row);
    });

    if (overflow.length) {
      const more = document.createElement("button");
      more.type = "button";
      more.className = "wallet-more";
      const label = (open) => (open ? "Show fewer" : `Show all ${list.length} users`);
      more.textContent = label(false);
      more.setAttribute("aria-expanded", "false");
      more.addEventListener("click", () => {
        const open = more.getAttribute("aria-expanded") !== "true";
        overflow.forEach((row) => {
          row.hidden = !open;
        });
        more.setAttribute("aria-expanded", String(open));
        more.textContent = label(open);
      });
      root.appendChild(more);
    }

    const newest = list.reduce(
      (max, node) => Math.max(max, Number(node.balanceCheckedAt) || 0), 0);
    const age = balanceAgeLabel(newest);
    if (age) {
      const note = document.createElement("div");
      note.className = "balance-checked";
      note.textContent = `Balances checked ${age}. Press Refresh to read them from Solana again.`;
      root.appendChild(note);
    }
  }

  applyTheme();

  // Mobile navigation: the shared .global-nav is hidden on narrow screens, so
  // inject a hamburger that reveals it as a dropdown panel. Without this the
  // header links (Repositories, Network, Docs, Sign Up…) are unreachable on
  // phones. Runs on every page carrying the shared header.
  (function initMobileNav() {
    const header = document.querySelector(".site-header");
    const nav = header && header.querySelector(".global-nav");
    if (!header || !nav) return;
    const host = header.querySelector(".header-actions") ||
      header.querySelector(".header-inner");
    if (!host) return;
    const toggle = document.createElement("button");
    toggle.type = "button";
    toggle.className = "nav-toggle";
    toggle.setAttribute("aria-label", "Toggle navigation menu");
    toggle.setAttribute("aria-expanded", "false");
    toggle.innerHTML =
      '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h18M3 12h18M3 18h18"></path></svg>';
    host.appendChild(toggle);
    function setOpen(open) {
      header.classList.toggle("nav-open", open);
      toggle.setAttribute("aria-expanded", open ? "true" : "false");
    }
    toggle.addEventListener("click", (event) => {
      event.stopPropagation();
      setOpen(!header.classList.contains("nav-open"));
    });
    // Collapse after following a link, or when tapping outside the menu.
    nav.addEventListener("click", (event) => {
      if (event.target.closest("a")) setOpen(false);
    });
    document.addEventListener("click", (event) => {
      if (header.classList.contains("nav-open") && !header.contains(event.target)) {
        setOpen(false);
      }
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") setOpen(false);
    });
  })();

  // Reveal the header "Admin" link for a signed-in admin. The adminUrl is
  // derived from the Worker's ADMIN_PATH env and carried in the session that
  // login/dashboard persist to localStorage, so static pages can surface the
  // same shortcut the dashboard header shows without a fresh round-trip.
  function readStoredSession() {
    try {
      return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (error) {
      return null;
    }
  }

  function applyAdminLink(session) {
    const link = document.querySelector("[data-admin-link]");
    if (!link) return;
    let adminUrl = session && session.isAdmin ? (session.adminUrl || "") : "";
    if (adminUrl && session.nodeName && !/[?&]admin=/.test(adminUrl)) {
      adminUrl += (adminUrl.includes("?") ? "&" : "?") +
        "admin=" + encodeURIComponent(session.nodeName);
    }
    if (adminUrl) {
      link.href = adminUrl;
      link.removeAttribute("hidden");
    } else {
      link.href = "#";
      link.setAttribute("hidden", "");
    }
    return adminUrl;
  }

  async function hydrateAdminLink() {
    const session = readStoredSession();
    const adminUrl = applyAdminLink(session);
    // An older session may be flagged admin but predate adminUrl being carried
    // through — backfill it from the account endpoint so the shortcut appears.
    if (session && session.isAdmin && !adminUrl && session.nodeName &&
        location.protocol !== "file:") {
      try {
        const response = await fetch(
          `/api/accounts/${encodeURIComponent(session.nodeName)}`,
          { headers: { accept: "application/json" } },
        );
        if (!response.ok) return;
        const body = await response.json();
        if (body && body.adminUrl) {
          const next = { ...session, isAdmin: Boolean(body.isAdmin), adminUrl: body.adminUrl };
          try { localStorage.setItem("forkmesh.session", JSON.stringify(next)); } catch (error) {}
          applyAdminLink(next);
        }
      } catch (error) {
        /* offline or logged out — leave the link hidden */
      }
    }
  }

  hydrateAdminLink();

  const themeToggle = document.querySelector("#theme-toggle");
  if (themeToggle) {
    themeToggle.addEventListener("click", () => {
      applyTheme(document.documentElement.classList.contains("dark") ? "light" : "dark");
    });
  }

  const installCopy = document.querySelector("#install-copy");
  if (installCopy) {
    installCopy.addEventListener("click", () => {
      const cmd = document.querySelector("#install-cmd");
      copyText(cmd ? cmd.textContent : "", installCopy);
    });
  }

  const clientsCount = document.querySelector("#clients-count");
  const clientsDot = document.querySelector("#clients-dot");
  // Live counts come from a single cached aggregate endpoint. Fetch it once on
  // load after rendering cached values; do not keep a periodic client timer open.
  const STATS_PATH = "/api/network/stats";
  const payoutWalletsEl = document.querySelector("#network-wallets");

  // The header pill reflects everything that is live on the network — open host
  // tunnels plus chat clients — so it doesn't read "0 online" while a host is
  // clearly up. The per-metric breakdown stays in the network stat cards.
  function renderOnline(online) {
    const label = online === 1 ? "1 node online" : `${online} nodes online`;
    if (clientsCount) clientsCount.textContent = label;
    if (clientsDot) clientsDot.classList.toggle("online", online > 0);
  }

  function renderNetworkMetricDescriptions(hosts, repos) {
    setText(
      "[data-network-hosts-description]",
      hosts === 0
        ? "No managed mirror hosts are reachable right now."
        : "Managed mirrors reachable by this relay right now.",
    );
    setText(
      "[data-network-repos-description]",
      repos === 0
        ? "No public repositories have been cataloged yet."
        : "Public repositories indexed for discovery.",
    );
  }

  async function pollStats(options) {
    // refresh=1 asks the relay to re-read these balances from Solana instead of
    // answering from its hourly cache. Only the button sets it: a page load
    // must never spend an on-chain read.
    const refresh = Boolean(options && options.refresh);
    const reposEl = document.querySelector("#network-repos");
    const hostsEl = document.querySelector("#network-hosts");
    if (location.protocol === "file:") {
      if (clientsCount) clientsCount.textContent = "Preview only";
      setText("#network-clients, [data-network-clients]", "—");
      if (reposEl) reposEl.textContent = "—";
      setText("[data-network-repos]", "—");
      if (hostsEl) hostsEl.textContent = "—";
      setText("[data-network-hosts]", "—");
      renderPayoutNodes([]);
      return;
    }
    try {
      let statsPath = payoutWalletsEl ? `${STATS_PATH}?payouts=1` : STATS_PATH;
      if (refresh) statsPath += `${payoutWalletsEl ? "&" : "?"}refresh=1`;
      const response = await fetch(statsPath, {
        headers: { accept: "application/json" },
        cache: refresh ? "no-store" : "default",
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      const clients = Number(data.clients) || 0;
      const hosts = Number(data.hosts) || 0;
      const repos = Number(data.repos) || 0;
      renderOnline(clients + hosts);
      setText("#network-clients, [data-network-clients]", String(clients));
      if (reposEl) reposEl.textContent = String(repos);
      setText("[data-network-repos]", String(repos));
      if (hostsEl) hostsEl.textContent = String(hosts);
      setText("[data-network-hosts]", String(hosts));
      renderNetworkMetricDescriptions(hosts, repos);
      writeCachedStat("clients", clients);
      writeCachedStat("hosts", hosts);
      writeCachedStat("repos", repos);
      renderPayoutNodes(data.payoutNodes);
    } catch (error) {
      if (clientsCount) clientsCount.textContent = "Reconnecting…";
      if (clientsDot) clientsDot.classList.remove("online");
      renderPayoutNodes([]);
    }
  }

  // Render the last-known counts immediately (stale-while-revalidate) so the
  // header pill and stat cards aren't blank "Checking…"/"—" placeholders on a
  // cold load; the one-shot fetch below refreshes them as soon as live data
  // arrives.
  function primeCachedStats() {
    if (location.protocol === "file:") return;
    const clients = readCachedStat("clients");
    const hosts = readCachedStat("hosts");
    const repos = readCachedStat("repos");
    if (clients !== null && hosts !== null) renderOnline(clients + hosts);
    if (clients !== null) setText("#network-clients, [data-network-clients]", String(clients));
    if (hosts !== null) setText("#network-hosts, [data-network-hosts]", String(hosts));
    if (repos !== null) setText("#network-repos, [data-network-repos]", String(repos));
    if (hosts !== null && repos !== null) renderNetworkMetricDescriptions(hosts, repos);
  }

  // Manual refresh: the only path on this page that costs a public-RPC read.
  const walletsRefreshEl = document.querySelector("#wallets-refresh");
  if (walletsRefreshEl) {
    walletsRefreshEl.addEventListener("click", async () => {
      if (walletsRefreshEl.disabled) return;
      const label = walletsRefreshEl.textContent;
      walletsRefreshEl.disabled = true;
      walletsRefreshEl.textContent = "Refreshing…";
      try {
        await pollStats({ refresh: true });
      } finally {
        walletsRefreshEl.disabled = false;
        walletsRefreshEl.textContent = label;
      }
    });
  }

  primeCachedStats();
  pollStats();
})();

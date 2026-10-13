(() => {
  const $ = (selector) => document.querySelector(selector);
  const isLive = location.protocol !== "file:";

  function setText(selector, value) {
    const element = $(selector);
    if (element) element.textContent = value;
  }

  function readSession() {
    try {
      return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (_) {
      return null;
    }
  }

  async function api(path) {
    const response = await fetch(path, {
      headers: { accept: "application/json" },
      cache: "no-store",
    });
    let body = {};
    try {
      body = await response.json();
    } catch (_) {}
    return { ok: response.ok, body };
  }

  function trimSol(lamports) {
    const value = (Number(lamports || 0) / 1_000_000_000).toFixed(9);
    return value.replace(/0+$/, "").replace(/\.$/, "") || "0";
  }

  // The pool balance comes from an hourly cache, so report the reading's age
  // instead of implying every page load is a live on-chain check.
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

  // refresh=1 makes the relay re-read the pool balance from Solana. Only the
  // Refresh button passes it; a plain page load must cost no on-chain read.
  async function loadStatus(options) {
    if (!isLive) return;
    const refresh = Boolean(options && options.refresh);
    const [network, pool] = await Promise.allSettled([
      api("/api/network/stats"),
      api(refresh ? "/api/rewards/pool?refresh=1" : "/api/rewards/pool"),
    ]);
    if (network.status === "fulfilled" && network.value.ok) {
      setText("#calc-nodes", String(Number(network.value.body.hosts) || 0));
    }
    if (pool.status === "fulfilled" && pool.value.ok) {
      const state = pool.value.body || {};
      setText(
        "#calc-reward",
        `${trimSol(state.rewardAmountLamports)} SOL`,
      );
      setText("#pool-network", String(state.network || "mainnet-beta"));
      setText(
        "#pool-balance",
        `${String(state.balanceSol || trimSol(state.balanceLamports))} SOL`,
      );
      const age = balanceAgeLabel(state.balanceCheckedAt);
      setText(
        "#pool-balance-checked",
        age ? `on-chain, checked ${age}` : "on-chain, not an estimate",
      );
      setText(
        "#pool-transfers",
        String(Array.isArray(state.transactions) ? state.transactions.length : 0),
      );
    }
  }

  function renderSession() {
    const session = readSession();
    const name = session && (session.nodeName || session.email);
    const element = $("#mp-node");
    if (!element) return;
    if (name) {
      element.textContent = session.nodeName
        ? `Signed in as node “${session.nodeName}”.`
        : `Signed in as ${session.email}.`;
    } else {
      element.innerHTML =
        'You are not signed in. <a href="/login?next='
        + encodeURIComponent(location.pathname + location.search)
        + '">Log in</a> to manage a public payout address.';
    }
  }

  const poolRefreshEl = $("#pool-balance-refresh");
  if (poolRefreshEl) {
    poolRefreshEl.addEventListener("click", async () => {
      if (poolRefreshEl.disabled) return;
      const label = poolRefreshEl.textContent;
      poolRefreshEl.disabled = true;
      poolRefreshEl.textContent = "Refreshing…";
      try {
        await loadStatus({ refresh: true });
      } finally {
        poolRefreshEl.disabled = false;
        poolRefreshEl.textContent = label;
      }
    });
  }

  renderSession();
  loadStatus();
})();

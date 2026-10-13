
  const DASHBOARD_UPDATE_CHECK_MIN_GAP_MS = 60 * 1000;
  const DASHBOARD_UPDATE_CHECK_INTERVAL_MS = 5 * 60 * 1000;
  const DASHBOARD_UPDATE_DISMISSED_KEY = "forkmesh.dashboard.updateDismissed";
  const dashboardUpdate = {
    running: "",
    available: "",
    checking: false,
    lastCheckAt: 0,
  };

  async function fetchDashboardBuildRev() {
    if (location.protocol === "file:") return "";
    try {
      const response = await fetch("/api/version", {
        cache: "no-store",
        credentials: "omit",
        headers: { accept: "application/json" },
      });
      if (!response.ok) return "";
      const data = await response.json();
      return String(data?.rev || "").trim();
    } catch (_) {
      return "";
    }
  }

  function dismissedDashboardUpdate() {
    try {
      return sessionStorage.getItem(DASHBOARD_UPDATE_DISMISSED_KEY) || "";
    } catch (_) {
      return "";
    }
  }

  function renderDashboardUpdateNotice() {
    const notice = $("[data-update-notice]");
    if (!notice) return;
    const rev = dashboardUpdate.available;
    notice.hidden = !rev || rev === dismissedDashboardUpdate();
  }

  async function checkForDashboardUpdate(force = false) {
    if (dashboardUpdate.checking || document.hidden) return;
    const now = Date.now();
    if (!force && now - dashboardUpdate.lastCheckAt < DASHBOARD_UPDATE_CHECK_MIN_GAP_MS) return;
    dashboardUpdate.checking = true;
    dashboardUpdate.lastCheckAt = now;
    try {
      const rev = await fetchDashboardBuildRev();
      if (!rev) return;
      if (!dashboardUpdate.running) {
        dashboardUpdate.running = rev;
        return;
      }
      if (rev === dashboardUpdate.running || rev === dashboardUpdate.available) return;
      dashboardUpdate.available = rev;
      renderDashboardUpdateNotice();
    } finally {
      dashboardUpdate.checking = false;
    }
  }

  function applyDashboardUpdate(button) {
    if (button) {
      button.disabled = true;
      button.innerHTML = loadingHtml("Updating...");
    }
    window.location.reload();
  }

  function initDashboardUpdateNotice() {
    const notice = $("[data-update-notice]");
    if (!notice) return;
    notice.querySelector("[data-update-notice-apply]")?.addEventListener("click", (event) => {
      applyDashboardUpdate(event.currentTarget);
    });
    notice.querySelector("[data-update-notice-dismiss]")?.addEventListener("click", () => {
      try {
        sessionStorage.setItem(DASHBOARD_UPDATE_DISMISSED_KEY, dashboardUpdate.available);
      } catch (_) {}
      notice.hidden = true;
    });
    window.addEventListener("focus", () => void checkForDashboardUpdate());
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) void checkForDashboardUpdate();
    });
    window.setInterval(() => void checkForDashboardUpdate(), DASHBOARD_UPDATE_CHECK_INTERVAL_MS);
    void checkForDashboardUpdate(true);
  }

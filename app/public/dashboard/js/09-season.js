  const seasonUi = { loaded: false, busy: false, data: null };

  async function seasonSettingsPost(body) {
    const response = await fetch("/api/season/settings", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ ...body, sessionToken: state.session?.sessionToken || "" }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) throw new Error(String(data.error || `HTTP ${response.status}`));
    return data;
  }

  function seasonDefaultUntil() {
    const now = new Date();
    const end = new Date(now.getFullYear(), 10, 1, 0, 0, 0);
    return end.getTime() > Date.now() + 60 * 60 * 1000 ? end.getTime() : Date.now() + 24 * 60 * 60 * 1000;
  }

  function seasonInputValue(ms) {
    const date = new Date(ms);
    const pad = (value) => String(value).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`;
  }

  function seasonStatusText(data) {
    if (data?.season !== "halloween") return "Off. Turn it on to dress up ForkMesh for everyone.";
    const until = new Date(Number(data.until)).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
    return `On for everyone until ${until}${data.by ? `, turned on by @${data.by}` : ""}.`;
  }

  function seasonErrorText(error) {
    return ({
      forbidden: "Only platform administrators and moderators can change this.",
      unauthorized: "Sign in again to change this.",
      invalid_until: "Pick a time between 5 minutes and 60 days from now.",
    })[String(error?.message || "")] || "That didn't save. Try again.";
  }

  function renderSeasonSetting(card, data) {
    const on = data?.season === "halloween";
    card.querySelector("[data-season-toggle]").setAttribute("aria-checked", on ? "true" : "false");
    card.querySelector("[data-season-until]").value = seasonInputValue(on ? Number(data.until) : seasonDefaultUntil());
    card.querySelector("[data-season-status]").textContent = seasonStatusText(data);
  }

  async function saveSeasonSetting(card, season) {
    if (seasonUi.busy) return;
    const status = card.querySelector("[data-season-status]");
    const until = new Date(card.querySelector("[data-season-until]").value).getTime();
    if (season && !Number.isFinite(until)) {
      status.textContent = seasonErrorText(new Error("invalid_until"));
      return;
    }
    seasonUi.busy = true;
    status.textContent = "Saving…";
    try {
      const data = await seasonSettingsPost({ season, until: season ? until : 0 });
      seasonUi.data = data;
      renderSeasonSetting(card, data);
      window.ForkMeshSeason?.set({ season: data.season || "", until: Number(data.until) || 0 });
    } catch (error) {
      renderSeasonSetting(card, seasonUi.data);
      status.textContent = seasonErrorText(error);
    } finally {
      seasonUi.busy = false;
    }
  }

  async function initSeasonSettings() {
    const card = $("[data-season-setting]");
    if (!card || seasonUi.loaded || !state.session?.sessionToken) return;
    seasonUi.loaded = true;
    let data = null;
    try {
      data = await fetchJson("/api/season/settings", { fresh: true });
    } catch (_) {
      return;
    }
    if (data?.ok !== true) return;
    seasonUi.data = data;
    card.hidden = false;
    renderSeasonSetting(card, data);
    window.lucide?.createIcons();
    card.querySelector("[data-season-toggle]").addEventListener("click", () => {
      void saveSeasonSetting(card, seasonUi.data?.season === "halloween" ? "" : "halloween");
    });
    card.querySelector("[data-season-until]").addEventListener("change", () => {
      if (seasonUi.data?.season === "halloween") void saveSeasonSetting(card, "halloween");
    });
  }

  function watchSeason() {
    window.addEventListener("forkmesh:season", () => {
      if ($("[data-home-feed]")) renderHomeFeed();
      if ($("[data-home-agents]")) renderHomeAgentSessions();
      $$("[data-user-activity-card]").forEach((card) => { card.outerHTML = renderUserActivityCard(); });
      window.lucide?.createIcons();
    });
  }

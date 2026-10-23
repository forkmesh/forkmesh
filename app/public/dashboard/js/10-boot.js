  // ---------------------------------------------------------------------------
  // Boot. Every page document ships this same bundle; <body data-page="..."> is
  // baked at build time (dashboard_shell.PAGES) and picks which init runs. The
  // page's own markup is the only view in the document and is active at parse
  // time, so the right page paints immediately — data fills in afterwards.
  const BUILTIN_PAGE_INITS = Object.freeze({
    "home": initHomePage,
    "repos": initReposPage,
    "new-repo": initNewRepoPage,
    "network": initNetworkPage,
    "ads": initAdsPage,
    "runners": initRunnersPage,
    "agents": initAgentsPage,
    "docs": initDocsPage,
    "apps": initAppsPage,
    "apps-build": initAppsBuildPage,
    "tasks": initTasksPage,
    "calendar": initCalendarPage,
    "notes": initNotesPage,
    "hackathon": initHackathonPage,
    "microjobs": initMicrojobsPage,
    "referrals": initReferralsPage,
    "challenges": initChallengesPage,
    "workflows": initWorkflowsPage,
    "bounties": initBountiesPage,
    "links": initLinksPage,
    "achievements": initAchievementsPage,
    "settings": initSettingsPage,
    "profile": initProfileOverviewPage,
    "profile-repositories": initProfileOverviewPage,
    "repo": initRepoPage,
  });
  const PAGE_INITS = { ...BUILTIN_PAGE_INITS };
  for (const packagedPage of ["tasks", "calendar", "notes", "workflows"]) {
    delete PAGE_INITS[packagedPage];
  }

  // Package scripts execute through this single public socket. They receive a
  // small host API instead of sharing this bundle's lexical scope, and cannot
  // replace a core page initializer. Package scripts themselves are copied
  // only into the deploy stage from Marketplace package entrypoints.
  const pluginPageInits = Object.create(null);

  async function pluginRequest(path, options = {}) {
    const url = new URL(path, location.origin);
    if (url.origin !== location.origin || !url.pathname.startsWith("/api/plugins/")) {
      throw new Error("plugin_request_outside_namespace");
    }
    const requestOptions = { ...options };
    const headers = new Headers(requestOptions.headers || {});
    headers.set("accept", "application/json");
    const token = state.session?.sessionToken || "";
    if (token) headers.set("authorization", "Bearer " + token);
    if (requestOptions.body && typeof requestOptions.body !== "string") {
      headers.set("content-type", "application/json");
      requestOptions.body = JSON.stringify(requestOptions.body);
    }
    requestOptions.headers = headers;
    const response = await fetch(url.pathname + url.search, requestOptions);
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(data.error || `HTTP ${response.status}`);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  async function pluginDesktopUsage(options = {}) {
    if (!state.session?.sessionToken) throw new Error("desktop_sign_in_required");
    const hub = await desktopControlHub();
    const desktops = await hub.desktops();
    const readings = await Promise.all((Array.isArray(desktops) ? desktops : []).map(async (desktop) => {
      const node = String(desktop?.node || "");
      if (!node) return null;
      try {
        const connection = await hub.connect(node);
        const usage = options?.refresh
          ? await connection.request("agentops.usage", { refresh: true }, 30000)
          : await connection.request("agentops.usage", {});
        return {
          node,
          name: String(connection.info?.name || desktop?.name || node),
          usage: usage && typeof usage === "object" ? usage : {},
        };
      } catch (error) {
        return { node, name: String(desktop?.name || node), error: desktopControlErrorText(error) };
      }
    }));
    return { desktops: readings.filter(Boolean), observedAt: Date.now() };
  }

  const pluginApi = Object.freeze({
    request: pluginRequest,
    desktopUsage: pluginDesktopUsage,
    select: (selector) => document.querySelector(selector),
    selectAll: (selector) => Array.from(document.querySelectorAll(selector)),
    session: () => state.session ? { ...state.session } : null,
    escapeHtml,
    relativeTimeLabel,
    publicSiteOrigin,
    // Bundled first-party packages can keep a mature host implementation while
    // the manifest owns route, navigation, install identity, and presentation.
    // The allowlist prevents a package from invoking arbitrary dashboard code.
    builtinPage: (pageId) => {
      if (!new Set(["tasks", "calendar", "notes", "workflows"]).has(pageId)) {
        throw new Error("plugin_builtin_page_not_available");
      }
      return BUILTIN_PAGE_INITS[pageId]?.();
    },
  });
  const pendingPluginRegistrations = Array.isArray(
    window.ForkMeshDashboard?._registrations
  ) ? window.ForkMeshDashboard._registrations.slice() : [];

  function registerPluginPage(pageId, initializer) {
    if (
      typeof pageId !== "string" || typeof initializer !== "function"
      || PAGE_INITS[pageId] || pluginPageInits[pageId]
    ) {
      throw new Error("invalid_plugin_page_registration");
    }
    pluginPageInits[pageId] = initializer;
    PAGE_INITS[pageId] = () => initializer(pluginApi);
  }
  window.ForkMeshDashboard = Object.freeze({
    registerPage: registerPluginPage,
  });
  pendingPluginRegistrations.forEach(
    ([pageId, initializer]) => registerPluginPage(pageId, initializer)
  );

  function composerRepoView(repo) {
    if (!repo?.owner || !repo?.name) return null;
    return {
      owner: String(repo.owner),
      name: String(repo.name),
      key: repoKey(repo),
      display: agentModalRepoLabel(repo),
      owns: sessionOwnsRepo(repo),
      endpoint: orgAgentEndpoint(repo),
    };
  }

  window.ForkMeshComposerHost = Object.freeze({
    session: () => state.session ? { ...state.session } : null,
    page: () => currentPage(),
    repo: () => composerRepoView(state.selectedRepo),
    repoChoices: () => agentModalRepoChoices().map(composerRepoView).filter(Boolean),
    loadRepoChoices: async () => {
      try {
        await ensureHomeOrganizationRepositories();
      } catch (_) {}
      return agentModalRepoChoices().map(composerRepoView).filter(Boolean);
    },
    selectedAgentId: () => String(state.agentsView?.selectedAgentId || ""),
    rememberRepo: (display) => {
      const [owner, name] = String(display || "").split("/");
      if (owner && name) rememberAgentVisitedRepository({ owner, name });
    },
    agentsChanged: (display, notice) => {
      if (currentPage() === "home") void loadHomeAgentSessions();
      const repo = state.selectedRepo;
      if (repo && repoDisplayKey(repo).toLowerCase() === String(display || "").toLowerCase()) {
        state.agentsView.selectedAgentId = null;
        void loadRepoAgents(repo, { notice: String(notice || "") });
      }
    },
  });

  function refreshRepoAddressViews() {
    const steps = [
      () => (Array.isArray(state.repositories)
        ? applyRepositoryFilter()
        : renderSidebarRepositories(state.session)),
      () => renderHeaderContext(),
      () => refreshRepoLinks(document),
      () => replaceLongRepoLocation(),
    ];
    for (const step of steps) {
      try {
        step();
      } catch (_) {}
    }
  }

  async function bootDashboard() {
    const startupStartedAt = 0;
    window.addEventListener("forkmesh:repo-addresses", refreshRepoAddressViews);
    const repoAddressesLoaded = loadRepoAddresses();
    applyDashboardTheme(readDashboardTheme());
    applyPlatformName();
    void instanceInfo();
    watchGenieSetting();
    watchHomeRedirectSetting();
    installErrorActions();
    preconnectRememberedDirectMirrors();
    void ensurePortableControllerSession();
    await hydratePortableMirrorSession();
    await restoreControllerSession();
    if (!repoAddressesKnown()) {
      await Promise.race([repoAddressesLoaded, new Promise((resolve) => setTimeout(resolve, 1500))]);
    }
    initDailyInterface();
    if (initSharedChrome()) {
      startAccountSessionWatch();
      watchDashboardUserStatus();
      startUserActivity();
      initDashboardFeedback();
      initPluginMenu();
      initDashboardUpdateNotice();
      initRecentPushes();
      initCommentReactions();
      initCommentAnchor();
      initCommentGifs();
      initCommentTips();
      void initReactionSettings();
      void initAiReviewSettings();
      (PAGE_INITS[currentPage()] || initHomePage)();
      initPageHistory();
      if (state.repoAddressesChanged) refreshRepoAddressViews();
      watchUserNames();
      window.dispatchEvent(new CustomEvent("forkmesh:composer-host"));
      // The ForkMesh Ads `web` slot: every page but the ads page and chat,
      // fetched anonymously once the page is idle (09-ads.js).
      initAdSlot();
      recordInterfaceMetric("startup", startupStartedAt,
        readStoredRepositoryCatalog() ? "local" : "remote");
    }
  }

  void bootDashboard();
})();

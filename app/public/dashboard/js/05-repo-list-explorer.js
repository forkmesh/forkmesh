  function repositoryMatchesQuery(repo, query) {
    if (!query) return true;
    const canonical = repoCanonicalIdentity(repo);
    const haystack = [
      repo.owner,
      repo.name,
      canonical.owner,
      canonical.name,
      // Cards are labelled with the logical owner, so filtering by the
      // organization (or account) name has to match too.
      ...repoLogicalOwners(repo).map((value) => value.owner),
      repo.description,
      repo.channel,
      repo.source,
      // Catalog records advertise the current head and stable root commits.
      // Include both so a pasted full SHA or a short prefix opens the matching
      // repository without fetching every repository's commit history.
      repo.commit,
      repo.rootCommit,
    ].join(" ").toLowerCase();
    return haystack.includes(query);
  }

  // --- Header search as a live page filter (adhoc #37) -------------------
  //
  // Typing in the header search box opens its repository dropdown *and*
  // narrows the list you are already looking at, with no Enter and no round
  // trip. The two are independent: the dropdown always searches the whole
  // catalog, while the page filter only ever hides rows the page had already
  // rendered. Clearing the box (including the native type="search" ✕) restores
  // everything.
  //
  // A page keeps its own filter box where it has one (#repoSearch, ...); the
  // global query is ANDed on top of it rather than replacing it, so neither
  // control silently overrides the other. The repository Issues and Pull
  // requests tabs are the exception: they answer to their own search box alone
  // (localFilterQueries), see issue #673.
  function globalSearchPageQuery() {
    return state.globalSearch.pageQuery || "";
  }

  // The lowercase terms a page list must match: its own filter box, then the
  // header search box. Empty terms drop out, so an untouched box filters
  // nothing.
  function pageFilterQueries(ownQuery) {
    return [String(ownQuery || "").trim().toLowerCase(), globalSearchPageQuery()]
      .filter(Boolean);
  }

  // The terms for a list that answers only to its own search box. The repo
  // Issues/Pull requests tabs render a dedicated "Search issues..." box, and
  // leaving the header box in charge too meant text in it (typed for the repo
  // dropdown, or put there by browser autofill) emptied the list with
  // "No results for ..." while the tab's own box sat blank (issue #673).
  function localFilterQueries(ownQuery) {
    return [String(ownQuery || "").trim().toLowerCase()].filter(Boolean);
  }

  function repositoryMatchesPageFilter(repo, ownQuery) {
    return pageFilterQueries(ownQuery)
      .every((query) => repositoryMatchesQuery(repo, query));
  }

  // The empty state a filtered list shows when nothing survived the search
  // (adhoc #62). Repeating the terms back is what turns "this page is broken"
  // into "this page is filtered", and the link is the one click out of the dead
  // end — an empty list otherwise gives you nothing to aim at.
  function noSearchResultsHtml(ownQuery = "", queries = pageFilterQueries(ownQuery)) {
    const terms = queries.join(" ");
    return `No results for "${escapeHtml(terms)}" <button type="button" data-clear-search class="text-accent underline underline-offset-2 hover:text-foreground">(clear search)</button>`;
  }

  // What an empty list should say: "no results for <terms>" when a search is
  // what emptied it, and the page's own wording when it is genuinely empty.
  // `queries` overrides the terms for a list that ignores the header box.
  function emptyListHtml(fallback, ownQuery = "", queries = pageFilterQueries(ownQuery)) {
    return queries.length
      ? noSearchResultsHtml(ownQuery, queries)
      : escapeHtml(fallback);
  }

  // Every box that can be narrowing the list in front of you: the header search
  // plus whichever local filter the page renders. noSearchResultsHtml names the
  // terms from all of them, so "(clear search)" has to empty all of them —
  // clearing one and leaving another would leave the list just as empty.
  const PAGE_FILTER_INPUTS = [
    "[data-global-search]",
    "#repoSearch",
    "[data-home-repo-search]",
    "[data-profile-repo-search]",
    '[data-repo-filter-query="issues"]',
    '[data-repo-filter-query="pulls"]',
  ].join(", ");

  function clearPageSearchFilters() {
    $$(PAGE_FILTER_INPUTS).forEach((input) => { input.value = ""; });
    // Repository record searches keep their terms in state rather than reading
    // their boxes back.
    state.issuesView.query = "";
    state.pullsView.query = "";
    state.globalSearch.open = false;
    state.globalSearch.selectedIndex = 0;
    state.globalSearch.pageQuery = "";
    state.page = 1;
    renderGlobalSearchResults();
    // The repo Issues/Pull requests lists are outside the header page filter
    // (localFilterQueries), so its re-render below would leave their
    // "(clear search)" dead end on screen.
    if (currentPage() === "repo") {
      if (state.issuesView.items.length) renderRepoIssues();
      renderRepoCollection("pulls");
    }
    // Straight to the re-render rather than through applyGlobalSearchPageFilter,
    // which short-circuits on an unchanged header query — and the header box is
    // exactly what is unchanged when only a local box held the term.
    renderGlobalSearchPageFilter();
  }

  // The header ✕ only earns its slot once there is something to clear; the "/"
  // shortcut hint is what it replaces.
  function renderGlobalSearchClear() {
    const input = $("[data-global-search]");
    const clear = $("[data-global-search-clear]");
    if (!input || !clear) return;
    const filled = Boolean(input.value);
    clear.classList.toggle("hidden", !filled);
    $("[data-global-search-shortcut]")?.classList.toggle("hidden", filled);
  }

  function repositoryTermsBadge(repo, compact = false) {
    if (repo?.termsFlagged !== true) return "";
    const category = String(repo.termsCategory || "policy").replace(
      /[^a-z-]/gi,
      "",
    ).slice(0, 20);
    return `<span title="This repository has an active ${escapeHtml(platformName())} Terms of Service moderation flag${category ? `: ${escapeHtml(category)}` : ""}." class="inline-flex shrink-0 items-center gap-1 rounded-full border border-destructive/60 bg-destructive/10 ${compact ? "px-1 py-0.5 text-[8px]" : "px-2 py-0.5 text-[10px]"} font-semibold text-destructive"><i data-lucide="flag" class="${compact ? "h-2.5 w-2.5" : "h-3 w-3"}"></i>${compact ? "ToS" : "Terms flag"}</span>`;
  }

  function groupMatchesGlobalSearch(group, query) {
    if (!query) return true;
    if (repositoryMatchesQuery(sourceOfTruth(group), query)) return true;
    return (group.members || []).some((member) => repositoryMatchesQuery(member, query));
  }

  function globalSearchRepoSummary(group, repo) {
    const members = group.members || [];
    const liveCount = members.reduce((count, member) => count + (repoIsLive(member) ? 1 : 0), 0);
    const nodeText = members.length > 1 ? `${liveCount} of ${members.length} nodes online` : (repoIsLive(repo) ? "online" : "offline");
    const visibility = repo.isPrivate ? "private" : "public";
    const description = String(repo.description || "").trim();
    return [visibility, nodeText, description].filter(Boolean).join(" - ");
  }

  function setGlobalSearchOpen(open) {
    state.globalSearch.open = Boolean(open);
    renderGlobalSearchResults();
  }

  function closeGlobalSearch(options = {}) {
    const input = $("[data-global-search]");
    if (options.clear && input) input.value = "";
    state.globalSearch.open = false;
    state.globalSearch.selectedIndex = 0;
    renderGlobalSearchResults();
    // Clearing the box must un-filter the page too, not just close the panel.
    if (options.clear) applyGlobalSearchPageFilter();
  }

  // Pull the header box's current text into state.globalSearch.pageQuery and
  // re-render this page's lists through it. Cheap enough to run per keystroke:
  // every list it drives filters an already-loaded array in memory, so nothing
  // here refetches.
  function applyGlobalSearchPageFilter() {
    const shell = $("[data-global-search-shell]");
    const input = $("[data-global-search]");
    // The box is lg-only chrome; when it is not on screen it must not be
    // holding a stale filter over the page.
    const visible = Boolean(input) && (!shell || shell.getClientRects().length > 0);
    const next = visible ? (input.value || "").trim().toLowerCase() : "";
    if (next === state.globalSearch.pageQuery) return;
    state.globalSearch.pageQuery = next;
    renderGlobalSearchPageFilter();
  }

  // Re-render whichever lists this page shows. Each branch reuses the page's
  // normal render path, so the filtered view is the same markup (empty states,
  // pagination, counts) the page would draw for a hand-typed filter.
  function renderGlobalSearchPageFilter() {
    const page = currentPage();
    if (["home", "repos", "profile-overview", "profile-repositories"].includes(page)) {
      // Filtering to a shorter list can strand you past the last page.
      state.page = 1;
      applyRepositoryFilter();
    } else if (page === "network") {
      renderNetworkRows(Array.isArray(state.networkNodeRows) ? state.networkNodeRows : []);
      window.lucide?.createIcons();
    } else if (page === "repo") {
      renderRepoPageFilter();
    }
    renderGlobalSearchPageFilterNotice();
  }

  // How many rows the page filter is currently showing, or null when this page
  // has nothing the header box filters (settings, chat).
  function globalSearchPageFilterCount() {
    const page = currentPage();
    if (["home", "repos", "profile-overview", "profile-repositories"].includes(page)) {
      return (state.filteredGroups || []).length;
    }
    if (page === "network") {
      return networkRowsMatchingPageFilter(
        Array.isArray(state.networkNodeRows) ? state.networkNodeRows : []).length;
    }
    if (page === "repo") return repoPageFilterCount();
    return null;
  }

  // A one-line footer under the dropdown, so it is obvious the page behind the
  // panel narrowed on purpose rather than losing its rows.
  function renderGlobalSearchPageFilterNotice() {
    const notice = $("[data-global-search-page-filter]");
    if (!notice) return;
    const query = globalSearchPageQuery();
    const count = query ? globalSearchPageFilterCount() : null;
    const show = Boolean(query) && count !== null;
    notice.classList.toggle("hidden", !show);
    notice.classList.toggle("flex", show);
    if (!show) {
      notice.innerHTML = "";
      return;
    }
    notice.innerHTML = `<i data-lucide="list-filter" class="h-3 w-3 shrink-0"></i><span class="min-w-0 truncate">Filtering this page: ${formatCount(count)} match${count === 1 ? "" : "es"}</span>`;
    window.lucide?.createIcons();
  }

  function renderGlobalSearchResults() {
    const input = $("[data-global-search]");
    const panel = $("[data-global-search-panel]");
    const container = $("[data-global-search-results]");
    if (!input || !panel || !container) return;
    renderGlobalSearchClear();

    const query = input.value.trim().toLowerCase();
    const groups = mergeGroupsByPublicAddress(groupRepositories(state.repositories || []))
      .filter((group) => groupMatchesGlobalSearch(group, query))
      .slice(0, 8);
    state.globalSearch.results = groups.map((group) => sourceOfTruth(group));
    state.globalSearch.selectedIndex = Math.min(
      state.globalSearch.selectedIndex,
      Math.max(state.globalSearch.results.length - 1, 0),
    );

    input.setAttribute("aria-expanded", state.globalSearch.open ? "true" : "false");
    panel.classList.toggle("hidden", !state.globalSearch.open);
    // Keep the "filtering this page" footer honest when the list behind it is
    // re-rendered (catalog arriving, page change) rather than only on keystroke.
    renderGlobalSearchPageFilterNotice();
    if (!state.globalSearch.open) return;

    if (state.repositoriesLoading) {
      container.innerHTML = `<div class="px-3 py-5 text-sm text-muted-foreground">${loadingHtml("Loading repositories...")}</div>`;
      return;
    }

    if (!groups.length) {
      container.innerHTML = '<div class="px-3 py-5 text-sm text-muted-foreground">No repositories match this search.</div>';
      return;
    }

    container.innerHTML = groups.map((group, index) => {
      const repo = sourceOfTruth(group);
      const key = repoKey(repo);
      const selected = index === state.globalSearch.selectedIndex;
      return `
        <button
          type="button"
          data-global-search-result
          data-dashboard-open-repo="${escapeHtml(key)}"
          role="option"
          aria-selected="${selected ? "true" : "false"}"
          class="grid w-full grid-cols-[1.25rem_minmax(0,1fr)] items-center gap-2 px-3 py-2 text-left text-sm transition-colors ${selected ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-secondary/70 hover:text-foreground"}"
        >
          <i data-lucide="book-marked" class="h-3.5 w-3.5 text-muted-foreground"></i>
          <span class="min-w-0">
            <span class="block truncate font-medium text-foreground">${escapeHtml(groupDisplayName(group))}</span>
            <span class="block truncate text-xs text-muted-foreground">${escapeHtml(globalSearchRepoSummary(group, repo))}</span>
          </span>
        </button>`;
    }).join("");
    window.lucide?.createIcons();
  }

  function moveGlobalSearchSelection(delta) {
    const results = $$("[data-global-search-result]");
    if (!results.length) return;
    state.globalSearch.selectedIndex = (state.globalSearch.selectedIndex + delta + results.length) % results.length;
    results.forEach((button, index) => {
      const selected = index === state.globalSearch.selectedIndex;
      button.setAttribute("aria-selected", selected ? "true" : "false");
      button.classList.toggle("bg-secondary", selected);
      button.classList.toggle("text-foreground", selected);
      button.classList.toggle("text-muted-foreground", !selected);
      if (selected) button.scrollIntoView({ block: "nearest" });
    });
  }

  function selectGlobalSearchResult(key = "") {
    const selected = $('[data-global-search-result][aria-selected="true"]') || $("[data-global-search-result]");
    const wanted = key || selected?.dataset?.dashboardOpenRepo || repoKey(state.globalSearch.results[state.globalSearch.selectedIndex] || {});
    // Enter with nothing to open — a term that matched no repository, or the
    // catalog still loading — leaves the dropdown and the typed term alone so
    // the search can be corrected instead of restarted.
    if (!isRepoKey(wanted)) return;
    closeGlobalSearch({ clear: true });
    openRepoPage(wanted);
  }

  function focusGlobalSearch() {
    const shell = $("[data-global-search-shell]");
    const input = $("[data-global-search]");
    if (!input || (shell && !shell.getClientRects().length)) return false;
    state.globalSearch.open = true;
    state.globalSearch.selectedIndex = 0;
    renderGlobalSearchResults();
    input.focus();
    input.select();
    return true;
  }

  // A repo is reachable when its own host is live OR - for a public repo - a peer
  // mirroring the same logical repo is online and the relay serves it in place
  // through the repo's own URL (adhoc #61). cloneOnline is the worker's group
  // verdict; fall back to liveHost for older payloads that predate it.
  function repoIsLive(repo) {
    return Boolean(repo?.cloneOnline ?? repo?.liveHost);
  }
  // Live, but the named source of truth is down - a mirror node is serving it.
  function repoServedByMirror(repo) {
    return repoIsLive(repo) && !repo?.liveHost;
  }
  // The signed-in account owns this repo when their node name matches the repo
  // owner slug (case-insensitive). Owners get to keep their own offline issue
  // submissions visible until their source-of-truth node drains them (#379).
  function isRepoOwner(repo) {
    const owner = String(repo?.owner || "").trim().toLowerCase();
    const me = String(state.session?.nodeName || "").trim().toLowerCase();
    return Boolean(owner && me && owner === me);
  }

  function groupRepoMetric(group, keys) {
    let best = null;
    for (const member of group.members || []) {
      const value = repoCount(member, keys);
      if (value === null) continue;
      best = best === null ? value : Math.max(best, value);
    }
    return best === null ? 0 : best;
  }

  function normalizeActivityWeeks(value) {
    const raw = Array.isArray(value) ? value.slice(-52) : [];
    const series = raw.map((item) => {
      const number = Number(item);
      return Number.isFinite(number) && number > 0 ? number : 0;
    });
    while (series.length < 52) series.unshift(0);
    return series;
  }

  const ACTIVITY_WEEK_MS = 7 * 24 * 60 * 60 * 1000;

  // A publisher buckets its commits into 52 windows that end at *its* clock when
  // the advert was built (mirrorGitActivityWeeks), so the series is anchored to
  // the publish moment, not to a calendar week. Label the windows from the
  // record's own timestamp when we have one - anchoring a month-old catalog
  // entry on "now" would claim last month's commits landed this week.
  function activityWeekWindows(anchor) {
    const anchorDate = parseFlexibleDate(anchor);
    const end = anchorDate ? anchorDate.getTime() : Date.now();
    return Array.from({ length: 52 }, (_, index) => ({
      start: end - ((52 - index) * ACTIVITY_WEEK_MS),
      // Exclusive bound; the label reads the last millisecond inside the window
      // so a Monday-to-Sunday week never spills into the next Monday.
      end: end - ((51 - index) * ACTIVITY_WEEK_MS),
    }));
  }

  function activityWeekLabel(week, value) {
    const commits = Number(value) || 0;
    const range = week
      ? `${formatDate(week.start)} - ${formatDate(week.end - 1)}`
      : "Unknown week";
    return `${range}: ${formatCount(commits)} commit${commits === 1 ? "" : "s"}`;
  }

  function groupActivityWeeks(group) {
    const buckets = Array.from({ length: 52 }, () => 0);
    for (const member of group.members || []) {
      const series = normalizeActivityWeeks(member.activityWeeks);
      for (let i = 0; i < buckets.length; i += 1) {
        buckets[i] = Math.max(buckets[i], series[i] || 0);
      }
    }
    return buckets;
  }

  function repoLanguage(repo) {
    return String(repo.language || repo.primaryLanguage || repo.stack || "TypeScript");
  }

  function repoLicense(repo) {
    return String(repo.license || repo.licenseName || "MIT License");
  }

  function repoMetricStat(label, icon, value) {
    // An inline stat, not a boxed chip: issues/commits/pulls/discussions ride
    // the same meta line as language/visibility/mirrors/updated, so a card
    // costs one row instead of a separate chip grid underneath.
    return `
      <span class="inline-flex shrink-0 items-center gap-1 text-xs text-muted-foreground">
        <i data-lucide="${icon}" class="h-3 w-3 shrink-0"></i>${escapeHtml(label)}
        <span class="font-mono text-foreground">${formatCount(value)}</span>
      </span>
    `;
  }

  const nativeRepositoryLogoCache = new Map();
  let nativeRepositoryLogoSessionToken = "";
  let nativeRepositoryLogoSessionEpoch = 0;

  function nativeRepositoryLogoEndpoint(repo) {
    // Through repoApiBase so a list row is fetched from the repository's
    // public address, never from the mirror that published its catalog row.
    const owner = String(repo?.owner || "").trim();
    const name = String(repo?.name || "").trim();
    if (!owner || !name) return "";
    return `${repoApiBase(repo)}/logo`;
  }

  function nativeRepositoryLogoMarkup(repo, sizeClass = "h-10 w-10") {
    const endpoint = nativeRepositoryLogoEndpoint(repo);
    if (!endpoint) return "";
    return `
      <span data-native-repo-logo-frame class="relative flex ${sizeClass} shrink-0 items-center justify-center overflow-hidden rounded-xl border border-border bg-secondary text-muted-foreground">
        <i data-native-repo-logo-fallback data-lucide="book-marked" class="h-4 w-4"></i>
        <img data-native-repo-logo data-logo-endpoint="${escapeHtml(endpoint)}" alt="" loading="lazy" decoding="async" class="absolute inset-0 hidden h-full w-full object-cover" />
      </span>
    `;
  }

  function nativeRepositoryLogoDataUrl(value) {
    const url = String(value || "");
    return (
      /^data:image\/(?:svg\+xml|png|jpeg|webp)(?:;|,)/i.test(url)
      || /^\/api\/repo\/[^/?#]+\/[^/?#]+\/(?:raw\?[^#]+|dir-logo\?[^#]+|media\/logo\.png(?:\?[^#]*)?)$/i.test(url)
    )
      ? url
      : "";
  }

  function nativeRepositoryLogoCacheKey(endpoint) {
    const token = String(state.session?.sessionToken || "");
    if (token !== nativeRepositoryLogoSessionToken) {
      // A guest miss must not hide a private logo after login, and an
      // owner-authorized logo must never survive logout/account switching.
      nativeRepositoryLogoCache.clear();
      nativeRepositoryLogoSessionToken = token;
      nativeRepositoryLogoSessionEpoch += 1;
    }
    return `${nativeRepositoryLogoSessionEpoch}:${endpoint}`;
  }

  function loadNativeRepositoryLogo(endpoint) {
    const cacheKey = nativeRepositoryLogoCacheKey(endpoint);
    if (!nativeRepositoryLogoCache.has(cacheKey)) {
      const token = String(state.session?.sessionToken || "");
      const headers = { Accept: "application/json" };
      if (token) headers.Authorization = `Bearer ${token}`;
      const pending = fetch(endpoint, {
        headers,
        credentials: "same-origin",
      }).then(async (response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const body = await response.json();
        if (String(state.session?.sessionToken || "") !== token) return null;
        return {
          dataUrl: nativeRepositoryLogoDataUrl(body?.logo?.dataUrl),
          // The repository's own root logo streams through a mirror, so it can
          // fail long after the card rendered. The owner-approved artwork ships
          // with it so the card swaps instead of showing a broken image.
          fallbackDataUrl: nativeRepositoryLogoDataUrl(
            body?.logo?.fallbackDataUrl),
          // "owner" when the logo is the one uploaded from Settings.
          source: String(body?.logo?.source || ""),
        };
      }).catch(() => {
        // Do not negatively cache authorization failures or transient errors.
        // The same card can be retried after login or on a later render.
        if (nativeRepositoryLogoCache.get(cacheKey) === pending) {
          nativeRepositoryLogoCache.delete(cacheKey);
        }
        return null;
      });
      nativeRepositoryLogoCache.set(cacheKey, pending);
    }
    return nativeRepositoryLogoCache.get(cacheKey);
  }

  function showNativeRepositoryLogo(image) {
    image.classList.remove("hidden");
    image.parentElement?.querySelector("[data-native-repo-logo-fallback]")
      ?.classList.add("hidden");
  }

  function hideNativeRepositoryLogo(image) {
    image.classList.add("hidden");
    image.removeAttribute("src");
    image.parentElement?.querySelector("[data-native-repo-logo-fallback]")
      ?.classList.remove("hidden");
  }

  function hydrateNativeRepositoryLogos(root) {
    if (!root) return;
    root.querySelectorAll("[data-native-repo-logo]").forEach(async (image) => {
      const endpoint = image.getAttribute("data-logo-endpoint") || "";
      if (!endpoint || image.dataset.logoHydrated === "true") return;
      image.dataset.logoHydrated = "true";
      const logo = await loadNativeRepositoryLogo(endpoint);
      const dataUrl = String(logo?.dataUrl || "");
      const fallbackDataUrl = String(logo?.fallbackDataUrl || "");
      if (!dataUrl || !image.isConnected) return;
      // The committed root logo is served by a mirror, so it can fail after the
      // card rendered (offline or lagging host). Swap to the owner-approved
      // artwork, then to the repository icon — never leave a broken image
      // behind, and never stand in a machine-generated mark for a real logo.
      image.onerror = () => {
        if (fallbackDataUrl && image.dataset.logoFallbackUsed !== "true") {
          image.dataset.logoFallbackUsed = "true";
          image.src = fallbackDataUrl;
          return;
        }
        image.onerror = null;
        hideNativeRepositoryLogo(image);
      };
      image.src = dataUrl;
      showNativeRepositoryLogo(image);
    });
  }

  // Two shapes share one series: the compact card summary (bars ~2px wide, so
  // only the whole strip is labelled) and the Insights graph, where every week
  // is its own hover/focus target carrying its date range and commit count.
  function repoActivitySparkline(values, options = {}) {
    const series = normalizeActivityWeeks(values);
    const max = Math.max(1, ...series);
    const total = series.reduce((sum, n) => sum + n, 0);
    const totalHint = Number(options.totalHint);
    const displayTotal = total || (Number.isFinite(totalHint) && totalHint > 0 ? totalHint : 0);
    const loading = Boolean(options.loading);
    const detailed = Boolean(options.detailed);
    const windows = activityWeekWindows(options.anchor);
    const title = loading
      ? `Loading activity for ${formatCount(displayTotal)} commits`
      : total
        ? `${formatCount(total)} commits in the past 52 weeks`
        : `${formatCount(displayTotal)} commits`;
    const barHeight = (value) => {
      const scale = detailed ? 56 : 30;
      return value > 0 ? Math.max(detailed ? 4 : 3, Math.round((value / max) * scale)) : 2;
    };
    const bars = series.map((value, index) => {
      const tone = value > 0 ? "bg-primary" : loading ? "bg-muted-foreground/30" : "bg-muted-foreground/20";
      const bar = `<span class="repo-activity-bar ${tone}" style="height:${barHeight(value)}px"></span>`;
      if (!detailed) return bar;
      // The column, not the coloured bar, is the target: a quiet week is 2px
      // tall and would otherwise be impossible to hover or see focused. Only
      // one week holds the tab stop; hydrateRepoActivityGraphs moves it with
      // the arrow keys so the graph is not 52 stops deep.
      const label = activityWeekLabel(windows[index], value);
      return `<span class="repo-activity-week" data-repo-activity-week="${index}" data-repo-activity-label="${escapeHtml(label)}" role="img" aria-label="${escapeHtml(label)}" tabindex="${index === 0 ? "0" : "-1"}">${bar}</span>`;
    }).join("");
    const header = `
        <div class="mb-1 flex items-center justify-between gap-2 text-[10px] font-mono text-muted-foreground">
          <span>52 weeks</span>
          <span>${loading ? "loading" : `${formatCount(displayTotal)} commits`}</span>
        </div>`;
    if (!detailed) {
      return `
      <div data-repo-activity-sparkline class="repo-activity-sparkline w-full" title="${escapeHtml(title)}" aria-label="${escapeHtml(title)}">${header}
        <div class="repo-activity-bars h-8">${bars}</div>
      </div>
    `;
    }
    const first = windows[0];
    const last = windows[windows.length - 1];
    return `
      <div data-repo-activity-sparkline data-repo-activity-graph class="repo-activity-sparkline repo-activity-graph w-full" role="group" aria-label="${escapeHtml(`Weekly commit activity: ${title}`)}">${header}
        <span class="sr-only">Use the left and right arrow keys to read each week.</span>
        <div class="repo-activity-bars h-16">${bars}</div>
        <div class="mt-1 flex items-center justify-between gap-2 text-[10px] font-mono text-muted-foreground">
          <span>${escapeHtml(formatDate(first.start))}</span>
          <span>${escapeHtml(formatDate(last.end - 1))}</span>
        </div>
        <div data-repo-activity-tooltip hidden aria-hidden="true"></div>
      </div>
    `;
  }

  function showRepoActivityTooltip(graph, week, event) {
    const tooltip = graph?.querySelector("[data-repo-activity-tooltip]");
    if (!tooltip || !week) return;
    tooltip.textContent = week.getAttribute("data-repo-activity-label") || "";
    tooltip.hidden = false;
    const graphRect = graph.getBoundingClientRect();
    const weekRect = week.getBoundingClientRect();
    const pointerX = Number.isFinite(event?.clientX)
      ? event.clientX
      : weekRect.left + (weekRect.width / 2);
    const left = Math.max(0, Math.min(
      Math.max(0, graphRect.width - tooltip.offsetWidth),
      pointerX - graphRect.left - (tooltip.offsetWidth / 2),
    ));
    // The Insights card clips its overflow, so the tooltip stays inside the
    // graph box - .repo-activity-graph reserves the headroom it floats into.
    const top = Math.max(0, weekRect.top - graphRect.top - tooltip.offsetHeight - 6);
    tooltip.style.left = `${left}px`;
    tooltip.style.top = `${top}px`;
  }

  function hideRepoActivityTooltip(graph) {
    const tooltip = graph?.querySelector("[data-repo-activity-tooltip]");
    if (tooltip) tooltip.hidden = true;
  }

  // The graph is rendered as an HTML string, so its hover/focus behaviour is
  // wired once the markup is in the DOM (same shape as the profile skyline).
  function hydrateRepoActivityGraphs(root) {
    const scope = root || document;
    scope.querySelectorAll("[data-repo-activity-graph]").forEach((graph) => {
      if (graph.dataset.repoActivityHydrated === "true") return;
      graph.dataset.repoActivityHydrated = "true";
      const weeks = [...graph.querySelectorAll("[data-repo-activity-week]")];
      if (!weeks.length) return;
      const moveTo = (index) => {
        const next = weeks[Math.max(0, Math.min(weeks.length - 1, index))];
        if (!next) return;
        weeks.forEach((week) => week.setAttribute("tabindex", week === next ? "0" : "-1"));
        next.focus();
      };
      graph.addEventListener("mouseleave", () => hideRepoActivityTooltip(graph));
      weeks.forEach((week, index) => {
        week.addEventListener("mouseenter", (event) => showRepoActivityTooltip(graph, week, event));
        week.addEventListener("mousemove", (event) => showRepoActivityTooltip(graph, week, event));
        week.addEventListener("mouseleave", () => hideRepoActivityTooltip(graph));
        week.addEventListener("focus", () => showRepoActivityTooltip(graph, week));
        week.addEventListener("blur", () => hideRepoActivityTooltip(graph));
        // Touch never hovers: a tap takes the tab stop, which shows the label
        // through the same focus path.
        week.addEventListener("click", () => moveTo(index));
        week.addEventListener("keydown", (event) => {
          const targets = {
            ArrowLeft: index - 1,
            ArrowRight: index + 1,
            Home: 0,
            End: weeks.length - 1,
          };
          if (!(event.key in targets)) return;
          event.preventDefault();
          moveTo(targets[event.key]);
        });
      });
    });
  }

  function repositoryCard(group) {
    const origin = sourceOfTruth(group);
    const repo = group.primary;
    const key = repoKey(origin);
    const label = repoLabelParts(groupDisplayName(group));
    const displayOwner = label.owner;
    const servedNote = !dashboardOnInstance() && displayOwner && displayOwner.toLowerCase() !== String(origin.owner || "").toLowerCase()
      ? ` (published from ${origin.owner || "a node"})`
      : "";
    const fullLabel = displayOwner ? `${displayOwner}/${origin.name || "repository"}` : String(origin.name || "repository");
    const live = repoIsLive(origin);
    const viaMirror = repoServedByMirror(origin);
    const visibility = origin.isPrivate ? "private" : "public";
    const statusClass = live ? "text-success" : "text-muted-foreground";
    const nodeCount = group.members.length;
    const liveCount = group.members.reduce((n, m) => n + (repoIsLive(m) ? 1 : 0), 0);
    const onInstance = dashboardOnInstance();
    const statusText = nodeCount > 1
      ? `${liveCount} of ${nodeCount} nodes`
      : (viaMirror && !onInstance ? "via mirror" : live ? "online" : "offline");
    const metrics = [
      repoMetricStat("Issues", "circle-dot", groupRepoMetric(group, ["issueCount", "issues", "issuesCount", "openIssues"])),
      repoMetricStat("Commits", "git-commit-horizontal", groupRepoMetric(group, ["commitCount", "commits", "commitHistory"])),
      repoMetricStat("Pulls", "git-pull-request", groupRepoMetric(group, ["pullCount", "pulls", "pullsCount", "openPulls", "pullRequests"])),
      repoMetricStat("Discussions", "message-square", groupRepoMetric(group, ["discussionCount", "discussions"])),
    ].join("");
    const commitTotal = groupRepoMetric(group, ["commitCount", "commits", "commitHistory"]);
    const activityWeeks = groupActivityWeeks(group);
    const language = repoLanguage(origin);
    return `
      <div data-repo="${escapeHtml(key.toLowerCase())}" data-dashboard-open-repo="${escapeHtml(key)}" data-clone-url="${escapeHtml(cloneUrl(origin))}" role="link" tabindex="0" aria-label="Open ${escapeHtml(fullLabel)}" class="repo-card group cursor-pointer px-4 py-3 hover:bg-secondary/40 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring/60">
        <div class="repo-layout grid gap-3 md:grid-cols-[minmax(0,1fr)_minmax(10rem,12rem)] md:items-center">
          <div class="flex min-w-0 items-start gap-3">
            ${nativeRepositoryLogoMarkup(origin)}
            <div class="min-w-0 flex-1">
            <div class="flex min-w-0 items-center gap-2">
              <p class="min-w-0 truncate text-sm font-medium text-foreground" title="${escapeHtml(`${fullLabel}${servedNote}`)}">
                ${displayOwner ? `<span class="text-muted-foreground">${escapeHtml(displayOwner)}/</span>` : ""}${escapeHtml(origin.name || "repository")}
              </p>
              <span class="shrink-0 rounded-full border border-border px-2 py-0.5 text-[10px] font-mono ${statusClass}">
                ${statusText}
              </span>
              ${repositoryTermsBadge(origin)}
              <button data-repo-star-button data-repo-key="${escapeHtml(key)}" type="button" aria-pressed="false" aria-label="Star ${escapeHtml(`${displayOwner || ""}/${origin.name || ""}`)}" class="ml-auto hidden shrink-0 items-center gap-1 rounded-md border border-border bg-secondary px-2 py-1 text-xs text-foreground hover:bg-background sm:inline-flex">
                <i data-lucide="star" data-repo-star-icon class="h-3.5 w-3.5 text-muted-foreground"></i>
                <span data-repo-star-label>Star</span>
                <span data-repo-star-count class="font-mono text-muted-foreground">${formatCount(0)}</span>
              </button>
            </div>
            <p class="mt-1 truncate text-xs text-muted-foreground">${escapeHtml(repoDescriptionText(repo, origin) || "No description published.")}</p>
            <div class="mt-2 flex items-center gap-x-3 gap-y-1 flex-wrap">
              <span class="flex items-center gap-1.5 text-xs text-muted-foreground">
                <span data-repo-language-dot class="w-2 h-2 rounded-full bg-primary"></span>${escapeHtml(language)}
              </span>
              <span class="flex items-center gap-1.5 text-xs text-muted-foreground">
                <i data-lucide="${origin.isPrivate ? "lock" : "globe-2"}" class="h-3 w-3"></i>${escapeHtml(visibility)}
              </span>
              ${onInstance ? "" : `<span data-repo-card-served-by class="flex items-center gap-1 text-xs text-muted-foreground">
                <i data-lucide="radio" class="w-3 h-3"></i>${nodeCount > 1 ? `${nodeCount} mirrors` : (viaMirror ? "served by mirror" : live ? "mirror online" : "mirror offline")}
              </span>`}
              <span class="text-xs text-muted-foreground font-mono">updated ${escapeHtml(formatDate(repo.updatedAt || repo.lastSync))}</span>
              ${metrics}
            </div>
            </div>
          </div>
          <div class="min-w-0">
            ${repoActivitySparkline(activityWeeks, { totalHint: commitTotal })}
          </div>
        </div>
      </div>
    `;
  }

  const PROFILE_REPO_LANGUAGE_FALLBACK_COLOR = "#8b949e";

  function profileRepositoryLanguage(group) {
    const repo = sourceOfTruth(group);
    return organizationRepositoryLanguages(group)[0]?.[0] ||
      String(repo.language || repo.primaryLanguage || "").trim();
  }

  function profileRepositoryLanguageColor(language) {
    return REPO_LANGUAGE_COLORS[language] || PROFILE_REPO_LANGUAGE_FALLBACK_COLOR;
  }

  function profileRepositoryLanguageMarkup(language) {
    if (!language) return "";
    return `<span class="inline-flex items-center gap-1"><span data-repo-language-dot class="inline-block h-3 w-3 shrink-0 rounded-full border border-black/10" style="background-color:${escapeHtml(profileRepositoryLanguageColor(language))}"></span><span>${escapeHtml(language)}</span></span>`;
  }

  function profileRepositoryUpdatedMarkup(repo) {
    const date = parseFlexibleDate(repo.updatedAt || repo.lastSync);
    if (!date) return "";
    const recent = Date.now() - date.getTime() < 30 * 24 * 60 * 60 * 1000;
    const label = recent ? formatTimeAgo(date.getTime()) : `on ${formatDate(date.getTime())}`;
    return `<span>Updated <time datetime="${escapeHtml(date.toISOString())}" title="${escapeHtml(date.toLocaleString())}">${escapeHtml(label)}</time></span>`;
  }

  function profileRepositorySparkline(weeks) {
    const series = normalizeActivityWeeks(weeks);
    const total = series.reduce((sum, value) => sum + value, 0);
    if (!total) return "";
    const width = 155;
    const height = 30;
    const max = Math.max(1, ...series);
    const step = width / Math.max(1, series.length - 1);
    const points = series.map((value, index) =>
      `${(index * step).toFixed(1)},${(height - 1 - (value / max) * (height - 4)).toFixed(1)}`).join(" ");
    const label = `${formatCount(total)} commit${total === 1 ? "" : "s"} in the past 52 weeks`;
    return `<svg data-repo-activity-sparkline class="hidden text-primary lg:block" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" role="img" aria-label="${escapeHtml(label)}"><title>${escapeHtml(label)}</title><polyline points="${points}" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"></polyline></svg>`;
  }

  function profileRepositoryHasCount(text) {
    return /[1-9]/.test(String(text || ""));
  }

  function syncProfileRepositoryStarMeta(root) {
    root.querySelectorAll("[data-profile-repository-row]").forEach((row) => {
      const text = String(row.querySelector("[data-repo-star-button] [data-repo-star-count]")?.textContent || "").trim();
      const meta = row.querySelector("[data-profile-repo-star-meta]");
      const value = meta?.querySelector("[data-profile-repo-star-meta-count]");
      if (!meta || !value) return;
      if (value.textContent !== text) value.textContent = text;
      const show = profileRepositoryHasCount(text);
      if (meta.classList.contains("hidden") === show) meta.classList.toggle("hidden", !show);
    });
  }

  function observeProfileRepositoryStars(container) {
    if (!container || container.dataset.profileRepoStarObserved === "true") return;
    container.dataset.profileRepoStarObserved = "true";
    new MutationObserver(() => syncProfileRepositoryStarMeta(container)).observe(container, {
      subtree: true,
      childList: true,
      characterData: true,
      attributes: true,
      attributeFilter: ["aria-pressed"],
    });
  }

  function profileRepositoryRow(group) {
    const repo = sourceOfTruth(group);
    const key = repoKey(repo);
    const visibility = repo.isPrivate ? "Private" : "Public";
    const language = profileRepositoryLanguage(group);
    const license = String(repo.license || repo.licenseName || "").trim();
    const description = String(repo.description || "").trim();
    const displayOwner = repoLabelParts(groupDisplayName(group)).owner;
    const publishedFrom = !dashboardOnInstance() && displayOwner && repo.owner &&
      displayOwner.toLowerCase() !== String(repo.owner).toLowerCase()
      ? `<p class="mb-1 text-xs text-muted-foreground">Published from ${escapeHtml(repo.owner)}/${escapeHtml(repo.name || "repository")}</p>`
      : "";
    const meta = [
      profileRepositoryLanguageMarkup(language),
      `<span data-profile-repo-star-meta class="hidden inline-flex items-center gap-1"><i data-lucide="star" class="h-4 w-4"></i><span data-profile-repo-star-meta-count>0</span></span>`,
      license ? `<span class="inline-flex items-center gap-1"><i data-lucide="scale" class="h-4 w-4"></i>${escapeHtml(license)}</span>` : "",
      profileRepositoryUpdatedMarkup(repo),
    ].filter(Boolean).join("");
    return `<article data-profile-repository-row class="flex w-full justify-between gap-4 border-b border-border py-6">
      <div class="min-w-0 flex-1 lg:max-w-[75%]">
        <div class="mb-1 flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
          <h3 class="min-w-0 break-all text-[20px] font-semibold leading-[30px]"><a href="${escapeHtml(groupLinkUrl(group))}" class="text-accent hover:underline">${escapeHtml(repo.name || "repository")}</a></h3>
          <span class="inline-flex shrink-0 items-center rounded-full border border-border px-[7px] text-xs font-medium leading-[18px] text-muted-foreground">${escapeHtml(visibility)}</span>
          ${repositoryTermsBadge(repo)}
        </div>
        ${publishedFrom}
        ${description ? `<p class="mb-2 break-words pr-6 text-sm text-muted-foreground">${escapeHtml(description)}</p>` : ""}
        <div class="mt-2 flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-muted-foreground">${meta}</div>
      </div>
      <div class="flex shrink-0 flex-col items-end justify-between gap-3">
        <button data-repo-star-button data-repo-key="${escapeHtml(key)}" type="button" aria-pressed="false" aria-label="Star ${escapeHtml(`${displayOwner || repo.owner || ""}/${repo.name || ""}`)}" class="inline-flex h-7 items-center gap-1.5 rounded-md border border-border bg-secondary px-3 text-xs font-medium text-foreground hover:bg-muted">
          <i data-lucide="star" data-repo-star-icon class="h-4 w-4 text-muted-foreground"></i>
          <span data-repo-star-label>Star</span>
          <span data-repo-star-count class="hidden">${formatCount(0)}</span>
        </button>
        ${profileRepositorySparkline(groupActivityWeeks(group))}
      </div>
    </article>`;
  }

  function updateRepositoryPagination() {
    const total = state.filteredGroups.length;
    const pages = Math.max(1, Math.ceil(total / state.pageSize));
    state.page = Math.min(Math.max(1, state.page), pages);

    const start = (state.page - 1) * state.pageSize;
    const end = Math.min(start + state.pageSize, total);
    const visible = state.filteredGroups.slice(start, end);
    const list = $("#repoList");
    const summary = $("[data-repo-summary]");
    const prev = $("[data-repo-prev]");
    const next = $("[data-repo-next]");
    const pageList = $("[data-repo-pages]");

    if (state.repositoriesLoading) {
      if (list) {
        list.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading repositories from an online node...")}</div>`;
      }
      if (summary) summary.textContent = "Loading repositories from an online node";
      if (prev) {
        prev.disabled = true;
        prev.classList.add("opacity-40");
      }
      if (next) {
        next.disabled = true;
        next.classList.add("opacity-40");
      }
      if (pageList) pageList.innerHTML = "";
      return;
    }

    if (list) {
      list.innerHTML = visible.length
        ? visible.map(repositoryCard).join("")
        : `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${emptyListHtml("No repositories match this filter.", $("#repoSearch")?.value)}</div>`;
    }
    if (summary) {
      summary.textContent = total
        ? `Showing ${start + 1}-${end} of ${total} repositories`
        : "0 mirrored repositories";
    }
    if (prev) {
      prev.disabled = state.page <= 1;
      prev.classList.toggle("opacity-40", prev.disabled);
    }
    if (next) {
      next.disabled = state.page >= pages;
      next.classList.toggle("opacity-40", next.disabled);
    }
    if (pageList) {
      pageList.innerHTML = "";
      for (let page = 1; page <= pages; page += 1) {
        const button = document.createElement("button");
        const active = page === state.page;
        button.type = "button";
        button.textContent = String(page);
        button.dataset.dashboardRepoPage = String(page);
        button.className = [
          "h-8 min-w-8 rounded-md border px-2 text-xs font-mono transition-colors",
          active
            ? "border-border bg-secondary text-foreground"
            : "border-transparent text-muted-foreground hover:border-border hover:bg-secondary hover:text-foreground",
        ].join(" ");
        pageList.append(button);
      }
    }

    window.lucide?.createIcons();
    hydrateRepoStarButtons(list);
    hydrateNativeRepositoryLogos(list);
  }

  function renderSidebarRepositories(session) {
    const list = $("[data-sidebar-repo-list]");
    const count = $("[data-sidebar-repo-count]");
    const groups = Array.isArray(state.filteredGroups)
      ? state.filteredGroups
      : mergeGroupsByPublicAddress(groupRepositories(state.repositories || []));
    const organizationRepositories = Array.isArray(
      state.homeOrganizationRepositories,
    )
      ? state.homeOrganizationRepositories
      : [];
    const entries = [
      ...organizationRepositories.map((repo) => ({
        repo,
        key: repoHasShortAddress(repo) ? String(repo.name) : `${repo.owner}/${repo.name}`,
        href: repoPathUrl(repo),
        organization: true,
      })),
      ...groups.map((group) => {
        const repo = sourceOfTruth(group);
        return {
          repo,
          key: groupDisplayName(group),
          href: groupLinkUrl(group),
          organization: false,
        };
      }),
    ].filter((entry, index, values) =>
      values.findIndex((candidate) =>
        candidate.key.toLowerCase() === entry.key.toLowerCase()) === index);
    if (count) count.textContent = formatCount(entries.length);
    if (!list) return;
    if (!entries.length) {
      list.innerHTML = '<div class="dashboard-nav-repo-empty">No mirrored repositories yet.</div>';
      return;
    }
    list.innerHTML = entries.slice(0, 6).map((entry) => {
      const repo = entry.repo;
      const live = repoIsLive(repo);
      const label = repoLabelParts(entry.key);
      return `
        <a href="${escapeHtml(entry.href)}" class="dashboard-nav-repo" data-sidebar-repo="${escapeHtml(entry.key)}">
          <span class="h-2 w-2 shrink-0 rounded-full ${live ? "bg-success" : "bg-muted-foreground/40"}"></span>
          <span class="min-w-0 flex-1 truncate">${label.owner ? `<span class="text-muted-foreground">${escapeHtml(label.owner)}/</span>` : ""}<span class="text-foreground">${escapeHtml(label.name || repo.name || "repository")}</span></span>
          ${repositoryTermsBadge(repo, true)}
          ${entry.organization ? '<span class="shrink-0 rounded border border-border px-1 py-0.5 font-mono text-[8px] uppercase text-muted-foreground">org</span>' : ""}
          <i data-lucide="arrow-right" class="dashboard-nav-repo-arrow"></i>
        </a>`;
    }).join("");
    window.lucide?.createIcons();
  }

  const HOME_TOP_REPOSITORY_LIMIT = 8;
  const HOME_SHOW_MORE_MIN_REPOSITORIES = 25;
  const HOME_REPO_SEARCH_MIN_REPOSITORIES = 10;
  const HOME_REPOSITORY_VIEW_LOOKUP_LIMIT = 200;

  function homeRepositoryViewTotals(repo) {
    return state.homeRepositoryViews?.[repositoryViewKey(repo)] || null;
  }

  function homeRepositoryLastViewedAt(entry) {
    return Number(homeRepositoryViewTotals(entry.repo)?.lastViewedAt) || 0;
  }

  async function loadHomeRepositoryViews(keys, signature) {
    try {
      const response = await fetch("/api/repo-views", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ repos: keys }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data.ok === false) throw new Error(data.error || `HTTP ${response.status}`);
      if (state.homeRepositoryViewsSignature !== signature) return;
      state.homeRepositoryViews = {
        ...(state.homeRepositoryViews || {}),
        ...(data.views && typeof data.views === "object" ? data.views : {}),
      };
      renderHomeRepositories();
    } catch (_) {
      if (state.homeRepositoryViewsSignature === signature) state.homeRepositoryViewsSignature = "";
    }
  }

  function refreshHomeRepositoryViews(entries) {
    const keys = [...new Set(entries
      .filter((entry) => !entry.repo?.isPrivate)
      .map((entry) => repositoryViewKey(entry.repo)))]
      .filter(isRepoKey)
      .slice(0, HOME_REPOSITORY_VIEW_LOOKUP_LIMIT);
    const signature = keys.join("\n");
    if (!keys.length || signature === state.homeRepositoryViewsSignature) return;
    state.homeRepositoryViewsSignature = signature;
    loadHomeRepositoryViews(keys, signature);
  }

  function homeRepositoryEntries() {
    return [
      ...(Array.isArray(state.homeOrganizationRepositories)
        ? state.homeOrganizationRepositories
        : []).map((repo) => ({
          repo,
          href: repoPathUrl(repo),
          organization: true,
        })),
      ...mergeGroupsByPublicAddress(groupRepositories(state.repositories || [])).map((group) => {
        const repo = sourceOfTruth(group);
        return { repo, label: groupDisplayName(group), href: groupLinkUrl(group), organization: false };
      }),
    ].map((entry) => ({ ...entry, label: entry.label || repoDisplayName(entry.repo) }))
      .filter((entry, index, values) =>
        values.findIndex((candidate) =>
          candidate.label.toLowerCase() === entry.label.toLowerCase()) === index)
      .sort((a, b) => homeRepositoryLastViewedAt(b) - homeRepositoryLastViewedAt(a));
  }

  function homeRepositoryRow(entry) {
    const repo = entry.repo;
    const live = repoIsLive(repo);
    const status = live
      ? (repoServedByMirror(repo) ? "served by mirror" : "live host")
      : "offline";
    const statusHtml = dashboardOnInstance() ? "" : `<span data-home-repo-served-by class="inline-flex items-center gap-1.5"><span class="h-2 w-2 shrink-0 rounded-full ${live ? "bg-success" : "bg-muted-foreground/40"}"></span>${status}</span>`;
    const logo = nativeRepositoryLogoMarkup(repo, "h-8 w-8")
      || '<span class="flex h-8 w-8 shrink-0 items-center justify-center rounded-xl border border-border bg-secondary text-muted-foreground"><i data-lucide="book-marked" class="h-4 w-4"></i></span>';
    const description = String(repo.description || "").trim();
    const views = Number(homeRepositoryViewTotals(repo)?.views) || 0;
    return `<a href="${escapeHtml(entry.href)}" data-home-repo-row class="flex min-w-0 items-start gap-2.5 rounded-md px-2 py-2 text-left text-sm text-muted-foreground hover:bg-secondary hover:text-foreground">
      ${logo}
      <span class="min-w-0 flex-1">
        <span class="block truncate font-medium text-foreground" title="${escapeHtml(entry.label || repoDisplayName(repo))}">${escapeHtml(entry.label || repoDisplayName(repo))}</span>
        ${description ? `<span class="mt-0.5 block line-clamp-2 text-xs leading-5 text-muted-foreground">${escapeHtml(description)}</span>` : ""}
        <span class="mt-1 flex flex-wrap items-center gap-2 text-xs text-muted-foreground">
          ${statusHtml}
          ${entry.organization ? '<span class="shrink-0 text-[9px] uppercase text-muted-foreground">organization</span>' : ""}
          ${views ? `<span data-home-repo-views class="inline-flex items-center gap-1"><i data-lucide="eye" class="h-3 w-3"></i>${escapeHtml(formatCount(views))} ${views === 1 ? "view" : "views"}</span>` : ""}
          ${repositoryTermsBadge(repo, true)}
        </span>
      </span>
    </a>`;
  }

  function renderHomeRepositories() {
    const container = $("[data-home-top-repositories]");
    if (!container) return;
    const all = homeRepositoryEntries();
    refreshHomeRepositoryViews(all);
    const searchField = $("[data-home-repo-search-field]");
    const searchable = all.length >= HOME_REPO_SEARCH_MIN_REPOSITORIES;
    searchField?.classList.toggle("hidden", !searchable);
    searchField?.classList.toggle("flex", searchable);
    const search = $("[data-home-repo-search]");
    if (!searchable && search?.value) search.value = "";
    const query = (search?.value || "").trim().toLowerCase();
    const matches = all.filter((entry) => repositoryMatchesPageFilter(entry.repo, query));
    const entries = state.homeRepositoriesExpanded
      ? matches
      : matches.slice(0, HOME_TOP_REPOSITORY_LIMIT);
    const showMore = $("[data-home-repos-show-more]");
    if (showMore) {
      showMore.classList.toggle(
        "hidden", matches.length <= HOME_SHOW_MORE_MIN_REPOSITORIES);
      showMore.textContent = state.homeRepositoriesExpanded
        ? "Show less"
        : "Show more";
    }
    if (!entries.length && !query && state.repositoriesLoading) return;
    container.innerHTML = entries.length
      ? entries.map(homeRepositoryRow).join("")
      : `<div class="px-2 py-3 text-sm text-muted-foreground">${query
        ? emptyListHtml("No repositories match this filter.", query)
        : 'No repositories yet. <a href="/dashboard/repos/new?source=remote" class="text-accent hover:underline">Mirror one from GitHub, GitLab or Codeberg, or create one</a>.'}</div>`;
    window.lucide?.createIcons();
    hydrateNativeRepositoryLogos(container);
  }

  const HOME_FEED_CARD_LIMIT = 8;

  function homeFeedActor(item) {
    return String(item?.actor || "").trim().replace(/^@/, "").toLowerCase();
  }

  function homeFeedItems() {
    const self = profileRepositoryAliases(state.session);
    return (Array.isArray(state.notifications) ? state.notifications : [])
      .filter((item) => {
        const actor = homeFeedActor(item);
        return actor && !self.has(actor);
      })
      .slice(0, HOME_FEED_CARD_LIMIT);
  }

  function homeFeedCard(item) {
    const href = repoPublicHref(String(item?.href || "").trim());
    const tag = href ? "a" : "div";
    const hrefAttr = href ? ` href="${escapeHtml(href)}"` : "";
    const meta = [
      userNameHtml(`@${homeFeedActor(item)}`),
      escapeHtml(notificationTimeLabel(item.ts)),
      item.repo ? escapeHtml(repoPublicKey(item.repo)) : "",
    ].filter(Boolean).join(" · ");
    return `
      <${tag}${hrefAttr} data-home-feed-kind="activity" class="flex min-w-0 items-start gap-2.5 rounded-md border border-border bg-background px-3 py-2.5 hover:bg-secondary">
        <i data-notification-kind-icon data-lucide="${notificationIcon(item.kind)}" class="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground"></i>
        <span class="min-w-0 flex-1">
          <span class="block break-words text-sm font-medium text-foreground">${escapeHtml(repoPublicText(item.title) || `${platformName()} activity`)}</span>
          ${item.body ? `<span class="mt-0.5 block line-clamp-2 text-xs leading-5 text-muted-foreground">${escapeHtml(repoPublicText(item.body))}</span>` : ""}
          <span class="mt-1 block break-words text-xs text-muted-foreground">${meta}</span>
        </span>
      </${tag}>
    `;
  }

  function renderHomeFeed() {
    const container = $("[data-home-feed]");
    if (!container) return;
    const cards = homeFeedItems().map(homeFeedCard);
    container.innerHTML = cards.length
      ? cards.join("")
      : '<div class="rounded-md border border-border bg-background px-3 py-3 text-sm text-muted-foreground">Nothing from others yet. Mentions, pull requests and pushes by other people show up here.</div>';
    window.lucide?.createIcons();
  }

  // Home right-rail "Latest from the blog" (adhoc #381): the newest feature
  // posts with their artwork, read from the blog's own RSS document. The feed
  // is derived from the shipped blog index and edge-cached for thirty minutes
  // (see blog_feed.py), so this is one cheap same-origin read per dashboard
  // load rather than a second hand-maintained copy of the post list.
  const HOME_BLOG_POST_LIMIT = 3;
  const HOME_BLOG_FEED_URL = "/rss.xml";

  // Feed URLs are absolute against forkmesh.com; keep only the path so post
  // links stay on whatever origin the dashboard is served from (the App worker
  // 308s /blog/* to WWW, so a path link still lands on the post).
  function homeBlogUrl(value) {
    const raw = String(value || "").trim();
    if (!raw) return "";
    try {
      const parsed = new URL(raw, window.location.origin);
      return parsed.pathname + parsed.search;
    } catch (_) {
      return "";
    }
  }

  // Artwork cannot take that shortcut: the blog images live in the WWW
  // worker's asset bundle only (build_site_assets.shared_assets deliberately
  // skips assets/blog for App and World), so a path-only src 404s on the
  // dashboard's own origin and every card paints a broken image. Keep the
  // feed's absolute URL, but only for ForkMesh's own sites, so a rewritten
  // feed can still never make the dashboard fetch a foreign host.
  const HOME_BLOG_IMAGE_HOSTS = new Set(["forkmesh.com", "www.forkmesh.com"]);

  function homeBlogImageUrl(value) {
    const raw = String(value || "").trim();
    if (!raw) return "";
    try {
      const parsed = new URL(raw, window.location.origin);
      if (parsed.origin === window.location.origin) {
        return parsed.pathname + parsed.search;
      }
      if (parsed.protocol !== "https:") return "";
      if (!HOME_BLOG_IMAGE_HOSTS.has(parsed.hostname.toLowerCase())) return "";
      return parsed.href;
    } catch (_) {
      return "";
    }
  }

  function parseHomeBlogFeed(xml) {
    const doc = new DOMParser().parseFromString(String(xml || ""), "application/xml");
    if (doc.querySelector("parsererror")) return [];
    return Array.from(doc.querySelectorAll("item"))
      .map((item) => ({
        title: (item.querySelector("title")?.textContent || "").trim(),
        href: homeBlogUrl(item.querySelector("link")?.textContent),
        meta: (item.querySelector("category")?.textContent || "").trim(),
        image: homeBlogImageUrl(item.querySelector("enclosure")?.getAttribute("url")),
      }))
      .filter((post) => post.title && post.href)
      .slice(0, HOME_BLOG_POST_LIMIT);
  }

  function homeBlogPostCard(post) {
    return `
      <a href="${escapeHtml(post.href)}" class="group block overflow-hidden rounded-md border border-border hover:bg-secondary">
        ${post.image ? `<img src="${escapeHtml(post.image)}" alt="" loading="lazy" class="block aspect-[16/9] w-full object-cover" />` : ""}
        <span class="block px-3 py-2.5">
          ${post.meta ? `<span class="block truncate font-mono text-[10px] uppercase text-muted-foreground">${escapeHtml(post.meta)}</span>` : ""}
          <span class="mt-1 block text-sm font-semibold leading-5 text-foreground group-hover:text-accent">${escapeHtml(post.title)}</span>
        </span>
      </a>
    `;
  }

  function renderHomeBlogPosts() {
    const container = $("[data-home-blog-list]");
    if (!container) return;
    const posts = state.homeBlogPosts;
    if (posts === null) {
      container.innerHTML = '<div class="text-sm text-muted-foreground"><span class="fm-spinner" aria-hidden="true"></span>Loading blog posts...</div>';
      return;
    }
    container.innerHTML = posts.length
      ? posts.map(homeBlogPostCard).join("")
      : '<div class="text-sm text-muted-foreground">Blog posts are unavailable right now.</div>';
  }

  async function loadHomeBlogPosts() {
    if (!$("[data-home-blog-list]")) return;
    if (!(await instancePluginEnabled("blog"))) return;
    try {
      const response = await fetch(HOME_BLOG_FEED_URL, {
        credentials: "omit",
        headers: { accept: "application/rss+xml, application/xml" },
      });
      if (!response.ok) throw new Error(`blog feed returned ${response.status}`);
      state.homeBlogPosts = parseHomeBlogFeed(await response.text());
    } catch (_) {
      state.homeBlogPosts = [];
    }
    renderHomeBlogPosts();
  }

  let homeOrganizationRepositoriesPromise = null;

  function ensureHomeOrganizationRepositories() {
    if (!homeOrganizationRepositoriesPromise) {
      homeOrganizationRepositoriesPromise = loadHomeOrganizationRepositories();
    }
    return homeOrganizationRepositoriesPromise;
  }

  async function loadHomeOrganizationRepositories() {
    if (!state.session?.sessionToken) {
      state.homeOrganizationRepositories = [];
      return;
    }
    try {
      const organizations = await fetchJson("/api/orgs");
      const orgs = Array.isArray(organizations?.orgs)
        ? organizations.orgs.slice(0, 20)
        : [];
      const linked = await Promise.all(orgs.map(async (organization) => {
        const owner = String(organization?.name || "").trim().toLowerCase();
        if (!owner) return [];
        try {
          const data = await fetchJson(
            `/api/orgs/${encodeURIComponent(owner)}/repos`,
          );
          return (Array.isArray(data?.repos) ? data.repos : []).map((item) => ({
            owner,
            name: String(item?.repo || "").trim(),
            linkedNode: String(item?.node || "").trim(),
            source: "organization-alias",
            organizationOwned: true,
            cloneOnline: true,
          })).filter((repo) => repo.name);
        } catch (_) {
          return [];
        }
      }));
      state.homeOrganizationRepositories = linked.flat();
    } catch (_) {
      state.homeOrganizationRepositories = [];
    }
    renderSidebarRepositories(state.session);
    renderHomeRepositories();
  }

  const HOME_AGENT_LIMIT = 10;

  function homeAgentRow(session, account) {
    const owner = String(session.org || account || "").trim();
    const repo = String(session.repo || "").trim();
    const repoLabel = owner && repo ? `${owner}/${repo}` : repo;
    const href = owner && repo
      ? `${repoPathUrl({ owner, name: repo })}/agents`
      : "";
    const tag = href ? "a" : "div";
    const hrefAttr = href ? ` href="${escapeHtml(href)}"` : "";
    const status = String(session.displayStatus || session.status || "unknown");
    const title = String(session.title || "").trim()
      || (session.issueNumber ? `Issue #${session.issueNumber}` : `${orgAgentProviderLabel(session)} session`);
    const meta = [
      repoLabel,
      orgAgentProviderLabel(session),
      session.targetNode ? `on ${session.targetNode}` : "",
      notificationTimeLabel(session.updatedAt || session.createdAt),
    ].filter(Boolean).map(escapeHtml);
    return `
      <${tag}${hrefAttr} data-home-agent="${escapeHtml(String(session.id || ""))}" class="flex min-w-0 items-start gap-3 rounded-lg border border-border bg-card px-4 py-3 hover:bg-secondary/40">
        <span class="mt-1.5 h-2 w-2 shrink-0 rounded-full ${orgAgentStatusDotClass(session.status)}"></span>
        <span class="min-w-0 flex-1">
          <span class="flex min-w-0 items-start justify-between gap-3">
            <span class="min-w-0 break-words text-sm font-medium text-foreground">${escapeHtml(title)}</span>
            <span class="shrink-0 font-mono text-xs ${repoAgentStatusTone(session.status)}">${escapeHtml(status.replace(/_/g, " "))}</span>
          </span>
          <span class="mt-1 flex flex-wrap gap-x-3 gap-y-0.5 text-xs text-muted-foreground">${meta.map((part) => `<span>${part}</span>`).join("")}</span>
        </span>
      </${tag}>`;
  }

  function renderHomeAgentSessions() {
    const container = $("[data-home-agents]");
    if (!container) return;
    const count = $("[data-home-agents-count]");
    const data = state.homeAgentSessions;
    if (data === null) return;
    const sessions = Array.isArray(data?.sessions) ? data.sessions : [];
    if (count) {
      count.textContent = data?.activeCount ? `${formatCount(data.activeCount)} running` : "";
    }
    if (data?.error) {
      container.innerHTML = `<div class="rounded-lg border border-border bg-card px-4 py-6 text-sm text-muted-foreground">${escapeHtml(data.error)}</div>`;
      return;
    }
    container.innerHTML = sessions.length
      ? sessions.slice(0, HOME_AGENT_LIMIT).map((session) => homeAgentRow(session, data.account)).join("")
      : '<div class="rounded-lg border border-border bg-card px-4 py-6 text-sm text-muted-foreground">No agents yet. Type a prompt above and press new to start one.</div>';
  }

  async function loadHomeAgentSessions() {
    if (!$("[data-home-agents]")) return;
    if (!state.session?.sessionToken) {
      state.homeAgentSessions = { sessions: [], error: "Sign in to see your agents." };
      renderHomeAgentSessions();
      return;
    }
    try {
      const payload = await orgAgentRequest("GET", "/api/account/agent-sessions");
      state.homeAgentSessions = {
        account: String(payload?.account || state.session?.nodeName || ""),
        activeCount: Number(payload?.activeCount) || 0,
        sessions: Array.isArray(payload?.sessions) ? payload.sessions : [],
      };
    } catch (_) {
      state.homeAgentSessions = { sessions: [], error: "Agents are unavailable right now." };
    }
    renderHomeAgentSessions();
  }

  // The repo groups the profile pages list: ONLY the viewed account's own
  // repos - the signed-in session on /dashboard/profile, the fetched account
  // in public-profile mode (/@name). profileRepositoryAliases() already
  // defaults to that subject. The authenticated catalog deliberately returns
  // every public repo plus the private repos shared WITH the viewer, so the
  // profile has to scope by owner itself; without it "Your repositories"
  // listed the whole mesh and counted other people's repos as the account's
  // own (#538). An account with no resolvable identity owns nothing.
  function profileRepositoryGroups() {
    const groups = mergeGroupsByPublicAddress(
      groupRepositories(state.repositories || []));
    const aliases = profileRepositoryAliases();
    if (!aliases.size) return [];
    return groups.filter((group) =>
      repoBelongsToProfile(sourceOfTruth(group), aliases) ||
      (group.members || []).some((member) => repoBelongsToProfile(member, aliases)));
  }

  function renderProfileRepositories() {
    renderOrganizationRepositories();
    const container = $("[data-profile-repo-list]");
    if (!container) return;
    const query = ($("[data-profile-repo-search]")?.value || "").trim().toLowerCase();
    const groups = profileRepositoryGroups().filter((group) => {
      return repositoryMatchesPageFilter(sourceOfTruth(group), query);
    });
    container.innerHTML = groups.length
      ? groups.map((group) => profileRepositoryRow(group)).join("")
      : `<div class="border-b border-border py-8 text-sm text-muted-foreground">${emptyListHtml("No repositories match this filter.", query)}</div>`;
    window.lucide?.createIcons();
    observeProfileRepositoryStars(container);
    hydrateRepoStarButtons(container);
  }

  function renderProfileRepositoryCount(count = profileRepositoryGroups().length) {
    $$("[data-profile-repo-count]").forEach((element) => {
      element.textContent = formatCount(count);
    });
  }

  // Catalog groups are keyed by root commit, so one repository whose history
  // was rewritten arrives as several groups that all resolve to the same
  // public address. Listing them separately shows the same name three times,
  // so the richest group keeps the source of truth and the rest hand over
  // their mirrors.
  function groupHostRank(group) {
    return (repoIsLive(sourceOfTruth(group)) ? 1e6 : 0) + (group.members || []).length;
  }

  function mergeGroupsByPublicAddress(groups) {
    const merged = new Map();
    for (const group of groups) {
      const origin = sourceOfTruth(group);
      const key = groupDisplayName(group).toLowerCase();
      const seen = merged.get(key);
      if (!seen) {
        merged.set(key, group);
        continue;
      }
      const winner = groupHostRank(group) > groupHostRank(seen) ? group : seen;
      const other = winner === group ? seen : group;
      merged.set(key, {
        ...winner,
        members: [...(winner.members || []), ...(other.members || [])],
      });
    }
    return [...merged.values()];
  }

  function applyRepositoryFilter() {
    const query = ($("#repoSearch")?.value || "").trim().toLowerCase();
    state.filteredRepositories = state.repositories.filter((repo) =>
      repositoryMatchesPageFilter(repo, query));
    state.filteredGroups = mergeGroupsByPublicAddress(
      groupRepositories(state.filteredRepositories));
    updateRepositoryPagination();
    renderSidebarRepositories();
    renderHomeRepositories();
    renderHomeFeed();
    renderHomeBlogPosts();
    renderHomeAgentSessions();
    renderProfileRepositories();
    renderProfileRepositoryCount();
  }

  function renderRepositories(repositories, session) {
    state.repositoriesLoading = false;
    state.repositories = withoutDeletedRepositories(
      Array.isArray(repositories) ? repositories : []);
    rememberAndPreconnectDirectMirrors(state.repositories);
    state.filteredRepositories = state.repositories.slice();
    state.filteredGroups = mergeGroupsByPublicAddress(
      groupRepositories(state.repositories));
    state.page = 1;

    const count = $("[data-repo-count]");
    if (count) count.textContent = `${formatCount(state.filteredGroups.length)} mirrored`;

    renderProfileRepositoryCount();
    applyRepositoryFilter();
    renderGlobalSearchResults();
  }

  function externalRepositoryCard(repository) {
    const logo = String(repository?.logo?.dataUrl || "");
    const hosted = Boolean(repository?.mirrored && repository?.mirror?.owner && repository?.mirror?.name);
    const status = hosted
      ? `Fully hosted by ${platformName()}`
      : String(repository?.statusLabel || "External repository");
    const provider = String(repository?.attribution?.provider || repository?.provider || "Provider");
    const original = String(repository?.originalUrl || "");
    const forkmeshUrl = hosted
      ? repoPathUrl({ owner: repository.targetOwner, name: repository.name })
      : original;
    const canVolunteer = Boolean(state.session?.sessionToken) &&
      !["actively_mirrored", "archived"].includes(String(repository?.status || ""));
    const volunteeredNode = state.externalMirrorVolunteers.get(String(repository?.id || "")) || "";
    const manageable = Boolean(repository?.canManage);
    const migrationJob = repository?.migration;
    const migrationComplete = migrationJob?.status === "completed";
    // The signed report says what arrived, what was skipped or failed, and
    // what stays external (issue #490); its summary is plain language already.
    const migrationReport = migrationJob?.report?.latest;
    const selected = state.externalRepositorySelection.has(String(repository?.id || ""));
    const incompleteItems = Array.isArray(repository?.metadataIncomplete)
      ? repository.metadataIncomplete.filter((item) => !migrationComplete || !String(item).endsWith(":transfer_pending"))
      : [];
    const incomplete = incompleteItems.length
      ? `<p class="mt-2 text-[11px] text-attention">Some metadata was unavailable or rate-limited during import.</p>`
      : "";
    // "Complete" here only ever meant the signed provider-metadata snapshot;
    // Git contents are hosted by a mirror node, so a finished job on an
    // unmirrored entry must not read as "mirrored by ForkMesh" (issue #678).
    const migrationSummary = migrationJob
      ? `<div class="mt-2 rounded-md border border-border bg-background px-2.5 py-2 text-[11px] text-muted-foreground">
          <span class="font-medium text-foreground">${migrationComplete ? `Signed metadata import complete${hosted ? "" : " · Git contents not mirrored"}` : `Resumable import · ${escapeHtml(String(migrationJob.status || "pending"))}`}</span>
          <span aria-hidden="true"> · </span>
          <span>${formatCount(Object.values(migrationJob.counts || {}).reduce((sum, value) => sum + Number(value || 0), 0))} objects</span>
          <span aria-hidden="true"> · </span>
          <span>setup ${escapeHtml(migrationDuration(migrationJob.setup?.durationMs))}; transfer ${migrationComplete ? escapeHtml(migrationDuration(migrationJob.transfer?.durationMs)) : "separate"}</span>
          ${migrationReport?.available ? `<p class="mt-1.5 leading-4" data-repository-migration-report-summary>${escapeHtml((migrationReport.summary || []).join(" "))}</p>` : ""}
        </div>`
      : "";
    return `
      <article class="flex flex-col gap-3 px-4 py-4 sm:flex-row sm:items-start sm:px-5" data-external-repo-id="${escapeHtml(repository?.id || "")}">
        ${manageable ? `<label class="mt-3 inline-flex shrink-0 items-center" title="Select ${escapeHtml(repository?.fullName || repository?.name || "repository")}">
          <input data-external-repo-select="${escapeHtml(repository?.id || "")}" type="checkbox" ${selected ? "checked" : ""} class="h-4 w-4 rounded border-border bg-card accent-primary" />
          <span class="sr-only">Select ${escapeHtml(repository?.fullName || repository?.name || "repository")}</span>
        </label>` : ""}
        ${logo
          ? `<img src="${escapeHtml(logo)}" alt="" class="h-12 w-12 shrink-0 rounded-xl border border-border bg-secondary object-cover" />`
          : `<span class="flex h-12 w-12 shrink-0 items-center justify-center rounded-xl border border-border bg-secondary text-muted-foreground"><i data-lucide="book-marked" class="h-5 w-5"></i></span>`}
        <div class="min-w-0 flex-1">
          <div class="flex flex-wrap items-center gap-2">
            <a href="${escapeHtml(forkmeshUrl)}" ${hosted ? "" : 'target="_blank" rel="noopener noreferrer"'} class="truncate font-mono text-sm font-semibold text-foreground hover:text-accent hover:underline">${escapeHtml(repository?.fullName || repository?.name || "External repository")}</a>
            <span class="rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold text-muted-foreground">${escapeHtml(status)}</span>
            ${repository?.isPrivate ? '<span class="rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold text-muted-foreground">Private · owner only</span>' : ""}
          </div>
          <p class="mt-1 text-xs leading-5 text-muted-foreground">${escapeHtml(repository?.metadata?.description || "No provider description.")}</p>
          <p class="mt-2 text-[11px] leading-4 text-muted-foreground">${escapeHtml(hosted ? `The complete Git repository is synced, cloneable, and hosted by ${platformName()}.` : repository?.mirrorNotice || `This external entry is not mirrored by ${platformName()}.`)}</p>
          ${migrationSummary}
          ${incomplete}
          <div class="mt-3 flex flex-wrap items-center gap-2 text-[11px] text-muted-foreground">
            <a href="${escapeHtml(original)}" target="_blank" rel="noopener noreferrer" class="font-medium text-accent hover:underline">Open on ${escapeHtml(provider)}</a>
            ${repository?.targetOwner ? `<span aria-hidden="true">·</span><span>Listed under ${repository?.targetOwnerType === "organization" ? "organization " : ""}<span class="font-mono text-foreground">${escapeHtml(repository.targetOwner)}</span></span>` : ""}
            <span aria-hidden="true">·</span>
            <span>${hosted ? `Full Git data hosted by ${escapeHtml(platformName())}` : `${escapeHtml(platformName())} does not own or control this repository`}</span>
          </div>
        </div>
        <div class="flex shrink-0 flex-col gap-2">
          ${migrationJob && !migrationComplete ? `
            <button type="button" data-repository-migration-resume="${escapeHtml(migrationJob.id || "")}" class="inline-flex h-8 items-center justify-center gap-1.5 rounded-md bg-cta px-3 text-xs font-semibold text-cta-foreground hover:bg-cta/90">
              <i data-lucide="rotate-cw" class="h-3.5 w-3.5"></i>
              Continue import
            </button>` : ""}
          ${migrationJob && migrationComplete && migrationReport?.retryable ? `
            <button type="button" data-repository-migration-retry="${escapeHtml(migrationJob.id || "")}" class="inline-flex h-8 items-center justify-center gap-1.5 rounded-md bg-cta px-3 text-xs font-semibold text-cta-foreground hover:bg-cta/90">
              <i data-lucide="rotate-cw" class="h-3.5 w-3.5"></i>
              Retry failed work
            </button>` : ""}
          ${migrationJob ? `
            <button type="button" data-repository-migration-report="${escapeHtml(migrationJob.id || "")}" class="inline-flex h-8 items-center justify-center gap-1.5 rounded-md border border-border px-3 text-xs font-semibold text-foreground hover:bg-secondary disabled:cursor-progress disabled:opacity-60">
              <i data-lucide="download" class="h-3.5 w-3.5"></i>
              ${migrationReport?.available ? "Download signed report" : "Download draft report"}
            </button>` : ""}
          ${canVolunteer && volunteeredNode ? `
            <span data-external-mirror-volunteered="${escapeHtml(repository?.id || "")}" role="status" class="inline-flex h-8 items-center justify-center gap-1.5 rounded-md border border-success/40 bg-success/10 px-3 text-xs font-semibold text-foreground" title="Recorded for ${escapeHtml(volunteeredNode)}">
              <i data-lucide="check" class="h-3.5 w-3.5"></i>
              Mirror requested · <span class="font-mono">${escapeHtml(volunteeredNode)}</span>
            </span>` : ""}
          ${canVolunteer && !volunteeredNode ? `
            <button type="button" data-external-mirror-volunteer="${escapeHtml(repository?.id || "")}" class="inline-flex h-8 items-center justify-center gap-1.5 rounded-md border border-border px-3 text-xs font-semibold text-foreground hover:bg-secondary disabled:cursor-progress disabled:opacity-60">
              <i data-lucide="copy-plus" class="h-3.5 w-3.5"></i>
              Volunteer to mirror
            </button>` : ""}
        </div>
      </article>`;
  }

  function renderExternalRepositories(repositories) {
    state.externalRepositoriesLoading = false;
    const migrations = new Map(
      (state.repositoryMigrations || []).map((job) => [String(job.repositoryId || ""), job]));
    state.externalRepositories = (Array.isArray(repositories) ? repositories : []).map((repository) => ({
      ...repository,
      migration: migrations.get(String(repository?.id || "")) || null,
    }));
    const list = $("[data-external-repo-list]");
    const count = $("[data-external-repo-count]");
    if (count) count.textContent = `${formatCount(state.externalRepositories.length)} external`;
    if (!list) return;
    list.innerHTML = state.externalRepositories.length
      ? state.externalRepositories.map(externalRepositoryCard).join("")
      : '<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">No external repositories or stubs have been listed yet. Use “New repository” to import one.</div>';
    syncExternalRepositoryActions();
    window.lucide?.createIcons();
  }

  function syncExternalRepositoryActions() {
    const manageable = state.externalRepositories.filter((repository) => repository?.canManage);
    const manageableIds = new Set(manageable.map((repository) => String(repository.id || "")));
    for (const id of [...state.externalRepositorySelection]) {
      if (!manageableIds.has(id)) state.externalRepositorySelection.delete(id);
    }
    const actions = $("[data-external-repo-actions]");
    actions?.classList.toggle("hidden", manageable.length === 0);
    actions?.classList.toggle("flex", manageable.length > 0);
    const all = $("[data-external-repo-select-all]");
    if (all) {
      all.checked = manageable.length > 0 &&
        manageable.every((repository) => state.externalRepositorySelection.has(String(repository.id || "")));
      all.indeterminate = state.externalRepositorySelection.size > 0 && !all.checked;
    }
    const button = $("[data-external-repo-delete-selected]");
    if (button) {
      const count = state.externalRepositorySelection.size;
      button.disabled = count === 0;
      button.lastChild.textContent = count ? ` Delete selected (${count})` : " Delete selected";
    }
  }

  function toggleAllExternalRepositories(checked) {
    state.externalRepositorySelection.clear();
    if (checked) {
      state.externalRepositories
        .filter((repository) => repository?.canManage)
        .forEach((repository) => state.externalRepositorySelection.add(String(repository.id || "")));
    }
    renderExternalRepositories(state.externalRepositories);
  }

  async function deleteSelectedExternalRepositories() {
    const ids = [...state.externalRepositorySelection];
    if (!ids.length || !state.session?.sessionToken) return;
    if (!window.confirm(`Delete ${ids.length} selected external ${ids.length === 1 ? "repository" : "repositories"}? This removes ${platformName()} metadata and does not delete anything from the source provider.`)) return;
    const button = $("[data-external-repo-delete-selected]");
    if (button) button.disabled = true;
    try {
      for (const id of ids) {
        const response = await fetch(`/api/repository-imports/${encodeURIComponent(id)}`, {
          method: "DELETE",
          body: JSON.stringify({ sessionToken: state.session.sessionToken }),
          headers: {
            accept: "application/json",
            authorization: `Bearer ${state.session.sessionToken}`,
            "content-type": "application/json",
          },
          cache: "no-store",
        });
        const payload = await response.json().catch(() => ({}));
        if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
        state.externalRepositorySelection.delete(id);
      }
      await loadExternalRepositories({ fresh: true });
    } catch (error) {
      setNewRepoHint(String(error?.message || "Could not delete selected imports."), "bad");
      await loadExternalRepositories({ fresh: true });
    }
  }

  async function loadExternalRepositories({ fresh = false } = {}) {
    const list = $("[data-external-repo-list]");
    if (!list) return;
    state.externalRepositoriesLoading = true;
    try {
      const [data, migrationData] = await Promise.all([
        fetchJson("/api/repository-imports", { fresh }),
        state.session?.sessionToken
          ? fetchJson("/api/repository-imports/migrations", { fresh }).catch(() => ({ migrations: [] }))
          : Promise.resolve({ migrations: [] }),
      ]);
      state.repositoryMigrations = Array.isArray(migrationData.migrations)
        ? migrationData.migrations : [];
      renderExternalRepositories(data.repositories);
    } catch (_) {
      state.externalRepositoriesLoading = false;
      const count = $("[data-external-repo-count]");
      if (count) count.textContent = "Unavailable";
      list.innerHTML = '<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">External repository metadata is temporarily unavailable.</div>';
    }
  }

  // The node a volunteer request is recorded against. Only a desktop node can
  // mirror: a node-kind session IS the node, and a user-kind session may
  // volunteer one of its linked nodes. A web-only user account never falls
  // back to its own name (issue #680): the relay used to accept that because an
  // account trivially "owns" itself, recording a request nothing could ever
  // serve while the card looked exactly the same afterwards.
  function externalMirrorNodeName() {
    const session = state.session || {};
    if (session.kind === "node") return String(session.nodeName || "").trim();
    const nodes = Array.isArray(session.nodes) ? session.nodes : [];
    const first = nodes.find((node) => String(node?.name || node || "").trim());
    return String(first?.name || first || "").trim();
  }

  function externalRepositoryById(repositoryId) {
    const id = String(repositoryId || "");
    return (state.externalRepositories || []).find(
      (entry) => String(entry?.id || "") === id) || null;
  }

  const EXTERNAL_MIRROR_BUTTON_LABELS = {
    busy: "Volunteering…",
    retry: "Could not volunteer — try again",
    signin: "Sign in again to volunteer",
    desktop: "Desktop node required",
  };

  function setExternalMirrorButtonLabel(button, label) {
    if (!button) return;
    const icon = button.querySelector("i");
    button.textContent = "";
    if (icon) button.appendChild(icon);
    button.appendChild(document.createTextNode(label));
  }

  async function volunteerForExternalMirror(repositoryId, button) {
    const repository = externalRepositoryById(repositoryId);
    if (!state.session?.sessionToken) {
      // The card only offers the button to signed-in sessions; a stale card
      // clicked after a sign-out still has to answer visibly.
      setExternalMirrorButtonLabel(button, EXTERNAL_MIRROR_BUTTON_LABELS.signin);
      return;
    }
    const node = externalMirrorNodeName();
    if (!node) {
      // Nothing to volunteer with yet: explain the desktop handoff instead of
      // silently doing nothing (issue #680).
      if (!openExternalMirrorHandoff(repository)) {
        setExternalMirrorButtonLabel(button, EXTERNAL_MIRROR_BUTTON_LABELS.desktop);
      }
      return;
    }
    if (button) {
      button.disabled = true;
      button.setAttribute("aria-busy", "true");
      setExternalMirrorButtonLabel(button, EXTERNAL_MIRROR_BUTTON_LABELS.busy);
    }
    try {
      const response = await fetch(
        `/api/repository-imports/${encodeURIComponent(repositoryId)}/mirror-volunteers`,
        {
          method: "POST",
          headers: { "content-type": "application/json", accept: "application/json" },
          body: JSON.stringify({ node, sessionToken: state.session.sessionToken }),
        },
      );
      const body = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
      // Remember the request before the reload so the re-rendered card confirms
      // it rather than offering the same button again.
      state.externalMirrorVolunteers.set(String(repositoryId), node);
      await loadExternalRepositories({ fresh: true });
    } catch (error) {
      const code = String(error?.message || "");
      if (code === "eligible_owned_mirror_node_required") {
        openExternalMirrorHandoff(repository, { node });
        setExternalMirrorButtonLabel(button, "Volunteer to mirror");
      } else if (code === "invalid_session") {
        setExternalMirrorButtonLabel(button, EXTERNAL_MIRROR_BUTTON_LABELS.signin);
      } else {
        setExternalMirrorButtonLabel(button, EXTERNAL_MIRROR_BUTTON_LABELS.retry);
      }
    } finally {
      if (button) {
        button.disabled = false;
        button.removeAttribute("aria-busy");
      }
    }
  }

  // The steps a person completes on a desktop node to actually mirror an
  // external entry. `node` names a linked node the relay just refused as an
  // eligible mirror; empty means the account has no linked node at all.
  function externalMirrorHandoffSteps(repository, node) {
    const name = String(repository?.name || "").trim() || "the repository";
    const original = String(repository?.originalUrl || "").trim();
    const nodeName = String(node || "").trim();
    const mono = (text) => `<span class="font-mono text-foreground">${escapeHtml(text)}</span>`;
    const ui = (text) => `<span class="text-foreground">${escapeHtml(text)}</span>`;
    return [
      nodeName
        ? `Make sure ${mono(nodeName)} is online and still linked to this account under <a href="/dashboard/settings/nodes" class="text-accent hover:underline">Settings › Nodes</a>.`
        : `Install the ${desktopBrand()}desktop app and link it to this account under <a href="/dashboard/settings/nodes" class="text-accent hover:underline">Settings › Nodes</a>.`,
      `Open the desktop node's ${ui("Repos")} page, click ${ui("+ Add")}, then choose ${ui("Remote clone URL")}.`,
      `${original ? `Paste ${mono(original)}` : "Paste the repository's clone URL"} and name it ${mono(name)}.`,
      `Publish — the node clones and hosts the mirror.`,
      `Back here, ${ui("Volunteer to mirror")} records that node as a volunteer so the importer can see a mirror is available.`,
    ];
  }

  function openExternalMirrorHandoff(repository, { node = "" } = {}) {
    const list = $("[data-new-repo-steps-list]");
    if (!$("[data-new-repo-modal]") || !list) return false;
    const name = String(repository?.name || "").trim();
    setNewRepoModalOpen(true);
    list.innerHTML = externalMirrorHandoffSteps(repository, node)
      .map((step, index) => `<li class="flex gap-2"><span class="shrink-0 font-mono text-foreground">${index + 1}.</span><span>${step}</span></li>`)
      .join("");
    setNewRepoHandoffOpen(true, {
      title: name ? `Mirror ${name} from your desktop node` : "Mirror this repository from your desktop node",
      subtitle: node
        ? `${node} is not an eligible mirror node for this account yet, so no request was recorded.`
        : "This account has no linked desktop node, so there is nothing to volunteer yet.",
      intro: `Mirroring runs on a ${desktopBrand()}desktop node; the browser only lists the repository. Nothing has been requested yet. Complete these steps:`,
    });
    window.setTimeout(() => $("[data-new-repo-close]")?.focus(), 0);
    window.lucide?.createIcons();
    return true;
  }

  // New-repository flow (adhoc #30). Publishing a signed catalog record and
  // running the git mirror both require the account's Ed25519 key, which lives
  // on the desktop node — the browser only holds a separate web-issue identity.
  // So this "Create & mirror" modal collects the repo details on the web, then
  // hands off the concrete steps to complete it in the desktop node's Repos
  // page, rather than pretending the pure-web path can publish.
  let newRepoSetupStartedAt = 0;

  // What this relay's own nodes can host (GET /api/hosted-repositories,
  // docs/HOSTED_REPOS.md): "core" = the Next core nodes keep a replica each,
  // so private repositories, a first commit, an import from a public forge and
  // an organization owner are offered, within the account's quota.
  let hostedSettings = null;

  async function loadHostedSettings() {
    try {
      const token = state.session?.sessionToken || "";
      const headers = { accept: "application/json" };
      if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
      const response = await fetch("/api/hosted-repositories", {
        headers, credentials: "same-origin", cache: "no-store",
      });
      hostedSettings = response.ok ? await response.json() : null;
    } catch {
      hostedSettings = null;
    }
    applyHostedSettings();
  }

  function hostedCoreMode() {
    return hostedSettings?.mode === "core";
  }

  function fillSelect(select, values, labels = {}) {
    if (!select || select.dataset.filled === "1") return;
    for (const value of values || []) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = labels[value] || value;
      select.append(option);
    }
    select.dataset.filled = "1";
  }

  function applyHostedSettings() {
    const core = hostedCoreMode();
    const owner = $("[data-new-repo-owner]");
    if (owner && core) {
      const me = state.session?.nodeName || "";
      const current = owner.value;
      owner.textContent = "";
      const mine = document.createElement("option");
      mine.value = "";
      mine.textContent = me ? `${me} (you)` : "You";
      owner.append(mine);
      for (const org of hostedSettings.organizations || []) {
        const option = document.createElement("option");
        option.value = org;
        option.textContent = `${org} (organization)`;
        owner.append(option);
      }
      owner.value = current || "";
    }
    fillSelect($("[data-new-repo-gitignore]"), hostedSettings?.gitignoreTemplates, {
      python: "Python", node: "Node", go: "Go", rust: "Rust", java: "Java", c: "C / C++",
    });
    fillSelect($("[data-new-repo-license]"), hostedSettings?.licenseTemplates, {
      mit: "MIT", "bsd-3-clause": "BSD 3-Clause", isc: "ISC", unlicense: "Unlicense",
    });
    const quota = $("[data-new-repo-quota]");
    if (quota) {
      const limits = hostedSettings?.limits || {};
      const perAccount = Number(limits.repositoriesPerAccount || 0);
      const megabytes = Math.round(Number(limits.repositoryBytes || 0) / 1048576);
      quota.textContent = core && perAccount
        ? `${platformName()} hosts up to ${perAccount} repositories per account (${Number(hostedSettings.used || 0)} used), ${megabytes} MB each, on every core node. Move one to your own node any time to grow past that.`
        : "";
    }
    setNewRepoSource(newRepoSource());
  }

  function hostedImportAllowed(url) {
    if (!hostedCoreMode()) return false;
    try {
      const parsed = new URL(url);
      return parsed.protocol === "https:"
        && (hostedSettings.importHosts || []).includes(parsed.hostname.toLowerCase());
    } catch {
      return false;
    }
  }

  // The extra create fields the core-hosted form collects.
  function hostedCreateExtras() {
    if (!hostedCoreMode()) return {};
    return {
      organization: $("[data-new-repo-owner]")?.value || "",
      readme: $("[data-new-repo-readme]")?.checked !== false,
      gitignore: $("[data-new-repo-gitignore]")?.value || "",
      license: $("[data-new-repo-license]")?.value || "",
    };
  }

  function prepareNewRepoForm() {
    newRepoSetupStartedAt = window.performance?.now?.() || Date.now();
    setNewRepoHandoffOpen(false);
    setNewRepoPushOpen(false);
    // The markup ships with one source pre-pressed; apply that source's
    // field visibility, hint, and submit label before the form is seen,
    // instead of waiting for the first click on a tab.
    setNewRepoSource(requestedNewRepoSource() || newRepoSource());
    setNewRepoHint("");
    loadHostedSettings();
    $(`[data-new-repo-migration-status]`)?.classList.add("hidden");
    window.setTimeout(() => $("[data-new-repo-name]")?.focus(), 0);
    window.lucide?.createIcons();
  }

  function setNewRepoModalOpen(open) {
    const modal = $("[data-new-repo-modal]");
    if (!modal) return;
    modal.classList.toggle("hidden", !open);
    modal.classList.toggle("flex", open);
    if (open) {
      prepareNewRepoForm();
    } else {
      const token = $("[data-new-repo-provider-token]");
      if (token) token.value = "";
      // The push credential is shown once and never stored; closing the modal
      // takes it out of the document rather than leaving it in the DOM.
      const password = $("[data-new-repo-push-password]");
      if (password) password.textContent = "";
      const commands = $("[data-new-repo-push-commands]");
      if (commands) commands.textContent = "";
    }
  }

  // The dashboard home rail's "New" is a real link into this page rather than a
  // second copy of the modal (the markup ships only on the Repositories
  // document), so it arrives as /dashboard/repos?new=1. Consume that marker:
  // open the flow once, then strip it from the address bar so a refresh or a
  // Back step doesn't reopen a modal nobody asked for again (issue #626).
  function consumeNewRepositoryRequest() {
    const params = new URLSearchParams(location.search);
    if (params.get("new") !== "1") return;
    location.replace("/dashboard/repos/new");
  }

  const NEW_REPO_STEPS_INTRO =
    `The browser cannot create or queue this mirror. Nothing has been published yet. Complete these steps in your ${desktopBrand()}desktop node:`;

  const NEW_REPO_SUBTITLE =
    `Mirror an existing repository. Your ${desktopBrand()}desktop node finishes creating and signing it.`;
  // The provider tab imports provider records only. Say so before the import
  // starts, so "complete" is never read as "mirrored" afterwards (issue #678).
  const NEW_REPO_PROVIDER_SUBTITLE =
    `Import signed provider metadata and collaboration history. Mirroring the Git contents is a separate step on a ${desktopBrand()}desktop node.`;
  // The empty source needs neither a node nor another provider, so it must
  // not repeat the desktop-handoff sentence (issue #676).
  const NEW_REPO_EMPTY_SUBTITLE =
    `A repository holds your project's files and history. ${platformName()} keeps a copy on every core node.`;

  function newRepoSubtitleFor(source) {
    if (source === "provider") return NEW_REPO_PROVIDER_SUBTITLE;
    if (source === "empty") return NEW_REPO_EMPTY_SUBTITLE;
    if (source === "remote" && hostedCoreMode()) {
      return `Paste a public repository URL: ${platformName()} copies it and hosts it on its own nodes, no desktop app needed.`;
    }
    return NEW_REPO_SUBTITLE;
  }

  // `copy` lets another flow (the external-mirror handoff, issue #680) reuse
  // the steps panel with its own title, subtitle, and intro sentence.
  function setNewRepoHandoffOpen(open, copy = {}) {
    $("[data-new-repo-details]")?.classList.toggle("hidden", open);
    $("[data-new-repo-steps]")?.classList.toggle("hidden", !open);
    $("[data-new-repo-submit]")?.classList.toggle("hidden", open);
    const title = $("[data-new-repo-title]");
    const subtitle = $("[data-new-repo-subtitle]");
    const intro = $("[data-new-repo-steps-intro]");
    const cancelLabel = $("[data-new-repo-cancel-label]");
    if (title) title.textContent = open ? (copy.title || "Finish creating your repository") : "Create a new repository";
    if (subtitle) {
      subtitle.textContent = open
        ? (copy.subtitle || `${platformName()} needs your desktop node to publish and mirror this repository.`)
        : newRepoSubtitleFor(newRepoSource());
    }
    if (intro) intro.textContent = (open && copy.intro) || NEW_REPO_STEPS_INTRO;
    if (cancelLabel) cancelLabel.textContent = open ? "Close" : "Cancel";
    const form = $("[data-new-repo-form]");
    if (open && form) form.scrollTop = 0;
  }

  // A Windows user reading /home/you/code/my-project has to translate the
  // example before they can use it (issue #676, UX note). Ask the platform.
  function localRepositoryPlaceholder() {
    const platform = String(
      navigator.userAgentData?.platform || navigator.platform || "");
    return /win/i.test(platform)
      ? "C:\\Users\\you\\code\\my-project"
      : "/home/you/code/my-project";
  }

  function newRepoVisibility() {
    const checked = $("[data-new-repo-visibility]:checked");
    const value = checked ? checked.value : $("[data-new-repo-visibility]")?.value;
    return value === "private" ? "private" : "public";
  }

  function setNewRepoVisibility(value) {
    $$("[data-new-repo-visibility]").forEach((input) => {
      if (input.type === "radio") input.checked = input.value === value;
      else input.value = value;
    });
  }

  function newRepoSource() {
    return $('[data-new-repo-source][aria-pressed="true"]')?.dataset.newRepoSource || "remote";
  }

  function requestedNewRepoSource() {
    const requested = new URLSearchParams(location.search).get("source") || "";
    return $$("[data-new-repo-source]").some((btn) => btn.dataset.newRepoSource === requested)
      ? requested : "";
  }

  function setNewRepoSource(source) {
    $$("[data-new-repo-source]").forEach((btn) => {
      const active = btn.dataset.newRepoSource === source;
      btn.setAttribute("aria-pressed", active ? "true" : "false");
      btn.classList.toggle("bg-secondary", active);
      btn.classList.toggle("text-foreground", active);
      btn.classList.toggle("text-muted-foreground", !active);
    });
    const value = $("[data-new-repo-source-value]");
    const hint = $("[data-new-repo-source-hint]");
    const provider = source === "provider";
    $("[data-new-repo-provider-options]")?.classList.toggle("hidden", !provider);
    $("[data-new-repo-name-wrap]")?.classList.toggle("hidden", provider);
    $("[data-new-repo-visibility-wrap]")?.classList.toggle("hidden", provider);
    $("[data-new-repo-description-wrap]")?.classList.toggle("hidden", provider);
    const name = $("[data-new-repo-name]");
    if (name) name.required = !provider;
    const submitLabel = $("[data-new-repo-submit-label]");
    if (submitLabel) submitLabel.textContent = provider ? "Start resumable import" : "Create & mirror";
    const subtitle = $("[data-new-repo-subtitle]");
    if (subtitle) subtitle.textContent = newRepoSubtitleFor(source);
    // Switching tabs starts a different flow, so a previous run's steps panel
    // must not linger under the new form.
    $("[data-new-repo-steps]")?.classList.add("hidden");
    if (!provider) {
      const token = $("[data-new-repo-provider-token]");
      if (token) token.value = "";
    }
    // An empty repository is created here and then pushed to, so there is no
    // source to point at: the input is hidden rather than left asking for a
    // path or URL the flow never reads (issue #676).
    const empty = source === "empty";
    if (value) value.classList.toggle("hidden", empty);
    if (submitLabel && empty) submitLabel.textContent = "Create empty repository";
    // A single hosted write node cannot hold a private repository (only a node
    // holding the account key can seal one). The Next core nodes can: they
    // serve it only to the owner and the organization's readers.
    const core = hostedCoreMode();
    const hostedHere = empty || (source === "remote" && core);
    const privateOption = $('[data-new-repo-visibility][value="private"]')
      || $("[data-new-repo-visibility]")?.querySelector('option[value="private"]');
    const privateBlocked = hostedHere && !hostedSettings?.privateSupported;
    if (privateOption) {
      privateOption.disabled = privateBlocked;
      const privateText = core && hostedHere
        ? "Only you and your organization's readers can see and clone it."
        : "Encrypted mirror, contents stay hidden.";
      const privateDesc = $("[data-new-repo-private-desc]");
      if (privateDesc) privateDesc.textContent = privateText;
      else privateOption.textContent = "Private — " + privateText.replace(/\.$/, "");
    }
    if (privateBlocked && newRepoVisibility() === "private") setNewRepoVisibility("public");
    $$("[data-new-repo-hosted-options]").forEach((element) =>
      element.classList.toggle("hidden", !(core && hostedHere)));
    $("[data-new-repo-name-row]")?.classList.toggle("nr-has-owner", core && hostedHere);
    // An import brings its own history; the first-commit fields do not apply.
    const importing = source === "remote" && core;
    $("[data-new-repo-readme-wrap]")?.classList.toggle("hidden", importing);
    $("[data-new-repo-init-wrap]")?.classList.toggle("hidden", importing);
    if (submitLabel && empty && core) submitLabel.textContent = "Create repository";
    if (submitLabel && importing) submitLabel.textContent = "Import & host";
    if (empty) {
      if (hint) {
        hint.textContent = core
          ? "Start empty and push with git over HTTPS."
          : "Start empty and push with git. It is public and stays empty until your first push.";
      }
    } else if (importing) {
      if (value) value.placeholder = "https://github.com/owner/repo.git";
      if (hint) hint.textContent = `${platformName()} copies a public repository from ${(hostedSettings.importHosts || []).join(", ")} and hosts it on its core nodes. Other URLs need a ${desktopBrand()}desktop node.`;
    } else if (source === "local") {
      if (value) value.placeholder = localRepositoryPlaceholder();
      if (hint) hint.textContent = "The desktop node reads this local repo directly — the path never leaves your machine.";
    } else if (provider) {
      if (value) value.placeholder = "https://gitlab.com/group/repo, https://gitlab.com/group, or https://github.com/owner/repo";
      if (hint) hint.textContent = `Choose a repository URL, or a GitLab group / Codeberg profile to choose all repositories below it. This imports provider metadata and collaboration history, not the Git contents: the result is an external listing, not a ${platformName()} mirror. Setup ends as soon as each durable job is accepted; transfer continues separately and can resume from its checkpoint.`;
    } else {
      if (value) value.placeholder = "https://github.com/owner/repo.git";
      if (hint) hint.textContent = `${platformName()} clones this URL into a bare mirror you then keep in sync.`;
    }
  }

  function setNewRepoHint(text, cls) {
    const hint = $("[data-new-repo-hint]");
    if (!hint) return;
    hint.textContent = text || "";
    hint.className = "min-h-4 text-xs " + (cls === "bad" ? "text-destructive" : "text-muted-foreground");
  }

  function renderNewRepoSteps(details) {
    const list = $("[data-new-repo-steps-list]");
    const panel = $("[data-new-repo-steps]");
    if (!list || !panel) return;
    const sourceLabel = details.source === "local" ? "Local repository" : "Remote clone URL";
    const pick = details.source === "local" ? "Select the local repository" : "Paste the clone URL";
    const sourceValue = details.sourceValue
      ? ` (<span class="font-mono text-foreground">${escapeHtml(details.sourceValue)}</span>)` : "";
    const steps = [
      `Open the ${desktopBrand()}desktop node and go to the <span class="text-foreground">Repos</span> page.`,
      `Click <span class="text-foreground">+ Add</span>, then choose <span class="text-foreground">${sourceLabel}</span>.`,
      `${pick}${sourceValue} and name it <span class="font-mono text-foreground">${escapeHtml(details.name)}</span>.`,
      `Set visibility to <span class="text-foreground">${details.visibility === "private" ? "Private" : "Public"}</span>${details.description ? ` and add your description` : ""}.`,
      `Publish — the node mirrors it and it appears here in your repositories.`,
    ];
    list.innerHTML = steps
      .map((step, index) => `<li class="flex gap-2"><span class="shrink-0 font-mono text-foreground">${index + 1}.</span><span>${step}</span></li>`)
      .join("");
    panel.classList.remove("hidden");
    window.lucide?.createIcons();
  }

  // A signed import archives provider records; it never clones Git contents,
  // so the repository is still only an external listing when the receipt
  // appears. Close the flow on the steps that do produce a mirror rather than
  // letting "complete" imply one (issue #678). The details block stays
  // visible so the signed receipt sits above these steps.
  function showNewRepoMirrorSteps(sourceUrls) {
    const list = $("[data-new-repo-steps-list]");
    const panel = $("[data-new-repo-steps]");
    if (!list || !panel) return;
    const urls = Array.isArray(sourceUrls) ? sourceUrls : [];
    const single = urls.length === 1 ? String(urls[0] || "").trim() : "";
    const name = single
      ? single.replace(/\.git$/, "").split("/").filter(Boolean).pop() || ""
      : "";
    list.innerHTML = externalMirrorHandoffSteps({ name, originalUrl: single }, "")
      .map((step, index) => `<li class="flex gap-2"><span class="shrink-0 font-mono text-foreground">${index + 1}.</span><span>${step}</span></li>`)
      .join("");
    const intro = $("[data-new-repo-steps-intro]");
    if (intro) {
      intro.textContent = urls.length === 1
        ? `Provider metadata and collaboration history are imported and signed. The Git contents are not mirrored, so this repository is listed as external until a ${desktopBrand()}desktop node hosts it:`
        : `Provider metadata and collaboration history are imported and signed. The Git contents are not mirrored, so these repositories stay external until a ${desktopBrand()}desktop node hosts each one:`;
    }
    panel.classList.remove("hidden");
    window.lucide?.createIcons();
  }

  // A bare organization link — a GitLab group or a Codeberg profile — imports
  // every repository underneath it. GitHub is absent because the relay has no
  // organization walk for it, and a GitLab subgroup path is not matched because
  // group/thing is indistinguishable from a repository URL; the group walk
  // covers subgroups anyway.
  function providerNamespaceUrl(value) {
    let parsed;
    try {
      parsed = new URL(value.includes("://") ? value : `https://${value}`);
    } catch (_) {
      return "";
    }
    const namespaceHosts = ["gitlab.com", "www.gitlab.com", "codeberg.org", "www.codeberg.org"];
    if (parsed.protocol !== "https:" || !namespaceHosts.includes(parsed.hostname.toLowerCase())) return "";
    return parsed.pathname.split("/").filter(Boolean).length === 1 ? parsed.toString() : "";
  }

  async function discoverNamespaceRepositories(sourceUrl, providerToken) {
    const response = await fetch("/api/repository-imports/discover", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ sourceUrl, providerToken, sessionToken: state.session.sessionToken }),
    });
    const body = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
    return {
      repositories: Array.isArray(body.repositories) ? body.repositories : [],
      incomplete: Boolean(body.incomplete),
    };
  }

  function migrationDuration(milliseconds) {
    const seconds = Math.max(0, Math.round(Number(milliseconds || 0) / 1000));
    if (seconds < 60) return `${seconds}s`;
    const minutes = Math.floor(seconds / 60);
    return `${minutes}m ${seconds % 60}s`;
  }

  function renderNewRepoMigrationStatus(migrationJob) {
    const panel = $(`[data-new-repo-migration-status]`);
    if (!panel || !migrationJob) return;
    panel.classList.remove("hidden");
    const setup = $(`[data-new-repo-setup-time]`);
    const transfer = $(`[data-new-repo-transfer-time]`);
    const phase = $(`[data-new-repo-migration-phase]`);
    if (setup) setup.textContent = `Hands-on setup ${migrationDuration(migrationJob.setup?.durationMs)} (transfer excluded)`;
    const transferMs = migrationJob.transfer?.durationMs || (
      migrationJob.transfer?.startedAt ? Date.now() - migrationJob.transfer.startedAt : 0);
    if (transfer) transfer.textContent = migrationJob.status === "completed"
      ? `Transfer ${migrationDuration(transferMs)}`
      : `Transfer running ${migrationDuration(transferMs)}`;
    if (phase) {
      const counts = Object.values(migrationJob.counts || {}).reduce((sum, value) => sum + Number(value || 0), 0);
      phase.textContent = migrationJob.status === "completed"
        ? `${counts} provider objects verified · signed state ${String(migrationJob.state?.hash || "").slice(0, 22)}… · metadata only, no Git contents mirrored`
        : `${String(migrationJob.phase || "starting").replaceAll("_", " ")} · ${counts} objects checkpointed · safe to close this page`;
    }
  }

  async function advanceRepositoryMigration(migrationJob, providerToken, onProgress = () => {}) {
    let current = migrationJob;
    while (current?.status !== "completed") {
      const response = await fetch(
        `/api/repository-imports/migrations/${encodeURIComponent(current.id)}/resume`,
        {
          method: "POST",
          headers: { "content-type": "application/json", accept: "application/json" },
          body: JSON.stringify({
            providerToken,
            recover: current.status === "abandoned",
            sessionToken: state.session.sessionToken,
          }),
        },
      );
      const body = await response.json().catch(() => ({}));
      if (body.migration) {
        current = body.migration;
        onProgress(current);
      }
      if (!response.ok) {
        const error = new Error(body.error || `HTTP ${response.status}`);
        error.migration = current;
        throw error;
      }
      if (!body.hasMore) break;
    }
    return current;
  }

  async function continueRepositoryMigration(migrationJob, button) {
    if (!migrationJob?.id || !state.session?.sessionToken) return;
    let providerToken = "";
    if (migrationJob.isPrivate) {
      providerToken = window.prompt("Enter a current provider token to continue this private import. It will not be saved.") || "";
      if (!providerToken) return;
    }
    if (button) button.disabled = true;
    try {
      const completed = await advanceRepositoryMigration(
        migrationJob,
        providerToken,
        (current) => {
          if (button) button.textContent = `${String(current.phase || "transfer").replaceAll("_", " ")} · ${current.processed || 0}`;
        },
      );
      if (button) button.textContent = completed.status === "completed" ? "Import verified" : "Continue import";
      await loadExternalRepositories({ fresh: true });
    } catch (error) {
      const code = String(error?.message || "");
      if (button) button.textContent = code === "provider_rate_limited" ? "Rate-limited · resume later" : "Continue import";
    } finally {
      providerToken = "";
      if (button) button.disabled = false;
    }
  }

  // The report export is the machine-readable JSON that
  // tools/verify_migration_report.py checks; saving it is the download.
  async function downloadRepositoryMigrationReport(migrationJob, button) {
    if (!migrationJob?.id) return;
    if (button) button.disabled = true;
    try {
      const body = await fetchJson(
        `/api/repository-imports/migrations/${encodeURIComponent(migrationJob.id)}/report`,
        { fresh: true },
      );
      const url = URL.createObjectURL(new Blob(
        [`${JSON.stringify(body, null, 2)}\n`], { type: "application/json" }));
      const link = document.createElement("a");
      link.href = url;
      link.download = `forkmesh-migration-report-${migrationJob.id}-${body?.draft ? "draft" : body?.report?.sequence || "latest"}.json`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 0);
      if (button) button.textContent = body?.draft ? "Draft report saved" : "Signed report saved";
    } catch (error) {
      if (button) button.textContent = "Report unavailable";
    } finally {
      if (button) button.disabled = false;
    }
  }

  // Retry queues only the failed pages; the resume loop then works through
  // them and the finished run signs a report that supersedes the last one.
  async function retryRepositoryMigration(migrationJob, button) {
    if (!migrationJob?.id || !state.session?.sessionToken) return;
    if (button) button.disabled = true;
    let started = null;
    try {
      const response = await fetch(
        `/api/repository-imports/migrations/${encodeURIComponent(migrationJob.id)}/retry`,
        {
          method: "POST",
          headers: { "content-type": "application/json", accept: "application/json" },
          body: JSON.stringify({ sessionToken: state.session.sessionToken }),
        },
      );
      const body = await response.json().catch(() => ({}));
      if (!response.ok || !body.migration) {
        if (button) button.textContent = body.error === "no_failed_work" ? "Nothing to retry" : "Retry unavailable";
        return;
      }
      started = body.migration;
    } finally {
      if (button) button.disabled = false;
    }
    await continueRepositoryMigration(started, button);
  }

  // A repository ForkMesh itself hosts (issue #676). The browser holds no
  // signing key — keys arrive with a desktop or mobile node — so the relay
  // authors this record from the session and answers with the credential a
  // stock `git push` needs for the first push.
  function setNewRepoPushOpen(open) {
    $("[data-new-repo-details]")?.classList.toggle("hidden", open);
    $("[data-new-repo-push]")?.classList.toggle("hidden", !open);
    $("[data-new-repo-submit]")?.classList.toggle("hidden", open);
    const cancelLabel = $("[data-new-repo-cancel-label]");
    if (cancelLabel && open) cancelLabel.textContent = "Close";
    const form = $("[data-new-repo-form]");
    if (open && form) form.scrollTop = 0;
  }

  function renderNewRepoPush(repository, push, hosting = null) {
    const repoLabel = $("[data-new-repo-push-repo]");
    if (repoLabel) repoLabel.textContent = repoPublicKey(repoKey(repository || {}));
    const intro = $("[data-new-repo-push-intro]");
    if (intro) {
      intro.textContent = hosting?.importing
        ? `on ${hosting.nodes?.length || 3} core nodes and is importing it now. Clone it with the commands below once the import finishes; git asks for the credential below.`
        : hosting?.initialized
          ? `on ${hosting.nodes?.length || 3} core nodes, with a first commit. Clone it with the commands below; git asks for the credential below.`
          : hosting?.mode === "core"
            ? `on ${hosting.nodes?.length || 3} core nodes and it stays empty until your first push. Run these in your project folder; git asks for the credential below.`
            : "and it stays empty until your first push. Run these in your project folder; git asks for the credential below.";
    }
    const terms = $("[data-new-repo-push-visibility]");
    if (terms) {
      terms.textContent = repository?.visibility === "private"
        ? "Your repository is private: only you and your organization's readers can clone it, with a credential like this one. The core nodes hold it unencrypted, like any hosted forge."
        : hosting?.mode === "core"
          ? "Your repository is public: anyone can browse and clone it."
          : "Your repository is public; a private mirror is an owner-sealed archive only a ForkMesh node holding your key can publish.";
    }
    const commands = $("[data-new-repo-push-commands]");
    if (commands) commands.textContent = (push?.commands || []).join("\n");
    const username = $("[data-new-repo-push-username]");
    if (username) username.textContent = push?.username || "";
    const password = $("[data-new-repo-push-password]");
    if (password) password.textContent = push?.password || "";
    const expiry = $("[data-new-repo-push-expiry]");
    if (expiry) {
      expiry.textContent = push?.expiresAt
        ? new Date(Number(push.expiresAt)).toLocaleString() : "soon";
    }
    const link = $("[data-new-repo-push-open]");
    if (link && repository) link.href = repoLinkUrl(repository);
    const copyLabel = $("[data-new-repo-push-copy-label]");
    if (copyLabel) copyLabel.textContent = "Copy commands";
    setNewRepoPushOpen(true);
    window.lucide?.createIcons();
  }

  function newRepoHostedErrorText(code) {
    if (code === "repository_exists") return "You already have a repository with that name.";
    if (code === "too_many_repos") return "This account has reached its repository limit.";
    if (code === "invalid_name") return "Use letters, numbers, dots, dashes, or underscores in the name.";
    if (code === "private_hosted_unsupported") {
      return "A private repository is an owner-sealed encrypted mirror, which only a ForkMesh node holding your key can publish. Create it as public instead.";
    }
    if (code === "forbidden") return "That namespace belongs to another account.";
    if (code === "unauthorized" || code === "session_required") {
      return "Sign in again in this browser to create a repository.";
    }
    if (code === "email_not_verified") return "Confirm your email address to create a repository.";
    if (code === "blocked") return "That repository name is not available.";
    if (code === "hosted_quota_exceeded") {
      return `This account hosts as many repositories on ${platformName()} as it may. Delete one, or host more from your own node.`;
    }
    if (code === "invalid_import_url") {
      return `Use a public https URL on ${(hostedSettings?.importHosts || ["github.com", "gitlab.com", "codeberg.org"]).join(", ")}.`;
    }
    if (code === "organization_not_found") return "That organization does not exist.";
    return "Could not create the repository.";
  }

  async function createHostedRepository(details) {
    if (!state.session?.sessionToken) {
      setNewRepoHint(`Sign in to create a ${platformName()}-hosted repository.`, "bad");
      return;
    }
    const submit = $("[data-new-repo-submit]");
    if (submit) submit.disabled = true;
    setNewRepoHint("Creating the repository…");
    try {
      const response = await fetch("/api/hosted-repositories", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ ...details, sessionToken: state.session.sessionToken }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
      setNewRepoHint("");
      forgetDeletedRepository(body.repository);
      renderNewRepoPush(body.repository, body.push, body.hosting);
      await loadRepositories({ fresh: true });
    } catch (error) {
      const code = String(error?.message || "");
      if (code === "hosted_git_unavailable") {
        // This relay has no writable git node, so nothing here could accept
        // the push. Fall back to the handoff that does work instead of
        // leaving the user at a dead end.
        renderNewRepoSteps({ ...details, source: "local", sourceValue: "" });
        setNewRepoHandoffOpen(true, {
          title: "Finish creating your repository",
          subtitle: "This relay hosts no ForkMesh git node, so the repository has to come from a node you run.",
        });
        return;
      }
      setNewRepoHint(newRepoHostedErrorText(code), "bad");
    } finally {
      if (submit) submit.disabled = false;
    }
  }

  async function handleNewRepoSubmit() {
    const source = newRepoSource();
    const sourceValue = String($("[data-new-repo-source-value]")?.value || "").trim();
    if (source === "provider") {
      if (!state.session?.sessionToken) {
        setNewRepoHint("Sign in to import a GitHub, GitLab, or Codeberg repository.", "bad");
        return;
      }
      if (!sourceValue) {
        setNewRepoHint("Enter a GitHub, GitLab, or Codeberg repository URL.", "bad");
        $("[data-new-repo-source-value]")?.focus();
        return;
      }
      const tokenInput = $("[data-new-repo-provider-token]");
      const providerToken = String(tokenInput?.value || "").trim();
      const mode = $("[data-new-repo-import-mode]")?.value === "stub" ? "stub" : "import";
      const submit = $("[data-new-repo-submit]");
      if (submit) submit.disabled = true;
      const namespaceUrl = providerNamespaceUrl(sourceValue);
      setNewRepoHint(namespaceUrl ? "Listing the organization…" : "Reading provider metadata…");
      try {
        let sourceUrls = [sourceValue];
        let incomplete = false;
        if (namespaceUrl) {
          const discovery = await discoverNamespaceRepositories(namespaceUrl, providerToken);
          sourceUrls = discovery.repositories;
          incomplete = discovery.incomplete;
          if (!sourceUrls.length) throw new Error("empty_namespace");
        }
        // Accept every durable job before transferring any pages. That keeps
        // hands-on setup independent of repository size and means closing the
        // page cannot strand later repositories in a large organization.
        let imported = 0;
        let failed = 0;
        let lastLabel = "";
        const jobs = [];
        for (const [index, sourceUrl] of sourceUrls.entries()) {
          if (namespaceUrl) {
            setNewRepoHint(`Starting ${index + 1} of ${sourceUrls.length} — ${sourceUrl}…`);
          }
          const setupDurationMs = Math.max(0, Math.round(
            (window.performance?.now?.() || Date.now()) - newRepoSetupStartedAt));
          const requestOptions = {
            method: "POST",
            headers: { "content-type": "application/json", accept: "application/json" },
            body: JSON.stringify({
              sourceUrl,
              providerToken,
              mode,
              setupDurationMs,
              sessionToken: state.session.sessionToken,
            }),
          };
          const response = mode === "import"
            ? await fetch("/api/repository-imports/migrations", requestOptions)
            : await fetch("/api/repository-imports", requestOptions);
          const body = await response.json().catch(() => ({}));
          if (!response.ok) {
            // A lone URL keeps the precise provider error; inside an
            // organization walk one refusal must not discard the rest.
            if (!namespaceUrl) throw new Error(body.error || `HTTP ${response.status}`);
            failed += 1;
            continue;
          }
          imported += 1;
          lastLabel = body.repository?.statusLabel || lastLabel;
          if (body.migration) jobs.push(body.migration);
        }
        if (!imported) throw new Error("namespace_import_failed");
        let completed = 0;
        let paused = 0;
        for (const [index, job] of jobs.entries()) {
          setNewRepoHint(`Transfer ${index + 1} of ${jobs.length} — ${job.sourceUrl}…`);
          renderNewRepoMigrationStatus(job);
          try {
            const result = await advanceRepositoryMigration(
              job, providerToken, renderNewRepoMigrationStatus);
            if (result?.status === "completed") completed += 1;
          } catch (error) {
            paused += 1;
            renderNewRepoMigrationStatus(error?.migration || job);
          }
        }
        setNewRepoHint(
          jobs.length
            ? `Started ${jobs.length} resumable ${jobs.length === 1 ? "import" : "imports"}; ${completed} reached signed metadata completion${paused ? ` and ${paused} paused safely at a checkpoint` : ""}. No Git contents were mirrored: ${jobs.length === 1 ? "the entry stays" : "the entries stay"} under External repositories until a ForkMesh node hosts ${jobs.length === 1 ? "it" : "them"}. Setup time excludes transfer time.`
            : namespaceUrl
              ? `Imported ${imported} of ${sourceUrls.length} repositories${failed ? ` (${failed} could not be read)` : ""}.`
              + `${incomplete ? " The organization holds more repositories than one listing returns, so the rest were not reached; import them by their own links or subgroup links." : ""}`
              + " They remain separate from live mirrors."
              : `${lastLabel || "External repository"} created. It remains separate from live mirrors.`,
          "good",
        );
        // Every provider path above lands on an external listing, never a
        // mirror, so end on the steps that actually produce one (issue #678).
        showNewRepoMirrorSteps(sourceUrls);
        await loadExternalRepositories({ fresh: true });
      } catch (error) {
        const code = String(error?.message || "");
        setNewRepoHint(
          code === "provider_rate_limited" ? "The provider rate limit was reached. Try again after its reset time."
            : code.includes("authorization") ? "The provider rejected access. Private repositories require a valid scoped token."
            : code === "unsupported_provider" ? "Use a github.com, gitlab.com, or codeberg.org repository URL."
            : code === "empty_namespace" ? "That organization listed no importable repositories. A private group needs a token with read access."
            : code === "namespace_import_failed" ? "None of that organization's repositories could be read."
            : code === "provider_request_failed" ? "The provider did not recognise that organization. Check the group path, or add a token if it is private."
            : "Could not import provider metadata.",
          "bad",
        );
      } finally {
        // The credential lives only in this function while it advances pages.
        // Remove the form copy after completion, pause, or provider failure.
        if (tokenInput) tokenInput.value = "";
        if (submit) submit.disabled = false;
      }
      return;
    }
    const name = String($("[data-new-repo-name]")?.value || "").trim();
    const visibility = newRepoVisibility();
    const description = String($("[data-new-repo-description]")?.value || "").trim();
    if (!name) {
      setNewRepoHint("Enter a repository name.", "bad");
      $("[data-new-repo-name]")?.focus();
      return;
    }
    if (!/^[A-Za-z0-9._-]+$/.test(name)) {
      setNewRepoHint("Use letters, numbers, dots, dashes, or underscores in the name.", "bad");
      $("[data-new-repo-name]")?.focus();
      return;
    }
    if (source === "empty") {
      await createHostedRepository({ name, visibility, description, ...hostedCreateExtras() });
      return;
    }
    if (source === "remote" && hostedImportAllowed(sourceValue)) {
      const { organization } = hostedCreateExtras();
      await createHostedRepository({ name, visibility, description, organization, importUrl: sourceValue });
      return;
    }
    if (!sourceValue) {
      setNewRepoHint(source === "local" ? "Enter the local repository path." : "Enter a clone URL.", "bad");
      $("[data-new-repo-source-value]")?.focus();
      return;
    }
    renderNewRepoSteps({ name, source, sourceValue, visibility, description });
    setNewRepoHandoffOpen(true);
    window.setTimeout(() => $("[data-new-repo-cancel]")?.focus(), 0);
  }

  function keepRepoActiveTabVisible(strip) {
    if (!strip || strip.scrollWidth <= strip.clientWidth + 1) return;
    const active = strip.querySelector('[role="tab"][aria-selected="true"]');
    if (!active || active.classList.contains("hidden")) return;
    const frame = strip.getBoundingClientRect();
    const box = active.getBoundingClientRect();
    const inset = 12;
    if (box.left < frame.left + inset) {
      strip.scrollLeft -= Math.ceil(frame.left + inset - box.left);
    } else if (box.right > frame.right - inset) {
      strip.scrollLeft += Math.ceil(box.right - (frame.right - inset));
    }
  }

  function setRepoTab(tab) {
    const detail = $(`[data-dashboard-repo-tab-panel="${tab}"]`)?.closest("[data-repo-detail]") || $("[data-repo-detail]");
    if (!detail) return;
    const tabChanged = state.activeRepoTab !== tab;
    state.activeRepoTab = tab;
    // The selected tab's weight and underline are styled from aria-selected
    // (shell.html), so this only has to move the attribute.
    detail.querySelectorAll("[data-dashboard-repo-tab]").forEach((button) => {
      const active = button.dataset.dashboardRepoTab === tab;
      button.setAttribute("aria-selected", active ? "true" : "false");
    });
    if (tabChanged) keepRepoActiveTabVisible(detail.querySelector("[data-repo-tabs]"));
    detail.querySelectorAll("[data-dashboard-repo-tab-panel]").forEach((panel) => {
      panel.classList.toggle("hidden", panel.dataset.dashboardRepoTabPanel !== tab);
    });
    // The About right-hand rail only belongs next to the file tree/README
    // (owner decision 2026-07-12, discussion #2): every other tab — commits,
    // releases, issues, projects, pulls, discussions, insights, mirrors,
    // agents, settings — goes full-width instead of leaving a rail with nothing beside
    // it to explain. The explorer focus mode independently hides the rail
    // (and collapses this same grid) while active on the code tab.
    const contentGrid = detail.querySelector("[data-repo-content-grid]");
    const about = detail.querySelector("[data-repo-about]");
    const showAbout = tab === "code";
    contentGrid?.classList.toggle("lg:grid-cols-[minmax(0,1fr)_18rem]", showAbout);
    contentGrid?.classList.toggle("lg:grid-cols-1", !showAbout);
    about?.classList.toggle("hidden", !showAbout);
    if (!showAbout) detail.querySelector("[data-repo-dir-about]")?.classList.add("hidden");
    // Mirrors is the one tab whose content is genuinely wider than the 80rem
    // reading column: twenty-odd node-health columns. Capping it there pushed
    // most of them behind a horizontal scrollbar on a monitor with the room to
    // show them, so this tab drops the cap and uses the whole viewport.
    const fullBleed = tab === "mirrors";
    contentGrid?.classList.toggle("max-w-7xl", !fullBleed);
    contentGrid?.classList.toggle("max-w-none", fullBleed);
  }

  // Tab switch requested by the user (or a Back/Forward step): shows the tab,
  // mirrors it into the address bar so refresh/back land on the same page,
  // and fetches its records on first view. Issues, pull requests and
  // discussions load lazily here rather than on repo open so a repo with many
  // records doesn't fire record reads for tabs nobody opened. Pulls are the
  // exception after that first load: their dedicated metadata branch can move
  // without the repository's selected code branch moving, so revisiting the
  // list must check for a newer ledger tip.
  function activateRepoTab(tab) {
    setRepoTab(tab);
    // Pull activity only polls while the pull detail is actually visible.
    // Leaving the tab must cancel immediately instead of waiting for the next
    // timer tick to notice the now-hidden record.
    if (tab !== "pulls") stopRepoPullActivityRefresh();
    // The Actions poll follows a mirror executor and only while it is shown.
    if (tab !== "actions") stopRepoActionsRefresh();
    // The Agents auto-refresh poll only makes sense while that tab is the one
    // on screen; leaving it (to any other tab) stops the poll.
    if (tab !== "agents") {
      // Leaving the tab closes any open agent detail page (adhoc #259) so
      // returning to Agents lands on the session list, not a stale transcript.
      state.agentsView.selectedAgentId = null;
    }
    if (!state.selectedRepo) return;
    // Record pages have no Back button: the tab is the way back to the list,
    // as on GitHub, so selecting it leaves the open record behind.
    if (["issues", "pulls", "discussions"].includes(tab) && state.repoRecordDetail?.kind === tab) {
      if (tab === "pulls") stopRepoPullActivityRefresh();
      state.repoRecordDetail = null;
      state.repoRecordSection = "conversation";
    }
    navigateHistory(tab === "code"
      ? (state.repoCodeUrl || repoLinkUrl(state.selectedRepo))
      : `${repoLinkUrl(state.selectedRepo)}/${tab}`);
    if (tab === "code") {
      if (!state.loadedRepoTabs) state.loadedRepoTabs = {};
      if (!state.loadedRepoTabs.code) {
        state.loadedRepoTabs.code = true;
        loadRepositoryTree(state.selectedRepo, "");
      }
      scheduleRepoAboutRail(state.selectedRepo);
    }
    // Mirror health loads once with the repository summary, then refreshes
    // only when its own tab is actually opened (plus the bounded visible-tab
    // fallback and coalesced socket signal below).
    if (tab === "mirrors") {
      void loadRepoMirrors(state.selectedRepo, { background: true });
    }
    // Runs are live executor state, never a load-once cache.
    if (tab === "actions") void loadRepoActions(state.selectedRepo);
    if (tab === "security") void loadRepoSecurity(state.selectedRepo);
    if (tab === "settings") {
      loadRepoFediverse(state.selectedRepo);
      void loadRepoSettingsLogo(state.selectedRepo);
      loadRepoDigestPreview(state.selectedRepo);
      void loadRepoWebhooks(state.selectedRepo);
      void loadRepoProviderMirror(state.selectedRepo);
      void loadRepoPullRulesSettings(state.selectedRepo);
      mountActionsSecrets($("[data-actions-secrets]"));
    }
    // Counts are live mirror state, not part of the tab's load-once cache.
    // Revalidate them whenever a collaboration tab is explicitly opened.
    if (tab === "issues" || tab === "pulls") {
      void refreshServedCounts(state.selectedRepo);
    }
    if (["branches", "commits", "issues", "projects", "pulls", "discussions", "releases", "insights", "sizemap", "agents"].includes(tab) && !state.loadedRepoTabs?.[tab]) {
      if (!state.loadedRepoTabs) state.loadedRepoTabs = {};
      state.loadedRepoTabs[tab] = true;
      if (tab === "branches") loadRepoBranches(state.selectedRepo);
      else if (tab === "commits") loadRepoCommits(state.selectedRepo);
      else if (tab === "issues") loadRepoIssues(state.selectedRepo);
      else if (tab === "projects") loadRepoProjects(state.selectedRepo);
      else if (tab === "releases") loadRepoReleases(state.selectedRepo);
      else if (tab === "insights") loadRepoInsights(state.selectedRepo);
      else if (tab === "sizemap") loadRepoSizeMapTab(state.selectedRepo);
      else if (tab === "agents") loadRepoAgents(state.selectedRepo);
      else loadRepoCollection(state.selectedRepo, tab, `[data-repo-${tab}]`);
    } else if (tab === "pulls"
        && state.repoRecordDetail?.kind !== "pulls") {
      // Re-selecting the tab from a comparison or the new-pull form returns
      // to the list those were opened from.
      state.repoCompare = null;
      loadRepoCollection(state.selectedRepo, "pulls", "[data-repo-pulls]");
    } else if (tab === "discussions") {
      if (Array.isArray(state.repoCollectionItems.discussions)) renderRepoDiscussions({ syncQuery: true });
      else loadRepoCollection(state.selectedRepo, "discussions", "[data-repo-discussions]");
    } else if (tab === "issues") {
      // Re-selecting returns to a freshly-read list. Rendering the prior array
      // here made both the list and its header look permanently cached.
      loadRepoIssues(state.selectedRepo);
    } else if (tab === "projects") {
      renderRepoProjects();
    } else if (tab === "agents") {
      loadRepoAgents(state.selectedRepo);
    } else if (tab === "branches") {
      // Re-entering repaints from the cached listing so the "viewing" marker
      // follows the current branch - and retries the fetch if the first
      // attempt never reached a mirror.
      void loadRepoBranches(state.selectedRepo);
    }
  }

  function repoPathParts(path) {
    return String(path || "").split("/").filter(Boolean);
  }

  function repoChildPath(path, name) {
    const child = String(name || "");
    return path ? `${path}/${child}` : child;
  }

  function renderRepoBreadcrumb(repo, path, terminalKind = "tree") {
    const detail = $("[data-repo-detail]");
    const crumb = detail?.querySelector("[data-repo-breadcrumb]");
    if (!crumb) return;

    const parts = repoPathParts(path);
    let acc = "";
    const rootLabel = repo.name || "repository";
    crumb.innerHTML = [`<button type="button" data-dashboard-tree-path="" class="font-semibold text-accent hover:underline">${escapeHtml(rootLabel)}</button>`]
      .concat(parts.map((part, index) => {
        acc = acc ? `${acc}/${part}` : part;
        const isBlobTerminal = terminalKind === "blob" && index === parts.length - 1;
        const pathAttribute = isBlobTerminal ? "data-dashboard-blob-path" : "data-dashboard-tree-path";
        return `<span class="text-muted-foreground">/</span> <button type="button" ${pathAttribute}="${escapeHtml(acc)}" class="${isBlobTerminal ? "text-foreground" : "text-accent hover:underline"}">${escapeHtml(part)}</button>`;
      })).join(" ");
  }

  let repoServedByFrame = 0;

  function renderRepoServedBy(node, tookMs) {
    const name = String(node || "").trim();
    state.repoServedBy = name ? { name, tookMs: Number(tookMs) || 0 } : null;
    // This status belongs with the repository identity. Keeping it in the
    // header avoids a duplicate, easily missed status line beside the file
    // breadcrumb while preserving the exact mirror that served the request.
    renderHeaderContext("explore");
    // Re-render the repo's mirror lists so the node that just answered gets
    // its green "serving this request" highlight without waiting on a fresh
    // /mirrors fetch.
    if (repoServedByFrame) return;
    repoServedByFrame = requestAnimationFrame(() => {
      repoServedByFrame = 0;
      repaintRepoMirrorLists();
    });
  }

  function repoExplorerRowClass(active = false) {
    return [
      "flex w-full min-w-0 items-center gap-2 border-l-2 px-3 py-2 text-left text-xs transition-colors",
      active ? "border-border bg-secondary text-foreground" : "border-transparent text-muted-foreground hover:bg-secondary/50 hover:text-foreground",
    ].join(" ");
  }

  function renderRepoExplorer(repo, path, entries) {
    const detail = $("[data-repo-detail]");
    const explorer = detail?.querySelector("[data-repo-explorer-tree]");
    if (!explorer) return;

    const parts = repoPathParts(path);
    const parentPath = parts.slice(0, -1).join("/");
    const currentName = parts[parts.length - 1] || repo.name || "repository";
    const rows = [
      `<button type="button" data-dashboard-tree-path="" class="${repoExplorerRowClass(!path)}"><i data-lucide="book-marked" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><span class="min-w-0 truncate">${escapeHtml(repo.name || "repository")}</span></button>`,
    ];

    if (path) {
      rows.push(`<button type="button" data-dashboard-tree-path="${escapeHtml(parentPath)}" class="${repoExplorerRowClass(false)}"><i data-lucide="corner-up-left" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><span class="min-w-0 truncate">..</span></button>`);
      rows.push(`<div data-repo-explorer-current-path="${escapeHtml(path)}" class="${repoExplorerRowClass(true)}"><i data-lucide="folder-open" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><span class="min-w-0 truncate">${escapeHtml(currentName)}</span></div>`);
    }

    if (entries.length) {
      rows.push(entries.map((entry) => {
        const isTree = entry.type === "tree";
        const childPath = repoChildPath(path, entry.name);
        return `<button type="button" data-repo-explorer-entry data-dashboard-${isTree ? "tree" : "blob"}-path="${escapeHtml(childPath)}" class="${repoExplorerRowClass(false)}">${fileIconHtml(entry, "h-3.5 w-3.5 shrink-0")}<span class="min-w-0 truncate">${escapeHtml(entry.name || "entry")}</span></button>`;
      }).join(""));
    } else {
      rows.push('<div class="px-3 py-2 text-xs text-muted-foreground">No files in this folder.</div>');
    }

    explorer.innerHTML = rows.join("");
  }

  function setRepoExplorerSelection(path, kind = "tree") {
    const detail = $("[data-repo-detail]");
    detail?.querySelectorAll("[data-repo-explorer-tree] [data-dashboard-tree-path], [data-repo-explorer-tree] [data-dashboard-blob-path]").forEach((button) => {
      const buttonPath = button.dataset.dashboardTreePath ?? button.dataset.dashboardBlobPath ?? "";
      const activeKind = button.hasAttribute("data-dashboard-blob-path") ? "blob" : "tree";
      const active = buttonPath === path && activeKind === kind;
      button.classList.toggle("border-border", active);
      button.classList.toggle("border-transparent", !active);
      button.classList.toggle("bg-secondary", active);
      button.classList.toggle("text-foreground", active);
      button.classList.toggle("text-muted-foreground", !active);
    });
  }

  function setRepoExplorerFocusMode(active) {
    const detail = $("[data-repo-detail]");
    const contentGrid = detail?.querySelector("[data-repo-content-grid]");
    const workspace = detail?.querySelector("[data-repo-code-workspace]");
    const explorer = detail?.querySelector("[data-repo-code-explorer]");
    const about = detail?.querySelector("[data-repo-about]");
    const rootToolbar = detail?.querySelector("[data-repo-root-toolbar]");
    const focusActions = detail?.querySelector("[data-repo-focus-actions]");
    if (!detail) return;

    // The grid collapse matches the base class renderRepoDetail emits (lg:) —
    // legitimate here because focus mode hides the About rail entirely.
    contentGrid?.classList.toggle("lg:grid-cols-[minmax(0,1fr)_18rem]", !active);
    contentGrid?.classList.toggle("lg:grid-cols-1", active);
    workspace?.classList.toggle("grid", active);
    workspace?.classList.toggle("lg:grid-cols-[13rem_minmax(0,1fr)]", active);
    workspace?.classList.toggle("xl:grid-cols-[14rem_minmax(0,1fr)]", active);
    workspace?.classList.toggle("2xl:grid-cols-[16rem_minmax(0,1fr)]", active);
    explorer?.classList.toggle("hidden", !active);
    about?.classList.toggle("hidden", active);
    rootToolbar?.classList.toggle("hidden", active);
    focusActions?.classList.toggle("hidden", !active);
  }

  function fileFinderRepoKey(repo) {
    return repo ? `${repoKey(repo)}@${repoSelectedBranch(repo)}` : "";
  }

  function resetRepoFileFinder(repo) {
    state.repoFileFinder = {
      repoKey: fileFinderRepoKey(repo),
      files: [],
      indexed: false,
      indexing: false,
      partial: false,
      error: "",
      selectedIndex: 0,
      returnFocus: null,
    };
  }

  function fileFinderOpen() {
    const finder = $("[data-repo-file-finder]");
    return Boolean(finder && !finder.classList.contains("hidden"));
  }

  async function buildRepoFileIndex(repo) {
    const key = fileFinderRepoKey(repo);
    if (!repo) return [];
    if (state.repoFileFinder.repoKey !== key) resetRepoFileFinder(repo);
    if (state.repoFileFinder.indexed || state.repoFileFinder.indexing) return state.repoFileFinder.files;

    state.repoFileFinder.indexing = true;
    state.repoFileFinder.error = "";
    renderRepoFileFinderResults($("[data-repo-file-finder-input]")?.value || "");
    const files = [];
    const queue = [""];
    const started = Date.now();
    try {
      while (queue.length && files.length < MAX_REPO_FILE_FINDER_RESULTS && ((Date.now() - started) / 1000) < MAX_REPO_FILE_FINDER_SECONDS) {
        const path = queue.shift();
        const data = await fetchRepoJson(repoLiveUrl(repo, "tree", { path }));
        const entries = Array.isArray(data.entries) ? data.entries.slice() : [];
        entries.sort((a, b) => {
          if (a.type !== b.type) return a.type === "tree" ? -1 : 1;
          return String(a.name || "").localeCompare(String(b.name || ""));
        });
        entries.forEach((entry) => {
          const childPath = repoChildPath(path, entry.name);
          if (entry.type === "tree") {
            queue.push(childPath);
          } else if (entry.type === "blob" && files.length < MAX_REPO_FILE_FINDER_RESULTS) {
            files.push(childPath);
          }
        });
      }
      state.repoFileFinder.files = files;
      state.repoFileFinder.partial = queue.length > 0 || files.length >= MAX_REPO_FILE_FINDER_RESULTS;
      state.repoFileFinder.indexed = true;
      return files;
    } catch (error) {
      state.repoFileFinder.error = error?.message || "Could not index files from the live mirror.";
      state.repoFileFinder.files = files;
      return files;
    } finally {
      state.repoFileFinder.indexing = false;
      renderRepoFileFinderResults($("[data-repo-file-finder-input]")?.value || "");
    }
  }

  function renderRepoFileFinderResults(query = "") {
    const finder = $("[data-repo-file-finder]");
    if (!finder) return;
    const results = finder.querySelector("[data-repo-file-finder-results]");
    const status = finder.querySelector("[data-repo-file-finder-status]");
    if (!results || !status) return;

    const needle = String(query || "").trim().toLowerCase();
    const matches = state.repoFileFinder.files
      .filter((path) => !needle || path.toLowerCase().includes(needle))
      .slice(0, 50);
    state.repoFileFinder.selectedIndex = Math.min(state.repoFileFinder.selectedIndex, Math.max(matches.length - 1, 0));

    if (state.repoFileFinder.indexing) {
      status.textContent = "Indexing live mirror...";
    } else if (state.repoFileFinder.error && !state.repoFileFinder.files.length) {
      status.textContent = "No reachable mirror host is available to index files.";
    } else if (state.repoFileFinder.partial) {
      status.textContent = `Showing ${formatCount(matches.length)} matches from the first ${formatCount(state.repoFileFinder.files.length)} indexed files.`;
    } else {
      status.textContent = `${formatCount(matches.length)} matching file${matches.length === 1 ? "" : "s"}.`;
    }

    if (!matches.length) {
      results.innerHTML = `<div class="px-3 py-6 text-center text-sm text-muted-foreground">${state.repoFileFinder.indexing ? "Still indexing files..." : "No files match your search."}</div>`;
      syncRepoFileFinderActiveDescendant();
      return;
    }
    results.innerHTML = matches.map((path, index) => `
      <button type="button" role="option" tabindex="-1" id="repo-file-finder-result-${index}" data-repo-file-finder-result data-repo-file-finder-path="${escapeHtml(path)}" aria-selected="${index === state.repoFileFinder.selectedIndex ? "true" : "false"}" class="grid w-full grid-cols-[1.25rem_minmax(0,1fr)] items-center gap-2 rounded-md px-3 py-2 text-left text-sm transition-colors ${index === state.repoFileFinder.selectedIndex ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-secondary/60 hover:text-foreground"}">
        <i data-lucide="file" class="h-3.5 w-3.5 text-muted-foreground"></i>
        <span class="min-w-0 truncate font-mono">${escapeHtml(path)}</span>
      </button>`).join("");
    window.lucide?.createIcons();
    syncRepoFileFinderActiveDescendant();
  }

  function syncRepoFileFinderActiveDescendant() {
    const finder = $("[data-repo-file-finder]");
    const input = finder?.querySelector("[data-repo-file-finder-input]");
    const selected = finder?.querySelector('[data-repo-file-finder-result][aria-selected="true"]');
    if (input) input.toggleAttribute("aria-activedescendant", Boolean(selected));
    if (input && selected) input.setAttribute("aria-activedescendant", selected.id);
  }

  function moveRepoFileFinderSelection(delta) {
    const results = $$('[data-repo-file-finder-result]');
    if (!results.length) return;
    state.repoFileFinder.selectedIndex = (state.repoFileFinder.selectedIndex + delta + results.length) % results.length;
    results.forEach((button, index) => {
      const active = index === state.repoFileFinder.selectedIndex;
      button.setAttribute("aria-selected", active ? "true" : "false");
      button.classList.toggle("bg-secondary", active);
      button.classList.toggle("text-foreground", active);
      button.classList.toggle("text-muted-foreground", !active);
      if (active) button.scrollIntoView({ block: "nearest" });
    });
    syncRepoFileFinderActiveDescendant();
  }

  function selectRepoFileFinderResult() {
    const selected = $('[data-repo-file-finder-result][aria-selected="true"]') || $('[data-repo-file-finder-result]');
    if (!selected || !state.selectedRepo) return;
    closeRepoFileFinder({ restoreFocus: false });
    loadRepositoryBlob(state.selectedRepo, selected.dataset.repoFileFinderPath || "");
  }

  function closeRepoFileFinder({ restoreFocus = true } = {}) {
    const finder = $("[data-repo-file-finder]");
    if (!finder) return;
    finder.classList.add("hidden");
    finder.classList.remove("flex");
    const opener = state.repoFileFinder.returnFocus;
    state.repoFileFinder.returnFocus = null;
    if (restoreFocus && opener?.isConnected) opener.focus({ preventScroll: true });
  }

  async function openRepoFileFinder(trigger) {
    if (!state.selectedRepo) return;
    const finder = $("[data-repo-file-finder]");
    if (!finder) return;
    if (state.repoFileFinder.repoKey !== fileFinderRepoKey(state.selectedRepo)) resetRepoFileFinder(state.selectedRepo);
    const opener = trigger instanceof HTMLElement ? trigger : document.activeElement;
    state.repoFileFinder.returnFocus = opener instanceof HTMLElement ? opener : null;
    finder.classList.remove("hidden");
    finder.classList.add("flex");
    const input = finder.querySelector("[data-repo-file-finder-input]");
    if (input) input.value = "";
    state.repoFileFinder.selectedIndex = 0;
    renderRepoFileFinderResults("");
    window.setTimeout(() => input?.focus(), 0);
    buildRepoFileIndex(state.selectedRepo);
  }

  function repoLanguage(path) {
    const file = String(path || "").toLowerCase();
    if (file.endsWith(".css") || file.endsWith(".scss")) return "css";
    if (file.endsWith(".ts") || file.endsWith(".tsx") || file.endsWith(".js") || file.endsWith(".jsx") || file.endsWith(".mjs") || file.endsWith(".cjs")) return "javascript";
    if (file.endsWith(".json") || file.endsWith(".jsonc")) return "json";
    if (file.endsWith(".html") || file.endsWith(".xml") || file.endsWith(".svg")) return "markup";
    return "text";
  }

  function highlightWithRules(line, rules) {
    let index = 0;
    let html = "";
    const source = String(line ?? "");
    while (index < source.length) {
      const chunk = source.slice(index);
      const rule = rules.find((candidate) => candidate.pattern.test(chunk));
      if (rule) {
        rule.pattern.lastIndex = 0;
        const match = chunk.match(rule.pattern)?.[0] || "";
        html += `<span class="${rule.className}">${escapeHtml(match)}</span>`;
        index += match.length;
      } else {
        html += escapeHtml(source[index]);
        index += 1;
      }
    }
    return html || " ";
  }

  function highlightCodeLine(line, path) {
    const language = repoLanguage(path);
    const shared = [
      { pattern: /^\/\/.*/, className: "text-zinc-500" },
      { pattern: /^\/\*.*?\*\//, className: "text-zinc-500" },
      { pattern: /^"(?:\\.|[^"\\])*"/, className: "text-emerald-300" },
      { pattern: /^'(?:\\.|[^'\\])*'/, className: "text-emerald-300" },
      { pattern: /^`(?:\\.|[^`\\])*`/, className: "text-emerald-300" },
      { pattern: /^\b\d+(?:\.\d+)?(?:px|rem|em|vh|vw|%|ms|s)?\b/, className: "text-amber-300" },
    ];

    if (language === "css") {
      return highlightWithRules(line, [
        { pattern: /^\/\*.*?\*\//, className: "text-zinc-500" },
        { pattern: /^"(?:\\.|[^"\\])*"/, className: "text-emerald-300" },
        { pattern: /^'(?:\\.|[^'\\])*'/, className: "text-emerald-300" },
        { pattern: /^@[a-zA-Z-]+/, className: "text-sky-300" },
        { pattern: /^--[a-zA-Z0-9-_]+(?=\s*:)?/, className: "text-blue-300" },
        { pattern: /^[a-zA-Z-]+(?=\s*:)/, className: "text-blue-300" },
        { pattern: /^#[0-9a-fA-F]{3,8}\b/, className: "text-amber-300" },
        { pattern: /^\b\d+(?:\.\d+)?(?:px|rem|em|vh|vw|%|ms|s)?\b/, className: "text-amber-300" },
        { pattern: /^[a-zA-Z-]+(?=\()/, className: "text-purple-300" },
      ]);
    }

    if (language === "json") {
      return highlightWithRules(line, [
        { pattern: /^"(?:\\.|[^"\\])*"(?=\s*:)/, className: "text-blue-300" },
        { pattern: /^"(?:\\.|[^"\\])*"/, className: "text-emerald-300" },
        { pattern: /^-?\b\d+(?:\.\d+)?\b/, className: "text-amber-300" },
        { pattern: /^(?:true|false|null)\b/, className: "text-sky-300" },
      ]);
    }

    if (language === "markup") {
      return highlightWithRules(line, [
        { pattern: /^<!--.*?-->/, className: "text-zinc-500" },
        { pattern: /^<\/?[a-zA-Z][a-zA-Z0-9:-]*/, className: "text-sky-300" },
        { pattern: /^\s+[a-zA-Z_:][-a-zA-Z0-9_:.]*(?==)/, className: "text-purple-300" },
        { pattern: /^"(?:\\.|[^"\\])*"/, className: "text-emerald-300" },
        { pattern: /^'(?:\\.|[^'\\])*'/, className: "text-emerald-300" },
      ]);
    }

    if (language === "javascript") {
      return highlightWithRules(line, [
        ...shared,
        { pattern: /^@[a-zA-Z_$][\w$]*/, className: "text-amber-300" },
        { pattern: /^(?:import|from|export|const|let|var|function|return|if|else|for|while|await|async|class|extends|interface|type|new|try|catch|throw|switch|case|break|continue|default|true|false|null|undefined)\b/, className: "text-sky-300" },
        { pattern: /^[A-Z][a-zA-Z0-9_$]*(?=\b)/, className: "text-cyan-300" },
        { pattern: /^[a-zA-Z_$][\w$]*(?=\()/, className: "text-purple-300" },
      ]);
    }

    return escapeHtml(line) || " ";
  }

  function repoFileName(path) {
    const name = repoPathParts(path).pop() || "file.txt";
    return name.replace(/[\\/:*?"<>|]+/g, "-") || "file.txt";
  }

  function repoPathExtension(path) {
    const name = String(repoFileName(path) || "").toLowerCase();
    const dot = name.lastIndexOf(".");
    return dot >= 0 ? name.slice(dot + 1) : "";
  }

  function repoFileSupportsMarkdownPreview(path) {
    return ["md", "markdown", "mdx"].includes(repoPathExtension(path));
  }

  function repoPreviewKindFromPath(path) {
    const extension = repoPathExtension(path);
    const previews = {
      apng: ["image", "image/png", "Image"],
      avif: ["image", "image/avif", "Image"],
      bmp: ["image", "image/bmp", "Image"],
      gif: ["image", "image/gif", "Image"],
      ico: ["image", "image/x-icon", "Image"],
      jfif: ["image", "image/jpeg", "Image"],
      jpeg: ["image", "image/jpeg", "Image"],
      jpg: ["image", "image/jpeg", "Image"],
      png: ["image", "image/png", "Image"],
      svg: ["image", "image/svg+xml", "Image"],
      tif: ["image", "image/tiff", "Image"],
      tiff: ["image", "image/tiff", "Image"],
      webp: ["image", "image/webp", "Image"],
      "3g2": ["video", "video/3gpp2", "Video"],
      "3gp": ["video", "video/3gpp", "Video"],
      avi: ["video", "video/x-msvideo", "Video"],
      m4v: ["video", "video/mp4", "Video"],
      mkv: ["video", "video/x-matroska", "Video"],
      mov: ["video", "video/quicktime", "Video"],
      mp4: ["video", "video/mp4", "Video"],
      mpeg: ["video", "video/mpeg", "Video"],
      mpg: ["video", "video/mpeg", "Video"],
      ogv: ["video", "video/ogg", "Video"],
      webm: ["video", "video/webm", "Video"],
      aac: ["audio", "audio/aac", "Audio"],
      flac: ["audio", "audio/flac", "Audio"],
      m4a: ["audio", "audio/mp4", "Audio"],
      mid: ["audio", "audio/midi", "Audio"],
      midi: ["audio", "audio/midi", "Audio"],
      mp3: ["audio", "audio/mpeg", "Audio"],
      oga: ["audio", "audio/ogg", "Audio"],
      ogg: ["audio", "audio/ogg", "Audio"],
      opus: ["audio", "audio/ogg", "Audio"],
      wav: ["audio", "audio/wav", "Audio"],
      weba: ["audio", "audio/webm", "Audio"],
      pdf: ["pdf", "application/pdf", "PDF"],
      csv: ["csv", "text/csv", "CSV"],
      tab: ["csv", "text/tab-separated-values", "TSV"],
      tsv: ["csv", "text/tab-separated-values", "TSV"],
    };
    const preview = previews[extension];
    if (!preview) return null;
    return { kind: preview[0], mime: preview[1], label: preview[2] };
  }

  function repoRawUrl(repo, path) {
    return repoLiveUrl(repo, "raw", { path });
  }

  function repoLiveUrl(repo, action, params = {}) {
    const query = new URLSearchParams();
    Object.entries(params || {}).forEach(([key, value]) => {
      query.set(key, String(value ?? ""));
    });
    const current = new URLSearchParams(location.search || "");
    ["viewer", "ts", "sig"].forEach((key) => {
      const value = current.get(key);
      if (value) query.set(key, value);
    });
    // Callers reading the dedicated pull-metadata branch first resolve its
    // immutable OID with resolveRepoPullMetadataCommit(), then pass that exact
    // value here. Explicitly empty refs retain the generic no-ref URL contract
    // for compatibility callers; pull readers never rely on that ambiguity.
    if (!("ref" in (params || {}))) query.set("ref", repoSelectedBranch(repo));
    else if (!String(params.ref || "")) query.delete("ref");
    const version = repoDataVersion(repo);
    if (version) query.set("fmv", version);
    return `${repoApiBase(repo)}/${action}?${query.toString()}`;
  }

  function repoPreviewMeta(data, repo, path) {
    const preview = repoPreviewKindFromPath(path);
    const encoding = String(data?.encoding || "").toLowerCase();
    const content = String(data?.content ?? data?.text ?? "");
    const baseSize = encoding === "base64"
      ? Math.ceil((content.replace(/\s/g, "").length * 3) / 4)
      : content.length;
    if (!preview && encoding !== "base64") return null;
    if (preview?.kind === "csv" && encoding === "base64") {
      return null;
    }
    return {
      path,
      rawUrl: repoRawUrl(repo, path),
      kind: preview?.kind || "binary",
      mime: preview?.mime || "application/octet-stream",
      label: preview?.label || "Binary",
      content,
      size: Number(data?.size) || baseSize,
      truncated: Boolean(data?.truncated),
    };
  }

  function repoFileModeButtonClass(active) {
    return [
      "rounded px-3 py-1 text-xs font-medium transition-colors",
      active ? "bg-secondary text-foreground" : "text-muted-foreground hover:text-foreground",
    ].join(" ");
  }

  async function copyTextToClipboard(text) {
    const value = String(text || "");
    try {
      if (!navigator.clipboard?.writeText) throw new Error("clipboard unavailable");
      await navigator.clipboard.writeText(value);
      return true;
    } catch (_) {
      const temp = document.createElement("textarea");
      temp.value = value;
      temp.setAttribute("readonly", "");
      temp.style.position = "fixed";
      temp.style.opacity = "0";
      document.body.appendChild(temp);
      try {
        temp.focus();
        temp.select();
        return document.execCommand?.("copy") === true;
      } catch (_) {
        return false;
      } finally {
        temp.remove();
      }
    }
  }

  function downloadRepoFile(path, content) {
    const blob = new Blob([String(content || "")], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = repoFileName(path);
    link.rel = "noopener";
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 30000);
  }

  function downloadRepoRawBlob(meta) {
    const link = document.createElement("a");
    link.href = meta.rawUrl;
    link.download = repoFileName(meta.path);
    link.rel = "noopener";
    document.body.appendChild(link);
    link.click();
    link.remove();
  }

  function repoFileStatusForMode(mode) {
    if (mode === "blame") return "Blame view - current mirror commit shown for each line.";
    if (mode === "raw") return "Raw view - exact file contents.";
    if (mode === "preview") return "Preview view - rendered Markdown.";
    return "Code view - highlighted source.";
  }

  function updateRepoFileStatus(viewer, message, tone = "muted") {
    const status = viewer?.querySelector("[data-repo-file-status]");
    if (!status) return;
    status.textContent = message;
    status.classList.toggle("text-destructive", tone === "bad");
    status.classList.toggle("text-muted-foreground", tone !== "bad");
  }

  function renderRepoFileRows(lines, path, mode, meta) {
    const content = String(meta?.content ?? lines.join("\n"));
    if (mode === "preview") {
      const rendered = typeof renderMarkdown === "function"
        ? renderMarkdown(content, { repo: meta?.repo, basePath: path })
        : escapeHtml(content);
      // A rendered document reads top to bottom like the README card, so it
      // is not boxed into the code view's scrolling viewport.
      return `<article data-repo-file-preview class="overflow-x-auto p-4 text-sm leading-6 text-foreground [&>*:first-child]:mt-0 [&>*:last-child]:mb-0">${rendered}</article>`;
    }
    if (mode === "raw") {
      return `<pre data-repo-file-raw class="max-h-[34rem] overflow-auto whitespace-pre p-4 font-mono text-xs leading-5 text-zinc-200">${escapeHtml(content)}</pre>`;
    }

    const blame = mode === "blame";
    const commit = String(meta?.commitId || "live").slice(0, 7);
    const author = String(meta?.author || meta?.owner || "mirror");
    const blameLabel = `${commit} ${author}`;
    const grid = blame
      ? "grid-cols-[10rem_3.25rem_minmax(32rem,1fr)]"
      : "grid-cols-[3.25rem_minmax(32rem,1fr)]";
    return `<div data-repo-file-code class="max-h-[34rem] overflow-auto py-3 text-xs leading-5">
      ${lines.map((line, index) => `
        <div class="grid min-w-max ${grid}">
          ${blame ? `<span class="select-none truncate px-3 font-mono text-muted-foreground" title="${escapeHtml(blameLabel)}">${escapeHtml(blameLabel)}</span>` : ""}
          <span class="select-none px-3 text-right font-mono text-muted-foreground">${index + 1}</span>
          <span class="whitespace-pre px-4 font-mono text-zinc-200">${highlightCodeLine(line, path)}</span>
        </div>`).join("")}
    </div>`;
  }

  function renderRepoMediaPreview(meta) {
    const name = escapeHtml(repoFileName(meta.path));
    const mime = escapeHtml(meta.mime);
    const rawUrl = escapeHtml(meta.rawUrl);
    let media = "";
    if (meta.kind === "image") {
      const source = meta.mime === "image/svg+xml" ? "data-repo-svg-src" : "src";
      media = `<img data-repo-image class="max-h-[34rem] max-w-full rounded-sm object-contain" ${source}="${rawUrl}" alt="${name}" />`;
    } else if (meta.kind === "video") {
      media = `<video data-repo-video controls preload="metadata" class="max-h-[34rem] w-full max-w-5xl rounded-md bg-black"><source src="${rawUrl}" type="${mime}" />This browser cannot play this video.</video>`;
    } else if (meta.kind === "audio") {
      media = `<audio data-repo-audio controls preload="metadata" class="w-full max-w-3xl"><source src="${rawUrl}" type="${mime}" />This browser cannot play this audio.</audio>`;
    } else if (meta.kind === "pdf") {
      media = `<iframe data-repo-pdf title="${name}" src="${rawUrl}" class="h-[34rem] w-full rounded-md border border-border bg-background"></iframe>`;
    }
    return `
      <figure data-repo-media-preview class="grid gap-3 p-4">
        <div class="flex min-h-[18rem] items-center justify-center rounded-md border border-border bg-secondary/20 p-3">
          ${media}
        </div>
        <figcaption class="flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground">
          <span class="min-w-0 truncate font-mono text-foreground">${name}</span>
          <span class="font-mono">${mime}</span>
          ${meta.size ? `<span class="font-mono">${escapeHtml(formatSize(meta.size))}</span>` : ""}
          ${meta.truncated ? '<span class="font-mono text-attention">metadata truncated</span>' : ""}
        </figcaption>
      </figure>`;
  }

  function parseDelimitedPreviewRows(content, delimiter) {
    const rows = [];
    let row = [];
    let field = "";
    let quoted = false;
    const text = String(content || "").replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    for (let index = 0; index < text.length && rows.length < 51; index += 1) {
      const char = text[index];
      if (quoted) {
        if (char === '"' && text[index + 1] === '"') {
          field += '"';
          index += 1;
        } else if (char === '"') {
          quoted = false;
        } else {
          field += char;
        }
      } else if (char === '"') {
        quoted = true;
      } else if (char === delimiter) {
        row.push(field);
        field = "";
      } else if (char === "\n") {
        row.push(field);
        rows.push(row);
        row = [];
        field = "";
      } else {
        field += char;
      }
    }
    if (field || row.length || text.endsWith(delimiter)) {
      row.push(field);
      rows.push(row);
    }
    return rows.filter((items) => items.some((item) => String(item).length));
  }

  function renderRepoCsvPreview(meta) {
    const delimiter = meta.mime === "text/tab-separated-values" ? "\t" : ",";
    const rows = parseDelimitedPreviewRows(meta.content, delimiter);
    const visibleRows = rows.slice(0, 50);
    const columnCount = Math.min(20, Math.max(1, ...visibleRows.map((row) => row.length)));
    const headers = visibleRows[0] || [];
    const body = visibleRows.slice(1);
    const cellClass = "max-w-[18rem] truncate border-t border-border px-3 py-2 text-left align-top";
    return `
      <div data-repo-csv-preview class="max-h-[34rem] overflow-auto p-4">
        <table class="min-w-full border-separate border-spacing-0 text-xs">
          <thead class="sticky top-0 z-10 bg-background text-muted-foreground">
            <tr>${Array.from({ length: columnCount }, (_, index) => `<th class="${cellClass} font-mono">${escapeHtml(headers[index] || `Column ${index + 1}`)}</th>`).join("")}</tr>
          </thead>
          <tbody class="text-zinc-200">
            ${body.length ? body.map((row) => `<tr>${Array.from({ length: columnCount }, (_, index) => `<td class="${cellClass} font-mono">${escapeHtml(row[index] || "")}</td>`).join("")}</tr>`).join("") : `<tr><td class="${cellClass} font-mono" colspan="${columnCount}">${rows.length ? "No data rows." : "No previewable rows."}</td></tr>`}
          </tbody>
        </table>
      </div>`;
  }

  function renderRepoUnsupportedBinary(meta) {
    return `
      <div data-repo-binary-preview class="grid gap-4 p-6">
        <div class="flex min-w-0 items-center gap-3 rounded-md border border-border bg-secondary/20 p-4">
          <i data-lucide="file-archive" class="h-5 w-5 shrink-0 text-muted-foreground"></i>
          <div class="min-w-0">
            <div class="truncate font-mono text-sm text-foreground">${escapeHtml(repoFileName(meta.path))}</div>
            <div class="mt-1 flex flex-wrap gap-x-3 gap-y-1 text-xs text-muted-foreground">
              <span class="font-mono">${escapeHtml(meta.mime)}</span>
              ${meta.size ? `<span class="font-mono">${escapeHtml(formatSize(meta.size))}</span>` : ""}
              <span>Preview unavailable in browser.</span>
            </div>
          </div>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <a href="${escapeHtml(meta.rawUrl)}" target="_blank" rel="noopener" class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary transition-colors"><i data-lucide="external-link" class="h-3.5 w-3.5"></i>Raw</a>
          <a href="${escapeHtml(meta.rawUrl)}" download="${escapeHtml(repoFileName(meta.path))}" rel="noopener" class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary transition-colors"><i data-lucide="download" class="h-3.5 w-3.5"></i>Download</a>
        </div>
      </div>`;
  }

  function bindRepoPreviewToolbar(viewer, meta) {
    viewer._repoFileToolbarAbort?.abort();
    const controller = new AbortController();
    viewer._repoFileToolbarAbort = controller;
    viewer.addEventListener("click", (event) => {
      const actionButton = event.target.closest("button[data-repo-preview-action]");
      if (!actionButton || !viewer.contains(actionButton)) return;
      const action = actionButton.dataset.repoPreviewAction;
      if (action === "download") {
        downloadRepoRawBlob(meta);
        updateRepoFileStatus(viewer, `Downloading ${repoFileName(meta.path)}.`, "good");
      } else if (action === "fullscreen") {
        toggleRepoFileFullscreen(viewer);
      }
    }, { signal: controller.signal });

    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      setRepoFileFullscreenFallback(viewer.querySelector("[data-repo-file-shell]"), false);
    }, { signal: controller.signal });
  }

  function renderRepoPreview(viewer, repo, path, data) {
    const meta = repoPreviewMeta(data, repo, path);
    if (!meta) return false;
    const content = meta.kind === "csv"
      ? renderRepoCsvPreview(meta)
      : ["image", "video", "audio", "pdf"].includes(meta.kind)
        ? renderRepoMediaPreview(meta)
        : renderRepoUnsupportedBinary(meta);
    const status = meta.kind === "binary"
      ? "Binary file - use raw or download."
      : `${meta.label} preview.`;
    viewer.innerHTML = `
      <div data-repo-file-shell class="overflow-hidden rounded-lg border border-border bg-background">
        <div data-repo-file-toolbar class="flex min-h-11 flex-wrap items-center justify-between gap-2 border-b border-border bg-secondary/40 px-3 py-2">
          <div class="flex min-w-0 flex-wrap items-center gap-2">
            <span class="font-mono text-[10px] text-muted-foreground">${escapeHtml(meta.label)}</span>
            ${meta.size ? `<span class="font-mono text-[10px] text-muted-foreground">${escapeHtml(formatSize(meta.size))}</span>` : ""}
            <span class="font-mono text-[10px] text-muted-foreground">${escapeHtml(meta.mime)}</span>
            <span data-repo-file-status class="min-w-0 font-mono text-[10px] text-muted-foreground">${escapeHtml(status)}</span>
          </div>
          <div class="flex items-center gap-1.5">
            <a href="${escapeHtml(meta.rawUrl)}" target="_blank" rel="noopener" aria-label="Open raw file" class="fm-btn fm-btn-sm fm-btn-icon"><i data-lucide="external-link" class="h-3.5 w-3.5"></i></a>
            <button type="button" data-repo-preview-action="download" aria-label="Download file" class="fm-btn fm-btn-sm fm-btn-icon"><i data-lucide="download" class="h-3.5 w-3.5"></i></button>
            <button type="button" data-repo-preview-action="fullscreen" aria-label="Open full screen" class="fm-btn fm-btn-sm fm-btn-icon"><i data-lucide="maximize-2" class="h-3.5 w-3.5"></i></button>
          </div>
        </div>
        <div data-repo-file-content>${content}</div>
      </div>`;
    bindRepoPreviewToolbar(viewer, meta);
    navigateHistory(repoLinkUrl(repo, "blob", path));
    window.lucide?.createIcons();
    return true;
  }

  function setRepoFileMode(viewer, mode, lines, path, meta) {
    const content = viewer?.querySelector("[data-repo-file-content]");
    if (!content) return;
    content.innerHTML = renderRepoFileRows(lines, path, mode, meta);
    viewer.dataset.repoFileCurrentMode = mode;
    delete viewer.dataset.repoFileMode;
    viewer.querySelectorAll("button[data-repo-file-mode]").forEach((button) => {
      const active = button.dataset.repoFileMode === mode;
      button.className = repoFileModeButtonClass(active);
      button.setAttribute("aria-pressed", active ? "true" : "false");
    });
    updateRepoFileStatus(viewer, repoFileStatusForMode(mode));
    window.lucide?.createIcons();
  }

  async function repoFileEditRequest(repo, body) {
    const headers = {
      accept: "application/json",
      "content-type": "application/json",
    };
    const token = String(state.session?.sessionToken || "");
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const payload = { ...body };
    if (token && token !== "cookie") payload.sessionToken = token;
    const response = await fetch(`${repoApiBase(repo)}/file-edit`, {
      method: "POST",
      credentials: "same-origin",
      cache: "no-store",
      headers,
      body: JSON.stringify(payload),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data?.ok === false) {
      const error = new Error(data?.message || data?.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = data?.error || "";
      error.data = data;
      throw error;
    }
    return data;
  }

  function repoFileEditError(error) {
    if (error?.code === "read_only_mirror") return error?.data?.message || REPO_READ_ONLY_MIRROR_HINT;
    if (error?.code === "conflict" || error?.status === 409) {
      return "The file changed while you were editing. Reload and try again.";
    }
    if (error?.status === 401) return "Sign in to edit repository files.";
    if (error?.status === 403) return "You don't have write access to this repository. Save again to propose your change as a pull request.";
    return error?.message || "Could not save the file.";
  }

  function repoFileSignInUrl() {
    return "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
  }

  const REPO_FILE_PROPOSE_NOTICE = "You don't have write access to this repository. Saving proposes your change as a pull request.";

  function setRepoFileEditorProposing(editor, proposing, { access = "" } = {}) {
    if (!editor) return;
    editor.dataset.repoFileEditorMode = proposing ? "propose" : "commit";
    if (access) editor.dataset.repoFileEditorAccess = access;
    const readOnly = editor.dataset.repoFileEditorAccess === "propose";
    const notice = editor.querySelector("[data-repo-file-editor-notice]");
    if (notice) notice.hidden = !readOnly;
    const proposeToggle = editor.querySelector("[data-repo-file-editor-propose-wrap]");
    if (proposeToggle) proposeToggle.hidden = editor.dataset.repoFileEditorAccess !== "write";
    const checkbox = editor.querySelector("[data-repo-file-editor-propose]");
    if (checkbox) checkbox.checked = proposing;
    const branchLabel = editor.querySelector("[data-repo-file-editor-branch-label]");
    if (branchLabel) branchLabel.textContent = proposing ? "Base branch" : "Branch";
    const branchInput = editor.querySelector("[data-repo-file-editor-branch]");
    if (branchInput) branchInput.readOnly = proposing;
    const saveLabel = editor.querySelector("[data-repo-file-editor-save-label]");
    if (saveLabel) saveLabel.textContent = proposing ? "Propose changes" : "Save";
  }

  async function probeRepoFileEditAccess(viewer, editor, repo, signal) {
    try {
      const data = await repoFileEditRequest(repo, { mode: "probe" });
      if (signal.aborted || !editor.isConnected) return;
      if (data?.access === "propose") {
        setRepoFileEditorProposing(editor, true, { access: "propose" });
        const branchInput = editor.querySelector("[data-repo-file-editor-branch]");
        if (branchInput) branchInput.value = editor.dataset.repoFileEditorBase || branchInput.value;
      } else if (data?.access === "write") {
        setRepoFileEditorProposing(editor, editor.dataset.repoFileEditorMode === "propose", { access: "write" });
      }
    } catch (error) {
      if (signal.aborted || !editor.isConnected) return;
      if (error?.status === 401) {
        updateRepoFileStatus(viewer, "Sign in to edit repository files.", "bad");
        location.href = repoFileSignInUrl();
      }
    }
  }

  async function proposeRepoFileEdit(viewer, repo, proposal) {
    const title = String(proposal.subject || "").trim();
    updateRepoFileStatus(viewer, "Signing and sending the pull request...", "good");
    const result = await submitWebFileProposal(repo, proposal, title);
    const number = Number(result?.number || 0);
    if (!Number.isInteger(number) || number <= 0) throw new Error("number_unavailable");
    rememberOptimisticRepoRecord(repo, "pulls", number, {
      values: {
        number,
        title,
        status: "open",
        base: result.pull.base,
        head: result.pull.head,
        author: result.pull.author,
        authorName: result.pull.authorName || "you",
        ts: result.pull.ts,
      },
      body: "",
      pullPatch: {
        patch: result.pull.patch,
        files: parsePatchStats(result.pull.patch),
        unavailable: false,
      },
      pullConversation: [],
      pullCommits: [],
      pullAgents: [],
      pullTimeline: [],
    });
    updateRepoFileStatus(viewer, `Opened pull request #${number}.`, "good");
    window.location.assign(repoRecordUrl(repo, "pulls", number, "conversation"));
    return number;
  }

  function repoFileEditorBranchOptions(repo, current) {
    const seen = new Set();
    const branches = [];
    [current, ...repoBranchList(repo).map((branch) => branch.name)].forEach((branch) => {
      const name = String(branch || "").trim();
      if (!name || seen.has(name)) return;
      seen.add(name);
      branches.push(name);
    });
    return branches;
  }

  function closeRepoFileEditor(viewer, repo, path, content, lines, meta) {
    viewer._repoFileEditorAbort?.abort();
    delete viewer._repoFileEditorAbort;
    viewer.querySelector("[data-repo-file-editor]")?.remove();
    viewer.querySelector("[data-repo-file-content]")?.classList.remove("hidden");
    viewer.querySelector("[data-repo-file-modes]")?.classList.remove("hidden");
    setRepoFileMode(viewer, viewer.dataset.repoFileCurrentMode || "code", lines, path, meta);
    bindRepoFileToolbar(viewer, repo, path, content, lines, meta);
  }

  function openRepoFileEditor(viewer, repo, path, content, lines, meta) {
    const shell = viewer?.querySelector("[data-repo-file-shell]");
    const fileContent = viewer?.querySelector("[data-repo-file-content]");
    if (!shell || !fileContent) return;
    viewer.querySelector("[data-repo-file-editor]")?.remove();
    viewer._repoFileEditorAbort?.abort();
    const controller = new AbortController();
    viewer._repoFileEditorAbort = controller;
    const mode = viewer.dataset.repoFileCurrentMode || "code";
    const currentBranch = repoSelectedBranch(repo) || repoDefaultBranch(repo) || "main";
    const branchListId = `repo-file-branches-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
    const options = repoFileEditorBranchOptions(repo, currentBranch)
      .map((branch) => `<option value="${escapeHtml(branch)}"></option>`)
      .join("");
    const editor = document.createElement("form");
    editor.dataset.repoFileEditor = "true";
    editor.className = "grid gap-3 border-t border-border bg-background p-3";
    editor.innerHTML = `
      <p data-repo-file-editor-notice hidden class="flex items-center gap-2 text-xs text-muted-foreground"><i data-lucide="git-pull-request" class="h-3.5 w-3.5 text-muted-foreground"></i><span data-repo-file-editor-notice-text>${escapeHtml(REPO_FILE_PROPOSE_NOTICE)}</span></p>
      <textarea data-repo-file-editor-content spellcheck="false" class="min-h-[24rem] w-full resize-y rounded-md border border-border bg-black/20 p-3 font-mono text-xs leading-5 text-foreground outline-none focus:border-primary/70">${escapeHtml(content)}</textarea>
      <div class="grid gap-2 md:grid-cols-[minmax(12rem,18rem)_1fr_auto]">
        <label class="grid gap-1 text-[10px] uppercase tracking-wide text-muted-foreground">
          <span data-repo-file-editor-branch-label>Branch</span>
          <input data-repo-file-editor-branch list="${escapeHtml(branchListId)}" value="${escapeHtml(currentBranch)}" class="h-9 rounded-md border border-border bg-background px-3 font-mono text-xs normal-case tracking-normal text-foreground outline-none focus:border-primary/70">
          <datalist id="${escapeHtml(branchListId)}">${options}</datalist>
        </label>
        <label class="grid gap-1 text-[10px] uppercase tracking-wide text-muted-foreground">
          <span>Commit message</span>
          <input data-repo-file-editor-message value="${escapeHtml(`Edit ${repoFileName(path)} on the web`)}" class="h-9 rounded-md border border-border bg-background px-3 text-xs normal-case tracking-normal text-foreground outline-none focus:border-primary/70">
        </label>
        <div class="flex items-end gap-2">
          <button type="button" data-repo-file-editor-cancel class="inline-flex h-9 items-center rounded-md border border-border px-3 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors">Cancel</button>
          <button type="submit" data-repo-file-editor-save class="inline-flex h-9 items-center gap-1.5 rounded-md border border-primary/50 bg-cta px-3 text-xs font-medium text-cta-foreground hover:bg-cta/90 transition-colors"><i data-lucide="save" class="h-3.5 w-3.5"></i><span data-repo-file-editor-save-label>Save</span></button>
        </div>
      </div>
      <label data-repo-file-editor-propose-wrap class="inline-flex items-center gap-2 text-xs text-muted-foreground">
        <input type="checkbox" data-repo-file-editor-propose class="h-3.5 w-3.5 rounded border-border accent-primary">
        <span>Propose as a pull request instead of committing</span>
      </label>`;
    fileContent.classList.add("hidden");
    viewer.querySelector("[data-repo-file-modes]")?.classList.add("hidden");
    shell.appendChild(editor);
    window.lucide?.createIcons();
    const textarea = editor.querySelector("[data-repo-file-editor-content]");
    const branchInput = editor.querySelector("[data-repo-file-editor-branch]");
    const messageInput = editor.querySelector("[data-repo-file-editor-message]");
    const saveButton = editor.querySelector("[data-repo-file-editor-save]");
    const cancel = () => closeRepoFileEditor(viewer, repo, path, content, lines, { ...meta });
    editor.querySelector("[data-repo-file-editor-cancel]")?.addEventListener("click", cancel, {
      signal: controller.signal,
    });
    editor.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        cancel();
      } else if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "s") {
        event.preventDefault();
        editor.requestSubmit();
      }
    }, { signal: controller.signal });
    editor.querySelector("[data-repo-file-editor-propose]")?.addEventListener("change", (event) => {
      setRepoFileEditorProposing(editor, Boolean(event.target.checked));
      if (event.target.checked && branchInput) branchInput.value = currentBranch;
    }, { signal: controller.signal });
    editor.addEventListener("submit", async (event) => {
      event.preventDefault();
      const nextContent = textarea?.value || "";
      const message = String(messageInput?.value || "").trim();
      const proposing = editor.dataset.repoFileEditorMode === "propose";
      const branch = proposing ? currentBranch : String(branchInput?.value || "").trim();
      saveButton.disabled = true;
      if (proposing) {
        updateRepoFileStatus(viewer, "Preparing your proposed change...", "good");
        let proposal = null;
        try {
          proposal = await repoFileEditRequest(repo, {
            mode: "propose",
            path,
            branch,
            oid: String(meta?.oid || ""),
            content: nextContent,
            message,
          });
          if (proposal.unchanged) {
            saveButton.disabled = false;
            updateRepoFileStatus(viewer, "No changes to propose.", "good");
            return;
          }
          await proposeRepoFileEdit(viewer, repo, proposal);
        } catch (error) {
          saveButton.disabled = false;
          if (!proposal && error?.status === 401) {
            location.href = repoFileSignInUrl();
            return;
          }
          updateRepoFileStatus(viewer, proposal
            ? webPullSubmitErrorText(error, branch, proposal.head, repoDisplayName(repo))
            : repoFileEditError(error), "bad");
        }
        return;
      }
      updateRepoFileStatus(viewer, "Saving file...", "good");
      try {
        const payload = await repoFileEditRequest(repo, {
          mode: "commit",
          path,
          branch,
          baseRef: currentBranch,
          oid: String(meta?.oid || ""),
          content: nextContent,
          message,
        });
        const nextLines = String(nextContent).split(/\r\n|\r|\n/);
        const nextMeta = {
          ...meta,
          content: nextContent,
          oid: String(payload.oid || meta?.oid || ""),
          commitId: String(payload.commit || meta?.commitId || ""),
        };
        closeRepoFileEditor(viewer, repo, path, nextContent, nextLines, nextMeta);
        const shortCommit = String(payload.commit || "").slice(0, 10);
        updateRepoFileStatus(
          viewer,
          payload.unchanged
            ? "No changes to save."
            : `Saved to ${payload.branch || branch}${shortCommit ? ` at ${shortCommit}` : ""}.`,
          "good",
        );
        if (payload.branch) {
          state.selectedBranches[repoKey(repo)] = payload.branch;
        }
      } catch (error) {
        saveButton.disabled = false;
        if (error?.status === 401) {
          location.href = repoFileSignInUrl();
          return;
        }
        if (error?.status === 403 && error?.data?.canPropose) {
          setRepoFileEditorProposing(editor, true, { access: "propose" });
          if (branchInput) branchInput.value = currentBranch;
        }
        updateRepoFileStatus(viewer, repoFileEditError(error), "bad");
      }
    }, { signal: controller.signal });
    editor.dataset.repoFileEditorBase = currentBranch;
    setRepoFileEditorProposing(editor, false);
    void probeRepoFileEditAccess(viewer, editor, repo, controller.signal);
    textarea?.focus();
    updateRepoFileStatus(viewer, "Editing file.", "good");
    if (mode !== viewer.dataset.repoFileCurrentMode) viewer.dataset.repoFileCurrentMode = mode;
  }

  function setRepoFileFullscreenFallback(shell, active) {
    if (!shell) return;
    shell.classList.toggle("fixed", active);
    shell.classList.toggle("inset-3", active);
    shell.classList.toggle("z-50", active);
    shell.classList.toggle("shadow-2xl", active);
    shell.classList.toggle("max-h-[calc(100vh-1.5rem)]", active);
    shell.classList.toggle("overflow-auto", active);
    document.body.classList.toggle("overflow-hidden", active);
    if (active) {
      shell.dataset.repoFileFullscreen = "fallback";
    } else {
      delete shell.dataset.repoFileFullscreen;
    }
  }

  function toggleRepoFileFullscreen(viewer) {
    const shell = viewer?.querySelector("[data-repo-file-shell]");
    if (!shell) return;
    if (shell.dataset.repoFileFullscreen === "fallback") {
      setRepoFileFullscreenFallback(shell, false);
      return;
    }
    if (document.fullscreenElement === shell) {
      document.exitFullscreen?.();
      return;
    }
    if (shell.requestFullscreen) {
      shell.requestFullscreen().catch(() => setRepoFileFullscreenFallback(shell, true));
      return;
    }
    setRepoFileFullscreenFallback(shell, true);
  }

  function bindRepoFileToolbar(viewer, repo, path, content, lines, meta) {
    viewer._repoFileToolbarAbort?.abort();
    const controller = new AbortController();
    viewer._repoFileToolbarAbort = controller;
    viewer.addEventListener("click", async (event) => {
      const modeButton = event.target.closest("button[data-repo-file-mode]");
      if (modeButton && viewer.contains(modeButton)) {
        setRepoFileMode(viewer, modeButton.dataset.repoFileMode || "code", lines, path, meta);
        return;
      }

      const actionButton = event.target.closest("button[data-repo-file-action]");
      if (!actionButton || !viewer.contains(actionButton)) return;
      const action = actionButton.dataset.repoFileAction;
      if (action === "raw") {
        setRepoFileMode(viewer, "raw", lines, path, meta);
      } else if (action === "edit") {
        if (repoReadOnlyMirror(repo)) {
          updateRepoFileStatus(viewer, REPO_READ_ONLY_MIRROR_HINT, "bad");
          return;
        }
        if (!state.session?.nodeName) {
          updateRepoFileStatus(viewer, "Sign in to edit this file.", "bad");
          location.href = repoFileSignInUrl();
          return;
        }
        openRepoFileEditor(viewer, repo, path, content, lines, meta);
      } else if (action === "copy") {
        const copied = await copyTextToClipboard(content);
        if (copied) {
          actionButton.classList.add("text-primary");
          window.setTimeout(() => actionButton.classList.remove("text-primary"), 1200);
          updateRepoFileStatus(viewer, "File contents copied.", "good");
        } else {
          updateRepoFileStatus(viewer, "Could not copy file contents.", "bad");
        }
      } else if (action === "download") {
        downloadRepoFile(path, content);
        updateRepoFileStatus(viewer, `Downloading ${repoFileName(path)}.`, "good");
      } else if (action === "fullscreen") {
        toggleRepoFileFullscreen(viewer);
      }
    }, { signal: controller.signal });

    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      setRepoFileFullscreenFallback(viewer.querySelector("[data-repo-file-shell]"), false);
    }, { signal: controller.signal });
  }

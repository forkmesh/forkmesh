  const RECENT_PUSHES_URL = "/api/me/recent-pushes";
  const RECENT_PUSHES_CACHE_KEY = "forkmesh.recentPushes";
  const RECENT_PUSHES_DISMISSED_KEY = "forkmesh.recentPushes.dismissed";
  const RECENT_PUSHES_FRESH_MS = 60 * 1000;
  const RECENT_PUSHES_WINDOW_MS = 60 * 60 * 1000;
  const RECENT_PUSHES_DISMISS_KEEP_MS = 2 * 24 * 60 * 60 * 1000;
  const RECENT_PUSHES_SHOWN = 3;
  const RECENT_PUSHES_PAGES = ["home", "repos", "repo"];
  const recentPushes = { account: "", items: [], windowMs: RECENT_PUSHES_WINDOW_MS, loading: null };

  function recentPushStore(key, value) {
    try {
      if (value === undefined) return JSON.parse(localStorage.getItem(key) || "null");
      if (value === null) localStorage.removeItem(key);
      else localStorage.setItem(key, JSON.stringify(value));
    } catch (_) {}
    return null;
  }

  function recentPushAccount() {
    const session = state.session || {};
    return session.sessionToken ? String(session.nodeName || "").trim().toLowerCase() : "";
  }

  function recentPushKey(item) {
    return `${item?.id || ""}@${item?.head || ""}`;
  }

  function recentPushPath(value) {
    return String(value || "").replace(/\/+$/, "").toLowerCase();
  }

  function recentPushOnRepo(item, pathname) {
    const here = recentPushPath(pathname);
    return [item.repoUrl, item.targetUrl, `/${item.repo}`, `/${item.target}`]
      .map(recentPushPath)
      .some((base) => base.length > 1 && (here === base || here.startsWith(`${base}/`)));
  }

  function recentPushAgo(pushedAt, now) {
    const minutes = Math.floor(Math.max(0, now - Number(pushedAt || 0)) / 60000);
    if (minutes < 1) return "less than a minute ago";
    if (minutes < 60) return `${minutes} minute${minutes === 1 ? "" : "s"} ago`;
    const hours = Math.floor(minutes / 60);
    return `${hours} hour${hours === 1 ? "" : "s"} ago`;
  }

  function visibleRecentPushes(items, { now, page, pathname, dismissed, windowMs }) {
    if (!RECENT_PUSHES_PAGES.includes(page)) return [];
    const compare = recentPushPath(pathname);
    return (Array.isArray(items) ? items : [])
      .filter((item) => item && item.id && item.branch && item.compareUrl)
      .filter((item) => now - Number(item.pushedAt || 0) <= windowMs)
      .filter((item) => !dismissed?.[recentPushKey(item)])
      .filter((item) => page !== "repo" || recentPushOnRepo(item, pathname))
      .filter((item) => recentPushPath(item.compareUrl) !== compare)
      .sort((a, b) => Number(b.pushedAt || 0) - Number(a.pushedAt || 0))
      .slice(0, RECENT_PUSHES_SHOWN);
  }

  function recentPushBannerHtml(items, now, pathname) {
    return items.map((item) => {
      const when = new Date(Number(item.pushedAt || 0));
      const where = recentPushRepoLabel(item, pathname);
      return `<div data-recent-push="${escapeHtml(recentPushKey(item))}" class="fm-flash fm-flash-warning fm-flash-full" role="status">
          <i data-lucide="git-branch" aria-hidden="true"></i>
          <div class="fm-flash-body">
            <p class="text-foreground"><a href="${escapeHtml(item.branchUrl)}" class="break-all font-mono font-semibold text-accent hover:underline">${escapeHtml(item.branch)}</a>${where} had recent pushes <time datetime="${escapeHtml(when.toISOString())}" title="${escapeHtml(when.toLocaleString())}">${escapeHtml(recentPushAgo(item.pushedAt, now))}</time></p>
          </div>
          <div class="fm-flash-actions">
            <a href="${escapeHtml(`${item.compareUrl}?expand=1`)}" data-recent-push-compare class="fm-btn fm-btn-sm fm-btn-cta"><i data-lucide="git-pull-request" aria-hidden="true"></i>Compare &amp; pull request</a>
            <button type="button" data-recent-push-dismiss="${escapeHtml(recentPushKey(item))}" aria-label="Dismiss ${escapeHtml(item.branch)}" class="fm-btn fm-btn-sm fm-btn-invisible fm-btn-icon"><i data-lucide="x" aria-hidden="true"></i></button>
          </div>
        </div>`;
    }).join("");
  }

  function recentPushRepoLabel(item, pathname) {
    const repo = String(item.repo || "");
    if (!repo || recentPushOnRepo({ ...item, targetUrl: "", target: "" }, pathname)) return "";
    return ` <span class="text-muted-foreground">on <span class="break-all">${escapeHtml(repo)}</span></span>`;
  }

  function renderRecentPushes() {
    const slot = $("[data-recent-pushes]");
    if (!slot) return;
    const account = recentPushAccount();
    const items = account && account === recentPushes.account
      ? visibleRecentPushes(recentPushes.items, {
        now: Date.now(),
        page: currentPage(),
        pathname: location.pathname,
        dismissed: recentPushStore(RECENT_PUSHES_DISMISSED_KEY) || {},
        windowMs: recentPushes.windowMs,
      })
      : [];
    const html = recentPushBannerHtml(items, Date.now(), location.pathname);
    if (slot.dataset.recentPushesHtml === html) return;
    slot.dataset.recentPushesHtml = html;
    slot.innerHTML = html;
    slot.hidden = !items.length;
    if (items.length) window.lucide?.createIcons();
  }

  function rememberRecentPushes(account, items, windowMs) {
    recentPushes.account = account;
    recentPushes.items = Array.isArray(items) ? items : [];
    if (windowMs) recentPushes.windowMs = windowMs;
    recentPushStore(RECENT_PUSHES_CACHE_KEY, {
      account, at: Date.now(), windowMs: recentPushes.windowMs, items: recentPushes.items,
    });
  }

  async function loadRecentPushes() {
    const account = recentPushAccount();
    if (!account || location.protocol === "file:") return;
    if (recentPushes.loading) return recentPushes.loading;
    recentPushes.loading = (async () => {
      try {
        const response = await fetch(RECENT_PUSHES_URL, {
          credentials: "same-origin",
          cache: "no-store",
          headers: sessionHeaders(),
        });
        if (!response.ok) return;
        const data = await response.json();
        if (recentPushAccount() !== account) return;
        const minutes = Number(data?.windowMinutes || 0);
        rememberRecentPushes(account, data?.pushes, minutes > 0 ? minutes * 60000 : 0);
        renderRecentPushes();
      } catch (_) {
      } finally {
        recentPushes.loading = null;
      }
    })();
    return recentPushes.loading;
  }

  function dismissRecentPush(key) {
    const now = Date.now();
    const dismissed = recentPushStore(RECENT_PUSHES_DISMISSED_KEY) || {};
    Object.keys(dismissed).forEach((name) => {
      if (now - Number(dismissed[name] || 0) > RECENT_PUSHES_DISMISS_KEEP_MS) delete dismissed[name];
    });
    dismissed[key] = now;
    recentPushStore(RECENT_PUSHES_DISMISSED_KEY, dismissed);
    renderRecentPushes();
  }

  function forgetRecentPushes(match) {
    const account = recentPushAccount();
    const gone = recentPushes.items.filter(match);
    if (!account || !gone.length) return;
    rememberRecentPushes(account, recentPushes.items.filter((item) => !match(item)));
    renderRecentPushes();
    gone.forEach((item) => {
      void fetch(RECENT_PUSHES_URL, {
        method: "POST",
        credentials: "same-origin",
        headers: sessionHeaders(true),
        body: JSON.stringify({ id: item.id }),
      }).catch(() => {});
    });
  }

  function recentPushRepoMatches(value, repo) {
    const wanted = String(value || "").toLowerCase();
    if (!wanted || !repo) return false;
    return [repoKey(repo), repoDisplayKey(repo)].some((key) => key.toLowerCase() === wanted);
  }

  function noteRecentPushPullOpened(repo, head) {
    const branch = String(head || "").replace(/^[^:]*:/, "");
    forgetRecentPushes((item) => item.branch === branch
      && (recentPushRepoMatches(item.repo, repo) || recentPushRepoMatches(item.target, repo)));
  }

  function noteRecentPushOpenPulls(repo, pulls) {
    const heads = new Set((Array.isArray(pulls) ? pulls : [])
      .filter((pull) => String(pull?.state || pull?.status || "").toLowerCase() === "open")
      .map((pull) => String(pull?.head || pull?.values?.head || "").toLowerCase())
      .filter(Boolean));
    if (!heads.size) return;
    forgetRecentPushes((item) => recentPushRepoMatches(item.target, repo)
      && (heads.has(String(item.branch).toLowerCase())
        || heads.has(`${String(item.repo).split("/")[0]}:${item.branch}`.toLowerCase())));
  }

  function initRecentPushes() {
    const slot = $("[data-recent-pushes]");
    if (!slot) return;
    slot.addEventListener("click", (event) => {
      const dismiss = event.target.closest("[data-recent-push-dismiss]");
      if (dismiss) dismissRecentPush(dismiss.dataset.recentPushDismiss || "");
    });
    const account = recentPushAccount();
    if (!account) recentPushStore(RECENT_PUSHES_CACHE_KEY, null);
    if (!account || !RECENT_PUSHES_PAGES.includes(currentPage())) {
      renderRecentPushes();
      return;
    }
    const cached = recentPushStore(RECENT_PUSHES_CACHE_KEY);
    if (cached && cached.account === account) {
      recentPushes.account = account;
      recentPushes.items = Array.isArray(cached.items) ? cached.items : [];
      recentPushes.windowMs = Number(cached.windowMs) || RECENT_PUSHES_WINDOW_MS;
      renderRecentPushes();
      if (Date.now() - Number(cached.at || 0) < RECENT_PUSHES_FRESH_MS) return;
    }
    const idle = window.requestIdleCallback || ((callback) => window.setTimeout(callback, 1200));
    idle(() => void loadRecentPushes(), { timeout: 4000 });
  }

  function installIncrementalIcons() {
    const lucide = window.lucide;
    if (!lucide?.createIcons || !lucide.icons || !lucide.createElement || lucide.createIcons.incremental) return;
    const svgNs = "http://www.w3.org/2000/svg";
    const iconKey = (name) => name.replace(/(\w)(\w*)(_|-|\s*)/g,
      (_, first, rest) => first.toUpperCase() + rest.toLowerCase());
    const renderIcon = (element) => {
      const name = element.getAttribute("data-lucide");
      const icon = name && lucide.icons[iconKey(name)];
      if (!icon || !element.parentNode) return;
      const [tag, defaults, children] = icon;
      const own = {};
      for (const attribute of Array.from(element.attributes)) own[attribute.name] = attribute.value;
      const classes = ["lucide", `lucide-${name}`, ...String(own.class || "").split(" ")]
        .map((value) => value.trim()).filter(Boolean);
      const attrs = { ...defaults, "data-lucide": name, ...own, class: [...new Set(classes)].join(" ") };
      element.parentNode.replaceChild(lucide.createElement([tag, attrs, children]), element);
    };
    const createIcons = (options) => {
      if (options && Object.keys(options).length) return lucide.createIcons.full(options);
      for (const element of Array.from(document.querySelectorAll("[data-lucide]"))) {
        if (element.namespaceURI === svgNs
          && element.classList.contains(`lucide-${element.getAttribute("data-lucide")}`)) continue;
        renderIcon(element);
      }
    };
    createIcons.full = lucide.createIcons;
    createIcons.incremental = true;
    lucide.createIcons = createIcons;
  }
  installIncrementalIcons();

  function readSession() {
    try {
      return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (_) {
      return null;
    }
  }

  // app/www/world are access layers in front of one public site. Anything a
  // user reads, copies, or shares — a profile, a clone address, a referral
  // link, a fediverse handle, an MCP endpoint — is spelled on the apex
  // forkmesh.com; only routing (repoApiBase, the API origin) names a service
  // subdomain. A self-hosted or local deployment keeps its own origin.
  const OFFICIAL_SERVICE_HOSTS = new Set([
    "app.forkmesh.com",
    "forkmesh.com",
    "world.forkmesh.com",
  ]);

  function publicSiteOrigin() {
    return OFFICIAL_SERVICE_HOSTS.has(location.hostname.toLowerCase())
      ? "https://forkmesh.com"
      : location.origin;
  }

  function signedOutHomeUrl() {
    return `${publicSiteOrigin()}/`;
  }

  const USER_NAME_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
  const USER_NAME_BATCH = 50;

  function userNameKey(name) {
    const key = String(name || "").trim().replace(/^@/, "").toLowerCase();
    return USER_NAME_RE.test(key) ? key : "";
  }

  function rememberUserNameCase(name, nameCase) {
    const key = userNameKey(name);
    const text = String(nameCase || "").trim();
    if (!key || text.toLowerCase() !== key) return false;
    if (!state.userNameCases) state.userNameCases = new Map();
    const changed = state.userNameCases.get(key) !== text;
    state.userNameCases.set(key, text);
    return changed;
  }

  function userDisplayName(name) {
    const text = String(name || "").trim();
    const key = userNameKey(text);
    if (!key) return text;
    const known = state.userNameCases?.get(key);
    if (known) return text.startsWith("@") ? `@${known}` : known;
    return text;
  }

  function userNameHtml(name) {
    const text = String(name || "").trim();
    const key = userNameKey(text);
    if (!key) return escapeHtml(text);
    return `<span data-user-name="${escapeHtml(key)}">${escapeHtml(userDisplayName(text))}</span>`;
  }

  async function loadUserNameCases(names) {
    if (!state.userNameCasesAsked) state.userNameCasesAsked = new Set();
    const wanted = [...new Set((names || []).map(userNameKey).filter(Boolean))]
      .filter((key) => !state.userNameCasesAsked.has(key) && !state.userNameCases?.has(key));
    let changed = false;
    for (let start = 0; start < wanted.length; start += USER_NAME_BATCH) {
      const batch = wanted.slice(start, start + USER_NAME_BATCH);
      batch.forEach((key) => state.userNameCasesAsked.add(key));
      try {
        const response = await fetch(`/api/accounts/name-case?names=${encodeURIComponent(batch.join(","))}`, {
          credentials: "omit",
          headers: { accept: "application/json" },
        });
        const data = response.ok ? await response.json() : null;
        for (const [key, value] of Object.entries(data?.names || {})) {
          if (rememberUserNameCase(key, value)) changed = true;
        }
      } catch (_) {}
    }
    return changed;
  }

  function applyUserNameCases(root) {
    const scope = root || document;
    const nodes = scope.querySelectorAll ? [...scope.querySelectorAll("[data-user-name]")] : [];
    for (const node of nodes) {
      const key = node.getAttribute("data-user-name");
      const known = state.userNameCases?.get(key);
      if (!known) continue;
      const text = String(node.textContent || "");
      const at = text.startsWith("@") ? "@" : "";
      if (text.slice(at.length).toLowerCase() === key && text !== `${at}${known}`) {
        node.textContent = `${at}${known}`;
      }
    }
    return nodes;
  }

  async function hydrateUserNames(root) {
    const nodes = applyUserNameCases(root);
    const keys = nodes.map((node) => node.getAttribute("data-user-name"));
    if (await loadUserNameCases(keys)) applyUserNameCases(root);
  }

  function watchUserNames() {
    if (state.userNameWatch || typeof MutationObserver === "undefined" || !document.body) return;
    let timer = 0;
    state.userNameWatch = new MutationObserver(() => {
      if (timer) return;
      timer = setTimeout(() => {
        timer = 0;
        void hydrateUserNames(document);
      }, 50);
    });
    state.userNameWatch.observe(document.body, { childList: true, subtree: true });
    void hydrateUserNames(document);
  }

  function publicProfileUrl(name, suffix = "") {
    const handle = String(name || "").trim().toLowerCase();
    return handle
      ? `${publicSiteOrigin()}/@${encodeURIComponent(handle)}${suffix}`
      : `${publicSiteOrigin()}/@username${suffix}`;
  }

  // Issue #592: an account whose address nobody has confirmed is held out of
  // the product, so every dashboard document bounces it to the pending screen.
  // Deliberately `=== false` rather than falsy: a stored session written before
  // the flag existed carries no opinion, and hydrateCanonicalProfile settles it
  // from the server a moment later rather than guessing "unverified" here.
  const VERIFY_PENDING_PATH = "/verify-pending";

  function verificationPending(session) {
    return Boolean(session?.email) && session?.emailVerified === false;
  }

  function bounceToVerification(session) {
    if (!verificationPending(session)) return false;
    location.replace(VERIFY_PENDING_PATH);
    return true;
  }

  function normalizeDashboardTheme(theme) {
    return theme === "light" ? "light" : "dark";
  }

  function readDashboardTheme() {
    try {
      const cookieTheme = document.cookie
        .split(";")
        .map((part) => part.trim())
        .find((part) => part.startsWith("forkmesh_theme="))
        ?.slice("forkmesh_theme=".length);
      return normalizeDashboardTheme(
        localStorage.getItem(DASHBOARD_THEME_KEY) ||
        localStorage.getItem("forkmesh.theme") ||
        cookieTheme
      );
    } catch (_) {
      return "dark";
    }
  }

  function renderAppearanceTheme(theme = readDashboardTheme()) {
    const current = normalizeDashboardTheme(theme);
    $$("[data-appearance-theme]").forEach((button) => {
      const active = button.dataset.appearanceTheme === current;
      button.setAttribute("aria-checked", active ? "true" : "false");
      button.classList.toggle("border-border", active);
      button.classList.toggle("bg-secondary", active);
      button.classList.toggle("shadow-sm", active);
      button.querySelector("[data-appearance-check]")?.classList.toggle("hidden", !active);
    });
    const status = $("[data-appearance-theme-status]");
    if (status) {
      status.textContent = current === "light"
        ? "Light mode is active for the dashboard."
        : "Dark mode is active for the dashboard.";
    }
  }

  function applyDashboardTheme(theme) {
    const nextTheme = normalizeDashboardTheme(theme);
    document.documentElement.dataset.dashboardTheme = nextTheme;
    document.documentElement.style.colorScheme = nextTheme;
    document.querySelector('meta[name="theme-color"]')?.setAttribute(
      "content",
      nextTheme === "light" ? "#f6f8fb" : "#090909",
    );
    renderAppearanceTheme(nextTheme);
    return nextTheme;
  }

  function saveDashboardTheme(theme) {
    const nextTheme = normalizeDashboardTheme(theme);
    try {
      localStorage.setItem(DASHBOARD_THEME_KEY, nextTheme);
      localStorage.setItem("forkmesh.theme", nextTheme);
      const sharedDomain = location.hostname === "forkmesh.com" ||
        location.hostname.endsWith(".forkmesh.com")
        ? "; Domain=.forkmesh.com" : "";
      document.cookie = `forkmesh_theme=${nextTheme}; Path=/; Max-Age=31536000; SameSite=Lax${sharedDomain}`
        + (location.protocol === "https:" ? "; Secure" : "");
    } catch (_) {}
    applyDashboardTheme(nextTheme);
  }

  function renderGenieSetting() {
    const toggle = $("[data-genie-visible]");
    if (!toggle) return;
    const genie = window.ForkMeshGenie;
    const ready = typeof genie?.isOff === "function";
    toggle.setAttribute("aria-checked", ready && genie.isOff() ? "false" : "true");
    toggle.disabled = !ready;
  }

  function saveGenieSetting(shown) {
    const genie = window.ForkMeshGenie;
    if (shown) genie?.show?.();
    else genie?.hide?.();
    renderGenieSetting();
  }

  function watchGenieSetting() {
    window.addEventListener("forkmesh:genie", renderGenieSetting);
    if (document.readyState !== "complete") {
      document.addEventListener("DOMContentLoaded", renderGenieSetting, { once: true });
    }
    renderGenieSetting();
  }

  const HOME_REDIRECT_COOKIE = "forkmesh_home";

  function homeRedirectEnabled() {
    let cookie = "";
    try {
      cookie = document.cookie;
    } catch (_) {}
    const entry = String(cookie || "").split(";").map((part) => part.trim())
      .find((part) => part.startsWith(HOME_REDIRECT_COOKIE + "="));
    return !entry || entry.slice(HOME_REDIRECT_COOKIE.length + 1) !== "stay";
  }

  function renderHomeRedirectSetting() {
    const toggle = $("[data-home-redirect]");
    if (!toggle) return;
    toggle.setAttribute("aria-checked", homeRedirectEnabled() ? "true" : "false");
  }

  function saveHomeRedirectSetting(enabled) {
    const sharedDomain = location.hostname === "forkmesh.com" ||
      location.hostname.endsWith(".forkmesh.com")
      ? "; Domain=.forkmesh.com" : "";
    try {
      document.cookie = `${HOME_REDIRECT_COOKIE}=${enabled ? "dashboard" : "stay"}; Path=/; Max-Age=31536000; SameSite=Lax${sharedDomain}`
        + (location.protocol === "https:" ? "; Secure" : "");
    } catch (_) {}
    renderHomeRedirectSetting();
  }

  function watchHomeRedirectSetting() {
    if (document.readyState !== "complete") {
      document.addEventListener("DOMContentLoaded", renderHomeRedirectSetting, { once: true });
    }
    renderHomeRedirectSetting();
  }

  function writeSession(nextSession) {
    const storedSession = nextSession && typeof nextSession === "object"
      ? {
          ...nextSession,
          sessionToken: (
            location.protocol === "https:" &&
            nextSession.sessionToken &&
            window.ForkMeshAPI?.isSameSite !== false
              ? "cookie"
              : nextSession.sessionToken || ""
          ),
        }
      : nextSession;
    state.session = storedSession;
    if (storedSession?.nodeName || storedSession?.email) document.documentElement?.removeAttribute?.("data-guest");
    try {
      localStorage.setItem("forkmesh.session", JSON.stringify(storedSession));
      document.cookie = "forkmesh_session=1; Path=/; Max-Age=2592000; SameSite=Lax"
        + (location.protocol === "https:" ? "; Secure" : "");
    } catch (_) {}
  }

  // Existing browser sessions can predate the portable mirror cookie. Login
  // responses mint it now, but a person who stayed signed in across that
  // rollout would otherwise look logged out on their first direct mirror
  // visit until the next focus revalidation. Refresh the signed, domain-wide
  // grant in the background at most once every four hours (its lifetime is
  // twelve), without holding up App paint or introducing a poll.
  const PORTABLE_SESSION_REFRESH_KEY = "forkmesh.portableSessionRefresh.v2";
  const PORTABLE_SESSION_REFRESH_MS = 4 * 60 * 60 * 1000;
  const PORTABLE_SESSION_RETRY_MS = 5 * 60 * 1000;

  async function ensurePortableControllerSession(force = false) {
    const hostname = String(location.hostname || "").toLowerCase();
    if (hostname !== "forkmesh.com" && !OFFICIAL_SERVICE_HOSTS.has(hostname)) {
      return null;
    }
    const session = readSession();
    const nodeName = String(session?.nodeName || "").trim().toLowerCase();
    if (!nodeName || !session?.sessionToken) return null;

    const now = Date.now();
    let marker = {};
    try {
      marker = JSON.parse(
        localStorage.getItem(PORTABLE_SESSION_REFRESH_KEY) || "{}",
      ) || {};
    } catch (_) {}
    if (
      !force
      && String(marker.nodeName || "").toLowerCase() === nodeName
      && (
        now - Number(marker.refreshedAt || 0) < PORTABLE_SESSION_REFRESH_MS
        || now - Number(marker.attemptedAt || 0) < PORTABLE_SESSION_RETRY_MS
      )
    ) {
      return null;
    }
    const pendingMarker = {
      nodeName,
      refreshedAt: Number(marker.refreshedAt || 0),
      attemptedAt: now,
    };
    try {
      localStorage.setItem(
        PORTABLE_SESSION_REFRESH_KEY, JSON.stringify(pendingMarker),
      );
    } catch (_) {}
    try {
      const token = String(session.sessionToken || "");
      const response = await fetch("/api/accounts/sessions", {
        method: "GET",
        credentials: "same-origin",
        cache: "no-store",
        headers: {
          accept: "application/json",
          ...(token && token !== "cookie"
            ? { authorization: `Bearer ${token}` }
            : {}),
        },
      });
      if (!response.ok) return null;
      // "current" is the relay declining to re-sign a grant this browser was
      // handed within the refresh window; the cookie it holds is still good.
      const portable = response.headers.get("x-forkmesh-portable-session");
      if (portable !== "issued" && portable !== "current") return null;
      const refreshed = { ...pendingMarker, refreshedAt: Date.now() };
      try {
        localStorage.setItem(
          PORTABLE_SESSION_REFRESH_KEY, JSON.stringify(refreshed),
        );
      } catch (_) {}
      return refreshed;
    } catch (_) {
      return null;
    }
  }

  // A portable login is carried between forkmesh.com hosts by the signed,
  // domain-scoped HttpOnly cookie. localStorage cannot cross origins, though,
  // so a mirror opened for the first time would otherwise render as signed
  // out even while its own API already recognizes the user. Hydrate only from
  // the mirror's local session endpoint; the controller hosts already manage
  // their richer account session and must not gain another background request.
  async function hydratePortableMirrorSession() {
    const hostname = String(location.hostname || "").toLowerCase();
    if (hostname === "forkmesh.com" || OFFICIAL_SERVICE_HOSTS.has(hostname)) {
      return null;
    }

    const controller = typeof AbortController === "function"
      ? new AbortController() : null;
    const timeout = window.setTimeout(() => controller?.abort(), 2000);
    try {
      // The page's head script asked already (dashboard/shell.html).
      const check = window.__forkmeshSessionCheck;
      const early = check?.kind === "mirror" ? check.pending : null;
      if (early) window.__forkmeshSessionCheck = null;
      const response = early
        ? await Promise.race([early, new Promise((_, reject) => {
          controller?.signal.addEventListener("abort", () => reject(new Error("timeout")));
        })]).then((result) => {
          if (!result) throw new Error("session_check_failed");
          return { ok: result.ok, status: result.status, json: async () => result.payload };
        })
        : await fetch("/api/accounts/sessions", {
          method: "GET",
          credentials: "same-origin",
          cache: "no-store",
          headers: { Accept: "application/json" },
          signal: controller?.signal,
        });
      if (!response.ok) {
        // localStorage belongs to a previous mirror visit and cannot prove a
        // current login. Remove it when this node rejects the shared cookie so
        // switching accounts on App is reflected on the next direct visit.
        if (response.status === 401 || response.status === 403) {
          state.session = null;
          try { localStorage.removeItem("forkmesh.session"); } catch (_) {}
        }
        return null;
      }
      const payload = await response.json();
      const account = payload?.account;
      const nodeName = String(account?.nodeName || "").trim();
      if (!payload?.portable || !nodeName) return null;

      const hydrated = {
        ...account,
        nodeName,
        sessionToken: "cookie",
        portable: true,
        controllerIndependent: payload.controllerIndependent === true,
      };
      writeSession(hydrated);
      return hydrated;
    } catch (_) {
      return null;
    } finally {
      window.clearTimeout?.(timeout);
    }
  }

  // On App itself the HttpOnly account cookie is the login and this origin's
  // localStorage marker only a copy. The copy can go missing while the cookie
  // is live, and the dashboard then rendered a signed-in browser as a guest
  // (and bounced Settings to /login) even though the homepage, World and the
  // API all still knew the account. Rebuild the copy from the cookie before
  // the chrome decides who is signed in, as WWW and World already do. A guest
  // has no cookie, so the check is a D1-free 401. An answer slower than the
  // budget still lands: the page reloads once into the signed-in view.
  const CONTROLLER_SESSION_RESTORE_BUDGET_MS = 1500;

  async function restoreControllerSession() {
    const hostname = String(location.hostname || "").toLowerCase();
    if (hostname !== "forkmesh.com" && !OFFICIAL_SERVICE_HOSTS.has(hostname)) {
      return null;
    }
    if (readSession()?.nodeName) return null;
    const restore = window.ForkMeshAPI?.restoreSession;
    if (typeof restore !== "function") return null;
    // The page's head script asked already (dashboard/shell.html).
    const check = window.__forkmeshSessionCheck;
    const early = check?.kind === "controller" ? check.pending : null;
    if (early) window.__forkmeshSessionCheck = null;
    const pending = (early || Promise.resolve().then(restore)).catch(() => null);
    let timer = 0;
    const late = Symbol("late");
    const restored = await Promise.race([
      pending,
      new Promise((resolve) => {
        timer = window.setTimeout(() => resolve(late), CONTROLLER_SESSION_RESTORE_BUDGET_MS);
      }),
    ]);
    window.clearTimeout(timer);
    if (restored === late) {
      pending.then((session) => {
        if (session?.nodeName) location.reload();
      });
      return null;
    }
    return restored || null;
  }

  function clearForkMeshBrowserState() {
    const preserved = new Set([
      "forkmesh.analyticsConsent.v1",
      "forkmesh.apiOrigin",
      "forkmesh.dashboard.theme",
      "forkmesh.genie.off",
      "forkmesh.theme",
    ]);
    try {
      for (let index = localStorage.length - 1; index >= 0; index -= 1) {
        const key = localStorage.key(index);
        if (key?.startsWith("forkmesh.") && !preserved.has(key)) {
          localStorage.removeItem(key);
        }
      }
      for (let index = sessionStorage.length - 1; index >= 0; index -= 1) {
        const key = sessionStorage.key(index);
        if (key?.startsWith("forkmesh.")) sessionStorage.removeItem(key);
      }
      state.fetchJsonCache = {};
      state.fetchJsonInflight = {};
      document.cookie = "forkmesh_session=; Path=/; Max-Age=0; SameSite=Lax";
      document.cookie = "forkmesh_account=; Path=/; Max-Age=0; SameSite=Strict";
      document.cookie = "forkmesh_admin=; Path=/; Max-Age=0; SameSite=Lax";
      try {
        new BroadcastChannel("forkmesh.session").postMessage({
          type: "signed-out",
        });
      } catch (_) {}
    } catch (_) {}
    if (!("caches" in window)) return Promise.resolve();
    return caches.keys()
      .then((names) => Promise.all(names.map((name) => caches.delete(name))))
      .catch(() => {});
  }

  function logout() {
    let serverLogout = Promise.resolve();
    try {
      // The Worker owns session invalidation: this clears the HttpOnly
      // forkmesh_admin cookie so the admin page is unreachable after logout.
      // site-header.js (marketing pages) calls the same endpoint.
      serverLogout = fetch("/api/accounts/logout", { method: "POST",
        keepalive: true,
        credentials: "same-origin",
        cache: "no-store",
      }).catch(() => {});
    } catch (_) {}
    const cleanup = clearForkMeshBrowserState();
    let redirected = false;
    const finish = () => {
      if (redirected) return;
      redirected = true;
      location.replace(signedOutHomeUrl());
    };
    Promise.allSettled([serverLogout, cleanup]).then(finish);
    window.setTimeout(finish, 1200);
  }

  // Revalidation floor for the event-driven session check below. A user who
  // alt-tabs repeatedly fires `focus`/`visibilitychange` in bursts, and without
  // a floor that burst is just the 20s poll back under another name.
  const SESSION_REVALIDATE_MIN_MS = 5 * 60 * 1000;

  // Session revocation used to cost a `GET /api/accounts/sessions` every 20
  // seconds per open tab — 180 requests an hour to learn "still signed in",
  // with the whole body thrown away (only the 401 mattered). It was the single
  // most-requested endpoint on the relay.
  //
  // The timer is gone. Liveness now rides the same per-account WebSocket the
  // desktop node and /world already hold (public/account-events.js): every
  // (re)connect trades the stored session for a fresh event ticket, so a
  // revoked session surfaces as an authoritative 401/403 on that handshake for
  // no extra request. Sign-in state is the socket's job, exactly like unread
  // counts (docs/operations/polling-elimination.md).
  //
  // What survives is not a poll: a floor-limited revalidation on focus and
  // visibilitychange, which covers the tab that was hidden while the socket was
  // down, and the cross-tab BroadcastChannel that makes one logout clear the
  // rest immediately.
  function startAccountSessionWatch() {
    let checking = false;
    let lastValidatedMs = Date.now();
    const validate = async (force) => {
      if (checking || !readSession()) return;
      // A hidden tab has nothing to revalidate for, and a check inside the
      // floor is the poll again. Neither applies to `force`: that is the event
      // channel reporting the relay refused this session, which has to boot the
      // tab whether or not anyone is looking at it.
      if (!force &&
          (document.hidden ||
           Date.now() - lastValidatedMs < SESSION_REVALIDATE_MIN_MS)) {
        return;
      }
      checking = true;
      try {
        const session = readSession();
        const token = String(session?.sessionToken || "");
        const response = await fetch("/api/accounts/sessions", {
          headers: {
            accept: "application/json",
            ...(token && token !== "cookie"
              ? { authorization: `Bearer ${token}` }
              : {}),
          },
          credentials: "same-origin",
          cache: "no-store",
        });
        if (response.status === 401) logout();
        // Only an answered request refreshes the floor. A network failure must
        // stay eligible, or an offline stretch would suppress the next real
        // check for five minutes after the connection came back.
        else lastValidatedMs = Date.now();
      } catch (_) {
        // Losing the network is not a logout signal. The next check retries;
        // only an authoritative 401 boots the browser.
      } finally {
        checking = false;
      }
    };
    window.addEventListener("focus", () => void validate(false));
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) void validate(false);
    });
    startAccountEventChannel(() => void validate(true));
    try {
      const channel = new BroadcastChannel("forkmesh.session");
      channel.addEventListener("message", (event) => {
        if (event.data?.type === "signed-out" && readSession()) {
          clearForkMeshBrowserState().finally(() => location.replace(signedOutHomeUrl()));
        }
      });
    } catch (_) {}
  }

  // Join the per-account push channel. The dashboard was the last signed-in
  // surface still asking the relay questions on a timer; chat.js and world.js
  // have held this socket since the unread counts moved to pushes.
  //
  // `onSignedOut` fires when the relay refuses to issue an event ticket for the
  // stored session — the authoritative revocation the 20s poll existed to
  // catch. `revalidate` is the belt-and-braces confirmation: the ticket
  // endpoint and the session endpoint are different routes, so one real
  // signed read decides whether to actually boot the browser.
  //
  // Dynamic import because the bundle is a classic script, not a module. A
  // browser that cannot load it keeps the focus/visibilitychange checks, which
  // is the same coverage the page had before the socket existed.
  function startAccountEventChannel(revalidate) {
    if (state.accountEvents || !readSession()) return;
    let connectedOnce = false;
    import("/account-events.js")
      .then(({ createAccountEventChannel }) => {
        if (state.accountEvents) return;
        state.accountEvents = createAccountEventChannel({
          sessionToken: () => String(readSession()?.sessionToken || ""),
          onSignedOut: revalidate,
          onConnected: () => {
            if (connectedOnce) {
              window.dispatchEvent(new CustomEvent(
                "forkmesh:mirror-signal", { detail: { catchUp: true } },
              ));
              window.dispatchEvent(new CustomEvent("forkmesh:cloud-agents"));
              void checkForDashboardUpdate(true);
            }
            connectedOnce = true;
          },
          onTopic: (topic, frame) => {
            // One read of the surface the topic names, and only when the page
            // actually shows it. No topic here is a "did anything change?"
            // question — the relay only sends one because something did.
            if (topic === "pings" || topic === "issues") {
              if (typeof loadNotifications === "function") {
                void loadNotifications();
              }
            }
            if (topic === "cloud-agents" || topic === "cloud-agents-live") {
              window.dispatchEvent(new CustomEvent("forkmesh:cloud-agents"));
            }
            if (topic === "agents") {
              window.dispatchEvent(new CustomEvent("forkmesh:desktop-agents"));
            }
            if (topic === "catalog") {
              window.dispatchEvent(new CustomEvent(
                "forkmesh:mirror-signal",
                { detail: { repo: String(frame?.repo || "") } },
              ));
            }
            if (topic === "direct-messages") {
              window.dispatchEvent(new CustomEvent("forkmesh:direct-messages"));
            }
          },
        });
        state.accountEvents.start();
      })
      .catch(() => {});
  }

  function escapeHtml(value) {
    return String(value ?? "").replace(/[&<>"']/g, (char) => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      '"': "&quot;",
      "'": "&#039;",
    })[char]);
  }

  // Loading placeholder: spinner (see .fm-spinner in the shell <style>)
  // followed by the label. `label` is inserted as HTML so call sites can keep
  // their pre-escaped fragments.
  function loadingHtml(label) {
    return `<span class="fm-spinner" aria-hidden="true"></span>${label}`;
  }

  // In-flight state for a form's submit button (issue #687): disabled, with a
  // spinner in place of its icon and a progress label, restored verbatim when
  // the request settles. `label` is plain text.
  function setSubmitBusy(button, busy, label) {
    if (!button) return;
    if (busy) {
      if (button._idleHtml === undefined) button._idleHtml = button.innerHTML;
      button.disabled = true;
      button.setAttribute("aria-busy", "true");
      button.innerHTML = loadingHtml(escapeHtml(label));
      return;
    }
    if (button._idleHtml !== undefined) button.innerHTML = button._idleHtml;
    button._idleHtml = undefined;
    button.disabled = false;
    button.removeAttribute("aria-busy");
  }

  // One formatter for the page: building an Intl.NumberFormat per call cost
  // ~12 ms of the repository page's first render (hundreds of counts).
  let countFormatter = null;
  function formatCount(value) {
    const number = Number(value) || 0;
    countFormatter ||= new Intl.NumberFormat();
    return countFormatter.format(number);
  }

  // Compose-identity chip: avatar + username shown next to any box where the
  // session user is about to send content (issues, replies, reviews). Built as
  // an HTML string so it can be dropped straight into the template-literal forms
  // in 06/07; mirrors the initial/avatarPng logic in applyAvatar().
  function composeIdentityHtml(session, verb) {
    const name = String(session?.nodeName || session?.email || "you");
    const initial = escapeHtml((name[0] || "F").toUpperCase());
    const avatarPng = String(session?.avatarPng || "");
    const inner = avatarPng
      ? `<img class="h-full w-full object-cover" src="data:image/png;base64,${avatarPng}" alt="${escapeHtml(name)} avatar" />`
      : `<span class="text-[10px] font-semibold text-muted-foreground">${initial}</span>`;
    const label = verb
      ? `<span class="text-muted-foreground">${escapeHtml(verb)} as</span> <span class="font-semibold text-foreground">${escapeHtml(name)}</span>`
      : `<span class="font-semibold text-foreground">${escapeHtml(name)}</span>`;
    return `<span class="inline-flex min-w-0 items-center gap-2 text-xs">
        <span class="flex h-6 w-6 shrink-0 items-center justify-center overflow-hidden rounded-full border border-border bg-secondary">${inner}</span>
        <span class="min-w-0 truncate">${label}</span>
      </span>`;
  }

  const ISSUE_PERSON_HANDLE_RE = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$/;

  function issuePersonHandle(name) {
    const text = String(name || "").trim().replace(/^@/, "");
    return ISSUE_PERSON_HANDLE_RE.test(text) && !text.includes("--") ? text.toLowerCase() : "";
  }

  function issueActorIsBot(name) {
    const text = String(name || "").trim();
    if (!text) return false;
    if (/\[bot\]$/i.test(text)) return true;
    return typeof repoContributorIsBot === "function" && repoContributorIsBot({ name: text });
  }

  function issueActorPixels(size) {
    const match = /\bh-(\d+(?:\.5)?)\b/.exec(String(size || ""));
    return match ? Math.round(Number(match[1]) * 4) : 28;
  }

  function issuePersonProfileHref(handle) {
    return handle && validNodeName(handle) ? publicProfileUrl(handle) : "";
  }

  function issueActorAvatarHtml(name, author = "", size = "h-7 w-7", agent = false, link = true) {
    const label = String(name || author || (agent ? "Agent" : "unknown"));
    const bot = agent || issueActorIsBot(label);
    const sessionName = String(state.session?.nodeName || state.session?.email || "");
    const ownAvatar = !bot && label.toLowerCase() === sessionName.toLowerCase()
      ? String(state.session?.avatarPng || "") : "";
    const handle = bot ? "" : issuePersonHandle(label);
    const pixels = issueActorPixels(size);
    const inner = bot
      ? `<i data-lucide="bot" class="${pixels <= 24 ? "h-3.5 w-3.5" : pixels <= 32 ? "h-4 w-4" : "h-5 w-5"} text-muted-foreground"></i>`
      : ownAvatar
        ? `<img class="h-full w-full object-cover" src="data:image/png;base64,${escapeHtml(ownAvatar)}" alt="" width="${pixels}" height="${pixels}" decoding="async" />`
        : `<span class="text-[9px] font-bold text-muted-foreground">${escapeHtml((label[0] || "?").toUpperCase())}</span>`;
    const hydrate = handle && !ownAvatar ? ` data-issue-actor-avatar="${escapeHtml(handle)}"` : "";
    if (hydrate) watchIssueActorAvatars();
    const classes = `relative flex ${size} shrink-0 items-center justify-center overflow-hidden rounded-full border border-border bg-secondary`;
    const title = bot ? `${label} (bot)` : label;
    if (handle && link) {
      const href = issuePersonProfileHref(handle);
      return `<a${href ? ` href="${escapeHtml(href)}"` : ""} data-issue-person="${escapeHtml(handle)}" title="${escapeHtml(title)}" aria-label="${escapeHtml(`${label}'s profile`)}"${hydrate} class="${classes} hover:ring-2 hover:ring-[var(--dashboard-link)]">${inner}</a>`;
    }
    return `<span title="${escapeHtml(title)}"${bot ? ' data-issue-actor-bot=""' : ""}${hydrate} class="${classes}">${inner}</span>`;
  }

  function issuePersonNameHtml(name, classes = "font-semibold text-foreground") {
    const label = String(name || "").trim() || "unknown";
    if (issueActorIsBot(label)) return `<span class="${classes}">${escapeHtml(label)}</span>`;
    const handle = issuePersonHandle(label);
    if (!handle) return `<span class="${classes}">${userNameHtml(label)}</span>`;
    watchIssueActorAvatars();
    const href = issuePersonProfileHref(handle);
    return `<a${href ? ` href="${escapeHtml(href)}"` : ""} data-issue-person="${escapeHtml(handle)}" class="${classes} hover:text-[var(--dashboard-link)] hover:underline">${userNameHtml(label)}</a>`;
  }

  const issueActorAccounts = new Map();
  const issueActorAccountsSettled = new Map();
  let issueActorAvatarObserver = null;
  let issueActorAvatarFrame = 0;

  function issueActorPortraitPng(value) {
    const png = String(value || "").trim();
    return png.length <= 400_000 && /^[A-Za-z0-9+/]+={0,2}$/.test(png) ? png : "";
  }

  function issueActorAccount(name) {
    const key = String(name || "").trim().toLowerCase();
    if (!key || !validNodeName(key)) return Promise.resolve({ exists: false, png: "" });
    if (!issueActorAccounts.has(key)) {
      issueActorAccounts.set(key, fetch(`/api/accounts/${encodeURIComponent(key)}`, { headers: { accept: "application/json" } })
        .then((response) => (response.ok ? response.json() : null))
        .then((profile) => ({
          exists: profile?.exists === true,
          png: profile?.exists === true && profile?.profilePrivate !== true
            ? issueActorPortraitPng(profile.avatarPng) : "",
        }))
        .catch(() => ({ exists: false, png: "" }))
        .then((account) => {
          issueActorAccountsSettled.set(key, account);
          return account;
        }));
    }
    return issueActorAccounts.get(key);
  }

  function issueActorPortrait(name) {
    return issueActorAccount(name).then((account) => account.png);
  }

  function issuePersonRoster(handle) {
    if (typeof repoPeopleRosterMatch !== "function") return Promise.resolve(null);
    return repoPeopleRosterMatch(state.selectedRepo, handle).catch(() => null);
  }

  function issuePersonResolve(handle) {
    return Promise.all([issueActorAccount(handle), issuePersonRoster(handle)])
      .then(([account, roster]) => ({ account, roster }));
  }

  function issueActorAvatarImage(avatar, src) {
    const pixels = Math.max(16, Math.round(avatar.getBoundingClientRect?.().width || 0) || 28);
    const image = document.createElement("img");
    image.src = src;
    image.alt = "";
    image.width = pixels;
    image.height = pixels;
    image.loading = "lazy";
    image.decoding = "async";
    image.referrerPolicy = "no-referrer";
    image.className = "absolute inset-0 h-full w-full object-cover";
    image.addEventListener("error", () => image.remove(), { once: true });
    avatar.appendChild(image);
  }

  function hydrateIssuePersonLink(link, found) {
    if (found.account.exists) {
      const href = issuePersonProfileHref(link.dataset.issuePerson);
      if (href) link.setAttribute("href", href);
      return;
    }
    const profile = found.roster?.profile || "";
    if (profile) {
      link.setAttribute("href", profile);
      link.setAttribute("target", "_blank");
      link.setAttribute("rel", "noopener noreferrer nofollow ugc");
      return;
    }
    if (link.hasAttribute("data-issue-mention")) {
      link.replaceWith(document.createTextNode(link.textContent || ""));
      return;
    }
    link.removeAttribute("href");
  }

  function hydrateIssueActorAvatars(root = document) {
    const avatars = [];
    if (root.matches?.("[data-issue-actor-avatar]")) avatars.push(root);
    root.querySelectorAll?.("[data-issue-actor-avatar]").forEach((avatar) => avatars.push(avatar));
    for (const avatar of avatars) {
      if (avatar.dataset.issueActorState) continue;
      avatar.dataset.issueActorState = "loading";
      void issuePersonResolve(avatar.dataset.issueActorAvatar).then((found) => {
        const png = found.account.png;
        const picture = png ? `data:image/png;base64,${png}` : found.roster?.avatarUrl || "";
        avatar.dataset.issueActorState = png ? "image" : picture ? "roster" : "initial";
        if (!picture || !avatar.isConnected) return;
        issueActorAvatarImage(avatar, picture);
      });
    }
    const links = [];
    if (root.matches?.("[data-issue-person]")) links.push(root);
    root.querySelectorAll?.("[data-issue-person]").forEach((link) => links.push(link));
    for (const link of links) {
      if (link.dataset.issuePersonState) continue;
      link.dataset.issuePersonState = "loading";
      void issuePersonResolve(link.dataset.issuePerson).then((found) => {
        link.dataset.issuePersonState = "done";
        if (link.isConnected) hydrateIssuePersonLink(link, found);
      });
    }
  }

  function watchIssueActorAvatars() {
    if (issueActorAvatarObserver || typeof MutationObserver !== "function" || !document.body) return;
    const pending = new Set();
    issueActorAvatarObserver = new MutationObserver((records) => {
      for (const record of records) {
        record.addedNodes.forEach((node) => { if (node.nodeType === 1) pending.add(node); });
      }
      if (!pending.size || issueActorAvatarFrame) return;
      issueActorAvatarFrame = requestAnimationFrame(() => {
        issueActorAvatarFrame = 0;
        const roots = Array.from(pending);
        pending.clear();
        roots.forEach((node) => { if (node.isConnected) hydrateIssueActorAvatars(node); });
      });
    });
    issueActorAvatarObserver.observe(document.body, { childList: true, subtree: true });
    requestAnimationFrame(() => hydrateIssueActorAvatars(document));
  }

  function parseFlexibleDate(value) {
    if (value === undefined || value === null || value === "") return null;
    const numeric = Number(value);
    const date = Number.isFinite(numeric) && numeric > 0
      ? new Date(numeric < 1000000000000 ? numeric * 1000 : numeric)
      : new Date(value);
    return Number.isNaN(date.getTime()) ? null : date;
  }

  function formatDate(value) {
    const date = parseFlexibleDate(value);
    if (!date) return "unknown";
    return date.toLocaleDateString(undefined, {
      month: "short",
      day: "numeric",
      year: "numeric",
    });
  }

  // GitHub-style relative timestamp ("3 days ago") for the repo detail page's
  // commit/file dates - absolute dates there made every mirrored commit look
  // identically stale ("Jul 10, 2026") instead of showing recency at a glance.
  function formatTimeAgo(value) {
    const date = parseFlexibleDate(value);
    if (!date) return "unknown";
    const seconds = Math.max(0, Math.round((Date.now() - date.getTime()) / 1000));
    if (seconds < 45) return "just now";
    const units = [
      ["year", 31536000],
      ["month", 2592000],
      ["week", 604800],
      ["day", 86400],
      ["hour", 3600],
      ["minute", 60],
    ];
    for (const [name, secs] of units) {
      const count = Math.floor(seconds / secs);
      if (count >= 1) return `${count} ${name}${count === 1 ? "" : "s"} ago`;
    }
    return "just now";
  }

  // Keep the web tree quiet and scannable: one monochrome folder glyph and one
  // monochrome file glyph, inheriting the page foreground (black in light
  // mode). The desktop browser can retain its richer per-filetype icon set.
  function fileIconHtml(entry, cls = "h-4 w-4 shrink-0") {
    const isTree = entry?.type === "tree";
    return `<i data-lucide="${isTree ? "folder" : "file"}" aria-hidden="true" class="${cls} text-foreground"></i>`;
  }

  function formatSize(bytes) {
    let value = Number(bytes) || 0;
    const units = ["B", "KB", "MB", "GB", "TB"];
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return `${unit === 0 ? value : value.toFixed(1)} ${units[unit]}`;
  }

  function formatServeSpeed(ms) {
    const value = Number(ms);
    if (!Number.isFinite(value) || value < 0) return "";
    return value < 1000 ? `${Math.round(value)} ms` : `${(value / 1000).toFixed(1)} s`;
  }

  function formatUsd(value) {
    const number = Number(value);
    return `$${(Number.isFinite(number) ? number : 0).toFixed(2)}`;
  }

  function normalizedCount(value) {
    const number = Number(value);
    return Number.isFinite(number) && number >= 0 ? number : null;
  }

  function tabCountLabel(value) {
    const number = normalizedCount(value);
    return number === null ? "..." : formatCount(number);
  }

  // The advertised clone/clean URL is the repository's public address, not the
  // machine currently serving the bytes: cloning forkmesh/forkmesh must keep
  // working after the mirror that answered today goes away.
  function cloneUrl(repo) {
    const link = repoLinkUrl(repo);
    return `${publicSiteOrigin()}${link}${link.split("/").length === 2 ? ".git" : ""}`;
  }

  function repoKey(repo) {
    return `${repo.owner || ""}/${repo.name || ""}`;
  }

  // repoKey substitutes for the halves it is missing, so a repository that is
  // not there still yields the truthy "/" — a key that addresses nothing. Code
  // that routes on a key has to ask this instead of testing the key for
  // emptiness: "/" becomes the hostless URL "//", which the browser rejects as
  // invalid rather than navigating (adhoc #104).
  function isRepoKey(value) {
    const [owner, name] = String(value || "").split("/");
    return Boolean(owner && name);
  }

  // Repository bytes are published by nodes, but the catalog resolves every
  // physical route to the user/organization that actually owns the work
  // (ownerKind / logicalOwner / logicalOwners). Lists must show that identity —
  // "forkmesh/forkmesh", never the serving machine's "mirror8/forkmesh".
  // Routing keys stay physical: repoKey/repoPathUrl/data-dashboard-open-repo
  // must keep matching the catalog rows.
  function repoLogicalOwners(repo) {
    const values = Array.isArray(repo?.logicalOwners) ? repo.logicalOwners : [];
    const owners = values.map((value) => ({
      kind: String(value?.kind || "").trim().toLowerCase(),
      owner: String(value?.owner || "").trim(),
    }));
    if (!owners.length) {
      owners.push({
        kind: String(repo?.ownerKind || "").trim().toLowerCase(),
        owner: String(repo?.logicalOwner || "").trim(),
      });
    }
    return owners.filter((value) =>
      value.owner && (value.kind === "user" || value.kind === "organization"));
  }

  // The public address of a repository, resolved without consulting the
  // account that happens to own the serving node. In order: an organization
  // alias on this row, an organization alias registered on any row of the same
  // mirror group, the identity this row explicitly resolved to, and finally
  // the upstream ForkMesh route the row is a copy of. Every one of these is a
  // routable address, so links may use them; a bare user identity is not (no
  // worker rewrite backs /<user>/<repo>) and is therefore label-only.
  function repoPublicOwner(repo) {
    const organization = repoLogicalOwners(repo)
      .find((value) => value.kind === "organization");
    if (organization) return organization.owner;
    const alias = repoAliasOrganization(repo);
    if (alias) return alias;
    // Only the owner segment moves: a route whose repository name differs from
    // this row's would address a different repository, not a rename of it.
    const name = String(repo?.name || "").trim().toLowerCase();
    const sameName = (value) => String(value || "").trim().toLowerCase() === name;
    const resolved = String(repo?.canonicalOwner || "").trim();
    if (resolved && sameName(repo?.canonicalName)) return resolved;
    const canonical = repoCanonicalIdentity(repo);
    return canonical.owner
      && sameName(canonical.name)
      && canonical.owner !== String(repo?.owner || "").trim()
      ? canonical.owner
      : "";
  }

  // Organization aliases are registered on a single catalog row but they name
  // the whole mirror group, so a row that only carries the publishing node's
  // account still resolves to the organization. Indexed once per catalog
  // snapshot: display and link helpers run inside list render loops.
  let repoAliasIndex = { repositories: null, byKey: new Map() };
  let repoAliasIndexBuilding = false;
  function repoAliasOrganizationIndex() {
    const repositories = Array.isArray(state.repositories) ? state.repositories : [];
    if (repoAliasIndex.repositories === repositories) return repoAliasIndex.byKey;
    if (repoAliasIndexBuilding) return new Map();
    repoAliasIndexBuilding = true;
    const byKey = new Map();
    try {
      for (const group of groupRepositories(repositories)) {
        const organization = groupOrganizationAlias(group);
        if (!organization) continue;
        for (const member of [sourceOfTruth(group), ...(group.members || [])]) {
          for (const key of [repoKey(member), repoCanonicalKey(member)]) {
            const text = String(key || "").trim().toLowerCase();
            if (text.length > 1 && text.includes("/")) byKey.set(text, organization);
          }
        }
      }
    } finally {
      repoAliasIndexBuilding = false;
    }
    repoAliasIndex = { repositories, byKey };
    return byKey;
  }

  function repoAliasOrganization(repo) {
    if (!repo?.name) return "";
    const index = repoAliasOrganizationIndex();
    return index.get(repoKey(repo).toLowerCase())
      || index.get(repoCanonicalKey(repo).toLowerCase())
      || "";
  }

  function repoDisplayOwner(repo) {
    const owners = repoLogicalOwners(repo);
    return repoPublicOwner(repo)
      || owners[0]?.owner
      || String(repo?.owner || "").trim();
  }

  function repoDisplayKey(repo) {
    return `${repoDisplayOwner(repo) || ""}/${repo?.name || ""}`;
  }

  // Mirrors of one logical repository are grouped by root commit, so an
  // organization alias registered on any member names the whole group.
  function groupOrganizationAlias(group) {
    const origin = sourceOfTruth(group);
    const name = String(origin?.name || "").trim().toLowerCase();
    for (const member of [origin, ...(group?.members || [])]) {
      if (String(member?.name || "").trim().toLowerCase() !== name) continue;
      const organization = repoLogicalOwners(member)
        .find((value) => value.kind === "organization");
      if (organization) return organization.owner;
    }
    return "";
  }

  function groupDisplayOwner(group) {
    return groupOrganizationAlias(group) || repoDisplayOwner(sourceOfTruth(group));
  }

  function normalizeRepoSegment(value) {
    const text = String(value || "").trim();
    return /^[A-Za-z0-9._:-]+$/.test(text) ? text : "";
  }

  function safeDecodeURIComponent(value) {
    try {
      return decodeURIComponent(value);
    } catch (_) {
      return String(value || "");
    }
  }

  function parseCloneUrlIdentity(value) {
    const raw = String(value || "").trim();
    if (!raw) return null;
    let path = raw;
    try {
      if (/^[a-z][a-z0-9+.-]*:\/\//i.test(path)) {
        path = new URL(path).pathname;
      } else if (path.startsWith("/") && typeof location !== "undefined") {
        path = new URL(path, location.origin).pathname;
      } else if (path.startsWith("git@") && path.includes(":")) {
        path = path.slice(path.indexOf(":") + 1);
      }
    } catch (_) {}
    path = path.split("?")[0].split("#")[0].replace(/^\/+|\/+$/g, "");
    const parts = path.split("/").filter(Boolean);
    const addresses = parts.length === 1 && isThisSiteUrl(raw) ? repoAddresses() : null;
    if (addresses) parts.unshift(addresses.owner);
    if (parts.length < 2) return null;
    const owner = normalizeRepoSegment(safeDecodeURIComponent(parts[parts.length - 2] || ""));
    let name = normalizeRepoSegment(safeDecodeURIComponent(parts[parts.length - 1] || ""));
    if (name.toLowerCase().endsWith(".git")) {
      name = normalizeRepoSegment(name.slice(0, -4));
    }
    return owner && name ? { owner, name } : null;
  }

  // forkmesh.com and app.forkmesh.com are the same deployment, so a clone URL
  // on either host names a route of this site. Comparing bare origins missed
  // the sibling host, which is how a copy advertising
  // https://forkmesh.com/forkmesh/forkmesh still read as its own repository.
  function isThisSiteUrl(value) {
    const bare = (host) => String(host || "").toLowerCase().replace(/^(?:www|app)\./, "");
    const raw = String(value || "").trim();
    if (!raw) return false;
    try {
      const url = new URL(raw, location.origin);
      return url.origin === location.origin
        || (Boolean(url.hostname) && bare(url.hostname) === bare(location.hostname));
    } catch (_) {
      return false;
    }
  }

  function repoCanonicalIdentity(repo) {
    // A fork deliberately retains its parent's clone URL as its upstream.
    // That transport address must never become an identity alias: otherwise a
    // lookup for alice/project could resolve bob's fork solely because bob's
    // catalog row says it was cloned from alice/project.
    if (repoForkParent(repo)) {
      return {
        owner: String(repo?.owner || "").trim(),
        name: String(repo?.name || "").trim(),
      };
    }
    const source = String(repo?.source || "").trim();
    const parsed = parseCloneUrlIdentity(repo?.cloneUrl);
    // A locally served copy can still be a clone of another ForkMesh route.
    // Keep the upstream's canonical name in that case; otherwise a serving
    // node such as jett would relabel forkmesh/forkmesh as jett/forkmesh.
    const isThisForkMeshOrigin = isThisSiteUrl(repo?.cloneUrl);
    if (parsed && (
      source === "remote-clone"
      || (source === "local-node" && isThisForkMeshOrigin)
    )) {
      return parsed;
    }
    return {
      owner: String(repo?.owner || "").trim(),
      name: String(repo?.name || "").trim(),
    };
  }

  function repoCanonicalKey(repo) {
    const id = repoCanonicalIdentity(repo);
    return `${id.owner || ""}/${id.name || ""}`;
  }

  function repoAliasKeys(members, canonical) {
    const aliases = new Set();
    const add = (key) => {
      const text = String(key || "").trim().toLowerCase();
      if (text && text.includes("/")) aliases.add(text);
    };
    add(repoKey(canonical));
    add(repoCanonicalKey(canonical));
    (members || []).forEach((member) => {
      add(repoKey(member));
      add(repoCanonicalKey(member));
    });
    return [...aliases];
  }

  function canonicalRepoFromGroup(primary, members) {
    const all = Array.isArray(members) ? members : [];
    const local = all.find((m) => (m.source || "").trim() === "local-node");
    if (local) {
      return { ...local, _repoAliases: repoAliasKeys(all, local) };
    }
    const mirror = all.find((m) =>
      (m.source || "").trim() === "remote-clone" && parseCloneUrlIdentity(m.cloneUrl));
    const base = mirror || primary || all[0] || {};
    // A fork intentionally keeps its source URL pointed at the parent for
    // upstream pulls. That URL is not the fork's identity; treating it as one
    // redirected /bob/project straight back to /alice/project and erased the
    // provenance the PR composer needs.
    const id = repoForkParent(base)
      ? { owner: base.owner || "", name: base.name || "" }
      : repoCanonicalIdentity(base);
    const canonical = {
      ...base,
      owner: id.owner || base.owner || "",
      name: id.name || base.name || "",
      canonicalOwner: id.owner || "",
      canonicalName: id.name || "",
      servingOwner: base.owner || "",
      servingName: base.name || "",
    };
    if ((base.owner || "") !== canonical.owner || (base.name || "") !== canonical.name) {
      canonical.liveHost = false;
      canonical.cloneOnline = Boolean(base.cloneOnline ?? base.liveHost);
    }
    canonical._repoAliases = repoAliasKeys(all, canonical);
    return canonical;
  }

  function repoMatchesKey(repo, key) {
    const wanted = String(key || "").trim().toLowerCase();
    if (!wanted) return false;
    if (repoKey(repo).toLowerCase() === wanted || repoCanonicalKey(repo).toLowerCase() === wanted) {
      return true;
    }
    return Array.isArray(repo?._repoAliases) && repo._repoAliases.includes(wanted);
  }

  // A fork publishes the public identity of its parent in its signed catalog
  // row. Keep parsing in one place: this value controls both where a pull
  // request is delivered and which repository the composer names as its base.
  function repoForkParent(repo) {
    const value = String(repo?.forkedFrom || "").trim().toLowerCase();
    if (!value || value.length > 161 || value.indexOf("/") !== value.lastIndexOf("/")) {
      return null;
    }
    const [rawOwner, rawName] = value.split("/");
    const owner = normalizeRepoSegment(rawOwner);
    const name = normalizeRepoSegment(rawName);
    if (!owner || !name || owner.length > 80 || name.length > 80) return null;
    // Do not call repoPublicOwner here: alias resolution itself groups repos
    // and would recurse back through repoGroupKey.
    const currentOwner = String(repo?.canonicalOwner || repo?.owner || "").trim().toLowerCase();
    const currentName = String(repo?.name || "").trim().toLowerCase();
    if (owner === currentOwner && name === currentName) return null;
    return { owner, name, key: `${owner}/${name}` };
  }

  function repoMirrorOf(repo) {
    const value = String(repo?.mirrorOf || "").trim();
    return /^https?:\/\/[^\s"'<>]+$/i.test(value) ? value : "";
  }

  function repoReadOnlyMirror(repo) {
    return Boolean(repoMirrorOf(repo)) || repo?.readOnly === true;
  }

  function repoMirrorUpstreamPath(url) {
    try {
      const parsed = new URL(url);
      const parts = parsed.pathname.split("/").filter(Boolean);
      return parts.length >= 2 ? `${parsed.host.replace(/^app\./i, "")}/${parts[0]}/${parts[1].replace(/\.git$/i, "")}` : "";
    } catch (_) {
      return "";
    }
  }

  function repoDescriptionText(...repos) {
    for (const repo of repos) {
      const text = String(repo?.description || "").trim();
      if (text) return text;
    }
    for (const repo of repos) {
      const mirrorOf = repoMirrorOf(repo);
      if (mirrorOf) return `Read-only mirror of ${repoMirrorUpstreamPath(mirrorOf) || mirrorOf}`;
    }
    return "";
  }

  function repoMirrorSyncedAt(repo) {
    const value = Number(repo?.lastSyncedAt || 0);
    return Number.isFinite(value) && value > 0 ? value : 0;
  }

  const REPO_READ_ONLY_MIRROR_HINT = "This repository is a read-only mirror. Fork it to make changes.";

  function rememberRepoMirrorAbout(repo, body) {
    if (!repo || !body || typeof body !== "object") return false;
    const before = `${repoMirrorOf(repo)}|${repoReadOnlyMirror(repo)}|${repoMirrorSyncedAt(repo)}|${repo.upstreamCommit || ""}`;
    if ("mirrorOf" in body) repo.mirrorOf = String(body.mirrorOf || "");
    if ("readOnly" in body) repo.readOnly = body.readOnly === true;
    if (Number(body.lastSyncedAt) > 0) repo.lastSyncedAt = Number(body.lastSyncedAt);
    if (body.upstreamCommit) repo.upstreamCommit = String(body.upstreamCommit);
    return before !== `${repoMirrorOf(repo)}|${repoReadOnlyMirror(repo)}|${repoMirrorSyncedAt(repo)}|${repo.upstreamCommit || ""}`;
  }

  function repoProvenanceHtml(repo, { forkButton = true } = {}) {
    const mirrorOf = repoMirrorOf(repo);
    const parent = mirrorOf ? null : repoForkParent(repo);
    if (!mirrorOf && !parent) return "";
    const linkClass = "dashboard-accent-link min-w-0 truncate font-medium hover:underline";
    if (!mirrorOf) {
      const contribute = `<button type="button" data-repo-contribute title="Open a pull request from this fork to ${escapeHtml(repoDisplayName(parent.key))}" class="fm-btn fm-btn-sm shrink-0"><i data-lucide="git-pull-request"></i>Contribute: Open pull request</button>`;
      return `<span data-repo-provenance-line="fork" class="inline-flex min-w-0 items-center gap-1.5"><i data-lucide="git-fork" aria-hidden="true" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><span class="shrink-0">forked from</span><a data-repo-provenance-link href="${escapeHtml(repoLinkUrl({ owner: parent.owner, name: parent.name }))}" class="${linkClass}">${escapeHtml(repoDisplayName(parent.key))}</a></span>${contribute}`;
    }
    const syncedAt = repoMirrorSyncedAt(repo);
    const commit = String(repo?.upstreamCommit || "").trim();
    const syncedTitle = [
      syncedAt ? new Date(syncedAt).toLocaleString() : "",
      commit ? `serving ${commit.slice(0, 12)}` : "",
    ].filter(Boolean).join(" · ");
    const synced = syncedAt
      ? `<span data-repo-mirror-synced${syncedTitle ? ` title="${escapeHtml(syncedTitle)}"` : ""} class="shrink-0 text-muted-foreground">· synced ${escapeHtml(formatTimeAgo(syncedAt))}</span>`
      : "";
    const notice = `<span data-repo-mirror-notice class="min-w-0 text-[11px] text-muted-foreground">Read-only. Send changes <a href="${escapeHtml(mirrorOf)}" target="_blank" rel="noopener" class="dashboard-accent-link hover:underline">upstream</a> or into a fork.</span>`;
    const fork = forkButton ? `<button type="button" data-repo-mirror-fork title="${escapeHtml(REPO_READ_ONLY_MIRROR_HINT)}" class="fm-btn fm-btn-sm shrink-0"><i data-lucide="git-fork"></i>Fork</button>` : "";
    const line = `<span data-repo-provenance-line="mirror" class="inline-flex min-w-0 items-center gap-1.5"><i data-lucide="refresh-cw" aria-hidden="true" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><span class="shrink-0">Mirror of</span><a data-repo-provenance-link href="${escapeHtml(mirrorOf)}" target="_blank" rel="noopener" class="${linkClass}">${escapeHtml(repoMirrorUpstreamPath(mirrorOf) || mirrorOf)}</a>${synced}</span>`;
    return `${line}${notice}${fork}`;
  }

  function repoGroupKey(repo) {
    // A fork can have the same root commit as its parent for its entire useful
    // lifetime. Folding solely by rootCommit therefore made a freshly-created
    // fork disappear into the parent repository. Its signed provenance is an
    // explicit boundary; mirrors without that extension keep the legacy
    // content-addressed grouping behavior.
    const parent = repoForkParent(repo);
    if (parent) {
      // Different serving nodes publish the same fork under their physical
      // node names. Group those rows by the owner identity the relay attached
      // to them, while keeping unrelated forks with the same root separate.
      const logicalOwners = repoLogicalOwners(repo);
      const logicalOwner = logicalOwners.find((value) => value.kind === "organization")?.owner
        || logicalOwners[0]?.owner
        || repo?.owner
        || "";
      const owner = normalizeRepoSegment(logicalOwner).toLowerCase();
      const name = normalizeRepoSegment(repo?.name || "").toLowerCase();
      return `fork:${owner}/${name}:from:${parent.key}`;
    }
    const root = (repo?.rootCommit || "").trim();
    return root ? "root:" + root : "name:" + (repo?.name || "").trim().toLowerCase();
  }

  function groupRepositories(repos) {
    const groups = new Map();
    for (const repo of repos) {
      const key = repoGroupKey(repo);
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(repo);
    }
    // Fold empty-root name-keyed mirrors into rooted groups (same logic as worker).
    const rootedByName = new Map();
    for (const [key, members] of groups) {
      if (!key.startsWith("root:")) continue;
      for (const m of members) {
        const n = (m.name || "").trim().toLowerCase();
        if (n && !rootedByName.has(n)) rootedByName.set(n, key);
      }
    }
    for (const [key, members] of [...groups]) {
      if (!key.startsWith("name:")) continue;
      const target = rootedByName.get(key.slice("name:".length));
      if (target && groups.has(target)) {
        groups.get(target).push(...members);
        groups.delete(key);
      }
    }
    return [...groups.values()].map((members) => {
      const sorted = [...members].sort((a, b) => {
        if (!!b.liveHost !== !!a.liveHost) return (b.liveHost ? 1 : 0) - (a.liveHost ? 1 : 0);
        return (b.lastSync || 0) > (a.lastSync || 0) ? 1 : -1;
      });
      const group = { primary: sorted[0], members: sorted };
      group.source = canonicalRepoFromGroup(group.primary, group.members);
      return group;
    });
  }

  function sourceOfTruth(group) {
    const canonical = group.source || canonicalRepoFromGroup(group.primary, group.members);
    return (canonical.owner || canonical.name)
      ? canonical
      : group.members.find((m) => (m.source || "").trim() === "local-node") || group.primary;
  }

  // A page that reads "forkmesh/forkmesh" must also FETCH it there, by the
  // same rule that already governs its links (repoLinkUrl, adhoc #132): no
  // request the site makes should ever name a mirror. `repo.owner` is
  // whichever node published the catalog row we happened to resolve, and
  // sourceOfTruth prefers a source-of-truth row over a live one — so for the
  // flagship it picks a long-retired node whose row still says
  // source:"local-node" while its cloneUrl points at forkmesh/forkmesh. Only
  // that one node ever served /api/repo/<node>/<repo> (a mirror gateway
  // serves exactly its configured owner/name pairs), so every read answered
  // 503 "mirror_unavailable" while healthy mirrors served the identical bytes
  // under the public address. Which mirror actually answers is decided behind
  // that address by the Worker, which rewrites /api/repo/<org>/<repo>/... onto
  // a serving node; a clone URL a node publishes is likewise a real address.
  function repoApiBase(repo) {
    const owner = repoPublicOwner(repo) || repo.owner || "";
    return `/api/repo/${encodeURIComponent(owner)}/${encodeURIComponent(repo.name || "")}`;
  }

  function repositoryViewKey(repo) {
    const owner = repoPublicOwner(repo) || repo?.owner || "";
    return `${owner}/${repo?.name || ""}`.trim().toLowerCase();
  }

  const DIRECT_MIRROR_READ_OPERATIONS = new Set([
    "tree", "blobs", "blob", "raw", "history", "commit", "compare",
    "branches", "search", "stats", "sizes", "release-blob", "merge-preview",
    "about", "mirrors",
  ]);

  // Return direct HTTPS origins for the healthy copies of this repository.
  // The visible route remains /<public-owner>/<repo>; only data requests move
  // off the canonical Worker. A mirror URL contributes its ORIGIN only: the
  // public owner below remains the address the direct gateway is configured to
  // serve (for example forkmesh/forkmesh, never mirror37/forkmesh).
  function directMirrorOriginsForRepo(repo) {
    if (!repo || repo.isPrivate) return [];
    let members = [repo];
    const wanted = new Set([
      repoKey(repo).toLowerCase(),
      repoCanonicalKey(repo).toLowerCase(),
      ...(Array.isArray(repo._repoAliases) ? repo._repoAliases : []),
    ]);
    for (const group of groupRepositories(state.repositories || [])) {
      const candidates = [sourceOfTruth(group), ...(group.members || [])];
      if (candidates.some((item) =>
        wanted.has(repoKey(item).toLowerCase())
        || wanted.has(repoCanonicalKey(item).toLowerCase())
        || (Array.isArray(item?._repoAliases)
          && item._repoAliases.some((key) => wanted.has(String(key).toLowerCase()))))) {
        members = candidates;
        break;
      }
    }
    const origins = [];
    [...members].sort((a, b) =>
      Number(b?.lastSync || b?.updatedAt || 0)
      - Number(a?.lastSync || a?.updatedAt || 0)).forEach((member) => {
      if (member?.cloneOnline === false || member?.liveHost === false) return;
      const address = String(
        member?.mirrorUrl || member?.directUrl || member?.dashboardUrl || "",
      ).trim();
      try {
        const parsed = new URL(address);
        if (parsed.protocol !== "https:" || parsed.username || parsed.password) return;
        if (!origins.includes(parsed.origin)) origins.push(parsed.origin);
      } catch (_) { /* malformed catalog rows are not routing candidates */ }
    });
    return origins;
  }

  function dashboardOnInstance() {
    return Boolean(document?.querySelector?.("script[data-instance-brand]"));
  }

  function platformName() {
    const brand = document?.querySelector?.("script[data-instance-brand]");
    return String(brand?.dataset?.instanceName || "").trim() || "ForkMesh";
  }

  function desktopBrand() {
    return dashboardOnInstance() ? "" : "ForkMesh ";
  }

  function applyPlatformName(root = document) {
    const name = platformName();
    root?.querySelectorAll?.("[data-platform-name]").forEach((element) => {
      if (element.textContent !== name) element.textContent = name;
    });
    const desktop = desktopBrand();
    root?.querySelectorAll?.("[data-desktop-brand]").forEach((element) => {
      if (element.textContent !== desktop) element.textContent = desktop;
    });
  }

  // Dialogs no page shows on load are not in the page documents
  // (dashboard_bundle.py DEFERRED_MARKUP): the first open adds their markup,
  // then the caller finds and opens the dialog as before.
  function ensureDeferredMarkup(name) {
    const spec = deferredMarkup(name);
    if (!spec || !document.body || document.querySelector(spec.marker)) return;
    const template = document.createElement("template");
    template.innerHTML = spec.html;
    const added = [...template.content.children];
    const anchor = spec.after ? document.querySelector(spec.after) : null;
    if (anchor) anchor.after(template.content);
    else document.body.append(template.content);
    added.forEach((element) => applyPlatformName(element));
    window.lucide?.createIcons();
  }

  let instanceInfoRequest = null;
  let instanceGitNode = "";

  // GET /api/instance is the same public document for every visitor. One
  // request per page serves every reader: an instance page starts it from
  // the first inline script (tools/apply_instance_brand.py), and
  // instance-brand.js and genie-spider.js share the same window promise, so
  // whichever asks first sends it and the rest wait on that answer.
  function sharedInstanceDocument() {
    let pending = window.__forkmeshInstanceInfo;
    if (!pending || typeof pending.then !== "function") {
      pending = fetch("/api/instance", { credentials: "omit", headers: { accept: "application/json" } })
        .then((response) => (response.ok ? response.json() : null))
        .catch(() => null);
      window.__forkmeshInstanceInfo = pending;
    }
    return pending;
  }

  function instanceInfo() {
    if (!dashboardOnInstance()) return Promise.resolve(null);
    if (!instanceInfoRequest) {
      instanceInfoRequest = sharedInstanceDocument()
        .then((info) => {
          instanceGitNode = String(info?.gitNode || "").toLowerCase();
          return info;
        })
        .catch(() => null);
    }
    return instanceInfoRequest;
  }

  function platformAccount(name) {
    const value = String(name || "").trim().toLowerCase();
    return Boolean(value) && dashboardOnInstance() && value === instanceGitNode;
  }

  function shownAccountName(name) {
    return platformAccount(name) ? platformName() : String(name || "");
  }

  async function instancePluginEnabled(id) {
    if (!dashboardOnInstance()) return true;
    const info = await instanceInfo();
    const plugins = Array.isArray(info?.plugins) ? info.plugins : [];
    return plugins.some((plugin) => plugin?.id === id && plugin.enabled);
  }

  const DIRECT_MIRROR_ORIGINS_KEY = "forkmesh.directMirrorOrigins.v1";

  function preconnectDirectMirrorOrigins(origins) {
    if (typeof document?.createElement !== "function") return;
    [...new Set(origins || [])].slice(0, 3).forEach((origin) => {
      let parsed;
      try { parsed = new URL(origin); } catch (_) { return; }
      if (parsed.protocol !== "https:" || parsed.origin === location.origin) return;
      if (document.querySelector(`link[data-forkmesh-mirror-origin="${parsed.origin}"]`)) return;
      const link = document.createElement("link");
      link.rel = "preconnect";
      link.href = parsed.origin;
      link.crossOrigin = "anonymous";
      link.dataset.forkmeshMirrorOrigin = parsed.origin;
      document.head?.appendChild(link);
    });
  }

  function rememberAndPreconnectDirectMirrors(repositories) {
    const origins = [];
    [...(repositories || [])]
      .filter((repo) => repo?.liveHost !== false && repo?.cloneOnline !== false)
      .sort((left, right) =>
        Number(right?.lastSync || right?.updatedAt || 0)
        - Number(left?.lastSync || left?.updatedAt || 0))
      .forEach((repo) => {
        const address = String(
          repo?.mirrorUrl || repo?.directUrl || repo?.dashboardUrl || "",
        ).trim();
        try {
          const parsed = new URL(address);
          if (parsed.protocol === "https:" && !origins.includes(parsed.origin)) {
            origins.push(parsed.origin);
          }
        } catch (_) {}
      });
    const selected = origins.slice(0, 3);
    try {
      localStorage.setItem(DIRECT_MIRROR_ORIGINS_KEY, JSON.stringify(selected));
    } catch (_) {}
    preconnectDirectMirrorOrigins(selected);
  }

  function preconnectRememberedDirectMirrors() {
    try {
      const origins = JSON.parse(
        localStorage.getItem(DIRECT_MIRROR_ORIGINS_KEY) || "[]",
      );
      if (Array.isArray(origins)) preconnectDirectMirrorOrigins(origins);
    } catch (_) {}
  }

  function directRepoRequestCandidates(path, { mutation = false } = {}) {
    let request;
    try { request = new URL(path, location.origin); } catch (_) { return []; }
    if (request.origin !== location.origin) return [];
    const match = /^\/api\/repo\/([^/]+)\/([^/]+)\/([^/]+)$/.exec(request.pathname);
    if (!match) return [];
    const operation = decodeURIComponent(match[3]);
    if (!mutation && !DIRECT_MIRROR_READ_OPERATIONS.has(operation)) return [];
    if (mutation && !["issues", "pulls", "discussions"].includes(operation)) return [];
    let owner;
    let name;
    try {
      owner = decodeURIComponent(match[1]);
      name = decodeURIComponent(match[2]);
    } catch (_) { return []; }
    const repo = findRepository(`${owner}/${name}`) || state.selectedRepo;
    if (!repo || repo.isPrivate) return [];
    // This origin reads the repository from its own node (ForkMesh Next's
    // core nodes hold every public repository locally), so a mirror origin
    // is only an extra connection: stay same-origin for reads.
    if (!mutation && (state.localRepoReads || globalThis.__forkmeshEarly?.local)) return [];
    // Routing-only fields belong to the canonical Worker. Direct gateways
    // accept only the operation's own Git query and ignore account/view hints.
    ["fmv", "viewer", "ts", "sig", "_"].forEach((key) =>
      request.searchParams.delete(key));
    return directMirrorOriginsForRepo(repo)
      // A page already opened on a mirror should stay on that independent
      // node first. The App origin is never a mirror candidate, so its visible
      // URL still fans directly to the freshest HTTPS node.
      .sort((left, right) =>
        Number(right === location.origin) - Number(left === location.origin))
      .map((origin) => `${origin}${request.pathname}${request.search}`);
  }

  function repoDataVersion(repo) {
    return String(
      repo?.updatedAt || repo?.lastSync || repo?.commit || repo?.stateHash || "",
    ).trim();
  }

  const REPO_ADDRESSES_STORE = "forkmesh.repoAddresses";
  const REPO_CORE_HOSTS = ["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"];
  const REPO_SHORT_NAME_RE = /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/;
  const REPO_SHORT_FILE_RE = /\.(?:php\d?|txt|xml|json|ico|png|jpe?g|gif|svg|js|mjs|css|map|env|html?|aspx?|jsp|cgi|zip|gz|tgz|tar|sql|bak|ya?ml|ini|log|webmanifest|woff2?|ttf|pdf)$/i;

  function normalizeRepoAddresses(value) {
    const owner = String(value?.owner || "").trim().toLowerCase();
    if (!value || value.canonical !== true || !owner) return null;
    const names = (list) => new Set((Array.isArray(list) ? list : []).map((name) => String(name || "").toLowerCase()));
    const aliases = new Map(Object.entries(value.aliases && typeof value.aliases === "object" ? value.aliases : {})
      .map(([name, creator]) => [String(name).toLowerCase(), String(creator || "").toLowerCase()]));
    return { owner, keep: names(value.keep), reserved: names(value.reserved), aliases };
  }

  const REPO_ADDRESSES_SAVED_STORE = "forkmesh.repoAddressesSavedAt";
  const REPO_ADDRESSES_TTL_MS = 10 * 60 * 1000;

  function repoAddressesSignature(addresses) {
    if (!addresses) return "";
    return JSON.stringify([
      addresses.owner,
      [...addresses.keep].sort(),
      [...addresses.reserved].sort(),
      [...addresses.aliases.entries()].sort(),
    ]);
  }

  function repoAddresses() {
    if (state.repoAddresses !== undefined) return state.repoAddresses;
    let stored = null;
    try {
      stored = JSON.parse(localStorage.getItem(REPO_ADDRESSES_STORE) || "null");
    } catch (_) {}
    let addresses = normalizeRepoAddresses(stored);
    const pageOwner = String(document.querySelector('meta[name="forkmesh-repo-canonical"]')?.content || "").toLowerCase();
    if (pageOwner && addresses?.owner !== pageOwner) {
      addresses = normalizeRepoAddresses({ owner: pageOwner, canonical: true });
    }
    state.repoAddresses = addresses;
    return addresses;
  }

  function applyRepoAddresses(value) {
    const used = state.repoAddresses !== undefined;
    const before = used ? repoAddressesSignature(state.repoAddresses) : "";
    state.repoAddresses = normalizeRepoAddresses(value);
    try {
      localStorage.setItem(REPO_ADDRESSES_STORE, JSON.stringify(value || null));
      localStorage.setItem(REPO_ADDRESSES_SAVED_STORE, String(Date.now()));
    } catch (_) {}
    if (used && before !== repoAddressesSignature(state.repoAddresses)) {
      state.repoAddressesChanged = true;
      try {
        window.dispatchEvent(new CustomEvent("forkmesh:repo-addresses"));
      } catch (_) {}
    }
    return state.repoAddresses;
  }

  async function loadRepoAddresses() {
    if (REPO_CORE_HOSTS.includes(location.hostname.toLowerCase())) return null;
    try {
      const data = await sharedInstanceDocument();
      if (data && typeof data === "object" && data.ok !== false) return applyRepoAddresses(data.repoAddresses || null);
    } catch (_) {}
    return repoAddresses();
  }

  function repoAddressesKnown() {
    try {
      if (localStorage.getItem(REPO_ADDRESSES_STORE) === null) return false;
      const savedAt = Number(localStorage.getItem(REPO_ADDRESSES_SAVED_STORE)) || 0;
      const age = Date.now() - savedAt;
      return age >= 0 && age < REPO_ADDRESSES_TTL_MS;
    } catch (_) {
      return false;
    }
  }

  function repoShortName(owner, name) {
    const addresses = repoAddresses();
    const value = String(name || "");
    const lowered = value.toLowerCase();
    const lowerOwner = String(owner || "").toLowerCase();
    if (!addresses || !value || !lowerOwner) return false;
    if (lowerOwner !== addresses.owner && addresses.aliases.get(lowered) !== lowerOwner) return false;
    if (lowered === addresses.owner || addresses.keep.has(lowered) || addresses.reserved.has(lowered)) return false;
    return REPO_SHORT_NAME_RE.test(value) && !lowered.endsWith(".git") && !REPO_SHORT_FILE_RE.test(value);
  }

  function repoPathUrl(repo, kind = "tree", path = "") {
    const owner = encodeURIComponent(repo.owner || "");
    const name = encodeURIComponent(repo.name || "");
    const suffix = path ? `/${kind}/${path.split("/").map(encodeURIComponent).join("/")}` : "";
    let publicOwner = "";
    try {
      publicOwner = repoPublicOwner(repo);
    } catch (_) {}
    if (repoShortName(repo.owner, repo.name) || (publicOwner && repoShortName(publicOwner, repo.name))) {
      if (!state.repoShortNames) state.repoShortNames = new Map();
      state.repoShortNames.set(String(repo.name).toLowerCase(), { name: String(repo.name), owner: repoAddresses().owner });
      return `/${name}${suffix}`;
    }
    return `/${owner}/${name}${suffix}`;
  }

  // A list that reads "forkmesh/forkmesh" must also link there, not to the
  // machine that happens to publish the bytes (adhoc #132). Organization
  // aliases are real addresses: the Worker rewrites /<org>/<repo> and every
  // /api/repo/<org>/<repo>/... path onto the serving node before routing, and
  // a direct visit resolves the alias again on the repo page. A clone URL a
  // node publishes is likewise a real address. Which mirror actually answers
  // is decided behind that address, so no link on the site should ever name a
  // mirror. User identities have no such rewrite, so a repo whose logical
  // owner is only a user keeps the physical /<node>/<repo> route.
  function repoLinkUrl(repo, kind = "tree", path = "") {
    const owner = repoPublicOwner(repo);
    return repoPathUrl(owner ? { ...repo, owner } : repo, kind, path);
  }

  function groupLinkUrl(group, kind = "tree", path = "") {
    const origin = sourceOfTruth(group);
    const organization = groupOrganizationAlias(group);
    return organization
      ? repoPathUrl({ ...origin, owner: organization }, kind, path)
      : repoLinkUrl(origin, kind, path);
  }

  // Feature-tab route segments (mirrors 404.html's `featureTabs` list) - tells
  // a tab route (e.g. /owner/repo/issues) apart from a tree/blob code deep link.
  const REPO_TAB_ROUTES = ["branches", "commits", "insights", "sizemap", "releases", "issues", "projects", "pulls", "discussions", "mirrors", "actions", "security"];
  const REPO_ROUTE_SEGMENTS = [...REPO_TAB_ROUTES, "pull", "agents", "settings", "tree", "blob", "compare"];

  // Owner/admin Agents and owner-only Settings are only recognized for an
  // account that can actually see them. A non-owner deep link falls through
  // to the harmless tree/blob path, so neither private control surface is
  // addressable merely by guessing its URL.
  function repoTabRoutesFor(repo) {
    const routes = REPO_TAB_ROUTES.slice();
    if (repo && sessionCanAssignAgent(repo)) routes.push("agents");
    if (repo && sessionAdministersRepo(repo)) routes.push("settings");
    return routes;
  }

  // Splits the current path into segments, stripping a leading /dashboard
  // route prefix (e.g. /dashboard/owner/repo/issues -> ["owner","repo","issues"])
  // so repo-route parsing sees the same clean owner/repo/tab shape regardless
  // of whether the browser landed here directly or via the 404.html bounce.
  function repoPublicHref(href) {
    const value = String(href || "");
    if (!value.startsWith("/") || value.startsWith("//")) return value;
    const [path, ...tail] = value.split(/(?=[?#])/);
    const parts = path.split("/").filter(Boolean);
    if (parts.length < 2) return value;
    const owner = safeDecodeURIComponent(parts[0]);
    const name = safeDecodeURIComponent(parts[1]);
    const rest = parts.slice(2);
    if (rest.length && !REPO_ROUTE_SEGMENTS.includes(rest[0])) return value;
    if (!repoShortName(owner, name)) return value;
    return [repoPathUrl({ owner, name }), ...rest].join("/") + tail.join("");
  }

  function repoPublicKey(key) {
    const [owner, name, ...extra] = String(key || "").split("/");
    return owner && name && !extra.length && repoShortName(owner, name) ? name : String(key || "");
  }

  function repoHasShortAddress(repo) {
    const name = String(repo?.name || "");
    if (!name || !repoAddresses()) return false;
    const owners = [repo.owner];
    try {
      owners.push(repoPublicOwner(repo), repoDisplayOwner(repo));
    } catch (_) {}
    return owners.some((owner) => owner && repoShortName(owner, name));
  }

  function repoDisplayName(repo, longLabel) {
    if (typeof repo === "string") return repoPublicKey(repo);
    if (!repo) return "";
    if (repoHasShortAddress(repo)) return String(repo.name);
    return longLabel === undefined ? repoDisplayKey(repo) : String(longLabel);
  }

  function repoPublicText(text) {
    const value = String(text || "");
    if (!value || !repoAddresses()) return value;
    return value.replace(/(^|[^A-Za-z0-9._\/@-])([A-Za-z0-9_-][A-Za-z0-9._-]*)\/([A-Za-z0-9_-][A-Za-z0-9._-]*)(?![A-Za-z0-9_\/-])/g,
      (match, lead, owner, name) => {
        const trimmed = name.replace(/\.+$/, "");
        const dots = name.slice(trimmed.length);
        return repoShortName(owner, trimmed) ? `${lead}${trimmed}${dots}` : match;
      });
  }

  function repoReferenceLabel(repo, number) {
    const label = repoDisplayName(repo);
    return Number(number) ? `${label}#${Number(number)}` : label;
  }

  function groupDisplayName(group) {
    const origin = sourceOfTruth(group);
    const name = String(origin?.name || "");
    const organization = groupOrganizationAlias(group);
    if (repoHasShortAddress(origin) || (organization && repoShortName(organization, name))) return name;
    return `${groupDisplayOwner(group) || origin?.owner || ""}/${name}`;
  }

  function repoLabelParts(label) {
    const text = String(label || "");
    const slash = text.indexOf("/");
    return slash < 0 ? { owner: "", name: text } : { owner: text.slice(0, slash), name: text.slice(slash + 1) };
  }

  function repoShortHrefTarget(href) {
    const value = String(href || "");
    if (!value.startsWith("/") || value.startsWith("//")) return value;
    const shortened = repoPublicHref(value);
    if (shortened !== value) return shortened;
    const [path, ...tail] = value.split(/(?=[?#])/);
    const parts = path.split("/").filter(Boolean);
    if (!parts.length) return value;
    const known = state.repoShortNames?.get(safeDecodeURIComponent(parts[0]).toLowerCase());
    if (!known || repoShortName(known.owner, known.name)) return value;
    const rest = parts.slice(1);
    return `${repoPathUrl({ owner: known.owner, name: known.name })}${rest.length ? `/${rest.join("/")}` : ""}${tail.join("")}`;
  }

  function refreshRepoLinks(root) {
    const scope = root || document;
    let changed = 0;
    for (const link of scope.querySelectorAll ? scope.querySelectorAll("a[href^='/']") : []) {
      const href = link.getAttribute("href");
      const target = repoShortHrefTarget(href);
      if (target !== href) {
        link.setAttribute("href", target);
        changed += 1;
      }
    }
    return changed;
  }

  function replaceLongRepoLocation() {
    const current = `${location.pathname}${location.search}${location.hash}`;
    const target = repoShortHrefTarget(current);
    if (target === current) return false;
    try {
      window.history.replaceState(window.history.state, "", target);
      return true;
    } catch (_) {
      return false;
    }
  }

  function repoShortRoute(first = "") {
    const name = document.querySelector('meta[name="forkmesh-repo-short"]')?.content || "";
    const owner = document.querySelector('meta[name="forkmesh-repo-owner"]')?.content || "";
    if (name && owner && (!first || first.toLowerCase() === name.toLowerCase())) return { name, owner };
    const known = first ? state.repoShortNames?.get(first.toLowerCase()) : null;
    if (known && repoShortName(known.owner, known.name)) return known;
    return name && owner ? { name, owner } : null;
  }

  function repoRouteParts() {
    let parts = location.pathname.split("/").filter(Boolean);
    const prefixed = parts[0] === "dashboard";
    if (prefixed) parts = parts.slice(1);
    const first = parts.length ? safeDecodeURIComponent(parts[0]) : "";
    const short = prefixed ? null : repoShortRoute(first);
    if (short && parts.length && first.toLowerCase() === short.name.toLowerCase()) {
      const rest = parts.slice(1);
      const routed = !rest.length || REPO_ROUTE_SEGMENTS.includes(rest[0]);
      parts = [encodeURIComponent(short.owner), encodeURIComponent(short.name), ...(routed ? rest : ["tree", ...rest])];
    }
    return parts[2] === "pull" ? [...parts.slice(0, 2), "pulls", ...parts.slice(3)] : parts;
  }

  function requestedRepoKey() {
    const params = new URLSearchParams(location.search);
    const value = params.get("repo");
    if (value && value.includes("/")) return decodeURIComponent(value);
    const parts = repoRouteParts();
    if (parts.length >= 2 && !["api", "assets", "dashboard", "docs", "blogs", "blog", "login", "signup", "network", "desktop", "about", "careers", "changelog", "privacy", "terms"].includes(parts[0])) {
      return `${decodeURIComponent(parts[0])}/${decodeURIComponent(parts[1])}`;
    }
    return "";
  }

  // Pull-detail sections are addressable in their own right
  // (/owner/repo/pull/93/files) so a link to the Files changed review opens
  // there for whoever it is sent to. Conversation is the default and stays on
  // the bare record URL.
  const REPO_RECORD_SECTIONS = [
    "conversation", "commits", "checks", "files", "badge",
  ];

  function normalizeRepoRecordSection(value) {
    const section = String(value || "").trim().toLowerCase();
    return REPO_RECORD_SECTIONS.includes(section) ? section : "conversation";
  }

  // repoLinkUrl, not repoPathUrl: a record link addresses the repository the
  // page names (an organization alias), never the mirror publishing the bytes.
  function repoCommitUrl(repo, hash) {
    return `${repoLinkUrl(repo)}/commits/${encodeURIComponent(String(hash || ""))}`;
  }

  function repoRecordUrl(repo, kind, number, section) {
    const url = `${repoLinkUrl(repo)}/${kind === "pulls" ? "pull" : kind}/${number}`;
    // Only pulls have sections; an issue/discussion URL never grows a suffix.
    const suffix = kind === "pulls" ? normalizeRepoRecordSection(section) : "conversation";
    return suffix === "conversation" ? url : `${url}/${suffix}`;
  }

  // A branch comparison is addressable too (/owner/repo/compare/main...agent/x):
  // the Agents tab links an agent's branch here so its work can be read in the
  // web diff viewer before any pull request exists, and the address survives a
  // refresh or a paste. Branch names keep their slashes - git forbids ".." in a
  // ref name, so the first "..." is always the separator - and each segment is
  // percent-encoded so a name cannot smuggle a query or fragment.
  function repoCompareUrl(repo, base, head) {
    const encodeRef = (ref) => String(ref || "")
      .split("/").map(encodeURIComponent).join("/");
    return `${repoLinkUrl(repo)}/compare/${encodeRef(base)}...${encodeRef(head)}`;
  }

  // The inverse of repoCompareUrl over the route's path segments: the segments
  // after "compare" are joined back with "/" and split on the first "...".
  function parseRepoCompareRoute(parts) {
    if (!Array.isArray(parts) || parts[2] !== "compare" || parts.length < 4) return null;
    const spec = parts.slice(3).map(safeDecodeURIComponent).join("/");
    const separator = spec.indexOf("...");
    if (separator <= 0) return null;
    const base = spec.slice(0, separator).trim();
    const head = spec.slice(separator + 3).trim();
    if (!base || !head || base.length > 250 || head.length > 250) return null;
    return { base, head };
  }

  function splitRepoCodeRef(kind, parts, refs) {
    const names = new Set((refs || []).map((ref) => String(ref || "").trim()).filter(Boolean));
    const segments = (parts || []).map((part) => String(part || "")).filter(Boolean);
    const longest = segments.length - (kind === "blob" ? 1 : 0);
    for (let size = longest; size > 0; size -= 1) {
      const ref = segments.slice(0, size).join("/");
      if (names.has(ref)) return { ref, path: segments.slice(size).join("/") };
    }
    return null;
  }

  function repoCompareRouteOptions(search) {
    const params = new URLSearchParams(String(search || ""));
    const title = String(params.get("title") || "").replace(/\s+/g, " ").trim().slice(0, 240);
    return { expand: params.get("expand") === "1", title };
  }

  // Pushes a new history entry for real in-app navigations (opening a repo,
  // browsing into a folder/file, leaving a repo for another section) so the
  // browser Back/Forward buttons step through them one at a time instead of
  // exiting the app entirely. No-ops when already on that URL (e.g. while
  // restoring state from a popstate event) to avoid piling up duplicate
  // entries that would otherwise make Back a no-op.
  function navigateHistory(url) {
    if (`${location.pathname}${location.search}` === url) return;
    window.history.pushState(null, "", url);
  }

  function cloneJson(value) {
    if (typeof structuredClone === "function") return structuredClone(value);
    return JSON.parse(JSON.stringify(value));
  }

  function fetchJsonCacheTtl(path) {
    if (path === "/api/repositories") return 15000;
    if (path === "/api/network/overview") return 20000;
    if (path === "/api/version") return 60000;
    return 0;
  }

  function cacheBustedPath(path) {
    const url = new URL(path, location.origin);
    url.searchParams.set("_", String(Date.now()));
    return `${url.pathname}${url.search}${url.hash}`;
  }

  // ForkMesh's own QR generator (no third-party library). It is staged onto
  // this origin from world/public/qr.js by tools/build_site_assets.py rather
  // than bundled here, so the pages that never draw a code - almost all of
  // them - never pay for it. One loader is shared by every caller, so the
  // script is fetched at most once no matter how many surfaces want a code.
  let qrGeneratorLoader = null;
  function loadQrGenerator() {
    if (window.ForkMeshQR) return Promise.resolve(true);
    if (qrGeneratorLoader) return qrGeneratorLoader;
    qrGeneratorLoader = new Promise((resolve) => {
      const script = document.createElement("script");
      script.src = "/qr.js";
      script.onload = () => resolve(Boolean(window.ForkMeshQR));
      script.onerror = () => resolve(false);
      document.head.append(script);
    });
    return qrGeneratorLoader;
  }

  // A wedged Worker isolate fails a request in a few milliseconds - a 500
  // from Pyodide's "NoGilError" or a "hung" cancel - before any route runs,
  // and the next request usually lands on a healthy isolate. fetchJson only
  // reads, so it retries those blips instead of surfacing them. 429 is left
  // alone: retrying a rate limit only deepens it.
  const FETCH_JSON_RETRY_DELAYS_MS = [400, 1500];

  function fetchJsonTransientStatus(status) {
    return status === 0 || status === 408 || status === 425 || status >= 500;
  }

  // One-shot: the catalog fetched early by the repository page, if any.
  async function takeEarlyPublicRead() {
    const early = globalThis.__forkmeshEarly;
    const pending = early?.catalog;
    if (!pending) return null;
    early.catalog = null;
    const data = await pending;
    return data && typeof data === "object" && data.ok !== false ? data : null;
  }

  // One-shot: a JSON read the repository page started before this script ran
  // (partials/repo-early-reads.html early.json), taken over by exact path. It
  // was sent with the page's cookies and no bearer token, so it only stands in
  // for a request that would carry the same identity.
  async function takeEarlyJsonRead(path) {
    const early = globalThis.__forkmeshEarly;
    const pending = early?.json?.[path];
    if (!pending) return null;
    delete early.json[path];
    const data = await pending;
    return data && typeof data === "object" && data.ok !== false ? data : null;
  }

  async function fetchJson(path, options = {}) {
    const fresh = options.fresh === true;
    const cacheBust = options.cacheBust !== false;
    const baseTtl = fetchJsonCacheTtl(path);
    const ttl = fresh ? 0 : baseTtl;
    if (!state.fetchJsonInflight) state.fetchJsonInflight = {};
    if (!state.fetchJsonCache) state.fetchJsonCache = {};
    if (fresh) delete state.fetchJsonCache[path];
    const now = Date.now();
    const cached = ttl ? state.fetchJsonCache[path] : null;
    if (cached && cached.expiresAt > now) return cloneJson(cached.data);
    const requestPath = fresh && cacheBust ? cacheBustedPath(path) : path;
    const inflightKey = fresh ? `${requestPath}#fresh` : path;
    if (state.fetchJsonInflight[inflightKey]) {
      return cloneJson(await state.fetchJsonInflight[inflightKey]);
    }

    const pending = (async () => {
      // Only explicit fresh loads bypass HTTP caching; plain fetches send no
      // cache-buster and no cache-control override so the browser can reuse
      // responses within the server's max-age (request budget).
      const noStore = fresh;
      // Attach the account session as a bearer token when logged in. Endpoints
      // that expose per-account data (e.g. /api/notifications) require it;
      // public endpoints simply ignore it. Same-origin only.
      const token = state.session?.sessionToken || "";
      // The public catalog has a prebuilt privacy-validated snapshot. Sending
      // cookies or the local "cookie" token marker makes the App ingress
      // deliberately bypass that fast path and rebuild the live catalog. Keep
      // this one public read anonymous; account/private APIs remain credentialed.
      const anonymousPublicRead = path === "/api/repositories";
      const headers = noStore
        ? { accept: "application/json", "cache-control": "no-cache" }
        : { accept: "application/json" };
      if (!anonymousPublicRead && token && token !== "cookie") {
        headers.authorization = "Bearer " + token;
      }
      // The repository page starts the catalog read before this script runs
      // (partials/views/repo.html); take that answer over instead of asking
      // again.
      let data = anonymousPublicRead && !noStore ? await takeEarlyPublicRead() : null;
      if (!data && !noStore && !anonymousPublicRead && (!token || token === "cookie")) {
        data = await takeEarlyJsonRead(path);
      }
      if (!data) for (let attempt = 0; ; attempt += 1) {
        const last = attempt >= FETCH_JSON_RETRY_DELAYS_MS.length;
        let response;
        try {
          response = await fetch(requestPath, {
            cache: noStore ? "no-store" : "default",
            credentials: anonymousPublicRead ? "omit" : "same-origin",
            headers,
          });
        } catch (cause) {
          if (last) throw cause;
        }
        if (response) {
          data = await response.json().catch(() => ({}));
          if (response.ok && data.ok !== false) break;
          if (last || !fetchJsonTransientStatus(response.status)) {
            const error = new Error(data.error || `HTTP ${response.status}`);
            error.status = response.status;
            throw error;
          }
        }
        await new Promise((resolve) => setTimeout(resolve, FETCH_JSON_RETRY_DELAYS_MS[attempt]));
      }
      if (ttl || (fresh && baseTtl)) {
        state.fetchJsonCache[path] = {
          data: cloneJson(data),
          expiresAt: Date.now() + (ttl || baseTtl),
        };
      }
      return data;
    })();
    state.fetchJsonInflight[inflightKey] = pending;
    try {
      return cloneJson(await pending);
    } finally {
      if (state.fetchJsonInflight[inflightKey] === pending) delete state.fetchJsonInflight[inflightKey];
    }
  }

  // ---- Web issue authoring (signed inbox submissions) ------------------------
  // A logged-in web user files an issue the same way a mirror node does: a signed
  // "open" event POSTed to the repo's inbox for the maintainer to drain. The
  // browser holds no desktop key, so it keeps a persistent Ed25519 identity of
  // its own; the logged-in node name rides along as authorName for display.
  const ISSUE_TEXT_ENCODER = new TextEncoder();
  const WEB_ISSUE_KEY_STORAGE = "forkmesh.issueKey";
  // Attached images are embedded as base64 data: URLs inside the issue body
  // text itself (no separate upload channel), so they share the body's 64 KB
  // server-side cap (MAX_ISSUE_BYTES in the worker). Kept well under that so a
  // couple of small screenshots plus title/description text still fit.
  const ISSUE_IMAGE_MAX_COUNT = 4;
  const ISSUE_IMAGE_MAX_BYTES = 40 * 1024;
  const ISSUE_IMAGE_MAX_TOTAL_BYTES = 45 * 1024;
  const ISSUE_IMAGE_FIT_OPTIONS = { maxDimension: 1024 };
  // Raw files can be much bigger than the final embedded size - anything under
  // this is accepted into the crop/compress modal rather than rejected outright.
  const ISSUE_IMAGE_RAW_MAX_BYTES = 20 * 1024 * 1024;
  const PROFILE_AVATAR_MAX_BYTES = 256 * 1024;
  const PROFILE_AVATAR_RAW_MAX_BYTES = 20 * 1024 * 1024;
  // Screenshots pasted/attached onto a "start agent" prompt (adhoc #78). More
  // generous than issue images so a screenshot stays legible for the agent, but
  // still bounded to keep the queued-prompt row (and /api/sync payload) modest;
  // mirrors the worker's MAX_AGENT_PROMPT_IMAGE(S)* caps.
  const AGENT_IMAGE_MAX_COUNT = 3;
  const AGENT_IMAGE_MAX_BYTES = 1000 * 1024;
  const AGENT_IMAGE_MAX_TOTAL_BYTES = 1700 * 1024;

  function readAsDataUrl(fileOrBlob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result || ""));
      reader.onerror = () => reject(reader.error || new Error("read_failed"));
      reader.readAsDataURL(fileOrBlob);
    });
  }

  // Intermediary crop/compress step for images too large to embed directly.
  // The user drags a crop box, then the modal auto-retries JPEG re-encoding at
  // shrinking quality/scale ("multiple takes") until the result fits maxBytes,
  // or lets the user redraw a smaller crop and retry. Resolves to
  // { dataUrl, size } on confirm, or null if cancelled.

  const PULL_RULE_KINDS = [
    { kind: "tests", label: "Testing: tests accompany code changes" },
    { kind: "docs", label: "Documentation: docs accompany code changes" },
    { kind: "size", label: "Code quality: pull request size limit" },
    { kind: "commits", label: "Code quality: commit message format" },
    { kind: "secrets", label: "Security: no secrets in the change" },
    { kind: "paths", label: "Security: protected paths" },
    { kind: "model", label: `Required AI model (signed by a ${platformName()} runner)` },
  ];
  const PULL_RULE_MODELS = [
    "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5", "claude-fable-5",
    "claude-haiku-4-5", "claude-sonnet-4-6", "claude-opus-4-8",
    "gpt-5.6-sol", "gpt-5.6-luna", "gpt-5.6-terra", "gpt-5.3-codex-spark",
  ];
  const PULL_RULE_INPUT = "h-8 w-full rounded-md border border-border bg-secondary px-2 text-xs text-foreground outline-none placeholder:text-muted-foreground focus:border-primary/70";
  const PULL_RULE_LABEL = "grid gap-1 text-[11px] font-medium text-foreground";

  function pullRuleList(value) {
    return Array.isArray(value) ? value.map((item) => String(item)).join(", ") : "";
  }

  function pullRuleSplit(value, separator = /,/) {
    return String(value || "").split(separator).map((item) => item.trim()).filter(Boolean);
  }

  function pullRuleRowHtml(rule, models, editable) {
    const kind = PULL_RULE_KINDS.some((item) => item.kind === rule.kind) ? rule.kind : "tests";
    const disabled = editable ? "" : " disabled";
    const field = (name, label, value, placeholder = "", type = "text") =>
      `<label class="${PULL_RULE_LABEL}">${escapeHtml(label)}<input data-pull-rule-field="${name}" type="${type}" value="${escapeHtml(value ?? "")}" placeholder="${escapeHtml(placeholder)}" class="${PULL_RULE_INPUT}"${type === "number" ? ' min="0"' : ""}${disabled} /></label>`;
    const group = (name, html) =>
      `<div data-pull-rule-kind-fields="${name}" class="${name === kind ? "" : "hidden "}grid gap-2 sm:grid-cols-2">${html}</div>`;
    const chosen = new Set(Array.isArray(rule.models) ? rule.models : []);
    const modelBoxes = [...new Set([...models, ...chosen])].map((model) =>
      `<label class="inline-flex items-center gap-1.5 rounded-md border border-border px-2 py-1 font-mono text-[11px] text-foreground"><input type="checkbox" data-pull-rule-model value="${escapeHtml(model)}"${chosen.has(model) ? " checked" : ""}${disabled} />${escapeHtml(model)}</label>`).join("");
    return `
      <fieldset data-pull-rule-row data-pull-rule-id="${escapeHtml(rule.id || "")}" class="grid gap-2 rounded-md border border-border p-3">
        <div class="grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto_auto_auto]">
          <label class="${PULL_RULE_LABEL}">Rule
            <select data-pull-rule-kind class="${PULL_RULE_INPUT}"${disabled}>${PULL_RULE_KINDS.map((item) => `<option value="${item.kind}"${item.kind === kind ? " selected" : ""}>${escapeHtml(item.label)}</option>`).join("")}</select>
          </label>
          <label class="${PULL_RULE_LABEL}">When it fails
            <select data-pull-rule-field="enforcement" class="${PULL_RULE_INPUT}"${disabled}>
              <option value="block"${rule.enforcement !== "warn" ? " selected" : ""}>Block the merge</option>
              <option value="warn"${rule.enforcement === "warn" ? " selected" : ""}>Flag only</option>
            </select>
          </label>
          ${editable ? `<button type="button" data-pull-rule-remove class="fm-btn fm-btn-sm fm-btn-danger self-end"><i data-lucide="trash-2" aria-hidden="true"></i>Remove</button>` : ""}
        </div>
        <div class="grid gap-2 sm:grid-cols-2">
          ${field("title", "Title (optional)", rule.title, "Shown on the pull request")}
          ${field("branches", "Branches (empty = every branch)", pullRuleList(rule.branches), "main, release/*")}
        </div>
        ${group("tests", field("paths", "Code that needs tests", pullRuleList(rule.kind === "tests" ? rule.paths : []), "*.go, *.py, src/**") + field("testPaths", "Test files", pullRuleList(rule.testPaths), "**/tests/**, *_test.*"))}
        ${group("docs", field("paths", "Code that needs docs", pullRuleList(rule.kind === "docs" ? rule.paths : []), "src/**, api/**") + field("docsPaths", "Documentation files", pullRuleList(rule.docsPaths), "**/docs/**, *.md"))}
        ${group("size", field("maxFiles", "Most files changed", rule.maxFiles || "", "50", "number") + field("maxLines", "Most lines changed", rule.maxLines || "", "800", "number"))}
        ${group("commits", field("subjectPattern", "Subject must match (regular expression)", rule.subjectPattern, "^(feat|fix|docs|chore)(\\(.+\\))?: ") + field("maxSubjectLength", "Longest subject", rule.maxSubjectLength || "", "72", "number"))}
        ${group("secrets", `<label class="${PULL_RULE_LABEL} sm:col-span-2">Extra secret patterns, one regular expression per line (API keys, private keys and common tokens are always checked)<textarea data-pull-rule-field="patterns" rows="2" class="w-full rounded-md border border-border bg-secondary px-2 py-1 font-mono text-xs text-foreground outline-none"${disabled}>${escapeHtml((rule.patterns || []).join("\n"))}</textarea></label>` + field("paths", "Only scan these paths (empty = everything)", pullRuleList(rule.kind === "secrets" ? rule.paths : [])))}
        ${group("paths", field("paths", "Protected paths", pullRuleList(rule.kind === "paths" ? rule.paths : []), ".github/workflows/**, deploy/**"))}
        ${group("model", `<div class="grid gap-1 text-[11px] font-medium text-foreground sm:col-span-2">Allowed models<div class="flex flex-wrap gap-1.5">${modelBoxes}</div><span class="font-normal leading-4 text-muted-foreground">Every commit must carry a ForkMesh-Attestation that a ${escapeHtml(platformName())} runner signed under a router-issued grant naming one of these models. Commits written by hand, on a desktop or on a personal runner fail this rule; clean merges of the base branch pass.</span></div>`)}
        <label class="${PULL_RULE_LABEL}">How to fix it (optional, shown when the rule fails)
          <textarea data-pull-rule-field="suggestion" rows="2" class="w-full rounded-md border border-border bg-secondary px-2 py-1 text-xs text-foreground outline-none"${disabled}>${escapeHtml(rule.suggestion || "")}</textarea>
        </label>
      </fieldset>`;
  }

  function pullRulesEditorHtml(policy, options = {}) {
    const editable = options.editable !== false;
    const models = Array.isArray(options.models) && options.models.length ? options.models : PULL_RULE_MODELS;
    const rules = Array.isArray(policy?.rules) ? policy.rules : [];
    return `
      <form data-pull-rules-form class="grid gap-3">
        <div data-pull-rules-rows class="grid gap-3">${rules.map((rule) => pullRuleRowHtml(rule, models, editable)).join("") || `<div data-pull-rules-empty class="fm-blankslate"><i data-lucide="git-pull-request" aria-hidden="true"></i><p class="fm-blankslate-title">No rules yet</p><p class="fm-blankslate-desc">Add a rule to check each pull request.</p></div>`}</div>
        ${editable ? `
        <div class="flex flex-wrap items-center justify-between gap-2">
          <button type="button" data-pull-rule-add class="fm-btn fm-btn-sm"><i data-lucide="plus" aria-hidden="true"></i>Add rule</button>
          <div class="flex flex-wrap items-center gap-2">
            <span data-pull-rules-status class="text-xs text-muted-foreground"></span>
            <button type="submit" class="fm-btn fm-btn-sm fm-btn-cta">${escapeHtml(options.saveLabel || "Save rules")}</button>
          </div>
        </div>` : ""}
      </form>`;
  }

  function readPullRuleRow(row, kept, taken) {
    const kind = row.querySelector("[data-pull-rule-kind]")?.value || "tests";
    const fields = row.querySelector(`[data-pull-rule-kind-fields="${kind}"]`);
    const value = (scope, name) => String(scope?.querySelector(`[data-pull-rule-field="${name}"]`)?.value || "").trim();
    let id = kept;
    if (!id) {
      let suffix = 1;
      id = kind;
      while (taken.has(id)) id = `${kind}-${++suffix}`;
      taken.add(id);
    }
    const rule = { id, kind };
    const title = value(row, "title");
    if (title) rule.title = title;
    if (value(row, "enforcement") === "warn") rule.enforcement = "warn";
    const branches = pullRuleSplit(value(row, "branches"));
    if (branches.length) rule.branches = branches;
    const suggestion = value(row, "suggestion");
    if (suggestion) rule.suggestion = suggestion;
    ["paths", "testPaths", "docsPaths"].forEach((name) => {
      const items = pullRuleSplit(value(fields, name));
      if (items.length) rule[name] = items;
    });
    ["maxFiles", "maxLines", "maxSubjectLength"].forEach((name) => {
      const number = Number.parseInt(value(fields, name), 10);
      if (Number.isFinite(number) && number > 0) rule[name] = number;
    });
    const subject = value(fields, "subjectPattern");
    if (subject) rule.subjectPattern = subject;
    const patterns = pullRuleSplit(value(fields, "patterns"), /\n/);
    if (patterns.length) rule.patterns = patterns;
    if (kind === "model") {
      rule.models = [...row.querySelectorAll("[data-pull-rule-model]:checked")].map((input) => input.value);
    }
    return rule;
  }

  function readPullRulesEditor(root) {
    const taken = new Set();
    const rows = [...root.querySelectorAll("[data-pull-rule-row]")];
    const kept = rows.map((row) => {
      const id = String(row.dataset.pullRuleId || "").trim();
      if (!id || taken.has(id)) return "";
      taken.add(id);
      return id;
    });
    return {
      schema: "forkmesh-pull-rules-v1",
      rules: rows.map((row, index) => readPullRuleRow(row, kept[index], taken)),
    };
  }

  function pullRulesProblem(policy) {
    const branchPattern = /^[A-Za-z0-9*?][A-Za-z0-9._/*?-]{0,199}$/;
    const regexProblem = (value) => {
      if (/\(\?[=!<]|\(\?P=|\\[1-9]|\(\?>/.test(value)) return "uses lookaround or backreferences, which the rule checker does not support";
      try { new RegExp(value); } catch (_) { return "is not a valid regular expression"; }
      return "";
    };
    if ((policy.rules || []).length > 32) return "A rule set holds at most 32 rules.";
    for (const rule of policy.rules || []) {
      const name = rule.title || rule.id;
      if ((rule.title || "").length > 120) return `${name}: the title is longer than 120 characters.`;
      if ((rule.suggestion || "").length > 600) return `${name}: the fix text is longer than 600 characters.`;
      if ((rule.branches || []).some((branch) => !branchPattern.test(branch))) return `${name}: a branch pattern is invalid.`;
      if (rule.kind === "paths" && !(rule.paths || []).length) return `${name}: name at least one protected path.`;
      if (rule.kind === "size" && !rule.maxFiles && !rule.maxLines) return `${name}: set a file or line limit.`;
      if (rule.kind === "commits" && !rule.subjectPattern && !rule.maxSubjectLength) return `${name}: set a subject pattern or a length limit.`;
      if (rule.kind === "model" && !(rule.models || []).length) return `${name}: choose at least one allowed model.`;
      for (const pattern of [rule.subjectPattern, ...(rule.patterns || [])].filter(Boolean)) {
        const problem = regexProblem(pattern);
        if (problem) return `${name}: ${pattern} ${problem}.`;
      }
    }
    return "";
  }

  function bindPullRulesEditor(root, options = {}) {
    const form = root?.querySelector("[data-pull-rules-form]");
    if (!form) return;
    const models = Array.isArray(options.models) && options.models.length ? options.models : PULL_RULE_MODELS;
    const status = (text, tone = "") => {
      const node = form.querySelector("[data-pull-rules-status]");
      if (!node) return;
      node.textContent = text;
      node.className = `text-xs ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-success" : "text-muted-foreground"}`;
    };
    form.addEventListener("change", (event) => {
      const select = event.target.closest("[data-pull-rule-kind]");
      if (!select) return;
      const row = select.closest("[data-pull-rule-row]");
      row?.querySelectorAll("[data-pull-rule-kind-fields]").forEach((group) => {
        group.classList.toggle("hidden", group.dataset.pullRuleKindFields !== select.value);
      });
    });
    form.addEventListener("click", (event) => {
      if (event.target.closest("[data-pull-rule-remove]")) {
        event.target.closest("[data-pull-rule-row]")?.remove();
        return;
      }
      if (event.target.closest("[data-pull-rule-add]")) {
        const rows = form.querySelector("[data-pull-rules-rows]");
        rows?.querySelector("[data-pull-rules-empty]")?.remove();
        rows?.insertAdjacentHTML("beforeend", pullRuleRowHtml({ kind: "tests" }, models, true));
        window.lucide?.createIcons();
      }
    });
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const policy = readPullRulesEditor(form);
      const problem = pullRulesProblem(policy);
      if (problem) {
        status(problem, "bad");
        return;
      }
      const button = form.querySelector('button[type="submit"]');
      if (button) button.disabled = true;
      status("Saving…");
      try {
        const message = await options.onSave?.(policy);
        status(message || "Saved.", "good");
      } catch (error) {
        status(error?.message || "Could not save the rules.", "bad");
      } finally {
        if (button) button.disabled = false;
      }
    });
  }

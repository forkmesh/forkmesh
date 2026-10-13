(() => {
  "use strict";

  if (window.ForkMeshAPI?.installed) return;

  const STORAGE_KEY = "forkmesh.apiOrigin";
  const OFFICIAL_APP_ORIGIN = "https://app.forkmesh.com";
  const LEGACY_OFFICIAL_API_ORIGIN = "https://app.forkmesh.com";
  const NativeWebSocket = window.WebSocket;
  const nativeFetch = window.fetch.bind(window);
  const canonicalFrontendHosts = new Set([
    "forkmesh.com",
    "www.forkmesh.com",
    "app.forkmesh.com",
    "world.forkmesh.com",
  ]);

  function normalizeOrigin(value) {
    const raw = String(value || "").trim();
    if (!raw) return "";
    let url;
    try {
      url = new URL(raw);
    } catch (_) {
      return "";
    }
    if (url.username || url.password || url.pathname !== "/" || url.search || url.hash) {
      return "";
    }
    if (url.protocol === "https:") {
      return url.origin === LEGACY_OFFICIAL_API_ORIGIN
        ? OFFICIAL_APP_ORIGIN
        : url.origin;
    }
    if (
      url.protocol === "http:" &&
      ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname)
    ) {
      return url.origin;
    }
    return "";
  }

  function storedOrigin() {
    try {
      const value = localStorage.getItem(STORAGE_KEY);
      const origin = normalizeOrigin(value);
      if (origin === OFFICIAL_APP_ORIGIN && String(value || "").trim()) {
        const legacy = new URL(String(value).trim());
        if (legacy.origin === LEGACY_OFFICIAL_API_ORIGIN) {
          localStorage.setItem(STORAGE_KEY, OFFICIAL_APP_ORIGIN);
        }
      }
      return origin;
    } catch (_) {
      return "";
    }
  }

  function metadataOrigin() {
    try {
      return normalizeOrigin(
        document.querySelector('meta[name="forkmesh-api-origin"]')?.content,
      );
    } catch (_) {
      return "";
    }
  }

  function resolveOrigin() {
    const configured =
      normalizeOrigin(window.FORKMESH_API_ORIGIN) ||
      metadataOrigin() ||
      storedOrigin();
    if (configured) return configured;
    if (canonicalFrontendHosts.has(location.hostname.toLowerCase())) {
      return OFFICIAL_APP_ORIGIN;
    }
    return location.origin;
  }

  function isApiPath(pathname) {
    return pathname.startsWith("/api/");
  }

  function routedHttpURL(value) {
    const raw = value instanceof Request ? value.url : value;
    let url;
    try {
      url = new URL(raw, location.href);
    } catch (_) {
      return null;
    }
    if (url.origin === location.origin && isApiPath(url.pathname)) {
      const api = new URL(resolveOrigin());
      url.protocol = api.protocol;
      url.host = api.host;
    }
    return url;
  }

  function apiTarget(url) {
    return Boolean(
      url && url.origin === resolveOrigin() && isApiPath(url.pathname),
    );
  }

  function routedFetch(input, init) {
    const url = routedHttpURL(input);
    if (!url) return nativeFetch(input, init);
    const original = input instanceof Request ? new URL(input.url) : new URL(input, location.href);
    let routedInput = input;
    if (url.href !== original.href) {
      routedInput = input instanceof Request
        ? new Request(url.href, input)
        : input instanceof URL
          ? url
          : url.href;
    }
    if (!apiTarget(url)) return nativeFetch(routedInput, init);
    const options = init ? { ...init } : {};
    const credentials = options.credentials || (
      input instanceof Request ? input.credentials : ""
    );
    if (!credentials || credentials === "same-origin") {
      options.credentials = "include";
    }
    if (options.mode === "same-origin") options.mode = "cors";
    return nativeFetch(routedInput, options);
  }

  function websocketURL(value) {
    let url;
    try {
      url = new URL(String(value || ""), location.href);
    } catch (_) {
      return String(value || "");
    }
    const secure = url.protocol === "wss:" || url.protocol === "https:";
    const socketScheme = secure ? "wss:" : "ws:";
    if (!["ws:", "wss:", "http:", "https:"].includes(url.protocol) || !url.host) {
      return url.href;
    }
    const current = `${secure ? "https:" : "http:"}//${url.host}`;
    if (current === location.origin && isApiPath(url.pathname)) {
      const api = new URL(resolveOrigin());
      return `${api.protocol === "https:" ? "wss:" : "ws:"}//${api.host}${url.pathname}${url.search}`;
    }
    return `${socketScheme}//${url.host}${url.pathname}${url.search}`;
  }

  function RoutedWebSocket(url, protocols) {
    if (!new.target) throw new TypeError("WebSocket constructor requires new");
    const target = websocketURL(url);
    return protocols === undefined
      ? new NativeWebSocket(target)
      : new NativeWebSocket(target, protocols);
  }

  if (NativeWebSocket) {
    RoutedWebSocket.prototype = NativeWebSocket.prototype;
    Object.setPrototypeOf(RoutedWebSocket, NativeWebSocket);
    for (const name of ["CONNECTING", "OPEN", "CLOSING", "CLOSED"]) {
      Object.defineProperty(RoutedWebSocket, name, {
        value: NativeWebSocket[name],
        enumerable: true,
      });
    }
    window.WebSocket = RoutedWebSocket;
  }

  function sameSite() {
    const api = new URL(resolveOrigin());
    if (api.origin === location.origin) return true;
    if (api.protocol !== location.protocol) return false;
    const currentHost = location.hostname.toLowerCase();
    const apiHost = api.hostname.toLowerCase();
    if (canonicalFrontendHosts.has(currentHost) && canonicalFrontendHosts.has(apiHost)) {
      return true;
    }
    return currentHost === apiHost;
  }

  function configure(value, persist = true) {
    const origin = normalizeOrigin(value);
    if (!origin) throw new TypeError("ForkMesh API origin must be HTTPS");
    if (persist) localStorage.setItem(STORAGE_KEY, origin);
    window.FORKMESH_API_ORIGIN = origin;
    window.FORKMESH_RELAY_HOST = new URL(origin).host;
    window.dispatchEvent(new CustomEvent("forkmesh:api-origin", {
      detail: { origin },
    }));
    return origin;
  }

  function clearConfiguredOrigin() {
    try {
      localStorage.removeItem(STORAGE_KEY);
    } catch (_) {}
    delete window.FORKMESH_API_ORIGIN;
    window.FORKMESH_RELAY_HOST = new URL(resolveOrigin()).host;
  }

  let restoreSessionPromise = null;

  function storedUserSession() {
    try {
      const session = JSON.parse(
        localStorage.getItem("forkmesh.session") || "null",
      );
      if (!session?.nodeName || !session?.sessionToken) return null;
      return session.kind === "user" || session.email ? session : null;
    } catch (_) {
      return null;
    }
  }

  // localStorage is origin-scoped, while the revocable account cookie belongs
  // to the App API. Trusted ForkMesh frontends exchange that cookie for a
  // non-secret local marker so logging in on app.forkmesh.com also signs the
  // same browser into World and the other www-served tools.
  //
  // A visitor who is simply signed out has nothing to restore, and the in-page
  // promise dies with the document — so every docs → blog → pricing navigation
  // of every anonymous reader re-asked the relay the same question. An
  // authoritative 401 is remembered for this tab for a few minutes. Arriving
  // from another ForkMesh origin (the login page lives on App) skips the memo,
  // so signing in and coming back is still reflected on the first paint — and
  // so does an explicit reload, which is what someone who just signed in on
  // another tab reaches for.
  const RESTORE_MISS_KEY = "forkmesh.sessionRestoreMiss";
  const RESTORE_MISS_TTL_MS = 5 * 60 * 1000;

  function arrivedFromAnotherOrigin() {
    try {
      const referrer = String(document.referrer || "");
      return Boolean(referrer) && new URL(referrer).origin !== location.origin;
    } catch (_) {
      return false;
    }
  }

  function explicitReload() {
    try {
      return performance.getEntriesByType("navigation")[0]?.type === "reload";
    } catch (_) {
      return false;
    }
  }

  function recentRestoreMiss() {
    try {
      const missedAt = Number(sessionStorage.getItem(RESTORE_MISS_KEY) || 0);
      const age = Date.now() - missedAt;
      return missedAt > 0 && age >= 0 && age < RESTORE_MISS_TTL_MS;
    } catch (_) {
      return false;
    }
  }

  function restoreSession() {
    const stored = storedUserSession();
    if (stored) return Promise.resolve(stored);
    if (restoreSessionPromise) return restoreSessionPromise;
    if (
      recentRestoreMiss() && !arrivedFromAnotherOrigin() && !explicitReload()
    ) {
      return Promise.resolve(null);
    }
    restoreSessionPromise = routedFetch("/api/accounts/sessions", {
      headers: { accept: "application/json" },
      credentials: "include",
      cache: "no-store",
    }).then(async (response) => {
      if (response.status === 401) {
        // Only the relay's own "nobody is signed in" is remembered; an outage
        // or a network error must stay eligible for the very next page.
        try {
          sessionStorage.setItem(RESTORE_MISS_KEY, String(Date.now()));
        } catch (_) {}
      }
      if (!response.ok) return null;
      const payload = await response.json().catch(() => null);
      const account = payload?.account;
      if (!account?.nodeName || account.kind !== "user") return null;
      const session = {
        ...account,
        sessionToken: "cookie",
        at: Date.now(),
      };
      localStorage.setItem("forkmesh.session", JSON.stringify(session));
      try {
        sessionStorage.removeItem(RESTORE_MISS_KEY);
        // The headers keep this response's membership projection in a short
        // per-tab cache (site-header.js, home-header-auth.js). Filing it here,
        // straight from the relay's answer, saves them re-reading the route
        // this function just read.
        sessionStorage.setItem(
          "forkmesh.membershipProjection.v1",
          JSON.stringify({
            account: String(account.nodeName),
            membership: account.membership || null,
            ts: Date.now(),
          }),
        );
      } catch (_) {}
      window.dispatchEvent(new CustomEvent("forkmesh:session-restored", {
        detail: { nodeName: session.nodeName },
      }));
      return session;
    }).catch(() => null).finally(() => {
      restoreSessionPromise = null;
    });
    return restoreSessionPromise;
  }

  const STATUS_PAGE_KEY = "forkmesh.statusPage.v1";
  const STATUS_PAGE_TTL_MS = 5 * 60 * 1000;
  const PLUGINS_KEY = "forkmesh.plugins.v1";
  const PLUGIN_ID_RE = /^[a-z0-9][a-z0-9-]{0,63}$/;
  let pluginsPromise = null;
  let pluginStyle = null;

  function readStatusPage() {
    try {
      const entry = JSON.parse(localStorage.getItem(STATUS_PAGE_KEY) || "null");
      return entry && typeof entry.account === "string" ? entry : null;
    } catch (_) {
      return null;
    }
  }

  function readPlugins() {
    try {
      const entry = JSON.parse(localStorage.getItem(PLUGINS_KEY) || "null");
      return entry && typeof entry.account === "string" && Array.isArray(entry.plugins)
        ? entry
        : null;
    } catch (_) {
      return null;
    }
  }

  function accountPlugins(account) {
    const entry = readPlugins();
    return entry && account && entry.account === account ? entry.plugins : null;
  }

  function cleanPlugins(plugins) {
    return [...new Set((plugins || []).map(String).filter((id) => PLUGIN_ID_RE.test(id)))];
  }

  function statusPageShown(account) {
    if (!account) return true;
    const entry = readStatusPage();
    return !(entry && entry.account === account && entry.installed === false);
  }

  function pluginShown(id, account) {
    if (!account) return true;
    const list = accountPlugins(account);
    return !list || list.includes(String(id || ""));
  }

  function currentAccount() {
    return String(storedUserSession()?.nodeName || "");
  }

  function pluginGateRule(account) {
    const list = accountPlugins(account);
    if (!account || !list) return "";
    const allowed = list.map((id) => `:not([data-plugin-entry="${id}"])`).join("");
    return `[data-plugin-entry]${allowed}{display:none!important}`;
  }

  function applyStatusPage() {
    const root = document.documentElement;
    if (!root) return;
    const account = currentAccount();
    root.toggleAttribute("data-status-page-off", !statusPageShown(account));
    if (pluginStyle) pluginStyle.textContent = pluginGateRule(account);
  }

  function writeStatusPage(name, installed) {
    try {
      localStorage.setItem(STATUS_PAGE_KEY, JSON.stringify({
        account: name,
        installed: installed !== false,
        ts: Date.now(),
      }));
    } catch (_) {}
  }

  function writePlugins(name, plugins) {
    try {
      localStorage.setItem(PLUGINS_KEY, JSON.stringify({
        account: name,
        plugins,
        ts: Date.now(),
      }));
    } catch (_) {}
  }

  function rememberPlugins(account, plugins) {
    const name = String(account || "");
    if (!name || !Array.isArray(plugins)) return;
    const list = cleanPlugins(plugins);
    writePlugins(name, list);
    writeStatusPage(name, list.includes("status-page"));
    applyStatusPage();
  }

  function rememberPlugin(account, id, installed) {
    const name = String(account || "");
    const plugin = String(id || "");
    if (!name || !PLUGIN_ID_RE.test(plugin)) return;
    const list = accountPlugins(name);
    if (list) {
      const next = list.filter((item) => item !== plugin);
      if (installed !== false) next.push(plugin);
      const entry = readPlugins();
      try {
        localStorage.setItem(PLUGINS_KEY, JSON.stringify({
          account: name,
          plugins: next,
          ts: Number(entry?.ts || 0),
        }));
      } catch (_) {}
    } else if (name === currentAccount()) {
      void refreshPlugins(true);
    }
    if (plugin === "status-page") writeStatusPage(name, installed);
    applyStatusPage();
  }

  function rememberStatusPage(account, installed) {
    rememberPlugin(account, "status-page", installed !== false);
  }

  function refreshPlugins(force = false) {
    const session = storedUserSession();
    applyStatusPage();
    if (!session) return Promise.resolve(null);
    const account = String(session.nodeName);
    const entry = readPlugins();
    if (
      !force && entry && entry.account === account &&
      Date.now() - Number(entry.ts || 0) < STATUS_PAGE_TTL_MS
    ) {
      return Promise.resolve(entry.plugins);
    }
    if (pluginsPromise) return pluginsPromise;
    const token = String(session.sessionToken || "");
    pluginsPromise = routedFetch("/api/plugins/", {
      headers: {
        accept: "application/json",
        ...(token && token !== "cookie"
          ? { authorization: `Bearer ${token}` }
          : {}),
      },
      credentials: "include",
      cache: "no-store",
    }).then(async (response) => {
      const payload = response.ok
        ? await response.json().catch(() => null)
        : null;
      if (!Array.isArray(payload?.plugins)) return accountPlugins(account);
      rememberPlugins(account, payload.plugins);
      return accountPlugins(account);
    }).catch(() => accountPlugins(account)).finally(() => {
      pluginsPromise = null;
    });
    return pluginsPromise;
  }

  function refreshStatusPage(force = false) {
    const account = currentAccount();
    if (!account) {
      applyStatusPage();
      return Promise.resolve(true);
    }
    return refreshPlugins(force).then(() => statusPageShown(account));
  }

  try {
    const style = document.createElement("style");
    style.textContent =
      "html[data-status-page-off] [data-status-entry]{display:none!important}";
    (document.head || document.documentElement).append(style);
    pluginStyle = document.createElement("style");
    pluginStyle.setAttribute?.("data-plugin-gate", "");
    (document.head || document.documentElement).append(pluginStyle);
    applyStatusPage();
    window.addEventListener("storage", (event) => {
      if (
        event.key === STATUS_PAGE_KEY || event.key === PLUGINS_KEY ||
        event.key === "forkmesh.session" || event.key === null
      ) {
        applyStatusPage();
      }
    });
    window.addEventListener("forkmesh:session-restored", () => {
      void refreshStatusPage(false);
    });
  } catch (_) {}

  const api = {
    installed: true,
    storageKey: STORAGE_KEY,
    configure,
    clearConfiguredOrigin,
    restoreSession,
    statusPageShown: (account) => statusPageShown(
      account === undefined ? currentAccount() : String(account || ""),
    ),
    rememberStatusPage,
    refreshStatusPage,
    pluginShown: (id, account) => pluginShown(
      id, account === undefined ? currentAccount() : String(account || ""),
    ),
    rememberPlugins,
    rememberPlugin,
    refreshPlugins,
    fetch: routedFetch,
    url(path = "/api") {
      return new URL(String(path || "/api"), resolveOrigin()).href;
    },
    websocketURL,
  };
  Object.defineProperties(api, {
    origin: { enumerable: true, get: resolveOrigin },
    isSameSite: { enumerable: true, get: sameSite },
  });
  window.ForkMeshAPI = Object.freeze(api);
  window.FORKMESH_RELAY_HOST = new URL(resolveOrigin()).host;
  window.fetch = routedFetch;
})();

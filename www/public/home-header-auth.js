// Session-aware auth buttons for the home page's hero header. Every other
// page gets this for free from the shared site-header.js renderer, but the
// home page keeps its own hero header — so before this script a logged-in
// visitor still saw "Sign in / Sign up" on /, disagreeing with the
// dashboard, chat and marketing pages. Reads the same forkmesh.session
// marker the rest of the site uses and swaps the two hero buttons for
// Dashboard + account chip; re-renders on storage events so a login or
// logout in another open section is reflected here without a reload.
(() => {
  // Same acceptance rule as site-header.js / chat.js: a user session carries
  // kind "user" (or a legacy pre-kind session with an email); node sessions
  // never render as a logged-in website account.
  function readSession() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!session || !session.nodeName) return null;
      if (session.kind === "node") return null;
      if (session.kind === "user" || session.email) return session;
      return null;
    } catch (_) {
      return null;
    }
  }

  function makeAvatar(session) {
    const avatar = document.createElement("span");
    avatar.className = "home-auth-avatar";
    if (session.avatarPng) {
      const img = document.createElement("img");
      img.alt = "";
      img.src = String(session.avatarPng).startsWith("data:")
        ? session.avatarPng
        : "data:image/png;base64," + session.avatarPng;
      avatar.append(img);
    } else {
      avatar.textContent = String(session.nodeName || "?").charAt(0).toUpperCase();
    }
    return avatar;
  }

  let membershipProjection = null;
  let membershipProjectionPromise = null;

  // Same cross-navigation cache, key and TTL as site-header.js: the homepage
  // and the site header draw the same plan badge from the same projection, so
  // a visitor bouncing between the homepage and docs should pay for one read,
  // not one per navigation. See the longer note there for why sessionStorage
  // and why a badge is safe to cache when an authorization decision would not
  // be.
  const MEMBERSHIP_CACHE_KEY = "forkmesh.membershipProjection.v1";
  const MEMBERSHIP_CACHE_TTL_MS = 5 * 60 * 1000;

  function readCachedMembership(account) {
    try {
      const raw = sessionStorage.getItem(MEMBERSHIP_CACHE_KEY);
      if (!raw) return undefined;
      const entry = JSON.parse(raw);
      if (!entry || entry.account !== account) return undefined;
      if (Date.now() - Number(entry.ts || 0) >= MEMBERSHIP_CACHE_TTL_MS) {
        return undefined;
      }
      return entry.membership || null;
    } catch (_) {
      return undefined;
    }
  }

  function writeCachedMembership(account, membership) {
    try {
      sessionStorage.setItem(
        MEMBERSHIP_CACHE_KEY,
        JSON.stringify({ account, membership: membership || null, ts: Date.now() }),
      );
    } catch (_) {}
  }

  // The marker this page reads is a copy, not the authority. The account
  // session itself is an HttpOnly cookie on the App origin, and localStorage
  // is origin-scoped, so signing out on app.forkmesh.com cannot reach this
  // origin's copy. Nothing here ever expired it, which is why a logged-out
  // browser kept showing an account chip on the marketing pages until its
  // storage was cleared by hand. Only a 401 from the session API clears it.
  function clearLocalSession() {
    membershipProjection = null;
    membershipProjectionPromise = null;
    try {
      localStorage.removeItem("forkmesh.session");
      sessionStorage.removeItem(MEMBERSHIP_CACHE_KEY);
    } catch (_) {}
  }

  function membershipBadgeLabel(membership) {
    if (!membership || typeof membership !== "object") return "";
    if (membership.studio === true || membership.tier === "studio") {
      return "STUDIO";
    }
    if (membership.builder === true || membership.tier === "builder") {
      return "BUILDER";
    }
    if (membership.pro === true || membership.tier === "pro") return "PRO";
    if (membership.supporter === true || membership.tier === "supporter") {
      return "SUPPORTER";
    }
    return "";
  }

  function appendPlanBadge(parent, membership) {
    const text = membershipBadgeLabel(membership);
    if (!text || !parent) return;
    const badge = document.createElement("span");
    badge.className = "fm-header-plan-badge home-auth-plan-badge";
    badge.dataset.plan = text.toLowerCase();
    badge.textContent = text;
    parent.append(badge);
  }

  function fetchMembershipProjection(force) {
    if (!force && membershipProjectionPromise) return membershipProjectionPromise;
    const session = readSession();
    if (!session) {
      membershipProjection = null;
      membershipProjectionPromise = Promise.resolve(null);
      return membershipProjectionPromise;
    }
    const account = String(session.nodeName || "");
    if (!force) {
      const cached = readCachedMembership(account);
      if (cached !== undefined) {
        membershipProjection = cached;
        membershipProjectionPromise = Promise.resolve(cached);
        return membershipProjectionPromise;
      }
    }
    const token = String(session.sessionToken || "");
    membershipProjectionPromise = fetch("/api/accounts/sessions", {
      headers: {
        accept: "application/json",
        ...(token && token !== "cookie"
          ? { authorization: `Bearer ${token}` }
          : {}),
      },
      credentials: "same-origin",
      cache: "no-store",
    }).then(async (response) => {
      // 401 is the session API saying this browser has no live session --
      // the one answer that means signed out. A 5xx, or no answer at all,
      // means we could not ask; network loss is not a logout, so the marker
      // stays and the next focus retries.
      if (response.status === 401) {
        clearLocalSession();
        return null;
      }
      if (!response.ok) {
        membershipProjection = null;
        return null;
      }
      const data = await response.json().catch(() => null);
      membershipProjection = data?.account?.membership || null;
      writeCachedMembership(account, membershipProjection);
      return membershipProjection;
    }).catch(() => {
      membershipProjection = null;
      return null;
    });
    return membershipProjectionPromise;
  }

  function render() {
    const secondary = document.querySelector("[data-home-auth-secondary]");
    const primary = document.querySelector("[data-home-auth-primary]");
    if (!secondary || !primary) return;
    const session = readSession();
    if (!session) {
      membershipProjection = null;
      membershipProjectionPromise = null;
      secondary.textContent = "Sign in";
      secondary.href = "/login";
      primary.textContent = "Sign up";
      primary.href = "/signup";
      primary.removeAttribute("title");
      primary.removeAttribute("aria-label");
      return;
    }
    // Account name is DOM-built (never innerHTML) so a stored name can't
    // inject markup.
    secondary.textContent = "Dashboard";
    secondary.href = "https://app.forkmesh.com/";
    primary.textContent = "";
    primary.append(makeAvatar(session));
    const status = document.createElement("span");
    status.className = "home-auth-session-dot";
    status.setAttribute("aria-hidden", "true");
    primary.append(status);
    const label = document.createElement("span");
    label.className = "home-auth-name";
    label.textContent = String(session.nodeName).slice(0, 32);
    primary.append(label);
    appendPlanBadge(primary, membershipProjection);
    primary.href = "https://app.forkmesh.com/dashboard/settings";
    primary.title = "Account settings";
    primary.setAttribute(
      "aria-label",
      `Signed in as ${session.nodeName}; open account settings`,
    );
  }

  // The app-cookie exchange below lands a moment after first paint and turns
  // "Sign in" into "Dashboard" in place, so a visitor already reaching for
  // Sign in clicked Dashboard instead and was dropped into the app. Hold any
  // re-render while the pointer or focus is on these buttons (applied as soon
  // as it leaves), and if a press began on one destination but the swap still
  // landed before its click (touch fires pointerleave before click), follow
  // the destination that was pressed.
  const authLinks = [
    document.querySelector("[data-home-auth-secondary]"),
    document.querySelector("[data-home-auth-primary]"),
  ].filter(Boolean);
  let pointerOver = null;
  let press = null;

  function requestRender() {
    if (authLinks.includes(pointerOver) || authLinks.includes(document.activeElement)) {
      return;
    }
    render();
  }

  for (const link of authLinks) {
    link.addEventListener("pointerenter", () => {
      pointerOver = link;
    });
    link.addEventListener("pointerleave", () => {
      if (pointerOver === link) pointerOver = null;
      requestRender();
    });
    link.addEventListener("focusout", () => setTimeout(requestRender, 0));
    link.addEventListener("pointerdown", () => {
      press = { link, href: link.href };
    });
    link.addEventListener("click", (event) => {
      const pressed = press;
      press = null;
      // detail 0 is a keyboard activation, which the focus hold covers.
      if (!pressed || pressed.link !== link || event.detail === 0) return;
      if (pressed.href === link.href || event.defaultPrevented) return;
      if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) {
        return;
      }
      event.preventDefault();
      location.assign(pressed.href);
    });
  }

  // Check on focus and visibilitychange, never on a timer: these pages carry
  // no socket, and a landing page left open overnight must not spend an
  // authenticated read every few seconds to learn nothing. The floor keeps an
  // alt-tab burst from becoming that timer by another route. Same rule and
  // same interval as the shared site header.
  const SESSION_REVALIDATE_MIN_MS = 5 * 60 * 1000;

  function startSessionWatch() {
    let checking = false;
    let lastValidatedMs = Date.now();
    const validate = async () => {
      if (checking || document.hidden || !readSession()) return;
      if (Date.now() - lastValidatedMs < SESSION_REVALIDATE_MIN_MS) return;
      checking = true;
      try {
        await fetchMembershipProjection(true);
        lastValidatedMs = Date.now();
        requestRender();
      } catch (_) {
        // Leave the floor unarmed so the next focus retries.
      } finally {
        checking = false;
      }
    };
    window.addEventListener("focus", validate);
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) void validate();
    });
    // A sign-out in another tab on this origin. BroadcastChannel is
    // origin-scoped, so this never hears the App; that case is the session
    // check above.
    try {
      const channel = new BroadcastChannel("forkmesh.session");
      channel.addEventListener("message", (event) => {
        if (event.data?.type !== "signed-out" || !readSession()) return;
        clearLocalSession();
        requestRender();
      });
    } catch (_) {}
  }

  // A session api-client.js just rebuilt from the cookie came out of this same
  // endpoint a moment ago, and api-client filed that response's projection in
  // the cache above, so read it from there rather than asking again: a restore
  // used to cost three identical requests.
  function adoptRestoredSession() {
    if (!readSession()) return;
    membershipProjectionPromise = null;
    fetchMembershipProjection(false).then(requestRender);
  }

  const HOME_REDIRECT_COOKIE = "forkmesh_home";
  const MARKETING_HOST = /^(?:www\.)?forkmesh\.com$/i.test(location.hostname);
  const DASHBOARD_HREF = MARKETING_HOST
    ? "https://app.forkmesh.com/dashboard"
    : "/dashboard";

  function homeRedirectEnabled() {
    let cookie = "";
    try {
      cookie = document.cookie;
    } catch (_) {}
    const entry = String(cookie || "").split(";").map((part) => part.trim())
      .find((part) => part.startsWith(HOME_REDIRECT_COOKIE + "="));
    return !entry || entry.slice(HOME_REDIRECT_COOKIE.length + 1) !== "stay";
  }

  function arrivedFromForkMesh() {
    try {
      const from = new URL(document.referrer).hostname.toLowerCase();
      return from === location.hostname.toLowerCase() ||
        from === "forkmesh.com" || from.endsWith(".forkmesh.com");
    } catch (_) {
      return false;
    }
  }

  const homeRedirectAllowed = location.pathname === "/" && !location.hash &&
    !location.search && homeRedirectEnabled() && !arrivedFromForkMesh();

  function openDashboardIfSignedIn() {
    if (!homeRedirectAllowed || !readSession() || !homeRedirectEnabled()) {
      return false;
    }
    location.replace(DASHBOARD_HREF);
    return true;
  }

  render();
  window.ForkMeshAPI?.refreshStatusPage?.();
  startSessionWatch();
  if (readSession()) {
    // Not forced: within one tab the five-minute cache answers reloads and
    // back-navigation. A new tab has no cache, so a marker whose session died
    // elsewhere is still cleared by the first page it opens; the focus watch
    // and the sign-out broadcast cover a page that stays open.
    fetchMembershipProjection(false).then(() => {
      if (!openDashboardIfSignedIn()) requestRender();
    });
  } else if (window.ForkMeshAPI?.restoreSession) {
    window.ForkMeshAPI.restoreSession().then((session) => {
      if (session && !openDashboardIfSignedIn()) adoptRestoredSession();
    });
  }
  window.addEventListener("storage", (event) => {
    if (event.key === "forkmesh.session" || event.key === null) {
      if (readSession()) {
        membershipProjectionPromise = null;
        fetchMembershipProjection(false).then(requestRender);
      } else {
        requestRender();
      }
    }
  });
  window.addEventListener("forkmesh:session-restored", adoptRestoredSession);
})();

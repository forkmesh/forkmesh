// Universal site header, mounted wherever a page places
// <div data-forkmesh-header="simple"></div>. One renderer for every page
// (except the home page, which keeps its own hero header), styled to match
// the dashboard chrome: a hamburger menu holding every site link (grouped,
// fully expanded), the brand mark with the live release version, the current
// page title, a mirror-reward settings shortcut, the theme toggle, and a
// session-aware account area — a logged-in visitor sees their account chip
// (Dashboard / Profile / Log out) instead of hardcoded "Sign Up / Log In".
(() => {
  // ---- Site-wide light/dark theme -----------------------------------------
  // The header is the one script every page already loads, so it doubles as
  // the theme engine: stamp html.light / html.dark from the visitor's saved
  // choice (same localStorage keys the dashboard and docs use) or, absent a
  // choice, their OS preference. styles.css keys its palettes off these
  // classes; pages without JavaScript simply stay dark.
  const THEME_KEYS = ["forkmesh.dashboard.theme", "forkmesh.theme"];
  const MARKETING_HOST = /^(?:www\.)?forkmesh\.com$/i.test(location.hostname);
  // Marketing pages are static assets. Call the App origin directly so a
  // public badge/version read does not invoke the www Worker and then the App
  // Worker through a Service Binding. App and local-development pages keep
  // their same-origin behavior.
  const APP_API_ORIGIN = MARKETING_HOST ? "https://app.forkmesh.com" : "";
  // Docs and Blog can be delivered through a node-hosted App origin so its
  // host-only account cookie and local session marker stay available. Its App
  // control must return to that same origin's dashboard instead of opening a
  // new hosted session at the canonical App.
  const APP_HREF = MARKETING_HOST
    ? "https://app.forkmesh.com/"
    : "/dashboard";
  // Pricing is a section of the marketing home page, not a page of its own.
  const PRICING_HREF = MARKETING_HOST
    ? "/#pricing"
    : "https://forkmesh.com/#pricing";
  // Where joining actually happens. It is the membership link's real href so
  // the link still works without JavaScript, and the fallback when the
  // membership bundle cannot be fetched.
  const PATREON_JOIN_HREF = "https://www.patreon.com/16434219/join";
  const MARKET_HREF = MARKETING_HOST
    ? "/market"
    : "https://forkmesh.com/market";
  const HOME_HREF = MARKETING_HOST ? "/" : "https://forkmesh.com/";
  const SIGNED_OUT_HREF = /^(?:app|world)\.forkmesh\.com$/i.test(location.hostname)
    ? "https://forkmesh.com/"
    : "/";
  const APP_PAGE_ORIGIN = MARKETING_HOST ? "https://app.forkmesh.com" : "";
  const PROFILE_HREF = APP_PAGE_ORIGIN + "/dashboard/profile";
  const REPOS_HREF = APP_PAGE_ORIGIN + "/dashboard/repos";
  const APPS_HREF = APP_PAGE_ORIGIN + "/dashboard/apps";
  const CHALLENGES_HREF = APP_PAGE_ORIGIN + "/dashboard/challenges";
  const ADS_HREF = APP_PAGE_ORIGIN + "/dashboard/ads";
  const HACKATHON_HREF = APP_PAGE_ORIGIN + "/hackathon/me";
  const appApiUrl = (path) => APP_API_ORIGIN + path;
  const PUBLIC_PROFILE_ORIGIN = location.hostname.toLowerCase() ===
    "forkmesh.com" ? "https://forkmesh.com" : location.origin;

  function publicProfileUrl(name) {
    return PUBLIC_PROFILE_ORIGIN + "/@" +
      encodeURIComponent(String(name || "").trim().toLowerCase());
  }

  function storedTheme() {
    try {
      for (const key of THEME_KEYS) {
        const value = localStorage.getItem(key);
        if (value === "light" || value === "dark") return value;
      }
    } catch (_) {}
    return "";
  }

  function resolveTheme() {
    const saved = storedTheme();
    if (saved) return saved;
    try {
      if (window.matchMedia("(prefers-color-scheme: light)").matches) return "light";
    } catch (_) {}
    return "dark";
  }

  function applySiteTheme(theme) {
    const light = theme === "light";
    const root = document.documentElement;
    root.classList.toggle("light", light);
    root.classList.toggle("dark", !light);
    root.style.colorScheme = light ? "light" : "dark";
    // The button shows the theme a click would move you to, so a dark page
    // offers the sun. Two inline SVGs rather than the ☀/☾ glyphs: at 16px
    // those render as a spindly asterisk in most UI fonts, and their weight
    // changes from platform to platform.
    document.querySelectorAll(".fm-header-theme").forEach((button) => {
      const sun = button.querySelector(".fm-theme-sun");
      const moon = button.querySelector(".fm-theme-moon");
      // toggleAttribute, not `.hidden`: these are <svg> elements, and the
      // `hidden` IDL property belongs to HTMLElement. Assigning it on an SVG
      // sets a plain expando and never touches the attribute, so the icon
      // that ships hidden in the markup stayed hidden forever and the other
      // one stayed visible - the toggle showed the same icon in both themes,
      // or both icons at once on a page whose stylesheet does not enforce
      // the attribute.
      sun?.toggleAttribute("hidden", light);
      moon?.toggleAttribute("hidden", !light);
      button.setAttribute("aria-label",
                          light ? "Switch to dark mode" : "Switch to light mode");
      button.title = light ? "Switch to dark mode" : "Switch to light mode";
    });
  }

  function saveTheme(theme) {
    try {
      THEME_KEYS.forEach((key) => localStorage.setItem(key, theme));
    } catch (_) {}
    applySiteTheme(theme);
  }

  // Apply immediately at parse time (the script loads early on every page) so
  // light-mode visitors don't get a dark flash, and follow OS changes live
  // while the visitor hasn't made an explicit choice.
  applySiteTheme(resolveTheme());
  try {
    window.matchMedia("(prefers-color-scheme: light)")
      .addEventListener("change", () => {
        if (!storedTheme()) applySiteTheme(resolveTheme());
      });
  } catch (_) {}

  // Every site link lives in the hamburger menu, grouped and fully expanded —
  // no nested "More" submenu to open. Groups are flat columns rather than
  // boxed cards, and the legal links sit in a quiet footer row, so uneven
  // group lengths read as whitespace instead of half-empty boxes.
  const NAV_HTML = `
    <nav class="forkmesh-simple-header-nav" aria-label="Primary">
      <div class="fm-nav-group">
        <span class="fm-nav-group-title">Dashboard</span>
        <a href="${HOME_HREF}">Home</a>
        <a href="${APP_HREF}">Dashboard</a>
        <a href="${PROFILE_HREF}">Profile</a>
        <a href="${REPOS_HREF}">Repositories</a>
        <a href="${APPS_HREF}">Apps</a>
      </div>
      <div class="fm-nav-group">
        <span class="fm-nav-group-title">Community</span>
        <a href="https://world.forkmesh.com/" data-plugin-entry="world">World</a>
        <a href="/chat" data-plugin-entry="chat">Chat</a>
        <a href="/network" data-plugin-entry="network">Network</a>
        <a href="/ecosystem">Ecosystem</a>
        <a href="/leaderboards">Leaderboards</a>
        <a href="/microjobs">Microjobs</a>
        <a href="/referral">Referral</a>
        <a href="${CHALLENGES_HREF}">Challenges</a>
        <a href="${ADS_HREF}">Ads</a>
        <a href="${HACKATHON_HREF}">Hackathon</a>
      </div>
      <div class="fm-nav-group">
        <span class="fm-nav-group-title">Product</span>
        <a href="/features">Features</a>
        <a href="/desktop">Desktop app</a>
        <a href="/mobile">Mobile app</a>
        <a class="fm-nav-marketplace" href="${MARKET_HREF}">Marketplace <svg class="fm-nav-external" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7 17 17 7M8 7h9v9"></path></svg></a>
        <a href="${PRICING_HREF}">Pricing</a>
        <a href="${PATREON_JOIN_HREF}" target="_blank" rel="noopener" data-forkmesh-membership>Membership</a>
        <a href="/mirror-payouts">Mirror payouts</a>
      </div>
      <div class="fm-nav-group">
        <span class="fm-nav-group-title">Resources</span>
        <a href="/docs">Docs</a>
        <a href="/blog">Blog</a>
        <a href="/changelog">Changelog</a>
        <a href="/status" data-status-entry>Status</a>
        <a href="/ledger" data-plugin-entry="ledger">Financial ledger</a>
      </div>
      <div class="fm-nav-group">
        <span class="fm-nav-group-title">Company</span>
        <a href="/about">About</a>
        <a href="/careers">Careers</a>
        <a href="/press">Press</a>
      </div>
    </nav>
  `;

  const LEGAL_HTML = `
    <nav class="fm-nav-legal" aria-label="Legal and security">
      <span class="fm-nav-group-title">Legal &amp; security</span>
      <a href="/security-report">Security</a>
      <a href="/privacy">Privacy</a>
      <a href="/terms">Terms</a>
    </nav>
  `;

  // Stroke icons for the menu links, keyed by href and drawn into a small
  // tile in front of each label at mount. Static strings only.
  const NAV_ICONS = {
    [HOME_HREF]: '<path d="M3 10.5 12 3l9 7.5"></path><path d="M5 9.5V20a1 1 0 0 0 1 1h4v-6h4v6h4a1 1 0 0 0 1-1V9.5"></path>',
    [PROFILE_HREF]: '<circle cx="12" cy="8" r="4"></circle><path d="M4 21a8 8 0 0 1 16 0"></path>',
    [REPOS_HREF]: '<path d="M4 19.5V5a2 2 0 0 1 2-2h13v15H6.5A2.5 2.5 0 0 0 4 20.5 2.5 2.5 0 0 0 6.5 23H19v-5"></path><path d="M9 3v7l2.5-1.5L14 10V3"></path>',
    [APPS_HREF]: '<rect x="3" y="3" width="7" height="7" rx="1.5"></rect><rect x="14" y="3" width="7" height="7" rx="1.5"></rect><rect x="3" y="14" width="7" height="7" rx="1.5"></rect><path d="M17.5 14v7M14 17.5h7"></path>',
    [CHALLENGES_HREF]: '<path d="M14.5 17.5 3 6V3h3l11.5 11.5"></path><path d="m13 19 6-6M16 16l4 4M19 21l2-2"></path><path d="M9.5 6.5 14 2h3v3l-4.5 4.5"></path><path d="m5 14 4 4M7 17l-3 3M3 19l2 2"></path>',
    [ADS_HREF]: '<path d="m3 11 18-5v12L3 14v-3Z"></path><path d="M11.6 16.8a3 3 0 1 1-5.8-1.6"></path>',
    [HACKATHON_HREF]: '<path d="M4 15s1-1 4-1 5 2 8 2 4-1 4-1V3s-1 1-4 1-5-2-8-2-4 1-4 1Z"></path><path d="M4 22v-7"></path>',
    "/features": '<path d="M12 3.5 13.9 9l5.6 1.9-5.6 1.9L12 18.5l-1.9-5.7-5.6-1.9L10.1 9Z"></path><path d="M19 3v4M17 5h4"></path>',
    [APP_HREF]: '<rect x="3" y="3" width="7" height="9" rx="1.5"></rect><rect x="14" y="3" width="7" height="5" rx="1.5"></rect><rect x="14" y="12" width="7" height="9" rx="1.5"></rect><rect x="3" y="16" width="7" height="5" rx="1.5"></rect>',
    "/desktop": '<rect x="2.5" y="4" width="19" height="13" rx="2"></rect><path d="M8 21h8M12 17v4"></path>',
    "/mobile": '<rect x="6" y="2.5" width="12" height="19" rx="2.5"></rect><path d="M10.5 18.5h3"></path>',
    [MARKET_HREF]: '<path d="M4 9h16l-1.2 10.2a2 2 0 0 1-2 1.8H7.2a2 2 0 0 1-2-1.8Z"></path><path d="M9 9V7a3 3 0 0 1 6 0v2"></path>',
    [PRICING_HREF]: '<path d="M20.6 13.4 13.4 20.6a2 2 0 0 1-2.8 0L3 13V3h10l7.6 7.6a2 2 0 0 1 0 2.8Z"></path><circle cx="7.5" cy="7.5" r="1.5"></circle>',
    [PATREON_JOIN_HREF]: '<path d="M12 20s-7.5-4.4-7.5-10A4.3 4.3 0 0 1 12 7.3 4.3 4.3 0 0 1 19.5 10c0 5.6-7.5 10-7.5 10Z"></path>',
    "/mirror-payouts": '<rect x="3" y="6" width="18" height="14" rx="2"></rect><path d="M3 10h18M7 6V4.5A1.5 1.5 0 0 1 8.5 3h9"></path><circle cx="16.5" cy="15" r="1.25"></circle>',
    "/docs": '<path d="M2.5 5H8a4 4 0 0 1 4 4v11a3 3 0 0 0-3-3H2.5Z"></path><path d="M21.5 5H16a4 4 0 0 0-4 4v11a3 3 0 0 1 3-3h6.5Z"></path>',
    "/blog": '<path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9Z"></path><path d="M14 3v6h6M8 13h8M8 17h5"></path>',
    "/changelog": '<circle cx="12" cy="12" r="3"></circle><path d="M12 3v6M12 15v6"></path>',
    "/status": '<path d="M21.5 12h-4l-3 8.5-5-17-3 8.5h-4"></path>',
    "/ledger": '<path d="M5 3h14v18l-2.5-1.8L14 21l-2-1.8-2 1.8-2.5-1.8L5 21Z"></path><path d="M9 8h6M9 12h6"></path>',
    "https://world.forkmesh.com/": '<circle cx="12" cy="12" r="9"></circle><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"></path>',
    "/chat": '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path>',
    "/network": '<circle cx="18" cy="5" r="2.5"></circle><circle cx="6" cy="12" r="2.5"></circle><circle cx="18" cy="19" r="2.5"></circle><path d="m8.2 10.8 7.6-4.4M8.2 13.2l7.6 4.4"></path>',
    "/ecosystem": '<path d="M12 2.8 20 7.4v9.2l-8 4.6-8-4.6V7.4Z"></path><path d="m4 7.4 8 4.6 8-4.6M12 12v9.2"></path>',
    "/leaderboards": '<path d="M8 21h8M12 16v5M7 3h10v6a5 5 0 0 1-10 0Z"></path><path d="M17 5h3v2a3 3 0 0 1-3 3M7 5H4v2a3 3 0 0 0 3 3"></path>',
    "/microjobs": '<rect x="3" y="7" width="18" height="13" rx="2"></rect><path d="M9 7V5a2 2 0 0 1 2-2h2a2 2 0 0 1 2 2v2M3 13h18"></path>',
    "/referral": '<circle cx="9" cy="8" r="4"></circle><path d="M2 21a7 7 0 0 1 14 0M19 8v6M16 11h6"></path>',
    "/about": '<circle cx="12" cy="12" r="9"></circle><path d="M12 11v5M12 8h.01"></path>',
    "/careers": '<circle cx="9" cy="8" r="3.5"></circle><path d="M2.5 20a6.5 6.5 0 0 1 13 0M16 4.5a3.5 3.5 0 0 1 0 7M18 14a6.5 6.5 0 0 1 3.5 6"></path>',
    "/press": '<path d="M3 10.5v3a1 1 0 0 0 1 1h2.5L12 19V5L6.5 9.5H4a1 1 0 0 0-1 1Z"></path><path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"></path>',
  };

  function decorateNavIcons(scope) {
    scope.querySelectorAll(".forkmesh-simple-header-nav a[href]").forEach((link) => {
      const paths = NAV_ICONS[link.getAttribute("href")];
      if (!paths) return;
      link.insertAdjacentHTML(
        "afterbegin",
        `<span class="fm-nav-icon" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${paths}</svg></span>`,
      );
    });
  }

  const HEADER_HTML = `
    <header class="forkmesh-simple-header">
      <button type="button" class="fm-header-burger" aria-label="Open site menu" aria-haspopup="true" aria-expanded="false">
        <span></span><span></span><span></span>
      </button>
      <a href="/" class="forkmesh-simple-brand" aria-label="ForkMesh home">
        <img class="forkmesh-simple-brand-mark" src="/assets/logo.svg" alt="" aria-hidden="true" />
        <span class="fm-header-version" hidden></span>
      </a>
      <span class="fm-header-context"></span>
      <div class="fm-header-right">
        <a class="fm-header-app" href="${APP_HREF}" title="Open the ForkMesh app">App</a>
        <a class="fm-header-world" href="https://world.forkmesh.com/" data-plugin-entry="world" title="Open ForkMesh World">
          <svg class="fm-header-world-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <circle cx="12" cy="12" r="9"></circle>
            <path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"></path>
          </svg>
          <strong>World</strong>
        </a>
        <a class="fm-header-payout" href="/mirror-payouts" title="Mirror reward settings" aria-label="Mirror reward settings">
          <img src="/assets/sol.png" alt="" aria-hidden="true" />
          <span>Mirror rewards</span>
        </a>
        <a class="fm-header-chat" href="/chat" data-plugin-entry="chat" title="Community chat" aria-label="Community chat">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path>
          </svg>
          <span class="fm-header-chat-badge" hidden></span>
        </a>
        <button type="button" class="fm-header-theme" aria-label="Switch color theme">
          <svg class="fm-theme-sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <circle cx="12" cy="12" r="4"></circle>
            <path d="M12 2.5v2M12 19.5v2M4.6 4.6l1.4 1.4M18 18l1.4 1.4M2.5 12h2M19.5 12h2M6 18l-1.4 1.4M19.4 4.6L18 6"></path>
          </svg>
          <svg class="fm-theme-moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" hidden>
            <path d="M20.5 13.4A8.5 8.5 0 1 1 10.6 3.5a6.6 6.6 0 0 0 9.9 9.9Z"></path>
          </svg>
        </button>
        <div class="fm-header-account"></div>
      </div>
      <div class="fm-header-mobile" hidden>
        ${NAV_HTML}
        <div class="fm-nav-group fm-nav-account">
          <span class="fm-nav-group-title">Account</span>
          <div class="fm-header-mobile-account"></div>
        </div>
        ${LEGAL_HTML}
      </div>
    </header>
  `;

  // Static markup only — user data never goes through innerHTML.
  const SIGNED_OUT_HTML = `
    <a class="fm-header-login" href="/login">Login</a>
    <a class="fm-header-signup" href="/signup">Sign Up</a>
  `;

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

  function clearSignedInBrowserState() {
    const preserved = new Set([
      "forkmesh.analyticsConsent.v1",
      "forkmesh.apiOrigin",
      "forkmesh.dashboard.theme",
      "forkmesh.genie.off",
      "forkmesh.theme",
    ]);
    try {
      localStorage.removeItem("forkmesh.session");
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
      // Server-side logout first: only the Worker can clear the HttpOnly
      // forkmesh_admin cookie, and skipping this left the admin page readable
      // after a marketing-page logout. Same call as the dashboard logout in
      // dashboard/js/02-helpers.js — every web logout goes through this one
      // endpoint.
      serverLogout = fetch(appApiUrl("/api/accounts/logout"), {
        method: "POST", credentials: "include", keepalive: true,
      }).catch(() => {});
    } catch (_) {}
    const cleanup = clearSignedInBrowserState();
    let redirected = false;
    const finish = () => {
      if (redirected) return;
      redirected = true;
      location.replace(SIGNED_OUT_HREF);
    };
    Promise.allSettled([serverLogout, cleanup]).then(finish);
    window.setTimeout(finish, 1200);
  }

  let membershipProjectionPromise = null;
  let membershipProjection = null;

  // Cross-navigation cache for the membership projection.
  //
  // The in-page `membershipProjectionPromise` memo only survives one document,
  // so browsing docs → blog → pricing re-read `GET /api/accounts/sessions` on
  // every single navigation just to decide whether to draw a "PRO" badge. The
  // projection is a badge, not an authorization decision — the Worker re-checks
  // membership on every request that depends on it — so a short shared TTL in
  // sessionStorage is safe and collapses a browse into one read.
  //
  // sessionStorage, not localStorage: it dies with the tab, so a stale plan
  // cannot outlive the visit. Keyed by account so switching users cannot
  // inherit the previous badge.
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
      if (response.status === 401) {
        membershipProjection = null;
        logout();
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

  // A session api-client.js just rebuilt from the cookie came out of this same
  // endpoint a moment ago, and api-client filed that response's projection in
  // the cache above. Read it from there instead of asking the route again: a
  // restore used to cost three identical requests (the restore, its `.then`,
  // and the `forkmesh:session-restored` listener).
  function adoptRestoredSession(renderAccountAreas) {
    if (!readSession()) return;
    membershipProjectionPromise = null;
    fetchMembershipProjection(false).then(renderAccountAreas);
  }

  window.forkmeshMembershipProjection = () => fetchMembershipProjection(false);

  // Marketing, docs and blog pages carry no WebSocket — opening one per blog
  // page would cost more than the poll it replaced — so this surface stays on
  // the plan's other prescription for session validity: check on focus and
  // visibilitychange, never on a timer.
  //
  // The 20s interval this used to run was the same request the dashboard ran,
  // on every docs and blog page, forever, in every background tab. A reader who
  // left a doc open overnight sent ~1,800 authenticated reads to learn nothing.
  const SESSION_REVALIDATE_MIN_MS = 5 * 60 * 1000;

  function startSessionWatch(renderAccountAreas) {
    let checking = false;
    let lastValidatedMs = Date.now();
    const validate = async () => {
      if (checking || document.hidden || !readSession()) return;
      // Focus fires in bursts while alt-tabbing; without a floor that burst is
      // the interval again, just driven by the user instead of a timer.
      if (Date.now() - lastValidatedMs < SESSION_REVALIDATE_MIN_MS) return;
      checking = true;
      try {
        await fetchMembershipProjection(true);
        lastValidatedMs = Date.now();
        renderAccountAreas();
      } catch (_) {
        // Network loss is not a logout. Only the session API's 401 is, and a
        // failure leaves the floor unarmed so the next focus retries.
      } finally {
        checking = false;
      }
    };
    window.addEventListener("focus", validate);
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) void validate();
    });
    try {
      const channel = new BroadcastChannel("forkmesh.session");
      channel.addEventListener("message", (event) => {
        if (event.data?.type !== "signed-out" || !readSession()) return;
        clearSignedInBrowserState().finally(() => {
          renderAccountAreas();
          location.replace(SIGNED_OUT_HREF);
        });
      });
    } catch (_) {}
  }

  function makeAccountAvatar(session) {
    const avatar = document.createElement("span");
    avatar.className = "fm-header-avatar";
    const name = String(session.nodeName || "?");
    if (session.avatarPng) {
      const img = document.createElement("img");
      img.alt = "";
      img.src = String(session.avatarPng).startsWith("data:")
        ? session.avatarPng
        : "data:image/png;base64," + session.avatarPng;
      avatar.append(img);
    } else {
      avatar.textContent = name.charAt(0).toUpperCase();
    }
    return avatar;
  }

  // Session-dependent right side. The signed-in branch is built with DOM
  // methods (never innerHTML) so a stored account name can't inject markup.
  function appendPlanBadge(parent, membership) {
    const text = membershipBadgeLabel(membership);
    if (!text || !parent) return;
    const badge = document.createElement("span");
    badge.className = "fm-header-plan-badge";
    badge.dataset.plan = text.toLowerCase();
    badge.textContent = text;
    parent.append(badge);
  }

  function buildAccountArea(container, session, options) {
    const stacked = Boolean(options && options.stacked);
    container.textContent = "";
    if (!session) {
      container.innerHTML = SIGNED_OUT_HTML;
      return;
    }
    const name = String(session.nodeName).slice(0, 32);
    if (stacked) {
      const status = document.createElement("p");
      status.className = "fm-header-mobile-session";
      const dot = document.createElement("span");
      dot.className = "fm-header-session-dot";
      dot.setAttribute("aria-hidden", "true");
      status.append(dot, document.createTextNode("Signed in as @" + name));
      const dash = document.createElement("a");
      dash.href = APP_HREF;
      dash.textContent = "Dashboard";
      const profile = document.createElement("a");
      profile.href = publicProfileUrl(name);
      profile.textContent = "Public profile (@" + name + ")";
      const edit = document.createElement("a");
      edit.href = MARKETING_HOST
        ? "https://app.forkmesh.com/dashboard/settings"
        : "/dashboard/settings";
      edit.textContent = "Settings";
      const out = document.createElement("button");
      out.type = "button";
      out.textContent = "Log out";
      out.addEventListener("click", logout);
      appendPlanBadge(container, membershipProjection);
      container.append(status, dash, profile, edit, out);
      return;
    }
    const wrap = document.createElement("div");
    wrap.className = "fm-header-menu fm-header-account-menu";
    const settings = document.createElement("a");
    settings.href = MARKETING_HOST
      ? "https://app.forkmesh.com/dashboard/settings"
      : "/dashboard/settings";
    settings.className = "fm-header-account-settings";
    settings.setAttribute("aria-label", "Open account settings");
    settings.title = "Settings";
    settings.append(makeAccountAvatar(session));
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "fm-header-account-chip";
    chip.setAttribute("aria-label", `Signed in as ${name}; open account menu`);
    chip.setAttribute("aria-haspopup", "true");
    chip.setAttribute("aria-expanded", "false");
    const statusDot = document.createElement("span");
    statusDot.className = "fm-header-session-dot";
    statusDot.setAttribute("aria-hidden", "true");
    chip.append(statusDot);
    const label = document.createElement("span");
    label.className = "fm-header-account-name";
    label.textContent = name;
    chip.append(label);
    const userStatus = window.ForkMeshUserStatus;
    const ownStatus = userStatus ? userStatus.mine() : null;
    if (ownStatus) {
      statusDot.classList.toggle("is-away", ownStatus.away);
      const ownBadge = userStatus.badge(ownStatus);
      if (ownBadge) chip.append(ownBadge);
    }
    appendPlanBadge(chip, membershipProjection);
    const chevron = document.createElement("span");
    chevron.className = "fm-header-account-chevron";
    chevron.setAttribute("aria-hidden", "true");
    chevron.textContent = "▾";
    chip.append(chevron);
    const dropdown = document.createElement("div");
    dropdown.className = "fm-header-dropdown fm-header-dropdown-right";
    dropdown.hidden = true;
    const dash = document.createElement("a");
    dash.href = APP_HREF;
    dash.textContent = "Dashboard";
    // Both sides of the profile: /@name is the page everyone else sees;
    // the dashboard's profile section is the private, editable view
    // (avatar, bio, email, account status).
    const profile = document.createElement("a");
    profile.href = publicProfileUrl(name);
    profile.textContent = "Public profile";
    // All account settings (profile, email, password, payout, nodes) live on
    // the dashboard settings page — the same destination as the avatar link.
    const edit = document.createElement("a");
    edit.href = MARKETING_HOST
      ? "https://app.forkmesh.com/dashboard/settings"
      : "/dashboard/settings";
    edit.textContent = "Settings";
    const out = document.createElement("button");
    out.type = "button";
    out.textContent = "Log out";
    out.addEventListener("click", logout);
    dropdown.append(dash, profile, edit);
    // Away + custom status: only on pages that load /user-status.js (chat).
    if (ownStatus) {
      const setStatus = document.createElement("button");
      setStatus.type = "button";
      setStatus.textContent = userStatus.hasCustom(ownStatus)
        ? "Edit status (" + userStatus.describe({ ...ownStatus, away: false }) + ")"
        : "Set a status…";
      setStatus.addEventListener("click", () => {
        dropdown.hidden = true;
        userStatus.openEditor();
      });
      const away = document.createElement("button");
      away.type = "button";
      away.textContent = ownStatus.away
        ? "Set yourself active" : "Set yourself away";
      away.addEventListener("click", () => {
        dropdown.hidden = true;
        userStatus.setAway(!ownStatus.away).catch(() => {});
      });
      dropdown.append(setStatus, away);
    }
    dropdown.append(out);
    wrap.append(settings, chip, dropdown);
    container.append(wrap);
    // Every render replaces the chip (the membership read that follows mount
    // re-renders a signed-in header), so the toggle is wired to this chip
    // here; wiring once at mount left the replacement chip dead.
    wireDropdown(wrap);
  }

  function markCurrentPage(scope) {
    const path = location.pathname.replace(/\.html$/, "").replace(/\/$/, "") || "/";
    scope.querySelectorAll("a[href]").forEach((link) => {
      const href = link.getAttribute("href");
      if (href !== "/" && (path === href || path.startsWith(href + "/"))) {
        link.setAttribute("aria-current", "page");
      }
    });
  }

  function wireDropdown(menu) {
    const btn = menu.querySelector("button");
    const dropdown = menu.querySelector(".fm-header-dropdown");
    if (!btn || !dropdown) return;
    btn.addEventListener("click", (event) => {
      event.stopPropagation();
      const open = dropdown.hidden;
      document.querySelectorAll(".fm-header-dropdown").forEach((el) => {
        el.hidden = true;
      });
      dropdown.hidden = !open;
      btn.setAttribute("aria-expanded", open ? "true" : "false");
    });
  }

  // Live release version pill next to the logo, same as the dashboard header
  // (and the same sessionStorage cache, so the two never disagree or double-
  // fetch). Best-effort: stay hidden if /api/version is unavailable.
  const APP_VERSION_STORAGE = "forkmesh.appVersion";
  const APP_VERSION_TTL_MS = 60 * 60 * 1000;

  async function renderAppVersion(el) {
    if (!el || location.protocol === "file:") return;
    const show = (version) => {
      el.textContent = version[0] === "v" ? version : "v" + version;
      el.hidden = false;
    };
    try {
      const cached = JSON.parse(sessionStorage.getItem(APP_VERSION_STORAGE) || "null");
      if (cached && cached.version && Number(cached.expiresAt) > Date.now()) {
        show(String(cached.version));
        return;
      }
    } catch (_) {}
    try {
      const response = await fetch(appApiUrl("/api/version"), {
        headers: { accept: "application/json" },
      });
      if (!response.ok) return;
      const data = await response.json();
      const version = (data && data.version ? String(data.version) : "").trim();
      if (!version) return;
      try {
        sessionStorage.setItem(APP_VERSION_STORAGE, JSON.stringify({
          version,
          expiresAt: Date.now() + APP_VERSION_TTL_MS,
        }));
      } catch (_) {}
      show(version);
    } catch (_) {}
  }

  // ---- Chat activity badge -------------------------------------------------
  // A small unread pill on the header's chat icon: counts new room messages
  // and new user signups since this browser last opened /chat, so a fresh
  // face can be welcomed from any page. Source of truth is the cheap,
  // edge-cached /api/chat/activity counters; the "seen" baseline lives in
  // localStorage and is (re)written by the chat page itself (chat.js) and by
  // this script whenever the visitor is on /chat. A short sessionStorage
  // cache keeps page-to-page navigation from re-fetching every load.
  const CHAT_ACTIVITY_ENDPOINT = "/api/chat/activity";
  const CHAT_ACTIVITY_STORAGE = "forkmesh.chatActivity";
  const CHAT_ACTIVITY_TTL_MS = 5 * 60 * 1000;
  const CHAT_ACTIVITY_SEEN_KEY = "forkmesh.chat.activitySeen";
  const CHAT_READS_ENDPOINT = "/api/chat/reads";
  const CHAT_GENERAL_READ_KEY = "#general";
  const CHAT_READ_CLOCK_SLACK_MS = 5 * 60 * 1000;

  async function fetchChatActivity() {
    try {
      const cached = JSON.parse(sessionStorage.getItem(CHAT_ACTIVITY_STORAGE) || "null");
      if (cached && Number(cached.expiresAt) > Date.now()) return cached;
    } catch (_) {}
    try {
      const response = await fetch(appApiUrl(CHAT_ACTIVITY_ENDPOINT), {
        headers: { accept: "application/json" },
      });
      if (!response.ok) return null;
      const data = await response.json();
      if (!data || !data.ok) return null;
      const activity = {
        messageCount: Number(data.messageCount) || 0,
        userCount: Number(data.userCount) || 0,
        latestMessageTs: Number(data.latestMessageTs) || 0,
        expiresAt: Date.now() + CHAT_ACTIVITY_TTL_MS,
      };
      try {
        sessionStorage.setItem(CHAT_ACTIVITY_STORAGE, JSON.stringify(activity));
      } catch (_) {}
      return activity;
    } catch (_) {
      return null;
    }
  }

  function writeChatSeen(activity) {
    try {
      localStorage.setItem(CHAT_ACTIVITY_SEEN_KEY, JSON.stringify({
        messageCount: activity.messageCount,
        userCount: activity.userCount,
        at: Date.now(),
      }));
    } catch (_) {}
  }

  async function readGeneralReadMark() {
    const session = readSession();
    if (!session) return null;
    const token = String(session.sessionToken || "");
    try {
      const response = await fetch(appApiUrl(CHAT_READS_ENDPOINT), {
        headers: {
          accept: "application/json",
          ...(token && token !== "cookie"
            ? { authorization: `Bearer ${token}` }
            : {}),
        },
        credentials: "include",
        cache: "no-store",
      });
      if (!response.ok) return null;
      const data = await response.json();
      return {
        ts: Number(data?.marks?.[CHAT_GENERAL_READ_KEY]) || 0,
        at: Number(data?.readAt?.[CHAT_GENERAL_READ_KEY]) || 0,
      };
    } catch (_) {
      return null;
    }
  }

  function readCoversActivity(mark, activity) {
    return Boolean(mark) && activity.latestMessageTs > 0 &&
      mark.at >= activity.latestMessageTs &&
      mark.ts >= activity.latestMessageTs - CHAT_READ_CLOCK_SLACK_MS;
  }

  async function renderChatBadge(header) {
    const badge = header.querySelector(".fm-header-chat-badge");
    if (!badge || location.protocol === "file:") return;
    const activity = await fetchChatActivity();
    if (!activity) return;
    const path = location.pathname.replace(/\.html$/, "").replace(/\/$/, "") || "/";
    let seen = null;
    try {
      seen = JSON.parse(localStorage.getItem(CHAT_ACTIVITY_SEEN_KEY) || "null");
    } catch (_) {}
    // On the chat page everything is on screen; and a first-time visitor has
    // no baseline, so seed one silently instead of badging all history.
    if (path === "/chat" || !seen) {
      writeChatSeen(activity);
      badge.hidden = true;
      return;
    }
    let unread =
      Math.max(0, activity.messageCount - (Number(seen.messageCount) || 0)) +
      Math.max(0, activity.userCount - (Number(seen.userCount) || 0));
    if (unread > 0 && readCoversActivity(await readGeneralReadMark(), activity)) {
      writeChatSeen(activity);
      unread = 0;
    }
    if (unread > 0) {
      badge.textContent = unread > 999 ? "999+" : String(unread);
      badge.hidden = false;
    } else {
      badge.hidden = true;
    }
  }

  function mountHeader(mount) {
    const holder = document.createElement("div");
    holder.innerHTML = HEADER_HTML;
    const header = holder.querySelector("header");
    const panel = header.querySelector(".fm-header-mobile");
    decorateNavIcons(panel);
    mount.replaceWith(header);

    const renderAccountAreas = () => {
      const current = readSession();
      if (!current) {
        membershipProjection = null;
        membershipProjectionPromise = null;
      }
      buildAccountArea(header.querySelector(".fm-header-account"), current);
      buildAccountArea(panel.querySelector(".fm-header-mobile-account"), current,
                       { stacked: true });
    };
    renderAccountAreas();
    window.ForkMeshAPI?.refreshStatusPage?.();
    if (readSession()) {
      // Not forced: the five-minute sessionStorage cache above exists for
      // exactly this read, and forcing it made every navigation of a
      // signed-in reader a fresh authenticated request anyway. Revocation is
      // still caught by the focus/visibility watch and the sign-out broadcast.
      fetchMembershipProjection(false).then(renderAccountAreas);
    } else if (window.ForkMeshAPI?.restoreSession) {
      // Marketing/blog storage is origin-scoped. Exchange the App's shared,
      // HttpOnly portable cookie for a non-secret local marker so these pages
      // accurately reflect an already signed-in browser.
      window.ForkMeshAPI.restoreSession().then((session) => {
        if (session) adoptRestoredSession(renderAccountAreas);
      });
    }
    startSessionWatch(renderAccountAreas);
    // The chat page's status helper loads alongside this header; repaint the
    // chip (away dot, status emoji, menu items) when this member's own status
    // changes — not on every other member's frame, which would snap an open
    // dropdown shut.
    const watchUserStatus = () => {
      const userStatus = window.ForkMeshUserStatus;
      if (!userStatus) return;
      let painted = "";
      const repaint = () => {
        const next = JSON.stringify(userStatus.mine());
        if (next === painted) return;
        painted = next;
        renderAccountAreas();
      };
      userStatus.subscribe(repaint);
      userStatus.loadMine();
      repaint();
    };
    if (window.ForkMeshUserStatus) watchUserStatus();
    else window.addEventListener("forkmesh:user-status-ready", watchUserStatus,
                                 { once: true });
    // Keep every open section in agreement about the login state: a login or
    // logout in another tab (dashboard, chat, home, …) fires a storage event
    // here, so this header flips between Sign Up / Log In and the account chip
    // without a manual reload.
    window.addEventListener("storage", (event) => {
      if (event.key === "forkmesh.session" || event.key === null) {
        renderAccountAreas();
      }
    });
    window.addEventListener("forkmesh:session-restored", () => {
      adoptRestoredSession(renderAccountAreas);
    });

    markCurrentPage(header);

    // Current page title next to the brand, mirroring the dashboard's header
    // context ("Dashboard"). Prefer the menu link for this path; fall back to
    // the leading segment of the document title ("Pricing - ForkMesh").
    const context = header.querySelector(".fm-header-context");
    const current = panel.querySelector('a[aria-current="page"]');
    const fromTitle = (document.title || "").split(/\s+[·|\-–—]\s+/)[0].trim();
    context.textContent = current
      ? current.textContent.trim()
      : (fromTitle && fromTitle !== "ForkMesh" ? fromTitle : "");

    const themeButton = header.querySelector(".fm-header-theme");
    themeButton.addEventListener("click", () => {
      saveTheme(document.documentElement.classList.contains("light")
                ? "dark" : "light");
    });
    applySiteTheme(resolveTheme()); // sync the freshly-rendered button's icon

    const burger = header.querySelector(".fm-header-burger");
    const setMenuOpen = (open) => {
      panel.hidden = !open;
      burger.setAttribute("aria-expanded", open ? "true" : "false");
      burger.classList.toggle("is-open", open);
    };
    burger.addEventListener("click", (event) => {
      event.stopPropagation();
      setMenuOpen(panel.hidden);
    });

    document.addEventListener("click", (event) => {
      if (!panel.hidden && !header.contains(event.target)) setMenuOpen(false);
      document.querySelectorAll(".fm-header-dropdown").forEach((dropdown) => {
        if (!dropdown.hidden && !dropdown.parentElement.contains(event.target)) {
          dropdown.hidden = true;
        }
      });
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") setMenuOpen(false);
    });

    renderAppVersion(header.querySelector(".fm-header-version"));
    renderChatBadge(header);
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState !== "visible") return;
      const badge = header.querySelector(".fm-header-chat-badge");
      if (badge && !badge.hidden) renderChatBadge(header);
    });
  }

  document.querySelectorAll("[data-forkmesh-header]").forEach(mountHeader);

  // Membership is offered, never pushed: the panel opens when a visitor asks
  // for it and never on a timer. Keep it in its own bundle because Dashboard
  // and World load it too, and fetch that bundle only on the first request so
  // header-backed pages carry no membership weight until it is wanted.
  let membershipBundle = null;

  function loadMembershipPanel() {
    if (window.ForkMeshMembershipPrompt) return Promise.resolve();
    if (!membershipBundle) {
      membershipBundle = new Promise((resolve, reject) => {
        const script = document.createElement("script");
        script.src = "/membership-prompt.js";
        script.addEventListener("load", () => resolve());
        script.addEventListener("error", () => reject(new Error("load failed")));
        document.head.appendChild(script);
      }).catch((error) => {
        membershipBundle = null;
        throw error;
      });
    }
    return membershipBundle;
  }

  function showMembership() {
    loadMembershipPanel().then(
      () => window.ForkMeshMembershipPrompt?.open(),
      // Without the panel the visitor still needs a way to join.
      () => window.open(PATREON_JOIN_HREF, "_blank", "noopener"),
    );
  }

  // Apps and extensions link to #membership; honour it on header pages too.
  if (location.hash === "#membership") showMembership();
  window.addEventListener("hashchange", () => {
    if (location.hash === "#membership") showMembership();
  });

  document.addEventListener("click", (event) => {
    const trigger = event.target?.closest?.("[data-forkmesh-membership]");
    if (!trigger) return;
    event.preventDefault();
    showMembership();
  });
})();

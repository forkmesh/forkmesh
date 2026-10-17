const APP_PATHS = new Set([
  "/chat",
  "/forgot-password",
  "/ledger",
  "/login",
  "/microjobs",
  "/mirror-payouts",
  "/referral",
  "/referrals",
  "/reset-password",
  "/signup",
  "/start",
]);

// /@name is the one App-rendered document WWW proxies instead of redirecting,
// so the public profile stays on the marketing origin. Its markup asks for the
// App's dashboard bundle by absolute path, and WWW ships neither file: the
// script would 404 and the stylesheet would 308 to another origin, where the
// `script-src 'self'`/`style-src 'self'` policy blocks it. Proxy them so they
// resolve same-origin, and stay the exact build the proxied HTML stamped.
// /qr.js is the same story one step later: the bundle fetches it at runtime to
// draw a profile's tip address, and only App stages it (from world/public).
const APP_SUBRESOURCES = new Set([
  "/dashboard.js",
  "/dashboard/tailwind.css",
  "/dashboard-composer.css",
  "/dashboard-composer.js",
  "/dashboard-plugin-host.js",
  "/user-status.js",
  "/qr.js",
]);

// Personalised hackathon documents. They carry an inviter's live standing,
// so the App Worker renders them -- but the link a participant shares is
// spelled on the apex (the referral verifier and every share card use
// forkmesh.com), so these are proxied in place rather than redirected. The
// static /hackathon, /hackathon/leaderboard and /hackathon/rules documents
// are ordinary assets on this origin, and /hackathon/me redirects to the App
// through _redirects like every other dashboard page.
const HACKATHON_APP_DOCUMENTS = /^\/hackathon\/(?:join|squad|challenge|projects)\/[^/]+\/?$/;
// Investor deck links the CRM mints for one contact each. The App renders
// the document (it is stamped with the reader's name and reports page views
// under its token), and the link is spelled on the apex like every share
// link, so it is proxied in place rather than redirected.
const PITCH_DECK_DOCUMENTS = /^\/pitch-deck\/[a-z0-9]{10,40}\/?$/;
const ECOSYSTEM_PROJECT = /^\/ecosystem\/[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?\/?$/;
const MARKET_PACKAGE = /^\/market\/(?!previews\/?$)(?=[^/]{1,96}\/?$)[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*\/?$/;

const WWW_PREFIXES = ["/assets/", "/favicon/", "/blog/", "/docs/", "/market/previews/"];
const FEED_PATHS = new Set([
  "/rss.xml",
  "/feed.xml",
  "/blog/rss.xml",
  "/blog/feed.xml",
]);
const RSS_HEADERS = {
  "cache-control": "public, max-age=1800",
  "content-type": "application/rss+xml; charset=utf-8",
};
// Acquisition attribution (docs/attribution.md). Marketing HTML is served
// from static assets, so the only server-side place a landing page can be
// observed is here. The cookie holds one opaque random id and nothing else;
// UTM parsing, allowlisting and storage all happen in the App Worker, which
// receives one observation per navigation inside waitUntil so the page never
// waits on it. DNT, Sec-GPC and the configured skip countries (EU/EEA until a
// consent banner exists) get the page with no cookie and no observation.
const ATTRIBUTION_COOKIE = "fm_attr";
const ATTRIBUTION_COOKIE_MAX_AGE = 90 * 24 * 60 * 60;
const ATTRIBUTION_CAPTURE_PATH = "/api/attribution/capture";
const NOT_FOUND_RELAY_PATH = "/api/not-found";
const ATTRIBUTION_SESSION_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const ATTRIBUTION_DEFAULT_SKIP_COUNTRIES = [
  "AT", "BE", "BG", "HR", "CY", "CZ", "DK", "EE", "FI", "FR", "DE", "GR",
  "HU", "IE", "IT", "LV", "LT", "LU", "MT", "NL", "PL", "PT", "RO", "SK",
  "SI", "ES", "SE", "IS", "LI", "NO",
];
// The visitor's address and country travel only in the signed
// x-forkmesh-edge-client header (see appHeaders), never as Cf-* headers.
const ATTRIBUTION_FORWARDED_HEADERS = [
  "accept", "user-agent", "referer", "dnt", "sec-gpc", "sec-fetch-dest",
  "sec-fetch-mode",
];
const HOME_AVAILABILITY_PATH = "/api/home-availability";
const HOME_AVAILABILITY_CACHE_SECONDS = 180;
const HOME_AVAILABILITY_PROBE_TIMEOUT_MS = 4_000;
const DIRECT_MIRRORS = Object.freeze([
  ["n1", "https://n1.next.forkmesh.com"],
  ["n2", "https://n2.next.forkmesh.com"],
  ["n3", "https://n3.next.forkmesh.com"],
]);
// Certification badges (docs/protocol/certification.md). badges.json is
// generated when evidence is attested; expiry is re-checked on every request
// so an embedded badge turns "expired" without a redeploy.
const CERTIFICATION_BADGE =
  /^\/docs\/certification\/badges\/([a-z0-9][a-z0-9-]{0,62})\/([a-z0-9][a-z0-9-]{0,62})(\.svg)?$/;
const CERTIFICATION_BADGE_DATA = "/docs/certification/badges.json";
const CERTIFICATION_COLORS = {
  certified: "#2e7d32",
  expired: "#9a6700",
  failing: "#c62828",
  revoked: "#c62828",
  unverified: "#6e7781",
};
let memoryAvailability = null;
let memoryAvailabilityAt = 0;

function redirect(origin, url, pathname = url.pathname, status = 308) {
  const destination = new URL(origin);
  destination.pathname = pathname;
  destination.search = url.search;
  return Response.redirect(destination, status);
}

// ---------------------------------------------------------------------------
// App origin. Next has no service binding: the App runs on the core nodes
// behind Caddy, reached over the public internet at APP_ORIGIN (round-robin
// A records over the three core nodes). Each node also answers under its own
// name at /_app/* (deploy/core Caddyfile), which is the fallback when the
// round-robin name does not answer.
//
// Client identity: Caddy on the core replaces every incoming Cf-* header with
// the connecting peer's address, which for this Worker is a Cloudflare egress
// address. The visitor's real address and country therefore travel in one
// HMAC-signed header, x-forkmesh-edge-client, which the App trusts only with a
// valid signature (deploy/core/edge_client.md):
//   x-forkmesh-edge-client: v1,<ip>,<country>,<unix seconds>,<base64url mac>
//   mac = HMAC-SHA256(WWW_EDGE_SECRET,
//         "v1\n<ip>\n<country>\n<ts>\n<METHOD>\n<pathname>")
// Without WWW_EDGE_SECRET no identity is forwarded at all.
const EDGE_CLIENT_HEADER = "x-forkmesh-edge-client";
const EDGE_WHERE_HEADER = "x-forkmesh-edge-where";
const EDGE_CLIENT_VERSION = "v1";
// Request headers that claim a client address or country. None of them is
// forwarded: a visitor could have set any of them, and the App must not see a
// second, unsigned opinion next to the signed one.
const CLIENT_CLAIM_HEADERS = [
  "cf-connecting-ip", "cf-connecting-ipv6", "cf-ipcountry", "cf-ray",
  "cf-visitor", "cf-worker", "cf-ew-via", "true-client-ip", "x-real-ip",
  "x-forwarded-for", "x-forwarded-host", "x-forwarded-proto", "forwarded",
  EDGE_CLIENT_HEADER, EDGE_WHERE_HEADER,
];
// Only reads (GET/HEAD) carry a header deadline and fall back to the node
// names: a write may already have been applied when the connection dropped,
// and a push can legitimately take minutes before the App answers.
const APP_FETCH_TIMEOUT_MS = 15_000;
const APP_FALLBACK_STATUSES = new Set([502, 504]);
const APP_FALLBACK_PREFIX = "/_app";

const edgeKeys = new Map();

function base64url(bytes) {
  let binary = "";
  for (const byte of new Uint8Array(bytes)) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

async function edgeKey(secret) {
  let key = edgeKeys.get(secret);
  if (!key) {
    key = crypto.subtle.importKey(
      "raw", new TextEncoder().encode(secret), { name: "HMAC", hash: "SHA-256" },
      false, ["sign"],
    );
    edgeKeys.set(secret, key);
  }
  return key;
}

function edgeClientAddress(request) {
  return String(request.headers.get("cf-connecting-ip") || "").trim().slice(0, 64);
}

function edgeClientMessage(ip, country, ts, method, pathname) {
  return [EDGE_CLIENT_VERSION, ip, country, String(ts), method.toUpperCase(), pathname].join("\n");
}

async function edgeClientValue(request, env, method, pathname, now = Date.now()) {
  const secret = String(env.WWW_EDGE_SECRET || "");
  const ip = edgeClientAddress(request);
  if (!secret || !ip || /[,\s]/.test(ip)) return "";
  const country = attributionCountry(request).replace(/[^A-Z]/g, "");
  const ts = Math.floor(now / 1000);
  const mac = await crypto.subtle.sign(
    "HMAC", await edgeKey(secret),
    new TextEncoder().encode(edgeClientMessage(ip, country, ts, method, pathname)),
  );
  return [EDGE_CLIENT_VERSION, ip, country, ts, base64url(mac)].join(",");
}

function edgeWhereValue(request) {
  const cf = request.cf || {};
  const where = new URLSearchParams();
  for (const [name, value] of [["city", cf.city], ["region", cf.region],
    ["asn", cf.asn], ["org", cf.asOrganization]]) {
    const text = String(value ?? "").replace(/[\u0000-\u001f\u007f]+/g, " ").trim().slice(0, 80);
    if (text) where.set(name, text);
  }
  return where.toString();
}

async function appHeaders(request, env, method, pathname, source = request.headers) {
  const headers = new Headers(source);
  for (const name of CLIENT_CLAIM_HEADERS) headers.delete(name);
  const edge = await edgeClientValue(request, env, method, pathname);
  if (edge) {
    headers.set(EDGE_CLIENT_HEADER, edge);
    const where = edgeWhereValue(request);
    if (where) headers.set(EDGE_WHERE_HEADER, where);
  }
  return headers;
}

function appFallbackOrigins(env) {
  const configured = String(env.APP_FALLBACK_ORIGINS || "").trim();
  if (configured) {
    return configured.split(",").map((origin) => origin.trim()).filter(Boolean);
  }
  return DIRECT_MIRRORS.map(([, origin]) => origin);
}

async function fetchWithDeadline(request, timeoutMs) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(new Error("App origin timed out")), timeoutMs);
  try {
    return await fetch(request, { signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

// Send `outgoing` (already addressed to APP_ORIGIN) to the App. Reads retry
// the per-node names when the origin is unreachable, slow, or answers with a
// gateway error; the last answer or a 502 comes back when none do.
async function appFetch(outgoing, env) {
  const method = outgoing.method.toUpperCase();
  if (method !== "GET" && method !== "HEAD") return fetch(outgoing);
  const timeoutMs = Number(env.APP_FETCH_TIMEOUT_MS) > 0
    ? Number(env.APP_FETCH_TIMEOUT_MS)
    : APP_FETCH_TIMEOUT_MS;
  const target = new URL(outgoing.url);
  let last = null;
  const origins = [null, ...appFallbackOrigins(env)];
  for (const origin of origins) {
    let attempt = outgoing;
    if (origin) {
      const fallback = new URL(APP_FALLBACK_PREFIX + target.pathname, origin);
      fallback.search = target.search;
      attempt = new Request(fallback, {
        method,
        headers: outgoing.headers,
        redirect: "manual",
      });
    }
    try {
      const response = await fetchWithDeadline(attempt, timeoutMs);
      if (!APP_FALLBACK_STATUSES.has(response.status)) return response;
      last = response;
    } catch (_error) {
      // Unreachable or past the deadline: try the next node.
    }
  }
  return last || new Response(JSON.stringify({ error: "app_unavailable" }), {
    status: 502,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "x-forkmesh-worker": "www",
    },
  });
}

async function proxyToApp(request, env, publicProfile = false) {
  const url = new URL(request.url);
  const destination = new URL(env.APP_ORIGIN);
  destination.pathname = url.pathname;
  destination.search = url.search;
  const headers = await appHeaders(request, env, request.method, url.pathname);
  if (publicProfile) headers.set("x-forkmesh-public-profile-proxy", "forkmesh.com");
  const forwarded = new Request(destination, request);
  return appFetch(new Request(forwarded, { headers, redirect: "manual" }), env);
}

function cookieValue(header, name) {
  for (const part of String(header || "").split(";")) {
    const [key, ...rest] = part.trim().split("=");
    if (key === name) return rest.join("=");
  }
  return "";
}

function isPageNavigation(request) {
  const dest = String(request.headers.get("sec-fetch-dest") || "").toLowerCase();
  const mode = String(request.headers.get("sec-fetch-mode") || "").toLowerCase();
  if (dest && dest !== "document") return false;
  if (mode && mode !== "navigate") return false;
  return String(request.headers.get("accept") || "").toLowerCase().includes("text/html");
}

function attributionCountry(request) {
  const fromEdge = request.cf && request.cf.country ? String(request.cf.country) : "";
  return (fromEdge || String(request.headers.get("cf-ipcountry") || "")).toUpperCase().slice(0, 2);
}

function attributionSkipCountries(env) {
  const configured = String(env.ATTRIBUTION_SKIP_COUNTRIES || "").trim();
  const list = configured
    ? configured.split(",").map((c) => c.trim().toUpperCase()).filter(Boolean)
    : ATTRIBUTION_DEFAULT_SKIP_COUNTRIES;
  return new Set(list);
}

function attributionDeclined(request, env) {
  if (String(request.headers.get("dnt") || "").trim() === "1") return true;
  if (String(request.headers.get("sec-gpc") || "").trim() === "1") return true;
  const country = attributionCountry(request);
  return Boolean(country) && attributionSkipCountries(env).has(country);
}

function attributionCookieDomain(env, hostname) {
  const configured = String(env.ATTRIBUTION_COOKIE_DOMAIN || "forkmesh.com")
    .trim().toLowerCase().replace(/^\./, "");
  const host = String(hostname || "").toLowerCase();
  if (!configured || !(host === configured || host.endsWith("." + configured))) return "";
  return configured;
}

async function captureAttribution(request, env, ctx, response) {
  try {
    if (request.method !== "GET" || !isPageNavigation(request)) return response;
    if (response.status !== 200) return response;
    if (!String(response.headers.get("content-type") || "").toLowerCase().includes("text/html")) {
      return response;
    }
    if (attributionDeclined(request, env)) return response;
    const url = new URL(request.url);
    let sessionId = cookieValue(request.headers.get("cookie"), ATTRIBUTION_COOKIE).toLowerCase();
    let minted = false;
    if (!ATTRIBUTION_SESSION_RE.test(sessionId)) {
      sessionId = crypto.randomUUID();
      minted = true;
    }
    const forwardedHeaders = new Headers({ "content-type": "application/json" });
    for (const name of ATTRIBUTION_FORWARDED_HEADERS) {
      const value = request.headers.get(name);
      if (value) forwardedHeaders.set(name, value);
    }
    const headers = await appHeaders(
      request, env, "POST", ATTRIBUTION_CAPTURE_PATH, forwardedHeaders);
    headers.set("x-forkmesh-attribution-relay", "www");
    const country = attributionCountry(request);
    if (country) headers.set("x-forkmesh-attribution-country", country);
    const capture = new Request(new URL(ATTRIBUTION_CAPTURE_PATH, env.APP_ORIGIN), {
      method: "POST",
      headers,
      body: JSON.stringify({
        sessionId,
        minted,
        path: url.pathname,
        search: url.search,
        host: url.hostname,
      }),
    });
    const delivery = appFetch(capture, env).then(() => undefined, () => undefined);
    if (ctx && typeof ctx.waitUntil === "function") ctx.waitUntil(delivery);
    else await delivery;
    if (!minted) return response;
    const domain = attributionCookieDomain(env, url.hostname);
    const cookie = [
      `${ATTRIBUTION_COOKIE}=${sessionId}`,
      "Path=/",
      domain ? `Domain=${domain}` : "",
      `Max-Age=${ATTRIBUTION_COOKIE_MAX_AGE}`,
      "HttpOnly",
      "Secure",
      "SameSite=Lax",
    ].filter(Boolean).join("; ");
    const withCookie = new Headers(response.headers);
    withCookie.append("set-cookie", cookie);
    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers: withCookie,
    });
  } catch (_) {
    // Attribution must never change what the visitor gets.
    return response;
  }
}

function isGitProtocol(pathname) {
  return /^\/[^/]+\/[^/]+(?:\.git)?\/(?:info\/refs|git-upload-pack|git-receive-pack|info\/lfs\/)/.test(pathname);
}

function isBackendProtocol(pathname) {
  return pathname.startsWith("/.well-known/") || pathname.startsWith("/nodeinfo/") ||
    pathname.startsWith("/ap/") || pathname.startsWith("/@") ||
    pathname.startsWith("/%40") || pathname === "/health" ||
    pathname === "/mcp" || pathname.startsWith("/mcp/");
}

function availabilityCache() {
  return globalThis.caches && globalThis.caches.default
    ? globalThis.caches.default
    : null;
}

function availabilityCacheKey() {
  return new Request("https://forkmesh.internal/home-availability-v1");
}

async function availabilityProbe(url) {
  try {
    const response = await fetch(url, {
      headers: {
        accept: "application/json",
        "user-agent": "ForkMesh-WWW-Availability/1",
      },
      redirect: "manual",
      signal: AbortSignal.timeout(HOME_AVAILABILITY_PROBE_TIMEOUT_MS),
    });
    return { online: response.status >= 200 && response.status < 300, status: response.status };
  } catch (_) {
    return { online: false, status: 0 };
  }
}

function expectedRecoveryAt(now, status) {
  if (status === 429) {
    const next = new Date(now);
    next.setUTCHours(24, 5, 0, 0);
    return next.getTime();
  }
  // For an unclassified outage, publish a deliberately short rolling ETA.
  // The cached probe updates this estimate every three minutes; it is an
  // operational expectation, not a claim that an upstream provider supplied
  // a recovery deadline.
  return now + 15 * 60 * 1000;
}

async function buildAvailability(env) {
  const checkedAt = Date.now();
  const app = await availabilityProbe(new URL("/health", env.APP_ORIGIN));
  if (app.online) {
    return {
      ok: true,
      appOnline: true,
      checkedAt,
      expectedReturnAt: null,
      mirrors: [],
    };
  }
  const probes = await Promise.all(DIRECT_MIRRORS.map(async ([name, origin]) => {
    const health = await availabilityProbe(origin + "/health");
    return health.online ? {
      name,
      url: origin + "/forkmesh/forkmesh",
    } : null;
  }));
  return {
    ok: true,
    appOnline: false,
    appStatus: app.status,
    checkedAt,
    expectedReturnAt: expectedRecoveryAt(checkedAt, app.status),
    mirrors: probes.filter(Boolean),
  };
}

async function homeAvailability(env) {
  const now = Date.now();
  if (memoryAvailability &&
      now - memoryAvailabilityAt < HOME_AVAILABILITY_CACHE_SECONDS * 1000) {
    return memoryAvailability.clone();
  }
  const cache = availabilityCache();
  const key = availabilityCacheKey();
  if (cache) {
    const cached = await cache.match(key);
    if (cached) {
      memoryAvailability = cached.clone();
      memoryAvailabilityAt = now;
      return cached;
    }
  }
  const payload = await buildAvailability(env);
  const response = Response.json(payload, {
    headers: {
      "cache-control": "public, max-age=60, stale-if-error=900",
      "x-forkmesh-worker": "www",
    },
  });
  memoryAvailability = response.clone();
  memoryAvailabilityAt = now;
  if (cache) {
    const cached = new Response(response.clone().body, {
      status: response.status,
      headers: response.headers,
    });
    cached.headers.set(
      "cache-control",
      `public, max-age=${HOME_AVAILABILITY_CACHE_SECONDS}, stale-if-error=900`,
    );
    await cache.put(key, cached);
  }
  return response;
}

function svgText(value) {
  return String(value).replace(/[&<>"']/g, (character) => `&#${character.charCodeAt(0)};`);
}

function certificationStatus(badge, now) {
  if (!badge) return "unverified";
  if (badge.status !== "certified") return badge.status;
  // expiresAt is the last valid UTC day; a missing or bad date is expired.
  const end = Date.parse(`${badge.expiresAt}T00:00:00Z`) + 86_400_000;
  return now < end ? "certified" : "expired";
}

async function certificationBadge(request, env, url, match) {
  const headers = { "x-forkmesh-worker": "www" };
  if (request.method !== "GET" && request.method !== "HEAD") {
    return new Response(null, {
      status: 405,
      headers: { ...headers, allow: "GET, HEAD", "cache-control": "no-store" },
    });
  }
  const [, implementation, profile, svg] = match;
  let badge = null;
  const source = await env.ASSETS.fetch(new Request(new URL(CERTIFICATION_BADGE_DATA, url)));
  if (source.ok) {
    try {
      const data = await source.json();
      badge = (data.badges || []).find((item) =>
        item.implementation === implementation && item.profile === profile) || null;
    } catch (_error) {
      badge = null;
    }
  }
  if (!svg) {
    if (!badge || typeof badge.evidence !== "string" || !badge.evidence.startsWith("/docs/certification/evidence/")) {
      return new Response("No certification evidence for this implementation and profile.\n", {
        status: 404,
        headers: { ...headers, "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" },
      });
    }
    return new Response(null, {
      status: 302,
      headers: { ...headers, location: new URL(badge.evidence, url).toString(), "cache-control": "public, max-age=300" },
    });
  }
  const status = certificationStatus(badge, Date.now());
  const label = `ForkMesh ${profile}`;
  const message = status === "certified" ? `certified to ${badge.expiresAt}` : status;
  const labelWidth = Math.round(label.length * 6.6) + 12;
  const messageWidth = Math.round(message.length * 6.6) + 12;
  const width = labelWidth + messageWidth;
  const body = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="20" role="img" aria-label="${svgText(`${label}: ${message}`)}">`
    + `<title>${svgText(`${implementation} · ${label}: ${message}`)}</title>`
    + `<rect width="${labelWidth}" height="20" fill="#24292f"/>`
    + `<rect x="${labelWidth}" width="${messageWidth}" height="20" fill="${CERTIFICATION_COLORS[status] || CERTIFICATION_COLORS.unverified}"/>`
    + `<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11">`
    + `<text x="6" y="14">${svgText(label)}</text>`
    + `<text x="${labelWidth + 6}" y="14">${svgText(message)}</text></g></svg>`;
  return new Response(request.method === "HEAD" ? null : body, {
    status: 200,
    headers: {
      ...headers,
      "content-type": "image/svg+xml; charset=utf-8",
      "cache-control": "public, max-age=3600",
      "content-security-policy": "default-src 'none'; style-src 'unsafe-inline'",
      "x-content-type-options": "nosniff",
      "x-forkmesh-certification-status": status,
    },
  });
}

async function reportNotFound(request, env, ctx, response) {
  if (response.status !== 404 || (request.method !== "GET" && request.method !== "HEAD")) {
    return response;
  }
  const delivery = (async () => {
    const headers = await appHeaders(
      request, env, "POST", NOT_FOUND_RELAY_PATH,
      new Headers({ "content-type": "application/json" }));
    await appFetch(new Request(new URL(NOT_FOUND_RELAY_PATH, env.APP_ORIGIN), {
      method: "POST",
      headers,
      body: JSON.stringify({
        path: new URL(request.url).pathname,
        referrer: String(request.headers.get("referer") || "").slice(0, 500),
        userAgent: String(request.headers.get("user-agent") || "").slice(0, 500),
        fetchSite: String(request.headers.get("sec-fetch-site") || "").slice(0, 20),
      }),
    }), env);
  })().then(() => undefined, () => undefined);
  if (ctx && typeof ctx.waitUntil === "function") ctx.waitUntil(delivery);
  else await delivery;
  return response;
}

async function asset(request, env, overrides = {}) {
  let response = await env.ASSETS.fetch(request);
  if (response.status === 404 && (request.method === "GET" || request.method === "HEAD")) {
    const fallbackUrl = new URL(request.url);
    fallbackUrl.pathname = "/404";
    fallbackUrl.search = "";
    const fallback = await env.ASSETS.fetch(new Request(fallbackUrl, {
      method: request.method,
      headers: request.headers,
    }));
    if (fallback.ok) {
      response = new Response(request.method === "HEAD" ? null : fallback.body, {
        status: 404,
        headers: fallback.headers,
      });
    }
  }
  const headers = new Headers(response.headers);
  for (const [name, value] of Object.entries(overrides)) headers.set(name, value);
  headers.set("x-forkmesh-worker", "www");
  headers.set("x-forkmesh-deploy-fingerprint", String(env.DEPLOY_FINGERPRINT || ""));
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;
    const publicProfile = path.startsWith("/@") || path.startsWith("/%40");

    if (publicProfile && url.hostname === "www.next.forkmesh.com") {
      return redirect("https://forkmesh.com", url);
    }

    if (FEED_PATHS.has(path)) {
      if (request.method !== "GET" && request.method !== "HEAD") {
        return new Response(null, {
          status: 405,
          headers: {
            allow: "GET, HEAD",
            "cache-control": "no-store",
            "x-forkmesh-worker": "www",
          },
        });
      }
      const assetUrl = new URL(request.url);
      assetUrl.pathname = "/rss.xml";
      return asset(new Request(assetUrl, request), env, RSS_HEADERS);
    }

    const certificationMatch = CERTIFICATION_BADGE.exec(path);
    if (certificationMatch) {
      return certificationBadge(request, env, url, certificationMatch);
    }

    if (path === HOME_AVAILABILITY_PATH) {
      if (request.method !== "GET" && request.method !== "HEAD") {
        return new Response(null, {
          status: 405,
          headers: {
            allow: "GET, HEAD",
            "cache-control": "no-store",
            "x-forkmesh-worker": "www",
          },
        });
      }
      const response = await homeAvailability(env);
      if (request.method === "HEAD") {
        return new Response(null, {
          status: response.status,
          headers: response.headers,
        });
      }
      return response;
    }

    if (path === "/api") {
      return redirect(env.APP_ORIGIN, url);
    }
    if (path.startsWith("/api/")) {
      return proxyToApp(request, env);
    }
    if (isGitProtocol(path) || isBackendProtocol(path) || APP_SUBRESOURCES.has(path)) {
      return proxyToApp(request, env, publicProfile);
    }
    if (path === "/world" || path.startsWith("/world/")) {
      return redirect(env.WORLD_ORIGIN, url, path === "/world" ? "/" : path);
    }
    if (path === "/dashboard" || path.startsWith("/dashboard/") ||
        path.startsWith("/notes/") || APP_PATHS.has(path)) {
      return redirect(env.APP_ORIGIN, url);
    }
    if (HACKATHON_APP_DOCUMENTS.test(path)) {
      return proxyToApp(request, env);
    }
    if (PITCH_DECK_DOCUMENTS.test(path)) {
      return proxyToApp(request, env);
    }
    if (request.method !== "GET" && request.method !== "HEAD") {
      return proxyToApp(request, env);
    }
    if (ECOSYSTEM_PROJECT.test(path)) {
      const pageUrl = new URL(request.url);
      pageUrl.pathname = "/ecosystem";
      pageUrl.search = "";
      return captureAttribution(request, env, ctx, await asset(new Request(pageUrl, request), env));
    }
    if (MARKET_PACKAGE.test(path)) {
      const pageUrl = new URL(request.url);
      pageUrl.pathname = "/market";
      pageUrl.search = "";
      return captureAttribution(request, env, ctx, await asset(new Request(pageUrl, request), env));
    }
    if (/^\/[^/]+\/[^/]+(?:\/.*)?$/.test(path) &&
        !WWW_PREFIXES.some((prefix) => path.startsWith(prefix))) {
      return redirect(env.APP_ORIGIN, url);
    }
    return captureAttribution(
      request, env, ctx, await reportNotFound(request, env, ctx, await asset(request, env)));
  },
};

// Optional, first-party product analytics.
//
// This file is included by every public page, but it does not load the
// tracking SDK or send a network request unless this browser has explicitly
// opted in. Events go to this instance's own Analytics package
// (/api/analytics); no third party receives anything.
//
// Public pages (home, docs, blog, marketing) are measured in full: paths,
// clicks, heatmaps, and, when the project enables it, session replay with
// every input masked. Signed-in and repository surfaces can show private
// names, end-to-end encrypted chat, and notes, so there ForkMesh sends only a
// coarse page category: no path, no element text, no heatmap, no replay.
(() => {
  "use strict";

  const CONSENT_KEY = "forkmesh.analyticsConsent.v1";
  // Resolves to the project an instance administrator marked as the site's.
  const PROJECT_KEY = "site";
  const SDK_SRC = "/analytics-sdk.js";
  const CLIENT_ERROR_ENDPOINT = "/api/client-errors";
  const CLIENT_ERROR_WINDOW_MS = 60_000;
  const CLIENT_ERROR_MAX_PER_WINDOW = 8;
  let started = false;
  let clientErrorWindowStartedAt = 0;
  let clientErrorWindowCount = 0;
  const recentClientErrors = new Map();
  const PRODUCT_ACTIONS = new Set([
    "commercial-proof-opened",
    "home-capabilities-world-opened",
    "home-install-final",
    "home-install-hero",
    "home-install-nav",
    "home-pro-membership",
    "home-proof-opened",
    "onboarding-proof-opened",
    "pricing-contract-opened",
    "pricing-managed-interest",
    "proof-clone-copied",
    "repository-clone-copied",
    "repository-proof-opened",
  ]);

  function privacySignalEnabled() {
    const dnt = String(
      navigator.doNotTrack ||
        window.doNotTrack ||
        navigator.msDoNotTrack ||
        "",
    ).toLowerCase();
    return navigator.globalPrivacyControl === true || dnt === "1" || dnt === "yes";
  }

  function storedConsent() {
    try {
      return localStorage.getItem(CONSENT_KEY) || "unset";
    } catch (_) {
      return "unset";
    }
  }

  function saveConsent(value) {
    try {
      localStorage.setItem(CONSENT_KEY, value);
      return true;
    } catch (_) {
      return false;
    }
  }

  // Deliberately map paths to a small fixed vocabulary. In particular,
  // /owner/repository, private deep links, issue ids, searches, and query
  // parameters never leave the browser.
  function coarseSurface(pathname) {
    const path = String(pathname || "/").toLowerCase();
    if (path === "/") return "home";
    if (path === "/world" || path.startsWith("/world/")) return "world";
    if (path === "/dashboard" || path.startsWith("/dashboard/")) return "dashboard";
    if (path === "/docs" || path.startsWith("/docs/")) return "documentation";
    if (path === "/blog" || path.startsWith("/blog/")) return "blog";
    if (path === "/network" || path.startsWith("/network/")) return "network";
    if (path === "/chat" || path.startsWith("/chat/")) return "chat";
    if (
      path === "/login" ||
      path === "/signup" ||
      path === "/forgot-password" ||
      path === "/reset-password"
    ) {
      return "account";
    }
    const parts = path.split("/").filter(Boolean);
    if (parts.length >= 2) return "repository";
    return "public-page";
  }

  function redactClientError(value, maximum = 500) {
    return String(value || "")
      .replace(
        /\b(?:authorization|bearer|password|passwd|secret|token|api[_-]?key)\b\s*[:=]?\s*[^\s,;]+/gi,
        "[redacted-secret]",
      )
      .replace(
        /\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b/gi,
        "[redacted-email]",
      )
      .replace(/\b[A-Za-z0-9_-]{32,}\b/g, "[redacted-id]")
      .replace(/\bhttps?:\/\/[^\s)]+/gi, "[redacted-url]")
      .replace(
        /\/(?:[A-Za-z0-9._~%-]+\/)+[A-Za-z0-9._~%-]+(?:[?#][^\s]*)?/g,
        "/[redacted-path]",
      )
      .replace(/[\u0000-\u001f\u007f]+/g, " ")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, maximum);
  }

  function clientErrorSource(value) {
    const withoutQuery = String(value || "").split(/[?#]/, 1)[0];
    const source = withoutQuery.split("/").filter(Boolean).pop() || "";
    return source.replace(/[^A-Za-z0-9._-]+/g, "").slice(0, 80);
  }

  // Extensions and injected userscripts run inside the page, so their uncaught
  // exceptions reach window.onerror looking exactly like ours. Their frames are
  // never a script this origin served: they are extension-scheme URLs, or bare
  // "<anonymous>" for code installed through eval/new Function. One such family
  // — two copies of an extension wrapping addEventListener and then recursing
  // into "Maximum call stack size exceeded" — filled the operational error
  // groups with a defect no ForkMesh change can fix.
  const INJECTED_FRAME =
    /^(?:<anonymous>|[a-z][a-z0-9-]*-extension:|iabjs:|blob:|data:|about:|resource:|webkit-masked-url:)/i;
  const STACK_FRAME = /<anonymous>|[a-z][a-z0-9.+-]*:[^\s)'"]+/gi;

  function injectedOnlyOrigin(stack, source) {
    // A stack outranks the filename: browsers sometimes attribute an evaluated
    // extension frame to the document that happened to be loaded.
    const frames = String(stack || "").match(STACK_FRAME) || [];
    if (frames.length) return frames.every((frame) => INJECTED_FRAME.test(frame));
    const filename = String(source || "").trim();
    return filename ? INJECTED_FRAME.test(filename) : false;
  }

  function knownNonActionableBrowserError(error, fallbackMessage = "") {
    const message = String(error?.message || fallbackMessage || "").trim();
    // "Script error." is the deliberately opaque cross-origin onerror value:
    // it has no filename, stack, or line that can identify code we control.
    // ResizeObserver delivery warnings likewise describe the browser deferring
    // a resize callback to its next frame, not a failed application operation.
    return (
      /^script error\.?$/i.test(message) ||
      /^resizeobserver loop (?:limit exceeded|completed with undelivered notifications)\.?$/i.test(
        message,
      ) ||
      /^invalid call to runtime\.sendmessage\(\)\. tab not found\.?$/i.test(message)
    );
  }

  function reportClientError(kind, error, fallbackMessage = "", source = "", line = 0, column = 0) {
    // Classify before redaction, which rewrites the very URLs that say where a
    // frame came from. Dropped reports also must not spend the send budget.
    if (knownNonActionableBrowserError(error, fallbackMessage)) return;
    if (injectedOnlyOrigin(error?.stack, source)) return;
    const now = Date.now();
    if (
      !clientErrorWindowStartedAt ||
      now - clientErrorWindowStartedAt >= CLIENT_ERROR_WINDOW_MS
    ) {
      clientErrorWindowStartedAt = now;
      clientErrorWindowCount = 0;
    }
    if (clientErrorWindowCount >= CLIENT_ERROR_MAX_PER_WINDOW) return;
    const message = redactClientError(
      error?.message || fallbackMessage || "Unspecified browser exception",
      500,
    );
    const stack = redactClientError(
      String(error?.stack || "")
        .split("\n")
        .slice(0, 8)
        .join("\n"),
      700,
    );
    const surface = coarseSurface(location.pathname);
    const signature = `${kind}|${surface}|${message}|${stack.slice(0, 180)}`;
    const previous = recentClientErrors.get(signature) || 0;
    if (now - previous < CLIENT_ERROR_WINDOW_MS) return;
    recentClientErrors.set(signature, now);
    for (const [key, seenAt] of recentClientErrors) {
      if (now - seenAt >= CLIENT_ERROR_WINDOW_MS) recentClientErrors.delete(key);
    }
    clientErrorWindowCount += 1;
    fetch(CLIENT_ERROR_ENDPOINT, {
      method: "POST",
      credentials: "same-origin",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind,
        surface,
        message,
        stack,
        source: clientErrorSource(source),
        line: Math.max(0, Number(line) || 0),
        column: Math.max(0, Number(column) || 0),
      }),
      keepalive: true,
    }).catch(() => {});
  }

  window.addEventListener(
    "error",
    (event) => {
      // Resource-load errors do not expose a useful exception and can reveal
      // asset paths. The operational collector is for uncaught JavaScript.
      if (event.target && event.target !== window) return;
      reportClientError(
        "error",
        event.error,
        event.message,
        event.filename,
        event.lineno,
        event.colno,
      );
    },
    true,
  );
  window.addEventListener("unhandledrejection", (event) => {
    const reason = event.reason;
    // World navigation and mobile gesture reconciliation intentionally abort
    // stale fetches. Browsers surface those cancellations as unhandled
    // AbortError rejections even though no request or UI action failed.
    if (
      reason?.name === "AbortError" ||
      /signal is aborted|operation was aborted/i.test(
        String(reason?.message || ""),
      )
    ) {
      return;
    }
    const errorLike = rejectionErrorLike(reason);
    setTimeout(() => {
      if (event.defaultPrevented) return;
      reportClientError(
        "unhandledrejection",
        errorLike,
        errorLike ? "" : describeRejectionReason(reason),
      );
    }, 0);
  });

  function rejectionErrorLike(reason) {
    if (reason instanceof Error) {
      return reason.message
        ? reason
        : { message: `${reason.name || "Error"} with no message`, stack: reason.stack };
    }
    if (
      reason &&
      typeof reason === "object" &&
      typeof reason.message === "string" &&
      reason.message &&
      (typeof reason.stack === "string" || typeof reason.name === "string")
    ) {
      return { message: reason.message, stack: String(reason.stack || "") };
    }
    return null;
  }

  function rejectionTypeName(reason) {
    try {
      const name = Object.getPrototypeOf(reason)?.constructor?.name;
      return typeof name === "string" && /^[A-Za-z_$][\w$]{0,40}$/.test(name)
        ? name
        : "Object";
    } catch (_) {
      return "Object";
    }
  }

  function describeRejectionReason(reason) {
    const prefix = "Unhandled promise rejection";
    if (typeof reason === "string") return reason || `${prefix}: empty string`;
    if (reason === undefined || reason === null) return `${prefix}: ${reason}`;
    if (typeof reason !== "object" && typeof reason !== "function") {
      return `${prefix}: ${typeof reason} value`;
    }
    const type = rejectionTypeName(reason);
    try {
      if (typeof Event === "function" && reason instanceof Event) {
        const target = reason.target;
        const element = target && typeof target.tagName === "string"
          ? target.tagName.toLowerCase()
          : target === window ? "window" : rejectionTypeName(target || {});
        return `${prefix}: ${type} "${String(reason.type || "").slice(0, 40)}" from ${element}`;
      }
      if (typeof Response === "function" && reason instanceof Response) {
        return `${prefix}: Response status ${Number(reason.status) || 0}`;
      }
    } catch (_) {}
    let keys = [];
    try {
      keys = Object.keys(reason)
        .filter((key) => /^[A-Za-z_$][\w$]{0,40}$/.test(key))
        .sort()
        .slice(0, 8);
    } catch (_) {}
    const code = typeof reason.code === "string" || typeof reason.code === "number"
      ? `; code ${String(reason.code).slice(0, 40)}`
      : "";
    const error = typeof reason.error === "string"
      ? `; error ${reason.error.slice(0, 80)}`
      : "";
    return `${prefix}: ${type}${keys.length ? ` with keys ${keys.join(", ")}` : " with no own keys"}${code}${error}`;
  }

  // Surfaces whose documents are public to every visitor. Everything else may
  // render private names or decrypted content and stays coarse.
  const PUBLIC_SURFACES = new Set([
    "home",
    "documentation",
    "blog",
    "public-page",
  ]);
  const PRIVATE_SURFACE_PROPERTIES =
    /^\$(?:current_url|pathname|title|referrer|referring_domain|el_text|elements)$/;

  function publicSurface() {
    return PUBLIC_SURFACES.has(coarseSurface(location.pathname));
  }

  // Runs on every event. A private surface keeps the coarse category in place
  // of its URL, path, and title; a public one drops only the query and hash.
  function sanitizeProperties(properties) {
    const surface = coarseSurface(location.pathname);
    const result = { ...properties, surface, consent: "explicit" };
    if (PUBLIC_SURFACES.has(surface)) return result;
    for (const key of Object.keys(result)) {
      if (PRIVATE_SURFACE_PROPERTIES.test(key)) delete result[key];
    }
    result.$pathname = `/(${surface})`;
    return result;
  }

  let sdk = null;
  let sdkLoading = null;

  function loadSdk() {
    if (window.ForkMeshAnalyticsSDK) return Promise.resolve(window.ForkMeshAnalyticsSDK);
    if (sdkLoading) return sdkLoading;
    sdkLoading = new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = SDK_SRC;
      script.async = true;
      script.onload = () => resolve(window.ForkMeshAnalyticsSDK);
      script.onerror = () => reject(new Error("analytics SDK unavailable"));
      document.head.appendChild(script);
    }).catch(() => {
      sdkLoading = null;
      return null;
    });
    return sdkLoading;
  }

  function start() {
    if (started || storedConsent() !== "granted" || privacySignalEnabled()) {
      return false;
    }
    started = true;
    const detailed = publicSurface();
    loadSdk().then((library) => {
      // Consent may have been withdrawn while the script was in flight.
      if (!library || !started) return;
      sdk = library.init({
        key: PROJECT_KEY,
        apiHost: location.origin,
        persistence: "localStorage",
        respectDoNotTrack: true,
        maskUrlQuery: true,
        capturePageview: true,
        capturePageleave: detailed,
        autocapture: detailed,
        heatmaps: detailed,
        sessionRecording: detailed,
        surveys: detailed,
        // /api/client-errors above is the operational error collector.
        captureErrors: false,
        sanitizeProperties,
      });
    });
    return true;
  }

  function stop() {
    if (sdk) {
      sdk.optOut();
      sdk.shutdown();
      sdk = null;
    }
    started = false;
  }

  function setConsent(granted) {
    if (granted && privacySignalEnabled()) {
      saveConsent("denied");
      stop();
      window.dispatchEvent(
        new CustomEvent("forkmesh:analytics-consent", {
          detail: { status: "blocked-by-privacy-signal" },
        }),
      );
      return "blocked-by-privacy-signal";
    }
    const status = granted ? "granted" : "denied";
    saveConsent(status);
    if (granted) start();
    else stop();
    window.dispatchEvent(
      new CustomEvent("forkmesh:analytics-consent", {
        detail: { status },
      }),
    );
    return status;
  }

  function status() {
    if (privacySignalEnabled()) return "blocked-by-privacy-signal";
    return storedConsent();
  }

  function recordProductAction(action) {
    if (!started || !sdk || !PRODUCT_ACTIONS.has(action)) return false;
    sdk.capture("forkmesh_product_action", { action });
    return true;
  }

  document.addEventListener("click", (event) => {
    const target = event.target?.closest?.("[data-forkmesh-product-action]");
    if (!target) return;
    recordProductAction(target.dataset.forkmeshProductAction || "");
  });

  const PROMPT_ID = "fm-analytics-consent";
  const PROMPT_STYLE = `
    .fm-consent { position: fixed; z-index: 2147483000; left: 16px; bottom: 16px; max-width: 380px;
      margin: 0; padding: 14px 16px; border-radius: 12px; border: 1px solid rgb(240 246 252 / 0.16);
      background: #0d1117; color: #e6edf3; box-shadow: 0 12px 32px rgb(0 0 0 / 0.45);
      font: 13px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
    html.light .fm-consent { background: #ffffff; color: #1f2328; border-color: rgb(31 35 40 / 0.15);
      box-shadow: 0 12px 32px rgb(31 35 40 / 0.18); }
    .fm-consent p { margin: 0 0 10px; }
    .fm-consent a { color: inherit; text-decoration: underline; }
    .fm-consent-actions { display: flex; justify-content: flex-end; gap: 8px; }
    .fm-consent button { min-height: 36px; padding: 6px 14px; border-radius: 999px; font: inherit;
      font-weight: 600; cursor: pointer; border: 1px solid currentColor; background: transparent; color: inherit; }
    .fm-consent button[data-choice="grant"] { background: #238636; border-color: #238636; color: #ffffff; }
    @media (max-width: 480px) { .fm-consent { left: 8px; right: 8px; bottom: 8px; max-width: none; } }`;

  function promptWanted() {
    if (storedConsent() !== "unset" || privacySignalEnabled() || !publicSurface()) return false;
    if (location.pathname.replace(/\/+$/, "") === "/privacy") return false;
    if (navigator.webdriver) return false;
    try {
      return window.top === window;
    } catch (_) {
      return false;
    }
  }

  function closePrompt() {
    document.getElementById(PROMPT_ID)?.remove();
  }

  function showPrompt() {
    if (!promptWanted() || !document.body || document.getElementById(PROMPT_ID)) return;
    const box = document.createElement("section");
    box.id = PROMPT_ID;
    box.className = "fm-consent";
    box.setAttribute("role", "region");
    box.setAttribute("aria-label", "Analytics choice");
    const style = document.createElement("style");
    style.textContent = PROMPT_STYLE;
    const text = document.createElement("p");
    text.textContent =
      "Help us see which pages and posts are useful? Analytics is off until you allow it. It stays on ForkMesh's own servers, stores no IP address, and masks what you type. ";
    const details = document.createElement("a");
    details.href = "/privacy#analytics-choice-heading";
    details.textContent = "Details";
    text.append(details);
    const actions = document.createElement("div");
    actions.className = "fm-consent-actions";
    for (const [choice, label] of [["deny", "No thanks"], ["grant", "Allow analytics"]]) {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.choice = choice;
      button.textContent = label;
      button.addEventListener("click", () => {
        setConsent(choice === "grant");
        closePrompt();
      });
      actions.append(button);
    }
    box.append(style, text, actions);
    document.body.append(box);
  }

  window.addEventListener("forkmesh:analytics-consent", closePrompt);

  window.ForkMeshAnalytics = Object.freeze({
    recordProductAction,
    setConsent,
    status,
  });

  // Default is off. This is the sole automatic start path and it requires a
  // previously stored explicit grant on this device.
  if (storedConsent() === "granted" && !privacySignalEnabled()) start();
  else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", showPrompt, { once: true });
  } else {
    showPrompt();
  }
})();

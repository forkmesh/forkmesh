// ForkMesh extension: the bridge on the ForkMesh dashboard pages that work
// with it (CRM, Password Vault, Marketing, and the extension's own page).
//
// Tells the page which extension version is installed, so it can offer an
// update, and opens chrome://extensions when the page asks: Chrome refuses
// that address from a web page but lets an extension open it. On Marketing it
// also passes "update this channel", "walk the Reddit watch list", "update
// my Reddit posts" and "update the logged threads" to the extension, and their
// progress back.
// Nothing else crosses: no storage, no token, no vault key, no requests.

(() => {
  const version = chrome.runtime.getManifest().version;
  document.documentElement.dataset.forkmeshExtension = version;
  // The CRM page has read this name since extension 1.7.0.
  document.documentElement.dataset.forkmeshCrmExtension = version;
  const marketing = location.pathname.startsWith("/dashboard/marketing");
  const crm = location.pathname.startsWith("/dashboard/crm");
  if (crm) document.documentElement.dataset.forkmeshCrmZohoSetup = version;
  if (marketing) {
    document.documentElement.dataset.forkmeshMarketingCapture = version;
    document.documentElement.dataset.forkmeshMarketingXSetup = version;
    document.documentElement.dataset.forkmeshMarketingRedditWalk = version;
  }
  for (const name of ["forkmesh-extension", "forkmesh-crm-extension"]) {
    window.dispatchEvent(new CustomEvent(name, { detail: { version } }));
  }

  const PAGES = ["forkmesh-crm-page", "forkmesh-extension-page", "forkmesh-marketing-page"];
  const REDDIT_WALK = {
    "marketing-reddit-walk": "fm-reddit-walk",
    "marketing-reddit-walk-state": "fm-reddit-walk-state",
    "marketing-reddit-walk-stop": "fm-reddit-walk-stop",
  };
  const reply = (message) => window.postMessage({ source: "forkmesh-extension", ...message }, location.origin);
  window.addEventListener("message", (event) => {
    if (event.source !== window || event.origin !== location.origin) return;
    if (!PAGES.includes(event.data?.source)) return;
    if (event.data.type === "open-extensions") {
      chrome.runtime.sendMessage({ type: "fm-open-extensions" }).catch(() => null);
      return;
    }
    if (crm && event.data.source === "forkmesh-crm-page" && ["crm-zoho-setup", "crm-zoho-setup-stop"].includes(event.data.type)) {
      const request = String(event.data.request || "").slice(0, 64);
      const type = event.data.type === "crm-zoho-setup" ? "fm-zoho-setup-start" : "fm-zoho-setup-stop";
      chrome.runtime.sendMessage({ type, organization: String(event.data.organization || "").slice(0, 64) })
        .then((response) => reply({ type: event.data.type, request, response: response || { ok: false } }))
        .catch((error) => reply({ type: event.data.type, request, response: { ok: false, error: "extension_unavailable", message: String(error?.message || "") } }));
      return;
    }
    if (!marketing || event.data.source !== "forkmesh-marketing-page") return;
    if (event.data.type === "marketing-hello") {
      reply({ type: "marketing-ready", version, xSetup: true, redditWalk: true, redditPosts: true, redditThreads: true });
      return;
    }
    if (REDDIT_WALK[event.data.type]) {
      const request = String(event.data.request || "").slice(0, 64);
      chrome.runtime.sendMessage({ type: REDDIT_WALK[event.data.type], ...(event.data.rulesOnly === true ? { rulesOnly: true } : {}) })
        .then((response) => reply({ type: event.data.type, request, response: response || { ok: false } }))
        .catch((error) => reply({ type: event.data.type, request, response: { ok: false, error: "extension_unavailable", message: String(error?.message || "") } }));
      return;
    }
    if (event.data.type === "marketing-reddit-posts-update") {
      const request = String(event.data.request || "").slice(0, 64);
      chrome.runtime.sendMessage({ type: "fm-reddit-accounts-update", organization: String(event.data.organization || "").slice(0, 64) })
        .then((response) => reply({ type: "marketing-reddit-posts-update", request, response: response || { ok: false } }))
        .catch((error) => reply({ type: "marketing-reddit-posts-update", request, response: { ok: false, error: "extension_unavailable", message: String(error?.message || "") } }));
      return;
    }
    if (event.data.type === "marketing-reddit-threads-update") {
      const request = String(event.data.request || "").slice(0, 64);
      const threadIds = (Array.isArray(event.data.threadIds) ? event.data.threadIds : []).slice(0, 200).map((id) => String(id).slice(0, 64));
      chrome.runtime.sendMessage({ type: "fm-reddit-threads-update", organization: String(event.data.organization || "").slice(0, 64), threadIds })
        .then((response) => reply({ type: "marketing-reddit-threads-update", request, response: response || { ok: false } }))
        .catch((error) => reply({ type: "marketing-reddit-threads-update", request, response: { ok: false, error: "extension_unavailable", message: String(error?.message || "") } }));
      return;
    }
    if (event.data.type === "marketing-x-setup") {
      const request = String(event.data.request || "").slice(0, 64);
      chrome.runtime.sendMessage({ type: "fm-x-setup-start", organization: String(event.data.organization || "").slice(0, 64) })
        .then((response) => reply({ type: "marketing-x-setup", request, response: response || { ok: false } }))
        .catch((error) => reply({ type: "marketing-x-setup", request, response: { ok: false, error: "extension_unavailable", message: String(error?.message || "") } }));
      return;
    }
    if (event.data.type !== "marketing-capture") return;
    const request = String(event.data.request || "").slice(0, 64);
    chrome.runtime.sendMessage({
      type: "fm-marketing-capture",
      organization: String(event.data.organization || "").slice(0, 64),
      channel: String(event.data.channel || "").slice(0, 32),
    }).then((response) => reply({ type: "marketing-capture", request, response: response || { ok: false } }))
      .catch((error) => reply({ type: "marketing-capture", request, response: { ok: false, error: "extension_unavailable", message: String(error?.message || "") } }));
  });

  if (crm) {
    chrome.runtime.onMessage.addListener((message) => {
      if (message?.type === "fm-zoho-setup-update") reply({ type: "crm-zoho-setup-update", setup: message.setup });
    });
  }

  if (marketing) {
    chrome.runtime.onMessage.addListener((message) => {
      if (message?.type === "fm-marketing-capture-update") reply({ type: "marketing-capture-update", job: message.job });
      if (message?.type === "fm-x-setup-update") reply({ type: "marketing-x-setup-update", setup: message.setup });
      if (message?.type === "fm-reddit-walk-update") reply({ type: "marketing-reddit-walk-update", walk: message.walk });
      if (message?.type === "fm-reddit-posts-progress") reply({ type: "marketing-reddit-posts-progress", check: message.check });
      if (message?.type === "fm-reddit-threads-progress") reply({ type: "marketing-reddit-threads-progress", update: message.update });
    });
    reply({ type: "marketing-ready", version, xSetup: true, redditWalk: true, redditPosts: true, redditThreads: true });
  }
})();

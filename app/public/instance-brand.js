(() => {
  const script = document.currentScript;
  const name = (script && script.dataset.instanceName || "").trim();
  if (!name || name === "ForkMesh") return;
  const BRAND = /(?<![A-Za-z0-9_.-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)/g;
  const SKIP = new Set(["SCRIPT", "STYLE", "TEXTAREA", "CODE", "PRE", "KBD", "SAMP", "NOSCRIPT"]);
  const ATTRIBUTES = ["title", "aria-label", "placeholder", "alt"];
  let hiddenPaths = [];
  let offsiteHosts = [];
  let ready = false;
  let legal = null;
  const LEGAL_PATHS = { "/terms": "terms", "/privacy": "privacy", "/security": "security", "/security-report": "security" };
  const LEGAL_PENDING =
    Object.keys(LEGAL_PATHS)
      .map((path) => `:is(header, nav, aside, footer) a[href="${path}"]:not([data-instance-legal])`)
      .concat(["[data-instance-legal-consent]:not([data-instance-legal])", "[data-instance-legal-group]:not([data-instance-legal])"])
      .join(", ") + " { display: none !important; }";

  const DESKTOP = /(?<![A-Za-z0-9_.-])ForkMesh (?=[Dd]esktop)/g;
  const swap = (value) => value.replace(DESKTOP, "").replace(BRAND, name);

  const USER_CONTENT =
    "[data-user-content], [data-instance-powered-by], [class*='readme'], [class*='markdown'], [class*='commit'], " +
    "[class*='message'], [class*='description'], [data-markdown], [data-repo-detail], " +
    "[data-repo-readme-body], [data-repo-about-description]";
  const CONTENT_PAGES = /^(repo|profile)/;
  const CONTENT_PATHS = /^\/(chat|notes|docs)(\/|$)/;
  const CHROME = "header, nav, aside, footer, dialog, [role='dialog'], [role='banner'], [role='navigation']";

  const contentPage = () =>
    CONTENT_PAGES.test((document.body && document.body.dataset.page) || "") || CONTENT_PATHS.test(location.pathname);

  const leaveAlone = (node) => {
    const element = node.nodeType === 1 ? node : node.parentElement;
    if (!element) return false;
    if (element.closest(USER_CONTENT)) return true;
    return contentPage() && Boolean(element.closest("main")) && !element.closest(CHROME);
  };

  const brandText = (node) => {
    if (!node.nodeValue || node.nodeValue.indexOf("ForkMesh") === -1) return;
    const parent = node.parentElement;
    if (!parent || SKIP.has(parent.tagName) || parent.isContentEditable || leaveAlone(node)) return;
    const next = swap(node.nodeValue);
    if (next !== node.nodeValue) node.nodeValue = next;
  };

  // Every element of the page passes through here, so the cheap attribute
  // test comes first: leaveAlone walks the ancestors with a long selector and
  // is only asked about the rare element that names ForkMesh.
  const brandElement = (element) => {
    let checked = false;
    for (const attribute of ATTRIBUTES) {
      const value = element.getAttribute(attribute);
      if (!value || value.indexOf("ForkMesh") === -1) continue;
      if (!checked) {
        if (leaveAlone(element)) return;
        checked = true;
      }
      element.setAttribute(attribute, swap(value));
    }
  };

  const hide = (link) => {
    const item = link.closest("li") || link;
    item.hidden = true;
    item.setAttribute("data-instance-plugin-off", "");
  };

  const isHidden = (path) =>
    hiddenPaths.some((prefix) => path === prefix || path.startsWith(prefix.replace(/\/$/, "") + "/"));

  const legalPage = (url) => (url.origin === location.origin ? LEGAL_PATHS[url.pathname.replace(/(.)\/$/, "$1")] || "" : "");

  const legalEntry = (page) => {
    const entry = legal && legal[page];
    return entry && entry.enabled && typeof entry.href === "string" && entry.href ? entry : null;
  };

  const showLegal = (link, page) => {
    const entry = legalEntry(page);
    if (!entry) {
      hide(link);
      return;
    }
    link.setAttribute("href", entry.href);
    link.setAttribute("data-instance-legal", page);
    if (entry.external) {
      link.rel = "noopener";
      link.target = "_blank";
    }
  };

  const legalLink = (page, label) => {
    const entry = legalEntry(page);
    const link = document.createElement("a");
    link.href = entry.href;
    link.textContent = label;
    link.setAttribute("data-instance-legal", page);
    if (entry.external) {
      link.rel = "noopener";
      link.target = "_blank";
    }
    return link;
  };

  const legalConsent = () => {
    const label = document.querySelector("[data-instance-legal-consent]:not([data-instance-legal])");
    if (!label || !legal) return;
    const terms = Boolean(legalEntry("terms"));
    const privacy = Boolean(legalEntry("privacy"));
    const text = label.querySelector("[data-instance-legal-consent-text]");
    if (!terms && !privacy) {
      label.hidden = true;
    } else if (text) {
      const parts = terms
        ? ["I agree to the ", legalLink("terms", "Terms")].concat(
            privacy ? [" and have read the ", legalLink("privacy", "Privacy"), " policy."] : ["."])
        : ["I have read the ", legalLink("privacy", "Privacy"), " policy."];
      text.replaceChildren(...parts);
    }
    label.setAttribute("data-instance-legal", terms || privacy ? "on" : "off");
    const box = label.querySelector("input[type='checkbox']");
    if (box) box.dispatchEvent(new Event("change", { bubbles: true }));
  };

  const legalGroups = (root) => {
    if (!legal || !root.querySelectorAll) return;
    for (const group of root.querySelectorAll("[data-instance-legal-group]:not([data-instance-legal])")) {
      const on = group.querySelector("a[data-instance-legal]");
      if (!on) group.hidden = true;
      group.setAttribute("data-instance-legal", on ? "on" : "off");
    }
  };

  const hideLinks = (root) => {
    if (!ready || !root.querySelectorAll) return;
    for (const link of root.querySelectorAll("a[href]")) {
      if (link.hasAttribute("data-instance-legal")) continue;
      let url;
      try {
        url = new URL(link.getAttribute("href"), location.href);
      } catch {
        continue;
      }
      const page = legal ? legalPage(url) : "";
      const offsite = !page && offsiteHosts.includes(url.hostname);
      const local = !page && !offsite && url.origin === location.origin && isHidden(url.pathname);
      // Most links need nothing; the ancestor checks run only for one that does.
      if (!page && !offsite && !local) continue;
      if (leaveAlone(link) || link.closest("[data-instance-legal-consent]")) continue;
      if (page) {
        showLegal(link, page);
        continue;
      }
      if (offsite) {
        if (url.hostname === "world.forkmesh.com") {
          if (isHidden("/world")) hide(link);
          else link.setAttribute("href", "/world");
        } else if (url.hostname === "forkmesh.com" && url.pathname === "/" && !url.hash) {
          link.setAttribute("href", "/");
        } else {
          hide(link);
        }
        continue;
      }
      hide(link);
    }
    legalGroups(root);
    legalConsent();
  };

  const nameBesideLogo = () => {
    if (document.querySelector("[data-instance-name-label]")) return;
    const brand = document.querySelector(
      "header a.forkmesh-simple-brand, header a.brand, header a[href='/'][aria-label]");
    if (!brand) return;
    const label = document.createElement("span");
    label.setAttribute("data-instance-name-label", "");
    label.textContent = name;
    Object.assign(label.style, { marginLeft: "8px", fontWeight: "700", whiteSpace: "nowrap" });
    brand.style.display = brand.style.display || "inline-flex";
    brand.style.alignItems = "center";
    brand.append(label);
  };

  const PLUGIN_ID = /^[a-z][a-z0-9-]{0,40}$/;
  const PLUGIN_HIDDEN = {
    genie: ".fm-genie, .fm-genie-bubble, .fm-genie-fx, genie-spider",
  };

  const pluginStyle = () => {
    let style = document.querySelector("style[data-instance-plugins]");
    if (!style) {
      style = document.createElement("style");
      style.setAttribute("data-instance-plugins", "");
      (document.head || document.documentElement).append(style);
    }
    return style;
  };

  const hidePlugins = (off) => {
    pluginStyle().textContent = off
      ? off
          .filter((id) => PLUGIN_ID.test(id))
          .map((id) => `[data-instance-plugin~="${id}"]${PLUGIN_HIDDEN[id] ? `, ${PLUGIN_HIDDEN[id]}` : ""} { display: none !important; }`)
          .join("\n")
      : "[data-instance-plugin] { display: none !important; }";
  };

  const walk = (root) => {
    if (!root) return;
    if (root.nodeType === 3) {
      brandText(root);
      return;
    }
    if (root.nodeType !== 1 && root.nodeType !== 9) return;
    if (root.nodeType === 1) {
      if (SKIP.has(root.tagName)) return;
      brandElement(root);
    }
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT);
    let node = walker.nextNode();
    while (node) {
      if (node.nodeType === 3) brandText(node);
      else brandElement(node);
      node = walker.nextNode();
    }
    hideLinks(root.nodeType === 9 ? root.documentElement : root);
  };

  const poweredBy = () => {
    if (script && script.dataset.instancePoweredBy === "off") return;
    if (document.querySelector("[data-instance-powered-by]")) return;
    const footer = document.createElement("footer");
    footer.setAttribute("data-instance-powered-by", "");
    footer.setAttribute("role", "contentinfo");
    Object.assign(footer.style, {
      position: "fixed",
      left: "10px",
      bottom: "8px",
      zIndex: "2147483000",
      padding: "3px 9px",
      borderRadius: "999px",
      font: "500 11px/1.4 system-ui, -apple-system, Segoe UI, sans-serif",
      color: "rgba(127, 127, 127, 0.95)",
      background: "rgba(127, 127, 127, 0.12)",
      backdropFilter: "blur(6px)",
      pointerEvents: "none",
    });
    footer.append("Powered by ");
    const link = document.createElement("a");
    link.href = "https://forkmesh.com/";
    link.rel = "noopener";
    link.target = "_blank";
    link.textContent = "ForkMesh";
    Object.assign(link.style, { color: "inherit", fontWeight: "700", pointerEvents: "auto" });
    footer.append(link);
    document.body.append(footer);
  };

  const legalStyle = () => {
    if (document.querySelector("style[data-instance-legal-pending]")) return;
    const style = document.createElement("style");
    style.setAttribute("data-instance-legal-pending", "");
    style.textContent = LEGAL_PENDING;
    (document.head || document.documentElement).append(style);
  };

  const pluginsStyled = () => {
    try {
      return getComputedStyle(document.documentElement).getPropertyValue("--instance-plugins").trim() === "ready";
    } catch {
      return false;
    }
  };

  const start = () => {
    if (!pluginsStyled()) hidePlugins(null);
    legalStyle();
    if (document.title.indexOf("ForkMesh") !== -1) document.title = swap(document.title);
    walk(document);
    poweredBy();
    nameBesideLogo();
    new MutationObserver((records) => {
      for (const record of records) {
        if (record.type === "characterData") brandText(record.target);
        else if (record.type === "attributes") brandElement(record.target);
        else for (const node of record.addedNodes) walk(node);
      }
      nameBesideLogo();
    }).observe(document.documentElement, {
      subtree: true,
      childList: true,
      characterData: true,
      attributes: true,
      attributeFilter: ATTRIBUTES,
    });
    // One /api/instance request per page, shared with the dashboard
    // (02-helpers.js sharedInstanceDocument) and genie-spider.js.
    let shared = window.__forkmeshInstanceInfo;
    if (!shared || typeof shared.then !== "function") {
      shared = fetch("/api/instance", { credentials: "omit", headers: { accept: "application/json" } })
        .then((response) => (response.ok ? response.json() : null))
        .catch(() => null);
      window.__forkmeshInstanceInfo = shared;
    }
    shared
      .then((data) => {
        legal = (data && data.legal && typeof data.legal === "object") ? data.legal : {};
        if (!data || !Array.isArray(data.plugins)) {
          ready = true;
          hideLinks(document.documentElement);
          return;
        }
        const off = data.plugins.filter((plugin) => !plugin.enabled);
        hiddenPaths = off.flatMap((plugin) => plugin.paths || []);
        offsiteHosts = Array.isArray(data.offsiteHosts) ? data.offsiteHosts : [];
        ready = true;
        hideLinks(document.documentElement);
        hidePlugins(off.map((plugin) => String(plugin.id || "")));
      })
      .catch(() => {
        legal = {};
        ready = true;
        hideLinks(document.documentElement);
      });
  };

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, { once: true });
  else start();
})();

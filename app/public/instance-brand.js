(() => {
  const script = document.currentScript;
  const name = (script && script.dataset.instanceName || "").trim();
  if (!name || name === "ForkMesh") return;
  const BRAND = /ForkMesh(?! [Dd]esktop)/g;
  const SKIP = new Set(["SCRIPT", "STYLE", "TEXTAREA", "CODE", "PRE", "KBD", "SAMP", "NOSCRIPT"]);
  const ATTRIBUTES = ["title", "aria-label", "placeholder", "alt"];
  let hiddenPaths = [];
  let offsiteHosts = [];
  let ready = false;

  const swap = (value) => value.replace(BRAND, name);

  const USER_CONTENT =
    "[data-instance-powered-by], [class*='readme'], [class*='markdown'], [class*='commit'], " +
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

  const brandElement = (element) => {
    if (leaveAlone(element)) return;
    for (const attribute of ATTRIBUTES) {
      const value = element.getAttribute(attribute);
      if (value && value.indexOf("ForkMesh") !== -1) element.setAttribute(attribute, swap(value));
    }
  };

  const hide = (link) => {
    const item = link.closest("li") || link;
    item.hidden = true;
    item.setAttribute("data-instance-plugin-off", "");
  };

  const isHidden = (path) =>
    hiddenPaths.some((prefix) => path === prefix || path.startsWith(prefix.replace(/\/$/, "") + "/"));

  const hideLinks = (root) => {
    if (!ready || !root.querySelectorAll) return;
    for (const link of root.querySelectorAll("a[href]")) {
      if (leaveAlone(link)) continue;
      let url;
      try {
        url = new URL(link.getAttribute("href"), location.href);
      } catch {
        continue;
      }
      if (offsiteHosts.includes(url.hostname)) {
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
      if (url.origin === location.origin && isHidden(url.pathname)) hide(link);
    }
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

  const start = () => {
    hidePlugins(null);
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
    fetch("/api/instance", { credentials: "same-origin" })
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => {
        if (!data || !Array.isArray(data.plugins)) return;
        const off = data.plugins.filter((plugin) => !plugin.enabled);
        hiddenPaths = off.flatMap((plugin) => plugin.paths || []);
        offsiteHosts = Array.isArray(data.offsiteHosts) ? data.offsiteHosts : [];
        ready = true;
        hideLinks(document.documentElement);
        hidePlugins(off.map((plugin) => String(plugin.id || "")));
      })
      .catch(() => {});
  };

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, { once: true });
  else start();
})();

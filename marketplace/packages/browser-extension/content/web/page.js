// Browser Extension page: install, update, and explain the ForkMesh extension
// for Chrome. The page talks to nothing; it reads the version the extension's
// bridge (content/extension/dashboard.js) stamps on the document.
window.ForkMeshDashboard.registerPage("browser-extension", ({ select: $ }) => {
  // The extension this package version ships. The deploy stage zips
  // content/extension to this path (app/tools/build_app_plugins.py).
  const EXTENSION_VERSION = "2.20.0";
  const EXTENSION_ZIP = `/dashboard-plugins/browser-extension/browser-extension-${EXTENSION_VERSION}.zip`;

  function installedVersion() {
    const data = document.documentElement.dataset;
    return String(data.forkmeshExtension || data.forkmeshCrmExtension || "");
  }
  function versionBehind(installed, current) {
    const have = installed.split(".").map(Number); const want = current.split(".").map(Number);
    for (let index = 0; index < Math.max(have.length, want.length); index += 1) {
      if ((have[index] || 0) !== (want[index] || 0)) return (have[index] || 0) < (want[index] || 0);
    }
    return false;
  }
  function notice(text = "") {
    const node = $(`[data-extension-notice]`); if (!node) return;
    const slot = node.querySelector("[data-extension-notice-text]") || node;
    slot.textContent = text; node.classList.toggle("hidden", !text);
  }
  function render() {
    const installed = installedVersion();
    const link = $(`[data-extension-download]`); if (!link) return;
    link.href = EXTENSION_ZIP;
    const state = $(`[data-extension-state]`), detail = $(`[data-extension-state-detail]`), label = $(`[data-extension-download-label]`);
    if (!installed) {
      state.textContent = "The extension is not installed in this browser.";
      detail.textContent = "Chrome, Edge, Brave and other Chromium browsers, version 137 or newer.";
      label.textContent = `Download v${EXTENSION_VERSION}`;
    } else if (versionBehind(installed, EXTENSION_VERSION)) {
      state.textContent = `An update is ready: v${installed} is installed, v${EXTENSION_VERSION} is current.`;
      detail.textContent = installed.startsWith("1.") ? "v1 was the CRM-only extension. v2 adds the Password Vault." : "";
      label.textContent = `Download v${EXTENSION_VERSION}`;
    } else {
      state.textContent = `Extension v${installed} is installed and up to date.`;
      detail.textContent = "";
      label.textContent = `Download v${EXTENSION_VERSION} again`;
    }
    window.lucide?.createIcons?.();
  }
  async function openExtensions() {
    if (installedVersion()) {
      window.postMessage({ source: "forkmesh-extension-page", type: "open-extensions" }, location.origin);
      return;
    }
    // Chrome will not open chrome:// addresses from a website. Without the
    // extension to open it, hand the address over for the address bar.
    const copied = await navigator.clipboard?.writeText("chrome://extensions").then(() => true, () => false);
    notice(copied
      ? "Copied chrome://extensions. Paste it into the address bar; Chrome does not let websites open it."
      : "Type chrome://extensions into the address bar. Chrome does not let websites open it.");
  }
  function init() {
    render();
    // The bridge runs at document_start, but announce itself again if it
    // loads after this script (an extension reloaded while the page is open).
    window.addEventListener("forkmesh-extension", render);
    $(`[data-extension-open-extensions]`)?.addEventListener("click", () => void openExtensions());
    $(`[data-extension-download]`)?.addEventListener("click", () => notice(installedVersion()
      ? "Unzip it over the browser-extension folder you loaded before, then press Reload on the ForkMesh card in chrome://extensions."
      : "Unzip it, open chrome://extensions, turn on Developer mode, choose Load unpacked, and pick the unzipped browser-extension folder, the one with manifest.json directly inside."));
  }
  return init();
});

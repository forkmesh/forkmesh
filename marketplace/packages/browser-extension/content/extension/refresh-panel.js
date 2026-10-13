const $ = (selector) => document.querySelector(selector);
let windowId = null;
let tab = null;
let plan = null;
let busy = false;
let status = null;
let loading = 0;

function send(message) {
  return new Promise((resolve) => {
    chrome.runtime.sendMessage(message, (response) => {
      if (chrome.runtime.lastError) resolve({ ok: false, error: "extension_unavailable", message: chrome.runtime.lastError.message });
      else resolve(response || { ok: false, error: "no_response", message: "No response" });
    });
  });
}

function draw() {
  ForkMeshRefreshView.render($("[data-refresh]"), plan, { busy, status, go, run: () => work({ type: "fm-refresh-run" }),
    update: (next) => work({ type: "fm-refresh-channel", channel: next.channel }) });
  $("[data-refresh-empty]").hidden = Boolean(plan?.show);
}

async function load() {
  const ticket = ++loading;
  [tab] = await chrome.tabs.query({ active: true, windowId });
  const result = tab?.url ? await send({ type: "fm-refresh-plan", url: tab.url }) : null;
  if (ticket !== loading) return;
  plan = result?.ok ? result : null;
  draw();
}

function go(url) {
  if (!tab?.id) return;
  status = null;
  void chrome.tabs.update(tab.id, { url });
}

async function work(message) {
  if (busy || !tab?.id) return;
  busy = true;
  status = { text: "Working…", error: false, busy: true };
  draw();
  const result = await send({ ...message, url: tab.url, tabId: tab.id });
  busy = false;
  loading += 1;
  if (result.plan) plan = result.plan;
  status = result.ok ? { text: result.message, error: Boolean(result.failed) } : { text: result.message || "Request failed", error: true };
  draw();
}

chrome.tabs.onActivated.addListener((info) => {
  if (info.windowId !== windowId) return;
  status = null;
  void load();
});
chrome.tabs.onUpdated.addListener((tabId, change) => {
  if (tabId !== tab?.id || (!change.url && change.status !== "complete")) return;
  if (change.url) status = null;
  void load();
});
chrome.runtime.onMessage.addListener((request) => {
  if (request?.type === "fm-refresh-changed" && !busy) void load();
  return false;
});

chrome.windows.getCurrent().then((current) => {
  windowId = current.id;
  void load();
});

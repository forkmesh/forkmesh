const $ = (selector) => document.querySelector(selector);
const jobId = new URLSearchParams(location.search).get("job") || "";
let origin = "";

const send = (message) => new Promise((resolve) => {
  chrome.runtime.sendMessage(message, (response) => {
    resolve(chrome.runtime.lastError ? { ok: false, message: chrome.runtime.lastError.message } : response || { ok: false });
  });
});

function fail(text) {
  $("[data-capture-error]").textContent = text;
  $("[data-capture-error]").hidden = false;
}

async function load() {
  const result = await send({ type: "fm-marketing-capture-job", job: jobId });
  if (!result.ok) {
    $("[data-capture-text]").textContent = "This update has expired. Start it again from Marketing.";
    return;
  }
  origin = result.origin;
  const host = new URL(result.job.url).host;
  $("[data-capture-title]").textContent = `Update ${result.job.name}`;
  $("[data-capture-text]").textContent = `Chrome needs your permission once for the extension to read ${host}. After that, this channel updates with one click.`;
  $("[data-capture-allow]").disabled = false;
}

$("[data-capture-allow]").addEventListener("click", () => {
  $("[data-capture-allow]").disabled = true;
  chrome.permissions.request({ origins: [origin] }, async (granted) => {
    const result = await send({ type: "fm-marketing-capture-continue", job: jobId, granted: Boolean(granted) });
    if (!result.ok || result.job?.state === "error") {
      $("[data-capture-allow]").disabled = false;
      fail(granted ? result.message || "Could not continue." : "Chrome did not grant access to that site.");
    }
  });
});

$("[data-capture-cancel]").addEventListener("click", async () => {
  await send({ type: "fm-marketing-capture-continue", job: jobId, granted: false });
  window.close();
});

void load();

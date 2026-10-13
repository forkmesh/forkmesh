/* ForkMesh World Discord bridge loader.
 *
 * The bridge is click-initiated UI that most visits never open, so its
 * implementation lives in world-discord-panel.js and stays outside the initial
 * World module graph that world/tools/build_worker_footprint.py budgets. This loader
 * owns the same two entry points the panel used to bind directly — the
 * settings trigger and the forkmesh:open-discord event — and hands the first
 * one that fires to the real module.
 */

const OPEN_EVENT = "forkmesh:open-discord";
const TRIGGER_SELECTOR = "[data-world-discord-open]";

let panelPromise = null;
let panelLoadFailures = 0;
let boundTrigger = null;
let triggerObserver = null;

// The panel binds this same trigger and event itself once it boots, so the
// loader drops its own stand-ins first: leaving both attached would toggle the
// panel twice per click and close it again immediately.
function releaseLoaderBindings() {
  window.removeEventListener(OPEN_EVENT, handleOpenEvent);
  boundTrigger?.removeEventListener("click", handleTriggerClick);
  boundTrigger = null;
  triggerObserver?.disconnect();
  triggerObserver = null;
}

function loadPanel() {
  panelPromise ||= (panelLoadFailures
    ? import(`./world-discord-panel.js?retry=${panelLoadFailures}`)
    : import("./world-discord-panel.js")
  ).then((module) => {
    releaseLoaderBindings();
    module.bootWorldDiscord();
    return module;
  }, (error) => {
    panelPromise = null;
    panelLoadFailures += 1;
    throw error;
  });
  return panelPromise;
}

async function openPanel() {
  let module;
  try {
    module = await loadPanel();
  } catch (error) {
    console.warn("ForkMesh World could not load the Discord panel", error);
    return;
  }
  await module.showWorldDiscordPanel();
}

function preloadPanel() {
  loadPanel().catch((error) => {
    console.warn("ForkMesh World could not load the Discord panel", error);
    bindEntryPoints();
  });
}

function handleOpenEvent() {
  void openPanel();
}

function handleTriggerClick() {
  void openPanel();
}

function bindTrigger() {
  const trigger = document.querySelector(TRIGGER_SELECTOR);
  if (!trigger || boundTrigger) return false;
  boundTrigger = trigger;
  trigger.setAttribute("aria-expanded", "false");
  trigger.addEventListener("click", handleTriggerClick);
  return true;
}

function boot() {
  // Returning from Discord authorization: the outcome notice belongs to this
  // navigation and the panel opens itself, so the real module is needed now.
  if (new URL(window.location.href).searchParams.has("discord")) {
    preloadPanel();
    return;
  }
  bindEntryPoints();
}

function bindEntryPoints() {
  window.addEventListener(OPEN_EVENT, handleOpenEvent);
  if (bindTrigger()) return;
  // The settings panel that owns the trigger is built by the World element
  // after its scene boots, so wait for it exactly as the panel used to.
  triggerObserver = new MutationObserver(() => {
    if (!bindTrigger()) return;
    triggerObserver?.disconnect();
    triggerObserver = null;
  });
  triggerObserver.observe(document.body, { childList: true, subtree: true });
}

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot, { once: true });
else boot();

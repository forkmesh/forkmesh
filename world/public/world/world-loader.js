/* Production entry shell for ForkMesh World.
 *
 * The authored world.js remains the directly testable runtime source. During
 * staging it is deployed as world-runtime.js and this file is deployed at the
 * public /world/world.js URL. The shell paints before crossing the dynamic
 * import boundary, so a first visit does not have to parse the full World
 * implementation before it can show useful progress.
 */

const host = document.querySelector("forkmesh-world");
const bootModule = import("./world-boot.js").then(
  (module) => {
    module.startWorldBootPrefetch(window);
    return module.installWorldBoot(window);
  },
  () => null,
);
const bootCurtain = document.querySelector("[data-world-loading]");
if (bootCurtain) {
  const stage = bootCurtain.querySelector("[data-world-loading-stage]");
  const bar = bootCurtain.querySelector("[data-world-loading-bar]");
  const progress = bootCurtain.querySelector("[data-world-loading-progress]");
  const percent = bootCurtain.querySelector("[data-world-loading-percent]");
  if (stage) stage.textContent = "Downloading the World runtime…";
  if (bar) {
    bar.setAttribute("aria-valuenow", "6");
    bar.setAttribute("aria-valuetext", "Downloading the World runtime…");
  }
  if (progress) progress.style.width = "6%";
  if (percent) percent.textContent = "6%";
}

// Yield through an animation frame before requesting the large runtime. The
// loading shell can therefore paint from this tiny graph even on a cold cache.
await new Promise((resolve) => window.requestAnimationFrame(resolve));

for (const href of [
  "/world/vendor/three-0.184.0/three.module.min.js",
  "/world/vendor/three-0.184.0/three.core.min.js",
  "/world/world-scene.js",
  "/world/scene/core.js",
  "/world/scene/agent-bots.js",
  "/world/scene/aquarium-panel.js",
  "/world/scene/aquarium-schooling.js",
  "/world/scene/arrival.js",
  "/world/scene/avatar-badges.js",
  "/world/scene/avatar-controls.js",
  "/world/scene/avatar-instancing.js",
  "/world/scene/avatar-rig.js",
  "/world/scene/avatar-outfits.js",
  "/world/scene/avatar-shared.js",
  "/world/scene/avatars.js",
  "/world/scene/batching.js",
  "/world/scene/boards-tasks.js",
  "/world/scene/boards.js",
  "/world/scene/canvas.js",
  "/world/scene/desert-plots.js",
  "/world/scene/environment.js",
  "/world/scene/figure-batches.js",
  "/world/scene/gameplay-constants.js",
  "/world/scene/geometry-dedupe.js",
  "/world/scene/gpu-pacer.js",
  "/world/scene/instance-hexes.js",
  "/world/scene/instance-territory.js",
  "/world/scene/labels.js",
  "/world/scene/landmarks.js",
  "/world/scene/layout.js",
  "/world/scene/leaderboards.js",
  "/world/scene/materials.js",
  "/world/scene/member-engagement.js",
  "/world/scene/members-circle.js",
  "/world/scene/mirror-nodes.js",
  "/world/scene/movement.js",
  "/world/scene/neighbour-footprints.js",
  "/world/scene/object-utils.js",
  "/world/scene/office-aquarium.js",
  "/world/scene/office-building.js",
  "/world/scene/office-infrastructure.js",
  "/world/scene/office-marketing.js",
  "/world/scene/quality.js",
  "/world/scene/repositories.js",
  "/world/scene/render-stalls.js",
  "/world/scene/resources.js",
  "/world/scene/scene-build.js",
  "/world/scene/settled-matrices.js",
  "/world/scene/shader-warmup.js",
  "/world/scene/sol-fountain.js",
  "/world/scene/territory-bugs.js",
  "/world/scene/territory-geometry.js",
  "/world/scene/town.js",
  "/world/scene/treasury.js",
  "/world/scene/weather.js",
  "/world/scene/drone-race.js",
  "/world/scene/coaster.js",
  "/world/scene/districts/agent-bots.js",
  "/world/scene/districts/arrival.js",
  "/world/scene/districts/beach.js",
  "/world/scene/districts/bikes.js",
  "/world/scene/districts/billboard-circles.js",
  "/world/scene/districts/coaster.js",
  "/world/scene/districts/compact-districts.js",
  "/world/scene/districts/leaderboard-district.js",
  "/world/scene/districts/leaderboards.js",
  "/world/scene/districts/members-circle.js",
  "/world/scene/districts/office-boards.js",
  "/world/scene/districts/office-displays.js",
  "/world/scene/districts/office-floors.js",
  "/world/scene/districts/office-lobby.js",
  "/world/scene/districts/office-rooftop.js",
  "/world/scene/districts/quadcopters.js",
  "/world/scene/districts/recreation.js",
  "/world/scene/districts/roads.js",
  "/world/scene/districts/social-packages.js",
  "/world/scene/districts/town-boards.js",
  "/world/scene/districts/town-terrain.js",
  "/world/scene/runtime/animate.js",
  "/world/scene/runtime/avatar-selection.js",
  "/world/scene/runtime/avatar-voice.js",
  "/world/scene/runtime/bots.js",
  "/world/scene/runtime/camera-controls.js",
  "/world/scene/runtime/camera.js",
  "/world/scene/runtime/coaster.js",
  "/world/scene/runtime/diagnostics.js",
  "/world/scene/runtime/drone-race.js",
  "/world/scene/runtime/effects.js",
  "/world/scene/runtime/network.js",
  "/world/scene/runtime/office-exits.js",
  "/world/scene/runtime/office-navigation.js",
  "/world/scene/runtime/office-presence.js",
  "/world/scene/runtime/office-walking.js",
  "/world/scene/runtime/pointer.js",
  "/world/scene/runtime/quality-environment.js",
  "/world/scene/runtime/remote-players.js",
  "/world/scene/runtime/render-setup.js",
  "/world/scene/runtime/repository-catalog.js",
  "/world/scene/runtime/repository-records.js",
  "/world/scene/runtime/rides.js",
  "/world/scene/runtime/travel.js",
  "/world/scene/runtime/walk-player.js",
  "/world/scene/runtime/world-elements.js",
]) {
  const link = document.createElement("link");
  link.rel = "modulepreload";
  link.href = href;
  document.head.append(link);
}

// A deploy between this shell and its runtime request leaves the old shell
// asking for a module the new build no longer serves ("Failed to fetch
// dynamically imported module"). Reload once so shell and runtime come from
// the same build; a second failure is a real outage and is reported.
const RUNTIME_RELOAD_KEY = "forkmesh-world-runtime-reload";
function runtimeReloadAttempted() {
  try { return sessionStorage.getItem(RUNTIME_RELOAD_KEY) === "1"; } catch { return true; }
}
function markRuntimeReload(done) {
  try {
    if (done) sessionStorage.removeItem(RUNTIME_RELOAD_KEY);
    else sessionStorage.setItem(RUNTIME_RELOAD_KEY, "1");
  } catch { /* storage unavailable: fall through to the failure notice */ }
}

setTimeout(() => {
  try {
    new Intl.DateTimeFormat(undefined, { hour: "numeric", minute: "2-digit" }).format(0);
  } catch {}
}, 0);

let runtime;
try {
  runtime = await import("./world-runtime.js");
  markRuntimeReload(true);
} catch (error) {
  const moduleFetchFailed =
    /dynamically imported module|Failed to fetch|Importing a module script failed/i
      .test(String(error?.message || error));
  if (moduleFetchFailed && !runtimeReloadAttempted()) {
    markRuntimeReload(false);
    window.location.reload();
    await new Promise(() => {});
  }
  bootModule.then((boot) => boot?.fail(error?.message || error));
  document.querySelector("[data-world-boot]")?.remove();
  host?.removeAttribute("data-world-booting");
  host?.removeAttribute("inert");
  const failure = document.querySelector("[data-world-load-error]");
  if (failure) failure.hidden = false;
  throw error;
}

// Preserve the intentionally public pure helper exposed by world.js.
export const normalizeMemberDirectory = runtime.normalizeMemberDirectory;

export const SCENE_BUILD_SLICE_MS = 12;
export const FIRST_VIEW_HOLD_MS = 4_000;
export const SCENE_BUILD_FRAME_MS = 32;
export const FIRST_VIEW_WARM_SLICE_MS = 16;
export const FIRST_VIEW_WARM_PASSES = 12;
export const CANVAS_RESIZE_COMPILE_WAIT_MS = 1_500;
export const SCENE_BUILD_FRAME_WAIT_MS = 50;

export function yieldSceneBuild() {
  const scheduler = globalThis.scheduler;
  if (typeof scheduler?.yield === "function") return scheduler.yield();
  return new Promise((resolve) => {
    if (typeof MessageChannel !== "function") {
      setTimeout(resolve, 0);
      return;
    }
    const channel = new MessageChannel();
    channel.port1.onmessage = () => {
      channel.port1.close();
      resolve();
    };
    channel.port2.postMessage(0);
  });
}

export function yieldSceneFrame() {
  if (typeof requestAnimationFrame !== "function") return yieldSceneBuild();
  return new Promise((resolve) => {
    let settled = false;
    const resume = () => {
      if (settled) return;
      settled = true;
      clearTimeout(fallback);
      resolve();
    };
    const fallback = setTimeout(resume, SCENE_BUILD_FRAME_WAIT_MS);
    requestAnimationFrame(() => setTimeout(resume, 0));
  });
}

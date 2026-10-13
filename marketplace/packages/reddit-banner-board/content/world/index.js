// Reddit banner board package.
//
// Network access stays with the World host. This module owns only the board's
// portable identity, appearance, placement and scene-object lifecycle.

export const manifest = Object.freeze({
  id: "reddit-banner-board",
  version: "1.0.0",
  label: "Reddit Banner Board",
  category: "3D",
  summary: "The public r/forkmesh community feed board.",
  feedId: "reddit",
  placement: Object.freeze({
    circle: "social",
    feed: "social",
    order: 120,
  }),
});

export const bannerOptions = Object.freeze({
  id: "reddit",
  position: Object.freeze([27.6, 0, -26.6]),
  accent: "#ff4500",
  frameColor: "#7a2d0e",
  titleColor: "#fff4ee",
  divider: "rgba(255,69,0,0.5)",
  title: "REDDIT",
  handle: "r/forkmesh",
  host: "reddit.com/r/forkmesh",
  lines: Object.freeze([
    "COMMUNITY SUBREDDIT",
    "QUESTIONS · SHOWCASES · HELP",
    "MODERATED BY THE CORE TEAM",
  ]),
  footer: "OPENS REDDIT.COM IN A NEW TAB",
  url: "https://www.reddit.com/r/forkmesh/",
  staleness: Object.freeze({
    label: "LAST POST",
    freshMs: 24 * 60 * 60 * 1000,
    staleMs: 72 * 60 * 60 * 1000,
  }),
});

export function install(host) {
  if (typeof host?.createSocialBanner !== "function") {
    throw new Error("reddit-banner-board requires the social-banner host");
  }
  const group = host.createSocialBanner(bannerOptions);
  let disposed = false;
  return {
    group,
    options: bannerOptions,
    feedId: manifest.feedId,
    placement: manifest.placement,
    dispose() {
      if (disposed) return;
      disposed = true;
      host.disposeObject3D?.(group);
    },
  };
}

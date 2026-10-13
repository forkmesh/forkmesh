// X banner board package.
//
// The package receives one bounded host capability: build a board from these
// declarative options. It performs no fetches and imports no World internals;
// the host supplies the already-normalized public feed snapshot.

export const manifest = Object.freeze({
  id: "x-banner-board",
  version: "1.0.0",
  label: "X Banner Board",
  category: "3D",
  summary: "The public @forkmesh X feed board.",
  feedId: "twitter",
  placement: Object.freeze({
    circle: "social",
    feed: "social",
    order: 110,
  }),
});

export const bannerOptions = Object.freeze({
  id: "twitter",
  position: Object.freeze([37.2, 0, -9.2]),
  accent: "#1d9bf2",
  frameColor: "#1b4e73",
  titleColor: "#f2faff",
  divider: "rgba(29,155,242,0.5)",
  title: "TWITTER / X",
  handle: "@forkmesh",
  host: "x.com/forkmesh",
  lines: Object.freeze([
    "OFFICIAL FORKMESH ACCOUNT",
    "RELEASES · OUTAGES · NEWS",
    "REPLIES WELCOME OVER THERE",
  ]),
  footer: "OPENS X.COM IN A NEW TAB",
  url: "https://x.com/forkmesh",
  staleness: Object.freeze({
    label: "LAST POST",
    freshMs: 24 * 60 * 60 * 1000,
    staleMs: 72 * 60 * 60 * 1000,
  }),
});

export function install(host) {
  if (typeof host?.createSocialBanner !== "function") {
    throw new Error("x-banner-board requires the social-banner host");
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

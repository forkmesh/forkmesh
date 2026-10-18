export const PORTAL_PARAM = "portal";
export const HOME_PORTAL_ID = "forkmesh-world-home";
const SLOTS = 12;

function validSlot(value) {
  const slot = Number(value);
  return Number.isInteger(slot) && slot >= 0 && slot < SLOTS ? slot : -1;
}

export function worldPortalURL(value, host = "") {
  try {
    const url = new URL(String(value || ""));
    if (
      url.protocol !== "https:" ||
      url.pathname !== "/world/" ||
      url.search ||
      url.hash ||
      url.username ||
      url.password ||
      (host && url.hostname !== host)
    ) {
      return "";
    }
    return url.href;
  } catch (_) {
    return "";
  }
}

export function portalDestination(worldUrl, slot) {
  const base = worldPortalURL(worldUrl);
  if (!base) return "";
  const arrival = validSlot(slot);
  if (arrival < 0) return base;
  const url = new URL(base);
  url.searchParams.set(PORTAL_PARAM, String(arrival));
  return url.href;
}

export function requestedPortalSlot(search = "") {
  const value = new URLSearchParams(String(search || "")).get(PORTAL_PARAM);
  return value !== null && /^\d{1,2}$/.test(value) ? validSlot(value) : -1;
}

export function homePortalHex(payload) {
  if (payload?.branded !== true) return null;
  const portal = payload?.directory?.portal;
  const home = worldPortalURL(portal?.home);
  if (!home) return null;
  const arrival = validSlot(portal?.slot);
  return {
    id: HOME_PORTAL_ID,
    name: "ForkMesh World",
    url: home,
    host: new URL(home).hostname,
    logo: "",
    slot: arrival < 0 ? 0 : arrival,
    worldUrl: home,
    arrivalSlot: arrival,
    home: true,
  };
}

export function withHomePortal(hexes = [], home = null) {
  const list = Array.isArray(hexes) ? hexes : [];
  if (!home || list.some((hex) => hex.slot === home.slot)) return list;
  return [...list, home];
}

export function instancePluginOff(payload, id) {
  if (payload?.branded !== true || !Array.isArray(payload?.plugins)) return false;
  const plugin = payload.plugins.find((item) => item?.id === id);
  return Boolean(plugin) && plugin.enabled !== true;
}

export function instanceKeepsDirectory(payload) {
  return payload?.branded !== true;
}

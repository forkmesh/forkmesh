// Federated hexes: the World page talks to the owner of a neighbour's hexagons
// for what stands on them. Each owner has one context: its API origin and a
// visitor token minted there (POST /api/world/visa/session) from a one-minute
// visa issued by this World. Tokens are fetched lazily, the first time
// something on that owner's ground needs one, cached until shortly before they
// expire, and sent as a bearer header with credentials omitted, so no cookie
// ever crosses origins. Only the owner's federated World endpoints are called.
export const FEDERATION_SESSION_PATH = "/api/world/visa/session";
export const FEDERATED_PATHS = Object.freeze([
  "/api/world/visa/session",
  "/api/world/mesh",
  "/api/world/neighbour-scene",
  "/api/world/organizations",
  "/api/world/events",
  "/api/world/office/floors",
  "/api/world/office/attendance",
  "/api/world/office/general/status",
  "/api/world/office/general/entry",
  "/api/repositories",
  "/api/leaderboards",
]);
const TOKEN_RE = /^fmwv1\.[A-Za-z0-9_-]{1,1950}\.[0-9a-f]{64}$/;
const RENEW_MARGIN_MS = 60 * 1000;
const MAX_BODY_CHARS = 512 * 1024;
const HOST_RE = /^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z][a-z0-9-]{0,61}[a-z0-9](?::\d{1,5})?$/;

export function federationOrigin(value) {
  try {
    const url = new URL(String(value || ""));
    if (url.protocol !== "https:" || url.username || url.password) return "";
    return HOST_RE.test(url.host) ? url.origin : "";
  } catch (_) {
    return "";
  }
}

export function federatedPath(path) {
  const raw = String(path || "");
  const bare = raw.split("?")[0].replace(/\/+$/, "");
  return FEDERATED_PATHS.includes(bare) && !/[\s#]/.test(raw) ? raw : "";
}

export class WorldFederation {
  constructor({
    requestVisa = async () => "",
    fetchImpl = (...args) => globalThis.fetch(...args),
    now = () => Date.now(),
    timeoutMs = 8000,
  } = {}) {
    this.requestVisa = requestVisa;
    this.fetchImpl = fetchImpl;
    this.now = now;
    this.timeoutMs = timeoutMs;
    this.contexts = new Map();
    this.cache = new Map();
  }

  context(world) {
    const api = federationOrigin(world?.api);
    const id = String(world?.id || "");
    if (!api || !id) return null;
    let context = this.contexts.get(id);
    if (!context || context.api !== api) {
      context = { id, api, world: federationOrigin(world?.world), token: "",
        expiresAt: 0, name: "", home: "", pending: null, unsupported: false };
      this.contexts.set(id, context);
    }
    return context;
  }

  // The visitor token for an owner, minted there from a fresh visa. "" when
  // the walker is not signed in, the owner predates federation (404) or the
  // owner refuses; callers then carry on as a guest.
  async token(world) {
    const context = this.context(world);
    if (!context || context.unsupported) return "";
    if (context.token && context.expiresAt - RENEW_MARGIN_MS > this.now()) {
      return context.token;
    }
    if (context.pending) return context.pending;
    context.pending = (async () => {
      try {
        const visa = String((await this.requestVisa(world)) || "");
        if (!visa) return "";
        context.visa = visa;
        context.visaAt = this.now();
        const response = await this.send(context, FEDERATION_SESSION_PATH, {
          method: "POST",
          body: JSON.stringify({ visa }),
          headers: { "content-type": "application/json" },
        });
        if (response.status === 404) {
          context.unsupported = true;
          return "";
        }
        const payload = response.ok ? await this.read(response) : null;
        const token = String(payload?.token || "");
        const expiresAt = Number(payload?.expiresAt);
        if (!TOKEN_RE.test(token) || !Number.isFinite(expiresAt)) return "";
        context.token = token;
        context.expiresAt = expiresAt;
        context.name = String(payload?.name || "").slice(0, 64);
        context.home = String(payload?.home || "").slice(0, 253);
        return token;
      } catch (_) {
        return "";
      } finally {
        context.pending = null;
      }
    })();
    return context.pending;
  }

  // What a socket to the owner should carry: the visitor token, or the visa
  // itself when the owner does not exchange visas yet.
  async credential(world) {
    const token = await this.token(world);
    if (token) return { visitor: token };
    const context = this.context(world);
    if (!context?.unsupported) return {};
    // A visa lives a minute; reuse the one just tried rather than ask again.
    const fresh = context.visa && this.now() - context.visaAt < 45 * 1000;
    const visa = fresh ? context.visa : String((await this.requestVisa(world)) || "");
    return visa ? { visa } : {};
  }

  // The identity this page holds at the owner, for that owner's UI.
  visitorSession(world) {
    const context = this.context(world);
    if (!context?.token || context.expiresAt <= this.now()) return null;
    return {
      nodeName: context.home ? `${context.name}@${context.home}` : context.name,
      visitor: true,
      owner: context.id,
    };
  }

  async send(context, path, { method = "GET", body, headers = {}, auth = true } = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    const request = { accept: "application/json", ...headers };
    if (auth && context.token && path !== FEDERATION_SESSION_PATH) {
      request.authorization = `Bearer ${context.token}`;
    }
    try {
      return await this.fetchImpl(context.api + path, {
        method,
        body,
        headers: request,
        mode: "cors",
        credentials: "omit",
        redirect: "error",
        cache: "no-store",
        signal: controller.signal,
      });
    } finally {
      clearTimeout(timer);
    }
  }

  async read(response) {
    const text = await response.text();
    if (text.length > MAX_BODY_CHARS) throw new Error("response_too_large");
    return JSON.parse(text);
  }

  // A World call routed to the owner of the ground it is for. Reads are
  // cached per owner and path for maxAge; nothing outside FEDERATED_PATHS is
  // ever sent across.
  async fetchJSON(world, path, { method = "GET", body, maxAge = 0, auth = true } = {}) {
    const context = this.context(world);
    const route = federatedPath(path);
    if (!context || !route) throw new Error("not_federated");
    const verb = String(method).toUpperCase();
    const key = `${context.id}:${route}`;
    const cached = this.cache.get(key);
    if (verb === "GET" && maxAge > 0 && cached && this.now() - cached.at <= maxAge) {
      return cached.value;
    }
    if (auth) await this.token(world);
    const response = await this.send(context, route, {
      method: verb,
      body: body === undefined ? undefined : JSON.stringify(body),
      headers: body === undefined ? {} : { "content-type": "application/json" },
      auth,
    });
    const payload = await this.read(response);
    if (!response.ok) {
      const error = new Error(String(payload?.error || `status_${response.status}`));
      error.status = response.status;
      throw error;
    }
    if (verb === "GET") this.cache.set(key, { at: this.now(), value: payload });
    return payload;
  }

  postJSON(world, path, body, options = {}) {
    return this.fetchJSON(world, path, { ...options, method: "POST", body: body || {} });
  }

  forget(world) {
    const id = String(world?.id || "");
    this.contexts.delete(id);
    [...this.cache.keys()].filter((key) => key.startsWith(`${id}:`))
      .forEach((key) => this.cache.delete(key));
  }
}

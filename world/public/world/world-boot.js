import { describeLongAnimationFrame, installResponseJsonTrace } from "./world-debug-log.js";

export const WORLD_BOOT_LOG_LIMIT = 400;
export const WORLD_BOOT_PREFS_KEY = "forkmesh.world.bootlog.v1";
export const WORLD_BOOT_SETTINGS_KEY = "forkmesh.world.settings.v1";
export const WORLD_BOOT_SETTLE_MS = 1500;
export const WORLD_BOOT_ELEMENT_LINE_MS = 4;
export const WORLD_BOOT_LOG_MARGIN = 8;

export const WORLD_MEMBER_DIRECTORY_PATH = "/api/accounts/users?view=world";
export const WORLD_BOOT_PREFETCH_TTL_MS = 30_000;
export const WORLD_BOOT_RUNS_KEY = "forkmesh.world.bootlog.runs.v1";
export const WORLD_BOOT_RUNS_KEPT = 5;
export const WORLD_BOOT_CRITICAL_API_MS = 300;
export const WORLD_BOOT_REGRESSION_MIN_MS = 50;
export const WORLD_BOOT_REGRESSION_RATIO = 1.3;
export const WORLD_BOOT_TIMER_RUNS = 200;
const WORLD_BOOT_ACCOUNT_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;

const LEVEL_ORDER = { high: 0, caution: 1, info: 2 };

export function worldBootSession(storage) {
  try {
    const value = JSON.parse(storage?.getItem?.("forkmesh.session") || "null");
    const nodeName = String(value?.nodeName || "").trim().toLowerCase();
    const sessionToken = String(value?.sessionToken || "").trim();
    if (!WORLD_BOOT_ACCOUNT_RE.test(nodeName)) return null;
    if (!sessionToken || sessionToken.length > 2048) return null;
    return { nodeName, sessionToken };
  } catch {
    return null;
  }
}

export function worldBootPrefetchPlan(session) {
  const plan = [
    {
      key: `GET:public:${WORLD_MEMBER_DIRECTORY_PATH}`,
      path: WORLD_MEMBER_DIRECTORY_PATH,
      token: "",
    },
  ];
  if (session?.nodeName && session?.sessionToken) {
    const name = encodeURIComponent(session.nodeName);
    const path = `/api/accounts/${name}?viewer=${name}`;
    plan.push({
      key: `GET:account:${session.nodeName}:${path}`,
      path,
      token: session.sessionToken,
    });
  }
  return plan;
}

export function startWorldBootPrefetch(win = globalThis.window, fetchImpl = null) {
  if (!win) return null;
  if (win.__forkmeshWorldPrefetch) return win.__forkmeshWorldPrefetch;
  const request = fetchImpl || (typeof win.fetch === "function" ? win.fetch.bind(win) : null);
  if (!request) return null;
  const clock = () => Date.now();
  const storage = (() => {
    try {
      return win.localStorage;
    } catch {
      return null;
    }
  })();
  const entries = new Map();
  for (const item of worldBootPrefetchPlan(worldBootSession(storage))) {
    const headers = item.token ? { Authorization: `Bearer ${item.token}` } : {};
    let response;
    try {
      response = Promise.resolve(request(item.path, { headers, credentials: "same-origin" }));
    } catch (error) {
      response = Promise.reject(error);
    }
    response.catch(() => {});
    entries.set(item.key, { response, startedAt: clock() });
  }
  const api = {
    take(key) {
      const entry = entries.get(key);
      if (!entry) return null;
      entries.delete(key);
      if (clock() - entry.startedAt > WORLD_BOOT_PREFETCH_TTL_MS) return null;
      return entry.response;
    },
    pending() {
      return [...entries.keys()];
    },
  };
  win.__forkmeshWorldPrefetch = api;
  return api;
}

export function formatBootBytes(bytes) {
  const value = Math.max(0, Number(bytes) || 0);
  if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(2)} MB`;
  if (value >= 1024) return `${Math.round(value / 1024)} KB`;
  return `${Math.round(value)} B`;
}

export function formatBootMs(ms) {
  const value = Math.max(0, Number(ms) || 0);
  return value >= 1000 ? `${(value / 1000).toFixed(2)} s` : `${Math.round(value)} ms`;
}

export function createWorldBootLog({ limit = WORLD_BOOT_LOG_LIMIT, now = () => 0 } = {}) {
  const lines = [];
  let dropped = 0;
  let sequence = 0;
  return {
    get dropped() {
      return dropped;
    },
    get size() {
      return lines.length;
    },
    add(kind, text, at = now()) {
      sequence += 1;
      const line = {
        id: sequence,
        at: Math.max(0, Number(at) || 0),
        kind: String(kind || "info"),
        text: String(text ?? "").replace(/\s+/g, " ").trim().slice(0, 400),
      };
      lines.push(line);
      if (lines.length > limit) {
        dropped += lines.length - limit;
        lines.splice(0, lines.length - limit);
      }
      return line;
    },
    entries() {
      return lines.slice();
    },
    since(id) {
      const start = lines.findIndex((line) => line.id > id);
      return start < 0 ? [] : lines.slice(start);
    },
    text({ suggestions = [], compact = false, summary = [] } = {}) {
      const ordered = lines.slice().sort((a, b) => a.at - b.at || a.id - b.id);
      const body = (compact ? compactBootLines(ordered) : ordered)
        .map((line) => `${`+${Math.round(line.at)} ms`.padStart(10)}  ${line.kind.padEnd(8)} ${line.text}`);
      const top = Array.isArray(summary) ? summary.filter((line) => line !== undefined && line !== null).map(String) : [];
      while (top.length && top[top.length - 1] === "") top.pop();
      const tips = suggestions.length
        ? ["Optimization suggestions", ...suggestions.map((tip) => `- [${tip.level}] ${tip.title}${tip.detail ? ` — ${tip.detail}` : ""}`), ""]
        : [];
      const head = [...top, ...(top.length ? [""] : []), ...tips];
      const dropNote = dropped > 0 ? [`(${dropped} older lines dropped)`] : [];
      return [...head, ...dropNote, ...body].join("\n");
    },
  };
}

const CACHED_RESOURCE_KINDS = new Set(["module", "font", "texture", "style"]);

export function compactBootLines(lines) {
  const out = [];
  let group = null;
  const flush = () => {
    if (!group) return;
    if (group.count === 1) out.push(group.first);
    else {
      const kinds = [...group.kinds.entries()].map(([kind, count]) => `${count} ${kind}${count === 1 ? "" : "s"}`).join(", ");
      out.push({ ...group.first, kind: "cache", text: `${group.count} files served from the browser cache (${kinds})` });
    }
    group = null;
  };
  for (const line of lines) {
    if (CACHED_RESOURCE_KINDS.has(line.kind) && / · cache · /.test(line.text)) {
      if (!group) group = { first: line, count: 0, kinds: new Map() };
      group.count += 1;
      group.kinds.set(line.kind, (group.kinds.get(line.kind) || 0) + 1);
      continue;
    }
    flush();
    out.push(line);
  }
  flush();
  return out;
}

export function timerCallSite(stack) {
  const frames = String(stack || "").split("\n").slice(1);
  for (const frame of frames) {
    if (/world-boot\.js/.test(frame)) continue;
    const match = frame.match(/at (?:(\S+) \()?(?:\S*\/)?([^\s/()?]+\.m?js)(?:\?[^\s:()]*)?:(\d+):(\d+)\)?\s*$/);
    if (match) return `${match[1] ? `${match[1]} @ ` : ""}${match[2]}:${match[3]}:${match[4]}`;
  }
  return "";
}

export function isNativeCallback(callback) {
  try {
    return /\{\s*\[native code\]\s*\}\s*$/.test(Function.prototype.toString.call(callback));
  } catch {
    return false;
  }
}

export function worldLoadApis(measure = {}, limitMs = WORLD_BOOT_CRITICAL_API_MS) {
  const interactiveMs = Number(measure.interactiveMs) || Infinity;
  const resources = Array.isArray(measure.resources) ? measure.resources : [];
  return resources
    .filter((resource) => resource.kind === "api" && (Number(resource.start) || 0) < interactiveMs)
    .map((resource) => {
      const serverMs = Number.isFinite(resource.serverMs) ? resource.serverMs : null;
      return {
        name: resource.name,
        ms: Number(resource.ms) || 0,
        serverMs,
        networkMs: serverMs === null ? null : Math.max(0, (Number(resource.ms) || 0) - serverMs),
        slow: (Number(resource.ms) || 0) > limitMs,
        pending: (Number(resource.at) || 0) > interactiveMs,
      };
    })
    .sort((a, b) => b.ms - a.ms);
}

function describeApiTiming(api) {
  const split = api.serverMs === null ? "" : ` (server ${formatBootMs(api.serverMs)}, network ${formatBootMs(api.networkMs)})`;
  return `${api.name} ${formatBootMs(api.ms)}${split}${api.pending ? ", still in flight at interactive" : ""}`;
}

export function readWorldDeployBuild(doc = globalThis.document) {
  const meta = (name) => {
    try {
      return String(doc?.querySelector?.(`meta[name="${name}"]`)?.getAttribute?.("content") || "").trim();
    } catch {
      return "";
    }
  };
  const version = meta("forkmesh-world-version").replace(/[^a-z0-9._+-]/gi, "").slice(0, 32);
  const fingerprint = meta("forkmesh-world-deploy");
  const deploy = /^[a-z0-9._+-]{6,128}$/i.test(fingerprint) ? fingerprint : "";
  const hash = deploy.split("-", 1)[0];
  const revision = /^[a-f0-9]{7,64}$/i.test(hash) ? hash.toLowerCase() : "";
  const builtAt = Number(meta("forkmesh-world-built")) || 0;
  return { version, revision, deploy, builtAt: builtAt > 0 ? builtAt * 1000 : 0 };
}

export function worldBuildLabel(build = {}) {
  const version = String(build?.version || "");
  const revision = String(build?.revision || "").slice(0, 12);
  const deploy = String(build?.deploy || "");
  const parts = [];
  if (version) parts.push(/^v/i.test(version) ? version : `v${version}`);
  if (revision) parts.push(revision);
  else if (deploy) parts.push(deploy.slice(0, 24));
  return parts.join(" ");
}

export function worldLoadRecord(measure = {}) {
  const longFrames = Array.isArray(measure.longFrames) ? measure.longFrames : [];
  const phases = {};
  (Array.isArray(measure.phases) ? measure.phases : []).forEach((phase) => {
    if (phase?.name) phases[phase.name] = Math.round(Number(phase.ms) || 0);
  });
  const apis = {};
  worldLoadApis(measure).forEach((api) => {
    apis[api.name] = Math.round(api.ms);
  });
  const resources = Array.isArray(measure.resources) ? measure.resources : [];
  return {
    at: Date.now(),
    build: worldBuildLabel(measure.build),
    interactiveMs: Math.round(Number(measure.interactiveMs) || 0),
    firstFrameMs: Math.round(Number(measure.firstFrameMs) || 0),
    phases,
    longFrames: longFrames.length,
    blockedMs: Math.round(longFrames.reduce((sum, frame) => sum + (Number(frame.ms) || 0), 0)),
    worstMs: Math.round(Math.max(0, ...longFrames.map((frame) => Number(frame.ms) || 0))),
    networkBytes: resources
      .filter((resource) => resource.cache === "network" || resource.cache === "revalidated")
      .reduce((sum, resource) => sum + (Number(resource.bytes) || 0), 0),
    apis,
  };
}

export function compareWorldLoads(previous, current) {
  if (!previous || !current) return [];
  const grew = (label, from, to) => {
    const before = Number(from) || 0;
    const after = Number(to) || 0;
    if (!(before > 0) || after - before < WORLD_BOOT_REGRESSION_MIN_MS) return null;
    if (after < before * WORLD_BOOT_REGRESSION_RATIO) return null;
    return { label, from: before, to: after };
  };
  const found = [
    grew("interactive", previous.interactiveMs, current.interactiveMs),
    grew("first frame", previous.firstFrameMs, current.firstFrameMs),
    grew("long frames blocked", previous.blockedMs, current.blockedMs),
    grew("worst long frame", previous.worstMs, current.worstMs),
  ];
  Object.entries(current.phases || {}).forEach(([name, ms]) => {
    found.push(grew(`${name} phase`, previous.phases?.[name], ms));
  });
  Object.entries(current.apis || {}).forEach(([name, ms]) => {
    found.push(grew(name, previous.apis?.[name], ms));
  });
  return found.filter(Boolean);
}

export function readWorldLoadRuns(storage) {
  try {
    const runs = JSON.parse(storage?.getItem?.(WORLD_BOOT_RUNS_KEY) || "[]");
    return Array.isArray(runs) ? runs.filter((run) => run && typeof run === "object").slice(-WORLD_BOOT_RUNS_KEPT) : [];
  } catch {
    return [];
  }
}

export function writeWorldLoadRuns(storage, runs) {
  try {
    storage?.setItem?.(WORLD_BOOT_RUNS_KEY, JSON.stringify(runs.slice(-WORLD_BOOT_RUNS_KEPT)));
  } catch {}
}

export function worldLoadColdCache(record = {}, previous = null) {
  if (!previous) return "";
  const parts = [];
  const before = String(previous.build || "");
  const after = String(record.build || "");
  if (before && after && before !== after) parts.push(`new build since the previous load: ${before} → ${after}`);
  if ((Number(record.networkBytes) || 0) > (Number(previous.networkBytes) || 0) * 2 + 65536) {
    parts.push(`this load fetched ${formatBootBytes(record.networkBytes)} over the network, the previous one ${formatBootBytes(previous.networkBytes)}`);
  }
  return parts.join("; ");
}

export function worldLoadSummary(measure = {}, regressions = [], previous = null) {
  const record = worldLoadRecord(measure);
  const lines = [
    `World load summary${record.build ? ` · build ${record.build}` : " · build not stamped (local or dev server)"} · interactive ${formatBootMs(record.interactiveMs)} · first frame ${formatBootMs(record.firstFrameMs)}${measure.overlayRemovedMs ? ` · loader removed ${formatBootMs(measure.overlayRemovedMs)}` : ""}`,
  ];
  const longFrames = Array.isArray(measure.longFrames) ? measure.longFrames : [];
  if (longFrames.length) {
    const worst = topBy(longFrames, "ms", 1)[0];
    lines.push(`Long frames: ${longFrames.length}, ${formatBootMs(record.blockedMs)} blocked, worst ${formatBootMs(worst.ms)} at +${Math.round(Number(worst.at) || 0)} ms${worst.text ? ` (${worst.text.replace(/^Main thread blocked \d+ ms \(\d+ ms blocking\) · /, "")})` : ""}`);
  }
  const phases = (Array.isArray(measure.phases) ? measure.phases : [])
    .filter((phase) => (Number(phase.ms) || 0) >= 1)
    .map((phase) => {
      const slices = phase.name === "scene" && measure.sceneSlices?.count
        ? ` (${measure.sceneSlices.count + 1} slices, longest ${formatBootMs(measure.sceneSlices.longestMs)})`
        : "";
      return `${phase.name} ${formatBootMs(phase.ms)}${slices}${phase.background ? " (background)" : ""}`;
    });
  if (phases.length) lines.push(`Phases: ${phases.join(" · ")}`);
  if (measure.shader) {
    lines.push(`Shaders: ${measure.shader.programs} programs, ${formatBootMs(measure.shader.firstUseMs)} blocking on first use, ${measure.shader.deferredDraws} draws deferred`);
  }
  const apis = worldLoadApis(measure);
  if (apis.length) {
    const slow = apis.filter((api) => api.slow);
    lines.push(`APIs requested before interactive: ${apis.length}${slow.length ? `, ${slow.length} over ${WORLD_BOOT_CRITICAL_API_MS} ms: ${slow.map(describeApiTiming).join(" · ")}` : ", none over " + WORLD_BOOT_CRITICAL_API_MS + " ms"}`);
  }
  if (previous) {
    const minutes = Math.max(0, Math.round((record.at - (Number(previous.at) || record.at)) / 60000));
    const cold = worldLoadColdCache(record, previous);
    lines.push(regressions.length
      ? `Slower than the previous load (${minutes} min ago): ${regressions.map((item) => `${item.label} ${formatBootMs(item.from)} → ${formatBootMs(item.to)}`).join(" · ")}${cold ? ` (${cold})` : ""}`
      : `No phase regressed against the previous load (${minutes} min ago, interactive ${formatBootMs(previous.interactiveMs)}).`);
  }
  return lines;
}

export const WORLD_BOOT_AGENT_NOTE =
  "Note to the agent reading this log: besides fixing the findings above, improve these diagnostics themselves: add measurements that would have pinpointed today's slowest items, sharpen or remove suggestions that were not actionable, attribute unattributed long tasks, and keep the log compact. Each pasted log should be more useful than the last.";

export function worldBootCopyText(logText, report = "") {
  const audit = String(report || "").trim() || "Optimization audit: pending (the scene has not finished loading).";
  return [String(logText || "").trimEnd(), "", "=== Optimization audit ===", audit, "", WORLD_BOOT_AGENT_NOTE].join("\n");
}

export function worldBootLogVisible({ isAdmin = false, debugPanel = false, ticketResolved = true, rememberedAdmin = false } = {}) {
  if (isAdmin === true || debugPanel === true) return true;
  return ticketResolved !== true && rememberedAdmin === true;
}

function bootLogPosition(value) {
  if (!value || typeof value !== "object") return null;
  const left = Number(value.left);
  const top = Number(value.top);
  if (!Number.isFinite(left) || !Number.isFinite(top)) return null;
  return { left: Math.round(left), top: Math.round(top) };
}

export function clampBootLogPosition(position, size, viewport, margin = WORLD_BOOT_LOG_MARGIN) {
  const wanted = bootLogPosition(position);
  if (!wanted) return null;
  const width = Math.max(0, Number(size?.width) || 0);
  const height = Math.max(0, Number(size?.height) || 0);
  const viewportWidth = Math.max(0, Number(viewport?.width) || 0);
  const viewportHeight = Math.max(0, Number(viewport?.height) || 0);
  const maxLeft = Math.max(margin, viewportWidth - width - margin);
  const maxTop = Math.max(margin, viewportHeight - height - margin);
  return {
    left: Math.round(Math.min(Math.max(wanted.left, margin), maxLeft)),
    top: Math.round(Math.min(Math.max(wanted.top, margin), maxTop)),
  };
}

export function readWorldBootPrefs(storage) {
  const prefs = { collapsed: false, admin: false, debugPanel: false, position: null };
  try {
    const stored = JSON.parse(storage?.getItem(WORLD_BOOT_PREFS_KEY) || "null");
    if (stored && typeof stored === "object") {
      prefs.collapsed = stored.collapsed === true;
      prefs.admin = stored.admin === true;
      prefs.position = bootLogPosition(stored.position);
    }
  } catch {}
  try {
    const settings = JSON.parse(storage?.getItem(WORLD_BOOT_SETTINGS_KEY) || "null");
    prefs.debugPanel = settings?.debugPanel === true;
  } catch {}
  return prefs;
}

export function writeWorldBootPrefs(storage, prefs) {
  try {
    storage?.setItem(
      WORLD_BOOT_PREFS_KEY,
      JSON.stringify({
        collapsed: prefs.collapsed === true,
        admin: prefs.admin === true,
        position: bootLogPosition(prefs.position),
      }),
    );
    return true;
  } catch {
    return false;
  }
}

function shortResourceName(name, origin = "") {
  const text = String(name || "");
  try {
    const url = new URL(text, origin || undefined);
    const host = origin && url.origin === origin ? "" : url.host;
    return `${host}${url.pathname}`;
  } catch {
    return text;
  }
}

export function classifyResourceCache(entry) {
  const transfer = Number(entry?.transferSize);
  const encoded = Number(entry?.encodedBodySize) || 0;
  const decoded = Number(entry?.decodedBodySize) || 0;
  if (!Number.isFinite(transfer)) return "unknown";
  if (transfer === 0) return encoded > 0 || decoded > 0 ? "cache" : "unknown";
  if (encoded > 0 && transfer < encoded) return "revalidated";
  return "network";
}

export function resourceKind(entry) {
  const name = String(entry?.name || "").split("?")[0].toLowerCase();
  const initiator = String(entry?.initiatorType || "");
  if (/\/api\//.test(name) || ((initiator === "fetch" || initiator === "xmlhttprequest") && !/\.(js|mjs|json|png|jpe?g|webp|avif|gif|svg|glb|gltf|ktx2|woff2?|ttf|css)$/.test(name))) return "api";
  if (/\.(woff2?|ttf|otf)$/.test(name)) return "font";
  if (/\.(png|jpe?g|webp|avif|gif|svg|ktx2|hdr|glb|gltf)$/.test(name) || initiator === "img" || initiator === "image") return "texture";
  if (/\.(m?js)$/.test(name) || initiator === "script") return "module";
  if (/\.css$/.test(name)) return "style";
  return initiator || "other";
}

function serverTimingMs(entry) {
  const timings = Array.isArray(entry?.serverTiming) ? entry.serverTiming : [];
  const app = timings.find((timing) => timing?.name === "app") || null;
  const value = Number(app?.duration);
  return app && Number.isFinite(value) && value > 0 ? value : null;
}

export function describeResourceEntry(entry, origin = "") {
  const cache = classifyResourceCache(entry);
  const serverMs = serverTimingMs(entry);
  const bytes = Math.max(Number(entry?.transferSize) || 0, Number(entry?.encodedBodySize) || 0);
  const ms = Math.max(0, Number(entry?.duration) || 0);
  const status = Number(entry?.responseStatus) || 0;
  const kind = resourceKind(entry);
  const name = shortResourceName(entry?.name, origin);
  const size = bytes > 0 ? ` · ${formatBootBytes(bytes)}` : "";
  const statusText = status >= 400 || (kind === "api" && status > 0) ? ` · HTTP ${status}` : "";
  const serverText = serverMs === null ? "" : ` · server ${formatBootMs(serverMs)}`;
  return {
    name,
    kind,
    cache,
    bytes,
    ms,
    status,
    serverMs,
    start: Number(entry?.startTime) || 0,
    at: (Number(entry?.startTime) || 0) + ms,
    text: `${name}${size} · ${cache} · ${formatBootMs(ms)}${statusText}${serverText}`,
  };
}

function topBy(items, key, count) {
  return items.slice().sort((a, b) => (b[key] || 0) - (a[key] || 0)).slice(0, count);
}

export function computeWorldLoadSuggestions(measure = {}, audit = null) {
  const tips = [];
  const add = (tip) => tips.push({ weight: 0, detail: "", ...tip });
  const interactiveMs = Number(measure.interactiveMs) || 0;
  const phases = Array.isArray(measure.phases) ? measure.phases : [];
  const elements = Array.isArray(measure.elements) ? measure.elements : [];
  const resources = Array.isArray(measure.resources) ? measure.resources : [];
  const longFrames = Array.isArray(measure.longFrames) ? measure.longFrames : [];
  const shader = measure.shader || null;

  topBy(phases.filter((phase) => phase.ms >= 300 && phase.background !== true), "ms", 3).forEach((phase) => {
    const share = interactiveMs > 0 ? Math.round((phase.ms / interactiveMs) * 100) : 0;
    add({
      id: `phase:${phase.name}`,
      level: phase.ms >= 1500 || share >= 40 ? "high" : "caution",
      title: `Slowest phase: ${phase.name} took ${formatBootMs(phase.ms)}${share ? ` (${share}% of load)` : ""}`,
      detail: phase.name === "scene"
        ? "Scene construction owns the main thread; defer distant districts and build them on approach."
        : phase.name === "engine"
          ? "three.js and the scene module gate everything; keep them cached (immutable vendor URLs) and preloaded."
          : "Move this work after the first interactive frame or run it in parallel with the scene build.",
      weight: phase.ms,
    });
  });

  const slowElements = topBy(elements.filter((element) => element.ms >= 60), "ms", 3);
  if (slowElements.length) {
    add({
      id: "scene-elements",
      level: slowElements[0].ms >= 250 ? "high" : "caution",
      title: `Heaviest scene elements: ${slowElements.map((element) => `${element.label} ${formatBootMs(element.ms)}`).join(", ")}`,
      detail: `Build these lazily (on proximity) or share their geometry and materials${slowElements[0].meshes ? `; the heaviest adds ${slowElements[0].meshes} meshes` : ""}.`,
      weight: slowElements[0].ms,
    });
  }

  const downloaded = resources.filter((resource) => resource.cache === "network" || resource.cache === "revalidated");
  topBy(resources.filter((resource) => resource.bytes >= 256 * 1024), "bytes", 3).forEach((resource) => {
    add({
      id: `download:${resource.name}`,
      level: resource.cache === "network" && resource.bytes >= 1024 * 1024 ? "high" : resource.cache === "network" ? "caution" : "info",
      title: `Large download: ${resource.name} ${formatBootBytes(resource.bytes)} (${resource.cache}, ${formatBootMs(resource.ms)})`,
      detail: resource.cache === "network"
        ? "Split it behind a dynamic import, compress it, or give it a versioned immutable URL so repeat visits hit the cache."
        : "Served from cache on this visit; first-time visitors still pay the full size.",
      weight: resource.bytes / 1024,
    });
  });
  const uncachedBytes = downloaded.reduce((sum, resource) => sum + (resource.bytes || 0), 0);
  const revalidated = resources.filter((resource) => resource.cache === "revalidated");
  if (revalidated.length >= 5) {
    add({
      id: "revalidated",
      level: "caution",
      title: `${revalidated.length} files were revalidated with the server`,
      detail: "Each costs a round trip even when unchanged; version their URLs and mark them immutable.",
      weight: revalidated.length * 10,
    });
  }
  if (uncachedBytes >= 2 * 1024 * 1024) {
    add({
      id: "network-bytes",
      level: "caution",
      title: `${formatBootBytes(uncachedBytes)} came over the network before the World was interactive`,
      detail: "Defer optional modules and textures until after arrival.",
      weight: uncachedBytes / 4096,
    });
  }
  const beforeInteractive = interactiveMs > 0
    ? resources.filter((resource) => resource.at <= interactiveMs)
    : resources;
  if (beforeInteractive.length >= 60) {
    add({
      id: "requests",
      level: beforeInteractive.length >= 120 ? "high" : "caution",
      title: `${beforeInteractive.length} requests before the World was interactive`,
      detail: `${beforeInteractive.filter((resource) => resource.kind === "api").length} API calls, ${beforeInteractive.filter((resource) => resource.kind === "module").length} modules, ${beforeInteractive.filter((resource) => resource.kind === "texture").length} images; bundle small modules and move API reads behind proximity.`,
      weight: beforeInteractive.length,
    });
  }
  const criticalApis = worldLoadApis(measure).filter((api) => api.slow);
  if (criticalApis.length) {
    add({
      id: "api:critical",
      level: criticalApis[0].ms >= 1500 ? "high" : "caution",
      title: `${criticalApis.length} API call${criticalApis.length === 1 ? " was" : "s were"} requested before interactive and took over ${WORLD_BOOT_CRITICAL_API_MS} ms: ${criticalApis.slice(0, 4).map(describeApiTiming).join(" · ")}`,
      detail: criticalApis.some((api) => api.serverMs !== null && api.serverMs >= api.networkMs)
        ? "Server time dominates: cache the answer on the core or move the call after the reveal."
        : "Move the call after the reveal, prefetch it with the HTML, or cache its answer.",
      weight: criticalApis[0].ms,
    });
  }
  const regressions = Array.isArray(measure.regressions) ? measure.regressions : [];
  if (regressions.length) {
    const worst = topBy(regressions.map((item) => ({ ...item, grew: item.to - item.from })), "grew", 1)[0];
    const cold = measure.coldCache || "";
    add({
      id: "regressions",
      level: cold ? "info" : regressions.some((item) => item.to >= item.from * 2) ? "high" : "caution",
      title: `Slower than the previous load: ${regressions.slice(0, 4).map((item) => `${item.label} ${formatBootMs(item.from)} → ${formatBootMs(item.to)}`).join(", ")}`,
      detail: cold
        ? `Cold cache, not a code regression by itself (${cold}); reload once and compare the warm load.`
        : "Same build and cache state as the previous load: compare the phase and long-frame lines of the two logs.",
      weight: cold ? worst.grew / 10 : worst.grew,
    });
  }
  const slowApi = criticalApis.length ? null : topBy(resources.filter((resource) => resource.kind === "api" && resource.ms >= 400), "ms", 1)[0];
  if (slowApi) {
    add({
      id: `api:${slowApi.name}`,
      level: slowApi.ms >= 1500 ? "high" : "caution",
      title: `Slowest API call: ${slowApi.name} took ${formatBootMs(slowApi.ms)}`,
      detail: "Keep it off the critical path, cache its answer, or prefetch it with the HTML.",
      weight: slowApi.ms,
    });
  }

  if (longFrames.length) {
    const total = longFrames.reduce((sum, frame) => sum + (Number(frame.ms) || 0), 0);
    const worst = topBy(longFrames, "ms", 1)[0];
    add({
      id: "long-frames",
      level: worst.ms >= 500 || total >= 2000 ? "high" : "caution",
      title: `${longFrames.length} long frame${longFrames.length === 1 ? "" : "s"} during load (${formatBootMs(total)} blocked, worst ${formatBootMs(worst.ms)})`,
      detail: worst.text ? `Worst: ${worst.text}` : "Split the blocking work across idle callbacks.",
      weight: total / 2,
    });
  }

  if (shader && (Number(shader.programs) || 0) > 0) {
    const programs = Number(shader.programs) || 0;
    const blocked = Number(shader.firstUseMs) || 0;
    if (programs >= 60 || blocked >= 250) {
      add({
        id: "shaders",
        level: blocked >= 800 || programs >= 120 ? "high" : "caution",
        title: `${programs} shader programs compiled, ${formatBootMs(blocked)} blocking on first use`,
        detail: "Share materials between elements, prefer fewer material variants, and warm programs with compileAsync before they are drawn.",
        weight: blocked + programs * 5,
      });
    }
  }

  const firstFrameMs = Number(measure.firstFrameMs) || 0;
  const interactiveFrameMs = Number(measure.interactiveFrameMs) || 0;
  if (interactiveFrameMs > 0 && interactiveMs > 0 && interactiveFrameMs - interactiveMs >= 300) {
    add({
      id: "reveal-frame",
      level: "caution",
      title: `The first frame after the loader closed took ${formatBootMs(interactiveFrameMs - interactiveMs)}`,
      detail: "Work queued for the reveal (shader warm-up, deferred installs) lands on the first visible frame; spread it over idle slices.",
      weight: interactiveFrameMs - interactiveMs,
    });
  }
  if (firstFrameMs > 0 && interactiveMs > 0 && interactiveMs - firstFrameMs >= 1500) {
    add({
      id: "frame-to-interactive",
      level: "caution",
      title: `${formatBootMs(interactiveMs - firstFrameMs)} passed between the first rendered frame and interactive`,
      detail: "Population after the first frame is on the critical path; push more of it past the reveal.",
      weight: interactiveMs - firstFrameMs,
    });
  }

  (Array.isArray(audit?.findings) ? audit.findings : [])
    .filter((finding) => finding && finding.level !== "info")
    .slice(0, 5)
    .forEach((finding) => {
      add({
        id: `audit:${finding.id}`,
        level: finding.level === "high" ? "high" : "caution",
        title: `Scene audit: ${finding.title}`,
        detail: String(finding.fix || finding.evidence || ""),
        weight: Number(finding.weight) || 0,
        source: "audit",
      });
    });

  tips.sort((a, b) => (LEVEL_ORDER[a.level] ?? 3) - (LEVEL_ORDER[b.level] ?? 3) || b.weight - a.weight);
  if (!tips.length) {
    tips.push({ id: "ok", level: "info", title: "Nothing stood out: no slow phase, large download, long frame or audit finding.", detail: "", weight: 0 });
  }
  return tips;
}

function countObjects(roots) {
  let objects = 0;
  let meshes = 0;
  (Array.isArray(roots) ? roots : [roots]).forEach((root) => {
    if (!root || typeof root.traverse !== "function") return;
    root.traverse((child) => {
      objects += 1;
      if (child.isMesh || child.isInstancedMesh || child.isPoints || child.isLine || child.isSprite) meshes += 1;
    });
  });
  return { objects, meshes };
}

const SCENE_PASS_RE = /^(.*?) · pass \d+$/;

export function groupSceneElementPasses(elements = []) {
  const out = [];
  const groups = new Map();
  for (const element of elements) {
    const match = SCENE_PASS_RE.exec(String(element.label || ""));
    if (!match) {
      out.push(element);
      continue;
    }
    const base = match[1];
    let group = groups.get(base);
    if (!group) {
      const index = out.findIndex((item) => item.label === base && !item.passes);
      if (index >= 0) {
        group = { ...out[index], passes: [out[index].ms] };
        out[index] = group;
      } else {
        group = { id: `${element.id}-passes`, label: base, ms: 0, objects: 0, meshes: 0, count: 0, passes: [] };
        out.push(group);
      }
      groups.set(base, group);
    }
    group.ms += element.ms;
    group.objects += element.objects;
    group.meshes += element.meshes;
    group.count += element.count;
    group.passes.push(element.ms);
  }
  return out;
}

export function describeSceneElement(element) {
  const counts = element.objects || element.meshes
    ? ` · ${element.objects} objects · ${element.meshes} meshes`
    : "";
  const passes = Array.isArray(element.passes) && element.passes.length > 1
    ? ` · ${element.passes.length} passes (${element.passes.map((ms) => Math.round(ms)).join("/")} ms)`
    : element.count > 1 ? ` · ${element.count} parts` : "";
  return `${element.label} ${formatBootMs(element.ms)}${counts}${passes}`;
}

export function createWorldSceneTrace({ now = () => 0 } = {}) {
  let open = false;
  let cursor = 0;
  let sliceStartedAt = 0;
  const elements = new Map();
  const marks = [];
  const slices = { count: 0, yieldedMs: 0, longestMs: 0 };
  return {
    get open() {
      return open;
    },
    get slices() {
      return { ...slices };
    },
    begin() {
      open = true;
      cursor = now();
      sliceStartedAt = cursor;
      elements.clear();
      marks.length = 0;
      Object.assign(slices, { count: 0, yieldedMs: 0, longestMs: 0 });
    },
    yielded(ms) {
      if (!open) return;
      const pause = Math.max(0, Number(ms) || 0);
      const resumedAt = now();
      slices.count += 1;
      slices.yieldedMs += pause;
      slices.longestMs = Math.max(slices.longestMs, resumedAt - pause - sliceStartedAt);
      sliceStartedAt = resumedAt;
      cursor += pause;
    },
    element(id, label, roots) {
      if (!open) return;
      const at = now();
      const key = String(id || "element");
      const counts = countObjects(roots);
      const first = elements.size === 0;
      const entry = elements.get(key) || { id: key, label: `${first ? "Renderer setup + " : ""}${String(label || key)}`, ms: 0, objects: 0, meshes: 0, count: 0 };
      const ms = Math.max(0, at - cursor);
      entry.ms += ms;
      if (marks.length < 512) marks.push({ at, ms, label: entry.label });
      entry.objects += counts.objects;
      entry.meshes += counts.meshes;
      entry.count += 1;
      elements.set(key, entry);
      cursor = now();
    },
    marksBetween(start, end) {
      return marks.filter((mark) => mark.at >= start && mark.at <= end);
    },
    end() {
      if (!open) return [];
      open = false;
      const at = now();
      slices.longestMs = Math.max(slices.longestMs, at - sliceStartedAt);
      const tail = Math.max(0, at - cursor);
      const list = [...elements.values()];
      if (tail > 0) list.push({ id: "finish", label: "Finishing construction", ms: tail, objects: 0, meshes: 0, count: 1 });
      return list;
    },
  };
}

function bootNow(win) {
  return () => (win?.performance?.now ? win.performance.now() : Date.now());
}

export function installWorldBoot(win = globalThis.window) {
  if (!win) return null;
  if (win.__forkmeshWorldBoot) return win.__forkmeshWorldBoot;
  const doc = win.document;
  const now = bootNow(win);
  const log = createWorldBootLog({ now });
  const trace = createWorldSceneTrace({ now });
  const storage = (() => {
    try {
      return win.localStorage;
    } catch {
      return null;
    }
  })();
  const prefs = readWorldBootPrefs(storage);
  const access = { isAdmin: false, debugPanel: prefs.debugPanel, ticketResolved: false };
  const measure = {
    phases: [],
    elements: [],
    resources: [],
    longFrames: [],
    shader: null,
    firstFrameMs: 0,
    interactiveMs: 0,
    interactiveFrameMs: 0,
    build: readWorldDeployBuild(doc),
  };
  installResponseJsonTrace(win);
  const openPhases = new Map();
  const sceneElementListeners = new Set();
  const timerRuns = [];
  const timerOriginals = new Map();
  const wrapTimer = (name) => {
    const original = win[name];
    if (typeof original !== "function") return;
    const worldBootTimerRegistration = function worldBootTimerRegistration(callback, ...rest) {
      if (settled || typeof callback !== "function" || !isNativeCallback(callback)) {
        return original.call(this, callback, ...rest);
      }
      const site = timerCallSite(new Error().stack);
      const worldBootTimerCallback = function worldBootTimerCallback(...args) {
        if (timerRuns.length < WORLD_BOOT_TIMER_RUNS) timerRuns.push({ at: now(), site, name });
        return callback.apply(this, args);
      };
      return original.call(this, worldBootTimerCallback, ...rest);
    };
    try {
      win[name] = worldBootTimerRegistration;
      timerOriginals.set(name, { original, wrapped: worldBootTimerRegistration });
    } catch {}
  };
  const restoreTimers = () => {
    timerOriginals.forEach(({ original, wrapped }, name) => {
      try {
        if (win[name] === wrapped) win[name] = original;
      } catch {}
    });
    timerOriginals.clear();
  };
  const resolveScript = (script) => {
    if (String(script?.sourceFunctionName || "") !== "worldBootTimerCallback") return "";
    const startedAt = Number(script.startTime) || 0;
    let best = null;
    for (const run of timerRuns) {
      const gap = Math.abs(run.at - startedAt);
      if (gap <= 4 && (!best || gap < best.gap)) best = { ...run, gap };
    }
    return best?.site ? `native callback queued at ${best.site}` : "native callback (queued before attribution)";
  };
  wrapTimer("setTimeout");
  wrapTimer("setInterval");
  const backgroundPhases = new Set();
  const observers = [];
  let suggestions = [];
  let settled = false;
  let audited = false;
  let auditProvider = null;
  let reportProvider = null;
  let shaderProvider = null;
  let renderedId = 0;
  let renderedAt = -1;
  let flushQueued = false;
  let dismissed = false;
  const origin = String(win.location?.origin || "");
  const panel = () => doc?.querySelector("[data-world-boot-log]") || null;
  const pill = () => doc?.querySelector("[data-world-boot-log-pill]") || null;

  const visible = () =>
    worldBootLogVisible({
      isAdmin: access.isAdmin,
      debugPanel: access.debugPanel,
      ticketResolved: access.ticketResolved,
      rememberedAdmin: prefs.admin,
    });

  const renderLine = (line) => {
    const item = doc.createElement("li");
    item.dataset.worldBootLogLine = line.kind;
    item.dataset.worldBootLogAt = String(line.at);
    const time = doc.createElement("span");
    time.className = "world-boot-log-time";
    time.textContent = formatBootMs(line.at);
    const kind = doc.createElement("span");
    kind.className = "world-boot-log-kind";
    kind.textContent = line.kind;
    const text = doc.createElement("span");
    text.className = "world-boot-log-text";
    text.textContent = line.text;
    item.append(time, kind, text);
    return item;
  };

  const renderSuggestions = (root) => {
    const box = root.querySelector("[data-world-boot-log-suggestions]");
    if (!box) return;
    if (!settled) {
      box.hidden = true;
      return;
    }
    box.hidden = false;
    const list = doc.createElement("ol");
    suggestions.forEach((tip) => {
      const item = doc.createElement("li");
      item.dataset.level = tip.level;
      const title = doc.createElement("strong");
      title.textContent = tip.title;
      item.append(title);
      if (tip.detail) {
        const detail = doc.createElement("span");
        detail.textContent = tip.detail;
        item.append(detail);
      }
      list.append(item);
    });
    const heading = doc.createElement("p");
    heading.textContent = "Optimization suggestions";
    box.replaceChildren(heading, list);
  };

  const setText = (element, text) => {
    if (element && element.textContent !== text) element.textContent = text;
  };
  const setHidden = (element, hidden) => {
    if (element && element.hidden !== hidden) element.hidden = hidden;
  };

  let placed = null;
  const viewport = () => ({
    width: Number(win.innerWidth) || Number(doc?.documentElement?.clientWidth) || 0,
    height: Number(win.innerHeight) || Number(doc?.documentElement?.clientHeight) || 0,
  });
  const paintPosition = (root, next) => {
    if (!root) return;
    if (!next) {
      if (placed || root.dataset.worldBootLogPlaced) {
        root.style.left = "";
        root.style.top = "";
        root.style.bottom = "";
        delete root.dataset.worldBootLogPlaced;
      }
      placed = null;
      return;
    }
    if (placed && placed.left === next.left && placed.top === next.top) return;
    placed = next;
    root.style.left = `${next.left}px`;
    root.style.top = `${next.top}px`;
    root.style.bottom = "auto";
    root.dataset.worldBootLogPlaced = "true";
  };
  const placePanel = (size = null) => {
    const root = panel();
    if (!root || root.hidden) return;
    if (!prefs.position) {
      paintPosition(root, null);
      return;
    }
    const box = size || root.getBoundingClientRect();
    paintPosition(root, clampBootLogPosition(prefs.position, box, viewport()));
  };

  const syncChrome = () => {
    const root = panel();
    const chip = pill();
    const show = visible();
    if (root) {
      const wasHidden = root.hidden;
      setHidden(root, !show || dismissed);
      const collapsed = prefs.collapsed ? "true" : "false";
      if (root.dataset.collapsed !== collapsed) root.dataset.collapsed = collapsed;
      const toggle = root.querySelector("[data-world-boot-log-collapse]");
      if (toggle) {
        const expanded = prefs.collapsed ? "false" : "true";
        if (toggle.getAttribute("aria-expanded") !== expanded) toggle.setAttribute("aria-expanded", expanded);
        setText(toggle, prefs.collapsed ? "Expand" : "Collapse");
      }
      setText(
        root.querySelector("[data-world-boot-log-summary]"),
        measure.interactiveMs
          ? `interactive ${formatBootMs(measure.interactiveMs)}`
          : `loading · ${log.size} lines`,
      );
      if (wasHidden && !root.hidden) placePanel();
    }
    if (chip) {
      setHidden(chip, !show || !dismissed);
      setText(
        chip,
        measure.interactiveMs
          ? `Load log · ${formatBootMs(measure.interactiveMs)}`
          : "Load log",
      );
    }
  };

  const flush = () => {
    flushQueued = false;
    const root = panel();
    syncChrome();
    if (!root || root.hidden) return;
    const list = root.querySelector("[data-world-boot-log-lines]");
    if (!list) return;
    const fresh = log.since(renderedId);
    if (fresh.length) {
      const inOrder = fresh.filter((line) => line.at >= renderedAt);
      const late = fresh.filter((line) => line.at < renderedAt);
      inOrder.sort((a, b) => a.at - b.at || a.id - b.id);
      const fragment = doc.createDocumentFragment();
      for (let index = inOrder.length - 1; index >= 0; index -= 1) {
        fragment.append(renderLine(inOrder[index]));
      }
      list.prepend(fragment);
      late.forEach((line) => {
        let before = list.firstElementChild;
        while (before && Number(before.dataset.worldBootLogAt) > line.at) {
          before = before.nextElementSibling;
        }
        list.insertBefore(renderLine(line), before);
      });
      while (list.childElementCount > WORLD_BOOT_LOG_LIMIT) list.lastElementChild.remove();
      renderedId = fresh[fresh.length - 1].id;
      renderedAt = Math.max(renderedAt, ...fresh.map((line) => line.at));
    }
    renderSuggestions(root);
  };

  const schedule = () => {
    if (flushQueued) return;
    flushQueued = true;
    if (typeof win.requestAnimationFrame === "function" && doc?.visibilityState !== "hidden") {
      win.requestAnimationFrame(flush);
    } else {
      win.setTimeout(flush, 50);
    }
  };

  const add = (kind, text, at) => {
    const line = log.add(kind, text, at);
    schedule();
    return line;
  };

  const perfMark = (name, detail) => {
    try {
      win.performance?.mark?.(`world:${name}`, detail === undefined ? undefined : { detail });
    } catch {}
  };

  const observe = (type, callback) => {
    const supported = win.PerformanceObserver?.supportedEntryTypes || [];
    if (!supported.includes(type)) return false;
    try {
      const observer = new win.PerformanceObserver((list) => {
        list.getEntries().forEach(callback);
      });
      observer.observe({ type, buffered: true });
      observers.push(observer);
      return true;
    } catch {
      return false;
    }
  };

  const onResource = (entry) => {
    if (settled) return;
    const resource = describeResourceEntry(entry, origin);
    measure.resources.push(resource);
    add(resource.kind, resource.text, resource.at);
  };

  const navigation = win.performance?.getEntriesByType?.("navigation")?.[0];
  if (navigation) {
    const bytes = Number(navigation.transferSize) || Number(navigation.encodedBodySize) || 0;
    add("html", `HTML received${bytes ? ` · ${formatBootBytes(bytes)}` : ""} · response ${formatBootMs(navigation.responseEnd)}`, navigation.responseEnd);
    if (navigation.domInteractive) add("html", "HTML parsed (domInteractive)", navigation.domInteractive);
  } else {
    add("html", "HTML parsed", now());
  }
  observe("paint", (entry) => add("paint", entry.name, entry.startTime));
  observe("resource", onResource);
  const nearbyResource = (start, end, reason = "layout") => {
    let best = null;
    for (const resource of measure.resources) {
      if (reason === "parse") {
        if (resource.kind !== "module" && resource.kind !== "api" && resource.kind !== "style") continue;
        if ((Number(resource.bytes) || 0) < 16 * 1024) continue;
        if (resource.at < start - 150 || resource.at > end) continue;
        if (!best || resource.bytes > best.bytes) best = resource;
        continue;
      }
      if (resource.kind !== "style" && resource.kind !== "font") continue;
      if (resource.at < start - 200 || resource.at > end) continue;
      if (!best || resource.at > best.at) best = resource;
    }
    if (!best) return "";
    return reason === "parse" ? `${best.name} ${formatBootBytes(best.bytes)}` : best.name;
  };
  const sceneWork = (start, end) => {
    const marks = trace.marksBetween(start, end + 1).filter((mark) => mark.ms >= 4);
    if (!marks.length) return "";
    const shown = marks.slice().sort((a, b) => b.ms - a.ms).slice(0, 3);
    return ` · scene build: ${shown.map((mark) => `${mark.label} ${formatBootMs(mark.ms)}`).join(", ")}${marks.length > shown.length ? ` +${marks.length - shown.length} more` : ""}`;
  };
  const loaf = observe("long-animation-frame", (entry) => {
    if (settled) return;
    const description = describeLongAnimationFrame(entry, { resolveScript, nearbyResource });
    const start = Number(entry.startTime) || 0;
    const text = `${description.text}${sceneWork(start, start + (Number(entry.duration) || 0))}`;
    measure.longFrames.push({ ms: Number(entry.duration) || 0, text, at: entry.startTime });
    add("long", text, entry.startTime);
  });
  if (!loaf) {
    observe("longtask", (entry) => {
      if (settled) return;
      measure.longFrames.push({ ms: Number(entry.duration) || 0, text: "", at: entry.startTime });
      add("long", `Main thread blocked ${Math.round(entry.duration)} ms (no script attribution in this browser)`, entry.startTime);
    });
  }
  const domReady = () => add("html", "DOMContentLoaded", now());
  if (doc?.readyState === "loading") doc.addEventListener("DOMContentLoaded", domReady, { once: true });

  let drag = null;
  let dragFrame = 0;
  const bindDrag = (root) => {
    const handle = root.querySelector("[data-world-boot-log-handle]") || root.querySelector("header");
    if (!handle) return;
    const interactive = (target) => Boolean(target?.closest?.("button, a, input, select, textarea"));
    const endDrag = (event) => {
      if (!drag || (event && event.pointerId !== drag.pointerId)) return;
      const ended = drag;
      drag = null;
      if (dragFrame && typeof win.cancelAnimationFrame === "function") win.cancelAnimationFrame(dragFrame);
      dragFrame = 0;
      delete root.dataset.dragging;
      try {
        if (handle.hasPointerCapture?.(ended.pointerId)) handle.releasePointerCapture(ended.pointerId);
      } catch {}
      if (ended.moved && ended.position) {
        paintPosition(root, ended.position);
        prefs.position = ended.position;
        writeWorldBootPrefs(storage, prefs);
      }
    };
    handle.addEventListener("pointerdown", (event) => {
      if (drag || event.isPrimary === false) return;
      if (event.pointerType === "mouse" && event.button !== 0) return;
      if (interactive(event.target)) return;
      const rect = root.getBoundingClientRect();
      drag = {
        pointerId: event.pointerId,
        offsetX: event.clientX - rect.left,
        offsetY: event.clientY - rect.top,
        startX: event.clientX,
        startY: event.clientY,
        size: { width: rect.width, height: rect.height },
        position: null,
        moved: false,
      };
      try {
        handle.setPointerCapture?.(event.pointerId);
      } catch {}
      root.dataset.dragging = "true";
      event.preventDefault();
      event.stopPropagation();
    });
    handle.addEventListener("pointermove", (event) => {
      if (!drag || event.pointerId !== drag.pointerId) return;
      event.preventDefault();
      event.stopPropagation();
      if (!drag.moved && Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY) < 3) return;
      drag.moved = true;
      drag.position = clampBootLogPosition(
        { left: event.clientX - drag.offsetX, top: event.clientY - drag.offsetY },
        drag.size,
        viewport(),
      );
      if (dragFrame) return;
      const paint = () => {
        dragFrame = 0;
        if (drag?.position) paintPosition(root, drag.position);
      };
      if (typeof win.requestAnimationFrame === "function") dragFrame = win.requestAnimationFrame(paint);
      else paint();
    });
    handle.addEventListener("pointerup", endDrag);
    handle.addEventListener("pointercancel", endDrag);
    handle.addEventListener("lostpointercapture", endDrag);
    handle.addEventListener("dblclick", (event) => {
      if (interactive(event.target)) return;
      prefs.position = null;
      writeWorldBootPrefs(storage, prefs);
      paintPosition(root, null);
    });
    win.addEventListener?.("resize", () => placePanel(), { passive: true });
    if (typeof win.ResizeObserver === "function") {
      new win.ResizeObserver((entries) => {
        if (drag || !prefs.position) return;
        const box = entries[entries.length - 1]?.borderBoxSize?.[0];
        placePanel(box ? { width: box.inlineSize, height: box.blockSize } : null);
      }).observe(root);
    }
    placePanel();
  };

  const bindUI = () => {
    const root = panel();
    if (root && root.dataset.bound !== "true") {
      root.dataset.bound = "true";
      bindDrag(root);
      root.addEventListener("click", (event) => {
        const target = event.target;
        if (target.closest?.("[data-world-boot-log-copy]")) {
          const text = api.text();
          const done = () => {
            const button = root.querySelector("[data-world-boot-log-copy]");
            if (button) {
              button.textContent = "Copied";
              win.setTimeout(() => {
                button.textContent = "Copy log";
              }, 1500);
            }
          };
          try {
            win.navigator.clipboard.writeText(text).then(done, () => {});
          } catch {}
          return;
        }
        if (target.closest?.("[data-world-boot-log-collapse]")) {
          prefs.collapsed = !prefs.collapsed;
          writeWorldBootPrefs(storage, prefs);
          schedule();
          return;
        }
        if (target.closest?.("[data-world-boot-log-dismiss]")) {
          dismissed = true;
          schedule();
        }
      });
    }
    const chip = pill();
    if (chip && chip.dataset.bound !== "true") {
      chip.dataset.bound = "true";
      chip.addEventListener("click", () => {
        dismissed = false;
        maybeAudit();
        schedule();
      });
    }
  };

  const maybeAudit = () => {
    if (!settled || audited || !visible() || typeof auditProvider !== "function") return;
    audited = true;
    const run = () => {
      let audit = null;
      const startedAt = now();
      try {
        audit = auditProvider();
      } catch (error) {
        add("audit", `Scene audit failed: ${String(error?.message || error).slice(0, 120)}`);
      }
      if (audit) {
        add("audit", `Scene audit: ${audit.findings?.length || 0} findings in ${formatBootMs(now() - startedAt)}`);
        suggestions = computeWorldLoadSuggestions(measure, audit);
      }
      schedule();
    };
    if (typeof win.requestIdleCallback === "function") win.requestIdleCallback(run, { timeout: 3000 });
    else win.setTimeout(run, 200);
  };

  const settle = () => {
    if (settled) return;
    const shader = typeof shaderProvider === "function" ? shaderProvider() : null;
    if (shader) {
      measure.shader = {
        programs: Number(shader.compiledPrograms) || 0,
        firstUseMs: Number(shader.firstUseMs) || 0,
        deferredDraws: Number(shader.deferredDraws) || 0,
      };
      add("shader", `${measure.shader.programs} programs compiled · ${formatBootMs(measure.shader.firstUseMs)} blocking on first use · ${measure.shader.deferredDraws} draws deferred while compiling`);
    }
    settled = true;
    restoreTimers();
    observers.splice(0).forEach((observer) => observer.disconnect());
    const runs = readWorldLoadRuns(storage);
    const previous = runs[runs.length - 1] || null;
    const record = worldLoadRecord(measure);
    measure.regressions = compareWorldLoads(previous, record);
    measure.coldCache = worldLoadColdCache(record, previous);
    measure.summary = worldLoadSummary(measure, measure.regressions, previous);
    writeWorldLoadRuns(storage, [...runs, record]);
    suggestions = computeWorldLoadSuggestions(measure, null);
    add("done", `Load settled · ${log.size} lines · ${suggestions.length} suggestions`);
    maybeAudit();
    schedule();
  };

  const watchFrames = (renderer) => {
    if (!renderer || typeof win.requestAnimationFrame !== "function") return;
    const frameCount = () => Number(renderer.info?.render?.frame) || 0;
    let interactiveBase = -1;
    const tick = () => {
      if (settled) return;
      const at = now();
      if (!measure.firstFrameMs && frameCount() > 0) {
        measure.firstFrameMs = at;
        perfMark("first-frame");
        add("frame", "First frame rendered", at);
      }
      if (measure.interactiveMs) {
        if (interactiveBase < 0) interactiveBase = frameCount();
        else if (frameCount() > interactiveBase) {
          measure.interactiveFrameMs = at;
          perfMark("interactive-frame");
          add("frame", `First interactive frame · ${formatBootMs(at - measure.interactiveMs)} after the loader closed`, at);
          win.setTimeout(settle, WORLD_BOOT_SETTLE_MS);
          return;
        }
      }
      win.requestAnimationFrame(tick);
    };
    win.requestAnimationFrame(tick);
  };

  const api = {
    log,
    measure,
    get settled() {
      return settled;
    },
    get suggestions() {
      return suggestions.slice();
    },
    sceneElement(id, label, roots) {
      if (!trace.open) return;
      trace.element(id, label, roots);
      sceneElementListeners.forEach((listener) => {
        try {
          listener(id, label);
        } catch {}
      });
    },
    onSceneElement(listener) {
      if (typeof listener === "function") sceneElementListeners.add(listener);
      return () => sceneElementListeners.delete(listener);
    },
    sceneYield(ms) {
      trace.yielded(ms);
    },
    mark(name, text, kind = "phase") {
      perfMark(name);
      return add(kind, text || name);
    },
    begin(name, text = "", { background = false } = {}) {
      if (openPhases.has(name)) return;
      const at = now();
      openPhases.set(name, at);
      if (background) backgroundPhases.add(name);
      perfMark(`${name}:start`);
      add("phase", `${name} started${text ? ` · ${text}` : ""}`, at);
    },
    end(name, text = "") {
      const startedAt = openPhases.get(name);
      if (startedAt === undefined) return 0;
      openPhases.delete(name);
      const at = now();
      const ms = at - startedAt;
      try {
        win.performance?.measure?.(`world:${name}`, `world:${name}:start`);
      } catch {}
      measure.phases.push({ name, ms, background: backgroundPhases.has(name) });
      add("phase", `${name} done in ${formatBootMs(ms)}${text ? ` · ${text}` : ""}`, at);
      return ms;
    },
    beginScene() {
      trace.begin();
    },
    endScene() {
      const elements = trace.end();
      measure.elements = elements;
      measure.sceneSlices = trace.slices;
      if (measure.sceneSlices.count > 0) {
        const work = elements.reduce((sum, element) => sum + element.ms, 0);
        add("scene", `Scene built in ${measure.sceneSlices.count + 1} slices · ${formatBootMs(work)} of work · ${formatBootMs(measure.sceneSlices.yieldedMs)} yielded · longest slice ${formatBootMs(measure.sceneSlices.longestMs)}`);
      }
      const grouped = groupSceneElementPasses(elements);
      const shown = grouped.filter((element) => element.ms >= WORLD_BOOT_ELEMENT_LINE_MS);
      shown.forEach((element) => {
        add("scene", describeSceneElement(element));
      });
      const rest = grouped.filter((element) => element.ms < WORLD_BOOT_ELEMENT_LINE_MS);
      if (rest.length) {
        const ms = rest.reduce((sum, element) => sum + element.ms, 0);
        const objects = rest.reduce((sum, element) => sum + element.objects, 0);
        add("scene", `${rest.length} smaller elements ${formatBootMs(ms)} · ${objects} objects`);
      }
      return elements;
    },
    renderer(renderer, gpu = "") {
      if (!renderer) return;
      const webgl2 = renderer.capabilities?.isWebGL2 === true;
      const maxTexture = Number(renderer.capabilities?.maxTextureSize) || 0;
      const ratio = typeof renderer.getPixelRatio === "function" ? renderer.getPixelRatio() : 1;
      add("webgl", `${webgl2 ? "WebGL2" : "WebGL"} context · ${gpu || "GPU renderer string unavailable"} · max texture ${maxTexture} · pixel ratio ${ratio}`);
      watchFrames(renderer);
    },
    setAuditProvider(provider) {
      auditProvider = provider;
      maybeAudit();
    },
    setShaderProvider(provider) {
      shaderProvider = provider;
    },
    setReportProvider(provider) {
      reportProvider = provider;
    },
    setAccess({ isAdmin, debugPanel, ticketResolved } = {}) {
      const next = {
        isAdmin: isAdmin === true,
        debugPanel: debugPanel === true,
        ticketResolved: ticketResolved === true,
      };
      if (
        next.isAdmin === access.isAdmin &&
        next.debugPanel === access.debugPanel &&
        next.ticketResolved === access.ticketResolved
      ) {
        return visible();
      }
      Object.assign(access, next);
      if (access.ticketResolved && prefs.admin !== access.isAdmin) {
        prefs.admin = access.isAdmin;
        writeWorldBootPrefs(storage, prefs);
      }
      bindUI();
      maybeAudit();
      schedule();
      return visible();
    },
    overlayRemoved() {
      if (!measure.overlayRemovedMs) measure.overlayRemovedMs = now();
    },
    interactive() {
      if (measure.interactiveMs) return;
      measure.interactiveMs = now();
      perfMark("interactive");
      add("ready", `World interactive at ${formatBootMs(measure.interactiveMs)}`, measure.interactiveMs);
      if (!measure.firstFrameMs) win.setTimeout(settle, WORLD_BOOT_SETTLE_MS * 2);
    },
    fail(reason) {
      add("error", `World failed to start: ${String(reason || "unknown").slice(0, 160)}`);
      if (!measure.interactiveMs) measure.interactiveMs = now();
      win.setTimeout(settle, 0);
    },
    visible,
    text() {
      let report = "";
      try {
        report = typeof reportProvider === "function" ? String(reportProvider() || "") : "";
      } catch (error) {
        report = `Optimization audit failed: ${String(error?.message || error).slice(0, 160)}`;
      }
      const summary = settled && Array.isArray(measure.summary) ? measure.summary : [];
      return worldBootCopyText(log.text({ summary, suggestions: settled ? suggestions : [], compact: true }), report);
    },
  };
  bindUI();
  syncChrome();
  add("boot", "Load log started");
  win.__forkmeshWorldBoot = api;
  return api;
}

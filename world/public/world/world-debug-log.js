export const WORLD_DEBUG_LOG_CATEGORIES = [
  { id: "ERROR", short: "ERR", icon: "!", title: "Errors: console.error, uncaught exceptions and rejected promises" },
  { id: "WARN", short: "WARN", icon: "▲", title: "Warnings written to the console" },
  { id: "JANK", short: "JANK", icon: "≋", title: "Seconds with frames over 34 ms" },
  { id: "LONG", short: "LOAF", icon: "⏱", title: "Long animation frames: the main thread was blocked for 50 ms or more, with the script that did it" },
  { id: "INPUT", short: "INPUT", icon: "⌨", title: "Movement input that took 80 ms or more to reach a frame" },
  { id: "SHADER", short: "SHDR", icon: "◈", title: "New shader programs compiled and draws held back while compiling" },
  { id: "GPU", short: "GPU", icon: "▦", title: "Geometries and textures created or freed on the GPU" },
  { id: "GC", short: "GC", icon: "♻", title: "JS heap drops of 4 MB or more (garbage collection)" },
  { id: "QUALITY", short: "TIER", icon: "◐", title: "Adaptive quality tier changes" },
  { id: "SOCKET", short: "SOCK", icon: "⇅", title: "World socket state changes, reconnects and backpressure" },
  { id: "NET", short: "HTTP", icon: "⇄", title: "HTTP requests that returned 400 or above" },
  { id: "AUDIT", short: "AUDIT", icon: "✓", title: "Optimization audit runs" },
];

const CATEGORY_IDS = new Set(WORLD_DEBUG_LOG_CATEGORIES.map((category) => category.id));

export function worldDebugLogText(value, limit = 280) {
  let text;
  if (value instanceof Error) {
    text = `${value.name}: ${value.message}`;
  } else if (typeof value === "string") {
    text = value;
  } else {
    try {
      text = JSON.stringify(value);
    } catch {
      text = String(value);
    }
  }
  text = String(text ?? "").replace(/\s+/g, " ").trim();
  return text.length > limit ? `${text.slice(0, limit - 1)}…` : text;
}

export function createWorldDebugLog({ capacity = 800, now = () => Date.now() } = {}) {
  const entries = [];
  const counts = Object.fromEntries(
    WORLD_DEBUG_LOG_CATEGORIES.map((category) => [category.id, 0]),
  );
  const listeners = new Set();
  let total = 0;
  let sequence = 0;
  return {
    get total() {
      return total;
    },
    counts() {
      return { ...counts };
    },
    entries(category = "") {
      return category
        ? entries.filter((entry) => entry.category === category)
        : entries.slice();
    },
    tail(size = 5) {
      return entries.slice(-size);
    },
    add(category, text) {
      if (!CATEGORY_IDS.has(category)) return null;
      sequence += 1;
      const entry = {
        id: sequence,
        at: now(),
        category,
        text: worldDebugLogText(text),
      };
      entries.push(entry);
      if (entries.length > capacity) entries.shift();
      counts[category] += 1;
      total += 1;
      listeners.forEach((listener) => listener(entry));
      return entry;
    },
    reset() {
      entries.length = 0;
      for (const id of Object.keys(counts)) counts[id] = 0;
      total = 0;
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}

function sourceFile(url) {
  const text = String(url || "");
  if (!text) return "";
  try {
    return new URL(text, location.href).pathname.split("/").pop() || text;
  } catch {
    return text.split("/").pop();
  }
}

function unattributedFrameText(entry, nearbyResource) {
  const start = Number(entry?.startTime) || 0;
  const end = start + (Number(entry?.duration) || 0);
  const renderStart = Number(entry?.renderStart) || 0;
  const layoutStart = Number(entry?.styleAndLayoutStart) || 0;
  const parts = [];
  const before = renderStart > start ? renderStart - start : layoutStart > start ? layoutStart - start : 0;
  const callbacks = renderStart > 0 && layoutStart >= renderStart ? layoutStart - renderStart : 0;
  const layout = layoutStart > 0 ? end - layoutStart : 0;
  if (before >= 1) parts.push(`${Math.round(before)} ms of tasks before rendering (scripts under 5 ms each, GC, parsing or browser work)`);
  if (callbacks >= 1) parts.push(`${Math.round(callbacks)} ms in frame callbacks`);
  if (layout >= 1) parts.push(`${Math.round(layout)} ms style, layout & paint`);
  let text = " · no script attribution";
  if (parts.length) {
    const largest = Math.max(before, callbacks, layout);
    text += `: ${parts.join(" · ")}`;
    if (typeof nearbyResource === "function") {
      if (largest === layout) {
        const resource = nearbyResource(start, end, "layout");
        if (resource) text += ` (after ${resource} loaded)`;
      } else if (largest === before) {
        const resource = nearbyResource(start, end, "parse");
        if (resource) text += ` (parsing or compiling ${resource}, which arrived just before)`;
      }
    }
    if (largest < (end - start) / 2) text += ` · ${Math.round(end - start - before - callbacks - layout)} ms unaccounted (GPU or GC)`;
  } else {
    text += " (rendering, GC or browser work)";
  }
  return text;
}

export const WORLD_JSON_TRACE_KEPT = 64;
const jsonRuns = [];

export function installResponseJsonTrace(win = globalThis) {
  const prototype = win?.Response?.prototype;
  if (!prototype || typeof prototype.json !== "function" || prototype.json.__forkmeshJsonTrace) return false;
  const original = prototype.json;
  const clock = () => (win.performance?.now ? win.performance.now() : Date.now());
  const traced = function worldTracedJson(...args) {
    const promise = original.apply(this, args);
    const url = String(this?.url || "");
    const requestedAt = clock();
    try {
      promise.then(() => {
        jsonRuns.push({ at: clock(), url, parseMs: clock() - requestedAt });
        if (jsonRuns.length > WORLD_JSON_TRACE_KEPT) jsonRuns.shift();
      }, () => {});
    } catch {}
    return promise;
  };
  traced.__forkmeshJsonTrace = true;
  try {
    prototype.json = traced;
  } catch {
    return false;
  }
  return true;
}

export function noteJsonRun(url, at, parseMs = 0) {
  jsonRuns.push({ at: Number(at) || 0, url: String(url || ""), parseMs: Number(parseMs) || 0 });
  if (jsonRuns.length > WORLD_JSON_TRACE_KEPT) jsonRuns.shift();
}

function shortUrl(url) {
  const text = String(url || "");
  try {
    const parsed = new URL(text, location.href);
    return `${parsed.host === location.host ? "" : parsed.host}${parsed.pathname}`;
  } catch {
    return text.replace(/^https?:\/\//, "").split("?")[0];
  }
}

export function resolveJsonScript(script) {
  if (!/Response\.json/.test(String(script?.invoker || ""))) return "";
  const startedAt = Number(script.startTime) || 0;
  let best = null;
  for (const run of jsonRuns) {
    const gap = startedAt - run.at;
    if (gap < -2 || gap > 12) continue;
    if (!best || Math.abs(gap) < Math.abs(best.gap)) best = { ...run, gap };
  }
  return best ? shortUrl(best.url) : "";
}

export function describeLongAnimationFrame(entry, { resolveScript = null, nearbyResource = null } = {}) {
  const scripts = Array.isArray(entry?.scripts) ? [...entry.scripts] : [];
  scripts.sort((a, b) => (Number(b.duration) || 0) - (Number(a.duration) || 0));
  const blocking = Number(entry?.blockingDuration);
  const head = `Main thread blocked ${Math.round(Number(entry?.duration) || 0)} ms${Number.isFinite(blocking) ? ` (${Math.round(blocking)} ms blocking)` : ""}`;
  const culprits = scripts.slice(0, 2).map((script) => {
    const resolved = typeof resolveScript === "function" ? resolveScript(script) : "";
    if (resolved) {
      const layout = Number(script.forcedStyleAndLayoutDuration) || 0;
      return `${script.invoker || script.invokerType || "script"} → ${resolved} ${Math.round(Number(script.duration) || 0)} ms${layout >= 1 ? `, forced layout ${Math.round(layout)} ms` : ""}`;
    }
    const file = sourceFile(script.sourceURL);
    const where = file
      ? `${file}${Number(script.sourceCharPosition) >= 0 ? `:${script.sourceCharPosition}` : ""}`
      : "unknown source";
    const layout = Number(script.forcedStyleAndLayoutDuration) || 0;
    const json = resolveJsonScript(script);
    return `${script.invoker || script.invokerType || "script"}${json ? ` (${json})` : ""} → ${script.sourceFunctionName || "anonymous"} @ ${where} ${Math.round(Number(script.duration) || 0)} ms${layout >= 1 ? `, forced layout ${Math.round(layout)} ms` : ""}`;
  });
  const render = Number(entry?.renderStart) > 0 && Number(entry?.styleAndLayoutStart) > 0
    ? Math.round(Number(entry.startTime) + Number(entry.duration) - Number(entry.styleAndLayoutStart))
    : 0;
  return {
    text: culprits.length
      ? `${head} · ${culprits.join(" · ")}${render >= 10 ? ` · style/layout ${render} ms` : ""}`
      : `${head}${unattributedFrameText(entry, nearbyResource)}`,
    culprits: scripts.map((script) => ({
      key: `${script.sourceFunctionName || "anonymous"} @ ${sourceFile(script.sourceURL) || "unknown"}${Number(script.sourceCharPosition) >= 0 ? `:${script.sourceCharPosition}` : ""}${resolveJsonScript(script) ? ` (${resolveJsonScript(script)})` : ""}`,
      invoker: String(script.invoker || script.invokerType || ""),
      ms: Number(script.duration) || 0,
      layoutMs: Number(script.forcedStyleAndLayoutDuration) || 0,
    })),
  };
}

function compactCount(value) {
  const count = Math.max(0, Number(value) || 0);
  return count >= 1000 ? `${(count / 1000).toFixed(1)}k` : String(Math.round(count));
}

export function describeRenderStall(stall) {
  const parts = [`Render frame ${Math.round(stall.frameMs)} ms`];
  parts.push(`draw ${Math.round(stall.renderMs)} ms`);
  if (stall.programs) {
    const names = stall.programNames.length ? ` (${stall.programNames.join(", ")})` : "";
    parts.push(`${stall.programs} program ${stall.programs === 1 ? "compile" : "compiles"}, ${Math.round(stall.compileMs + stall.waitMs)} ms waiting${names}`);
  } else if (stall.waitMs >= 1) {
    parts.push(`${Math.round(stall.waitMs)} ms waiting on programs`);
  }
  if (stall.shadowDraws) {
    parts.push(`shadow map ${stall.shadowDraws} draws ${Math.round(stall.shadowMs)} ms`);
  }
  if (stall.otherDraws) {
    parts.push(`reflection ${stall.otherDraws} draws ${Math.round(stall.otherMs)} ms`);
  }
  if (stall.uploads) {
    const names = stall.uploadNames.length ? ` (${stall.uploadNames.join(", ")})` : "";
    parts.push(`${stall.uploads} texture ${stall.uploads === 1 ? "upload" : "uploads"} ${Math.round(stall.uploadMs)} ms, ${(stall.uploadBytes / 1048576).toFixed(1)} MB${names}`);
  }
  const other = stall.frameMs - stall.renderMs;
  if (other >= 10) {
    const split = (Array.isArray(stall.updateParts) ? stall.updateParts : [])
      .filter((part) => part.ms >= 1)
      .slice(0, 4)
      .map((part) => {
        const walked = part.label === "matrix walk" && Number(stall.matrixVisited) > 0
          ? `, ${compactCount(stall.matrixVisited)} objects walked, ${compactCount(stall.matrixComposed)} recomposed`
          : "";
        return `${part.label} ${Math.round(part.ms)} ms${walked}`;
      });
    parts.push(`scene update ${Math.round(other)} ms${split.length ? ` (${split.join(" · ")})` : ""}`);
  }
  return parts.join(" · ");
}

export function describeFirstSight(entry) {
  const label = entry.material ? `${entry.type} "${entry.material}"` : entry.type;
  const pass = entry.pass === "main" ? "" : ` in the ${entry.pass} pass`;
  return `First-sight compile${pass}: ${label} · ${entry.element || "unregistered"}${entry.object ? ` (${entry.object})` : ""}`;
}

export function observeWorldMainThread(log, { onLongFrame = () => {} } = {}) {
  const disposers = [];
  const onError = (event) => {
    const where = event?.filename
      ? ` @ ${sourceFile(event.filename)}:${event.lineno || 0}`
      : "";
    log.add("ERROR", `${worldDebugLogText(event?.error || event?.message || "Script error")}${where}`);
  };
  const onRejection = (event) => {
    log.add("ERROR", `Unhandled rejection: ${worldDebugLogText(event?.reason ?? "unknown")}`);
  };
  window.addEventListener("error", onError);
  window.addEventListener("unhandledrejection", onRejection);
  disposers.push(() => {
    window.removeEventListener("error", onError);
    window.removeEventListener("unhandledrejection", onRejection);
  });

  let writing = false;
  for (const [method, category] of [["error", "ERROR"], ["warn", "WARN"]]) {
    const original = console[method];
    if (typeof original !== "function") continue;
    const hooked = function worldDebugConsoleHook(...args) {
      if (!writing) {
        writing = true;
        try {
          log.add(category, args.map((arg) => worldDebugLogText(arg, 200)).join(" "));
        } finally {
          writing = false;
        }
      }
      return original.apply(this, args);
    };
    console[method] = hooked;
    disposers.push(() => {
      if (console[method] === hooked) console[method] = original;
    });
  }

  const supported = globalThis.PerformanceObserver?.supportedEntryTypes || [];
  const observe = (type, callback) => {
    if (!supported.includes(type)) return false;
    try {
      const observer = new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) callback(entry);
      });
      observer.observe({ type, buffered: false });
      disposers.push(() => observer.disconnect());
      return true;
    } catch {
      return false;
    }
  };
  const loaf = observe("long-animation-frame", (entry) => {
    const description = describeLongAnimationFrame(entry);
    log.add("LONG", description.text);
    onLongFrame(entry, description);
  });
  if (!loaf) {
    observe("longtask", (entry) => {
      log.add(
        "LONG",
        `Main thread blocked ${Math.round(entry.duration)} ms (long task; this browser gives no script attribution)`,
      );
      onLongFrame(entry, { text: "", culprits: [] });
    });
  }
  observe("resource", (entry) => {
    const status = Number(entry.responseStatus);
    if (!(status >= 400)) return;
    let path = entry.name;
    try {
      const url = new URL(entry.name);
      path = `${url.host === location.host ? "" : url.host}${url.pathname}`;
    } catch {
      path = String(entry.name);
    }
    log.add("NET", `HTTP ${status} ${entry.initiatorType || "fetch"} ${path} in ${Math.round(entry.duration)} ms`);
  });

  return {
    longFrameAttribution: loaf ? "long-animation-frame" : supported.includes("longtask") ? "longtask" : "none",
    dispose() {
      while (disposers.length) disposers.pop()();
    },
  };
}

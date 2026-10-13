// Analytics package: the session replay player (window.ForkMeshReplayPlayer).
// tools/sync_assets.py prepends this file to src/page.ui.js to make page.js.
//
// A recording is hostile input: whoever holds a project's public write key can
// upload one. It is rebuilt inside an iframe sandboxed WITHOUT allow-scripts,
// so nothing in it can run; script bodies, event-handler attributes,
// javascript: URLs, meta refreshes, <base>, and embedded frames are dropped
// on top of that, and the frame never receives pointer events.
/*
 * Session replay event format -- produced by the recorder below and consumed
 * by content/web/src/replay-player.js. Keep this block identical in both.
 *
 *   Every event is {t, k, d}: t = epoch ms, k = kind, d = data.
 *
 *   meta    {href, width, height}     URL + viewport; emitted right before each full
 *   full    {node, scroll: {x, y}}    node = serialized Document, window scroll
 *   mut     {removes: [{id}],
 *            adds:    [{parentId, nextId, node}],   nextId null = append
 *            attrs:   [{id, attrs: {name: value | null}}],   null = removed
 *            texts:   [{id, text}],
 *            css:     [{id, css}]}    stylesheet text of a style/link node
 *           (every array optional; apply in the order listed; adds are
 *            ordered so a nextId is always known when its add is applied)
 *   move    {positions: [[x, y, timeOffset], ...]}   client px; a sample's
 *           time is t + timeOffset, t being the first sample of the batch
 *   mouse   {type, id, x, y}   type: click dblclick contextmenu mousedown
 *           mouseup touchstart touchend focus blur (focus/blur: no x, y)
 *   scroll  {id, x, y}         id of the Document node = window scroll
 *   resize  {width, height}
 *   input   {id, value?, checked?}    value is already masked
 *   custom  {name, url?, message?}    analytics event marker for the timeline
 *
 *   Serialized node, by type:
 *     doc      {id, type, children, adopted?: [cssText]}
 *     doctype  {id, type, name, publicId, systemId}
 *     element  {id, type, tag, attrs, children, svg?: 1, css?: string,
 *               block?: {width, height, inline}, scroll?: [left, top]}
 *              css set: a style/link whose rules are inlined (no children);
 *              block set: a data-fm-block placeholder (no attrs, no children)
 *     text     {id, type, text}
 *     comment  {id, type, text: ""}
 *
 *   Ids are only meaningful until the next full snapshot, which rebuilds the
 *   whole tree. Events are NOT guaranteed sorted by t (a move batch carries
 *   the time of its first sample); consumers stable-sort by t.
 */
(() => {
  "use strict";
  if (window.ForkMeshReplayPlayer) return;

  const SVG_NS = "http://www.w3.org/2000/svg";
  const XLINK_NS = "http://www.w3.org/1999/xlink";
  const IDLE_GAP_MS = 5000;           // no interaction this long = inactivity
  const IDLE_LEAD_MS = 600;           // resume this long before activity returns
  const MAX_EVENTS = 400000;
  const ACTIVE_KINDS = new Set(["move", "mouse", "scroll", "input", "resize", "full"]);
  // Elements that would act on the replay document or load active content.
  const INERT_TAGS = new Set(["script", "noscript", "base", "template"]);
  const FRAME_TAGS = new Set(["iframe", "object", "embed", "frame", "frameset", "portal"]);
  const URL_ATTRS = new Set(["src", "href", "poster", "action", "formaction", "data", "background", "xlink:href"]);
  const CURSOR_SVG = '<svg width="18" height="22" viewBox="0 0 18 22" aria-hidden="true"><path d="M1 1l6.2 19 2.6-7.6L17 10z" fill="#111" stroke="#fff" stroke-width="1.5" stroke-linejoin="round"/></svg>';

  // ------------------------------------------------------------ decoding

  // One stored chunk -> an array of {t, k, d}. Anything malformed yields [].
  async function decodeChunk(chunk) {
    try {
      let text = "";
      if (chunk && chunk.encoding === "gzip-b64") {
        if (typeof DecompressionStream !== "function") throw new Error("gzip_unsupported");
        const binary = atob(String(chunk.events || ""));
        const bytes = new Uint8Array(binary.length);
        for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
        const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
        text = await new Response(stream).text();
      } else text = String(chunk && chunk.events || "");
      const events = JSON.parse(text);
      return Array.isArray(events) ? events.filter((event) => event && typeof event === "object"
        && Number.isFinite(Number(event.t)) && typeof event.k === "string") : [];
    } catch (error) {
      if (error && error.message === "gzip_unsupported") throw error;
      return [];
    }
  }

  // Stable sort by time, with each move batch expanded into cursor samples.
  function normalize(events) {
    const out = [];
    events.forEach((event, order) => {
      const t = Number(event.t);
      if (event.k === "move") {
        const positions = Array.isArray(event.d && event.d.positions) ? event.d.positions : [];
        for (const sample of positions.slice(0, 500)) {
          if (!Array.isArray(sample)) continue;
          out.push({ t: t + (Number(sample[2]) || 0), k: "move", d: { x: Number(sample[0]) || 0, y: Number(sample[1]) || 0 }, o: order });
        }
      } else out.push({ t, k: event.k, d: event.d || {}, o: order });
    });
    out.sort((a, b) => a.t - b.t || a.o - b.o);
    return out.slice(0, MAX_EVENTS);
  }

  // ------------------------------------------------------------ sanitizing

  function safeUrl(value) {
    const text = String(value == null ? "" : value).trim();
    if (/^(?:javascript|vbscript|data:text\/html)/i.test(text.replace(/[\s\u0000-\u001f]/g, ""))) return null;
    return text;
  }
  // undefined = do not set.
  function cleanAttr(tag, name, value) {
    const lower = name.toLowerCase();
    if (!/^[a-zA-Z_:][-a-zA-Z0-9_:.]*$/.test(name)) return undefined;
    if (lower.startsWith("on") || lower === "srcdoc" || lower === "nonce" || lower === "integrity") return undefined;
    if (lower === "autofocus" || lower === "autoplay" || lower === "ping" || lower === "target") return undefined;
    if (tag === "meta" && (lower === "http-equiv" || lower === "content")) return undefined;
    if (tag === "link" && lower === "rel" && !/^\s*(?:stylesheet|icon)\s*$/i.test(String(value))) return undefined;
    if (URL_ATTRS.has(lower)) { const url = safeUrl(value); return url == null ? undefined : url; }
    return String(value == null ? "" : value).slice(0, 20000);
  }
  function setAttr(element, tag, name, value) {
    try {
      if (value === null) { element.removeAttribute(name); return; }
      const clean = cleanAttr(tag, name, value);
      if (clean === undefined) return;
      if (name === "xlink:href") element.setAttributeNS(XLINK_NS, name, clean);
      else element.setAttribute(name, clean);
      if (name === "value" && "value" in element) element.value = clean;
      if (name === "checked" && "checked" in element) element.checked = true;
    } catch (_) { /* an attribute name the DOM refuses */ }
  }

  // ------------------------------------------------------------ player

  // create(host, {onTime(ms), onState(playing), onNote(text), onReady()}) -> player
  function create(host, callbacks = {}) {
    const frameBox = document.createElement("div");
    frameBox.style.cssText = "position:relative;margin:0 auto;overflow:hidden;transform-origin:0 0";
    const frame = document.createElement("iframe");
    frame.setAttribute("sandbox", "allow-same-origin");
    frame.setAttribute("referrerpolicy", "no-referrer");
    frame.setAttribute("title", "Session replay");
    frame.setAttribute("tabindex", "-1");
    frame.style.cssText = "position:absolute;left:0;top:0;border:0;background:#fff;pointer-events:none;transform-origin:0 0";
    const overlay = document.createElement("div");
    overlay.style.cssText = "position:absolute;left:0;top:0;pointer-events:none;transform-origin:0 0";
    const cursor = document.createElement("div");
    cursor.style.cssText = "position:absolute;left:0;top:0;width:18px;height:22px;display:none;will-change:transform";
    cursor.innerHTML = CURSOR_SVG;
    overlay.appendChild(cursor);
    frameBox.append(frame, overlay);
    host.textContent = "";
    host.appendChild(frameBox);

    let events = [], idle = [], marks = [];
    let start = 0, end = 0, position = 0, index = 0;
    let playing = false, speed = 1, skipIdle = true, raf = 0, lastTick = 0;
    let nodes = new Map(), docId = -1;
    let viewport = { width: 1280, height: 720 }, destroyed = false;
    const pending = new Map();      // node -> [left, top] scroll to apply once laid out

    const observer = typeof ResizeObserver === "function" ? new ResizeObserver(() => fit()) : null;
    observer?.observe(host);

    function note(text) { callbacks.onNote?.(text || ""); }
    function doc() { return frame.contentDocument; }

    function fit() {
      const width = Math.max(1, viewport.width), height = Math.max(1, viewport.height);
      const available = Math.max(200, host.clientWidth || 640);
      const tallest = Math.max(240, Math.round(window.innerHeight * 0.62));
      const scale = Math.min(1, available / width, tallest / height);
      frame.style.width = `${width}px`; frame.style.height = `${height}px`;
      frame.style.transform = `scale(${scale})`;
      overlay.style.width = `${width}px`; overlay.style.height = `${height}px`;
      overlay.style.transform = `scale(${scale})`;
      frameBox.style.width = `${Math.round(width * scale)}px`;
      frameBox.style.height = `${Math.round(height * scale)}px`;
    }

    // -- building

    function build(node, target, inSvg) {
      if (!node || typeof node !== "object") return null;
      let built = null;
      if (node.type === "text") built = target.createTextNode(String(node.text || ""));
      else if (node.type === "comment") built = target.createComment("");
      else if (node.type === "element") {
        const tag = String(node.tag || "div");
        const lower = tag.toLowerCase();
        const svg = Boolean(node.svg) || inSvg;
        if (node.block) {
          built = target.createElement("div");
          const block = node.block;
          built.style.cssText = `display:${block.inline ? "inline-block" : "block"};width:${Number(block.width) || 0}px;height:${Number(block.height) || 0}px;background:repeating-linear-gradient(135deg,#d0d7de 0 6px,#eef1f4 6px 12px)`;
        } else if (typeof node.css === "string") {
          built = target.createElement("style");
          built.textContent = node.css;
        } else if (INERT_TAGS.has(lower) || (lower === "meta" && node.attrs && "http-equiv" in node.attrs)) {
          built = target.createElement("template");      // never rendered, never runs
        } else if (FRAME_TAGS.has(lower)) {
          built = target.createElement("div");
          const attrs = node.attrs || {};
          const width = /^\d+%?$/.test(String(attrs.width || "")) ? attrs.width : "300";
          const height = /^\d+%?$/.test(String(attrs.height || "")) ? attrs.height : "150";
          built.style.cssText = `display:inline-block;width:${/%$/.test(width) ? width : `${width}px`};height:${/%$/.test(height) ? height : `${height}px`};background:#e5e7eb;border:1px dashed #9ca3af`;
          if (attrs.class) setAttr(built, "div", "class", attrs.class);
          if (attrs.style) built.style.cssText += `;${String(attrs.style).slice(0, 2000)}`;
        } else {
          try {
            built = svg ? target.createElementNS(SVG_NS, tag) : target.createElement(tag);
          } catch (_) { built = target.createElement("span"); }
          for (const [name, value] of Object.entries(node.attrs || {})) setAttr(built, lower, name, value);
          const inner = svg && lower !== "foreignobject";
          for (const child of Array.isArray(node.children) ? node.children : []) {
            const item = build(child, target, inner);
            if (item) { try { built.appendChild(item); } catch (_) { /* not allowed here */ } }
          }
          if (Array.isArray(node.scroll)) pending.set(built, node.scroll);
        }
      }
      if (built && Number.isFinite(Number(node.id))) nodes.set(Number(node.id), built);
      return built;
    }

    function rebuild(full) {
      const root = full && full.node;
      nodes = new Map(); pending.clear();
      const target = doc();
      if (!target || !root || root.type !== "doc") return false;
      const hasDoctype = (root.children || []).some((child) => child && child.type === "doctype");
      target.open(); target.write(hasDoctype ? "<!DOCTYPE html><html></html>" : "<html></html>"); target.close();
      if (target.documentElement) target.documentElement.remove();
      docId = Number(root.id);
      nodes.set(docId, target);
      for (const child of root.children || []) {
        if (!child || child.type === "doctype") continue;
        const item = build(child, target, false);
        if (item && item.nodeType === 1 && !target.documentElement) target.appendChild(item);
      }
      if (Array.isArray(root.adopted) && target.head) {
        for (const css of root.adopted) { const style = target.createElement("style"); style.textContent = String(css || ""); target.head.appendChild(style); }
      }
      pending.forEach((scroll, element) => { try { element.scrollLeft = scroll[0]; element.scrollTop = scroll[1]; } catch (_) { /* detached */ } });
      pending.clear();
      const scroll = full.scroll || {};
      try { frame.contentWindow.scrollTo(Number(scroll.x) || 0, Number(scroll.y) || 0); } catch (_) { /* not ready */ }
      return true;
    }

    function applyMutation(data) {
      const target = doc(); if (!target) return;
      for (const item of data.removes || []) {
        const node = nodes.get(Number(item.id));
        if (node && node !== target) { try { node.remove(); } catch (_) { /* gone */ } nodes.delete(Number(item.id)); }
      }
      for (const item of data.adds || []) {
        const parent = nodes.get(Number(item.parentId)); if (!parent) continue;
        const inSvg = parent.namespaceURI === SVG_NS && parent.localName !== "foreignObject";
        const built = build(item.node, target, inSvg); if (!built) continue;
        const next = item.nextId == null ? null : nodes.get(Number(item.nextId)) || null;
        try {
          if (parent === target) {
            if (built.nodeType === 1) { target.documentElement?.remove(); target.appendChild(built); }
          } else parent.insertBefore(built, next && next.parentNode === parent ? next : null);
        } catch (_) { /* a structure the DOM refuses */ }
        pending.forEach((scroll, element) => { try { element.scrollLeft = scroll[0]; element.scrollTop = scroll[1]; } catch (_) { /* detached */ } });
        pending.clear();
      }
      for (const item of data.attrs || []) {
        const node = nodes.get(Number(item.id)); if (!node || node.nodeType !== 1) continue;
        const tag = node.localName;
        if (tag === "template") continue;
        for (const [name, value] of Object.entries(item.attrs || {})) setAttr(node, tag, name, value);
      }
      for (const item of data.texts || []) {
        const node = nodes.get(Number(item.id));
        if (node && node.nodeType === 3) node.nodeValue = String(item.text || "");
      }
      for (const item of data.css || []) {
        const node = nodes.get(Number(item.id));
        if (node && node.localName === "style") node.textContent = String(item.css || "");
      }
    }

    function showCursor(x, y) {
      cursor.style.display = "block";
      cursor.style.transform = `translate(${Math.round(x)}px, ${Math.round(y)}px)`;
    }
    function ripple(x, y) {
      const ring = document.createElement("div");
      ring.style.cssText = `position:absolute;left:${Math.round(x) - 14}px;top:${Math.round(y) - 14}px;width:28px;height:28px;border-radius:50%;border:3px solid #ef4444;opacity:.9;transition:transform .45s ease-out,opacity .45s ease-out`;
      overlay.appendChild(ring);
      requestAnimationFrame(() => { ring.style.transform = "scale(1.8)"; ring.style.opacity = "0"; });
      setTimeout(() => ring.remove(), 500);
    }

    // visual = false while fast-forwarding during a seek: DOM only, no effects.
    function apply(event, visual) {
      const data = event.d || {};
      switch (event.k) {
        case "meta":
          viewport = { width: Number(data.width) || viewport.width, height: Number(data.height) || viewport.height };
          fit();
          break;
        case "full": rebuild(data); break;
        case "mut": applyMutation(data); break;
        case "resize":
          viewport = { width: Number(data.width) || viewport.width, height: Number(data.height) || viewport.height };
          fit();
          break;
        case "scroll": {
          const node = nodes.get(Number(data.id));
          try {
            if (Number(data.id) === docId) frame.contentWindow.scrollTo(Number(data.x) || 0, Number(data.y) || 0);
            else if (node && node.nodeType === 1) { node.scrollLeft = Number(data.x) || 0; node.scrollTop = Number(data.y) || 0; }
          } catch (_) { /* detached */ }
          break;
        }
        case "input": {
          const node = nodes.get(Number(data.id));
          if (node && node.nodeType === 1) {
            if ("checked" in data && "checked" in node) node.checked = Boolean(data.checked);
            if ("value" in data && "value" in node) node.value = String(data.value == null ? "" : data.value);
          }
          break;
        }
        case "move": showCursor(data.x, data.y); break;
        case "mouse":
          if (Number.isFinite(Number(data.x)) && data.type !== "focus" && data.type !== "blur") {
            showCursor(Number(data.x), Number(data.y));
            if (visual && (data.type === "click" || data.type === "dblclick" || data.type === "touchstart")) ripple(Number(data.x), Number(data.y));
          }
          break;
        default: break;
      }
    }

    // -- timeline

    // The timeline starts at the first page snapshot: nothing earlier can be
    // drawn. seek() still applies the meta recorded just before it.
    function analyse() {
      const first = events.findIndex((event) => event.k === "full");
      start = events.length ? events[Math.max(0, first)].t : 0;
      end = events.length ? events[events.length - 1].t : 0;
      idle = []; marks = [];
      let lastActive = start;
      for (const event of events) {
        if (event.t < start) continue;
        if (ACTIVE_KINDS.has(event.k)) {
          if (event.t - lastActive > IDLE_GAP_MS) idle.push([lastActive - start, event.t - start]);
          lastActive = event.t;
        }
        if (event.k === "mouse" && event.d && event.d.type === "click") marks.push({ at: event.t - start, kind: "click", label: "Click" });
        else if (event.k === "meta") marks.push({ at: event.t - start, kind: "navigation", label: String(event.d && event.d.href || "Page") });
        else if (event.k === "custom" && event.d) {
          const name = String(event.d.name || "");
          if (name === "$exception") marks.push({ at: event.t - start, kind: "error", label: String(event.d.message || "Exception") });
          else if (name === "$pageview") marks.push({ at: event.t - start, kind: "pageview", label: String(event.d.url || "Pageview") });
        }
      }
      if (end - lastActive > IDLE_GAP_MS) idle.push([lastActive - start, end - start]);
    }

    function lastFullBefore(offset) {
      let found = -1;
      for (let at = 0; at < events.length && events[at].t - start <= offset; at += 1) if (events[at].k === "full") found = at;
      return found;
    }

    // Rebuild from the last full snapshot at or before `offset`, then replay
    // the DOM up to it without effects.
    function seek(offset) {
      offset = Math.max(0, Math.min(end - start, Number(offset) || 0));
      const first = lastFullBefore(offset);
      if (first < 0) { position = offset; index = 0; note("Waiting for the first page snapshot…"); callbacks.onTime?.(position); return; }
      let from = first;
      // The meta that precedes a full carries the viewport of that snapshot.
      while (from > 0 && events[from - 1].k === "meta" && events[from - 1].t >= events[first].t - 1000) from -= 1;
      cursor.style.display = "none";
      for (index = from; index < events.length && events[index].t - start <= offset; index += 1) apply(events[index], false);
      position = offset;
      note("");
      callbacks.onTime?.(position);
    }

    function idleEndAt(offset) {
      for (const [from, to] of idle) if (offset >= from && offset < to - IDLE_LEAD_MS) return to - IDLE_LEAD_MS;
      return -1;
    }

    function tick(now) {
      if (!playing || destroyed) return;
      const elapsed = Math.min(1000, now - lastTick); lastTick = now;
      let next = position + elapsed * speed;
      if (skipIdle) { const jump = idleEndAt(next); if (jump > next) next = jump; }
      const target = Math.min(end - start, next);
      while (index < events.length && events[index].t - start <= target) {
        apply(events[index], true);
        index += 1;
      }
      position = target;
      callbacks.onTime?.(position);
      if (position >= end - start) { pause(); return; }
      raf = requestAnimationFrame(tick);
    }

    function play() {
      if (destroyed || !events.length) return;
      if (position >= end - start) seek(0);
      playing = true; lastTick = performance.now();
      callbacks.onState?.(true);
      cancelAnimationFrame(raf); raf = requestAnimationFrame(tick);
    }
    function pause() {
      playing = false; cancelAnimationFrame(raf);
      callbacks.onState?.(false);
    }

    function load(raw) {
      events = normalize(raw || []);
      analyse();
      const first = events.find((event) => event.k === "meta");
      if (first) viewport = { width: Number(first.d.width) || 1280, height: Number(first.d.height) || 720 };
      fit();
      if (!events.some((event) => event.k === "full")) { note("This recording has no page snapshot yet, so there is nothing to show."); return false; }
      seek(0);
      return true;
    }

    return {
      load, play, pause, seek,
      toggle() { if (playing) pause(); else play(); },
      setSpeed(value) { speed = Math.max(0.25, Math.min(16, Number(value) || 1)); },
      setSkipInactivity(value) { skipIdle = Boolean(value); },
      get playing() { return playing; },
      get duration() { return Math.max(0, end - start); },
      get position() { return position; },
      get start() { return start; },
      get idle() { return idle.slice(); },
      get marks() { return marks.slice(); },
      destroy() {
        destroyed = true; pause(); observer?.disconnect();
        nodes = new Map(); events = [];
        host.textContent = "";
      },
    };
  }

  window.ForkMeshReplayPlayer = Object.freeze({ create, decodeChunk });
})();

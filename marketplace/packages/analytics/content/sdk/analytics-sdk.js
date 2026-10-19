/*
 * ForkMesh Analytics browser SDK. Dependency-free; exposes
 * window.ForkMeshAnalyticsSDK = { init(config) -> instance, version }.
 * The wire contract (capture / decide payloads, reserved names) is the
 * package README.
 *
 * Session replay event format -- produced by the recorder below and consumed
 * by content/web/src/replay-player.js. Keep this block identical in both.
 *
 *   Every event is {t, k, d}: t = epoch ms, k = kind, d = data.
 *
 *   meta    {href, width, height, pageHeight}   URL + viewport + document
 *           height; emitted right before each full
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
 *   scroll  {id, x, y, h?}     id of the Document node = window scroll;
 *           h = document height, set on window scrolls only
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
  if (typeof window === "undefined" || typeof document === "undefined") return;
  if (window.ForkMeshAnalyticsSDK && window.ForkMeshAnalyticsSDK.init) return;

  const VERSION = "1.0.0";
  const win = window, doc = document, nav = navigator;
  const SVG_NS = "http://www.w3.org/2000/svg";
  const CAPTURE_PATH = "/api/analytics/_/capture";
  const DECIDE_PATH = "/api/analytics/_/decide";
  const FLUSH_MS = 10000, FLUSH_AT = 40, MAX_BATCH = 100, MAX_QUEUE = 1000;
  const MAX_PERSISTED = 200, BEACON_BUDGET = 60000, KEEPALIVE_MAX = 50000;
  const SESSION_IDLE_MS = 30 * 60000, SESSION_MAX_MS = 24 * 3600000;
  const DECIDE_TTL_MS = 5 * 60000;
  // The server keeps at most 60 items of any list property, so a larger
  // $heatmap_points array would be silently cut there.
  const HEAT_POINTS_PER_EVENT = 60, HEAT_BUFFER_MAX = 600;
  const REPLAY_CHUNK_MAX = 600 * 1024, REPLAY_MEMORY_MAX = 4 * 1024 * 1024;
  const REPLAY_IDLE_MS = 5 * 60000, ACTIVE_GAP_MS = 5000;
  const SCRIPT_ORIGIN = (() => {
    try { return new URL(doc.currentScript.src).origin; } catch (_) { return location.origin; }
  })();

  // ---------------------------------------------------------------- helpers

  const now = () => Date.now();
  // Nothing in here may ever throw into the host page.
  const guard = (fn) => function () {
    try { return fn.apply(this, arguments); } catch (_) { return undefined; }
  };
  const later = (fn, ms) => setTimeout(guard(fn), ms);
  const byteLength = (text) => {
    try { return new Blob([text]).size; } catch (_) { return text.length * 3; }
  };

  function randomHex(bytes) {
    const data = new Uint8Array(bytes);
    try { crypto.getRandomValues(data); } catch (_) {
      for (let i = 0; i < bytes; i++) data[i] = Math.floor(Math.random() * 256);
    }
    return Array.from(data, (b) => b.toString(16).padStart(2, "0")).join("");
  }

  // UUIDv7 layout: time-ordered, so ids sort by creation.
  function uuid7() {
    const t = now().toString(16).padStart(12, "0");
    const r = randomHex(10);
    const variant = (8 + (parseInt(r[3], 16) & 3)).toString(16);
    return `${t.slice(0, 8)}-${t.slice(8, 12)}-7${r.slice(0, 3)}-${variant}${r.slice(4, 7)}-${r.slice(7, 19)}`;
  }

  function hash32(text) {
    let h = 0x811c9dc5;
    for (let i = 0; i < text.length; i++) {
      h ^= text.charCodeAt(i);
      h = Math.imul(h, 0x01000193);
    }
    return h >>> 0;
  }

  function cleanUrl(href, mask) {
    try {
      const url = new URL(href, location.href);
      if (mask) { url.search = ""; url.hash = ""; }
      return url.href;
    } catch (_) { return ""; }
  }

  const hostOf = (href) => { try { return new URL(href).host; } catch (_) { return ""; } };

  function privacySignal() {
    const dnt = String(nav.doNotTrack || win.doNotTrack || nav.msDoNotTrack || "").toLowerCase();
    return nav.globalPrivacyControl === true || dnt === "1" || dnt === "yes";
  }

  function isBot() {
    return nav.webdriver === true ||
      /bot\b|crawl|spider|slurp|headless|lighthouse|pingdom|phantomjs|puppeteer|playwright|preview|monitor|scrape|httpclient|python-requests|curl\//i
        .test(nav.userAgent || "");
  }

  function parseUA(ua) {
    const pick = (re) => { const m = ua.match(re); return m ? m[1] : ""; };
    let browser = "Other", version = "";
    const table = [
      ["Edge", /Edg(?:e|A|iOS)?\/([\d.]+)/], ["Opera", /(?:OPR|Opera)\/([\d.]+)/],
      ["Samsung Internet", /SamsungBrowser\/([\d.]+)/], ["Firefox", /(?:Firefox|FxiOS)\/([\d.]+)/],
      ["Chrome", /(?:Chrome|CriOS)\/([\d.]+)/], ["Safari", /Version\/([\d.]+).*Safari/],
    ];
    for (const [name, re] of table) {
      const found = pick(re);
      if (found) { browser = name; version = found; break; }
    }
    const touchMac = /Macintosh/.test(ua) && nav.maxTouchPoints > 1;   // iPadOS poses as a Mac
    let os = "Other";
    if (/Windows/.test(ua)) os = "Windows";
    else if (/iPhone|iPad|iPod/.test(ua) || touchMac) os = "iOS";
    else if (/Android/.test(ua)) os = "Android";
    else if (/Mac OS X|Macintosh/.test(ua)) os = "macOS";
    else if (/CrOS/.test(ua)) os = "ChromeOS";
    else if (/Linux|X11/.test(ua)) os = "Linux";
    let type = "Desktop";
    if (/iPad|Tablet|PlayBook|Silk/.test(ua) || touchMac || (/Android/.test(ua) && !/Mobile/.test(ua))) type = "Tablet";
    else if (/Mobi|iPhone|iPod|Android/.test(ua)) type = "Mobile";
    return { browser, version: version.split(".")[0] || "", os, type };
  }

  // Per-key storage under fma.<key>.*; falls back to memory whenever the
  // browser refuses storage, and never touches it in "memory" mode.
  function makeStore(key, persistent) {
    const prefix = `fma.${key}.`;
    const memory = new Map();
    const area = (session) => {
      if (!persistent) return null;
      try { return session ? win.sessionStorage : win.localStorage; } catch (_) { return null; }
    };
    return {
      get(name, session) {
        try {
          if (memory.has(name)) return JSON.parse(memory.get(name));
          const a = area(session);
          const raw = a ? a.getItem(prefix + name) : null;
          return raw == null ? null : JSON.parse(raw);
        } catch (_) { return null; }
      },
      set(name, value, session) {
        const raw = JSON.stringify(value);
        try {
          const a = area(session);
          if (a) { a.setItem(prefix + name, raw); memory.delete(name); return true; }
        } catch (_) { /* quota or blocked: keep it in memory */ }
        memory.set(name, raw);
        return false;
      },
      remove(name, session) {
        memory.delete(name);
        try { const a = area(session); if (a) a.removeItem(prefix + name); } catch (_) { /* ignore */ }
      },
    };
  }

  // ------------------------------------------------------- session recorder

  const maskText = (text) => text.replace(/[^\s]/g, "*");
  const URL_ATTRS = { src: 1, href: 1, poster: 1, action: 1, formaction: 1, data: 1, background: 1, "xlink:href": 1 };
  const DROPPED_ATTRS = { integrity: 1, nonce: 1, autoplay: 1, srcdoc: 1 };
  const OPAQUE_TAGS = { script: 1, noscript: 1, canvas: 1, iframe: 1, object: 1, embed: 1 };
  const MASKED_CONTEXT = '[data-fm-mask],[contenteditable]:not([contenteditable="false"])';

  function absoluteUrl(value, base) {
    const text = String(value).trim();
    if (/^javascript:/i.test(text)) return null;
    if (!text || /^(data:|blob:|about:|mailto:|tel:|#)/i.test(text)) {
      return text.length > 200000 ? "" : text;
    }
    try { return new URL(text, base || doc.baseURI).href; } catch (_) { return text; }
  }

  function absoluteSrcset(value) {
    return String(value).split(",").map((part) => {
      const bits = part.trim().split(/\s+/);
      bits[0] = absoluteUrl(bits[0]) || "";
      return bits.join(" ");
    }).join(", ");
  }

  // Inlined rules lose their stylesheet's base URL, so relative url()s are
  // rewritten against it.
  function absoluteCss(css, base) {
    return String(css).replace(/url\(\s*(['"]?)([^'")]+)\1\s*\)/g, (match, quote, url) => {
      if (/^(data:|blob:|#|[a-z][a-z0-9+.-]*:)/i.test(url)) return match;
      try { return `url(${quote}${new URL(url, base).href}${quote})`; } catch (_) { return match; }
    });
  }

  // null when the rules are unreadable (a cross-origin sheet).
  function sheetCss(sheet, depth) {
    let rules;
    try { rules = sheet.cssRules; } catch (_) { return null; }
    if (!rules) return null;
    let out = "";
    for (let i = 0; i < rules.length; i++) {
      const rule = rules[i];
      const inner = rule.styleSheet && depth < 3 ? sheetCss(rule.styleSheet, depth + 1) : null;
      out += inner != null ? inner : rule.cssText + "\n";
    }
    return absoluteCss(out, sheet.href || doc.baseURI);
  }

  function sensitiveInput(el) {
    const type = (el.getAttribute("type") || "").toLowerCase();
    const hints = `${el.getAttribute("autocomplete") || ""} ${el.getAttribute("name") || ""} ${el.id || ""}`;
    return type === "password" || type === "hidden" || type === "email" || type === "tel" ||
      type === "number" || /cc-|card|cvv|cvc|ssn|passw|secret|token/i.test(hints);
  }

  // Everything typed is masked; data-fm-unmask opts a harmless field out.
  function inputValue(el, value) {
    const text = String(value == null ? "" : value).slice(0, 1000);
    return el.hasAttribute("data-fm-unmask") && !sensitiveInput(el) ? text : text.replace(/[\s\S]/g, "*");
  }

  function createRecorder(host) {
    let ids = new WeakMap(), blocked = new WeakSet(), opaque = new WeakSet(), nextId = 1;
    let buf = [], jobs = [], chain = Promise.resolve();
    const ready = [];
    const newStats = () => ({ activeMs: 0, clicks: 0, keys: 0 });
    let stats = newStats(), lastActivity = 0, running = false, paused = false, dead = false;
    let sid = "", t0 = 0, begunAt = 0, observer = null, offs = [], patches = [];
    let moves = [], lastMove = 0, moveTimer = 0, inputTimer = 0, cssTimer = 0, resizeTimer = 0, lockCheck = 0;
    let settings = {}, minDuration = 0;
    const inputs = new Set(), scrolls = new Map(), cssDirty = new Set();

    const assign = (node) => { const id = nextId++; ids.set(node, id); return id; };
    const maskedContext = (el) => !!(el && el.closest && el.closest(MASKED_CONTEXT));

    function emit(kind, data, t) {
      if (!running || paused || dead) return;
      buf.push({ t: t || now(), k: kind, d: data });
      if (buf.length > 8000) host.flushSoon(0); else host.requestFlush();
    }

    // One tab records a session at a time: two tabs interleaving node ids in
    // one recording could not be replayed. The lock also carries seq so a
    // takeover continues the numbering.
    function ownsLock() {
      const lock = host.store.get("rlock"), t = now();
      const same = lock && lock.sid === sid;
      if (same && lock.win !== host.windowId && t - lock.t < 30000) return false;
      host.store.set("rlock", { sid, win: host.windowId, t, seq: same ? lock.seq : 0 });
      return true;
    }

    function takeSeq(session) {
      const mine = host.store.get("rseq", true), lock = host.store.get("rlock");
      let seq = mine && mine.sid === session ? mine.seq : 0;
      if (lock && lock.sid === session && lock.seq > seq) seq = lock.seq;
      host.store.set("rseq", { sid: session, seq: seq + 1, t0 }, true);
      if (session === sid) host.store.set("rlock", { sid, win: host.windowId, t: now(), seq: seq + 1 });
      return seq;
    }

    // -- serialization

    // undefined = leave the attribute out.
    function cleanAttr(tag, name, value) {
      if (name.indexOf("on") === 0 || DROPPED_ATTRS[name]) return undefined;
      if (name === "value" && (tag === "input" || tag === "textarea" || tag === "select")) return undefined;
      if (name === "checked" && tag === "input") return undefined;
      if ((name === "src" || name === "data") && OPAQUE_TAGS[tag] && tag !== "canvas") return undefined;
      if (name === "srcset" || name === "imagesrcset") return absoluteSrcset(value);
      if (URL_ATTRS[name]) { const url = absoluteUrl(value); return url == null ? undefined : url; }
      if (name === "style") return absoluteCss(value, doc.baseURI);
      return String(value).slice(0, 20000);
    }

    function formState(el, tag, attrs) {
      const type = (el.getAttribute("type") || "").toLowerCase();
      if (tag === "input" && (type === "checkbox" || type === "radio")) {
        if (el.checked) attrs.checked = "";
      } else if (tag === "input" && /^(submit|button|reset|image)$/.test(type)) {
        if (el.hasAttribute("value")) attrs.value = el.getAttribute("value");   // a label, not typed
      } else if (tag === "select") {
        if (el.hasAttribute("data-fm-unmask")) attrs.value = String(el.value);
      } else if (type !== "hidden" && el.value) {
        attrs.value = inputValue(el, el.value);
      }
    }

    function attrsOf(el, tag) {
      const attrs = {};
      if (tag === "script") return attrs;
      for (let i = 0; i < el.attributes.length; i++) {
        const attr = el.attributes[i];
        const value = cleanAttr(tag, attr.name.toLowerCase(), attr.value);
        if (value !== undefined) attrs[attr.name] = value;
      }
      // A refresh or CSP meta would act on the replay document.
      if (tag === "meta" && el.hasAttribute("http-equiv")) return {};
      if (tag === "input" || tag === "textarea" || tag === "select") formState(el, tag, attrs);
      return attrs;
    }

    function serialize(node, masked) {
      const type = node.nodeType;
      if (type === 3 || type === 4) {
        const parent = node.parentNode;
        let text = node.nodeValue || "";
        const field = parent && parent.nodeName === "TEXTAREA" && !parent.hasAttribute("data-fm-unmask");
        if (masked || settings.maskAllText || field) text = maskText(text);
        return { id: assign(node), type: "text", text };
      }
      if (type === 8) return { id: assign(node), type: "comment", text: "" };
      if (type === 10) {
        return { id: assign(node), type: "doctype", name: node.name, publicId: node.publicId, systemId: node.systemId };
      }
      if (type === 9) {
        const out = { id: assign(node), type: "doc", children: children(node, false) };
        try {
          const adopted = (node.adoptedStyleSheets || []).map((sheet) => sheetCss(sheet, 0)).filter(Boolean);
          if (adopted.length) out.adopted = adopted;
        } catch (_) { /* optional */ }
        return out;
      }
      if (type !== 1) return null;
      const svg = node.namespaceURI === SVG_NS;
      const tag = svg ? node.tagName : node.tagName.toLowerCase();
      const id = assign(node);
      if (node.hasAttribute("data-fm-block")) {
        blocked.add(node);
        const rect = node.getBoundingClientRect();
        let inline = false;
        try { inline = getComputedStyle(node).display.indexOf("inline") === 0; } catch (_) { /* detached */ }
        return { id, type: "element", tag: "div", attrs: {}, children: [],
          block: { width: Math.round(rect.width), height: Math.round(rect.height), inline } };
      }
      const out = { id, type: "element", tag, attrs: attrsOf(node, tag), children: [] };
      if (svg) out.svg = 1;
      if (OPAQUE_TAGS[tag]) { opaque.add(node); return out; }
      if (tag === "style" || (tag === "link" && node.sheet)) {
        let css = node.sheet ? sheetCss(node.sheet, 0) : null;
        if (css == null && tag === "style") css = absoluteCss(node.textContent || "", doc.baseURI);
        if (css != null) {
          // Inlined so the replay needs no network and sees CSSOM-inserted rules.
          out.css = css;
          delete out.attrs.href;
          opaque.add(node);
          return out;
        }
      }
      const inner = masked || node.hasAttribute("data-fm-mask") ||
        (node.hasAttribute("contenteditable") && node.getAttribute("contenteditable") !== "false");
      out.children = children(node, inner);
      if (node.scrollTop || node.scrollLeft) out.scroll = [Math.round(node.scrollLeft), Math.round(node.scrollTop)];
      return out;
    }

    function children(node, masked) {
      const list = [];
      for (let child = node.firstChild; child; child = child.nextSibling) {
        const item = serialize(child, masked);
        if (item) list.push(item);
      }
      return list;
    }

    function pageHeight() {
      const root = doc.documentElement, body = doc.body;
      return Math.round(Math.max(root ? root.scrollHeight : 0, body ? body.scrollHeight : 0, win.innerHeight));
    }

    function snapshot() {
      if (!running || dead) return;
      if (observer) observer.takeRecords();
      ids = new WeakMap(); blocked = new WeakSet(); opaque = new WeakSet();
      inputs.clear(); cssDirty.clear();
      paused = false;
      try {
        emit("meta", { href: host.url(location.href), width: win.innerWidth, height: win.innerHeight, pageHeight: pageHeight() });
        emit("full", { node: serialize(doc, false), scroll: { x: Math.round(win.scrollX), y: Math.round(win.scrollY) } });
      } catch (_) { fail(); }
    }

    // -- incremental: DOM

    function forget(root) {
      const stack = [root];
      while (stack.length) {
        const node = stack.pop();
        ids.delete(node);
        for (let child = node.firstChild; child; child = child.nextSibling) stack.push(child);
      }
    }

    // Works from the DOM's final state rather than replaying each record, so a
    // node added and removed in one batch never appears, and a moved node is a
    // remove plus a freshly serialized add.
    function onMutations(records) {
      if (!running || paused || dead) return;
      const removes = [], attrs = new Map(), texts = new Set(), parents = new Set();
      for (const record of records) {
        const target = record.target;
        if (record.type === "childList") {
          for (const node of record.removedNodes) {
            const id = ids.get(node);
            if (id) { removes.push({ id }); forget(node); }
          }
          if (target.nodeName === "STYLE") markCss(target);
          else if (record.addedNodes.length) parents.add(target);
        } else if (record.type === "attributes") {
          if (!ids.has(target) || blocked.has(target)) continue;
          let names = attrs.get(target);
          if (!names) attrs.set(target, names = {});
          names[record.attributeName] = 1;
        } else if (ids.has(target)) {
          texts.add(target);
        } else if (target.parentNode && target.parentNode.nodeName === "STYLE") {
          markCss(target.parentNode);
        }
      }
      const firstFresh = nextId, adds = [];
      for (const parent of parents) {
        const parentId = ids.get(parent);
        if (!parentId || blocked.has(parent) || opaque.has(parent)) continue;
        const masked = parent.nodeType === 1 && maskedContext(parent);
        // Backwards, so every add's next sibling already has an id.
        let following = null;
        for (let child = parent.lastChild; child; child = child.previousSibling) {
          const known = ids.get(child);
          if (known) { following = known; continue; }
          const node = serialize(child, masked);
          if (!node) continue;
          adds.push({ parentId, nextId: following, node });
          following = node.id;
        }
      }
      const data = {};
      if (removes.length) data.removes = removes;
      if (adds.length) data.adds = adds;
      const attrList = [];
      for (const [el, names] of attrs) {
        const id = ids.get(el);
        if (!id || id >= firstFresh) continue;   // just serialized: already current
        const tag = el.namespaceURI === SVG_NS ? el.tagName : el.tagName.toLowerCase();
        const changed = {};
        let any = false;
        for (const name in names) {
          const value = el.hasAttribute(name) ? cleanAttr(tag, name.toLowerCase(), el.getAttribute(name)) : null;
          if (value !== undefined) { changed[name] = value; any = true; }
        }
        if (any) attrList.push({ id, attrs: changed });
      }
      if (attrList.length) data.attrs = attrList;
      const textList = [];
      for (const node of texts) {
        const id = ids.get(node);
        if (!id || id >= firstFresh) continue;
        const parent = node.parentNode;
        let text = node.nodeValue || "";
        if (settings.maskAllText || (parent && (parent.nodeName === "TEXTAREA" || maskedContext(parent)))) text = maskText(text);
        textList.push({ id, text });
      }
      if (textList.length) data.texts = textList;
      if (data.removes || data.adds || data.attrs || data.texts) emit("mut", data);
    }

    // insertRule / deleteRule never reach a MutationObserver, so the sheet is
    // re-read (throttled) whenever its owner node is touched.
    function markCss(owner) {
      if (!ids.has(owner)) return;
      cssDirty.add(owner);
      if (!cssTimer) cssTimer = later(flushCss, 200);
    }

    function flushCss() {
      cssTimer = 0;
      const css = [];
      for (const owner of cssDirty) {
        const id = ids.get(owner);
        if (!id) continue;
        let text = owner.sheet ? sheetCss(owner.sheet, 0) : null;
        if (text == null && owner.nodeName === "STYLE") text = absoluteCss(owner.textContent || "", doc.baseURI);
        if (text != null) css.push({ id, css: text });
      }
      cssDirty.clear();
      if (css.length) emit("mut", { css });
    }

    // -- incremental: user input

    function touch() {
      const t = now();
      if (paused) {
        if (t - lockCheck > 5000) { lockCheck = t; if (ownsLock()) { lastActivity = t; snapshot(); } }
        return;
      }
      if (lastActivity && t - lastActivity < ACTIVE_GAP_MS) stats.activeMs += t - lastActivity;
      lastActivity = t;
    }

    const pointOf = (e) => (e.touches && e.touches[0]) || (e.changedTouches && e.changedTouches[0]) || e;

    function flushMoves() {
      if (moveTimer) { clearTimeout(moveTimer); moveTimer = 0; }
      if (!moves.length) return;
      const base = moves[0][2];
      emit("move", { positions: moves.map((m) => [m[0], m[1], m[2] - base]) }, base);
      moves = [];
    }

    function onMove(e) {
      const t = now();
      if (t - lastMove < 50) return;
      lastMove = t;
      touch();
      if (paused) return;
      const p = pointOf(e);
      moves.push([Math.round(p.clientX), Math.round(p.clientY), t]);
      if (!moveTimer) moveTimer = later(flushMoves, 500);
    }

    function onPointer(e) {
      touch();
      if (e.type === "click") stats.clicks++;
      const p = pointOf(e);
      emit("mouse", { type: e.type, id: ids.get(e.target) || 0, x: Math.round(p.clientX || 0), y: Math.round(p.clientY || 0) });
    }

    function onFocus(e) {
      const id = e.target && e.target.nodeType === 1 ? ids.get(e.target) : 0;
      if (id) emit("mouse", { type: e.type, id });
    }

    function onScroll(e) {
      const target = e.target === win || e.target === doc ? doc : e.target;
      if (!ids.has(target)) return;
      touch();
      if (scrolls.has(target)) return;
      scrolls.set(target, later(() => {
        scrolls.delete(target);
        const id = ids.get(target);
        if (!id) return;
        const page = target === doc;
        if (page) emit("scroll", { id, x: Math.round(win.scrollX), y: Math.round(win.scrollY), h: pageHeight() });
        else emit("scroll", { id, x: Math.round(target.scrollLeft), y: Math.round(target.scrollTop) });
      }, 100));
    }

    function onResize() {
      if (resizeTimer) return;
      resizeTimer = later(() => {
        resizeTimer = 0;
        emit("resize", { width: win.innerWidth, height: win.innerHeight });
      }, 200);
    }

    function flushInputs() {
      if (inputTimer) { clearTimeout(inputTimer); inputTimer = 0; }
      for (const el of inputs) {
        const id = ids.get(el);
        if (!id) continue;
        const tag = el.tagName.toLowerCase(), type = (el.getAttribute("type") || "").toLowerCase();
        if (type === "checkbox" || type === "radio") emit("input", { id, checked: !!el.checked });
        else if (tag !== "select" || el.hasAttribute("data-fm-unmask")) emit("input", { id, value: inputValue(el, el.value) });
      }
      inputs.clear();
    }

    function onInput(e) {
      const el = e.target;
      if (!el || !ids.has(el) || !/^(INPUT|TEXTAREA|SELECT)$/.test(el.nodeName)) return;
      touch();
      inputs.add(el);
      if (!inputTimer) inputTimer = later(flushInputs, 100);
    }

    // -- chunking

    function toBase64(bytes) {
      let text = "";
      for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
      return btoa(text);
    }

    async function pack(json) {
      if (typeof CompressionStream === "function") {
        try {
          const stream = new Blob([json]).stream().pipeThrough(new CompressionStream("gzip"));
          return { encoding: "gzip-b64", events: toBase64(new Uint8Array(await new Response(stream).arrayBuffer())) };
        } catch (_) { /* fall through to raw json */ }
      }
      return { encoding: "json", events: json };
    }

    // [] when an oversized event had to be dropped, null when that event was
    // a full snapshot (the page cannot be recorded at all).
    async function encode(events, job) {
      const packed = await pack(JSON.stringify(events));
      const size = packed.encoding === "json" ? byteLength(packed.events) : packed.events.length;
      if (size <= REPLAY_CHUNK_MAX) return [packed];
      if (events.length === 1) {
        if (events[0].k === "full") return null;
        job.lossy = true;
        return [];
      }
      const mid = events.length >> 1;
      const head = await encode(events.slice(0, mid), job);
      if (!head) return null;
      const tail = await encode(events.slice(mid), job);
      return tail ? head.concat(tail) : null;
    }

    function payload(job, packed, first) {
      return {
        sessionId: job.sid, distinctId: host.distinctId(), seq: takeSeq(job.sid), startedAt: job.events[0].t,
        url: host.url(location.href), viewport: { width: win.innerWidth, height: win.innerHeight },
        device: { browser: host.device.browser, os: host.device.os, type: host.device.type },
        encoding: packed.encoding, events: packed.events,
        activeMs: first ? Math.min(job.stats.activeMs, 3600000) : 0,
        clicks: first ? job.stats.clicks : 0, keys: first ? job.stats.keys : 0,
      };
    }

    async function encodeJob(job) {
      let parts = null;
      if (!job.cancelled && !dead) {
        try { parts = await encode(job.events, job); } catch (_) { parts = null; }
      }
      const index = jobs.indexOf(job);
      if (index >= 0) jobs.splice(index, 1);
      if (job.cancelled || dead) return;
      if (!parts) { fail(); return; }
      parts.forEach((packed, i) => ready.push({ payload: payload(job, packed, i === 0), tries: 0 }));
      if (ready.reduce((sum, chunk) => sum + chunk.payload.events.length, 0) > REPLAY_MEMORY_MAX) { fail(); return; }
      // A dropped mutation leaves the player's tree wrong; start over.
      if (job.lossy) snapshot();
    }

    function cut() {
      if (!running || dead) return chain;
      flushMoves(); flushInputs();
      if (!paused && now() - (lastActivity || begunAt) > REPLAY_IDLE_MS) pause();   // nobody is there
      if (!buf.length || paused) return chain;
      if (!ownsLock()) { pause(); return chain; }
      const job = { events: buf, stats, sid, cancelled: false };
      buf = []; stats = newStats();
      jobs.push(job);
      chain = chain.then(() => encodeJob(job)).catch(() => {});
      return chain;
    }

    function pause() { paused = true; buf = []; moves = []; }

    function fail() {
      dead = true;
      stop();
      ready.length = 0; buf = [];
    }

    // Unload: compression is async, so whatever is not encoded yet goes out as
    // raw JSON if it fits the beacon budget; otherwise the tail is lost.
    function syncTail(budget) {
      const out = [];
      if (!running && !ready.length) return out;
      flushMoves(); flushInputs();
      if (now() - t0 < minDuration) { ready.length = 0; buf = []; return out; }
      while (ready.length && ready[0].payload.events.length + 700 <= budget) {
        const chunk = ready.shift();
        budget -= chunk.payload.events.length + 700;
        out.push(chunk.payload);
      }
      if (!ready.length) {
        const job = { events: [], stats: newStats(), sid };
        for (const pending of jobs.concat([{ events: buf, stats }])) {
          pending.cancelled = true;
          job.events = job.events.concat(pending.events);
          for (const name in job.stats) job.stats[name] += pending.stats[name];
        }
        buf = []; stats = newStats();
        let json = JSON.stringify(job.events);
        while (job.events.length && byteLength(json) + 700 > budget) {
          job.events = job.events.slice(0, Math.floor(job.events.length * 0.7));
          json = JSON.stringify(job.events);
        }
        if (job.events.length) out.push(payload(job, { encoding: "json", events: json }, true));
      }
      return out;
    }

    // -- lifecycle

    function listen(target, type, fn, options) {
      const handler = guard(fn);
      target.addEventListener(type, handler, options);
      offs.push(() => target.removeEventListener(type, handler, options));
    }

    function patchSheets() {
      const proto = win.CSSStyleSheet && win.CSSStyleSheet.prototype;
      if (!proto) return;
      for (const name of ["insertRule", "deleteRule", "replaceSync"]) {
        const original = proto[name];
        if (typeof original !== "function") continue;
        const wrapper = function () {
          const result = original.apply(this, arguments);
          try { if (this.ownerNode) markCss(this.ownerNode); } catch (_) { /* never break the page's CSSOM call */ }
          return result;
        };
        proto[name] = wrapper;
        patches.push(() => { if (proto[name] === wrapper) proto[name] = original; });
      }
    }

    function start(replaySettings) {
      if (running || dead) return;
      settings = replaySettings || {};
      minDuration = Math.max(0, Number(settings.minDurationMs) || 0);
      sid = host.sessionId();
      const saved = host.store.get("rseq", true);
      t0 = saved && saved.sid === sid && saved.t0 ? saved.t0 : now();
      running = true;
      begunAt = now();
      const capture = { capture: true, passive: true };
      listen(doc, "mousemove", onMove, capture);
      listen(doc, "touchmove", onMove, capture);
      for (const type of ["click", "dblclick", "contextmenu", "mousedown", "mouseup", "touchstart", "touchend"]) {
        listen(doc, type, onPointer, capture);
      }
      listen(doc, "focus", onFocus, capture);
      listen(doc, "blur", onFocus, capture);
      listen(doc, "scroll", onScroll, capture);
      listen(doc, "input", onInput, capture);
      listen(doc, "change", onInput, capture);
      listen(doc, "keydown", () => { stats.keys++; touch(); }, capture);   // a count, never the key
      listen(doc, "load", (e) => { if (e.target && e.target.nodeName === "LINK" && e.target.sheet) markCss(e.target); }, capture);
      listen(win, "resize", onResize, { passive: true });
      patchSheets();
      const begin = () => {
        if (!running) return;
        if (ownsLock()) snapshot(); else paused = true;
        if (win.MutationObserver) {
          observer = new MutationObserver(guard(onMutations));
          observer.observe(doc, { childList: true, subtree: true, attributes: true, characterData: true });
        }
        // Short visits would otherwise only ever reach the lossy unload path.
        host.flushSoon(Math.max(minDuration, 2000) + 50);
      };
      if (doc.readyState === "loading") listen(doc, "DOMContentLoaded", begin, { once: true }); else begin();
    }

    function stop() {
      if (!running) return;
      if (!dead) cut();
      running = false;
      if (observer) { observer.disconnect(); observer = null; }
      offs.forEach((off) => off()); offs = [];
      patches.forEach((undo) => undo()); patches = [];
      for (const timer of scrolls.values()) clearTimeout(timer);
      scrolls.clear();
      [moveTimer, inputTimer, cssTimer, resizeTimer].forEach((timer) => timer && clearTimeout(timer));
      moveTimer = inputTimer = cssTimer = resizeTimer = 0;
    }

    return {
      start, stop, cut, snapshot, syncTail,
      running: () => running && !dead,
      custom: (name, data) => emit("custom", Object.assign({ name }, data)),
      peek: () => (ready.length && now() - t0 >= minDuration ? ready[0] : null),
      sent: (chunk) => { const i = ready.indexOf(chunk); if (i >= 0) ready.splice(i, 1); },
      drop: (chunk) => { const i = ready.indexOf(chunk); if (i >= 0) ready.splice(i, 1); snapshot(); },
    };
  }

  // ---------------------------------------------------------------- surveys

  const SURVEY_CSS = `
:host{all:initial;--bg:#fff;--fg:#18181b;--muted:#6b7280;--line:#d4d4d8;--accent:var(--fm-accent,#2563eb)}
@media (prefers-color-scheme:dark){:host{--bg:#18181b;--fg:#f4f4f5;--muted:#a1a1aa;--line:#3f3f46}}
.box{position:relative;box-sizing:border-box;width:340px;max-width:calc(100vw - 32px);max-height:calc(100vh - 32px);overflow:auto;padding:16px;
border:1px solid var(--line);border-radius:12px;background:var(--bg);color:var(--fg);
font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;box-shadow:0 12px 32px rgba(0,0,0,.22);outline:none}
.q{margin:0 28px 4px 0;font-size:15px;font-weight:600}.d{margin:0 0 8px;color:var(--muted)}
.x{position:absolute;top:8px;right:8px;width:28px;height:28px;border:0;border-radius:6px;background:none;color:var(--muted);font-size:20px;line-height:1;cursor:pointer}
textarea{box-sizing:border-box;width:100%;min-height:72px;padding:8px;border:1px solid var(--line);border-radius:8px;background:transparent;color:inherit;font:inherit;resize:vertical}
.scale{display:flex;gap:4px;margin:8px 0 2px}.scale button{flex:1;min-width:0;height:36px;padding:0;border:1px solid var(--line);border-radius:8px;background:transparent;color:inherit;font:inherit;cursor:pointer}
.scale button[aria-pressed=true]{border-color:var(--accent);background:var(--accent);color:#fff}
.ends{display:flex;justify-content:space-between;color:var(--muted);font-size:12px}
fieldset{margin:8px 0 0;padding:0;border:0}label.c{display:flex;gap:8px;align-items:center;padding:6px 8px;border:1px solid var(--line);border-radius:8px;margin-top:6px;cursor:pointer}
input{accent-color:var(--accent);margin:0}
.row{display:flex;align-items:center;justify-content:space-between;margin-top:12px}.step{color:var(--muted);font-size:12px}
.go{padding:8px 14px;border:0;border-radius:8px;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer;text-decoration:none;display:inline-block}
.go[disabled]{opacity:.45;cursor:default}
button:focus-visible,a:focus-visible,textarea:focus-visible,input:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
.thanks{margin:4px 0;font-weight:600}`;

  function createSurveys(host) {
    let list = [], showing = null, pending = 0;
    const shown = new Set();

    const make = (tag, className, text) => {
      const el = doc.createElement(tag);
      if (className) el.className = className;
      if (text != null) el.textContent = text;   // server strings only ever land as text
      return el;
    };

    function urlMatches(pattern, mode) {
      const href = location.href;
      if (mode === "exact") return href === pattern;
      if (mode === "regex") { try { return new RegExp(pattern).test(href); } catch (_) { return false; } }
      return href.toLowerCase().indexOf(String(pattern).toLowerCase()) >= 0;
    }

    function eligible(survey) {
      if (!survey || !survey.id || !Array.isArray(survey.questions) || !survey.questions.length) return false;
      if (shown.has(survey.id) || !host.enabled()) return false;
      const target = survey.targeting || {};
      const seen = (host.store.get("sv") || {})[survey.id];
      const waitDays = target.waitDays == null ? 30 : Number(target.waitDays) || 0;
      if (seen && now() - seen < waitDays * 86400000) return false;
      if (target.url && !urlMatches(target.url, target.urlMatching)) return false;
      return !target.flagKey || !!host.flag(target.flagKey);
    }

    function schedule(survey) {
      if (pending || showing) return;
      const delay = Math.max(0, Number((survey.targeting || {}).delaySeconds) || 0) * 1000;
      pending = later(() => {
        pending = 0;
        if (!showing && eligible(survey)) show(survey);
      }, delay);
    }

    function evaluate() {
      for (const survey of list) {
        const target = (survey && survey.targeting) || {};
        if (!target.selector && !target.event && eligible(survey)) { schedule(survey); return; }
      }
    }

    function onClick(e) {
      if (!e.target || !e.target.closest) return;
      for (const survey of list) {
        const selector = survey && survey.targeting && survey.targeting.selector;
        if (!selector || !eligible(survey)) continue;
        let hit = false;
        try { hit = !!e.target.closest(selector); } catch (_) { hit = false; }   // a bad selector is the project's typo
        if (hit) { schedule(survey); return; }
      }
    }

    function onEvent(name) {
      for (const survey of list) {
        if (survey && survey.targeting && survey.targeting.event === name && eligible(survey)) { schedule(survey); return; }
      }
    }

    function responses(survey, answers) {
      const props = { $survey_id: survey.id, $survey_name: survey.name || "" };
      answers.forEach((answer, i) => {
        if (answer !== undefined) props[i ? `$survey_response_${i}` : "$survey_response"] = answer;
      });
      return props;
    }

    function close() {
      if (!showing) return;
      const el = showing.el;
      showing = null;
      if (el.parentNode) el.parentNode.removeChild(el);
    }

    function show(survey) {
      if (!doc.body || !doc.body.attachShadow) return;
      shown.add(survey.id);
      const seen = host.store.get("sv") || {};
      seen[survey.id] = now();
      host.store.set("sv", seen);

      const look = survey.appearance || {};
      const el = doc.createElement("div");
      el.setAttribute("data-fm-no-capture", "");
      el.setAttribute("data-fm-survey", survey.id);
      const side = look.position === "left" ? "left:16px" : look.position === "center" ? "left:50%;transform:translateX(-50%)" : "right:16px";
      el.style.cssText = `position:fixed;z-index:2147483000;bottom:16px;${side}`;
      if (/^#[0-9a-fA-F]{3,8}$/.test(look.accent || "")) el.style.setProperty("--fm-accent", look.accent);
      const root = el.attachShadow({ mode: "open" });
      try {
        // A constructed sheet is not subject to the host page's style-src.
        const sheet = new CSSStyleSheet();
        sheet.replaceSync(SURVEY_CSS);
        root.adoptedStyleSheets = [sheet];
      } catch (_) {
        root.appendChild(make("style", "", SURVEY_CSS));
      }
      const box = make("div", "box");
      box.setAttribute("role", "dialog");
      box.setAttribute("aria-modal", "false");
      box.setAttribute("aria-labelledby", "fm-q");
      box.tabIndex = -1;
      root.appendChild(box);

      const answers = [];
      let index = 0;
      const dismiss = guard(() => {
        if (!showing || showing.el !== el) return;
        host.capture("survey dismissed", responses(survey, answers));
        close();
      });
      box.addEventListener("keydown", guard((e) => { if (e.key === "Escape") { e.stopPropagation(); dismiss(); } }));

      const finish = () => {
        host.capture("survey sent", responses(survey, answers));
        box.textContent = "";
        const thanks = make("p", "thanks", look.thankYou || "Thank you!");
        thanks.setAttribute("role", "status");
        box.appendChild(thanks);
        later(() => { if (showing && showing.el === el) close(); }, 2500);
      };

      const render = () => {
        const q = survey.questions[index] || {};
        const last = index >= survey.questions.length - 1;
        box.textContent = "";
        const x = make("button", "x", "×");
        x.type = "button";
        x.setAttribute("aria-label", "Dismiss survey");
        x.addEventListener("click", dismiss);
        const form = make("form");
        form.noValidate = true;
        const title = make("h2", "q", q.question || "");
        title.id = "fm-q";
        form.appendChild(title);
        if (q.description) { const d = make("p", "d", q.description); d.id = "fm-d"; form.appendChild(d); }
        const go = make("button", "go", last ? "Submit" : "Next");
        go.type = "submit";
        const update = () => {
          const a = answers[index];
          const empty = a === undefined || a === "" || (Array.isArray(a) && !a.length);
          go.disabled = !q.optional && q.type !== "link" && empty;
        };
        const describe = (control) => { if (q.description) control.setAttribute("aria-describedby", "fm-d"); };

        if (q.type === "rating" || q.type === "nps") {
          const from = q.type === "nps" ? 0 : 1;
          const to = q.type === "nps" ? 10 : Math.min(10, Math.max(3, Number(q.scale) || 5));
          const group = make("div", "scale");
          group.setAttribute("role", "group");
          group.setAttribute("aria-labelledby", "fm-q");
          describe(group);
          for (let value = from; value <= to; value++) {
            const b = make("button", "", String(value));
            b.type = "button";
            b.setAttribute("aria-pressed", String(answers[index] === value));
            b.addEventListener("click", guard(() => {
              answers[index] = value;
              group.querySelectorAll("button").forEach((other) => other.setAttribute("aria-pressed", String(other === b)));
              update();
            }));
            group.appendChild(b);
          }
          form.appendChild(group);
          if (q.lowLabel || q.highLabel) {
            const ends = make("div", "ends");
            ends.appendChild(make("span", "", q.lowLabel || ""));
            ends.appendChild(make("span", "", q.highLabel || ""));
            form.appendChild(ends);
          }
        } else if (q.type === "single" || q.type === "multiple") {
          const set = make("fieldset");
          set.setAttribute("aria-labelledby", "fm-q");
          describe(set);
          (q.choices || []).forEach((choice) => {
            const label = make("label", "c");
            const input = make("input");
            input.type = q.type === "single" ? "radio" : "checkbox";
            input.name = "fm-choice";
            const current = answers[index];
            input.checked = q.type === "single" ? current === choice : Array.isArray(current) && current.indexOf(choice) >= 0;
            input.addEventListener("change", guard(() => {
              if (q.type === "single") answers[index] = choice;
              else {
                answers[index] = Array.from(set.querySelectorAll("input")).filter((i) => i.checked)
                  .map((i) => i.parentNode.textContent);
              }
              update();
            }));
            label.appendChild(input);
            label.appendChild(make("span", "", choice));
            set.appendChild(label);
          });
          form.appendChild(set);
        } else if (q.type === "link") {
          if (/^https:\/\//i.test(q.link || "")) {
            const link = make("a", "go", q.buttonText || "Open");
            link.href = q.link;
            link.target = "_blank";
            link.rel = "noopener noreferrer";
            link.addEventListener("click", guard(() => { answers[index] = "link clicked"; }));
            form.appendChild(link);
          }
        } else {
          const area = make("textarea");
          area.maxLength = 1000;
          area.rows = 3;
          area.value = answers[index] || "";
          area.setAttribute("aria-labelledby", "fm-q");
          describe(area);
          area.addEventListener("input", guard(() => { answers[index] = area.value.trim(); update(); }));
          form.appendChild(area);
        }

        const row = make("div", "row");
        row.appendChild(make("span", "step", survey.questions.length > 1 ? `${index + 1} / ${survey.questions.length}` : ""));
        if (q.type === "link" && last) go.textContent = "Done";
        row.appendChild(go);
        form.appendChild(row);
        form.addEventListener("submit", guard((e) => {
          e.preventDefault();
          if (go.disabled) return;
          if (last) finish(); else { index++; render(); box.focus({ preventScroll: true }); }
        }));
        update();
        box.appendChild(x);
        box.appendChild(form);
      };

      showing = { el, id: survey.id };
      render();
      doc.body.appendChild(el);
      // Take focus only when the visitor is not in the middle of something.
      if (!doc.activeElement || doc.activeElement === doc.body) box.focus({ preventScroll: true });
      host.capture("survey shown", { $survey_id: survey.id, $survey_name: survey.name || "" });
    }

    return {
      set(items) { list = Array.isArray(items) ? items : []; evaluate(); },
      evaluate, onClick, onEvent,
      stop() { if (pending) clearTimeout(pending); pending = 0; list = []; close(); },
    };
  }

  // --------------------------------------------------------------- instance

  const instances = {};
  const INTERACTIVE = 'a,button,input,select,textarea,label,[role="button"]';
  const SAFE_ATTRS = { type: 1, name: 1, role: 1, "aria-label": 1, "data-attr": 1, "data-forkmesh-product-action": 1 };

  function createInstance(options) {
    const config = Object.assign({
      apiHost: SCRIPT_ORIGIN, persistence: "localStorage", autocapture: true, capturePageview: true,
      capturePageleave: true, sessionRecording: true, heatmaps: true, captureErrors: true,
      capturePerformance: true, surveys: true, respectDoNotTrack: true, maskUrlQuery: false,
      trackBots: false, bootstrap: null, sanitizeProperties: null, beforeSend: null,
    }, options);
    const key = typeof config.key === "string" && config.key.length <= 64 ? config.key : "";
    const apiHost = String(config.apiHost || SCRIPT_ORIGIN).replace(/\/+$/, "");
    const store = makeStore(key, config.persistence !== "memory");
    const device = parseUA(nav.userAgent || "");
    const url = (href) => cleanUrl(href, !!config.maskUrlQuery);

    // A privacy signal or a bot disables the instance for good: no storage
    // writes, no listeners, no requests.
    let disabled = !key || (config.respectDoNotTrack !== false && privacySignal()) || (!config.trackBots && isBot());
    let optedOut = !disabled && store.get("optout") === 1;
    let stopped = false;   // 401 / 403: the project does not want this page
    const enabled = () => !disabled && !optedOut && !stopped;

    let identity = (!disabled && store.get("id")) || null;
    if (!identity || !identity.d) identity = { d: uuid7(), i: 0 };
    let superProps = (!disabled && store.get("sp")) || {};
    let groups = (!disabled && store.get("grp")) || {};
    let personProps = {};
    let session = null, sessionSync = 0;
    let windowId = (!disabled && store.get("win", true)) || "";
    if (!windowId) { windowId = uuid7(); if (enabled()) store.set("win", windowId, true); }

    let queue = [], sending = false, again = false, flushTimer = 0, soonTimer = 0;
    let failures = 0, backoffUntil = 0, persisted = false;
    let decide = null, flags = {}, payloads = {}, flagsLoaded = false, decideRun = 0, decideRetries = 0;
    const flagListeners = [];
    const cleanups = [], observers = [];
    let started = false, recordingForced = false;
    let heat = [], lastHeat = 0, clicks = [];
    let errorCount = 0, lastSetHash = "";
    const seenErrors = new Set();
    const vitals = {}, interactions = new Map();
    let vitalsSent = false;
    let page = null;

    if (config.bootstrap && config.bootstrap.flags) {
      flags = Object.assign({}, config.bootstrap.flags);
      payloads = Object.assign({}, config.bootstrap.payloads);
      flagsLoaded = true;
    }

    // -- session and attribution

    function landing() {
      const at = {};
      try {
        const params = new URL(location.href).searchParams;
        for (const name of ["utm_source", "utm_medium", "utm_campaign", "utm_term", "utm_content"]) {
          const value = params.get(name);
          if (value) at[name] = value.slice(0, 200);
        }
        // Click ids identify a person to an ad network; only their presence is kept.
        if (params.has("gclid")) at.gclid = true;
        if (params.has("fbclid")) at.fbclid = true;
      } catch (_) { /* opaque URL */ }
      at.$referrer = doc.referrer ? url(doc.referrer) : "$direct";
      at.$referring_domain = doc.referrer ? hostOf(doc.referrer) : "$direct";
      at.$url = url(location.href);
      at.$path = location.pathname;
      return at;
    }

    // passive = an event the visitor did not cause; it must not extend or
    // rotate the session.
    function touchSession(t, passive) {
      const expired = (s) => !s || !s.id || t - s.last > SESSION_IDLE_MS || t - s.start > SESSION_MAX_MS;
      if (passive && session) return session;
      const previous = session && session.id;
      if (expired(session) || t - sessionSync > 5000) {
        // Another tab may have refreshed or rotated the shared session.
        const saved = store.get("ses");
        if (saved && !expired(saved) && (expired(session) || (saved.id !== session.id && saved.start > session.start))) session = saved;
      }
      let write = t - sessionSync > 5000;
      if (expired(session)) { session = { id: uuid7(), start: t, last: t, at: landing(), io: 0, fc: {} }; write = true; }
      session.last = t;
      if (write) { store.set("ses", session); sessionSync = t; }
      if (previous && previous !== session.id) later(restartRecorder, 0);
      return session;
    }

    // -- page state

    function newPage(referrer) {
      return {
        url: url(location.href), pathname: location.pathname, host: location.host,
        referrer: referrer ? url(referrer) : "$direct", referringDomain: referrer ? hostOf(referrer) : "$direct",
        title: "", entered: now(), maxScroll: Math.max(0, Math.round(win.scrollY || 0)), left: false,
        route: config.maskUrlQuery ? location.pathname : location.pathname + location.search + location.hash,
      };
    }

    function eventProps(at, from) {
      const screen = win.screen || {};
      if (!from.left) from.title = String(doc.title || "").slice(0, 200);
      const props = {
        $current_url: from.url, $pathname: from.pathname, $host: from.host, $referrer: from.referrer,
        $referring_domain: from.referringDomain, $title: from.title,
        $browser: device.browser, $browser_version: device.version, $os: device.os, $device_type: device.type,
        $screen_width: screen.width || 0, $screen_height: screen.height || 0,
        $viewport_width: win.innerWidth || 0, $viewport_height: win.innerHeight || 0,
        $language: nav.language || "", $timezone_offset: new Date().getTimezoneOffset(),
        $lib: "forkmesh-web", $lib_version: VERSION, $window_id: windowId,
      };
      for (const name in at) if (name[0] !== "$") props[name] = at[name];
      return props;
    }

    // -- capture

    function capture(name, props, extra) {
      if (!enabled()) return null;
      name = String(name == null ? "" : name).trim().slice(0, 200);
      if (!name) return null;
      extra = extra || {};
      const t = extra.ts || now();
      const s = touchSession(t, extra.passive);
      let properties = Object.assign(eventProps(s.at || {}, extra.page || page || newPage(doc.referrer)), { $session_id: s.id }, superProps, props);
      if (typeof config.sanitizeProperties === "function") {
        try { properties = config.sanitizeProperties(properties, name) || properties; } catch (_) { /* keep ours */ }
      }
      let event = { event: name, distinctId: identity.d, ts: t, uuid: uuid7(), sessionId: s.id, properties };
      if (extra.set && Object.keys(extra.set).length) event.set = extra.set;
      if (extra.setOnce && Object.keys(extra.setOnce).length) event.setOnce = extra.setOnce;
      if (!s.io) {
        // First event of the session: first-touch attribution, written once.
        const at = s.at || {}, initial = {
          $initial_referrer: at.$referrer, $initial_referring_domain: at.$referring_domain,
          $initial_current_url: at.$url, $initial_pathname: at.$path,
        };
        for (const item in at) if (item[0] !== "$") initial["$initial_" + item] = at[item];
        event.setOnce = Object.assign(initial, event.setOnce);
        s.io = 1;
        store.set("ses", s);
      }
      if (Object.keys(groups).length) event.groups = Object.assign({}, groups);
      if (typeof config.beforeSend === "function") {
        // A hook that throws is treated as "drop": it exists to keep data in.
        try { event = config.beforeSend(event); } catch (_) { event = null; }
        if (!event) return null;
      }
      queue.push(event);
      if (queue.length > MAX_QUEUE) queue.splice(0, queue.length - MAX_QUEUE);
      if (recorder.running() && name !== "$heatmap") {
        recorder.custom(name, name === "$pageview" ? { url: properties.$current_url }
          : name === "$exception" ? { message: String(properties.$exception_message || "").slice(0, 200) } : null);
      }
      if (!/^survey /.test(name)) surveys.onEvent(name);
      if (queue.length >= FLUSH_AT) flush(); else scheduleFlush();
      return event;
    }

    // -- transport

    function scheduleFlush() {
      if (!flushTimer && enabled()) flushTimer = later(flush, Math.max(FLUSH_MS, backoffUntil - now()));
    }

    function flushSoon(ms) {
      if (soonTimer || !enabled()) return;
      soonTimer = later(() => { soonTimer = 0; flush(); }, ms);
    }

    // keepLarge: on the beacon path an event that does not fit the remaining
    // budget stays queued instead of being discarded.
    function takeBatch(budget, keepLarge) {
      const batch = [];
      let size = 0;
      while (queue.length && batch.length < MAX_BATCH) {
        const length = JSON.stringify(queue[0]).length;
        if (size + length > budget) {
          if (batch.length || keepLarge) break;
          queue.shift();   // can never be sent
          continue;
        }
        size += length;
        batch.push(queue.shift());
      }
      return batch;
    }

    function persistQueue() {
      if (config.persistence === "memory") return;
      if (!queue.length) { if (persisted) { store.remove("q"); persisted = false; } return; }
      let keep = queue.slice(-MAX_PERSISTED);
      while (keep.length && JSON.stringify(keep).length > 400000) keep = keep.slice(Math.ceil(keep.length / 2));
      try { win.localStorage.setItem(`fma.${key}.q`, JSON.stringify(keep)); persisted = true; } catch (_) { /* full: drop */ }
    }

    function halt() {
      stopped = true;
      queue = [];
      store.remove("q");
      recorder.stop();
      surveys.stop();
      clearTimeout(flushTimer); clearTimeout(soonTimer);
      flushTimer = soonTimer = 0;
    }

    function request(path, body) {
      return fetch(apiHost + path, {
        method: "POST", body, mode: "cors", credentials: "omit", cache: "no-store",
        keepalive: body.length < KEEPALIVE_MAX,
      });
    }

    function flush() {
      if (flushTimer) { clearTimeout(flushTimer); flushTimer = 0; }
      if (!enabled()) return;
      if (sending) { again = true; return; }
      if (now() < backoffUntil) { scheduleFlush(); return; }
      drainHeat();
      sending = true;
      const next = guard(sendNext);
      Promise.resolve(recorder.cut()).then(next, next);
    }

    function sendNext() {
      if (!enabled()) { sending = false; return; }
      const chunk = recorder.peek();
      const batch = takeBatch(chunk ? 250000 : 800000, false);
      if (!chunk && !batch.length) {
        sending = false;
        if (again) { again = false; scheduleFlush(); }
        return;
      }
      const message = { key, sentAt: now(), batch };
      if (chunk) message.replay = chunk.payload;
      const done = guard((status, data) => settle(status, data, batch, chunk));
      let sent;
      try { sent = request(CAPTURE_PATH, JSON.stringify(message)); } catch (_) { sent = Promise.reject(); }
      sent.then((response) => response.json().catch(() => null).then((data) => done(response.status, data)), () => done(0, null));
    }

    function settle(status, data, batch, chunk) {
      if (status >= 200 && status < 300) {
        failures = 0;
        if (chunk) recorder.sent(chunk);
        if (data && data.replayError === "replay_disabled") recorder.stop();
        if (persisted) persistQueue();
        if (queue.length >= FLUSH_AT || recorder.peek()) { sendNext(); return; }
        sending = false;
        if (again || queue.length) scheduleFlush();
        again = false;
        return;
      }
      sending = false;
      if (status === 401 || status === 403) { halt(); return; }
      if (status >= 400 && status < 500 && status !== 429 && status !== 408) {
        // The request itself is unacceptable (413, 400); retrying cannot help.
        if (chunk) recorder.drop(chunk);
        if (queue.length) scheduleFlush();
        return;
      }
      queue = batch.concat(queue).slice(0, MAX_QUEUE);
      if (chunk && ++chunk.tries > 3) recorder.drop(chunk);
      failures++;
      backoffUntil = now() + Math.min(300000, 5000 * Math.pow(2, failures - 1)) * (0.8 + Math.random() * 0.4);
      persistQueue();
      scheduleFlush();
    }

    function beacon(body) {
      let ok = false;
      try { ok = !!nav.sendBeacon && nav.sendBeacon(apiHost + CAPTURE_PATH, new Blob([body], { type: "text/plain" })); } catch (_) { ok = false; }
      if (ok) return;
      try {
        fetch(apiHost + CAPTURE_PATH, { method: "POST", body, mode: "cors", credentials: "omit", keepalive: true }).catch(() => {});
      } catch (_) { /* nothing left to try */ }
    }

    // unload = pagehide: nothing async will finish. A plain "hidden" is often
    // just a tab switch, so the recording keeps its lossless async path there.
    function onHide(unload) {
      if (!enabled()) return;
      if (unload && config.capturePageleave !== false) leavePage();
      sendVitals();
      drainHeat();
      let budget = BEACON_BUDGET;
      while (queue.length && budget > 1500 && now() >= backoffUntil) {
        const batch = takeBatch(budget - 300, true);
        if (!batch.length) break;
        const body = JSON.stringify({ key, sentAt: now(), batch });
        budget -= byteLength(body);
        beacon(body);
      }
      if (unload) {
        for (const replay of recorder.syncTail(Math.max(0, budget))) {
          beacon(JSON.stringify({ key, sentAt: now(), batch: [], replay }));
        }
      } else if (queue.length || recorder.running()) {
        flush();
      }
      persistQueue();
    }

    // -- pageviews

    function leavePage() {
      if (!page || page.left) return;
      page.left = true;   // also freezes $title: the router may already have set the next one
      capture("$pageleave", {
        $max_scroll_y: page.maxScroll, $duration_ms: now() - page.entered,
        $page_height: Math.round((doc.documentElement && doc.documentElement.scrollHeight) || 0),
      }, { page, passive: true });
    }

    function enterPage(referrer) {
      page = newPage(referrer);
      if (config.capturePageview !== false) capture("$pageview");
      surveys.evaluate();
    }

    function onRoute() {
      if (!enabled() || !page) return;
      const route = config.maskUrlQuery ? location.pathname : location.pathname + location.search + location.hash;
      if (route === page.route) return;
      const from = page.url;
      drainHeat();
      if (config.capturePageleave !== false) leavePage();
      enterPage(from);
    }

    function patchHistory() {
      for (const name of ["pushState", "replaceState"]) {
        const original = history[name];
        if (typeof original !== "function") continue;
        const wrapper = function () {
          const result = original.apply(this, arguments);
          later(onRoute, 0);   // after the router has updated the title
          return result;
        };
        history[name] = wrapper;
        cleanups.push(() => { if (history[name] === wrapper) history[name] = original; });
      }
    }

    // -- autocapture, rage clicks, heatmaps

    const allowed = (name) => enabled() && config[name] !== false && !!decide && !!decide[name];

    function elementChain(el) {
      const chain = [];
      for (let node = el; node && node.nodeType === 1 && node !== doc.documentElement && chain.length < 8; node = node.parentNode) {
        const attrs = {};
        for (let i = 0; i < node.attributes.length; i++) {
          const name = node.attributes[i].name.toLowerCase();
          let value = node.attributes[i].value;
          if (name === "href") { try { const u = new URL(value, location.href); value = u.origin + u.pathname; } catch (_) { continue; } }
          else if (!SAFE_ATTRS[name] && name.indexOf("data-fm-") !== 0 && name.indexOf("data-ph-capture-attribute-") !== 0) continue;
          attrs[name] = String(value).slice(0, 200);
        }
        let nth = 1;
        for (let sibling = node.previousElementSibling; sibling; sibling = sibling.previousElementSibling) nth++;
        const classes = typeof node.className === "string" ? node.className.split(/\s+/).filter(Boolean).slice(0, 8).map((c) => c.slice(0, 60)) : [];
        chain.push({ tag: node.tagName.toLowerCase(), id: String(node.id || "").slice(0, 100), classes, attrs, nth });
      }
      // The server nulls an $elements value over 6000 characters.
      while (chain.length > 1 && JSON.stringify(chain).length > 5500) chain.pop();
      return chain;
    }

    function elementText(el) {
      if (/^(INPUT|TEXTAREA|SELECT)$/.test(el.nodeName) || el.closest('[data-fm-mask],[contenteditable]:not([contenteditable="false"])')) return "";
      const text = String(el.textContent || "").replace(/\s+/g, " ").trim().slice(0, 120);
      return /(?:\d[ -]?){13,19}/.test(text) ? "" : text;   // looks like a card number
    }

    function describe(el, e) {
      const props = { $elements: elementChain(el) };
      const text = elementText(el);
      if (text) props.$el_text = text;
      if (e && typeof e.clientX === "number" && (e.clientX || e.clientY)) {
        props.$x_ratio = Math.round((e.clientX / Math.max(1, win.innerWidth)) * 1000) / 1000;
        props.$page_y = Math.round(e.clientY + (win.scrollY || 0));
      }
      return props;
    }

    function interactiveTarget(target) {
      let el = target && target.nodeType === 3 ? target.parentNode : target;
      for (let depth = 0; el && el.nodeType === 1 && depth <= 5; depth++, el = el.parentNode) {
        if (el.matches(INTERACTIVE)) return el;
      }
      return null;
    }

    function onAutocapture(e) {
      const raw = e.target && e.target.nodeType === 3 ? e.target.parentNode : e.target;
      if (!raw || raw.nodeType !== 1) return;
      if (e.type === "click") surveys.onClick(e);
      if (!allowed("autocapture") || raw.closest("[data-fm-no-capture]")) return;
      if (e.type === "click") {
        const t = now();
        clicks = clicks.filter((c) => t - c.t < 1000 && Math.abs(c.x - e.clientX) <= 30 && Math.abs(c.y - e.clientY) <= 30);
        clicks.push({ t, x: e.clientX, y: e.clientY });
        if (clicks.length >= 3) { clicks = []; capture("$rageclick", describe(raw, e)); }
      }
      const el = e.type === "submit" ? raw : interactiveTarget(raw);
      if (!el || (e.type === "change" && !/^(INPUT|SELECT|TEXTAREA)$/.test(el.nodeName))) return;
      capture("$autocapture", Object.assign({ $event_type: e.type }, describe(el, e.type === "click" ? e : null)));
    }

    function onHeatMove(e) {
      const t = now();
      if (t - lastHeat < 200 || !allowed("heatmaps")) return;
      lastHeat = t;
      if (heat.length >= HEAT_BUFFER_MAX) return;
      heat.push([Math.round((e.clientX / Math.max(1, win.innerWidth)) * 1000) / 1000, Math.round(e.clientY + (win.scrollY || 0))]);
      scheduleFlush();
    }

    function drainHeat() {
      const points = heat;
      heat = [];
      for (let i = 0; i < points.length; i += HEAT_POINTS_PER_EVENT) {
        capture("$heatmap", { $heatmap_points: points.slice(i, i + HEAT_POINTS_PER_EVENT) }, { passive: true });
      }
    }

    // -- errors and web vitals

    function captureException(error, props, automatic) {
      if (automatic && !allowed("captureErrors")) return;
      let type = "Error", message = "", stack = "";
      if (error && typeof error === "object") {
        type = String(error.name || type);
        message = String(error.message || error);
        stack = String(error.stack || "");
      } else message = String(error);
      if (!message || type === "AbortError" || /^Script error\.?$/i.test(message) || /ResizeObserver loop/i.test(message)) return;
      if (errorCount >= 10 || seenErrors.has(message)) return;
      seenErrors.add(message);
      errorCount++;
      capture("$exception", Object.assign({
        $exception_type: type.slice(0, 120), $exception_message: message.slice(0, 1000), $exception_stack: stack.slice(0, 4000),
      }, props), { passive: true });
    }

    function watchVitals() {
      const PO = win.PerformanceObserver;
      if (!PO || config.capturePerformance === false) return;
      const observe = (type, each, more) => {
        try {
          const o = new PO(guard((list) => list.getEntries().forEach(each)));
          o.observe(Object.assign({ type, buffered: true }, more));
          observers.push(o);
          return true;
        } catch (_) { return false; }
      };
      observe("largest-contentful-paint", (e) => { vitals.LCP = e.startTime; });
      observe("paint", (e) => { if (e.name === "first-contentful-paint") vitals.FCP = e.startTime; });
      // CLS: the worst 5 s window of shifts that are at most 1 s apart.
      let sum = 0, first = 0, last = 0;
      if (observe("layout-shift", (e) => {
        if (e.hadRecentInput) return;
        if (sum && e.startTime - last < 1000 && e.startTime - first < 5000) sum += e.value;
        else { sum = e.value; first = e.startTime; }
        last = e.startTime;
        if (sum > vitals.CLS) vitals.CLS = sum;
      })) vitals.CLS = 0;
      observe("event", (e) => {
        if (!e.interactionId) return;
        interactions.set(e.interactionId, Math.max(interactions.get(e.interactionId) || 0, e.duration));
        if (interactions.size > 400) interactions.delete(interactions.keys().next().value);
      }, { durationThreshold: 40 });
    }

    function sendVitals() {
      if (vitalsSent || !allowed("capturePerformance")) return;
      try {
        const entry = performance.getEntriesByType("navigation")[0];
        if (entry && entry.responseStart > 0) vitals.TTFB = entry.responseStart;
      } catch (_) { /* no navigation timing */ }
      if (interactions.size) {
        // INP ~ the 98th percentile interaction; the worst one under 50.
        const sorted = Array.from(interactions.values()).sort((a, b) => b - a);
        vitals.INP = sorted[Math.min(sorted.length - 1, Math.floor(sorted.length / 50))];
      }
      const props = {};
      for (const name in vitals) props["$web_vitals_" + name] = name === "CLS" ? Math.round(vitals[name] * 10000) / 10000 : Math.round(vitals[name]);
      if (!Object.keys(props).length || (Object.keys(props).length === 1 && "$web_vitals_CLS" in props)) return;
      vitalsSent = true;
      capture("$web_vitals", props, { passive: true });
    }

    // -- feature flags and remote config

    function notifyFlags() {
      for (const cb of flagListeners.slice()) { try { cb(Object.assign({}, flags), Object.assign({}, payloads)); } catch (_) { /* host callback */ } }
    }

    function applyDecide(data, fresh) {
      decide = data;
      flags = Object.assign({}, data.flags);
      payloads = Object.assign({}, data.payloads);
      flagsLoaded = true;
      if (fresh) store.set("dc", { d: identity.d, t: now(), r: data });
      notifyFlags();
      applyFeatures();
    }

    function loadFlags() {
      if (!enabled()) return;
      const run = ++decideRun;
      let sent;
      try { sent = request(DECIDE_PATH, JSON.stringify({ key, distinctId: identity.d, personProperties: personProps })); } catch (_) { sent = Promise.reject(); }
      sent.then((response) => {
        if (response.status === 401 || response.status === 403) { halt(); return null; }
        if (!response.ok) throw new Error("decide");
        return response.json();
      }).then(guard((data) => {
        if (!data || run !== decideRun) return;
        decideRetries = 0;
        applyDecide(data, true);
      })).catch(guard(() => {
        if (run === decideRun && decideRetries < 2) later(loadFlags, 15000 * ++decideRetries);
      }));
    }

    function sampled() {
      const rate = decide && decide.replay ? Number(decide.replay.sampleRate) : 0;
      return recordingForced || hash32(touchSession(now(), true).id) % 100 < (isNaN(rate) ? 100 : rate);
    }

    function applyFeatures() {
      if (!enabled() || !decide) return;
      const replay = decide.replay || {};
      if (config.sessionRecording !== false && replay.enabled && sampled()) recorder.start(replay);
      else recorder.stop();
      if (config.surveys !== false) surveys.set(decide.surveys);
    }

    function restartRecorder() {
      if (!recorder.running()) return;
      recorder.stop();
      applyFeatures();
    }

    function getFeatureFlag(name, options) {
      const value = Object.prototype.hasOwnProperty.call(flags, name) ? flags[name] : false;
      if (flagsLoaded && enabled() && !(options && options.sendEvent === false)) {
        const s = touchSession(now(), true), mark = `${name}=${value}`;
        s.fc = s.fc || {};
        if (!s.fc[mark]) {
          s.fc[mark] = 1;
          store.set("ses", s);
          capture("$feature_flag_called", { $feature_flag: name, $feature_flag_response: value });
        }
      }
      return value;
    }

    // -- wiring

    const recorder = createRecorder({
      store, windowId, device, url,
      sessionId: () => touchSession(now(), true).id,
      distinctId: () => identity.d,
      requestFlush: scheduleFlush, flushSoon,
    });
    const surveys = createSurveys({
      store, enabled, flag: (name) => getFeatureFlag(name, { sendEvent: false }),
      capture: (name, props) => capture(name, props),
    });

    function listen(target, type, fn, options) {
      const handler = guard(fn);
      target.addEventListener(type, handler, options);
      cleanups.push(() => target.removeEventListener(type, handler, options));
    }

    function start() {
      if (started || !enabled()) return;
      started = true;
      store.set("id", identity);
      const saved = config.persistence === "memory" ? null : store.get("q");
      if (Array.isArray(saved) && saved.length) { queue = saved.slice(-MAX_PERSISTED).concat(queue); store.remove("q"); }
      touchSession(now(), false);

      const capturing = { capture: true, passive: true };
      for (const type of ["click", "change", "submit"]) listen(doc, type, onAutocapture, capturing);
      listen(doc, "mousemove", onHeatMove, capturing);
      listen(win, "scroll", () => { if (page && win.scrollY > page.maxScroll) page.maxScroll = Math.round(win.scrollY); }, capturing);
      listen(win, "error", (e) => captureException(e.error || { name: "Error", message: e.message, stack: `${e.filename || ""}:${e.lineno || 0}:${e.colno || 0}` }, null, true));
      listen(win, "unhandledrejection", (e) => captureException(e.reason == null ? "Unhandled rejection" : e.reason, null, true));
      listen(win, "popstate", () => later(onRoute, 0));
      listen(win, "hashchange", () => later(onRoute, 0));
      listen(win, "pagehide", () => onHide(true));
      listen(doc, "visibilitychange", () => { if (doc.visibilityState === "hidden") onHide(false); });
      listen(win, "pageshow", (e) => {
        if (!e.persisted) return;   // back from the bfcache: a new visit to the same page
        enterPage(doc.referrer);
        recorder.snapshot();
      });
      patchHistory();
      watchVitals();

      const cached = store.get("dc");
      const usable = cached && cached.r && cached.d === identity.d;
      if (usable) applyDecide(cached.r, false);
      enterPage(doc.referrer);
      // One decide per five minutes per visitor, not one per page load.
      if (!usable || now() - cached.t > DECIDE_TTL_MS) loadFlags();
      if (queue.length) scheduleFlush();
    }

    function teardown() {
      started = false;
      recorder.stop();
      surveys.stop();
      cleanups.splice(0).forEach((undo) => { try { undo(); } catch (_) { /* ignore */ } });
      observers.splice(0).forEach((o) => { try { o.disconnect(); } catch (_) { /* ignore */ } });
      clearTimeout(flushTimer); clearTimeout(soonTimer);
      flushTimer = soonTimer = 0;
    }

    function personEvent(name, props, set, setOnce) {
      return capture(name, props, { set: set && typeof set === "object" ? set : null, setOnce: setOnce && typeof setOnce === "object" ? setOnce : null });
    }

    // -- public API (every method is guarded; none may throw)

    const api = {
      capture: (name, props, extra) => { capture(name, props, extra); },
      captureException: (error, props) => captureException(error, props, false),
      identify(id, set, setOnce) {
        if (id == null || id === "" || !enabled()) return;
        id = String(id).slice(0, 200);
        if (set && typeof set === "object") personProps = Object.assign(personProps, set);
        if (id !== identity.d) {
          const anonymous = identity.i ? "" : identity.d;
          identity = { d: id, i: 1 };
          store.set("id", identity);
          personEvent("$identify", anonymous ? { $anon_distinct_id: anonymous } : {}, set, setOnce);
          lastSetHash = "";
          loadFlags();
        } else {
          if (!identity.i) { identity.i = 1; store.set("id", identity); }
          api.setPersonProperties(set, setOnce);
        }
      },
      alias(other) {
        if (other == null || other === "" || String(other) === identity.d) return;
        capture("$create_alias", { alias: String(other).slice(0, 200) });
      },
      group(type, groupKey, props) {
        if (!type || groupKey == null || groupKey === "" || !enabled()) return;
        type = String(type).toLowerCase().slice(0, 40);
        groups = Object.assign({}, groups, { [type]: String(groupKey).slice(0, 200) });
        store.set("grp", groups);
        capture("$groupidentify", { $group_type: type, $group_key: groups[type], $group_set: props && typeof props === "object" ? props : {} });
      },
      setPersonProperties(set, setOnce) {
        const mark = JSON.stringify([set || null, setOnce || null]);
        if (mark === "[null,null]" || mark === lastSetHash) return;   // unchanged: no event
        lastSetHash = mark;
        if (set && typeof set === "object") personProps = Object.assign(personProps, set);
        personEvent("$set", {}, set, setOnce);
      },
      register(props) {
        if (!props || typeof props !== "object") return;
        superProps = Object.assign({}, superProps, props);
        if (enabled()) store.set("sp", superProps);
      },
      unregister(name) {
        delete superProps[name];
        if (enabled()) store.set("sp", superProps);
      },
      isFeatureEnabled: (name, options) => { const value = getFeatureFlag(name, options); return value !== false && value != null; },
      getFeatureFlag,
      getFeatureFlagPayload: (name) => (Object.prototype.hasOwnProperty.call(payloads, name) ? payloads[name] : null),
      onFeatureFlags(cb) {
        if (typeof cb !== "function") return () => {};
        flagListeners.push(cb);
        if (flagsLoaded) { try { cb(Object.assign({}, flags), Object.assign({}, payloads)); } catch (_) { /* host callback */ } }
        return () => { const i = flagListeners.indexOf(cb); if (i >= 0) flagListeners.splice(i, 1); };
      },
      reloadFeatureFlags: loadFlags,
      startSessionRecording() { recordingForced = true; config.sessionRecording = true; applyFeatures(); },
      stopSessionRecording() { recordingForced = false; config.sessionRecording = false; recorder.stop(); if (enabled()) flush(); },
      sessionRecordingStarted: () => recorder.running(),
      optOut() {
        if (disabled) return;
        optedOut = true;
        store.set("optout", 1);
        queue = []; heat = [];
        store.remove("q");
        teardown();
      },
      optIn() {
        if (disabled || !optedOut) return;
        optedOut = false;
        store.remove("optout");
        start();
      },
      hasOptedOut: () => disabled || optedOut,
      reset() {
        if (!enabled()) return;
        const recording = recorder.running();
        recorder.stop();
        identity = { d: uuid7(), i: 0 };
        store.set("id", identity);
        superProps = {}; groups = {}; personProps = {}; lastSetHash = "";
        flags = {}; payloads = {};
        ["sp", "grp", "dc", "ses"].forEach((name) => store.remove(name));
        session = null;
        touchSession(now(), false);
        if (recording) applyFeatures();
        loadFlags();
      },
      flush: () => { flush(); },
      shutdown() {
        if (enabled() && started) onHide(true);
        teardown();
        disabled = true;
        if (instances[key] === api) delete instances[key];
      },
      getDistinctId: () => identity.d,
      getSessionId: () => (enabled() ? touchSession(now(), true).id : ""),
      version: VERSION,
    };
    for (const name of Object.keys(api)) if (typeof api[name] === "function") api[name] = guard(api[name]);
    const fallbacks = { isFeatureEnabled: false, getFeatureFlag: false, getFeatureFlagPayload: null, hasOptedOut: true, getDistinctId: "", getSessionId: "", sessionRecordingStarted: false };
    for (const name in fallbacks) {
      const inner = api[name];
      api[name] = function () { const value = inner.apply(this, arguments); return value === undefined ? fallbacks[name] : value; };
    }
    const guardedOn = api.onFeatureFlags;
    api.onFeatureFlags = (cb) => guardedOn(cb) || (() => {});

    guard(start)();
    return api;
  }

  win.ForkMeshAnalyticsSDK = {
    version: VERSION,
    // Idempotent per key: the page and a plugin may both call init.
    init(config) {
      const key = config && typeof config.key === "string" ? config.key : "";
      if (key && instances[key]) return instances[key];
      let instance;
      try { instance = createInstance(config || {}); } catch (_) { instance = createInstance({ key: "" }); }
      if (key) instances[key] = instance;
      return instance;
    },
  };
})();

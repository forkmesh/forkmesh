(() => {
  const root = document.documentElement;
  const body = document.body;
  if (!body || !body.classList.contains("dp-body")) return;
  root.classList.add("dp-enhanced");

  const STORE = "forkmesh.docs.reader.v1";
  const SVG_NS = "http://www.w3.org/2000/svg";
  const here = window.location.pathname;
  const narrow = window.matchMedia("(max-width: 1023px)");
  const finePointer = window.matchMedia("(hover: hover) and (pointer: fine)");
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  const prose = document.querySelector(".dp-prose");
  const toolbar = document.querySelector(".dp-toolbar");
  const search = document.querySelector("#docs-search");

  const make = (tag, className, text) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text != null) node.textContent = text;
    return node;
  };

  const vector = (tag, attributes = {}) => {
    const node = document.createElementNS(SVG_NS, tag);
    Object.entries(attributes).forEach(([name, value]) => node.setAttribute(name, String(value)));
    return node;
  };

  const ICONS = {
    library: "M4 5h6v6H4zM14 5h6v6h-6zM4 15h6v4H4zM14 15h6v4h-6z",
    sections: "M5 6h14M5 12h10M5 18h7",
    search: "M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-3.5-3.5",
    spark: "M12 3l1.9 5.6L19.5 10l-5.6 1.9L12 17.5l-1.9-5.6L4.5 10l5.6-1.4zM19 15l.8 2.2L22 18l-2.2.8L19 21l-.8-2.2L16 18l2.2-.8z",
    up: "M12 18V7M7 11.5 12 6.5l5 5",
    close: "M6 6l12 12M18 6 6 18",
    expand: "M9 4H4v5M15 4h5v5M9 20H4v-5M15 20h5v-5",
    copy: "M8 8h12v12H8zM16 8V4H4v12h4",
    minus: "M6 12h12",
    plus: "M12 6v12M6 12h12",
    fit: "M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5M9 12h6",
  };

  const icon = (name) => {
    const node = vector("svg", { viewBox: "0 0 24 24", "aria-hidden": "true" });
    node.append(vector("path", { d: ICONS[name] }));
    return node;
  };

  const iconButton = (name, label, className = "dp-figure-button") => {
    const button = make("button", className);
    button.type = "button";
    button.setAttribute("aria-label", label);
    button.title = label;
    button.append(icon(name));
    return button;
  };

  const blank = () => ({ v: 1, pages: {}, terms: {}, queries: [], taps: {} });
  const loadMemory = () => {
    try {
      const stored = JSON.parse(window.localStorage.getItem(STORE) || "null");
      if (stored && stored.v === 1 && stored.pages) return { ...blank(), ...stored };
    } catch (error) {}
    return blank();
  };
  let memory = loadMemory();
  let saveTimer = 0;

  const keepTop = (object, limit, rank) => {
    const entries = Object.entries(object);
    if (entries.length <= limit) return object;
    return Object.fromEntries(entries.sort((a, b) => rank(b[1]) - rank(a[1])).slice(0, limit));
  };

  const persist = () => {
    memory.pages = keepTop(memory.pages, 160, (page) => page.l || 0);
    memory.terms = keepTop(memory.terms, 48, (weight) => weight);
    memory.taps = keepTop(memory.taps, 80, (count) => count);
    memory.queries = memory.queries.slice(-6);
    try {
      window.localStorage.setItem(STORE, JSON.stringify(memory));
    } catch (error) {}
  };

  const remember = () => {
    window.clearTimeout(saveTimer);
    saveTimer = window.setTimeout(persist, 500);
  };

  const pageTitle = () => {
    const heading = prose && prose.querySelector("h1");
    return (heading ? heading.firstChild && heading.firstChild.textContent : document.title.split(" · ")[0]) || here;
  };

  const visit = memory.pages[here] || { n: 0, s: 0, d: 0, x: {}, f: Date.now() };
  visit.t = pageTitle();
  visit.n = (visit.n || 0) + 1;
  visit.l = Date.now();
  visit.x = visit.x || {};
  memory.pages[here] = visit;
  remember();
  window.addEventListener("pagehide", persist);

  const headings = prose
    ? Array.from(prose.querySelectorAll("h2[id], h3[id]")).map((heading) => ({
        id: heading.id,
        heading,
        level: Number(heading.tagName.slice(1)),
        text: (heading.firstChild && heading.firstChild.textContent ? heading.firstChild.textContent : heading.textContent).trim(),
      }))
    : [];

  const currentHeading = () => {
    const line = Math.min(window.innerHeight * 0.35, 260);
    let found = null;
    for (const item of headings) {
      if (item.heading.getBoundingClientRect().top <= line) found = item;
      else break;
    }
    return found;
  };

  const readingDepth = () => {
    if (!prose) return 0;
    const box = prose.getBoundingClientRect();
    const span = box.height - window.innerHeight * 0.6;
    if (span <= 0) return box.top < window.innerHeight ? 1 : 0;
    return Math.max(0, Math.min(1, (window.innerHeight * 0.4 - box.top) / span));
  };

  let lastInput = Date.now();
  ["scroll", "pointerdown", "keydown", "touchstart"].forEach((type) =>
    window.addEventListener(type, () => {
      lastInput = Date.now();
    }, { passive: true }),
  );

  window.setInterval(() => {
    if (document.visibilityState !== "visible" || Date.now() - lastInput > 45000) return;
    visit.s = (visit.s || 0) + 5;
    const section = currentHeading();
    if (section) {
      visit.x[section.id] = (visit.x[section.id] || 0) + 5;
      visit.x = keepTop(visit.x, 10, (seconds) => seconds);
    }
    remember();
  }, 5000);

  const termsOf = (text) =>
    (text.toLowerCase().match(/[a-z][a-z0-9]+/g) || []).filter((word) => word.length > 2);

  if (search) {
    let queryTimer = 0;
    search.addEventListener("input", () => {
      window.clearTimeout(queryTimer);
      queryTimer = window.setTimeout(() => {
        const query = search.value.trim();
        if (query.length < 3) return;
        termsOf(query).forEach((term) => {
          memory.terms[term] = Math.min(12, (memory.terms[term] || 0) + 3);
        });
        memory.queries = memory.queries.filter((item) => item !== query).concat(query);
        remember();
        refreshSuggestions();
      }, 900);
    });
  }

  document.addEventListener("click", (event) => {
    const target = event.target instanceof Element ? event.target : null;
    if (!target) return;
    if (target.closest(".dp-copy-code, [data-figure-copy]")) {
      visit.c = (visit.c || 0) + 1;
      remember();
    }
    const link = target.closest("a[href]");
    if (!link) return;
    const url = new URL(link.href, window.location.href);
    if (url.origin === window.location.origin && url.pathname.startsWith("/docs/") && url.pathname !== here) {
      memory.taps[url.pathname] = (memory.taps[url.pathname] || 0) + 1;
      remember();
      persist();
    }
  });

  let indexRequest = null;
  const readerIndex = () => {
    if (indexRequest) return indexRequest;
    const source = body.getAttribute("data-reader-index");
    indexRequest = !source
      ? Promise.resolve(null)
      : fetch(source, { headers: { accept: "application/json" } })
          .then((response) => (response.ok ? response.json() : null))
          .then((data) => {
            if (!data || !Array.isArray(data.pages)) return null;
            data.byUrl = new Map(data.pages.map((page, index) => [page.u, index]));
            data.incoming = data.pages.map(() => []);
            data.pages.forEach((page, index) => page.o.forEach((target) => data.incoming[target] && data.incoming[target].push(index)));
            return data;
          })
          .catch(() => null);
    return indexRequest;
  };

  const sectionLinks = (sectionId) => {
    const start = headings.findIndex((item) => item.id === sectionId);
    if (start < 0) return new Set();
    const end = headings.slice(start + 1).find((item) => item.level <= headings[start].level);
    const urls = new Set();
    let node = headings[start].heading.nextElementSibling;
    while (node && (!end || node !== end.heading)) {
      node.querySelectorAll("a[href]").forEach((link) => {
        const url = new URL(link.href, window.location.href);
        if (url.origin === window.location.origin) urls.add(url.pathname);
      });
      node = node.nextElementSibling;
    }
    return urls;
  };

  const suggest = (index, limit = 5) => {
    const now = Date.now();
    const self = index.byUrl.get(here);
    const current = self != null ? index.pages[self] : null;
    const outgoing = new Set(current ? current.o : []);
    const incoming = new Set(self != null ? index.incoming[self] : []);
    const profile = new Map();
    const areas = new Map();
    Object.entries(memory.pages).forEach(([url, page]) => {
      const position = index.byUrl.get(url);
      if (position == null) return;
      const entry = index.pages[position];
      const fade = Math.pow(0.5, (now - (page.l || now)) / (14 * 864e5));
      const engagement = Math.min(3, 0.4 + (page.s || 0) / 120 + (page.d || 0) + (page.c ? 0.5 : 0)) * fade * (url === here ? 1.5 : 1);
      entry.k.forEach(([term, weight]) => profile.set(term, (profile.get(term) || 0) + weight * engagement));
      areas.set(entry.a, (areas.get(entry.a) || 0) + engagement);
    });
    Object.entries(memory.terms).forEach(([term, weight]) => profile.set(term, (profile.get(term) || 0) + weight));
    let norm = 0;
    profile.forEach((weight) => {
      norm += weight * weight;
    });
    norm = Math.sqrt(norm);
    const favouriteArea = Array.from(areas.entries()).sort((a, b) => b[1] - a[1])[0];
    const focusId = Object.entries(visit.x || {}).sort((a, b) => b[1] - a[1])[0];
    const focus = focusId && focusId[1] >= 15 ? headings.find((item) => item.id === focusId[0]) : null;
    const focusUrls = focus ? sectionLinks(focus.id) : new Set();
    const queries = memory.queries.slice(-3).reverse();

    const SIMILAR = "similar";
    const ranked = index.pages
      .map((entry, position) => {
        if (position === self) return null;
        const seen = memory.pages[entry.u];
        const finished = seen && (seen.d || 0) >= 0.85;
        const reasons = [];
        const add = (weight, reason) => {
          if (weight > 0) reasons.push([weight, reason]);
        };
        if (norm) {
          let dot = 0;
          let own = 0;
          entry.k.forEach(([term, weight]) => {
            own += weight * weight;
            dot += (profile.get(term) || 0) * weight;
          });
          const similarity = own ? dot / (Math.sqrt(own) * norm) : 0;
          add(similarity * 4, SIMILAR);
        }
        if (focusUrls.has(entry.u)) add(1.6, `Linked from “${focus.text}”, where you spent the most time`);
        else if (outgoing.has(position)) add(1, "Linked from this page");
        if (incoming.has(position)) add(0.5, "Links back to this page");
        const haystack = `${entry.t} ${entry.d}`.toLowerCase();
        queries.some((query) => {
          const terms = termsOf(query);
          if (!terms.length) return false;
          const inTitle = terms.filter((term) => entry.t.toLowerCase().includes(term)).length;
          const inText = terms.filter((term) => haystack.includes(term)).length;
          if (!inText) return false;
          add((inTitle * 1.2 + inText * 0.5) / terms.length, `Matches your search “${query}”`);
          return true;
        });
        if (seen && !finished && (seen.d || 0) > 0.1) add(1.4, `Pick up where you left off, ${Math.round(seen.d * 100)}% read`);
        if (favouriteArea && entry.a === favouriteArea[0]) add(0.35, `More in ${index.areas[entry.a] || "this area"}, your most-read area`);
        let score = reasons.reduce((total, [weight]) => total + weight, 0);
        if (finished) score *= 0.15;
        reasons.sort((a, b) => b[0] - a[0]);
        return score > 0.08 ? { entry, score, reason: reasons[0] ? reasons[0][1] : "", seen } : null;
      })
      .filter(Boolean)
      .sort((a, b) => b.score - a.score)
      .slice(0, limit);
    const read = Object.entries(memory.pages)
      .filter(([url]) => index.byUrl.has(url))
      .sort((a, b) => (b[1].s || 0) + (b[1].d || 0) * 60 - (a[1].s || 0) - (a[1].d || 0) * 60)
      .slice(0, 12)
      .map(([url, page]) => [url, page, new Map(index.pages[index.byUrl.get(url)].k)]);
    ranked.forEach((item) => {
      if (item.reason !== SIMILAR) return;
      let best = null;
      let bestScore = 0;
      read.forEach(([url, page, terms]) => {
        if (url === item.entry.u) return;
        const overlap = item.entry.k.reduce((total, [term, weight]) => total + weight * (terms.get(term) || 0), 0);
        if (overlap > bestScore) {
          best = [url, page];
          bestScore = overlap;
        }
      });
      item.reason = !best || best[0] === here ? "Close to what you are reading" : `Close to “${best[1].t}”, which you read`;
    });
    const top = ranked.length ? ranked[0].score : 1;
    ranked.forEach((item) => {
      item.match = Math.max(0.12, Math.min(1, item.score / top));
    });
    return ranked;
  };

  const suggestionList = (index, items) => {
    const list = make("ol", "dp-suggest-list");
    items.forEach((item, position) => {
      const row = make("li");
      row.style.setProperty("--dp-delay", `${position * 60}ms`);
      const link = make("a", "dp-suggest-card");
      link.href = item.entry.u;
      const meta = make("span", "dp-suggest-meta");
      meta.textContent = [index.areas[item.entry.a], item.entry.m ? `${item.entry.m} min` : ""].filter(Boolean).join(" · ");
      const title = make("strong", "", item.entry.t);
      const why = make("span", "dp-suggest-why", item.reason);
      const meter = make("span", "dp-suggest-meter");
      meter.setAttribute("aria-hidden", "true");
      meter.style.setProperty("--dp-match", item.match.toFixed(2));
      link.append(meta, title, why, meter);
      row.append(link);
      list.append(row);
    });
    return list;
  };

  const forgetButton = () => {
    const button = make("button", "dp-suggest-forget", "Forget my reading");
    button.type = "button";
    button.addEventListener("click", () => {
      memory = blank();
      memory.pages[here] = { t: visit.t, n: 1, s: 0, d: 0, x: {}, f: Date.now(), l: Date.now() };
      try {
        window.localStorage.removeItem(STORE);
      } catch (error) {}
      Object.assign(visit, memory.pages[here]);
      memory.pages[here] = visit;
      refreshSuggestions();
    });
    return button;
  };

  const suggestPanel = (index, headingTag, titleId) => {
    const items = suggest(index);
    if (!items.length) return null;
    const panel = make("div", "dp-suggest-panel");
    const head = make("div", "dp-suggest-head");
    const spark = make("span", "dp-spark");
    spark.append(icon("spark"));
    const title = make(headingTag, "", "Suggested for you");
    if (titleId) title.id = titleId;
    head.append(spark, title, forgetButton());
    const note = make(
      "p",
      "dp-suggest-note",
      "Ranked in your browser from the pages you read, the sections you stay on and what you search. Nothing is sent anywhere.",
    );
    panel.append(head, note, suggestionList(index, items));
    return panel;
  };

  const suggestSection = document.querySelector("[data-suggest]");
  let dockSpark = null;

  function refreshSuggestions() {
    readerIndex().then((index) => {
      if (!index || !suggestSection) return;
      const panel = suggestPanel(index, "h2", "dp-suggest-title");
      suggestSection.textContent = "";
      suggestSection.hidden = !panel;
      if (panel) suggestSection.append(panel);
      if (dockSpark) dockSpark.classList.toggle("has-new", Boolean(panel) && visit.d > 0.5);
      if (sheet.kind === "foryou") openSheet("foryou", null, true);
    });
  }

  const enhanceLinks = () => {
    if (!prose) return;
    prose.querySelectorAll("a[href]").forEach((link) => {
      if (link.classList.contains("heading-anchor") || link.querySelector("img")) return;
      const url = new URL(link.href, window.location.href);
      if (url.origin !== window.location.origin) {
        if (/^https?:$/.test(url.protocol)) link.classList.add("dp-link-ext");
        return;
      }
      if (url.pathname === here && url.hash) {
        link.classList.add("dp-link-jump");
        return;
      }
      if (!url.pathname.startsWith("/docs/") || url.pathname === here) return;
      link.classList.add("dp-link-doc");
      const seen = memory.pages[url.pathname];
      if (seen && (seen.d || 0) >= 0.85) link.classList.add("dp-link-read");
      else if (seen) link.classList.add("dp-link-seen");
    });
  };

  const peek = make("div", "dp-peek");
  peek.setAttribute("role", "tooltip");
  peek.id = "dp-peek";
  peek.hidden = true;
  body.append(peek);
  let peekTimer = 0;
  let peekLink = null;

  const hidePeek = () => {
    window.clearTimeout(peekTimer);
    peek.hidden = true;
    peek.classList.remove("is-open");
    if (peekLink) peekLink.removeAttribute("aria-describedby");
    peekLink = null;
  };

  const showPeek = (link) => {
    window.clearTimeout(peekTimer);
    peekTimer = window.setTimeout(() => {
      readerIndex().then((index) => {
        if (!index || !link.isConnected) return;
        const url = new URL(link.href, window.location.href);
        const position = index.byUrl.get(url.pathname);
        if (position == null) return;
        const entry = index.pages[position];
        const seen = memory.pages[entry.u];
        peek.textContent = "";
        const meta = make("span", "dp-peek-meta");
        meta.textContent = [index.areas[entry.a], entry.m ? `${entry.m} min read` : ""].filter(Boolean).join(" · ");
        const title = make("strong", "", entry.t);
        const description = make("span", "dp-peek-body", entry.d);
        peek.append(meta, title, description);
        if (url.hash) {
          const section = make("span", "dp-peek-section", `Opens at #${decodeURIComponent(url.hash.slice(1))}`);
          peek.append(section);
        }
        if (seen) {
          const state = make("span", "dp-peek-state");
          state.textContent = (seen.d || 0) >= 0.85 ? "You have read this page" : `You read ${Math.round((seen.d || 0) * 100)}% of this page`;
          state.style.setProperty("--dp-read", String(Math.max(0.04, seen.d || 0)));
          peek.append(state);
        }
        peek.hidden = false;
        const box = link.getBoundingClientRect();
        const width = Math.min(340, window.innerWidth - 24);
        peek.style.width = `${width}px`;
        const left = Math.max(12, Math.min(window.innerWidth - width - 12, box.left + box.width / 2 - width / 2));
        const below = box.bottom + 10 + peek.offsetHeight < window.innerHeight;
        peek.style.left = `${left}px`;
        peek.style.top = `${below ? box.bottom + 10 : box.top - peek.offsetHeight - 10}px`;
        peek.dataset.side = below ? "below" : "above";
        peek.classList.add("is-open");
        link.setAttribute("aria-describedby", peek.id);
        peekLink = link;
      });
    }, 260);
  };

  const peekTarget = (event) => {
    const target = event.target instanceof Element ? event.target.closest(".dp-link-doc, .dp-map-node, .dp-suggest-card") : null;
    return target && target.closest(".dp-article") ? target : null;
  };

  document.addEventListener("pointerover", (event) => {
    if (!finePointer.matches) return;
    const link = peekTarget(event);
    if (link && link.classList.contains("dp-link-doc") && link !== peekLink) showPeek(link);
  });
  document.addEventListener("pointerout", (event) => {
    const link = peekTarget(event);
    if (link && (!event.relatedTarget || !link.contains(event.relatedTarget))) hidePeek();
  });
  document.addEventListener("focusin", (event) => {
    const link = peekTarget(event);
    if (link && link.classList.contains("dp-link-doc")) showPeek(link);
  });
  document.addEventListener("focusout", hidePeek);
  window.addEventListener("scroll", () => {
    if (!peek.hidden) hidePeek();
  }, { passive: true });

  const drawers = new Set();
  let drawFrame = 0;
  const redrawAll = () => {
    if (drawFrame) return;
    drawFrame = window.requestAnimationFrame(() => {
      drawFrame = 0;
      drawers.forEach((draw) => draw());
    });
  };
  const resizeWatch = "ResizeObserver" in window ? new ResizeObserver(redrawAll) : null;
  window.addEventListener("resize", redrawAll);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(redrawAll);

  let markerCount = 0;
  const wireLayer = (container) => {
    let layer = container.querySelector(":scope > svg.dp-wires");
    const width = Math.max(container.scrollWidth, container.clientWidth);
    const height = Math.max(container.scrollHeight, container.clientHeight);
    if (!layer) {
      markerCount += 1;
      layer = vector("svg", { class: "dp-wires", "aria-hidden": "true" });
      layer.dataset.marker = `dp-arrow-${markerCount}`;
      container.prepend(layer);
    }
    layer.textContent = "";
    layer.setAttribute("width", width);
    layer.setAttribute("height", height);
    layer.setAttribute("viewBox", `0 0 ${width} ${height}`);
    const defs = vector("defs");
    const marker = vector("marker", {
      id: layer.dataset.marker,
      viewBox: "0 0 10 10",
      refX: 8,
      refY: 5,
      markerWidth: 7,
      markerHeight: 7,
      orient: "auto-start-reverse",
    });
    marker.append(vector("path", { d: "M0 0 10 5 0 10z", class: "dp-wire-head" }));
    defs.append(marker);
    layer.append(defs);
    return layer;
  };

  const boxIn = (container, node) => {
    const outer = container.getBoundingClientRect();
    const inner = node.getBoundingClientRect();
    const left = inner.left - outer.left + container.scrollLeft;
    const top = inner.top - outer.top + container.scrollTop;
    return { left, top, right: left + inner.width, bottom: top + inner.height, x: left + inner.width / 2, y: top + inner.height / 2 };
  };

  const wire = (layer, d, arrow = true, pulse = true) => {
    const path = vector("path", { d, class: "dp-wire" });
    if (arrow) path.setAttribute("marker-end", `url(#${layer.dataset.marker})`);
    layer.append(path);
    if (pulse && !reducedMotion.matches) layer.append(vector("path", { d, class: "dp-wire-pulse" }));
  };

  const wireFlow = (figure) => {
    const canvas = figure.querySelector("[data-flow-canvas]");
    if (!canvas) return;
    const nodes = new Map(
      Array.from(canvas.querySelectorAll("[data-flow-node]")).map((node) => [node.getAttribute("data-flow-node"), node]),
    );
    const edges = Array.from(figure.querySelectorAll(".dp-flow-edges li")).map((item) => [
      item.getAttribute("data-from"),
      item.getAttribute("data-to"),
    ]);
    const draw = () => {
      if (!canvas.isConnected || !canvas.offsetParent) return;
      const layer = wireLayer(canvas);
      edges.forEach(([from, to]) => {
        const a = nodes.get(from);
        const b = nodes.get(to);
        if (!a || !b) return;
        const p = boxIn(canvas, a);
        const q = boxIn(canvas, b);
        if (q.top >= p.bottom - 2) {
          const bend = Math.max(18, (q.top - p.bottom) / 2);
          wire(layer, `M${p.x} ${p.bottom} C${p.x} ${p.bottom + bend} ${q.x} ${q.top - bend} ${q.x} ${q.top - 2}`);
        } else if (Math.abs(q.y - p.y) < 4) {
          const forward = q.left >= p.right;
          const start = forward ? p.right : p.left;
          const end = forward ? q.left - 2 : q.right + 2;
          wire(layer, `M${start} ${p.y} L${end} ${q.y}`);
        } else {
          const side = Math.min(p.left, q.left) - 22;
          wire(layer, `M${p.left} ${p.y} C${side} ${p.y} ${side} ${q.y} ${q.left - 2} ${q.y}`);
        }
      });
    };
    drawers.add(draw);
    if (resizeWatch) resizeWatch.observe(canvas);
    draw();
  };

  const wireMap = (grid) => {
    const hub = grid.querySelector("[data-map-hub]");
    if (!hub) return;
    const draw = () => {
      if (!grid.isConnected || !grid.offsetParent) return;
      const layer = wireLayer(grid);
      const center = boxIn(grid, hub);
      const visible = (node) => node.offsetParent !== null && !node.closest("details:not([open])");
      const incoming = Array.from(grid.querySelectorAll(".dp-map-in [data-map-node]")).filter(visible);
      const outgoing = Array.from(grid.querySelectorAll(".dp-map-out [data-map-node]")).filter(visible);
      const sideBySide = incoming.concat(outgoing).some((node) => {
        const box = boxIn(grid, node);
        return box.right < center.left || box.left > center.right;
      });
      if (sideBySide) {
        incoming.forEach((node) => {
          const p = boxIn(grid, node);
          const mid = (p.right + center.left) / 2;
          wire(layer, `M${p.right} ${p.y} C${mid} ${p.y} ${mid} ${center.y} ${center.left - 2} ${center.y}`);
        });
        outgoing.forEach((node) => {
          const q = boxIn(grid, node);
          const mid = (center.right + q.left) / 2;
          wire(layer, `M${center.right} ${center.y} C${mid} ${center.y} ${mid} ${q.y} ${q.left - 2} ${q.y}`);
        });
        return;
      }
      const all = incoming.concat(outgoing);
      if (!all.length) return;
      const spine = Math.max(10, center.left - 16);
      const first = boxIn(grid, all[0]);
      const last = boxIn(grid, all[all.length - 1]);
      wire(layer, `M${spine} ${Math.min(first.y, center.y)} L${spine} ${Math.max(last.y, center.y)}`, false, true);
      all.forEach((node) => {
        const box = boxIn(grid, node);
        wire(layer, `M${spine} ${box.y} L${box.left - 2} ${box.y}`, true, false);
      });
      layer.append(vector("circle", { cx: spine, cy: center.y, r: 6, class: "dp-wire-hub" }));
    };
    drawers.add(draw);
    if (resizeWatch) resizeWatch.observe(grid);
    grid.querySelectorAll("details").forEach((details) => details.addEventListener("toggle", redrawAll));
    draw();
  };

  document.querySelectorAll(".dp-flow").forEach(wireFlow);
  document.querySelectorAll("[data-map]").forEach(wireMap);

  const viewer = make("div", "dp-viewer");
  viewer.hidden = true;
  viewer.setAttribute("role", "dialog");
  viewer.setAttribute("aria-modal", "true");
  viewer.setAttribute("aria-label", "Expanded figure");
  const viewerBar = make("div", "dp-viewer-bar");
  const viewerTitle = make("strong", "dp-viewer-title");
  const viewerZoom = make("span", "dp-viewer-zoom");
  const zoomOut = iconButton("minus", "Zoom out");
  const zoomIn = iconButton("plus", "Zoom in");
  const zoomFit = iconButton("fit", "Fit to screen");
  const viewerClose = iconButton("close", "Close");
  viewerBar.append(viewerTitle, zoomOut, viewerZoom, zoomIn, zoomFit, viewerClose);
  const viewerStage = make("div", "dp-viewer-stage");
  viewer.append(viewerBar, viewerStage);
  body.append(viewer);
  let viewerScale = 1;
  let viewerReturn = null;
  let viewerFlow = null;

  const applyZoom = () => {
    viewerStage.style.setProperty("--dp-zoom", viewerScale.toFixed(3));
    viewerZoom.textContent = `${Math.round(viewerScale * 100)}%`;
    if (viewerFlow) redrawAll();
  };

  const fitZoom = () => {
    const content = viewerStage.firstElementChild;
    if (!content) return;
    viewerScale = 1;
    applyZoom();
    const ratio = (viewerStage.clientWidth - 32) / Math.max(1, content.scrollWidth);
    viewerScale = Math.max(0.6, Math.min(2.5, ratio < 1 ? ratio : 1));
    applyZoom();
  };

  const closeViewer = () => {
    if (viewer.hidden) return;
    viewer.hidden = true;
    root.classList.remove("dp-locked");
    if (viewerFlow) drawers.delete(viewerFlow);
    viewerFlow = null;
    viewerStage.textContent = "";
    if (viewerReturn) viewerReturn.focus();
  };

  const openViewer = (figure, opener) => {
    const source = figure.querySelector(".dp-diagram-canvas, .dp-flow-canvas, .dp-data-view");
    if (!source) return;
    const copy = source.cloneNode(true);
    copy.querySelectorAll("svg.dp-wires").forEach((layer) => layer.remove());
    viewerStage.textContent = "";
    viewerStage.append(copy);
    viewerTitle.textContent = (figure.querySelector("[data-figure-tools]") || {}).dataset?.figureLabel || "Figure";
    viewer.dataset.kind = figure.classList.contains("dp-data") ? "data" : "diagram";
    viewer.hidden = false;
    root.classList.add("dp-locked");
    viewerReturn = opener;
    if (figure.classList.contains("dp-flow")) {
      const shadow = make("figure", "dp-flow");
      copy.replaceWith(shadow);
      shadow.append(copy);
      figure.querySelectorAll(".dp-flow-edges").forEach((edges) => shadow.append(edges.cloneNode(true)));
      wireFlow(shadow);
      viewerFlow = Array.from(drawers).pop();
    }
    if (viewer.dataset.kind === "diagram") fitZoom();
    else {
      viewerScale = 1;
      applyZoom();
    }
    viewerClose.focus();
  };

  zoomOut.addEventListener("click", () => {
    viewerScale = Math.max(0.35, viewerScale / 1.2);
    applyZoom();
  });
  zoomIn.addEventListener("click", () => {
    viewerScale = Math.min(3, viewerScale * 1.2);
    applyZoom();
  });
  zoomFit.addEventListener("click", fitZoom);
  viewerClose.addEventListener("click", closeViewer);

  const copyText = async (text) => {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (error) {
      return false;
    }
  };

  document.querySelectorAll(".dp-figure").forEach((figure) => {
    const tools = figure.querySelector("[data-figure-tools]");
    if (!tools) return;
    const code = figure.querySelector("pre code");
    if (code && !figure.classList.contains("dp-diagram")) {
      const copy = iconButton("copy", "Copy source");
      copy.setAttribute("data-figure-copy", "");
      copy.addEventListener("click", async () => {
        const done = await copyText(code.textContent.replace(/\n$/, ""));
        copy.classList.toggle("is-done", done);
        window.setTimeout(() => copy.classList.remove("is-done"), 1400);
      });
      tools.append(copy);
    }
    const expand = iconButton("expand", "Expand");
    expand.addEventListener("click", () => openViewer(figure, expand));
    tools.append(expand);
    const canvas = figure.querySelector(".dp-diagram-canvas, .dp-flow-canvas");
    if (canvas) {
      const check = () => figure.classList.toggle("is-overflowing", canvas.scrollWidth > canvas.clientWidth + 2);
      drawers.add(check);
      if (resizeWatch) resizeWatch.observe(canvas);
      check();
    }
  });

  const dock = make("nav", "dp-dock");
  dock.setAttribute("aria-label", "Documentation navigation");
  const dockBar = make("div", "dp-dock-bar");
  dock.append(dockBar);
  const dockButton = (name, label, kind) => {
    const button = make("button", "dp-dock-button");
    button.type = "button";
    button.dataset.kind = kind;
    button.append(icon(name), make("span", "dp-dock-label", label));
    dockBar.append(button);
    return button;
  };
  const outlineLinks = Array.from(document.querySelectorAll(".dp-outline .dp-outline-link"));
  const libraryButton = dockButton("library", "Library", "library");
  const sectionsButton = outlineLinks.length ? dockButton("sections", "Sections", "sections") : null;
  const searchButton = dockButton("search", "Search", "search");
  dockSpark = dockButton("spark", "For you", "foryou");
  const topButton = make("button", "dp-dock-button dp-dock-top");
  topButton.type = "button";
  topButton.setAttribute("aria-label", "Back to top");
  const ring = vector("svg", { viewBox: "0 0 36 36", class: "dp-dock-ring", "aria-hidden": "true" });
  ring.append(vector("circle", { cx: 18, cy: 18, r: 15, class: "dp-ring-track" }));
  const ringValue = vector("circle", { cx: 18, cy: 18, r: 15, class: "dp-ring-value", pathLength: 100 });
  ring.append(ringValue);
  topButton.append(ring, icon("up"));
  dockBar.append(topButton);
  [libraryButton, sectionsButton, dockSpark].forEach((button) => {
    if (!button) return;
    button.setAttribute("aria-haspopup", "dialog");
    button.setAttribute("aria-expanded", "false");
    button.setAttribute("aria-controls", "dp-sheet");
  });
  body.append(dock);

  const hud = make("div", "dp-hud");
  hud.setAttribute("aria-hidden", "true");
  const hudSection = make("span", "dp-hud-section");
  const hudPercent = make("span", "dp-hud-percent");
  hud.append(hudSection, hudPercent);
  body.append(hud);

  const layer = make("div", "dp-sheet-layer");
  layer.hidden = true;
  const backdrop = make("div", "dp-sheet-backdrop");
  const panel = make("section", "dp-sheet");
  panel.id = "dp-sheet";
  panel.setAttribute("role", "dialog");
  panel.setAttribute("aria-modal", "true");
  panel.setAttribute("aria-labelledby", "dp-sheet-title");
  const sheetHead = make("header", "dp-sheet-head");
  const grip = make("span", "dp-sheet-grip");
  grip.setAttribute("aria-hidden", "true");
  const sheetTitle = make("h2", "dp-sheet-title");
  sheetTitle.id = "dp-sheet-title";
  const sheetClose = iconButton("close", "Close", "dp-sheet-close");
  sheetHead.append(grip, sheetTitle, sheetClose);
  const sheetBody = make("div", "dp-sheet-body");
  panel.append(sheetHead, sheetBody);
  layer.append(backdrop, panel);
  body.append(layer);

  const sheet = { kind: "", opener: null, nav: null, home: null, next: null };
  const navigation = document.querySelector(".dp-nav nav");

  const restoreNav = () => {
    if (sheet.nav && sheet.home) {
      sheet.home.insertBefore(sheet.nav, sheet.next);
      sheet.nav.querySelectorAll(".dp-nav-link, .dp-nav-group").forEach((node) => {
        node.hidden = false;
      });
    }
    sheet.nav = null;
  };

  const libraryBody = () => {
    const wrap = make("div", "dp-sheet-library");
    const field = make("label", "dp-sheet-filter");
    field.append(icon("search"));
    const input = make("input");
    input.type = "search";
    input.placeholder = "Filter the library…";
    input.autocomplete = "off";
    input.setAttribute("aria-label", "Filter the library");
    field.append(input);
    wrap.append(field);
    if (navigation) {
      sheet.home = navigation.parentNode;
      sheet.next = navigation.nextSibling;
      sheet.nav = navigation;
      wrap.append(navigation);
      const groups = Array.from(navigation.querySelectorAll(".dp-nav-group"));
      const opened = new Map(groups.map((group) => [group, group.open]));
      input.addEventListener("input", () => {
        const terms = termsOf(input.value);
        groups.forEach((group) => {
          let any = false;
          group.querySelectorAll(".dp-nav-link").forEach((link) => {
            const text = `${link.textContent} ${link.getAttribute("data-search-body") || ""}`.toLowerCase();
            const match = terms.every((term) => text.includes(term));
            link.hidden = !match;
            any = any || match;
          });
          group.hidden = !any;
          group.open = terms.length ? any : opened.get(group);
        });
      });
      window.setTimeout(() => {
        const active = navigation.querySelector(".dp-nav-link.active");
        if (active) active.scrollIntoView({ block: "center" });
      }, 30);
    }
    return wrap;
  };

  const sectionsBody = () => {
    const list = make("nav", "dp-sheet-sections");
    list.setAttribute("aria-label", "On this page");
    const active = document.querySelector(".dp-outline .dp-outline-link.active");
    outlineLinks.forEach((link) => {
      const copy = link.cloneNode(true);
      copy.classList.remove("active", "active-parent");
      const id = decodeURIComponent((link.getAttribute("href") || "").slice(1));
      const seconds = visit.x[id] || 0;
      if (seconds >= 10) {
        copy.dataset.read = "";
        copy.title = `You spent ${seconds < 60 ? `${seconds} seconds` : `${Math.round(seconds / 60)} min`} here`;
      }
      if (link === active) {
        copy.classList.add("active");
        copy.setAttribute("aria-current", "location");
      }
      copy.addEventListener("click", () => closeSheet(false));
      list.append(copy);
    });
    window.setTimeout(() => {
      const current = list.querySelector(".active");
      if (current) current.scrollIntoView({ block: "center" });
    }, 30);
    return list;
  };

  const forYouBody = () => {
    const wrap = make("div", "dp-sheet-foryou");
    wrap.append(make("p", "dp-sheet-loading", "Reading the library…"));
    readerIndex().then((index) => {
      if (sheet.kind !== "foryou") return;
      wrap.textContent = "";
      const content = index ? suggestPanel(index, "h3", "") : null;
      if (content) wrap.append(content);
      else wrap.append(make("p", "dp-sheet-loading", "Read a little more and suggestions will appear here."));
      const recent = Object.entries(memory.pages)
        .filter(([url]) => url !== here)
        .sort((a, b) => (b[1].l || 0) - (a[1].l || 0))
        .slice(0, 4);
      if (recent.length) {
        const heading = make("h3", "dp-sheet-subtitle", "Recently read");
        const list = make("ul", "dp-sheet-recent");
        recent.forEach(([url, page]) => {
          const item = make("li");
          const link = make("a", "", page.t || url);
          link.href = url;
          const bar = make("span", "dp-sheet-recent-bar");
          bar.style.setProperty("--dp-read", String(Math.max(0.04, page.d || 0)));
          bar.setAttribute("aria-hidden", "true");
          item.append(link, bar);
          list.append(item);
        });
        wrap.append(heading, list);
      }
    });
    return wrap;
  };

  const SHEETS = {
    library: ["All documentation", libraryBody],
    sections: ["On this page", sectionsBody],
    foryou: ["For you", forYouBody],
  };

  function openSheet(kind, opener, refresh = false) {
    if (!SHEETS[kind]) return;
    if (!refresh && sheet.kind === kind) {
      closeSheet(true);
      return;
    }
    restoreNav();
    sheet.kind = kind;
    if (opener) sheet.opener = opener;
    sheetTitle.textContent = SHEETS[kind][0];
    sheetBody.textContent = "";
    sheetBody.append(SHEETS[kind][1]());
    panel.dataset.kind = kind;
    panel.style.transform = "";
    layer.hidden = false;
    root.classList.add("dp-locked");
    window.requestAnimationFrame(() => layer.classList.add("is-open"));
    dock.querySelectorAll("[aria-expanded]").forEach((button) => {
      button.setAttribute("aria-expanded", button.dataset.kind === kind ? "true" : "false");
      button.classList.toggle("is-active", button.dataset.kind === kind);
    });
    if (kind === "foryou" && dockSpark) dockSpark.classList.remove("has-new");
    if (!refresh) sheetClose.focus();
  }

  function closeSheet(returnFocus = true) {
    if (!sheet.kind) return;
    sheet.kind = "";
    layer.classList.remove("is-open");
    root.classList.remove("dp-locked");
    dock.querySelectorAll("[aria-expanded]").forEach((button) => {
      button.setAttribute("aria-expanded", "false");
      button.classList.remove("is-active");
    });
    const finish = () => {
      if (sheet.kind) return;
      layer.hidden = true;
      restoreNav();
      sheetBody.textContent = "";
    };
    if (reducedMotion.matches) finish();
    else window.setTimeout(finish, 220);
    if (returnFocus && sheet.opener) sheet.opener.focus();
  }

  dock.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest(".dp-dock-button") : null;
    if (!button) return;
    if (button === topButton) {
      closeSheet(false);
      window.scrollTo({ top: 0, behavior: reducedMotion.matches ? "auto" : "smooth" });
      return;
    }
    if (button === searchButton) {
      closeSheet(false);
      body.classList.remove("dp-toolbar-tucked");
      if (search) {
        search.focus();
        search.select();
      }
      return;
    }
    openSheet(button.dataset.kind, button);
  });
  backdrop.addEventListener("click", () => closeSheet(true));
  sheetClose.addEventListener("click", () => closeSheet(true));

  let dragStart = null;
  sheetHead.addEventListener("pointerdown", (event) => {
    if (event.target instanceof Element && event.target.closest("button")) return;
    dragStart = { y: event.clientY, id: event.pointerId };
    sheetHead.setPointerCapture(event.pointerId);
    panel.classList.add("is-dragging");
  });
  sheetHead.addEventListener("pointermove", (event) => {
    if (!dragStart || event.pointerId !== dragStart.id) return;
    panel.style.transform = `translateY(${Math.max(0, event.clientY - dragStart.y)}px)`;
  });
  const endDrag = (event) => {
    if (!dragStart || event.pointerId !== dragStart.id) return;
    const distance = event.clientY - dragStart.y;
    dragStart = null;
    panel.classList.remove("is-dragging");
    panel.style.transform = "";
    if (distance > 90) closeSheet(true);
  };
  sheetHead.addEventListener("pointerup", endDrag);
  sheetHead.addEventListener("pointercancel", endDrag);

  const focusables = (container) =>
    Array.from(container.querySelectorAll("a[href], button, input, summary, [tabindex]:not([tabindex='-1'])")).filter(
      (node) => !node.hidden && node.offsetParent !== null,
    );

  document.addEventListener("keydown", (event) => {
    const container = !viewer.hidden ? viewer : sheet.kind ? panel : null;
    if (!container) return;
    if (event.key === "Escape") {
      event.preventDefault();
      if (container === viewer) closeViewer();
      else closeSheet(true);
      return;
    }
    if (event.key !== "Tab") return;
    const items = focusables(container);
    if (!items.length) return;
    const first = items[0];
    const last = items[items.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  });

  narrow.addEventListener("change", () => {
    if (!narrow.matches) closeSheet(false);
  });

  document.addEventListener("focusin", (event) => {
    const typing = event.target instanceof Element && event.target.matches("input, textarea, select");
    body.classList.toggle("dp-typing", typing && !event.target.closest(".dp-sheet"));
  });
  document.addEventListener("focusout", () => body.classList.remove("dp-typing"));

  let lastY = window.scrollY;
  let hudTimer = 0;
  let scrollFrame = 0;
  const onScroll = () => {
    if (scrollFrame) return;
    scrollFrame = window.requestAnimationFrame(() => {
      scrollFrame = 0;
      const y = window.scrollY;
      const doc = document.documentElement;
      const span = doc.scrollHeight - window.innerHeight;
      const overall = span > 0 ? Math.min(1, y / span) : 0;
      ringValue.style.strokeDashoffset = String(100 - overall * 100);
      topButton.classList.toggle("is-away", y > window.innerHeight * 0.6);
      const depth = readingDepth();
      if (depth > (visit.d || 0) + 0.02) {
        visit.d = Math.round(depth * 100) / 100;
        remember();
        if (visit.d >= 0.6 && dockSpark && !dockSpark.dataset.nudged) {
          dockSpark.dataset.nudged = "1";
          refreshSuggestions();
        }
      }
      const searching = document.activeElement === search;
      if (narrow.matches && !searching && !sheet.kind) {
        if (y > 160 && y - lastY > 6) body.classList.add("dp-toolbar-tucked");
        else if (lastY - y > 6 || y < 80) body.classList.remove("dp-toolbar-tucked");
      }
      if (narrow.matches && Math.abs(y - lastY) > 2 && headings.length) {
        const section = currentHeading();
        hudSection.textContent = section ? section.text : pageTitle();
        hudPercent.textContent = `${Math.round(overall * 100)}%`;
        hud.classList.add("is-visible");
        window.clearTimeout(hudTimer);
        hudTimer = window.setTimeout(() => hud.classList.remove("is-visible"), 1100);
      }
      lastY = y;
    });
  };
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  enhanceLinks();
  const idle = window.requestIdleCallback || ((callback) => window.setTimeout(callback, 1200));
  idle(() => refreshSuggestions(), { timeout: 3000 });
})();

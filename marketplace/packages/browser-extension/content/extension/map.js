(() => {
  const $ = (selector) => document.querySelector(selector);
  const DARK = ["#3987e5", "#199e70", "#c98500", "#008300", "#9085e9", "#e66767", "#d55181", "#d95926"];
  const LIGHT = ["#2a78d6", "#1baf7a", "#eda100", "#008300", "#4a3aa7", "#e34948", "#e87ba4", "#eb6834"];
  const GRAY = "#8a8a8a";
  const VIEW = 720;
  const HOLE = 112;
  const WORDS = {
    provider_rate_limited: "The provider is rate-limiting requests from this address. Try again in a few minutes.",
    provider_unavailable: "The provider did not answer. Try again in a moment.",
    provider_unreachable: "The provider could not be reached.",
    repository_not_found_or_authorization_required: "The provider says this repository does not exist or is private.",
    map_unsupported_provider: "GitLab's public API does not list file sizes, so maps are drawn for GitHub, Codeberg and Gitea repositories.",
    not_a_repository_page: "This address is not a GitHub, Codeberg or Gitea repository.",
  };

  const params = new URLSearchParams(location.search);
  const page = self.ForkMeshRepoBackground.repoPage(params.get("url"));
  const dark = matchMedia("(prefers-color-scheme: dark)").matches;
  const palette = dark ? DARK : LIGHT;
  let data = null;
  let colors = new Map();
  let focus = null;
  let rings = 4;
  let segments = [];
  let view = params.get("view") === "code" ? "code" : "size";

  const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[char]));
  const number = (value) => Number(value || 0).toLocaleString();

  function size(bytes) {
    const n = Number(bytes) || 0;
    if (n < 1024) return `${n} B`;
    if (n < 1024 ** 2) return `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KB`;
    if (n < 1024 ** 3) return `${(n / 1024 ** 2).toFixed(n < 10 * 1024 ** 2 ? 1 : 0)} MB`;
    return `${(n / 1024 ** 3).toFixed(1)} GB`;
  }

  const share = (part, whole) => (whole > 0 ? `${((part / whole) * 100).toFixed(part / whole < 0.1 ? 1 : 0)}%` : "");

  function state(name, text = "") {
    document.querySelectorAll("[data-map-state]").forEach((node) => { node.hidden = node.dataset.mapState !== name; });
    if (name === "error") $("[data-map-error]").textContent = text;
    if (name === "loading") $("[data-map-loading]").textContent = text || "Reading the file tree…";
  }

  function shade(hex, depth, extra) {
    const toward = dark ? 255 : 0;
    const f = Math.min(0.55, Math.max(0, (depth - 1) * 0.16 + extra));
    const channel = (i) => {
      const v = parseInt(hex.slice(1 + i * 2, 3 + i * 2), 16);
      return Math.round(v + (toward - v) * f);
    };
    return `rgb(${channel(0)} ${channel(1)} ${channel(2)})`;
  }

  function paint(root) {
    const out = new Map();
    const walk = (node, depth, base, index) => {
      const hue = depth === 1 ? (index < palette.length ? palette[index] : GRAY) : base;
      out.set(node, shade(hue, depth, depth > 1 ? (index % 2) * 0.06 : 0));
      (node.children || []).forEach((child, i) => walk(child, depth + 1, hue, i));
    };
    (root.children || []).forEach((child, i) => walk(child, 1, GRAY, i));
    return out;
  }

  function parents(node) {
    const chain = [];
    for (let at = node; at; at = at.parent) chain.unshift(at);
    return chain;
  }

  function link(root) {
    const walk = (node) => (node.children || []).forEach((child) => { child.parent = node; walk(child); });
    walk(root);
  }

  function ring(focusNode) {
    const out = [];
    const minSpan = (2 * Math.PI) / 900;
    const walk = (node, depth, from, span) => {
      if (depth > rings || !(node.size > 0)) return;
      let at = from;
      (node.children || []).forEach((child) => {
        const childSpan = span * Math.min(1, child.size / node.size);
        if (child.size > 0 && childSpan >= minSpan) {
          out.push({ node: child, depth, a0: at, a1: at + childSpan });
          walk(child, depth + 1, at, childSpan);
        }
        at += childSpan;
      });
    };
    walk(focusNode, 1, -Math.PI / 2, 2 * Math.PI);
    return out;
  }

  function arc(c, r0, r1, a0, a1) {
    const span = Math.min(a1 - a0, 2 * Math.PI - 0.0004);
    const end = a0 + span;
    const large = span > Math.PI ? 1 : 0;
    const px = (r, a) => (c + r * Math.cos(a)).toFixed(2);
    const py = (r, a) => (c + r * Math.sin(a)).toFixed(2);
    return `M ${px(r1, a0)} ${py(r1, a0)} A ${r1} ${r1} 0 ${large} 1 ${px(r1, end)} ${py(r1, end)} ` +
      `L ${px(r0, end)} ${py(r0, end)} A ${r0} ${r0} 0 ${large} 0 ${px(r0, a0)} ${py(r0, a0)} Z`;
  }

  function relative(node) {
    const base = focus.path ? `${focus.path}/` : "";
    return node.path.startsWith(base) ? node.path.slice(base.length) : node.path;
  }

  function drawChart() {
    const c = VIEW / 2;
    const width = (c - 8 - HOLE) / rings;
    const gap = getComputedStyle(document.body).getPropertyValue("--surface").trim() || (dark ? "#0d1117" : "#fff");
    segments = ring(focus);
    const parts = segments.map((seg, i) => {
      const r0 = HOLE + (seg.depth - 1) * width;
      const verb = seg.node.type === "file" ? "Open file" : "Zoom into folder";
      return `<path data-seg="${i}" d="${arc(c, r0, r0 + width, seg.a0, seg.a1)}" fill="${colors.get(seg.node) || GRAY}" ` +
        `stroke="${gap}" stroke-width="1.5" tabindex="0" role="button" aria-label="${escapeHtml(`${verb} ${relative(seg.node)}, ${size(seg.node.size)}`)}"></path>`;
    });
    segments.forEach((seg) => {
      const r = HOLE + (seg.depth - 0.5) * width;
      if ((seg.a1 - seg.a0) * r < 64 || width < 34) return;
      const mid = (seg.a0 + seg.a1) / 2;
      const x = (c + r * Math.cos(mid)).toFixed(1);
      const y = (c + r * Math.sin(mid)).toFixed(1);
      const name = seg.node.name.length > 16 ? `${seg.node.name.slice(0, 15)}…` : seg.node.name;
      parts.push(`<text x="${x}" y="${y}" text-anchor="middle" fill="#fff" font-size="12" ` +
        `style="pointer-events:none;paint-order:stroke;stroke:rgb(0 0 0 / .5);stroke-width:2.5px">` +
        `<tspan x="${x}" dy="-0.15em" font-weight="600">${escapeHtml(name)}</tspan>` +
        `<tspan x="${x}" dy="1.2em" font-size="11">${escapeHtml(size(seg.node.size))}</tspan></text>`);
    });
    const up = Boolean(focus.parent);
    const title = focus.name || page.name;
    parts.push(`<circle data-center cx="${c}" cy="${c}" r="${HOLE - 3}" fill="transparent"` +
      `${up ? ' tabindex="0" role="button" aria-label="Zoom out one folder"' : ""}></circle>`);
    parts.push(`<text x="${c}" y="${c}" text-anchor="middle" style="pointer-events:none">` +
      `<tspan x="${c}" dy="-0.4em" fill="currentColor" font-size="17" font-weight="650">${escapeHtml(title.length > 18 ? `${title.slice(0, 17)}…` : title)}</tspan>` +
      `<tspan x="${c}" dy="1.45em" style="fill:var(--muted)" font-size="13">${escapeHtml(size(focus.size))}</tspan>` +
      (up ? `<tspan x="${c}" dy="1.5em" style="fill:var(--accent)" font-size="11">↑ up</tspan>` : "") + `</text>`);
    $("[data-map-chart]").innerHTML = `<svg viewBox="0 0 ${VIEW} ${VIEW}" role="img" aria-label="Folder size sunburst of ${escapeHtml(page.fullName)}">${parts.join("")}</svg>`;
  }

  function drawCrumbs() {
    const chain = parents(focus);
    $("[data-map-crumbs]").innerHTML = chain.map((node, i) =>
      `${i ? "<span>/</span>" : ""}<button type="button" data-crumb="${i}"${node === focus ? ' aria-current="true"' : ""}>${escapeHtml(node.name || page.name)}</button>`).join("");
    $("[data-map-crumbs]").querySelectorAll("[data-crumb]").forEach((button) => {
      button.addEventListener("click", () => zoom(chain[Number(button.dataset.crumb)]));
    });
    $("[data-map-up]").disabled = !focus.parent;
  }

  function subtree(node) {
    const files = [];
    let folders = 0;
    const stack = [node];
    while (stack.length) {
      const at = stack.pop();
      if (at.type === "file") files.push(at);
      else {
        if (at !== node) folders += 1;
        stack.push(...(at.children || []));
      }
    }
    return { files, folders };
  }

  function row(node, label, value, part, whole, color, key) {
    const width = whole > 0 ? Math.max(1, (part / whole) * 100).toFixed(1) : 0;
    return `<li><button type="button" data-row="${escapeHtml(key)}" title="${escapeHtml(node ? node.path || page.fullName : label)}">` +
      `<i style="background:${color}"></i><span>${escapeHtml(label)}</span><em>${escapeHtml(value)}</em>` +
      `<u><b style="width:${width}%"></b></u></button></li>`;
  }

  function drawSide() {
    const { files, folders } = subtree(focus);
    $("[data-map-focus-size]").textContent = size(focus.size);
    $("[data-map-stats]").innerHTML = [["Size", size(focus.size)], ["Files", number(files.length)], ["Folders", number(folders)]]
      .map(([label, value]) => `<div><dt>${label}</dt><dd>${escapeHtml(value)}</dd></div>`).join("");
    const kids = (focus.children || []).slice(0, 60);
    const children = $("[data-map-children]");
    children.innerHTML = kids.map((node, i) => row(node, `${node.name}${node.type === "dir" ? "/" : ""}`,
      `${size(node.size)} · ${share(node.size, focus.size)}`, node.size, focus.size, colors.get(node) || GRAY, `child:${i}`)).join("")
      || '<li class="muted">This folder is empty.</li>';
    children.querySelectorAll("[data-row]").forEach((button) => {
      button.addEventListener("click", () => activate(kids[Number(button.dataset.row.split(":")[1])]));
    });
    const largest = files.sort((a, b) => b.size - a.size).slice(0, 15);
    const top = largest[0]?.size || 0;
    const list = $("[data-map-largest]");
    list.innerHTML = largest.map((node, i) => row(node, relative(node), size(node.size), node.size, top,
      colors.get(node) || GRAY, `file:${i}`)).join("");
    list.querySelectorAll("[data-row]").forEach((button) => {
      button.addEventListener("click", () => activate(largest[Number(button.dataset.row.split(":")[1])]));
    });
    const languageOf = self.ForkMeshCodeMap?.languageOf || ((name) => name.split(".").pop());
    const tally = new Map();
    files.forEach((node) => { const lang = languageOf(node.name); tally.set(lang, (tally.get(lang) || 0) + node.size); });
    const types = [...tally.entries()].sort((a, b) => b[1] - a[1]).slice(0, 10);
    $("[data-map-types]").innerHTML = types.map(([lang, bytes], i) => row(null, lang,
      `${size(bytes)} · ${share(bytes, focus.size)}`, bytes, types[0][1], palette[i] || GRAY, `type:${i}`)).join("");
  }

  function zoom(node) {
    if (!node || node.type !== "dir") return;
    focus = node;
    hideTip();
    drawChart();
    drawCrumbs();
    drawSide();
  }

  function openUrl(url) { window.open(url, "_blank", "noopener"); }

  function activate(node) {
    if (!node) return;
    if (node.type === "file") openUrl(data.webUrl(node.path));
    else zoom(node);
  }

  function hideTip() {
    $("[data-map-tip]").hidden = true;
    $("[data-map-chart]").classList.remove("hovering");
  }

  function showTip(seg, event) {
    const chart = $("[data-map-chart]");
    const chain = new Set(parents(seg.node));
    chart.classList.add("hovering");
    chart.querySelectorAll("path[data-seg]").forEach((path) => {
      path.classList.toggle("lit", chain.has(segments[Number(path.dataset.seg)].node));
    });
    const tip = $("[data-map-tip]");
    const count = seg.node.type === "dir" ? ` · ${number(subtree(seg.node).files.length)} files` : "";
    tip.innerHTML = `${escapeHtml(relative(seg.node))}${seg.node.type === "dir" ? "/" : ""}` +
      `<small>${escapeHtml(size(seg.node.size))} · ${escapeHtml(share(seg.node.size, focus.size))} of ${escapeHtml(focus.name || page.name)}${count}</small>`;
    tip.hidden = false;
    const box = chart.parentElement.getBoundingClientRect();
    const x = event.clientX - box.left + 14;
    const y = event.clientY - box.top + 14;
    tip.style.left = `${Math.min(x, box.width - tip.offsetWidth - 8)}px`;
    tip.style.top = `${Math.min(y, box.height - tip.offsetHeight - 8)}px`;
  }

  function bindChart() {
    const chart = $("[data-map-chart]");
    const segOf = (target) => {
      const path = target.closest?.("path[data-seg]");
      return path ? segments[Number(path.dataset.seg)] : null;
    };
    chart.addEventListener("pointermove", (event) => {
      const seg = segOf(event.target);
      if (seg) showTip(seg, event); else hideTip();
    });
    chart.addEventListener("pointerleave", hideTip);
    chart.addEventListener("click", (event) => {
      const seg = segOf(event.target);
      if (seg) activate(seg.node);
      else if (event.target.closest?.("[data-center]") && focus.parent) zoom(focus.parent);
    });
    chart.addEventListener("keydown", (event) => {
      if (event.key !== "Enter" && event.key !== " ") return;
      const seg = segOf(event.target);
      if (!seg && !event.target.closest?.("[data-center]")) return;
      event.preventDefault();
      if (seg) activate(seg.node); else if (focus.parent) zoom(focus.parent);
    });
    $("[data-map-up]").addEventListener("click", () => focus.parent && zoom(focus.parent));
    document.querySelectorAll("[data-rings]").forEach((button) => {
      button.addEventListener("click", () => {
        rings = Number(button.dataset.rings);
        document.querySelectorAll("[data-rings]").forEach((other) => other.setAttribute("aria-pressed", String(other === button)));
        drawChart();
      });
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Backspace" && view === "size" && focus?.parent && !event.target.closest?.("input")) zoom(focus.parent);
    });
  }

  function setView(next) {
    view = next;
    document.querySelectorAll("[data-map-view]").forEach((button) => {
      button.setAttribute("aria-pressed", String(button.dataset.mapView === next));
    });
    document.title = `${page.fullName} · ${next === "code" ? "Code map" : "Size map"} · ForkMesh`;
    if (next === "code") openCodeMap();
  }

  function openCodeMap() {
    if (!data || !self.ForkMeshCodeMap?.open) return;
    const handle = self.ForkMeshCodeMap.open({
      title: page.fullName,
      root: data.root,
      theme: dark ? "dark" : "light",
      formatSize: size,
      fetchBlobs: (paths) => self.ForkMeshRepoMap.readTexts(paths, data.rawUrl),
      fetchRaw: (path) => self.ForkMeshRepoMap.readBlob(data.rawUrl(path)),
      rawUrl: data.rawUrl,
      openFile: (path) => openUrl(data.webUrl(path)),
      openFolder: (path) => openUrl(data.webUrl(path, true)),
      onClose: () => { if (view === "code") setView("size"); },
    });
    if (focus?.path) handle.goToPath(focus.path);
  }

  async function load() {
    if (!page) { state("error", WORDS.not_a_repository_page); return; }
    state("loading", `Reading ${page.fullName}'s file tree from ${page.providerLabel}…`);
    try {
      data = await self.ForkMeshRepoMap.load(page, params.get("branch") || "");
    } catch (error) {
      state("error", WORDS[error?.code] || error?.message || "Something went wrong.");
      return;
    }
    link(data.root);
    colors = paint(data.root);
    focus = data.root;
    $("[data-map-meta]").textContent = `${size(data.root.size)} · ${number(data.root.fileCount)} files · ${data.branch} @ ${data.commit.slice(0, 7)}`;
    const truncated = $("[data-map-truncated]");
    truncated.hidden = !data.truncated;
    truncated.textContent = `${page.providerLabel} listed only part of this repository's tree, so the maps show the part it listed.`;
    state(data.root.fileCount ? "ready" : "error", data.root.fileCount ? "" : "This repository has no files yet.");
    if (!data.root.fileCount) return;
    zoom(focus);
    setView(view);
  }

  function start() {
    $("[data-map-name]").textContent = page ? page.fullName : "Repository";
    if (page) {
      const chip = $("[data-map-provider]");
      chip.textContent = page.providerLabel;
      chip.hidden = false;
      const source = $("[data-map-source]");
      source.href = page.canonicalUrl;
      source.textContent = `Open on ${page.providerLabel} ↗`;
      source.hidden = false;
      $("[data-map-provider-name]").textContent = page.providerLabel;
    }
    document.querySelectorAll("[data-map-view]").forEach((button) => {
      button.addEventListener("click", () => {
        if (button.dataset.mapView === "code") setView("code");
        else if (view === "code") self.ForkMeshCodeMap?.current?.close();
      });
    });
    $("[data-map-retry]").addEventListener("click", () => void load());
    bindChart();
    void load();
  }

  start();
})();

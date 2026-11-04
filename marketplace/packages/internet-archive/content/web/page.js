window.ForkMeshDashboard.registerPage("internet-archive", ({ select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel }) => {
  const PACKAGE = "internet-archive";
  const API = "/api/internet-archive";
  const SVG = "http://www.w3.org/2000/svg";
  const MAP_MIRRORS = 10;
  const REPO_RE = /^([a-z0-9][a-z0-9_.-]{0,99})\/([A-Za-z0-9][A-Za-z0-9_.-]{0,99})$/;
  const ERRORS = {
    invalid_identifier: "Enter an item identifier of 5 to 100 letters, digits, dots, dashes or underscores.",
    invalid_access_key: "Paste the S3 access key from your archive.org account.",
    invalid_secret: "The secret key has letters and digits only.",
    secret_required: "Paste the S3 secret key that goes with this access key.",
    invalid_interval: "Choose every day, every week or every month.",
    private_repository: "Only public repositories can be archived: everything on archive.org is public.",
    sync_too_soon: "A backup was requested a moment ago. Try again in ten minutes.",
    not_authorized: "Only the repository owner can manage its backup.",
    not_configured: "This repository is not backed up yet.",
    not_found: "There is no public repository with that name.",
    internet_archive_not_installed: "Internet Archive is not installed for your account.",
  };
  const VERDICTS = {
    match: { tone: "good", glyph: "check", label: "Matches the archive", detail: "Same default branch and tags as the latest archive.org backup." },
    head: { tone: "good", glyph: "check", label: "Same default branch", detail: "Its default branch is the one in the latest backup; some tags differ." },
    earlier: { tone: "warn", glyph: "back", label: "Matches an older backup", detail: "Serves a state that was archived earlier: genuine, but behind." },
    newer: { tone: "warn", glyph: "ahead", label: "Moved on since the backup", detail: "Synced after the latest backup. Check that it builds on the archived history." },
    differs: { tone: "bad", glyph: "cross", label: "Differs from the archive", detail: "Matches no archived state and has not synced since the latest backup." },
    unknown: { tone: "muted", glyph: "unknown", label: "Cannot be compared", detail: "This copy does not publish its refs." },
  };
  const LEGEND = ["match", "earlier", "newer", "differs", "unknown"];
  const WITNESS = {
    verified: { tone: "good", glyph: "check", label: "Verified on archive.org", detail: "archive.org holds exactly the bundle that was uploaded (same SHA-1)." },
    older: { tone: "warn", glyph: "back", label: "archive.org serves an older backup", detail: "The newest upload has not replaced the file yet." },
    pending: { tone: "muted", glyph: "unknown", label: "archive.org is processing", detail: "A new upload can take a few hours to appear in the item." },
    unrecorded: { tone: "muted", glyph: "unknown", label: "No checksum recorded yet", detail: "This backup predates checksums; the next upload records one." },
    missing: { tone: "bad", glyph: "cross", label: "Missing from archive.org", detail: "The item does not list the bundle." },
    changed: { tone: "bad", glyph: "cross", label: "Changed on archive.org", detail: "The file in the item is not the bundle that was uploaded." },
    unreachable: { tone: "muted", glyph: "unknown", label: "archive.org did not answer", detail: "Try again in a few minutes." },
    none: { tone: "muted", glyph: "unknown", label: "Nothing uploaded yet", detail: "The first upload starts within a few minutes of setting up the backup." },
  };
  const GLYPHS = {
    check: "M-4 0.5 L-1.2 3.2 L4 -2.8",
    cross: "M-3 -3 L3 3 M3 -3 L-3 3",
    ahead: "M-3.5 0 H3.5 M0.8 -2.8 L3.5 0 L0.8 2.8",
    back: "M3.5 0 H-3.5 M-0.8 -2.8 L-3.5 0 L-0.8 2.8",
    unknown: "M-2.4 -1.6 A2.4 2.4 0 1 1 0 0.8 V1.6 M0 3.8 V3.9",
  };
  const state = {
    loaded: false, installed: false, backups: [], repo: null, witness: null, settings: null,
    settingsDenied: false, editing: false, commandMirror: "",
  };
  const reducedMotion = () => window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches;

  function message(kind, value = "") {
    const node = $(`[data-ia-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-ia-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = session()?.sessionToken;
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false || payload.error) {
      const code = String(payload.error || `HTTP ${response.status}`);
      const error = new Error(payload.message || ERRORS[code] || code.replaceAll("_", " "));
      error.status = response.status; error.code = code; throw error;
    }
    return payload;
  }
  const repoPath = (repo) => `${API}/repo/${encodeURIComponent(repo.owner)}/${encodeURIComponent(repo.name)}`;
  const shortId = (value) => String(value || "").slice(0, 10);
  const when = (ms) => (ms ? relativeTimeLabel(ms) || "just now" : "never");
  function bytes(value) {
    const size = Number(value || 0);
    if (size >= 1073741824) return `${(size / 1073741824).toFixed(1)} GB`;
    if (size >= 1048576) return `${(size / 1048576).toFixed(1)} MB`;
    return `${Math.max(1, Math.round(size / 1024))} KB`;
  }
  function day(ms) {
    return new Date(ms).toLocaleDateString(undefined, { month: "short", day: "numeric" });
  }
  function svg(name, attrs = {}, parent = null) {
    const node = document.createElementNS(SVG, name);
    Object.entries(attrs).forEach(([key, value]) => node.setAttribute(key, String(value)));
    if (parent) parent.appendChild(node);
    return node;
  }
  function text(parent, x, y, value, cls, anchor = "start") {
    const node = svg("text", { x, y, class: cls, "text-anchor": anchor }, parent);
    node.textContent = value;
    return node;
  }
  function glyphIcon(glyph, tone) {
    return `<svg class="ia-icon ia-tone-${tone}" viewBox="-7 -7 14 14" width="14" height="14" aria-hidden="true"><circle r="6.5" fill="currentColor"></circle><path d="${GLYPHS[glyph]}" fill="none" stroke="rgb(var(--dashboard-card-rgb))" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"></path></svg>`;
  }
  function verdictOf(copy) { return VERDICTS[copy.verdict] || VERDICTS.unknown; }
  function witnessOf(data) { return WITNESS[data?.witness?.state] || WITNESS.none; }
  function truncate(value, length) {
    const clean = String(value || "");
    return clean.length > length ? `${clean.slice(0, length - 1)}…` : clean;
  }

  const tip = () => $("[data-ia-tip]");
  function showTip(target, html) {
    const node = tip(); const root = $("[data-ia-root]");
    if (!node || !root) return;
    node.innerHTML = html; node.classList.remove("hidden");
    const box = target.getBoundingClientRect(); const frame = root.getBoundingClientRect();
    const width = node.offsetWidth; const height = node.offsetHeight;
    let left = box.left - frame.left + box.width / 2 - width / 2;
    left = Math.max(8, Math.min(left, frame.width - width - 8));
    let top = box.top - frame.top - height - 8;
    if (top < 8) top = box.bottom - frame.top + 8;
    node.style.left = `${left}px`; node.style.top = `${top}px`;
  }
  function hideTip() { tip()?.classList.add("hidden"); }
  function hoverable(node, html, label) {
    node.setAttribute("tabindex", "0");
    node.setAttribute("aria-label", label);
    const open = () => showTip(node, html);
    node.addEventListener("pointerenter", open);
    node.addEventListener("focus", open);
    node.addEventListener("pointerleave", hideTip);
    node.addEventListener("blur", hideTip);
  }
  function details(title, rows) {
    return `<strong>${escapeHtml(title)}</strong><dl>${rows.filter(([, value]) => value).map(([key, value]) =>
      `<dt>${escapeHtml(key)}</dt><dd>${escapeHtml(value)}</dd>`).join("")}</dl>`;
  }
  function copyTip(copy) {
    const verdict = verdictOf(copy);
    return details(copy.node || (copy.role === "source" ? "Source" : "Mirror"), [
      ["Compared", verdict.label],
      ["Why", verdict.detail],
      ["Branch", copy.branch ? `${copy.branch} @ ${shortId(copy.commit)}` : shortId(copy.commit)],
      ["Matched", copy.at && copy.verdict === "earlier" ? `backup from ${day(copy.at)}` : ""],
      ["Synced", copy.lastSync ? when(copy.lastSync) : ""],
      ["State", copy.status === "online" ? (copy.cloneAvailable ? "Online, serving clones" : "Online") : "Offline"],
    ]);
  }

  function edgePath(x1, y1, x2, y2) {
    const bend = (x2 - x1) * 0.5;
    return `M${x1} ${y1} C${x1 + bend} ${y1} ${x2 - bend} ${y2} ${x2} ${y2}`;
  }
  function edgeClass(verdict) {
    if (verdict === "earlier" || verdict === "newer") return " is-dashed";
    if (verdict === "unknown") return " is-dotted";
    return "";
  }
  function drawCopy(layer, copy, x, y, index, radius) {
    const verdict = verdictOf(copy);
    const group = svg("g", { class: `ia-node ia-tone-${verdict.tone}`, style: `--i:${index}`, role: "listitem" }, layer);
    const right = copy.role === "mirror";
    svg("rect", right
      ? { class: "ia-hit", x: x - radius - 8, y: y - 24, width: 196, height: 48, rx: 8 }
      : { class: "ia-hit", x: x - 84, y: y - radius - 8, width: 168, height: radius * 2 + 52, rx: 8 }, group);
    svg("circle", { class: `ia-mark${copy.status === "online" ? "" : " is-offline"}`, cx: x, cy: y, r: radius }, group);
    svg("path", { class: "ia-glyph", d: GLYPHS[verdict.glyph], transform: `translate(${x} ${y}) scale(${radius / 11})` }, group);
    if (right) {
      text(group, x + radius + 10, y - 2, truncate(copy.node || "mirror", 22), "ia-text");
      text(group, x + radius + 10, y + 13, `${verdict.label}${copy.status === "online" ? "" : " · offline"}`, "ia-text-muted");
    } else {
      text(group, x, y + radius + 18, truncate(copy.node || "Source", 22), "ia-text", "middle");
      text(group, x, y + radius + 33, copy.status === "online" ? `Source · ${verdict.label.toLowerCase()}` : "Source · offline", "ia-text-muted", "middle");
    }
    const status = copy.status === "online" ? "online" : "offline";
    hoverable(group, copyTip(copy), `${copy.role === "source" ? "Source" : "Mirror"} ${copy.node || ""}, ${status}: ${verdict.label}`);
  }
  function renderMap(data) {
    const holder = $("[data-ia-map]"); if (!holder) return;
    holder.replaceChildren();
    const copies = data.copies || [];
    const source = copies.find((copy) => copy.role === "source") || null;
    const mirrors = copies.filter((copy) => copy.role === "mirror");
    const shown = mirrors.slice(0, MAP_MIRRORS);
    const gap = 54; const width = 780;
    const height = Math.max(220, shown.length * gap + 50);
    const cx = 330; const cy = height / 2; const vaultW = 168; const vaultH = 70;
    const sx = 96; const mx = 560;
    const root = svg("svg", { viewBox: `0 0 ${width} ${height}`, role: "list", "aria-label": "Copies of the repository" }, holder);
    const witness = witnessOf(data);
    const edges = svg("g", {}, root);
    if (source) {
      svg("path", { class: `ia-edge ia-tone-${witness.tone}`, d: edgePath(sx + 18, cy, cx - vaultW / 2 - 2, cy), style: "--i:0" }, edges);
      const latest = data.archive?.latest;
      text(root, (sx + cx - vaultW / 2) / 2 + 8, cy - 10, latest ? `backed up ${day(latest.at)}` : "not backed up yet", "ia-text-muted", "middle");
    }
    shown.forEach((copy, index) => {
      const y = cy + (index - (shown.length - 1) / 2) * gap;
      svg("path", { class: `ia-edge ia-tone-${verdictOf(copy).tone}${edgeClass(copy.verdict)}`, d: edgePath(cx + vaultW / 2 + 2, cy, mx - 14, y), style: `--i:${index + 1}` }, edges);
    });
    const vault = svg("g", { class: `ia-node ia-tone-${witness.tone}`, role: "listitem" }, root);
    svg("rect", { class: "ia-vault-halo", x: cx - vaultW / 2 - 6, y: cy - vaultH / 2 - 6, width: vaultW + 12, height: vaultH + 12, rx: 16 }, vault);
    svg("rect", { class: "ia-vault", x: cx - vaultW / 2, y: cy - vaultH / 2, width: vaultW, height: vaultH, rx: 12 }, vault);
    svg("circle", { class: "ia-mark", cx: cx - vaultW / 2 + 22, cy: cy - 10, r: 9 }, vault);
    svg("path", { class: "ia-glyph", d: GLYPHS[witness.glyph], transform: `translate(${cx - vaultW / 2 + 22} ${cy - 10}) scale(.82)` }, vault);
    text(vault, cx - vaultW / 2 + 38, cy - 6, "archive.org", "ia-text-strong");
    text(vault, cx - vaultW / 2 + 16, cy + 14, truncate(data.archive?.identifier || "", 24), "ia-text-muted");
    text(vault, cx, cy + vaultH / 2 + 20, witness.label, "ia-text-muted", "middle");
    const file = data.witness?.file;
    hoverable(vault, details("archive.org copy", [
      ["Item", data.archive?.identifier],
      ["State", witness.label],
      ["Why", witness.detail],
      ["SHA-1", file?.sha1 ? shortId(file.sha1) : (data.archive?.latest?.sha1 ? shortId(data.archive.latest.sha1) : "")],
      ["Size", file?.size ? bytes(file.size) : ""],
      ["Head", data.archive?.latest?.head ? `${data.archive.latest.head.name.replace("refs/heads/", "")} @ ${shortId(data.archive.latest.head.commit)}` : ""],
    ]), `archive.org copy: ${witness.label}`);
    if (source) drawCopy(root, source, sx, cy, 0, 16);
    shown.forEach((copy, index) => drawCopy(root, copy, mx, cy + (index - (shown.length - 1) / 2) * gap, index + 1, 11));
    if (!mirrors.length) text(root, mx - 4, cy + 4, "No mirrors carry this repository yet", "ia-text-muted");
    if (mirrors.length > shown.length) text(root, mx, height - 10, `+${mirrors.length - shown.length} more in the table below`, "ia-text-muted");
    if (reducedMotion()) root.querySelectorAll(".ia-edge, .ia-node").forEach((node) => { node.style.animation = "none"; });
  }
  function renderLegend() {
    const holder = $("[data-ia-legend]"); if (!holder) return;
    holder.innerHTML = LEGEND.map((key) => {
      const verdict = VERDICTS[key];
      return `<li>${glyphIcon(verdict.glyph, verdict.tone)}${escapeHtml(verdict.label)}</li>`;
    }).join("") + `<li><svg viewBox="0 0 14 14" aria-hidden="true"><circle cx="7" cy="7" r="5.5" fill="none" stroke="currentColor" stroke-dasharray="2.5 2.5"></circle></svg>Offline</li>`;
  }

  function renderTimeline(data) {
    const holder = $("[data-ia-timeline]"); if (!holder) return;
    holder.replaceChildren();
    const uploads = (data.archive?.uploads || []).slice().sort((a, b) => a.at - b.at);
    const failures = (data.archive?.failures || []).slice();
    if (!uploads.length && !failures.length) {
      holder.innerHTML = `<p class="text-xs text-muted-foreground">No uploads yet.</p>`;
      return;
    }
    const width = 560; const height = 170; const left = 52; const right = 12; const top = 10; const bottom = 26;
    const plotW = width - left - right; const plotH = height - top - bottom;
    const times = uploads.map((item) => item.at).concat(failures.map((item) => item.at));
    const now = Number(data.checkedAt || Date.now());
    let start = Math.min(...times); const end = Math.max(now, ...times);
    if (end - start < 7 * 86400000) start = end - 7 * 86400000;
    const span = Math.max(1, end - start);
    const max = Math.max(1, ...uploads.map((item) => item.bytes)) * 1.15;
    const x = (at) => left + ((at - start) / span) * plotW;
    const y = (value) => top + plotH - (value / max) * plotH;
    const root = svg("svg", { viewBox: `0 0 ${width} ${height}`, role: "list", "aria-label": "Uploads" }, holder);
    [0, 0.5, 1].forEach((share) => {
      const value = (max / 1.15) * share; const gy = y(value);
      svg("line", { class: "ia-grid", x1: left, x2: width - right, y1: gy, y2: gy }, root);
      text(root, left - 8, gy + 3, share === 0 ? "0" : bytes(value), "ia-axis", "end");
    });
    text(root, left, height - 6, day(start), "ia-axis");
    text(root, width - right, height - 6, day(end), "ia-axis", "end");
    const barW = Math.max(3, Math.min(10, (plotW / Math.max(1, uploads.length)) * 0.55));
    const heldSha = data.witness?.file?.sha1 || "";
    uploads.forEach((item, index) => {
      const group = svg("g", { class: "ia-bar-group", role: "listitem" }, root);
      const bx = x(item.at) - barW / 2; const by = y(item.bytes); const bh = Math.max(2, top + plotH - by);
      const r = Math.min(2, barW / 2);
      svg("rect", { class: "ia-hit", x: bx - 6, y: top, width: barW + 12, height: plotH, fill: "transparent" }, group);
      svg("path", {
        class: `ia-bar${index === uploads.length - 1 ? " is-latest" : ""}`,
        d: `M${bx} ${top + plotH} V${top + plotH - bh + r} Q${bx} ${top + plotH - bh} ${bx + r} ${top + plotH - bh} H${bx + barW - r} Q${bx + barW} ${top + plotH - bh} ${bx + barW} ${top + plotH - bh + r} V${top + plotH} Z`,
      }, group);
      hoverable(group, details(new Date(item.at).toLocaleString(), [
        ["Size", bytes(item.bytes)],
        ["Head", item.head ? `${item.head.name.replace("refs/heads/", "")} @ ${shortId(item.head.commit)}` : ""],
        ["Refs", item.refCount ? String(item.refCount) : ""],
        ["SHA-1", item.sha1 ? shortId(item.sha1) : "not recorded"],
        ["archive.org", item.sha1 && item.sha1 === heldSha ? "holds this upload" : ""],
      ]), `Upload ${day(item.at)}, ${bytes(item.bytes)}`);
    });
    failures.forEach((item) => {
      const group = svg("g", { class: "ia-bar-group", role: "listitem" }, root);
      const fx = x(item.at); const fy = top + plotH;
      svg("rect", { class: "ia-hit", x: fx - 8, y: fy - 16, width: 16, height: 24, fill: "transparent" }, group);
      svg("path", { class: "ia-fail", d: `M${fx} ${fy - 6} L${fx + 5} ${fy} L${fx} ${fy + 6} L${fx - 5} ${fy} Z` }, group);
      hoverable(group, details(`Failed ${new Date(item.at).toLocaleString()}`, [["Error", item.error || "Upload failed"]]), `Failed upload ${day(item.at)}`);
    });
  }

  function renderTiles(data) {
    const holder = $("[data-ia-tiles]"); if (!holder) return;
    const witness = witnessOf(data); const summary = data.summary || {};
    const latest = data.archive?.latest;
    const serving = Number(summary.servingMirrors || 0); const agreeing = Number(summary.agreeingMirrors || 0);
    const agreeTone = !serving ? "muted" : agreeing === serving ? "good" : agreeing ? "warn" : "bad";
    const sourceTone = !summary.sourceKnown ? "muted" : summary.sourceOnline ? "good" : "bad";
    const tile = (label, value, note, tone, glyph) => `<div class="ia-tile">
        <p class="text-xs text-muted-foreground">${escapeHtml(label)}</p>
        <p class="ia-tile-value mt-1 flex items-center gap-2">${glyph ? glyphIcon(glyph, tone) : ""}<span>${escapeHtml(value)}</span></p>
        <p class="mt-0.5 truncate text-xs text-muted-foreground">${escapeHtml(note)}</p>
      </div>`;
    holder.innerHTML = [
      tile("Source", !summary.sourceKnown ? "Unknown" : summary.sourceOnline ? "Online" : "Offline", summary.sourceOnline ? "The source answers; mirrors follow it" : "Clones are served by mirrors", sourceTone, !summary.sourceKnown ? "unknown" : summary.sourceOnline ? "check" : "cross"),
      tile("archive.org copy", witness.label.replace(/^archive\.org /, "").replace(/^Verified on archive\.org$/, "Verified"), latest ? `${data.archive.identifier}` : "No upload yet", witness.tone, witness.glyph),
      tile("Mirrors agreeing", serving ? `${agreeing} of ${serving}` : "None online", serving ? "Serving mirrors that match the backup" : "No mirror is serving clones", agreeTone, serving ? (agreeing === serving ? "check" : agreeing ? "ahead" : "cross") : "unknown"),
      tile("Latest backup", latest ? when(latest.at) : "None", latest ? `${bytes(latest.bytes)} · ${latest.refCount || (data.archive.refs || []).length} refs` : "Waiting for the first upload", "muted", ""),
    ].join("");
  }
  function renderVerdict(data) {
    const node = $("[data-ia-verdict]"); if (!node) return;
    const summary = data.summary || {}; const latest = data.archive?.latest;
    const serving = Number(summary.servingMirrors || 0); const agreeing = Number(summary.agreeingMirrors || 0);
    const differs = Number(summary.verdicts?.differs || 0);
    let tone = "fm-flash"; let icon = "info"; let title = ""; let body = "";
    if (!latest) {
      title = "Nothing to compare yet"; body = "Copies are compared once the first backup reaches archive.org.";
    } else if (differs) {
      tone = "fm-flash fm-flash-danger"; icon = "shield-alert";
      title = `${differs} ${differs === 1 ? "copy differs" : "copies differ"} from every archived backup`;
      body = "Do not trust those copies until their owners explain the change. Clone from a copy that matches instead.";
    } else if (!summary.sourceOnline && summary.sourceKnown) {
      tone = agreeing ? "fm-flash fm-flash-success" : "fm-flash fm-flash-warning"; icon = agreeing ? "shield-check" : "triangle-alert";
      title = agreeing ? `Source offline: ${agreeing} of ${serving} serving ${serving === 1 ? "mirror matches" : "mirrors match"} the archive` : "Source offline and no serving mirror matches the latest backup";
      body = agreeing ? `Their default branch is the one archived on archive.org ${when(latest.at)}, an independent copy the mirrors cannot change.` : "Mirrors that moved on may still be fine: check that they build on the archived history below.";
    } else {
      tone = serving && agreeing === serving ? "fm-flash fm-flash-success" : "fm-flash"; icon = serving && agreeing === serving ? "shield-check" : "info";
      title = serving ? `${agreeing} of ${serving} serving ${serving === 1 ? "mirror matches" : "mirrors match"} the latest backup` : "No mirror is serving this repository";
      body = "If the source goes offline, this page tells you which mirrors still serve the archived repository.";
    }
    node.className = `${tone}`;
    node.innerHTML = `<i data-lucide="${icon}"></i><div class="fm-flash-body"><p class="fm-flash-title">${escapeHtml(title)}</p><p>${escapeHtml(body)}</p></div>`;
  }
  function renderCopies(data) {
    const body = $("[data-ia-copies]"); if (!body) return;
    const copies = data.copies || [];
    if (!copies.length) {
      body.innerHTML = `<tr><td colspan="5" class="text-muted-foreground">No copies are published for this repository.</td></tr>`;
      return;
    }
    body.innerHTML = copies.map((copy) => {
      const verdict = verdictOf(copy);
      return `<tr>
        <td><span class="font-medium text-foreground">${escapeHtml(copy.node || "unnamed")}</span> <span class="fm-label">${copy.role === "source" ? "Source" : "Mirror"}</span></td>
        <td><span class="inline-flex items-center gap-1.5">${glyphIcon(verdict.glyph, verdict.tone)}<span>${escapeHtml(verdict.label)}${copy.verdict === "earlier" && copy.at ? ` (${escapeHtml(day(copy.at))})` : ""}</span></span></td>
        <td class="font-mono text-xs">${escapeHtml(copy.branch || "")}${copy.commit ? ` @ ${escapeHtml(shortId(copy.commit))}` : ""}</td>
        <td class="text-muted-foreground">${escapeHtml(copy.lastSync ? when(copy.lastSync) : "")}</td>
        <td><span class="inline-flex items-center gap-1.5 ia-tone-${copy.status === "online" ? "good" : "muted"}"><span class="ia-dot"></span>${copy.status === "online" ? (copy.cloneAvailable ? "Serving clones" : "Online") : "Offline"}</span></td>
      </tr>`;
    }).join("");
  }
  function renderCommands(data) {
    const list = $("[data-ia-commands]"); const picker = $("[data-ia-command-mirror]");
    if (!list || !picker) return;
    const latest = data.archive?.latest; const file = data.archive?.file || "repository.bundle";
    const withUrl = (data.copies || []).filter((copy) => copy.mirrorUrl);
    if (!withUrl.some((copy) => copy.mirrorUrl === state.commandMirror)) {
      state.commandMirror = (withUrl.find((copy) => copy.status === "online") || withUrl[0])?.mirrorUrl || "";
    }
    picker.innerHTML = withUrl.length ? withUrl.map((copy) =>
      `<option value="${escapeHtml(copy.mirrorUrl)}"${copy.mirrorUrl === state.commandMirror ? " selected" : ""}>${escapeHtml(copy.node || copy.mirrorUrl)}</option>`).join("")
      : `<option value="">No mirror publishes an address</option>`;
    picker.disabled = !withUrl.length;
    if (!latest) { list.innerHTML = `<li>Commands appear after the first backup.</li>`; return; }
    const mirror = state.commandMirror || "<mirror address>";
    const branch = (latest.head?.name || "refs/heads/main").replace("refs/heads/", "");
    const step = (title, code) => `<li class="grid gap-1.5"><span>${escapeHtml(title)}</span><pre class="ia-code">${escapeHtml(code)}</pre></li>`;
    list.innerHTML = [
      step("Download the archived bundle and check it is the one that was uploaded.", `curl -LO ${data.archive.downloadUrl}\nsha1sum ${file}   # ${latest.sha1 || "no checksum recorded"}\ngit bundle verify ${file}`),
      step("Compare the mirror's default branch with the archived one.", `git ls-remote ${mirror} refs/heads/${branch}\n# archived: ${latest.head?.commit || "unknown"}`),
      step("If the mirror moved on, check it builds on the archived history.", `git clone ${file} archived && cd archived\ngit fetch ${mirror} ${branch}\ngit merge-base --is-ancestor ${latest.head?.commit || "HEAD"} FETCH_HEAD && echo "builds on the archive"`),
    ].join("");
  }

  function renderSettingsForm(body) {
    const archive = body?.archive || null;
    const interval = archive?.interval || body?.catalog?.defaultInterval || "weekly";
    const options = (body?.catalog?.intervals || []).map((item) =>
      `<option value="${escapeHtml(item.key)}"${item.key === interval ? " selected" : ""}>${escapeHtml(item.label)}</option>`).join("");
    const keepsSecret = Boolean(archive?.secretHint);
    const placeholder = state.repo ? `${state.repo.owner}-${state.repo.name}` : "owner-name";
    const input = "h-control w-full rounded-md border border-border bg-background px-2 font-mono text-control text-foreground outline-none focus:border-primary";
    return `<form data-ia-settings-form class="grid gap-4">
      <div class="grid gap-3 sm:grid-cols-2">
        <label class="grid gap-1 text-xs font-medium text-foreground">Item identifier
          <span class="flex h-control min-w-0 items-center rounded-md border border-border bg-background text-control focus-within:border-primary">
            <span class="shrink-0 pl-2 font-mono text-muted-foreground">archive.org/details/</span>
            <input data-ia-identifier type="text" autocomplete="off" spellcheck="false" required value="${escapeHtml(archive?.identifier || "")}" placeholder="${escapeHtml(placeholder)}" class="h-full min-w-0 flex-1 bg-transparent px-1 font-mono text-control text-foreground outline-none" />
          </span>
        </label>
        <label class="grid gap-1 text-xs font-medium text-foreground">How often
          <select data-ia-interval class="${input} font-sans">${options}</select>
        </label>
        <label class="grid gap-1 text-xs font-medium text-foreground">S3 access key
          <input data-ia-access type="text" autocomplete="off" spellcheck="false" required value="${escapeHtml(archive?.accessKey || "")}" class="${input}" />
        </label>
        <label class="grid gap-1 text-xs font-medium text-foreground">S3 secret key
          <input data-ia-secret type="password" autocomplete="new-password" spellcheck="false"${keepsSecret ? "" : " required"} placeholder="${keepsSecret ? `Saved (${escapeHtml(archive.secretHint)}). Leave blank to keep it` : ""}" class="${input}" />
        </label>
      </div>
      <p class="text-xs leading-5 text-muted-foreground">The item is created on the first upload if it does not exist yet. <a href="${escapeHtml(body?.catalog?.keysUrl || "https://archive.org/account/s3.php")}" target="_blank" rel="noopener noreferrer" class="text-accent hover:underline">Get your S3 keys on archive.org</a>. They are stored encrypted, used only for this backup, and the secret is never shown again.</p>
      <div class="flex flex-wrap items-center justify-end gap-2">
        ${archive ? `<button type="button" data-ia-cancel class="fm-btn fm-btn-sm">Cancel</button>` : ""}
        <button type="submit" class="fm-btn fm-btn-sm"><i data-lucide="archive"></i>${archive ? "Save backup" : "Start backing up"}</button>
      </div>
    </form>`;
  }
  function uploadLine(body) {
    const archive = body.archive; const upload = archive.status?.upload || {};
    const every = ((body.catalog?.intervals || []).find((item) => item.key === archive.interval)?.label || "Every week").toLowerCase();
    if (!body.codeHosted) return ["muted", "Waiting for this repository to be hosted on the cores."];
    if (!upload.at) return ["muted", "The first upload starts within a few minutes."];
    if (!upload.ok) return ["bad", `Last upload failed ${when(upload.at)}: ${upload.error || "unknown error"}`];
    return ["good", `Uploaded ${archive.file} (${bytes(upload.bytes)}) ${when(upload.at)}. Next upload ${every}, once something changes.`];
  }
  function renderSettingsOverview(body) {
    const archive = body.archive; const status = archive.status || {};
    const [tone, line] = uploadLine(body);
    const connection = status.error ? "bad" : status.username ? "good" : "muted";
    const history = (archive.history || []).slice(0, 6).map((entry) =>
      `<li class="flex min-w-0 items-start gap-2 py-1.5"><span class="ia-dot ia-tone-${entry.ok ? "good" : "bad"} mt-1"></span><span class="min-w-0 flex-1 break-words">${escapeHtml(entry.message || entry.kind || "")}</span><span class="shrink-0">${escapeHtml(when(entry.at))}</span></li>`).join("");
    return `<div class="grid gap-4">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div class="grid min-w-0 gap-0.5">
          <a href="${escapeHtml(archive.itemUrl)}" target="_blank" rel="noopener noreferrer" class="truncate font-mono text-control font-semibold text-accent hover:underline">archive.org/details/${escapeHtml(archive.identifier)}</a>
          <span class="inline-flex items-center gap-1.5"><span class="ia-dot ia-tone-${connection}"></span>${status.error ? escapeHtml(status.error) : `As ${escapeHtml(status.username || "unknown")}`}</span>
        </div>
        <span class="inline-flex items-center gap-1.5"><span class="ia-dot ia-tone-${archive.active ? "good" : "muted"}"></span>${archive.active ? "Backing up" : "Paused"}</span>
      </div>
      <p class="flex items-start gap-2"><span class="ia-dot ia-tone-${tone} mt-1"></span><span>${escapeHtml(line)}</span></p>
      ${body.private ? `<p class="text-destructive">${escapeHtml(ERRORS.private_repository)} Nothing is uploaded while it is private.</p>` : ""}
      <div class="flex flex-wrap items-center gap-2">
        <button type="button" data-ia-action="sync" class="fm-btn fm-btn-sm"><i data-lucide="upload-cloud"></i>Back up now</button>
        <button type="button" data-ia-edit class="fm-btn fm-btn-sm"><i data-lucide="sliders-horizontal"></i>Change settings</button>
        <button type="button" data-ia-action="verify" class="fm-btn fm-btn-sm">Check keys</button>
        <button type="button" data-ia-action="toggle" data-ia-active="${archive.active ? "1" : "0"}" class="fm-btn fm-btn-sm">${archive.active ? "Pause" : "Resume"}</button>
        <button type="button" data-ia-action="disconnect" class="fm-btn fm-btn-sm fm-btn-danger">Stop backing up</button>
      </div>
      ${history ? `<div><p class="text-xs font-medium text-foreground">Recent activity</p><ul class="mt-1 divide-y divide-border">${history}</ul></div>` : ""}
    </div>`;
  }
  function settingsStatus(value, tone = "") {
    const node = $("[data-ia-settings-status]"); if (!node) return;
    node.textContent = value || "";
    node.className = `text-xs ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-success" : "text-muted-foreground"}`;
  }
  function renderSettings() {
    const holder = $("[data-ia-settings-body]"); if (!holder) return;
    if (state.settingsDenied) {
      holder.innerHTML = `<p>${escapeHtml(ERRORS.not_authorized)}</p>`;
      return;
    }
    const body = state.settings;
    if (!body) { holder.textContent = "Loading…"; return; }
    if (!body.archive && body.private) {
      holder.innerHTML = `<p>${escapeHtml(ERRORS.private_repository)} Make the repository public to back it up there.</p>`;
      return;
    }
    holder.innerHTML = body.archive && !state.editing ? renderSettingsOverview(body) : renderSettingsForm(body);
    window.lucide?.createIcons?.();
  }

  function renderRepo() {
    const data = state.witness;
    $("[data-ia-empty]")?.classList.toggle("hidden", Boolean(state.repo));
    $("[data-ia-repo]")?.classList.toggle("hidden", !state.repo);
    const input = $("[data-ia-repo-input]");
    if (input && state.repo && document.activeElement !== input) input.value = `${state.repo.owner}/${state.repo.name}`;
    renderChips();
    if (!state.repo) return;
    renderSettings();
    if (!data) return;
    const checked = $("[data-ia-checked]");
    if (checked) checked.textContent = data.archive ? `Checked ${when(data.checkedAt)}` : "";
    if (!data.archive) {
      $("[data-ia-verdict]").className = "fm-flash";
      $("[data-ia-verdict]").innerHTML = `<i data-lucide="info"></i><div class="fm-flash-body"><p class="fm-flash-title">This repository has no archive.org backup</p><p>Its owner can start one below. Until then there is no independent copy to compare the mirrors with.</p></div>`;
      ["[data-ia-tiles]", "[data-ia-map]", "[data-ia-legend]", "[data-ia-copies]", "[data-ia-timeline]", "[data-ia-commands]"].forEach((selector) => { const node = $(selector); if (node) node.innerHTML = ""; });
      window.lucide?.createIcons?.();
      return;
    }
    renderVerdict(data); renderTiles(data); renderMap(data); renderLegend(); renderCopies(data); renderTimeline(data); renderCommands(data);
    window.lucide?.createIcons?.();
  }
  function renderChips() {
    const holder = $("[data-ia-backup-chips]"); if (!holder) return;
    holder.innerHTML = state.backups.map((item) => {
      const key = `${item.owner}/${item.repo}`;
      const current = state.repo && `${state.repo.owner}/${state.repo.name}` === key;
      const tone = item.upload?.ok === false ? "bad" : item.active ? "good" : "muted";
      return `<button type="button" data-ia-open="${escapeHtml(key)}" class="fm-btn fm-btn-sm${current ? " fm-btn-invisible" : ""}" aria-pressed="${current ? "true" : "false"}"><span class="ia-dot ia-tone-${tone}"></span>${escapeHtml(key)}</button>`;
    }).join("");
  }
  function renderInstall() {
    $("[data-ia-install-panel]")?.classList.toggle("hidden", !state.loaded || state.installed);
    $("[data-ia-app]")?.classList.toggle("hidden", !state.loaded || !state.installed);
    $$("[data-ia-requires-install]").forEach((node) => node.classList.toggle("hidden", !state.loaded || !state.installed));
    $$(`[data-plugin-nav="${PACKAGE}"]`).forEach((link) => link.classList.toggle("hidden", !state.installed));
  }

  function parseRepo(value) {
    const match = REPO_RE.exec(String(value || "").trim().replace(/^\/+|\/+$/g, ""));
    return match ? { owner: match[1].toLowerCase(), name: match[2] } : null;
  }
  async function loadWitness() {
    if (!state.repo) return;
    const repo = state.repo;
    try {
      const data = await request(`${repoPath(repo)}/witness`);
      if (state.repo !== repo) return;
      state.witness = data; message("error", "");
    } catch (error) {
      if (state.repo !== repo) return;
      state.witness = null;
      message("error", error.code === "not_found" ? ERRORS.not_found : `Could not check this repository: ${error.message}`);
    }
  }
  async function loadSettings() {
    if (!state.repo) return;
    const repo = state.repo;
    try {
      const body = await request(repoPath(repo), "POST", { action: "get" });
      if (state.repo !== repo) return;
      state.settings = body; state.settingsDenied = false;
    } catch (error) {
      if (state.repo !== repo) return;
      state.settings = null; state.settingsDenied = true;
      if (error.code !== "not_authorized") settingsStatus(error.message, "bad");
    }
  }
  async function openRepo(repo, push = true) {
    state.repo = repo; state.witness = null; state.settings = null; state.settingsDenied = false; state.editing = false;
    settingsStatus("");
    if (push) {
      const url = new URL(window.location.href);
      if (repo) url.searchParams.set("repo", `${repo.owner}/${repo.name}`); else url.searchParams.delete("repo");
      window.history.replaceState(window.history.state, "", url);
    }
    renderRepo();
    if (!repo) return;
    await Promise.all([loadWitness(), loadSettings()]);
    renderRepo();
  }
  async function loadBackups() {
    try { state.backups = (await request(`${API}/backups`)).backups || []; }
    catch (_) { state.backups = []; }
  }
  async function load() {
    const button = $("[data-ia-refresh]"); if (button) button.disabled = true;
    try {
      const account = await request("/api/plugins/@account");
      const entry = (account.packages || []).find((item) => item && item.id === PACKAGE);
      state.installed = entry ? entry.installed !== false : false;
      state.loaded = true; message("error", "");
      renderInstall();
      if (!state.installed) return;
      await loadBackups();
      const wanted = parseRepo(new URLSearchParams(window.location.search).get("repo"))
        || state.repo
        || (state.backups[0] ? { owner: state.backups[0].owner, name: state.backups[0].repo } : null);
      await openRepo(wanted, false);
    } catch (error) {
      message("error", `Internet Archive is unavailable: ${error.message}`);
    } finally { if (button) button.disabled = false; }
  }
  async function lifecycle(action) {
    await request(`/api/plugins/@account/${PACKAGE}/${action}`, "POST", {});
  }
  async function install() {
    const button = $("[data-ia-install]"); if (button) button.disabled = true;
    try { await lifecycle("install"); message("notice", "Internet Archive installed."); await load(); }
    catch (error) { message("error", `Could not install Internet Archive: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function remove() {
    if (!window.confirm("Remove Internet Archive? Every backup you set up stops and its stored keys are deleted. Nothing already on archive.org is removed.")) return;
    try {
      await lifecycle("uninstall");
      state.backups = []; state.repo = null; state.witness = null; state.settings = null;
      message("notice", "Internet Archive removed.");
      await load();
    } catch (error) { message("error", `Could not remove Internet Archive: ${error.message}`); }
  }
  async function settingsAction(button) {
    const action = button.dataset.iaAction;
    if (action === "disconnect" && !window.confirm("Stop backing up this repository to the Internet Archive? The stored keys are deleted. Nothing already uploaded is removed from archive.org.")) return;
    const payload = { action };
    if (action === "toggle") payload.active = button.dataset.iaActive !== "1";
    const pending = { sync: "Requesting a backup…", verify: "Checking the keys…", toggle: payload.active ? "Resuming…" : "Pausing…", disconnect: "Stopping…" }[action];
    button.disabled = true; settingsStatus(pending);
    try {
      const body = await request(repoPath(state.repo), "POST", payload);
      state.settings = body;
      if (action === "disconnect") state.editing = false;
      settingsStatus({
        sync: "Backup requested. The upload starts within a few minutes.",
        verify: body.archive?.status?.error || "The keys work.",
        toggle: payload.active ? "Backups resumed." : "Backups paused.",
        disconnect: "Stopped backing up.",
      }[action], action === "verify" && body.archive?.status?.error ? "bad" : "good");
      if (action === "disconnect") { await Promise.all([loadBackups(), loadWitness()]); }
      renderRepo();
    } catch (error) {
      settingsStatus(error.message, "bad");
    } finally { button.disabled = false; }
  }
  async function saveSettings(form) {
    const submit = form.querySelector('button[type="submit"]'); if (submit) submit.disabled = true;
    settingsStatus("Checking the keys with archive.org…");
    try {
      state.settings = await request(repoPath(state.repo), "POST", {
        action: "save",
        identifier: form.querySelector("[data-ia-identifier]")?.value || "",
        accessKey: form.querySelector("[data-ia-access]")?.value || "",
        secret: form.querySelector("[data-ia-secret]")?.value || "",
        interval: form.querySelector("[data-ia-interval]")?.value || "",
      });
      state.editing = false;
      settingsStatus("Backup saved.", "good");
      await Promise.all([loadBackups(), loadWitness()]);
      renderRepo();
    } catch (error) {
      settingsStatus(error.message, "bad");
    } finally { if (submit) submit.disabled = false; }
  }

  function init() {
    if (!session()?.sessionToken) return message("error", "Sign in to use Internet Archive.");
    $("[data-ia-install]")?.addEventListener("click", () => void install());
    $("[data-ia-refresh]")?.addEventListener("click", () => { message("notice", ""); void load(); });
    $("[data-ia-remove]")?.addEventListener("click", () => void remove());
    $("[data-ia-pick]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const repo = parseRepo($("[data-ia-repo-input]")?.value);
      if (!repo) return message("error", "Enter the repository as owner/name.");
      message("error", ""); void openRepo(repo);
    });
    $("[data-ia-backup-chips]")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-ia-open]");
      if (button) void openRepo(parseRepo(button.dataset.iaOpen));
    });
    $("[data-ia-command-mirror]")?.addEventListener("change", (event) => {
      state.commandMirror = event.target.value; if (state.witness?.archive) renderCommands(state.witness);
    });
    const settings = $("[data-ia-settings-body]");
    settings?.addEventListener("click", (event) => {
      if (event.target.closest("[data-ia-edit]")) { state.editing = true; renderSettings(); return; }
      if (event.target.closest("[data-ia-cancel]")) { state.editing = false; renderSettings(); return; }
      const button = event.target.closest("[data-ia-action]");
      if (button && !button.disabled) void settingsAction(button);
    });
    settings?.addEventListener("submit", (event) => {
      const form = event.target.closest("[data-ia-settings-form]");
      if (!form) return;
      event.preventDefault(); void saveSettings(form);
    });
    window.lucide?.createIcons?.();
    return load();
  }
  return init();
});

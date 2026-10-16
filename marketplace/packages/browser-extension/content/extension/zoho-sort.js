(() => {
  if (window.__forkmeshZohoSort) return;
  window.__forkmeshZohoSort = true;

  const HOST_ID = "forkmesh-zoho-sort";
  const BOX = 'input[type="checkbox"], [role="checkbox"], [class*="checkbox" i], [class*="chkbox" i]';
  const SENDER = '[class*="zmLFrom"], [class*="sender" i], [class*="from" i], [class*="Frm"]';
  const SUBJECT = '[class*="zmLSub"], [class*="subject" i]';
  const SIZE = /^\d+(?:[.,]\d+)?\s*(?:bytes?|[KMG]B)$/i;
  const MONTH = "(?:jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\\.?";
  const STAMP = new RegExp("^(?:(?:mon|tue|wed|thu|fri|sat|sun)[a-z]*,?\\s*)?"
    + `(?:${MONTH}\\s+\\d{1,2}(?:,?\\s+\\d{4})?|\\d{1,2}\\s+${MONTH}(?:\\s+\\d{4})?|\\d{1,2}[/.-]\\d{1,2}[/.-]\\d{2,4}|today|yesterday)?`
    + ",?\\s*(?:\\d{1,2}:\\d{2}(?:\\s*[AP]M)?)?$", "i");
  const REPLY_PREFIX = /^(?:(?:re|fwd?|aw|sv|wg|antw)\s*(?:\[\d+\])?\s*:\s*)+/i;
  const COLUMNS = [["sender", "Sender"], ["subject", "Subject"], ["date", "Date"]];
  const state = { by: "date", descending: false };
  const collator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

  const visible = (node) => Boolean(node && node.offsetParent !== null);
  const stamp = (value) => /\d|today|yesterday/i.test(value) && STAMP.test(value);

  function cells(row) {
    const found = [];
    const walker = document.createTreeWalker(row, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      const value = node.nodeValue.replace(/\s+/g, " ").trim();
      const parent = node.parentElement;
      if (!value || !parent || !visible(parent)) continue;
      const last = found[found.length - 1];
      if (last && last.node === parent) last.text += ` ${value}`;
      else found.push({ node: parent, text: value });
    }
    return found;
  }

  function columns(row) {
    const all = cells(row);
    const words = all.filter((cell) => !SIZE.test(cell.text) && !stamp(cell.text) && !/^\(?\d+\)?$/.test(cell.text));
    const tagged = (cell, selector) => {
      const hit = cell.node.closest(selector);
      return Boolean(hit && hit !== row && row.contains(hit));
    };
    const sender = words.find((cell) => tagged(cell, SENDER)) || words[0] || null;
    const subject = words.find((cell) => cell !== sender && tagged(cell, SUBJECT))
      || words[words.indexOf(sender) + 1] || null;
    const date = all.findLast((cell) => stamp(cell.text)) || null;
    return { sender, subject, date };
  }

  function keyOf(row, by) {
    const cols = columns(row);
    if (by === "sender") return (cols.sender?.text || "").replace(/^["'\s]+|["'\s]+$/g, "").replace(/\s*\(\d+\)$/, "");
    return (cols.subject?.text || "").replace(REPLY_PREFIX, "").trim();
  }

  function common(a, b) {
    for (let node = a; node; node = node.parentElement) if (node.contains(b)) return node;
    return null;
  }

  function depth(node, ancestor) {
    let steps = 0;
    for (let up = node; up && up !== ancestor; up = up.parentElement) steps += 1;
    return steps;
  }

  function ancestor(node, steps) {
    let up = node;
    for (let step = 0; step < steps && up.parentElement; step += 1) up = up.parentElement;
    return up;
  }

  function scrollerOf(node, memo) {
    const path = [];
    let found = null;
    for (let up = node.parentElement; up && up !== document.body && up !== document.documentElement; up = up.parentElement) {
      if (memo.has(up)) { found = memo.get(up); break; }
      path.push(up);
      if (/(auto|scroll|overlay)/.test(getComputedStyle(up).overflowY)) { found = up; break; }
    }
    for (const step of path) memo.set(step, found);
    return found;
  }

  function findList() {
    const boxes = [...document.querySelectorAll(BOX)]
      .filter((box) => !box.parentElement?.closest(BOX) && !/select all/i.test(box.getAttribute("aria-label") || ""));
    const memo = new Map();
    const groups = new Map();
    for (const box of boxes) {
      const scroller = scrollerOf(box, memo);
      if (!scroller || !visible(scroller)) continue;
      if (!groups.has(scroller)) groups.set(scroller, []);
      groups.get(scroller).push(box);
    }
    let scroller = null;
    let inside = [];
    for (const [candidate, list] of groups) if (list.length > inside.length) { scroller = candidate; inside = list; }
    if (inside.length < 2) return null;
    let steps = Infinity;
    for (let index = 1; index < inside.length; index += 1) {
      const shared = common(inside[index - 1], inside[index]);
      steps = Math.min(steps, depth(inside[index - 1], shared) - 1, depth(inside[index], shared) - 1);
    }
    const rows = [...new Set(inside.map((box) => ancestor(box, steps)))].filter(visible);
    if (rows.length < 2) return null;
    let root = rows[0].parentElement;
    while (root && !rows.every((row) => root.contains(row))) root = root.parentElement;
    if (!root || !scroller.contains(root)) return null;
    return { scroller, root, rows };
  }

  function clearMarks() {
    for (const node of document.querySelectorAll("[data-fm-sort]")) {
      node.removeAttribute("data-fm-sort");
      node.style.removeProperty("order");
    }
  }

  function arrange({ root, rows }) {
    clearMarks();
    if (state.by === "date" && !state.descending) return;
    const rowSet = new Set(rows);
    const last = rows[rows.length - 1];
    const flatten = (parent) => {
      for (const kid of parent.children) {
        if (kid.id === HOST_ID || rowSet.has(kid)) continue;
        if (rows.some((row) => kid.contains(row))) {
          kid.dataset.fmSort = "flat";
          flatten(kid);
        } else if (kid.compareDocumentPosition(last) & Node.DOCUMENT_POSITION_FOLLOWING) {
          kid.dataset.fmSort = "hide";
        } else {
          kid.dataset.fmSort = "tail";
          kid.style.setProperty("order", String(rows.length + 1));
        }
      }
    };
    root.dataset.fmSort = "root";
    flatten(root);
    const keyed = rows.map((row, index) => ({ row, index, key: state.by === "date" ? "" : keyOf(row, state.by) }));
    const direction = state.descending ? -1 : 1;
    keyed.sort((a, b) => (state.by === "date" ? b.index - a.index
      : (collator.compare(a.key, b.key) * direction) || a.index - b.index));
    keyed.forEach((item, position) => {
      item.row.dataset.fmSort = "row";
      item.row.style.setProperty("order", String(position));
    });
  }

  const host = document.createElement("div");
  host.id = HOST_ID;
  const shadow = host.attachShadow({ mode: "open" });
  shadow.innerHTML = `<style>
    .bar { position: relative; height: 28px; font-size: 12px; line-height: 28px; white-space: nowrap; }
    .bar.inline { display: flex; gap: 12px; padding-left: 12px; }
    .bar.inline button { position: static; }
    button { position: absolute; top: 0; height: 28px; padding: 0 4px; border: 0; background: none;
      color: inherit; font: inherit; cursor: pointer; opacity: 0.7; }
    button:hover, button.on { opacity: 1; }
    button.on { font-weight: 600; }
    button:focus-visible { outline: 2px solid #2563eb; outline-offset: -2px; border-radius: 4px; }
    b { display: inline-block; min-width: 1em; margin-left: 3px; font-weight: 400; }
  </style><div class="bar" role="toolbar" aria-label="Sort messages">${COLUMNS.map(([key, label]) =>
    `<button type="button" data-sort="${key}" title="Sort by ${label.toLowerCase()}"><span>${label}</span><b></b></button>`).join("")}</div>`;
  const bar = shadow.querySelector(".bar");

  shadow.addEventListener("click", (event) => {
    const button = event.target.closest?.("button[data-sort]");
    if (!button) return;
    const by = button.dataset.sort;
    if (state.by === by) state.descending = !state.descending;
    else { state.by = by; state.descending = false; }
    sync();
  });

  function backgroundOf(node) {
    for (let up = node; up; up = up.parentElement) {
      const color = getComputedStyle(up).backgroundColor;
      if (color && color !== "transparent" && !/rgba\(.*,\s*0\)$/.test(color)) return color;
    }
    return "#fff";
  }

  function paint(list) {
    const row = list.rows[0];
    const look = getComputedStyle(row);
    host.style.background = backgroundOf(list.scroller);
    host.style.color = look.color;
    host.style.fontFamily = look.fontFamily;
    const cols = columns(row);
    const base = host.getBoundingClientRect().left;
    const left = (cell) => (cell ? cell.node.getBoundingClientRect().left - base - 4 : NaN);
    const spots = { sender: left(cols.sender), subject: left(cols.subject), date: left(cols.date) };
    const columnar = spots.subject - spots.sender >= 60
      && (!Number.isFinite(spots.date) || spots.date - spots.subject >= 60);
    bar.classList.toggle("inline", !columnar);
    for (const button of shadow.querySelectorAll("button[data-sort]")) {
      const by = button.dataset.sort;
      const on = state.by === by;
      button.classList.toggle("on", on);
      button.setAttribute("aria-pressed", String(on));
      button.querySelector("b").textContent = on ? ((by === "date") === !state.descending ? "▼" : "▲") : "";
      const spot = spots[by];
      button.style.left = columnar && Number.isFinite(spot) ? `${Math.max(0, spot)}px` : "";
      button.style.right = columnar && by === "date" && !Number.isFinite(spot) ? "12px" : "";
    }
  }

  function sync() {
    let list = null;
    try { list = findList(); } catch { list = null; }
    if (!list) {
      host.remove();
      clearMarks();
      return;
    }
    if (host.parentNode !== list.scroller) list.scroller.prepend(host);
    paint(list);
    arrange(list);
  }

  let timer = 0;
  function schedule(delay = 400) {
    if (timer) return;
    timer = setTimeout(() => { timer = 0; sync(); }, delay);
  }
  new MutationObserver(() => schedule()).observe(document.body || document.documentElement, { childList: true, subtree: true });
  window.addEventListener("hashchange", () => schedule(150));
  window.addEventListener("resize", () => schedule(150));
  schedule(0);
})();

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
  const COLUMNS = [["sender", "Sender"], ["subject", "Subject"], ["date", "Date"]];
  const tidy = globalThis.ForkMeshZohoTidy;
  const state = { by: "date", descending: false, group: false, panel: false, query: "", sender: "", senderLabel: "", busy: false, pending: null };
  let current = null;

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

  function clearFilter() {
    for (const node of document.querySelectorAll("[data-fm-filter]")) node.removeAttribute("data-fm-filter");
  }

  function arrange() {
    clearMarks();
    const { list: { root, rows }, items } = current;
    const keep = tidy.shown(items, state);
    for (const item of items) if (!keep.has(item.index)) item.row.dataset.fmFilter = "out";
    if (state.by === "date" && !state.descending && !state.group && keep.size === items.length) return;
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
    tidy.order(items, state).forEach((item, position) => {
      item.row.dataset.fmSort = "row";
      item.row.style.setProperty("order", String(position));
    });
  }

  const host = document.createElement("div");
  host.id = HOST_ID;
  const shadow = host.attachShadow({ mode: "open" });
  shadow.innerHTML = `<style>
    :host { --fm-line: rgba(128, 128, 128, 0.35); --fm-blue: #2563eb; --fm-red: #dc2626; }
    .bar { position: relative; height: 28px; font-size: 12px; line-height: 28px; white-space: nowrap; }
    .bar.inline { display: flex; gap: 12px; padding-left: 12px; }
    .bar.inline button { position: static; }
    .bar button { position: absolute; top: 0; height: 28px; padding: 0 4px; border: 0; background: none;
      color: inherit; font: inherit; cursor: pointer; opacity: 0.7; }
    .bar button:hover, .bar button.on { opacity: 1; }
    .bar button.on { font-weight: 600; }
    button:focus-visible, input:focus-visible { outline: 2px solid var(--fm-blue); outline-offset: -2px; border-radius: 4px; }
    b { display: inline-block; min-width: 1em; margin-left: 3px; font-weight: 400; }
    .tools { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; padding: 4px 12px; font-size: 12px;
      line-height: 22px; border-top: 1px solid var(--fm-line); }
    .tools input { box-sizing: border-box; height: 24px; width: 220px; max-width: 100%; padding: 0 8px; border: 1px solid var(--fm-line);
      border-radius: 4px; background: transparent; color: inherit; font: inherit; }
    .tools button, .confirm button, .senders .head button { height: 24px; padding: 0 8px; border: 1px solid var(--fm-line); border-radius: 4px;
      background: none; color: inherit; font: inherit; cursor: pointer; white-space: nowrap; }
    .tools button:hover:not(:disabled), .confirm button:hover { background: rgba(128, 128, 128, 0.12); }
    .tools button:disabled { opacity: 0.45; cursor: default; }
    .tools button[aria-pressed="true"] { border-color: var(--fm-blue); color: var(--fm-blue); }
    .tools .count { opacity: 0.7; }
    .tools .acts { display: flex; gap: 6px; margin-left: auto; }
    .tools .danger:not(:disabled) { color: var(--fm-red); }
    .from { display: inline-flex; align-items: center; gap: 2px; padding-left: 8px; border: 1px solid var(--fm-blue);
      border-radius: 4px; color: var(--fm-blue); max-width: 220px; }
    .from span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .from button { border: 0; height: 22px; padding: 0 6px; color: inherit; }
    .confirm, .status { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; padding: 4px 12px; font-size: 12px; line-height: 22px; }
    .confirm .danger { background: var(--fm-red); border-color: var(--fm-red); color: #fff; }
    .confirm .danger:hover { background: var(--fm-red); opacity: 0.9; }
    .status.warn { color: var(--fm-red); }
    [hidden] { display: none !important; }
    .senders { position: absolute; left: 12px; right: 12px; z-index: 4; max-height: 50vh; overflow: auto; font-size: 12px;
      background: var(--fm-bg, #fff); border: 1px solid var(--fm-line); border-radius: 6px; box-shadow: 0 6px 20px rgba(0, 0, 0, 0.18); }
    .senders .head { position: sticky; top: 0; display: flex; align-items: center; gap: 6px; padding: 4px 8px;
      background: var(--fm-bg, #fff); border-bottom: 1px solid var(--fm-line); opacity: 0.85; }
    .senders .head span { flex: 1; }
    .senders .row { display: flex; align-items: center; gap: 8px; width: 100%; height: 28px; padding: 0 8px; border: 0;
      background: none; color: inherit; font: inherit; text-align: left; cursor: pointer; }
    .senders .row:hover, .senders .row[aria-selected="true"] { background: rgba(37, 99, 235, 0.12); }
    .senders .row .n { min-width: 2.5em; text-align: right; font-variant-numeric: tabular-nums; opacity: 0.7; }
    .senders .row .name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  </style><div class="bar" role="toolbar" aria-label="Sort messages">${COLUMNS.map(([key, label]) =>
    `<button type="button" data-sort="${key}" title="Sort by ${label.toLowerCase()}"><span>${label}</span><b></b></button>`).join("")}</div>
  <div class="tools" role="toolbar" aria-label="Inbox zero">
    <input type="search" class="find" placeholder="Search subject or sender" aria-label="Search loaded messages by subject or sender" title="Words match the subject or sender; from:name or subject:word looks in one only">
    <button type="button" data-act="group" aria-pressed="false" title="Group the list by sender, largest groups first">Group by sender</button>
    <span class="from" hidden><span></span><button type="button" data-act="all-senders" aria-label="Show every sender">×</button></span>
    <span class="count"></span>
    <span class="acts">${["select", "read", "archive", "delete"].map((kind) =>
      `<button type="button" data-act="${kind}"${kind === "delete" ? ' class="danger"' : ""}></button>`).join("")}</span>
  </div>
  <div class="senders" role="listbox" aria-label="Senders" hidden></div>
  <div class="confirm" role="alertdialog" hidden><span></span><button type="button" class="danger" data-act="confirm"></button><button type="button" data-act="cancel">Cancel</button></div>
  <div class="status" role="status" aria-live="polite" hidden></div>`;
  const bar = shadow.querySelector(".bar");
  const find = shadow.querySelector(".find");
  const fromChip = shadow.querySelector(".from");
  const counter = shadow.querySelector(".count");
  const panel = shadow.querySelector(".senders");
  const confirmBox = shadow.querySelector(".confirm");
  const statusLine = shadow.querySelector(".status");

  function say(text, warn = false) {
    statusLine.textContent = text;
    statusLine.classList.toggle("warn", warn);
    statusLine.hidden = !text;
  }

  function targets() {
    return current ? tidy.filtered(current.items, state) : [];
  }

  let panelKey = "";
  function renderSenders() {
    if (!state.panel || !current) { panel.hidden = true; panelKey = ""; return; }
    const list = tidy.groups(tidy.filtered(current.items, { query: state.query }));
    const key = JSON.stringify([state.sender, list.slice(0, 300).map((entry) => [entry.key, entry.count])]);
    if (key === panelKey && !panel.hidden) return;
    panelKey = key;
    const head = document.createElement("div");
    head.className = "head";
    const label = document.createElement("span");
    label.textContent = `${tidy.plural(list.length, "sender")}: pick one to show only theirs`;
    const close = document.createElement("button");
    close.type = "button";
    close.dataset.act = "close-senders";
    close.textContent = "Close";
    head.append(label, close);
    const rows = list.slice(0, 300).map((entry) => {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "row";
      row.setAttribute("role", "option");
      row.setAttribute("aria-selected", String(entry.key === state.sender));
      row.dataset.sender = entry.key;
      row.dataset.label = entry.label;
      const count = document.createElement("span");
      count.className = "n";
      count.textContent = String(entry.count);
      const name = document.createElement("span");
      name.className = "name";
      name.textContent = entry.label;
      row.append(count, name);
      return row;
    });
    const scroll = panel.scrollTop;
    panel.replaceChildren(head, ...rows);
    panel.scrollTop = scroll;
    panel.hidden = false;
  }

  function renderTools() {
    const total = current ? current.items.length : 0;
    const count = targets().length;
    counter.textContent = tidy.summary(count, total);
    fromChip.hidden = !state.sender;
    fromChip.firstElementChild.textContent = state.sender ? `From: ${state.senderLabel}` : "";
    fromChip.title = fromChip.firstElementChild.textContent;
    const toggle = shadow.querySelector('[data-act="group"]');
    toggle.setAttribute("aria-pressed", String(state.group));
    for (const button of shadow.querySelectorAll(".acts button")) {
      button.textContent = tidy.verb(button.dataset.act, count);
      button.disabled = state.busy || count === 0;
    }
    if (state.pending && state.pending.count !== count) cancelPending();
    renderSenders();
  }

  function cancelPending() {
    state.pending = null;
    confirmBox.hidden = true;
  }

  function askDelete(count) {
    state.pending = { kind: "delete", count };
    confirmBox.firstElementChild.textContent = tidy.confirmText(count);
    confirmBox.querySelector('[data-act="confirm"]').textContent = tidy.verb("delete", count);
    confirmBox.hidden = false;
    confirmBox.querySelector('[data-act="cancel"]').focus();
  }

  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const onScreen = (node) => Boolean(node && node.getClientRects().length);

  function boxOf(row) {
    return [...row.querySelectorAll(BOX)].find((box) => !box.parentElement?.closest(BOX)) || null;
  }

  function classText(node) {
    const value = node?.className;
    return String(value?.baseVal ?? value ?? "");
  }

  function isTicked(row) {
    const box = boxOf(row);
    return tidy.ticked({
      checked: box?.checked === true,
      ariaChecked: box?.getAttribute("aria-checked") ?? null,
      ariaSelected: row.getAttribute("aria-selected"),
      boxClass: classText(box),
      rowClass: classText(row),
    });
  }

  function press(node) {
    for (const type of ["mousedown", "mouseup", "click"]) {
      node.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, composed: true, view: window }));
    }
  }

  async function tick(rows) {
    const want = new Set(rows);
    for (const row of current.list.rows) {
      const box = boxOf(row);
      if (!box || !row.isConnected) continue;
      if (isTicked(row) !== want.has(row)) {
        press(box);
        await wait(20);
      }
    }
    await wait(350);
    const all = current.list.rows.filter((row) => row.isConnected && isTicked(row));
    return { picked: all.filter((row) => want.has(row)).length, extra: all.filter((row) => !want.has(row)).length };
  }

  function labelOf(node) {
    return node.getAttribute("aria-label") || node.getAttribute("title") || node.getAttribute("data-title")
      || node.getAttribute("data-tooltip") || (node.children.length ? "" : node.textContent);
  }

  function findAction(kind) {
    const near = current.list.scroller;
    let best = null;
    let bestDepth = -1;
    for (const node of document.querySelectorAll('button, [role="button"], [role="menuitem"], [title], [aria-label], [data-title], [data-tooltip]')) {
      if (current.list.root.contains(node) || !onScreen(node)) continue;
      if (tidy.actionOf(labelOf(node)) !== kind) continue;
      const target = node.closest('button, [role="button"], [role="menuitem"], a') || node;
      const shared = common(target, near);
      const score = shared ? depth(shared, null) : 0;
      if (score > bestDepth) { best = target; bestDepth = score; }
    }
    return best;
  }

  async function act(kind, expected = null) {
    const chosen = targets();
    const rows = chosen.map((item) => item.row);
    if (!rows.length || state.busy) return;
    if (expected !== null && expected !== rows.length) {
      cancelPending();
      say(`The list changed to ${tidy.plural(rows.length)}; nothing was deleted. Press Delete again.`, true);
      return;
    }
    state.busy = true;
    cancelPending();
    renderTools();
    say(`Selecting ${tidy.plural(rows.length)} in Zoho Mail…`);
    try {
      const { picked, extra } = await tick(rows);
      if (picked !== rows.length || extra) {
        say(`Zoho Mail selected ${picked} of ${rows.length}${extra ? ` and ${extra} other` : ""}; nothing else was done. Check the ticks and use Zoho's own toolbar.`, true);
        return;
      }
      if (kind === "select") { say(tidy.doneText("select", picked)); return; }
      const button = findAction(kind);
      if (!button) {
        say(`${tidy.plural(picked)} are selected, but Zoho Mail's ${tidy.verb(kind, "").trim()} button is not on screen. Use Zoho's toolbar to finish.`, true);
        return;
      }
      press(button);
      say(tidy.doneText(kind, picked));
    } finally {
      state.busy = false;
      renderTools();
      schedule(600);
    }
  }

  shadow.addEventListener("click", (event) => {
    const sort = event.target.closest?.("button[data-sort]");
    if (sort) {
      const by = sort.dataset.sort;
      if (state.by === by) state.descending = !state.descending;
      else { state.by = by; state.descending = false; }
      sync();
      return;
    }
    const pick = event.target.closest?.("button[data-sender]");
    if (pick) {
      const same = state.sender === pick.dataset.sender;
      state.sender = same ? "" : pick.dataset.sender;
      state.senderLabel = same ? "" : pick.dataset.label;
      state.panel = false;
      say("");
      sync();
      return;
    }
    const button = event.target.closest?.("button[data-act]");
    if (!button) return;
    const kind = button.dataset.act;
    if (kind === "group") { state.group = !state.group; state.panel = state.group; sync(); }
    else if (kind === "close-senders") { state.panel = false; renderTools(); }
    else if (kind === "all-senders") { state.sender = ""; state.senderLabel = ""; sync(); }
    else if (kind === "cancel") cancelPending();
    else if (kind === "confirm") { if (state.pending) act("delete", state.pending.count); }
    else if (kind === "delete") askDelete(targets().length);
    else act(kind);
  });

  for (const type of ["keydown", "keyup", "keypress"]) {
    shadow.addEventListener(type, (event) => {
      if (type === "keydown" && event.key === "Escape") {
        if (state.pending) cancelPending();
        else if (state.panel) { state.panel = false; renderTools(); }
        else if (event.target === find && find.value) { find.value = ""; state.query = ""; sync(); }
      }
      event.stopPropagation();
    });
  }

  let typing = 0;
  find.addEventListener("input", () => {
    clearTimeout(typing);
    typing = setTimeout(() => { state.query = find.value; say(""); sync(); }, 150);
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
    const background = backgroundOf(list.scroller);
    host.style.background = background;
    host.style.setProperty("--fm-bg", background);
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
    if (state.busy) { schedule(600); return; }
    clearFilter();
    let list = null;
    try { list = findList(); } catch { list = null; }
    if (!list) {
      host.remove();
      clearMarks();
      current = null;
      return;
    }
    current = {
      list,
      items: list.rows.map((row, index) => {
        const cols = columns(row);
        return { row, index, sender: tidy.senderName(cols.sender?.text), subject: tidy.subjectText(cols.subject?.text) };
      }),
    };
    if (host.parentNode !== list.scroller) list.scroller.prepend(host);
    paint(list);
    arrange();
    renderTools();
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

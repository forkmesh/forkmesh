(function () {
  "use strict";
  if (window.ForkMeshUsernames) return;

  const SEARCH_ENDPOINT = "/api/chat/direct-messages/users?query=";
  const NAME_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
  const QUERY_RE = /^[a-z0-9-]{1,63}$/;
  const FIELD_SELECTOR = "input[data-username-autocomplete]";
  const LIST_ID = "fm-username-list";
  const LIMIT = 8;
  const DEBOUNCE_MS = 150;

  const known = new Set();
  const searched = new Map();
  const ui = { input: null, items: [], index: 0, start: 0, end: 0, list: null, timer: 0, bound: false };

  function clean(value) {
    return String(value || "").trim().replace(/^@+/, "").toLowerCase();
  }

  function remember(names) {
    for (const value of names || []) {
      const name = clean(typeof value === "string" ? value : value?.name);
      if (NAME_RE.test(name)) known.add(name);
    }
  }

  function selfName() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      return clean(session?.nodeName);
    } catch (_) {
      return "";
    }
  }

  function sessionToken() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      return String(session?.sessionToken || "");
    } catch (_) {
      return "";
    }
  }

  function rank(names, query, exclude, limit = LIMIT) {
    const needle = clean(query);
    const skip = exclude instanceof Set ? exclude : new Set(exclude || []);
    const prefix = [];
    const inside = [];
    for (const name of new Set(names)) {
      if (skip.has(name)) continue;
      if (!needle || name.startsWith(needle)) prefix.push(name);
      else if (name.includes(needle)) inside.push(name);
    }
    prefix.sort((a, b) => a.length - b.length || a.localeCompare(b));
    inside.sort((a, b) => a.localeCompare(b));
    return prefix.concat(inside).slice(0, limit);
  }

  function search(query) {
    const needle = clean(query);
    if (!QUERY_RE.test(needle)) return Promise.resolve([]);
    if (searched.has(needle)) return searched.get(needle);
    const headers = { accept: "application/json" };
    const token = sessionToken();
    if (token) headers.authorization = "Bearer " + token;
    const pending = fetch(SEARCH_ENDPOINT + encodeURIComponent(needle), { headers })
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => {
        const names = (Array.isArray(data?.users) ? data.users : [])
          .map((user) => clean(user?.name))
          .filter((name) => NAME_RE.test(name));
        remember(names);
        return names;
      })
      .catch(() => {
        searched.delete(needle);
        return [];
      });
    searched.set(needle, pending);
    return pending;
  }

  function lookup(query, onUpdate, exclude) {
    const needle = clean(query);
    if (!needle) return [];
    const skip = new Set(exclude || []);
    const self = selfName();
    if (self) skip.add(self);
    const now = rank(known, needle, skip);
    if (QUERY_RE.test(needle) && !searched.has(needle) && typeof onUpdate === "function") {
      search(needle).then(() => onUpdate(rank(known, needle, skip)));
    }
    return now;
  }

  function fieldMode(input) {
    return input.dataset.usernameAutocomplete === "list" ? "list" : "single";
  }

  function splitNames(value) {
    return String(value || "").split(/[\s,]+/).map(clean).filter(Boolean);
  }

  function tokenAt(value, caret, mode) {
    const text = String(value || "");
    if (mode !== "list") return { start: 0, end: text.length, query: clean(text) };
    const at = Math.max(0, Math.min(Number(caret) || 0, text.length));
    const start = text.lastIndexOf(",", at - 1) + 1;
    const comma = text.indexOf(",", at);
    const end = comma < 0 ? text.length : comma;
    return { start, end, query: clean(text.slice(start, end)) };
  }

  function insertName(value, token, name, mode) {
    const text = String(value || "");
    if (mode !== "list") return { value: name, caret: name.length };
    const before = text.slice(0, token.start).replace(/\s+$/, "");
    const after = text.slice(token.end).replace(/^\s*,?\s*/, "");
    const head = (before ? before + " " : "") + name + ", ";
    return { value: head + after, caret: head.length };
  }

  function fieldExclude(input, token) {
    const skip = new Set(splitNames(input.dataset.usernameExclude));
    if (fieldMode(input) === "list") {
      const text = input.value;
      splitNames(text.slice(0, token.start) + "," + text.slice(token.end)).forEach((name) => skip.add(name));
    }
    return skip;
  }

  function injectStyles() {
    if (document.getElementById("fm-username-styles")) return;
    const style = document.createElement("style");
    style.id = "fm-username-styles";
    style.textContent = `
      .fm-username-list { --un-bg: var(--surface2, Canvas); --un-fg: var(--fg, CanvasText);
        --un-border: var(--border, #d0d7de); --un-muted: var(--muted, GrayText);
        --un-active: var(--accent-dim, rgba(9,105,218,.14));
        position: fixed; z-index: 2147483000; box-sizing: border-box; margin: 0;
        max-height: min(18rem, 45vh); overflow-y: auto; overscroll-behavior: contain; padding: 4px;
        border: 1px solid var(--un-border); border-radius: 8px; background: var(--un-bg); color: var(--un-fg);
        box-shadow: 0 12px 32px rgba(1,4,9,.35); font: inherit; font-size: 13px; }
      [data-dashboard-theme] .fm-username-list { --un-bg: rgb(var(--dashboard-popover-rgb));
        --un-fg: rgb(var(--dashboard-popover-foreground-rgb)); --un-border: rgb(var(--dashboard-border-rgb));
        --un-muted: rgb(var(--dashboard-muted-foreground-rgb)); --un-active: rgb(var(--dashboard-accent-rgb) / .12); }
      .fm-username-list[hidden] { display: none; }
      .fm-username-option { display: flex; align-items: center; gap: 8px; min-height: 32px; padding: 4px 8px;
        border-radius: 6px; cursor: pointer; line-height: 20px; -webkit-tap-highlight-color: transparent; }
      .fm-username-option[aria-selected="true"], .fm-username-option:hover { background: var(--un-active); }
      .fm-username-avatar { display: inline-flex; flex: none; align-items: center; justify-content: center;
        width: 22px; height: 22px; border-radius: 9999px; border: 1px solid var(--un-border);
        font-size: 10px; font-weight: 700; color: var(--un-muted); text-transform: uppercase; }
      .fm-username-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      .fm-username-name b { font-weight: 600; }
    `;
    document.head.append(style);
  }

  function listElement(input) {
    injectStyles();
    if (!ui.list) {
      const list = document.createElement("div");
      list.id = LIST_ID;
      list.className = "fm-username-list";
      list.setAttribute("role", "listbox");
      list.setAttribute("aria-label", "Matching usernames");
      list.hidden = true;
      const pick = (event) => {
        const option = event.target.closest?.("[data-username-pick]");
        if (event.cancelable) event.preventDefault();
        if (option && event.type === "click") choose(Number(option.dataset.usernamePick));
      };
      list.addEventListener("pointerdown", pick);
      list.addEventListener("mousedown", pick);
      list.addEventListener("click", pick);
      ui.list = list;
    }
    const host = input.closest("dialog") || document.body;
    if (ui.list.parentNode !== host) host.append(ui.list);
    return ui.list;
  }

  function position() {
    const list = ui.list;
    const input = ui.input;
    if (!list || list.hidden || !input?.isConnected) return;
    const box = input.getBoundingClientRect();
    const view = window.visualViewport;
    const viewTop = view ? view.offsetTop : 0;
    const viewLeft = view ? view.offsetLeft : 0;
    const viewBottom = viewTop + (view ? view.height : window.innerHeight);
    const viewRight = viewLeft + (view ? view.width : window.innerWidth);
    list.style.width = Math.round(Math.min(Math.max(box.width, 224), viewRight - viewLeft - 16)) + "px";
    const height = list.offsetHeight;
    const width = list.offsetWidth;
    let top = box.bottom + 4;
    if (top + height > viewBottom - 8 && box.top - height - 4 >= viewTop + 8) top = box.top - height - 4;
    const left = Math.max(viewLeft + 8, Math.min(box.left, viewRight - 8 - width));
    list.style.top = Math.round(top) + "px";
    list.style.left = Math.round(left) + "px";
    const placed = list.getBoundingClientRect();
    if (Math.abs(placed.top - top) > 1 || Math.abs(placed.left - left) > 1) {
      list.style.top = Math.round(2 * top - placed.top) + "px";
      list.style.left = Math.round(2 * left - placed.left) + "px";
    }
  }

  function close() {
    if (ui.input) {
      ui.input.setAttribute("aria-expanded", "false");
      ui.input.removeAttribute("aria-activedescendant");
    }
    if (ui.list) {
      ui.list.hidden = true;
      ui.list.replaceChildren();
    }
    ui.input = null;
    ui.items = [];
  }

  function render() {
    const list = listElement(ui.input);
    const query = tokenAt(ui.input.value, ui.input.selectionEnd, fieldMode(ui.input)).query;
    list.replaceChildren(...ui.items.map((name, index) => {
      const option = document.createElement("div");
      option.id = LIST_ID + "-" + index;
      option.className = "fm-username-option";
      option.setAttribute("role", "option");
      option.setAttribute("aria-selected", index === ui.index ? "true" : "false");
      option.dataset.usernamePick = String(index);
      const avatar = document.createElement("span");
      avatar.className = "fm-username-avatar";
      avatar.setAttribute("aria-hidden", "true");
      avatar.textContent = name[0] || "?";
      const label = document.createElement("span");
      label.className = "fm-username-name";
      const at = query ? name.indexOf(query) : -1;
      if (at >= 0) {
        const strong = document.createElement("b");
        strong.textContent = name.slice(at, at + query.length);
        label.append("@" + name.slice(0, at), strong, name.slice(at + query.length));
      } else {
        label.textContent = "@" + name;
      }
      option.append(avatar, label);
      option.addEventListener("mouseenter", () => highlight(index, false));
      return option;
    }));
    list.hidden = false;
    ui.input.setAttribute("aria-expanded", "true");
    ui.input.setAttribute("aria-activedescendant", LIST_ID + "-" + ui.index);
    position();
    list.querySelector("#" + LIST_ID + "-" + ui.index)?.scrollIntoView?.({ block: "nearest" });
  }

  function highlight(index, scroll) {
    const count = ui.items.length;
    if (!count || !ui.list) return;
    ui.index = ((index % count) + count) % count;
    ui.list.querySelectorAll(".fm-username-option").forEach((option, i) => {
      option.setAttribute("aria-selected", i === ui.index ? "true" : "false");
      if (i === ui.index && scroll) option.scrollIntoView?.({ block: "nearest" });
    });
    ui.input?.setAttribute("aria-activedescendant", LIST_ID + "-" + ui.index);
  }

  function show(input, items, token) {
    if (!items.length || (items.length === 1 && items[0] === token.query)) {
      if (ui.input === input) close();
      return;
    }
    const previous = ui.input === input ? ui.items[ui.index] : null;
    ui.input = input;
    ui.items = items;
    ui.start = token.start;
    ui.end = token.end;
    ui.index = Math.max(0, previous ? items.indexOf(previous) : 0);
    render();
  }

  function refresh(input) {
    if (input.disabled || input.readOnly) return;
    const mode = fieldMode(input);
    const token = tokenAt(input.value, input.selectionEnd, mode);
    if (!token.query) {
      if (ui.input === input) close();
      return;
    }
    const items = lookup(token.query, (later) => {
      if (document.activeElement !== input) return;
      const current = tokenAt(input.value, input.selectionEnd, mode);
      if (current.query === token.query) show(input, later, current);
    }, fieldExclude(input, token));
    show(input, items, token);
  }

  function choose(index) {
    const input = ui.input;
    const name = ui.items[index];
    if (!input || !name) return;
    const mode = fieldMode(input);
    const edit = insertName(input.value, { start: ui.start, end: ui.end }, name, mode);
    close();
    input.value = edit.value;
    input.focus();
    try { input.setSelectionRange(edit.caret, edit.caret); } catch (_) {}
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.dispatchEvent(new CustomEvent("forkmesh:username-picked", { bubbles: true, detail: { name } }));
  }

  function fieldFrom(target) {
    return target?.matches?.(FIELD_SELECTOR) ? target : null;
  }

  function prepare(input) {
    if (input.dataset.usernameReady) return;
    input.dataset.usernameReady = "1";
    input.setAttribute("role", "combobox");
    input.setAttribute("aria-autocomplete", "list");
    input.setAttribute("aria-expanded", "false");
    input.setAttribute("aria-controls", LIST_ID);
    input.setAttribute("autocomplete", "off");
    input.setAttribute("autocapitalize", "none");
    input.setAttribute("spellcheck", "false");
  }

  function bind() {
    if (ui.bound) return;
    ui.bound = true;
    document.addEventListener("focusin", (event) => {
      const input = fieldFrom(event.target);
      if (input) prepare(input);
    });
    document.addEventListener("input", (event) => {
      const input = fieldFrom(event.target);
      if (!input) return;
      prepare(input);
      if (event.isTrusted || ui.input === input) refresh(input);
    });
    document.addEventListener("keydown", (event) => {
      const input = fieldFrom(event.target);
      if (!input || event.isComposing || event.altKey || event.ctrlKey || event.metaKey) return;
      const open = ui.input === input && ui.items.length > 0;
      if (!open) {
        if (event.key === "ArrowDown") {
          refresh(input);
          if (ui.input === input) event.preventDefault();
        }
        return;
      }
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        highlight(ui.index + (event.key === "ArrowDown" ? 1 : -1), true);
      } else if (event.key === "Enter" || (event.key === "Tab" && !event.shiftKey)) {
        event.preventDefault();
        event.stopPropagation();
        choose(ui.index);
      } else if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        close();
      }
    }, true);
    document.addEventListener("focusout", (event) => {
      if (ui.input && event.target === ui.input) close();
    });
    const reposition = () => { if (ui.input) position(); };
    window.addEventListener("resize", reposition);
    window.addEventListener("scroll", reposition, true);
    window.visualViewport?.addEventListener("resize", reposition);
    window.visualViewport?.addEventListener("scroll", reposition);
  }

  window.ForkMeshUsernames = {
    clean,
    remember,
    rank,
    search,
    lookup,
    tokenAt,
    insertName,
    close,
  };
  bind();
})();

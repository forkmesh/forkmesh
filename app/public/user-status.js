// Away flag + custom status (emoji, short text, auto-clear time) for the web.
//
// One shared helper for the chat page, the dashboard and the site header:
// it keeps the known status of every member (from the account directory and
// from live relay frames), paints the emoji badge beside a name, and owns the
// "Set a status" editor that POSTs /api/accounts/user-status. The desktop and
// phone apps speak the same contract (app/src/account_status.py).
(function () {
  "use strict";
  if (window.ForkMeshUserStatus) return;

  const ENDPOINT = "/api/accounts/user-status";
  const TEXT_MAX = 100;
  // A relay frame is fresher than the edge-cached directory (120s per colo),
  // so a directory refresh never overwrites a frame seen this recently.
  const FRAME_WINS_MS = 3 * 60 * 1000;
  const QUICK_EMOJI = ["📅", "🚌", "🤒", "🌴", "🏡", "🎧", "🍔", "💻", "🎉", "🔕"];
  const CLEAR_AFTER = [
    ["never", "Don't clear"],
    ["30m", "30 minutes"],
    ["1h", "1 hour"],
    ["4h", "4 hours"],
    ["today", "Today"],
    ["week", "This week"],
    ["custom", "Choose date and time…"],
  ];
  const PRESETS = [
    ["📅", "In a meeting", "1h"],
    ["🚌", "Commuting", "30m"],
    ["🤒", "Out sick", "today"],
    ["🌴", "Vacationing", "never"],
    ["🏡", "Working remotely", "today"],
  ];
  const ERRORS = {
    bad_status_emoji: "Pick a single emoji.",
    status_text_too_long: "Keep the status under 100 characters.",
    bad_status_expiry: "That clear time isn't valid.",
    status_expiry_in_past: "The clear time has already passed.",
    status_expiry_too_far: "Pick a clear time within a year.",
    unauthorized: "Sign in to set a status.",
  };
  const EMPTY = Object.freeze({ away: false, emoji: "", text: "", expiresAt: 0 });

  const known = new Map(); // lowercased name -> { status, frameAt }
  const listeners = new Set();
  let expiryTimer = 0;

  function normalize(raw) {
    if (!raw || typeof raw !== "object") return { ...EMPTY };
    const expiresAt = Math.max(0, Number(raw.expiresAt) || 0);
    return {
      away: Boolean(raw.away),
      emoji: String(raw.emoji || "").trim().slice(0, 32),
      text: String(raw.text || "").replace(/\s+/g, " ").trim().slice(0, TEXT_MAX),
      expiresAt,
    };
  }

  // The status as it stands now: an expired custom status reads as none.
  function current(raw, now = Date.now()) {
    const status = normalize(raw);
    if (status.expiresAt && status.expiresAt <= now) {
      return { ...status, emoji: "", text: "", expiresAt: 0 };
    }
    return status;
  }

  function hasCustom(status) {
    return Boolean(status && (status.emoji || status.text));
  }

  function notify() {
    scheduleExpiry();
    for (const fn of listeners) {
      try { fn(); } catch (_) {}
    }
  }

  function scheduleExpiry() {
    clearTimeout(expiryTimer);
    const now = Date.now();
    let next = 0;
    for (const { status } of known.values()) {
      if (status.expiresAt > now && (!next || status.expiresAt < next)) {
        next = status.expiresAt;
      }
    }
    if (!next) return;
    // Browsers clamp setTimeout at ~24.8 days; re-arm past that.
    expiryTimer = setTimeout(notify, Math.min(next - now + 250, 2 ** 31 - 1));
  }

  // source: "directory" (account directory / profile lookup) or "frame"
  // (a live relay frame, or this browser's own save).
  function remember(name, raw, source = "directory") {
    const key = String(name || "").trim().toLowerCase();
    if (!key || !raw || typeof raw !== "object") return;
    const prev = known.get(key);
    if (source !== "frame" && prev && Date.now() - prev.frameAt < FRAME_WINS_MS) {
      return;
    }
    const status = normalize(raw);
    const same = prev && JSON.stringify(prev.status) === JSON.stringify(status);
    known.set(key, {
      status,
      frameAt: source === "frame" ? Date.now() : prev ? prev.frameAt : 0,
    });
    if (!same) notify();
  }

  function forName(name) {
    const entry = known.get(String(name || "").trim().toLowerCase());
    return current(entry && entry.status);
  }

  function formatUntil(expiresAt) {
    if (!expiresAt) return "";
    const when = new Date(expiresAt);
    const now = new Date();
    const time = when.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    if (when.toDateString() === now.toDateString()) return "until " + time;
    const tomorrow = new Date(now);
    tomorrow.setDate(now.getDate() + 1);
    if (when.toDateString() === tomorrow.toDateString()) {
      return "until tomorrow " + time;
    }
    return "until " + when.toLocaleDateString([], {
      weekday: "short", month: "short", day: "numeric",
    }) + " " + time;
  }

  // "🌴 Vacationing · until Mon, Sep 21 9:00 AM" / "Away".
  function describe(raw) {
    const status = current(raw);
    const parts = [];
    if (hasCustom(status)) {
      parts.push([status.emoji, status.text].filter(Boolean).join(" "));
      if (status.expiresAt) parts.push(formatUntil(status.expiresAt));
    }
    if (status.away) parts.push("Away");
    return parts.join(" · ");
  }

  // The emoji that sits beside a member's name, or null when they wear none.
  function badge(raw, options = {}) {
    const status = current(raw);
    if (!hasCustom(status)) return null;
    const el = document.createElement("span");
    el.className = "fm-user-status-badge" + (options.className ? " " + options.className : "");
    el.textContent = status.emoji || "💬";
    const label = describe({ ...status, away: false });
    el.title = label;
    el.setAttribute("aria-label", "Status: " + label);
    el.setAttribute("role", "img");
    return el;
  }

  function expiresAtFor(choice, custom, now = new Date()) {
    const base = now.getTime();
    switch (choice) {
      case "30m": return base + 30 * 60 * 1000;
      case "1h": return base + 60 * 60 * 1000;
      case "4h": return base + 4 * 60 * 60 * 1000;
      case "today": {
        const end = new Date(now);
        end.setHours(23, 59, 59, 0);
        return end.getTime();
      }
      case "week": {
        // The end of this local week: Sunday night.
        const end = new Date(now);
        end.setDate(now.getDate() + ((7 - now.getDay()) % 7));
        end.setHours(23, 59, 59, 0);
        return end.getTime();
      }
      case "custom": {
        const at = custom ? new Date(custom).getTime() : NaN;
        return Number.isFinite(at) ? at : -1;
      }
      default: return 0;
    }
  }

  // ---- own status -----------------------------------------------------------

  function session() {
    try {
      const value = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!value || !value.nodeName || value.kind === "node") return null;
      return value;
    } catch (_) {
      return null;
    }
  }

  function mine() {
    const own = session();
    return own ? forName(own.nodeName) : { ...EMPTY };
  }

  // What a hello/presence frame should carry, or null before this browser
  // has read its own status — announcing "nothing" then would, for a few
  // minutes, mask the real status peers already hold from the directory.
  function ownFrameStatus() {
    const own = session();
    if (!own || !known.has(String(own.nodeName).trim().toLowerCase())) return null;
    return mine();
  }

  async function post(body) {
    const own = session();
    if (!own) throw new Error(ERRORS.unauthorized);
    const token = String(own.sessionToken || "").trim();
    const headers = { accept: "application/json", "content-type": "application/json" };
    if (token && token !== "cookie") headers.authorization = "Bearer " + token;
    let response;
    try {
      response = await fetch(ENDPOINT, {
        method: "POST",
        credentials: "same-origin",
        cache: "no-store",
        headers,
        body: JSON.stringify(body || {}),
      });
    } catch (_) {
      throw new Error("Couldn't reach ForkMesh. Try again.");
    }
    const data = await response.json().catch(() => ({}));
    if (!response.ok || !data || !data.userStatus) {
      throw new Error(ERRORS[data && data.error] || "Couldn't save your status.");
    }
    remember(own.nodeName, data.userStatus, "frame");
    try {
      sessionStorage.setItem(MINE_CACHE_KEY, JSON.stringify({
        at: Date.now(), name: own.nodeName, status: data.userStatus,
      }));
    } catch (_) {}
    if (body && Object.keys(body).length) {
      // Lets an open chat socket announce the change right away instead of
      // waiting for its next presence beat.
      window.dispatchEvent(new CustomEvent("forkmesh:user-status-self", {
        detail: current(data.userStatus),
      }));
    }
    return current(data.userStatus);
  }

  // Own status on page load. Moving between dashboard pages reuses a read
  // from the last two minutes instead of spending a Worker request per view.
  const MINE_CACHE_KEY = "forkmesh.userStatus.mine";
  const MINE_CACHE_MS = 2 * 60 * 1000;
  let loadingMine = null;
  function loadMine() {
    const own = session();
    if (!own) return Promise.resolve({ ...EMPTY });
    try {
      const cached = JSON.parse(sessionStorage.getItem(MINE_CACHE_KEY) || "null");
      if (cached && cached.name === own.nodeName &&
          Date.now() - Number(cached.at || 0) < MINE_CACHE_MS) {
        remember(own.nodeName, cached.status, "frame");
        return Promise.resolve(mine());
      }
    } catch (_) {}
    if (!loadingMine) {
      loadingMine = post({}).catch(() => mine()).finally(() => {
        loadingMine = null;
      });
    }
    return loadingMine;
  }

  const setAway = (away) => post({ away: Boolean(away) });
  const clearStatus = () => post({ clear: true });

  // ---- editor ---------------------------------------------------------------

  function injectStyles() {
    if (document.getElementById("fm-user-status-styles")) return;
    const style = document.createElement("style");
    style.id = "fm-user-status-styles";
    style.textContent = `
      .fm-user-status-badge { display: inline-block; margin-left: 4px; font-size: 0.95em;
        line-height: 1; vertical-align: -0.05em; cursor: default; flex: none; }
      .fm-user-status-dialog { border: 1px solid var(--border, #d0d7de); border-radius: 12px;
        padding: 0; width: min(440px, calc(100vw - 24px)); background: var(--surface, Canvas);
        color: var(--fg, CanvasText); font: inherit; box-shadow: 0 20px 50px rgba(0,0,0,.35); }
      .fm-user-status-dialog::backdrop { background: rgba(0,0,0,.45); }
      .fm-user-status-dialog form { display: grid; gap: 12px; padding: 18px; }
      .fm-user-status-dialog h2 { margin: 0; font-size: 16px; }
      .fm-user-status-row { display: flex; gap: 8px; align-items: center; }
      .fm-user-status-dialog input, .fm-user-status-dialog select { font: inherit; color: inherit;
        background: var(--surface2, transparent); border: 1px solid var(--border, #d0d7de);
        border-radius: 8px; padding: 7px 9px; min-width: 0; }
      .fm-user-status-emoji-input { width: 3.2em; text-align: center; font-size: 18px; }
      .fm-user-status-text-input { flex: 1; }
      .fm-user-status-quick { display: flex; flex-wrap: wrap; gap: 4px; }
      .fm-user-status-quick button, .fm-user-status-preset { font: inherit; color: inherit;
        background: transparent; border: 1px solid transparent; border-radius: 8px; cursor: pointer; }
      .fm-user-status-quick button { font-size: 18px; width: 34px; height: 34px; }
      .fm-user-status-quick button:hover, .fm-user-status-preset:hover,
      .fm-user-status-quick button:focus-visible, .fm-user-status-preset:focus-visible {
        background: var(--surface2, rgba(127,127,127,.15)); border-color: var(--border, #d0d7de); }
      .fm-user-status-presets { display: grid; gap: 2px; }
      .fm-user-status-preset { display: flex; gap: 8px; align-items: center; padding: 6px 8px;
        text-align: left; }
      .fm-user-status-preset small { margin-left: auto; color: var(--muted, GrayText); }
      .fm-user-status-label { font-size: 12px; color: var(--muted, GrayText); }
      .fm-user-status-away { display: flex; gap: 8px; align-items: center; }
      .fm-user-status-error { color: #d1242f; font-size: 13px; min-height: 1em; margin: 0; }
      .fm-user-status-actions { display: flex; gap: 8px; justify-content: flex-end; }
      .fm-user-status-actions button { font: inherit; border-radius: 8px; padding: 7px 14px;
        border: 1px solid var(--border, #d0d7de); background: transparent; color: inherit; cursor: pointer; }
      .fm-user-status-actions .is-primary { background: var(--accent, #1f883d); color: #fff;
        border-color: transparent; }
      .fm-user-status-actions .is-clear { margin-right: auto; }
    `;
    document.head.append(style);
  }

  function el(tag, props = {}, children = []) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(props)) {
      if (key === "className") node.className = value;
      else if (key === "text") node.textContent = value;
      else node.setAttribute(key, value);
    }
    node.append(...children);
    return node;
  }

  function localInputValue(ms) {
    const date = new Date(ms);
    const pad = (n) => String(n).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}` +
      `T${pad(date.getHours())}:${pad(date.getMinutes())}`;
  }

  async function openEditor() {
    if (!session()) {
      location.href = "/login?next=" + encodeURIComponent(location.pathname);
      return;
    }
    injectStyles();
    const existing = mine();
    const emojiInput = el("input", {
      className: "fm-user-status-emoji-input", type: "text", maxlength: "16",
      "aria-label": "Status emoji", placeholder: "🙂",
    });
    const textInput = el("input", {
      className: "fm-user-status-text-input", type: "text",
      maxlength: String(TEXT_MAX), placeholder: "What's your status?",
      "aria-label": "Status text",
    });
    const clearSelect = el("select", { "aria-label": "Clear after" },
      CLEAR_AFTER.map(([value, label]) => el("option", { value, text: label })));
    const customInput = el("input", {
      type: "datetime-local", "aria-label": "Clear at",
    });
    customInput.hidden = true;
    const awayInput = el("input", { type: "checkbox", id: "fm-user-status-away" });
    const error = el("p", { className: "fm-user-status-error", role: "alert" });

    emojiInput.value = existing.emoji;
    textInput.value = existing.text;
    awayInput.checked = existing.away;
    if (existing.expiresAt) {
      clearSelect.value = "custom";
      customInput.value = localInputValue(existing.expiresAt);
      customInput.hidden = false;
    }
    clearSelect.addEventListener("change", () => {
      customInput.hidden = clearSelect.value !== "custom";
      if (!customInput.hidden && !customInput.value) {
        customInput.value = localInputValue(Date.now() + 60 * 60 * 1000);
      }
    });

    const quick = el("div", { className: "fm-user-status-quick" },
      QUICK_EMOJI.map((emoji) => {
        const button = el("button", { type: "button", title: emoji, text: emoji });
        button.addEventListener("click", () => {
          emojiInput.value = emoji;
          textInput.focus();
        });
        return button;
      }));
    const presets = el("div", { className: "fm-user-status-presets" },
      PRESETS.map(([emoji, text, clear]) => {
        const label = CLEAR_AFTER.find(([value]) => value === clear)[1];
        const button = el("button", { type: "button", className: "fm-user-status-preset" }, [
          el("span", { text: emoji }), el("span", { text }), el("small", { text: label }),
        ]);
        button.addEventListener("click", () => {
          emojiInput.value = emoji;
          textInput.value = text;
          clearSelect.value = clear;
          customInput.hidden = true;
        });
        return button;
      }));

    const cancel = el("button", { type: "button", text: "Cancel" });
    const clear = el("button", { type: "button", className: "is-clear", text: "Clear status" });
    clear.hidden = !hasCustom(existing);
    const save = el("button", { type: "submit", className: "is-primary", text: "Save" });
    const form = el("form", { method: "dialog" }, [
      el("h2", { text: "Set a status" }),
      el("div", { className: "fm-user-status-row" }, [emojiInput, textInput]),
      quick,
      el("span", { className: "fm-user-status-label", text: "Suggestions" }),
      presets,
      el("div", { className: "fm-user-status-row" }, [
        el("span", { className: "fm-user-status-label", text: "Clear after" }),
        clearSelect, customInput,
      ]),
      el("label", { className: "fm-user-status-away", for: "fm-user-status-away" }, [
        awayInput, el("span", { text: "Set yourself away" }),
      ]),
      error,
      el("div", { className: "fm-user-status-actions" }, [clear, cancel, save]),
    ]);
    const dialog = el("dialog", {
      className: "fm-user-status-dialog", "aria-label": "Set a status",
    }, [form]);
    document.body.append(dialog);
    dialog.addEventListener("close", () => dialog.remove());
    cancel.addEventListener("click", () => dialog.close());

    const run = async (body) => {
      error.textContent = "";
      save.disabled = clear.disabled = true;
      try {
        await post(body);
        dialog.close();
      } catch (failure) {
        error.textContent = failure.message;
      } finally {
        save.disabled = clear.disabled = false;
      }
    };
    clear.addEventListener("click", () => run({ clear: true, away: awayInput.checked }));
    form.addEventListener("submit", (event) => {
      event.preventDefault();
      const emoji = emojiInput.value.trim();
      const text = textInput.value.replace(/\s+/g, " ").trim();
      let expiresAt = 0;
      if (emoji || text) {
        expiresAt = expiresAtFor(clearSelect.value, customInput.value);
        if (expiresAt < 0) {
          error.textContent = "Choose when the status should clear.";
          return;
        }
      }
      run(emoji || text
        ? { away: awayInput.checked, emoji, text, expiresAt }
        : { away: awayInput.checked, clear: true });
    });
    dialog.showModal();
    (existing.emoji || existing.text ? textInput : emojiInput).focus();
  }

  function subscribe(fn) {
    listeners.add(fn);
    return () => listeners.delete(fn);
  }

  // The badge style is needed as soon as a name is painted, not only once
  // the editor opens.
  injectStyles();
  window.ForkMeshUserStatus = Object.freeze({
    normalize, current, hasCustom, remember, forName, describe, badge,
    expiresAtFor, mine, ownFrameStatus, loadMine, setAway, clearStatus, openEditor, subscribe,
  });
  // The header's account menu may have rendered before this script ran.
  window.dispatchEvent(new CustomEvent("forkmesh:user-status-ready"));
})();
